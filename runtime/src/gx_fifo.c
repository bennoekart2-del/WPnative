/* GX command stream decoder: frames the FIFO into CP/XF/BP register writes, display-list calls and
 * primitive draws. Vertices are fully decoded (direct and indexed attributes, per the VCD/VAT) and
 * handed to the renderer in gx_render.c. Bit layouts follow Dolphin's VideoCommon/CPMemory.h and
 * VertexLoader, and YAGCD chapter 5.
 *
 * Decoding runs on its own host thread, the way the GPU runs beside the CPU on hardware (and in
 * Dolphin's dual-core mode): the guest only appends to a ring buffer and goes on, so time it would
 * spend rasterizing is left to the game's lower-priority threads (file decompression, for one).
 * The game synchronises with the GPU the usual way, through PE draw-done and token interrupts,
 * which the GPU thread raises when it reaches those commands. WPR_GPU_SYNC=1 decodes on the
 * writing thread instead, for debugging.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "gx_internal.h"
#include "ppc_context.h"

static uint8_t fifo[1 << 20];
static size_t fifo_len;
static GxStats stats;

static uint32_t vcd_lo, vcd_hi, vat_a[8], vat_b[8], vat_c[8];
uint32_t g_konst[4][2];

const GxStats* gx_stats(void) { return &stats; }

static uint32_t field(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

static int comp_size(uint32_t fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }

static int color_size(uint32_t fmt) {
    static const int s[8] = {2, 3, 4, 2, 3, 4, 4, 4};
    return s[fmt & 7];
}

static int attr_size(uint32_t type, int direct_size) {
    switch (type) {
    case 1: return direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}

typedef struct { uint32_t cnt, fmt, frac; } TexFmt;

static TexFmt tex_fmt(int vat, int t) {
    uint32_t a = vat_a[vat], b = vat_b[vat], c = vat_c[vat];
    switch (t) {
    case 0: return (TexFmt){field(a, 21, 1), field(a, 22, 3), field(a, 25, 5)};
    case 1: return (TexFmt){field(b, 0, 1), field(b, 1, 3), field(b, 4, 5)};
    case 2: return (TexFmt){field(b, 9, 1), field(b, 10, 3), field(b, 13, 5)};
    case 3: return (TexFmt){field(b, 18, 1), field(b, 19, 3), field(b, 22, 5)};
    case 4: return (TexFmt){field(b, 27, 1), field(b, 28, 3), field(c, 0, 5)};
    case 5: return (TexFmt){field(c, 5, 1), field(c, 6, 3), field(c, 9, 5)};
    case 6: return (TexFmt){field(c, 14, 1), field(c, 15, 3), field(c, 18, 5)};
    default: return (TexFmt){field(c, 23, 1), field(c, 24, 3), field(c, 27, 5)};
    }
}

int gx_vertex_size(int vat) {
    uint32_t a = vat_a[vat];
    int size = 0;
    for (int i = 0; i < 9; i++) size += field(vcd_lo, i, 1); /* PNMTXIDX + TEX0..7MTXIDX, 1 byte each */
    size += attr_size(field(vcd_lo, 9, 2), (field(a, 0, 1) ? 3 : 2) * comp_size(field(a, 1, 3)));
    {
        uint32_t ntype = field(vcd_lo, 11, 2), nbt = field(a, 9, 1), idx3 = field(a, 31, 1);
        int comps = nbt ? 9 : 3;
        int s = attr_size(ntype, comps * comp_size(field(a, 10, 3)));
        if (ntype >= 2 && nbt && idx3) s *= 3;
        size += s;
    }
    size += attr_size(field(vcd_lo, 13, 2), color_size(field(a, 14, 3)));
    size += attr_size(field(vcd_lo, 15, 2), color_size(field(a, 18, 3)));
    for (int t = 0; t < 8; t++) {
        TexFmt f = tex_fmt(vat, t);
        size += attr_size(field(vcd_hi, t * 2, 2), (f.cnt ? 2 : 1) * comp_size(f.fmt));
    }
    return size;
}

/* ------------------------------------------------------------------ vertex decoding */
static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static float read_comp(const uint8_t** pp, uint32_t fmt, uint32_t frac) {
    const uint8_t* p = *pp;
    float v;
    switch (fmt) {
    case 0: v = (float)p[0]; *pp += 1; break;
    case 1: v = (float)(int8_t)p[0]; *pp += 1; break;
    case 2: v = (float)rd16(p); *pp += 2; break;
    case 3: v = (float)(int16_t)rd16(p); *pp += 2; break;
    default: { uint32_t u = rd32(p); memcpy(&v, &u, 4); *pp += 4; return v; }
    }
    return v / (float)(1u << frac);
}

static void read_color(const uint8_t* p, uint32_t fmt, float out[4]) {
    switch (fmt) {
    case 0: { uint16_t v = rd16(p);
        out[0] = (float)(((v >> 11) << 3) | (v >> 13)); out[1] = (float)((((v >> 5) & 63) << 2) | ((v >> 9) & 3));
        out[2] = (float)(((v & 31) << 3) | ((v >> 2) & 7)); out[3] = 255; break; }
    case 1: case 2: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255; break;
    case 3: { uint16_t v = rd16(p);
        out[0] = (float)(((v >> 12) & 15) * 17); out[1] = (float)(((v >> 8) & 15) * 17);
        out[2] = (float)(((v >> 4) & 15) * 17); out[3] = (float)((v & 15) * 17); break; }
    case 4: { uint32_t v = (uint32_t)p[0] << 16 | p[1] << 8 | p[2];
        for (int i = 0; i < 4; i++) { uint32_t c = (v >> (18 - 6 * i)) & 63; out[i] = (float)((c << 2) | (c >> 4)); }
        break; }
    default: out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3]; break;
    }
}

/* Returns a pointer to the attribute data: inline in the stream, or in the indexed array. */
static const uint8_t* attr_ptr(const uint8_t** pp, uint32_t type, int array, int direct_size) {
    const uint8_t* p = *pp;
    if (type == 1) { *pp += direct_size; return p; }
    uint32_t idx;
    if (type == 2) { idx = p[0]; *pp += 1; }
    else { idx = rd16(p); *pp += 2; }
    return g_mem + PPC_PHYS(0x80000000u | (g_cp_array_base[array] + idx * g_cp_array_stride[array]));
}

static void decode_vertex(const uint8_t** pp, int vat, GxRawVertex* v) {
    uint32_t a = vat_a[vat];
    memset(v, 0, sizeof *v);
    const uint8_t* p = *pp;
    if (field(vcd_lo, 0, 1)) { v->has_pnmtx = 1; v->pnmtx = *p++ & 63; }
    for (int t = 0; t < 8; t++)
        if (field(vcd_lo, 1 + t, 1)) { v->has_tmtx[t] = 1; v->tmtx[t] = *p++ & 63; }
    uint32_t ptype = field(vcd_lo, 9, 2);
    if (ptype) {
        int n = field(a, 0, 1) ? 3 : 2;
        uint32_t fmt = field(a, 1, 3), frac = field(a, 4, 5);
        const uint8_t* q = attr_ptr(&p, ptype, 0, n * comp_size(fmt));
        for (int i = 0; i < n; i++) v->pos[i] = read_comp(&q, fmt, frac);
    }
    uint32_t ntype = field(vcd_lo, 11, 2);
    if (ntype) {
        uint32_t nbt = field(a, 9, 1), idx3 = field(a, 31, 1);
        int s = (nbt ? 9 : 3) * comp_size(field(a, 10, 3));
        if (ntype == 1) p += s;
        else p += (ntype == 2 ? 1 : 2) * ((nbt && idx3) ? 3 : 1);
    }
    for (int ch = 0; ch < 2; ch++) {
        uint32_t ctype = field(vcd_lo, 13 + 2 * ch, 2);
        if (!ctype) continue;
        uint32_t fmt = field(a, 14 + 4 * ch, 3);
        const uint8_t* q = attr_ptr(&p, ctype, 2 + ch, color_size(fmt));
        read_color(q, fmt, v->col[ch]);
        v->has_col[ch] = 1;
    }
    for (int t = 0; t < 8; t++) {
        uint32_t ttype = field(vcd_hi, 2 * t, 2);
        if (!ttype) continue;
        TexFmt f = tex_fmt(vat, t);
        int n = f.cnt ? 2 : 1;
        const uint8_t* q = attr_ptr(&p, ttype, 4 + t, n * comp_size(f.fmt));
        for (int i = 0; i < n; i++) v->tex[t][i] = read_comp(&q, f.fmt, f.frac);
    }
    *pp = p;
}

static void draw(uint8_t cmd, const uint8_t* data, int n) {
    int vat = cmd & 7, prim = cmd & 0xF8;
    static GxPipeVertex pv[0x10000];
    for (int i = 0; i < n; i++) {
        GxRawVertex rv;
        decode_vertex(&data, vat, &rv);
        gx_vertex(&rv, &pv[i]);
    }
    switch (prim) {
    case 0x80: case 0x88: /* quads */
        for (int i = 0; i + 3 < n; i += 4) { gx_triangle(&pv[i], &pv[i + 1], &pv[i + 2]); gx_triangle(&pv[i], &pv[i + 2], &pv[i + 3]); }
        break;
    case 0x90:
        for (int i = 0; i + 2 < n; i += 3) gx_triangle(&pv[i], &pv[i + 1], &pv[i + 2]);
        break;
    case 0x98:
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) gx_triangle(&pv[i + 1], &pv[i], &pv[i + 2]);
            else gx_triangle(&pv[i], &pv[i + 1], &pv[i + 2]);
        }
        break;
    case 0xA0:
        for (int i = 1; i + 1 < n; i++) gx_triangle(&pv[0], &pv[i], &pv[i + 1]);
        break;
    default: break; /* lines and points: not needed for the boot screens yet */
    }
}

/* ------------------------------------------------------------------ register loads */
static void load_cp(uint8_t reg, uint32_t v) {
    stats.cp_writes++;
    if (reg >= 0x30 && reg < 0x38) g_cp_matidx_a = v;
    else if (reg >= 0x40 && reg < 0x48) g_cp_matidx_b = v;
    else if (reg >= 0x50 && reg < 0x58) vcd_lo = v;
    else if (reg >= 0x60 && reg < 0x68) vcd_hi = v;
    else if (reg >= 0x70 && reg < 0x78) vat_a[reg & 7] = v;
    else if (reg >= 0x80 && reg < 0x88) vat_b[reg & 7] = v;
    else if (reg >= 0x90 && reg < 0x98) vat_c[reg & 7] = v;
    else if (reg >= 0xA0 && reg < 0xB0) g_cp_array_base[reg & 15] = v & 0x3FFFFFFF;
    else if (reg >= 0xB0 && reg < 0xC0) g_cp_array_stride[reg & 15] = v & 0xFF;
}

static void load_bp(uint32_t v) {
    uint8_t reg = (uint8_t)(v >> 24);
    uint32_t val = v & 0xFFFFFF;
    stats.bp_writes++;
    if (reg == 0xFE) { g_bp[0xFE] = val; return; }  /* write mask for the next BP load */
    uint32_t mask = g_bp[0xFE] ? g_bp[0xFE] : 0xFFFFFF;
    g_bp[0xFE] = 0;
    if (reg >= 0xE0 && reg < 0xE8 && (val & 0x800000)) {
        g_konst[(reg - 0xE0) >> 1][reg & 1] = val;
        return;
    }
    g_bp[reg] = (g_bp[reg] & ~mask) | (val & mask);
    switch (reg) {
    case 0x45: case 0x47: case 0x48: gx_pe_event(reg, val); break;
    case 0x52: stats.efb_copies++; gx_efb_copy(val); break;
    case 0x65: gx_load_tlut(); break;
    case 0x66: gx_invalidate_textures(); break;
    default: break;
    }
}

static void load_indexed_xf(int array, uint32_t v) {
    uint32_t idx = v >> 16, n = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
    uint32_t src = 0x80000000u | (g_cp_array_base[array] + idx * g_cp_array_stride[array]);
    for (uint32_t i = 0; i < n; i++)
        if (addr + i < 0x1100) g_xf[addr + i] = mem_r32(src + 4 * i);
}

/* Decode as many complete commands as `buf` holds; returns bytes consumed. */
static size_t decode(const uint8_t* buf, size_t len, int depth) {
    size_t p = 0;
    while (p < len) {
        uint8_t cmd = buf[p];
        size_t need = 1;
        if (cmd == 0x00 || cmd == 0x48 || cmd == 0x44) { p += 1; continue; }
        if (cmd == 0x08) {
            need = 6;
            if (p + need > len) break;
            load_cp(buf[p + 1], rd32(buf + p + 2));
        } else if (cmd == 0x10) {
            if (p + 5 > len) break;
            uint32_t hdr = rd32(buf + p + 1);
            uint32_t count = (hdr >> 16) + 1, addr = hdr & 0xFFFF;
            need = 5 + 4 * count;
            if (p + need > len) break;
            for (uint32_t i = 0; i < count; i++)
                if (addr + i < 0x1100) g_xf[addr + i] = rd32(buf + p + 5 + 4 * i);
            stats.xf_writes += count;
        } else if ((cmd & 0xE7) == 0x20) {  /* LOAD_INDX_A..D: arrays 12..15 into XF memory */
            need = 5;
            if (p + need > len) break;
            load_indexed_xf(12 + ((cmd >> 3) & 3), rd32(buf + p + 1));
        } else if (cmd == 0x40) {  /* CALL_DL */
            need = 9;
            if (p + need > len) break;
            uint32_t addr = rd32(buf + p + 1), size = rd32(buf + p + 5);
            stats.display_lists++;
            if (depth < 4 && size < 0x1000000) decode(g_mem + PPC_PHYS(0x80000000u | addr), size, depth + 1);
        } else if (cmd == 0x61) {
            need = 5;
            if (p + need > len) break;
            load_bp(rd32(buf + p + 1));
        } else if (cmd & 0x80) {  /* draw: primitive (bits 3-6), vertex format (bits 0-2) */
            if (p + 3 > len) break;
            int vat = cmd & 7;
            uint16_t n = rd16(buf + p + 1);
            need = 3 + (size_t)n * gx_vertex_size(vat);
            if (p + need > len) break;
            stats.draws++;
            stats.vertices += n;
            draw(cmd, buf + p + 3, n);
        } else {
            stats.unknown++;
            if (stats.unknown < 16) fprintf(stderr, "[gx] unknown FIFO opcode %02X\n", cmd);
        }
        p += need;
    }
    return p;
}

/* ------------------------------------------------------------------ CPU -> GPU ring */
#define RING_SIZE (1u << 23)
static uint8_t ring[RING_SIZE];
static SDL_AtomicInt ring_w, ring_r;  /* free-running byte counters */
static SDL_Semaphore* gpu_wake;
static SDL_AtomicInt gpu_sleeping;
static int gpu_sync = -1;

static void consume(void) {
    size_t used = decode(fifo, fifo_len, 0);
    memmove(fifo, fifo + used, fifo_len - used);
    fifo_len -= used;
}

static int gpu_main(void* arg) {
    (void)arg;
    for (;;) {
        uint32_t w = (uint32_t)SDL_GetAtomicInt(&ring_w), r = (uint32_t)SDL_GetAtomicInt(&ring_r);
        if (w == r) {
            SDL_SetAtomicInt(&gpu_sleeping, 1);
            if ((uint32_t)SDL_GetAtomicInt(&ring_w) == r) SDL_WaitSemaphoreTimeout(gpu_wake, 2);
            SDL_SetAtomicInt(&gpu_sleeping, 0);
            continue;
        }
        while (r != w && fifo_len < sizeof fifo) {
            uint32_t n = w - r, room = (uint32_t)(sizeof fifo - fifo_len), at = r & (RING_SIZE - 1);
            if (n > room) n = room;
            if (n > RING_SIZE - at) n = RING_SIZE - at;
            memcpy(fifo + fifo_len, ring + at, n);
            fifo_len += n;
            r += n;
        }
        SDL_SetAtomicInt(&ring_r, (int)r);
        consume();
    }
    return 0;
}

void gx_flush(void) {
    if (gpu_sync) { consume(); return; }
    while (SDL_GetAtomicInt(&ring_w) != SDL_GetAtomicInt(&ring_r)) SDL_Delay(0);
}

/* The CPU-side FIFO (PI registers 0C base, 10 end, 14 write pointer, bit 29 = wrapped). The
 * write-gather pipe feeds the GPU only while the CPU FIFO is linked to it (CP control bit 4);
 * unlinked, as while GXBeginDisplayList records, the commands land in guest memory at the write
 * pointer, and GXEndDisplayList measures the list from it. */
static uint32_t pi_fifo_base, pi_fifo_end, pi_fifo_wptr;
static uint16_t cp_regs[0x40];

uint32_t gx_pi_fifo_read(uint32_t off) {
    return off == 0x0C ? pi_fifo_base : off == 0x10 ? pi_fifo_end : pi_fifo_wptr;
}
void gx_pi_fifo_write(uint32_t off, uint32_t v) {
    if (off == 0x0C) pi_fifo_base = v & 0x1FFFFFE0u;
    else if (off == 0x10) pi_fifo_end = v & 0x1FFFFFE0u;
    else pi_fifo_wptr = v & 0x1FFFFFE0u;
}

static int fifo_linked(void) { return (cp_regs[0x02 / 2] & 0x10) || !pi_fifo_base; }

static void pi_fifo_advance(uint32_t n) {
    uint32_t p = (pi_fifo_wptr & 0x1FFFFFFFu) + n;
    if (pi_fifo_end && p >= pi_fifo_end + 32) p = pi_fifo_base | 0x20000000u;  /* wrapped */
    else p |= pi_fifo_wptr & 0x20000000u;
    pi_fifo_wptr = p;
}

void gx_fifo_write(uint32_t val, int size) {
    if (!fifo_linked()) {
        for (int i = size - 1; i >= 0; i--) {
            g_mem[pi_fifo_wptr & 0x1FFFFFFFu] = (uint8_t)(val >> (8 * i));
            pi_fifo_advance(1);
        }
        return;
    }
    pi_fifo_advance((uint32_t)size);
    if (gpu_sync < 0) {
        gpu_sync = getenv("WPR_GPU_SYNC") != NULL;
        if (!gpu_sync) {
            gpu_wake = SDL_CreateSemaphore(0);
            SDL_CreateThread(gpu_main, "gpu", NULL);
        }
    }
    if (gpu_sync) {
        if (fifo_len + 4 > sizeof fifo) consume();
        for (int i = size - 1; i >= 0; i--) fifo[fifo_len++] = (uint8_t)(val >> (8 * i));
        consume();
        return;
    }
    uint32_t w = (uint32_t)SDL_GetAtomicInt(&ring_w);
    while (w + (uint32_t)size - (uint32_t)SDL_GetAtomicInt(&ring_r) > RING_SIZE) SDL_Delay(0);  /* GPU behind */
    for (int i = size - 1; i >= 0; i--) ring[(w++) & (RING_SIZE - 1)] = (uint8_t)(val >> (8 * i));
    SDL_SetAtomicInt(&ring_w, (int)w);
    if (SDL_GetAtomicInt(&gpu_sleeping)) SDL_SignalSemaphore(gpu_wake);
}

/* ------------------------------------------------------------------ CP and PE registers */
void irq_raise(uint32_t cause);
void irq_clear(uint32_t cause);
#define PI_CAUSE_PE_TOKEN 0x200u
#define PI_CAUSE_PE_FINISH 0x400u

static uint16_t pe_isr, pe_token;
static SDL_SpinLock pe_lock;  /* pe_isr is set by the GPU thread and acknowledged by the CPU */

uint32_t gx_cp_read(uint32_t addr, int size) {
    (void)size;
    uint32_t a = addr & 0xFFF;
    if (a == 0x00) return 0x0C | 0x02;  /* CP status: read idle, command idle, underflow (FIFO drained) */
    return cp_regs[(a & 0x7F) >> 1];
}
void gx_cp_write(uint32_t addr, uint32_t val, int size) {
    (void)size;
    cp_regs[(addr & 0x7F) >> 1] = (uint16_t)val;
}

static void pe_update_irq(void) {
    if ((pe_isr & 1) && (pe_isr & 4)) irq_raise(PI_CAUSE_PE_TOKEN); else irq_clear(PI_CAUSE_PE_TOKEN);
    if ((pe_isr & 2) && (pe_isr & 8)) irq_raise(PI_CAUSE_PE_FINISH); else irq_clear(PI_CAUSE_PE_FINISH);
}

void gx_pe_event(int reg, uint32_t v) {
    SDL_LockSpinlock(&pe_lock);
    if (reg == 0x45) pe_isr |= 8;
    else { pe_token = (uint16_t)v; if (reg == 0x47) pe_isr |= 4; }
    pe_update_irq();
    SDL_UnlockSpinlock(&pe_lock);
}

uint32_t gx_pe_read(uint32_t addr, int size) {
    (void)size;
    switch (addr & 0xFF) {
    case 0x0A: return pe_isr;
    case 0x0E: return pe_token;
    default: return 0;
    }
}

void gx_pe_write(uint32_t addr, uint32_t val, int size) {
    (void)size;
    if ((addr & 0xFF) == 0x0A) {
        /* bits 0-1 enable, bits 2-3 write 1 to acknowledge */
        SDL_LockSpinlock(&pe_lock);
        pe_isr = (uint16_t)((pe_isr & 0xC & ~(val & 0xC)) | (val & 3));
        pe_update_irq();
        SDL_UnlockSpinlock(&pe_lock);
    }
}
