#include "app/update.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include <json.hpp>
#include <wx/app.h>
#include <wx/defs.h>
#if wxUSE_WEBREQUEST
#include <wx/stream.h>
#include <wx/webrequest.h>
#endif

// both from CMakeLists.txt, which sets them on this file alone
#ifndef SVS_VERSION
#define SVS_VERSION "0"
#endif
#ifndef SVS_UPDATE_REPO
#define SVS_UPDATE_REPO "masonasons/SingingVoiceStudio"
#endif

namespace svs {

const char *program_version() { return SVS_VERSION; }

namespace {

using json = nlohmann::json;

const char *const kLatest = "https://api.github.com/repos/" SVS_UPDATE_REPO "/releases/latest";
const char *const kReleases = "https://github.com/" SVS_UPDATE_REPO "/releases/latest";

//: "v1.10.2" as 1, 10, 2: each part's leading digits, up to the first part
//: that has none
std::vector<long> version_numbers(const std::string &v) {
    std::vector<long> out;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        long n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            // nine digits and no more, which a long holds everywhere
            if (n < 100000000L) n = n * 10 + (v[i] - '0');
            ++i;
        }
        out.push_back(n);
        // the next part, if a dot leads to one
        while (i < v.size() && v[i] != '.') ++i;
        if (i < v.size()) ++i;
    }
    return out;
}

//: The file the release workflow builds for the system this is running on
//: (.github/workflows/release.yml): the zip on Windows, the disk image for
//: this processor on a Mac.
bool for_this_system(const std::string &file, const std::string &version) {
#if defined(_WIN32)
    return file == "SingingVoiceStudio-" + version + ".zip";
#elif defined(__APPLE__)
#if defined(__aarch64__) || defined(__arm64__)
    return file == "SingingVoiceStudio-" + version + "-macos-arm64.dmg";
#else
    return file == "SingingVoiceStudio-" + version + "-macos-x86_64.dmg";
#endif
#else
    (void)file;
    (void)version;
    return false;
#endif
}

std::string text_of(const json &doc, const char *key) {
    auto it = doc.find(key);
    return it != doc.end() && it->is_string() ? it->get<std::string>() : std::string();
}

}  // namespace

bool later_version(const std::string &a, const std::string &b) {
    std::vector<long> x = version_numbers(a), y = version_numbers(b);
    if (x.empty()) return false;
    size_t n = std::max(x.size(), y.size());
    x.resize(n, 0);
    y.resize(n, 0);
    return x > y;
}

bool parse_release(const std::string &text, Release *out, std::string *error) {
    json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        *error = "GitHub's answer was not a release";
        return false;
    }
    std::string tag = text_of(doc, "tag_name");
    if (version_numbers(tag).empty()) {
        *error = tag.empty() ? "the newest release has no tag" : "the newest release's tag, " + tag + ", is not a version";
        return false;
    }
    Release r;
    r.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    // the release's notes keep the line endings they were typed with
    std::string notes = text_of(doc, "body");
    notes.erase(std::remove(notes.begin(), notes.end(), '\r'), notes.end());
    while (!notes.empty() && (notes.back() == '\n' || notes.back() == ' ')) notes.pop_back();
    r.notes = notes;
    r.page = text_of(doc, "html_url");
    if (r.page.empty()) r.page = kReleases;
    auto assets = doc.find("assets");
    if (assets != doc.end() && assets->is_array())
        for (const json &a : *assets) {
            if (!a.is_object()) continue;
            std::string file = text_of(a, "name"), url = text_of(a, "browser_download_url");
            if (!url.empty() && for_this_system(file, r.version)) r.download = url;
        }
    *out = r;
    return true;
}

namespace {

//: What the answer is made of once GitHub has sent it.
UpdateAnswer answer_from(const std::string &body) {
    UpdateAnswer a;
    a.ok = parse_release(body, &a.latest, &a.error);
    a.newer = a.ok && later_version(a.latest.version, program_version());
    return a;
}

#if wxUSE_WEBREQUEST
class Checker : public wxEvtHandler, public UpdateCheck {
public:
    explicit Checker(Done done) : done_(std::move(done)) {}

    bool begin() {
        wxWebSession &session = wxWebSession::GetDefault();
        if (!session.IsOpened()) return false;
        request_ = session.CreateRequest(this, kLatest);
        if (!request_.IsOk()) return false;
        // GitHub refuses a request that does not say what sent it
        request_.SetHeader("User-Agent", "SingingVoiceStudio/" SVS_VERSION);
        request_.SetHeader("Accept", "application/vnd.github+json");
        Bind(wxEVT_WEBREQUEST_STATE, &Checker::on_state, this);
        request_.Start();
        return true;
    }

    void abandon() override {
        done_ = nullptr;
        // its state changes once more, to cancelled, and it goes then
        if (request_.IsOk()) request_.Cancel();
    }

private:
    void on_state(wxWebRequestEvent &evt) {
        UpdateAnswer a;
        switch (evt.GetState()) {
            case wxWebRequest::State_Completed:
                a = answer_from(body_of(evt.GetResponse()));
                break;
            case wxWebRequest::State_Failed:
                a.error = failure(evt);
                break;
            case wxWebRequest::State_Cancelled:
                a.error = "the check was cancelled";
                break;
            default:
                return;   // still on its way
        }
        finish(a);
    }

    static std::string body_of(const wxWebResponse &res) {
        std::string body;
        wxInputStream *in = res.IsOk() ? res.GetStream() : nullptr;
        if (!in) return body;
        char buf[4096];
        for (;;) {
            in->Read(buf, sizeof buf);
            size_t n = in->LastRead();
            if (n == 0) break;
            body.append(buf, n);
        }
        return body;
    }

    //: why GitHub could not be asked, as plainly as can be said
    static std::string failure(const wxWebRequestEvent &evt) {
        const wxWebResponse &res = evt.GetResponse();
        int status = res.IsOk() ? res.GetStatus() : 0;
        if (status == 404) return "GitHub has no release of Singing Voice Studio to offer";
        // an hour's allowance of questions from this address is used up
        if (status == 403 || status == 429) return "GitHub is not answering this computer for now; try again later";
        if (status != 0) return "GitHub answered " + std::to_string(status);
        std::string why = evt.GetErrorDescription().utf8_string();
        return why.empty() ? std::string("GitHub could not be reached") : "GitHub could not be reached: " + why;
    }

    void finish(const UpdateAnswer &a) {
        Done done;
        done.swap(done_);
        request_ = wxWebRequest();
        // not while its own event is being handled
        wxTheApp->ScheduleForDestruction(this);
        if (done) done(a);
    }

    Done done_;
    wxWebRequest request_;
};
#endif

}  // namespace

UpdateCheck *UpdateCheck::start(Done done) {
#if wxUSE_WEBREQUEST
    Checker *c = new Checker(done);
    if (c->begin()) return c;
    delete c;
    const char *why = "the system offers no way to reach GitHub";
#else
    const char *why = "this build has no way to reach GitHub";
#endif
    UpdateAnswer a;
    a.error = why;
    // answered later, as an answer from GitHub would be
    if (done) wxTheApp->CallAfter([done, a] { done(a); });
    return nullptr;
}

}  // namespace svs
