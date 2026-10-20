/* Finding recompiled REL code for a real guest address.
 *
 * OSLink keeps every linked module on a list (head at 0x800030C8, each header: id +0, next +4).
 * The module's code sits inside its header block, so real = header + offset and the recompiled
 * function lives at virt_base + offset. g_rel_base[id] is updated on every lookup, which is how
 * generated code (RB(), relocated immediates) learns where its module is loaded.
 */
#include <stdio.h>

#include "rel_module.h"

uint32_t g_rel_base[256];

static const RelModule* module_by_id(uint32_t id) {
    for (unsigned i = 0; i < g_rel_module_count; i++)
        if (g_rel_modules[i]->id == id) return g_rel_modules[i];
    return NULL;
}

static PPCFunc find_in(const RelModule* m, uint32_t v) {
    unsigned lo = 0, hi = m->count;
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < v) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->count && m->funcs[lo].addr == v ? m->funcs[lo].fn : NULL;
}

PPCFunc rel_lookup(uint32_t addr) {
    uint32_t hdr = mem_r32(0x800030C8);
    for (int guard = 0; hdr && guard < 64; guard++, hdr = mem_r32(hdr + 4)) {
        uint32_t id = mem_r32(hdr);
        const RelModule* m = module_by_id(id);
        if (!m) {
            static uint32_t warned[256];
            if (id < 256 && !warned[id]++) fprintf(stderr, "[rel] module %u is loaded but was not recompiled\n", id);
            continue;
        }
        if (addr < hdr || addr >= hdr + m->size) continue;
        g_rel_base[id & 255] = hdr;
        PPCFunc f = find_in(m, m->virt_base + (addr - hdr));
        if (f) return f;
        fprintf(stderr, "[rel] %08X is inside %s (+%X) but not a function start\n", addr, m->name, addr - hdr);
        return NULL;
    }
    return NULL;
}
