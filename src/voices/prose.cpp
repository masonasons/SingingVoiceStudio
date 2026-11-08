// prose.cpp -- the Prose 2000's voices, singing.
//
// The Prose 2000 is Speech Plus's text-to-speech board of 1988, sold by
// Telesensory Systems: an 8086 running the text and prosody rules and an NEC
// uPD7720 DSP running a formant synthesiser, as third_party/prose2000_reborn
// decompiles its v3.4.1 firmware and models its DSP, frame and sample exact.
// The program sings with its three voices, the ROM's data compiled in
// (cmake/engines/prose.cmake), so it needs no data.
//
// The board has no way to be told how long a sound lasts, and takes no pitch
// above 255 Hz, so it is told neither. A phrase is written in its own phoneme
// codes, every note a stressed word of its own, and spoken once, with every
// 10 ms frame of its twenty-two parameters kept and every phoneme it plays
// reported with where it starts (the library's on_params and on_phoneme). The
// phonemes it played are matched to the ones written, past any its allophone
// rules changed, joined or put in. Then the frames are laid out again: each
// sound is stretched or squeezed to the length the studio gives it, a held
// vowel's transitions and the breath before it kept at their own speed and its
// loud middle drawn out, and the frames are synthesised one at a time through
// the library's own frame builder and DSP model. Their two pitch periods come
// from the studio's pitch curve, handed to the frame builder through the
// build's hook in place of the ones its F0 byte would give. The DSP plays
// whole samples, and a pitch between them made of periods a sample apart in
// turn is heard as hoarseness, so both periods are the whole number of samples
// nearest the curve's, and the phrase is retuned to the curve afterwards by
// pitch-synchronous overlap-add (psola_matched, macintalk_dsp.h), every period
// moved by less than half a sample.
//
// A long phrase is sung in pieces of forty notes. A rest ends a piece, and
// the board's own fall into silence is heard in it. A piece ends in a full
// stop joined to its last word, as in text: only then does the board release
// a final plosive or nasal before the pause, with a burst and a short vowel
// after it, and without that a final d or t is all but silent.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <prose.h>

#include "core/phonology.h"
#include "voices/family.h"
#include "voices/macintalk_dsp.h"
#include "voices/pitch.h"

extern "C" {
// added to the frame builder by prose.cmake
extern void (*svs_prose_period_hook)(int16_t *per1, int16_t *per2);
}

namespace svs {

namespace {

constexpr int kEngineRate = PROSE_SAMPLE_RATE;
//: a frame: 10 ms, a hundred samples
constexpr int kFrameSamples = 100;
constexpr int kParms = 22;
//: the parameters: voicing amplitude, the pitch in hertz, and from p18 on the
//: voice's source settings, which are not drawn between frames
constexpr int kAV = 0, kF0 = 17, kSource = 18;
//: the noise's amplitudes, frication to the bypass
constexpr int kAF = 1, kAB = 8;
//: the nasal resonator's frequency, which the board raises as a nasal nears
constexpr int kFN = 16;
constexpr int kVoiceCount = 3;
//: notes to a piece
constexpr size_t kPieceWords = 40;
//: how long the sound fades into the DSP's silence and out of it; and the
//: step there, the DAC's bias above its silent code, (0x800 - 0x7F0) * 15
constexpr double kQuietFade = 0.005, kSilentStep = 240.0;
//: how much of a held sound's either end keeps its own speed
constexpr double kEdge = 0.03;
//: the most of a vowel the breath before it takes, at its own length
constexpr double kTailShare = 0.4;
//: how far under a held vowel's loudest frame, in decibels, the frames it
//: is drawn out over may be; how far over its lowest their nasal resonator
//: may be, in the board's steps; and how far its second formant may move
//: after that for the vowel to have settled before it
constexpr int kLoudDb = 2, kNasalSteps = 6, kSettledSteps = 10;
//: the second formant
constexpr int kF2 = 10;
//: the pitches the board sings, in hertz: a phrase above them goes whole
//: octaves down, and what still does not fit is held to the nearest. The
//: shortest period it voices is 13 samples; at 12 its voicing step is 0.
constexpr double kLowest = 50.0, kHighest = 740.0;
constexpr double kMinHz = 40.0, kMaxHz = 765.0;

//: the pitch the board sings at `t` seconds into the phrase, moved by
//: `shift` octaves into its range
double sung_hz(const PitchCurve &curve, double shift, double t) {
    return std::clamp(curve.hz_at(std::clamp(t, 0.0, curve.total())) * shift, kMinHz, kMaxHz);
}

// The voices are evened out as the other engines' are: each was sung through
// ten vowels on notes across its range and scaled so that its median peak
// is 0.5 (prose_sing_test --levels).
const double kGains[kVoiceCount] = {1.42, 1.66, 1.85};
//: the voices by what they are; what the board called them is not known
const char *const kVoiceNames[kVoiceCount] = {"Standard", "Deep", "High"};

// VocalWriter's fifty-seven phonemes as the Prose's codes. Its r-coloured
// vowels, YU, syllabic l and n and dark l are codes of their own; CH and JH
// are a closure and a release, as it spells them; the flaps of "better" and
// "greater" are its flap, t, and the t of "it" its glottalised t, q. O, the
// first part of OY, is the vowel of "caught".
struct PhoneMap {
    const char *vw;
    const char *prose;
};
const PhoneMap kPhoneMap[] = {
    {"IY", "E"},  {"IH", "i"},  {"EH", "e"},  {"AE", "a"},  {"AA", "o"},  {"UX", "v"},  {"AO", "w"},
    {"UH", "u"},  {"AX", "@"},  {"ER", "3"},  {"EY", "A"},  {"AY", "I"},  {"OY", "y"},  {"AW", "f"},
    {"OW", "O"},  {"UW", "b"},  {"YU", "U"},  {"IR", "4"},  {"XR", "k"},  {"AR", "r"},  {"OR", "g"},
    {"UR", "c"},  {"IX", "|"},  {"%", ""},    {"RX", "3"},  {"LX", "j"},  {"EL", "l"},  {"EN", "n"},
    {"w", "W"},   {"y", "Y"},   {"r", "R"},   {"l", "L"},   {"h", "H"},   {"m", "M"},   {"n", "N"},
    {"NG", "~"},  {"f", "F"},   {"v", "V"},   {"TH", "X"},  {"DH", "x"},  {"s", "S"},   {"z", "Z"},
    {"SH", "s"},  {"ZH", "z"},  {"p", "P"},   {"b", "B"},   {"t", "T"},   {"d", "D"},   {"k", "K"},
    {"g", "G"},   {"CH", "Cs"}, {"JH", "Jz"}, {"TX", "q"},  {"Q", "t"},   {"QX", "t"},  {"DD", "t"},
    {"O", "w"},
};

const PhoneMap *find_map(const std::string &symbol) {
    std::string s = palette_alias(symbol);   // OH and DX, as VocalWriter's MIDI files spell them
    for (const PhoneMap &m : kPhoneMap)
        if (s == m.vw) return &m;
    return nullptr;
}

//: what a syllable is sung on: the vowels, and l, n and m said as one
bool nucleus(char c) { return std::strchr("EiAev@|4aowOubIfyU3rgkclnm", c) != nullptr; }

//: One of VocalWriter's phonemes, as the Prose's codes, from `start` to `end`
//: in seconds from the phrase's start. `held` is the note's held one.
struct Group {
    std::string codes;
    double start = 0, end = 0;
    bool held = false;
};

//: A piece: its groups in order, and the text that says them.
struct Piece {
    std::vector<Group> groups;
    std::string text;
    //: the end of the rest after it, if one follows
    double fall = -1;
};

using Frame = std::array<uint8_t, kParms>;

//: A phoneme the board played, from `start` to `end` in seconds.
struct Segment {
    char code;
    double start, end;
};

//: what the board made of a piece: its frames, each with where it starts in
//: seconds, and the phonemes it played
struct Spoken {
    std::vector<Frame> frames;
    std::vector<double> at;
    std::vector<Segment> segments;
};

//: A stretch of the board's frames, `s0` to `s1` in seconds of its own time,
//: sung from `t0` to `t1` in the phrase's.
struct Part {
    double s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    bool held = false;
    //: in a held one, the stretch drawn out: before and after it the sound
    //: keeps its own speed
    double a = -1, b = -1;
};

// The frame builder's hook is global, so one phrase is sung at a time.
std::mutex g_engine;
//: the two periods the next frame is built with
int16_t g_periods[2];

void use_periods(int16_t *per1, int16_t *per2) {
    *per1 = g_periods[0];
    *per2 = g_periods[1];
}

class ProseSinger : public Singer {
public:
    explicit ProseSinger(int v) : v_(v) {}

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        std::vector<Piece> pieces = lay_out(phrase, curve);

        std::lock_guard<std::mutex> lock(g_engine);
        // A handle a phrase: the board's noise and its jitter carry on from one
        // utterance to the next, and a phrase should sing the same each time.
        prose_h h = nullptr;
        if (prose_open(&h, PROSE_V341) != PROSE_OK) {
            sung.error = "The Prose 2000 could not be started";
            return sung;
        }
        std::unique_ptr<prose_handle, void (*)(prose_h)> close(h, [](prose_h x) { prose_close(x); });
        prose_set_voice(h, v_);

        // whole octaves down into the range the board sings
        double lo = 1e9, hi = 0, shift = 1;
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            bool voiced = false;
            for (const std::string &ph : phrase.notes[i].phonemes) {
                const PhoneMap *m = find_map(ph);
                if (m && *m->prose) voiced = true;
            }
            if (!voiced) continue;
            double hz = PitchCurve::midi_to_hz(curve.steady_midi_at((curve.note_start(i) + curve.note_end(i)) / 2));
            lo = std::min(lo, hz);
            hi = std::max(hi, hz);
        }
        while (hi * shift > kHighest && lo * shift / 2 >= kLowest) shift /= 2;

        std::vector<int16_t> pcm;
        // where each frame sung starts, in samples, and its pitch (0 unvoiced)
        std::vector<std::pair<size_t, double>> sung_at;
        for (const Piece &piece : pieces) {
            // up to where the piece starts, silence
            long from = std::lround(piece.groups.front().start * kEngineRate);
            if (long(pcm.size()) < from) pcm.resize(size_t(from), 0);
            Spoken said;
            if (!speak(h, piece.text, &said)) {
                sung.error = "The Prose 2000 could not say a phrase";
                return sung;
            }
            if (said.frames.empty()) continue;
            std::vector<Frame> out = lay_frames(piece, said, pcm.size());
            for (size_t k = 0; k < out.size(); ++k) {
                size_t at = pcm.size() + k * kFrameSamples;
                double hz = sung_hz(curve, shift, (double(at) + 0.5 * kFrameSamples) / kEngineRate);
                sung_at.push_back({at, out[k][kAV] > 0 ? kEngineRate / std::round(kEngineRate / hz) : 0.0});
            }
            if (!render(h, out, curve, shift, pcm.size(), &pcm)) {
                sung.error = "The Prose 2000 could not sing a phrase";
                return sung;
            }
        }
        // and silence to the end of the last rest
        long end = std::lround(curve.total() * kEngineRate);
        if (long(pcm.size()) < end) pcm.resize(size_t(end), 0);

        std::vector<float> whole(pcm.begin(), pcm.end());
        // The DSP's silence is the DAC code under its signal's bias, 240 in
        // the library's samples (prose_wave.h), and it goes in and out of it
        // at once: into it with what is left of the sound before it, an offset
        // and the resonators' last ringing, cut off, and out of it with the
        // bias at a step, both heard as a click. The sound is faded out over
        // kQuietFade before each silence, and the bias brought in over as long
        // after it, so that a burst there is kept whole.
        auto fade = size_t(kQuietFade * kEngineRate);
        auto silent_from = [&](size_t i) {
            size_t j = i;
            while (j < whole.size() && j < i + kFrameSamples && whole[j] == 0) ++j;
            return j == whole.size() || j == i + kFrameSamples;
        };
        // (the phrase starts and ends in silence)
        for (size_t i = 0; i < whole.size();) {
            while (i < whole.size() && whole[i] == 0) ++i;
            for (size_t k = 0; k < fade && i + k < whole.size(); ++k)
                whole[i + k] -= float(kSilentStep * (1.0 - double(k + 1) / double(fade + 1)));
            while (i < whole.size() && (whole[i] != 0 || !silent_from(i))) ++i;
            for (size_t k = 1; k <= fade && k <= i; ++k)
                whole[i - k] *= float(double(k) / double(fade + 1));
        }
        // the pitch the frames were given, rather than measured: a tracker
        // lags a glide between two short notes, and the periods found
        // across it are the wrong length
        dsp::F0Track f0 = dsp::frame_track(sung_at, kFrameSamples, whole.size(), kEngineRate);
        std::vector<float> wide = dsp::psola_matched(
            whole, kEngineRate, f0, whole.size(), [](double t) { return t; },
            [&curve, shift](double t) { return sung_hz(curve, shift, t); });
        std::vector<float> samples = resample_to_output(wide, kEngineRate);
        // to +-1 at the part's level; a phrase that would still clip is
        // brought down whole rather than cut off
        double gain = phrase.level * kGains[v_] / 32768.0, peak = 0.0;
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
    // A piece spoken, its frames and phonemes kept; the audio is not wanted.
    static bool speak(prose_h h, const std::string &text, Spoken *said) {
        prose_callbacks cb = {};
        cb.on_params = [](prose_h, const uint8_t *p, uint32_t position, void *user) {
            auto *s = static_cast<Spoken *>(user);
            Frame f;
            std::copy(p, p + kParms, f.begin());
            s->frames.push_back(f);
            s->at.push_back(double(position) / kEngineRate);
        };
        cb.on_phoneme = [](prose_h, char ph, int ms, uint32_t position, void *user) {
            auto *s = static_cast<Spoken *>(user);
            double t = double(position) / kEngineRate;
            s->segments.push_back({ph, t, t + ms / 1000.0});
        };
        prose_set_callbacks(h, &cb, said);
        static int16_t buf[4096];
        int32_t n = prose_speak_to_buffer(h, ("\x1b[1I" + text).c_str(), buf, sizeof buf / sizeof buf[0],
                                          [](prose_h, const int16_t *, size_t, uint32_t, void *) { return 0; },
                                          nullptr);
        prose_set_callbacks(h, nullptr, nullptr);
        return n >= 0;
    }

    //: a piece's frames being played: the octaves the phrase is moved by,
    //: and where the first starts in the phrase, in samples
    struct Run {
        const std::vector<Frame> *out;
        const PitchCurve *curve;
        double shift;
        size_t at;
        std::vector<int16_t> *pcm;
    };

    // The k'th frame's parameters, and its two periods for the frame builder,
    // both the whole number of samples nearest the pitch curve's at the
    // frame's middle.
    static void prepare(prose_h h, const Run &r, size_t k) {
        Frame f = (*r.out)[k];
        double t = (double(r.at) + (double(k) + 0.5) * kFrameSamples) / kEngineRate;
        double hz = sung_hz(*r.curve, r.shift, t);
        // the byte only says the frame is voiced, and how deep its jitter is
        if (f[kAV] > 0) f[kF0] = uint8_t(std::clamp(std::lround(hz), 1L, 255L));
        for (int p = 0; p < kParms; ++p) prose_set_frame_param(h, p, f[size_t(p)]);
        // a period the frame builder gives as n samples lasts n + 1
        g_periods[0] = g_periods[1] = int16_t(std::lround(kEngineRate / hz) - 1);
    }

    // The frames, played one at a time from `at` in the phrase, in samples.
    static bool render(prose_h h, const std::vector<Frame> &out, const PitchCurve &curve, double shift, size_t at,
                       std::vector<int16_t> *pcm) {
        Run run{&out, &curve, shift, at, pcm};
        prepare(h, run, 0);
        svs_prose_period_hook = use_periods;
        int n = prose_render_frames(
            h, int(out.size()),
            [](prose_h hh, int k, const int16_t *samples, int count, void *user) {
                auto *r = static_cast<Run *>(user);
                r->pcm->insert(r->pcm->end(), samples, samples + count);
                if (size_t(k) + 1 < r->out->size()) prepare(hh, *r, size_t(k) + 1);
                return 0;
            },
            &run);
        svs_prose_period_hook = nullptr;
        return n == int(out.size());
    }

    // The phrase as pieces: runs of notes between its rests, each run cut
    // where it grows too long, every note a word of its own with its stress
    // on its held vowel. A note with no vowel in it joins the word before it,
    // or else the one after.
    static std::vector<Piece> lay_out(const Phrase &phrase, const PitchCurve &curve) {
        std::vector<Piece> pieces;
        std::vector<std::vector<Group>> words;
        auto has_nucleus = [](const std::vector<Group> &w) {
            for (const Group &g : w)
                for (char c : g.codes)
                    if (nucleus(c)) return true;
            return false;
        };
        auto close = [&](double fall) {
            if (words.empty()) return;
            std::vector<std::vector<Group>> kept;
            std::vector<Group> carry;
            for (auto &w : words) {
                if (!has_nucleus(w)) {
                    if (!kept.empty()) kept.back().insert(kept.back().end(), w.begin(), w.end());
                    else carry.insert(carry.end(), w.begin(), w.end());
                    continue;
                }
                w.insert(w.begin(), carry.begin(), carry.end());
                carry.clear();
                kept.push_back(w);
            }
            if (kept.empty()) kept.push_back(carry);
            else if (!carry.empty()) kept.back().insert(kept.back().end(), carry.begin(), carry.end());
            Piece piece;
            piece.fall = fall;
            for (const auto &w : kept) {
                // the stress after the held vowel, or else the first
                size_t stress = std::string::npos, k = 0;
                for (const Group &g : w)
                    for (char c : g.codes) {
                        if (nucleus(c) && (stress == std::string::npos || (g.held && c == g.codes[0])))
                            stress = k;
                        ++k;
                    }
                // A reduced vowel and an n or l after it the board makes one
                // syllabic n or l, and the note loses the vowel it is sung on;
                // a morph boundary between them keeps them apart.
                std::string codes;
                for (const Group &g : w) codes += g.codes;
                piece.text += '&';
                for (k = 0; k < codes.size(); ++k) {
                    piece.text += codes[k];
                    if (k == stress) piece.text += '1';
                    bool reduced = codes[k] == '@' || codes[k] == '|';
                    if (reduced && k + 1 < codes.size() && (codes[k + 1] == 'N' || codes[k + 1] == 'L'))
                        piece.text += '[';
                }
                piece.groups.insert(piece.groups.end(), w.begin(), w.end());
            }
            piece.text += ".";   // joined: a final plosive is released
            pieces.push_back(piece);
            words.clear();
        };
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            const SungNote &n = phrase.notes[i];
            std::vector<Group> word;
            int held = held_phoneme(n.phonemes);
            double at = curve.note_start(i);
            for (size_t k = 0; k < n.phonemes.size(); ++k) {
                double ms = k < n.durations_ms.size() ? n.durations_ms[k] : 0.0;
                const PhoneMap *m = find_map(n.phonemes[k]);
                Group g;
                g.start = at;
                g.end = at += ms / 1000.0;
                if (!m || !*m->prose) continue;
                g.codes = m->prose;
                g.held = int(k) == held;
                word.push_back(g);
            }
            if (word.empty()) {
                // a rest
                close(curve.note_end(i));
                continue;
            }
            if (words.size() == kPieceWords) close(-1);
            words.push_back(word);
        }
        close(-1);
        return pieces;
    }

    // Where each code written was played: the board's phonemes matched to
    // them in order, the fewest changes apart. A code its rules changed into
    // another is matched all the same, a vowel to a vowel rather than to a
    // consonant (an r-coloured vowel is a vowel and r joined); one it put in
    // belongs to the code before it; codes it joined into one share that one
    // between them.
    static std::vector<std::pair<double, double>> match(const std::string &want, const std::vector<Segment> &got) {
        size_t n = want.size(), m = got.size();
        auto change = [](char a, char b) { return a == b ? 0 : nucleus(a) == nucleus(b) ? 2 : 4; };
        std::vector<std::vector<int>> cost(n + 1, std::vector<int>(m + 1, 0));
        for (size_t i = 0; i <= n; ++i)
            for (size_t j = 0; j <= m; ++j) {
                if (!i && !j) continue;
                int best = 1 << 30;
                if (i && j) best = cost[i - 1][j - 1] + change(want[i - 1], got[j - 1].code);
                if (i) best = std::min(best, cost[i - 1][j] + 3);
                if (j) best = std::min(best, cost[i][j - 1] + (got[j - 1].code == ' ' ? 0 : 1));
                cost[i][j] = best;
            }
        std::vector<int> pair(n, -1);
        for (size_t i = n, j = m; i || j;) {
            if (i && j && cost[i][j] == cost[i - 1][j - 1] + change(want[i - 1], got[j - 1].code)) {
                pair[--i] = int(--j);
            } else if (i && cost[i][j] == cost[i - 1][j] + 3) {
                --i;
            } else {
                --j;
            }
        }
        std::vector<std::pair<double, double>> span(n, {0.0, 0.0});
        double end = m ? got.front().start : 0.0;
        for (size_t i = 0; i < n;) {
            if (pair[i] < 0) {
                span[i++] = {end, end};
                continue;
            }
            // this code, and the unmatched ones up to the next matched one
            size_t j = i + 1;
            while (j < n && pair[j] < 0) ++j;
            const Segment &s = got[size_t(pair[i])];
            double stop = j < n ? got[size_t(pair[j])].start : s.end;
            double len = (stop - s.start) / double(j - i);
            for (size_t k = i; k < j; ++k) span[k] = {s.start + len * double(k - i), s.start + len * double(k - i + 1)};
            end = stop;
            i = j;
        }
        return span;
    }

    // The frames the board made of a piece, laid out on the phrase's time
    // from `at`, in samples.
    static std::vector<Frame> lay_frames(const Piece &piece, const Spoken &said, size_t at) {
        std::string want;
        for (const Group &g : piece.groups) want += g.codes;
        std::vector<std::pair<double, double>> span = match(want, said.segments);
        double last = said.at.back() + double(kFrameSamples) / kEngineRate;
        // where the voicing starts in a stretch, or its end
        auto voiced_from = [&](double a, double b) {
            for (size_t i = 0; i < said.frames.size(); ++i)
                if (said.at[i] >= a && said.at[i] < b && said.frames[i][kAV] > 0) return said.at[i];
            return b;
        };

        // A held vowel is drawn out over its loud frames, those no more than
        // kLoudDb under its loudest, so that an AY glides from its a to its i
        // over the whole note; the way in and the way out, at least kEdge
        // each, and the board's fall into a pause after them, keep their own
        // speed. Of a vowel the board nasalises for a nasal after it, raising
        // its nasal resonator and widening its first formant's bandwidth, the
        // frames it has nasalised are left out when it has settled before
        // them, its second formant moving no further; one still on its way
        // there, the i of "falling", keeps them. Drawn out whole, a vowel
        // before a pause or a nasal fades over the whole note, and one loud
        // from its first frame holds its move out of the consonant before it,
        // the t of "stay" heard as an n.
        const auto &fr = said.frames;
        auto drawn_stretch = [&](Part *p) {
            size_t i0 = size_t(std::lower_bound(said.at.begin(), said.at.end(), p->s0) - said.at.begin());
            size_t i1 = i0;
            while (i1 + 1 < fr.size() && said.at[i1 + 1] <= p->s1) ++i1;
            if (i1 <= i0) return;
            int loudest = 0, oral = 255;
            for (size_t i = i0; i <= i1; ++i) {
                loudest = std::max(loudest, int(fr[i][kAV]));
                oral = std::min(oral, int(fr[i][kFN]));
            }
            size_t lo = i1, hi = i0, oral_hi = i1 + 1;
            for (size_t i = i0; i <= i1; ++i)
                if (fr[i][kAV] + kLoudDb >= loudest) {
                    lo = std::min(lo, i);
                    hi = i;
                    if (fr[i][kFN] <= oral + kNasalSteps) oral_hi = i;
                }
            if (lo > hi) return;
            if (oral_hi <= i1 && oral_hi >= lo && std::abs(fr[hi][kF2] - fr[oral_hi][kF2]) <= kSettledSteps)
                hi = oral_hi;
            double e = std::min((p->s1 - p->s0) / 4, kEdge);
            p->a = std::max(said.at[lo], p->s0 + e);
            p->b = std::max(p->a, std::min(said.at[hi], p->s1 - e));
        };

        std::vector<Part> parts;
        size_t c = 0;
        for (const Group &g : piece.groups) {
            std::vector<Part> mine;
            std::vector<bool> tail;
            bool vowel_seen = false;
            for (char code : g.codes) {
                Part p;
                p.s0 = span[c].first;
                p.s1 = span[c].second;
                ++c;
                // a vowel holds; a stop's breath at its start keeps its length
                bool hold = g.held && nucleus(code) && !vowel_seen;
                if (nucleus(code)) vowel_seen = true;
                if (hold) {
                    double v = voiced_from(p.s0, p.s1);
                    if (v > p.s0 && v < p.s1) {
                        Part breath = p;
                        breath.s1 = p.s0 = v;
                        mine.push_back(breath);
                        tail.push_back(true);
                    }
                }
                p.held = hold;
                if (hold) drawn_stretch(&p);
                mine.push_back(p);
                tail.push_back(!hold);
            }
            share(g, &mine, tail);
            parts.insert(parts.end(), mine.begin(), mine.end());
        }
        double fall_at = want.empty() ? 0.0 : span.back().second;

        // the frame at `s`, between the two either side of it
        auto frame_at = [&](double s) {
            size_t i = size_t(std::upper_bound(said.at.begin(), said.at.end(), s) - said.at.begin());
            i = i ? i - 1 : 0;
            Frame f = said.frames[i];
            if (i + 1 < said.frames.size()) {
                double w = std::clamp((s - said.at[i]) / (said.at[i + 1] - said.at[i]), 0.0, 1.0);
                for (int k = 0; k < kSource; ++k)
                    f[size_t(k)] = uint8_t(std::lround(said.frames[i][size_t(k)] +
                                                       w * (said.frames[i + 1][size_t(k)] - said.frames[i][size_t(k)])));
            }
            return f;
        };
        // Outside a held vowel the board's own frames, not drawn between:
        // drawn, a stop's one frame of burst is smeared into the frames
        // either side of it and all but lost, and a b is heard as a v. Where
        // the sound goes faster than its own speed, the frame of those
        // passed over with the most noise in it, so that no burst is
        // skipped.
        auto noise = [&](size_t i) {
            int n = 0;
            for (int k = kAF; k <= kAB; ++k) n += said.frames[i][size_t(k)];
            return n;
        };
        auto own_frame = [&](double from, double s) {
            auto upto = size_t(std::upper_bound(said.at.begin(), said.at.end(), s) - said.at.begin());
            size_t i = upto ? upto - 1 : 0;
            for (size_t k = i; k-- > 0 && said.at[k] > from;)
                if (noise(k) > noise(i)) i = k;
            return said.frames[i];
        };
        std::vector<Frame> out;
        double t = double(at) / kEngineRate, step = double(kFrameSamples) / kEngineRate;
        size_t p = 0;
        double end = parts.back().t1, was = -1;
        for (; t < end - step / 2; t += step) {
            while (p + 1 < parts.size() && t >= parts[p].t1) ++p;
            const Part &u = parts[p];
            double S = std::max(0.0, u.s1 - u.s0), T = std::max(1e-6, u.t1 - u.t0);
            double tau = std::clamp(t - u.t0, 0.0, T);
            double e = std::min(S / 4, kEdge), s;
            if (T > S && u.a >= 0) {
                double in = u.a - u.s0, out = u.s1 - u.b;
                if (tau < in) s = u.s0 + tau;
                else if (tau > T - out) s = u.s1 - (T - tau);
                else s = u.a + (tau - in) * (u.b - u.a) / (T - in - out);
            } else if (T <= S || T <= 2 * e) s = u.s0 + tau * S / T;
            else if (tau < e) s = u.s0 + tau;
            else if (tau > T - e) s = u.s1 - (T - tau);
            else s = u.s0 + e + (tau - e) * (S - 2 * e) / (T - 2 * e);
            out.push_back(u.held ? frame_at(s) : own_frame(was, s));
            was = s;
        }
        // the fall into silence at its own speed, as long as the rest allows
        for (double s = fall_at; piece.fall > 0 && t < piece.fall - step / 2 && s < last; s += step, t += step)
            out.push_back(frame_at(s));
        return out;
    }

    // A group's time over its stretches: in a vowel, the stretches marked
    // `tail` at their own length (no more than kTailShare of it between
    // them) and the held one the rest; otherwise each in proportion to its
    // own length.
    static void share(const Group &g, std::vector<Part> *parts, const std::vector<bool> &tail) {
        double T = g.end - g.start, own = 0, tails = 0;
        size_t holds = 0;
        for (size_t i = 0; i < parts->size(); ++i) {
            double len = (*parts)[i].s1 - (*parts)[i].s0;
            own += len;
            if (tail[i]) tails += len;
            else ++holds;
        }
        double k = tails > kTailShare * T ? kTailShare * T / tails : 1.0;
        double t = g.start;
        for (size_t i = 0; i < parts->size(); ++i) {
            Part &p = (*parts)[i];
            double len = p.s1 - p.s0, d;
            if (holds) d = tail[i] ? len * k : (T - tails * k) / double(holds);
            else d = own > 0 ? T * len / own : T / double(parts->size());
            p.t0 = t;
            p.t1 = t += d;
        }
    }

    int v_;
};

class ProseFamily : public Family {
public:
    std::string name() const override { return "Prose 2000"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int i = 0; i < kVoiceCount; ++i)
            out.push_back({kProseBase + i, std::string("Prose 2000 ") + kVoiceNames[i]});
        return out;
    }

    bool available(std::string *) override { return true; }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int v = voice_id - kProseBase;
        if (v < 0 || v >= kVoiceCount) {
            if (err) *err = "There is no Prose 2000 voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        return std::make_unique<ProseSinger>(v);
    }
};

}  // namespace

std::unique_ptr<Family> make_prose_family() { return std::make_unique<ProseFamily>(); }

//: Not part of family.h: the self-check the engine's test program runs. Every
//: VocalWriter phoneme has a row in the table and every row is one of
//: VocalWriter's. Returns an empty string, or what is wrong.
std::string prose_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes())
        if (!find_map(p.symbol)) bad += std::string("no Prose 2000 code for ") + p.symbol + "; ";
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    return bad;
}

}  // namespace svs
