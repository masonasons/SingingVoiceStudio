# Eloquence: Eloquent Technology's formant synthesiser, later IBM's ViaVoice
# TTS, the voice of JAWS and of NVDA's Eloquence add-on.
#
# The engine is third_party/openevv (OpenEVV), a reimplementation in C that is
# checked sample for sample against IBM's own binary, compiled as its own
# Makefile compiles it with its rules as bytecode (RULES=bytecode), US English
# only. Three of its files are written by its Python tools out of the rules and
# dictionary held as text: the rules as bytecode, the header that numbers them
# and a shim under each rule's name, and the built-in dictionary. The tools
# write beside what they read, so they are run on a copy in the build folder and
# the submodule is left as it is; the build therefore needs Python 3. The list
# of languages the Makefile writes is written here.
#
# Three changes, made here so the engine stays as its authors wrote it:
#   - each parameter frame on its way to the synthesiser goes to a hook the
#     singer sets (svs_evv_frame_hook) instead, svs_evv_synth_frame
#     synthesises one, and svs_evv_restart starts the synthesiser again as a
#     new instance's starts, its noise from the same seed;
#   - the frame generator reports the stretch of time each of its runs is asked
#     for (svs_evv_run_hook), one run to a phoneme, which is how
#     src/voices/eloquence.cpp knows where each phoneme starts;
#   - its library front (lib/eci_api.c) does not start the engine as the program
#     loads, as a DLL's would; the singer starts it when it is first wanted.
#
# Included by the root CMakeLists.txt and by tests/engines/eloquence; both set
# SVS_ROOT.
set(SVS_EVV_DIR ${SVS_ROOT}/third_party/openevv)
set(SVS_EVV_OUT ${CMAKE_CURRENT_BINARY_DIR}/eloquence)
if(NOT EXISTS ${SVS_EVV_DIR}/src/klatt/klatt_run.c)
  message(FATAL_ERROR "third_party/openevv is empty: git submodule update --init")
endif()
if(APPLE)
  message(FATAL_ERROR "Eloquence keeps everything it points at below two gigabytes, which a Mac's "
                      "64-bit programs cannot have: leave eloquence out of SVS_ENGINES")
endif()
find_package(Python3 COMPONENTS Interpreter)
if(NOT Python3_Interpreter_FOUND)
  message(FATAL_ERROR "Eloquence's build needs Python 3 (msys2: pacman -S mingw-w64-x86_64-python), "
                      "or leave eloquence out of SVS_ENGINES")
endif()

# -- the generated files, from a copy of what the tools read ----------------
set(SVS_EVV_TREE ${SVS_EVV_OUT}/tree)
set(SVS_EVV_LANG ${SVS_EVV_TREE}/lang/enus)
set(SVS_EVV_GENERATED ${SVS_EVV_LANG}/delta_rules_enus.c ${SVS_EVV_LANG}/delta_rules_enus.h
                      ${SVS_EVV_LANG}/delta_rules_shim_enus.c ${SVS_EVV_LANG}/delta_builtin_enus.c)
file(GLOB_RECURSE SVS_EVV_INPUTS LIST_DIRECTORIES false
     ${SVS_EVV_DIR}/lang/enus/* ${SVS_EVV_DIR}/tools/*.py ${SVS_EVV_DIR}/src/*.c ${SVS_EVV_DIR}/src/*.h)
list(FILTER SVS_EVV_INPUTS EXCLUDE REGEX "/delta_rules_(enus|shim_enus|c[0-9]+_enus)\\.[ch]$|/delta_builtin_enus\\.c$")
set(SVS_EVV_STAMP ${SVS_EVV_OUT}/inputs.txt)
string(SHA256 SVS_EVV_HASH "${SVS_EVV_INPUTS}")
foreach(f ${SVS_EVV_INPUTS})
  file(SHA256 ${f} h)
  string(SHA256 SVS_EVV_HASH "${SVS_EVV_HASH}${h}")
endforeach()
set(SVS_EVV_STALE TRUE)
if(EXISTS ${SVS_EVV_STAMP})
  file(READ ${SVS_EVV_STAMP} SVS_EVV_WAS)
  if(SVS_EVV_WAS STREQUAL SVS_EVV_HASH)
    set(SVS_EVV_STALE FALSE)
  endif()
endif()
foreach(f ${SVS_EVV_GENERATED})
  if(NOT EXISTS ${f})
    set(SVS_EVV_STALE TRUE)
  endif()
endforeach()
if(SVS_EVV_STALE)
  message(STATUS "Writing Eloquence's rules and dictionary out of their text")
  file(REMOVE_RECURSE ${SVS_EVV_TREE})
  # the tools find the tree by the Makefile above them
  file(MAKE_DIRECTORY ${SVS_EVV_TREE}/lang)
  file(WRITE ${SVS_EVV_TREE}/Makefile "")
  file(COPY ${SVS_EVV_DIR}/tools ${SVS_EVV_DIR}/src DESTINATION ${SVS_EVV_TREE}
       PATTERN __pycache__ EXCLUDE)
  file(COPY ${SVS_EVV_DIR}/lang/enus DESTINATION ${SVS_EVV_TREE}/lang
       REGEX "/delta_rules_(enus|shim_enus|c[0-9]+_enus)\\.[ch]$|/delta_builtin_enus\\.c$" EXCLUDE)
  set(ENV{EVV_NOTATION_LANG} enus)
  foreach(step "tools/rules/letters.py;write;enus" "tools/rules/notation.py;build"
               "tools/module/builtindict.py;write;lang/enus")
    execute_process(COMMAND ${Python3_EXECUTABLE} ${step} WORKING_DIRECTORY ${SVS_EVV_TREE}
                    RESULT_VARIABLE rc OUTPUT_QUIET)
    if(NOT rc EQUAL 0)
      list(GET step 0 tool)
      message(FATAL_ERROR "Eloquence's ${tool} failed")
    endif()
  endforeach()
  file(WRITE ${SVS_EVV_STAMP} "${SVS_EVV_HASH}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_EVV_INPUTS})

# what the Makefile writes as build/delta_langs_enus.c
file(WRITE ${SVS_EVV_OUT}/next/delta_langs_enus.c
"/* Written by cmake/engines/eloquence.cmake: the one language this build has. */

#include \"delta_lang.h\"

extern delta_language delta_lang_enus;
void delta_lang_bind_enus(void);

const delta_language *const delta_languages[] = {
    &delta_lang_enus,
    0,
};

void delta_lang_bind_all(void)
{
    delta_lang_bind_enus();
}
")

# -- the three changes ------------------------------------------------------
# each file read, its text replaced, and written to the build folder
function(svs_evv_patch from to)
  string(FIND "${SVS_EVV_TEXT}" "${from}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${SVS_EVV_FILE} has changed: cannot find the text to patch: ${from}")
  endif()
  if(to STREQUAL "<cut>")
    string(SUBSTRING "${SVS_EVV_TEXT}" 0 ${at} text)
  else()
    string(REPLACE "${from}" "${to}" text "${SVS_EVV_TEXT}")
  endif()
  set(SVS_EVV_TEXT "${text}" PARENT_SCOPE)
endfunction()
foreach(SVS_EVV_FILE src/klatt/klatt_run.c src/eci/bridge/eci_arraygen.c lib/eci_api.c)
  file(READ ${SVS_EVV_DIR}/${SVS_EVV_FILE} SVS_EVV_TEXT)
  # as LF, whatever line ends the checkout gave it
  string(REPLACE "\r\n" "\n" SVS_EVV_TEXT "${SVS_EVV_TEXT}")
  if(SVS_EVV_FILE MATCHES "klatt_run")
    svs_evv_patch("int klattSynthFrame(DeltaThis *d, const int32_t *frame)\n{\n    DeltaLang *lang = DT_LANG(d);\n"
      "int (*svs_evv_frame_hook)(void *d, const int32_t *frame);\n\nint svs_evv_synth_frame(void *dv, const int32_t *frame)\n{\n    DeltaThis *d = dv;\n    DeltaLang *lang = DT_LANG(d);\n")
    svs_evv_patch("    return KlattSynth(DL_KLATT(lang), frame);\n}\n"
      "    return KlattSynth(DL_KLATT(lang), frame);\n}\n\nint klattSynthFrame(DeltaThis *d, const int32_t *frame)\n{\n    if (svs_evv_frame_hook)\n        return svs_evv_frame_hook(d, frame);\n    return svs_evv_synth_frame(d, frame);\n}\n\nvoid svs_evv_restart(void *dv)\n{\n    DeltaThis *d = dv;\n    DeltaLang *lang = DT_LANG(d);\n    klatt_state *k = DL_KLATT(lang);\n\n    KlattSetConstParms(k, k->cp);\n    KlattOpen(k);\n    if (DL_WIDE(lang)) {\n        klatt_wide_close(DL_WIDE(lang));\n        klatt_wide_setup(DL_WIDE(lang), k->cp);\n        klatt_wide_open(DL_WIDE(lang));\n    }\n}\n")
  elseif(SVS_EVV_FILE MATCHES "arraygen")
    svs_evv_patch("static FILE *array_tap;"
      "void (*svs_evv_run_hook)(int32_t from, int32_t to);\n\nstatic FILE *array_tap;")
    svs_evv_patch("    (void)unused;\n"
      "    (void)unused;\n\n    if (svs_evv_run_hook)\n        svs_evv_run_hook(from, to);\n")
  else()
    svs_evv_patch("#if defined(_WIN32)\n\nint __stdcall DllMain" "<cut>")
  endif()
  get_filename_component(name ${SVS_EVV_FILE} NAME)
  file(WRITE ${SVS_EVV_OUT}/next/${name} "${SVS_EVV_TEXT}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SVS_EVV_DIR}/${SVS_EVV_FILE})
endforeach()
foreach(name delta_langs_enus.c klatt_run.c eci_arraygen.c eci_api.c)
  configure_file(${SVS_EVV_OUT}/next/${name} ${SVS_EVV_OUT}/${name} COPYONLY)
endforeach()

# -- the engine -------------------------------------------------------------
# every directory under src, as its Makefile finds them, and the porting layer
# for this system
file(GLOB SVS_EVV_FOUND LIST_DIRECTORIES true ${SVS_EVV_DIR}/src/* ${SVS_EVV_DIR}/src/*/*)
set(SVS_EVV_SRCDIRS)
foreach(d ${SVS_EVV_FOUND})
  if(IS_DIRECTORY ${d})
    list(APPEND SVS_EVV_SRCDIRS ${d})
  endif()
endforeach()
set(SVS_EVV_SOURCES)
foreach(d ${SVS_EVV_DIR}/src ${SVS_EVV_SRCDIRS})
  file(GLOB c ${d}/*.c)
  list(APPEND SVS_EVV_SOURCES ${c})
endforeach()
if(WIN32)
  list(REMOVE_ITEM SVS_EVV_SOURCES ${SVS_EVV_DIR}/src/port/port_posix.c)
else()
  list(REMOVE_ITEM SVS_EVV_SOURCES ${SVS_EVV_DIR}/src/port/port_win32.c)
endif()
list(REMOVE_ITEM SVS_EVV_SOURCES ${SVS_EVV_DIR}/src/klatt/klatt_run.c ${SVS_EVV_DIR}/src/eci/bridge/eci_arraygen.c)
# the language as the copy holds it, with its rules as bytecode only
file(GLOB SVS_EVV_LANG_SOURCES ${SVS_EVV_DIR}/lang/enus/*.c)
list(FILTER SVS_EVV_LANG_SOURCES EXCLUDE REGEX "/delta_rules_[^/]*\\.c$|/delta_builtin_enus\\.c$")
list(TRANSFORM SVS_EVV_LANG_SOURCES REPLACE "^${SVS_EVV_DIR}/lang/enus" "${SVS_EVV_LANG}")
list(APPEND SVS_EVV_LANG_SOURCES ${SVS_EVV_GENERATED} ${SVS_EVV_LANG}/delta_rules_none_enus.c)
list(FILTER SVS_EVV_LANG_SOURCES EXCLUDE REGEX "\\.h$")

add_library(svs_eloquence_engine OBJECT ${SVS_EVV_SOURCES} ${SVS_EVV_LANG_SOURCES}
  ${SVS_EVV_OUT}/delta_langs_enus.c ${SVS_EVV_OUT}/klatt_run.c ${SVS_EVV_OUT}/eci_arraygen.c
  ${SVS_EVV_OUT}/eci_api.c)
target_include_directories(svs_eloquence_engine PRIVATE
  ${SVS_EVV_DIR}/include ${SVS_EVV_DIR}/src ${SVS_EVV_SRCDIRS} ${SVS_EVV_LANG})
# as its Makefile compiles it: its address arena on, which keeps what it points
# at below four gigabytes, and every warning but the ones it makes errors off
set_target_properties(svs_eloquence_engine PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(svs_eloquence_engine PRIVATE -O2 -fno-strict-aliasing -w
  -Werror=implicit-function-declaration -Werror=int-conversion -Werror=incompatible-pointer-types)
target_compile_definitions(svs_eloquence_engine PRIVATE EVV_ARENA=1 ECI_STATIC)

add_library(svs_eloquence STATIC ${SVS_ROOT}/src/voices/eloquence.cpp $<TARGET_OBJECTS:svs_eloquence_engine>)
target_include_directories(svs_eloquence PUBLIC ${SVS_ROOT}/src PRIVATE ${SVS_EVV_DIR}/include)
set_target_properties(svs_eloquence PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_eloquence PRIVATE -O2)
target_compile_definitions(svs_eloquence PUBLIC SVS_HAVE_ELOQUENCE=1 PRIVATE ECI_STATIC)
# eloquence.cpp uses the studio's phonology and pitch curve, which live in
# svs_core in the program; svs_core in turn links this library, and naming it
# here lets the linker go round the two again. In the engine's own test there is
# no svs_core and the test lists those sources.
find_package(Threads REQUIRED)
# The sung phrase is retuned to its curve (macintalk_dsp.cpp).
include(${SVS_ROOT}/cmake/engines/dsp.cmake)
target_link_libraries(svs_eloquence PUBLIC svs_dsp $<TARGET_NAME_IF_EXISTS:svs_core> Threads::Threads m)
