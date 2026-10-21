/* Deterministic mode (netplay): see det.c. */
#pragma once
#include <stdint.h>

struct PPCContext;
extern int g_det;                       /* 1: virtual guest time, device events at fixed points */
void det_init(void);
uint64_t det_now_ns(void);              /* guest time: virtual in deterministic mode, else host time */
void det_poll(struct PPCContext* c);    /* every safe point (threads.c, ppc_check_irq) */
void det_frame(void);                   /* each VI retrace: pacing, netplay input, state hash */
void det_mark_gpu(uint32_t lo, uint32_t hi);  /* guest memory the GPU wrote (EFB copies): not hashed */

/* threads.c */
uint64_t timers_next(void);             /* when the next timer event is due (guest ns) */
void timers_run(uint64_t now);
/* gx_fifo.c: wait for the GPU thread, then deliver the PE interrupts it produced */
void gx_det_sync(void);
