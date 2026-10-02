/* sam_sing.c -- Microsoft Sam, Mike and Mary singing notes of phonemes (see sam_sing.h).
 *
 * The engine's front end is a set of static functions in sam_front.c: the item list, the
 * position flags, the unit selection by decision tree, the natural durations. Rather than copy
 * them, this file includes sam_front.c whole and adds one more entry point beside its own
 * sam_tts_sing, so the build compiles sam_front.c here and not on its own.
 *
 * The path is sam_tts_sing's with three differences. The phonemes arrive ready-made instead of
 * coming out of the dictionary. The pitch is not one value per note but a contour, sampled at
 * the sounds' own times. And the timing is kept honest: the synthesiser lays down whole pitch
 * periods, so a sound comes out a little longer or shorter than asked; over a long phrase that
 * would add up, so every sound is given the time left until its scheduled end as measured by
 * the samples actually produced, and the error never carries past the next sound.
 */
#include "sam_front.c"

#include "sam_sing.h"

/* the longest a note may hold its notes' phones; the engine's word holds 128 */
#define SVS_MAX_NOTE_PH (MAX_WORD_PH - 1)
/* a sound is never asked for less than this, unless it was scheduled shorter */
#define SVS_MIN_SOUND 0.010
/* the contour is sampled every 5 ms before choosing the 20 knots */
#define SVS_F0_STEP 0.005
#define SVS_F0_GRID 512

sam_tts *svs_sam_open(const sam_voice *voice)
{
    sam_tts *t;
    if (!voice) return NULL;
    t = calloc(1, sizeof *t);
    if (!t) return NULL;
    /* borrowed: svs_sam_close never frees it, and synthesis never writes to it */
    t->voice = (sam_voice *)voice;
    t->base_pitch = 100.0f;
    t->rand_state = 1;
    t->st_rng = 1.0f;
    t->st_rate = 1.0f;
    return t;
}

void svs_sam_close(sam_tts *t)
{
    if (!t) return;
    sam_synth_free(t->synth);
    free(t->pend);
    free(t);
}

static int sound_of(int sapi)
{
    int ph;
    if (sapi < SVS_SAPI_AA || sapi > SVS_SAPI_ZH) return -1;
    ph = sapi_to_internal(sapi);
    return ph >= 0 && ph < 0x2B && ph != SIL ? ph : -1;
}

int svs_sam_knows_phone(int sapi) { return sound_of(sapi) >= 0; }

int svs_sam_phone_is_vowel(int sapi)
{
    int ph = sound_of(sapi);
    return ph >= 0 && is_vowel(ph);
}

/* Choose up to SAM_MAX_KNOTS points on the contour over `span` seconds from `t0` so that the
 * straight lines between them (which is how the synthesiser reads them) follow it closely: the
 * two ends, then again and again the point furthest from the lines so far, until 20 or until
 * nothing is off by more than a fraction of a cent. A glide or a bend gets its knots where it
 * moves; a held note needs two. Times come back in samples from t0. */
static int fit_knots(double (*f0_at)(void *, double), void *fu, double t0, double span, double sr,
                     float *kt, float *kf)
{
    double v[SVS_F0_GRID];
    int sel[SVS_F0_GRID], n, i, count = 2;
    n = (int)ceil(span / SVS_F0_STEP) + 1;
    if (n < 2) n = 2;
    if (n > SVS_F0_GRID) n = SVS_F0_GRID;
    for (i = 0; i < n; i++) {
        double f = f0_at(fu, t0 + span * i / (n - 1));
        if (!(f > 0.0) || f != f) f = 100.0;
        if (f < 20.0) f = 20.0;
        if (f > 4000.0) f = 4000.0;
        v[i] = f;
        sel[i] = i == 0 || i == n - 1;
    }
    while (count < SAM_MAX_KNOTS) {
        int a = 0, b, best = -1;
        double worst = 1e-4; /* relative: about 0.17 cents */
        for (b = 1; b < n; b++) {
            int g;
            if (!sel[b]) continue;
            for (g = a + 1; g < b; g++) {
                double line = v[a] + (v[b] - v[a]) * (g - a) / (double)(b - a);
                double e = fabs(v[g] - line) / v[g];
                if (e > worst) {
                    worst = e;
                    best = g;
                }
            }
            a = b;
        }
        if (best < 0) break;
        sel[best] = 1;
        count++;
    }
    count = 0;
    for (i = 0; i < n; i++)
        if (sel[i]) {
            kt[count] = (float)(span * sr * i / (n - 1));
            kf[count] = (float)v[i];
            count++;
        }
    return count;
}

/* the vocoder's moving vibrato depth (see cmake/engines/microsoft.cmake) */
extern __thread double (*svs_vib_depth_at)(void *, double);
extern __thread void *svs_vib_user;
extern __thread double svs_vib_t0;

int svs_sam_sing_phones(sam_tts *t, const svs_sam_note *notes, int nnotes,
                        double (*f0_at)(void *fu, double sec), void *fu,
                        const svs_sam_sing_opts *opts, sam_pcm_cb cb, void *user)
{
    wordlist l = {0};
    itemlist il = {0};
    sam_params p;
    double *ends = NULL, cs, at, sr;
    int rc = 0, k, i;
    if (!t || !t->voice || !notes || nnotes <= 0 || !f0_at || !cb) return -1;
    cs = opts && opts->consonant_scale > 0.0 ? opts->consonant_scale : 1.0;

    /* A fresh synthesiser per phrase: the filter memory, the noise position and the vibrato
     * phase all start from zero every time, so a phrase sounds the same however often it is
     * sung and whatever was sung before it. Smoothing on, as the engine's own singing mode has
     * it: long notes otherwise step audibly from frame to frame. */
    sam_params_default(&p);
    p.smooth = 1;
    p.whisper = opts ? opts->whisper : 0;
    if (opts && opts->vibrato_hz > 0.0 && (opts->vibrato_cents > 0.0 || opts->vibrato_at)) {
        /* nonzero switches the engine's vibrato on; a moving depth then replaces it */
        p.sing_vibrato = opts->vibrato_cents > 0.0 ? (float)opts->vibrato_cents : 1e-3f;
        p.sing_vibrato_rate = (float)opts->vibrato_hz;
    }
    svs_vib_depth_at = opts ? opts->vibrato_at : NULL;
    svs_vib_user = opts ? opts->vibrato_user : NULL;
    sam_synth_free(t->synth);
    t->synth = sam_synth_new(t->voice, &p);
    if (!t->synth) return -1;
    t->monotone = 0;
    sr = (double)sam_voice_rate(t->voice);

    /* One word per note. VocalWriter's notes do not say where words begin, and most notes are a
     * syllable, so each is its own word with its main vowel stressed: the engine picks its units
     * for word starts and ends, and stressed units are the fuller ones. A rest is a silence
     * word, as in sam_tts_sing. */
    for (k = 0; k < nnotes && rc == 0; k++) {
        const svs_sam_note *nt = &notes[k];
        word *w = wl_insert(&l, l.n);
        int j, sounds = 0;
        if (!w) {
            rc = -1;
            break;
        }
        w->rate = 1.0f;
        for (j = 0; j < nt->nphones && w->nph < SVS_MAX_NOTE_PH; j++) {
            int s = nt->phones[j], ph;
            if (s == SVS_SAPI_ST1 || s == SVS_SAPI_ST2) {
                /* a stress mark belongs after a vowel; anywhere else the engine ignores it */
                if (w->nph > 0 && is_vowel(w->ph[w->nph - 1])) {
                    w->ph[w->nph] = s == SVS_SAPI_ST1 ? ST1 : ST2;
                    w->note[w->nph] = (short)(k + 1);
                    w->nph++;
                }
                continue;
            }
            ph = sound_of(s);
            if (ph < 0) continue; /* syllable marks, silences, anything unknown */
            w->ph[w->nph] = ph;
            w->note[w->nph] = (short)(k + 1);
            w->nph++;
            sounds++;
        }
        if (!sounds) {
            w->nph = 1;
            w->ph[0] = SIL;
            w->note[0] = (short)(k + 1);
            w->silence_ms = (int)(nt->sec * 1000.0);
        }
    }
    if (rc == 0 && build_items(&l, &il) != 0) rc = -1;
    if (rc == 0) {
        position_flags(&il);
        select_units(t, &il);
        durations(&il, 1.0);
    }

    /* Fit each note to its length, as sam_tts_sing does: the consonants at their natural
     * length (times the scale), the vowels taking the rest in proportion to their own, shared
     * with the r, l, m, n or ng after them. If the consonants alone would fill the note, they
     * are squeezed to leave the vowel at least 40 ms (or half the note), so the note still
     * ends on time; a note with no vowel at all is its consonants, stretched or squeezed. */
    for (k = 0; k < nnotes && rc == 0; k++) {
        double sec = notes[k].sec > 0.0 ? notes[k].sec : 0.0;
        double cons = 0.0, vow = 0.0, son = 0.0, room, vroom, sroom, fill_min, squeeze = 1.0;
        int nv = 0, ns = 0, seen_vowel = 0;
        for (i = 0; i < il.n; i++) {
            item *x = &il.it[i];
            if (x->note != k + 1) continue;
            x->coda = 0;
            if (is_vowel(x->type)) {
                vow += x->dur;
                nv++;
                seen_vowel = 1;
            } else if (x->type != SIL) {
                if (seen_vowel && is_sonorant(x->type)) {
                    x->coda = 1;
                    son += x->dur;
                    ns++;
                } else {
                    cons += x->dur * cs;
                }
            }
        }
        if (nv + ns == 0) {
            squeeze = cons > 0.0 ? sec / cons : 0.0;
            room = 0.0;
        } else {
            fill_min = 0.04 * (nv + ns);
            if (fill_min > 0.5 * sec) fill_min = 0.5 * sec;
            if (cons > sec - fill_min) {
                squeeze = cons > 0.0 ? (sec - fill_min) / cons : 0.0;
                cons = sec - fill_min;
            }
            room = sec - cons;
        }
        vroom = ns ? room * 0.55 : room;
        sroom = room - vroom;
        for (i = 0; i < il.n; i++) {
            item *x = &il.it[i];
            if (x->note != k + 1) continue;
            if (x->type == SIL) x->dur = (float)sec;
            else if (is_vowel(x->type)) x->dur = (float)(vow > 0.0 ? vroom * x->dur / vow : vroom / nv);
            else if (x->coda) x->dur = (float)(son > 0.0 ? sroom * x->dur / son : sroom / ns);
            else x->dur = (float)(x->dur * cs * squeeze);
        }
    }

    /* Where every sound is due to end, in seconds from the start of the phrase. The last sound
     * of a note ends exactly where the next note starts, whatever the float rounding of the
     * lengths above. Sounds that belong to no note -- the engine's own opening and closing
     * silences -- end where they start and are not rendered: the phrase starts with its first
     * note and ends with its last. */
    if (rc == 0) {
        ends = calloc((size_t)(il.n ? il.n : 1), sizeof *ends);
        if (!ends) rc = -1;
    }
    if (rc == 0) {
        /* note_start is the sum of the first `counted` notes' lengths */
        double note_start = 0.0;
        int cur = 0, last = -1, counted = 0;
        at = 0.0;
        for (i = 0; i < il.n; i++) {
            item *x = &il.it[i];
            if (!x->note) {
                ends[i] = at;
                continue;
            }
            if (x->note != cur) {
                /* a new note (x->note counts from 1): the previous one closes exactly here */
                while (counted < x->note - 1) {
                    note_start += notes[counted].sec > 0.0 ? notes[counted].sec : 0.0;
                    counted++;
                }
                if (last >= 0) ends[last] = note_start;
                at = note_start;
                cur = x->note;
            }
            at += x->dur;
            ends[i] = at;
            last = i;
        }
        if (last >= 0) {
            while (counted < nnotes) {
                note_start += notes[counted].sec > 0.0 ? notes[counted].sec : 0.0;
                counted++;
            }
            ends[last] = note_start;
        }
    }

    /* Render. Samples go through the engine's hold / release, which keeps back the part of its
     * 5000-sample output chunk not yet complete, exactly as sam_tts_sing does; t->ev_total
     * counts every sample made, held or released, and that is the clock the sounds are timed
     * by. Nothing is rendered before the first note, so its first sample is sample 0. */
    if (rc == 0) {
        t->out_cb = cb;
        t->out_user = user;
        t->npend = 0;
        t->ev_total = 0;
        t->opts = NULL;
        sam_synth_chunk_reset(t->synth);
        if (opts && opts->starts)
            for (k = 0; k < nnotes; k++) opts->starts[k] = -1.0;
    }
    for (i = 0; i < il.n && rc == 0; i++) {
        item *x = &il.it[i];
        sam_segment g;
        double start, dur, planned = x->dur;
        int j;
        if (!x->note) continue;
        start = (double)t->ev_total / sr;
        dur = ends[i] - start;
        if (opts && opts->starts && opts->starts[x->note - 1] < 0.0) opts->starts[x->note - 1] = start;
        memset(&g, 0, sizeof g);
        g.unit = x->unit;
        if (g.unit == 0) {
            /* a rest, or a sound the voice has no unit for: silence to the scheduled end */
            if (dur * sr < 1.0) continue;
            g.dur = (float)dur;
            g.n_knots = 2;
            g.t[1] = (float)(dur * sr);
            g.f0[0] = g.f0[1] = 100.0f;
            g.amp[0] = g.amp[1] = 1.0f;
        } else {
            double floor_dur = planned < SVS_MIN_SOUND ? planned : SVS_MIN_SOUND;
            if (dur < floor_dur) dur = floor_dur;
            if (dur * sr < 1.0) continue;
            g.dur = (float)dur;
            g.hold = x->coda ? 2 : is_vowel(x->type) ? 1 : 0;
            /* the contour over the sound and a little past it: the last pitch period can start
             * after the nominal end, and past the last knot the synthesiser extrapolates */
            g.n_knots = fit_knots(f0_at, fu, start, dur + 0.02, sr, g.t, g.f0);
            for (j = 0; j < g.n_knots; j++) g.amp[j] = x->amp;
        }
        if (getenv("SAM_DEBUG"))
            fprintf(stderr, "SVS item %d type %d unit %d note %d coda %d at %.4f dur %.4f (planned %.4f) knots %d\n", i,
                    x->type, x->unit, x->note, x->coda, start, dur, planned, g.n_knots);
        svs_vib_t0 = start;
        rc = sam_synth_segment(t->synth, &g, hold, t);
        if (rc == 0) release(t, (size_t)sam_synth_chunk_pos(t->synth));
    }
    if (rc == 0) release(t, 0);
    svs_vib_depth_at = NULL;
    svs_vib_user = NULL;
    t->npend = 0;
    t->out_cb = NULL;
    t->out_user = NULL;
    free(ends);
    free(il.it);
    free(l.w);
    return rc;
}
