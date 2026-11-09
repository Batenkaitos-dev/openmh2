/*
 * The table of settings (the launcher design, 5.2): every setting of mh2.ini and the command line as data.
 * The ini parser, the command line's setting options, --help, the launcher's rows and its writer all read it.
 * The table is in settings.c; the texts it names (a label, a help, a choice's words) are in launcher_text.h.
 */
#ifndef MH2_SETTINGS_H
#define MH2_SETTINGS_H

#include <stddef.h>
#include <stdio.h>

/* How the file's value is read (and what a value of the command line may be). */
enum {
    SET_FLAG,    /* on, off (1, yes, true) */
    SET_TRI,     /* a flag whose off is a statement of its own (-1) */
    SET_VSYNC,   /* auto, on, off */
    SET_CHOICE,  /* a number from a list (choices) */
    SET_RANGE,   /* a number from lo to hi */
    SET_TEXT,    /* a word or a path */
    SET_KEY,     /* a key's name (keyboard.c), or none */
    SET_BIND,    /* key.NAME, pad.NAME: a binding (input.c) */
    SET_OLD,     /* a key of the original's, said to be ignored */
    SET_PACK,    /* pack.NAME: a texture pack on or off (packs.c) */
};
/* How the file's value joins the command line's (settings_apply). */
enum {
    AP_NONE,
    AP_ON,        /* a flag: on makes the effective value on; the command line's on stays */
    AP_TEXT,      /* a text: the file's, unless the command line gave one */
    AP_TEXT_SET,  /* a text with no command line option: the file's */
    AP_RECORD,    /* record: on unless the file says off */
    AP_INVERT,     /* an inverted flag: the option says no to it */
    AP_VSYNC,
    AP_SSAA,
    AP_DAMAGE,
    AP_TEXMB,     /* texture_cache_mb: the file's, when set */
    AP_PRELOAD,   /* texture_preload: on unless the file says off */
    AP_FPS,       /* fps: the file's, unless the command line gave one */
    AP_FPS60,     /* fps60 = on: 60, unless fps or the command line says */
};
/* The pages of the launcher; PAGE_NONE: not on a page of its own (kept in the file, or development only). */
enum { PAGE_NONE, PAGE_HOME, PAGE_PICTURE, PAGE_GAME, PAGE_CARDS, PAGE_DISC, PAGE_CONTENT, PAGE_DEV };
/* What changing it changes: the renderer's picture only; what the game computes; or where the game starts. */
enum { FX_RENDERER, FX_COMPUTED, FX_START };
/* How the command line's option writes HostConfig, and the effective value reads from it. */
enum { C_NONE, C_FLAG, C_STR, C_NUM, C_VSYNC };

typedef struct Setting {
    const char *key;      /* the ini key (a prefix, ending in a dot, for the bindings) */
    const char *opt;      /* the command line's option, or NULL */
    const char *arg;      /* the option's argument (N, FOLDER), or NULL for a flag */
    unsigned char type;   /* SET_* */
    unsigned char how;    /* AP_* */
    unsigned char page;   /* PAGE_* */
    unsigned char effect; /* FX_* */
    unsigned char restart;   /* the in-game menu cannot apply it while playing */
    unsigned char hidden; /* not in --help */
    unsigned char prefix; /* key is a prefix */
    unsigned char ckind;  /* C_*: the command line's way into HostConfig */
    unsigned char onval;  /* a flag's value when on in HostConfig (1, or 2 for widescreen) */
    size_t cfg;           /* offsetof(HostConfig, field), 0: none */
    size_t merge;         /* the field settings_apply writes, when it is not cfg */
    const char *dflt;     /* the default, as the file spells it */
    const char *choices;  /* the file's words for a choice list, "a|b|c"; flags: "off|on" */
    int rule;             /* what a bad value is told to be, a text id: "1 (off), 2, 4, 8 or 16" */
    int lo, hi;           /* SET_RANGE's bounds */
    int label, help;      /* text ids (launcher_text.h) */
    int shown;            /* the choices' labels, "off|2x|4x": a text id, 0: the choices' words */
    void *st;             /* where the file's value is kept */
    size_t cap;           /* a text's capacity */
} Setting;

int settings_count(void);
const Setting *settings_row(int i);
int settings_find(const char *key);   /* the row of an exact key, or -1 */

/* The command line's setting options (one option at argv[*i]): 0 not one; 1 taken (*i is the last word taken);
 * -1 a missing or bad value. */
int settings_cli(int argc, char **argv, int *i);
/* --help: the setting options, one per line, from the table (OTHER_HEAD: the caller's own come first). */
void settings_help(FILE *f);
void settings_usage_words(FILE *f);   /* "[--ssaa 1|2|4|8|16] [--hires] ..." */

/* The effective values, for the launcher (settings.c, "the launcher's session") */
const char *settings_text(int i);              /* the value as the file spells it ("on", "4", "auto", a path), after apply */
int settings_given(int i);                     /* 1: the command line or --set gave it: shown, not saved over */
void settings_session_begin(void);             /* the values now are the baseline: what is saved over, and what "changed" means */
int settings_changed(int i);                   /* differs from the baseline */
int settings_dirty(void);                      /* some row the file would take differs from the baseline */
void settings_put(int i, const char *text);    /* the session's value (HostConfig and the file's copy); apply's coupling follows */
int settings_step(int i, int dir);             /* the next (dir 1) or previous choice of a flag or choice list: 1 if it moved */
const char *settings_shown(int i);             /* the current value in words ("4x", "on", "auto") */
const char *settings_choice_label(int i, int n);   /* the n-th choice's words, NULL past the end */
int settings_choice_count(int i);
int settings_choice_pos(int i);                /* the current choice's place in the list, -1 if none */
int settings_is_default(int i);                /* the value is the default */
void settings_default(int i);                  /* back to the default */
/* The file to save: --config's, the one read, else the user data folder's mh2.ini. NULL if there is none to make. */
const char *settings_save_path(void);
/* Writes the changed rows into the file, keeping its other lines (5.1), through FILE.tmp renamed over it. 0, or -1
 * with a message in ERR. A row the command line gave is not written. */
int settings_save(char *err, size_t cap);
void settings_normalize(void);                 /* ssaa and hires, as settings_apply leaves them */

#endif
