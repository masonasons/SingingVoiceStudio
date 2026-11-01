// dialogs.h -- where editing happens: ordinary labelled fields in a dialog
// rather than inside the list, so every one of them reads properly.
#pragma once

#include <memory>
#include <vector>

#include <wx/wx.h>
#include <wx/spinctrl.h>

#include "app/widgets.h"
#include "core/project.h"
#include "voices/controls.h"
#include "voices/registry.h"

namespace svs {

class Studio;

//: Something a dialog hands to work that answers later, so the answer can
//: tell whether the dialog is still there to receive it.
class Alive {
public:
    Alive() : token_(std::make_shared<bool>(true)) {}
    ~Alive() { *token_ = false; }
    std::weak_ptr<bool> token() const { return token_; }

private:
    std::shared_ptr<bool> token_;
};

//: Choose one phoneme, with its example word, and hear it first.
class PhonemePicker : public wxDialog, Alive {
public:
    PhonemePicker(wxWindow *parent, Studio *studio, int pitch);
    std::string chosen() const;

private:
    void on_preview(wxCommandEvent &);
    Studio *studio_;
    int pitch_;
    std::vector<std::string> symbols_;
    wxChoice *choice_;
    wxButton *preview_;
};

//: Look a word up and lay it out over one note or several. A hyphen sets the
//: number of notes to start from, as VocalWriter's own lyrics are typed:
//: "Dai-sy", one fragment per note.
class AddWordDialog : public wxDialog, Alive {
public:
    AddWordDialog(wxWindow *parent, Studio *studio, int pitch, double beats);
    //: Add word to existing note(s): the word goes on `existing`, the notes
    //: that sound from the cursor on, each keeping all but what it sings.
    //: `text`: Add text, words divided by spaces put on them one after another.
    //: `spots`: where each of `existing` is in the track, for Add text's preview.
    AddWordDialog(wxWindow *parent, Studio *studio, std::vector<Note> existing, bool text = false,
                  std::vector<int> spots = {});
    ~AddWordDialog() override;
    std::vector<Note> result() const { return rows_; }

private:
    void build(double beats);
    std::string raw() const;
    std::pair<std::string, int> typed() const;
    std::vector<std::string> pieces() const;
    void on_look(wxCommandEvent &);
    void looked(const std::string &word, int wanted, const std::vector<std::string> &phones);
    void looked_text(std::vector<TextWord> words, std::map<std::string, std::vector<std::string>> found);
    bool ready() const { return !phonemes_.empty() || !words_.empty(); }
    void show_count();
    double value_beats() const;
    void build_rows(int select = 0);
    void edit_word(int i);
    void sync(int select = 0);
    void refresh_row(int i);
    void on_count(wxSpinEvent &);
    void on_beats(wxEvent &evt);
    void on_key(wxKeyEvent &evt);
    void on_edit(wxCommandEvent &);
    void on_preview(wxCommandEvent &);
    //: Add text's Preview, or Stop while it plays
    void on_play(wxCommandEvent &);
    void start_play();
    void stop_play();
    void show_stopped();

    Studio *studio_;
    int pitch_;
    std::vector<Note> existing_;
    std::vector<int> spots_;
    //: whether it is Add text, and the text's words once looked up
    bool text_ = false;
    std::vector<TextWord> words_;
    //: the notes each word goes on, as Notes to use spreads them (text_shares)
    std::vector<int> shares_;
    std::vector<std::string> phonemes_;
    std::vector<Note> rows_;
    wxTextCtrl *word_, *beats_ = nullptr;
    wxButton *hear_ = nullptr, *ok_;
    wxSpinCtrl *count_;
    ReportList *list_;
    //: Add text's preview: the button after the list, a timer that sees it
    //: has finished, and which render is wanted, so a late one is not played
    wxButton *play_ = nullptr;
    wxTimer play_timer_;
    bool playing_ = false;
    int play_wanted_ = 0;
};

//: The curves a note carries, the bend, the mod wheel and VocalWriter's
//: controls, edited the same way.
struct CurveKind {
    const char *title, *point_title, *list_caption, *value_label, *value_hint;
    double lo, hi;
    bool signed_values;
    const char *said, *unit;
    Bend Note::*field;
    //: one of the note's controls instead of `field` (see kNoteControls)
    const char *control = nullptr;
};
extern const CurveKind kBendCurve, kModCurve;
//: the curve for one of VocalWriter's controls, by its key
const CurveKind &control_curve(const std::string &key);
//: what the list shows for a note's velocity and controls
std::string describe_controls(const Note &note);

//: One point on a curve: how far into the note, and its value.
class PointDialog : public wxDialog {
public:
    PointDialog(wxWindow *parent, const CurveKind &kind, double at = 0.0, double value = 0.0);
    std::pair<double, double> result() const;

private:
    void on_ok(wxCommandEvent &evt);
    const CurveKind &kind_;
    wxTextCtrl *at_, *value_;
};

//: The whole shape of one note's bend, or its mod wheel, point by point.
class CurveDialog : public wxDialog, Alive {
public:
    CurveDialog(wxWindow *parent, Studio *studio, const Note &note, const CurveKind &kind);
    Bend result() const;

private:
    void sync(int select = 0);
    void on_key(wxKeyEvent &evt);
    void on_add(wxCommandEvent &);
    void on_edit(wxCommandEvent &);
    void on_remove(wxCommandEvent &);
    void on_preview(wxCommandEvent &);

    Studio *studio_;
    Note note_;
    const CurveKind &kind_;
    Bend points_;
    ReportList *list_;
    wxButton *hear_ = nullptr;
};

//: A note's VocalWriter controls, one row each; Enter edits one as a curve.
//: Only those `voice` sings are listed, and any the note already carries.
class ControlsDialog : public wxDialog, Alive {
public:
    ControlsDialog(wxWindow *parent, Studio *studio, const Note &note, int voice);
    NoteControls result() const { return note_.controls; }

private:
    void sync(int select = 0);
    void on_key(wxKeyEvent &evt);
    void on_edit(wxCommandEvent &);
    void on_clear(wxCommandEvent &);

    Studio *studio_;
    Note note_;
    //: the list's rows, as indices into kNoteControls
    std::vector<size_t> rows_;
    ReportList *list_;
};

//: Edit one note: the phonemes sung on it, its pitch and its length, and
//: for an instrument how long it sounds and the rest of its chord.
class NoteDialog : public wxDialog, Alive {
public:
    NoteDialog(wxWindow *parent, Studio *studio, const Note *note, const Note &seed = Note());
    Note result();
    //: every pitch typed, in order; more than one is a chord (chord_rows)
    std::vector<int> pitches() const;

private:
    void on_ok(wxCommandEvent &evt);
    void on_insert(wxCommandEvent &);
    void on_preview(wxCommandEvent &);
    void on_points(const CurveKind &kind);
    void on_controls(wxCommandEvent &);
    int value_pitch() const;

    Studio *studio_;
    Note note_;
    bool played_ = false;
    wxTextCtrl *phon_, *pitch_, *beats_, *hold_, *bend_start_, *bend_end_, *mod_start_, *mod_end_, *velocity_;
    wxButton *hear_;
};

//: A note as a chord: a row for each pitch, all but the last taking no time
//: and sounding as long as the note.
std::vector<Note> chord_rows(const Note &note, const std::vector<int> &pitches);
//: Whether a note control (kNoteControls) reaches a voice: the volume
//: reaches every voice; all but the pedal reach VocalWriter's voices, the
//: bend range and the rest's pitch only through its own sequencer; the
//: pedal, bend range and detune reach its instruments.
bool control_reaches(const std::string &key, int voice);
//: A voice's name without its engine's: "Betty", "Cello" for "GM Cello".
std::string short_voice_name(const std::string &name, const std::string &engine);

//: Settings that belong to the whole song rather than to one part.
class SongSettingsDialog : public wxDialog {
public:
    //: `own_time`: every part is sung by an engine that places its consonants
    //: itself, which leaves "Consonants before the beat" nothing to do
    SongSettingsDialog(wxWindow *parent, const Song &song, bool own_time = false);
    //: the song with the dialog's settings put on it
    void apply(Song &song) const;

private:
    wxSpinCtrl *tempo_, *consonants_, *room_, *wet_;
    wxTextCtrl *sig_, *end_;
    wxCheckBox *anticipate_;
    Sig was_sig_;
    double was_end_;
    wxString end_shown_;
    std::vector<std::pair<const VoiceControl *, wxSpinCtrl *>> voice_ctrls_;
};

//: A controller: one change from where it stands in the track on, chosen
//: first. The voice is the track's from there, any engine's; the tempo and
//: the time signature are the whole song's, whichever track they are on; or
//: one of VocalWriter's controls, as a note carries them, of those the voice
//: in force sings.
class ControllerDialog : public wxDialog {
public:
    //: `voice_before`: the voice the track sings with up to the controller
    ControllerDialog(wxWindow *parent, Studio *studio, const Note &controller, int voice_before);
    //: the controller as the fields have it: the one change chosen, or
    //: nothing if its field was left blank
    Note result() const;

private:
    void on_ok(wxCommandEvent &evt);
    void fill_voices(int select);
    void show_kind();
    Note note_;
    std::vector<VoiceListing> voices_;
    std::vector<std::string> engine_names_;
    std::vector<int> ids_;      // the voices in the voice box, in order
    wxChoice *kind_, *engine_, *voice_;
    wxTextCtrl *tempo_, *sig_, *pan_;
    std::vector<std::pair<const NoteControl *, wxTextCtrl *>> controls_;
    //: the fields of each kind, in the order the first box lists them
    wxBoxSizer *outer_;
    std::vector<wxSizer *> groups_;
};

//: Whether a MIDI import closes the short rests between notes, and up to how long.
class CloseRestsDialog : public wxDialog {
public:
    CloseRestsDialog(wxWindow *parent, bool on, double beats, bool in_words);
    bool on() const { return on_->GetValue(); }
    double beats() const;
    bool in_words() const { return words_->GetValue(); }

private:
    wxCheckBox *on_, *words_;
    wxTextCtrl *beats_;
    double was_;
};

//: Which of a MIDI file's parts are imported, each ticked or unticked with
//: Space. The first row, every part, is ticked to begin with and ticks or
//: unticks all the others with it.
class ImportPartsDialog : public wxDialog {
public:
    ImportPartsDialog(wxWindow *parent, const wxArrayString &parts);
    //: the parts ticked, as indices into `parts`
    std::vector<int> chosen() const;

private:
    void on_checked(int row);
    void on_ok(wxCommandEvent &evt);
    //: whether a part is ticked; if not, says so
    bool ready();
    ReportList *list_;
};

//: Which track a type 0 MIDI file's lyrics are sung on: one of `parts`,
//: chosen in a box that starts on the first. Without `lyrics`, which track
//: is sung in a file that has none.
class LyricTrackDialog : public wxDialog {
public:
    LyricTrackDialog(wxWindow *parent, const wxArrayString &parts, bool lyrics = true);
    int chosen() const { return choice_->GetSelection(); }

private:
    wxChoice *choice_;
};

//: Where Panthera is, for the MacinTalk voices: a folder typed in or browsed
//: to, or nothing, to look where Panthera's NVDA add-on puts it.
class PantheraDialog : public wxDialog {
public:
    PantheraDialog(wxWindow *parent, const std::string &folder);
    std::string folder() const;

private:
    void on_browse(wxCommandEvent &);
    wxTextCtrl *path_;
};

//: Which MIDI channel each track goes out on, asked before a MIDI export as
//: VocalWriter's Set MIDI Channels asked: the drum kit on 10 to begin with.
class MidiChannelsDialog : public wxDialog {
public:
    MidiChannelsDialog(wxWindow *parent, const std::vector<const Track *> &tracks, const std::vector<int> &channels);
    //: one for each track, 0-15
    std::vector<int> channels() const;

private:
    std::vector<wxChoice *> choices_;
};

//: A track's name, voice, volume and pan, and how that voice is set up.
class TrackDialog : public wxDialog {
public:
    TrackDialog(wxWindow *parent, Studio *studio, const Track &track);
    void apply(Track &track) const;

private:
    void on_own_voice(wxCommandEvent &);
    void on_own_reverb(wxCommandEvent &);

    Studio *studio_;
    void fill_voices(int select);
    void sync_velocity();
    bool changes_to_vocalwriter_ = false;
    struct Engine {
        std::string name;
        bool available;
        std::vector<VoiceListing> voices;
        int last = -1;
    };
    std::vector<Engine> engines_;
    wxChoice *engine_;
    std::vector<int> ids_;      // the voices in the voice box, in order
    wxTextCtrl *name_;
    wxChoice *voice_;
    wxSpinCtrl *volume_, *pan_, *velocity_, *consonants_, *room_, *wet_;
    wxCheckBox *own_consonants_, *own_reverb_, *own_voice_;
    std::vector<std::pair<const VoiceControl *, wxSpinCtrl *>> voice_ctrls_;
};

//: A backing track's name, its file, where in the song the file starts, and
//: its volume and pan.
class BackingDialog : public wxDialog {
public:
    BackingDialog(wxWindow *parent, const Track &track);
    void apply(Track &track) const;

private:
    void on_browse(wxCommandEvent &);
    void on_ok(wxCommandEvent &);
    std::string file() const;
    wxTextCtrl *name_, *path_, *start_;
    wxSpinCtrl *volume_, *pan_;
};

}  // namespace svs
