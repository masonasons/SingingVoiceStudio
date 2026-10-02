// project.h -- the song: notes, tracks, saving, reopening and MIDI import.
//
// A project is plain JSON, the same `.vws` file VocalWriter Studio writes, so
// a song moves between the two programs. It holds what the editor knows and
// nothing else -- the notes, the tempo and the voices -- so it stays readable,
// stays small, and does not go stale when an engine changes. The audio is not
// in it; that is what Export WAV is for.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voices/singer.h"

namespace svs {

extern const char *const kProjectSuffix;      // ".vws"
extern const char *const kProjectWildcard;
extern const char *const kMidiWildcard;
//: What a note sings when the file says nothing: "AA", the open vowel of
//: "father" -- something to sing the line on, and the obvious thing to replace.
extern const char *const kDefaultPhoneme;

constexpr int kDefaultPitch = 60;
constexpr double kDefaultBeats = 0.5;
//: semitones below which a bend point is not worth keeping
constexpr double kBendEpsilon = 0.01;
//: the mod wheel moves in whole steps, so half of one is nothing
constexpr double kModEpsilon = 0.5;

using Sig = std::pair<int, int>;
constexpr Sig kDefaultSig{4, 4};

//: [(where in the note 0..1, semitones)]
using Bend = std::vector<std::pair<double, double>>;
//: [(where in the note 0..1, mod wheel 0..127)] -- the same shape as a bend
using Mod = Bend;

struct Note {
    std::vector<std::string> phonemes;
    int pitch = kDefaultPitch;
    double beats = kDefaultBeats;
    std::string word;
    Bend bend;
    //: The mod wheel across the note, attached to it the way the bend is, so
    //: moving or retiming the note takes it along. Empty is the wheel at rest.
    Mod mod;

    Note() = default;
    Note(std::vector<std::string> ph, int p = kDefaultPitch, double b = kDefaultBeats,
         std::string w = "", Bend bd = {}, Mod md = {})
        : phonemes(std::move(ph)), pitch(p), beats(b), word(std::move(w)), bend(std::move(bd)),
          mod(std::move(md)) {}

    std::string text() const;
    bool is_rest() const;
    //: what the list shows: a rest reads as one, not as a per cent sign
    std::string label() const;
    bool operator==(const Note &o) const {
        return phonemes == o.phonemes && pitch == o.pitch && beats == o.beats &&
               word == o.word && bend == o.bend && mod == o.mod;
    }
};

//: (room, wet), whole percentages
using Reverb = std::pair<int, int>;

struct Track {
    std::string name;
    //: which program change this part used to be, for songs written before
    //: voices were chosen out of the bank by name
    int program = 0;
    //: which voice sings this part (see voices/family.h for the numbering);
    //: unset means the program above still decides
    std::optional<int> voice_id;
    int volume = 100;   // per cent
    int pan = 0;        // -100 .. 100
    bool mute = false;
    bool solo = false;
    std::vector<Note> notes;
    //: the part's own voice controls, or none to follow the song's
    std::optional<VoiceStyle> voice;
    std::optional<Reverb> reverb;
    std::optional<double> consonants;
    //: which note was last selected here; not saved
    int cursor = 0;

    bool operator==(const Track &o) const {
        return name == o.name && program == o.program && voice_id == o.voice_id &&
               volume == o.volume && pan == o.pan && mute == o.mute && solo == o.solo &&
               notes == o.notes && voice == o.voice && reverb == o.reverb &&
               consonants == o.consonants;
    }
};

//: Everything a song is, apart from the window's own state.
struct Song {
    double bpm = 120;
    Sig sig = kDefaultSig;
    double consonants = 1.0;          // a fraction
    VoiceStyle voice;                 // the song's voice controls
    Reverb reverb{0, 0};
    bool anticipate = true;
    std::vector<Track> tracks;
    bool operator==(const Song &o) const {
        return bpm == o.bpm && sig == o.sig && consonants == o.consonants &&
               voice == o.voice && reverb == o.reverb && anticipate == o.anticipate &&
               tracks == o.tracks;
    }
    bool operator!=(const Song &o) const { return !(*this == o); }
};

// -- what is shown --------------------------------------------------------

std::string file_name(const std::string &text, const std::string &fallback = "track");
std::string pan_text(int pan);
double bar_beats(Sig sig);
//: (bar, beat), both counting from 1, the beat in the signature's own units
std::pair<int, double> bar_and_beat(double position, Sig sig);
std::string format_sig(Sig sig);
Sig parse_sig(const std::string &text, Sig fallback = kDefaultSig);
std::string describe_bend(const Bend &bend);
//: the same for the mod wheel: blank, "64", "0 to 127", "127 and back"
std::string describe_mod(const Mod &mod);
std::pair<std::optional<double>, std::optional<double>> bend_ends(const Bend &bend);
Bend with_ends(const Bend &bend, std::optional<double> start, std::optional<double> end);
//: A MIDI number, or a name like C4 / F#3 / Bb5. Throws std::invalid_argument.
int parse_pitch(const std::string &text);
std::string pitch_name(int midi);
//: %g, as Python and C print it
std::string fmt_g(double v);

// -- which tracks sing ---------------------------------------------------

//: Solo wins; otherwise everything not muted. Muted and soloed stays silent.
std::vector<const Track *> audible(const std::vector<Track> &tracks);
//: which voice a part sings with, resolving an old program number
int track_voice(const Track &track, const std::map<int, int> &program_map);
//: every note's bend, gathered up, as (beat, semitones, slides into the next)
struct BendPoint {
    double beat;
    double semitones;
    bool slides;
};
std::vector<BendPoint> timeline(const std::vector<Note> &notes);
//: every note's mod wheel gathered up the same way (`semitones` holds the wheel)
std::vector<BendPoint> mod_timeline(const std::vector<Note> &notes);

//: (track, where it goes) for each track with notes, no two files alike
std::vector<std::pair<const Track *, std::string>> export_jobs(
    const std::vector<Track> &tracks, const std::string &folder, const std::string &base);

// -- files -----------------------------------------------------------------

//: Throws std::runtime_error with a readable message.
void save_project(const std::string &path, const Song &song);
Song load_project(const std::string &path);

//: the clipboard: notes as text, so they survive between two copies
std::string notes_to_clipboard(const std::vector<Note> &notes);
std::vector<Note> notes_from_clipboard(const std::string &text);

//: the document as JSON text, for comparing two states (undo, dirty)
std::string song_fingerprint(const Song &song);

// -- MIDI --------------------------------------------------------------------

//: The tracks worth importing, as [(name, note count)].
std::vector<std::pair<std::string, int>> midi_tracks(const std::string &path);

struct ImportedPart {
    std::string name;
    std::vector<Note> notes;
    //: words still to be looked up: (word, [note index...])
    std::vector<std::pair<std::string, std::vector<int>>> pending;
};
struct Imported {
    double bpm = 120;
    Sig sig = kDefaultSig;
    std::vector<ImportedPart> parts;
};
Imported import_midi(const std::string &path, const std::vector<std::string> &names);

//: Put looked-up pronunciations on the notes an import left blank. Returns
//: how many words were pronounced. A word the dictionary does not know leaves
//: its notes singing kDefaultPhoneme.
int pronounce(std::vector<ImportedPart> &parts,
              const std::map<std::string, std::vector<std::string>> &found);

}  // namespace svs
