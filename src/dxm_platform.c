/* platform_dxm.c — THE shared adapter (PORTING.md §1).
 * Implements the standard platform.h seam in terms of dxm_host.  Contains no
 * game-specific logic, so every port links this same file rather than writing
 * its own: that is what makes game N cheap. */
#include "platform.h"
#include "compat.h"
#include "dxm_core.h"
#include <string.h>
#include <setjmp.h>

volatile duint Time;
void (*plat_f9_hook)(void);

static const dxm_host *H;
static double  tick_origin;
static jmp_buf exit_jmp;
static int     exit_armed;
#define TICK_HZ (DXM_PIT_HZ / 0x19e4 / 5.0)

static const dxm_mode MODE_13H = { 320, 200, DXM_FB_INDEX8, 5, 6, 400 };
static uint8_t pal8[256*3];

void dxm_adapter_bind(const dxm_host *h){ H=h; tick_origin=h->now(); }
jmp_buf *dxm_adapter_exit_target(void){ exit_armed=1; return &exit_jmp; }

int  plat_init(const char *title,int scale){ (void)title;(void)scale; return 0; }
void plat_quit(void){}

void plat_present(void){
    const uint8_t *src=vga_mem();
    for(int i=0;i<256;i++){                    /* 6-bit DAC -> 8-bit */
        const uint8_t *c=g_palette[i];
        pal8[i*3+0]=(uint8_t)((c[0]<<2)|(c[0]>>4));
        pal8[i*3+1]=(uint8_t)((c[1]<<2)|(c[1]>>4));
        pal8[i*3+2]=(uint8_t)((c[2]<<2)|(c[2]>>4));
    }
    dxm_frame f = { &MODE_13H, src, pal8 };
    H->present(&f);
}
int plat_pump(void){ return !H->should_quit(); }

unsigned plat_keys(void){
    unsigned m=0;
    if(H->key_down(DXM_SC_LEFT))  m|=K_LEFT;
    if(H->key_down(DXM_SC_RIGHT)) m|=K_RIGHT;
    if(H->key_down(DXM_SC_UP))    m|=K_UP;
    if(H->key_down(DXM_SC_DOWN))  m|=K_DOWN;
    if(H->key_down(DXM_SC_SPACE)) m|=K_SPACE;
    if(H->key_down(DXM_SC_ESC))   m|=K_ESC;
    if(H->key_down(DXM_SC_ENTER)) m|=K_RET;
    return m;
}
int plat_getch(void){
    int c=H->getch();
    if(c & 0x100) return 0;                    /* extended: not plain ASCII */
    return c;
}
int plat_getch_ext(void){
    int c=H->getch();
    if(!(c & 0x100)) return c;
    switch(c & 0xFF){
        case DXM_SC_UP:    return 0x148;
        case DXM_SC_DOWN:  return 0x150;
        case DXM_SC_LEFT:  return 0x14b;
        case DXM_SC_RIGHT: return 0x14d;
    }
    return 0;
}
void   plat_sleep(int ms){ H->sleep_ms(ms); }

/* The game's one lock, taken from the SHELL.  It cannot be created here:
 * this file is compiled INTO the core, and a core shipped as a loadable
 * module must link no threading library of its own or it stops being
 * loadable.  A call before dxm_adapter_bind() is a no-op, which is correct
 * - the core thread has not started, so there is nothing to race. */
void   plat_lock(void)  { if(H && H->lock)   H->lock(); }
void   plat_unlock(void){ if(H && H->unlock) H->unlock(); }
double plat_now(void){ return H->now(); }
void   plat_tick_update(void){ Time=(duint)((plat_now()-tick_origin)*TICK_HZ); }
void   plat_osd(const char *msg){ H->log(msg); }
const char *plat_pref_path(void){ return H->pref_dir; }
const char *plat_base_path(void){ return H->data_dir; }

/* PORTING.md §3.1: unwind instead of killing the host process. */
void plat_exit(int code){ (void)code; if(exit_armed) longjmp(exit_jmp,1); }
