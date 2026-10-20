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

void irq_raise(uint32_t cause);
void irq_clear(uint32_t cause);
#define PI_CAUSE_VI 0x100u

/* Display interrupts DI0..DI3 at 0x30..0x3C: bit 31 status, bit 28 enable (high halfword view). */
static void vi_update_irq(void) {
    int any = 0;
    for (int i = 0; i < 4; i++) {
        uint16_t hi = vi_regs[(0x30 + 4 * i) / 2];
        if ((hi & 0x8000) && (hi & 0x1000)) any = 1;
    }
    if (any) irq_raise(PI_CAUSE_VI); else irq_clear(PI_CAUSE_VI);
}

void vi_retrace(void) {
    /* both the pre-retrace (DI0) and post-retrace (DI1) interrupts the VI library programs */
    for (int i = 0; i < 2; i++) {
        uint16_t* di = &vi_regs[(0x30 + 4 * i) / 2];
        if (*di & 0x1000) *di |= 0x8000;
    }
    vi_update_irq();
}

void hw_vi_write(uint32_t addr, uint32_t val, int size) {
    uint32_t r = (addr & 0xFF) >> 1;
    if (size == 4) { vi_regs[r] = (uint16_t)(val >> 16); vi_regs[r + 1] = (uint16_t)val; }
    else vi_regs[r] = (uint16_t)val;
    if ((addr & 0xFF) >= 0x30 && (addr & 0xFF) < 0x40) vi_update_irq();
}

/* The VI comes out of the IPL already running in the console's video standard, and VIInit takes
 * over that state (it only programs the VI itself when DCR.ENB is clear). Register values from
 * Dolphin's VideoInterfaceManager::Preset; DCR bits 8-9 are the format (0 NTSC, 1 PAL). */
static void vi_set32(uint32_t off, uint32_t v) { vi_regs[off / 2] = (uint16_t)(v >> 16); vi_regs[off / 2 + 1] = (uint16_t)v; }

void hw_vi_set_format(int pal) {
    vi_regs[0x00 / 2] = (uint16_t)(6 | 0 << 4);                    /* VTR: EQU 6, ACV 0 */
    vi_regs[0x02 / 2] = (uint16_t)(1 | (pal ? 1u : 0u) << 8);      /* DCR: enabled, format */
    vi_set32(0x04, 429u | 105u << 16 | 71u << 24);                 /* HTR0: HLW, HCE, HCS */
    vi_set32(0x08, 64u | 162u << 7 | 373u << 17);                  /* HTR1: HSY, HBE640, HBS640 */
    vi_set32(0x0C, 502u | 5u << 16);                               /* VTO: PRB, PSB */
    vi_set32(0x10, 503u | 4u << 16);                               /* VTE */
    vi_set32(0x14, 12u | 520u << 5 | 12u << 16 | 520u << 21);      /* BBOI */
    vi_set32(0x18, 13u | 519u << 5 | 13u << 16 | 519u << 21);      /* BBEI */
    vi_set32(0x30, 430u | 263u << 16 | 1u << 28);                  /* DI0 */
    vi_set32(0x34, 1u | 1u << 16 | 1u << 28);                      /* DI1 */
    vi_regs[0x48 / 2] = (uint16_t)(40 | 40 << 8);                  /* picture config: STD, WPL */
}

/* Top field framebuffer address: VI_TFBL at 0x1C (bit 28 = shift by 5). */
uint32_t hw_vi_xfb_addr(void) {
    uint32_t v = ((uint32_t)vi_regs[0x1C / 2] << 16) | vi_regs[0x1E / 2];
    uint32_t a = v & 0x00FFFFFF;
    if (v & 0x10000000) a <<= 5;
    return a;
}

/* Log each unhandled register once so busy-wait loops don't flood the console. */
static void log_once(const char* what, uint32_t addr, uint32_t val, int size) {
    static uint32_t seen[512];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == addr) return;
    if (n < 512) seen[n++] = addr;
    fprintf(stderr, "[hw] unhandled %s%d %08X = %08X\n", what, size * 8, addr, val);
}

/* DSP interface (CC005000): mailboxes + control/status. Bits that hardware clears on its own
 * (reset, DMA busy, DSP init) read back as done. */
static uint16_t dsp_regs[0x40];
static uint32_t dsp_read(uint32_t a, int size) {
    uint32_t r = (a & 0x7F) >> 1;
    if (r == 0x0A / 2) return dsp_regs[r] & ~0x0E01u;
    /* DSP->CPU mailbox: report a mail so the boot-time DSP handshakes complete. Audio (AX)
     * will be done natively at the AX API level, so the DSP never runs real microcode. */
    static int mail_valid;
    if (r == 0x04 / 2) {
        /* No mail on the first look, then "the DSP answered"; reading the low half acks it. */
        int v = mail_valid;
        if (!v) mail_valid = 1;
        if (size == 4) { mail_valid = 0; return v ? 0x80000000u : 0; }
        return v ? 0x8000u : 0;
    }
    if (r == 0x06 / 2) { mail_valid = 0; return 0; }
    if (size == 4) return ((uint32_t)dsp_regs[r] << 16) | dsp_regs[r + 1];
    return dsp_regs[r];
}
static void dsp_write(uint32_t a, uint32_t v, int size) {
    uint32_t r = (a & 0x7F) >> 1;
    if (size == 4) {
        dsp_regs[r] = (uint16_t)(v >> 16); dsp_regs[r + 1] = (uint16_t)v;
        if (r == 0x28 / 2) dsp_regs[0x0A / 2] |= 0x20;
        return;
    }
    if (r == 0x0A / 2) { dsp_regs[r] = (uint16_t)((dsp_regs[r] & ~0xA8u & ~(v & 0xA8u)) | (v & ~0xA8u)); return; }
    if (r == 0x2A / 2) { dsp_regs[r] = (uint16_t)v; dsp_regs[0x0A / 2] |= 0x20; return; } /* ARAM DMA done at once */
    dsp_regs[r] = (uint16_t)v;
}

uint32_t pi_read(uint32_t addr);
void pi_write(uint32_t addr, uint32_t v);
extern volatile int g_irq_pending;
void ppc_check_irq_current(void);

uint32_t ipc_read(uint32_t addr);
void ipc_write(uint32_t addr, uint32_t v);

uint32_t mmio_read(uint32_t addr, int size) {
    uint32_t a = addr & 0x00FFFFFF;
    if (g_irq_pending) ppc_check_irq_current();  /* busy-wait loops on registers are safe points */
    if ((addr >> 24) == 0xCC) {
        if (a >= 0x3000 && a < 0x3100) return pi_read(addr);
        if (a >= 0x2000 && a < 0x2100) return hw_vi_read(addr, size);
        if (a >= 0x0000 && a < 0x1000) return gx_cp_read(addr, size);
        if (a >= 0x1000 && a < 0x2000) return gx_pe_read(addr, size);
        if (a >= 0x5000 && a < 0x5200) return dsp_read(addr, size);
    }
    if ((addr >> 24) == 0xCD && a < 0x40) return ipc_read(addr);
    log_once("read", addr, 0, size);
    return 0;
}

void mmio_write(uint32_t addr, uint32_t val, int size) {
    uint32_t a = addr & 0x00FFFFFF;
    if ((addr >> 24) == 0xCC) {
        if (a >= 0x8000 && a < 0x9000) { gx_fifo_write(val, size); return; }
        if (a >= 0x3000 && a < 0x3100) { pi_write(addr, val); return; }
        if (a >= 0x2000 && a < 0x2100) { hw_vi_write(addr, val, size); return; }
        if (a < 0x1000) { gx_cp_write(addr, val, size); return; }
        if (a >= 0x1000 && a < 0x2000) { gx_pe_write(addr, val, size); return; }
        if (a >= 0x5000 && a < 0x5200) { dsp_write(addr, val, size); return; }
    }
    if ((addr >> 24) == 0xCD && a < 0x40) { ipc_write(addr, val); return; }
    log_once("write", addr, val, size);
}
