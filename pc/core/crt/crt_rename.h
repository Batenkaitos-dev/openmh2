/* ps2crt's exported names with a prefix: for the tests, which link the host's
 * C library and the original game's library in one program. Included first
 * by ps2crt.h under -DCRT_PREFIX. The game's build does not use it. */
#ifndef CRT_RENAME_H
#define CRT_RENAME_H

#define memcpy crt_memcpy
#define memmove crt_memmove
#define memset crt_memset
#define memcmp crt_memcmp
#define strlen crt_strlen
#define strcpy crt_strcpy
#define strncpy crt_strncpy
#define strcat crt_strcat
#define strncat crt_strncat
#define strcmp crt_strcmp
#define strncmp crt_strncmp
#define strchr crt_strchr
#define strrchr crt_strrchr
#define strstr crt_strstr
#define strupr crt_strupr
#define strtok crt_strtok
#define abs crt_abs
#define atoi crt_atoi
#define rand crt_rand

#define expf crt_expf
#define acosf crt_acosf
#define atan2f crt_atan2f
#define powf crt_powf
#define sinf crt_sinf
#define cosf crt_cosf
#define atanf crt_atanf
#define floorf crt_floorf
#define ceilf crt_ceilf
#define tan crt_tan
#define atan2 crt_atan2
#define sqrt crt_sqrt

#define vsprintf crt_vsprintf
#define vsnprintf crt_vsnprintf
#define sscanf crt_sscanf
#define ps2_printf crt_printf
#define ps2_sprintf crt_sprintf
#define malloc crt_malloc
#define free crt_free

#endif
