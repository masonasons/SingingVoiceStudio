/* sam_sing.h -- Microsoft Sam, Mike and Mary singing phonemes rather than words.
 *
 * The decompiled engine (third_party/ms-sam-mike-mary-decomp) has a singing mode of its own,
 * sam_tts_sing, but it takes a score of words and looks them up in Microsoft's dictionary. The
 * studio already knows what every note sings, phoneme by phoneme, and where its pitch is at every
 * moment, so this is the same singing mode one level down: notes of SAPI phone ids, each lasting
 * exactly the length it is given, and a pitch contour sampled wherever the sounds fall.
 *
 * Only the voice file (<Voice>.spd) is needed. The dictionary and the letter-to-sound rules
 * (LTTS1033.LXA, r1033tts.LXA) are what turn words into phonemes, and nothing here has any words.
 */
#ifndef SVS_SAM_SING_H
#define SVS_SAM_SING_H

#include "sam.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The SAPI 5 US English phone ids the notes are written in, as SAPI_PHONE in sam_front.c has
 * them. 1 is a syllable boundary and 8 / 9 primary / secondary stress, written after the vowel
 * they belong to; 10..49 are the sounds. */
enum {
    SVS_SAPI_SYL = 1, SVS_SAPI_ST1 = 8, SVS_SAPI_ST2 = 9,
    SVS_SAPI_AA = 10, SVS_SAPI_AE, SVS_SAPI_AH, SVS_SAPI_AO, SVS_SAPI_AW, SVS_SAPI_AX, SVS_SAPI_AY,
    SVS_SAPI_B, SVS_SAPI_CH, SVS_SAPI_D, SVS_SAPI_DH, SVS_SAPI_EH, SVS_SAPI_ER, SVS_SAPI_EY,
    SVS_SAPI_F, SVS_SAPI_G, SVS_SAPI_H, SVS_SAPI_IH, SVS_SAPI_IY, SVS_SAPI_JH, SVS_SAPI_K,
    SVS_SAPI_L, SVS_SAPI_M, SVS_SAPI_N, SVS_SAPI_NG, SVS_SAPI_OW, SVS_SAPI_OY, SVS_SAPI_P,
    SVS_SAPI_R, SVS_SAPI_S, SVS_SAPI_SH, SVS_SAPI_T, SVS_SAPI_TH, SVS_SAPI_UH, SVS_SAPI_UW,
    SVS_SAPI_V, SVS_SAPI_W, SVS_SAPI_Y, SVS_SAPI_Z, SVS_SAPI_ZH /* = 49 */
};

/* One note: its phones (stress marks included) and how long it lasts. A note with no phones,
 * or none the engine knows, is a rest -- silence for its length. */
typedef struct {
    const int *phones;
    int nphones;
    double sec;
} svs_sam_note;

typedef struct {
    /* the consonants' natural lengths are multiplied by this (1 = as the engine speaks them) */
    double consonant_scale;
    /* A vibrato for the synthesiser to add on every pitch period, in cents and Hz (0 = none).
     * The contour reaches the engine as at most 20 points per sound, too few to carry a vibrato
     * through a long vowel, so a caller with a vibrato passes it here and leaves it out of the
     * contour. It fades in from 0.15 s into each sound, as the engine's singing mode does. */
    double vibrato_cents, vibrato_hz;
    /* Optional: the vibrato depth in cents at `sec` seconds into the phrase, for a depth that
     * moves (the mod wheel). Asked on every pitch period; vibrato_hz still sets the speed. */
    double (*vibrato_at)(void *user, double sec);
    void *vibrato_user;
    /* SAPI 4 Whisper: every frame takes the noise excitation */
    int whisper;
    /* optional, nnotes entries: where each note's first sound actually started, in seconds
     * (-1 for a note that made no sound at all), for checking the timing */
    double *starts;
} svs_sam_sing_opts;

/* A singing engine over a loaded voice. The voice is borrowed: it must outlive the engine, and
 * since synthesis only ever reads it, any number of engines on any threads may share one. One
 * engine is used by one thread at a time. NULL when out of memory. */
sam_tts *svs_sam_open(const sam_voice *voice);
void svs_sam_close(sam_tts *t);

/* Sing `nnotes` notes, one after another, starting at once: the first note's first sound is the
 * first sample delivered, and note k starts (to within a pitch period or so) at the sum of the
 * lengths before it. Within a note the consonants keep their natural length and the vowel, with
 * an r, l, m, n or ng after it, holds the rest; when a note is too short for its consonants they
 * are shortened too, so that no note ever runs over.
 *
 * f0_at(fu, sec) is the pitch in Hz at `sec` seconds into the phrase. 22050 Hz 16-bit mono
 * arrives through cb, about sum(sec) * 22050 samples in all. Every call starts from a fresh
 * synthesiser, so the same notes always give the same samples. Returns 0, or -1 on error. */
int svs_sam_sing_phones(sam_tts *t, const svs_sam_note *notes, int nnotes,
                        double (*f0_at)(void *fu, double sec), void *fu,
                        const svs_sam_sing_opts *opts, sam_pcm_cb cb, void *user);

/* Whether the engine has a sound for SAPI phone id `sapi` (10..49), for self-checks. */
int svs_sam_knows_phone(int sapi);
/* ... and whether it counts that sound as a vowel */
int svs_sam_phone_is_vowel(int sapi);

#ifdef __cplusplus
}
#endif
#endif
