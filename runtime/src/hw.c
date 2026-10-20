/* Hardware register dispatch (0xCC000000 / 0xCD000000).
 * Map from Dolphin's Source/Core/Core/HW/Memmap.cpp + MMIO.cpp:
 *   CC000000 CP (command processor)   CC001000 PE (pixel engine)   CC002000 VI (video interface)
 *   CC003000 PI (interrupts)          CC004000 MI (memory)         CC005000 DSP / ARAM
 *   CC006000 DI / SI / EXI / AI       CC008000 GX write-gather FIFO
 *   CD000000 Hollywood (IPC to IOS, GPIO, timers)
 */
#include <stdio.h>

#include "ppc_context.h"
#include "runtime.h"
#include "gx.h"

static uint16_t vi_regs[0x80];

uint32_t hw_vi_read(uint32_t addr, int size) {
    uint32_t r = (addr & 0xFF) >> 1;
    if (r == 0x2C / 2) {
        /* VI_VCOUNT: fake a scanline counter so retrace waits make progress */
        static uint16_t line;
        line = (uint16_t)((line + 1) % 525);
        return line;
    }
    if (size == 4) return ((uint32_t)vi_regs[r] << 16) | vi_regs[r + 1];
    return vi_regs[r];
}

void hw_vi_write(uint32_t addr, uint32_t val, int size) {
    uint32_t r = (addr & 0xFF) >> 1;
    if (size == 4) { vi_regs[r] = (uint16_t)(val >> 16); vi_regs[r + 1] = (uint16_t)val; }
    else vi_regs[r] = (uint16_t)val;
}

/* Top field framebuffer address: VI_TFBL at 0x1C (bit 28 = shift by 5). */
uint32_t hw_vi_xfb_addr(void) {
    uint32_t v = ((uint32_t)vi_regs[0x1C / 2] << 16) | vi_regs[0x1E / 2];
    uint32_t a = v & 0x00FFFFFF;
    if (v & 0x10000000) a <<= 5;
    return a;
}

uint32_t mmio_read(uint32_t addr, int size) {
    uint32_t a = addr & 0x00FFFFFF;
    if ((addr >> 24) == 0xCC) {
        if (a >= 0x2000 && a < 0x2100) return hw_vi_read(addr, size);
        if (a >= 0x0000 && a < 0x1000) return gx_cp_read(addr, size);
    }
    fprintf(stderr, "[hw] unhandled read%d %08X\n", size * 8, addr);
    return 0;
}

void mmio_write(uint32_t addr, uint32_t val, int size) {
    uint32_t a = addr & 0x00FFFFFF;
    if ((addr >> 24) == 0xCC) {
        if (a >= 0x8000 && a < 0x9000) { gx_fifo_write(val, size); return; }
        if (a >= 0x2000 && a < 0x2100) { hw_vi_write(addr, val, size); return; }
        if (a < 0x1000) { gx_cp_write(addr, val, size); return; }
    }
    fprintf(stderr, "[hw] unhandled write%d %08X = %08X\n", size * 8, addr, val);
}
