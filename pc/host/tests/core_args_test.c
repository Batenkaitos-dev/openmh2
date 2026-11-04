/* core_args.c: when mh2 adds --local-date to the core's arguments. */
#include <stdio.h>
#include <string.h>

#include "core_args.h"

static int fails;

static void check(int got, int want, const char *what)
{
    if (got != want) {
        printf("FAIL: %s: %d, expected %d\n", what, got, want);
        fails++;
    }
}

int main(void)
{
    char *play[] = {"--memcard1", "Mcd001.ps2"};
    char *frames[] = {"--disc", "a.arc", "--frames", "600"};
    char *twice[] = {"--local-date"};
    check(core_args_local_date(play, 0, 0), 1, "play, no arguments");
    check(core_args_local_date(play, 2, 0), 1, "play with a card");
    check(core_args_local_date(play, 2, 1), 0, "a scripted run (--quit-after, --screenshot)");
    check(core_args_local_date(frames, 4, 0), 0, "a run of N frames");
    check(core_args_local_date(twice, 1, 0), 0, "already there");
    printf("core_args_test: %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
