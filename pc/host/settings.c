/*
 * The settings file, mh2.ini, and the launcher's choices for the core, as the
 * original's: the same choices as the command line,
 * which overrides it, plus the key and gamepad bindings.
 *
 * Found at (the first that exists): --config FILE; mh2.ini next to mh2 (a
 * portable setup); mh2.ini in the user data folder (next to the memcards
 * folder; never in an automated run, main.c's guard). It is created or changed by the launcher only, when the player
 * presses Play or Save (settings_save below: a line-preserving editor, through FILE.tmp renamed over the file; never in an
 * automated run), and a setting the command line gave is not saved over. --set KEY=VALUE options are applied after it.
 * Lines are `key = value`; `#` and `;` start comments; `[section]` lines are only for readability.
 *
 * The settings are one table (g_rows, below; settings.h): the ini parser, the command line's setting options, --help, the
 * launcher's rows and its writer all read it. The keys:
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
 *                         --hires), drawn by the renderer only;
 *   texture_dump = on     the textures the game draws are written, once each, as PNGs to the textures folder's dump
 *                         (as --texture-dump; the renderer's, off by default);
 *   texture_packs = on    the PNGs in the textures folder's packs are drawn in place of the textures they are named
 *                         for (as --texture-packs; off by default);
 *   texture_cache_mb = N  the memory the packs may keep decoded (16 to 16384, default 1024; also the graphics card's images);
 *   texture_preload = off the packs are not decoded at the start, only when first drawn (default on)
 *   textures = FOLDER     the textures folder (default: textures in the user data folder; as --textures FOLDER)
 *   usb_keyboard = on     the PC keyboard is the game's USB keyboard (as --usb-keyboard; off by default: keyboard.c), usb_keyboard_toggle = KEY (default ScrollLock) switches between
 *                         typing and the pad's keyboard keys, usb_key_yen, usb_key_ro, usb_key_henkan, usb_key_muhenkan,
 *                         usb_key_kana and usb_key_zenkaku = KEY (or none) bind the Japanese keyboard's extra keys
 *   widescreen = on       the 16:9 view (as --widescreen). Unlike the two above it changes the
 *                         game's state: it is the core's --widescreen; the host shows the picture at 16:9,
 *                         the movies at 4:3. Off by default.
 *   offline_hub = on      the village's gate leads to the online town (the game's own, with its two quest counters), played with no
 *                         network (as --offline-hub). It changes the game's state: it is the core's --offline-hub, and a saved session
 *                         or save state made with it is good only with it. Off by default: the game is Dos as it was.
 *   launcher = off        the launcher's window is not shown at the start (Shift held at the start, or --launcher, shows it)
 *   orig, any_core: the old build's, not needed by mh2 (it says why)
 *   camera_stick = hold   the camera on the right stick (camstick.c): off (default), hold (while
 *                         camera_stick_button is held: l3 by default) or on (always); camera_stick_which = right|left,
 *                         camera_stick_invert = none|x|y|xy, camera_stick_speed = 25|50|75|100 (percent of the game's turn)
 *
 * The launcher: mh2-core gets --disc (mh2's --archive, the setting, else
 * mh2-disc.arc next to mh2, in the user data folder, or the build tree's
 * build/port), and the cards. None of this reaches the game but the files
 * the core opens.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_text.h"
#include "mh2host.h"
#include "packs.h"
#include "settings.h"

static char g_msgs[2048];   /* logged after the options are read */
static size_t g_nmsgs;

static struct {
    char archive[1024];
    char memcard[2][1024];
    char cards[1024], card[2][256];
    int fullscreen, mute, vsync;
    int ssaa, hires, widescreen, fast_loading, offline_hub;
    int texture_dump, texture_packs, texture_cache_mb, texture_preload;
    int usb_keyboard;
    char textures[1024];
    char text_pack[256];
    char states[1024];
    char pictures[1024];
    char freecam_input[16], freecam_speed[16], freecam_pad[16], fast_forward_cap[16];
    char camera_stick[8], camera_stick_button[12], camera_stick_which[8], camera_stick_invert[8], camera_stick_speed[8];
    int launcher;   /* launcher = off: the launcher's window is not shown at the start (launcher.c) */
} st = {.fullscreen = -1, .mute = -1, .vsync = -2, .launcher = 1};

static char g_loaded[1200];   /* the settings file read last (the launcher saves into it) */

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

/* ------------------------------------------------------------ the table (the launcher design, 5.2) */

#define INTST(f) .st = &st.f
#define TXTST(f) .st = st.f, .cap = sizeof(st.f)
#define CFG(f) .cfg = offsetof(HostConfig, f)

/* One row a setting. The order is the order of --help; a prefix row (key.NAME) comes after the exact keys. */
static const Setting g_rows[] = {
    /* the picture: the renderer's only */
    {.key = "ssaa", .opt = "--ssaa", .arg = "1|2|4|8|16", .type = SET_CHOICE, .how = AP_SSAA, .page = PAGE_PICTURE,
     .effect = FX_RENDERER, .ckind = C_NUM, CFG(ssaa), .dflt = "1", .choices = "1|2|4|8|16", .rule = T_SSAA_RULE,
     .label = T_SSAA, .help = T_SSAA_H, .shown = T_SSAA_SHOWN, INTST(ssaa)},
    {.key = "hires", .opt = "--hires", .type = SET_FLAG, .how = AP_ON, .page = PAGE_PICTURE, .effect = FX_RENDERER,
     .ckind = C_FLAG, .onval = 1, CFG(hires), .dflt = "off", .label = T_HIRES, .help = T_HIRES_H, INTST(hires)},
    {.key = "fullscreen", .opt = "--fullscreen", .type = SET_FLAG, .how = AP_ON, .page = PAGE_PICTURE, .effect = FX_RENDERER,
     .ckind = C_FLAG, .onval = 1, CFG(fullscreen), .dflt = "off", .label = T_FULLSCREEN, .help = T_FULLSCREEN_H,
     INTST(fullscreen)},
    {.key = "vsync", .opt = "--vsync", .arg = "auto|on|off", .type = SET_VSYNC, .how = AP_VSYNC, .page = PAGE_PICTURE,
     .effect = FX_RENDERER, .ckind = C_VSYNC, CFG(vsync), .dflt = "auto", .choices = "auto|on|off", .label = T_VSYNC,
     .help = T_VSYNC_H, .shown = T_CH_VSYNC, INTST(vsync)},
    {.key = "mute", .opt = "--mute", .type = SET_FLAG, .how = AP_ON, .page = PAGE_PICTURE, .effect = FX_RENDERER,
     .ckind = C_FLAG, .onval = 1, CFG(mute), .dflt = "off", .label = T_MUTE, .help = T_MUTE_H, INTST(mute)},
    {.key = "texture_dump", .opt = "--texture-dump", .type = SET_FLAG, .how = AP_ON, .page = PAGE_PICTURE,
     .effect = FX_RENDERER, .ckind = C_FLAG, .onval = 1, CFG(texture_dump), .dflt = "off", .label = T_TEXDUMP,
     .help = T_TEXDUMP_H, INTST(texture_dump)},
    {.key = "texture_packs", .opt = "--texture-packs", .type = SET_FLAG, .how = AP_ON, .page = PAGE_PICTURE,
     .effect = FX_RENDERER, .ckind = C_FLAG, .onval = 1, CFG(texture_packs), .dflt = "off", .label = T_TEXPACKS,
     .help = T_TEXPACKS_H, INTST(texture_packs)},
    {.key = "texture_cache_mb", .type = SET_RANGE, .how = AP_TEXMB, .page = PAGE_CONTENT, .effect = FX_RENDERER, .restart = 1,
     .ckind = C_NUM,
     CFG(texture_cache_mb), .lo = 16, .hi = 16384, .dflt = "1024", .choices = "256|512|1024|2048|4096|8192", .label = T_TEXMB, .help = T_TEXMB_H, INTST(texture_cache_mb)},
    {.key = "texture_preload", .type = SET_TRI, .how = AP_PRELOAD, .page = PAGE_CONTENT, .effect = FX_RENDERER, .restart = 1,
     .ckind = C_FLAG, .onval = 1, .choices = "off|on", CFG(texture_no_preload), .dflt = "on", .label = T_TEXPRE, .help = T_TEXPRE_H, INTST(texture_preload)},
    /* the translation pack: a folder of the text_packs folder, or a path; off: none */
    {.key = "text_pack", .opt = "--text-pack", .arg = "off|NAME", .type = SET_TEXT, .how = AP_TEXT, .page = PAGE_CONTENT,
     .effect = FX_START, .restart = 1, .ckind = C_STR, CFG(text_pack), .dflt = "off", .label = T_TEXTPACK, .help = T_TEXTPACK_H,
     TXTST(text_pack)},
    {.key = "textures", .opt = "--textures", .arg = "FOLDER", .type = SET_TEXT, .how = AP_TEXT, .page = PAGE_PICTURE,
     .effect = FX_RENDERER, .ckind = C_STR, CFG(textures), .dflt = "", .label = T_TEXTURES, .help = T_TEXTURES_H,
     TXTST(textures)},
    /* the game: what is computed, and the keyboard */
    {.key = "widescreen", .opt = "--widescreen", .type = SET_FLAG, .how = AP_ON, .page = PAGE_GAME, .effect = FX_COMPUTED,
     .restart = 1, .ckind = C_FLAG, .onval = 2, CFG(widescreen), .dflt = "off", .label = T_WIDESCREEN,
     .help = T_WIDESCREEN_H, INTST(widescreen)},
    /* the offline hub (Offline Dundorma): the Game page lists it in launcher.c */
    {.key = "offline_hub", .opt = "--offline-hub", .type = SET_FLAG, .how = AP_ON, .page = PAGE_GAME, .effect = FX_COMPUTED,
     .restart = 1, .ckind = C_FLAG, .onval = 1, CFG(offline_hub), .dflt = "off", .label = T_OFFLINE_HUB, .help = T_OFFLINE_HUB_H,
     INTST(offline_hub)},
    {.key = "usb_keyboard", .opt = "--usb-keyboard", .type = SET_FLAG, .how = AP_ON, .page = PAGE_GAME,
     .effect = FX_COMPUTED, .restart = 1, .ckind = C_FLAG, .onval = 1, CFG(usb_keyboard), .dflt = "off",
     .label = T_USBKB, .help = T_USBKB_H, INTST(usb_keyboard)},
    {.key = "usb_keyboard_toggle", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "ScrollLock",
     .label = T_TOGGLE, .help = T_TOGGLE_H},
    {.key = "usb_key_yen", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "Backslash", .label = T_KEY_YEN,
     .help = T_KEY_H},
    {.key = "usb_key_ro", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "none", .label = T_KEY_RO,
     .help = T_KEY_H},
    {.key = "usb_key_henkan", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "Pause",
     .label = T_KEY_HENKAN, .help = T_KEY_H},
    {.key = "usb_key_muhenkan", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "Application",
     .label = T_KEY_MUHENKAN, .help = T_KEY_H},
    {.key = "usb_key_kana", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "Right Alt",
     .label = T_KEY_KANA, .help = T_KEY_H},
    {.key = "usb_key_zenkaku", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_COMPUTED, .dflt = "`", .label = T_KEY_ZENKAKU,
     .help = T_KEY_H},
    /* save states (states.c): the four keys, and the folder of the ten slots */
    {.key = "state_key_save", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Home", .label = T_SKEY_SAVE,
     .help = T_SKEY_SAVE_H},
    {.key = "state_key_load", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "End", .label = T_SKEY_LOAD,
     .help = T_SKEY_LOAD_H},
    {.key = "state_key_next", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "PageDown",
     .label = T_SKEY_NEXT, .help = T_SKEY_NEXT_H},
    {.key = "state_key_prev", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "PageUp",
     .label = T_SKEY_PREV, .help = T_SKEY_PREV_H},
    {.key = "states", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START, .ckind = C_STR,
     CFG(states_dir), .dflt = "", .label = T_STATES, .help = T_STATES_H, TXTST(states)},
    /* the freecam (freecam.c): its keys, what the game is given while it has control, and its speed */
    {.key = "freecam_key", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "F9", .label = T_FCK_TOGGLE,
     .help = T_FCK_TOGGLE_H},
    {.key = "freecam_key_forward", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "W", .label = T_FCK_FWD,
     .help = T_FCK_H},
    {.key = "freecam_key_back", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "S", .label = T_FCK_BACK,
     .help = T_FCK_H},
    {.key = "freecam_key_left", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "A", .label = T_FCK_LEFT,
     .help = T_FCK_H},
    {.key = "freecam_key_right", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "D", .label = T_FCK_RIGHT,
     .help = T_FCK_H},
    {.key = "freecam_key_down", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Q", .label = T_FCK_DOWN,
     .help = T_FCK_H},
    {.key = "freecam_key_up", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "E", .label = T_FCK_UP,
     .help = T_FCK_H},
    {.key = "freecam_key_reset", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "R", .label = T_FCK_RESET,
     .help = T_FCK_H},
    {.key = "freecam_key_fov_narrow", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Z", .label = T_FCK_NARROW,
     .help = T_FCK_H},
    {.key = "freecam_key_fov_wide", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "X", .label = T_FCK_WIDE,
     .help = T_FCK_H},
    {.key = "freecam_key_hold", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Tab", .label = T_FCK_HOLD,
     .help = T_FCK_H},
    {.key = "pictures", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START, .ckind = C_STR,
     CFG(pictures_dir), .dflt = "", .label = T_PICS, .help = T_PICS_H, TXTST(pictures)},
    {.key = "freecam_key_pause", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Space", .label = T_FCK_PAUSE,
     .help = T_FCK_PAUSE_H},
    {.key = "freecam_key_step", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Period", .label = T_FCK_STEP,
     .help = T_FCK_STEP_H},
    {.key = "freecam_key_photo", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "P", .label = T_FCK_PHOTO,
     .help = T_FCK_PHOTO_H},
    {.key = "freecam_key_hide", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "H", .label = T_FCK_HIDE,
     .help = T_FCK_HIDE_H},
    /* the pacing (pace.c): fast loading, the pause for the plain game, fast-forward */
    {.key = "fast_loading", .opt = "--fast-loading", .type = SET_FLAG, .how = AP_ON, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_FLAG, .onval = 1, CFG(fast_loading), .dflt = "off", .label = T_FASTLOAD, .help = T_FASTLOAD_H,
     INTST(fast_loading)},
    {.key = "pace_key_pause", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Space", .label = T_PZ_PAUSE,
     .help = T_PZ_PAUSE_H},
    {.key = "pace_key_step", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "Period", .label = T_PZ_STEP,
     .help = T_PZ_STEP_H},
    {.key = "pace_key_fast", .type = SET_KEY, .page = PAGE_GAME, .effect = FX_START, .dflt = "`", .label = T_PZ_FAST,
     .help = T_PZ_FAST_H},
    {.key = "fast_forward_cap", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START, .ckind = C_STR,
     CFG(fast_forward_cap), .dflt = "4", .choices = "2|3|4|6|8|none", .label = T_PZ_CAP, .help = T_PZ_CAP_H,
     TXTST(fast_forward_cap)},
    {.key = "freecam_game_input", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(freecam_input), .dflt = "neutral", .choices = "neutral|pad", .label = T_FCIN, .help = T_FCIN_H,
     TXTST(freecam_input)},
    {.key = "freecam_speed", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(freecam_speed), .dflt = "400", .choices = "200|400|800|1500|3000", .label = T_FCSPEED,
     .help = T_FCSPEED_H, TXTST(freecam_speed)},
    /* the camera on the right stick (camstick.c): the stick as the D-pad the game reads; nothing in the game changes */
    {.key = "camera_stick", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START, .ckind = C_STR,
     CFG(camera_stick), .dflt = "off", .choices = "off|hold|on", .label = T_CAMSTICK, .help = T_CAMSTICK_H, TXTST(camera_stick)},
    {.key = "camera_stick_button", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(camera_stick_button), .dflt = "l3", .choices = "l3|select|l2|r2", .label = T_CAMSTICK_BTN,
     .help = T_CAMSTICK_BTN_H, TXTST(camera_stick_button)},
    {.key = "camera_stick_which", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(camera_stick_which), .dflt = "right", .choices = "right|left", .label = T_CAMSTICK_WHICH,
     .help = T_CAMSTICK_WHICH_H, TXTST(camera_stick_which)},
    {.key = "camera_stick_invert", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(camera_stick_invert), .dflt = "none", .choices = "none|x|y|xy", .label = T_CAMSTICK_INV,
     .help = T_CAMSTICK_INV_H, TXTST(camera_stick_invert)},
    {.key = "camera_stick_speed", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(camera_stick_speed), .dflt = "100", .choices = "25|50|75|100", .label = T_CAMSTICK_SPEED,
     .help = T_CAMSTICK_SPEED_H, TXTST(camera_stick_speed)},
    {.key = "freecam_pad_toggle", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_GAME, .effect = FX_START,
     .ckind = C_STR, CFG(freecam_pad), .dflt = "off", .choices = "on|off", .label = T_FCPAD, .help = T_FCPAD_H,
     TXTST(freecam_pad)},
    /* the launcher */
    {.key = "launcher", .type = SET_FLAG, .how = AP_NONE, .page = PAGE_HOME, .effect = FX_START, .dflt = "on",
     .label = T_LAUNCHER, .help = T_LAUNCHER_H, INTST(launcher)},
    /* where things are */
    {.key = "archive", .opt = "--archive", .arg = "FILE", .type = SET_TEXT, .how = AP_NONE, .page = PAGE_DISC,
     .effect = FX_START, .restart = 1, .ckind = C_STR, CFG(archive), .dflt = "", .label = T_ARCHIVE, .help = T_ARCHIVE_H,
     TXTST(archive)},
    {.key = "cards", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_CARDS, .effect = FX_START, .restart = 1,
     .ckind = C_STR, CFG(cards_dir), .dflt = "", .label = T_CARDS, .help = T_CARDS_H, TXTST(cards)},
    {.key = "card1", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_CARDS, .effect = FX_START, .restart = 1,
     .ckind = C_STR, CFG(card[0]), .dflt = "Mcd001.ps2", .label = T_CARD1, .help = T_CARD1_H, TXTST(card[0])},
    {.key = "card2", .type = SET_TEXT, .how = AP_TEXT_SET, .page = PAGE_CARDS, .effect = FX_START, .restart = 1,
     .ckind = C_STR, CFG(card[1]), .dflt = "", .label = T_CARD2, .help = T_CARD2_H, TXTST(card[1])},
    {.key = "memcard1", .opt = "--memcard1", .arg = "FILE", .type = SET_TEXT, .how = AP_TEXT_SET, .effect = FX_START,
     .ckind = C_STR, CFG(memcard[0]), .merge = offsetof(HostConfig, old_card[0]), .label = T_MEMCARD, .help = T_O_MEMCARD1,
     TXTST(memcard[0])},
    {.key = "memcard2", .opt = "--memcard2", .arg = "FILE", .type = SET_TEXT, .how = AP_TEXT_SET, .effect = FX_START,
     .ckind = C_STR, CFG(memcard[1]), .merge = offsetof(HostConfig, old_card[1]), .label = T_MEMCARD, .help = T_O_MEMCARD2,
     TXTST(memcard[1])},
    /* bindings, and the original's keys */
    {.key = "key.", .type = SET_BIND, .prefix = 1, .label = T_BIND, .help = T_BIND_H},
    {.key = "pad.", .type = SET_BIND, .prefix = 1, .label = T_BIND, .help = T_BIND_H, .hidden = 1},
    {.key = "pack.", .type = SET_PACK, .prefix = 1, .hidden = 1},
    {.key = "orig", .type = SET_OLD, .hidden = 1, .help = T_OLD_ORIG},
    {.key = "any_core", .type = SET_OLD, .hidden = 1, .help = T_OLD_ANYCORE},
    /* fps and fps60 come from a development build's file: read and ignored, as the original's keys are */
    {.key = "fps", .type = SET_OLD, .hidden = 1, .help = T_OLD_ANYCORE},
    {.key = "fps60", .type = SET_OLD, .hidden = 1, .help = T_OLD_ANYCORE},
};
#define NROWS ((int)(sizeof(g_rows) / sizeof(g_rows[0])))

int settings_count(void)
{
    return NROWS;
}

const Setting *settings_row(int i)
{
    return i >= 0 && i < NROWS ? &g_rows[i] : NULL;
}

int settings_find(const char *key)
{
    for (int i = 0; i < NROWS; i++)
        if (!g_rows[i].prefix && !strcmp(g_rows[i].key, key))
            return i;
    return -1;
}

/* The accepted numbers of a list "1|2|4": is N one? */
static int in_numbers(const char *list, int n)
{
    while (*list) {
        if (atoi(list) == n)
            return 1;
        const char *bar = strchr(list, '|');
        if (!bar)
            break;
        list = bar + 1;
    }
    return 0;
}

/* One `key = value` of a file (or --set): the table's row for the key reads the value. */
static void apply(const char *key, const char *v, int line)
{
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        if (s->prefix ? strncmp(key, s->key, strlen(s->key)) : strcmp(key, s->key))
            continue;
        switch (s->type) {
        case SET_FLAG:
            *(int *)s->st = flag(v);
            break;
        case SET_TRI:
            *(int *)s->st = flag(v) ? 1 : -1;
            break;
        case SET_VSYNC:
            *(int *)s->st = !SDL_strcasecmp(v, "auto") ? -1 : flag(v);
            break;
        case SET_CHOICE: {
            int n = atoi(v);
            if (in_numbers(s->choices, n))
                *(int *)s->st = n;
            else
                note("%s: %s is %s: ignored", at(line), s->key, TX(s->rule));
            break;
        }
        case SET_RANGE: {
            int n = atoi(v);
            if (n < s->lo || n > s->hi) {
                note("%s: %s is %d to %d: ignored", at(line), s->key, s->lo, s->hi);
                n = 0;
            }
            *(int *)s->st = n;
            break;
        }
        case SET_TEXT:
            snprintf((char *)s->st, s->cap, "%s", v);
            break;
        case SET_KEY: {
            int r = kbd_setting(key, v);
            if (r == -1)
                r = states_setting(key, v);
            if (r == -1)
                r = freecam_setting(key, v);
            if (r == -1)
                r = pace_setting(key, v);
            if (r == -1)
                continue;
            if (r == -2)
                note("%s: \"%s\" is not a key name", at(line), v);
            break;
        }
        case SET_BIND: {
            int r = input_bind(key, v);
            if (r == -1)
                note("%s: no button \"%s\"", at(line), key + 4);
            else if (r == -2)
                note("%s: \"%s\" is not a %s name", at(line), v, key[0] == 'k' ? "key" : "gamepad button");
            break;
        }
        case SET_PACK: {
            int r = packs_setting(key, v);
            if (r == -2)
                note("%s: %s is on or off", at(line), key);
            else if (r == -1)
                note("%s: \"%s\" is not a pack's name", at(line), key);
            break;
        }
        case SET_OLD:
            note("%s: \"%s\" %s", at(line), key, TX(s->help));
            break;
        }
        return;
    }
    note("%s: unknown setting \"%s\"", at(line), key);
}

/* ------------------------------------------------------------ the command line's setting options */

static unsigned char g_given[sizeof(g_rows) / sizeof(g_rows[0])];

static void *cfg_at(size_t off)
{
    return (char *)&g_cfg + off;
}

int settings_cli(int argc, char **argv, int *i)
{
    const char *a = argv[*i];
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        if (!s->opt || strcmp(a, s->opt))
            continue;
        if (!s->arg) {
            *(int *)cfg_at(s->cfg) = s->onval;
            g_given[k] = 1;
            return 1;
        }
        if (*i + 1 >= argc)
            return -1;
        const char *v = argv[++*i];
        switch (s->ckind) {
        case C_STR:
            *(const char **)cfg_at(s->cfg) = v;
            break;
        case C_NUM: {
            int n = atoi(v);
            if (s->type == SET_CHOICE ? !in_numbers(s->choices, n) : (n < s->lo || n > s->hi))
                return -1;
            *(int *)cfg_at(s->cfg) = n;
            break;
        }
        case C_VSYNC:
            if (strcmp(v, "on") && strcmp(v, "off") && strcmp(v, "auto"))
                return -1;
            *(int *)cfg_at(s->cfg) = !strcmp(v, "on") ? 1 : !strcmp(v, "off") ? 0 : -1;
            break;
        default:
            return -1;
        }
        g_given[k] = 1;
        return 1;
    }
    return 0;
}

void settings_usage_words(FILE *f)
{
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        if (s->opt && !s->hidden)
            fprintf(f, " [%s%s%s]", s->opt, s->arg ? " " : "", s->arg ? s->arg : "");
    }
}

void settings_help(FILE *f)
{
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        if (!s->opt || s->hidden)
            continue;
        char left[80];
        snprintf(left, sizeof(left), "%s%s%s", s->opt, s->arg ? " " : "", s->arg ? s->arg : "");
        const char *text = TX(s->help ? s->help : s->label);
        fprintf(f, "  %-26s %s%s%s%s\n", left, text, s->key ? " (mh2.ini: " : "", s->key ? s->key : "", s->key ? ")" : "");
    }
    fprintf(f, "\nSettings of mh2.ini that have no option:\n");
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        if (s->opt || s->hidden || s->type == SET_OLD || !s->help)
            continue;
        fprintf(f, "  %-26s %s\n", s->prefix ? "key.NAME, pad.NAME" : s->key, TX(s->help));
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
    FILE *f = hfs_fopen(path, "r");
    if (!f)
        return 0;
    note("settings from %s", path);
    snprintf(g_loaded, sizeof(g_loaded), "%s", path);
    char buf[512];
    int line = 0;
    while (fgets(buf, sizeof(buf), f)) {
        line++;
        char conv[2 * sizeof(buf) + 8];   /* a line written in the ANSI code page (an older file) is read as such (hostfs.h) */
        const char *u = hfs_utf8(buf, conv, sizeof(conv));
        if (u != buf)
            snprintf(buf, sizeof(buf), "%s", u);
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
    const char *eq = strchr(line, '=');
    if (eq) {   /* the launcher shows it as given, and does not save over it */
        char key[128];
        size_t n = (size_t)(eq - line);
        if (n < sizeof(key)) {
            memcpy(key, line, n);
            key[n] = 0;
            char *k = trim(key);
            for (char *q = k; *q; q++)
                *q = (char)SDL_tolower((unsigned char)*q);
            int row = settings_find(k);
            if (row >= 0)
                g_given[row] = 1;
        }
    }
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
    const char *pref = play_data_dir();
    if (pref) {
        snprintf(path, sizeof(path), "%smh2.ini", pref);
        load(path);
    }
    apply_sets();
}

/* ssaa and hires, as the settings leave them: hires implies ssaa 4 at least; 1 is off. */
void settings_normalize(void)
{
    if (g_cfg.hires && g_cfg.ssaa < 4)
        g_cfg.ssaa = 4;
    if (g_cfg.ssaa == 1)
        g_cfg.ssaa = 0;
}

/* The settings under the command line's choices (which win). */
void settings_apply(int cli_fullscreen, int cli_mute)
{
    (void)cli_fullscreen;   /* an option of the command line has set its field already: the file only adds (AP_ON) */
    (void)cli_mute;
    for (int k = 0; k < NROWS; k++) {
        const Setting *s = &g_rows[k];
        size_t at = s->merge ? s->merge : s->cfg;
        switch (s->how) {
        case AP_ON:
            if (*(int *)s->st > 0)
                *(int *)cfg_at(at) = s->onval;
            break;
        case AP_TEXT:
            if (!*(const char **)cfg_at(at) && ((char *)s->st)[0])
                *(const char **)cfg_at(at) = (const char *)s->st;
            break;
        case AP_TEXT_SET:
            if (((char *)s->st)[0])
                *(const char **)cfg_at(at) = (const char *)s->st;
            break;
        case AP_TEXMB:
            if (st.texture_cache_mb)
                g_cfg.texture_cache_mb = st.texture_cache_mb;
            break;
        case AP_PRELOAD:
            g_cfg.texture_no_preload = st.texture_preload < 0;
            break;
        case AP_VSYNC:
            if (g_cfg.vsync == -2)   /* not on the command line */
                g_cfg.vsync = st.vsync == -2 ? -1 : st.vsync;
            break;
        case AP_SSAA:
            if (!g_cfg.ssaa)
                g_cfg.ssaa = st.ssaa;
            break;
        default:
            break;
        }
    }
    settings_normalize();
}

/* ------------------------------------------------------------ the launcher's session (settings.h) */

#define SBUF 1024
static char g_buf[sizeof(g_rows) / sizeof(g_rows[0])][SBUF];    /* settings_text's answers */
static char g_base[sizeof(g_rows) / sizeof(g_rows[0])][SBUF];   /* the values the session began with */
static char g_cur[sizeof(g_rows) / sizeof(g_rows[0])][SBUF];    /* the values now (a text's pointer in HostConfig is here) */

/* A row the launcher shows and the file takes. */
static int owned(int i)
{
    const Setting *s = &g_rows[i];
    return s->page != PAGE_NONE && s->page != PAGE_DEV &&
           (s->type == SET_FLAG || s->type == SET_VSYNC || s->type == SET_CHOICE || s->type == SET_TEXT ||
            s->type == SET_RANGE || s->type == SET_TRI);
}

const char *settings_text(int i)
{
    const Setting *s = &g_rows[i];
    char *b = g_buf[i];
    b[0] = 0;
    if (s->type == SET_KEY) {
        snprintf(b, SBUF, "%s", !strncmp(s->key, "state_key_", 10)  ? states_key_text(s->key)
                                : !strncmp(s->key, "freecam_key", 11) ? freecam_key_text(s->key)
                                : !strncmp(s->key, "pace_key", 8) ? pace_key_text(s->key)
                                                                      : kbd_key_name(s->key));
    } else if (s->cfg && s->ckind) {
        switch (s->ckind) {
        case C_FLAG:
            snprintf(b, SBUF, "%s", (*(int *)cfg_at(s->cfg) != 0) != (s->how == AP_INVERT || s->how == AP_PRELOAD) ? "on" : "off");
            break;
        case C_VSYNC: {
            int v = *(int *)cfg_at(s->cfg);
            snprintf(b, SBUF, "%s", v == 1 ? "on" : v == 0 ? "off" : "auto");
            break;
        }
        case C_NUM: {
            int v = *(int *)cfg_at(s->cfg);
            if (s->type == SET_CHOICE)
                snprintf(b, SBUF, "%d", v < 2 ? (s->dflt ? atoi(s->dflt) : 1) : v);   /* (no value: the choice's default, 1 for the samples, 30 for the frame rate) */
            else if (v)
                snprintf(b, SBUF, "%d", v);
            break;
        }
        case C_STR: {
            const char *p = *(const char **)cfg_at(s->cfg);
            snprintf(b, SBUF, "%s", p && *p ? p : s->choices && s->dflt ? s->dflt : p ? p : "");   /* (a choice list shows its default) */
            break;
        }
        }
    } else if (s->type == SET_FLAG && s->st) {
        snprintf(b, SBUF, "%s", *(int *)s->st > 0 ? "on" : "off");
    } else if (s->type == SET_TEXT && s->st) {
        snprintf(b, SBUF, "%s", (char *)s->st);
    }
    return b;
}

int settings_given(int i)
{
    return i >= 0 && i < NROWS && g_given[i];
}

void settings_session_begin(void)
{
    for (int i = 0; i < NROWS; i++)
        if (owned(i)) {
            snprintf(g_base[i], SBUF, "%s", settings_text(i));
            snprintf(g_cur[i], SBUF, "%s", g_base[i]);
        }
}

int settings_changed(int i)
{
    return owned(i) && strcmp(g_cur[i], g_base[i]) != 0;
}

int settings_dirty(void)
{
    if (packs_changed())
        return 1;
    for (int i = 0; i < NROWS; i++)
        if (settings_changed(i) && !g_given[i])
            return 1;
    return 0;
}

static const char *choice_words(const Setting *s)
{
    return s->choices ? s->choices : s->type == SET_FLAG ? "off|on" : NULL;
}

/* The N-th word of a "a|b|c" list (a static buffer), or NULL. */
static const char *nth_word(const char *list, int n)
{
    static char w[128];
    while (list && n-- > 0) {
        list = strchr(list, '|');
        if (list)
            list++;
    }
    if (!list)
        return NULL;
    size_t len = strcspn(list, "|");
    if (len >= sizeof(w))
        len = sizeof(w) - 1;
    memcpy(w, list, len);
    w[len] = 0;
    return w;
}

static int choice_index(int i, const char *value);

int settings_choice_pos(int i)
{
    return choice_index(i, g_cur[i]);
}

int settings_choice_count(int i)
{
    const char *l = choice_words(&g_rows[i]);
    if (!l)
        return 0;
    int n = 1;
    for (; *l; l++)
        n += *l == '|';
    return n;
}

const char *settings_choice_label(int i, int n)
{
    const Setting *s = &g_rows[i];
    const char *l = s->shown ? TX(s->shown) : s->type == SET_FLAG ? TX(T_CH_FLAG) : choice_words(s);
    return nth_word(l, n);
}

static int choice_index(int i, const char *value)
{
    const char *l = choice_words(&g_rows[i]);
    for (int n = 0; l; n++) {
        const char *w = nth_word(l, n);
        if (!w)
            break;
        if (!SDL_strcasecmp(w, value))
            return n;
    }
    return -1;
}

const char *settings_shown(int i)
{
    static char b[SBUF];
    int n = choice_index(i, g_cur[i]);
    if (n >= 0)
        return settings_choice_label(i, n);
    snprintf(b, sizeof(b), "%s", g_cur[i][0] ? g_cur[i] : TX(T_DEFAULT));
    return b;
}

void settings_put(int i, const char *text)
{
    const Setting *s = &g_rows[i];
    if (!owned(i))
        return;
    if (s->cfg && s->ckind) {
        switch (s->ckind) {
        case C_FLAG:
            *(int *)cfg_at(s->cfg) = (flag(text) != 0) != (s->how == AP_INVERT || s->how == AP_PRELOAD) ? s->onval : 0;
            break;
        case C_VSYNC:
            *(int *)cfg_at(s->cfg) = !SDL_strcasecmp(text, "auto") ? -1 : flag(text);
            break;
        case C_NUM:
            *(int *)cfg_at(s->cfg) = atoi(text);
            break;
        case C_STR:
            snprintf(g_cur[i], SBUF, "%s", text);
            *(const char **)cfg_at(s->cfg) = g_cur[i][0] ? g_cur[i] : NULL;
            break;
        }
    } else if (s->type == SET_FLAG) {
        *(int *)s->st = flag(text);
    } else if (s->type == SET_TEXT) {
        snprintf((char *)s->st, s->cap, "%s", text);
    }
    settings_normalize();
    for (int j = 0; j < NROWS; j++)   /* the coupling (hires and ssaa) moves other rows too */
        if (owned(j))
            snprintf(g_cur[j], SBUF, "%s", settings_text(j));
}

int settings_step(int i, int dir)
{
    const Setting *s = &g_rows[i];
    int count = settings_choice_count(i);
    if (!owned(i) || count < 2)
        return 0;
    int n = choice_index(i, g_cur[i]);
    int m;
    if (n < 0 && s->type == SET_RANGE) {   /* a number of the file between the listed ones: the next listed one that way */
        int cur = SDL_atoi(g_cur[i][0] ? g_cur[i] : s->dflt ? s->dflt : "");
        m = -1;
        for (int k = 0; k < count; k++) {
            int v = SDL_atoi(nth_word(choice_words(s), k));
            if (dir > 0 && v > cur) {
                m = k;
                break;
            }
            if (dir < 0 && v < cur)
                m = k;
        }
        if (m < 0)
            return 0;
        n = m;
    } else {
        if (n < 0)
            n = 0;
        if ((s->type == SET_FLAG || s->type == SET_TRI) && count == 2)
            m = 1 - n;
        else
            m = n + dir < 0 ? 0 : n + dir >= count ? count - 1 : n + dir;
        if (m == n && n == choice_index(i, g_cur[i]))
            return 0;
    }
    char word[SBUF];
    snprintf(word, sizeof(word), "%s", nth_word(choice_words(s), m));
    char before[SBUF];
    snprintf(before, sizeof(before), "%s", g_cur[i]);
    settings_put(i, word);
    return strcmp(before, g_cur[i]) != 0;
}

int settings_is_default(int i)
{
    const Setting *s = &g_rows[i];
    return !SDL_strcasecmp(g_cur[i], s->dflt ? s->dflt : "");
}

void settings_default(int i)
{
    settings_put(i, g_rows[i].dflt ? g_rows[i].dflt : "");
}

/* ------------------------------------------------------------ the writer (the launcher design, 5.1) */

const char *settings_save_path(void)
{
    static char path[1200];
    if (g_cfg.config_path)
        return g_cfg.config_path;
    if (g_loaded[0])
        return g_loaded;
    const char *pref = play_data_dir();   /* (NULL in an automated run: nothing is saved there) */
    if (!pref)
        return NULL;
    snprintf(path, sizeof(path), "%smh2.ini", pref);
    return path;
}

typedef struct Line {
    const char *p;
    size_t len;   /* without its end of line */
    size_t eol;   /* the end of line's length: 0, 1 or 2 */
} Line;

/* The loader's reading of a line: the key (lowercased) and where its value starts and ends. 0: not a setting line. */
static int line_key(const Line *l, char *key, size_t kcap, size_t *vs, size_t *ve)
{
    size_t a = 0, e = l->len;
    while (a < e && (l->p[a] == ' ' || l->p[a] == '\t'))
        a++;
    if (a == e || l->p[a] == '[' || l->p[a] == '#' || l->p[a] == ';')
        return 0;
    size_t c = e;
    for (size_t k = a; k < e; k++)
        if (l->p[k] == '#' || l->p[k] == ';') {
            c = k;
            break;
        }
    size_t eq = a;
    while (eq < c && l->p[eq] != '=')
        eq++;
    if (eq == c)
        return 0;
    size_t ke = eq;
    while (ke > a && (l->p[ke - 1] == ' ' || l->p[ke - 1] == '\t'))
        ke--;
    if (ke - a >= kcap)
        return 0;
    for (size_t k = a; k < ke; k++)
        key[k - a] = (char)SDL_tolower((unsigned char)l->p[k]);
    key[ke - a] = 0;
    size_t s = eq + 1;
    while (s < c && (l->p[s] == ' ' || l->p[s] == '\t'))
        s++;
    size_t t = c;
    while (t > s && (l->p[t - 1] == ' ' || l->p[t - 1] == '\t' || l->p[t - 1] == '\r'))
        t--;
    *vs = s;
    *ve = t;
    return 1;
}

/* A line of a pack key (pack.NAME): the loader's reading of it. */
static int is_pack_line(const Line *l)
{
    char key[PACK_NAME_MAX + 16];
    size_t vs, ve;
    return line_key(l, key, sizeof(key), &vs, &ve) && !strncmp(key, "pack.", 5);
}

typedef struct Out {
    char *p;
    size_t len, cap;
    int fail;
} Out;

static void out_add(Out *o, const char *s, size_t n)
{
    if (o->fail)
        return;
    if (o->len + n + 1 > o->cap) {
        size_t nc = (o->cap ? o->cap : 4096) * 2 + n;
        char *np = realloc(o->p, nc);
        if (!np) {
            o->fail = 1;
            return;
        }
        o->p = np;
        o->cap = nc;
    }
    memcpy(o->p + o->len, s, n);
    o->len += n;
    o->p[o->len] = 0;
}

/* The list of packs as lines: pack.NAME = on|off, in the list's order. */
static void out_packs(Out *o, const char *nl, size_t nll)
{
    for (int i = 0; i < packs_key_count(); i++) {
        int on = 0;
        const char *name = packs_key(i, &on);
        out_add(o, "pack.", 5);
        out_add(o, name, strlen(name));
        out_add(o, on ? " = on" : " = off", on ? 5 : 6);
        out_add(o, nl, nll);
    }
}

int settings_save(char *err, size_t cap)
{
    if (!settings_dirty())
        return 0;
    const char *path = settings_save_path();
    if (!path) {
        snprintf(err, cap, "there is no folder for mh2.ini");
        return -1;
    }
    /* the file as it is */
    char *text = NULL;
    size_t tlen = 0;
    FILE *f = hfs_fopen(path, "rb");
    if (f) {
        char chunk[4096];
        size_t n;
        Out in = {0};
        while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
            out_add(&in, chunk, n);
        fclose(f);
        if (in.fail) {
            free(in.p);
            snprintf(err, cap, "out of memory");
            return -1;
        }
        text = in.p;
        tlen = in.len;
    }
    int nlines = 0, lcap = 0;
    Line *lines = NULL;
    for (size_t k = 0; k < tlen;) {
        size_t e = k;
        while (e < tlen && text[e] != '\n')
            e++;
        size_t len = e - k, eol = e < tlen ? 1 : 0;
        if (nlines == lcap) {
            lcap = lcap ? lcap * 2 : 64;
            Line *nl = realloc(lines, sizeof(Line) * (size_t)lcap);
            if (!nl) {
                free(lines);
                free(text);
                snprintf(err, cap, "out of memory");
                return -1;
            }
            lines = nl;
        }
        lines[nlines++] = (Line){text + k, len, eol};
        k = e < tlen ? e + 1 : e;
    }
    /* each line's end of line: CRLF files keep it */
    const char *nl = nlines && lines[0].eol && lines[0].len && lines[0].p[lines[0].len - 1] == '\r' ? "\r\n" : "\n";
    size_t nll = strlen(nl);
    /* the new values, by line: replaced in place; a key not in the file is appended */
    char **repl = calloc((size_t)nlines + 1, sizeof(char *));   /* a line's new value, or NULL */
    unsigned char *pending = calloc(NROWS, 1);
    if (!repl || !pending) {
        free(repl);
        free(pending);
        free(lines);
        free(text);
        snprintf(err, cap, "out of memory");
        return -1;
    }
    for (int i = 0; i < NROWS; i++) {
        if (!settings_changed(i) || g_given[i])
            continue;
        int last = -1;
        for (int k = 0; k < nlines; k++) {
            char key[128];
            size_t vs, ve;
            if (line_key(&lines[k], key, sizeof(key), &vs, &ve) && !strcmp(key, g_rows[i].key))
                last = k;
        }
        if (last >= 0) {
            repl[last] = g_cur[i];
            pending[i] = 2;   /* done in place */
        } else if (!settings_is_default(i)) {
            pending[i] = 1;
        }
    }
    Out o = {0};
    int launcher_end = -1, in_launcher = 0, last_nonblank = -1;
    for (int k = 0; k < nlines; k++) {
        const Line *l = &lines[k];
        size_t a = 0;
        while (a < l->len && (l->p[a] == ' ' || l->p[a] == '\t'))
            a++;
        if (a < l->len && l->p[a] == '[') {
            in_launcher = l->len - a >= 10 && !SDL_strncasecmp(l->p + a, "[launcher]", 10);
            if (in_launcher)
                launcher_end = k;
        } else if (in_launcher && a < l->len) {
            launcher_end = k;
        }
        if (a < l->len)
            last_nonblank = k;
    }
    int any_pending = 0;
    for (int i = 0; i < NROWS; i++)
        any_pending |= pending[i] == 1;
    /* the packs the page changed: every pack line goes and the list takes the first one's place; with no line, it goes where
     * the new keys go */
    int packs_edit = packs_changed(), pack_at = -1;
    if (packs_edit)
        for (int k = 0; k < nlines && pack_at < 0; k++)
            if (is_pack_line(&lines[k]))
                pack_at = k;
    int pack_pending = packs_edit && pack_at < 0 && packs_key_count() > 0;
    any_pending |= pack_pending;
    int inserted = 0;
    for (int k = 0; k < nlines; k++) {
        const Line *l = &lines[k];
        char key[128];
        size_t vs, ve;
        if (packs_edit && is_pack_line(l)) {   /* (the insertion below still follows it if it ends the [launcher] section) */
            if (k == pack_at)
                out_packs(&o, nl, nll);
        } else {
            if (repl[k] && line_key(l, key, sizeof(key), &vs, &ve)) {
                out_add(&o, l->p, vs);
                out_add(&o, repl[k], strlen(repl[k]));
                out_add(&o, l->p + ve, l->len - ve);
            } else {
                out_add(&o, l->p, l->len);
            }
            if (l->eol)
                out_add(&o, l->p + l->len, 1);
            else if (k + 1 < nlines || any_pending)
                out_add(&o, nl, nll);
        }
        if (any_pending && k == launcher_end && !inserted) {   /* an existing [launcher] section takes them */
            for (int i = 0; i < NROWS; i++)
                if (pending[i] == 1) {
                    out_add(&o, g_rows[i].key, strlen(g_rows[i].key));
                    out_add(&o, " = ", 3);
                    out_add(&o, g_cur[i], strlen(g_cur[i]));
                    out_add(&o, nl, nll);
                }
            if (pack_pending)
                out_packs(&o, nl, nll);
            inserted = 1;
        }
    }
    if (any_pending && !inserted) {
        if (last_nonblank >= 0 && last_nonblank == nlines - 1)
            out_add(&o, nl, nll);
        out_add(&o, "[launcher]", 10);
        out_add(&o, nl, nll);
        for (int i = 0; i < NROWS; i++)
            if (pending[i] == 1) {
                out_add(&o, g_rows[i].key, strlen(g_rows[i].key));
                out_add(&o, " = ", 3);
                out_add(&o, g_cur[i], strlen(g_cur[i]));
                out_add(&o, nl, nll);
            }
        if (pack_pending)
            out_packs(&o, nl, nll);
    }
    free(repl);
    free(pending);
    free(lines);
    free(text);
    if (o.fail) {
        free(o.p);
        snprintf(err, cap, "out of memory");
        return -1;
    }
    /* through FILE.tmp, renamed over the file: a crash never leaves half a file */
    char tmp[1300];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *w = hfs_fopen(tmp, "wb");
    int ok = w && fwrite(o.p ? o.p : "", 1, o.len, w) == o.len && fflush(w) == 0;
    if (w && fclose(w) != 0)
        ok = 0;
    free(o.p);
    if (!ok || !SDL_RenamePath(tmp, path)) {
        snprintf(err, cap, "cannot write %s", path);
        hfs_remove(tmp);
        return -1;
    }
    for (int i = 0; i < NROWS; i++)
        if (owned(i))
            snprintf(g_base[i], SBUF, "%s", g_cur[i]);
    packs_saved();
    return 0;
}

const char *settings_enhancements(void)
{
    static char b[160];
    if (g_cfg.ssaa_asked) {   /* gs.c: the renderer could not make the samples and runs at one */
        snprintf(b, sizeof(b), "off (ssaa %d%s asked: the graphics card could not make the samples; one sample)", g_cfg.ssaa_asked,
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

/* Where the disc archive is: the core's --disc, mh2's --archive, the setting, or the first found. 0 with PATH set (a named
 * one need not be a file), or -1 (none found). */
static int disc_find(char *path, size_t cap)
{
    if (has_arg("--disc")) {
        for (int i = 0; i + 1 < g_cfg.core_nargs; i++)
            if (!strcmp(g_cfg.core_args[i], "--disc"))
                snprintf(path, cap, "%s", g_cfg.core_args[i + 1]);
    } else if (g_cfg.archive) {
        snprintf(path, cap, "%s", g_cfg.archive);
    } else if (st.archive[0]) {
        snprintf(path, cap, "%s", st.archive);
    } else {
        const char *base = SDL_GetBasePath();
        const char *pref = play_data_dir();   /* (NULL in an automated run: the guard, main.c) */
        path[0] = 0;
        if (base) {
            snprintf(path, cap, "%smh2-disc.arc", base);
            if (!is_file(path) && pref)
                snprintf(path, cap, "%smh2-disc.arc", pref);
            if (!is_file(path))   /* the build tree: build/pc/<platform>-x86_64/mh2 -> build/port */
                snprintf(path, cap, "%s../../port/mh2-disc.arc", base);
        }
        if (!path[0] || !is_file(path))
            return -1;
    }
    return 0;
}

/* The disc archive for the core: 0 (--disc given to the core and g_cfg.disc_path set), or -1 (none). */
int launcher_disc(void)
{
    static char path[1200];
    if (disc_find(path, sizeof(path)) != 0)
        return -1;
    if (!is_file(path)) {
        host_log("the game archive %s is not a file", path);
        return -1;
    }
    if (!has_arg("--disc"))
        launcher_core_arg("--disc", path);
    g_cfg.disc_path = path;   /* MH2_DISC too (process.c): io's default archive; io finds the movie table beside it */
    host_log("game archive: %s", path);
    return 0;
}

/* The archive the settings name: --archive (or the launcher's choice), else mh2.ini's archive; "" if none. */
const char *settings_archive(void)
{
    return g_cfg.archive && g_cfg.archive[0] ? g_cfg.archive : st.archive;
}

/* The archive the launcher shows: 0 with PATH the file found; -1 with PATH where it would be made (the setting archive, else
 * mh2-disc.arc in the user data folder, else beside mh2). */
int launcher_disc_path(char *path, size_t cap)
{
    if (disc_find(path, cap) == 0 && is_file(path))
        return 0;
    const char *dd = play_data_dir();
    if (g_cfg.archive && g_cfg.archive[0])
        snprintf(path, cap, "%s", g_cfg.archive);
    else if (st.archive[0])
        snprintf(path, cap, "%s", st.archive);
    else if (dd)
        snprintf(path, cap, "%smh2-disc.arc", dd);
    else {
        const char *base = SDL_GetBasePath();
        snprintf(path, cap, "%smh2-disc.arc", base ? base : "");
    }
    return -1;
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

/* The offline hub reaches the core as its own option, --offline-hub. */
void launcher_offline_hub(void)
{
    if (has_arg("--offline-hub"))
        g_cfg.offline_hub = 1;
    if (!g_cfg.offline_hub)
        return;
    if (!has_arg("--offline-hub"))
        launcher_core_flag("--offline-hub");
    host_log("offline hub: on (the village's gate leads to the online town, played with no network)");
}

/* The text pack reaches the core as its folder, --pack DIR: the setting's NAME is a folder of text_packs in the user data
 * folder, or (with a separator in it) a path. A pack that was asked for and is not there stops the core (it says why). */
void launcher_text_pack(void)
{
    const char *name = g_cfg.text_pack;
    static char dir[1200];
    if (!name || !*name || !strcmp(name, "off") || has_arg("--pack"))
        return;
    if (strpbrk(name, "/\\")) {
        snprintf(dir, sizeof(dir), "%s", name);
    } else if (play_data_dir()) {
        snprintf(dir, sizeof(dir), "%stext_packs/%s", play_data_dir(), name);
    } else {
        host_log("text_pack = %s: there is no user data folder to look in (name a path)", name);
        snprintf(dir, sizeof(dir), "%s", name);
    }
    host_log("translation pack: %s", dir);
    launcher_core_arg("--pack", dir);
}


#ifdef MH2_SETTINGS_TEST
/* The settings test (pc/host/tests/settings_writer_test.c) starts each case from nothing. */
void settings_test_reset(void)
{
    packs_reset();
    memset(&st, 0, sizeof(st));
    st.fullscreen = -1;
    st.mute = -1;
    st.vsync = -2;
    st.launcher = 1;
    memset(g_given, 0, sizeof(g_given));
    memset(g_base, 0, sizeof(g_base));
    memset(g_cur, 0, sizeof(g_cur));
    g_loaded[0] = 0;
    g_msgs[0] = 0;
    g_nmsgs = 0;
    for (int i = 0; i < g_nsets; i++)
        SDL_free(g_sets[i]);
    g_nsets = 0;
}
#endif
