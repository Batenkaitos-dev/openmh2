/*
 * The settings writer (settings.c, the launcher design, 5.1): the launcher's session changes some rows and saves; the file
 * keeps every other line, a CRLF file stays CRLF, a key the file lacks goes into a [launcher] section, a value equal to its
 * default is not added, and a setting the command line gave is not saved over. settings.c is compiled into this test with
 * stubs for what it asks of the rest of the host; every file is under the directory the test is given.
 *
 *   settings_writer_test DIR
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

static void write_file(const char *path, const char *text, size_t n)
{
    FILE *f = hfs_fopen(path, "wb");
    if (!f || fwrite(text, 1, n, f) != n) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fclose(f);
}

static char *read_file(const char *path)
{
    FILE *f = hfs_fopen(path, "rb");
    if (!f)
        return NULL;
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return buf;
}

typedef struct Step {
    const char *key, *value;
} Step;

/* One case: INI is the file before (NULL: none); ARGS the command line's setting options (NULL-terminated);
 * STEPS the launcher's changes; SAVE whether to save; EXPECT the file after (NULL: still none). */
static void run(const char *name, const char *ini, const char *const *args, const Step *steps, const char *expect,
                const char *cfg_check)
{
    char path[1000];
    snprintf(path, sizeof(path), "%s/case.ini", g_dir);
    hfs_remove(path);
    char tmp[1010];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    hfs_remove(tmp);
    if (ini)
        write_file(path, ini, strlen(ini));
    settings_test_reset();
    packs_reset();
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.vsync = -2;
    g_cfg.config_path = path;
    int n = 0;
    while (args && args[n])
        n++;
    for (int i = 0; i < n; i++) {
        char *av[2] = {(char *)"mh2", (char *)args[i]};
        int k = 1;
        if (settings_cli(2, av, &k) != 1) {   /* a flag; one with a value is given as "--ssaa=8" below */
            char *eq = strchr((char *)args[i], '=');
            if (eq) {
                char opt[64];
                snprintf(opt, sizeof(opt), "%.*s", (int)(eq - args[i]), args[i]);
                char *av3[3] = {(char *)"mh2", opt, eq + 1};
                int j = 1;
                if (settings_cli(3, av3, &j) != 1)
                    fprintf(stderr, "%s: bad option %s\n", name, args[i]);
            }
        }
    }
    settings_load(path);
    settings_apply(0, 0);
    settings_session_begin();
    for (const Step *s = steps; s && s->key; s++) {
        if (!strcmp(s->key, "@packs")) {   /* the Content page's list: "name=on,name=off" in order */
            PackInfo list[16];
            int np = 0;
            char text[256];
            snprintf(text, sizeof(text), "%s", s->value);
            for (char *tok = strtok(text, ","); tok && np < 16; tok = strtok(NULL, ",")) {
                char *eq = strchr(tok, '=');
                if (!eq)
                    continue;
                *eq = 0;
                snprintf(list[np].name, PACK_NAME_MAX, "%s", tok);
                list[np].on = !strcmp(eq + 1, "on");
                list[np].present = 1;
                np++;
            }
            packs_set(list, np);
            continue;
        }
        int row = settings_find(s->key);
        if (row < 0) {
            fprintf(stderr, "%s: no row %s\n", name, s->key);
            g_fail++;
            continue;
        }
        settings_put(row, s->value);
    }
    char err[200] = "";
    int rc = settings_save(err, sizeof(err));
    char *after = read_file(path);
    int ok = rc == 0 && (expect ? after && !strcmp(after, expect) : after == NULL);
    if (cfg_check) {   /* "ssaa=4": the session's value in HostConfig */
        char got[64];
        snprintf(got, sizeof(got), "ssaa=%d", g_cfg.ssaa);
        ok = ok && !strcmp(got, cfg_check);
    }
    FILE *t = hfs_fopen(tmp, "rb");
    if (t) {
        fclose(t);
        ok = 0;
        fprintf(stderr, "%s: %s.tmp is left\n", name, path);
    }
    if (!ok) {
        g_fail++;
        fprintf(stderr, "FAIL %s (rc %d %s)\n--- expected\n%s\n--- got\n%s\n---\n", name, rc, err, expect ? expect : "(no file)",
                after ? after : "(no file)");
    } else {
        printf("ok   %s\n", name);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: settings_writer_test DIR\n");
        return 2;
    }
    snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);

    run("in place, the comment kept", "ssaa = 2  # smoother\nfullscreen = off\n", NULL,
        (Step[]){{"ssaa", "4"}, {"fullscreen", "on"}, {0}}, "ssaa = 4  # smoother\nfullscreen = on\n", NULL);
    run("no file: made, a launcher section", NULL, NULL, (Step[]){{"ssaa", "8"}, {"hires", "on"}, {0}},
        "[launcher]\nssaa = 8\nhires = on\n", NULL);
    run("a new key after other lines", "[display]\nmute = on\n", NULL, (Step[]){{"vsync", "on"}, {0}},
        "[display]\nmute = on\n\n[launcher]\nvsync = on\n", NULL);
    run("an existing launcher section takes it", "[launcher]\nssaa = 2\n[input]\nkey.cross = Space\n", NULL,
        (Step[]){{"hires", "on"}, {0}}, "[launcher]\nssaa = 4\nhires = on\n[input]\nkey.cross = Space\n", NULL);
    run("CRLF stays", "ssaa = 2\r\nmute = off\r\n", NULL, (Step[]){{"ssaa", "4"}, {"fullscreen", "on"}, {0}},
        "ssaa = 4\r\nmute = off\r\n\r\n[launcher]\r\nfullscreen = on\r\n", NULL);
    run("no newline at the end", "mute = on", NULL, (Step[]){{"mute", "off"}, {0}}, "mute = off", NULL);
    {
        const char *args[] = {"--ssaa=8", NULL};
        run("the command line is not saved over", "ssaa = 2\n", args, (Step[]){{"ssaa", "4"}, {"hires", "on"}, {0}},
            "ssaa = 2\n\n[launcher]\nhires = on\n", NULL);
        run("the command line's, shown", "ssaa = 2\n", args, (Step[]){{0}}, "ssaa = 2\n", "ssaa=8");
    }
    run("a change and back is no change", NULL, NULL, (Step[]){{"ssaa", "4"}, {"ssaa", "1"}, {0}}, NULL, NULL);
    run("back to the default, in the file", "ssaa = 4\n", NULL, (Step[]){{"ssaa", "1"}, {0}}, "ssaa = 1\n", NULL);
    run("the last of two lines", "ssaa = 2\nssaa = 4\n", NULL, (Step[]){{"ssaa", "8"}, {0}}, "ssaa = 2\nssaa = 8\n", NULL);
    run("unknown keys and comments stay", "# mine\nfoo = 1\n; and\n[x]\nkey.cross = Space\nssaa=2\n", NULL,
        (Step[]){{"ssaa", "16"}, {0}}, "# mine\nfoo = 1\n; and\n[x]\nkey.cross = Space\nssaa=16\n", NULL);
    run("a text value", NULL, NULL, (Step[]){{"textures", "/my textures"}, {0}}, "[launcher]\ntextures = /my textures\n", NULL);
    run("the key's case stays", "SSAA = 2\n", NULL, (Step[]){{"ssaa", "8"}, {0}}, "SSAA = 8\n", NULL);
    run("hires implies ssaa", NULL, NULL, (Step[]){{"hires", "on"}, {0}}, "[launcher]\nssaa = 4\nhires = on\n", NULL);
    run("skip the launcher", NULL, NULL, (Step[]){{"launcher", "off"}, {0}}, "[launcher]\nlauncher = off\n", NULL);
    run("a blank line before the section is not doubled", "mute = on\n\n", NULL, (Step[]){{"vsync", "off"}, {0}},
        "mute = on\n\n[launcher]\nvsync = off\n", NULL);

    /* the Content page's pack list: the lines it writes, in its order */
    run("packs: no lines, a launcher section", NULL, NULL, (Step[]){{"@packs", "Zed=on,alpha=off"}, {0}},
        "[launcher]\npack.Zed = on\npack.alpha = off\n", NULL);
    run("packs: the list takes the first line's place, in its order", "# mine\nmute = on\npack.a = on\nfoo = 1\npack.b = off\n", NULL,
        (Step[]){{"@packs", "b=on,a=off,c=on"}, {0}}, "# mine\nmute = on\npack.b = on\npack.a = off\npack.c = on\nfoo = 1\n", NULL);
    run("packs: a line ends the launcher section, and a new key follows it", "[launcher]\nssaa = 2\npack.a = on\n[input]\nkey.cross = Space\n",
        NULL, (Step[]){{"@packs", "b=on,a=on"}, {"vsync", "on"}, {0}},
        "[launcher]\nssaa = 2\npack.b = on\npack.a = on\nvsync = on\n[input]\nkey.cross = Space\n", NULL);
    run("packs: all of them gone", "pack.a = on\nmute = on\n", NULL, (Step[]){{"@packs", ""}, {0}}, "mute = on\n", NULL);
    run("packs: a Japanese name, CRLF", "ssaa = 2\r\n", NULL, (Step[]){{"@packs", "\xe3\x83\x87\xe3\x82\xa3=on"}, {0}},
        "ssaa = 2\r\n\r\n[launcher]\r\npack.\xe3\x83\x87\xe3\x82\xa3 = on\r\n", NULL);
    run("packs: the last line has no end of line", "mute = on\npack.a = on", NULL, (Step[]){{"@packs", "a=off"}, {0}},
        "mute = on\npack.a = off\n", NULL);
    run("packs: unchanged, the file is left alone", "pack.b = off\npack.a = on\n", NULL, (Step[]){{0}}, "pack.b = off\npack.a = on\n", NULL);

    /* a file that cannot be made */
    {
        settings_test_reset();
        memset(&g_cfg, 0, sizeof(g_cfg));
        char bad[1000];
        snprintf(bad, sizeof(bad), "%s/no-such-folder/mh2.ini", g_dir);
        g_cfg.config_path = bad;
        settings_session_begin();
        settings_put(settings_find("mute"), "on");
        char err[200] = "";
        int rc = settings_save(err, sizeof(err));
        if (rc == 0 || !strstr(err, "cannot write")) {
            g_fail++;
            fprintf(stderr, "FAIL an unwritable place (rc %d, \"%s\")\n", rc, err);
        } else {
            printf("ok   an unwritable place: %s\n", err);
        }
    }
    printf(g_fail ? "settings writer: %d FAILED\n" : "settings writer: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
