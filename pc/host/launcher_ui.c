/*
 * The launcher's UI base (launcher_ui.h; the launcher design, 2, 3 and 9).
 *
 * The layout, the colours and the behaviour are numbers read from the game's Option screens (the rows from (60, 70), 30 apart,
 * the selected row 1.35 times larger in azure, a value centred in a column from x = 290, 320 wide, between two green arrows,
 * every text with a drop shadow, the help at (50, 368)); nothing of the game's art is used: the background is flat, the
 * arrows are two triangles, the font is Noto Sans through stb_truetype.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "launcher_font.h"
#include "launcher_ui.h"
#include "mh2host.h"
#include "third_party/stb/stb_truetype.h"

/* ---------------------------------------------------------------- the font */

#define NFACE 6
typedef struct Face {
    stbtt_fontinfo info;
    unsigned char *data;   /* a system font's file (freed at the end); the embedded font's is not ours */
    int ok, tried;
} Face;

#define ATLAS 1024
#define NGLYPH 8192
typedef struct Glyph {
    uint32_t cp;
    int px;
    short x, y, w, h, xoff, yoff;
    float adv;
    char used;
} Glyph;

static struct {
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Gamepad *pad;
    SDL_Texture *atlas;
    SDL_Surface *surf;
    int atlas_dirty, ax, ay, arow, nglyph;
    float atlas_scale;
    Face face[NFACE];
    int nface;
    unsigned char sys_tried[8];
    Glyph *gl;
    int w, h;                 /* the output's size in pixels */
    float scale, ox, oy;      /* logical to physical */
    float density;
    int fullscreen;
    int key_confirm, key_cancel, pad_confirm, pad_cancel;
    int text_mode;
    float row_top;
    int bounce_l, bounce_r;
    /* the last page's rows, for the mouse */
    int nrows;
    float row_y[24];
    unsigned row_flags[24];
    float mouse_x, mouse_y;
    int mouse_in;
    char err[256];
} ui;

const char *ui_error(void)
{
    return ui.err;
}

/* System fonts for glyphs the embedded one lacks (a path with Japanese letters): the first that has the glyph. */
static const char *const k_system_fonts[] = {
#ifdef _WIN32
    "C:\\Windows\\Fonts\\YuGothM.ttc", "C:\\Windows\\Fonts\\msgothic.ttc", "C:\\Windows\\Fonts\\meiryo.ttc",
    "C:\\Windows\\Fonts\\arialuni.ttf", "C:\\Windows\\Fonts\\seguisym.ttf",
#else
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
#endif
};

static int face_init(Face *f, const unsigned char *data)
{
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    return off >= 0 && stbtt_InitFont(&f->info, data, off);
}

/* The face that has a glyph for CP (the embedded Noto first); -1 if none. */
static int face_for(uint32_t cp, int *gi)
{
    for (int i = 0; i < ui.nface || i == 0; i++) {
        if (ui.face[i].ok && (*gi = stbtt_FindGlyphIndex(&ui.face[i].info, (int)cp)) != 0)
            return i;
    }
    /* not yet looked for: the system's fonts, each opened once */
    for (size_t k = 0; k < sizeof(k_system_fonts) / sizeof(k_system_fonts[0]) && ui.nface < NFACE; k++) {
        int slot = ui.nface;
        Face *f = &ui.face[slot];
        if (k >= sizeof(ui.sys_tried) || ui.sys_tried[k])
            continue;
        ui.sys_tried[k] = 1;
        size_t len = 0;
        void *data = SDL_LoadFile(k_system_fonts[k], &len);
        if (!data)
            continue;
        f->data = data;
        if (!face_init(f, f->data)) {
            SDL_free(f->data);
            f->data = NULL;
            continue;
        }
        f->ok = 1;
        ui.nface++;
        if ((*gi = stbtt_FindGlyphIndex(&f->info, (int)cp)) != 0)
            return slot;
    }
    return -1;
}

static void atlas_clear(void)
{
    if (ui.gl)
        memset(ui.gl, 0, sizeof(Glyph) * NGLYPH);
    if (ui.surf)
        SDL_ClearSurface(ui.surf, 0, 0, 0, 0);
    ui.ax = ui.ay = ui.arow = ui.nglyph = 0;
    ui.atlas_dirty = 1;
}

/* The glyph of CP at PX pixels high, rasterised on first use into the atlas. */
static Glyph *glyph(uint32_t cp, int px)
{
    uint32_t h = (cp * 2654435761u ^ (uint32_t)px * 40503u) & (NGLYPH - 1);
    for (int tries = 0; tries < NGLYPH; tries++, h = (h + 1) & (NGLYPH - 1)) {
        Glyph *g = &ui.gl[h];
        if (g->used && g->cp == cp && g->px == px)
            return g;
        if (!g->used)
            break;
    }
    if (ui.nglyph > NGLYPH * 3 / 4)
        atlas_clear();
    int gi = 0;
    uint32_t want = cp;
    int fi = face_for(cp, &gi);
    if (fi < 0) {
        fi = face_for('?', &gi);
        if (fi < 0)
            return NULL;
    }
    Face *f = &ui.face[fi];
    float sc = stbtt_ScaleForPixelHeight(&f->info, (float)px);
    int w = 0, hh = 0, xo = 0, yo = 0, adv = 0, lsb = 0;
    unsigned char *bm = stbtt_GetGlyphBitmap(&f->info, sc, sc, gi, &w, &hh, &xo, &yo);
    stbtt_GetGlyphHMetrics(&f->info, gi, &adv, &lsb);
    if (w > 0 && hh > 0) {
        if (ui.ax + w + 1 > ATLAS) {
            ui.ax = 0;
            ui.ay += ui.arow + 1;
            ui.arow = 0;
        }
        if (ui.ay + hh + 1 > ATLAS || w > ATLAS) {
            atlas_clear();   /* the cache is full: start again (the glyphs asked for next are rasterised again) */
        }
        if (ui.ax + w + 1 > ATLAS || w > ATLAS || hh > ATLAS) {
            stbtt_FreeBitmap(bm, NULL);
            return NULL;
        }
    }
    uint32_t key = h;
    Glyph *g = &ui.gl[key];
    for (int tries = 0; g->used && tries < NGLYPH; tries++) {   /* the table was cleared above, or has room */
        key = (key + 1) & (NGLYPH - 1);
        g = &ui.gl[key];
    }
    g->used = 1;
    g->cp = want;
    g->px = px;
    g->w = (short)w;
    g->h = (short)hh;
    g->xoff = (short)xo;
    g->yoff = (short)yo;
    g->adv = (float)adv * sc;
    if (w > 0 && hh > 0) {
        g->x = (short)ui.ax;
        g->y = (short)ui.ay;
        Uint32 *dst = ui.surf->pixels;
        int pitch = ui.surf->pitch / 4;
        for (int yy = 0; yy < hh; yy++)
            for (int xx = 0; xx < w; xx++) {
                unsigned char c = bm[yy * w + xx];
                unsigned char *p = (unsigned char *)&dst[(ui.ay + yy) * pitch + ui.ax + xx];
                p[0] = p[1] = p[2] = 255;
                p[3] = c;
            }
        ui.ax += w + 1;
        if (hh > ui.arow)
            ui.arow = hh;
        ui.atlas_dirty = 1;
    }
    stbtt_FreeBitmap(bm, NULL);
    ui.nglyph++;
    return g;
}

static uint32_t next_cp(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = p[0];
    int n = 1;
    if (c >= 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
        c = ((c & 7) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
        n = 4;
    } else if (c >= 0xE0 && c < 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
        c = ((c & 15) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
        n = 3;
    } else if (c >= 0xC0 && c < 0xE0 && (p[1] & 0xC0) == 0x80) {
        c = ((c & 31) << 6) | (p[1] & 0x3Fu);
        n = 2;
    } else if (c >= 0x80) {
        c = '?';
    }
    *s += n;
    return c;
}

/* The line metrics of the embedded font at PX pixels: ascent and descent (negative), in pixels. */
static void metrics(int px, float *asc, float *desc)
{
    int a, d, g;
    stbtt_GetFontVMetrics(&ui.face[0].info, &a, &d, &g);
    float sc = stbtt_ScaleForPixelHeight(&ui.face[0].info, (float)px);
    *asc = (float)a * sc;
    *desc = (float)d * sc;
}

static int size_px(float size)
{
    int px = (int)(size * ui.scale + 0.5f);
    return px < 6 ? 6 : px;
}

/* The width of S at SIZE (logical units). */
static float text_width(const char *s, float size)
{
    int px = size_px(size);
    float w = 0;
    while (*s) {
        uint32_t cp = next_cp(&s);
        if (cp < 32)
            cp = ' ';
        Glyph *g = glyph(cp, px);
        if (g)
            w += g->adv;
    }
    return w / ui.scale;
}

static void upload(void)
{
    if (ui.atlas_dirty && ui.atlas) {
        SDL_UpdateTexture(ui.atlas, NULL, ui.surf->pixels, ui.surf->pitch);
        ui.atlas_dirty = 0;
    }
}

/* S at (X, CY): CY the middle of the line; ALIGN 0 left, 1 centred, 2 right. */
static void draw_text(float x, float cy, float size, int r, int g, int b, int a, int align, const char *s)
{
    int px = size_px(size);
    float asc, desc;
    metrics(px, &asc, &desc);
    float base = ui.oy + cy * ui.scale + (asc + desc) / 2;
    float wl = text_width(s, size);
    if (align == 1)
        x -= wl / 2;
    else if (align == 2)
        x -= wl;
    float pen = ui.ox + x * ui.scale;
    SDL_SetTextureColorMod(ui.atlas, (Uint8)r, (Uint8)g, (Uint8)b);
    SDL_SetTextureAlphaMod(ui.atlas, (Uint8)a);
    while (*s) {
        uint32_t cp = next_cp(&s);
        if (cp < 32)
            cp = ' ';
        Glyph *gl = glyph(cp, px);
        if (!gl)
            continue;
        upload();
        if (gl->w > 0) {
            SDL_FRect src = {gl->x, gl->y, gl->w, gl->h};
            SDL_FRect dst = {(float)(int)(pen + 0.5f) + gl->xoff, (float)(int)(base + 0.5f) + gl->yoff, gl->w, gl->h};
            SDL_RenderTexture(ui.ren, ui.atlas, &src, &dst);
        }
        pen += gl->adv;
    }
}

/* With the drop shadow of the game's text: the same in half-black, 2 px away (3 above size 22). */
static void text(float x, float cy, float size, int r, int g, int b, int align, const char *s)
{
    float d = size > 22 ? 3 : 2;
    draw_text(x + d, cy + d, size, 0, 0, 0, 128, align, s);
    draw_text(x, cy, size, r, g, b, 255, align, s);
}

/* ---------------------------------------------------------------- shapes */

static void fill(float x, float y, float w, float h, int r, int g, int b, int a)
{
    SDL_FRect rc = {ui.ox + x * ui.scale, ui.oy + y * ui.scale, w * ui.scale, h * ui.scale};
    SDL_SetRenderDrawBlendMode(ui.ren, a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ui.ren, (Uint8)r, (Uint8)g, (Uint8)b, (Uint8)a);
    SDL_RenderFillRect(ui.ren, &rc);
}

static void outline(float x, float y, float w, float h, int r, int g, int b)
{
    float t = 1;
    fill(x, y, w, t, r, g, b, 255);
    fill(x, y + h - t, w, t, r, g, b, 255);
    fill(x, y, t, h, r, g, b, 255);
    fill(x + w - t, y, t, h, r, g, b, 255);
}

static void triangle(float x1, float y1, float x2, float y2, float x3, float y3, int r, int g, int b)
{
    SDL_FColor c = {r / 255.0f, g / 255.0f, b / 255.0f, 1};
    SDL_Vertex v[3] = {
        {{ui.ox + x1 * ui.scale, ui.oy + y1 * ui.scale}, c, {0, 0}},
        {{ui.ox + x2 * ui.scale, ui.oy + y2 * ui.scale}, c, {0, 0}},
        {{ui.ox + x3 * ui.scale, ui.oy + y3 * ui.scale}, c, {0, 0}},
    };
    SDL_SetRenderDrawBlendMode(ui.ren, SDL_BLENDMODE_NONE);
    SDL_RenderGeometry(ui.ren, NULL, v, 3, NULL, 0);
}

/* ---------------------------------------------------------------- the window */

static void geometry(void)
{
    SDL_GetRenderOutputSize(ui.ren, &ui.w, &ui.h);
    float sx = (float)ui.w / UI_W, sy = (float)ui.h / UI_H;
    ui.scale = sx < sy ? sx : sy;
    ui.ox = (float)(int)(((float)ui.w - UI_W * ui.scale) / 2);
    ui.oy = (float)(int)(((float)ui.h - UI_H * ui.scale) / 2);
    ui.density = SDL_GetWindowPixelDensity(ui.win);
    if (ui.density <= 0)
        ui.density = 1;
    float d = ui.atlas_scale - ui.scale;
    if (d > 0.01f * ui.scale || d < -0.01f * ui.scale) {   /* the text is baked at the size it is drawn */
        atlas_clear();
        ui.atlas_scale = ui.scale;
    }
}

int ui_open(int fullscreen)
{
    memset(&ui, 0, sizeof(ui));
    ui.err[0] = 0;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        snprintf(ui.err, sizeof(ui.err), "%s", SDL_GetError());
        return -1;
    }
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);   /* a pad is optional */
    SDL_WindowFlags wf = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen)
        wf |= SDL_WINDOW_FULLSCREEN;
    ui.win = SDL_CreateWindow(MH2_WINDOW_TITLE, 960, 672, wf);
    if (!ui.win) {
        snprintf(ui.err, sizeof(ui.err), "%s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -1;
    }
    SDL_SetWindowMinimumSize(ui.win, UI_W, UI_H);
    float mx, my;
    SDL_GetGlobalMouseState(&mx, &my);   /* centred on the display the mouse is on */
    SDL_Point pt = {(int)mx, (int)my};
    SDL_DisplayID d = SDL_GetDisplayForPoint(&pt);
    if (d && !fullscreen)
        SDL_SetWindowPosition(ui.win, SDL_WINDOWPOS_CENTERED_DISPLAY(d), SDL_WINDOWPOS_CENTERED_DISPLAY(d));
    const char *soft = hfs_getenv("MH2_LAUNCHER_SOFTWARE");
    ui.ren = SDL_CreateRenderer(ui.win, soft && *soft ? "software" : NULL);
    if (!ui.ren)
        ui.ren = SDL_CreateRenderer(ui.win, "software");
    if (!ui.ren) {
        snprintf(ui.err, sizeof(ui.err), "%s", SDL_GetError());
        SDL_DestroyWindow(ui.win);
        ui.win = NULL;
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return -1;
    }
    SDL_SetRenderVSync(ui.ren, 1);
    ui.surf = SDL_CreateSurface(ATLAS, ATLAS, SDL_PIXELFORMAT_RGBA32);
    ui.atlas = SDL_CreateTexture(ui.ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, ATLAS, ATLAS);
    ui.gl = calloc(NGLYPH, sizeof(Glyph));
    if (!ui.surf || !ui.atlas || !ui.gl || !face_init(&ui.face[0], g_font_noto)) {
        snprintf(ui.err, sizeof(ui.err), "%s", ui.surf && ui.atlas ? "the font could not be read" : SDL_GetError());
        ui_close();
        return -1;
    }
    ui.face[0].ok = ui.face[0].tried = 1;
    ui.nface = 1;
    SDL_SetTextureBlendMode(ui.atlas, SDL_BLENDMODE_BLEND);
    atlas_clear();
    ui.fullscreen = fullscreen;
    /* the player's own buttons: Circle confirms, Cross cancels (the keys and the pad's, as input.c has them) */
    int k = 0, p = 0;
    ui.key_confirm = SDL_SCANCODE_L;
    ui.key_cancel = SDL_SCANCODE_K;
    ui.pad_confirm = SDL_GAMEPAD_BUTTON_EAST;
    ui.pad_cancel = SDL_GAMEPAD_BUTTON_SOUTH;
    if (input_button("circle", &k, &p) == 0) {
        ui.key_confirm = k;
        ui.pad_confirm = p;
    }
    if (input_button("cross", &k, &p) == 0) {
        ui.key_cancel = k;
        ui.pad_cancel = p;
    }
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    if (ids && n > 0)
        ui.pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    geometry();
    ui.atlas_scale = ui.scale;
    return 0;
}

void ui_close(void)
{
    if (ui.pad)
        SDL_CloseGamepad(ui.pad);
    for (int i = 1; i < NFACE; i++)
        if (ui.face[i].data)
            SDL_free(ui.face[i].data);
    free(ui.gl);
    if (ui.atlas)
        SDL_DestroyTexture(ui.atlas);
    if (ui.surf)
        SDL_DestroySurface(ui.surf);
    if (ui.ren)
        SDL_DestroyRenderer(ui.ren);
    if (ui.win)
        SDL_DestroyWindow(ui.win);
    memset(&ui, 0, sizeof(ui));
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

int ui_fullscreen(void)
{
    return ui.fullscreen;
}

const char *ui_renderer(void)
{
    return ui.ren ? SDL_GetRendererName(ui.ren) : "none";
}

void ui_text_input(int on)
{
    ui.text_mode = on;
    if (!ui.win)
        return;
    if (on)
        SDL_StartTextInput(ui.win);
    else
        SDL_StopTextInput(ui.win);
}

void *ui_window(void)
{
    return ui.win;
}

static void toggle_fullscreen(void)
{
    ui.fullscreen = !ui.fullscreen;
    SDL_SetWindowFullscreen(ui.win, ui.fullscreen != 0);
}

void ui_window_point(float x, float y, float *wx, float *wy)
{
    *wx = (ui.ox + x * ui.scale) / ui.density;
    *wy = (ui.oy + y * ui.scale) / ui.density;
}

void ui_set_size(int w, int h)
{
    SDL_SetWindowSize(ui.win, w, h);
}

void ui_bounce(int dir)
{
    if (dir < 0)
        ui.bounce_l = 6;
    else
        ui.bounce_r = 6;
}

int ui_rows_visible(void)
{
    return ui.nrows;
}

/* ---------------------------------------------------------------- input */

/* The row under a window point, or -1; PART: -1 the left arrow, 1 the right arrow, 0 elsewhere. */
static int row_at(float wx, float wy, int *part)
{
    float x = (wx * ui.density - ui.ox) / ui.scale, y = (wy * ui.density - ui.oy) / ui.scale;
    *part = 0;
    for (int i = 0; i < ui.nrows; i++)
        if (y >= ui.row_y[i] && y < ui.row_y[i] + 30 && x >= 40 && x < 620) {
            if (ui.row_flags[i] & UR_ARROWS) {
                if (x >= 280 && x < 330)
                    *part = -1;
                else if (x >= 570 && x < 620)
                    *part = 1;
            }
            return i;
        }
    return -1;
}

static int key_event(const SDL_KeyboardEvent *k, UiEvent *out)
{
    SDL_Scancode sc = k->scancode;
    if (sc == SDL_SCANCODE_F11 || (sc == SDL_SCANCODE_RETURN && (k->mod & SDL_KMOD_ALT))) {
        if (!k->repeat)
            toggle_fullscreen();
        return 0;
    }
    int t = UE_NONE;
    switch (sc) {
    case SDL_SCANCODE_UP: t = UE_UP; break;
    case SDL_SCANCODE_DOWN: t = UE_DOWN; break;
    case SDL_SCANCODE_LEFT: t = UE_LEFT; break;
    case SDL_SCANCODE_RIGHT: t = UE_RIGHT; break;
    case SDL_SCANCODE_PAGEUP: t = UE_PGUP; break;
    case SDL_SCANCODE_PAGEDOWN: t = UE_PGDN; break;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: t = k->repeat ? UE_NONE : UE_CONFIRM; break;
    case SDL_SCANCODE_ESCAPE: t = k->repeat ? UE_NONE : UE_CANCEL; break;
    case SDL_SCANCODE_BACKSPACE: t = ui.text_mode ? UE_BACKSPACE : UE_NONE; break;
    default:
        if (ui.text_mode)
            break;
        if ((int)sc == ui.key_confirm)
            t = k->repeat ? UE_NONE : UE_CONFIRM;
        else if ((int)sc == ui.key_cancel)
            t = k->repeat ? UE_NONE : UE_CANCEL;
    }
    out->type = t;
    return t != UE_NONE;
}

int ui_poll(UiEvent *out, int wait_ms)
{
    SDL_Event e;
    int first = 1;
    for (;;) {
        int got = first && wait_ms > 0 ? SDL_WaitEventTimeout(&e, wait_ms) : SDL_PollEvent(&e);
        first = 0;
        if (!got)
            return 0;
        out->row = out->dir = 0;
        switch (e.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            out->type = UE_QUIT;
            return 1;
        case SDL_EVENT_KEY_DOWN:
            if (key_event(&e.key, out))
                return 1;
            break;
        case SDL_EVENT_TEXT_INPUT:
            if (ui.text_mode && e.text.text && e.text.text[0]) {
                out->type = UE_TEXT;
                snprintf(out->text, sizeof(out->text), "%s", e.text.text);
                return 1;
            }
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!ui.pad)
                ui.pad = SDL_OpenGamepad(e.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (ui.pad && SDL_GetGamepadID(ui.pad) == e.gdevice.which) {
                SDL_CloseGamepad(ui.pad);
                ui.pad = NULL;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            int b = e.gbutton.button, t = UE_NONE;
            if (b == SDL_GAMEPAD_BUTTON_DPAD_UP)
                t = UE_UP;
            else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN)
                t = UE_DOWN;
            else if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT)
                t = UE_LEFT;
            else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT)
                t = UE_RIGHT;
            else if (b == ui.pad_confirm || b == SDL_GAMEPAD_BUTTON_START)
                t = UE_CONFIRM;
            else if (b == ui.pad_cancel || b == SDL_GAMEPAD_BUTTON_BACK)
                t = UE_CANCEL;
            if (t != UE_NONE) {
                out->type = t;
                return 1;
            }
            break;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            int part;
            ui.mouse_x = e.motion.x;
            ui.mouse_y = e.motion.y;
            ui.mouse_in = 1;
            int r = row_at(e.motion.x, e.motion.y, &part);
            if (r >= 0) {
                out->type = UE_HOVER;
                out->row = r;
                return 1;
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int part;
            if (e.button.button == SDL_BUTTON_RIGHT) {
                out->type = UE_CANCEL;
                return 1;
            }
            if (e.button.button != SDL_BUTTON_LEFT)
                break;
            ui.mouse_x = e.button.x;
            ui.mouse_y = e.button.y;
            int r = row_at(e.button.x, e.button.y, &part);
            if (r < 0)
                break;
            out->row = r;
            out->type = part ? UE_ARROW : UE_CLICK;
            out->dir = part;
            return 1;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            int part;
            int r = ui.mouse_in ? row_at(ui.mouse_x, ui.mouse_y, &part) : -1;
            out->type = UE_WHEEL;
            out->row = r;
            out->dir = e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0;
            if (out->dir)
                return 1;
            break;
        }
        default:
            break;
        }
    }
}

/* ---------------------------------------------------------------- pages */

void ui_begin(void)
{
    geometry();
    SDL_SetRenderDrawBlendMode(ui.ren, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(ui.ren, 0, 0, 0, 255);
    SDL_RenderClear(ui.ren);
    fill(0, 0, UI_W, UI_H, 12, 15, 24, 255);
    outline(8, 8, UI_W - 16, UI_H - 16, 52, 66, 96);
    ui.nrows = 0;
    ui.row_top = 0;
}

void ui_header_sub(const char *title, const char *sub)
{
    ui_header(title);
    text(50 + text_width(title, 28) + 16, 40, 15, 150, 165, 200, 0, sub);
}

void ui_header(const char *title)
{
    text(50, 38, 28, 235, 240, 255, 0, title);
    fill(50, 58, 540, 1, 52, 66, 96, 255);
}

/* Cut S to the width MAXW at SIZE: the end of a path is the part that matters, so the start gives way. */
static const char *fit(const char *s, float size, float maxw, char *buf, size_t cap)
{
    if (text_width(s, size) <= maxw)
        return s;
    size_t len = strlen(s);
    for (size_t cut = 1; cut < len; cut++) {
        const char *p = s + cut;
        while ((*p & 0xC0) == 0x80)   /* not in the middle of a UTF-8 letter */
            p++;
        snprintf(buf, cap, "...%s", p);
        if (text_width(buf, size) <= maxw)
            return buf;
    }
    return s;
}

void ui_rows_top(float y)
{
    ui.row_top = y;
}

void ui_rows(const UiRow *rows, int n, int sel)
{
    float y = ui.row_top > 0 ? ui.row_top : 70;
    if (n > 24)
        n = 24;
    ui.nrows = n;
    for (int i = 0; i < n; i++) {
        const UiRow *r = &rows[i];
        if (r->flags & UR_GAP)
            y += 15;
        int selected = i == sel;
        float size = selected ? 18 * 1.35f : 18;
        int cr = 255, cg = 255, cb = 255;
        if (r->flags & UR_WARN) {
            cr = 255; cg = 176; cb = 48;
            if (selected) { cg = 200; cb = 90; }
        } else if (r->flags & UR_DIM) {
            cr = cg = cb = selected ? 150 : 128;
        } else if (r->flags & UR_GREEN) {
            cr = 86; cg = 255; cb = 86;
            if (selected) { cr = 140; cg = 255; cb = 140; }
        } else if (selected) {
            cr = 0; cg = 130; cb = 240;
        }
        ui.row_y[i] = y;
        ui.row_flags[i] = r->flags;
        float cy = y + 15;
        text(60, cy, size, cr, cg, cb, 0, r->label);
        if (r->value && (r->flags & UR_EDIT)) {   /* typed: from the left of the value's area, a caret after it */
            char buf[512], shown[560];
            snprintf(shown, sizeof(shown), "%s_", r->value);
            const char *v = fit(shown, size, 260, buf, sizeof(buf));
            text(330, cy, size, 255, 255, 255, 0, v);
        } else if (r->value) {
            char buf[512];
            const char *v = fit(r->value, size, 230, buf, sizeof(buf));
            text(450, cy, size, cr, cg, cb, 1, v);
        }
        if (r->flags & UR_ARROWS) {
            float bl = selected && ui.bounce_l ? (float)ui.bounce_l / 2 : 0, br = selected && ui.bounce_r ? (float)ui.bounce_r / 2 : 0;
            if (!(r->flags & UR_NOLEFT))
                triangle(304 - bl, cy - 9, 304 - bl, cy + 9, 290 - bl, cy, 100, 188, 50);
            if (!(r->flags & UR_NORIGHT))
                triangle(596 + br, cy - 9, 596 + br, cy + 9, 610 + br, cy, 100, 188, 50);
        }
        y += 30;
    }
    if (ui.bounce_l)
        ui.bounce_l--;
    if (ui.bounce_r)
        ui.bounce_r--;
}

/* Words of S broken into lines no wider than MAXW; returns the lines in OUT (pointers into BUF). */
static int wrap(const char *s, float size, float maxw, char *buf, size_t cap, const char **out, int maxlines)
{
    int n = 0;
    size_t used = 0;
    const char *p = s;
    while (*p && n < maxlines) {
        while (*p == ' ')
            p++;
        const char *line = p;
        const char *best = p;   /* the end of the longest fitting run of words */
        const char *q = p;
        for (;;) {
            const char *e = q;
            while (*e && *e != ' ')
                e++;
            char tmp[512];
            size_t len = (size_t)(e - line);
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, line, len);
            tmp[len] = 0;
            if (text_width(tmp, size) > maxw && best != line)
                break;
            best = e;
            if (!*e)
                break;
            q = e + 1;
        }
        size_t len = (size_t)(best - line);
        if (used + len + 1 > cap)
            break;
        memcpy(buf + used, line, len);
        buf[used + len] = 0;
        out[n++] = buf + used;
        used += len + 1;
        p = best;
    }
    return n;
}

void ui_help(const char *help)
{
    if (!help || !*help)
        return;
    char buf[1024];
    const char *lines[4];
    int n = wrap(help, 16, 540, buf, sizeof(buf), lines, 3);
    for (int i = 0; i < n; i++)
        text(50, 376 + (float)i * 21, 16, 205, 210, 225, 0, lines[i]);
}

void ui_para(float y, const char *s)
{
    char buf[1024];
    const char *lines[5];
    int n = wrap(s, 16, 540, buf, sizeof(buf), lines, 4);
    for (int i = 0; i < n; i++)
        text(50, y + (float)i * 21, 16, 205, 210, 225, 0, lines[i]);
}

void ui_line(float y, int shade, const char *s)
{
    char buf[600];
    const char *v = fit(s, 18, 520, buf, sizeof(buf));
    if (shade == 2)
        text(60, y, 18, 255, 176, 48, 0, v);
    else if (shade == 1)
        text(60, y, 16, 160, 168, 190, 0, v);
    else
        text(60, y, 18, 255, 255, 255, 0, v);
}

void ui_bar(float y, float frac, const char *label)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    fill(60, y - 9, 400, 18, 28, 34, 54, 255);
    fill(60, y - 9, 400 * frac, 18, 100, 188, 50, 255);
    outline(60, y - 9, 400, 18, 120, 140, 180);
    text(480, y, 18, 255, 255, 255, 0, label);
}

int ui_lines(const char *const *lines, int n, int first)
{
    int fit_n = UI_LOG_LINES;
    for (int i = 0; i < fit_n && first + i < n; i++) {
        char buf[512];
        const char *l = lines[first + i];
        /* a long line is cut at the right with "..." */
        if (text_width(l, 13) > 540) {
            size_t len = strlen(l);
            while (len > 1) {
                len--;
                while (len > 1 && (l[len] & 0xC0) == 0x80)
                    len--;
                snprintf(buf, sizeof(buf), "%.*s...", (int)len, l);
                if (text_width(buf, 13) <= 540)
                    break;
            }
            l = buf;
        }
        draw_text(50, 74 + (float)i * 17, 13, 205, 210, 225, 255, 0, l);
    }
    return fit_n;
}

void ui_message(const char *msg, const char *const *buttons, int nb, int sel)
{
    fill(0, 0, UI_W, UI_H, 0, 0, 0, 150);
    char buf[1400];
    const char *lines[9];
    int n = wrap(msg, 18, 400, buf, sizeof(buf), lines, 8);
    float h = 46 + (float)n * 26 + 52;
    float y0 = (UI_H - h) / 2;
    fill(90, y0, 460, h, 20, 26, 42, 255);
    outline(90, y0, 460, h, 120, 140, 180);
    for (int i = 0; i < n; i++)
        text(320, y0 + 36 + (float)i * 26, 18, 255, 255, 255, 1, lines[i]);
    float bx = 320 - (float)(nb - 1) * 70;
    for (int i = 0; i < nb; i++) {
        int s = i == sel;
        if (s)
            text(bx + (float)i * 140, y0 + h - 30, 18 * 1.35f, 0, 130, 240, 1, buttons[i]);
        else
            text(bx + (float)i * 140, y0 + h - 30, 18, 255, 255, 255, 1, buttons[i]);
    }
}

void ui_end(void)
{
    SDL_RenderPresent(ui.ren);
}

int ui_readback(uint64_t *hash, const char *png_path)
{
    SDL_Surface *s = SDL_RenderReadPixels(ui.ren, NULL);
    if (!s)
        return -1;
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!c)
        return -1;
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < c->h; y++) {
        const unsigned char *row = (const unsigned char *)c->pixels + (size_t)y * (size_t)c->pitch;
        for (int x = 0; x < c->w * 4; x++)
            h = (h ^ row[x]) * 1099511628211ull;
    }
    *hash = h;
    if (png_path)
        SDL_SavePNG(c, png_path);
    SDL_DestroySurface(c);
    return 0;
}
