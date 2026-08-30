/* audio.c — SkyRoads audio: adlib.asm event-stream player on Nuked-OPL3
 * (OPL2 subset) + SoundBlaster PCM sfx, mixed in the SDL audio callback.
 *
 * Song stream (adlib.asm:19-29): u16 words, low byte = cmd(bits 0-2) |
 * channel<<4, high byte = argument.  adltick executes commands until a
 * DELAY, at 180 Hz (miscasm.asm timint, PIT divisor 0x19e4).
 */
#include "assets.h"
#include "platform.h"
#include "opl3.h"
#if !SKY_CORE
#include <SDL.h>
#endif
#include <string.h>
#include <stdio.h>

/* vendored header uses the classic null-pointer offsetof trick; silence
 * clang's UB warning for it without touching the vendored file */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnull-pointer-subtraction"
#endif
#define TSF_IMPLEMENTATION
#include "tsf.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
#include "gm_map.h"

#define ADLTICK_HZ   180.02
#define SAMPLE_RATE  44100
#define MELOCHN      6

/* ---- tables from adlib.asm:35-58 ---- */
static const uint8_t basereg[11] = { 0x20,0x40,0x60,0x80,0xe0, 0x20,0x40,0x60,0x80,0xe0, 0xc0 };
static const uint8_t chnop[22] = {
    0x00,0x01,0x02,0x08,0x09,0x0a,0x10,0x14,0x12,0x15,0x11,       /* op1 */
    0x03,0x04,0x05,0x0b,0x0c,0x0d,0x13,0xff,0xff,0xff,0xff        /* op2 */
};
static const uint8_t chnchn[11] = { 0,1,2,3,4,5,6,7,0xff,8,0xff };
static const uint8_t chgvol[31] = {
    63,20,16,14,12,10,9,8,7,6, 6,5,5,4,4,4,4,4,3,3, 3,3,2,2,2,1,1,1,1,0, 0
};
static const uint8_t lofreq[12] = { 0xac,0xb6,0xc1,0xcd,0xd9,0xe6,0xf3,0x02,0x11,0x22,0x33,0x45 };
static const uint8_t hifreq[12] = { 0,0,0,0,0,0,0,1,1,1,1,1 };

/* built-in percussion instruments (adlib.asm:62-65), 11 bytes each */
static const uint8_t perc_ins[4][11] = {
    { 0x0C,0x00,0xF8,0xB5,0x00, 0x00,0x00,0xD6,0x4F,0x00, 0x01 },  /* snare1  */
    { 0x04,0x00,0xF7,0xB5,0x00, 0x00,0x00,0xD6,0x4F,0x00, 0x01 },  /* tom1    */
    { 0x01,0x00,0xF5,0xB5,0x00, 0x00,0x00,0xD6,0x4F,0x00, 0x01 },  /* cymbal1 */
    { 0x01,0x00,0xF7,0xB5,0x00, 0x4E,0x00,0x10,0x00,0x00, 0x01 },  /* hihat1  */
};
static const uint8_t empty_song[6] = { 6,0, 0,0xff, 5,0 };  /* loopb, delay 255, end */

/* ---- player state ---- */
static opl3_chip chip;
static const uint8_t *Ins_Ptr;
static const uint8_t *Song_Ptr, *Loop_Ptr;
static uint8_t PercKey, Delay, ChnIns[11];
uint8_t Adl_Event;
static duint Playing_Song = (duint)-1;
static uint8_t musbuf[16000];
/* Locking: the standalone build uses SDL's mutex; the DXM core must link no
 * SDL at all (PORTING.md §3.5), so it uses pthreads directly. */
#if SKY_CORE
#include <pthread.h>
static pthread_mutex_t core_lock = PTHREAD_MUTEX_INITIALIZER;
#define SDL_LockMutex(m)    pthread_mutex_lock(&core_lock)
#define SDL_UnlockMutex(m)  pthread_mutex_unlock(&core_lock)
#define SDL_CreateMutex()   NULL
static void *lock;
#else
static SDL_mutex *lock;
#endif

/* ---- wavetable ("AWE32") backend: TinySoundFont over the same events ---- */
static tsf *wt;                       /* NULL if no soundfont found */
static int  wt_on;                    /* F9 toggles when wt is available */
static int  wt_note[11];              /* sounding MIDI note per channel, -1 none */
static float wt_vel[11];              /* velocity from VOL_CHANGE, 0..1 */
/* percussion channels 6..10 -> GM drum notes (kick/snare/tom/cymbal/hat) */
static const uint8_t wt_drum[5] = { 36, 38, 47, 49, 42 };

static int gm_program(int ins) {
    const uint8_t *p = Ins_Ptr + ins * 16;
    for (unsigned i = 0; i < sizeof GmMap / sizeof *GmMap; i++)
        if (!memcmp(GmMap[i].patch, p, 11)) return GmMap[i].gm;
    return 80;                        /* unknown patch: square lead */
}

/* PCM sfx channel (sbdma) */
static const uint8_t *pcm_buf;
static uint32_t pcm_len, pcm_pos_fx;   /* pos in 16.16 */
static uint32_t pcm_step;

static void adlout(uint8_t reg, uint8_t val) { OPL3_WriteRegBuffered(&chip, reg, val); }

static void noteoff(int ch) {                       /* adlib.asm:249 */
    if (wt && wt_note[ch] >= 0) {
        tsf_channel_note_off(wt, ch < MELOCHN ? ch : 9, wt_note[ch]);
        wt_note[ch] = -1;
    }
    if (ch < MELOCHN) { adlout(0xb0 + ch, 0); return; }
    PercKey &= (uint8_t)((0xef >> (ch - MELOCHN)) | (0xef << (8 - (ch - MELOCHN))));
    adlout(0xbd, PercKey);
}

static void chprog(int ch, int ins) {               /* adlib.asm:156 */
    noteoff(ch);
    const uint8_t *si = Ins_Ptr + ins * 16;
    ChnIns[ch] = (uint8_t)ins;
    for (int b = 0; b < 5; b++)
        adlout(chnop[ch] + basereg[b], si[b]);
    if (chnop[ch + 11] != 0xff)
        for (int b = 5; b < 10; b++)
            adlout(chnop[ch + 11] + basereg[b], si[b]);
    if (chnchn[ch] != 0xff)
        adlout(chnchn[ch] + basereg[10], si[10]);
}

static void chprog_raw(int ch, const uint8_t *si) { /* init's perc setup */
    noteoff(ch);
    ChnIns[ch] = 0;
    for (int b = 0; b < 5; b++)
        adlout(chnop[ch] + basereg[b], si[b]);
    if (chnop[ch + 11] != 0xff)
        for (int b = 5; b < 10; b++)
            adlout(chnop[ch + 11] + basereg[b], si[b]);
    if (chnchn[ch] != 0xff)
        adlout(chnchn[ch] + basereg[10], si[10]);
}

static void noteon(int ch, int note) {              /* adlib.asm:196 */
    noteoff(ch);
    if (wt && wt_on) {
        if (ch < MELOCHN) {
            tsf_channel_set_presetnumber(wt, ch, gm_program(ChnIns[ch]), 0);
            wt_note[ch] = note + 24;              /* song note 0 = C1 */
            tsf_channel_note_on(wt, ch, wt_note[ch], wt_vel[ch]);
        } else {
            wt_note[ch] = wt_drum[ch - MELOCHN];
            tsf_channel_note_on(wt, 9, wt_note[ch], wt_vel[ch]);
        }
    }
    int slot = chnchn[ch];
    if (ch < MELOCHN + 1) {                         /* melodic + bass drum */
        int oct = note / 12 + 2, n = note % 12;
        adlout(0xa0 + slot, lofreq[n]);
        uint8_t hi = (uint8_t)(hifreq[n] | (oct << 2));
        if (slot < MELOCHN) { adlout(0xb0 + slot, hi | 0x20); return; }
        adlout(0xb0 + slot, hi);                    /* bass drum: no key-on bit */
    }
    PercKey |= (uint8_t)(0x10 >> (ch - MELOCHN));
    adlout(0xbd, PercKey);
}

static void volume(int ch, int vol) {               /* adlib.asm:265-301 */
    const uint8_t *si = Ins_Ptr + ChnIns[ch] * 16;
    if (vol > 30) vol = 30;
    wt_vel[ch] = (float)(63 - chgvol[vol]) / 63.0f;
    int two_op = chnop[ch + 11] != 0xff;
    /* op2 (carrier) always when present */
    if (two_op) {
        uint8_t lvl = si[5 + 1];
        uint8_t out = (uint8_t)((lvl & 0x3f) + chgvol[vol]);
        if (out > 0x3f) out = 0x3f;
        adlout(chnop[ch + 11] + 0x40, (uint8_t)((lvl & 0xc0) | out));
        if (!(si[10] & 1)) return;                  /* FM: modulator untouched */
    }
    uint8_t lvl = si[1];
    uint8_t out = (uint8_t)((lvl & 0x3f) + chgvol[vol]);
    if (out > 0x3f) out = 0x3f;
    adlout(chnop[ch] + 0x40, (uint8_t)((lvl & 0xc0) | out));
}

static void adl_stop_locked(void) {                 /* adlib.asm:101 */
    if (wt) tsf_note_off_all(wt);
    for (int i = 0; i < 11; i++) { wt_note[i] = -1; wt_vel[i] = 0.85f; }
    Song_Ptr = Loop_Ptr = empty_song;
    PercKey = 0xe0;
    for (int r = 0x40; r <= 0x55; r++) adlout((uint8_t)r, 0x3f);
    for (int ch = MELOCHN + 1; ch >= 0; ch--) noteoff(ch);
}

static void adl_init_locked(void) {                 /* adlib.asm:121 */
    adl_stop_locked();
    adlout(0x01, 0x20);                             /* waveform select enable */
    adlout(0x08, 0x00);
    adlout(0xbd, 0xe0);                             /* percussion mode on */
    for (int ch = MELOCHN + 1, k = 0; k < 4; ch++, k++)
        chprog_raw(ch, perc_ins[k]);
    adlout(0xa8, 0xac); adlout(0xb8, 0x0c);         /* SD/TT fixed freqs */
    adlout(0xa7, 0x02); adlout(0xb7, 0x0d);
}

static void adltick(void) {                         /* adlib.asm:320 */
    for (;;) {
        if (Delay) { Delay--; return; }
        uint8_t lo = Song_Ptr[0], hi = Song_Ptr[1];
        Song_Ptr += 2;
        int cmd = lo & 7, ch = lo >> 4;
        switch (cmd) {
        case 0: Delay = hi; break;
        case 1: chprog(ch, hi); break;
        case 2: noteon(ch, hi & 0x7f); break;
        case 3: noteoff(ch); break;
        case 4: volume(ch, hi); break;
        case 5: Song_Ptr = Loop_Ptr; break;
        case 6: Loop_Ptr = Song_Ptr; break;
        case 7: Adl_Event = hi; break;
        }
    }
}

/* ---- SDL mixing ---- */
static double tick_accum;

static void audio_cb(void *ud, uint8_t *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / 4;                           /* stereo s16 */
    SDL_LockMutex(lock);
    int use_wt = wt && wt_on;
    for (int i = 0; i < frames; ) {
        tick_accum += ADLTICK_HZ / SAMPLE_RATE;
        if (tick_accum >= 1.0) { tick_accum -= 1.0; adltick(); }
        int n = (int)((1.0 - tick_accum) / (ADLTICK_HZ / SAMPLE_RATE)) + 1;
        if (n > frames - i) n = frames - i;
        if (n < 1) n = 1;
        if (use_wt) {
            tsf_render_short(wt, out + i * 2, n, 0);
            tick_accum += (n - 1) * (ADLTICK_HZ / SAMPLE_RATE);
        } else {
            n = 1;
            int16_t sm[2];
            OPL3_GenerateResampled(&chip, sm);
            int ml = sm[0] * 2, mr = sm[1] * 2;
            out[i * 2]     = (int16_t)(ml > 32767 ? 32767 : ml < -32768 ? -32768 : ml);
            out[i * 2 + 1] = (int16_t)(mr > 32767 ? 32767 : mr < -32768 ? -32768 : mr);
        }
        i += n;
    }
    /* overlay the PCM sfx channel */
    for (int i = 0; i < frames && pcm_buf; i++) {
        uint32_t p = pcm_pos_fx >> 16;
        if (p >= pcm_len) { pcm_buf = NULL; break; }
        int s = ((int)pcm_buf[p] - 128) * 200;   /* sfx +4 dB over the old mix */
        pcm_pos_fx += pcm_step;
        int l = out[i * 2] + s, r = out[i * 2 + 1] + s;
        out[i * 2]     = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
        out[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
    }
    SDL_UnlockMutex(lock);
}

static void wt_toggle(void) {
    SDL_LockMutex(lock);
    if (wt) {
        wt_on = !wt_on;
        plat_osd(wt_on ? "MUSIC AWE32" : "MUSIC ADLIB");
        tsf_note_off_all(wt);
        for (int i = 0; i < 11; i++) wt_note[i] = -1;
    }
    SDL_UnlockMutex(lock);
}

/* Reset every mutable audio static between runs (PORTING.md §3.2). */
void audio_reset_state(void) {
#if !SKY_CORE
    if (!lock) return;              /* called before audio_init on first run */
#endif
    SDL_LockMutex(lock);
    pcm_buf = NULL; pcm_len = 0; pcm_pos_fx = 0;
    Playing_Song = (duint)-1;
    Song_Ptr = Loop_Ptr = NULL; Ins_Ptr = NULL;
    Delay = 0; PercKey = 0;
    SDL_UnlockMutex(lock);
}

void audio_init(void) {
    static int inited;
    if (inited) {                 /* re-init would leak the soundfont and race
                                   * the host's audio thread mid-render */
        audio_reset_state();
        OPL3_Reset(&chip, SAMPLE_RATE);
        adl_init_locked();
        return;
    }
    inited = 1;
    lock = SDL_CreateMutex();
    char sf[1200];
    snprintf(sf, sizeof sf, "%s/TimGM6mb.sf2", sky_data_dir());
    wt = tsf_load_filename(sf);
    if (wt) {
        tsf_set_output(wt, TSF_STEREO_INTERLEAVED, SAMPLE_RATE, -3.0f);
        tsf_channel_set_bank_preset(wt, 9, 128, 0);   /* GM drums */
        wt_on = 0;                    /* default: authentic AdLib FM; F9 for AWE32 */
        plat_f9_hook = wt_toggle;
    }
    for (int i = 0; i < 11; i++) { wt_note[i] = -1; wt_vel[i] = 0.85f; }
    OPL3_Reset(&chip, SAMPLE_RATE);
    adl_init_locked();
#if SKY_CORE
    /* No device here — the host owns the one audio device and pulls frames
     * via sky_audio_render() (PORTING.md §2.5). */
#else
    SDL_AudioSpec want = {0}, have;
    want.freq = SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = (SDL_AudioCallback)audio_cb;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev) SDL_PauseAudioDevice(dev, 0);
#endif
}

/* sbdma(buf,len,smprate): SB time constant tc -> rate = 1000000/(256-tc) */
void sbdma(const uint8_t *buf, uint32_t len, duint smprate) {
    SDL_LockMutex(lock);
    uint32_t rate = smprate > 255 ? smprate : 1000000u / (256u - smprate);
    pcm_buf = buf;
    pcm_len = len;
    pcm_pos_fx = 0;
    pcm_step = (uint32_t)((uint64_t)rate * 65536 / SAMPLE_RATE);
    SDL_UnlockMutex(lock);
}

void sbstop(void) { SDL_LockMutex(lock); pcm_buf = NULL; SDL_UnlockMutex(lock); }

/* ---- play_song (intro.c:996) — real implementation ---- */
void play_song(duint songnr) {
    struct { duint offset, instruments, songlen; } hdr;
    if (Playing_Song == songnr) return;
    SDL_LockMutex(lock);
    adl_stop_locked();
    SDL_UnlockMutex(lock);
    if (cfg.silence) return;
    int h = xopenr("muzax.lzs");
    if (SysErr) { SysErr = 0; return; }
    xseek(h, songnr * (long)sizeof hdr, 0);
    xread(h, &hdr, sizeof hdr);
    xseek(h, hdr.offset, 0);
    init_bit_i(h, 0, 4096, 0);
    if (hdr.songlen < sizeof musbuf) {
        extr_lzss(musbuf, hdr.songlen);
        if (!SysErr) {
            SDL_LockMutex(lock);
            adl_init_locked();
            Ins_Ptr = musbuf;
            Song_Ptr = Loop_Ptr = musbuf + hdr.instruments * 16;
            Adl_Event = 0;
            Delay = 0;
            Playing_Song = songnr;
            SDL_UnlockMutex(lock);
        }
    }
    xclose(h);
    SysErr = 0;
}

void stop_song(void) {
    SDL_LockMutex(lock);
    adl_stop_locked();
    Playing_Song = (duint)-1;
    SDL_UnlockMutex(lock);
}

#if SKY_CORE
/* Host-pulled audio: DXM calls this from its own device callback.  Pull, not
 * push — the game already renders on demand, so pushing would only add a
 * buffer and its latency. */
void sky_audio_render(int16_t *out, int nframes) {
    audio_cb(NULL, (uint8_t *)out, nframes * 2 * (int)sizeof(int16_t));
}
#endif
