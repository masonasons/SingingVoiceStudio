#include "core/project.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

#include <json.hpp>

#include "core/midifile.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/controls.h"

namespace svs {

using json = nlohmann::ordered_json;

const char *const kProjectSuffix = ".vws";
const char *const kProjectWildcard = "Singing Voice Studio project (*.vws)|*.vws";
const char *const kMidiWildcard = "MIDI files (*.mid;*.midi)|*.mid;*.midi";
const char *const kDefaultPhoneme = "AA";

namespace {
//: Bumped only if an older file would otherwise be read wrongly. Version 2
//: keeps the notes under `tracks`; a version 1 file is read as one track.
const int kVersion = 2;
const char *const kFormat = "vocalwriter-studio";
const char *const kClipKey = "vocalwriter-studio-notes";
//: VocalWriter's own defaults for a reverb that is asked for but not given
const Reverb kDefaultReverb{40, 24};
const char *const kBadInNames = "\\/:*?\"<>|";

double round_to(double v, int places) {
    double k = std::pow(10.0, places);
    return std::nearbyint(v * k) / k;
}

std::string lower(std::string s) {
    for (char &c : s) c = char(std::tolower((unsigned char)c));
    return s;
}
}  // namespace

std::string fmt_g(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

// -- Note --------------------------------------------------------------------

std::string Note::text() const {
    std::string out;
    for (size_t i = 0; i < phonemes.size(); ++i) {
        if (i) out += ' ';
        out += phonemes[i];
    }
    return out;
}

bool Note::is_rest() const {
    for (const std::string &p : phonemes)
        if (p != kRest) return false;
    return true;
}

std::string Note::label() const { return is_rest() ? "(rest)" : text(); }

// -- what is shown -------------------------------------------------------------

std::string file_name(const std::string &text, const std::string &fallback) {
    std::string out;
    for (char c : text) {
        bool bad = std::strchr(kBadInNames, c) != nullptr || (unsigned char)c < 32;
        out.push_back(bad ? '-' : c);
    }
    // collapse whitespace, then trim spaces, dots and hyphens from the ends
    std::istringstream words(out);
    std::string w, joined;
    while (words >> w) joined += (joined.empty() ? "" : " ") + w;
    size_t a = joined.find_first_not_of(" .-");
    size_t b = joined.find_last_not_of(" .-");
    if (a == std::string::npos) return fallback;
    return joined.substr(a, b - a + 1);
}

std::string pan_text(int pan) {
    if (!pan) return "centre";
    return std::string(pan > 0 ? "right " : "left ") + std::to_string(std::abs(pan));
}

double bar_beats(Sig sig) {
    return std::max(0.25, sig.first * 4.0 / std::max(sig.second, 1));
}

std::pair<int, double> bar_and_beat(double position, Sig sig) {
    double span = bar_beats(sig);
    int bar = int(std::floor(position / span));
    double within = position - bar * span;
    return {bar + 1, within * sig.second / 4.0 + 1};
}

std::string format_sig(Sig sig) {
    return std::to_string(sig.first) + "/" + std::to_string(sig.second);
}

Sig parse_sig(const std::string &text, Sig fallback) {
    std::string t;
    for (char c : text)
        if (c != ' ') t.push_back(c);
    size_t slash = t.find('/');
    if (slash == std::string::npos || t.find('/', slash + 1) != std::string::npos) return fallback;
    try {
        size_t used = 0;
        int num = std::stoi(t.substr(0, slash), &used);
        if (used != slash) return fallback;
        std::string d = t.substr(slash + 1);
        int den = std::stoi(d, &used);
        if (used != d.size()) return fallback;
        static const std::set<int> dens = {1, 2, 4, 8, 16, 32};
        if (num < 1 || num > 32 || !dens.count(den)) return fallback;
        return {num, den};
    } catch (...) {
        return fallback;
    }
}

static std::string semis(double v) {
    if (std::fabs(v) < kBendEpsilon) return "0";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+.3g", v);
    return buf;
}

std::pair<std::optional<double>, std::optional<double>> bend_ends(const Bend &bend) {
    if (bend.empty()) return {std::nullopt, std::nullopt};
    return {bend.front().second, bend.back().second};
}

Bend with_ends(const Bend &bend, std::optional<double> start, std::optional<double> end) {
    if (!start && !end) return {};
    Bend middle;
    for (const auto &p : bend)
        if (p.first > 0.0 && p.first < 1.0) middle.push_back(p);
    double s = start ? *start : (bend.empty() ? 0.0 : bend.front().second);
    Bend out{{0.0, s}};
    out.insert(out.end(), middle.begin(), middle.end());
    if (end)
        out.push_back({1.0, *end});
    else if (!bend.empty() && bend.back().first >= 1.0)
        out.push_back(bend.back());
    return out;
}

// How a curve reads in the list: blank, one value, start to end, or where it
// goes and comes back from -- what matters is where it sets off and arrives.
static std::string describe_curve(const Bend &curve, double epsilon,
                                  std::string (*say)(double)) {
    if (curve.empty()) return "";
    double start = curve.front().second, end = curve.back().second;
    if (std::fabs(end - start) >= epsilon) return say(start) + " to " + say(end);
    // the ends agree, so say what it does in between: a note that swoops up
    // and comes back is not a note with no bend
    double peak = curve.front().second;
    for (const auto &p : curve)
        if (std::fabs(p.second - start) > std::fabs(peak - start)) peak = p.second;
    if (std::fabs(peak - start) >= epsilon) return say(peak) + " and back";
    return std::fabs(start) >= epsilon ? say(start) : "";
}

std::string describe_bend(const Bend &bend) { return describe_curve(bend, kBendEpsilon, semis); }

static std::string wheel(double v) { return std::to_string(int(std::lrint(v))); }

std::string describe_mod(const Mod &mod) { return describe_curve(mod, kModEpsilon, wheel); }

int parse_pitch(const std::string &raw) {
    std::string tok = raw;
    tok.erase(0, tok.find_first_not_of(" \t"));
    tok.erase(tok.find_last_not_of(" \t") + 1);
    if (tok.empty()) throw std::invalid_argument("empty pitch");
    std::string digits = tok[0] == '-' ? tok.substr(1) : tok;
    if (!digits.empty() && std::all_of(digits.begin(), digits.end(), ::isdigit))
        return std::stoi(tok);
    static const std::map<char, int> names = {{'c', 0}, {'d', 2}, {'e', 4}, {'f', 5},
                                              {'g', 7}, {'a', 9}, {'b', 11}};
    auto it = names.find(char(std::tolower((unsigned char)tok[0])));
    if (it == names.end()) throw std::invalid_argument("bad pitch " + tok);
    int step = it->second;
    size_t i = 1;
    while (i < tok.size() && (tok[i] == '#' || tok[i] == 'b')) {
        step += tok[i] == '#' ? 1 : -1;
        ++i;
    }
    std::string oct = tok.substr(i);
    size_t used = 0;
    int octave;
    try {
        octave = std::stoi(oct, &used);
    } catch (...) {
        throw std::invalid_argument("bad pitch " + tok);
    }
    if (used != oct.size()) throw std::invalid_argument("bad pitch " + tok);
    return 12 * (octave + 1) + step;
}

std::string pitch_name(int midi) {
    static const char *sharp[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    int m = ((midi % 12) + 12) % 12;
    int octave = (midi - m) / 12 - 1;
    return std::string(sharp[m]) + std::to_string(octave);
}

// -- which tracks sing -------------------------------------------------------

std::vector<const Track *> audible(const std::vector<Track> &tracks) {
    std::vector<const Track *> solo, on;
    for (const Track &t : tracks) {
        if (t.solo && !t.mute) solo.push_back(&t);
        if (!t.mute) on.push_back(&t);
    }
    return solo.empty() ? on : solo;
}

int track_voice(const Track &track, const std::map<int, int> &program_map) {
    if (track.voice_id) return *track.voice_id;
    auto it = program_map.find(track.program);
    return it == program_map.end() ? 0 : it->second;
}

// A curve slides between the points of one note and holds between notes; a
// note with none of its own is given an explicit return to rest when the note
// before it ended away from rest, or one bent note would bend everything after.
static std::vector<BendPoint> gather(const std::vector<Note> &notes, Bend Note::*field,
                                     double epsilon) {
    std::vector<BendPoint> out;
    double at = 0.0, held = 0.0;
    for (const Note &n : notes) {
        const Bend &c = n.*field;
        if (c.empty()) {
            if (std::fabs(held) >= epsilon) {
                out.push_back({at, 0.0, false});
                held = 0.0;
            }
        } else {
            for (size_t k = 0; k < c.size(); ++k)
                out.push_back({at + c[k].first * n.beats, c[k].second, k + 1 < c.size()});
            held = c.back().second;
        }
        at += n.beats;
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const BendPoint &a, const BendPoint &b) { return a.beat < b.beat; });
    return out;
}

std::vector<BendPoint> timeline(const std::vector<Note> &notes) {
    return gather(notes, &Note::bend, kBendEpsilon);
}

std::vector<BendPoint> mod_timeline(const std::vector<Note> &notes) {
    return gather(notes, &Note::mod, kModEpsilon);
}

std::vector<std::pair<const Track *, std::string>> export_jobs(const std::vector<Track> &tracks,
                                                               const std::string &folder,
                                                               const std::string &base) {
    std::vector<std::pair<const Track *, std::string>> jobs;
    std::set<std::string> taken;
    for (const Track &t : tracks) {
        if (t.notes.empty()) continue;
        std::string stem = file_name(base, "song") + " - " + file_name(t.name);
        std::string name = stem;
        int k = 1;
        while (taken.count(lower(name))) {
            ++k;
            name = stem + " " + std::to_string(k);
        }
        taken.insert(lower(name));
        jobs.push_back({&t, join_path(folder, name + ".wav")});
    }
    return jobs;
}

// -- files --------------------------------------------------------------------

namespace {

json voice_doc(const VoiceStyle &v) {
    json out = json::object();
    size_t n;
    const VoiceControl *c = voice_controls(&n);
    for (size_t i = 0; i < n; ++i)
        if (v.*(c[i].field) != c[i].def) out[c[i].key] = v.*(c[i].field);
    return out;
}

VoiceStyle read_voice(const json &doc) {
    VoiceStyle v;
    if (!doc.is_object()) return v;
    size_t n;
    const VoiceControl *c = voice_controls(&n);
    for (size_t i = 0; i < n; ++i) {
        auto it = doc.find(c[i].key);
        if (it == doc.end()) continue;
        if (it->is_number()) v.*(c[i].field) = int(it->get<double>());
        else if (it->is_string()) {
            try {
                v.*(c[i].field) = std::stoi(it->get<std::string>());
            } catch (...) {
            }
        }
    }
    return clean_voice(v);
}

Reverb read_reverb(const json &doc) {
    if (!doc.is_object() || doc.empty()) return {0, 0};
    auto num = [&](const char *k, int d) -> double {
        auto it = doc.find(k);
        if (it == doc.end()) return d;
        if (it->is_number()) return it->get<double>();
        throw std::invalid_argument("reverb");
    };
    try {
        int room = int(std::lrint(num("room", kDefaultReverb.first)));
        int wet = int(std::lrint(num("wet", kDefaultReverb.second)));
        return {std::max(0, std::min(100, room)), std::max(0, std::min(100, wet))};
    } catch (...) {
        return {0, 0};
    }
}

json note_doc(const Note &n) {
    json d;
    d["phonemes"] = n.phonemes;
    d["pitch"] = n.pitch;
    d["beats"] = double(n.beats);
    d["word"] = n.word;
    if (!n.bend.empty()) {
        json b = json::array();
        for (const auto &p : n.bend) b.push_back({round_to(p.first, 4), round_to(p.second, 4)});
        d["bend"] = b;
    }
    // written only when there is one, so a song without the wheel is the same
    // file VocalWriter Studio writes -- and it reads this one, ignoring the wheel
    if (!n.mod.empty()) {
        json m = json::array();
        for (const auto &p : n.mod) m.push_back({round_to(p.first, 4), round_to(p.second, 4)});
        d["mod"] = m;
    }
    return d;
}

json track_doc(const Track &t) {
    json d;
    d["name"] = t.name;
    d["program"] = t.program;
    d["voice_id"] = t.voice_id ? json(*t.voice_id) : json(nullptr);
    d["volume"] = t.volume;
    d["pan"] = t.pan;
    d["mute"] = t.mute;
    d["solo"] = t.solo;
    json notes = json::array();
    for (const Note &n : t.notes) notes.push_back(note_doc(n));
    d["notes"] = notes;
    // A track with voice controls of its own writes what it has moved, which
    // may be nothing -- an empty entry still says "its own".
    if (t.voice) d["voice"] = voice_doc(*t.voice);
    if (t.reverb) d["reverb"] = {{"room", t.reverb->first}, {"wet", t.reverb->second}};
    if (t.consonants) d["consonants"] = *t.consonants;
    return d;
}

int as_int(const json &v, int fallback) {
    try {
        if (v.is_number()) return int(v.get<double>());
        if (v.is_string()) return std::stoi(v.get<std::string>());
        if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    } catch (...) {
    }
    return fallback;
}

double as_double(const json &v, double fallback) {
    try {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) return std::stod(v.get<std::string>());
    } catch (...) {
    }
    return fallback;
}

bool truthy(const json &v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_number()) return v.get<double>() != 0;
    if (v.is_string()) return !v.get<std::string>().empty();
    if (v.is_array() || v.is_object()) return !v.empty();
    return false;
}

std::vector<Note> read_notes(const json &entries) {
    std::vector<Note> out;
    if (!entries.is_array()) return out;
    for (const json &e : entries) {
        if (!e.is_object()) continue;
        try {
            Note n;
            auto ph = e.find("phonemes");
            if (ph != e.end() && ph->is_array())
                for (const json &p : *ph) n.phonemes.push_back(p.get<std::string>());
            if (n.phonemes.empty()) n.phonemes = {kRest};
            auto p = e.find("pitch");
            n.pitch = p == e.end() ? 60 : as_int(*p, 60);
            auto b = e.find("beats");
            n.beats = b == e.end() ? 0.5 : as_double(*b, 0.5);
            auto w = e.find("word");
            n.word = (w != e.end() && w->is_string()) ? w->get<std::string>() : "";
            auto bd = e.find("bend");
            if (bd != e.end() && bd->is_array())
                for (const json &pt : *bd)
                    n.bend.push_back({pt.at(0).get<double>(), pt.at(1).get<double>()});
            auto md = e.find("mod");
            if (md != e.end() && md->is_array())
                for (const json &pt : *md)
                    n.mod.push_back({pt.at(0).get<double>(),
                                     std::max(0.0, std::min(127.0, pt.at(1).get<double>()))});
            out.push_back(n);
        } catch (...) {
            continue;
        }
    }
    return out;
}

Track read_track(const json &doc, size_t index) {
    Track t;
    auto get = [&](const char *k) -> const json * {
        auto it = doc.find(k);
        return it == doc.end() ? nullptr : &*it;
    };
    const json *name = get("name");
    t.name = (name && name->is_string() && !name->get<std::string>().empty())
                 ? name->get<std::string>()
                 : "Voice " + std::to_string(index + 1);
    if (const json *v = get("voice_id"); v && !v->is_null()) t.voice_id = as_int(*v, 0);
    if (const json *v = get("voice")) t.voice = read_voice(*v);
    if (const json *v = get("reverb")) t.reverb = read_reverb(*v);
    if (const json *v = get("consonants"); v && !v->is_null()) {
        double c = as_double(*v, -1);
        if (c >= 0) t.consonants = c;
    }
    t.program = get("program") ? as_int(*get("program"), 0) : 0;
    t.volume = std::max(0, std::min(100, get("volume") ? as_int(*get("volume"), 100) : 100));
    t.pan = std::max(-100, std::min(100, get("pan") ? as_int(*get("pan"), 0) : 0));
    t.mute = get("mute") && truthy(*get("mute"));
    t.solo = get("solo") && truthy(*get("solo"));
    if (const json *n = get("notes")) t.notes = read_notes(*n);
    return t;
}

json song_doc(const Song &song) {
    json doc;
    doc["format"] = kFormat;
    doc["version"] = kVersion;
    doc["bpm"] = double(song.bpm);
    doc["time_signature"] = {song.sig.first, song.sig.second};
    doc["consonants"] = double(song.consonants);
    json tracks = json::array();
    for (const Track &t : song.tracks) tracks.push_back(track_doc(t));
    doc["tracks"] = tracks;
    json v = voice_doc(song.voice);
    if (!v.empty()) doc["voice"] = v;
    if (song.reverb.first || song.reverb.second)
        doc["reverb"] = {{"room", song.reverb.first}, {"wet", song.reverb.second}};
    // written only when it is off, so a file that says nothing about it sings
    // on the beat -- which is what a file should do
    if (!song.anticipate) doc["anticipate"] = false;
    return doc;
}

// Python's json.dump with indent=1, which is what the original wrote; the
// same layout keeps a file saved here readable next to one saved there.
// On Windows it wrote text mode's CRLF, so this does too.
std::string dump(const json &doc) {
    std::string text = doc.dump(1) + "\n";
#ifdef _WIN32
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    return crlf;
#else
    return text;
#endif
}

}  // namespace

void save_project(const std::string &path, const Song &song) {
    std::string text = dump(song_doc(song));
#ifdef _WIN32
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
#else
    FILE *f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("cannot write " + path);
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) throw std::runtime_error("cannot write " + path);
}

Song load_project(const std::string &path) {
    std::vector<unsigned char> bytes;
    if (!read_file(path, &bytes)) throw std::runtime_error("cannot read the file");
    json doc;
    try {
        doc = json::parse(bytes.begin(), bytes.end());
    } catch (const std::exception &) {
        throw std::runtime_error("not a Singing Voice Studio project");
    }
    if (!doc.is_object() || !(doc.contains("tracks") || doc.contains("notes")))
        throw std::runtime_error("not a Singing Voice Studio project");
    Song song;
    json parts = doc.value("tracks", json::array());
    if (!parts.is_array() || parts.empty()) {
        json one;
        one["name"] = "Voice 1";
        one["program"] = doc.contains("program") ? doc["program"] : json(0);
        one["notes"] = doc.contains("notes") ? doc["notes"] : json::array();
        parts = json::array({one});
    }
    for (size_t i = 0; i < parts.size(); ++i)
        if (parts[i].is_object()) song.tracks.push_back(read_track(parts[i], i));
    song.sig = kDefaultSig;
    if (doc.contains("time_signature")) {
        const json &s = doc["time_signature"];
        try {
            if (s.is_array() && s.size() >= 2) song.sig = {as_int(s[0], 4), as_int(s[1], 4)};
        } catch (...) {
        }
    }
    song.consonants = doc.contains("consonants") ? as_double(doc["consonants"], 1.0) : 1.0;
    song.bpm = doc.contains("bpm") ? as_double(doc["bpm"], 120) : 120;
    song.voice = doc.contains("voice") ? read_voice(doc["voice"]) : VoiceStyle();
    song.reverb = doc.contains("reverb") ? read_reverb(doc["reverb"]) : Reverb{0, 0};
    song.anticipate = doc.contains("anticipate") ? truthy(doc["anticipate"]) : true;
    if (song.tracks.empty()) {
        Track t;
        t.name = "Voice 1";
        song.tracks.push_back(t);
    }
    return song;
}

std::string notes_to_clipboard(const std::vector<Note> &notes) {
    json arr = json::array();
    for (const Note &n : notes) arr.push_back(note_doc(n));
    json doc;
    doc[kClipKey] = arr;
    return doc.dump(1);
}

std::vector<Note> notes_from_clipboard(const std::string &text) {
    try {
        json doc = json::parse(text);
        return read_notes(doc.at(kClipKey));
    } catch (...) {
        return {};
    }
}

std::string song_fingerprint(const Song &song) { return song_doc(song).dump(); }

// -- MIDI ---------------------------------------------------------------------

namespace {

//: Every track with notes, each with a name of its own: MIDI files routinely
//: leave every track unnamed, so a repeated name is numbered.
std::vector<std::pair<std::string, const MidiTrack *>> named_tracks(const MidiFile &midi) {
    std::vector<std::pair<std::string, const MidiTrack *>> out;
    std::map<std::string, int> seen;
    for (const MidiTrack &t : midi.tracks) {
        if (t.notes.empty()) continue;
        std::string base = t.name.empty() ? "untitled" : t.name;
        int k = ++seen[base];
        out.push_back({k == 1 ? base : base + " " + std::to_string(k), &t});
    }
    return out;
}

double midi_tempo(const MidiFile &midi) {
    for (const MidiTrack &t : midi.tracks) {
        if (t.tempos.empty()) continue;
        auto first = *std::min_element(t.tempos.begin(), t.tempos.end());
        return 60e6 / double(first.second);
    }
    return 120.0;
}

Sig midi_sig(const MidiFile &midi) {
    for (const MidiTrack &t : midi.tracks) {
        if (t.time_sigs.empty()) continue;
        auto first = *std::min_element(t.time_sigs.begin(), t.time_sigs.end(),
                                       [](const MidiTrack::TimeSig &a, const MidiTrack::TimeSig &b) {
                                           if (a.tick != b.tick) return a.tick < b.tick;
                                           if (a.num != b.num) return a.num < b.num;
                                           return a.den < b.den;
                                       });
        return {first.num, first.den};
    }
    return kDefaultSig;
}

double quantise(double beats, double grid) {
    if (!grid) return beats;
    return std::max(grid, std::nearbyint(beats / grid) * grid);
}

const double kSixteenthGrid = 0.25;
const double kTripletGrid = 1.0 / 12.0;
const double kGridSlop = 1.0 / 32.0;

double off_grid(double beats, double grid) { return std::fabs(beats - quantise(beats, grid)); }

//: A twelfth of a beat when a real share of the lengths are triplets, which
//: a sixteenth cannot hold; otherwise a sixteenth, as it always was.
double midi_grid(const MidiFile &midi) {
    double div = midi.division ? midi.division : 480;
    int total = 0, triplets = 0;
    for (const MidiTrack &t : midi.tracks)
        for (const MidiNote &n : t.notes) {
            double b = std::max(n.duration, 1L) / div;
            ++total;
            if (off_grid(b, kSixteenthGrid) > kGridSlop && off_grid(b, kTripletGrid) <= kGridSlop)
                ++triplets;
        }
    if (!total) return kSixteenthGrid;
    return triplets * 10 >= total ? kTripletGrid : kSixteenthGrid;
}

//: the track's bend as [(tick, semitones)], the range resolved
std::vector<std::pair<long, double>> bend_curve(const MidiTrack &track) {
    auto ranges = track.bend_range;
    std::sort(ranges.begin(), ranges.end());
    if (ranges.empty()) ranges.push_back({0, 2});
    auto bends = track.bends;
    std::sort(bends.begin(), bends.end());
    std::vector<std::pair<long, double>> out;
    for (const auto &b : bends) {
        int span = ranges[0].second;
        for (const auto &r : ranges) {
            if (r.first <= b.first) span = r.second;
            else break;
        }
        out.push_back({b.first, b.second / 8192.0 * span});
    }
    return out;
}

//: The bend (or mod wheel) points inside one note, placed 0 to 1 across it; a MIDI step is
//: written out as the two points a step really has.
Bend curve_over(const std::vector<std::pair<long, double>> &curve, long start, long end,
                double epsilon) {
    double span = double(std::max(end - start, 1L));
    Bend inside;
    double held = 0.0;
    for (const auto &c : curve) {
        if (c.first < start) held = c.second;
        if (start <= c.first && c.first < end) inside.push_back({(c.first - start) / span, c.second});
    }
    if (inside.empty() && std::fabs(held) < epsilon) return {};
    if (inside.empty() || inside[0].first > 0.0) inside.insert(inside.begin(), {0.0, held});
    Bend steps;
    for (size_t k = 0; k < inside.size(); ++k) {
        if (k && inside[k - 1].second != inside[k].second)
            steps.push_back({inside[k].first, inside[k - 1].second});
        steps.push_back(inside[k]);
    }
    for (auto &p : steps) p = {round_to(p.first, 4), round_to(p.second, 4)};
    return steps;
}

//: Words still needing a pronunciation, lyrics broken across notes with
//: hyphens ("count-", "ry") put back together.
std::vector<std::pair<std::string, std::vector<int>>> pending_words(const std::vector<Note> &rows) {
    std::vector<std::pair<std::string, std::vector<int>>> pending;
    std::vector<int> run;
    std::string letters;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Note &r = rows[i];
        if (!r.phonemes.empty() || r.word.empty()) continue;
        run.push_back(int(i));
        letters += r.word;
        if (r.word.back() == '-') continue;
        std::string clean;
        for (char c : letters)
            if (std::isalpha((unsigned char)c) || c == '\'') clean.push_back(c);
        if (!clean.empty()) pending.push_back({clean, run});
        run.clear();
        letters.clear();
    }
    return pending;
}

ImportedPart from_midi(const MidiFile &midi, const MidiTrack &track, const std::string &name,
                       double grid) {
    double div = midi.division ? midi.division : 480;
    auto curve = bend_curve(track);
    // CC 1, the mod wheel, as it stands: already 0 to 127
    std::vector<std::pair<long, double>> wheel;
    for (const auto &m : track.mods) wheel.push_back({m.first, double(m.second)});
    std::sort(wheel.begin(), wheel.end());
    std::vector<MidiNote> ordered = track.notes;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const MidiNote &a, const MidiNote &b) { return a.tick < b.tick; });
    // Rounding where each note falls rather than how long each one is, and
    // counting from the start of the file, so nothing drifts and a part that
    // comes in late stays late.
    double step = grid ? grid : 1.0 / div;
    auto edge = [&](double ticks) { return long(std::lrint(ticks / div / step)); };
    ImportedPart part;
    part.name = name;
    long at = 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        const MidiNote &n = ordered[i];
        long span = std::max(n.duration, 1L);
        long start = edge(double(n.tick));
        long end = std::max(start + 1, edge(double(n.tick + span)));
        if (i + 1 < ordered.size()) {
            long nxt = std::max(start + 1, edge(double(ordered[i + 1].tick)));
            long gap = std::lrint((ordered[i + 1].tick - n.tick - span) / div / step);
            end = gap <= 0 ? nxt : std::min(end, nxt);
        }
        if (start > at) {
            part.notes.push_back(Note({kRest}, n.pitch, (start - at) * step, "", {}));
            at = start;
        }
        std::string word = n.text;
        word.erase(0, word.find_first_not_of(" \t\r\n"));
        word.erase(word.find_last_not_of(" \t\r\n") + 1);
        std::vector<std::string> ph;
        if (!n.phonemes.empty()) {
            for (const std::string &x : split_phonemes(n.phonemes)) ph.push_back(palette_alias(x));
        } else if (word.empty()) {
            ph = {kDefaultPhoneme};
        }
        long length = std::max(1L, end - at);
        part.notes.push_back(Note(ph, n.pitch, length * step, word,
                                  curve_over(curve, n.tick, n.tick + span, kBendEpsilon),
                                  curve_over(wheel, n.tick, n.tick + span, kModEpsilon)));
        at += length;
    }
    part.pending = pending_words(part.notes);
    return part;
}

}  // namespace

std::vector<std::pair<std::string, int>> midi_tracks(const std::string &path) {
    MidiFile midi = read_midi(path);
    std::vector<std::pair<std::string, int>> out;
    for (const auto &nt : named_tracks(midi)) out.push_back({nt.first, int(nt.second->notes.size())});
    return out;
}

Imported import_midi(const std::string &path, const std::vector<std::string> &names) {
    MidiFile midi = read_midi(path);
    auto named = named_tracks(midi);
    if (named.empty()) throw std::runtime_error("this file has no notes in it");
    double grid = midi_grid(midi);
    Imported out;
    out.bpm = midi_tempo(midi);
    out.sig = midi_sig(midi);
    for (const std::string &want : names) {
        const MidiTrack *track = nullptr;
        for (const auto &nt : named)
            if (nt.first == want) track = nt.second;
        if (!track) throw std::runtime_error("no track named " + want);
        out.parts.push_back(from_midi(midi, *track, want, grid));
    }
    return out;
}

int pronounce(std::vector<ImportedPart> &parts,
              const std::map<std::string, std::vector<std::string>> &found) {
    int got = 0;
    for (ImportedPart &part : parts) {
        for (const auto &pw : part.pending) {
            auto it = found.find(pw.first);
            const std::vector<int> &indices = pw.second;
            if (it == found.end() || it->second.empty()) {
                for (int i : indices)
                    if (i >= 0 && i < int(part.notes.size()) && part.notes[i].phonemes.empty())
                        part.notes[i].phonemes = {kDefaultPhoneme};
                continue;
            }
            auto groups = regroup(it->second, int(indices.size()));
            while (groups.size() < indices.size()) groups.push_back({});
            for (size_t k = 0; k < indices.size(); ++k) {
                int i = indices[k];
                if (i < 0 || i >= int(part.notes.size())) continue;
                part.notes[i].phonemes = groups[k].empty() ? std::vector<std::string>{kRest} : groups[k];
            }
            ++got;
        }
    }
    return got;
}

}  // namespace svs
