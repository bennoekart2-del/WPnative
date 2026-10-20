/* GX (Flipper/Hollywood GPU) front end. The game streams commands into the write-gather pipe;
 * we collect them here and decode them into draw calls for the host renderer.
 * Command format reference: Dolphin's VideoCommon/OpcodeDecoding.cpp, YAGCD chapter 5.
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void gx_fifo_write(uint32_t val, int size);
uint32_t gx_cp_read(uint32_t addr, int size);
void gx_cp_write(uint32_t addr, uint32_t val, int size);
void gx_flush(void);  /* decode everything queued so far */
uint32_t gx_pe_read(uint32_t addr, int size);
void gx_pe_write(uint32_t addr, uint32_t val, int size);
extern int g_xfb_w, g_xfb_h, g_xfb_stride;

typedef struct {
    uint64_t draws, vertices, bp_writes, cp_writes, xf_writes, display_lists, efb_copies, unknown;
} GxStats;
const GxStats* gx_stats(void);

#ifdef __cplusplus
}
#endif
