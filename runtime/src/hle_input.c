/* Wii Remote input at the KPAD/WPAD level. The real path is Bluetooth HCI through
 * /dev/usb/oh1/57e/305; instead the Bluetooth stack is skipped and the game's reads of the
 * Remotes (WPADProbe, KPADReadEx) are answered from the host's keyboard, mouse and gamepads.
 *
 * KPADStatus layout (RVL SDK 4.3, 0xF0 bytes; the game reads 16 of them into a 0xF00 buffer):
 *   0x00 hold, 0x04 trig, 0x08 release, 0x0C acc (Vec), 0x18 acc_value, 0x1C acc_speed,
 *   0x20 pos (Vec2, -1..1), 0x28 vec, 0x30 speed, 0x34 horizon, 0x3C hori_vec, 0x44 hori_speed,
 *   0x48 dist, 0x4C dist_vec, 0x50 dist_speed, 0x54 acc_vertical, 0x5C dev_type, 0x5D wpad_err,
 *   0x5E dpd_valid_fg, 0x5F data_format, 0x60 extension / MotionPlus data (left zero). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc_context.h"
#include "runtime.h"

#define KPAD_STATUS_SIZE 0xF0
#define WPAD_DEV_CORE 0
#define WPAD_DEV_NONE 0xFD
#define WPAD_ERR_NONE 0
#define WPAD_ERR_NO_CONTROLLER (-1)

static struct {
    uint16_t hold;
    float pos[2];
} prev[MAX_PLAYERS];
static unsigned reads[MAX_PLAYERS];

/* WPR_PRESS="A:300,AB:400,2:500" holds buttons on player 1 for 8 reads starting at a read count
 * (for headless test runs). Keys: A B 1 2 + - H U D L R. */
static uint16_t key_bit(char k) {
    switch (k) {
    case 'A': return 0x0800; case 'B': return 0x0400; case '1': return 0x0200; case '2': return 0x0100;
    case '+': return 0x0010; case '-': return 0x1000; case 'H': return 0x8000;
    case 'U': return 0x0008; case 'D': return 0x0004; case 'L': return 0x0001; case 'R': return 0x0002;
    default: return 0;
    }
}

static uint16_t scripted_buttons(int chan) {
    const char* s = getenv("WPR_PRESS");
    if (!s || chan != 0) return 0;
    uint16_t b = 0;
    unsigned n = reads[0];
    while (*s) {
        uint16_t keys = 0;
        while (*s && *s != ':') keys |= key_bit(*s++);
        if (*s != ':') break;
        char* end;
        unsigned at = (unsigned)strtoul(s + 1, &end, 10);
        if (n >= at && n < at + 8) b |= keys;
        s = end;
        if (*s != ',') break;
        s++;
    }
    return b;
}

/* WPR_POINT="0.2,-0.4@900;-0.5,0@1200" puts player 1's pointer at x,y (-1..1) from a read count on. */
static int scripted_pointer(int chan, float* x, float* y) {
    const char* s = getenv("WPR_POINT");
    if (!s || chan != 0) return 0;
    int hit = 0;
    while (*s) {
        char* end;
        float px = strtof(s, &end);
        if (*end != ',') break;
        float py = strtof(end + 1, &end);
        if (*end != '@') break;
        unsigned at = (unsigned)strtoul(end + 1, &end, 10);
        if (reads[0] >= at) { *x = px; *y = py; hit = 1; }
        s = end;
        if (*s != ';') break;
        s++;
    }
    return hit;
}

static int connected(int chan) {
    return chan >= 0 && chan < MAX_PLAYERS && platform_remote(chan)->connected;
}

/* s32 WPADProbe(s32 chan, u32* type) */
void hle_WPADProbe(PPCContext* c) {
    int chan = (int32_t)c->r[3];
    int ok = connected(chan);
    if (c->r[4]) mem_w32(c->r[4], ok ? WPAD_DEV_CORE : WPAD_DEV_NONE);
    c->r[3] = (uint32_t)(ok ? WPAD_ERR_NONE : WPAD_ERR_NO_CONTROLLER);
}

/* s32 KPADReadEx(s32 chan, KPADStatus* buf, u32 length, s32* err): newest sample first,
 * returns the number of samples written. */
void hle_KPADReadEx(PPCContext* c) {
    int chan = (int32_t)c->r[3];
    uint32_t buf = c->r[4], len = c->r[5], err = c->r[6];
    static unsigned calls;
    if (getenv("WPR_TRACE_INPUT") && ++calls % 200 == 0)
        fprintf(stderr, "[input] call %u chan %d len %u connected %d\n", calls, chan, len, connected(chan));
    if (!connected(chan) || !buf || !len) {
        if (err) mem_w32(err, (uint32_t)WPAD_ERR_NO_CONTROLLER);
        c->r[3] = 0;
        return;
    }
    reads[chan]++;
    if (chan == 0 && getenv("WPR_TRACE_INPUT") && reads[0] % 200 == 0)
        fprintf(stderr, "[input] read %u (lr %08X)\n", reads[0], c->lr);
    const HostRemote* r = platform_remote(chan);
    uint16_t hold = r->buttons | scripted_buttons(chan);
    uint16_t old = prev[chan].hold;
    float px = r->ir_x, py = r->ir_y;
    scripted_pointer(chan, &px, &py);
    float vx = px - prev[chan].pos[0], vy = py - prev[chan].pos[1];

    for (uint32_t i = 0; i < KPAD_STATUS_SIZE; i += 4) mem_w32(buf + i, 0);
    mem_w32(buf + 0x00, hold);
    mem_w32(buf + 0x04, hold & ~old);
    mem_w32(buf + 0x08, old & ~hold);
    /* held level and still: gravity along -y; the left stick tilts */
    mem_wf32(buf + 0x0C, r->accel[0]);
    mem_wf32(buf + 0x10, -1.0f + r->accel[1]);
    mem_wf32(buf + 0x14, 0.0f);
    mem_wf32(buf + 0x18, 1.0f);
    mem_wf32(buf + 0x20, px);
    mem_wf32(buf + 0x24, py);
    mem_wf32(buf + 0x28, vx);
    mem_wf32(buf + 0x2C, vy);
    mem_wf32(buf + 0x30, sqrtf(vx * vx + vy * vy));
    mem_wf32(buf + 0x34, 1.0f);   /* horizon: no roll */
    mem_wf32(buf + 0x48, 1.5f);   /* distance to the sensor bar, metres */
    mem_wf32(buf + 0x54, 1.0f);
    mem_w8(buf + 0x5C, WPAD_DEV_CORE);
    mem_w8(buf + 0x5D, WPAD_ERR_NONE);
    mem_w8(buf + 0x5E, 2);        /* both sensor bar dots visible */
    mem_w8(buf + 0x5F, 0);

    prev[chan].hold = hold;
    prev[chan].pos[0] = px;
    prev[chan].pos[1] = py;
    if (err) mem_w32(err, WPAD_ERR_NONE);
    c->r[3] = 1;
}

void hle_WUDInit(PPCContext* c) {
    c->r[3] = 0;
}
