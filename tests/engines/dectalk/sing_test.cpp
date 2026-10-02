// sing_test.cpp -- sings a few phrases with DECtalk's voices and checks what
// came out.
//
//   dectalk_sing_test [output folder]
//
// "daisy" is a tune to listen to, with VocalWriter's usual vibrato and a
// bend of two semitones up and back in its last note. "consonants" is there
// to be listened to as well. "pitch" holds an open vowel on notes from G2 to
// B4 with no vibrato, and the F0 of each is measured by autocorrelation
// against the note asked for. "timing" sings a vowel straight through on
// alternate notes a fifth apart, and finds, period by period, where the
// pitch crosses half way from one note to the next: the engine starts each
// note on the 6.44 ms frame nearest its time, with a first piece of vowel
// three frames long that glides to the new pitch, so the crossing belongs
// one and a half frames after that. "vibrato" holds one note and measures
// the depth and rate of the
// wobble. "long" is a phrase of over half a minute without a rest, which
// DECtalk has to be given in many pieces; "high" goes above DECtalk's top
// note and is sung an octave down. Every file is checked for its length (the
// phrase's own, from its pitch curve) and its peak.
//
// SVS_DECTALK_TRACE=1 in the environment shows how each phrase was cut up.
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

// F0 of x over [a, b), between lo and hi Hz, by normalised autocorrelation:
// the first lag whose correlation comes within 10% of the best, refined by a
// parabola through its neighbours.
double f0_between(const std::vector<float> &x, long a, long b, double lo, double hi) {
    const double sr = kSampleRate;
    long l0 = long(sr / hi), l1 = long(sr / lo) + 1;
    a = std::max(0L, a);
    b = std::min(long(x.size()), b);
    long n = b - a - l1 - 1;
    if (n <= 16) return 0.0;
    std::vector<double> r(size_t(l1 + 2), 0.0);
    double e0 = 0.0;
    for (long i = a; i < a + n; ++i) e0 += double(x[size_t(i)]) * x[size_t(i)];
    if (e0 <= 1e-9) return 0.0;
    for (long lag = l0 - 1; lag <= l1 + 1; ++lag) {
        double s = 0.0, e1 = 0.0;
        for (long i = a; i < a + n; ++i) {
            s += double(x[size_t(i)]) * x[size_t(i + lag)];
            e1 += double(x[size_t(i + lag)]) * x[size_t(i + lag)];
        }
        r[size_t(lag)] = s / std::sqrt(e0 * e1 + 1e-12);
    }
    double top = -1.0;
    for (long lag = l0; lag <= l1; ++lag) top = std::max(top, r[size_t(lag)]);
    if (top < 0.5) return 0.0;
    for (long lag = l0; lag <= l1; ++lag) {
        if (r[size_t(lag)] >= 0.9 * top && r[size_t(lag)] >= r[size_t(lag - 1)] &&
            r[size_t(lag)] >= r[size_t(lag + 1)]) {
            double y0 = r[size_t(lag - 1)], y1 = r[size_t(lag)], y2 = r[size_t(lag + 1)];
            double d = y0 - 2 * y1 + y2;
            double frac = d != 0 ? 0.5 * (y0 - y2) / d : 0.0;
            return sr / (lag + frac);
        }
    }
    return 0.0;
}

double cents(double f, double want) { return 1200.0 * std::log2(f / want); }

// A Butterworth low-pass at `hz`, two second-order sections run forwards and
// then backwards, so that it delays nothing.
std::vector<double> lowpass(const std::vector<float> &x, double hz) {
    std::vector<double> y(x.begin(), x.end());
    const double pi = 3.14159265358979323846;
    double w = std::tan(pi * hz / kSampleRate);
    const double qs[2] = {0.5411961, 1.3065630};
    for (double q : qs) {
        double n = 1.0 / (1.0 + w / q + w * w);
        double b0 = w * w * n, b1 = 2 * b0, b2 = b0;
        double a1 = 2.0 * (w * w - 1.0) * n, a2 = (1.0 - w / q + w * w) * n;
        for (int pass = 0; pass < 2; ++pass) {
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (size_t k = 0; k < y.size(); ++k) {
                size_t i = pass == 0 ? k : y.size() - 1 - k;
                double v = b0 * y[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1;
                x1 = y[i];
                y2 = y1;
                y1 = v;
                y[i] = v;
            }
        }
    }
    return y;
}

long at_ms(double ms) { return std::lround(ms * kSampleRate / 1000.0); }

}  // namespace

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : "build-tests/dectalk/out";
    make_dirs(out);

    // "Daisy, Daisy, give me your answer, do" in 3/4, the last note bent.
    Phrase daisy = make_phrase(150, {
        {67, 3, "d EY"}, {64, 3, "z IY"}, {60, 3, "d EY"}, {55, 3, "z IY"},
        {57, 1, "g IH v"}, {59, 1, "m IY"}, {60, 1, "y UR"},
        {57, 2, "AE n"}, {60, 1, "s ER"}, {55, 6, "d UW"},
    }, 31);
    {
        // up two semitones over a fifth of a second, a moment into the last
        // note, and back down near its end; filled in at 5 ms as the
        // renderer does
        PitchCurve c(daisy);
        double t0 = c.note_start(9) + 0.6, t1 = c.note_end(9) - 0.7;
        for (double t = t0; t <= t0 + 0.2 + 1e-9; t += 0.005)
            daisy.bends.push_back({t, 2.0 * (t - t0) / 0.2});
        for (double t = t1; t <= t1 + 0.2 + 1e-9; t += 0.005)
            daisy.bends.push_back({t, 2.0 - 2.0 * (t - t1) / 0.2});
    }
    Phrase consonants = make_phrase(120, {
        {57, 1, "p AA"}, {59, 1, "t AA"}, {60, 1, "k AA"}, {62, 1, "b AA"},
        {64, 1, "d AA"}, {62, 1, "g AA"}, {60, 1, "CH AA"}, {59, 1, "JH AA"},
        {57, 1, "s IY"}, {59, 1, "SH IY"}, {60, 1, "f IY"}, {62, 1, "TH IY"},
        {64, 1, "m AY"}, {62, 1, "n OW"}, {60, 1, "l AW"}, {59, 1, "r OY"},
        {57, 2, "w AA r m"},
    }, 31);
    // "Twinkle, twinkle, little star, how I wonder what you are": clusters,
    // syllabic l, diphthongs, and an r after a vowel at the end.
    Phrase twinkle = make_phrase(100, {
        {60, 1, "t w IH NG"}, {60, 1, "k EL"}, {67, 1, "t w IH NG"}, {67, 1, "k EL"},
        {69, 1, "l IH"}, {69, 1, "DD EL"}, {67, 2, "s t AR"},
        {65, 1, "h AW"}, {65, 1, "AY"}, {64, 1, "w UX n"}, {64, 1, "d ER"},
        {62, 1, "w AX t"}, {62, 1, "y UW"}, {60, 2, "AA r"},
    }, 31);
    // sixteenths at 140 with consonants crowding the vowels
    Phrase fast = make_phrase(140, {
        {62, 0.25, "s t r IY t s"}, {64, 0.25, "AH n d"}, {65, 0.25, "s k w EH r z"},
        {67, 0.25, "AE n d"}, {69, 0.25, "s p l AE SH"}, {67, 0.25, "IH NG k s"},
        {65, 0.25, "f l AY t s"}, {64, 1.0, "s t OW n z"},
    }, 31);
    Phrase pitch = make_phrase(60, {
        {43, 1, "AA"}, {48, 1, "AA"}, {52, 1, "AA"}, {55, 1, "AA"},
        {57, 1, "AA"}, {62, 1, "AA"}, {64, 1, "AA"}, {67, 1, "AA"},
        {69, 1, "AA"}, {71, 1, "AA"},
    }, 0);
    Phrase timing = make_phrase(120, {
        {57, 0.5, "AA"}, {64, 0.5, "AA"}, {57, 0.5, "AA"}, {64, 0.5, "AA"},
        {57, 0.5, "AA"}, {64, 0.5, "AA"}, {57, 0.5, "AA"}, {64, 1.0, "AA"},
    }, 0);
    Phrase vibrato = make_phrase(60, {{57, 4, "AA"}}, 31);
    std::vector<Syl> long_syls;
    const int tune[8] = {60, 62, 64, 65, 67, 65, 64, 62};
    for (int k = 0; k < 64; ++k) long_syls.push_back({tune[k % 8], 1, k % 4 == 3 ? "l AA" : "AA"});
    Phrase longp = make_phrase(110, long_syls, 31);
    Phrase high = make_phrase(100, {
        {76, 1, "l AA"}, {79, 1, "l AA"}, {84, 1, "l AA"}, {81, 2, "l AA"},
    }, 31);

    std::unique_ptr<Family> fam = make_dectalk_family();
    std::string why;
    bool have = fam->available(&why);
    std::printf("%s: %s\n", fam->name().c_str(), have ? "available" : why.c_str());
    for (const VoiceEntry &v : fam->voices()) std::printf("  %d %s\n", v.id, v.name.c_str());
    if (!have) return 1;

    int failures = 0;
    struct Job {
        const char *name;
        const Phrase *phrase;
        std::vector<int> voices;
    } jobs[] = {
        {"daisy", &daisy, {0, 1, 2, 5, 8}},
        {"consonants", &consonants, {0, 1}},
        {"twinkle", &twinkle, {0, 6, 7}},
        {"fast", &fast, {0, 3}},
        {"pitch", &pitch, {0, 1}},
        {"timing", &timing, {0, 1}},
        {"vibrato", &vibrato, {0}},
        {"long", &longp, {0}},
        {"high", &high, {1}},
    };
    for (const Job &job : jobs) {
        PitchCurve curve(*job.phrase);
        for (int vi : job.voices) {
            std::string err;
            int id = kDectalkBase + vi;
            auto singer = fam->singer(id, &err);
            if (!singer) {
                std::printf("%d: %s\n", id, err.c_str());
                ++failures;
                continue;
            }
            std::string vname = fam->voices()[size_t(vi)].name.substr(8);
            std::string tag = vname;
            for (char &c : tag) c = char(std::tolower((unsigned char)c));
            Sung s = singer->sing(*job.phrase);
            double peak = 0;
            for (float f : s.samples) peak = std::max(peak, double(std::fabs(f)));
            long want = long(std::llround(curve.total() * kSampleRate));
            std::string base = join_path(out, "dectalk_" + tag + "_" + job.name);
            write_wav(base + ".wav", s.samples, 1);
            // the pitch asked for, every 5 ms, and where the notes start,
            // for looking at beside the sound
            if (FILE *f = std::fopen((base + ".curve.txt").c_str(), "w")) {
                for (double t = 0; t < curve.total(); t += 0.005)
                    std::fprintf(f, "%.3f %.3f\n", t, curve.hz_at(t));
                std::fclose(f);
            }
            if (FILE *f = std::fopen((base + ".notes.txt").c_str(), "w")) {
                for (size_t i = 0; i < curve.notes(); ++i)
                    std::fprintf(f, "%.4f %.4f %d\n", curve.note_start(i), curve.note_end(i),
                                 job.phrase->notes[i].midi);
                std::fclose(f);
            }
            bool ok = long(s.samples.size()) == want && peak > 0.05 && peak < 0.99 && !s.stopped_short;
            if (!ok) ++failures;
            std::printf("%-7s %-10s %7.3f s (curve %7.3f s)  peak %.3f  %s%s\n", vname.c_str(),
                        job.name, s.samples.size() / double(kSampleRate), curve.total(), peak,
                        ok ? "ok" : "FAIL", s.stopped_short ? " (stopped short)" : "");
            const std::vector<float> &x = s.samples;

            if (job.phrase == &pitch) {
                double worst = 0;
                std::printf("    note   want Hz    got Hz   cents\n");
                for (size_t i = 0; i + 1 < job.phrase->notes.size(); ++i) {
                    double want_hz = PitchCurve::midi_to_hz(job.phrase->notes[i].midi);
                    double a = curve.note_start(i), b = curve.note_end(i);
                    double f = f0_between(x, at_ms(1000 * (a + 0.3 * (b - a))),
                                          at_ms(1000 * (a + 0.8 * (b - a))), want_hz / 1.3, want_hz * 1.3);
                    double c = f > 0 ? cents(f, want_hz) : 999;
                    worst = std::max(worst, std::fabs(c));
                    std::printf("    %4d  %8.2f  %8.2f  %+6.1f\n", job.phrase->notes[i].midi, want_hz, f, c);
                }
                std::printf("    worst %.1f cents\n", worst);
                if (worst > 15) ++failures;
            }

            if (job.phrase == &timing) {
                // The pitch period by period: the sound low-passed below its
                // second harmonic (both ways, so nothing is delayed), and
                // the time between rising zero crossings. Each period's
                // pitch is put at its middle.
                std::vector<double> y = lowpass(x, 300.0);
                std::vector<double> zc;
                for (size_t k = 1; k < y.size(); ++k)
                    if (y[k - 1] < 0 && y[k] >= 0) zc.push_back(k - 1 + -y[k - 1] / (y[k] - y[k - 1]));
                double worst = 0;
                std::printf("    note  crossing after start (ms)  expected  error (ms)\n");
                for (size_t i = 1; i + 1 < job.phrase->notes.size(); ++i) {
                    double fa = PitchCurve::midi_to_hz(job.phrase->notes[i - 1].midi);
                    double fb = PitchCurve::midi_to_hz(job.phrase->notes[i].midi);
                    double mid = std::sqrt(fa * fb);
                    double st = curve.note_start(i);
                    double found = 999;
                    double pt = -1, pf = 0;
                    for (size_t k = 1; k < zc.size(); ++k) {
                        double t = 0.5 * (zc[k] + zc[k - 1]) / kSampleRate;
                        double f = kSampleRate / (zc[k] - zc[k - 1]);
                        if (t < st - 0.02) continue;
                        if (t > st + 0.06) break;
                        if (pt >= 0 && (pf - mid) * (f - mid) <= 0) {
                            found = 1000 * (pt + (mid - pf) / (f - pf) * (t - pt) - st);
                            break;
                        }
                        pt = t;
                        pf = f;
                    }
                    // The note starts on the frame nearest its time, and the
                    // first 3-frame piece of the new note crosses half way
                    // at its middle.
                    const double frame = 71.0 / 11025.0;
                    double expect = 1000 * (std::round(st / frame) * frame - st + 1.5 * frame);
                    double e = found - expect;
                    worst = std::max(worst, std::fabs(e));
                    std::printf("    %4zu  %8.1f                    %6.1f    %+6.1f\n", i, found, expect, e);
                }
                std::printf("    worst %.1f ms\n", worst);
                if (worst > 7) ++failures;
            }

            if (job.phrase == &vibrato) {
                // F0 every 10 ms over the held part, then its swing and how
                // often it crosses its own mean
                std::vector<double> f;
                for (double m = 600; m < 3800; m += 10) {
                    double v = f0_between(x, at_ms(m - 10), at_ms(m + 10), 180, 260);
                    if (v > 0) f.push_back(cents(v, 220.0));
                }
                double mean = 0;
                for (double v : f) mean += v;
                mean /= std::max<size_t>(1, f.size());
                double lo = 1e9, hi = -1e9;
                int crossings = 0;
                for (size_t k = 0; k < f.size(); ++k) {
                    lo = std::min(lo, f[k]);
                    hi = std::max(hi, f[k]);
                    if (k > 0 && (f[k - 1] - mean) * (f[k] - mean) < 0) ++crossings;
                }
                double rate = crossings / 2.0 / (f.size() * 0.01);
                std::printf("    vibrato: centre %+.1f cents, swing %.1f cents peak to peak, %.2f Hz "
                            "(asked: +-%.1f cents, %.2f Hz)\n",
                            mean, hi - lo, rate, curve.vibrato_cents(), curve.vibrato_hz());
            }
        }
    }
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
