// wav.h -- 16-bit PCM WAV files, mono or stereo.
#pragma once

#include <string>
#include <vector>

namespace svs {

//: `samples` are interleaved when `channels` is 2. Floats are clipped to
//: +-1 and scaled by 32767, as the original program wrote them.
bool write_wav(const std::string &path, const std::vector<float> &samples,
               int channels, int rate = 44100);

//: Reads a 16-bit PCM WAV back as floats (/32768). Returns false on anything
//: else.
bool read_wav(const std::string &path, std::vector<float> *samples, int *channels,
              int *rate);

}  // namespace svs
