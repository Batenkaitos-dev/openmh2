/* ps2crt's call taps.
 *
 * The C library functions of config/pc/seam_calls.txt describe each call to
 * the trace (pc/core/verify/verify.h, section 1) when the core's build
 * defines CRT_TAPS (crt/CMakeLists.txt) and the trace is on: one test of a
 * flag when it is off. The function's body is unchanged (CrtImpl_NAME); its
 * public name is the tap. The tests of ps2crt build without CRT_TAPS: the
 * taps compile away, and the bodies are what they test.
 *
 * The kinds per function are the core's half of the typed taps; the
 * original game's side prints the same from config/pc/seam_calls.txt's prototypes.
 * Doubles go as their 64 bits (Dword), as the EE's soft-float
 * passes them.
 */
#ifndef CRT_TAP_H
#define CRT_TAP_H



#endif
