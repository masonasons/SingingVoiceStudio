# The SSI-263 speech chip (Votrax SC-02): the firmware-free chip core of
# third_party/ssi263-speech (MIT), and the singer that drives it.
#
# Only the chip itself is used -- ssi263.c, its output stage ssi263dsp.c and
# the generated defaults and ROM in ssi263_defaults.h. The C is built as its
# author builds it: C99, -O2, and -ffp-contract=off so that every
# multiply-add rounds twice, as the Python reference it is checked against.
#
# ssi263.h marks every function __declspec(dllexport) on Windows, which in a
# static library would make the program export them. The chip's sources are
# compiled with dllexport defined away (the attribute becomes `unused`,
# which does nothing), so the symbols stay private to the executable.
set(SSI263_DIR ${SVS_ROOT}/third_party/ssi263-speech/src/csrc)

add_library(svs_ssi263 STATIC
  ${SVS_ROOT}/src/voices/ssi263.cpp
  ${SSI263_DIR}/ssi263.c
  ${SSI263_DIR}/ssi263dsp.c
)
target_include_directories(svs_ssi263 PUBLIC ${SVS_ROOT}/src ${SSI263_DIR})
set_target_properties(svs_ssi263 PROPERTIES
  C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_ssi263 PRIVATE
  $<$<COMPILE_LANGUAGE:C>:-O2 -ffp-contract=off -Ddllexport=__unused__>)
