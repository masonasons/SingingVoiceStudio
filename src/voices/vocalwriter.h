// vocalwriter.h -- VocalWriter 2.0's own synthesiser, its data and its
// dictionary.
//
// The synthesis is VocalWriter's PowerPC code lifted into C (engine/, the
// VocalWriterC project) and checked against the original until every sample
// agrees. Its data files are KAE Labs' and are read from `assets\` beside the
// program.
#pragma once

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
    //: empty if there is no pronunciation
    std::vector<std::string> phonemes(const std::string &word);

private:
    Lexicon();
    vw_editor *ed_ = nullptr;
    std::mutex mu_;
};

//: Which voice each old program number picks, for songs written down as
//: program changes. -1 where there is none.
std::vector<int> vocalwriter_program_voices();
std::string vocalwriter_engine_name();

//: The application's reverb over interleaved stereo floats, through the
//: engine's own 16-bit reverberator. Returns false (and leaves `mix` alone)
//: if the reverb is off or the engine is not available.
bool vocalwriter_reverb(std::vector<float> &mix, int room, int wet);

}  // namespace svs
