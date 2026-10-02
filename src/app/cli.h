// cli.h -- the same program without the window: a song in, a WAV out.
//
//   svs song.vws -o song.wav
//   svs tune.mid -o tune.wav --voice "DECtalk Betty" --tempo 96
//   svs song.vws --tracks stems
//   svs --list-voices
#pragma once

#include <string>
#include <vector>

namespace svs {

struct CliArgs {
    std::string file, output, tracks, save;
    std::vector<std::string> track;
    bool has_tempo = false;
    double tempo = 0;
    std::string voice;
    bool has_consonants = false;
    double consonants = 0;
    std::string reverb;
    int anticipate = -1;          // -1 unset, 0 off, 1 on
    double start = 0.0;
    bool list_voices = false, list_tracks = false, version = false, quiet = false, help = false;
    std::vector<std::string> pronounce;
    std::string error;            // what was wrong with the command line
};

CliArgs parse_args(const std::vector<std::string> &argv);
std::string usage();
//: whether these arguments are a job to do rather than a window to open
bool wants_console(const CliArgs &a);
int run_cli(const CliArgs &a);

}  // namespace svs
