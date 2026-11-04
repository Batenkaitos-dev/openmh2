# Local arrays of 16 bytes or more, and VLAs, get 16-byte alignment in the core's code built with the
# game's flags, on both targets: the AMD64 psABI's rule, which the original game (an x86-64 program) has and
# clang gives only on x86-64. tools/pc/stackalign.py
# raises those allocas' alignment in clang's unoptimised IR. It is the compiler launcher of every target
# created after this (include()d by pc/core/CMakeLists.txt): a compile with the game's flags (regcall)
# goes through it, any other is passed on unchanged. The game's own files keep ps2fp/ps2fpcc.py as their
# launcher, which calls the same rewrite after its pass.
set(CMAKE_C_COMPILER_LAUNCHER "${Python3_EXECUTABLE};${MH2_ROOT}/tools/pc/stackalign.py")
