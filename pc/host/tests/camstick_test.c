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
    if (bad) {
        printf("camstick: %d failed\n", bad);
        return 1;
    }
    printf("camstick: all checks passed\n");
    return 0;
}
