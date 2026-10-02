// ssi263.cpp -- the SSI-263 speech chip (Votrax's SC-02) as a singer.
//
// The chip is the model in third_party/ssi263-speech: a line-by-line port of
// the reverse-engineered SC-02, which is handed register writes and answers
// with audio, exactly as the real part sat on a bus. Nothing here changes
// it; the one look inside (ssi263_state) is to see how much of a stop is left
// when the stop asks for what follows. The singer plays the host -- the way the
// Robovox did for Kraftwerk, and SSInger (third_party/ssi263-speech/SSInger)
// does again from MIDI -- and writes the five registers as a song goes by:
//
//   R0  the phoneme, and two bits of duration
//   R1  the inflection (pitch), bits I10..I3
//   R2  the speech rate in the top four bits; I11 and I2..I0 below
//   R3  power, the articulation rate and the amplitude
//   R4  the filter clock, which is the length of the vocal tract
//
// The chip runs in its mode 2: every phoneme has its own length and the
// 12-bit inflection takes effect at once. The pitch is written every
// 32 samples from the phrase's pitch curve, so bends, glides and vibrato are
// the curve's own; between the chip's pitch steps (a few cents apart in the
// singing range) the written value is dithered so that the pitch heard is
// the one asked for, not the nearest step.
//
// Time is kept by the host, not the chip. Each phoneme is written at the
// sample the score puts it at, and a vowel simply goes on sounding until the
// next one is written -- the SC-02 holds its last phoneme, so a long note
// needs nothing more. The stops are the exception: a stop's closure and its
// release burst are timed by the chip from the phoneme's own duration, and
// the chip asks for the following phoneme a little before the stop ends so
// that it can release into it. So a stop is loaded with the speech rate and
// duration bits that make its length the one the score gives it, and the next
// phoneme is written when the chip asks, which is what the chip's own
// programs did.
//
// The amplitude register stays at full; a note's velocity and the part's
// level are applied to the samples, and a look-ahead limiter catches the odd
// note whose harmonic lands on one of the chip's very narrow formants.
#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

extern "C" {
#include "ssi263.h"
}

namespace svs {

namespace {

// The chip's phonemes, by their ROM codes (the mnemonics of the SC-02 data
// sheet, with the sample word it gives for each).
enum Code : int {
    PA = 0x00,    // pause
    E = 0x01,     // mEEt
    E1 = 0x02,    // bEnt
    Y = 0x03,     // bEfore
    YI = 0x04,    // Year
    AY = 0x05,    // plEAse
    IE = 0x06,    // anY
    I = 0x07,     // sIx
    A = 0x08,     // mAde
    AI = 0x09,    // cAre
    EH = 0x0A,    // nEst
    EH1 = 0x0B,   // bElt
    AE = 0x0C,    // dAd
    AE1 = 0x0D,   // After
    AH = 0x0E,    // gOt
    AH1 = 0x0F,   // fAther
    AW = 0x10,    // Office
    O = 0x11,     // stOre
    OU = 0x12,    // bOAt
    OO = 0x13,    // lOOk
    IU = 0x14,    // yOU
    IU1 = 0x15,   // cOUld
    U = 0x16,     // tUne
    U1 = 0x17,    // cartOOn
    UH = 0x18,    // wOnder
    UH1 = 0x19,   // lOve
    UH2 = 0x1A,   // whAt
    UH3 = 0x1B,   // nUt
    ER = 0x1C,    // bIRd
    R = 0x1D,     // Roof
    R1 = 0x1E,    // Rug
    R2 = 0x1F,    // mutteR
    L = 0x20,     // Lift
    L1 = 0x21,    // pLay
    LF = 0x22,    // falL
    W = 0x23,     // Water
    B = 0x24,     // Bag
    D = 0x25,     // paiD
    KV = 0x26,    // taG
    P = 0x27,     // Pen
    T = 0x28,     // Tart
    K = 0x29,     // Kit
    HV = 0x2A,    // a voiced h
    HVC = 0x2B,   // a voiced closure, held
    HF = 0x2C,    // Heart
    HFC = 0x2D,   // a voiceless closure
    HN = 0x2E,    // a nasal h
    Z = 0x2F,     // Zero
    S = 0x30,     // Same
    J = 0x31,     // meaSure
    SCH = 0x32,   // SHip
    V = 0x33,     // Very
    F = 0x34,     // Four
    THV = 0x35,   // THere
    TH = 0x36,    // wiTH
    M = 0x37,     // More
    N = 0x38,     // NiNe
    NG = 0x39,    // raNG
};

// How one of VocalWriter's phonemes becomes the chip's: one phoneme, or two
// that share its time.
enum Shape {
    kOne,     // `first` for the whole length
    kTail,    // `first` held, `second` a glide over the end (diphthongs, r-colouring)
    kHead,    // `second` held after a short `first` (the y of YU)
    kSplit,   // `first` takes `share` of the length, `second` the rest
};

struct Mapping {
    const char *symbol;
    Shape shape;
    int first;
    int second;
    double share;
};

// VocalWriter's fifty-seven phonemes in the chip's terms.
//
// The vowels go to the chip vowel whose sample word has the same sound.
// The diphthongs hold their first vowel and glide into the second over the
// last stretch of the note, as a singer holds "ah" and closes to "ee" only at
// the end of "my"; the r-coloured vowels do the same into the chip's ER. The
// chip has a G that is only a voiced release (KV), so g is D's closure
// released into it. The affricates are a stop and a fricative, and the
// glottal stops are the chip's own held closure. The rest is silence, PA.
const Mapping kMap[] = {
    // vowels
    {"IY", kOne, E, 0, 0},           // bEEt
    {"IH", kOne, I, 0, 0},           // bIt
    {"EH", kOne, EH, 0, 0},          // bEt
    {"AE", kOne, AE, 0, 0},          // bAt
    {"AA", kOne, AH1, 0, 0},         // hOt, fAther
    {"UX", kOne, UH3, 0, 0},         // bUd
    {"AO", kOne, AW, 0, 0},          // cAUght
    {"UH", kOne, OO, 0, 0},          // bOOk
    {"AX", kOne, UH, 0, 0},          // About
    {"IX", kOne, I, 0, 0},           // rosEs
    {"O", kOne, O, 0, 0},            // bOy's first half
    {"RX", kOne, ER, 0, 0},          // a short r-coloured vowel
    // diphthongs: the nucleus held, the glide at the end
    {"EY", kTail, A, Y, 0},          // bAIt
    {"AY", kTail, AH1, Y, 0},        // bIte
    {"OY", kTail, O, Y, 0},          // bOY
    {"AW", kTail, AH1, U1, 0},       // bOUt
    {"OW", kTail, OU, U1, 0},        // bOAt
    {"UW", kOne, U1, 0, 0},          // bOOt
    {"YU", kHead, Y, U1, 0},         // mUte: the y short, then oo
    // r-coloured vowels
    {"ER", kTail, ER, R1, 0},        // bIRd
    {"IR", kTail, E, ER, 0},         // bEER
    {"XR", kTail, AI, ER, 0},        // bEAR
    {"AR", kTail, AH1, ER, 0},       // bAR
    {"OR", kTail, O, ER, 0},         // fOR
    {"UR", kTail, OO, ER, 0},        // pOOR
    // syllabic consonants: a little of the vowel, then the consonant
    {"EL", kSplit, UH, LF, 0.5},     // bottLE
    {"EN", kSplit, UH, N, 0.5},      // buttON
    {"LX", kOne, LF, 0, 0},          // beLL
    // glides and liquids
    {"w", kOne, W, 0, 0},
    {"y", kOne, Y, 0, 0},
    {"r", kOne, R, 0, 0},
    {"l", kOne, L, 0, 0},
    {"h", kOne, HF, 0, 0},
    // nasals
    {"m", kOne, M, 0, 0},
    {"n", kOne, N, 0, 0},
    {"NG", kOne, NG, 0, 0},
    // fricatives
    {"f", kOne, F, 0, 0},
    {"v", kOne, V, 0, 0},
    {"TH", kOne, TH, 0, 0},
    {"DH", kOne, THV, 0, 0},
    {"s", kOne, S, 0, 0},
    {"z", kOne, Z, 0, 0},
    {"SH", kOne, SCH, 0, 0},
    {"ZH", kOne, J, 0, 0},
    // stops
    {"p", kOne, P, 0, 0},
    {"b", kOne, B, 0, 0},
    {"t", kOne, T, 0, 0},
    {"d", kOne, D, 0, 0},
    {"k", kOne, K, 0, 0},
    {"g", kSplit, D, KV, 0.6},       // a closure released into the chip's G
    {"TX", kOne, T, 0, 0},           // iT
    {"DD", kOne, D, 0, 0},           // the flap of beTTer
    {"Q", kOne, HVC, 0, 0},          // glottal stop
    {"QX", kOne, HVC, 0, 0},
    // affricates
    {"CH", kSplit, T, SCH, 0.4},     // CHin
    {"JH", kSplit, D, J, 0.4},       // Joy
    // silence
    {"%", kOne, PA, 0, 0},
};

const Mapping *find_mapping(const std::string &symbol) {
    std::string s = palette_alias(symbol);
    for (const Mapping &m : kMap)
        if (s == m.symbol) return &m;
    return nullptr;
}

// A diphthong's glide takes about a third of the note, but never less than
// what can be heard as a glide nor more than a singer spends closing a long
// note; the y of YU is short whatever the length.
const double kTailShare = 0.3, kTailMin = 0.030, kTailMax = 0.120;
const double kHeadShare = 0.25, kHeadMin = 0.020, kHeadMax = 0.070;

// The voices: one chip, four vocal tracts. The filter clock (R4) scales
// every resonance together, as a longer or shorter tract would; the
// articulation rate is how quickly the formants move from one phoneme to the
// next. 0xE4 is the clock SSInger and the speech boxes use (about 17.9 kHz);
// Deep's tract is about a quarter longer, Child's nearly a third shorter.
// All four come out equally loud: every vowel on every note from G2 to C6 at
// full velocity peaks at 0.45 to 0.5 (the median), 0.8 for one in ten, which
// is left as it is and kept from clipping by the limiter below.
struct Preset {
    int offset;
    const char *name;
    int ff;              // R4
    int articulation;    // R3's articulation rate, 0-7
};

const Preset kPresets[] = {
    {0, "SSI-263", 0xE4, 5},          // 17.9 kHz
    {1, "SSI-263 Deep", 0xDC, 4},     // 13.9 kHz, and slower to move
    {2, "SSI-263 Bright", 0xE8, 6},   // 20.8 kHz, and quicker
    {3, "SSI-263 Child", 0xEC, 6},    // 25.0 kHz
};

// The speech rate used for everything but a stop's own duration. The rate
// also sets how fast the formants travel (so many codes a frame): at 13 a
// frame is 12.3 ms and a full sweep of the second formant about 45 ms, close
// to the transitions of sung consonants.
const int kBaseRate = 13;
// Mode 2: phoneme timing from R0's duration bits, inflection at once.
const int kMode = 2;
const int kAmplitude = 15;
// Every phoneme is written this much ahead of its time. A vowel does not
// stop dead when the next phoneme is written: its voicing fades over a few
// milliseconds and its narrow first formant rings on after that, so that
// heard, a vowel ends some 6 ms after the write. Writing 3 ms early puts the
// change of sound where the score has it.
const double kLead = 0.003;
// How often the pitch is written and the stops' requests looked at.
const long kChunk = 32;
// The most a phrase may reach.
const double kCeiling = 0.89;

struct Segment {
    long start;      // sample
    int code;
    bool closing;    // a stop: the chip closes the tract and times its release
};

// The chip's master clock and frame, from its parameters.
struct ChipClock {
    double xck = 1e6;
    double frame_cycles = 4096.0;
};

// Pick the speech rate and duration bits that give a stop the nearest length
// to `seconds` without running past it by more than a millisecond (a stop
// that runs over holds the next phoneme back). Its length is
// frame(rate) * (4 - DUR), frame = 4096 (16 - rate) / XCK.
void fit_duration(const ChipClock &clk, double seconds, int *rate, int *dur) {
    double best = 1e9;
    *rate = 15;
    *dur = 3;
    for (int m = 1; m <= 16; ++m)
        for (int k = 4; k >= 1; --k) {
            double d = clk.frame_cycles * m * k / clk.xck;
            if (d > seconds + 0.001) continue;
            double e = std::fabs(d - seconds);
            if (e < best - 1e-9) {
                best = e;
                *rate = 16 - m;
                *dur = 4 - k;
            }
        }
}

// The chip's formants are narrow, and when a harmonic of the sung note
// lands on one a vowel can come out at twice its usual level. Rather than
// turn every voice down for those few notes, the phrase goes through a
// limiter that sees them coming: the gain each sample needs to stay under
// the ceiling, held over a few milliseconds either side, eased down ahead of
// the peak and back up after it. Nothing else is touched.
void limit(std::vector<double> &x, double ceiling) {
    const size_t n = x.size();
    const long hold = long(0.003 * kSampleRate);
    std::vector<double> need(n, 1.0);
    bool any = false;
    for (size_t i = 0; i < n; ++i)
        if (std::fabs(x[i]) > ceiling) {
            need[i] = ceiling / std::fabs(x[i]);
            any = true;
        }
    if (!any) return;
    // the lowest need within `hold` of each sample (only near the peaks)
    std::vector<double> g(need);
    for (size_t i = 0; i < n; ++i)
        if (need[i] < 1.0) {
            size_t a = size_t(std::max(0L, long(i) - hold)), b = std::min(n, i + size_t(hold) + 1);
            for (size_t j = a; j < b; ++j) g[j] = std::min(g[j], need[i]);
        }
    // back up over 60 ms after, down over 3 ms before: each pass can only
    // lower the gain, so no sample ends above the ceiling
    const double up = 1.0 - std::exp(-1.0 / (0.060 * kSampleRate));
    const double down = 1.0 - std::exp(-1.0 / (0.003 * kSampleRate));
    for (size_t i = 1; i < n; ++i) g[i] = std::min(g[i], g[i - 1] + (1.0 - g[i - 1]) * up);
    for (size_t i = n - 1; i-- > 0;) g[i] = std::min(g[i], g[i + 1] + (1.0 - g[i + 1]) * down);
    for (size_t i = 0; i < n; ++i) x[i] *= g[i];
}

class Ssi263Singer : public Singer {
public:
    explicit Ssi263Singer(const Preset &p) : preset_(p) {}
    Sung sing(const Phrase &phrase) override;

private:
    const Preset &preset_;
};

// The chip state the host keeps beside it: what was last written to R2's
// rate and the pitch, so that a pitch write keeps the rate and the other way
// round.
struct Host {
    ssi263 *chip = nullptr;
    ChipClock clk;
    int rate = kBaseRate;
    int infl = 0;
    double dither = 0.0;

    void write(int addr, int v) { ssi263_write(chip, addr, v); }
    void write_r2() { write(2, (rate << 4) | (((infl >> 11) & 1) << 3) | (infl & 7)); }
    void write_pitch(int i) {
        infl = std::max(0, std::min(4095, i));
        write(1, (infl >> 3) & 0xFF);
        write_r2();
    }
    // F0 = XCK / (8 (4096 - I)), so I = 4096 - XCK / (8 F0); the fraction
    // left over is carried to the next write, so that the pitch heard
    // averages to the one wanted.
    void set_hz(double hz) {
        double want = 4096.0 - clk.xck / (8.0 * std::max(hz, 1.0));
        double v = want + dither;
        int i = int(std::lround(v));
        i = std::max(0, std::min(4095, i));
        dither = std::max(-1.0, std::min(1.0, v - i));
        if (i != infl) write_pitch(i);
    }
    // Load a phoneme. A stop is given its length through the rate and
    // duration bits, which the chip reads as the phoneme loads; the rate then
    // goes back at once, so the formants keep their speed.
    void load(int code, bool closing, double seconds) {
        if (closing) {
            int r, d;
            fit_duration(clk, seconds, &r, &d);
            rate = r;
            write_r2();
            write(0, (d << 6) | code);
            rate = kBaseRate;
            write_r2();
        } else {
            write(0, (3 << 6) | code);
        }
    }
};

Sung Ssi263Singer::sing(const Phrase &phrase) {
    Sung sung;
    const double sr = kSampleRate;
    PitchCurve curve(phrase);
    long total = long(std::llround(curve.total() * sr));
    if (total <= 0) return sung;

    ssi263_params params;
    ssi263_default_params(&params);
    const unsigned char *rom = ssi263_default_rom();
    // The phonemes that close the tract are the ones whose ROM entry has its
    // closure-clear flag (the seventh byte) off: B D P T K, and the held
    // closures HVC and HFC, which close without a release.
    auto closing = [&](int code) { return rom[9 * code + 6] == 0; };

    // -- the score as chip phonemes at sample positions --
    std::vector<Segment> segs;
    auto add = [&](double seconds, int code) {
        long at = std::max(0L, long(std::llround((seconds - kLead) * sr)));
        // a phoneme rounded away to nothing is dropped; the same phoneme
        // twice in a row (a vowel tied across notes) is simply held
        if (!segs.empty() && segs.back().start >= at) segs.pop_back();
        if (!segs.empty() && segs.back().code == code && !closing(code)) return;
        segs.push_back({at, code, closing(code)});
    };
    for (size_t i = 0; i < phrase.notes.size(); ++i) {
        const SungNote &note = phrase.notes[i];
        double t0 = curve.note_start(i), len = curve.note_end(i) - t0;
        std::vector<double> ms = note.durations_ms;
        if (ms.size() != note.phonemes.size())
            ms = syllable_lengths(note.phonemes, len * 1000.0);
        double sum = 0.0;
        for (double v : ms) sum += std::max(0.0, v);
        if (note.phonemes.empty() || sum <= 0.0) {
            add(t0, PA);
            continue;
        }
        // the lengths are stretched or squeezed to fill the note exactly
        double k = len / (sum / 1000.0), at = t0;
        for (size_t j = 0; j < note.phonemes.size(); ++j) {
            double d = std::max(0.0, ms[j]) / 1000.0 * k;
            const Mapping *m = find_mapping(note.phonemes[j]);
            if (!m) {
                add(at, PA);
            } else if (m->shape == kOne) {
                add(at, m->first);
            } else {
                double a;
                if (m->shape == kTail)
                    a = d - std::min(0.5 * d, std::max(kTailMin, std::min(kTailMax, kTailShare * d)));
                else if (m->shape == kHead)
                    a = std::min(0.5 * d, std::max(kHeadMin, std::min(kHeadMax, kHeadShare * d)));
                else
                    a = m->share * d;
                add(at, m->first);
                add(at + a, m->second);
            }
            at += d;
        }
    }
    if (segs.empty() || segs.front().start > 0) segs.insert(segs.begin(), Segment{0, PA, false});

    // -- loudness: the note's velocity, smoothed; a rest keeps the last --
    std::vector<float> gain(size_t(total), 1.0f);
    {
        double g = -1.0;
        const double tc = 1.0 - std::exp(-1.0 / (0.008 * sr));
        double cur = -1.0;
        for (long s = 0; s < total; ++s) {
            size_t i = curve.note_at(s / sr);
            const SungNote &n = phrase.notes[i];
            bool rest = n.phonemes.empty() ||
                        (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
            if (!rest || g < 0) g = std::max(1, std::min(127, n.velocity)) / 127.0;
            cur = cur < 0 ? g : cur + (g - cur) * tc;
            gain[size_t(s)] = float(cur);
        }
    }
    // The voice control "color" moves the vocal tract a little either side
    // of the preset: VocalWriter's default (95) leaves it where it is.
    int ff = preset_.ff + int(std::lround((phrase.style.color - 95) / 16.0));
    ff = std::max(0xC0, std::min(0xF4, ff));

    // -- power up, as a host does: mode in R0, then CTL 1 -> 0 --
    Host h;
    h.clk.xck = params.xck_hz;
    h.clk.frame_cycles = params.frame_xck_cycles;
    h.chip = ssi263_new(&params, rom, sr);
    if (!h.chip) {
        sung.stopped_short = true;
        return sung;
    }
    h.write(3, 0x80);
    h.write(0, kMode << 6);
    h.infl = int(std::lround(4096.0 - h.clk.xck / (8.0 * curve.hz_at(0.0))));
    h.write(1, (h.infl >> 3) & 0xFF);
    h.write_r2();
    h.write(4, ff);
    h.write(3, ((preset_.articulation & 7) << 4) | kAmplitude);
    // A moment of pause before the first note lets the amplitude, which
    // rises from nothing at power-up, reach its level unheard.
    h.write(0, (3 << 6) | PA);
    // (A skip stops where the phoneme's time runs out, so it may take more
    // than one.)
    for (int k = 0; k < 8 && ssi263_time(h.chip) < 0.03; ++k)
        ssi263_skip(h.chip, 0.03 - ssi263_time(h.chip));

    // -- run --
    std::vector<double> out(size_t(total), 0.0);
    std::vector<double> buf(size_t(kChunk) + 1);
    const double level = phrase.level;
    auto seconds_of = [&](size_t k) {
        long end = k + 1 < segs.size() ? segs[k + 1].start : total;
        return (end - segs[k].start) / sr;
    };
    size_t w = 0;          // the last segment written
    h.load(segs[0].code, segs[0].closing, seconds_of(0));
    long pos = 0;
    while (pos < total) {
        // Phonemes due now.
        while (w + 1 < segs.size() && segs[w + 1].start <= pos) {
            ++w;
            h.load(segs[w].code, segs[w].closing, seconds_of(w));
        }
        // A stop asks for what follows before it ends, so that it can
        // release into it; the write waits inside the chip until the stop's
        // time is up. Only taken when that is when the score wants it.
        // (ssi263_state's order is listed above it in ssi263.c: elapsed and
        // duration first, timer_done 12th, early_req 28th, from 0.)
        double st[32];
        ssi263_state(h.chip, st, 32);
        bool waiting = st[28] != 0.0 && st[12] == 0.0;     // early_req, not timer_done
        if (w + 1 < segs.size() && segs[w].closing && waiting &&
            ssi263_request(h.chip)) {
            double left = st[1] - st[0];                    // duration - elapsed
            double until = (segs[w + 1].start - pos) / sr;
            if (std::fabs(left - until) <= 0.004) {
                h.load(segs[w + 1].code, segs[w + 1].closing, seconds_of(w + 1));
                ++w;
                waiting = true;
            }
        }
        long next = total;
        if (w + 1 < segs.size()) next = std::min(next, segs[w + 1].start);
        long n = std::max(1L, std::min(kChunk, next - pos));
        n = std::min(n, total - pos);
        // The pitch at the middle of the coming chunk. While writes wait in
        // the chip behind a stop they would only queue, so none are made.
        if (!waiting) h.set_hz(curve.hz_at((pos + 0.5 * n) / sr));
        long got = ssi263_run(h.chip, n, buf.data());
        for (long s = 0; s < got && pos + s < total; ++s)
            out[size_t(pos + s)] = buf[size_t(s)] * level * gain[size_t(pos + s)];
        pos += n;
    }
    ssi263_free(h.chip);
    limit(out, kCeiling);
    sung.samples.assign(out.begin(), out.end());
    return sung;
}

class Ssi263Family : public Family {
public:
    std::string name() const override { return "SSI-263"; }
    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> v;
        for (const Preset &p : kPresets) v.push_back({kSsi263Base + p.offset, p.name});
        return v;
    }
    // The chip and its ROM are compiled in: nothing to look for.
    bool available(std::string *why) override {
        if (why) why->clear();
        return true;
    }
    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        for (const Preset &p : kPresets)
            if (kSsi263Base + p.offset == voice_id) return std::make_unique<Ssi263Singer>(p);
        if (err) *err = "There is no SSI-263 voice numbered " + std::to_string(voice_id) + ".";
        return nullptr;
    }
};

}  // namespace

std::unique_ptr<Family> make_ssi263_family() { return std::make_unique<Ssi263Family>(); }

}  // namespace svs
