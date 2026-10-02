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
    std::vector<Note> result() const { return rows_; }

private:
    std::pair<std::string, int> typed() const;
    void on_look(wxCommandEvent &);
    void looked(const std::string &word, int wanted, const std::vector<std::string> &phones);
    double value_beats() const;
    void build_rows();
    void sync(int select = 0);
    void refresh_row(int i);
    void on_count(wxSpinEvent &);
    void on_beats(wxEvent &evt);
    void on_key(wxKeyEvent &evt);
    void on_edit(wxCommandEvent &);
    void on_preview(wxCommandEvent &);

    Studio *studio_;
    int pitch_;
    std::vector<std::string> phonemes_;
    std::vector<Note> rows_;
    wxTextCtrl *word_, *beats_;
    wxButton *hear_, *ok_;
    wxSpinCtrl *count_;
    ReportList *list_;
};

//: The two curves a note carries, the bend and the mod wheel, edited the same way.
struct CurveKind {
    const char *title, *point_title, *list_caption, *value_label, *value_hint;
    double lo, hi;
    bool signed_values;
    const char *said, *unit;
    Bend Note::*field;
};
extern const CurveKind kBendCurve, kModCurve;

//: One point on a curve: how far into the note, and its value.
class PointDialog : public wxDialog {
public:
    PointDialog(wxWindow *parent, const CurveKind &kind, double at = 0.0, double value = 0.0);
    std::pair<double, double> result() const;

private:
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

//: Edit one note: the phonemes sung on it, its pitch and its length.
class NoteDialog : public wxDialog, Alive {
public:
    NoteDialog(wxWindow *parent, Studio *studio, const Note *note, const Note &seed = Note());
    Note result();

private:
    void on_insert(wxCommandEvent &);
    void on_preview(wxCommandEvent &);
    void on_points(const CurveKind &kind);
    int value_pitch() const;

    Studio *studio_;
    Note note_;
    wxTextCtrl *phon_, *pitch_, *beats_, *bend_start_, *bend_end_, *mod_start_, *mod_end_;
    wxButton *hear_;
};

//: Settings that belong to the whole song rather than to one part.
class SongSettingsDialog : public wxDialog {
public:
    SongSettingsDialog(wxWindow *parent, const Song &song);
    //: the song with the dialog's settings put on it
    void apply(Song &song) const;

private:
    wxSpinCtrl *tempo_, *consonants_, *room_, *wet_;
    wxTextCtrl *sig_;
    wxCheckBox *anticipate_;
    Sig was_sig_;
    std::vector<std::pair<const VoiceControl *, wxSpinCtrl *>> voice_ctrls_;
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
    wxSpinCtrl *volume_, *pan_, *consonants_, *room_, *wet_;
    wxCheckBox *own_consonants_, *own_reverb_, *own_voice_;
    std::vector<std::pair<const VoiceControl *, wxSpinCtrl *>> voice_ctrls_;
};

}  // namespace svs
