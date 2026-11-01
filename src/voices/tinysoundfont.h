// tinysoundfont.h -- what soundfont.cpp adds to TinySoundFont, to play a
// SoundFont as the SoundFont players do (tinysoundfont.c).
#pragma once

#include <stdio.h>

#include "tsf.h"

#ifdef __cplusplus
extern "C" {
#endif

//: Each preset's own attenuation at 0.04 dB to the centibel, as E-mu's cards
//: and the players since have it, where TinySoundFont takes 0.01. Called once,
//: after loading.
void svs_tsf_attenuation(tsf *f);

//: A SoundFont's modulators, which TinySoundFont leaves out: each region's,
//: merged as SoundFont 2 merges them (section 9.5), with the default ones.
struct svs_tsf_mods;
//: Read from `file`, the SoundFont `f` was loaded from; null if they cannot
//: be read, and the regions then play with the default modulators.
struct svs_tsf_mods *svs_tsf_read_mods(const tsf *f, FILE *file);
void svs_tsf_free_mods(struct svs_tsf_mods *mods);

//: The regions one player's voices play, each with its modulators applied.
struct svs_tsf_voices;
void svs_tsf_free_voices(struct svs_tsf_voices *voices);

//: A note on at `velocity`, 1-127, its regions' modulators worked out from
//: the velocity and the key: the velocity's attenuation, the filter, the
//: envelopes, the tuning and the rest (`mods` may be null for the defaults
//: alone). `voices` is the player's own, made when first needed.
void svs_tsf_note_on(tsf *f, const struct svs_tsf_mods *mods, struct svs_tsf_voices **voices, int channel, int key,
                     int velocity);

#ifdef __cplusplus
}
#endif
