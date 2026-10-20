/* Recompiled REL modules (see recomp/rel.py). Each module is compiled at a virtual base; the
 * runtime maps the real heap address OSLink chose to that base. */
#pragma once
#include "ppc_context.h"

typedef struct {
    uint32_t id, virt_base, size;
    const PPCFuncEntry* funcs;
    unsigned count;
    const char* name;
} RelModule;

extern const RelModule* const g_rel_modules[];
extern const unsigned g_rel_module_count;
extern uint32_t g_rel_base[256];

PPCFunc rel_lookup(uint32_t real_addr);
