/* The card file's durable replace on Windows (host_mcfs.c's write_whole): the
 * new image's data flushed to the disk (_commit: FlushFileBuffers), then
 * MoveFileEx over the card, written through. The C library's rename fails
 * when the card exists. The names are UTF-8 (host_file.h). */
#include <io.h>
#include <stdio.h>
#include <windows.h>

#include "host_file.h"

int HostMcfs_SyncFile(FILE *f);
int HostMcfs_Replace(const char *from, const char *to);

int HostMcfs_SyncFile(FILE *f)
{
    return _commit(_fileno(f)) == 0 ? 0 : -1;
}

int HostMcfs_Replace(const char *from, const char *to)
{
    return HostFile_Replace(from, to);
}
