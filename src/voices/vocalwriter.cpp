// VocalWriter's engine, driven the way VocalWriter Studio drives it: the same
// sequence of calls, in the same order, as the VocalWriter repository's
// ppc/render.py, so the samples are the same samples.
#include "voices/vocalwriter.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <memory>

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/controls.h"

extern "C" {
#include "vw_editor.h"
}

namespace svs {

namespace {

const char *const kRsrc = "assets\\VocalWriter.app\\Contents\\Resources\\VocalWriter.rsrc";
const char *const kGmSpeech = "assets\\GMSpeech.rsrc";
const char *const kGmBank = "assets\\GMBank.rsrc";
const char *const kEnglishLex = "assets\\EnglishLex";

const int kSamplesPerFrame = 220;
//: How many frames a second is: 200.4545..., the engine's only clock.
const double kFramesASecond = double(kSampleRate) / kSamplesPerFrame;
//: calibrated so frames per beat matches the above
const float kTempoScale = 1.0f / 240.0f;
const int kMaxFrames = 200000;
//: Speech_PitchBend wants MIDI's 14-bit bend times 8 for a full-scale bend to
//: reach the whole range (measured: 0 cents error at 12 semitones).
const int kBendScale = 8;
//: the bend range the engine is put into before any bend is sent
const double kBendRange = 12.0;

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

int bend_raw(double semitones) {
    long v = round_half_even(semitones / kBendRange * 8192);
    return int(std::max(-8192L, std::min(8191L, v)));
}

}  // namespace

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

std::vector<std::string> Lexicon::phonemes(const std::string &word) {
    std::string clean;
    for (unsigned char c : word)
        if (std::isalpha(c) || c == '\'') clean.push_back(char(c));
    if (clean.empty() || !ed_) return {};
    std::lock_guard<std::mutex> lock(mu_);
    unsigned char out[10 * 9] = {0};
    int n = vw_ed_word(ed_, clean.c_str(), out);
    if (n <= 0) return {};
    const auto &table = svs::phonemes();
    std::vector<std::string> phones;
    for (int i = 0; i < n && i < 10; ++i) {
        int count = out[i * 9];
        for (int k = 0; k < count && k < 8; ++k) {
            int code = out[i * 9 + 1 + k];
            if (code < int(table.size())) phones.push_back(table[size_t(code)].symbol);
        }
    }
    if (phones.empty()) return {};
    return allophones(phones);
}

// -- singing -------------------------------------------------------------------

namespace {

class VwSinger : public Singer {
public:
    VwSinger(int voice_id) : voice_id_(voice_id) {}

    Sung sing(const Phrase &phrase) override {
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
        if (vw_ed_voice(e, voice_id_) != 0) return out;
        vw_ed_sequence(e, blob.data(), blob.size());
        vw_ed_start(e);
        voice_controls(e, phrase);

        // the controller events: the bend range first, then each value at
        // its moment, applied between frames -- as fine as the engine's own
        // control rate
        enum Kind { kSens, kBend, kVibDepth };
        struct Ev {
            double t;
            Kind kind;
            int value;
        };
        std::vector<Ev> ev;
        if (phrase.any_bends) {
            ev.push_back({-1.0, kSens, int(kBendRange)});
            for (const BendEvent &b : phrase.bends)
                ev.push_back({b.seconds, kBend, bend_raw(b.semitones)});
        }
        // The mod wheel is the engine's own vibrato depth, moved between
        // frames: Speech_VibDepth is the control VocalWriter's sequencer drives
        // for exactly this. With the wheel at rest it is the part's setting.
        if (!phrase.mods.empty()) {
            int base = clean_voice(phrase.style).vibrato, last = -1;
            std::vector<Ev> depth;
            for (const BendEvent &m : phrase.mods) {
                int v = int(std::lrint(effective_vibrato(base, m.semitones)));
                if (v != last) depth.push_back({m.seconds, kVibDepth, v});
                last = v;
            }
            ev.insert(ev.end(), depth.begin(), depth.end());
            std::stable_sort(ev.begin(), ev.end(), [](const Ev &a, const Ev &b) { return a.t < b.t; });
        }
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
                else
                    vw_ed_control(e, "Speech_VibDepth", x.value);
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
        vw_ed_volume(e, 127);
        if (phrase.level != 1.0) vw_ed_level(e, float(phrase.level));
        size_t count;
        const VoiceControl *c = svs::voice_controls(&count);
        VoiceStyle v = clean_voice(phrase.style);
        for (size_t i = 0; i < count; ++i) {
            int value = v.*(c[i].field);
            if (value != c[i].def) vw_ed_control(e, c[i].call, value);
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

    std::vector<VoiceEntry> voices() override {
        std::lock_guard<std::mutex> lock(mu_);
        load_names();
        std::vector<VoiceEntry> out;
        for (size_t i = 0; i < names_.size(); ++i)
            if (!names_[i].empty()) out.push_back({kVocalWriterBase + int(i), names_[i]});
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
        int index = voice_id - kVocalWriterBase;
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
        return std::make_unique<VwSinger>(index);
    }

    //: The bank's voices are nothing like each other in level: Robert peaks
    //: at about seven tenths of full scale, the wavetable voices forty or
    //: fifty times over it. Each is sung once, quietly, to see, and a voice
    //: that already fits is left exactly alone.
    double headroom(int voice_id) override {
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
            Sung y = s->sing(p);
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
    std::vector<std::string> names_;
    std::map<int, double> headroom_;
};

}  // namespace

std::unique_ptr<Family> make_vocalwriter_family() { return std::make_unique<VocalWriterFamily>(); }

std::vector<int> vocalwriter_program_voices() {
    std::vector<int> out(128, -1);
    std::unique_ptr<vw_editor, void (*)(vw_editor *)> e(VwAssets::get().open_editor(), vw_ed_close);
    if (!e) return out;
    for (int p = 0; p < 128; ++p) out[size_t(p)] = vw_ed_program_voice(e.get(), p);
    return out;
}

std::string vocalwriter_engine_name() { return VwAssets::get().ok() ? "C engine" : "not available"; }

bool vocalwriter_reverb(std::vector<float> &mix, int room, int wet) {
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
    vw_ed_reverberate(e.get(), pcm.data(), int32_t(frames));
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
