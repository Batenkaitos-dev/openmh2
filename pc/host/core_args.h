#ifndef MH2_HOST_CORE_ARGS_H
#define MH2_HOST_CORE_ARGS_H
/* The core's command line (core_args.c). */

/* 1 when mh2 adds --local-date to the core's arguments ARGS (NARGS of them):
 * in play only. SCRIPTED: mh2's own run is a test or a picture check. */
int core_args_local_date(char *const *args, int nargs, int scripted);


#endif
