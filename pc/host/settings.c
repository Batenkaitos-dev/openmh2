/*
 * The settings file, mh2.ini, and the launcher's choices for the core, as the
 * original's: the same choices as the command line,
 * which overrides it, plus the key and gamepad bindings.
 *
 * Found at (the first that exists): --config FILE; mh2.ini next to mh2 (a
 * portable setup); mh2.ini in the user data folder (next to the memcards
 * folder; never in an automated run, main.c's guard). It is never created
 * by the program. --set KEY=VALUE options are applied after it. Lines are `key = value`;
 * `#` and `;` start comments; `[section]` lines are only for readability.
 * The keys:
 *   archive = FILE        the disc archive (mh2-core's --disc)
 *   cards = DIR           mh2's own cards' folder (cards.c; default: the user
 *                         data folder's cards)
 *   card1, card2 = NAME   the cards in it for ports 1 and 2 (Mcd001.ps2, none)
 *   memcard1, memcard2    cards of an earlier setup: offered for import at
 *                         mh2's first start, never written (cards.c)
 *   fullscreen, mute      as --fullscreen and --mute
 *   key.NAME, pad.NAME    the bindings (input.c, input_bind)
 *   vsync = auto|on|off   as --vsync (auto: on in fullscreen only)
 *   ssaa = N, hires = on  the enhancements (off by default; as --ssaa N and
 *                         --hires: PLAYING.md), drawn by the renderer only;
 *   widescreen = on       the 16:9 view (as --widescreen: PLAYING.md). Unlike the two above it changes the
 *                         game's state: it is the core's --widescreen; the host shows the picture at 16:9,
 *                         the movies at 4:3. Off by default.
 *   orig, any_core: the old build's, not needed by mh2 (it says why)
 *
 * The launcher: mh2-core gets --disc (mh2's --archive, the setting, else
 * mh2-disc.arc next to mh2, in the user data folder, or the build tree's
 * build/port), and the cards. None of this reaches the game but the files
 * the core opens.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

static char g_msgs[2048];   /* logged after the options are read */
static size_t g_nmsgs;

static struct {
    char archive[1024];
    char memcard[2][1024];
    char cards[1024], card[2][256];
    int fullscreen, mute, vsync;
    int ssaa, hires, widescreen;
} st = {.fullscreen = -1, .mute = -1, .vsync = -2};

static void note(const char *fmt, ...) SDL_PRINTF_VARARG_FUNC(1);
static void note(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_msgs + g_nmsgs, sizeof(g_msgs) - g_nmsgs, fmt, ap);
    va_end(ap);
    if (n > 0 && g_nmsgs + (size_t)n + 1 < sizeof(g_msgs)) {
        g_nmsgs += (size_t)n;
        g_msgs[g_nmsgs++] = '\n';
        g_msgs[g_nmsgs] = 0;
    }
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

static int flag(const char *v)
{
    return !strcmp(v, "1") || !SDL_strcasecmp(v, "on") || !SDL_strcasecmp(v, "yes") || !SDL_strcasecmp(v, "true");
}

/* Where a setting came from: its line in the file, or --set (LINE 0). */
static const char *at(int line)
{
    static char buf[24];
    if (!line)
        return "--set";
    snprintf(buf, sizeof(buf), "line %d", line);
    return buf;
}

static void apply(const char *key, const char *v, int line)
{
    if (!strcmp(key, "archive")) {
        snprintf(st.archive, sizeof(st.archive), "%s", v);
    } else if (!strcmp(key, "memcard1") || !strcmp(key, "memcard2")) {
        snprintf(st.memcard[key[7] - '1'], sizeof(st.memcard[0]), "%s", v);
    } else if (!strcmp(key, "fullscreen")) {
        st.fullscreen = flag(v);
    } else if (!strcmp(key, "mute")) {
        st.mute = flag(v);
    } else if (!strcmp(key, "cards")) {
        snprintf(st.cards, sizeof(st.cards), "%s", v);
    } else if (!strcmp(key, "card1") || !strcmp(key, "card2")) {
        snprintf(st.card[key[4] - '1'], sizeof(st.card[0]), "%s", v);
    } else if (!strcmp(key, "vsync")) {
        st.vsync = !SDL_strcasecmp(v, "auto") ? -1 : flag(v);
    } else if (!strcmp(key, "ssaa")) {
        int n = atoi(v);
        if (n == 1 || n == 2 || n == 4 || n == 8 || n == 16)
            st.ssaa = n;
        else
            note("%s: ssaa is 1 (off), 2, 4, 8 or 16: ignored", at(line));
    } else if (!strcmp(key, "hires")) {
        st.hires = flag(v);
    } else if (!strcmp(key, "widescreen")) {
        st.widescreen = flag(v);
    } else if (!strcmp(key, "orig")) {
        note("%s: \"orig\" (the original files) is the original's: mh2 reads the disc archive only", at(line));
    } else if (!strcmp(key, "any_core")) {
        note("%s: \"any_core\" is the original's: mh2 doesn't place its threads", at(line));
    } else if (!strncmp(key, "key.", 4) || !strncmp(key, "pad.", 4)) {
        int r = input_bind(key, v);
        if (r == -1)
            note("%s: no button \"%s\"", at(line), key + 4);
        else if (r == -2)
            note("%s: \"%s\" is not a %s name", at(line), v, key[0] == 'k' ? "key" : "gamepad button");
    } else {
        note("%s: unknown setting \"%s\"", at(line), key);
    }
}

/* One `key = value` line (comments and blanks gone); LINE 0: --set's. */
static void apply_line(char *s, int line)
{
    char *eq = strchr(s, '=');
    if (!eq) {
        if (line)
            note("line %d: expected key = value", line);
        else
            note("--set %s: expected KEY=VALUE", s);
        return;
    }
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    for (char *p = key; *p; p++)
        *p = (char)SDL_tolower((unsigned char)*p);
    apply(key, val, line);
}

static int load(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    note("settings from %s", path);
    char buf[512];
    int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        char *s = trim(buf);
        char *c = strpbrk(s, "#;");
        if (c)
            *c = 0;
        s = trim(s);
        if (!*s || *s == '[')
            continue;
        apply_line(s, line);
    }
    fclose(f);
    return 1;
}

/* --set KEY=VALUE, in order: applied after the settings file, so they win over it. */
static char *g_sets[32];
static int g_nsets;

void settings_set(const char *line)
{
    if (g_nsets < 32)
        g_sets[g_nsets++] = SDL_strdup(line);
    else
        note("--set %s: more than 32 --set options: ignored", line);
}

static void apply_sets(void)
{
    for (int i = 0; i < g_nsets; i++) {
        char *s = trim(g_sets[i]);
        if (*s)
            apply_line(s, 0);
    }
}

/* An automated run (g_cfg.automated) reads --config's file or the one next to mh2, never the user
 * data folder's: SDL_GetPrefPath alone would make that folder. */
void settings_load(const char *explicit_path)
{
    if (explicit_path) {
        if (!load(explicit_path))
            note("cannot read the settings file %s", explicit_path);
        apply_sets();
        return;
    }
    char path[1200];
    const char *base = SDL_GetBasePath();
    if (base) {
        snprintf(path, sizeof(path), "%smh2.ini", base);
        if (load(path)) {
            apply_sets();
            return;
        }
    }
    if (g_cfg.automated) {
        apply_sets();
        return;
    }
    char *pref = SDL_GetPrefPath("mh2-decomp", "mh2");
    if (pref) {
        snprintf(path, sizeof(path), "%smh2.ini", pref);
        SDL_free(pref);
        load(path);
    }
    apply_sets();
}

/* The settings under the command line's choices (which win). */
void settings_apply(int cli_fullscreen, int cli_mute)
{
    if (!cli_fullscreen && st.fullscreen > 0)
        g_cfg.fullscreen = 1;
    if (!cli_mute && st.mute > 0)
        g_cfg.mute = 1;
    if (st.cards[0])
        g_cfg.cards_dir = st.cards;
    for (int p = 0; p < 2; p++) {
        if (st.card[p][0])
            g_cfg.card[p] = st.card[p];
        if (st.memcard[p][0])
            g_cfg.old_card[p] = st.memcard[p];
    }
    if (g_cfg.vsync == -2)   /* not on the command line */
        g_cfg.vsync = st.vsync == -2 ? -1 : st.vsync;
    /* the enhancements: the command line's ssaa wins; --hires or hires = on; hires implies ssaa 4 at least */
    if (!g_cfg.ssaa)
        g_cfg.ssaa = st.ssaa;
    g_cfg.hires |= st.hires > 0;
    if (g_cfg.hires && g_cfg.ssaa < 4)
        g_cfg.ssaa = 4;
    if (g_cfg.ssaa == 1)
        g_cfg.ssaa = 0;
    if (st.widescreen > 0)
        g_cfg.widescreen = 2;   /* on is the newest level */
}

const char *settings_enhancements(void)
{
    static char b[160];
    if (g_cfg.ssaa_asked) {   /* gs.c: the renderer could not make the samples and runs at one */
        snprintf(b, sizeof(b), "off (ssaa %d%s asked: the renderer could not make the samples; one sample)", g_cfg.ssaa_asked,
                 g_cfg.hires_asked ? ", hires" : "");
        return b;
    }
    if (g_cfg.ssaa < 2 && !g_cfg.hires)
        return "off";
    snprintf(b, sizeof(b), "ssaa %d, hires %s", g_cfg.ssaa < 2 ? 1 : g_cfg.ssaa, g_cfg.hires ? "on" : "off");
    return b;
}

const char *settings_widescreen(void)
{
    return g_cfg.widescreen == 2 ? "on" : g_cfg.widescreen ? "on, level 1 (the camera only)" : "off";
}

void settings_log(void)
{
    char *s = g_msgs;
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = 0;
        host_log("%s", s);
        if (!nl)
            break;
        s = nl + 1;
    }
}

/* ------------------------------------------------------------ the launcher */

/* NAME alone before the core's own arguments. */
void launcher_core_flag(const char *name)
{
    char **args = malloc(sizeof(char *) * (size_t)(g_cfg.core_nargs + 2));
    if (!args)
        return;
    args[0] = SDL_strdup(name);
    for (int i = 0; i < g_cfg.core_nargs; i++)
        args[1 + i] = g_cfg.core_args[i];
    args[1 + g_cfg.core_nargs] = NULL;
    g_cfg.core_args = args;
    g_cfg.core_nargs += 1;
}

static int has_arg(const char *name)
{
    for (int i = 0; i < g_cfg.core_nargs; i++)
        if (!strcmp(g_cfg.core_args[i], name))
            return 1;
    return 0;
}

/* NAME VALUE before the core's own arguments (which win: the core takes the last). */
void launcher_core_arg(const char *name, const char *value)
{
    char **args = malloc(sizeof(char *) * (size_t)(g_cfg.core_nargs + 3));
    if (!args)
        return;
    args[0] = SDL_strdup(name);
    args[1] = SDL_strdup(value);
    for (int i = 0; i < g_cfg.core_nargs; i++)
        args[2 + i] = g_cfg.core_args[i];
    args[2 + g_cfg.core_nargs] = NULL;
    g_cfg.core_args = args;
    g_cfg.core_nargs += 2;
}

static int is_file(const char *path)
{
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* The disc archive for the core: the core's --disc, mh2's --archive, the setting, or the first
 * found; 0 (--disc given to the core and g_cfg.disc_path set), or -1 (none). */
int launcher_disc(void)
{
    static char path[1200];
    if (has_arg("--disc")) {
        for (int i = 0; i + 1 < g_cfg.core_nargs; i++)
            if (!strcmp(g_cfg.core_args[i], "--disc"))
                snprintf(path, sizeof(path), "%s", g_cfg.core_args[i + 1]);
    } else if (g_cfg.archive) {
        snprintf(path, sizeof(path), "%s", g_cfg.archive);
    } else if (st.archive[0]) {
        snprintf(path, sizeof(path), "%s", st.archive);
    } else {
        const char *base = SDL_GetBasePath();
        char *pref = g_cfg.automated ? NULL : SDL_GetPrefPath("mh2-decomp", "mh2");   /* (the guard, main.c) */
        path[0] = 0;
        if (base) {
            snprintf(path, sizeof(path), "%smh2-disc.arc", base);
            if (!is_file(path) && pref)
                snprintf(path, sizeof(path), "%smh2-disc.arc", pref);
            if (!is_file(path))   /* the build tree: build/pc/<platform>-x86_64/mh2 -> build/port */
                snprintf(path, sizeof(path), "%s../../port/mh2-disc.arc", base);
        }
        SDL_free(pref);
        if (!path[0] || !is_file(path))
            return -1;
    }
    if (!is_file(path)) {
        host_log("the disc archive %s is not a file", path);
        return -1;
    }
    if (!has_arg("--disc"))
        launcher_core_arg("--disc", path);
    g_cfg.disc_path = path;   /* MH2_DISC too (process.c): io's default archive; io finds the movie table beside it */
    host_log("disc archive: %s", path);
    return 0;
}


/* Widescreen reaches the core as its own option, --widescreen. */
void launcher_widescreen(void)
{
    if (has_arg("--widescreen"))
        g_cfg.widescreen = 2;
    if (!g_cfg.widescreen)
        return;
    if (g_cfg.widescreen == 2 && !has_arg("--widescreen"))
        launcher_core_flag("--widescreen");
    host_log("widescreen: on (the camera and the 2D picture are 16:9; movies are shown at 4:3)");
}

