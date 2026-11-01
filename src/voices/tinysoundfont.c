/* tinysoundfont.c -- TinySoundFont, one header, compiled once here
   (soundfont.cpp uses it), and what tinysoundfont.h adds to it. */
#define TSF_IMPLEMENTATION
#include "voices/tinysoundfont.h"

#include <math.h>

void svs_tsf_attenuation(tsf *f) {
    int p, r;
    for (p = 0; p < f->presetNum; ++p)
        for (r = 0; r < f->presets[p].regionNum; ++r) f->presets[p].regions[r].attenuation *= 4.0f;
}

void svs_tsf_note_on(tsf *f, int channel, int key, int velocity) {
    struct tsf_voice *v;
    float vel = velocity / 127.0f;
    /* half a step up, so that TinySoundFont's whole velocity, which picks the
       zones, is `velocity` itself */
    tsf_channel_note_on(f, channel, key, (velocity + 0.5f) / 127.0f);
    for (v = f->voices; v != f->voices + f->voiceNum; ++v)
        if (v->playingChannel == channel && v->playingKey == key && v->playIndex == f->voicePlayIndex - 1)
            v->noteGainDB += 40.0f * log10f(vel) - 20.0f * log10f((velocity + 0.5f) / 127.0f);
}
