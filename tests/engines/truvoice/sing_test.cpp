// sing_test.cpp -- TruVoice on its own.
//
//   truvoice_sing_test [--out DIR] [--levels] [--creak]
//
// Lists the voices, checks the phoneme table, and sings "Twinkle, twinkle"
// with every voice to WAV files in --out DIR (default: the current folder),
// checking that each note is sung on its pitch and that a phrase sings the
// same twice. --levels sings ten vowels on notes across each voice's range
// and prints each voice's median peak, which is what the voices' gains in
// src/voices/truvoice.cpp are set from.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include <tvtts.h>

#include "audio/wav.h"
#include "core/paths.h"
#include "voices/family.h"
#include "voices/pitch.h"

namespace svs {
std::string truvoice_self_check();
void truvoice_set_retune(int mode);
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
// From 1200 Hz down to `lowest`, which needs a window `win` of two of its
// periods or more.
double pitch_once(const std::vector<float> &x, double t, double win = 0.04, double lowest = 50) {
    if (t < 0) return 0;
    size_t a = size_t(t * kSampleRate), n = size_t(win * kSampleRate);
    int lo = kSampleRate / 1200, hi = int(kSampleRate / lowest);
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
    // Half of it is the pitch if half sounds, or if half's own third harmonic
    // does, between the two, as a harmonic rather than as noise: a peak, 10 dB
    // over the gaps either side of it, where half's series has nothing. Eager
    // Eddie and Melvin sing the "ar" of "star" with its first harmonic 26 dB
    // under the second and its third only 16 dB under; a breathy voice's noise
    // reaches as high but makes no peak there.
    if (f / 2 > lowest) {
        if (level(f / 2) > level(f) - 25) return f / 2;
        double third = level(1.5 * f);
        if (third > level(f) - 25 && third > std::max(level(1.25 * f), level(1.75 * f)) + 10) return f / 2;
    }
    return f;
}

//: the median of seven measures `spread` apart around `t`, so that a rough
//: period or two does not decide it
double pitch_at(const std::vector<float> &x, double t, double spread = 0.02, double win = 0.04,
                double lowest = 50) {
    std::vector<double> m;
    for (int k = -3; k <= 3; ++k) m.push_back(pitch_once(x, t + k * spread - win / 2, win, lowest));
    std::sort(m.begin(), m.end());
    return m[3];
}

// The pitch at `t` seconds as ten periods of it measure, about `hz`: the lag
// within half a period of ten where 60 ms of `x` best matches itself, and how
// well, in `match`. Periods of whole samples that a fraction makes alternate
// -- D6's 9.385 is 9, 9, 10 -- and a tracker that takes the first period
// that repeats hears the 9s; ten of them are their average. 0 for silence.
double ten_period_pitch(const std::vector<float> &x, double t, double hz, double *match) {
    double want = 10.0 * kSampleRate / hz;
    long lo = long(want - 0.5 * want / 10), hi = long(want + 0.5 * want / 10) + 1;
    size_t a = size_t(std::max(0.0, t - 0.03) * kSampleRate), n = size_t(0.06 * kSampleRate);
    *match = 0;
    if (a + n + size_t(hi) + 2 > x.size()) return 0;
    std::vector<double> r(size_t(hi) + 2, 0.0);
    for (long lag = lo - 1; lag <= hi + 1; ++lag) {
        double xy = 0, xx = 0, yy = 0;
        for (size_t i = 0; i < n; ++i) {
            double p = x[a + i], q = x[a + i + size_t(lag)];
            xy += p * q;
            xx += p * p;
            yy += q * q;
        }
        r[size_t(lag)] = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0;
    }
    long best = lo;
    for (long lag = lo; lag <= hi; ++lag)
        if (r[size_t(lag)] > r[size_t(best)]) best = lag;
    double y0 = r[size_t(best - 1)], y1 = r[size_t(best)], y2 = r[size_t(best + 1)], den = y0 - 2 * y1 + y2;
    double off = den != 0 ? 0.5 * (y0 - y2) / den : 0;
    *match = y1;
    return y1 > 0 ? 10.0 * kSampleRate / (double(best) + off) : 0;
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

// How periodic a stretch of voicing is: for windows of 40 ms, every 20 ms from
// `from` to `to` seconds, the best normalised match between a window and the
// one a period on, over 0.7 to 1.3 of the period expected, averaged. A voice
// that never wavers is near 1; jitter and shimmer, which are what creak is,
// take it down (docs/VOICES.md in OpenTV measures a voice's creak this way).
double periodicity(const std::vector<float> &x, double rate, double hz, double from, double to) {
    size_t lo = size_t(0.7 * rate / hz), hi = size_t(1.3 * rate / hz) + 1, win = size_t(0.04 * rate);
    size_t end = std::min(x.size(), size_t(to * rate));
    double sum = 0;
    int n = 0;
    for (size_t a = size_t(from * rate); a + win + hi < end; a += win / 2) {
        double best = -1;
        for (size_t lag = lo; lag <= hi; ++lag) {
            double xy = 0, xx = 0, yy = 0;
            for (size_t k = 0; k < win; ++k) {
                double u = x[a + k], v = x[a + k + lag];
                xy += u * v;
                xx += u * u;
                yy += v * v;
            }
            if (xx > 0 && yy > 0) best = std::max(best, xy / std::sqrt(xx * yy));
        }
        sum += best;
        ++n;
    }
    return n ? sum / n : 0;
}

std::vector<float> rendered(tvtts_synth *s, const std::vector<uint8_t> &frames, const uint16_t *periods) {
    std::vector<float> pcm;
    tvtts_speak_frames_periods(
        s, frames.data(), periods, uint32_t(frames.size() / TVTTS_FRAME_TRACKS),
        [](const tvtts_event *ev, void *user) -> int {
            if (ev->type == TVTTS_AUDIO)
                static_cast<std::vector<float> *>(user)->insert(static_cast<std::vector<float> *>(user)->end(),
                                                                ev->samples, ev->samples + ev->count);
            return 0;
        },
        &pcm);
    return pcm;
}

// Shimmer, the other half of creak: the stretch cut into lengths of the
// period expected, each length's loudest sample, and the step from each to
// the next on average, over the average. A voice that never wavers in
// loudness is near 0.
double shimmer(const std::vector<float> &x, double rate, double hz, double from, double to) {
    double T = rate / hz, end = std::min(to * rate, double(x.size()));
    std::vector<double> peaks;
    for (double a = from * rate; a + T < end; a += T) {
        double pk = 0;
        for (size_t k = size_t(a); k < size_t(a + T); ++k) pk = std::max(pk, double(std::fabs(x[k])));
        peaks.push_back(pk);
    }
    double steps = 0, sum = 0;
    for (size_t i = 1; i < peaks.size(); ++i) steps += std::fabs(peaks[i] - peaks[i - 1]);
    for (double pk : peaks) sum += pk;
    return peaks.size() > 1 && sum > 0 ? (steps / double(peaks.size() - 1)) / (sum / double(peaks.size())) : 0;
}

// The level of the 10 ms of `x` from `t` seconds, in decibels.
double level_at(const std::vector<float> &x, double t) {
    long a = std::lround(t * kSampleRate), n = kSampleRate / 100;
    double e = 0;
    for (long i = std::max(a, 0L); i < a + n && i < long(x.size()); ++i) e += double(x[size_t(i)]) * x[size_t(i)];
    return 10 * std::log10(e / double(n) + 1e-12);
}

// How deep a hole in `x` near `t` seconds is: each 10 ms from 100 ms before
// it to 50 ms after, every 2.5 ms, against the loudest 10 ms in the 50 ms
// before that and in the 50 ms after, the lesser of the two; the deepest, in
// decibels. A stop's closure is one; between two vowels there should be none.
double deepest_dip(const std::vector<float> &x, double t) {
    double worst = 0;
    for (int k = -40; k <= 20; ++k) {
        double u = t + k * 0.0025, before = -300, after = -300;
        for (int i = 0; i <= 16; ++i) {
            before = std::max(before, level_at(x, u - 0.05 + i * 0.0025));
            after = std::max(after, level_at(x, u + 0.0125 + i * 0.0025));
        }
        worst = std::max(worst, std::min(before, after) - level_at(x, u));
    }
    return worst;
}

// Every voice's creak, each holding AE at the voice's own speaking pitch: R,
// the engine's own, from its pitch byte, as it speaks; G, the same frames
// given that pitch as an exact period; and S, the studio singing it, both
// ways -- on exact periods as the engine makes them ("frac"), and on whole
// periods retuned afterwards ("retune"). Each as periodicity, which jitter and
// breath take down, and shimmer. The renderings go to WAV files.
void creak(Family &fam, const std::string &out) {
    std::printf("  %-14s %6s | %-13s %-13s %-13s %-13s\n", "", "", "R", "G", "S frac", "S retune");
    std::printf("  %-14s %6s | %6s %6s %6s %6s %6s %6s %6s %6s\n", "voice", "Hz", "period", "shim", "period", "shim",
                "period", "shim", "period", "shim");
    for (int v = 0; v < 11; ++v) {
        tvtts_synth *s = tvtts_create(11025);
        tvtts_set_voice(s, v);
        // the engine's own AE, its middle frame, held for two seconds
        struct Seen {
            std::vector<uint8_t> frames;
            uint32_t a = 0, after = 0;
            bool in_a = false;
        } seen;
        tvtts_phonemes_to_frames(
            s, "&a1.",
            [](const tvtts_frame_event *ev, void *user) -> int {
                auto *t = static_cast<Seen *>(user);
                if (ev->type == TVTTS_FRAME) {
                    t->frames.insert(t->frames.end(), ev->tracks, ev->tracks + TVTTS_FRAME_TRACKS);
                } else if (ev->type == TVTTS_PHONEME) {
                    if (ev->phoneme == 'a') {
                        t->a = ev->frame;
                        t->in_a = true;
                    } else if (t->in_a) {
                        t->after = ev->frame;
                        t->in_a = false;
                    }
                }
                return 0;
            },
            &seen);
        size_t mid = (seen.a + std::max(seen.a + 1, seen.after)) / 2;
        int midi = int(std::lround(69 + 12 * std::log2(tvtts_voice_pitch(v) / 440.0)));
        double hz = PitchCurve::midi_to_hz(midi);
        std::vector<uint8_t> held;
        for (int k = 0; k < 200; ++k)
            held.insert(held.end(), seen.frames.begin() + long(mid * TVTTS_FRAME_TRACKS),
                        seen.frames.begin() + long((mid + 1) * TVTTS_FRAME_TRACKS));
        for (int k = 0; k < 200; ++k) held[size_t(k) * TVTTS_FRAME_TRACKS + 17] = uint8_t(std::lround(hz / 2));
        std::vector<uint16_t> periods(200, uint16_t(std::lround(11025 / hz * TVTTS_PERIOD_UNIT)));
        std::vector<float> r = rendered(s, held, nullptr), g = rendered(s, held, periods.data());
        tvtts_destroy(s);
        double lead = TVTTS_LEAD_FRAMES * 110 / 11025.0;

        Phrase p;
        p.bpm = 60;
        p.notes = {note(midi, 2, {"m", "AE"}, {80, 1920})};
        SungNote rest = note(midi, 0.4, {"%"}, {400});
        rest.velocity = 1;
        p.notes.push_back(rest);
        p.style.vibrato = 0;
        std::string err;
        auto singer = fam.singer(kTruVoiceBase + v, &err);
        truvoice_set_retune(0);
        Sung frac = singer->sing(p);
        truvoice_set_retune(1);
        Sung tuned = singer->sing(p);
        truvoice_set_retune(-1);
        double a = 0.5 + frac.starts_before, b = 1.5 + frac.starts_before;
        std::printf("  %-14s %6.1f | %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f %6.3f\n", tvtts_voice_name(v), hz,
                    periodicity(r, 11025, hz, lead + 0.5, lead + 1.5), shimmer(r, 11025, hz, lead + 0.5, lead + 1.5),
                    periodicity(g, 11025, hz, lead + 0.5, lead + 1.5), shimmer(g, 11025, hz, lead + 0.5, lead + 1.5),
                    periodicity(frac.samples, kSampleRate, hz, a, b), shimmer(frac.samples, kSampleRate, hz, a, b),
                    periodicity(tuned.samples, kSampleRate, hz, a, b), shimmer(tuned.samples, kSampleRate, hz, a, b));
        for (float &x : r) x /= 32768.0f;
        for (float &x : g) x /= 32768.0f;
        std::string base = join_path(out, "creak_" + std::to_string(kTruVoiceBase + v));
        write_wav(base + "_R.wav", r, 1, 11025);
        write_wav(base + "_G.wav", g, 1, 11025);
        write_wav(base + "_S_frac.wav", frac.samples, 1, kSampleRate);
        write_wav(base + "_S_retune.wav", tuned.samples, 1, kSampleRate);
    }
}

}  // namespace

int main(int argc, char **argv) {
    std::string out = ".";
    bool want_levels = false, want_creak = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else if (!std::strcmp(argv[i], "--levels")) want_levels = true;
        else if (!std::strcmp(argv[i], "--creak")) want_creak = true;
        else {
            std::fprintf(stderr, "usage: truvoice_sing_test [--out DIR] [--levels] [--creak]\n");
            return 2;
        }
    }
    std::unique_ptr<Family> fam = make_truvoice_family();
    std::printf("%s voices:\n", fam->name().c_str());
    auto voices = fam->voices();
    std::set<int> ids;
    for (const VoiceEntry &v : voices) {
        std::printf("  %d  %s\n", v.id, v.name.c_str());
        ids.insert(v.id);
    }
    check(voices.size() == 11 && ids.size() == 11, "11 voices, every number different");
    check(voices.front().id == kTruVoiceBase && voices.front().name == "TruVoice Peter", "9000 is TruVoice Peter");
    check(voices.back().id == kTruVoiceBase + 10 && voices.back().name == "TruVoice Frank", "9010 is TruVoice Frank");

    std::string bad = truvoice_self_check();
    check(bad.empty(), "phoneme table: all 57 VocalWriter phonemes map to TruVoice's codes, and the voices are the engine's" +
                           (bad.empty() ? std::string() : " -- " + bad));
    std::string why, err;
    check(fam->available(&why), "TruVoice is compiled in and needs no data");
    check(!fam->singer(kTruVoiceBase + 999, &err) && !err.empty(), "an unknown voice number is refused: " + err);
    if (want_levels) {
        levels(*fam);
        return failures ? 1 : 0;
    }
    if (want_creak) {
        make_dirs(out);
        creak(*fam, out);
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
        std::string file = join_path(out, "truvoice_" + std::to_string(v.id) + ".wav");
        write_wav(file, sung.samples, 1, kSampleRate);
        check(!sung.stopped_short && secs >= curve.total() + sung.starts_before && peak <= 1.0f && peak > 0.05f,
              v.name + ": " + std::to_string(secs) + " s for " + std::to_string(curve.total()) +
                  " s of notes, peak " + std::to_string(peak) + " -> " + file);
        // The middle of each note's vowel, on its pitch to within 10 cents.
        // The voices that creak are sung on exact periods, the same way Peter
        // is below, and the engine's own test holds a creaking voice's periods
        // to the one given on average; but measured, a creak is not a pitch --
        // Grandpa Amos moves a period a fifth of its length, and a tracker
        // finds tens of cents either way from one note to the next, sharp more
        // often, as it finds the shortest periods first. Of them it is asked
        // only that no note is a wrong one: nothing a semitone out.
        bool creaks = v.name == "TruVoice Grandpa Amos" || v.name == "TruVoice Melvin" || v.name == "TruVoice Alex" ||
                      v.name == "TruVoice Frank";
        double worst = 0;
        std::string each;
        for (size_t i = 0; i + 1 < p.notes.size(); ++i) {
            // the notes sung on a full vowel: a syllabic l is too quiet in
            // some voices to measure
            if (p.notes[i].phonemes.back() == "EL") continue;
            double mid = (curve.note_start(i) + curve.note_end(i)) / 2 + sung.starts_before;
            double want = PitchCurve::midi_to_hz(p.notes[i].midi);
            double hz = pitch_at(sung.samples, mid);
            double cents = hz > 0 ? 1200 * std::log2(hz / want) : 9999;
            worst = std::max(worst, std::fabs(cents));
            each += "  note " + std::to_string(i) + ": " + std::to_string(int(std::lround(hz))) + " Hz for " +
                    std::to_string(int(std::lround(want))) + "\n";
        }
        double allowed = creaks ? 100 : 10;
        check(worst < allowed, v.name + (creaks ? ": no note a wrong one (worst " : ": every note on its pitch (worst ") +
                                   std::to_string(worst) + " cents)");
        if (worst >= allowed) std::printf("%s", each.c_str());
    }
    // The way the creaking voices are sung -- exact periods, nothing after --
    // on a voice with nothing to creak with, where a tracker can be believed:
    // Peter, every note on its pitch to within 10 cents that way too.
    {
        truvoice_set_retune(0);
        auto peter = fam->singer(kTruVoiceBase, &err);
        Sung exact = peter->sing(p);
        truvoice_set_retune(-1);
        double worst = 0;
        for (size_t i = 0; i + 1 < p.notes.size(); ++i) {
            if (p.notes[i].phonemes.back() == "EL") continue;
            double mid = (curve.note_start(i) + curve.note_end(i)) / 2 + exact.starts_before;
            double hz = pitch_at(exact.samples, mid);
            double want = PitchCurve::midi_to_hz(p.notes[i].midi);
            worst = std::max(worst, hz > 0 ? std::fabs(1200 * std::log2(hz / want)) : 9999.0);
        }
        write_wav(join_path(out, "truvoice_9000_exact.wav"), exact.samples, 1, kSampleRate);
        check(worst < 10, "sung on exact periods, as the creaking voices are, every note is on its pitch (worst " +
                              std::to_string(worst) + " cents)");
    }
    // The ends of the range, F1 and D6, past the engine's own pitches, G#1 to
    // B4: sung where they are written, not moved an octave in, both ways --
    // on whole periods retuned (Wanda at the top, Deep Douglas at the bottom)
    // and on exact ones (Peter, made to sing that way). Ten periods at a time
    // repeat almost perfectly, and last as long as ten of the note's.
    {
        struct End {
            const char *what;
            int voice, retune;
            std::vector<int> notes;
        };
        const End ends[] = {
            {"Wanda sings C6 and D6", 8, -1, {84, 86}},
            {"Peter on exact periods sings C6 and D6", 0, 0, {84, 86}},
            {"Deep Douglas sings F1 and G1", 3, -1, {29, 31}},
            {"Peter on exact periods sings F1 and G1", 0, 0, {29, 31}},
        };
        for (const End &e : ends) {
            Phrase edge;
            edge.bpm = 60;
            for (int midi : e.notes) edge.notes.push_back(note(midi, 1, {"m", "AA"}, {80, 920}));
            SungNote edge_rest = note(e.notes.back(), 0.4, {"%"}, {400});
            edge_rest.velocity = 1;
            edge.notes.push_back(edge_rest);
            edge.style.vibrato = 0;
            PitchCurve edge_curve(edge);
            truvoice_set_retune(e.retune);
            auto singer = fam->singer(kTruVoiceBase + e.voice, &err);
            Sung sung = singer->sing(edge);
            truvoice_set_retune(-1);
            write_wav(join_path(out, "truvoice_" + std::to_string(kTruVoiceBase + e.voice) +
                                         (e.retune == 0 ? "_exact" : "") + (e.notes[0] > 60 ? "_top" : "_bottom") +
                                         ".wav"),
                      sung.samples, 1, kSampleRate);
            double worst = 0;
            std::string each;
            for (size_t i = 0; i < e.notes.size(); ++i) {
                double mid = (edge_curve.note_start(i) + edge_curve.note_end(i)) / 2 + sung.starts_before;
                double want = PitchCurve::midi_to_hz(e.notes[i]);
                double match = 0, hz = ten_period_pitch(sung.samples, mid, want, &match);
                worst = std::max(worst, hz > 0 && match > 0.8 ? std::fabs(1200 * std::log2(hz / want)) : 9999.0);
                each += "  " + std::to_string(hz) + " Hz for " + std::to_string(want) + ", ten periods matching " +
                        std::to_string(match) + "\n";
            }
            check(worst < 5, std::string(e.what) + " where they are written, ten periods at a time (worst " +
                                 std::to_string(worst) + " cents)");
            if (worst >= 5) std::printf("%s", each.c_str());
        }
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
    auto first = fam->singer(kTruVoiceBase, &err);
    Sung long_sung = first->sing(run);
    write_wav(join_path(out, "truvoice_long.wav"), long_sung.samples, 1, kSampleRate);
    double worst = 0;
    std::string each_run;
    for (size_t i = 290; i < 300; ++i) {
        double mid = (run_curve.note_start(i) + run_curve.note_end(i)) / 2 + 0.02 + long_sung.starts_before;
        double hz = pitch_at(long_sung.samples, mid, 0.006);
        double cents = hz > 0 ? 1200 * std::log2(hz / PitchCurve::midi_to_hz(run.notes[i].midi)) : 9999;
        worst = std::max(worst, std::fabs(cents));
        each_run += "  note " + std::to_string(i) + ": " + std::to_string(hz) + " Hz for " +
                    std::to_string(PitchCurve::midi_to_hz(run.notes[i].midi)) + "\n";
    }
    check(worst < 15, "the last notes of a 37-second phrase are on their pitches where they are due (worst " +
                          std::to_string(worst) + " cents)");
    if (worst >= 15) std::printf("%s", each_run.c_str());
    // Nor does it dip at any note more than at the others: every note's l,
    // against the middle of them all. Sung in pieces of forty notes, every
    // fortieth dipped 9 dB deeper, the engine fading out and in again.
    std::vector<double> dips;
    for (size_t i = 1; i < 300; ++i)
        dips.push_back(deepest_dip(long_sung.samples, run_curve.note_start(i) + long_sung.starts_before));
    std::vector<double> by_depth = dips;
    std::sort(by_depth.begin(), by_depth.end());
    double middle = by_depth[by_depth.size() / 2];
    size_t deepest = size_t(std::max_element(dips.begin(), dips.end()) - dips.begin()) + 1;
    check(by_depth.back() - middle < 5, "no note of the 37-second phrase dips deeper than the others (note " +
                                            std::to_string(deepest) + ", " + std::to_string(by_depth.back()) +
                                            " dB; the middle of them " + std::to_string(middle) + ")");
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
    // No gap is sung where TruVoice pauses as it speaks: before a word that
    // starts on its vowel after one that ends on one ("your | answer", and
    // "marri | age" sung over two notes), and between an s and the w after
    // it ("sweet"). Each was a hole 12 to 18 dB deep in "Daisy Bell".
    Phrase joins;
    joins.bpm = 100;
    joins.notes = {
        note(55, 1, {"y", "OR"}, {60, 540}),   note(52, 1, {"AE", "n"}, {480, 120}),
        note(55, 1, {"s", "ER"}, {90, 510}),   note(57, 1, {"m", "AR"}, {60, 540}),
        note(55, 1, {"IH", "JH"}, {480, 120}), note(52, 1, {"l", "UH", "k"}, {60, 470, 70}),
        note(55, 2, {"s", "w", "IY", "t"}, {90, 50, 1000, 60}),
    };
    SungNote joins_end = note(55, 0.4, {"%"}, {240});
    joins_end.velocity = 1;
    joins.notes.push_back(joins_end);
    joins.style.vibrato = 0;
    PitchCurve joins_curve(joins);
    Sung joined = first->sing(joins);
    write_wav(join_path(out, "truvoice_joins.wav"), joined.samples, 1, kSampleRate);
    const struct {
        const char *name;
        double at, allowed;
    } holes[] = {
        {"your | answer", joins_curve.note_start(1), 8},
        {"marri | age", joins_curve.note_start(4), 8},
        {"s | weet", joins_curve.note_start(6) + 0.09, 14},
    };
    for (const auto &h : holes) {
        double dip = deepest_dip(joined.samples, h.at + joined.starts_before);
        check(dip < h.allowed, std::string("no hole sung at \"") + h.name + "\" (the deepest dip " +
                                   std::to_string(dip) + " dB, under " + std::to_string(int(h.allowed)) + ")");
    }
    std::printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
