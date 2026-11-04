# The renderer, mh2gs: pc/gs (the bridge, unchanged) with paraLLEl-GS, Granite and volk, in a
# directory of its own so that their warnings are off here only (R7 is about the port's own code: mh2's C
# in pc/host keeps -Wall -Wextra -Werror). Granite asks for CMake 2.8: no compatibility warning.
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
string(APPEND CMAKE_C_FLAGS " -w")
string(APPEND CMAKE_CXX_FLAGS " -w")
add_subdirectory("${MH2_ROOT}/pc/gs" gs-parallel)

# The texture keys and the dump (pc/gs/mh2tex.cpp), without a GPU.
find_package(Threads REQUIRED)
add_executable(host_mh2tex_test "${MH2_ROOT}/pc/gs/tests/mh2tex_test.cpp"
               "${MH2_ROOT}/pc/gs/mh2tex.cpp")
target_link_libraries(host_mh2tex_test PRIVATE parallel-gs Threads::Threads)
target_include_directories(host_mh2tex_test PRIVATE "${MH2_ROOT}/pc/gs")
set_target_properties(host_mh2tex_test PROPERTIES CXX_STANDARD 14)
host_add_exe_test(host_mh2tex host_mh2tex_test "${CMAKE_BINARY_DIR}")

# The renderer's save state (mh2gs_state_get/_set) through the library's own API, with the library's sources
# built into the test (a copy of the exe in a temporary folder, MH2_TEST_COPY, has no mh2gs.dll beside it); it
# needs a GPU, so it is skipped (exit 77) where mh2gs finds none.
add_executable(host_mh2gs_state_test "${MH2_ROOT}/pc/gs/tests/mh2gs_state_test.cpp"
               "${MH2_ROOT}/pc/gs/mh2gs.cpp" "${MH2_ROOT}/pc/gs/mh2tex.cpp")
target_link_libraries(host_mh2gs_state_test PRIVATE parallel-gs Threads::Threads)
target_include_directories(host_mh2gs_state_test PRIVATE "${MH2_ROOT}/pc/gs")
set_target_properties(host_mh2gs_state_test PROPERTIES CXX_STANDARD 14)
if(MINGW)
    target_link_options(host_mh2gs_state_test PRIVATE -static)   # as mh2gs: no libc++ DLLs beside it
endif()
host_add_exe_test(host_mh2gs_state host_mh2gs_state_test)
set_tests_properties(host_mh2gs_state PROPERTIES SKIP_RETURN_CODE 77)
