// sing_test.cpp -- sings a few phrases with DECtalk's voices and checks what
// came out.
//
//   dectalk_sing_test [output folder]
//
// "daisy" is a tune to listen to (its last note has a bend that rises and
// falls inside the vowel, which DECtalk cannot follow, and does not hear). "consonants" and
// "twinkle" are there to be listened to as well. Each of them is also read
// back: every clause the singer sent DECtalk is spoken again with DECtalk's
// phoneme log on, and the phonemes DECtalk sang are compared with the ones
// sent -- the same phonemes, apart from DECtalk's own allophones (which are
// listed), and every one exactly as many frames long as asked.
//
// "scale" holds an open vowel on every note of DECtalk's scale, C2 to C5, in
// three voices, and the mean F0 of each, over whole cycles of DECtalk's
// vibrato, is measured against equal temperament at A = 440 Hz. "pitch" does
// the same for a few notes and voices. "timing" sings a vowel straight
// through on alternate notes a fifth apart, and finds, period by period,
// where the pitch crosses half way (geometrically) from one note to the
// next: DECtalk starts each note on the 6.44 ms frame nearest its time and
// glides to it in a straight line, in hertz, over 16 frames, so the crossing
// belongs that far into the glide. "vibrato" holds one note and measures
// DECtalk's vibrato. "bend" holds a note, bends it up a whole tone for the
// next note, and back; DECtalk follows bends by whole semitones. "long" is a
// phrase of over half a minute without a rest, which DECtalk has to be given
// in two pieces; "high" goes above DECtalk's top note and is sung an octave
// down.
//
// "reference" is the first line of DECtalk's own "Happy Birthday", from its
// reference guide, rendered straight through DECtalk as written; the same
// tune, the same phonemes and the same lengths are then sung through the
// singer, and the two are compared: the pitch of each held vowel against
// A = 440 Hz, the vibrato's rate and depth, how much the pitch wanders
// besides the vibrato and a straight line, and how smooth the loudness is
// through the long vowels (the deepest dip of the RMS envelope below its own
// 150 ms average). DECtalk's own glide is timed on a tune written in its
// syntax, and the three places a note number can go (the note's first
// phoneme, every phoneme, the vowel only) are compared by how far the pitch
// still is from the note 20 ms into the vowel. "diphthongs" sings long "day"
// and "my" both ways and tracks the second formant through the vowel by LPC:
// DECtalk's ey must rise towards iy and its ay from aa towards iy, in the
// singer as in DECtalk's own.
//
// The test drives DECtalk directly too, for the reference and the log, so it
// needs DECtalk.dll in voices\dectalk like the singer does.
//
// Every file is checked for its length (the phrase's own, from its pitch
// curve) and its peak.
//
// SVS_DECTALK_TRACE=1 in the environment shows how each phrase was cut up.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "audio/wav.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

using namespace svs;

namespace {

const double kPi = 3.14159265358979323846;

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

void add_rest(Phrase &p) {
    // the closing rest every phrase ends with
    SungNote rest;
    rest.midi = p.notes.back().midi;
    rest.beats = 0.4;
    rest.velocity = 1;
    rest.phonemes = {kRest};
    rest.durations_ms = {0.4 * 60000.0 / p.bpm};
    p.notes.push_back(rest);
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
    add_rest(p);
    return p;
}

// F0 of x over [a, b), between lo and hi Hz, by normalised autocorrelation:
// the first lag whose correlation comes within 10% of the best, refined by a
// parabola through its neighbours.
double f0_between(const std::vector<float> &x, long a, long b, double lo, double hi, double sr = kSampleRate) {
    long l0 = long(sr / hi), l1 = long(sr / lo) + 1;
    a = std::max(0L, a);
    b = std::min(long(x.size()), b);
    long n = b - a - l1 - 1;
    if (n <= 16) return 0.0;
    std::vector<double> r(size_t(l1 + 2), 0.0);
    double e0 = 0.0;
    for (long i = a; i < a + n; ++i) e0 += double(x[size_t(i)]) * x[size_t(i)];
    if (e0 <= 1e-9) return 0.0;
    for (long lag = std::max(1L, l0 - 1); lag <= l1 + 1; ++lag) {
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
double midi_hz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

// A Butterworth low-pass at `hz`, two second-order sections run forwards and
// then backwards, so that it delays nothing.
std::vector<double> lowpass(const std::vector<float> &x, double hz, double sr = kSampleRate) {
    std::vector<double> y(x.begin(), x.end());
    double w = std::tan(kPi * hz / sr);
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

long at_ms(double ms, double sr = kSampleRate) { return std::lround(ms * sr / 1000.0); }

// -- measuring a held vowel --------------------------------------------------

//: F0 every 5 ms over [t0, t1) seconds, by autocorrelation over 30 ms.
struct Track {
    std::vector<double> t, f;
};
Track f0_track(const std::vector<float> &x, double sr, double t0, double t1, double lo, double hi) {
    Track tr;
    for (double t = t0; t < t1; t += 0.005) {
        double v = f0_between(x, std::lround((t - 0.015) * sr), std::lround((t + 0.015) * sr), lo, hi, sr);
        if (v > 0) {
            tr.t.push_back(t);
            tr.f.push_back(v);
        }
    }
    return tr;
}

struct Held {
    double mean_hz = 0;       //: mean F0
    double rate = 0;          //: vibrato rate, from crossings of the mean
    double swing = 0;         //: vibrato depth, peak to peak in Hz (mean of each cycle's)
    double wander = 0;        //: rms of F0 minus the best sinusoid at the vibrato rate and a line, Hz
    double dip = 0;           //: deepest RMS-envelope dip below its 150 ms average, dB
};

Held held(const std::vector<float> &x, double sr, double t0, double t1, double want_hz) {
    Held h;
    Track tr = f0_track(x, sr, t0, t1, want_hz / 1.4, want_hz * 1.4);
    if (tr.f.size() < 8) return h;
    double m = 0;
    for (double v : tr.f) m += v;
    m /= tr.f.size();
    h.mean_hz = m;
    // crossings of the mean, and the swing of each half cycle between them
    std::vector<double> cross;
    for (size_t k = 1; k < tr.f.size(); ++k)
        if ((tr.f[k - 1] - m) * (tr.f[k] - m) < 0) cross.push_back(tr.t[k]);
    if (cross.size() >= 3) h.rate = (cross.size() - 1) / 2.0 / (cross.back() - cross.front());
    double sw = 0;
    int n = 0;
    for (size_t c = 0; c + 2 < cross.size(); c += 2) {
        double lo = 1e9, hi = -1e9;
        for (size_t k = 0; k < tr.t.size(); ++k)
            if (tr.t[k] >= cross[c] && tr.t[k] < cross[c + 2]) {
                lo = std::min(lo, tr.f[k]);
                hi = std::max(hi, tr.f[k]);
            }
        if (hi > lo) {
            sw += hi - lo;
            ++n;
        }
    }
    h.swing = n ? sw / n : 0;
    // least squares: f = a + b t + c sin + d cos at the measured rate
    if (h.rate > 0) {
        double w = 2 * kPi * h.rate;
        double A[4][5] = {};
        for (size_t k = 0; k < tr.f.size(); ++k) {
            double t = tr.t[k] - tr.t[0];
            double v[4] = {1, t, std::sin(w * t), std::cos(w * t)};
            for (int i = 0; i < 4; ++i) {
                for (int j = 0; j < 4; ++j) A[i][j] += v[i] * v[j];
                A[i][4] += v[i] * tr.f[k];
            }
        }
        for (int i = 0; i < 4; ++i) {
            int p = i;
            for (int r = i + 1; r < 4; ++r)
                if (std::fabs(A[r][i]) > std::fabs(A[p][i])) p = r;
            for (int c = 0; c < 5; ++c) std::swap(A[i][c], A[p][c]);
            for (int r = 0; r < 4; ++r)
                if (r != i && A[i][i] != 0) {
                    double f = A[r][i] / A[i][i];
                    for (int c = 0; c < 5; ++c) A[r][c] -= f * A[i][c];
                }
        }
        double co[4];
        for (int i = 0; i < 4; ++i) co[i] = A[i][i] != 0 ? A[i][4] / A[i][i] : 0;
        double e = 0;
        for (size_t k = 0; k < tr.f.size(); ++k) {
            double t = tr.t[k] - tr.t[0];
            double fit = co[0] + co[1] * t + co[2] * std::sin(w * t) + co[3] * std::cos(w * t);
            e += (tr.f[k] - fit) * (tr.f[k] - fit);
        }
        h.wander = std::sqrt(e / tr.f.size());
    }
    // the loudness every 10 ms, against its 150 ms moving average
    std::vector<double> env;
    long win = std::lround(0.010 * sr);
    for (long s = std::lround(t0 * sr); s + win <= std::lround(t1 * sr) && s + win <= long(x.size()); s += win) {
        double e = 0;
        for (long i = s; i < s + win; ++i) e += double(x[size_t(i)]) * x[size_t(i)];
        env.push_back(std::sqrt(e / win) + 1e-9);
    }
    double dip = 0;
    for (size_t k = 7; k + 7 < env.size(); ++k) {
        double avg = 0;
        for (size_t j = k - 7; j <= k + 7; ++j) avg += env[j];
        avg /= 15;
        dip = std::min(dip, 20 * std::log10(env[k] / avg));
    }
    h.dip = dip;
    return h;
}

void print_held(const char *who, const char *what, const Held &h, double want_hz) {
    std::printf("    %-9s %-6s %+6.1f cents  vibrato %.2f Hz, %.2f Hz p-p  wander %.2f Hz  dip %.1f dB\n",
                who, what, h.mean_hz > 0 ? cents(h.mean_hz, want_hz) : 999.0, h.rate, h.swing,
                h.wander, h.dip);
}

// The pitch period by period: the sound low-passed below the second
// harmonic of the lower note (both ways, so nothing is delayed), and the time
// between rising zero crossings, each period's pitch put at its middle.
Track periods(const std::vector<float> &x, double sr, double cutoff) {
    std::vector<double> y = lowpass(x, cutoff, sr);
    std::vector<double> zc;
    for (size_t k = 1; k < y.size(); ++k)
        if (y[k - 1] < 0 && y[k] >= 0) zc.push_back(k - 1 + -y[k - 1] / (y[k] - y[k - 1]));
    Track tr;
    for (size_t k = 1; k < zc.size(); ++k) {
        tr.t.push_back(0.5 * (zc[k] + zc[k - 1]) / sr);
        tr.f.push_back(sr / (zc[k] - zc[k - 1]));
    }
    return tr;
}

// The 10%-90% time of a pitch change from fa to fb that starts after `t`.
double glide_ms(const Track &tr, double t, double fa, double fb) {
    double a = fa + 0.1 * (fb - fa), b = fa + 0.9 * (fb - fa);
    double ta = -1, tb = -1;
    for (size_t k = 1; k < tr.f.size(); ++k) {
        if (tr.t[k] < t) continue;
        if (tr.t[k] > t + 0.4) break;
        if (ta < 0 && (tr.f[k - 1] - a) * (tr.f[k] - a) <= 0) ta = tr.t[k];
        if (ta >= 0 && tb < 0 && (tr.f[k - 1] - b) * (tr.f[k] - b) <= 0) tb = tr.t[k];
    }
    return ta >= 0 && tb >= 0 ? 1000 * (tb - ta) : -1;
}

// The first two formants at `t`, by LPC (order 12, at 11025 Hz) over 30 ms:
// the first two peaks of the LPC envelope above 200 Hz.
std::vector<float> to_11025(const std::vector<float> &x, double sr) {
    if (sr == 11025) return x;
    std::vector<double> y = lowpass(x, 4500, sr);
    std::vector<float> out;
    for (size_t i = 0; i < y.size(); i += 4) out.push_back(float(y[i]));
    return out;
}
bool formants(const std::vector<float> &x, double t, double *f1, double *f2) {
    const int order = 12, n = 331;
    long s0 = std::lround(t * 11025) - n / 2;
    if (s0 < 1 || s0 + n >= long(x.size())) return false;
    std::vector<double> w(n);
    for (int i = 0; i < n; ++i)
        w[size_t(i)] = (x[size_t(s0 + i)] - 0.94 * x[size_t(s0 + i - 1)]) *
                       (0.54 - 0.46 * std::cos(2 * kPi * i / (n - 1)));
    double r[order + 1];
    for (int k = 0; k <= order; ++k) {
        r[k] = 0;
        for (int i = k; i < n; ++i) r[k] += w[size_t(i)] * w[size_t(i - k)];
    }
    if (r[0] <= 0) return false;
    double a[order + 1] = {1}, e = r[0];
    for (int i = 1; i <= order; ++i) {
        double k = r[i];
        for (int j = 1; j < i; ++j) k += a[j] * r[i - j];
        k = -k / e;
        double tmp[order + 1];
        for (int j = 0; j <= order; ++j) tmp[j] = a[j];
        for (int j = 1; j < i; ++j) a[j] = tmp[j] + k * tmp[i - j];
        a[i] = k;
        e *= 1 - k * k;
    }
    std::vector<double> mag;
    for (int hz = 0; hz <= 4000; hz += 10) {
        double re = 0, im = 0, om = 2 * kPi * hz / 11025.0;
        for (int j = 0; j <= order; ++j) {
            re += a[j] * std::cos(om * j);
            im -= a[j] * std::sin(om * j);
        }
        mag.push_back(-std::log(re * re + im * im));
    }
    std::vector<double> peaks;
    for (size_t k = 21; k + 1 < mag.size(); ++k)
        if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1]) peaks.push_back(10.0 * k);
    if (peaks.size() < 2) return false;
    *f1 = peaks[0];
    *f2 = peaks[1];
    return true;
}

// -- DECtalk itself, for the reference and the phoneme log --------------------

struct Direct {
    typedef unsigned int (*StartupFn)(void **, unsigned int, unsigned long, void *, long, char *);
    typedef unsigned int (*SpeakFn)(void *, char *, unsigned long);
    typedef unsigned int (*HFn)(void *);
    typedef unsigned int (*FileFn)(void *, char *, unsigned long);
    StartupFn startup = nullptr;
    SpeakFn speak = nullptr;
    HFn sync = nullptr, close_wave = nullptr, close_log = nullptr;
    FileFn open_wave = nullptr, open_log = nullptr;
    void *h = nullptr;

    bool start() {
        std::string dir = find_data("voices\\dectalk");
#ifdef _WIN32
        HMODULE m = LoadLibraryW(widen(join_path(dir, "DECtalk.dll")).c_str());
        auto get = [&](const char *n) { return reinterpret_cast<void *>(GetProcAddress(m, n)); };
#else
#ifdef __APPLE__
        void *m = dlopen(join_path(dir, "libtts.dylib").c_str(), RTLD_NOW);
#else
        void *m = dlopen(join_path(dir, "libtts.so").c_str(), RTLD_NOW);
#endif
        auto get = [&](const char *n) { return m ? dlsym(m, n) : nullptr; };
#endif
        if (!m) return false;
        startup = reinterpret_cast<StartupFn>(get("TextToSpeechStartupExFonix"));
        speak = reinterpret_cast<SpeakFn>(get("TextToSpeechSpeak"));
        sync = reinterpret_cast<HFn>(get("TextToSpeechSync"));
        open_wave = reinterpret_cast<FileFn>(get("TextToSpeechOpenWaveOutFile"));
        close_wave = reinterpret_cast<HFn>(get("TextToSpeechCloseWaveOutFile"));
        open_log = reinterpret_cast<FileFn>(get("TextToSpeechOpenLogFile"));
        close_log = reinterpret_cast<HFn>(get("TextToSpeechCloseLogFile"));
        if (!startup || !speak || !sync || !open_wave || !close_wave || !open_log || !close_log) return false;
        std::string dic = join_path(dir, "dtalk_us.dic");
        std::vector<char> d(dic.begin(), dic.end());
        d.push_back(0);
        return startup(&h, 0xFFFFFFFFu, 0x80000000u, nullptr, 0, d.data()) == 0 && h;
    }
    void say(const std::string &text) {
        std::vector<char> b(text.begin(), text.end());
        b.push_back(0);
        speak(h, b.data(), 1);
        sync(h);
    }
    bool wave(const std::string &text, const std::string &path, std::vector<float> *out) {
        std::vector<char> p(path.begin(), path.end());
        p.push_back(0);
        if (open_wave(h, p.data(), 0x0004)) return false;
        say(text);
        close_wave(h);
        int ch = 0, rate = 0;
        return read_wav(path, out, &ch, &rate) && rate == 11025;
    }
    // The phonemes DECtalk sang, with their lengths in frames.
    struct Ph {
        std::string sym;
        int frames;
    };
    bool log(const std::string &text, const std::string &path, std::vector<Ph> *out) {
        std::vector<char> p(path.begin(), path.end());
        p.push_back(0);
        if (open_log(h, p.data(), 0x0002)) return false;
        say("[:log outphon on]" + text);
        close_log(h);
        out->clear();
        FILE *f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        char line[4096];
        while (std::fgets(line, sizeof line, f)) {
            // "us_aa<333,110>": the phoneme, its length in samples * 10 / 71 ... as
            // DECtalk prints it (frames * 71 / 10), and the pitch it starts on
            char sym[16];
            int dur = 0, f0 = 0;
            if (std::strncmp(line, "us_", 3) == 0 && std::strchr(line + 3, ' ') == nullptr &&
                std::sscanf(line + 3, "%15[a-z_]<%d,%d>", sym, &dur, &f0) == 3)
                out->push_back({sym, int(std::lround(dur * 10.0 / 71.0))});
        }
        std::fclose(f);
        return true;
    }
};

// What a clause asked for: each phoneme and its frames.
std::vector<Direct::Ph> clause_phonemes(const std::string &text) {
    std::vector<Direct::Ph> out;
    size_t at = text.rfind("][");
    if (at == std::string::npos) return out;
    for (size_t i = at + 2; i < text.size();) {
        if (text[i] == '\'' || text[i] == ']') {
            ++i;
            continue;
        }
        size_t lt = text.find('<', i), gt = text.find('>', i);
        if (lt == std::string::npos || gt == std::string::npos) break;
        int ms = std::atoi(text.c_str() + lt + 1);
        out.push_back({text.substr(i, lt - i), ((ms + 4) * 10) >> 6});
        i = gt + 1;
    }
    return out;
}

// DECtalk's own allophones of what is sent (ph_aloph1.c): l after a vowel is
// dark, t before dh or a sonorant can be d or tx, dh after t, d or n is dz or
// n, and so on. Allowed, and reported.
bool allophone_of(const std::string &sent, const std::string &sung) {
    static const char *pairs[][2] = {{"ll", "lx"}, {"t", "d"}, {"t", "tx"}, {"t", "df"}, {"t", "dx"},
                                     {"d", "dx"}, {"dh", "dz"}, {"dh", "n"}, {"t", "ch"}, {"d", "jh"},
                                     {"r", "rx"}};
    for (auto &p : pairs)
        if (sent == p[0] && sung == p[1]) return true;
    return false;
}

}  // namespace

int main(int argc, char **argv) {
    std::string out = argc > 1 ? argv[1] : "build-tests/dectalk/out";
    make_dirs(out);
    std::string text_file = join_path(out, "dectalk_sent.txt");
    std::remove(text_file.c_str());
#ifdef _WIN32
    _putenv_s("SVS_DECTALK_TEXT", text_file.c_str());
#else
    setenv("SVS_DECTALK_TEXT", text_file.c_str(), 1);
#endif

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
        daisy.any_bends = true;
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
    std::vector<Syl> scale_syls;
    for (int m = 36; m <= 72; ++m) scale_syls.push_back({m, 1.5, "AA"});
    Phrase scale = make_phrase(60, scale_syls, 0);
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
    Phrase bend = make_phrase(60, {{57, 1.5, "l AA"}, {57, 1.5, "l AA"}, {57, 1.5, "l AA"}}, 31);
    {
        // a whole tone up over the second note's "l", held, and back for the third
        PitchCurve c(bend);
        for (double t = c.note_start(1) - 0.03; t < c.note_start(2) - 0.03; t += 0.005)
            bend.bends.push_back({t, std::min(2.0, 2.0 * (t - c.note_start(1) + 0.03) / 0.05)});
        bend.bends.push_back({c.note_start(2) - 0.03, 0.0});
        bend.any_bends = true;
    }
    std::vector<Syl> long_syls;
    const int tune[8] = {60, 62, 64, 65, 67, 65, 64, 62};
    for (int k = 0; k < 64; ++k) long_syls.push_back({tune[k % 8], 1, k % 2 ? "s t AA n" : "m AA"});
    Phrase longp = make_phrase(110, long_syls, 31);
    Phrase high = make_phrase(100, {
        {76, 1, "l AA"}, {79, 1, "l AA"}, {84, 1, "l AA"}, {81, 2, "l AA"},
    }, 31);
    Phrase diph = make_phrase(60, {{57, 2.5, "d EY"}, {57, 0.5, "%"}, {57, 2.5, "m AY"}}, 31);

    std::unique_ptr<Family> fam = make_dectalk_family();
    std::string why;
#ifdef _WIN32
    // DECtalk prints a little of its phoneme log to its own stdout, which it
    // takes when it is loaded; it is given nowhere to print it.
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE nowhere = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (nowhere != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, nowhere);
    bool have = fam->available(&why);
    SetStdHandle(STD_OUTPUT_HANDLE, console);
#else
    bool have = fam->available(&why);
#endif
    std::printf("%s: %s\n", fam->name().c_str(), have ? "available" : why.c_str());
    for (const VoiceEntry &v : fam->voices()) std::printf("  %d %s\n", v.id, v.name.c_str());
    if (!have) return 1;
    Direct direct;
    bool have_direct = direct.start();
    if (!have_direct) std::printf("DECtalk could not be opened directly; no reference or phoneme log\n");

    int failures = 0;
    auto sing = [&](int vi, const Phrase &p, Sung *s) {
        std::string err;
        auto singer = fam->singer(kDectalkBase + vi, &err);
        if (!singer) {
            std::printf("%d: %s\n", kDectalkBase + vi, err.c_str());
            ++failures;
            return false;
        }
        *s = singer->sing(p);
        return true;
    };

    struct Job {
        const char *name;
        const Phrase *phrase;
        std::vector<int> voices;
    } jobs[] = {
        {"daisy", &daisy, {0, 1, 2, 5, 8}},
        {"consonants", &consonants, {0, 1}},
        {"twinkle", &twinkle, {0, 6, 7}},
        {"fast", &fast, {0, 3}},
        {"scale", &scale, {0, 1, 5}},
        {"pitch", &pitch, {0, 1}},
        {"timing", &timing, {0, 1}},
        {"vibrato", &vibrato, {0}},
        {"bend", &bend, {0}},
        {"long", &longp, {0}},
        {"high", &high, {1}},
    };
    for (const Job &job : jobs) {
        PitchCurve curve(*job.phrase);
        for (int vi : job.voices) {
            Sung s;
            if (!sing(vi, *job.phrase, &s)) continue;
            std::string vname = fam->voices()[size_t(vi)].name.substr(8);
            std::string tag = vname;
            for (char &c : tag) c = char(std::tolower((unsigned char)c));
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

            if (job.phrase == &pitch || job.phrase == &scale) {
                // the mean over whole vibrato cycles (6.25 Hz: 0.96 s is six)
                double worst = 0;
                std::string line;
                for (size_t i = 0; i + 1 < job.phrase->notes.size(); ++i) {
                    double want_hz = midi_hz(job.phrase->notes[i].midi);
                    double a = curve.note_start(i), b = curve.note_end(i);
                    double t0 = a + 0.25, t1 = std::min(b - 0.05, t0 + 0.96);
                    double sum = 0;
                    int n = 0;
                    for (double t = t0; t < t1; t += 0.01) {
                        double v = f0_between(x, at_ms(1000 * t), at_ms(1000 * t) + std::max(at_ms(25), long(3 * kSampleRate / want_hz)),
                                              want_hz / 1.25, want_hz * 1.25);
                        if (v > 0) {
                            sum += v;
                            ++n;
                        }
                    }
                    double c = n ? cents(sum / n, want_hz) : 999;
                    worst = std::max(worst, std::fabs(c));
                    char buf[32];
                    std::snprintf(buf, sizeof buf, " %+.1f", c);
                    line += buf;
                    if (job.phrase == &pitch)
                        std::printf("    %4d  %8.2f  %8.2f  %+6.1f\n", job.phrase->notes[i].midi, want_hz,
                                    n ? sum / n : 0.0, c);
                }
                if (job.phrase == &scale) std::printf("    C2..C5, cents:%s\n", line.c_str());
                std::printf("    worst %.1f cents\n", worst);
                if (worst > 8) ++failures;
            }

            if (job.phrase == &timing) {
                // The pitch period by period: the sound low-passed below its
                // second harmonic (both ways, so nothing is delayed), and
                // the time between rising zero crossings. Each period's
                // pitch is put at its middle.
                Track tr = periods(x, kSampleRate, 300.0);
                double worst = 0;
                std::printf("    note  crossing after start (ms)  expected  error (ms)\n");
                for (size_t i = 1; i + 1 < job.phrase->notes.size(); ++i) {
                    double fa = midi_hz(job.phrase->notes[i - 1].midi);
                    double fb = midi_hz(job.phrase->notes[i].midi);
                    double mid = std::sqrt(fa * fb);
                    double st = curve.note_start(i);
                    double found = 999;
                    double pt = -1, pf = 0;
                    for (size_t k = 0; k < tr.t.size(); ++k) {
                        double t = tr.t[k], f = tr.f[k];
                        if (t < st - 0.02) continue;
                        if (t > st + 0.15) break;
                        if (pt >= 0 && (pf - mid) * (f - mid) <= 0) {
                            found = 1000 * (pt + (mid - pf) / (f - pf) * (t - pt) - st);
                            break;
                        }
                        pt = t;
                        pf = f;
                    }
                    // The note starts on the frame nearest its time, and
                    // DECtalk's straight glide over 16 frames crosses the
                    // geometric middle (mid - fa) / (fb - fa) of the way in.
                    const double frame = 71.0 / 11025.0;
                    double expect = 1000 * (std::round(st / frame) * frame - st + 16 * frame * (mid - fa) / (fb - fa));
                    double e = found - expect;
                    worst = std::max(worst, std::fabs(e));
                    std::printf("    %4zu  %8.1f                    %6.1f    %+6.1f\n", i, found, expect, e);
                }
                std::printf("    worst %.1f ms\n", worst);
                if (worst > 7) ++failures;
                double g = 0;
                for (size_t i = 1; i + 1 < job.phrase->notes.size(); ++i)
                    g += glide_ms(tr, curve.note_start(i) - 0.01, midi_hz(job.phrase->notes[i - 1].midi),
                                  midi_hz(job.phrase->notes[i].midi));
                g /= double(job.phrase->notes.size() - 2);
                std::printf("    the glide, 10%%-90%%, on average: %.1f ms (DECtalk's 16 frames: 82 ms)\n", g);
                if (g < 70 || g > 95) ++failures;
            }

            if (job.phrase == &vibrato) {
                Held h = held(x, kSampleRate, 0.6, 3.8, 220.0);
                print_held(vname.c_str(), "A3", h, 220.0);
                std::printf("    (DECtalk's own: 6.25 Hz, 4.2 Hz peak to peak and the steps of its pitch period; asked for +-%.1f cents at "
                            "%.2f Hz, which DECtalk cannot do)\n", curve.vibrato_cents(), curve.vibrato_hz());
                if (h.rate < 5.9 || h.rate > 6.6 || h.swing < 3.0 || h.swing > 6.0) ++failures;
            }

            if (job.phrase == &bend) {
                const char *what[3] = {"A3", "B3 (bent)", "A3"};
                const int m[3] = {57, 59, 57};
                for (int i = 0; i < 3; ++i) {
                    double want_hz = midi_hz(m[i]);
                    Held h = held(x, kSampleRate, curve.note_start(size_t(i)) + 0.3, curve.note_end(size_t(i)) - 0.1, want_hz);
                    print_held(vname.c_str(), what[i], h, want_hz);
                    if (std::fabs(cents(h.mean_hz, want_hz)) > 8) ++failures;
                }
            }
        }
    }

    // -- DECtalk's own "Happy Birthday", and the singer singing it --
    if (have_direct) {
        std::printf("reference: DECtalk's Happy Birthday (reference guide, figure 4-1), first line\n");
        const std::string ref_text = "[:phoneme arpabet speak on][:np][hxae<300,10>piy<300,10> "
                                     "brr<600,12>th<100>dey<600,10> tuw<600,15> yu<1200,14>_<120>]";
        std::vector<float> ref;
        std::vector<Direct::Ph> ref_ph;
        direct.wave(ref_text, join_path(out, "dectalk_reference_birthday.wav"), &ref);
        direct.log(ref_text, join_path(out, "dectalk_reference_birthday.log"), &ref_ph);
        // Where each phoneme starts in the reference, in seconds.
        std::vector<double> ref_at;
        double at = 0;
        for (const Direct::Ph &p : ref_ph) {
            ref_at.push_back(at);
            at += p.frames * 71.0 / 11025.0;
        }
        // The same tune for the singer, a note per syllable at the lengths
        // DECtalk gave each phoneme.
        const char *vw[] = {"h AE", "p IY", "b ER TH", "d EY", "t UW", "YU"};
        const int notes[] = {10, 10, 12, 10, 15, 14};
        Phrase bd;
        bd.bpm = 60;
        bd.style.vibrato = 31;
        size_t k = 0;
        while (k < ref_ph.size() && ref_ph[k].sym == "_") ++k;
        for (int i = 0; i < 6; ++i) {
            SungNote n;
            n.midi = notes[i] + 35;
            n.velocity = 100;
            n.phonemes = split(vw[i]);
            double total = 0;
            for (size_t j = 0; j < n.phonemes.size() && k < ref_ph.size(); ++j, ++k) {
                double ms = ref_ph[k].frames * 71.0 / 11.025;
                n.durations_ms.push_back(ms);
                total += ms;
            }
            n.beats = total / 1000.0;
            bd.notes.push_back(n);
        }
        add_rest(bd);
        Sung ours;
        if (sing(0, bd, &ours)) {
            write_wav(join_path(out, "dectalk_paul_birthday.wav"), ours.samples, 1);
            PitchCurve c(bd);
            // the held vowels: iy, rr, ey, uw, yu (ae's note is the first,
            // which DECtalk's own song glides into from its speaking pitch)
            struct V {
                const char *sym;
                size_t note, ref_index;
            };
            std::vector<V> vs;
            for (size_t j = 0; j < ref_ph.size(); ++j) {
                const std::string &s = ref_ph[j].sym;
                if (s == "iy") vs.push_back({"iy", 1, j});
                if (s == "rr") vs.push_back({"rr", 2, j});
                if (s == "ey") vs.push_back({"ey", 3, j});
                if (s == "uw") vs.push_back({"uw", 4, j});
                if (s == "yu") vs.push_back({"yu", 5, j});
            }
            double worst_ref = 0, worst_ours = 0, ref_dip = 0, our_dip = 0, ref_w = 0, our_w = 0;
            for (const V &v : vs) {
                double want_hz = midi_hz(notes[v.note] + 35);
                double rt0 = ref_at[v.ref_index] + 0.12, rt1 = ref_at[v.ref_index] + ref_ph[v.ref_index].frames * 71.0 / 11025.0 - 0.04;
                // in ours, the vowel starts after the note's consonants
                const SungNote &n = bd.notes[v.note];
                double ot0 = c.note_start(v.note);
                for (size_t j = 0; j < n.phonemes.size() && !is_vowel(n.phonemes[j]); ++j)
                    ot0 += n.durations_ms[j] / 1000.0;
                ot0 += 0.12;
                double ot1 = ot0 + (rt1 - rt0);
                Held hr = held(ref, 11025, rt0, rt1, want_hz);
                Held ho = held(ours.samples, kSampleRate, ot0, ot1, want_hz);
                print_held("DECtalk", v.sym, hr, want_hz);
                print_held("singer", v.sym, ho, want_hz);
                // (iy is too short for whole cycles of the vibrato)
                if (std::string(v.sym) == "iy") continue;
                worst_ref = std::max(worst_ref, std::fabs(cents(hr.mean_hz, want_hz)));
                worst_ours = std::max(worst_ours, std::fabs(cents(ho.mean_hz, want_hz)));
                ref_dip = std::min(ref_dip, hr.dip);
                our_dip = std::min(our_dip, ho.dip);
                ref_w = std::max(ref_w, hr.wander);
                our_w = std::max(our_w, ho.wander);
            }
            // DECtalk's own glide, from the timing phrase written in DECtalk's
            // syntax: A3 and E4 a quarter second each
            std::vector<float> tref;
            direct.wave("[:phoneme arpabet speak on][:np][_<200,22>'aa<250,22>'aa<250,29>'aa<250,22>"
                        "'aa<250,29>'aa<250,22>_<200>]",
                        join_path(out, "dectalk_reference_timing.wav"), &tref);
            Track ttr = periods(tref, 11025, 300.0);
            double rg = 0;
            for (int i = 1; i <= 4; ++i)
                rg += glide_ms(ttr, 0.2 + 0.25 * i - 0.01, i % 2 ? 220.0 : 329.63, i % 2 ? 329.63 : 220.0);
            std::printf("    DECtalk's own glide between A3 and E4, 10%%-90%%: %.1f ms\n", rg / 4);
            if (rg / 4 < 70 || rg / 4 > 95) ++failures;
            std::printf("    worst pitch: DECtalk %.1f cents, singer %.1f; most wander: %.2f Hz and %.2f; "
                        "deepest dip: %.1f dB and %.1f\n", worst_ref, worst_ours, ref_w, our_w, ref_dip, our_dip);
            if (worst_ours > 8 || our_w > ref_w + 0.5 || our_dip < ref_dip - 1.5) ++failures;
        }

        // -- where the note number goes: notes of "s t l aa" between A3 and
        //    E4, the number on the note's first phoneme (as the singer does),
        //    on every phoneme, or on the vowel only; how far the pitch still
        //    is from the note 20 ms into the vowel --
        {
            const char *how[3] = {"on the note's first phoneme", "on every phoneme", "on the vowel only"};
            for (int mode = 0; mode < 3; ++mode) {
                std::string t = "[:phoneme arpabet speak on][:np][_<128,22>";
                for (int i = 0; i < 6; ++i) {
                    std::string n = i % 2 ? "29" : "22";
                    const char *ph[4] = {"s", "t", "ll", "'aa"};
                    const int ms[4] = {70, 50, 60, 320};
                    for (int j = 0; j < 4; ++j) {
                        bool num = mode == 0 ? j == 0 : mode == 1 ? true : j == 3;
                        t += std::string(ph[j]) + "<" + std::to_string(ms[j]) + (num ? "," + n : "") + ">";
                    }
                }
                t += "_<64>]";
                std::vector<float> w;
                direct.wave(t, join_path(out, "dectalk_reference_placement.wav"), &w);
                double worst = 0;
                for (int i = 1; i < 6; ++i) {
                    double fb = i % 2 ? 329.63 : 220.0;
                    // the lead is 20 frames; each note 500 ms; the vowel 180 ms in
                    double tv = 20 * 71.0 / 11025 + 0.5 * i + 0.18 + 0.02;
                    double f = f0_between(w, std::lround((tv - 0.012) * 11025), std::lround((tv + 0.012) * 11025),
                                          150, 400, 11025);
                    worst = std::max(worst, f > 0 ? std::fabs(cents(f, fb)) : 9999.0);
                }
                std::printf("    note number %-28s: 20 ms into the vowel, up to %.0f cents from the note\n",
                            how[mode], worst);
            }
        }

        // -- long diphthongs, both ways --
        Sung d;
        if (sing(0, diph, &d)) {
            write_wav(join_path(out, "dectalk_paul_diphthongs.wav"), d.samples, 1);
            PitchCurve c(diph);
            std::vector<float> ours11 = to_11025(d.samples, kSampleRate);
            std::vector<float> r1, r2;
            // the same lengths, as DECtalk's own songs would write them
            int dms = int(std::lround(syllable_lengths(split("d EY"), 2500)[0]));
            int mms = int(std::lround(syllable_lengths(split("m AY"), 2500)[0]));
            direct.wave("[:phoneme arpabet speak on][:np][_<200,22>d<" + std::to_string(dms) + ",22>'ey<" +
                            std::to_string(2500 - dms) + ",22>_<300>]",
                        join_path(out, "dectalk_reference_day.wav"), &r1);
            direct.wave("[:phoneme arpabet speak on][:np][_<200,22>m<" + std::to_string(mms) + ",22>'ay<" +
                            std::to_string(2500 - mms) + ",22>_<300>]",
                        join_path(out, "dectalk_reference_my.wav"), &r2);
            struct D {
                const char *name;
                const std::vector<float> *ref;
                double ref_start, our_start, len;
            } ds[2] = {{"ey", &r1, 0.2 + dms / 1000.0, c.note_start(0) + dms / 1000.0, (2500 - dms) / 1000.0},
                       {"ay", &r2, 0.2 + mms / 1000.0, c.note_start(2) + mms / 1000.0, (2500 - mms) / 1000.0}};
            for (const D &dd : ds) {
                std::string a = "", b = "";
                double f2r[3] = {0, 0, 0}, f2o[3] = {0, 0, 0};
                const double at_frac[3] = {0.05, 0.5, 0.95};
                for (int i = 0; i < 3; ++i) {
                    double f1 = 0;
                    formants(*dd.ref, dd.ref_start + at_frac[i] * dd.len, &f1, &f2r[i]);
                    formants(ours11, dd.our_start + at_frac[i] * dd.len, &f1, &f2o[i]);
                }
                // and through the first half second, every 50 ms
                for (double t = 0; t <= 0.5; t += 0.05) {
                    double f1 = 0, f2 = 0;
                    char buf[16];
                    formants(*dd.ref, dd.ref_start + t, &f1, &f2);
                    std::snprintf(buf, sizeof buf, " %.0f", f2);
                    a += buf;
                    formants(ours11, dd.our_start + t, &f1, &f2);
                    std::snprintf(buf, sizeof buf, " %.0f", f2);
                    b += buf;
                }
                std::printf("    %s held %.1f s: F2 at 5%%, 50%%, 95%%: DECtalk %.0f %.0f %.0f, singer %.0f %.0f %.0f\n",
                            dd.name, dd.len, f2r[0], f2r[1], f2r[2], f2o[0], f2o[1], f2o[2]);
                std::printf("      first half second, every 50 ms: DECtalk%s\n", a.c_str());
                std::printf("                                       singer %s\n", b.c_str());
                // it must rise, as DECtalk's own does, from eh or aa towards iy
                if (f2o[2] - f2o[0] < 150 || std::fabs(f2o[2] - f2r[2]) > 120) ++failures;
            }
        }

        // -- what DECtalk sang, against what it was sent --
        std::vector<std::string> sent;
        if (FILE *f = std::fopen(text_file.c_str(), "r")) {
            std::string line;
            int ch;
            while ((ch = std::fgetc(f)) != EOF) {
                if (ch == '\n') {
                    if (!line.empty()) sent.push_back(line);
                    line.clear();
                } else {
                    line += char(ch);
                }
            }
            std::fclose(f);
        }
        int checked = 0, bad = 0;
        std::vector<std::string> subs;
        for (const std::string &text : sent) {
            std::vector<Direct::Ph> want = clause_phonemes(text), got;
            direct.log(text, join_path(out, "dectalk_clause.log"), &got);
            // DECtalk starts every clause with a frame of silence of its own
            // and ends it with a pause; a stop before a pause is released
            // into a short vowel
            size_t j = 0;
            if (!got.empty() && got[0].sym == "_" && got[0].frames == 1) j = 1;
            bool ok = true;
            std::string why_bad;
            for (size_t i = 0; i < want.size() && ok; ++i, ++j) {
                if (j < got.size() && (got[j].sym == "ax" || got[j].sym == "ix") && got[j].frames == 4 &&
                    want[i].sym == "_" && i > 0)
                    ++j;
                if (j >= got.size()) {
                    ok = false;
                    why_bad = "ran out at " + want[i].sym;
                    break;
                }
                if (got[j].sym != want[i].sym) {
                    if (!allophone_of(want[i].sym, got[j].sym)) {
                        ok = false;
                        why_bad = "sent " + want[i].sym + ", sang " + got[j].sym;
                        break;
                    }
                    std::string s = want[i].sym + ">" + got[j].sym;
                    if (std::find(subs.begin(), subs.end(), s) == subs.end()) subs.push_back(s);
                }
                if (got[j].frames != want[i].frames) {
                    ok = false;
                    why_bad = want[i].sym + " sent " + std::to_string(want[i].frames) + " frames, sang " +
                              std::to_string(got[j].frames);
                }
            }
            for (; ok && j < got.size(); ++j)
                if (got[j].sym != "_") {
                    ok = false;
                    why_bad = "sang an extra " + got[j].sym;
                }
            ++checked;
            if (!ok) {
                ++bad;
                std::printf("  clause not sung as sent (%s):\n    %s\n", why_bad.c_str(), text.c_str());
            }
        }
        std::string s;
        for (const std::string &x : subs) s += " " + x;
        std::printf("phoneme log: %d clauses sung exactly as sent, %d not; DECtalk's own allophones:%s\n",
                    checked - bad, bad, s.empty() ? " none" : s.c_str());
        failures += bad;
    }
    std::printf("%s\n", failures ? "FAILED" : "all ok");
    return failures ? 1 : 0;
}
