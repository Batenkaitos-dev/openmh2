#ifndef EE_STDARG_H
#define EE_STDARG_H

#include "platform.h"

/*
 * va_list and va_start as MWCC compiles them for the EE. The register save
 * area ends at the incoming stack arguments; __builtin_args_info(2) is the
 * number of named arguments in registers. The game never calls va_end (a
 * no-op on the EE).
 *
 * PLATFORM_PC takes the host compiler's own: the variadic functions the
 * game defines (Font_Printf, Text_Printf and their kin) pass their va_list
 * to the platform layer's vsprintf and vsnprintf, which read it with the
 * same compiler's va_arg. MWCC's builtins do not exist there.
 */
#if PLATFORM_PS2
typedef char *va_list;
#define va_start(ap, last) \
    ((ap) = (char *)__builtin_next_arg(last) - (__builtin_args_info(2) >= 8 ? 0 : (8 - __builtin_args_info(2)) * 8))
#else
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#endif

#endif /* EE_STDARG_H */
