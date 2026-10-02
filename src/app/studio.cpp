#include "app/studio.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <wx/choicdlg.h>
#include <wx/clipbrd.h>
#include <wx/dirdlg.h>
#include <wx/filedlg.h>
#include <wx/textdlg.h>

#include "app/announce.h"
#include "app/dialogs.h"
#include "core/paths.h"
#include "voices/controls.h"
#include "voices/registry.h"
#include "voices/vocalwriter.h"

namespace svs {

namespace {

const char *const kAppName = "Singing Voice Studio";

enum {
    ID_ADD_WORD = wxID_HIGHEST + 1, ID_ADD_NOTE, ID_EDIT, ID_REMOVE,
    ID_UP, ID_DOWN, ID_LONGER, ID_SHORTER,
    ID_PLAY, ID_HEAR, ID_STOP, ID_KEYS,
    ID_ADD_REST, ID_IMPORT, ID_EXPORT, ID_AUTO_PREVIEW, ID_EXPORT_TRACKS,
    ID_BAR_REST, ID_GOTO_BAR, ID_METRONOME, ID_PLAY_PAUSE, ID_PANES,
    ID_TRACK_NEW, ID_TRACK_EDIT, ID_TRACK_REMOVE, ID_TRACK_MUTE, ID_TRACK_SOLO,
    ID_TRACK_UP, ID_TRACK_DOWN, ID_PREVIEW_TIMER, ID_RECOVERY_TIMER,
};

//: A beat is a quarter note, so a sixteenth is a quarter of a beat: the step
//: Alt with Left and Right moves a note by, the same step every time.
const double kSixteenth = 0.25;
//: A nudge will not take a note below a thirty-second note. The floor only
//: refuses; it never shortens the step to fit.
const double kMinBeats = 0.125;
//: how long the song has to stop changing before a copy is kept
const int kRecoveryWait = 2500;
//: how long the nudging has to stop before a note is previewed
const int kPreviewWait = 180;

const int kMaxUndo = 100;

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
      recovery_timer_(this, ID_RECOVERY_TIMER) {
    Track first;
    first.name = "Voice 1";
    song_.tracks.push_back(first);
    wav_ = join_path(temp_dir(), "svs_studio.wav");
    preview_wav_ = join_path(temp_dir(), "svs_note.wav");
    settings_ = load_settings();
    mixer_ = std::make_shared<Mixer>();
    worker_ = std::make_unique<Worker>(this);
    worker_->on_error = [this](const std::string &msg) { say("engine error: " + W(msg)); };

    build();
    reset_history(true);
    build_menu();
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { preview_now(preview_row_); }, ID_PREVIEW_TIMER);
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { write_copy(); }, ID_RECOVERY_TIMER);

    // The engines answer in order, so the cheap things are asked for first.
    worker_->ask<std::vector<std::string>>([] { return Registry::get().status(); },
                                           [this](std::vector<std::string> lines) {
                                               (void)lines;
                                               ready();
                                           });
    worker_->ask<std::vector<VoiceListing>>([] { return Registry::get().voices(); },
                                            [this](std::vector<VoiceListing> v) { set_voices(v); });
    worker_->ask<std::vector<int>>([] { return vocalwriter_program_voices(); },
                                   [this](std::vector<int> picks) { set_program_map(picks); });
    Bind(wxEVT_CLOSE_WINDOW, &Studio::on_close, this);
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
    if (worker_) worker_->close();
}

void Studio::build() {
    wxPanel *p = new wxPanel(this);
    wxBoxSizer *outer = new wxBoxSizer(wxVERTICAL);

    // The tracks come before the notes, so that Tab runs down the window in
    // the order the song is put together: which part, then what it sings.
    tracks_list_ = new ReportList(p, wxLC_REPORT | wxLC_SINGLE_SEL, wxSize(-1, 110));
    const std::pair<const char *, int> track_columns[] = {
        {"Track", 150}, {"Voice", 160}, {"Volume", 70}, {"Pan", 90}, {"State", 90}};
    int c = 0;
    for (const auto &col : track_columns) tracks_list_->InsertColumn(c++, col.first, wxLIST_FORMAT_LEFT, col.second);
    tracks_list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &) {
        wxCommandEvent e;
        on_track_edit(e);
    });
    tracks_list_->Bind(wxEVT_LIST_ITEM_SELECTED, &Studio::on_track_chosen, this);
    tracks_list_->Bind(wxEVT_KEY_DOWN, &Studio::on_track_key, this);
    caption(p, outer, "Tracks", tracks_list_, 0);

    // not single-select: a phrase is copied, transposed or deleted as a
    // whole, and Shift with the arrow keys is how that is reached
    list_ = new ReportList(p, wxLC_REPORT);
    const std::pair<const char *, int> columns[] = {{"Phonemes", 165}, {"Pitch", 55}, {"Beats", 150},
                                                    {"Word", 95},      {"Bend", 90},  {"Mod", 80},
                                                    {"Bar", 55}};
    c = 0;
    for (const auto &col : columns) list_->InsertColumn(c++, col.first, wxLIST_FORMAT_LEFT, col.second);
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &) {
        wxCommandEvent e;
        on_edit(e);
    });
    list_->Bind(wxEVT_KEY_DOWN, &Studio::on_key, this);
    caption(p, outer, "Notes", list_);

    messages_ = new wxTextCtrl(p, wxID_ANY, "", wxDefaultPosition, wxSize(-1, 80),
                               wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    labelled(p, outer, "Messages", messages_, 1);

    p->SetSizer(outer);
    status_ = CreateStatusBar();
    sync_tracks(0);
    say("starting the engine");
}

// Menus rather than buttons: a screen reader announces an item's shortcut
// along with its name, so the keys are discoverable.
void Studio::build_menu() {
    wxMenuBar *bar = new wxMenuBar();

    wxMenu *note = new wxMenu();
    note->Append(ID_ADD_WORD, "Add &word...\tCtrl+W", "Look a word up and spread it over one note or several");
    note->Append(ID_ADD_NOTE, "Add &note...\tCtrl+N", "Add a note and choose its phonemes");
    note->Append(ID_ADD_REST, "Add &rest\tCtrl+R", "Add a silent break of the same length");
    note->Append(ID_BAR_REST, "Rest to the end of the &bar\tCtrl+Shift+R",
                 "Add a rest long enough to reach the next bar line");
    note->Append(ID_EDIT, "&Edit note...\tCtrl+E", "Edit the selected note");
    note->AppendSeparator();
    // No tab, so no global accelerator: Ctrl+C belongs to whatever text field
    // has the focus. The notes list handles these keys itself.
    note->Append(wxID_CUT, "Cu&t (Ctrl+X on a note)", "Remove the selected notes and keep them to paste");
    note->Append(wxID_COPY, "&Copy (Ctrl+C on a note)", "Keep the selected notes to paste");
    note->Append(wxID_PASTE, "&Paste (Ctrl+V on a note)", "Put the kept notes in after the selection");
    note->Append(wxID_SELECTALL, "Select &all (Ctrl+A on a note)", "Select every note in the song");
    note->AppendSeparator();
    note->Append(ID_REMOVE, "&Remove note\tCtrl+D", "Remove the selected note");
    note->AppendSeparator();
    note->Append(ID_UP, "Transpose &up (Alt+Up on a note)", "Up one semitone");
    note->Append(ID_DOWN, "Transpose &down (Alt+Down on a note)", "Down one semitone");
    note->Append(ID_LONGER, "&Longer\tAlt+Right", "One sixteenth note longer");
    note->Append(ID_SHORTER, "&Shorter\tAlt+Left", "One sixteenth note shorter");

    wxMenu *track = new wxMenu();
    track->Append(ID_TRACK_NEW, "&New track\tCtrl+T", "Add another part, with its own voice");
    track->Append(ID_TRACK_EDIT, "&Track settings... (Enter on a track)", "Name, voice, volume and pan");
    track->Append(ID_TRACK_REMOVE, "&Remove track (Delete on a track)",
                  "Remove the selected track and everything in it");
    track->AppendSeparator();
    // No tab, so no accelerator: S and M would be unusable in every text field.
    track->Append(ID_TRACK_MUTE, "&Mute or unmute (M on a track)", "Silence this track");
    track->Append(ID_TRACK_SOLO, "&Solo or unsolo (S on a track)", "Hear only the soloed tracks");
    track->AppendSeparator();
    track->Append(ID_TRACK_UP, "Move track &up (Ctrl+Up on a track)", "Put this part earlier in the list");
    track->Append(ID_TRACK_DOWN, "Move track &down (Ctrl+Down on a track)", "Put this part later in the list");

    wxMenu *play = new wxMenu();
    // Space is handled by the lists rather than registered as an
    // accelerator, or it could never be typed into a word again.
    mi_play_ = play->Append(ID_PLAY_PAUSE, "Play or &stop (Space)",
                            "Play from the note the cursor is on, or stop if it is going");
    // Handled directly rather than as accelerators, keeping the shortcut
    // visible in the label without a menu activation being announced.
    play->Append(ID_PLAY, "Play from the &start (Ctrl+P)", "Sing every track from the beginning");
    play->Append(ID_HEAR, "&Hear note (Ctrl+H)", "Sing the selected note");
    play->Append(ID_STOP, "&Stop\tCtrl+.", "Stop playing");
    play->AppendSeparator();
    mi_metronome_ = play->AppendCheckItem(ID_METRONOME, "&Metronome\tCtrl+M",
                                          "Tick along with Play. Never written into an exported file.");

    wxMenu *go = new wxMenu();
    go->Append(ID_GOTO_BAR, "Go to &bar...\tCtrl+G", "Jump to the first note in a bar");
    go->Append(ID_PANES, "&Tracks or notes\tF6", "Move between the tracks list and the notes list");

    wxMenu *f = new wxMenu();
    f->Append(wxID_NEW, "&New\tCtrl+Shift+N", "Start an empty song");
    f->Append(wxID_OPEN, "&Open project...\tCtrl+O", "Reopen a song saved earlier");
    f->Append(wxID_SAVE, "&Save project\tCtrl+S", "Save the song");
    f->Append(wxID_SAVEAS, "Save project &as...", "Save the song under a new name");
    f->AppendSeparator();
    f->Append(ID_IMPORT, "&Import MIDI...\tCtrl+I", "Take the notes from a MIDI file");
    f->Append(ID_EXPORT, "&Export WAV...\tCtrl+Shift+S", "Write the whole song to one file");
    f->Append(ID_EXPORT_TRACKS, "Export &tracks...\tCtrl+Shift+T", "Write every track to a file of its own");
    f->AppendSeparator();
    f->Append(wxID_PREFERENCES, "&Song settings...\tCtrl+,",
              "Tempo, time signature, consonants, reverb, voice controls");
    f->AppendSeparator();
    f->Append(wxID_EXIT, "E&xit\tAlt+F4");

    wxMenu *help = new wxMenu();
    help->Append(ID_KEYS, "&Keys\tF1", "List the shortcuts in Messages");

    wxMenu *edit = new wxMenu();
    edit->Append(wxID_UNDO, "&Undo\tCtrl+Z", "Undo the last song edit");
    edit->Append(wxID_REDO, "&Redo\tCtrl+Shift+Z", "Redo the last undone edit");
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
    mi_auto_preview_ = prefs->AppendCheckItem(ID_AUTO_PREVIEW, "&Preview notes as they change\tCtrl+Shift+P",
                                              "Hear a note whenever its pitch or length is nudged");
    mi_auto_preview_->Check(settings_.auto_preview);
    bar->Append(prefs, "&Settings");
    bar->Append(help, "&Help");
    SetMenuBar(bar);
    Bind(wxEVT_CHAR_HOOK, &Studio::on_playback_shortcut, this);

    auto on = [this](int id, void (Studio::*fn)(wxCommandEvent &)) { Bind(wxEVT_MENU, fn, this, id); };
    on(ID_ADD_WORD, &Studio::on_add_word);
    on(ID_ADD_NOTE, &Studio::on_add_note);
    on(ID_ADD_REST, &Studio::on_add_rest);
    on(ID_BAR_REST, &Studio::on_bar_rest);
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
    on(ID_METRONOME, &Studio::on_metronome);
    on(ID_TRACK_NEW, &Studio::on_track_new);
    on(ID_TRACK_EDIT, &Studio::on_track_edit);
    on(ID_TRACK_REMOVE, &Studio::on_track_remove);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { toggle_track(false); }, ID_TRACK_MUTE);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { toggle_track(true); }, ID_TRACK_SOLO);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { move_tracks(-1); }, ID_TRACK_UP);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { move_tracks(1); }, ID_TRACK_DOWN);
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
    on(ID_EXPORT, &Studio::on_export);
    on(ID_EXPORT_TRACKS, &Studio::on_export_tracks);
    on(wxID_PREFERENCES, &Studio::on_song_settings);
    on(ID_AUTO_PREVIEW, &Studio::on_auto_preview);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
}

//: Ctrl+H and Ctrl+P played directly, without a menu activation announcement.
void Studio::on_playback_shortcut(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    if (evt.ShiftDown()) {
        evt.Skip();
        return;
    }
    bool command = evt.ControlDown() && !evt.AltDown();
    wxCommandEvent e;
    if (code == 'H' && command) {
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
        SongSettingsDialog dlg(this, song_);
        if (dlg.ShowModal() != wxID_OK) return;
        Song was = song_;
        dlg.apply(song_);
        song_.bpm = std::max(30.0, std::min(250.0, song_.bpm));
        bool changed = song_.bpm != was.bpm || song_.sig != was.sig || song_.consonants != was.consonants ||
                       song_.voice != was.voice || song_.reverb != was.reverb ||
                       song_.anticipate != was.anticipate;
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

//: Hear one note once the nudging has stopped: a held arrow key would
//: otherwise queue a stream of little sounds, each already out of date.
void Studio::preview_note(int i) {
    if (!settings_.auto_preview || rendering_) return;
    preview_row_ = i;
    preview_timer_.StartOnce(kPreviewWait);
}

void Studio::preview_now(int i) {
    if (i < 0 || i >= int(notes().size()) || rendering_) return;
    const Note &note = notes()[size_t(i)];
    if (note.phonemes.empty()) return;          // a rest has nothing to hear
    stop_audio();
    rendering_ = true;
    render(request(new std::vector<Note>{note}), preview_wav_, [this](RenderResult res) { heard(res); });
}

void Studio::on_keys(wxCommandEvent &) {
    const char *lines[] = {
        "Ctrl+Z  undo, Ctrl+Shift+Z  redo",
        "F6  move between the tracks list and the notes list",
        "Ctrl+T  add a track. Each track has its own voice, volume, pan and notes",
        "Enter on a track  its name, voice, volume and pan",
        "M on a track  mute it, S  solo it",
        "Delete on a track  remove it",
        "Ctrl+Up or Ctrl+Down on a track  reorder the parts",
        "Ctrl+W  add word",
        "Ctrl+N  add note",
        "Ctrl+R  add a rest, a silent break",
        "Ctrl+Shift+R  rest to the end of the bar",
        "Ctrl+G  go to a bar",
        "Ctrl+E  edit note",
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
        "Pitch bend and the mod wheel are set per note, in the note editor. The mod wheel deepens "
        "the vibrato",
        "Space  play from the note the cursor is on, or stop if it is playing",
        "Ctrl+P  play every track from the start",
        "Ctrl+H  hear the selected note",
        "Ctrl+M  metronome on or off, for playing only",
        "Ctrl+. stop",
        "Ctrl+O open a project, Ctrl+S save it",
        "Ctrl+I  import a MIDI file",
        "Ctrl+Shift+S  export the whole song as one WAV",
        "Ctrl+Shift+T  export every track to its own WAV",
        "Enter on a note edits it",
    };
    for (const char *line : lines) say(line);
}

void Studio::say(const wxString &text) {
    status_->SetStatusText(text);
    messages_->AppendText(text + "\n");
}

//: Speak a shortcut's result after updating its row, keeping focus.
void Studio::announce_note(const wxString &text, wxListCtrl *control, int row) {
    say(text);
    if (announce(control, text)) return;
    reannounce(control, row);
}

void Studio::ready() {
    auto voices = Registry::get().voices();
    size_t ready_count = 0;
    for (const auto &v : voices) ready_count += v.available ? 1 : 0;
    say(W("engines ready, " + std::to_string(ready_count) + " voices"));
    for (const std::string &line : Registry::get().status()) say(W(line));
    say(W("this is build " + build_stamp()));
    VwAssets &a = VwAssets::get();
    if (a.ok() && !a.has_bank())
        // Without GMBank the voices built on its wavetables cannot be chosen,
        // so name the missing file rather than letting someone find out.
        say("the instrument bank, GMBank.rsrc, is missing: the VocalWriter voices with instrument "
            "names cannot be used");
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
    sync_tracks(current_);
}

void Studio::set_voices(std::vector<VoiceListing> v) {
    voices_ = std::move(v);
    sync_tracks(current_);        // the Voice column can now say
}

std::string Studio::voice_name(int id) const {
    for (const VoiceListing &v : voices_)
        if (v.id == id) return v.name;
    if (voices_.empty() && id == 0) return "Robert";
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
    tracks_list_->SetItem(i, 1, W(voice_name(track_voice_of(t))));
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
void Studio::on_track_chosen(wxListEvent &evt) {
    evt.Skip();
    int i = tracks_list_->FirstSelected();
    if (switching_ || i == current_ || i < 0 || i >= int(song_.tracks.size())) return;
    track().cursor = std::max(0, selection());
    current_ = i;
    const Track &t = track();
    relabel(list_, W("Notes in " + t.name));
    sync(t.cursor);
    say(W(t.name + ", " + voice_name(track_voice_of(t)) + ", " + plural(t.notes.size(), "note") + ", " +
          track_state(t)));
}

void Studio::on_track_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    bool plain = !(evt.ControlDown() || evt.AltDown() || evt.ShiftDown());
    wxCommandEvent e;
    if (evt.ControlDown() && (code == WXK_UP || code == WXK_DOWN))
        move_tracks(code == WXK_UP ? -1 : 1);
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

int Studio::track_at() const {
    int i = tracks_list_->FirstSelected();
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

void Studio::on_track_edit(wxCommandEvent &) {
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
        // a voice that cannot sing here is worth saying at once, not at Play
        Family *f = Registry::get().family_of(track_voice_of(t));
        std::string why;
        if (f && !f->available(&why)) say(W(why));
    });
}

void Studio::on_track_remove(wxCommandEvent &) {
    undoable("remove track", [&] {
        if (song_.tracks.size() == 1) {
            say("a song needs at least one track");
            return;
        }
        int i = track_at();
        Track t = song_.tracks[size_t(i)];
        if (!t.notes.empty()) {
            int answer = wxMessageBox(W("Remove " + t.name + " and the " + plural(t.notes.size(), "note") + " in it?"),
                                      kAppName, wxYES_NO | wxICON_QUESTION, this);
            if (answer != wxYES) {
                say(W("kept " + t.name));
                return;
            }
        }
        song_.tracks.erase(song_.tracks.begin() + i);
        touch();
        sync_tracks(std::min(i, int(song_.tracks.size()) - 1));
        sync(track().cursor);
        say(W("removed " + t.name + ", " + plural(song_.tracks.size(), "track") + " left"));
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

int Studio::bar_of(int index) { return bar_and_beat(position(index), signature()).first; }

//: Jump to a bar, which is how a long song is got about.
void Studio::on_goto_bar(wxCommandEvent &) {
    if (notes().empty()) {
        say("there are no notes yet");
        return;
    }
    int last_bar = bar_of(int(notes().size()) - 1);
    wxTextEntryDialog dlg(this, W("Bar number, 1 to " + std::to_string(last_bar)), "Go to bar", "1");
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
        if (bar_of(i) >= want) {
            select_only({i});
            list_->SetFocus();
            say(W("bar " + std::to_string(bar_of(i)) + ", note " + std::to_string(i + 1) + " of " +
                  std::to_string(notes().size()) + ", " + notes()[size_t(i)].label()));
            return;
        }
    }
    say(W("the song ends at bar " + std::to_string(last_bar)));
}

//: Fill the rest of the bar with silence, which is what a time signature is
//: for while a song is being written.
void Studio::on_bar_rest(wxCommandEvent &) {
    undoable("add bar rest", [&] {
        std::vector<int> rows = selected();
        int at = rows.empty() ? int(notes().size()) : rows.back() + 1;
        double span = bar_beats(signature());
        double pos = position(at);
        double left = span - std::fmod(pos, span);
        if (left < 1e-9 || std::fabs(left - span) < 1e-9) left = span;   // a whole bar
        const Note *nearby = at ? &notes()[size_t(at - 1)] : last();
        Note rest({kRest}, nearby ? nearby->pitch : kDefaultPitch, left);
        notes().insert(notes().begin() + at, rest);
        touch();
        sync(at);
        say(W("rest to the end of bar " + std::to_string(bar_and_beat(pos, signature()).first) + ": " +
              beat_count(std::nearbyint(left * 1e6) / 1e6)));
    });
}

//: Where each note starts, as "bar:beat".
std::vector<std::string> Studio::bars() {
    std::vector<std::string> out;
    double at = 0;
    for (const Note &n : notes()) {
        auto bb = bar_and_beat(at, signature());
        out.push_back(std::to_string(bb.first) + ":" + fmt_g(std::nearbyint(bb.second * 1000) / 1000));
        at += n.beats;
    }
    return out;
}

void Studio::sync(int select) {
    list_->DeleteAllItems();
    for (size_t i = 0; i < notes().size(); ++i) {
        const Note &n = notes()[i];
        list_->InsertItem(long(i), W(n.label()));
        list_->SetItem(long(i), 1, W(pitch_name(n.pitch)));
        list_->SetItem(long(i), 2, W(length_text(n.beats)));
        list_->SetItem(long(i), 3, W(n.word));
        list_->SetItem(long(i), 4, W(describe_bend(n.bend)));
        list_->SetItem(long(i), 5, W(describe_mod(n.mod)));
    }
    auto where = bars();
    for (size_t i = 0; i < where.size(); ++i) list_->SetItem(long(i), 6, W(where[i]));
    if (!notes().empty()) select_only({std::min(std::max(select, 0), int(notes().size()) - 1)});
}

void Studio::on_key(wxKeyEvent &evt) {
    int code = evt.GetKeyCode();
    wxCommandEvent e;
    if (evt.AltDown() && (code == WXK_UP || code == WXK_DOWN || code == WXK_LEFT || code == WXK_RIGHT)) {
        if (code == WXK_UP) nudge_pitch(kSemitone);
        else if (code == WXK_DOWN) nudge_pitch(-kSemitone);
        else nudge_length(code == WXK_RIGHT);
    } else if (evt.ControlDown() && (code == WXK_UP || code == WXK_DOWN)) {
        move_notes(code == WXK_UP ? -1 : 1);
    } else if (code == WXK_SPACE && !(evt.ControlDown() || evt.AltDown() || evt.ShiftDown())) {
        on_play_stop(e);
    } else if (evt.ControlDown() && code == 'A') {
        on_select_all(e);
    } else if (evt.ControlDown() && code == 'C') {
        on_copy(e);
    } else if (evt.ControlDown() && code == 'X') {
        on_cut(e);
    } else if (evt.ControlDown() && code == 'V') {
        on_paste(e);
    } else if (code == WXK_DELETE || code == WXK_NUMPAD_DELETE) {
        on_remove(e);
    } else if (code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
        on_edit(e);
    } else {
        evt.Skip();
    }
}

//: Update one row in place, so the selection and focus do not move.
void Studio::refresh_row(int i) {
    const Note &n = notes()[size_t(i)];
    list_->SetItem(i, 0, W(n.label()));
    list_->SetItem(i, 1, W(pitch_name(n.pitch)));
    list_->SetItem(i, 2, W(length_text(n.beats)));
    list_->SetItem(i, 3, W(n.word));
    list_->SetItem(i, 4, W(describe_bend(n.bend)));
    list_->SetItem(i, 5, W(describe_mod(n.mod)));
    // a length change moves every note after this one to a different beat
    auto where = bars();
    for (size_t k = 0; k < where.size(); ++k) list_->SetItem(long(k), 6, W(where[k]));
}

void Studio::nudge_pitch(int by) {
    undoable("transpose notes", [&] {
        std::vector<int> rows = selected();
        if (rows.empty()) {
            say("select a note first");
            return;
        }
        for (int i : rows) {
            Note &n = notes()[size_t(i)];
            n.pitch = std::max(0, std::min(127, n.pitch + by));
            refresh_row(i);
        }
        touch();
        std::string message = rows.size() == 1 ? pitch_name(notes()[size_t(rows[0])].pitch)
                                               : std::to_string(rows.size()) + " notes " +
                                                     (by > 0 ? "up" : "down") + " a semitone";
        announce_note(W(message), list_, rows[0]);
        preview_note(rows[0]);
    });
}

//: A sixteenth note longer or shorter.
void Studio::nudge_length(bool up) {
    undoable("change note length", [&] {
        std::vector<int> rows = selected();
        if (rows.empty()) {
            say("select a note first");
            return;
        }
        int moved = 0;
        for (int i : rows) {
            Note &n = notes()[size_t(i)];
            auto want = stepped_length(n.beats, up);
            if (!want) continue;
            n.beats = *want;
            ++moved;
        }
        if (!moved) {
            announce_note(W(spoken_length(notes()[size_t(rows[0])].beats, signature()) +
                            ", the shortest a nudge will make it. Edit the note to go shorter."),
                          list_, rows[0]);
            return;
        }
        touch();
        sync_lengths();
        preview_note(rows[0]);
        std::string message = rows.size() == 1 ? spoken_length(notes()[size_t(rows[0])].beats, signature())
                                               : std::to_string(moved) + " notes a sixteenth " +
                                                     (up ? "longer" : "shorter");
        announce_note(W(message), list_, rows[0]);
    });
}

void Studio::sync_lengths() {
    for (size_t i = 0; i < notes().size(); ++i) refresh_row(int(i));
}

const Note *Studio::last() { return notes().empty() ? nullptr : &notes().back(); }

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

void Studio::on_add_word(wxCommandEvent &) {
    undoable("add word", [&] {
        const Note *l = last();
        AddWordDialog dlg(this, this, l ? l->pitch : kDefaultPitch, l ? l->beats : kDefaultBeats);
        if (dlg.ShowModal() != wxID_OK) return;
        std::vector<Note> added = dlg.result();
        if (added.empty()) return;
        int at = int(notes().size());
        notes().insert(notes().end(), added.begin(), added.end());
        touch();
        sync(at);
        std::string all;
        for (const Note &n : added) all += (all.empty() ? "" : ", ") + n.text();
        say(W("added " + (added[0].word.empty() ? std::string("the word") : added[0].word) + " over " +
              plural(added.size(), "note") + ": " + all));
    });
}

void Studio::on_add_note(wxCommandEvent &) {
    undoable("add note", [&] {
        const Note *l = last();
        Note seed({}, l ? l->pitch : kDefaultPitch, l ? l->beats : kDefaultBeats);
        NoteDialog dlg(this, this, nullptr, seed);
        if (dlg.ShowModal() != wxID_OK) return;
        notes().push_back(dlg.result());
        touch();
        sync(int(notes().size()) - 1);
        std::string text = notes().back().text();
        say(W("added a note: " + (text.empty() ? std::string("empty") : text)));
    });
}

//: A rest is a note nobody sings. It breaks the line into phrases, so the
//: phrase before it is allowed to end instead of running into the next.
void Studio::on_add_rest(wxCommandEvent &) {
    undoable("add rest", [&] {
        int i = selection();
        const Note *nearby = i >= 0 ? &notes()[size_t(i)] : last();
        Note rest({kRest}, nearby ? nearby->pitch : kDefaultPitch, nearby ? nearby->beats : kDefaultBeats);
        int at = i < 0 ? int(notes().size()) : i + 1;
        notes().insert(notes().begin() + at, rest);
        touch();
        sync(at);
        say(W("added a rest of " + beat_count(rest.beats) + ". Alt+Right and Alt+Left change it."));
    });
}

void Studio::on_edit(wxCommandEvent &) {
    undoable("edit note", [&] {
        int i = selection();
        if (i < 0) {
            say("select a note first");
            return;
        }
        NoteDialog dlg(this, this, &notes()[size_t(i)]);
        if (dlg.ShowModal() != wxID_OK) return;
        notes()[size_t(i)] = dlg.result();
        touch();
        sync(i);
        say(W("note " + std::to_string(i + 1) + " is " + notes()[size_t(i)].text() + " at " +
              pitch_name(notes()[size_t(i)].pitch)));
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
        copy.voice_id = track_voice_of(track());
        copy.notes = *only;
        sing = {&copy};
    }
    return make_request(song_, sing, start, program_map_);
}

//: The song as it is played: the metronome goes on here and nowhere else.
RenderRequest Studio::playback(double start) {
    RenderRequest r = request(nullptr, start);
    if (mi_metronome_->IsChecked()) r.metronome_bar = bar_beats(signature());
    return r;
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

void Studio::preview(const std::vector<Note> &rows, std::function<void()> done) {
    render(request(new std::vector<Note>(rows)), preview_wav_, [this, done](RenderResult res) {
        if (done) done();
        if (res.ok) play_file(res.path);
    });
}

void Studio::lookup(const std::vector<std::string> &words,
                    std::function<void(std::map<std::string, std::vector<std::string>>)> done) {
    worker_->ask<std::map<std::string, std::vector<std::string>>>(
        [words] {
            std::map<std::string, std::vector<std::string>> out;
            for (const std::string &w : words) out[w] = Lexicon::get().phonemes(w);
            return out;
        },
        done);
}

void Studio::on_metronome(wxCommandEvent &) {
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
    stop_audio();
    rendering_ = true;
    render(request(new std::vector<Note>{notes()[size_t(i)]}), preview_wav_,
           [this](RenderResult res) { heard(res); });
}

void Studio::heard(const RenderResult &res) {
    rendering_ = false;
    if (res.ok) play_file(res.path, true);
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
    for (const Track *t : audible(song_.tracks))
        if (!t->notes.empty()) live.push_back(t);
    if (live.empty()) {
        bool any = false;
        for (const Track &t : song_.tracks) any = any || !t.notes.empty();
        say(any ? "every track with notes in it is silent. M unmutes a track, S turns solo off."
                : "nothing to sing yet. Add a word or a note.");
        return;
    }
    double total = 0;
    for (const Track *t : live) {
        double s = 0;
        for (const Note &n : t->notes) s += n.beats;
        total = std::max(total, s);
    }
    if (start >= total - 1e-9) start = 0.0;      // past the end: start again
    stop_audio();          // the file cannot be written while it plays
    rendering_ = true;
    mi_play_->Enable(false);
    if (!quiet) {
        auto bb = bar_and_beat(start, signature());
        say(W(printf_s("rendering %s from bar %d beat %s, about %.1f seconds", plural(live.size(), "track").c_str(),
                       bb.first, fmt_g(std::nearbyint(bb.second * 100) / 100).c_str(),
                       (total - start) * 60.0 / song_.bpm)));
    }
    render(playback(start), wav_, [this, quiet](RenderResult res) { played(res, quiet); });
}

void Studio::played(const RenderResult &res, bool quiet) {
    rendering_ = false;
    mi_play_->Enable(true);
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
    play_file(res.path, quiet);
}

void Studio::play_file(const std::string &path, bool quiet) {
    if (!file_exists(path)) {
        say("no audio file to play");
        return;
    }
    std::vector<unsigned char> b;
    read_file(path, &b);
    if (player_.play(path)) {
        if (!quiet) say(W("playing " + base_name(path) + ", " + std::to_string(b.size() / 1024) + " KB"));
        return;
    }
    say(W("cannot play it. The file is at " + path));
}

void Studio::stop_audio() { player_.stop(); }

void Studio::on_stop(wxCommandEvent &) {
    stop_audio();
    say("stopped");
}

void Studio::on_export(wxCommandEvent &) {
    bool any = false;
    for (const Track *t : audible(song_.tracks)) any = any || !t->notes.empty();
    if (!any) {
        say("nothing to export: no track that can be heard has any notes in it");
        return;
    }
    wxFileDialog dlg(this, "Export audio", "", "", "WAV files (*.wav)|*.wav", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    say(W("rendering to " + base_name(path)));
    render(request(), path, [this, path](RenderResult res) {
        say(W(printf_s("wrote %s, %.2f seconds", base_name(path).c_str(), res.ok ? res.seconds : 0.0)));
    });
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

void Studio::on_update_history(wxUpdateUIEvent &evt) {
    bool undo = evt.GetId() == wxID_UNDO;
    const auto &entries = undo ? undo_ : redo_;
    evt.Enable(!entries.empty());
    evt.SetText(W(std::string(undo ? "Undo" : "Redo") + (entries.empty() ? "" : " " + entries.back().label) +
                  "\t" + (undo ? "Ctrl+Z" : "Ctrl+Shift+Z")));
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
    std::string name = path_.empty() ? "Untitled" : base_name(path_);
    SetTitle(W((dirty ? "*" : "") + name + " - " + kAppName));
    keep_a_copy();
}

//: The synthesisers are in this process, so the song is written out a couple
//: of seconds after it stops changing.
void Studio::keep_a_copy() { recovery_timer_.StartOnce(kRecoveryWait); }

void Studio::write_copy() {
    bool any = false;
    for (const Track &t : song_.tracks) any = any || !t.notes.empty();
    if (!any) return;
    recovery_write(song_, path_);
}

void Studio::offer_recovery() {
    auto left = recovery_waiting();
    if (!left) return;
    std::string from = left->was_path.empty() ? "" : ", from " + base_name(left->was_path);
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
    int answer = wxMessageBox("This song has changes that have not been saved. " + what + " anyway?", kAppName,
                              wxYES_NO | wxCANCEL | wxICON_QUESTION, this);
    return answer == wxYES;
}

//: Replace the song with what was loaded or imported.
void Studio::take(const Song &song, const std::string &path) {
    song_ = song;
    if (song_.tracks.empty()) {
        Track t;
        t.name = "Voice 1";
        song_.tracks.push_back(t);
    }
    song_.bpm = double(std::max(30L, std::min(250L, std::lrint(song.bpm))));
    song_.sig = parse_sig(format_sig(song.sig));
    song_.consonants = std::max(10L, std::min(100L, std::lrint(song.consonants * 100))) / 100.0;
    song_.voice = clean_voice(song.voice);
    path_ = path;
    sync_tracks(0);
    sync(0);
    reset_history(!path.empty());
    touch(path.empty());
}

void Studio::on_new(wxCommandEvent &) {
    if (!may_discard("Start a new song")) return;
    Track t;
    t.name = "Voice 1";
    t.program = track().program;
    song_.tracks = {t};
    path_.clear();
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
    Song song;
    try {
        song = load_project(path);
    } catch (const std::exception &e) {
        say(W("cannot open " + base_name(path) + ": " + e.what()));
        return;
    }
    double bpm = song.bpm;
    take(song, path);
    int n = 0;
    for (const Track &t : song_.tracks) n += int(t.notes.size());
    say(W("opened " + base_name(path) + ": " + plural(song_.tracks.size(), "track") + ", " + std::to_string(n) +
          " notes, " + fmt_g(bpm) + " bpm"));
}

void Studio::on_save(wxCommandEvent &e) {
    if (!path_.empty()) write_project(path_);
    else on_save_as(e);
}

void Studio::on_save_as(wxCommandEvent &) {
    wxFileDialog dlg(this, "Save project", "", W(std::string("song") + kProjectSuffix), kProjectWildcard,
                     wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    if (extension_lower(path) != kProjectSuffix) path += kProjectSuffix;
    write_project(path);
}

void Studio::write_project(const std::string &path) {
    try {
        save_project(path, song_);
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
    wxFileDialog dlg(this, "Import MIDI", "", "", kMidiWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;
    std::string path = U(dlg.GetPath());
    std::vector<std::pair<std::string, int>> parts;
    try {
        parts = midi_tracks(path);
    } catch (const std::exception &e) {
        say(W("cannot read " + base_name(path) + ": " + e.what()));
        return;
    }
    if (parts.empty()) {
        say(W(base_name(path) + " has no notes in it"));
        return;
    }
    std::vector<std::string> names;
    for (const auto &p : parts) names.push_back(p.first);
    if (parts.size() > 1) {
        // Every part at once is the first choice, and the default: a file
        // with several parts in it is usually several parts of one song.
        wxArrayString labels;
        labels.Add("Every part, one track each");
        for (const auto &p : parts) labels.Add(W(p.first + ", " + std::to_string(p.second) + " notes"));
        wxSingleChoiceDialog pick(this, "What should be imported?", "Import MIDI", labels);
        if (pick.ShowModal() != wxID_OK) return;
        int i = pick.GetSelection();
        if (i > 0) names = {names[size_t(i - 1)]};
    }
    Imported got;
    try {
        got = import_midi(path, names);
    } catch (const std::exception &e) {
        say(W(std::string("cannot import: ") + e.what()));
        return;
    }
    // as the original: the imported song keeps the consonant length, and
    // starts its voice controls, reverb and timing afresh
    Song song;
    song.bpm = got.bpm;
    song.sig = got.sig;
    song.consonants = song_.consonants;
    int program = track().program;
    for (const ImportedPart &p : got.parts) {
        Track t;
        t.name = p.name;
        t.program = program;
        t.notes = p.notes;
        song.tracks.push_back(t);
    }
    take(song, "");
    size_t total = 0;
    for (const auto &p : got.parts) total += p.notes.size();
    say(W("imported " + base_name(path) + ": " + plural(got.parts.size(), "track") + ", " + std::to_string(total) +
          " notes, " + fmt_g(std::nearbyint(got.bpm)) + " bpm, " + format_sig(got.sig)));
    bool any_pending = false, any_words = false;
    for (const auto &p : got.parts) {
        any_pending = any_pending || !p.pending.empty();
        for (const Note &n : p.notes) any_words = any_words || !n.word.empty();
    }
    if (!any_pending && !any_words)
        say(W(std::string("it carried no words, so every note sings ") + kDefaultPhoneme +
              ". Ctrl+W puts a word on one."));
    if (got.parts.size() > 1) {
        auto it = program_map_.find(program);
        say(W("every part is singing in " + voice_name(it == program_map_.end() ? 0 : it->second) +
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
        lookup(words, [this, parts_copy, pending](std::map<std::string, std::vector<std::string>> found) {
            imported_words(parts_copy, found, pending);
        });
    }
}

//: Fill in the pronunciations for a MIDI that carried only lyrics.
void Studio::imported_words(std::vector<ImportedPart> parts,
                            const std::map<std::string, std::vector<std::string>> &found, size_t pending) {
    undoable("pronounce imported words", [&] {
        int got = pronounce(parts, found);
        for (size_t ti = 0; ti < song_.tracks.size() && ti < parts.size(); ++ti) song_.tracks[ti].notes = parts[ti].notes;
        sync(0);
        touch();
        size_t missing = pending - size_t(got);
        say(W(plural(size_t(got), "word") + " pronounced" +
              (missing ? ", " + std::to_string(missing) + " not in the dictionary -- those notes sing " +
                             kDefaultPhoneme + " until you say otherwise"
                       : "")));
    });
}

void Studio::on_close(wxCloseEvent &evt) {
    if (evt.CanVeto() && !may_discard("Close")) {
        evt.Veto();
        return;
    }
    stop_audio();
    preview_timer_.Stop();
    recovery_timer_.Stop();
    worker_->close();
    // closing properly is what says the kept copy is not needed
    recovery_clear();
    Destroy();
}

}  // namespace svs
