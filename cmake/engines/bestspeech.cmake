# BeSTspeech: Berkeley Speech Technologies' diphone synthesiser, the engine of
# HumanWare's Keynote GOLD.
#
# The engine is third_party/openbst, a reimplementation in C that is checked
# sample for sample against the original binaries, compiled as it is with the
# tables of its 1995 English build, the one the program sings with. Its index of
# all twenty builds' tables is left out with the other nineteen, and
# src/voices/bestspeech_port.c answers for it.
#
# One change to the frame generator, made here so the engine stays as its
# authors wrote it: each frame it makes is reported, with the number of diphone
# segments taken so far, to a hook the singer sets (svs_bst_frame_hook), which
# is how src/voices/bestspeech.cpp knows which sound a frame belongs to.
#
# Included by the root CMakeLists.txt and by tests/engines/bestspeech; both set
# SVS_ROOT.
set(SVS_BST_DIR ${SVS_ROOT}/third_party/openbst)
if(NOT EXISTS ${SVS_BST_DIR}/src/frames.c)
  message(FATAL_ERROR "third_party/openbst is empty: git submodule update --init")
endif()

file(READ ${SVS_BST_DIR}/src/frames.c SVS_BST_FRAMES)
# as LF, whatever line ends the checkout gave it
string(REPLACE "\r\n" "\n" SVS_BST_FRAMES "${SVS_BST_FRAMES}")
function(svs_bst_patch from to)
  string(FIND "${SVS_BST_FRAMES}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "frames.c has changed: cannot find the line to patch: ${from}")
  endif()
  string(REPLACE "${from}" "${to}" patched "${SVS_BST_FRAMES}")
  set(SVS_BST_FRAMES "${patched}" PARENT_SCOPE)
endfunction()
svs_bst_patch("#include \"bst_frames.h\""
              "#include \"bst_frames.h\"\nvoid (*svs_bst_frame_hook)(int frame, int segment);")
svs_bst_patch("    g->nout++;\n}"
              "    if (svs_bst_frame_hook) svs_bst_frame_hook(g->nout, g->segrd);\n    g->nout++;\n}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/bestspeech/next/frames.c "${SVS_BST_FRAMES}")
configure_file(${CMAKE_CURRENT_BINARY_DIR}/bestspeech/next/frames.c
               ${CMAKE_CURRENT_BINARY_DIR}/bestspeech/frames.c COPYONLY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_BST_DIR}/src/frames.c)

set(SVS_BST_ENGINE_SOURCES)
foreach(u text dict lts cursors wordstream stress assemble tokenise translit except numbers phrules
          accents pairs contour diphone interp synth speak)
  list(APPEND SVS_BST_ENGINE_SOURCES ${SVS_BST_DIR}/src/${u}.c)
endforeach()
list(APPEND SVS_BST_ENGINE_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/bestspeech/frames.c
                                   ${SVS_BST_DIR}/src/data/1995.c)

add_library(svs_bestspeech STATIC
  ${SVS_ROOT}/src/voices/bestspeech.cpp
  ${SVS_ROOT}/src/voices/bestspeech_port.c
  ${SVS_BST_ENGINE_SOURCES}
)
target_include_directories(svs_bestspeech PUBLIC ${SVS_ROOT}/src PRIVATE ${SVS_BST_DIR}/include)
set_target_properties(svs_bestspeech PROPERTIES
  C_STANDARD 11 C_EXTENSIONS ON
  CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_bestspeech PRIVATE -O2)
# The engine is kept as its authors wrote it; its warnings are theirs.
set_source_files_properties(${SVS_BST_ENGINE_SOURCES} PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(svs_bestspeech PUBLIC SVS_HAVE_BESTSPEECH=1)
# bestspeech.cpp uses the studio's phonology and pitch curve, which live in
# svs_core in the program; svs_core in turn links this library, and naming it
# here lets the linker go round the two again. In the engine's own test there is
# no svs_core and the test lists those sources.
target_link_libraries(svs_bestspeech PUBLIC $<TARGET_NAME_IF_EXISTS:svs_core> m)
