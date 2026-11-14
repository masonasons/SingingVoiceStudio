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
#include "voices/family.h"

namespace svs {

using json = nlohmann::ordered_json;

const char *const kProjectSuffix = ".vws";
const char *const kProjectWildcard = "Singing Voice Studio project (*.vws)|*.vws";
const char *const kImportWildcard =
    "MIDI files and VocalWriter songs (*.mid;*.midi;*.trk)|*.mid;*.midi;*.trk|"
    "MIDI files (*.mid;*.midi)|*.mid;*.midi|VocalWriter songs (*.trk)|*.trk";
const char *const kMidiWildcard = "MIDI files (*.mid;*.midi)|*.mid;*.midi";

bool is_midi_file(const std::string &path) {
    std::string e = extension_lower(path);
    return e == ".mid" || e == ".midi";
}
const char *const kDefaultPhoneme = "AA";

const NoteControl kNoteControls[] = {
    {"volume", "Volume", 0, 127, "0 to 127"},
    {"color", "Colour", 0, 127, "0 to 127, brighter as it rises"},
    {"noise", "Noise", 0, 127, "0 to 127, the hiss in the voice"},
    {"breath", "Breath", 0, 127, "0 to 127, air in the tone"},
    {"vibrato", "Vibrato depth", 0, 127, "0 to 127"},
    {"vibrato_rate", "Vibrato rate", 0, 127, "0 to 127"},
    {"portamento", "Portamento", 0, 127, "0 to 127, the glide between notes"},
    {"chorus", "Chorus", -100, 100, "-100 to 100"},
    {"detune", "Detune", -8192, 8191, "-8192 to 8191"},
    {"bend_range", "Bend range", 0, 12, "0 to 12, semitones a full bend reaches; VocalWriter starts at 2"},
    {"lead_pitch", "Lead-in pitch", 0, 127,
     "on a rest, the MIDI pitch the next note's first consonants are sung at, when not the next note's"},
    {"sustain", "Sustain pedal", 0, 127,
     "the GM Soundbank's instruments only: 64 and up holds every note on until it is let up"},
};
const size_t kNoteControlCount = sizeof kNoteControls / sizeof kNoteControls[0];

const NoteControl *note_control(const std::string &key) {
    for (const NoteControl &c : kNoteControls)
        if (key == c.key) return &c;
    return nullptr;
}

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

Note Note::tempo_change(double bpm) {
    Note n({}, kDefaultPitch, 0.0);
    n.controller = true;
    n.tempo = bpm;
    return n;
}

bool Note::is_rest() const {
    if (controller) return false;
    for (const std::string &p : phonemes)
        if (p != kRest) return false;
    return true;
}

std::string Note::label() const {
    if (controller) {
        std::string said;
        if (tempo > 0) said = "tempo change to " + fmt_g(tempo);
        if (sig) said += std::string(said.empty() ? "" : ", ") + "time signature change to " + format_sig(*sig);
        if (voice_id) said += std::string(said.empty() ? "" : ", ") + "voice change";
        if (pan) said += std::string(said.empty() ? "" : ", ") + "pan " + std::to_string(*pan);
        for (const auto &c : controls) {
            const NoteControl *k = note_control(c.first);
            if (k && !c.second.empty())
                said += std::string(said.empty() ? "" : ", ") + k->label + " " + fmt_g(c.second.front().second);
        }
        return "(" + (said.empty() ? std::string("controller") : said) + ")";
    }
    return is_rest() ? "(rest)" : text();
}

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
        // a Tempo track's solo, set before it had none, silences nothing
        if (t.solo && !t.mute && !is_tempo_track(t)) solo.push_back(&t);
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
        // a note taking no time, as a chord's, carries no curve
        if (n.controller || n.beats <= 0) continue;
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
                                                               const std::string &base,
                                                               const std::string &ext) {
    std::vector<std::pair<const Track *, std::string>> jobs;
    std::set<std::string> taken;
    for (const Track &t : tracks) {
        // a backing track is a file already: there is nothing to write
        if (!sings(t) || t.is_backing()) continue;
        std::string stem = file_name(base, "song") + " - " + file_name(t.name);
        std::string name = stem;
        int k = 1;
        while (taken.count(lower(name))) {
            ++k;
            name = stem + " " + std::to_string(k);
        }
        taken.insert(lower(name));
        jobs.push_back({&t, join_path(folder, name + ext)});
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

//: A controller's changes, only those it makes.
json controller_doc(const Note &n) {
    json c = json::object();
    if (n.tempo > 0) c["tempo"] = round_to(n.tempo, 3);
    if (n.sig) c["time_signature"] = {n.sig->first, n.sig->second};
    if (n.voice_id) c["voice_id"] = *n.voice_id;
    if (n.voice_name) c["voice_name"] = *n.voice_name;
    if (n.pan) c["pan"] = *n.pan;
    // VocalWriter's controls, each as the value it sets
    for (const auto &k : n.controls)
        if (!k.second.empty()) c["controls"][k.first] = k.second.front().second;
    return c;
}

json note_doc(const Note &n) {
    json d;
    // on the clipboard, where a controller goes in among the notes
    if (n.controller) {
        d["controller"] = controller_doc(n);
        return d;
    }
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
    // only when it differs from its beats
    if (n.hold > 0) d["hold"] = round_to(n.hold, 6);
    // VocalWriter's own, again only when there are any
    if (n.velocity) d["velocity"] = n.velocity;
    if (!n.controls.empty()) {
        json c = json::object();
        for (const auto &kv : n.controls) {
            json pts = json::array();
            for (const auto &p : kv.second) pts.push_back({round_to(p.first, 6), round_to(p.second, 4)});
            c[kv.first] = pts;
        }
        d["controls"] = c;
    }
    return d;
}

json track_doc(const Track &t) {
    json d;
    d["name"] = t.name;
    d["program"] = t.program;
    d["voice_id"] = t.voice_id ? json(*t.voice_id) : json(nullptr);
    if (t.voice_name) d["voice_name"] = *t.voice_name;
    d["volume"] = t.volume;
    d["pan"] = t.pan;
    // always: a song file without it is from before the default changed
    d["velocity"] = t.velocity;
    d["mute"] = t.mute;
    d["solo"] = t.solo;
    // The controllers apart from the notes, each with how many notes come
    // before it: the notes are what VocalWriter Studio reads, and a track
    // without controllers is the same file it writes.
    json notes = json::array(), controllers = json::array();
    for (const Note &n : t.notes) {
        if (!n.controller) {
            notes.push_back(note_doc(n));
            continue;
        }
        json c = controller_doc(n);
        c["at"] = notes.size();
        controllers.push_back(c);
    }
    d["notes"] = notes;
    if (!controllers.empty()) d["controllers"] = controllers;
    // A track with voice controls of its own writes what it has moved, which
    // may be nothing -- an empty entry still says "its own".
    if (t.voice) d["voice"] = voice_doc(*t.voice);
    if (t.reverb) d["reverb"] = {{"room", t.reverb->first}, {"wet", t.reverb->second}};
    if (t.consonants) d["consonants"] = *t.consonants;
    // a backing track's file, which VocalWriter Studio knows nothing of: it
    // reads the track as a part with no notes
    if (t.is_backing()) {
        d["audio"] = t.audio;
        if (t.audio_beat != 0) d["audio_beat"] = round_to(t.audio_beat, 6);
    }
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

void read_controller(const json &c, Note &n) {
    n = Note::tempo_change(0);
    if (!c.is_object()) return;
    if (c.contains("tempo")) n.tempo = std::max(0.0, as_double(c["tempo"], 0));
    if (c.contains("time_signature") && c["time_signature"].is_array() && c["time_signature"].size() >= 2) {
        // one that is no time signature is left out
        Sig s = parse_sig(std::to_string(as_int(c["time_signature"][0], 0)) + "/" +
                          std::to_string(as_int(c["time_signature"][1], 0)), {0, 0});
        if (s.first) n.sig = s;
    }
    if (c.contains("voice_id") && !c["voice_id"].is_null()) n.voice_id = as_int(c["voice_id"], 0);
    if (c.contains("voice_name") && c["voice_name"].is_string()) n.voice_name = c["voice_name"].get<std::string>();
    if (c.contains("pan") && c["pan"].is_number()) n.pan = std::max(-100, std::min(100, as_int(c["pan"], 0)));
    if (c.contains("controls") && c["controls"].is_object())
        for (auto it = c["controls"].begin(); it != c["controls"].end(); ++it)
            if (const NoteControl *k = note_control(it.key()))
                n.controls[it.key()] = {{0.0, std::max(double(k->lo), std::min(double(k->hi), as_double(it.value(), 0)))}};
}

std::vector<Note> read_notes(const json &entries) {
    std::vector<Note> out;
    if (!entries.is_array()) return out;
    for (const json &e : entries) {
        if (!e.is_object()) continue;
        try {
            Note n;
            if (e.contains("controller")) {
                read_controller(e["controller"], n);
                out.push_back(n);
                continue;
            }
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
            auto vel = e.find("velocity");
            if (vel != e.end()) n.velocity = std::max(0, std::min(127, as_int(*vel, 0)));
            auto hold = e.find("hold");
            if (hold != e.end()) n.hold = std::max(0.0, as_double(*hold, 0));
            auto ctl = e.find("controls");
            if (ctl != e.end() && ctl->is_object())
                for (auto it = ctl->begin(); it != ctl->end(); ++it) {
                    const NoteControl *c = note_control(it.key());
                    if (!c || !it->is_array()) continue;
                    auto &pts = n.controls[c->key];
                    for (const json &pt : *it)
                        pts.push_back({std::max(0.0, std::min(1.0, pt.at(0).get<double>())),
                                       std::max(double(c->lo), std::min(double(c->hi), pt.at(1).get<double>()))});
                    std::stable_sort(pts.begin(), pts.end(), [](const std::pair<double, double> &a,
                                                                const std::pair<double, double> &b) {
                        return a.first < b.first;
                    });
                    if (pts.empty()) n.controls.erase(c->key);
                }
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
    if (const json *v = get("voice_name"); v && v->is_string()) t.voice_name = v->get<std::string>();
    if (const json *v = get("voice")) t.voice = read_voice(*v);
    if (const json *v = get("reverb")) t.reverb = read_reverb(*v);
    if (const json *v = get("consonants"); v && !v->is_null()) {
        double c = as_double(*v, -1);
        if (c >= 0) t.consonants = c;
    }
    t.program = get("program") ? as_int(*get("program"), 0) : 0;
    t.volume = std::max(0, std::min(kMaxVolume, get("volume") ? as_int(*get("volume"), 100) : 100));
    t.pan = std::max(-100, std::min(100, get("pan") ? as_int(*get("pan"), 0) : 0));
    int velocity = get("velocity") ? as_int(*get("velocity"), kPlainVelocity) : kPlainVelocity;
    t.velocity = std::max(1, std::min(127, velocity));
    t.mute = get("mute") && truthy(*get("mute"));
    t.solo = get("solo") && truthy(*get("solo"));
    if (const json *n = get("notes")) t.notes = read_notes(*n);
    if (const json *cs = get("controllers"); cs && cs->is_array()) {
        // in order, each before the note it was saved in front of
        size_t placed = 0, seen = 0;
        for (const json &c : *cs) {
            if (!c.is_object()) continue;
            Note n;
            read_controller(c, n);
            size_t at = c.contains("at") ? size_t(std::max(0, as_int(c.at("at"), 0))) : 0;
            while (placed < t.notes.size() && (seen < at || t.notes[placed].controller)) {
                if (!t.notes[placed].controller) ++seen;
                ++placed;
            }
            t.notes.insert(t.notes.begin() + long(placed), n);
            ++placed;
        }
    }
    if (const json *v = get("audio"); v && v->is_string()) t.audio = v->get<std::string>();
    if (const json *v = get("audio_beat")) t.audio_beat = as_double(*v, 0);
    return t;
}

json song_doc(const Song &song) {
    json doc;
    doc["format"] = kFormat;
    doc["version"] = kVersion;
    doc["bpm"] = double(song.bpm);
    // only a song that keeps its end says so, so every other file stays as it was
    if (song.end > 0) doc["end"] = song.end;
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
    song.end = doc.contains("end") ? std::max(0.0, as_double(doc["end"], 0)) : 0;
    song.voice = doc.contains("voice") ? read_voice(doc["voice"]) : VoiceStyle();
    song.reverb = doc.contains("reverb") ? read_reverb(doc["reverb"]) : Reverb{0, 0};
    song.anticipate = doc.contains("anticipate") ? truthy(doc["anticipate"]) : true;
    // a backing track whose start was saved in seconds, as it was before it
    // was in beats, starts on the beat the song's tempo had there
    Clock clock = song.clock();
    for (size_t i = 0, k = 0; i < parts.size() && k < song.tracks.size(); ++i) {
        if (!parts[i].is_object()) continue;
        Track &t = song.tracks[k++];
        if (!parts[i].contains("audio_beat") && parts[i].contains("audio_start"))
            t.audio_beat = std::nearbyint(clock.beat_at(as_double(parts[i]["audio_start"], 0)) * 1e6) / 1e6;
    }
    if (song.tracks.empty()) {
        Track t;
        t.name = "Voice 1";
        song.tracks.push_back(t);
    }
    // A backing track's file that is not where it was, but is beside the
    // project: the two were moved, or sent to someone, together.
    for (Track &t : song.tracks) {
        if (!t.is_backing() || file_exists(t.audio)) continue;
        std::string beside = join_path(dir_name(path), base_name(t.audio));
        if (file_exists(beside)) t.audio = beside;
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

int close_short_rests(std::vector<Note> &notes, double longest, bool in_words) {
    if (longest <= 0 && !in_words) return 0;
    // a note whose word is still to be looked up has no phonemes yet, and
    // is no rest
    auto rest = [](const Note &n) { return !n.controller && !n.phonemes.empty() && n.is_rest(); };
    auto sung = [&](const Note &n) { return !n.controller && !rest(n); };
    // a curve over the note, moved into the first `a` of `a + b` beats, and
    // the rest's after it; a note without one was at rest, and holds there
    auto stretch = [](Bend &mine, const Bend &theirs, double a, double b) {
        double total = a + b;
        if (mine.empty() && !theirs.empty()) mine = {{0.0, 0.0}, {1.0, 0.0}};
        for (auto &p : mine) p.first = p.first * a / total;
        for (const auto &p : theirs) mine.push_back({(a + p.first * b) / total, p.second});
    };
    std::vector<Note> out;
    int closed = 0;
    std::string word;       // the last syllable sung, "al-" running on into "right"
    for (size_t i = 0; i < notes.size(); ++i) {
        const Note &r = notes[i];
        if (!r.word.empty()) word = r.word;
        bool inside = in_words && !word.empty() && word.back() == '-';
        if (rest(r) && (r.beats <= longest + 1e-9 || inside) && !out.empty() && sung(out.back()) &&
            i + 1 < notes.size() && sung(notes[i + 1])) {
            Note &n = out.back();
            double a = n.beats, b = r.beats;
            stretch(n.bend, r.bend, a, b);
            stretch(n.mod, r.mod, a, b);
            for (auto &c : n.controls)
                for (auto &p : c.second) p.first = p.first * a / (a + b);
            for (const auto &c : r.controls)
                for (const auto &p : c.second) n.controls[c.first].push_back({(a + p.first * b) / (a + b), p.second});
            n.beats = a + b;
            ++closed;
            continue;
        }
        out.push_back(r);
    }
    notes = out;
    return closed;
}

Song fresh_song() {
    Song song;
    song.voice.portamento = kVocalWriterPortamento;
    song.reverb = kVocalWriterReverb;
    return song;
}

Track backing_track(const std::string &path) {
    Track t;
    t.audio = path;
    t.name = base_name(strip_extension(path));
    t.volume = 50;
    return t;
}

Song backing_song(const std::string &path) {
    Song song = fresh_song();
    Track voice;
    voice.name = "Voice 1";
    song.tracks = {voice, backing_track(path)};
    return song;
}

// -- MIDI ---------------------------------------------------------------------

namespace {

int program_at_start(const MidiTrack &track);
bool drum_track(const MidiTrack &track);

//: Every track with notes, each with a name of its own: MIDI files routinely
//: leave every track unnamed, so an unnamed one is named after its
//: instrument, as `program_names` names it, and a repeated name is numbered.
std::vector<std::pair<std::string, const MidiTrack *>> named_tracks(const MidiFile &midi,
                                                                    const ProgramNames &program_names) {
    std::vector<std::pair<std::string, const MidiTrack *>> out;
    std::map<std::string, int> seen;
    for (const MidiTrack &t : midi.tracks) {
        if (t.notes.empty()) continue;
        int program = std::max(0, program_at_start(t));
        bool drums = drum_track(t);
        std::string base = !t.name.empty() ? t.name
                           : program_names ? program_names(program, drums)
                           : drums         ? std::string("Drums")
                                           : std::string(gm_program_name(program));
        int k = ++seen[base];
        out.push_back({k == 1 ? base : base + " " + std::to_string(k), &t});
    }
    return out;
}

//: A tempo as VocalWriter's import reads it: 60000000 over the
//: microseconds a beat, cut to a whole number.
double whole_bpm(long us) { return us > 0 ? double(60000000L / us) : 120.0; }

double midi_tempo(const MidiFile &midi) {
    for (const MidiTrack &t : midi.tracks) {
        if (t.tempos.empty()) continue;
        auto first = *std::min_element(t.tempos.begin(), t.tempos.end());
        return whole_bpm(first.second);
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

//: The time signature's changes after the one it starts with, at the beats
//: they fall on.
std::vector<SigChange> midi_sig_changes(const MidiFile &midi) {
    double div = midi.division ? midi.division : 480;
    std::vector<MidiTrack::TimeSig> all;
    for (const MidiTrack &t : midi.tracks) all.insert(all.end(), t.time_sigs.begin(), t.time_sigs.end());
    std::stable_sort(all.begin(), all.end(),
                     [](const MidiTrack::TimeSig &a, const MidiTrack::TimeSig &b) { return a.tick < b.tick; });
    std::vector<SigChange> out;
    Sig now = midi_sig(midi);
    for (const MidiTrack::TimeSig &s : all) {
        Sig sig{s.num, s.den};
        if (s.tick <= 0 || sig == now) continue;
        out.push_back({s.tick / div, sig});
        now = sig;
    }
    return clean_sig_changes(out);
}

//: The tempo changes after the first, at the beats they fall on.
std::vector<TempoChange> midi_tempo_changes(const MidiFile &midi) {
    double div = midi.division ? midi.division : 480;
    std::vector<std::pair<long, long>> all;
    for (const MidiTrack &t : midi.tracks) all.insert(all.end(), t.tempos.begin(), t.tempos.end());
    std::stable_sort(all.begin(), all.end(),
                     [](const std::pair<long, long> &a, const std::pair<long, long> &b) { return a.first < b.first; });
    std::vector<TempoChange> out;
    for (size_t i = 1; i < all.size(); ++i)
        if (all[i].second != all[i - 1].second) out.push_back({all[i].first / div, whole_bpm(all[i].second)});
    return clean_tempo_changes(out);
}

//: the track's bend as [(tick, semitones)], the range resolved: two
//: semitones until RPN 0 first sets it, as MIDI starts, and a change of
//: range moving a wheel already off centre, as a synthesiser hears it
std::vector<std::pair<long, double>> bend_curve(const MidiTrack &track) {
    // in the file's order where two fall on one tick
    auto by_tick = [](const std::pair<long, int> &a, const std::pair<long, int> &b) { return a.first < b.first; };
    auto ranges = track.bend_range;
    std::stable_sort(ranges.begin(), ranges.end(), by_tick);
    auto bends = track.bends;
    std::stable_sort(bends.begin(), bends.end(), by_tick);
    std::vector<std::pair<long, double>> out;
    int span = 2, wheel = 0;
    size_t r = 0;
    auto ranges_to = [&](long tick, bool through) {
        for (; r < ranges.size() && (through || ranges[r].first <= tick); ++r) {
            if (ranges[r].second == span) continue;
            span = ranges[r].second;
            if (wheel != 0) out.push_back({ranges[r].first, wheel / 8192.0 * span});
        }
    };
    for (const auto &b : bends) {
        ranges_to(b.first, false);
        wheel = b.second;
        out.push_back({b.first, b.second / 8192.0 * span});
    }
    ranges_to(0, true);
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

//: What of a piece of lyric is looked up: its letters, apostrophes and
//: figures (in_word).
std::string letters_of(const std::string &text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i)
        if (in_word(text, i)) out.push_back(text[i]);
    return out;
}

//: Whether a syllable only holds on to the vowel sung before it, as karaoke
//: files write a melisma ("Go-", "o-", "o"): no letters, or the last vowel
//: letter again.
bool holds_vowel(const std::string &before, const std::string &text) {
    if (before.empty()) return false;
    std::string t = letters_of(text);
    if (t.empty()) return true;
    char last = char(std::tolower((unsigned char)before.back()));
    if (!std::strchr("aeiouy", last)) return false;
    for (char c : t)
        if (std::tolower((unsigned char)c) != last) return false;
    return true;
}

//: The piece a word only repeats ("yo-", "yo"), or nothing. The dictionary
//: guesses at such a word ("yoyo" as "y OY . OW") and knows the piece.
std::string repeated(const std::vector<std::string> &pieces) {
    if (pieces.size() < 2 || pieces[0].empty()) return "";
    for (const std::string &p : pieces)
        if (p.size() != pieces[0].size() ||
            !std::equal(p.begin(), p.end(), pieces[0].begin(), [](char a, char b) {
                return std::tolower((unsigned char)a) == std::tolower((unsigned char)b);
            }))
            return "";
    return pieces[0];
}

//: Words still needing a pronunciation, lyrics broken across notes with
//: hyphens ("count-", "ry") put back together, a held vowel left out, a
//: repeated piece looked up once. The pieces keep their hyphens, for a word
//: the dictionary only knows in pieces (Lexicon::phonemes). A note with no
//: lyric that holds on to the word before it (see hold_vowels) goes with
//: that word. A controller among them is passed over.
std::vector<std::pair<std::string, std::vector<int>>> pending_words(const std::vector<Note> &rows) {
    std::vector<std::pair<std::string, std::vector<int>>> pending;
    std::vector<int> run;
    std::string letters;
    std::vector<std::string> pieces;
    int before = -1;    // the row before, controllers left out
    for (size_t i = 0; i < rows.size(); ++i) {
        const Note &r = rows[i];
        if (r.controller) continue;
        int was = before;
        before = int(i);
        if (!r.phonemes.empty()) continue;
        if (r.word.empty()) {
            if (!run.empty()) run.push_back(int(i));
            else if (!pending.empty() && pending.back().second.back() == was)
                pending.back().second.push_back(int(i));
            continue;
        }
        run.push_back(int(i));
        if (!holds_vowel(letters, r.word)) {
            letters += letters_of(r.word);
            pieces.push_back(letters_of(r.word));
        }
        if (r.word.back() == '-') continue;
        std::string once = repeated(pieces);
        if (once.empty())
            for (const std::string &p : pieces)
                if (!p.empty()) once += (once.empty() ? "" : "-") + p;
        if (!letters.empty()) pending.push_back({once, run});
        run.clear();
        letters.clear();
        pieces.clear();
    }
    return pending;
}

//: A note with no lyric right after a sung one holds on to its vowel, as a
//: karaoke file leaves the rest of a melisma ("name" on three notes sings
//: "n EY", "EY", "EY m"), and so does one after a rest inside a word that
//: goes on ("g-", rest, two notes, "o."); any other, or one after nothing
//: to look up, sings kDefaultPhoneme.
void hold_vowels(std::vector<Note> &notes) {
    bool open = false;
    for (size_t i = 0; i < notes.size(); ++i) {
        Note &n = notes[i];
        if (!n.word.empty()) {
            open = n.phonemes.empty() && n.word.back() == '-';
        } else if (!n.phonemes.empty()) {
            open = open && n.is_rest();
        } else if (!open && (i == 0 || !notes[i - 1].phonemes.empty())) {
            n.phonemes = {kDefaultPhoneme};
        }
    }
    std::vector<bool> held(notes.size());
    for (const auto &pw : pending_words(notes))
        for (int i : pw.second) held[size_t(i)] = true;
    for (size_t i = 0; i < notes.size(); ++i)
        if (notes[i].phonemes.empty() && notes[i].word.empty() && !held[i]) notes[i].phonemes = {kDefaultPhoneme};
}

//: CC 7 by CC 11 as it changes, (tick, 0-127), in single precision as
//: VocalWriter has it: CC 7 at 100 is 99
std::vector<std::pair<long, int>> midi_levels(const MidiTrack &track) {
    std::vector<std::tuple<long, int, int>> ev;   // (tick, which, value)
    for (const auto &v : track.volumes) ev.push_back({v.first, 0, v.second});
    for (const auto &v : track.expressions) ev.push_back({v.first, 1, v.second});
    std::stable_sort(ev.begin(), ev.end(), [](const auto &a, const auto &b) { return std::get<0>(a) < std::get<0>(b); });
    std::vector<std::pair<long, int>> out;
    float vol = 1.0f, expr = 1.0f;
    for (const auto &e : ev) {
        (std::get<1>(e) ? expr : vol) = float(std::get<2>(e)) / 127.0f;
        float both = vol * expr;
        out.push_back({std::get<0>(e), int(double(both) * 127.0)});
    }
    return out;
}

//: a level as a track's volume, 127 being 100%
int level_volume(int level) { return int(std::lround(100.0 * level / 127.0)); }

//: The track's volume, CC 7 by CC 11 at its loudest, and into `changes`
//: VocalWriter's volume control against that wherever it is not full.
template <class Beat>
int midi_volume(const MidiTrack &track, Beat beat, std::vector<PlayedChange> &changes) {
    auto levels = midi_levels(track);
    long first = track.notes.empty() ? 0 : track.notes.front().tick;
    for (const MidiNote &n : track.notes) first = std::min(first, n.tick);
    // the level at the first note, and after it the last at each tick
    int at_first = 127;
    long set_at = -1;
    std::vector<std::pair<long, int>> later;
    for (const auto &l : levels) {
        if (l.first <= first) {
            at_first = l.second;
            set_at = l.first;
        } else if (!later.empty() && later.back().first == l.first) {
            later.back().second = l.second;
        } else {
            later.push_back(l);
        }
    }
    int loudest = at_first;
    for (const auto &l : later) loudest = std::max(loudest, l.second);
    auto against = [&](int level) { return loudest ? int(std::lround(127.0 * level / loudest)) : 0; };
    for (const auto &l : later) changes.push_back({beat(l.first), "volume", double(against(l.second))});
    // full is where the engine starts
    if (set_at >= 0 && against(at_first) != 127)
        changes.push_back({beat(set_at), "volume", double(against(at_first))});
    return levels.empty() ? 100 : level_volume(loudest);
}

//: The track's pan at its first note, -100 to 100, and into `changes` each
//: one after it that moves it, as "pan": a controller.
template <class Beat>
int midi_pan(const MidiTrack &track, Beat beat, std::vector<PlayedChange> &changes) {
    auto as_pan = [](int v) { return std::max(-100, std::min(100, int(std::lrint((v - 64) * 100.0 / 63.0)))); };
    long first = track.notes.empty() ? 0 : track.notes.front().tick;
    for (const MidiNote &n : track.notes) first = std::min(first, n.tick);
    auto pans = track.pans;
    std::stable_sort(pans.begin(), pans.end(),
                     [](const std::pair<long, int> &a, const std::pair<long, int> &b) { return a.first < b.first; });
    int pan = 0;
    std::vector<std::pair<long, int>> later;     // the last at each tick
    for (const auto &p : pans) {
        if (p.first <= first) pan = as_pan(p.second);
        else if (!later.empty() && later.back().first == p.first) later.back().second = as_pan(p.second);
        else later.push_back({p.first, as_pan(p.second)});
    }
    int now = pan;
    for (const auto &p : later) {
        if (p.second != now) changes.push_back({beat(p.first), "pan", double(p.second)});
        now = p.second;
    }
    return pan;
}

//: Into `changes`, each program change after the track's first note that
//: moves it, as "voice": the voice `programs` gives the number, where it
//: gives one.
template <class Beat>
void midi_voices(const MidiTrack &track, Beat beat, const std::vector<int> &programs,
                 std::vector<PlayedChange> &changes) {
    long first = track.notes.empty() ? 0 : track.notes.front().tick;
    for (const MidiNote &n : track.notes) first = std::min(first, n.tick);
    auto all = track.programs;
    std::stable_sort(all.begin(), all.end(),
                     [](const std::pair<long, int> &a, const std::pair<long, int> &b) { return a.first < b.first; });
    std::vector<std::pair<long, int>> later;     // the last at each tick
    for (const auto &p : all) {
        if (p.first <= first) continue;
        if (!later.empty() && later.back().first == p.first) later.back().second = p.second;
        else later.push_back(p);
    }
    int now = program_at_start(track);
    for (const auto &p : later) {
        if (p.second == now) continue;
        now = p.second;
        if (now >= 0 && size_t(now) < programs.size() && programs[size_t(now)] >= 0)
            changes.push_back({beat(p.first), "voice", double(programs[size_t(now)])});
    }
}

//: Each "voice" and "pan" change a controller before the first row that
//: starts where it is or after, as lay_out_played puts an instrument's.
void place_controllers(std::vector<Note> &rows, std::vector<PlayedChange> changes) {
    const double eps = 1e-9;
    std::vector<std::vector<Note>> before(rows.size() + 1);
    std::vector<double> starts;
    double at = 0;
    for (const Note &n : rows) {
        starts.push_back(at);
        at += n.beats;
    }
    std::stable_sort(changes.begin(), changes.end(),
                     [](const PlayedChange &a, const PlayedChange &b) { return a.beat < b.beat; });
    for (const PlayedChange &c : changes) {
        if (c.name != "voice" && c.name != "pan") continue;
        size_t k = 0;
        while (k < rows.size() && starts[k] < c.beat - eps) ++k;
        Note change = Note::tempo_change(0);
        if (c.name == "voice") change.voice_id = int(c.value);
        else change.pan = int(c.value);
        before[k].push_back(change);
    }
    std::vector<Note> all;
    for (size_t k = 0; k <= rows.size(); ++k) {
        all.insert(all.end(), before[k].begin(), before[k].end());
        if (k < rows.size()) all.push_back(rows[k]);
    }
    rows = all;
}

//: Each change onto the row taking time it falls in, at its place there, as
//: lay_out_played puts an instrument's.
void place_controls(std::vector<Note> &rows, std::vector<PlayedChange> changes) {
    const double eps = 1e-9;
    std::stable_sort(changes.begin(), changes.end(),
                     [](const PlayedChange &a, const PlayedChange &b) { return a.beat < b.beat; });
    std::vector<size_t> spans;
    std::vector<double> starts;
    double at = 0;
    for (size_t k = 0; k < rows.size(); ++k) {
        if (rows[k].beats > eps) {
            spans.push_back(k);
            starts.push_back(at);
        }
        at += rows[k].beats;
    }
    if (spans.empty()) return;
    for (const PlayedChange &c : changes) {
        auto it = std::upper_bound(starts.begin(), starts.end(), c.beat + eps);
        size_t m = it == starts.begin() ? 0 : size_t(it - starts.begin()) - 1;
        Note &row = rows[spans[m]];
        row.controls[c.name].push_back({std::max(0.0, std::min(1.0, (c.beat - starts[m]) / row.beats)), c.value});
    }
}

//: VocalWriter's controls, as Singing Voice Studio's MIDI export writes
//: them, each kept to its range.
template <class Beat>
void vocalwriter_controls(const MidiTrack &track, Beat beat, std::vector<PlayedChange> &out) {
    for (const auto &c : track.controls) {
        const NoteControl *k = note_control(c.first);
        if (!k) continue;
        for (const auto &v : c.second)
            out.push_back({beat(v.first), c.first, double(std::max(k->lo, std::min(k->hi, v.second)))});
    }
}

//: A part's notes are kept to VocalWriter's 240 ticks a beat, sung or
//: played, as its MIDI import kept them.
const double kPlayedTicks = 240.0;

ImportedPart from_midi(const MidiFile &midi, const MidiTrack &track, const std::string &name,
                       double close_rests, int *closed, const std::vector<int> &programs,
                       bool close_word_rests) {
    double div = midi.division ? midi.division : 480;
    auto curve = bend_curve(track);
    // CC 1, the mod wheel, as it stands: already 0 to 127
    std::vector<std::pair<long, double>> wheel;
    for (const auto &m : track.mods) wheel.push_back({m.first, double(m.second)});
    std::sort(wheel.begin(), wheel.end());
    std::vector<MidiNote> ordered = track.notes;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const MidiNote &a, const MidiNote &b) { return a.tick < b.tick; });
    // Where each note falls rather than how long each one is, cut to a
    // whole tick as ConvertSMF rescales it, and counting from the start of
    // the file, so nothing drifts and a part that comes in late stays late.
    const double step = 1.0 / kPlayedTicks;
    float scale = float(kPlayedTicks) / float(div);
    auto edge = [&](double ticks) { return long(std::floor(double(float(ticks) * scale))); };
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
            long gap = edge(double(ordered[i + 1].tick)) - edge(double(n.tick + span));
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
        }
        long length = std::max(1L, end - at);
        part.notes.push_back(Note(ph, n.pitch, length * step, word,
                                  curve_over(curve, n.tick, n.tick + span, kBendEpsilon),
                                  curve_over(wheel, n.tick, n.tick + span, kModEpsilon)));
        part.notes.back().velocity = std::max(1, std::min(127, n.velocity));
        at += length;
    }
    *closed += close_short_rests(part.notes, close_rests, close_word_rests);
    hold_vowels(part.notes);
    // each note's velocity, as VocalWriter's import kept it; the commonest is
    // the track's
    part.velocity = common_velocity(part.notes);
    // the volume, as an instrument's
    std::vector<PlayedChange> volumes;
    part.volume = midi_volume(track, [&](long tick) { return double(tick) / div; }, volumes);
    vocalwriter_controls(track, [&](long tick) { return double(tick) / div; }, volumes);
    place_controls(part.notes, volumes);
    // the pan, as an instrument's, and the program changes as changes of
    // voice, each where the notes fall on the grid
    std::vector<PlayedChange> changes;
    auto on_grid = [&](long tick) { return double(edge(double(tick))) * step; };
    part.pan = midi_pan(track, on_grid, changes);
    midi_voices(track, on_grid, programs, changes);
    place_controllers(part.notes, changes);
    part.pending = pending_words(part.notes);
    return part;
}

//: the shortest note that import made, in ticks
const double kShortestPlayed = 10.0;

//: The program in force at the track's first note, or -1.
int program_at_start(const MidiTrack &track) {
    long first = track.notes.empty() ? 0 : track.notes.front().tick;
    for (const MidiNote &n : track.notes) first = std::min(first, n.tick);
    auto programs = track.programs;
    std::stable_sort(programs.begin(), programs.end(),
                     [](const std::pair<long, int> &a, const std::pair<long, int> &b) { return a.first < b.first; });
    int program = -1;
    for (const auto &p : programs)
        if (p.first <= first) program = p.second;
    return program;
}

bool drum_track(const MidiTrack &track) { return track.channel == 9; }

//: A track as an instrument plays it, as VocalWriter's import took it in:
//: the notes and chords, the volume (CC 7 by CC 11), the pedal, the bend and
//: its range, and the program changes.
ImportedPart from_midi_played(const MidiFile &midi, const MidiTrack &track, const std::string &name) {
    double div = midi.division ? midi.division : 480;
    // as ConvertSMF rescales: times cut to a whole tick, no note under ten
    float scale = float(kPlayedTicks) / float(div);
    auto ticks = [&](long t) { return std::floor(double(float(t) * scale)); };
    auto beat = [&](long tick) { return ticks(tick) / kPlayedTicks; };
    bool drums = drum_track(track);
    int program = std::max(0, program_at_start(track));
    std::vector<PlayedNote> notes;
    for (const MidiNote &n : track.notes) {
        PlayedNote p;
        p.start = beat(n.tick);
        p.length = std::max(kShortestPlayed, ticks(n.duration)) / kPlayedTicks;
        p.pitch = n.pitch;
        p.velocity = std::max(1, std::min(127, n.velocity));
        notes.push_back(p);
    }
    std::stable_sort(notes.begin(), notes.end(), [](const PlayedNote &a, const PlayedNote &b) {
        return a.start != b.start ? a.start < b.start : a.pitch < b.pitch;
    });
    std::vector<PlayedChange> changes;
    // the volume, CC 7 by CC 11
    int volume = midi_volume(track, beat, changes);
    int pan = midi_pan(track, beat, changes);
    for (const auto &v : track.sustains) changes.push_back({beat(v.first), "sustain", v.second <= 63 ? 0.0 : 127.0});
    for (const auto &r : track.bend_range) changes.push_back({beat(r.first), "bend_range", double(std::max(0, std::min(12, r.second)))});
    for (const auto &b : bend_curve(track)) changes.push_back({beat(b.first), "", round_to(b.second, 4)});
    vocalwriter_controls(track, beat, changes);
    if (!drums) {
        long first = track.notes.empty() ? 0 : track.notes.front().tick;
        for (const MidiNote &n : track.notes) first = std::min(first, n.tick);
        for (const auto &p : track.programs)
            if (p.first > first) changes.push_back({beat(p.first), "voice", double(kGmBase + p.second)});
    }
    std::stable_sort(changes.begin(), changes.end(),
                     [](const PlayedChange &a, const PlayedChange &b) { return a.beat < b.beat; });
    ImportedPart part;
    part.name = name;
    part.played = true;
    part.volume = volume;
    part.notes = lay_out_played(notes, changes);
    part.velocity = common_velocity(part.notes);
    part.voice_id = drums ? kGmDrums : kGmBase + program;
    // the pan, which VocalWriter left out
    part.pan = pan;
    return part;
}

}  // namespace

int MidiPart::voice() const { return drums ? kGmDrums : kGmBase + std::max(0, program); }

std::vector<MidiPart> midi_parts(const std::string &path, int lyrics_on, const ProgramNames &program_names) {
    MidiFile midi = read_midi(path, lyrics_on);
    std::vector<MidiPart> out;
    for (const auto &nt : named_tracks(midi, program_names)) {
        MidiPart p;
        p.name = nt.first;
        p.notes = int(nt.second->notes.size());
        for (const MidiNote &n : nt.second->notes) p.words = p.words || !n.text.empty() || !n.phonemes.empty();
        p.drums = drum_track(*nt.second);
        p.program = std::max(0, program_at_start(*nt.second));
        p.channel = nt.second->channel;
        out.push_back(p);
    }
    return out;
}

LyricChoice lyric_choice(const std::string &path) {
    MidiFile midi = read_midi(path);
    return {midi.lyric_channels, midi.likely_lyric_channel};
}

std::vector<std::string> parts_to_play(const std::vector<MidiPart> &parts) {
    std::vector<std::string> out;
    for (const MidiPart &p : parts)
        if (!p.words) out.push_back(p.name);
    return out;
}

Track imported_track(const ImportedPart &part) {
    Track t;
    t.name = part.name;
    t.notes = part.notes;
    t.velocity = part.velocity;
    t.volume = part.volume;
    t.pan = part.pan;
    if (part.played) t.voice_id = part.voice_id;
    return t;
}

int common_velocity(std::vector<Note> &notes) {
    // a note still waiting for its word has no phonemes yet, but is sung
    auto silent = [](const Note &n) { return n.controller || (!n.phonemes.empty() && n.is_rest()); };
    std::map<int, int> counts;
    for (const Note &n : notes)
        if (!silent(n) && n.velocity > 0) ++counts[n.velocity];
    if (counts.empty()) return kDefaultVelocity;
    int most = counts.begin()->first;
    for (const auto &c : counts)
        if (c.second > counts[most]) most = c.first;
    for (Note &n : notes)
        if (n.velocity == most || silent(n)) n.velocity = 0;
    return most;
}

std::vector<Note> lay_out_played(std::vector<PlayedNote> notes, std::vector<PlayedChange> changes, double rest_gap) {
    const double eps = 1e-9;
    std::stable_sort(notes.begin(), notes.end(), [](const PlayedNote &a, const PlayedNote &b) { return a.start < b.start; });
    std::stable_sort(changes.begin(), changes.end(),
                     [](const PlayedChange &a, const PlayedChange &b) { return a.beat < b.beat; });
    std::vector<Note> rows;
    std::vector<double> starts;      // where each row starts
    auto rest = [&](double from, double beats, int pitch) {
        rows.push_back(Note({kRest}, pitch, beats));
        starts.push_back(from);
    };
    double at = 0.0, free = 0.0;     // where the rows end; when the last note stops
    for (size_t i = 0; i < notes.size();) {
        double b = notes[i].start;
        size_t j = i;
        while (j < notes.size() && notes[j].start - b < eps) ++j;
        if (b > at + eps) rest(at, b - at, notes[i].pitch);
        for (size_t k = i; k < j; ++k) free = std::max(free, b + notes[k].length);
        bool more = j < notes.size();
        double next = more ? notes[j].start : free;
        double step = std::max(0.0, next - b);
        // a silence of `rest_gap` or more is a rest
        if (more && next - free >= rest_gap - eps && free > b + eps) step = free - b;
        for (size_t k = i; k < j; ++k) {
            // a chord's notes but the last take no time
            Note n({kDefaultPhoneme}, notes[k].pitch, k + 1 < j ? 0.0 : step);
            n.velocity = notes[k].velocity;
            if (std::fabs(notes[k].length - n.beats) > eps) n.hold = std::max(notes[k].length, eps);
            rows.push_back(n);
            starts.push_back(b);
        }
        at = std::max(at, b + step);
        i = j;
    }
    if (rows.empty()) return rows;
    // a rest reaching any controls after the last note
    double last = at;
    for (const PlayedChange &c : changes)
        if (c.name != "voice" && c.name != "pan") last = std::max(last, c.beat);
    if (last > at + eps) rest(at, last - at, rows.back().pitch);

    // the controls and the bend go on the rows that take time
    std::vector<size_t> spans;
    std::vector<double> span_starts;
    for (size_t k = 0; k < rows.size(); ++k)
        if (rows[k].beats > eps) {
            spans.push_back(k);
            span_starts.push_back(starts[k]);
        }
    auto span_of = [&](double beat) -> size_t {
        auto it = std::upper_bound(span_starts.begin(), span_starts.end(), beat + eps);
        return it == span_starts.begin() ? 0 : size_t(it - span_starts.begin()) - 1;
    };
    auto position = [&](size_t k, double beat) {
        return std::max(0.0, std::min(1.0, (beat - starts[k]) / rows[k].beats));
    };
    if (!spans.empty()) {
        for (const PlayedChange &c : changes) {
            if (c.name.empty() || c.name == "voice" || c.name == "pan") continue;
            size_t k = spans[span_of(c.beat)];
            // every one, even a repeat, as VocalWriter sends it
            rows[k].controls[c.name].push_back({position(k, c.beat), c.value});
        }
        // the bend, held across the rows and moving in steps
        std::vector<std::pair<double, double>> bends;
        for (const PlayedChange &c : changes)
            if (c.name.empty()) bends.push_back({c.beat, c.value});
        double held = 0.0;
        size_t bi = 0;
        for (size_t m = 0; m < spans.size(); ++m) {
            size_t k = spans[m];
            double a = starts[k], b = a + rows[k].beats;
            bool final_span = m + 1 == spans.size();
            while (bi < bends.size() && bends[bi].first <= a + eps) held = bends[bi++].second;
            Bend inside;
            while (bi < bends.size() && (bends[bi].first < b - eps || final_span)) {
                inside.push_back({position(k, bends[bi].first), bends[bi].second});
                ++bi;
            }
            if (inside.empty() && std::fabs(held) < kBendEpsilon) continue;
            Bend steps;
            if (inside.empty() || inside[0].first > 0.0) steps.push_back({0.0, held});
            double was = held;
            for (const auto &p : inside) {
                if (p.second != was && !steps.empty()) steps.push_back({p.first, was});
                steps.push_back(p);
                was = p.second;
            }
            if (steps.size() == 1 && std::fabs(held) < kBendEpsilon) continue;
            rows[k].bend = steps;
            held = was;
        }
    }
    // each change of instrument or pan a controller before its notes
    std::vector<std::vector<Note>> before(rows.size() + 1);
    for (const PlayedChange &c : changes) {
        if (c.name != "voice" && c.name != "pan") continue;
        size_t k = 0;
        while (k < rows.size() && starts[k] < c.beat - eps) ++k;
        Note change = Note::tempo_change(0);
        if (c.name == "voice") change.voice_id = int(c.value);
        else change.pan = int(c.value);
        before[k].push_back(change);
    }
    std::vector<Note> all;
    for (size_t k = 0; k <= rows.size(); ++k) {
        all.insert(all.end(), before[k].begin(), before[k].end());
        if (k < rows.size()) all.push_back(rows[k]);
    }
    return all;
}

std::vector<std::pair<std::string, int>> midi_tracks(const std::string &path, const ProgramNames &program_names) {
    MidiFile midi = read_midi(path);
    std::vector<std::pair<std::string, int>> out;
    for (const auto &nt : named_tracks(midi, program_names)) out.push_back({nt.first, int(nt.second->notes.size())});
    return out;
}

Imported import_midi(const std::string &path, const std::vector<std::string> &names, double close_rests,
                     const std::vector<std::string> &played, bool remove_velocities, int lyrics_on,
                     const std::vector<int> &programs, bool close_word_rests, bool remove_volumes,
                     const ProgramNames &program_names) {
    MidiFile midi = read_midi(path, lyrics_on);
    auto named = named_tracks(midi, program_names);
    if (named.empty()) throw std::runtime_error("this file has no notes in it");
    Imported out;
    out.bpm = midi_tempo(midi);
    out.tempo_changes = midi_tempo_changes(midi);
    out.sig = midi_sig(midi);
    out.sig_changes = midi_sig_changes(midi);
    for (const std::string &want : names) {
        const MidiTrack *track = nullptr;
        for (const auto &nt : named)
            if (nt.first == want) track = nt.second;
        if (!track) throw std::runtime_error("no track named " + want);
        if (std::find(played.begin(), played.end(), want) == played.end()) {
            out.parts.push_back(
                from_midi(midi, *track, want, close_rests, &out.closed_rests, programs, close_word_rests));
            if (remove_velocities)
                for (Note &n : out.parts.back().notes) n.velocity = 0;
            if (remove_volumes)
                for (Note &n : out.parts.back().notes) n.controls.erase("volume");
            continue;
        }
        out.parts.push_back(from_midi_played(midi, *track, want));
    }
    return out;
}

namespace {

//: One syllable sung over `share` notes, split at its vowel: each note after
//: the first starts on it, as "s t AR", "AR t". With no vowel the notes after
//: the first rest. `plain`: every note but the last holds the vowel's first
//: sound, as "s t AA", "AR t" (first_sound); a vowel opening on a glide is
//: whole on the first note alone, as "f YU", "UW" (after_glide).
std::vector<std::vector<std::string>> split_held(const std::vector<std::string> &syl, int share,
                                                 bool plain) {
    auto vowel = std::find_if(syl.begin(), syl.end(), [](const std::string &x) { return is_nucleus(x); });
    std::vector<std::vector<std::string>> out;
    if (share <= 1 || vowel == syl.end()) {
        out.push_back(syl);
        out.resize(size_t(std::max(share, 1)));
        return out;
    }
    if (plain && after_glide(*vowel) != *vowel) {
        std::string later = after_glide(*vowel);
        out.emplace_back(syl.begin(), vowel + 1);
        for (int j = 2; j < share; ++j) out.push_back({later});
        out.push_back({later});
        out.back().insert(out.back().end(), vowel + 1, syl.end());
        return out;
    }
    std::string held = plain ? first_sound(*vowel) : *vowel;
    out.emplace_back(syl.begin(), vowel);
    out.back().push_back(held);
    for (int j = 2; j < share; ++j) out.push_back({held});
    out.emplace_back(vowel, syl.end());
    return out;
}

//: How many syllables a piece of lyric looks to hold: its runs of vowel
//: letters, a silent e at the end of a word not counted; figures as the
//: words they are said as (number_words).
int vowel_groups(const std::string &text, bool word_end) {
    std::vector<std::string> said = number_words(text);
    if (!said.empty()) {
        int n = 0;
        for (const std::string &w : said) n += vowel_groups(w, true);
        return n;
    }
    std::string t = letters_of(text);
    int n = 0;
    bool in = false;
    for (char c : t) {
        bool v = std::strchr("aeiouyAEIOUY", c) != nullptr;
        if (v && !in) ++n;
        in = v;
    }
    size_t z = t.size();
    if (word_end && n > 1 && z >= 2 && std::tolower((unsigned char)t[z - 1]) == 'e' &&
        !std::strchr("aeiouyAEIOUY", t[z - 2]))
        --n;
    return std::max(n, 1);
}

//: Whether a piece of lyric goes on with the syllable before it rather than
//: starting one: it opens on a vowel letter, or has none ("in-", "ste-", "ad").
//: One with a figure in it starts its own.
bool goes_on(const std::string &text) {
    std::string t = letters_of(text);
    if (std::any_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); })) return false;
    if (t.empty() || std::strchr("aeiouAEIOU", t[0])) return true;
    for (char c : t)
        if (std::strchr("aeiouyAEIOUY", c)) return false;
    return true;
}

//: The phonemes of one word on its notes, from the lyric each note carries.
//: A note that holds the vowel before it sings on that syllable; the others
//: share the syllables out as their lyrics read ("Jitter-", "bug" is two and
//: one; "in-", "ste-", "ad" is one and two) where that adds up, and evenly
//: where it does not, a syllable sung on more of them than one split at its
//: vowel (split_held, `plain` as it has it).
std::vector<std::vector<std::string>> on_notes(std::vector<std::string> phonemes,
                                               const std::vector<std::string> &texts, bool plain) {
    std::vector<size_t> sung;              // the notes that start a syllable
    std::vector<int> held;                 // how many notes hold each one's vowel
    std::string letters;
    std::vector<std::string> pieces;
    for (size_t k = 0; k < texts.size(); ++k) {
        if (!sung.empty() && holds_vowel(letters, texts[k])) {
            ++held.back();
            continue;
        }
        letters += letters_of(texts[k]);
        pieces.push_back(letters_of(texts[k]));
        sung.push_back(k);
        held.push_back(0);
    }
    // a word that only repeats a piece was looked up as the piece
    if (!repeated(pieces).empty()) {
        std::vector<std::string> once = phonemes;
        for (size_t k = 1; k < pieces.size(); ++k) {
            phonemes.push_back(kSyllableBreak);
            phonemes.insert(phonemes.end(), once.begin(), once.end());
        }
    }
    // the dictionary's syllables where it marks them, else as they divide
    std::vector<std::vector<std::string>> syls(1);
    for (const std::string &p : phonemes) {
        if (p != kSyllableBreak) syls.back().push_back(p);
        else if (!syls.back().empty()) syls.emplace_back();
    }
    if (syls.back().empty()) syls.pop_back();
    // The dictionary keeps a consonant between vowels with the syllable
    // before it ("b EY b", "IY"); it is sung opening the next one instead.
    for (size_t k = 0; k + 1 < syls.size(); ++k) {
        auto &a = syls[k], &b = syls[k + 1];
        auto nucleus = [](const std::string &x) { return is_nucleus(x); };
        auto last = std::find_if(a.rbegin(), a.rend(), nucleus);
        auto first = std::find_if(b.begin(), b.end(), nucleus);
        if (last == a.rend() || first == b.end()) continue;
        if (b.front() == "NG") {        // no syllable opens on it ("h AE NG", "IH NG")
            a.push_back("NG");
            b.erase(b.begin());
            continue;
        }
        size_t from = a.size(), stop = size_t(a.rend() - last);
        while (from > stop) {
            std::vector<std::string> onset(a.begin() + long(from - 1), a.end());
            onset.insert(onset.end(), b.begin(), first);
            if (onset.front() == "NG" || !legal_onset(onset)) break;
            --from;
        }
        b.insert(b.begin(), a.begin() + long(from), a.end());
        a.resize(from);
    }
    if (syls.size() < 2) syls = syllabify(syls.empty() ? std::vector<std::string>{} : syls[0]);
    if (syls.empty()) return std::vector<std::vector<std::string>>(texts.size());
    int n = int(syls.size()), m = int(sung.size());
    std::vector<std::vector<std::string>> groups;
    if (m > n) {
        std::vector<int> starts;
        for (int k = 0; k < m; ++k)
            if (k == 0 || !goes_on(texts[sung[size_t(k)]])) starts.push_back(k);
        for (int k = 0; k < n; ++k) {
            int share = int(starts.size()) == n ? (k + 1 < n ? starts[size_t(k + 1)] : m) - starts[size_t(k)]
                                                : m / n + (k < m % n ? 1 : 0);
            auto part = split_held(syls[size_t(k)], share, plain);
            groups.insert(groups.end(), part.begin(), part.end());
        }
    } else {
        std::vector<int> want;
        int sum = 0;
        for (size_t k = 0; k < sung.size(); ++k) {
            want.push_back(vowel_groups(texts[sung[k]], k + 1 == sung.size()));
            sum += want.back();
        }
        // otherwise evenly, as regroup does
        if (sum != n)
            for (int k = 0, at = 0; k < m; ++k) {
                want[size_t(k)] = (n - at) / (m - k);
                at += want[size_t(k)];
            }
        int at = 0;
        for (int w : want) {
            std::vector<std::string> g;
            for (int j = at; j < at + w; ++j) g.insert(g.end(), syls[size_t(j)].begin(), syls[size_t(j)].end());
            groups.push_back(g);
            at += w;
        }
    }
    groups.resize(size_t(m));
    std::vector<std::vector<std::string>> out;
    for (int k = 0; k < m; ++k) {
        auto part = split_held(groups[size_t(k)], 1 + held[size_t(k)], plain);
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

}  // namespace

namespace {

//: The first sound sung after note `after`, rests and controllers passed
//: over: the note's own, or the word still to be looked up that starts
//: there. Empty at the end.
std::string sound_after(const ImportedPart &part, int after,
                        const std::map<std::string, std::vector<std::string>> &found) {
    int i = after + 1;
    // one still to be looked up has no phonemes, which is_rest takes for a rest
    auto passed = [&](const Note &n) { return n.controller || (!n.phonemes.empty() && n.is_rest()); };
    while (i < int(part.notes.size()) && passed(part.notes[size_t(i)])) ++i;
    if (i >= int(part.notes.size())) return "";
    const Note &n = part.notes[size_t(i)];
    std::vector<std::string> sounds = n.phonemes;
    if (sounds.empty())
        for (const auto &pw : part.pending)
            if (!pw.second.empty() && pw.second.front() == i) {
                auto it = found.find(pw.first);
                if (it != found.end()) sounds = it->second;
            }
    for (const std::string &s : sounds)
        if (s != kSyllableBreak) return s;
    return "";
}

}  // namespace

std::vector<std::vector<std::string>> word_on_notes(const std::vector<std::string> &phonemes,
                                                    const std::vector<std::string> &pieces, int count, bool plain) {
    count = std::max(count, 1);
    // the pieces typed, as an import reads a word's lyrics on its notes, a
    // note left for one holding its vowel; the first take any notes over
    int m = int(pieces.size());
    if (m > 1 && m <= count) {
        std::vector<std::string> texts;
        for (int k = 0; k < m; ++k) {
            texts.push_back(pieces[size_t(k)]);
            texts.resize(texts.size() + size_t(count / m + (k < count % m ? 1 : 0) - 1));
        }
        return on_notes(phonemes, texts, plain);
    }
    std::vector<std::string> sounds;
    for (const std::string &p : phonemes)
        if (p != kSyllableBreak) sounds.push_back(p);
    auto syls = syllabify(sounds);
    int n = int(syls.size());
    if (count <= n || n == 0) return regroup(sounds, count);
    // more notes than syllables: each split at its vowel, the first taking
    // a note more where they do not share evenly
    std::vector<std::vector<std::string>> out;
    for (int k = 0; k < n; ++k) {
        auto part = split_held(syls[size_t(k)], count / n + (k < count % n ? 1 : 0), plain);
        out.insert(out.end(), part.begin(), part.end());
    }
    return out;
}

bool in_word(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    if (std::isalnum(c) || c == '\'') return true;
    return (c == '.' || c == ',') && i > 0 && i + 1 < s.size() && std::isdigit((unsigned char)s[i - 1]) &&
           std::isdigit((unsigned char)s[i + 1]);
}

namespace {

const char *const kOnes[20] = {"zero",    "one",     "two",       "three",    "four",
                               "five",    "six",     "seven",     "eight",    "nine",
                               "ten",     "eleven",  "twelve",    "thirteen", "fourteen",
                               "fifteen", "sixteen", "seventeen", "eighteen", "nineteen"};
const char *const kTens[10] = {"", "", "twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};

//: 1 to 999 in words: "one hundred twenty three"
void say_hundreds(int n, std::vector<std::string> &out) {
    if (n >= 100) {
        out.push_back(kOnes[n / 100]);
        out.push_back("hundred");
        n %= 100;
        if (n == 0) return;
    }
    if (n >= 20) {
        out.push_back(kTens[n / 10]);
        if (n % 10) out.push_back(kOnes[n % 10]);
    } else {
        out.push_back(kOnes[n]);
    }
}

//: a whole number in words: "one thousand nine hundred"
void say_number(unsigned long long n, std::vector<std::string> &out) {
    static const char *const scale[] = {"", "thousand", "million", "billion"};
    if (n == 0) {
        out.push_back("zero");
        return;
    }
    std::vector<int> groups;
    for (; n; n /= 1000) groups.push_back(int(n % 1000));
    for (size_t g = groups.size(); g-- > 0;) {
        if (groups[g] == 0) continue;
        say_hundreds(groups[g], out);
        if (g) out.push_back(scale[g]);
    }
}

//: a year in pairs: "nineteen eighty four", "nineteen oh five"
void say_year(int n, std::vector<std::string> &out) {
    say_hundreds(n / 100, out);
    int r = n % 100;
    if (r == 0) {
        out.push_back("hundred");
    } else if (r < 10) {
        out.push_back("oh");
        out.push_back(kOnes[r]);
    } else {
        say_hundreds(r, out);
    }
}

//: a number word as an ordinal: "first", "twelfth", "twentieth"
std::string ordinal(const std::string &w) {
    static const std::map<std::string, std::string> odd = {{"one", "first"},   {"two", "second"},
                                                           {"three", "third"}, {"five", "fifth"},
                                                           {"eight", "eighth"}, {"nine", "ninth"},
                                                           {"twelve", "twelfth"}};
    auto it = odd.find(w);
    if (it != odd.end()) return it->second;
    if (!w.empty() && w.back() == 'y') return w.substr(0, w.size() - 1) + "ieth";
    return w + "th";
}

//: a number word as a plural: "sixties", "sixes", "hundreds"
std::string plural_of(const std::string &w) {
    if (!w.empty() && w.back() == 'y') return w.substr(0, w.size() - 1) + "ies";
    if (!w.empty() && w.back() == 'x') return w + "es";
    return w + "s";
}

}  // namespace

std::vector<std::string> number_words(const std::string &word) {
    auto digit = [&](size_t i) { return i < word.size() && std::isdigit((unsigned char)word[i]); };
    auto letter = [&](size_t i) { return i < word.size() && std::isalpha((unsigned char)word[i]); };
    if (std::none_of(word.begin(), word.end(), [](unsigned char c) { return std::isdigit(c); })) return {};
    std::vector<std::string> out;
    size_t i = 0;
    while (i < word.size()) {
        if (letter(i)) {
            std::string w;
            while (letter(i) || (i < word.size() && word[i] == '\'' && letter(i + 1))) w.push_back(word[i++]);
            out.push_back(w);
            continue;
        }
        if (!digit(i)) {
            ++i;      // anything else only divides
            continue;
        }
        // the figures, a comma before each three ("1,000"), and a decimal part
        std::string figures, decimals;
        bool grouped = false;
        while (digit(i) || (word[i] == ',' && digit(i + 1) && digit(i + 2) && digit(i + 3) && !digit(i + 4))) {
            if (word[i] == ',') grouped = true;
            else figures.push_back(word[i]);
            ++i;
        }
        if (i < word.size() && word[i] == '.' && digit(i + 1))
            for (++i; digit(i); ++i) decimals.push_back(word[i]);
        // "st", "nd", "rd" or "th" after it an ordinal, "s" or "'s" a plural
        size_t j = i + (i < word.size() && word[i] == '\'' ? 1 : 0);
        std::string after;
        while (letter(j)) after.push_back(char(std::tolower((unsigned char)word[j++])));
        bool nth = after == "st" || after == "nd" || after == "rd" || after == "th";
        bool many = after == "s";
        if (nth || many) i = j;
        if ((figures.size() > 1 && figures[0] == '0') || figures.size() > 12) {
            for (char d : figures) out.push_back(d == '0' ? "oh" : kOnes[d - '0']);
        } else {
            unsigned long long n = std::stoull(figures);
            // a year in pairs, but 2005 as two thousand five, 3000 as three thousand
            if (figures.size() == 4 && !grouped && decimals.empty() && !nth && n >= 1100 && n % 1000 >= 10)
                say_year(int(n), out);
            else
                say_number(n, out);
        }
        if (!decimals.empty()) {
            out.push_back("point");
            for (char d : decimals) out.push_back(kOnes[d - '0']);
        }
        if (nth) out.back() = ordinal(out.back());
        else if (many) out.back() = plural_of(out.back());
    }
    return out;
}

std::string TextWord::key() const {
    std::string out;
    for (const std::string &p : pieces) out += p;
    return out;
}

std::vector<TextWord> text_words(const std::string &text) {
    // a tag in square brackets left out, a space in its place; an unclosed
    // bracket is just a character
    std::string kept;
    for (size_t i = 0; i < text.size(); ++i) {
        size_t close = text[i] == '[' ? text.find(']', i) : std::string::npos;
        if (close == std::string::npos) {
            kept.push_back(text[i]);
            continue;
        }
        kept.push_back(' ');
        i = close;
    }
    std::vector<TextWord> out;
    std::istringstream in(kept);
    std::string token;
    while (in >> token) {
        TextWord w;
        w.pieces.emplace_back();
        for (size_t k = 0; k < token.size(); ++k) {
            unsigned char c = (unsigned char)token[k];
            if (c == '-' && !w.pieces.back().empty()) w.pieces.emplace_back();
            else if (in_word(token, k)) w.pieces.back().push_back(char(c));
        }
        if (w.pieces.back().empty()) w.pieces.pop_back();
        if (w.pieces.empty()) continue;
        for (size_t i = 0; i < w.pieces.size(); ++i) {
            w.word += w.pieces[i];
            if (i + 1 < w.pieces.size()) w.pieces[i] += '-';
        }
        out.push_back(w);
    }
    return out;
}

int text_word_notes(const TextWord &w) {
    if (w.pieces.size() > 1) return int(w.pieces.size());
    std::vector<std::string> sounds;
    for (const std::string &p : w.phonemes)
        if (p != kSyllableBreak) sounds.push_back(p);
    return std::max(1, int(syllabify(sounds).size()));
}

void text_looked_up(std::vector<TextWord> &words) {
    for (size_t i = 0; i < words.size(); ++i) {
        TextWord &w = words[i];
        w.notes = text_word_notes(w);
        // "the" before a vowel is "thee"; YU starts with a y
        if (lower(w.word) == "the" && !w.phonemes.empty() && i + 1 < words.size()) {
            const auto &next = words[i + 1].phonemes;
            auto first = std::find_if(next.begin(), next.end(), [](const std::string &p) { return p != kSyllableBreak; });
            if (first != next.end() && is_vowel(*first) && *first != "YU") w.phonemes = {"DH", "IY"};
        }
    }
}

std::vector<int> text_shares(const std::vector<TextWord> &words, int count) {
    int n = int(words.size()), want = 0;
    std::vector<int> share;
    for (const TextWord &w : words) {
        share.push_back(std::max(1, w.notes));
        want += share.back();
    }
    if (count >= want) {
        int over = count - want;
        for (int i = 0; i < n; ++i) share[size_t(i)] += over / n + (i < over % n ? 1 : 0);
    } else {
        int left = std::max(count, 0);
        for (int &s : share) {
            s = std::min(s, left);
            left -= s;
        }
    }
    return share;
}

std::vector<std::pair<std::vector<std::string>, std::string>> text_on_notes(const std::vector<TextWord> &words,
                                                                            const std::vector<int> &shares,
                                                                            bool plain) {
    std::vector<std::pair<std::vector<std::string>, std::string>> out;
    for (size_t i = 0; i < words.size() && i < shares.size() && shares[i] > 0; ++i) {
        const TextWord &w = words[i];
        int k = shares[i];
        auto groups = w.phonemes.empty() ? std::vector<std::vector<std::string>>(size_t(k), {kDefaultPhoneme})
                                         : word_on_notes(w.phonemes, w.pieces, k, plain);
        for (size_t j = 0; j < groups.size(); ++j) out.push_back({groups[j], j == 0 ? w.word : ""});
    }
    return out;
}

int pronounce(std::vector<ImportedPart> &parts,
              const std::map<std::string, std::vector<std::string>> &found, bool plain_held) {
    int got = 0;
    for (ImportedPart &part : parts) {
        for (const auto &pw : part.pending) {
            auto it = found.find(pw.first);
            const std::vector<int> &indices = pw.second;
            // "the" before a vowel is "thee"; YU starts with a y
            if (it != found.end() && lower(pw.first) == "the" && !indices.empty()) {
                std::string next = sound_after(part, indices.back(), found);
                if (is_vowel(next) && next != "YU") {
                    static const std::map<std::string, std::vector<std::string>> thee = {{"the", {"DH", "IY"}}};
                    it = thee.find("the");
                }
            }
            if (it == found.end() || it->second.empty()) {
                for (int i : indices)
                    if (i >= 0 && i < int(part.notes.size()) && part.notes[i].phonemes.empty())
                        part.notes[i].phonemes = {kDefaultPhoneme};
                continue;
            }
            std::vector<std::string> texts;
            for (int i : indices) texts.push_back(i >= 0 && i < int(part.notes.size()) ? part.notes[i].word : "");
            auto groups = on_notes(it->second, texts, plain_held);
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

double Clock::seconds(double beat) const {
    double s = 0, at = 0, now = bpm;
    for (const TempoChange &c : changes) {
        if (c.beat >= beat) break;
        s += (c.beat - at) * 60.0 / std::max(now, 1e-6);
        at = c.beat;
        now = c.bpm;
    }
    return s + (beat - at) * 60.0 / std::max(now, 1e-6);
}

double Clock::beat_at(double secs) const {
    double s = 0, at = 0, now = bpm;
    for (const TempoChange &c : changes) {
        double span = (c.beat - at) * 60.0 / std::max(now, 1e-6);
        if (s + span >= secs) break;
        s += span;
        at = c.beat;
        now = c.bpm;
    }
    return at + (secs - s) * std::max(now, 1e-6) / 60.0;
}

double Clock::bpm_at(double beat) const {
    double now = bpm;
    for (const TempoChange &c : changes)
        if (c.beat <= beat) now = c.bpm;
    return now;
}

std::vector<TempoChange> clean_tempo_changes(std::vector<TempoChange> changes) {
    std::stable_sort(changes.begin(), changes.end(),
                     [](const TempoChange &a, const TempoChange &b) { return a.beat < b.beat; });
    std::vector<TempoChange> out;
    for (TempoChange c : changes) {
        if (!(c.beat > 1e-9) || !std::isfinite(c.beat) || !std::isfinite(c.bpm) || c.bpm <= 0) continue;
        c.bpm = std::max(kMinBpm, std::min(kMaxBpm, std::nearbyint(c.bpm * 1e3) / 1e3));
        c.beat = std::nearbyint(c.beat * 1e6) / 1e6;
        if (!out.empty() && out.back().beat == c.beat) out.back() = c;
        else out.push_back(c);
    }
    return out;
}

Clock Song::clock() const {
    // the lowest track first, so that where two change it at one beat the
    // one higher up is the one kept
    std::vector<TempoChange> all;
    for (auto t = tracks.rbegin(); t != tracks.rend(); ++t) {
        double at = 0;
        for (const Note &n : t->notes) {
            if (n.controller && n.tempo > 0) all.push_back({at, n.tempo});
            at += n.beats;
        }
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const TempoChange &a, const TempoChange &b) { return a.beat < b.beat; });
    Clock c{std::max(kMinBpm, std::min(kMaxBpm, bpm)), {}};
    for (const TempoChange &t : all)
        if (t.beat <= 1e-9) c.bpm = std::max(kMinBpm, std::min(kMaxBpm, t.bpm));
    c.changes = clean_tempo_changes(all);
    return c;
}

//: the bars from `from` to a change at `to` in `sig`, one it cuts short counted
static int bars_between(double from, double to, Sig sig) {
    return std::max(0, int(std::ceil((to - from) / bar_beats(sig) - 1e-6)));
}

std::pair<int, double> Meter::bar_and_beat(double position) const {
    int bars = 0;
    double from = 0;
    Sig now = sig;
    for (const SigChange &c : changes) {
        if (c.beat > position + 1e-9) break;
        bars += bars_between(from, c.beat, now);
        from = c.beat;
        now = c.sig;
    }
    auto bb = svs::bar_and_beat(from > 0 ? std::max(0.0, position - from) : position, now);
    return {bars + bb.first, bb.second};
}

double Meter::position(int bar, double beat) const {
    int first = 1;      // the bar the stretch from `from` begins with
    double from = 0;
    Sig now = sig;
    for (const SigChange &c : changes) {
        int bars = bars_between(from, c.beat, now);
        if (bar < first + bars) break;
        first += bars;
        from = c.beat;
        now = c.sig;
    }
    return from + (bar - first) * bar_beats(now) + (beat - 1) * 4.0 / std::max(now.second, 1);
}

Sig Meter::sig_at(double position) const {
    Sig now = sig;
    for (const SigChange &c : changes)
        if (c.beat <= position + 1e-9) now = c.sig;
    return now;
}

std::vector<SigChange> clean_sig_changes(std::vector<SigChange> changes) {
    std::stable_sort(changes.begin(), changes.end(),
                     [](const SigChange &a, const SigChange &b) { return a.beat < b.beat; });
    std::vector<SigChange> out;
    for (SigChange c : changes) {
        if (!(c.beat > 1e-9) || !std::isfinite(c.beat)) continue;
        if (parse_sig(format_sig(c.sig), {0, 0}) != c.sig) continue;
        c.beat = std::nearbyint(c.beat * 1e6) / 1e6;
        if (!out.empty() && out.back().beat == c.beat) out.back() = c;
        else out.push_back(c);
    }
    return out;
}

Meter Song::meter() const {
    // as clock(): the lowest track first, so the one higher up is kept
    std::vector<SigChange> all;
    for (auto t = tracks.rbegin(); t != tracks.rend(); ++t) {
        double at = 0;
        for (const Note &n : t->notes) {
            if (n.controller && n.sig) all.push_back({at, *n.sig});
            at += n.beats;
        }
    }
    std::stable_sort(all.begin(), all.end(),
                     [](const SigChange &a, const SigChange &b) { return a.beat < b.beat; });
    Meter m{sig, {}};
    for (const SigChange &s : all)
        if (s.beat <= 1e-9 && parse_sig(format_sig(s.sig), {0, 0}) == s.sig) m.sig = s.sig;
    m.changes = clean_sig_changes(all);
    return m;
}

Track tempo_track(const std::vector<TempoChange> &changes, const std::vector<SigChange> &sigs) {
    // both kinds in order, one controller where they fall together
    std::map<double, Note> at_beat;
    for (const TempoChange &c : changes) {
        auto it = at_beat.emplace(c.beat, Note::tempo_change(0)).first;
        it->second.tempo = c.bpm;
    }
    for (const SigChange &c : sigs) {
        auto it = at_beat.emplace(c.beat, Note::tempo_change(0)).first;
        it->second.sig = c.sig;
    }
    Track t;
    t.name = "Tempo";
    double at = 0;
    for (const auto &c : at_beat) {
        if (c.first > at) t.notes.push_back(Note({kRest}, kDefaultPitch, c.first - at));
        t.notes.push_back(c.second);
        at = std::max(at, c.first);
    }
    return t;
}

bool is_tempo_track(const Track &track) {
    if (track.is_backing()) return false;
    bool any = false;
    for (const Note &n : track.notes) {
        if (!n.controller) {
            if (!n.is_rest()) return false;
            continue;
        }
        // a controller that changes anything else belongs to a part
        if (n.voice_id || n.voice_name || n.pan || !n.controls.empty()) return false;
        any = any || n.tempo > 0 || n.sig;
    }
    return any;
}

std::string tempo_map_said(size_t tempos, size_t sigs) {
    auto count = [](size_t n, const char *what) {
        return std::to_string(n) + " " + what + " change" + (n == 1 ? "" : "s");
    };
    std::string said;
    if (tempos) said = count(tempos, "tempo");
    if (sigs) said += (said.empty() ? "" : " and ") + count(sigs, "time signature");
    return said;
}

std::vector<std::pair<double, int>> voice_changes(const Track &track) {
    std::vector<std::pair<double, int>> out;
    double at = 0;
    for (const Note &n : track.notes) {
        if (n.controller && n.voice_id) out.push_back({at, *n.voice_id});
        at += n.beats;
    }
    return out;
}

bool sings(const Track &track) {
    for (const Note &n : track.notes)
        if (!n.controller && !n.is_rest()) return true;
    return false;
}

void steady_tempo(Song &song) {
    for (Track &t : song.tracks) {
        std::vector<Note> kept;
        for (Note n : t.notes) {
            // a controller that changes something else too keeps doing that
            if (n.controller) n.tempo = 0;
            if (!n.controller || n.voice_id || n.pan || n.sig || !n.controls.empty()) kept.push_back(n);
        }
        t.notes = kept;
    }
}

void drop_tempo_map(Song &song) {
    std::vector<bool> tempo(song.tracks.size());
    for (size_t k = 0; k < song.tracks.size(); ++k)
        for (const Note &n : song.tracks[k].notes)
            tempo[k] = tempo[k] || (n.controller && (n.tempo > 0 || n.sig));
    for (Track &t : song.tracks)
        for (Note &n : t.notes) n.sig.reset();
    steady_tempo(song);
    std::vector<Track> kept;
    for (size_t k = 0; k < song.tracks.size(); ++k) {
        const Track &t = song.tracks[k];
        bool more = t.is_backing() || sings(t);
        for (const Note &n : t.notes) more = more || n.controller;
        if (!tempo[k] || more) kept.push_back(t);
    }
    song.tracks = kept;
}

}  // namespace svs
