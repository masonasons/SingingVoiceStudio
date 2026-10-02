// player_mac.mm -- the Player on macOS: an AVAudioPlayer, which can be asked
// whether it is still going, so one key can start the song and stop it.
#include "audio/player.h"

#include "core/paths.h"

#import <AVFoundation/AVFoundation.h>

namespace svs {

namespace {
AVAudioPlayer *player_of(void *native) { return (AVAudioPlayer *)native; }
}  // namespace

Player::~Player() { close(); }

void Player::close() {
    if (native_) {
        AVAudioPlayer *p = player_of(native_);
        [p stop];
        [p release];
        native_ = nullptr;
    }
    opened_ = false;
}

bool Player::play(const std::string &path) {
    if (!file_exists(path)) return false;
    close();
    NSString *file = [NSString stringWithUTF8String:path.c_str()];
    if (!file) return false;
    NSURL *url = [NSURL fileURLWithPath:file];
    NSError *error = nil;
    AVAudioPlayer *p = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
    if (!p || error) {
        [p release];
        return false;
    }
    native_ = p;
    opened_ = true;
    [p setCurrentTime:0];
    return [p play] ? true : false;
}

void Player::stop() {
    if (opened_) close();
}

bool Player::playing() {
    if (!opened_ || !native_) return false;
    if (![player_of(native_) isPlaying]) {
        close();
        return false;
    }
    return true;
}

}  // namespace svs
