#include "core/settings.h"

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

void remove_file(const std::string &path) {
#ifdef _WIN32
    _wremove(widen(path).c_str());
#else
    std::remove(path.c_str());
#endif
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
    return s;
}

bool save_settings(const Settings &s) {
    if (!make_dirs(settings_dir())) return false;
    json doc;
    doc["auto_preview"] = s.auto_preview;
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
