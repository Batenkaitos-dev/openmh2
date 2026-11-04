/* ps2crt: the game's C library on PC.
 *
 * What the files of pc/core/crt share. ps2crt is the PS2 C library's
 * behaviour as plain C: newlib 1.8.2's allocator and printf, scanf and math
 * functions, the string and memory functions, and the soft-double helpers.
 * It was written against the original game's guest-register code
 * (lib_NAME(Ctx *)); here every function is an ordinary C function with the
 * signature the game declares (include/libc.h, libm.h), working on native
 * pointers (the core's pointers are 32 bits and game memory is at the PS2
 * addresses). The algorithms are unchanged.
 *
 * ps2crt is compiled with the core's flags (regcall by default, so a game
 * call can only bind to a function defined here) but never through the
 * ps2fp pass: every float operation is written with the bit-level
 * functions of ps2fp.h (through the nf_* names below). It includes no system
 * header with functions; it calls the platform layer only through
 * crt_kernel.h.
 *
 * The names a game file calls (memcpy, strlen, sinf, ...) are defined as
 * they are; a test that links the host's libc as well builds with
 * -DCRT_PREFIX to give every one of them the prefix crt_ (crt_rename.h).
 */
#ifndef PS2CRT_H
#define PS2CRT_H

#ifdef CRT_PREFIX
#include "crt_rename.h"
#endif
#include <stdint.h>

#include "ps2fp.h"

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;
typedef long long s64;

#ifndef NULL
#define NULL ((void *)0)
#endif

/* ----------------------------------------------- the EE's float, by name */
/* The names mh2native_float.h gave (the code of libm
 * was written with them): a float carries the 32 bits of an EE value, and
 * every operation is ps2fp.h's. Never an x86 float operation. */
static inline u32 nf_bits(float f) { return ps2fp_bits(f); }
static inline float nf_val(u32 u) { return ps2fp_val(u); }
static inline float nf_add(float a, float b) { return ps2fp_val(ps2fp_add(ps2fp_bits(a), ps2fp_bits(b))); }
static inline float nf_sub(float a, float b) { return ps2fp_val(ps2fp_sub(ps2fp_bits(a), ps2fp_bits(b))); }
static inline float nf_mul(float a, float b) { return ps2fp_val(ps2fp_mul(ps2fp_bits(a), ps2fp_bits(b))); }
static inline float nf_div(float a, float b) { return ps2fp_val(ps2fp_div(ps2fp_bits(a), ps2fp_bits(b))); }
static inline float nf_neg(float a) { return ps2fp_val(ps2fp_neg(ps2fp_bits(a))); }
static inline float nf_abs(float a) { return ps2fp_val(ps2fp_abs(ps2fp_bits(a))); }
static inline int nf_eq(float a, float b) { return ps2fp_eq(ps2fp_bits(a), ps2fp_bits(b)); }
static inline int nf_lt(float a, float b) { return ps2fp_lt(ps2fp_bits(a), ps2fp_bits(b)); }
static inline int nf_le(float a, float b) { return ps2fp_le(ps2fp_bits(a), ps2fp_bits(b)); }
static inline int nf_ne(float a, float b) { return !nf_eq(a, b); }
static inline int nf_gt(float a, float b) { return nf_lt(b, a); }
static inline int nf_ge(float a, float b) { return nf_le(b, a); }
static inline float nf_i2f(int w) { return ps2fp_val(ps2fp_i2f(w)); }
static inline int nf_f2i(float a) { return ps2fp_f2i(ps2fp_bits(a)); }

/* ------------------------------------------------------------ diagnostics */
/* The owner's decision 1: inputs the
 * game was never seen to pass are handled by the C standard, and the
 * function notes it (crt_diag.c counts them; a debug build of the platform
 * layer may print them). */
void crt_unseen(const char *func, const char *fmt, ...);
u32 crt_unseen_count(void);
int crt_unseen_get(u32 i, const char **func, const char **fmt, u32 *count);
#define CRT_UNSEEN(func, ...) crt_unseen(func, __VA_ARGS__)

#endif /* PS2CRT_H */
