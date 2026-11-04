# ps2crt: the game's C library on PC, and the VU0 operations.
# include()d by pc/core/CMakeLists.txt in its own scope: this file only sets
# MH2_CRT_SOURCES, with absolute paths. The core builds them as the "crt"
# library with its flags (regcall by default, -Wall -Wextra -Werror), never
# through tools/pc/ps2fp/ps2fpcc.py: every float operation in them is written
# with the bit-level functions of ps2fp.h. What they call outside themselves is
# crt_kernel.h (weak defaults in crt_k_default.c).
set(MH2_CRT_SOURCES "")
foreach(_f crt_libc.c crt_libm.c crt_libm_d.c
           crt_heap.c crt_malloc.c crt_stdout.c crt_printf.c crt_scanf.c crt_dtoa_quick.c
           crt_diag.c crt_k_default.c crt_vu0.c crt_mem_loops.c crt_ctype.c)
  list(APPEND MH2_CRT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/${_f}")
endforeach()
# The call taps (crt_tap.h): on in the core's build, off in the tests of ps2crt.
