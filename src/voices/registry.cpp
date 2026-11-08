#include "voices/registry.h"

#include "voices/soundfont.h"

namespace svs {

Registry &Registry::get() {
    static Registry instance;
    return instance;
}

Registry::Registry() {
    families_.push_back(make_vocalwriter_family());
    families_.push_back(make_dectalk_family());
    families_.push_back(make_ssi263_family());
    families_.push_back(make_microsoft_family());
    families_.push_back(make_macintalk_family());
    families_.push_back(make_eloquence_family());
    families_.push_back(make_bestspeech_family());
    families_.push_back(make_prose_family());
    families_.push_back(make_sharpvox_family());
    families_.push_back(make_gm_family());
}

void Registry::soundfonts_locked() {
    int version = soundfonts_version();
    if (version == soundfonts_seen_) return;
    soundfonts_seen_ = version;
    while (families_.size() > kBuiltInFamilies) {
        retire_soundfont_family(families_.back().get());
        retired_.push_back(std::move(families_.back()));
        families_.pop_back();
    }
    soundfont_bases_.clear();
    for (const SoundFontInfo &info : soundfonts()) {
        families_.push_back(make_soundfont_family(info));
        soundfont_bases_.push_back(soundfont_base(info.slot));
    }
    cache_.clear();
    listed_ = false;
}

std::vector<VoiceListing> Registry::voices() {
    std::lock_guard<std::mutex> lock(mu_);
    soundfonts_locked();
    if (!listed_) {
        for (auto &f : families_) {
            bool ok = f->available(nullptr);
            for (const VoiceEntry &v : f->voices())
                cache_.push_back({v.id, v.name, v.group.empty() ? f->name() : v.group, ok && v.installed});
        }
        listed_ = true;
    }
    return cache_;
}

Family *Registry::family_of(int voice_id) {
    int base = voice_id < 0 ? 0 : (voice_id / kFamilySpan) * kFamilySpan;
    std::lock_guard<std::mutex> lock(mu_);
    soundfonts_locked();
    if (is_soundfont_voice(voice_id)) {
        for (size_t i = 0; i < soundfont_bases_.size(); ++i)
            if (soundfont_bases_[i] == base) return families_[kBuiltInFamilies + i].get();
        return nullptr;
    }
    for (auto &f : families_) {
        if (base == kVocalWriterBase && f->name() == "VocalWriter") return f.get();
        if (base == kDectalkBase && f->name() == "DECtalk") return f.get();
        if (base == kSsi263Base && f->name() == "SSI-263") return f.get();
        if (base == kMicrosoftBase && f->name() == "Microsoft") return f.get();
        if (base == kMacinTalkBase && f->name() == "MacinTalk") return f.get();
        if (base == kEloquenceBase && f->name() == "Eloquence") return f.get();
        if (base == kBestBase && f->name() == "BeSTspeech") return f.get();
        if (base == kProseBase && f->name() == "Prose 2000") return f.get();
        if (base == kSharpVoxBase && f->name() == "SharpVox") return f.get();
        if (base == kGmBase && f->name() == "GM Soundbank") return f.get();
    }
    return nullptr;
}

std::string Registry::voice_name(int voice_id) {
    for (const VoiceListing &v : voices())
        if (v.id == voice_id) return v.name;
    return "voice " + std::to_string(voice_id);
}

std::unique_ptr<Singer> Registry::singer(int voice_id, std::string *err) {
    Family *f = family_of(voice_id);
    if (!f) {
        if (err) *err = "there is no voice " + std::to_string(voice_id);
        return nullptr;
    }
    std::string why;
    if (!f->available(&why)) {
        if (err) *err = why;
        return nullptr;
    }
    return f->singer(voice_id, err);
}

double Registry::headroom(int voice_id) {
    Family *f = family_of(voice_id);
    return f ? f->headroom(voice_id) : 1.0;
}

std::vector<std::string> Registry::status() {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(mu_);
    soundfonts_locked();
    for (auto &f : families_) {
        std::string why;
        bool ok = f->available(&why);
        // only the voices that can sing: MacinTalk lists the ones that cannot
        // as well, so the user can be told why
        size_t n = 0;
        for (const VoiceEntry &v : f->voices()) n += v.installed;
        out.push_back(f->name() + ": " +
                      (ok ? std::to_string(n) + " voices ready" : why));
    }
    return out;
}

}  // namespace svs

namespace svs {

//: One voice put back by its name; true if it moved. A SoundFont's voice is
//: put only on a SoundFont's, and another engine's only on another engine's,
//: as a SoundFont may name a voice as another engine does; unless its number
//: is no voice at all.
static bool by_name(std::optional<int> &id, const std::optional<std::string> &name,
                    const std::vector<VoiceListing> &voices) {
    if (!name || name->empty()) return false;
    bool known = false;
    for (const VoiceListing &v : voices) {
        if (id && v.id == *id && v.name == *name) return false;
        known = known || (id && v.id == *id);
    }
    bool soundfont = id && is_soundfont_voice(*id);
    for (const VoiceListing &v : voices)
        if (v.name == *name && (!known || is_soundfont_voice(v.id) == soundfont)) {
            id = v.id;
            return true;
        }
    return false;
}

//: A voice no longer offered, put on the one it sounded as: DECtalk's Val is
//: where DECtalk keeps a voice of the user's own, and was only Paul. A voice
//: renamed since is given its new name: the SSI-263's first voice, once
//: "SSI-263", is Standard.
static bool retired(std::optional<int> &id, std::optional<std::string> &name) {
    if (name && *name == "SSI-263") name = "SSI-263 Standard";
    if (!(id && *id == kDectalkBase + 9) && !(name && *name == "DECtalk Val")) return false;
    id = kDectalkBase;
    name = "DECtalk Paul";
    return true;
}

bool resolve_voice_names(Song &song, const std::vector<VoiceListing> &voices) {
    bool moved = false;
    for (Track &t : song.tracks) {
        moved = retired(t.voice_id, t.voice_name) || moved;
        moved = by_name(t.voice_id, t.voice_name, voices) || moved;
        // and every voice a controller changes to
        for (Note &n : t.notes)
            if (n.controller) {
                moved = retired(n.voice_id, n.voice_name) || moved;
                moved = by_name(n.voice_id, n.voice_name, voices) || moved;
            }
    }
    return moved;
}

std::optional<std::string> saved_voice_name(int voice_id, const std::vector<VoiceListing> &voices) {
    // VocalWriter's own voices are only a number, as VocalWriter Studio
    // writes them, so a song sung by them alone is the same file it was
    if (voice_id < kFamilySpan) return std::nullopt;
    for (const VoiceListing &v : voices)
        if (v.id == voice_id) return v.name;
    return std::nullopt;
}

void name_voices(Song &song, const std::vector<VoiceListing> &voices) {
    for (Track &t : song.tracks) {
        if (t.voice_id && !t.voice_name) t.voice_name = saved_voice_name(*t.voice_id, voices);
        for (Note &n : t.notes)
            if (n.controller && n.voice_id && !n.voice_name) n.voice_name = saved_voice_name(*n.voice_id, voices);
    }
}

}  // namespace svs
