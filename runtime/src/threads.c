/* Guest threads and interrupts.
 *
 * Every Wii OS thread runs on its own host thread with its own PPCContext, but only one runs at a
 * time (a baton is passed), exactly like the single-core Broadway. The OS's own scheduler code runs
 * unchanged; we only replace the two primitives it switches with:
 *
 *   OSSaveContext(ctx)  -> remember that `ctx` belongs to the calling host thread; return 0.
 *   OSLoadContext(ctx)  -> if ctx is ours, just return (interrupt return / no switch).
 *                          otherwise hand the baton to ctx's host thread (starting one if ctx is a
 *                          fresh thread) and sleep until someone loads our context again, then
 *                          return into the scheduler code, which unwinds as if nothing happened.
 *
 * Interrupts are taken at safe points (loop back-edges, MMIO reads, interrupt enables): we save the
 * guest registers into the current OSContext like the exception vector would, and call the OS's
 * handler from its exception table at 0x80003000.
 */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppc_context.h"
#include "runtime.h"

#define MAX_THREADS 256

typedef struct {
    uint32_t ctx;          /* guest OSContext address this host thread owns */
    SDL_Semaphore* run;    /* posted when this thread gets the baton */
    PPCContext* cpu;
    int used;
    int in_irq;
    uint32_t irq_ctx;      /* context handed to the exception handler running on this thread */
    /* coroutines (guest setjmp/longjmp stack switching), see hle_longjmp */
    int is_coro, cmd;
    void** resume;         /* host setjmp buffer to resume at (cmd == CORO_RESUME) */
    uint32_t entry;        /* guest address to start at (cmd == CORO_START) */
    void* top[5];          /* host buffer at the coroutine thread's base, for restarts */
} GuestThread;

enum { CORO_NONE, CORO_RESUME, CORO_START };

static GuestThread threads[MAX_THREADS];
static SDL_Mutex* table_lock;
static SDL_TLSID tls_self;

volatile int g_irq_pending;
static SDL_AtomicInt pi_cause;  /* PI interrupt cause bits raised by devices */
static uint32_t pi_mask;
static SDL_AtomicInt dec_fired;

static GuestThread* self(void) { return SDL_GetTLS(&tls_self); }

static void spawn(SDL_ThreadFunction fn, const char* name, size_t stack, void* arg) {
    SDL_PropertiesID p = SDL_CreateProperties();
    SDL_SetPointerProperty(p, SDL_PROP_THREAD_CREATE_ENTRY_FUNCTION_POINTER, (void*)fn);
    SDL_SetStringProperty(p, SDL_PROP_THREAD_CREATE_NAME_STRING, name);
    SDL_SetPointerProperty(p, SDL_PROP_THREAD_CREATE_USERDATA_POINTER, arg);
    SDL_SetNumberProperty(p, SDL_PROP_THREAD_CREATE_STACKSIZE_NUMBER, (Sint64)stack);
    SDL_Thread* t = SDL_CreateThreadWithProperties(p);
    SDL_DestroyProperties(p);
    if (!t) { fprintf(stderr, "[thread] cannot create host thread: %s\n", SDL_GetError()); abort(); }
    SDL_DetachThread(t);
}

static GuestThread* alloc_thread(uint32_t ctx, PPCContext* cpu) {
    SDL_LockMutex(table_lock);
    for (int i = 0; i < MAX_THREADS; i++) {
        if (!threads[i].used) {
            threads[i] = (GuestThread){ctx, SDL_CreateSemaphore(0), cpu, 1, 0, 0, 0, 0, NULL, 0, {0}};
            SDL_UnlockMutex(table_lock);
            return &threads[i];
        }
    }
    SDL_UnlockMutex(table_lock);
    fprintf(stderr, "[thread] too many guest threads\n");
    abort();
}

static GuestThread* find_thread(uint32_t ctx) {
    for (int i = 0; i < MAX_THREADS; i++)
        if (threads[i].used && threads[i].ctx == ctx) return &threads[i];
    return NULL;
}

static void claim_ctx(uint32_t ctx) {
    GuestThread* me = self();
    GuestThread* other = find_thread(ctx);
    if (other && other != me) other->ctx = 0; /* context memory reused by a new owner */
    me->ctx = ctx;
}

void threads_init_main(PPCContext* cpu) {
    table_lock = SDL_CreateMutex();
    GuestThread* t = alloc_thread(0, cpu);
    SDL_SetTLS(&tls_self, t, NULL);
}

/* ------------------------------------------------------------------ OSContext <-> PPCContext */
static void ctx_load(PPCContext* c, uint32_t ctx) {
    for (int i = 0; i < 32; i++) c->r[i] = mem_r32(ctx + 4 * i);
    ppc_mtcrf(c, 0xFF, mem_r32(ctx + 0x80));
    c->lr = mem_r32(ctx + 0x84);
    c->ctr = mem_r32(ctx + 0x88);
    ppc_mtspr(c, 1, mem_r32(ctx + 0x8C));
    for (int i = 0; i < 32; i++) c->f[i].u = mem_r64(ctx + 0x90 + 8 * i);
    for (int i = 0; i < 8; i++) c->gqr[i] = mem_r32(ctx + 0x1A4 + 4 * i);
    for (int i = 0; i < 32; i++) { PPCFpr p; p.u = mem_r64(ctx + 0x1C8 + 8 * i); c->ps1[i] = p.d; }
    c->msr = mem_r32(ctx + 0x19C);
}

static void ctx_store(PPCContext* c, uint32_t ctx, uint32_t pc) {
    for (int i = 0; i < 32; i++) mem_w32(ctx + 4 * i, c->r[i]);
    mem_w32(ctx + 0x80, ppc_get_cr(c));
    mem_w32(ctx + 0x84, c->lr);
    mem_w32(ctx + 0x88, c->ctr);
    mem_w32(ctx + 0x8C, ppc_mfspr(c, 1));
    mem_w32(ctx + 0x198, pc);
    mem_w32(ctx + 0x19C, c->msr);
}

/* ------------------------------------------------------------------ host thread for a fresh guest thread */
static int SDLCALL guest_entry(void* arg) {
    GuestThread* t = arg;
    SDL_SetTLS(&tls_self, t, NULL);
    SDL_WaitSemaphore(t->run);
    PPCContext* c = t->cpu;
    uint32_t entry = mem_r32(t->ctx + 0x198);
    uint32_t exit_fn = c->lr;
    fprintf(stderr, "[thread] start ctx %08X entry %08X lr %08X sp %08X\n", t->ctx, entry, exit_fn, c->r[1]);
    ppc_call_indirect(c, entry);
    /* thread function returned: the OS set LR to OSExitThread for exactly this */
    c->lr = exit_fn;
    ppc_call_indirect(c, exit_fn);
    fprintf(stderr, "[thread] guest thread %08X fell off OSExitThread\n", t->ctx);
    for (;;) SDL_WaitSemaphore(t->run);
}

static void switch_to(GuestThread* target) {
    GuestThread* me = self();
    SDL_SignalSemaphore(target->run);
    SDL_WaitSemaphore(me->run);
}

/* ------------------------------------------------------------------ HLE entry points */
static int trace_threads = -1;
#define TTRACE(...) do { if (trace_threads < 0) trace_threads = getenv("WPR_TRACE_THREADS") != NULL; \
    if (trace_threads) fprintf(stderr, __VA_ARGS__); } while (0)

void hle_OSSaveContext(PPCContext* c) {
    TTRACE("[thread] save %08X (self %08X lr %08X)\n", c->r[3], self()->ctx, c->lr);
    claim_ctx(c->r[3]);
    c->r[3] = 0;
}

void hle_OSSetCurrentContext(PPCContext* c) {
    uint32_t ctx = c->r[3];
    TTRACE("[thread] setcurrent %08X (self %08X lr %08X)\n", ctx, self()->ctx, c->lr);
    mem_w32(0x800000D4, ctx);
    mem_w32(0x800000C0, ctx & 0x3FFFFFFF);
    if (!self()->ctx) claim_ctx(ctx);
}

void hle_OSLoadContext(PPCContext* c) {
    uint32_t ctx = c->r[3];
    GuestThread* me = self();
    TTRACE("[thread] load %08X (self %08X lr %08X)\n", ctx, me->ctx, c->lr);
    if (me->ctx == ctx || me->irq_ctx == ctx) return;
    GuestThread* t = find_thread(ctx);
    if (!t) {
        PPCContext* nc = calloc(1, sizeof *nc);
        ctx_load(nc, ctx);
        t = alloc_thread(ctx, nc);
        spawn(guest_entry, "guest", 1 << 20, t);
    }
    switch_to(t);
}

/* ------------------------------------------------------------------ coroutines: setjmp / longjmp
 *
 * The game's task system gives every task its own stack and switches between them with setjmp and
 * longjmp (fill a jmp_buf with setjmp, then point its LR/SP at a trampoline and a fresh stack).
 * Recompiled code cannot jump into the middle of a C function, so each guest stack becomes a host
 * thread, and only one runs at a time (same baton as OS threads):
 *
 *   setjmp call sites run a host setjmp first (PPC_SETJMP), remembering which host thread and
 *   which host frame the guest buffer belongs to, and the LR/SP the guest saved.
 *   longjmp(buf) to a buffer still holding those LR/SP resumes that frame: directly on the same
 *   host thread, otherwise by waking its thread. A buffer with a rewritten LR/SP starts a new
 *   coroutine at that LR (reusing the thread that last owned the buffer when possible).
 *
 * The running guest OS thread's identity (its OSContext) moves along with every switch.
 */
#define MAX_JMP 512
typedef struct { uint32_t buf, lr, sp; GuestThread* owner; void* jb[5]; } JmpSlot;
static JmpSlot jslots[MAX_JMP];

static JmpSlot* jslot(uint32_t buf, int create) {
    for (int i = 0; i < MAX_JMP; i++) if (jslots[i].buf == buf) return &jslots[i];
    if (!create) return NULL;
    for (int i = 0; i < MAX_JMP; i++) if (!jslots[i].buf) { jslots[i].buf = buf; return &jslots[i]; }
    fprintf(stderr, "[coro] out of jmp_buf slots\n");
    abort();
}

void** wpr_setjmp_begin(PPCContext* c) {
    JmpSlot* s = jslot(c->r[3], 1);
    s->owner = self();
    s->lr = c->lr;
    s->sp = c->r[1];
    return s->jb;
}

static void jmp_restore(PPCContext* c, uint32_t buf, uint32_t v) {
    c->lr = mem_r32(buf);
    ppc_mtcrf(c, 0xFF, mem_r32(buf + 4));
    c->r[1] = mem_r32(buf + 8);
    c->r[2] = mem_r32(buf + 0xC);
    for (int r = 13; r < 32; r++) c->r[r] = mem_r32(buf + 0x14 + 4 * (uint32_t)(r - 13));
    for (int f = 14; f < 32; f++) {
        uint32_t o = buf + 0x60 + 0x10 * (uint32_t)(f - 14);
        c->f[f].u = mem_r64(o);
        ppc_psq_l(c, (uint32_t)f, o + 8, 0, 0);
    }
    c->fpscr = (uint32_t)mem_r64(buf + 0x180);
    c->r[3] = v ? v : 1;
}

static void coro_dispatch(GuestThread* me) {
    int cmd = me->cmd;
    me->cmd = CORO_NONE;
    if (cmd == CORO_RESUME) __builtin_longjmp(me->resume, 1);
    if (cmd == CORO_START) __builtin_longjmp(me->top, 1);
    fprintf(stderr, "[coro] woken without a command\n");
    abort();
}

static int SDLCALL coro_main(void* arg) {
    GuestThread* t = arg;
    SDL_SetTLS(&tls_self, t, NULL);
    SDL_WaitSemaphore(t->run);
    if (__builtin_setjmp(t->top) == 0) t->cmd = CORO_START;
    t->cmd = CORO_NONE;
    for (;;) {
        /* LR = entry when it starts, so a guest return from it re-enters it, as on hardware */
        ppc_call_indirect(t->cpu, t->entry);
    }
    return 0;
}

static void coro_switch(GuestThread* me, GuestThread* t) {
    t->ctx = me->ctx;
    me->ctx = 0;
    switch_to(t);
    coro_dispatch(me);
}

void hle_longjmp(PPCContext* c) {
    uint32_t buf = c->r[3], v = c->r[4];
    uint32_t lr = mem_r32(buf), sp = mem_r32(buf + 8);
    GuestThread* me = self();
    JmpSlot* s = jslot(buf, 0);
    if (s && s->owner && s->lr == lr && s->sp == sp) {
        GuestThread* t = s->owner;
        if (t == me) {
            jmp_restore(c, buf, v);
            __builtin_longjmp(s->jb, 1);
        }
        *t->cpu = *c;
        jmp_restore(t->cpu, buf, v);
        t->cmd = CORO_RESUME;
        t->resume = s->jb;
        coro_switch(me, t);
    }
    GuestThread* t = (s && s->owner && s->owner->is_coro && s->owner != me) ? s->owner : NULL;
    if (s) s->owner = NULL;
    int fresh = t == NULL;
    if (fresh) {
        t = alloc_thread(0, calloc(1, sizeof(PPCContext)));
        t->is_coro = 1;
    }
    *t->cpu = *c;
    jmp_restore(t->cpu, buf, v);
    t->entry = lr;
    t->cmd = CORO_START;
    TTRACE("[coro] start %08X sp %08X on %s thread\n", lr, sp, fresh ? "new" : "reused");
    if (fresh) spawn(coro_main, "coro", 4 << 20, t);
    coro_switch(me, t);
}

/* ------------------------------------------------------------------ interrupts */
static void update_pending(void) {
    g_irq_pending = ((uint32_t)SDL_GetAtomicInt(&pi_cause) & pi_mask) != 0 || SDL_GetAtomicInt(&dec_fired);
}

void irq_raise(uint32_t cause) {
    int old;
    do { old = SDL_GetAtomicInt(&pi_cause); } while (!SDL_CompareAndSwapAtomicInt(&pi_cause, old, old | (int)cause));
    update_pending();
}

void irq_clear(uint32_t cause) {
    int old;
    do { old = SDL_GetAtomicInt(&pi_cause); } while (!SDL_CompareAndSwapAtomicInt(&pi_cause, old, old & ~(int)cause));
    update_pending();
}

void irq_decrementer(void) { SDL_SetAtomicInt(&dec_fired, 1); g_irq_pending = 1; }

uint32_t pi_read(uint32_t addr) {
    switch (addr & 0xFF) {
    case 0x00: return (uint32_t)SDL_GetAtomicInt(&pi_cause);
    case 0x04: return pi_mask;
    case 0x2C: return 0x20000000; /* PI_FLIPPER_REV: Hollywood */
    default: return 0;
    }
}

void pi_write(uint32_t addr, uint32_t v) {
    switch (addr & 0xFF) {
    case 0x00: irq_clear(v & 0x3000); break;  /* only reset-switch bits are write-to-clear */
    case 0x04: pi_mask = v; update_pending(); break;
    default: break;
    }
}

static void take_exception(PPCContext* c, int exc) {
    uint32_t ctx = mem_r32(0x800000D4);
    uint32_t handler = mem_r32(0x80003000 + 4 * exc);
    if (!ctx || !handler) return;
    TTRACE("[thread] exception %d ctx %08X handler %08X (self %08X)\n", exc, ctx, handler, self()->ctx);
    GuestThread* me = self();
    uint32_t prev_irq_ctx = me->irq_ctx;
    me->irq_ctx = ctx;
    PPCContext saved = *c;
    ctx_store(c, ctx, 0);
    c->msr &= ~0x8000u;  /* EE off while handling */
    c->r[3] = (uint32_t)exc;
    c->r[4] = ctx;
    c->spr[26] = 0; c->spr[27] = saved.msr;
    ppc_call_indirect(c, handler);
    /* the handler finished with OSLoadContext(ctx): resume where we were */
    *c = saved;
    me->irq_ctx = prev_irq_ctx;
}

void ppc_check_irq(PPCContext* c);
void ppc_check_irq_current(void) { ppc_check_irq(self()->cpu); }

void ppc_check_irq(PPCContext* c) {
    /* MSR.EE is the only guard: handlers run with EE off, and the OS turns it back on when it idles
     * or switches threads from inside a handler, which is when nested interrupts must be taken. */
    if (!(c->msr & 0x8000u)) return;
    if (SDL_GetAtomicInt(&dec_fired)) {
        SDL_SetAtomicInt(&dec_fired, 0);
        update_pending();
        take_exception(c, 8);
    }
    if ((c->msr & 0x8000u) && ((uint32_t)SDL_GetAtomicInt(&pi_cause) & pi_mask)) take_exception(c, 4);
    update_pending();
}

/* ------------------------------------------------------------------ timers: decrementer and VI retrace */
static SDL_AtomicInt dec_armed;
static uint64_t dec_deadline_ns;
void vi_retrace(void);  /* hw.c */

void dec_write(uint32_t ticks) {
    dec_deadline_ns = SDL_GetTicksNS() + (uint64_t)ticks * 4000 / 243;
    SDL_SetAtomicInt(&dec_armed, (int32_t)ticks >= 0);
    if ((int32_t)ticks < 0) irq_decrementer();
}

uint32_t dec_read(void) {
    int64_t left = (int64_t)(dec_deadline_ns - SDL_GetTicksNS());
    return (uint32_t)(left * 243 / 4000);
}

static int SDLCALL timer_thread(void* arg) {
    (void)arg;
    uint64_t next_vi = SDL_GetTicksNS();
    for (;;) {
        uint64_t now = SDL_GetTicksNS();
        if (SDL_GetAtomicInt(&dec_armed) && now >= dec_deadline_ns) {
            SDL_SetAtomicInt(&dec_armed, 0);
            irq_decrementer();
        }
        if (now >= next_vi) {
            next_vi += 1000000000ull / 60;
            vi_retrace();
        }
        SDL_DelayPrecise(250000);
    }
    return 0;
}

void timers_start(void) { SDL_DetachThread(SDL_CreateThread(timer_thread, "timers", NULL)); }
