/* main.c — standalone entry point.  Data-dir discovery and platform bring-up
 * only; the game's actual flow lives in sky_run.c so the DXM core drives the
 * identical sequence (PORTING.md §1).  Excluded from the SKY_CORE build. */
#include "assets.h"
#include "platform.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int  sky_run(void);
void sky_reset_state(void);

static int has_data(const char *dir) {
    const char *names[] = { "roads.lzs", "ROADS.LZS" };
    for (int i = 0; i < 2; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, names[i]);
        FILE *f = fopen(p, "rb");
        if (f) { fclose(f); return 1; }
    }
    return 0;
}

int main(int argc, char **argv) {
    /* data dir: explicit arg, else the exe's dir (mac bundle Resources /
     * flat layout), else a data/ next to the exe (linux tarball), else
     * ./data, else cwd */
    const char *base = plat_base_path();
    static char basedata[1200];
    const char *dd;
    if (argc > 1)                    dd = argv[1];
    else if (base && has_data(base)) dd = base;
    else if (base && (snprintf(basedata, sizeof basedata, "%sdata", base),
                      has_data(basedata)))
                                     dd = basedata;
    else if (has_data("data"))       dd = "data";
    else                             dd = ".";
    set_data_dir(dd);

    /* Xmas Special: a complete second data set in <data>/xmas (fetched by
     * get_data.sh); the X key on the main menu swaps editions. */
    static char xdir[1240];
    snprintf(xdir, sizeof xdir, "%s/xmas", dd);
    xmas_available = has_data(xdir);
    if (plat_init("SkyRoads", 3) != 0) {
        fprintf(stderr, "SDL init failed\n");
        return 1;
    }
    for (;;) {
        sky_reset_state();
        int r = sky_run();
        if (r != SKY_RESTART_SWAP) return r;
        sky_xmas = !sky_xmas;              /* X on the main menu */
        set_data_dir(sky_xmas ? xdir : dd);
    }
}
