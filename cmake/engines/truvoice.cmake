# TruVoice: Centigram's text-to-speech system, the voice of Microsoft Agent,
# a descendant of the Speech Plus Prose 2000.
#
# The engine is third_party/opentv, OpenTV, a decompilation of the 1997
# English engine into C that is checked byte for byte against the original. It
# is compiled here as its own 64-bit build compiles it (harness/build.sh): the
# C in src/, then the constant tables in lang/enu/data/engine.tvdata laid out
# by its tools/gen_data.py into an assembly file that puts every byte where
# the original had it, with a label on each table the C reads. That step reads
# the engine's object files with nm to find those tables, so it needs Python 3
# and nm, which the msys2 build has.
#
# English alone: the Spanish engine and British English are left out
# (TVTTS_NO_ES, TVTTS_NO_ENGB), since what the singer uses -- phonemes in,
# frames out, frames in -- is the American engine's. src/port/main.c is the
# library's own command line and is not built.
#
# SVS_TRUVOICE_DIR names another OpenTV checkout to build instead of the
# submodule, for working on the engine and the singer together.
#
# Included by the root CMakeLists.txt and by tests/engines/truvoice; both set
# SVS_ROOT.
set(SVS_TRUVOICE_DIR ${SVS_ROOT}/third_party/opentv CACHE PATH "the OpenTV checkout TruVoice is built from")
set(SVS_TRUVOICE_OUT ${CMAKE_CURRENT_BINARY_DIR}/truvoice)
if(NOT EXISTS ${SVS_TRUVOICE_DIR}/src/engine/stage3.c)
  message(FATAL_ERROR "${SVS_TRUVOICE_DIR} is empty: git submodule update --init")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_language(ASM)

# The engine's structures are written as the offsets the original used
# (src/*.fields), and its tools/gen_struct.py makes the C structs of them.
file(GLOB SVS_TRUVOICE_FIELDS CONFIGURE_DEPENDS ${SVS_TRUVOICE_DIR}/src/*.fields)
set(SVS_TRUVOICE_STRUCTS)
foreach(f ${SVS_TRUVOICE_FIELDS})
  get_filename_component(n ${f} NAME_WE)
  add_custom_command(
    OUTPUT ${SVS_TRUVOICE_OUT}/gen/${n}_struct.h
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SVS_TRUVOICE_OUT}/gen
    COMMAND ${Python3_EXECUTABLE} ${SVS_TRUVOICE_DIR}/tools/gen_struct.py ${f} ${SVS_TRUVOICE_OUT}/gen/${n}_struct.h
    DEPENDS ${f} ${SVS_TRUVOICE_DIR}/tools/gen_struct.py
    VERBATIM)
  list(APPEND SVS_TRUVOICE_STRUCTS ${SVS_TRUVOICE_OUT}/gen/${n}_struct.h)
endforeach()

# CONFIGURE_DEPENDS: a submodule update that adds a file is noticed
file(GLOB SVS_TRUVOICE_SOURCES CONFIGURE_DEPENDS ${SVS_TRUVOICE_DIR}/src/engine/*.c ${SVS_TRUVOICE_DIR}/src/port/*.c)
list(REMOVE_ITEM SVS_TRUVOICE_SOURCES ${SVS_TRUVOICE_DIR}/src/port/main.c)
add_library(svs_truvoice_engine OBJECT ${SVS_TRUVOICE_SOURCES} ${SVS_TRUVOICE_STRUCTS})
target_include_directories(svs_truvoice_engine PRIVATE
  ${SVS_TRUVOICE_DIR}/src ${SVS_TRUVOICE_DIR}/include ${SVS_TRUVOICE_DIR}/lang/eng/data
  ${SVS_TRUVOICE_OUT}/gen)
set_target_properties(svs_truvoice_engine PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
# as the engine's own build has them: the original's arithmetic wraps, and
# its tables are read through pointers of other types. Its warnings are its own.
target_compile_options(svs_truvoice_engine PRIVATE -O2 -fwrapv -fno-strict-aliasing -w)
target_compile_definitions(svs_truvoice_engine PRIVATE TVTTS_NO_ES TVTTS_NO_ENGB)

# the tables, laid out from the objects that read them
add_custom_command(
  OUTPUT ${SVS_TRUVOICE_OUT}/tvdata.s
  COMMAND ${CMAKE_COMMAND} -E make_directory ${SVS_TRUVOICE_OUT}
  COMMAND ${Python3_EXECUTABLE} ${SVS_TRUVOICE_DIR}/tools/gen_data.py
          ${SVS_TRUVOICE_DIR}/lang/enu/data/engine.tvdata ${SVS_TRUVOICE_DIR}/src
          ${SVS_TRUVOICE_OUT}/tvdata.s "$<TARGET_OBJECTS:svs_truvoice_engine>"
  DEPENDS svs_truvoice_engine "$<TARGET_OBJECTS:svs_truvoice_engine>"
          ${SVS_TRUVOICE_DIR}/tools/gen_data.py ${SVS_TRUVOICE_DIR}/lang/enu/data/engine.tvdata
  COMMENT "Laying out TruVoice's tables"
  COMMAND_EXPAND_LISTS VERBATIM)

# the engine, on its own: its library interface, include/tvtts.h
add_library(svs_opentv STATIC $<TARGET_OBJECTS:svs_truvoice_engine> ${SVS_TRUVOICE_OUT}/tvdata.s)
target_include_directories(svs_opentv PUBLIC ${SVS_TRUVOICE_DIR}/include)
set_target_properties(svs_opentv PROPERTIES LINKER_LANGUAGE C)

# the singer, src/voices/truvoice.cpp, over it
add_library(svs_truvoice STATIC ${SVS_ROOT}/src/voices/truvoice.cpp)
target_include_directories(svs_truvoice PUBLIC ${SVS_ROOT}/src)
set_target_properties(svs_truvoice PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_compile_options(svs_truvoice PRIVATE -O2)
target_compile_definitions(svs_truvoice PUBLIC SVS_HAVE_TRUVOICE=1)
# truvoice.cpp uses the studio's phonology and pitch curve, which live in
# svs_core in the program; svs_core in turn links this library, and naming it
# here lets the linker go round the two again. In the engine's own test there
# is no svs_core and the test lists those sources. The sung phrase is retuned
# to its curve (macintalk_dsp.cpp).
include(${SVS_ROOT}/cmake/engines/dsp.cmake)
target_link_libraries(svs_truvoice PUBLIC svs_opentv svs_dsp $<TARGET_NAME_IF_EXISTS:svs_core> m)
