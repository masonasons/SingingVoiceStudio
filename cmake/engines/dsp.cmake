# The retiming and retuning of speech to a score (src/voices/macintalk_dsp.cpp),
# for a Mac's MacinTalk, which can only speak, and the retuning of the engines
# that sing on whole-sample periods, Eloquence and the Prose 2000. One library,
# whichever of them is built.
#
# Included by the engines' own cmake/engines/<name>.cmake.
if(NOT TARGET svs_dsp)
  add_library(svs_dsp STATIC ${SVS_ROOT}/src/voices/macintalk_dsp.cpp)
  target_include_directories(svs_dsp PUBLIC ${SVS_ROOT}/src)
  set_target_properties(svs_dsp PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
  target_compile_options(svs_dsp PRIVATE -O2)
endif()
