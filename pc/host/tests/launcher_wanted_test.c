/*
 * When the launcher appears (launcher.c, launcher_wanted): at every start until "skip" is ticked; with --launcher, or with
 * Shift held; never in an automated run (--no-window, --quit-after, a test's options, the core's own arguments, --import-card)
 * and never with --no-launcher; with no archive, an archive that is not valid for this version or one with no movie table it always opens,
 * even with launcher = off (the first run, or the banner). launcher.c and settings.c are compiled into this test
 * with stubs for the window and the rest of the host.
 *
 *   launcher_wanted_test DIR      (DIR: a folder to put its files in)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "launcher.h"
#include "mh2host.h"
#include "settings.h"

HostConfig g_cfg;

void host_log(const char *fmt, ...)
{
    (void)fmt;
}
int input_bind(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}
int kbd_setting(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}
const char *kbd_key_name(const char *key)
{
    (void)key;
    return "none";
}
int states_setting(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}
const char *states_key_text(const char *key)
{
    (void)key;
    return "none";
}
int freecam_setting(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}
const char *freecam_key_text(const char *key)
{
    (void)key;
    return "none";
}
int pace_setting(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return 0;
}
const char *pace_key_text(const char *key)
{
    (void)key;
    return "none";
}
const char *play_data_dir(void)
{
    return NULL;
}
/* the window: not used here */
#include "launcher_ui.h"
int ui_open(int fullscreen)
{
    (void)fullscreen;
    return -1;
}
const char *ui_error(void)
{
    return "";
}
void ui_close(void) {}
int ui_poll(UiEvent *e, int wait_ms)
{
    (void)e;
    (void)wait_ms;
    return 0;
}
int ui_fullscreen(void)
{
    return 0;
}
void ui_begin(void) {}
void ui_header(const char *t)
{
    (void)t;
}
void ui_rows(const UiRow *r, int n, int s)
{
    (void)r;
    (void)n;
    (void)s;
}
void ui_help(const char *t)
{
    (void)t;
}
int ui_lines(const char *const *l, int n, int f)
{
    (void)l;
    (void)n;
    (void)f;
    return 0;
}
void ui_message(const char *t, const char *const *b, int n, int s)
{
    (void)t;
    (void)b;
    (void)n;
    (void)s;
}
void ui_end(void) {}
int ui_cuts(void) { return 0; }
int ui_readback(uint64_t *h, const char *p)
{
    (void)h;
    (void)p;
    return -1;
}
void ui_window_point(float x, float y, float *wx, float *wy)
{
    *wx = x;
    *wy = y;
}
int ui_rows_visible(void)
{
    return 0;
}
void ui_set_size(int w, int h)
{
    (void)w;
    (void)h;
}
void ui_bounce(int d)
{
    (void)d;
}

const char *ui_renderer(void)
{
    return "none";
}
void ui_text_input(int on)
{
    (void)on;
}
void *ui_window(void)
{
    return 0;
}
void ui_rows_top(float y)
{
    (void)y;
}
int ui_rows_fit(const UiRow *r, int n, int first)
{
    (void)r;
    (void)first;
    return n;
}
int ui_rows_scroll(const UiRow *r, int n, int sel, int first)
{
    (void)r;
    (void)n;
    (void)sel;
    return first;
}
void ui_rows_from(int first)
{
    (void)first;
}
void ui_para(float y, const char *t)
{
    (void)y;
    (void)t;
}
void ui_line(float y, int s, const char *t)
{
    (void)y;
    (void)s;
    (void)t;
}
void ui_bar(float y, float f, const char *t)
{
    (void)y;
    (void)f;
    (void)t;
}
void ui_header_sub(const char *t, const char *s)
{
    (void)t;
    (void)s;
}
void ui_scripted(int on)
{
    (void)on;
}
void ui_header_warn(const char *t, const char *s)
{
    (void)t;
    (void)s;
}
float ui_header_warn_room(const char *t, const char *s)
{
    (void)t;
    (void)s;
    return 0;
}
/* the cards (cards.c): not used here */
int cards_folder(const char *s, char *o, size_t c)
{
    (void)s;
    (void)o;
    (void)c;
    return -1;
}
int cards_name_ok(const char *n)
{
    (void)n;
    return 0;
}
int cards_list(const char *d, CardInfo *o, int m, int n)
{
    (void)d;
    (void)o;
    (void)m;
    (void)n;
    return 0;
}
int cards_in_use(const char *d, const char *n)
{
    (void)d;
    (void)n;
    return 0;
}
void cards_time_text(long long t, char *o, size_t c)
{
    (void)t;
    (void)o;
    (void)c;
}
int cards_old_count(const char *a, const char *b)
{
    (void)a;
    (void)b;
    return 0;
}
int cards_import_old(const char *d, const char *a, const char *b, int *c, int *l)
{
    (void)d;
    (void)a;
    (void)b;
    (void)c;
    (void)l;
    return -1;
}
void cards_default_name(const char *d, char *o, size_t c)
{
    (void)d;
    (void)o;
    (void)c;
}
int cards_exists(const char *d, const char *n)
{
    (void)d;
    (void)n;
    return 0;
}

void settings_test_reset(void);

static int g_fail;
static char g_archive[1000], g_ini[1000], g_empty_ini[1000], g_bad[1000], g_notable[1000];

/* a start: the settings as main.c leaves them (the file, then the command line's choices), then the case's changes */
static void start(const char *ini, void (*cli)(void))
{
    settings_test_reset();
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.vsync = -2;
    g_cfg.archive = g_archive;
    if (cli)
        cli();
    settings_load(ini);
    settings_apply(0, 0);
}

static void expect(const char *name, int want)
{
    int got = launcher_wanted();
    if (got != want) {
        g_fail++;
        fprintf(stderr, "FAIL %s: wanted %d, got %d\n", name, want, got);
    } else {
        printf("ok   %-44s %s\n", name, got ? "the launcher opens" : "no launcher");
    }
}

static void no_window(void) { g_cfg.no_window = 1; }
static void quit_after(void) { g_cfg.quit_after = 2; }
static void import_card(void) { g_cfg.import_card = "card.ps2"; }
static void automated(void) { g_cfg.automated = "--gs-off"; }
static void core_args(void) { g_cfg.automated = "the core's own arguments, after --"; }
static void no_launcher(void) { g_cfg.launcher = -1; }
static void force(void) { g_cfg.launcher = 1; }
static void force_but_automated(void)
{
    g_cfg.launcher = 1;
    g_cfg.quit_after = 1;
}
static void no_archive(void) { g_cfg.archive = "/nonexistent/mh2-disc.arc"; }
static void bad_archive(void) { g_cfg.archive = g_bad; }
static void no_table(void) { g_cfg.archive = g_notable; }
static void bad_archive_no_launcher(void) { g_cfg.archive = g_bad; g_cfg.launcher = -1; }
static void no_archive_no_launcher(void) { g_cfg.archive = "/nonexistent/mh2-disc.arc"; g_cfg.launcher = -1; }
static void no_table_no_launcher(void) { g_cfg.archive = g_notable; g_cfg.launcher = -1; }

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: launcher_wanted_test DIR\n");
        return 2;
    }
    snprintf(g_archive, sizeof(g_archive), "%s/disc.arc", argv[1]);
    snprintf(g_ini, sizeof(g_ini), "%s/skip.ini", argv[1]);
    snprintf(g_empty_ini, sizeof(g_empty_ini), "%s/empty.ini", argv[1]);
    /* an archive that passes the launcher's look (tools/port/make_archive.py's header: the name, version 1, a file count, the
     * header's size 128, its own size) with its movie table beside it, one without the table, and one that is not an archive */
    char tbl[1000];
    unsigned char hdr[128];
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "MH2DARC", 8);
    hdr[8] = 1;
    hdr[12] = 1;
    hdr[16] = 128;
    hdr[24] = 128;
    snprintf(g_notable, sizeof(g_notable), "%s/notable", argv[1]);
    SDL_CreateDirectory(g_notable);
    snprintf(g_notable, sizeof(g_notable), "%s/notable/disc.arc", argv[1]);
    snprintf(g_bad, sizeof(g_bad), "%s/bad.arc", argv[1]);
    snprintf(tbl, sizeof(tbl), "%s/mh2-movies.txt", argv[1]);
    FILE *f = hfs_fopen(g_archive, "wb");
    if (f) {
        fwrite(hdr, 1, sizeof(hdr), f);
        fclose(f);
    }
    f = hfs_fopen(g_notable, "wb");
    if (f) {
        fwrite(hdr, 1, sizeof(hdr), f);
        fclose(f);
    }
    f = hfs_fopen(g_bad, "wb");
    if (f) {
        fputs("not an archive", f);
        fclose(f);
    }
    f = hfs_fopen(tbl, "wb");
    if (f) {
        fputs("movies\n", f);
        fclose(f);
    }
    f = hfs_fopen(g_ini, "wb");
    if (f) {
        fputs("launcher = off\n", f);
        fclose(f);
    }
    f = hfs_fopen(g_empty_ini, "wb");
    if (f)
        fclose(f);

    start(g_empty_ini, NULL);
    expect("a start with nothing said", 1);
    start(g_ini, NULL);
    expect("launcher = off in mh2.ini", 0);
    start(g_ini, force);
    expect("launcher = off, and --launcher", 1);
    start(g_empty_ini, no_launcher);
    expect("--no-launcher", 0);
    start(g_empty_ini, no_window);
    expect("--no-window", 0);
    start(g_empty_ini, quit_after);
    expect("--quit-after", 0);
    start(g_empty_ini, import_card);
    expect("--import-card", 0);
    start(g_empty_ini, automated);
    expect("a test's option (the guard's automated run)", 0);
    start(g_empty_ini, core_args);
    expect("the core's own arguments, after --", 0);
    start(g_empty_ini, force_but_automated);
    expect("--launcher in an automated run", 0);
    start(g_empty_ini, no_archive);
    expect("no archive: it opens, for the first run's steps or the missing-archive banner", 1);
    start(g_ini, no_archive);
    expect("launcher = off, no archive: it opens", 1);
    start(g_ini, bad_archive);
    expect("launcher = off, an archive that is not valid for this version: it opens", 1);
    start(g_ini, no_table);
    expect("launcher = off, an archive with no movie table: it opens", 1);
    start(g_empty_ini, bad_archive);
    expect("an archive that is not valid for this version: it opens", 1);
    start(g_ini, no_archive_no_launcher);
    expect("launcher = off, no archive, and --no-launcher", 0);
    start(g_ini, bad_archive_no_launcher);
    expect("launcher = off, a bad archive, and --no-launcher", 0);
    start(g_ini, no_table_no_launcher);
    expect("launcher = off, no movie table, and --no-launcher", 0);
    printf(g_fail ? "launcher_wanted: %d FAILED\n" : "launcher_wanted: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
