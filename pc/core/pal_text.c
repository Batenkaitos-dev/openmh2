/* PC_TEXT (include/pal.h): the text layer asks the platform layer for the text pack's replacement of a string. */
#include "common.h"
#include "io/io_host.h"
#include "pal.h"

const char *Pal_Text(const char *s)
{
    return Host_TextLookup(s);
}
