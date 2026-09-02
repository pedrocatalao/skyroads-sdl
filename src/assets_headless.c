#include <stdlib.h>
/* assets_headless.c — assets.c with a headless platform stub. */
#include "assets_platform.h"
volatile duint Time;
int  plat_pump(void) { return 1; }
void plat_exit(int c) { exit(c); }
void plat_tick_update(void) { Time++; }
void plat_present(void) {}
int  plat_getch(void) { return 0; }
const char *cfg_path(void) { return "skyroads.cfg"; }
void sbdma(const uint8_t *buf, uint32_t len, duint smprate) { (void)buf; (void)len; (void)smprate; }
void sbstop(void) {}
void play_song(duint songnr) { (void)songnr; }
void stop_song(void) {}
#include "assets.c"
