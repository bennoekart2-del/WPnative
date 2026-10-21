/* Shared between the software renderer (gx_render.c) and the OpenGL backend (gx_gl.c). */
#pragma once
#include <stdint.h>

/* One bound texture map, decoded to RGBA8 (r in the low byte) with rows `stride` texels apart. */
typedef struct { uint32_t* px; int w, h, stride; int wrap_s, wrap_t, linear; } GxGlTexMap;

/* Everything a draw's pixels depend on, resolved from the BP registers once per state change. */
typedef struct {
    int texen, texcoord, ras_sel, texmap;
    GxGlTexMap tm;
    int tsw[4], rsw[4];                  /* swap tables as channel indices */
    int konst[3], kalpha;
    int csel[4], asel[4];
    int c_bias, c_sub, c_clamp, c_scale, c_dest;
    int a_bias, a_sub, a_clamp, a_scale, a_dest;
} GxGlStage;

typedef struct {
    int nstages, ntex, ncol;
    int regs[4][4];                      /* PREV, REG0..2 as r, g, b, a */
    uint32_t alpha_cfg, zmode, cmode;
    GxGlStage st[16];
} GxGlState;

int platform_gl_make_current(void);    /* platform_sdl.c: binds the renderer's GL context to this thread */

int gx_gl_active(void);                 /* initializes on first use; 0 = use the software renderer */
void gx_gl_triangle(const GxGlState* s, const int scissor[4], const float pos[3][4], const float col[3][2][4],
                    const float tc[3][8][3]);
void gx_gl_flush(void);
void gx_gl_forget_texture(const void* px);
void gx_gl_read_efb(int x0, int y0, int w, int h, uint8_t (*efb)[640][4]);
void gx_gl_clear(int x0, int y0, int w, int h, const uint8_t rgba[4], float z);
