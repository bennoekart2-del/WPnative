/* DSP interface (CC005000) and a high-level stand-in for the DSP itself.
 *
 * The real DSP runs microcode the game uploads (AX, the audio mixer). Like Dolphin's DSPHLE, the
 * microcode is not run: the mailbox conversation it has with the CPU is answered here instead
 * (mail values and boot sequence from Dolphin's HW/DSPHLE/UCodes/ROM.cpp and AX.cpp).
 *
 *   ROM:  says hello (0x8071FEED) when released from reset and halt, then takes the task boot mails from __DSP_boot_task
 *         (0x80F3A001 addr, 0x80F3C002 addr, 0x80F3A002 len, 0x80F3B002 addr, 0x80F3D001 vec)
 *         and "starts" the task, which answers DSP_INIT.
 *   AX:   0xBABEnnnn announces a command list whose address is the next mail; once handled the
 *         DSP yields (DSP_YIELD) and the CPU answers MAIL_CONTINUE or MAIL_NEW_UCODE.
 *
 * Registers (16-bit): 00/02 CPU->DSP mail, 04/06 DSP->CPU mail, 0A DSPCR, 20-2A ARAM DMA,
 * 30/32 AI DMA start, 36 AI DMA control/length (bit 15 = run, length in 32-byte blocks),
 * 3A AI DMA blocks left.
 * DSPCR: 0 RES, 2 HALT, 3 AIDINT, 4 AIDINTMSK, 5 ARINT, 6 ARINTMSK, 7 DSPINT, 8 DSPINTMSK, 9 DMA busy.
 * Interrupt status bits 3/5/7 are cleared by writing 1. Any of them with its mask set raises
 * the PI DSP interrupt (cause bit 6).
 *
 * The AX command lists are carried out here (see the AX section below), and each AI DMA block
 * the game queues is played through platform_audio.c when its period starts. */
#include <stdio.h>
#include <stdlib.h>

#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_timer.h>

#include "ppc_context.h"
#include "runtime.h"

void irq_raise(uint32_t cause);
void irq_clear(uint32_t cause);
uint32_t hw_ai_dma_rate(void);  /* hw.c */
#define PI_CAUSE_DSP 0x40u

#define DSP_INIT   0xDCD10000u
#define DSP_RESUME 0xDCD10001u
#define DSP_YIELD  0xDCD10002u
#define MAIL_RESUME    0xCDD10000u
#define MAIL_NEW_UCODE 0xCDD10001u
#define MAIL_RESET     0xCDD10002u
#define MAIL_CONTINUE  0xCDD10003u

#define CR_RES 0x0001u
#define CR_HALT 0x0004u
#define CR_AIDINT 0x0008u
#define CR_ARINT 0x0020u
#define CR_DSPINT 0x0080u
#define CR_INTS (CR_AIDINT | CR_ARINT | CR_DSPINT)

static uint16_t regs[0x40];
static SDL_SpinLock lock;  /* DSPCR is touched by the guest and by the AI DMA timer */
static int trace = -1;

static void update_irq(void) {
    uint16_t cr = regs[0x0A / 2];
    int on = ((cr & CR_AIDINT) && (cr & 0x10)) || ((cr & CR_ARINT) && (cr & 0x40)) ||
             ((cr & CR_DSPINT) && (cr & 0x100));
    if (on) irq_raise(PI_CAUSE_DSP); else irq_clear(PI_CAUSE_DSP);
}

static void cr_set(uint16_t bits) {
    SDL_LockSpinlock(&lock);
    regs[0x0A / 2] |= bits;
    update_irq();
    SDL_UnlockSpinlock(&lock);
}

/* ------------------------------------------------------------------ DSP -> CPU mail queue */
static uint32_t outq[16];
static unsigned out_r, out_w;

static void push_mail(uint32_t m, int interrupt) {
    if (trace) fprintf(stderr, "[dsp] -> %08X\n", m);
    if (out_w - out_r < 16) outq[out_w++ % 16] = m;
    if (interrupt) cr_set(CR_DSPINT);
}

/* ------------------------------------------------------------------ AX command lists (AXWii)
 * What the AX microcode does with a command list, written from the description of the Wii
 * variant in Dolphin's HW/DSPHLE/UCodes/AXWii.cpp, AXVoice.h and DSP/DSPAccelerator.cpp.
 * One list covers one 3 ms frame: 96 samples at 32 kHz on twelve buses (main, aux A/B/C; each
 * L, R, surround). Voices are parameter blocks (PBs) in main memory, chained by next_pb; each is
 * decoded (ADPCM, PCM8, PCM16 through the accelerator), resampled, put through its volume
 * envelope and filters, and added to the buses it is routed to. OUTPUT then writes main L/R as
 * big-endian s16 pairs, right sample first, which the CPU hands to the AI DMA.
 * The 4-tap polyphase resampler needs coefficient tables from the DSP's own ROM, which is not
 * available here, so it falls back to linear interpolation (as Dolphin does without a DROM). */
#define AX_N 96
enum { B_ML, B_MR, B_MS, B_AL, B_AR, B_AS, B_BL, B_BR, B_BS, B_CL, B_CR, B_CS, B_COUNT };
static int32_t bus[B_COUNT][AX_N];
static uint16_t last_main_vol = 0x8000, last_aux_vol[3] = {0x8000, 0x8000, 0x8000};
static uint16_t compressor_pos;
static uint32_t ucode_crc;
/* PB layouts and command numbers differ between AXWii builds (keyed by CRC, as in Dolphin):
 * the oldest has per-millisecond PB updates and a PB_ADDR command, the newest a biquad filter. */
enum { AXV_OLD, AXV_MID, AXV_NEW };
static int ax_ver = AXV_NEW;
static uint16_t cmdlist_size;

/* Guest memory as the DSP sees it: physical addresses in MEM1 or MEM2. */
static int ax_ok(uint32_t p, uint32_t n) {
    return p + n <= 0x01800000u || (p >= 0x10000000u && p + n <= 0x14000000u);
}
static uint16_t rd16(uint32_t a) {
    uint32_t p = a & 0x1FFFFFFFu;
    return ax_ok(p, 2) ? (uint16_t)(g_mem[p] << 8 | g_mem[p + 1]) : 0;
}
static uint32_t rd32(uint32_t a) { return (uint32_t)rd16(a) << 16 | rd16(a + 2); }
static void wr16(uint32_t a, uint16_t v) {
    uint32_t p = a & 0x1FFFFFFFu;
    if (ax_ok(p, 2)) { g_mem[p] = (uint8_t)(v >> 8); g_mem[p + 1] = (uint8_t)v; }
}
static void wr32(uint32_t a, uint32_t v) { wr16(a, (uint16_t)(v >> 16)); wr16(a + 2, (uint16_t)v); }

static int16_t clamp16(int64_t v) { return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v); }

static void ramp(uint16_t* out, uint16_t v1, uint16_t v2, int n) {
    float cur = v1, step = (float)((int)v2 - (int)v1) / (float)n;
    for (int i = 0; i < n; i++) { cur += step; out[i] = (uint16_t)cur; }
}

/* PB word offsets (u16 index into the guest struct). Fields after the depop block move down by
 * five words in the oldest layout, which keeps an "updates" header there. */
enum {
    PB_NEXT = 0, PB_SRC_TYPE = 4, PB_MIXCTL = 6, PB_RUNNING = 8, PB_IS_STREAM = 9,
    PB_MIXER = 10,  /* 12 x {volume, delta}: ML MR AL AR BL BR CL CR MS AS BS CS */
    PB_DPOP = 41,   /* 12 x s16: ML AL BL CL MR AR BR CR MS AS BS CS */
    PB_UPDATES = 53, /* old layout only: count[3], data hi/lo */
    /* the rest is relative to pb_tail (53, or 58 in the old layout) */
    T_VOLENV = 0, T_LOOPING = 2, T_FORMAT = 3, T_LOOP = 4, T_END = 6, T_CUR = 8,
    T_COEFS = 10, T_GAIN = 26, T_PS = 27, T_YN1 = 28, T_YN2 = 29,
    T_RATIO = 30, T_FRAC = 32, T_LAST = 33, T_LOOP_PS = 37, T_LOOP_YN1 = 38, T_LOOP_YN2 = 39,
    T_LPF = 40, T_BIQUAD = 44, T_END_OF_FIELDS = 54,
};
#define PB_WORDS (58 + T_END_OF_FIELDS)
/* per bus: mixer slot, depop slot, mixer_control bits that enable it / enable its ramp */
static const struct { uint8_t mix, dpop; uint32_t on, rmp; } bus_map[B_COUNT] = {
    {0, 0, 0x1 | 0x4, 0x4},                {1, 4, 0x2 | 0x4, 0x4},
    {8, 8, 0x8 | 0x10, 0x10},              {2, 1, 0x10000 | 0x40000, 0x40000},
    {3, 5, 0x20000 | 0x40000, 0x40000},    {9, 9, 0x80000 | 0x100000, 0x100000},
    {4, 2, 0x200000 | 0x800000, 0x800000}, {5, 6, 0x400000 | 0x800000, 0x800000},
    {10, 10, 0x1000000 | 0x2000000, 0x2000000}, {6, 3, 0x4000000 | 0x10000000, 0x10000000},
    {7, 7, 0x8000000 | 0x10000000, 0x10000000}, {11, 11, 0x20000000 | 0x40000000, 0x40000000},
};

/* The DSP accelerator: reads and decodes samples, raising the end-of-sample event at end. */
typedef struct {
    uint16_t* pb;
    int tail;
    uint32_t start, end, cur;
    uint16_t fmt, ps;
    int16_t yn1, yn2, gain;
    int stopped;
} Acc;

static uint8_t acc_byte(uint32_t a) {  /* MEM2 when bit 28 is set, otherwise MEM1 */
    uint32_t p = (a & 0x10000000u) ? 0x10000000u | (a & 0x3FFFFFFu) : a & 0x1FFFFFFu;
    return ax_ok(p, 1) ? g_mem[p] : 0;
}

static uint16_t acc_raw(const Acc* s) {
    switch (s->fmt & 3) {
    case 0: { uint8_t b = acc_byte(s->cur >> 1); return (s->cur & 1) ? (b & 0xF) : (b >> 4); }
    case 1: return acc_byte(s->cur);
    case 2: return (uint16_t)(acc_byte(s->cur * 2) << 8 | acc_byte(s->cur * 2 + 1));
    }
    return 0;
}

static int16_t acc_read(Acc* s) {
    if (s->stopped) return 0;
    uint16_t* pb = s->pb;
    const uint16_t* t = pb + s->tail;
    int16_t raw = (int16_t)acc_raw(s);
    int ci = (s->ps >> 4) & 7;
    int32_t c1 = (int16_t)t[T_COEFS + ci * 2], c2 = (int16_t)t[T_COEFS + ci * 2 + 1];
    int16_t val = 0;
    uint32_t step = 2;
    int decode = (s->fmt >> 2) & 3;
    if (decode == 0) {  /* ADPCM: 4-bit nibbles, a header byte (pred/scale) every 16 nibbles */
        int n = raw & 0xF;
        if (n >= 8) n -= 16;
        int32_t v = (1 << (s->ps & 0xF)) * n + ((0x400 + c1 * s->yn1 + c2 * s->yn2) >> 11);
        val = clamp16(v);
        s->yn2 = s->yn1; s->yn1 = val;
        s->cur++;
        if ((s->end & 0xF) == 0 && s->cur == s->end) s->cur = s->start + 1;
        else if ((s->end & 0xF) == 1 && s->cur == s->end - 1) s->cur = s->start;
        else if ((s->cur & 15) == 0) {
            s->ps = acc_byte((s->cur & ~15u) >> 1) & 0x7F;
            s->cur += 2;
            step += 2;
        }
    } else {  /* PCM8 / PCM16 with gain */
        int shift = ((s->fmt >> 4) & 3) == 0 ? 11 : ((s->fmt >> 4) & 3) == 1 ? 0 : 16;
        int32_t v = ((int32_t)s->gain * raw >> shift) + ((c1 * s->yn1) >> shift) + ((c2 * s->yn2) >> shift);
        val = (int16_t)v;
        s->yn2 = s->yn1; s->yn1 = val;
        if (decode != 1) s->cur++;
    }
    if (s->cur == s->end + step - 1) {  /* end of sample: loop, or stop the voice */
        s->cur = s->start;
        s->stopped = 1;
        if (t[T_LOOPING]) {
            s->ps = t[T_LOOP_PS] & 0x7F;
            if (pb[PB_IS_STREAM] != 1) { s->yn1 = (int16_t)t[T_LOOP_YN1]; s->yn2 = (int16_t)t[T_LOOP_YN2]; }
            s->stopped = 0;
        } else {
            pb[PB_RUNNING] = 0;
        }
    }
    s->cur &= 0xBFFFFFFFu;
    return val;
}

/* Read <count> samples at the PB's rate and convert them to 32 kHz. */
static void voice_input(uint16_t* pb, int tail, int16_t* out, int count) {
    uint16_t* t = pb + tail;
    Acc s = {pb, tail,
             ((uint32_t)t[T_LOOP] << 16 | t[T_LOOP + 1]) & 0x3FFFFFFFu,
             ((uint32_t)t[T_END] << 16 | t[T_END + 1]) & 0x3FFFFFFFu,
             ((uint32_t)t[T_CUR] << 16 | t[T_CUR + 1]) & 0xBFFFFFFFu,
             t[T_FORMAT], (uint16_t)(t[T_PS] & 0x7F), (int16_t)t[T_YN1], (int16_t)t[T_YN2],
             (int16_t)t[T_GAIN], 0};
    uint32_t pos = t[T_FRAC], ratio = (uint32_t)t[T_RATIO] << 16 | t[T_RATIO + 1];
    int16_t* last = (int16_t*)&t[T_LAST];
    if (pb[PB_SRC_TYPE] >= 2) {  /* no conversion */
        for (int i = 0; i < count; i++) out[i] = acc_read(&s);
        for (int k = 0; k < 4; k++) last[k] = out[count - 4 + k];
    } else {  /* linear (also standing in for the polyphase filter) */
        int16_t ring[4] = {last[0], last[1], last[2], last[3]};
        unsigned idx = 4;
        for (int i = 0; i < count; i++) {
            pos += ratio;
            while (pos >= 0x10000) { ring[idx++ & 3] = acc_read(&s); pos -= 0x10000; }
            uint32_t frac = pos & 0xFFFF;
            int32_t s0 = ring[idx & 3], s1 = ring[(idx + 1) & 3];
            out[i] = frac ? (int16_t)((s0 * (int32_t)(0x10000 - frac) + s1 * (int32_t)frac) >> 16) : (int16_t)s0;
        }
        for (int k = 0; k < 4; k++) last[k] = ring[(idx + k) & 3];
    }
    t[T_FRAC] = (uint16_t)pos;
    t[T_CUR] = (uint16_t)(s.cur >> 16); t[T_CUR + 1] = (uint16_t)s.cur;
    t[T_YN1] = (uint16_t)s.yn1; t[T_YN2] = (uint16_t)s.yn2; t[T_PS] = s.ps;
}

static void process_voice(uint16_t* pb, int off, int count) {
    if (pb[PB_RUNNING] != 1) return;
    int tail = ax_ver == AXV_OLD ? 58 : 53;
    uint16_t* t = pb + tail;
    int16_t smp[AX_N];
    voice_input(pb, tail, smp, count);

    for (int i = 0; i < count; i++) {  /* volume envelope (unsigned volume on Wii) */
        smp[i] = clamp16(((int32_t)smp[i] * (int32_t)t[T_VOLENV]) >> 15);
        t[T_VOLENV] = (uint16_t)(t[T_VOLENV] + t[T_VOLENV + 1]);
    }
    if (t[T_LPF]) {  /* one-pole low pass: on, yn1, a0, b0 */
        int16_t* f = (int16_t*)&t[T_LPF];
        for (int i = 0; i < count; i++)
            f[1] = smp[i] = clamp16(((int32_t)(uint16_t)f[2] * smp[i] + (int32_t)f[3] * f[1]) >> 15);
    }
    if (ax_ver == AXV_NEW && t[T_BIQUAD]) {  /* on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2 */
        int16_t* f = (int16_t*)&t[T_BIQUAD];
        for (int i = 0; i < count; i++) {
            int64_t acc = (int64_t)f[5] * smp[i] + (int64_t)f[6] * f[1] + (int64_t)f[7] * f[2] +
                          (int64_t)f[8] * f[3] + (int64_t)f[9] * f[4];
            acc <<= 2;
            acc += (acc & 0x10000) ? 0x8000 : 0x7FFF;
            int16_t y = clamp16(acc >> 16);
            f[2] = f[1]; f[4] = f[3]; f[1] = smp[i]; f[3] = y;
            smp[i] = y;
        }
    }
    uint32_t mctl = (uint32_t)pb[PB_MIXCTL] << 16 | pb[PB_MIXCTL + 1];
    for (int b = 0; b < B_COUNT; b++) {
        if (!(mctl & bus_map[b].on)) continue;
        uint16_t* vd = &pb[PB_MIXER + 2 * bus_map[b].mix];
        uint16_t vol = vd[0], delta = (mctl & bus_map[b].rmp) ? vd[1] : 0;
        int16_t s16 = 0;
        for (int i = 0; i < count; i++) {
            s16 = clamp16(((int64_t)smp[i] * vol) >> 15);
            bus[b][off + i] += s16;
            vol = (uint16_t)(vol + delta);
        }
        vd[0] = vol;
        pb[PB_DPOP + bus_map[b].dpop] = (uint16_t)s16;
    }
}

static void ax_process_pbs(uint32_t addr) {
    uint16_t pb[PB_WORDS];
    for (int n = 0; addr && n < 1024; n++) {
        for (int i = 0; i < PB_WORDS; i++) pb[i] = rd16(addr + 2 * i);
        uint16_t* upd = &pb[PB_UPDATES];
        if (ax_ver == AXV_OLD && (upd[0] | upd[1] | upd[2])) {
            /* old AXWii: parameter changes applied at each 1 ms boundary */
            uint32_t data = (uint32_t)upd[3] << 16 | upd[4];
            unsigned first = 0;
            for (int ms = 0; ms < 3; ms++) {
                for (unsigned u = first; u < first + upd[ms] && u < 32; u++) {
                    uint16_t o = rd16(data + 4 * u), v = rd16(data + 4 * u + 2);
                    if (o < PB_WORDS) pb[o] = v;
                }
                first += upd[ms];
                process_voice(pb, ms * 32, 32);
            }
        } else {
            process_voice(pb, 0, AX_N);
        }
        for (int i = 0; i < PB_WORDS; i++) wr16(addr + 2 * i, pb[i]);
        addr = (uint32_t)pb[PB_NEXT] << 16 | pb[PB_NEXT + 1];
    }
}

static void ax_setup(uint32_t addr) {  /* initial value and per-sample delta for each bus */
    for (int b = 0; b < B_COUNT; b++) {
        int32_t v = (int32_t)rd32(addr + 6 * b);
        int16_t d = (int16_t)rd16(addr + 6 * b + 4);
        for (int i = 0; i < AX_N; i++) bus[b][i] = v ? v + i * d : 0;
    }
}

static void ax_mix_aux(int id, uint32_t up, uint32_t down, uint16_t vol) {
    uint16_t r[AX_N];
    ramp(r, last_aux_vol[id], vol, AX_N);
    last_aux_vol[id] = vol;
    int first = B_AL + 3 * id;
    if (up)  /* aux L, R, S go to the CPU for its effect callback ... */
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < AX_N; i++) wr32(up + 4 * (AX_N * c + i), (uint32_t)bus[first + c][i]);
    if (down)  /* ... and its output comes back into main L, R, S */
        for (int c = 0; c < 3; c++)
            for (int i = 0; i < AX_N; i++)
                bus[B_ML + c][i] += (int32_t)(((int64_t)(int32_t)rd32(down + 4 * (AX_N * c + i)) * r[i]) >> 15);
}

static void ax_upload_aux_mix_lrsc(int id, const uint32_t* a, uint16_t vol) {
    int first = id ? B_BL : B_AL;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < AX_N; i++) wr32(a[0] + 4 * (AX_N * c + i), (uint32_t)bus[first + c][i]);
    int cbus = id ? B_CS : B_CR;
    for (int i = 0; i < AX_N; i++) wr32(a[1] + 4 * i, (uint32_t)bus[cbus][i]);
    uint16_t r[AX_N];
    ramp(r, last_aux_vol[id], vol, AX_N);
    last_aux_vol[id] = vol;
    static const int dest[4] = {B_ML, B_MR, B_MS, B_CL};
    for (int m = 0; m < 4; m++)
        for (int i = 0; i < AX_N; i++)
            bus[dest[m]][i] += (int32_t)(((int64_t)(int32_t)rd32(a[2 + m] + 4 * i) * r[i]) >> 15);
}

static void ax_compressor(uint16_t threshold, uint16_t frames, uint32_t table) {
    int hit = 0;
    for (int i = 0; i < AX_N && !hit; i++)
        hit = abs(bus[B_ML][i]) > threshold || abs(bus[B_MR][i]) > threshold;
    uint32_t off;
    if (hit) { off = compressor_pos * AX_N * 2u; compressor_pos = frames; }
    else if (compressor_pos) { compressor_pos--; off = (11u + compressor_pos) * AX_N * 2u; }
    else return;
    for (int i = 0; i < AX_N; i++) {
        uint16_t k = rd16(table + off + 2 * i);
        bus[B_ML][i] = (int32_t)(((int64_t)bus[B_ML][i] * k) >> 15);
        bus[B_MR][i] = (int32_t)(((int64_t)bus[B_MR][i] * k) >> 15);
    }
}

static void ax_output(uint32_t lr, uint32_t surround, uint16_t vol, int dpl2) {
    uint16_t r[AX_N];
    ramp(r, last_main_vol, vol, AX_N);
    last_main_vol = vol;
    for (int i = 0; i < AX_N; i++) wr32(surround + 4 * i, (uint32_t)bus[B_MS][i]);
    if (dpl2)
        for (int i = 0; i < AX_N; i++) wr32(surround + 4 * (AX_N + i), (uint32_t)bus[B_CL][i]);
    for (int i = 0; i < AX_N; i++) {
        int16_t L = clamp16(((int64_t)bus[B_ML][i] * r[i]) >> 15);
        int16_t R = clamp16(((int64_t)bus[B_MR][i] * r[i]) >> 15);
        wr16(lr + 4 * i, (uint16_t)R);
        wr16(lr + 4 * i + 2, (uint16_t)L);
    }
}

static void ax_wm_output(const uint32_t* a) {  /* Wii Remote speakers are not emulated: silence */
    for (int w = 0; w < 4; w++)
        if (a[w]) for (int i = 0; i < 18; i++) wr16(a[w] + 2 * i, 0);
}

static void ax_command_list(uint32_t addr) {
    static int warned;
    uint16_t cl[1024];
    unsigned n = cmdlist_size ? cmdlist_size : 1024;
    if (n > 1024) n = 1024;
    for (unsigned i = 0; i < n; i++) cl[i] = rd16(addr + 2 * i);
#define ARG32(k) ((uint32_t)cl[p + 2 * (k)] << 16 | cl[p + 2 * (k) + 1])
#define NEED(w) if (p + (w) > n) goto bad
    int old = ax_ver == AXV_OLD;
    uint32_t pb_addr = 0;
    for (unsigned p = 0; p < n;) {
        unsigned cmd = cl[p++];
        if (old && cmd >= 4) {
            if (cmd == 4) { NEED(2); pb_addr = ARG32(0); p += 2; continue; }  /* PB_ADDR */
            if (cmd == 5) { ax_process_pbs(pb_addr); continue; }               /* PROCESS */
            cmd--;  /* the rest line up with the new numbering, one higher */
            if (cmd == 0x0B) {  /* OUTPUT without a volume */
                NEED(4); ax_output(ARG32(1), ARG32(0), 0x8000, 0); p += 4; continue;
            }
            if (cmd == 0x0C) { NEED(4); ax_output(ARG32(1), ARG32(0), 0x8000, 1); p += 4; continue; }
        }
        switch (cmd) {
        case 0x00: NEED(2); ax_setup(ARG32(0)); p += 2; break;
        case 0x01: case 0x02: {  /* ADD_TO_LR / SUB_TO_LR */
            NEED(2);
            uint32_t a = ARG32(0);
            for (int i = 0; i < AX_N; i++) {
                int32_t v = (int32_t)rd32(a + 4 * i);
                if (cmd == 2) v = -v;
                bus[B_ML][i] += v; bus[B_MR][i] += v;
            }
            p += 2;
            break;
        }
        case 0x03: {  /* ADD_SUB_TO_LR */
            NEED(2);
            uint32_t a = ARG32(0);
            for (int i = 0; i < AX_N; i++) bus[B_ML][i] += (int32_t)rd32(a + 4 * i);
            for (int i = 0; i < AX_N; i++) bus[B_MR][i] -= (int32_t)rd32(a + 4 * (AX_N + i));
            p += 2;
            break;
        }
        case 0x04: NEED(2); ax_process_pbs(ARG32(0)); p += 2; break;
        case 0x05: case 0x06: case 0x07: {  /* MIX_AUXA/B/C: volume, upload addr, download addr */
            NEED(5);
            uint16_t vol = cl[p++];
            ax_mix_aux((int)cmd - 5, ARG32(0), ARG32(1), vol);
            p += 4;
            break;
        }
        case 0x08: case 0x09: {  /* UPL_AUXA/B_MIX_LRSC */
            NEED(13);
            uint16_t vol = cl[p++];
            uint32_t a[6];
            for (int k = 0; k < 6; k++) a[k] = ARG32(k);
            ax_upload_aux_mix_lrsc(cmd == 9, a, vol);
            p += 12;
            break;
        }
        case 0x0A: {  /* COMPRESSOR: threshold, release frames, table */
            NEED(4);
            ax_compressor(cl[p], cl[p + 1], (uint32_t)cl[p + 2] << 16 | cl[p + 3]);
            p += 4;
            break;
        }
        case 0x0B: case 0x0C: {  /* OUTPUT / OUTPUT_DPL2: volume, surround addr, LR addr */
            uint16_t vol = 0x8000;
            if (ucode_crc != 0xd9c4bf34u) { NEED(1); vol = cl[p++]; }
            NEED(4);
            ax_output(ARG32(1), ARG32(0), vol, cmd == 0x0C);
            p += 4;
            break;
        }
        case 0x0D: {
            NEED(8);
            uint32_t a[4];
            for (int k = 0; k < 4; k++) a[k] = ARG32(k);
            ax_wm_output(a);
            p += 8;
            break;
        }
        case 0x0E: return;  /* END */
        default: goto bad;
        }
    }
    return;
bad:
    if (!warned++) fprintf(stderr, "[dsp] AX command list at %08X: unknown command or overrun\n", addr);
#undef ARG32
#undef NEED
}

/* The microcode the game boots is told apart by Dolphin's hash of its IRAM image. */
static void identify_ucode(uint32_t addr, uint32_t len) {
    uint32_t crc = 0, p = addr & 0x1FFFFFFFu;
    if (!ax_ok(p, len)) return;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= g_mem[p + i];
        crc = (crc << 3) | (crc >> 29);
    }
    ucode_crc = crc;
    switch (crc) {
    case 0xfa450138u: case 0x7699af32u: ax_ver = AXV_OLD; break;
    case 0xd9c4bf34u: case 0xadbc06bdu: ax_ver = AXV_MID; break;
    default: ax_ver = AXV_NEW; break;  /* 0x347112ba, 0x4cc52064 and later builds */
    }
    fprintf(stderr, "[dsp] microcode %08X bytes at %08X, crc %08X, AXWii layout %d\n", len, addr, crc, ax_ver);
    last_main_vol = 0x8000;
    for (int i = 0; i < 3; i++) last_aux_vol[i] = 0x8000;
    compressor_pos = 0;
}

/* ------------------------------------------------------------------ the microcode stand-in */
enum { UC_ROM, UC_TASK };
static int ucode = UC_ROM;
static int rom_hello_sent;
static uint32_t rom_expect;  /* the boot mail whose argument comes next */
static uint32_t boot_iram_addr, boot_iram_len;
static int next_is_cmdlist, upload_steps;

static void cpu_mail(uint32_t m) {
    if (trace) fprintf(stderr, "[dsp] <- %08X\n", m);
    if (ucode == UC_ROM) {
        if (rom_expect) {
            uint32_t what = rom_expect;
            rom_expect = 0;
            if (what == 0x80F3A001u) boot_iram_addr = m;
            if (what == 0x80F3A002u) boot_iram_len = m & 0xFFFF;
            if (what == 0x80F3D001u) {  /* init vector: the task starts and says hello */
                identify_ucode(boot_iram_addr, boot_iram_len);
                ucode = UC_TASK;
                next_is_cmdlist = upload_steps = 0;
                push_mail(DSP_INIT, 1);
            }
            return;
        }
        if ((m & 0xFFFF0000u) == 0x80F30000u) rom_expect = m;
        return;
    }
    if (upload_steps) {          /* MAIL_NEW_UCODE: iram addr/len/dest, dram len, init vec */
        if (++upload_steps > 5) {
            upload_steps = 0;
            push_mail(DSP_INIT, 1);
        }
        return;
    }
    if (next_is_cmdlist) {  /* the command list address: mix the frame, then yield */
        next_is_cmdlist = 0;
        ax_command_list(m);
        push_mail(DSP_YIELD, 1);
        return;
    }
    if (m == MAIL_RESUME) push_mail(DSP_RESUME, 1);
    else if (m == MAIL_NEW_UCODE) upload_steps = 1;
    else if (m == MAIL_RESET) { ucode = UC_ROM; rom_hello_sent = 0; }
    else if (m == MAIL_CONTINUE) {}
    else if ((m & 0xFFFF0000u) == 0xBABE0000u) { next_is_cmdlist = 1; cmdlist_size = (uint16_t)m; }
}

/* ------------------------------------------------------------------ AI DMA timing */
static SDL_AtomicInt aid_running;
static uint64_t aid_next_ns;

void audio_play_ai(const uint8_t* samples, uint32_t bytes, uint32_t rate);  /* platform_audio.c */

/* Called from the timer thread: each AI DMA block "starts" one period after the last, which
 * is when the hardware latches the start address and length, raises AIDINT and the game
 * queues the next buffer. The latched buffer (s16 pairs, right first, big endian) is played. */
void dsp_ai_tick(uint64_t now) {
    if (!SDL_GetAtomicInt(&aid_running)) return;
    uint32_t blocks = regs[0x36 / 2] & 0x7FFF;
    if (!blocks) return;
    if (now < aid_next_ns) return;
    uint32_t rate = hw_ai_dma_rate();
    uint64_t period = (uint64_t)blocks * 32 / 4 * 1000000000ull / rate;
    aid_next_ns = (now - aid_next_ns > period * 4) ? now + period : aid_next_ns + period;
    uint32_t src = ((uint32_t)(regs[0x30 / 2] & 0x1FFF) << 16 | (regs[0x32 / 2] & 0xFFE0)), len = blocks * 32;
    if (ax_ok(src, len)) audio_play_ai(g_mem + src, len, rate);
    cr_set(CR_AIDINT);
}

/* ------------------------------------------------------------------ registers */
uint32_t dsp_read(uint32_t a, int size) {
    uint32_t r = (a & 0x7F) >> 1;
    if (trace < 0) trace = getenv("WPR_TRACE_DSP") != NULL;
    switch (r) {
    case 0x00 / 2:  /* the DSP takes each mail at once: never pending */
        return size == 4 ? (((uint32_t)regs[0] & 0x7FFF) << 16) | regs[1] : regs[0] & 0x7FFFu;
    case 0x04 / 2: {
        if (out_r == out_w) return 0;
        uint32_t m = outq[out_r % 16];
        if (size == 4) { out_r++; return m | 0x80000000u; }
        return (m >> 16) | 0x8000u;
    }
    case 0x06 / 2: {  /* reading the low half takes the mail */
        if (out_r == out_w) return 0;
        return outq[out_r++ % 16] & 0xFFFF;
    }
    case 0x0A / 2: return regs[r] & ~0x0E01u;  /* reset, DMA busy and init always done */
    case 0x3A / 2: return 0;
    }
    if (size == 4) return ((uint32_t)regs[r] << 16) | regs[r + 1];
    return regs[r];
}

static void write16(uint32_t r, uint16_t v) {
    switch (r) {
    case 0x02 / 2:
        regs[r] = v;
        cpu_mail(((uint32_t)regs[0] << 16) | v);
        return;
    case 0x0A / 2:
        SDL_LockSpinlock(&lock);
        if (v & CR_RES) { ucode = UC_ROM; rom_hello_sent = 0; rom_expect = 0; out_r = out_w; }
        regs[r] = (uint16_t)((regs[r] & CR_INTS & ~(v & CR_INTS)) | (v & ~CR_INTS));
        update_irq();
        SDL_UnlockSpinlock(&lock);
        /* out of halt after a reset, the ROM greets the CPU */
        if (!(v & CR_HALT) && ucode == UC_ROM && !rom_hello_sent) {
            rom_hello_sent = 1;
            push_mail(0x8071FEEDu, 0);
        }
        return;
    case 0x2A / 2:  /* ARAM DMA length: the copy is done at once */
        regs[r] = v;
        cr_set(CR_ARINT);
        return;
    case 0x36 / 2: {
        int run = (v & 0x8000) != 0;
        if (run && !(regs[r] & 0x8000)) aid_next_ns = SDL_GetTicksNS();
        regs[r] = v;
        SDL_SetAtomicInt(&aid_running, run);
        return;
    }
    }
    regs[r] = v;
}

void dsp_write(uint32_t a, uint32_t v, int size) {
    uint32_t r = (a & 0x7F) >> 1;
    if (size == 4) { write16(r, (uint16_t)(v >> 16)); write16(r + 1, (uint16_t)v); return; }
    write16(r, (uint16_t)v);
}
