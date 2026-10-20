/* GX command stream decoder: frames the FIFO into CP/XF/BP register writes, display-list calls and
 * primitive draws, tracking the vertex descriptor (VCD) and vertex attribute tables (VAT) so we know
 * how many bytes each vertex takes. Bit layouts follow Dolphin's VideoCommon/CPMemory.h.
 * Draws are handed to the renderer (gx_render_*); for now they are counted.
 */
#include <stdio.h>
#include <string.h>

#include "gx.h"
#include "ppc_context.h"

static uint8_t fifo[1 << 20];
static size_t fifo_len;
static GxStats stats;

static uint32_t vcd_lo, vcd_hi, vat_a[8], vat_b[8], vat_c[8];
static uint32_t xf_mem[0x1100];
static uint32_t bp_mem[0x100];

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

int gx_vertex_size(int vat) {
    uint32_t a = vat_a[vat], b = vat_b[vat], cc = vat_c[vat];
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
    const uint32_t tex_cnt[8] = {field(a, 21, 1), field(b, 0, 1), field(b, 9, 1), field(b, 18, 1),
                                 field(b, 27, 1), field(cc, 5, 1), field(cc, 14, 1), field(cc, 23, 1)};
    const uint32_t tex_fmt[8] = {field(a, 22, 3), field(b, 1, 3), field(b, 10, 3), field(b, 19, 3),
                                 field(b, 28, 3), field(cc, 6, 3), field(cc, 15, 3), field(cc, 24, 3)};
    for (int t = 0; t < 8; t++)
        size += attr_size(field(vcd_hi, t * 2, 2), (tex_cnt[t] ? 2 : 1) * comp_size(tex_fmt[t]));
    return size;
}

static void load_cp(uint8_t reg, uint32_t v) {
    stats.cp_writes++;
    if (reg >= 0x50 && reg < 0x58) vcd_lo = v;
    else if (reg >= 0x60 && reg < 0x68) vcd_hi = v;
    else if (reg >= 0x70 && reg < 0x78) vat_a[reg & 7] = v;
    else if (reg >= 0x80 && reg < 0x88) vat_b[reg & 7] = v;
    else if (reg >= 0x90 && reg < 0x98) vat_c[reg & 7] = v;
}

static void load_bp(uint32_t v) {
    uint8_t reg = (uint8_t)(v >> 24);
    stats.bp_writes++;
    bp_mem[reg] = v & 0xFFFFFF;
    if (reg == 0x52) stats.efb_copies++; /* EFB -> XFB/texture copy: where a frame becomes visible */
}

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

/* Decode as many complete commands as `buf` holds; returns bytes consumed. */
static size_t decode(const uint8_t* buf, size_t len, int depth) {
    size_t p = 0;
    while (p < len) {
        uint8_t cmd = buf[p];
        size_t need = 1;
        if (cmd == 0x00 || cmd == 0x48) { p += 1; continue; }
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
                if (addr + i < 0x1100) xf_mem[addr + i] = rd32(buf + p + 5 + 4 * i);
            stats.xf_writes += count;
        } else if ((cmd & 0xE7) == 0x20) {  /* LOAD_INDX_A..D */
            need = 5;
        } else if (cmd == 0x40) {  /* CALL_DL */
            need = 9;
            if (p + need > len) break;
            uint32_t addr = rd32(buf + p + 1), size = rd32(buf + p + 5);
            stats.display_lists++;
            if (depth < 4 && size < 0x1000000) decode(g_mem + PPC_PHYS(addr), size, depth + 1);
        } else if (cmd == 0x44) {
            need = 1;
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
        } else {
            stats.unknown++;
            if (stats.unknown < 16) fprintf(stderr, "[gx] unknown FIFO opcode %02X\n", cmd);
        }
        if (p + need > len) break;
        p += need;
    }
    return p;
}

void gx_flush(void) {
    size_t used = decode(fifo, fifo_len, 0);
    memmove(fifo, fifo + used, fifo_len - used);
    fifo_len -= used;
}

void gx_fifo_write(uint32_t val, int size) {
    if (fifo_len + 4 > sizeof fifo) gx_flush();
    for (int i = size - 1; i >= 0; i--) fifo[fifo_len++] = (uint8_t)(val >> (8 * i));
    if (fifo_len > sizeof fifo / 2) gx_flush();
}

static uint16_t cp_regs[0x40];
uint32_t gx_cp_read(uint32_t addr, int size) {
    (void)size;
    return cp_regs[(addr & 0x7F) >> 1];
}
void gx_cp_write(uint32_t addr, uint32_t val, int size) {
    (void)size;
    cp_regs[(addr & 0x7F) >> 1] = (uint16_t)val;
}
