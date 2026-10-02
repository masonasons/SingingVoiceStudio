# Singing Voice Studio
#
#   sh build.sh                 (Windows: msys2 mingw64 GCC, CMake and Ninja; see the README)
#   sh scripts/build_mac.sh     (macOS: clang, CMake and Ninja; builds wxWidgets and DECtalk too)
#
# Builds, statically, so that each executable stands on its own:
#
#   SingingVoiceStudio.exe   the editor                           (Windows)
#   svs.exe                  the same program as a console program, for
#                            scripts: a song in, a WAV out
#   SingingVoiceStudio.app   the editor, as a bundle                (macOS)
#   svs                      the same program under a shorter name
#
# The synthesisers are compiled in. Their data is not -- see src/core/paths.h.
cmake_minimum_required(VERSION 3.20)
if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "the oldest macOS the program runs on")
endif()
project(SingingVoiceStudio C CXX)
if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 17)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(SVS_VERSION "1.0")

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
target_compile_options(vwengine PRIVATE -O2 -std=c11 -w -ffp-contract=off -fwrapv -fno-fast-math)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  # clang has no excess precision to turn off on these targets
  target_compile_options(vwengine PRIVATE -fexcess-precision=standard)
endif()

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

# -- wxWidgets, statically, at a master commit ------------------------------
#
# Built from source as part of this build rather than taken from a package,
# as FastPlay does: the 3.3 line at a master commit, which carries the screen
# reader fixes made since the last release. Move the pin forward by changing
# the commit; move to a 3.3 release tag once one has them.
#
# On macOS, for now, scripts/build_mac.sh builds wxWidgets 3.2 itself and it
# is found through its wx-config.
if(WIN32)
  include(FetchContent)
  set(FETCHCONTENT_QUIET OFF)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Floor for old dependencies")
  FetchContent_Declare(wxWidgets
      GIT_REPOSITORY https://github.com/wxWidgets/wxWidgets.git
      GIT_TAG e320e82ec94df7ff5bb85b73344407190ce1d4f6     # master, 2026-10-01
      GIT_SHALLOW FALSE
  )
  set(wxBUILD_SHARED OFF CACHE INTERNAL "")
  set(wxBUILD_USE_STATIC_RUNTIME ON CACHE INTERNAL "")
  set(wxBUILD_TESTS OFF CACHE INTERNAL "")
  set(wxBUILD_SAMPLES OFF CACHE INTERNAL "")
  set(wxBUILD_DEMOS OFF CACHE INTERNAL "")
  set(wxBUILD_PRECOMP ON CACHE INTERNAL "")
  set(wxBUILD_INSTALL OFF CACHE INTERNAL "")
  set(wxUSE_STL ON CACHE INTERNAL "")
  # What the screen reader support stands on; on by default for Windows, and
  # said here so that it cannot quietly go away.
  set(wxUSE_ACCESSIBILITY ON CACHE INTERNAL "")
  # The program uses core and base; the rest of wxWidgets is most of the build
  # for code that is never called.
  set(wxUSE_STC OFF CACHE INTERNAL "")
  set(wxUSE_WEBVIEW OFF CACHE INTERNAL "")
  set(wxUSE_MEDIACTRL OFF CACHE INTERNAL "")
  set(wxUSE_RICHTEXT OFF CACHE INTERNAL "")
  set(wxUSE_PROPGRID OFF CACHE INTERNAL "")
  set(wxUSE_RIBBON OFF CACHE INTERNAL "")
  set(wxUSE_AUI OFF CACHE INTERNAL "")
  set(wxUSE_XRC OFF CACHE INTERNAL "")
  set(wxUSE_LIBJPEG OFF CACHE INTERNAL "")
  set(wxUSE_LIBTIFF OFF CACHE INTERNAL "")
  set(wxUSE_LIBWEBP OFF CACHE INTERNAL "")
  FetchContent_MakeAvailable(wxWidgets)
else()
  # scripts/build_mac.sh builds a static wxWidgets into build-deps/wx; one
  # installed elsewhere can be named with -DWX_CONFIG=<path to wx-config>.
  find_program(WX_CONFIG wx-config HINTS ${SVS_ROOT}/build-deps/wx/bin NO_DEFAULT_PATH)
  if(NOT WX_CONFIG)
    find_program(WX_CONFIG wx-config)
  endif()
  if(NOT WX_CONFIG)
    message(FATAL_ERROR "wx-config not found: run scripts/build_mac.sh, which builds wxWidgets")
  endif()
  execute_process(COMMAND ${WX_CONFIG} --static=yes --cxxflags
                  OUTPUT_VARIABLE WX_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND ${WX_CONFIG} --static=yes --libs core,base
                  OUTPUT_VARIABLE WX_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
  separate_arguments(WX_CXXFLAGS UNIX_COMMAND "${WX_CXXFLAGS}")
  separate_arguments(WX_LIBS_SPLIT UNIX_COMMAND "${WX_LIBS}")
  # "-framework X" is one argument to the linker, not two
  set(WX_LIBS)
  set(WX_PENDING_FRAMEWORK OFF)
  foreach(item ${WX_LIBS_SPLIT})
    if(WX_PENDING_FRAMEWORK)
      list(APPEND WX_LIBS "-framework ${item}")
      set(WX_PENDING_FRAMEWORK OFF)
    elseif(item STREQUAL "-framework")
      set(WX_PENDING_FRAMEWORK ON)
    else()
      list(APPEND WX_LIBS ${item})
    endif()
  endforeach()
endif()

# -- the program -------------------------------------------------------------
set(CORE_SOURCES
  src/core/paths.cpp
  src/core/phonology.cpp
  src/core/project.cpp
  src/core/midifile.cpp
  src/core/settings.cpp
  src/audio/wav.cpp
  src/audio/mixer.cpp
  src/voices/pitch.cpp
  src/voices/registry.cpp
  src/voices/vocalwriter.cpp
  src/voices/stubs.cpp
)
if(APPLE)
  list(APPEND CORE_SOURCES src/audio/player_mac.mm)
else()
  list(APPEND CORE_SOURCES src/audio/player.cpp)
endif()
add_library(svs_core STATIC ${CORE_SOURCES})
target_include_directories(svs_core PUBLIC ${SVS_ROOT}/src ${SVS_ROOT}/third_party/nlohmann)
target_compile_definitions(svs_core PUBLIC SVS_SOURCE_DIR="${SVS_ROOT}" ${SVS_ENGINE_DEFS})
target_link_libraries(svs_core PUBLIC vwengine ${SVS_ENGINE_LIBS})
if(WIN32)
  target_link_libraries(svs_core PUBLIC winmm)
elseif(APPLE)
  target_link_libraries(svs_core PUBLIC "-framework AVFoundation" "-framework Foundation")
endif()

set(APP_SOURCES
  src/main.cpp
  src/app/cli.cpp
  src/app/studio.cpp
  src/app/dialogs.cpp
  src/app/widgets.cpp
  src/app/worker.cpp
)
if(APPLE)
  list(APPEND APP_SOURCES src/app/announce_mac.mm)
else()
  list(APPEND APP_SOURCES src/app/announce.cpp)
endif()

function(svs_executable name console)
  if(APPLE AND NOT console)
    add_executable(${name} MACOSX_BUNDLE ${APP_SOURCES})
    set_target_properties(${name} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST ${SVS_ROOT}/cmake/Info.plist.in
      MACOSX_BUNDLE_BUNDLE_NAME "Singing Voice Studio"
      MACOSX_BUNDLE_GUI_IDENTIFIER "com.github.masonasons.SingingVoiceStudio"
      MACOSX_BUNDLE_BUNDLE_VERSION "${SVS_VERSION}"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "${SVS_VERSION}"
      MACOSX_BUNDLE_COPYRIGHT "MIT. Sings with VocalWriter 2.0 (KAE Labs, 2005), DECtalk, the SSI-263 and Microsoft Sam, Mike and Mary, none of which are included.")
  else()
    add_executable(${name} ${APP_SOURCES})
  endif()
  if(WIN32)
    target_link_libraries(${name} PRIVATE svs_core wx::core wx::base)
    target_link_options(${name} PRIVATE -static -static-libgcc -static-libstdc++)
    if(console)
      target_compile_definitions(${name} PRIVATE SVS_CONSOLE=1)
    else()
      target_link_options(${name} PRIVATE -mwindows)
    endif()
  elseif(APPLE)
    target_compile_options(${name} PRIVATE ${WX_CXXFLAGS})
    target_link_libraries(${name} PRIVATE svs_core ${WX_LIBS})
    target_link_libraries(${name} PRIVATE "-framework AppKit")
    if(console)
      target_compile_definitions(${name} PRIVATE SVS_CONSOLE=1)
    endif()
  endif()
endfunction()

svs_executable(SingingVoiceStudio OFF)
svs_executable(svs ON)
