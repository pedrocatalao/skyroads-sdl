/* dxm_core.h — the contract between DOS ex Machina and a game core.
 * Normative reference: PORTING.md.  Pure C; no SDL, no GL types. */
#ifndef DXM_CORE_H
#define DXM_CORE_H
#include <stdint.h>

/* The ABI version of this contract.  A port compiles against its own copy of
 * this header - it must, since it has to build standalone without DXM
 * checked out anywhere - so the two copies can drift.  DXM asserts on this
 * where it links a core, which turns drift into a compile error instead of
 * a crash at the first present().  Bump it whenever anything below changes
 * shape; the catalogue manifest carries the same number so DXM can say "this
 * game needs a newer DXM" rather than failing to load it.
 *
 * 2: dxm_host gained lock/unlock.  A core built as a loadable module carries
 *    the adapter inside it and must link no threading library of its own, so
 *    the one lock it needs comes from the shell that owns the thread. */
#define DXM_ABI 2

#define DXM_PIT_HZ 1193182.0    /* 8253 input clock; a port picks its divisor */

typedef enum { DXM_FB_INDEX8 = 0, DXM_FB_RGB888 = 1 } dxm_fb_format;

/* A video mode the core can be in.  Declared, never assumed: a DOS game that
 * shows a text intro then switches to Mode 13h is normal (PORTING.md §2.1). */
typedef struct dxm_mode {
    int           w, h;         /* framebuffer dimensions                    */
    dxm_fb_format format;
    int           par_num, par_den;  /* PIXEL aspect: 320x200 is 6:5         */
    int           crt_lines;    /* PHYSICAL scanlines this mode drove.  13h  */
                                /* is line-doubled: 400, not 200 (§2.2).     */
} dxm_mode;

typedef struct dxm_frame {
    const dxm_mode *mode;
    const uint8_t  *pixels;         /* w*h (INDEX8) or w*h*3 (RGB888)        */
    const uint8_t  *palette;        /* 256*3 bytes; NULL unless INDEX8       */
} dxm_frame;

typedef struct dxm_core_info {
    /* FIRST, and it stays first.  A version field is only useful if it can
     * be read from a core built against a different version of this header,
     * which means its offset must never move.  Set it to DXM_ABI; the shell
     * refuses a core whose value it does not know.  Today the core is linked
     * in and this is trivially true - it earns its place when cores become
     * loadable modules built somewhere else entirely. */
    int             abi;
    const char     *id;             /* "skyroads" — also the DOS command     */
    const char     *exe_name;       /* "SKYROADS.EXE" — shown by DIR         */
    const char     *title, *publisher;
    int             year;
    const dxm_mode *modes;
    int             n_modes;
    const char     *data_probe;     /* file proving data present             */
} dxm_core_info;

/* Provided BY the shell TO the core. */
typedef struct dxm_host {
    void   (*present)(const dxm_frame *f);
    int    (*key_down)(int xt_scancode);
    int    (*getch)(void);          /* BIOS-style; 0 if none, ext = 0x100|sc */
    int    (*should_quit)(void);
    double (*now)(void);            /* monotonic seconds                     */
    void   (*sleep_ms)(int ms);
    void   (*log)(const char *msg);
    /* The core's one lock, guarding audio state against whoever renders it.
     * It comes from the shell because the shell owns the thread - and
     * because a core shipped as a module must not link a threading library
     * of its own, which is what makes it loadable everywhere. */
    void   (*lock)(void);
    void   (*unlock)(void);
    const char *data_dir;
    const char *pref_dir;
} dxm_host;

/* ---- a core as a loadable module -------------------------------------
 * A module exports exactly these three symbols and hides everything else,
 * so two games can be loaded at once without their globals colliding.  The
 * names are FIXED - the shell resolves them by name, so they cannot carry
 * the per-port prefix the rest of a port's symbols do (PORTING.md 3.8). */
#if defined(_WIN32)
#  define DXM_EXPORT __declspec(dllexport)
#else
#  define DXM_EXPORT __attribute__((visibility("default")))
#endif

#define DXM_SYM_INFO  "dxm_core_get_info"
#define DXM_SYM_MAIN  "dxm_core_main"
#define DXM_SYM_AUDIO "dxm_core_audio"

typedef const dxm_core_info *(*dxm_core_get_info_fn)(void);
typedef int                  (*dxm_core_main_fn)(const dxm_host *h,
                                                 const char *data_dir);
typedef void                 (*dxm_core_audio_fn)(int16_t *out, int frames);

/* XT scancodes the shell speaks (PORTING.md §2.3). */
enum {
    DXM_SC_ESC = 0x01, DXM_SC_ENTER = 0x1C, DXM_SC_SPACE = 0x39,
    DXM_SC_UP = 0x48, DXM_SC_LEFT = 0x4B, DXM_SC_RIGHT = 0x4D,
    DXM_SC_DOWN = 0x50, DXM_SC_F9 = 0x43, DXM_SC_F10 = 0x44,
};

/* Provided by the shared adapter (platform_dxm.c) to a core's dxm_entry.c. */
#include <setjmp.h>
void     dxm_adapter_bind(const dxm_host *h);
jmp_buf *dxm_adapter_exit_target(void);

/* Every core exports exactly these three, under exactly these names - see
 * DXM_SYM_* above.  They are the only symbols a module makes visible. */
DXM_EXPORT const dxm_core_info *dxm_core_get_info(void);
DXM_EXPORT int  dxm_core_main(const dxm_host *host, const char *data_dir);
DXM_EXPORT void dxm_core_audio(int16_t *out, int frames);

#endif
