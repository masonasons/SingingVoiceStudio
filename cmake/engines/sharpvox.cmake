# SharpVox TTS: a formant synthesiser in the manner of Klatt's, with DECtalk's
# phonemes.
#
# The engine is third_party/SharpVoxTTS (MIT), compiled as it is: only the part
# that turns phonemes into sound -- the phoneme processor, the renderer, its
# pitch interpolator and tables, the synthesiser and the voice presets. The
# dictionary, the letter-to-sound rules and the Japanese reader are for reading
# text, which nothing here does, and are most of its size, so they are left out.
#
# One change to the synthesiser, made here so the engine stays as its author
# wrote it: its output stage is 12 dB lower. Sung vowels held at full voice go
# past its 16-bit full scale, which it clips; src/voices/sharpvox.cpp gives the
# 12 dB back in floats.
#
# Included by the root CMakeLists.txt and by tests/engines/sharpvox; both set
# SVS_ROOT.
set(SVS_SHARPVOX_DIR ${SVS_ROOT}/third_party/SharpVoxTTS)
if(NOT EXISTS ${SVS_SHARPVOX_DIR}/src/KlattSynthesizerFP.cpp)
  message(FATAL_ERROR "third_party/SharpVoxTTS is empty: git submodule update --init")
endif()

file(READ ${SVS_SHARPVOX_DIR}/src/KlattSynthesizerFP.cpp SVS_SHARPVOX_SYNTH)
set(SVS_SHARPVOX_GAIN "_outputGain   = 4.10f;")
string(FIND "${SVS_SHARPVOX_SYNTH}" "${SVS_SHARPVOX_GAIN}" at)
if(at EQUAL -1)
  message(FATAL_ERROR "KlattSynthesizerFP.cpp has changed: cannot find the line to patch: ${SVS_SHARPVOX_GAIN}")
endif()
string(REPLACE "${SVS_SHARPVOX_GAIN}" "_outputGain   = 4.10f / 4.0f;" SVS_SHARPVOX_SYNTH "${SVS_SHARPVOX_SYNTH}")
# its headers are named as ../include/, which from the build folder is the
# include path's
string(REPLACE "#include \"../include/" "#include \"" SVS_SHARPVOX_SYNTH "${SVS_SHARPVOX_SYNTH}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sharpvox/next/KlattSynthesizerFP.cpp "${SVS_SHARPVOX_SYNTH}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/sharpvox/next/KlattSynthesizerFP.cpp
               ${CMAKE_CURRENT_BINARY_DIR}/sharpvox/KlattSynthesizerFP.cpp COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_SHARPVOX_DIR}/src/KlattSynthesizerFP.cpp)

set(SVS_SHARPVOX_ENGINE_SOURCES
  ${SVS_SHARPVOX_DIR}/src/AudioProcessor.cpp
  ${SVS_SHARPVOX_DIR}/src/SpeechRenderer.cpp
  ${SVS_SHARPVOX_DIR}/src/PitchInterpolator.cpp
  ${SVS_SHARPVOX_DIR}/src/Tables.cpp
  ${SVS_SHARPVOX_DIR}/src/VoicePresets.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/sharpvox/KlattSynthesizerFP.cpp
)

add_library(svs_sharpvox STATIC ${SVS_ROOT}/src/voices/sharpvox.cpp ${SVS_SHARPVOX_ENGINE_SOURCES})
target_include_directories(svs_sharpvox PUBLIC ${SVS_ROOT}/src PRIVATE ${SVS_SHARPVOX_DIR}/include)
set_target_properties(svs_sharpvox PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_sharpvox PRIVATE -O2)
# The engine is kept as its author wrote it; its warnings are his.
set_source_files_properties(${SVS_SHARPVOX_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(svs_sharpvox PUBLIC SVS_HAVE_SHARPVOX=1)
# sharpvox.cpp uses the studio's phonology and pitch curve, which live in
# svs_core in the program; svs_core in turn links this library, and naming it
# here lets the linker go round the two again. In the engine's own test there is
# no svs_core and the test lists those sources.
target_link_libraries(svs_sharpvox PUBLIC $<TARGET_NAME_IF_EXISTS:svs_core>)
