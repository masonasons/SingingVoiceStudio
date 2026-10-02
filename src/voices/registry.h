// registry.h -- every voice of every engine, in one list.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    //: DECtalk's, the SSI-263's and Microsoft's. Fixed numbers; see family.h.
    std::vector<VoiceListing> voices();
    Family *family_of(int voice_id);
    std::string voice_name(int voice_id);
    std::unique_ptr<Singer> singer(int voice_id, std::string *err);
    double headroom(int voice_id);
    //: one line per engine: "DECtalk: ready" or why not
    std::vector<std::string> status();

private:
    Registry();
    std::mutex mu_;
    std::vector<std::unique_ptr<Family>> families_;
    std::vector<VoiceListing> cache_;
    bool listed_ = false;
};

}  // namespace svs
