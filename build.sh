#!/bin/sh
# Build Singing Voice Studio with msys2's mingw64 GCC, CMake and Ninja.
# (On macOS use scripts/build_mac.sh instead.)
#
#   sh build.sh              everything, into build/
#   sh build.sh clean        start again
#
# Needs: pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake
#        mingw-w64-x86_64-ninja mingw-w64-x86_64-wxwidgets3.2-msw
# DECtalk is built separately, with Visual Studio: scripts/build_dectalk.ps1.
set -e
cd "$(dirname "$0")"
MINGW=${MINGW:-/c/Users/mew/scoop/apps/msys2/current/mingw64}
if [ -d "$MINGW/bin" ]; then
    PATH="$MINGW/bin:$MINGW/../usr/bin:$PATH"
fi
[ "$1" = clean ] && rm -rf build
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release ${SVS_CMAKE_ARGS}
cmake --build build
