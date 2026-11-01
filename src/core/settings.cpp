#include "core/settings.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

#include <json.hpp>

#include "core/paths.h"

namespace svs {

using json = nlohmann::ordered_json;

namespace {

std::string in_folder(const char *name) { return join_path(settings_dir(), name); }

bool write_text(const std::string &path, const std::string &text) {
#ifdef _WIN32
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
#else
    FILE *f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    return fclose(f) == 0 && ok;
}

bool read_json(const std::string &path, json *out) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b)) return false;
    try {
        *out = json::parse(b.begin(), b.end());
        return true;
    } catch (...) {
        return false;
    }
}

const char *const kNotes = "recovery.vws";
const char *const kMark = "recovery.json";

}  // namespace

Settings load_settings() {
    Settings s;
    json doc;
    if (!read_json(in_folder("settings.json"), &doc) || !doc.is_object()) return s;
    if (doc.contains("auto_preview") && doc["auto_preview"].is_boolean())
        s.auto_preview = doc["auto_preview"].get<bool>();
    if (doc.contains("panthera_folder") && doc["panthera_folder"].is_string())
        s.panthera_folder = doc["panthera_folder"].get<std::string>();
    if (doc.contains("close_rests") && doc["close_rests"].is_boolean())
        s.close_rests = doc["close_rests"].get<bool>();
    if (doc.contains("close_rests_beats") && doc["close_rests_beats"].is_number())
        s.close_rests_beats = std::max(0.0, doc["close_rests_beats"].get<double>());
    if (doc.contains("remove_note_velocities") && doc["remove_note_velocities"].is_boolean())
        s.remove_note_velocities = doc["remove_note_velocities"].get<bool>();
    if (doc.contains("remove_volume_changes") && doc["remove_volume_changes"].is_boolean())
        s.remove_volume_changes = doc["remove_volume_changes"].get<bool>();
    if (doc.contains("vocalwriter_own_path") && doc["vocalwriter_own_path"].is_boolean())
        s.vocalwriter_own_path = doc["vocalwriter_own_path"].get<bool>();
    if (doc.contains("close_word_rests") && doc["close_word_rests"].is_boolean())
        s.close_word_rests = doc["close_word_rests"].get<bool>();
    if (doc.contains("plain_held_vowels") && doc["plain_held_vowels"].is_boolean())
        s.plain_held_vowels = doc["plain_held_vowels"].get<bool>();
    if (doc.contains("octave_instruments") && doc["octave_instruments"].is_boolean())
        s.octave_instruments = doc["octave_instruments"].get<bool>();
    if (doc.contains("follow_playback") && doc["follow_playback"].is_boolean())
        s.follow_playback = doc["follow_playback"].get<bool>();
    if (doc.contains("export_format") && doc["export_format"].is_string())
        s.export_format = doc["export_format"].get<std::string>();
    if (doc.contains("soundfonts") && doc["soundfonts"].is_array())
        for (const json &f : doc["soundfonts"]) {
            if (!f.is_object() || !f.contains("path") || !f["path"].is_string()) continue;
            SoundFontFile sf;
            sf.path = f["path"].get<std::string>();
            if (f.contains("slot") && f["slot"].is_number_integer()) sf.slot = f["slot"].get<int>();
            // each in a slot of its own, from 0, its numbers inside an int
            bool taken = sf.slot < 0 || sf.slot > 9999;
            for (const SoundFontFile &o : s.soundfonts) taken = taken || o.slot == sf.slot || o.path == sf.path;
            if (!sf.path.empty() && !taken) s.soundfonts.push_back(sf);
        }
    if (doc.contains("default_soundbank") && doc["default_soundbank"].is_string())
        s.default_soundbank = doc["default_soundbank"].get<std::string>();
    return s;
}

bool save_settings(const Settings &s) {
    if (!make_dirs(settings_dir())) return false;
    json doc;
    doc["auto_preview"] = s.auto_preview;
    if (!s.panthera_folder.empty()) doc["panthera_folder"] = s.panthera_folder;
    doc["close_rests"] = s.close_rests;
    doc["close_rests_beats"] = s.close_rests_beats;
    doc["remove_note_velocities"] = s.remove_note_velocities;
    doc["remove_volume_changes"] = s.remove_volume_changes;
    doc["vocalwriter_own_path"] = s.vocalwriter_own_path;
    doc["close_word_rests"] = s.close_word_rests;
    doc["plain_held_vowels"] = s.plain_held_vowels;
    doc["octave_instruments"] = s.octave_instruments;
    doc["follow_playback"] = s.follow_playback;
    doc["export_format"] = s.export_format;
    json fonts = json::array();
    for (const SoundFontFile &f : s.soundfonts) fonts.push_back({{"path", f.path}, {"slot", f.slot}});
    doc["soundfonts"] = fonts;
    if (!s.default_soundbank.empty()) doc["default_soundbank"] = s.default_soundbank;
    return write_text(in_folder("settings.json"), doc.dump(1) + "\n");
}

bool recovery_write(const Song &song, const std::string &path) {
    try {
        if (!make_dirs(settings_dir())) return false;
        save_project(in_folder(kNotes), song);
        int notes = 0;
        for (const Track &t : song.tracks) notes += int(t.notes.size());
        json mark;
        mark["path"] = path.empty() ? json(nullptr) : json(path);
        mark["when"] = double(std::time(nullptr));
        mark["notes"] = notes;
        return write_text(in_folder(kMark), mark.dump() + "\n");
    } catch (...) {
        return false;
    }
}

std::optional<Recovered> recovery_waiting() {
    json mark;
    if (!read_json(in_folder(kMark), &mark)) return std::nullopt;
    if (!file_exists(in_folder(kNotes))) return std::nullopt;
    Recovered r;
    r.file = in_folder(kNotes);
    if (mark.is_object()) {
        if (mark.contains("path") && mark["path"].is_string()) r.was_path = mark["path"].get<std::string>();
        if (mark.contains("notes") && mark["notes"].is_number()) r.notes = mark["notes"].get<int>();
    }
    return r;
}

void recovery_clear() {
    remove_file(in_folder(kNotes));
    remove_file(in_folder(kMark));
}

}  // namespace svs
