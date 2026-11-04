/*
 * The window and the presentation: the GS thread submits each scanned-out picture,
 * and the main thread draws the latest one with SDL's renderer, at 4:3 and
 * letterboxed into the window (at 16:9 while widescreen is on and no movie plays). F11 or Alt+Enter: borderless fullscreen,
 * with VSync on there (a window's compositor shows whole pictures, so VSync
 * stays off in a window), unless --vsync or the setting vsync says on or off.
 * The core's VBlanks pace the game, never the display.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mh2host.h"

uint32_t g_present_event;

static struct {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Mutex *mu;
    unsigned char *pixels;   /* the latest picture, RGBA8, width * 4 a row */
    uint32_t width, height;
    size_t cap;
    int fresh;
    int shown;
    int movie;   /* the latest picture is a movie one */
} pr;

static void apply_vsync(bool fullscreen)
{
    int on = g_cfg.vsync < 0 ? fullscreen : g_cfg.vsync;
    if (pr.renderer)
        SDL_SetRenderVSync(pr.renderer, on ? 1 : 0);
}

static void set_fullscreen(bool on)
{
    if (!pr.window)
        return;
    SDL_SetWindowFullscreen(pr.window, on);
    apply_vsync(on);
    host_log("%s", on ? "fullscreen" : "windowed");
    if (on)
        SDL_HideCursor();
    else
        SDL_ShowCursor();
}

/* Before a message box: a box over a fullscreen window may not be seen. */
void present_leave_fullscreen(void)
{
    if (pr.window && (SDL_GetWindowFlags(pr.window) & SDL_WINDOW_FULLSCREEN))
        set_fullscreen(false);
}

int present_init(void)
{
    g_present_event = SDL_RegisterEvents(1);
    pr.mu = SDL_CreateMutex();
    pr.window = SDL_CreateWindow("Monster Hunter 2 (mh2-decomp)", 960, 720, SDL_WINDOW_RESIZABLE);
    if (!pr.window) {
        host_log("SDL_CreateWindow: %s", SDL_GetError());
        return -1;
    }
    pr.renderer = SDL_CreateRenderer(pr.window, NULL);
    if (!pr.renderer) {
        host_log("SDL_CreateRenderer: %s", SDL_GetError());
        return -1;
    }
    host_log("renderer: %s", SDL_GetRendererName(pr.renderer));
    apply_vsync(false);
    SDL_SetRenderDrawColor(pr.renderer, 0, 0, 0, 255);
    SDL_RenderClear(pr.renderer);
    SDL_RenderPresent(pr.renderer);
    if (g_cfg.fullscreen)
        set_fullscreen(true);
    return 0;
}


void present_toggle_fullscreen(void)
{
    if (pr.window)
        set_fullscreen(!(SDL_GetWindowFlags(pr.window) & SDL_WINDOW_FULLSCREEN));
}

void present_submit(const void *pixels, uint32_t width, uint32_t height, uint32_t stride, uint64_t vblank, int movie)
{
    (void)vblank;
    if (!pr.mu || !width || !height)
        return;
    SDL_LockMutex(pr.mu);
    size_t need = (size_t)width * height * 4;
    if (need > pr.cap) {
        unsigned char *p = realloc(pr.pixels, need);
        if (!p) {
            SDL_UnlockMutex(pr.mu);
            return;
        }
        pr.pixels = p;
        pr.cap = need;
    }
    for (uint32_t y = 0; y < height; y++)
        memcpy(pr.pixels + (size_t)y * width * 4, (const unsigned char *)pixels + (size_t)y * stride, (size_t)width * 4);
    pr.width = width;
    pr.height = height;
    pr.movie = movie;
    pr.fresh = 1;
    SDL_UnlockMutex(pr.mu);
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = g_present_event;
    SDL_PushEvent(&e);
}

void present_draw(void)
{
    if (!pr.renderer)
        return;
    SDL_LockMutex(pr.mu);
    if (!pr.fresh) {
        SDL_UnlockMutex(pr.mu);
        return;
    }
    pr.fresh = 0;
    if (pr.texture && (pr.texture->w != (int)pr.width || pr.texture->h != (int)pr.height)) {
        SDL_DestroyTexture(pr.texture);
        pr.texture = NULL;
    }
    if (!pr.texture) {
        pr.texture = SDL_CreateTexture(pr.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STREAMING, (int)pr.width,
                                       (int)pr.height);
        if (pr.texture) {
            SDL_SetTextureBlendMode(pr.texture, SDL_BLENDMODE_NONE);
            SDL_SetTextureScaleMode(pr.texture, SDL_SCALEMODE_LINEAR);
        }
    }
    if (pr.texture)
        SDL_UpdateTexture(pr.texture, NULL, pr.pixels, (int)pr.width * 4);
    uint32_t w = pr.width, h = pr.height;
    int movie = pr.movie;
    SDL_UnlockMutex(pr.mu);
    if (!pr.texture)
        return;
    SDL_SetRenderDrawColor(pr.renderer, 0, 0, 0, 255);
    SDL_RenderClear(pr.renderer);
    /* the largest 4:3 area of the window, centred (a TV's shape); 16:9 while the widescreen setting is on and no
     * movie is shown (the movies keep their 4:3, pillarboxed in the wide window) */
    int ow = 0, oh = 0;
    SDL_GetCurrentRenderOutputSize(pr.renderer, &ow, &oh);
    SDL_FRect dst = {0, 0, (float)ow, (float)oh};
    int aw = g_cfg.widescreen && !movie ? 16 : 4, ah = g_cfg.widescreen && !movie ? 9 : 3;
    if (ow * ah > oh * aw) {
        dst.w = (float)oh * (float)aw / (float)ah;
        dst.x = ((float)ow - dst.w) / 2.0f;
    } else {
        dst.h = (float)ow * (float)ah / (float)aw;
        dst.y = ((float)oh - dst.h) / 2.0f;
    }
    SDL_RenderTexture(pr.renderer, pr.texture, NULL, &dst);
    SDL_RenderPresent(pr.renderer);
    if (!pr.shown++)
        host_log("first picture shown (%ux%u)", w, h);
}

void present_shutdown(void)
{
    if (pr.texture)
        SDL_DestroyTexture(pr.texture);
    if (pr.renderer)
        SDL_DestroyRenderer(pr.renderer);
    if (pr.window)
        SDL_DestroyWindow(pr.window);
    if (pr.mu)
        SDL_DestroyMutex(pr.mu);
    free(pr.pixels);
    memset(&pr, 0, sizeof(pr));
}
