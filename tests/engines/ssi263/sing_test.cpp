// sing_test.cpp -- sings four phrases with every SSI-263 voice and checks
// what came out.
//
//   ssi263_sing_test <output folder>
//
// "daisy" is a tune to listen to, and "stops" the consonants the chip times
// itself. "pitch" holds an open vowel on notes from G2 to A5 with no vibrato,
// and the F0 of each is measured by autocorrelation against the note asked
// for. "timing" starts every note with an s after a held vowel; the vowel
// dies away the moment the chip is given the s, so each note's start can be
// found from the sound. Every file is checked for its length (the phrase's
// own, from its pitch curve) and its peak.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

using namespace svs;

namespace {

struct Syl {
    int midi;
    double beats;
    const char *phonemes;
};

std::vector<std::string> split(const char *s) {
    std::vector<std::string> out;
    std::string cur;
    for (; *s; ++s) {
        if (*s == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += *s;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

Phrase make_phrase(double bpm, const std::vector<Syl> &syls, int vibrato) {
    Phrase p;
    p.bpm = bpm;
    p.style.vibrato = vibrato;
    for (const Syl &s : syls) {
        SungNote n;
        n.midi = s.midi;
        n.beats = s.beats;
        n.velocity = 100;
        n.phonemes = split(s.phonemes);
        n.durations_ms = syllable_lengths(n.phonemes, s.beats * 60000.0 / bpm);
        p.notes.push_back(n);
    }
    // the closing rest every phrase ends with
    SungNote rest;
    rest.midi = syls.back().midi;
    rest.beats = 0.4;
    rest.velocity = 1;
    rest.phonemes = {kRest};
    rest.durations_ms = {0.4 * 60000.0 / bpm};
    p.notes.push_back(rest);
    return p;
}

// F0 of x[a, b), near `guess`, by autocorrelation. The peak looked for is
// not the first period but the one nearest 20 ms, a whole number of periods
// on: a lag of 20 ms is found as precisely as one of 1 ms, so the estimate is
// that many times finer than one period's would be.
double estimate_f0(const std::vector<float> &x, long a, long b, double guess) {
    const double sr = kSampleRate;
    double period = sr / guess;
    int k = std::max(1, int(std::lround(0.02 * sr / period)));
    long centre = std::lround(k * period), span = std::lround(0.3 * period);
    long n = b - a - centre - span - 2;
    if (n <= 0) return 0.0;
    auto r = [&](long lag) {
        double s = 0;
        for (long i = a; i < a + n; ++i) s += double(x[size_t(i)]) * x[size_t(i + lag)];
        return s;
    };
    long best = centre;
    double top = -1e300;
    for (long lag = centre - span; lag <= centre + span; ++lag) {
        double v = r(lag);
        if (v > top) {
            top = v;
            best = lag;
        }
    }
    double y0 = r(best - 1), y1 = top, y2 = r(best + 1);
    double d = y0 - 2 * y1 + y2;
    double frac = d != 0 ? 0.5 * (y0 - y2) / d : 0.0;
    return sr * k / (best + frac);
}

// RMS of x over the millisecond starting at `ms`.
double rms_ms(const std::vector<float> &x, double ms) {
    long a = std::lround(ms * kSampleRate / 1000.0);
    long b = std::lround((ms + 1) * kSampleRate / 1000.0);
    a = std::max(0L, a);
    b = std::min(b, long(x.size()));
    double s = 0;
    for (long i = a; i < b; ++i) s += double(x[size_t(i)]) * x[size_t(i)];
    return b > a ? std::sqrt(s / double(b - a)) : 0.0;
}

// Where a note starts, from the sound, in ms after where it should. The
// first comes out of silence: the first millisecond with anything in it. The
// others go from a held vowel to an s, which is far quieter: the middle of
// the first five milliseconds, from 20 ms before the note, whose level is
// below half the vowel's before it (five, so that the vowel's own waver is
// not taken for its end).
double onset_ms(const std::vector<float> &x, double start_ms, bool first) {
    if (first) {
        for (double m = start_ms; m < start_ms + 60; m += 1)
            if (rms_ms(x, m) > 0.003) return m - start_ms;
        return 999;
    }
    double ref = 0;
    for (double m = start_ms - 45; m < start_ms - 5; m += 1) ref += rms_ms(x, m);
    ref /= 40;
    for (double m = start_ms - 20; m < start_ms + 60; m += 1) {
        double v = 0;
        for (int j = 0; j < 5; ++j) v += rms_ms(x, m + j);
        if (v / 5 < 0.5 * ref) return m + 2.5 - start_ms;
    }
    return 999;
}

}  // namespace

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : ".";
    make_dirs(out);

    // "Daisy, Daisy, give me your answer, do" in 3/4.
    Phrase daisy = make_phrase(150, {
        {67, 3, "d EY"}, {64, 3, "z IY"}, {60, 3, "d EY"}, {55, 3, "z IY"},
        {57, 1, "g IH v"}, {59, 1, "m IY"}, {60, 1, "y UR"},
        {57, 2, "AE n"}, {60, 1, "s ER"}, {55, 5, "d UW"},
    }, 31);
    Phrase stops = make_phrase(120, {
        {57, 1, "p AA"}, {59, 1, "t AA"}, {60, 1, "k AA"}, {62, 1, "b AA"},
        {64, 1, "d AA"}, {62, 1, "g AA"}, {60, 1, "CH AA"}, {59, 1, "JH AA"},
        {57, 2, "AA t"},
    }, 31);
    Phrase pitch = make_phrase(60, {
        {43, 1, "AA"}, {50, 1, "AA"}, {57, 1, "AA"}, {64, 1, "AA"},
        {69, 1, "AA"}, {76, 1, "AA"}, {81, 1, "AA"},
    }, 0);
    Phrase timing = make_phrase(120, {
        {60, 0.5, "s AA"}, {62, 0.5, "s AA"}, {64, 0.5, "s IY"}, {65, 0.5, "s AA"},
        {67, 0.5, "s AA"}, {65, 0.5, "s IY"}, {64, 0.5, "s AA"}, {62, 1.0, "s AA"},
    }, 31);

    std::unique_ptr<Family> fam = make_ssi263_family();
    std::string why;
    std::printf("%s: %s\n", fam->name().c_str(),
                fam->available(&why) ? "available" : why.c_str());
    int failures = 0;
    for (const VoiceEntry &v : fam->voices()) {
        std::string err;
        auto singer = fam->singer(v.id, &err);
        if (!singer) {
            std::printf("%d %s: %s\n", v.id, v.name.c_str(), err.c_str());
            ++failures;
            continue;
        }
        std::string tag = v.name == "SSI-263" ? "plain" : v.name.substr(8);
        for (char &c : tag) c = char(std::tolower((unsigned char)c));
        struct Job {
            const char *name;
            const Phrase *phrase;
        } jobs[] = {{"daisy", &daisy}, {"stops", &stops}, {"pitch", &pitch}, {"timing", &timing}};
        for (const Job &job : jobs) {
            PitchCurve curve(*job.phrase);
            Sung s = singer->sing(*job.phrase);
            double peak = 0;
            for (float f : s.samples) peak = std::max(peak, double(std::fabs(f)));
            long want = long(std::llround(curve.total() * kSampleRate));
            std::string path = join_path(out, "ssi263_" + tag + "_" + job.name + ".wav");
            write_wav(path, s.samples, 1);
            bool ok = long(s.samples.size()) == want && peak > 0.05 && peak < 0.9;
            if (!ok) ++failures;
            std::printf("%-15s %-6s %6.3f s (curve %6.3f s)  peak %.3f  %s%s\n",
                        v.name.c_str(), job.name, s.samples.size() / double(kSampleRate),
                        curve.total(), peak, ok ? "ok" : "FAIL",
                        s.stopped_short ? " stopped short" : "");
            if (job.phrase == &pitch) {
                // the middle half of each held note
                for (size_t i = 0; i + 1 < curve.notes(); ++i) {
                    double a = curve.note_start(i), b = curve.note_end(i), m = b - a;
                    long sa = long((a + 0.3 * m) * kSampleRate);
                    long sb = long((b - 0.2 * m) * kSampleRate);
                    double want_hz = PitchCurve::midi_to_hz(job.phrase->notes[i].midi);
                    double f0 = estimate_f0(s.samples, sa, sb, want_hz);
                    double cents = f0 > 0 ? 1200.0 * std::log2(f0 / want_hz) : 9999;
                    bool good = std::fabs(cents) <= 10.0;
                    if (!good) ++failures;
                    std::printf("    note %2d  want %7.2f Hz  got %7.2f Hz  %+5.1f cents  %s\n",
                                job.phrase->notes[i].midi, want_hz, f0, cents,
                                good ? "ok" : "FAIL");
                }
            }
            if (job.phrase == &timing) {
                // The level is the chip's own: a vowel takes a few
                // milliseconds to die away once the s is written, so up to
                // 8 ms after the note passes.
                std::printf("    onsets (ms after the note):");
                double worst = 0;
                for (size_t i = 0; i + 1 < curve.notes(); ++i) {
                    double late = onset_ms(s.samples, curve.note_start(i) * 1000.0, i == 0);
                    worst = std::max(worst, std::fabs(late));
                    std::printf(" %+.1f", late);
                }
                bool good = worst <= 8.0;
                if (!good) ++failures;
                std::printf("  %s\n", good ? "ok" : "FAIL");
            }
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
