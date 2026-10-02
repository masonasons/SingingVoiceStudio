#include "audio/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/paths.h"

namespace svs {

static FILE *open_file(const std::string &path, const char *mode) {
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

static void put32(std::vector<unsigned char> &b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back((unsigned char)(v >> (8 * i)));
}
static void put16(std::vector<unsigned char> &b, uint16_t v) {
    b.push_back((unsigned char)v);
    b.push_back((unsigned char)(v >> 8));
}

bool write_wav(const std::string &path, const std::vector<float> &samples, int channels,
               int rate) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) make_dirs(path.substr(0, slash));
    std::vector<unsigned char> b;
    uint32_t data = uint32_t(samples.size() * 2);
    b.insert(b.end(), {'R', 'I', 'F', 'F'});
    put32(b, 36 + data);
    b.insert(b.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(b, 16);
    put16(b, 1);
    put16(b, uint16_t(channels));
    put32(b, uint32_t(rate));
    put32(b, uint32_t(rate * channels * 2));
    put16(b, uint16_t(channels * 2));
    put16(b, 16);
    b.insert(b.end(), {'d', 'a', 't', 'a'});
    put32(b, data);
    b.reserve(b.size() + data);
    for (float s : samples) {
        float c = std::max(-1.0f, std::min(1.0f, s));
        // as numpy's astype('<i2') does: truncation toward zero
        int16_t v = int16_t(c * 32767.0f);
        put16(b, uint16_t(v));
    }
    FILE *f = open_file(path, "wb");
    if (!f) return false;
    bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    ok = (fclose(f) == 0) && ok;
    return ok;
}

bool read_wav(const std::string &path, std::vector<float> *samples, int *channels,
              int *rate) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b) || b.size() < 44) return false;
    if (memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4)) return false;
    size_t pos = 12;
    int ch = 0, sr = 0, bits = 0;
    while (pos + 8 <= b.size()) {
        uint32_t len = b[pos + 4] | b[pos + 5] << 8 | b[pos + 6] << 16 | uint32_t(b[pos + 7]) << 24;
        const unsigned char *d = b.data() + pos + 8;
        if (!memcmp(b.data() + pos, "fmt ", 4) && len >= 16) {
            ch = d[2] | d[3] << 8;
            sr = int(d[4] | d[5] << 8 | d[6] << 16 | uint32_t(d[7]) << 24);
            bits = d[14] | d[15] << 8;
        } else if (!memcmp(b.data() + pos, "data", 4)) {
            if (bits != 16 || ch <= 0) return false;
            size_t n = std::min<size_t>(len, b.size() - pos - 8) / 2;
            samples->resize(n);
            for (size_t i = 0; i < n; ++i)
                (*samples)[i] = int16_t(d[2 * i] | d[2 * i + 1] << 8) / 32768.0f;
            *channels = ch;
            *rate = sr;
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    return false;
}

}  // namespace svs
