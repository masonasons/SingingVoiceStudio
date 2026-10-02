# Singing Voice Studio

VocalWriter Studio rebuilt in C++ and wxWidgets, with more singers in it, for
Windows and macOS. The window, the menus, the keys, the dialogs, the messages
and the project files are VocalWriter Studio's own (see
`C:\stuff\VocalWriter\app`); what is new is that a track can be sung by any of
four synthesisers:

| engine | voices | numbers | what it is |
|---|---|---|---|
| **VocalWriter 2.0** (KAE Labs, 2005) | 87, the bank's own | 0 - 86 | the original's PowerPC code lifted into C (`engine/`, VocalWriterC) |
| **DECtalk** | Paul, Betty, Harry, Frank, Dennis, Kit, Ursula, Rita, Wendy, Val | 1000 - 1009 | github.com/dectalk/dectalk, sung through its phoneme `<duration,pitch>` notation |
| **SSI-263** | SSI-263, Deep, Bright, Child | 2000 - 2003 | the chip itself, driven at register level (github.com/tgeczy/ssi263-speech) |
| **Microsoft** | Sam, Mike, Mary, each plain, in Hall, in Stadium, in Space, Whisper, RoboSoft One to Six | 3000 - 3050 | the SAPI 5 engine rebuilt in C, with the SAPI 4 voice modes (github.com/KamiKitsune420/ms-sam-mike-mary-decomp) |

A voice's number is what a song saves, and it is fixed per engine, so a song
written with DECtalk Betty still says Betty on a machine without DECtalk and
says why it cannot sing there.

Everything renders through one path. A part is cut into phrases at its rests,
the consonants are moved in front of the beat so the vowel lands on it, each
phrase is sung by the part's voice and placed where the score puts it, and the
parts are panned and mixed, the ones sharing a reverb setting through one room.
VocalWriter's voices get exactly the calls the original made -- a song sung by
VocalWriter's voices here is the same file, sample for sample, as the Python
VocalWriter Studio renders, and a project saved here is the same file byte for
byte. The other engines are handed the same notes, the same phoneme timings
and one shared pitch curve (written note, bend, detune, portamento, vibrato),
each in its own terms: DECtalk a pitch per phoneme in Hz, the SSI-263 its
inflection register every 32 samples, Microsoft's vocoder twenty pitch knots
per acoustic unit.

**The mod wheel** sits beside pitch bend on every note -- two fields in the
note editor, Mod points for the whole curve, a Mod column in the list, CC 1
when a MIDI file is imported. As General MIDI has it, the wheel is vibrato: at
rest the part sings with its own vibrato setting, and turning it up deepens it
towards full. VocalWriter's voices get it as their own vibrato-depth control,
the others through the shared pitch curve.

Notes are written in VocalWriter's phoneme symbols whichever engine sings
them; each engine translates (the tables are at the top of
`src/voices/<engine>.cpp`). Add word uses VocalWriter's own dictionary.

## Data you supply

None of the synthesisers' data is in this program. It is looked for beside the
executable, the folder above it, the source tree, and `%SVS_DATA%` (on macOS
also the bundle's `Contents/Resources` and the folder the `.app` sits in):

```
assets\                     VocalWriter 2.0's files, laid out as in the
                            VocalWriter repository (VocalWriter.app\...\VocalWriter.rsrc,
                            GMSpeech.rsrc, GMBank.rsrc, EnglishLex)
voices\dectalk\             DECtalk.dll and dtalk_us.dic (scripts\build_dectalk.ps1 makes them)
voices\microsoft\           Sam.spd, Mike.spd, Mary.spd (any of them). Without them, the
                            installed copies are used: Common Files\SpeechEngines\Microsoft\TTS\1033
                            (the SAPI 5 voice package) or Common Files\Microsoft Shared\Speech\1033
                            (Windows XP)
```

On macOS DECtalk is `libtts.dylib`, built here with clang
(`scripts/build_dectalk_mac.sh`) and kept in the bundle's `Contents/Frameworks`.

The SSI-263's phoneme ROM is compiled in. An engine whose data is missing still
lists its voices, marked "(not installed)", and says what is missing in
Messages when the program starts.

## Build

msys2's mingw64 GCC, CMake and Ninja. wxWidgets is fetched from GitHub and built
with the program, statically, at a pinned master commit (see CMakeLists.txt), so
the first build takes a while longer:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja git
sh build.sh
```

That gives `build\SingingVoiceStudio.exe`, the editor, and `build\svs.exe`, the
same program as a console program. Both are linked statically and need no
DLLs of their own. DECtalk only builds with Visual Studio, so it is built
separately and loaded at run time:

```
powershell -File scripts\build_dectalk.ps1
```

Or all of it at once -- DECtalk, the program, every engine's data gathered
beside it, a check that it sings from there, and a zip:

```
powershell -ExecutionPolicy Bypass -File scripts\build_all.ps1          (add -Clean to start afresh)
```

That makes `dist\SingingVoiceStudio\` and `dist\SingingVoiceStudio-<date>.zip`,
which runs on any 64-bit Windows with nothing installed. It carries KAE Labs'
and Microsoft's data, so it is for your own machines, not for publishing.

`-DSVS_ENGINES="ssi263;microsoft"` (through `SVS_CMAKE_ARGS`) builds without an
engine; its voices stay listed.

### macOS

Xcode's command line tools, CMake and Ninja (`brew install cmake ninja`):

```
sh scripts/build_mac.sh
```

That builds wxWidgets statically into `build-deps/wx` the first time, DECtalk
with clang, then the program, and gathers every engine's data into
`dist/Singing Voice Studio.app` (VocalWriter's files from `assets/` here or a
VocalWriter Studio checkout beside this one, Microsoft's from
`voices/microsoft`), checks that each engine sings from inside the bundle,
signs it with the Developer ID in the keychain, puts it on
`dist/SingingVoiceStudio-<date>-macos-<arch>.dmg`, and has Apple notarize
both. `--no-sign` gives a bundle to run here; `--no-notarize` skips Apple; see
the top of the script for the credentials it looks for. `cmake --build build`
alone gives `build/SingingVoiceStudio.app` and `build/svs`, which find the
data in the source tree.

The program is the same on a Mac, with Cmd where this file says Ctrl and
Option where it says Alt, two keys excepted: hearing one note is Option+H,
since Cmd+H hides the program, and quitting is Cmd+Q. The lists are native
macOS tables there, so VoiceOver reads them, and what a key did is announced
through VoiceOver. On the command line the program is
`Singing Voice Studio.app/Contents/MacOS/svs`, the same `svs` as on Windows.
Settings live in `~/Library/Application Support/Singing Voice Studio`.

## The keys

As VocalWriter Studio's. F1 lists them in Messages. On a Mac read Cmd for
Ctrl and Option for Alt, and Option+H to hear a note.

| | |
|---|---|
| F6 | between the tracks list and the notes list |
| Ctrl+T | add a track |
| Enter on a track | its name, voice, volume, pan and voice controls |
| M / S on a track | mute / solo |
| Delete on a track | remove it |
| Ctrl+Up / Ctrl+Down | reorder the parts, or move a note |
| Ctrl+W / Ctrl+N / Ctrl+R | add a word / a note / a rest |
| Ctrl+Shift+R | a rest to the end of the bar |
| Ctrl+E, or Enter on a note | edit it, including its pitch bend |
| Ctrl+D, or Delete | remove it |
| Alt+Up / Alt+Down | transpose a semitone |
| Alt+Right / Alt+Left | a sixteenth note longer or shorter |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo |
| Ctrl+C / Ctrl+X / Ctrl+V / Ctrl+A | copy, cut, paste, select all |
| Ctrl+G | go to a bar |
| Ctrl+, | song settings |
| Ctrl+Shift+P | hear a note whenever it is nudged |
| Space | play from the cursor, or stop |
| Ctrl+P / Ctrl+H / Ctrl+M / Ctrl+. | play from the start / hear one note / metronome / stop |
| Ctrl+O / Ctrl+S | open and save a project |
| Ctrl+I | import a MIDI file |
| Ctrl+Shift+S / Ctrl+Shift+T | export one WAV / one WAV per track |

## From the command line

```
svs song.vws -o song.wav
svs tune.mid -o tune.wav --voice "DECtalk Betty" --tempo 96
svs song.vws --tracks stems
svs --list-voices
svs --pronounce daisy bicycle
```

## Layout

```
src/app/       the window, the dialogs, the command line (studio.cpp is VocalWriter
               Studio's app/studio.py, method for method; widgets.h is the list
               that is wxListCtrl on Windows and a native table on macOS;
               announce.cpp and announce_mac.mm speak to the screen reader)
src/core/      the song, projects, MIDI import, phonology, settings and recovery
src/audio/     the mixer (VocalWriter Studio's ppc/engine.py), WAV files, the player
               (player.cpp on Windows, player_mac.mm on macOS)
src/voices/    singer.h (what an engine is asked to do), the four engines, the shared
               pitch curve
engine/        VocalWriter's synthesiser in C (VocalWriterC)
third_party/   DECtalk, the SSI-263, Microsoft Sam's reconstruction, nlohmann/json
scripts/       the Windows build (build_all.ps1, build_dectalk.ps1) and the macOS
               build (build_mac.sh, build_dectalk_mac.sh)
tests/engines/ a standalone sing test for each engine
```

## Licences

The code here is MIT, as VocalWriter Studio's is. VocalWriter 2.0 and its data
are KAE Labs'; DECtalk, the SSI-263 emulation and the Sam reconstruction carry
their own licences in `third_party/`. Microsoft's voice files are Microsoft's
and are not included.
