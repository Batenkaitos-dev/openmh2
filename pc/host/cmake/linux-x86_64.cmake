# mh2, the host, on Linux: 64-bit, with the core's compiler (pc/cmake/linux-i686.cmake),
# so that the two processes use the same clang. Only the target and the flags differ.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/linux-i686.cmake")
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
string(REGEX REPLACE "clang(-[0-9.]+)?$" "clang++\\1" CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}")
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_C_FLAGS_INIT "")
set(CMAKE_EXE_LINKER_FLAGS_INIT "")
unset(CMAKE_POSITION_INDEPENDENT_CODE)
