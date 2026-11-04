# The host (mh2) on Linux: clang and clang++, 64-bit x86, from the pinned LLVM release (LLVM_VERSION in
# tools/versions.txt; tools/setup.sh installs it in tools/deps/linux-x64/llvm), the same version as
# mh2-core's and as llvm-mingw's on Windows. -DMH2_LLVM=<its directory>, or $MH2_LLVM, for another copy.
# The C++ library is the system's libstdc++, the linker the system's ld, as for mh2-core.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT MH2_LLVM AND DEFINED ENV{MH2_LLVM})
  set(MH2_LLVM "$ENV{MH2_LLVM}")
endif()
if(NOT MH2_LLVM)
  get_filename_component(MH2_LLVM "${CMAKE_CURRENT_LIST_DIR}/../../tools/deps/linux-x64/llvm" ABSOLUTE)
endif()
if(NOT EXISTS "${MH2_LLVM}/bin/clang")
  message(FATAL_ERROR "no clang in ${MH2_LLVM}/bin: run tools/setup.sh (it installs the pinned LLVM), or give -DMH2_LLVM=DIR")
endif()
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MH2_LLVM)
set(CMAKE_C_COMPILER "${MH2_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "${MH2_LLVM}/bin/clang++")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(MH2_PLATFORM linux)
