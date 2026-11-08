#include "app/studio.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/progdlg.h>
#include <wx/textdlg.h>

#ifdef __WXMSW__
#include <windows.h>
#endif

#include "app/announce.h"
#include "app/crash.h"
#include "app/dialogs.h"
#include "audio/decode.h"
#include "audio/encode.h"
#include "core/midiexport.h"
#include "core/paths.h"
#include "core/vwsong.h"
#include "voices/controls.h"
#include "voices/registry.h"
#include "voices/soundfont.h"
#include "voices/vocalwriter.h"

namespace svs {

namespace {

const char *const kAppName = "Singing Voice Studio";

enum {
    ID_ADD_WORD = wxID_HIGHEST + 1, ID_ADD_NOTE, ID_EDIT, ID_REMOVE,
    ID_UP, ID_DOWN, ID_LONGER, ID_SHORTER, ID_OCTAVE_UP, ID_OCTAVE_DOWN, ID_DOUBLE, ID_HALVE,
    ID_PLAY, ID_HEAR, ID_STOP, ID_KEYS,
    ID_ADD_REST, ID_IMPORT, ID_EXPORT, ID_AUTO_PREVIEW, ID_PANTHERA, ID_EXPORT_TRACKS,
    ID_BAR_REST, ID_GOTO_BAR, ID_METRONOME, ID_PLAY_PAUSE, ID_PANES,
    ID_TRACK_NEW, ID_TRACK_BACKING, ID_TRACK_EDIT, ID_TRACK_REMOVE, ID_TRACK_MUTE, ID_TRACK_SOLO,
    ID_TRACK_UP, ID_TRACK_DOWN, ID_PREVIEW_TIMER, ID_RECOVERY_TIMER, ID_ADD_CONTROLLER, ID_CLOSE_RESTS,
    ID_REMOVE_VELOCITIES, ID_APPEND, ID_OWN_PATH, ID_PLAIN_HELD, ID_FOLLOW, ID_FOLLOW_TIMER, ID_EXPORT_TIMER,
    ID_EXPORT_MIDI, ID_ADD_WORD_EXISTING, ID_OCTAVE_INSTRUMENTS, ID_ADD_TEXT, ID_REMOVE_VOLUMES,
    ID_TRACK_TRANSPOSE_UP, ID_TRACK_TRANSPOSE_DOWN, ID_TRACK_OCTAVE_UP, ID_TRACK_OCTAVE_DOWN, ID_SOUNDBANKS,
    ID_NOTE_TO_REST, ID_TRACK_LOUDER, ID_TRACK_QUIETER, ID_TRACK_PAN_LEFT, ID_TRACK_PAN_RIGHT,
};

//: A beat is a quarter note, so a sixteenth is a quarter of a beat: the step
//: Alt with Left and Right moves a note by, the same step every time.
const double kSixteenth = 0.25;
//: A nudge will not take a note below a thirty-second note. The floor only
//: refuses; it never shortens the step to fit.
const double kMinBeats = 0.125;
//: the steps Ctrl+Shift and the arrows move a track's volume by, in per
//: cent, and its pan by
const int kVolumeStep = 1;
const int kPanStep = 1;
//: how long the song has to stop changing before a copy is kept
const int kRecoveryWait = 2500;
//: how long the nudging has to stop before a note is previewed
const int kPreviewWait = 180;
//: how often the cursors are moved on while the song plays and follows
const int kFollowStep = 40;
//: how often an export's progress window looks to see whether it was cancelled
const int kExportPoll = 200;

const int kMaxUndo = 100;
//: the most existing notes a word is offered, more than any word has syllables
const size_t kMaxWordNotes = 64;

//: The system's exclamation, so that a question is heard as well as read.
void exclaim() {
#ifdef __WXMSW__
    ::MessageBeep(MB_ICONEXCLAMATION);
#else
    wxBell();
#endif
}

//: The system's critical stop, so that a failure is heard as well as read.
void critical_stop() {
#ifdef __WXMSW__
    ::MessageBeep(MB_ICONHAND);
#else
    wxBell();
#endif
}

std::string plural(size_t n, const char *one, const char *many = nullptr) {
    return std::to_string(n) + " " + (n == 1 ? one : (many ? many : (std::string(one) + "s").c_str()));
}

std::string s_if(size_t n) { return n == 1 ? "" : "s"; }

std::string printf_s(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
std::string printf_s(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

std::string build_stamp() { return std::string("built ") + __DATE__; }

}  // namespace

// -- lengths ---------------------------------------------------------------------

std::string note_value(double beats) {
    static const std::vector<std::pair<double, const char *>> values = {
        {6.0, "dotted whole"},      {4.0, "whole"},          {3.0, "dotted half"},
        {2.0, "half"},              {1.5, "dotted quarter"}, {1.0, "quarter"},
        {0.75, "dotted eighth"},    {0.5, "eighth"},         {0.375, "dotted sixteenth"},
        {0.25, "sixteenth"},        {0.125, "thirty-second"}, {4.0 / 3.0, "triplet half"},
        {2.0 / 3.0, "triplet quarter"}, {1.0 / 3.0, "triplet eighth"}, {1.0 / 6.0, "triplet sixteenth"},
    };
    for (const auto &v : values)
        if (std::fabs(beats - v.first) < 1e-6) return v.second;
    return "";
}

std::string note_name(double beats) {
    std::string name = note_value(beats);
    if (name.empty()) return "";
    return std::string(strchr("aeiou", name[0]) ? "an " : "a ") + name + " note";
}

std::string beat_count(double n) {
    std::string said = note_name(n);
    return fmt_g(n) + " beat" + (n == 1 ? "" : "s") + (said.empty() ? "" : ", " + said);
}

std::string length_text(double beats) {
    std::string name = note_value(beats);
    return name.empty() ? fmt_g(beats) : fmt_g(beats) + ", " + name + " note";
}

std::string spoken_length(double beats, Sig sig) {
    double span = bar_beats(sig);
    int bars = int(std::floor((beats + 1e-6) / span));
    double remaining = std::nearbyint((beats - bars * span) * sig.second / 4.0 * 1e6) / 1e6;
    auto count = [](double value, const char *unit) {
        return (value == 1 ? std::string("one") : fmt_g(value)) + " " + unit + (value == 1 ? "" : "s");
    };
    if (bars) {
        std::string result = count(bars, "bar");
        if (remaining > 1e-6) result += " " + count(remaining, "beat");
        return result;
    }
    std::string name = note_value(beats);
    return name.empty() ? count(remaining, "beat") : name + " note";
}

std::optional<double> stepped_length(double beats, bool up) {
    double want = beats + (up ? kSixteenth : -kSixteenth);
    if (want < kMinBeats - 1e-9) return std::nullopt;
    return std::nearbyint(want * 1e6) / 1e6;
}

// -- the window --------------------------------------------------------------------

Studio::Studio(const std::string &path)
    : wxFrame(nullptr, wxID_ANY, kAppName, wxDefaultPosition, wxSize(720, 560)),
      preview_timer_(this, ID_PREVIEW_TIMER),
      recovery_timer_(this, ID_RECOVERY_TIMER),
      follow_timer_(this, ID_FOLLOW_TIMER),
      export_timer_(this, ID_EXPORT_TIMER) {
    song_ = fresh_song();
    Track first;
    first.name = "Voice 1";
    song_.tracks.push_back(first);
    settings_ = load_settings();
    // before anything asks the engines for their voices
    set_macintalk_folder(settings_.panthera_folder);
    set_vocalwriter_own_path(settings_.vocalwriter_own_path);
    set_gm_octaves(settings_.octave_instruments);
    set_soundfonts(settings_.soundfonts);
    mixer_ = std::make_shared<Mixer>();
    worker_ = std::make_unique<Worker>(this);
    worker_->on_error = [this](const std::string &msg) { say("engine error: " + W(msg)); };

    build();
    reset_history(true);
    build_menu();
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { preview_now(preview_row_); }, ID_PREVIEW_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { write_copy(); }, ID_RECOVERY_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { follow(); }, ID_FOLLOW_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { export_progress(-1); }, ID_EXPORT_TIMER);

    // The engines answer in order, so the cheap things are asked for first.
    worker_->ask<std::vector<VoiceListing>>([] { return Registry::get().voices(); },
                                            [this](std::vector<VoiceListing> v) { set_voices(v); });
    // Everything that asks the engines whether they can sing is asked here,
    // on the engines' own thread, and the answers kept: the window never asks
    // an engine anything itself, so it can never be caught waiting on one
    // that is in the middle of singing.
    worker_->ask<std::vector<std::string>>([] { return Registry::get().status(); },
                                           [this](std::vector<std::string> lines) { ready(lines); });
    worker_->ask<std::vector<int>>([] { return vocalwriter_program_voices(); },
                                   [this](std::vector<int> picks) { set_program_map(picks); });
    Bind(wxEVT_CLOSE_WINDOW, &Studio::on_close, this);
    // the list the keyboard was last in, to be given back when the program is
    // come back to (on_app_activate)
    Bind(wxEVT_CHILD_FOCUS, [this](wxChildFocusEvent &e) {
        e.Skip();
        wxWindow *w = e.GetWindow(), *list = nullptr;
        for (wxWindow *l : {static_cast<wxWindow *>(tracks_list_), static_cast<wxWindow *>(list_)})
            if (w && (w == l || l->IsDescendant(w))) list = l;
        // coming back, the system's first choice is not where it was left
        if (returning()) {
            if (list != return_to_) {
                CallAfter([this] { give_back_focus(); });
                return;
            }
            return_to_ = nullptr;
        }
        if (list) last_list_ = list;
    });
    wxTheApp->Bind(wxEVT_ACTIVATE_APP, &Studio::on_app_activate, this);
    Bind(wxEVT_ICONIZE, [this](wxIconizeEvent &e) {
        e.Skip();
        if (e.IsIconized()) left_list_ = last_list_;
        else come_back();
    });
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent &e) {
        e.Skip();
        if (e.GetActive() && returning()) CallAfter([this] { give_back_focus(); });
    });
    if (!path.empty()) {
        // a song named on the command line, or opened from the desktop
        std::string p = path;
        CallAfter([this, p] { open_project(p); });
    } else {
        // after the window is up, so the question has something to sit on
        CallAfter([this] { offer_recovery(); });
    }
}

Studio::~Studio() {
    wxTheApp->Unbind(wxEVT_ACTIVATE_APP, &Studio::on_app_activate, this);
    if (worker_) worker_->close();
}

//: Back in the program, the keyboard is back in the list it was in, not on
//: the window's first control, where the system would put it. Not while a
//: dialog of the program's is what is active: it keeps the keyboard.
void Studio::on_app_activate(wxActivateEvent &evt) {
    evt.Skip();
    if (!evt.GetActive()) left_list_ = last_list_;
    else come_back();
}

//: For a moment after coming back or a restore, the keyboard is sent back
//: to the list it was left in, however often the system moves it first.
void Studio::come_back() {
    if (!left_list_) return;
    return_to_ = left_list_;
    returned_at_ = wxGetLocalTimeMillis();
    CallAfter([this] { give_back_focus(); });
}

bool Studio::returning() {
    if (return_to_ && wxGetLocalTimeMillis() - returned_at_ > kReturnMs) return_to_ = nullptr;
    return return_to_ != nullptr;
}

void Studio::give_back_focus() {
    // a minimised window, or a dialog in front, is left to call again
    if (!returning() || !IsActive() || IsIconized()) return;
    wxWindow *f = wxWindow::FindFocus();
    if (f == return_to_ || (f && return_to_->IsDescendant(f))) {
        last_list_ = return_to_;
        return_to_ = nullptr;
        return;
    }
    return_to_->SetFocus();
}

void Studio::build() {
    wxPanel *p = new wxPanel(this);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);

    // The tracks come before the notes, so that Tab runs down the window in
    // the order the song is put together: which part, then what it sings.
    // Not single-select: several tracks are removed at once.
    tracks_list_ = new ReportList(p, wxLC_REPORT, wxSize(-1, 110));
    const std::pair<const char *, int> track_columns[] = {
        {"Track", 150}, {"Voice", 160}, {"Volume", 70}, {"Pan", 90}, {"State", 90}};
    int c = 0;
    for (const auto &col : track_columns) tracks_list_->InsertColumn(c++, col.first, wxLIST_FORMAT_LEFT, col.second);
    tracks_list_->OnActivated([this] {
        wxCommandEvent e;
        on_track_edit(e);
    });
    tracks_list_->OnSelected([this] { on_track_chosen(); });
    tracks_list_->OnKey([this](wxKeyEvent &e) { on_track_key(e); });
    // the Track menu, from the application key, Shift+F10 or a right click
    tracks_list_->OnContextMenu([this](wxPoint at) {
        std::unique_ptr<wxMenu> menu(track_menu());
        tracks_list_->PopupMenu(menu.get(), at);
    });
    caption(p, outer, "Tracks", tracks_list_, 0);

    // not single-select: a phrase is copied, transposed or deleted as a
    // whole, and Shift with the arrow keys is how that is reached
    list_ = new ReportList(p, wxLC_REPORT);
    const std::pair<const char *, int> columns[] = {{"Phonemes", 165}, {"Pitch", 55}, {"Beats", 150},
                                                    {"Word", 95},      {"Bend", 90},  {"Mod", 80},
                                                    {"Bar", 55},       {"Controls", 160}};
    c = 0;
    for (const auto &col : columns) list_->InsertColumn(c++, col.first, wxLIST_FORMAT_LEFT, col.second);
    list_->OnActivated([this] {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->OnKey([this](wxKeyEvent &e) { on_key(e); });
    // and the Note menu the same way on a note
    list_->OnContextMenu([this](wxPoint at) {
        std::unique_ptr<wxMenu> menu(note_menu());
        list_->PopupMenu(menu.get(), at);
    });
    caption(p, outer, "Notes", list_);

    messages_ = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 80),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    labelled(p, outer, "Messages", messages_, 1);

    p->SetSizer(outer);
    status_ = CreateStatusBar();
    sync_tracks(0);
    say("starting the engine");
}

//: The Note menu, as the menu bar has it and as a note's context menu has it.
wxMenu *Studio::note_menu() {
    wxMenu *note = new wxMenu();
    note->Append(ID_ADD_WORD, K("Add &word...\tCtrl+W"), "Look a word up and spread it over one note or several");
    note->Append(ID_ADD_WORD_EXISTING, K("Add word to ex&isting note(s)...\tCtrl+Shift+W"),
                 "Look a word up and put it on the notes already there, from the selected one on");
    note->Append(ID_ADD_TEXT, K("Add te&xt...\tCtrl+Shift+X"),
                 "Look a text up word by word and put it on the notes already there, from the selected one on");
    note->Append(ID_ADD_NOTE, K("Add &note...\tCtrl+N"), "Add a note and choose its phonemes");
    note->Append(ID_ADD_REST, K("Add &rest\tCtrl+R"), "Add a silent break of the same length");
    note->Append(ID_BAR_REST, K("Rest to the end of the &bar\tCtrl+Shift+R"),
                 "Add a rest long enough to reach the next bar line");
    note->Append(ID_ADD_CONTROLLER, K("Add &controller...\tCtrl+Shift+C"),
                 "A change from here on, taking no time: a tempo or time signature change applies to the entire song");
    note->Append(ID_EDIT, K("&Edit note...\tCtrl+E"), "Edit the selected note");
    note->Append(ID_NOTE_TO_REST, K("Convert note to rest\tCtrl+L"),
                 "Make the note the cursor is on a rest, as long and at the same pitch");
    note->AppendSeparator();
    // No tab, so no global accelerator: Ctrl+C belongs to whatever text field
    // has the focus. The notes list handles these keys itself.
    note->Append(wxID_CUT, K("Cu&t (Ctrl+X on a note)"), "Remove the selected notes and keep them to paste");
    note->Append(wxID_COPY, K("&Copy (Ctrl+C on a note)"), "Keep the selected notes to paste");
    note->Append(wxID_PASTE, K("&Paste (Ctrl+V on a note)"), "Put the kept notes in after the selection");
    note->Append(wxID_SELECTALL, K("Select &all (Ctrl+A on a note)"), "Select every note in the song");
    note->AppendSeparator();
    note->Append(ID_REMOVE, K("&Remove note\tCtrl+D"), "Remove the selected note");
    note->AppendSeparator();
    note->Append(ID_UP, K("Transpose &up (Alt+Up on a note)"), "Up one semitone");
    note->Append(ID_DOWN, K("Transpose &down (Alt+Down on a note)"), "Down one semitone");
    note->Append(ID_LONGER, K("&Longer\tAlt+Right"), "One sixteenth note longer");
    note->Append(ID_SHORTER, K("&Shorter\tAlt+Left"), "One sixteenth note shorter");
    note->Append(ID_OCTAVE_UP, K("Up an &octave (Alt+Shift+Up on a note)"), "Up twelve semitones");
    note->Append(ID_OCTAVE_DOWN, K("Down an octa&ve (Alt+Shift+Down on a note)"), "Down twelve semitones");
    note->Append(ID_DOUBLE, K("Twice as lon&g\tAlt+Shift+Right"), "Double the length");
    note->Append(ID_HALVE, K("&Half as long\tAlt+Shift+Left"), "Halve the length");
    return note;
}

//: The Track menu, as the menu bar has it and as a track's context menu has it.
wxMenu *Studio::track_menu() {
    wxMenu *track = new wxMenu();
    track->Append(ID_TRACK_NEW, K("&New track\tCtrl+T"), "Add another part, with its own voice");
    track->Append(ID_TRACK_BACKING, K("Add &backing track...\tCtrl+Shift+B"),
                  "A sound file played under the voices");
    track->Append(ID_TRACK_EDIT, K("&Track settings... (Enter on a track)"), "Name, voice, volume and pan");
    track->Append(ID_TRACK_REMOVE, K("&Remove track (Delete on a track)"),
                  "Remove the selected tracks and everything in them");
    track->AppendSeparator();
    // No tab, so no accelerator: S and M would be unusable in every text field.
    track->Append(ID_TRACK_MUTE, K("&Mute or unmute (M on a track)"), "Silence this track");
    track->Append(ID_TRACK_SOLO, K("&Solo or unsolo (S on a track)"), "Hear only the soloed tracks");
    track->AppendSeparator();
    track->Append(ID_TRACK_UP, K("Move track &up (Ctrl+Up on a track)"), "Put this part earlier in the list");
    track->Append(ID_TRACK_DOWN, K("Move track &down (Ctrl+Down on a track)"), "Put this part later in the list");
    track->AppendSeparator();
    // handled by the tracks list, as the notes list handles a note's
    track->Append(ID_TRACK_TRANSPOSE_UP, K("Transpose u&p (Alt+Up on a track)"),
                  "Every note of the selected tracks up one semitone");
    track->Append(ID_TRACK_TRANSPOSE_DOWN, K("Transpose do&wn (Alt+Down on a track)"),
                  "Every note of the selected tracks down one semitone");
    track->Append(ID_TRACK_OCTAVE_UP, K("Up an &octave (Alt+Shift+Up on a track)"),
                  "Every note of the selected tracks up twelve semitones");
    track->Append(ID_TRACK_OCTAVE_DOWN, K("Down an octa&ve (Alt+Shift+Down on a track)"),
                  "Every note of the selected tracks down twelve semitones");
    track->AppendSeparator();
    track->Append(ID_TRACK_LOUDER, K("&Louder (Ctrl+Shift+Up on a track)"),
                  W("The selected tracks' volume up " + std::to_string(kVolumeStep) + " per cent"));
    track->Append(ID_TRACK_QUIETER, K("&Quieter (Ctrl+Shift+Down on a track)"),
                  W("The selected tracks' volume down " + std::to_string(kVolumeStep) + " per cent"));
    track->Append(ID_TRACK_PAN_RIGHT, K("Pan ri&ght (Ctrl+Shift+Right on a track)"),
                  W("The selected tracks' pan " + std::to_string(kPanStep) + " to the right"));
    track->Append(ID_TRACK_PAN_LEFT, K("Pan l&eft (Ctrl+Shift+Left on a track)"),
                  W("The selected tracks' pan " + std::to_string(kPanStep) + " to the left"));
    return track;
}

// Menus rather than buttons: a screen reader announces an item's shortcut
// along with its name, so the keys are discoverable.
void Studio::build_menu() {
    wxMenuBar *bar = new wxMenuBar();

    wxMenu *note = note_menu();
    wxMenu *track = track_menu();

    wxMenu *play = new wxMenu();
    // Space is handled by the lists rather than registered as an
    // accelerator, or it could never be typed into a word again.
    mi_play_ = play->Append(ID_PLAY_PAUSE, K("Play or &stop (Space)"),
                            "Play from the note the cursor is on, or stop if it is going");
    // Handled directly rather than as accelerators, keeping the shortcut
    // visible in the label without a menu activation being announced. On a
    // Mac, Cmd+H hides the program, so hearing a note is Option+H there.
    play->Append(ID_PLAY, K("Play from the &start (Ctrl+P)"), "Sing every track from the beginning");
    play->Append(ID_HEAR, (is_mac() ? wxString("&Hear note (Option+H)") : K("&Hear note (Ctrl+H)")), "Sing the selected note");
    play->Append(ID_STOP, K("&Stop\tCtrl+."), "Stop playing");
    play->AppendSeparator();
    mi_metronome_ = play->AppendCheckItem(ID_METRONOME, K("&Metronome\tCtrl+M"),
                                          "Tick along with Play. Never written into an exported file.");

    wxMenu *go = new wxMenu();
    go->Append(ID_GOTO_BAR, K("Go to &bar...\tCtrl+G"), "Jump to the first note in a bar");
    go->Append(ID_PANES, K("&Tracks or notes\tF6"), "Move between the tracks list and the notes list");

    wxMenu *f = new wxMenu();
    f->Append(wxID_NEW, K("&New\tCtrl+Shift+N"), "Start an empty song");
    f->Append(wxID_OPEN, K("&Open project...\tCtrl+O"), "Reopen a song saved earlier");
    f->Append(wxID_SAVE, K("&Save project\tCtrl+S"), "Save the song");
    f->Append(wxID_SAVEAS, K("Save project &as..."), "Save the song under a new name");
    f->AppendSeparator();
    f->Append(ID_IMPORT, K("&Import MIDI or VocalWriter song...\tCtrl+I"),
              "Take the notes from a MIDI file, or a whole song from VocalWriter");
    f->Append(ID_APPEND, K("A&ppend MIDI...\tCtrl+Shift+A"),
              "Put a MIDI file's parts under the song's tracks, its tempo in place of the song's");
    f->Append(ID_EXPORT, K("&Export audio...\tCtrl+Shift+S"),
              "Write the whole song to one file: WAV, FLAC or Ogg Vorbis");
    f->Append(ID_EXPORT_TRACKS, K("Export &tracks...\tCtrl+Shift+T"), "Write every track to a file of its own");
    f->Append(ID_EXPORT_MIDI, K("Export &MIDI...\tCtrl+Shift+M"),
              "Write the song as a Standard MIDI File, the words and phonemes on its notes, as VocalWriter did");
    f->AppendSeparator();
    f->Append(wxID_PREFERENCES, K("&Song settings...\tCtrl+,"),
              "Tempo, time signature, consonants, reverb, voice controls");
    f->AppendSeparator();
    f->Append(wxID_EXIT, (is_mac() ? wxString("&Quit\tCtrl+Q") : K("E&xit\tAlt+F4")));

    wxMenu *help = new wxMenu();
    help->Append(ID_KEYS, K("&Keys\tF1"), "List every shortcut");

    wxMenu *edit = new wxMenu();
    mi_undo_ = edit->Append(wxID_UNDO, K("&Undo\tCtrl+Z"), "Undo the last song edit");
    mi_redo_ = edit->Append(wxID_REDO, K("&Redo\tCtrl+Shift+Z"), "Redo the last undone edit");
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { restore_history(false); }, wxID_UNDO);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { restore_history(true); }, wxID_REDO);
    Bind(wxEVT_UPDATE_UI, &Studio::on_update_history, this, wxID_UNDO);
    Bind(wxEVT_UPDATE_UI, &Studio::on_update_history, this, wxID_REDO);

    bar->Append(f, "&File");
    bar->Append(edit, "&Edit");
    bar->Append(track, "&Track");
    bar->Append(note, "&Note");
    bar->Append(play, "&Play");
    bar->Append(go, "&Go");
    wxMenu *prefs = new wxMenu();
    mi_auto_preview_ = prefs->AppendCheckItem(ID_AUTO_PREVIEW, K("&Preview notes as they change\tCtrl+Shift+P"),
                                              "Hear a note whenever its pitch or length is nudged");
    mi_auto_preview_->Check(settings_.auto_preview);
    prefs->Append(ID_CLOSE_RESTS, K("Close short &rests when importing MIDI...\tCtrl+Shift+I"),
                  "Take out the short rests between notes when a MIDI file comes in");
    mi_remove_velocities_ =
        prefs->AppendCheckItem(ID_REMOVE_VELOCITIES, K("Remove note &velocities from sung tracks when importing MIDI\tCtrl+Shift+E"),
                               "Give the sung parts of a MIDI file the track's velocity alone");
    mi_remove_velocities_->Check(settings_.remove_note_velocities);
    mi_remove_volumes_ = prefs->AppendCheckItem(
        ID_REMOVE_VOLUMES, K("Remove volume &changes from sung tracks when importing MIDI\tCtrl+Shift+L"),
        "Give the sung parts of a MIDI file the track's volume alone, without the swells on their notes");
    mi_remove_volumes_->Check(settings_.remove_volume_changes);
    mi_plain_held_ = prefs->AppendCheckItem(
        ID_PLAIN_HELD,
        K("Sing a vowel &held over several notes on its first sound when importing MIDI or adding a word\tCtrl+Shift+D"),
        "The vowel's whole sound on the last note alone: \"day\" over three notes is d EH, EH, EY rather than d EY, EY, "
        "EY; YU on the first alone, \"few\" as f YU, UW, UW");
    mi_plain_held_->Check(settings_.plain_held_vowels);
    mi_octave_instruments_ = prefs->AppendCheckItem(
        ID_OCTAVE_INSTRUMENTS,
        K("Compensate for the different &octave of certain instruments when using GM Soundbank during playback and "
          "export\tCtrl+Shift+O"),
        "The GM Soundbank plays programs 29 - 32, 34, 36, 37, 53, 80, 86, 96, 100 and 101 an octave up, and 98 and "
        "102 an octave down; the notes stay as they are, and SoundFonts play them as written");
    mi_octave_instruments_->Check(settings_.octave_instruments);
    mi_follow_ = prefs->AppendCheckItem(ID_FOLLOW, K("&Notes follow playback\tCtrl+Shift+F"),
                                        "While the song plays, every track's cursor moves to the note being sung, "
                                        "and stays there when it stops");
    mi_follow_->Check(settings_.follow_playback);
    mi_own_path_ = prefs->AppendCheckItem(ID_OWN_PATH, K("Sing VocalWriter's voices through its own &sequencer\tCtrl+Shift+V"),
                                          "Sample for sample as VocalWriter 2.0 sang them; otherwise phrase by phrase, "
                                          "as the other engines sing");
    mi_own_path_->Check(settings_.vocalwriter_own_path);
    prefs->Append(ID_PANTHERA, K("Panthera &folder..."), "Where to find Panthera, for the MacinTalk voices");
    prefs->Append(ID_SOUNDBANKS, K("Sound&bank manager...\tCtrl+Shift+K"),
                  "Load SoundFonts to play with, and choose what plays the parts of an imported song that are not sung");
    bar->Append(prefs, "&Settings");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
    Bind(wxEVT_CHAR_HOOK, &Studio::on_playback_shortcut, this);

    auto on = [this](int id, void (Studio::*fn)(wxCommandEvent &)) { Bind(wxEVT_MENU, fn, this, id); };
    on(ID_ADD_WORD, &Studio::on_add_word);
    on(ID_ADD_WORD_EXISTING, &Studio::on_add_word_existing);
    on(ID_ADD_TEXT, &Studio::on_add_text);
    on(ID_ADD_NOTE, &Studio::on_add_note);
    on(ID_ADD_REST, &Studio::on_add_rest);
    on(ID_NOTE_TO_REST, &Studio::on_note_to_rest);
    on(ID_BAR_REST, &Studio::on_bar_rest);
    on(ID_ADD_CONTROLLER, &Studio::on_add_controller);
    on(ID_GOTO_BAR, &Studio::on_goto_bar);
    on(ID_EDIT, &Studio::on_edit);
    on(wxID_CUT, &Studio::on_cut);
    on(wxID_COPY, &Studio::on_copy);
    on(wxID_PASTE, &Studio::on_paste);
    on(wxID_SELECTALL, &Studio::on_select_all);
    on(ID_REMOVE, &Studio::on_remove);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_pitch(kSemitone); }, ID_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_pitch(-kSemitone); }, ID_DOWN);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_length(true); }, ID_LONGER);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_length(false); }, ID_SHORTER);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_pitch(kOctave); }, ID_OCTAVE_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_pitch(-kOctave); }, ID_OCTAVE_DOWN);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { scale_length(2.0); }, ID_DOUBLE);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { scale_length(0.5); }, ID_HALVE);
    on(ID_METRONOME, &Studio::on_metronome);
    on(ID_TRACK_NEW, &Studio::on_track_new);
    on(ID_TRACK_BACKING, &Studio::on_track_backing);
    on(ID_TRACK_EDIT, &Studio::on_track_edit);
    on(ID_TRACK_REMOVE, &Studio::on_track_remove);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { toggle_track(false); }, ID_TRACK_MUTE);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { toggle_track(true); }, ID_TRACK_SOLO);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { move_tracks(-1); }, ID_TRACK_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { move_tracks(1); }, ID_TRACK_DOWN);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { transpose_tracks(kSemitone); }, ID_TRACK_TRANSPOSE_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { transpose_tracks(-kSemitone); }, ID_TRACK_TRANSPOSE_DOWN);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { transpose_tracks(kOctave); }, ID_TRACK_OCTAVE_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { transpose_tracks(-kOctave); }, ID_TRACK_OCTAVE_DOWN);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_tracks(kVolumeStep, 0); }, ID_TRACK_LOUDER);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_tracks(-kVolumeStep, 0); }, ID_TRACK_QUIETER);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_tracks(0, kPanStep); }, ID_TRACK_PAN_RIGHT);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { nudge_tracks(0, -kPanStep); }, ID_TRACK_PAN_LEFT);
    on(ID_PANES, &Studio::on_panes);
    on(ID_PLAY, &Studio::on_play);
    on(ID_PLAY_PAUSE, &Studio::on_play_stop);
    on(ID_HEAR, &Studio::on_hear);
    on(ID_STOP, &Studio::on_stop);
    on(ID_KEYS, &Studio::on_keys);
    on(wxID_NEW, &Studio::on_new);
    on(wxID_OPEN, &Studio::on_open);
    on(wxID_SAVE, &Studio::on_save);
    on(wxID_SAVEAS, &Studio::on_save_as);
    on(ID_IMPORT, &Studio::on_import);
    on(ID_APPEND, &Studio::on_append);
    on(ID_EXPORT, &Studio::on_export);
    on(ID_EXPORT_TRACKS, &Studio::on_export_tracks);
    on(ID_EXPORT_MIDI, &Studio::on_export_midi);
    on(wxID_PREFERENCES, &Studio::on_song_settings);
    on(ID_AUTO_PREVIEW, &Studio::on_auto_preview);
    on(ID_PANTHERA, &Studio::on_panthera);
    on(ID_SOUNDBANKS, &Studio::on_soundbanks);
    on(ID_CLOSE_RESTS, &Studio::on_close_rests);
    on(ID_REMOVE_VELOCITIES, &Studio::on_remove_velocities);
    on(ID_REMOVE_VOLUMES, &Studio::on_remove_volumes);
    on(ID_OWN_PATH, &Studio::on_own_path);
    on(ID_PLAIN_HELD, &Studio::on_plain_held);
    on(ID_OCTAVE_INSTRUMENTS, &Studio::on_octave_instruments);
    on(ID_FOLLOW, &Studio::on_follow);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
}

//: Ctrl+H and Ctrl+P (Option+H and Cmd+P on a Mac) played directly, without
//: a menu activation announcement.
void Studio::on_playback_shortcut(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    bool mac = is_mac();
    if (evt.ShiftDown() || (mac && evt.RawControlDown())) {
        evt.Skip();
        return;
    }
    // CmdDown is the Control key on Windows and Command on a Mac
    bool command = evt.CmdDown() && !evt.AltDown();
    bool hear = mac ? (evt.AltDown() && !evt.CmdDown()) : command;
    wxCommandEvent e;
    if (code == 'H' && hear) {
        on_hear(e);
        return;
    }
    if (code == 'P' && command) {
        on_play(e);
        return;
    }
    evt.Skip();
}

void Studio::on_song_settings(wxCommandEvent &) {
    undoable("song settings", [this] {
        // VocalWriter's sequencer places the consonants itself, so the
        // setting does nothing when every part is sung by it
        bool own_time = false;
        for (const Track &t : song_.tracks) {
            if (t.is_backing()) continue;
            Family *f = Registry::get().family_of(track_voice_of(t));
            own_time = f && f->sings_parts();
            if (!own_time) break;
        }
        SongSettingsDialog dlg(this, song_, own_time);
        if (dlg.ShowModal() != wxID_OK) return;
        Song was = song_;
        dlg.apply(song_);
        song_.bpm = std::max(kMinBpm, std::min(kMaxBpm, song_.bpm));
        bool changed = song_.bpm != was.bpm || song_.sig != was.sig || song_.consonants != was.consonants ||
                       song_.voice != was.voice || song_.reverb != was.reverb ||
                       song_.anticipate != was.anticipate || song_.end != was.end;
        if (changed) touch();
        // which bar a note falls in is read off the signature
        if (song_.sig != was.sig) sync(selection());
        say(W(printf_s("%d beats per minute, %s, %s beats to the bar, consonant length %d%%",
                       int(song_.bpm), format_sig(song_.sig).c_str(), fmt_g(bar_beats(song_.sig)).c_str(),
                       consonant_pct())));
        if (song_.voice != was.voice) say("voice controls set for every part that has not been given its own");
        if (song_.reverb != was.reverb)
            say(song_.reverb.second ? W(printf_s("reverb room %d, %d%% heard", song_.reverb.first,
                                                 song_.reverb.second))
                                    : wxString("reverb off"));
        if (song_.anticipate != was.anticipate)
            say(wxString("consonants ") + (song_.anticipate ? "before" : "after") + " the beat");
        if (song_.end != was.end) {
            auto bb = song_.meter().bar_and_beat(song_.end);
            say(song_.end > 0 ? W("the song ends at bar " + std::to_string(bb.first) + " beat " +
                                  fmt_g(std::nearbyint(bb.second * 1000) / 1000))
                              : wxString("the song ends a beat after its last note"));
        }
    });
}

//: A setting of the program, not of the song: saved at once.
void Studio::on_auto_preview(wxCommandEvent &) {
    bool on = mi_auto_preview_->IsChecked();
    settings_.auto_preview = on;
    bool kept = save_settings(settings_);
    say(wxString("previewing notes as they change is ") + (on ? "on" : "off") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: A setting of the program too: whether a MIDI import closes short rests.
void Studio::on_close_rests(wxCommandEvent &) {
    CloseRestsDialog dlg(this, settings_.close_rests, settings_.close_rests_beats, settings_.close_word_rests);
    if (dlg.ShowModal() != wxID_OK) return;
    settings_.close_rests = dlg.on();
    settings_.close_rests_beats = dlg.beats();
    settings_.close_word_rests = dlg.in_words();
    bool kept = save_settings(settings_);
    std::string words = settings_.close_word_rests ? ", and every rest in the middle of a word" : "";
    say(W((settings_.close_rests ? "importing MIDI closes the rests between notes up to " +
                                       beat_count(settings_.close_rests_beats) + " long" + words
                                 : settings_.close_word_rests ? "importing MIDI closes every rest in the middle "
                                                                "of a word, and keeps the others"
                                                              : std::string("importing MIDI keeps every rest")) +
          (kept ? "" : ", for this sitting only: the settings file could not be written")));
}

//: the longest rest a MIDI import closes, or 0 to keep them all
double Studio::rests_to_close() const { return settings_.close_rests ? settings_.close_rests_beats : 0.0; }

//: A setting of the program too: whether a MIDI import removes the notes'
//: own velocities from the sung tracks.
void Studio::on_remove_velocities(wxCommandEvent &) {
    bool on = mi_remove_velocities_->IsChecked();
    settings_.remove_note_velocities = on;
    bool kept = save_settings(settings_);
    say(wxString(on ? "importing MIDI removes the note velocities from the sung tracks"
                    : "importing MIDI keeps every note's velocity") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: A setting of the program too: whether a MIDI import removes the volume
//: changes from the sung tracks' notes.
void Studio::on_remove_volumes(wxCommandEvent &) {
    bool on = mi_remove_volumes_->IsChecked();
    settings_.remove_volume_changes = on;
    bool kept = save_settings(settings_);
    say(wxString(on ? "importing MIDI removes the volume changes from the sung tracks, leaving the track's volume"
                    : "importing MIDI keeps the volume changes on the sung tracks") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: A setting of the program too: whether the cursors follow the song as it plays.
void Studio::on_follow(wxCommandEvent &) {
    bool on = mi_follow_->IsChecked();
    settings_.follow_playback = on;
    bool kept = save_settings(settings_);
    if (on && following_ && player_.playing()) follow_timer_.Start(kFollowStep);
    if (!on) follow_timer_.Stop();
    say(wxString(on ? "the notes follow playback: every track's cursor moves to the note being sung"
                    : "the notes stay where they are while the song plays") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: Every track's cursor onto the row being sung, the notes list's too: the
//: last row starting at or before the beat the song has reached. When the
//: song stops, they stay where it got to.
void Studio::follow() {
    if (!following_ || !player_.playing()) {
        follow_timer_.Stop();
        return;
    }
    // the sound may begin a little before the beat it was played from
    double beat = play_clock_.beat_at(std::max(0.0, play_clock_.seconds(play_from_) + player_.seconds() - play_lead_));
    for (size_t k = 0; k < song_.tracks.size(); ++k) {
        const std::vector<Note> &all = song_.tracks[k].notes;
        if (all.empty()) continue;
        int row = 0;
        double at = 0;
        for (size_t i = 0; i < all.size() && at <= beat + 1e-9; ++i) {
            row = int(i);
            at += all[i].beats;
        }
        if (int(k) != current_) {
            song_.tracks[k].cursor = row;
            continue;
        }
        std::vector<int> rows = selected();
        if (rows.size() != 1 || rows[0] != row) select_only({row});
    }
}

//: A setting of the program too: whether a vowel a MIDI import or Add word
//: holds over several notes sings its first sound until the last of them.
void Studio::on_plain_held(wxCommandEvent &) {
    bool on = mi_plain_held_->IsChecked();
    settings_.plain_held_vowels = on;
    bool kept = save_settings(settings_);
    say(wxString(on ? "importing MIDI and adding a word sing a vowel held over several notes on its first sound "
                      "until the last of them"
                    : "importing MIDI and adding a word sing a vowel held over several notes whole on every one of "
                      "them") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: A setting of the program too: whether the GM Soundbank moves the
//: instruments it has an octave off by that octave (gm_octaves). Changed on
//: the engines' thread, between two renders.
void Studio::on_octave_instruments(wxCommandEvent &) {
    bool on = mi_octave_instruments_->IsChecked();
    settings_.octave_instruments = on;
    bool kept = save_settings(settings_);
    worker_->ask<bool>([on] {
        set_gm_octaves(on);
        return on;
    }, [](bool) {});
    say(wxString(on ? "the GM Soundbank plays the instruments it has an octave off where their notes are"
                    : "the GM Soundbank plays every instrument as it sounds in the soundbank") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: A setting of the program too: whether VocalWriter's voices are sung
//: through its own sequencer. Changed on the engines' thread, between two
//: renders, so no song is sung half one way and half the other.
void Studio::on_own_path(wxCommandEvent &) {
    bool on = mi_own_path_->IsChecked();
    settings_.vocalwriter_own_path = on;
    bool kept = save_settings(settings_);
    worker_->ask<bool>([on] {
        set_vocalwriter_own_path(on);
        return on;
    }, [](bool) {});
    say(wxString(on ? "VocalWriter's voices are sung through its own sequencer, as VocalWriter 2.0 sang them"
                    : "VocalWriter's voices are sung phrase by phrase, as the other engines sing") +
        (kept ? "" : ", for this sitting only: the settings file could not be written"));
}

//: Where Panthera is. MacinTalk looks again at once, and what it finds is
//: said: the voices it has now, or why it has none.
void Studio::on_panthera(wxCommandEvent &) {
    PantheraDialog dlg(this, settings_.panthera_folder);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string folder = dlg.folder();
    if (folder == settings_.panthera_folder) return;
    if (!folder.empty() && !dir_exists(folder)) {
        say(W("there is no folder called " + folder));
        return;
    }
    settings_.panthera_folder = folder;
    bool kept = save_settings(settings_);
    set_macintalk_folder(folder);
    // phrases already sung by MacinTalk may have been sung by another copy
    mixer_ = std::make_shared<Mixer>();
    worker_->ask<std::vector<VoiceListing>>([] { return Registry::get().voices(); },
                                            [this](std::vector<VoiceListing> v) { set_voices(v); });
    worker_->ask<std::vector<std::string>>([] { return Registry::get().status(); },
                                           [this, kept](std::vector<std::string> lines) {
                                               status_lines_ = lines;
                                               for (const std::string &line : lines)
                                                   if (line.rfind("MacinTalk", 0) == 0) say(W(line));
                                               if (!kept)
                                                   say("for this sitting only: the settings file could not be written");
                                           });
}

//: The Soundbank manager. The SoundFonts kept are read again and their
//: voices listed, and the phrases already played are played afresh.
void Studio::on_soundbanks(wxCommandEvent &) {
    SoundbankDialog dlg(this, settings_.soundfonts, settings_.default_soundbank);
    if (dlg.ShowModal() != wxID_OK) return;
    std::vector<SoundFontFile> fonts = dlg.fonts();
    std::string bank = dlg.default_soundbank();
    if (fonts == settings_.soundfonts && bank == settings_.default_soundbank) return;
    settings_.soundfonts = fonts;
    settings_.default_soundbank = bank;
    bool kept = save_settings(settings_);
    set_soundfonts(fonts);
    mixer_ = std::make_shared<Mixer>();
    std::string said = plural(fonts.size(), "SoundFont") + " loaded; imported parts that are not sung are played by " +
                       (bank.empty() ? std::string("the GM Soundbank") : strip_extension(base_name(bank)));
    if (!kept) said += ", for this sitting only: the settings file could not be written";
    say(W(said));
    worker_->ask<std::vector<VoiceListing>>([] { return Registry::get().voices(); },
                                            [this](std::vector<VoiceListing> v) { set_voices(v); });
    worker_->ask<std::vector<std::string>>([] { return Registry::get().status(); },
                                           [this](std::vector<std::string> lines) { status_lines_ = lines; });
}

//: The SoundFont that plays the parts an import does not sing, by its slot,
//: or -1 for the GM Soundbank: the default soundbank, if it can play here.
int Studio::soundbank_slot() {
    if (settings_.default_soundbank.empty()) return -1;
    int slot = soundfont_slot(settings_.default_soundbank);
    for (const VoiceListing &v : voices_)
        if (slot >= 0 && v.id == soundfont_base(slot) && v.available) return slot;
    say(W("the default soundbank, " + strip_extension(base_name(settings_.default_soundbank)) +
          ", cannot play here, so the GM Soundbank plays instead"));
    return -1;
}

//: "the GM Soundbank", or the SoundFont in `slot` by its name.
std::string Studio::soundbank_name(int slot) const {
    for (const VoiceListing &v : voices_)
        if (slot >= 0 && v.id == soundfont_base(slot)) return v.family;
    return "the GM Soundbank";
}

//: Hear one note once the nudging has stopped: a held arrow key would
//: otherwise queue a stream of little sounds, each already out of date.
void Studio::preview_note(int i) {
    if (!settings_.auto_preview || rendering_) return;
    preview_row_ = i;
    preview_timer_.StartOnce(kPreviewWait);
}

//: A note heard on its own: a chord's, which takes no time, as long as it is held.
static Note alone(Note n) {
    if (n.hold > 0) {
        n.beats = n.hold;
        n.hold = 0;
    }
    return n;
}

void Studio::preview_now(int i) {
    if (i < 0 || i >= int(notes().size()) || rendering_) return;
    const Note &note = notes()[size_t(i)];
    if (note.phonemes.empty() || note.controller) return;   // nothing to hear
    stop_audio();
    rendering_ = true;
    mix(request(new std::vector<Note>{alone(note)}), [this](Mixed res) { heard(res); });
}

void Studio::on_keys(wxCommandEvent &) {
    const char *lines[] = {
        "Ctrl+Z  undo, Ctrl+Shift+Z  redo",
        "F6  move between the tracks list and the notes list",
        "Ctrl+T  add a track. Each track has its own voice, volume, pan and notes",
        "Ctrl+Shift+B  add a backing track: a sound file played under the voices",
        "Enter on a track  its name, voice, volume and pan, or a backing track's file and where it starts",
        "M on a track  mute it, S  solo it",
        "Delete on a track  remove it, or every selected track: Shift with the arrow keys selects more than one",
        "Ctrl+Up or Ctrl+Down on a track  reorder the parts",
        "Alt+Up or Alt+Down on a track  transpose every note of the selected tracks a semitone, Alt+Shift+Up or "
        "Alt+Shift+Down an octave",
        "Ctrl+Shift+Up or Ctrl+Shift+Down on a track  the volume of the selected tracks 1 per cent up or down, "
        "Ctrl+Shift+Right or Ctrl+Shift+Left  their pan 1 to the right or left",
        "Ctrl+A on a track  select every track",
        "Ctrl+W  add a word after the selected note, or at the end",
        "Ctrl+Shift+W  add a word to the notes already there, from the selected one on",
        "Ctrl+Shift+X  add a text, words divided by spaces, to the notes already there, from the selected one on",
        "Ctrl+N  add a note after the selected note, or at the end",
        "Ctrl+R  add a rest, a silent break",
        "Ctrl+Shift+R  rest to the end of the bar",
        "Ctrl+Shift+C  add a controller: a change from there on, such as a tempo or time signature change, "
        "which applies to the entire song whichever track it is on",
        "Ctrl+G  go to a bar",
        "Ctrl+E  edit note",
        "Ctrl+L  make the note the cursor is on a rest, as long and at the same pitch",
        "Application key or Shift+F10  the Track menu on a track, the Note menu on a note",
        "Ctrl+D or Delete  remove note",
        "Ctrl+C, Ctrl+X, Ctrl+V  copy, cut and paste notes",
        "Ctrl+A  select every note",
        "Ctrl+Up or Ctrl+Down  move a note earlier or later",
        "Ctrl+comma  song settings: tempo, time signature, consonant length, the reverb, and the "
        "voice controls every part follows unless it has its own",
        "Ctrl+Shift+P  hear a note whenever it is nudged",
        "Shift with the arrow keys selects more than one note",
        "Alt+Up or Alt+Down  transpose a semitone",
        "Alt+Right or Alt+Left  a sixteenth note longer or shorter",
        "Alt+Shift+Up or Alt+Shift+Down  transpose an octave",
        "Alt+Shift+Right or Alt+Shift+Left  twice or half as long",
        "Pitch bend and the mod wheel are set per note, in the note editor. The mod wheel deepens "
        "the vibrato",
        "On a track an instrument plays, a chord is its notes in a row, all but the last taking 0 "
        "beats; several pitches typed into a note, as C4 E4 G4, make one. Alt+Right and Alt+Left "
        "change how long a note held on its own sounds, and Ctrl+H plays the note the cursor is on",
        "Space  play from the note the cursor is on, or stop if it is playing",
        "Ctrl+P  play every track from the start",
        is_mac() ? "Option+H  hear the selected note" : "Ctrl+H  hear the selected note",
        "Ctrl+M  metronome on or off, for playing only; while the song plays it comes in or goes at once",
        "Ctrl+. stop",
        "Ctrl+Shift+N  start an empty song",
        "Ctrl+O open a project, Ctrl+S save it",
        "Ctrl+,  song settings: tempo, time signature, consonants, reverb, voice controls",
        "Ctrl+I  import a MIDI file or a VocalWriter song. The parts without words are played by "
        "the default soundbank: the GM Soundbank, VocalWriter's own instruments, as VocalWriter played them, "
        "unless a SoundFont is chosen in the Soundbank manager",
        "Ctrl+Shift+K  the Soundbank manager: load SoundFonts, whose instruments then play as engines of their "
        "own, and choose the default soundbank",
        "Ctrl+Shift+A  append a MIDI file: its parts under the song's tracks, its tempo and time signature in "
        "place of the song's",
        "Ctrl+Shift+I  whether importing MIDI closes the short rests between notes",
        "Ctrl+Shift+E  whether importing MIDI removes the note velocities from the sung tracks",
        "Ctrl+Shift+L  whether importing MIDI removes the volume changes from the sung tracks",
        "Ctrl+Shift+D  whether importing MIDI and adding a word sing a vowel held over several notes on its "
        "first sound until the last of them: day over three notes as d EH, EH, EY",
        "Ctrl+Shift+O  whether the GM Soundbank makes good the octave its instruments for a few programs are off "
        "by, in playing and exporting; the notes stay as they are",
        "Ctrl+Shift+F  whether every track's cursor follows the note being sung while the song plays, staying "
        "where it got to when it stops",
        "Ctrl+Shift+V  whether VocalWriter's voices are sung through its own sequencer, sample for sample as "
        "VocalWriter 2.0 sang them, or phrase by phrase as the other engines sing",
        "Ctrl+Shift+S  export the whole song as one WAV, FLAC or Ogg Vorbis file",
        "Ctrl+Shift+T  export every track to its own WAV",
        "Ctrl+Shift+M  export the song as a MIDI file, as VocalWriter did: the channels are asked first",
        "Enter on a note edits it",
    };
    // In a window of their own, where the focus goes, so they are read the
    // moment F1 is pressed. They used to go into Messages alone, which left
    // nothing to hear: the focus stayed in the list and F1 seemed to do
    // nothing at all.
    wxString text;
    for (const char *line : lines) text += K(line) + "\n";
    wxDialog dlg(this, wxID_ANY, "Keys", wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);
    wxTextCtrl *list = new wxTextCtrl(&dlg, wxID_ANY, text, wxDefaultPosition, wxSize(560, 360),
                                      wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    labelled(&dlg, outer, "Keys", list, 1);
    wxButton *close = new wxButton(&dlg, wxID_CANCEL, "Close");
    outer->Add(close, 0, wxALIGN_RIGHT | wxALL, 8);
    dlg.SetEscapeId(wxID_CANCEL);
    dlg.SetSizerAndFit(outer);
    list->SetFocus();
    list->SetInsertionPoint(0);
    dlg.ShowModal();
}

void Studio::say(const wxString &text) {
    crash_note(U(text));
    status_->SetStatusText(text);
    messages_->AppendText(text + "\n");
}

//: Speak a shortcut's result after updating its row, keeping focus.
void Studio::announce_note(const wxString &text, ReportList *control, int row) {
    say(text);
    if (announce(control, text)) return;
    reannounce(control, row);
}

void Studio::ready(const std::vector<std::string> &lines) {
    status_lines_ = lines;
    size_t ready_count = 0;
    for (const auto &v : voices_) ready_count += v.available ? 1 : 0;
    say(W("engines ready, " + std::to_string(ready_count) + " voices"));
    for (const std::string &line : lines) say(W(line));
    for (const std::string &report : unseen_crash_reports())
        say(W("the program crashed last time, and wrote down where: " + report +
              ". Sending that file along with what you were doing will help find it."));
    say(W("this is build " + build_stamp()));
    VwAssets &a = VwAssets::get();
    if (a.ok() && !a.has_bank())
        // Without GMBank the voices built on its wavetables cannot be chosen,
        // so name the missing file rather than letting someone find out.
        say("the instrument bank, GMBank.rsrc, is missing: the VocalWriter voices with instrument "
            "names cannot be used, and the GM Soundbank cannot play");
    if (!Lexicon::get().ok())
        say("VocalWriter's dictionary is not here, so Add word cannot look words up. Phonemes can "
            "still be typed into a note by hand.");
    size_t n = singable().size();
    say(W(std::to_string(n) + " phonemes available"));
}

void Studio::set_program_map(const std::vector<int> &picks) {
    program_map_.clear();
    for (size_t p = 0; p < picks.size(); ++p)
        if (picks[p] >= 0) program_map_[int(p)] = picks[p];
    programs_known_ = true;
    sync_tracks(current_);
    if (!vw_waiting_.empty()) {
        std::string path;
        path.swap(vw_waiting_);
        import_vocalwriter(path);
    }
    if (!midi_waiting_.empty() && !voices_.empty()) {
        std::string path;
        path.swap(midi_waiting_);
        take_midi(path, false);
    }
}

void Studio::set_voices(std::vector<VoiceListing> v) {
    voices_ = std::move(v);
    // a song opened before the list arrived is put on its voices now
    resolve_voice_names(song_, voices_);
    sync_tracks(current_);        // the Voice column can now say
    // a MIDI file opened before it arrived comes in now
    if (!midi_waiting_.empty() && !voices_.empty() && programs_known_) {
        std::string path;
        path.swap(midi_waiting_);
        take_midi(path, false);
    }
}

std::string Studio::voice_name(int id) const {
    // with its engine's name, as every engine but VocalWriter names its
    // voices; an instrument by its own name alone
    for (const VoiceListing &v : voices_)
        if (v.id == id) {
            if (is_instrument(id)) return short_voice_name(v.name, v.family);
            if (is_vocalwriter_voice(id)) return v.family + " " + v.name;
            return v.name;
        }
    if (voices_.empty() && id == 0) return "VocalWriter Robert";
    return "voice " + std::to_string(id);
}

std::vector<PaletteRow> Studio::singable() const {
    std::vector<PaletteRow> out;
    for (const PaletteRow &r : palette())
        if (r.symbol != kRest) out.push_back(r);
    return out;
}

// -- tracks ---------------------------------------------------------------------------

//: On, muted, soloed -- or silent because some other track is soloed, which
//: is worth saying outright.
std::string Studio::track_state(const Track &t) const {
    std::vector<std::string> bits;
    if (t.solo) bits.push_back("solo");
    if (t.mute) bits.push_back("muted");
    if (bits.empty())
        for (const Track &x : song_.tracks)
            if (x.solo && !x.mute) {
                bits.push_back("silent");
                break;
            }
    if (bits.empty()) return "on";
    std::string out;
    for (const auto &b : bits) out += (out.empty() ? "" : ", ") + b;
    return out;
}

void Studio::refresh_track(int i) {
    const Track &t = song_.tracks[size_t(i)];
    tracks_list_->SetItem(i, 0, W(t.name));
    tracks_list_->SetItem(i, 1, W(track_sound(t)));
    tracks_list_->SetItem(i, 2, W(std::to_string(t.volume) + "%"));
    tracks_list_->SetItem(i, 3, W(pan_text(t.pan)));
    tracks_list_->SetItem(i, 4, W(track_state(t)));
}

void Studio::sync_tracks(int select) {
    switching_ = true;              // our own selecting is not a choice
    tracks_list_->DeleteAllItems();
    for (size_t i = 0; i < song_.tracks.size(); ++i) {
        tracks_list_->InsertItem(long(i), W(song_.tracks[i].name));
        refresh_track(int(i));
    }
    int i = std::min(std::max(select, 0), int(song_.tracks.size()) - 1);
    current_ = i;
    tracks_list_->SelectRow(i);
    tracks_list_->FocusRow(i);
    switching_ = false;
    relabel(list_, W("Notes in " + track().name));
}

//: Moving down the tracks list changes what the notes list shows.
void Studio::on_track_chosen() {
    int i = track_at();
    if (switching_ || i == current_ || i < 0 || i >= int(song_.tracks.size())) return;
    track().cursor = std::max(0, selection());
    current_ = i;
    const Track &t = track();
    relabel(list_, W("Notes in " + t.name));
    sync(t.cursor);
    if (t.is_backing()) say(W(t.name + ", " + track_sound(t) + ", " + track_state(t)));
    else say(W(t.name + ", " + track_sound(t) + ", " + plural(t.notes.size(), "note") + ", " + track_state(t)));
}

void Studio::on_track_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    bool plain = !(evt.ControlDown() || evt.AltDown() || evt.ShiftDown());
    bool arrow = code == WXK_UP || code == WXK_DOWN || code == WXK_LEFT || code == WXK_RIGHT;
    wxCommandEvent e;
    if (evt.AltDown() && !evt.ControlDown() && (code == WXK_UP || code == WXK_DOWN))
        transpose_tracks((code == WXK_UP ? 1 : -1) * (evt.ShiftDown() ? kOctave : kSemitone));
    else if (evt.ControlDown() && evt.ShiftDown() && !evt.AltDown() && arrow)
        nudge_tracks(code == WXK_UP ? kVolumeStep : code == WXK_DOWN ? -kVolumeStep : 0,
                     code == WXK_RIGHT ? kPanStep : code == WXK_LEFT ? -kPanStep : 0);
    else if (evt.ControlDown() && (code == WXK_UP || code == WXK_DOWN))
        move_tracks(code == WXK_UP ? -1 : 1);
    else if (evt.ControlDown() && !evt.ShiftDown() && !evt.AltDown() && code == 'A')
        select_all_tracks();
    else if (code == WXK_SPACE && plain)
        on_play_stop(e);
    else if (code == 'S' && plain)
        toggle_track(true);
    else if (code == 'M' && plain)
        toggle_track(false);
    else if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE)
        on_track_remove(e);
    else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER)
        on_track_edit(e);              // not skipped: one dialog, not two
    else
        evt.Skip();
}

//: Ctrl+A on a track: every track, to transpose or remove them together.
void Studio::select_all_tracks() {
    switching_ = true;              // the notes list stays on its track
    for (int i = 0; i < int(song_.tracks.size()); ++i) tracks_list_->SelectRow(i);
    switching_ = false;
    say(W("all " + plural(song_.tracks.size(), "track") + " selected"));
}

//: Alt with Up and Down on a track, a semitone, and with Shift an octave:
//: every note of the selected tracks, or of the one the keyboard is on. A
//: rest's lead-in pitch goes with them; a drum kit's notes are drums, and
//: stay as they are.
void Studio::transpose_tracks(int by) {
    undoable("transpose tracks", [&] {
        std::vector<int> rows;
        for (int r : tracks_list_->SelectedRows())
            if (r >= 0 && r < int(song_.tracks.size())) rows.push_back(r);
        if (rows.empty()) rows = {track_at()};
        std::vector<std::string> moved;
        size_t drums = 0;
        for (int r : rows) {
            Track &t = song_.tracks[size_t(r)];
            int voice = track_voice_of(t);
            bool any = false;
            for (Note &n : t.notes) {
                if (n.controller && n.voice_id) voice = *n.voice_id;
                if (is_drum_kit(voice)) {
                    drums += !n.controller && !n.is_rest();
                    continue;
                }
                if (!n.controller) {
                    any = any || !n.is_rest();
                    n.pitch = std::max(0, std::min(127, n.pitch + by));
                }
                auto lead = n.controls.find("lead_pitch");
                if (lead != n.controls.end())
                    for (auto &p : lead->second) p.second = std::max(0.0, std::min(127.0, p.second + by));
            }
            if (any) moved.push_back(t.name);
        }
        std::string how = std::string(by > 0 ? "up" : "down") + (std::abs(by) == kOctave ? " an octave" : " a semitone");
        if (moved.empty()) {
            say(drums ? "a drum kit's notes are drums, so they stay as they are"
                      : rows.size() == 1 ? "there are no notes in this track to transpose"
                                         : "there are no notes in these tracks to transpose");
            return;
        }
        touch();
        if (std::find(rows.begin(), rows.end(), current_) != rows.end()) sync(std::max(0, selection()));
        std::string message = (moved.size() == 1 ? moved[0] : plural(moved.size(), "track")) + " " + how;
        if (drums) message += ", the drums left as they are";
        announce_note(W(message), tracks_list_, track_at());
    });
}

//: Ctrl+Shift with Up and Down on a track, its volume a step up or down,
//: and with Right and Left its pan a step to the right or left: every
//: selected track, or the one the keyboard is on.
void Studio::nudge_tracks(int volume, int pan) {
    undoable(volume ? "change track volume" : "change track pan", [&] {
        std::vector<int> rows;
        for (int r : tracks_list_->SelectedRows())
            if (r >= 0 && r < int(song_.tracks.size())) rows.push_back(r);
        if (rows.empty()) rows = {track_at()};
        size_t moved = 0;
        for (int r : rows) {
            Track &t = song_.tracks[size_t(r)];
            int was_volume = t.volume, was_pan = t.pan;
            t.volume = std::max(0, std::min(kMaxVolume, t.volume + volume));
            t.pan = std::max(-100, std::min(100, t.pan + pan));
            if (t.volume == was_volume && t.pan == was_pan) continue;
            ++moved;
            refresh_track(r);
        }
        const Track &t = song_.tracks[size_t(track_at())];
        std::string now = volume ? "volume " + std::to_string(t.volume) + " per cent" : "pan " + pan_text(t.pan);
        if (!moved) {
            announce_note(W(now + ", as far as it goes"), tracks_list_, track_at());
            return;
        }
        touch();
        std::string how = volume ? std::to_string(std::abs(volume)) + " per cent " + (volume > 0 ? "louder" : "quieter")
                                 : std::to_string(std::abs(pan)) + " to the " + (pan > 0 ? "right" : "left");
        announce_note(W(rows.size() == 1 ? now : plural(moved, "track") + " " + how), tracks_list_, track_at());
    });
}

//: The track the keyboard is on, when several are selected; else the
//: selected one, else the one the notes list shows.
int Studio::track_at() const {
    std::vector<int> rows = tracks_list_->SelectedRows();
    int i = tracks_list_->FocusedRow();
    if (std::find(rows.begin(), rows.end(), i) == rows.end()) i = rows.empty() ? -1 : rows[0];
    return (i >= 0 && i < int(song_.tracks.size())) ? i : current_;
}

//: S and M on a track. Every row is redrawn, because soloing one track is a
//: statement about all the others as well.
void Studio::toggle_track(bool solo) {
    undoable("mute or solo track", [&] {
        int i = track_at();
        Track &t = song_.tracks[size_t(i)];
        bool &field = solo ? t.solo : t.mute;
        field = !field;
        touch();
        for (size_t k = 0; k < song_.tracks.size(); ++k) refresh_track(int(k));
        reannounce(tracks_list_, i);
        say(W(t.name + " " + (field ? "" : "not ") + (solo ? "soloed" : "muted")));
    });
}

//: a name not already in use, so two tracks never read the same
std::string Studio::track_name() const {
    size_t n = song_.tracks.size() + 1;
    auto taken = [&](const std::string &name) {
        for (const Track &t : song_.tracks)
            if (t.name == name) return true;
        return false;
    };
    while (taken("Voice " + std::to_string(n))) ++n;
    return "Voice " + std::to_string(n);
}

void Studio::on_track_new(wxCommandEvent &) {
    undoable("add track", [&] {
        Track t;
        t.name = track_name();
        t.program = track().program;
        song_.tracks.insert(song_.tracks.begin() + current_ + 1, t);
        touch();
        sync_tracks(current_ + 1);
        sync();
        tracks_list_->SetFocus();
        say(W("added " + t.name + ". Enter on a track sets its voice, volume and pan."));
    });
}

//: Ctrl+Shift+B: a sound file played under the voices -- the band the song
//: is sung over -- as a track of its own, with a volume, a pan, and mute and
//: solo like any other.
void Studio::on_track_backing(wxCommandEvent &) {
    undoable("add backing track", [&] {
        wxFileDialog pick(this, "Backing track", "", "", kAudioWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (pick.ShowModal() != wxID_OK) return;
        std::string why;
        if (!probe_audio(U(pick.GetPath()), &why)) {
            cannot_load(U(pick.GetPath()), why);
            return;
        }
        Track t = backing_track(U(pick.GetPath()));
        song_.tracks.insert(song_.tracks.begin() + current_ + 1, t);
        // a song with no name yet is named after it, as after an import
        if (path_.empty() && import_name_.empty()) import_name_ = t.name;
        touch();
        sync_tracks(current_ + 1);
        sync();
        tracks_list_->SetFocus();
        say(W("added the backing track " + t.name + ", at half volume, from the start of the song. Enter on it "
              "sets its volume, pan and where it starts."));
    });
}

std::string Studio::track_sound(const Track &t) const {
    if (t.is_backing()) return "backing track, " + base_name(t.audio);
    // a track that sings nothing, as the tempo is kept on, has no voice to speak of
    if (!sings(t)) {
        bool tempo = false, sig = false;
        for (const Note &n : t.notes) {
            tempo = tempo || (n.controller && n.tempo > 0);
            sig = sig || (n.controller && n.sig);
        }
        if (tempo && sig) return "tempo and time signature changes";
        if (tempo || sig) return tempo ? "tempo changes" : "time signature changes";
    }
    bool changes = !voice_changes(t).empty();
    return voice_name(track_voice_of(t)) + (changes ? ", with voice changes" : "");
}

//: The voice the current track sings with after its first `upto` rows.
int Studio::voice_at(int upto) const {
    int v = track_voice_of(track());
    const std::vector<Note> &all = track().notes;
    for (int i = 0; i < upto && i < int(all.size()); ++i)
        if (all[size_t(i)].controller && all[size_t(i)].voice_id) v = *all[size_t(i)].voice_id;
    return v;
}

//: A row as the list shows it: a controller with the voice it changes to.
std::string Studio::label_of(const Note &n) const {
    if (!n.controller) return n.label();
    std::string said;
    auto add = [&](const std::string &s) { said += (said.empty() ? "" : ", ") + s; };
    if (n.voice_id) add("voice change to " + voice_name(*n.voice_id));
    if (n.tempo > 0) add("tempo change to " + fmt_g(n.tempo));
    if (n.sig) add("time signature change to " + format_sig(*n.sig));
    if (n.pan) add("pan " + std::to_string(*n.pan));
    if (!n.controls.empty()) add(describe_controls(n));
    return "(" + (said.empty() ? std::string("controller") : said) + ")";
}

bool Studio::takes_notes() {
    if (!track().is_backing()) return true;
    say(W(track().name + " is a backing track, which plays its file and has no notes. Notes go on a track "
          "that sings."));
    return false;
}

void Studio::on_track_edit(wxCommandEvent &) {
    if (song_.tracks[size_t(track_at())].is_backing()) {
        undoable("track settings", [&] {
            int i = track_at();
            Track &t = song_.tracks[size_t(i)];
            BackingDialog dlg(this, t);
            if (dlg.ShowModal() != wxID_OK) return;
            dlg.apply(t);
            touch();
            refresh_track(i);
            if (i == current_) relabel(list_, W("Notes in " + t.name));
            reannounce(tracks_list_, i);
            auto bb = song_.meter().bar_and_beat(t.audio_beat);
            say(W(t.name + ", " + base_name(t.audio) + ", starting " + fmt_g(std::nearbyint(t.audio_beat * 1000) / 1000) +
                  " beats in, at bar " + std::to_string(bb.first) + " beat " +
                  fmt_g(std::nearbyint(bb.second * 1000) / 1000) + ", volume " + std::to_string(t.volume) + "%, " +
                  pan_text(t.pan)));
        });
        return;
    }
    undoable("track settings", [&] {
        int i = track_at();
        Track &t = song_.tracks[size_t(i)];
        TrackDialog dlg(this, this, t);
        if (dlg.ShowModal() != wxID_OK) return;
        dlg.apply(t);
        touch();
        refresh_track(i);
        if (i == current_) relabel(list_, W("Notes in " + t.name));
        reannounce(tracks_list_, i);
        say(W(t.name + ", " + voice_name(track_voice_of(t)) + ", volume " + std::to_string(t.volume) + "%, " +
              pan_text(t.pan) + ", " + (t.voice ? "its own voice controls" : "the song's voice controls")));
        // a voice that cannot sing here is worth saying at once, not at Play --
        // from what the engines said at the start, not by asking one now
        for (const VoiceListing &v : voices_)
            if (v.id == track_voice_of(t) && !v.available)
                for (const std::string &line : status_lines_)
                    if (line.compare(0, v.family.size() + 1, v.family + ":") == 0) say(W(line));
    });
}

//: Delete on a track: the selected tracks, one or several.
void Studio::on_track_remove(wxCommandEvent &) {
    undoable("remove track", [&] {
        if (song_.tracks.size() == 1) {
            say("a song needs at least one track");
            return;
        }
        std::vector<int> rows;
        for (int r : tracks_list_->SelectedRows())
            if (r >= 0 && r < int(song_.tracks.size())) rows.push_back(r);
        if (rows.empty()) rows = {track_at()};
        if (rows.size() >= song_.tracks.size()) {
            say("a song needs at least one track, so not every track can be removed");
            return;
        }
        size_t notes_in = 0;
        for (int r : rows) notes_in += song_.tracks[size_t(r)].notes.size();
        std::string what = rows.size() == 1 ? song_.tracks[size_t(rows[0])].name : plural(rows.size(), "track");
        if (notes_in) {
            std::string ask = rows.size() == 1 ? "Remove " + what + " and the " + plural(notes_in, "note") + " in it?"
                                               : "Remove the " + what + " and the " + plural(notes_in, "note") + " in them?";
            exclaim();
            int answer = wxMessageBox(W(ask), kAppName, wxYES_NO | wxICON_QUESTION, this);
            if (answer != wxYES) {
                say(W("kept " + (rows.size() == 1 ? what : "the " + what)));
                return;
            }
        }
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) song_.tracks.erase(song_.tracks.begin() + *it);
        touch();
        sync_tracks(std::min(rows[0], int(song_.tracks.size()) - 1));
        sync(track().cursor);
        say(W("removed " + what + ", " + plural(song_.tracks.size(), "track") + " left"));
    });
}

//: Ctrl+Up and Ctrl+Down on a track: put the parts in another order.
void Studio::move_tracks(int delta) {
    undoable("move track", [&] {
        int i = track_at();
        int j = i + delta;
        if (j < 0 || j >= int(song_.tracks.size())) {
            say(W(song_.tracks[size_t(i)].name + " is already " + (delta < 0 ? "first" : "last")));
            return;
        }
        std::swap(song_.tracks[size_t(i)], song_.tracks[size_t(j)]);
        touch();
        sync_tracks(j);
        tracks_list_->SetFocus();
        say(W(song_.tracks[size_t(j)].name + " is now track " + std::to_string(j + 1) + " of " +
              std::to_string(song_.tracks.size())));
    });
}

//: F6: between the tracks and the notes, without hunting with Tab.
void Studio::on_panes(wxCommandEvent &) {
    if (list_->HasFocus()) {
        tracks_list_->SetFocus();
        say("tracks");
    } else {
        list_->SetFocus();
        say(W("notes in " + track().name));
    }
}

// -- the list ---------------------------------------------------------------------------

int Studio::selection() const { return list_->FirstSelected(); }

std::vector<int> Studio::selected() const { return list_->SelectedRows(); }

void Studio::select_only(const std::vector<int> &rows) {
    for (int i : selected()) list_->SelectRow(i, false);
    for (int i : rows)
        if (i >= 0 && i < list_->GetItemCount()) list_->SelectRow(i);
    if (!rows.empty()) list_->FocusRow(std::max(0, std::min(rows[0], list_->GetItemCount() - 1)));
}

double Studio::position(int index) {
    double at = 0;
    for (int k = 0; k < index && k < int(notes().size()); ++k) at += notes()[size_t(k)].beats;
    return at;
}

//: The time signature in force where row `index` starts.
Sig Studio::sig_at(int index) { return song_.meter().sig_at(position(index)); }

//: Jump to a bar, which is how a long song is got about.
void Studio::on_goto_bar(wxCommandEvent &) {
    if (notes().empty()) {
        say("there are no notes yet");
        return;
    }
    // each row's bar, worked out once
    Meter meter = song_.meter();
    std::vector<int> bar_at;
    double at = 0;
    for (const Note &n : notes()) {
        bar_at.push_back(meter.bar_and_beat(at).first);
        at += n.beats;
    }
    int last_bar = bar_at.back();
    wxTextEntryDialog dlg(this, W("Bar number, 1 to " + std::to_string(last_bar)), "Go to bar", "1");
    // the 1 selected, so a bar typed replaces it
    for (wxWindow *w : dlg.GetChildren())
        if (wxTextCtrl *t = wxDynamicCast(w, wxTextCtrl)) dlg.CallAfter([t] { t->SelectAll(); });
    bool ok = dlg.ShowModal() == wxID_OK;
    wxString text = dlg.GetValue();
    if (!ok) return;
    long want;
    wxString t = text;
    t.Trim(true).Trim(false);
    if (!t.ToLong(&want)) {
        say("'" + text + "' is not a bar number");
        return;
    }
    for (int i = 0; i < int(notes().size()); ++i) {
        if (bar_at[size_t(i)] >= want) {
            select_only({i});
            list_->SetFocus();
            say(W("bar " + std::to_string(bar_at[size_t(i)]) + ", note " + std::to_string(i + 1) + " of " +
                  std::to_string(notes().size()) + ", " + notes()[size_t(i)].label()));
            return;
        }
    }
    say(W("the song ends at bar " + std::to_string(last_bar)));
}

//: Fill the rest of the bar with silence, which is what a time signature is
//: for while a song is being written.
void Studio::on_bar_rest(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("add bar rest", [&] {
        std::vector<int> rows = selected();
        int at = rows.empty() ? int(notes().size()) : rows.back() + 1;
        Meter meter = song_.meter();
        double pos = position(at);
        int bar = meter.bar_and_beat(pos).first;
        // from a bar line, a whole bar
        double left = meter.position(bar + 1) - pos;
        if (left < 1e-9) left = meter.position(++bar + 1) - pos;
        const Note *nearby = note_after(at);
        if (!nearby) nearby = note_before(at);
        Note rest({kRest}, nearby ? nearby->pitch : kDefaultPitch, left);
        notes().insert(notes().begin() + at, rest);
        touch();
        sync(at);
        say(W("rest to the end of bar " + std::to_string(bar) + ": " + beat_count(std::nearbyint(left * 1e6) / 1e6)));
    });
}

//: Where each note starts, as "bar:beat".
std::vector<std::string> Studio::bars() {
    std::vector<std::string> out;
    Meter meter = song_.meter();
    double at = 0;
    for (const Note &n : notes()) {
        auto bb = meter.bar_and_beat(at);
        out.push_back(std::to_string(bb.first) + ":" + fmt_g(std::nearbyint(bb.second * 1000) / 1000));
        at += n.beats;
    }
    return out;
}

void Studio::sync(int select) {
    list_->DeleteAllItems();
    // the voice in force, followed down the rows
    int voice = track_voice_of(track());
    for (size_t i = 0; i < notes().size(); ++i) {
        const Note &n = notes()[i];
        if (n.controller && n.voice_id) voice = *n.voice_id;
        list_->InsertItem(long(i), "");
        refresh_row(int(i), voice);
    }
    bars_shown_.clear();
    refresh_bars();
    // a voice change added or taken away shows in the tracks list
    refresh_track(current_);
    if (!notes().empty()) select_only({std::min(std::max(select, 0), int(notes().size()) - 1)});
}

void Studio::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    wxCommandEvent e;
    bool arrow = code == WXK_UP || code == WXK_DOWN || code == WXK_LEFT || code == WXK_RIGHT;
    if (evt.AltDown() && evt.ShiftDown() && arrow) {
        // the same keys, a bigger step: an octave, or twice and half the length
        if (code == WXK_UP) nudge_pitch(kOctave);
        else if (code == WXK_DOWN) nudge_pitch(-kOctave);
        else scale_length(code == WXK_RIGHT ? 2.0 : 0.5);
    } else if (evt.AltDown() && arrow) {
        if (code == WXK_UP) nudge_pitch(kSemitone);
        else if (code == WXK_DOWN) nudge_pitch(-kSemitone);
        else nudge_length(code == WXK_RIGHT);
    } else if (evt.ControlDown() && (code == WXK_UP || code == WXK_DOWN)) {
        move_notes(code == WXK_UP ? -1 : 1);
    } else if (code == WXK_SPACE && !(evt.ControlDown() || evt.AltDown() || evt.ShiftDown())) {
        on_play_stop(e);
    } else if (evt.ControlDown() && evt.ShiftDown() && code == 'C') {
        on_add_controller(e);
    } else if (evt.ControlDown() && !evt.ShiftDown() && code == 'A') {
        // with Shift these are the menu's, so they are skipped to it
        on_select_all(e);
    } else if (evt.ControlDown() && !evt.ShiftDown() && code == 'C') {
        on_copy(e);
    } else if (evt.ControlDown() && !evt.ShiftDown() && code == 'X') {
        on_cut(e);
    } else if (evt.ControlDown() && !evt.ShiftDown() && code == 'V') {
        on_paste(e);
    } else if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE) {
        on_remove(e);
    } else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
        on_edit(e);
    } else {
        evt.Skip();
    }
}

namespace {

//: A length on a track an instrument plays, to a thousandth of a beat: a
//: recorded performance is seldom on the beat.
std::string played_length(double beats) {
    std::string name = note_value(beats);
    std::string figure = fmt_g(std::nearbyint(beats * 1000) / 1000);
    return name.empty() ? figure : figure + ", " + name + " note";
}

}  // namespace

//: Update one row in place, so the selection and focus do not move.
void Studio::refresh_row(int i, int voice) {
    const Note &n = notes()[size_t(i)];
    if (voice < 0) voice = voice_at(i);
    // An instrument has no phonemes to show: a note is a note, or on a drum
    // kit the drum its key strikes, as General MIDI places them.
    bool played = is_instrument(voice) && !n.controller && !n.is_rest();
    if (played) {
        bool kit = is_drum_kit(voice);
        const char *drum = kit ? gm_drum_name(n.pitch) : nullptr;
        list_->SetItem(i, 0, drum ? W(drum) : wxString(kit ? "(drum)" : "(note)"));
    } else {
        list_->SetItem(i, 0, W(label_of(n)));
    }
    // a controller has no pitch, length, words or controls of its own
    if (n.controller) {
        for (int c : {1, 2, 3, 4, 5, 7}) list_->SetItem(i, c, "");
        return;
    }
    list_->SetItem(i, 1, W(pitch_name(n.pitch)));
    if (played && n.beats <= 0)
        list_->SetItem(i, 2, W(played_length(n.sounds()) + ", with the next"));
    else if (played && n.hold > 0)
        list_->SetItem(i, 2, W(played_length(n.beats) + ", sounds " + fmt_g(std::nearbyint(n.hold * 1000) / 1000)));
    else if (is_instrument(voice))
        list_->SetItem(i, 2, W(played_length(n.beats)));
    else
        list_->SetItem(i, 2, W(length_text(n.beats)));
    list_->SetItem(i, 3, W(n.word));
    list_->SetItem(i, 4, W(describe_bend(n.bend)));
    list_->SetItem(i, 5, W(describe_mod(n.mod)));
    list_->SetItem(i, 7, W(describe_controls(n)));
}

//: The Bar column, worked out once and written only where it changed. It used
//: to be worked out and written for every row each time any one row was
//: refreshed -- and a length change refreshes every row -- so one nudge in a
//: song of a few hundred notes was tens of thousands of updates to the list.
void Studio::refresh_bars() {
    std::vector<std::string> where = bars();
    bars_shown_.resize(where.size());
    for (size_t k = 0; k < where.size(); ++k) {
        if (bars_shown_[k] == where[k]) continue;
        list_->SetItem(long(k), 6, W(where[k]));
        bars_shown_[k] = where[k];
    }
}

//: The selected rows that are notes or rests; a controller has no pitch or
//: length to change. Empty, and said, if there are none.
std::vector<int> Studio::selected_notes() {
    std::vector<int> rows;
    for (int i : selected())
        if (!notes()[size_t(i)].controller) rows.push_back(i);
    if (rows.empty()) say(selected().empty() ? "select a note first" : "a controller has no pitch or length");
    return rows;
}

//: The nearest note or rest at or before row `at - 1`, else the last: what
//: a new one takes its pitch and length from.
const Note *Studio::note_before(int at) {
    for (int i = std::min(at, int(notes().size())) - 1; i >= 0; --i)
        if (!notes()[size_t(i)].controller) return &notes()[size_t(i)];
    return last();
}

//: The first note sung or played at or after row `at`, or none: the pitch
//: a rest put in before it leads up to.
const Note *Studio::note_after(int at) {
    for (int i = std::max(0, at); i < int(notes().size()); ++i)
        if (!notes()[size_t(i)].controller && !notes()[size_t(i)].is_rest()) return &notes()[size_t(i)];
    return nullptr;
}

void Studio::nudge_pitch(int by) {
    undoable("transpose notes", [&] {
        std::vector<int> rows = selected_notes();
        if (rows.empty()) return;
        for (int i : rows) {
            Note &n = notes()[size_t(i)];
            n.pitch = std::max(0, std::min(127, n.pitch + by));
            refresh_row(i);
        }
        touch();
        std::string message = rows.size() == 1 ? pitch_name(notes()[size_t(rows[0])].pitch)
                                               : std::to_string(rows.size()) + " notes " +
                                                     (by > 0 ? "up" : "down") + (std::abs(by) == kOctave ? " an octave" : " a semitone");
        announce_note(W(message), list_, rows[0]);
        preview_note(rows[0]);
    });
}

//: A sixteenth note longer or shorter.
void Studio::nudge_length(bool up) {
    undoable("change note length", [&] {
        std::vector<int> rows = selected_notes();
        if (rows.empty()) return;
        int moved = 0;
        for (int i : rows) {
            // a held note changes its hold, the notes after it staying put
            Note &n = notes()[size_t(i)];
            double &length = n.hold > 0 ? n.hold : n.beats;
            auto want = stepped_length(length, up);
            if (!want) continue;
            length = *want;
            ++moved;
        }
        if (!moved) {
            announce_note(W(spoken_length(notes()[size_t(rows[0])].sounds(), sig_at(rows[0])) +
                            ", the shortest a nudge will make it. Edit the note to go shorter."),
                          list_, rows[0]);
            return;
        }
        touch();
        sync_lengths(rows);
        preview_note(rows[0]);
        std::string message = rows.size() == 1 ? spoken_length(notes()[size_t(rows[0])].sounds(), sig_at(rows[0]))
                                               : std::to_string(moved) + " notes a sixteenth " +
                                                     (up ? "longer" : "shorter");
        announce_note(W(message), list_, rows[0]);
    });
}

//: Twice or half the length: Alt+Shift with Right and Left. A note that
//: would come out shorter than a thirty-second note is left as it is, as a
//: nudge leaves it, rather than being made as short as it can go.
void Studio::scale_length(double factor) {
    undoable("change note length", [&] {
        std::vector<int> rows = selected_notes();
        if (rows.empty()) return;
        int moved = 0;
        for (int i : rows) {
            Note &n = notes()[size_t(i)];
            double &length = n.hold > 0 ? n.hold : n.beats;
            double want = std::nearbyint(length * factor * 1e6) / 1e6;
            if (want < kMinBeats - 1e-9) continue;
            length = want;
            ++moved;
        }
        if (!moved) {
            announce_note(W(spoken_length(notes()[size_t(rows[0])].sounds(), sig_at(rows[0])) +
                            ", and half of that is shorter than a nudge will go. Edit the note to go shorter."),
                          list_, rows[0]);
            return;
        }
        touch();
        sync_lengths(rows);
        preview_note(rows[0]);
        std::string message = rows.size() == 1 ? spoken_length(notes()[size_t(rows[0])].sounds(), sig_at(rows[0]))
                                               : std::to_string(moved) + " notes " +
                                                     (factor > 1 ? "twice as long" : "half as long");
        announce_note(W(message), list_, rows[0]);
    });
}

//: The notes whose lengths changed, and the bars after them.
void Studio::sync_lengths(const std::vector<int> &rows) {
    for (int i : rows) refresh_row(i);
    refresh_bars();
}

const Note *Studio::last() {
    for (auto it = notes().rbegin(); it != notes().rend(); ++it)
        if (!it->controller) return &*it;
    return nullptr;
}

void Studio::on_select_all(wxCommandEvent &) {
    if (notes().empty()) {
        say("there are no notes yet");
        return;
    }
    std::vector<int> all;
    for (size_t i = 0; i < notes().size(); ++i) all.push_back(int(i));
    select_only(all);
    say(W("all " + std::to_string(notes().size()) + " notes selected"));
}

//: Walk the selected notes one place earlier or later, the selection with them.
void Studio::move_notes(int delta) {
    undoable("move notes", [&] {
        std::vector<int> rows = selected();
        if (rows.empty()) {
            say("select a note first");
            return;
        }
        if ((delta < 0 && rows.front() == 0) || (delta > 0 && rows.back() == int(notes().size()) - 1)) {
            say(wxString("already at the ") + (delta < 0 ? "beginning" : "end"));
            return;
        }
        if (delta < 0)
            for (int i : rows) std::swap(notes()[size_t(i)], notes()[size_t(i + delta)]);
        else
            for (auto it = rows.rbegin(); it != rows.rend(); ++it)
                std::swap(notes()[size_t(*it)], notes()[size_t(*it + delta)]);
        std::vector<int> moved;
        for (int i : rows) moved.push_back(i + delta);
        touch();
        sync(moved[0]);
        select_only(moved);
        if (moved.size() == 1)
            say(W(notes()[size_t(moved[0])].label() + " is now note " + std::to_string(moved[0] + 1) + " of " +
                  std::to_string(notes().size())));
        else
            say(W(std::to_string(moved.size()) + " notes moved " + (delta < 0 ? "earlier" : "later")));
        reannounce(list_, moved[0]);
    });
}

// -- copy and paste ---------------------------------------------------------------------
//
// Notes go to the system clipboard as text, so a phrase can be carried
// between two copies of the program and what was copied can be seen.

bool Studio::to_clipboard(const std::string &text) {
    if (!wxTheClipboard->Open()) return false;
    wxTheClipboard->SetData(new wxTextDataObject(W(text)));
    wxTheClipboard->Close();
    return true;
}

std::string Studio::from_clipboard() {
    if (!wxTheClipboard->Open()) return "";
    wxTextDataObject data;
    std::string out;
    if (wxTheClipboard->GetData(data)) out = U(data.GetText());
    wxTheClipboard->Close();
    return out;
}

void Studio::on_copy(wxCommandEvent &) {
    std::vector<int> rows = selected();
    if (rows.empty()) {
        say("select a note first");
        return;
    }
    std::vector<Note> picked;
    for (int i : rows) picked.push_back(notes()[size_t(i)]);
    if (!to_clipboard(notes_to_clipboard(picked))) {
        say("the clipboard is busy, try again");
        return;
    }
    say(W("copied " + (picked.size() == 1 ? picked[0].label() : std::to_string(picked.size()) + " notes")));
}

void Studio::on_cut(wxCommandEvent &) {
    undoable("cut notes", [&] {
        std::vector<int> rows = selected();
        if (rows.empty()) {
            say("select a note first");
            return;
        }
        wxCommandEvent e;
        on_copy(e);
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) notes().erase(notes().begin() + *it);
        touch();
        sync(std::min(rows[0], int(notes().size()) - 1));
        say(W("cut " + plural(rows.size(), "note")));
    });
}

void Studio::on_paste(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("paste notes", [&] {
        std::vector<Note> added = notes_from_clipboard(from_clipboard());
        if (added.empty()) {
            say("there are no notes on the clipboard");
            return;
        }
        std::vector<int> here = selected();
        int at = here.empty() ? int(notes().size()) : here.back() + 1;
        notes().insert(notes().begin() + at, added.begin(), added.end());
        touch();
        sync(at);
        std::vector<int> rows;
        for (size_t k = 0; k < added.size(); ++k) rows.push_back(at + int(k));
        select_only(rows);
        std::string first;
        for (size_t k = 0; k < added.size() && k < 4; ++k) first += (k ? ", " : "") + added[k].label();
        say(W("pasted " + plural(added.size(), "note") + ": " + first));
    });
}

//: Where a new note goes, as everything else that adds goes: after the
//: selected notes, or at the end when nothing is selected. It used to be the
//: end always -- the original program's way -- so adding a word in the middle
//: of a song meant adding it at the end and walking it back up note by note.
int Studio::insertion_point() {
    std::vector<int> rows = selected();
    return rows.empty() ? int(notes().size()) : rows.back() + 1;
}

//: The note a new one takes its pitch and length from: the one it goes in
//: after, or the last.
const Note *Studio::insertion_neighbour() { return note_before(insertion_point()); }

void Studio::on_add_word(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("add word", [&] {
        const Note *l = insertion_neighbour();
        AddWordDialog dlg(this, this, l ? l->pitch : kDefaultPitch,
                          l ? (l->beats > 0 ? l->beats : l->sounds()) : kDefaultBeats);
        if (dlg.ShowModal() != wxID_OK) return;
        std::vector<Note> added = dlg.result();
        if (added.empty()) return;
        int at = insertion_point();
        notes().insert(notes().begin() + at, added.begin(), added.end());
        touch();
        sync(at);
        std::string all;
        for (const Note &n : added) all += (all.empty() ? "" : ", ") + n.text();
        say(W("added " + (added[0].word.empty() ? std::string("the word") : added[0].word) + " over " +
              plural(added.size(), "note") + ": " + all));
    });
}

//: A word on the notes already there, from the one the cursor is on: a
//: melody given its words without a phoneme typed.
void Studio::on_add_word_existing(wxCommandEvent &) {
    if (!takes_notes()) return;
    // the notes that sound, from the cursor on, a chord's last alone
    std::vector<int> spots;
    std::vector<Note> existing;
    int from = list_->FocusedRow();
    if (from < 0 && !selected().empty()) from = selected().front();
    for (int i = std::max(0, from); from >= 0 && i < int(notes().size()) && spots.size() < kMaxWordNotes; ++i) {
        const Note &n = notes()[size_t(i)];
        if (n.controller || n.is_rest() || n.beats <= 0) continue;
        spots.push_back(i);
        existing.push_back(n);
    }
    if (spots.empty()) {
        say("there is no note from the cursor on to put a word on");
        return;
    }
    undoable("add word to existing note(s)", [&] {
        AddWordDialog dlg(this, this, existing);
        if (dlg.ShowModal() != wxID_OK) return;
        std::vector<Note> put = dlg.result();
        if (put.empty()) return;
        for (size_t k = 0; k < put.size() && k < spots.size(); ++k) notes()[size_t(spots[k])] = put[k];
        touch();
        // on to the note after, ready for the next word
        sync(put.size() < spots.size() ? spots[put.size()] : spots[std::min(put.size(), spots.size()) - 1]);
        std::string all;
        for (const Note &n : put) all += (all.empty() ? "" : ", ") + n.text();
        say(W("put " + (put[0].word.empty() ? std::string("the word") : put[0].word) + " on " +
              plural(put.size(), "note") + ": " + all));
    });
}

//: A whole text on the notes already there, from the cursor to the end of
//: the track: a song's words at once.
void Studio::on_add_text(wxCommandEvent &) {
    if (!takes_notes()) return;
    // the notes that sound, as for Add word, with no limit
    std::vector<int> spots;
    std::vector<Note> existing;
    int from = list_->FocusedRow();
    if (from < 0 && !selected().empty()) from = selected().front();
    for (int i = std::max(0, from); from >= 0 && i < int(notes().size()); ++i) {
        const Note &n = notes()[size_t(i)];
        if (n.controller || n.is_rest() || n.beats <= 0) continue;
        spots.push_back(i);
        existing.push_back(n);
    }
    if (spots.empty()) {
        say("there is no note from the cursor on to put a text on");
        return;
    }
    undoable("add text", [&] {
        AddWordDialog dlg(this, this, existing, true, spots);
        if (dlg.ShowModal() != wxID_OK) return;
        std::vector<Note> put = dlg.result();
        if (put.empty()) return;
        for (size_t k = 0; k < put.size() && k < spots.size(); ++k) notes()[size_t(spots[k])] = put[k];
        touch();
        // on to the note after, ready for more
        sync(put.size() < spots.size() ? spots[put.size()] : spots[std::min(put.size(), spots.size()) - 1]);
        size_t words = size_t(std::count_if(put.begin(), put.end(), [](const Note &n) { return !n.word.empty(); }));
        say(W("put " + plural(words, "word") + " on " + plural(put.size(), "note")));
    });
}

void Studio::on_add_note(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("add note", [&] {
        const Note *l = insertion_neighbour();
        // an instrument's note takes the beats and hold of the one before it;
        // a voice's takes the length, never 0 beats
        bool played = played_at(insertion_point());
        Note seed({}, l ? l->pitch : kDefaultPitch, l ? l->beats : kDefaultBeats);
        if (l && played) seed.hold = l->hold;
        else if (l && l->beats <= 0) seed.beats = l->sounds();
        NoteDialog dlg(this, this, nullptr, seed);
        if (dlg.ShowModal() != wxID_OK) return;
        int at = insertion_point();
        Note n = dlg.result();
        // a note with no phonemes would be a rest
        if (played && n.phonemes.empty()) n.phonemes = {kDefaultPhoneme};
        std::vector<Note> rows = played ? chord_rows(n, dlg.pitches()) : std::vector<Note>{n};
        notes().insert(notes().begin() + at, rows.begin(), rows.end());
        touch();
        sync(at);
        if (rows.size() > 1) {
            std::string names;
            for (const Note &r : rows) names += (names.empty() ? "" : " ") + pitch_name(r.pitch);
            say(W("added a chord, " + names + ", " + plural(rows.size(), "note")));
            return;
        }
        std::string text = played ? pitch_name(n.pitch) : notes()[size_t(at)].text();
        say(W("added a note: " + (text.empty() ? std::string("empty") : text)));
    });
}

//: A rest is a note nobody sings. It breaks the line into phrases, so the
//: phrase before it is allowed to end instead of running into the next.
void Studio::on_add_rest(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("add rest", [&] {
        int i = selection();
        int at = i < 0 ? int(notes().size()) : i + 1;
        const Note *nearby = i >= 0 ? note_before(i + 1) : last();
        // the pitch of the note it leads up to; the length of the one before
        const Note *next = note_after(at);
        Note rest({kRest}, next ? next->pitch : nearby ? nearby->pitch : kDefaultPitch,
                  nearby ? (nearby->beats > 0 ? nearby->beats : nearby->sounds()) : kDefaultBeats);
        notes().insert(notes().begin() + at, rest);
        touch();
        sync(at);
        say(W("added a rest of " + beat_count(rest.beats) + ". Alt+Right and Alt+Left change it."));
    });
}

//: The note the cursor is on, and no other, made a rest as long and at the
//: same pitch. Its word, bend and wheel go; the controls it carries stay, as
//: they hold for the notes after it.
void Studio::on_note_to_rest(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("convert note to rest", [&] {
        int i = list_->FocusedRow();
        if (i < 0 || i >= int(notes().size())) i = selection();
        if (i < 0) {
            say("select a note first");
            return;
        }
        Note &n = notes()[size_t(i)];
        if (n.controller) {
            say("a controller is not a note, so it cannot be a rest");
            return;
        }
        if (n.is_rest()) {
            say("it is a rest already");
            return;
        }
        // a chord's note but the last sounds with the next, taking no time
        if (n.beats <= 0) {
            say("a chord's note takes no time of its own, so it cannot be a rest");
            return;
        }
        Note rest({kRest}, n.pitch, n.beats);
        rest.controls = n.controls;
        n = rest;
        touch();
        sync(i);
        say(W("now a rest of " + beat_count(rest.beats)));
    });
}

//: A controller: a change from here on, which takes no time. A tempo it sets
//: is the whole song's.
void Studio::on_add_controller(wxCommandEvent &) {
    if (!takes_notes()) return;
    undoable("add controller", [&] {
        ControllerDialog dlg(this, this, Note::tempo_change(0), voice_at(insertion_point()));
        if (dlg.ShowModal() != wxID_OK) return;
        Note c = dlg.result();
        if (c.tempo <= 0 && !c.sig && !c.voice_id && !c.pan && c.controls.empty()) {
            say("nothing was filled in, so no controller was added");
            return;
        }
        int at = insertion_point();
        notes().insert(notes().begin() + at, c);
        touch();
        sync(at);
        say(W("added a controller: " + controller_said(at)));
    });
}

//: What a controller changes, where, and what that does to the song.
std::string Studio::controller_said(int i) {
    const Note &c = notes()[size_t(i)];
    double at = position(i);
    auto bb = song_.meter().bar_and_beat(at);
    std::string where = "bar " + std::to_string(bb.first) + " beat " + fmt_g(std::nearbyint(bb.second * 1000) / 1000);
    if (c.tempo <= 0 && !c.sig && !c.voice_id && !c.pan && c.controls.empty())
        return "nothing at " + where + ". Delete removes it";
    std::string out;
    auto add = [&](const std::string &s) { out += (out.empty() ? "" : ", ") + s; };
    if (c.voice_id) add("voice " + voice_name(*c.voice_id));
    if (c.pan) add("pan " + std::to_string(*c.pan));
    if (!c.controls.empty()) add(describe_controls(c));
    if (!out.empty()) out += " from " + where;
    if (c.tempo <= 0 && !c.sig) return out;
    // what it changes for the whole song
    std::string whole;
    if (c.tempo > 0) whole = "tempo " + fmt_g(c.tempo);
    if (c.sig) whole += (whole.empty() ? "" : " and ") + std::string("time signature ") + format_sig(*c.sig);
    out += (out.empty() ? whole + " from " + where : ", and " + whole) + ", for the whole song";
    // the same beat changed elsewhere too: the track higher up wins
    for (size_t k = 0; k < song_.tracks.size(); ++k) {
        if (int(k) == current_) continue;
        double b = 0;
        for (const Note &n : song_.tracks[k].notes) {
            bool same = (c.tempo > 0 && n.tempo > 0) || (c.sig && n.sig);
            if (n.controller && same && std::fabs(b - at) < 1e-6) {
                out += ". " + song_.tracks[k].name + " changes it there too, and the track higher up wins";
                return out;
            }
            b += n.beats;
        }
    }
    return out;
}

void Studio::on_edit(wxCommandEvent &) {
    undoable("edit note", [&] {
        int i = selection();
        if (i < 0) {
            say("select a note first");
            return;
        }
        if (notes()[size_t(i)].controller) {
            ControllerDialog dlg(this, this, notes()[size_t(i)], voice_at(i));
            if (dlg.ShowModal() != wxID_OK) return;
            notes()[size_t(i)] = dlg.result();
            touch();
            sync(i);
            say(W("controller: " + controller_said(i)));
            return;
        }
        NoteDialog dlg(this, this, &notes()[size_t(i)]);
        if (dlg.ShowModal() != wxID_OK) return;
        Note n = dlg.result();
        bool played = played_at(i);
        if (played && n.phonemes.empty() && !notes()[size_t(i)].is_rest()) n.phonemes = {kDefaultPhoneme};
        std::vector<Note> rows = played ? chord_rows(n, dlg.pitches()) : std::vector<Note>{n};
        notes()[size_t(i)] = rows.back();
        notes().insert(notes().begin() + i, rows.begin(), rows.end() - 1);
        touch();
        sync(i);
        if (rows.size() > 1) {
            say(W("note " + std::to_string(i + 1) + " is now a chord of " + plural(rows.size(), "note")));
            return;
        }
        say(W("note " + std::to_string(i + 1) + " is " + (played ? std::string("played") : notes()[size_t(i)].text()) +
              " at " + pitch_name(notes()[size_t(i)].pitch)));
    });
}

void Studio::on_remove(wxCommandEvent &) {
    undoable("remove notes", [&] {
        std::vector<int> rows = selected();
        if (rows.empty()) {
            say("select a note first");
            return;
        }
        std::string gone = rows.size() == 1 ? notes()[size_t(rows[0])].label()
                                            : std::to_string(rows.size()) + " notes";
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) notes().erase(notes().begin() + *it);
        touch();
        sync(std::min(rows[0], int(notes().size()) - 1));
        say(W("removed " + gone));
    });
}

// -- audio --------------------------------------------------------------------------------

//: The song as the engines want it. Only the tracks that can be heard are
//: sent. `only` renders just those notes as the current part -- the part
//: copied whole and given those notes, so a preview sounds like the part it
//: is in -- and `parts` says outright which tracks to sing.
RenderRequest Studio::request(const std::vector<Note> *only, double start,
                              const std::vector<const Track *> *parts) {
    std::unique_ptr<const std::vector<Note>> owned(only);
    std::vector<const Track *> sing;
    Track copy;
    if (parts) {
        sing = *parts;
    } else if (!only) {
        sing = audible(song_.tracks);
    } else {
        copy = track();
        copy.voice_id = current_voice();
        copy.notes = *only;
        sing = {&copy};
    }
    return make_request(song_, sing, start, program_map_);
}

//: The song as it is played; the metronome goes on when it plays (ticked).
RenderRequest Studio::playback(double start) { return request(nullptr, start); }

//: The song as it plays from play_from_, with the metronome if it is on.
std::shared_ptr<const std::vector<float>> Studio::ticked(const Mixed &res) const {
    if (!mi_metronome_->IsChecked() || !res.samples) return res.samples;
    auto y = std::make_shared<std::vector<float>>(*res.samples);
    with_metronome(*y, res.channels, song_.clock(), song_.meter(), play_from_, res.lead);
    return y;
}

void Studio::render(const RenderRequest &req, const std::string &out, std::function<void(RenderResult)> done) {
    std::shared_ptr<Mixer> mixer = mixer_;
    worker_->ask<RenderResult>([mixer, req, out] { return mixer->render(req, out); },
                               [this, done](RenderResult res) {
                                   // a failed render still has to come back, or
                                   // whatever asked would wait for ever
                                   if (!res.ok && !res.error.empty()) say("engine error: " + W(res.error));
                                   done(res);
                               });
}

//: The song in memory, for playing: nothing is written to disk.
void Studio::mix(const RenderRequest &req, std::function<void(Mixed)> done) {
    std::shared_ptr<Mixer> mixer = mixer_;
    worker_->ask<Mixed>([mixer, req] { return mixer->mix(req); },
                        [this, done](Mixed res) {
                            if (!res.ok && !res.error.empty()) say("engine error: " + W(res.error));
                            done(res);
                        });
}

void Studio::preview(const std::vector<Note> &rows, std::function<void()> done) {
    mix(request(new std::vector<Note>(rows)), [this, done](Mixed res) {
        if (done) done();
        if (res.ok) play_audio(res, true);
    });
}

void Studio::preview_track(const std::vector<int> &spots, const std::vector<Note> &put, int from,
                           std::function<bool()> wanted, std::function<void(bool)> done) {
    Track copy = track();
    for (size_t k = 0; k < put.size() && k < spots.size(); ++k)
        if (spots[k] >= 0 && spots[k] < int(copy.notes.size())) copy.notes[size_t(spots[k])] = put[k];
    double start = 0;
    for (int i = 0; i < from && i < int(copy.notes.size()); ++i) start += copy.notes[size_t(i)].beats;
    stop_audio();
    // the track alone, muted or not, whatever else is soloed
    std::vector<const Track *> one{&copy};
    mix(request(nullptr, start, &one), [this, wanted, done](Mixed res) {
        bool play = res.ok && wanted && wanted();
        if (play) play_audio(res, true);
        if (done) done(play && player_.playing());
    });
}

void Studio::lookup(const std::vector<std::string> &words,
                    std::function<void(std::map<std::string, std::vector<std::string>>)> done,
                    bool marked) {
    worker_->ask<std::map<std::string, std::vector<std::string>>>(
        [words, marked] {
            std::map<std::string, std::vector<std::string>> out;
            for (const std::string &w : words) out[w] = Lexicon::get().phonemes(w, marked);
            return out;
        },
        done);
}

void Studio::on_metronome(wxCommandEvent &) {
    // into the song playing, or out of it, from where it has got to
    if (play_mix_.samples && player_.playing()) player_.swap(ticked(play_mix_), play_mix_.channels);
    if (!mi_metronome_->IsChecked()) {
        say("metronome off");
        return;
    }
    say(W("metronome on, " + format_sig(signature()) + ", " + fmt_g(bar_beats(signature())) +
          " beats to the bar. It ticks along with Play and is never written into an exported file."));
}

//: Render only the selected note -- far quicker than the whole line.
void Studio::on_hear(wxCommandEvent &) {
    int i = selection();
    if (i < 0) {
        say("select a note first");
        return;
    }
    if (rendering_) {
        say("already rendering");
        return;
    }
    if (notes()[size_t(i)].controller) {
        say("a controller has nothing to hear");
        return;
    }
    stop_audio();
    rendering_ = true;
    // the note the cursor is on, a chord's too
    mix(request(new std::vector<Note>{alone(notes()[size_t(i)])}), [this](Mixed res) { heard(res); });
}

void Studio::heard(const Mixed &res) {
    rendering_ = false;
    if (discard_render_) {
        discard_render_ = false;
        return;
    }
    if (res.ok) play_audio(res, true);
    else say("render failed");
}

//: Where Space starts from: the note the cursor is on. Every track starts
//: there -- the cursor is a place in the song, and the parts stay together.
double Studio::start_beats() {
    int i = selection();
    return i > 0 ? position(i) : 0.0;
}

void Studio::on_play_stop(wxCommandEvent &) {
    if (rendering_) {
        say("still rendering");
        return;
    }
    if (player_.playing()) {
        player_.stop();
        say("stopped");
        return;
    }
    start_playing(start_beats());
}

//: Ctrl+P: from the top, wherever the cursor happens to be.
void Studio::on_play(wxCommandEvent &) { start_playing(0.0, true); }

void Studio::start_playing(double start, bool quiet) {
    if (rendering_) {
        say("already rendering");
        return;
    }
    std::vector<const Track *> live;
    bool backed = false;
    for (const Track *t : audible(song_.tracks))
        if (!t->notes.empty() || t->is_backing()) {
            live.push_back(t);
            backed = backed || t->is_backing();
        }
    if (live.empty()) {
        bool any = false;
        for (const Track &t : song_.tracks) any = any || !t.notes.empty() || t.is_backing();
        say(any ? "every track with notes in it is silent. M unmutes a track, S turns solo off."
                : "nothing to sing yet. Add a word or a note.");
        return;
    }
    double total = 0;
    for (const Track *t : live) {
        if (t->is_backing()) continue;
        double s = 0;
        for (const Note &n : t->notes) s += n.beats;
        total = std::max(total, s);
    }
    // past the end: start again (a backing track runs on past the voices,
    // but where the cursor is is where the notes are)
    if (start >= total - 1e-9) start = 0.0;
    stop_audio();          // the file cannot be written while it plays
    play_from_ = start;
    rendering_ = true;
    mi_play_->Enable(false);
    if (!quiet && backed) {
        // how long a backing track's file runs on is not known until it is read
        auto bb = song_.meter().bar_and_beat(start);
        say(W(printf_s("rendering %s from bar %d beat %s", plural(live.size(), "track").c_str(), bb.first,
                       fmt_g(std::nearbyint(bb.second * 100) / 100).c_str())));
    } else if (!quiet) {
        auto bb = song_.meter().bar_and_beat(start);
        say(W(printf_s("rendering %s from bar %d beat %s, about %.1f seconds", plural(live.size(), "track").c_str(),
                       bb.first, fmt_g(std::nearbyint(bb.second * 100) / 100).c_str(),
                       song_.clock().seconds(total) - song_.clock().seconds(start))));
    }
    mix(playback(start), [this, quiet](Mixed res) { played(res, quiet); });
}

void Studio::played(const Mixed &res, bool quiet) {
    rendering_ = false;
    mi_play_->Enable(true);
    if (discard_render_) {
        discard_render_ = false;
        return;
    }
    if (!res.ok) {
        say("render failed");
        return;
    }
    std::string loud;
    if (res.peak > 1.0)
        // said out loud: the song simply getting quieter is worse than clipping
        loud = ", the tracks added up past full scale so the mix was turned down";
    if (res.stopped_short)
        loud += ", and one phrase was too long to render in one go, so it stopped early -- a rest "
                "anywhere in it renders the two halves separately";
    if (!quiet)
        say(W(printf_s("playing %.2f seconds%s%s", res.seconds, res.cached ? " from cache" : "", loud.c_str())));
    else if (!loud.empty())
        say(W(loud.substr(2)));
    Mixed shown = res;
    shown.samples = ticked(res);
    play_audio(shown, quiet);
    play_mix_ = res;
    // the song, not a note heard on its own, is what the cursors follow
    play_clock_ = song_.clock();
    play_lead_ = res.lead;
    following_ = player_.playing();
    if (following_ && settings_.follow_playback) follow_timer_.Start(kFollowStep);
}

//: Straight from memory to the sound card.
void Studio::play_audio(const Mixed &res, bool quiet) {
    (void)quiet;
    play_mix_ = Mixed();
    if (!res.samples || res.samples->empty()) {
        say("nothing to play");
        return;
    }
    if (!player_.play(res.samples, res.channels)) say("cannot play: no sound device would open");
}

void Studio::stop_audio() {
    player_.stop();
    play_mix_ = Mixed();
    following_ = false;
}

//: Another song came in: what plays stops, and one on its way is not played.
void Studio::drop_playback() {
    stop_audio();
    if (rendering_) discard_render_ = true;
}

void Studio::on_stop(wxCommandEvent &) {
    stop_audio();
    say("stopped");
}

//: The name a file made of the song starts with: the project's, or the
//: file's it was imported from; empty for a song that has neither.
std::string Studio::song_name() const {
    if (!path_.empty()) return base_name(strip_extension(path_));
    return import_name_;
}

void Studio::on_export(wxCommandEvent &) {
    if (rendering_) {
        say("already rendering");
        return;
    }
    bool any = false;
    for (const Track *t : audible(song_.tracks)) any = any || !t->notes.empty() || t->is_backing();
    if (!any) {
        say("nothing to export: no track that can be heard has any notes in it");
        return;
    }
    // the kind of file chosen last time to begin with
    int kind = 0;
    for (int k = 0; k < kExportKinds; ++k)
        if (settings_.export_format == kExportExtensions[k]) kind = k;
    std::string name = song_name();
    wxFileDialog dlg(this, "Export audio", "", W(name.empty() ? name : name + kExportExtensions[kind]),
                     kExportWildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    dlg.SetFilterIndex(kind);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    // a name with none of the kinds' endings is given the one chosen
    kind = std::max(0, std::min(kExportKinds - 1, dlg.GetFilterIndex()));
    std::string ext = extension_lower(path);
    bool known = false;
    for (int k = 0; k < kExportKinds; ++k)
        if (ext == kExportExtensions[k]) {
            known = true;
            kind = k;
        }
    if (!known) path += kExportExtensions[kind];
    if (settings_.export_format != kExportExtensions[kind]) {
        settings_.export_format = kExportExtensions[kind];
        save_settings(settings_);
    }
    say(W("rendering to " + base_name(path)));
    // A window that stays until the file is written, so the program is not
    // closed under it, and that can give the export up.
    rendering_ = true;
    mi_play_->Enable(false);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    export_cancel_ = cancel;
    export_shown_ = 0;
    exporting_ = new wxProgressDialog("Export audio", W("Exporting " + base_name(path)), 1000, this,
                                      wxPD_APP_MODAL | wxPD_CAN_ABORT | wxPD_ELAPSED_TIME | wxPD_REMAINING_TIME);
    export_timer_.Start(kExportPoll);
    std::shared_ptr<Mixer> mixer = mixer_;
    RenderRequest req = request();
    worker_->ask<RenderResult>(
        [this, mixer, req, path, cancel] {
            return mixer->render(req, path, [this, cancel](double done) {
                CallAfter([this, done] { export_progress(done); });
                return !*cancel;
            });
        },
        [this, path](RenderResult res) { exported(res, path); });
}

//: How far the export has got, 0 to 1, onto its window; below 0 only looks
//: whether Cancel was pressed.
void Studio::export_progress(double done) {
    if (!exporting_) return;
    if (done >= 0) export_shown_ = std::max(export_shown_, std::min(999, int(done * 1000)));
    if (!exporting_->Update(export_shown_) && export_cancel_) *export_cancel_ = true;
}

//: The export is over, written or given up: its window goes, and what
//: happened is said -- with the system's asterisk when it was written.
void Studio::exported(const RenderResult &res, const std::string &path) {
    export_timer_.Stop();
    delete exporting_;
    exporting_ = nullptr;
    export_cancel_.reset();
    rendering_ = false;
    mi_play_->Enable(true);
    if (!res.ok) {
        say(res.cancelled ? W("export cancelled: " + base_name(path) + " was not written")
                          : W("could not write " + base_name(path) + (res.error.empty() ? "" : ": " + res.error)));
        return;
    }
#ifdef __WXMSW__
    ::MessageBeep(MB_ICONASTERISK);
#endif
    say(W(printf_s("wrote %s, %.2f seconds", base_name(path).c_str(), res.seconds)));
}

//: File > Export MIDI: the song as a Standard MIDI File, as VocalWriter
//: exported one, each track on the channel asked for.
void Studio::on_export_midi(wxCommandEvent &) {
    std::vector<const Track *> tracks = midi_export_tracks(song_);
    if (tracks.empty()) {
        say("nothing to export: no track has any notes in it");
        return;
    }
    MidiChannelsDialog chans(this, tracks, default_midi_channels(tracks, program_map_));
    if (chans.ShowModal() != wxID_OK) return;
    std::string name = song_name();
    wxFileDialog dlg(this, "Export MIDI", "", W(name.empty() ? name : name + ".mid"), kMidiWildcard,
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    if (extension_lower(path).empty()) path += ".mid";
    std::vector<int> channels = chans.channels();
    std::vector<MidiOut> out;
    for (size_t k = 0; k < tracks.size(); ++k) out.push_back({tracks[k], k < channels.size() ? channels[k] : 0});
    try {
        export_midi(path, song_, out, program_map_);
    } catch (const std::exception &e) {
        say(W(std::string("could not export: ") + e.what()));
        return;
    }
    // written by now: the system's asterisk, as a WAV export ends (exported)
#ifdef __WXMSW__
    ::MessageBeep(MB_ICONASTERISK);
#endif
    say(W("wrote " + base_name(path) + ": " + plural(tracks.size(), "track")));
}

//: Every track as a file of its own, at its own volume and pan: stems, so
//: laying them on top of one another gives back the mix.
void Studio::on_export_tracks(wxCommandEvent &) {
    if (rendering_) {
        say("already rendering");
        return;
    }
    bool any = false;
    for (const Track &t : song_.tracks) any = any || !t.notes.empty();
    if (!any) {
        say("nothing to export: no track has any notes in it");
        return;
    }
    wxDirDialog dlg(this, "Export each track into this folder");
    if (dlg.ShowModal() != wxID_OK) return;
    std::string folder = U(dlg.GetPath());
    std::string base = path_.empty() ? "song" : base_name(strip_extension(path_));
    auto jobs = std::make_shared<std::vector<std::pair<Track, std::string>>>();
    for (const auto &j : export_jobs(song_.tracks, folder, base)) jobs->push_back({*j.first, j.second});
    std::vector<std::string> silent;
    auto heard_now = audible(song_.tracks);
    for (const Track &t : song_.tracks)
        if (!t.notes.empty() && std::find(heard_now.begin(), heard_now.end(), &t) == heard_now.end())
            silent.push_back(t.name);
    rendering_ = true;
    mi_play_->Enable(false);
    say(W("exporting " + plural(jobs->size(), "track") + " into " + folder));
    if (!silent.empty()) {
        std::string names;
        for (const auto &s : silent) names += (names.empty() ? "" : ", ") + s;
        say(W("including " + names + ", which " + (silent.size() == 1 ? "is" : "are") +
              " silent in the song at the moment"));
    }
    export_track(jobs, 0, 0);
}

//: One track, then the next: the engines do one at a time, and each one is
//: worth saying out loud.
void Studio::export_track(std::shared_ptr<std::vector<std::pair<Track, std::string>>> jobs, size_t i,
                          int written) {
    if (i >= jobs->size()) {
        rendering_ = false;
        mi_play_->Enable(true);
        size_t failed = jobs->size() - size_t(written);
        say(W("exported " + std::to_string(written) + " of " + plural(jobs->size(), "track") +
              (failed ? ", " + std::to_string(failed) + " failed" : "")));
        return;
    }
    std::string path = (*jobs)[i].second;
    say(W("rendering " + base_name(path) + ", " + std::to_string(i + 1) + " of " + std::to_string(jobs->size())));
    std::vector<const Track *> one{&(*jobs)[i].first};
    render(request(nullptr, 0.0, &one), path, [this, jobs, i, written, path](RenderResult res) {
        int w = written;
        if (res.ok) {
            ++w;
            say(W(printf_s("wrote %s, %.2f seconds", base_name(path).c_str(), res.seconds)));
        } else {
            say(W("could not write " + base_name(path)));
        }
        export_track(jobs, i + 1, w);
    });
}

// -- undo ----------------------------------------------------------------------------------

Studio::Snapshot Studio::snapshot() { return {song_, current_, selected()}; }

bool Studio::record(const Snapshot &before, const Snapshot &after, const std::string &label) {
    if (song_fingerprint(before.song) == song_fingerprint(after.song)) return false;
    undo_.push_back({before, after, label});
    if (undo_.size() > size_t(kMaxUndo)) undo_.erase(undo_.begin());
    redo_.clear();
    return true;
}

void Studio::history_status() { touch(song_fingerprint(song_) != saved_); }

//: Undo and Redo are available when there is something to undo -- and only
//: that is said here. On macOS this runs while AppKit searches the menus for
//: the item a key press belongs to (wxWidgets answers menuNeedsUpdate and
//: validateMenuItem by running it), and changing a label there resets the
//: item's key equivalent mid-search, which crashed the program now and then
//: on the first shortcut after an edit. The labels are set by
//: relabel_history, afterwards.
void Studio::on_update_history(wxUpdateUIEvent &evt) {
    bool undo = evt.GetId() == wxID_UNDO;
    evt.Enable(!(undo ? undo_ : redo_).empty());
}

//: Name the next undo and redo on the Edit menu.
void Studio::relabel_history() {
    relabel_pending_ = false;
    for (bool undo : {true, false}) {
        wxMenuItem *item = undo ? mi_undo_ : mi_redo_;
        if (!item) continue;
        const auto &entries = undo ? undo_ : redo_;
        std::string label = std::string(undo ? "Undo" : "Redo") +
                            (entries.empty() ? "" : " " + entries.back().label) + "\t" +
                            (undo ? "Ctrl+Z" : "Ctrl+Shift+Z");
        wxString text = K(label.c_str());
        if (item->GetItemLabel() != text) item->SetItemLabel(text);
    }
}

void Studio::restore_history(bool redo) {
    auto &source = redo ? redo_ : undo_;
    auto &target = redo ? undo_ : redo_;
    if (source.empty()) {
        say(redo ? "nothing to redo" : "nothing to undo");
        return;
    }
    Entry entry = source.back();
    source.pop_back();
    target.push_back(entry);
    const Snapshot &state = redo ? entry.after : entry.before;
    preview_timer_.Stop();
    stop_audio();
    song_ = state.song;
    sync_tracks(state.current);
    sync();
    select_only(state.selected);
    history_status();
    say(W(std::string(redo ? "redid " : "undid ") + entry.label));
}

void Studio::reset_history(bool saved_now) {
    undo_.clear();
    redo_.clear();
    saved_ = saved_now ? song_fingerprint(song_) : std::string();
}

// -- projects --------------------------------------------------------------------------------

//: Note that the song has changed, and show it in the title.
void Studio::touch(bool dirty) {
    dirty_ = dirty;
    // renamed after the event that changed it has finished, never during it
    if (!relabel_pending_) {
        relabel_pending_ = true;
        CallAfter([this] { relabel_history(); });
    }
    show_title();
    keep_a_copy();
}

//: The project's file, or the name of the file the song was imported from
//: until it is saved as a project, or Untitled.
void Studio::show_title() {
    std::string name = !path_.empty() ? base_name(path_) : !import_name_.empty() ? import_name_ : "Untitled";
    SetTitle(W((dirty_ ? "*" : "") + name + " - " + kAppName));
}

//: The synthesisers are in this process, so the song is written out a couple
//: of seconds after it stops changing.
void Studio::keep_a_copy() { recovery_timer_.StartOnce(kRecoveryWait); }

void Studio::write_copy() {
    bool any = false;
    for (const Track &t : song_.tracks) any = any || !t.notes.empty();
    if (!any) return;
    // with the voices' names, as a saved song has them (name_voices)
    Song named = song_;
    name_voices(named, voices_);
    recovery_write(named, path_);
}

void Studio::offer_recovery() {
    auto left = recovery_waiting();
    if (!left) return;
    std::string from = left->was_path.empty() ? "" : ", from " + base_name(left->was_path);
    exclaim();
    int answer = wxMessageBox(
        W(std::string(kAppName) + " did not close properly last time, and the song you were working on "
                                   "was kept.\n\n" +
          std::to_string(left->notes) + " notes" + from + ".\n\nOpen it?"),
        "Recover the song", wxYES_NO | wxICON_QUESTION, this);
    if (answer != wxYES) {
        recovery_clear();
        return;
    }
    Song song;
    try {
        song = load_project(left->file);
    } catch (const std::exception &e) {
        say(W(std::string("the kept song could not be read: ") + e.what()));
        recovery_clear();
        return;
    }
    take(song, left->was_path);
    saved_.clear();
    touch(true);
    int notes_n = 0;
    for (const Track &t : song_.tracks) notes_n += int(t.notes.size());
    say(W("recovered " + std::to_string(notes_n) + " notes" + from + ". Save it somewhere before going on."));
}

//: Ask before throwing away unsaved work. True to go ahead.
bool Studio::may_discard(const wxString &what) {
    if (!dirty_) return true;
    exclaim();
    int answer = wxMessageBox("This song has changes that have not been saved. " + what + " anyway?", kAppName,
                              wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
    return answer == wxYES;
}

//: Replace the song with what was loaded or imported.
void Studio::take(const Song &song, const std::string &path) {
    drop_playback();
    song_ = song;
    resolve_voice_names(song_, voices_);
    if (song_.tracks.empty()) {
        Track t;
        t.name = "Voice 1";
        song_.tracks.push_back(t);
    }
    song_.bpm = double(std::max(long(kMinBpm), std::min(long(kMaxBpm), std::lrint(song.bpm))));
    song_.sig = parse_sig(format_sig(song.sig));
    song_.consonants = std::max(10L, std::min(100L, std::lrint(song.consonants * 100))) / 100.0;
    song_.voice = clean_voice(song.voice);
    path_ = path;
    sync_tracks(0);
    sync(0);
    // as it came in, nothing changed yet: an imported song is still in the
    // file it came from, so only an edit makes it worth asking about
    reset_history(true);
    touch(false);
    import_name_.clear();
}

void Studio::on_new(wxCommandEvent &) {
    if (!may_discard("Start a new song")) return;
    to_tracks();
    drop_playback();
    Track t;
    t.name = "Voice 1";
    t.program = track().program;
    song_.tracks = {t};
    path_.clear();
    import_name_.clear();
    sync_tracks(0);
    sync();
    reset_history(true);
    touch(false);
    say("new song");
}

void Studio::on_open(wxCommandEvent &) {
    if (!may_discard("Open another project")) return;
    wxFileDialog dlg(this, "Open project", "", "", kProjectWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    open_project(U(dlg.GetPath()));
}

//: Open a project by name: the file dialog, and the command line.
void Studio::open_project(const std::string &path) {
    if (is_vocalwriter_song(path)) {
        import_vocalwriter(path);
        return;
    }
    if (is_midi_file(path)) {
        take_midi(path, false);
        return;
    }
    if (is_audio_file(path)) {
        open_sound(path);
        return;
    }
    Song song;
    try {
        song = load_project(path);
    } catch (const std::exception &e) {
        // anything not named as a project is not a file this program opens
        cannot_load(path, extension_lower(path) == kProjectSuffix
                              ? std::string(e.what())
                              : "it is not a project, a MIDI file, a VocalWriter song or a sound file");
        return;
    }
    double bpm = song.bpm;
    take(song, path);
    int n = 0;
    for (const Track &t : song_.tracks) n += int(t.notes.size());
    say(W("opened " + base_name(path) + ": " + plural(song_.tracks.size(), "track") + ", " + std::to_string(n) +
          " notes, " + fmt_g(bpm) + " bpm"));
    to_tracks();
}

//: A song just come in starts in the tracks list, wherever the keyboard was;
//: after the dialogs that brought it in have given the focus back.
void Studio::to_tracks() {
    CallAfter([this] { tracks_list_->SetFocus(); });
}

void Studio::on_save(wxCommandEvent &e) {
    if (!path_.empty()) write_project(path_);
    else on_save_as(e);
}

void Studio::on_save_as(wxCommandEvent &) {
    // named after the file the song came from, if it was imported
    std::string name = song_name();
    wxFileDialog dlg(this, "Save project", "", W((name.empty() ? std::string("song") : name) + kProjectSuffix),
                     kProjectWildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    if (extension_lower(path) != kProjectSuffix) path += kProjectSuffix;
    write_project(path);
}

void Studio::write_project(const std::string &path) {
    try {
        Song named = song_;
        name_voices(named, voices_);
        save_project(path, named);
    } catch (const std::exception &e) {
        say(W(std::string("could not save: ") + e.what()));
        return;
    }
    path_ = path;
    saved_ = song_fingerprint(song_);
    touch(false);
    int n = 0;
    for (const Track &t : song_.tracks) n += int(t.notes.size());
    say(W("saved " + base_name(path) + ": " + plural(song_.tracks.size(), "track") + ", " + std::to_string(n) +
          " notes"));
}

// -- MIDI ------------------------------------------------------------------------------------

void Studio::on_import(wxCommandEvent &) {
    if (!may_discard("Import over it")) return;
    wxFileDialog dlg(this, "Import", "", "", kImportWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    if (is_vocalwriter_song(path)) {
        import_vocalwriter(path);
        return;
    }
    take_midi(path, false);
}

//: File > Append MIDI: a MIDI file's parts under the song's tracks, its
//: tempo map in place of the song's.
void Studio::on_append(wxCommandEvent &) {
    wxFileDialog dlg(this, "Append MIDI", "", "", kMidiWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    take_midi(U(dlg.GetPath()), true);
}

//: A MIDI file's parts, chosen as Import asks: as a new song, or with
//: `append` under this one.
void Studio::take_midi(const std::string &path, bool append) {
    // which parts the GM Soundbank can play is known once the voices are,
    // and which voice a sung part's program changes to once the program map is
    if (voices_.empty() || !programs_known_) {
        if (append) say("the voices have not loaded yet. Try again in a moment.");
        else midi_waiting_ = path;
        return;
    }
    // what plays the parts not sung, and so what an unnamed part is called
    int slot = soundbank_slot();
    ProgramNames program_names;
    if (slot >= 0)
        program_names = [slot](int program, bool drums) { return soundfont_program_name(program, drums, slot); };
    std::vector<MidiPart> parts;
    try {
        parts = midi_parts(path, -1, program_names);
    } catch (const std::exception &e) {
        cannot_load(path, e.what());
        return;
    }
    if (parts.empty()) {
        cannot_load(path, "there are no notes in it");
        return;
    }
    // A type 0 file's lyrics could be sung on any of several channels
    LyricChoice choice;
    try {
        choice = lyric_choice(path);
    } catch (const std::exception &) {
    }
    bool ask_lyrics = choice.channels.size() > 1;
    // what a part is, as the boxes below say it: its instrument too, unless
    // named after it; not "with words" while which part sings is to be asked
    auto described = [ask_lyrics, slot](const MidiPart &p) {
        std::string what = p.words && !ask_lyrics ? "with words"
                           : p.drums              ? "drums"
                           : slot >= 0            ? soundfont_program_name(p.program, false, slot)
                                                  : gm_program_name(p.program);
        bool said = p.name.size() >= what.size() &&
                    std::equal(what.begin(), what.end(), p.name.begin(),
                               [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
        return p.name + ", " + std::to_string(p.notes) + " notes" + (said ? "" : ", " + what);
    };
    if (parts.size() > 1) {
        // Every part at once is the default: a file with several parts in it
        // is usually several parts of one song.
        wxArrayString labels;
        for (const auto &p : parts) labels.Add(W(described(p)));
        ImportPartsDialog pick(this, labels);
        if (pick.ShowModal() != wxID_OK) return;
        std::vector<MidiPart> kept;
        for (int i : pick.chosen()) kept.push_back(parts[size_t(i)]);
        parts = kept;
    }
    // Then which of the parts chosen sings the lyrics, the first to begin
    // with; one sings them unasked, and with none every part is played.
    int lyrics_on = -1;
    if (ask_lyrics) {
        wxArrayString labels;
        std::vector<int> channels;
        for (const MidiPart &p : parts) {
            if (std::find(choice.channels.begin(), choice.channels.end(), p.channel) == choice.channels.end()) continue;
            channels.push_back(p.channel);
            labels.Add(W(described(p)));
        }
        if (channels.size() > 1) {
            LyricTrackDialog ask(this, labels);
            if (ask.ShowModal() != wxID_OK || ask.chosen() < 0) return;
            lyrics_on = channels[size_t(ask.chosen())];
        } else if (channels.size() == 1) {
            lyrics_on = channels[0];
        }
        // the same parts, the words now on the one that sings them
        std::vector<std::string> wanted;
        for (const MidiPart &p : parts) wanted.push_back(p.name);
        try {
            parts.clear();
            for (const MidiPart &p : midi_parts(path, lyrics_on, program_names))
                if (std::find(wanted.begin(), wanted.end(), p.name) != wanted.end()) parts.push_back(p);
        } catch (const std::exception &e) {
            cannot_load(path, e.what());
            return;
        }
    }
    std::vector<std::string> names, played;
    for (const auto &p : parts) names.push_back(p.name);
    // The parts with words are sung; the rest are played by the default
    // soundbank, a SoundFont or the GM Soundbank.
    bool gm = false;
    for (const VoiceListing &v : voices_) gm = gm || (v.id == kGmBase && v.available);
    if (gm || slot >= 0) {
        played = parts_to_play(parts);
        // Without words, which parts sing is asked all the same: they come in
        // to be sung, their short rests closed, their words added later. Or none.
        bool words = std::any_of(parts.begin(), parts.end(), [](const MidiPart &p) { return p.words; });
        std::vector<std::string> singable;
        wxArrayString labels;
        for (const MidiPart &p : parts)
            if (!p.drums) {
                singable.push_back(p.name);
                labels.Add(W(described(p)));
            }
        if (!words && !singable.empty()) {
            SungPartsDialog ask(this, labels);
            if (ask.ShowModal() != wxID_OK) return;
            for (int i : ask.chosen())
                played.erase(std::remove(played.begin(), played.end(), singable[size_t(i)]), played.end());
        }
    } else {
        say("the GM Soundbank cannot play here, so every part is sung: VocalWriter's instrument bank, "
            "GMBank.rsrc, is not in assets");
    }
    Imported got;
    try {
        std::vector<int> programs(128, -1);
        for (const auto &pv : program_map_)
            if (pv.first >= 0 && pv.first < 128) programs[size_t(pv.first)] = pv.second;
        got = import_midi(path, names, rests_to_close(), played, settings_.remove_note_velocities, lyrics_on,
                          programs, settings_.close_word_rests, settings_.remove_volume_changes, program_names);
        if (slot >= 0)
            for (ImportedPart &p : got.parts)
                if (p.played) play_on_soundfont(p.voice_id, p.notes, slot);
    } catch (const std::exception &e) {
        cannot_load(path, e.what());
        return;
    }
    // the tempo and time signature changes on a track of their own, above the parts
    std::vector<Track> tracks;
    bool tempo_map = !got.tempo_changes.empty() || !got.sig_changes.empty();
    if (tempo_map) tracks.push_back(tempo_track(got.tempo_changes, got.sig_changes));
    int program = track().program;
    size_t sung = 0;
    for (const ImportedPart &p : got.parts) {
        Track t = imported_track(p);
        t.program = program;
        sung += !p.played;
        tracks.push_back(t);
    }
    size_t first = 0;
    if (append) {
        undoable("append MIDI", [&] {
            drop_playback();
            // its tempo and time signature in place of the song's
            drop_tempo_map(song_);
            song_.bpm = double(std::max(long(kMinBpm), std::min(long(kMaxBpm), std::lrint(got.bpm))));
            song_.sig = parse_sig(format_sig(got.sig));
            first = song_.tracks.size();
            song_.tracks.insert(song_.tracks.end(), tracks.begin(), tracks.end());
            touch();
            sync_tracks(int(first));
            sync(0);
        });
    } else {
        // as the original: the imported song keeps the consonant length, and
        // starts its voice controls, reverb and timing afresh
        Song song = fresh_song();
        song.bpm = got.bpm;
        song.sig = got.sig;
        song.consonants = song_.consonants;
        song.tracks = tracks;
        take(song, "");
    }
    // what a file made of the song is called, until it is saved as a project
    if (path_.empty() && (!append || import_name_.empty())) import_name_ = base_name(strip_extension(path));
    show_title();
    size_t total = 0;
    for (const auto &p : got.parts)
        for (const Note &n : p.notes) total += !n.controller;
    say(W(std::string(append ? "appended " : "imported ") + base_name(path) + ": " + plural(got.parts.size(), "track") +
          ", " + std::to_string(total) + " notes, " + fmt_g(std::nearbyint(got.bpm)) + " bpm, " + format_sig(got.sig) +
          (append ? ", its tempo and time signature now the song's" : "")));
    if (got.closed_rests) say(W(plural(size_t(got.closed_rests), "short rest") + " closed"));
    if (tempo_map) {
        sync_tracks(int(first) + 1);
        sync(0);
        say(W(tempo_map_said(got.tempo_changes.size(), got.sig_changes.size()) +
              (append ? " on a Tempo track above its parts" : " on the Tempo track at the top")));
    }
    bool any_pending = false, any_words = false;
    for (const auto &p : got.parts) {
        any_pending = any_pending || !p.pending.empty();
        for (const Note &n : p.notes) any_words = any_words || !n.word.empty();
    }
    if (!any_pending && !any_words && sung)
        say(W(std::string("it carried no words, so every note sings ") + kDefaultPhoneme +
              ". Ctrl+W puts a word on one."));
    if (!played.empty())
        say(W(plural(played.size(), "part") + " played by " + soundbank_name(slot) +
              ", each with the instrument the file gives it; the drums on its drum kit"));
    if (sung > 1) {
        auto it = program_map_.find(program);
        say(W("every part that sings is singing in " + voice_name(it == program_map_.end() ? 0 : it->second) +
              ". Enter on a track gives it its own voice, volume and pan."));
    }
    std::vector<std::string> words;
    size_t pending = 0;
    for (const auto &p : got.parts)
        for (const auto &pw : p.pending) {
            ++pending;
            if (std::find(words.begin(), words.end(), pw.first) == words.end()) words.push_back(pw.first);
        }
    if (pending) {
        std::sort(words.begin(), words.end());
        say(W("looking up " + plural(words.size(), "word")));
        std::vector<ImportedPart> parts_copy = got.parts;
        lookup(
            words,
            [this, parts_copy, pending](std::map<std::string, std::vector<std::string>> found) {
                imported_words(parts_copy, found, pending);
            },
            true);
    }
    to_tracks();
}

//: A VocalWriter song comes in whole: its tempo changes, voices, controls,
//: reverb and end, so that it sings as VocalWriter sang it.
void Studio::import_vocalwriter(const std::string &path) {
    // its voice changes are read through the program map, which is still
    // being loaded when a song is opened from the command line
    if (!programs_known_) {
        vw_waiting_ = path;
        return;
    }
    std::vector<int> programs(128, -1);
    for (const auto &pv : program_map_)
        if (pv.first >= 0 && pv.first < 128) programs[size_t(pv.first)] = pv.second;
    VwSongImport got;
    try {
        got = import_vocalwriter_song(path, programs);
    } catch (const std::exception &e) {
        cannot_load(path, e.what());
        return;
    }
    // its instruments on the default soundbank
    int slot = soundbank_slot();
    if (slot >= 0)
        for (Track &t : got.song.tracks) {
            std::optional<int> was = t.voice_id;
            play_on_soundfont(t.voice_id, t.notes, slot);
            if (t.voice_id != was) t.voice_name.reset();
        }
    Clock clock = got.song.clock();
    take(got.song, "");
    import_name_ = base_name(strip_extension(path));
    show_title();
    int n = 0, parts = 0;
    for (const Track &t : song_.tracks) {
        parts += sings(t);
        for (const Note &x : t.notes) n += !x.controller;
    }
    say(W("imported " + base_name(path) + ": " + plural(size_t(parts), "track") + ", " + std::to_string(n) +
          " notes, " + fmt_g(clock.bpm) + " bpm, " + format_sig(song_.sig)));
    // the tempo changes are on a track at the top; the first part is shown
    if (!clock.changes.empty()) {
        sync_tracks(1);
        sync(0);
        say(W(plural(clock.changes.size(), "tempo change") + " on the Tempo track at the top"));
    }
    for (const std::string &note : got.notes) say(W(note));
    to_tracks();
}

//: A sound file opened as a song: a new song named after it, the file on a
//: backing track under Voice 1, as Add backing track would put it there.
void Studio::open_sound(const std::string &path) {
    std::string why;
    if (!probe_audio(path, &why)) {
        cannot_load(path, why);
        return;
    }
    take(backing_song(path), "");
    import_name_ = base_name(strip_extension(path));
    show_title();
    say(W("opened " + base_name(path) + " as a backing track under Voice 1, at half volume, from the start of "
          "the song"));
    to_tracks();
}

//: A file that could not be opened or imported: said in a box to be put
//: away with OK, with the system's critical stop, and in Messages.
void Studio::cannot_load(const std::string &path, const std::string &why) {
    std::string said = base_name(path) + " could not be loaded: " + why + ".";
    say(W(said));
    critical_stop();
    wxMessageBox(W(said), kAppName, wxOK | wxICON_ERROR, this);
}

//: Fill in the pronunciations for a MIDI that carried only lyrics.
void Studio::imported_words(std::vector<ImportedPart> parts,
                            const std::map<std::string, std::vector<std::string>> &found, size_t pending) {
    // straight after an import it is part of the import, not an edit of its own
    bool as_imported = !dirty_ && undo_.empty();
    undoable("pronounce imported words", [&] {
        int got = pronounce(parts, found, settings_.plain_held_vowels);
        // the parts are the last tracks: a Tempo track may come first
        size_t first = song_.tracks.size() > parts.size() ? song_.tracks.size() - parts.size() : 0;
        for (size_t ti = 0; first + ti < song_.tracks.size() && ti < parts.size(); ++ti)
            song_.tracks[first + ti].notes = parts[ti].notes;
        sync(0);
        touch();
        size_t missing = pending - size_t(got);
        say(W(plural(size_t(got), "word") + " pronounced" +
              (missing ? ", " + std::to_string(missing) + " not in the dictionary -- those notes sing " +
                             kDefaultPhoneme + " until you say otherwise"
                       : "")));
    });
    if (as_imported) {
        reset_history(true);
        touch(false);
    }
}

void Studio::on_close(wxCloseEvent &evt) {
    if (evt.CanVeto() && !may_discard("Close")) {
        evt.Veto();
        return;
    }
    stop_audio();
    preview_timer_.Stop();
    recovery_timer_.Stop();
    follow_timer_.Stop();
    export_timer_.Stop();
    worker_->close();
    // closing properly is what says the kept copy is not needed
    recovery_clear();
    Destroy();
}

}  // namespace svs
