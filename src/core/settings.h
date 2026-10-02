// settings.h -- what belongs to the program rather than to a song, and the
// copy of the song kept in case the program does not get to say goodbye.
//
// A song carries its own tempo and the rest. What is here is how you like to
// work, which follows you from song to song, in one small JSON file beside the
// system's other per-user settings. If it is missing or nonsense the defaults
// stand and nothing is said: a preference is not worth an error message.
#pragma once

#include <optional>
#include <string>

#include "core/project.h"

namespace svs {

struct Settings {
    //: preview a note as it is nudged, so pitch and length can be heard
    bool auto_preview = false;
};

Settings load_settings();
bool save_settings(const Settings &s);

// -- recovery -------------------------------------------------------------
//
// The synthesisers run in this process. That is what makes them fast, and it
// means a fault in one ends the program -- so the song has to be somewhere
// other than in memory. A copy is written whenever the song changes and a
// moment has passed, and deleted on the way out; anything left behind next
// time is offered back.

struct Recovered {
    std::string file;          // the kept song
    std::string was_path;      // the project it came from, if it had one
    int notes = 0;
};

bool recovery_write(const Song &song, const std::string &path);
std::optional<Recovered> recovery_waiting();
void recovery_clear();

}  // namespace svs
