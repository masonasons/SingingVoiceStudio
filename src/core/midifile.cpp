#include "core/midifile.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <deque>
#include <map>
#include <stdexcept>

#include "core/paths.h"

namespace svs {

namespace {

const int kMetaText = 0x01, kMetaTrackName = 0x03, kMetaLyric = 0x05, kMetaTempo = 0x51,
          kMetaTimeSig = 0x58;

// MIDI text is bytes in whatever the writer liked; VocalWriter wrote Mac
// Roman. Plain ASCII is what matters for words, so anything above 127 is
// read as Latin-1, which keeps it one character to a byte.
std::string text_of(const unsigned char *p, size_t n) {
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        if (c < 0x80) {
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

MidiTrack read_track(const unsigned char *b, size_t n) {
    MidiTrack trk;
    size_t i = 0;
    long tick = 0;
    int running = -1;
    int rpn_msb = 127, rpn_lsb = 127;
    std::map<int, std::deque<std::pair<long, int>>> sounding;
    std::map<long, std::string> text_at, lyric_at;
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
                text_at[tick] = text_of(d, size_t(len));
            } else if (type == kMetaLyric) {
                lyric_at[tick] = text_of(d, size_t(len));
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
            int high = status & 0xF0;
            if (high == 0xC0 || high == 0xD0) {
                need(1);
                if (high == 0xC0) trk.programs.push_back({tick, b[i]});
                i += 1;
                continue;
            }
            need(2);
            int a = b[i], v = b[i + 1];
            if (high == 0x90 && v > 0) {
                sounding[a].push_back({tick, v});
            } else if (high == 0x80 || (high == 0x90 && v == 0)) {
                auto &q = sounding[a];
                if (!q.empty()) {
                    auto [start, vel] = q.front();
                    q.pop_front();
                    MidiNote note;
                    note.tick = start;
                    note.pitch = a;
                    note.velocity = vel;
                    note.duration = tick - start;
                    trk.notes.push_back(note);
                }
            } else if (high == 0xE0) {
                trk.bends.push_back({tick, ((v << 7) | a) - 8192});
            } else if (high == 0xB0) {
                // RPN 0 is the pitch-bend sensitivity, which VocalWriter's own
                // exports change mid-song
                if (a == 1)
                    trk.mods.push_back({tick, v});
                else if (a == 6 && rpn_msb == 0 && rpn_lsb == 0)
                    trk.bend_range.push_back({tick, v});
                else if (a == 101)
                    rpn_msb = v;
                else if (a == 100)
                    rpn_lsb = v;
            }
            i += 2;
        }
    }
    std::stable_sort(trk.notes.begin(), trk.notes.end(), [](const MidiNote &x, const MidiNote &y) {
        return x.tick != y.tick ? x.tick < y.tick : x.pitch < y.pitch;
    });
    for (MidiNote &note : trk.notes) {
        auto t = text_at.find(note.tick);
        auto l = lyric_at.find(note.tick);
        std::string text = t != text_at.end() ? t->second : "";
        std::string lyric = l != lyric_at.end() ? l->second : "";
        if (!text.empty()) {
            note.text = text;          // VocalWriter: the word, and its phonemes
            note.phonemes = lyric;
        } else {
            note.text = lyric;         // everyone else: the word as the lyric
        }
    }
    return trk;
}

}  // namespace

MidiFile read_midi(const std::string &path) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b)) throw std::runtime_error("cannot read the file");
    if (b.size() < 14 || std::string(b.begin(), b.begin() + 4) != "MThd")
        throw std::runtime_error("not a Standard MIDI File");
    uint32_t hlen = be32(&b[4]);
    MidiFile mf;
    mf.format = b[8] << 8 | b[9];
    int ntracks = b[10] << 8 | b[11];
    mf.division = b[12] << 8 | b[13];
    size_t pos = 8 + hlen;
    for (int k = 0; k < ntracks; ++k) {
        if (pos + 8 > b.size() || std::string(b.begin() + pos, b.begin() + pos + 4) != "MTrk")
            throw std::runtime_error("a track is missing or damaged");
        uint32_t len = be32(&b[pos + 4]);
        if (pos + 8 + len > b.size()) throw std::runtime_error("a track runs past the end of the file");
        mf.tracks.push_back(read_track(&b[pos + 8], len));
        pos += 8 + len;
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
        } else if (std::islower(c)) {
            out.push_back(s.substr(i, 1));
            ++i;
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace svs
