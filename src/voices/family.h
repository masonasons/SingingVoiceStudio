// family.h -- one synthesiser and the voices it offers.
//
// Every voice in the program has a number that is saved with the song, so the
// numbers are fixed per engine and never depend on what happens to be
// installed: a song written with DECtalk's Betty still says Betty on a machine
// without DECtalk, and says why it cannot sing.
//
//      0 -  999   VocalWriter's voice bank, in the bank's own order
//   1000 - 1999   DECtalk
//   2000 - 2999   the SSI-263
//   3000 - 3999   Microsoft Sam, Mike and Mary, and the SAPI 4 voice modes
//   4000 - 4999   MacinTalk: Apple's voices from Mac OS X 10.4 to 10.7, a
//                 hundred numbers to each generation
//   5000 - 5999   the GM Soundbank: VocalWriter's own General MIDI instruments
//                 (GMBank), 5000 + the GM program, and 5128 the drum kit
//   6000 -        the SoundFonts loaded in the Soundbank manager, a thousand
//                 numbers to each (soundfont.h)
//
// The number is not the only thing a song keeps: a voice outside
// VocalWriter's bank is saved with its name as well (see Track::voice_name),
// and the name wins if the two ever disagree.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "voices/singer.h"

namespace svs {

enum : int {
    kVocalWriterBase = 0,
    kDectalkBase = 1000,
    kSsi263Base = 2000,
    kMicrosoftBase = 3000,
    kMacinTalkBase = 4000,
    kGmBase = 5000,
    kSoundFontBase = 6000,
    kFamilySpan = 1000,
};

//: The GM Soundbank's drum kit: General MIDI's channel 10, each key a drum.
constexpr int kGmDrums = kGmBase + 128;

//: Whether a voice is one of VocalWriter's: the only engine a velocity means
//: more than loudness to, which every engine has as the part's volume.
inline bool is_vocalwriter_voice(int id) { return id >= kVocalWriterBase && id < kVocalWriterBase + kFamilySpan; }
//: Whether a voice is one of the GM Soundbank's instruments, which play the
//: notes, chords and all, rather than sing them.
inline bool is_gm_voice(int id) { return id >= kGmBase && id < kGmBase + kFamilySpan; }
//: Whether a voice is one of a SoundFont's (soundfont.h).
inline bool is_soundfont_voice(int id) { return id >= kSoundFontBase; }
//: Whether a voice is an instrument, the GM Soundbank's or a SoundFont's,
//: which plays the notes, chords and all, rather than sings them.
inline bool is_instrument(int id) { return is_gm_voice(id) || is_soundfont_voice(id); }
//: Whether a voice is a drum kit, each key a drum: the GM Soundbank's, or a
//: SoundFont's preset in its percussion bank (soundfont.h).
bool is_drum_kit(int id);
//: Whether a voice hears velocity as more than loudness: VocalWriter's and
//: the instruments do.
inline bool uses_velocity(int id) { return is_vocalwriter_voice(id) || is_instrument(id); }
//: Whether an engine follows VocalWriter's volume control itself; for the
//: others the mix does.
inline bool sings_volume(int id) { return is_vocalwriter_voice(id) || is_gm_voice(id); }
//: The General MIDI name of a program, 0-127: "Acoustic Grand Piano".
const char *gm_program_name(int program);
//: The General MIDI name of the drum on a key, 35 to 81, or null.
const char *gm_drum_name(int key);

struct VoiceEntry {
    int id = 0;
    std::string name;    //: as the voice list shows it, e.g. "DECtalk Betty"
    //: The engine as the Engine box shows it, when one family is several
    //: there -- "MacinTalk Leopard" -- or empty for the family's own name.
    //: A voice's name starts with its group's, so the Voice box can leave
    //: it off ("Fred" beside "MacinTalk Leopard").
    std::string group;
    //: false for a voice whose data is missing although its family's is not
    //: (one Mac OS X generation extracted and not another)
    bool installed = true;
};

class Family {
public:
    virtual ~Family() = default;
    //: "VocalWriter", "DECtalk", "SSI-263", "Microsoft"
    virtual std::string name() const = 0;
    //: The voices, always the same list, installed or not.
    virtual std::vector<VoiceEntry> voices() = 0;
    //: Whether it can sing at all; if not, `why` says what is missing in a
    //: sentence that can be read out ("DECtalk's dictionary, dtalk_us.dic,
    //: is not in voices\dectalk").
    virtual bool available(std::string *why) = 0;
    //: A singer for one of its voices, or null with `err` set. A singer is
    //: used from one thread at a time; the renderer makes one per phrase or
    //: keeps one per part, as it likes.
    virtual std::unique_ptr<Singer> singer(int voice_id, std::string *err) = 0;
    //: How loud a voice comes out at full level, as a factor to multiply by
    //: so that it does not clip. 1.0 leaves it alone. The default measures
    //: nothing; an engine with very uneven voices overrides it.
    virtual double headroom(int /*voice_id*/) { return 1.0; }
    //: Whether it keeps its own time, as VocalWriter's sequencer does: it
    //: places the consonants before the beat itself, and an export is sung
    //: as whole parts rather than phrases.
    virtual bool sings_parts() const { return false; }
    //: Whether it plays rather than sings, as the GM Soundbank does: chords
    //: and all, and always the whole part.
    virtual bool plays() const { return false; }
};

std::unique_ptr<Family> make_vocalwriter_family();
std::unique_ptr<Family> make_dectalk_family();
std::unique_ptr<Family> make_ssi263_family();
std::unique_ptr<Family> make_microsoft_family();
std::unique_ptr<Family> make_macintalk_family();
std::unique_ptr<Family> make_gm_family();

//: Where Panthera is, as the user has named it in the settings: NVDA's
//: configuration folder (with Panthera's add-on and its macintalk folder in
//: it), or a folder holding panthera_host.exe and the tiger, leopard,
//: snowleopard and lion folders. It is looked in before anywhere else; empty
//: is only the usual places. MacinTalk looks again the next time it is asked.
void set_macintalk_folder(const std::string &folder);

}  // namespace svs
