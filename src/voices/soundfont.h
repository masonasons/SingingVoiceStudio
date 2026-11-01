// soundfont.h -- SoundFonts (.sf2), each an engine of its own, played by
// TinySoundFont.
//
// A SoundFont loaded in the Soundbank manager is a family named after its
// file ("SGM-v2.01.sf2" is "SGM-v2.01"), with the thousand voice numbers of
// its slot: 101000 - 101999 for the first slot, 102000 - 102999 for the next
// and so on. Its presets are numbered in the order of their bank and program,
// so the same file numbers them alike every time; only the first thousand are
// used.
// A preset in bank 128, a SoundFont's percussion bank, is a drum kit, as is
// one in bank 120 or above whose name says it is a kit, as some SoundFonts
// keep them there.
//
// The list of presets is read from the file when it is loaded; the samples
// only when one of its voices first plays.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/project.h"
#include "core/settings.h"
#include "voices/family.h"

namespace svs {

//: The most presets a SoundFont has voices for: its thousand numbers.
constexpr int kSoundFontPresets = kFamilySpan;

struct SoundFontPreset {
    int bank = 0;
    int program = 0;
    bool drums = false;
    //: the preset's name, made unique within the SoundFont
    std::string name;
};

struct SoundFontInfo {
    std::string path;
    int slot = 0;
    //: the family's name: the file's, without its extension
    std::string name;
    //: the first kSoundFontPresets, in order of bank and program
    std::vector<SoundFontPreset> presets;
    //: how many presets the file has, used or not
    size_t total = 0;
    //: why the file cannot be read, or empty
    std::string error;
};

//: The first voice number of a slot.
inline int soundfont_base(int slot) { return kSoundFontBase + slot * kFamilySpan; }

//: A SoundFont's presets, read from its file; `error` set if it is not one.
SoundFontInfo read_soundfont(const std::string &path, int slot);

//: The SoundFonts as the settings have them. Each file is read again, and the
//: registry makes a family of each the next time it is asked.
void set_soundfonts(const std::vector<SoundFontFile> &files);
//: The SoundFonts set, as read.
std::vector<SoundFontInfo> soundfonts();
//: Counts every set_soundfonts, for the registry to see a change by.
int soundfonts_version();

//: The preset a voice plays, if it is a SoundFont's that is loaded.
std::optional<SoundFontPreset> soundfont_preset(int voice);
//: The slot of the SoundFont at `path`, or -1 if none is loaded from there.
int soundfont_slot(const std::string &path);

//: The voice the SoundFont in `slot` plays one of the GM Soundbank's with:
//: the preset in bank 0 with the same program, or failing that in another
//: melodic bank; for the drum kit, the kit in its percussion bank at program
//: `drum_program` or 0, or failing that any of its kits. The GM Soundbank's
//: own voice if the SoundFont has nothing for it.
int soundfont_voice_for(int gm_voice, int slot, int drum_program = 0);
//: An imported part played by the SoundFont in `slot` rather than the GM
//: Soundbank: its voice, and every voice its controllers change to.
void play_on_soundfont(std::optional<int> &voice, std::vector<Note> &notes, int slot);

std::unique_ptr<Family> make_soundfont_family(const SoundFontInfo &info);
//: A family of make_soundfont_family no longer loaded: it lets go of its
//: samples once nothing plays them, and says it was removed.
void retire_soundfont_family(Family *family);

}  // namespace svs
