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
 * No samples are mixed yet: voices stay silent and the AI DMA just keeps time. */
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

/* ------------------------------------------------------------------ the microcode stand-in */
enum { UC_ROM, UC_TASK };
static int ucode = UC_ROM;
static int rom_hello_sent;
static uint32_t rom_expect;  /* the boot mail whose argument comes next */
static int next_is_cmdlist, upload_steps;

static void cpu_mail(uint32_t m) {
    if (trace) fprintf(stderr, "[dsp] <- %08X\n", m);
    if (ucode == UC_ROM) {
        if (rom_expect) {
            uint32_t what = rom_expect;
            rom_expect = 0;
            if (what == 0x80F3D001u) {  /* init vector: the task starts and says hello */
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
    if (next_is_cmdlist) {
        next_is_cmdlist = 0;
        push_mail(DSP_YIELD, 1);
        return;
    }
    if (m == MAIL_RESUME) push_mail(DSP_RESUME, 1);
    else if (m == MAIL_NEW_UCODE) upload_steps = 1;
    else if (m == MAIL_RESET) { ucode = UC_ROM; rom_hello_sent = 0; }
    else if (m == MAIL_CONTINUE) {}
    else if ((m & 0xFFFF0000u) == 0xBABE0000u) next_is_cmdlist = 1;
}

/* ------------------------------------------------------------------ AI DMA timing */
static SDL_AtomicInt aid_running;
static uint64_t aid_next_ns;

/* Called from the timer thread: each AI DMA block "starts" one period after the last, which
 * is when the hardware raises AIDINT and the game queues the next buffer. */
void dsp_ai_tick(uint64_t now) {
    if (!SDL_GetAtomicInt(&aid_running)) return;
    uint32_t blocks = regs[0x36 / 2] & 0x7FFF;
    if (!blocks) return;
    if (now < aid_next_ns) return;
    uint64_t period = (uint64_t)blocks * 32 / 4 * 1000000000ull / hw_ai_dma_rate();
    aid_next_ns = (now - aid_next_ns > period * 4) ? now + period : aid_next_ns + period;
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
