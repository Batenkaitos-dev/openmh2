/* ps2eval: evaluates ps2fp.h's operations on lines "OP A [B]" (hex bit
 * patterns) from stdin, printing the result bits, one per line. Used by
 * tools/pc/fpcheck.py to compare the EE's result of an operation on
 * constants with the round-to-nearest value a compiler's constant folding
 * gives. */
#include <stdio.h>
#include <string.h>

#include "ps2fp.h"

int main(void)
{
    char op[32];
    unsigned a, b;
    char line[256];
    setvbuf(stdout, NULL, _IOLBF, 0); /* line by line: fpfold.py drives it through a pipe */
    while (fgets(line, sizeof line, stdin)) {
        a = b = 0;
        if (sscanf(line, "%31s %x %x", op, &a, &b) < 2)
            continue;
        unsigned r = 0;
        if (!strcmp(op, "ps2_fadd")) r = ps2fp_add(a, b);
        else if (!strcmp(op, "ps2_fsub")) r = ps2fp_sub(a, b);
        else if (!strcmp(op, "ps2_fmul")) r = ps2fp_mul(a, b);
        else if (!strcmp(op, "ps2_fdiv")) r = ps2fp_div(a, b);
        else if (!strcmp(op, "ps2_fneg")) r = ps2fp_neg(a);
        else if (!strcmp(op, "ps2_fabs")) r = ps2fp_abs(a);
        else if (!strcmp(op, "ps2_fsqrt")) r = ps2fp_sqrt(a);
        else if (!strcmp(op, "ps2_feq")) r = ps2fp_eq(a, b);
        else if (!strcmp(op, "ps2_flt")) r = ps2fp_lt(a, b);
        else if (!strcmp(op, "ps2_fle")) r = ps2fp_le(a, b);
        else if (!strcmp(op, "ps2_i2f")) r = ps2fp_i2f((int)a);
        else if (!strcmp(op, "ps2_u2f")) r = ps2fp_u2f(a);
        else if (!strcmp(op, "ps2_f2i")) r = (unsigned)ps2fp_f2i(a);
        else if (!strcmp(op, "ps2_f2u")) r = ps2fp_f2u(a);
        printf("%08x\n", r);
    }
    return 0;
}
