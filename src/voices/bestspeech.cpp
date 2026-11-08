// bestspeech.cpp -- BeSTspeech's voices, singing.
//
// BeSTspeech is Berkeley Speech Technologies' engine, the one HumanWare's
// Keynote GOLD spoke with, as third_party/openbst reimplements it from the
// original binaries, sample for sample. It is a diphone synthesiser: each
// pair of sounds names a stretch of stored analysis, and the frames made
// from them drive a tenth-order lattice filter, one or more pitch periods to
// a frame. The program sings with its 1995 build, English, whose tables are
// compiled in (cmake/engines/bestspeech.cmake), so it needs no data.
//
// The engine has no way to be told how long a sound lasts or what pitch it
// is on, so it is told neither. A phrase is written as a stream of its sound
// codes, as its assembler writes one -- every note a stressed word of its
// own -- and run through its own stages to frames (bestspeech_port.c). Each
// frame says which diphone it came from, and where in a diphone the sound
// changes (voicing starting or stopping, else its loudness, else its middle)
// is where one sound ends and the next begins. Then the frames are laid out again: each
// sound is stretched or squeezed to the length the studio gives it, a held
// vowel's transitions kept at their own speed and its middle drawn out, and
// every voiced frame is played for exactly one pitch period, the period the
// studio's pitch curve asks for at that moment, in sixteenths of a sample.
// A period's pulse carries the same energy whatever its length, so each
// frame's gain is moved by half the change in period, which keeps a vowel
// as loud at C5 as it was spoken.
//
// The engine says one phrase in at most about four hundred bytes of codes,
// so a long phrase is sung in pieces, which meet at a note. A rest ends a
// piece, and the engine's own fall into silence is heard in it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/phonology.h"
#include "voices/bestspeech_port.h"
#include "voices/family.h"
#include "voices/pitch.h"

namespace svs {

namespace {

const char *const kBuild = "1995";
constexpr int kEngineRate = 11025;
//: the build's voices are its table's slots 1 to 6, the one it starts on
//: being 1; slot 0 is no voice, its coefficients unstable
constexpr int kVoiceCount = 6;
//: a piece's stream, in bytes, short of what the engine allows a phrase
constexpr size_t kPieceBytes = 300;
//: the gain table's steps: sixteen of them are a factor of two
constexpr int kGainSteps = 16;
//: headroom: 6 dB down in the lattice, given back in floats
constexpr int kGainCut = 16;
constexpr double kCutBack = 2.0;
//: how much of a held sound's either end keeps its own speed
constexpr double kEdge = 0.03;
//: the shortest and longest periods sung, in sixteenths of a sample
constexpr int kMinPeriod = 11 * 16, kMaxPeriod = 0xFFF;
//: a silence no longer than this, in samples, is one of the engine's own
//: frames between two sounds, and the sound after it is brought in over
//: kJoinFade samples; a stop's closure is longer, and its release keeps
//: its edge
constexpr size_t kJoinGap = 165, kJoinFade = 33;

// The voices are evened out as the other engines' are: each was sung through
// ten vowels on notes across its range and scaled so that its median peak
// is 0.5 (bestspeech_sing_test --levels).
const double kGains[kVoiceCount] = {0.99, 1.15, 0.90, 0.96, 1.26, 0.92};

// The sound codes, as BeSTspeech's English streams number them.
enum : uint8_t {
    F = 0x01, TH = 0x02, SH = 0x03, V = 0x04, DH = 0x05, Z = 0x06, ZH = 0x07, W = 0x08, Y = 0x09,
    H = 0x0A, CH = 0x0B, JH = 0x0C, S = 0x0D, M = 0x0E, N = 0x0F, NG = 0x10, L = 0x11, R = 0x12,
    B = 0x13, D = 0x14, G = 0x15, P = 0x16, P_ = 0x17, T = 0x18, DX = 0x19, T_ = 0x1B, K = 0x1C,
    K_ = 0x1D, EY = 0x1E, AY = 0x1F, OY = 0x20, AW = 0x21, OW = 0x22, IY = 0x23, AX = 0x24,
    AXL = 0x25, EH = 0x26, AA = 0x27, AO = 0x28, IH = 0x29, UH = 0x2A, AH = 0x2B, UW = 0x2C,
    AE = 0x2D, ER = 0x2E, PAUSE = 0x2F,
};

// VocalWriter's fifty-seven phonemes as BeSTspeech's sounds. An r-coloured
// vowel is its vowel and r, as BeSTspeech spells "air" and "car"; YU is y and
// UW; syllabic l and n are the reduced vowels it says them with and the
// consonant. Each part's share of the phoneme's time is given beside it. A
// stop is released at the start of a syllable and not at its end, as
// BeSTspeech's own p, t and k are; TX, the t of "it", is the unreleased one,
// and the glottal stops are too. O, the first part of OY, is AO.
struct Part {
    uint8_t code;
    double share;
};
struct PhoneMap {
    const char *vw;
    Part parts[2];
    uint8_t coda;      //: the code at the end of a syllable, if not the same
};
const PhoneMap kPhoneMap[] = {
    {"IY", {{IY, 1}}, 0},           {"IH", {{IH, 1}}, 0},           {"EH", {{EH, 1}}, 0},
    {"AE", {{AE, 1}}, 0},           {"AA", {{AA, 1}}, 0},           {"UX", {{AH, 1}}, 0},
    {"AO", {{AO, 1}}, 0},           {"UH", {{UH, 1}}, 0},           {"AX", {{AX, 1}}, 0},
    {"ER", {{ER, 1}}, 0},           {"EY", {{EY, 1}}, 0},           {"AY", {{AY, 1}}, 0},
    {"OY", {{OY, 1}}, 0},           {"AW", {{AW, 1}}, 0},           {"OW", {{OW, 1}}, 0},
    {"UW", {{UW, 1}}, 0},           {"YU", {{Y, 0.25}, {UW, 0.75}}, 0},
    {"IR", {{IH, 0.7}, {R, 0.3}}, 0}, {"XR", {{EH, 0.7}, {R, 0.3}}, 0},
    {"AR", {{AA, 0.7}, {R, 0.3}}, 0}, {"OR", {{AO, 0.7}, {R, 0.3}}, 0},
    {"UR", {{UH, 0.7}, {R, 0.3}}, 0}, {"IX", {{AX, 1}}, 0},           {"%", {}, 0},
    {"RX", {{ER, 1}}, 0},           {"LX", {{L, 1}}, 0},            {"EL", {{AXL, 0.4}, {L, 0.6}}, 0},
    {"EN", {{AX, 0.4}, {N, 0.6}}, 0}, {"w", {{W, 1}}, 0},           {"y", {{Y, 1}}, 0},
    {"r", {{R, 1}}, 0},             {"l", {{L, 1}}, 0},             {"h", {{H, 1}}, 0},
    {"m", {{M, 1}}, 0},             {"n", {{N, 1}}, 0},             {"NG", {{NG, 1}}, 0},
    {"f", {{F, 1}}, 0},             {"v", {{V, 1}}, 0},             {"TH", {{TH, 1}}, 0},
    {"DH", {{DH, 1}}, 0},           {"s", {{S, 1}}, 0},             {"z", {{Z, 1}}, 0},
    {"SH", {{SH, 1}}, 0},           {"ZH", {{ZH, 1}}, 0},           {"p", {{P, 1}}, P_},
    {"b", {{B, 1}}, 0},             {"t", {{T, 1}}, T_},            {"d", {{D, 1}}, 0},
    {"k", {{K, 1}}, K_},            {"g", {{G, 1}}, 0},             {"CH", {{CH, 1}}, 0},
    {"JH", {{JH, 1}}, 0},           {"TX", {{T_, 1}}, 0},           {"Q", {{T_, 1}}, 0},
    {"QX", {{T_, 1}}, 0},           {"DD", {{DX, 1}}, 0},           {"O", {{AO, 1}}, 0},
};

const PhoneMap *find_map(const std::string &symbol) {
    std::string s = palette_alias(symbol);   // OH and DX, as VocalWriter's MIDI files spell them
    for (const PhoneMap &m : kPhoneMap)
        if (s == m.vw) return &m;
    return nullptr;
}

//: whether a sound is one the engine says unstressed
bool reduced(uint8_t code) { return code == AX || code == AXL; }
//: whether a sound is a vowel, which the stream follows with its stress
bool vowel(uint8_t code) { return code >= EY && code <= ER; }

//: One sound to be sung, from `start` to `end` in seconds from the phrase's
//: start; a rest has no code.
struct Sound {
    int code = -1;
    double start = 0, end = 0;
};

//: A piece: its sounds, and its stream with the header's place in it.
struct Piece {
    std::vector<Sound> sounds;
    std::vector<uint8_t> stream;
    int hdr = 0;
};

//: what the engine made of a piece: its frames, each with when it starts in
//: seconds, and where each of the piece's sounds starts, with the place its
//: fall into silence starts at the end
struct Spoken {
    std::vector<uint8_t> frames;
    std::vector<double> at;
    std::vector<double> starts;
    double total = 0;
};

// The engine's frame generator reports to a global hook.
std::mutex g_engine;

int frame_mode(const uint8_t *f) { return f[0] & 0x30; }
int frame_period(const uint8_t *f) { return (f[3] << 4) | (f[1] & 0x0F); }
double frame_length(const uint8_t *f) { return (f[0] & 0x0F) * frame_period(f) / 16.0 / kEngineRate; }

class BestSinger : public Singer {
public:
    explicit BestSinger(int v) : v_(v) { h_ = bst_open(kBuild); }

    //: the build's own number for the voice
    int slot() const { return v_ + 1; }
    ~BestSinger() override {
        if (h_) bst_close(h_);
    }

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (!h_) {
            sung.error = "BeSTspeech could not be started";
            return sung;
        }
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        std::vector<Piece> pieces = lay_out(phrase, curve);

        std::unique_ptr<svs_bst_lattice, void (*)(svs_bst_lattice *)> lattice(svs_bst_lattice_new(h_),
                                                                            svs_bst_lattice_free);
        if (!lattice) {
            sung.error = "BeSTspeech could not be started";
            return sung;
        }
        std::vector<int16_t> pcm;
        for (size_t p = 0; p < pieces.size(); ++p) {
            const Piece &piece = pieces[p];
            if (piece.stream.empty()) {
                // a rest: what the piece before it left ringing, then silence
                silence(piece.sounds.front().end, &pcm);
                continue;
            }
            Spoken said;
            if (!speak(piece, &said)) {
                sung.error = "BeSTspeech could not say a phrase";
                return sung;
            }
            bool fall = p + 1 < pieces.size() && pieces[p + 1].stream.empty();
            sing_piece(piece, said, curve, fall ? pieces[p + 1].sounds.front().end : -1, &pcm, lattice.get());
        }

        std::vector<float> wide(pcm.begin(), pcm.end());
        std::vector<float> samples = resample_to_output(wide, kEngineRate);
        // to +-1 at the part's level; a phrase that would still clip is
        // brought down whole rather than cut off
        double gain = phrase.level * kGains[v_] * kCutBack / 32768.0, peak = 0.0;
        for (float &s : samples) {
            s = float(s * gain);
            peak = std::max(peak, double(std::fabs(s)));
        }
        if (peak > 0.999) {
            float k = float(0.999 / peak);
            for (float &s : samples) s *= k;
        }
        sung.samples = std::move(samples);
        return sung;
    }

private:
    // The phrase as pieces: runs of notes between its rests, each run cut
    // where its stream would grow too long, every note a stressed word of its
    // own and the last of a piece its phrase-final word. A rest is a piece
    // with no stream.
    static std::vector<Piece> lay_out(const Phrase &phrase, const PitchCurve &curve) {
        std::vector<Piece> pieces;
        Piece cur;
        std::vector<uint8_t> words;     // the current piece's words, before its ends
        size_t last_word = 0;           // where the last word began in `words`
        auto close = [&]() {
            if (cur.sounds.empty()) return;
            // the last word ends the phrase: its stress the final accent, on
            // its stressed vowel or else its first
            size_t at = words.size();
            for (size_t i = last_word; i < words.size() && at == words.size(); ++i)
                if (words[i] == 0x36) at = i;
            for (size_t i = last_word; i < words.size() && at == words.size(); ++i)
                if (words[i] == 0x32) at = i;
            if (at < words.size()) {
                words[at + 1] = 0x43;
                words[at + 2] = 0x3D;
            }
            if (!words.empty() && words.back() == 0x4C) words.pop_back();
            static const uint8_t head[] = {0x4E, 0, 0, 0, 0x40, 0x7C, 0x49, 0, 0x04, 0x4C, 0x04, 0x7C};
            cur.stream.assign(head, head + sizeof head);
            cur.stream.insert(cur.stream.end(), words.begin(), words.end());
            cur.stream.push_back(PAUSE);
            cur.hdr = int(cur.stream.size());
            static const uint8_t tail[] = {0x4E, 0, 0, 0x3A, 0, 0x5C};
            cur.stream.insert(cur.stream.end(), tail, tail + sizeof tail);
            pieces.push_back(cur);
            cur = Piece();
            words.clear();
        };
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            const SungNote &n = phrase.notes[i];
            std::vector<Sound> sounds;
            std::vector<uint8_t> word;
            int held = held_phoneme(n.phonemes);
            double at = curve.note_start(i);
            bool marked = false;
            for (size_t k = 0; k < n.phonemes.size(); ++k) {
                double ms = k < n.durations_ms.size() ? n.durations_ms[k] : 0.0;
                const PhoneMap *m = find_map(n.phonemes[k]);
                double from = at;
                at += ms / 1000.0;
                if (!m || !m->parts[0].share) continue;
                bool said_vowel = false;
                for (int q = 0; q < 2 && m->parts[q].share; ++q) {
                    const Part &part = m->parts[q];
                    Sound s;
                    s.code = int(k) > held && m->coda ? m->coda : part.code;
                    s.start = from;
                    s.end = from += part.share * ms / 1000.0;
                    sounds.push_back(s);
                    word.push_back(uint8_t(s.code));
                    // the syllable's stress, after its vowel: the first part
                    // of an r-coloured or syllabic one, the second of YU.
                    // Every other vowel is marked unstressed: one with no
                    // mark at all takes the next word's first syllable with
                    // it, which then goes unsung.
                    if (!vowel(uint8_t(s.code)) || said_vowel) continue;
                    said_vowel = true;
                    if (int(k) == held) {
                        bool weak = reduced(part.code);
                        word.insert(word.end(), {uint8_t(weak ? 0x32 : 0x36), uint8_t(weak ? 0x40 : 0x42), 0});
                        marked = true;
                    } else {
                        word.insert(word.end(), {0x32, 0x40, 0});
                    }
                }
            }
            if (sounds.empty()) {
                // a rest
                close();
                Sound rest;
                rest.start = curve.note_start(i);
                rest.end = curve.note_end(i);
                if (!pieces.empty() && pieces.back().stream.empty()) pieces.back().sounds.front().end = rest.end;
                else {
                    Piece r;
                    r.sounds.push_back(rest);
                    pieces.push_back(r);
                }
                continue;
            }
            if (!marked) word.insert(word.end(), {0x36, 0x42, 0});
            word.push_back(0x4C);
            if (!cur.sounds.empty() && words.size() + word.size() > kPieceBytes) close();
            last_word = words.size();
            words.insert(words.end(), word.begin(), word.end());
            cur.sounds.insert(cur.sounds.end(), sounds.begin(), sounds.end());
        }
        close();
        return pieces;
    }

    // The engine's frames for a piece, and where each of its sounds starts in
    // them.
    bool speak(const Piece &piece, Spoken *said) {
        std::vector<uint8_t> stream(BST_SVS_STREAM, 0);
        std::copy(piece.stream.begin(), piece.stream.end(), stream.begin());
        const int room = 16384, segs = 0x100;
        said->frames.assign(size_t(room) * 16, 0);
        std::vector<int> frame_seg(room, 0), seg_sound(segs, -1);
        int nseg = 0, m;
        {
            std::lock_guard<std::mutex> lock(g_engine);
            m = svs_bst_frames(h_, kBuild, slot(), stream.data(), int(piece.stream.size()), piece.hdr,
                               said->frames.data(), room, frame_seg.data(), seg_sound.data(), segs, &nseg);
        }
        if (m <= 0) return false;
        said->frames.resize(size_t(m) * 16);
        said->at.resize(size_t(m));
        double t = 0;
        for (int i = 0; i < m; ++i) {
            said->at[size_t(i)] = t;
            t += frame_length(&said->frames[size_t(i) * 16]);
        }
        said->total = t;

        // Each segment is the second half of one sound and the first of the
        // next: the sound the segment leads into starts where the voicing
        // changes in it, counted from its first frame with any sound (a
        // diphone can start on a silent one, and the s of "st" would go to
        // the t); else where its loudness moves most, if by as much as
        // twice, as an n opens into the vowel after it; else at its middle.
        auto change = [&](int seg) {
            int first = -1, last = -1;
            for (int i = 0; i < m; ++i)
                if (frame_seg[size_t(i)] == seg + 1) {
                    if (first < 0) first = i;
                    last = i;
                }
            if (first < 0) return -1.0;
            auto frame = [&](int i) { return &said->frames[size_t(i) * 16]; };
            int from = first;
            while (from < last && frame_mode(frame(from)) == 0) ++from;
            int mode = frame_mode(frame(from));
            for (int i = from + 1; i <= last; ++i)
                if (frame_seg[size_t(i)] == seg + 1 && frame_mode(frame(i)) != mode) return said->at[size_t(i)];
            int most = 0, at = -1;
            for (int i = from + 1; i <= last; ++i)
                if (frame_seg[size_t(i)] == seg + 1 && std::abs(frame(i)[2] - frame(i - 1)[2]) > most) {
                    most = std::abs(frame(i)[2] - frame(i - 1)[2]);
                    at = i;
                }
            if (most >= kGainSteps) return said->at[size_t(at)];
            double end = said->at[size_t(last)] + frame_length(frame(last));
            return (said->at[size_t(first)] + end) / 2;
        };
        // The segments matched to the sounds in order, the fewest changes
        // apart: a sound the rule pass changed into another is matched all
        // the same, a vowel to a vowel rather than to a consonant; one it put
        // in (a glottal stop between words, say) is passed over; one it left
        // out is not found.
        const std::vector<Sound> &want = piece.sounds;
        std::vector<int> got;   // the segments that are sounds, by number
        for (int s = 0; s < nseg; ++s)
            if (seg_sound[size_t(s)] >= 0) got.push_back(s);
        size_t n = want.size(), g = got.size();
        auto code_of = [&](size_t k) { return seg_sound[size_t(got[k])]; };
        auto differ = [&](int a, int b) {
            return a == b ? 0 : vowel(uint8_t(a)) == vowel(uint8_t(b)) ? 2 : 4;
        };
        std::vector<std::vector<int>> cost(n + 1, std::vector<int>(g + 1, 0));
        for (size_t i = 0; i <= n; ++i)
            for (size_t k = 0; k <= g; ++k) {
                if (!i && !k) continue;
                int best = 1 << 30;
                if (i && k) best = cost[i - 1][k - 1] + differ(want[i - 1].code, code_of(k - 1));
                if (i) best = std::min(best, cost[i - 1][k] + 3);
                if (k) best = std::min(best, cost[i][k - 1] + 1);
                cost[i][k] = best;
            }
        said->starts.assign(n + 1, -1.0);
        for (size_t i = n, k = g; i && k;) {
            if (cost[i][k] == cost[i - 1][k - 1] + differ(want[i - 1].code, code_of(k - 1))) {
                said->starts[--i] = change(got[--k]);
            } else if (cost[i][k] == cost[i - 1][k] + 3) {
                --i;
            } else {
                --k;
            }
        }
        // and the phrase's end where the engine says its closing pause
        for (size_t k = g; k-- > 0;)
            if (code_of(k) == PAUSE) {
                said->starts[n] = change(got[k]);
                break;
            }
        // anything not found starts where the sound before it does
        double prev = 0;
        for (double &t0 : said->starts) {
            if (t0 < prev) t0 = prev;
            prev = t0;
        }
        if (said->starts.back() <= said->starts.front()) said->starts.back() = said->total;
        return true;
    }

    // A piece's sounds laid out at their lengths and on the pitch curve,
    // played into `pcm`, which is where the phrase has got to. With `fall`,
    // the end of the rest after it, the engine's own fall into silence is
    // played into that rest.
    void sing_piece(const Piece &piece, const Spoken &said, const PitchCurve &curve, double fall,
                    std::vector<int16_t> *pcm, svs_bst_lattice *lattice) const {
        size_t src = 0, quiet = SIZE_MAX;
        uint8_t mixed[16];
        // the frame at `s`: between two frames of the same kind, their
        // reflection coefficients and gain drawn from one to the other, which
        // stays a stable filter, so that a drawn-out vowel glides rather than
        // stepping from frame to frame
        auto frame_at = [&](double s) -> const uint8_t * {
            while (src + 1 < said.at.size() && said.at[src + 1] <= s) ++src;
            while (src > 0 && said.at[src] > s) --src;
            const uint8_t *a = &said.frames[src * 16];
            if (src + 1 >= said.at.size()) return a;
            const uint8_t *b = &said.frames[(src + 1) * 16];
            double len = said.at[src + 1] - said.at[src];
            if (frame_mode(a) != frame_mode(b) || frame_mode(a) == 0 || len <= 0) return a;
            double w = std::clamp((s - said.at[src]) / len, 0.0, 1.0);
            std::copy(a, a + 16, mixed);
            int k0a = int8_t(a[4]) * 2 | (a[1] >> 7), k0b = int8_t(b[4]) * 2 | (b[1] >> 7);
            int k0 = int(std::lround(k0a + w * (k0b - k0a)));
            mixed[4] = uint8_t(int8_t(k0 >> 1));
            mixed[1] = uint8_t((a[1] & 0x7F) | ((k0 & 1) << 7));
            for (int i = 5; i < 14; ++i) mixed[i] = uint8_t(int8_t(std::lround(int8_t(a[i]) + w * (int8_t(b[i]) - int8_t(a[i])))));
            if (a[2] && b[2]) mixed[2] = uint8_t(std::lround(a[2] + w * (b[2] - a[2])));
            return mixed;
        };
        for (size_t j = 0; j < piece.sounds.size(); ++j) {
            const Sound &u = piece.sounds[j];
            double s0 = said.starts[j], s1 = said.starts[j + 1], S = std::max(0.0, s1 - s0);
            double T = std::max(1e-6, u.end - u.start);
            double e = std::min(S / 4, kEdge);
            long end = std::lround(u.end * kEngineRate);
            while (long(pcm->size()) < end) {
                double tau = std::max(0.0, double(pcm->size()) / kEngineRate - u.start);
                double s;
                if (T <= S || T <= 2 * e) s = s0 + tau * S / T;
                else if (tau < e) s = s0 + tau;
                else if (tau > T - e) s = s1 - (T - tau);
                else s = s0 + e + (tau - e) * (S - 2 * e) / (T - 2 * e);
                play(frame_at(s), double(pcm->size()) / kEngineRate, end, curve, pcm, lattice, &quiet);
            }
        }
        if (fall < 0) return;
        // the fall into silence at its own speed, as long as the rest allows
        long end = std::lround(fall * kEngineRate);
        double s = said.starts.back();
        while (long(pcm->size()) < end && s < said.total) {
            const uint8_t *f = frame_at(s);
            size_t before = pcm->size();
            play(f, double(pcm->size()) / kEngineRate, end, curve, pcm, lattice, &quiet);
            s += double(pcm->size() - before) / kEngineRate;
        }
    }

    // One frame, played once: a voiced one for one period of the curve's
    // pitch at `t`, its gain moved with the period; an unvoiced one for its
    // own length, or what is left before `end`. `quiet` counts the samples of
    // silent frames since the last that sounded, SIZE_MAX before any has.
    static void play(const uint8_t *src, double t, long end, const PitchCurve &curve, std::vector<int16_t> *pcm,
                     svs_bst_lattice *lattice, size_t *quiet) {
        uint8_t f[16];
        std::copy(src, src + 16, f);
        int mode = frame_mode(f), from = std::max(1, frame_period(f)), period;
        int gain = f[2];
        if (mode == 0x10 || mode == 0x30) {
            double hz = curve.hz_at(std::clamp(t, 0.0, curve.total()));
            period = std::clamp(int(std::lround(kEngineRate * 16.0 / hz)), kMinPeriod, kMaxPeriod);
            if (gain) gain += int(std::lround(kGainSteps / 2.0 * std::log2(double(period) / from)));
        } else {
            long left = end - long(pcm->size());
            period = std::clamp(int(std::min<long>(from, left * 16)), 8 * 16, kMaxPeriod);
        }
        if (gain) gain = std::clamp(gain - kGainCut, 1, 207);
        f[0] = uint8_t(mode | 1);
        f[1] = uint8_t((f[1] & 0xF0) | (period & 0x0F));
        f[2] = uint8_t(gain);
        f[3] = uint8_t(period >> 4);
        int16_t buf[4096];
        size_t got = svs_bst_lattice_frame(lattice, f, buf, 4096);
        if (got == 0) {
            pcm->push_back(0);   // an end marker would stand the phrase still
            return;
        }
        // a sound straight after a silent frame between two others starts
        // with a knock, the lattice set going again on the sound before it:
        // "goes", its vowel stopped and its z whispered. Counted in frames,
        // not by looking for quiet samples, which a period ends on as often
        // as not.
        if (!gain) {
            if (*quiet != SIZE_MAX) *quiet += got;
        } else {
            if (*quiet > 0 && *quiet <= kJoinGap)
                for (size_t i = 0; i < std::min(got, kJoinFade); ++i)
                    buf[i] = int16_t(buf[i] * double(i) / kJoinFade);
            *quiet = 0;
        }
        pcm->insert(pcm->end(), buf, buf + got);
    }

    // Silence until `until`; the lattice is left as it was.
    static void silence(double until, std::vector<int16_t> *pcm) {
        long end = std::lround(until * kEngineRate);
        if (long(pcm->size()) < end) pcm->resize(size_t(end), 0);
    }

    int v_;
    bst *h_ = nullptr;
};

class BestFamily : public Family {
public:
    std::string name() const override { return "BeSTspeech"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int i = 0; i < kVoiceCount; ++i)
            out.push_back({kBestBase + i, "BeSTspeech " + std::to_string(i + 1)});
        return out;
    }

    bool available(std::string *) override { return true; }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int v = voice_id - kBestBase;
        if (v < 0 || v >= kVoiceCount) {
            if (err) *err = "There is no BeSTspeech voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        return std::make_unique<BestSinger>(v);
    }
};

}  // namespace

std::unique_ptr<Family> make_bestspeech_family() { return std::make_unique<BestFamily>(); }

//: Not part of family.h: the self-check the engine's test program runs. Every
//: VocalWriter phoneme has a row in the table and every row is one of
//: VocalWriter's. Returns an empty string, or what is wrong.
std::string bestspeech_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes())
        if (!find_map(p.symbol)) bad += std::string("no BeSTspeech sound for ") + p.symbol + "; ";
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    return bad;
}

}  // namespace svs
