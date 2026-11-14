// studio.h -- the window: build a song note by note.
//
// A note carries a *group* of phonemes and one pitch, because that is how
// singing works: a syllable sits on a note, not one phoneme per note. "Add
// word" looks the word up in VocalWriter's own dictionary and can spread it
// over as many notes as it has syllables.
//
// Everything is on the menus with a shortcut on each item, so a screen reader
// announces the key along with the name; editing happens in dialogs with
// ordinary labelled fields rather than inside the list; and nothing is
// conveyed by colour or position alone.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/wx.h>

class wxProgressDialog;

#include "app/widgets.h"
#include "app/worker.h"
#include "audio/mixer.h"
#include "audio/player.h"
#include "core/phonology.h"
#include "core/project.h"
#include "core/settings.h"
#include "voices/registry.h"

namespace svs {

//: The name of a length, if it has one: "dotted quarter", "triplet eighth".
std::string note_value(double beats);
std::string note_name(double beats);
std::string beat_count(double n);
std::string length_text(double beats);
std::string spoken_length(double beats, Sig sig);
//: a sixteenth longer or shorter, or nothing if that is too short
std::optional<double> stepped_length(double beats, bool up);

constexpr int kSemitone = 1;
constexpr int kOctave = 12;
//: what a second copy of the program marks the file it hands the copy
//: running with (WM_COPYDATA), "SVSO"
constexpr unsigned long kHandedFile = 0x5356534F;

class Studio : public wxFrame {
public:
    explicit Studio(const std::string &path = "");
    ~Studio() override;

    // -- what the dialogs ask of the window ----------------------------------
    void say(const wxString &text);
    void announce_note(const wxString &text, ReportList *control, int row);
    std::vector<PaletteRow> singable() const;
    Sig signature() const { return song_.sig; }
    const std::vector<VoiceListing> &voice_list() const { return voices_; }
    //: a voice as the window names it, "VocalWriter Robert" or "Acoustic Grand Piano"
    std::string voice_name(int id) const;
    int track_voice_of(const Track &t) const { return track_voice(t, program_map_); }
    int consonant_pct() const { return int(std::lround(song_.consonants * 100)); }
    const VoiceStyle &song_voice() const { return song_.voice; }
    Reverb song_reverb() const { return song_.reverb; }
    //: the voice of the track being worked on where the cursor is, for
    //: previews: a controller before it may have changed it
    int current_voice() const {
        int i = list_ ? list_->FirstSelected() : -1;
        return voice_at(i < 0 ? int(track().notes.size()) : i + 1);
    }
    int voice_at(int upto) const;
    std::string label_of(const Note &n) const;
    //: a vowel held over several notes sings its first sound until the last
    bool plain_held() const { return settings_.plain_held_vowels; }
    //: VocalWriter's unfinished voices are offered in the voice boxes too
    bool show_unfinished() const { return settings_.show_unfinished_voices; }
    //: render notes on the current track's voice and play them
    void preview(const std::vector<Note> &notes, std::function<void()> done);
    //: Render the current track alone, `put` on its rows `spots`, from row
    //: `from` on, and play it if `wanted` still says so; `done` is told
    //: whether it plays.
    void preview_track(const std::vector<int> &spots, const std::vector<Note> &put, int from,
                       std::function<bool()> wanted, std::function<void(bool)> done);
    bool playing() { return player_.playing(); }
    void stop_playing() { stop_audio(); }
    //: `marked`: with the dictionary's syllables marked (Lexicon::phonemes)
    void lookup(const std::vector<std::string> &words,
                std::function<void(std::map<std::string, std::vector<std::string>>)> done,
                bool marked = false);
    void play_audio(const Mixed &res, bool quiet = false);

private:
    // -- the song -------------------------------------------------------------
    std::vector<Note> &notes() { return song_.tracks[size_t(current_)].notes; }
    const Track &track() const { return song_.tracks[size_t(current_)]; }
    Track &track() { return song_.tracks[size_t(current_)]; }

    // -- interface ---------------------------------------------------------
    void build();
    void build_menu();
    wxMenu *note_menu();
    wxMenu *track_menu();
    void on_playback_shortcut(wxKeyEvent &evt);
    void on_keys(wxCommandEvent &);

    // -- engine answers ---------------------------------------------------
    void ready(const std::vector<std::string> &lines);
    void set_voices(std::vector<VoiceListing> v);
    void set_program_map(const std::vector<int> &picks);

    // -- tracks -----------------------------------------------------------
    std::string track_state(const Track &t) const;
    void refresh_track(int i);
    void sync_tracks(int select);
    void on_track_chosen();
    void on_track_key(wxKeyEvent &evt);
    int track_at() const;
    void toggle_track(bool solo);
    std::string track_name() const;
    void on_track_new(wxCommandEvent &);
    void on_track_backing(wxCommandEvent &);
    //: what a track sounds like: its voice, or the file a backing track plays
    std::string track_sound(const Track &t) const;
    //: false, said, when the track being worked on is a backing track
    bool takes_notes();
    void on_track_edit(wxCommandEvent &);
    void on_track_remove(wxCommandEvent &);
    void move_tracks(int delta);
    void select_all_tracks();
    void transpose_tracks(int by);
    void nudge_tracks(int volume, int pan);
    void on_panes(wxCommandEvent &);
    void on_app_activate(wxActivateEvent &evt);
    void come_back();
    bool returning();
    void give_back_focus();
    void to_tracks();
    void show_title();

    // -- notes ------------------------------------------------------------
    int selection() const;
    std::vector<int> selected() const;
    void select_only(const std::vector<int> &rows);
    double position(int index);
    Sig sig_at(int index);
    void on_goto_bar(wxCommandEvent &);
    void on_bar_rest(wxCommandEvent &);
    std::vector<std::string> bars();
    void sync(int select = 0);
    void on_key(wxKeyEvent &evt);
    //: `voice`: the voice in force at the row, if the caller knows it
    void refresh_row(int i, int voice = -1);
    //: whether an instrument plays the current track's row `i`
    bool played_at(int i) const { return is_instrument(voice_at(i)); }
    void refresh_bars();
    void nudge_pitch(int by);
    void nudge_length(bool up);
    void scale_length(double factor);
    void sync_lengths(const std::vector<int> &rows);
    const Note *last();
    void on_select_all(wxCommandEvent &);
    void move_notes(int delta);
    bool to_clipboard(const std::string &text);
    std::string from_clipboard();
    void on_copy(wxCommandEvent &);
    void on_cut(wxCommandEvent &);
    void on_paste(wxCommandEvent &);
    int insertion_point();
    const Note *insertion_neighbour();
    void on_add_word(wxCommandEvent &);
    void on_add_word_existing(wxCommandEvent &);
    void on_add_text(wxCommandEvent &);
    void on_add_note(wxCommandEvent &);
    void on_add_rest(wxCommandEvent &);
    void on_note_to_rest(wxCommandEvent &);
    void on_add_controller(wxCommandEvent &);
    std::string controller_said(int i);
    std::vector<int> selected_notes();
    const Note *note_before(int at);
    const Note *note_after(int at);
    void on_edit(wxCommandEvent &);
    void on_remove(wxCommandEvent &);

    // -- audio ------------------------------------------------------------
    RenderRequest request(const std::vector<Note> *only = nullptr, double start = 0.0,
                          const std::vector<const Track *> *parts = nullptr);
    RenderRequest playback(double start);
    void on_metronome(wxCommandEvent &);
    void on_hear(wxCommandEvent &);
    void heard(const Mixed &res);
    double start_beats();
    void on_play_stop(wxCommandEvent &);
    void on_play(wxCommandEvent &);
    void start_playing(double start, bool quiet = false);
    void played(const Mixed &res, bool quiet);
    void stop_audio();
    void drop_playback();
    void on_stop(wxCommandEvent &);
    std::string song_name() const;
    void on_export(wxCommandEvent &);
    void export_progress(double done);
    void exported(const RenderResult &res, const std::string &path);
    void on_export_tracks(wxCommandEvent &);
    void on_export_midi(wxCommandEvent &);
    void export_track(std::shared_ptr<std::vector<std::pair<Track, std::string>>> jobs, size_t i,
                      int written);
    std::shared_ptr<std::atomic<bool>> open_export_window(const std::string &title, const std::string &message);
    void close_export_window(bool full);
    void mix(const RenderRequest &req, std::function<void(Mixed)> done);
    void render(const RenderRequest &req, const std::string &out, std::function<bool(double)> progress,
                std::function<void(RenderResult)> done);
    void on_auto_preview(wxCommandEvent &);
    void on_panthera(wxCommandEvent &);
    void on_soundbanks(wxCommandEvent &);
    int soundbank_slot();
    std::string soundbank_name(int slot) const;
    void on_close_rests(wxCommandEvent &);
    double rests_to_close() const;
    void on_remove_velocities(wxCommandEvent &);
    void on_remove_volumes(wxCommandEvent &);
    void on_own_path(wxCommandEvent &);
    void on_plain_held(wxCommandEvent &);
    void on_octave_instruments(wxCommandEvent &);
    void on_show_unfinished(wxCommandEvent &);
    void on_follow(wxCommandEvent &);
    void follow();
    void preview_note(int i);
    void preview_now(int i);

    // -- undo -------------------------------------------------------------
    struct Snapshot {
        Song song;
        int current;
        std::vector<int> selected;
    };
    struct Entry {
        Snapshot before, after;
        std::string label;
    };
    Snapshot snapshot();
    //: Wraps an edit: what the song was before and after, and what it is
    //: called on the Edit menu. Nothing is recorded if nothing changed.
    template <class F>
    void undoable(const char *label, F &&edit) {
        Snapshot before = snapshot();
        edit();
        Snapshot after = snapshot();
        bool changed = record(before, after, label);
        bool now_dirty = song_fingerprint(after.song) != saved_;
        if (changed || dirty_ != now_dirty) history_status();
    }
    bool record(const Snapshot &before, const Snapshot &after, const std::string &label);
    void history_status();
    void on_update_history(wxUpdateUIEvent &evt);
    void relabel_history();
    void restore_history(bool redo);
    void reset_history(bool saved_now);

    // -- projects ---------------------------------------------------------
    void touch(bool dirty = true);
    void keep_a_copy();
    void write_copy();
    void offer_recovery();
    bool may_discard(const wxString &what);
    void take(const Song &song, const std::string &path);
    void on_new(wxCommandEvent &);
    void on_open(wxCommandEvent &);
public:
    void open_project(const std::string &path);
    //: a file opened from Explorer or the Finder while the program runs
    void open_from_outside(const std::string &path);
#ifdef __WXMSW__
    WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wparam, WXLPARAM lparam) override;
#endif
private:
    bool dialog_open() const;
    void on_save(wxCommandEvent &);
    void on_save_as(wxCommandEvent &);
    void write_project(const std::string &path);
    void on_song_settings(wxCommandEvent &);
    void on_import(wxCommandEvent &);
    void on_append(wxCommandEvent &);
    void take_midi(const std::string &path, bool append);
    void import_vocalwriter(const std::string &path);
    void open_sound(const std::string &path);
    void cannot_load(const std::string &path, const std::string &why);
    void imported_words(std::vector<ImportedPart> parts,
                        const std::map<std::string, std::vector<std::string>> &found, size_t pending);
    void on_close(wxCloseEvent &evt);

    Song song_;
    int current_ = 0;
    bool switching_ = false;
    std::vector<VoiceListing> voices_;
    //: what the Bar column says now, row by row, so only changes are written
    std::vector<std::string> bars_shown_;
    std::vector<std::string> status_lines_;   // what each engine said at the start
    std::map<int, int> program_map_;
    bool programs_known_ = false;
    std::string vw_waiting_;   // a VocalWriter song to import once they are
    std::string midi_waiting_;   // a MIDI file to import once the voices are known
    Player player_;
    bool rendering_ = false;
    bool discard_render_ = false;   // a new song came in while it rendered
    std::string path_;
    //: a file opened from outside while a dialog was open, waiting for it to close
    std::string outside_waiting_;
    bool dirty_ = false;
    Settings settings_;
    wxTimer preview_timer_, recovery_timer_, follow_timer_, export_timer_;
    //: an export's progress window, while there is one, and its Cancel
    wxProgressDialog *exporting_ = nullptr;
    std::shared_ptr<std::atomic<bool>> export_cancel_;
    int export_shown_ = 0;       // thousandths
    //: the name of the file the song was imported from, for a song not yet saved
    std::string import_name_;
    //: what is playing, for the cursors to follow: the song from a beat, on its clock
    bool following_ = false;
    double play_from_ = 0;
    double play_lead_ = 0;      // seconds the sound began before play_from_
    //: the song playing, without the metronome, which is put in or taken out
    //: of it as Ctrl+M asks (ticked); empty when it is not the song
    Mixed play_mix_;
    std::shared_ptr<const std::vector<float>> ticked(const Mixed &res) const;
    Clock play_clock_;
    int preview_row_ = -1;
    std::unique_ptr<Worker> worker_;
    std::shared_ptr<Mixer> mixer_;
    std::vector<Entry> undo_, redo_;
    std::string saved_;      // the fingerprint of the song as last saved

    ReportList *tracks_list_ = nullptr;
    ReportList *list_ = nullptr;
    wxWindow *last_list_ = nullptr;     // the list the keyboard was last in
    wxWindow *left_list_ = nullptr;     // the one it was in when the program was left
    //: the list the keyboard is being given back to, and since when
    wxWindow *return_to_ = nullptr;
    wxLongLong returned_at_;
    static constexpr int kReturnMs = 1500;
    wxTextCtrl *messages_ = nullptr;
    wxStatusBar *status_ = nullptr;
    wxMenuItem *mi_play_ = nullptr;
    wxMenuItem *mi_metronome_ = nullptr;
    wxMenuItem *mi_auto_preview_ = nullptr;
    wxMenuItem *mi_remove_velocities_ = nullptr;
    wxMenuItem *mi_remove_volumes_ = nullptr;
    wxMenuItem *mi_own_path_ = nullptr;
    wxMenuItem *mi_plain_held_ = nullptr;
    wxMenuItem *mi_octave_instruments_ = nullptr;
    wxMenuItem *mi_follow_ = nullptr;
    wxMenuItem *mi_show_unfinished_ = nullptr;
    wxMenuItem *mi_undo_ = nullptr;
    wxMenuItem *mi_redo_ = nullptr;
    bool relabel_pending_ = false;
};

}  // namespace svs
