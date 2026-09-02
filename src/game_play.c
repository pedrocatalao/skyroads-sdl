/* game_play.c — portable C port of game.c (the SkyRoads gameplay module).
 *
 * Faithful to the 16-bit original: physics state keeps the DOS types
 * (duint = uint16_t with wraparound, sint = int16_t, slong = int32_t) and the
 * fixed-point layout (x in 65536*slabs, y/z in 128*pixels).
 *
 * Deviations from game.c (all intentional):
 *  - KEYBOARD control only; joystick/mouse/demo-record paths dropped.
 *  - Esc aborts immediately (original had a pause + confirm flow).
 *  - Interrupt-driven busy waits replaced by SDL pump / tick / present.
 *  - EGA branches dropped (VGA path only); switch_pages() is a no-op since
 *    the renderer draws into a work page that we blit into vga_mem().
 *  - Sound effects are no-op stubs (see AUDIO-HOOK), text output is a no-op
 *    stub (see TEXT-HOOK).
 */
#include "assets.h"
#include "render.h"
#include "platform.h"
#include <stdlib.h>
#include <string.h>

typedef uint32_t dulong;    /* the original's 32-bit ulong; 'd' avoids
                               colliding with glibc's ulong on Linux */

enum game_result {NO_CRASH=0,WALL_CRASH,EXPLOSION,HOLE,FUEL_OUT,OXY_OUT,
                  TUNNEL_MISSED,ABORT};
enum control_devices {KEYBOARD=0,JOYSTICK,MOUSE,DEMO};

typedef uint8_t car_t[24*30];           /* public.h: one car frame, col-major */

#define VGA_SCREEN      (vga_mem())

#define CENTER          (256*128)       /* y when car is at screen center     */
#define BEG_X           (3*65536L)      /* x at the beginning of the game     */
#define ROAD_END_LEN    (65536L/2)
#define ROAD_END_TIME   (2*36)          /* ticks to show road after the end   */
#define SHOW_LEVEL_TIME (4*36)
#define GROUND          (80*128)        /* z of car bottom at slab level      */
#define STICKY_SINK_DEPTH (1*128)
#define LEFT_BORDER     (CENTER/128-SLAB_WIDTH*SLABS/2)
#define SLABS           7
#define SLAB_STEP       8
#define SLAB_HEIGHT     7
#define SLAB_WIDTH      46
#define WALL_HEIGHT     20
#define ROOF            (GROUND+WALL_HEIGHT*128)
#define DOUBLE_ROOF     (GROUND+2*WALL_HEIGHT*128)
#define CAR_WIDTH       29
#define CAR_HEIGHT      13

                        /* values in Road_Dat: */
#define BRAKE_SLAB      2
#define SLIPPERY_SLAB   8
#define SUPPLIES_SLAB   9
#define SPEED_SLAB      10
#define FIRE_SLAB       12
#define SLAB_MASK       0x00f
#define TYPE_MASK       0xf00
#define TUNNEL          0x100
#define WALL            0x200
#define ARCH            0x300
#define DOUBLE_WALL     0x400
#define DOUBLE_ARCH     0x500

#define COORD_STEPS     5
#define MAX_JUMP_ADJUST 6

#define MAX_OXY         30000
#define MAX_FUEL        30000
#define JUMP_FUELDEC    0
#define CAR_PHASELEN    2
#define CRASH_PHASELEN  3
#define JUMP_CAR_VZ     (100*128/36)
#define CRASH_TIME      (3*36)
#define EXPLOSION_IMAGES 14
#define TURN_IMAGES     7

#define MAX_SPEED       (6L*65536L/36)
#define SPEED_ACC       (MAX_SPEED/(36*4))
#define WALL_SLOWDOWN   (MAX_SPEED/(36*2))
#define SLAB_SLOWDOWN   (MAX_SPEED/(36*1))
#define SLAB_SPEEDUP    (MAX_SPEED/(36*1))
#define CRASH_SPEED     (MAX_SPEED/3)
#define PUSH_Y          (CAR_WIDTH*128/4)

#define NORM_AY         29
#define STANDING_VX     (MAX_SPEED/7)
#define SINK_ACC        (5*128/36)
#define STEER_ZONE_HEIGHT 30

#define JUMP_HEIGHT     45
#define JUMP_TIME       20
#define FALLDOWN_VZ     (-30*128/36)
#define EXPLOSION_Z_ACC (400L*128/36/36)
#define MAX_EXPLOSION_VZ (20*128/36)
#define BOUNCE_FACTOR   5
#define MIN_BOUNCE_VZ   260
#define MIN_BOUNCE_SINK_DIST 2
#define MAX_GRAVITY     20

#define DIST_DIVISIONS  29
#define DIST_DISPLAY_X  42
#define DIST_DISPLAY_Y  143

#define FUELOUT_BLINK_PERIOD 9
#define SOUND_FX_LEN    (8)

#define lenof(a)        (sizeof(a)/sizeof((a)[0]))
#define dmin(a,b)       ((a)<(b)?(a):(b))
#define dmax(a,b)       ((a)>(b)?(a):(b))
#define bound(a,mi,ma)  do { if ((a)<(mi)) (a)=(mi); \
                             else if ((a)>(ma)) (a)=(ma); } while (0)

/* Game_Time == Time in the retail build (#define Game_Time Time).  The
 * original zeroes it; our Time is derived from wall time, so Game_Time is
 * Time plus a settable bias with the same 16-bit wrap behaviour. */
static duint gt_bias;
#define Game_Time  ((duint)(Time + gt_bias))
static void gt_set(duint v) { gt_bias = (duint)(v - Time); }

/* ---------------------- game state (game.c globals) ---------------------- */

                /* current coordinates */
dulong x;        /*   65536*slabs   */
duint y, z;     /*   128*pixels    */
                /* current speed:  */
slong vx;       /*   65536*slabs/volatile tick */
sint ay;        /*   pixels/slab               */
sint sink_vy;   /*   128*pixels/volatile tick  */
sint vz;        /*   128*pixels/volatile tick  */
slong vx_adjust;/* vx added by adjust_jump()   */

duint current_jump_adjusted;

duint fuel, oxy;
duint crash_type;
duint crash_time;
duint burning_time;

sint z_acc;

static duint cur_video_page;
static duint redraw_all_screen;

static duint e_speed,e_oxy_disp,e_fuel_disp,e_distance,e_adjusting,
             e_fuelout_blink_on;

duint adjust_jumps = 1;                 /* sky2.c sets this before game()    */
duint control_device = KEYBOARD;

static sint ver_control_status, hor_control_status;
static duint jump_control_status;

static const sint car_hotspot[TURN_IMAGES]={-1,-1,-1,0,+1,+2,+4};

static const duint max_tunnel_z[SLAB_WIDTH/2+CAR_WIDTH/2+1]={
                        16,16,16,16,15,14,13,11, 8, 7,
                         6, 5, 3, 3, 3, 3, 3, 3, 2, 1,
                         0, 0, 0, 0, 0, 0, 1, 2, 3, 3,
                         3, 3, 3, 3, 5, 6, 7, 8
                        };

static const duint min_tunnel_z[SLAB_WIDTH/2+CAR_WIDTH/2+1]={
                        32,32,32,32,32,32,32,32,32,32,
                        32,32,32,32,32,32,32,31,31,31,
                        31,31,30,30,30,29,29,29,28,27,
                        26,25,24,22,20,18,17,14
                        };

static const duint block_roof[]={GROUND,ROOF,ROOF,ROOF,DOUBLE_ROOF,DOUBLE_ROOF};

static const duint car_ani_sequence[]={0,1,2,1};

static duint sound_fx_time;

static duint show_level_only = 0;       /* retail flow never sets this */
static dulong show_level_timer;

static duint tmpheap_seg;               /* scratch segment for gauge blits */
static duint vga_buf_seg;               /* 320*DASHBOARD_Y work page       */
static const car_t *Cars;               /* frames inside Cars_Seg          */

static const uint8_t digits_display_dat[][4*5]={
                            {
                            0,0,0,0,    /* 0 */
                            0,2,2,0,
                            0,1,1,0,
                            0,2,2,0,
                            0,0,0,0,
                            },
                            {
                            1,1,1,0,    /* 1 */
                            1,2,2,0,
                            1,1,1,0,
                            1,2,2,0,
                            1,1,1,0
                            },
                            {
                            0,0,0,0,    /* 2 */
                            1,2,2,0,
                            0,0,0,0,
                            0,2,2,1,
                            0,0,0,0
                            },
                            {
                            0,0,0,0,    /* 3 */
                            1,2,2,0,
                            0,0,0,0,
                            1,2,2,0,
                            0,0,0,0
                            },
                            {
                            0,1,1,0,    /* 4 */
                            0,2,2,0,
                            0,0,0,0,
                            1,2,2,0,
                            1,1,1,0
                            },
                            {
                            0,0,0,0,    /* 5 */
                            0,2,2,1,
                            0,0,0,0,
                            1,2,2,0,
                            0,0,0,0
                            },
                            {
                            0,1,1,1,    /* 6 */
                            0,2,2,1,
                            0,0,0,0,
                            0,2,2,0,
                            0,0,0,0
                            },
                            {
                            0,0,0,0,    /* 7 */
                            1,2,2,0,
                            1,1,1,0,
                            1,2,2,0,
                            1,1,1,0
                            },
                            {
                            0,0,0,0,    /* 8 */
                            0,2,2,0,
                            0,0,0,0,
                            0,2,2,0,
                            0,0,0,0
                            },
                            {
                            0,0,0,0,    /* 9 */
                            0,2,2,0,
                            0,0,0,0,
                            1,2,2,0,
                            1,1,1,0
                            }
                            };

static const uint8_t adjust_display_dat[][26*5]={
                            {
#define O 0
#define _ 5
#define m 6
                            _, m, _,_,_,O, m, O,O,O,_, m, O,_,_,_, m, O,O,O,O, m, _,_,_,_,
                            _, m, _,_,_,O, m, O,m,m,O, m, O,m,m,_, m, O,m,m,_, m, _,m,m,_,
                            _, m, _,_,_,O, m, O,_,_,O, m, O,_,_,_, m, O,O,O,_, m, _,_,_,_,
                            _, m, _,_,_,O, m, O,m,m,O, m, O,m,m,_, m, O,m,m,_, m, _,m,m,_,
                            _, m, _,_,_,O, m, O,O,O,_, m, O,O,O,O, m, O,O,O,O, m, _,_,_,_
                            },
                            {
                            O, m, O,O,O,_, m, _,_,_,_, m, O,_,_,O, m, O,O,O,O, m, O,O,O,O,
                            O, m, O,m,m,O, m, _,m,m,_, m, O,m,m,O, m, O,m,m,_, m, O,m,m,_,
                            O, m, O,_,_,O, m, _,_,_,_, m, O,_,_,O, m, O,O,O,O, m, O,O,O,_,
                            O, m, O,m,m,O, m, _,m,m,_, m, O,m,m,O, m, _,m,m,O, m, O,m,m,_,
                            O, m, O,_,_,O, m, _,_,_,_, m, O,O,O,O, m, O,O,O,O, m, O,O,O,O
#undef O
#undef _
#undef m
                            }
                            };

/*************************** SDL frame plumbing *****************************/

static void frame_idle(void) {          /* replaces the PIT interrupt */
    if (!plat_pump()) plat_exit(0);
    plat_tick_update();
    plat_present();
}

static void wait_for_time_tick(void) {
    duint e_Time = Time;
    do {
        frame_idle();
        plat_sleep(2);
    } while (Time == e_Time);
}

/*************************** Functions **************************************/

/* AUDIO-HOOK: the original programs the SB DMA (sbdma/sbstop with samples
 * from Sample_Seg) or the PC speaker (Sound_FX_Ptr + the sfx tables).  Wire
 * sample playback here later; timing side effects are preserved. */
enum sfx {EXPLOSION_SFX=0,BOUNCE_SFX,PUSH_SFX,FUELOUT_SFX,SUPPLIES_SFX};

void sbdma(const uint8_t *buf, uint32_t len, duint smprate);
void sbstop(void);

static void gen_sound_fx(enum sfx nr)
{
    sound_fx_time=Time;
    if (cfg.silence) return;
    /* game.c gen_sound_fx: Sample_Seg = sfx.snd, u16 index[6] header of
     * offsets; sample = [SB time-constant byte][8-bit unsigned PCM] */
    const duint *p = (const duint *)seg_ptr(Sample_Seg);
    duint off = p[nr], len = (duint)(p[nr+1] - p[nr]);
    const uint8_t *data = seg_ptr(Sample_Seg) + off;
    sbstop();
    sbdma(data + 1, len - 1, data[0]);
}

static duint sound_fx_playing(void)
{
    return (duint)(Time - sound_fx_time) < SOUND_FX_LEN;    /* AUDIO-HOOK */
}

#include "font8.h"

/* Original blits BIOS 8x8 font glyphs into VGA_SCREEN; we use an
 * embedded font (see font8.h) with a 1px drop shadow for readability. */
static void text(duint tx,duint ty,const char *s,duint col)
{
    uint8_t *vram = vga_mem();
    for (; *s; s++, tx += 8) {
        const uint8_t *g = font8_glyph(*s);
        if (!g) continue;
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c)) {
                    vram[(ty + r + 1) * 320 + tx + c + 1] = 0;
                    vram[(ty + r) * 320 + tx + c] = (uint8_t)col;
                }
    }
}

static duint get_slab(dulong x,duint y)
{
    y=(duint)(y/128-LEFT_BORDER);
    if (y < SLABS*SLAB_WIDTH) {         /* this is not true with y<LEFT_BORDER either */
        dulong row=(x/(65536L/SLAB_STEP))/SLAB_STEP;
        if (row >= MAX_STAGE_LEN+8)     /* off the stored road (portable safety) */
            return 0;
        return Road_Dat[row][y/SLAB_WIDTH];
    }
    else
        return 0;
}

static duint car_inside_tunnel_(dulong x,duint y,duint z)
{   duint slab_type,a_deviation;

    slab_type=get_slab(x,y)&TYPE_MASK;
    if (slab_type==TUNNEL || slab_type==ARCH || slab_type==DOUBLE_ARCH) {
        a_deviation=(duint)((SLAB_WIDTH/2)-
                    (duint)((duint)(y/128-LEFT_BORDER+SLAB_WIDTH)%SLAB_WIDTH));
        if (a_deviation > 0x7fff || !a_deviation)
            a_deviation=(duint)(1-a_deviation);
        if (a_deviation <= SLAB_WIDTH/2+CAR_WIDTH/2)
            return (duint)((duint)(z+(CAR_HEIGHT-1)*128-GROUND)/128) <
                (min_tunnel_z[a_deviation]+max_tunnel_z[a_deviation])/2;
    }
    return 0;
}

static void get_controls(void)          /* evaluates *_control_status */
{
    unsigned k=plat_keys();             /* KEYBOARD only */
    hor_control_status=(sint)(!!(k&K_RIGHT)-!!(k&K_LEFT));
    ver_control_status=(sint)(!!(k&K_UP)-!!(k&K_DOWN));
    jump_control_status=!!(k&K_SPACE);
}

static duint jump_car(sint vz,duint z)  /* decides which car image to display */
{
    if (vz <= -JUMP_CAR_VZ || z < GROUND)
        return 2;
    if (vz >= JUMP_CAR_VZ)
        return 1;
    return 0;
}

static duint turn_car(duint y)          /* returns 0..TURN_IMAGES-1 */
{   sint yy;

    yy=(sint)(duint)(y/128-LEFT_BORDER)/(SLABS*SLAB_WIDTH/TURN_IMAGES);
    bound(yy,0,TURN_IMAGES-1);
    return (duint)yy;
}

static duint surface_z(dulong x,duint y,duint inside_tunnel)
{   duint slab,slab_type;               /* height of the world at given x,y */

    slab=get_slab(x,y);
    slab_type=slab&TYPE_MASK;

    if (slab_type && !inside_tunnel) {
        if (slab_type != TUNNEL)
            return block_roof[slab_type>>8];
        else
            return 0;
    }
    else {
        if (slab&SLAB_MASK)
            return GROUND;
        else
            return 0;
    }
}

static void display(duint sticky)       /* displays one frame */
{   static dulong ex;
    static duint ey,ez,ecar_nr,evid_page;
    duint car_nr;
    duint inside_tunnel,left_surface_z,right_surface_z;

    inside_tunnel=car_inside_tunnel_(x,y,z);

    if (burning_time) {
        car_nr=(duint)(burning_time/CRASH_PHASELEN);
        if (car_nr >= EXPLOSION_IMAGES)
            car_nr=0xffff;
    }
    else
        car_nr=(duint)(EXPLOSION_IMAGES+
                3*(turn_car(y)*3+(inside_tunnel?0:jump_car(vz,z)))+
                (crash_type==FUEL_OUT?0:car_ani_sequence[(Time/CAR_PHASELEN)%
                lenof(car_ani_sequence)]));

    left_surface_z= surface_z(x,(duint)(y-CAR_WIDTH/4*128),inside_tunnel);
    right_surface_z=surface_z(x,(duint)(y+CAR_WIDTH/4*128),inside_tunnel);

    if (ex != x || ey != y || ez != z || ecar_nr != car_nr ||
                evid_page != cur_video_page) {
        ex=x;
        ey=y;
        ez=sticky?(duint)(z-STICKY_SINK_DEPTH):z;
        ecar_nr=car_nr;
        evid_page=cur_video_page;
        video(  (int)(y/128)+car_hotspot[turn_car(y)],
                (int)(x/(65536L/SLAB_STEP)),
                (car_nr != 0xffff)?(int)(ez/128):0,
                /* Cars[0xffff] was a wild far pointer in DOS; the mask is
                 * empty then anyway, so pass frame 0 to stay in bounds */
                Cars[(car_nr != 0xffff)?car_nr:0],
                (int)redraw_all_screen,
                burning_time?0x7fff:
                    (int)(duint)((duint)(z-dmax(left_surface_z,right_surface_z))/128),
                vga_buf_seg);
        cur_video_page^=1;
        redraw_all_screen=0;
        /* VGA path of trek.asm copies the work page to the screen itself;
         * our renderer draws into the page only, so blit it here. */
        memcpy(VGA_SCREEN,seg_ptr(vga_buf_seg),320*DASHBOARD_Y);
    }
}

static duint display_road_end(void)
{   duint t0;

    z=0;
    t0=Time;                            /* original: Time=0 */
    while ((duint)(Time-t0) < ROAD_END_TIME) {
        if (plat_keys()&K_ESC)
            return ABORT;
        display(0);
        wait_for_time_tick();
        x+=(dulong)vx;
    }
    return NO_CRASH;
}

static void xlat_to_tmpheap(const uint8_t *p,duint len,duint col1,duint col2)
{   uint8_t *dst=seg_ptr(tmpheap_seg);
    duint i;

    for (i=0;i < len;i++) {
        uint8_t al=p[i];
        dst[i]=(al==0)?0:(al==1)?(uint8_t)col1:(uint8_t)col2;
    }
}

static void fill_segment_division(const uint8_t *p,duint on)
{   duint col1,col2,len;
    pic_t pic;

    /* seg_hdr_t: { duint offs; uchar x_size; uchar y_size; } */
    col1=on?DASHBOARD_COLOR+2:DASHBOARD_COLOR+0;
    col2=on?DASHBOARD_COLOR+3:DASHBOARD_COLOR+1;
    pic.seg=tmpheap_seg;
    pic.addr=(duint)(p[0]|(p[1]<<8));
    pic.len=p[2];
    pic.lines=p[3];
    len=(duint)(pic.len*pic.lines);
    xlat_to_tmpheap(p+4,len,col1,col2);
    init_mix(0,SEG_VGA,0,1);
    mix_picture(&pic);
}

static void display_rectangle(duint x,duint y,duint x_len,duint y_len,
                              const uint8_t *p)
{   duint col1,col2;
    pic_t pic;

    col1=DASHBOARD_COLOR+5;
    col2=DASHBOARD_COLOR+6;
    xlat_to_tmpheap(p,(duint)(x_len*y_len),col1,col2);

    pic.seg=tmpheap_seg;
    pic.addr=(duint)(x+320*y);
    pic.len=x_len;
    pic.lines=y_len;
    init_mix(0,SEG_VGA,0,0);
    mix_picture(&pic);
}

static void display_number(duint x,duint y,duint n,duint digits)
{   duint i,val;
    static const duint pow10[]={1,10,100,1000,10000};

    for (i=0;i < digits && (n || !i);i++) {
        val=(n/pow10[i])%10;
        display_rectangle((duint)(x+(digits-i-1)*5),y,4,5,
                          digits_display_dat[val]);
        n=(duint)(n-val*pow10[i]);
    }
}

static duint get_pixel(duint x,duint y)
{
    return VGA_SCREEN[(duint)(x+320*y)];    /* 16-bit offset wrap, as in DOS */
}

static void put_pixel(duint x,duint y,duint col)
{
    VGA_SCREEN[(duint)(x+320*y)]=(uint8_t)col;
}

static void fill_dist_division(duint nr)
{   duint x,y;
    duint cur_col;

    x=(duint)(DIST_DISPLAY_X+nr);
    y=DIST_DISPLAY_Y;
    cur_col=get_pixel(x,y);
    while (get_pixel(x,y) == cur_col)
        y--;
    while (get_pixel(x,++y) == cur_col)
        put_pixel(x,y,DASHBOARD_COLOR+4);
}

static void exchange_colors(duint x,duint y,duint x_len,duint y_len,
                            duint col1,duint col2)
{   duint i,j;
    duint c;

    for (i=x;i < x+x_len;i++)
        for (j=y;j < y+y_len;j++) {
            c=get_pixel(i,j);
            if (c == col1)
                put_pixel(i,j,col2);
            else
                if (c == col2)
                    put_pixel(i,j,col1);
        }
}

static void display_controls(void)
{   duint i,fuelout_blink_on;
    duint speed,oxy_disp,fuel_disp,distance;

    fuelout_blink_on=(Time%FUELOUT_BLINK_PERIOD > FUELOUT_BLINK_PERIOD/2);

    /* display speed */

    speed=(duint)((vx-vx_adjust)/(MAX_SPEED/SPEED_DIVISIONS));
    if (speed > SPEED_DIVISIONS)
        speed=SPEED_DIVISIONS;
    for (i=dmin(speed,e_speed);i < dmax(speed,e_speed);i++)
        fill_segment_division(speed_display_dat+speed_display_offset[i],
                (speed>e_speed));
    e_speed=speed;

    /* display oxy */

    oxy_disp=(duint)((oxy+MAX_OXY/OXY_DIVISIONS-1)/(MAX_OXY/OXY_DIVISIONS));
    if (oxy_disp > OXY_DIVISIONS)
        oxy_disp=OXY_DIVISIONS;
    for (i=dmin(oxy_disp,e_oxy_disp);i < dmax(oxy_disp,e_oxy_disp);i++)
        fill_segment_division(oxy_display_dat+oxy_display_offset[i],
                (oxy_disp>e_oxy_disp));
    e_oxy_disp=oxy_disp;
    if (crash_type == OXY_OUT && fuelout_blink_on != e_fuelout_blink_on) {
        exchange_colors(160,161,7,7,DASHBOARD_COLOR+7,DASHBOARD_COLOR+8);
        if (fuelout_blink_on)
            gen_sound_fx(FUELOUT_SFX);
    }

    /* display fuel */

    fuel_disp=(duint)((fuel+MAX_FUEL/FUEL_DIVISIONS-1)/(MAX_FUEL/FUEL_DIVISIONS));
    if (fuel_disp > FUEL_DIVISIONS)
        fuel_disp=FUEL_DIVISIONS;
    for (i=dmin(fuel_disp,e_fuel_disp);i < dmax(fuel_disp,e_fuel_disp);i++)
        fill_segment_division(fuel_display_dat+fuel_display_offset[i],
                (fuel_disp>e_fuel_disp));
    e_fuel_disp=fuel_disp;
    if (crash_type == FUEL_OUT && fuelout_blink_on != e_fuelout_blink_on) {
        exchange_colors(155,169,16,5,DASHBOARD_COLOR+7,DASHBOARD_COLOR+8);
        if (fuelout_blink_on)
            gen_sound_fx(FUELOUT_SFX);
    }

    /* display distance */

    distance=(duint)((x-BEG_X)/(((dulong)road_len*65536UL-BEG_X)/
                                                    (DIST_DIVISIONS+1)));
    if (distance > DIST_DIVISIONS)
        distance=DIST_DIVISIONS;
    for (i=e_distance;i < distance;i++)
        fill_dist_division(i);
    e_distance=distance;

    if (e_adjusting != current_jump_adjusted)
        display_rectangle(203,156,26,5,
                                adjust_display_dat[current_jump_adjusted]);
    e_adjusting=current_jump_adjusted;

    e_fuelout_blink_on=fuelout_blink_on;
}

static duint car_inside_building(duint slab,duint y_deviation,duint z)
{   duint relative_z;

    if (y_deviation > SLAB_WIDTH/2+CAR_WIDTH/2)
        return 0;
    relative_z=(duint)((duint)(z+(CAR_HEIGHT-1)*128-GROUND)/128);
    switch (slab&TYPE_MASK) {
        case WALL       :return (z < ROOF);
        case DOUBLE_WALL:return (z < DOUBLE_ROOF);
        case ARCH       :return (z < ROOF &&
                            relative_z >= max_tunnel_z[y_deviation]);
        case DOUBLE_ARCH:return (z < DOUBLE_ROOF &&
                            relative_z >= max_tunnel_z[y_deviation]);
        case TUNNEL     :return (relative_z < min_tunnel_z[y_deviation] &&
                            relative_z >= max_tunnel_z[y_deviation]);
    }
    return 0;   /* original falls off the switch (undefined AX) */
}

static duint car_inside_something(dulong x,duint y,duint z)
{   duint slab,l_slab,r_slab;
    duint a_deviation,b_offset;

    r_slab=get_slab(x,(duint)(y+CAR_WIDTH/2*128));
    l_slab=get_slab(x,(duint)(y-CAR_WIDTH/2*128));

    /* inside a slab */
    if ((r_slab&SLAB_MASK || l_slab&SLAB_MASK) &&
                z < GROUND &&
                (duint)(z+(CAR_HEIGHT-1)*128) > GROUND-SLAB_HEIGHT*128)
        return 1;

    if ((duint)(z+CAR_HEIGHT*128) > GROUND &&
                (l_slab&TYPE_MASK || r_slab&TYPE_MASK)) {
        slab=get_slab(x,y);
        a_deviation=(duint)((SLAB_WIDTH/2)-
                    (duint)((duint)(y/128-LEFT_BORDER+SLAB_WIDTH)%SLAB_WIDTH));
        b_offset=(duint)(-SLAB_WIDTH*128);
        if (a_deviation > 0x7fff || !a_deviation) {
            a_deviation=(duint)(1-a_deviation);
            b_offset=(duint)(-(sint)b_offset);
        }
        if (car_inside_building(slab,a_deviation,z) ||
                car_inside_building(get_slab(x,(duint)(y+b_offset)),
                                    (duint)(SLAB_WIDTH+1-a_deviation),z))
            return 1;
    }
    return 0;
}

static void change_coordinates(dulong target_x,duint target_y,duint target_z)
{   duint i;
    sint step;
    dulong lstep;

    if (x == target_x && y == target_y && z == target_z)
        return;
    for (i=1;i <= COORD_STEPS;i++)
        if (car_inside_something(x+(dulong)((slong)((target_x-x)*i)/COORD_STEPS),
                    (duint)(y+(sint)(duint)((duint)(target_y-y)*i)/COORD_STEPS),
                    (duint)(z+(sint)(duint)((duint)(target_z-z)*i)/COORD_STEPS)
                    ))
            break;
    x+=(dulong)((slong)((target_x-x)*(i-1))/COORD_STEPS);
    y=(duint)(y+(sint)(duint)((duint)(target_y-y)*(i-1))/COORD_STEPS);
    z=(duint)(z+(sint)(duint)((duint)(target_z-z)*(i-1))/COORD_STEPS);

    for (lstep=4096L;lstep;lstep/=16)
        for (;target_x-x>=lstep && !car_inside_something(x+lstep,y,z);x+=lstep)
            ;
    for (step=(target_y > y)?125:-125;step;step/=5)
        for (;abs((sint)(duint)(target_y-y))>=abs(step) &&
                !car_inside_something(x,(duint)(y+step),z);y=(duint)(y+step))
            ;
    for (step=(target_z > z)?125:-125;step;step/=5)
        for (;abs((sint)(duint)(target_z-z))>=abs(step) &&
                !car_inside_something(x,y,(duint)(z+step));z=(duint)(z+step))
            ;
}

static void process_slab(duint slab)
{
    switch (slab&SLAB_MASK) {
        case FIRE_SLAB  :if (crash_type == NO_CRASH)
                            crash_type=EXPLOSION;
                        if (!burning_time) {
                            burning_time=1;
                            gen_sound_fx(EXPLOSION_SFX);
                        }
                        break;
        case SUPPLIES_SLAB:
                        if (crash_type == NO_CRASH) {
                            if (fuel<9*(MAX_FUEL/10) || oxy<9*(MAX_OXY/10))
                                gen_sound_fx(SUPPLIES_SFX);
                            fuel=MAX_FUEL;
                            oxy=MAX_OXY;
                        }
                        break;
        case BRAKE_SLAB:if (!burning_time)
                            vx-=SLAB_SLOWDOWN;
                        break;
        case SPEED_SLAB:if (!burning_time)
                            vx+=SLAB_SPEEDUP;
                        break;
    }
    bound(vx,0,MAX_SPEED);
}

static duint slab_bad(dulong x,duint y)
{   duint slab,slab_type;

    slab=get_slab(x,y);
    slab_type=slab&TYPE_MASK;
    if (slab_type) {
        if (slab_type == TUNNEL)
            return 0;
        else
            slab>>=4;
    }
    slab&=SLAB_MASK;
    if ((!slab && !slab_type) || slab == FIRE_SLAB)
        return 1;
    else
        return 0;
}

static duint simulate_jump(dulong x,duint y,duint z,slong vx,sint ay,sint vz)
{   duint e_y;
    dulong e_x;                          /* returns 1 if jump successful */

    do {
        e_y=y;
        e_x=x;
        vz=(sint)(vz+z_acc);
        x+=(dulong)vx;
        y=(duint)(y+(duint)((vx+STANDING_VX)*ay/(65536L/128))+(duint)sink_vy);
        if (y < LEFT_BORDER*128 || y > (LEFT_BORDER+SLABS*SLAB_WIDTH)*128)
            return 0;
        z=(duint)(z+(duint)vz);
        vx+=ver_control_status*SPEED_ACC;
        bound(vx,0,MAX_SPEED);
    } while (z > GROUND);
    if (slab_bad(e_x,e_y) || slab_bad(x,y))
        return 0;
    else
        return 1;
}

static void adjust_jump(void)
{   slong orig_vx;
    sint orig_ay,i;

    if (simulate_jump(x,y,z,vx,ay,vz))
        return;
    orig_vx=vx;
    orig_ay=ay;
    for (i=1;i <= MAX_JUMP_ADJUST;i++) {
        if (simulate_jump(x,y,z,vx,ay=(sint)(orig_ay+(orig_ay*i)/10),vz))
            break;
        if (simulate_jump(x,y,z,vx,ay=(sint)(orig_ay-(orig_ay*i)/10),vz))
            break;
        ay=orig_ay;

        vx=orig_vx+(orig_vx*i)/10;
        if (vx < MAX_SPEED)
            if (simulate_jump(x,y,z,vx,ay,vz))
                break;
        if (simulate_jump(x,y,z,vx=orig_vx-(orig_vx*i)/10,ay,vz))
            break;
        vx=orig_vx;
    }
    vx_adjust=vx-orig_vx;

    if (i <= MAX_JUMP_ADJUST)
        current_jump_adjusted=1;
}

static duint game_body(void)
{   duint computed_time,e_Game_Time;
    duint jump_adjusted,jumping,jump_z,on_ground,slippery,sticky,hole,slab,i;
    duint sink_distance;
    duint target_y,target_z;
    dulong target_x;
    sint sink_direction;

    z_acc=-(sint)(JUMP_HEIGHT*128*(dulong)gravity/(JUMP_TIME*JUMP_TIME));

    x=BEG_X;
    y=CENTER;
    z=GROUND;
    vx=vz=ay=sink_vy=0;

    fuel=MAX_FUEL;
    oxy=MAX_OXY;

    crash_type=NO_CRASH;
    crash_time=0;
    burning_time=0;

    on_ground=1;
    slippery=sticky=0;
    vx_adjust=jumping=jump_adjusted=current_jump_adjusted=0;
    jump_z=0;
    sink_distance=0;                    /* original reads it uninitialised */
    target_x=x; target_y=y; target_z=z; /* original reads target_z before   */
                                        /*  first assignment (stack junk)   */
    show_level_timer=computed_time=0;
    gt_set(0);
    sound_fx_time=0;
    redraw_all_screen=0;
    while (1) {
        if (Game_Time < computed_time) {
            computed_time=0;
            gt_set(0);
        }
        e_Game_Time=Game_Time;

        if (plat_keys()&K_ESC)          /* original: pause/confirm flow */
            return ABORT;

        if ((!burning_time || burning_time >
                    EXPLOSION_IMAGES*CRASH_PHASELEN) &&
                    (crash_type == WALL_CRASH || crash_type == EXPLOSION ||
                    (crash_type == HOLE && burning_time) ||
                    crash_time >= CRASH_TIME)) {
            return crash_type;
        }

        display(sticky);
        display_controls();
        for (;;) {                      /* while (Game_Time == e_Game_Time); */
            frame_idle();
            if (Game_Time != e_Game_Time)
                break;
            plat_sleep(2);
        }

        if ((plat_getch() | 0x20) == 'p') {     /* P: pause/unpause */
            for (;;) {
                plat_osd("PAUSE");
                if (!plat_pump()) plat_exit(0);
                plat_present();
                int c = plat_getch();
                if ((c | 0x20) == 'p')
                    break;
                plat_sleep(10);
            }
            plat_osd("");
            gt_set(computed_time);      /* no physics catch-up on resume */
        }

        if (!show_level_only)
            get_controls();
        else
            ver_control_status=hor_control_status=(sint)(jump_control_status=0);

        for (;computed_time < Game_Time;computed_time++) {
            if (show_level_only)
                if (show_level_timer++ >= SHOW_LEVEL_TIME &&
                                            !crash_type && !burning_time) {
                    crash_type=EXPLOSION;
                    burning_time=1;
                    gen_sound_fx(EXPLOSION_SFX);
                }

/*************************** Process current slab ***************************/

            slab=get_slab(x,y);
            hole=!slab;
            if (on_ground) {
                if (z > GROUND) {
                    if (z == block_roof[slab>>8])
                        slab>>=4;
                    else
                        slab=0;
                }
                process_slab(slab);
                slippery=((slab&SLAB_MASK) == SLIPPERY_SLAB);
                sticky=((slab&SLAB_MASK) == BRAKE_SLAB);
            }
            else
                sticky=0;
            if (x >= ((dulong)road_len*65536UL-ROAD_END_LEN) &&
                        car_inside_tunnel_(x,y,z) && crash_type == NO_CRASH) {
                return display_road_end();
            }

            if (z != target_z) {
                if ((!sink_vy || sink_distance >= MIN_BOUNCE_SINK_DIST) &&
                                    abs(vz) >= MIN_BOUNCE_VZ*gravity/8 &&
                                    !burning_time) {
                    if (crash_type == NO_CRASH && vz < 0 &&
                                                        !sound_fx_playing())
                        gen_sound_fx(BOUNCE_SFX);
                    vz=(sint)(-(BOUNCE_FACTOR*vz)/10);
                }
                else
                    vz=0;
            }

/**************************** Change vx, ay, vz *****************************/

            if (crash_type == NO_CRASH) {
                vx+=ver_control_status*SPEED_ACC;
                bound(vx,0,MAX_SPEED);
                if (!slippery)
                    if ((!jumping && !hole) ||
                        (!ay && vz > 0 &&
                                (duint)(z-jump_z) < STEER_ZONE_HEIGHT*128)) {
                        ay=(sint)(NORM_AY*hor_control_status);
                    }
                if (!jumping && !hole && jump_control_status &&
                                                    gravity < MAX_GRAVITY) {
                    vz=4*JUMP_HEIGHT*128/JUMP_TIME;
                    fuel-=JUMP_FUELDEC;
                    jumping=1;
                    jump_z=z;
                }
            }
            if (adjust_jumps && jumping && !jump_adjusted &&
                                        z >= GROUND+STEER_ZONE_HEIGHT*128) {
                adjust_jump();          /* evaluates vx_adjust */
                jump_adjusted=1;
            }
            if (!burning_time) {
                if (z >= GROUND)
                    vz=(sint)(vz+z_acc);
                else
                    if (vz > FALLDOWN_VZ)
                        vz=FALLDOWN_VZ;
            }
            else {
                if (vz < 0)
                    vz=0;
                if (vz < MAX_EXPLOSION_VZ)
                    vz=(sint)(vz+EXPLOSION_Z_ACC);
                else
                    vz=MAX_EXPLOSION_VZ;
            }

/***************** Change x, y, z; react to obstacles ***********************/

            target_x=x+(dulong)vx;
            target_y=(duint)(y+(duint)((vx+(sticky?0:STANDING_VX))*ay/
                                        (65536L/128))+(duint)sink_vy);
            target_z=(duint)(z+(duint)vz);
            if ((y < LEFT_BORDER*128 &&
                        target_y > LEFT_BORDER*128+SLABS*SLAB_WIDTH*128) ||
                        (target_y < LEFT_BORDER*128 &&
                        y > LEFT_BORDER*128+SLABS*SLAB_WIDTH*128))
                target_y=y;
            change_coordinates(target_x,target_y,target_z);
            if (x != target_x && y == target_y &&
                                    car_inside_something(target_x,y,z)) {
                if (!car_inside_something(target_x,(duint)(y-PUSH_Y),z)) {
                    y=(duint)(y-PUSH_Y);
                    target_x=x;
                    gen_sound_fx(PUSH_SFX);
                }
                else
                    if (!car_inside_something(target_x,(duint)(y+PUSH_Y),z)) {
                        y=(duint)(y+PUSH_Y);
                        target_x=x;
                        gen_sound_fx(PUSH_SFX);
                    }
            }
            if (x != target_x) {
                if (vx >= CRASH_SPEED && !burning_time) {
                    burning_time=1;
                    gen_sound_fx(EXPLOSION_SFX);
                    if (crash_type == NO_CRASH)
                        crash_type=WALL_CRASH;
                }
                else
                    if (x > target_x-(dulong)vx)
                        gen_sound_fx(PUSH_SFX);
                vx=0;
            }
            if (y != target_y) {
                ay=0;
                if ((sink_vy>0 && target_y>y) || (sink_vy<0 && target_y<y))
                    sink_vy=0;
                vx-=WALL_SLOWDOWN;
                bound(vx,0,MAX_SPEED);
            }
            on_ground=0;
            if (z != target_z) {
                if (vz < 0) {
                    jumping=jump_adjusted=current_jump_adjusted=0;
                    on_ground=1;
                    vx-=vx_adjust;
                    bound(vx,0,MAX_SPEED);
                    vx_adjust=0;

                    sink_direction=0;
                    for (i=1;i <= CAR_WIDTH/2;i++)
                        if (!car_inside_something(x,(duint)(y+i*128),
                                                    (duint)(z-1))) {
                            sink_direction++;
                            sink_distance=i;
                            break;
                        }
                    for (i=1;i <= CAR_WIDTH/2;i++)
                        if (!car_inside_something(x,(duint)(y-i*128),
                                                    (duint)(z-1))) {
                            sink_direction--;
                            sink_distance=i;
                            break;
                        }
                    if (sink_direction)
                        sink_vy=(sint)(sink_vy+sink_direction*SINK_ACC);
                    else
                        sink_vy=0;
                }
            }
            if (z > 0x7fff)
                z=0;

/******************* Decrement oxy and fuel *********************************/

            if (crash_type == NO_CRASH) {
                oxy=(duint)(oxy-MAX_OXY/(duint)(oxy_time*36));
                if (oxy > MAX_OXY)
                    oxy=0;
                fuel=(duint)(fuel-
                        (duint)((MAX_FUEL/fuel_distance)*vx/65536L));
                if (fuel > MAX_FUEL)
                    fuel=0;
            }

            if (crash_type == NO_CRASH) {
                if (z < GROUND)
                    crash_type=HOLE;
                if (!fuel)
                    crash_type=FUEL_OUT;
                if (!oxy)
                    crash_type=OXY_OUT;
            }
            else
                crash_time++;
            if (burning_time)
                burning_time++;
        }
    }
}

int game(int the_end)
{   duint ret_val;

    e_speed=e_oxy_disp=e_fuel_disp=e_distance=e_fuelout_blink_on=0;
    e_adjusting=0xffff;
    cur_video_page=0;

    vga_buf_seg=alloc(320*DASHBOARD_Y);     /* sky2.c allocs this per road */
    tmpheap_seg=alloc(4096);                /* original: static Tmpheap in DS */
    check_error();
    Cars=(const car_t *)seg_ptr(Cars_Seg);

    memcpy(VGA_SCREEN+320*DASHBOARD_Y,
           seg_ptr(Background_Seg)+320*DASHBOARD_Y,320*(200-DASHBOARD_Y));

    display_number(96,156,(duint)((gravity-4+1)*100),4);

    video(  CENTER/128,
            (int)(BEG_X/(65536L/SLAB_STEP)),
            GROUND/128,
            Cars[EXPLOSION_IMAGES+3*(turn_car(CENTER)*3+jump_car(0,GROUND))],
            1,
            0,
            vga_buf_seg);
    memcpy(VGA_SCREEN,seg_ptr(vga_buf_seg),320*DASHBOARD_Y);
    fade(game_palette,1,36);
    ret_val=game_body();

    if (ret_val == NO_CRASH) {
        if (the_end)
            text(160-(7*8/2),80,"The End",DASHBOARD_COLOR+7);
        else
            text(160-(14*8/2),80,"Road Completed",DASHBOARD_COLOR+7);
        delay_ticks(36*3/4);
    }
    fade(game_palette,0,36);

    free_top();                             /* tmpheap */
    free_top();                             /* work page */

    return (int)ret_val;
}
