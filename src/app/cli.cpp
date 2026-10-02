#include "app/cli.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

#include <wx/version.h>

#include "audio/mixer.h"
#include "core/paths.h"
#include "core/project.h"
#include "voices/registry.h"
#include "voices/vocalwriter.h"

namespace svs {

namespace {

const char *const kName = "svs";
bool g_quiet = false;

//: progress on the error stream, answers on the output stream
void say(const std::string &text) {
    if (!g_quiet) std::fprintf(stderr, "%s\n", text.c_str());
}
void out(const std::string &text) { std::printf("%s\n", text.c_str()); }
int fail(const std::string &message) {
    std::fprintf(stderr, "%s: %s\n", kName, message.c_str());
    return 1;
}

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

bool all_digits(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
}

std::string s_if(size_t n) { return n == 1 ? "" : "s"; }

//: Which voice that is: a number, or a name -- in full first, then as the
//: beginning of one, so "--voice trum" is Trumpet and "--voice Robert" is
//: Robert rather than an ambiguity with Robert 2.
int find_voice(const std::string &name, const std::vector<VoiceListing> &voices) {
    std::string text = name;
    text.erase(0, text.find_first_not_of(" \t"));
    text.erase(text.find_last_not_of(" \t") + 1);
    if (all_digits(text)) {
        int id = std::stoi(text);
        for (const auto &v : voices)
            if (v.id == id) return id;
        throw std::runtime_error("there is no voice numbered " + text + "; --list-voices says what there is");
    }
    for (const auto &v : voices)
        if (lower(v.name) == lower(text)) return v.id;
    std::vector<const VoiceListing *> near;
    for (const auto &v : voices)
        if (lower(v.name).compare(0, text.size(), lower(text)) == 0) near.push_back(&v);
    if (near.size() == 1) return near[0]->id;
    if (!near.empty()) {
        std::string list;
        for (const auto *v : near) list += (list.empty() ? "" : ", ") + v->name;
        throw std::runtime_error("'" + name + "' could be " + list);
    }
    throw std::runtime_error("no voice called '" + name + "'; --list-voices says what there is");
}

std::vector<const Track *> chosen_tracks(const std::vector<Track> &tracks, const std::vector<std::string> &wanted) {
    std::vector<const Track *> all;
    for (const Track &t : tracks) all.push_back(&t);
    if (wanted.empty()) return all;
    std::vector<const Track *> keep;
    std::vector<std::string> unknown;
    for (const std::string &want : wanted) {
        if (all_digits(want) && std::stoi(want) >= 1 && std::stoi(want) <= int(tracks.size())) {
            keep.push_back(&tracks[size_t(std::stoi(want) - 1)]);
            continue;
        }
        std::vector<const Track *> match;
        for (const Track &t : tracks)
            if (lower(t.name) == lower(want)) match.push_back(&t);
        if (match.empty())
            for (const Track &t : tracks)
                if (lower(t.name).compare(0, want.size(), lower(want)) == 0) match.push_back(&t);
        if (match.empty()) unknown.push_back(want);
        keep.insert(keep.end(), match.begin(), match.end());
    }
    if (!unknown.empty()) {
        std::string list;
        for (const auto &u : unknown) list += (list.empty() ? "'" : ", '") + u + "'";
        throw std::runtime_error("no track called " + list);
    }
    std::vector<const Track *> out;
    for (const Track *t : all)
        if (std::find(keep.begin(), keep.end(), t) != keep.end()) out.push_back(t);
    return out;
}

Reverb parse_reverb(const std::string &text) {
    std::string t = text;
    std::replace(t.begin(), t.end(), ',', ' ');
    double a, b;
    char extra;
    if (std::sscanf(t.c_str(), "%lf %lf %c", &a, &b, &extra) != 2)
        throw std::runtime_error("reverb wants a room size and an amount, as 40,24");
    int room = int(std::lrint(a)), wet = int(std::lrint(b));
    if (room < 0 || room > 100 || wet < 0 || wet > 100) throw std::runtime_error("reverb runs from 0 to 100");
    return {room, wet};
}

bool is_midi(const std::string &path) {
    std::string e = extension_lower(path);
    return e == ".mid" || e == ".midi";
}

//: A project or a MIDI file, the MIDI imported exactly as the window does it.
Song open_song(const std::string &path) {
    if (!is_midi(path)) return load_project(path);
    std::vector<std::string> names;
    for (const auto &p : midi_tracks(path)) names.push_back(p.first);
    if (names.empty()) throw std::runtime_error(base_name(path) + " has no notes in it");
    Imported got = import_midi(path, names);
    std::vector<std::string> words;
    size_t pending = 0;
    for (const auto &p : got.parts)
        for (const auto &pw : p.pending) {
            ++pending;
            if (std::find(words.begin(), words.end(), pw.first) == words.end()) words.push_back(pw.first);
        }
    if (pending) {
        say("looking up " + std::to_string(words.size()) + " word" + s_if(words.size()));
        std::map<std::string, std::vector<std::string>> found;
        for (const auto &w : words) found[w] = Lexicon::get().phonemes(w);
        int done = pronounce(got.parts, found);
        size_t shortfall = pending - size_t(done);
        if (shortfall)
            say(std::to_string(shortfall) + " word" + s_if(shortfall) + " not in the dictionary; those notes sing " +
                kDefaultPhoneme);
    }
    Song song;
    song.bpm = got.bpm;
    song.sig = got.sig;
    size_t total = 0;
    for (const auto &p : got.parts) {
        Track t;
        t.name = p.name;
        t.notes = p.notes;
        total += p.notes.size();
        song.tracks.push_back(t);
    }
    say("imported " + std::to_string(song.tracks.size()) + " track" + s_if(song.tracks.size()) + ", " +
        std::to_string(total) + " notes, " + fmt_g(std::nearbyint(got.bpm)) + " bpm");
    return song;
}

std::map<int, int> program_map() {
    std::map<int, int> m;
    auto picks = vocalwriter_program_voices();
    for (size_t p = 0; p < picks.size(); ++p)
        if (picks[p] >= 0) m[int(p)] = picks[p];
    return m;
}

bool render_to(Mixer &mixer, const RenderRequest &req, const std::string &path) {
    RenderResult res = mixer.render(req, path);
    if (!res.ok) {
        say("nothing was written to " + path + (res.error.empty() ? "" : ": " + res.error));
        return false;
    }
    char buf[256];
    std::snprintf(buf, sizeof buf, "wrote %s, %.2f seconds, peak %.2f%s", res.path.c_str(), res.seconds, res.peak,
                  res.cached ? " (from the cache)" : "");
    say(buf);
    if (res.peak >= 1.0)
        say("it is at full scale and may be clipping: turn a track down, or turn the reverb down");
    if (res.stopped_short) say("the song was cut short: a phrase with no rest in it can only run so long");
    return true;
}

}  // namespace

std::string usage() {
    return "usage: svs [FILE] [-o FILE] [--tracks FOLDER] [--save FILE] [--track NAME]...\n"
           "           [--tempo BPM] [--voice NAME] [--consonants PERCENT] [--reverb ROOM,WET]\n"
           "           [--anticipate | --no-anticipate] [--from BEATS]\n"
           "           [--list-voices] [--list-tracks] [--pronounce WORD...] [--version] [-q]\n"
           "\n"
           "Sing a Singing Voice Studio project or a MIDI file.\n"
           "\n"
           "what to write:\n"
           "  -o, --output FILE     render the whole song to this WAV file\n"
           "  --tracks FOLDER       render every track to its own WAV in this folder\n"
           "  --save FILE           write the song as a project (.vws)\n"
           "\n"
           "how to sing it (each overrides what the file says):\n"
           "  --track NAME          sing only this track, by name or number; may be repeated\n"
           "  --tempo BPM\n"
           "  --voice NAME          sing every track with this voice, by name or number\n"
           "  --consonants PERCENT  consonant length, 10 to 100\n"
           "  --reverb ROOM,WET     room size and amount, each 0 to 100, as in 40,24\n"
           "  --anticipate          sing a note's consonants before its beat\n"
           "  --no-anticipate       sing every note squarely on its beat\n"
           "  --from BEATS          start this many beats in\n"
           "\n"
           "what it can tell you:\n"
           "  --list-voices         every voice of every engine, numbered\n"
           "  --list-tracks         the tracks of FILE, numbered\n"
           "  --pronounce WORD...   what VocalWriter's dictionary says a word is\n"
           "  --version             which build this is, and which engines\n"
           "  -q, --quiet           say nothing but what was asked for\n"
           "\n"
           "examples:\n"
           "  svs song.vws -o song.wav           render a song\n"
           "  svs tune.mid -o tune.wav           import a MIDI file and render it\n"
           "  svs song.vws --tracks stems        one WAV per track, into stems\n"
           "  svs tune.mid --save tune.vws       import, save, open it later\n"
           "  svs song.vws -o q.wav --voice \"DECtalk Betty\"\n"
           "\n"
           "With a file and none of --output, --tracks or --save, the editor opens with\n"
           "that song in it. With no arguments at all, the editor opens empty.\n";
}

CliArgs parse_args(const std::vector<std::string> &argv) {
    CliArgs a;
    auto need = [&](size_t &i, const std::string &opt) -> std::string {
        if (i + 1 >= argv.size()) {
            a.error = "argument " + opt + ": expected one argument";
            return "";
        }
        return argv[++i];
    };
    auto number = [&](const std::string &opt, const std::string &v, double *outv) {
        try {
            size_t used = 0;
            *outv = std::stod(v, &used);
            if (used != v.size()) throw 1;
        } catch (...) {
            a.error = "argument " + opt + ": invalid number: '" + v + "'";
        }
    };
    for (size_t i = 0; i < argv.size() && a.error.empty(); ++i) {
        const std::string &s = argv[i];
        if (s == "-h" || s == "--help") a.help = true;
        else if (s == "-o" || s == "--output") a.output = need(i, s);
        else if (s == "--tracks") a.tracks = need(i, s);
        else if (s == "--save") a.save = need(i, s);
        else if (s == "--track") a.track.push_back(need(i, s));
        else if (s == "--tempo") {
            a.has_tempo = true;
            number(s, need(i, s), &a.tempo);
        } else if (s == "--voice") a.voice = need(i, s);
        else if (s == "--consonants") {
            a.has_consonants = true;
            number(s, need(i, s), &a.consonants);
        } else if (s == "--reverb") a.reverb = need(i, s);
        else if (s == "--anticipate") a.anticipate = 1;
        else if (s == "--no-anticipate") a.anticipate = 0;
        else if (s == "--from") number(s, need(i, s), &a.start);
        else if (s == "--list-voices") a.list_voices = true;
        else if (s == "--list-tracks") a.list_tracks = true;
        else if (s == "--version") a.version = true;
        else if (s == "-q" || s == "--quiet") a.quiet = true;
        else if (s == "--pronounce") {
            while (i + 1 < argv.size() && argv[i + 1].rfind("-", 0) != 0) a.pronounce.push_back(argv[++i]);
            if (a.pronounce.empty()) a.error = "argument --pronounce: expected at least one argument";
        } else if (!s.empty() && s[0] == '-') a.error = "unrecognized arguments: " + s;
        else if (a.file.empty()) a.file = s;
        else a.error = "unrecognized arguments: " + s;
    }
    return a;
}

bool wants_console(const CliArgs &a) {
    return a.help || !a.error.empty() || !a.output.empty() || !a.tracks.empty() || !a.save.empty() ||
           a.list_voices || a.list_tracks || !a.pronounce.empty() || a.version;
}

int run_cli(const CliArgs &args) {
    g_quiet = args.quiet;
    if (args.help) {
        out(usage());
        return 0;
    }
    if (!args.error.empty()) {
        std::fprintf(stderr, "%s", usage().c_str());
        return fail(args.error) + 1;
    }
    if (args.version) {
        out(std::string("Singing Voice Studio, built ") + __DATE__);
        out(std::string("wxWidgets ") + std::to_string(wxMAJOR_VERSION) + "." + std::to_string(wxMINOR_VERSION) +
            "." + std::to_string(wxRELEASE_NUMBER));
        for (const std::string &line : Registry::get().status()) out(line);
        return 0;
    }
    if (!args.file.empty() && !file_exists(args.file)) return fail("there is no file called " + args.file);
    if (args.file.empty() && (!args.output.empty() || !args.tracks.empty() || !args.save.empty() || args.list_tracks))
        return fail("which file? Give a project or a MIDI file to work on");

    if (!args.pronounce.empty()) {
        if (!Lexicon::get().ok()) return fail("VocalWriter's dictionary is not here, so words cannot be looked up");
        std::vector<std::string> words = args.pronounce;
        std::sort(words.begin(), words.end());
        for (const std::string &w : words) {
            auto ph = Lexicon::get().phonemes(w);
            std::string joined;
            for (const auto &p : ph) joined += (joined.empty() ? "" : " ") + p;
            out(w + "  " + (ph.empty() ? "(not in the dictionary)" : joined));
        }
    }
    auto voices = Registry::get().voices();
    if (args.list_voices) {
        for (const auto &v : voices) {
            char buf[256];
            std::snprintf(buf, sizeof buf, "%5d  %s%s", v.id, v.name.c_str(), v.available ? "" : "  (not installed)");
            out(buf);
        }
        say(std::to_string(voices.size()) + " voices");
    }
    if (args.file.empty()) return 0;

    Song song;
    try {
        song = open_song(args.file);
    } catch (const std::exception &e) {
        return fail("cannot open " + args.file + ": " + e.what());
    }
    std::map<int, int> programs = program_map();
    if (args.list_tracks) {
        for (size_t i = 0; i < song.tracks.size(); ++i) {
            const Track &t = song.tracks[i];
            char buf[512];
            std::snprintf(buf, sizeof buf, "%2zu  %-24s %-20s %3d%%  %s%s", i + 1, t.name.c_str(),
                          Registry::get().voice_name(track_voice(t, programs)).c_str(), t.volume,
                          pan_text(t.pan).c_str(), t.mute ? "  muted" : "");
            out(buf);
        }
        size_t n = 0;
        for (const Track &t : song.tracks) n += t.notes.size();
        say(std::to_string(song.tracks.size()) + " track" + s_if(song.tracks.size()) + ", " + std::to_string(n) +
            " notes, " + fmt_g(std::nearbyint(song.bpm)) + " bpm, " + format_sig(song.sig));
        if (args.output.empty() && args.tracks.empty() && args.save.empty()) return 0;
    }
    if (args.has_tempo) song.bpm = args.tempo;
    if (args.has_consonants) song.consonants = std::max(10.0, std::min(100.0, args.consonants)) / 100.0;
    if (!args.reverb.empty()) {
        try {
            song.reverb = parse_reverb(args.reverb);
        } catch (const std::exception &e) {
            return fail(e.what());
        }
    }
    if (args.anticipate >= 0) song.anticipate = args.anticipate == 1;
    if (!args.voice.empty()) {
        int pick;
        try {
            pick = find_voice(args.voice, voices);
        } catch (const std::exception &e) {
            return fail(e.what());
        }
        for (Track &t : song.tracks) t.voice_id = pick;
        say("every track is singing in " + Registry::get().voice_name(pick));
    }
    std::vector<const Track *> wanted;
    try {
        wanted = chosen_tracks(song.tracks, args.track);
    } catch (const std::exception &e) {
        return fail(e.what());
    }
    if (!args.save.empty()) {
        Song kept = song;
        kept.tracks.clear();
        for (const Track *t : wanted) kept.tracks.push_back(*t);
        try {
            save_project(args.save, kept);
        } catch (const std::exception &e) {
            return fail(std::string("could not write ") + args.save + ": " + e.what());
        }
        size_t n = 0;
        for (const Track &t : kept.tracks) n += t.notes.size();
        say("saved " + args.save + ": " + std::to_string(kept.tracks.size()) + " track" + s_if(kept.tracks.size()) +
            ", " + std::to_string(n) + " notes");
    }
    if (args.output.empty() && args.tracks.empty()) return 0;

    Mixer mixer;
    int status = 0;
    if (!args.output.empty()) {
        std::vector<const Track *> parts;
        for (const Track *t : audible(song.tracks))
            if (std::find(wanted.begin(), wanted.end(), t) != wanted.end()) parts.push_back(t);
        if (parts.empty()) return fail("every chosen track is muted");
        if (!render_to(mixer, make_request(song, parts, args.start, programs), args.output)) status = 1;
    }
    if (!args.tracks.empty()) {
        make_dirs(args.tracks);
        std::vector<Track> only;
        for (const Track *t : wanted) only.push_back(*t);
        std::string base = file_name(base_name(strip_extension(args.file)), "song");
        for (const auto &job : export_jobs(only, args.tracks, base))
            if (!render_to(mixer, make_request(song, {job.first}, args.start, programs), job.second)) status = 1;
    }
    return status;
}

}  // namespace svs
