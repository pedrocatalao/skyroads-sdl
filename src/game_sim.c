#include <stdlib.h>
/* game_sim.c — headless smoke test for the game_play.c physics port.
 * Follows the assets_headless.c pattern: defines the plat_* stubs, then
 * includes assets.c.  Loads road 1, holds UP for 200 physics ticks, prints
 * forward position and speed every 20 ticks, then aborts via Esc. */
#include "assets_platform.h"
#include "platform.h"
#include "render.h"
#include <stdio.h>
#include <stdint.h>

/* game_play.c physics state (the original's globals) */
extern uint32_t x;                     /* 65536*slabs (road-forward)  */
extern duint    y, z;                  /* 128*pixels                  */
extern int32_t  vx;
extern int16_t  vz;
extern duint    crash_type, fuel, oxy;

int game(int the_end);

#define SIM_TICKS  200
#define BEG_X      (3*65536L)

volatile duint Time;
static int started, ticks_run, esc_now, false_crash;

const char *cfg_path(void) { return "skyroads.cfg"; }

int  plat_pump(void)     { return 1; }
void plat_exit(int c) { exit(c); }
void plat_present(void)  {}
int  plat_getch(void)    { return 0; }
int  plat_getch_ext(void){ return 0; }
void plat_sleep(int ms)  { (void)ms; }

unsigned plat_keys(void) {
    if (esc_now) return K_ESC;
    if (!started) return 0;
    unsigned k = K_UP;                 /* hold UP throughout */
    /* road 1's center lane ends in a hole at slab 20: change to the solid
     * lane on the right (column 4, center y = (95+4*46+23)*128 = 38656)
     * while approaching, then hop once */
    if (ticks_run >= 40 && y < 38400) k |= K_RIGHT;
    if (ticks_run >= 100 && ticks_run < 103) k |= K_SPACE;
    return k;
}

void plat_tick_update(void) {
    Time++;
    if (!started) {
        if (x != 0) started = 1;       /* game_body set x = BEG_X */
        return;
    }
    if (ticks_run <= SIM_TICKS && ticks_run % 20 == 0)
        printf("tick %3d: x=%7.3f slabs  vx=%5d  y=%5u  z=%5u  "
               "fuel=%u oxy=%u crash=%u\n",
               ticks_run, x / 65536.0, (int)vx, y, z, fuel, oxy, crash_type);
    if (crash_type != 0 && !false_crash) {
        false_crash = 1;
        printf("tick %3d: CRASH type=%u at x=%.3f\n",
               ticks_run, crash_type, x / 65536.0);
    }
    if (ticks_run++ >= SIM_TICKS) esc_now = 1;
}

#include "assets.c"

int main(int argc, char **argv) {
    set_data_dir(argc > 1 ? argv[1] : ".");

    load_data();
    load_trekdat();
    check_error();
    initvid();

    start_alloc();
    load_game_data();
    start_alloc();
    road_len = load_road(1);
    load_background(0);
    check_error();
    printf("road 1: len=%u slabs gravity=%u fuel_distance=%u oxy_time=%u\n",
           road_len, gravity, fuel_distance, oxy_time);

    int r = game(0);
    printf("game() returned %d (%s)\n", r,
           r == 7 ? "ABORT as scripted" : r == 0 ? "NO_CRASH" : "crash");
    printf("final: x=%.3f slabs vx=%d crash_during_sim=%s\n",
           x / 65536.0, (int)vx, false_crash ? "YES" : "no");

    int ok = !false_crash && x > (uint32_t)BEG_X && vx >= 0;
    printf(ok ? "SIM OK\n" : "SIM FAIL\n");
    return ok ? 0 : 1;
}

void sbdma(const uint8_t *buf, uint32_t len, duint smprate) { (void)buf; (void)len; (void)smprate; }
void sbstop(void) {}

void plat_osd(const char *msg) { (void)msg; }
