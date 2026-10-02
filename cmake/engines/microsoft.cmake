# Microsoft Sam, Mike and Mary, and the SAPI 4 voice modes.
#
# The engine is the C99 reconstruction of the Windows XP text-to-speech engine in
# third_party/ms-sam-mike-mary-decomp, compiled as it is. src/voices/sam_sing.c includes the
# engine's sam_front.c to reach its static front-end functions, so sam_front.c is compiled there
# and must not be listed again. sam_tts.c and the command-line programs are not needed.
#
# Included by the root CMakeLists.txt and by tests/engines/microsoft; both set SVS_ROOT.
set(SVS_SAM_DIR ${SVS_ROOT}/third_party/ms-sam-mike-mary-decomp/src)

# One change to the vocoder, made here so the reconstruction stays as its authors wrote it. The
# engine takes each pitch period as a whole number of samples, truncated: sr / F rounded down.
# That is faithful -- it is what the real engine does, and speech never noticed -- but a sung note
# comes out sharp by the fraction it drops, 6 cents on an average note and over 20 on a high one
# (Mary's E4: 66.8 samples become 66). When singing (sam_params.smooth, which only the singer
# sets) the fraction is carried into the next period instead, so periods alternate 66, 67, 67 and
# the pitch averages out exact. Speech is untouched and stays sample for sample.
file(READ ${SVS_SAM_DIR}/sam.c SVS_SAM_VOCODER)
# A function, not a macro, and quoted arguments throughout: the C is full of the semicolons
# CMake otherwise splits lists on.
function(svs_sam_patch from to)
  string(FIND "${SVS_SAM_VOCODER}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "sam.c has changed: cannot find the line to patch: ${from}")
  endif()
  string(REPLACE "${from}" "${to}" patched "${SVS_SAM_VOCODER}")
  set(SVS_SAM_VOCODER "${patched}" PARENT_SCOPE)
endfunction()
# The fraction is per thread and starts again with every synthesiser -- every phrase gets a
# fresh one -- so the same phrase always sings the same samples.
svs_sam_patch("sam_synth *sam_synth_new(const sam_voice *v, const sam_params *p)"
              "static __thread double svs_period_carry = 0.0;\nsam_synth *sam_synth_new(const sam_voice *v, const sam_params *p)")
# The singer's vibrato depth can move -- the mod wheel -- so when the singer hands over a
# function for it, each pitch period asks it for the depth at that moment instead of using the
# one fixed depth the engine's singing mode has. svs_vib_t0 is where the current sound starts in
# the phrase. Per thread, like the carry; unset, nothing changes.
svs_sam_patch("sam_synth *sam_synth_new(const sam_voice *v, const sam_params *p)"
              "__thread double (*svs_vib_depth_at)(void *, double) = 0;
__thread void *svs_vib_user = 0;
__thread double svs_vib_t0 = 0.0;
sam_synth *sam_synth_new(const sam_voice *v, const sam_params *p)")
svs_sam_patch("F *= pow(2.0, (double)s->p.sing_vibrato * fade * sin(s->svib_phase) / 1200.0);"
              "F *= pow(2.0, (svs_vib_depth_at ? svs_vib_depth_at(svs_vib_user, svs_vib_t0 + (double)tsamp / sr) : (double)s->p.sing_vibrato) * fade * sin(s->svib_phase) / 1200.0);")
svs_sam_patch("    sam_synth *s = calloc(1, sizeof *s);"
              "    sam_synth *s = calloc(1, sizeof *s);\n    svs_period_carry = 0.0;")
svs_sam_patch("int f = 0, rep = 0, tsamp = 0, e = 0;"
              "int f = 0, rep = 0, tsamp = 0, e = 0;\n    double svs_want = 0.0;")
svs_sam_patch("dst_n = (int)(sr / F);"
              "svs_want = sr / F + svs_period_carry;\n            dst_n = s->p.smooth ? (int)(svs_want + 0.5) : (int)(sr / F);")
svs_sam_patch("            len[e] = dst_n;"
              "            len[e] = dst_n;\n            if (s->p.smooth && !unvoiced) svs_period_carry = svs_want - dst_n;")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/sam_singing.c "${SVS_SAM_VOCODER}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_SAM_DIR}/sam.c)

set(SVS_SAM_ENGINE_SOURCES
  ${CMAKE_CURRENT_BINARY_DIR}/sam_singing.c
  ${SVS_SAM_DIR}/sam_lex.c
  ${SVS_SAM_DIR}/sam_morph.c
  ${SVS_SAM_DIR}/sam_pos.c
  ${SVS_SAM_DIR}/sam_norm.c
  ${SVS_SAM_DIR}/sam4fx.c
)

add_library(svs_microsoft STATIC
  ${SVS_ROOT}/src/voices/microsoft.cpp
  ${SVS_ROOT}/src/voices/sam_sing.c
  ${SVS_SAM_ENGINE_SOURCES}
)
target_include_directories(svs_microsoft PUBLIC ${SVS_ROOT}/src ${SVS_SAM_DIR})
set_target_properties(svs_microsoft PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_microsoft PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=c99 -O2>)
# The reconstruction is kept as its authors wrote it; its warnings are theirs.
set_source_files_properties(${SVS_SAM_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(svs_microsoft PUBLIC SVS_HAVE_MICROSOFT=1)
# microsoft.cpp uses the studio's paths, phonology and pitch curve, which live in svs_core in
# the program; svs_core in turn links this library, and naming it here lets the linker go round
# the two again. In the engine's own test there is no svs_core and the test lists those sources.
target_link_libraries(svs_microsoft PUBLIC $<TARGET_NAME_IF_EXISTS:svs_core> m)
