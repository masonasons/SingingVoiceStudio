// macintalk.cpp -- Apple's MacinTalk voices, from Mac OS X 10.4 to 10.7, singing.
//
// The engines are Apple's own, as Intel Mac OS X shipped them, run on Windows
// by Panthera (github.com/tgeczy/panthera-speech): panthera_host.exe is a
// 32-bit program that maps Apple's MacinTalk into memory and serves requests
// over its standard input and output. Panthera's NVDA add-on installs the host
// and extracts the engine and voices from the user's own install discs into
// NVDA's configuration folder, macintalk\tiger, \leopard, \snowleopard and
// \lion. Nothing of Apple's or Panthera's is part of this program; the host is
// started the way the add-on's own driver starts it (pantheradriver.py, the
// reference client):
//
//   panthera_host.exe --serve <MacinTalk> <SpeechDictionary> <Speech\Voices>
//
// one host per generation, kept for as long as the program runs, and asked
// one utterance at a time:
//
//   request   u32 'TGR3', i32 rate (words a minute), i32 pitch (tenths of a
//             semitone from the voice's own, 0), u32 flags (0: the text is
//             left alone), u32 voice bytes, u32 text bytes, the voice's
//             bundle name in UTF-8, the text in MacRoman
//   response  u32 'TGRS', i32 status (an OSErr), u32 frames, then that many
//             16-bit samples, mono, 22050 Hz
//
// A request that fails part way leaves the pipe out of step, so a host that
// fails is killed and a new one started (it takes a few tens of milliseconds)
// and the phrase asked again. MacinTalk does sometimes corrupt its own heap
// and die -- the six voices that play their own tunes most of all -- so this
// is not only for accidents.
//
// SINGING. MacinTalk has a singing input of its own, which Apple documents in
// its Speech Synthesis Programming Guide as TUNE: after "[[inpt TUNE]]" each
// line is one phoneme of Apple's American English set with its length in
// milliseconds and its pitch as points of (hertz : percent through the
// phoneme), "AA {D 600; P 220:0 220:100}", the points joined by straight
// lines. A phrase is one utterance: every phoneme of every note, one line
// each, the pitch points sampled from the phrase's pitch curve (pitch.h), so
// bends, glides, vibrato and the mod wheel are all just more points. What the
// engines do with that was measured, on every voice of the four generations,
// and everything below follows from it:
//
//   Mac OS X 10.7 accepts TUNE and then ignores every length and pitch in it
//   (Panthera's driver says the same, panthera-speech#6), so Lion's voices are
//   listed and cannot sing. That is very likely Panthera's host rather than
//   Lion: 10.7 moved TUNE's reading into SpeechDictionary, which hands the
//   notes to MacinTalk as CFArrays of CFDictionaries, and the host's stand-in
//   for CoreFoundation has no CFArrayCreate, CFDictionaryCreate,
//   CFArrayGetCount or CFArrayGetValueAtIndex. Alex, from 10.5 on, follows
//   the notes as Vicki does (an earlier reading here, that he kept a pitch of
//   his own and came back silent from 10.6, did not hold up). Six of the novelty voices -- Bad News,
//   Bells, Cellos, Good News, Hysterical and Pipe Organ -- read TUNE exactly as
//   they read text, one note of their own song to each syllable at their own
//   speed, whatever the lines say; they are listed and cannot sing either.
//   Albert, Bahh, Bubbles, Deranged and Whisper keep to the lengths but make
//   their own pitch (a breath, a bleat, bubbles, a wander, a whisper): they
//   sing the words in time.
//
//   A line takes at most eleven pitch points; from the twelfth on, the
//   engine drops back to its own speaking intonation. A line's points are
//   only the ones its curve needs -- two for a held note, the turns of a
//   vibrato or a bend -- and a sound is cut into several lines of itself
//   only when it needs more than eleven, into as few as will do. Every cut
//   is heard: the engines start the vowel again, MacinTalk Pro loudly, and a
//   UW glides into itself each time ("oow oow"). A diphthong is never cut
//   (each half would glide), nor one of MacinTalk Pro's lax vowels (see
//   below): those keep one line and the eleven points that best draw the
//   curve.
//
//   Pitch comes out on a grid: MacinTalk Pro (Agnes, Bruce, Victoria) and the
//   concatenative engine (Vicki) make each pitch period a whole number of
//   samples, MacinTalk 3 (Fred and the rest) each pitch a whole number of
//   hertz, so a held note lands on the nearest step above or below -- 17
//   cents apart at A3 for MacinTalk Pro, 26 at C2 for MacinTalk 3. Each
//   line's points therefore alternate a little either side of the curve, a
//   step's width in all, which the engine follows period by period, and the
//   pitch heard averages out to the one asked for. What is left is each
//   voice's own offset (MacinTalk 3 sings about 16 cents flat everywhere,
//   Zarvox 9, Boing between three eighths and five twelfths of what it is
//   asked), measured for every voice across its range and taken out (kTuning
//   below). The engines follow the points some 8 to 10 ms late, so the curve
//   is read that much ahead.
//
//   Time: MacinTalk 3 works in frames of 112 samples. A line of D ms lasts
//   round(D / 5) frames, at least one, and the lines follow one another
//   frame for frame, so each line's length is worked out in frames from where
//   the phrase should be, and every note starts within half a frame (2.5 ms)
//   of its time however long the phrase. The one exception is measured and
//   allowed for: a stop or nasal that ends before a silence is given five
//   frames more.
//
//   MacinTalk Pro and the concatenative engine are speech engines first. Mac
//   OS X 10.5 and 10.6 make a line about 0.82 of its D and then add a length
//   of their own that depends on the phoneme, its neighbours and the pitch,
//   rounded to whole pitch periods; Tiger keeps to D, but not exactly. And
//   every generation drops lax vowels where English would not have them:
//   AE EH IH IX after another lax vowel with no consonant after it, and UH
//   with no consonant on either side. (A zero-length silence before the
//   first kind keeps it; the second is sung as UW.) So for these voices the
//   lengths are found by listening: the phrase is sung, and in what comes
//   back each note's start is found -- where the pitch of the note before
//   gives way to its own (the engines change pitch the moment a line
//   begins), or where the vowel before it stops if it begins with no voice --
//   and inside a long vowel, where the vibrato or the bend sung lines up with
//   the curve. The lengths are corrected from that and the phrase sung again,
//   until every start that can be found is within a few milliseconds of its
//   time, kMaxPasses at most, keeping the best. Nothing is added to the
//   phrase to find them: an embedded command, even a [[volm]], can change
//   how MacinTalk Pro divides the syllables, and so the lengths.
//
// The loudness is the engines' own [[volm]], kept low enough that the host's
// 16-bit samples never clip, and each voice is brought to the same level
// afterwards (kTuning). A note's velocity and the part's level are applied to
// the samples.
//
// ON A MAC the voices are the system's own, still installed under English in
// System Settings (Accessibility, Spoken Content, Manage Voices) and spoken
// through AVSpeechSynthesizer (macintalk_mac.mm). Apple's engine there no
// longer sings: the TUNE and PHON inputs are read out as text. So each
// syllable is spoken from its phonemes, in the International Phonetic
// Alphabet, which AVSpeech still accepts, with its intonation flattened
// ([[pmod 0]]) and its baseline ([[pbas]]) put near the note -- what a
// baseline number gives differs by voice, so each voice is measured once --
// and the speech is then retimed and retuned to the score by
// pitch-synchronous overlap-add (macintalk_dsp.h): the consonants kept to
// their planned lengths, the vowel stretched to fill the note, and every
// glottal period laid down again at the pitch the phrase's curve asks for,
// vibrato, bends and glides included. One utterance sings a phrase, with a
// silence between the syllables to cut it on. A Mac has one MacinTalk of
// each name, so every generation's number sings with it, and the list shows
// one set of voices, under the numbers Snow Leopard's have on Windows.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/paths.h"
#include "core/phonology.h"
#include "voices/family.h"
#include "voices/pitch.h"

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <tlhelp32.h>
#endif
#ifdef __APPLE__
#include "voices/macintalk_dsp.h"
#include "voices/macintalk_mac.h"
#endif

namespace svs {

namespace {

// -- the voices ---------------------------------------------------------------

enum Engine {
    kMtk3,   // MacinTalk 3: Fred, Kathy and the novelty voices
    kPro,    // MacinTalk Pro: Agnes, Bruce, Victoria
    kMeow,   // the concatenative engine: Vicki and Alex
};

enum Gift {
    kSings,      // follows the lengths and the pitch
    kInTime,     // follows the lengths; the pitch is the voice's own
    kOwnTune,    // ignores both: plays its own song
};

struct VoiceDef {
    const char *bundle;    //: the folder, Speech\Voices\<bundle>.SpeechVoice
    const char *display;   //: as the voice list shows it
    Engine engine;
    Gift gift;
};

// The voices of 10.4 to 10.7 -- every one of them has the first twenty-three
// -- in a fixed order: a voice's number is 4000 + 100 * generation + its
// place here.
const VoiceDef kVoices[] = {
    {"Agnes", "Agnes", kPro, kSings},
    {"Albert", "Albert", kMtk3, kInTime},
    {"BadNews", "Bad News", kMtk3, kOwnTune},
    {"Bahh", "Bahh", kMtk3, kInTime},
    {"Bells", "Bells", kMtk3, kOwnTune},
    {"Boing", "Boing", kMtk3, kSings},
    {"Bruce", "Bruce", kPro, kSings},
    {"Bubbles", "Bubbles", kMtk3, kInTime},
    {"Cellos", "Cellos", kMtk3, kOwnTune},
    {"Deranged", "Deranged", kMtk3, kInTime},
    {"Fred", "Fred", kMtk3, kSings},
    {"GoodNews", "Good News", kMtk3, kOwnTune},
    {"Hysterical", "Hysterical", kMtk3, kOwnTune},
    {"Junior", "Junior", kMtk3, kSings},
    {"Kathy", "Kathy", kMtk3, kSings},
    {"Organ", "Pipe Organ", kMtk3, kOwnTune},
    {"Princess", "Princess", kMtk3, kSings},
    {"Ralph", "Ralph", kMtk3, kSings},
    {"Trinoids", "Trinoids", kMtk3, kSings},
    {"Vicki", "Vicki", kMeow, kSings},
    {"Victoria", "Victoria", kPro, kSings},
    {"Whisper", "Whisper", kMtk3, kInTime},
    {"Zarvox", "Zarvox", kMtk3, kSings},
    // added after the rest, so that their numbers stay as they were; not in
    // Tiger, which came before him
    {"Alex", "Alex", kMeow, kSings},
};
constexpr int kVoiceCount = int(sizeof kVoices / sizeof kVoices[0]);
constexpr int kGenStride = 100;

// -- the generations ----------------------------------------------------------

struct GenDef {
    const char *key;        //: its folder under macintalk
    const char *title;      //: "Leopard"
    const char *release;    //: "10.5"
    const char *env;        //: a variable naming the folder, as the add-on reads it
    const char *legacy[3];  //: where older add-ons kept it, in NVDA's configuration
    const char *pointers[4];  //: text files there that name the folder
    const char *runtime[3];   //: Apple's C++ runtime: one of these, in the folder or usr\lib
    const char *abi;          //: and this one too, if set
    bool tune;                //: whether its MacinTalk honours TUNE's lengths and pitches
};

// pantheratiger.py, pantheraleopard.py, pantherasnowleopard.py and
// pantheralion.py, which say where each generation is looked for.
const GenDef kGens[] = {
    {"tiger", "Tiger", "10.4", "TIGER_TREE", {"tigerspeech-data"},
     {"tigerspeech-data.txt", "tiger-tree.txt"}, {nullptr}, nullptr, true},
    {"leopard", "Leopard", "10.5", "LEOPARD_TREE", {"leopardspeech-data", "leopard-data"},
     {"leopardspeech-data.txt", "leopard-data.txt", "leopard-tree.txt"},
     {"libstdc++.6.0.4.dylib", "libstdc++.6.dylib"}, nullptr, true},
    {"snowleopard", "Snow Leopard", "10.6", "SNOWLEOPARD_TREE", {nullptr},
     {"snowleopardspeech-data.txt", "snowleopard-tree.txt"},
     {"libstdc++.6.0.9.dylib", "libstdc++.6.dylib"}, nullptr, true},
    {"lion", "Lion", "10.7", "LION_TREE", {nullptr}, {"lionspeech-data.txt", "lion-tree.txt"},
     {"libstdc++.6.0.9.dylib", "libstdc++.6.dylib"}, "libc++abi.dylib", false},
};
constexpr int kGenCount = int(sizeof kGens / sizeof kGens[0]);

std::string group_name(int g) { return std::string("MacinTalk ") + kGens[g].title; }

//: Whether generation `g` has voice `v`: Alex came with Leopard.
bool in_generation(int g, int v) { return g > 0 || std::strcmp(kVoices[v].bundle, "Alex") != 0; }

// -- tuning, measured ---------------------------------------------------------

//: How far each voice sings from the pitch it is asked for, in cents, at
//: every third semitone of the pitch asked for from kTuneLow (MIDI) up,
//: measured with the dither below in place; and the [[volm]] the voice is
//: sent and the gain that brings it to the common level (a phrase of
//: "Daisy" in the voice's own range peaking at half of full scale). The
//: voices that make their own pitch have no offsets. Above what an engine
//: can sing (790 Hz for MacinTalk 3) the last measured offset is repeated.
constexpr int kTuneLow = 36;
constexpr int kTuneSteps = 17;   // 36, 39, ... 84
struct Tuning {
    int gen;
    const char *bundle;
    double volm;
    double gain;
    double cents[kTuneSteps];
};
// Measured on Tiger 10.4.0, Leopard 10.5 and Snow Leopard 10.6 by
// tests/engines/macintalk/sing_test.cpp --calibrate, which prints these rows.
const Tuning kTuning[] = {
    {0, "Agnes", 0.250, 2.599, {3.5, 4.6, 6.3, 4.9, 7.0, 8.6, 11.5, 11.6, 13.6, 16.1, 18.8, 23.2, 27.1, 29.8, 44.7, 55.0, 55.0}},
    {0, "Albert", 0.250, 2.382, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {0, "Bahh", 0.250, 1.975, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {0, "Boing", 0.250, 2.888, {-1498.4, -1505.5, -1501.9, -1502.9, -1502.9, -1502.7, -1501.7, -1501.9, -1502.5, -1502.2, -1503.5, -1600.8, -1698.1, -1697.9, -1697.6, -1697.6, -1697.6}},
    {0, "Bruce", 0.250, 2.403, {3.4, 2.8, 4.6, 5.1, 5.5, 5.8, 8.2, 10.0, 11.7, 12.8, 14.7, 18.4, 22.2, 26.3, 33.1, 34.7, 5.6}},
    {0, "Bubbles", 0.250, 1.985, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {0, "Deranged", 0.250, 1.952, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {0, "Fred", 0.250, 1.979, {-12.7, -19.4, -16.3, -16.3, -15.8, -16.7, -16.9, -16.4, -16.6, -16.0, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {0, "Junior", 0.250, 1.963, {-13.0, -19.3, -16.3, -16.3, -15.7, -16.7, -17.0, -16.3, -16.7, -16.0, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {0, "Kathy", 0.250, 2.312, {-12.3, -18.8, -16.4, -16.4, -15.8, -16.6, -16.9, -16.3, -16.6, -15.9, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {0, "Princess", 0.250, 2.000, {-12.9, -19.3, -16.3, -16.4, -15.8, -16.6, -16.9, -16.3, -16.7, -15.9, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {0, "Ralph", 0.250, 1.973, {-13.1, -19.3, -16.3, -16.2, -15.9, -16.6, -16.9, -16.3, -16.6, -15.9, -17.7, -16.9, -15.9, -15.9, -15.5, -15.5, -15.5}},
    {0, "Trinoids", 0.250, 2.000, {-12.1, -18.3, -17.6, -20.7, -20.3, -20.0, -19.7, -19.4, -19.1, -16.1, -19.4, -20.8, -18.3, -15.7, -12.1, -12.1, -12.1}},
    {0, "Vicki", 0.250, 1.491, {2.9, 3.2, 4.1, 4.2, 4.9, 5.7, 7.9, 9.1, 11.0, 12.2, 14.2, 18.0, 21.8, 25.8, 34.4, 43.1, 43.1}},
    {0, "Victoria", 0.250, 2.094, {2.8, 3.0, 4.9, 4.7, 5.5, 6.7, 8.8, 8.8, 10.7, 12.0, 14.0, 17.5, 21.2, 25.3, 32.2, 37.7, 37.7}},
    {0, "Whisper", 0.250, 3.693, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {0, "Zarvox", 0.250, 1.960, {-5.6, -11.2, -9.8, -9.6, -6.4, -9.9, -10.6, -7.8, -9.4, -9.3, -9.8, -10.8, -9.7, -8.6, -7.7, -7.7, -7.7}},
    {1, "Agnes", 0.250, 2.599, {3.9, 4.4, 5.9, 5.7, 5.8, 8.7, 11.0, 12.3, 13.8, 14.0, 17.4, 20.9, 20.5, 27.4, 35.0, 31.4, 31.4}},
    {1, "Albert", 0.250, 4.539, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {1, "Bahh", 0.250, 3.761, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {1, "Boing", 0.250, 5.405, {-1498.3, -1505.6, -1502.1, -1503.0, -1502.8, -1502.2, -1502.1, -1501.9, -1502.7, -1502.0, -1503.5, -1600.8, -1698.1, -1697.8, -1697.5, -1697.5, -1697.5}},
    {1, "Bruce", 0.250, 2.403, {2.8, 3.1, 5.1, 5.2, 5.9, 5.7, 8.0, 9.8, 11.7, 12.1, 14.8, 17.5, 21.5, 26.4, 31.2, 35.4, 11.6}},
    {1, "Bubbles", 0.250, 3.760, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {1, "Deranged", 0.250, 3.718, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {1, "Fred", 0.250, 3.767, {-12.9, -19.2, -16.4, -16.3, -15.9, -16.6, -16.9, -16.3, -16.6, -16.0, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {1, "Junior", 0.250, 3.739, {-13.1, -19.3, -16.2, -16.3, -15.8, -16.7, -17.0, -16.3, -16.7, -16.0, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {1, "Kathy", 0.250, 4.605, {-12.6, -19.2, -15.9, -16.3, -16.0, -16.5, -16.9, -16.3, -16.6, -15.9, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {1, "Princess", 0.250, 3.810, {-13.0, -19.2, -16.1, -16.2, -15.9, -16.6, -16.9, -16.3, -16.6, -15.9, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {1, "Ralph", 0.250, 3.759, {-13.0, -19.3, -16.2, -16.3, -15.9, -16.7, -16.9, -16.3, -16.6, -15.9, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {1, "Trinoids", 0.250, 3.810, {-12.1, -18.3, -17.6, -20.7, -20.3, -20.0, -19.7, -19.4, -19.1, -16.1, -19.4, -20.8, -18.3, -15.7, -12.1, -12.1, -12.1}},
    {1, "Vicki", 0.250, 2.929, {2.6, 2.9, 3.0, 3.9, 4.5, 5.4, 7.2, 8.1, 9.7, 11.6, 13.3, 17.0, 20.7, 25.6, 30.1, 39.1, 58.6}},
    {1, "Victoria", 0.250, 2.193, {3.0, 3.1, 3.9, 5.0, 5.8, 7.0, 8.7, 8.8, 11.0, 11.8, 13.7, 16.8, 20.1, 25.1, 29.0, 34.9, 16.0}},
    {1, "Whisper", 0.250, 6.835, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {1, "Zarvox", 0.250, 3.734, {-5.6, -11.2, -9.8, -9.6, -6.4, -9.9, -10.6, -7.8, -9.4, -9.3, -9.8, -10.8, -9.7, -8.6, -7.7, -7.7, -7.7}},
    {2, "Agnes", 0.250, 2.599, {3.9, 4.4, 5.9, 5.7, 5.8, 8.7, 11.0, 12.3, 13.8, 14.0, 17.4, 20.9, 20.5, 27.4, 35.0, 31.4, 31.4}},
    {2, "Albert", 0.250, 4.539, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Bahh", 0.250, 3.761, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Boing", 0.250, 5.405, {-1498.3, -1505.6, -1502.1, -1503.0, -1502.8, -1502.2, -1502.1, -1501.9, -1502.7, -1502.0, -1503.5, -1600.8, -1698.1, -1697.8, -1697.5, -1697.5, -1697.5}},
    {2, "Bruce", 0.250, 2.403, {2.8, 3.1, 5.1, 5.2, 5.9, 5.7, 8.0, 9.8, 11.7, 12.1, 14.8, 17.5, 21.5, 26.4, 31.2, 35.4, 11.6}},
    {2, "Bubbles", 0.250, 3.760, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Deranged", 0.250, 3.718, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Fred", 0.250, 3.767, {-12.9, -19.2, -16.4, -16.3, -15.9, -16.6, -16.9, -16.3, -16.6, -16.0, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {2, "Junior", 0.250, 3.739, {-13.1, -19.3, -16.2, -16.3, -15.8, -16.7, -17.0, -16.3, -16.7, -16.0, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {2, "Kathy", 0.250, 4.605, {-12.6, -19.2, -15.9, -16.3, -16.0, -16.5, -16.9, -16.3, -16.6, -15.9, -17.7, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {2, "Princess", 0.250, 3.810, {-13.0, -19.2, -16.1, -16.2, -15.9, -16.6, -16.9, -16.3, -16.6, -15.9, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {2, "Ralph", 0.250, 3.759, {-13.0, -19.3, -16.2, -16.3, -15.9, -16.7, -16.9, -16.3, -16.6, -15.9, -17.6, -16.9, -15.9, -15.9, -15.4, -15.4, -15.4}},
    {2, "Trinoids", 0.250, 3.810, {-12.1, -18.3, -17.6, -20.7, -20.3, -20.0, -19.7, -19.4, -19.1, -16.1, -19.4, -20.8, -18.3, -15.7, -12.1, -12.1, -12.1}},
    {2, "Vicki", 0.250, 3.268, {2.6, 2.9, 3.0, 3.9, 4.5, 5.4, 7.2, 8.1, 9.7, 11.6, 13.3, 17.0, 20.7, 25.6, 30.1, 39.1, 58.6}},
    {2, "Victoria", 0.250, 2.193, {3.0, 3.1, 3.9, 5.0, 5.8, 7.0, 8.7, 8.8, 11.0, 11.8, 13.7, 16.8, 20.1, 25.1, 29.0, 34.9, 16.0}},
    {2, "Whisper", 0.250, 6.835, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Zarvox", 0.250, 3.734, {-5.6, -11.2, -9.8, -9.6, -6.4, -9.9, -10.6, -7.8, -9.4, -9.3, -9.8, -10.8, -9.7, -8.6, -7.7, -7.7, -7.7}},    // Alex: his loudness; his pitch is measured as he sings and put right
    // (true_pitch), so no offsets
    {1, "Alex", 0.250, 4.876, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
    {2, "Alex", 0.250, 3.563, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
};

#ifdef _WIN32
const Tuning *find_tuning(int g, const char *bundle) {
    for (const Tuning &t : kTuning)
        if (t.gen == g && std::strcmp(t.bundle, bundle) == 0) return &t;
    return nullptr;
}
#endif

// When set, no tuning is applied: the test program's --calibrate measures
// the engines as they are.
bool g_raw = false;

// -- the phonemes ---------------------------------------------------------------

// How one of VocalWriter's phonemes becomes Apple's: one phoneme, or two
// that share its time.
enum Shape {
    kOne,     // `first` for the whole length
    kTail,    // `first` held, `second` over the end (diphthongs, r-colouring)
    kHead,    // a short `first`, then `second` held (the y of YU, the vowel of ER)
    kSplit,   // `first` takes `share` of the length, `second` the rest
};

struct Mapping {
    const char *symbol;
    Shape shape;
    const char *first;
    const char *second;
    double share;
};

// VocalWriter's fifty-seven phonemes in Apple's American English set (the
// Speech Synthesis Programming Guide's table: AE EY AO AX IY EH IH AY IX AA UW
// UH UX OW AW OY, b C d D f g h J k l m n N p r s S t T v w y z Z, and % for
// silence). Each symbol was checked against what the engines accept and what
// they sing -- the vowels by their formants on Fred, Victoria and Bruce.
//
// Apple's diphthongs glide over most of their length whatever it is, so a
// long one is sung as its first vowel held and Apple's own diphthong over the
// end (kTail), as a singer holds the "ah" of "my" and closes only at the end.
// OW is left whole: its glide is slight and slow. Apple has no r-coloured
// vowels as phonemes; its own rules colour a vowel before r, and "UX r" is
// the vowel of "bird" through and through, so ER holds the r after a moment
// of UX. The other r-coloured vowels are the vowel held and r at the end, as
// Microsoft's voices spell them. Apple has no glottal stop or flap: they are
// sung as t and d.
const Mapping kMap[] = {
    // vowels
    {"IY", kOne, "IY", nullptr, 0},        // bEEt
    {"IH", kOne, "IH", nullptr, 0},        // bIt
    {"EH", kOne, "EH", nullptr, 0},        // bEt
    {"AE", kOne, "AE", nullptr, 0},        // bAt
    {"AA", kOne, "AA", nullptr, 0},        // hOt, fAther
    {"UX", kOne, "UX", nullptr, 0},        // bUd: Apple's UX is the same vowel
    {"AO", kOne, "AO", nullptr, 0},        // cAUght
    {"UH", kOne, "UH", nullptr, 0},        // bOOk
    {"AX", kOne, "AX", nullptr, 0},        // About
    {"IX", kOne, "IX", nullptr, 0},        // rosEs
    {"O", kOne, "AO", nullptr, 0},         // bOy's first half
    // diphthongs
    {"EY", kTail, "EH", "EY", 0},          // bAIt
    {"AY", kTail, "AA", "AY", 0},          // bIte
    {"OY", kTail, "AO", "OY", 0},          // bOY
    {"AW", kTail, "AA", "AW", 0},          // bOUt
    {"OW", kOne, "OW", nullptr, 0},        // bOAt
    {"UW", kOne, "UW", nullptr, 0},        // bOOt
    {"YU", kHead, "y", "UW", 0},           // mUte
    // r-coloured vowels
    {"ER", kHead, "UX", "r", 0},           // bIRd
    {"RX", kHead, "AX", "r", 0},           // buttER
    {"IR", kTail, "IY", "r", 0},           // bEER
    {"XR", kTail, "EH", "r", 0},           // bEAR
    {"AR", kTail, "AA", "r", 0},           // bAR
    {"OR", kTail, "AO", "r", 0},           // fOR
    {"UR", kTail, "UH", "r", 0},           // pOOR
    // syllabic consonants: a little of the vowel, then the consonant
    {"EL", kSplit, "AX", "l", 0.4},        // bottLE
    {"EN", kSplit, "AX", "n", 0.4},        // buttON
    {"LX", kOne, "l", nullptr, 0},         // beLL
    // glides and liquids
    {"w", kOne, "w", nullptr, 0},
    {"y", kOne, "y", nullptr, 0},
    {"r", kOne, "r", nullptr, 0},
    {"l", kOne, "l", nullptr, 0},
    {"h", kOne, "h", nullptr, 0},
    // nasals
    {"m", kOne, "m", nullptr, 0},
    {"n", kOne, "n", nullptr, 0},
    {"NG", kOne, "N", nullptr, 0},
    // fricatives
    {"f", kOne, "f", nullptr, 0},
    {"v", kOne, "v", nullptr, 0},
    {"TH", kOne, "T", nullptr, 0},
    {"DH", kOne, "D", nullptr, 0},
    {"s", kOne, "s", nullptr, 0},
    {"z", kOne, "z", nullptr, 0},
    {"SH", kOne, "S", nullptr, 0},
    {"ZH", kOne, "Z", nullptr, 0},
    // stops
    {"p", kOne, "p", nullptr, 0},
    {"b", kOne, "b", nullptr, 0},
    {"t", kOne, "t", nullptr, 0},
    {"d", kOne, "d", nullptr, 0},
    {"k", kOne, "k", nullptr, 0},
    {"g", kOne, "g", nullptr, 0},
    {"TX", kOne, "t", nullptr, 0},         // iT
    {"DD", kOne, "d", nullptr, 0},         // beTTer: the flap
    {"Q", kOne, "t", nullptr, 0},          // the glottal stop of greaTer
    {"QX", kOne, "t", nullptr, 0},         // a longer Q
    // affricates
    {"CH", kOne, "C", nullptr, 0},         // CHin
    {"JH", kOne, "J", nullptr, 0},         // Joy
    // silence
    {"%", kOne, "%", nullptr, 0},
};

const Mapping *find_mapping(const std::string &symbol) {
    std::string s = palette_alias(symbol);
    for (const Mapping &m : kMap)
        if (s == m.symbol) return &m;
    return nullptr;
}

// Apple's phonemes, and what they are.
const char *const kAppleVowels[] = {"AE", "EY", "AO", "AX", "IY", "EH", "IH", "AY",
                                    "IX", "AA", "UW", "UH", "UX", "OW", "AW", "OY"};
const char *const kAppleConsonants[] = {"b", "C", "d", "D", "f", "g", "h", "J", "k", "l", "m", "n",
                                        "N", "p", "r", "s", "S", "t", "T", "v", "w", "y", "z", "Z"};

bool apple_vowel(const std::string &p) {
    for (const char *v : kAppleVowels)
        if (p == v) return true;
    return false;
}
bool apple_consonant(const std::string &p) {
    for (const char *c : kAppleConsonants)
        if (p == c) return true;
    return false;
}
#ifdef _WIN32
// the lax vowels MacinTalk Pro will not end a syllable on after another
bool lax(const std::string &p) { return p == "AE" || p == "EH" || p == "IH" || p == "IX"; }
bool lax_before(const std::string &p) { return lax(p) || p == "UH" || p == "UX"; }
// MacinTalk 3 gives these five frames more when a silence follows
bool closes(const std::string &p) {
    return p == "b" || p == "d" || p == "g" || p == "p" || p == "t" || p == "k" || p == "m" ||
           p == "n" || p == "N";
}
#endif

// A diphthong's or r's tail, and a short head, as parts of the sound.
const double kTailShare = 0.4, kTailMin = 0.080, kTailMax = 0.250;
const double kRTailShare = 0.3, kRTailMin = 0.060, kRTailMax = 0.150;
const double kHeadShare = 0.2, kHeadMin = 0.025, kHeadMax = 0.070;
// A diphthong shorter than this is Apple's own diphthong whole.
const double kWholeDiphthong = 0.200;

// -- the engines' measured behaviour --------------------------------------------

constexpr double kEngineRate = 22050.0;
//: MacinTalk 3's frame, in samples, and what a D of 5 ms asks for
constexpr int kMtkFrame = 112;
//: the most pitch points a line is followed for: from the twelfth on, the
//: engine goes back to its own speaking intonation, however long the line
//: (measured: eleven points across two seconds are followed to the end)
constexpr int kMaxPoints = 11;
//: how near the straight lines between a line's points come to the curve
constexpr double kTraceCents = 10.0;
//: and how close two of them may be: Snow Leopard's Vicki takes the host
//: down now and then when a short line is crowded with points
constexpr double kMinGap = 0.020;
//: the longest line Vicki is sent: a longer one takes Snow Leopard's host
//: down about one time in three (none in forty-four up to 900 ms)
constexpr double kMeowMaxLine = 0.8;
//: the speaking rate sent: at 180 MacinTalk Pro and the concatenative engine
//: give a silence exactly its D
constexpr int kRate = 180;
//: how many times a MacinTalk Pro phrase may be sung to find its lengths
constexpr int kMaxPasses = 5;
//: and how near its notes must come to stop early
constexpr double kGoodEnough = 0.0025;
//: what a host may take over a phrase before it is taken for hung
constexpr unsigned kReplyTimeoutMs = 60000;

// -- small helpers ------------------------------------------------------------------

// Numbers for the engine, written without the C library: a locale with a
// decimal comma (the window may set one) must not reach MacinTalk.
std::string fixed(double v, int decimals) {
    bool neg = v < 0;
    double scale = std::pow(10.0, decimals);
    long long n = (long long)std::llround(std::fabs(v) * scale);
    long long whole = n / (long long)scale, frac = n % (long long)scale;
    std::string s = (neg && n ? "-" : "") + std::to_string(whole);
    if (decimals > 0) {
        std::string f = std::to_string(frac);
        while (int(f.size()) < decimals) f = "0" + f;
        s += "." + f;
    }
    return s;
}

#ifdef _WIN32
bool is_dir(const std::string &p) { return dir_exists(p); }

std::vector<std::string> subdirs(const std::string &dir) {
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(widen(join_path(dir, "*")).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        std::string n = narrow(fd.cFileName);
        if (n == "." || n == "..") continue;
        out.push_back(join_path(dir, n));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

std::string read_line_file(const std::string &path) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b)) return std::string();
    std::string s(b.begin(), b.end());
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB) s = s.substr(3);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t'))
        s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    return s;
}

std::string env(const char *name) {
    const wchar_t *v = _wgetenv(widen(name).c_str());
    return v ? narrow(v) : std::string();
}

// NVDA's configuration folders: the installed copy's, and that of any
// portable copy that is running (it keeps its configuration in userConfig
// beside nvda.exe). A copy started with -c somewhere else can be found by
// pointing $NVDA_CONFIG at it.
std::vector<std::string> nvda_configs() {
    std::vector<std::string> out;
    std::string named = env("NVDA_CONFIG");
    if (!named.empty()) out.push_back(named);
    std::string app = env("APPDATA");
    if (app.empty() || !is_dir(app)) {
        // started by something that rewrote the environment (msys2 does)
        wchar_t buf[MAX_PATH];
        if (SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf) == S_OK) app = narrow(buf);
    }
    if (!app.empty()) out.push_back(join_path(app, "nvda"));
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof pe;
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
            if (_wcsicmp(pe.szExeFile, L"nvda.exe") != 0) continue;
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!p) continue;
            wchar_t path[MAX_PATH * 2];
            DWORD n = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(p, 0, path, &n)) {
                std::string exe = narrow(std::wstring(path, n));
                size_t cut = exe.find_last_of("\\/");
                if (cut != std::string::npos) {
                    std::string user = join_path(exe.substr(0, cut), "userConfig");
                    if (is_dir(user) && std::find(out.begin(), out.end(), user) == out.end())
                        out.push_back(user);
                }
            }
            CloseHandle(p);
        }
        CloseHandle(snap);
    }
    return out;
}

bool is_tree(const std::string &p) { return !p.empty() && is_dir(join_path(p, "Speech\\Voices")); }

std::string find_runtime(const std::string &tree, const char *const *names, int count) {
    for (const char *sub : {"", "usr\\lib"})
        for (int i = 0; i < count && names[i]; ++i) {
            std::string p = *sub ? join_path(join_path(tree, sub), names[i]) : join_path(tree, names[i]);
            if (file_exists(p)) return p;
        }
    return std::string();
}

// Whether a Mach-O file calls Apple's "Don't Steal Mac OS X" routine, as
// MacinTalk 3.4 (Mac OS X 10.4.5 on) does; off Apple's hardware it does not
// answer and the engine dies (pantheratiger.needs_dsmos).
bool needs_dsmos(const std::string &path) {
    std::vector<unsigned char> b;
    if (!read_file(path, &b)) return false;
    static const char sym[] = "___commpage_dsmos";
    const size_t n = sizeof sym - 1;
    if (b.size() < n) return false;
    return std::search(b.begin(), b.end(), sym, sym + n) != b.end();
}

// Windows N editions have no AAC decoder until the Media Feature Pack is
// installed, and Vicki's samples are AAC (pantheratrees.aac_available): absent
// from both views of the registry is a clear no; anything else is a yes.
bool aac_available() {
    const wchar_t *key = L"CLSID\\{32D186A7-218F-4C75-8876-DD77273A8999}";
    int missing = 0;
    for (REGSAM view : {REGSAM(KEY_WOW64_32KEY), REGSAM(KEY_WOW64_64KEY)}) {
        HKEY h;
        LONG r = RegOpenKeyExW(HKEY_CLASSES_ROOT, key, 0, KEY_READ | view, &h);
        if (r == ERROR_SUCCESS) {
            RegCloseKey(h);
            return true;
        }
        if (r == ERROR_FILE_NOT_FOUND) ++missing; else return true;
    }
    return missing < 2;
}
#endif

// -- the host ----------------------------------------------------------------------

#ifdef _WIN32
// One panthera_host.exe serving one generation. Used under its own lock; a
// host that fails is killed and the next request starts another.
class Host {
public:
    Host(std::string exe, std::string mt, std::string sd, std::string vd)
        : exe_(std::move(exe)), mt_(std::move(mt)), sd_(std::move(sd)), vd_(std::move(vd)) {}
    ~Host() { stop(); }

    //: Sing `text` with `voice`; the samples, or false with `err` set.
    bool say(const std::string &voice, const std::string &text, std::vector<int16_t> *pcm,
             std::string *err) {
        std::lock_guard<std::mutex> lock(mu_);
        // Twice more after a failure: a host that died of the engine's own
        // heap trouble usually sings the same phrase the next time.
        std::string why;
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (!proc_ && !start(&why)) break;
            int status = 0;
            if (exchange(voice, text, pcm, &status, &why)) return true;
            stop();
        }
        if (err) *err = why;
        return false;
    }

private:
    bool start(std::string *err) {
        // Its own job, so that the host goes when this program does, however
        // this program goes.
        if (!job_) {
            job_ = CreateJobObjectW(nullptr, nullptr);
            if (job_) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
                std::memset(&li, 0, sizeof li);
                li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &li, sizeof li);
            }
        }
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;
        HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr;
        if (!CreatePipe(&in_r, &in_w, &sa, 1 << 16) || !CreatePipe(&out_r, &out_w, &sa, 1 << 20)) {
            if (in_r) CloseHandle(in_r);
            if (in_w) CloseHandle(in_w);
            if (err) *err = "cannot make the pipes to talk to Panthera's host";
            return false;
        }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        // The host talks about what it is doing on its standard error; nobody
        // reads it here, so it goes nowhere rather than filling a pipe.
        HANDLE nul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, 0, nullptr);
        STARTUPINFOW si;
        std::memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = nul != INVALID_HANDLE_VALUE ? nul : out_w;
        PROCESS_INFORMATION pi;
        std::memset(&pi, 0, sizeof pi);
        auto quote = [](const std::string &s) { return "\"" + s + "\""; };
        std::wstring cmd = widen(quote(exe_) + " --serve " + quote(mt_) + " " + quote(sd_) + " " + quote(vd_));
        std::vector<wchar_t> line(cmd.begin(), cmd.end());
        line.push_back(0);
        // The host's verbose log is only for Panthera's own debugging.
        SetEnvironmentVariableW(L"TIGER_HOST_VERBOSE", nullptr);
        BOOL ok = CreateProcessW(widen(exe_).c_str(), line.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
        if (!ok) {
            CloseHandle(in_w);
            CloseHandle(out_r);
            if (err) *err = "Panthera's host, " + exe_ + ", would not start (error " +
                            std::to_string(GetLastError()) + ")";
            return false;
        }
        if (job_) AssignProcessToJobObject(job_, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        in_ = in_w;
        out_ = out_r;
        return true;
    }

    void stop() {
        if (in_) CloseHandle(in_);
        if (out_) CloseHandle(out_);
        if (proc_) {
            TerminateProcess(proc_, 1);
            WaitForSingleObject(proc_, 2000);
            CloseHandle(proc_);
        }
        proc_ = in_ = out_ = nullptr;
    }

    bool write_all(const std::string &bytes) {
        size_t at = 0;
        while (at < bytes.size()) {
            DWORD n = 0;
            if (!WriteFile(in_, bytes.data() + at, DWORD(bytes.size() - at), &n, nullptr) || n == 0)
                return false;
            at += n;
        }
        return true;
    }

    // Exactly `n` bytes, or false if the host dies or goes quiet for too long.
    bool read_all(void *dst, size_t n, std::string *err) {
        char *p = static_cast<char *>(dst);
        size_t got = 0;
        DWORD quiet_since = GetTickCount();
        while (got < n) {
            DWORD avail = 0;
            if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr)) {
                if (err) *err = "Panthera's host closed its pipe (it may have crashed)";
                return false;
            }
            if (avail == 0) {
                if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) {
                    // it may have written its last bytes and gone
                    if (PeekNamedPipe(out_, nullptr, 0, nullptr, &avail, nullptr) && avail) continue;
                    if (err) *err = "Panthera's host stopped part way through (it may have crashed)";
                    return false;
                }
                if (GetTickCount() - quiet_since > kReplyTimeoutMs) {
                    if (err) *err = "Panthera's host stopped answering";
                    return false;
                }
                Sleep(1);
                continue;
            }
            DWORD r = 0;
            DWORD want = DWORD(std::min<size_t>(avail, n - got));
            if (!ReadFile(out_, p + got, want, &r, nullptr) || r == 0) {
                if (err) *err = "Panthera's host closed its pipe (it may have crashed)";
                return false;
            }
            got += r;
            quiet_since = GetTickCount();
        }
        return true;
    }

    bool exchange(const std::string &voice, const std::string &text, std::vector<int16_t> *pcm,
                  int *status, std::string *err) {
        auto u32 = [](std::string &s, uint32_t v) {
            for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
        };
        std::string req;
        u32(req, 0x54475233u);           // 'TGR3'
        u32(req, uint32_t(int32_t(kRate)));
        u32(req, 0);                     // the voice's own pitch
        u32(req, 0);                     // flags: the text as it is
        u32(req, uint32_t(voice.size()));
        u32(req, uint32_t(text.size()));
        req += voice;
        req += text;
        if (!write_all(req)) {
            if (err) *err = "Panthera's host is not taking requests (it may have crashed)";
            return false;
        }
        unsigned char head[12];
        if (!read_all(head, sizeof head, err)) return false;
        auto rd = [&](int at) {
            return uint32_t(head[at]) | uint32_t(head[at + 1]) << 8 | uint32_t(head[at + 2]) << 16 |
                   uint32_t(head[at + 3]) << 24;
        };
        if (rd(0) != 0x54475253u) {      // 'TGRS'
            if (err) *err = "Panthera's host answered in a form this program does not know";
            return false;
        }
        *status = int32_t(rd(4));
        uint32_t frames = rd(8);
        if (frames > 22050u * 3600u) {
            if (err) *err = "Panthera's host answered with an impossible length";
            return false;
        }
        pcm->assign(frames, 0);
        if (frames && !read_all(pcm->data(), size_t(frames) * 2, err)) return false;
        return true;
    }

    std::string exe_, mt_, sd_, vd_;
    std::mutex mu_;
    HANDLE proc_ = nullptr, in_ = nullptr, out_ = nullptr, job_ = nullptr;
};
#endif

// -- what is installed ----------------------------------------------------------------

struct GenState {
    bool tree_found = false;
    bool usable = false;          //: the host can run it
    std::string tree, mt, sd, vd;
    std::string problem;          //: why not, as a sentence
    bool voice_present[kVoiceCount] = {};
};

struct Install {
    std::string host;             //: panthera_host.exe, or empty
    GenState gens[kGenCount];
    bool aac = true;
};

// The folder named in the settings, and how many times it has been named:
// the family looks again when that has changed.
std::mutex g_folder_mu;
std::string g_folder;
unsigned g_folder_version = 0;

std::string named_folder(unsigned *version) {
    std::lock_guard<std::mutex> lock(g_folder_mu);
    if (version) *version = g_folder_version;
    return g_folder;
}

#ifdef _WIN32
Install survey() {
    Install in;
    std::vector<std::string> configs = nvda_configs();
    std::vector<std::string> own;   // voices\macintalk under the data roots
    for (const std::string &r : data_roots()) own.push_back(join_path(r, "voices\\macintalk"));
    // The folder in the settings first, taken as either: NVDA's configuration
    // folder, or one laid out as voices\macintalk is.
    std::string named = named_folder(nullptr);
    if (!named.empty()) {
        configs.insert(configs.begin(), named);
        own.insert(own.begin(), named);
    }

    // the host: in Panthera's NVDA add-on, or put beside the program
    std::vector<std::string> hosts;
    for (const std::string &c : configs)
        hosts.push_back(join_path(c, "addons\\pantheraspeech\\synthDrivers\\_panthera\\panthera_host.exe"));
    for (const std::string &o : own) hosts.push_back(join_path(o, "panthera_host.exe"));
    for (const std::string &p : hosts)
        if (file_exists(p)) {
            in.host = p;
            break;
        }
    in.aac = aac_available();

    for (int g = 0; g < kGenCount; ++g) {
        const GenDef &gd = kGens[g];
        GenState &st = in.gens[g];
        std::vector<std::string> cands;
        std::string e = env(gd.env);
        if (!e.empty()) cands.push_back(e);
        auto with_children = [&](const std::string &root) {
            cands.push_back(root);
            for (const std::string &s : subdirs(root)) cands.push_back(s);
        };
        for (const std::string &o : own) {
            with_children(join_path(o, gd.key));
            std::string named = read_line_file(join_path(o, std::string(gd.key) + ".txt"));
            if (!named.empty()) cands.push_back(named);
        }
        for (const std::string &c : configs) {
            with_children(join_path(join_path(c, "macintalk"), gd.key));
            for (const char *l : gd.legacy)
                if (l) with_children(join_path(c, l));
            for (const char *ptr : gd.pointers) {
                if (!ptr) continue;
                std::string named = read_line_file(join_path(c, ptr));
                if (!named.empty()) cands.push_back(named);
            }
        }
        for (const std::string &c : cands)
            if (is_tree(c)) {
                st.tree = c;
                st.tree_found = true;
                break;
            }
        if (!st.tree_found) continue;
        st.mt = join_path(st.tree, "Speech\\Synthesizers\\MacinTalk.SpeechSynthesizer\\Contents\\MacOS\\MacinTalk");
        st.sd = join_path(st.tree, "SpeechDictionary.framework\\Versions\\A\\SpeechDictionary");
        st.vd = join_path(st.tree, "Speech\\Voices");
        for (int v = 0; v < kVoiceCount; ++v) {
            std::string desc = join_path(st.vd, std::string(kVoices[v].bundle) +
                                                    ".SpeechVoice\\Contents\\Resources\\VoiceDescription");
            std::vector<unsigned char> b;
            st.voice_present[v] = file_exists(desc) && read_file(desc, &b) && b.size() >= 80;
        }
        std::string where = " in " + st.tree;
        if (!file_exists(st.mt)) {
            st.problem = std::string("Mac OS X ") + gd.release + "'s MacinTalk engine is missing" + where;
        } else if (!file_exists(st.sd)) {
            st.problem = std::string("Mac OS X ") + gd.release + "'s SpeechDictionary is missing" + where;
        } else if (gd.runtime[0] && find_runtime(st.tree, gd.runtime, 3).empty()) {
            st.problem = std::string("Mac OS X ") + gd.release + "'s engine needs Apple's C++ runtime, " +
                         gd.runtime[0] + ", from the same disc's usr/lib, beside it" + where;
        } else if (gd.abi && find_runtime(st.tree, &gd.abi, 1).empty()) {
            st.problem = std::string("Mac OS X ") + gd.release + "'s engine needs Apple's " + gd.abi +
                         ", from the same disc's usr/lib, beside it" + where;
        } else if (g == 0 && (needs_dsmos(st.mt) || needs_dsmos(st.sd))) {
            st.problem = "this Tiger is MacinTalk 3.4, from Mac OS X 10.4.5 or later, which calls Apple's "
                         "copy protection and cannot run off Apple's hardware; an earlier Tiger disc "
                         "(MacinTalk 3.3) or Leopard has the same voices";
        } else if (!gd.tune) {
            st.problem = std::string("Mac OS X ") + gd.release + "'s MacinTalk takes Apple's TUNE "
                         "notation but ignores the lengths and pitches in it, so Lion's voices cannot "
                         "sing; Leopard and Snow Leopard have the same voices";
        } else {
            st.usable = true;
        }
    }
    return in;
}
#endif

// The lowest and highest pitches each engine sings: MacinTalk 3 holds 50 Hz
// below 50, and from 800 Hz up sings a pitch some way below the one asked.
double lowest_hz(Engine e) { return e == kMtk3 ? 50.0 : 45.0; }
double highest_hz(Engine e) { return e == kMtk3 ? 790.0 : 1200.0; }
// Boing takes the host down with it when a long line is pitched above about
// 520 Hz (three eighths of that is what it sings, so its notes stop near F#3).
double highest_hz(const VoiceDef &v) {
    return std::strcmp(v.bundle, "Boing") == 0 ? 500.0 : highest_hz(v.engine);
}
// The notes a voice sings inside those pitches: G#1 to G5 on MacinTalk 3,
// F#1 to D6 on the others, and up to F#3 for Boing. A phrase outside goes
// whole octaves into them.
void fit_voice(const VoiceDef &v, Phrase *phrase) {
    double sung = std::strcmp(v.bundle, "Boing") == 0 ? 3.0 / 8.0 : 1.0;
    auto note = [sung](double hz) { return 69.0 + 12.0 * std::log2(hz * sung / 440.0); };
    fit_octaves(phrase, std::ceil(note(lowest_hz(v.engine))), std::floor(note(highest_hz(v))));
}

#ifdef _WIN32   // everything from here to the family sings through Panthera

// -- a phrase as TUNE lines ----------------------------------------------------------

struct Line {
    std::string ph;        //: Apple's phoneme, "%" for silence
    bool stress = false;
    bool separator = false;   //: a zero-length silence that keeps two vowels apart
    double t0 = 0, t1 = 0;    //: where it belongs in the phrase, in seconds
    int note = 0;
    double d = 0;             //: the D sent, in ms
    int frames = 0;           //: MacinTalk 3's frames
};

bool silent(const Line &l) { return l.ph == "%"; }

// The engines follow a line's pitch points some milliseconds late (8 for
// MacinTalk 3, 10 for the others, measured on a vibrato), so the curve is
// read that much ahead -- inside the line: a new note's pitch still starts
// with its line, which the engines do on time.
double lead(Engine e) { return e == kMtk3 ? 0.008 : 0.010; }

// Where the pitch points of a sound from t0 to t1 go, as fractions of it:
// the ends, then wherever a straight line between those chosen so far is
// furthest from the curve, until every straight line is within kTraceCents
// of it or there are `most` of them -- and never two closer than kMinGap.
std::vector<double> trace(double t0, double t1, const PitchCurve &curve, Engine engine, int most) {
    double len = t1 - t0;
    most = std::max(2, std::min(most, 1 + int(len / kMinGap)));
    const int fine = std::max(2, int(len / 0.005) + 1);
    std::vector<double> c(static_cast<size_t>(fine), 0.0);
    for (int i = 0; i < fine; ++i) {
        double t = std::min(t0 + len * i / (fine - 1) + lead(engine), t1 - std::min(0.001, 0.25 * len));
        c[size_t(i)] = std::log2(curve.hz_at(t));
    }
    std::vector<int> pick = {0, fine - 1};
    while (int(pick.size()) < most) {
        double worst = -1;
        int where = -1;
        for (size_t p = 0; p + 1 < pick.size(); ++p) {
            int a = pick[p], b = pick[p + 1];
            for (int i = a + 1; i < b; ++i) {
                double line = c[size_t(a)] + (c[size_t(b)] - c[size_t(a)]) * (i - a) / double(b - a);
                double e = std::fabs(c[size_t(i)] - line);
                if (e > worst) {
                    worst = e;
                    where = i;
                }
            }
        }
        if (where < 0 || worst * 1200.0 < kTraceCents) break;
        pick.insert(std::upper_bound(pick.begin(), pick.end(), where), where);
    }
    std::vector<double> at;
    for (int i : pick) at.push_back(double(i) / (fine - 1));
    return at;
}

// Whether a sound can be sent as several lines of itself.
bool can_split(const std::string &ph, Engine engine) {
    if (ph == "AY" || ph == "AW" || ph == "OY" || ph == "EY" || ph == "OW") return false;
    return !(engine == kPro && lax(ph));
}

// The phrase's phonemes in Apple's terms, each at its time.
std::vector<Line> plan_lines(const Phrase &phrase, const PitchCurve &curve, Engine engine) {
    std::vector<Line> out;
    auto push = [&](const char *ph, double a, double b, int note, bool stress) {
        if (b <= a) return;
        Line l;
        l.ph = ph;
        l.t0 = a;
        l.t1 = b;
        l.note = note;
        l.stress = stress && apple_vowel(ph);
        out.push_back(l);
    };
    for (size_t i = 0; i < phrase.notes.size(); ++i) {
        const SungNote &note = phrase.notes[i];
        double t0 = curve.note_start(i), len = curve.note_end(i) - t0;
        std::vector<double> ms = note.durations_ms;
        if (ms.size() != note.phonemes.size()) ms = syllable_lengths(note.phonemes, len * 1000.0);
        double sum = 0.0;
        for (double v : ms) sum += std::max(0.0, v);
        if (note.phonemes.empty() || sum <= 0.0) {
            push("%", t0, t0 + len, int(i), false);
            continue;
        }
        int held = held_phoneme(note.phonemes);
        // the lengths stretched or squeezed to fill the note exactly
        double k = len / (sum / 1000.0), at = t0;
        for (size_t j = 0; j < note.phonemes.size(); ++j) {
            double d = std::max(0.0, ms[j]) / 1000.0 * k;
            double end = j + 1 == note.phonemes.size() ? t0 + len : at + d;
            bool stress = int(j) == held;
            const Mapping *m = find_mapping(note.phonemes[j]);
            if (!m) {
                push("%", at, end, int(i), false);
            } else if (m->shape == kOne || (m->shape == kTail && m->second && apple_vowel(m->second) &&
                                             end - at < kWholeDiphthong)) {
                // a short diphthong is Apple's own, whole
                push(m->shape == kOne ? m->first : m->second, at, end, int(i), stress);
            } else {
                double span = end - at, a;
                if (m->shape == kTail) {
                    bool r = std::strcmp(m->second, "r") == 0;
                    double tail = r ? std::max(kRTailMin, std::min(kRTailMax, kRTailShare * span))
                                    : std::max(kTailMin, std::min(kTailMax, kTailShare * span));
                    a = span - std::min(0.6 * span, tail);
                } else if (m->shape == kHead) {
                    a = std::min(0.5 * span, std::max(kHeadMin, std::min(kHeadMax, kHeadShare * span)));
                } else {
                    a = m->share * span;
                }
                // The stress goes on the vowel that is held: the first part of
                // a diphthong, the second of YU and ER.
                bool first_vowel = apple_vowel(m->first);
                push(m->first, at, at + a, int(i), stress && (m->shape != kHead || !apple_vowel(m->second)) && first_vowel);
                push(m->second, at + a, end, int(i), stress && (m->shape == kHead || !first_vowel));
            }
            at = end;
        }
    }

    // MacinTalk Pro: a lax vowel with another vowel or a silence after it is
    // dropped after another lax vowel, and UH with no consonant on either
    // side; a zero-length silence before the first keeps it, and UH in that
    // place is sung as UW, the nearest vowel Apple has.
    if (engine == kPro) {
        auto cons = [&](size_t i) { return i < out.size() && apple_consonant(out[i].ph); };
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].ph != "UH") continue;
            bool before = i > 0 && apple_consonant(out[i - 1].ph);
            if (!before && !cons(i + 1)) out[i].ph = "UW";
        }
        std::vector<Line> kept;
        for (size_t i = 0; i < out.size(); ++i) {
            if (i > 0 && lax(out[i].ph) && lax_before(out[i - 1].ph) && !cons(i + 1)) {
                Line s;
                s.ph = "%";
                s.separator = true;
                s.t0 = s.t1 = out[i].t0;
                s.note = out[i].note;
                kept.push_back(s);
            }
            kept.push_back(out[i]);
        }
        out.swap(kept);
    }

    // A sound whose curve needs more points than a line takes is cut into
    // lines of itself, at its own points, ten stretches to a line. A silence
    // needs none; a diphthong cut in two would glide twice, and MacinTalk Pro
    // would drop the second half of a lax vowel. Those keep one line, and
    // the points that best draw the curve.
    std::vector<Line> whole;
    for (const Line &l : out) {
        int n = engine == kMeow && !silent(l) && can_split(l.ph, engine)
                    ? int(std::ceil((l.t1 - l.t0) / kMeowMaxLine - 1e-9))
                    : 1;
        for (int k = 0; k < std::max(1, n); ++k) {
            Line p = l;
            if (n > 1) {
                p.t0 = l.t0 + (l.t1 - l.t0) * k / n;
                p.t1 = k + 1 == n ? l.t1 : l.t0 + (l.t1 - l.t0) * (k + 1) / n;
                p.stress = l.stress && k == 0;
            }
            whole.push_back(p);
        }
    }
    std::vector<Line> split;
    for (const Line &l : whole) {
        if (silent(l) || l.t1 <= l.t0 || !can_split(l.ph, engine)) {
            split.push_back(l);
            continue;
        }
        std::vector<double> at = trace(l.t0, l.t1, curve, engine, 1 << 20);
        double len = l.t1 - l.t0;
        size_t from = 0;
        while (from + 1 < at.size()) {
            size_t to = std::min(at.size() - 1, from + size_t(kMaxPoints - 1));
            Line p = l;
            p.t0 = from == 0 ? l.t0 : l.t0 + len * at[from];
            p.t1 = to + 1 == at.size() ? l.t1 : l.t0 + len * at[to];
            p.stress = l.stress && from == 0;
            split.push_back(p);
            from = to;
        }
    }
    return split;
}

// How far a voice sings from `hz` when asked for `hz`, in cents.
double offset_cents(const Tuning *t, double hz) {
    double midi = 69.0 + 12.0 * std::log2(hz / 440.0);
    double x = (midi - kTuneLow) / 3.0;
    x = std::max(0.0, std::min(double(kTuneSteps - 1), x));
    int i = std::min(kTuneSteps - 2, int(x));
    double f = x - i;
    return t->cents[i] * (1 - f) + t->cents[i + 1] * f;
}

// The pitch a voice is asked for so that it sings `hz`. The table is by the
// pitch asked for, so the answer is found by going round it a few times
// (Boing, a voice that sings three eighths of what it is asked, needs this;
// the rest are there at once).
double tuned_hz(const Tuning *t, double hz) {
    if (!t || g_raw) return hz;
    double ask = hz;
    for (int k = 0; k < 4; ++k) ask = hz * std::pow(2.0, -offset_cents(t, ask) / 1200.0);
    return ask;
}

// Where a line's pitch points go, as fractions of it, and the pitch at each:
// the fewest that draw its curve (trace). The pitch is asked for as it is:
// the engines round it to a step of their own (MacinTalk 3 to a whole hertz,
// the others to a whole pitch period at 22050 Hz), which costs at most half
// a step, and the tuning table takes out what they do on average.
// Points dithered either side of the curve every 25 ms used to average the
// steps out, but at eleven points to a line that cut every long note into
// lines a fifth of a second long, and every cut was heard.
void line_points(const Line &l, const PitchCurve &curve, const VoiceDef &voice, std::vector<double> *at,
                 std::vector<double> *hz) {
    double len = l.t1 - l.t0;
    *at = trace(l.t0, l.t1, curve, voice.engine, kMaxPoints);
    hz->clear();
    for (double f : *at)
        hz->push_back(curve.hz_at(std::min(l.t0 + len * f + lead(voice.engine), l.t1 - std::min(0.001, 0.25 * len))));
}

// The utterance.
std::string tune_text(const std::vector<Line> &lines, const PitchCurve &curve, const VoiceDef &voice,
                      const Tuning *tuning, double volm) {
    std::string s = "[[volm " + fixed(volm, 3) + "]][[inpt TUNE]]\n";
    std::vector<double> at, hz;
    // D is whole milliseconds; what each line's rounding leaves over is
    // carried to the next, so that the lengths add up to what was meant
    double carry = 0.0;
    for (size_t j = 0; j < lines.size(); ++j) {
        const Line &l = lines[j];
        double want = l.d + carry;
        long d = std::max(0L, std::lround(want));
        carry = want - double(d);
        if (silent(l)) {
            s += "% {D " + std::to_string(d) + "}\n";
            continue;
        }
        s += std::string(l.stress ? "1" : "") + l.ph + " {D " + std::to_string(d) + "; P";
        line_points(l, curve, voice, &at, &hz);
        for (size_t k = 0; k < at.size(); ++k) {
            double h = tuned_hz(tuning, hz[k]);
            h = std::max(lowest_hz(voice.engine), std::min(highest_hz(voice), h));
            s += " " + fixed(h, 2) + ":" + fixed(100.0 * at[k], 1);
        }
        s += "}\n";
    }
    s += "[[inpt TEXT]]";
    // $SVS_MACINTALK_TUNE names a file to keep the last utterance in, to see
    // what MacinTalk was asked
    if (const char *keep = std::getenv("SVS_MACINTALK_TUNE")) {
        if (*keep) {
            if (FILE *f = std::fopen(keep, "wb")) {
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
            }
        }
    }
    return s;
}

// MacinTalk 3: every line a whole number of frames, ending as near as can be
// to where it should.
void mtk3_lengths(std::vector<Line> &lines) {
    const double frame = kMtkFrame / kEngineRate;
    long at = 0;
    for (Line &l : lines) {
        long end = std::lround(l.t1 / frame);
        l.frames = int(std::max(1L, end - at));
        at += l.frames;
    }
    // A stop or nasal before a silence (or at the very end) is given five
    // frames more, which the silence gives back.
    for (size_t j = 0; j + 1 < lines.size(); ++j)
        if (closes(lines[j].ph) && silent(lines[j + 1]) && lines[j + 1].frames > 5)
            lines[j + 1].frames -= 5;
    for (Line &l : lines) l.d = 5.0 * l.frames;
}

// The engines' 16-bit samples as floats.
std::vector<float> to_float(const std::vector<int16_t> &pcm) {
    std::vector<float> out(pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) out[i] = pcm[i] / 32768.0f;
    return out;
}

// -- the last of the pitch, and the vibrato -----------------------------------------

// The engines sing a pitch of their own near the one asked: MacinTalk 3 a
// whole hertz, the others a pitch period of whole samples at 22050 Hz -- up
// to half a step out, a quarter tone by the top of a soprano's range. And a
// vibrato, eleven points to a line, would cut every long note into lines,
// each heard. So the engines are asked for the notes without their vibrato,
// a held vowel one line however long; then each vowel is measured as sung,
// and moved, a period at a time and without its length changing, onto the
// pitch it should have, vibrato and all.

//: how long a held vowel must be steady to be measured, and what is left
//: out at each end of it (the glide in from the sound before, and out)
constexpr double kSteadyMin = 0.12;
constexpr double kSteadyEdge = 0.03;
//: What of a vowel is moved: none of its first 25 ms or its last 15, where
//: the engine is still on its way from the sound before (it follows a new
//: pitch some 10 ms late) or already on its way to the next -- moving that
//: would move where the note is heard to change. The move comes in over
//: kMoveIn and goes over kMoveOut, so that its ends are as sung.
constexpr double kMoveFrom = 0.025;
constexpr double kMoveTo = 0.015;
constexpr double kMoveIn = 0.015;
constexpr double kMoveOut = 0.010;

// How well x from a for n samples repeats `lag` samples on.
double nac(const std::vector<float> &x, long a, long n, double lag) {
    long L = long(lag);
    double fr = lag - double(L), s = 0, e0 = 0, e1 = 0;
    if (a < 0 || a + n + L + 1 >= long(x.size())) return 0.0;
    for (long i = a; i < a + n; ++i) {
        double u = x[size_t(i)];
        double v = (1 - fr) * x[size_t(i + L)] + fr * x[size_t(i + L + 1)];
        s += u * v;
        e0 += u * u;
        e1 += v * v;
    }
    return e0 > 0 && e1 > 0 ? s / std::sqrt(e0 * e1) : 0.0;
}

// The pitch of x from a to b, near `guess` (within a semitone), or 0 when it
// is not periodic there. The period is found where it repeats best, then
// over as many periods as fit, which is what makes it exact.
double measure_hz(const std::vector<float> &x, long a, long b, double guess) {
    long n = b - a;
    double lag0 = kEngineRate / guess;
    double best = -1, at = lag0;
    for (double c = -100; c <= 100; c += 2) {
        double lag = lag0 * std::pow(2.0, c / 1200.0);
        double r = nac(x, a, n / 2, lag);
        if (r > best) {
            best = r;
            at = lag;
        }
    }
    if (best < 0.8) return 0.0;
    int k = std::max(1, int(0.5 * n / at));
    long m = n - long(std::ceil(k * at)) - 2;
    if (m < long(at)) return kEngineRate / at;
    // k periods on, within the 2 cents the first search went by, and closer
    // each round (compared over this window only: the first one was shorter)
    double span = k * at, step = span * 0.0012 / 10;
    best = nac(x, a, m, span);
    for (int round = 0; round < 3; ++round, step /= 10) {
        double centre = span;
        for (double d = -10 * step; d <= 10 * step; d += step) {
            double r = nac(x, a, m, centre + d);
            if (r > best) {
                best = r;
                span = centre + d;
            }
        }
    }
    return kEngineRate * k / span;
}

// x from a to b, sung at the pitch `from` gives (in hertz, at each sample
// from a), moved to the pitch `to` gives: two of its periods at a time,
// windowed, each taken from where it was sung (so the vowel goes on as it
// went) and laid down a new period on from the last. Blended in and out at
// the ends.
template <class From, class To>
void repitch(std::vector<float> &x, long a, long b, From from, To to) {
    long n = b - a;
    if (n < 8) return;
    // where each period begins as sung
    std::vector<double> marks;
    for (double t = 0; t < n + 1; t += kEngineRate / from(std::min(double(n - 1), t))) marks.push_back(t);
    if (marks.size() < 3) return;
    std::vector<double> y(size_t(n), 0.0), w(size_t(n), 0.0);
    auto at = [&](double t) {
        long i = long(std::floor(t));
        double f = t - double(i);
        if (i < 0 || i + 1 >= long(x.size())) return 0.0;
        return (1 - f) * x[size_t(i)] + f * x[size_t(i + 1)];
    };
    size_t closest = 0;
    for (double m = 0; m < n + 1;) {
        while (closest + 1 < marks.size() && std::fabs(marks[closest + 1] - m) <= std::fabs(marks[closest] - m)) ++closest;
        double src = marks[closest];
        double ps = closest + 1 < marks.size() ? marks[closest + 1] - marks[closest] : marks[closest] - marks[closest - 1];
        long lo = long(std::ceil(m - ps)), hi = long(std::floor(m + ps));
        for (long j = std::max(0L, lo); j <= std::min(n - 1, hi); ++j) {
            double u = (j - m) / ps;   // -1 .. 1
            double win = 0.5 + 0.5 * std::cos(M_PI * u);
            y[size_t(j)] += win * at(a + src + (j - m));
            w[size_t(j)] += win;
        }
        m += kEngineRate / to(std::min(double(n - 1), m));
    }
    const long fade = std::min(n / 4, long(0.010 * kEngineRate));
    for (long j = 0; j < n; ++j) {
        double v = w[size_t(j)] > 1e-6 ? y[size_t(j)] / w[size_t(j)] : x[size_t(a + j)];
        double k = 1.0;
        if (j < fade) k = double(j) / fade;
        else if (j >= n - fade) k = double(n - 1 - j) / fade;
        x[size_t(a + j)] = float(k * v + (1 - k) * x[size_t(a + j)]);
    }
}

// Every vowel of a phrase moved from the pitch it was sung at (the curve
// without vibrato, `sung`, and what the engine made of it) onto the pitch it
// should have (`want`, the curve itself). A vowel's pitch is measured where
// it was asked to hold still; where it was asked to move (a bend, a glide
// from note to note), the engine is taken to have followed.
void true_pitch(std::vector<float> &x, const std::vector<Line> &lines, const PitchCurve &sung,
                const PitchCurve &want) {
    // a vowel at a time, of one note: a vowel sung on through the next
    // note is the next vowel
    for (size_t i = 0; i < lines.size();) {
        size_t j = i + 1;
        while (j < lines.size() && lines[j].ph == lines[i].ph && lines[j].note == lines[i].note) ++j;
        double t0 = lines[i].t0, t1 = lines[j - 1].t1;
        bool vowel = apple_vowel(lines[i].ph);
        i = j;
        if (!vowel || t1 - t0 < kMoveFrom + kMoveTo + kMoveIn + kMoveOut) continue;
        // what the engine was asked, and how far from it it sang where that
        // held still
        double ratio = 1.0;
        double s0 = t0 + kSteadyEdge, s1 = t1 - kSteadyEdge;
        if (s1 - s0 >= kSteadyMin - 2 * kSteadyEdge) {
            double lo = 1e9, hi = 0;
            for (double t = s0; t <= s1; t += 0.005) {
                double h = sung.hz_at(t);
                lo = std::min(lo, h);
                hi = std::max(hi, h);
            }
            if (1200.0 * std::log2(hi / lo) <= 2.0) {
                double asked = std::sqrt(lo * hi);
                long a = long(s0 * kEngineRate), b = std::min(long(x.size()), long(s1 * kEngineRate));
                double got = b - a >= long(0.05 * kEngineRate) ? measure_hz(x, a, b, asked) : 0.0;
                double off = got > 0 ? 1200.0 * std::log2(got / asked) : 0.0;
                // past a semitone it is not this voice's step that is out but
                // the reading
                if (std::fabs(off) <= 100.0) ratio = got > 0 ? got / asked : 1.0;
            }
        }
        double m0 = t0 + kMoveFrom, m1 = t1 - kMoveTo;
        long a = long(m0 * kEngineRate), b = std::min(long(x.size()), long(m1 * kEngineRate));
        if (b - a < 8) continue;
        auto when = [&](double k) { return (a + k) / kEngineRate; };
        // Where the pitch moves, what the engine made of it is read every
        // 10 ms (over 30): MacinTalk Pro climbs a bend a step of a period at
        // a time, and the periods are cut from where they really are.
        const long step = long(0.010 * kEngineRate), span = long(0.030 * kEngineRate);
        std::vector<double> track;
        for (long k = 0; k <= b - a; k += step) {
            long c = a + k - span / 2;
            double guess = sung.hz_at(when(double(k))) * ratio;
            double got = c >= 0 && c + span < long(x.size()) ? measure_hz(x, c, c + span, guess) : 0.0;
            track.push_back(got > 0 && std::fabs(1200.0 * std::log2(got / guess)) < 60.0 ? got : guess);
        }
        auto from = [&](double k) {
            double f = k / step;
            size_t i = std::min(track.size() - 1, size_t(std::max(0.0, std::floor(f))));
            if (i + 1 >= track.size()) return track.back();
            return track[i] + (track[i + 1] - track[i]) * (f - double(i));
        };
        auto to = [&](double k) {
            double t = when(k);
            double v = std::min(1.0, std::min((t - m0) / kMoveIn, (m1 - t) / kMoveOut));
            v = std::max(0.0, v);
            double f = from(k);
            return f * std::pow(want.hz_at(t) / f, v);
        };
        // nothing to do where it is already within a cent all through
        double most = 0;
        for (long k = 0; k < b - a; k += long(0.005 * kEngineRate))
            most = std::max(most, std::fabs(1200.0 * std::log2(to(double(k)) / from(double(k)))));
        if (most < 1.0) continue;
        repitch(x, a, b, from, to);
    }
}

// MacinTalk Pro and Vicki let a vowel die away the longer it is held --
// Agnes to a tenth of where she started in a second -- and start it again
// loud at the next line of it. A held vowel is kept at the loudness it has
// once it has begun, until it is let go: its loudness measured over 30 ms
// every 5, and made up, smoothly, to where it was at the start.
void level_held(std::vector<float> &x, const std::vector<Line> &lines) {
    const long win = long(0.030 * kEngineRate), hop = long(0.005 * kEngineRate);
    for (size_t i = 0; i < lines.size();) {
        size_t j = i + 1;
        while (j < lines.size() && lines[j].ph == lines[i].ph) ++j;
        double t0 = lines[i].t0, t1 = lines[j - 1].t1;
        bool vowel = apple_vowel(lines[i].ph);
        i = j;
        if (!vowel || t1 - t0 < 0.25) continue;
        long a = long((t0 + 0.04) * kEngineRate), b = std::min(long(x.size()), long((t1 - 0.03) * kEngineRate));
        if (b - a < 4 * win) continue;
        // the loudness every hop, from a
        std::vector<double> env;
        for (long k = a; k + win <= b; k += hop) {
            double e = 0;
            for (long m = k; m < k + win; ++m) e += double(x[size_t(m)]) * x[size_t(m)];
            env.push_back(std::sqrt(e / win));
        }
        // where it stands once begun: the loudest of its first 80 ms
        size_t settle = std::min(env.size(), size_t(0.08 * kEngineRate / hop));
        double ref = 0;
        for (size_t k = 0; k < settle; ++k) ref = std::max(ref, env[k]);
        if (ref <= 1e-4) continue;
        std::vector<double> gain(env.size(), 1.0);
        for (size_t k = 0; k < env.size(); ++k)
            gain[k] = std::max(0.5, std::min(12.0, ref / std::max(env[k], 1e-6)));
        // smoothed over 20 ms, and only ever up from the start: the attack
        // is the engine's own
        std::vector<double> smooth(gain.size(), 1.0);
        const int half = 2;
        for (size_t k = 0; k < gain.size(); ++k) {
            double sum = 0;
            int n = 0;
            for (int d = -half; d <= half; ++d) {
                long q = long(k) + d;
                if (q < 0 || q >= long(gain.size())) continue;
                sum += gain[size_t(q)];
                ++n;
            }
            smooth[k] = sum / n;
        }
        for (size_t k = 0; k < settle && k < smooth.size(); ++k) smooth[k] = 1.0 + (smooth[k] - 1.0) * k / settle;
        // let go over its last 20 ms, as the engine does
        size_t tail = std::min(smooth.size(), size_t(0.02 * kEngineRate / hop));
        for (size_t k = 0; k < tail; ++k) {
            size_t q = smooth.size() - 1 - k;
            smooth[q] = 1.0 + (smooth[q] - 1.0) * k / tail;
        }
        for (long m = a; m < b; ++m) {
            double f = double(m - a - win / 2) / hop;
            long k = long(std::floor(f));
            double g;
            if (k < 0) g = smooth.front();
            else if (k + 1 >= long(smooth.size())) g = smooth.back();
            else g = smooth[size_t(k)] + (smooth[size_t(k + 1)] - smooth[size_t(k)]) * (f - k);
            x[size_t(m)] = float(x[size_t(m)] * g);
        }
    }
}

// -- finding where MacinTalk Pro put each line -------------------------------------------

// Where the stretches a phrase is divided into for finding its lengths
// begin: near the start of each note, on a line that follows a vowel -- the
// note's own first line if it can be. The phrase's first line begins the
// first.
std::vector<size_t> stretch_starts(const std::vector<Line> &lines) {
    std::vector<size_t> out = {0};
    auto fit = [&](size_t k) {
        return k > 0 && k < lines.size() && !lines[k].separator && apple_vowel(lines[k - 1].ph);
    };
    for (size_t k = 1; k < lines.size(); ++k) {
        if (lines[k].note == lines[k - 1].note) continue;
        size_t best = 0;
        double dist = 1e9;
        for (size_t j = k > 8 ? k - 8 : 1; j < std::min(lines.size(), k + 9); ++j) {
            if (!fit(j) || j <= out.back()) continue;
            double d = std::fabs(lines[j].t0 - lines[k].t0);
            if (d < dist) {
                dist = d;
                best = j;
            }
        }
        if (best && dist <= 0.15) out.push_back(best);
    }
    // and wherever a vowel goes on into another line of itself, which is
    // placed by matching the vibrato or the bend sung against the curve
    for (size_t k = 1; k < lines.size(); ++k)
        if (lines[k].ph == lines[k - 1].ph && apple_vowel(lines[k].ph) &&
            std::find(out.begin(), out.end(), k) == out.end())
            out.insert(std::upper_bound(out.begin(), out.end(), k), k);
    return out;
}

bool voiced(const std::string &p) {
    if (apple_vowel(p)) return true;
    for (const char *v : {"l", "r", "w", "y", "m", "n", "N", "z", "v", "D", "Z"})
        if (p == v) return true;
    return false;
}

// Normalised autocorrelation of x over [a, a + n) at a fractional lag.
double nac(const std::vector<int16_t> &x, long a, long n, double lag) {
    long L = long(lag);
    double fr = lag - double(L), s = 0, e0 = 0, e1 = 0;
    if (a < 0 || a + n + L + 1 >= long(x.size())) return 0.0;
    for (long i = a; i < a + n; ++i) {
        double u = x[size_t(i)];
        double v = (1 - fr) * x[size_t(i + L)] + fr * x[size_t(i + L + 1)];
        s += u * v;
        e0 += u * u;
        e1 += v * v;
    }
    return e0 > 0 && e1 > 0 ? s / std::sqrt(e0 * e1) : 0.0;
}

// How well x around `t` (seconds) repeats at the period of `hz`, allowing
// the pitch to be a little either side of it (vibrato, the dither).
double periodic_at(const std::vector<int16_t> &x, double t, double hz) {
    const long n = std::max(200L, long(2.5 * kEngineRate / hz));
    long a = long(t * kEngineRate) - n / 2;
    double best = 0, p = kEngineRate / hz;
    for (double d = -0.03; d <= 0.0301; d += 0.01) best = std::max(best, nac(x, a, n, p * (1 + d)));
    return best;
}

// Where MacinTalk Pro began each stretch, found in what it sang: its pitch
// changes the moment a line begins, so where the note before and the note
// after are sung at different pitches, the change is where the stretch
// begins; and where a stretch begins with no voice (an s, a stop's closure,
// a rest), it begins where the vowel before it stops. The time of each, in
// seconds, or NaN where neither can be heard. `expect` says roughly where
// each should be.
std::vector<double> find_starts(const std::vector<int16_t> &pcm, const std::vector<Line> &lines,
                                const std::vector<size_t> &stretch, const PitchCurve &curve,
                                const std::vector<double> &expect) {
    const double nan = std::nan("");
    std::vector<double> starts(stretch.size(), nan);
    starts[0] = 0.0;
    const double end = double(pcm.size()) / kEngineRate;
    const double step = 0.001;
    double last_seen = 0.0, last_want = 0.0;
    // the whole phrase came out this much longer or shorter than the score
    // (the closing silence apart, which is always its own length)
    double rest = silent(lines.back()) ? lines.back().t1 - lines.back().t0 : 0.0;
    double scale = (end - rest) / std::max(0.1, lines.back().t1 - rest);
    for (size_t i = 1; i < stretch.size(); ++i) {
        size_t k = stretch[i];
        const Line &here = lines[k];
        double before_hz = PitchCurve::midi_to_hz(curve.steady_midi_at(std::max(0.0, lines[k - 1].t1 - 0.02)));
        if (here.ph == lines[k - 1].ph && apple_vowel(here.ph)) {
            // A vowel going on: where the pitch sung around here lines up
            // best with the curve. Only where the curve moves enough to be
            // matched (a vibrato, a bend); where it is flat, where this line
            // begins makes no difference to what is heard.
            double off0 = (std::isnan(expect[i]) ? last_seen + (here.t0 - last_want) * scale : expect[i]) - here.t0;
            const double span = 0.15, hop = 0.005;
            std::vector<double> tt, sung;
            double lo = 1e9, hi = -1e9;
            for (double t = here.t0 - span; t <= here.t0 + span; t += hop) {
                double m = curve.midi_at(t);
                lo = std::min(lo, m);
                hi = std::max(hi, m);
            }
            if (hi - lo < 0.25) continue;
            for (double t = here.t0 + off0 - span - 0.1; t <= here.t0 + off0 + span + 0.1; t += hop) {
                if (t < 0.02 || t > end - 0.04) continue;
                double want = PitchCurve::midi_to_hz(curve.midi_at(t - off0));
                double p = kEngineRate / want, best = 0, lag = 0;
                long n = std::max(200L, long(2.5 * p));
                long a = long(t * kEngineRate) - n / 2;
                for (double L = p / 1.08; L <= p * 1.08; L += 0.25) {
                    double v = nac(pcm, a, n, L);
                    if (v > best) {
                        best = v;
                        lag = L;
                    }
                }
                if (best < 0.8) continue;
                tt.push_back(t);
                sung.push_back(69.0 + 12.0 * std::log2(kEngineRate / lag / 440.0));
            }
            if (tt.size() < 30) continue;
            double best_err = 1e9, best_off = 0;
            for (double off = off0 - 0.1; off <= off0 + 0.1; off += 0.002) {
                double e = 0;
                int used = 0;
                for (size_t q = 0; q < tt.size(); ++q) {
                    double want = curve.midi_at(tt[q] - off);
                    if (std::fabs(tt[q] - off - here.t0) > span) continue;
                    e += std::fabs(sung[q] - want);
                    ++used;
                }
                if (used < 30) continue;
                e /= used;
                if (e < best_err) {
                    best_err = e;
                    best_off = off;
                }
            }
            if (best_err > 0.2) continue;      // a fifth of a semitone on average
            starts[i] = here.t0 + best_off;
            last_seen = starts[i];
            last_want = here.t0;
            continue;
        }
        // where to look: from the last stretch seen, as far again as the
        // score has it, give or take
        double guess = std::isnan(expect[i]) ? last_seen + (here.t0 - last_want) * scale : expect[i];
        double from = std::max(last_seen + 0.02, guess - 0.3), to = std::min(end - 0.03, guess + 0.3);
        if (to <= from) continue;
        if (voiced(here.ph)) {
            // the pitch of the first voiced moment of this stretch
            double after_hz = PitchCurve::midi_to_hz(curve.steady_midi_at(std::min(here.t1, here.t0 + 0.02)));
            double apart = std::fabs(1200.0 * std::log2(after_hz / before_hz));
            if (apart < 70.0) continue;
            // the moment that best divides "the pitch before" from "the pitch
            // after": as many looks before it hear the first as can, as many
            // after it the second
            std::vector<int> heard;
            for (double t = from; t < to; t += step) {
                double a = periodic_at(pcm, t, before_hz), b = periodic_at(pcm, t, after_hz);
                heard.push_back(std::max(a, b) < 0.6 ? 0 : (a > b ? -1 : 1));
            }
            int n = int(heard.size()), score = 0, best = -(1 << 30), at = -1;
            for (int j = 0; j < n; ++j) score += heard[size_t(j)] == 1 ? 1 : 0;   // all "after"
            // score(j) = before-matches in [0, j) + after-matches in [j, n)
            int before = 0, after = score;
            for (int j = 0; j <= n; ++j) {
                int s = before + after;
                if (s > best) {
                    best = s;
                    at = j;
                }
                if (j < n) {
                    if (heard[size_t(j)] == -1) ++before;
                    if (heard[size_t(j)] == 1) --after;
                }
            }
            // both pitches must really be there
            int saw_before = 0, saw_after = 0;
            for (int j = 0; j < n; ++j) {
                if (j < at && heard[size_t(j)] == -1) ++saw_before;
                if (j >= at && heard[size_t(j)] == 1) ++saw_after;
            }
            if (saw_before < 8 || saw_after < 8) continue;
            starts[i] = from + at * step;
        } else {
            // the last moment the vowel before is heard, after which it is
            // not heard for 15 ms
            std::vector<bool> heard;
            for (double t = from; t < to + 0.02; t += step) heard.push_back(periodic_at(pcm, t, before_hz) >= 0.6);
            int n = int(heard.size()), at = -1;
            double closest = 1e9;
            for (int j = 8; j + 16 < n; ++j) {
                if (!heard[size_t(j)]) continue;
                int on = 0, off = 0;
                for (int q = j - 8; q < j; ++q) on += heard[size_t(q)];
                for (int q = j + 1; q <= j + 15; ++q) off += !heard[size_t(q)];
                if (on >= 6 && off >= 14) {
                    double d = std::fabs(from + j * step - guess);
                    if (d < closest) {
                        closest = d;
                        at = j;
                    }
                }
            }
            if (at < 0) continue;
            // a look is centred on its moment and heard while most of it is
            // voiced, so the voice stops a little after the last
            starts[i] = from + at * step + 0.004;
        }
        last_seen = starts[i];
        last_want = here.t0;
    }
    return starts;
}

// -- the singer ---------------------------------------------------------------------

class MacinTalkSinger : public Singer {
public:
    MacinTalkSinger(std::shared_ptr<Host> host, int gen, int voice)
        : host_(std::move(host)), gen_(gen), voice_(kVoices[voice]),
          tuning_(find_tuning(gen, kVoices[voice].bundle)) {}

    Sung sing(const Phrase &asked) override {
        Phrase phrase = asked;
        fit_voice(voice_, &phrase);
        Sung sung;
        if (phrase.notes.empty()) return sung;
        // Where the vowels can be measured and moved afterwards, the engine
        // is asked for the phrase without its vibrato (or the mod wheel's):
        // the vibrato is put in afterwards, exactly as deep as it should be.
        // Not Trinoids, three voices whose pitch cannot be read as one, nor
        // Boing, who sings three eighths of what he is asked.
        bool measurable = std::strcmp(voice_.bundle, "Trinoids") != 0 && std::strcmp(voice_.bundle, "Boing") != 0;
        bool move = voice_.gift == kSings && measurable && !g_raw;
        Phrase flat = phrase;
        if (move) {
            flat.style.vibrato = 0;
            flat.mods.clear();
        }
        PitchCurve want(phrase), curve(flat);
        long total = long(std::llround(curve.total() * kSampleRate));
        if (total <= 0) return sung;
        std::vector<Line> lines = plan_lines(phrase, curve, voice_.engine);
        double volm = tuning_ && !g_raw ? tuning_->volm : 0.25;
        double gain = tuning_ && !g_raw ? tuning_->gain : 1.0;

        std::vector<int16_t> pcm;
        std::string err;
        if (voice_.engine == kMtk3) {
            mtk3_lengths(lines);
            if (!host_->say(voice_.bundle, tune_text(lines, curve, voice_, tuning_, volm), &pcm, &err)) {
                sung.error = err.empty() ? "the MacinTalk host stopped answering" : err;
                return silence(total, &sung);
            }
        } else if (!sing_by_ear(lines, curve, volm, &pcm, &err)) {
            sung.error = err.empty() ? "the MacinTalk host stopped answering" : err;
            return silence(total, &sung);
        }

        std::vector<float> sung_at = to_float(pcm);
        if (move) true_pitch(sung_at, lines, curve, want);
        if (voice_.engine != kMtk3 && !g_raw) level_held(sung_at, lines);
        std::vector<float> out = resample_to_output(sung_at, kEngineRate);
        out.resize(size_t(total), 0.0f);
        apply_level(phrase, curve, gain, &out);
        sung.samples = std::move(out);
        return sung;
    }

private:
    static Sung silence(long total, Sung *s) {
        s->samples.assign(size_t(total), 0.0f);
        return *s;
    }

    // MacinTalk Pro and Vicki: sing, see where the lines fell, correct the
    // lengths and sing again (see the top of the file).
    bool sing_by_ear(std::vector<Line> &lines, const PitchCurve &curve, double volm,
                     std::vector<int16_t> *pcm, std::string *err) {
        // The first guess. Tiger keeps to D. Mac OS X 10.5 and 10.6 make a
        // line 0.82 of its D and add a length of their own: a consonant next
        // to nothing, a vowel some 25 ms, and a vowel that goes on from a
        // line of itself more -- by the voice, 57 ms for Victoria, 31 for
        // Agnes, 22 for Bruce, 40 for Vicki (measured, held at A3 between
        // consonants).
        const double slope = gen_ == 0 ? 1.0 : 0.82;
        double again = 0.040;
        if (gen_ != 0) {
            std::string b = voice_.bundle;
            again = b == "Victoria" ? 0.057 : b == "Agnes" ? 0.031 : b == "Bruce" ? 0.022 : 0.040;
        }
        for (size_t j = 0; j < lines.size(); ++j) {
            Line &l = lines[j];
            double len = l.t1 - l.t0, own = 0.0;
            if (gen_ != 0 && apple_vowel(l.ph))
                own = j > 0 && lines[j - 1].ph == l.ph ? again : 0.025;
            if (l.separator) l.d = 0;
            else if (silent(l)) l.d = len * 1000.0;
            else l.d = std::max(10.0, (len - own) / slope * 1000.0);
        }
        // The stretches stay put from pass to pass: they depend on the
        // phonemes, not their lengths.
        std::vector<size_t> anchors = stretch_starts(lines);
        std::vector<int16_t> plain, best;
        // where each stretch is expected: nowhere in particular the first
        // time, where it was asked for once the lengths have been corrected
        std::vector<double> expect(anchors.size(), std::nan(""));
        double best_worst = 1e9;
        for (int pass = 0; pass < kMaxPasses; ++pass) {
            if (!host_->say(voice_.bundle, tune_text(lines, curve, voice_, tuning_, volm), &plain, err))
                return false;
            std::vector<double> at = find_starts(plain, lines, anchors, curve, expect);
            // how far each start that can be seen is from its time
            double worst = 0.0;
            for (size_t i = 0; i < anchors.size(); ++i)
                if (!std::isnan(at[i])) worst = std::max(worst, std::fabs(at[i] - lines[anchors[i]].t0));
            if (worst < best_worst) {
                best_worst = worst;
                best = plain;
            }
            if (worst <= kGoodEnough || pass + 1 == kMaxPasses) break;
            for (size_t i = 0; i < anchors.size(); ++i) expect[i] = lines[anchors[i]].t0;
            // Between each two starts that were seen, the lines are made
            // longer or shorter together, in proportion to their lengths, by
            // what that stretch is out -- counting what the stretches before
            // it could not make up (`shift`: how late the stretch's first
            // line will be once the ones before are corrected).
            std::vector<size_t> seen;
            for (size_t i = 0; i < anchors.size(); ++i)
                if (!std::isnan(at[i])) seen.push_back(i);
            double shift = 0.0;
            for (size_t i = 0; i + 1 < seen.size(); ++i) {
                size_t a = anchors[seen[i]], b = anchors[seen[i + 1]];
                double have = at[seen[i + 1]] - at[seen[i]];
                double want = lines[b].t0 - (lines[a].t0 + shift);
                // A small difference is all given to the longest line: a
                // millisecond or two on each of many would be lost in the
                // engine's rounding to whole pitch periods.
                double weight = 0.0, longest = 0.0;
                size_t only = b;
                for (size_t j = a; j < b; ++j) {
                    if (lines[j].separator) continue;
                    double len = lines[j].t1 - lines[j].t0;
                    weight += len;
                    if (len > longest) {
                        longest = len;
                        only = j;
                    }
                }
                bool small = std::fabs(want - have) < 0.040;
                double moved = 0.0;
                if (weight > 0.0)
                    for (size_t j = a; j < b; ++j) {
                        Line &l = lines[j];
                        if (l.separator || (small && j != only)) continue;
                        double share = small ? 1.0 : (l.t1 - l.t0) / weight;
                        double s = silent(l) ? 1.0 : slope;
                        double nd = l.d + (want - have) * share / s * 1000.0;
                        nd = std::max(silent(l) ? 0.0 : 10.0, nd);
                        moved += (nd - l.d) * s / 1000.0;
                        l.d = nd;
                    }
                shift = lines[a].t0 + shift + have + moved - lines[b].t0;
            }
        }
        *pcm = best;
        return true;
    }

    // The note's velocity, smoothed over a few milliseconds (a rest keeps the
    // level before it), the part's level and the voice's own gain; and a
    // phrase that would still clip is brought down as a whole.
    void apply_level(const Phrase &phrase, const PitchCurve &curve, double gain, std::vector<float> *out) {
        const double sr = kSampleRate;
        const double tc = 1.0 - std::exp(-1.0 / (0.008 * sr));
        double g = -1.0, cur = -1.0, peak = 0.0;
        for (size_t s = 0; s < out->size(); ++s) {
            const SungNote &n = phrase.notes[curve.note_at(s / sr)];
            bool rest = n.phonemes.empty() || (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
            if (!rest || g < 0) g = std::max(1, std::min(127, n.velocity)) / 127.0;
            cur = cur < 0 ? g : cur + (g - cur) * tc;
            float v = float((*out)[s] * cur * gain * phrase.level);
            (*out)[s] = v;
            peak = std::max(peak, double(std::fabs(v)));
        }
        if (peak > 0.99) {
            float k = float(0.99 / peak);
            for (float &v : *out) v *= k;
        }
    }

    std::shared_ptr<Host> host_;
    int gen_;
    const VoiceDef &voice_;
    const Tuning *tuning_;
};
#endif

#ifdef __APPLE__   // on a Mac the system's own voices sing, retimed and retuned here

// -- Apple's phonemes in the International Phonetic Alphabet -----------------------------

struct Ipa {
    const char *apple;
    const char *ipa;
};
const Ipa kIpa[] = {
    {"AE", "æ"}, {"EY", "eɪ"}, {"AO", "ɔ"}, {"AX", "ə"}, {"IY", "i"},  {"EH", "ɛ"},  {"IH", "ɪ"},  {"AY", "aɪ"},
    {"IX", "ɪ"}, {"AA", "ɑ"},  {"UW", "u"}, {"UH", "ʊ"}, {"UX", "ʌ"},  {"OW", "oʊ"}, {"AW", "aʊ"}, {"OY", "ɔɪ"},
    {"b", "b"},  {"C", "tʃ"},  {"d", "d"},  {"D", "ð"},  {"f", "f"},   {"g", "g"},   {"h", "h"},   {"J", "dʒ"},
    {"k", "k"},  {"l", "l"},   {"m", "m"},  {"n", "n"},  {"N", "ŋ"},   {"p", "p"},   {"r", "ɹ"},   {"s", "s"},
    {"S", "ʃ"},  {"t", "t"},   {"T", "θ"},  {"v", "v"},  {"w", "w"},   {"y", "j"},   {"z", "z"},   {"Z", "ʒ"},
};

std::string ipa_of(const char *apple) {
    if (!apple) return "";
    for (const Ipa &i : kIpa)
        if (std::strcmp(i.apple, apple) == 0) return i.ipa;
    return "";
}

//: One note's phonemes as a stressed syllable in IPA, or empty for a rest.
//: The r-coloured vowels of "bird" and "butter" have symbols of their own;
//: a diphthong is Apple's whole (its glide is retimed with the rest).
std::string syllable_ipa(const SungNote &n) {
    std::string s;
    for (const std::string &sym : n.phonemes) {
        if (sym == kRest) continue;
        const Mapping *m = find_mapping(sym);
        if (!m) continue;
        std::string a = palette_alias(sym);
        if (a == "ER") {
            s += "ɝ";
            continue;
        }
        if (a == "RX") {
            s += "ɚ";
            continue;
        }
        if (m->shape == kTail && m->second && apple_vowel(m->second)) {
            s += ipa_of(m->second);
        } else {
            s += ipa_of(m->first);
            if (m->second) s += ipa_of(m->second);
        }
    }
    return s.empty() ? s : "ˈ" + s;
}

//: The planned lengths of the consonants before the first vowel and after
//: the last, in seconds, from the note's own phoneme lengths.
void consonant_lengths(const SungNote &n, double *head, double *tail) {
    *head = *tail = 0.0;
    int first = -1, last = -1;
    for (size_t i = 0; i < n.phonemes.size(); ++i)
        if (is_vowel(n.phonemes[i])) {
            if (first < 0) first = int(i);
            last = int(i);
        }
    if (first < 0) return;
    for (int i = 0; i < first && i < int(n.durations_ms.size()); ++i) *head += n.durations_ms[size_t(i)] / 1000.0;
    for (int i = last + 1; i < int(n.durations_ms.size()); ++i) *tail += n.durations_ms[size_t(i)] / 1000.0;
}

// -- how [[pbas]] maps to hertz, voice by voice --------------------------------------------
//
// Apple documents pbas as a baseline pitch, but what a number gives differs
// from voice to voice and is not quite a semitone a step. So the first time
// a voice sings, it is asked for one vowel at several baselines and the
// pitch of each is measured; a note is then asked for at the nearest
// baseline and the rest of the way is sung by the resynthesis, which keeps
// the shift small and the voice its own.
constexpr int kPbasMin = 20, kPbasMax = 95;
constexpr double kSyllableRms = 0.22;   //: every voice's vowels brought to this

class MacCalibration {
public:
    int pbas_for(const std::string &bundle, double hz) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = tables_.find(bundle);
        if (it == tables_.end()) it = tables_.emplace(bundle, measure(bundle)).first;
        const std::vector<Point> &table = it->second;
        if (table.size() < 2) return clamp(int(std::lround(69 + 12 * std::log2(std::max(hz, 20.0) / 440.0))));
        double want = std::log2(std::max(hz, 20.0));
        size_t k = 1;
        while (k + 1 < table.size() && want > table[k].second) ++k;
        const Point &a = table[k - 1], &b = table[k];
        double f = (want - a.second) / (b.second - a.second);
        return clamp(int(std::lround(a.first + f * (b.first - a.first))));
    }

private:
    using Point = std::pair<int, double>;   //: pbas, log2 of the hertz it gave
    static int clamp(int p) { return std::max(kPbasMin, std::min(kPbasMax, p)); }

    static std::vector<Point> measure(const std::string &bundle) {
        const int steps[] = {30, 40, 50, 60, 70, 80};
        std::vector<macspeech::Piece> pieces{{"[[pmod 0]]", ""}};
        for (int p : steps) {
            pieces.push_back({"[[pbas " + std::to_string(p) + "]]", ""});
            pieces.push_back({"la", "ˈlɑ"});
            pieces.push_back({"[[slnc 500]]", ""});
        }
        std::vector<float> x;
        double rate = 0.0;
        std::string err;
        if (!macspeech::render(bundle, pieces, &x, &rate, &err)) return {};
        std::vector<dsp::Segment> segs = dsp::split_on_silence(x, rate, 0.150);
        if (segs.size() != sizeof steps / sizeof steps[0]) return {};
        std::vector<Point> out;
        for (size_t i = 0; i < segs.size(); ++i) {
            std::vector<float> seg(x.begin() + long(segs[i].begin), x.begin() + long(segs[i].end));
            dsp::F0Track f = dsp::track_f0(seg, rate);
            std::vector<double> voiced;
            for (double v : f.f0)
                if (v > 0) voiced.push_back(v);
            if (voiced.size() < 4) continue;
            std::nth_element(voiced.begin(), voiced.begin() + long(voiced.size() / 2), voiced.end());
            double median = voiced[voiced.size() / 2];
            // only what rises with the number counts: a voice whose pitch is
            // its own (Albert, Bahh) gives nothing usable here
            if (out.empty() || std::log2(median) > out.back().second + 0.05) out.push_back({steps[i], std::log2(median)});
        }
        return out.size() >= 2 ? out : std::vector<Point>();
    }

    std::mutex mu_;
    std::map<std::string, std::vector<Point>> tables_;
};

// -- the singer, on a Mac -------------------------------------------------------------------

class MacSinger : public Singer {
public:
    MacSinger(const VoiceDef &voice, std::shared_ptr<MacCalibration> cal) : voice_(voice), cal_(std::move(cal)) {}

    Sung sing(const Phrase &asked) override {
        Phrase phrase = asked;
        fit_voice(voice_, &phrase);
        Sung sung;
        if (phrase.notes.empty()) return sung;
        PitchCurve curve(phrase);
        long total = long(std::llround(curve.total() * kSampleRate));
        if (total <= 0) return sung;

        // the syllables: every note with a sound in it
        std::vector<size_t> which;
        std::vector<std::string> ipas;
        for (size_t i = 0; i < phrase.notes.size(); ++i) {
            std::string ipa = syllable_ipa(phrase.notes[i]);
            if (ipa.empty()) continue;
            which.push_back(i);
            ipas.push_back(ipa);
        }
        if (which.empty()) return silence(total, &sung);

        // one utterance for the phrase, a silence between syllables to cut on
        std::vector<macspeech::Piece> pieces{{"[[pmod 0]]", ""}};
        std::vector<int> pbas;
        for (size_t k = 0; k < which.size(); ++k) {
            size_t i = which[k];
            double mid = 0.5 * (curve.note_start(i) + curve.note_end(i));
            double hz = PitchCurve::midi_to_hz(curve.steady_midi_at(mid));
            pbas.push_back(cal_->pbas_for(voice_.bundle, hz));
            pieces.push_back({"[[pbas " + std::to_string(pbas.back()) + "]]", ""});
            pieces.push_back({"la", ipas[k]});
            pieces.push_back({"[[slnc 500]]", ""});
        }
        std::vector<float> x;
        double rate = 0.0;
        std::string err;
        if (!macspeech::render(voice_.bundle, pieces, &x, &rate, &err)) {
            sung.error = err;
            return silence(total, &sung);
        }
        std::vector<dsp::Segment> segs = dsp::split_on_silence(x, rate, 0.150);
        std::vector<std::vector<float>> syllables;
        if (segs.size() == which.size()) {
            for (const dsp::Segment &g : segs) syllables.emplace_back(x.begin() + long(g.begin), x.begin() + long(g.end));
        } else {
            // the cut did not come out one to a syllable: ask for each on its own
            for (size_t k = 0; k < which.size(); ++k) {
                std::vector<macspeech::Piece> one{{"[[pmod 0]]", ""},
                                                 {"[[pbas " + std::to_string(pbas[k]) + "]]", ""},
                                                 {"la", ipas[k]}};
                std::vector<float> y;
                double r = 0.0;
                syllables.emplace_back();
                if (!macspeech::render(voice_.bundle, one, &y, &r, &err) || r != rate) continue;
                std::vector<dsp::Segment> g = dsp::split_on_silence(y, rate, 0.150);
                if (g.empty()) continue;
                syllables.back().assign(y.begin() + long(g.front().begin), y.begin() + long(g.back().end));
            }
        }

        // each syllable to its note's time and pitch, at the engine's rate
        const double sr = rate;
        std::vector<float> out(size_t(std::llround(curve.total() * sr)) + 1, 0.0f);
        for (size_t k = 0; k < which.size(); ++k) {
            std::vector<float> &syl = syllables[k];
            if (syl.empty()) continue;
            size_t i = which[k];
            double a = curve.note_start(i), b = curve.note_end(i);
            long want = std::llround((b - a) * sr);
            if (want <= 0) continue;
            dsp::F0Track f0 = dsp::track_f0(syl, sr);
            double level = dsp::rms(syl, sr, &f0);
            if (level > 1e-5) {
                float g = float(kSyllableRms / level);
                for (float &v : syl) v *= g;
            }
            double head = 0.0, tail = 0.0;
            consonant_lengths(phrase.notes[i], &head, &tail);
            auto map = dsp::syllable_time_map(syl.size(), sr, f0, size_t(want), head, tail);
            auto hz = [&curve, a](double t) { return curve.hz_at(a + t); };
            std::vector<float> y = dsp::psola(syl, sr, f0, size_t(want), map, hz);
            // a few milliseconds in and out, so the joins do not click
            long edge = std::min<long>(std::llround(0.003 * sr), want / 2);
            for (long j = 0; j < edge; ++j) {
                float w = float(j) / float(edge);
                y[size_t(j)] *= w;
                y[size_t(want - 1 - j)] *= w;
            }
            size_t at = size_t(std::llround(a * sr));
            for (long j = 0; j < want && at + size_t(j) < out.size(); ++j) out[at + size_t(j)] += y[size_t(j)];
        }
        std::vector<float> res = resample_to_output(out, sr);
        res.resize(size_t(total), 0.0f);
        apply_levels(phrase, curve, &res);
        sung.samples = std::move(res);
        return sung;
    }

private:
    static Sung silence(long total, Sung *s) {
        s->samples.assign(size_t(total), 0.0f);
        return *s;
    }

    // The note's velocity, smoothed over a few milliseconds (a rest keeps the
    // level before it), and the part's level; a phrase that would still clip
    // is brought down as a whole.
    static void apply_levels(const Phrase &phrase, const PitchCurve &curve, std::vector<float> *out) {
        const double sr = kSampleRate;
        const double tc = 1.0 - std::exp(-1.0 / (0.008 * sr));
        double g = -1.0, cur = -1.0, peak = 0.0;
        for (size_t s = 0; s < out->size(); ++s) {
            const SungNote &n = phrase.notes[curve.note_at(s / sr)];
            bool rest = n.phonemes.empty() || (n.phonemes.size() == 1 && n.phonemes[0] == kRest);
            if (!rest || g < 0) g = std::max(1, std::min(127, n.velocity)) / 127.0;
            cur = cur < 0 ? g : cur + (g - cur) * tc;
            float v = float((*out)[s] * cur * phrase.level);
            (*out)[s] = v;
            peak = std::max(peak, double(std::fabs(v)));
        }
        if (peak > 0.99) {
            float k = float(0.99 / peak);
            for (float &v : *out) v *= k;
        }
    }

    const VoiceDef &voice_;
    std::shared_ptr<MacCalibration> cal_;
};

//: The one slot the list shows on a Mac: Snow Leopard's numbers, which sing
//: on Windows too, so a song made here sings there.
constexpr int kMacSlot = 2;
#endif

// -- the family -----------------------------------------------------------------------

class MacinTalkFamily : public Family {
public:
    std::string name() const override { return "MacinTalk"; }

    std::vector<VoiceEntry> voices() override {
        std::vector<VoiceEntry> out;
#ifdef __APPLE__
        for (int v = 0; v < kVoiceCount; ++v) {
            VoiceEntry e;
            e.id = kMacinTalkBase + kGenStride * kMacSlot + v;
            e.name = std::string("MacinTalk ") + kVoices[v].display;
            e.installed = can_sing(install(), kMacSlot, v, nullptr);
            out.push_back(e);
        }
        return out;
#endif
        const Install &in = install();
        for (int g = 0; g < kGenCount; ++g)
            for (int v = 0; v < kVoiceCount; ++v) {
                if (!in_generation(g, v)) continue;
                VoiceEntry e;
                e.id = kMacinTalkBase + kGenStride * g + v;
                e.group = group_name(g);
                e.name = e.group + " " + kVoices[v].display;
                e.installed = can_sing(in, g, v, nullptr);
                out.push_back(e);
            }
        return out;
    }

    bool available(std::string *why) override {
        const Install &in = install();
#ifdef _WIN32
        if (in.host.empty()) {
            if (why)
                *why = "MacinTalk needs Panthera's host, panthera_host.exe, which comes with Panthera's "
                       "NVDA add-on (github.com/tgeczy/panthera-speech); it is not in NVDA's add-ons or in " +
                       data_name("voices\\macintalk") + ".";
            return false;
        }
        for (int g = 0; g < kGenCount; ++g)
            if (in.gens[g].usable) return true;
        if (why) {
            std::string s = "MacinTalk needs Apple's speech files from a Mac OS X 10.4, 10.5 or 10.6 "
                            "install disc, which Panthera's NVDA add-on (github.com/tgeczy/panthera-speech) "
                            "extracts into NVDA's macintalk folder";
            std::string found;
            for (int g = 0; g < kGenCount; ++g)
                if (in.gens[g].tree_found) found += (found.empty() ? "" : "; ") + in.gens[g].problem;
            *why = found.empty() ? s + "; none was found." : s + ". " + capital(found) + ".";
        }
        return false;
#elif defined(__APPLE__)
        (void)in;
        for (const VoiceDef &v : kVoices)
            if (macspeech::voice_present(v.bundle)) return true;
        if (why)
            *why = "MacinTalk's voices are not installed on this Mac: System Settings, Accessibility, Spoken "
                   "Content, System Voice, Manage Voices adds them, under English (Fred, Agnes, Victoria and "
                   "the rest).";
        return false;
#else
        (void)in;
        if (why)
            *why = "MacinTalk runs through Panthera, which is Windows-only; on this system the voices "
                   "cannot sing.";
        return false;
#endif
    }

    std::unique_ptr<Singer> singer(int voice_id, std::string *err) override {
        int rel = voice_id - kMacinTalkBase;
        int g = rel / kGenStride, v = rel % kGenStride;
        if (rel < 0 || g >= kGenCount || v >= kVoiceCount || !in_generation(g, v)) {
            if (err) *err = "There is no MacinTalk voice numbered " + std::to_string(voice_id) + ".";
            return nullptr;
        }
        const Install &in = install();
        std::string why;
        if (!can_sing(in, g, v, &why)) {
            if (err) *err = why;
            return nullptr;
        }
#ifdef _WIN32
        std::lock_guard<std::mutex> lock(mu_);
        if (!hosts_[g]) {
            const GenState &st = in.gens[g];
            hosts_[g] = std::make_shared<Host>(in.host, st.mt, st.sd, st.vd);
        }
        return std::make_unique<MacinTalkSinger>(hosts_[g], g, v);
#elif defined(__APPLE__)
        // every generation's number sings with the Mac's one voice of that name
        std::lock_guard<std::mutex> lock(mu_);
        if (!calibration_) calibration_ = std::make_shared<MacCalibration>();
        return std::make_unique<MacSinger>(kVoices[v], calibration_);
#else
        return nullptr;
#endif
    }

private:
    static std::string capital(std::string s) {
        if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = char(s[0] - 'a' + 'A');
        return s;
    }

    // Whether voice `v` of generation `g` can sing here, and if not why.
    static bool can_sing(const Install &in, int g, int v, std::string *why) {
        const VoiceDef &vd = kVoices[v];
#ifdef __APPLE__
        std::string name = std::string("MacinTalk ") + vd.display;
#else
        std::string name = group_name(g) + " " + vd.display;
#endif
#if defined(__APPLE__)
        (void)in;
        (void)g;
        if (!macspeech::voice_present(vd.bundle)) {
            if (why)
                *why = name + " is not installed on this Mac: System Settings, Accessibility, Spoken Content, "
                       "System Voice, Manage Voices adds it, under English.";
            return false;
        }
        if (vd.gift == kOwnTune) {
            if (why)
                *why = name + " plays its own tune to every syllable, at its own speed, whatever it is "
                       "given, so it cannot sing a part.";
            return false;
        }
        return true;
#elif !defined(_WIN32)
        (void)in;
        if (why)
            *why = "MacinTalk runs through Panthera, which is Windows-only; on this system the voices "
                   "cannot sing.";
        return false;
#else
        const GenState &st = in.gens[g];
        if (in.host.empty()) {
            if (why) *why = "Panthera's host, panthera_host.exe, is not installed (it comes with Panthera's NVDA add-on).";
            return false;
        }
        if (!st.tree_found) {
            if (why)
                *why = std::string("Mac OS X ") + kGens[g].release + " (" + kGens[g].title +
                       ") has not been extracted: Panthera's NVDA add-on extracts it into NVDA's macintalk\\" +
                       kGens[g].key + " folder.";
            return false;
        }
        if (!st.usable) {
            if (why) *why = capital(st.problem) + ".";
            return false;
        }
        if (!st.voice_present[v]) {
            if (why) *why = name + " is not among the voices in " + st.vd + ".";
            return false;
        }
        if (vd.gift == kOwnTune) {
            if (why)
                *why = name + " plays its own tune to every syllable, at its own speed, whatever it is "
                       "given, so it cannot sing a part.";
            return false;
        }
        if (vd.engine == kMeow && !in.aac) {
            if (why)
                *why = name + "'s samples are AAC, and this copy of Windows has no AAC decoder (an N "
                       "edition without the Media Feature Pack).";
            return false;
        }
        return true;
#endif
    }

    const Install &install() {
        std::lock_guard<std::mutex> lock(mu_);
        unsigned version = 0;
        named_folder(&version);
        if (!surveyed_ || version != surveyed_version_) {
#ifdef _WIN32
            install_ = survey();
            // a singer already made keeps the host it has; new ones get new
            // hosts, from wherever MacinTalk is now
            for (auto &h : hosts_) h.reset();
#endif
            surveyed_ = true;
            surveyed_version_ = version;
        }
        return install_;
    }

    std::mutex mu_;
    bool surveyed_ = false;
    unsigned surveyed_version_ = 0;
    Install install_;
#ifdef _WIN32
    std::shared_ptr<Host> hosts_[kGenCount];
#endif
#ifdef __APPLE__
    std::shared_ptr<MacCalibration> calibration_;
#endif
};

}  // namespace

std::unique_ptr<Family> make_macintalk_family() { return std::make_unique<MacinTalkFamily>(); }

void set_macintalk_folder(const std::string &folder) {
    std::lock_guard<std::mutex> lock(g_folder_mu);
    if (folder == g_folder) return;
    g_folder = folder;
    ++g_folder_version;
}

//: Not part of family.h: sing with no tuning at all -- no offsets, nothing
//: measured and moved, one [[volm]] for every voice -- for the test
//: program's --calibrate.
void macintalk_raw(bool raw) { g_raw = raw; }

//: Not part of family.h: the self-check the test program runs without any
//: data. Every VocalWriter phoneme has a row, and every row's phonemes are
//: Apple's. Returns an empty string, or what is wrong.
std::string macintalk_self_check() {
    std::string bad;
    for (const PhonemeInfo &p : phonemes()) {
        const Mapping *m = find_mapping(p.symbol);
        if (!m) {
            bad += std::string("no Apple phoneme for ") + p.symbol + "; ";
            continue;
        }
        for (const char *a : {m->first, m->second}) {
            if (!a) continue;
            std::string s = a;
            if (s != "%" && !apple_vowel(s) && !apple_consonant(s))
                bad += std::string(p.symbol) + " maps to " + s + ", which is not Apple's; ";
        }
        if (is_vowel(p.symbol) && !apple_vowel(m->first) && !(m->second && apple_vowel(m->second)) &&
            std::string(p.symbol) != "ER" && std::string(p.symbol) != "RX")
            bad += std::string(p.symbol) + " is a vowel but sung without one; ";
        if (m->shape != kOne && !m->second) bad += std::string(p.symbol) + " has no second phoneme; ";
    }
    for (const Mapping &m : kMap)
        if (!phoneme(m.symbol)) bad += std::string(m.symbol) + " is not a VocalWriter phoneme; ";
    if (sizeof kMap / sizeof kMap[0] != phonemes().size())
        bad += "the table has " + std::to_string(sizeof kMap / sizeof kMap[0]) + " rows for " +
               std::to_string(phonemes().size()) + " phonemes; ";
    if (fixed(220.0, 2) != "220.00" || fixed(3.14159, 1) != "3.1" || fixed(0.05, 1) != "0.1")
        bad += "numbers are not written as MacinTalk reads them; ";
    return bad;
}

}  // namespace svs
