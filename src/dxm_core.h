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
 * game needs a newer DXM" rather than failing to load it. */
#define DXM_ABI 1

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
    const char *data_dir;
    const char *pref_dir;
} dxm_host;

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

/* Each core exports these two, prefixed (PORTING.md §3.8). */
const dxm_core_info *sky_core_info(void);
int                  sky_core_main(const dxm_host *host, const char *data_dir);

#endif
