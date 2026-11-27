// truvoice.cpp -- TruVoice's voices, singing.
//
// TruVoice is Centigram's text-to-speech system, the voice of Microsoft Agent,
// grown out of the Speech Plus Prose 2000: third_party/opentv decompiles its
// 1997 English engine into C, checked byte for byte against the original. The
// program sings with its eleven American voices, the engine's tables compiled
// in (cmake/engines/truvoice.cmake), so it needs no data.
//
// TruVoice took the Prose 2000's phoneme codes and its twenty-two parameters,
// a frame of them every hundredth of a second, so it sings the way the Prose
// 2000 does (prose.cpp). The engine cannot be told how long a sound lasts, and
// its own pitch is a byte holding half of it, so it is told neither. A phrase
// is written in its phoneme codes, every note a stressed word of its own, and
// traced (tvtts_phonemes_to_frames): every frame the synthesiser reads, and
// every phoneme it plays with the frame it starts on. The phonemes it played
// are matched to the ones written, past any its rules changed, joined or put
// in. Then the frames are laid out again: each sound is stretched or squeezed
// to the length the studio gives it, a held vowel's transitions and the breath
// before it kept at their own speed and its loud middle drawn out, and the
// whole piece is synthesised in one go (tvtts_speak_frames_periods), each
// frame with its period given in place of what its pitch byte would make of
// it, and the engine puts the voice's jitter on it as it does on a pitch.
//
// The jitter is most of what makes a voice creak, Grandpa Amos most of all,
// so the voices whose creak is heard -- he, Melvin and Alex, and Frank, whose
// breath is -- are given their periods to a 256th of a sample and the engine's
// own sound is what is heard: it plays whole samples, a period a sample longer
// whenever the fractions add up to one, so the pitch is exact and the creak,
// the shimmer and the breath are the engine's (kCreaks). Retuned afterwards,
// as the other engines are, laid down again at the curve's even spacing, the
// creak was ironed out. The steady voices have no jitter at the notes they
// sing for a pattern of periods a sample apart to hide in, and it is heard as
// roughness, so they are given the whole number of samples nearest the curve's
// period and retuned to the curve afterwards by pitch-synchronous overlap-add
// (psola_matched, macintalk_dsp.h), every period moved by less than half a
// sample.
//
// A phrase is sung as one piece up to a rest, which ends it, and the engine's
// own fall into silence is heard in it; a piece cut anywhere else fades out
// and in again, and is heard as a gap. A piece ends in a full stop joined to
// its last word, as in text: only then does the engine release a final
// plosive or nasal before the pause. Where the engine pauses inside a piece,
// as it does in speech, nothing is sung: between a vowel and a word that
// starts on one, which a morph boundary keeps it from, and between an s and
// the w, l, n or m after it, whose frames are taken out (speak).

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

#include <tvtts.h>

#include "core/phonology.h"
#include "voices/family.h"
#include "voices/macintalk_dsp.h"
#include "voices/pitch.h"

namespace svs {

namespace {

constexpr int kEngineRate = 11025;
//: a frame: a hundredth of a second rounded down, 110 samples
constexpr int kFrameSamples = kEngineRate / 100;
constexpr int kParms = TVTTS_FRAME_TRACKS;
//: the parameters: voicing amplitude, the pitch (half of it, in hertz), and
//: from p18 on the voice's source settings and, in p21, the voice itself,
//: none of which are drawn between frames
constexpr int kAV = 0, kF0 = 17, kSource = 18;
//: the noise's amplitudes, frication to the bypass
constexpr int kAF = 1, kAB = 8;
//: the nasal resonator's frequency, which the engine raises as a nasal nears
constexpr int kFN = 16;
//: the second formant
constexpr int kF2 = 10;
constexpr int kVoiceCount = 11;
//: The most notes to a piece. The engine takes a phrase of any length, and
//: cutting one is heard; this only bounds the matching (match), whose table
//: grows with the square of a piece's length: five hundred notes, minutes of
//: singing without a rest, is a few megabytes.
constexpr size_t kPieceWords = 500;
//: how much of a held sound's either end keeps its own speed
constexpr double kEdge = 0.03;
//: the most of a vowel the breath before it takes, at its own length
constexpr double kTailShare = 0.4;
//: how far under a held vowel's loudest frame, in decibels, the frames it is
//: drawn out over may be; how far over its lowest their nasal resonator may
//: be, in the engine's steps; and how far its second formant may move after
//: that for the vowel to have settled before it
constexpr int kLoudDb = 2, kNasalSteps = 6, kSettledSteps = 10;
//: the fade at the end of a piece, for a rest too short to hear the engine's
//: own fall into silence through
constexpr double kEndFade = 0.005;
//: The notes the engine sings, F1 to D6. Its own pitches make periods of 22
//: to 221 samples, G#1 to B4; tvtts_speak_frames_periods plays them longer,
//: to the 255.99 samples 16 bits of 256ths reach, and shorter, with a glottal
//: pulse shorter than any in the engine's table, the voicing holding its
//: level to about 9 samples. A phrase outside them goes whole octaves into
//: them, and what still does not fit is held to the nearest.
constexpr int kLowestNote = 29, kHighestNote = 86;
//: the shortest and longest periods sung, a bend or vibrato past the notes and
//: all: whole samples, so that one rounded to the nearest stays inside
constexpr double kShortestPeriod = 9, kLongestPeriod = 255;
constexpr double kMinHz = double(kEngineRate) / kLongestPeriod, kMaxHz = double(kEngineRate) / kShortestPeriod;

//: the pitch the engine sings at `t` seconds into the phrase, moved by
//: `shift` octaves into its range
double sung_hz(const PitchCurve &curve, double shift, double t) {
    return std::clamp(curve.hz_at(std::clamp(t, 0.0, curve.total())) * shift, kMinHz, kMaxHz);
}

//: the period the engine is given for a pitch, in TVTTS_PERIOD_UNIT to the
//: sample: exactly, or for a voice retuned afterwards the whole number of
//: samples nearest it
uint16_t period_of(double hz, bool whole) {
    double p = std::clamp(kEngineRate / hz, kShortestPeriod, kLongestPeriod);
    if (whole) p = std::round(p);
    return uint16_t(std::lround(p * TVTTS_PERIOD_UNIT));
}

// The voices are evened out as the other engines' are: each was sung through
// ten vowels on notes across its range and scaled so that its median peak is
// 0.5 (truvoice_sing_test --levels).
const double kGains[kVoiceCount] = {1.68, 1.45, 1.29, 1.15, 2.39, 0.84, 1.44, 1.29, 2.23, 1.94, 0.83};
//: The voices whose creak or breath is heard, and so are sung on exact periods
//: with nothing done to the sound after: Grandpa Amos, Melvin and Alex, whose
//: jitter Centigram set at 5, 2 and 2 (OpenTV's docs/VOICES.md, "Creakiness"),
//: and Frank, who has none but breathes. Retuned, Amos's periodicity went from
//: his own 0.82 to 0.96 and Frank's from 0.87 to 0.93; on exact periods they
//: stay at 0.80 and 0.86 (truvoice_sing_test --creak). The others' jitter, 1
//: or none, acts only above about 190 Hz, and the pattern of periods a sample
//: apart that exact periods are made of has nothing to hide in: Peter went
//: from 1.00 to 0.97 with it, and retuned stays at 1.00.
const bool kCreaks[kVoiceCount] = {false, false, false, false, false, true, true, true, false, false, true};
//: which way every voice is sung, for the engine's test to compare: -1 as
//: kCreaks says, 0 exact periods and no retune, 1 whole periods retuned
int g_retune = -1;
//: as the engine names them, in its own order; Frank is OpenTV's own
const char *const kVoiceNames[kVoiceCount] = {"Peter", "Sidney", "Eager Eddie", "Deep Douglas", "Biff", "Grandpa Amos",
                                              "Melvin", "Alex", "Wanda", "Julia", "Frank"};

// VocalWriter's fifty-seven phonemes as TruVoice's codes, which are the Prose
// 2000's (prose.cpp): its r-coloured vowels, YU, syllabic l and n and dark l
// are codes of their own; CH and JH are a closure and a release, as the engine
// spells them itself; the flaps of "better" and "greater" are its flap, t, and
// the t of "it" its glottalised t, q. O, the first part of OY, is the vowel of
// "caught".
struct PhoneMap {
    const char *vw;
    const char *code;
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

//: One of VocalWriter's phonemes, as TruVoice's codes, from `start` to `end`
//: in seconds from the phrase's start. `held` is the note's held one.
struct Group {
    std::string codes;
    double start = 0, end = 0;
    bool held = false;
};

//: A piece: its groups in order, and the phonemes that say them.
struct Piece {
    std::vector<Group> groups;
    std::string text;
    //: the end of the rest after it, if one follows
    double fall = -1;
};

using Frame = std::array<uint8_t, kParms>;

//: A phoneme the engine played, from `start` to `end` in seconds.
struct Segment {
    char code;
    double start, end;
};

//: what the engine made of a piece: its frames, each with where it starts in
//: seconds, and the phonemes it played
struct Spoken {
    std::vector<Frame> frames;
    std::vector<double> at;
    std::vector<Segment> segments;
};

//: A stretch of the engine's frames, `s0` to `s1` in seconds of its own time,
//: sung from `t0` to `t1` in the phrase's.
struct Part {
    double s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    bool held = false;
    //: in a held one, the stretch drawn out: before and after it the sound
    //: keeps its own speed
    double a = -1, b = -1;
};

// The engine keeps some of its working state in globals (its stage 2), so one
// phrase is sung at a time.
std::mutex g_engine;

class TruVoiceSinger : public Singer {
public:
    explicit TruVoiceSinger(int v) : v_(v) {}

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        std::vector<Piece> pieces = lay_out(phrase, curve);

        std::lock_guard<std::mutex> lock(g_engine);
        // A synth a phrase, so that a phrase sings the same each time: the
        // engine's noise carries on from one utterance to the next.
        std::unique_ptr<tvtts_synth, void (*)(tvtts_synth *)> synth(tvtts_create(kEngineRate),
                                                                    [](tvtts_synth *x) { tvtts_destroy(x); });
        tvtts_synth *s = synth.get();
        if (!s) {
            sung.error = "TruVoice could not be started";
            return sung;
        }
        tvtts_set_voice(s, v_);
        // a voice that creaks is sung on exact periods and heard as the engine
        // makes it; a steady one is given whole periods and retuned
        const bool retune = g_retune < 0 ? !kCreaks[v_] : g_retune == 1;

        // whole octaves into the notes the engine sings
        double lo = 1e9, hi = -1e9, shift = 1;
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            bool voiced = false;
            for (const std::string &ph : phrase.notes[i].phonemes) {
                const PhoneMap *m = find_map(ph);
                if (m && *m->code) voiced = true;
            }
            if (!voiced) continue;
            double m = curve.steady_midi_at((curve.note_start(i) + curve.note_end(i)) / 2);
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        shift = std::pow(2.0, octaves_to_fit(lo, hi, kLowestNote, kHighestNote));

        std::vector<int16_t> pcm;
        // where each frame sung starts, in samples, and its pitch (0 unvoiced)
        std::vector<std::pair<size_t, double>> sung_at;
        for (const Piece &piece : pieces) {
            // up to where the piece starts, silence
            long from = std::lround(piece.groups.front().start * kEngineRate);
            if (long(pcm.size()) < from) pcm.resize(size_t(from), 0);
            Spoken said;
            if (!speak(s, piece.text, &said)) {
                sung.error = "TruVoice could not say a phrase";
                return sung;
            }
            if (said.frames.empty()) continue;
            std::vector<Frame> out = lay_frames(piece, said, pcm.size());
            std::vector<uint16_t> periods(out.size(), 0);
            for (size_t k = 0; k < out.size(); ++k) {
                size_t at = pcm.size() + k * kFrameSamples;
                double hz = sung_hz(curve, shift, (double(at) + 0.5 * kFrameSamples) / kEngineRate);
                bool voiced = out[k][kAV] > 0;
                if (voiced) {
                    // the byte is the pitch, which says how deep the voice's
                    // jitter goes; the period is given
                    out[k][kF0] = uint8_t(std::clamp(std::lround(hz / 2), 25L, 255L));
                    periods[k] = period_of(hz, retune);
                }
                sung_at.push_back(
                    {at, voiced ? double(kEngineRate) * TVTTS_PERIOD_UNIT / double(periods[k]) : 0.0});
            }
            if (!render(s, out, periods, &pcm)) {
                sung.error = "TruVoice could not sing a phrase";
                return sung;
            }
        }
        // and silence to the end of the last rest
        long end = std::lround(curve.total() * kEngineRate);
        if (long(pcm.size()) < end) pcm.resize(size_t(end), 0);

        std::vector<float> whole(pcm.begin(), pcm.end());
        if (retune) {
            // the pitch the frames were given, rather than measured: a
            // tracker lags a glide between two short notes, and the periods
            // found across it are the wrong length
            dsp::F0Track f0 = dsp::frame_track(sung_at, kFrameSamples, whole.size(), kEngineRate);
            whole = dsp::psola_matched(
                whole, kEngineRate, f0, whole.size(), [](double t) { return t; },
                [&curve, shift](double t) { return sung_hz(curve, shift, t); });
        }
        std::vector<float> samples = resample_to_output(whole, kEngineRate);
        // to +-1 at the part's level; a phrase that would still clip is
        // brought down whole rather than cut off
        double gain = phrase.level * kGains[v_] / 32768.0, peak = 0.0;
        for (float &x : samples) {
            x = float(x * gain);
            peak = std::max(peak, double(std::fabs(x)));
        }
        if (peak > 0.999) {
            float k = float(0.999 / peak);
            for (float &x : samples) x *= k;
        }
        sung.samples = std::move(samples);
        return sung;
    }

private:
    // A piece traced: its frames and the phonemes the engine played, from the
    // first frame after the lead every utterance starts on.
    static bool speak(tvtts_synth *s, const std::string &text, Spoken *said) {
        struct Trace {
            Spoken *said;
            std::vector<std::pair<char, uint32_t>> starts;
        } trace{said, {}};
        int r = tvtts_phonemes_to_frames(
            s, text.c_str(),
            [](const tvtts_frame_event *ev, void *user) -> int {
                auto *t = static_cast<Trace *>(user);
                if (ev->type == TVTTS_FRAME) {
                    Frame f;
                    std::copy(ev->tracks, ev->tracks + kParms, f.begin());
                    t->said->frames.push_back(f);
                    t->said->at.push_back(double(ev->frame) * kFrameSamples / kEngineRate);
                } else if (ev->type == TVTTS_PHONEME) {
                    t->starts.push_back({char(ev->phoneme), ev->frame});
                }
                return 0;
            },
            &trace);
        if (r != 0) return false;
        // A pause the engine makes between two phonemes is not sung: its
        // frames are taken out, and what comes after moves up. It makes one
        // between an s and a w, l, n or m after it, as it speaks "sweet" and
        // "sleep", the s dying away over 30 ms; in a song that is a gap in
        // the middle of a word, and with the w squeezed into the note before,
        // the frame the w was given was one of the s's. The pauses before the
        // first phoneme and after the last, the fall into silence, stay.
        const auto &st = trace.starts;
        size_t first = 0, end = st.size();
        while (first < end && st[first].first == ' ') ++first;
        while (end > first && st[end - 1].first == ' ') --end;
        std::vector<std::pair<char, uint32_t>> starts;
        std::vector<bool> keep(said->frames.size(), true);
        uint32_t gone = 0;
        for (size_t i = 0; i < st.size(); ++i) {
            if (st[i].first != ' ' || i <= first || i >= end) {
                starts.push_back({st[i].first, st[i].second - gone});
                continue;
            }
            for (uint32_t f = st[i].second; f < st[i + 1].second && f < keep.size(); ++f, ++gone) keep[f] = false;
        }
        size_t kept = 0;
        for (size_t f = 0; f < keep.size(); ++f)
            if (keep[f]) {
                said->frames[kept] = said->frames[f];
                said->at[kept] = double(kept) * kFrameSamples / kEngineRate;
                ++kept;
            }
        said->frames.resize(kept);
        said->at.resize(kept);
        // each phoneme lasts until the next one starts, or the frames end
        double last = double(said->frames.size()) * kFrameSamples / kEngineRate;
        for (size_t i = 0; i < starts.size(); ++i) {
            double a = double(starts[i].second) * kFrameSamples / kEngineRate;
            double b = i + 1 < starts.size() ? double(starts[i + 1].second) * kFrameSamples / kEngineRate : last;
            said->segments.push_back({starts[i].first, std::min(a, last), std::min(std::max(a, b), last)});
        }
        return true;
    }

    // A piece's frames, synthesised in one go with the periods they were
    // given, after the silent lead the engine starts every utterance on.
    static bool render(tvtts_synth *s, const std::vector<Frame> &out, const std::vector<uint16_t> &periods,
                       std::vector<int16_t> *pcm) {
        std::vector<uint8_t> flat(out.size() * kParms);
        for (size_t k = 0; k < out.size(); ++k) std::copy(out[k].begin(), out[k].end(), flat.begin() + long(k * kParms));
        struct Into {
            std::vector<int16_t> *pcm;
            size_t skip;
        } into{pcm, size_t(TVTTS_LEAD_FRAMES) * kFrameSamples};
        size_t from = pcm->size();
        int r = tvtts_speak_frames_periods(
            s, flat.data(), periods.data(), uint32_t(out.size()),
            [](const tvtts_event *ev, void *user) -> int {
                auto *t = static_cast<Into *>(user);
                if (ev->type != TVTTS_AUDIO) return 0;
                uint32_t k = 0;
                if (t->skip) {
                    k = uint32_t(std::min<size_t>(t->skip, ev->count));
                    t->skip -= k;
                }
                t->pcm->insert(t->pcm->end(), ev->samples + k, ev->samples + ev->count);
                return 0;
            },
            &into);
        if (r != 0) return false;
        // A rest too short for the engine's own fall into silence ends the
        // piece before it is silent; the last few milliseconds fade, so that
        // the ringing it is cut off in is not heard as a click.
        size_t got = pcm->size() - from, fade = std::min(got, size_t(kEndFade * kEngineRate));
        for (size_t k = 0; k < fade; ++k) {
            int16_t &x = (*pcm)[pcm->size() - 1 - k];
            x = int16_t(std::lround(x * double(k) / double(fade)));
        }
        return true;
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
                // A reduced vowel and an n or l after it the engine makes one
                // syllabic n or l, and the note loses the vowel it is sung on;
                // a morph boundary between them keeps them apart.
                std::string codes;
                for (const Group &g : w) codes += g.codes;
                piece.text += '&';
                // Before a word that starts on its vowel the engine puts a
                // pause of 50 ms, which keeps two vowels apart in speech and
                // in a song is a hole between two notes -- "your | answer",
                // "crazy | all". A morph boundary just inside the word stops
                // it, and every phoneme keeps the length it had.
                if (!codes.empty() && nucleus(codes[0])) piece.text += '[';
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
                if (!m || !*m->code) continue;
                g.codes = m->code;
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

    // Where each code written was played: the engine's phonemes matched to
    // them in order, the fewest changes apart. A code its rules changed into
    // another is matched all the same, a vowel to a vowel rather than to a
    // consonant (an r-coloured vowel is a vowel and r joined); one it put in
    // belongs to the code before it; codes it joined into one share that one
    // between them. A pause costs nothing to pass over.
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

    // The frames the engine made of a piece, laid out on the phrase's time
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
        // each, and the engine's fall into a pause after them, keep their own
        // speed. Of a vowel the engine nasalises for a nasal after it, raising
        // its nasal resonator, the frames it has nasalised are left out when
        // it has settled before them, its second formant moving no further;
        // one still on its way there keeps them.
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
        // Outside a held vowel the engine's own frames, not drawn between:
        // drawn, a stop's one frame of burst is smeared into the frames
        // either side of it and all but lost, and a b is heard as a v. Where
        // the sound goes faster than its own speed, the frame of those passed
        // over with the most noise in it, so that no burst is skipped.
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
                double in = u.a - u.s0, out_ = u.s1 - u.b;
                if (tau < in) s = u.s0 + tau;
                else if (tau > T - out_) s = u.s1 - (T - tau);
                else s = u.a + (tau - in) * (u.b - u.a) / (T - in - out_);
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
    // `tail` at their own length (no more than kTailShare of it between them)
    // and the held one the rest; otherwise each in proportion to its own
    // length.
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

class TruVoiceFamily : public Family {
public:
    std::string name() const override { return "TruVoice"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int i = 0; i < kVoiceCount; ++i) out.push_back({kTruVoiceBase + i, std::string("TruVoice ") + kVoiceNames[i]});
        return out;
    }

    bool available(std::string *) override { return true; }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int v = voice_id - kTruVoiceBase;
        if (v < 0 || v >= kVoiceCount) {
            if (err) *err = "There is no TruVoice voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        return std::make_unique<TruVoiceSinger>(v);
    }
};

}  // namespace

std::unique_ptr<Family> make_truvoice_family() { return std::make_unique<TruVoiceFamily>(); }

//: Not part of family.h, as truvoice_self_check is not: how the engine's test
//: picks which way the voices are sung, to hear and measure both. -1 is the
//: program's way, voice by voice; 0 and 1 are one way for all of them.
void truvoice_set_retune(int mode) { g_retune = mode; }

//: Not part of family.h: the self-check the engine's test program runs. Every
//: VocalWriter phoneme has a row in the table and every row is one of
//: VocalWriter's, and the voices are the engine's own, by the names it gives
//: them. Returns an empty string, or what is wrong.
std::string truvoice_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes())
        if (!find_map(p.symbol)) bad += std::string("no TruVoice code for ") + p.symbol + "; ";
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    if (tvtts_voice_count() < kVoiceCount)
        bad += "the engine has " + std::to_string(tvtts_voice_count()) + " voices, not " +
               std::to_string(kVoiceCount) + "; ";
    for (int i = 0; i < kVoiceCount && i < tvtts_voice_count(); ++i)
        if (std::string(tvtts_voice_name(i)) != kVoiceNames[i])
            bad += std::string("voice ") + std::to_string(i) + " is " + tvtts_voice_name(i) + ", not " +
                   kVoiceNames[i] + "; ";
    return bad;
}

}  // namespace svs
