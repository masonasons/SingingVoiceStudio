// mixer.h -- a song in, a WAV out.
//
// Each part is cut into phrases at its rests, each phrase sung by the part's
// voice and placed at the time the score puts it (an instrument plays its part
// whole, in stereo); the parts are panned, the
// ones sharing a reverb setting go through one reverberator together, and the
// whole is turned down if it adds up past full scale. A backing track's file
// is laid under the voices after the reverb, which is the voices' room alone. Finished renders are
// kept under a key made from the song, so playing the same thing twice
// renders once.
#pragma once

#include <functional>
#include <map>
#include <memory>
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
    int velocity = kDefaultVelocity;
    //: how many of the song's tracks before this one its engine sings too
    int channel = 0;
    struct N {
        int pitch;
        double beats;
        std::vector<std::string> phonemes;
        std::string word = {};
        int velocity = 0;                // 0: the part's
        NoteControls controls = {};
        double hold = 0;                 // Note::hold
    };
    std::vector<N> notes;
    //: (beat, voice): where it goes on to another voice; `voice_id` is the
    //: one it starts singing with
    std::vector<std::pair<double, int>> voices;
    //: the controls controllers set, each from its beat on
    std::vector<ControlAt> controls;
    //: (beat, -1 .. 1): the pan a controller sets from its beat on
    std::vector<std::pair<double, double>> pans;
    std::vector<BendPoint> bends;   // (beat, semitones, slides)
    std::vector<BendPoint> mods;    // (beat, mod wheel 0-127, slides)
};

//: A backing track: a sound file played under the voices.
struct BackingRequest {
    std::string path;
    double volume = 1.0;     // fraction
    double pan = 0.0;        // -1 .. 1
    double start = 0.0;      // seconds into the song the file begins
    std::string name;        // the track's, for a message
};

struct RenderRequest {
    double bpm = 120;
    std::vector<TempoChange> tempo_changes;
    double end = 0;              // beats, as Song::end
    double consonants = 1.0;
    double start = 0.0;          // beats
    Reverb reverb{0, 0};
    bool anticipate = true;
    std::vector<PartRequest> tracks;
    std::vector<BackingRequest> backings;
    //: where the bars fall, when the metronome is wanted (never in an export)
    std::optional<Meter> metronome;
    Clock clock() const { return {bpm, tempo_changes}; }
};

//: A song mixed in memory, ready to play.
struct Mixed {
    bool ok = false;
    std::string error;
    std::shared_ptr<const std::vector<float>> samples;   //: interleaved when stereo
    int channels = 1;
    double seconds = 0;
    double peak = 0;
    //: nothing was sung: the same song had just been mixed
    bool cached = false;
    bool stopped_short = false;
    //: given up part of the way, as an export can be
    bool cancelled = false;
    //: seconds it begins before the beat it was asked from: what the notes
    //: there sing ahead of it
    double lead = 0;
};

//: The same, written to a sound file: an export.
struct RenderResult {
    bool ok = false;
    std::string error;
    double seconds = 0;
    double peak = 0;
    std::string path;
    bool cached = false;
    bool stopped_short = false;
    //: given up part of the way: nothing was written
    bool cancelled = false;
};

//: The song as the renderer wants it: `parts` are the tracks to sing and
//: nothing else, so mute and solo are settled by whoever calls this.
RenderRequest make_request(const Song &song, const std::vector<const Track *> &parts,
                           double start, const std::map<int, int> &program_map);

class Mixer {
public:
    Mixer();
    //: The song in memory, the metronome on it if the request asks for it.
    //: Only phrases not sung before are sung: the rest come from memory.
    Mixed mix(const RenderRequest &req);
    //: The song written to `out`, a WAV, FLAC or Ogg Vorbis file as its name
    //: says (write_audio): what Export does.
    //: `progress`, if given, is told how far it has got, 0 to 1, on this
    //: thread as it goes, and returns false to give it up: then nothing is
    //: written, and nothing half sung is kept.
    RenderResult render(const RenderRequest &req, const std::string &out,
                        std::function<bool(double)> progress = {});

private:
    //: an export being followed, while render has one
    struct Watch;
    Watch *watch_ = nullptr;
    //: `whole`: an export, which an engine that keeps its own time sings
    //: part by part rather than phrase by phrase
    Mixed mix_locked(const RenderRequest &req, bool whole = false);
    //: One part from `start`: mono, or stereo (`*channels` 2) if an
    //: instrument plays any of it.
    std::vector<float> track(const PartRequest &t, const Clock &clock, double end, double consonants, double start,
                             bool early, bool whole, bool *stopped_short, std::string *error, int *channels);
    void keep(const std::string &key, std::shared_ptr<const std::vector<float>> sung,
              bool stopped_short, double starts_before, int channels = 1);
    //: An export of a song VocalWriter sings every part of, with no part
    //: panned or in a room of its own, sung as VocalWriter played a song:
    //: all the parts at once, through its reverb. False if it is not such a
    //: song, or VocalWriter could not; it is then sung part by part.
    bool sung_together(const RenderRequest &req, std::vector<float> *out, int *channels);
    bool add_backings(const RenderRequest &req, std::vector<float> &out, int &channels, double &peak,
                      std::string *error, long lead = 0);
    //: the mix made, kept as the last one
    Mixed finished(const std::string &key, std::vector<float> out, int channels, double peak, bool stopped,
                   double lead = 0);
    //: a backing track's file, read the first time it is played and kept
    //: until it changes on disk
    std::shared_ptr<const std::vector<float>> backing(const std::string &path, std::string *error);
    std::mutex mu_;

    struct Read {
        long long size, time;
        std::shared_ptr<const std::vector<float>> samples;
        unsigned long long used;
    };
    std::map<std::string, Read> files_;

    //: Every phrase sung, kept in memory under a key made of everything that
    //: decides how it sounds -- the voice, its controls, its level, the notes,
    //: their phonemes and lengths, the bends and the wheel -- so that changing
    //: one note sings one phrase again, not the song. The phrases sung longest
    //: ago go first when the store passes kPhraseBytes.
    struct Kept {
        std::shared_ptr<const std::vector<float>> samples;
        bool stopped_short;
        unsigned long long used;
        double starts_before;
        int channels;
    };
    std::map<std::string, Kept> phrases_;
    size_t phrase_bytes_ = 0;
    unsigned long long clock_ = 0;
    //: the clock when the mix in hand began: what it used since is kept
    unsigned long long mixing_since_ = 0;
    //: the most any one mix has used of the store, in bytes
    size_t biggest_mix_ = 0;
    //: the store back within its room, all but what the last mix used
    void trim();
    //: and the last song mixed, so playing it again does nothing at all
    std::string last_key_;
    Mixed last_;
};

//: One tick of the metronome mixed in, accented at each bar line `meter`
//: puts; `start` is the beat the audio begins on, `lead` seconds into it, so
//: the ticks stay on the song's beats.
void with_metronome(std::vector<float> &y, int channels, const Clock &clock, const Meter &meter,
                    double start, double lead = 0);

}  // namespace svs
