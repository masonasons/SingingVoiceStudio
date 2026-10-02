#include "audio/player.h"

#include "core/paths.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace svs {

#ifdef _WIN32
namespace {
const wchar_t *const kAlias = L"singingvoicestudio";

MCIERROR mci(const std::wstring &command, std::wstring *reply = nullptr) {
    wchar_t buf[256] = {0};
    MCIERROR err = mciSendStringW(command.c_str(), buf, 254, nullptr);
    if (reply) *reply = buf;
    return err;
}
}  // namespace
#endif

Player::~Player() { close(); }

void Player::close() {
#ifdef _WIN32
    if (opened_) {
        mci(std::wstring(L"close ") + kAlias);
        opened_ = false;
    }
#endif
}

bool Player::play(const std::string &path) {
    if (!file_exists(path)) return false;
#ifdef _WIN32
    close();
    // the quotes matter: a path with a space in it is otherwise read as
    // several arguments
    if (mci(L"open \"" + widen(path) + L"\" type waveaudio alias " + kAlias)) return false;
    opened_ = true;
    return mci(std::wstring(L"play ") + kAlias + L" from 0") == 0;
#else
    return false;
#endif
}

void Player::stop() {
#ifdef _WIN32
    if (opened_) {
        mci(std::wstring(L"stop ") + kAlias);
        close();
    }
#endif
}

bool Player::playing() {
#ifdef _WIN32
    if (!opened_) return false;
    std::wstring mode;
    if (mci(std::wstring(L"status ") + kAlias + L" mode", &mode) || mode != L"playing") {
        close();
        return false;
    }
    return true;
#else
    return false;
#endif
}

}  // namespace svs
