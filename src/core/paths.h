// paths.h -- where the program's data lives.
//
// None of the synthesisers' own data is part of this program: VocalWriter's
// tables, voice bank and dictionary belong to KAE Labs, Microsoft's voice files
// to Microsoft. They are looked for beside the program, the way an emulator
// looks for the software it runs:
//
//   assets\                 VocalWriter 2.0's files, as the VocalWriter
//                           repository lays them out
//   voices\microsoft\       Sam.spd/.sdf, Mike.*, Mary.*, LTTS1033.LXA,
//                           r1033tts.LXA
//   voices\dectalk\         DECtalk's dictionary
//
// Places searched, nearest first: beside the executable, the folder above it,
// the source tree it was built from, and $SVS_DATA. On macOS the executable
// is inside the bundle, so the bundle's Resources folder and the folder the
// .app sits in are searched too.
#pragma once

#include <string>
#include <vector>

namespace svs {

std::string exe_dir();
std::vector<std::string> data_roots();
//: The first root under which `relative` exists, joined to it; or the
//: path under the first root if none has it (so a message can name it).
std::string find_data(const std::string &relative);
bool data_exists(const std::string &relative);

//: Join with this platform's separator. `b` may be written with either
//: separator (the data's names are written with backslashes throughout).
std::string join_path(const std::string &a, const std::string &b);
//: A relative data path as this platform writes it, for a message.
std::string data_name(const std::string &relative);
std::string base_name(const std::string &path);
std::string strip_extension(const std::string &path);
std::string extension_lower(const std::string &path);
bool file_exists(const std::string &path);
bool dir_exists(const std::string &path);
bool make_dirs(const std::string &path);
std::string temp_dir();
std::string settings_dir();
bool read_file(const std::string &path, std::vector<unsigned char> *out);

//: Windows wants wide strings for anything outside ASCII; everything in this
//: program is UTF-8 until the moment it reaches the system.
std::wstring widen(const std::string &utf8);
std::string narrow(const std::wstring &wide);

}  // namespace svs
