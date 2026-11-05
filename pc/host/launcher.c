/*
 * The launcher: the pages and what the player's keys do on them (launcher.h; the launcher design, 4 and 9). The window,
 * the font and the rows' drawing are launcher_ui.c; the settings and their file are settings.c.
 *
 * Pages: Home, Disc and archive (and the progress of making it), Memory cards (and a new card), Picture, Game (and the
 * Japanese keyboard keys under it), Folders and logs (and the last log), and the first run's steps. Each row is made from
 * the settings table's page and type. The player's changes live in HostConfig at once (what the game will do)
 * and reach mh2.ini when Play or Save is chosen, never in an automated run.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher.h"
#include "launcher_make.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "mh2host.h"
#include "settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

enum { PG_HOME, PG_PICTURE, PG_GAME, PG_KEYS, PG_FOLDERS, PG_LOG, PG_DISC, PG_MAKING, PG_CARDS, PG_NEWCARD, PG_FIRST1,
       PG_FIRST3, PG_FCKEYS, NPAGES };
enum { A_INFO, A_SET, A_GO, A_PLAY, A_QUIT, A_BACK, A_DEFAULTS, A_OPEN_DATA, A_OPEN_LOGS, A_SHOWLOG,
       A_PICK_ISO, A_PICK_DIR, A_CHOOSE_ARC, A_MAKE_TABLE, A_USE_SOURCE, A_CANCEL_MAKE, A_MAKE_DONE,
       A_CARDPORT, A_NEWCARD, A_IMPORT, A_OPEN_CARDS, A_NC_NAME, A_NC_PORT, A_NC_MAKE, A_FIRST_NEW, A_LATER };
enum { D_NONE, D_SAVE, D_FAIL, D_OPENFAIL, D_MAKEFAIL };
enum { ARC_MISSING, ARC_READY, ARC_BAD, ARC_NOTABLE };

#define MAXROWS 20
typedef struct PRow {
    int act, arg;
    int invert;   /* the value is shown the other way round (Skip this screen: the key is launcher) */
    const char *help;
} PRow;
typedef struct Page {
    UiRow ui[MAXROWS];
    PRow row[MAXROWS];
    char val[MAXROWS][300];
    char lab[MAXROWS][128];   /* the labels are copies: a row's text may be made by the page being built */
    int n;
    const char *title;
} Page;

static struct {
    int page, sel[NPAGES];
    int dialog, dsel, after;   /* after: A_PLAY or A_QUIT, the act a failed save was part of */
    int done, result;
    char message[800];
    char **log;
    int nlog, log_first;
    int script;   /* the test drives it (development builds) */
    /* the disc and archive */
    struct {
        int state;                   /* ARC_* */
        char path[1200];             /* the archive found, or where it would be made */
        unsigned long long size;
        int nfiles;
    } arc;
    MakeSource src[6];               /* the copies found for the first run */
    int nsrc;
    int make_back;                   /* the page the progress screen goes back to (PG_FIRST1: the first run's step 2) */
    char make_source[1200];
    MakeStatus ms;
    int pick_wait, pick_kind;        /* a file dialog is open: 0 disc image, 1 folder, 2 another archive */
    /* the memory cards */
    char cdir[1100];
    int have_cdir;
    CardInfo cards[24];
    int ncards;
    char nc_name[80];
    int nc_port;
    int first;                       /* the first run: no mh2.ini, and no archive or no card */
    int text_on;
} L;

static struct {
    SDL_AtomicInt got;
    char path[1200];
} dlg;

/* ---------------------------------------------------------------- when it appears */

static int shift_held(void)
{
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return 0;   /* no window yet to ask: --launcher shows it */
#endif
}

int launcher_wanted(void)
{
    if (g_cfg.launcher < 0 || g_cfg.no_window || g_cfg.import_card)
        return 0;
    if (g_cfg.automated || g_cfg.quit_after)
        return 0;
    if (!launcher_disc_present())
        return 1;   /* the first run, or an archive gone missing: the steps to make it (6) */
    if (g_cfg.launcher > 0 || shift_held())
        return 1;
    return !strcmp(settings_text(settings_find("launcher")), "on");
}

/* ---------------------------------------------------------------- the rows */

static void add(Page *p, const char *label, const char *value, unsigned flags, int act, int arg, const char *help)
{
    if (p->n >= MAXROWS)
        return;
    int i = p->n++;
    p->row[i] = (PRow){act, arg, 0, help};
    p->val[i][0] = 0;
    if (value)
        snprintf(p->val[i], sizeof(p->val[i]), "%s", value);
    snprintf(p->lab[i], sizeof(p->lab[i]), "%s", label);
    p->ui[i] = (UiRow){p->lab[i], value ? p->val[i] : NULL, flags};
}

/* A setting's row: its label, its value in words between arrows, and the command line's mark. */
static void add_setting(Page *p, int idx)
{
    const Setting *s = settings_row(idx);
    if (p->n >= MAXROWS)
        return;
    if ((s->type == SET_TEXT && !s->choices) || s->type == SET_KEY) {   /* shown, not changed here */
        const char *v = settings_text(idx);
        add(p, TX(s->label), *v ? v : TX(T_DEFAULT), UR_DIM, A_INFO, idx, TX(s->help));
        return;
    }
    char v[300];
    snprintf(v, sizeof(v), "%s%s%s", settings_shown(idx), settings_given(idx) ? " " : "", settings_given(idx) ? TX(T_CMDLINE) : "");
    unsigned fl = UR_ARROWS;
    int count = settings_choice_count(idx);
    if (count > 2) {
        int pos = settings_choice_pos(idx);
        if (pos <= 0)
            fl |= UR_NOLEFT;
        if (pos >= count - 1)
            fl |= UR_NORIGHT;
    }
    add(p, TX(s->label), v, fl, A_SET, idx, TX(s->help));
}

static void add_defaults(Page *p)
{
    int off = 0;
    for (int i = 0; i < p->n; i++)
        if (p->row[i].act == A_SET && !settings_is_default(p->row[i].arg))
            off = 1;
    if (off)
        add(p, TX(T_DEFAULTS), NULL, UR_GREEN, A_DEFAULTS, 0, TX(T_DEFAULTS_H));
}

static void summary_picture(char *out, size_t cap)
{
    out[0] = 0;
    char part[100];
    int ssaa = settings_find("ssaa"), hires = settings_find("hires"), full = settings_find("fullscreen");
    if (strcmp(settings_text(ssaa), "1")) {
        snprintf(part, sizeof(part), TX(T_SUM_SSAA), settings_shown(ssaa));
        snprintf(out + strlen(out), cap - strlen(out), "%s", part);
    }
    if (!strcmp(settings_text(hires), "on"))
        snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? ", " : "", TX(T_SUM_HIRES));
    if (!strcmp(settings_text(full), "on"))
        snprintf(out + strlen(out), cap - strlen(out), "%s%s", out[0] ? ", " : "", TX(T_SUM_FULLSCREEN));
    if (!out[0])
        snprintf(out, cap, "%s", TX(T_SUM_DEFAULTS));
}

static void summary_game(char *out, size_t cap)
{
    snprintf(out, cap, "%s", !strcmp(settings_text(settings_find("widescreen")), "on") ? TX(T_SUM_WIDE_ON) : TX(T_SUM_WIDE_OFF));
    if (!strcmp(settings_text(settings_find("usb_keyboard")), "on"))
        snprintf(out + strlen(out), cap - strlen(out), ", %s", TX(T_SUM_KEYBOARD));
}

/* The data folder, as the page shows it (the test shows no machine's path). */
static const char *data_dir(void)
{
    return play_data_dir();
}

static void log_load(void);

/* ---------------------------------------------------------------- the disc and archive, the cards: what the pages show */

static int is_file(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* A path as the page shows it (the test shows no machine's path: TEST/ and the last part). */
static const char *shown(const char *path)
{
    return path;
}

static unsigned le32(const unsigned char *p)
{
    return p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* The archive's header and size agree (tools/port/make_archive.py's format), and the movie table is beside it. */
static int arc_inspect(const char *path, unsigned long long *size, int *nfiles)
{
    SDL_PathInfo info;
    unsigned char h[32];
    if (!SDL_GetPathInfo(path, &info) || info.type != SDL_PATHTYPE_FILE)
        return ARC_MISSING;
    FILE *f = hfs_fopen(path, "rb");
    size_t got = f ? fread(h, 1, sizeof(h), f) : 0;
    if (f)
        fclose(f);
    unsigned long long recorded = got == sizeof(h) ? (unsigned long long)le32(h + 24) | (unsigned long long)le32(h + 28) << 32 : 0;
    if (got != sizeof(h) || memcmp(h, "MH2DARC", 8) || le32(h + 8) != 1 || le32(h + 16) != 128 || !le32(h + 12) ||
        le32(h + 12) > 4096 || recorded != (unsigned long long)info.size)
        return ARC_BAD;
    *size = recorded;
    *nfiles = (int)le32(h + 12);
    char table[1400];
    snprintf(table, sizeof(table), "%s", path);
    char *cut = NULL;
    for (char *p = table; *p; p++)
        if (*p == '/' || *p == '\\')
            cut = p;
    snprintf(cut ? cut + 1 : table, sizeof(table) - (size_t)(cut ? cut + 1 - table : 0), "mh2-movies.txt");
    return is_file(table) ? ARC_READY : ARC_NOTABLE;
}

static void arc_refresh(void)
{
    memset(&L.arc, 0, sizeof(L.arc));
    int found;
        found = launcher_disc_path(L.arc.path, sizeof(L.arc.path)) == 0;
    L.arc.state = found ? arc_inspect(L.arc.path, &L.arc.size, &L.arc.nfiles) : ARC_MISSING;
}

static void arc_summary(char *out, size_t cap)
{
    switch (L.arc.state) {
    case ARC_READY: snprintf(out, cap, TX(T_ARC_SUM_READY), L.arc.nfiles); break;
    case ARC_BAD: snprintf(out, cap, "%s", TX(T_ARC_BAD)); break;
    case ARC_NOTABLE: snprintf(out, cap, "%s", TX(T_ARC_NOTABLE)); break;
    default: snprintf(out, cap, "%s", TX(T_ARC_MISSING)); break;
    }
}

/* The free space where the archive goes. */
static void space_text(char *out, size_t cap)
{
    char sz[40];
    uint64_t f = make_free_space(L.arc.path);
    if (!f) {
        snprintf(out, cap, "?");
        return;
    }
    make_size_text(f, sz, sizeof(sz));
    snprintf(out, cap, "%s", sz);
}

static void size_text(unsigned long long b, char *out, size_t cap)
{
    if (b >= 1000000ull)
        make_size_text(b, out, cap);
    else
        snprintf(out, cap, "%.0f KB", (double)b / 1024.0);
}

static void cards_refresh(void)
{
    L.have_cdir = cards_folder(settings_text(settings_find("cards")), L.cdir, sizeof(L.cdir)) == 0;
    L.ncards = L.have_cdir ? cards_list(L.cdir, L.cards, 24, L.script) : 0;
}

static const char *port_card(int p)
{
    const char *v = settings_text(settings_find(p ? "card2" : "card1"));
    return v && *v ? v : p ? NULL : "Mcd001.ps2";
}

static int card_listed(const char *name)
{
    for (int i = 0; i < L.ncards; i++)
        if (!strcmp(L.cards[i].name, name))
            return 1;
    return 0;
}

/* The Home page's words for the cards. */
static void cards_summary(char *out, size_t cap)
{
    const char *c1 = port_card(0), *c2 = port_card(1);
    snprintf(out, cap, "%s in port 1", c1);
    if (c2)
        snprintf(out + strlen(out), cap - strlen(out), ", %s in port 2", c2);
}

/* The names a port can take: the cards in the folder (the other port's left out), the card it has now if it is not in the
 * folder yet, and for port 2 "none" first (""). *CUR: the place of the card it has now. Returns how many. */
static int port_choices(int port, const char **names, int max, int *cur)
{
    const char *mine = port_card(port), *other = port_card(!port);
    int n = 0;
    *cur = 0;
    if (port) {
        names[n++] = "";
        if (!mine)
            *cur = 0;
    }
    for (int i = 0; i < L.ncards && n < max; i++) {
        if (other && !strcmp(L.cards[i].name, other))
            continue;
        if (mine && !strcmp(L.cards[i].name, mine))
            *cur = n;
        names[n++] = L.cards[i].name;
    }
    if (mine && !card_listed(mine) && n < max) {
        *cur = n;
        names[n++] = mine;
    }
    return n;
}

/* The first run (6.1): no mh2.ini, and the game not set up (no archive, or no card yet). */
static int first_run(void)
{
    const char *ini = settings_save_path();
    if (!ini || is_file(ini))
        return 0;
    return L.arc.state != ARC_READY || L.ncards == 0;
}

static void build(Page *p)
{
    memset(p, 0, sizeof(*p));
    char sum[200];
    switch (L.page) {
    case PG_HOME: {
        p->title = TX(T_TITLE);
        int ready = L.arc.state == ARC_READY;
        if (!ready)
            add(p, TX(T_BANNER), NULL, UR_WARN, A_GO, PG_DISC, TX(T_BANNER_H));
        add(p, TX(T_PLAY), NULL, ready ? 0 : UR_DIM, A_PLAY, 0, ready ? TX(T_PLAY_H) : TX(T_PLAY_NOARC));
        arc_summary(sum, sizeof(sum));
        add(p, TX(T_DISC), sum, ready ? 0 : UR_WARN, A_GO, PG_DISC, TX(T_DISC_H));
        cards_summary(sum, sizeof(sum));
        add(p, TX(T_CARDS_PAGE), sum, 0, A_GO, PG_CARDS, TX(T_CARDS_PAGE_H));
        summary_picture(sum, sizeof(sum));
        add(p, TX(T_PICTURE), sum, 0, A_GO, PG_PICTURE, TX(T_PICTURE_H));
        summary_game(sum, sizeof(sum));
        add(p, TX(T_GAME), sum, 0, A_GO, PG_GAME, TX(T_GAME_H));
        add(p, TX(T_FOLDERS), NULL, 0, A_GO, PG_FOLDERS, TX(T_FOLDERS_H));
        int li = settings_find("launcher");
        add(p, TX(T_SKIP), settings_text(li)[1] == 'n' ? "off" : "on", UR_ARROWS | UR_GAP, A_SET, li, TX(T_SKIP_H));
        p->row[p->n - 1].invert = 1;
        add(p, TX(T_QUIT), NULL, 0, A_QUIT, 0, TX(T_QUIT_H));
        break;
    }
    case PG_PICTURE:
        p->title = TX(T_HEAD_PICTURE);
        for (int i = 0; i < settings_count(); i++)
            if (settings_row(i)->page == PAGE_PICTURE && settings_row(i)->type != SET_TEXT)   /* (the textures folder: mh2.ini) */
                add_setting(p, i);
        add_defaults(p);
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    case PG_GAME:
        p->title = TX(T_HEAD_GAME);
        add_setting(p, settings_find("widescreen"));
        add_setting(p, settings_find("usb_keyboard"));
        add_setting(p, settings_find("usb_keyboard_toggle"));
        add_setting(p, settings_find("state_key_save"));
        add_setting(p, settings_find("state_key_load"));
        add_setting(p, settings_find("state_key_next"));
        add_setting(p, settings_find("state_key_prev"));
        add_setting(p, settings_find("states"));
        add(p, TX(T_JKEYS), NULL, 0, A_GO, PG_KEYS, TX(T_JKEYS_H));
        add(p, TX(T_FREECAM), NULL, 0, A_GO, PG_FCKEYS, TX(T_FREECAM_H));
        add_defaults(p);
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    case PG_KEYS:
        p->title = TX(T_HEAD_JKEYS);
        for (int i = 0; i < settings_count(); i++)
            if (settings_row(i)->type == SET_KEY && !strncmp(settings_row(i)->key, "usb_key_", 8))
                add_setting(p, i);
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_GAME, TX(T_BACK_H));
        break;
    case PG_FCKEYS:
        p->title = TX(T_HEAD_FCKEYS);
        add_setting(p, settings_find("freecam_key"));
        add_setting(p, settings_find("freecam_game_input"));
        add_setting(p, settings_find("freecam_speed"));
        add_setting(p, settings_find("freecam_key_pause"));
        add_setting(p, settings_find("freecam_key_step"));
        add_setting(p, settings_find("freecam_key_photo"));
        add_setting(p, settings_find("freecam_key_hide"));
        add_setting(p, settings_find("freecam_pad_toggle"));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_GAME, TX(T_BACK_H));
        break;
    case PG_FOLDERS: {
        p->title = TX(T_HEAD_FOLDERS);
        const char *dd = data_dir();
        char dir[300] = "";
        if (dd) {
            snprintf(dir, sizeof(dir), "%s", dd);
            if (dir[0] && (dir[strlen(dir) - 1] == '/' || dir[strlen(dir) - 1] == '\\') && strlen(dir) > 1)
                dir[strlen(dir) - 1] = 0;
        }
        add(p, TX(T_OPEN_DATA), dir[0] ? dir : TX(T_NONE_SET), dir[0] ? 0 : UR_DIM, A_OPEN_DATA, 0, TX(T_OPEN_DATA_H));
        add(p, TX(T_OPEN_LOGS), TX(T_OPEN_LOGS_V), dir[0] ? 0 : UR_DIM, A_OPEN_LOGS, 0, TX(T_OPEN_LOGS_H));
        log_load();
        if (L.nlog) {
            char n[100];
            snprintf(n, sizeof(n), TX(T_LOG_LINES), L.nlog);
            add(p, TX(T_SHOW_LOG), n, 0, A_SHOWLOG, 0, TX(T_SHOW_LOG_H));
        } else {
            add(p, TX(T_SHOW_LOG), TX(T_NO_LOG), UR_DIM, A_INFO, 0, TX(T_SHOW_LOG_H));
        }
        add_setting(p, settings_find("pictures"));
        const char *sf = settings_save_path();
        add(p, TX(T_SETTINGS_FILE), sf ? sf : TX(T_NONE_SET), UR_DIM, A_INFO, 0, TX(T_SETTINGS_FILE_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    }
    case PG_DISC: {
        p->title = TX(T_HEAD_DISC);
        char v[300], sz[40];
        int st = L.arc.state;
        arc_summary(v, sizeof(v));
        add(p, TX(T_ARC), v, st == ARC_READY ? 0 : UR_WARN, A_INFO, 0,
            st == ARC_READY ? TX(T_DISC_H) : st == ARC_MISSING ? TX(T_ARC_MISSING_H) : st == ARC_BAD ? TX(T_ARC_BAD_H) : TX(T_ARC_NOTABLE_H));
        add(p, TX(T_ARC_FILE), shown(L.arc.path), UR_DIM, A_INFO, 0, TX(T_DISC_H));
        if (st == ARC_READY || st == ARC_NOTABLE) {
            size_text(L.arc.size, sz, sizeof(sz));
            snprintf(v, sizeof(v), TX(T_ARC_SIZE_V), sz, L.arc.nfiles);
            add(p, TX(T_ARC_SIZE), v, UR_DIM, A_INFO, 0, TX(T_ARC_SIZE_H));
        }
        space_text(v, sizeof(v));
        add(p, TX(T_FREE), v, UR_DIM, A_INFO, 0, TX(T_FREE_H));
        int again = st != ARC_MISSING;
        add(p, TX(again ? T_MAKE_ISO_AGAIN : T_MAKE_ISO), NULL, 0, A_PICK_ISO, 0, TX(T_MAKE_ISO_H));
        add(p, TX(again ? T_MAKE_DIR_AGAIN : T_MAKE_DIR), NULL, 0, A_PICK_DIR, 0, TX(T_MAKE_DIR_H));
        if (st == ARC_NOTABLE)
            add(p, TX(T_MAKE_TABLE), NULL, 0, A_MAKE_TABLE, 0, TX(T_MAKE_TABLE_H));
        add(p, TX(T_CHOOSE_ARC), NULL, 0, A_CHOOSE_ARC, 0, TX(T_CHOOSE_ARC_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    }
    case PG_MAKING:
        p->title = TX(T_MAKING);
        if (L.ms.state == MK_DONE)
            add(p, TX(T_MK_CONTINUE), NULL, 0, A_MAKE_DONE, 0, TX(T_MK_CONTINUE_H));
        else
            add(p, TX(T_MK_CANCEL), NULL, 0, A_CANCEL_MAKE, 0, TX(T_MK_CANCEL_H));
        break;
    case PG_CARDS: {
        p->title = TX(T_HEAD_CARDS);
        for (int port = 0; port < 2; port++) {
            const char *names[28];
            int cur, count = port_choices(port, names, 28, &cur);
            const char *c = port_card(port);
            char v[260];
            if (!c)
                snprintf(v, sizeof(v), "%s", TX(T_CARD_NONE));
            else if (!card_listed(c))
                snprintf(v, sizeof(v), TX(T_CARD_NEW), c);
            else
                snprintf(v, sizeof(v), "%s", c);
            int idx = settings_find(port ? "card2" : "card1");
            unsigned fl = UR_ARROWS;
            if (settings_given(idx)) {
                snprintf(v + strlen(v), sizeof(v) - strlen(v), " %s", TX(T_CMDLINE));
                fl = 0;
            } else {
                if (cur <= 0)
                    fl |= UR_NOLEFT;
                if (cur >= count - 1)
                    fl |= UR_NORIGHT;
            }
            add(p, TX(port ? T_PORT2 : T_PORT1), v, fl, A_CARDPORT, port, TX(port ? T_PORT2_H : T_PORT1_H));
        }
        for (int i = 0; i < L.ncards && i < 3; i++) {
            const CardInfo *c = &L.cards[i];
            char when[40], sz[40], v[200];
            cards_time_text(c->mtime, when, sizeof(when));
            snprintf(sz, sizeof(sz), "%.0f MB", (double)c->size / 1048576.0);
            snprintf(v, sizeof(v), TX(T_CARD_INFO), sz, when);
            if (c->in_use)
                snprintf(v + strlen(v), sizeof(v) - strlen(v), ", %s", TX(T_CARD_INUSE));
            add(p, c->name, v, 0, A_INFO, 0, TX(T_CARD_ROW_H));
        }
        if (L.ncards > 3) {
            char more[60];
            snprintf(more, sizeof(more), TX(T_CARD_MORE), L.ncards - 3);
            add(p, more, NULL, UR_DIM, A_INFO, 0, TX(T_CARD_ROW_H));
        }
        add(p, TX(T_NEWCARD), NULL, 0, A_NEWCARD, 0, TX(T_NEWCARD_H));
        int old = cards_old_count(g_cfg.old_card[0], g_cfg.old_card[1]);
        add(p, TX(T_IMPORT), old ? NULL : TX(T_IMPORT_NONE), old ? 0 : UR_DIM, A_IMPORT, 0, old ? TX(T_IMPORT_H) : TX(T_IMPORT_NONE_H));
        add(p, TX(T_OPEN_CARDS), L.have_cdir ? shown(L.cdir) : TX(T_NO_CARDS_DIR), L.have_cdir ? 0 : UR_DIM, A_OPEN_CARDS, 0,
            TX(T_OPEN_CARDS_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    }
    case PG_NEWCARD: {
        p->title = TX(T_HEAD_NEWCARD);
        add(p, TX(T_NC_NAME), L.nc_name, L.sel[PG_NEWCARD] == 0 ? UR_EDIT : 0, A_NC_NAME, 0, TX(T_NC_NAME_H));
        add(p, TX(T_NC_PORT), L.nc_port ? "2" : "1", UR_ARROWS | (L.nc_port ? UR_NORIGHT : UR_NOLEFT), A_NC_PORT, 0, TX(T_NC_PORT_H));
        add(p, TX(T_NC_MAKE), NULL, 0, A_NC_MAKE, 0, TX(T_NC_MAKE_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_CARDS, TX(T_BACK_H));
        break;
    }
    case PG_FIRST1: {
        p->title = TX(T_HEAD_FIRST);
        for (int i = 0; i < L.nsrc && i < 2; i++) {
            char lab[100];
            snprintf(lab, sizeof(lab), TX(T_FIRST_USE), TX(L.src[i].iso ? T_FIRST_USE_ISO : T_FIRST_USE_DIR));
            add(p, lab, shown(L.src[i].path), 0, A_USE_SOURCE, i, TX(T_FIRST_USE_H));
        }
        add(p, TX(T_MAKE_ISO), NULL, 0, A_PICK_ISO, 0, TX(T_MAKE_ISO_H));
        add(p, TX(T_MAKE_DIR), NULL, 0, A_PICK_DIR, 0, TX(T_MAKE_DIR_H));
        char v[100];
        space_text(v, sizeof(v));
        add(p, TX(T_FREE), v, UR_DIM, A_INFO, 0, TX(T_FREE_H));
        add(p, TX(T_FIRST_LATER), NULL, UR_GAP, A_LATER, 0, TX(T_FIRST_LATER_H));
        add(p, TX(T_QUIT), NULL, 0, A_QUIT, 0, TX(T_QUIT_H));
        break;
    }
    case PG_FIRST3: {
        p->title = TX(T_HEAD_FIRST);
        int old = cards_old_count(g_cfg.old_card[0], g_cfg.old_card[1]);
        if (old)
            add(p, TX(T_FIRST_IMPORT), NULL, 0, A_IMPORT, 0, TX(T_IMPORT_H));
        add(p, TX(T_FIRST_NEWCARD), NULL, 0, A_FIRST_NEW, 0, TX(T_FIRST_NEWCARD_H));
        add(p, TX(T_FIRST_LATER), NULL, UR_GAP, A_LATER, 0, TX(T_FIRST_LATER_H));
        break;
    }
    default:
        break;
    }
}

/* ---------------------------------------------------------------- the last log */

static void log_free(void)
{
    for (int i = 0; i < L.nlog; i++)
        free(L.log[i]);
    free(L.log);
    L.log = NULL;
    L.nlog = 0;
}

/* The last 200 lines of mh2.log: the session before this one (the log of this one is opened after the launcher). */
static void log_load(void)
{
    if (L.log)
        return;   /* once a visit to the page: log_free on leaving */
    char **lines = calloc(200, sizeof(char *));
    int n = 0;
    if (!lines)
        return;
    const char *dd = play_data_dir();
    if (dd) {
        char path[1300];
        snprintf(path, sizeof(path), "%smh2.log", dd);
        size_t len = 0;
        char *data = SDL_LoadFile(path, &len);
        if (data) {
            size_t start = 0;
            for (size_t k = 0; k <= len; k++) {
                if (k == len || data[k] == '\n') {
                    size_t e = k;
                    if (e > start && data[e - 1] == '\r')
                        e--;
                    if (k < len || e > start) {
                        if (n == 200) {
                            free(lines[0]);
                            memmove(lines, lines + 1, sizeof(char *) * 199);
                            n = 199;
                        }
                        lines[n++] = SDL_strndup(data + start, e - start);
                    }
                    start = k + 1;
                }
            }
            SDL_free(data);
        }
    }
    L.log = lines;
    L.nlog = n;
}

/* ---------------------------------------------------------------- actions */

/* A folder in the system's file manager. */
static int open_folder(const char *dir)
{
    char url[1500] = "file://";
    size_t u = strlen(url);
    if (dir[0] && dir[1] == ':')   /* C:\... -> file:///C:/... */
        url[u++] = '/';
    for (const char *s = dir; *s && u + 4 < sizeof(url); s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\\')
            c = '/';
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("/._~:-", c))
            url[u++] = (char)c;
        else
            u += (size_t)snprintf(url + u, sizeof(url) - u, "%%%02X", c);
    }
    url[u] = 0;
    return SDL_OpenURL(url);
}

static void set_page(int page)
{
    if (L.page == PG_LOG && page != PG_LOG)
        log_free();
    L.page = page;
    if (page == PG_HOME || page == PG_DISC)
        arc_refresh();
    if (page == PG_HOME || page == PG_CARDS)
        cards_refresh();
}

/* A message over the page, one button. */
static void show_note(const char *text)
{
    snprintf(L.message, sizeof(L.message), "%s", text);
    L.dialog = D_OPENFAIL;
    L.dsel = 0;
}

/* The first row of the page that does ACT is the selected one (the page the player has to use first). */
static void select_act(int act)
{
    Page p;
    build(&p);
    for (int i = 0; i < p.n; i++)
        if (p.row[i].act == act) {
            L.sel[L.page] = i;
            return;
        }
}

static void fail(const char *what, int after)
{
    snprintf(L.message, sizeof(L.message), "%s: %s", TX(T_SAVE_FAIL), what);
    L.dialog = D_FAIL;
    L.dsel = 0;
    L.after = after;
}

/* Save what changed. 0 saved or nothing to save; -1 failed (the dialog is up). */
static int save(int after)
{
    if (!settings_dirty())
        return 0;
    char err[300] = "";
    if (settings_save(err, sizeof(err)) != 0) {
        fail(err, after);
        return -1;
    }
    return 0;
}

static void do_play(void)
{
    if (L.arc.state != ARC_READY) {   /* the help line says why; pressing it says it again */
        show_note(TX(T_PLAY_NOARC));
        return;
    }
    if (save(A_PLAY) != 0)
        return;
    L.done = 1;
    L.result = LAUNCH_PLAY;
}

static void do_quit(void)
{
    if (settings_dirty()) {
        L.dialog = D_SAVE;
        L.dsel = 0;
        L.after = A_QUIT;
        return;
    }
    L.done = 1;
    L.result = LAUNCH_QUIT;
}

/* ---------------------------------------------------------------- making the archive */

static void SDLCALL dlg_cb(void *ud, const char *const *list, int filter)
{
    (void)ud;
    (void)filter;
    if (list && list[0])
        snprintf(dlg.path, sizeof(dlg.path), "%s", list[0]);
    else
        dlg.path[0] = 0;   /* cancelled, or no dialog could open */
    SDL_SetAtomicInt(&dlg.got, 1);
}

/* The system's file or folder dialog (KIND 0 disc image, 1 folder, 2 another archive); its answer is taken in the loop. In
 * the test the script's `pick` is the answer. */
static void begin_pick(int kind)
{
    if (L.pick_wait)
        return;
    L.pick_wait = 1;
    L.pick_kind = kind;
    SDL_SetAtomicInt(&dlg.got, 0);
    static const SDL_DialogFileFilter iso[] = {{"Disc images", "iso"}, {"All files", "*"}};
    static const SDL_DialogFileFilter arc[] = {{"Disc archives", "arc"}, {"All files", "*"}};
    const char *where = L.nsrc ? L.src[0].path : NULL;
    SDL_Window *win = ui_window();
    if (kind == 1)
        SDL_ShowOpenFolderDialog(dlg_cb, NULL, win, where, false);
    else
        SDL_ShowOpenFileDialog(dlg_cb, NULL, win, kind == 2 ? arc : iso, 2, where, false);
}

/* Starts the job (SOURCE NULL: only the movie table) and shows its progress. */
static void make_begin(const char *source)
{
    char err[800];
    if (make_start(source, L.arc.path, L.arc.state != ARC_MISSING, err, sizeof(err)) != 0) {
        show_note(err);
        return;
    }
    snprintf(L.make_source, sizeof(L.make_source), "%s", source ? source : "");
    make_poll(&L.ms);
    L.make_back = L.page == PG_FIRST1 ? PG_FIRST1 : PG_DISC;
    set_page(PG_MAKING);
    L.sel[PG_MAKING] = 0;
}

/* The first run's step 3 follows step 2 unless a card is there already. */
static void first_next(void)
{
    cards_refresh();
    if (L.ncards == 0 && L.have_cdir) {
        set_page(PG_FIRST3);
        L.sel[PG_FIRST3] = 0;
    } else {
        set_page(PG_HOME);
    }
}

static void pick_done(void)
{
    if (!L.pick_wait || !SDL_GetAtomicInt(&dlg.got))
        return;
    L.pick_wait = 0;
    if (!dlg.path[0])
        return;
    if (L.pick_kind == 2) {   /* another archive: it must be one */
        unsigned long long size;
        int n;
        int st = arc_inspect(dlg.path, &size, &n);
        if (st == ARC_MISSING || st == ARC_BAD) {
            show_note(TX(T_ARC_BADFILE));
            return;
        }
        settings_put(settings_find("archive"), dlg.path);
        arc_refresh();
        return;
    }
    make_begin(dlg.path);
}

/* ---------------------------------------------------------------- the memory cards */

static void do_import(void)
{
    char msg[300];
    int copied, left;
    if (!L.have_cdir) {
        show_note(TX(T_IMPORT_FAIL));
        return;
    }
    int rc = cards_import_old(L.cdir, g_cfg.old_card[0], g_cfg.old_card[1], &copied, &left);
    if (rc == 0)
        snprintf(msg, sizeof(msg), TX(T_IMPORT_DONE), copied, left);
    else
        snprintf(msg, sizeof(msg), "%s", TX(T_IMPORT_FAIL));
    cards_refresh();
    if (L.page == PG_FIRST3)
        set_page(PG_HOME);
    show_note(msg);
}

static void card_step(int port, int dir)
{
    int idx = settings_find(port ? "card2" : "card1");
    if (settings_given(idx))
        return;
    const char *names[28];
    int cur, count = port_choices(port, names, 28, &cur);
    int m = cur + dir;
    if (m < 0 || m >= count)
        return;
    settings_put(idx, names[m]);
    ui_bounce(dir);
}

static void do_newcard(void)
{
    char name[100], msg[300];
    snprintf(name, sizeof(name), "%s", L.nc_name);
    size_t n = strlen(name);
    while (n && name[n - 1] == ' ')
        name[--n] = 0;
    if (!n)
        return;
    if (!strchr(name, '.'))
        snprintf(name + n, sizeof(name) - n, ".ps2");
    int idx = settings_find(L.nc_port ? "card2" : "card1");
    const char *other = port_card(!L.nc_port);
    if (!cards_name_ok(name)) {
        snprintf(msg, sizeof(msg), TX(T_NC_BAD), name);
        show_note(msg);
        return;
    }
    if ((L.have_cdir && cards_exists(L.cdir, name)) || (other && !strcmp(other, name))) {
        snprintf(msg, sizeof(msg), TX(T_NC_EXISTS), name);
        show_note(msg);
        return;
    }
    if (settings_given(idx)) {
        snprintf(msg, sizeof(msg), TX(T_NC_FIXED), L.nc_port + 1);
        show_note(msg);
        return;
    }
    settings_put(idx, name);
    cards_refresh();
    set_page(PG_CARDS);
    L.sel[PG_CARDS] = L.nc_port;
}

static void step(const PRow *r, int dir)
{
    if (r->act == A_CARDPORT) {
        card_step(r->arg, dir);
        return;
    }
    if (r->act == A_NC_PORT) {
        int port = dir > 0;
        if (port != L.nc_port) {
            L.nc_port = port;
            ui_bounce(dir);
        }
        return;
    }
    if (r->act != A_SET)
        return;
    if (settings_step(r->arg, dir))
        ui_bounce(dir);
}

static void activate(Page *p, int sel)
{
    const PRow *r = &p->row[sel];
    switch (r->act) {
    case A_SET:   /* confirm moves on to the next choice, and round to the first */
        if (settings_step(r->arg, 1))
            ui_bounce(1);
        else {
            settings_default(r->arg);
            ui_bounce(-1);
        }
        break;
    case A_GO:
        set_page(r->arg);
        L.sel[L.page] = 0;
        if (L.page == PG_DISC && L.arc.state != ARC_READY)
            select_act(A_PICK_ISO);
        break;
    case A_PLAY:
        do_play();
        break;
    case A_QUIT:
        do_quit();
        break;
    case A_BACK:
        set_page(r->arg);
        break;
    case A_DEFAULTS:
        for (int i = 0; i < p->n; i++)
            if (p->row[i].act == A_SET)
                settings_default(p->row[i].arg);
        break;
    case A_OPEN_DATA:
    case A_OPEN_LOGS: {
        const char *dd = data_dir();
        if (dd && *dd
        ) {
            if (!open_folder(dd)) {
                snprintf(L.message, sizeof(L.message), "%s", TX(T_OPEN_FAIL));
                L.dialog = D_OPENFAIL;
                L.dsel = 0;
            }
        }
        break;
    }
    case A_SHOWLOG:
        set_page(PG_LOG);
        L.log_first = L.nlog > UI_LOG_LINES ? L.nlog - UI_LOG_LINES : 0;
        break;
    case A_PICK_ISO:
    case A_PICK_DIR:
        begin_pick(r->act == A_PICK_ISO ? 0 : 1);
        break;
    case A_CHOOSE_ARC:
        begin_pick(2);
        break;
    case A_USE_SOURCE:
        make_begin(L.src[r->arg].path);
        break;
    case A_MAKE_TABLE:
        make_begin(NULL);
        break;
    case A_CANCEL_MAKE:
        make_cancel();
        make_reset();
        arc_refresh();
        set_page(L.make_back);
        break;
    case A_MAKE_DONE:
        make_reset();
        arc_refresh();
        if (L.make_back == PG_FIRST1)
            first_next();
        else
            set_page(L.make_back);
        break;
    case A_LATER:
        set_page(PG_HOME);
        L.sel[PG_HOME] = 0;
        if (L.arc.state != ARC_READY) {   /* the missing archive is what Home is about (6.2) */
            Page hp;
            build(&hp);
            for (int i = 0; i < hp.n; i++)
                if (hp.row[i].act == A_GO && hp.row[i].arg == PG_DISC)
                    L.sel[PG_HOME] = i;
        }
        break;
    case A_NEWCARD:
        if (L.have_cdir)
            cards_default_name(L.cdir, L.nc_name, sizeof(L.nc_name));
        L.nc_port = 0;
        set_page(PG_NEWCARD);
        L.sel[PG_NEWCARD] = 0;
        break;
    case A_IMPORT:
        do_import();
        break;
    case A_OPEN_CARDS:
        if (L.have_cdir
        ) {
            SDL_CreateDirectory(L.cdir);
            if (!open_folder(L.cdir))
                show_note(TX(T_OPEN_FAIL));
        }
        break;
    case A_NC_NAME:
        L.sel[PG_NEWCARD] = 2;
        break;
    case A_NC_MAKE:
        do_newcard();
        break;
    case A_FIRST_NEW:
        if (L.have_cdir)
            SDL_CreateDirectory(L.cdir);
        set_page(PG_HOME);
        L.sel[PG_HOME] = 0;
        break;
    default:
        break;
    }
}

static void dialog_event(const UiEvent *e)
{
    int nb = L.dialog == D_SAVE ? 3 : L.dialog == D_FAIL ? 2 : 1;
    if (e->type == UE_CANCEL && L.dialog == D_MAKEFAIL) {
        L.dsel = 0;
        L.dialog = D_NONE;
        make_reset();
        arc_refresh();
        set_page(L.make_back);
        return;
    }
    switch (e->type) {
    case UE_LEFT:
    case UE_UP:
        L.dsel = (L.dsel + nb - 1) % nb;
        break;
    case UE_RIGHT:
    case UE_DOWN:
        L.dsel = (L.dsel + 1) % nb;
        break;
    case UE_CANCEL:
        L.dialog = D_NONE;
        break;
    case UE_CONFIRM:
        if (L.dialog == D_SAVE) {
            if (L.dsel == 0) {   /* Yes */
                L.dialog = D_NONE;
                if (save(A_QUIT) == 0) {
                    L.done = 1;
                    L.result = LAUNCH_QUIT;
                }
            } else if (L.dsel == 1) {   /* No */
                L.dialog = D_NONE;
                L.done = 1;
                L.result = LAUNCH_QUIT;
            } else {
                L.dialog = D_NONE;
            }
        } else if (L.dialog == D_FAIL) {
            L.dialog = D_NONE;
            if (L.dsel == 0) {   /* go on anyway */
                L.done = 1;
                L.result = L.after == A_PLAY ? LAUNCH_PLAY : LAUNCH_QUIT;
            }
        } else if (L.dialog == D_MAKEFAIL) {
            L.dialog = D_NONE;
            make_reset();
            arc_refresh();
            set_page(L.make_back);
        } else {
            L.dialog = D_NONE;
        }
        break;
    default:
        break;
    }
}

static void log_event(const UiEvent *e)
{
    int fit = UI_LOG_LINES;
    int max_first = L.nlog > fit ? L.nlog - fit : 0;
    switch (e->type) {
    case UE_UP: L.log_first--; break;
    case UE_DOWN: L.log_first++; break;
    case UE_PGUP: L.log_first -= fit - 1; break;
    case UE_PGDN: L.log_first += fit - 1; break;
    case UE_WHEEL: L.log_first -= e->dir * 3; break;
    case UE_CANCEL:
    case UE_CONFIRM:
    case UE_CLICK:
        set_page(PG_FOLDERS);
        return;
    case UE_QUIT:
        do_quit();
        return;
    default:
        return;
    }
    if (L.log_first > max_first)
        L.log_first = max_first;
    if (L.log_first < 0)
        L.log_first = 0;
}

static void handle(const UiEvent *e)
{
    if (L.dialog) {
        dialog_event(e);
        return;
    }
    if (L.page == PG_LOG) {
        log_event(e);
        return;
    }
    Page p;
    build(&p);
    int *sel = &L.sel[L.page];
    if (*sel >= p.n)
        *sel = p.n - 1;
    switch (e->type) {
    case UE_QUIT:
        do_quit();
        break;
    case UE_UP:
        *sel = (*sel + p.n - 1) % p.n;
        break;
    case UE_DOWN:
        *sel = (*sel + 1) % p.n;
        break;
    case UE_HOVER:
        if (e->row >= 0 && e->row < p.n)
            *sel = e->row;
        break;
    case UE_LEFT:
        step(&p.row[*sel], -1);
        break;
    case UE_RIGHT:
        step(&p.row[*sel], 1);
        break;
    case UE_ARROW:
    case UE_WHEEL:
        if (e->row >= 0 && e->row < p.n) {
            *sel = e->row;
            step(&p.row[*sel], e->dir);
        }
        break;
    case UE_CLICK:
        if (e->row >= 0 && e->row < p.n) {
            *sel = e->row;
            activate(&p, *sel);
        }
        break;
    case UE_CONFIRM:
        activate(&p, *sel);
        break;
    case UE_TEXT:
        if (L.page == PG_NEWCARD && *sel == 0)
            for (const char *c = e->text; *c && strlen(L.nc_name) + 2 < 40; c++)
                if (!strchr("/\\:*?\"<>|", *c) && (unsigned char)*c >= 32) {
                    size_t n = strlen(L.nc_name);
                    L.nc_name[n] = *c;
                    L.nc_name[n + 1] = 0;
                }
        break;
    case UE_BACKSPACE:
        if (L.page == PG_NEWCARD && *sel == 0 && L.nc_name[0]) {
            size_t n = strlen(L.nc_name);
            while (n > 1 && (L.nc_name[n - 1] & 0xC0) == 0x80)   /* a whole UTF-8 letter */
                n--;
            L.nc_name[n - 1] = 0;
        }
        break;
    case UE_CANCEL:   /* the game's screens: first to the last row (Back, Quit), then out */
        if (*sel != p.n - 1)
            *sel = p.n - 1;
        else
            activate(&p, *sel);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------- drawing */

/* The progress screen (4.2): where it reads from, the file or movie, a bar. */
static void draw_making(void)
{
    const MakeStatus *m = &L.ms;
    char a[800], lab[80], step[100];
    int first = L.make_back == PG_FIRST1;
    if (first) {
        snprintf(step, sizeof(step), TX(T_FIRST_STEP), 2, TX(T_FIRST_2));
        ui_header_sub(TX(T_HEAD_FIRST), step);
    } else if (m->state == MK_RUNNING && m->stage >= MKS_FILES) {
        snprintf(step, sizeof(step), TX(T_MK_STEP), m->stage == MKS_FILES ? 1 : 2);
        ui_header_sub(TX(T_MAKING), step);
    } else {
        ui_header(TX(T_MAKING));
    }
    if (L.make_source[0]) {
        snprintf(a, sizeof(a), TX(T_MK_READING), shown(L.make_source));
        ui_line(104, 0, a);
    }
    float frac = 0;
    if (m->state == MK_DONE) {
        ui_line(140, 0, TX(T_MK_DONE));
        char sz[40];
        make_size_text(m->total ? m->total : L.arc.size, sz, sizeof(sz));
        snprintf(a, sizeof(a), TX(T_MK_DONE_V), sz, m->nfiles);
        ui_line(170, 1, a);
        frac = 1;
    } else if (m->stage == MKS_FILES && m->file > 0) {
        snprintf(a, sizeof(a), TX(T_MK_FILE_OF), m->file, m->nfiles, m->name);
        ui_line(140, 0, a);
        frac = m->total ? (float)((double)m->done / (double)m->total) : 0;
    } else if (m->stage == MKS_MOVIES && m->movie > 0) {
        snprintf(a, sizeof(a), TX(T_MK_MOVIE_OF), m->movie, m->nmovies);
        ui_line(140, 0, a);
        frac = m->nmovies ? (float)(m->movie - 1) / (float)m->nmovies : 0;
    } else {
        ui_line(140, 0, TX(T_MK_STAGE_SIZE));
    }
    int secs = (int)m->secs;
    if (L.script)
        snprintf(lab, sizeof(lab), "%d%%", (int)(frac * 100));
    else
        snprintf(lab, sizeof(lab), "%d%%  %d:%02d", (int)(frac * 100), secs / 60, secs % 60);
    ui_bar(196, frac, lab);
    if (m->state != MK_DONE)
        ui_line(236, 1, TX(m->stage == MKS_MOVIES ? T_MK_MOVIES_NOTE : T_MK_CHECKING));
    ui_rows_top(280);
}

static void draw(uint64_t *hash, const char *png)
{
    Page p;
    build(&p);
    int sel = L.sel[L.page];
    if (sel >= p.n)
        sel = p.n - 1;
    ui_begin();
    if (L.page == PG_LOG) {
        ui_header(TX(T_HEAD_LOG));
        ui_lines((const char *const *)L.log, L.nlog, L.log_first);
        ui_help(TX(T_LOG_KEYS));
    } else {
        if (L.page == PG_MAKING) {
            draw_making();
        } else if (L.page == PG_FIRST1 || L.page == PG_FIRST3) {
            char step[100];
            snprintf(step, sizeof(step), TX(T_FIRST_STEP), L.page == PG_FIRST1 ? 1 : 3, TX(L.page == PG_FIRST1 ? T_FIRST_1 : T_FIRST_3));
            ui_header_sub(p.title, step);
            ui_para(84, TX(L.page == PG_FIRST1 ? T_FIRST_1_H : T_FIRST_3_H));
            ui_rows_top(172);
        } else if (L.page == PG_HOME) {
            ui_header_sub(p.title, TX(T_TITLE_SUB));
        } else {
            ui_header(p.title);
        }
        ui_rows(p.ui, p.n, sel);
        ui_help(p.row[sel].help);
    }
    if (L.dialog) {
        const char *b3[3] = {TX(T_YES), TX(T_NO), TX(T_BACK)};
        const char *b2[2] = {L.after == A_PLAY ? TX(T_PLAY_ANYWAY) : TX(T_QUIT), TX(T_BACK)};
        const char *b1[1] = {TX(T_OK)};
        if (L.dialog == D_MAKEFAIL)
            ui_message(L.message, b1, 1, 0);
        else if (L.dialog == D_SAVE)
            ui_message(TX(T_SAVE_Q), b3, 3, L.dsel);
        else if (L.dialog == D_FAIL)
            ui_message(L.message, b2, 2, L.dsel);
        else
            ui_message(L.message, b1, 1, 0);
    }
    if (hash && ui_readback(hash, png) != 0)
        *hash = 0;
    ui_end();
}

/* ---------------------------------------------------------------- the test's script (development builds) */


/* ---------------------------------------------------------------- the loop */

int launcher_run(void)
{
    memset(&L, 0, sizeof(L));
    if (ui_open(g_cfg.fullscreen) != 0) {
        host_log("the launcher could not open a window (%s): the game starts without it", ui_error());
        return LAUNCH_NONE;
    }
    settings_session_begin();
    arc_refresh();
    cards_refresh();
    L.nsrc = 0;
        L.nsrc = make_sources(L.src, 6, 0);
    L.first = first_run();
    if (L.first && L.arc.state != ARC_READY) {
        L.page = PG_FIRST1;
    } else if (L.first && L.have_cdir && L.ncards == 0) {
        L.page = PG_FIRST3;
    } else if (L.arc.state != ARC_READY) {   /* the archive is gone (6.2): Home, with the disc row selected */
        L.page = PG_HOME;
        Page hp;
        build(&hp);
        for (int i = 0; i < hp.n; i++)
            if (hp.row[i].act == A_GO && hp.row[i].arg == PG_DISC)
                L.sel[PG_HOME] = i;
    }
    draw(NULL, NULL);   /* the rows are known (the mouse's) before the first event */
    char shot[80] = "";
    while (!L.done) {
        UiEvent e;
        int script_end = 0, script_wait = 0;
        if (L.page == PG_MAKING) {   /* the child's progress, and its end */
            make_poll(&L.ms);
            if (L.ms.state == MK_FAILED && !L.dialog) {
                snprintf(L.message, sizeof(L.message), "%s", L.ms.message);
                L.dialog = D_MAKEFAIL;
                L.dsel = 0;
            }
        }
        pick_done();
        if (script_end)
            break;
        if (script_wait)
            SDL_Delay(5);
        int want_text = L.page == PG_NEWCARD && L.sel[PG_NEWCARD] == 0 && !L.dialog;
        if (want_text != L.text_on) {
            ui_text_input(want_text);
            L.text_on = want_text;
        }
        while (ui_poll(&e, L.script ? 0 : 30)) {
            handle(&e);
            if (L.done)
                break;
        }
        if (L.done)
            break;
        uint64_t hash = 0;
        char png[1100] = "";
        draw(shot[0] ? &hash : NULL, png[0] ? png : NULL);
        if (shot[0]) {
            printf("launcher shot %s %016llx\n", shot, (unsigned long long)hash);
            fflush(stdout);
            shot[0] = 0;
        }
    }
    int result = L.done ? L.result : LAUNCH_QUIT;
    make_reset();
    log_free();
    ui_text_input(0);
    ui_close();
    return result;
}
