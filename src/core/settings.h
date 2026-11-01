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
#include <vector>

#include "core/project.h"

namespace svs {

//: A SoundFont loaded in the Soundbank manager, and the slot its voices are
//: numbered from: 101000 + 1000 * slot (voices/soundfont.h).
struct SoundFontFile {
    std::string path;
    int slot = 0;
    bool operator==(const SoundFontFile &o) const { return path == o.path && slot == o.slot; }
};

struct Settings {
    //: preview a note as it is nudged, so pitch and length can be heard
    bool auto_preview = false;
    //: where Panthera is, for the MacinTalk voices, when it is not where its
    //: NVDA add-on puts it; empty to look only there (set_macintalk_folder)
    std::string panthera_folder;
    //: close the short rests between notes when a MIDI file is imported,
    //: those up to `close_rests_beats` long
    bool close_rests = false;
    double close_rests_beats = 0.25;
    //: and every rest in the middle of a word, however long
    bool close_word_rests = false;
    //: remove the notes' own velocities from the sung tracks of an imported
    //: MIDI file, so they take the track's
    bool remove_note_velocities = false;
    //: and the volume changes on their notes, leaving the track's volume
    bool remove_volume_changes = false;
    //: sing VocalWriter's voices through VocalWriter 2.0's own play path
    //: rather than phrase by phrase (set_vocalwriter_own_path)
    bool vocalwriter_own_path = false;
    //: a vowel an imported MIDI file holds over several notes sings its
    //: first sound until the last of them (pronounce, `plain_held`)
    bool plain_held_vowels = false;
    //: an imported MIDI file's played parts on the instruments the soundbank
    //: has an octave off are transposed an octave (import_midi, `octaves`)
    bool octave_instruments = false;
    //: while the song plays, every track's cursor moves to the note sung
    bool follow_playback = false;
    //: what Export audio wrote last: ".wav", ".flac" or ".ogg"
    std::string export_format = ".wav";
    //: the SoundFonts loaded in the Soundbank manager
    std::vector<SoundFontFile> soundfonts;
    //: what plays the parts of an imported MIDI file or VocalWriter song that
    //: are not sung: the path of one of `soundfonts`, or empty for the GM
    //: Soundbank
    std::string default_soundbank;
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
