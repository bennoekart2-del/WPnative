/* Guest CPU state and the helpers that recompiled code calls.
 * Recompiled functions are plain C: void f_XXXXXXXX(PPCContext* c).
 */
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef union { double d; uint64_t u; } PPCFpr;

typedef struct PPCContext {
    uint32_t r[32];
    PPCFpr f[32];        /* FPR / paired-single slot 0 */
    double ps1[32];      /* paired-single slot 1 */
    uint8_t cr[8];       /* each field: bit3 LT, bit2 GT, bit1 EQ, bit0 SO */
    uint32_t lr, ctr;
    uint32_t xer_so, xer_ov, xer_ca, xer_bc;
    uint32_t fpscr, msr;
    uint32_t gqr[8];
    uint32_t spr[1024];
} PPCContext;

typedef void (*PPCFunc)(PPCContext*);
typedef struct { uint32_t addr; PPCFunc fn; } PPCFuncEntry;

/* ------------------------------------------------------------------ memory
 * MEM1 (24 MB, phys 0x00000000) and MEM2 (64 MB, phys 0x10000000) are mapped at
 * 0x80000000/0xC0000000 and 0x90000000/0xD0000000. Masking with 0x1FFFFFFF gives the physical
 * address for all four views. 0xCC/0xCD xxxxxx are hardware registers, routed to the runtime.
 */
extern uint8_t* g_mem;                         /* host base of physical memory */
uint32_t mmio_read(uint32_t addr, int size);
void mmio_write(uint32_t addr, uint32_t val, int size);

#define PPC_IS_MMIO(a) (((a) >> 25) == (0xCC000000u >> 25))
#define PPC_PHYS(a) ((a) & 0x1FFFFFFFu)

#if defined(_MSC_VER)
#include <stdlib.h>
#define ppc_bswap16(x) _byteswap_ushort(x)
#define ppc_bswap32(x) _byteswap_ulong(x)
#define ppc_bswap64(x) _byteswap_uint64(x)
#else
#define ppc_bswap16(x) __builtin_bswap16(x)
#define ppc_bswap32(x) __builtin_bswap32(x)
#define ppc_bswap64(x) __builtin_bswap64(x)
#endif

static inline uint32_t mem_r8(uint32_t a) {
    if (PPC_IS_MMIO(a)) return mmio_read(a, 1);
    return g_mem[PPC_PHYS(a)];
}
static inline uint32_t mem_r16(uint32_t a) {
    uint16_t v;
    if (PPC_IS_MMIO(a)) return mmio_read(a, 2);
    memcpy(&v, g_mem + PPC_PHYS(a), 2);
    return ppc_bswap16(v);
}
static inline uint32_t mem_r32(uint32_t a) {
    uint32_t v;
    if (PPC_IS_MMIO(a)) return mmio_read(a, 4);
    memcpy(&v, g_mem + PPC_PHYS(a), 4);
    return ppc_bswap32(v);
}
static inline uint64_t mem_r64(uint32_t a) {
    uint64_t v;
    memcpy(&v, g_mem + PPC_PHYS(a), 8);
    return ppc_bswap64(v);
}
static inline void mem_w8(uint32_t a, uint32_t v) {
    if (PPC_IS_MMIO(a)) { mmio_write(a, v & 0xFF, 1); return; }
    g_mem[PPC_PHYS(a)] = (uint8_t)v;
}
static inline void mem_w16(uint32_t a, uint32_t v) {
    uint16_t t = ppc_bswap16((uint16_t)v);
    if (PPC_IS_MMIO(a)) { mmio_write(a, v & 0xFFFF, 2); return; }
    memcpy(g_mem + PPC_PHYS(a), &t, 2);
}
static inline void mem_w32(uint32_t a, uint32_t v) {
    uint32_t t = ppc_bswap32(v);
    if (PPC_IS_MMIO(a)) { mmio_write(a, v, 4); return; }
    memcpy(g_mem + PPC_PHYS(a), &t, 4);
}
static inline void mem_w64(uint32_t a, uint64_t v) {
    uint64_t t = ppc_bswap64(v);
    if (PPC_IS_MMIO(a)) { mmio_write(a, (uint32_t)(v >> 32), 4); mmio_write(a, (uint32_t)v, 4); return; }
    memcpy(g_mem + PPC_PHYS(a), &t, 8);
}
static inline void mem_wf32(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); mem_w32(a, u); }
static inline float mem_rf32(uint32_t a) { uint32_t u = mem_r32(a); float f; memcpy(&f, &u, 4); return f; }

/* ------------------------------------------------------------------ integer helpers */
static inline uint32_t ppc_rotl(uint32_t v, uint32_t n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }
static inline uint32_t ppc_cntlzw(uint32_t v) { uint32_t n = 0; if (!v) return 32; while (!(v & 0x80000000u)) { v <<= 1; n++; } return n; }
static inline uint32_t ppc_divw(uint32_t a, uint32_t b) {
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    if (sb == 0 || (sa == INT32_MIN && sb == -1)) return sa < 0 ? 0xFFFFFFFFu : 0;
    return (uint32_t)(sa / sb);
}
static inline uint32_t ppc_sraw_impl(uint32_t* ca, uint32_t v, uint32_t n) {
    int32_t s = (int32_t)v;
    if (n > 31) { *ca = s < 0; return s < 0 ? 0xFFFFFFFFu : 0; }
    *ca = (s < 0) && n && (v & ((1u << n) - 1));
    return (uint32_t)(s >> n);
}
#define ppc_sraw(c, v, n) ppc_sraw_impl(&(c)->xer_ca, (v), (n))

static inline void ppc_set_cr0(PPCContext* c, int32_t v) {
    c->cr[0] = (uint8_t)((v < 0 ? 8 : v > 0 ? 4 : 2) | c->xer_so);
}
static inline void ppc_cmps(PPCContext* c, int f, int32_t a, int32_t b) {
    c->cr[f] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c->xer_so);
}
static inline void ppc_cmpu(PPCContext* c, int f, uint32_t a, uint32_t b) {
    c->cr[f] = (uint8_t)((a < b ? 8 : a > b ? 4 : 2) | c->xer_so);
}
static inline uint32_t ppc_crbit(PPCContext* c, int n) { return (c->cr[n >> 2] >> (3 - (n & 3))) & 1; }
static inline void ppc_set_crbit(PPCContext* c, int n, uint32_t v) {
    uint8_t m = (uint8_t)(1u << (3 - (n & 3)));
    c->cr[n >> 2] = (uint8_t)(v ? (c->cr[n >> 2] | m) : (c->cr[n >> 2] & ~m));
}
static inline uint32_t ppc_get_cr(PPCContext* c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)c->cr[i] << (28 - 4 * i);
    return v;
}
static inline void ppc_mtcrf(PPCContext* c, uint32_t crm, uint32_t v) {
    for (int i = 0; i < 8; i++)
        if (crm & (0x80u >> i)) c->cr[i] = (v >> (28 - 4 * i)) & 0xF;
}

/* ------------------------------------------------------------------ floating point */
static inline void ppc_set_ps(PPCContext* c, int d, double v) { c->f[d].d = v; c->ps1[d] = v; }
static inline void ppc_lfs(PPCContext* c, int d, uint32_t ea) { ppc_set_ps(c, d, (double)mem_rf32(ea)); }
static inline void ppc_fcmp(PPCContext* c, int f, double a, double b) {
    c->cr[f] = (uint8_t)(a < b ? 8 : a > b ? 4 : a == b ? 2 : 1);
}
static inline void ppc_set_cr1(PPCContext* c) { c->cr[1] = (uint8_t)(c->fpscr >> 28); }
static inline int32_t ppc_fctiwz(double v) {
    if (v != v) return INT32_MIN;
    if (v >= 2147483647.0) return INT32_MAX;
    if (v <= -2147483648.0) return INT32_MIN;
    return (int32_t)v;
}
static inline int32_t ppc_fctiw(double v) { return ppc_fctiwz(nearbyint(v)); }
static inline void ppc_dcbz(uint32_t ea) { memset(g_mem + PPC_PHYS(ea & ~31u), 0, 32); }

/* ------------------------------------------------------------------ runtime services */
void ppc_psq_l(PPCContext* c, int d, uint32_t ea, int w, int q);
void ppc_psq_st(PPCContext* c, int s, uint32_t ea, int w, int q);
uint32_t ppc_mfspr(PPCContext* c, uint32_t spr);
void ppc_mtspr(PPCContext* c, uint32_t spr, uint32_t v);
uint32_t ppc_mftb(PPCContext* c, uint32_t spr);
void ppc_mtmsr(PPCContext* c, uint32_t v);
void ppc_syscall(PPCContext* c, uint32_t addr);
void ppc_call_indirect(PPCContext* c, uint32_t addr);
void ppc_unimpl(PPCContext* c, uint32_t addr, uint32_t word);

#ifdef __cplusplus
}
#endif
