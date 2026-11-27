// update.h -- whether a newer Singing Voice Studio has been released.
//
// The releases are GitHub's (.github/workflows/release.yml cuts one for each
// tag), so GitHub is asked for the newest, and its version, the tag without
// its "v", is set against the one this program was built as, SVS_VERSION in
// CMakeLists.txt. Nothing is downloaded or installed here: a newer release is
// offered, and taking it opens its file in the browser.
//
// The question is asked in the background, through wxWebRequest (WinHTTP on
// Windows, NSURLSession on a Mac), and answered on the window's thread.
#pragma once

#include <functional>
#include <string>

namespace svs {

//: the version this program was built as: "1.8"
const char *program_version();

struct Release {
    std::string version;    // "1.9", the tag without its "v"
    std::string notes;      // what is new, in the release's own words
    std::string page;       // the release on GitHub
    std::string download;   // this system's file, or empty if it has none
};

//: What GitHub said: the newest release, or why it could not be had.
struct UpdateAnswer {
    bool ok = false;
    std::string error;
    Release latest;
    //: the release comes after this program
    bool newer = false;
};

//: Whether version `a` comes after `b`, compared number by number: 1.10
//: after 1.9, 1.8.1 after 1.8, and 1.8.0 the same as 1.8. A leading "v" is
//: passed over, as is anything after a part's digits. A version with no
//: number in it comes after nothing.
bool later_version(const std::string &a, const std::string &b);

//: A release from GitHub's JSON for one (what /releases/latest answers),
//: with the file for this system among its assets; false, with why, if the
//: JSON is not one.
bool parse_release(const std::string &json, Release *out, std::string *error);

//: One question to GitHub, answered once through `done` on the window's
//: thread, never before start() has returned. The question deletes itself
//: once answered; a window that goes first calls abandon(), after which
//: `done` is never called.
class UpdateCheck {
public:
    using Done = std::function<void(const UpdateAnswer &)>;
    static UpdateCheck *start(Done done);
    virtual void abandon() = 0;

protected:
    ~UpdateCheck() = default;
};

}  // namespace svs
