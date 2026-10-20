/* CPU-side services that recompiled code calls out to: SPRs, quantized paired-single load/store,
 * indirect calls, and diagnostics for untranslated instructions. Behaviour follows Dolphin's
 * Interpreter (Source/Core/Core/PowerPC/Interpreter).
 */
#include <stdio.h>
#include <stdlib.h>

#include "ppc_context.h"

extern const PPCFuncEntry g_recomp_funcs[];
extern const unsigned g_recomp_func_count;

uint8_t* g_mem;
uint64_t g_timebase;  /* advanced by the scheduler; 1 tick = 1/(243 MHz / 4) s on Broadway */

uint64_t host_ticks_ns(void);
void dec_write(uint32_t ticks);
uint32_t dec_read(void);
static uint64_t timebase_now(void);

/* Hooks the HLE layer can install to replace a guest function (OS threads, IOS calls, ...). */
PPCFunc ppc_hle_lookup(uint32_t addr);

static PPCFunc find_func(uint32_t addr) {
    unsigned lo = 0, hi = g_recomp_func_count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (g_recomp_funcs[mid].addr == addr) return g_recomp_funcs[mid].fn;
        if (g_recomp_funcs[mid].addr < addr) lo = mid + 1; else hi = mid;
    }
    return NULL;
}

void ppc_call_indirect(PPCContext* c, uint32_t addr) {
    PPCFunc f = ppc_hle_lookup(addr);
    if (!f) f = find_func(addr);
    if (!f) {
        fprintf(stderr, "[cpu] indirect call to unknown address %08X (lr=%08X)\n", addr, c->lr);
        abort();
    }
    f(c);
}

/* rfi with a physical SRR0 is the OS's "run this in real mode" trick (__OSRealMode): call the routine
 * through its cached mirror. Its own closing rfi (SRR0 = virtual return address) is a plain return. */
void ppc_rfi(PPCContext* c) {
    uint32_t target = c->spr[26];
    c->msr = c->spr[27];
    if (target < 0x80000000u) ppc_call_indirect(c, target | 0x80000000u);
}

void ppc_unimpl(PPCContext* c, uint32_t addr, uint32_t word) {
    fprintf(stderr, "[cpu] untranslated instruction %08X at %08X (lr=%08X)\n", word, addr, c->lr);
    abort();
}

void ppc_syscall(PPCContext* c, uint32_t addr) {
    /* On the Wii the game only uses `sc` for the OS's cache/flush helper; nothing to do natively. */
    (void)c; (void)addr;
}

uint32_t ppc_mfspr(PPCContext* c, uint32_t spr) {
    switch (spr) {
    case 1: return (c->xer_so << 31) | (c->xer_ov << 30) | (c->xer_ca << 29) | c->xer_bc;
    case 8: return c->lr;
    case 9: return c->ctr;
    case 22: return dec_read();
    case 268: return (uint32_t)timebase_now();
    case 269: return (uint32_t)(timebase_now() >> 32);
    default:
        if (spr >= 912 && spr <= 919) return c->gqr[spr - 912];
        return c->spr[spr & 1023];
    }
}

void ppc_mtspr(PPCContext* c, uint32_t spr, uint32_t v) {
    switch (spr) {
    case 1:
        c->xer_so = v >> 31; c->xer_ov = (v >> 30) & 1; c->xer_ca = (v >> 29) & 1; c->xer_bc = v & 0x7F;
        return;
    case 8: c->lr = v; return;
    case 9: c->ctr = v; return;
    case 22: dec_write(v); return;
    case 923: /* DMA_L: locked cache <-> memory DMA, done instantly (Dolphin does the same) */
        c->spr[923] = v & ~2u;
        if (v & 2) {
            uint32_t u = c->spr[922];
            uint32_t mem = u & ~0x1Fu, lc = (v & ~0x1Fu) | 0xE0000000u;
            uint32_t blocks = ((u & 0x1F) << 2) | ((v >> 2) & 3);
            if (!blocks) blocks = 128;
            if (v & 0x10) memcpy(g_mem + PPC_PHYS(lc), g_mem + PPC_PHYS(mem), 32 * blocks);
            else memcpy(g_mem + PPC_PHYS(mem), g_mem + PPC_PHYS(lc), 32 * blocks);
        }
        return;
    case 284: { uint64_t t = timebase_now(); g_timebase += ((t & ~0xFFFFFFFFull) | v) - t; return; }
    case 285: { uint64_t t = timebase_now(); g_timebase += (((uint64_t)v << 32) | (t & 0xFFFFFFFFull)) - t; return; }
    default:
        if (spr >= 912 && spr <= 919) { c->gqr[spr - 912] = v; return; }
        c->spr[spr & 1023] = v;
    }
}

/* The time base runs at bus clock / 4 = 60.75 MHz; drive it from the host clock so the game's
 * busy-wait delays take real time. */
static uint64_t timebase_now(void) {
    return g_timebase + host_ticks_ns() * 243 / 4000;
}

uint32_t ppc_mftb(PPCContext* c, uint32_t spr) {
    (void)c;
    uint64_t t = timebase_now();
    return spr == 269 ? (uint32_t)(t >> 32) : (uint32_t)t;
}

void ppc_mtmsr(PPCContext* c, uint32_t v) {
    c->msr = v;
    if ((v & 0x8000u) && g_irq_pending) ppc_check_irq(c);  /* interrupts just got enabled */
}

/* ------------------------------------------------------------------ quantized paired singles */
static double gqr_scale(int s6) {
    int s = (s6 & 0x20) ? s6 - 64 : s6;
    return ldexp(1.0, s);
}

static double dequant(uint32_t ea, int type, int scale6) {
    double m = 1.0 / gqr_scale(scale6);
    switch (type) {
    case 4: return (double)(uint8_t)mem_r8(ea) * m;
    case 5: return (double)(uint16_t)mem_r16(ea) * m;
    case 6: return (double)(int8_t)mem_r8(ea) * m;
    case 7: return (double)(int16_t)mem_r16(ea) * m;
    default: return (double)mem_rf32(ea);
    }
}

static int type_size(int type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

void ppc_psq_l(PPCContext* c, int d, uint32_t ea, int w, int q) {
    uint32_t g = c->gqr[q];
    int type = (g >> 16) & 7, scale = (g >> 24) & 0x3F;
    c->f[d].d = dequant(ea, type, scale);
    c->ps1[d] = w ? 1.0 : dequant(ea + type_size(type), type, scale);
}

static void quant(uint32_t ea, double v, int type, int scale6) {
    double x = v * gqr_scale(scale6);
    switch (type) {
    case 4: mem_w8(ea, x <= 0 ? 0 : x >= 255 ? 255 : (uint32_t)x); break;
    case 5: mem_w16(ea, x <= 0 ? 0 : x >= 65535 ? 65535 : (uint32_t)x); break;
    case 6: mem_w8(ea, (uint32_t)(int32_t)(x <= -128 ? -128 : x >= 127 ? 127 : (int32_t)x)); break;
    case 7: mem_w16(ea, (uint32_t)(int32_t)(x <= -32768 ? -32768 : x >= 32767 ? 32767 : (int32_t)x)); break;
    default: mem_wf32(ea, (float)v); break;
    }
}

void ppc_psq_st(PPCContext* c, int s, uint32_t ea, int w, int q) {
    uint32_t g = c->gqr[q];
    int type = g & 7, scale = (g >> 8) & 0x3F;
    quant(ea, c->f[s].d, type, scale);
    if (!w) quant(ea + type_size(type), c->ps1[s], type, scale);
}

/* ------------------------------------------------------------------ OSReport: the game's debug log */
static void guest_str(char* dst, size_t n, uint32_t a) {
    size_t i = 0;
    if (a) for (; i + 1 < n; i++) { dst[i] = (char)mem_r8(a + (uint32_t)i); if (!dst[i]) return; }
    dst[i] = 0;
}

void hle_OSReport(PPCContext* c) {
    char fmt[512], out[1024], spec[32], tmp[256];
    guest_str(fmt, sizeof fmt, c->r[3]);
    int gpr = 4, fpr = 1;
    size_t o = 0;
    for (const char* p = fmt; *p && o < sizeof out - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        size_t k = 0;
        spec[k++] = *p++;
        while (*p && strchr("-+ #0123456789.lhz", *p) && k < sizeof spec - 2) spec[k++] = *p++;
        if (!*p) break;
        char conv = *p;
        spec[k++] = conv; spec[k] = 0;
        uint32_t arg = gpr <= 10 ? c->r[gpr] : 0;
        tmp[0] = 0;
        switch (conv) {
        case 's': { char s[200]; guest_str(s, sizeof s, arg); gpr++; snprintf(tmp, sizeof tmp, "%s", s); break; }
        case 'f': case 'g': case 'e': snprintf(tmp, sizeof tmp, spec, fpr <= 8 ? c->f[fpr++].d : 0.0); break;
        case '%': snprintf(tmp, sizeof tmp, "%%"); break;
        default: {
            char s2[32]; size_t j = 0;
            for (size_t i = 0; spec[i]; i++) if (spec[i] != 'l' && spec[i] != 'h' && spec[i] != 'z') s2[j++] = spec[i];
            s2[j] = 0;
            snprintf(tmp, sizeof tmp, s2, arg);
            gpr++;
        }
        }
        for (char* t = tmp; *t && o < sizeof out - 1; t++) out[o++] = *t;
    }
    out[o] = 0;
    fprintf(stderr, "[game] %s%s", out, (o && out[o - 1] == '\n') ? "" : "\n");
}
