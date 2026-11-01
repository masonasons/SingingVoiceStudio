// project.h -- the song: notes, tracks, saving, reopening and MIDI import.
//
// A project is plain JSON, the same `.vws` file VocalWriter Studio writes, so
// a song moves between the two programs. It holds what the editor knows and
// nothing else -- the notes, the tempo and the voices -- so it stays readable,
// stays small, and does not go stale when an engine changes. The audio is not
// in it; that is what Export WAV is for.
#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "voices/singer.h"

namespace svs {

extern const char *const kProjectSuffix;      // ".vws"
extern const char *const kProjectWildcard;
extern const char *const kImportWildcard;  // MIDI files and VocalWriter songs
extern const char *const kMidiWildcard;
//: whether a file is a MIDI file, by its name
bool is_midi_file(const std::string &path);
//: What a note sings when the file says nothing: "AA", the open vowel of
//: "father" -- something to sing the line on, and the obvious thing to replace.
extern const char *const kDefaultPhoneme;

constexpr int kDefaultPitch = 60;
//: how hard a part's notes are sung when it says nothing, 1-127: VocalWriter's,
//: whose notes window starts its velocity slider at 90 per cent
constexpr int kDefaultVelocity = 114;
//: the default before that: what a song file without a velocity means
constexpr int kPlainVelocity = 64;
//: the loudest a track may be turned up, in per cent
constexpr int kMaxVolume = 200;
//: the slowest and fastest tempo, beats a minute; VocalWriter's were 10 and 250
constexpr double kMinBpm = 5, kMaxBpm = 500;
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

//: One of VocalWriter's controls a note may carry: its name in the file, what
//: the window calls it, and its range -- 0-127 for most, detune 14-bit about
//: zero, chorus as far either way as is any use.
struct NoteControl {
    const char *key, *label;
    int lo, hi;
    const char *hint;
};
extern const NoteControl kNoteControls[];
extern const size_t kNoteControlCount;
const NoteControl *note_control(const std::string &key);
//: How far a bend typed in may go either way, in semitones: two octaves.
const double kMaxBend = 24.0;

struct Note {
    std::vector<std::string> phonemes;
    int pitch = kDefaultPitch;
    double beats = kDefaultBeats;
    std::string word;
    Bend bend;
    //: The mod wheel across the note, attached to it the way the bend is, so
    //: moving or retiming the note takes it along. Empty is the wheel at rest.
    Mod mod;
    //: How hard the note is sung, 1-127, as VocalWriter keeps it per note;
    //: 0 leaves it to the track.
    int velocity = 0;
    //: VocalWriter's own controls, as its songs have them (see NoteControls)
    NoteControls controls;
    //: How long an instrument sounds the note, in beats, if not `beats`,
    //: which is always the time to the next note: a chord is its notes in a
    //: row, all but the last at 0 beats with a hold. Voices ignore it.
    double hold = 0;
    //: A controller rather than a note: it takes no time and nobody sings
    //: it, it changes what comes after it. `tempo` is the beats a minute
    //: from here on, for the whole song whichever track it is on; 0 changes
    //: nothing. `voice_id` is the voice this track sings with from here on,
    //: any engine's, its name kept as a track keeps it (Track::voice_name).
    //: `pan` is the track's pan from here on, -100 to 100, whatever sings it.
    //: `sig` is the time signature from here on, for the whole song as the
    //: tempo is, a new bar beginning where it stands.
    bool controller = false;
    double tempo = 0;
    std::optional<int> voice_id;
    std::optional<std::string> voice_name;
    std::optional<int> pan;
    std::optional<Sig> sig;

    Note() = default;
    Note(std::vector<std::string> ph, int p = kDefaultPitch, double b = kDefaultBeats,
         std::string w = "", Bend bd = {}, Mod md = {})
        : phonemes(std::move(ph)), pitch(p), beats(b), word(std::move(w)), bend(std::move(bd)),
          mod(std::move(md)) {}

    static Note tempo_change(double bpm);
    std::string text() const;
    bool is_rest() const;
    //: its hold, or its beats
    double sounds() const { return hold > 0 ? hold : beats; }
    //: what the list shows: a rest reads as one, not as a per cent sign
    std::string label() const;
    bool operator==(const Note &o) const {
        return phonemes == o.phonemes && pitch == o.pitch && beats == o.beats &&
               word == o.word && bend == o.bend && mod == o.mod && velocity == o.velocity &&
               controls == o.controls && hold == o.hold && controller == o.controller && tempo == o.tempo &&
               voice_id == o.voice_id && voice_name == o.voice_name && pan == o.pan && sig == o.sig;
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
    //: the voice's name too, for one outside VocalWriter's bank: numbers are
    //: fixed per engine, but the name is what a person chose, and it wins if
    //: the two ever disagree (see resolve_voice_names)
    std::optional<std::string> voice_name;
    //: per cent, up to kMaxVolume: VocalWriter's songs go past 100 now and then
    int volume = 100;
    int pan = 0;        // -100 .. 100
    //: how hard the notes are sung: louder for most engines, and for
    //: VocalWriter's the vowels against the consonants
    int velocity = kDefaultVelocity;
    bool mute = false;
    bool solo = false;
    std::vector<Note> notes;
    //: the part's own voice controls, or none to follow the song's
    std::optional<VoiceStyle> voice;
    std::optional<Reverb> reverb;
    std::optional<double> consonants;
    //: A backing track: the sound file it plays under the voices, at its
    //: volume and pan, instead of singing. Empty for a part that sings.
    std::string audio;
    //: where in the song the file begins, in beats, so it keeps its place
    //: against the bars: later than the first beat when above 0, and below
    //: 0 its opening is left out
    double audio_beat = 0;
    //: which note was last selected here; not saved
    int cursor = 0;

    bool is_backing() const { return !audio.empty(); }
    bool operator==(const Track &o) const {
        return name == o.name && program == o.program && voice_id == o.voice_id &&
               voice_name == o.voice_name &&
               volume == o.volume && pan == o.pan && velocity == o.velocity &&
               mute == o.mute && solo == o.solo &&
               notes == o.notes && voice == o.voice && reverb == o.reverb &&
               consonants == o.consonants && audio == o.audio && audio_beat == o.audio_beat;
    }
};

//: From `beat` on, counted from the song's start, the song goes at `bpm`.
struct TempoChange {
    double beat = 0;
    double bpm = 120;
    bool operator==(const TempoChange &o) const { return beat == o.beat && bpm == o.bpm; }
};

//: Where the beats fall in time: a tempo from the start, then its changes.
struct Clock {
    double bpm = 120;
    std::vector<TempoChange> changes;   // in order, all after the first beat
    double seconds(double beat) const;
    //: the beat `seconds` into the song: seconds() turned round
    double beat_at(double seconds) const;
    double bpm_at(double beat) const;
};

//: The changes in order, one to a beat, none at the start or out of range.
std::vector<TempoChange> clean_tempo_changes(std::vector<TempoChange> changes);

//: From `beat` on, counted from the song's start, the bars are of `sig`, the
//: first of them beginning there.
struct SigChange {
    double beat = 0;
    Sig sig = kDefaultSig;
    bool operator==(const SigChange &o) const { return beat == o.beat && sig == o.sig; }
};

//: Where the bars fall: a time signature from the start, then its changes,
//: each beginning a new bar; a bar one cuts short is still a bar.
struct Meter {
    Sig sig = kDefaultSig;
    std::vector<SigChange> changes;     // in order, all after the first beat
    //: (bar, beat) as bar_and_beat has them; before the first beat, bar 0 and below
    std::pair<int, double> bar_and_beat(double position) const;
    //: the beats from the song's start to `beat` of `bar`: bar_and_beat turned round
    double position(int bar, double beat = 1) const;
    Sig sig_at(double position) const;
};

//: The changes in order, one to a beat, none at the start or not a signature.
std::vector<SigChange> clean_sig_changes(std::vector<SigChange> changes);

//: Everything a song is, apart from the window's own state.
struct Song {
    //: the tempo it starts at; the tempo changes are controllers on its tracks
    double bpm = 120;
    //: where the song ends, in beats, as a VocalWriter song keeps it; 0, or
    //: anywhere before the last note's end, is a beat after the last note
    double end = 0;
    Sig sig = kDefaultSig;
    double consonants = 1.0;          // a fraction
    VoiceStyle voice;                 // the song's voice controls
    Reverb reverb{0, 0};
    bool anticipate = true;
    std::vector<Track> tracks;
    bool operator==(const Song &o) const {
        return bpm == o.bpm && end == o.end && sig == o.sig && consonants == o.consonants &&
               voice == o.voice && reverb == o.reverb && anticipate == o.anticipate &&
               tracks == o.tracks;
    }
    bool operator!=(const Song &o) const { return !(*this == o); }
    //: the tempo and every track's tempo changes; where two tracks change it
    //: at one beat, the one higher up wins, and one at the very start is the
    //: tempo the song starts at
    Clock clock() const;
    //: the time signature and every track's changes of it, as clock() has the tempo
    Meter meter() const;
};

//: A song begun afresh, a new one or one imported from MIDI: with the
//: portamento and reverb VocalWriter's own songs have. A project that says
//: nothing of them still means none, as VocalWriter Studio wrote it.
constexpr int kVocalWriterPortamento = 40;
constexpr Reverb kVocalWriterReverb{40, 24};
Song fresh_song();

//: A track of rests holding the tempo and time signature changes, each at
//: its beat: how an imported song brings its changes in, wherever they fall.
Track tempo_track(const std::vector<TempoChange> &changes, const std::vector<SigChange> &sigs = {});
//: what the Tempo track holds, as said: "2 tempo changes and 1 time
//: signature change"; empty for none
std::string tempo_map_said(size_t tempos, size_t sigs);
//: whether a track has a note anybody sings
bool sings(const Track &track);
//: the track's changes of voice, as (beat, voice), in order
std::vector<std::pair<double, int>> voice_changes(const Track &track);
//: the song at its starting tempo throughout, every tempo change taken out
void steady_tempo(Song &song);
//: steady_tempo, every time signature change taken out too, and the tracks
//: that held nothing but those changes taken out with them, as an imported
//: song's Tempo track
void drop_tempo_map(Song &song);

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
//: The name a part the file leaves unnamed is given after its program,
//: 0-127, and whether it is the drums: the instrument or drum kit the default
//: soundbank plays it with. Empty is the GM Soundbank's name for the program
//: (gm_program_name), and "Drums" for the drums.
using ProgramNames = std::function<std::string(int, bool)>;
std::vector<std::pair<std::string, int>> midi_tracks(const std::string &path, const ProgramNames &program_names = {});

//: A track of a MIDI file as the import offers it.
struct MidiPart {
    std::string name;
    int notes = 0;
    bool words = false;
    bool drums = false;
    int program = 0;
    //: the channel it plays on, 0-15
    int channel = -1;
    //: the GM Soundbank voice it would be played with
    int voice() const;
};
//: `lyrics_on`: the channel a type 0 file's lyrics go to (lyric_choice);
//: -1 for the one most of them fall on.
std::vector<MidiPart> midi_parts(const std::string &path, int lyrics_on = -1,
                                 const ProgramNames &program_names = {});
//: The channels a type 0 file's lyrics could be sung on, and the likeliest;
//: none when there is nothing to choose (one channel, or no lyrics).
struct LyricChoice {
    std::vector<int> channels;
    int likely = -1;
};
LyricChoice lyric_choice(const std::string &path);
//: The parts an import plays on the GM Soundbank: every one without words,
//: so only a part with words is sung, and none in a file without them.
std::vector<std::string> parts_to_play(const std::vector<MidiPart> &parts);

struct ImportedPart {
    std::string name;
    std::vector<Note> notes;
    //: words still to be looked up: (word, [note index...])
    std::vector<std::pair<std::string, std::vector<int>>> pending;
    //: played by the GM Soundbank rather than sung, with this instrument
    bool played = false;
    std::optional<int> voice_id;
    //: the velocity most of the notes have; a note keeps its own if it differs
    int velocity = kDefaultVelocity;
    //: at the first note; a change after it is a controller
    int pan = 0;
    //: per cent, CC 7 by CC 11 (see midi_volume)
    int volume = 100;
};
//: The part as a track of the song.
Track imported_track(const ImportedPart &part);
struct Imported {
    double bpm = 120;
    //: the tempo changes after the start, in the file's beats
    std::vector<TempoChange> tempo_changes;
    Sig sig = kDefaultSig;
    //: and the time signature's
    std::vector<SigChange> sig_changes;
    std::vector<ImportedPart> parts;
    //: how many short rests were closed (close_short_rests)
    int closed_rests = 0;
};
//: `close_rests`: the longest rest between two notes to close, in beats; 0
//: keeps every rest. The tracks named in `played` are played by the GM
//: Soundbank, chords and all; the rest are sung, and `remove_velocities`
//: leaves their notes the track's velocity, none of their own. A sung
//: part's program changes are changes of voice, to the one `programs` gives
//: each number, as vocalwriter_program_voices does (-1 for none).
//: `close_word_rests`: a sung part's rests in the middle of a word are
//: closed whatever `close_rests` is (close_short_rests). `octaves`: the
//: played parts of programs 29 - 32, 34, 36, 37, 53, 80, 86, 96, 100 and 101
//: are moved an octave up, and of 98 and 102 an octave down, as the
//: soundbank's instruments for them sound an octave off. `remove_volumes`:
//: the sung parts keep the track's volume, none of its changes on the notes.
Imported import_midi(const std::string &path, const std::vector<std::string> &names, double close_rests = 0,
                     const std::vector<std::string> &played = {}, bool remove_velocities = false,
                     int lyrics_on = -1, const std::vector<int> &programs = {}, bool close_word_rests = false,
                     bool octaves = false, bool remove_volumes = false, const ProgramNames &program_names = {});

// -- tracks an instrument plays ---------------------------------------------

//: A note as an instrument plays it, in beats from the song's start.
struct PlayedNote {
    double start = 0, length = 0;
    int pitch = 60, velocity = 0;
};
//: A change from `beat` on: a note control ("volume", "sustain",
//: "bend_range", "detune"), "" for the bend in semitones, "voice", or "pan"
//: (-100 to 100).
struct PlayedChange {
    double beat = 0;
    std::string name;
    double value = 0;
};
//: The notes as rows in order of start: a chord's notes but the last at 0
//: beats, each held as long as it sounds, a rest wherever nothing sounds for
//: `rest_gap` beats. The controls and the bend go on the rows they fall in,
//: a change of instrument or pan is a controller.
std::vector<Note> lay_out_played(std::vector<PlayedNote> notes, std::vector<PlayedChange> changes,
                                 double rest_gap = 0.25);
//: The commonest velocity, for the track; notes keep theirs only if it differs.
int common_velocity(std::vector<Note> &notes);

//: The rests between two notes no longer than `longest` beats taken out, the
//: note before each one held through it, its bend, wheel and controls
//: stretched with it: a line the file has detached, sung legato. With
//: `in_words`, every rest in the middle of a word too, however long ("al-",
//: rest, "right"). Returns how many were closed.
int close_short_rests(std::vector<Note> &notes, double longest, bool in_words = false);

//: A word's phonemes, the dictionary's syllables marked, on `count` notes, as
//: Add word lays it out: as the pieces typed with hyphens divide it when
//: there are no more of them than notes ("did-", "n't"), else a syllable to
//: a note; with notes over, each is split at its vowel, the first ones
//: taking the extra ("d EY", "EY", "z IY"), `plain` as on import.
std::vector<std::vector<std::string>> word_on_notes(const std::vector<std::string> &phonemes,
                                                    const std::vector<std::string> &pieces, int count,
                                                    bool plain = false);

//: A word of a text for Add text: its letters, the pieces its hyphens make
//: ("did-", "n't"), its phonemes with the syllables marked (empty if not
//: found), and how many notes it goes on.
struct TextWord {
    std::string word;
    std::vector<std::string> pieces;
    std::vector<std::string> phonemes;
    int notes = 1;
    //: what it is looked up by: the pieces as typed, "did-n't"
    std::string key() const;
};

//: Whether the character at `i` of `s` belongs to a typed word: a letter, a
//: figure, an apostrophe, or a point or comma between figures ("3.5", "1,000").
bool in_word(const std::string &s, size_t i);

//: The words a word with figures in it is said as: "39" and "'39" as
//: "thirty", "nine", "1984" as a year, "2nd" as "second", "60s" as
//: "sixties", "3.5" as "three", "point", "five", a leading 0 figure by figure
//: ("007"); its letters kept as words ("4ever"). Empty if it has no figure.
std::vector<std::string> number_words(const std::string &word);

//: The words of `text`, divided where it has white space; a stretch with no
//: letter in it is no word. A tag in square brackets, "[Verse 1]", is left
//: out; words in round ones, a backing part's, are kept.
std::vector<TextWord> text_words(const std::string &text);

//: How many notes a word of a text takes by itself: one a piece if it was
//: typed in pieces, else one a syllable, or one if it was not found.
int text_word_notes(const TextWord &w);

//: The words of a text looked up: each on as many notes as it takes
//: (text_word_notes), and "the" before a vowel "thee".
void text_looked_up(std::vector<TextWord> &words);

//: How many notes each word goes on with `count` in all: its own, the first
//: words one more each where there are notes over; with too few, the words
//: in order until they run out, the last on what is left, the rest on none.
std::vector<int> text_shares(const std::vector<TextWord> &words, int count);

//: The phonemes and the word of each note the words go on, `shares` of
//: them each (text_shares), laid out as word_on_notes does, `plain` as on
//: import; a word with no phonemes sings kDefaultPhoneme.
std::vector<std::pair<std::vector<std::string>, std::string>> text_on_notes(const std::vector<TextWord> &words,
                                                                            const std::vector<int> &shares,
                                                                            bool plain = false);

//: Put looked-up pronunciations on the notes an import left blank. Returns
//: how many words were pronounced. A word the dictionary does not know leaves
//: its notes singing kDefaultPhoneme. `plain_held`: a vowel held over several
//: notes sings its first sound until the last of them, as "day" over three
//: is "d EH", "EH", "EY" rather than "d EY", "EY", "EY".
int pronounce(std::vector<ImportedPart> &parts,
              const std::map<std::string, std::vector<std::string>> &found, bool plain_held = false);

}  // namespace svs
