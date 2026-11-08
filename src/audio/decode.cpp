#include "audio/decode.h"

#include <cstring>

#include "core/paths.h"
#include "miniaudio.h"
#include "voices/singer.h"

namespace svs {

const char *const kAudioWildcard =
    "Sound files (*.wav;*.mp3;*.flac;*.ogg)|*.wav;*.mp3;*.flac;*.ogg|All files (*.*)|*.*";

bool is_audio_file(const std::string &path) {
    std::string e = extension_lower(path);
    return e == ".wav" || e == ".mp3" || e == ".flac" || e == ".ogg";
}

namespace {

//: Whether an MPEG audio frame's header is at `at`: the sync, and a version,
//: layer, bit rate and sample rate that are not reserved.
bool mpeg_frame(const std::vector<unsigned char> &b, size_t at) {
    if (at + 4 > b.size() || b[at] != 0xFF || (b[at + 1] & 0xE0) != 0xE0) return false;
    return ((b[at + 1] >> 3) & 3) != 1 && ((b[at + 1] >> 1) & 3) != 0 && (b[at + 2] >> 4) != 15 &&
           ((b[at + 2] >> 2) & 3) != 3;
}

//: Whether `b` begins as a WAV, FLAC, MP3 or Ogg file does, and the decoder
//: for it. This is not left to the decoders: MP3's looks through anything
//: for a frame, and now and then finds one in a program.
bool sound_kind(const std::vector<unsigned char> &b, ma_encoding_format *kind) {
    auto at = [&](size_t i, const char *magic) {
        size_t n = std::strlen(magic);
        return i + n <= b.size() && std::memcmp(b.data() + i, magic, n) == 0;
    };
    *kind = ma_encoding_format_unknown;
    if (((at(0, "RIFF") || at(0, "RF64")) && at(8, "WAVE")) || (at(0, "riff") && at(24, "wave"))) {
        *kind = ma_encoding_format_wav;
        return true;
    }
    if (at(0, "fLaC")) {
        *kind = ma_encoding_format_flac;
        return true;
    }
    // Vorbis, or FLAC in an Ogg file: the decoders tell which
    if (at(0, "OggS")) return true;
    // an MP3's first frame, after any ID3 tag and its padding
    size_t start = 0;
    if (at(0, "ID3") && b.size() >= 10) {
        start = 10 + ((size_t(b[6] & 0x7F) << 21) | (size_t(b[7] & 0x7F) << 14) | (size_t(b[8] & 0x7F) << 7) |
                      size_t(b[9] & 0x7F));
        if (b[5] & 0x10) start += 10;
        while (start < b.size() && b[start] == 0) ++start;
    }
    if (!mpeg_frame(b, start)) return false;
    *kind = ma_encoding_format_mp3;
    return true;
}

//: The decoder for `bytes`, if they are a sound file (sound_kind).
bool open_decoder(const std::vector<unsigned char> &bytes, ma_decoder_config config, ma_decoder *decoder) {
    if (!sound_kind(bytes, &config.encodingFormat)) return false;
    return ma_decoder_init_memory(bytes.data(), bytes.size(), &config, decoder) == MA_SUCCESS;
}

}  // namespace

// The file is read into memory first and decoded from there: the decoders'
// own ways of opening a file do not all take a name outside ASCII on Windows.
bool decode_audio(const std::string &path, std::vector<float> *stereo, std::string *error) {
    stereo->clear();
    std::vector<unsigned char> bytes;
    if (!read_file(path, &bytes) || bytes.empty()) {
        *error = file_exists(path) ? "cannot read " + path : "there is no file " + path;
        return false;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, ma_uint32(kSampleRate));
    // the steepest filter the resampler has, for a file at 48 kHz or 22 kHz
    config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    ma_decoder decoder;
    if (!open_decoder(bytes, config, &decoder)) {
        *error = base_name(path) + " is not a WAV, MP3, FLAC or Ogg Vorbis file this program can read";
        return false;
    }
    const ma_uint64 chunk = 65536;
    std::vector<float> buf(size_t(chunk) * 2);
    while (true) {
        ma_uint64 got = 0;
        ma_result r = ma_decoder_read_pcm_frames(&decoder, buf.data(), chunk, &got);
        stereo->insert(stereo->end(), buf.begin(), buf.begin() + long(got) * 2);
        if (r != MA_SUCCESS || got < chunk) break;
    }
    ma_decoder_uninit(&decoder);
    if (stereo->empty()) {
        *error = base_name(path) + " has no sound in it";
        return false;
    }
    return true;
}

bool probe_audio(const std::string &path, std::string *error) {
    std::vector<unsigned char> bytes;
    if (!read_file(path, &bytes) || bytes.empty()) {
        *error = file_exists(path) ? "it cannot be read" : "it is not there";
        return false;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 2, ma_uint32(kSampleRate));
    ma_decoder decoder;
    if (!open_decoder(bytes, config, &decoder)) {
        *error = "it is not a WAV, MP3, FLAC or Ogg Vorbis file this program can read";
        return false;
    }
    // a header with nothing after it decodes to no sound at all
    std::vector<float> buf(4096 * 2);
    ma_uint64 got = 0;
    ma_decoder_read_pcm_frames(&decoder, buf.data(), 4096, &got);
    ma_decoder_uninit(&decoder);
    if (got == 0) {
        *error = "there is no sound in it";
        return false;
    }
    return true;
}

}  // namespace svs
