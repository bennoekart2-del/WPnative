/* wiiparty-native: runs the recompiled game.
 *
 *   wiiparty-native.exe <extracted-disc-folder>     run the game
 *   wiiparty-native.exe --selftest                  window + controller test, no game needed
 */
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gx.h"
#include "ios.h"
#include "ppc_context.h"
#include "runtime.h"

extern const uint32_t g_recomp_entry;
extern const unsigned g_recomp_func_count;
extern uint64_t g_timebase;
uint32_t hw_vi_xfb_addr(void);

static PPCContext g_cpu;

uint64_t host_ticks_ns(void) { return SDL_GetTicksNS(); }
static SDL_AtomicInt g_running;

PPCFunc ppc_hle_lookup(uint32_t addr) {
    (void)addr; /* HLE replacements (OS threads, IOS, WPAD) register here as they land */
    return NULL;
}

void threads_init_main(PPCContext* cpu);
void timers_start(void);

static int SDLCALL guest_thread(void* entry) {
    threads_init_main(&g_cpu);
    g_cpu.r[1] = 0x816FFFF0;   /* initial stack, as the apploader leaves it */
    g_cpu.msr = 0x00002032;
    ppc_call_indirect(&g_cpu, (uint32_t)(uintptr_t)entry);
    fprintf(stderr, "[main] game entry returned\n");
    SDL_SetAtomicInt(&g_running, 0);
    return 0;
}

/* Draw colour bars plus one marker per connected controller, straight into a YUYV buffer. */
static void selftest_frame(uint8_t* yuyv, int w, int h, int frame) {
    static const uint8_t bars[8][3] = {{235, 128, 128}, {210, 16, 146}, {170, 166, 16}, {145, 54, 34},
                                       {106, 202, 222}, {81, 90, 240}, {41, 240, 110}, {16, 128, 128}};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x += 2) {
            const uint8_t* c = bars[(x * 8 / w + frame / 60) & 7];
            uint8_t* p = yuyv + (y * w + x) * 2;
            p[0] = c[0]; p[1] = c[1]; p[2] = c[0]; p[3] = c[2];
        }
    for (int pl = 0; pl < MAX_PLAYERS; pl++) {
        const HostRemote* r = platform_remote(pl);
        if (!r->connected) continue;
        int cx = (int)((r->ir_x + 1) * 0.5f * (w - 40)), cy = (int)((r->ir_y + 1) * 0.5f * (h - 40));
        uint8_t luma = r->buttons ? 235 : 16;
        for (int y = cy; y < cy + 40 && y < h; y++)
            for (int x = cx & ~1; x < cx + 40 && x < w; x += 2) {
                uint8_t* p = yuyv + (y * w + x) * 2;
                p[0] = p[2] = luma; p[1] = (uint8_t)(64 * pl); p[3] = (uint8_t)(255 - 64 * pl);
            }
    }
}

int main(int argc, char** argv) {
    int selftest = argc < 2 || strcmp(argv[1], "--selftest") == 0;
    if (!platform_init("Wii Party (native)", 1280, 960)) return 1;

    uint32_t entry = 0;
    if (!selftest) {
        if (!g_recomp_func_count) {
            fprintf(stderr, "this build has no recompiled game code; run tools/build_game first\n");
            return 1;
        }
        if (!boot_load(argv[1], &entry)) return 1;
        ios_set_disc_root(argv[1]);
        SDL_SetAtomicInt(&g_running, 1);
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void*)guest_thread);
        SDL_SetStringProperty(props, SDL_PROP_THREAD_CREATE_NAME_STRING, "guest-main");
        SDL_SetPointerProperty(props, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, (void*)(uintptr_t)entry);
        SDL_SetNumberProperty(props, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, 8 << 20);
        SDL_DetachThread(SDL_CreateThreadWithProperties(props));
        timers_start();
    }

    static uint8_t test_xfb[640 * 480 * 2];
    for (int frame = 0; platform_pump(); frame++) {
        if (selftest) {
            selftest_frame(test_xfb, 640, 480, frame);
            platform_present_xfb(test_xfb, 640, 480, 1280);
            continue;
        }
        uint32_t xfb = hw_vi_xfb_addr();
        platform_present_xfb(xfb ? g_mem + PPC_PHYS(xfb) : NULL, g_xfb_w & ~1, g_xfb_h, g_xfb_stride);
        if (frame % 300 == 0) {
            const GxStats* s = gx_stats();
            fprintf(stderr, "[stats] draws=%llu verts=%llu efb_copies=%llu xfb=%08X\n",
                    (unsigned long long)s->draws, (unsigned long long)s->vertices,
                    (unsigned long long)s->efb_copies, xfb);
        }
        if (!SDL_GetAtomicInt(&g_running)) break;
    }
    platform_shutdown();
    return 0;
}
