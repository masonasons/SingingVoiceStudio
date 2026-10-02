// Stand-ins for the engines a build was made without. Their voices are still
// listed, under the same numbers, so a song that uses them opens and says
// what it would be sung with; it just cannot be sung here.
#include <memory>

#include "voices/family.h"

namespace svs {

namespace {

class Absent : public Family {
public:
    Absent(std::string name, std::vector<VoiceEntry> voices)
        : name_(std::move(name)), voices_(std::move(voices)) {}
    std::string name() const override { return name_; }
    std::vector<VoiceEntry> voices() override { return voices_; }
    bool available(std::string *why) override {
        if (why) *why = name_ + " was not built into this copy of the program";
        return false;
    }
    std::unique_ptr<Singer> singer(int, std::string *err) override {
        available(err);
        return nullptr;
    }

private:
    std::string name_;
    std::vector<VoiceEntry> voices_;
};

}  // namespace

#ifndef SVS_HAVE_DECTALK
std::unique_ptr<Family> make_dectalk_family() {
    const char *names[] = {"Paul", "Betty", "Harry", "Frank", "Dennis",
                           "Kit",  "Ursula", "Rita", "Wendy", "Val"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 10; ++i) v.push_back({kDectalkBase + i, std::string("DECtalk ") + names[i]});
    return std::make_unique<Absent>("DECtalk", v);
}
#endif

#ifndef SVS_HAVE_SSI263
std::unique_ptr<Family> make_ssi263_family() {
    return std::make_unique<Absent>("SSI-263", std::vector<VoiceEntry>{{kSsi263Base, "SSI-263"}});
}
#endif

#ifndef SVS_HAVE_MICROSOFT
std::unique_ptr<Family> make_microsoft_family() {
    const char *names[] = {"Sam", "Mike", "Mary"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 3; ++i) v.push_back({kMicrosoftBase + i * 20, std::string("Microsoft ") + names[i]});
    return std::make_unique<Absent>("Microsoft", v);
}
#endif

}  // namespace svs
