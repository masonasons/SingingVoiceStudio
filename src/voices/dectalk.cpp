// dectalk.cpp -- DECtalk's ten voices, singing.
//
// The synthesiser is DECtalk 5 as published in third_party/dectalk, built
// with Microsoft's compiler into DECtalk.dll (scripts/build_dectalk.ps1) on
// Windows, and with clang into libtts.dylib (scripts/build_dectalk_mac.sh) on
// macOS. The rest of the program is built with GCC on Windows, so the DLL is
// not linked: it is loaded when first needed from voices\dectalk under the
// data roots (see core/paths.h), or from beside the program (on macOS, from
// the bundle's Frameworks folder), with the US English dictionary
// dtalk_us.dic in voices\dectalk. The dictionary is only for reading text,
// which nothing here does, but DECtalk will not start without it.
//
// DECtalk sings from phonemes: in its phoneme mode every phoneme can carry a
// length and a pitch, "aa<400,262>". Everything below follows from how the
// engine treats those two numbers, measured on this build:
//
//   Lengths are counted in frames of 71 samples at 11025 Hz (6.44 ms; the
//   engine's own arithmetic says 6.4). A length of n ms becomes
//   ((n + 4) * 10) >> 6 frames, and the phonemes follow one another frame
//   for frame, so asking for the right number of frames for each puts every
//   phoneme where it belongs, to the nearest frame, however long the phrase.
//
//   A pitch of 38 or more is a frequency in whole hertz. The pitch moves in a
//   straight line from the previous phoneme's target to this one's over the
//   phoneme and then holds. (1 to 37 are notes of DECtalk's own scale, with a
//   built-in vibrato of a few hertz; a clause cannot mix the two, and the
//   scale is not used.) What comes out is not exactly the hertz asked for:
//   the period is kept in steps of a quarter of a 10 kHz sample, truncated,
//   then rescaled to 11025 Hz and rounded to quarter samples again, so the
//   pitch sung for H is 44100 / ((18063 * floor(400000 / (10 H)) + 8192) >> 14).
//   That was checked against every H from 48 to 520. The program inverts it,
//   choosing the H whose pitch is nearest the one wanted. Every note of the
//   equal-tempered scale from G#1 to B4 then comes out within 10 cents (most
//   within 5); between notes, as a vibrato or a bend passes, the nearest
//   pitch can be up to 18 cents off at the ends of the range. The range is
//   50 to 512.8 Hz, G1 to a little under C5: a phrase that goes outside it
//   is sung whole octaves up or down, and what still does not fit is held
//   at the edge.
//
// Pitch that moves inside a note -- vibrato, a bend, a glide -- is followed
// by cutting the vowel into consecutive copies of itself, each with its own
// target, so that the straight lines between targets trace the curve to
// within a few cents. DECtalk carries a vowel across copies of itself
// without a seam, provided each copy is stressed (an unstressed vowel is
// sung 4 dB softer). Diphthongs cannot be cut that way -- each copy would
// glide again -- and DECtalk's own long diphthongs spend most of their time
// on the second vowel, so a long diphthong is sung as a singer sings it: the
// first vowel held, and the diphthong itself only at the end.
//
// A clause holds about 150 such pieces; beyond that DECtalk forces a comma
// and a pause into the singing. So a long phrase is rendered in chunks, each
// with some of the phrase before and after it, and the chunks are joined
// where the singing is silent or unvoiced if there is such a place (the
// closure of a stop, best), with a short crossfade, lined up on the glottal
// pulses if the join has to fall in a vowel.
//
// Wendy is DECtalk's whispering voice, and whispers her songs.

#include <algorithm>
#include <atomic>
#include <climits>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
// DECtalk's own names for its types, as its dtmmedefs.h has them on Unix:
// 32-bit DWORD and UINT, and LONG a long, which is 64 bits here.
#include <dlfcn.h>
typedef uint32_t DWORD;
typedef uint32_t UINT;
typedef long LONG;
typedef void *HMODULE;
#endif

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

namespace svs {

namespace {

// -- DECtalk's API, as ttsapi.h declares it ----------------------------------
//
// The Release build is the multibyte one, so strings are char. There is one
// calling convention on x64. The buffer callback is not used: on x64 it is
// handed the buffer's address cut to 32 bits.

typedef void *DtHandle;
typedef void (*DtCallback)(LONG, LONG, DWORD, UINT);

struct DtPhonemeMark {
    DWORD phoneme, sample, duration, reserved;
};
struct DtIndexMark {
    DWORD value, sample, reserved;
};
struct DtBuffer {
    char *data;
    DtPhonemeMark *phonemes;
    DtIndexMark *index;
    DWORD max_length;
    DWORD max_phonemes;
    DWORD max_index;
    DWORD length;           //: bytes written
    DWORD phoneme_count;
    DWORD index_count;
    DWORD reserved;
};
static_assert(sizeof(void *) != 8 || sizeof(DtBuffer) == 56, "TTS_BUFFER_T is 56 bytes on x64");

typedef UINT (*StartupFn)(DtHandle *, UINT, DWORD, DtCallback, LONG, char *);
typedef UINT (*HandleFn)(DtHandle);
typedef UINT (*SpeakFn)(DtHandle, char *, DWORD);
typedef UINT (*OpenInMemoryFn)(DtHandle, DWORD);
typedef UINT (*AddBufferFn)(DtHandle, DtBuffer *);
typedef UINT (*ReturnBufferFn)(DtHandle, DtBuffer **);

const UINT kWaveMapper = UINT(-1);
const DWORD kDoNotUseAudioDevice = 0x80000000;
const DWORD kWaveFormat1M16 = 0x0004;        //: 11025 Hz, mono, 16 bits
const DWORD kTtsForce = 1;

// -- the engine's numbers ---------------------------------------------------

constexpr int kDtRate = 11025;
constexpr int kFrame = 71;                 //: samples per frame at 11025 Hz
constexpr double kFrameSeconds = double(kFrame) / kDtRate;

//: The milliseconds to ask for so that DECtalk gives exactly `frames`:
//: it takes ((ms + 4) * 10) >> 6, so ms + 4 must be ceil(6.4 * frames).
int ms_for_frames(int frames) { return (64 * frames + 9) / 10 - 4; }

//: The pitch DECtalk sings for a target of `hz` (see the top of the file).
double sung_hz(int hz) {
    int f0 = std::max(500, std::min(5121, hz * 10));
    long t0 = 400000L / f0;
    long q = (18063L * t0 + 8192L) >> 14;
    return 44100.0 / double(q);
}
const double kLowestHz = 50.0;
const double kHighestHz = 44100.0 / 86.0;    //: 512.8, what 508 to 512 give

//: The whole hertz whose sung pitch is nearest `hz`.
int target_for(double hz) {
    hz = std::max(kLowestHz, std::min(kHighestHz, hz));
    int lo = std::max(50, int(std::floor(hz)) - 4), hi = std::min(512, int(std::ceil(hz)) + 4);
    int best = lo;
    double err = 1e9;
    for (int h = lo; h <= hi; ++h) {
        double e = std::fabs(std::log(sung_hz(h) / hz));
        if (e < err) {
            err = e;
            best = h;
        }
    }
    return best;
}

// The pieces a vowel is cut into: no shorter than three frames (19 ms), no
// longer than forty (a quarter of a second), and placed so that the pitch
// between their targets stays within a few cents of the curve.
constexpr int kMinPiece = 3;
constexpr int kMaxPiece = 40;
constexpr double kPieceCents = 5.0;
//: Anything longer is cut anyway, voiced or not, to keep every length well
//: inside what the engine can count.
constexpr int kMaxUnit = 150;
//: A diphthong shorter than this is sung whole; a longer one holds its
//: first vowel and ends on the diphthong, which takes this share of it.
constexpr int kHoldDiphthong = 30;
constexpr double kGlideShare = 0.35;
constexpr int kGlideMin = 15, kGlideMax = 23;

// A chunk: at most this many symbols (a stressed vowel is two), well under
// the 300 a clause has room for.
constexpr int kChunkSymbols = 220;
//: Silence before each chunk, cut off afterwards, and after it.
constexpr int kLeadFrames = 8;
constexpr int kTailFrames = 8;
//: Every render but the first after DECtalk starts begins one frame later
//: than asked (the synthesiser hands back a frame of the render before),
//: and every one is a frame longer. The engine renders a moment of silence
//: as it starts, so that all the renders that matter are alike.
constexpr int kRenderDelay = 1;
//: Context a chunk renders beyond its join, so that what is kept on either
//: side of a join was sung with what follows and what came before.
constexpr int kPostRoll = 12;
constexpr int kPreRoll = 20;

// -- VocalWriter's phonemes as DECtalk's ------------------------------------

enum Kind {
    kSilence,      //: _
    kVowel,        //: held by repeating it, stressed
    kSonorant,     //: voiced and steady, held by repeating it (m, n, l, ...)
    kVoiced,       //: voiced, not repeated (v, z, b, d, ...)
    kStop,         //: has a closure, a good place for a join (p, t, k, ch, ...)
    kUnvoiced,     //: noise (s, f, sh, ...)
};

struct DtPhone {
    const char *vw;
    const char *hold;    //: what is repeated to hold it
    const char *glide;   //: a diphthong: what it ends on, if long (else null)
    const char *onset;   //: YU: what it begins with, if long (else null)
    Kind kind;
    bool stress;
};

// VocalWriter's fifty-seven phonemes in DECtalk's arpabet: what holds each,
// the diphthong a long one ends on or (YU) starts with, its kind, and whether
// it takes stress. DECtalk's symbols, from include\usa_phon.tab: iy ih ey eh
// ae aa ay aw ah ao ow oy uh uw rr yu ax ix ir er ar or ur, w yx r ll hx rx
// lx, m n nx el dz en, f v th dh s z sh zh, p b t d k g dx tx q ch jh.
// VocalWriter's allophones of t that DECtalk also has (TX, Q, DD) keep their
// own symbols; QX, a longer glottal stop, is DECtalk's q held longer. Stress
// goes on vowels and on syllabic l and n; on any other consonant DECtalk
// loses some of its length.
const DtPhone kPhones[] = {
    {"IY", "iy", nullptr, nullptr, kVowel, true},
    {"IH", "ih", nullptr, nullptr, kVowel, true},
    {"EH", "eh", nullptr, nullptr, kVowel, true},
    {"AE", "ae", nullptr, nullptr, kVowel, true},
    {"AA", "aa", nullptr, nullptr, kVowel, true},
    {"UX", "ah", nullptr, nullptr, kVowel, true},     // bUd
    {"AO", "ao", nullptr, nullptr, kVowel, true},
    {"UH", "uh", nullptr, nullptr, kVowel, true},
    {"AX", "ax", nullptr, nullptr, kVowel, true},
    {"ER", "rr", nullptr, nullptr, kVowel, true},     // bIRd: DECtalk's r-coloured vowel
    // The diphthongs hold the vowel DECtalk's own diphthong starts from
    // (measured by its formants) and end on the diphthong.
    {"EY", "eh", "ey", nullptr, kVowel, true},
    {"AY", "aa", "ay", nullptr, kVowel, true},
    {"OY", "ow", "oy", nullptr, kVowel, true},        // oy starts nearer ow than ao
    {"AW", "aa", "aw", nullptr, kVowel, true},
    {"OW", "ow", nullptr, nullptr, kVowel, true},     // repeated ow only drifts a little
    {"UW", "uw", nullptr, nullptr, kVowel, true},
    {"YU", "uw", nullptr, "yu", kVowel, true},        // mUte: the glide is at the start
    // The r-coloured vowels: the vowel held, the r at the end.
    {"IR", "iy", "ir", nullptr, kVowel, true},        // bEER
    {"XR", "eh", "er", nullptr, kVowel, true},        // bEAR (DECtalk's er is "air")
    {"AR", "aa", "ar", nullptr, kVowel, true},
    {"OR", "ow", "or", nullptr, kVowel, true},        // or starts close to ow
    {"UR", "uw", "ur", nullptr, kVowel, true},
    {"IX", "ix", nullptr, nullptr, kVowel, true},
    {"%", "_", nullptr, nullptr, kSilence, false},
    {"RX", "rr", nullptr, nullptr, kVowel, true},     // a short r-coloured vowel
    {"LX", "lx", nullptr, nullptr, kSonorant, false},
    {"EL", "el", nullptr, nullptr, kSonorant, true},  // syllabic: takes stress
    {"EN", "en", nullptr, nullptr, kSonorant, true},
    {"w", "w", nullptr, nullptr, kSonorant, false},
    {"y", "yx", nullptr, nullptr, kSonorant, false},
    {"r", "r", nullptr, nullptr, kSonorant, false},   // rx after a vowel, see below
    {"l", "ll", nullptr, nullptr, kSonorant, false},
    {"h", "hx", nullptr, nullptr, kUnvoiced, false},
    {"m", "m", nullptr, nullptr, kSonorant, false},
    {"n", "n", nullptr, nullptr, kSonorant, false},
    {"NG", "nx", nullptr, nullptr, kSonorant, false},
    {"f", "f", nullptr, nullptr, kUnvoiced, false},
    {"v", "v", nullptr, nullptr, kVoiced, false},
    {"TH", "th", nullptr, nullptr, kUnvoiced, false},
    {"DH", "dh", nullptr, nullptr, kVoiced, false},
    {"s", "s", nullptr, nullptr, kUnvoiced, false},
    {"z", "z", nullptr, nullptr, kVoiced, false},
    {"SH", "sh", nullptr, nullptr, kUnvoiced, false},
    {"ZH", "zh", nullptr, nullptr, kVoiced, false},
    {"p", "p", nullptr, nullptr, kStop, false},
    {"b", "b", nullptr, nullptr, kStop, false},
    {"t", "t", nullptr, nullptr, kStop, false},
    {"d", "d", nullptr, nullptr, kStop, false},
    {"k", "k", nullptr, nullptr, kStop, false},
    {"g", "g", nullptr, nullptr, kStop, false},
    {"CH", "ch", nullptr, nullptr, kStop, false},
    {"JH", "jh", nullptr, nullptr, kStop, false},
    {"TX", "tx", nullptr, nullptr, kStop, false},     // the t of "it"
    {"Q", "q", nullptr, nullptr, kStop, false},       // glottal stop
    {"QX", "q", nullptr, nullptr, kStop, false},      // a longer one
    {"DD", "dx", nullptr, nullptr, kVoiced, false},   // flap, beTTer
    {"O", "ow", nullptr, nullptr, kVowel, true},      // bOy's first part
};

const DtPhone *dt_phone(const std::string &vw) {
    std::string s = palette_alias(vw);
    for (const DtPhone &p : kPhones)
        if (s == p.vw) return &p;
    return nullptr;
}

// -- the engine: one per process --------------------------------------------
//
// Starting DECtalk loads the dictionary and starts its threads, so it is done
// once and the handle kept; renders take turns on it. It is never shut down:
// at exit the process takes its threads with it, and shutting down from a
// static destructor can wait for threads that are already gone.

class Engine {
public:
    static Engine &get() {
        static Engine *e = new Engine;
        return *e;
    }

    bool ready(std::string *why) {
        std::lock_guard<std::mutex> lock(mu_);
        start_locked();
        if (why) *why = why_;
        return handle_ != nullptr;
    }

    //: Render `text`, which should come out at about `expect` samples.
    bool render(const std::string &text, size_t expect, std::vector<int16_t> *out,
                std::string *err) {
        std::lock_guard<std::mutex> lock(mu_);
        out->clear();
        if (!start_locked()) {
            if (err) *err = why_;
            return false;
        }
        UINT r = render_locked(text, expect, out);
        if (r != 0) {
            if (err) *err = "DECtalk did not accept the phrase (error " + std::to_string(r) + ")";
            return false;
        }
        return true;
    }

private:
    UINT render_locked(const std::string &text, size_t expect, std::vector<int16_t> *out) {
        out->clear();
        // Enough buffers queued for twice what is expected, and a watch that
        // queues more if it is not enough: DECtalk waits forever for a buffer
        // when it runs out, and Sync with it.
        top_up((expect * 2 + kDtRate) * 2);
        std::atomic<bool> done(false);
        std::mutex wmu;
        std::condition_variable wcv;
        std::thread watch([&] {
            std::unique_lock<std::mutex> l(wmu);
            while (!done.load()) {
                wcv.wait_for(l, std::chrono::milliseconds(15));
                if (done.load()) break;
                if (untaken() < 8) top_up_count(32);
            }
        });
        std::vector<char> buf(text.begin(), text.end());
        buf.push_back('\0');
        UINT r = speak_(handle_, buf.data(), kTtsForce);
        if (r == 0) r = sync_(handle_);
        {
            std::lock_guard<std::mutex> l(wmu);
            done.store(true);
        }
        wcv.notify_all();
        watch.join();
        DtBuffer *partial = nullptr;
        return_(handle_, &partial);
        // The buffers come back in the order they were queued; the ones
        // DECtalk took have their length set.
        while (!queue_.empty() && taken(queue_.front())) {
            DtBuffer *b = queue_.front();
            queue_.pop_front();
            const int16_t *s = reinterpret_cast<const int16_t *>(b->data);
            out->insert(out->end(), s, s + b->length / 2);
            requeue(b);
        }
        return r;
    }

    struct Slot {
        DtBuffer header;
        std::vector<char> data;
        std::vector<DtPhonemeMark> marks;
    };
    static constexpr DWORD kUntaken = 0xFFFFFFFFu;
    static constexpr DWORD kBufferBytes = 65536;      //: 3 s of samples each
    static constexpr size_t kMaxQueued = 200;         //: DECtalk's queue holds 256

    // Look for DECtalk and start it. Missing files are looked for again the
    // next time (they may have been put there since); a failure to start is
    // not tried again.
    bool start_locked() {
        if (tried_) return handle_ != nullptr;
#ifdef _WIN32
        const char *lib_name = "DECtalk.dll";
#elif defined(__APPLE__)
        const char *lib_name = "libtts.dylib";
#else
        const char *lib_name = "libtts.so";
#endif
        const std::string folder = data_name("voices\\dectalk");
        std::string dir = find_data("voices\\dectalk");
        std::string dll = join_path(dir, lib_name);
        std::string dic = join_path(dir, "dtalk_us.dic");
        if (!file_exists(dll)) {
            // beside the program, or on macOS in the bundle's Frameworks
            std::vector<std::string> near{join_path(exe_dir(), lib_name)};
#ifdef __APPLE__
            near.push_back(join_path(exe_dir(), std::string("../Frameworks/") + lib_name));
#endif
            for (const std::string &beside : near) {
                if (!file_exists(beside)) continue;
                dll = beside;
                if (!file_exists(dic)) dic = join_path(exe_dir(), "dtalk_us.dic");
                break;
            }
        }
        if (!file_exists(dll)) {
            why_ = std::string("DECtalk itself, ") + lib_name + ", is not in " + folder;
            return false;
        }
        if (!file_exists(dic)) {
            why_ = "DECtalk's dictionary, dtalk_us.dic, is not in " + folder;
            return false;
        }
        tried_ = true;
#ifdef _WIN32
        // The DLL's own dependencies are looked for beside it.
        dll_ = LoadLibraryExW(widen(dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!dll_) {
            why_ = std::string(lib_name) + " in " + folder + " could not be loaded (Windows error " +
                   std::to_string(GetLastError()) + ")";
            return false;
        }
        // (through void *, as GetProcAddress's own type is not the function's)
        auto get = [&](const char *name) {
            return reinterpret_cast<void *>(GetProcAddress(dll_, name));
        };
#else
        dll_ = dlopen(dll.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!dll_) {
            const char *e = dlerror();
            why_ = std::string(lib_name) + " in " + folder + " could not be loaded (" + (e ? e : "dlopen failed") +
                   ")";
            return false;
        }
        auto get = [&](const char *name) { return dlsym(dll_, name); };
#endif
        startup_ = reinterpret_cast<StartupFn>(get("TextToSpeechStartupExFonix"));
        speak_ = reinterpret_cast<SpeakFn>(get("TextToSpeechSpeak"));
        sync_ = reinterpret_cast<HandleFn>(get("TextToSpeechSync"));
        open_ = reinterpret_cast<OpenInMemoryFn>(get("TextToSpeechOpenInMemory"));
        add_ = reinterpret_cast<AddBufferFn>(get("TextToSpeechAddBuffer"));
        return_ = reinterpret_cast<ReturnBufferFn>(get("TextToSpeechReturnBuffer"));
        if (!startup_ || !speak_ || !sync_ || !open_ || !add_ || !return_) {
            why_ = std::string(lib_name) + " in " + folder + " is not a DECtalk 5 that this program knows";
            return false;
        }
#ifdef _WIN32
        // DECtalk opens the dictionary with the ANSI file functions; the
        // short form of the path is plain ASCII wherever the folder is.
        std::wstring wdic = widen(dic);
        wchar_t shortp[MAX_PATH * 2];
        DWORD n = GetShortPathNameW(wdic.c_str(), shortp, DWORD(sizeof shortp / sizeof shortp[0]));
        if (n > 0 && n < sizeof shortp / sizeof shortp[0]) wdic.assign(shortp, n);
        int len = WideCharToMultiByte(CP_ACP, 0, wdic.c_str(), int(wdic.size()), nullptr, 0, nullptr, nullptr);
        std::vector<char> adic(size_t(std::max(0, len)) + 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, wdic.c_str(), int(wdic.size()), adic.data(), len, nullptr, nullptr);
#else
        std::vector<char> adic(dic.begin(), dic.end());
        adic.push_back('\0');
#endif
        if (adic.size() >= 500) {
            why_ = "the path to " + folder + " is too long for DECtalk";
            return false;
        }
        DtHandle h = nullptr;
        UINT r = startup_(&h, kWaveMapper, kDoNotUseAudioDevice, nullptr, 0, adic.data());
        if (r != 0 || !h) {
            why_ = "DECtalk would not start (error " + std::to_string(r) +
                   "); its dictionary may be damaged";
            return false;
        }
        r = open_(h, kWaveFormat1M16);
        if (r != 0) {
            why_ = "DECtalk would not render into memory (error " + std::to_string(r) + ")";
            return false;
        }
        handle_ = h;
        why_.clear();
        // the first render, which is a frame short (see kRenderDelay)
        std::vector<int16_t> scratch;
        render_locked("[:phoneme arpabet speak on][:np][_<48,100>]", kDtRate, &scratch);
        return true;
    }

    static bool taken(const DtBuffer *b) {
        return *static_cast<const volatile DWORD *>(&b->length) != kUntaken;
    }
    size_t untaken() {
        std::lock_guard<std::mutex> l(qmu_);
        size_t n = 0;
        for (auto it = queue_.rbegin(); it != queue_.rend() && !taken(*it); ++it) ++n;
        return n;
    }
    void requeue(DtBuffer *b) {
        b->length = kUntaken;
        b->phoneme_count = b->index_count = 0;
        std::lock_guard<std::mutex> l(qmu_);
        queue_.push_back(b);
        add_(handle_, b);
    }
    void top_up_count(size_t more) {
        for (size_t k = 0; k < more && untaken() < kMaxQueued; ++k) {
            slots_.emplace_back(new Slot);
            Slot &s = *slots_.back();
            s.data.assign(kBufferBytes, 0);
            s.marks.assign(512, DtPhonemeMark());
            s.header = DtBuffer();
            s.header.data = s.data.data();
            s.header.phonemes = s.marks.data();
            s.header.max_length = kBufferBytes;
            s.header.max_phonemes = DWORD(s.marks.size());
            requeue(&s.header);
        }
    }
    void top_up(size_t bytes) {
        size_t want = std::min(kMaxQueued, (bytes + kBufferBytes - 1) / kBufferBytes + 4);
        size_t have = untaken();
        if (have < want) top_up_count(want - have);
    }

    std::mutex mu_;
    std::mutex qmu_;
    bool tried_ = false;
    std::string why_ = "DECtalk has not been started";
    HMODULE dll_ = nullptr;
    DtHandle handle_ = nullptr;
    StartupFn startup_ = nullptr;
    SpeakFn speak_ = nullptr;
    HandleFn sync_ = nullptr;
    OpenInMemoryFn open_ = nullptr;
    AddBufferFn add_ = nullptr;
    ReturnBufferFn return_ = nullptr;
    std::deque<DtBuffer *> queue_;
    std::vector<std::unique_ptr<Slot>> slots_;
};

// -- the voices --------------------------------------------------------------

struct Voice {
    const char *name;
    const char *code;     //: DECtalk's commands for it
    double gain;          //: to bring full velocity to peaks of about one half
};

// Sung, every vowel is stressed and held, and five of the voices (Paul,
// Harry, Dennis, Rita and Val) then go past full scale inside DECtalk, which
// clips. Each voice's loudness (g5, the gain into its first formant, in dB)
// is set so that the loudest vowels on the highest notes peak at about
// -3 dB: 6 dB under DECtalk's own for Paul, 9 for Harry, a few or none for
// the others. The gains after it even the voices out: each was sung through
// ten vowels on notes across its range at full velocity and scaled so that
// its median peak is 0.5, as the SSI-263's are. Wendy whispers.
const Voice kVoices[10] = {
    {"Paul", "[:np][:dv g5 80]", 1.34},   {"Betty", "[:nb][:dv g5 78]", 1.37},
    {"Harry", "[:nh][:dv g5 72]", 1.67},  {"Frank", "[:nf][:dv g5 84]", 1.27},
    {"Dennis", "[:nd][:dv g5 80]", 1.01}, {"Kit", "[:nk][:dv g5 70]", 1.17},
    {"Ursula", "[:nu][:dv g5 79]", 1.29}, {"Rita", "[:nr][:dv g5 76]", 1.15},
    {"Wendy", "[:nw][:dv g5 82]", 1.04},  {"Val", "[:nv][:dv g5 80]", 1.33},
};

// -- turning a phrase into what DECtalk is sent ------------------------------

//: One phoneme as sent: its symbol, how many frames, and its pitch target.
struct Unit {
    std::string sym;
    bool stress = false;
    Kind kind = kSilence;
    int start = 0;          //: frame, from the start of the phrase
    int frames = 1;
    double cents = 0.0;     //: the pitch it should end on, cents above MIDI 0
    int hz = 100;           //: the target DECtalk is given for that
    //: joins are best made in silence, then in a stop, then in noise, and
    //: only then in the middle of a held vowel
    int join_cost = 9;
};

double cents_to_hz(double c) { return 440.0 * std::pow(2.0, (c - 6900.0) / 1200.0); }
double hz_to_cents(double hz) { return 6900.0 + 1200.0 * std::log2(hz / 440.0); }

//: A VocalWriter phoneme with its place in time, in frames.
struct Placed {
    const DtPhone *ph;
    int start, end;
    size_t note;
};

class Plan {
public:
    Plan(const Phrase &phrase, const PitchCurve &curve) : phrase_(phrase), curve_(curve) {
        choose_octave();
        place();
        sample_curve();
        build();
    }

    const std::vector<Unit> &units() const { return units_; }
    int frames() const { return total_frames_; }
    int octave_shift() const { return shift_; }
    bool clamped() const { return clamped_; }

    //: Where the pitch curve wants the voice at frame boundary `f`, in
    //: cents, after any octave shift and held inside DECtalk's range;
    //: `before` takes the value just before it (the end of a phoneme rather
    //: than the start of the next, which matters where a note changes).
    double cents_at(int f, bool before = false) const {
        f = std::max(0, std::min(total_frames_, f));
        return before ? before_[size_t(f)] : at_[size_t(f)];
    }

private:
    // The pitch at frame boundary `f` as the note being built sees it. The
    // phonemes' boundaries are on frames and the notes' are not, so near the
    // end of a note "the end of this phoneme" can lie a fraction of a frame
    // inside the next note; the time is kept inside the note.
    double pitch(int f) const {
        double t = f * kFrameSeconds;
        double a = curve_.note_start(note_), b = curve_.note_end(note_) - 1e-6;
        if (t >= a && t <= b && f >= 0 && f <= total_frames_) return at_[size_t(f)];
        return curve_cents(std::max(a, std::min(b, t)));
    }

    double curve_cents(double t) const {
        t = std::max(0.0, std::min(curve_.total() - 1e-6, t));
        double c = 100.0 * curve_.midi_at(t) + 1200.0 * shift_;
        return std::max(hz_to_cents(kLowestHz), std::min(hz_to_cents(kHighestHz), c));
    }

    void sample_curve() {
        at_.resize(size_t(total_frames_) + 1);
        before_.resize(size_t(total_frames_) + 1);
        for (int f = 0; f <= total_frames_; ++f) {
            at_[size_t(f)] = curve_cents(f * kFrameSeconds);
            before_[size_t(f)] = curve_cents(f * kFrameSeconds - 1e-6);
        }
    }

    // DECtalk sings from 50 to 512.8 Hz. A phrase that goes outside that is
    // moved by whole octaves until it fits, or fits as nearly as it can, so
    // that the tune stays the tune; whatever is still outside is held at
    // the edge.
    void choose_octave() {
        double lo = 1e9, hi = -1e9;
        for (size_t i = 0; i < phrase_.notes.size(); ++i) {
            const SungNote &n = phrase_.notes[i];
            bool rest = n.phonemes.empty() || (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
            if (rest) continue;
            double a = curve_.note_start(i), b = curve_.note_end(i);
            for (double t = a; t < b; t += 0.01) {
                double m = curve_.steady_midi_at(t);
                lo = std::min(lo, m);
                hi = std::max(hi, m);
            }
        }
        if (lo > hi) return;
        double top = hz_to_cents(kHighestHz) / 100.0, bottom = hz_to_cents(kLowestHz) / 100.0;
        // How far outside the range the phrase is when moved k octaves,
        // forgiving half a semitone: a C5 is better sung 35 cents flat than
        // two octaves down.
        auto out_by = [&](int k) {
            return std::max(0.0, hi + 12.0 * k - top - 0.5) +
                   std::max(0.0, bottom - 0.5 - (lo + 12.0 * k));
        };
        while (out_by(shift_ - 1) < out_by(shift_)) --shift_;
        while (out_by(shift_ + 1) < out_by(shift_)) ++shift_;
        clamped_ = hi + 12.0 * shift_ > top + 0.01 || lo + 12.0 * shift_ < bottom - 0.01;
    }

    // Every phoneme at its place: each note's phonemes take the note's time
    // in proportion to their lengths, and every boundary is put on the
    // frame nearest where it belongs, so that nothing drifts however long
    // the phrase.
    void place() {
        auto frame_of = [](double t) { return int(std::lround(t / kFrameSeconds)); };
        int at = 0;
        for (size_t i = 0; i < phrase_.notes.size(); ++i) {
            const SungNote &n = phrase_.notes[i];
            double a = curve_.note_start(i), b = curve_.note_end(i);
            std::vector<std::string> syms = n.phonemes;
            if (syms.empty()) syms.push_back(kRest);
            std::vector<double> ms = n.durations_ms;
            double want = (b - a) * 1000.0;
            double sum = 0.0;
            for (double v : ms) sum += std::max(0.0, v);
            if (ms.size() != syms.size() || sum <= 0.0) {
                ms = syllable_lengths(syms, want);
                sum = 0.0;
                for (double v : ms) sum += v;
            }
            double scale = sum > 0.0 ? want / sum : 0.0;
            double t = a;
            for (size_t k = 0; k < syms.size(); ++k) {
                // a symbol DECtalk has nothing for keeps its time, silent
                const DtPhone *ph = dt_phone(syms[k]);
                if (!ph) ph = dt_phone(kRest);
                double t1 = k + 1 == syms.size() ? b : t + std::max(0.0, ms[k]) * scale / 1000.0;
                int f0 = at;
                int f1 = std::max(f0 + 1, frame_of(t1));
                placed_.push_back({ph, f0, f1, i});
                at = f1;
                t = t1;
            }
        }
        total_frames_ = std::max(at, frame_of(curve_.total()));
    }

    void add(const char *sym, bool stress, Kind kind, int start, int frames, int cost) {
        Unit u;
        u.sym = sym;
        u.stress = stress;
        u.kind = kind;
        u.start = start;
        u.frames = frames;
        u.join_cost = cost;
        u.cents = pitch(start + frames);
        u.hz = target_for(cents_to_hz(u.cents));
        units_.push_back(u);
    }

    // Cut [a, b) into pieces of `sym` whose straight lines from target to
    // target follow the pitch curve, each as long as it can be while doing
    // so. `from` is the target the pitch arrives from.
    void hold(const char *sym, bool stress, Kind kind, int a, int b, double from) {
        int s = a;
        double v = from;
        while (s < b) {
            int best = b - s <= kMinPiece ? b : s + kMinPiece;
            for (int e = std::min(b, s + kMaxPiece); e > s + kMinPiece; --e) {
                double target = pitch(e);
                bool ok = true;
                for (int f = s + 1; f < e && ok; ++f) {
                    double line = v + (target - v) * double(f - s) / double(e - s);
                    ok = std::fabs(line - pitch(f)) <= kPieceCents;
                }
                if (ok) {
                    best = e;
                    break;
                }
            }
            // never leave a sliver at the end
            if (b - best > 0 && b - best < kMinPiece) best = b - s <= kMaxPiece ? b : b - kMinPiece;
            add(sym, stress, kind, s, best - s, best == b ? 4 : 3);
            v = units_.back().cents;
            s = best;
        }
    }

    void build() {
        double prev = cents_at(0);
        for (size_t k = 0; k < placed_.size(); ++k) {
            const Placed &p = placed_[k];
            const DtPhone *ph = p.ph;
            note_ = p.note;
            int n = p.end - p.start;
            bool prev_vowel = k > 0 && placed_[k - 1].ph->kind == kVowel;
            bool next_vowel = k + 1 < placed_.size() && placed_[k + 1].ph->kind == kVowel;
            const char *sym = ph->hold;
            // DECtalk folds an r that follows a vowel into the vowel, and
            // drops its time, unless a vowel follows; its postvocalic r is rx.
            if (std::string(ph->vw) == "r" && prev_vowel && !next_vowel) sym = "rx";
            if (ph->kind == kVowel || ph->kind == kSonorant) {
                const char *whole = ph->glide ? ph->glide : ph->onset;
                if (whole && n < kHoldDiphthong) {
                    // short: the diphthong itself, its pitch sliding across it
                    add(whole, ph->stress, ph->kind, p.start, n, 5);
                } else if (ph->glide) {
                    int g = std::max(kGlideMin, std::min(kGlideMax, int(std::lround(n * kGlideShare))));
                    hold(sym, ph->stress, ph->kind, p.start, p.end - g, prev);
                    add(ph->glide, ph->stress, ph->kind, p.end - g, g, 5);
                } else if (ph->onset) {
                    int g = std::max(12, std::min(20, int(std::lround(n * 0.3))));
                    add(ph->onset, ph->stress, ph->kind, p.start, g, 5);
                    hold(sym, ph->stress, ph->kind, p.start + g, p.end, units_.back().cents);
                } else {
                    hold(sym, ph->stress, ph->kind, p.start, p.end, prev);
                }
            } else {
                // Not held: one phoneme (or more if very long), ending on
                // where the curve is at its end.
                int cost = ph->kind == kSilence ? 0 : ph->kind == kStop ? 1 : ph->kind == kUnvoiced ? 2 : 9;
                for (int s = p.start; s < p.end; s += kMaxUnit)
                    add(sym, false, ph->kind, s, std::min(p.end, s + kMaxUnit) - s, cost);
            }
            if (!units_.empty()) prev = units_.back().cents;
        }
    }

    const Phrase &phrase_;
    const PitchCurve &curve_;
    std::vector<Placed> placed_;
    std::vector<Unit> units_;
    std::vector<double> at_, before_;
    int total_frames_ = 0;
    size_t note_ = 0;
    int shift_ = 0;
    bool clamped_ = false;
};

//: What a chunk sends DECtalk, and where its output belongs.
struct Chunk {
    size_t first = 0, last = 0;   //: units [first, last) rendered
    int join_in = 0;              //: the frame it takes over from the chunk before
    int join_out = 0;             //: the frame it hands over at (or the end)
};

int symbols(const Unit &u) { return u.stress ? 2 : 1; }

// Divide the units into chunks DECtalk can sing as one clause each, joining
// them where the singing is silent or unvoiced if possible.
std::vector<Chunk> plan_chunks(const std::vector<Unit> &u, int total_frames) {
    std::vector<Chunk> out;
    size_t first = 0;
    int join_in = 0;
    while (true) {
        Chunk c;
        c.first = first;
        c.join_in = join_in;
        int count = 0;
        size_t m = first;
        while (m < u.size() && count + symbols(u[m]) <= kChunkSymbols - 16) count += symbols(u[m++]);
        if (m >= u.size()) {
            c.last = u.size();
            c.join_out = total_frames;
            out.push_back(c);
            break;
        }
        // The best place for the join among the later units that fit, at
        // least one full unit and the pre-roll past the last join.
        size_t lo = first + (m - first) / 2;
        long best_score = -1;
        size_t best_unit = m - 1;
        int best_frame = u[m - 1].start;
        for (size_t k = lo; k < m; ++k) {
            const Unit &x = u[k];
            int f = x.start + x.frames / 2;           // the middle of a silence or noise
            if (x.kind == kStop) f = x.start + std::max(1, x.frames / 3);   // in the closure
            if (x.kind == kVowel || x.kind == kSonorant || x.kind == kVoiced) f = x.start;
            if (f - kPreRoll <= join_in || f <= u[first].start + kPreRoll) continue;
            long score = long(9 - x.join_cost) * 100000L + long(k);
            if (score > best_score) {
                best_score = score;
                best_unit = k;
                best_frame = f;
            }
        }
        c.join_out = best_frame;
        size_t last = best_unit + 1;
        while (last < u.size() && u[last].start < best_frame + kPostRoll) ++last;
        if (last < u.size()) ++last;
        c.last = last;
        out.push_back(c);
        // The next chunk starts far enough back to be settled by the join.
        size_t next = best_unit;
        while (next > first + 1 && u[next].start > best_frame - kPreRoll) --next;
        if (next <= first) next = first + 1;
        first = next;
        join_in = best_frame;
    }
    return out;
}

std::string chunk_text(const std::vector<Unit> &u, const Chunk &c, const char *voice,
                       double lead_cents) {
    std::string s = "[:phoneme arpabet speak on]";
    s += voice;
    s += "[_<" + std::to_string(ms_for_frames(kLeadFrames)) + "," +
         std::to_string(target_for(cents_to_hz(lead_cents))) + ">";
    for (size_t k = c.first; k < c.last; ++k) {
        if (u[k].stress) s += "'";
        s += u[k].sym;
        s += "<" + std::to_string(ms_for_frames(u[k].frames)) + "," + std::to_string(u[k].hz) + ">";
    }
    int last_hz = c.last > c.first ? u[c.last - 1].hz : 100;
    s += "_<" + std::to_string(ms_for_frames(kTailFrames)) + "," + std::to_string(last_hz) + ">]";
    return s;
}

// SVS_DECTALK_TRACE=1 in the environment reports each phrase's chunks on
// stderr; 2 also shows what DECtalk is sent.
int tracing() {
    static int t = -1;
    if (t < 0) {
        const char *e = std::getenv("SVS_DECTALK_TRACE");
        t = e ? std::atoi(e) : 0;
    }
    return t;
}

// The look-ahead limiter the SSI-263 uses: the gain each sample needs to stay
// under the ceiling, held a few milliseconds either side, eased down before a
// peak and back up after it.
void limit(std::vector<float> &x, double ceiling) {
    const size_t n = x.size();
    const long hold = long(0.003 * kSampleRate);
    std::vector<double> need(n, 1.0);
    bool any = false;
    for (size_t i = 0; i < n; ++i)
        if (std::fabs(x[i]) > ceiling) {
            need[i] = ceiling / std::fabs(x[i]);
            any = true;
        }
    if (!any) return;
    std::vector<double> g(need);
    for (size_t i = 0; i < n; ++i)
        if (need[i] < 1.0) {
            size_t a = size_t(std::max(0L, long(i) - hold)), b = std::min(n, i + size_t(hold) + 1);
            for (size_t j = a; j < b; ++j) g[j] = std::min(g[j], need[i]);
        }
    const double up = 1.0 - std::exp(-1.0 / (0.060 * kSampleRate));
    const double down = 1.0 - std::exp(-1.0 / (0.003 * kSampleRate));
    for (size_t i = 1; i < n; ++i) g[i] = std::min(g[i], g[i - 1] + (1.0 - g[i - 1]) * up);
    for (size_t i = n - 1; i-- > 0;) g[i] = std::min(g[i], g[i + 1] + (1.0 - g[i + 1]) * down);
    for (size_t i = 0; i < n; ++i) x[i] = float(x[i] * g[i]);
}

//: One chunk's output and where its sample 0 falls in the phrase.
struct Rendered {
    std::vector<int16_t> samples;
    long offset = 0;          //: in samples at 11025 Hz
    double at(long s) const {
        long j = s - offset;
        return j >= 0 && j < long(samples.size()) ? samples[size_t(j)] / 32768.0 : 0.0;
    }
};

// Where chunk `b` lines up with chunk `a` around sample `at`: the shift that
// puts b's glottal pulses on a's (within half a pitch period, and never more
// than 6 ms). Where there are no pulses to match -- silence, noise -- it is
// left where it is.
long align(const Rendered &a, const Rendered &b, long at, double hz) {
    const long win = 220;
    long period = long(std::lround(kDtRate / std::max(50.0, hz)));
    long range = std::min(long(0.006 * kDtRate), period / 2 + 1);
    double ea = 0.0;
    for (long s = at - win; s < at + win; ++s) ea += a.at(s) * a.at(s);
    if (ea < 1e-4 * 2 * win) return 0;
    double best = 0.0;
    long lag = 0;
    for (long l = -range; l <= range; ++l) {
        double xy = 0.0, yy = 0.0;
        for (long s = at - win; s < at + win; ++s) {
            double y = b.at(s - l);
            xy += a.at(s) * y;
            yy += y * y;
        }
        double r = yy > 0.0 ? xy / std::sqrt(ea * yy) : 0.0;
        if (r > best) {
            best = r;
            lag = l;
        }
    }
    return best > 0.6 ? lag : 0;
}

class DectalkSinger : public Singer {
public:
    explicit DectalkSinger(const Voice &v) : voice_(v) {}

    Sung sing(const Phrase &phrase) override {
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        const size_t out_len = size_t(std::lround(curve.total() * kSampleRate));
        Plan plan(phrase, curve);
        const std::vector<Unit> &u = plan.units();
        std::vector<Chunk> chunks = plan_chunks(u, plan.frames());
        if (tracing())
            std::fprintf(stderr, "[dectalk] %s: %zu units, %d frames, %zu chunks, octave %+d%s\n",
                         voice_.name, u.size(), plan.frames(), chunks.size(), plan.octave_shift(),
                         plan.clamped() ? ", clamped" : "");

        // -- each chunk rendered, and placed: its first unit where it
        //    belongs, then nudged onto the pulses of the chunk before --
        Engine &engine = Engine::get();
        std::vector<Rendered> parts(chunks.size());
        for (size_t ci = 0; ci < chunks.size(); ++ci) {
            const Chunk &c = chunks[ci];
            double lead = c.first > 0 ? u[c.first - 1].cents : u[0].cents;
            std::string text = chunk_text(u, c, voice_.code, lead);
            int frames = kLeadFrames + kRenderDelay + kTailFrames;
            for (size_t k = c.first; k < c.last; ++k) frames += u[k].frames;
            std::string err;
            bool ok = engine.render(text, size_t(frames) * kFrame, &parts[ci].samples, &err);
            if (tracing() > 1) std::fprintf(stderr, "%s\n", text.c_str());
            if (tracing())
                std::fprintf(stderr, "[dectalk] chunk %zu: units %zu-%zu, joins at frames %d and %d, "
                             "%d frames asked for, %.1f came%s%s\n",
                             ci, c.first, c.last, c.join_in, c.join_out, frames,
                             parts[ci].samples.size() / double(kFrame), ok ? "" : "; ", err.c_str());
            // Every frame asked for must have come (DECtalk adds a little
            // silence of its own at the end of a clause).
            if (!ok || parts[ci].samples.size() < size_t(frames) * kFrame) sung.stopped_short = true;
            // It always comes back 13 frames longer (the clause's own end);
            // anything else means DECtalk changed a length it was given.
            if (tracing() && ok && parts[ci].samples.size() != size_t(frames + 13) * kFrame)
                std::fprintf(stderr, "[dectalk] chunk %zu came back %+.1f frames from the usual\n", ci,
                             parts[ci].samples.size() / double(kFrame) - (frames + 13));
            // The chunk's first unit is kLeadFrames (and the frame every
            // render is late by) into what came back.
            parts[ci].offset = long(u[c.first].start - kLeadFrames - kRenderDelay) * kFrame;
            if (ci > 0) {
                long at = long(c.join_in) * kFrame;
                long shift = align(parts[ci - 1], parts[ci], at, cents_to_hz(plan.cents_at(c.join_in)));
                parts[ci].offset += shift;
                if (tracing() && shift)
                    std::fprintf(stderr, "[dectalk] chunk %zu moved %ld samples to meet the one before\n", ci, shift);
            }
        }

        // -- joined with short raised-cosine crossfades, at 11025 Hz --
        const long dt_len = long(std::ceil(curve.total() * kDtRate)) + 64;
        std::vector<float> dt(size_t(dt_len), 0.0f);
        const long fade = 48;       //: half the crossfade, 4.4 ms
        const double pi = 3.14159265358979323846;
        auto rise = [&](long s, long at) {
            if (s <= at - fade) return 0.0;
            if (s >= at + fade) return 1.0;
            return 0.5 - 0.5 * std::cos(pi * double(s - (at - fade)) / double(2 * fade));
        };
        for (size_t ci = 0; ci < parts.size(); ++ci) {
            long in = ci > 0 ? long(chunks[ci].join_in) * kFrame : LONG_MIN / 2;
            long out = ci + 1 < parts.size() ? long(chunks[ci].join_out) * kFrame : LONG_MAX / 2;
            long a = std::max(0L, in - fade), b = std::min(dt_len, out + fade);
            for (long s = a; s < b; ++s) {
                double w = (ci > 0 ? rise(s, in) : 1.0) * (ci + 1 < parts.size() ? 1.0 - rise(s, out) : 1.0);
                if (w > 0.0) dt[size_t(s)] += float(w * parts[ci].at(s));
            }
        }

        // -- to 44.1 kHz, loudness, and the limiter --
        std::vector<float> s = resample_to_output(dt, kDtRate);
        s.resize(out_len, 0.0f);
        double g = -1.0, cur = -1.0;
        const double tc = 1.0 - std::exp(-1.0 / (0.008 * kSampleRate));
        const double level = phrase.level * voice_.gain;
        for (size_t k = 0; k < out_len; ++k) {
            // the note's velocity, smoothed; a rest keeps the last
            const SungNote &n = phrase.notes[curve.note_at(double(k) / kSampleRate)];
            bool rest = n.phonemes.empty() || (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
            if (!rest || g < 0) g = std::max(1, std::min(127, n.velocity)) / 127.0;
            cur = cur < 0 ? g : cur + (g - cur) * tc;
            s[k] = float(s[k] * cur * level);
        }
        limit(s, 0.98);
        sung.samples.swap(s);
        return sung;
    }

private:
    Voice voice_;
};

class DectalkFamily : public Family {
public:
    std::string name() const override { return "DECtalk"; }
    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> v;
        for (int i = 0; i < 10; ++i)
            v.push_back({kDectalkBase + i, std::string("DECtalk ") + kVoices[i].name});
        return v;
    }
    bool available(std::string *why) override { return Engine::get().ready(why); }
    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int i = voice_id - kDectalkBase;
        if (i < 0 || i >= 10) {
            if (err) *err = "There is no DECtalk voice numbered " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        std::string why;
        if (!Engine::get().ready(&why)) {
            if (err) *err = why;
            return nullptr;
        }
        return std::make_unique<DectalkSinger>(kVoices[i]);
    }
};

}  // namespace

std::unique_ptr<Family> make_dectalk_family() { return std::make_unique<DectalkFamily>(); }

}  // namespace svs
