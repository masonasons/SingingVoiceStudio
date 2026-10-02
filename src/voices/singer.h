// singer.h -- what every voice engine is asked to do: sing one phrase.
//
// A phrase is a run of notes with no rest inside it. The renderer above this
// cuts a song at its rests, places each phrase at the time the score puts it,
// mixes the parts and adds the reverb; an engine only ever has to turn notes
// into samples. Everything here is in the studio's own terms -- VocalWriter's
// phoneme symbols, MIDI pitches, lengths in beats -- and each engine
// translates into its own.
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace svs {

//: Every engine delivers 44.1 kHz mono floats, full scale at +-1. An engine
//: that runs at another rate resamples before handing its samples back.
constexpr int kSampleRate = 44100;

//: The phoneme that is silence. A note holding nothing else is a rest.
inline const char *const kRest = "%";

//: One sung note. `durations_ms` has one entry per phoneme and they add up
//: to the note's length: the consonants at the length the singer gives them
//: and the vowels holding the rest (see phonology.h, syllable_lengths).
struct SungNote {
    int midi = 60;
    double beats = 0.5;
    int velocity = 64;
    std::vector<std::string> phonemes;
    std::vector<double> durations_ms;
};

//: A bend in force from `seconds` (from the start of the phrase) onwards, in
//: semitones. Already filled in between the written points at 5 ms steps, so
//: an engine can simply hold each value until the next one.
struct BendEvent {
    double seconds = 0.0;
    double semitones = 0.0;
};

//: VocalWriter's own voice controls, 0-127 except detune (-8192..8191). They
//: are the song's, or the part's own. Engines that have nothing matching a
//: control approximate it from the pitch curve (vibrato, detune,
//: portamento) or leave it alone.
struct VoiceStyle {
    int color = 95;
    int vibrato = 31;
    int vibrato_rate = 47;
    int chorus = 0;
    int breath = 0;
    int detune = 0;
    int portamento = 0;
    bool operator==(const VoiceStyle &o) const {
        return color == o.color && vibrato == o.vibrato &&
               vibrato_rate == o.vibrato_rate && chorus == o.chorus &&
               breath == o.breath && detune == o.detune &&
               portamento == o.portamento;
    }
    bool operator!=(const VoiceStyle &o) const { return !(*this == o); }
};

struct Phrase {
    double bpm = 120.0;
    //: The notes in order. The last one is always a marked rest of 0.4
    //: beats, velocity 1: something for the final syllable to be scaled
    //: against and room for it to decay rather than being cut off.
    std::vector<SungNote> notes;
    std::vector<BendEvent> bends;
    //: Whether the part has any bend written anywhere. VocalWriter's engine
    //: is put into its bend range only then, so a part without bends renders
    //: exactly as it always did. Other engines can ignore it.
    bool any_bends = false;
    //: The mod wheel, 0 to 127, from `seconds` onwards, filled in at 5 ms
    //: steps like the bends. As General MIDI has it, the wheel is vibrato: at
    //: 0 the part sings with its own vibrato setting, and turning it up takes
    //: the depth towards the control's full 127 (see effective_vibrato). Empty
    //: when the part has no mod wheel anywhere, which leaves it as it was.
    std::vector<BendEvent> mods;
    VoiceStyle style;
    //: The part's volume as a factor, already multiplied by whatever the
    //: voice needs to stay inside full scale.
    double level = 1.0;
};

//: The rendered phrase. `stopped_short` says the engine ran out of room or
//: frames before the singing finished, which the window reports.
struct Sung {
    std::vector<float> samples;
    bool stopped_short = false;
};

//: The vibrato depth control (0-127) a part sings with when its own setting
//: is `base` and the mod wheel stands at `mod`.
inline double effective_vibrato(int base, double mod) {
    double m = mod < 0 ? 0 : mod > 127 ? 127 : mod;
    return base + (127.0 - base) * m / 127.0;
}

class Singer {
public:
    virtual ~Singer() = default;
    virtual Sung sing(const Phrase &phrase) = 0;
};

}  // namespace svs
