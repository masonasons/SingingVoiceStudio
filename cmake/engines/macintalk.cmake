# MacinTalk: Apple's voices from Mac OS X 10.4 to 10.7.
#
# Nothing of Apple's is built here, and nothing of Panthera's: the engines run
# in Panthera's own host process (panthera_host.exe, from its NVDA add-on),
# which src/voices/macintalk.cpp starts and talks to over pipes. This library
# is only that file on Windows. On macOS the system's own voices sing
# instead, through AVSpeechSynthesizer, retimed and retuned to the score.
#
# Included by the root CMakeLists.txt and by tests/engines/macintalk; both set
# SVS_ROOT.
add_library(svs_macintalk STATIC ${SVS_ROOT}/src/voices/macintalk.cpp)
if(APPLE)
  # On a Mac the system's own voices sing: spoken through AVSpeechSynthesizer
  # (macintalk_mac.mm) and retimed and retuned to the score (macintalk_dsp.cpp).
  target_sources(svs_macintalk PRIVATE ${SVS_ROOT}/src/voices/macintalk_mac.mm)
  include(${SVS_ROOT}/cmake/engines/dsp.cmake)
  target_link_libraries(svs_macintalk PUBLIC svs_dsp)
  set_source_files_properties(${SVS_ROOT}/src/voices/macintalk_mac.mm PROPERTIES
                              COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(svs_macintalk PUBLIC "-framework AVFoundation" "-framework Foundation")
endif()
target_include_directories(svs_macintalk PUBLIC ${SVS_ROOT}/src)
set_target_properties(svs_macintalk PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_macintalk PRIVATE -O2)
target_compile_definitions(svs_macintalk PUBLIC SVS_HAVE_MACINTALK=1)
# macintalk.cpp uses the studio's paths, phonology and pitch curve, which live
# in svs_core in the program; svs_core in turn links this library, and naming
# it here lets the linker go round the two again. In the engine's own test
# there is no svs_core and the test lists those sources.
target_link_libraries(svs_macintalk PUBLIC $<TARGET_NAME_IF_EXISTS:svs_core>)
