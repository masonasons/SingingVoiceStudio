// sing_test.cpp -- the Microsoft engine on its own.
//
//   microsoft_sing_test [--data DIR] [--out DIR]
//
// Lists the voices, checks the phoneme table against the engine, and says whether the voice
// data is there and, if not, what the program would tell the user. With the data (found the
// way the program finds it, or in --data DIR, which may be the folder holding Sam.spd itself or
// one with voices\microsoft under it) it sings "Twinkle, twinkle" with a few of the voices to
// WAV files in --out DIR (default: the current folder), and checks that the notes start where
// the pitch curve says they do.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "audio/wav.h"
#include "core/paths.h"
#include "voices/family.h"
#include "voices/pitch.h"
#include "voices/sam_sing.h"

namespace svs {
std::unique_ptr<Family> make_microsoft_family_at(const std::string &voice_dir);
std::string microsoft_self_check();
}  // namespace svs

using namespace svs;

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

SungNote note(int midi, double beats, std::vector<std::string> ph) {
    SungNote n;
    n.midi = midi;
    n.beats = beats;
    n.velocity = 100;
    n.phonemes = std::move(ph);
    return n;
}

Phrase twinkle() {
    Phrase p;
    p.bpm = 100;
    p.notes = {
        note(60, 1, {"t", "w", "IH", "NG"}), note(60, 1, {"k", "EL"}),
        note(67, 1, {"t", "w", "IH", "NG"}), note(67, 1, {"k", "EL"}),
        note(69, 1, {"l", "IH"}),            note(69, 1, {"DD", "EL"}),
        note(67, 2, {"s", "t", "AR"}),
    };
    SungNote rest = note(67, 0.4, {"%"});
    rest.velocity = 1;
    p.notes.push_back(rest);
    // a little scoop up into "star" and back
    double star = 6 * 0.6;
    for (int k = 0; k <= 20; ++k)
        p.bends.push_back({star + k * 0.005, -1.0 + k / 20.0});
    p.level = 0.8;
    return p;
}

// The C layer directly, to see where each note really starts.
void timing_check(const std::string &dir) {
    std::string path;
    for (const char *v : {"Sam", "Mike", "Mary"})
        if (file_exists(join_path(dir, std::string(v) + ".spd"))) {
            path = join_path(dir, std::string(v) + ".spd");
            break;
        }
    std::vector<unsigned char> bytes;
    if (path.empty() || !read_file(path, &bytes)) return;
    char err[256] = {0};
    sam_voice *voice = sam_voice_load_mem(bytes.data(), bytes.size(), err, sizeof err);
    check(voice != nullptr, "load " + path + (err[0] ? std::string(": ") + err : ""));
    if (!voice) return;
    sam_tts *eng = svs_sam_open(voice);
    // "strike the bell, ring" -- heavy consonant clusters on short notes, a long vowel, a rest
    std::vector<std::vector<int>> ph = {
        {SVS_SAPI_S, SVS_SAPI_T, SVS_SAPI_R, SVS_SAPI_AY, SVS_SAPI_ST1, SVS_SAPI_K},
        {SVS_SAPI_DH, SVS_SAPI_AX, SVS_SAPI_ST1},
        {SVS_SAPI_B, SVS_SAPI_EH, SVS_SAPI_ST1, SVS_SAPI_L},
        {},
        {SVS_SAPI_R, SVS_SAPI_IH, SVS_SAPI_ST1, SVS_SAPI_NG},
        {},
    };
    std::vector<double> secs = {0.12, 0.25, 1.7, 0.3, 0.9, 0.24};
    std::vector<svs_sam_note> notes(ph.size());
    for (size_t i = 0; i < ph.size(); ++i)
        notes[i] = {ph[i].empty() ? nullptr : ph[i].data(), int(ph[i].size()), secs[i]};
    std::vector<double> starts(ph.size());
    svs_sam_sing_opts o{};
    o.consonant_scale = 1.0;
    o.vibrato_cents = 25;
    o.vibrato_hz = 5;
    o.starts = starts.data();
    std::vector<int16_t> pcm;
    auto f0 = [](void *, double t) { return 150.0 * std::pow(2.0, std::sin(t) / 12.0); };
    int rc = svs_sam_sing_phones(
        eng, notes.data(), int(notes.size()), f0, nullptr, &o,
        [](const int16_t *s, size_t n, void *u) {
            auto *v = static_cast<std::vector<int16_t> *>(u);
            v->insert(v->end(), s, s + n);
        },
        &pcm);
    check(rc == 0, "svs_sam_sing_phones returns 0");
    double at = 0.0, worst = 0.0;
    for (size_t i = 0; i < notes.size(); ++i) {
        double off = (starts[i] - at) * 1000.0;
        std::printf("      note %zu due %.4f s, started %.4f s (%+.2f ms)\n", i, at, starts[i], off);
        worst = std::max(worst, std::fabs(off));
        at += secs[i];
    }
    check(starts[0] == 0.0, "the first note starts at sample 0");
    check(worst < 15.0, "every note starts within 15 ms of its time (worst " +
                            std::to_string(worst) + " ms)");
    double len = pcm.size() / 22050.0;
    check(std::fabs(len - at) < 0.015, "the phrase lasts " + std::to_string(len) + " s for " +
                                           std::to_string(at) + " s of notes");
    svs_sam_close(eng);
    sam_voice_free(voice);
}

}  // namespace

int main(int argc, char **argv) {
    std::string data, out = ".";
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--data") && i + 1 < argc) data = argv[++i];
        else if (!std::strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
        else {
            std::fprintf(stderr, "usage: microsoft_sing_test [--data DIR] [--out DIR]\n");
            return 2;
        }
    }
    std::string dir;
    if (!data.empty()) {
        std::string sub = join_path(data, "voices\\microsoft");
        dir = dir_exists(sub) ? sub : data;
    }
    std::unique_ptr<Family> fam = data.empty() ? make_microsoft_family() : make_microsoft_family_at(dir);
    if (dir.empty()) dir = find_data("voices\\microsoft");

    std::printf("%s voices:\n", fam->name().c_str());
    auto voices = fam->voices();
    std::set<int> ids;
    for (const VoiceEntry &v : voices) {
        std::printf("  %d  %s\n", v.id, v.name.c_str());
        ids.insert(v.id);
    }
    check(voices.size() == 33 && ids.size() == 33, "33 voices, every number different");
    check(voices.front().id == kMicrosoftBase && voices.front().name == "Microsoft Sam",
          "3000 is Microsoft Sam");
    bool named = false;
    for (const VoiceEntry &v : voices)
        if (v.id == 3000 + 2 * 20 + 1 && v.name == "Microsoft Mary in Hall") named = true;
    check(named, "3041 is Microsoft Mary in Hall");

    std::string bad = microsoft_self_check();
    check(bad.empty(), "phoneme table: all 57 VocalWriter phonemes map to SAPI phones the engine has" +
                           (bad.empty() ? std::string() : " -- " + bad));

    std::string why, err;
    bool refused = !fam->singer(4242, &err);
    check(refused && !err.empty(), "an unknown voice number is refused: " + err);
    bool avail = fam->available(&why);
    std::printf("available: %s\n", avail ? "yes" : "no");
    if (!avail) {
        std::printf("why: %s\n", why.c_str());
        check(why.find("Sam.spd") != std::string::npos && why.find("voices\\microsoft") != std::string::npos,
              "the reason names the files and where they go");
        err.clear();
        refused = !fam->singer(3000, &err);
        check(refused && !err.empty(), "Sam cannot be had: " + err);
        std::printf("%s\n", failures ? "FAILED" : "all checks passed (no voice data, nothing sung)");
        return failures ? 1 : 0;
    }

    timing_check(dir);
    make_dirs(out);
    Phrase p = twinkle();
    PitchCurve curve(p);
    const int picks[] = {3000, 3021, 3041, 3044, 3007, 3005};
    for (int id : picks) {
        std::string name;
        for (const VoiceEntry &v : voices)
            if (v.id == id) name = v.name;
        err.clear();
        auto s = fam->singer(id, &err);
        if (!s) {
            std::printf("skip  %s: %s\n", name.c_str(), err.c_str());
            continue;
        }
        Sung sung = s->sing(p);
        // a second time: a phrase sings the same whatever was sung before it
        check(s->sing(p).samples == sung.samples, name + " sings the same twice");
        float peak = 0;
        for (float v : sung.samples) peak = std::max(peak, std::fabs(v));
        double secs = sung.samples.size() / double(kSampleRate);
        std::string file = join_path(out, "microsoft_" + std::to_string(id) + ".wav");
        write_wav(file, sung.samples, 1, kSampleRate);
        check(!sung.stopped_short && secs >= curve.total() - 0.02 && peak <= 1.0f,
              name + ": " + std::to_string(secs) + " s for " + std::to_string(curve.total()) +
                  " s of notes, peak " + std::to_string(peak) + " -> " + file);
    }
    std::printf("%s\n", failures ? "FAILED" : "all checks passed");
    return failures ? 1 : 0;
}
