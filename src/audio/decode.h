// decode.h -- a sound file read whole, for a backing track.
#pragma once

#include <string>
#include <vector>

namespace svs {

//: The sound files a backing track can play, for a file dialog.
extern const char *const kAudioWildcard;
//: whether the file is one of those, by its name
bool is_audio_file(const std::string &path);

//: WAV, MP3, FLAC or Ogg Vorbis, read into memory as interleaved stereo
//: floats at the program's own rate (a mono file is put in both channels).
//: Returns false, with `error` saying why, on anything else.
bool decode_audio(const std::string &path, std::vector<float> *stereo, std::string *error);
//: Whether decode_audio would read the file, without reading it all: its
//: opening is decoded and the rest left. `error` says why not, as a reason
//: to follow the file's name.
bool probe_audio(const std::string &path, std::string *error);

}  // namespace svs
