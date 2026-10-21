/* Software GX renderer: transforms, rasterizes and shades the draws decoded from the FIFO into an
 * emulated EFB (embedded framebuffer), and performs EFB copies into guest memory (XFB / textures),
 * which is how frames reach the screen. Register layouts follow Dolphin's VideoCommon
 * (BPMemory.h, XFMemory.h, CPMemory.h, TextureDecoder) and YAGCD.
 *
 * This is the correctness-first path to the first frames; a GPU (D3D12/Vulkan) backend replaces it
 * for speed later.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx_gl.h"
#include "gx_internal.h"
#include "ppc_context.h"

#define EFB_W 640
#define EFB_H 528

uint32_t g_xf[0x1100];          /* XF memory + registers (floats stored as raw bits) */
uint32_t g_bp[0x100];
uint32_t g_cp_matidx_a, g_cp_matidx_b, g_cp_array_base[16], g_cp_array_stride[16];

static uint8_t efb[EFB_H][EFB_W][4];
static float zbuf[EFB_H][EFB_W];
static uint8_t tmem[1 << 20];

int g_xfb_w = 640, g_xfb_h = 480;
uint64_t g_draw_count;

static float xff(int a) { float f; memcpy(&f, &g_xf[a], 4); return f; }

static void raster_drain(void);
void gx_raster_dirty(void);

/* ------------------------------------------------------------------ textures */
typedef struct { uint32_t addr, fmt, w, h, tlut; uint32_t* px; } TexCache;
#define TCACHE_MAX 1024
static TexCache tcache[TCACHE_MAX];
static int tcache_n, tcache_flushed, tcache_game_invalidated;

static uint32_t rgba(int r, int g, int b, int a) { return (uint32_t)r | (uint32_t)g << 8 | (uint32_t)b << 16 | (uint32_t)a << 24; }
static int e5(int v) { return (v << 3) | (v >> 2); }
static int e6(int v) { return (v << 2) | (v >> 4); }
static int e4(int v) { return (v << 4) | v; }
static int e3(int v) { return (v << 5) | (v << 2) | (v >> 1); }

static uint32_t from565(uint16_t v) { return rgba(e5(v >> 11), e6((v >> 5) & 63), e5(v & 31), 255); }
static uint32_t from5a3(uint16_t v) {
    if (v & 0x8000) return rgba(e5((v >> 10) & 31), e5((v >> 5) & 31), e5(v & 31), 255);
    return rgba(e4((v >> 8) & 15), e4((v >> 4) & 15), e4(v & 15), e3((v >> 12) & 7));
}
static uint32_t fromIA8(uint16_t v) { int i = v & 0xFF; return rgba(i, i, i, v >> 8); }

static uint32_t tlut_color(uint32_t tlut, uint32_t idx) {
    uint32_t off = (tlut & 0x3FF) << 9, fmt = (tlut >> 10) & 3;
    uint16_t v = (uint16_t)(tmem[(off + idx * 2) & 0xFFFFF] << 8 | tmem[(off + idx * 2 + 1) & 0xFFFFF]);
    return fmt == 0 ? fromIA8(v) : fmt == 1 ? from565(v) : from5a3(v);
}

static void decode_cmpr_block(const uint8_t* s, uint32_t* out, int stride) {
    uint16_t c0 = (uint16_t)(s[0] << 8 | s[1]), c1 = (uint16_t)(s[2] << 8 | s[3]);
    uint32_t p[4];
    p[0] = from565(c0); p[1] = from565(c1);
    int r0 = p[0] & 255, g0 = (p[0] >> 8) & 255, b0 = (p[0] >> 16) & 255;
    int r1 = p[1] & 255, g1 = (p[1] >> 8) & 255, b1 = (p[1] >> 16) & 255;
    if (c0 > c1) {
        p[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        p[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        p[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        p[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);
    }
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) out[y * stride + x] = p[(s[4 + y] >> (6 - 2 * x)) & 3];
}

static uint32_t* decode_texture(uint32_t addr, uint32_t fmt, uint32_t w, uint32_t h, uint32_t tlut) {
    for (int i = 0; i < tcache_n; i++)
        if (tcache[i].addr == addr && tcache[i].fmt == fmt && tcache[i].w == w && tcache[i].h == h && tcache[i].tlut == tlut)
            return tcache[i].px;
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0};
    static const int bh[16] = {8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0};
    if (!bw[fmt & 15]) return NULL;
    int tw = bw[fmt], th = bh[fmt];
    uint32_t W = (w + tw - 1) / tw * tw, H = (h + th - 1) / th * th;
    uint32_t* px = calloc((size_t)W * H, 4);
    const uint8_t* s = g_mem + PPC_PHYS(addr);
    for (uint32_t by = 0; by < H; by += th)
        for (uint32_t bx = 0; bx < W; bx += tw) {
            switch (fmt) {
            case 0: /* I4 */
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x += 2, s++) {
                    int a = e4(*s >> 4), b = e4(*s & 15);
                    px[(by + y) * W + bx + x] = rgba(a, a, a, a);
                    px[(by + y) * W + bx + x + 1] = rgba(b, b, b, b);
                }
                break;
            case 1: /* I8 */
                for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++, s++) px[(by + y) * W + bx + x] = rgba(*s, *s, *s, *s);
                break;
            case 2: /* IA4 */
                for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++, s++) {
                    int i = e4(*s & 15); px[(by + y) * W + bx + x] = rgba(i, i, i, e4(*s >> 4));
                }
                break;
            case 3: /* IA8 */
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++, s += 2) px[(by + y) * W + bx + x] = fromIA8((uint16_t)(s[0] << 8 | s[1]));
                break;
            case 4:
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++, s += 2) px[(by + y) * W + bx + x] = from565((uint16_t)(s[0] << 8 | s[1]));
                break;
            case 5:
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++, s += 2) px[(by + y) * W + bx + x] = from5a3((uint16_t)(s[0] << 8 | s[1]));
                break;
            case 6: /* RGBA8: 32 bytes of AR then 32 bytes of GB */
                for (int i = 0; i < 16; i++) {
                    int a = s[i * 2], r = s[i * 2 + 1], g = s[32 + i * 2], b = s[32 + i * 2 + 1];
                    px[(by + i / 4) * W + bx + i % 4] = rgba(r, g, b, a);
                }
                s += 64;
                break;
            case 8: /* C4 */
                for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x += 2, s++) {
                    px[(by + y) * W + bx + x] = tlut_color(tlut, *s >> 4);
                    px[(by + y) * W + bx + x + 1] = tlut_color(tlut, *s & 15);
                }
                break;
            case 9: /* C8 */
                for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++, s++) px[(by + y) * W + bx + x] = tlut_color(tlut, *s);
                break;
            case 10: /* C14X2 */
                for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++, s += 2) px[(by + y) * W + bx + x] = tlut_color(tlut, (uint32_t)((s[0] << 8 | s[1]) & 0x3FFF));
                break;
            case 14: /* CMPR: 2x2 DXT1 sub-blocks */
                for (int sb = 0; sb < 4; sb++, s += 8)
                    decode_cmpr_block(s, &px[(by + (sb / 2) * 4) * W + bx + (sb % 2) * 4], (int)W);
                break;
            }
        }
    if (tcache_n == TCACHE_MAX) {  /* full: start over (callers holding pointers retry, see build_state) */
        raster_drain();
        for (int i = 0; i < tcache_n; i++) { gx_gl_forget_texture(tcache[i].px); free(tcache[i].px); }
        tcache_n = 0;
        tcache_flushed = 1;
    }
    tcache[tcache_n++] = (TexCache){addr, fmt, w, h, tlut, px};
    /* stride is the padded width; record it via the cache entry's w */
    tcache[tcache_n - 1].w = w;
    return px;
}

void gx_invalidate_textures(void) {
    tcache_game_invalidated = 1;
    if (!tcache_n) return;
    raster_drain();
    for (int i = 0; i < tcache_n; i++) { gx_gl_forget_texture(tcache[i].px); free(tcache[i].px); }
    tcache_n = 0;
    gx_raster_dirty();
}

/* Drops cached textures decoded from guest memory in [lo, hi) (after an EFB copy wrote there),
 * or, with lo == hi == 0, every paletted texture (after a TLUT load). */
static void invalidate_textures_in(uint32_t lo, uint32_t hi) {
    int n = 0, hit = 0;
    for (int i = 0; i < tcache_n; i++) {
        TexCache* t = &tcache[i];
        int pal = t->fmt == 8 || t->fmt == 9 || t->fmt == 10;
        uint32_t a = PPC_PHYS(t->addr), end = a + t->w * t->h * 4 + 4096;  /* generous: covers padding and 32bpp */
        if (lo == hi ? pal : (a < hi && end > lo)) {
            if (!hit) { raster_drain(); hit = 1; }
            gx_gl_forget_texture(t->px);
            free(t->px);
        } else tcache[n++] = *t;
    }
    tcache_n = n;
    if (hit) gx_raster_dirty();
}

typedef GxGlTexMap TexMap;

static int tex_get(int map, TexMap* t) {
    int set = map < 4 ? 0 : 0x20;
    int m = map & 3;
    uint32_t img0 = g_bp[0x88 + m + set], img3 = g_bp[0x94 + m + set], mode0 = g_bp[0x80 + m + set];
    uint32_t tlut = g_bp[0x98 + m + set];
    t->w = (int)(img0 & 0x3FF) + 1;
    t->h = (int)((img0 >> 10) & 0x3FF) + 1;
    uint32_t fmt = (img0 >> 20) & 0xF;
    uint32_t addr = (img3 & 0xFFFFFF) << 5;
    static const int bw[16] = {8, 8, 8, 4, 4, 4, 4, 1, 8, 8, 4, 1, 1, 1, 8, 1};
    t->stride = (t->w + bw[fmt] - 1) / bw[fmt] * bw[fmt];
    t->wrap_s = mode0 & 3;
    t->wrap_t = (mode0 >> 2) & 3;
    t->linear = (mode0 >> 4) & 1;
    t->px = decode_texture(addr | 0x80000000u, fmt, (uint32_t)t->w, (uint32_t)t->h, tlut);
    return t->px != NULL;
}

static int wrap(int v, int n, int mode) {
    if (mode == 1) { v %= n; return v < 0 ? v + n : v; }
    if (mode == 2) { int p = 2 * n; v %= p; if (v < 0) v += p; return v < n ? v : p - 1 - v; }
    return v < 0 ? 0 : v >= n ? n - 1 : v;
}

static void tex_sample(const TexMap* t, float u, float v, int out[4]) {
    float x = u * t->w - 0.5f, y = v * t->h - 0.5f;
    if (!t->linear) {
        int xi = wrap((int)floorf(x + 0.5f), t->w, t->wrap_s), yi = wrap((int)floorf(y + 0.5f), t->h, t->wrap_t);
        uint32_t p = t->px[yi * t->stride + xi];
        out[0] = p & 255; out[1] = (p >> 8) & 255; out[2] = (p >> 16) & 255; out[3] = p >> 24;
        return;
    }
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    float fx = x - x0, fy = y - y0;
    int xa = wrap(x0, t->w, t->wrap_s), xb = wrap(x0 + 1, t->w, t->wrap_s);
    int ya = wrap(y0, t->h, t->wrap_t), yb = wrap(y0 + 1, t->h, t->wrap_t);
    uint32_t p00 = t->px[ya * t->stride + xa], p10 = t->px[ya * t->stride + xb];
    uint32_t p01 = t->px[yb * t->stride + xa], p11 = t->px[yb * t->stride + xb];
    for (int ch = 0; ch < 4; ch++) {
        float a = (float)((p00 >> (8 * ch)) & 255), b = (float)((p10 >> (8 * ch)) & 255);
        float c = (float)((p01 >> (8 * ch)) & 255), d = (float)((p11 >> (8 * ch)) & 255);
        out[ch] = (int)((a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy + 0.5f);
    }
}

/* ------------------------------------------------------------------ TEV */
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

typedef struct { int c[4]; } Col;

typedef GxGlStage Stage;
typedef GxGlState RasterState;

static void tev_konst(int sel, int out[4]) {
    if (sel < 8) { /* 1, 7/8, 3/4, 5/8, 1/2, 3/8, 1/4, 1/8 */
        static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
        out[0] = out[1] = out[2] = out[3] = frac[sel];
        return;
    }
    if (sel < 12) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    int v[4][4];
    for (int k = 0; k < 4; k++) {
        uint32_t ra = g_konst[k][0], bg = g_konst[k][1];
        v[k][0] = ra & 0xFF; v[k][3] = (ra >> 12) & 0xFF; v[k][2] = bg & 0xFF; v[k][1] = (bg >> 12) & 0xFF;
    }
    if (sel < 16) { memcpy(out, v[sel - 12], sizeof v[0]); return; }
    int comp = (sel - 16) >> 2, k = (sel - 16) & 3;
    out[0] = out[1] = out[2] = out[3] = v[k][comp];
}

static void swap_table(int table, int out[4]) {
    uint32_t a = g_bp[0xF6 + table * 2], b = g_bp[0xF7 + table * 2];
    out[0] = a & 3; out[1] = (a >> 2) & 3; out[2] = b & 3; out[3] = (b >> 2) & 3;
}

static void build_state_once(RasterState* rs) {
    uint32_t gen = g_bp[0x00];
    rs->nstages = (int)((gen >> 10) & 15) + 1;
    rs->ntex = (int)(gen & 15);
    rs->ncol = (int)((gen >> 4) & 7);
    rs->alpha_cfg = g_bp[0xF3];
    rs->zmode = g_bp[0x40];
    rs->cmode = g_bp[0x41];
    for (int i = 0; i < 4; i++) {
        uint32_t ra = g_bp[0xE0 + i * 2], bg = g_bp[0xE1 + i * 2];
        int r = ra & 0x7FF, a = (ra >> 12) & 0x7FF, b = bg & 0x7FF, g = (bg >> 12) & 0x7FF;
        if (r & 0x400) r -= 0x800;
        if (a & 0x400) a -= 0x800;
        if (b & 0x400) b -= 0x800;
        if (g & 0x400) g -= 0x800;
        rs->regs[i][0] = r; rs->regs[i][1] = g; rs->regs[i][2] = b; rs->regs[i][3] = a;
    }
    for (int s = 0; s < rs->nstages; s++) {
        Stage* st = &rs->st[s];
        uint32_t tref = g_bp[0x28 + s / 2] >> ((s & 1) * 12);
        int texmap = st->texmap = tref & 7;
        st->texcoord = (tref >> 3) & 7;
        st->texen = ((tref >> 6) & 1) && st->texcoord < rs->ntex && tex_get(texmap, &st->tm);
        st->ras_sel = (tref >> 7) & 7;
        uint32_t ce = g_bp[0xC0 + s * 2], ae = g_bp[0xC1 + s * 2];
        swap_table((int)((ae >> 2) & 3), st->tsw);
        swap_table((int)(ae & 3), st->rsw);
        uint32_t ksel = g_bp[0xF6 + s / 2];
        int kc[4], ka[4];
        tev_konst((int)((ksel >> (s & 1 ? 14 : 4)) & 31), kc);
        tev_konst((int)((ksel >> (s & 1 ? 19 : 9)) & 31), ka);
        st->konst[0] = kc[0]; st->konst[1] = kc[1]; st->konst[2] = kc[2];
        st->kalpha = ka[3];
        for (int i = 0; i < 4; i++) {
            st->csel[i] = (int)((ce >> (12 - 4 * i)) & 15);
            st->asel[i] = (int)((ae >> (13 - 3 * i)) & 7);
        }
        st->c_bias = (int)((ce >> 16) & 3); st->c_sub = (int)((ce >> 18) & 1); st->c_clamp = (int)((ce >> 19) & 1);
        st->c_scale = (int)((ce >> 20) & 3); st->c_dest = (int)((ce >> 22) & 3);
        st->a_bias = (int)((ae >> 16) & 3); st->a_sub = (int)((ae >> 18) & 1); st->a_clamp = (int)((ae >> 19) & 1);
        st->a_scale = (int)((ae >> 20) & 3); st->a_dest = (int)((ae >> 22) & 3);
    }
}

static void build_state(RasterState* rs) {
    do { tcache_flushed = 0; build_state_once(rs); } while (tcache_flushed);
}

static int tev_combine(int A, int B, int C, int D, int bias, int sub, int scale, int clamp) {
    A &= 255; B &= 255; C &= 255;
    int v;
    if (bias == 3) v = D + (A > B ? C : 0);  /* compare modes */
    else {
        int lerp = (A * (256 - (C + (C >> 7))) + B * (C + (C >> 7))) >> 8;
        v = (sub ? D - lerp : D + lerp) + (bias == 1 ? 128 : bias == 2 ? -128 : 0);
        v = scale == 3 ? v / 2 : v << (scale == 1 ? 1 : scale == 2 ? 2 : 0);
    }
    return clamp ? clamp255(v) : (v < -1024 ? -1024 : v > 1023 ? 1023 : v);
}

static void tev_shade(const RasterState* rs, const int ras[2][4], const float tc[8][2], int out[4]) {
    Col r[4];
    memcpy(r, rs->regs, sizeof r);  /* r[0] = PREV, r[1..3] = REG0..2 */
    int tex[4] = {255, 255, 255, 255};
    for (int s = 0; s < rs->nstages; s++) {
        const Stage* st = &rs->st[s];
        if (st->texen) {
            int raw[4];
            tex_sample(&st->tm, tc[st->texcoord][0], tc[st->texcoord][1], raw);
            for (int k = 0; k < 4; k++) tex[k] = raw[st->tsw[k]];
        }
        int rasc[4] = {0, 0, 0, 0};
        if (st->ras_sel <= 1)
            for (int k = 0; k < 4; k++) rasc[k] = ras[st->ras_sel][st->rsw[k]];
        int cin[4][3];
        for (int i = 0; i < 4; i++) {
            int sel = st->csel[i];
            int* d = cin[i];
            switch (sel) {
            case 0: case 2: case 4: case 6: { const Col* x = &r[sel / 2]; d[0] = x->c[0]; d[1] = x->c[1]; d[2] = x->c[2]; break; }
            case 1: case 3: case 5: case 7: { int a = r[sel / 2].c[3]; d[0] = d[1] = d[2] = a; break; }
            case 8: d[0] = tex[0]; d[1] = tex[1]; d[2] = tex[2]; break;
            case 9: d[0] = d[1] = d[2] = tex[3]; break;
            case 10: d[0] = rasc[0]; d[1] = rasc[1]; d[2] = rasc[2]; break;
            case 11: d[0] = d[1] = d[2] = rasc[3]; break;
            case 12: d[0] = d[1] = d[2] = 255; break;
            case 13: d[0] = d[1] = d[2] = 128; break;
            case 14: d[0] = st->konst[0]; d[1] = st->konst[1]; d[2] = st->konst[2]; break;
            default: d[0] = d[1] = d[2] = 0; break;
            }
        }
        int ain[4];
        for (int i = 0; i < 4; i++) {
            int sel = st->asel[i];
            ain[i] = sel < 4 ? r[sel].c[3] : sel == 4 ? tex[3] : sel == 5 ? rasc[3] : sel == 6 ? st->kalpha : 0;
        }
        for (int ch = 0; ch < 3; ch++)
            r[st->c_dest].c[ch] = tev_combine(cin[0][ch], cin[1][ch], cin[2][ch], cin[3][ch],
                                              st->c_bias, st->c_sub, st->c_scale, st->c_clamp);
        r[st->a_dest].c[3] = tev_combine(ain[0], ain[1], ain[2], ain[3], st->a_bias, st->a_sub, st->a_scale, st->a_clamp);
        if (s == rs->nstages - 1) {
            out[0] = clamp255(r[st->c_dest].c[0]); out[1] = clamp255(r[st->c_dest].c[1]);
            out[2] = clamp255(r[st->c_dest].c[2]); out[3] = clamp255(r[st->a_dest].c[3]);
        }
    }
}

static int alpha_cmp(int a, int ref, int fn) {
    switch (fn) {
    case 0: return 0;
    case 1: return a < ref;
    case 2: return a == ref;
    case 3: return a <= ref;
    case 4: return a > ref;
    case 5: return a != ref;
    case 6: return a >= ref;
    default: return 1;
    }
}

static int alpha_test(uint32_t ac, int a) {
    int r0 = alpha_cmp(a, ac & 255, (ac >> 16) & 7), r1 = alpha_cmp(a, (ac >> 8) & 255, (ac >> 19) & 7);
    switch ((ac >> 22) & 3) {
    case 0: return r0 && r1;
    case 1: return r0 || r1;
    case 2: return r0 != r1;
    default: return r0 == r1;
    }
}

static int blend_factor(int f, const int src[4], const int dst[4], int ch, int is_src) {
    switch (f) {
    case 0: return 0;
    case 1: return 255;
    case 2: return is_src ? dst[ch] : src[ch];
    case 3: return 255 - (is_src ? dst[ch] : src[ch]);
    case 4: return src[3];
    case 5: return 255 - src[3];
    case 6: return dst[3];
    default: return 255 - dst[3];
    }
}

static void write_pixel(const RasterState* rs, int x, int y, float z, const int col[4]) {
    uint32_t zmode = rs->zmode;
    if (zmode & 1) {
        float zr = zbuf[y][x];
        int pass;
        switch ((zmode >> 1) & 7) {
        case 0: pass = 0; break;
        case 1: pass = z < zr; break;
        case 2: pass = z == zr; break;
        case 3: pass = z <= zr; break;
        case 4: pass = z > zr; break;
        case 5: pass = z != zr; break;
        case 6: pass = z >= zr; break;
        default: pass = 1; break;
        }
        if (!pass) return;
        if (zmode & 0x10) zbuf[y][x] = z;
    }
    uint32_t cm = rs->cmode;
    uint8_t* d = efb[y][x];
    int out[4];
    if (cm & 1) {
        int dst[4] = {d[0], d[1], d[2], d[3]};
        int sf = (int)((cm >> 8) & 7), df = (int)((cm >> 5) & 7);
        for (int ch = 0; ch < 4; ch++) {
            if (cm & 0x800) out[ch] = clamp255(dst[ch] - col[ch]);
            else out[ch] = clamp255((col[ch] * blend_factor(sf, col, dst, ch, 1) + dst[ch] * blend_factor(df, col, dst, ch, 0)) / 255);
        }
    } else {
        memcpy(out, col, sizeof out);
    }
    if (cm & 8) { d[0] = (uint8_t)out[0]; d[1] = (uint8_t)out[1]; d[2] = (uint8_t)out[2]; }
    if (cm & 0x10) d[3] = (uint8_t)out[3];
}

/* ------------------------------------------------------------------ vertex pipeline */
typedef struct {
    float x, y, z, w;        /* clip space */
    float sx, sy, sz;        /* screen */
    float col[2][4];
    float tc[8][3];
} Vtx;

static void mtx_mul(int base, const float in[3], float out[3], int rows) {
    for (int r = 0; r < rows; r++)
        out[r] = xff(base + r * 4) * in[0] + xff(base + r * 4 + 1) * in[1] + xff(base + r * 4 + 2) * in[2] + xff(base + r * 4 + 3);
}

void gx_vertex(const GxRawVertex* rv, GxPipeVertex* out) {
    float p[3], q[3];
    p[0] = rv->pos[0]; p[1] = rv->pos[1]; p[2] = rv->pos[2];
    int pm = rv->has_pnmtx ? rv->pnmtx : (int)(g_cp_matidx_a & 63);
    mtx_mul(pm * 4, p, q, 3);
    /* projection */
    float a = xff(0x1020), b = xff(0x1021), c = xff(0x1022), d = xff(0x1023), e = xff(0x1024), f = xff(0x1025);
    int ortho = g_xf[0x1026] != 0;
    if (ortho) { out->clip[0] = a * q[0] + b; out->clip[1] = c * q[1] + d; out->clip[2] = e * q[2] + f; out->clip[3] = 1; }
    else { out->clip[0] = a * q[0] + b * q[2]; out->clip[1] = c * q[1] + d * q[2]; out->clip[2] = e * q[2] + f; out->clip[3] = -q[2]; }
    /* colors: material from register or vertex, lighting ignored for now */
    for (int ch = 0; ch < 2; ch++) {
        uint32_t cc = g_xf[0x100E + ch], ac = g_xf[0x1010 + ch];
        uint32_t mat = g_xf[0x100C + ch];
        float vc[4];
        for (int k = 0; k < 4; k++) vc[k] = rv->col[ch][k];
        if (!rv->has_col[ch]) { vc[0] = vc[1] = vc[2] = vc[3] = 255; }
        for (int k = 0; k < 3; k++) out->col[ch][k] = (cc & 1) ? vc[k] : (float)((mat >> (24 - 8 * k)) & 255);
        out->col[ch][3] = (ac & 1) ? vc[3] : (float)(mat & 255);
    }
    /* texgen */
    int ngen = (int)(g_xf[0x103F] & 15);
    for (int t = 0; t < ngen && t < 8; t++) {
        uint32_t g = g_xf[0x1040 + t];
        int proj = (g >> 1) & 1, type = (g >> 4) & 7, src = (g >> 7) & 31;
        float in[3] = {0, 0, 1};
        if (src >= 5 && src <= 12) { in[0] = rv->tex[src - 5][0]; in[1] = rv->tex[src - 5][1]; }
        else if (src == 0) { in[0] = p[0]; in[1] = p[1]; in[2] = p[2]; }
        int tm;
        uint32_t mi = t < 4 ? g_cp_matidx_a : g_cp_matidx_b;
        tm = t < 4 ? (int)((mi >> (6 + 6 * t)) & 63) : (int)((mi >> (6 * (t - 4))) & 63);
        if (rv->has_tmtx[t]) tm = rv->tmtx[t];
        float o[3] = {in[0], in[1], 1};
        if (type == 0) {
            float in4[3] = {in[0], in[1], in[2]};
            mtx_mul(tm * 4, in4, o, proj ? 3 : 2);
            if (!proj) o[2] = 1;
        } else {
            o[0] = in[0]; o[1] = in[1]; o[2] = 1;
        }
        out->tc[t][0] = o[0]; out->tc[t][1] = o[1]; out->tc[t][2] = o[2];
    }
    out->ntex = ngen;
}

static void to_screen(const GxPipeVertex* v, float* sx, float* sy, float* sz) {
    float wd = xff(0x101A), ht = xff(0x101B), zr = xff(0x101C), xo = xff(0x101D), yo = xff(0x101E), fz = xff(0x101F);
    float iw = 1.0f / v->clip[3];
    *sx = xo + wd * v->clip[0] * iw - 342.0f;
    *sy = yo + ht * v->clip[1] * iw - 342.0f;
    *sz = fz + zr * v->clip[2] * iw;
}

static void scissor(int* x0, int* y0, int* x1, int* y1) {
    uint32_t tl = g_bp[0x20], br = g_bp[0x21], off = g_bp[0x59];
    int ox = (int)(off & 0x3FF) * 2 - 342, oy = (int)((off >> 10) & 0x3FF) * 2 - 342;
    *x0 = (int)((tl >> 12) & 0x7FF) - 342 - ox; *y0 = (int)(tl & 0x7FF) - 342 - oy;
    *x1 = (int)((br >> 12) & 0x7FF) - 342 - ox + 1; *y1 = (int)(br & 0x7FF) - 342 - oy + 1;
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > EFB_W) *x1 = EFB_W;
    if (*y1 > EFB_H) *y1 = EFB_H;
}


/* ------------------------------------------------------------------ rasterizer
 * The decoder thread sets up each triangle (screen positions, edge equations, the resolved
 * RasterState) and queues it; raster workers shade it. Every worker walks every triangle in
 * order but only touches its own 8-line bands of the EFB, so per-pixel draw order is kept and
 * no two workers write the same pixel. Anything that reads the EFB or frees textures drains
 * the queue first (raster_drain). WPR_RASTER_THREADS=0 shades on the decoder thread. */
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <SDL3/SDL_timer.h>

typedef struct {
    const RasterState* rs;
    float sx[3], sy[3], sz[3], iw[3];
    float e0[3], e1[3];      /* normalized edge functions: w = e[0] + e[1]*px + e[2]*py */
    int minx, miny, maxx, maxy;
    float col[3][2][4];
    float tc[3][8][3];
} RasterJob;

#define JOBS 4096
#define STATES 64
#define BAND_SHIFT 3
static RasterJob jobs[JOBS];
static RasterState states[STATES];
static uint64_t state_last_job[STATES];  /* jobs up to here use the slot */
static int cur_state = -1, nstate;
static int raster_dirty = 1;
static uint64_t job_w;                   /* decoder-owned count of queued jobs */
static SDL_AtomicInt pub_lo;             /* published job count (low 32 bits suffice: ring is small) */
static int nworkers = -1;
static SDL_AtomicInt done[8];            /* jobs each worker has finished */
static SDL_AtomicInt sleepers;
static SDL_Mutex* wake_mu;
static SDL_Condition* wake_cv;

void gx_raster_dirty(void) { raster_dirty = 1; }

static void raster_job(const RasterJob* j, int band, int nbands) {
    const RasterState* rs = j->rs;
    const float* sz = j->sz;
    const float* iw = j->iw;
    for (int y = j->miny; y < j->maxy; y++) {
        if (nbands > 1 && ((y >> BAND_SHIFT) % nbands) != band) {
            /* skip to the first line of the next band of ours */
            continue;
        }
        float py = y + 0.5f;
        float r0 = j->e0[0] + j->e0[2] * py, r1 = j->e1[0] + j->e1[2] * py;
        for (int x = j->minx; x < j->maxx; x++) {
            float px = x + 0.5f;
            float w0 = r0 + j->e0[1] * px, w1 = r1 + j->e1[1] * px;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float pw = 1.0f / (w0 * iw[0] + w1 * iw[1] + w2 * iw[2]);
            float b0 = w0 * iw[0] * pw, b1 = w1 * iw[1] * pw, b2 = w2 * iw[2] * pw;
            int ras[2][4];
            for (int ch = 0; ch < 2; ch++)
                for (int k = 0; k < 4; k++) {
                    float cv = ch < rs->ncol || rs->ncol == 0
                                   ? b0 * j->col[0][ch][k] + b1 * j->col[1][ch][k] + b2 * j->col[2][ch][k] : 0;
                    ras[ch][k] = clamp255((int)(cv + 0.5f));
                }
            float tc[8][2];
            for (int t = 0; t < rs->ntex; t++) {
                float s = b0 * j->tc[0][t][0] + b1 * j->tc[1][t][0] + b2 * j->tc[2][t][0];
                float tt = b0 * j->tc[0][t][1] + b1 * j->tc[1][t][1] + b2 * j->tc[2][t][1];
                float q = b0 * j->tc[0][t][2] + b1 * j->tc[1][t][2] + b2 * j->tc[2][t][2];
                if (q != 0 && q != 1) { s /= q; tt /= q; }
                tc[t][0] = s; tc[t][1] = tt;
            }
            int col[4];
            tev_shade(rs, (const int (*)[4])ras, (const float (*)[2])tc, col);
            if (!alpha_test(rs->alpha_cfg, col[3])) continue;
            write_pixel(rs, x, y, w0 * sz[0] + w1 * sz[1] + w2 * sz[2], col);
        }
    }
}

static int worker_main(void* arg) {
    int k = (int)(intptr_t)arg;
    uint32_t mine = 0;
    for (;;) {
        uint32_t avail = (uint32_t)SDL_GetAtomicInt(&pub_lo);
        if (avail == mine) {
            int spins = 0;
            while ((uint32_t)SDL_GetAtomicInt(&pub_lo) == mine && ++spins < 4000) SDL_CPUPauseInstruction();
            if ((uint32_t)SDL_GetAtomicInt(&pub_lo) == mine) {
                SDL_LockMutex(wake_mu);
                SDL_AddAtomicInt(&sleepers, 1);
                while ((uint32_t)SDL_GetAtomicInt(&pub_lo) == mine) SDL_WaitConditionTimeout(wake_cv, wake_mu, 5);
                SDL_AddAtomicInt(&sleepers, -1);
                SDL_UnlockMutex(wake_mu);
            }
            continue;
        }
        while (mine != avail) {
            raster_job(&jobs[mine % JOBS], k, nworkers);
            mine++;
            if ((mine & 15) == 0 || mine == avail) SDL_SetAtomicInt(&done[k], (int)mine);
        }
    }
    return 0;
}

static void raster_init(void) {
    if (gx_gl_active()) { nworkers = 0; return; }
    const char* e = getenv("WPR_RASTER_THREADS");
    int n = e ? atoi(e) : SDL_GetNumLogicalCPUCores() - 2;
    if (n > 8) n = 8;
    if (n < 0) n = 0;
    if (!e && n < 1) n = 1;
    nworkers = n;
    if (!n) return;
    wake_mu = SDL_CreateMutex();
    wake_cv = SDL_CreateCondition();
    for (int k = 0; k < n; k++) {
        SDL_Thread* t = SDL_CreateThread(worker_main, "raster", (void*)(intptr_t)k);
        if (t) SDL_DetachThread(t);
    }
    fprintf(stderr, "[gx] %d raster threads\n", n);
}

static uint32_t slowest_done(void) {
    uint32_t lo = (uint32_t)job_w;
    for (int k = 0; k < nworkers; k++) {
        uint32_t d = (uint32_t)SDL_GetAtomicInt(&done[k]);
        if ((uint32_t)job_w - d > (uint32_t)job_w - lo) lo = d;
    }
    return lo;
}

/* Waits until the workers have finished every job before `upto` (a job count). */
static void wait_jobs(uint64_t upto) {
    if (job_w - upto >= JOBS) return;  /* older than the ring: already done */
    int spins = 0;
    while ((uint32_t)job_w - slowest_done() > (uint32_t)(job_w - upto)) {
        if (++spins < 2000) SDL_CPUPauseInstruction();
        else SDL_Delay(0);
    }
}

static void raster_drain(void) {
    if (nworkers > 0) wait_jobs(job_w);
    gx_gl_flush();
}

static void publish(void) {
    SDL_SetAtomicInt(&pub_lo, (int)(uint32_t)job_w);
    if (SDL_GetAtomicInt(&sleepers)) {
        SDL_LockMutex(wake_mu);
        SDL_BroadcastCondition(wake_cv);
        SDL_UnlockMutex(wake_mu);
    }
}

static const RasterState* current_state(void) {
    if (raster_dirty || cur_state < 0) {
        int slot = nstate++ % STATES;
        if (nworkers > 0) wait_jobs(state_last_job[slot]);
        build_state(&states[slot]);
        cur_state = slot;
        raster_dirty = 0;
    }
    return &states[cur_state];
}

/* WPR_RENDER_EVERY=N (testing): draw only every Nth frame, so scripted runs on a machine without a
 * GPU reach later screens sooner. Frame dumps then hold only the drawn frames. */
extern uint64_t g_xfb_copies;
static int render_every(void) {
    static int every;
    if (!every) {
        const char* e = getenv("WPR_RENDER_EVERY");
        every = e && atoi(e) > 1 ? atoi(e) : 1;
    }
    return every;
}

void gx_triangle(const GxPipeVertex* v0, const GxPipeVertex* v1, const GxPipeVertex* v2) {
    if (render_every() > 1 && g_xfb_copies % (uint64_t)render_every() != 0) return;
    if (nworkers < 0) raster_init();
    const GxPipeVertex* v[3] = {v0, v1, v2};
    float sx[3], sy[3], sz[3], iw[3];
    for (int i = 0; i < 3; i++) {
        if (v[i]->clip[3] <= 1e-6f) return; /* behind the eye: proper clipping comes with the GPU path */
        to_screen(v[i], &sx[i], &sy[i], &sz[i]);
        iw[i] = 1.0f / v[i]->clip[3];
    }
    float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
    if (fabsf(area) < 1e-8f) return;
    int cull = (int)((g_bp[0x00] >> 14) & 3);
    if (cull == 3) return;
    int cx0, cy0, cx1, cy1;
    scissor(&cx0, &cy0, &cx1, &cy1);
    int minx = (int)floorf(fminf(sx[0], fminf(sx[1], sx[2]))), maxx = (int)ceilf(fmaxf(sx[0], fmaxf(sx[1], sx[2])));
    int miny = (int)floorf(fminf(sy[0], fminf(sy[1], sy[2]))), maxy = (int)ceilf(fmaxf(sy[0], fmaxf(sy[1], sy[2])));
    if (minx < cx0) minx = cx0;
    if (miny < cy0) miny = cy0;
    if (maxx > cx1) maxx = cx1;
    if (maxy > cy1) maxy = cy1;
    g_draw_count++;
    if (minx >= maxx || miny >= maxy) return;

    const RasterState* rs = current_state();
    if (nworkers == 0 && gx_gl_active()) {
        /* the same viewport transform as to_screen, kept linear in clip space so the GPU clips */
        float wd = xff(0x101A), ht = xff(0x101B), zr = xff(0x101C), xo = xff(0x101D), yo = xff(0x101E), fz = xff(0x101F);
        float pos[3][4], col[3][2][4], tcs[3][8][3];
        for (int i = 0; i < 3; i++) {
            const float* c = v[i]->clip;
            pos[i][0] = ((xo - 342.0f) / 320.0f - 1.0f) * c[3] + wd / 320.0f * c[0];
            pos[i][1] = ((yo - 342.0f) / 264.0f - 1.0f) * c[3] + ht / 264.0f * c[1];
            pos[i][2] = (2.0f * fz / 16777216.0f - 1.0f) * c[3] + 2.0f * zr / 16777216.0f * c[2];
            pos[i][3] = c[3];
            memcpy(col[i], v[i]->col, sizeof col[i]);
            memcpy(tcs[i], v[i]->tc, sizeof tcs[i]);
        }
        int sc[4] = {cx0, cy0, cx1, cy1};
        gx_gl_triangle(rs, sc, (const float (*)[4])pos, (const float (*)[2][4])col, (const float (*)[8][3])tcs);
        return;
    }
    if (nworkers > 0 && job_w >= JOBS) wait_jobs(job_w - JOBS + 1);  /* the slot we are about to reuse */
    RasterJob* j = &jobs[job_w % JOBS];
    j->rs = rs;
    memcpy(j->sx, sx, sizeof sx); memcpy(j->sy, sy, sizeof sy); memcpy(j->sz, sz, sizeof sz); memcpy(j->iw, iw, sizeof iw);
    /* w0 = ((sx1-px)(sy2-py) - (sx2-px)(sy1-py)) / area, and likewise w1, as linear functions of px, py */
    float ia = 1.0f / area;
    j->e0[0] = (sx[1] * sy[2] - sx[2] * sy[1]) * ia; j->e0[1] = (sy[1] - sy[2]) * ia; j->e0[2] = (sx[2] - sx[1]) * ia;
    j->e1[0] = (sx[2] * sy[0] - sx[0] * sy[2]) * ia; j->e1[1] = (sy[2] - sy[0]) * ia; j->e1[2] = (sx[0] - sx[2]) * ia;
    j->minx = minx; j->miny = miny; j->maxx = maxx; j->maxy = maxy;
    for (int i = 0; i < 3; i++) {
        memcpy(j->col[i], v[i]->col, sizeof j->col[i]);
        memcpy(j->tc[i], v[i]->tc, sizeof(float) * 3 * (size_t)(rs->ntex > 8 ? 8 : rs->ntex));
    }
    if (nworkers == 0) { raster_job(j, 0, 1); job_w++; return; }
    job_w++;
    state_last_job[cur_state] = job_w;
    publish();
}

/* ------------------------------------------------------------------ EFB copies */
static uint8_t to_y(int r, int g, int b) { return (uint8_t)clamp255(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16); }
static uint8_t to_u(int r, int g, int b) { return (uint8_t)clamp255(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); }
static uint8_t to_v(int r, int g, int b) { return (uint8_t)clamp255(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128); }

int g_xfb_stride = 1280;
uint32_t g_xfb_last_addr;
uint64_t g_xfb_copies;  /* frames the game finished: the fps counter in the title bar */

static void dump_frame(int x0, int y0, int w, int h) {
    static const char* dir;
    static int checked, n;
    if (!checked) { dir = getenv("WPR_DUMP_FRAMES"); checked = 1; }
    if (!dir || (g_xfb_copies - 1) % (uint64_t)render_every() != 0) return;
    char path[512];
    snprintf(path, sizeof path, "%s/frame_%04d.ppm", dir, n++);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) fwrite(efb[(y0 + y) % EFB_H][(x0 + x) % EFB_W], 1, 3, f);
    fclose(f);
}

/* One texel in a texture copy format, as the bytes it occupies (big endian). */
static void encode_texel(int fmt, int intensity, const uint8_t* p, uint8_t* out, int* bits) {
    int r = p[0], g = p[1], b = p[2], a = p[3];
    int y = intensity ? to_y(r, g, b) : r;
    switch (fmt) {
    case 0: case 1: case 8: *bits = fmt == 0 ? 4 : 8; out[0] = (uint8_t)(fmt == 0 ? y >> 4 : y); break; /* R4/I4, R8/I8 */
    case 2: *bits = 8; out[0] = (uint8_t)((a >> 4) << 4 | (y >> 4)); break;                         /* RA4/IA4 */
    case 3: *bits = 16; out[0] = (uint8_t)a; out[1] = (uint8_t)y; break;                            /* RA8/IA8 */
    case 4: { *bits = 16; int v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3); out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; break; }
    case 5: { *bits = 16; int v = a >= 0xE0 ? 0x8000 | (r >> 3) << 10 | (g >> 3) << 5 | (b >> 3)
                                           : (a >> 5) << 12 | (r >> 4) << 8 | (g >> 4) << 4 | (b >> 4);
        out[0] = (uint8_t)(v >> 8); out[1] = (uint8_t)v; break; }
    case 7: *bits = 8; out[0] = (uint8_t)a; break;
    case 9: *bits = 8; out[0] = (uint8_t)g; break;
    case 10: *bits = 8; out[0] = (uint8_t)b; break;
    case 11: *bits = 16; out[0] = (uint8_t)g; out[1] = (uint8_t)r; break;
    case 12: *bits = 16; out[0] = (uint8_t)b; out[1] = (uint8_t)g; break;
    default: *bits = 32; out[0] = (uint8_t)a; out[1] = (uint8_t)r; out[2] = (uint8_t)g; out[3] = (uint8_t)b; break;
    }
}

static void copy_to_texture(uint32_t trigger, int x0, int y0, int w, int h, uint32_t dst) {
    int fmt = (int)(((trigger >> 4) & 7) | (((trigger >> 3) & 1) << 3));
    int intensity = (trigger >> 15) & 1, half = (trigger >> 9) & 1;
    int tw = half ? w / 2 : w, th = half ? h / 2 : h;
    uint8_t probe[4]; int bits;
    encode_texel(fmt, 0, (const uint8_t[4]){0, 0, 0, 0}, probe, &bits);
    int bw = bits == 4 ? 8 : bits == 8 ? 8 : 4, bh = bits == 4 ? 8 : 4;
    uint8_t* out = g_mem + PPC_PHYS(dst | 0x80000000u);
    for (int by = 0; by < th; by += bh)
        for (int bx = 0; bx < tw; bx += bw) {
            uint8_t block[64] = {0};
            for (int i = 0; i < bw * bh; i++) {
                int px = bx + i % bw, py = by + i / bw;
                int sx = x0 + (half ? px * 2 : px), sy = y0 + (half ? py * 2 : py);
                if (sx >= EFB_W) sx = EFB_W - 1;
                if (sy >= EFB_H) sy = EFB_H - 1;
                uint8_t px4[4];
                if (half) {
                    for (int ch = 0; ch < 4; ch++) {
                        int sx1 = sx + 1 < EFB_W ? sx + 1 : sx, sy1 = sy + 1 < EFB_H ? sy + 1 : sy;
                        px4[ch] = (uint8_t)((efb[sy][sx][ch] + efb[sy][sx1][ch] + efb[sy1][sx][ch] + efb[sy1][sx1][ch]) / 4);
                    }
                } else memcpy(px4, efb[sy][sx], 4);
                uint8_t e[4];
                encode_texel(fmt, intensity, px4, e, &bits);
                if (bits == 4) block[i / 2] |= (uint8_t)(i & 1 ? e[0] & 15 : e[0] << 4);
                else if (bits == 8) block[i] = e[0];
                else if (bits == 16) { block[i * 2] = e[0]; block[i * 2 + 1] = e[1]; }
                else { block[i * 2] = e[0]; block[i * 2 + 1] = e[1]; block[32 + i * 2] = e[2]; block[32 + i * 2 + 1] = e[3]; }
            }
            int sz = bits == 32 ? 64 : 32;
            memcpy(out, block, (size_t)sz);
            out += sz;
        }
}

void gx_efb_copy(uint32_t trigger) {
    if (nworkers < 0) raster_init();
    raster_drain();
    uint32_t src = g_bp[0x49], size = g_bp[0x4A];
    int x0 = (int)(src & 0x3FF), y0 = (int)((src >> 10) & 0x3FF);
    int w = (int)(size & 0x3FF) + 1, h = (int)((size >> 10) & 0x3FF) + 1;
    int gl = nworkers == 0 && gx_gl_active();
    if (gl) gx_gl_read_efb(x0, y0, w + 1, h + 1, efb);  /* +1: the box filter of half-size copies reads one more */
    uint32_t dst = (g_bp[0x4B] & 0xFFFFFF) << 5;
    uint32_t stride = (g_bp[0x4D] & 0x3FF) << 5;
    if (trigger & (1u << 14)) {
        /* to XFB as YUYV, with the vertical scale the game set (BP 0x4E, 8.8 fixed, inverted) */
        uint32_t ys = g_bp[0x4E] & 0x1FF;
        float scale = (trigger & (1u << 10)) && ys ? 256.0f / (float)ys : 1.0f;
        int lines = (int)((float)(h - 1) * scale) + 1;
        for (int y = 0; y < lines; y++) {
            int sy = y0 + (int)((float)y / scale);
            if (sy >= EFB_H) sy = EFB_H - 1;
            uint8_t* row = g_mem + PPC_PHYS((dst + (uint32_t)y * stride) | 0x80000000u);
            for (int x = 0; x + 1 < w && x0 + x + 1 < EFB_W; x += 2) {
                uint8_t* a = efb[sy][x0 + x];
                uint8_t* b = efb[sy][x0 + x + 1];
                int r = (a[0] + b[0]) / 2, g = (a[1] + b[1]) / 2, bl = (a[2] + b[2]) / 2;
                row[x * 2 + 0] = to_y(a[0], a[1], a[2]);
                row[x * 2 + 1] = to_u(r, g, bl);
                row[x * 2 + 2] = to_y(b[0], b[1], b[2]);
                row[x * 2 + 3] = to_v(r, g, bl);
            }
        }
        g_xfb_w = w;
        g_xfb_h = lines;
        g_xfb_stride = (int)stride;
        g_xfb_last_addr = dst;
        g_xfb_copies++;
        dump_frame(x0, y0, w, h);
        /* a game that never invalidates still gets textures it rewrote in memory picked up each frame */
        if (!tcache_game_invalidated) gx_invalidate_textures();
        tcache_game_invalidated = 0;
    } else {
        copy_to_texture(trigger, x0, y0, w, h, dst);
        invalidate_textures_in(PPC_PHYS(dst | 0x80000000u), PPC_PHYS(dst | 0x80000000u) + (uint32_t)(w * h * 4));
    }
    if (trigger & (1u << 11)) {
        uint32_t ar = g_bp[0x4F], gb = g_bp[0x50];
        uint8_t cr = (uint8_t)(ar & 255), ca = (uint8_t)((ar >> 8) & 255), cg = (uint8_t)((gb >> 8) & 255), cb = (uint8_t)(gb & 255);
        float cz = (float)(g_bp[0x51] & 0xFFFFFF);
        if (gl) {
            gx_gl_clear(x0, y0, w, h, (const uint8_t[4]){cr, cg, cb, ca}, cz / 16777216.0f);
            return;
        }
        for (int y = y0; y < y0 + h && y < EFB_H; y++)
            for (int x = x0; x < x0 + w && x < EFB_W; x++) {
                efb[y][x][0] = cr; efb[y][x][1] = cg; efb[y][x][2] = cb; efb[y][x][3] = ca;
                zbuf[y][x] = cz;
            }
    }
}

void gx_load_tlut(void) {
    uint32_t src = (g_bp[0x64] & 0xFFFFFF) << 5;
    uint32_t v = g_bp[0x65];
    uint32_t tm = (v & 0x3FF) << 9, count = ((v >> 10) & 0x7FF) * 16 * 2;
    memcpy(tmem + (tm & 0xFFFFF), g_mem + PPC_PHYS(src | 0x80000000u), count);
    invalidate_textures_in(0, 0);
}
