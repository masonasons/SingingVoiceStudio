// encode.h -- a song written out as a sound file: WAV, FLAC or Ogg Vorbis,
// whichever the file's name asks for.
#pragma once

#include <string>
#include <vector>

namespace svs {

//: The kinds Export audio offers, in the order of their extensions below.
extern const char *const kExportWildcard;
extern const char *const kExportExtensions[];
extern const int kExportKinds;

//: `samples` as write_wav takes them, written as the extension of `path`
//: says: ".flac" lossless, the same 16-bit samples a WAV holds; ".ogg" Ogg
//: Vorbis at quality 6; anything else a WAV. False, with `error` set, if it
//: could not be written.
bool write_audio(const std::string &path, const std::vector<float> &samples, int channels,
                 std::string *error = nullptr);

}  // namespace svs
