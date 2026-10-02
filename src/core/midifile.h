// midifile.h -- a minimal Standard MIDI File reader.
//
// Enough for importing: notes with their text, the tempo, the time signature,
// pitch bend and the bend range set by RPN 0. VocalWriter's own exports write
// each note's word as a Text event and the phonemes it derived as a Lyric
// event beside it; everyone else writes the sung word in the Lyric event,
// which is what that event is for. Both are read.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace svs {

struct MidiNote {
    long tick = 0;
    int pitch = 60;
    int velocity = 64;
    long duration = 0;
    std::string text;       // the word
    std::string phonemes;   // VocalWriter's spelling of it, case carrying length
};

struct MidiTrack {
    std::string name;
    std::vector<MidiNote> notes;
    std::vector<std::pair<long, long>> tempos;        // (tick, microseconds a beat)
    std::vector<std::pair<long, int>> bends;          // (tick, -8192..8191)
    std::vector<std::pair<long, int>> bend_range;     // (tick, semitones)
    std::vector<std::pair<long, int>> programs;
    std::vector<std::pair<long, int>> mods;           // (tick, CC 1, the mod wheel)
    struct TimeSig {
        long tick;
        int num;
        int den;
    };
    std::vector<TimeSig> time_sigs;
};

struct MidiFile {
    int format = 0;
    int division = 480;
    std::vector<MidiTrack> tracks;
};

//: Throws std::runtime_error.
MidiFile read_midi(const std::string &path);

//: Split VocalWriter's phoneme string: every uppercase symbol is two letters,
//: every lowercase one is one, so "swIYt" is s w IY t.
std::vector<std::string> split_phonemes(const std::string &s);

}  // namespace svs
