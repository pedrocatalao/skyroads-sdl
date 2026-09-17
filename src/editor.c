/* editor.c — port of Bluemoon's own in-game road editor (editor.c in the
 * original source, compiled behind #if STAGE_EDITOR; dev builds invoked it
 * as `stage_editor(argv[1])` from sky2.c:225).  This is the tool the 30
 * original roads were built with, shipping to players for the first time.
 *
 * Deviations from the 1993 editor (all intentional):
 *  - Esc returns to the main menu (original quit to DOS).
 *  - F2 saves (original used F10, which is our CRT-effects toggle).  Roads
 *    are saved as single-road .ro files — the same format as the source
 *    tree's roads/ahti_*.ro — into the pref dir, never into roads.lzs.
 *  - Ctrl+D shell escape dropped.
 *  - Enter test-flies the road with the real physics, then returns HERE
 *    (the road is snapshotted first — gameplay chips burning/breaking
 *    slabs, and edits must survive the flight).
 *  - Legends render in our 8x8 font, uppercase.
 *
 * Everything else — keys, palette raytracing, the 16-color scheme, the
 * grid semantics — is the original, line for line where possible. */
#include "assets.h"
#include "platform.h"
#include "render.h"
#include "font8.h"
#include <stdio.h>
#include <string.h>

#define SCRY 11
#define SCRX 7
#define C    60

#define SLAB_FRONT      (duint)((59269L+407L*C)/1000)
#define SLAB_RIGHT      (duint)((30680L+693L*C)/1000)
#define SLAB_LEFT       C
#define WALL_FRONT      SLAB_FRONT
#define WALL_RIGHT      SLAB_RIGHT
#define WALL_LEFT       C
#define WALL_INSIDE     C
#define TUNNEL_INSIDE   C
#define TUNNEL_FRONT    SLAB_FRONT
#define TUNNEL_SIDE2    (duint)((91820L+82L*C)/1000)
#define TUNNEL_SIDE3    (duint)((56944L+431L*C)/1000)
#define TUNNEL_SIDE4    C

enum { KEY_ESC = 27, KEY_RET = 13,
       KEY_UP = 0x148, KEY_DOWN = 0x150, KEY_LEFT = 0x14b, KEY_RIGHT = 0x14d,
       KEY_HOME = 0x147, KEY_END = 0x14f, KEY_INS = 0x152, KEY_DEL = 0x153,
       KEY_F1 = 0x13b, KEY_F2 = 0x13c,
       CTRL_R = 'r'-'a'+1, CTRL_G = 'g'-'a'+1, CTRL_B = 'b'-'a'+1,
       ALT_R = 0x113, ALT_G = 0x122, ALT_B = 0x130 };

static duint Edit_Y, Edit_X;
static const char pcolorkeys[] = " aesdfghrqwjtkl;";  /* plate colors 0..15 */
static const char rcolorkeys[] = "\x01""AESDFGHRQWJTKL:"; /* roof colors 1..15 */
static const char elementkeys[] = "123456";
static const char fixed_colors[] = "\x2\x8\x9\xa\xc";

extern duint control_device;
int game(int the_end);                     /* game_play.c */
enum { E_ABORT = 7 };

/* --- 8x8 text into VGA memory, opaque cell (bg color), original-style --- */
static void etext(duint tx, duint ty, const char *s, duint col, duint bg) {
    uint8_t *vga = vga_mem();
    for (; *s; s++, tx += 8) {
        const uint8_t *g = font8_glyph(*s);
        for (duint r = 0; r < 8; r++)
            for (duint c = 0; c < 8; c++)
                vga[(ty + r) * 320 + tx + c] =
                    (g && (g[r] & (0x80u >> c))) ? (uint8_t)col : (uint8_t)bg;
    }
}
static void eput_char(duint tx, duint ty, char ch, duint col) {
    char b[2] = { ch, 0 };
    etext(tx, ty, b, col, 0);
}
static const char *unum(duint v) {          /* 5-char right-aligned number */
    static char b[8];
    snprintf(b, sizeof b, "%5u", (unsigned)v);
    return b;
}

/* --- palette machinery, straight from the original --- */
static uint8_t bk_palette[256][3], def_palette[256][3];

static void shade(duint dest, duint src, duint factor) {
    game_palette[dest][0] = (uint8_t)(game_palette[src][0] * factor / 100);
    game_palette[dest][1] = (uint8_t)(game_palette[src][1] * factor / 100);
    game_palette[dest][2] = (uint8_t)(game_palette[src][2] * factor / 100);
}

static void ray_trace(void) {
    for (duint i = 1; i < 16; i++) {
        shade(i + 15, i, SLAB_FRONT);       /* 1..15-slab roof */
        shade(i + 30, i, SLAB_RIGHT);
        shade(i + 45, i, SLAB_LEFT);
    }
    shade(62, 61, WALL_FRONT);              /* 61-wall roof */
    shade(63, 61, WALL_RIGHT);
    shade(64, 61, WALL_LEFT);
    shade(65, 61, WALL_INSIDE);
    shade(67, 68, TUNNEL_INSIDE);           /* 68-tunnel side 1 */
    shade(66, 68, TUNNEL_FRONT);
    shade(69, 68, TUNNEL_SIDE2);
    shade(70, 68, TUNNEL_SIDE3);
    shade(71, 68, TUNNEL_SIDE4);
    set_color_regs(0, STAGE_COLORS, (const uint8_t (*)[3])game_palette);
}

/* the original read the BIOS default palette; colors 1..15 are the classic
 * VGA 16-color set, which is all ray_trace shades from */
static const uint8_t vga16[16][3] = {
    { 0, 0, 0},{ 0, 0,42},{ 0,42, 0},{ 0,42,42},{42, 0, 0},{42, 0,42},
    {42,21, 0},{42,42,42},{21,21,21},{21,21,63},{21,63,21},{21,63,63},
    {63,21,21},{63,21,63},{63,63,21},{63,63,63} };

static duint stage_len(void) {
    duint len = MAX_STAGE_LEN;
    while (len > 0) {
        const duint *row = Road_Dat[len - 1];
        duint any = 0;
        for (duint i = 0; i < SCRX; i++) any |= row[i];
        if (any) break;
        len--;
    }
    return len ? len : 1;
}

static char save_name[64];
static void save_stage(void) {
    char path[1400];
    snprintf(path, sizeof path, "%s%s", plat_pref_path(), save_name);
    FILE *f = fopen(path, "wb");
    if (!f) { plat_osd("SAVE FAILED"); return; }
    duint len = stage_len();
    uint8_t hdr[6] = { (uint8_t)gravity, (uint8_t)(gravity >> 8),
                       (uint8_t)fuel_distance, (uint8_t)(fuel_distance >> 8),
                       (uint8_t)oxy_time, (uint8_t)(oxy_time >> 8) };
    fwrite(hdr, 1, 6, f);
    fwrite(game_palette, 1, STAGE_COLORS * 3, f);
    for (duint r = 0; r < len; r++)
        for (duint i = 0; i < SCRX; i++) {
            uint8_t w[2] = { (uint8_t)Road_Dat[r][i],
                             (uint8_t)(Road_Dat[r][i] >> 8) };
            fwrite(w, 1, 2, f);
        }
    fclose(f);
    plat_osd(save_name);
}

static int egetch(void) {
    for (;;) {
        if (!plat_pump()) plat_exit(0);
        plat_tick_update();
        plat_present();
        int c = plat_getch_ext();
        if (c) return c;
        plat_sleep(10);
    }
}

#define bound(v, lo, hi) do { if ((sint)(v) < (lo)) (v) = (lo); \
                              if ((sint)(v) > (hi)) (v) = (hi); } while (0)

static duint copybuf[MAX_STAGE_LEN][SCRX];

/* returns: 0/1 = Enter (test-fly; value is `buildings`), -1 = Esc (leave) */
static int stage_editor(duint vga_buf_seg) {
    duint col;
    duint *sptr;
    duint buildings = 1;
    const char *s;

    memset(vga_mem() + 138 * 320, 0, 320 * (200 - 138));

    memcpy(bk_palette, game_palette, sizeof bk_palette);
    /* original: setvideomode + BIOS palette read; the base 16 are vga16 */
    memset(game_palette, 0, sizeof game_palette);
    memcpy(game_palette, vga16, sizeof vga16);
    ray_trace();
    memcpy(def_palette, game_palette, sizeof def_palette);
    memcpy(game_palette, bk_palette, sizeof bk_palette);
    ray_trace();
    set_color_regs(0, 256, (const uint8_t (*)[3])game_palette);

    etext(256, 142, "  SLAB",   11, 0);
    etext(256, 150, "  TUNNEL", 11, 0);
    etext(256, 158, "  WALL",   11, 0);
    etext(256, 166, "  ARCH",   11, 0);
    etext(256, 174, "  2*WALL", 11, 0);
    etext(256, 182, "  2*ARCH", 11, 0);
    eput_char(256, 142, '1', 10); eput_char(256, 150, '2', 2);
    eput_char(256, 158, '3', 10); eput_char(256, 166, '4', 2);
    eput_char(256, 174, '5', 10); eput_char(256, 182, '6', 2);

    etext(184, 142, "SUPPLIES", 11, 0); eput_char(168, 142, 'Q', 9);
    etext(184, 150, "SPEED +",  11, 0); eput_char(168, 150, 'W', 10);
    etext(184, 158, "SPEED -",  11, 0); eput_char(168, 158, 'E', 2);
    etext(184, 166, "SLIPPERY", 11, 0); eput_char(168, 166, 'R', 8);
    etext(184, 174, "BURNING",  11, 0); eput_char(168, 174, 'T', 12);

    etext(0, 190, "ESC F1 F2-SAVE INS DEL SHIFT ENTER-FLY", 15, 3);

    eput_char(120, 142, 'a', 1);  eput_char(120, 150, 's', 3);
    eput_char(120, 158, 'd', 4);  eput_char(120, 166, 'f', 5);
    eput_char(120, 174, 'g', 6);
    eput_char(136, 142, 'h', 7);  eput_char(136, 150, 'j', 11);
    eput_char(136, 158, 'k', 13); eput_char(136, 166, 'l', 14);
    eput_char(136, 174, ';', 15);

    etext(0, 150, "LEN:",  11, 0);
    etext(0, 158, "POS:",  11, 0);
    etext(0, 166, "GRAV +/-:", 13, 0);
    etext(0, 174, "FUEL [/]:", 13, 0);
    etext(0, 182, "OXY  {/}:", 13, 0);
    etext(120, 182, "R:   G:   B:", 14, 0);

redraw:
    etext(80, 166, unum(gravity), 13, 0);
    etext(80, 174, unum(fuel_distance), 13, 0);
    etext(80, 182, unum(oxy_time), 13, 0);

    for (;;) {
        etext(40, 158, unum(Edit_Y), 11, 0);
        etext(40, 150, unum(road_len = stage_len()), 11, 0);
        sptr = &Road_Dat[Edit_Y][Edit_X];

        if (((*sptr) >> 8) == 1)            col = 68;
        else if (*sptr & 0xf00)             col = 61;
        else if (strchr(fixed_colors, (char)(*sptr & 15)) && (*sptr & 15))
                                            col = STAGE_COLORS;
        else                                col = (*sptr & 0xf);

        etext(136, 182, (col == STAGE_COLORS) ? "--" : unum(game_palette[col][0]) + 3, 14, 0);
        etext(176, 182, (col == STAGE_COLORS) ? "--" : unum(game_palette[col][1]) + 3, 14, 0);
        etext(216, 182, (col == STAGE_COLORS) ? "--" : unum(game_palette[col][2]) + 3, 14, 0);

        if (!buildings) {                   /* F1: plates only, structures off */
            memcpy(copybuf, Road_Dat, sizeof copybuf);
            for (duint r = 0; r < MAX_STAGE_LEN; r++)
                for (duint i = 0; i < SCRX; i++)
                    Road_Dat[r][i] &= 15;
        }
        video((int)(Edit_X * 46 + 46 / 2 - 13 + 109), (int)(Edit_Y * 8), 100,
              seg_ptr(Cars_Seg), 1, 100, vga_buf_seg);
        if (!buildings) memcpy(Road_Dat, copybuf, sizeof copybuf);

        memcpy(vga_mem(), seg_ptr(vga_buf_seg), 320 * 138);

        int c = egetch();
        switch (c) {
        case CTRL_R: game_palette[col][0]--; goto setpal;
        case CTRL_G: game_palette[col][1]--; goto setpal;
        case CTRL_B: game_palette[col][2]--; goto setpal;
        case ALT_R:  game_palette[col][0]++; goto setpal;
        case ALT_G:  game_palette[col][1]++; goto setpal;
        case ALT_B:  game_palette[col][2]++;
setpal:     game_palette[col][0] &= 63;
            game_palette[col][1] &= 63;
            game_palette[col][2] &= 63;
            ray_trace();
            break;
        case '*': memcpy(game_palette, def_palette, sizeof def_palette);
                  ray_trace(); break;
        case '/': memcpy(game_palette, bk_palette, sizeof bk_palette);
                  ray_trace(); break;
        case '+': gravity += 2;             /* falls through, original style */
        case '-': gravity--;
                  bound(gravity, 4, 20); goto redraw;
        case '[': fuel_distance -= 2;
        case ']': fuel_distance++;
                  bound(fuel_distance, 1, 1000); goto redraw;
        case '{': oxy_time -= 2;
        case '}': oxy_time++;
                  bound(oxy_time, 1, 800); goto redraw;
        case KEY_F1: buildings ^= 1; break;
        case KEY_F2: save_stage(); break;
        case KEY_ESC: return -1;
        case KEY_RET: return (int)buildings;
        case KEY_DOWN: if (Edit_Y) Edit_Y--; break;
        case KEY_UP:   if (Edit_Y < MAX_STAGE_LEN - SCRY) Edit_Y++; break;
        case KEY_LEFT: if (Edit_X) Edit_X--; break;
        case KEY_RIGHT:if (Edit_X < SCRX - 1) Edit_X++; break;
        case KEY_HOME: Edit_Y = 0; break;
        case KEY_END:  Edit_Y = stage_len() - 1; break;
        case KEY_DEL:
            memmove(&Road_Dat[Edit_Y][0], &Road_Dat[Edit_Y + 1][0],
                    (MAX_STAGE_LEN - Edit_Y - 1) * SCRX * sizeof(duint));
            break;
        case KEY_INS:
            memmove(&Road_Dat[Edit_Y + 1][0], &Road_Dat[Edit_Y][0],
                    (MAX_STAGE_LEN - Edit_Y - 1) * SCRX * sizeof(duint));
            memset(&Road_Dat[Edit_Y][0], 0, SCRX * sizeof(duint));
            break;
        default:
            if ((s = strchr(pcolorkeys, c)) && c) {
                if (plat_shift_down() && s == pcolorkeys)
                    *sptr &= 0xf0f;         /* Shift+Space: clear roof */
                else {
                    *sptr &= 0xff0;
                    *sptr |= (duint)(s - pcolorkeys);
                }
            }
            if ((s = strchr(rcolorkeys, c)) && c && s != rcolorkeys) {
                *sptr &= 0xf0f;
                *sptr |= (duint)(s - rcolorkeys) << 4;
            }
            if ((s = strchr(elementkeys, c)) && c) {
                *sptr &= 0x0ff;
                *sptr |= (duint)(s - elementkeys) << 8;
            }
            if (!(*sptr & 0xf00))
                *sptr &= 0xf;
        }
    }
}

/* menu-level glue: pick a road, edit it, Enter test-flies, Esc backs out */
void editor_session(void) {
    duint gomenu(void);
    static duint road_snapshot[MAX_STAGE_LEN + 32][7];

    start_alloc();
    load_game_data();
    for (;;) {
        start_alloc();
        if (gomenu()) {                    /* Esc: back to main menu */
            free_memory();
            free_memory();
            return;
        }
        road_len = load_road(Cur + 1);
        load_background(Cur / 3);
        check_error();
        snprintf(save_name, sizeof save_name, "road_%u.ro", (unsigned)(Cur + 1));
        Edit_Y = 0; Edit_X = 0;
        duint vga_buf = alloc(320 * 138);
        check_error();
        control_device = CTL_KEYBOARD;
        for (;;) {
            int r = stage_editor(vga_buf);
            if (r < 0) break;
            /* Enter: test-fly with the real physics, then come back */
            memcpy(road_snapshot, Road_Dat_store, sizeof road_snapshot);
            duint g = gravity, fd = fuel_distance, ot = oxy_time;
            game(0);
            memcpy(Road_Dat_store, road_snapshot, sizeof road_snapshot);
            gravity = g; fuel_distance = fd; oxy_time = ot;
        }
        free_memory();
    }
}
