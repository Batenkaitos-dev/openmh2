# mh2-core on Linux: clang, 32-bit x86 (gcc-multilib provides the 32-bit libc).
# The clang is the pinned LLVM release (LLVM_VERSION in tools/versions.txt; tools/setup.sh installs it
# in tools/deps/linux-x64/llvm), the same version as llvm-mingw's on Windows. Configure with
# -DMH2_LLVM=<its directory>, or set $MH2_LLVM, if it isn't the repository's own.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
if(NOT MH2_LLVM AND DEFINED ENV{MH2_LLVM})
  set(MH2_LLVM "$ENV{MH2_LLVM}")
endif()
if(NOT MH2_LLVM)
  get_filename_component(MH2_LLVM "${CMAKE_CURRENT_LIST_DIR}/../../tools/deps/linux-x64/llvm" ABSOLUTE)
endif()
if(NOT EXISTS "${MH2_LLVM}/bin/clang")
  message(FATAL_ERROR "no clang in ${MH2_LLVM}/bin: run tools/setup.sh (it installs the pinned LLVM), or give -DMH2_LLVM=DIR")
endif()
# try_compile projects read this file again: they get the same directory
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MH2_LLVM)
set(CMAKE_C_COMPILER "${MH2_LLVM}/bin/clang")
set(CMAKE_C_COMPILER_TARGET i686-linux-gnu)
# -fno-pie: the image is linked -no-pie at a fixed address (below), so position-independent code would
# only cost a register: clang compiles for PIE by default on Linux, which keeps EBX for the GOT in every
# function. Without it the code is that of the Windows target, which has no PIC.
set(CMAKE_C_FLAGS_INIT "-m32 -fno-pie")
# The image at 0x08000000, as the Windows executable's (mingw-i686.cmake): 96 MB of image (the RAM block
# and the low pool are its .bss), so 32 MB of room below 0x10000000. Not lower:
# at 0x400000 the core's own addresses overlapped the PS2's (0x00100000-0x02000000), and an integer
# in that range in untyped memory read as a pointer in the RAM images and traces.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie -Wl,-Ttext-segment=0x08000000")
# a fixed layout: the image below 0x10000000
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(MH2_PLATFORM linux)
