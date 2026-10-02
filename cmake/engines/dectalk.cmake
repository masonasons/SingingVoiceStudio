# DECtalk's ten voices.
#
# DECtalk itself builds only with Microsoft's compiler, so it is not built
# here: scripts/build_dectalk.ps1 makes DECtalk.dll and the dictionary
# dtalk_us.dic and puts them in voices\dectalk, and src/voices/dectalk.cpp
# loads the DLL when it is first needed. This library is only that file.
#
# Included by the root CMakeLists.txt and by tests/engines/dectalk; both set
# SVS_ROOT.
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
