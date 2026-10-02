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
// DECtalk sings the way its own songs are written (the reference guide's
// "Happy Birthday", samplosf\src\data\birthday.txt): in phoneme mode, one
// phoneme for each sound, each with a length in milliseconds and a note of
// DECtalk's scale, "dey<600,10>". Notes 1 to 37 are C2 to C5. A clause sung
// in notes is in DECtalk's singing mode, and DECtalk does the rest itself:
// each new note is reached by a straight glide over 100 ms from the start
// of the phoneme that asks for it, and every note carries DECtalk's own
// vibrato, 6.3 Hz and +-2.05 Hz whatever the note (so deep on low notes and
// slight on high ones). That glide and that vibrato are what DECtalk singing
// sounds like, so they are left to DECtalk: a vowel is one phoneme however
// long the note, a diphthong is DECtalk's own diphthong, and the pitch is
// never steered from here. Everything below follows from how the engine
// treats the two numbers, measured on this build:
//
//   Lengths are counted in frames of 71 samples at 11025 Hz (6.44 ms; the
//   engine's own arithmetic says 6.4). A length of n ms becomes
//   ((n + 4) * 10) >> 6 frames, and the phonemes follow one another frame
//   for frame, so asking for the right number of frames for each puts every
//   phoneme where it belongs, to the nearest frame, however long the phrase.
//   A note number on a phoneme takes effect at the phoneme's first frame.
//
//   The scale is DECtalk's table of notes (notetab), retuned when DECtalk is
//   built (scripts/build_dectalk.ps1 says how; scripts/build_dectalk_mac.sh
//   does the same on macOS): every note from C2 to C5 comes out within about
//   a cent of equal temperament at A = 440 Hz, as an average over its
//   vibrato, but A4 (+4 cents) and B4 (+6), where the pitches DECtalk's
//   pitch period can make are too far apart. A phrase that
//   goes outside C2 to C5 is sung whole octaves up or down, and what still
//   does not fit is sung on the nearest end of the scale. The voices make no
//   difference to the pitch of a note.
//
// So the song's own vibrato controls and the mod wheel do not reach DECtalk:
// its vibrato cannot be changed or turned off in singing mode, and it is
// kept, at vibrato 0 too, because it is the DECtalk sound. Nor do pitch bends
// and detune, but as whole semitones. DECtalk will mix notes with targets in
// hertz (which slide straight from one target to the next over a phoneme,
// with no vibrato) only in a clause that starts with a target in hertz, and
// such a clause is not in singing mode: it loses the 4190/4096 that brings
// DECtalk's scale to concert pitch, and it cannot go above 512 Hz, a
// little under C5. A note in hertz would also slide across its whole vowel
// rather than bend inside it. So a bend is followed only as far as DECtalk's
// scale can: each phoneme takes the semitone nearest the bent pitch at its
// start (after any portamento has arrived), and a new note number is sent
// where that changes. A bend held by whole semitones is sung exactly, with
// DECtalk's glide into it; a bend that moves inside a vowel, or one smaller
// than half a semitone, is not heard.
//
// VocalWriter's phonemes are DECtalk's nearly one for one (the table below).
// Every vowel, and syllabic l and n, is stressed, as a sung syllable is: an
// unstressed vowel is sung 4 dB softer, and DECtalk's rules for unstressed
// syllables (flapping a t, for one) would change what was written. A vowel
// followed by r in the same note is sung as DECtalk's r-coloured vowel with
// the time of both (DECtalk folds such an r into the vowel itself, and drops
// its time); an r after a vowel that no vowel follows is DECtalk's
// postvocalic r. What DECtalk sings was checked against what it was sent
// with its own phoneme log (tests/engines/dectalk).
//
// A clause holds about 290 symbols (a phoneme, and a stress mark) and 200
// phonemes; beyond that DECtalk forces a comma and a pause into the singing.
// So a long phrase is rendered in chunks, each with some of the phrase before
// and after it, and the chunks are joined where the singing is silent or
// unvoiced if there is such a place (the closure of a stop, best), with a
// short crossfade, lined up on the glottal pulses if the join has to fall in
// a voiced sound. Each chunk's leading silence is made just long enough that
// its vibrato is in step with the chunk before.
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

//: DECtalk's scale: note 1 is C2, MIDI 36, and note 37 is C5, MIDI 72.
constexpr int kNoteBase = 35;
constexpr int kLowestNote = 1, kHighestNote = 37;
double note_hz(int note) { return 440.0 * std::pow(2.0, (note + kNoteBase - 69) / 12.0); }

//: The note numbers go on the first phoneme of each note, and on any later
//: one whose pitch differs (a bend). That starts DECtalk's glide where the
//: note starts. (Putting the number on every phoneme restarts the glide at
//: each, slower each time, so a note that starts with consonants arrives
//: late; putting it on the vowel only, as DECtalk's own songs mostly do,
//: starts the glide at the vowel, after the consonants. Both were measured,
//: in tests/engines/dectalk.)
enum class NoteOn { kNoteStart, kEveryPhoneme, kVowel };
constexpr NoteOn kNoteOn = NoteOn::kNoteStart;

//: Longer than this, a phoneme is sent as several copies of itself, which
//: keeps its length inside what the engine counts in 16 bits (32767 ms).
//: Nothing sung is ever near it.
constexpr int kMaxUnit = 4000;

// A chunk: at most this many symbols and phonemes, under the 290 and 200 a
// clause has room for.
constexpr int kChunkSymbols = 240;
constexpr int kChunkPhonemes = 170;
//: Silence before each chunk, cut off afterwards: long enough for DECtalk's
//: 100 ms glide from wherever its pitch was to the first note, and up to a
//: vibrato cycle more to bring the vibrato into step (see vibrato_lead).
constexpr int kLeadFrames = 20;
//: The vibrato advances 165/4096 of a cycle a frame (ph_drwt01.c,
//: linear_interp), from nought at the start of each render.
constexpr int kVibratoStep = 165, kVibratoCycle = 4096;
//: Silence after each chunk.
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
    kVowel,        //: a vowel, or syllabic l or n
    kSonorant,     //: voiced and steady (m, n, l, w, r, y)
    kVoiced,       //: voiced (v, z, dh, zh, the flap)
    kStop,         //: has a closure, a good place for a join (p, t, k, ch, ...)
    kUnvoiced,     //: noise (s, f, sh, h, ...)
};

struct DtPhone {
    const char *vw;
    const char *dt;        //: DECtalk's phoneme
    Kind kind;
    bool stress;
    //: a vowel: what it and an r after it in the same note are sung as
    //: (null: the vowel, then DECtalk's postvocalic r)
    const char *with_r;
};

// VocalWriter's fifty-seven phonemes in DECtalk's arpabet, from
// include\usa_phon.tab: iy ih ey eh ae aa ay aw ah ao ow oy uh uw rr yu ax ix
// ir er ar or ur, w yx r ll hx rx lx, m n nx el en, f v th dh s z sh zh, p b
// t d k g dx tx q ch jh. They are written in full (yx, not y; ll, not l):
// DECtalk reads a one-letter abbreviation it does not know as text. The
// diphthongs (ey ay oy aw ow yu) and the r-coloured vowels (ir er ar or ur,
// and rr) are DECtalk's own, sung whole, long or short; held long, DECtalk's
// diphthong reaches its second vowel within about a third of a second and
// holds that, as DECtalk's own long notes do.
//
// The r-coloured forms of vowel + r are VocalWriter's own (phonology.cpp,
// allophones: UX and AX + r are ER, EH + r is XR), with DECtalk's rule for
// the vowels VocalWriter has none for (EY, AE + r are er; OW + r is or).
// VocalWriter's O, bOy's first part, is DECtalk's ow: the start of DECtalk's
// oy is nearest ow (by its first two formants), and DECtalk's ow, unlike its
// ay or ey, drifts only a little. QX, a longer glottal stop, is q held
// longer. DECtalk dips the pitch for a glottal stop (q, and tx), as it does
// when it speaks.
const DtPhone kPhones[] = {
    {"IY", "iy", kVowel, true, "ir"},
    {"IH", "ih", kVowel, true, "ir"},
    {"EH", "eh", kVowel, true, "er"},
    {"AE", "ae", kVowel, true, "er"},
    {"AA", "aa", kVowel, true, "ar"},
    {"UX", "ah", kVowel, true, "rr"},      // bUd
    {"AO", "ao", kVowel, true, "or"},
    {"UH", "uh", kVowel, true, "ur"},
    {"AX", "ax", kVowel, true, "rr"},
    {"ER", "rr", kVowel, true, "rr"},      // bIRd
    {"EY", "ey", kVowel, true, "er"},
    {"AY", "ay", kVowel, true, nullptr},
    {"OY", "oy", kVowel, true, nullptr},
    {"AW", "aw", kVowel, true, nullptr},
    {"OW", "ow", kVowel, true, "or"},
    {"UW", "uw", kVowel, true, "ur"},
    {"YU", "yu", kVowel, true, nullptr},   // mUte
    {"IR", "ir", kVowel, true, "ir"},      // bEER
    {"XR", "er", kVowel, true, "er"},      // bEAR (DECtalk's er is "air")
    {"AR", "ar", kVowel, true, "ar"},
    {"OR", "or", kVowel, true, "or"},
    {"UR", "ur", kVowel, true, "ur"},      // pOOR
    {"IX", "ix", kVowel, true, nullptr},
    {"%", "_", kSilence, false, nullptr},
    {"RX", "rr", kVowel, true, "rr"},      // a short r-coloured vowel
    {"LX", "lx", kSonorant, false, nullptr},
    {"EL", "el", kVowel, true, nullptr},   // syllabic: takes stress
    {"EN", "en", kVowel, true, nullptr},
    {"w", "w", kSonorant, false, nullptr},
    {"y", "yx", kSonorant, false, nullptr},
    {"r", "r", kSonorant, false, nullptr},   // rx after a vowel, see Plan::shape
    {"l", "ll", kSonorant, false, nullptr},  // DECtalk makes it lx after a vowel
    {"h", "hx", kUnvoiced, false, nullptr},
    {"m", "m", kSonorant, false, nullptr},
    {"n", "n", kSonorant, false, nullptr},
    {"NG", "nx", kSonorant, false, nullptr},
    {"f", "f", kUnvoiced, false, nullptr},
    {"v", "v", kVoiced, false, nullptr},
    {"TH", "th", kUnvoiced, false, nullptr},
    {"DH", "dh", kVoiced, false, nullptr},
    {"s", "s", kUnvoiced, false, nullptr},
    {"z", "z", kVoiced, false, nullptr},
    {"SH", "sh", kUnvoiced, false, nullptr},
    {"ZH", "zh", kVoiced, false, nullptr},
    {"p", "p", kStop, false, nullptr},
    {"b", "b", kStop, false, nullptr},
    {"t", "t", kStop, false, nullptr},
    {"d", "d", kStop, false, nullptr},
    {"k", "k", kStop, false, nullptr},
    {"g", "g", kStop, false, nullptr},
    {"CH", "ch", kStop, false, nullptr},
    {"JH", "jh", kStop, false, nullptr},
    {"TX", "tx", kStop, false, nullptr},     // the t of "it"
    {"Q", "q", kStop, false, nullptr},       // glottal stop
    {"QX", "q", kStop, false, nullptr},      // a longer one
    {"DD", "dx", kVoiced, false, nullptr},   // flap, beTTer
    {"O", "ow", kVowel, true, "or"},         // bOy's first part
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
        // the folder as this platform writes it, for a message (as
        // core/paths.h's data_name writes it, where there is one)
#ifdef _WIN32
        const std::string folder = "voices\\dectalk";
#else
        const std::string folder = "voices/dectalk";
#endif
        std::string dir = find_data("voices\\dectalk");
        std::string dll = join_path(dir, lib_name);
        std::string dic = join_path(dir, "dtalk_us.dic");
        if (!file_exists(dll)) {
            // beside the program, or on macOS in the bundle's Frameworks
            std::vector<std::string> nearby{join_path(exe_dir(), lib_name)};
#ifdef __APPLE__
            nearby.push_back(join_path(exe_dir(), std::string("../Frameworks/") + lib_name));
#endif
            for (const std::string &beside : nearby) {
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
// -1.5 dB (0.74 to 0.86 of full scale, nothing clipped): 6 dB under
// DECtalk's own for Paul, 9 for Harry, a few or none for the others. The
// gains after it even the voices out: each was sung through ten vowels on
// notes across its range at full velocity and scaled so that its median
// peak is 0.5, as the SSI-263's are. Wendy whispers.
const Voice kVoices[10] = {
    {"Paul", "[:np][:dv g5 80]", 1.24},   {"Betty", "[:nb][:dv g5 78]", 1.21},
    {"Harry", "[:nh][:dv g5 72]", 1.56},  {"Frank", "[:nf][:dv g5 84]", 1.15},
    {"Dennis", "[:nd][:dv g5 80]", 0.92}, {"Kit", "[:nk][:dv g5 70]", 1.10},
    {"Ursula", "[:nu][:dv g5 79]", 1.19}, {"Rita", "[:nr][:dv g5 76]", 1.13},
    {"Wendy", "[:nw][:dv g5 82]", 1.09},  {"Val", "[:nv][:dv g5 80]", 1.24},
};

// -- turning a phrase into what DECtalk is sent ------------------------------

//: One phoneme as sent: its symbol, how many frames, and its note.
struct Unit {
    std::string sym;
    bool stress = false;
    Kind kind = kSilence;
    int start = 0;          //: frame, from the start of the phrase
    int frames = 1;
    int note = 0;           //: the note number sent with it, or 0 for none
    int sung = 1;           //: the note in force while it sounds
    //: joins are best made in silence, then in a stop, then in noise, and
    //: only then at the start of a voiced sound
    int join_cost = 9;
};

//: A VocalWriter phoneme with its place in time, in frames, and what it is
//: sung as.
struct Placed {
    const DtPhone *ph;
    std::string sym;
    int start, end;
    size_t note;
    double midi = 60.0;     //: the pitch it is sung at, before any octave shift
};

class Plan {
public:
    Plan(const Phrase &phrase, const PitchCurve &curve) : phrase_(phrase), curve_(curve) {
        place();
        shape();
        pitch();
        choose_octave();
        build();
    }

    const std::vector<Unit> &units() const { return units_; }
    int frames() const { return total_frames_; }
    int octave_shift() const { return shift_; }
    bool clamped() const { return clamped_; }

private:
    static bool is_rest(const SungNote &n) {
        return n.phonemes.empty() || (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
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
                Placed p{ph, ph->dt, f0, f1, i};
                placed_.push_back(p);
                at = f1;
                t = t1;
            }
        }
        total_frames_ = std::max(at, frame_of(curve_.total()));
    }

    // An r after a vowel. In the same note, with no vowel after it in the
    // note, it and the vowel are one r-coloured vowel, as DECtalk itself
    // would make them -- but DECtalk would also drop the r's time, so it is
    // done here, with the time of both. Otherwise an r after a vowel that no
    // vowel follows is DECtalk's postvocalic rx, which keeps its time.
    void shape() {
        std::vector<Placed> out;
        for (size_t k = 0; k < placed_.size(); ++k) {
            Placed p = placed_[k];
            if (std::string(p.ph->vw) == "r" && !out.empty() && out.back().ph->kind == kVowel) {
                Placed &v = out.back();
                bool vowel_next = k + 1 < placed_.size() && placed_[k + 1].ph->kind == kVowel;
                bool vowel_next_in_note = vowel_next && placed_[k + 1].note == p.note;
                if (v.note == p.note && !vowel_next_in_note && v.ph->with_r) {
                    v.sym = v.ph->with_r;
                    v.end = p.end;
                    continue;
                }
                if (!vowel_next) p.sym = "rx";
            }
            out.push_back(p);
        }
        placed_.swap(out);
    }

    // The pitch of each phoneme: the curve without its vibrato (DECtalk has
    // its own) where the phoneme starts, or, if portamento is set, where the
    // glide it asks for has arrived (DECtalk glides by itself).
    void pitch() {
        const double glide = portamento_seconds(phrase_.style.portamento);
        for (Placed &p : placed_) {
            double a = curve_.note_start(p.note), b = curve_.note_end(p.note) - 1e-6;
            double t = p.start * kFrameSeconds;
            t = std::max(t, a + glide);
            t = std::max(a, std::min(b, t));
            p.midi = curve_.steady_midi_at(t);
        }
    }

    // DECtalk sings from C2 to C5. A phrase that goes outside that is moved
    // by whole octaves until it fits, or fits as nearly as it can, so that
    // the tune stays the tune; whatever is still outside is sung at the end
    // of the scale.
    void choose_octave() {
        int lo = INT_MAX, hi = INT_MIN;
        for (const Placed &p : placed_) {
            if (is_rest(phrase_.notes[p.note])) continue;
            int m = int(std::lround(p.midi));
            lo = std::min(lo, m);
            hi = std::max(hi, m);
        }
        if (lo > hi) return;
        const int top = kHighestNote + kNoteBase, bottom = kLowestNote + kNoteBase;
        auto out_by = [&](int k) {
            return std::max(0, hi + 12 * k - top) + std::max(0, bottom - (lo + 12 * k));
        };
        while (out_by(shift_ - 1) < out_by(shift_)) --shift_;
        while (out_by(shift_ + 1) < out_by(shift_)) ++shift_;
        clamped_ = out_by(shift_) > 0;
    }

    int number(const Placed &p) const {
        int n = int(std::lround(p.midi)) + 12 * shift_ - kNoteBase;
        return std::max(kLowestNote, std::min(kHighestNote, n));
    }

    void build() {
        int sent = 0;
        for (size_t k = 0; k < placed_.size(); ++k) {
            const Placed &p = placed_[k];
            const DtPhone *ph = p.ph;
            bool first = k == 0 || placed_[k - 1].note != p.note;
            int n = number(p);
            bool send = false;
            switch (kNoteOn) {
            case NoteOn::kNoteStart: send = first || n != sent; break;
            case NoteOn::kEveryPhoneme: send = true; break;
            case NoteOn::kVowel: {
                // the note's vowel, or its first phoneme if it has none
                bool nucleus = ph->kind == kVowel;
                if (!nucleus && first) {
                    nucleus = true;
                    for (size_t j = k; j < placed_.size() && placed_[j].note == p.note; ++j)
                        if (placed_[j].ph->kind == kVowel) nucleus = false;
                }
                send = nucleus && n != sent;
                break;
            }
            }
            if (send) sent = n;
            int cost = ph->kind == kSilence ? 0 : ph->kind == kStop ? 1 : ph->kind == kUnvoiced ? 2 : 9;
            for (int s = p.start; s < p.end; s += kMaxUnit) {
                Unit u;
                u.sym = p.sym;
                u.stress = ph->stress;
                u.kind = ph->kind;
                u.start = s;
                u.frames = std::min(p.end, s + kMaxUnit) - s;
                u.note = send && s == p.start ? n : 0;
                u.sung = sent;
                u.join_cost = cost;
                units_.push_back(u);
            }
        }
        // whatever comes before the first note number is sung on it
        int first = 0;
        for (const Unit &u : units_)
            if (u.sung) {
                first = u.sung;
                break;
            }
        for (Unit &u : units_)
            if (!u.sung) u.sung = first ? first : 1;
    }

    const Phrase &phrase_;
    const PitchCurve &curve_;
    std::vector<Placed> placed_;
    std::vector<Unit> units_;
    int total_frames_ = 0;
    int shift_ = 0;
    bool clamped_ = false;
};

//: What a chunk sends DECtalk, and where its output belongs.
struct Chunk {
    size_t first = 0, last = 0;   //: units [first, last) rendered
    int join_in = 0;              //: the frame it takes over from the chunk before
    int join_out = 0;             //: the frame it hands over at (or the end)
    int lead = kLeadFrames;       //: its leading silence, in frames
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
        int count = 0, phonemes = 0;
        size_t m = first;
        while (m < u.size() && count + symbols(u[m]) <= kChunkSymbols && phonemes < kChunkPhonemes) {
            count += symbols(u[m++]);
            ++phonemes;
        }
        if (m >= u.size()) {
            c.last = u.size();
            c.join_out = total_frames;
            out.push_back(c);
            break;
        }
        // The best place for the join among the later units that fit, at
        // least the pre-roll past the last join.
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
    // DECtalk's vibrato starts from nought with each render, so each chunk's
    // leading silence is lengthened, by up to a cycle, until the vibrato
    // under its units is where the first chunk's would have been.
    const int origin = out[0].lead - u[out[0].first].start;
    for (size_t ci = 1; ci < out.size(); ++ci) {
        Chunk &c = out[ci];
        int best = kLeadFrames, err = INT_MAX;
        for (int lead = kLeadFrames; lead < kLeadFrames + 26; ++lead) {
            int d = (lead - u[c.first].start - origin) * kVibratoStep;
            d = ((d % kVibratoCycle) + kVibratoCycle) % kVibratoCycle;
            d = std::min(d, kVibratoCycle - d);
            if (d < err) {
                err = d;
                best = lead;
            }
        }
        c.lead = best;
    }
    return out;
}

std::string chunk_text(const std::vector<Unit> &u, const Chunk &c, const char *voice) {
    std::string s = "[:phoneme arpabet speak on]";
    s += voice;
    s += "[_<" + std::to_string(ms_for_frames(c.lead)) + "," + std::to_string(u[c.first].sung) + ">";
    for (size_t k = c.first; k < c.last; ++k) {
        if (u[k].stress) s += "'";
        s += u[k].sym;
        s += "<" + std::to_string(ms_for_frames(u[k].frames));
        if (u[k].note) s += "," + std::to_string(u[k].note);
        s += ">";
    }
    s += "_<" + std::to_string(ms_for_frames(kTailFrames)) + ">]";
    return s;
}

// SVS_DECTALK_TRACE=1 in the environment reports each phrase's chunks on
// stderr; 2 also shows what DECtalk is sent. SVS_DECTALK_TEXT names a file
// that every clause sent is added to, a line each (the engine's test reads
// it back through DECtalk's phoneme log).
int tracing() {
    static int t = -1;
    if (t < 0) {
        const char *e = std::getenv("SVS_DECTALK_TRACE");
        t = e ? std::atoi(e) : 0;
    }
    return t;
}

void keep_text(const std::string &text) {
    const char *path = std::getenv("SVS_DECTALK_TEXT");
    if (!path || !*path) return;
    if (FILE *f = std::fopen(path, "a")) {
        std::fprintf(f, "%s\n", text.c_str());
        std::fclose(f);
    }
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
            std::string text = chunk_text(u, c, voice_.code);
            keep_text(text);
            int frames = c.lead + kRenderDelay + kTailFrames;
            for (size_t k = c.first; k < c.last; ++k) frames += u[k].frames;
            std::string err;
            bool ok = engine.render(text, size_t(frames) * kFrame, &parts[ci].samples, &err);
            if (tracing() > 1) std::fprintf(stderr, "%s\n", text.c_str());
            if (tracing()) {
                size_t full = 0;
                for (int16_t v : parts[ci].samples) full += v >= 32767 || v <= -32768;
                std::fprintf(stderr, "[dectalk] chunk %zu: units %zu-%zu, joins at frames %d and %d, "
                             "lead %d, %d frames asked for, %.1f came, %zu samples at full scale%s%s\n",
                             ci, c.first, c.last, c.join_in, c.join_out, c.lead, frames,
                             parts[ci].samples.size() / double(kFrame), full, ok ? "" : "; ",
                             err.c_str());
            }
            // Every frame asked for must have come (DECtalk adds a little
            // silence of its own at the end of a clause).
            if (!ok || parts[ci].samples.size() < size_t(frames) * kFrame) sung.stopped_short = true;
            // It comes back 13 frames longer (the clause's own end), and 4
            // more when the last phoneme is a stop, which DECtalk releases
            // into a short vowel before a pause; anything else means DECtalk
            // changed a length it was given.
            if (tracing() && ok) {
                long extra = long(parts[ci].samples.size()) - long(frames + 13) * kFrame;
                if (extra != 0 && extra != 4L * kFrame)
                    std::fprintf(stderr, "[dectalk] chunk %zu came back %+.1f frames from the usual\n", ci,
                                 extra / double(kFrame));
            }
            // The chunk's first unit is its lead (and the frame every render
            // is late by) into what came back.
            parts[ci].offset = long(u[c.first].start - c.lead - kRenderDelay) * kFrame;
            if (ci > 0) {
                long at = long(c.join_in) * kFrame;
                size_t k = c.first;
                while (k + 1 < c.last && u[k + 1].start <= c.join_in) ++k;
                long shift = align(parts[ci - 1], parts[ci], at, note_hz(u[k].sung));
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
