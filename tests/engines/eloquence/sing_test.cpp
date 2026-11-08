// sing_test.cpp -- the Eloquence engine on its own.
//
//   eloquence_sing_test [--out DIR] [--levels]
//
// Lists the voices, checks the phoneme table, and sings "Twinkle, twinkle"
// with every voice to WAV files in --out DIR (default: the current folder),
// checking that each note is sung on its pitch and that a phrase sings the
// same twice. --levels sings ten vowels on notes across each voice's range
// and prints each voice's median peak, which is what the voices' gains in
// src/voices/eloquence.cpp are set from.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "core/paths.h"
#include "voices/family.h"
#include "voices/pitch.h"

namespace svs {
std::string eloquence_self_check();
}  // namespace svs

using namespace svs;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

SungNote note(int midi, double beats, std::vector<std::string> ph, std::vector<double> ms) {
    SungNote n;
    n.midi = midi;
    n.beats = beats;
    n.velocity = 100;
    n.phonemes = std::move(ph);
    n.durations_ms = std::move(ms);
    return n;
}

// The lengths are the ones the program would give these syllables at 100 bpm.
Phrase twinkle() {
    Phrase p;
    p.bpm = 100;
    p.notes = {
        note(60, 1, {"t", "w", "IH", "NG"}, {60, 50, 430, 60}), note(60, 1, {"k", "EL"}, {70, 530}),
        note(67, 1, {"t", "w", "IH", "NG"}, {60, 50, 430, 60}), note(67, 1, {"k", "EL"}, {70, 530}),
        note(69, 1, {"l", "IH"}, {60, 540}),                    note(69, 1, {"DD", "EL"}, {30, 570}),
        note(67, 2, {"s", "t", "AR"}, {90, 50, 1060}),
    };
    SungNote rest = note(67, 0.4, {"%"}, {240});
    rest.velocity = 1;
    p.notes.push_back(rest);
    p.style.vibrato = 0;
    p.level = 0.8;
    return p;
}

// The pitch of `x` around `t` seconds, in hertz, by de Cheveigne and
// Kawahara's YIN: the first lag whose normalised difference falls under 0.15,
// or the lowest if none does, checked against the spectrum; 0 for silence.
double pitch_once(const std::vector<float> &x, double t, double win = 0.04) {
    if (t < 0) return 0;
    size_t a = size_t(t * kSampleRate), n = size_t(win * kSampleRate);
    int lo = kSampleRate / 1200, hi = kSampleRate / 50;
    if (a + n + size_t(hi) + 1 > x.size()) return 0;
    double energy = 0;
    for (size_t i = 0; i < n; ++i) energy += double(x[a + i]) * x[a + i];
    if (energy < n * 1e-7) return 0;
    std::vector<double> d(size_t(hi) + 2, 1.0);
    double sum = 0;
    for (int lag = 1; lag <= hi + 1; ++lag) {
        double s = 0;
        for (size_t i = 0; i < n; ++i) {
            double e = double(x[a + i]) - x[a + i + size_t(lag)];
            s += e * e;
        }
        sum += s;
        d[size_t(lag)] = sum > 0 ? s * lag / sum : 1.0;
    }
    int k = -1;
    for (int lag = lo; lag <= hi; ++lag)
        if (d[size_t(lag)] < 0.15) {
            while (lag < hi && d[size_t(lag) + 1] < d[size_t(lag)]) ++lag;
            k = lag;
            break;
        }
    if (k < 0) k = int(std::min_element(d.begin() + lo, d.begin() + hi) - d.begin());
    double y0 = d[size_t(k) - 1], y1 = d[size_t(k)], y2 = d[size_t(k) + 1], den = y0 - 2 * y1 + y2;
    double off = den != 0 ? 0.5 * (y0 - y2) / den : 0;
    double f = kSampleRate / (k + off);
    // A lag that is a multiple of the period, or half of it, can repeat
    // nearly as well; the spectrum says which is the pitch: the first of its
    // multiples within 25 dB of the loudest of them (a voice's first
    // harmonic can be 20 dB under the formant it feeds), and nothing under it
    // that comes within 25 dB of it.
    auto level = [&](double hz) {
        double re = 0, im = 0;
        for (size_t i = 0; i < n; ++i) {
            double w = 0.5 - 0.5 * std::cos(2 * 3.14159265358979 * double(i) / double(n));
            re += x[a + i] * w * std::cos(2 * 3.14159265358979 * hz * double(i) / kSampleRate);
            im -= x[a + i] * w * std::sin(2 * 3.14159265358979 * hz * double(i) / kSampleRate);
        }
        return 10 * std::log10(re * re + im * im + 1e-30);
    };
    double loudest = -1e9;
    for (int m = 1; m <= 6; ++m) loudest = std::max(loudest, level(m * f));
    for (int m = 1; m <= 6; ++m)
        if (level(m * f) >= loudest - 25) {
            f *= m;
            break;
        }
    if (f / 2 > 50 && level(f / 2) > level(f) - 25) return f / 2;
    return f;
}

//: the median of seven measures `spread` apart around `t`, so that a rough
//: period or two does not decide it
double pitch_at(const std::vector<float> &x, double t, double spread = 0.02) {
    std::vector<double> m;
    for (int k = -3; k <= 3; ++k) m.push_back(pitch_once(x, t + k * spread - 0.02));
    std::sort(m.begin(), m.end());
    return m[3];
}

void levels(Family &fam) {
    const char *vowels[] = {"IY", "IH", "EH", "AE", "AA", "UX", "AO", "UH", "OW", "UW"};
    for (const VoiceEntry &v : fam.voices()) {
        std::string err;
        auto s = fam.singer(v.id, &err);
        std::vector<double> peaks;
        for (int midi = 48; midi <= 72; midi += 6)
            for (const char *vowel : vowels) {
                Phrase p;
                p.bpm = 120;
                p.notes = {note(midi, 2, {"m", vowel}, {80, 920})};
                SungNote rest = note(midi, 0.4, {"%"}, {200});
                rest.velocity = 1;
                p.notes.push_back(rest);
                // a quarter of the level, so that no phrase is brought down
                p.level = 0.25;
                Sung sung = s->sing(p);
                float peak = 0;
                for (float x : sung.samples) peak = std::max(peak, std::fabs(x));
                peaks.push_back(peak * 4);
            }
        std::sort(peaks.begin(), peaks.end());
        std::printf("  %-20s median peak %.3f, loudest %.3f\n", v.name.c_str(), peaks[peaks.size() / 2],
                    peaks.back());
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::string out = ".";
    bool want_levels = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--levels")) want_levels = true;
        else {
            std::fprintf(stderr, "usage: eloquence_sing_test [--out DIR] [--levels]\n");
            return 2;
        }
    }
    std::unique_ptr<Family> fam = make_eloquence_family();
    std::printf("%s voices:\n", fam->name().c_str());
    auto voices = fam->voices();
    std::set<int> ids;
    for (const VoiceEntry &v : voices) {
        std::printf("  %d  %s\n", v.id, v.name.c_str());
        ids.insert(v.id);
    }
    check(voices.size() == 8 && ids.size() == 8, "8 voices, every number different");
    check(voices.front().id == kEloquenceBase && voices.front().name == "Eloquence Reed", "5000 is Eloquence Reed");

    std::string bad = eloquence_self_check();
    check(bad.empty(), "phoneme table: all 57 VocalWriter phonemes map to Eloquence's phonemes" +
                           (bad.empty() ? std::string() : " -- " + bad));
    std::string why, err;
    check(fam->available(&why), "Eloquence is compiled in and needs no data");
    check(!fam->singer(kEloquenceBase + 999, &err) && !err.empty(), "an unknown voice number is refused: " + err);
    if (want_levels) {
        levels(*fam);
        return failures ? 1 : 0;
    }

    make_dirs(out);
    Phrase p = twinkle();
    PitchCurve curve(p);
    for (const VoiceEntry &v : voices) {
        err.clear();
        auto s = fam->singer(v.id, &err);
        if (!s) {
            check(false, v.name + ": " + err);
            continue;
        }
        Sung sung = s->sing(p);
        check(s->sing(p).samples == sung.samples, v.name + " sings the same twice");
        float peak = 0;
        for (float x : sung.samples) peak = std::max(peak, std::fabs(x));
        double secs = sung.samples.size() / double(kSampleRate);
        std::string file = join_path(out, "eloquence_" + std::to_string(v.id) + ".wav");
        write_wav(file, sung.samples, 1, kSampleRate);
        check(!sung.stopped_short && secs >= curve.total() + sung.starts_before && peak <= 1.0f && peak > 0.05f,
              v.name + ": " + std::to_string(secs) + " s for " + std::to_string(curve.total()) +
                  " s of notes, peak " + std::to_string(peak) + " -> " + file);
        // The middle of each note's vowel, on its pitch to within 10 cents.
        double worst = 0;
        for (size_t i = 0; i + 1 < p.notes.size(); ++i) {
            // the notes sung on a full vowel: a syllabic l is too quiet in
            // some voices to measure
            if (p.notes[i].phonemes.back() == "EL") continue;
            double mid = (curve.note_start(i) + curve.note_end(i)) / 2 + sung.starts_before;
            double hz = pitch_at(sung.samples, mid);
            double cents = hz > 0 ? 1200 * std::log2(hz / PitchCurve::midi_to_hz(p.notes[i].midi)) : 9999;
            worst = std::max(worst, std::fabs(cents));
        }
        check(worst < 10, v.name + ": every note on its pitch (worst " + std::to_string(worst) + " cents)");
    }
    // A long phrase keeps its time: three hundred short notes up and down a
    // fifth, and the last of them still on its own pitch where it is due.
    Phrase run;
    run.bpm = 120;
    for (int i = 0; i < 300; ++i) run.notes.push_back(note(i % 2 ? 67 : 60, 0.25, {"l", "AA"}, {40, 85}));
    SungNote rest = note(60, 0.4, {"%"}, {200});
    rest.velocity = 1;
    run.notes.push_back(rest);
    run.style.vibrato = 0;
    PitchCurve run_curve(run);
    auto first = fam->singer(kEloquenceBase, &err);
    Sung long_sung = first->sing(run);
    write_wav(join_path(out, "eloquence_long.wav"), long_sung.samples, 1, kSampleRate);
    double worst = 0;
    for (size_t i = 290; i < 300; ++i) {
        double mid = (run_curve.note_start(i) + run_curve.note_end(i)) / 2 + 0.02 + long_sung.starts_before;
        double hz = pitch_at(long_sung.samples, mid, 0.006);
        double cents = hz > 0 ? 1200 * std::log2(hz / PitchCurve::midi_to_hz(run.notes[i].midi)) : 9999;
        worst = std::max(worst, std::fabs(cents));
    }
    check(worst < 15, "the last notes of a 37-second phrase are on their pitches where they are due (worst " +
                          std::to_string(worst) + " cents)");
    // A bend inside a held vowel is followed: two seconds of C4 bent up a
    // whole tone over its second second.
    Phrase bend;
    bend.bpm = 60;
    bend.notes = {note(60, 2, {"m", "AA"}, {80, 1920})};
    SungNote last = note(60, 0.4, {"%"}, {400});
    last.velocity = 1;
    bend.notes.push_back(last);
    bend.style.vibrato = 0;
    for (int k = 0; k <= 200; ++k) bend.bends.push_back({1.0 + k * 0.005, 2.0 * k / 200});
    Sung bent = first->sing(bend);
    double before = pitch_at(bent.samples, 0.8 + bent.starts_before);
    double after = pitch_at(bent.samples, 1.95 + bent.starts_before);
    double rise = 1200 * std::log2(after / before);
    check(std::fabs(rise - 200) < 30, "a bend up a whole tone inside a held vowel rises " + std::to_string(rise) +
                                          " cents");
    std::printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
