/* assets.c — port of intro.c's loaders/mixers/config + sky2.c's load_trekdat.
 * Faithful to the originals; far pointers become arena offsets. */
#include "assets.h"
#include "assets_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t game_palette[256][3];
uint8_t menu_palette[256][3];
duint   Road_Dat_store[MAX_STAGE_LEN + 32][7];
duint   road_len, gravity, oxy_time, fuel_distance;
duint   Background_Seg, Sample_Seg, PicDatSegments[16];
duint   Cars_Seg;
pic_t   Dash_Pic;
cfg_t   cfg;
duint   Cur;
int     Esc, Break;

duint    speed_display_offset[SPEED_DIVISIONS];
duint    oxy_display_offset[OXY_DIVISIONS];
duint    fuel_display_offset[FUEL_DIVISIONS];
uint8_t *speed_display_dat, *oxy_display_dat, *fuel_display_dat;

static int g_file;                       /* intro.c's global `file` */
static duint Alloc[350];
static duint Segs;
static uint8_t Tmpheap[4096];

/* ---------------- alloc stack ---------------- */
duint alloc(uint32_t bytes) { return Alloc[Segs++] = xalloc(bytes); }
void  start_alloc(void)     { Alloc[Segs++] = 1; }
void  free_top(void)        { Segs--; xfree(Alloc[Segs]); }
void free_memory(void) {
    Segs--;
    while (Alloc[Segs] != 1) { xfree(Alloc[Segs]); Segs--; }
}

void check_error(void) {
    if (SysErr) {
        fprintf(stderr, SysErr == NO_MEM ? "Not enough memory\n"
                                         : "Error loading data files (code %d)\n", SysErr);
        plat_exit(1);
    }
}

/* ---------------- chunk loaders ---------------- */
void open_picture(const char *name) {
    g_file = xopenr(name);
    init_bit_i(g_file, 0, 4096, 0);
}

void close_picture(void) {
    xclose(g_file);
    check_error();
}

static uint8_t egapal[512];

duint load_palette(uint8_t (*pal)[3], duint offset) {
    rd_word(); rd_word();                          /* "CMAP" */
    duint colors = rd_byte();
    if (!SysErr) rd_mem((uint8_t *)pal + 3 * offset, 3 * colors);
    rd_mem(egapal, 2 * colors);
    return colors;
}

duint load_palette_t(pal_t *pal, duint offset) {
    rd_word(); rd_word();
    pal->begcol = offset;
    pal->colors = rd_byte();
    pal->seg = alloc(pal->colors * 3);
    if (!SysErr) {
        rd_mem(seg_ptr(pal->seg), 3 * pal->colors);
        rd_mem(egapal, 2 * pal->colors);
    }
    return pal->colors;
}

static void move_colors(uint8_t *p, uint32_t len, duint offs) {
    for (uint32_t i = 0; i < len; i++)
        if (p[i]) p[i] = (uint8_t)(p[i] + offs);
}

void load_picture(pic_t *p, duint begcol) {
    rd_word();                                     /* "PI" */
    struct { duint seg, addr, lines, len; } hdr;   /* "CT" -> seg (junk) */
    rd_mem(&hdr, sizeof hdr);
    p->addr = hdr.addr; p->lines = hdr.lines; p->len = hdr.len;
    uint32_t len = (uint32_t)p->lines * p->len;
    /* +320 slack: the intro's flash wipe reads mix_line spans of up to 320px
     * from pictures whose stride may be narrower (as the original did from
     * adjacent DOS heap memory). */
    p->seg = alloc(len + 320);
    check_error();
    extr_lzss(seg_ptr(p->seg), (duint)len);
    move_colors(seg_ptr(p->seg), len, begcol);
}

void draw_picture(const pic_t *p) {
    const uint8_t *src = seg_ptr(p->seg);
    uint8_t *dst = vga_mem() + p->addr;
    for (duint i = 0; i < p->lines; i++, dst += 320, src += p->len)
        memcpy(dst, src, p->len);
}

void set_palette(const pal_t *p) {
    set_color_regs(p->begcol, p->colors, (const uint8_t (*)[3])seg_ptr(p->seg));
}

void copy_to_pal(uint8_t (*pal)[3], const pal_t *p) {
    memcpy((uint8_t *)pal + p->begcol * 3, seg_ptr(p->seg), p->colors * 3);
}

/* ---------------- mixing ---------------- */
static duint Bk_Seg, Dest_Seg;
static uint8_t Trans_Cols, Prot_Cols;
duint Line_Len, Src_Seg;

void init_mix(duint bk_seg, duint dest_seg, duint tcols, duint pcols) {
    Bk_Seg = bk_seg; Dest_Seg = dest_seg;
    Trans_Cols = (uint8_t)tcols; Prot_Cols = (uint8_t)pcols;
}

void mix_line(duint dest, duint src, duint begspace, duint endspace) {
    uint8_t *bk = seg_ptr(Bk_Seg), *dst = seg_ptr(Dest_Seg), *sp = seg_ptr(Src_Seg);
    duint di = dest;
    memcpy(dst + di, bk + di, begspace);           /* leading from background */
    di += begspace;
    duint n = Line_Len - begspace - endspace;
    duint si = src;
    for (duint i = 0; i < n; i++, si++, di++) {
        uint8_t al = sp[si];
        if (al < Prot_Cols) continue;              /* keep dest */
        if (al < Trans_Cols) al = bk[di];          /* take background */
        dst[di] = al;
    }
    memcpy(dst + di, bk + di, endspace);
}

void mix_picture(const pic_t *p) {
    Src_Seg = p->seg;
    Line_Len = p->len;
    duint vaddr = p->addr, paddr = 0;
    for (duint i = 0; i < p->lines; i++, vaddr += 320, paddr += p->len)
        mix_line(vaddr, paddr, 0, 0);
}

/* ---------------- fades & timing ----------------
 * Original busy-waits on the PIT tick; here every wait pumps SDL and
 * presents, so the window stays live and the fade is visible. */
static void idle_frame(void) {
    if (!plat_pump()) plat_exit(0);
    plat_tick_update();
    plat_present();
}

void clear_keybuf(void) {
    idle_frame();
    int c;
    while ((c = plat_getch()) != 0)
        if (Break && c == 27) Esc = 1;
}

void delay_ticks(duint ticks) {
    duint t0 = Time;
    while ((duint)(Time - t0) < ticks && !Esc) clear_keybuf();
}

static void make_palette(pal_t *pal, const uint8_t *p) {
    pal->seg = alloc(3 * 256);
    check_error();
    memcpy(seg_ptr(pal->seg), p, 3 * 256);
    pal->begcol = 0;
    pal->colors = 256;
}

void fade_palette(const pal_t *pal1, const pal_t *pal2, duint time) {
    duint t0 = Time;
    int j;
    const uint8_t *p1 = seg_ptr(pal1->seg), *p2 = seg_ptr(pal2->seg);
    do {
        duint el = (duint)(Time - t0);
        j = time ? (int)(el * 100 / time) : 100;
        if (j > 100 || Esc) j = 100;
        uint8_t *p = Tmpheap;
        for (int i = 0; i < (int)(pal1->colors * 3); i++)
            p[i] = (uint8_t)(p1[i] + ((int)p2[i] - (int)p1[i]) * j / 100);
        set_color_regs(pal1->begcol, pal1->colors, (const uint8_t (*)[3])Tmpheap);
        clear_keybuf();
    } while (j != 100);
}

void fade(uint8_t (*palette)[3], int on, duint time) {
    pal_t pal, blackpal;
    memset(Tmpheap, 0, 3 * 256);
    start_alloc();
    make_palette(&pal, (const uint8_t *)palette);
    make_palette(&blackpal, Tmpheap);
    if (on) fade_palette(&blackpal, &pal, time);
    else    fade_palette(&pal, &blackpal, time);
    free_memory();
}

/* ---------------- game data ---------------- */
void load_trekdat(void) {                          /* sky2.c:116 */
    int h = xopenr("trekdat.lzs");
    init_bit_i(h, 0, 4096, 0);
    for (int seg = 0; !SysErr; seg++) {
        duint memlen  = rd_word();
        duint disklen = rd_word();
        if (!SysErr) {
            if (!Sample_Seg) Sample_Seg = xalloc(SMP_LEN);
            uint8_t *p;
            PicDatSegments[seg] = xalloc(memlen);
            p = seg_ptr(PicDatSegments[seg]);
            duint off = memlen - disklen;
            *(duint *)p = off;      /* headroom word at offset 0 (sky2.c:134) */
            if (!SysErr) extr_lzss(p + off, disklen);
        }
    }
    norm_sys_err();
    xclose(h);
}

static void load_display_dat(const char *name, duint *table, duint offs, uint8_t **dat) {
    int h = xopenr(name);
    duint i = (duint)(xseek(h, 0, 2) - offs * 2);
    xseek(h, 0, 0);
    duint seg = alloc(i);
    *dat = seg_ptr(seg);
    check_error();
    xread(h, table, offs * 2);
    xread(h, *dat, i);
    xclose(h);
}

void load_data(void) {                             /* intro.c:870 */
    load_display_dat("oxy_disp.dat", oxy_display_offset, OXY_DIVISIONS, &oxy_display_dat);
    load_display_dat("ful_disp.dat", fuel_display_offset, FUEL_DIVISIONS, &fuel_display_dat);
    load_display_dat("speed.dat", speed_display_offset, SPEED_DIVISIONS, &speed_display_dat);
}

void load_game_data(void) {                        /* intro.c:881 */
    pic_t cars_pic;
    open_picture("cars.lzs");
    load_palette(game_palette, CAR_COLOR);
    load_picture(&cars_pic, CAR_COLOR);
    Cars_Seg = cars_pic.seg;
    close_picture();

    open_picture("dashbrd.lzs");
    load_palette(game_palette, DASHBOARD_COLOR);
    load_picture(&Dash_Pic, DASHBOARD_COLOR);
    close_picture();

    g_file = xopenr("sfx.snd");
    xread(g_file, seg_ptr(Sample_Seg), SMP_LEN);
    norm_sys_err();
    close_picture();
}

duint load_road(duint nr) {                        /* intro.c:906 */
    duint offs, len;
    memset(Road_Dat_store, 0, sizeof Road_Dat_store);
    int h = xopenr("roads.lzs");
    xseek(h, nr * 4, 0);
    xread(h, &offs, 2);
    xread(h, &len, 2);
    xseek(h, offs, 0);
    init_bit_i(h, 0, 4096, 0);
    gravity = rd_word();
    fuel_distance = rd_word();
    oxy_time = rd_word();
    rd_mem(game_palette, STAGE_COLORS * 3);
    extr_lzss((uint8_t *)Road_Dat, len);
    xclose(h);
    return len / 7 / 2;
}

void load_background(duint world) {                /* intro.c:828 */
    pic_t bkgr_pic;
    char name[40];
    snprintf(name, sizeof name, "world%u.lzs", world);
    open_picture(name);
    load_palette(game_palette, BACKGROUND_COLOR);
    load_picture(&bkgr_pic, BACKGROUND_COLOR);
    close_picture();
    Background_Seg = bkgr_pic.seg;
    /* original xresize()s to a full 320x200 screen; our arena block is
     * already zeroed and alloc'd big enough only for the picture, so
     * re-alloc a full screen and copy. */
    duint full = alloc(320 * 200);
    uint8_t *dst = seg_ptr(full);
    memset(dst, 0, 320 * 200);
    {
        const uint8_t *src = seg_ptr(bkgr_pic.seg);
        uint8_t *d = dst + bkgr_pic.addr;
        for (duint i = 0; i < bkgr_pic.lines; i++, d += 320, src += bkgr_pic.len)
            memcpy(d, src, bkgr_pic.len);
    }
    Background_Seg = full;
    memset(dst + DASHBOARD_Y * 320, 0, (200 - DASHBOARD_Y) * 320);
    /* blend dashboard art into the background's bottom rows */
    init_mix(Background_Seg, Background_Seg, 0, 1);
    Line_Len = 320;
    Src_Seg = Dash_Pic.seg;
    for (duint i = 0; i < (duint)((200 - DASH_Y) * 320); i += 320)
        mix_line(i + DASH_Y * 320, i, 0, 0);
}

/* ---------------- config ---------------- */
static duint make_crc(void) {                      /* intro.c:936 */
    duint *u = (duint *)&cfg, crc = 0;
    for (duint i = 1; i < sizeof(cfg) / 2; i++)
        crc += u[i] ^ i;
    if (crc == cfg.crc) return 0;
    cfg.crc = crc;
    return 1;
}

const char *cfg_path(void);

void load_cfg(void) {
    int h = xopenr(cfg_path());
    xread(h, &cfg, sizeof cfg);
    xclose(h);
    if (SysErr || make_crc()) memset(&cfg, 0, sizeof cfg);
    SysErr = 0;
}

void save_cfg(void) {
    make_crc();
    int h = xcreate(cfg_path(), 0);
    xwrite(h, &cfg, sizeof cfg);
    xclose(h);
    SysErr = 0;
}

/* PORTING.md §3.2: plat_exit() longjmps out of the game's blocking loops, so
 * the matching free_memory() calls never run and Segs never returns to 0.  A
 * second run then exhausts Alloc[] and check_error() reports "Not enough
 * memory".  Reclaim everything explicitly before each run. */
void assets_reset_state(void) {
    while (Segs > 0) {
        Segs--;
        if (Alloc[Segs] != 1) xfree(Alloc[Segs]);
    }
    g_file = 0;
    Bk_Seg = Dest_Seg = 0;
    Trans_Cols = Prot_Cols = 0;
}
