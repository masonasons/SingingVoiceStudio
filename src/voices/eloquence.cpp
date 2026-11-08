// eloquence.cpp -- Eloquence's voices, singing.
//
// Eloquence is Eloquent Technology's formant synthesiser, later IBM's
// ViaVoice TTS, the voice of JAWS and of NVDA's Eloquence add-on, as
// third_party/openevv reimplements it from IBM's binaries, sample for sample.
// The program sings with its US English, compiled in with its rules and
// dictionary (cmake/engines/eloquence.cmake), so it needs no data.
//
// The engine has no way to be told how long a sound lasts or what pitch it
// is on, so it is told neither. A phrase is written in the engine's own
// phoneme alphabet, every note a stressed word of its own, and spoken once
// with the frames kept rather than synthesised: every five milliseconds a
// frame of the synthesiser's sixty-two parameters, and the frame generator
// says what stretch of time each of its runs is for, one run to a phoneme
// (two to an affricate or a diphthong; an h has none, being the sound after
// it breathed). Then the frames are laid out again: each sound is stretched
// or squeezed to the length the studio gives it, a held vowel's transitions
// and the breath before it kept at their own speed and the vowel drawn out
// where it is steadiest, with every frame's f0 the studio's pitch curve at
// that moment. And the phrase is spoken a second time, with those frames
// synthesised in place of the engine's own (the build's hooks; see
// eloquence.cmake), so the engine's own synthesiser and output stage are
// what is heard.
//
// The synthesiser starts every glottal pulse on a whole sample, carrying
// what is left of the period over to the next one, so a pitch between whole
// periods comes out as periods a sample apart in turn: at 11025 Hz, about
// 2 per cent of a period at C4, heard when sung as hoarseness. So each frame
// is given the pitch of the whole period nearest the curve's, and the
// phrase is retuned to the curve afterwards by pitch-synchronous
// overlap-add (psola_matched, macintalk_dsp.h), every period moved by less
// than half a sample. Diplophonia, the creak the engine's rules give some
// sounds, alternates long and short periods, which sung is a voice dropping
// an octave, so it is taken out.
//
// A long phrase is sung in pieces of forty notes. A rest ends a piece, and
// the engine's own fall into silence is heard in it.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <eci.h>

#include "core/phonology.h"
#include "voices/family.h"
#include "voices/macintalk_dsp.h"
#include "voices/pitch.h"

extern "C" {
// added to the engine by eloquence.cmake
extern int (*svs_evv_frame_hook)(void *d, const int32_t *frame);
extern void (*svs_evv_run_hook)(int32_t from, int32_t to);
int svs_evv_synth_frame(void *d, const int32_t *frame);
void svs_evv_restart(void *d);
// what its library's entry point would have run
void evv_port_start(void);
void evvRunStaticInitialisers(void);
}

namespace svs {

namespace {

constexpr int kEngineRate = 11025;
//: a frame's length, and what the synthesiser makes of it at kEngineRate:
//: 55.125 samples, of which it keeps 55
constexpr int kStepMs = 5;
constexpr int kFrameSamples = 55;
constexpr int kParms = 62;
constexpr int kF0 = 1;   //: in tenths of a hertz
constexpr int kDi = 6;   //: diplophonia, in per cent
//: the sources' amplitudes, in decibels: voicing, aspiration and frication
constexpr int kAmplitudes[] = {2, 7, 8};
//: the first three formants, and the most each moves in a frame while the
//: voice sounds, in hertz: a resonator retuned further at once rings, and
//: the second's 900 Hz drop from the i of "me" to a w is heard as a pop
constexpr int kFormants[] = {9, 13, 15};
constexpr int kSlewHz[] = {150, 300, 375};
//: a nasal's murmur, its first formant at 200 Hz, has nothing to ring in a
//: voice above it, and is all but silent: the formant is kept this far over
//: the pitch sung
constexpr double kMurmurOverF0 = 1.25;
//: a voice starting out of silence, its first frames this many decibels
//: down: the engine starts it at full strength, a click before the vowel
constexpr int kOnsetDb[] = {12, 6, 2};
//: headroom: the sources 9 dB down, as the synthesiser's sums would
//: otherwise overflow on a loud vowel sung high, given back in floats
constexpr int kCut = 9;
const double kCutBack = std::pow(10.0, kCut / 20.0);
constexpr int kVoiceCount = 8;
//: notes to a piece, short of anything the engine minds
constexpr size_t kPieceWords = 40;
//: what the engine is given to say the second time through: one sound, for
//: its synthesiser to be asked for a frame and given the piece's instead
const char *const kCue = "`[.1a]";
//: how much of a held sound's either end keeps its own speed
constexpr double kEdge = 0.03;
//: the most of a vowel its glide or its r takes, at their own length
constexpr double kTailShare = 0.4;
//: how far the first two formants may move between two frames, in hertz,
//: for a vowel to be still where it is held
constexpr int kSteadyHz = 20;
//: the lowest and highest f0 sung, in hertz
constexpr double kMinHz = 40, kMaxHz = 1500;

//: the pitch sung at `t` seconds into the phrase
double sung_hz(const PitchCurve &curve, double t) {
    return std::clamp(curve.hz_at(std::clamp(t, 0.0, curve.total())), kMinHz, kMaxHz);
}

const char *const kVoiceNames[kVoiceCount] = {"Reed", "Shelley", "Bobby", "Rocko",
                                              "Glen", "Sandy",   "Grandma", "Grandpa"};

// The voices are evened out as the other engines' are: each was sung through
// ten vowels on notes across its range and scaled so that its median peak
// is 0.5 (eloquence_sing_test --levels).
const double kGains[kVoiceCount] = {0.80, 0.82, 1.00, 1.03, 0.79, 0.88, 1.07, 1.01};

// VocalWriter's fifty-seven phonemes in Eloquence's alphabet, as it spells
// them itself: an r-coloured vowel is its vowel and r ("car" is kar), YU is
// y and u, syllabic l is a reduced vowel and l and syllabic n its own N. The
// flaps of "better" and "greater" are its flap, F; the t of "it" is t. O, the
// first part of OY, is the vowel of "caught".
struct PhoneMap {
    const char *vw;
    const char *evv;
};
const PhoneMap kPhoneMap[] = {
    {"IY", "i"},  {"IH", "I"},  {"EH", "E"},  {"AE", "A"},  {"AA", "a"},  {"UX", "H"},  {"AO", "c"},
    {"UH", "U"},  {"AX", "x"},  {"ER", "R"},  {"EY", "e"},  {"AY", "Y"},  {"OY", "O"},  {"AW", "W"},
    {"OW", "o"},  {"UW", "u"},  {"YU", "yu"}, {"IR", "ir"}, {"XR", "er"}, {"AR", "ar"}, {"OR", "cr"},
    {"UR", "ur"}, {"IX", "X"},  {"%", ""},    {"RX", "R"},  {"LX", "l"},  {"EL", "xl"}, {"EN", "N"},
    {"w", "w"},   {"y", "y"},   {"r", "r"},   {"l", "l"},   {"h", "h"},   {"m", "m"},   {"n", "n"},
    {"NG", "G"},  {"f", "f"},   {"v", "v"},   {"TH", "T"},  {"DH", "D"},  {"s", "s"},   {"z", "z"},
    {"SH", "S"},  {"ZH", "Z"},  {"p", "p"},   {"b", "b"},   {"t", "t"},   {"d", "d"},   {"k", "k"},
    {"g", "g"},   {"CH", "C"},  {"JH", "J"},  {"TX", "t"},  {"Q", "F"},   {"QX", "F"},  {"DD", "F"},
    {"O", "c"},
};

const PhoneMap *find_map(const std::string &symbol) {
    std::string s = palette_alias(symbol);   // OH and DX, as VocalWriter's MIDI files spell them
    for (const PhoneMap &m : kPhoneMap)
        if (s == m.vw) return &m;
    return nullptr;
}

//: what a word must have one of: the vowels, and r, n and m said as one
bool nucleus(char c) { return std::strchr("iIeEAacoUuHxXRYWONM", c) != nullptr; }

//: whether a note that starts on `next` goes on with the sound `before`
//: ended on: the same vowel, or the end of the diphthong it was
bool carries_on(const std::string &before, const std::string &next) {
    if (before.empty() || next.size() != 1 || !nucleus(next[0]) || !nucleus(before.back())) return false;
    char a = before.back(), b = next[0];
    if (a == b) return true;
    for (const char *pair : {"ou", "Wu", "ei", "Yi", "Oi"})
        if (a == pair[0] && b == pair[1]) return true;
    return false;
}

//: whether a note that starts on the vowel `next` goes on from the vowel
//: `before` ended on, as the song spells a diphthong over two notes:
//: "ni-ght" as AA, AY, "sta-rts" as AA, AR, "bra-in" as EH, EY
bool begins_as(const std::string &before, const std::string &next) {
    if (before.size() != 1 || next.empty() || !nucleus(next[0])) return false;
    for (const char *pair : {"aY", "aW", "aar", "Ee", "Eer", "cO", "ccr", "Iir", "Uur"})
        if (before[0] == pair[0] && next == pair + 1) return true;
    return false;
}
//: how many runs the frame generator gives a phoneme
int runs_of(char c) {
    if (c == 'h') return 0;
    return std::strchr("CJYWO", c) ? 2 : 1;
}

//: One of VocalWriter's phonemes, as the engine's phonemes, from `start` to
//: `end` in seconds from the phrase's start. `held` is the note's held one.
struct Group {
    std::string evv;
    double start = 0, end = 0;
    bool held = false;
    bool coda = false;   //: after the note's held one
};

//: A piece: its groups in order, and the text that says them.
struct Piece {
    std::vector<Group> groups;
    std::string text;
    //: the end of the rest after it, if one follows
    double fall = -1;
};

using Frame = std::array<int32_t, kParms>;

//: A stretch of the engine's frames, `s0` to `s1` in seconds of its own
//: time, sung from `t0` to `t1` in the phrase's.
struct Part {
    double s0 = 0, s1 = 0, t0 = 0, t1 = 0;
    //: in a held one, the stretch drawn out: before and after it the sound
    //: keeps its own speed
    double a = -1, b = -1;
    bool nasal = false;
};

// The hooks the build adds are global, so one phrase is spoken at a time.
std::mutex g_engine;

//: what the hooks are doing for the phrase being spoken
struct Job {
    bool capture = true;
    std::vector<Frame> frames;
    //: each run's stretch of the engine's time, in seconds
    std::vector<std::pair<double, double>> runs;
    const std::vector<Frame> *out = nullptr;
    bool sent = false;
};
Job *g_job = nullptr;

int capture_frame(void *d, const int32_t *frame) {
    Job *j = g_job;
    if (j->capture) {
        Frame f;
        std::copy(frame, frame + kParms, f.begin());
        j->frames.push_back(f);
        return 1;
    }
    // the second time through: the phrase's frames in place of the first
    // the engine sends, and none of the rest, from a synthesiser started
    // again as a new instance's is, its noise from the same seed
    if (!j->sent) {
        j->sent = true;
        svs_evv_restart(d);
        for (const Frame &f : *j->out)
            if (!svs_evv_synth_frame(d, f.data())) break;
    }
    return 1;
}

void capture_run(int32_t from, int32_t to) {
    if (g_job->capture) g_job->runs.push_back({from / 1000.0, to / 1000.0});
}

void start_engine() {
    static std::once_flag once;
    std::call_once(once, [] {
        evv_port_start();
        evvRunStaticInitialisers();
    });
}

class EvvSinger : public Singer {
public:
    explicit EvvSinger(int v) : v_(v) {}
    ~EvvSinger() override {
        std::lock_guard<std::mutex> lock(g_engine);
        engine_.reset();
    }

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        std::vector<Piece> pieces = lay_out(phrase, curve);

        std::vector<int16_t> pcm;
        // where each frame sung starts, in samples, and its pitch (0 unvoiced)
        std::vector<std::pair<size_t, double>> sung_at;
        std::lock_guard<std::mutex> lock(g_engine);
        // One instance for the voice, kept: a new one costs as much as
        // singing a short phrase, and its first utterance as much again.
        if (!engine_) engine_ = std::make_unique<Instance>(v_);
        if (!engine_->h) {
            engine_.reset();
            sung.error = "Eloquence could not be started";
            return sung;
        }
        Instance &h = *engine_;
        for (const Piece &piece : pieces) {
            // up to where the piece starts, silence
            long from = std::lround(piece.groups.front().start * kEngineRate);
            if (long(pcm.size()) < from) pcm.resize(size_t(from), 0);

            Job job;
            g_job = &job;
            svs_evv_frame_hook = capture_frame;
            svs_evv_run_hook = capture_run;
            bool said = h.speak(piece.text);
            std::vector<Frame> out;
            if (said && !job.frames.empty()) out = lay_frames(piece, job, curve, pcm.size());
            for (size_t k = 0; k < out.size(); ++k)
                sung_at.push_back({pcm.size() + k * kFrameSamples, out[k][kAmplitudes[0]] > 0 ? out[k][kF0] / 10.0 : 0.0});
            job.capture = false;
            job.out = &out;
            h.heard.clear();
            // Said again only to have the synthesiser asked for a frame, and
            // given the piece's instead: one sound does that, and the piece
            // again would only have it worked out a second time.
            if (!out.empty()) said = h.speak(kCue);
            svs_evv_frame_hook = nullptr;
            svs_evv_run_hook = nullptr;
            g_job = nullptr;
            if (!said) {
                engine_.reset();   // and a new one for the next phrase
                sung.error = "Eloquence could not say a phrase";
                return sung;
            }
            pcm.insert(pcm.end(), h.heard.begin(), h.heard.end());
        }
        // and silence to the end of the last rest
        long end = std::lround(curve.total() * kEngineRate);
        if (long(pcm.size()) < end) pcm.resize(size_t(end), 0);

        std::vector<float> whole(pcm.begin(), pcm.end());
        // the pitch the frames were given, rather than measured: a tracker
        // lags a glide between two short notes, and the periods found
        // across it are the wrong length
        dsp::F0Track f0 = dsp::frame_track(sung_at, kFrameSamples, whole.size(), kEngineRate);
        std::vector<float> wide = dsp::psola_matched(
            whole, kEngineRate, f0, whole.size(), [](double t) { return t; },
            [&curve](double t) { return sung_hz(curve, t); });
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
    //: An engine instance speaking with voice `v`, and what it has said.
    struct Instance {
        ECIHand h = NULL_ECI_HAND;
        short buf[4096];
        std::vector<int16_t> heard;

        explicit Instance(int v) {
            start_engine();
            h = eciNew();
            if (h == NULL_ECI_HAND) return;
            eciSetParam(h, eciInputType, 1);   // annotations: `[ ] is a pronunciation
            eciSetParam(h, eciSampleRate, 1);  // 11025 Hz
            eciCopyVoice(h, v + 1, 0);
            eciRegisterCallback(h, &Instance::on_message, this);
            eciSetOutputBuffer(h, int(sizeof buf / sizeof buf[0]), buf);
        }
        ~Instance() {
            if (h != NULL_ECI_HAND) eciDelete(h);
        }
        Instance(const Instance &) = delete;
        Instance &operator=(const Instance &) = delete;

        bool speak(const std::string &text) {
            return eciAddText(h, text.c_str()) && eciSynthesize(h) && eciSynchronize(h);
        }

        static int ECICALL on_message(ECIHand, ECIMessage msg, int param, void *data) {
            auto *self = static_cast<Instance *>(data);
            if (msg == eciWaveformBuffer && param > 0) self->heard.insert(self->heard.end(), self->buf, self->buf + param);
            return eciDataProcessed;
        }
    };

    // The phrase as pieces: runs of notes between its rests, each run cut
    // where it grows too long, every note a word of its own. A note with no
    // vowel in it joins the word before it, or else the one after. One that
    // goes on with the vowel the note before it ended on is that note's
    // syllable still, its vowel held over both; one that goes on to the
    // diphthong that vowel began is said in that note's word: a word of its
    // own would start the vowel afresh, and the voice would dip between them.
    static std::vector<Piece> lay_out(const Phrase &phrase, const PitchCurve &curve) {
        std::vector<Piece> pieces;
        std::vector<std::vector<Group>> words;
        auto close = [&](double fall) {
            if (words.empty()) return;
            // the words with no vowel, given to their neighbours
            auto has_nucleus = [](const std::vector<Group> &w) {
                for (const Group &g : w)
                    for (char c : g.evv)
                        if (nucleus(c)) return true;
                return false;
            };
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
            if (kept.empty()) {
                // nothing to say it with: a reduced vowel, given no time
                Group x;
                x.evv = "x";
                x.start = x.end = carry.back().end;
                carry.push_back(x);
                kept.push_back(carry);
            } else if (!carry.empty()) {
                kept.back().insert(kept.back().end(), carry.begin(), carry.end());
            }
            Piece piece;
            piece.fall = fall;
            for (const auto &w : kept) {
                // stressed, reduced vowels too: a sung note is said in full
                if (!piece.text.empty()) piece.text += ' ';
                piece.text += "`[.1";
                for (const Group &g : w) piece.text += g.evv;
                piece.text += ']';
                piece.groups.insert(piece.groups.end(), w.begin(), w.end());
            }
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
                if (!m || !*m->evv) continue;
                g.evv = m->evv;
                g.held = int(k) == held;
                g.coda = int(k) > held;
                word.push_back(g);
            }
            if (word.empty()) {
                // a rest
                close(curve.note_end(i));
                continue;
            }
            if (!words.empty() && carries_on(words.back().back().evv, word.front().evv)) {
                Group &v = words.back().back();
                v.end = word.front().end;
                v.held = v.held || word.front().held;
                words.back().insert(words.back().end(), word.begin() + 1, word.end());
                continue;
            }
            if (!words.empty() && begins_as(words.back().back().evv, word.front().evv)) {
                words.back().insert(words.back().end(), word.begin(), word.end());
                continue;
            }
            if (words.size() == kPieceWords) close(-1);
            words.push_back(word);
        }
        close(-1);
        return pieces;
    }

    // The frames the engine made of a piece, laid out on the phrase's time
    // from `at`, in samples, and on its pitch curve.
    static std::vector<Frame> lay_frames(const Piece &piece, const Job &job, const PitchCurve &curve, size_t at) {
        const std::vector<Frame> &src = job.frames;
        // when each frame starts, in seconds of the engine's own time
        std::vector<double> when(src.size() + 1, 0.0);
        for (size_t i = 0; i < src.size(); ++i) when[i + 1] = when[i] + std::max(1, src[i][0]) / 1000.0;
        auto run_at = [&](size_t r) { return r < job.runs.size() ? job.runs[r].first : when.back(); };
        // where the voicing starts in a run, or its end
        auto voiced_from = [&](size_t r) {
            double end = r < job.runs.size() ? job.runs[r].second : when.back();
            for (size_t i = 0; i < src.size() && when[i] < end; ++i)
                if (when[i] >= run_at(r) && src[i][2] > 0) return when[i];
            return end;
        };

        // Where a held run is steadiest, to be drawn out there: the frame
        // whose formants move least to the next, and the frames either side
        // of it that move little more, or else its end. The engine moves into
        // a vowel inside its run, an AY's first run all the way to its open
        // start, and held part way the vowel would come in muffled and swell.
        auto moves = [&](size_t i) {
            return std::abs(src[i + 1][kFormants[0]] - src[i][kFormants[0]]) +
                   std::abs(src[i + 1][kFormants[1]] - src[i][kFormants[1]]);
        };
        auto steady = [&](Part *p) {
            size_t i0 = size_t(std::lower_bound(when.begin(), when.end(), p->s0) - when.begin());
            size_t i1 = i0;
            while (i1 + 1 < src.size() && when[i1 + 1] <= p->s1) ++i1;
            if (i1 <= i0) return;
            size_t best = i0;
            for (size_t i = i0; i < i1; ++i)
                if (moves(i) < moves(best)) best = i;
            if (moves(best) > kSteadyHz) {
                // moving all the way, to where it is going
                p->a = p->b = p->s1;
                return;
            }
            size_t lo = best, hi = best + 1;
            while (lo > i0 && moves(lo - 1) <= kSteadyHz) --lo;
            while (hi < i1 && moves(hi) <= kSteadyHz) ++hi;
            p->a = when[lo];
            p->b = when[hi];
        };

        // each phoneme's runs, in order, and the run after them all the
        // engine's fall into silence
        size_t want = 1;
        for (const Group &g : piece.groups)
            for (char c : g.evv) want += size_t(runs_of(c));
        std::vector<Part> parts;
        double fall_at = when.back();
        if (job.runs.size() == want) {
            size_t r = 0;
            double carry = -1;   // after an h, where the sound it breathes starts in its run
            std::vector<size_t> group_of;
            for (const Group &g : piece.groups) {
                // the group's stretches of frames
                std::vector<Part> mine;
                std::vector<bool> tail;
                bool vowel_seen = false;
                for (char c : g.evv) {
                    int n = runs_of(c);
                    if (n == 0) {
                        // breath, up to where its run's voicing starts
                        Part p;
                        p.s0 = run_at(r);
                        p.s1 = voiced_from(r);
                        mine.push_back(p);
                        tail.push_back(true);
                        carry = p.s1;
                        continue;
                    }
                    for (int q = 0; q < n; ++q, ++r) {
                        Part p;
                        p.nasal = std::strchr("mnGNM", c) != nullptr;
                        p.s0 = carry >= 0 ? carry : run_at(r);
                        p.s1 = run_at(r + 1);
                        carry = -1;
                        // a vowel's first run holds; a glide, r or y keeps its
                        // length
                        bool hold = g.held && nucleus(c) && !vowel_seen && q == 0;
                        if (nucleus(c)) vowel_seen = true;
                        if (hold) {
                            // and so does the breath a stop leaves
                            double v = std::min(voiced_from(r), p.s1);
                            if (v > p.s0) {
                                Part breath = p;
                                breath.s1 = p.s0 = v;
                                mine.push_back(breath);
                                tail.push_back(true);
                            }
                        }
                        if (hold) steady(&p);
                        mine.push_back(p);
                        tail.push_back(!hold);
                    }
                }
                share(g, &mine, tail);
                parts.insert(parts.end(), mine.begin(), mine.end());
                group_of.resize(parts.size(), size_t(&g - piece.groups.data()));
            }
            fall_at = run_at(r);
            lengthen_codas(piece, group_of, &parts);
        } else {
            // the engine said the piece some other way: its frames over the
            // piece's whole length
            Part p;
            p.s0 = 0;
            p.s1 = run_at(job.runs.empty() ? 0 : job.runs.size() - 1);
            p.t0 = piece.groups.front().start;
            p.t1 = piece.groups.back().end;
            parts.push_back(p);
            fall_at = p.s1;
        }

        // the frame at `s`, between the two either side of it
        auto frame_at = [&](double s) {
            size_t i = size_t(std::upper_bound(when.begin(), when.end() - 1, s) - when.begin());
            i = i ? i - 1 : 0;
            Frame f = src[i];
            if (i + 1 < src.size()) {
                double len = when[i + 1] - when[i];
                double w = len > 0 ? std::clamp((s - when[i]) / len, 0.0, 1.0) : 0.0;
                for (int k = 2; k < kParms; ++k)
                    f[size_t(k)] = int32_t(std::lround(src[i][size_t(k)] + w * (src[i + 1][size_t(k)] - src[i][size_t(k)])));
            }
            return f;
        };
        std::vector<Frame> out;
        size_t onset = std::size(kOnsetDb);
        // the frame at `s` sung at `t`, in the part `u`
        auto emit = [&](double s, double t, const Part *u = nullptr) {
            Frame f = frame_at(s);
            if (u && u->nasal) {
                auto k = size_t(kFormants[0]);
                f[k] = std::max(f[k], int32_t(std::lround(kMurmurOverF0 * sung_hz(curve, t))));
            }
            f[0] = kStepMs;
            for (int k : kAmplitudes)
                if (f[size_t(k)] > 0) f[size_t(k)] = std::max(1, f[size_t(k)] - kCut);
            const int av = kAmplitudes[0];
            bool silent = out.empty();
            if (!out.empty()) {
                const Frame &last = out.back();
                silent = std::all_of(std::begin(kAmplitudes), std::end(kAmplitudes), [&](int k) { return last[size_t(k)] == 0; });
                if (f[av] > 0 && last[av] > 0)
                    for (size_t q = 0; q < std::size(kFormants); ++q) {
                        auto k = size_t(kFormants[q]);
                        f[k] = std::clamp(f[k], last[k] - kSlewHz[q], last[k] + kSlewHz[q]);
                    }
            }
            if (f[av] > 0 && silent) onset = 0;
            if (f[av] > 0 && onset < std::size(kOnsetDb)) f[av] = std::max(1, f[av] - kOnsetDb[onset++]);
            // the pitch of the whole period nearest the curve's; the
            // retuning afterwards does the rest
            f[kF0] = int32_t(std::lround(kEngineRate * 10.0 / std::round(kEngineRate / sung_hz(curve, t))));
            f[kDi] = 0;
            out.push_back(f);
        };
        double t = double(at) / kEngineRate, step = double(kFrameSamples) / kEngineRate;
        size_t p = 0;
        double end = parts.back().t1;
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
            emit(s, t, &u);
        }
        // the fall into silence at its own speed, as long as the rest allows
        for (double s = fall_at; piece.fall > 0 && t < piece.fall - step / 2 && s < when.back(); s += step, t += step)
            emit(s, t);
        return out;
    }

    // The nasals after the last held vowel before a rest at their own length
    // at least, the time taken from the vowel's drawn-out stretch: in the
    // song's timing an n at the end of a word has a third of the engine's,
    // and with nothing after it is hardly heard. Before another note the
    // move into that note's first sound is heard, and a longer n there would
    // only break the vowel off early. (A stop's closure is silence, and
    // longer would only cut the note short.)
    static void lengthen_codas(const Piece &piece, const std::vector<size_t> &group_of, std::vector<Part> *parts) {
        std::vector<Part> &ps = *parts;
        if (piece.fall <= 0) return;
        for (size_t h = 0; h < ps.size(); ++h) {
            if (ps[h].a < 0) continue;
            size_t first = h + 1;
            while (first < ps.size() && group_of[first] == group_of[h]) ++first;
            size_t end = first;
            double short_by = 0;
            for (; end < ps.size() && piece.groups[group_of[end]].coda; ++end)
                if (ps[end].nasal) short_by += std::max(0.0, (ps[end].s1 - ps[end].s0) - (ps[end].t1 - ps[end].t0));
            if (end < ps.size()) continue;
            double spare = (ps[h].t1 - ps[h].t0) - (ps[h].s1 - ps[h].s0);
            double take = std::min(short_by, spare);
            if (take <= 0) continue;
            ps[h].t1 -= take;
            double t = ps[h].t1;
            for (size_t q = h + 1; q < end; ++q) {
                double len = ps[q].t1 - ps[q].t0, own = ps[q].s1 - ps[q].s0;
                if (q >= first && ps[q].nasal) len += std::max(0.0, own - len) * take / short_by;
                ps[q].t0 = t;
                ps[q].t1 = t += len;
            }
        }
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
    std::unique_ptr<Instance> engine_;
};

class EvvFamily : public Family {
public:
    std::string name() const override { return "Eloquence"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int i = 0; i < kVoiceCount; ++i) out.push_back({kEloquenceBase + i, std::string("Eloquence ") + kVoiceNames[i]});
        return out;
    }

    bool available(std::string *) override { return true; }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int v = voice_id - kEloquenceBase;
        if (v < 0 || v >= kVoiceCount) {
            if (err) *err = "There is no Eloquence voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        return std::make_unique<EvvSinger>(v);
    }
};

}  // namespace

std::unique_ptr<Family> make_eloquence_family() { return std::make_unique<EvvFamily>(); }

//: Not part of family.h: the self-check the engine's test program runs. Every
//: VocalWriter phoneme has a row in the table and every row is one of
//: VocalWriter's. Returns an empty string, or what is wrong.
std::string eloquence_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes())
        if (!find_map(p.symbol)) bad += std::string("no Eloquence phoneme for ") + p.symbol + "; ";
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    return bad;
}

}  // namespace svs
