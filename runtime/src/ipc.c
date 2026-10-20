/* Hollywood IPC (0xCD000000) and the IOS side of it.
 *
 * Register behaviour follows Dolphin's HW/WII_IPC.cpp; the request/reply flow follows
 * IOS::EmulationKernel::UpdateIPC: a request is acked (Y2), executed, and its reply (Y1) is
 * delivered once the PPC has consumed the previous one. IOS devices are emulated in ios_*.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ios.h"
#include "ppc_context.h"

void irq_raise(uint32_t cause);
void irq_clear(uint32_t cause);
#define PI_CAUSE_WII_IPC 0x4000u
#define IRQ_IPC_BROADWAY 0x40000000u

static uint32_t ppc_msg, arm_msg, irq_flags, irq_mask;
static uint8_t X1, X2, Y1, Y2, IY1, IY2;
static uint32_t req_q[64], rep_q[64];
static int req_n, rep_n;

static void update_irq(void) {
    if ((Y1 & IY1) || (Y2 & IY2)) irq_flags |= IRQ_IPC_BROADWAY;
    if (irq_flags & irq_mask) irq_raise(PI_CAUSE_WII_IPC); else irq_clear(PI_CAUSE_WII_IPC);
}

static int ready(void) { return !Y1 && !Y2 && !(irq_flags & IRQ_IPC_BROADWAY); }

void ios_enqueue_reply(uint32_t addr) {
    if (rep_n < 64) rep_q[rep_n++] = addr;
}

static void execute(uint32_t addr);

static void update_ipc(void) {
    if (!ready()) return;
    if (req_n) {
        uint32_t addr = req_q[0];
        memmove(req_q, req_q + 1, sizeof(uint32_t) * (size_t)--req_n);
        X1 = 0;
        Y2 = 1;  /* ack */
        update_irq();
        execute(addr);
        return;
    }
    if (rep_n) {
        arm_msg = rep_q[0];
        memmove(rep_q, rep_q + 1, sizeof(uint32_t) * (size_t)--rep_n);
        Y1 = 1;  /* reply ready */
        update_irq();
    }
}

uint32_t ipc_read(uint32_t addr) {
    switch (addr & 0xFF) {
    case 0x00: return ppc_msg;
    case 0x04: return (uint32_t)((IY2 << 5) | (IY1 << 4) | (X2 << 3) | (Y1 << 2) | (Y2 << 1) | X1);
    case 0x08: return arm_msg;
    case 0x30: return irq_flags;
    case 0x34: return irq_mask;
    default: return 0;
    }
}

void ipc_write(uint32_t addr, uint32_t v) {
    if (getenv("WPR_TRACE_IPC")) fprintf(stderr, "[ipc] write %02X = %08X\n", addr & 0xFF, v);
    switch (addr & 0xFF) {
    case 0x00: ppc_msg = v; break;
    case 0x04:
        X1 = v & 1;
        X2 = (v >> 3) & 1;
        if ((v >> 2) & 1) Y1 = 0;
        if ((v >> 1) & 1) Y2 = 0;
        IY1 = (v >> 4) & 1;
        IY2 = (v >> 5) & 1;
        if (X1 && req_n < 64) req_q[req_n++] = ppc_msg;
        update_irq();
        break;
    case 0x30: irq_flags &= ~v; update_irq(); break;
    case 0x34: irq_mask = v; update_irq(); break;
    default: break;
    }
    update_ipc();
    update_ipc();
}

/* ------------------------------------------------------------------ IOS kernel: fd table + dispatch */
#define MAX_FDS 32
static const IosDevice* fd_dev[MAX_FDS];
static void* fd_state[MAX_FDS];

static void reply(uint32_t req, int32_t result) {
    uint32_t cmd = mem_r32(req);
    mem_w32(req + 4, (uint32_t)result);
    mem_w32(req + 8, cmd);
    mem_w32(req, IPC_REPLY);
    ios_enqueue_reply(req);
}

static void execute(uint32_t addr) {
    uint32_t req = addr | 0x80000000u;
    IosRequest r;
    r.addr = req;
    r.cmd = mem_r32(req);
    r.fd = (int32_t)mem_r32(req + 8);
    for (int i = 0; i < 5; i++) r.arg[i] = mem_r32(req + 0xC + 4 * i);
    static int trace = -1;
    if (trace < 0) trace = getenv("WPR_TRACE_IPC") != NULL;
    if (trace) fprintf(stderr, "[ipc] req %08X cmd %u fd %d args %08X %08X %08X\n", req, r.cmd, r.fd, r.arg[0], r.arg[1], r.arg[2]);

    if (r.cmd == IPC_OPEN) {
        char path[64] = {0};
        uint32_t p = r.arg[0];
        for (int i = 0; i < 63; i++) { path[i] = (char)mem_r8(p + i); if (!path[i]) break; }
        const IosDevice* dev = ios_find_device(path);
        int fd = -1;
        for (int i = 0; i < MAX_FDS; i++) if (!fd_dev[i]) { fd = i; break; }
        if (!dev) { fprintf(stderr, "[ios] open %s: no such device\n", path); reply(req, -6); return; }
        if (fd < 0) { reply(req, -5); return; }
        void* st = NULL;
        int32_t res = dev->open ? dev->open(path, r.arg[1], &st) : 0;
        fprintf(stderr, "[ios] open %s -> %d\n", path, res < 0 ? res : fd);
        if (res < 0) { reply(req, res); return; }
        fd_dev[fd] = dev;
        fd_state[fd] = st;
        reply(req, fd);
        return;
    }
    if (r.fd < 0 || r.fd >= MAX_FDS || !fd_dev[r.fd]) { reply(req, -4); return; }
    const IosDevice* dev = fd_dev[r.fd];
    void* st = fd_state[r.fd];
    int32_t res = 0;
    switch (r.cmd) {
    case IPC_CLOSE:
        if (dev->close) dev->close(st);
        fd_dev[r.fd] = NULL;
        break;
    case IPC_READ: res = dev->read ? dev->read(st, r.arg[0], r.arg[1]) : -4; break;
    case IPC_WRITE: res = dev->write ? dev->write(st, r.arg[0], r.arg[1]) : -4; break;
    case IPC_SEEK: res = dev->seek ? dev->seek(st, (int32_t)r.arg[0], r.arg[1]) : -4; break;
    case IPC_IOCTL:
        res = dev->ioctl ? dev->ioctl(st, r.arg[0], r.arg[1], r.arg[2], r.arg[3], r.arg[4]) : -4;
        break;
    case IPC_IOCTLV: {
        IosVec v[32];
        uint32_t n_in = r.arg[1], n_io = r.arg[2], vp = r.arg[3];
        for (uint32_t i = 0; i < n_in + n_io && i < 32; i++) {
            v[i].addr = mem_r32(vp + 8 * i);
            v[i].size = mem_r32(vp + 8 * i + 4);
        }
        res = dev->ioctlv ? dev->ioctlv(st, r.arg[0], n_in, n_io, v) : -4;
        break;
    }
    default:
        fprintf(stderr, "[ios] unknown command %u\n", r.cmd);
        res = -4;
    }
    if (res == IOS_NO_REPLY) return;  /* device replies later (e.g. blocking reads) */
    reply(req, res);
}

void ios_reply_later(uint32_t req, int32_t result) {
    reply(req, result);
    update_ipc();
}
