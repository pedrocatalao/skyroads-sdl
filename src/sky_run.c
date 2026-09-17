/* sky_run.c — sky2.c's top-level flow, factored out of main() so that both
 * the standalone build and the DXM core (PORTING.md §1) drive the identical
 * sequence.  Nothing here knows which host it is running under. */
#include "assets.h"
#include "platform.h"
#include "render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

duint main_menu(duint draw);
duint gomenu(void);
duint intro(void);
int   game(int the_end);               /* game_play.c */
void  audio_init(void);
void  editor_session(void);            /* editor.c */

enum { NO_CRASH = 0, ABORT = 7 };

const char *cfg_path(void) {
    static char buf[1200];
    snprintf(buf, sizeof buf, "%s%s", plat_pref_path(),
             sky_xmas ? "skyxmas.cfg" : "skyroads.cfg");
    return buf;
}

/* the random-road-song picker keeps one static across runs */
static duint last_muzak = (duint)-1;

/* Reset every file-scope thing sky_run touches, so a second run in the same
 * process starts clean (PORTING.md §3.2).  Called by the core entry before
 * each run; harmless in the standalone build. */
void assets_reset_state(void);
void audio_reset_state(void);

void sky_reset_state(void) {
    last_muzak = (duint)-1;
    assets_reset_state();
    audio_reset_state();
}

int sky_run(void) {
    audio_init();
    srand((unsigned)(plat_now() * 1e6));
    load_cfg();
    play_song(0);
    load_data();
    load_trekdat();
    check_error();
    initvid();

    duint menu_draw;
dem:
    /* sky2.c:193 — an intro left running to the end starts the recorded
     * demo (attract mode); Esc during the intro leaves the logo screen up
     * and the menu draws over it (draw=0). */
    if (!intro() && demo_ok) {
        control_device = CTL_DEMO;
        menu_draw = 1;
    } else {
        control_device = CTL_KEYBOARD;
        menu_draw = 0;
mm:
        control_device = CTL_KEYBOARD;
        switch (main_menu(menu_draw)) {
        case MM_XMAS: return SKY_RESTART_SWAP;  /* main.c swaps edition */
        case MM_EDIT:
            menu_draw = 1;
            editor_session();
            goto mm;
        }
        menu_draw = 1;
    }
    start_alloc();
    load_game_data();
    for (;;) {
        start_alloc();
        if (control_device == CTL_DEMO) {          /* sky2.c:203 */
            road_len = load_road(0);               /* road 0 = the demo road */
            load_background(0);
        } else {
            if (gomenu()) {            /* Esc from road select -> main menu */
                free_memory();
                free_memory();
                goto mm;
            }
            /* sky2.c:214-218 — pick a random road song (2..13), avoid repeats */
            {
                enum { ROAD_MUSICS = 12 };
                duint m = (duint)(rand() % ROAD_MUSICS);
                if (m == last_muzak) m = (m + 1) % ROAD_MUSICS;
                last_muzak = m;
                play_song(2 + m);
            }
            road_len = load_road(Cur + 1);
            load_background(Cur / 3);
        }
        check_error();
        int i;
        duint done = 0;
        for (duint k = 0; k < WORLDS * 3; k++)
            if (cfg.road_completed[k]) done++;
        do {
            i = game(control_device != CTL_DEMO &&
                     !cfg.road_completed[Cur] && done == WORLDS * 3 - 1);
            if (control_device == CTL_DEMO) {      /* sky2.c:249 */
                free_memory();
                free_memory();
                if (i == ABORT) goto mm;           /* key pressed -> menu */
                goto dem;                          /* crashed/finished -> intro */
            }
            if (i == NO_CRASH) {
                cfg.road_completed[Cur]++;
                Cur++;
                save_cfg();
            }
        } while (i != ABORT && i != NO_CRASH);
        free_memory();
    }
}
