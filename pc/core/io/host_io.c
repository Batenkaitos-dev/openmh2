/* The io module's small host helpers (io_host.h). */
#include <stdlib.h>

#include "io/io_host.h"

HOST void Host_IoFree(void *p)
{
    free(p);
}
