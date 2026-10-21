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
    float acc[3];
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
    float ax = r->accel[0], ay = r->accel[1], az = r->accel[2];
    float dx = ax - prev[chan].acc[0], dy = ay - prev[chan].acc[1], dz = az - prev[chan].acc[2];
    mem_wf32(buf + 0x0C, ax);
    mem_wf32(buf + 0x10, ay);
    mem_wf32(buf + 0x14, az);
    mem_wf32(buf + 0x18, sqrtf(ax * ax + ay * ay + az * az));   /* acc_value */
    mem_wf32(buf + 0x1C, sqrtf(dx * dx + dy * dy + dz * dz));   /* acc_speed: what shake checks read */
    prev[chan].acc[0] = ax; prev[chan].acc[1] = ay; prev[chan].acc[2] = az;
    mem_wf32(buf + 0x20, px);
    mem_wf32(buf + 0x24, py);
    mem_wf32(buf + 0x28, vx);
    mem_wf32(buf + 0x2C, vy);
    mem_wf32(buf + 0x30, sqrtf(vx * vx + vy * vy));
    float lv = sqrtf(ay * ay + az * az), lh = sqrtf(ay * ay + ax * ax);
    mem_wf32(buf + 0x34, lh > 0 ? -ay / lh : 1.0f);   /* horizon: the roll around the Remote */
    mem_wf32(buf + 0x38, lh > 0 ? ax / lh : 0.0f);
    mem_wf32(buf + 0x48, 1.5f);   /* distance to the sensor bar, metres */
    mem_wf32(buf + 0x54, lv > 0 ? -ay / lv : 1.0f);   /* acc_vertical: the tilt along the Remote */
    mem_wf32(buf + 0x58, lv > 0 ? az / lv : 0.0f);
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

/* The Bluetooth stack never runs, so WPAD's per-Remote command queues are never set up and any
 * WPAD call that queues a command would write through a null queue. The game only uses the ones
 * below; each finishes at once, reporting success to its callback the way a real Remote would. */
static int speaker_on[MAX_PLAYERS];

static void call_wpad_callback(PPCContext* c, uint32_t cb, int chan, int result) {
    if (!cb) return;
    c->r[3] = (uint32_t)chan;
    c->r[4] = (uint32_t)result;
    ppc_call_indirect(c, cb);
}

/* s32 WPADGetInfoAsync(s32 chan, WPADInfo* info, WPADCallback cb) */
void hle_WPADGetInfoAsync(PPCContext* c) {
    int chan = (int32_t)c->r[3];
    uint32_t info = c->r[4], cb = c->r[5];
    if (!connected(chan)) { c->r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER; return; }
    if (info) {
        mem_w32(info + 0x00, 1);              /* dpd (pointer camera) on */
        mem_w32(info + 0x04, (uint32_t)speaker_on[chan]);
        mem_w32(info + 0x08, 0);              /* nothing attached */
        mem_w32(info + 0x0C, 0);              /* battery not low */
        mem_w32(info + 0x10, 0);
        mem_w8(info + 0x14, 4);               /* battery level: full */
        mem_w8(info + 0x15, (uint8_t)(1 << chan));  /* player LED */
        mem_w8(info + 0x16, 0);
        mem_w8(info + 0x17, 0);
    }
    call_wpad_callback(c, cb, chan, WPAD_ERR_NONE);
    c->r[3] = WPAD_ERR_NONE;
}

/* BOOL WPADIsSpeakerEnabled(s32 chan) */
void hle_WPADIsSpeakerEnabled(PPCContext* c) {
    int chan = (int32_t)c->r[3];
    c->r[3] = (uint32_t)(chan >= 0 && chan < MAX_PLAYERS && speaker_on[chan]);
}

/* s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback cb): command 0 switches it off */
void hle_WPADControlSpeaker(PPCContext* c) {
    int chan = (int32_t)c->r[3];
    uint32_t cmd = c->r[4], cb = c->r[5];
    if (!connected(chan)) { c->r[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER; return; }
    int was = speaker_on[chan];
    speaker_on[chan] = cmd != 0;
    if (cmd != 0 || was) call_wpad_callback(c, cb, chan, WPAD_ERR_NONE);
    c->r[3] = WPAD_ERR_NONE;
}

/* BOOL WPADCanSendStreamData(s32 chan) */
void hle_WPADCanSendStreamData(PPCContext* c) {
    c->r[3] = (uint32_t)connected((int32_t)c->r[3]);
}

/* s32 WPADSendStreamData(s32 chan, void* buf, u32 len): Remote speaker audio, which is dropped */
void hle_WPADSendStreamData(PPCContext* c) {
    c->r[3] = connected((int32_t)c->r[3]) ? WPAD_ERR_NONE : (uint32_t)WPAD_ERR_NO_CONTROLLER;
}

/* void WPADDisconnect(s32 chan): keyboard and pad players stay connected */
void hle_WPADDisconnect(PPCContext* c) {
    (void)c;
}
