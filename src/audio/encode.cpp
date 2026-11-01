#include "audio/encode.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <FLAC/stream_encoder.h>
#include <vorbis/vorbisenc.h>

#include "audio/wav.h"
#include "core/paths.h"
#include "voices/singer.h"

namespace svs {

const char *const kExportWildcard =
    "WAV files (*.wav)|*.wav|FLAC files (*.flac)|*.flac|Ogg Vorbis files (*.ogg)|*.ogg";
const char *const kExportExtensions[] = {".wav", ".flac", ".ogg"};
const int kExportKinds = 3;

namespace {

//: Vorbis' quality, -0.1 to 1: 0.6 is oggenc's -q 6, about 190 kbit/s stereo.
const float kVorbisQuality = 0.6f;
//: frames handed to an encoder at a time
const size_t kChunk = 4096;

FILE *open_file(const std::string &path, const char *mode) {
    size_t slash = path.find_last_of("/\\");
    if (slash != std::string::npos) make_dirs(path.substr(0, slash));
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

//: a sample as write_wav writes it, so a FLAC holds what the WAV would
int16_t pcm16(float s) {
    float c = std::max(-32768.0f / 32767.0f, std::min(1.0f, s));
    return int16_t(c * 32767.0f);
}

FLAC__StreamEncoderWriteStatus flac_write(const FLAC__StreamEncoder *, const FLAC__byte buffer[], size_t bytes,
                                          uint32_t, uint32_t, void *f) {
    return fwrite(buffer, 1, bytes, static_cast<FILE *>(f)) == bytes ? FLAC__STREAM_ENCODER_WRITE_STATUS_OK
                                                                     : FLAC__STREAM_ENCODER_WRITE_STATUS_FATAL_ERROR;
}

// the seek and tell let the encoder write the lengths into the header at the end
FLAC__StreamEncoderSeekStatus flac_seek(const FLAC__StreamEncoder *, FLAC__uint64 at, void *f) {
#ifdef _WIN32
    int rc = _fseeki64(static_cast<FILE *>(f), (__int64)at, SEEK_SET);
#else
    int rc = fseeko(static_cast<FILE *>(f), (off_t)at, SEEK_SET);
#endif
    return rc == 0 ? FLAC__STREAM_ENCODER_SEEK_STATUS_OK : FLAC__STREAM_ENCODER_SEEK_STATUS_ERROR;
}

FLAC__StreamEncoderTellStatus flac_tell(const FLAC__StreamEncoder *, FLAC__uint64 *at, void *f) {
#ifdef _WIN32
    __int64 pos = _ftelli64(static_cast<FILE *>(f));
#else
    off_t pos = ftello(static_cast<FILE *>(f));
#endif
    if (pos < 0) return FLAC__STREAM_ENCODER_TELL_STATUS_ERROR;
    *at = FLAC__uint64(pos);
    return FLAC__STREAM_ENCODER_TELL_STATUS_OK;
}

bool write_flac(FILE *f, const std::vector<float> &samples, int channels) {
    FLAC__StreamEncoder *enc = FLAC__stream_encoder_new();
    if (!enc) return false;
    size_t frames = samples.size() / size_t(channels);
    bool ok = FLAC__stream_encoder_set_channels(enc, uint32_t(channels)) &&
              FLAC__stream_encoder_set_bits_per_sample(enc, 16) &&
              FLAC__stream_encoder_set_sample_rate(enc, kSampleRate) &&
              FLAC__stream_encoder_set_compression_level(enc, 5) &&
              FLAC__stream_encoder_set_total_samples_estimate(enc, frames) &&
              FLAC__stream_encoder_init_stream(enc, flac_write, flac_seek, flac_tell, nullptr, f) ==
                  FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    std::vector<FLAC__int32> buf;
    for (size_t at = 0; ok && at < frames; at += kChunk) {
        size_t n = std::min(kChunk, frames - at);
        buf.resize(n * size_t(channels));
        for (size_t i = 0; i < buf.size(); ++i) buf[i] = pcm16(samples[at * size_t(channels) + i]);
        ok = FLAC__stream_encoder_process_interleaved(enc, buf.data(), uint32_t(n));
    }
    // finished whatever happened, so the encoder lets go of what it holds
    ok = FLAC__stream_encoder_finish(enc) && ok;
    FLAC__stream_encoder_delete(enc);
    return ok;
}

bool write_vorbis(FILE *f, const std::vector<float> &samples, int channels) {
    vorbis_info vi;
    vorbis_info_init(&vi);
    if (vorbis_encode_init_vbr(&vi, channels, kSampleRate, kVorbisQuality) != 0) {
        vorbis_info_clear(&vi);
        return false;
    }
    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "Singing Voice Studio");
    vorbis_dsp_state vd;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block vb;
    vorbis_block_init(&vd, &vb);
    ogg_stream_state os;
    ogg_stream_init(&os, int(std::time(nullptr) & 0x7fffffff));

    bool ok = true;
    ogg_page page;
    auto put = [&]() {
        ok = ok && fwrite(page.header, 1, size_t(page.header_len), f) == size_t(page.header_len) &&
             fwrite(page.body, 1, size_t(page.body_len), f) == size_t(page.body_len);
    };
    // the three headers, on pages of their own as the format wants
    ogg_packet id, comments, books;
    vorbis_analysis_headerout(&vd, &vc, &id, &comments, &books);
    ogg_stream_packetin(&os, &id);
    ogg_stream_packetin(&os, &comments);
    ogg_stream_packetin(&os, &books);
    while (ogg_stream_flush(&os, &page)) put();

    size_t frames = samples.size() / size_t(channels);
    for (size_t at = 0; ok;) {
        size_t n = std::min(kChunk, frames - at);
        if (n) {
            float **buf = vorbis_analysis_buffer(&vd, int(n));
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < channels; ++c)
                    buf[c][i] = std::max(-1.0f, std::min(1.0f, samples[(at + i) * size_t(channels) + size_t(c)]));
            at += n;
        }
        // nothing written is the end of the stream
        vorbis_analysis_wrote(&vd, int(n));
        while (ok && vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);
            ogg_packet packet;
            while (ok && vorbis_bitrate_flushpacket(&vd, &packet)) {
                ogg_stream_packetin(&os, &packet);
                while (ogg_stream_pageout(&os, &page)) put();
            }
        }
        if (!n) break;
    }
    while (ogg_stream_flush(&os, &page)) put();

    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    return ok;
}

}  // namespace

bool write_audio(const std::string &path, const std::vector<float> &samples, int channels, std::string *error) {
    std::string ext = extension_lower(path);
    bool flac = ext == ".flac", ogg = ext == ".ogg";
    bool ok;
    if (!flac && !ogg) {
        ok = write_wav(path, samples, channels);
    } else {
        // read back as well: the FLAC encoder goes back to write the lengths
        FILE *f = open_file(path, "w+b");
        ok = f && (flac ? write_flac(f, samples, channels) : write_vorbis(f, samples, channels));
        if (f) ok = fclose(f) == 0 && ok;
    }
    if (!ok && error) *error = "could not write " + path;
    return ok;
}

}  // namespace svs
