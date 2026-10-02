// crash.h -- when the program dies, leave something behind that says where.
//
// A crash that happens "now and then, on a key" on someone else's machine is
// not something that can be found by looking at the code. So on any crash the
// program writes, into its settings folder:
//
//   crash-<date>-<time>.txt   what happened: the exception, the address and
//                             the module it fell in, the call stack as module
//                             and offset, the build, the last things said in
//                             Messages
//   crash-<date>-<time>.dmp   a Windows minidump of the moment, for a debugger
//
// and the next start says it found one. The offsets resolve against the
// linker map kept beside each release build (build\SingingVoiceStudio.map).
#pragma once

#include <string>
#include <vector>

namespace svs {

//: Install the handlers. Call once, first thing.
void install_crash_reporter();
//: Remember a line of what the program said, so a report can show what led up
//: to the crash. Cheap; keeps the last few dozen.
void crash_note(const std::string &line);
//: Reports left by earlier runs that have not been mentioned yet; each is
//: returned once.
std::vector<std::string> unseen_crash_reports();

}  // namespace svs
