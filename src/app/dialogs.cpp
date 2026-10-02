#include "app/dialogs.h"

#include <algorithm>
#include <cmath>

#include <wx/tokenzr.h>

#include "app/studio.h"
#include "core/phonology.h"

namespace svs {

namespace {

std::string bend_text(std::optional<double> value) {
    if (!value) return "";
    return fmt_g(std::nearbyint(*value * 1000) / 1000);
}

std::optional<double> read_bend(const wxString &raw) {
    wxString text = raw;
    text.Trim(true).Trim(false);
    if (text.empty()) return std::nullopt;
    double v;
    if (!text.ToCDouble(&v)) return std::nullopt;
    return v;
}

bool read_number(const wxString &raw, double *out) {
    wxString t = raw;
    t.Trim(true).Trim(false);
    return t.ToCDouble(out);
}

wxSizer *buttons(wxDialog *d) { return d->CreateStdDialogButtonSizer(wxOK | wxCANCEL); }

}  // namespace

// -- PhonemePicker --------------------------------------------------------------

PhonemePicker::PhonemePicker(wxWindow *parent, Studio *studio, int pitch)
    : wxDialog(parent, wxID_ANY, "Insert phoneme"), studio_(studio), pitch_(pitch) {
    wxArrayString choices;
    for (const PaletteRow &r : studio->singable()) {
        symbols_.push_back(r.symbol);
        choices.Add(r.example.empty() ? W(r.symbol) : W(r.symbol + "   as in " + r.example));
    }
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    choice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(300, -1), choices);
    choice_->SetSelection(0);
    labelled(this, outer, "Phoneme", choice_, 1);
    preview_ = new wxButton(this, wxID_ANY, "&Preview");
    preview_->Bind(wxEVT_BUTTON, &PhonemePicker::on_preview, this);
    outer->Add(preview_, 0, wxALL, 6);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    choice_->SetFocus();
}

std::string PhonemePicker::chosen() const {
    int i = choice_->GetSelection();
    return i >= 0 && i < int(symbols_.size()) ? symbols_[size_t(i)] : "";
}

void PhonemePicker::on_preview(wxCommandEvent &) {
    preview_->Disable();
    studio_->say("previewing " + W(chosen()));
    Note n({chosen()}, pitch_, 0.45);
    auto alive = token();
    wxButton *b = preview_;
    studio_->preview({n}, [alive, b]() {
        if (auto a = alive.lock(); a && *a) b->Enable();
    });
}

// -- AddWordDialog ---------------------------------------------------------------

AddWordDialog::AddWordDialog(wxWindow *parent, Studio *studio, int pitch, double beats)
    : wxDialog(parent, wxID_ANY, "Add word"), studio_(studio), pitch_(pitch) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    word_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(220, -1), wxTE_PROCESS_ENTER);
    word_->Bind(wxEVT_TEXT_ENTER, &AddWordDialog::on_look, this);
    labelled(this, outer, "Word", word_, 1, "a hyphen marks where the notes divide, as in dai-sy");

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    wxButton *look = new wxButton(this, wxID_ANY, "&Look up");
    look->Bind(wxEVT_BUTTON, &AddWordDialog::on_look, this);
    row->Add(look, 0, wxALL, 3);
    hear_ = new wxButton(this, wxID_ANY, "&Preview");
    hear_->Bind(wxEVT_BUTTON, &AddWordDialog::on_preview, this);
    hear_->Disable();
    row->Add(hear_, 0, wxALL, 3);
    outer->Add(row, 0);

    count_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 1, 1, 1);
    count_->Bind(wxEVT_SPINCTRL, &AddWordDialog::on_count, this);
    // no hint here: a spin control's edit box takes its name from the
    // caption as written, so the name has to stand on its own
    labelled(this, outer, "Notes to use", count_);
    beats_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(beats)), wxDefaultPosition, wxSize(80, -1),
                            wxTE_PROCESS_ENTER);
    beats_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &e) { on_beats(e); });
    beats_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { on_beats(e); });
    labelled(this, outer, "Beats", beats_, 0, "for each note");

    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(380, 140));
    list_->InsertColumn(0, "Phonemes", wxLIST_FORMAT_LEFT, 165);
    list_->InsertColumn(1, "Pitch", wxLIST_FORMAT_LEFT, 55);
    list_->InsertColumn(2, "Beats", wxLIST_FORMAT_LEFT, 150);
    list_->OnActivated([this] {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->OnKey([this](wxKeyEvent &e) { on_key(e); });
    caption(this, outer, "Notes for this word", list_);

    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    ok_ = wxDynamicCast(FindWindow(wxID_OK), wxButton);
    if (ok_) ok_->Disable();
    SetSizerAndFit(outer);
    word_->SetFocus();
}

std::pair<std::string, int> AddWordDialog::typed() const {
    std::string raw = U(word_->GetValue());
    raw.erase(0, raw.find_first_not_of(" \t"));
    raw.erase(raw.find_last_not_of(" \t") + 1);
    int parts = 0;
    bool in_part = false;
    for (char c : raw) {
        if (c == '-') in_part = false;
        else if (!in_part) {
            in_part = true;
            ++parts;
        }
    }
    std::string clean;
    for (unsigned char c : raw)
        if (std::isalpha(c) || c == '\'') clean.push_back(char(c));
    return {clean, parts};
}

void AddWordDialog::on_look(wxCommandEvent &) {
    auto [word, wanted] = typed();
    if (word.empty()) {
        studio_->say("type a word first");
        return;
    }
    studio_->say("looking up " + W(word));
    auto alive = token();
    std::string w = word;
    int n = wanted;
    studio_->lookup({word}, [this, alive, w, n](std::map<std::string, std::vector<std::string>> res) {
        if (auto a = alive.lock(); a && *a) looked(w, n, res[w]);
    });
}

void AddWordDialog::looked(const std::string &word, int wanted, const std::vector<std::string> &phones) {
    if (phones.empty()) {
        studio_->say("no pronunciation for " + W(word) +
                     ". Add a note and type its phonemes instead.");
        return;
    }
    phonemes_ = phones;
    auto parts = syllabify(phones);
    int n = int(parts.size());
    count_->SetRange(1, std::max(1, n));
    // one note per syllable unless the hyphens said otherwise
    count_->SetValue(wanted <= 1 ? n : std::min(wanted, n));
    build_rows();
    hear_->Enable();
    if (ok_) ok_->Enable();
    std::string joined;
    for (const auto &p : phones) joined += (joined.empty() ? "" : " ") + p;
    studio_->say(W(word + " is " + joined + ", " + std::to_string(n) + " syllable" +
                   (n == 1 ? "" : "s")));
    count_->SetFocus();
}

double AddWordDialog::value_beats() const {
    double v;
    if (!read_number(beats_->GetValue(), &v)) return kDefaultBeats;
    return std::max(0.05, v);
}

void AddWordDialog::build_rows() {
    std::string word = typed().first;
    auto groups = regroup(phonemes_, count_->GetValue());
    double b = value_beats();
    rows_.clear();
    for (size_t i = 0; i < groups.size(); ++i) rows_.push_back(Note(groups[i], pitch_, b, i == 0 ? word : ""));
    sync();
}

void AddWordDialog::sync(int select) {
    list_->DeleteAllItems();
    for (size_t i = 0; i < rows_.size(); ++i) {
        list_->InsertItem(long(i), W(rows_[i].label()));
        list_->SetItem(long(i), 1, W(pitch_name(rows_[i].pitch)));
        list_->SetItem(long(i), 2, W(fmt_g(rows_[i].beats)));
    }
    if (!rows_.empty()) {
        int i = std::min(std::max(select, 0), int(rows_.size()) - 1);
        list_->SelectRow(i);
        list_->FocusRow(i);
    }
}

void AddWordDialog::refresh_row(int i) {
    const Note &n = rows_[size_t(i)];
    list_->SetItem(i, 0, W(n.label()));
    list_->SetItem(i, 1, W(pitch_name(n.pitch)));
    list_->SetItem(i, 2, W(fmt_g(n.beats)));
}

void AddWordDialog::on_count(wxSpinEvent &) {
    if (phonemes_.empty()) return;
    build_rows();
    std::string all;
    for (const Note &n : rows_) all += (all.empty() ? "" : ", ") + n.text();
    studio_->say(W(std::to_string(rows_.size()) + " note" + (rows_.size() == 1 ? "" : "s") + ": " + all));
}

void AddWordDialog::on_beats(wxEvent &evt) {
    evt.Skip();
    if (phonemes_.empty()) return;
    double b = value_beats();
    for (size_t i = 0; i < rows_.size(); ++i) {
        rows_[i].beats = b;
        refresh_row(int(i));
    }
}

void AddWordDialog::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    int i = list_->FirstSelected();
    if (evt.AltDown() && i >= 0 &&
        (code == WXK_UP || code == WXK_DOWN || code == WXK_LEFT || code == WXK_RIGHT)) {
        Note &n = rows_[size_t(i)];
        std::string message;
        if (code == WXK_UP || code == WXK_DOWN) {
            n.pitch = std::max(0, std::min(127, n.pitch + (code == WXK_UP ? kSemitone : -kSemitone)));
            message = pitch_name(n.pitch);
        } else {
            auto want = stepped_length(n.beats, code == WXK_RIGHT);
            if (!want) {
                studio_->announce_note(
                    W(spoken_length(n.beats, studio_->signature()) + ", the shortest a nudge will make it"),
                    list_, i);
                return;
            }
            n.beats = *want;
            message = spoken_length(n.beats, studio_->signature());
        }
        refresh_row(i);
        studio_->announce_note(W(message), list_, i);
    } else if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) && i >= 0) {
        wxCommandEvent e;
        on_edit(e);
    } else {
        evt.Skip();
    }
}

void AddWordDialog::on_edit(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0) return;
    NoteDialog dlg(this, studio_, &rows_[size_t(i)]);
    if (dlg.ShowModal() != wxID_OK) return;
    rows_[size_t(i)] = dlg.result();
    sync(i);
}

void AddWordDialog::on_preview(wxCommandEvent &) {
    hear_->Disable();
    std::string all;
    for (const Note &n : rows_) all += (all.empty() ? "" : ", ") + n.text();
    studio_->say("previewing " + W(all));
    auto alive = token();
    wxButton *b = hear_;
    studio_->preview(rows_, [alive, b]() {
        if (auto a = alive.lock(); a && *a) b->Enable();
    });
}

// -- the two curves -------------------------------------------------------------------

//: What differs between editing a bend and editing the mod wheel: the words,
//: the range, and how a value is written.
const CurveKind kBendCurve = {"Pitch bend", "Bend point", "Bend points", "Semitones",
                              "above or below the written pitch", -1e9, 1e9, true, "bend",
                              " semitones", &Note::bend};
const CurveKind kModCurve = {"Mod wheel", "Mod point", "Mod wheel points", "Mod wheel",
                             "0 is at rest, 127 is the deepest vibrato", 0, 127, false, "mod wheel",
                             "", &Note::mod};

static std::string value_text(const CurveKind &k, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, k.signed_values ? "%+g" : "%g", std::nearbyint(v * 1000) / 1000);
    return buf;
}

static void by_position(Bend &points) {
    std::stable_sort(points.begin(), points.end(),
                     [](const std::pair<double, double> &a, const std::pair<double, double> &b) {
                         return a.first < b.first;
                     });
}

// -- PointDialog --------------------------------------------------------------------

PointDialog::PointDialog(wxWindow *parent, const CurveKind &kind, double at, double value)
    : wxDialog(parent, wxID_ANY, kind.point_title), kind_(kind) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    at_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(std::nearbyint(at * 100 * 1000) / 1000)), wxDefaultPosition,
                         wxSize(90, -1));
    labelled(this, outer, "Position", at_, 0, "per cent through the note, 0 is the start");
    value_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(std::nearbyint(value * 1000) / 1000)), wxDefaultPosition,
                            wxSize(90, -1));
    labelled(this, outer, kind.value_label, value_, 0, kind.value_hint);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    at_->SetFocus();
}

std::pair<double, double> PointDialog::result() const {
    double at = 0.0, v = 0.0;
    if (!read_number(at_->GetValue(), &at)) at = 0.0;
    if (!read_number(value_->GetValue(), &v)) v = 0.0;
    return {std::min(1.0, std::max(0.0, at / 100.0)), std::max(kind_.lo, std::min(kind_.hi, v))};
}

// -- CurveDialog ------------------------------------------------------------------------

CurveDialog::CurveDialog(wxWindow *parent, Studio *studio, const Note &note, const CurveKind &kind)
    : wxDialog(parent, wxID_ANY, kind.title), studio_(studio), note_(note), kind_(kind),
      points_(note.*(kind.field)) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(340, 170));
    list_->InsertColumn(0, "Position", wxLIST_FORMAT_LEFT, 150);
    list_->InsertColumn(1, kind.value_label, wxLIST_FORMAT_LEFT, 150);
    list_->OnActivated([this] {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->OnKey([this](wxKeyEvent &e) { on_key(e); });
    caption(this, outer, kind.list_caption, list_);

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    struct B {
        const char *label;
        void (CurveDialog::*fn)(wxCommandEvent &);
    } bs[] = {{"&Add point...", &CurveDialog::on_add},
              {"&Edit point...", &CurveDialog::on_edit},
              {"&Remove point", &CurveDialog::on_remove},
              {"&Preview note", &CurveDialog::on_preview}};
    for (const B &b : bs) {
        wxButton *btn = new wxButton(this, wxID_ANY, b.label);
        btn->Bind(wxEVT_BUTTON, b.fn, this);
        row->Add(btn, 0, wxALL, 3);
        if (b.fn == &CurveDialog::on_preview) hear_ = btn;
    }
    outer->Add(row, 0);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    sync();
    list_->SetFocus();
}

void CurveDialog::sync(int select) {
    by_position(points_);
    list_->DeleteAllItems();
    for (size_t i = 0; i < points_.size(); ++i) {
        list_->InsertItem(long(i), W(fmt_g(std::nearbyint(points_[i].first * 100 * 1000) / 1000) + " per cent"));
        list_->SetItem(long(i), 1, W(value_text(kind_, points_[i].second)));
    }
    if (!points_.empty()) {
        int i = std::min(std::max(select, 0), int(points_.size()) - 1);
        list_->SelectRow(i);
        list_->FocusRow(i);
    }
}

void CurveDialog::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    wxCommandEvent e;
    if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) on_edit(e);
    else if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE) on_remove(e);
    else evt.Skip();
}

void CurveDialog::on_add(wxCommandEvent &) {
    PointDialog dlg(this, kind_);
    if (dlg.ShowModal() != wxID_OK) return;
    auto pt = dlg.result();
    points_.push_back(pt);
    by_position(points_);
    int at = int(std::find(points_.begin(), points_.end(), pt) - points_.begin());
    sync(at);
    studio_->say(W(std::string(kind_.said) + " point at " + fmt_g(std::nearbyint(pt.first * 100 * 1000) / 1000) +
                   " per cent, " + value_text(kind_, pt.second) + kind_.unit));
}

void CurveDialog::on_edit(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0) {
        studio_->say("select a point first");
        return;
    }
    PointDialog dlg(this, kind_, points_[size_t(i)].first, points_[size_t(i)].second);
    if (dlg.ShowModal() != wxID_OK) return;
    points_[size_t(i)] = dlg.result();
    sync(i);
}

void CurveDialog::on_remove(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0) {
        studio_->say("select a point first");
        return;
    }
    points_.erase(points_.begin() + i);
    sync(std::min(i, int(points_.size()) - 1));
    studio_->say(W("point removed, " + std::to_string(points_.size()) + " left"));
}

void CurveDialog::on_preview(wxCommandEvent &) {
    hear_->Disable();
    Note n = note_;
    n.*(kind_.field) = result();
    auto alive = token();
    wxButton *b = hear_;
    studio_->preview({n}, [alive, b]() {
        if (auto a = alive.lock(); a && *a) b->Enable();
    });
}

Bend CurveDialog::result() const {
    Bend out = points_;
    by_position(out);
    return out;
}

// -- NoteDialog ------------------------------------------------------------------------

NoteDialog::NoteDialog(wxWindow *parent, Studio *studio, const Note *note, const Note &seed)
    : wxDialog(parent, wxID_ANY, note ? "Edit note" : "Add note"),
      studio_(studio),
      note_(note ? *note : seed) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    phon_ = new wxTextCtrl(this, wxID_ANY, W(note_.text()), wxDefaultPosition, wxSize(300, -1));
    labelled(this, outer, "Phonemes", phon_, 1, "separated by spaces");

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    wxButton *ins = new wxButton(this, wxID_ANY, "&Insert phoneme...");
    ins->Bind(wxEVT_BUTTON, &NoteDialog::on_insert, this);
    row->Add(ins, 0, wxALL, 3);
    hear_ = new wxButton(this, wxID_ANY, "Preview &note");
    hear_->Bind(wxEVT_BUTTON, &NoteDialog::on_preview, this);
    row->Add(hear_, 0, wxALL, 3);
    outer->Add(row, 0);

    pitch_ = new wxTextCtrl(this, wxID_ANY, W(pitch_name(note_.pitch)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Pitch", pitch_, 0, "a name like C4, or a MIDI number");
    beats_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(note_.beats)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Beats", beats_);
    auto ends = bend_ends(note_.bend);
    bend_start_ = new wxTextCtrl(this, wxID_ANY, W(bend_text(ends.first)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Bend at start", bend_start_, 0, "semitones, blank for none");
    bend_end_ = new wxTextCtrl(this, wxID_ANY, W(bend_text(ends.second)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Bend at end", bend_end_, 0, "semitones, blank to hold the starting value");
    wxButton *points = new wxButton(this, wxID_ANY, "Bend p&oints...");
    points->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_points(kBendCurve); });
    outer->Add(points, 0, wxALL, 6);

    // The mod wheel, the same way: its two ends here, every point behind the
    // button. It deepens the vibrato; blank is the wheel at rest.
    auto mends = bend_ends(note_.mod);
    mod_start_ = new wxTextCtrl(this, wxID_ANY, W(bend_text(mends.first)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Mod at start", mod_start_, 0, "0 to 127, blank for none");
    mod_end_ = new wxTextCtrl(this, wxID_ANY, W(bend_text(mends.second)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Mod at end", mod_end_, 0, "blank to hold the starting value");
    wxButton *mods = new wxButton(this, wxID_ANY, "&Mod points...");
    mods->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_points(kModCurve); });
    outer->Add(mods, 0, wxALL, 6);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    phon_->SetFocus();
}

void NoteDialog::on_insert(wxCommandEvent &) {
    if (studio_->singable().empty()) {
        studio_->say("the phoneme list has not loaded yet. Try again in a moment, or type the "
                     "phonemes in by hand.");
        return;
    }
    PhonemePicker dlg(this, studio_, value_pitch());
    if (dlg.ShowModal() != wxID_OK) return;
    std::string sym = dlg.chosen();
    wxString text = phon_->GetValue();
    text.Trim(true).Trim(false);
    wxString joined = text.empty() ? W(sym) : text + " " + W(sym);
    phon_->SetValue(joined);
    phon_->SetInsertionPointEnd();
    studio_->say("inserted " + W(sym));
}

void NoteDialog::on_preview(wxCommandEvent &) {
    hear_->Disable();
    Note n = result();
    studio_->say("previewing " + W(n.text().empty() ? "nothing" : n.text()));
    auto alive = token();
    wxButton *b = hear_;
    studio_->preview({n}, [alive, b]() {
        if (auto a = alive.lock(); a && *a) b->Enable();
    });
}

void NoteDialog::on_points(const CurveKind &kind) {
    Note n = result();
    CurveDialog dlg(this, studio_, n, kind);
    if (dlg.ShowModal() != wxID_OK) return;
    Bend &curve = note_.*(kind.field);
    curve = dlg.result();
    // the two fields have to catch up, or closing this window would write its
    // stale endpoints back over what was just edited
    bool bend = kind.field == &Note::bend;
    auto ends = bend_ends(curve);
    (bend ? bend_start_ : mod_start_)->ChangeValue(W(bend_text(ends.first)));
    (bend ? bend_end_ : mod_end_)->ChangeValue(W(bend_text(ends.second)));
    std::string described = bend ? describe_bend(curve) : describe_mod(curve);
    studio_->say(W(std::string(bend ? "bend" : "mod wheel") + " has " + std::to_string(curve.size()) + " point" +
                   (curve.size() == 1 ? "" : "s") + ", " + (described.empty() ? "none" : described)));
}

int NoteDialog::value_pitch() const {
    try {
        return parse_pitch(U(pitch_->GetValue()));
    } catch (...) {
        return kDefaultPitch;
    }
}

Note NoteDialog::result() {
    note_.phonemes.clear();
    wxStringTokenizer tok(phon_->GetValue(), " \t\r\n", wxTOKEN_STRTOK);
    while (tok.HasMoreTokens()) note_.phonemes.push_back(U(tok.GetNextToken()));
    note_.pitch = value_pitch();
    double b;
    if (read_number(beats_->GetValue(), &b)) note_.beats = std::max(0.05, b);
    note_.bend = with_ends(note_.bend, read_bend(bend_start_->GetValue()), read_bend(bend_end_->GetValue()));
    auto wheel = [](std::optional<double> v) -> std::optional<double> {
        if (!v) return v;
        return std::max(0.0, std::min(127.0, *v));
    };
    note_.mod = with_ends(note_.mod, wheel(read_bend(mod_start_->GetValue())), wheel(read_bend(mod_end_->GetValue())));
    return note_;
}

// -- SongSettingsDialog ------------------------------------------------------------------

namespace {
wxSpinCtrl *spin(wxWindow *parent, int lo, int hi, int value, int width) {
    return new wxSpinCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1), wxSP_ARROW_KEYS, lo,
                          hi, value);
}
}  // namespace

SongSettingsDialog::SongSettingsDialog(wxWindow *parent, const Song &song)
    : wxDialog(parent, wxID_ANY, "Song settings"), was_sig_(song.sig) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    tempo_ = spin(this, 30, 250, int(song.bpm), 80);
    labelled(this, outer, "Tempo", tempo_, 0, "beats per minute");
    sig_ = new wxTextCtrl(this, wxID_ANY, W(format_sig(song.sig)), wxDefaultPosition, wxSize(60, -1));
    labelled(this, outer, "Time signature", sig_, 0, "as 4/4; it sets where the bars fall");
    consonants_ = spin(this, 10, 100, int(std::lrint(song.consonants * 100)), 80);
    labelled(this, outer, "Consonant length", consonants_, 0,
             "100 is natural, 40 clips them hard against the vowels");

    // A note is heard where its vowel is, not where it starts, so a note that
    // opens with consonants sounds late -- by an amount that depends on how
    // the word is spelled, which is what makes a line drag unevenly.
    anticipate_ = new wxCheckBox(this, wxID_ANY, "Consonants &before the beat");
    anticipate_->SetValue(song.anticipate);
    anticipate_->SetToolTip(
        "The vowel lands on the beat and the consonants are sung into the end of the note before "
        "it, the way a singer does. Turn this off to hear a note start exactly where it is "
        "written, consonants and all.");
    outer->Add(anticipate_, 0, wxALL, 6);

    room_ = spin(this, 0, 100, song.reverb.first, 80);
    labelled(this, outer, "Reverb room", room_, 0, "how big the space is; VocalWriter used 40");
    wet_ = spin(this, 0, 100, song.reverb.second, 80);
    labelled(this, outer, "Reverb amount", wet_, 0, "per cent heard; 0 is none at all, VocalWriter used 24");

    size_t n;
    const VoiceControl *c = voice_controls(&n);
    for (size_t i = 0; i < n; ++i) {
        wxSpinCtrl *s = spin(this, c[i].lo, c[i].hi, song.voice.*(c[i].field), 90);
        labelled(this, outer, c[i].label, s, 0, c[i].hint);
        voice_ctrls_.push_back({&c[i], s});
    }
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    tempo_->SetFocus();
}

void SongSettingsDialog::apply(Song &song) const {
    song.bpm = tempo_->GetValue();
    song.sig = parse_sig(U(sig_->GetValue()), was_sig_);
    song.consonants = consonants_->GetValue() / 100.0;
    for (const auto &vc : voice_ctrls_) song.voice.*(vc.first->field) = vc.second->GetValue();
    song.voice = clean_voice(song.voice);
    song.reverb = {room_->GetValue(), wet_->GetValue()};
    song.anticipate = anticipate_->GetValue();
}

// -- TrackDialog --------------------------------------------------------------------------

TrackDialog::TrackDialog(wxWindow *parent, Studio *studio, const Track &track)
    : wxDialog(parent, wxID_ANY, "Track"), studio_(studio) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    name_ = new wxTextCtrl(this, wxID_ANY, W(track.name), wxDefaultPosition, wxSize(220, -1));
    labelled(this, outer, "Name", name_);

    // Every voice of every engine: VocalWriter's bank in its own order, the
    // people's names first and then the instruments, which sing too; then
    // DECtalk, the SSI-263 and Microsoft's voices.
    wxArrayString names;
    int current = studio->track_voice_of(track), select = 0;
    for (const VoiceListing &v : studio->voice_list()) {
        if (v.id == current) select = int(ids_.size());
        ids_.push_back(v.id);
        names.Add(W(v.available ? v.name : v.name + " (not installed)"));
    }
    if (ids_.empty()) {
        ids_.push_back(0);
        names.Add("Robert");
    }
    voice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(240, -1), names);
    voice_->SetSelection(select);
    labelled(this, outer, "Voice", voice_);

    volume_ = spin(this, 0, 100, track.volume, 80);
    labelled(this, outer, "Volume", volume_, 0, "per cent");
    pan_ = spin(this, -100, 100, track.pan, 80);
    labelled(this, outer, "Pan", pan_, 0, "-100 far left to 100 far right");

    own_consonants_ = new wxCheckBox(this, wxID_ANY, "&Consonant length just for this track");
    own_consonants_->SetValue(bool(track.consonants));
    outer->Add(own_consonants_, 0, wxALL, 6);
    consonants_ = spin(this, 10, 100,
                       int(std::lrint((track.consonants ? *track.consonants : studio->consonant_pct() / 100.0) * 100)),
                       80);
    labelled(this, outer, "Consonant length", consonants_, 0, "when the box above is ticked");
    consonants_->Enable(bool(track.consonants));
    own_consonants_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) {
        consonants_->Enable(own_consonants_->GetValue());
    });

    // A part may be sung in a different space from the rest -- a lead dry in
    // front of a choir in a hall.
    own_reverb_ = new wxCheckBox(this, wxID_ANY, "&Reverb just for this track");
    own_reverb_->SetValue(bool(track.reverb));
    outer->Add(own_reverb_, 0, wxALL, 6);
    Reverb shown = track.reverb ? *track.reverb : studio->song_reverb();
    room_ = spin(this, 0, 100, shown.first, 80);
    labelled(this, outer, "Reverb room", room_, 0, "when the box above is ticked");
    wet_ = spin(this, 0, 100, shown.second, 80);
    labelled(this, outer, "Reverb amount", wet_, 0, "per cent heard, when the box above is ticked");
    room_->Enable(bool(track.reverb));
    wet_->Enable(bool(track.reverb));
    own_reverb_->Bind(wxEVT_CHECKBOX, &TrackDialog::on_own_reverb, this);

    // The voice controls. Like the consonant length, a part follows the
    // song's until it is given its own: the values shown while the box is
    // unticked are the song's, so ticking it starts from what you heard.
    own_voice_ = new wxCheckBox(this, wxID_ANY, "&Voice controls just for this track");
    own_voice_->SetValue(bool(track.voice));
    outer->Add(own_voice_, 0, wxALL, 6);
    VoiceStyle values = track.voice ? *track.voice : studio->song_voice();
    size_t n;
    const VoiceControl *c = voice_controls(&n);
    for (size_t i = 0; i < n; ++i) {
        wxSpinCtrl *s = spin(this, c[i].lo, c[i].hi, values.*(c[i].field), 90);
        labelled(this, outer, c[i].label, s, 0, c[i].hint);
        s->Enable(bool(track.voice));
        voice_ctrls_.push_back({&c[i], s});
    }
    own_voice_->Bind(wxEVT_CHECKBOX, &TrackDialog::on_own_voice, this);

    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    name_->SetFocus();
    name_->SetInsertionPointEnd();
}

void TrackDialog::on_own_voice(wxCommandEvent &) {
    for (auto &vc : voice_ctrls_) vc.second->Enable(own_voice_->GetValue());
}

void TrackDialog::on_own_reverb(wxCommandEvent &) {
    room_->Enable(own_reverb_->GetValue());
    wet_->Enable(own_reverb_->GetValue());
}

void TrackDialog::apply(Track &track) const {
    wxString name = name_->GetValue();
    name.Trim(true).Trim(false);
    track.name = name.empty() ? "Voice" : U(name);
    int i = voice_->GetSelection();
    track.voice_id = ids_[size_t(std::max(0, std::min(i, int(ids_.size()) - 1)))];
    track.volume = volume_->GetValue();
    track.pan = pan_->GetValue();
    // None when the part is following the song, which is not the same as
    // holding a copy of what the song says now: a part that follows keeps
    // following when the song settings change.
    if (own_voice_->GetValue()) {
        VoiceStyle v;
        for (const auto &vc : voice_ctrls_) v.*(vc.first->field) = vc.second->GetValue();
        track.voice = clean_voice(v);
    } else {
        track.voice.reset();
    }
    if (own_consonants_->GetValue()) track.consonants = consonants_->GetValue() / 100.0;
    else track.consonants.reset();
    if (own_reverb_->GetValue()) track.reverb = Reverb{room_->GetValue(), wet_->GetValue()};
    else track.reverb.reset();
}

}  // namespace svs
