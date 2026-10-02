#include "voices/pitch.h"

#include <algorithm>
#include <cmath>

namespace svs {

namespace {
const double kPi = 3.14159265358979323846;
//: how long into a note the vibrato takes to arrive, as a singer's does
const double kVibratoOnset = 0.18;
}  // namespace

// VocalWriter's defaults (31 and 47) come out as a gentle vibrato of about a
// quarter of a semitone at a little under five a second, which is roughly what
// its own voices do with them. Zero is none at all.
double vibrato_depth_cents(int value) { return std::max(0, value) * 0.8; }
double vibrato_rate_hz(int value) { return 3.0 + std::max(0, value) / 127.0 * 5.0; }
// The engine's detune is a 14-bit control around zero; full scale is taken
// as a semitone either way.
double detune_semitones(int value) { return value / 8192.0; }
double portamento_seconds(int value) { return std::max(0, value) / 127.0 * 0.3; }

double PitchCurve::midi_to_hz(double midi) {
    return 440.0 * std::pow(2.0, (midi - 69.0) / 12.0);
}

PitchCurve::PitchCurve(const Phrase &phrase) {
    double spb = 60.0 / std::max(phrase.bpm, 1e-6);
    double at = 0.0;
    for (const SungNote &n : phrase.notes) {
        starts_.push_back(at);
        lengths_.push_back(n.beats * spb);
        pitches_.push_back(n.midi);
        at += n.beats * spb;
    }
    total_ = at;
    bends_ = phrase.bends;
    std::stable_sort(bends_.begin(), bends_.end(),
                     [](const BendEvent &a, const BendEvent &b) {
                         return a.seconds < b.seconds;
                     });
    mods_ = phrase.mods;
    std::stable_sort(mods_.begin(), mods_.end(),
                     [](const BendEvent &a, const BendEvent &b) { return a.seconds < b.seconds; });
    vib_control_ = phrase.style.vibrato;
    vib_cents_ = vibrato_depth_cents(phrase.style.vibrato);
    vib_hz_ = vibrato_rate_hz(phrase.style.vibrato_rate);
    detune_ = detune_semitones(phrase.style.detune);
    glide_ = portamento_seconds(phrase.style.portamento);
}

size_t PitchCurve::note_at(double t) const {
    if (starts_.empty()) return 0;
    auto it = std::upper_bound(starts_.begin(), starts_.end(), t);
    size_t i = it == starts_.begin() ? 0 : size_t(it - starts_.begin()) - 1;
    return std::min(i, starts_.size() - 1);
}

double PitchCurve::bend_at(double t) const {
    double v = 0.0;
    for (const BendEvent &b : bends_) {
        if (b.seconds > t) break;
        v = b.semitones;
    }
    return v;
}

double PitchCurve::steady_midi_at(double t) const {
    if (starts_.empty()) return 60.0;
    size_t i = note_at(t);
    double p = pitches_[i];
    // A glide arrives from the note before, over the start of this one.
    if (glide_ > 0.0 && i > 0) {
        double into = t - starts_[i];
        if (into < glide_) {
            double f = std::max(0.0, into) / glide_;
            f = f * f * (3.0 - 2.0 * f);
            p = pitches_[i - 1] + (p - pitches_[i - 1]) * f;
        }
    }
    return p + bend_at(t) + detune_;
}

double PitchCurve::vibrato_cents_at(double t) const {
    if (mods_.empty()) return vib_cents_;
    double m = 0.0;
    for (const BendEvent &e : mods_) {
        if (e.seconds > t) break;
        m = e.semitones;
    }
    // the same scale as vibrato_depth_cents, on the depth the wheel makes
    return 0.8 * effective_vibrato(vib_control_, m);
}

double PitchCurve::midi_at(double t) const {
    double p = steady_midi_at(t);
    double depth = vibrato_cents_at(t);
    if (depth > 0.0 && !starts_.empty()) {
        size_t i = note_at(t);
        double into = t - starts_[i];
        double fade = std::min(1.0, std::max(0.0, into / kVibratoOnset));
        p += fade * depth / 100.0 * std::sin(2.0 * kPi * vib_hz_ * t);
    }
    return p;
}

double PitchCurve::hz_at(double t) const { return midi_to_hz(midi_at(t)); }

std::vector<float> resample_to_output(const std::vector<float> &in, double rate) {
    if (in.empty() || std::fabs(rate - kSampleRate) < 1e-9) return in;
    double ratio = kSampleRate / rate;
    size_t n = size_t(std::floor(in.size() * ratio));
    std::vector<float> out(n);
    // A windowed sinc, its cutoff at the lower of the two Nyquists so that
    // going down does not alias and going up does not image.
    double cutoff = std::min(1.0, ratio);
    const int half = 16;
    auto weight = [&](double d) {
        double s = d == 0.0 ? 1.0 : std::sin(kPi * d * cutoff) / (kPi * d * cutoff);
        double w = 0.5 + 0.5 * std::cos(kPi * d / half);
        return s * w * cutoff;
    };
    // Going up by a whole number -- 11025 and 22050 to 44100, which is DECtalk and Microsoft --
    // every output sample sits at one of `up` phases between two input samples, so the filter
    // is worked out once per phase rather than once per sample. The same weights, in the same
    // order, so the same samples: only about thirty times sooner.
    double whole = std::round(ratio);
    if (ratio > 1.0 && std::fabs(ratio - whole) < 1e-12) {
        const size_t up = size_t(whole);
        std::vector<double> taps(up * 2 * half);
        for (size_t p = 0; p < up; ++p) {
            double x = double(p) / ratio;
            for (int t = 0; t < 2 * half; ++t) taps[p * 2 * half + size_t(t)] = weight(x - double(t - half + 1));
        }
        for (size_t k = 0; k < n; ++k) {
            size_t p = k % up;
            long centre = long(k / up);
            const double *c = &taps[p * 2 * half];
            double acc = 0.0;
            for (int t = 0; t < 2 * half; ++t) {
                long j = centre - half + 1 + t;
                if (j < 0 || j >= long(in.size())) continue;
                acc += in[size_t(j)] * c[t];
            }
            out[k] = float(acc);
        }
        return out;
    }
    for (size_t k = 0; k < n; ++k) {
        double x = k / ratio;
        long centre = long(std::floor(x));
        double acc = 0.0, wsum = 0.0;
        for (long j = centre - half + 1; j <= centre + half; ++j) {
            if (j < 0 || j >= long(in.size())) continue;
            double d = x - double(j);
            double s = d == 0.0 ? 1.0 : std::sin(kPi * d * cutoff) / (kPi * d * cutoff);
            double w = 0.5 + 0.5 * std::cos(kPi * d / half);
            double c = s * w * cutoff;
            acc += in[size_t(j)] * c;
            wsum += c;
        }
        (void)wsum;
        out[k] = float(acc);
    }
    return out;
}

}  // namespace svs
