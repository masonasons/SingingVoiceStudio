#include "core/midiexport.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "core/midifile.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"

namespace svs {

namespace {

//: VocalWriter's own resolution, which its export wrote
const int kTicks = 240;
//: how far apart the steps of a bend or wheel in motion are written, in ticks
const long kSlideStep = 10;

long tick_of(double beats) { return std::lround(beats * kTicks); }

int clamp7(double v) { return int(std::max(0L, std::min(127L, std::lround(v)))); }

//: An event at a tick. At one tick the track's name comes first, then the
//: note-offs, the controls, the program changes, the words, and the
//: note-ons last, as VocalWriter's songs have them.
struct Ev {
    long tick;
    int order;
    std::vector<unsigned char> bytes;
};
enum { kName, kOff, kControl, kProgram, kWords, kOn };

void vlq(std::vector<unsigned char> &b, unsigned long v) {
    unsigned char out[5];
    int n = 0;
    out[n++] = (unsigned char)(v & 0x7F);
    while (v >>= 7) out[n++] = (unsigned char)(0x80 | (v & 0x7F));
    while (n) b.push_back(out[--n]);
}

//: UTF-8 as the one byte a character MIDI text has, as the import reads it
//: back: Latin-1, and what it has no letter for a question mark.
std::string latin1(const std::string &utf8) {
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = (unsigned char)utf8[i];
        if (c < 0x80) {
            out.push_back(char(c));
            ++i;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
            unsigned v = ((c & 0x1Fu) << 6) | ((unsigned char)utf8[i + 1] & 0x3Fu);
            out.push_back(v < 0x100 ? char(v) : '?');
            i += 2;
        } else {
            out.push_back('?');
            ++i;
            while (i < utf8.size() && ((unsigned char)utf8[i] & 0xC0) == 0x80) ++i;
        }
    }
    return out;
}

std::vector<unsigned char> meta(int type, const std::string &text) {
    std::vector<unsigned char> b{0xFF, (unsigned char)type};
    vlq(b, text.size());
    b.insert(b.end(), text.begin(), text.end());
    return b;
}

std::vector<unsigned char> chunk(std::vector<Ev> ev) {
    std::stable_sort(ev.begin(), ev.end(),
                     [](const Ev &a, const Ev &b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });
    std::vector<unsigned char> body;
    long at = 0;
    for (const Ev &e : ev) {
        vlq(body, (unsigned long)std::max(0L, e.tick - at));
        at = std::max(at, e.tick);
        body.insert(body.end(), e.bytes.begin(), e.bytes.end());
    }
    body.insert(body.end(), {0x00, 0xFF, 0x2F, 0x00});
    std::vector<unsigned char> out{'M', 'T', 'r', 'k'};
    for (int s = 24; s >= 0; s -= 8) out.push_back((unsigned char)(body.size() >> s));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

//: VocalWriter's spelling of a phoneme in its exports, its g_Opcode_To_ASCII
//: table: the engine's own name but for these three.
std::string vw_spelling(const std::string &symbol) {
    if (symbol == kRest) return "_";
    if (symbol == "O") return "OH";
    if (symbol == "Q") return "DX";
    return symbol;
}

//: The program change that reaches a voice, or -1: a GM instrument's own, or
//: the first program VocalWriter's bank maps to one of its voices.
int program_of(int voice, const std::map<int, int> &program_map) {
    if (voice == kGmDrums) return -1;
    if (is_gm_voice(voice)) return voice - kGmBase;
    if (is_vocalwriter_voice(voice))
        for (const auto &pv : program_map)
            if (pv.second == voice) return pv.first;
    return -1;
}

//: The voice a track's first note is sung or played with.
int first_voice(const Track &t, const std::map<int, int> &program_map) {
    int v = track_voice(t, program_map);
    for (const Note &n : t.notes) {
        if (!n.controller && !n.is_rest()) break;
        if (n.controller && n.voice_id) v = *n.voice_id;
    }
    return v;
}

//: A curve (a bend in semitones, or the wheel), every point at its tick and
//: a step every few ticks where it slides, each value as `raw` makes it.
template <class Raw>
void curve_events(std::vector<Ev> &ev, const std::vector<BendPoint> &points, Raw raw) {
    int last = -100000;
    auto put = [&](long tick, double value) {
        std::vector<unsigned char> b = raw(tick, value);
        int key = (b[1] << 7) | (b.size() > 2 ? b[2] : 0);
        if (key == last) return;
        last = key;
        ev.push_back({tick, kControl, b});
    };
    for (size_t i = 0; i < points.size(); ++i) {
        const BendPoint &p = points[i];
        long a = tick_of(p.beat);
        put(a, p.semitones);
        if (!p.slides || i + 1 >= points.size()) continue;
        const BendPoint &q = points[i + 1];
        long b = tick_of(q.beat);
        for (long t = a + kSlideStep; t < b; t += kSlideStep)
            put(t, p.semitones + (q.semitones - p.semitones) * double(t - a) / double(b - a));
    }
}

std::vector<unsigned char> one_track(const Track &t, int ch, const std::map<int, int> &program_map) {
    std::vector<Ev> ev;
    ev.push_back({0, kName, meta(0x03, latin1(t.name))});
    auto cc = [&](long tick, int number, int value) {
        ev.push_back({tick, kControl, {(unsigned char)(0xB0 | ch), (unsigned char)number, (unsigned char)clamp7(value)}});
    };
    // VocalWriter's other controls, on the controllers midifile.h names
    auto control = [&](long tick, const std::string &key, double value) {
        long v = std::max(0L, std::min(16383L, std::lround(value) + 8192));
        if (key == "detune") {
            cc(tick, 101, 0);
            cc(tick, 100, 1);
            cc(tick, 6, int(v >> 7));
            cc(tick, 38, int(v & 127));
        } else if (key == "chorus") {
            cc(tick, kChorusCC, int(v >> 7));
            cc(tick, kChorusCC + 32, int(v & 127));
        } else {
            for (const MidiControl &m : kMidiControls)
                if (key == m.key) cc(tick, m.cc, value);
        }
    };
    auto program = [&](long tick, int voice) {
        int p = program_of(voice, program_map);
        if (p >= 0) ev.push_back({tick, kProgram, {(unsigned char)(0xC0 | ch), (unsigned char)p}});
    };
    // the voice at the start, unless a voice change before the first note
    // chooses it, as one from a VocalWriter song does: its program goes
    // there instead, where VocalWriter had it
    int voice = track_voice(t, program_map);
    for (const Note &n : t.notes) {
        if (!n.controller && !n.is_rest()) break;
        if (n.controller && n.voice_id) {
            voice = -1;
            break;
        }
    }
    if (voice >= 0) program(0, voice);
    if (t.pan) cc(0, 10, 64 + t.pan * 63 / 100.0);

    // the notes, and what the controllers and the notes' own controls change
    std::vector<std::pair<long, int>> ranges;       // (tick, semitones)
    double at = 0;
    bool sung_before = false;
    for (const Note &n : t.notes) {
        long a = tick_of(at), b = tick_of(at + n.beats);
        if (n.controller) {
            if (n.voice_id && *n.voice_id != voice) {
                voice = *n.voice_id;
                program(a, voice);
            }
            if (n.pan) cc(a, 10, 64 + *n.pan * 63 / 100.0);
        }
        for (const auto &c : n.controls)
            for (const auto &pt : c.second) {
                long tk = a + std::lround(pt.first * double(b - a));
                if (c.first == "volume") cc(tk, 7, pt.second);
                else if (c.first == "sustain") cc(tk, 64, pt.second);
                else if (c.first == "bend_range") ranges.push_back({tk, std::max(0, std::min(24, int(std::lround(pt.second))))});
                else control(tk, c.first, pt.second);
            }
        if (n.controller) continue;
        at += n.beats;
        if (n.phonemes.empty() || n.is_rest()) {
            sung_before = false;
            continue;
        }
        // scaled by the track's volume in 16.16 fixed point, as VocalWriter did
        long scale = long(std::max(0, t.volume)) * 65536 / 100;
        int vel = int(std::max(1L, std::min(127L, ((n.velocity ? n.velocity : t.velocity) * scale) >> 16)));
        unsigned char on[3] = {(unsigned char)(0x90 | ch), (unsigned char)std::max(0, std::min(127, n.pitch)),
                               (unsigned char)vel};
        if (is_gm_voice(voice)) {
            // an instrument holds a note as long as it sounds, a chord's too
            long end = n.hold > 0 ? tick_of(at - n.beats + n.hold) : b;
            ev.push_back({a, kOn, {on, on + 3}});
            ev.push_back({std::max(end, a + 1), kOff, {(unsigned char)(0x80 | ch), on[1], 0}});
            continue;
        }
        if (b <= a) continue;
        // The word as Text, the phonemes as a Lyric: VocalWriter's "=" for a
        // note that holds on to the syllable before it, and a note with no
        // word at all named after what it sings.
        std::string lyric, plain;
        for (const std::string &p : n.phonemes) {
            if (phoneme_code(p) < 0) continue;
            lyric += vw_spelling(p);
            if (p == kRest) continue;
            for (char c : p) plain.push_back(char(std::tolower((unsigned char)c)));
        }
        std::string text = !n.word.empty() ? latin1(n.word) : sung_before ? std::string("=") : plain;
        ev.push_back({a, kWords, meta(0x01, text)});
        ev.push_back({a, kWords, meta(0x05, lyric)});
        ev.push_back({a, kOn, {on, on + 3}});
        ev.push_back({b, kOff, {(unsigned char)(0x80 | ch), on[1], 0}});
        sung_before = true;
    }
    // the controllers' bend ranges too, then the bends in the range in force
    // (RPN 0, as VocalWriter wrote it); a song that never sets one is given
    // the narrowest that holds its bends, VocalWriter's two semitones if it can
    std::vector<BendPoint> bends = timeline(t.notes);
    std::stable_sort(ranges.begin(), ranges.end());
    if (ranges.empty() && !bends.empty()) {
        double widest = 0;
        for (const BendPoint &p : bends) widest = std::max(widest, std::fabs(p.semitones));
        int r = widest <= 2.0 + 1e-9 ? 2 : int(std::min(24.0, std::ceil(widest - 1e-9)));
        if (r != 2) ranges.push_back({0, r});
    }
    for (const auto &r : ranges) {
        cc(r.first, 101, 0);
        cc(r.first, 100, 0);
        cc(r.first, 6, r.second);
        cc(r.first, 38, 0);
    }
    curve_events(ev, bends, [&](long tick, double semitones) {
        int range = 2;
        for (const auto &r : ranges)
            if (r.first <= tick) range = r.second;
        // a range of 0 bends nothing, so the wheel stays in the middle
        long v = range > 0 ? std::lround(semitones / range * 8192) + 8192 : 8192;
        v = std::max(0L, std::min(16383L, v));
        return std::vector<unsigned char>{(unsigned char)(0xE0 | ch), (unsigned char)(v & 127), (unsigned char)(v >> 7)};
    });
    curve_events(ev, mod_timeline(t.notes), [&](long, double wheel) {
        return std::vector<unsigned char>{(unsigned char)(0xB0 | ch), 1, (unsigned char)clamp7(wheel)};
    });
    return chunk(ev);
}

}  // namespace

std::vector<const Track *> midi_export_tracks(const Song &song) {
    std::vector<const Track *> out;
    for (const Track &t : song.tracks)
        if (!t.is_backing() && sings(t)) out.push_back(&t);
    return out;
}

std::vector<int> default_midi_channels(const std::vector<const Track *> &tracks,
                                       const std::map<int, int> &program_map) {
    std::vector<int> out;
    int next = 0;
    for (const Track *t : tracks) {
        if (first_voice(*t, program_map) == kGmDrums) {
            out.push_back(9);
            continue;
        }
        out.push_back(next);
        next = next + 1 == 9 ? 10 : next + 1 > 15 ? 0 : next + 1;
    }
    return out;
}

void export_midi(const std::string &path, const Song &song, const std::vector<MidiOut> &tracks,
                 const std::map<int, int> &program_map) {
    // the first track: the time signature and its changes, then the tempo and its
    std::vector<Ev> conductor;
    auto time_sig = [&](long tick, Sig sig) {
        int den = 0;
        while ((1 << den) < sig.second && den < 7) ++den;
        conductor.push_back({tick, kControl, {0xFF, 0x58, 0x04, (unsigned char)sig.first, (unsigned char)den, 0x20, 0x08}});
    };
    Meter meter = song.meter();
    time_sig(0, meter.sig);
    for (const SigChange &c : meter.changes) time_sig(tick_of(c.beat), c.sig);
    Clock clock = song.clock();
    auto tempo = [&](long tick, double bpm) {
        // rounded down, as VocalWriter divided
        long us = long(60e6 / std::max(bpm, 1e-6));
        conductor.push_back({tick, kControl, {0xFF, 0x51, 0x03, (unsigned char)(us >> 16), (unsigned char)(us >> 8), (unsigned char)us}});
    };
    tempo(0, clock.bpm);
    for (const TempoChange &c : clock.changes) tempo(tick_of(c.beat), c.bpm);

    std::vector<unsigned char> file{'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1};
    size_t count = tracks.size() + 1;
    file.push_back((unsigned char)(count >> 8));
    file.push_back((unsigned char)count);
    file.push_back((unsigned char)(kTicks >> 8));
    file.push_back((unsigned char)kTicks);
    std::vector<unsigned char> c = chunk(conductor);
    file.insert(file.end(), c.begin(), c.end());
    for (const MidiOut &m : tracks) {
        std::vector<unsigned char> t = one_track(*m.track, std::max(0, std::min(15, m.channel)), program_map);
        file.insert(file.end(), t.begin(), t.end());
    }
#ifdef _WIN32
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
#else
    FILE *f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) throw std::runtime_error("cannot write " + path);
    bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
    ok = fclose(f) == 0 && ok;
    if (!ok) throw std::runtime_error("cannot write " + path);
}

}  // namespace svs
