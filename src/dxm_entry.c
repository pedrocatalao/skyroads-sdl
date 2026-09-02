/* dxm_entry.c — everything DXM-specific in this repo, and nothing more
 * (PORTING.md §4).  The adapter itself lives in DXM and is shared by every
 * port; all a game declares is what it is and how to start it. */
#include "dxm_core.h"
#include "compat.h"

int  sky_run(void);
void sky_reset_state(void);
void set_data_dir(const char *dir);

static const dxm_mode MODE_13H = {
    320, 200, DXM_FB_INDEX8, 5, 6, /* pixels taller than wide -> 4:3 */ 400
};

static const dxm_core_info INFO = {
    DXM_ABI,
    "skyroads", "SKYROADS.EXE", "SkyRoads", "BlueMoon Software", 1993,
    &MODE_13H, 1, "roads.lzs"
};

void sky_audio_render(int16_t *out, int frames);   /* audio.c, SKY_CORE */

/* The three exported entry points.  Fixed names, because the shell resolves
 * them by name out of a loaded module; everything else this library contains
 * is hidden. */
DXM_EXPORT const dxm_core_info *dxm_core_get_info(void) { return &INFO; }

DXM_EXPORT void dxm_core_audio(int16_t *out, int frames) {
    sky_audio_render(out, frames);
}

DXM_EXPORT int dxm_core_main(const dxm_host *host, const char *data_dir) {
    dxm_adapter_bind(host);
    set_data_dir(data_dir);
    sky_reset_state();                       /* PORTING.md §3.2 */
    if (setjmp(*dxm_adapter_exit_target()))  /* PORTING.md §3.1 */
        return 0;                            /* plat_exit() lands here */
    return sky_run();
}
