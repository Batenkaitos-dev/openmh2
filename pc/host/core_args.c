/*
 * The core's command line, as mh2 builds it (main.c). No SDL here, so the
 * host's tests can link it alone (tests/core_args_test.c).
 */
#include <string.h>

#include "core_args.h"

/* Play's calendar:
 * in play mh2 starts the core with --local-date, the host's date and time
 * zone. Not in a check or a route run (the core's --check, --frames or
 * --input), whose clock is the fixed one,
 * nor in a scripted run of mh2's own
 * (scripted: --quit-after or --screenshot, the ctest and the picture checks),
 * nor twice. */
int core_args_local_date(char *const *args, int nargs, int scripted)
{
    static const char *const fixed[] = {"--frames", "--local-date"};
    if (scripted)
        return 0;
    for (int i = 0; i < nargs; i++)
        for (size_t k = 0; k < sizeof(fixed) / sizeof(fixed[0]); k++)
            if (!strcmp(args[i], fixed[k]))
                return 0;
    return 1;
}

