# Prose 2000: Speech Plus / Telesensory Systems' text-to-speech board of 1988,
# an 8086 running the text and prosody rules and an NEC uPD7720 DSP running a
# formant synthesiser.
#
# The engine is third_party/prose2000_reborn, a decompilation of the board's
# v3.4.1 and v1.1 firmware into C, with a model of the DSP program, that is
# checked frame and sample exact against an emulator of the board. It is
# compiled as its own build compiles its static speech library (prose_static),
# with the ROM's data built in, so it needs no data.
#
# One change to the frame builder, made here so the engine stays as its authors
# wrote it: the two pitch periods it works out for a frame, in whole samples
# from an F0 byte in hertz, can be replaced by a hook the singer sets
# (svs_prose_period_hook), which is how src/voices/prose.cpp sings above 255 Hz
# and between whole periods.
#
# Included by the root CMakeLists.txt and by tests/engines/prose; both set
# SVS_ROOT.
set(SVS_PROSE_DIR ${SVS_ROOT}/third_party/prose2000_reborn/src)
set(SVS_PROSE_OUT ${CMAKE_CURRENT_BINARY_DIR}/prose)
if(NOT EXISTS ${SVS_PROSE_DIR}/frame/frame_build.c)
  message(FATAL_ERROR "third_party/prose2000_reborn is empty: git submodule update --init")
endif()

file(READ ${SVS_PROSE_DIR}/frame/frame_build.c SVS_PROSE_FRAME)
function(svs_prose_patch from to)
  string(FIND "${SVS_PROSE_FRAME}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "frame_build.c has changed: cannot find the line to patch: ${from}")
  endif()
  string(REPLACE "${from}" "${to}" patched "${SVS_PROSE_FRAME}")
  set(SVS_PROSE_FRAME "${patched}" PARENT_SCOPE)
endfunction()
svs_prose_patch("static int16_t half_period(int16_t x)\n"
                "void (*svs_prose_period_hook)(int16_t *per1, int16_t *per2);\n\nstatic int16_t half_period(int16_t x)\n")
svs_prose_patch("		int16_t per1 = half_period(f1), per2 = half_period(f2);\n"
                "		int16_t per1 = half_period(f1), per2 = half_period(f2);\n		if (svs_prose_period_hook)\n			svs_prose_period_hook(&per1, &per2);\n")
file(WRITE ${SVS_PROSE_OUT}/next/frame_build.c "${SVS_PROSE_FRAME}")
configure_file(${SVS_PROSE_OUT}/next/frame_build.c ${SVS_PROSE_OUT}/frame_build.c COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_PROSE_DIR}/frame/frame_build.c)

# everything prose_static is built from: the firmware's stages for both
# versions, the ROM's data, the frame builder and the DSP model, and the library
# around them
set(SVS_PROSE_DIRS common data dsp frame paramgen prosody lexical input textrules v1 dll)
set(SVS_PROSE_SOURCES)
foreach(d ${SVS_PROSE_DIRS})
  file(GLOB c ${SVS_PROSE_DIR}/${d}/*.c)
  list(APPEND SVS_PROSE_SOURCES ${c})
endforeach()
list(REMOVE_ITEM SVS_PROSE_SOURCES ${SVS_PROSE_DIR}/frame/frame_build.c)
list(APPEND SVS_PROSE_SOURCES ${SVS_PROSE_OUT}/frame_build.c)
list(TRANSFORM SVS_PROSE_DIRS PREPEND ${SVS_PROSE_DIR}/)

add_library(svs_prose_engine OBJECT ${SVS_PROSE_SOURCES})
target_include_directories(svs_prose_engine PRIVATE ${SVS_PROSE_DIR}/include ${SVS_PROSE_DIRS})
set_target_properties(svs_prose_engine PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
# The engine is kept as its authors wrote it; its warnings are theirs.
target_compile_options(svs_prose_engine PRIVATE -O2 -w)
target_compile_definitions(svs_prose_engine PRIVATE PROSE_STATIC)

add_library(svs_prose STATIC ${SVS_ROOT}/src/voices/prose.cpp $<TARGET_OBJECTS:svs_prose_engine>)
target_include_directories(svs_prose PUBLIC ${SVS_ROOT}/src PRIVATE ${SVS_PROSE_DIR}/include)
set_target_properties(svs_prose PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_prose PRIVATE -O2)
target_compile_definitions(svs_prose PUBLIC SVS_HAVE_PROSE=1 PRIVATE PROSE_STATIC)
# the library's sound output, which the singer does not use: winmm on Windows,
# and PulseAudio or ALSA loaded at run time elsewhere
find_package(Threads REQUIRED)
if(WIN32)
  target_link_libraries(svs_prose PUBLIC winmm)
else()
  target_link_libraries(svs_prose PUBLIC ${CMAKE_DL_LIBS})
endif()
# prose.cpp uses the studio's phonology and pitch curve, which live in svs_core
# in the program; svs_core in turn links this library, and naming it here lets
# the linker go round the two again. In the engine's own test there is no
# svs_core and the test lists those sources.
# The sung phrase is retuned to its curve (macintalk_dsp.cpp).
include(${SVS_ROOT}/cmake/engines/dsp.cmake)
target_link_libraries(svs_prose PUBLIC svs_dsp $<TARGET_NAME_IF_EXISTS:svs_core> Threads::Threads m)
