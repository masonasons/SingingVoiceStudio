// soundfont.cpp -- SoundFonts, played by TinySoundFont (tinysoundfont.c).
//
// A part is played as a MIDI channel would play it: each note on at its beat
// and off when it has sounded for its beats, or its hold; the instrument
// changes, the volume, the pedal, the detune and the bend at their moments.
// The pan is the mix's. The presets' attenuation, their modulators, the
// velocity and the volume are taken as the SoundFont players take them
// (tinysoundfont.h), which TinySoundFont alone does not, and so is the
// track's volume, where a MIDI file's CC 7 by CC 11 comes in.
#include "voices/soundfont.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "core/paths.h"
#include "voices/tinysoundfont.h"

namespace svs {

namespace {

uint32_t le32(const unsigned char *b) { return b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24); }
uint16_t le16(const unsigned char *b) { return uint16_t(b[0] | (b[1] << 8)); }

std::FILE *open_read(const std::string &path) {
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

//: A preset's name as the file has it: up to 20 bytes, read as Latin-1.
std::string preset_name(const unsigned char *b) {
    std::string out;
    for (int i = 0; i < 20 && b[i]; ++i) {
        unsigned char c = b[i];
        if (c < 0x20) c = ' ';
        if (c < 0x80) {
            out.push_back(char(c));
        } else {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t lead = out.find_first_not_of(' ');
    return lead == std::string::npos ? std::string() : out.substr(lead);
}

bool looks_like_kit(std::string name) {
    for (char &c : name) c = char(std::tolower((unsigned char)c));
    return name.find("drum") != std::string::npos || name.find("kit") != std::string::npos ||
           name.find("perc") != std::string::npos;
}

//: The preset headers (phdr) of a SoundFont, without its samples; false with
//: `error` set if the file is not one.
bool read_presets(const std::string &path, std::vector<SoundFontPreset> *out, std::string *error) {
    std::FILE *f = open_read(path);
    if (!f) {
        *error = "it could not be opened";
        return false;
    }
    unsigned char head[12];
    bool ok = std::fread(head, 1, 12, f) == 12 && std::memcmp(head, "RIFF", 4) == 0 &&
              std::memcmp(head + 8, "sfbk", 4) == 0;
    bool found = false;
    // the top level's chunks, then the pdta list's
    long list_end = -1;
    while (ok && !found) {
        unsigned char ch[8];
        if (std::fread(ch, 1, 8, f) != 8) break;
        uint32_t size = le32(ch + 4);
        if (std::memcmp(ch, "LIST", 4) == 0 && list_end < 0) {
            unsigned char type[4];
            if (std::fread(type, 1, 4, f) != 4) break;
            if (std::memcmp(type, "pdta", 4) == 0) {
                list_end = std::ftell(f) + long(size) - 4;
                continue;
            }
            if (std::fseek(f, long(size - 4 + (size & 1)), SEEK_CUR) != 0) break;
            continue;
        }
        if (list_end >= 0 && std::memcmp(ch, "phdr", 4) == 0) {
            // 38 bytes to a preset, and a last one that only ends the list
            std::vector<unsigned char> b(size);
            if (size < 76 || std::fread(b.data(), 1, size, f) != size) break;
            for (size_t i = 0; i + 1 < size / 38; ++i) {
                const unsigned char *r = b.data() + i * 38;
                SoundFontPreset p;
                p.name = p.plain = preset_name(r);
                p.program = le16(r + 20);
                p.bank = le16(r + 22);
                p.drums = p.bank == 128 || (p.bank >= 120 && looks_like_kit(p.name));
                out->push_back(p);
            }
            found = true;
            break;
        }
        if (list_end >= 0 && std::ftell(f) >= list_end) break;
        if (std::fseek(f, long(size + (size & 1)), SEEK_CUR) != 0) break;
    }
    std::fclose(f);
    if (!found) {
        *error = ok ? "it has no presets" : "it is not a SoundFont 2 file";
        return false;
    }
    if (out->empty()) {
        *error = "it has no presets";
        return false;
    }
    return true;
}

//: names a SoundFont is not given: the other families', and "GM", which the
//: GM Soundbank's voices begin with
const char *const kBuiltIn[] = {"VocalWriter", "DECtalk", "SSI-263", "Microsoft", "MacinTalk", "SharpVox",
                                "GM Soundbank", "GM"};

std::mutex g_mu;
std::vector<SoundFontInfo> g_fonts;
int g_version = 0;

//: The SoundFont a voice is one of, under g_mu, or null.
const SoundFontInfo *font_of(int voice) {
    if (!is_soundfont_voice(voice)) return nullptr;
    int slot = (voice - kSoundFontBase) / kFamilySpan;
    for (const SoundFontInfo &f : g_fonts)
        if (f.slot == slot) return &f;
    return nullptr;
}

// -- playing ----------------------------------------------------------------

int read_stream(void *data, void *ptr, unsigned int size) {
    return int(std::fread(ptr, 1, size, static_cast<std::FILE *>(data)));
}
int skip_stream(void *data, unsigned int count) {
    return std::fseek(static_cast<std::FILE *>(data), long(count), SEEK_CUR) == 0;
}

//: A SoundFont's samples, read once; each singer plays a copy of it
//: (tsf_copy), made and closed under `mu`.
struct Font {
    std::mutex mu;
    tsf *base = nullptr;
    svs_tsf_mods *mods = nullptr;
    ~Font() {
        svs_tsf_free_mods(mods);
        tsf_close(base);
    }
};

class SoundFontSinger : public Singer {
public:
    SoundFontSinger(std::shared_ptr<Font> font, const SoundFontInfo &info, int voice)
        : font_(std::move(font)), info_(info), voice_(voice) {
        std::lock_guard<std::mutex> lock(font_->mu);
        f_ = tsf_copy(font_->base);
    }
    ~SoundFontSinger() override {
        std::lock_guard<std::mutex> lock(font_->mu);
        tsf_close(f_);
        svs_tsf_free_voices(voices_);
    }

    //: A phrase, as a part that starts with it.
    Sung sing(const Phrase &phrase) override {
        Part part;
        part.bpm = phrase.bpm;
        part.tempos = phrase.tempos;
        part.notes = phrase.notes;
        part.bends = phrase.bends;
        part.any_bends = phrase.any_bends;
        part.level = phrase.level;
        for (const auto &h : phrase.held) part.controls.push_back({0.0, h.first, h.second});
        part.controls.insert(part.controls.end(), phrase.controls.begin(), phrase.controls.end());
        return sing_part(part);
    }

    Sung sing_part(const Part &part) override {
        Sung out;
        out.channels = 2;
        if (!f_) {
            out.error = "there was not the memory to play " + info_.name;
            return out;
        }
        tsf_reset(f_);
        tsf_set_output(f_, TSF_STEREO_INTERLEAVED, kSampleRate, 0.0f);
        const int ch = 0;
        int first = preset_index(voice_);
        if (first < 0) {
            out.error = info_.name + " has no preset for voice " + std::to_string(voice_);
            return out;
        }
        tsf_channel_set_presetindex(f_, ch, first);

        // everything that happens, in seconds from the part's start
        enum Kind { kPreset, kVolume, kSustain, kTuning, kWheel, kOff, kOn };
        struct Ev {
            double t;
            Kind kind;
            int a;
            double v;
        };
        std::vector<Ev> ev;
        auto seconds = [&](double beat) { return seconds_into(part.bpm, part.tempos, beat); };
        // The detune as VocalWriter's sequencer takes it (music.c, Set_Freq):
        // steps of a 32nd of a semitone, the whole range a semitone either
        // way, and four on a drum kit's track.
        const int detune_shift = is_drum_kit(voice_) ? 6 : 8;
        auto control = [&](double t, const std::string &name, double value) {
            if (name == "volume") {
                // as MIDI's channel volume: 40 log10(value / 127) dB
                double v = std::max(0.0, std::min(127.0, value)) / 127.0;
                ev.push_back({t, kVolume, 0, v * v});
            } else if (name == "sustain") {
                ev.push_back({t, kSustain, 0, value});
            } else if (name == "detune") {
                long v = std::max(-8192L, std::min(8191L, std::lround(value)));
                ev.push_back({t, kTuning, 0, double(v >> detune_shift) / 32.0});
            }
        };
        for (const auto &v : part.voices) {
            int index = preset_index(v.second);
            if (index >= 0) ev.push_back({seconds(v.first), kPreset, index, 0});
        }
        for (const ControlAt &c : part.controls) control(seconds(c.beat), c.name, c.value);
        double beat = 0, last = 0;
        for (size_t i = 0; i < part.notes.size(); ++i) {
            const SungNote &n = part.notes[i];
            for (const auto &c : n.controls)
                for (const auto &pt : c.second) control(seconds(beat + pt.first * n.beats), c.first, pt.second);
            bool rest = true;
            for (const std::string &p : n.phonemes) rest = rest && p == kRest;
            if (!rest) {
                // a chord's note held for no beats of its own sounds as long
                // as the note it is played with
                double len = n.hold > 0 ? n.hold : n.beats;
                for (size_t k = i + 1; len <= 0 && k < part.notes.size(); ++k) len = part.notes[k].beats;
                double on = seconds(beat), off = std::max(seconds(beat + len), on + 0.01);
                int key = std::max(0, std::min(127, n.midi));
                ev.push_back({on, kOn, key, double(std::max(1, std::min(127, n.velocity)))});
                ev.push_back({off, kOff, key, 0});
                last = std::max(last, off);
            }
            beat += n.beats;
        }
        if (ev.empty()) return out;
        // the bends in semitones, on a wheel wide enough for the widest
        double widest = 0;
        for (const BendEvent &b : part.bends) widest = std::max(widest, std::fabs(b.semitones));
        double range = std::max(2.0, std::ceil(widest - 1e-9));
        tsf_channel_set_pitchrange(f_, ch, float(range));
        for (const BendEvent &b : part.bends)
            ev.push_back({b.seconds, kWheel, int(std::lround(std::max(0.0, std::min(16383.0, 8192 + b.semitones / range * 8192)))), 0});
        std::stable_sort(ev.begin(), ev.end(), [](const Ev &x, const Ev &y) { return x.t != y.t ? x.t < y.t : x.kind < y.kind; });
        double end = std::max(last, part.end > 0 ? seconds(part.end) : 0.0);

        SingWatch *watch = sing_watch();
        size_t done = 0;
        long next_report = 0;
        // the samples up to `frame`; false once the watch says stop
        auto render = [&](size_t frame) {
            while (done < frame) {
                size_t n = std::min<size_t>(frame - done, 4096);
                out.samples.resize((done + n) * 2);
                tsf_render_float(f_, out.samples.data() + done * 2, int(n), 0);
                done += n;
                if (watch && long(done) >= next_report) {
                    next_report = long(done) + kSampleRate / 4;
                    if (!watch->sung(double(done) / kSampleRate)) return false;
                }
            }
            return true;
        };
        for (const Ev &e : ev) {
            if (!render(size_t(std::max(0L, std::lround(e.t * kSampleRate))))) return out;
            switch (e.kind) {
            case kPreset: tsf_channel_set_presetindex(f_, ch, e.a); break;
            case kVolume: tsf_channel_set_volume(f_, ch, float(e.v)); break;
            case kSustain: tsf_channel_set_sustain(f_, ch, e.v >= 64); break;
            case kTuning: tsf_channel_set_tuning(f_, ch, float(e.v)); break;
            case kWheel: tsf_channel_set_pitchwheel(f_, ch, e.a); break;
            case kOn: svs_tsf_note_on(f_, font_->mods, &voices_, ch, e.a, int(e.v)); break;
            // the key's oldest note still sounding, as a MIDI channel lets go
            case kOff: tsf_channel_note_off(f_, ch, e.a); break;
            }
        }
        if (!render(size_t(std::lround(end * kSampleRate)))) return out;
        // let go of the pedal and of whatever still sounds, and let it ring out
        tsf_channel_set_sustain(f_, ch, 0);
        tsf_channel_note_off_all(f_, ch);
        size_t limit = done + size_t(10 * kSampleRate);
        while (tsf_active_voice_count(f_) > 0 && done < limit)
            if (!render(done + 1024)) return out;
        // the track's volume as MIDI's channel volume, as the volume control
        if (part.level != 1.0)
            for (float &s : out.samples) s = float(s * part.level * part.level);
        return out;
    }

private:
    int preset_index(int voice) const {
        int i = voice - soundfont_base(info_.slot);
        if (i < 0 || i >= int(info_.presets.size())) return -1;
        return tsf_get_presetindex(f_, info_.presets[size_t(i)].bank, info_.presets[size_t(i)].program);
    }

    std::shared_ptr<Font> font_;
    SoundFontInfo info_;
    int voice_;
    tsf *f_ = nullptr;
    //: the regions its voices play, with their modulators applied
    svs_tsf_voices *voices_ = nullptr;
};

class SoundFontFamily : public Family {
public:
    explicit SoundFontFamily(const SoundFontInfo &info) : info_(info) {}

    std::string name() const override { return info_.name; }
    bool sings_parts() const override { return true; }
    bool plays() const override { return true; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
        int base = soundfont_base(info_.slot);
        for (size_t i = 0; i < info_.presets.size(); ++i)
            out.push_back({base + int(i), info_.name + " " + info_.presets[i].name, "", true});
        return out;
    }

    bool available(std::string *why) override {
        std::lock_guard<std::mutex> lock(mu_);
        std::string no;
        if (retired_) no = "the SoundFont " + info_.name + " was removed in the Soundbank manager";
        else if (!info_.error.empty()) no = "the SoundFont " + info_.path + " cannot be read: " + info_.error;
        else if (!file_exists(info_.path)) no = "the SoundFont " + info_.path + " is not there any more";
        else if (!load_error_.empty()) no = load_error_;
        if (no.empty()) return true;
        if (why) *why = no;
        return false;
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        if (!available(err)) return nullptr;
        std::lock_guard<std::mutex> lock(mu_);
        if (!font_) {
            // the samples, read the first time a voice plays
            std::FILE *f = open_read(info_.path);
            tsf *base = nullptr;
            svs_tsf_mods *mods = nullptr;
            if (f) {
                tsf_stream stream{f, read_stream, skip_stream};
                base = tsf_load(&stream);
                if (base) mods = svs_tsf_read_mods(base, f);
                std::fclose(f);
            }
            if (!base) {
                load_error_ = "the SoundFont " + info_.path + " would not load";
                if (err) *err = load_error_;
                return nullptr;
            }
            svs_tsf_attenuation(base);
            font_ = std::make_shared<Font>();
            font_->base = base;
            font_->mods = mods;
        }
        auto s = std::make_unique<SoundFontSinger>(font_, info_, voice_id);
        return s;
    }

    void retire() {
        std::lock_guard<std::mutex> lock(mu_);
        retired_ = true;
        font_.reset();          // the samples go when the last singer does
    }

private:
    SoundFontInfo info_;
    std::mutex mu_;
    std::shared_ptr<Font> font_;
    bool retired_ = false;
    std::string load_error_;
};

}  // namespace

SoundFontInfo read_soundfont(const std::string &path, int slot) {
    SoundFontInfo info;
    info.path = path;
    info.slot = slot;
    info.name = strip_extension(base_name(path));
    std::vector<SoundFontPreset> presets;
    if (!read_presets(path, &presets, &info.error)) return info;
    std::stable_sort(presets.begin(), presets.end(), [](const SoundFontPreset &a, const SoundFontPreset &b) {
        return a.bank != b.bank ? a.bank < b.bank : a.program < b.program;
    });
    info.total = presets.size();
    if (presets.size() > size_t(kSoundFontPresets)) presets.resize(size_t(kSoundFontPresets));
    // a name used twice is told apart by where the preset is
    std::map<std::string, int> uses;
    for (const SoundFontPreset &p : presets) ++uses[p.name];
    for (SoundFontPreset &p : presets) {
        if (p.name.empty() || uses[p.name] > 1)
            p.name += (p.name.empty() ? "" : " ") + std::string("(bank ") + std::to_string(p.bank) + " program " +
                      std::to_string(p.program) + ")";
    }
    info.presets = presets;
    return info;
}

void set_soundfonts(const std::vector<SoundFontFile> &files) {
    std::vector<SoundFontInfo> fonts;
    for (const SoundFontFile &f : files) {
        SoundFontInfo info = read_soundfont(f.path, f.slot);
        // a family's name is its own: another of the same name is numbered
        std::string name = info.name;
        for (int n = 2;; ++n) {
            bool taken = std::find(std::begin(kBuiltIn), std::end(kBuiltIn), info.name) != std::end(kBuiltIn);
            for (const SoundFontInfo &o : fonts) taken = taken || o.name == info.name;
            if (!taken) break;
            info.name = name + " (" + std::to_string(n) + ")";
        }
        fonts.push_back(info);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_fonts = fonts;
    ++g_version;
}

std::vector<SoundFontInfo> soundfonts() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_fonts;
}

int soundfonts_version() {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_version;
}

std::optional<SoundFontPreset> soundfont_preset(int voice) {
    std::lock_guard<std::mutex> lock(g_mu);
    const SoundFontInfo *f = font_of(voice);
    if (!f) return std::nullopt;
    size_t i = size_t(voice - soundfont_base(f->slot));
    if (i >= f->presets.size()) return std::nullopt;
    return f->presets[i];
}

int soundfont_slot(const std::string &path) {
    std::lock_guard<std::mutex> lock(g_mu);
    for (const SoundFontInfo &f : g_fonts)
        if (f.path == path) return f.slot;
    return -1;
}

bool is_drum_kit(int id) {
    if (id == kGmDrums) return true;
    std::optional<SoundFontPreset> p = soundfont_preset(id);
    return p && p->drums;
}

int soundfont_voice_for(int gm_voice, int slot, int drum_program) {
    if (!is_gm_voice(gm_voice)) return gm_voice;
    std::lock_guard<std::mutex> lock(g_mu);
    const SoundFontInfo *f = font_of(soundfont_base(slot));
    if (!f || !f->error.empty()) return gm_voice;
    int base = soundfont_base(slot);
    const std::vector<SoundFontPreset> &ps = f->presets;
    auto find = [&](auto want) {
        for (size_t i = 0; i < ps.size(); ++i)
            if (want(ps[i])) return base + int(i);
        return -1;
    };
    int v;
    if (gm_voice == kGmDrums) {
        v = find([&](const SoundFontPreset &p) { return p.drums && p.bank == 128 && p.program == drum_program; });
        if (v < 0) v = find([](const SoundFontPreset &p) { return p.drums && p.bank == 128 && p.program == 0; });
        if (v < 0) v = find([](const SoundFontPreset &p) { return p.drums; });
    } else {
        int program = gm_voice - kGmBase;
        v = find([&](const SoundFontPreset &p) { return p.bank == 0 && p.program == program; });
        if (v < 0) v = find([&](const SoundFontPreset &p) { return !p.drums && p.program == program; });
    }
    return v < 0 ? gm_voice : v;
}

std::string soundfont_program_name(int program, bool drums, int slot) {
    int voice = soundfont_voice_for(drums ? kGmDrums : kGmBase + program, slot);
    std::optional<SoundFontPreset> p = soundfont_preset(voice);
    if (p && !p->plain.empty()) return p->plain;
    return drums ? "Drums" : gm_program_name(program);
}

void play_on_soundfont(std::optional<int> &voice, std::vector<Note> &notes, int slot) {
    if (voice) voice = soundfont_voice_for(*voice, slot);
    for (Note &n : notes)
        if (n.controller && n.voice_id && is_gm_voice(*n.voice_id)) {
            n.voice_id = soundfont_voice_for(*n.voice_id, slot);
            n.voice_name.reset();
        }
}

std::unique_ptr<Family> make_soundfont_family(const SoundFontInfo &info) {
    return std::make_unique<SoundFontFamily>(info);
}

void retire_soundfont_family(Family *family) {
    if (auto *f = dynamic_cast<SoundFontFamily *>(family)) f->retire();
}

}  // namespace svs
