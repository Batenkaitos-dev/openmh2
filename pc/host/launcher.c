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
#include "launcher_build.h"
#include "launcher_make.h"
#include "launcher_text.h"
#include "launcher_ui.h"
#include "mh2host.h"
#include "packs.h"
#include "settings.h"

#ifdef _WIN32
#include <windows.h>
#endif

enum { PG_HOME, PG_PICTURE, PG_GAME, PG_KEYS, PG_FOLDERS, PG_LOG, PG_DISC, PG_MAKING, PG_CARDS, PG_NEWCARD, PG_FIRST1,
       PG_FIRST3, PG_FCKEYS, PG_CONTENT, PG_PZKEYS, PG_BUILDING, NPAGES };
enum { A_INFO, A_SET, A_GO, A_PLAY, A_QUIT, A_BACK, A_DEFAULTS, A_OPEN_DATA, A_OPEN_LOGS, A_SHOWLOG,
       A_PICK_ISO, A_PICK_DIR, A_CHOOSE_ARC, A_MAKE_TABLE, A_USE_SOURCE, A_CANCEL_MAKE, A_MAKE_DONE,
       A_CARDPORT, A_NEWCARD, A_IMPORT, A_OPEN_CARDS, A_NC_NAME, A_NC_PORT, A_NC_MAKE, A_FIRST_NEW, A_LATER, A_OPEN_PACKS, A_PACK, A_BUILD, A_CANCEL_BUILD, A_BUILD_DONE };
enum { D_NONE, D_SAVE, D_FAIL, D_OPENFAIL, D_MAKEFAIL, D_PACK, D_BUILDFAIL };
enum { ARC_MISSING, ARC_READY, ARC_BAD, ARC_NOTABLE };

#define MAXROWS UI_MAXROWS
#define PK_MAX 256     /* the most packs read */
#define PK_ROWS 40     /* the most the page lists */
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
    int page, sel[NPAGES], top[NPAGES];   /* top: the first row shown of a page whose rows pass the help */
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
    BuildStatus bs;                  /* the game's build (launcher_build.h) */
    int steps;                       /* the first run's steps: 3, or 4 with the game's build */
    int kit, core_ok;                /* the release's kit is beside mh2; the program it builds is in place and current */
    int build_first, build_back;     /* the build is the first run's step; the page it goes back to */
    int bd_stopping;
    int pick_build;                  /* the open dialog chooses the disc for the build (there is no archive yet) */
    int build_after_make;            /* the archive being made is for the build that follows */
    char build_source[1200];
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
    /* the content page: the texture packs, in load order (packs.h) */
    struct {
        PackInfo info;
        int files;               /* the PNG and DDS files in it (-1: not counted) */
        int keyok;               /* its name can be a key of mh2.ini */
        char author[64];         /* from its pack.txt: author = NAME */
    } pk[PK_MAX];
    int npk, pk_dlg;             /* pk_dlg: the pack the open dialog is about */
    char pk_dir[1200];           /* the packs folder ("" when there is none to look in) */
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

static void arc_refresh(void);

int launcher_wanted(void)
{
    if (g_cfg.launcher < 0 || g_cfg.no_window || g_cfg.import_card)
        return 0;
    if (g_cfg.automated || g_cfg.quit_after)
        return 0;
    arc_refresh();
    if (L.arc.state != ARC_READY)
        return 1;   /* the game cannot start from what is there (no archive: the first run's steps; an archive that is not valid for this
                     * version, or has no movie table: the banner): the core's refusal sends the player to the Game archive page, so it
                     * opens, whatever "skip" says. --no-launcher (and an automated run) was tested above and still wins. */
    if (build_kit_present(NULL, 0) && !build_core_current())
        return 1;   /* the release's game program is not built yet, or by an older kit */
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
    if (value && (act == A_GO || act == A_BUILD))   /* Home's summaries */
        flags |= UR_SUMMARY;
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
    if (!strcmp(settings_text(settings_find("offline_hub")), "on"))
        snprintf(out + strlen(out), cap - strlen(out), ", %s", TX(T_SUM_OFFLINE_HUB));
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
    snprintf(out, cap, "%s in slot 1", c1);
    if (c2)
        snprintf(out + strlen(out), cap - strlen(out), ", %s in slot 2", c2);
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

/* ---------------------------------------------------------------- the texture packs (4.6)

 * The list is packs.c's: the keys of mh2.ini in their order, then the folders with no key. The page edits it through packs_set,
 * so the renderer reads what the page shows. */

/* The packs folder, as the renderer reads it: the textures folder's packs. 0 if there is no textures folder to look in. */
static int packs_root(char *out, size_t cap)
{
    int ti = settings_find("textures");
    const char *t = ti >= 0 ? settings_text(ti) : "";
    if (t && *t) {
        snprintf(out, cap, "%s/packs", t);
        return 1;
    }
    const char *dd = data_dir();
    if (!dd || !*dd)
        return 0;
    snprintf(out, cap, "%stextures/packs", dd);
    return 1;
}

typedef struct FileCount {
    int n, depth;
} FileCount;

static SDL_EnumerationResult SDLCALL count_cb(void *user, const char *dirname, const char *fname)
{
    FileCount *c = (FileCount *)user;
    char path[2300];
    SDL_PathInfo info;
    size_t dl = strlen(dirname);
    snprintf(path, sizeof(path), "%s%s%s", dirname, dl && dirname[dl - 1] != '/' && dirname[dl - 1] != '\\' ? "/" : "", fname);
    if (!SDL_GetPathInfo(path, &info))
        return SDL_ENUM_CONTINUE;
    if (info.type == SDL_PATHTYPE_DIRECTORY) {
        if (c->depth < 8) {
            c->depth++;
            SDL_EnumerateDirectory(path, count_cb, c);
            c->depth--;
        }
    } else if (info.type == SDL_PATHTYPE_FILE) {
        size_t n = strlen(fname);
        if (n > 4 && (!SDL_strcasecmp(fname + n - 4, ".png") || !SDL_strcasecmp(fname + n - 4, ".dds")))
            c->n++;
    }
    return SDL_ENUM_CONTINUE;
}

/* The author a pack names: a line `author = NAME` in the pack.txt in its folder (UTF-8; a BOM and case do not matter). */
static void pack_author(const char *root, const char *name, char *out, size_t cap)
{
    out[0] = 0;
    char path[2300];
    snprintf(path, sizeof(path), "%s/%s/pack.txt", root, name);
    FILE *f = hfs_fopen(path, "rb");
    if (!f)
        return;
    char buf[2049];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = buf;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF)
        p += 3;
    while (*p) {
        char *e = strchr(p, '\n');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char line[300];
        snprintf(line, sizeof(line), "%.*s", (int)(len < sizeof(line) - 1 ? len : sizeof(line) - 1), p);
        char *eq = strchr(line, '=');
        if (eq) {
            *eq = 0;
            char *k = line;
            while (*k == ' ' || *k == '\t')
                k++;
            size_t kl = strlen(k);
            while (kl && (k[kl - 1] == ' ' || k[kl - 1] == '\t'))
                k[--kl] = 0;
            if (!SDL_strcasecmp(k, "author")) {
                char *v = eq + 1;
                while (*v == ' ' || *v == '\t')
                    v++;
                size_t vl = strlen(v);
                while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t' || v[vl - 1] == '\r'))
                    v[--vl] = 0;
                size_t o = 0;
                for (size_t i = 0; v[i] && o + 1 < cap; i++)
                    if ((unsigned char)v[i] >= 32)
                        out[o++] = v[i];
                while (o && (out[o - 1] & 0xC0) == 0x80 && (out[o - 1] & 0x40) == 0)   /* not half a letter */
                    o--;
                out[o] = 0;
                return;
            }
        }
        if (!e)
            break;
        p = e + 1;
    }
}

/* Reads the list again; COUNT: the files and authors too (the page's own visit, not the Home page's summary). */
static void packs_refresh(int count)
{
    L.npk = 0;
    L.pk_dir[0] = 0;
    if (!packs_root(L.pk_dir, sizeof(L.pk_dir)))
        return;
    static PackInfo all[PK_MAX];
    L.npk = packs_list(L.pk_dir, all, PK_MAX);
    for (int i = 0; i < L.npk; i++) {
        L.pk[i].info = all[i];
        L.pk[i].keyok = packs_name_ok(all[i].name);
        L.pk[i].files = -1;
        L.pk[i].author[0] = 0;
        if (count && all[i].present) {
            FileCount c = {0, 0};
            char dir[2300];
            snprintf(dir, sizeof(dir), "%s/%s", L.pk_dir, all[i].name);
            SDL_EnumerateDirectory(dir, count_cb, &c);
            L.pk[i].files = c.n;
            pack_author(L.pk_dir, all[i].name, L.pk[i].author, sizeof(L.pk[i].author));
        }
    }
}

/* The page's list is now the file's (and the renderer's). */
static void packs_commit(void)
{
    static PackInfo mine[PK_MAX];
    for (int i = 0; i < L.npk; i++)
        mine[i] = L.pk[i].info;
    packs_set(mine, L.npk);
}

/* The rows above the packs on the page: Use texture packs, and the renderer's memory budget (texture_preload, texture_cache_mb:
 * the renderer's settings, shown once the settings table has them). */
static int packs_row0(void)
{
    return 1 + (settings_find("texture_preload") >= 0) + (settings_find("texture_cache_mb") >= 0);
}

static int packs_shown(void)
{
    return L.npk < PK_ROWS ? L.npk : PK_ROWS;
}

static void packs_move(int i, int dir)
{
    int j = i + dir;
    if (i < 0 || i >= packs_shown() || j < 0 || j >= packs_shown())
        return;
    typeof(L.pk[0]) t = L.pk[i];
    L.pk[i] = L.pk[j];
    L.pk[j] = t;
    packs_commit();
    L.sel[PG_CONTENT] = packs_row0() + j;
}

static void packs_toggle(int i)
{
    if (i < 0 || i >= L.npk || !L.pk[i].info.present || !L.pk[i].keyok)
        return;
    L.pk[i].info.on = !L.pk[i].info.on;
    packs_commit();
}

static void packs_forget(int i)
{
    if (i < 0 || i >= L.npk || L.pk[i].info.present)
        return;
    for (int k = i; k + 1 < L.npk; k++)
        L.pk[k] = L.pk[k + 1];
    L.npk--;
    packs_commit();
}

static void thousands(int n, char *out, size_t cap)
{
    char d[24];
    snprintf(d, sizeof(d), "%d", n);
    size_t len = strlen(d), o = 0;
    for (size_t i = 0; i < len && o + 2 < cap; i++) {
        if (i && (len - i) % 3 == 0)
            out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

/* The Home page's summary of the Content page. */
static void packs_summary(char *out, size_t cap)
{
    int ti = settings_find("texture_packs");
    int on = 0, present = 0;
    for (int i = 0; i < L.npk; i++)
        if (L.pk[i].info.present) {
            present++;
            on += L.pk[i].info.on;
        }
    if (ti >= 0 && !strcmp(settings_text(ti), "off"))
        snprintf(out, cap, "%s", TX(T_CONTENT_SUM_OFF));
    else if (!present)
        snprintf(out, cap, "%s", TX(T_CONTENT_SUM_NONE));
    else
        snprintf(out, cap, TX(T_CONTENT_SUM), on, present);
}

/* A pack's row: "[x] Name" and, at the right, its author and its files. */
static void add_pack(Page *p, int i)
{
    char lab[PACK_NAME_MAX + 8], val[160], files[40], num[24], by[96];
    const PackInfo *pi = &L.pk[i].info;
    snprintf(lab, sizeof(lab), "[%c] %s", pi->on || !L.pk[i].keyok ? 'x' : ' ', pi->name);
    unsigned fl = 0;
    const char *help = TX(T_PACK_H);
    if (!pi->present) {
        snprintf(val, sizeof(val), "%s", TX(T_PACK_MISSING));
        fl = UR_WARN;
        help = TX(T_PACK_MISSING_H);
    } else if (!L.pk[i].keyok) {
        snprintf(val, sizeof(val), "%s", TX(T_PACK_NOKEY));
        fl = UR_DIM;
        help = TX(T_PACK_NOKEY_H);
    } else {
        if (L.pk[i].files == 1) {
            snprintf(files, sizeof(files), "%s", TX(T_PACK_FILE1));
        } else if (L.pk[i].files > 1) {
            thousands(L.pk[i].files, num, sizeof(num));
            snprintf(files, sizeof(files), TX(T_PACK_FILES), num);
        } else {
            snprintf(files, sizeof(files), "%s", TX(T_PACK_NOFILES));
        }
        by[0] = 0;
        if (L.pk[i].author[0]) {   /* at most 14 letters of it */
            const char *a = L.pk[i].author;
            int cps = 0;
            size_t cut = 0;
            while (a[cut]) {
                if ((a[cut] & 0xC0) != 0x80) {
                    if (cps == 14)
                        break;
                    cps++;
                }
                cut++;
            }
            char who[96];
            snprintf(who, sizeof(who), "%.*s%s", (int)cut, a, a[cut] ? "..." : "");
            snprintf(by, sizeof(by), TX(T_PACK_BY), who);
        }
        snprintf(val, sizeof(val), "%s%s%s", by, by[0] ? ", " : "", files);
        if (!pi->on)
            fl = UR_DIM;
    }
    add(p, lab, val, fl, A_PACK, i, help);
}

/* What is wrong with the game archive, in words: the Home banner, and the reason Play gives. NULL when the archive is ready. */
static const char *banner_text(void)
{
    switch (L.arc.state) {
    case ARC_READY: return NULL;
    case ARC_BAD: return TX(T_BANNER_BAD);
    case ARC_NOTABLE: return TX(T_BANNER_NOTABLE);
    default: return TX(T_BANNER);
    }
}

static const char *play_noarc_text(void)
{
    switch (L.arc.state) {
    case ARC_BAD: return TX(T_PLAY_NOARC_BAD);
    case ARC_NOTABLE: return TX(T_PLAY_NOARC_NOTABLE);
    default: return TX(T_PLAY_NOARC);
    }
}

static void build(Page *p)
{
    memset(p, 0, sizeof(*p));
    char sum[200];
    switch (L.page) {
    case PG_HOME: {
        p->title = TX(T_TITLE);
        int ready = L.arc.state == ARC_READY;
        int can_play = ready && L.core_ok;
        add(p, TX(T_PLAY), NULL, can_play ? 0 : UR_DIM, A_PLAY, 0, can_play ? TX(T_PLAY_H) : ready ? TX(T_PLAY_NOCORE) : play_noarc_text());
        arc_summary(sum, sizeof(sum));
        add(p, TX(T_DISC), sum, ready ? 0 : UR_WARN, A_GO, PG_DISC, TX(T_DISC_H));
        if (L.kit)
            add(p, TX(T_GAMEPROG), L.core_ok ? TX(T_GP_READY) : TX(T_GP_NOT), L.core_ok ? 0 : UR_WARN, A_BUILD, 0, TX(T_GAMEPROG_H));
        cards_summary(sum, sizeof(sum));
        add(p, TX(T_CARDS_PAGE), sum, 0, A_GO, PG_CARDS, TX(T_CARDS_PAGE_H));
        summary_picture(sum, sizeof(sum));
        add(p, TX(T_PICTURE), sum, 0, A_GO, PG_PICTURE, TX(T_PICTURE_H));
        summary_game(sum, sizeof(sum));
        add(p, TX(T_GAME), sum, 0, A_GO, PG_GAME, TX(T_GAME_H));
        packs_summary(sum, sizeof(sum));
        add(p, TX(T_CONTENT), sum, 0, A_GO, PG_CONTENT, TX(T_CONTENT_H));
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
        add_setting(p, settings_find("offline_hub"));
        add_setting(p, settings_find("fast_loading"));
        add_setting(p, settings_find("usb_keyboard"));
        add_setting(p, settings_find("usb_keyboard_toggle"));
        add_setting(p, settings_find("states"));
        add(p, TX(T_JKEYS), NULL, 0, A_GO, PG_KEYS, TX(T_JKEYS_H));
        add(p, TX(T_FREECAM), NULL, 0, A_GO, PG_FCKEYS, TX(T_FREECAM_H));
        add(p, TX(T_PZKEYS), NULL, 0, A_GO, PG_PZKEYS, TX(T_PZKEYS_H));
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
        add_setting(p, settings_find("camera_stick"));
        add_setting(p, settings_find("camera_stick_invert"));
        add_setting(p, settings_find("camera_stick_speed"));
        add(p, TX(T_FCOTHER), "mh2.ini", UR_DIM, A_INFO, 0, TX(T_FCOTHER_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_GAME, TX(T_BACK_H));
        break;
    case PG_CONTENT: {
        p->title = TX(T_HEAD_CONTENT);
        add_setting(p, settings_find("texture_packs"));
        if (settings_find("texture_preload") >= 0)
            add_setting(p, settings_find("texture_preload"));
        if (settings_find("texture_cache_mb") >= 0)
            add_setting(p, settings_find("texture_cache_mb"));
        int shown = packs_shown();
        for (int i = 0; i < shown; i++)
            add_pack(p, i);
        if (!L.npk)
            add(p, TX(T_PACKS_NONE), L.pk_dir[0] ? NULL : TX(T_PACKS_NOFOLDER), UR_DIM, A_INFO, 0, TX(T_PACKS_NONE_H));
        if (L.npk > shown) {
            char more[80];
            snprintf(more, sizeof(more), TX(T_PACK_MORE), L.npk - shown);
            add(p, more, NULL, UR_DIM, A_INFO, 0, TX(T_PACK_MORE_H));
        }
        add(p, TX(T_OPEN_PACKS), NULL, (L.pk_dir[0] ? 0 : UR_DIM) | UR_GAP, A_OPEN_PACKS, 0, TX(T_OPEN_PACKS_H));
        add(p, TX(T_BACK), NULL, UR_GAP, A_BACK, PG_HOME, TX(T_BACK_H));
        break;
    }
    case PG_PZKEYS:
        p->title = TX(T_HEAD_PZKEYS);
        add_setting(p, settings_find("state_key_save"));
        add_setting(p, settings_find("state_key_load"));
        add_setting(p, settings_find("state_key_next"));
        add_setting(p, settings_find("state_key_prev"));
        add_setting(p, settings_find("pace_key_pause"));
        add_setting(p, settings_find("pace_key_step"));
        add_setting(p, settings_find("pace_key_fast"));
        add_setting(p, settings_find("fast_forward_cap"));
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
    case PG_BUILDING:
        p->title = TX(T_BUILD);
        if (L.bs.state == BD_DONE)
            add(p, TX(T_MK_CONTINUE), NULL, 0, A_BUILD_DONE, 0, TX(T_MK_CONTINUE_H));
        else
            add(p, TX(L.bs.state == BD_RUNNING && L.bd_stopping ? T_BD_STOPPING : T_BD_CANCEL), NULL, 0, A_CANCEL_BUILD, 0, TX(T_BD_CANCEL_H));
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
    L.top[page] = 0;
    if (page != PG_MAKING && page != PG_BUILDING)
        L.build_after_make = 0;
    if (page == PG_HOME || page == PG_DISC)
        arc_refresh();
    if (page == PG_HOME || page == PG_CARDS)
        cards_refresh();
    if (page == PG_HOME || page == PG_CONTENT)
        packs_refresh(page == PG_CONTENT);
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
        show_note(play_noarc_text());
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
    static const SDL_DialogFileFilter arc[] = {{"Game archives", "arc"}, {"All files", "*"}};
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

/* Starts the game's build from SOURCE and shows its progress. */
static void build_begin(const char *source, int first)
{
    char err[800];
    if (build_start(source, err, sizeof(err)) != 0) {
        show_note(err);
        return;
    }
    snprintf(L.build_source, sizeof(L.build_source), "%s", source);
    L.build_first = first;
    L.build_back = L.page == PG_BUILDING ? L.build_back : L.page;
    L.bd_stopping = 0;
    build_poll(&L.bs);
    set_page(PG_BUILDING);
    L.sel[PG_BUILDING] = 0;
}

/* The build reads the archive of the player's game files (the disc is not needed again, after an update either). Without one, the
 * player chooses the disc first, and the archive is made. */
static void build_from_home(void)
{
    if (L.arc.state != ARC_MISSING && L.arc.state != ARC_BAD) {
        build_begin(L.arc.path, 0);
        return;
    }
    L.pick_build = 1;
    begin_pick(0);
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
    if (!dlg.path[0]) {
        L.pick_build = 0;
        return;
    }
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
    if (L.pick_build) {
        L.pick_build = 0;
        L.build_after_make = 1;   /* the archive first, then the game's program */
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
    if (r->act == A_PACK) {
        packs_toggle(r->arg);
        ui_bounce(dir);
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
        if (L.kit && !L.core_ok && L.arc.state != ARC_MISSING && L.arc.state != ARC_BAD && (L.make_back == PG_FIRST1 || L.build_after_make)) {
            L.build_after_make = 0;
            build_begin(L.arc.path, L.make_back == PG_FIRST1);   /* the game's program, built from the archive just made */
        } else if (L.make_back == PG_FIRST1)
            first_next();
        else
            set_page(L.make_back);
        break;
    case A_BUILD:
        build_from_home();
        break;
    case A_CANCEL_BUILD:
        if (!L.bd_stopping) {
            L.bd_stopping = 1;
            build_cancel();
        }
        break;
    case A_BUILD_DONE:
        build_reset();
        L.core_ok = build_core_current();
        if (L.build_first)
            first_next();
        else
            set_page(L.build_back == PG_BUILDING ? PG_HOME : L.build_back);
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
    case A_OPEN_PACKS:
        if (L.pk_dir[0]
        ) {
            char root[1200];
            snprintf(root, sizeof(root), "%s", L.pk_dir);
            SDL_CreateDirectory(root);   /* (textures/packs: both are made) */
            char *slash = strrchr(root, '/');
            if (slash) {
                *slash = 0;
                SDL_CreateDirectory(root);
                SDL_CreateDirectory(L.pk_dir);
            }
            if (!open_folder(L.pk_dir))
                show_note(TX(T_OPEN_FAIL));
        }
        break;
    case A_PACK:
        if (r->arg < L.npk && (L.pk[r->arg].keyok || !L.pk[r->arg].info.present)) {
            L.pk_dlg = r->arg;
            snprintf(L.message, sizeof(L.message), "%s", L.pk[r->arg].info.name);
            L.dialog = D_PACK;
            L.dsel = 0;
        }
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

static void pack_dialog_act(void)
{
    int i = L.pk_dlg;
    if (i >= 0 && i < L.npk) {
        if (L.dsel == 0) {
            if (L.pk[i].info.present)
                packs_toggle(i);
            else
                packs_forget(i);
        } else {
            packs_move(i, L.dsel == 1 ? -1 : 1);
        }
    }
    L.dialog = D_NONE;
}

static void dialog_event(const UiEvent *ev)
{
    UiEvent click = *ev;
    const UiEvent *e = ev;
    int nb = L.dialog == D_SAVE || L.dialog == D_PACK || L.dialog == D_BUILDFAIL ? 3 : L.dialog == D_FAIL ? 2 : 1;
    if (ev->type == UE_HOVER) {   /* the mouse over a button */
        if (ev->row >= 0 && ev->row < nb)
            L.dsel = ev->row;
        return;
    }
    if (ev->type == UE_CLICK) {   /* a click on a button is Enter on it */
        if (ev->row < 0 || ev->row >= nb)
            return;
        L.dsel = ev->row;
        click.type = UE_CONFIRM;
        e = &click;
    }
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
        if (L.dialog == D_PACK) {
            pack_dialog_act();
        } else if (L.dialog == D_SAVE) {
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
        } else if (L.dialog == D_BUILDFAIL) {
            L.dialog = D_NONE;
            if (L.dsel == 1) {   /* the logs folder; the dialog stays */
                char dir[1300];
                snprintf(dir, sizeof(dir), "%s", build_log_path());
                char *sl = strrchr(dir, '/');
                char *bs = strrchr(dir, '\\');
                if (bs > sl)
                    sl = bs;
                if (sl)
                    *sl = 0;
                SDL_OpenURL(dir);
                L.dialog = D_BUILDFAIL;
            } else {
                build_reset();
                if (L.dsel == 0) {   /* try again, from the same disc */
                    const char *src = L.build_source;
                    int first = L.build_first;
                    char keep[1200];
                    snprintf(keep, sizeof(keep), "%s", src);
                    set_page(L.build_back == PG_BUILDING ? PG_HOME : L.build_back);
                    build_begin(keep, first);
                } else {
                    set_page(PG_HOME);
                    L.sel[PG_HOME] = 0;
                }
            }
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

/* Page Up and Page Down: a page of rows up or down (to the first or last row on a page that fits). */
static void page_jump(const Page *p, int dir)
{
    int *sel = &L.sel[L.page];
    int fit = ui_rows_fit(p->ui, p->n, L.top[L.page]);
    int to = *sel + dir * (fit > 1 ? fit : 1);
    *sel = to < 0 ? 0 : to > p->n - 1 ? p->n - 1 : to;
}

/* The wheel on a page with rows out of sight: DIR 1 (up) shows earlier rows. The selection stays on a row in view. */
static void scroll_view(const Page *p, int dir)
{
    int *sel = &L.sel[L.page];
    int first = L.top[L.page] - dir;
    first = first < 0 ? 0 : first > p->n - 1 ? p->n - 1 : first;
    first = ui_rows_scroll(p->ui, p->n, first, first);
    int fit = ui_rows_fit(p->ui, p->n, first);
    L.top[L.page] = first;
    if (*sel < first)
        *sel = first;
    else if (fit > 0 && *sel >= first + fit)
        *sel = first + fit - 1;
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
    case UE_PGUP:
    case UE_PGDN:
        page_jump(&p, e->type == UE_PGUP ? -1 : 1);
        break;
    case UE_WHEEL:
        if (ui_rows_fit(p.ui, p.n, 0) < p.n) {   /* rows out of sight: the wheel scrolls (it changes no value) */
            scroll_view(&p, e->dir);
            break;
        }
        if (e->row >= 0 && e->row < p.n) {
            *sel = e->row;
            step(&p.row[*sel], e->dir);
        }
        break;
    case UE_ARROW:
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
        snprintf(step, sizeof(step), TX(T_FIRST_STEP), 2, L.steps, TX(T_FIRST_2));
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

/* The build's progress screen: the step it is in, the compiler's count, a bar, the time. */
static void draw_building(void)
{
    const BuildStatus *b = &L.bs;
    char a[300], lab[80], step[100];
    if (L.build_first) {
        snprintf(step, sizeof(step), TX(T_FIRST_STEP), 3, L.steps, TX(T_FIRST_BUILD));
        ui_header_sub(TX(T_HEAD_FIRST), step);
    } else {
        ui_header(TX(T_BUILD));
    }
    ui_para(84, TX(T_BD_INTRO));
    float frac = 0;
    if (b->state == BD_DONE) {
        ui_line(150, 0, TX(T_BD_DONE));
        ui_line(180, 1, TX(T_BD_DONE_V));
        frac = 1;
    } else if (b->step > 0) {
        snprintf(a, sizeof(a), TX(T_BD_STEP), b->step, b->nsteps, b->step_text);
        ui_line(150, 0, a);
        float inner = b->total > 0 ? (float)b->done / (float)b->total : 0;
        frac = ((float)(b->step - 1) + inner) / (float)(b->nsteps ? b->nsteps : 5);
    } else {
        ui_line(150, 0, TX(T_BD_START));
    }
    int secs = (int)b->secs;
    if (L.script)
        snprintf(lab, sizeof(lab), "%d%%", (int)(frac * 100));
    else
        snprintf(lab, sizeof(lab), "%d%%  %d:%02d", (int)(frac * 100), secs / 60, secs % 60);
    ui_bar(206, frac, lab);
    if (b->state != BD_DONE)
        ui_line(246, 1, TX(T_BD_NOTE));
    ui_rows_top(290);
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
        if (L.page == PG_BUILDING) {
            draw_building();
        } else if (L.page == PG_MAKING) {
            draw_making();
        } else if (L.page == PG_FIRST1 || L.page == PG_FIRST3) {
            char step[100];
            snprintf(step, sizeof(step), TX(T_FIRST_STEP), L.page == PG_FIRST1 ? 1 : L.steps, L.steps, TX(L.page == PG_FIRST1 ? T_FIRST_1 : T_FIRST_3));
            ui_header_sub(p.title, step);
            ui_para(84, TX(L.page == PG_FIRST1 ? T_FIRST_1_H : T_FIRST_3_H));
            ui_rows_top(172);
        } else if (L.page == PG_HOME) {
            const char *banner = banner_text();
            if (!banner)
                ui_header_sub(p.title, TX(T_TITLE_SUB));
            else
                ui_header_warn(p.title, banner);   /* what is wrong with the archive: the banner, in the subtitle's place */
        } else {
            ui_header(p.title);
        }
        L.top[L.page] = ui_rows_scroll(p.ui, p.n, sel, L.top[L.page]);
        ui_rows_from(L.top[L.page]);
        ui_rows(p.ui, p.n, sel);
        ui_help(p.row[sel].help);
    }
    if (L.dialog) {
        const char *b3[3] = {TX(T_YES), TX(T_NO), TX(T_BACK)};
        const char *b2[2] = {L.after == A_PLAY ? TX(T_PLAY_ANYWAY) : TX(T_QUIT), TX(T_BACK)};
        const char *b1[1] = {TX(T_OK)};
        const char *b3p[3] = {TX(T_PACK_OFF), TX(T_PACK_UP), TX(T_PACK_DOWN)};
        if (L.dialog == D_PACK) {
            int i = L.pk_dlg;
            if (i >= 0 && i < L.npk)
                b3p[0] = !L.pk[i].info.present ? TX(T_PACK_FORGET) : L.pk[i].info.on ? TX(T_PACK_OFF) : TX(T_PACK_ON);
            ui_message(L.message, b3p, 3, L.dsel);
        } else if (L.dialog == D_MAKEFAIL)
            ui_message(L.message, b1, 1, 0);
        else if (L.dialog == D_BUILDFAIL) {
            const char *bb[3] = {TX(T_BD_AGAIN), TX(T_BD_LOGS), TX(T_BD_LATER)};
            ui_message(L.message, bb, 3, L.dsel);
        }
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
    packs_refresh(0);
    L.nsrc = 0;
        L.nsrc = make_sources(L.src, 6, 0);
    L.first = first_run();
    L.kit = build_kit_present(NULL, 0);
    L.core_ok = !L.kit || build_core_current();
    L.steps = L.kit ? 4 : 3;
    if (L.first && L.arc.state != ARC_READY) {
        L.page = PG_FIRST1;
        L.sel[PG_HOME] = 0;
    } else if (L.kit && !L.core_ok && L.arc.state == ARC_READY) {   /* the game's program is not there: Home, with its row selected */
        L.page = PG_HOME;
        Page hp;
        build(&hp);
        for (int i = 0; i < hp.n; i++)
            if (hp.row[i].act == A_BUILD)
                L.sel[PG_HOME] = i;
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
        if (L.page == PG_BUILDING) {
            build_poll(&L.bs);
            if (L.bs.state == BD_FAILED && !L.dialog) {
                snprintf(L.message, sizeof(L.message), "%s", L.bs.message);
                L.dialog = D_BUILDFAIL;
                L.dsel = 0;
            } else if (L.bs.state == BD_CANCELLED) {
                build_reset();
                L.bd_stopping = 0;
                set_page(PG_HOME);
                L.sel[PG_HOME] = 0;
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
