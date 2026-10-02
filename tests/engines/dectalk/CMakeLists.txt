# DECtalk on its own: a console program that lists the voices, says whether
# DECtalk.dll and its dictionary are there, and if they are, sings a short
# tune in a few voices to WAV files and reports how well the timing and the
# pitch came out.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build_dectalk.ps1
#   cmake -G Ninja -S tests/engines/dectalk -B build-tests/dectalk
#   cmake --build build-tests/dectalk
#   build-tests/dectalk/dectalk_sing_test [OUT_DIR]
cmake_minimum_required(VERSION 3.20)
project(dectalk_sing_test C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(SVS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
include(${SVS_ROOT}/cmake/engines/dectalk.cmake)

add_executable(dectalk_sing_test
  ${CMAKE_CURRENT_SOURCE_DIR}/sing_test.cpp
  ${SVS_ROOT}/src/voices/pitch.cpp
  ${SVS_ROOT}/src/audio/wav.cpp
  ${SVS_ROOT}/src/core/paths.cpp
  ${SVS_ROOT}/src/core/phonology.cpp
)
# so that voices\dectalk is found in the source tree
target_compile_definitions(dectalk_sing_test PRIVATE SVS_SOURCE_DIR="${SVS_ROOT}")
target_compile_options(dectalk_sing_test PRIVATE -Wall -Wextra)
target_link_libraries(dectalk_sing_test PRIVATE svs_dectalk)
target_link_options(dectalk_sing_test PRIVATE -static -static-libgcc -static-libstdc++)
