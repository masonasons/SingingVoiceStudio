# FLAC and Ogg Vorbis, for Export audio: Xiph's libFLAC, libogg and libvorbis
# (BSD licences, see NOTICE) at pinned releases, linked statically into both
# executables like everything else.
#
# libogg and libFLAC are built with their own CMake, without their programs,
# tests and documents. libvorbis's looks for an installed libogg, so its
# library sources are compiled here instead, against the libogg above.
#
# Included by the root CMakeLists.txt, which has included FetchContent.

# libogg asks for a CMake older than the oldest one 4.0 still takes
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Floor for old dependencies")
foreach(off BUILD_TESTING INSTALL_DOCS INSTALL_PKG_CONFIG_MODULE INSTALL_CMAKE_PACKAGE_MODULE
            BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_DOCS INSTALL_MANPAGES
            INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG WITH_FORTIFY_SOURCE
            WITH_STACK_PROTECTOR)
  set(${off} OFF CACHE INTERNAL "")
endforeach()

FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
)
FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.4.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ogg flac)
# only what the program links is built, not FLAC's benchmark and helpers
set_property(DIRECTORY ${ogg_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
set_property(DIRECTORY ${flac_SOURCE_DIR} PROPERTY EXCLUDE_FROM_ALL TRUE)
FetchContent_GetProperties(vorbis)
if(NOT vorbis_POPULATED)
  FetchContent_Populate(vorbis)      # the sources only; not its own build
endif()

# libvorbis and libvorbisenc, as its lib/CMakeLists.txt lists them
set(SVS_VORBIS_SOURCES)
foreach(u mdct smallft block envelope window lsp lpc analysis synthesis psy info floor1 floor0
          res0 mapping0 registry codebook sharedbook lookup bitrate vorbisenc)
  list(APPEND SVS_VORBIS_SOURCES ${vorbis_SOURCE_DIR}/lib/${u}.c)
endforeach()
add_library(svs_vorbis STATIC ${SVS_VORBIS_SOURCES})
target_include_directories(svs_vorbis PUBLIC ${vorbis_SOURCE_DIR}/include PRIVATE ${vorbis_SOURCE_DIR}/lib)
target_link_libraries(svs_vorbis PUBLIC ogg)
target_compile_options(svs_vorbis PRIVATE -O2 -w)
if(NOT WIN32)
  target_link_libraries(svs_vorbis PUBLIC m)
endif()
