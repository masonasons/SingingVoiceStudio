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
                           "Kit",  "Ursula", "Rita", "Wendy"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 9; ++i) v.push_back({kDectalkBase + i, std::string("DECtalk ") + names[i]});
    return std::make_unique<Absent>("DECtalk", v);
}
#endif

#ifndef SVS_HAVE_SSI263
std::unique_ptr<Family> make_ssi263_family() {
    return std::make_unique<Absent>("SSI-263", std::vector<VoiceEntry>{{kSsi263Base, "SSI-263"}});
}
#endif

#ifndef SVS_HAVE_MACINTALK
std::unique_ptr<Family> make_macintalk_family() {
    return std::make_unique<Absent>("MacinTalk", std::vector<VoiceEntry>{});
}
void set_macintalk_folder(const std::string &) {}
#endif

#ifndef SVS_HAVE_SHARPVOX
std::unique_ptr<Family> make_sharpvox_family() {
    const char *names[] = {"Paul", "Beth", "Chris", "Deborah", "Jack", "Jess",
                           "John", "Matt", "Pirate", "Tommy", "Whisper"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 11; ++i) v.push_back({kSharpVoxBase + i, std::string("SharpVox ") + names[i]});
    return std::make_unique<Absent>("SharpVox", v);
}
#endif

#ifndef SVS_HAVE_BESTSPEECH
std::unique_ptr<Family> make_bestspeech_family() {
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 6; ++i) v.push_back({kBestBase + i, "BeSTspeech " + std::to_string(i + 1)});
    return std::make_unique<Absent>("BeSTspeech", v);
}
#endif

#ifndef SVS_HAVE_ELOQUENCE
std::unique_ptr<Family> make_eloquence_family() {
    const char *names[] = {"Reed", "Shelley", "Bobby", "Rocko", "Glen", "Sandy", "Grandma", "Grandpa"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 8; ++i) v.push_back({kEloquenceBase + i, std::string("Eloquence ") + names[i]});
    return std::make_unique<Absent>("Eloquence", v);
}
#endif

#ifndef SVS_HAVE_PROSE
std::unique_ptr<Family> make_prose_family() {
    const char *names[] = {"Standard", "Deep", "High"};
    std::vector<VoiceEntry> v;
    for (int i = 0; i < 3; ++i) v.push_back({kProseBase + i, std::string("Prose 2000 ") + names[i]});
    return std::make_unique<Absent>("Prose 2000", v);
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
