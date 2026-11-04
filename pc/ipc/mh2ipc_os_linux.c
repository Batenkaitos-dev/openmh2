/*
 * mh2ipc_os.h on Linux: the block is a memfd that the core inherits
 * (MH2_IPC=fd:N), or a file a test names by its path; the waits are
 * futexes on the shared words (FUTEX_WAIT without _PRIVATE: the two
 * processes map the same pages).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "ipc/mh2ipc_os.h"

static int map_fd(Mh2IpcMap *m, int fd, unsigned int bytes)
{
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        return -1;
    m->base = p;
    m->bytes = bytes;
    m->os[0] = (void *)(long)fd;
    return 0;
}

int mh2ipc_create(Mh2IpcMap *m, unsigned int bytes)
{
    memset(m, 0, sizeof(*m));
    int fd = (int)syscall(SYS_memfd_create, "mh2ipc", 0u);   /* no MFD_CLOEXEC: the core inherits it */
    if (fd < 0)
        return -1;
    if (ftruncate(fd, (off_t)bytes) != 0 || map_fd(m, fd, bytes) != 0) {
        close(fd);
        return -1;
    }
    snprintf(m->name, sizeof(m->name), "fd:%d", fd);
    return 0;
}

int mh2ipc_open(Mh2IpcMap *m, const char *name)
{
    memset(m, 0, sizeof(*m));
    int fd;
    if (strncmp(name, "fd:", 3) == 0)
        fd = dup(atoi(name + 3));
    else
        fd = open(name, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || size > 0x7FFFFFFF || map_fd(m, fd, (unsigned int)size) != 0) {
        close(fd);
        return -1;
    }
    /* the core's own descriptor: the inherited one isn't passed on */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    snprintf(m->name, sizeof(m->name), "%s", name);
    return 0;
}

void mh2ipc_close(Mh2IpcMap *m)
{
    if (m->base)
        munmap(m->base, m->bytes);
    if (m->base)
        close((int)(long)m->os[0]);
    m->base = NULL;
}

void mh2ipc_wait(Mh2IpcMap *m, int ev, const unsigned int *word, unsigned int old, unsigned int timeout_ms)
{
    (void)m;
    (void)ev;
    struct timespec ts = {(time_t)(timeout_ms / 1000u), (long)(timeout_ms % 1000u) * 1000000L};
    syscall(SYS_futex, word, FUTEX_WAIT, old, &ts, NULL, 0);
}

void mh2ipc_wake(Mh2IpcMap *m, int ev, const unsigned int *word)
{
    (void)m;
    (void)ev;
    syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0);
}

unsigned int mh2ipc_pid(void)
{
    return (unsigned int)getpid();
}

int mh2ipc_alive(Mh2IpcMap *m, unsigned int pid)
{
    (void)m;
    if (pid == (unsigned int)getpid() || pid == (unsigned int)getppid())
        return 1;
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

unsigned long long mh2ipc_time_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

void mh2ipc_wait_until_ns(unsigned long long due)
{
    unsigned long long now = mh2ipc_time_ns();
    if (now + 1500000ull < due) {
        unsigned long long ns = due - now - 1000000ull;
        struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
        nanosleep(&ts, NULL);
    }
    while (mh2ipc_time_ns() < due)
        __builtin_ia32_pause();
}
