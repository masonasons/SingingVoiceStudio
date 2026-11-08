#include "core/midifile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

#include "core/paths.h"

namespace svs {

namespace {

const int kMetaText = 0x01, kMetaTrackName = 0x03, kMetaLyric = 0x05, kMetaTempo = 0x51,
          kMetaTimeSig = 0x58;

// MIDI text is bytes in whatever the writer liked; VocalWriter wrote Mac
// Roman. Plain ASCII is what matters for words, so anything above 127 is
// read as Latin-1, which keeps it one character to a byte. Control bytes
// are dropped but for the line breaks: Yamaha's files end every syllable
// with a zero ("And \0"), which would hide the space that ends the word.
std::string text_of(const unsigned char *p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c == '\t') {
            out.push_back(' ');
        } else if (c < 0x20 && c != '\r' && c != '\n') {
            continue;
        } else if (c < 0x80) {
            out.push_back(char(c));
        } else {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

uint32_t be32(const unsigned char *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}

const int kDrumChannel = 9;          // MIDI's channel 10

//: One half of a 14-bit control about 8192: the MSB starts a value, and the
//: LSB that follows it fills in its low bits.
void fine(std::vector<std::pair<long, int>> &values, bool lsb, long tick, int v) {
    if (!lsb) values.push_back({tick, (v << 7) - 8192});
    else if (!values.empty()) values.back().second = (((values.back().second + 8192) & ~127) | v) - 8192;
}

//: One track as it is in the file, its channels still apart.
struct RawTrack {
    std::string name;
    MidiTrack channel[16];              // each channel's notes and controls
    std::vector<std::pair<long, std::string>> texts, lyrics;
    std::vector<std::pair<long, long>> tempos;
    std::vector<MidiTrack::TimeSig> time_sigs;
};

RawTrack read_track(const unsigned char *b, size_t n) {
    RawTrack trk;
    for (int ch = 0; ch < 16; ++ch) trk.channel[ch].channel = ch;
    size_t i = 0;
    long tick = 0;
    int running = -1;
    int rpn_msb[16], rpn_lsb[16];
    std::fill(rpn_msb, rpn_msb + 16, 127);
    std::fill(rpn_lsb, rpn_lsb + 16, 127);
    // notes sounding, by channel and pitch
    std::map<std::pair<int, int>, std::deque<std::pair<long, int>>> sounding;
    auto need = [&](size_t k) {
        if (i + k > n) throw std::runtime_error("the track ends in the middle of an event");
    };
    auto vlq = [&]() {
        long v = 0;
        while (true) {
            need(1);
            unsigned char c = b[i++];
            v = (v << 7) | (c & 0x7F);
            if (!(c & 0x80)) return v;
        }
    };
    while (i < n) {
        tick += vlq();
        need(1);
        int status = b[i];
        if (status & 0x80) {
            running = status;
            ++i;
        } else {
            if (running < 0) throw std::runtime_error("running status with nothing to run on");
            status = running;
        }
        if (status == 0xFF) {
            need(1);
            int type = b[i++];
            long len = vlq();
            need(size_t(len));
            const unsigned char *d = b + i;
            i += size_t(len);
            if (type == kMetaTrackName) {
                trk.name = text_of(d, size_t(len));
            } else if (type == kMetaText) {
                trk.texts.push_back({tick, text_of(d, size_t(len))});
            } else if (type == kMetaLyric) {
                trk.lyrics.push_back({tick, text_of(d, size_t(len))});
            } else if (type == kMetaTempo && len == 3) {
                trk.tempos.push_back({tick, long(d[0]) << 16 | long(d[1]) << 8 | d[2]});
            } else if (type == kMetaTimeSig && len >= 2) {
                trk.time_sigs.push_back({tick, d[0], 1 << d[1]});
            }
        } else if (status == 0xF0 || status == 0xF7) {
            long len = vlq();
            need(size_t(len));
            i += size_t(len);
        } else {
            int high = status & 0xF0, ch = status & 0x0F;
            MidiTrack &c = trk.channel[ch];
            if (high == 0xC0 || high == 0xD0) {
                need(1);
                if (high == 0xC0) c.programs.push_back({tick, b[i]});
                i += 1;
                continue;
            }
            need(2);
            int a = b[i], v = b[i + 1];
            if (high == 0x90 && v > 0) {
                sounding[{ch, a}].push_back({tick, v});
            } else if (high == 0x80 || (high == 0x90 && v == 0)) {
                // as ConvertSMF reads it, a note lasts until its key's next
                // release, so that ends every note of the key still sounding
                auto &q = sounding[{ch, a}];
                for (const auto &[start, vel] : q) {
                    MidiNote note;
                    note.tick = start;
                    note.pitch = a;
                    note.velocity = vel;
                    note.duration = tick - start;
                    c.notes.push_back(note);
                }
                q.clear();
            } else if (high == 0xE0) {
                c.bends.push_back({tick, ((v << 7) | a) - 8192});
            } else if (high == 0xB0) {
                // RPN 0 is the pitch-bend sensitivity, which VocalWriter's own
                // exports change mid-song
                if (a == 1)
                    c.mods.push_back({tick, v});
                else if (a == 7)
                    c.volumes.push_back({tick, v});
                else if (a == 11)
                    c.expressions.push_back({tick, v});
                else if (a == 64)
                    c.sustains.push_back({tick, v});
                else if (a == 10)
                    c.pans.push_back({tick, v});
                else if (a == 6 && rpn_msb[ch] == 0 && rpn_lsb[ch] == 0)
                    c.bend_range.push_back({tick, v});
                else if (a == 101)
                    rpn_msb[ch] = v;
                else if (a == 100)
                    rpn_lsb[ch] = v;
                else if ((a == 6 || a == 38) && rpn_msb[ch] == 0 && rpn_lsb[ch] == 1)
                    fine(c.controls["detune"], a == 38, tick, v);
                else if (a == kChorusCC || a == kChorusCC + 32)
                    fine(c.controls["chorus"], a != kChorusCC, tick, v);
                else
                    for (const MidiControl &m : kMidiControls)
                        if (a == m.cc) c.controls[m.key].push_back({tick, v});
            }
            i += 2;
        }
    }
    for (MidiTrack &c : trk.channel)
        std::stable_sort(c.notes.begin(), c.notes.end(), [](const MidiNote &x, const MidiNote &y) {
            return x.tick != y.tick ? x.tick < y.tick : x.pitch < y.pitch;
        });
    return trk;
}

bool has_letters(const std::string &s) {
    for (unsigned char c : s)
        if (std::isalnum(c) || c >= 0x80) return true;
    return false;
}

//: The sung syllables of a track's lyrics, cleaned of a karaoke file's line
//: and page marks and its "----" placeholders. Where the lyrics mark words by
//: spaces, as karaoke files do, a syllable that runs on into the next one
//: without a space is joined to it with "-", VocalWriter's way, so that the
//: dictionary is asked for "into" rather than for "in" and "to".
std::vector<std::pair<long, std::string>> syllables(std::vector<std::pair<long, std::string>> raw) {
    // two events at one moment are one syllable
    std::vector<std::pair<long, std::string>> merged;
    for (auto &e : raw) {
        if (!merged.empty() && merged.back().first == e.first) merged.back().second += e.second;
        else merged.push_back(e);
    }
    struct Syl {
        long tick;
        std::string text;
        bool lead, trail, brk_before;
    };
    std::vector<Syl> out;
    bool pending = false;               // a line break or a gap since the last syllable
    int spaced = 0;
    for (auto &e : merged) {
        std::string t = e.second;
        std::vector<size_t> marks;
        for (size_t k = 0; k < t.size(); ++k)
            if (t[k] == '\r' || t[k] == '\n') marks.push_back(k);
        size_t first = t.find_first_not_of(' ');
        if (first != std::string::npos && (t[first] == '/' || t[first] == '\\'))
            marks.push_back(first);     // a karaoke line or page break
        for (size_t k : marks) t[k] = ' ';
        // a placeholder at the same moment as a syllable ("I " and "- ") is
        // no part of it, though the space before it still ends the word
        for (size_t p = 0; p < t.size();) {
            size_t q = std::min(t.find(' ', p), t.size());
            if (!has_letters(t.substr(p, q - p))) std::fill(t.begin() + long(p), t.begin() + long(q), ' ');
            p = q + 1;
        }
        if (!has_letters(t)) {          // a mark, or a placeholder for no words
            pending = true;
            continue;
        }
        size_t a = t.find_first_not_of(' '), z = t.find_last_not_of(' ');
        bool before = false, after = false;
        for (size_t k : marks) (k < a ? before : after) = true;
        bool lead = a > 0, trail = z + 1 < t.size();
        if (lead || trail) ++spaced;
        out.push_back({e.first, t.substr(a, z - a + 1), lead, trail, pending || before});
        pending = after;
    }
    // spaces mark the words only where the lyrics use them at all
    bool by_spaces = !out.empty() && spaced * 10 >= int(out.size());
    std::vector<std::pair<long, std::string>> words;
    for (size_t k = 0; k < out.size(); ++k) {
        std::string w = out[k].text;
        bool runs_on = k + 1 < out.size() && !out[k].trail && !out[k + 1].lead && !out[k + 1].brk_before;
        if (by_spaces && runs_on && w.back() != '-') w += "-";
        words.push_back({out[k].tick, w});
    }
    return words;
}

//: Each syllable's note, as an index into `notes` (sorted by time), or -1.
//: A syllable takes the nearest note not yet taken, keeping their order: one
//: up to a 32nd note before it (never as much as half way to the next note),
//: or one up to an 8th note after it: a pick-up's lyric sits ahead of its
//: note, and a karaoke file's lyrics run ahead of a sung line by as much as a
//: 16th. `exact` counts those that sit right on their note.
std::vector<int> pair_up(const std::vector<MidiNote> &notes,
                         const std::vector<std::pair<long, std::string>> &syls, double div, int *exact) {
    // a chord sings on its top note, the last one at its moment
    std::vector<long> ticks;
    std::vector<int> index;
    for (size_t k = 0; k < notes.size(); ++k) {
        if (!ticks.empty() && ticks.back() == notes[k].tick) index.back() = int(k);
        else {
            ticks.push_back(notes[k].tick);
            index.push_back(int(k));
        }
    }
    const double before = div / 8, after = div / 2;
    std::vector<int> out(syls.size(), -1);
    *exact = 0;
    size_t from = 0;
    for (size_t s = 0; s < syls.size(); ++s) {
        long t = syls[s].first;
        size_t nx = size_t(std::upper_bound(ticks.begin() + long(from), ticks.end(), t) - ticks.begin());
        long best = -1;
        double dist = 0;
        if (nx > from) {
            size_t pv = nx - 1;
            double gap = pv + 1 < ticks.size() ? double(ticks[pv + 1] - ticks[pv]) / 2 : before;
            if (t - ticks[pv] <= std::min(before, gap)) {
                best = long(pv);
                dist = double(t - ticks[pv]);
            }
        }
        if (nx < ticks.size() && ticks[nx] - t <= after && (best < 0 || ticks[nx] - t < dist))
            best = long(nx);
        if (best < 0) continue;
        // a word with no note of its own ("A" before "jit-ter-bug") leaves
        // the note to the syllable that sits closer to it, if that one could
        // take it; one sung late takes the note it came after, up to an 8th
        if (s + 1 < syls.size()) {
            long d = ticks[size_t(best)] - syls[s + 1].first;
            if (std::labs(d) < std::labs(t - ticks[size_t(best)]) && (d >= 0 ? d <= after : -d <= before)) {
                if (nx == 0 || nx - 1 < from || size_t(best) == nx - 1 || t - ticks[nx - 1] > after) continue;
                best = long(nx - 1);
            }
        }
        out[s] = index[size_t(best)];
        if (ticks[size_t(best)] == t) ++*exact;
        from = size_t(best) + 1;
    }
    return out;
}

//: Whether a piece of lyric is a syllable of its own: it has a vowel letter,
//: and if it `continues` a word, it does not open on one, unlike the "xt" of
//: "ne", "xt" or the "ed" of "lov", "ed". A word starting ("a ") stands alone,
//: and so does one with a figure in it ("39"), said as words.
bool own_syllable(const std::string &piece, bool continues) {
    if (std::any_of(piece.begin(), piece.end(), [](unsigned char c) { return std::isdigit(c); })) return true;
    std::string t;
    for (unsigned char c : piece)
        if (std::isalpha(c)) t.push_back(char(std::tolower(c)));
    if (t.empty() || t.find_first_of("aeiouy") == std::string::npos) return false;
    return !continues || std::string("aeiou").find(t[0]) == std::string::npos;
}

//: Where a bend under a syllable at `t` arrives: the end of the run of
//: moving bend events, inside the note from `from` to `to`, that `t` falls in
//: or lies within a 16th of. `t` itself if there is none.
long bend_arrives(const std::vector<std::pair<long, int>> &bends, long from, long to, long t, double div) {
    const long step = std::lround(div / 8), near = std::lround(div / 4);
    std::vector<std::pair<long, int>> in;
    for (const auto &b : bends)
        if (b.first >= from && b.first < to) in.push_back(b);
    std::stable_sort(in.begin(), in.end(),
                     [](const std::pair<long, int> &x, const std::pair<long, int> &y) { return x.first < y.first; });
    for (size_t i = 0; i < in.size();) {
        // a glide: events no more than a 32nd apart, the value changing
        size_t j = i;
        while (j + 1 < in.size() && in[j + 1].first - in[j].first <= step) ++j;
        bool moves = false;
        for (size_t k = i; k < j; ++k) moves = moves || in[k + 1].second != in[k].second;
        if (moves && t >= in[i].first - near && t <= in[j].first + near) return in[j].first;
        i = j + 1;
    }
    return t;
}

//: The words onto the notes. VocalWriter's own exports write a note's word
//: as a Text event and its phonemes as a Lyric event, both on the note;
//: everyone else writes the sung syllable as a Lyric event, near enough.
void put_words(std::vector<MidiNote> &notes, const std::vector<std::pair<long, int>> &bends, const RawTrack &trk,
               double div) {
    std::map<long, std::string> text_at, lyric_at;
    for (const auto &e : trk.texts) text_at[e.first] = e.second;
    for (const auto &e : trk.lyrics) lyric_at[e.first] += e.second;
    std::set<long> spoken;
    for (MidiNote &note : notes) {
        auto t = text_at.find(note.tick);
        if (t == text_at.end() || t->second.empty()) continue;
        auto l = lyric_at.find(note.tick);
        note.text = t->second;
        note.phonemes = l != lyric_at.end() ? l->second : "";
        spoken.insert(note.tick);
    }
    // A word in brackets with no note of its own is a cue, as a karaoke file
    // names the instrument of an interlude, "(Harp)", and is not sung.
    auto cue = [&](const std::pair<long, std::string> &e) {
        std::string w = e.second;
        w.erase(0, w.find_first_not_of(" \t\r\n"));
        w.erase(w.find_last_not_of(" \t\r\n") + 1);
        if (w.size() < 2 || !((w.front() == '(' && w.back() == ')') || (w.front() == '[' && w.back() == ']')))
            return false;
        for (const MidiNote &n : notes)
            if (std::labs(n.tick - e.first) <= std::lround(div / 16)) return false;
        return true;
    };
    std::vector<std::pair<long, std::string>> rest;
    for (const auto &e : trk.lyrics)
        if (!spoken.count(e.first) && !cue(e)) rest.push_back(e);
    auto syls = syllables(rest);
    int exact;
    std::vector<int> at = pair_up(notes, syls, div, &exact);
    // A syllable of its own left without a note is given one where it is
    // sung: a file that sings "the sea" on one note has the note divided at
    // "sea", and one that leaves "to" in a rest has a note for it there, at
    // the pitch of the note after, up to that note. Too short a piece is not
    // made, and the syllable is left to the merging below. A note bent from
    // one pitch to the next as the syllable comes is divided where the bend
    // arrives, so the syllable starts on its own pitch ("the" on C#4 gliding
    // down, "sea" on B3). One before the part's first note is none of the
    // song: the title and the singer, as karaoke files show them first.
    const long shortest = std::lround(div / 16);
    long first = notes.empty() ? 0 : notes.front().tick;
    for (const MidiNote &n : notes) first = std::min(first, n.tick);
    std::vector<MidiNote> made;
    for (size_t s = 0; s < syls.size(); ++s) {
        bool continues = s > 0 && syls[s - 1].second.back() == '-';
        if (at[s] >= 0 || !own_syllable(syls[s].second, continues) || syls[s].first < first) continue;
        long t = syls[s].first;
        MidiNote *on = nullptr, *next = nullptr;
        auto look = [&](MidiNote &n) {
            if (n.tick < t && n.tick + n.duration > t && (!on || n.tick >= on->tick)) on = &n;
            if (n.tick > t && (!next || n.tick < next->tick)) next = &n;
        };
        for (MidiNote &n : notes) look(n);
        for (MidiNote &n : made) look(n);
        MidiNote piece;
        piece.tick = t;
        piece.text = syls[s].second;
        if (on) {
            long end = on->tick + on->duration;
            long at_bend = bend_arrives(bends, on->tick, end, t, div);
            if (at_bend - on->tick >= shortest && end - at_bend >= shortest) t = piece.tick = at_bend;
            if (t - on->tick < shortest || end - t < shortest) continue;
            piece.pitch = on->pitch;
            piece.velocity = on->velocity;
            piece.duration = end - t;
            on->duration = t - on->tick;
        } else if (next) {
            if (next->tick - t < shortest) continue;
            piece.pitch = next->pitch;
            piece.velocity = next->velocity;
            piece.duration = next->tick - t;
        } else {
            continue;
        }
        made.push_back(piece);
        at[s] = -2;
    }
    // A piece left without a note is part of the syllable it runs on into,
    // or of the one that runs on into it: a karaoke file lights up "ne" and
    // then "xt " on one note, or "ha" on its note and "nd " a little later.
    for (size_t s = 0; s < syls.size(); ++s) {
        if (at[s] != -1) continue;
        std::string &w = syls[s].second;
        if (s + 1 < syls.size() && w.back() == '-') {
            syls[s + 1].second = w.substr(0, w.size() - 1) + syls[s + 1].second;
        } else if (s > 0 && at[s - 1] >= 0 && syls[s - 1].second.back() == '-') {
            std::string &before = syls[s - 1].second;
            before = before.substr(0, before.size() - 1) + w;
        }
    }
    for (size_t s = 0; s < syls.size(); ++s)
        if (at[s] >= 0 && notes[size_t(at[s])].text.empty()) notes[size_t(at[s])].text = syls[s].second;
    notes.insert(notes.end(), made.begin(), made.end());
    std::stable_sort(notes.begin(), notes.end(), [](const MidiNote &x, const MidiNote &y) {
        return x.tick != y.tick ? x.tick < y.tick : x.pitch < y.pitch;
    });
}

//: Which channel a track's lyrics are sung on: the one most syllables fall
//: on, never the drums; within a tenth of that, the one with most of its
//: notes sung, then channel 1 or 4, then most syllables right on their
//: notes, then the lowest.
int lyric_channel(const RawTrack &trk, const std::vector<int> &used, double div) {
    auto syls = syllables(trk.lyrics);
    struct Score {
        int ch, hits, exact;
        double share;
    };
    std::vector<Score> scores;
    for (int ch : used) {
        if (ch == kDrumChannel && used.size() > 1) continue;
        int exact;
        const std::vector<MidiNote> &notes = trk.channel[ch].notes;
        std::vector<int> at = pair_up(notes, syls, div, &exact);
        int hits = int(std::count_if(at.begin(), at.end(), [](int k) { return k >= 0; }));
        std::set<long> onsets;
        for (const MidiNote &n : notes) onsets.insert(n.tick);
        scores.push_back({ch, hits, exact, onsets.empty() ? 0.0 : double(hits) / onsets.size()});
    }
    if (scores.empty()) return used.front();
    int best = 0;
    for (const Score &s : scores) best = std::max(best, s.hits);
    auto near = [&](const Score &s) { return s.hits * 10 >= best * 9; };
    double share = 0;
    for (const Score &s : scores)
        if (near(s)) share = std::max(share, s.share);
    auto rank = [&](const Score &s) {
        bool in = near(s) && s.share * 10 >= share * 9;
        return std::make_tuple(near(s), in, in && (s.ch == 0 || s.ch == 3), in ? s.exact : 0, s.hits, -s.ch);
    };
    const Score *pick = &scores[0];
    for (const Score &s : scores)
        if (rank(s) > rank(*pick)) pick = &s;
    return pick->ch;
}

}  // namespace

MidiFile read_midi(const std::string &path, int lyrics_on) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b)) throw std::runtime_error("cannot read the file");
    if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd")
        throw std::runtime_error("not a Standard MIDI File");
    uint32_t hlen = be32(&b[4]);
    MidiFile mf;
    mf.format = b[8] << 8 | b[9];
    int ntracks = b[10] << 8 | b[11];
    mf.division = b[12] << 8 | b[13];
    double div = mf.division ? mf.division : 480;
    size_t pos = 8 + hlen;
    // the programs and volumes set on a track that plays nothing on the channel
    MidiTrack setup[16];
    for (int k = 0; k < ntracks; ++k) {
        if (pos + 8 > b.size() || std::string(b.begin() + pos, b.begin() + pos + 4) != "MTrk")
            throw std::runtime_error("a track is missing or damaged");
        uint32_t len = be32(&b[pos + 4]);
        if (pos + 8 + len > b.size()) throw std::runtime_error("a track runs past the end of the file");
        RawTrack trk = read_track(&b[pos + 8], len);
        pos += 8 + len;
        std::vector<int> used;
        for (int ch = 0; ch < 16; ++ch) {
            const MidiTrack &c = trk.channel[ch];
            if (!c.notes.empty()) {
                used.push_back(ch);
                continue;
            }
            MidiTrack &to = setup[ch];
            to.programs.insert(to.programs.end(), c.programs.begin(), c.programs.end());
            to.volumes.insert(to.volumes.end(), c.volumes.begin(), c.volumes.end());
            to.expressions.insert(to.expressions.end(), c.expressions.begin(), c.expressions.end());
            to.pans.insert(to.pans.end(), c.pans.begin(), c.pans.end());
        }
        // A track playing on several channels -- a type 0 file's only track
        // -- is a part for each, as a type 1 file has them; the tempo and
        // time signature stay on a track of their own. Unnamed, so each is
        // named after its instrument: the one name is the song's, or no one
        // part's.
        MidiTrack meta;
        meta.name = trk.name;
        meta.tempos = trk.tempos;
        meta.time_sigs = trk.time_sigs;
        if (used.size() <= 1) {
            MidiTrack t = used.empty() ? MidiTrack() : trk.channel[used[0]];
            t.name = trk.name;
            t.tempos = trk.tempos;
            t.time_sigs = trk.time_sigs;
            put_words(t.notes, t.bends, trk, div);
            mf.tracks.push_back(t);
            continue;
        }
        mf.tracks.push_back(meta);
        int sung = trk.lyrics.empty() && trk.texts.empty() ? -1 : lyric_channel(trk, used, div);
        // A type 0 file's lyrics go where the caller says, once it has been
        // asked which of the channels sings them.
        bool words = std::any_of(trk.lyrics.begin(), trk.lyrics.end(),
                                 [](const std::pair<long, std::string> &l) { return has_letters(l.second); });
        if (mf.format == 0 && words && mf.lyric_channels.empty()) {
            for (int ch : used)
                if (ch != kDrumChannel) mf.lyric_channels.push_back(ch);
            if (mf.lyric_channels.size() < 2) {
                mf.lyric_channels.clear();
            } else {
                mf.likely_lyric_channel = sung;
                if (std::find(mf.lyric_channels.begin(), mf.lyric_channels.end(), lyrics_on) != mf.lyric_channels.end())
                    sung = lyrics_on;
            }
        }
        for (int ch : used) {
            MidiTrack t = trk.channel[ch];
            t.name.clear();
            if (ch == sung) put_words(t.notes, t.bends, trk, div);
            mf.tracks.push_back(t);
        }
    }
    for (MidiTrack &t : mf.tracks) {
        if (t.notes.empty() || t.channel < 0) continue;
        const MidiTrack &from = setup[t.channel];
        if (t.programs.empty()) t.programs = from.programs;
        if (t.volumes.empty()) t.volumes = from.volumes;
        if (t.expressions.empty()) t.expressions = from.expressions;
        if (t.pans.empty()) t.pans = from.pans;
        if (t.controls.empty()) t.controls = from.controls;
    }
    return mf;
}

std::vector<std::string> split_phonemes(const std::string &s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if (std::isupper(c) && i + 1 < s.size() && std::isupper((unsigned char)s[i + 1])) {
            out.push_back(s.substr(i, 2));
            i += 2;
        } else if (std::islower(c) || c == '_') {
            // "_" is VocalWriter's spelling of a rest
            out.push_back(s.substr(i, 1));
            ++i;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace svs
