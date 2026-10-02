# Singing Voice Studio
#
#   sh build.sh            (msys2 mingw64 GCC, CMake and Ninja; see the README)
#
# Builds, statically, so that each executable stands on its own:
#
#   SingingVoiceStudio.exe   the editor
#   svs.exe                  the same program as a console program, for
#                            scripts: a song in, a WAV out
#
# The synthesisers are compiled in. Their data is not -- see src/core/paths.h.
cmake_minimum_required(VERSION 3.20)
project(SingingVoiceStudio C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

# -- VocalWriter's engine ----------------------------------------------------
#
# VocalWriter's own PowerPC code lifted into C (the VocalWriterC project),
# checked against the original until every sample agrees. The flags are part
# of the port: -ffp-contract=off keeps every multiply-add as two roundings, as
# the PowerPC computed it, and -fwrapv gives the wrap-around integer arithmetic
# the original relied on.
set(VW_UNITS speech tables music reverb macshim orthtophon parsephons convertsmf
             expandtracks synthapi synthglue bank song vw_api editor)
set(VW_SOURCES)
foreach(u ${VW_UNITS})
  list(APPEND VW_SOURCES ${SVS_ROOT}/engine/src/${u}.c)
endforeach()
add_library(vwengine STATIC ${VW_SOURCES})
target_include_directories(vwengine PUBLIC ${SVS_ROOT}/engine/include
                                    PRIVATE ${SVS_ROOT}/engine/test)
target_compile_options(vwengine PRIVATE -O2 -std=c11 -w -ffp-contract=off -fwrapv
                       -fno-fast-math -fexcess-precision=standard)

# -- the other synthesisers --------------------------------------------------
#
# Each lives in cmake/engines/<name>.cmake, which defines a static library
# called svs_<name> holding src/voices/<name>.cpp and the engine's own
# sources, and defines SVS_HAVE_<NAME> on it. An engine that is not there is
# replaced by a stub that offers its voices and says it cannot sing them.
set(SVS_ENGINES "dectalk;ssi263;microsoft" CACHE STRING "the engines to build in")
set(SVS_ENGINE_LIBS)
set(SVS_ENGINE_DEFS)
foreach(eng ${SVS_ENGINES})
  if(EXISTS ${SVS_ROOT}/cmake/engines/${eng}.cmake)
    include(${SVS_ROOT}/cmake/engines/${eng}.cmake)
    list(APPEND SVS_ENGINE_LIBS svs_${eng})
    string(TOUPPER ${eng} ENG)
    list(APPEND SVS_ENGINE_DEFS SVS_HAVE_${ENG}=1)
  endif()
endforeach()

# -- wxWidgets, statically ---------------------------------------------------
find_program(SH_EXE sh)
find_program(WX_CONFIG wx-config)
if(NOT WX_CONFIG)
  message(FATAL_ERROR "wx-config not found: install mingw-w64-x86_64-wxwidgets3.2-msw")
endif()
execute_process(COMMAND ${SH_EXE} ${WX_CONFIG} --static=yes --cxxflags
                OUTPUT_VARIABLE WX_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${SH_EXE} ${WX_CONFIG} --static=yes --libs core,base
                OUTPUT_VARIABLE WX_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
separate_arguments(WX_CXXFLAGS NATIVE_COMMAND "${WX_CXXFLAGS}")
# the one library wx-config names as a DLL has a static twin beside it
string(REGEX REPLACE "[^ ]*liblzma\\.dll\\.a" "-l:liblzma.a" WX_LIBS "${WX_LIBS}")
separate_arguments(WX_LIBS NATIVE_COMMAND "${WX_LIBS}")

# -- the program -------------------------------------------------------------
set(CORE_SOURCES
  src/core/paths.cpp
  src/core/phonology.cpp
  src/core/project.cpp
  src/core/midifile.cpp
  src/core/settings.cpp
  src/audio/wav.cpp
  src/audio/mixer.cpp
  src/audio/player.cpp
  src/voices/pitch.cpp
  src/voices/registry.cpp
  src/voices/vocalwriter.cpp
  src/voices/stubs.cpp
)
add_library(svs_core STATIC ${CORE_SOURCES})
target_include_directories(svs_core PUBLIC ${SVS_ROOT}/src ${SVS_ROOT}/third_party/nlohmann)
target_compile_definitions(svs_core PUBLIC SVS_SOURCE_DIR="${SVS_ROOT}" ${SVS_ENGINE_DEFS})
target_link_libraries(svs_core PUBLIC vwengine ${SVS_ENGINE_LIBS} winmm)

set(APP_SOURCES
  src/main.cpp
  src/app/cli.cpp
  src/app/studio.cpp
  src/app/dialogs.cpp
  src/app/widgets.cpp
  src/app/announce.cpp
  src/app/worker.cpp
)

function(svs_executable name console)
  add_executable(${name} ${APP_SOURCES})
  target_compile_options(${name} PRIVATE ${WX_CXXFLAGS})
  target_link_libraries(${name} PRIVATE svs_core ${WX_LIBS} uiautomationcore)
  target_link_options(${name} PRIVATE -static -static-libgcc -static-libstdc++)
  if(console)
    target_compile_definitions(${name} PRIVATE SVS_CONSOLE=1)
  else()
    target_link_options(${name} PRIVATE -mwindows)
  endif()
endfunction()

svs_executable(SingingVoiceStudio OFF)
svs_executable(svs ON)
