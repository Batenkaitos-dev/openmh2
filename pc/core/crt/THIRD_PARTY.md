# Third-party code in pc/core/crt

The platform layer's C library (the functions the game's boot executable
links from newlib and its libm, written from their behaviour at the
interface) is CC0 like the rest of the project, except the files that take
code from a freely licensed source the owner allowed (2026-10-13,
decision 2: newlib's and fdlibm's sources, with their licence notices kept
at the top of each such file). Each such file is listed here, so that the
split's port repository can carry the notices. libgcc's source (GPL) is
never read or copied. (The files here are the PC platform layer's versions of
the functions of the original game's C library.)

| File | Origin | Licence |
|---|---|---|
| crt_dtoa_quick.c | newlib 1.8.2, libc/stdlib/dtoa.c (David M. Gay's dtoa: the start of `_dtoa_r`'s floating-point path), adapted | David M. Gay / AT&T (1991): permission to use, copy, modify and distribute with the notice kept (the notice is at the top of the file) |
| crt_stdout.c | newlib 1.8.2, libc/stdio/fvwrite.c (the line-buffered branch of `__sfvwrite`) and fflush.c, adapted: printf's stdout | The Regents of the University of California (1990): redistribution and use permitted with the notice and its acknowledgement kept (the notice is at the top of the file) |
| crt_malloc.c | newlib 1.8.2, libc/stdlib/mallocr.c (Doug Lea's malloc 2.6.4: `mALLOc`, `malloc_extend_top`, `fREe`, `malloc_trim` and their macros), adapted: configured as the game's library was | Doug Lea: released to the public domain (the notice is at the top of the file) |
| crt_scanf.c | newlib 1.8.2, libc/stdio/vfscanf.c (`__svfscanf` and `__sccl`, without MB_CAPABLE), with the steps of refill.c's `__srefill`, ungetc.c's `ungetc` and fread.c's `fread` on sscanf's string stream, adapted | The Regents of the University of California (1990): redistribution and use permitted with the notice and its acknowledgement kept (the notice is at the top of the file) |
| crt_libm.c | newlib 4.6.0 and 1.8.2, libm/math and libm/common (fdlibm: the float versions of floor, ceil, atan, exp, acos, sqrt, atan2, pow, scalbn, sin, cos and their kernels, with rem_pio2's tables; the float versions by Ian Lance Taylor, Cygnus Support), adapted to the EE's float rules, with the constants as the game's toolchain converted newlib's literals | Sun Microsystems (1993): use, copy, modify and distribute freely granted with the notice preserved (the notice is at the top of the file) |
| crt_libm_d.c | newlib 1.8.2, libm/math and libm/common (fdlibm: s_tan.c, k_tan.c, e_rem_pio2.c with its tables, k_rem_pio2.c, s_floor.c, s_scalbn.c, s_copysign.c, s_atan.c, e_atan2.c, e_sqrt.c), adapted: every double operation through the C library's soft-double helpers' model | Sun Microsystems (1993): use, copy, modify and distribute freely granted with the notice preserved (the notice is at the top of the file) |
| ps2fp.h, with ../third_party/pcsx2_ps2float/ps2float_mul.h and ps2float_add.h | PCSX2 pull request 12001 ("Soft-Float"), commit 0392a64e992ac3fa5445dbe484933616cb204674, pcsx2/PS2Float.cpp (SHA-256 2fa504d2ec7fbad7c3c7a40ea0e106f67e19321d46c2b6c54328327be53f5511): the EE multiplier's Booth radix-4 model (`Booth`, `Add3`, `MulMantissa`, `DoMul`) and the EE adder's guard-bit model (`DoAdd`, `Add`, `Sub`, with the bit counting of common/BitUtils.h, SHA-256 6b669c934089c13d7ceb8df54f12e12651dcfc5495ef0165a52215ce3d44192e), adapted to C. ps2fp.h includes the adaptation for the "ps2" float model (the EE multiplier as the console's hardware results and the community's console tests show it) | **GPL-3.0-or-later** (PCSX2 Dev Team, 2002-2024): the licence text is ../third_party/pcsx2_ps2float/COPYING, the notice is at the top of the adaptation, the source is in SOURCE.txt next to it. A core built with it is GPL-3.0-or-later as a whole; the project's other files stay CC0 1.0 |

crt_ctype.c is written from newlib's published definition of the C locale's
ctype table (its class bits), with no code; crt_printf.c, crt_libc.c,
crt_heap.c and the others take no third-party code (crt_printf.c calls
crt_dtoa_quick.c for one case of dtoa's trailing zeros, and crt_stdout.c
for printf's stream).

SSLeay's notices (the old libtls.c) went with that file: no
SSLeay code is in the tree, so the licence paragraph of README.md no longer
carries the acknowledgement of Eric Young's software.
