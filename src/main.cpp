// Singing Voice Studio: with no arguments, or a song to open, the editor;
// with anything else on the command line, the command line program.
//
// Both executables run this file. On Windows SingingVoiceStudio.exe is
// windowed and svs.exe is a console program, so that a script can wait for a
// render and read what it said; Windows decides that when a program is built
// rather than when it is run, so one executable cannot be both. On macOS a
// program prints to the terminal that started it whether it has a window or
// not, so the one inside the bundle does both, and svs beside it is the same
// program under a shorter name.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <memory>

#include <wx/wx.h>
#include <wx/snglinst.h>

#include "app/cli.h"
#include "app/crash.h"
#include "app/studio.h"
#include "app/widgets.h"
#include "core/paths.h"

#ifdef __WXMSW__
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> arguments(int argc, char **argv) {
    std::vector<std::string> out;
#ifdef __WXMSW__
    (void)argc;
    (void)argv;
    int n = 0;
    wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) out.push_back(svs::narrow(wide[i]));
    LocalFree(wide);
#else
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        // what the Finder passes to an application it launches
        if (a.rfind("-psn_", 0) == 0) continue;
        out.push_back(a);
    }
#endif
    return out;
}

//: A windowed program starts with no console. Borrowing the console of
//: whatever started it is what makes "SingingVoiceStudio.exe --help" answer;
//: svs.exe, built with one of its own, is the one to use in a script.
bool attach_console() {
#if defined(__WXMSW__) && !defined(SVS_CONSOLE)
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return false;
    std::freopen("CONOUT$", "w", stdout);
    std::freopen("CONOUT$", "w", stderr);
#endif
    return true;
}

//: One copy at a time: a second would take the song the first keeps for
//: recovery as left by a crash. It says so and gives way; on Windows a file
//: it was started for is handed to the first, which asks what to do with it.
const char *const kRunning = "Singing Voice Studio is already running.";
std::string running_text(const std::string &file) {
    return file.empty() ? kRunning : std::string(kRunning) + "\n\n" + file +
                                         " was not opened. Open it from the window that is running.";
}

#ifdef __WXMSW__
//: The window of the copy already running, by its title's ending.
BOOL CALLBACK find_studio(HWND hwnd, LPARAM found) {
    wchar_t title[512];
    int n = GetWindowTextW(hwnd, title, 512);
    const std::wstring tail = L" - Singing Voice Studio";
    if (n >= int(tail.size()) && std::wstring(title + n - tail.size()) == tail && IsWindowVisible(hwnd)) {
        *reinterpret_cast<HWND *>(found) = hwnd;
        return FALSE;
    }
    return TRUE;
}

//: The file, named in full, sent to the copy running, which takes it once
//: this has returned (Studio::open_from_outside). False if it would not.
bool hand_over(HWND other, const std::string &file) {
    std::wstring wide = svs::widen(file);
    wchar_t full[32768];
    DWORD n = GetFullPathNameW(wide.c_str(), 32768, full, nullptr);
    std::string path = n > 0 && n < 32768 ? svs::narrow(full) : file;
    COPYDATASTRUCT data;
    data.dwData = svs::kHandedFile;
    data.cbData = DWORD(path.size());
    data.lpData = const_cast<char *>(path.data());
    DWORD_PTR answer = 0;
    return SendMessageTimeoutW(other, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 5000,
                               &answer) &&
           answer == TRUE;
}

//: Asked before wx starts, so a second copy shows nothing but the message:
//: a mutex for this session, held until the copy that made it ends. Windows'
//: own message box sounds the asterisk with it. A file goes to the copy
//: running instead, which asks whether to give up its song for it.
bool gave_way(const std::string &file) {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\SingingVoiceStudio");
    if (!mutex || GetLastError() != ERROR_ALREADY_EXISTS) return false;
    HWND other = nullptr;
    EnumWindows(find_studio, reinterpret_cast<LPARAM>(&other));
    auto forward = [other] {
        if (IsIconic(other)) ShowWindow(other, SW_RESTORE);
        // so the question it asks comes up in front
        DWORD pid = 0;
        GetWindowThreadProcessId(other, &pid);
        AllowSetForegroundWindow(pid);
        SetForegroundWindow(other);
    };
    if (other && !file.empty()) {
        forward();
        if (hand_over(other, file)) return true;
    }
    MessageBoxW(nullptr, svs::widen(running_text(file)).c_str(), L"Singing Voice Studio",
                MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
    if (other) forward();
    return true;
}
#endif

class App : public wxApp {
public:
    explicit App(std::string file) : file_(std::move(file)) {}
    bool OnInit() override {
#ifndef __WXMSW__
        running_ = std::make_unique<wxSingleInstanceChecker>();
        if (running_->Create("SingingVoiceStudio-" + wxGetUserId()) && running_->IsAnotherRunning()) {
            wxMessageBox(wxString::FromUTF8(running_text(file_).c_str()), "Singing Voice Studio",
                         wxOK | wxICON_INFORMATION);
            return false;
        }
#endif
        frame_ = new svs::Studio(file_);
        frame_->Show();
        return true;
    }
#ifdef __WXOSX__
    //: A song double-clicked in the Finder, or dropped on the Dock icon:
    //: macOS says so with an event rather than on the command line.
    void MacOpenFiles(const wxArrayString &files) override {
        if (frame_ && !files.empty()) frame_->open_from_outside(svs::U(files[0]));
    }
    void MacNewFile() override {}
#endif

private:
    std::string file_;
    svs::Studio *frame_ = nullptr;
    std::unique_ptr<wxSingleInstanceChecker> running_;
};

}  // namespace

int main(int argc, char **argv) {
    svs::install_crash_reporter();
    // a deliberate crash, to check that a report is written
    if (const char *t = std::getenv("SVS_CRASH_TEST"); t && *t == '1') {
        volatile int *nothing = nullptr;
        *nothing = 1;
    }
    svs::CliArgs args = svs::parse_args(arguments(argc, argv));
    if (svs::wants_console(args)) {
        bool heard = attach_console();
        if (!heard && !args.error.empty()) {
            // nowhere to print: what would have been said goes in a dialog
            wxApp::SetInstance(new wxApp());
            int none = 0;
            wxEntryStart(none, (wxChar **)nullptr);
            wxMessageBox(wxString::FromUTF8(args.error.c_str()), "Singing Voice Studio",
                         wxOK | wxICON_INFORMATION);
            wxEntryCleanup();
            return 2;
        }
        return svs::run_cli(args);
    }
#ifdef __WXMSW__
    if (gave_way(args.file)) return 0;
#endif
    wxApp::SetInstance(new App(args.file));
#ifdef __WXMSW__
    int none = 0;
    char *empty[] = {nullptr};
    return wxEntry(none, empty);
#else
    return wxEntry(argc, argv);
#endif
}
