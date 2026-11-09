/*
 * The camera on the right stick: the host's mapping. The game's camera is turned and tilted by
 * the D-pad (LEFT and RIGHT turn it at a fixed step while held, UP and DOWN step it through four heights and distances,
 * one step to a press), and the right stick is the attack stick. This file turns the stick into the D-pad the game
 * already reads, so the game sees a pad it knows and nothing in the game changes.
 *
 * camera_stick = off | hold | on (off by default):
 *   hold: the stick is the camera while the camera_stick_button is held (L3 by default, a button the game does not read in
 *         play); released, the stick is the game's again, once it has come back to rest.
 *   on:   the stick is always the camera (the attack stick is then the keyboard's, if a keyboard is used).
 * While the stick is the camera the game is given it at rest. camera_stick_which = right | left picks the stick,
 * camera_stick_invert = none | x | y | xy turns an axis round, camera_stick_speed = 25 | 50 | 75 | 100 is the share of the
 * game's own turn step (the whole step at full tilt, at 100).
 *
 * The turn is analog: the D-pad bit is held for a share of the frames that follows the tilt (an error-feedback
 * accumulator over the VBlank numbers, so a frame count, not the host's time, decides), and the game's own easing smooths
 * the steps. The tilt is the game's: a push beyond three fifths presses UP or DOWN once and, held, again every 24 VBlanks. The game
 * reads the pad once a frame (two VBlanks), so a decision lasts two VBlanks.
 */
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

enum {
    B_SELECT = 1 << 0, B_L3 = 1 << 1,
    B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7,
    B_L2 = 1 << 8, B_R2 = 1 << 9,
};

#define DEAD 38          /* the stick's rest zone, of 127 (the game's own is 45 of its scale) */
#define TILT_ON 76       /* three fifths: a press */
#define TILT_OFF 51      /* two fifths: the release */
#define UNIT 2           /* VBlanks to a decision: the game reads the pad once a frame, and a frame is two VBlanks (a bit held for
                          * one VBlank would be seen on every second frame or on none, by the frame's parity) */
#define TILT_PRESS 1     /* decisions a press lasts */
#define TILT_REPEAT 12   /* decisions between a held tilt's presses (24 VBlanks) */

typedef struct CamStickCfg {
    int mode;            /* 0 off, 1 hold, 2 on */
    int button;          /* the hold button's bit */
    int left;            /* the left stick */
    int inv_x, inv_y;
    int speed;           /* percent of the game's step */
} CamStickCfg;

static struct {
    uint64_t t;          /* the decision's number (the VBlank / UNIT) */
    int have;
    int acc;             /* the turn's accumulator, 0..255 and the carry */
    int turning;         /* the turn bit was on in the last VBlank's decision */
    int vdir;            /* -1 up, 1 down, 0 at rest */
    int vtimer;          /* VBlanks since the tilt's last press */
    int vpress;          /* VBlanks the press has left */
    int latch;           /* hold released with the stick off rest: the game gets it at rest until it comes back */
    int was_active;
    unsigned bits;       /* the decision: D-pad bits added */
} cs;

/* A fresh state (a new session; the tests). */
void camstick_reset(void)
{
    memset(&cs, 0, sizeof(cs));
}

static int cs_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* One VBlank's decision: the D-pad bits for a stick at (x, y), each -127 to 127 (y negative is up). */
static unsigned step(const CamStickCfg *c, int x, int y, int active)
{
    unsigned bits = 0;
    int ax = abs(x);
    int duty = 0;
    if (active && ax > DEAD)
        duty = (ax - DEAD) * 256 / (127 - DEAD) * c->speed / 100;
    duty = cs_clamp(duty, 0, 256);
    if (duty) {
        if (!cs.turning)
            cs.acc = 256 - duty;   /* the first VBlank of a push turns at once */
        cs.acc += duty;
        if (cs.acc >= 256) {
            cs.acc -= 256;
            bits |= ((x > 0) != (c->inv_x != 0)) ? B_RIGHT : B_LEFT;
        }
        cs.turning = 1;
    } else {
        cs.turning = 0;
        cs.acc = 0;
    }
    int ay = abs(y);
    int dir = !active ? 0 : ay >= TILT_ON ? (y > 0 ? 1 : -1) : ay <= TILT_OFF ? 0 : cs.vdir;
    if (dir != cs.vdir) {
        cs.vdir = dir;
        cs.vtimer = TILT_REPEAT;   /* a new push presses at once */
    }
    if (dir) {
        if (cs.vtimer >= TILT_REPEAT) {
            cs.vtimer = 0;
            cs.vpress = TILT_PRESS;
        }
        cs.vtimer++;
    } else {
        cs.vtimer = 0;
    }
    if (cs.vpress > 0) {
        cs.vpress--;
        bits |= ((cs.vdir > 0) != (c->inv_y != 0)) ? B_DOWN : B_UP;
    }
    return bits;
}

/* The mapping on a pad at the VBlank vb it is for (the core's next sample). */
static void camstick_map(const CamStickCfg *c, Mh2IpcPad *pad, uint64_t vb)
{
    uint64_t t = vb / UNIT;
    if (!c->mode)
        return;
    uint8_t *sx = c->left ? &pad->lx : &pad->rx;
    uint8_t *sy = c->left ? &pad->ly : &pad->ry;
    int x = *sx - 128, y = *sy - 128;
    int resting = abs(x) <= DEAD && abs(y) <= DEAD;
    int active = c->mode == 2 || (pad->buttons & c->button) != 0;
    if (c->mode == 1) {
        pad->buttons &= (uint16_t)~c->button;   /* the game does not see the button */
        if (c->button == B_L2)
            pad->pressure[10] = 0;
        else if (c->button == B_R2)
            pad->pressure[11] = 0;
    }
    if (cs.was_active && !active && !resting)
        cs.latch = 1;   /* let go with the stick off rest: it is not the game's until it comes back */
    if (cs.latch && resting)
        cs.latch = 0;
    cs.was_active = active;
    if (!cs.have || t != cs.t) {
        /* a new decision (or several, when the host fell behind): it runs once for each */
        uint64_t n = cs.have && t > cs.t ? t - cs.t : 1;
        if (n > 8)
            n = 8;
        for (uint64_t i = 0; i < n; i++)
            cs.bits = step(c, x, y, active);
        cs.t = t;
        cs.have = 1;
    }
    if (!active)
        cs.bits = 0;
    if (active || cs.latch)
        *sx = *sy = 0x80;   /* the stick is the camera's: the game is given it at rest */
    pad->buttons |= (uint16_t)cs.bits;
    if (cs.bits & B_RIGHT)
        pad->pressure[0] = 0xFF;
    if (cs.bits & B_LEFT)
        pad->pressure[1] = 0xFF;
    if (cs.bits & B_UP)
        pad->pressure[2] = 0xFF;
    if (cs.bits & B_DOWN)
        pad->pressure[3] = 0xFF;
}

static int word(const char *s, const char *const *list, int n, int dflt)
{
    for (int i = 0; s && i < n; i++)
        if (!SDL_strcasecmp(s, list[i]))
            return i;
    return dflt;
}

void camstick_apply(Mh2IpcPad *pad)
{
    static const char *const modes[] = {"off", "hold", "on"};
    static const char *const buttons[] = {"l3", "select", "l2", "r2"};
    static const int bit[] = {B_L3, B_SELECT, B_L2, B_R2};
    static const char *const inverts[] = {"none", "x", "y", "xy"};
    CamStickCfg c;
    memset(&c, 0, sizeof(c));
    c.mode = word(g_cfg.camera_stick, modes, 3, 0);
    if (!c.mode || freecam_controls())   /* the freecam has the pad: the stick is its look */
        return;
    c.button = bit[word(g_cfg.camera_stick_button, buttons, 4, 0)];
    c.left = word(g_cfg.camera_stick_which, (const char *const[]){"right", "left"}, 2, 0);
    int inv = word(g_cfg.camera_stick_invert, inverts, 4, 0);
    c.inv_x = inv & 1;
    c.inv_y = (inv >> 1) & 1;
    c.speed = g_cfg.camera_stick_speed ? atoi(g_cfg.camera_stick_speed) : 100;
    c.speed = cs_clamp(c.speed, 10, 100);
    camstick_map(&c, pad, gs_last_vblank() + 1);
}
