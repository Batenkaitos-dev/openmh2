/* The second pass's own VIF: gfx_vif.c again, under the names of gfx_pass_prefix.h. */
#include "common.h"
#include "gfx_pass_prefix.h"
/* the table of the data blocks' pages is the real unit's, built on the game's thread and read through a pointer (gfx_pass_run.c hands it over) */
#define image_page (*PPN(image_page))
#include "gfx_vif.c"
#undef image_page
void PPN(vif_set_pages)(const u16 *pages)
{
    PPN(image_page) = (u16(*)[65536])pages;
    image_pages_ready = 1;
}
