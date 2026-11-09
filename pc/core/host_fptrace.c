/* The product trace's file (pal_fptrace.c, the trace build only): the system's stdio. */
#include <stdio.h>

#include "host_file.h"

static FILE *sFile;

HOST int Host_FpTraceOpen(const char *path)
{
    sFile = HostFile_Open(path, "wb");
    return sFile != NULL;
}

HOST void Host_FpTraceWrite(const void *rec, unsigned size)
{
    if (sFile)
        fwrite(rec, size, 1, sFile);
}
