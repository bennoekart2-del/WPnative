/* Deterministic mode, for netplay: every PC runs the whole game itself and only inputs travel, so
 * the game must do exactly the same thing everywhere given the same inputs.
 *
 * Normally guest time is host time: the timer thread raises VI, decrementer, audio DMA and IPC
 * events whenever the host clock says so, and the GPU thread raises PE interrupts whenever it gets
 * there, so where in the game's code an interrupt lands differs from run to run. Here instead:
 *   - guest time is virtual: it advances a little at every safe point (loop back-edge, MMIO read)
 *     and jumps straight to the next event while the OS idles (no current thread);
 *   - timer events are run from the guest's own safe points (timers_run), never from another thread;
 *   - PE interrupts are held back and delivered when the guest idles or reads a PE/CP register,
 *     after waiting for the GPU thread to finish everything sent so far (gx_det_sync);
 *   - each VI retrace waits for the host clock (so the game still runs at 60 fps) and for the
 *     netplay inputs of that frame.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "det.h"
#include "netplay.h"
#include "ppc_context.h"

int g_det;
static uint64_t vt;               /* virtual guest time, ns */
static uint64_t real0;            /* host time at vt = 0 */
static uint64_t step = 8;         /* ns per safe point */
static uint64_t frame;            /* VI retraces so far */
static int hash_every;            /* WPR_DET_HASH=n: print a memory hash every n frames (tests) */

#define MEM1_SIZE 0x01800000u
#define MEM2_BASE 0x10000000u
#define MEM2_SIZE 0x04000000u
#define PAGE 4096u
static uint8_t gpu_page1[MEM1_SIZE / PAGE], gpu_page2[MEM2_SIZE / PAGE];

void det_init(void) {
    const char* s = getenv("WPR_DET");
    g_det = (s && atoi(s)) || net_active();
    if ((s = getenv("WPR_DET_STEP"))) step = (uint64_t)strtoull(s, NULL, 10);
    if ((s = getenv("WPR_DET_HASH"))) hash_every = atoi(s);
    real0 = SDL_GetTicksNS();
    if (g_det) fprintf(stderr, "[det] deterministic mode (%llu ns per safe point)\n", (unsigned long long)step);
}

uint64_t det_now_ns(void) { return g_det ? vt : SDL_GetTicksNS(); }

void det_poll(PPCContext* c) {
    static unsigned n;
    vt += step;
    if ((c->msr & 0x8000u) && mem_r32(0x800000E4u) == 0) {  /* the OS idle loop: nothing to run */
        gx_det_sync();
        uint64_t next = timers_next();
        if (next > vt && next != UINT64_MAX) vt = next;
    } else if (++n & 31) {
        return;  /* timers are looked at every 32 safe points while the game is busy */
    }
    if (vt >= timers_next()) timers_run(vt);
}

void det_mark_gpu(uint32_t lo, uint32_t hi) {
    for (uint32_t p = lo & ~(PAGE - 1); p < hi; p += PAGE) {
        if (p < MEM1_SIZE) gpu_page1[p / PAGE] = 1;
        else if (p >= MEM2_BASE && p - MEM2_BASE < MEM2_SIZE) gpu_page2[(p - MEM2_BASE) / PAGE] = 1;
    }
}

/* One slice of guest memory per frame, skipping pages the GPU wrote (those depend on the
 * renderer and picture quality, which may differ between players). */
#define SLICES 64
static uint64_t hash_slice(int s) {
    const uint32_t pages1 = MEM1_SIZE / PAGE, pages2 = MEM2_SIZE / PAGE, total = pages1 + pages2;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)s;
    for (uint32_t i = (uint32_t)s * (total / SLICES); i < (uint32_t)(s + 1) * (total / SLICES); i++) {
        uint32_t phys;
        if (i < pages1) { if (gpu_page1[i]) continue; phys = i * PAGE; }
        else { if (gpu_page2[i - pages1]) continue; phys = MEM2_BASE + (i - pages1) * PAGE; }
        const uint64_t* w = (const uint64_t*)(g_mem + phys);
        for (uint32_t k = 0; k < PAGE / 8; k++) h = (h ^ w[k]) * 0x100000001B3ull + (h >> 29);
    }
    return h;
}

void det_frame(void) {
    /* keep to the host clock: run no faster than real time; if we fell far behind, don't race */
    uint64_t now = SDL_GetTicksNS();
    int64_t ahead = (int64_t)(real0 + vt - now);
    if (ahead > 0 && !getenv("WPR_DET_FAST")) SDL_DelayPrecise((uint64_t)ahead);
    else if (ahead < -100000000ll) real0 = now - vt;
    if (net_active() || hash_every) {
        gx_det_sync();  /* GPU writes to guest memory must be in before hashing */
        uint64_t h = hash_slice((int)(frame % SLICES));
        if (net_active()) net_hash(frame, h);
        if (hash_every) {
            static uint64_t acc;
            acc = acc * 31 + h;
            if (frame % (uint64_t)hash_every == 0)
                fprintf(stderr, "[det] frame %llu hash %016llX vt %llu ms\n", (unsigned long long)frame,
                        (unsigned long long)acc, (unsigned long long)(vt / 1000000));
        }
    }
    if (net_active()) net_frame(frame);
    frame++;
}
