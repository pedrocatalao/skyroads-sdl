/* assets_platform.h — indirection so assets.c links either against the
 * real SDL platform (game) or a headless stub (tools/tests). */
#ifndef SKY_ASSETS_PLATFORM_H
#define SKY_ASSETS_PLATFORM_H
#include "compat.h"
int  plat_pump(void);
void plat_tick_update(void);
void plat_present(void);
int  plat_getch(void);
void plat_exit(int code);
extern volatile duint Time;
#endif
