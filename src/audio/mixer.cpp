#include "audio/mixer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#endif

#include "audio/decode.h"
#include "audio/encode.h"
#include "core/paths.h"
#include "core/phonology.h"
#include "voices/registry.h"
#include "voices/vocalwriter.h"

namespace svs {

namespace {

const double kPi = 3.14159265358979323846;
//: how long after the last note the file runs on, so its decay is not clipped
const double kTailSeconds = 0.4;
//: How much of what the notes from the cursor on sing before it is kept
//: when playing: VocalWriter's sequencer sings "s t" ahead of the beat, and
//: a note heard alone lost it. An export, from the song's start, keeps none.
const double kPreRoll = 0.5;
//: quieter than this, the start of a mix is silence
const float kSilent = 1e-5f;
//: how often a bend in motion is sent: one VocalWriter frame, about 5 ms
const double kBendStep = 0.005;
//: The most of a note that may be given to the note in front of it so that a
//: note opening with consonants can start early enough for its vowel to land
//: on the beat. Take more than half and what is left is a grace note.
const double kAnticipateMost = 0.5;

long round_half_even(double x) { return long(std::nearbyint(x)); }

struct Pt {
    double t, v;
    bool slides;
};

//: Fill in between the written points so a bend moves rather than jumps --
//: only where the point says it slides into the next one. Each value says
//: whether it was written (and is not the first half of a step).
struct Glided {
    double t, v;
    bool written;
};
std::vector<Glided> glide(const std::vector<Pt> &pts) {
    std::vector<Glided> out;
    for (size_t i = 0; i < pts.size(); ++i) {
        const Pt &a = pts[i];
        bool step = i + 1 < pts.size() && pts[i + 1].t == a.t;
        out.push_back({a.t, a.v, !step});
        if (i + 1 == pts.size()) break;
        const Pt &b = pts[i + 1];
        if (!a.slides || b.v == a.v || b.t - a.t <= kBendStep) continue;
        int n = int((b.t - a.t) / kBendStep);
        for (int k = 1; k < n; ++k) {
            double f = k * kBendStep / (b.t - a.t);
            out.push_back({a.t + k * kBendStep, a.v + (b.v - a.v) * f, false});
        }
    }
    return out;
}

int wheel_step(double v) { return int(std::lrint(v)); }

int bend_raw(double semitones) {
    long v = round_half_even(semitones / 12.0 * 8192);
    return int(std::max(-8192L, std::min(8191L, v)));
}

//: Bend values for one phrase, in seconds from its own start; whatever was in
//: force when the phrase began is carried in ahead of it. `step` is the
//: smallest change worth sending: the engine's own bend resolution for a bend,
//: a whole step for the mod wheel.
std::vector<BendEvent> bend_events(const std::vector<Pt> &points, double t0, double t1,
                                   int (*step)(double) = bend_raw) {
    std::vector<BendEvent> ev;
    if (points.empty()) return ev;
    bool have_before = false;
    double before = 0;
    for (const Pt &p : points)
        if (p.t < t0) {
            before = p.v;
            have_before = true;
        }
    if (have_before) ev.push_back({-1.0, before});
    bool have_last = false;
    int last = 0;
    for (const Glided &g : glide(points)) {
        if (t0 <= g.t && g.t <= t1) {
            int raw = step(g.v);
            // a value filled in that the engine would not hear is work; a
            // written one is sent, as VocalWriter sends each of its own
            if (!have_last || raw != last || g.written) {
                ev.push_back({g.t - t0, g.v});
                last = raw;
                have_last = true;
            }
        }
    }
    return ev;
}

//: The bends as the notes are sung once their consonants are moved ahead of
//: the beat (anticipate): from where a note now starts to where it is
//: written, the bend it starts on rather than the one the note before ends
//: on, which on the new note's pitch would be heard as a jump. `written`:
//: each note's start in seconds before it was moved; the notes start at
//: `from_s`.
std::vector<Pt> bends_ahead(const std::vector<Pt> &bends, const std::vector<double> &written,
                            const std::vector<SungNote> &notes, double from_s, double spb) {
    std::vector<Pt> out = bends;
    double at = from_s;
    for (size_t k = 0; k < written.size() && k < notes.size(); ++k) {
        double sung = at;
        at += notes[k].beats * spb;
        if (k == 0 || sung >= written[k] - 1e-9) continue;
        double was = 0, starts = 0;
        for (const Pt &p : out) {
            if (p.t < sung - 1e-9) was = p.v;
            if (p.t <= written[k] + 1e-9) starts = p.v;
        }
        if (std::fabs(starts - was) < 1e-9) continue;
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const Pt &p) { return p.t >= sung - 1e-9 && p.t < written[k] - 1e-9; }),
                  out.end());
        auto it = std::lower_bound(out.begin(), out.end(), sung, [](const Pt &p, double t) { return p.t < t; });
        out.insert(it, {sung, starts, false});
    }
    return out;
}

std::pair<double, double> pan_gains(double pan) {
    double theta = (std::max(-1.0, std::min(1.0, pan)) + 1.0) * (kPi / 4.0);
    return {std::sqrt(2.0) * std::cos(theta), std::sqrt(2.0) * std::sin(theta)};
}

//: whether a part is heard anywhere but in the middle
bool panned(const PartRequest &t) {
    if (std::fabs(t.pan) > 1e-6) return true;
    for (const auto &p : t.pans)
        if (std::fabs(p.second) > 1e-6) return true;
    return false;
}

//: A part's pan and gain from a frame on
struct MixStep {
    size_t frame;
    double pan, gain;
};

//: The part's pan from each frame on, its own and then each controller's,
//: and the volume control as a gain wherever its engine does not follow it
//: itself; counted from `lead` frames before `start` (beats).
std::vector<MixStep> mix_steps(const PartRequest &t, const Clock &clock, double start, long lead) {
    struct Event {
        double beat;
        int kind;      // 0 pan, 1 volume, 2 voice
        double value;
    };
    std::vector<Event> ev;
    for (const auto &p : t.pans) ev.push_back({p.first, 0, p.second});
    for (const ControlAt &c : t.controls)
        if (c.name == "volume") ev.push_back({c.beat, 1, c.value});
    double at = 0;
    for (const auto &n : t.notes) {
        auto it = n.controls.find("volume");
        if (it != n.controls.end())
            for (const auto &p : it->second) ev.push_back({at + p.first * n.beats, 1, p.second});
        at += n.beats;
    }
    for (const auto &v : t.voices) ev.push_back({v.first, 2, double(v.second)});
    std::stable_sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) { return a.beat < b.beat; });
    double pan = t.pan, volume = 127;
    int voice = t.voice_id;
    auto gain = [&] { return sings_volume(voice) ? 1.0 : volume / 127.0; };
    std::vector<MixStep> out{{0, pan, gain()}};
    double from = clock.seconds(start);
    for (const Event &e : ev) {
        if (e.kind == 0) pan = e.value;
        else if (e.kind == 1) volume = e.value;
        else voice = int(e.value);
        long long f = std::llround((clock.seconds(e.beat) - from) * kSampleRate) + lead;
        if (f <= 0) out.front() = {0, pan, gain()};
        else if (size_t(f) == out.back().frame) out.back() = {size_t(f), pan, gain()};
        else out.push_back({size_t(f), pan, gain()});
    }
    return out;
}

Reverb clean_reverb(const Reverb &r) {
    return {std::max(0, std::min(100, r.first)), std::max(0, std::min(100, r.second))};
}

bool is_rest(const std::vector<std::string> &ph) {
    for (const std::string &p : ph)
        if (p != kRest) return false;
    return true;
}

//: How long a note's opening consonants last, in beats.
//: Only what comes before the phoneme the note is held on counts. A note with
//: no vowel used to count as all onset -- a hummed m was moved, whole, half a
//: beat ahead of where it was written, its bend left behind at the beat.
double onset_beats(const SungNote &note, double bpm) {
    int held = held_phoneme(note.phonemes);
    double ms = 0;
    for (int i = 0; i < held && size_t(i) < note.durations_ms.size(); ++i) ms += note.durations_ms[size_t(i)];
    return ms * bpm / 60000.0;
}

//: Move every note's consonants in front of its beat, taking the time from
//: the note before. Returns how much earlier the phrase now starts.
double anticipate(std::vector<SungNote> &notes, double bpm, double consonants, double room) {
    auto retime = [&](SungNote &n, double beats) {
        n.beats = beats;
        n.durations_ms = syllable_lengths(n.phonemes, beats * 60000.0 / bpm, consonants);
    };
    double lead = std::max(0.0, std::min(onset_beats(notes[0], bpm), room));
    if (lead > 0) retime(notes[0], notes[0].beats + lead);
    for (size_t i = 1; i < notes.size(); ++i) {
        double give = std::min(onset_beats(notes[i], bpm), kAnticipateMost * notes[i - 1].beats);
        if (give <= 0) continue;
        retime(notes[i - 1], notes[i - 1].beats - give);
        retime(notes[i], notes[i].beats + give);
    }
    return lead;
}

std::string hash_key(const std::string &s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char buf[40];
    std::snprintf(buf, sizeof buf, "%016llx%08zx", (unsigned long long)h, s.size());
    return buf;
}

//: `ys` (`ych` channels) added into `out` (`och`) from frame `at`, leaving
//: out its first `skip` frames; `out` grows to hold it.
void lay(std::vector<float> &out, int och, long at, const std::vector<float> &ys, int ych, size_t skip = 0) {
    if (at < 0) {
        skip += size_t(-at);
        at = 0;
    }
    size_t frames = ys.size() / size_t(ych);
    if (skip >= frames) return;
    size_t len = frames - skip, need = (size_t(at) + len) * size_t(och);
    if (out.size() < need) out.resize(need, 0.0f);
    float *o = out.data() + size_t(at) * size_t(och);
    const float *y = ys.data() + skip * size_t(ych);
    for (size_t k = 0; k < len; ++k) {
        if (och == ych) {
            for (int c = 0; c < och; ++c) o[k * size_t(och) + size_t(c)] += y[k * size_t(ych) + size_t(c)];
        } else if (och == 2) {
            o[2 * k] += y[k];
            o[2 * k + 1] += y[k];
        } else {
            o[k] += 0.5f * (y[2 * k] + y[2 * k + 1]);
        }
    }
}

//: whether an instrument plays rather than sings with this voice
bool played(int voice) {
    Family *f = Registry::get().family_of(voice);
    return f && f->plays();
}

//: A note's controls, written into a key in full.
void controls_key(std::string &k, const NoteControls &controls) {
    char buf[64];
    for (const auto &c : controls) {
        k += "C" + c.first + ":";
        for (const auto &p : c.second) {
            std::snprintf(buf, sizeof buf, "%.17g,%.17g,", p.first, p.second);
            k += buf;
        }
    }
}

std::string request_key(const RenderRequest &r) {
    std::string k;
    char buf[64];
    auto num = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.17g,", v);
        k += buf;
    };
    num(r.bpm);
    num(r.end);
    for (const TempoChange &c : r.tempo_changes) {
        k += "T";
        num(c.beat);
        num(c.bpm);
    }
    num(r.consonants);
    num(r.start);
    num(r.reverb.first);
    num(r.reverb.second);
    num(r.anticipate);
    // VocalWriter's voices sound otherwise through its own play path
    num(vocalwriter_own_path());
    for (const PartRequest &t : r.tracks) {
        k += "|T";
        num(t.voice_id);
        num(t.volume);
        num(t.pan);
        num(t.velocity);
        num(t.channel);
        for (int VoiceStyle::*f : {&VoiceStyle::color, &VoiceStyle::vibrato, &VoiceStyle::vibrato_rate,
                                   &VoiceStyle::chorus, &VoiceStyle::breath, &VoiceStyle::detune,
                                   &VoiceStyle::portamento})
            num(t.voice.*f);
        if (t.reverb) {
            num(t.reverb->first);
            num(t.reverb->second);
        } else {
            k += "-,";
        }
        if (t.consonants) num(*t.consonants);
        else k += "-,";
        for (const auto &v : t.voices) {
            k += "V";
            num(v.first);
            num(v.second);
        }
        for (const ControlAt &c : t.controls) {
            k += "K" + c.name + ":";
            num(c.beat);
            num(c.value);
        }
        for (const auto &p : t.pans) {
            k += "P";
            num(p.first);
            num(p.second);
        }
        for (const auto &n : t.notes) {
            k += "N";
            num(n.pitch);
            num(n.beats);
            for (const std::string &p : n.phonemes) k += p + " ";
            k += "W" + n.word + "|";
            num(n.velocity);
            controls_key(k, n.controls);
            num(n.hold);
        }
        for (const BendPoint &b : t.bends) {
            k += "B";
            num(b.beat);
            num(b.semitones);
            num(b.slides);
        }
        for (const BendPoint &b : t.mods) {
            k += "M";
            num(b.beat);
            num(b.semitones);
            num(b.slides);
        }
    }
    // a backing track's file by what it is now: changed on disk, it is
    // read again
    for (const BackingRequest &b : r.backings) {
        k += "|A" + b.path + "|";
        num(double(file_size(b.path)));
        num(double(file_time(b.path)));
        num(b.volume);
        num(b.pan);
        num(b.start);
    }
    return hash_key(k);
}

std::vector<float> click(double hz, double level) {
    int n = int(kSampleRate * 0.035);
    std::vector<float> out(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        double t = i / double(kSampleRate);
        out[static_cast<size_t>(i)] = float(level * std::sin(2 * kPi * hz * t) * std::exp(-t * 45.0));
    }
    return out;
}

//: Which of the song's tracks this is: itself, or the one it was copied
//: from, as a track being exported or the notes played on their own are.
const Track *in_song(const Song &song, const Track *t) {
    for (const Track &o : song.tracks)
        if (&o == t) return &o;
    for (const Track &o : song.tracks)
        if (o == *t) return &o;
    for (const Track &o : song.tracks)
        if (o.name == t->name) return &o;
    return nullptr;
}

}  // namespace

void with_metronome(std::vector<float> &y, int channels, const Clock &clock, const Meter &meter,
                    double start, double lead) {
    const double kClickHz = 1000.0, kAccentHz = 1600.0, kLevel = 0.22;
    std::vector<float> plain = click(kClickHz, kLevel), accent = click(kAccentHz, kLevel * 1.4);
    size_t n = y.size() / size_t(channels);
    std::vector<float> ticks(n, 0.0f);
    // the bars counted in whole beats from the first beat of each signature
    auto bar_line = [&meter](long k) {
        double from = 0;
        Sig sig = meter.sig;
        for (const SigChange &c : meter.changes)
            if (c.beat <= double(k) + 1e-9) {
                from = c.beat;
                sig = c.sig;
            }
        long beats = std::max(1L, round_half_even(bar_beats(sig)));   // keeps 3/4, 6/8 and 7/8
        return (k - long(std::ceil(from - 1e-9))) % beats == 0;
    };
    long k = long(std::ceil(start - 1e-9));
    while (true) {
        long at = round_half_even((clock.seconds(double(k)) - clock.seconds(start) + lead) * kSampleRate);
        if (at >= long(n)) break;
        const std::vector<float> &tick = bar_line(k) ? accent : plain;
        for (size_t i = 0; i < tick.size(); ++i) {
            long j = at + long(i);
            if (j >= 0 && j < long(n)) ticks[size_t(j)] += tick[i];
        }
        ++k;
    }
    float peak = 0;
    for (size_t i = 0; i < n; ++i)
        for (int c = 0; c < channels; ++c) {
            float &s = y[i * size_t(channels) + size_t(c)];
            s += ticks[i];
            peak = std::max(peak, std::fabs(s));
        }
    if (peak > 1.0f)                       // the ticks must not push it into clip
        for (float &s : y) s /= peak;
}

//: The voice a track's first note is sung with: its own, or the last
//: controller's before it.
static int first_voice(const Track &t, const std::map<int, int> &program_map) {
    int v = track_voice(t, program_map);
    for (const Note &n : t.notes) {
        if (!n.controller && !n.is_rest()) break;
        if (n.controller && n.voice_id) v = *n.voice_id;
    }
    return v;
}

RenderRequest make_request(const Song &song, const std::vector<const Track *> &parts, double start,
                           const std::map<int, int> &program_map) {
    RenderRequest r;
    // the tempo from every track, heard or not: a tempo change is the song's
    Clock clock = song.clock();
    r.bpm = clock.bpm;
    r.tempo_changes = clock.changes;
    r.end = song.end;
    r.consonants = song.consonants;
    r.start = std::nearbyint(start * 1e6) / 1e6;
    r.reverb = song.reverb;
    r.anticipate = song.anticipate;
    for (const Track *t : parts) {
        if (t->is_backing()) {
            // on the beat it starts at, in seconds as the file is played
            r.backings.push_back({t->audio, t->volume / 100.0, t->pan / 100.0,
                                  std::nearbyint(clock.seconds(t->audio_beat) * 1e3) / 1e3, t->name});
            continue;
        }
        // a track of rests and controllers, as the tempo is kept on, sings nothing
        if (!sings(*t)) continue;
        PartRequest p;
        // the voice it starts with is the one in force at its first note;
        // every change is kept, where it stands
        p.voice_id = first_voice(*t, program_map);
        p.voices = voice_changes(*t);
        // and the controls and pans the controllers set, where they stand
        double at = 0;
        for (const Note &n : t->notes) {
            if (n.controller) {
                for (const auto &c : n.controls)
                    if (!c.second.empty()) p.controls.push_back({at, c.first, c.second.front().second});
                if (n.pan) p.pans.push_back({at, *n.pan / 100.0});
            }
            at += n.beats;
        }
        p.volume = t->volume / 100.0;
        p.pan = t->pan / 100.0;
        p.velocity = t->velocity;
        // a part with no voice controls of its own is sung with the song's
        p.voice = t->voice ? *t->voice : song.voice;
        p.reverb = t->reverb;
        p.consonants = t->consonants;
        for (const Note &n : t->notes)
            if (!n.controller)
                p.notes.push_back({n.pitch, n.beats,
                                   n.phonemes.empty() ? std::vector<std::string>{kRest} : n.phonemes, n.word,
                                   n.velocity, n.controls, n.hold});
        // a VocalWriter song gives every vocal track a speech channel, in
        // order, whether it is heard or not
        Family *f = Registry::get().family_of(p.voice_id);
        const Track *self = in_song(song, t);
        for (const Track &o : song.tracks) {
            if (&o == self) break;
            if (!o.is_backing() && sings(o) && Registry::get().family_of(first_voice(o, program_map)) == f)
                ++p.channel;
        }
        for (BendPoint b : timeline(t->notes)) {
            b.beat = std::nearbyint(b.beat * 1e5) / 1e5;
            b.semitones = std::nearbyint(b.semitones * 1e4) / 1e4;
            p.bends.push_back(b);
        }
        for (BendPoint b : mod_timeline(t->notes)) {
            b.beat = std::nearbyint(b.beat * 1e5) / 1e5;
            b.semitones = std::nearbyint(b.semitones * 1e4) / 1e4;
            p.mods.push_back(b);
        }
        r.tracks.push_back(p);
    }
    return r;
}

namespace {

//: The most the phrase store keeps. A phrase is mono floats, 176 KB a second,
//: so this is three quarters of an hour of distinct singing; past it the
//: phrases sung longest ago go first.
const size_t kPhraseBytes = 512u * 1024 * 1024;

//: Everything that decides how a phrase sounds, as text: the key it is kept
//: under. Doubles are written in full, so two phrases that differ by a hair
//: are two phrases.
std::string phrase_key(int voice_id, const Phrase &p) {
    std::string k;
    char buf[64];
    auto num = [&](double v) {
        std::snprintf(buf, sizeof buf, "%.17g,", v);
        k += buf;
    };
    num(voice_id);
    num(p.bpm);
    for (const auto &c : p.tempos) {
        k += "T";
        num(c.first);
        num(c.second);
    }
    num(p.level);
    num(p.any_bends);
    num(p.consonants);
    num(p.room);
    num(p.channel);
    for (int VoiceStyle::*f : {&VoiceStyle::color, &VoiceStyle::vibrato, &VoiceStyle::vibrato_rate,
                               &VoiceStyle::chorus, &VoiceStyle::breath, &VoiceStyle::detune,
                               &VoiceStyle::portamento})
        num(p.style.*f);
    for (const SungNote &n : p.notes) {
        k += "N";
        num(n.midi);
        num(n.beats);
        num(n.velocity);
        for (const std::string &ph : n.phonemes) k += ph + " ";
        for (double d : n.durations_ms) num(d);
        k += "W" + n.word + "|";
        controls_key(k, n.controls);
        num(n.hold);
    }
    for (const auto &h : p.held) {
        k += "H" + h.first + ":";
        num(h.second);
    }
    for (const ControlAt &c : p.controls) {
        k += "K" + c.name + ":";
        num(c.beat);
        num(c.value);
    }
    for (const BendEvent &b : p.bends) {
        k += "B";
        num(b.seconds);
        num(b.semitones);
    }
    for (const BendEvent &m : p.mods) {
        k += "M";
        num(m.seconds);
        num(m.semitones);
    }
    return k;
}

//: The same for a part sung whole, apart from any phrase's key.
std::string part_key(int voice_id, const Part &p) {
    Phrase as_phrase;
    as_phrase.bpm = p.bpm;
    as_phrase.tempos = p.tempos;
    as_phrase.notes = p.notes;
    as_phrase.bends = p.bends;
    as_phrase.any_bends = p.any_bends;
    as_phrase.mods = p.mods;
    as_phrase.style = p.style;
    as_phrase.level = p.level;
    as_phrase.consonants = p.consonants;
    as_phrase.channel = p.channel;
    as_phrase.controls = p.controls;
    std::string voices;
    for (const auto &v : p.voices) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "V%.17g,%d,", v.first, v.second);
        voices += buf;
    }
    char end[32];
    std::snprintf(end, sizeof end, "E%.17g", p.end);
    return "part:" + phrase_key(voice_id, as_phrase) + voices + end;
}

//: A part as a whole, rests and all, for an engine that sings it so.
Part whole_part(const PartRequest &t, const Clock &clock, double consonants) {
    Part part;
    part.bpm = clock.bpm;
    for (const TempoChange &c : clock.changes) part.tempos.push_back({c.beat, c.bpm});
    part.style = t.voice;
    part.level = t.volume * Registry::get().headroom(t.voice_id);
    part.consonants = t.consonants ? *t.consonants : consonants;
    part.channel = t.channel;
    part.voices = t.voices;
    part.controls = t.controls;
    double total = 0;
    for (const auto &e : t.notes) {
        SungNote n;
        n.midi = e.pitch;
        n.beats = e.beats;
        n.velocity = e.velocity ? e.velocity : t.velocity;
        n.phonemes = e.phonemes.empty() ? std::vector<std::string>{kRest} : e.phonemes;
        n.word = e.word;
        n.controls = e.controls;
        n.hold = e.hold;
        part.notes.push_back(n);
        total += e.beats;
    }
    std::vector<Pt> bends, mods;
    for (const BendPoint &b : t.bends) bends.push_back({clock.seconds(b.beat), b.semitones, b.slides});
    for (const BendPoint &b : t.mods) mods.push_back({clock.seconds(b.beat), b.semitones, b.slides});
    std::stable_sort(bends.begin(), bends.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });
    std::stable_sort(mods.begin(), mods.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });
    part.any_bends = !bends.empty();
    double span = clock.seconds(total + 0.4);
    part.bends = bend_events(bends, 0.0, span);
    part.mods = bend_events(mods, 0.0, span, wheel_step);
    return part;
}

//: The most backing tracks' files kept read at once. A file is stereo floats,
//: 21 MB a minute, and a song seldom has more than one or two.
const size_t kFilesKept = 4;

}  // namespace

//: An export being followed: the share of the whole the part being sung
//: stands for, and how long that part is, so that how far into it the
//: singing has got is how far the export has.
struct Mixer::Watch : SingWatch {
    std::function<bool(double)> report;
    double base = 0, span = 1;
    double length = 1;           // seconds
    int shown = -1;              // the last thousandth reported
    bool stop = false;
    //: `fraction` of the way through the current share; false once given up
    bool at(double fraction) {
        if (stop) return false;
        int k = int(1000 * (base + span * std::max(0.0, std::min(1.0, fraction))));
        if (k != shown) {
            shown = k;
            if (!report(k / 1000.0)) stop = true;
        }
        return !stop;
    }
    bool sung(double seconds) override { return at(seconds / std::max(length, 1e-6)); }
};

//: The watch on the singing thread for as long as it lives, if there is one.
struct Watching {
    explicit Watching(SingWatch *w) { sing_watch() = w; }
    ~Watching() { sing_watch() = nullptr; }
};

// The renders used to be WAV files in a cache folder, which nothing emptied;
// they are kept in memory now, so whatever an older copy of the program left
// in the temporary folder is cleared away the first time this one starts.
Mixer::Mixer() { remove_tree(join_path(temp_dir(), "singing-voice-studio-cache")); }

std::shared_ptr<const std::vector<float>> Mixer::backing(const std::string &path, std::string *error) {
    long long size = file_size(path), time = file_time(path);
    auto kept = files_.find(path);
    if (kept != files_.end() && kept->second.size == size && kept->second.time == time) {
        kept->second.used = ++clock_;
        return kept->second.samples;
    }
    auto y = std::make_shared<std::vector<float>>();
    if (!decode_audio(path, y.get(), error)) return nullptr;
    files_[path] = {size, time, y, ++clock_};
    while (files_.size() > kFilesKept) {
        auto oldest = files_.begin();
        for (auto it = files_.begin(); it != files_.end(); ++it)
            if (it->second.used < oldest->second.used) oldest = it;
        files_.erase(oldest);
    }
    return y;
}

void Mixer::keep(const std::string &key, std::shared_ptr<const std::vector<float>> sung,
                 bool stopped_short, double starts_before, int channels) {
    phrases_[key] = {sung, stopped_short, ++clock_, starts_before, channels};
    phrase_bytes_ += sung->size() * sizeof(float);
}

// After a mix, not during it, or a song bigger than the store pushes out its
// own parts before using them again. What the mix used stays, and the room
// is the biggest mix and a quarter, so one note heard keeps the song.
void Mixer::trim() {
    size_t used = 0;
    for (const auto &k : phrases_)
        if (k.second.used >= mixing_since_) used += k.second.samples->size() * sizeof(float);
    biggest_mix_ = std::max(biggest_mix_, used);
    size_t room = std::max(kPhraseBytes, biggest_mix_ + biggest_mix_ / 4);
    while (phrase_bytes_ > room && phrases_.size() > 1) {
        auto oldest = phrases_.end();
        for (auto it = phrases_.begin(); it != phrases_.end(); ++it)
            if (it->second.used < mixing_since_ && (oldest == phrases_.end() || it->second.used < oldest->second.used))
                oldest = it;
        if (oldest == phrases_.end()) break;
        phrase_bytes_ -= oldest->second.samples->size() * sizeof(float);
        phrases_.erase(oldest);
    }
}

std::vector<float> Mixer::track(const PartRequest &t, const Clock &clock, double end, double consonants,
                                double start, bool early, bool whole, bool *stopped_short, std::string *error,
                                int *channels) {
    if (t.consonants) consonants = *t.consonants;
    // the bends and the wheel on the clock: a tempo change moves them too
    std::vector<Pt> bends;
    for (const BendPoint &b : t.bends) bends.push_back({clock.seconds(b.beat), b.semitones, b.slides});
    // by time alone: two points may share a moment, which is how a step is
    // written, and sorting on the value would turn it into a slide
    std::stable_sort(bends.begin(), bends.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });
    std::vector<Pt> mods;
    for (const BendPoint &b : t.mods) mods.push_back({clock.seconds(b.beat), b.semitones, b.slides});
    std::stable_sort(mods.begin(), mods.end(), [](const Pt &a, const Pt &b) { return a.t < b.t; });

    // which voice sings from a beat on: the part's, then each change
    auto voice_at = [&](double beat) {
        int v = t.voice_id;
        for (const auto &c : t.voices)
            if (c.first <= beat + 1e-9) v = c.second;
        return v;
    };
    // the phrases between the rests and the changes of voice, and the
    // song's total length
    struct Run {
        double at;
        int voice;
        std::vector<const PartRequest::N *> notes;
    };
    std::vector<Run> runs;
    double at = 0.0;
    bool open = false;
    for (const auto &e : t.notes) {
        if (is_rest(e.phonemes)) {
            open = false;
        } else if (e.beats > 0 || played(voice_at(at))) {
            // a note taking no time is a chord's, which only an instrument plays
            int v = voice_at(at);
            if (!open || v != runs.back().voice) {
                runs.push_back({at, v, {}});
                open = true;
            }
            runs.back().notes.push_back(&e);
        }
        at += e.beats;
    }
    double total = at;
    double start_s = clock.seconds(start);
    double length = std::max(0.0, clock.seconds(total) - start_s) + kTailSeconds;
    // in stereo if an instrument plays any of it
    int ch = played(t.voice_id) ? 2 : 1;
    for (const auto &c : t.voices) ch = played(c.second) ? 2 : ch;
    *channels = ch;
    // the time before the cursor its notes may sing in (kPreRoll)
    const long pre = whole ? 0 : round_half_even(kPreRoll * kSampleRate);
    std::vector<float> out((size_t(round_half_even(length * kSampleRate)) + size_t(pre)) * size_t(ch), 0.0f);
    if (watch_) watch_->length = length;

    // VocalWriter's sequencer places the consonants before the beats itself,
    // and an export is sung as the whole track, as VocalWriter sang it; an
    // instrument always plays the whole track, so held notes carry over rests
    Family *family = Registry::get().family_of(t.voice_id);
    bool own_time = family && family->sings_parts();
    // so long as every voice it changes to is the same engine's
    for (const auto &c : t.voices) own_time = own_time && Registry::get().family_of(c.second) == family;
    if (own_time && (whole || family->plays())) {
        Part part = whole_part(t, clock, consonants);
        part.end = end;
        std::string key = part_key(t.voice_id, part);
        std::shared_ptr<const std::vector<float>> sung;
        int sung_ch = 1;
        auto kept = phrases_.find(key);
        if (kept != phrases_.end()) {
            kept->second.used = ++clock_;
            sung = kept->second.samples;
            sung_ch = kept->second.channels;
            if (kept->second.stopped_short) *stopped_short = true;
        } else {
            std::string err;
            std::unique_ptr<Singer> singer = Registry::get().singer(t.voice_id, &err);
            if (!singer) {
                *error = "cannot sing " + Registry::get().voice_name(t.voice_id) + ": " + err;
                return out;
            }
            Sung y;
            {
                // the part from the song's start, the export from the cursor
                if (watch_) watch_->length = std::max(length + start_s, 1e-6);
                Watching on(watch_);
                y = singer->sing_part(part);
            }
            if (watch_ && watch_->stop) {
                *error = "the export was cancelled";        // half a part is never kept
                return out;
            }
            if (!y.error.empty()) {
                *error = Registry::get().voice_name(t.voice_id) + ": " + y.error;
                return out;
            }
            if (y.stopped_short) *stopped_short = true;
            sung_ch = y.channels;
            sung = std::make_shared<const std::vector<float>>(std::move(y.samples));
            keep(key, sung, y.stopped_short, 0.0, sung_ch);
        }
        size_t skip = size_t(std::max(0L, round_half_even(start_s * kSampleRate)));
        lay(out, ch, pre, *sung, sung_ch, skip);
        return out;
    }

    // made the first time a phrase is not already in the store, so a song
    // whose phrases are all there sings nothing and starts no engine; one
    // for each voice the part sings with
    std::map<int, std::unique_ptr<Singer>> singers;
    double was_over = 0.0;
    for (size_t r = 0; r < runs.size(); ++r) {
        Run &run = runs[r];
        int voice = run.voice;
        Family *f = Registry::get().family_of(voice);
        bool own = f && f->sings_parts();
        Phrase phrase;
        phrase.style = t.voice;
        // the voice's own level, so that a part that would clip is turned
        // down before an engine clamps it rather than after
        phrase.level = t.volume * Registry::get().headroom(voice);
        phrase.any_bends = !bends.empty();
        double sum = 0;                               // the phrase's beats
        for (const auto *e : run.notes) sum += e->beats;
        // VocalWriter's voices hear its controls, sung phrase by phrase or not
        bool hears = own || is_vocalwriter_voice(voice);
        if (hears) {
            // what the controllers and the notes before it set, the last of
            // each, which the phrase starts with; and the controllers inside it
            std::map<std::string, std::pair<double, double>> last;   // (beat, value)
            for (const ControlAt &c : t.controls) {
                if (c.beat > run.at + 1e-9) {
                    if (c.beat < run.at + sum - 1e-9) phrase.controls.push_back({c.beat - run.at, c.name, c.value});
                    continue;
                }
                auto it = last.find(c.name);
                if (it == last.end() || c.beat >= it->second.first) last[c.name] = {c.beat, c.value};
            }
            double note_beat = 0;
            for (const auto &e : t.notes) {
                if (note_beat >= run.at - 1e-9) break;
                for (const auto &c : e.controls)
                    for (const auto &pt : c.second) {
                        double b = note_beat + pt.first * e.beats;
                        auto it = last.find(c.first);
                        if (it == last.end() || b >= it->second.first) last[c.first] = {b, pt.second};
                    }
                note_beat += e.beats;
            }
            for (const auto &l : last) phrase.held[l.first] = l.second.second;
        }
        if (own) {
            // VocalWriter keeps the song's tempo changes itself
            phrase.consonants = consonants;
            // a beat of rest even where nothing comes before it, without which
            // the engine skips the first note's consonants
            phrase.room = run.at - was_over > 1e-9 ? std::min(run.at - was_over, 1.0) : 1.0;
            phrase.channel = t.channel;
            double from = run.at - phrase.room;
            phrase.bpm = clock.bpm_at(from);
            for (const TempoChange &c : clock.changes)
                if (c.beat > from) phrase.tempos.push_back({c.beat - from, c.bpm});
        } else {
            // any other engine has the notes at the tempo the phrase starts at
            phrase.bpm = clock.bpm_at(run.at);
        }
        double bpm = phrase.bpm, spb = 60.0 / std::max(bpm, 1e-6), note_at = run.at;
        for (const auto *e : run.notes) {
            SungNote n;
            n.midi = e->pitch;
            n.beats = own ? e->beats : (clock.seconds(note_at + e->beats) - clock.seconds(note_at)) / spb;
            note_at += e->beats;
            // only VocalWriter's voices are sung harder or softer; to the
            // others it would be loudness, which the part's volume is
            n.velocity = !uses_velocity(voice) ? kDefaultVelocity : e->velocity ? e->velocity : t.velocity;
            n.phonemes = e->phonemes.empty() ? std::vector<std::string>{kRest} : e->phonemes;
            n.durations_ms = syllable_lengths(n.phonemes, n.beats * 60000.0 / bpm, consonants);
            n.word = e->word;
            n.hold = e->hold;
            if (hears) n.controls = e->controls;
            phrase.notes.push_back(n);
        }
        if (hears && !own) {
            // Phrase by phrase the controls go on the notes, each where it
            // falls: the ones standing at the start on the first, ahead of
            // its own, and each controller's on the note it comes in.
            auto put = [](NoteControls &on, const std::string &name, double at, double value) {
                auto &pts = on[name];
                auto it = std::lower_bound(pts.begin(), pts.end(), at,
                                           [](const std::pair<double, double> &p, double a) { return p.first < a; });
                pts.insert(it, {at, value});
            };
            for (const auto &h : phrase.held) put(phrase.notes.front().controls, h.first, 0.0, h.second);
            for (const ControlAt &c : phrase.controls) {
                double start = 0;
                for (size_t k = 0; k < run.notes.size(); ++k) {
                    double beats = run.notes[k]->beats;
                    if (k + 1 < run.notes.size() && c.beat >= start + beats - 1e-9) {
                        start += beats;
                        continue;
                    }
                    double at = beats > 0 ? (c.beat - start) / beats : 0.0;
                    put(phrase.notes[k].controls, c.name, std::max(0.0, std::min(1.0, at)), c.value);
                    break;
                }
            }
            phrase.held.clear();
            phrase.controls.clear();
        }
        // each note's start as written, for its bend (bends_ahead)
        std::vector<double> written;
        for (double s = clock.seconds(run.at); written.size() < phrase.notes.size();) {
            written.push_back(s);
            s += phrase.notes[written.size() - 1].beats * spb;
        }
        double lead = early && !own ? anticipate(phrase.notes, bpm, consonants, run.at - was_over) : 0.0;
        was_over = run.at + sum - lead;
        // a marked rest to scale the last syllable against, and to let it
        // decay rather than being cut off
        SungNote tail;
        tail.midi = phrase.notes.back().midi;
        tail.beats = 0.4;
        tail.velocity = 1;
        tail.phonemes = {kRest};
        if (own) {
            // the rest the whole track has here, up to a beat, towards the
            // next note's key, so the phrase ends as it does there
            double gap = (r + 1 < runs.size() ? runs[r + 1].at : total) - (run.at + sum);
            tail.beats = gap > 0 ? std::min(gap, 1.0) : 1.0;
            if (r + 1 < runs.size()) tail.midi = runs[r + 1].notes.front()->pitch;
        }
        phrase.notes.push_back(tail);
        // where the phrase's samples begin and end, in seconds
        double run_s = clock.seconds(run.at);
        double from_s = own ? clock.seconds(run.at - phrase.room) : run_s - lead * spb;
        double sung_beats = 0;
        for (const SungNote &n : phrase.notes) sung_beats += n.beats;
        double end_s = own ? clock.seconds(run.at + sum + tail.beats) : from_s + sung_beats * spb;
        if (end_s <= start_s) continue;               // over before the cursor
        phrase.bends = bend_events(early && !own ? bends_ahead(bends, written, phrase.notes, from_s, spb) : bends,
                                   from_s, end_s);
        phrase.mods = bend_events(mods, from_s, end_s, wheel_step);
        std::string key = phrase_key(voice, phrase);
        std::shared_ptr<const std::vector<float>> sung;
        double before = 0.0;
        int sung_ch = 1;
        auto kept = phrases_.find(key);
        if (kept != phrases_.end()) {
            kept->second.used = ++clock_;
            sung = kept->second.samples;
            before = kept->second.starts_before;
            sung_ch = kept->second.channels;
            if (kept->second.stopped_short) *stopped_short = true;
        } else {
            std::unique_ptr<Singer> &singer = singers[voice];
            if (!singer) {
                std::string err;
                singer = Registry::get().singer(voice, &err);
                if (!singer) {
                    *error = "cannot sing " + Registry::get().voice_name(voice) + ": " + err;
                    return out;
                }
            }
            Sung y = singer->sing(phrase);
            if (!y.error.empty()) {
                // never kept: the next play asks the engine again
                *error = Registry::get().voice_name(voice) + ": " + y.error;
                return out;
            }
            if (y.stopped_short) *stopped_short = true;
            sung_ch = y.channels;
            sung = std::make_shared<const std::vector<float>>(std::move(y.samples));
            before = y.starts_before;
            keep(key, sung, y.stopped_short, before, sung_ch);
        }
        if (own) from_s = run_s - before;
        // a phrase begun before the cursor is heard from it on; one from the
        // cursor keeps what it sings ahead of it
        long at = round_half_even((from_s - start_s) * kSampleRate) + pre;
        size_t skip = 0;
        if (run.at < start - 1e-9 && at < pre) {
            skip = size_t(pre - at);
            at = pre;
        }
        lay(out, ch, at, *sung, sung_ch, skip);
        if (watch_ && !watch_->at((end_s - start_s) / length)) {
            *error = "the export was cancelled";
            return out;
        }
    }
    return out;
}

Mixed Mixer::mix(const RenderRequest &req) {
    std::lock_guard<std::mutex> lock(mu_);
    Mixed res = mix_locked(req);
    if (res.ok && req.metronome) {
        // on a copy: the song kept for next time is only ever the singing
        auto ticked = std::make_shared<std::vector<float>>(*res.samples);
        with_metronome(*ticked, res.channels, req.clock(), *req.metronome, req.start, res.lead);
        res.samples = ticked;
    }
    return res;
}

RenderResult Mixer::render(const RenderRequest &req, const std::string &out_path,
                           std::function<bool(double)> progress) {
    Mixed m;
    {
        std::lock_guard<std::mutex> lock(mu_);
        Watch watch;
        watch.report = progress;
        watch_ = progress ? &watch : nullptr;
        m = mix_locked(req, true);
        watch_ = nullptr;
        // the last chance to give it up, before the file is written
        if (m.ok && progress && !progress(0.99)) {
            m.ok = false;
            m.cancelled = true;
            m.error = "the export was cancelled";
        }
    }
    RenderResult res;
    res.ok = m.ok;
    res.error = m.error;
    res.seconds = m.seconds;
    res.peak = m.peak;
    res.cached = m.cached;
    res.stopped_short = m.stopped_short;
    res.cancelled = m.cancelled;
    res.path = out_path;
    if (!m.ok) return res;
    std::vector<float> y = *m.samples;
    if (req.metronome) with_metronome(y, m.channels, req.clock(), *req.metronome, req.start, m.lead);
    // WAV, FLAC or Ogg Vorbis, as the file's name says
    if (!write_audio(out_path, y, m.channels, &res.error)) res.ok = false;
    return res;
}

Mixed Mixer::mix_locked(const RenderRequest &req, bool whole) {
    Mixed res;
    // the metronome is put on afterwards and is no part of the key, so
    // switching it on and off never sings anything again
    RenderRequest core = req;
    core.metronome.reset();
    std::string key = request_key(core) + (whole ? "whole" : "");
    mixing_since_ = clock_ + 1;
    if (last_.ok && key == last_key_) {
        Mixed again = last_;
        again.cached = true;
        return again;
    }

    bool any = !req.backings.empty();
    for (const PartRequest &t : req.tracks) any = any || !t.notes.empty();
    if (!any) {
        res.error = "nothing to sing";
        return res;
    }
    bool stopped = false;
    auto cancelled = [&] {
        res.cancelled = true;
        res.error = "the export was cancelled";
        return res;
    };
    if (whole) {
        std::vector<float> out;
        int channels = 1;
        if (watch_) {
            watch_->base = 0;
            watch_->span = 0.97;
        }
        bool together = sung_together(req, &out, &channels);
        if (watch_ && watch_->stop) return cancelled();
        if (together) {
            double peak = 0;
            for (float v : out) peak = std::max(peak, double(std::fabs(v)));
            if (!add_backings(req, out, channels, peak, &res.error)) return res;
            return finished(key, std::move(out), channels, peak, stopped);
        }
    }
    // grouped by the reverb each part ends up with, in the order first met
    std::vector<std::pair<Reverb, std::vector<size_t>>> groups;
    std::vector<std::vector<float>> ys(req.tracks.size());
    std::vector<int> chans(req.tracks.size(), 1);
    Reverb song_reverb = clean_reverb(req.reverb);
    for (size_t k = 0; k < req.tracks.size(); ++k) {
        const PartRequest &t = req.tracks[k];
        std::string err;
        if (watch_) {
            // each track an equal share of the singing, which is most of it
            watch_->base = 0.8 * double(k) / double(req.tracks.size());
            watch_->span = 0.8 / double(req.tracks.size());
        }
        ys[k] = track(t, req.clock(), req.end, req.consonants, req.start, req.anticipate, whole, &stopped, &err,
                      &chans[k]);
        if (watch_ && watch_->stop) return cancelled();
        if (!err.empty()) {
            res.error = err;
            return res;
        }
        Reverb rev = t.reverb ? clean_reverb(*t.reverb) : song_reverb;
        auto g = std::find_if(groups.begin(), groups.end(),
                              [&](const std::pair<Reverb, std::vector<size_t>> &x) { return x.first == rev; });
        if (g == groups.end()) groups.push_back({rev, {k}});
        else g->second.push_back(k);
    }
    size_t n = 0;                                     // frames
    // the tracks begin the pre-roll before the cursor when playing
    const long pre = whole ? 0 : round_half_even(kPreRoll * kSampleRate);
    for (size_t k = 0; k < ys.size(); ++k) n = std::max(n, ys[k].size() / size_t(chans[k]));
    bool stereo = false;
    for (const PartRequest &t : req.tracks) stereo = stereo || panned(t);
    for (const auto &g : groups) stereo = stereo || g.first.second > 0;
    for (int c : chans) stereo = stereo || c == 2;

    std::vector<float> out;
    int channels = 1;
    double peak = 0;
    if (stereo) {
        channels = 2;
        struct Mix {
            Reverb rev;
            std::vector<float> mix, room;
        };
        std::vector<Mix> mixes;
        for (const auto &g : groups) {
            Mix m{g.first, std::vector<float>(n * 2, 0.0f), std::vector<float>(n * 2, 0.0f)};
            for (size_t k : g.second) {
                const auto &y = ys[k];
                size_t frames = y.size() / size_t(chans[k]);
                auto steps = mix_steps(req.tracks[k], req.clock(), req.start, pre);
                for (size_t j = 0; j < steps.size(); ++j) {
                    size_t a = std::min(steps[j].frame, frames);
                    size_t b = j + 1 < steps.size() ? std::min(steps[j + 1].frame, frames) : frames;
                    double pan = steps[j].pan, g = steps[j].gain;
                    if (chans[k] == 2) {
                        // stereo already: its pan is a balance
                        double bl = std::min(1.0, 1.0 - pan), br = std::min(1.0, 1.0 + pan);
                        for (size_t i = 2 * a; i < 2 * b; i += 2) {
                            double l = y[i] * g, r = y[i + 1] * g;
                            m.mix[i] += float(l * bl);
                            m.mix[i + 1] += float(r * br);
                            m.room[i] += float(l);
                            m.room[i + 1] += float(r);
                        }
                        continue;
                    }
                    auto [gl, gr] = pan_gains(pan);
                    for (size_t i = a; i < b; ++i) {
                        double v = y[i] * g;
                        m.mix[2 * i] += float(v * gl);
                        m.mix[2 * i + 1] += float(v * gr);
                        // the same voices with the panning left off, which is
                        // what the reverberator is given: panning moves the
                        // voices, not the room they are singing in
                        m.room[2 * i] += float(v);
                        m.room[2 * i + 1] += float(v);
                    }
                }
            }
            mixes.push_back(std::move(m));
        }
        for (size_t i = 0; i < n * 2; ++i) {
            double s = 0;
            for (const Mix &m : mixes) s += m.mix[i];
            peak = std::max(peak, std::fabs(s));
        }
        // several voices at once can add up past full scale; turning the mix
        // down is better than clipping it, and has to happen before the
        // reverb, which works on 16-bit samples
        if (peak > 1.0)
            for (Mix &m : mixes) {
                for (float &v : m.mix) v = float(v / peak);
                for (float &v : m.room) v = float(v / peak);
            }
        std::vector<std::vector<float>> done;
        for (size_t gi = 0; gi < mixes.size(); ++gi) {
            Mix &m = mixes[gi];
            std::vector<float> wet = m.room;
            std::function<bool(double)> going;
            if (watch_) {
                // the rooms, after the singing, the next share
                watch_->base = 0.8 + 0.17 * double(gi) / double(mixes.size());
                watch_->span = 0.17 / double(mixes.size());
                going = [this](double f) { return watch_->at(f); };
            }
            bool reverbed = m.rev.second > 0 && vocalwriter_reverb(wet, m.rev.first, m.rev.second, going);
            if (watch_ && watch_->stop) return cancelled();
            if (!reverbed) wet = m.room;
            // the panning added back, at the gain the dry signal is mixed in with
            double dry = reverbed ? 1.0 - m.rev.second / 100.0 : 1.0;
            for (size_t i = 0; i < n * 2 && i < wet.size(); ++i)
                wet[i] += float((m.mix[i] - m.room[i]) * dry);
            done.push_back(std::move(wet));
        }
        size_t longest = 0;
        for (const auto &d : done) longest = std::max(longest, d.size());
        out.assign(longest, 0.0f);
        for (const auto &d : done)
            for (size_t i = 0; i < d.size(); ++i) out[i] += d[i];
    } else {
        out.assign(n, 0.0f);
        for (size_t k = 0; k < req.tracks.size(); ++k) {
            auto steps = mix_steps(req.tracks[k], req.clock(), req.start, pre);
            for (size_t j = 0; j < steps.size(); ++j) {
                double gl = pan_gains(steps[j].pan).first, g = steps[j].gain;
                size_t a = std::min(steps[j].frame, ys[k].size());
                size_t b = j + 1 < steps.size() ? std::min(steps[j + 1].frame, ys[k].size()) : ys[k].size();
                for (size_t i = a; i < b; ++i) out[i] += float(ys[k][i] * g * gl);
            }
        }
        for (float v : out) peak = std::max(peak, double(std::fabs(v)));
        if (peak > 1.0)
            for (float &v : out) v = float(v / peak);
    }

    // the time before the cursor, as much of it as is sung in
    long lead = 0;
    if (!whole) {
        long frames = long(out.size()) / channels, first = pre;
        for (long i = 0; i < std::min(pre, frames) && first == pre; ++i)
            for (int c = 0; c < channels; ++c)
                if (std::fabs(out[size_t(i * channels + c)]) > kSilent) first = i;
        out.erase(out.begin(), out.begin() + std::min(first, frames) * channels);
        lead = pre - first;
    }
    if (!add_backings(req, out, channels, peak, &res.error, lead)) return res;
    return finished(key, std::move(out), channels, peak, stopped, double(lead) / kSampleRate);
}

bool Mixer::sung_together(const RenderRequest &req, std::vector<float> *out, int *channels) {
    if (req.tracks.empty()) return false;
    Reverb rev = clean_reverb(req.reverb);
    std::vector<std::pair<int, Part>> parts;
    std::string key = "song:";
    bool instruments = false;
    for (const PartRequest &t : req.tracks) {
        instruments = instruments || played(t.voice_id);
        Family *f = Registry::get().family_of(t.voice_id);
        if (!f || !f->sings_parts() || panned(t) || (t.reverb && clean_reverb(*t.reverb) != rev))
            return false;
        for (const auto &v : t.voices)
            if (Registry::get().family_of(v.second) != f) return false;
        parts.push_back({t.voice_id, whole_part(t, req.clock(), req.consonants)});
        parts.back().second.end = req.end;
        key += part_key(t.voice_id, parts.back().second) + ";";
    }
    key += std::to_string(rev.first) + "," + std::to_string(rev.second);
    std::shared_ptr<const std::vector<float>> sung;
    auto kept = phrases_.find(key);
    if (kept != phrases_.end()) {
        kept->second.used = ++clock_;
        sung = kept->second.samples;
    } else {
        std::vector<float> stereo;
        std::string err;
        bool ok;
        {
            if (watch_) {
                double beats = 0;
                for (const PartRequest &t : req.tracks) {
                    double b = 0;
                    for (const auto &n : t.notes) b += n.beats;
                    beats = std::max(beats, b);
                }
                watch_->length = std::max(req.clock().seconds(beats) + kTailSeconds, 1e-6);
            }
            Watching on(watch_);
            ok = vocalwriter_song(parts, rev.first, rev.second, &stereo, &err);
        }
        if (!ok || (watch_ && watch_->stop)) return false;
        sung = std::make_shared<const std::vector<float>>(std::move(stereo));
        keep(key, sung, false, 0.0);
    }
    // with no reverb and no instruments the two sides are the same
    *channels = rev.second > 0 || instruments ? 2 : 1;
    const std::vector<float> &y = *sung;
    size_t frames = y.size() / 2;
    size_t skip = std::min(frames, size_t(std::max(0L, round_half_even(req.clock().seconds(req.start) * kSampleRate))));
    out->clear();
    for (size_t i = skip; i < frames; ++i) {
        out->push_back(y[2 * i]);
        if (*channels == 2) out->push_back(y[2 * i + 1]);
    }
    return true;
}

Mixed Mixer::finished(const std::string &key, std::vector<float> out, int channels, double peak,
                      bool stopped, double lead) {
    Mixed res;
    res.ok = true;
    res.channels = channels;
    res.seconds = double(out.size() / size_t(channels)) / kSampleRate;
    res.peak = peak;
    res.stopped_short = stopped;
    res.lead = lead;
    res.samples = std::make_shared<const std::vector<float>>(std::move(out));
    last_key_ = key;
    last_ = res;
    trim();
    return res;
}

// The backing tracks, under the voices and after their reverb. The file
// is placed by the clock, not the beat: it is the same recording at any
// tempo, and the cursor's beat is only where in it playing begins.
bool Mixer::add_backings(const RenderRequest &req, std::vector<float> &out, int &channels, double &peak,
                         std::string *error, long lead) {
    if (req.backings.empty()) return true;
    if (channels == 1) {
        std::vector<float> two(out.size() * 2);
        for (size_t i = 0; i < out.size(); ++i) two[2 * i] = two[2 * i + 1] = out[i];
        out.swap(two);
        channels = 2;
    }
    double from = req.clock().seconds(req.start);
    for (const BackingRequest &b : req.backings) {
        std::string err;
        std::shared_ptr<const std::vector<float>> file = backing(b.path, &err);
        if (!file) {
            *error = "the backing track " + b.name + " cannot be played: " + err;
            return false;
        }
        const std::vector<float> &x = *file;
        size_t frames = x.size() / 2;
        // nothing of it before the cursor, where the mix may begin `lead` early
        long at = round_half_even((b.start - from) * kSampleRate) + lead;
        size_t skip = at < lead ? size_t(lead - at) : 0, to = size_t(std::max(at, lead));
        if (skip >= frames) continue;               // over before the cursor
        size_t need = (to + frames - skip) * 2;
        if (out.size() < need) out.resize(need, 0.0f);
        // a balance rather than a pan: the file is stereo already, and
        // turning it to one side turns the other side down
        double gl = b.volume * std::min(1.0, 1.0 - b.pan), gr = b.volume * std::min(1.0, 1.0 + b.pan);
        for (size_t k = skip; k < frames; ++k) {
            out[2 * (to + k - skip)] += float(x[2 * k] * gl);
            out[2 * (to + k - skip) + 1] += float(x[2 * k + 1] * gr);
        }
    }
    double all = 0;
    for (float v : out) all = std::max(all, double(std::fabs(v)));
    if (all > 1.0)
        for (float &v : out) v = float(v / all);
    peak = std::max(peak, all);
    return true;
}

}  // namespace svs
