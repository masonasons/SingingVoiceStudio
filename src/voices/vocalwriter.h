// vocalwriter.h -- VocalWriter 2.0's own synthesiser, its data and its
// dictionary.
//
// The synthesis is VocalWriter's PowerPC code lifted into C (engine/, the
// VocalWriterC project) and checked against the original until every sample
// agrees. Its data files are KAE Labs' and are read from `assets\` beside the
// program.
#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "voices/family.h"

struct vw_editor;

namespace svs {

//: VocalWriter's four files, read once.
class VwAssets {
public:
    static VwAssets &get();
    bool ok() const { return ok_; }
    bool has_bank() const { return !bank.empty(); }
    //: which required files are missing, relative to the data folder
    const std::vector<std::string> &missing() const { return missing_; }
    std::string where() const { return root_; }

    std::vector<unsigned char> rsrc, gmspeech, bank, lexicon;

    //: A fresh engine with the tables, the voices and (if present) the
    //: instrument bank loaded. Null if the files are not there.
    vw_editor *open_editor() const;

private:
    VwAssets();
    bool ok_ = false;
    std::vector<std::string> missing_;
    std::string root_;
};

//: Words to phonemes through VocalWriter's own dictionary search, suffix
//: rules and letter-to-sound rules, then the application's allophony.
class Lexicon {
public:
    static Lexicon &get();
    bool ok() const { return ed_ != nullptr; }
    //: empty if there is no pronunciation; `marked` puts kSyllableBreak
    //: between the dictionary's syllables. A word the dictionary does not
    //: know, written in pieces with hyphens, is the pieces' if it knows them.
    std::vector<std::string> phonemes(const std::string &word, bool marked = false);

private:
    Lexicon();
    //: one word, with the lock held; `known`: the dictionary knew it
    std::vector<std::string> look_up(const std::string &clean, bool marked, bool *known);
    vw_editor *ed_ = nullptr;
    std::mutex mu_;
};

//: Whether VocalWriter's voices are sung through VocalWriter 2.0's own play
//: path, its parser and sequencer, so that a part comes out sample for sample
//: as VocalWriter sang it; otherwise they are sung phrase by phrase, as every
//: other engine is and as VocalWriter Studio sang them. Off to begin with.
void set_vocalwriter_own_path(bool on);
bool vocalwriter_own_path();

//: Whether the GM Soundbank moves the instruments it has an octave off by
//: that octave, so that they sound where their notes are: programs 29 - 32,
//: 34, 36, 37, 53, 80, 86, 96, 100 and 101 up, 98 and 102 down, counting from
//: 0. Only the sound moves, not the notes. Off to begin with.
void set_gm_octaves(bool on);
bool gm_octaves();

//: Which voice each old program number picks, for songs written down as
//: program changes. -1 where there is none.
std::vector<int> vocalwriter_program_voices();

//: The parts of a song, as (voice, part) and each sung by VocalWriter, sung
//: together as VocalWriter played a song: one mix through its reverb, into
//: interleaved stereo. False, with `error` set if the engine failed, when
//: it could not: then they are sung one by one.
bool vocalwriter_song(const std::vector<std::pair<int, Part>> &parts, int room, int wet,
                      std::vector<float> *stereo, std::string *error);
std::string vocalwriter_engine_name();

//: The application's reverb over interleaved stereo floats, through the
//: engine's own 16-bit reverberator. Returns false (and leaves `mix` alone)
//: if the reverb is off or the engine is not available. `going`, if given,
//: hears how far it has got, 0 to 1, and returns false to stop it: then it
//: returns false too, `mix` part done.
bool vocalwriter_reverb(std::vector<float> &mix, int room, int wet,
                        const std::function<bool(double)> &going = {});

}  // namespace svs
