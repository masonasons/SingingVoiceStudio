// microsoft.cpp -- Microsoft Sam, Mike and Mary, and the SAPI 4 voice modes.
//
// The engine is the reconstruction of Windows XP's SAPI 5 text-to-speech engine in
// third_party/ms-sam-mike-mary-decomp, which reads Microsoft's own voice files and renders them
// sample for sample as the original did. It is driven here one level below text: each note's
// VocalWriter phonemes become SAPI phone ids, and sam_sing.c hands them to the engine's unit
// selection and synthesiser with the studio's pitch curve (see sam_sing.h).
//
// The voice data is Microsoft's and is not part of the program. Only <Voice>.spd is needed to
// sing; it is looked for in voices\microsoft under the data roots (see core/paths.h). The .sdf
// files and the two dictionaries that come with it (LTTS1033.LXA, r1033tts.LXA) may sit beside
// it but are not used: they are for reading text, and nothing here reads text.
//
// Voice numbers: 3000 + 20 * voice + mode, voice 0 Sam, 1 Mike, 2 Mary; mode 0 the plain voice,
// 1 in Hall, 2 in Stadium, 3 in Space, 4 Whisper, 5 to 10 RoboSoft One to Six. The SAPI 4
// engine offered these modes as voices of their own ("Mike in Hall"); here every voice has them.

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"
#include "voices/sam_sing.h"
#include "sam4fx.h"

namespace svs {

namespace {

const char *const kVoiceNames[3] = {"Sam", "Mike", "Mary"};
constexpr int kVoiceStride = 20;
constexpr double kEngineRate = 22050.0;
//: the engine writes its output in chunks of 5000 samples, and the RoboSoft One and Six warble
//: changes its loop once per chunk, so the effect is fed the same chunks
constexpr int kEffectChunk = 5000;
//: the longest an echo is let ring on after the phrase (Stadium is the slowest to die away)
constexpr double kMaxTail = 4.0;

struct Mode {
    const char *suffix;   //: added to "Microsoft Sam" in the voice list
    const char *effect;   //: the decomp's name for it, for sam4fx_lookup
};

// The SAPI 4 voice modes, in the order of their numbers.
const Mode kModes[] = {
    {"", "none"},
    {" in Hall", "hall"},
    {" in Stadium", "stadium"},
    {" in Space", "space"},
    {" Whisper", "whisper"},
    {" RoboSoft One", "robosoft1"},
    {" RoboSoft Two", "robosoft2"},
    {" RoboSoft Three", "robosoft3"},
    {" RoboSoft Four", "robosoft4"},
    {" RoboSoft Five", "robosoft5"},
    {" RoboSoft Six", "robosoft6"},
};
constexpr int kModeCount = int(sizeof kModes / sizeof kModes[0]);

// VocalWriter's fifty-seven phonemes as the SAPI 5 US English phones Microsoft's voices have.
// Most are the same sound under another name. VocalWriter's r-coloured vowels and its syllabic
// consonants are single phonemes where SAPI spells them as two (AR is "aa r", EL "ax l"); its
// allophones of t (TX, the t of "it"; Q and QX, the glottal t of "greater") have no SAPI
// counterpart and are sung as plain t, and its flap DD ("better") as d. "reduced" marks the
// vowels that are never the one a note is about, so that the stress goes on a full vowel when
// a note has one.
struct PhoneMap {
    const char *vw;
    int sapi[3];       //: zero-terminated
    bool reduced;
};

const PhoneMap kPhoneMap[] = {
    // the vowels
    {"IY", {SVS_SAPI_IY}, false},              // bEEt
    {"IH", {SVS_SAPI_IH}, false},              // bIt
    {"EH", {SVS_SAPI_EH}, false},              // bEt
    {"AE", {SVS_SAPI_AE}, false},              // bAt
    {"AA", {SVS_SAPI_AA}, false},              // hOt
    {"UX", {SVS_SAPI_AH}, false},              // bUd: SAPI's ah is the strut vowel
    {"AO", {SVS_SAPI_AO}, false},              // cAUght
    {"UH", {SVS_SAPI_UH}, false},              // bOOk
    {"AX", {SVS_SAPI_AX}, true},               // About
    {"ER", {SVS_SAPI_ER}, false},              // bIRd
    {"EY", {SVS_SAPI_EY}, false},              // bAIt
    {"AY", {SVS_SAPI_AY}, false},              // bIte
    {"OY", {SVS_SAPI_OY}, false},              // bOY
    {"AW", {SVS_SAPI_AW}, false},              // bOUt
    {"OW", {SVS_SAPI_OW}, false},              // bOAt
    {"UW", {SVS_SAPI_UW}, false},              // bOOt
    {"YU", {SVS_SAPI_Y, SVS_SAPI_UW}, false},  // mUte: a y glide into uw
    {"IR", {SVS_SAPI_IH, SVS_SAPI_R}, false},  // bEER, as SAPI's dictionary spells it
    {"XR", {SVS_SAPI_EH, SVS_SAPI_R}, false},  // bEAR
    {"AR", {SVS_SAPI_AA, SVS_SAPI_R}, false},  // bAR
    {"OR", {SVS_SAPI_AO, SVS_SAPI_R}, false},  // fOR
    {"UR", {SVS_SAPI_UH, SVS_SAPI_R}, false},  // pOOR
    {"IX", {SVS_SAPI_IH}, true},               // rosEs: the reduced i, which SAPI writes ih
    {"%", {0}, false},                         // silence: nothing is sung
    {"RX", {SVS_SAPI_ER}, true},               // a short r-coloured vowel
    {"LX", {SVS_SAPI_L}, false},               // beLL: dark l, the same l to SAPI
    {"EL", {SVS_SAPI_AX, SVS_SAPI_L}, true},   // bottLE: syllabic l
    {"EN", {SVS_SAPI_AX, SVS_SAPI_N}, true},   // buttON: syllabic n
    // the consonants
    {"w", {SVS_SAPI_W}, false},
    {"y", {SVS_SAPI_Y}, false},
    {"r", {SVS_SAPI_R}, false},
    {"l", {SVS_SAPI_L}, false},
    {"h", {SVS_SAPI_H}, false},
    {"m", {SVS_SAPI_M}, false},
    {"n", {SVS_SAPI_N}, false},
    {"NG", {SVS_SAPI_NG}, false},
    {"f", {SVS_SAPI_F}, false},
    {"v", {SVS_SAPI_V}, false},
    {"TH", {SVS_SAPI_TH}, false},
    {"DH", {SVS_SAPI_DH}, false},
    {"s", {SVS_SAPI_S}, false},
    {"z", {SVS_SAPI_Z}, false},
    {"SH", {SVS_SAPI_SH}, false},
    {"ZH", {SVS_SAPI_ZH}, false},
    {"p", {SVS_SAPI_P}, false},
    {"b", {SVS_SAPI_B}, false},
    {"t", {SVS_SAPI_T}, false},
    {"d", {SVS_SAPI_D}, false},
    {"k", {SVS_SAPI_K}, false},
    {"g", {SVS_SAPI_G}, false},
    {"CH", {SVS_SAPI_CH}, false},
    {"JH", {SVS_SAPI_JH}, false},
    {"TX", {SVS_SAPI_T}, false},               // iT: an unreleased t
    {"Q", {SVS_SAPI_T}, false},                // greaTer: a glottal stop
    {"QX", {SVS_SAPI_T}, false},               // a longer Q
    {"DD", {SVS_SAPI_D}, false},               // beTTer: the flap
    {"O", {SVS_SAPI_AO}, false},               // bOy: the first part of OY
};

const PhoneMap *find_map(const std::string &symbol) {
    std::string s = palette_alias(symbol);   // OH and DX, as VocalWriter's MIDI files spell them
    for (const PhoneMap &m : kPhoneMap)
        if (s == m.vw) return &m;
    return nullptr;
}

bool has_vowel(const PhoneMap &m) {
    for (int k = 0; k < 3 && m.sapi[k]; ++k)
        if (svs_sam_phone_is_vowel(m.sapi[k])) return true;
    return false;
}

// One note's phonemes as SAPI phones, with a primary stress after the vowel the note is about:
// the first full vowel, or failing that the first vowel of any kind. Each note is sung as a word
// of its own (sam_sing.c says why), so each is stressed.
std::vector<int> note_phones(const std::vector<std::string> &phonemes) {
    std::vector<const PhoneMap *> maps;
    for (const std::string &p : phonemes)
        if (const PhoneMap *m = find_map(p)) maps.push_back(m);
    int nucleus = -1;
    for (int pass = 0; pass < 2 && nucleus < 0; ++pass)
        for (size_t i = 0; i < maps.size(); ++i)
            if (has_vowel(*maps[i]) && (pass == 1 || !maps[i]->reduced)) {
                nucleus = int(i);
                break;
            }
    std::vector<int> out;
    for (size_t i = 0; i < maps.size(); ++i) {
        bool stressed = false;
        for (int k = 0; k < 3 && maps[i]->sapi[k]; ++k) {
            out.push_back(maps[i]->sapi[k]);
            if (int(i) == nucleus && !stressed && svs_sam_phone_is_vowel(maps[i]->sapi[k])) {
                out.push_back(SVS_SAPI_ST1);
                stressed = true;
            }
        }
    }
    return out;
}

// A loaded voice file, shared by every singer of that voice. Synthesis only reads it.
struct VoiceData {
    sam_voice *voice = nullptr;
    ~VoiceData() { sam_voice_free(voice); }
};

double steady_hz(void *u, double sec) {
    const PitchCurve *c = static_cast<const PitchCurve *>(u);
    return PitchCurve::midi_to_hz(c->steady_midi_at(sec));
}

double vibrato_depth(void *u, double sec) {
    return static_cast<const PitchCurve *>(u)->vibrato_cents_at(sec);
}

void collect(const int16_t *pcm, size_t n, void *user) {
    auto *out = static_cast<std::vector<float> *>(user);
    out->insert(out->end(), pcm, pcm + n);
}

class MicrosoftSinger : public Singer {
public:
    MicrosoftSinger(std::shared_ptr<VoiceData> voice, int mode)
        : voice_(std::move(voice)), mode_(mode) {
        engine_ = svs_sam_open(voice_->voice);
        int mono = 0;
        if (sam4fx_lookup(kModes[mode_].effect, &preset_, &mono, &whisper_) != 0) {
            preset_ = 0;
            whisper_ = 0;
        }
        robot_ = mono != 0;
    }
    ~MicrosoftSinger() override { svs_sam_close(engine_); }

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (!engine_) {
            sung.stopped_short = true;
            return sung;
        }
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);

        // The notes' lengths are worked out exactly as PitchCurve works out its note starts,
        // so that the engine's running sum of them lands on curve.note_start(i).
        const double spb = 60.0 / std::max(phrase.bpm, 1e-6);
        std::vector<std::vector<int>> phones(phrase.notes.size());
        std::vector<svs_sam_note> notes(phrase.notes.size());
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            const SungNote &n = phrase.notes[i];
            phones[i] = note_phones(n.phonemes);
            notes[i].phones = phones[i].empty() ? nullptr : phones[i].data();
            notes[i].nphones = int(phones[i].size());
            notes[i].sec = n.beats * spb;
        }

        // The contour goes to the engine without its vibrato, which the synthesiser adds on
        // every pitch period instead: twenty knots a sound cannot draw five wobbles a second
        // through a long vowel. The RoboSoft modes that were monotone in SAPI 4 sing without
        // vibrato -- dead straight on the notes -- but still on the notes: flattening every
        // pitch to the voice's own, as the original did, would leave no melody to sing.
        svs_sam_sing_opts opts{};
        opts.consonant_scale = 1.0;
        opts.whisper = whisper_;
        if (!robot_) {
            opts.vibrato_cents = curve.vibrato_cents();
            opts.vibrato_hz = curve.vibrato_hz();
            if (curve.has_mod()) {          // the mod wheel moves the depth as it goes
                opts.vibrato_at = vibrato_depth;
                opts.vibrato_user = &curve;
            }
        }
        std::vector<float> pcm;
        pcm.reserve(size_t(curve.total() * kEngineRate) + 1024);
        int rc = svs_sam_sing_phones(engine_, notes.data(), int(notes.size()), steady_hz,
                                     &curve, &opts, collect, &pcm);
        if (rc != 0) sung.stopped_short = true;

        apply_effect(&pcm);

        std::vector<float> out = resample_to_output(pcm, kEngineRate);
        // 16-bit scale to +-1, at the part's level; anything that would still clip (a RoboSoft
        // ringing up, the resampler's overshoot) brings the whole phrase down rather than
        // being cut off.
        double gain = phrase.level / 32768.0, peak = 0.0;
        for (float &s : out) {
            s = float(s * gain);
            peak = std::max(peak, double(std::fabs(s)));
        }
        if (peak > 0.999) {
            float k = float(0.999 / peak);
            for (float &s : out) s *= k;
        }
        sung.samples = std::move(out);
        return sung;
    }

private:
    // The SAPI 4 effect, on the 22 kHz stream the way the engine ran it. RoboSoft rings up to
    // 9 dB louder, so its input is trimmed the way the decomp's sam_say trims it, which keeps it
    // out of the effect's own clipping and about as loud as the plain voice. Whisper is the
    // noise excitation (set in the synthesiser) and this FIR after it. The echoes are let ring
    // on past the phrase until they have died away.
    void apply_effect(std::vector<float> *pcm) {
        if (!preset_ && !whisper_) return;
        sam4fx *fx = sam4fx_new(preset_, int(kEngineRate));
        if (!fx) return;
        float trim = sam4fx_default_trim_db(preset_);
        if (trim != 0.0f) sam4fx_set_input_gain(fx, float(std::pow(10.0, trim / 20.0)));
        if (whisper_) {
            static const float taps[3] = {0.25f, -0.5f, 0.25f};
            sam4fx_set_fir(fx, taps, 3);
        }
        for (size_t at = 0; at < pcm->size(); at += kEffectChunk) {
            int n = int(std::min<size_t>(kEffectChunk, pcm->size() - at));
            sam4fx_process(fx, pcm->data() + at, n);
        }
        if (preset_) {
            int left = int(kMaxTail * kEngineRate);
            while (left > 0) {
                std::vector<float> z(size_t(kEffectChunk), 0.0f);
                sam4fx_process(fx, z.data(), kEffectChunk);
                float peak = 0.0f;
                for (float v : z) peak = std::max(peak, std::fabs(v));
                if (peak < 0.5f) break;   // below the 16-bit stream's last bit
                pcm->insert(pcm->end(), z.begin(), z.end());
                left -= kEffectChunk;
            }
        }
        sam4fx_free(fx);
    }

    std::shared_ptr<VoiceData> voice_;
    int mode_;
    sam_tts *engine_ = nullptr;
    int preset_ = 0, whisper_ = 0;
    bool robot_ = false;
};

class MicrosoftFamily : public Family {
public:
    explicit MicrosoftFamily(std::string dir = std::string()) : fixed_dir_(std::move(dir)) {}

    std::string name() const override { return "Microsoft"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        for (int v = 0; v < 3; ++v)
            for (int m = 0; m < kModeCount; ++m)
                out.push_back({kMicrosoftBase + v * kVoiceStride + m,
                               std::string("Microsoft ") + kVoiceNames[v] + kModes[m].suffix});
        return out;
    }

    bool available(std::string *why) override {
        std::string d = dir();
        for (const char *v : kVoiceNames)
            if (file_exists(spd_path(d, v))) return true;
        if (why)
            *why = "Microsoft Sam, Mike and Mary need Microsoft's voice files, Sam.spd, "
                   "Mike.spd or Mary.spd, in voices\\microsoft (" + d + "). They come with "
                   "Windows XP and the SAPI 5.1 runtime, in Program Files\\Common Files\\"
                   "Microsoft Shared\\Speech.";
        return false;
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int rel = voice_id - kMicrosoftBase;
        int v = rel / kVoiceStride, m = rel % kVoiceStride;
        if (rel < 0 || v >= 3 || m >= kModeCount) {
            if (err) *err = "There is no Microsoft voice number " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        std::shared_ptr<VoiceData> data = load(v, err);
        if (!data) return nullptr;
        return std::make_unique<MicrosoftSinger>(data, m);
    }

private:
    // voices\microsoft beside the program first, so a copy put there wins; then wherever
    // Microsoft's own installers put the voices -- the SAPI 5 TTS package of 2005 (the one
    // Windows still accepts) and Windows XP's own layout.
    std::string dir() const {
        if (!fixed_dir_.empty()) return fixed_dir_;
        std::vector<std::string> places{find_data("voices\\microsoft")};
        for (const char *env : {"CommonProgramFiles(x86)", "CommonProgramFiles"}) {
            const char *base = std::getenv(env);
            if (!base || !*base) continue;
            places.push_back(join_path(base, "SpeechEngines\\Microsoft\\TTS\\1033"));
            places.push_back(join_path(base, "Microsoft Shared\\Speech\\1033"));
        }
        for (const std::string &d : places)
            for (const char *v : kVoiceNames)
                if (file_exists(spd_path(d, v))) return d;
        return places.front();
    }
    static std::string spd_path(const std::string &d, const char *voice) {
        return join_path(d, std::string(voice) + ".spd");
    }

    // A voice file is about ten megabytes; it is read once and kept for as long as the family
    // lives, shared by every singer of that voice whatever its mode.
    std::shared_ptr<VoiceData> load(int v, std::string *err) {
        std::string d = dir(), path = spd_path(d, kVoiceNames[v]);
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(path);
        if (it != cache_.end()) return it->second;
        std::vector<unsigned char> bytes;
        if (!file_exists(path) || !read_file(path, &bytes)) {
            if (err)
                *err = std::string("Microsoft ") + kVoiceNames[v] + "'s voice file, " +
                       kVoiceNames[v] + ".spd, is not in voices\\microsoft (" + d + ").";
            return nullptr;
        }
        char msg[256] = {0};
        auto data = std::make_shared<VoiceData>();
        data->voice = sam_voice_load_mem(bytes.data(), bytes.size(), msg, sizeof msg);
        if (!data->voice) {
            if (err)
                *err = std::string("Microsoft ") + kVoiceNames[v] + "'s voice file, " + path +
                       ", cannot be read" + (msg[0] ? std::string(": ") + msg : std::string()) + ".";
            return nullptr;
        }
        cache_[path] = data;
        return data;
    }

    std::string fixed_dir_;
    std::mutex mu_;
    std::map<std::string, std::shared_ptr<VoiceData>> cache_;
};

}  // namespace

std::unique_ptr<Family> make_microsoft_family() { return std::make_unique<MicrosoftFamily>(); }

//: Not part of family.h: the same family reading its voice files from `voice_dir` itself, for
//: the engine's own test program.
std::unique_ptr<Family> make_microsoft_family_at(const std::string &voice_dir) {
    return std::make_unique<MicrosoftFamily>(voice_dir);
}

//: Not part of family.h: the self-check the test program runs without any voice data. Every
//: VocalWriter phoneme has a row in the table, every row's phones are ones the engine has, and
//: every note made of a vowel gets its stress. Returns an empty string, or what is wrong.
std::string microsoft_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes()) {
        const PhoneMap *m = find_map(p.symbol);
        if (!m) {
            bad += std::string("no SAPI mapping for ") + p.symbol + "; ";
            continue;
        }
        if (std::string(p.symbol) == kRest) {
            if (m->sapi[0]) bad += "% should be silence; ";
            continue;
        }
        if (!m->sapi[0]) bad += std::string(p.symbol) + " maps to nothing; ";
        for (int k = 0; k < 3 && m->sapi[k]; ++k)
            if (!svs_sam_knows_phone(m->sapi[k]))
                bad += std::string(p.symbol) + " maps to SAPI " + std::to_string(m->sapi[k]) +
                       ", which the engine does not have; ";
        if (is_vowel(p.symbol) && !has_vowel(*m))
            bad += std::string(p.symbol) + " is a vowel but its SAPI phones have none; ";
        if (is_vowel(p.symbol)) {
            std::vector<int> ph = note_phones({"b", p.symbol});
            if (std::count(ph.begin(), ph.end(), int(SVS_SAPI_ST1)) != 1)
                bad += std::string("b ") + p.symbol + " is not stressed once; ";
        }
    }
    for (const PhoneMap &m : kPhoneMap)
        if (!phoneme(m.vw)) bad += std::string(m.vw) + " is not a VocalWriter phoneme; ";
    if (sizeof kPhoneMap / sizeof kPhoneMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kPhoneMap / sizeof kPhoneMap[0]) +
               " rows for " + std::to_string(phonemes().size()) + " phonemes; ";
    // all forty SAPI sounds are there, and the stress goes on the full vowel of "a-bout"
    for (int s = SVS_SAPI_AA; s <= SVS_SAPI_ZH; ++s)
        if (!svs_sam_knows_phone(s)) bad += "the engine lacks SAPI phone " + std::to_string(s) + "; ";
    std::vector<int> about = note_phones({"AX", "b", "AW", "t"});
    std::vector<int> want = {SVS_SAPI_AX, SVS_SAPI_B, SVS_SAPI_AW, SVS_SAPI_ST1, SVS_SAPI_T};
    if (about != want) bad += "AX b AW t is not stressed on AW; ";
    return bad;
}

}  // namespace svs
