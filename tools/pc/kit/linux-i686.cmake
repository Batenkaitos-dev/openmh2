# mh2-core on Linux from the release's private kit: the pinned clang with its own 32-bit C library (kit/toolchain/sysroot: glibc's
# headers and static libraries) and runtime (compiler-rt's builtins, libunwind), linked statically with lld, so that the core needs
# nothing of the system's but the kernel. Same image address as pc/cmake/linux-i686.cmake (lld names it --image-base).
# -DMH2_LLVM=<kit/toolchain>.
include("${CMAKE_CURRENT_LIST_DIR}/../../../pc/cmake/linux-i686.cmake")
set(CMAKE_C_FLAGS_INIT "-m32 -fno-pie --sysroot=${MH2_LLVM}/sysroot")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -static -fuse-ld=lld -rtlib=compiler-rt --unwindlib=libunwind -Wl,--image-base=0x08000000 -lmh2f128")
# -lmh2f128: the 128-bit float comparisons glibc's printf calls (tools/pc/kit/f128.c, built by dist.py). lld finds an
# archive's members whatever the order of the command line.
