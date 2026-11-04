/* printf's stream (crt_stdout.c). */
#ifndef CRT_STDOUT_H
#define CRT_STDOUT_H

#include "ps2crt.h"

void crt_so_begin(void);
void crt_so_piece(const char *p, u32 len);
int crt_so_end(void);
void crt_stdout_state_reset(void);

#endif
