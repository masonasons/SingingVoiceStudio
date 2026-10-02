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

#include <wx/wx.h>

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

class App : public wxApp {
public:
    explicit App(std::string file) : file_(std::move(file)) {}
    bool OnInit() override {
        frame_ = new svs::Studio(file_);
        frame_->Show();
        return true;
    }
#ifdef __WXOSX__
    //: A song double-clicked in the Finder, or dropped on the Dock icon:
    //: macOS says so with an event rather than on the command line.
    void MacOpenFiles(const wxArrayString &files) override {
        if (frame_ && !files.empty()) frame_->open_project(svs::U(files[0]));
    }
    void MacNewFile() override {}
#endif

private:
    std::string file_;
    svs::Studio *frame_ = nullptr;
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
    wxApp::SetInstance(new App(args.file));
#ifdef __WXMSW__
    int none = 0;
    char *empty[] = {nullptr};
    return wxEntry(none, empty);
#else
    return wxEntry(argc, argv);
#endif
}
