/*
 * mh2's memory cards:
 * - mh2 keeps its own cards, in a writable folder of its own: the setting
 *   cards = DIR, else the user data folder's cards (beside mh2.ini). The
 *   core writes CARD.journal, CARD.tmp and CARD.bak beside a card; cards are
 *   chosen by name only (card1 = NAME, card2 = NAME; port 1's default is
 *   Mcd001.ps2, which the core makes when it is missing).
 * - mh2 never writes the old version's cards (its memory card folder,
 *   memcards, and the settings memcard1/memcard2): at the first start, when
 *   its own folder doesn't exist yet, it offers once to copy them in
 *   (port 1's as Mcd001.ps2, port 2's as Mcd002.ps2). --import-card FILE
 *   copies any card in by its name.
 * - A second mh2 on the same card is refused: CARD.lock holds the PID, is
 *   removed at exit, and is taken over when the PID it names is gone.
 * mh2's --memcard1/--memcard2 FILE, and the core's own, use FILE in place
 * (tests and scripts), locked as well.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define SEP "\\"
#else
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#define SEP "/"
#endif

#include "mh2host.h"

static char g_dir[1200];
static char g_locks[2][1300];

static int is_file(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* A card's name: no folder in it, and not one of the core's companion files. */
static int card_name_ok(const char *n)
{
    size_t len = strlen(n);
    static const char *const companions[] = {".journal", ".tmp", ".bak", ".lock"};
    if (!len || strchr(n, '/') || strchr(n, '\\') || strchr(n, ':'))
        return 0;
    for (size_t i = 0; i < sizeof(companions) / sizeof(companions[0]); i++) {
        size_t c = strlen(companions[i]);
        if (len > c && !SDL_strcasecmp(n + len - c, companions[i]))
            return 0;
    }
    return 1;
}

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

static int copy_in(const char *from, const char *name)
{
    char to[1400];
    snprintf(to, sizeof(to), "%s" SEP "%s", g_dir, name);
    if (is_file(to)) {
        host_log("cards: %s is there already: %s not copied", to, from);
        return -1;
    }
    if (!SDL_CopyFile(from, to)) {
        host_log("cards: cannot copy %s to %s (%s)", from, to, SDL_GetError());
        return -1;
    }
    host_log("cards: copied %s to %s", from, to);
    return 0;
}

/* The first start: the old version's cards, copied in if the player says so. */
static void offer_import(const char *old1, const char *old2)
{
    const char *data = play_data_dir();
    static char def1[1300], def2[1300];
    const char *from[2] = {old1 && is_file(old1) ? old1 : NULL, old2 && is_file(old2) ? old2 : NULL};
    if (data) {
        snprintf(def1, sizeof(def1), "%smemcards" SEP "Mcd001.ps2", data);
        snprintf(def2, sizeof(def2), "%smemcards" SEP "Mcd002.ps2", data);
        if (!from[0] && is_file(def1))
            from[0] = def1;
        if (!from[1] && is_file(def2))
            from[1] = def2;
    }
    if (!from[0] && !from[1])
        return;
    char text[3200];
    snprintf(text, sizeof(text),
             "mh2 keeps its own memory cards, in\n%s\n\nCopy the old version's cards into it, so that your saves carry over?\n"
             "%s%s%s%s\nThe old files stay as they are, and mh2 never writes them.",
             g_dir, from[0] ? "\nPort 1: " : "", from[0] ? from[0] : "", from[1] ? "\nPort 2: " : "",
             from[1] ? from[1] : "");
    const SDL_MessageBoxButtonData buttons[2] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Copy them"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Start with a new card"},
    };
    const SDL_MessageBoxData box = {SDL_MESSAGEBOX_INFORMATION, NULL, "Monster Hunter 2: memory cards", text, 2, buttons,
                                    NULL};
    int pick = 0;
    if (!SDL_ShowMessageBox(&box, &pick))
        pick = 0;
    host_log("cards: the old version's cards %s", pick == 1 ? "are copied in" : "are not copied (the player's choice)");
    if (pick != 1)
        return;
    if (from[0])
        copy_in(from[0], "Mcd001.ps2");
    if (from[1])
        copy_in(from[1], "Mcd002.ps2");
}

/* The cards folder, made if missing; *fresh = 1 when it was made now. */
static int cards_dir(const char *setting, int *fresh)
{
    if (setting && setting[0]) {
        snprintf(g_dir, sizeof(g_dir), "%s", setting);
    } else {
        const char *data = play_data_dir();
        if (!data)
            return -1;
        snprintf(g_dir, sizeof(g_dir), "%scards", data);
    }
    SDL_PathInfo info;
    *fresh = !SDL_GetPathInfo(g_dir, &info);
    if (!SDL_CreateDirectory(g_dir)) {
        host_log("cards: cannot create %s (%s)", g_dir, SDL_GetError());
        return -1;
    }
    return 0;
}

int cards_import(const char *file, const char *setting)
{
    int fresh = 0;
    if (cards_dir(setting, &fresh) != 0)
        return 1;
    if (!is_file(file)) {
        host_log("--import-card: %s is not a file", file);
        return 1;
    }
    const char *name = base_name(file);
    if (!card_name_ok(name)) {
        host_log("--import-card: %s is not a card's name", name);
        return 1;
    }
    return copy_in(file, name) == 0 ? 0 : 1;
}

/* The cards for the core (main.c, before the launcher's other steps): ports 1 and 2 from mh2's
 * folder, unless --memcard1/2 or the core's arguments name a file. PLAY: the import may be offered. */
void cards_choose(int play, const char *card1, const char *card2, const char *old1, const char *old2,
                  const char *setting)
{
    int fresh = 0;
    if (cards_dir(setting, &fresh) != 0) {
        host_log("cards: no folder for mh2's cards: memory card 1 stays empty");
        return;
    }
    host_log("cards: %s", g_dir);
    if (fresh && play && g_cfg.automated)
        host_log("cards: an automated run (%s) offers no import", g_cfg.automated);
    else if (fresh && play)
        offer_import(old1, old2);
    else if (old1 || old2)
        host_log("cards: memcard1/memcard2 in the settings are the old version's: mh2 uses its own cards "
                 "(card1, card2), and never writes those");
    const char *names[2] = {card1 && card1[0] ? card1 : "Mcd001.ps2", card2};
    for (int p = 1; p >= 0; p--) {
        const char *arg = p ? "--memcard2" : "--memcard1";
        if (!names[p])
            continue;
        if (!card_name_ok(names[p])) {
            host_log("cards: card%d = %s: a card is named by its file name in %s", p + 1, names[p], g_dir);
            continue;
        }
        int named = 0;
        for (int i = 0; i < g_cfg.core_nargs; i++)
            named |= !strcmp(g_cfg.core_args[i], arg);
        if (named)
            continue;
        char path[1400];
        snprintf(path, sizeof(path), "%s" SEP "%s", g_dir, names[p]);
        launcher_core_arg(arg, path);
    }
}

static int pid_alive(unsigned long pid)
{
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
#else
    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
#endif
}

static void unlock_all(void)
{
    for (int i = 0; i < 2; i++)
        if (g_locks[i][0]) {
            remove(g_locks[i]);
            g_locks[i][0] = 0;
        }
}

/* CARD.lock for each card the core is given: 0, or -1 (another mh2 has it; the message in MSG). */
int cards_lock(char *msg, size_t cap)
{
    int n = 0;
    for (int i = 0; i + 1 < g_cfg.core_nargs && n < 2; i++) {
        if (strcmp(g_cfg.core_args[i], "--memcard1") && strcmp(g_cfg.core_args[i], "--memcard2"))
            continue;
        char path[1300];
        snprintf(path, sizeof(path), "%s.lock", g_cfg.core_args[i + 1]);
        for (int tries = 0; tries < 2; tries++) {
            FILE *f = fopen(path, "wx");
            if (f) {
                fprintf(f, "%u\n", (unsigned int)mh2ipc_pid());
                fclose(f);
                snprintf(g_locks[n++], sizeof(g_locks[0]), "%s", path);
                break;
            }
            unsigned long pid = 0;
            f = fopen(path, "r");
            if (f) {
                if (fscanf(f, "%lu", &pid) != 1)
                    pid = 0;
                fclose(f);
            }
            if (pid && pid_alive(pid)) {
                snprintf(msg, cap, "The memory card %s is in use by another mh2 (process %lu). Close it first.",
                         g_cfg.core_args[i + 1], pid);
                unlock_all();
                return -1;
            }
            host_log("cards: %s names a process that is gone (%lu): taken over", path, pid);
            remove(path);
        }
    }
    if (n)
        atexit(unlock_all);
    return 0;
}
