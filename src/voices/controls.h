// controls.h -- the voice controls the song settings and the track dialog
// offer, as VocalWriter exposes them: (key, engine call, default, low, high,
// label, hint).
//
// The defaults are VocalWriter's engine's own, read out of its context after
// InitDefaultVoiceCntrls has run. A control is sent only when it differs from
// its default, so a song that sets none of them sounds exactly as it did
// before they existed. The other engines take what they can from them: the
// vibrato, the detune and the portamento all become part of the pitch curve
// (see pitch.h); the colour, chorus and breath are VocalWriter's alone.
#pragma once

#include <map>
#include <string>

#include "voices/singer.h"

namespace svs {

struct VoiceControl {
    const char *key;
    const char *call;     // VocalWriter's own name for it
    int def;
    int lo;
    int hi;
    const char *label;
    const char *hint;
    int VoiceStyle::*field;
};

inline const VoiceControl *voice_controls(size_t *count) {
    static const VoiceControl table[] = {
        {"color", "Speech_Color", 95, 0, 127, "Colour", "brighter voice as it rises",
         &VoiceStyle::color},
        {"vibrato", "Speech_VibDepth", 31, 0, 127, "Vibrato depth", "0 for none",
         &VoiceStyle::vibrato},
        {"vibrato_rate", "Speech_VibFreq", 47, 0, 127, "Vibrato rate", "how fast it wavers",
         &VoiceStyle::vibrato_rate},
        {"chorus", "Speech_Chorus", 0, 0, 127, "Chorus", "thickens the voice",
         &VoiceStyle::chorus},
        {"breath", "Speech_Breath", 0, 0, 127, "Breath", "adds air to the tone",
         &VoiceStyle::breath},
        {"detune", "Speech_Detune", 0, -8192, 8191, "Detune", "a shade sharp or flat",
         &VoiceStyle::detune},
        {"portamento", "Speech_Portamento", 0, 0, 127, "Portamento",
         "glide between notes; 0 goes straight there", &VoiceStyle::portamento},
    };
    *count = sizeof table / sizeof table[0];
    return table;
}

//: Clamp every control into its range.
inline VoiceStyle clean_voice(const VoiceStyle &v) {
    size_t n;
    const VoiceControl *c = voice_controls(&n);
    VoiceStyle out = v;
    for (size_t i = 0; i < n; ++i) {
        int x = out.*(c[i].field);
        out.*(c[i].field) = x < c[i].lo ? c[i].lo : x > c[i].hi ? c[i].hi : x;
    }
    return out;
}

}  // namespace svs
