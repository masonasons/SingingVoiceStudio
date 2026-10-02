// pitch.h -- where a phrase's pitch is at any moment, for engines that take
// a pitch contour rather than notes.
//
// VocalWriter's engine is handed notes and controls and does its own vibrato,
// glide and bend. DECtalk, the SSI-263 and Microsoft's voices are handed
// something lower down -- a pitch per phoneme, an inflection register, a
// contour per acoustic unit -- so for them the curve is worked out here, once,
// the same way for all three: the written note, the bend in force, the
// detune, a glide between notes when portamento asks for one, and a vibrato
// that fades in after the start of each note, as a singer's does.
#pragma once

#include <vector>

#include "voices/singer.h"

namespace svs {

class PitchCurve {
public:
    explicit PitchCurve(const Phrase &phrase);

    //: The pitch at `t` seconds into the phrase, as a fractional MIDI note.
    double midi_at(double t) const;
    double hz_at(double t) const;
    //: The same without the vibrato: what the note is "on". Engines whose
    //: pitch can only change at phoneme boundaries use this.
    double steady_midi_at(double t) const;

    //: Where each note of the phrase starts and ends, in seconds.
    double note_start(size_t i) const { return starts_[i]; }
    double note_end(size_t i) const { return starts_[i] + lengths_[i]; }
    double total() const { return total_; }
    size_t notes() const { return starts_.size(); }
    //: which note `t` falls in (the last one past the end)
    size_t note_at(double t) const;

    static double midi_to_hz(double midi);

    //: the controls as the curve uses them; vibrato_cents() is the depth with
    //: the mod wheel at rest, vibrato_cents_at() follows the wheel
    double vibrato_cents() const { return vib_cents_; }
    double vibrato_cents_at(double t) const;
    bool has_mod() const { return !mods_.empty(); }
    double vibrato_hz() const { return vib_hz_; }

private:
    double bend_at(double t) const;

    std::vector<double> starts_, lengths_;
    std::vector<int> pitches_;
    std::vector<BendEvent> bends_;
    std::vector<BendEvent> mods_;     // .semitones holds the wheel, 0-127
    int vib_control_ = 31;
    double total_ = 0.0;
    double vib_cents_ = 0.0, vib_hz_ = 5.0, detune_ = 0.0, glide_ = 0.0;
};

//: Turn VocalWriter's 0-127 controls into something an engine can use.
double vibrato_depth_cents(int value);
double vibrato_rate_hz(int value);
double detune_semitones(int value);
double portamento_seconds(int value);

//: Resample mono floats from `rate` to kSampleRate, band-limited.
std::vector<float> resample_to_output(const std::vector<float> &in, double rate);

}  // namespace svs
