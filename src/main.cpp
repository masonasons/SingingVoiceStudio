// Singing Voice Studio: with no arguments, or a song to open, the editor;
// with anything else on the command line, the command line program.
//
// Both executables run this file. SingingVoiceStudio.exe is windowed and
// svs.exe is a console program, so that a script can wait for a render and
// read what it said. Windows decides that when a program is built rather than
// when it is run, so one executable cannot be both.
#include <cstdio>
#include <string>
#include <vector>

#include <wx/wx.h>

#include "app/cli.h"
#include "app/studio.h"
#include "core/paths.h"

#ifdef __WXMSW__
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

std::vector<std::string> arguments() {
    std::vector<std::string> out;
#ifdef __WXMSW__
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) out.push_back(svs::narrow(argv[i]));
    LocalFree(argv);
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
        svs::Studio *frame = new svs::Studio(file_);
        frame->Show();
        return true;
    }

private:
    std::string file_;
};

}  // namespace

int main(int, char **) {
    svs::CliArgs args = svs::parse_args(arguments());
    if (svs::wants_console(args)) {
        bool heard = attach_console();
        if (!heard && !args.error.empty()) {
            // nowhere to print: what would have been said goes in a dialog
            wxApp::SetInstance(new wxApp());
            int argc = 0;
            wxEntryStart(argc, (wxChar **)nullptr);
            wxMessageBox(wxString::FromUTF8(args.error.c_str()), "Singing Voice Studio",
                         wxOK | wxICON_INFORMATION);
            wxEntryCleanup();
            return 2;
        }
        return svs::run_cli(args);
    }
    wxApp::SetInstance(new App(args.file));
    int argc = 0;
    char *argv[] = {nullptr};
    return wxEntry(argc, argv);
}
