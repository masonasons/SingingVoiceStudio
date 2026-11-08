#include "app/dialogs.h"

#include <algorithm>
#include <cmath>
#include <deque>

#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/tokenzr.h>

#include "app/studio.h"
#include "audio/decode.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/registry.h"
#include "voices/vocalwriter.h"

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

//: the most new notes Add word lays one word over
const int kMostWordNotes = 16;

//: Whether `field` holds a number from `lo` to `hi`, or is blank where
//: `blank` allows it. If not, it is said, and the field is given back to be
//: put right rather than the value quietly moved into range.
bool check_range(wxDialog *d, wxTextCtrl *field, const wxString &name, double lo, double hi, bool blank) {
    wxString t = field->GetValue();
    t.Trim(true).Trim(false);
    if (t.empty() && blank) return true;
    double v = 0;
    bool number = t.ToCDouble(&v);
    if (number && v >= lo && v <= hi) return true;
    wxString range = name + " goes from " + W(fmt_g(lo)) + " to " + W(fmt_g(hi));
    wxString said = !number ? range + "; type a number in that range."
                            : range + ", so " + t + " is too " + (v < lo ? "low." : "high.");
    wxMessageBox(said, d->GetTitle(), wxOK | wxICON_ERROR, d);
    field->SetFocus();
    field->SelectAll();
    return false;
}

//: The voices the engine and voice boxes offer: those that can sing here,
//: and `keep`, the one in use, so that it can still be seen and kept.
std::vector<VoiceListing> offered(const std::vector<VoiceListing> &all, int keep) {
    std::vector<VoiceListing> out;
    for (const VoiceListing &v : all)
        if (v.available || v.id == keep) out.push_back(v);
    return out;
}

//: Whether `field` holds a time signature, or is blank; if not, as check_range.
bool check_sig(wxDialog *d, wxTextCtrl *field) {
    wxString t = field->GetValue();
    t.Trim(true).Trim(false);
    if (t.empty() || parse_sig(U(t), {0, 0}).first) return true;
    wxMessageBox("A time signature is the beats to the bar over the note that has the beat, as 3/4 or 6/8: from 1 "
                 "to 32 over 1, 2, 4, 8, 16 or 32.",
                 d->GetTitle(), wxOK | wxICON_ERROR, d);
    field->SetFocus();
    field->SelectAll();
    return false;
}

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

namespace {

//: phonemes as a note shows them, a syllable break as a full stop
std::string spaced(const std::vector<std::string> &phonemes) {
    std::string out;
    for (const std::string &p : phonemes) out += (out.empty() ? "" : " ") + p;
    return out;
}

//: One word of Add text: the phonemes it sings and how many notes it goes on.
class TextWordDialog : public wxDialog {
public:
    TextWordDialog(wxWindow *parent, const TextWord &w, int notes, int most)
        : wxDialog(parent, wxID_ANY, W(w.word)) {
        wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
        phon_ = new wxTextCtrl(this, wxID_ANY, W(spaced(w.phonemes)), wxDefaultPosition, wxSize(260, -1));
        labelled(this, outer, "Phonemes", phon_, 1, "separated by spaces, a full stop between syllables");
        select_on_focus(phon_);
        count_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 1,
                                std::max(1, most), notes);
        labelled(this, outer, "Notes to use", count_);
        outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
        SetSizerAndFit(outer);
        phon_->SetFocus();
    }
    std::vector<std::string> phonemes() const {
        std::vector<std::string> out;
        wxStringTokenizer tok(phon_->GetValue(), " \t\r\n", wxTOKEN_STRTOK);
        while (tok.HasMoreTokens()) out.push_back(U(tok.GetNextToken()));
        return out;
    }
    int notes() const { return count_->GetValue(); }

private:
    wxTextCtrl *phon_;
    wxSpinCtrl *count_;
};

}  // namespace

AddWordDialog::AddWordDialog(wxWindow *parent, Studio *studio, int pitch, double beats)
    : wxDialog(parent, wxID_ANY, "Add word"), studio_(studio), pitch_(pitch) {
    build(beats);
}

AddWordDialog::AddWordDialog(wxWindow *parent, Studio *studio, std::vector<Note> existing, bool text,
                             std::vector<int> spots)
    : wxDialog(parent, wxID_ANY, text ? "Add text" : "Add word to existing note(s)"), studio_(studio),
      pitch_(kDefaultPitch), existing_(std::move(existing)), spots_(std::move(spots)), text_(text) {
    build(kDefaultBeats);
}

//: A preview still playing stops with the dialog.
AddWordDialog::~AddWordDialog() {
    if (playing_) studio_->stop_playing();
}

void AddWordDialog::build(double beats) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    if (text_) {
        // several lines, pasted at once; Enter looks it up, Shift+Enter
        // starts a new line
        word_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(380, 90),
                               wxTE_MULTILINE | wxTE_PROCESS_ENTER);
        word_->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &e) {
            int code = e.GetKeyCode();
            if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) && e.ShiftDown() && !e.ControlDown() &&
                !e.AltDown())
                word_->WriteText("\n");
            else
                e.Skip();
        });
        labelled(this, outer, "Text", word_, 1,
                 "words divided by spaces, a hyphen where a word's notes divide, Shift+Enter for a new line");
    } else {
        word_ = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(220, -1), wxTE_PROCESS_ENTER);
        labelled(this, outer, "Word", word_, 1, "a hyphen marks where the notes divide, as in dai-sy");
    }
    word_->Bind(wxEVT_TEXT_ENTER, &AddWordDialog::on_look, this);

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    wxButton *look = new wxButton(this, wxID_ANY, "&Look up");
    look->Bind(wxEVT_BUTTON, &AddWordDialog::on_look, this);
    row->Add(look, 0, wxALL, 3);
    // Add text's Preview is after the list instead
    if (!text_) {
        hear_ = new wxButton(this, wxID_ANY, "&Preview");
        hear_->Bind(wxEVT_BUTTON, &AddWordDialog::on_preview, this);
        hear_->Disable();
        row->Add(hear_, 0, wxALL, 3);
    }
    outer->Add(row, 0);

    count_ = new wxSpinCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(80, -1), wxSP_ARROW_KEYS, 1, 1, 1);
    count_->Bind(wxEVT_SPINCTRL, &AddWordDialog::on_count, this);
    // no hint here: a spin control's edit box takes its name from the
    // caption as written, so the name has to stand on its own
    labelled(this, outer, "Notes to use", count_);
    // not there until something is looked up: a number typed before then
    // went nowhere (show_count)
    outer->Hide(count_->GetContainingSizer());
    // the existing notes keep their own lengths
    if (existing_.empty()) {
        beats_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(beats)), wxDefaultPosition, wxSize(80, -1),
                                wxTE_PROCESS_ENTER);
        beats_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &e) { on_beats(e); });
        beats_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { on_beats(e); });
        labelled(this, outer, "Beats", beats_, 0, "for each note");
    }

    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(380, 140));
    if (text_) {
        // the words, each opened with Enter (edit_word)
        list_->InsertColumn(0, "Word", wxLIST_FORMAT_LEFT, 110);
        list_->InsertColumn(1, "Phonemes", wxLIST_FORMAT_LEFT, 190);
        list_->InsertColumn(2, "Notes", wxLIST_FORMAT_LEFT, 70);
    } else {
        list_->InsertColumn(0, "Phonemes", wxLIST_FORMAT_LEFT, 165);
        list_->InsertColumn(1, "Pitch", wxLIST_FORMAT_LEFT, 55);
        list_->InsertColumn(2, "Beats", wxLIST_FORMAT_LEFT, 150);
    }
    list_->OnActivated([this] {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->OnKey([this](wxKeyEvent &e) { on_key(e); });
    caption(this, outer, text_ ? "Words of this text" : "Notes for this word", list_);

    // the track with the text on it, from the word selected; Stop while it plays
    if (text_) {
        play_ = new wxButton(this, wxID_ANY, "&Preview");
        play_->Bind(wxEVT_BUTTON, &AddWordDialog::on_play, this);
        play_->Disable();
        outer->Add(play_, 0, wxALL, 6);
        play_timer_.SetOwner(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) {
            if (playing_ && !studio_->playing()) show_stopped();
        }, play_timer_.GetId());
    }

    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    ok_ = wxDynamicCast(FindWindow(wxID_OK), wxButton);
    if (ok_) ok_->Disable();
    SetSizerAndFit(outer);
    word_->SetFocus();
}

//: The word as typed, a curly apostrophe made a plain one.
std::string AddWordDialog::raw() const {
    std::string s = U(word_->GetValue());
    for (size_t at; (at = s.find("\xE2\x80\x99")) != std::string::npos;) s.replace(at, 3, "'");
    return s;
}

std::pair<std::string, int> AddWordDialog::typed() const {
    std::string raw = this->raw();
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
    for (size_t i = 0; i < raw.size(); ++i)
        if (in_word(raw, i)) clean.push_back(raw[i]);
    return {clean, parts};
}

//: The pieces the hyphens divide the word into, each but the last ending
//: in one, as a MIDI file's lyrics have them: "did-", "n't".
std::vector<std::string> AddWordDialog::pieces() const {
    std::vector<std::string> out(1);
    std::string raw = this->raw();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '-' && !out.back().empty()) out.emplace_back();
        else if (in_word(raw, i)) out.back().push_back(raw[i]);
    }
    if (out.back().empty()) out.pop_back();
    for (size_t i = 0; i + 1 < out.size(); ++i) out[i] += '-';
    return out;
}

void AddWordDialog::on_look(wxCommandEvent &) {
    if (text_) {
        std::vector<TextWord> words = text_words(raw());
        if (words.empty()) {
            studio_->say("type some words first");
            return;
        }
        std::vector<std::string> keys;
        for (const TextWord &w : words)
            if (std::find(keys.begin(), keys.end(), w.key()) == keys.end()) keys.push_back(w.key());
        studio_->say("looking up " + std::to_string(words.size()) + (words.size() == 1 ? " word" : " words"));
        auto alive = token();
        studio_->lookup(
            keys,
            [this, alive, words](std::map<std::string, std::vector<std::string>> res) {
                if (auto a = alive.lock(); a && *a) looked_text(words, std::move(res));
            },
            true);
        return;
    }
    auto [word, wanted] = typed();
    if (word.empty()) {
        studio_->say("type a word first");
        return;
    }
    studio_->say("looking up " + W(word));
    auto alive = token();
    std::string w = word;
    int n = wanted;
    // with the dictionary's syllables marked, for word_on_notes
    studio_->lookup(
        {word},
        [this, alive, w, n](std::map<std::string, std::vector<std::string>> res) {
            if (auto a = alive.lock(); a && *a) looked(w, n, res[w]);
        },
        true);
}

void AddWordDialog::looked(const std::string &word, int wanted, const std::vector<std::string> &phones) {
    if (phones.empty()) {
        studio_->say("no pronunciation for " + W(word) +
                     ". Add a note and type its phonemes instead.");
        return;
    }
    phonemes_ = phones;
    std::vector<std::string> sounds;
    for (const auto &p : phones)
        if (p != kSyllableBreak) sounds.push_back(p);
    int n = int(syllabify(sounds).size());
    // more notes than syllables as well, a vowel held over them; no more
    // than there are existing notes
    int most = existing_.empty() ? std::max(n, kMostWordNotes) : int(existing_.size());
    count_->SetRange(1, std::max(1, most));
    // one note per syllable unless the hyphens said otherwise
    count_->SetValue(std::min(wanted <= 1 ? n : wanted, std::max(1, most)));
    build_rows();
    hear_->Enable();
    if (ok_) ok_->Enable();
    show_count();
    std::string joined;
    for (const auto &p : sounds) joined += (joined.empty() ? "" : " ") + p;
    studio_->say(W(word + " is " + joined + ", " + std::to_string(n) + " syllable" +
                   (n == 1 ? "" : "s")));
    count_->SetFocus();
}

//: Add text's words looked up: Notes to use set to the notes they take, at
//: most those from the cursor on.
void AddWordDialog::looked_text(std::vector<TextWord> words, std::map<std::string, std::vector<std::string>> found) {
    std::vector<std::string> missing;
    for (TextWord &w : words) {
        w.phonemes = found[w.key()];
        if (w.phonemes.empty() && std::find(missing.begin(), missing.end(), w.word) == missing.end())
            missing.push_back(w.word);
    }
    text_looked_up(words);
    int want = 0;
    for (const TextWord &w : words) want += w.notes;
    words_ = std::move(words);
    int most = std::max(1, int(existing_.size()));
    count_->SetRange(1, most);
    count_->SetValue(std::min(want, most));
    build_rows();
    play_->Enable();
    if (ok_) ok_->Enable();
    show_count();
    std::string said = std::to_string(words_.size()) + (words_.size() == 1 ? " word" : " words") + " on " +
                       std::to_string(rows_.size()) + (rows_.size() == 1 ? " note" : " notes");
    if (want > most)
        said += ", of the " + std::to_string(want) + " the text takes: there are no more from the cursor on";
    if (!missing.empty()) {
        said += ". Not in the dictionary, sung on " + std::string(kDefaultPhoneme) + ":";
        for (size_t i = 0; i < missing.size(); ++i) said += (i ? ", " : " ") + missing[i];
    }
    studio_->say(W(said));
    if (playing_) start_play();
    count_->SetFocus();
}

//: A word of Add text opened: its phonemes and its notes. What Notes to use
//: has spread is kept as each word's own, and Notes to use becomes their sum.
void AddWordDialog::edit_word(int i) {
    if (i < 0 || i >= int(words_.size())) return;
    int most = std::max(1, int(existing_.size()));
    TextWord &w = words_[size_t(i)];
    TextWordDialog dlg(this, w, shares_[size_t(i)] > 0 ? shares_[size_t(i)] : w.notes, most);
    if (dlg.ShowModal() != wxID_OK) return;
    for (size_t k = 0; k < words_.size(); ++k)
        if (shares_[k] > 0) words_[k].notes = shares_[k];
    w.phonemes = dlg.phonemes();
    w.notes = dlg.notes();
    int want = 0;
    for (const TextWord &x : words_) want += x.notes;
    count_->SetValue(std::min(want, most));
    build_rows(i);
    studio_->say(W(w.word + ", " + (w.phonemes.empty() ? std::string(kDefaultPhoneme) : spaced(w.phonemes)) +
                   ", on " + std::to_string(w.notes) + (w.notes == 1 ? " note" : " notes") + "; Notes to use " +
                   std::to_string(count_->GetValue())));
    // a preview going on is heard again as it is now
    if (playing_) start_play();
}

//: Notes to use put in the dialog, the first time something is looked up.
void AddWordDialog::show_count() {
    wxSizer *row = count_->GetContainingSizer();
    if (!row || count_->IsShown()) return;
    GetSizer()->Show(row, true);
    GetSizer()->Fit(this);
}

double AddWordDialog::value_beats() const {
    double v;
    if (!beats_ || !read_number(beats_->GetValue(), &v)) return kDefaultBeats;
    return std::max(0.05, v);
}

void AddWordDialog::build_rows(int select) {
    if (text_) {
        shares_ = text_shares(words_, count_->GetValue());
        auto notes = text_on_notes(words_, shares_, studio_->plain_held());
        rows_.clear();
        for (size_t i = 0; i < notes.size() && i < existing_.size(); ++i) {
            Note n = existing_[i];
            n.phonemes = notes[i].first;
            n.word = notes[i].second;
            rows_.push_back(n);
        }
        sync(select);
        return;
    }
    std::string word = typed().first;
    auto groups = word_on_notes(phonemes_, pieces(), count_->GetValue(), studio_->plain_held());
    double b = value_beats();
    rows_.clear();
    for (size_t i = 0; i < groups.size(); ++i) {
        if (existing_.empty()) {
            rows_.push_back(Note(groups[i], pitch_, b, i == 0 ? word : ""));
            continue;
        }
        Note n = existing_[i];
        n.phonemes = groups[i];
        n.word = i == 0 ? word : "";
        rows_.push_back(n);
    }
    sync();
}

void AddWordDialog::sync(int select) {
    list_->DeleteAllItems();
    // Add text lists its words, Add word its notes
    for (size_t i = 0; text_ && i < words_.size(); ++i) {
        int k = i < shares_.size() ? shares_[i] : 0;
        list_->InsertItem(long(i), W(words_[i].word));
        list_->SetItem(long(i), 1, W(words_[i].phonemes.empty() ? "not found, " + std::string(kDefaultPhoneme)
                                                                 : spaced(words_[i].phonemes)));
        list_->SetItem(long(i), 2, k > 0 ? W(std::to_string(k)) : wxString("none, the notes ran out"));
    }
    for (size_t i = 0; !text_ && i < rows_.size(); ++i) {
        list_->InsertItem(long(i), W(rows_[i].label()));
        list_->SetItem(long(i), 1, W(pitch_name(rows_[i].pitch)));
        list_->SetItem(long(i), 2, W(fmt_g(rows_[i].beats)));
    }
    size_t shown = text_ ? words_.size() : rows_.size();
    if (shown > 0) {
        int i = std::min(std::max(select, 0), int(shown) - 1);
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
    if (!ready()) return;
    if (text_) {
        // the word selected stays so, for a preview going on
        build_rows(std::max(0, list_->FirstSelected()));
        // a whole text is too long to hear: where it gets to instead
        std::string last;
        for (const Note &n : rows_)
            if (!n.word.empty()) last = n.word;
        studio_->say(W(std::to_string(rows_.size()) + (rows_.size() == 1 ? " note" : " notes") + ", as far as " +
                       last));
        if (playing_) start_play();
        return;
    }
    build_rows();
    std::string all;
    for (const Note &n : rows_) all += (all.empty() ? "" : ", ") + n.text();
    studio_->say(W(std::to_string(rows_.size()) + " note" + (rows_.size() == 1 ? "" : "s") + ": " + all));
}

void AddWordDialog::on_beats(wxEvent &evt) {
    evt.Skip();
    if (!ready()) return;
    double b = value_beats();
    for (size_t i = 0; i < rows_.size(); ++i) {
        rows_[i].beats = b;
        refresh_row(int(i));
    }
}

void AddWordDialog::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    int i = list_->FirstSelected();
    if (text_) {
        if ((code == WXK_RETURN || code == WXK_NUMPAD_ENTER) && i >= 0) edit_word(i);
        else evt.Skip();
        return;
    }
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
    if (text_) {
        edit_word(i);
        return;
    }
    NoteDialog dlg(this, studio_, &rows_[size_t(i)]);
    if (dlg.ShowModal() != wxID_OK) return;
    rows_[size_t(i)] = dlg.result();
    sync(i);
}

void AddWordDialog::on_preview(wxCommandEvent &) {
    hear_->Disable();
    std::string all;
    for (const Note &n : rows_) all += (all.empty() ? "" : ", ") + n.text();
    studio_->say(text_ ? W("previewing the text") : "previewing " + W(all));
    auto alive = token();
    wxButton *b = hear_;
    studio_->preview(rows_, [alive, b]() {
        if (auto a = alive.lock(); a && *a) b->Enable();
    });
}

void AddWordDialog::on_play(wxCommandEvent &) {
    if (!playing_) {
        start_play();
        return;
    }
    stop_play();
    studio_->say("stopped");
}

//: The track alone with the text on it, from the first note of the word
//: selected, as the words are now.
void AddWordDialog::start_play() {
    int i = std::max(0, list_->FirstSelected());
    if (i >= int(words_.size()) || rows_.empty()) {
        stop_play();
        return;
    }
    // the notes the words before it take
    int first = 0;
    for (int k = 0; k < i && k < int(shares_.size()); ++k) first += shares_[size_t(k)];
    if (size_t(i) >= shares_.size() || shares_[size_t(i)] <= 0 || first >= int(spots_.size())) {
        stop_play();
        studio_->say(W(words_[size_t(i)].word + " has no notes: the notes ran out before it"));
        return;
    }
    int want = ++play_wanted_;
    play_timer_.Stop();
    playing_ = true;
    play_->SetLabel("&Stop");
    studio_->say(W("previewing from " + words_[size_t(i)].word));
    auto alive = token();
    studio_->preview_track(
        spots_, rows_, spots_[size_t(first)],
        [this, alive, want] {
            auto a = alive.lock();
            return a && *a && playing_ && play_wanted_ == want;
        },
        [this, alive, want](bool started) {
            auto a = alive.lock();
            if (!a || !*a || play_wanted_ != want) return;
            if (started) play_timer_.Start(100);
            else show_stopped();
        });
}

//: Stop the preview, and any render on its way.
void AddWordDialog::stop_play() {
    ++play_wanted_;
    if (playing_) studio_->stop_playing();
    show_stopped();
}

void AddWordDialog::show_stopped() {
    play_timer_.Stop();
    playing_ = false;
    if (play_) play_->SetLabel("&Preview");
}

// -- the two curves -------------------------------------------------------------------

//: What differs between editing a bend and editing the mod wheel: the words,
//: the range, and how a value is written.
const CurveKind kBendCurve = {"Pitch bend", "Bend point", "Bend points", "Semitones",
                              "-24 to 24, above or below the written pitch", -kMaxBend, kMaxBend, true,
                              "bend", " semitones", &Note::bend};
const CurveKind kModCurve = {"Mod wheel", "Mod point", "Mod wheel points", "Mod wheel",
                             "0 is at rest, 127 is the deepest vibrato", 0, 127, false, "mod wheel",
                             "", &Note::mod};

const CurveKind &control_curve(const std::string &key) {
    // made once, so the dialogs can hold on to them
    static std::deque<std::string> words;
    static std::deque<CurveKind> kinds;
    if (kinds.empty())
        for (size_t i = 0; i < kNoteControlCount; ++i) {
            const NoteControl &c = kNoteControls[i];
            words.push_back(std::string(c.label) + " point");
            const char *point = words.back().c_str();
            words.push_back(std::string(c.label) + " points");
            const char *list = words.back().c_str();
            words.push_back(std::string(c.label));
            for (char &ch : words.back()) ch = char(std::tolower((unsigned char)ch));
            kinds.push_back({c.label, point, list, c.label, c.hint, double(c.lo), double(c.hi), c.lo < 0,
                             words.back().c_str(), "", nullptr, c.key});
        }
    for (const CurveKind &k : kinds)
        if (key == k.control) return k;
    return kinds.front();
}

//: the curve a kind edits on a note
static Bend &curve_of(Note &n, const CurveKind &k) { return k.field ? n.*(k.field) : n.controls[k.control]; }
static Bend curve_in(const Note &n, const CurveKind &k) {
    if (k.field) return n.*(k.field);
    auto it = n.controls.find(k.control);
    return it == n.controls.end() ? Bend() : it->second;
}

static std::string value_text(const CurveKind &k, double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, k.signed_values ? "%+g" : "%g", std::nearbyint(v * 1000) / 1000);
    return buf;
}

std::string describe_controls(const Note &note) {
    std::string out;
    auto add = [&](const std::string &s) { out += (out.empty() ? "" : ", ") + s; };
    if (note.velocity) add("velocity " + std::to_string(note.velocity));
    for (const auto &c : note.controls) {
        if (c.second.empty()) continue;
        const CurveKind &k = control_curve(c.first);
        std::string s = k.said + std::string(" ") + value_text(k, c.second.front().second);
        if (c.second.size() > 1) s += " to " + value_text(k, c.second.back().second);
        add(s);
    }
    return out;
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
    Bind(wxEVT_BUTTON, &PointDialog::on_ok, this, wxID_OK);
    SetSizerAndFit(outer);
    select_on_focus(at_);
    select_on_focus(value_);
    at_->SetFocus();
}

void PointDialog::on_ok(wxCommandEvent &evt) {
    if (check_range(this, at_, "Position", 0, 100, false) &&
        check_range(this, value_, kind_.value_label, kind_.lo, kind_.hi, false))
        evt.Skip();
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
      points_(curve_in(note, kind)) {
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
    curve_of(n, kind_) = result();
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

// -- ControlsDialog --------------------------------------------------------------------

ControlsDialog::ControlsDialog(wxWindow *parent, Studio *studio, const Note &note, int voice)
    : wxDialog(parent, wxID_ANY, "VocalWriter controls"), studio_(studio), note_(note) {
    for (size_t i = 0; i < kNoteControlCount; ++i)
        if (control_reaches(kNoteControls[i].key, voice) || note.controls.count(kNoteControls[i].key)) rows_.push_back(i);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(380, 250));
    list_->InsertColumn(0, "Control", wxLIST_FORMAT_LEFT, 130);
    list_->InsertColumn(1, "On this note", wxLIST_FORMAT_LEFT, 230);
    list_->OnActivated([this] {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->OnKey([this](wxKeyEvent &e) { on_key(e); });
    caption(this, outer, "Controls, each holding until the next one, as in VocalWriter", list_);
    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    wxButton *edit = new wxButton(this, wxID_ANY, "&Edit control...");
    edit->Bind(wxEVT_BUTTON, &ControlsDialog::on_edit, this);
    row->Add(edit, 0, wxALL, 3);
    wxButton *clear = new wxButton(this, wxID_ANY, "C&lear control");
    clear->Bind(wxEVT_BUTTON, &ControlsDialog::on_clear, this);
    row->Add(clear, 0, wxALL, 3);
    outer->Add(row, 0);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    sync();
    list_->SetFocus();
}

void ControlsDialog::sync(int select) {
    list_->DeleteAllItems();
    for (size_t r = 0; r < rows_.size(); ++r) {
        const CurveKind &k = control_curve(kNoteControls[rows_[r]].key);
        Bend pts = curve_in(note_, k);
        std::string text;
        for (const auto &p : pts)
            text += (text.empty() ? "" : "; ") + value_text(k, p.second) + " at " +
                    fmt_g(std::nearbyint(p.first * 100 * 1000) / 1000) + " per cent";
        list_->InsertItem(long(r), W(k.title));
        list_->SetItem(long(r), 1, W(text.empty() ? std::string("none") : text));
    }
    if (rows_.empty()) return;
    list_->SelectRow(std::min(std::max(select, 0), int(rows_.size()) - 1));
    list_->FocusRow(std::min(std::max(select, 0), int(rows_.size()) - 1));
}

void ControlsDialog::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    wxCommandEvent e;
    if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) on_edit(e);
    else if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE) on_clear(e);
    else evt.Skip();
}

void ControlsDialog::on_edit(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0 || size_t(i) >= rows_.size()) return;
    const CurveKind &k = control_curve(kNoteControls[rows_[size_t(i)]].key);
    CurveDialog dlg(this, studio_, note_, k);
    if (dlg.ShowModal() != wxID_OK) return;
    Bend pts = dlg.result();
    if (pts.empty()) note_.controls.erase(k.control);
    else note_.controls[k.control] = pts;
    sync(i);
}

void ControlsDialog::on_clear(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0 || size_t(i) >= rows_.size()) return;
    const NoteControl &c = kNoteControls[rows_[size_t(i)]];
    note_.controls.erase(c.key);
    sync(i);
    studio_->say(W(std::string(c.label) + " cleared"));
}

// -- NoteDialog ------------------------------------------------------------------------

std::vector<Note> chord_rows(const Note &note, const std::vector<int> &pitches) {
    if (pitches.size() < 2) return {note};
    std::vector<Note> rows;
    for (size_t k = 0; k < pitches.size(); ++k) {
        Note n = note;
        n.pitch = pitches[k];
        if (k + 1 < pitches.size()) {
            n.hold = note.sounds();
            n.beats = 0;
            // the curves go on the last, which carries the time
            n.bend.clear();
            n.mod.clear();
            n.controls.clear();
        }
        rows.push_back(n);
    }
    return rows;
}

bool control_reaches(const std::string &key, int voice) {
    // the mix follows the volume where an engine does not
    if (key == "volume") return true;
    // the bend range and the rest's pitch are VocalWriter's sequencer's
    if (is_vocalwriter_voice(voice))
        return key != "sustain" && (vocalwriter_own_path() || (key != "bend_range" && key != "lead_pitch"));
    if (is_instrument(voice)) return key == "volume" || key == "sustain" || key == "bend_range" || key == "detune";
    return false;
}

std::string short_voice_name(const std::string &name, const std::string &engine) {
    // the engine's name, or its first word: "GM" of "GM Soundbank"
    for (std::string prefix : {engine, engine.substr(0, engine.find(' '))}) {
        prefix += " ";
        if (name.size() > prefix.size() && name.compare(0, prefix.size(), prefix) == 0)
            return name.substr(prefix.size());
    }
    return name;
}

NoteDialog::NoteDialog(wxWindow *parent, Studio *studio, const Note *note, const Note &seed)
    : wxDialog(parent, wxID_ANY, note ? "Edit note" : "Add note"),
      studio_(studio),
      note_(note ? *note : seed) {
    int voice = studio_->current_voice();
    played_ = is_instrument(voice);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    phon_ = new wxTextCtrl(this, wxID_ANY, W(note_.text()), wxDefaultPosition, wxSize(300, -1));
    labelled(this, outer, "Phonemes", phon_, 1,
             played_ ? "what a voice would sing; an instrument plays the note whatever they are"
                     : "separated by spaces");

    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    wxButton *ins = new wxButton(this, wxID_ANY, "&Insert phoneme...");
    ins->Bind(wxEVT_BUTTON, &NoteDialog::on_insert, this);
    row->Add(ins, 0, wxALL, 3);
    hear_ = new wxButton(this, wxID_ANY, "Preview &note");
    hear_->Bind(wxEVT_BUTTON, &NoteDialog::on_preview, this);
    row->Add(hear_, 0, wxALL, 3);
    outer->Add(row, 0);

    pitch_ = new wxTextCtrl(this, wxID_ANY, W(pitch_name(note_.pitch)), wxDefaultPosition, wxSize(played_ ? 160 : 90, -1));
    labelled(this, outer, "Pitch", pitch_, 0,
             played_ ? "a name like C4, or a MIDI number; several, as C4 E4 G4, make a chord"
                     : "a name like C4, or a MIDI number");
    beats_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(note_.beats)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Beats", beats_, 0,
             played_ ? "to the next note; 0 plays it with the next one, as a chord" : "");
    // VocalWriter keeps a velocity on each note, which only its voices and
    // instruments hear as more than loudness: greyed for another engine
    bool vocalwriter = uses_velocity(voice);
    velocity_ = new wxTextCtrl(this, wxID_ANY, note_.velocity ? W(std::to_string(note_.velocity)) : wxString(),
                               wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Velocity", velocity_, 0,
             "1 to 127, blank for the track's; VocalWriter's voices and the instruments only");
    enable_labelled(velocity_, vocalwriter);
    // how long an instrument holds it, if not its beats
    hold_ = new wxTextCtrl(this, wxID_ANY, note_.hold > 0 ? W(fmt_g(note_.hold)) : wxString(), wxDefaultPosition,
                           wxSize(90, -1));
    labelled(this, outer, "Sounds for", hold_, 0,
             "beats an instrument holds it, blank for its beats; the instruments only");
    enable_labelled(hold_, played_);
    auto ends = bend_ends(note_.bend);
    bend_start_ = new wxTextCtrl(this, wxID_ANY, W(bend_text(ends.first)), wxDefaultPosition, wxSize(90, -1));
    labelled(this, outer, "Bend at start", bend_start_, 0, "semitones, -24 to 24, blank for none");
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

    // and its controls, greyed the same way
    wxButton *controls = new wxButton(this, wxID_ANY, "VocalWriter &controls...");
    controls->Bind(wxEVT_BUTTON, &NoteDialog::on_controls, this);
    controls->Enable(vocalwriter);
    outer->Add(controls, 0, wxALL, 6);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    Bind(wxEVT_BUTTON, &NoteDialog::on_ok, this, wxID_OK);
    SetSizerAndFit(outer);
    phon_->SetFocus();
    // selected, so what is typed replaces the phonemes, as in a field tabbed to
    phon_->SelectAll();
}

void NoteDialog::on_ok(wxCommandEvent &evt) {
    if (check_range(this, bend_start_, "Bend at start", -kMaxBend, kMaxBend, true) &&
        check_range(this, bend_end_, "Bend at end", -kMaxBend, kMaxBend, true) &&
        check_range(this, mod_start_, "Mod at start", 0, 127, true) &&
        check_range(this, mod_end_, "Mod at end", 0, 127, true) &&
        (!velocity_->IsEnabled() || check_range(this, velocity_, "Velocity", 1, 127, true)))
        evt.Skip();
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

void NoteDialog::on_controls(wxCommandEvent &) {
    ControlsDialog dlg(this, studio_, result(), studio_->current_voice());
    if (dlg.ShowModal() != wxID_OK) return;
    note_.controls = dlg.result();
    std::string said = describe_controls(note_);
    studio_->say(W(said.empty() ? std::string("no controls") : said));
}

int NoteDialog::value_pitch() const {
    std::vector<int> all = pitches();
    return all.empty() ? kDefaultPitch : all.front();
}

std::vector<int> NoteDialog::pitches() const {
    std::vector<int> out;
    wxStringTokenizer tok(pitch_->GetValue(), " ,\t", wxTOKEN_STRTOK);
    while (tok.HasMoreTokens()) {
        try {
            out.push_back(std::max(0, std::min(127, parse_pitch(U(tok.GetNextToken())))));
        } catch (...) {
        }
        // only an instrument plays a chord
        if (!played_ && !out.empty()) break;
    }
    return out;
}

Note NoteDialog::result() {
    note_.phonemes.clear();
    wxStringTokenizer tok(phon_->GetValue(), " \t\r\n", wxTOKEN_STRTOK);
    while (tok.HasMoreTokens()) note_.phonemes.push_back(U(tok.GetNextToken()));
    note_.pitch = value_pitch();
    double b, h;
    note_.hold = played_ && read_number(hold_->GetValue(), &h) && h > 0 ? std::max(0.01, h) : 0.0;
    // 0 beats only for a note that sounds, as a chord's do
    if (read_number(beats_->GetValue(), &b)) note_.beats = b <= 0 && note_.hold > 0 ? 0.0 : std::max(0.05, b);
    if (std::fabs(note_.hold - note_.beats) < 1e-9) note_.hold = 0;
    note_.bend = with_ends(note_.bend, read_bend(bend_start_->GetValue()), read_bend(bend_end_->GetValue()));
    auto wheel = [](std::optional<double> v) -> std::optional<double> {
        if (!v) return v;
        return std::max(0.0, std::min(127.0, *v));
    };
    note_.mod = with_ends(note_.mod, wheel(read_bend(mod_start_->GetValue())), wheel(read_bend(mod_end_->GetValue())));
    double vel;
    note_.velocity = read_number(velocity_->GetValue(), &vel) ? std::max(0, std::min(127, int(std::lrint(vel)))) : 0;
    return note_;
}

// -- SongSettingsDialog ------------------------------------------------------------------

namespace {
wxSpinCtrl *spin(wxWindow *parent, int lo, int hi, int value, int width) {
    return new wxSpinCtrl(parent, wxID_ANY, "", wxDefaultPosition, wxSize(width, -1), wxSP_ARROW_KEYS, lo,
                          hi, value);
}

//: the song's end as bar and beat, "12 1"; empty when it has none
std::string end_text(double end, const Meter &meter) {
    if (end <= 0) return "";
    auto bb = meter.bar_and_beat(end);
    return std::to_string(bb.first) + " " + fmt_g(std::nearbyint(bb.second * 1000) / 1000);
}

//: "12 1", "12:1" or "12" back to beats from the start; 0 for empty
double parse_end(const wxString &text, const Meter &meter) {
    wxString t = text;
    t.Replace(":", " ");
    t.Trim(true).Trim(false);
    if (t.empty()) return 0;
    wxString rest;
    double bar = 0, beat = 1;
    if (!read_number(t.BeforeFirst(' ', &rest), &bar) || bar < 1) return 0;
    rest.Trim(false);
    if (!rest.empty() && !read_number(rest, &beat)) beat = 1;
    beat = std::max(1.0, beat);
    return meter.position(int(std::floor(bar)), beat);
}
}  // namespace

// -- PantheraDialog --------------------------------------------------------------

// -- CloseRestsDialog --------------------------------------------------------------------

CloseRestsDialog::CloseRestsDialog(wxWindow *parent, bool on, double beats, bool in_words)
    : wxDialog(parent, wxID_ANY, "Close short rests"), was_(beats) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    on_ = new wxCheckBox(this, wxID_ANY, "&Close short rests when importing MIDI");
    on_->SetValue(on);
    on_->SetToolTip("A MIDI file often leaves a little gap between two notes. With this on, a rest between two "
                    "notes no longer than the length below is taken out and the note before it held on, so the "
                    "line is sung legato. A VocalWriter song is made for VocalWriter already and keeps its "
                    "rests.");
    outer->Add(on_, 0, wxALL, 6);
    beats_ = new wxTextCtrl(this, wxID_ANY, W(fmt_g(beats)), wxDefaultPosition, wxSize(80, -1));
    labelled(this, outer, "Longest rest to close", beats_, 0, "in beats; 0.25 is a sixteenth note");
    enable_labelled(beats_, on);
    on_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { enable_labelled(beats_, on_->GetValue()); });
    // whatever the length, and with the box above ticked or not
    words_ = new wxCheckBox(this, wxID_ANY, "Always close a rest in the &middle of a word");
    words_->SetValue(in_words);
    words_->SetToolTip("A rest between two syllables of one word, as \"al-\" and \"right\" with a gap between "
                       "them, is taken out however long it is, the syllable before it held on.");
    outer->Add(words_, 0, wxALL, 6);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    on_->SetFocus();
}

double CloseRestsDialog::beats() const {
    double v = 0;
    return read_number(beats_->GetValue(), &v) && v > 0 ? v : was_;
}

PantheraDialog::PantheraDialog(wxWindow *parent, const std::string &folder)
    : wxDialog(parent, wxID_ANY, "Panthera folder") {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *about = new wxStaticText(
        this, wxID_ANY,
        "The MacinTalk voices are Apple's, run by Panthera. Leave this empty to find them where "
        "Panthera's NVDA add-on puts them. Otherwise name NVDA's configuration folder, or a folder "
        "holding panthera_host.exe and the tiger, leopard, snowleopard and lion folders.");
    about->Wrap(420);
    outer->Add(about, 0, wxALL, 8);
    path_ = new wxTextCtrl(this, wxID_ANY, W(folder), wxDefaultPosition, wxSize(420, -1));
    labelled(this, outer, "&Folder", path_, 1);
    wxButton *browse = new wxButton(this, wxID_ANY, "&Browse...");
    browse->Bind(wxEVT_BUTTON, &PantheraDialog::on_browse, this);
    outer->Add(browse, 0, wxALL, 6);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    path_->SetFocus();
}

void PantheraDialog::on_browse(wxCommandEvent &) {
    wxDirDialog pick(this, "Panthera folder", path_->GetValue(), wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (pick.ShowModal() == wxID_OK) path_->SetValue(pick.GetPath());
    path_->SetFocus();
}

std::string PantheraDialog::folder() const {
    wxString t = path_->GetValue();
    t.Trim(true).Trim(false);
    return U(t);
}

SoundbankDialog::SoundbankDialog(wxWindow *parent, const std::vector<SoundFontFile> &fonts,
                                 const std::string &default_bank)
    : wxDialog(parent, wxID_ANY, "Soundbank manager"), fonts_(fonts), default_(default_bank) {
    for (const SoundFontFile &f : fonts_) infos_.push_back(read_soundfont(f.path, f.slot));
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(520, 180));
    list_->InsertColumn(0, "SoundFont", wxLIST_FORMAT_LEFT, 150);
    list_->InsertColumn(1, "Sounds", wxLIST_FORMAT_LEFT, 90);
    list_->InsertColumn(2, "Voice numbers", wxLIST_FORMAT_LEFT, 100);
    list_->InsertColumn(3, "File", wxLIST_FORMAT_LEFT, 170);
    list_->OnSelected([this] { show_remove(); });
    list_->OnKey([this](wxKeyEvent &e) {
        if (e.GetKeyCode() == WXK_DELETE && !e.HasAnyModifiers() && list_->FirstSelected() >= 0) {
            wxCommandEvent c;
            on_remove(c);
            return;
        }
        e.Skip();
    });
    caption(this, outer, "Loaded SoundFonts", list_, 1);
    // Remove comes before Load, and only while there is something to remove
    remove_ = new wxButton(this, wxID_ANY, "&Remove");
    remove_->Bind(wxEVT_BUTTON, &SoundbankDialog::on_remove, this);
    outer->Add(remove_, 0, wxALL, 6);
    load_ = new wxButton(this, wxID_ANY, "&Load soundfont...");
    load_->Bind(wxEVT_BUTTON, &SoundbankDialog::on_load, this);
    outer->Add(load_, 0, wxALL, 6);
    bank_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(300, -1));
    bank_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        int i = bank_->GetSelection();
        default_ = i > 0 && size_t(i) <= fonts_.size() ? fonts_[size_t(i - 1)].path : std::string();
    });
    labelled(this, outer, "&Default soundbank", bank_, 0,
             "plays the parts of an imported MIDI file or VocalWriter song that are not sung");
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    fill(fonts_.empty() ? -1 : 0);
    SetSizerAndFit(outer);
    if (fonts_.empty()) load_->SetFocus();
    else list_->SetFocus();
}

//: The list and the default soundbank's choices, from fonts_, with the row
//: `select` selected.
void SoundbankDialog::fill(int select) {
    list_->DeleteAllItems();
    for (size_t i = 0; i < infos_.size(); ++i) {
        const SoundFontInfo &f = infos_[i];
        list_->InsertItem(long(i), W(f.name));
        if (f.error.empty()) {
            std::string sounds = std::to_string(f.presets.size());
            if (f.total > f.presets.size()) sounds += " of " + std::to_string(f.total);
            list_->SetItem(long(i), 1, W(sounds));
            int base = soundfont_base(f.slot);
            list_->SetItem(long(i), 2, W(std::to_string(base) + " - " + std::to_string(base + int(f.presets.size()) - 1)));
        } else {
            list_->SetItem(long(i), 1, "none");
            list_->SetItem(long(i), 2, W("cannot be read: " + f.error));
        }
        list_->SetItem(long(i), 3, W(f.path));
    }
    if (select >= 0 && select < list_->GetItemCount()) {
        list_->SelectRow(select);
        list_->FocusRow(select);
    }
    bank_->Clear();
    bank_->Append("GM Soundbank (built-in)");
    int chosen = 0;
    for (size_t i = 0; i < infos_.size(); ++i) {
        bank_->Append(W(infos_[i].name));
        if (fonts_[i].path == default_) chosen = int(i) + 1;
    }
    if (chosen == 0) default_.clear();
    bank_->SetSelection(chosen);
    show_remove();
    list_->Refresh();
}

void SoundbankDialog::show_remove() {
    bool any = list_->FirstSelected() >= 0;
    if (remove_->IsShown() == any) return;
    remove_->Show(any);
    Layout();
}

void SoundbankDialog::on_load(wxCommandEvent &) {
    wxFileDialog pick(this, "Load soundfont", "", "", "SoundFonts (*.sf2)|*.sf2|All files (*.*)|*.*",
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() != wxID_OK) return;
    std::string path = U(pick.GetPath());
    for (size_t i = 0; i < fonts_.size(); ++i)
        if (fonts_[i].path == path) {
            wxMessageBox(W(base_name(path) + " is loaded already."), "Load soundfont", wxOK | wxICON_INFORMATION, this);
            fill(int(i));
            list_->SetFocus();
            return;
        }
    // the first slot no other SoundFont has, so the numbers of the others stay
    int slot = 0;
    for (bool taken = true; taken; ++slot) {
        taken = false;
        for (const SoundFontFile &f : fonts_) taken = taken || f.slot == slot;
        if (!taken) break;
    }
    SoundFontInfo info = read_soundfont(path, slot);
    if (!info.error.empty()) {
        wxMessageBox(W(base_name(path) + " cannot be loaded: " + info.error + "."), "Load soundfont",
                     wxOK | wxICON_ERROR, this);
        return;
    }
    fonts_.push_back({path, slot});
    infos_.push_back(info);
    fill(int(fonts_.size()) - 1);
    list_->SetFocus();
}

void SoundbankDialog::on_remove(wxCommandEvent &) {
    int i = list_->FirstSelected();
    if (i < 0 || size_t(i) >= fonts_.size()) return;
    if (fonts_[size_t(i)].path == default_) default_.clear();
    fonts_.erase(fonts_.begin() + i);
    infos_.erase(infos_.begin() + i);
    fill(std::min(i, int(fonts_.size()) - 1));
    if (fonts_.empty()) load_->SetFocus();
    else list_->SetFocus();
}

std::string SoundbankDialog::default_soundbank() const { return default_; }

ImportPartsDialog::ImportPartsDialog(wxWindow *parent, const wxArrayString &parts)
    : wxDialog(parent, wxID_ANY, "Import MIDI") {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *about = new wxStaticText(
        this, wxID_ANY,
        "What should be imported? Space ticks or unticks a part. Every part, the first, ticks or unticks them "
        "all, so untick it to choose a few.");
    about->Wrap(380);
    outer->Add(about, 0, wxALL, 8);
    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(380, 220));
    list_->EnableChecks();
    list_->InsertColumn(0, "Part", wxLIST_FORMAT_LEFT, 350);
    list_->InsertItem(0, "Every part, one track each");
    for (size_t i = 0; i < parts.size(); ++i) list_->InsertItem(long(i + 1), parts[i]);
    for (int i = 0; i < list_->GetItemCount(); ++i) list_->SetChecked(i, true);
    list_->OnChecked([this](int row) { on_checked(row); });
    // Enter is OK, caught before the list takes it; not the list's
    // activation, which Windows also gives Space, the tick
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
        int code = evt.GetKeyCode();
        wxWindow *focus = wxWindow::FindFocus();
        bool in_list = focus && (focus == list_ || list_->IsDescendant(focus));
        if (!in_list || evt.HasAnyModifiers() || (code != WXK_RETURN && code != WXK_NUMPAD_ENTER)) {
            evt.Skip();
            return;
        }
        if (ready()) EndModal(wxID_OK);
    });
    caption(this, outer, "Parts", list_, 1);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    Bind(wxEVT_BUTTON, &ImportPartsDialog::on_ok, this, wxID_OK);
    SetSizerAndFit(outer);
    list_->SelectRow(0);
    list_->FocusRow(0);
    list_->SetFocus();
}

void ImportPartsDialog::on_checked(int row) {
    int n = list_->GetItemCount();
    if (row == 0) {
        bool on = list_->IsChecked(0);
        for (int i = 1; i < n; ++i) list_->SetChecked(i, on);
        return;
    }
    // every part is ticked exactly when all the others are
    bool all = true;
    for (int i = 1; i < n; ++i) all = all && list_->IsChecked(i);
    list_->SetChecked(0, all);
}

void ImportPartsDialog::on_ok(wxCommandEvent &evt) {
    if (ready()) evt.Skip();
}

bool ImportPartsDialog::ready() {
    if (!chosen().empty()) return true;
    wxMessageBox("No part is ticked. Tick one with Space, or Every part for them all.", GetTitle(),
                 wxOK | wxICON_ERROR, this);
    list_->SetFocus();
    return false;
}

std::vector<int> ImportPartsDialog::chosen() const {
    std::vector<int> out;
    for (int i = 1; i < list_->GetItemCount(); ++i)
        if (list_->IsChecked(i)) out.push_back(i - 1);
    return out;
}

LyricTrackDialog::LyricTrackDialog(wxWindow *parent, const wxArrayString &parts)
    : wxDialog(parent, wxID_ANY, "Import MIDI") {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *about = new wxStaticText(this, wxID_ANY,
                                           "The lyrics in this file could belong to any of these tracks. Which "
                                           "track are they sung on? That track is the one that sings.");
    about->Wrap(380);
    outer->Add(about, 0, wxALL, 8);
    choice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(300, -1), parts);
    labelled(this, outer, "Sung track", choice_, 1);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    // the first chosen, so a screen reader has a track to read, not a blank
    if (!parts.empty()) choice_->SetSelection(0);
    choice_->SetFocus();
}

SungPartsDialog::SungPartsDialog(wxWindow *parent, const wxArrayString &parts)
    : wxDialog(parent, wxID_ANY, "Import MIDI") {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *about = new wxStaticText(
        this, wxID_ANY,
        W(std::string("This file has no lyrics. Which tracks are sung? Space ticks or unticks a track. Each one "
                      "ticked sings ") +
          kDefaultPhoneme + " on every note until words are put on it; the others, and every track if none is "
                            "ticked, are played by the default soundbank."));
    about->Wrap(380);
    outer->Add(about, 0, wxALL, 8);
    list_ = new ReportList(this, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(380, 220));
    list_->EnableChecks();
    list_->InsertColumn(0, "Part", wxLIST_FORMAT_LEFT, 350);
    for (size_t i = 0; i < parts.size(); ++i) list_->InsertItem(long(i), parts[i]);
    // Enter is OK, as in ImportPartsDialog: Windows gives Space, the tick,
    // to the list's activation too
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &evt) {
        int code = evt.GetKeyCode();
        wxWindow *focus = wxWindow::FindFocus();
        bool in_list = focus && (focus == list_ || list_->IsDescendant(focus));
        if (!in_list || evt.HasAnyModifiers() || (code != WXK_RETURN && code != WXK_NUMPAD_ENTER)) {
            evt.Skip();
            return;
        }
        EndModal(wxID_OK);
    });
    caption(this, outer, "Sung tracks", list_, 1);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    if (!parts.empty()) {
        list_->SelectRow(0);
        list_->FocusRow(0);
    }
    list_->SetFocus();
}

std::vector<int> SungPartsDialog::chosen() const {
    std::vector<int> out;
    for (int i = 0; i < list_->GetItemCount(); ++i)
        if (list_->IsChecked(i)) out.push_back(i);
    return out;
}

SongSettingsDialog::SongSettingsDialog(wxWindow *parent, const Song &song, bool own_time)
    : wxDialog(parent, wxID_ANY, "Song settings"), was_sig_(song.sig), was_end_(song.end),
      end_shown_(W(end_text(song.end, song.meter()))) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    tempo_ = spin(this, int(kMinBpm), int(kMaxBpm), int(song.bpm), 80);
    labelled(this, outer, "Tempo", tempo_, 0, "beats per minute at the start; a controller changes it");
    sig_ = new wxTextCtrl(this, wxID_ANY, W(format_sig(song.sig)), wxDefaultPosition, wxSize(60, -1));
    labelled(this, outer, "Time signature", sig_, 0, "as 4/4, where the bars fall from the start; a controller changes it");
    end_ = new wxTextCtrl(this, wxID_ANY, end_shown_, wxDefaultPosition, wxSize(80, -1));
    labelled(this, outer, "Song end", end_, 0,
             "bar and beat, as 12 1, where an export stops; empty is a beat after the last note");
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
    if (own_time) {
        anticipate_->Disable();
        anticipate_->SetToolTip("VocalWriter's voices sing their consonants before the beat themselves, as "
                                "VocalWriter did; this is for the other engines.");
    }
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
    // left as shown, it keeps the end it had, which may lie between the
    // thousandths of a beat it is shown in
    song.end = end_->GetValue() == end_shown_ ? was_end_ : parse_end(end_->GetValue(), song.meter());
    song.consonants = consonants_->GetValue() / 100.0;
    for (const auto &vc : voice_ctrls_) song.voice.*(vc.first->field) = vc.second->GetValue();
    song.voice = clean_voice(song.voice);
    song.reverb = {room_->GetValue(), wet_->GetValue()};
    song.anticipate = anticipate_->GetValue();
}

// -- ControllerDialog --------------------------------------------------------------------

ControllerDialog::ControllerDialog(wxWindow *parent, Studio *studio, const Note &controller, int voice_before)
    : wxDialog(parent, wxID_ANY, "Controller"), note_(controller), voices_(studio->voice_list()) {
    outer_ = new wxBoxSizer(wxVERTICAL);
    // What it changes comes first, and only that one's fields follow it, so
    // Tab goes from the choice to them and on to OK.
    wxArrayString kinds;
    kinds.Add("Voice change");
    kinds.Add("Tempo change");
    kinds.Add("Time signature change");
    kinds.Add("Pan change");
    kind_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(220, -1), kinds);
    labelled(this, outer_, "Controller", kind_, 0, "what it changes from here on");

    // the voice as a track's is chosen: the engine, then its voice
    int start = controller.voice_id ? *controller.voice_id : voice_before;
    // an engine or voice that cannot sing here is left out
    voices_ = offered(voices_, start);
    wxArrayString engines;
    int engine = 0;
    for (const VoiceListing &v : voices_) {
        if (engine_names_.empty() || engine_names_.back() != v.family) {
            engine_names_.push_back(v.family);
            engines.Add(W(v.available ? v.family : v.family + " (not installed)"));
        }
        if (v.id == start) engine = int(engine_names_.size()) - 1;
    }
    wxBoxSizer *voice_group = new wxBoxSizer(wxHORIZONTAL);
    engine_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(170, -1), engines);
    if (!engines.empty()) engine_->SetSelection(engine);
    labelled(this, voice_group, "Engine", engine_);
    voice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(200, -1));
    labelled(this, voice_group, "Voice", voice_);
    outer_->Add(voice_group, 0, wxEXPAND);
    groups_.push_back(voice_group);
    fill_voices(start);
    engine_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { fill_voices(-1); });

    wxBoxSizer *tempo_group = new wxBoxSizer(wxVERTICAL);
    tempo_ = new wxTextCtrl(this, wxID_ANY, controller.tempo > 0 ? W(fmt_g(controller.tempo)) : wxString(),
                            wxDefaultPosition, wxSize(80, -1));
    labelled(this, tempo_group, "Tempo change (applies to the entire song)", tempo_, 0,
             "beats per minute from here on, 5 to 500");
    outer_->Add(tempo_group, 0, wxEXPAND);
    groups_.push_back(tempo_group);

    wxBoxSizer *sig_group = new wxBoxSizer(wxVERTICAL);
    sig_ = new wxTextCtrl(this, wxID_ANY, controller.sig ? W(format_sig(*controller.sig)) : wxString(),
                          wxDefaultPosition, wxSize(80, -1));
    labelled(this, sig_group, "Time signature change (applies to the entire song)", sig_, 0,
             "as 3/4 or 6/8, from here on; a new bar begins here");
    outer_->Add(sig_group, 0, wxEXPAND);
    groups_.push_back(sig_group);

    wxBoxSizer *pan_group = new wxBoxSizer(wxVERTICAL);
    pan_ = new wxTextCtrl(this, wxID_ANY, controller.pan ? W(std::to_string(*controller.pan)) : wxString(),
                          wxDefaultPosition, wxSize(80, -1));
    labelled(this, pan_group, "Pan", pan_, 0, "-100 (left) to 100 (right) from here on, whatever sings the track");
    outer_->Add(pan_group, 0, wxEXPAND);
    groups_.push_back(pan_group);

    // VocalWriter's controls, each from here on, as a note's own can be set
    // inside it; only VocalWriter's voices sing them, so they are greyed
    // where the track is sung by another engine
    for (size_t i = 0; i < kNoteControlCount; ++i) {
        const NoteControl &k = kNoteControls[i];
        // a rest's own: the pitch its next note's consonants are sung at
        if (std::string(k.key) == "lead_pitch") continue;
        auto it = controller.controls.find(k.key);
        bool has = it != controller.controls.end() && !it->second.empty();
        // only what the voice in force here sings; one a controller holds
        // already stays, to be changed or taken away
        if (!control_reaches(k.key, voice_before) && !has) continue;
        wxString shown = has ? W(fmt_g(it->second.front().second)) : wxString();
        wxBoxSizer *group = new wxBoxSizer(wxVERTICAL);
        wxTextCtrl *field = new wxTextCtrl(this, wxID_ANY, shown, wxDefaultPosition, wxSize(80, -1));
        std::string key = k.key, reach;
        if (key == "volume") reach = "; every voice and instrument";
        else if (key != "sustain")
            reach = std::string("; VocalWriter's voices only") + (control_reaches(key, kGmBase) ? ", and its instruments" : "");
        labelled(this, group, k.label, field, 0, W(std::string(k.hint) + reach));
        outer_->Add(group, 0, wxEXPAND);
        groups_.push_back(group);
        controls_.push_back({&k, field});
        kind_->Append(k.label);
    }
    // an existing controller shows what it changes
    int kind = controller.voice_id ? 0 : controller.tempo > 0 ? 1 : controller.sig ? 2 : controller.pan ? 3 : 0;
    bool other = controller.voice_id || controller.tempo > 0 || controller.sig || controller.pan;
    for (size_t i = 0; i < controls_.size() && !other; ++i)
        if (controller.controls.count(controls_[i].first->key)) {
            kind = 4 + int(i);
            break;
        }
    kind_->SetSelection(kind);
    kind_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { show_kind(); });
    outer_->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    Bind(wxEVT_BUTTON, &ControllerDialog::on_ok, this, wxID_OK);
    SetSizer(outer_);
    show_kind();
    kind_->SetFocus();
}

//: Only the field of the kind chosen is checked: the others are not there.
void ControllerDialog::on_ok(wxCommandEvent &evt) {
    int k = kind_->GetSelection();
    bool fine = true;
    if (k == 1) fine = check_range(this, tempo_, "Tempo", kMinBpm, kMaxBpm, true);
    else if (k == 2) fine = check_sig(this, sig_);
    else if (k == 3) fine = check_range(this, pan_, "Pan", -100, 100, true);
    else if (k >= 4 && size_t(k - 4) < controls_.size()) {
        const auto &c = controls_[size_t(k - 4)];
        fine = check_range(this, c.second, c.first->label, c.first->lo, c.first->hi, true);
    }
    if (fine) evt.Skip();
}

//: Only the fields of the kind chosen are there, and the window fits them.
void ControllerDialog::show_kind() {
    int k = kind_->GetSelection();
    for (size_t i = 0; i < groups_.size(); ++i) outer_->Show(groups_[i], int(i) == k, true);
    Layout();
    Fit();
}

//: The voice box for the engine chosen, `select` chosen in it if it is
//: one of its voices, and otherwise its first.
void ControllerDialog::fill_voices(int select) {
    voice_->Clear();
    ids_.clear();
    int e = engine_->GetSelection();
    if (e < 0 || size_t(e) >= engine_names_.size()) return;
    int at = 0;
    for (const VoiceListing &v : voices_) {
        if (v.family != engine_names_[size_t(e)]) continue;
        // "Betty" rather than "DECtalk Betty", as the track's voice box has it
        std::string name = short_voice_name(v.name, v.family);
        if (v.id == select) at = int(ids_.size());
        ids_.push_back(v.id);
        voice_->Append(W(name));
    }
    if (!ids_.empty()) voice_->SetSelection(at);
}

Note ControllerDialog::result() const {
    // one change to a controller: the kind chosen, and nothing else
    Note n = note_;
    n.tempo = 0;
    n.sig.reset();
    n.voice_id.reset();
    n.voice_name.reset();
    n.pan.reset();
    n.controls.clear();
    int k = kind_->GetSelection();
    if (k == 0) {
        int i = voice_->GetSelection();
        if (i >= 0 && i < int(ids_.size())) {
            n.voice_id = ids_[size_t(i)];
            n.voice_name = saved_voice_name(*n.voice_id, voices_);
        }
    } else if (k == 1) {
        double bpm = 0;
        if (read_number(tempo_->GetValue(), &bpm) && bpm > 0)
            n.tempo = std::max(kMinBpm, std::min(kMaxBpm, std::nearbyint(bpm * 1e3) / 1e3));
    } else if (k == 2) {
        Sig sig = parse_sig(U(sig_->GetValue()), {0, 0});
        if (sig.first) n.sig = sig;
    } else if (k == 3) {
        double pan = 0;
        if (read_number(pan_->GetValue(), &pan)) n.pan = std::max(-100, std::min(100, int(std::lrint(pan))));
    } else if (k >= 4 && size_t(k - 4) < controls_.size()) {
        const auto &c = controls_[size_t(k - 4)];
        double v = 0;
        if (read_number(c.second->GetValue(), &v))
            n.controls[c.first->key] = {{0.0, std::max(double(c.first->lo), std::min(double(c.first->hi), std::nearbyint(v)))}};
    }
    return n;
}

// -- MidiChannelsDialog -------------------------------------------------------------------

MidiChannelsDialog::MidiChannelsDialog(wxWindow *parent, const std::vector<const Track *> &tracks,
                                       const std::vector<int> &channels)
    : wxDialog(parent, wxID_ANY, "Set MIDI channels") {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxStaticText *about = new wxStaticText(
        this, wxID_ANY,
        "The MIDI channel each track goes out on. MIDI has sixteen, so tracks may share one; in General MIDI "
        "channel 10 is the drum kit's.");
    about->Wrap(380);
    outer->Add(about, 0, wxALL, 8);
    // scrolled, for a song of many tracks
    wxScrolledWindow *rows = new wxScrolledWindow(this, wxID_ANY);
    wxBoxSizer *inner = new wxBoxSizer(wxVERTICAL);
    wxArrayString numbers;
    for (int c = 1; c <= 16; ++c) numbers.Add(wxString::Format("%d", c));
    for (size_t k = 0; k < tracks.size(); ++k) {
        wxChoice *choice = new wxChoice(rows, wxID_ANY, wxDefaultPosition, wxSize(80, -1), numbers);
        choice->SetSelection(k < channels.size() ? std::max(0, std::min(15, channels[k])) : 0);
        labelled(rows, inner, W(tracks[k]->name), choice, 0, "MIDI channel");
        choices_.push_back(choice);
    }
    rows->SetSizer(inner);
    rows->SetScrollRate(0, 10);
    inner->Fit(rows);
    wxSize all = inner->GetMinSize();
    rows->SetMinSize(wxSize(all.x + 30, std::min(all.y, 420)));
    outer->Add(rows, 1, wxEXPAND | wxALL, 4);
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    if (!choices_.empty()) choices_[0]->SetFocus();
}

std::vector<int> MidiChannelsDialog::channels() const {
    std::vector<int> out;
    for (wxChoice *c : choices_) out.push_back(std::max(0, c->GetSelection()));
    return out;
}

// -- TrackDialog --------------------------------------------------------------------------

TrackDialog::TrackDialog(wxWindow *parent, Studio *studio, const Track &track)
    : wxDialog(parent, wxID_ANY, "Track"), studio_(studio) {
    for (const auto &v : voice_changes(track)) changes_to_vocalwriter_ = changes_to_vocalwriter_ || uses_velocity(v.second);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    name_ = new wxTextCtrl(this, wxID_ANY, W(track.name), wxDefaultPosition, wxSize(220, -1));
    labelled(this, outer, "Name", name_);

    // The engine first, then its voices: VocalWriter's bank in its own order,
    // the people's names first and then the instruments, which sing too; then
    // DECtalk's, the SSI-263's and Microsoft's. Choosing an engine fills the
    // voice box with that engine's voices alone, so neither list is long.
    // An engine or voice that cannot sing here is left out, unless the track
    // sings with it already.
    int current = studio->track_voice_of(track);
    for (const VoiceListing &v : offered(studio->voice_list(), current)) {
        if (engines_.empty() || engines_.back().name != v.family) engines_.push_back({v.family, v.available, {}});
        engines_.back().voices.push_back(v);
    }
    if (engines_.empty()) engines_.push_back({"VocalWriter", true, {{0, "Robert", "VocalWriter", true}}});
    size_t engine = 0;
    for (size_t e = 0; e < engines_.size(); ++e)
        for (const VoiceListing &v : engines_[e].voices)
            if (v.id == current) {
                engine = e;
                engines_[e].last = current;     // coming back to it finds it again
            }
    wxArrayString engine_names;
    for (const Engine &e : engines_) engine_names.Add(W(e.available ? e.name : e.name + " (not installed)"));
    wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
    engine_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(170, -1), engine_names);
    engine_->SetSelection(int(engine));
    labelled(this, row, "Engine", engine_);
    voice_ = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(200, -1));
    labelled(this, row, "Voice", voice_);
    outer->Add(row, 0, wxEXPAND);
    fill_voices(current);
    engine_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        fill_voices(-1);
        sync_velocity();
    });
    voice_->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        int i = voice_->GetSelection();
        if (i >= 0 && i < int(ids_.size())) engines_[size_t(std::max(0, engine_->GetSelection()))].last = ids_[size_t(i)];
    });

    volume_ = spin(this, 0, kMaxVolume, track.volume, 80);
    labelled(this, outer, "Volume", volume_, 0, "per cent");
    pan_ = spin(this, -100, 100, track.pan, 80);
    labelled(this, outer, "Pan", pan_, 0, "-100 far left to 100 far right");
    velocity_ = spin(this, 1, 127, track.velocity, 80);
    labelled(this, outer, "Velocity", velocity_, 0,
             "how hard the notes are sung or struck, by VocalWriter's voices and the instruments only; "
             "VocalWriter's songs used about 114");

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
    sync_velocity();

    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    SetSizerAndFit(outer);
    name_->SetFocus();
    name_->SetInsertionPointEnd();
}

//: The voice box for the engine chosen, with `select` chosen in it if it is
//: one of its voices, and otherwise the voice last chosen from this engine
//: while the dialog has been open, or its first.
void TrackDialog::fill_voices(int select) {
    int e = std::max(0, engine_->GetSelection());
    Engine &engine = engines_[size_t(e)];
    if (select < 0) select = engine.last;
    voice_->Clear();
    ids_.clear();
    int at = 0;
    for (const VoiceListing &v : engine.voices) {
        // The engine's name is in the box beside it, so it is not said again:
        // "Betty" rather than "DECtalk Betty". A voice that is nothing but the
        // engine's name -- the SSI-263's own -- keeps it.
        std::string name = short_voice_name(v.name, engine.name);
        if (v.id == select) at = int(ids_.size());
        ids_.push_back(v.id);
        voice_->Append(W(name));
    }
    voice_->SetSelection(at);
}

//: The velocity is VocalWriter's alone, its voices' and its instruments',
//: so it is greyed for another engine.
void TrackDialog::sync_velocity() {
    const Engine &e = engines_[size_t(std::max(0, engine_->GetSelection()))];
    // or one of VocalWriter's the track changes to further on
    enable_labelled(velocity_, changes_to_vocalwriter_ || (!e.voices.empty() && uses_velocity(e.voices.front().id)));
    // an instrument has no consonants and no voice to set up
    bool sings = e.voices.empty() || !is_instrument(e.voices.front().id);
    own_consonants_->Enable(sings);
    consonants_->Enable(sings && own_consonants_->GetValue());
    own_voice_->Enable(sings);
    for (auto &vc : voice_ctrls_) vc.second->Enable(sings && own_voice_->GetValue());
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
    track.voice_name = saved_voice_name(*track.voice_id, studio_->voice_list());
    track.volume = volume_->GetValue();
    track.pan = pan_->GetValue();
    // the notes' own velocities scaled with the track's, so they keep their
    // place against it: 49 on a track going from 50 to 100 becomes 98
    int was = track.velocity, now = velocity_->GetValue();
    if (was > 0 && now != was)
        for (Note &n : track.notes)
            if (n.velocity > 0) n.velocity = std::max(1, std::min(127, int(std::lrint(n.velocity * double(now) / was))));
    track.velocity = now;
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

// -- BackingDialog ------------------------------------------------------------------------

BackingDialog::BackingDialog(wxWindow *parent, const Track &track)
    : wxDialog(parent, wxID_ANY, "Backing track"), was_start_(track.audio_beat),
      start_shown_(W(fmt_g(std::nearbyint(track.audio_beat * 1000) / 1000))) {
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    name_ = new wxTextCtrl(this, wxID_ANY, W(track.name), wxDefaultPosition, wxSize(220, -1));
    labelled(this, outer, "Name", name_);
    path_ = new wxTextCtrl(this, wxID_ANY, W(track.audio), wxDefaultPosition, wxSize(420, -1));
    labelled(this, outer, "&File", path_, 1, "a WAV, MP3, FLAC or Ogg Vorbis file");
    wxButton *browse = new wxButton(this, wxID_ANY, "&Browse...");
    browse->Bind(wxEVT_BUTTON, &BackingDialog::on_browse, this);
    outer->Add(browse, 0, wxALL, 6);
    start_ = new wxTextCtrl(this, wxID_ANY, start_shown_, wxDefaultPosition, wxSize(80, -1));
    labelled(this, outer, "Starts at", start_, 0,
             "beats into the song the file begins, as a rest's length is written: 0 is the start, 1.25 a beat "
             "and a quarter in; below 0 leaves out that much of its opening");
    volume_ = spin(this, 0, kMaxVolume, track.volume, 80);
    labelled(this, outer, "Volume", volume_, 0, "per cent");
    pan_ = spin(this, -100, 100, track.pan, 80);
    labelled(this, outer, "Pan", pan_, 0, "-100 far left to 100 far right");
    outer->Add(buttons(this), 0, wxEXPAND | wxALL, 8);
    Bind(wxEVT_BUTTON, &BackingDialog::on_ok, this, wxID_OK);
    SetSizerAndFit(outer);
    name_->SetFocus();
    name_->SetInsertionPointEnd();
}

std::string BackingDialog::file() const {
    wxString t = path_->GetValue();
    t.Trim(true).Trim(false);
    return U(t);
}

void BackingDialog::on_browse(wxCommandEvent &) {
    std::string now = file();
    wxFileDialog pick(this, "Backing track", W(dir_name(now)), W(base_name(now)), kAudioWildcard,
                      wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (pick.ShowModal() == wxID_OK) path_->SetValue(pick.GetPath());
    path_->SetFocus();
}

//: A file that is not there, or is not a sound file, is said now, while it
//: can still be put right.
void BackingDialog::on_ok(wxCommandEvent &evt) {
    double v;
    std::string why;
    if (!file_exists(file())) {
        wxMessageBox(file().empty() ? wxString("Choose the file the backing track plays.")
                                    : W("There is no file " + file() + "."),
                     "Backing track", wxOK | wxICON_ERROR, this);
        path_->SetFocus();
        path_->SelectAll();
    } else if (!probe_audio(file(), &why)) {
        wxMessageBox(W(base_name(file()) + " cannot be played: " + why + "."), "Backing track",
                     wxOK | wxICON_ERROR, this);
        path_->SetFocus();
        path_->SelectAll();
    } else if (!read_number(start_->GetValue(), &v)) {
        wxMessageBox("Starts at is a number of beats into the song, such as 0, 1.25 or -2.",
                     "Backing track", wxOK | wxICON_ERROR, this);
        start_->SetFocus();
        start_->SelectAll();
    } else {
        evt.Skip();
    }
}

void BackingDialog::apply(Track &track) const {
    wxString name = name_->GetValue();
    name.Trim(true).Trim(false);
    track.name = name.empty() ? "Backing" : U(name);
    track.audio = file();
    // left as shown, it keeps the beat it had, which may lie between the
    // thousandths of a beat it is shown in
    double v = 0;
    if (start_->GetValue() == start_shown_) track.audio_beat = was_start_;
    else if (read_number(start_->GetValue(), &v)) track.audio_beat = std::nearbyint(v * 1e6) / 1e6;
    track.volume = volume_->GetValue();
    track.pan = pan_->GetValue();
}

}  // namespace svs
