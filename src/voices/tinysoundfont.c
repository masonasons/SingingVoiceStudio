/* tinysoundfont.c -- TinySoundFont, one header, compiled once here
   (soundfont.cpp uses it), and what tinysoundfont.h adds to it. */
#define TSF_IMPLEMENTATION
#include "voices/tinysoundfont.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void svs_tsf_attenuation(tsf *f) {
    int p, r;
    for (p = 0; p < f->presetNum; ++p)
        for (r = 0; r < f->presets[p].regionNum; ++r) f->presets[p].regions[r].attenuation *= 4.0f;
}

/* -- modulators --------------------------------------------------------- */

struct svs_mod {
    unsigned short src, dest, amtsrc, trans;
    int amount;
};

struct svs_modlist {
    int count;
    struct svs_mod *m;
};

struct svs_tsf_mods {
    int presetNum;
    int *regionNum;                /* as TinySoundFont counted them */
    struct svs_modlist **regions;  /* [preset][region] */
};

struct svs_tsf_voices {
    int n;
    struct tsf_region **r;         /* one to a voice, never moved */
};

/* SoundFont 2's default modulators that a note-on reads: the velocity's
   attenuation. Not the velocity's filter, which VirtualMidiSynth leaves out
   too; the others (the volume, the pan, the wheels, the effects) are the
   mix's or not played. */
static const struct svs_mod kDefaults[] = {
    {0x0502, 48, 0, 0, 960},
};

static int svs_same(const struct svs_mod *a, const struct svs_mod *b) {
    return a->src == b->src && a->dest == b->dest && a->amtsrc == b->amtsrc && a->trans == b->trans;
}

/* `m` into `list`: in place of an identical one, or with its amount added */
static void svs_put(struct svs_modlist *list, const struct svs_mod *m, int add) {
    int i;
    for (i = 0; i < list->count; ++i)
        if (svs_same(&list->m[i], m)) {
            list->m[i].amount = add ? list->m[i].amount + m->amount : m->amount;
            return;
        }
    list->m = (struct svs_mod *)realloc(list->m, (list->count + 1) * sizeof(struct svs_mod));
    list->m[list->count++] = *m;
}

static unsigned short svs_u16(const unsigned char *b) { return (unsigned short)(b[0] | (b[1] << 8)); }
static unsigned svs_u32(const unsigned char *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | ((unsigned)b[3] << 24); }

struct svs_hydra {
    unsigned char *phdr, *pbag, *pmod, *pgen, *inst, *ibag, *imod, *igen;
    int phdrNum, pbagNum, pmodNum, pgenNum, instNum, ibagNum, imodNum, igenNum;
};

/* the modulators of zone `bag` into `list` */
static void svs_zone_mods(struct svs_modlist *list, const unsigned char *bags, int bag, const unsigned char *mods,
                          int modNum, int add) {
    int i, from = svs_u16(bags + bag * 4 + 2), to = svs_u16(bags + bag * 4 + 6);
    for (i = from; i < to && i < modNum; ++i) {
        const unsigned char *r = mods + i * 10;
        struct svs_mod m;
        m.src = svs_u16(r);
        m.dest = svs_u16(r + 2);
        m.amount = (short)svs_u16(r + 4);
        m.amtsrc = svs_u16(r + 6);
        m.trans = svs_u16(r + 8);
        svs_put(list, &m, add);
    }
}

static void svs_copy(struct svs_modlist *to, const struct svs_modlist *from) {
    to->count = from->count;
    to->m = (struct svs_mod *)malloc((from->count ? from->count : 1) * sizeof(struct svs_mod));
    if (from->count) memcpy(to->m, from->m, from->count * sizeof(struct svs_mod));
}

/* Each region's modulators, found as tsf_load_presets finds the regions: in
   the same order, so that region k of a preset is the same zone here. */
static int svs_build(struct svs_tsf_mods *out, const tsf *f, const struct svs_hydra *h) {
    enum { GenInstrument = 41, GenKeyRange = 43, GenVelRange = 44, GenSampleID = 53 };
    int p;
    out->presetNum = h->phdrNum - 1;
    out->regionNum = (int *)calloc(out->presetNum > 0 ? out->presetNum : 1, sizeof(int));
    out->regions = (struct svs_modlist **)calloc(out->presetNum > 0 ? out->presetNum : 1, sizeof(struct svs_modlist *));
    for (p = 0; p < out->presetNum; ++p) {
        const unsigned char *ph = h->phdr + p * 38, *other;
        int bank = svs_u16(ph + 22), preset = svs_u16(ph + 20), sorted = 0, o, count = 0, pb;
        int pbFrom = svs_u16(ph + 24), pbTo = svs_u16(ph + 38 + 24);
        struct svs_modlist *regions = TSF_NULL, presetGlobal = {0, TSF_NULL};
        unsigned char gLo = 0, gHi = 127, gVlo = 0, gVhi = 127;
        for (o = 0; o < out->presetNum; ++o) {
            int ob, op;
            other = h->phdr + o * 38;
            ob = svs_u16(other + 22), op = svs_u16(other + 20);
            if (o == p || ob > bank) continue;
            else if (ob < bank) sorted++;
            else if (op > preset) continue;
            else if (op < preset) sorted++;
            else if (o < p) sorted++;
        }
        if (sorted >= f->presetNum) continue;
        for (pb = pbFrom; pb < pbTo && pb + 1 < h->pbagNum; ++pb) {
            int g, gFrom = svs_u16(h->pbag + pb * 4), gTo = svs_u16(h->pbag + pb * 4 + 4), hadInst = 0;
            unsigned char pLo = gLo, pHi = gHi, pVlo = gVlo, pVhi = gVhi;
            struct svs_modlist presetZone;
            svs_copy(&presetZone, &presetGlobal);
            svs_zone_mods(&presetZone, h->pbag, pb, h->pmod, h->pmodNum, 0);
            for (g = gFrom; g < gTo && g < h->pgenNum; ++g) {
                const unsigned char *pg = h->pgen + g * 4;
                int op = svs_u16(pg), amt = svs_u16(pg + 2);
                if (op == GenKeyRange) { pLo = pg[2]; pHi = pg[3]; continue; }
                if (op == GenVelRange) { pVlo = pg[2]; pVhi = pg[3]; continue; }
                if (op != GenInstrument || amt >= h->instNum - 1) continue;
                {
                    const unsigned char *in = h->inst + amt * 22;
                    int ib, ibFrom = svs_u16(in + 20), ibTo = svs_u16(in + 22 + 20);
                    unsigned char iLo = 0, iHi = 127, iVlo = 0, iVhi = 127;
                    struct svs_modlist instGlobal = {0, TSF_NULL};
                    hadInst = 1;
                    for (ib = ibFrom; ib < ibTo && ib + 1 < h->ibagNum; ++ib) {
                        int ig, igFrom = svs_u16(h->ibag + ib * 4), igTo = svs_u16(h->ibag + ib * 4 + 4), hadSample = 0;
                        unsigned char zLo = iLo, zHi = iHi, zVlo = iVlo, zVhi = iVhi;
                        struct svs_modlist zone;
                        svs_copy(&zone, &instGlobal);
                        svs_zone_mods(&zone, h->ibag, ib, h->imod, h->imodNum, 0);
                        for (ig = igFrom; ig < igTo && ig < h->igenNum; ++ig) {
                            const unsigned char *zg = h->igen + ig * 4;
                            int zop = svs_u16(zg);
                            if (zop == GenKeyRange) { zLo = zg[2]; zHi = zg[3]; continue; }
                            if (zop == GenVelRange) { zVlo = zg[2]; zVhi = zg[3]; continue; }
                            if (zop != GenSampleID) continue;
                            hadSample = 1;
                            if (zHi < pLo || zLo > pHi || zVhi < pVlo || zVlo > pVhi) continue;
                            {
                                /* the defaults, the instrument's in place of the same, and
                                   the preset's added */
                                struct svs_modlist all = {0, TSF_NULL};
                                int k;
                                for (k = 0; k < (int)(sizeof kDefaults / sizeof kDefaults[0]); ++k)
                                    svs_put(&all, &kDefaults[k], 0);
                                for (k = 0; k < zone.count; ++k) svs_put(&all, &zone.m[k], 0);
                                for (k = 0; k < presetZone.count; ++k) svs_put(&all, &presetZone.m[k], 1);
                                regions = (struct svs_modlist *)realloc(regions, (count + 1) * sizeof(struct svs_modlist));
                                regions[count++] = all;
                            }
                        }
                        if (ib == ibFrom && !hadSample) {
                            iLo = zLo, iHi = zHi, iVlo = zVlo, iVhi = zVhi;
                            free(instGlobal.m);
                            instGlobal = zone;
                        } else {
                            free(zone.m);
                        }
                    }
                    free(instGlobal.m);
                }
            }
            if (pb == pbFrom && !hadInst) {
                gLo = pLo, gHi = pHi, gVlo = pVlo, gVhi = pVhi;
                free(presetGlobal.m);
                presetGlobal = presetZone;
            } else {
                free(presetZone.m);
            }
        }
        free(presetGlobal.m);
        /* only if the regions are the ones TinySoundFont made */
        if (count == f->presets[sorted].regionNum) {
            out->regionNum[sorted] = count;
            out->regions[sorted] = regions;
        } else {
            int k;
            for (k = 0; k < count; ++k) free(regions[k].m);
            free(regions);
        }
    }
    return 1;
}

struct svs_tsf_mods *svs_tsf_read_mods(const tsf *f, FILE *file) {
    unsigned char head[12], ch[8];
    unsigned char *pdta = TSF_NULL;
    unsigned size = 0;
    struct svs_hydra h;
    struct svs_tsf_mods *out;
    unsigned at;
    if (!file || fseek(file, 0, SEEK_SET) != 0 || fread(head, 1, 12, file) != 12 || memcmp(head, "RIFF", 4) ||
        memcmp(head + 8, "sfbk", 4))
        return TSF_NULL;
    while (fread(ch, 1, 8, file) == 8) {
        unsigned n = svs_u32(ch + 4);
        if (!memcmp(ch, "LIST", 4)) {
            unsigned char type[4];
            if (fread(type, 1, 4, file) != 4) break;
            if (!memcmp(type, "pdta", 4)) {
                size = n - 4;
                pdta = (unsigned char *)malloc(size ? size : 1);
                if (fread(pdta, 1, size, file) != size) {
                    free(pdta);
                    pdta = TSF_NULL;
                }
                break;
            }
            if (fseek(file, (long)(n - 4 + (n & 1)), SEEK_CUR) != 0) break;
        } else if (fseek(file, (long)(n + (n & 1)), SEEK_CUR) != 0) {
            break;
        }
    }
    if (!pdta) return TSF_NULL;
    memset(&h, 0, sizeof h);
    for (at = 0; at + 8 <= size;) {
        unsigned n = svs_u32(pdta + at + 4);
        unsigned char *d = pdta + at + 8;
        if (at + 8 + n > size) break;
#define SVS_CHUNK(name, rec, ptr, num) \
        if (!memcmp(pdta + at, name, 4)) { h.ptr = d; h.num = (int)(n / rec); }
        SVS_CHUNK("phdr", 38, phdr, phdrNum)
        SVS_CHUNK("pbag", 4, pbag, pbagNum)
        SVS_CHUNK("pmod", 10, pmod, pmodNum)
        SVS_CHUNK("pgen", 4, pgen, pgenNum)
        SVS_CHUNK("inst", 22, inst, instNum)
        SVS_CHUNK("ibag", 4, ibag, ibagNum)
        SVS_CHUNK("imod", 10, imod, imodNum)
        SVS_CHUNK("igen", 4, igen, igenNum)
#undef SVS_CHUNK
        at += 8 + n + (n & 1);
    }
    if (!h.phdr || !h.pbag || !h.pgen || !h.inst || !h.ibag || !h.igen || h.phdrNum < 2) {
        free(pdta);
        return TSF_NULL;
    }
    out = (struct svs_tsf_mods *)calloc(1, sizeof *out);
    svs_build(out, f, &h);
    free(pdta);
    return out;
}

void svs_tsf_free_mods(struct svs_tsf_mods *mods) {
    int p, r;
    if (!mods) return;
    for (p = 0; p < mods->presetNum; ++p) {
        for (r = 0; r < mods->regionNum[p]; ++r) free(mods->regions[p][r].m);
        free(mods->regions[p]);
    }
    free(mods->regions);
    free(mods->regionNum);
    free(mods);
}

void svs_tsf_free_voices(struct svs_tsf_voices *voices) {
    int i;
    if (!voices) return;
    for (i = 0; i < voices->n; ++i) free(voices->r[i]);
    free(voices->r);
    free(voices);
}

/* -- a note on ----------------------------------------------------------- */

/* A source's value, 0-1 or -1-1, as SoundFont 2 shapes it (section 8.2):
   at a note-on only the velocity and the key move; a controller is at rest. */
static float svs_source(unsigned short src, int key, int velocity) {
    int index = src & 127, cc = (src >> 7) & 1, negative = (src >> 8) & 1, bipolar = (src >> 9) & 1,
        type = src >> 10;
    float x;
    if (!cc && index == 0) return 1.0f;               /* no controller */
    if (!cc && index == 2) x = velocity / 127.0f;
    else if (!cc && index == 3) x = key / 127.0f;
    /* the sound controllers and the balance start centred */
    else if (cc && ((index >= 71 && index <= 79) || index == 8)) x = 64 / 127.0f;
    else x = 0.0f;
    if (negative) x = 1.0f - x;
    if (bipolar) x = 2.0f * x - 1.0f;
    switch (type) {
    case 1: /* concave */
    case 2: /* convex */
    {
        float a = x < 0 ? -x : x, y;
        if (type == 1) y = a >= 1.0f ? 1.0f : -(5.0f / 12.0f) * log10f(1.0f - a);
        else y = a <= 0.0f ? 0.0f : 1.0f + (5.0f / 12.0f) * log10f(a);
        if (y < 0) y = 0;
        if (y > 1) y = 1;
        return x < 0 ? -y : y;
    }
    case 3: /* switch */
        return bipolar ? (x >= 0.0f ? 1.0f : -1.0f) : (x >= 0.5f ? 1.0f : 0.0f);
    default:
        return x;
    }
}

/* whether a source is the mix's, or one that moves while a note plays:
   left out, as only a note's start is worked out here */
static int svs_skipped(unsigned short src) {
    int index = src & 127, cc = (src >> 7) & 1;
    if (cc) return index == 7 || index == 10 || index == 11 || index == 91 || index == 93;
    return index == 10 || index == 13 || index == 14 || index == 16;
}

static float svs_times(float secs, float timecents) { return secs * powf(2.0f, timecents / 1200.0f); }

/* the modulators' sum on `r`, a voice's own copy of its region; returns
   what goes on the attenuation, in centibels */
static float svs_apply(struct tsf_region *r, const struct svs_modlist *list, int key, int velocity) {
    float att = 0, sum[64];
    int i;
    memset(sum, 0, sizeof sum);
    for (i = 0; i < list->count; ++i) {
        const struct svs_mod *m = &list->m[i];
        float v;
        if (m->dest >= 64 || svs_skipped(m->src) || (m->amtsrc && svs_skipped(m->amtsrc))) continue;
        v = m->amount * svs_source(m->src, key, velocity) * svs_source(m->amtsrc, key, velocity);
        if (m->trans == 2 && v < 0) v = -v;
        sum[m->dest] += v;
    }
    r->modLfoToPitch += (int)sum[5];
    r->vibLfoToPitch += (int)sum[6];
    r->modEnvToPitch += (int)sum[7];
    r->initialFilterFc += (int)sum[8];
    r->initialFilterQ += (int)sum[9];
    r->modLfoToFilterFc += (int)sum[10];
    r->modEnvToFilterFc += (int)sum[11];
    r->modLfoToVolume += (int)sum[13];
    r->pan += sum[17] * 0.001f;
    if (r->pan < -0.5f) r->pan = -0.5f;
    if (r->pan > 0.5f) r->pan = 0.5f;
    r->delayModLFO = svs_times(r->delayModLFO, sum[21]);
    r->freqModLFO += (int)sum[22];
    r->delayVibLFO = svs_times(r->delayVibLFO, sum[23]);
    r->freqVibLFO += (int)sum[24];
    /* the envelopes' times are seconds by now, but a hold or decay that
       follows the key is still timecents (tsf_region_envtosecs) */
    r->modenv.delay = svs_times(r->modenv.delay, sum[25]);
    r->modenv.attack = svs_times(r->modenv.attack, sum[26]);
    if (r->modenv.keynumToHold) r->modenv.hold += sum[27];
    else r->modenv.hold = svs_times(r->modenv.hold, sum[27]);
    if (r->modenv.keynumToDecay) r->modenv.decay += sum[28];
    else r->modenv.decay = svs_times(r->modenv.decay, sum[28]);
    r->modenv.sustain -= sum[29] / 1000.0f;
    if (r->modenv.sustain < 0) r->modenv.sustain = 0;
    if (r->modenv.sustain > 1) r->modenv.sustain = 1;
    r->modenv.release = svs_times(r->modenv.release, sum[30]);
    r->ampenv.delay = svs_times(r->ampenv.delay, sum[33]);
    r->ampenv.attack = svs_times(r->ampenv.attack, sum[34]);
    if (r->ampenv.keynumToHold) r->ampenv.hold += sum[35];
    else r->ampenv.hold = svs_times(r->ampenv.hold, sum[35]);
    if (r->ampenv.keynumToDecay) r->ampenv.decay += sum[36];
    else r->ampenv.decay = svs_times(r->ampenv.decay, sum[36]);
    r->ampenv.sustain *= powf(10.0f, -sum[37] / 200.0f);
    if (r->ampenv.sustain > 1) r->ampenv.sustain = 1;
    r->ampenv.release = svs_times(r->ampenv.release, sum[38]);
    att = sum[48];
    r->transpose += (int)sum[51];
    r->tune += (int)sum[52];
    r->pitch_keytrack += (int)sum[56];
    return att;
}

void svs_tsf_note_on(tsf *f, const struct svs_tsf_mods *mods, struct svs_tsf_voices **voices, int channel, int key,
                     int velocity) {
    struct tsf_voice *v;
    struct tsf_channel *c;
    float used = (velocity + 0.5f) / 127.0f;
    struct svs_tsf_voices *own = *voices;
    int i;
    /* half a step up, so that TinySoundFont's whole velocity, which picks the
       zones, is `velocity` itself */
    tsf_channel_note_on(f, channel, key, used);
    if (!f->channels || channel >= f->channels->channelNum) return;
    c = &f->channels->channels[channel];
    if (!own) {
        own = *voices = (struct svs_tsf_voices *)calloc(1, sizeof *own);
        if (!own) return;
    }
    if (own->n < f->voiceNum) {
        struct tsf_region **r = (struct tsf_region **)realloc(own->r, f->voiceNum * sizeof *r);
        if (!r) return;
        own->r = r;
        for (i = own->n; i < f->voiceNum; ++i) own->r[i] = (struct tsf_region *)malloc(sizeof(struct tsf_region));
        own->n = f->voiceNum;
    }
    for (i = 0; i < f->voiceNum; ++i) {
        struct tsf_preset *preset;
        struct tsf_region *r;
        const struct svs_modlist *list = TSF_NULL;
        struct svs_modlist defaults;
        int k;
        float att, newpan, lowpassFc, lowpassFilterQDB;
        v = &f->voices[i];
        if (v->playingChannel != channel || v->playingKey != key || v->playIndex != f->voicePlayIndex - 1 ||
            v->playingPreset < 0 || !own->r[i])
            continue;
        preset = &f->presets[v->playingPreset];
        k = (int)(v->region - preset->regions);
        if (mods && v->playingPreset < mods->presetNum && mods->regions[v->playingPreset] && k >= 0 &&
            k < mods->regionNum[v->playingPreset])
            list = &mods->regions[v->playingPreset][k];
        if (!list) {
            defaults.count = (int)(sizeof kDefaults / sizeof kDefaults[0]);
            defaults.m = (struct svs_mod *)kDefaults;
            list = &defaults;
        }
        r = own->r[i];
        *r = *v->region;
        att = svs_apply(r, list, key, velocity);
        v->region = r;
        /* the velocity's attenuation is the modulators', in place of
           TinySoundFont's own */
        v->noteGainDB += -att / 10.0f - 20.0f * log10f(used);
        /* and what TinySoundFont set from the region, set again from the copy */
        newpan = r->pan + c->panOffset;
        if (newpan <= -0.5f) { v->panFactorLeft = 1.0f; v->panFactorRight = 0.0f; }
        else if (newpan >= 0.5f) { v->panFactorLeft = 0.0f; v->panFactorRight = 1.0f; }
        else { v->panFactorLeft = TSF_SQRTF(0.5f - newpan); v->panFactorRight = TSF_SQRTF(0.5f + newpan); }
        tsf_voice_calcpitchratio(v, (c->pitchWheel == 8192 ? c->tuning
                                                           : ((c->pitchWheel / 16383.0f * c->pitchRange * 2.0f) -
                                                              c->pitchRange + c->tuning)),
                                 f->outSampleRate);
        tsf_voice_envelope_setup(&v->ampenv, &r->ampenv, key, (short)velocity, TSF_TRUE, f->outSampleRate);
        tsf_voice_envelope_setup(&v->modenv, &r->modenv, key, (short)velocity, TSF_FALSE, f->outSampleRate);
        lowpassFc = (r->initialFilterFc <= 13500 ? tsf_cents2Hertz((float)r->initialFilterFc) / f->outSampleRate : 1.0f);
        lowpassFilterQDB = r->initialFilterQ / 10.0f;
        v->lowpass.QInv = 1.0 / TSF_POW(10.0, (lowpassFilterQDB / 20.0));
        v->lowpass.z1 = v->lowpass.z2 = 0;
        v->lowpass.active = (lowpassFc < 0.499f);
        if (v->lowpass.active) tsf_voice_lowpass_setup(&v->lowpass, lowpassFc);
        tsf_voice_lfo_setup(&v->modlfo, r->delayModLFO, r->freqModLFO, f->outSampleRate);
        tsf_voice_lfo_setup(&v->viblfo, r->delayVibLFO, r->freqVibLFO, f->outSampleRate);
    }
}
