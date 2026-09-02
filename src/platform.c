#include "platform.h"
#include <stdlib.h>
#include <SDL.h>
#include "font8.h"

static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static uint32_t      rgba[VGA_W * VGA_H];
static SDL_mutex    *audio_lock;
static unsigned      keymask;
static int           lastch;

/* CRT effects: F10 toggles.  4x render target for sharp-bilinear scaling
 * ("antialias"), scanline overlay, phosphor-persistence motion blur. */
#define FX_SCALE 4
static int          fx_on = 0;      /* default: original look; F10 for CRT fx */
static SDL_Texture *fx_target, *fx_scan;
static uint8_t      fx_acc[VGA_W * VGA_H][3];   /* phosphor accumulator */
#define FX_PERSIST 214                          /* trail decay, /256 per frame */

void (*plat_f9_hook)(void);
volatile duint Time = 0;
static double tick_origin;

/* PIT divisor 0x19e4 = 6628 -> 1193182/6628 = 180.02 Hz int8?  The game's
 * comments say 36 volatile ticks/sec; miscasm chains 1:5.  Net: Time += 1
 * at ~36.4 Hz.  We derive Time from wall time. */
#define TICK_HZ (1193182.0 / 0x19e4 / 5.0)

double plat_now(void) {
    return (double)SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency();
}

void plat_lock(void)   { if (audio_lock) SDL_LockMutex(audio_lock); }
void plat_unlock(void) { if (audio_lock) SDL_UnlockMutex(audio_lock); }

int plat_init(const char *title, int scale) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) return -1;
    audio_lock = SDL_CreateMutex();
    win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           VGA_W * scale, VGA_H * scale * 6 / 5,   /* 4:3 aspect (200->240) */
                           SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
    if (!win) return -1;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, VGA_W * 4, VGA_H * 4 * 6 / 5);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                            SDL_TEXTUREACCESS_STREAMING, VGA_W, VGA_H);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    fx_target = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                  SDL_TEXTUREACCESS_TARGET,
                                  VGA_W * FX_SCALE, VGA_H * FX_SCALE);
    if (fx_target) {
        SDL_SetTextureScaleMode(fx_target, SDL_ScaleModeLinear);
        /* scanline overlay: FX_SCALE rows per game line, last two darkened */
        fx_scan = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888,
                                    SDL_TEXTUREACCESS_STATIC,
                                    1, VGA_H * FX_SCALE);
        static uint32_t scan[VGA_H * FX_SCALE];
        for (int y = 0; y < VGA_H * FX_SCALE; y++) {
            static const uint8_t a[FX_SCALE] = { 0, 0, 40, 96 };
            scan[y] = (uint32_t)a[y % FX_SCALE] << 24;   /* black, alpha only */
        }
        SDL_UpdateTexture(fx_scan, NULL, scan, 4);
        SDL_SetTextureBlendMode(fx_scan, SDL_BLENDMODE_BLEND);
    } else fx_on = 0;                       /* no render-target support */
    if (fx_target && SDL_getenv("SKYROADS_FX")) fx_on = 1;
    tick_origin = plat_now();
    return 0;
}

void plat_exit(int code) { exit(code); }

void plat_quit(void) {
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

void plat_tick_update(void) {
    Time = (duint)((plat_now() - tick_origin) * TICK_HZ);
}


static char   osd_msg[32];
static double osd_until;

void plat_osd(const char *msg) {
    SDL_strlcpy(osd_msg, msg, sizeof osd_msg);
    osd_until = plat_now() + 1.5;
}

static void osd_draw(void) {
    if (plat_now() >= osd_until) return;
    int tx = 8, ty = 8;
    for (const char *p = osd_msg; *p; p++, tx += 8) {
        const uint8_t *g = font8_glyph(*p);
        if (!g) continue;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c)) {
                    rgba[(ty + r + 1) * VGA_W + tx + c + 1] = 0xff000000u;
                    rgba[(ty + r) * VGA_W + tx + c] = 0xff40e0ffu;   /* amber */
                }
    }
}

void plat_present(void) {
    const uint8_t *src = vga_mem();
    for (int i = 0; i < VGA_W * VGA_H; i++) {
        const uint8_t *c = g_palette[src[i]];
        /* 6-bit DAC -> 8-bit */
        uint32_t r = (c[0] << 2) | (c[0] >> 4);
        uint32_t g = (c[1] << 2) | (c[1] >> 4);
        uint32_t b = (c[2] << 2) | (c[2] >> 4);
        if (fx_on) {
            /* phosphor persistence: bright pixels decay instead of vanishing */
            uint8_t *a = fx_acc[i];
            uint32_t dr = (uint32_t)a[0] * FX_PERSIST >> 8;
            uint32_t dg = (uint32_t)a[1] * FX_PERSIST >> 8;
            uint32_t db = (uint32_t)a[2] * FX_PERSIST >> 8;
            if (r > dr) dr = r;
            if (g > dg) dg = g;
            if (b > db) db = b;
            a[0] = (uint8_t)dr; a[1] = (uint8_t)dg; a[2] = (uint8_t)db;
            r = dr; g = dg; b = db;
        }
        rgba[i] = 0xff000000u | (b << 16) | (g << 8) | r;
    }
    osd_draw();
    SDL_UpdateTexture(tex, NULL, rgba, VGA_W * 4);
    if (fx_on && fx_target) {
        SDL_SetRenderTarget(ren, fx_target);
        SDL_RenderCopy(ren, tex, NULL, NULL);       /* nearest 4x: crisp pixels */
        SDL_RenderCopy(ren, fx_scan, NULL, NULL);   /* scanlines */
        SDL_SetRenderTarget(ren, NULL);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, fx_target, NULL, NULL); /* linear: antialiased edge */
    } else {
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, NULL, NULL);
    }
    /* SKYROADS_DUMP_FX=<dir>: capture the post-effects backbuffer ~1/s */
    const char *fxdir = SDL_getenv("SKYROADS_DUMP_FX");
    if (fxdir) {
        static int frame, last;
        int now = (int)SDL_GetTicks() / 1000;
        int every = SDL_getenv("SKYROADS_DUMP_FX_ALL") != NULL;
        if ((every && frame < 150) || (!every && now != last)) {
            last = now;
            int w, h;
            SDL_GetRendererOutputSize(ren, &w, &h);
            uint8_t *px = SDL_malloc((size_t)w * h * 4);
            if (px && !SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ABGR8888, px, w * 4)) {
                char path[1100];
                SDL_snprintf(path, sizeof path, "%s/fx_%03d.ppm", fxdir, frame++);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", w, h);
                    for (int i = 0; i < w * h; i++) fwrite(px + i * 4, 1, 3, f);
                    fclose(f);
                }
            }
            SDL_free(px);
        }
    }
    SDL_RenderPresent(ren);
    void plat_debug_dump(const uint8_t *fb, const uint8_t (*pal)[3]);
    plat_debug_dump(src, (const uint8_t (*)[3])g_palette);
}

static void toggle_fullscreen(void) {
    static int fs;
    fs = !fs;
    SDL_SetWindowFullscreen(win, fs ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

static void key_event(SDL_Keycode k, int down) {
    if (down && (k == SDLK_F11 ||
                 (k == SDLK_f && (SDL_GetModState() & KMOD_GUI)))) {
        toggle_fullscreen();
        return;
    }
    if (down && k == SDLK_F10 && fx_target) {   /* CRT effects on/off */
        fx_on = !fx_on;
        plat_osd(fx_on ? "CRT ON" : "CRT OFF");
        return;
    }
    if (down && k == SDLK_F9 && plat_f9_hook) {  /* music mode toggle */
        plat_f9_hook();
        return;
    }
    unsigned bit = 0;
    switch (k) {
    case SDLK_LEFT:  bit = K_LEFT;  break;
    case SDLK_RIGHT: bit = K_RIGHT; break;
    case SDLK_UP:    bit = K_UP;    break;
    case SDLK_DOWN:  bit = K_DOWN;  break;
    case SDLK_SPACE: bit = K_SPACE; break;
    case SDLK_ESCAPE: bit = K_ESC;  break;
    case SDLK_RETURN: bit = K_RET;  break;
    default: break;
    }
    if (down) {
        keymask |= bit;
        if (k >= 32 && k < 127) lastch = (int)k;
        else if (k == SDLK_ESCAPE) lastch = 27;
        else if (k == SDLK_RETURN) lastch = 13;
        else if (k == SDLK_UP)    lastch = 0x148;
        else if (k == SDLK_DOWN)  lastch = 0x150;
        else if (k == SDLK_LEFT)  lastch = 0x14b;
        else if (k == SDLK_RIGHT) lastch = 0x14d;
    } else keymask &= ~bit;
}

int plat_getch_ext(void) { return plat_getch(); }
void plat_sleep(int ms)  { SDL_Delay((Uint32)ms); }

int plat_pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return 0;
        if (e.type == SDL_KEYDOWN) key_event(e.key.keysym.sym, 1);
        if (e.type == SDL_KEYUP) key_event(e.key.keysym.sym, 0);
    }
    return 1;
}

unsigned plat_keys(void) { return keymask; }
int plat_getch(void) { int c = lastch; lastch = 0; return c; }

/* SKYROADS_DUMP=<dir>: write a numbered PPM of each ~second of video.
 * Debug aid for headless runs. */
void plat_debug_dump(const uint8_t *fb, const uint8_t (*pal)[3]) {
    static int frame, last;
    const char *dir = SDL_getenv("SKYROADS_DUMP");
    if (!dir) return;
    int now = (int)SDL_GetTicks() / 1000;
    if (now == last) return;
    last = now;
    char path[1100];
    SDL_snprintf(path, sizeof path, "%s/dump_%03d.ppm", dir, frame++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n320 200\n255\n");
    for (int i = 0; i < 320 * 200; i++) {
        uint8_t px[3] = { (uint8_t)(pal[fb[i]][0] << 2), (uint8_t)(pal[fb[i]][1] << 2),
                          (uint8_t)(pal[fb[i]][2] << 2) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

const char *plat_pref_path(void) {
    static char *p;
    if (!p) p = SDL_GetPrefPath("SkyRoadsNative", "SkyRoads");
    return p ? p : "./";
}
const char *plat_base_path(void) {
    static char *p;
    if (!p) p = SDL_GetBasePath();
    return p;
}
