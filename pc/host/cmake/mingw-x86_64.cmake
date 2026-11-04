# mh2, the host, on Windows: llvm-mingw's x86_64 target, from the same llvm-mingw as the core
# (pc/cmake/mingw-i686.cmake). Only the target and the flags differ.
include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/mingw-i686.cmake")
set(CMAKE_SYSTEM_PROCESSOR x86_64)
string(REPLACE "i686-w64-mingw32" "x86_64-w64-mingw32" CMAKE_C_COMPILER "${CMAKE_C_COMPILER}")
string(REPLACE "x86_64-w64-mingw32-clang" "x86_64-w64-mingw32-clang++" CMAKE_CXX_COMPILER "${CMAKE_C_COMPILER}")
set(CMAKE_C_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)
string(REPLACE "i686-w64-mingw32" "x86_64-w64-mingw32" CMAKE_RC_COMPILER "${CMAKE_RC_COMPILER}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
