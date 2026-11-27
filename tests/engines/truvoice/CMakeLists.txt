# TruVoice on its own: a console program that lists the voices, checks the
# phoneme table, and sings a short tune to WAV with every voice, checking the
# notes' pitches.
#
#   cmake -G Ninja -S tests/engines/truvoice -B build-tests/truvoice
#   cmake --build build-tests/truvoice
#   build-tests/truvoice/truvoice_sing_test [--out DIR] [--levels]
cmake_minimum_required(VERSION 3.20)
project(truvoice_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/truvoice.cmake)

add_executable(truvoice_sing_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
target_compile_definitions(truvoice_sing_test PRIVATE SVS_SOURCE_DIR="${SVS_ROOT}")
target_compile_options(truvoice_sing_test PRIVATE -Wall -Wextra)
target_link_libraries(truvoice_sing_test PRIVATE svs_truvoice)
target_link_options(truvoice_sing_test PRIVATE -static -static-libgcc -static-libstdc++)
