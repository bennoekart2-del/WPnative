/* Linked when no recompiled game code is available, so the runtime still builds for --selftest. */
#include "ppc_context.h"

const PPCFuncEntry g_recomp_funcs[1] = {{0, 0}};
const unsigned g_recomp_func_count = 0;
const uint32_t g_recomp_entry = 0;

#include "rel_module.h"
const RelModule* const g_rel_modules[1] = {0};
const unsigned g_rel_module_count = 0;
