/*
 * The Frontier controls setting (settings.c): frontier_controls = auto|on|off. auto is on with the Frontier content's core and
 * off without it; on and off are as written, and a saved off stays off beside the content; the core is given --frontier-controls
 * (and the camera is the right stick's, the Modern camera on) only when the controls are on and the pad is a live one: a pad
 * file, a game played back and a command line that names the option itself are left as they are, so no pad is translated twice.
 * The launcher's row never shows "auto": it shows what auto comes to. settings.c is compiled into this test with stubs for what
 * it asks of the rest of the host.
 *
 *   frontier_controls_test DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"
#include "packs.h"
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
const char *play_data_dir(void)
{
    return NULL;
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

void settings_test_reset(void);

static int g_fail;
static char g_dir[900];

typedef struct Case {
    const char *name;
    const char *ini;       /* the settings file (NULL: none) */
    int content_core;      /* the core is the Frontier content's */
    int pad_rec;           /* --pad-rec was given */
    const char *core_arg;  /* a core argument on the command line (NULL: none) */
    int want_on;           /* the core is given --frontier-controls by the setting */
    int want_flags;        /* how many times --frontier-controls is among the core's arguments */
    const char *want_shown;   /* the row's text in the launcher */
} Case;

static int count_flag(void)
{
    int n = 0;
    for (int i = 0; i < g_cfg.core_nargs; i++)
        n += !strcmp(g_cfg.core_args[i], "--frontier-controls");
    return n;
}

static void run(const Case *c)
{
    char path[1000];
    snprintf(path, sizeof(path), "%s/fctl.ini", g_dir);
    hfs_remove(path);
    if (c->ini) {
        FILE *f = hfs_fopen(path, "wb");
        if (!f || fwrite(c->ini, 1, strlen(c->ini), f) != strlen(c->ini)) {
            fprintf(stderr, "cannot write %s\n", path);
            exit(2);
        }
        fclose(f);
    }
    settings_test_reset();
    packs_reset();
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.vsync = -2;
    g_cfg.config_path = path;
    g_cfg.content_core = c->content_core;
    if (c->pad_rec) {
        printf("skip %s (a release has no recorded pad)\n", c->name);
        return;
    }
    if (c->core_arg) {
        static char *args[2];
        args[0] = (char *)c->core_arg;
        g_cfg.core_args = args;
        g_cfg.core_nargs = 1;
    }
    settings_load(path);
    settings_apply(0, 0);
    settings_session_begin();
    launcher_frontier_controls();
    int row = settings_find("frontier_controls");
    const char *shown = row >= 0 ? settings_shown(row) : "(no row)";
    int ok = row >= 0 && g_cfg.frontier_on == c->want_on && count_flag() == c->want_flags && !strcmp(shown, c->want_shown);
    if (c->want_on)
        ok = ok && g_cfg.camera_smooth == 1 && g_cfg.camera_stick && !strcmp(g_cfg.camera_stick, "on");
    else
        ok = ok && g_cfg.camera_smooth == 0;
    if (!ok) {
        g_fail++;
        fprintf(stderr, "FAIL %s: on %d (want %d), flags %d (want %d), shown \"%s\" (want \"%s\")\n", c->name, g_cfg.frontier_on,
                c->want_on, count_flag(), c->want_flags, shown, c->want_shown);
    } else {
        printf("ok   %s\n", c->name);
    }
}

/* A saved value: the launcher turns the row to WORD, the file is written, and read again. */
static void saved(const char *name, const char *ini, int content_core, const char *word, const char *want_file, int want_on)
{
    char path[1000];
    snprintf(path, sizeof(path), "%s/fctl.ini", g_dir);
    hfs_remove(path);
    if (ini) {
        FILE *f = hfs_fopen(path, "wb");
        if (!f || fwrite(ini, 1, strlen(ini), f) != strlen(ini)) {
            fprintf(stderr, "cannot write %s\n", path);
            exit(2);
        }
        fclose(f);
    }
    settings_test_reset();
    packs_reset();
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.vsync = -2;
    g_cfg.config_path = path;
    g_cfg.content_core = content_core;
    settings_load(path);
    settings_apply(0, 0);
    settings_session_begin();
    int row = settings_find("frontier_controls");
    int step_ok = 1;
    if (word) {
        /* the arrow: from what the row shows to the other word, as the launcher does */
        step_ok = settings_step(row, !strcmp(word, "off") ? 1 : -1) == 1;
    }
    char err[200] = "";
    int rc = settings_save(err, sizeof(err));
    FILE *f = hfs_fopen(path, "rb");
    char buf[2048] = "";
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    /* read again, as the next start does */
    settings_test_reset();
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.vsync = -2;
    g_cfg.config_path = path;
    g_cfg.content_core = content_core;
    settings_load(path);
    settings_apply(0, 0);
    launcher_frontier_controls();
    int ok = rc == 0 && step_ok && (want_file ? strstr(buf, want_file) != NULL : strstr(buf, "frontier_controls") == NULL) &&
             g_cfg.frontier_on == want_on;
    if (!ok) {
        g_fail++;
        fprintf(stderr, "FAIL %s (rc %d %s, step %d, on %d want %d)\n--- file\n%s\n---\n", name, rc, err, step_ok, g_cfg.frontier_on,
                want_on, buf);
    } else {
        printf("ok   %s\n", name);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: frontier_controls_test DIR\n");
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    static const Case cases[] = {
        /* name, ini, content core, pad rec, core arg, on, flags, shown */
        {"auto, no Frontier content: off", NULL, 0, 0, NULL, 0, 0, "off"},
        {"auto, Frontier content: on", NULL, 1, 0, NULL, 1, 1, "on"},
        {"auto written, Frontier content: on", "frontier_controls = auto\n", 1, 0, NULL, 1, 1, "on"},
        {"on, no Frontier content: on", "frontier_controls = on\n", 0, 0, NULL, 1, 1, "on"},
        {"off, Frontier content: off", "frontier_controls = off\n", 1, 0, NULL, 0, 0, "off"},
        {"off, no Frontier content: off", "frontier_controls = off\n", 0, 0, NULL, 0, 0, "off"},
        {"on, a recorded pad: nothing added", "frontier_controls = on\n", 0, 1, NULL, 0, 0, "on"},
        {"auto, Frontier content, a recorded pad: nothing added", NULL, 1, 1, NULL, 0, 0, "on"},
        {"on, the core's option on the command line: once, camera as it is", "frontier_controls = on\n", 0, 0, "--frontier-controls", 0, 1,
         "on"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        run(&cases[i]);
    /* the arrow turns the row to the other word and the file keeps it; auto no longer follows */
    saved("arrow off from auto with the content: the file holds off", NULL, 1, "off", "frontier_controls = off", 0);
    saved("arrow on from auto without the content: the file holds on", NULL, 0, "on", "frontier_controls = on", 1);
    saved("untouched: the file says nothing", NULL, 1, NULL, NULL, 1);
    saved("a saved off is kept beside the content", "frontier_controls = off\n", 1, NULL, "frontier_controls = off", 0);
    if (g_fail) {
        fprintf(stderr, "%d failed\n", g_fail);
        return 1;
    }
    printf("frontier_controls_test: all cases passed\n");
    return 0;
}
