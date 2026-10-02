# A standalone check of the SSI-263 singer: it builds the engine exactly as
# the program does (cmake/engines/ssi263.cmake) and sings a few phrases to
# WAV files.
#
#   cmake -G Ninja -S tests/engines/ssi263 -B build-tests/ssi263
#   cmake --build build-tests/ssi263
#   build-tests/ssi263/ssi263_sing_test build-tests/out
cmake_minimum_required(VERSION 3.20)
project(ssi263_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/ssi263.cmake)

add_executable(ssi263_sing_test
  sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
target_compile_options(ssi263_sing_test PRIVATE -Wall -Wextra)
target_compile_options(svs_ssi263 PRIVATE -Wall -Wextra)
target_link_libraries(ssi263_sing_test PRIVATE svs_ssi263)
target_link_options(ssi263_sing_test PRIVATE -static)
