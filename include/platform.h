#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * Which platform the game's C is built for.
 * - PLATFORM_PS2: the matching build. MWCC predefines __MWERKS__ and none
 *   of the GNU compilers' macros (checked 2026-10-23 with mwccps2
 *   3.0b52-030722).
 * - PLATFORM_PC: the native port's game core, mh2-core, built by any other
 *   compiler.
 * A construct only MWCC and the EE understand keeps its PS2 form under
 * PLATFORM_PS2 and gets its PC form under PLATFORM_PC, so the matching
 * build's bytes never change.
 */
#ifdef __MWERKS__
#define PLATFORM_PS2 1
#define PLATFORM_PC 0
#else
#define PLATFORM_PS2 0
#define PLATFORM_PC 1
#endif

#endif /* PLATFORM_H */
