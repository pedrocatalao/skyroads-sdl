#include "compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t g_arena[ARENA_SIZE];
int     SysErr = 0;
uint8_t g_palette[256][3];
int     g_palette_dirty = 0;

void sys_err(int code)   { SysErr = code; }
void norm_sys_err(void)  { SysErr = 0; }

/* ---------------- allocator ----------------
 * Bump allocator over segs 0x1000..0x9FFF (below VGA), 16-byte aligned,
 * mirroring the original's start_alloc/free_memory stack usage loosely:
 * the game's own alloc()/free() wrappers (ported in game code) manage marks.
 */
static uint32_t arena_brk = 0x10000;           /* seg 0x1000 */

void arena_reset(void) { arena_brk = 0x10000; }

#ifdef SKY_GUARD
uint32_t arena_brk_dbg(void) { return arena_brk; }
#endif

duint xalloc(uint32_t bytes) {
    uint32_t need = (bytes + 15u) & ~15u;
    if (arena_brk + need > ((uint32_t)SEG_VGA << 4)) { SysErr = NO_MEM; return 0; }
    duint seg = (duint)(arena_brk >> 4);
    memset(g_arena + arena_brk, 0, need);
    arena_brk += need;
    return seg;
}

/* The game frees strictly LIFO via its Alloc[] stack; xfree(seg) rolls the
 * brk back when the freed block is the top allocation, which is always the
 * case for the ported call sites. */
void xfree(duint seg) {
    uint32_t base = (uint32_t)seg << 4;
    if (base < arena_brk) arena_brk = base;
}

/* ---------------- file I/O ---------------- */
#define MAX_FILES 16
static FILE *files[MAX_FILES];
static char  data_dir[1024] = ".";

const char *sky_data_dir(void) { return data_dir; }

void set_data_dir(const char *dir) {
    snprintf(data_dir, sizeof data_dir, "%s", dir);
}

static FILE *open_path(const char *name, const char *mode) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", data_dir, name);
    FILE *f = fopen(path, mode);
    if (!f) {                           /* DOS-style UPPERCASE data files */
        char up[1200];
        size_t n = strlen(data_dir);
        snprintf(up, sizeof up, "%s/%s", data_dir, name);
        for (size_t i = n + 1; up[i]; i++)
            if (up[i] >= 'a' && up[i] <= 'z') up[i] = (char)(up[i] - 32);
        f = fopen(up, mode);
    }
    if (!f) f = fopen(name, mode);      /* fall back to cwd (cfg files) */
    return f;
}

static int stash(FILE *f) {
    for (int i = 1; i < MAX_FILES; i++)
        if (!files[i]) { files[i] = f; return i; }
    fclose(f);
    SysErr = ERR_FILE;
    return 0;
}

int xopenr(const char *name) {
    FILE *f = open_path(name, "rb");
    if (!f) { SysErr = ERR_FILE; return 0; }
    return stash(f);
}

int xcreate(const char *name, int attr) {
    (void)attr;
    FILE *f = open_path(name, "wb");
    if (!f) { SysErr = ERR_FILE; return 0; }
    return stash(f);
}

void xclose(int h) {
    if (h > 0 && h < MAX_FILES && files[h]) { fclose(files[h]); files[h] = NULL; }
}

duint xread(int h, void *buf, duint len) {
    if (h <= 0 || !files[h]) { SysErr = ERR_FILE; return 0; }
    size_t n = fread(buf, 1, len, files[h]);
    if (n < len) SysErr = ERR_EOF;
    return (duint)n;
}

void xwrite(int h, const void *buf, duint len) {
    if (h <= 0 || !files[h]) { SysErr = ERR_FILE; return; }
    fwrite(buf, 1, len, files[h]);
}

long xseek(int h, long off, int whence) {
    if (h <= 0 || !files[h]) { SysErr = ERR_FILE; return -1; }
    fseek(files[h], off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
    return ftell(files[h]);
}

/* ---------------- pack.lib bit stream + LZSS ----------------
 * Format recovered from german/skyroads.exe disassembly (see tools/unlzs.c):
 * payload = 3 raw bytes (len_bits, pos_bits1, pos_bits2), then an MSB-first
 * bitstream of tokens:
 *   0            short match: dist=get(pos1)+2         len=get(len_bits)+2
 *   1,0          long match:  dist=get(pos2)+(1<<pos1)+2, same len
 *   1,1          literal:     get(8)
 * An over-long match token terminates.  Stream is byte-aligned afterwards.
 */
static struct {
    int   handle;
    uint8_t cur;        /* current byte, remaining bits in high positions */
    int   bits;         /* bits left in cur (0..8) */
    int   eof;
} bs;

void init_bit_i(int handle, int u1, duint bufsize, int u2) {
    (void)u1; (void)bufsize; (void)u2;
    bs.handle = handle;
    bs.bits = 0;
    bs.cur = 0;
    bs.eof = 0;
}

static int bs_next_byte(void) {
    uint8_t b;
    if (xread(bs.handle, &b, 1) != 1) { bs.eof = 1; return -1; }
    return b;
}

duint rd_byte(void) {              /* raw byte (discards partial bit buffer) */
    bs.bits = 0;
    int b = bs_next_byte();
    return b < 0 ? 0 : (duint)b;
}

duint rd_word(void) {
    duint lo = rd_byte();
    duint hi = rd_byte();
    return (duint)(lo | (hi << 8));
}

void rd_mem(void *buf, duint len) {
    bs.bits = 0;
    xread(bs.handle, buf, len);
}

static unsigned bs_bit(void) {
    if (bs.bits == 0) {
        int b = bs_next_byte();
        if (b < 0) return 0;
        bs.cur = (uint8_t)b;
        bs.bits = 8;
    }
    unsigned bit = (bs.cur >> 7) & 1u;
    bs.cur <<= 1;
    bs.bits--;
    return bit;
}

static unsigned bs_bits(int n) {
    unsigned v = 0;
    while (n--) v = (v << 1) | bs_bit();
    return v;
}

void extr_lzss(uint8_t *dest, duint outlen) {
    int len_bits = (int)rd_byte();
    int pos1     = (int)rd_byte();
    int pos2     = (int)rd_byte();
    if (SysErr) return;
    uint32_t op = 0, end = outlen;
    while (op < end && !bs.eof) {
        if (bs_bit()) {
            if (bs_bit()) {                       /* literal */
                dest[op++] = (uint8_t)bs_bits(8);
                continue;
            }
            unsigned dist = bs_bits(pos2) + (1u << pos1) + 2;
            unsigned cnt  = bs_bits(len_bits) + 1;
            if (cnt >= end - op) break;           /* end marker */
            for (unsigned i = 0; i <= cnt; i++, op++) dest[op] = dest[op - dist];
        } else {
            unsigned dist = bs_bits(pos1) + 2;
            unsigned cnt  = bs_bits(len_bits) + 1;
            if (cnt >= end - op) break;
            for (unsigned i = 0; i <= cnt; i++, op++) dest[op] = dest[op - dist];
        }
    }
    bs.bits = 0;                                  /* byte-align */
}

/* ---------------- palette ---------------- */
void set_color_regs(duint start, duint count, const uint8_t (*rgb)[3]) {
    for (duint i = 0; i < count && start + i < 256; i++) {
        g_palette[start + i][0] = rgb[i][0];
        g_palette[start + i][1] = rgb[i][1];
        g_palette[start + i][2] = rgb[i][2];
    }
    g_palette_dirty = 1;
}
