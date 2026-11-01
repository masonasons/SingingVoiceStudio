// tinysoundfont.h -- what soundfont.cpp adds to TinySoundFont, to play a
// SoundFont as the SoundFont players do (tinysoundfont.c).
#pragma once

#include "tsf.h"

#ifdef __cplusplus
extern "C" {
#endif

//: Each preset's own attenuation at 0.04 dB to the centibel, as E-mu's cards
//: and the players since have it, where TinySoundFont takes 0.01. Called once,
//: after loading.
void svs_tsf_attenuation(tsf *f);
//: A note on at `velocity`, 1-127, as SoundFont 2's default modulator has it:
//: 40 log10(velocity / 127) dB, where TinySoundFont gives half of that.
void svs_tsf_note_on(tsf *f, int channel, int key, int velocity);

#ifdef __cplusplus
}
#endif
