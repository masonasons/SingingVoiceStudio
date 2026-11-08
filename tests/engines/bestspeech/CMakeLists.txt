# The BeSTspeech engine on its own: a console program that lists the voices,
# checks the phoneme table, and sings a short tune to WAV with every voice,
# checking the notes' pitches.
#
#   cmake -G Ninja -S tests/engines/bestspeech -B build-tests/bestspeech
#   cmake --build build-tests/bestspeech
#   build-tests/bestspeech/bestspeech_sing_test [--out DIR] [--levels]
cmake_minimum_required(VERSION 3.20)
project(bestspeech_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/bestspeech.cmake)

add_executable(bestspeech_sing_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
target_compile_definitions(bestspeech_sing_test PRIVATE SVS_SOURCE_DIR="${SVS_ROOT}")
target_compile_options(bestspeech_sing_test PRIVATE -Wall -Wextra)
target_link_libraries(bestspeech_sing_test PRIVATE svs_bestspeech)
target_link_options(bestspeech_sing_test PRIVATE -static -static-libgcc -static-libstdc++)
