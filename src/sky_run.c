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

enum { NO_CRASH = 0, ABORT = 7 };

const char *cfg_path(void) {
    static char buf[1200];
    if (!buf[0]) snprintf(buf, sizeof buf, "%sskyroads.cfg", plat_pref_path());
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

    /* Esc during the intro leaves the logo screen up and the menu draws
     * over it (draw=0); a completed intro faded to black (draw=1). */
    duint menu_draw = intro() ? 0 : 1;
mm:
    main_menu(menu_draw);
    menu_draw = 1;
    start_alloc();
    load_game_data();
    for (;;) {
        start_alloc();
        if (gomenu()) {                /* Esc from road select -> main menu */
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
        check_error();
        int i;
        duint done = 0;
        for (duint k = 0; k < WORLDS * 3; k++)
            if (cfg.road_completed[k]) done++;
        do {
            i = game(!cfg.road_completed[Cur] && done == WORLDS * 3 - 1);
            if (i == NO_CRASH) {
                cfg.road_completed[Cur]++;
                Cur++;
                save_cfg();
            }
        } while (i != ABORT && i != NO_CRASH);
        free_memory();
    }
}
