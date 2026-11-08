/*
 * bestspeech_port.c -- BeSTspeech's own stages, from a stream of its sound
 * codes to the frames its lattice plays.
 *
 * third_party/openbst's bst_say runs text through the tokeniser and the
 * assembler and then these same stages in this order (src/speak.c); here the
 * stream is written by src/voices/bestspeech.cpp from the notes instead, and
 * the frames come back rather than going to the lattice, each with the
 * segment it was made from, which the frame generator says through the hook
 * cmake/engines/bestspeech.cmake adds to it.
 */
#include <stdlib.h>
#include <string.h>

#include "bst.h"
#include "bst_frames.h"
#include "bst_priv.h"
#include "bst_text.h"

#include "voices/bestspeech_port.h"

#define MAXSEG  0xD0
#define MAXTRN  0x210
#define MAXITO  0x60
#define MAXEMIT 4096

extern void (*svs_bst_frame_hook)(int frame, int segment);

/* Only the 1995 build's tables are compiled in, so this stands in for the
   index src/data/lifted.c keeps of all twenty. */
extern const bst_lifted BST_DATA_1995;

const bst_lifted *bst_lifted_for(const char *build)
{
    return build && strcmp(build, "1995") == 0 ? &BST_DATA_1995 : 0;
}

static int *seg_of;
static int seg_room;

static void heard(int frame, int segment)
{
    if (frame >= 0 && frame < seg_room)
        seg_of[frame] = segment;
}

int svs_bst_frames(bst *h, const char *build, int voice, uint8_t *stream, int len, int hdr,
                   uint8_t *frames, int max_frames, int *frame_seg, int *seg_sound, int max_seg,
                   int *nseg)
{
    const bst_image *img = bst_handle_image(h);
    const bst_tabmap *map = bst_map_named(build);
    bst_tables tab;
    bst_pair_state ps = { 0x30, 0, 0, 0 };
    bst_accent_state as = { 0, 0, 0, -1, 0 };
    bst_voice v;
    bst_gen g;
    int level, ne, ns = 0, nt = 0, ni, m, i;
    bst_emit *em = malloc(sizeof *em * MAXEMIT);
    bst_seg_rec *seg = malloc(sizeof *seg * MAXSEG);
    bst_trn_rec *trn = malloc(sizeof *trn * MAXTRN);
    bst_ito_rec *ito = malloc(sizeof *ito * MAXITO);
    bst_contour_rec *cr = malloc(sizeof *cr * MAXITO);

    *nseg = 0;
    if (!img || !map || !em || !seg || !trn || !ito || !cr) {
        m = -1;
        goto done;
    }
    bst_handle_tables(h, &tab);
    level = map->init_level ? map->init_level : 3;

    /* as bst_say has them, phrase by phrase */
    hdr += bst_phrules(img, stream, &len, BST_SVS_STREAM, 0);
    as.level = level;
    as.tail = hdr >= 0 ? hdr + 3 : -1;
    bst_accents(img, stream, len, &as);
    level = as.level;

    ne = bst_pairs(img, stream, len, &ps, em, MAXEMIT);
    if (ne < 0)
        ne = MAXEMIT;
    for (i = 0; i < ne; i++) {
        if (em[i].kind == BST_EMIT_SEG) {
            if (ns >= MAXSEG)
                break;
            seg[ns].count = (int16_t)em[i].count;
            seg[ns].index = em[i].index;
            if (em[i].index & 0xF000) {
                seg[ns].a = em[i].a;
                seg[ns].b = em[i].b;
            } else {
                seg[ns].a = -1;
                seg[ns].b = -1;
            }
            if (ns < max_seg)
                seg_sound[ns] = (em[i].index & 0xF000) ? -1 : em[i].index % 48;
            ns++;
        } else {
            if (nt >= MAXTRN)
                break;
            trn[nt].index = (uint8_t)em[i].index;
            trn[nt].dur = (uint8_t)em[i].count;
            trn[nt].p1 = (uint8_t)em[i].a;
            trn[nt].p2 = (uint8_t)em[i].b;
            trn[nt].span = -1;
            nt++;
        }
    }
    *nseg = ns < max_seg ? ns : max_seg;

    memset(&v, 0, sizeof v);
    v.base = v.voicebase = 0x50;
    v.top = map->voice_top ? map->voice_top : 0xA0;
    v.voice = voice;
    v.level = level;
    v.strong = ps.strong;
    ni = bst_contour(img, stream, len, &v, cr, MAXITO);
    if (ni > MAXITO)
        ni = MAXITO;
    for (i = 0; i < ni; i++) {
        ito[i].kind = cr[i].kind;
        ito[i].period = cr[i].period;
        ito[i].dur = cr[i].dur;
        ito[i].slope = cr[i].slope;
    }

    memset(&g, 0, sizeof g);
    g.img = img;
    g.tab = &tab;
    g.seg = seg;
    g.nseg = ns;
    g.trn = trn;
    g.ntrn = nt;
    g.ito = ito;
    g.nito = ni;
    g.voice = voice;
    g.rate = 0;
    g.rate_loaded = 0;
    g.flags = 0;
    g.defexc = 0x30;
    g.gain_base = 0x10;
    g.gain_adj = -0x12;
    g.out = frames;
    g.maxout = max_frames;
    seg_of = frame_seg;
    seg_room = max_frames;
    svs_bst_frame_hook = heard;
    m = bst_generate(&g);
    svs_bst_frame_hook = 0;
    if (m > max_frames)
        m = max_frames;

done:
    free(em);
    free(seg);
    free(trn);
    free(ito);
    free(cr);
    return m;
}

struct svs_bst_lattice {
    bst_tables tab;
    bst_synth synth;
};

svs_bst_lattice *svs_bst_lattice_new(bst *h)
{
    svs_bst_lattice *l = calloc(1, sizeof *l);

    if (!l)
        return 0;
    bst_handle_tables(h, &l->tab);
    bst_synth_init(&l->synth, &l->tab);
    return l;
}

void svs_bst_lattice_free(svs_bst_lattice *l)
{
    free(l);
}

size_t svs_bst_lattice_frame(svs_bst_lattice *l, const uint8_t f[16], int16_t *out, size_t max)
{
    if (!bst_synth_frame(&l->synth, f))
        return 0;
    return bst_synth_run(&l->synth, out, max);
}
