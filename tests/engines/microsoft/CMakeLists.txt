# The Microsoft engine on its own: a console program that lists the voices, says whether the
# voice data is there, checks the phoneme table, and with the data sings a short tune to WAV.
#
#   cmake -G Ninja -S tests/engines/microsoft -B build-tests/microsoft
#   cmake --build build-tests/microsoft
#   build-tests/microsoft/microsoft_sing_test [--data DIR] [--out DIR]
cmake_minimum_required(VERSION 3.20)
project(microsoft_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/microsoft.cmake)

add_executable(microsoft_sing_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
target_compile_definitions(microsoft_sing_test PRIVATE SVS_SOURCE_DIR="${SVS_ROOT}")
target_compile_options(microsoft_sing_test PRIVATE -Wall -Wextra)
target_link_libraries(microsoft_sing_test PRIVATE svs_microsoft)
target_link_options(microsoft_sing_test PRIVATE -static -static-libgcc -static-libstdc++)
