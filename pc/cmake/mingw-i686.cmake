# mh2-core on Windows: llvm-mingw's i686 target (tools/deps/windows-x64/llvm-mingw).
# Configure with -DMH2_LLVM_MINGW=<its directory> if it isn't the repository's own.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR i686)
if(NOT MH2_LLVM_MINGW AND DEFINED ENV{MH2_LLVM_MINGW})
  file(TO_CMAKE_PATH "$ENV{MH2_LLVM_MINGW}" MH2_LLVM_MINGW)
endif()
if(NOT MH2_LLVM_MINGW)
  get_filename_component(MH2_LLVM_MINGW "${CMAKE_CURRENT_LIST_DIR}/../../tools/deps/windows-x64/llvm-mingw" ABSOLUTE)
endif()
# try_compile projects read this file again: they get the same directory
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MH2_LLVM_MINGW)
set(CMAKE_C_COMPILER "${MH2_LLVM_MINGW}/bin/i686-w64-mingw32-clang.exe")
set(CMAKE_C_COMPILER_TARGET i686-w64-mingw32)
set(CMAKE_RC_COMPILER "${MH2_LLVM_MINGW}/bin/i686-w64-mingw32-windres.exe")
# The image at 0x08000000, fixed (no relocation), as on Linux (linux-i686.cmake): below 0x10000000 with
# 32 MB of room, and above the PS2's addresses. Sections start at multiples of 0x10000:
# the blocks of the PS2's data are 64 KiB aligned by it (pc/core/pal_align.h).
# lld warns "/align specified without /driver; image may not run" for any /align (rule 7 allows no warning).
# llvm-mingw's ld.lld hands an argument that starts with "/" to its COFF driver as it is, so -Wl,/driver is
# lld-link's /driver, the flag the warning asks for. Linked twice from the same objects (lld 23.1.1), with and
# without it, mh2-core.exe differs in two bytes only (the PE header and the debug directory timestamps).
# --no-insert-timestamp fixes both timestamps at 0, so two links of the same objects give the same bytes (the release
# kit's core equals build.py --release's, byte for byte).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -Wl,--image-base=0x08000000 -Wl,--disable-dynamicbase -Wl,--disable-high-entropy-va -Wl,--section-alignment=0x10000 -Wl,/driver -Wl,--no-insert-timestamp")
set(MH2_PLATFORM windows)
