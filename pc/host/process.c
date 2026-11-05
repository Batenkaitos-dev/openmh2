/*
 * Starting, watching and stopping mh2-core. Its environment is the host's
 * with MH2_IPC naming the block (on Linux the block's descriptor is
 * inherited) and MH2_DISC the disc archive (settings.c's launcher); its
 * standard streams are the host's. Only this child is
 * ever stopped, by its own handle or PID.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

#ifdef _WIN32
#define EXE ".exe"
#define PLATFORM "windows"
#else
#define EXE ""
#define PLATFORM "linux"
extern char **environ;
#endif

static struct {
    int started, ended, code;
#ifdef _WIN32
    HANDLE proc;
#else
    pid_t pid;
#endif
} core;

const char *process_default_core(void)
{
    static char path[1200];
    const char *base = SDL_GetBasePath();
    if (!base)
        base = "";
    snprintf(path, sizeof(path), "%smh2-core" EXE, base);
    FILE *f = hfs_fopen(path, "rb");
    if (!f) {
        /* the build tree: build/pc/<platform>-x86_64/ beside build/pc/<platform>-i686/core/ */
        snprintf(path, sizeof(path), "%s../" PLATFORM "-i686/core/mh2-core" EXE, base);
        f = hfs_fopen(path, "rb");
    }
    if (f)
        fclose(f);
    return path;
}

#ifdef _WIN32
/* The core takes its paths as UTF-8 (pc/core/host_file.h) and reads its command line and environment as UTF-16, so the
 * command line is made in UTF-16 from the strings given here, which are UTF-8 (a string that is not valid UTF-8 is read in
 * the ANSI code page, as the host's own ANSI sources give it: hostfs.h). */
#define to_wide hfs_wide

/* One argument on a command line, as the C runtime parses it back. */
static void append_arg(char *cmd, size_t cap, const char *a)
{
    size_t n = strlen(cmd);
    if (n && n + 1 < cap)
        cmd[n++] = ' ';
    if (n + 1 < cap)
        cmd[n++] = '"';
    for (const char *p = a;; p++) {
        size_t bs = 0;
        while (*p == '\\') {
            bs++;
            p++;
        }
        size_t times = !*p ? bs * 2 : *p == '"' ? bs * 2 + 1 : bs;
        for (size_t i = 0; i < times && n + 1 < cap; i++)
            cmd[n++] = '\\';
        if (!*p)
            break;
        if (n + 1 < cap)
            cmd[n++] = *p;
    }
    if (n + 1 < cap)
        cmd[n++] = '"';
    cmd[n] = 0;
}

int process_start(const char *path, char **args, int nargs)
{
    static char cmd[32768];
    cmd[0] = 0;
    append_arg(cmd, sizeof(cmd), path);
    for (int i = 0; i < nargs; i++)
        append_arg(cmd, sizeof(cmd), args[i]);
    SetEnvironmentVariableA(MH2IPC_ENV, g_map.name);
    if (g_cfg.disc_path) {
        wchar_t *wd = to_wide(g_cfg.disc_path);
        if (wd)
            SetEnvironmentVariableW(L"MH2_DISC", wd);
        free(wd);
    }
    wchar_t *wpath = to_wide(path), *wcmd = to_wide(cmd);
    STARTUPINFOEXW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    /* With a log file (play.c), the core's standard output and error are it, and that handle is the
     * only one the core inherits. */
    HANDLE log = play_log_handle();
    static _Alignas(16) unsigned char attrs[256];
    SIZE_T asize = sizeof(attrs);
    BOOL inherit = FALSE;
    DWORD flags = 0;
    if (log) {
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs;
        if (InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &asize) &&
            UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, &log, sizeof(log),
                                      NULL, NULL)) {
            si.StartupInfo.cb = sizeof(si);
            si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            si.StartupInfo.hStdInput = NULL;
            si.StartupInfo.hStdOutput = log;
            si.StartupInfo.hStdError = log;
            inherit = TRUE;
            flags = EXTENDED_STARTUPINFO_PRESENT;
        } else {
            si.lpAttributeList = NULL;
            si.StartupInfo.cb = sizeof(si.StartupInfo);
        }
    } else {
        si.StartupInfo.cb = sizeof(si.StartupInfo);
    }
    if (!GetConsoleWindow())   /* mh2 has no console (play.c, play_console_detach): nor does the core */
        flags |= CREATE_NO_WINDOW;
    BOOL created = wpath && wcmd &&
                   CreateProcessW(wpath, wcmd, NULL, NULL, inherit, flags, NULL, NULL, &si.StartupInfo, &pi);
    free(wpath);
    free(wcmd);
    if (!created) {
        host_log("CreateProcess %s: error %lu", path, (unsigned long)GetLastError());
        if (si.lpAttributeList)
            DeleteProcThreadAttributeList(si.lpAttributeList);
        return -1;
    }
    if (si.lpAttributeList)
        DeleteProcThreadAttributeList(si.lpAttributeList);
    CloseHandle(pi.hThread);
    core.proc = pi.hProcess;
    core.started = 1;
    host_log("mh2-core started (%s, pid %lu)", path, (unsigned long)pi.dwProcessId);
    return 0;
}

int process_running(int *code)
{
    if (!core.started || core.ended) {
        *code = core.code;
        return 0;
    }
    if (WaitForSingleObject(core.proc, 0) != WAIT_OBJECT_0)
        return 1;
    DWORD c = 1;
    GetExitCodeProcess(core.proc, &c);
    core.code = (int)c;
    core.ended = 1;
    CloseHandle(core.proc);
    *code = core.code;
    return 0;
}

void process_kill(void)
{
    if (core.started && !core.ended)
        TerminateProcess(core.proc, 1);
}
#else
int process_start(const char *path, char **args, int nargs)
{
    char **argv = calloc((size_t)nargs + 2, sizeof(char *));
    if (!argv)
        return -1;
    argv[0] = (char *)path;
    for (int i = 0; i < nargs; i++)
        argv[i + 1] = args[i];
    setenv(MH2IPC_ENV, g_map.name, 1);
    if (g_cfg.disc_path)
        setenv("MH2_DISC", g_cfg.disc_path, 1);
    int r = posix_spawn(&core.pid, path, NULL, NULL, argv, environ);
    free(argv);
    if (r != 0) {
        host_log("posix_spawn %s: %s", path, strerror(r));
        return -1;
    }
    core.started = 1;
    host_log("mh2-core started (%s, pid %d)", path, (int)core.pid);
    return 0;
}

int process_running(int *code)
{
    if (!core.started || core.ended) {
        *code = core.code;
        return 0;
    }
    int st;
    pid_t r = waitpid(core.pid, &st, WNOHANG);
    if (r == 0)
        return 1;
    core.code = r < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : WIFSIGNALED(st) ? 128 + WTERMSIG(st) : 1;
    core.ended = 1;
    *code = core.code;
    return 0;
}

void process_kill(void)
{
    if (core.started && !core.ended)
        kill(core.pid, SIGKILL);
}
#endif

