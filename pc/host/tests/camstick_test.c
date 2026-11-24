/*
 * The camera on the right stick (camstick.c): the stick as the D-pad. Off leaves the pad alone; on and hold turn the stick into
 * LEFT and RIGHT for a share of the VBlanks that follows the tilt, and into UP and DOWN presses; the game is given the stick at
 * rest while it is the camera's, and after the hold button's release until the stick comes back to rest.
 *   host_camstick_test
 */
#include <stdio.h>
#include <string.h>

#include "mh2host.h"

HostConfig g_cfg;
static uint64_t g_vblank;
static int g_freecam;

uint64_t gs_last_vblank(void)
{
    return g_vblank;
}

int freecam_controls(void)
{
    return g_freecam;
}

static int bad;

static void Check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL: %s\n", what);
        bad++;
    }
}

enum { B_L3 = 1 << 1, B_L2 = 1 << 8, B_UP = 1 << 4, B_RIGHT = 1 << 5, B_DOWN = 1 << 6, B_LEFT = 1 << 7, B_CROSS = 1 << 14 };

static void Cfg(const char *mode, const char *inv, const char *speed, const char *which)
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.camera_stick = mode;
    g_cfg.camera_stick_button = "l3";
    g_cfg.camera_stick_which = which;
    g_cfg.camera_stick_invert = inv;
    g_cfg.camera_stick_speed = speed;
    g_vblank = 0;
    g_freecam = 0;
    camstick_reset();
}

static Mh2IpcPad Pad(int buttons, int rx, int ry)
{
    Mh2IpcPad p;
    memset(&p, 0, sizeof(p));
    p.connected = 1;
    p.buttons = (uint16_t)buttons;
    p.rx = (uint8_t)rx;
    p.ry = (uint8_t)ry;
    p.lx = p.ly = 0x80;
    return p;
}

/* the share of the next n VBlanks the pad carries bit */
static int Count(int n, int buttons, int rx, int ry, unsigned bit)
{
    int c = 0;
    for (int i = 0; i < n; i++) {
        g_vblank++;
        Mh2IpcPad p = Pad(buttons, rx, ry);
        camstick_apply(&p);
        c += (p.buttons & bit) != 0;
    }
    return c;
}

/* A hash (FNV-1a, 64 bits) of the pads the mapping makes over a fixed sweep: every setting of the mode, the invert, the speed
 * and the stick; sticks over the whole range, the hold button and the player's own buttons coming and going, the VBlanks
 * counting up with gaps. smooth 1 sets the smooth camera's row. */
static uint64_t SweepHash(int smooth)
{
    static const char *const modes[] = {"on", "hold"}, *const invs[] = {"none", "x", "y", "xy"};
    static const char *const speeds[] = {"25", "50", "75", "100"}, *const whiches[] = {"right", "left"};
    uint64_t h = 1469598103934665603ull;
    for (int m = 0; m < 2; m++)
        for (int iv = 0; iv < 4; iv++)
            for (int sp = 0; sp < 4; sp++)
                for (int wh = 0; wh < 2; wh++) {
                    Cfg(modes[m], invs[iv], speeds[sp], whiches[wh]);
                    g_cfg.camera_smooth = smooth;
                    for (int i = 0; i < 1500; i++) {
                        g_vblank += 1 + (i % 7 == 0);
                        int x = (i * 37 + (i >> 5) * 11) & 255, y = (i * 91 + (i >> 3) * 5) & 255;
                        int bt = ((i & 64) ? B_L3 : 0) | ((i & 128) ? B_CROSS : 0) | ((i & 512) ? B_UP : 0);
                        Mh2IpcPad p = Pad(bt, x, y);
                        if (wh) {
                            p.lx = (uint8_t)x;
                            p.ly = (uint8_t)y;
                            p.rx = (uint8_t)(255 - x);
                            p.ry = (uint8_t)(255 - y);
                        }
                        camstick_apply(&p);
                        unsigned char raw[21];   /* connected, buttons, the four sticks, the twelve pressures, the two camera numbers */
                        raw[0] = p.connected;
                        raw[1] = (unsigned char)(p.buttons & 255);
                        raw[2] = (unsigned char)(p.buttons >> 8);
                        raw[3] = p.rx, raw[4] = p.ry, raw[5] = p.lx, raw[6] = p.ly;
                        memcpy(raw + 7, p.pressure, 12);
                        raw[19] = p.camTurn, raw[20] = p.camTilt;
                        for (unsigned k = 0; k < 21; k++)
                            h = (h ^ raw[k]) * 1099511628211ull;
                    }
                }
    return h;
}

#define SWEEP_OFF 0x837e89f04be9db2bull

int main(void)
{
    /* off: nothing changes */
    Cfg("off", "none", "100", "right");
    {
        Mh2IpcPad p = Pad(B_CROSS | B_L3, 0xFF, 0x00), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "off leaves the pad alone");
    }
    /* on, full tilt right: RIGHT every VBlank, the stick at rest for the game */
    Cfg("on", "none", "100", "right");
    {
        g_vblank = 10;
        Mh2IpcPad p = Pad(B_CROSS, 0xFF, 0x80);
        camstick_apply(&p);
        Check((p.buttons & B_RIGHT) && !(p.buttons & B_LEFT), "full tilt right presses RIGHT at once");
        Check(p.rx == 0x80 && p.ry == 0x80, "the game is given the stick at rest");
        Check(p.pressure[0] == 0xFF, "RIGHT is pressed fully");
        Check(p.buttons & B_CROSS, "other buttons stay");
        Mh2IpcPad q = Pad(B_CROSS, 0xFF, 0x80);
        camstick_apply(&q);   /* the same VBlank */
        Check(!memcmp(&p, &q, sizeof(p)), "the same VBlank, the same pad");
        Check(Count(60, 0, 0xFF, 0x80, B_RIGHT) == 60, "full tilt: every VBlank");
        Check(Count(5, 0, 0x80, 0x80, B_RIGHT) == 0, "at rest: none");
        Check(Count(60, 0, 0x00, 0x80, B_LEFT) >= 59, "full tilt left: LEFT (at the next decision)");
    }
    /* analog: half way between the rest zone and the edge turns about half the VBlanks */
    Cfg("on", "none", "100", "right");
    {
        int c = Count(128, 0, 128 + 38 + (127 - 38) / 2, 0x80, B_RIGHT);
        Check(c >= 60 && c <= 68, "half tilt: about half the VBlanks");
        Check(Count(30, 0, 128 + 30, 0x80, B_RIGHT) == 0, "inside the rest zone: none");
    }
    /* speed */
    Cfg("on", "none", "50", "right");
    {
        int c = Count(100, 0, 0xFF, 0x80, B_RIGHT);
        Check(c >= 48 && c <= 52, "speed 50: half of the step at full tilt");
    }
    /* inversion */
    Cfg("on", "x", "100", "right");
    Check(Count(10, 0, 0xFF, 0x80, B_LEFT) == 10, "invert x: right turns left");
    Cfg("on", "y", "100", "right");
    Check(Count(2, 0, 0x80, 0x00, B_DOWN) == 2, "invert y: up is DOWN");
    /* tilt: a press of two VBlanks, then every 24 while held */
    Cfg("on", "none", "100", "right");
    {
        int seq[60];
        for (int i = 0; i < 60; i++) {
            g_vblank++;
            Mh2IpcPad p = Pad(0, 0x80, 0x00);
            camstick_apply(&p);
            seq[i] = (p.buttons & B_UP) != 0;
        }
        Check(seq[0] && seq[1] && !seq[2], "tilt up: a press of two VBlanks at once");
        Check(!seq[23] && seq[24] && seq[25] && !seq[26], "tilt up held: pressed again 24 VBlanks later");
        Check(seq[48] && seq[49], "and again");
        Check(Count(30, 0, 0x80, 0x80, B_UP) == 0, "let go: no press");
    }
    /* which stick: the left stick's axes */
    Cfg("on", "none", "100", "left");
    {
        g_vblank = 3;
        Mh2IpcPad p = Pad(0, 0xFF, 0x80);
        p.lx = 0xFF;
        camstick_apply(&p);
        Check((p.buttons & B_RIGHT) && p.lx == 0x80 && p.rx == 0xFF, "which = left: the left stick is the camera, the right the game's");
    }
    /* hold: nothing without the button; the button is not the game's; the stick comes back at rest */
    Cfg("hold", "none", "100", "right");
    {
        g_vblank = 1;
        Mh2IpcPad p = Pad(B_CROSS, 0xFF, 0x80), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "hold: without the button the stick is the game's");
        Check(Count(5, B_L3, 0xFF, 0x80, B_RIGHT) >= 4, "hold: with the button, RIGHT");
        g_vblank++;
        Mh2IpcPad r = Pad(B_L3 | B_CROSS, 0xFF, 0x80);
        camstick_apply(&r);
        Check(!(r.buttons & B_L3) && (r.buttons & B_CROSS) && r.rx == 0x80, "hold: the button is kept from the game");
        g_vblank++;
        Mh2IpcPad s = Pad(0, 0xFF, 0x80);   /* released, the stick still over */
        camstick_apply(&s);
        Check(s.rx == 0x80 && !(s.buttons & B_RIGHT), "hold: released, the game gets the stick at rest while it is over");
        g_vblank++;
        Mh2IpcPad u = Pad(0, 0x80, 0x80);
        camstick_apply(&u);
        g_vblank++;
        Mh2IpcPad w = Pad(0, 0xFF, 0x80);
        camstick_apply(&w);
        Check(w.rx == 0xFF, "hold: back at rest, the stick is the game's again");
    }
    /* hold with L2: the button and its pressure are kept from the game */
    Cfg("hold", "none", "100", "right");
    {
        g_cfg.camera_stick_button = "l2";
        g_vblank = 6;
        Mh2IpcPad p = Pad(B_L2 | B_CROSS, 0xFF, 0x80);
        p.pressure[10] = 0xFF;
        camstick_apply(&p);
        Check(!(p.buttons & B_L2) && (p.buttons & B_CROSS) && p.pressure[10] == 0, "hold with l2: the button and its pressure are withheld");
    }
    /* the freecam has the pad */
    Cfg("on", "none", "100", "right");
    {
        g_freecam = 1;
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x00), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "the freecam in control: nothing");
    }
    /* the smooth camera: the stick as two numbers, no D-pad bit */
    Cfg("on", "none", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 10;
        Mh2IpcPad p = Pad(B_CROSS | B_LEFT, 0xFF, 0x80);
        camstick_apply(&p);
        Check((int8_t)p.camTurn == 50 && (int8_t)p.camTilt == 0, "smooth: full right is turn 50, tilt 0");
        Check(p.buttons == (B_CROSS | B_LEFT), "smooth: no D-pad bit is added, the player's own stays");
        Check(p.pressure[0] == 0 && p.pressure[1] == 0 && p.pressure[2] == 0 && p.pressure[3] == 0, "smooth: no D-pad pressure");
        Check(p.rx == 0x80 && p.ry == 0x80, "smooth: the game is given the stick at rest");
        Mh2IpcPad q = Pad(0, 0x00, 0x80);
        camstick_apply(&q);
        Check((int8_t)q.camTurn == -50 && q.camTilt == 0, "smooth: full left is turn -50");
        Mh2IpcPad u = Pad(0, 0x80, 0x00);
        camstick_apply(&u);
        Check(u.camTurn == 0 && (int8_t)u.camTilt == 50, "smooth: full up is tilt 50 (looks up)");
        Mh2IpcPad d = Pad(0, 0x80, 0xFF);
        camstick_apply(&d);
        Check(d.camTurn == 0 && (int8_t)d.camTilt == -50, "smooth: full down is tilt -50");
        Mh2IpcPad r = Pad(0, 0x80, 0x80);
        camstick_apply(&r);
        Check(r.camTurn == 0 && r.camTilt == 0, "smooth: at rest, both 0");
        Mh2IpcPad e = Pad(0, 128 + 38, 0x80);
        camstick_apply(&e);
        Check(e.camTurn == 0, "smooth: the edge of the rest zone is 0");
        Mh2IpcPad l = Pad(0, 128 + 64, 0x80);
        camstick_apply(&l);
        Check((int8_t)l.camTurn == 9, "smooth: a light push (r 64) is 9");
        Mh2IpcPad g = Pad(0, 128 + 90, 128 - 90);
        camstick_apply(&g);
        Check((int8_t)g.camTurn == 35 && (int8_t)g.camTilt == 35, "smooth: a diagonal gives both (35 and 35)");
        Mh2IpcPad h = Pad(0, 128 - 90, 128 + 90);
        camstick_apply(&h);
        Check((int8_t)h.camTurn == -35 && (int8_t)h.camTilt == -35, "smooth: the opposite diagonal gives both, negative");
        /* stateless: the same stick, the same numbers at any VBlank, whatever came before */
        int same = 1;
        for (int v = 1; v < 40; v++) {
            g_vblank = (uint64_t)v * 3;
            Mh2IpcPad w = Pad(0, 128 + 64, 0x80);
            camstick_apply(&w);
            same &= (int8_t)w.camTurn == 9;
        }
        Check(same, "smooth: the numbers do not depend on the VBlank");
        /* monotone and symmetric over the whole range of one axis */
        int mono = 1, sym = 1, prev = 0;
        for (int k = 0; k <= 127; k++) {
            Mh2IpcPad a = Pad(0, 128 + k, 0x80), b = Pad(0, 128 - k, 0x80);
            camstick_apply(&a);
            camstick_apply(&b);
            int t = (int8_t)a.camTurn;
            mono &= t >= prev && t <= 50;
            sym &= (int8_t)b.camTurn == -t;
            prev = t;
        }
        Check(mono && sym, "smooth: the turn grows with the push, to 50, and left is right reversed");
    }
    Cfg("on", "none", "50", "right");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x80);
        camstick_apply(&p);
        Check((int8_t)p.camTurn == 25, "smooth: speed 50 turns at half");
        Mh2IpcPad q = Pad(0, 0x80, 0x00);
        camstick_apply(&q);
        Check((int8_t)q.camTilt == 50, "smooth: the turning speed does not change the tilt");
    }
    Cfg("on", "x", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x00);
        camstick_apply(&p);
        Check((int8_t)p.camTurn == -35 && (int8_t)p.camTilt == 35, "smooth: invert x turns the other way, the tilt stays (a diagonal, 35 each)");
    }
    Cfg("on", "y", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x00);
        camstick_apply(&p);
        Check((int8_t)p.camTurn == 35 && (int8_t)p.camTilt == -35, "smooth: invert y tilts the other way, the turn stays (a diagonal, 35 each)");
    }
    Cfg("on", "none", "100", "left");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x80);
        p.lx = 0xFF;
        camstick_apply(&p);
        Check((int8_t)p.camTurn == 50 && p.lx == 0x80 && p.rx == 0xFF, "smooth: which = left: the left stick is the camera, the right the game's");
    }
    /* smooth with hold: nothing without the button, the button withheld, the latch after a release */
    Cfg("hold", "none", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        g_vblank = 1;
        Mh2IpcPad p = Pad(B_CROSS, 0xFF, 0x80), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "smooth hold: without the button the stick is the game's and the numbers are 0");
        g_vblank++;
        Mh2IpcPad r = Pad(B_L3 | B_CROSS, 0xFF, 0x80);
        camstick_apply(&r);
        Check((int8_t)r.camTurn == 50 && !(r.buttons & B_L3) && (r.buttons & B_CROSS) && r.rx == 0x80,
              "smooth hold: with the button, the numbers; the button is kept from the game");
        g_vblank++;
        Mh2IpcPad s = Pad(0, 0xFF, 0x80);   /* released, the stick still over */
        camstick_apply(&s);
        Check(s.camTurn == 0 && s.camTilt == 0 && s.rx == 0x80, "smooth hold: released with the stick over: the numbers 0, the game gets rest");
        g_vblank++;
        Mh2IpcPad u = Pad(0, 0x80, 0x80);
        camstick_apply(&u);
        g_vblank++;
        Mh2IpcPad w = Pad(0, 0xFF, 0x80);
        camstick_apply(&w);
        Check(w.rx == 0xFF && w.camTurn == 0, "smooth hold: back at rest, the stick is the game's again");
    }
    /* the freecam has the pad: the numbers stay 0 */
    Cfg("on", "none", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        g_freecam = 1;
        g_vblank = 4;
        Mh2IpcPad p = Pad(0, 0xFF, 0x00), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "smooth: the freecam in control: nothing");
    }
    /* the camera stick off: the row does nothing */
    Cfg("off", "none", "100", "right");
    g_cfg.camera_smooth = 1;
    {
        Mh2IpcPad p = Pad(B_CROSS | B_L3, 0xFF, 0x00), q = p;
        camstick_apply(&p);
        Check(!memcmp(&p, &q, sizeof(p)), "smooth with the camera stick off: nothing");
    }
    /* the row off is the old mapping to the bit: a sweep of sticks, buttons and VBlanks over every setting, hashed; the value
     * is what the mapping gave before the smooth camera was added (made with that file); with the row on the sweep differs */
    {
        uint64_t off = SweepHash(0), on = SweepHash(1);
        Check(off == SWEEP_OFF, "the row off: the sweep's hash is the old mapping's");
        Check(on != SWEEP_OFF, "the row on: the sweep's hash differs (the control)");
        printf("camstick sweep: off %016llx on %016llx\n", (unsigned long long)off, (unsigned long long)on);
    }
    if (bad) {
        printf("camstick: %d failed\n", bad);
        return 1;
    }
    printf("camstick: all checks passed\n");
    return 0;
}
