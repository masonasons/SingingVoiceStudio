// registry.h -- every voice of every engine, in one list.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/project.h"
#include "voices/family.h"

namespace svs {

struct VoiceListing {
    int id;
    std::string name;
    std::string family;
    bool available;
};

class Registry {
public:
    static Registry &get();

    //: Every voice, VocalWriter's first in the bank's own order, then
    //: DECtalk's, the SSI-263's and Microsoft's, and the SoundFonts' last.
    //: Fixed numbers; see family.h.
    std::vector<VoiceListing> voices();
    Family *family_of(int voice_id);
    std::string voice_name(int voice_id);
    std::unique_ptr<Singer> singer(int voice_id, std::string *err);
    double headroom(int voice_id);
    //: one line per engine: "DECtalk: ready" or why not
    std::vector<std::string> status();

private:
    Registry();
    //: the SoundFonts' families made again if set_soundfonts has been called
    void soundfonts_locked();
    std::mutex mu_;
    std::vector<std::unique_ptr<Family>> families_;
    //: the families of SoundFonts no longer loaded, kept for whatever may
    //: still hold one
    std::vector<std::unique_ptr<Family>> retired_;
    int soundfonts_seen_ = -1;
    //: the families before the SoundFonts', which are always there
    static constexpr size_t kBuiltInFamilies = 7;
    //: each SoundFont family's first voice, in order after those
    std::vector<int> soundfont_bases_;
    std::vector<VoiceListing> cache_;
    bool listed_ = false;
};

//: A song's tracks put back on the voices their saved names say, where the
//: numbers have moved since the song was written. A track whose name is not
//: here at all keeps its number and says so when it is sung.
//: True if any track moved. `voices` is Registry::voices(), or the copy of it
//: the window keeps (the window never asks the engines anything itself).
bool resolve_voice_names(Song &song, const std::vector<VoiceListing> &voices);
//: The name a track should be saved with for this voice: none for
//: VocalWriter's own, which stay just a number as VocalWriter Studio wrote them.
std::optional<std::string> saved_voice_name(int voice_id, const std::vector<VoiceListing> &voices);
//: Every voice of the song's tracks and controllers without a name given its
//: name (saved_voice_name): a song saved with them keeps its voices if their
//: numbers move, as a SoundFont's do from one machine to another.
void name_voices(Song &song, const std::vector<VoiceListing> &voices);

}  // namespace svs
