// VocalWriter's engine, driven one of two ways. Phrase by phrase as
// VocalWriter Studio drives it: the same sequence of calls, in the same
// order, as the VocalWriter repository's ppc/render.py, so the samples are
// the same samples. Or, when the settings ask for it, the way VocalWriter 2.0
// drives it: each part sung as a track of a VocalWriter song, through the
// application's own parser and sequencer (vw_sequencer.h).
#include "voices/vocalwriter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>

#include "core/paths.h"
#include "core/phonology.h"
#include "core/project.h"
#include "voices/controls.h"

extern "C" {
#include "vw_editor.h"
}
#include "voices/vw_sequencer.h"

namespace svs {

namespace {

const char *const kRsrc = "assets\\VocalWriter.app\\Contents\\Resources\\VocalWriter.rsrc";
const char *const kGmSpeech = "assets\\GMSpeech.rsrc";
const char *const kGmBank = "assets\\GMBank.rsrc";
const char *const kEnglishLex = "assets\\EnglishLex";

const int kTicksPerBeat = 240;
//: the portamento VocalWriter's engine starts every voice with, its glide
//: table's twentieth step: a song that wants another, none among them, says so
const int kEnginePortamento = 40;

const int kSamplesPerFrame = 220;
//: How many frames a second is: 200.4545..., the engine's only clock.
const double kFramesASecond = double(kSampleRate) / kSamplesPerFrame;
//: calibrated so frames per beat matches the above
const float kTempoScale = 1.0f / 240.0f;
const int kMaxFrames = 200000;
//: Speech_PitchBend wants MIDI's 14-bit bend times 8 for a full-scale bend to
//: reach the whole range (measured: 0 cents error at 12 semitones).
const int kBendScale = 8;
//: the bend range the engine is put into before any bend is sent, phrase by phrase
const int kBendRange = 12;

std::atomic<bool> g_own_path{false};
std::atomic<bool> g_gm_octaves{false};

//: Python's round(): halves go to the even neighbour.
long round_half_even(double x) { return long(std::nearbyint(x)); }

//: What the engine will make of a beat at this tempo, to the last bit:
//: 12027.2727273 / (tempoMul * bpm) / 240 in single precision.
double frames_per_beat(int bpm) {
    float product = kTempoScale * float(bpm);
    return double(float(12027.2727273 / double(product) / 240.0));
}

int engine_tempo(double bpm) {
    return int(std::max(10L, std::min(250L, round_half_even(bpm))));
}

//: A bend as MIDI's 14-bit bend about zero, in a range of `range` semitones.
int bend_raw(double semitones, int range) {
    long v = round_half_even(semitones / range * 8192);
    return int(std::max(-8192L, std::min(8191L, v)));
}

}  // namespace

void set_vocalwriter_own_path(bool on) { g_own_path = on; }
bool vocalwriter_own_path() { return g_own_path; }
void set_gm_octaves(bool on) { g_gm_octaves = on; }
bool gm_octaves() { return g_gm_octaves; }

// -- the data ---------------------------------------------------------------

VwAssets &VwAssets::get() {
    static VwAssets instance;
    return instance;
}

VwAssets::VwAssets() {
    const char *required[] = {kRsrc, kGmSpeech, kEnglishLex};
    for (const char *r : required)
        if (!data_exists(r)) missing_.push_back(r);
    root_ = data_roots().empty() ? "." : data_roots()[0];
    if (!missing_.empty()) return;
    std::string rsrc_path = find_data(kRsrc);
    root_ = rsrc_path.substr(0, rsrc_path.size() - std::strlen(kRsrc));
    ok_ = read_file(rsrc_path, &rsrc) && read_file(find_data(kGmSpeech), &gmspeech) &&
          read_file(find_data(kEnglishLex), &lexicon);
    // The instrument bank is not needed to sing with the natural voices; a
    // copy without it still works, with fewer voices to choose from.
    if (data_exists(kGmBank)) read_file(find_data(kGmBank), &bank);
}

vw_editor *VwAssets::open_editor() const {
    if (!ok_) return nullptr;
    vw_editor *e = vw_ed_open(rsrc.data(), rsrc.size(), gmspeech.data(), gmspeech.size());
    if (!e) return nullptr;
    if (!bank.empty() && vw_ed_bank(e, bank.data(), bank.size()) != 0) {
        // a damaged bank leaves the natural voices working
    }
    return e;
}

// -- the dictionary -----------------------------------------------------------

Lexicon &Lexicon::get() {
    static Lexicon instance;
    return instance;
}

Lexicon::Lexicon() {
    VwAssets &a = VwAssets::get();
    if (!a.ok()) return;
    ed_ = a.open_editor();
    // the engine reads the dictionary where it lies, so it must stay put
    if (ed_ && vw_ed_lexicon(ed_, a.lexicon.data(), a.lexicon.size()) != 0) {
        vw_ed_close(ed_);
        ed_ = nullptr;
    }
}

namespace {

//: A letter on its own, said as its name: "d" is "d IY". Not "a", a word;
//: the dictionary knows few of the others.
std::vector<std::string> letter_name(char c, bool marked) {
    static const char *const names[26] = {
        nullptr,  "b IY", "s IY",  "d IY", "IY",     "EH f", "JH IY",          "EY CH", "AY",
        "JH EY",  "k EY", "EH LX", "EH m", "EH n",   "OW",   "p IY",           "k YU",  "AR",
        "EH s",   "t IY", "YU",    "v IY", "d UX . b EL . YU", "EH k s", "w AY", "z IY"};
    const char *name = names[std::tolower((unsigned char)c) - 'a'];
    std::vector<std::string> out;
    if (!name) return out;
    std::string sym;
    for (const char *p = name;; ++p) {
        if (*p && *p != ' ') {
            sym.push_back(*p);
            continue;
        }
        if (!sym.empty() && (marked || sym != kSyllableBreak)) out.push_back(sym);
        sym.clear();
        if (!*p) break;
    }
    return out;
}

}  // namespace

std::vector<std::string> Lexicon::phonemes(const std::string &word, bool marked) {
    // figures said as words, each looked up: "39" as "thirty nine"
    std::vector<std::string> said = number_words(word);
    if (!said.empty()) {
        std::vector<std::string> out;
        for (const std::string &w : said) {
            std::vector<std::string> one = phonemes(w, marked);
            if (one.empty()) continue;
            if (marked && !out.empty()) out.push_back(kSyllableBreak);
            out.insert(out.end(), one.begin(), one.end());
        }
        return out;
    }
    std::vector<std::string> pieces(1);
    for (unsigned char c : word) {
        if (c == '-' && !pieces.back().empty()) pieces.emplace_back();
        else if (std::isalpha(c) || c == '\'') pieces.back().push_back(char(c));
    }
    if (pieces.back().empty()) pieces.pop_back();
    std::string whole;
    for (const std::string &p : pieces) whole += p;
    if (whole.size() == 1 && std::isalpha((unsigned char)whole[0])) {
        std::vector<std::string> name = letter_name(whole[0], marked);
        if (!name.empty()) return name;
    }
    if (whole.empty() || !ed_) return {};
    std::lock_guard<std::mutex> lock(mu_);
    bool known = false;
    std::vector<std::string> got = look_up(whole, marked, &known);
    if (known || pieces.size() < 2) return got;
    // a word the dictionary does not know is sung as its pieces, if it
    // knows every one of them: "jail-birds" as "jail" and "birds". A piece
    // of one or two letters is a syllable rather than a word ("A-la-ba-my").
    std::vector<std::string> joined;
    for (const std::string &p : pieces) {
        if (std::count_if(p.begin(), p.end(), [](unsigned char c) { return std::isalpha(c); }) < 3) return got;
        std::vector<std::string> one = look_up(p, marked, &known);
        if (!known || one.empty()) return got;
        if (marked && !joined.empty()) joined.push_back(kSyllableBreak);
        joined.insert(joined.end(), one.begin(), one.end());
    }
    return joined;
}

std::vector<std::string> Lexicon::look_up(const std::string &clean, bool marked, bool *known) {
    unsigned char out[10 * 9] = {0};
    int in_lexicon = 0;
    int n = vw_ed_word(ed_, clean.c_str(), out, &in_lexicon);
    *known = in_lexicon != 0;
    if (n <= 0) return {};
    const auto &table = svs::phonemes();
    std::vector<std::string> phones;
    std::vector<int> syllable;
    for (int i = 0; i < n && i < 10; ++i) {
        int count = out[i * 9];
        for (int k = 0; k < count && k < 8; ++k) {
            int code = out[i * 9 + 1 + k];
            if (code < int(table.size())) {
                phones.push_back(table[size_t(code)].symbol);
                syllable.push_back(i);
            }
        }
    }
    if (phones.empty()) return {};
    std::vector<std::string> sounds = allophones(phones);
    if (!marked) return sounds;
    // An allophone stands for one phoneme or for two run together ("AX r"
    // is ER); it goes in the syllable of the first.
    std::vector<std::string> out_marked;
    size_t i = 0;
    for (const std::string &s : sounds) {
        if (i >= phones.size()) break;
        if (!out_marked.empty() && syllable[i] != syllable[i - 1] && out_marked.back() != kSyllableBreak)
            out_marked.push_back(kSyllableBreak);
        out_marked.push_back(s);
        size_t took = s != phones[i] && s != "LX" ? 2 : 1;
        // a pair run over a break leaves the break after it
        if (took == 2 && i + 1 < phones.size() && syllable[i + 1] != syllable[i] &&
            i + 2 < phones.size())
            out_marked.push_back(kSyllableBreak);
        i += took;
    }
    return out_marked;
}

// -- singing -------------------------------------------------------------------
//
// Phrase by phrase, a phrase is handed to the engine as a note editor drives
// it (vw_editor.h), the consonants laid before the beat by the renderer as
// for every other engine. Through VocalWriter's own play path
// (vw_sequencer.h), its parser lays the consonants before the beats and fits
// the lengths to the speech rate, and its sequencer sings the track as
// VocalWriter sang it. An instrument of the General MIDI bank is always
// played by the sequencer.

namespace {

//: The sequencer's engine: opened once, as the application opened it at
//: launch, and used by one part at a time.
struct SeqEngine {
    std::mutex mu;
    vwseq_engine *e = nullptr;
    bool tried = false;
    std::string error;
};

SeqEngine &seq_engine() {
    static SeqEngine instance;
    return instance;
}

//: Called with the engine's lock held.
bool open_seq_engine(SeqEngine &s) {
    if (s.tried) return s.e != nullptr;
    s.tried = true;
    std::string rsrc = find_data(kRsrc), gm = find_data(kGmSpeech);
    std::string bank = data_exists(kGmBank) ? find_data(kGmBank) : std::string();
    int rc = 0;
    s.e = vwseq_open(rsrc.c_str(), gm.c_str(), bank.empty() ? nullptr : bank.c_str(), &rc);
    // a damaged bank leaves the natural voices working
    if (!s.e && !bank.empty()) s.e = vwseq_open(rsrc.c_str(), gm.c_str(), nullptr, &rc);
    if (!s.e) s.error = "VocalWriter's sequencer would not start (" + std::to_string(rc) + ")";
    return s.e != nullptr;
}

//: VocalWriter's lyric text: only its last character matters to the
//: parser, a "-" joining the syllable to the next one as a word.
std::string lyric_text(const std::string &word) {
    std::string out;
    for (unsigned char c : word)
        if (std::isalpha(c) || c == '-' || c == '=' || c == '\'') out.push_back(char(c));
    if (out.size() > 12) out = out.substr(0, 11) + out.back();
    return out;
}

bool is_rest_note(const SungNote &n) {
    for (const std::string &p : n.phonemes)
        if (p != kRest && phoneme_code(p) >= 0) return false;
    return true;
}

void control(std::vector<vwseq_event> &ev, int32_t t, int status, int value) {
    ev.push_back({t, status, 0, std::max(0, std::min(127, value)), 0});
}

//: the sequencer's status for one of a note's controls (NoteControls), or 0
int control_status(const std::string &key) {
    static const std::map<std::string, int> statuses = {
        {"volume", VWSEQ_VOLUME},         {"color", VWSEQ_COLOR},     {"noise", VWSEQ_NOISE},
        {"breath", VWSEQ_BREATH},         {"vibrato", VWSEQ_VIB_DEPTH}, {"vibrato_rate", VWSEQ_VIB_FREQ},
        {"portamento", VWSEQ_PORTAMENTO}, {"chorus", VWSEQ_CHORUS},   {"detune", VWSEQ_DETUNE},
        {"bend_range", VWSEQ_PB_RANGE},   {"sustain", VWSEQ_SUSTAIN},
    };
    auto it = statuses.find(key);
    return it == statuses.end() ? 0 : it->second;
}

//: a 14-bit control about 8192: pitch bend, detune and chorus
void control14(std::vector<vwseq_event> &ev, int32_t t, int status, int value) {
    int v = std::max(0, std::min(16383, value + 8192));
    ev.push_back({t, status, v & 127, v >> 7, 0});
}

//: Where the sequencer's samples go, and how many channels they are in.
struct Collected {
    std::vector<float> *samples;
    int channels;
};

//: Over 32767, as the WAV writer multiplies back, so an export of a
//: part holds the very samples VocalWriter wrote. An export being followed
//: hears how far it has got, and can stop it (sing_watch).
int collect(void *user, const int16_t *samples, size_t count) {
    auto *to = static_cast<Collected *>(user);
    for (size_t i = 0; i < count; ++i) to->samples->push_back(samples[i] / 32767.0f);
    if (SingWatch *w = sing_watch())
        return w->sung(double(to->samples->size()) / to->channels / kSampleRate) ? 0 : 1;
    return 0;
}

//: A part as the sequencer takes it, with the notes and events it points at.
struct SeqPart {
    std::vector<vwseq_note> notes;
    std::vector<vwseq_event> events;
    std::vector<vwseq_tempo> tempos;
    vwseq_part p{};
    //: what of the level is finer than the track level's whole percent
    double residual = 1.0;
};

//: How far the GM Soundbank's instrument for program `program` (0 to 127) is
//: moved, in semitones, to sound where its notes say (gm_octaves): some of
//: them sound an octave low, two an octave high.
int soundbank_octave(int program) {
    static const std::set<int> low = {29, 30, 31, 32, 34, 36, 37, 53, 80, 86, 96, 100, 101};
    if (low.count(program)) return 12;
    if (program == 98 || program == 102) return -12;
    return 0;
}

//: False if the part sings nothing. A GM Soundbank voice plays it as an
//: instrument track.
bool to_seq(const Part &part, int voice_id, SeqPart *sp) {
    if (part.notes.empty()) return false;
    const bool played = is_gm_voice(voice_id);
    const int kind = !played ? VWSEQ_VOCAL : voice_id == kGmDrums ? VWSEQ_DRUMS : VWSEQ_INSTRUMENT;
    // the engine's own index: the instrument, or the voice's place in the bank
    auto index = [played](int v) { return played ? v - kGmBase : vocalwriter_place(v); };
    const int voice = index(voice_id);
    // The sequencer's tempo is a whole number from 10 to 250. Any other is
    // played at the nearest it has, with a beat's ticks counted to match:
    // 240 at a tempo it has, as VocalWriter counts them. The changes go in
    // as the song's own.
    auto whole = [](double bpm) { return int(std::max(10L, std::min(250L, std::lround(bpm)))); };
    struct Span {
        double beat, tick, second;
        int bpm;              // the sequencer's
        double per_beat;      // ticks
    };
    auto span = [&](double beat, double tick, double second, double bpm) {
        int e = whole(bpm);
        return Span{beat, tick, second, e, kTicksPerBeat * e / std::max(bpm, 1e-6)};
    };
    std::vector<Span> spans = {span(0.0, 0.0, 0.0, part.bpm)};
    bool plain = spans[0].per_beat == kTicksPerBeat;
    for (const auto &c : part.tempos) {
        const Span last = spans.back();
        if (c.first <= last.beat) continue;
        double tick = plain ? c.first * kTicksPerBeat : last.tick + (c.first - last.beat) * last.per_beat;
        spans.push_back(span(c.first, tick, last.second + (tick - last.tick) * 60.0 / (kTicksPerBeat * last.bpm), c.second));
        sp->tempos.push_back({int32_t(std::llround(tick)), spans.back().bpm});
        plain = plain && spans.back().per_beat == kTicksPerBeat;
    }
    int tempo = spans[0].bpm;
    auto span_at = [&](auto key, double v) {
        size_t i = spans.size() - 1;
        while (i > 0 && spans[i].*key > v) --i;
        return spans[i];
    };
    auto ticks = [&](double beats) {
        if (plain) return int32_t(std::llround(beats * kTicksPerBeat));
        Span s = span_at(&Span::beat, beats);
        return int32_t(std::llround(s.tick + (beats - s.beat) * s.per_beat));
    };
    auto at_second = [&](double s) {
        Span sp_ = span_at(&Span::second, std::max(0.0, s));
        return int32_t(std::llround(sp_.tick + (std::max(0.0, s) - sp_.second) * sp_.bpm * kTicksPerBeat / 60.0));
    };
    auto second_of = [&](double tick) {
        Span sp_ = span_at(&Span::tick, tick);
        return sp_.second + (tick - sp_.tick) * 60.0 / (kTicksPerBeat * sp_.bpm);
    };

    // how far the instrument in force at a beat is moved (soundbank_octave):
    // the part's voice, then each change
    auto octave_at = [&](double at) {
        if (!played || !gm_octaves()) return 0;
        int v = voice_id;
        for (const auto &c : part.voices)
            if (c.first <= at + 1e-9) v = c.second;
        return v == kGmDrums || !is_gm_voice(v) ? 0 : soundbank_octave(v - kGmBase);
    };

    // placed by where each note begins, so that nothing drifts
    std::vector<vwseq_note> &notes = sp->notes;
    double beat = 0.0;
    int32_t end = 0;
    std::vector<vwseq_event> note_ev, ranges;
    int32_t first_sung = -1;
    for (size_t i = 0; i < part.notes.size(); ++i) {
        const SungNote &n = part.notes[i];
        int32_t a = ticks(beat), b = ticks(beat + n.beats);
        beat += n.beats;
        // the note's own controls, at their ticks
        for (const auto &ctl : n.controls) {
            int status = control_status(ctl.first);
            for (const auto &pt : ctl.second) {
                int32_t t = a + int32_t(std::llround(pt.first * std::max(0, b - a)));
                int value = int(std::lrint(pt.second));
                size_t from = note_ev.size(), from_r = ranges.size();
                if (status == VWSEQ_PB_RANGE) control(ranges, t, status, value);
                else if (status == VWSEQ_CHORUS || status == VWSEQ_DETUNE) control14(note_ev, t, status, value);
                else if (status) control(note_ev, t, status, value);
                // one at the very start of the note comes after it, where
                // VocalWriter writes what a note carries
                if (pt.first <= 0.0 && b > a) {
                    for (size_t k = from; k < note_ev.size(); ++k) note_ev[k].after_note = 1;
                    for (size_t k = from_r; k < ranges.size(); ++k) ranges[k].after_note = 1;
                }
            }
        }
        if (played) {
            // an instrument plays each note for as long as it sounds; a rest
            // is only time going by
            if (is_rest_note(n)) continue;
            int32_t c = n.hold > 0 ? ticks(beat - n.beats + n.hold) : b;
            vwseq_note v{};
            v.start = a;
            v.duration = std::max(1, c - a);
            // a note the move would take off the keyboard stays where it is
            int move = octave_at(beat - n.beats);
            v.key = n.midi + move >= 0 && n.midi + move <= 127 ? n.midi + move : n.midi;
            v.velocity = n.velocity;
            notes.push_back(v);
            if (first_sung < 0) first_sung = a;
            end = std::max(end, a + v.duration);
            continue;
        }
        if (b <= a) continue;
        vwseq_note v{};
        v.start = a;
        v.duration = b - a;
        v.key = n.midi;
        v.velocity = n.velocity;
        if (is_rest_note(n)) {
            // a rest carries the coming note's key, as VocalWriter
            // writes it: that note's first consonants are sung in it
            v.rest = 1;
            for (size_t k = i + 1; k < part.notes.size(); ++k)
                if (!is_rest_note(part.notes[k])) {
                    v.key = part.notes[k].midi;
                    break;
                }
            // unless the song says otherwise, as one from VocalWriter can
            auto lead = n.controls.find("lead_pitch");
            if (lead != n.controls.end() && !lead->second.empty())
                v.key = std::max(0, std::min(127, int(std::lrint(lead->second.front().second))));
            if (!notes.empty() && notes.back().rest && notes.back().start + notes.back().duration == a) {
                notes.back().duration += v.duration;
                end = b;
                continue;
            }
        } else {
            std::string text = lyric_text(n.word);
            std::memcpy(v.text, text.c_str(), text.size() + 1);
            int count = 0;
            for (const std::string &p : n.phonemes) {
                int code = phoneme_code(p);
                if (code < 0 || p == kRest || count == 12) continue;
                v.phonemes[1 + count++] = (unsigned char)code;
            }
            v.phonemes[0] = (unsigned char)count;
            if (first_sung < 0) first_sung = a;
        }
        notes.push_back(v);
        end = b;
    }
    if (notes.empty()) return false;
    // the sequencer finishes a track once its voice has sung a rest
    if (!played && !notes.back().rest) {
        vwseq_note r{};
        r.start = end;
        r.duration = kTicksPerBeat;
        r.key = notes.back().key;
        r.velocity = notes.back().velocity;
        r.rest = 1;
        notes.push_back(r);
    }

    // the controllers' controls, each where it stands: before a note that
    // starts there
    for (const ControlAt &c : part.controls) {
        int status = control_status(c.name);
        int value = int(std::lrint(c.value));
        if (status == VWSEQ_PB_RANGE) control(ranges, ticks(c.beat), status, value);
        else if (status == VWSEQ_CHORUS || status == VWSEQ_DETUNE) control14(note_ev, ticks(c.beat), status, value);
        else if (status) control(note_ev, ticks(c.beat), status, value);
    }

    // The part's settings at the start, only those off their defaults,
    // so the voice starts as a VocalWriter song starts it; then the
    // bends and the wheel as they move.
    std::vector<vwseq_event> &ev = sp->events;
    size_t count;
    const VoiceControl *c = svs::voice_controls(&count);
    VoiceStyle style = clean_voice(part.style);
    // the voice controls are a singer's; an instrument has its own sound
    for (size_t i = 0; i < count && !played; ++i) {
        int value = style.*(c[i].field);
        std::string call = c[i].call;
        if (value == (call == "Speech_Portamento" ? kEnginePortamento : c[i].def)) continue;
        if (call == "Speech_Color") control(ev, 0, VWSEQ_COLOR, value);
        else if (call == "Speech_VibDepth") control(ev, 0, VWSEQ_VIB_DEPTH, value);
        else if (call == "Speech_VibFreq") control(ev, 0, VWSEQ_VIB_FREQ, value);
        else if (call == "Speech_Chorus") control14(ev, 0, VWSEQ_CHORUS, value);
        else if (call == "Speech_Breath") control(ev, 0, VWSEQ_BREATH, value);
        else if (call == "Speech_Detune") control14(ev, 0, VWSEQ_DETUNE, value);
        else if (call == "Speech_Portamento") control(ev, 0, VWSEQ_PORTAMENTO, value);
    }
    if (!ranges.empty()) {
        // the bend range the notes set, each bend at the one in force, as a
        // song from VocalWriter has them
        std::stable_sort(ranges.begin(), ranges.end(),
                         [](const vwseq_event &a, const vwseq_event &b) { return a.time < b.time; });
        ev.insert(ev.end(), ranges.begin(), ranges.end());
        for (const BendEvent &b : part.bends) {
            int32_t t = at_second(b.seconds);
            int range = 2;
            for (const vwseq_event &r : ranges)
                if (r.time <= t) range = std::max(1, r.vol);
            control14(ev, t, VWSEQ_PITCH_BEND, bend_raw(b.semitones, range));
            // at a note's start the bend is the note's own, written after it
            for (const vwseq_note &n : notes)
                if (n.start == t) ev.back().after_note = 1;
        }
    } else if (part.any_bends) {
        // VocalWriter's own range of two semitones if the bends fit it,
        // else the narrowest that holds them
        double widest = 0;
        for (const BendEvent &b : part.bends) widest = std::max(widest, std::fabs(b.semitones));
        int range = widest <= 2.0 + 1e-9 ? 2 : int(std::min(12.0, std::ceil(widest - 1e-9)));
        if (range != 2) control(ev, 0, VWSEQ_PB_RANGE, range);
        for (const BendEvent &b : part.bends)
            control14(ev, at_second(b.seconds), VWSEQ_PITCH_BEND, bend_raw(b.semitones, range));
    }
    // the wheel is a singer's vibrato; the instruments have their own
    if (!part.mods.empty() && !played) {
        int last = -1;
        for (const BendEvent &m : part.mods) {
            int v = int(std::lrint(effective_vibrato(style.vibrato, m.semitones)));
            if (v != last) control(ev, at_second(m.seconds), VWSEQ_VIB_DEPTH, v);
            last = v;
        }
    }
    ev.insert(ev.end(), note_ev.begin(), note_ev.end());
    // the voice changes, each where it stands: before a note that starts
    // there, which VocalWriter sings the same as after it
    int32_t first_voice_tick = -1;
    for (const auto &v : part.voices) {
        ev.push_back({ticks(v.first), VWSEQ_SET_VOICE, 0, index(v.second), 0});
        if (first_voice_tick < 0) first_voice_tick = ev.back().time;
    }
    std::stable_sort(ev.begin(), ev.end(),
                     [](const vwseq_event &a, const vwseq_event &b) { return a.time < b.time; });

    vwseq_part &p = sp->p;
    p = vwseq_part{};
    p.bpm = tempo;
    p.voice = voice;
    p.notes = notes.data();
    p.note_count = int(notes.size());
    p.events = ev.data();
    p.event_count = int(ev.size());
    p.channel = part.channel;
    p.consonants = part.consonants;
    // a voice chosen before the first note is the part's own choosing
    p.voice_later = first_voice_tick >= 0 && first_sung >= 0 && first_voice_tick <= first_sung;
    if (part.end > 0) p.end = ticks(part.end);
    p.tempos = sp->tempos.data();
    p.tempo_count = int(sp->tempos.size());
    p.max_seconds = second_of(end) + 10.0;
    p.kind = kind;

    // the level as VocalWriter's track level, in whole percent, and what is
    // finer than that on the samples; a voice VocalWriter sang past full
    // scale is left as it sang it. Up to twice as loud is the engine's own.
    p.track_level = 100;
    sp->residual = part.level;
    if (part.level > 0 && part.level <= kMaxVolume / 100.0) {
        p.track_level = std::max(1, int(std::lround(part.level * 100)));
        sp->residual = part.level / (p.track_level / 100.0);
    }
    return true;
}

//: The editor's call for one of a note's controls (NoteControls), or null:
//: phrase by phrase the bends are in semitones whatever the bend range, and
//: the rest's pitch and the pedal are the sequencer's.
const char *editor_call(const std::string &key) {
    static const std::map<std::string, const char *> calls = {
        {"volume", "Speech_Volume"},         {"color", "Speech_Color"},     {"noise", "Speech_Noise"},
        {"breath", "Speech_Breath"},         {"vibrato", "Speech_VibDepth"}, {"vibrato_rate", "Speech_VibFreq"},
        {"portamento", "Speech_Portamento"}, {"chorus", "Speech_Chorus"},   {"detune", "Speech_Detune"},
    };
    auto it = calls.find(key);
    return it == calls.end() ? nullptr : it->second;
}

//: A voice of VocalWriter's, or an instrument of its General MIDI bank:
//: the same sequencer plays both, and the editor sings the voices phrase by
//: phrase when VocalWriter's own play path is not asked for.
class VwSinger : public Singer {
public:
    explicit VwSinger(int voice_id) : voice_id_(voice_id) {}

    Sung sing(const Phrase &phrase) override {
        if (is_vocalwriter_voice(voice_id_) && !vocalwriter_own_path()) return sing_phrase(phrase);
        return sing_sequenced(phrase);
    }

    //: A phrase as VocalWriter Studio sang it, through the editor.
    Sung sing_phrase(const Phrase &phrase) {
        Sung out;
        std::unique_ptr<vw_editor, void (*)(vw_editor *)> eng(VwAssets::get().open_editor(),
                                                              vw_ed_close);
        if (!eng) return out;
        vw_editor *e = eng.get();
        double bpm = phrase.bpm;
        vw_ed_tempo_scale(e, kTempoScale);
        vw_ed_tempo(e, engine_tempo(bpm));
        due_ = 0.0;
        given_ = 0;
        std::vector<unsigned char> blob = sequence(phrase.notes);
        if (vw_ed_voice(e, vocalwriter_place(voice_id_)) != 0) return out;
        vw_ed_sequence(e, blob.data(), blob.size());
        vw_ed_start(e);
        voice_controls(e, phrase);

        // the controller events: the bend range first, then each value at
        // its moment, applied between frames -- as fine as the engine's own
        // control rate
        enum Kind { kSens, kBend, kVibDepth, kControl };
        struct Ev {
            double t;
            Kind kind;
            int value;
            const char *call;
        };
        std::vector<Ev> ev;
        if (phrase.any_bends) {
            ev.push_back({-1.0, kSens, kBendRange, nullptr});
            for (const BendEvent &b : phrase.bends)
                ev.push_back({b.seconds, kBend, bend_raw(b.semitones, kBendRange), nullptr});
        }
        // The mod wheel is the engine's own vibrato depth, moved between
        // frames: Speech_VibDepth is the control VocalWriter's sequencer drives
        // for exactly this. With the wheel at rest it is the part's setting.
        if (!phrase.mods.empty()) {
            int base = clean_voice(phrase.style).vibrato, last = -1;
            for (const BendEvent &m : phrase.mods) {
                int v = int(std::lrint(effective_vibrato(base, m.semitones)));
                if (v != last) ev.push_back({m.seconds, kVibDepth, v, nullptr});
                last = v;
            }
        }
        // the notes' own controls, each at its point in the note
        double at = 0.0;
        for (const SungNote &n : phrase.notes) {
            double len = n.beats * 60.0 / std::max(bpm, 1e-6);
            for (const auto &c : n.controls) {
                const char *call = editor_call(c.first);
                if (!call) continue;
                for (const auto &pt : c.second) {
                    int value = int(std::lrint(pt.second));
                    // as a song's detune is read: its 14 bits moved up three
                    if (c.first == "detune") value *= 8;
                    ev.push_back({at + pt.first * len, kControl, value, call});
                }
            }
            at += len;
        }
        std::stable_sort(ev.begin(), ev.end(), [](const Ev &a, const Ev &b) { return a.t < b.t; });
        size_t ei = 0, next = 0;
        auto feed = [&]() {
            const SungNote &n = phrase.notes[next++];
            vw_ed_tempo(e, engine_tempo(bpm));
            note_call(e, n, bpm);
        };
        feed();
        int frames = 0;
        while (frames < kMaxFrames) {
            double now = frames * kSamplesPerFrame / double(kSampleRate);
            while (ei < ev.size() && ev[ei].t <= now) {
                const Ev &x = ev[ei++];
                if (x.kind == kSens)
                    vw_ed_control(e, "Speech_PBSens", x.value);
                else if (x.kind == kBend)
                    vw_ed_control(e, "Speech_PitchBend", x.value * kBendScale);
                else if (x.kind == kVibDepth)
                    vw_ed_control(e, "Speech_VibDepth", x.value);
                else
                    vw_ed_control(e, x.call, x.value);
            }
            if (!vw_ed_frames(e, 1)) {
                out.stopped_short = true;      // no room left to write into
                break;
            }
            ++frames;
            if (vw_ed_state(e) == 3) break;
            if (vw_ed_wants_note(e)) {
                if (next >= phrase.notes.size()) break;
                feed();
            }
        }
        if (frames >= kMaxFrames) out.stopped_short = true;
        // SayFrame writes groups of four halfwords as L0 L1 R0 R1: two
        // samples per channel, not two interleaved frames.
        int32_t n = vw_ed_wave_index(e);
        const int16_t *raw = vw_ed_wave(e);
        out.samples.reserve(size_t(n / 2));
        for (int32_t i = 0; i + 1 < n; i += 4) {
            out.samples.push_back(raw[i] / 32768.0f);
            out.samples.push_back(raw[i + 1] / 32768.0f);
        }
        return out;
    }

    //: A phrase through the sequencer: a part that begins `room` beats before
    //: it, so that its first consonants are sung before the beat as in the
    //: whole track.
    Sung sing_sequenced(const Phrase &phrase) {
        Part part;
        part.bpm = phrase.bpm;
        part.bends = phrase.bends;
        part.any_bends = phrase.any_bends;
        part.mods = phrase.mods;
        part.style = phrase.style;
        part.level = phrase.level;
        part.consonants = phrase.consonants;
        part.channel = phrase.channel;
        if (phrase.room > 0) {
            SungNote lead;
            lead.midi = phrase.notes.front().midi;
            lead.beats = phrase.room;
            lead.phonemes = {kRest};
            part.notes.push_back(lead);
        }
        part.notes.insert(part.notes.end(), phrase.notes.begin(), phrase.notes.end());
        part.tempos = phrase.tempos;
        // the controls the notes before it left standing, at its start
        for (const auto &h : phrase.held) {
            auto &pts = part.notes.front().controls[h.first];
            pts.insert(pts.begin(), {0.0, h.second});
        }
        for (const ControlAt &c : phrase.controls) part.controls.push_back({c.beat + phrase.room, c.name, c.value});
        Sung out = sing_part(part);
        out.starts_before = seconds_into(phrase.bpm, phrase.tempos, phrase.room);
        return out;
    }

    Sung sing_part(const Part &part) override {
        Sung out;
        SeqPart sp;
        if (!to_seq(part, voice_id_, &sp)) return out;
        SeqEngine &s = seq_engine();
        std::lock_guard<std::mutex> lock(s.mu);
        if (!open_seq_engine(s)) {
            out.error = s.error;
            return out;
        }
        if (sp.p.kind != VWSEQ_VOCAL && !vwseq_has_bank(s.e)) {
            out.error = "the instrument bank, GMBank.rsrc, would not load";
            return out;
        }
        out.channels = sp.p.kind == VWSEQ_VOCAL ? 1 : 2;
        Collected to{&out.samples, out.channels};
        int rc = vwseq_sing(s.e, &sp.p, collect, &to);
        if (rc != 0) {
            out.error = "VocalWriter's sequencer stopped (" + std::to_string(rc) + ")";
            out.samples.clear();
            return out;
        }
        if (sp.residual != 1.0)
            for (float &v : out.samples) v = float(v * sp.residual);
        return out;
    }

private:
    //: The packed block SetSeqAddr expects. Every sequence ends with a
    //: marked rest: Syllable_Duration scales a syllable against the next
    //: note-start and gives up if there is none.
    std::vector<unsigned char> sequence(const std::vector<SungNote> &notes) {
        std::vector<uint16_t> phon, ctrl, dur;
        for (const SungNote &note : notes) {
            // as the original: unknown symbols are dropped, and the lengths
            // are still read by position
            std::vector<std::string> syms;
            for (const std::string &p : note.phonemes)
                if (phoneme_code(p) >= 0) syms.push_back(p);
            if (syms.empty()) syms = {kRest};
            for (size_t k = 0; k < syms.size(); ++k) {
                double len = k < note.durations_ms.size() ? note.durations_ms[k]
                                                          : phoneme(syms[k])->max_ms;
                phon.push_back(uint16_t(phoneme_code(syms[k])));
                ctrl.push_back(k == 0 ? 1 : 0);        // bit 0: this begins a note
                dur.push_back(uint16_t(std::max(1L, round_half_even(len))));
            }
        }
        phon.push_back(uint16_t(phoneme_code(kRest)));
        ctrl.push_back(1);
        dur.push_back(uint16_t(std::max(1, phoneme(kRest)->max_ms)));
        size_t n = phon.size();
        std::vector<unsigned char> blob;
        auto be = [&](uint16_t v) {
            blob.push_back((unsigned char)(v >> 8));
            blob.push_back((unsigned char)v);
        };
        be(0);
        be(uint16_t(n));
        for (uint16_t v : phon) be(v);
        for (uint16_t v : ctrl) be(v);
        for (size_t i = 0; i < n; ++i) be(0);
        for (uint16_t v : dur) be(v);
        return blob;
    }

    //: InitDefaultVoiceCntrls and the glide table, the volume, the level,
    //: then only the controls moved off their defaults.
    void voice_controls(vw_editor *e, const Phrase &phrase) {
        vw_ed_defaults(e, 1);
        // The defaults clear the voice's own chorus (Webster's, the only one
        // in the bank), so the voice is chosen again, as VocalWriter does.
        vw_ed_voice(e, vocalwriter_place(voice_id_));
        vw_ed_volume(e, 127);
        if (phrase.level != 1.0) vw_ed_level(e, float(phrase.level));
        size_t count;
        const VoiceControl *c = svs::voice_controls(&count);
        VoiceStyle v = clean_voice(phrase.style);
        for (size_t i = 0; i < count; ++i) {
            int value = v.*(c[i].field);
            if (value == c[i].def) continue;
            // the detune as the sequencer takes it, moved up three, so it is
            // the same shade either way
            if (c[i].field == &VoiceStyle::detune) value *= 8;
            vw_ed_control(e, c[i].call, value);
        }
        // The radiation shelf at the engine's own setting. VocalWriter Studio
        // writes it back through the same arithmetic, so this does too.
        if (vw_ed_hf_emph(e)) {
            float a = vw_ed_emph_b(e);
            vw_ed_set_emph(e, 2.0f - a, a);
        }
    }

    //: Speech_Note, asked for the frames that put the *next* note where the
    //: score puts it, so the error is half a frame either way and never
    //: accumulates. Aiming at the middle of a frame makes the engine's floor
    //: land where it is meant to.
    void note_call(vw_editor *e, const SungNote &note, double bpm) {
        due_ += note.beats * 60.0 / std::max(bpm, 1e-6);
        long want = round_half_even(due_ * kFramesASecond);
        long frames = std::max(1L, want - given_);
        given_ += frames;
        vw_ed_note(e, note.midi, 0, note.velocity,
                   (frames + 0.5) / frames_per_beat(engine_tempo(bpm)));
    }

    int voice_id_;
    double due_ = 0.0;
    long given_ = 0;
};

class VocalWriterFamily : public Family {
public:
    std::string name() const override { return "VocalWriter"; }

    bool sings_parts() const override { return vocalwriter_own_path(); }

    std::vector<VoiceEntry> voices() override {
        std::lock_guard<std::mutex> lock(mu_);
        load_names();
        std::vector<VoiceEntry> out;
        for (size_t i = 0; i < names_.size(); ++i) {
            int place = vocalwriter_place(kVocalWriterBase + int(i));
            if (place < int(names_.size()) && !names_[size_t(place)].empty())
                out.push_back({kVocalWriterBase + int(i), names_[size_t(place)]});
        }
        return out;
    }

    bool available(std::string *why) override {
        VwAssets &a = VwAssets::get();
        if (a.ok()) return true;
        if (why) {
            std::string list;
            for (const std::string &m : a.missing()) list += (list.empty() ? "" : ", ") + m;
            *why = "VocalWriter 2.0's own files are not here: " + list;
        }
        return false;
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        if (!available(err)) return nullptr;
        int index = vocalwriter_place(voice_id);
        std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
        if (!e) {
            if (err) *err = "VocalWriter's engine would not start";
            return nullptr;
        }
        int rc = vw_ed_voice(e.get(), index);
        if (rc == -2) {
            if (err) *err = "that voice is built on the instrument bank, and GMBank.rsrc is not there";
            return nullptr;
        }
        if (rc != 0) {
            if (err) *err = "there is no voice " + std::to_string(index) + " in the bank";
            return nullptr;
        }
        return std::make_unique<VwSinger>(voice_id);
    }

    //: The bank's voices are nothing like each other in level: Robert peaks
    //: at about seven tenths of full scale, Richard and Minor Chrd a little
    //: over it. Phrase by phrase each is sung once, quietly, to see, and a
    //: voice that already fits is left exactly alone; VocalWriter's own play
    //: path leaves every voice as VocalWriter sang it.
    double headroom(int voice_id) override {
        if (vocalwriter_own_path()) return 1.0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto it = headroom_.find(voice_id);
            if (it != headroom_.end()) return it->second;
        }
        const double probe = 0.01;
        double gain = 1.0;
        std::unique_ptr<Singer> s = singer(voice_id, nullptr);
        if (s) {
            Phrase p;
            p.bpm = 100;
            p.level = probe;
            SungNote n;
            n.midi = 62;
            n.beats = 0.6;
            n.velocity = 100;
            n.phonemes = {"d", "AA"};
            p.notes = {n};
            Sung y = static_cast<VwSinger *>(s.get())->sing_phrase(p);
            float peak = 0;
            for (float v : y.samples) peak = std::max(peak, std::fabs(v));
            double rel = y.samples.empty() ? 1.0 : peak / probe;
            gain = rel <= 1.0 ? 1.0 : 0.7 / std::max(rel, 1e-6);
        }
        std::lock_guard<std::mutex> lock(mu_);
        headroom_[voice_id] = gain;
        return gain;
    }

private:
    void load_names() {
        if (loaded_) return;
        loaded_ = true;
        std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
        if (!e) return;
        int count = vw_ed_voice_count(e.get());
        for (int i = 0; i < count; ++i) {
            const char *n = vw_ed_voice_name_at(e.get(), i);
            names_.push_back(n ? mac_roman_to_utf8(n) : "");
        }
    }

    static std::string mac_roman_to_utf8(const char *s) {
        std::string out;
        for (; *s; ++s) {
            unsigned char c = (unsigned char)*s;
            if (c < 0x80) out.push_back(char(c));
            else out += "?";
        }
        return out;
    }

    std::mutex mu_;
    bool loaded_ = false;
    std::vector<std::string> names_;   // by place in the bank
    std::map<int, double> headroom_;
};


//: VocalWriter's General MIDI bank, GMBank, as an engine of its own,
//: played through the same sequencer that sings its voices.
class GmFamily : public Family {
public:
    std::string name() const override { return "GM Soundbank"; }
    bool sings_parts() const override { return true; }
    bool plays() const override { return true; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int p = 0; p < 128; ++p) out.push_back({kGmBase + p, std::string("GM ") + gm_program_name(p)});
        out.push_back({kGmDrums, "GM Drum Kit"});
        return out;
    }

    bool available(std::string *why) override {
        VwAssets &a = VwAssets::get();
        if (a.ok() && a.has_bank()) return true;
        if (why) {
            if (!a.ok()) {
                std::string list;
                for (const std::string &m : a.missing()) list += (list.empty() ? "" : ", ") + m;
                *why = "it plays through VocalWriter 2.0's own files, which are not here: " + list;
            } else {
                *why = "VocalWriter's instrument bank, GMBank.rsrc, is not in assets";
            }
        }
        return false;
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        if (!available(err)) return nullptr;
        if (voice_id < kGmBase || voice_id > kGmDrums) {
            if (err) *err = "there is no GM instrument " + std::to_string(voice_id - kGmBase);
            return nullptr;
        }
        return std::make_unique<VwSinger>(voice_id);
    }
};

}  // namespace

std::unique_ptr<Family> make_gm_family() { return std::make_unique<GmFamily>(); }

const char *gm_program_name(int program) {
    static const char *const names[128] = {
        "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
        "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone",
        "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ",
        "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        "Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
        "Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
        "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
        "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        "Violin", "Viola", "Cello", "Contrabass",
        "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
        "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
        "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
        "Oboe", "English Horn", "Bassoon", "Clarinet",
        "Piccolo", "Flute", "Recorder", "Pan Flute",
        "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)",
        "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass and lead)",
        "Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)",
        "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
        "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)",
        "FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
        "Sitar", "Banjo", "Shamisen", "Koto",
        "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock",
        "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet",
        "Telephone Ring", "Helicopter", "Applause", "Gunshot",
    };
    return program >= 0 && program < 128 ? names[program] : "";
}

const char *gm_drum_name(int key) {
    static const char *const names[] = {
        "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare", "Hand Clap",
        "Electric Snare", "Low Floor Tom", "Closed Hi-hat", "High Floor Tom", "Pedal Hi-hat",
        "Low Tom", "Open Hi-hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1",
        "High Tom", "Ride Cymbal 1", "Chinese Cymbal", "Ride Bell", "Tambourine",
        "Splash Cymbal", "Cowbell", "Crash Cymbal 2", "Vibraslap", "Ride Cymbal 2",
        "Hi Bongo", "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga",
        "High Timbale", "Low Timbale", "High Agogo", "Low Agogo", "Cabasa",
        "Maracas", "Short Whistle", "Long Whistle", "Short Guiro", "Long Guiro",
        "Claves", "Hi Wood Block", "Low Wood Block", "Mute Cuica", "Open Cuica",
        "Mute Triangle", "Open Triangle",
    };
    return key >= 35 && key <= 81 ? names[key - 35] : nullptr;
}

std::unique_ptr<Family> make_vocalwriter_family() { return std::make_unique<VocalWriterFamily>(); }

std::vector<int> vocalwriter_program_voices() {
    std::vector<int> out(128, -1);
    std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
    if (!e) return out;
    for (int p = 0; p < 128; ++p) {
        int place = vw_ed_program_voice(e.get(), p);
        out[size_t(p)] = place < 0 ? -1 : vocalwriter_voice_at(place);
    }
    return out;
}

bool vocalwriter_song(const std::vector<std::pair<int, Part>> &parts, int room, int wet,
                      std::vector<float> *stereo, std::string *error) {
    std::vector<SeqPart> seq(parts.size());
    std::vector<vwseq_part> sung;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!to_seq(parts[i].second, parts[i].first, &seq[i])) continue;
        // a level between VocalWriter's whole percents is put on the samples,
        // which one mix cannot do part by part
        if (seq[i].residual != 1.0) return false;
        sung.push_back(seq[i].p);
    }
    if (sung.empty() || sung.size() > 31) return false;
    SeqEngine &s = seq_engine();
    std::lock_guard<std::mutex> lock(s.mu);
    if (!open_seq_engine(s)) {
        *error = s.error;
        return false;
    }
    vwseq_song song{sung.data(), int(sung.size()), room, wet};
    stereo->clear();
    Collected to{stereo, 2};
    int rc = vwseq_sing_song(s.e, &song, collect, &to);
    if (rc != 0) {
        *error = "VocalWriter's sequencer stopped (" + std::to_string(rc) + ")";
        stereo->clear();
        return false;
    }
    return true;
}

std::string vocalwriter_engine_name() { return VwAssets::get().ok() ? "C engine" : "not available"; }

bool vocalwriter_reverb(std::vector<float> &mix, int room, int wet, const std::function<bool(double)> &going) {
    if (wet <= 0 || mix.empty()) return false;
    std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
    if (!e) return false;
    if (vw_ed_reverb(e.get(), room / 100.0f, wet / 100.0f) != 0) return false;
    // The reverberator works on 16-bit samples, 220 frames at a time, as the
    // application's own sound buffers did; the tail needs a second of room.
    size_t frames_in = mix.size() / 2;
    size_t padded = frames_in + kSampleRate;
    size_t frames = (padded / 220) * 220;
    std::vector<int16_t> pcm(frames * 2, 0);
    for (size_t i = 0; i < std::min(frames * 2, mix.size()); ++i) {
        float c = std::max(-1.0f, std::min(1.0f, mix[i]));
        pcm[i] = int16_t(c * 32767.0f);
    }
    // in pieces, which the delay lines carry across, so a long song can say
    // how far it has got
    const size_t piece = 220 * 400;
    for (size_t at = 0; at < frames; at += piece) {
        vw_ed_reverberate(e.get(), pcm.data() + at * 2, int32_t(std::min(piece, frames - at)));
        if (going && !going(double(std::min(frames, at + piece)) / double(frames))) return false;
    }
    // keep whatever of the tail is not silence
    size_t last = 0;
    for (size_t f = 0; f < frames; ++f)
        if (std::abs(pcm[2 * f]) > 1 || std::abs(pcm[2 * f + 1]) > 1) last = f + 1;
    size_t keep = std::max(frames_in, std::min(last, frames));
    if (!last) keep = frames_in;
    mix.assign(keep * 2, 0.0f);
    for (size_t i = 0; i < keep * 2; ++i) mix[i] = pcm[i] / 32767.0f;
    return true;
}

}  // namespace svs
