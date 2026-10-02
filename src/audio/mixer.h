// mixer.h -- a song in, a WAV out.
//
// Each part is cut into phrases at its rests, each phrase sung by the part's
// voice and placed at the time the score puts it; the parts are panned, the
// ones sharing a reverb setting go through one reverberator together, and the
// whole is turned down if it adds up past full scale. Finished renders are
// kept under a key made from the song, so playing the same thing twice
// renders once.
#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "core/project.h"
#include "voices/singer.h"

namespace svs {

struct PartRequest {
    int voice_id = 0;
    double volume = 1.0;     // fraction
    double pan = 0.0;        // -1 .. 1
    VoiceStyle voice;
    std::optional<Reverb> reverb;
    std::optional<double> consonants;
    int velocity = 64;
    struct N {
        int pitch;
        double beats;
        std::vector<std::string> phonemes;
    };
    std::vector<N> notes;
    std::vector<BendPoint> bends;   // (beat, semitones, slides)
    std::vector<BendPoint> mods;    // (beat, mod wheel 0-127, slides)
};

struct RenderRequest {
    double bpm = 120;
    double consonants = 1.0;
    double start = 0.0;          // beats
    Reverb reverb{0, 0};
    bool anticipate = true;
    std::vector<PartRequest> tracks;
    //: beats to the bar, when the metronome is wanted (never in an export)
    std::optional<double> metronome_bar;
};

struct RenderResult {
    bool ok = false;
    std::string error;
    double seconds = 0;
    double peak = 0;
    std::string path;
    bool cached = false;
    bool stopped_short = false;
};

//: The song as the renderer wants it: `parts` are the tracks to sing and
//: nothing else, so mute and solo are settled by whoever calls this.
RenderRequest make_request(const Song &song, const std::vector<const Track *> &parts,
                           double start, const std::map<int, int> &program_map);

class Mixer {
public:
    Mixer();
    RenderResult render(const RenderRequest &req, const std::string &out);

private:
    std::vector<float> track(const PartRequest &t, double bpm, double consonants, double start,
                             bool early, bool *stopped_short, std::string *error);
    std::mutex mu_;
    std::map<std::string, std::pair<double, double>> cache_;
    std::string cache_dir_;
};

//: One tick of the metronome mixed in, accented at each bar line; `start` is
//: the beat the audio begins on so the ticks stay on the song's beats.
void with_metronome(std::vector<float> &y, int channels, double bpm, double bar_beats,
                    double start);

}  // namespace svs
