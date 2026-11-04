/* The card file's durable replace on Linux (host_mcfs.c's write_whole): the
 * new image's data flushed to the disk, the rename over the card, then the
 * directory flushed so that the rename itself survives a power cut. */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int HostMcfs_SyncFile(FILE *f);
int HostMcfs_Replace(const char *from, const char *to);

int HostMcfs_SyncFile(FILE *f)
{
    return fsync(fileno(f)) == 0 ? 0 : -1;
}

int HostMcfs_Replace(const char *from, const char *to)
{
    char dir[1100];
    const char *slash;
    int fd;

    if (rename(from, to) != 0)
        return -1;
    slash = strrchr(to, '/');
    if (!slash) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == to) {
        snprintf(dir, sizeof(dir), "/");
    } else {
        snprintf(dir, sizeof(dir), "%.*s", (int)(slash - to), to);
    }
    fd = open(dir, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);   /* the rename is done whatever this gives */
        close(fd);
    }
    return 0;
}
