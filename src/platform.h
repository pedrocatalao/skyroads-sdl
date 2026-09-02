/* platform.h — SDL2 window/input/timing for the SkyRoads port. */
#ifndef SKY_PLATFORM_H
#define SKY_PLATFORM_H

#include "compat.h"

int  plat_init(const char *title, int scale);
void plat_quit(void);

/* Blit the 320x200 8-bit VGA arena buffer through the current palette. */
void plat_present(void);

/* Pump events; returns 0 when the window is closed / Cmd-Q. */
int  plat_pump(void);

/* Key state, DOS-ish: arrows, space, esc, enter... */
enum {
    K_LEFT = 1 << 0, K_RIGHT = 1 << 1, K_UP = 1 << 2, K_DOWN = 1 << 3,
    K_SPACE = 1 << 4, K_ESC = 1 << 5, K_RET = 1 << 6,
};
unsigned plat_keys(void);          /* currently-held key mask */
int      plat_getch(void);         /* last pressed key (ASCII-ish), 0 if none */
int      plat_getch_ext(void);     /* like plat_getch but arrows as 0x148/0x150/0x14b/0x14d */
void     plat_sleep(int ms);

/* Leave the game.  Standalone: exit().  DXM core: longjmp back to the core
 * entry point so the host process survives (PORTING.md 3.1).  Game code MUST
 * use this instead of exit(). */
void     plat_exit(int code);

/* 36.4 Hz "volatile tick" clock — the game's PIT timer (0x19e4 divisor). */
extern volatile duint Time;
void plat_tick_update(void);       /* call once per frame to advance Time */

extern void (*plat_f9_hook)(void);
double plat_now(void);             /* seconds, monotonic */
void plat_osd(const char *msg);    /* flash a message in the corner ~1.5s */
const char *plat_pref_path(void);  /* ~/Library/Application Support/SkyRoads/ */
const char *plat_base_path(void);  /* bundle Resources dir (or NULL) */

#endif
