# The IPC's tests in the core's build (32-bit): the protocol between a host side and the core's side
# (core_ipc.h) in one process, and mh2-fake-core, a stand-in for mh2-core that draws a test picture
# through the IPC, for the host's own checks (pc/host).
set(_ipc_core "${MH2_ROOT}/pc/core/ipc/host_ipc.c" "${MH2_ROOT}/pc/ipc/mh2ipc_os_${MH2_PLATFORM}.c"
              "${MH2_ROOT}/pc/core/host_${MH2_PLATFORM}.c")
set(_ipc_inc "${MH2_ROOT}/pc/core" "${MH2_ROOT}/pc")

add_executable(mh2_ipc_test ipc_test.c ${_ipc_core})
target_include_directories(mh2_ipc_test PRIVATE ${_ipc_inc})
target_compile_options(mh2_ipc_test PRIVATE ${MH2_HOST_FLAGS} -Wall -Wextra -Werror)
if(MH2_PLATFORM STREQUAL "linux")
  target_link_libraries(mh2_ipc_test PRIVATE -pthread)
endif()
mh2_add_exe_test(ipc_protocol mh2_ipc_test)

add_executable(mh2-fake-core fake_core.c ${_ipc_core})
target_include_directories(mh2-fake-core PRIVATE ${_ipc_inc})
target_compile_options(mh2-fake-core PRIVATE ${MH2_HOST_FLAGS} -Wall -Wextra -Werror)
if(MH2_PLATFORM STREQUAL "linux")
  target_link_libraries(mh2-fake-core PRIVATE m)
endif()
set_target_properties(mh2-fake-core PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/core")

# The movies' core side (ipc_movie.c) against stand-ins for io, the kernel's events and the IPC.
add_executable(mh2_movie_test movie_test.c "${MH2_ROOT}/pc/core/ipc/ipc_movie.c")
target_include_directories(mh2_movie_test PRIVATE "${MH2_ROOT}/include" ${_ipc_inc})
target_compile_options(mh2_movie_test PRIVATE ${MH2_HOST_FLAGS} -Wall -Wextra -Werror)   # all cdecl: the stand-ins use libc
mh2_add_exe_test(ipc_movie mh2_movie_test)
