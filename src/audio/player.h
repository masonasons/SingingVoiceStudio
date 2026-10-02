// player.h -- playing a WAV, and knowing whether it is still playing.
//
// That second part is why this exists: without it one key cannot both start
// the song and stop it. Windows' media control interface answers the
// question; it is spoken to by sending it strings, which is as odd as it
// sounds, but it needs nothing installed (player.cpp). On macOS an
// AVAudioPlayer does the same (player_mac.mm).
#pragma once

#include <string>

namespace svs {

class Player {
public:
    ~Player();
    //: Start from the beginning. True if it is playing.
    bool play(const std::string &path);
    void stop();
    //: Whether a sound is playing. A file that has finished is closed here,
    //: since Windows will not let the next render overwrite an open file.
    bool playing();

private:
    void close();
    bool opened_ = false;
    void *native_ = nullptr;   //: macOS: the AVAudioPlayer, retained
};

}  // namespace svs
