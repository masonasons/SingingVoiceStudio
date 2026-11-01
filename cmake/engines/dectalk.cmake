# DECtalk's voices.
#
# On Windows DECtalk is compiled into the program (SVS_DECTALK_STATIC, on to
# begin with): the same sources, defines and include folders as the "Release
# - ENGLISH_US" build of DECtalk.dll (third_party/dectalk, dapi/src/DECtalk
# API.vcxproj), compiled with the program's own GCC, with DECtalk's scale
# retuned as scripts/build_dectalk.ps1 retunes the DLL's. The engine's own
# names are made local to it, so none of them meets another engine's, and
# only the six entry points src/voices/dectalk.cpp calls are left, and the
# four more the engine's test calls. Its US English dictionary is compiled
# in as well (SVS_DECTALK_DIC).
#
# Elsewhere, or with SVS_DECTALK_STATIC off, DECtalk is a library of its own,
# loaded when it is first needed: DECtalk.dll from scripts/build_dectalk.ps1,
# libtts.dylib from scripts/build_dectalk_mac.sh, with dtalk_us.dic beside
# it in voices/dectalk.
#
# Included by the root CMakeLists.txt and by tests/engines/dectalk; both set
# SVS_ROOT.
if(WIN32)
  option(SVS_DECTALK_STATIC "compile DECtalk into the program rather than load DECtalk.dll" ON)
else()
  option(SVS_DECTALK_STATIC "compile DECtalk into the program rather than load its library" OFF)
endif()

add_library(svs_dectalk STATIC ${SVS_ROOT}/src/voices/dectalk.cpp)
target_include_directories(svs_dectalk PUBLIC ${SVS_ROOT}/src)
set_target_properties(svs_dectalk PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_dectalk PRIVATE -O2)
target_compile_definitions(svs_dectalk PUBLIC SVS_HAVE_DECTALK=1)
# dectalk.cpp uses the studio's paths, phonology and pitch curve, which live
# in svs_core in the program; svs_core in turn links this library, and naming
# it here lets the linker go round the two again. In the engine's own test
# there is no svs_core and the test lists those sources.
target_link_libraries(svs_dectalk PUBLIC $<TARGET_NAME_IF_EXISTS:svs_core>)

if(NOT SVS_DECTALK_STATIC)
  return()
endif()
if(NOT WIN32)
  message(FATAL_ERROR "SVS_DECTALK_STATIC builds DECtalk's Windows configuration, so it is for Windows only")
endif()

set(DT_SRC ${SVS_ROOT}/third_party/dectalk/src/dapi/src)
set(DT_OUT ${CMAKE_CURRENT_BINARY_DIR}/dectalk)
if(NOT EXISTS ${DT_SRC}/api/ttsapi.c)
  message(FATAL_ERROR "third_party/dectalk is empty: git submodule update --init")
endif()

# DECtalk.dll's sources, as its project lists them
set(DT_UNITS
  api/crypt2 api/decstd97 api/ttsapi
  cmd/cmd_init cmd/cmd_wav cmd/cm_char cmd/cm_cmd cmd/cm_copt cmd/cm_main cmd/cm_pars cmd/cm_phon
  cmd/cm_text cmd/cm_util cmd/par_ambi cmd/par_char cmd/par_dict cmd/par_pars cmd/par_rule
  hlsyn/acxf1c hlsyn/brent hlsyn/circuit hlsyn/frame hlsyn/hlframe hlsyn/inithl hlsyn/llinit
  hlsyn/log10table hlsyn/nasalf1x hlsyn/reson hlsyn/sample hlsyn/sqrttable hlsyn/voice
  kernel/services kernel/usa_init
  lts/loaddict lts/lsa_adju lts/lsa_coni lts/lsa_fr lts/lsa_gr lts/lsa_ir lts/lsa_it lts/lsa_ja
  lts/lsa_rtbi lts/lsa_rule lts/lsa_sl lts/lsa_sp lts/lsa_task lts/lsa_us lts/lsa_util lts/lsw_main
  lts/ls_chari lts/ls_dict lts/ls_homo lts/ls_math lts/ls_proc lts/ls_spel lts/ls_speli lts/ls_suff
  lts/ls_suffi
  nt/dbgwins nt/mmalloc nt/opthread nt/pipe nt/playaud nt/spc
  ph/phinit ph/phlog ph/phprint ph/ph_aloph ph/ph_claus ph/ph_draw ph/ph_drwt0 ph/ph_inton ph/ph_main
  ph/ph_romi ph/ph_setar ph/ph_sort ph/ph_syl ph/ph_syntx ph/ph_task ph/ph_timng ph/ph_vdefi ph/ph_vset
  vtm/playtone vtm/sync vtm/vtm vtm/vtmiont)

# A few files are compiled from a copy with a change made, DECtalk's own
# sources left as they are. svs_dectalk_copy reads `unit` and writes it to
# the build folder with each FROM replaced by its TO, once each.
function(svs_dectalk_copy unit)
  file(READ ${DT_SRC}/${unit} text)
  set(pairs ${ARGN})
  list(LENGTH pairs n)
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last} 2)
    math(EXPR j "${i} + 1")
    list(GET pairs ${i} from)
    list(GET pairs ${j} to)
    string(FIND "${text}" "${from}" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "${unit} is not as svs_dectalk_copy expects: no \"${from}\"")
    endif()
    string(REPLACE "${from}" "${to}" text "${text}")
  endforeach()
  # written only when it changes, so a new configure compiles nothing again
  file(WRITE ${DT_OUT}/next/${unit} "${text}")
  configure_file(${DT_OUT}/next/${unit} ${DT_OUT}/${unit} COPYONLY)
endfunction()

# DECtalk's scale retuned to A = 440 Hz: the values scripts/build_dectalk.ps1
# works out and patches into the DLL, and scripts/build_dectalk_mac.sh writes
# into its copy of ph_romi.c (see either for why they are these).
set(DT_NOTES_ORIGINAL 640 678 718 761 806 854 905 959 1016 1076 1140 1208 1280 1356 1437 1522 1613 1709
                      1810 1918 2032 2152 2280 2416 2560 2712 2874 3044 3226 3418 3620 3836 4064 4304 4560
                      4832 5120)
set(DT_NOTES_TUNED 640 678 718 761 806 853 904 958 1015 1075 1139 1206 1278 1353 1434 1519 1610 1704
                   1804 1913 2028 2146 2274 2407 2554 2704 2860 3026 3212 3411 3601 3812 4049 4267 4540
                   4783 5092)
file(READ ${DT_SRC}/ph/ph_romi.c romi)
string(REGEX MATCHALL "const short notetab\\[\\][ \t\r\n]*=[ \t\r\n]*{[^}]*}" tables "${romi}")
list(LENGTH tables n)
if(NOT n EQUAL 1)
  message(FATAL_ERROR "notetab is in ph_romi.c ${n} times, not once")
endif()
string(REGEX REPLACE "/\\*[^*]*\\*/" "" bare "${tables}")
string(REGEX REPLACE "^[^{]*{" "" bare "${bare}")
string(REGEX MATCHALL "[0-9]+" values "${bare}")
if(NOT values STREQUAL DT_NOTES_ORIGINAL AND NOT values STREQUAL DT_NOTES_TUNED)
  message(FATAL_ERROR "notetab in ph_romi.c is not the table DECtalk came with")
endif()
string(REPLACE ";" ",\n\t" tuned "${DT_NOTES_TUNED}")
svs_dectalk_copy(ph/ph_romi.c "${tables}" "const short notetab[] =\n{\n\t${tuned}\n}")
# Microsoft's compiler takes a cast as somewhere to assign to; GCC needs the
# name DECtalk's other builds assign through (the user dictionary only)
svs_dectalk_copy(lts/ls_dict.c
  "if (!(UDICT_INDEX = (S32 *) mallocLock(4)))" "if (!(UDICT_INDEX_ASSIGN = (S32 *) mallocLock(4)))"
  "if ((UDICT_INDEX = reallocLock(" "if ((UDICT_INDEX_ASSIGN = reallocLock("
  "\tUDICT_INDEX = reallocLock(" "\tUDICT_INDEX_ASSIGN = reallocLock(")
# ls_util.c, which lsa_util.c includes, has two headers declare putseq and
# putstring two ways, which Microsoft's compiler lets pass; it uses neither,
# so the second header's are given other names there
svs_dectalk_copy(lts/lsa_util.c "#include \"ls_util.c\"" "#include \"ls_util.c\"")
svs_dectalk_copy(lts/ls_util.c
  "#include \"ph_def.h\""
  "#define putseq svs_unused_putseq\n#define putstring svs_unused_putstring\n#include \"ph_def.h\"\n#undef putseq\n#undef putstring")

# written for Windows' own file names, which only a compiler running there reads
svs_dectalk_copy(cmd/cmd_wav.c "#include <sys\\types.h>" "#include <sys/types.h>"
  "#include <sys\\stat.h>" "#include <sys/stat.h>")

set(DT_SOURCES)
foreach(u ${DT_UNITS})
  if(EXISTS ${DT_OUT}/${u}.c)
    list(APPEND DT_SOURCES ${DT_OUT}/${u}.c)
  else()
    list(APPEND DT_SOURCES ${DT_SRC}/${u}.c)
  endif()
endforeach()
add_library(svs_dectalk_engine OBJECT ${DT_SOURCES})
target_include_directories(svs_dectalk_engine PRIVATE
  ${DT_SRC}/api ${DT_SRC}/cmd ${DT_SRC}/hlsyn ${DT_SRC}/include ${DT_SRC}/lts ${DT_SRC}/nt ${DT_SRC}/ph
  ${DT_SRC}/protos ${DT_SRC}/vtm ${DT_SRC}/../..)
# the project's own defines; _int64 is __int64 to Microsoft's compiler
target_compile_definitions(svs_dectalk_engine PRIVATE
  WIN32 _WINDOWS USE_CORE_DLL ACNA BLD_DECTALK_DLL ENGLISH_US ENGLISH NDEBUG AMD64 _int64=__int64)
# 1990s C, compiled as such; warnings off, as the code is kept as its authors wrote it
set_target_properties(svs_dectalk_engine PROPERTIES C_STANDARD 90 C_EXTENSIONS ON)
target_compile_options(svs_dectalk_engine PRIVATE -O2 -w -fcommon)

# The engine as one object, its common variables given their place and every
# name but the entry points made its own.
set(DT_KEEP TextToSpeechStartupExFonix TextToSpeechSpeak TextToSpeechSync TextToSpeechOpenInMemory
            TextToSpeechAddBuffer TextToSpeechReturnBuffer
            # for tests/engines/dectalk
            TextToSpeechOpenWaveOutFile TextToSpeechCloseWaveOutFile TextToSpeechOpenLogFile
            TextToSpeechCloseLogFile)
set(DT_KEEP_ARGS)
foreach(s ${DT_KEEP})
  list(APPEND DT_KEEP_ARGS --keep-global-symbol=${s})
endforeach()
set(DT_OBJECT ${DT_OUT}/dectalk_engine.o)
add_custom_command(OUTPUT ${DT_OBJECT}
  COMMAND ${CMAKE_LINKER} -r -d -o ${DT_OUT}/dectalk_all.o $<TARGET_OBJECTS:svs_dectalk_engine>
  COMMAND ${CMAKE_OBJCOPY} ${DT_KEEP_ARGS} ${DT_OUT}/dectalk_all.o ${DT_OBJECT}
  DEPENDS svs_dectalk_engine $<TARGET_OBJECTS:svs_dectalk_engine>
  COMMAND_EXPAND_LISTS
  COMMENT "Linking DECtalk into one object")

# The dictionary, compiled in: SVS_DECTALK_DIC if it is given, else the one
# scripts/build_dectalk.ps1 compiles into voices/dectalk if it is there (as
# it is looked for at each configure), else the one that comes with DECtalk's
# sources, an earlier build of the same word list. It is only for reading
# text, which nothing here does, but DECtalk will not start without it.
set(SVS_DECTALK_DIC "" CACHE FILEPATH "DECtalk's US English dictionary, compiled into the program")
if(SVS_DECTALK_DIC)
  set(DT_DIC ${SVS_DECTALK_DIC})
elseif(EXISTS ${SVS_ROOT}/voices/dectalk/dtalk_us.dic)
  set(DT_DIC ${SVS_ROOT}/voices/dectalk/dtalk_us.dic)
else()
  set(DT_DIC ${SVS_ROOT}/third_party/dectalk/ports/emscripten/fs/dtalk_us.dic)
endif()
message(STATUS "DECtalk's dictionary: ${DT_DIC}")
set(DT_DIC_SOURCE ${DT_OUT}/dtalk_us_dic.c)
add_custom_command(OUTPUT ${DT_DIC_SOURCE}
  COMMAND ${CMAKE_COMMAND} -DIN=${DT_DIC} -DOUT=${DT_DIC_SOURCE} -DNAME=svs_dectalk_dictionary
          -P ${SVS_ROOT}/cmake/embed.cmake
  DEPENDS ${DT_DIC} ${SVS_ROOT}/cmake/embed.cmake
  COMMENT "Compiling DECtalk's dictionary in")

set_source_files_properties(${DT_OBJECT} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
target_sources(svs_dectalk PRIVATE ${DT_OBJECT} ${DT_DIC_SOURCE})
target_compile_definitions(svs_dectalk PUBLIC SVS_DECTALK_STATIC=1)
target_link_libraries(svs_dectalk PUBLIC winmm)
