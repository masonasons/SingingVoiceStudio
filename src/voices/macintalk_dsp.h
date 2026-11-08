// macintalk_dsp.h -- retiming and retuning speech to a score.
//
// What Apple's engine gives back on a Mac is speech: each syllable at its
// own length, at a flat baseline pitch near the note (macintalk_mac.h). What
// the score wants is every syllable exactly as long as its note and its
// pitch following the phrase's curve, vibrato, bends and all. The gap is
// closed here with pitch-synchronous overlap-add (TD-PSOLA), the classic
// way of changing the pitch and length of speech without moving its
// formants: the voiced parts are cut into one grain per glottal period, two
// periods wide, and the grains are laid down again at the spacing of the
// pitch wanted, taken from where in the syllable the new timing says. The
// unvoiced parts (s, f, the burst of a t) are laid down unchanged at a fixed
// hop. Nothing here knows about MacinTalk: it is given samples.
#pragma once

#include <cstddef>
#include <functional>
#include <utility>
#include <vector>

namespace svs {
namespace dsp {

//: Where the voice is, and at what pitch: `f0[i]` is the fundamental in
//: hertz at `i * hop_s` seconds, 0 where there is no voicing.
struct F0Track {
    double hop_s = 0.005;
    std::vector<double> f0;
    double at(double seconds) const;
    bool voiced_at(double seconds) const { return at(seconds) > 0; }
};

//: The YIN estimator (de Cheveigne and Kawahara, 2002): the cumulative mean
//: normalised difference, the first dip under a threshold, refined by a
//: parabola. Frames of about 46 ms every 5 ms.
F0Track track_f0(const std::vector<float> &x, double rate, double fmin = 55.0, double fmax = 1100.0);

//: The pitch of samples an engine was told to sing, frame by frame: each
//: frame `frame` samples long from where it starts, with its pitch, 0 where
//: it is unvoiced; `length` samples in all.
F0Track frame_track(const std::vector<std::pair<size_t, double>> &frames, size_t frame, size_t length, double rate);

struct Segment {
    size_t begin = 0, end = 0;   //: samples, [begin, end)
};

//: The stretches of sound in `x`: anything louder than `floor_db` below the
//: loudest moment, with gaps shorter than `gap_s` bridged, and anything
//: shorter than `min_s` dropped.
std::vector<Segment> split_on_silence(const std::vector<float> &x, double rate, double gap_s,
                                      double floor_db = -40.0, double min_s = 0.030);

//: The root mean square of the samples, or of the voiced ones only if the
//: track has any.
double rms(const std::vector<float> &x, double rate, const F0Track *voicing);

//: Resynthesise `src` into `length` samples. `source_time(t)` says which
//: moment of `src` (seconds) the output at `t` seconds should come from --
//: a non-decreasing map, which is how the lengths are changed -- and
//: `target_hz(t)` the pitch wanted there (0 to keep the source's own). The
//: voiced grains are scaled so the loudness stays as it was whatever the
//: new spacing.
std::vector<float> psola(const std::vector<float> &src, double rate, const F0Track &f0, size_t length,
                         const std::function<double(double)> &source_time,
                         const std::function<double(double)> &target_hz);

//: `psola` for a voice whose waveform has more than one peak a period of
//: about the same height, as a formant synthesiser's often has, where the
//: strongest peak moves from one to another and the grains come back
//: jittered. Each mark is put one period on from the last where the period
//: after it best matches the one before (to a fraction of a sample, by a
//: parabola through the correlations), and the grains are read and laid
//: down between samples. Eloquence and the Prose 2000 are retuned through
//: this one.
std::vector<float> psola_matched(const std::vector<float> &src, double rate, const F0Track &f0, size_t length,
                                 const std::function<double(double)> &source_time,
                                 const std::function<double(double)> &target_hz);

//: A time map for a syllable, for `psola`: the vowel -- the longest voiced
//: stretch -- is stretched or squeezed to fill the note, and the consonants
//: before and after it are given `head_out` and `tail_out` seconds (their
//: own lengths, if negative). A syllable that has to be shortened past that
//: is shortened evenly.
std::function<double(double)> syllable_time_map(size_t src_length, double rate, const F0Track &f0,
                                                size_t out_length, double head_out = -1.0,
                                                double tail_out = -1.0);

}  // namespace dsp
}  // namespace svs
