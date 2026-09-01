/* render.c — C port of trek.asm (the SkyRoads 3D road renderer), VGA path.
 * Follows native/docs/trek_blueprint.md; full-redraw model (AllScreen every
 * frame), so the dirty-fragment machinery (§3.1) is intentionally absent.
 */
#include "assets.h"
#include "render.h"
#include <string.h>

#include "render_tables.h"


enum { LINE0 = 32, MINX = 110, MAXX = MINX + 319,
       CARW = 29, CARH = 24, GROUNDZ = 80,
       MINZ = GROUNDZ - CARH - 36, MAXZ = MINZ - 1 + 138,
       ROWS = 11, VIRTUAL_ROWS = ROWS + 2, GROUNDROWS = 4, COLS = 7,
       PHASES = 8, PHAS = 3, ELEMENTS = 20,
       SHADOWS = 5, SHDCONST = 5, SHDH = 9, SHDZDIF = 8,
       INDEXSIZE = VIRTUAL_ROWS * (COLS / 2 + 1) * 6 * 2,
       WALLROOFCOL = 61, WALLRIGHTCOL = 64, PLATESIDECOL0 = 31,
       PLATEFRONTCOL0 = 16, ARCHINSIDECOL = 65, TUNNELINSIDECOL = 67,
       T_TUNNEL = 1, T_WALL = 2, T_DWALL = 4 };

typedef struct { uint8_t vgaleft, vgaright; } colinfo_t;
static colinfo_t ColInfo[74];

static const uint8_t XLimits9[9] = { 89, 111, 123, 133, 138, 143, 148, 153, 158 };
static uint8_t XLimits[138];

static uint8_t *Page;                 /* work page (framebuffer) */
static int X, Y, Z, ShadowH;
static const uint8_t *CarPtr;
static int Side;
static uint8_t CarMask[(CARH + SHDH) * CARW];
static int CarOfs, ShdOfs;

static const uint16_t *g_index;       /* current phase Index[13][4][6] */
static uint8_t *g_ph;                 /* current phase block */
static const duint *g_cell;           /* current Road_Dat cell */
static const uint16_t *g_tab;         /* 6 index entries for (vrow,col) */

#define RTYPE(w) (((w) >> 8) & 0xf)

/* ---- span guard (build with -DSKY_GUARD) --------------------------------
 * Every span drawelm() writes is data-driven: `base` comes out of the record
 * stream and climbs 320 a row until a 0xff terminator.  So the moment a walk
 * desyncs - REC() landing off the expanded phase block, or a record chain
 * skipped by the wrong number of bytes - `base` grows without bound and the
 * memset runs off the end of the page, and eventually off the arena.  That
 * is a SIGBUS with the whole story already gone from the stack.
 *
 * Three checks: the span stays inside the page; roadrow stays inside
 * Road_Dat; and the expanded phase blocks are both VALID when expand()
 * finishes and UNCHANGED every frame after.  The last two separate the only
 * two ways a record pointer comes out garbage - built wrong, or written
 * over later. */
#ifdef SKY_GUARD
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "compat.h"
uint32_t arena_brk_dbg(void);
static long g_pagelim;                    /* writable bytes from Page */
static int  g_roadrow, g_vrow, g_col, g_elm;
/* DXM runs fullscreen, so stderr is easy to lose behind the app; every
 * report goes to a file as well. */
static FILE *g_lg;
#define GLOG(...) do { if(!g_lg) g_lg=fopen("/tmp/sky_guard.log","w"); \
                       fprintf(stderr,__VA_ARGS__); \
                       if(g_lg) fprintf(g_lg,__VA_ARGS__); } while(0)
#define GEND()    do { fflush(stderr); \
                       if(g_lg){ fflush(g_lg); fclose(g_lg); g_lg=NULL; } \
                       abort(); } while(0)
static uint8_t g_canary[PHASES][INDEXSIZE];
static int     g_canary_set;
static void guard_fail(long off,int w,const uint8_t *rec,int base,int x2,int c){
    GLOG("\n*** SKY_GUARD: span leaves the page ***\n"
      "  write [%ld,%ld) into a page of %ld bytes  (over by %ld)\n"
      "  base=%d x2=%d w=%d colour=%d Side=%d\n"
      "  roadrow=%d vrow=%d col=%d elm=%d\n"
      "  record at +%ld of the phase block (records start at +%d)\n"
      "  Page=+%ld, es=Page+%d, phase block at +%ld, brk=%u\n",
      off,off+w,g_pagelim,off+w-g_pagelim,
      base,x2,w,c,Side,g_roadrow,g_vrow,g_col,g_elm,
      (long)(rec-g_ph),INDEXSIZE,(long)(Page-g_arena),LINE0*320,
      (long)(g_ph-g_arena),(unsigned)arena_brk_dbg());
    GLOG("  g_tab:");
    for (int k=0;k<6;k++) GLOG(" %u",(unsigned)g_tab[k]);
    GLOG("\n  cell=%04x front=%04x side=%04x\n",
         (unsigned)*g_cell,(unsigned)g_cell[-COLS],
         (unsigned)g_cell[Side?-1:+1]);
    GEND();
}
/* called once expand() has run over every phase block */
static void guard_check_expanded(void){
    for (int p = 0; p < PHASES; p++) {
        const uint8_t  *blk = seg_ptr(PicDatSegments[p]);
        const uint16_t *ix  = (const uint16_t *)blk;
        for (int k = 0; k < INDEXSIZE/2; k++)
            if (ix[k] < INDEXSIZE) {
                GLOG("*** SKY_GUARD: phase %d index[%d]=%u is below "
                     "INDEXSIZE %d straight out of expand()\n"
                     "  -> the block was NOT expanded from raw data "
                     "(expanded twice?)\n  block at +%ld, brk=%u\n",
                     p,k,(unsigned)ix[k],INDEXSIZE,
                     (long)(blk-g_arena),(unsigned)arena_brk_dbg());
                GEND();
            }
        memcpy(g_canary[p], blk, INDEXSIZE);
    }
    g_canary_set = 1;
    GLOG("SKY_GUARD: %d phase blocks expanded and snapshotted, brk=%u\n",
         PHASES,(unsigned)arena_brk_dbg());
    if (g_lg) { fflush(g_lg); fclose(g_lg); g_lg=NULL; }
}
/* called every frame, before anything reads the index */
static void guard_check_canary(int ph){
    if (!g_canary_set) return;
    if (memcmp(g_canary[ph], g_ph, INDEXSIZE) == 0) return;
    int k = 0;
    while (k < INDEXSIZE && g_canary[ph][k] == g_ph[k]) k++;
    GLOG("*** SKY_GUARD: phase %d index table CLOBBERED ***\n"
         "  first difference at +%d: was %02x, now %02x\n"
         "  block at +%ld, brk=%u, Page=+%ld\n"
         "  -> something was allocated over the phase blocks\n",
         ph,k,g_canary[ph][k],g_ph[k],
         (long)(g_ph-g_arena),(unsigned)arena_brk_dbg(),
         (long)(Page-g_arena));
    GEND();
}
#  define GUARD_SPAN(off,w,rec,base,x2,c) \
     do { long o_=(long)(LINE0*320)+(long)(off); \
          if(o_<0 || o_+(long)(w)>g_pagelim) \
              guard_fail(o_,(int)(w),(rec),(base),(x2),(c)); } while(0)
#else
#  define GUARD_SPAN(off,w,rec,base,x2,c) ((void)0)
#endif

/* ---- initvid: expand phase blocks + build ColInfo/XLimits ---- */
static void expand(uint8_t *blk) {                 /* trek.asm:1966-2001 */
    uint8_t *src = blk + *(duint *)blk;
    uint8_t *dst = blk;
    memmove(dst, src, INDEXSIZE);
    dst += INDEXSIZE; src += INDEXSIZE;
    for (int n = 0; n < VIRTUAL_ROWS * (COLS / 2 + 1) * ELEMENTS; n++) {
        *dst++ = *src++;                           /* color */
        *dst++ = *src++; *dst++ = *src++;          /* base u16 */
        for (;;) {
            uint8_t x2 = *src++;
            *dst++ = x2;
            if (x2 == 0xff) break;
            *dst++ = *src++;                       /* width */
            *dst++ = 0;                            /* filler */
        }
    }
}

void initvid(void) {
    for (int p = 0; p < PHASES; p++)
        expand(seg_ptr(PicDatSegments[p]));
    for (int c = 0; c < 74; c++) {                 /* §1.3 formulas */
        uint8_t l = (uint8_t)c, r = (uint8_t)c;
        if (c >= 31 && c <= 45) r = (uint8_t)(c + 15);
        else if (c == 63) r = 64;
        else if (c >= 68 && c <= 73) {
            static const uint8_t lt[6] = { 71, 70, 69, 68, 69, 70 };
            static const uint8_t rt[6] = { 70, 69, 68, 69, 70, 71 };
            l = lt[c - 68]; r = rt[c - 68];
        }
        ColInfo[c].vgaleft = l; ColInfo[c].vgaright = r;
    }
    memset(XLimits, 0, sizeof XLimits);
    memcpy(XLimits + 129, XLimits9, 9);
#ifdef SKY_GUARD
    guard_check_expanded();
#endif
}

/* ---- span drawing (vgadrwl/vgadrwr) ---- */
static const uint8_t *drawelm(const uint8_t *rec, int color) {
    uint8_t *es = Page + LINE0 * 320;
    int c = (color >= 0) ? color : rec[0];
    c = Side ? ColInfo[c].vgaright : ColInfo[c].vgaleft;
    int base = rec[1] | (rec[2] << 8);
    rec += 3;
    for (;;) {
        uint8_t x2 = *rec++;
        if (x2 == 0xff) return rec;
        uint8_t w = *rec++; rec++;
        if (w) {
            int off = Side ? (base + x2 - w) : (base - x2);
            GUARD_SPAN(off, w, rec, base, x2, c);
            memset(es + off, c, w);
        }
        base += 320;
    }
}

static const uint8_t *skiprec(const uint8_t *rec) {
    rec += 3;
    while (*rec != 0xff) rec += 3;
    return rec + 1;
}

#define REC(f)  (g_ph + g_tab[f])
#define FT      RTYPE(g_cell[-COLS])
#define ST      RTYPE(g_cell[Side ? -1 : +1])
enum { F_SLAB = 0, F_INSIDE = 1, F_OUTSIDE = 2, F_FRONT = 3, F_TUNNEL = 4, F_DWALL = 5 };

/* ---- element constructors (trek.asm:470-723) ---- */
static void el_plate(void) {
    int C = *g_cell & 0xf;
    if (!C) return;
    const uint8_t *si = drawelm(REC(F_SLAB), C);
    if ((g_cell[Side ? -1 : +1] & 0xf) == 0)
        si = drawelm(si, PLATESIDECOL0 - 1 + C);
    else si = skiprec(si);
    if ((g_cell[-COLS] & 0xf) == 0)
        drawelm(si, PLATEFRONTCOL0 - 1 + C);
}

static void el_tunnel(void) {
    el_plate();
    if (FT < T_TUNNEL) drawelm(REC(F_INSIDE), TUNNELINSIDECOL);
    const uint8_t *si = REC(F_TUNNEL);
    for (int i = 0; i < 6; i++) si = drawelm(si, -1);
    if (FT < T_TUNNEL) { si = drawelm(si, -1); drawelm(si, -1); }
}

static void el_wall(void) {
    el_plate();
    if (FT < T_WALL) drawelm(REC(F_FRONT), -1);
    int B = (*g_cell >> 4) & 0xf;
    const uint8_t *si = drawelm(REC(F_OUTSIDE), B ? B : WALLROOFCOL);
    if (ST < T_WALL) drawelm(si, -1);
}

static void el_arch(void) {
    el_plate();
    if (FT < T_WALL) drawelm(REC(F_INSIDE), ARCHINSIDECOL);
    int B = (*g_cell >> 4) & 0xf;
    const uint8_t *si = drawelm(REC(F_OUTSIDE), B ? B : WALLROOFCOL);
    if (ST < T_WALL) drawelm(si, -1);
    if (FT < T_WALL) {
        const uint8_t *s2 = skiprec(REC(F_FRONT));
        s2 = drawelm(s2, -1);
        drawelm(s2, -1);
    }
}

static void el_dwall(void) {
    el_plate();
    if (FT < T_WALL) drawelm(REC(F_FRONT), -1);
    const uint8_t *si = skiprec(REC(F_OUTSIDE));
    if (ST < T_WALL) drawelm(si, -1);
    int B = (*g_cell >> 4) & 0xf;
    si = drawelm(REC(F_DWALL), B ? B : WALLROOFCOL);
    if (ST < T_DWALL) si = drawelm(si, -1); else si = skiprec(si);
    if (FT < T_DWALL) drawelm(si, -1);
}

static void el_darch(void) {
    el_plate();
    if (FT < T_WALL) drawelm(REC(F_INSIDE), ARCHINSIDECOL);
    const uint8_t *si = skiprec(REC(F_OUTSIDE));
    if (ST < T_WALL) drawelm(si, -1);
    if (FT < T_WALL) {
        const uint8_t *s2 = skiprec(REC(F_FRONT));
        s2 = drawelm(s2, -1);
        drawelm(s2, -1);
    }
    int B = (*g_cell >> 4) & 0xf;
    si = drawelm(REC(F_DWALL), B ? B : WALLROOFCOL);
    if (ST < T_DWALL) si = drawelm(si, -1); else si = skiprec(si);
    if (FT < T_DWALL) drawelm(si, -1);
}

static void (*const ElmDraw[6])(void) =
    { el_plate, el_tunnel, el_wall, el_arch, el_dwall, el_darch };

/* ---- car mask (trek.asm:1080-1165) ---- */
static void carmask(void) {
    memset(CarMask, 0, sizeof CarMask);
    int z = Z, k = MAXZ - Z;
    for (int r = 0; r < CARH + SHDH; r++) {
        if (z >= MINZ && z <= MAXZ) {
            int beg = MINX, end = MAXX + 1, xl = XLimits[k];
            if (xl) {
                int a = MINX + 160 - xl;
                if (X >= a) beg = MINX + 160 + xl; else end = a;
            }
            int lo = beg - X;
            if (lo < 0) lo = 0;
            if (lo < CARW) {
                int cnt = CARW - lo, over = X + CARW - end;
                if (over > 0) cnt -= over;
                if (cnt > 0) memset(&CarMask[r * CARW + lo], 1, cnt);
            }
        }
        z--; k++;
        if (r == CARH - 1) { z -= ShadowH - SHDZDIF; k += ShadowH - SHDZDIF; }
    }
}

/* ---- car + shadow (vgacar/vgashd) ---- */
static void vgacar(void) {
    carmask();
    CarOfs = (MAXZ - Z) * 320 + X - MINX;
    for (int c = 0; c < CARW; c++)                 /* sprite is column-major */
        for (int r = 0; r < CARH; r++) {
            uint8_t p = CarPtr[c * CARH + r];
            if (p && CarMask[r * CARW + c]) {
                CarMask[r * CARW + c] = 2;
                Page[CarOfs + r * 320 + c] = p;
            }
        }
    unsigned n = (unsigned)ShadowH / SHDCONST;
    if (n >= SHADOWS) return;
    const uint8_t *shape = &ShdShapes[n][0][0];
    ShdOfs = (MAXZ - Z + CARH - SHDZDIF + ShadowH) * 320 + X - MINX;
    for (int c = 0; c < CARW; c++)
        for (int r = 0; r < SHDH; r++) {
            int m = CARH * CARW + r * CARW + c;
            if (shape[r * CARW + c] && CarMask[m]) {
                CarMask[m] = 2;
                uint8_t p = Page[ShdOfs + r * 320 + c];
                if (p == WALLROOFCOL) p = WALLRIGHTCOL;
                else if (p >= 1 && p < 16) p = (uint8_t)(p + 45);
                Page[ShdOfs + r * 320 + c] = p;
            }
        }
}

/* ---- video() (trek.asm:339-463), AllScreen model ---- */
void video(int x, int y, int z, const uint8_t *carptr,
           int car_inside_tunnel, int surface_relative_z, duint page_seg) {
    (void)car_inside_tunnel;
    X = x; Y = y; Z = z; CarPtr = carptr; ShadowH = surface_relative_z;
    Page = seg_ptr(page_seg);
#ifdef SKY_GUARD
    /* the caller allocates exactly one viewport: 320 * (MAXZ-MINZ+1) */
    g_pagelim = 320L * (MAXZ - MINZ + 1);
#endif

    /* background restore: full copy of the viewport (sky + road area) */
    memcpy(Page, seg_ptr(Background_Seg), 320 * (MAXZ - MINZ + 1));

    g_ph = seg_ptr(PicDatSegments[Y & (PHASES - 1)]);
#ifdef SKY_GUARD
    guard_check_canary(Y & (PHASES - 1));
#endif
    g_index = (const uint16_t *)g_ph;
    int roadrow = (Y >> PHAS) + (ROWS - GROUNDROWS);
    int Half = 0;

    for (int Rows = ROWS; Rows >= 1; Rows--, roadrow--) {
    again:;
        int vrow = ROWS - Rows;
        if (Rows == GROUNDROWS) vrow = ROWS + Half;
        for (Side = 0; Side < 2; Side++)
            for (int c = 0; c < COLS / 2 + 1; c++) {
                int col = Side ? COLS - 1 - c : c;
                g_cell = &Road_Dat[roadrow][col];
                g_tab = &g_index[(vrow * (COLS / 2 + 1) + c) * 6];
                int t = RTYPE(*g_cell);
#ifdef SKY_GUARD
                g_roadrow = roadrow; g_vrow = vrow; g_col = col; g_elm = t;
                /* Road_Dat has 8 zeroed guard rows below and 24 above; past
                 * those we are reading whatever global follows it, and the
                 * types that come back drive the record walk. */
                if (roadrow < -8 || roadrow >= MAX_STAGE_LEN + 24) {
                    GLOG("*** SKY_GUARD: roadrow %d out of Road_Dat "
                         "[-8,%d)  (Y=%d vrow=%d)\n",
                         roadrow, MAX_STAGE_LEN+24, Y, vrow);
                    GEND();
                }
#endif
                if (t < 6) ElmDraw[t]();
            }
        if (Rows == GROUNDROWS && Half == 0) {
            Half = 1;
            vgacar();
            goto again;
        }
    }
}
