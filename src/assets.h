/* assets.h — ported data structures + loaders from intro.c / sky2.c. */
#ifndef SKY_ASSETS_H
#define SKY_ASSETS_H

#include "compat.h"

#define STAGE_COLORS        72
#define CAR_COLOR           72
#define DASHBOARD_COLOR     92
#define BACKGROUND_COLOR    142
#define DASHBOARD_Y         138
#define DASH_Y              129
#define WORLDS              10
#define MAX_STAGE_LEN       500
#define SMP_LEN             32100
#define FADE_TIME           36

/* menu colour bases (intro.c) */
#define DEMO_ROAD_COLOR     0
#define FLASH_COLOR         50
#define TEXT_FADE_COLOR     160
#define MAIN_MENU_COLOR     190
#define CTRL_MENU_COLOR     200
#define CTRL_CURSOR_COLOR   250
#define GO_MENU_COLOR       0
#define GO_MENU_CUR_COLOR   240

typedef struct { duint seg, addr, lines, len; } pic_t;
typedef struct { duint seg, begcol, colors; } pal_t;

typedef struct {
    duint crc;
    duint control_device;
    duint silence;
    duint road_completed[WORLDS * 3];
} cfg_t;

extern uint8_t game_palette[256][3];
extern uint8_t menu_palette[256][3];
/* margin rows before (8) and after (24): the renderer reads front-neighbor
 * rows (cell[-7]) below row 0, and up to ~10 rows past the road end during
 * display_road_end on maximum-length roads. */
extern duint   Road_Dat_store[MAX_STAGE_LEN + 32][7];
#define Road_Dat (Road_Dat_store + 8)
extern duint   road_len, gravity, oxy_time, fuel_distance;
extern duint   Background_Seg, Sample_Seg, PicDatSegments[16];
extern duint   Cars_Seg;                 /* car sprite data (cars.lzs) */
extern pic_t   Dash_Pic;
extern cfg_t   cfg;
extern duint   Cur;
extern int     Esc, Break;

/* alloc stack (intro.c) */
duint alloc(uint32_t bytes);
void  start_alloc(void);
void  free_memory(void);
void  free_top(void);                    /* original `free()` */

void check_error(void);

/* chunk loaders */
void  open_picture(const char *name);
void  close_picture(void);
duint load_palette(uint8_t (*pal)[3], duint offset);
duint load_palette_t(pal_t *pal, duint offset);
void  load_picture(pic_t *p, duint begcol);
void  draw_picture(const pic_t *p);
void  set_palette(const pal_t *p);
void  copy_to_pal(uint8_t (*pal)[3], const pal_t *p);

/* mixing (intro.c mix_line/mix_picture) */
void init_mix(duint bk_seg, duint dest_seg, duint tcols, duint pcols);
void mix_picture(const pic_t *p);
void mix_line(duint dest, duint src, duint begspace, duint endspace);
extern duint Line_Len, Src_Seg;

/* fades + timing (pump SDL inside) */
void fade(uint8_t (*pal)[3], int on, duint time);
void fade_palette(const pal_t *p1, const pal_t *p2, duint time);
void delay_ticks(duint ticks);
void clear_keybuf(void);

/* game data */
#define DEMO_CONTROLS 6398               /* public.h:47 demo_controls_t[6398] */
extern uint8_t demo_controls[DEMO_CONTROLS];
extern duint control_device;             /* game_play.c; 0=KEYBOARD 3=DEMO */
#define CTL_KEYBOARD 0
#define CTL_DEMO     3

/* main_menu() outcomes (Start = 0, matching the original's fall-through) */
enum { MM_PLAY = 0, MM_XMAS = 2, MM_EDIT = 3 };
#define SKY_RESTART_SWAP 100             /* sky_run(): relaunch, other edition */
extern duint sky_xmas;                   /* 1 = running the Xmas Special */
extern duint xmas_available;             /* xmas data dir found at startup */
extern duint demo_ok;                    /* demo.rec loaded — attract enabled */
void  load_trekdat(void);
void  load_data(void);
void  load_game_data(void);
duint load_road(duint nr);               /* returns road_len in slabs */
void  load_background(duint world);

/* config */
void load_cfg(void);
void save_cfg(void);

/* music stub (task 6 wires the OPL2 core here) */
void play_song(duint songnr);
void stop_song(void);

/* display gauge tables (game.c dashboard) */
#define SPEED_DIVISIONS 34
#define OXY_DIVISIONS   10
#define FUEL_DIVISIONS  10
extern duint    speed_display_offset[SPEED_DIVISIONS];
extern duint    oxy_display_offset[OXY_DIVISIONS];
extern duint    fuel_display_offset[FUEL_DIVISIONS];
extern uint8_t *speed_display_dat, *oxy_display_dat, *fuel_display_dat;

#endif
