/* Shared between the FIFO decoder (gx_fifo.c) and the software renderer (gx_render.c). */
#pragma once
#include <stdint.h>

extern uint32_t g_xf[0x1100];
extern uint32_t g_bp[0x100];
extern uint32_t g_konst[4][2];  /* TEV konst colors: BP 0xE0..0xE7 written with the konst bit set */
extern uint32_t g_cp_matidx_a, g_cp_matidx_b, g_cp_array_base[16], g_cp_array_stride[16];

/* One vertex as read from the stream, before transform. */
typedef struct {
    float pos[3];
    float nrm[3][3];       /* normal, binormal, tangent */
    int has_nrm, has_nbt;
    float col[2][4];       /* 0..255 */
    float tex[8][2];
    int has_col[2];
    int has_pnmtx, pnmtx;
    int has_tmtx[8], tmtx[8];
} GxRawVertex;

/* After transform: clip-space position, lit colors, generated texture coordinates. */
typedef struct {
    float clip[4];
    float col[2][4];
    float tc[8][3];
    int ntex;
} GxPipeVertex;

void gx_vertex(const GxRawVertex* rv, GxPipeVertex* out);
void gx_triangle(const GxPipeVertex* v0, const GxPipeVertex* v1, const GxPipeVertex* v2);
void gx_line(const GxPipeVertex* a, const GxPipeVertex* b);
void gx_point(const GxPipeVertex* v);
void gx_efb_copy(uint32_t trigger);
void gx_load_tlut(void);
void gx_invalidate_textures(void);
void gx_raster_dirty(void);     /* a BP register the per-draw raster state depends on changed */
void gx_pe_event(int reg, uint32_t v);  /* BP 0x45 (draw done), 0x47/0x48 (tokens) */
