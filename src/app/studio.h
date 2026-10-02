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

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <wx/wx.h>

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
    std::string voice_name(int id) const;
    int track_voice_of(const Track &t) const { return track_voice(t, program_map_); }
    int consonant_pct() const { return int(std::lround(song_.consonants * 100)); }
    const VoiceStyle &song_voice() const { return song_.voice; }
    Reverb song_reverb() const { return song_.reverb; }
    //: the voice of the track being worked on, for previews
    int current_voice() const { return track_voice_of(track()); }
    //: render notes on the current track's voice and play them
    void preview(const std::vector<Note> &notes, std::function<void()> done);
    void lookup(const std::vector<std::string> &words,
                std::function<void(std::map<std::string, std::vector<std::string>>)> done);
    void play_file(const std::string &path, bool quiet = false);

private:
    // -- the song -------------------------------------------------------------
    std::vector<Note> &notes() { return song_.tracks[size_t(current_)].notes; }
    const Track &track() const { return song_.tracks[size_t(current_)]; }
    Track &track() { return song_.tracks[size_t(current_)]; }

    // -- interface ---------------------------------------------------------
    void build();
    void build_menu();
    void on_playback_shortcut(wxKeyEvent &evt);
    void on_keys(wxCommandEvent &);

    // -- engine answers ---------------------------------------------------
    void ready();
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
    void on_track_edit(wxCommandEvent &);
    void on_track_remove(wxCommandEvent &);
    void move_tracks(int delta);
    void on_panes(wxCommandEvent &);

    // -- notes ------------------------------------------------------------
    int selection() const;
    std::vector<int> selected() const;
    void select_only(const std::vector<int> &rows);
    double position(int index);
    int bar_of(int index);
    void on_goto_bar(wxCommandEvent &);
    void on_bar_rest(wxCommandEvent &);
    std::vector<std::string> bars();
    void sync(int select = 0);
    void on_key(wxKeyEvent &evt);
    void refresh_row(int i);
    void nudge_pitch(int by);
    void nudge_length(bool up);
    void sync_lengths();
    const Note *last();
    void on_select_all(wxCommandEvent &);
    void move_notes(int delta);
    bool to_clipboard(const std::string &text);
    std::string from_clipboard();
    void on_copy(wxCommandEvent &);
    void on_cut(wxCommandEvent &);
    void on_paste(wxCommandEvent &);
    void on_add_word(wxCommandEvent &);
    void on_add_note(wxCommandEvent &);
    void on_add_rest(wxCommandEvent &);
    void on_edit(wxCommandEvent &);
    void on_remove(wxCommandEvent &);

    // -- audio ------------------------------------------------------------
    RenderRequest request(const std::vector<Note> *only = nullptr, double start = 0.0,
                          const std::vector<const Track *> *parts = nullptr);
    RenderRequest playback(double start);
    void on_metronome(wxCommandEvent &);
    void on_hear(wxCommandEvent &);
    void heard(const RenderResult &res);
    double start_beats();
    void on_play_stop(wxCommandEvent &);
    void on_play(wxCommandEvent &);
    void start_playing(double start, bool quiet = false);
    void played(const RenderResult &res, bool quiet);
    void stop_audio();
    void on_stop(wxCommandEvent &);
    void on_export(wxCommandEvent &);
    void on_export_tracks(wxCommandEvent &);
    void export_track(std::shared_ptr<std::vector<std::pair<Track, std::string>>> jobs, size_t i,
                      int written);
    void render(const RenderRequest &req, const std::string &out,
                std::function<void(RenderResult)> done);
    void on_auto_preview(wxCommandEvent &);
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
private:
    void on_save(wxCommandEvent &);
    void on_save_as(wxCommandEvent &);
    void write_project(const std::string &path);
    void on_song_settings(wxCommandEvent &);
    void on_import(wxCommandEvent &);
    void imported_words(std::vector<ImportedPart> parts,
                        const std::map<std::string, std::vector<std::string>> &found, size_t pending);
    void on_close(wxCloseEvent &evt);

    Song song_;
    int current_ = 0;
    bool switching_ = false;
    std::vector<VoiceListing> voices_;
    std::map<int, int> program_map_;
    std::string wav_, preview_wav_;
    Player player_;
    bool rendering_ = false;
    std::string path_;
    bool dirty_ = false;
    Settings settings_;
    wxTimer preview_timer_, recovery_timer_;
    int preview_row_ = -1;
    std::unique_ptr<Worker> worker_;
    std::shared_ptr<Mixer> mixer_;
    std::vector<Entry> undo_, redo_;
    std::string saved_;      // the fingerprint of the song as last saved

    ReportList *tracks_list_ = nullptr;
    ReportList *list_ = nullptr;
    wxTextCtrl *messages_ = nullptr;
    wxStatusBar *status_ = nullptr;
    wxMenuItem *mi_play_ = nullptr;
    wxMenuItem *mi_metronome_ = nullptr;
    wxMenuItem *mi_auto_preview_ = nullptr;
};

}  // namespace svs
