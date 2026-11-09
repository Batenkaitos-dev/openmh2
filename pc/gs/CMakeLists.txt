# mh2gs: the GS backend shared library (paraLLEl-GS + Granite + the bridge).
# The bridge source is CC0 like the rest of the port; the built library links
# paraLLEl-GS and is LGPL-3.0-or-later as a whole. The port's executable only
# loads it at run time.
#
# PARALLEL_GS_DIR points at a paraLLEl-GS checkout with its Granite, volk
# and Vulkan-Headers submodules (tools/setup.* fetch it at the pinned commit).

set(PARALLEL_GS_STANDALONE ON CACHE BOOL "" FORCE)
# The static libraries end up in a shared library (Granite resets
# CMAKE_POSITION_INDEPENDENT_CODE unless its own option is set).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(GRANITE_POSITION_INDEPENDENT ON CACHE BOOL "" FORCE)
add_subdirectory("${PARALLEL_GS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/parallel-gs" EXCLUDE_FROM_ALL)

find_package(Threads REQUIRED)
add_library(mh2gs SHARED mh2gs.cpp mh2gs.h mh2tex.cpp mh2tex.hpp mh2interp.cpp mh2interp.hpp)
target_link_libraries(mh2gs PRIVATE parallel-gs Threads::Threads)
set_target_properties(mh2gs PROPERTIES
    CXX_STANDARD 14
    CXX_VISIBILITY_PRESET hidden
    PREFIX ""
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    # a release library does not read the development switches (the MH2GS_* environment variables: mh2gs.cpp, DEV_ENV)
    target_compile_definitions(mh2gs PRIVATE MH2_RELEASE=1)
if(MINGW)
    # Self-contained DLL: no libstdc++/libgcc DLLs to ship next to it.
    target_link_options(mh2gs PRIVATE -static)
endif()
