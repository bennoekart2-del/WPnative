#!/usr/bin/env python3
"""Recompile REL modules (the game's dynamically linked code: boot screen, menus, minigames).

A REL is linked by the game's own OSLink at a heap address only known at run time. We recompile it
once at a fixed *virtual* base per module id (VB = 0x40000000 + id * 4 MB): relocations that point
into the module itself are applied with virtual addresses, so branches, jump tables and vtables can
be analysed statically. At run time:

  * the runtime finds loaded modules through the OS module list (0x800030C8) and maps a real address
    to its virtual twin (real - module_header + VB) for indirect calls;
  * immediates that OSLink patches with module addresses (ADDR16 relocations against the module) are
    read back from the linked code in guest memory, so they are always the real ones;
  * link registers are computed as real addresses (RB()), and bctr dispatch maps CTR back.

Branches into the main DOL become direct calls to its recompiled functions.

Usage:
    python3 recomp/rel.py --rel files/rel/boot.rel.lz [...] --dol-gen build/gen --out build/gen
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from dataclasses import dataclass

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from emit import Emitter, SymImm, fn, h  # noqa: E402
from formats import (R_PPC_ADDR16, R_PPC_ADDR16_HA, R_PPC_ADDR16_HI, R_PPC_ADDR16_LO,  # noqa: E402
                     R_PPC_ADDR32, R_PPC_REL14, R_PPC_REL24, parse_rel)
from lz import decompress  # noqa: E402
from ppc import Insn  # noqa: E402
from recompile import Image, discover_functions, emit_function  # noqa: E402

VIRT_BASE = 0x40000000
VIRT_STRIDE = 0x00400000


@dataclass
class VSection:
    address: int
    size: int
    data: bytes
    executable: bool


class FarInsn(Insn):
    """A branch whose target lies outside the module (in the DOL): carries the absolute target."""
    __slots__ = ("far",)

    def __init__(self, addr, word, far):
        super().__init__(addr, word)
        self.far = far

    def branch_target(self):
        return self.far


class RelImmInsn(Insn):
    """An instruction whose 16-bit immediate OSLink patches with a module address."""
    __slots__ = ()

    @property
    def simm(self):
        return SymImm(f"((uint32_t)(int32_t)(int16_t)mem_r16(RB({h(self.addr + 2)})))")

    @property
    def uimm(self):
        return SymImm(f"((uint32_t)mem_r16(RB({h(self.addr + 2)})))")


class RelImage(Image):
    def __init__(self, sections, all_sections, far, sym):
        super().__init__(sections, all_sections)
        self.far = far
        self.sym = sym

    def insn(self, addr, emit=False):
        w = self.word(addr)
        if addr in self.far:
            return FarInsn(addr, w, self.far[addr])
        if emit and addr in self.sym:
            return RelImmInsn(addr, w)
        return Insn(addr, w)


def prelink(blob: bytes):
    rel = parse_rel(blob)
    vb = VIRT_BASE + rel.module_id * VIRT_STRIDE
    bss_addr = vb + ((len(blob) + 31) & ~31)
    mem = bytearray(blob) + bytearray(bss_addr - vb - len(blob) + rel.bss_size)
    vaddr = {}
    for s in rel.sections:
        if s.size:
            vaddr[s.index] = vb + s.offset if s.offset else bss_addr
    far, sym, fptrs = {}, set(), set()
    exec_secs = {s.index for s in rel.sections if s.executable and s.size}

    def w32(a, v): struct.pack_into(">I", mem, a - vb, v & 0xFFFFFFFF)
    def w16(a, v): struct.pack_into(">H", mem, a - vb, v & 0xFFFF)
    def r32(a): return struct.unpack_from(">I", mem, a - vb)[0]

    for imp, relocs in rel.imports.items():
        if imp not in (0, rel.module_id):
            raise ValueError(f"module {rel.module_id} imports module {imp}: cross-module links not supported")
        for r in relocs:
            if r.section not in vaddr:
                continue
            p = vaddr[r.section] + r.offset
            s = r.addend if imp == 0 else vaddr.get(r.target_section, 0) + r.addend
            t = r.type
            if t == R_PPC_ADDR32:
                w32(p, s)
                if imp != 0 and r.target_section in exec_secs and r.section not in exec_secs:
                    fptrs.add(s)
            elif t in (R_PPC_ADDR16_LO, R_PPC_ADDR16_HI, R_PPC_ADDR16_HA, R_PPC_ADDR16):
                v = {R_PPC_ADDR16_LO: s, R_PPC_ADDR16: s, R_PPC_ADDR16_HI: s >> 16,
                     R_PPC_ADDR16_HA: (s + 0x8000) >> 16}[t]
                w16(p, v)
                if imp != 0 and r.section in exec_secs:
                    sym.add(p & ~3)
            elif t == R_PPC_REL24:
                if imp == 0:
                    far[p] = s
                else:
                    w32(p, (r32(p) & 0xFC000003) | ((s - p) & 0x03FFFFFC))
            elif t == R_PPC_REL14:
                if imp == 0:
                    raise ValueError("REL14 into the DOL")
                w32(p, (r32(p) & 0xFFFF0003) | ((s - p) & 0xFFFC))
            else:
                raise ValueError(f"relocation type {t} not supported")
    secs = [VSection(vaddr[s.index], s.size,
                     bytes(mem[vaddr[s.index] - vb:vaddr[s.index] - vb + s.size]), s.executable)
            for s in rel.sections if s.size]
    entries = [vaddr[sec] + off for sec, off in (rel.prolog, rel.epilog, rel.unresolved) if sec in vaddr]
    return rel, vb, secs, far, sym, entries + sorted(fptrs), len(mem)


class RelEmitter(Emitter):
    def __init__(self, known, dol_funcs, setjmp_addr=None):
        super().__init__(known, setjmp_addr)
        self.dol_funcs = dol_funcs

    def addr_expr(self, addr):
        return f"RB({h(addr)})"

    def call_expr(self, target):
        if target >= 0x80000000 and target not in self.dol_funcs:
            return f"ppc_call_indirect(c, {h(target)});"
        return f"{fn(target)}(c);"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rel", nargs="+", required=True)
    ap.add_argument("--dol-gen", required=True, help="directory with the DOL's recomp_funcs.h")
    ap.add_argument("--out", required=True)
    ap.add_argument("--chunk", type=int, default=2000)
    ap.add_argument("--hle", help="the DOL's HLE JSON (for its _setjmp address)")
    args = ap.parse_args()

    with open(os.path.join(args.dol_gen, "recomp_funcs.h")) as f:
        dol_funcs = {int(m, 16) for m in re.findall(r"void f_([0-9A-F]{8})\(", f.read())}
    setjmp_addr = None
    if args.hle:
        import json
        with open(args.hle) as f:
            cfg = json.load(f)
        setjmp_addr = int(cfg["_setjmp"], 16) if "_setjmp" in cfg else None
    os.makedirs(args.out, exist_ok=True)
    done, modules = set(), []
    for path in args.rel:
        with open(path, "rb") as f:
            blob = f.read()
        if blob[0] in (0x10, 0x11) and path.endswith(".lz"):
            blob = decompress(blob)
        rel, vb, secs, far, sym, entries, size = prelink(blob)
        if rel.module_id in done:
            continue
        done.add(rel.module_id)
        mid = rel.module_id
        img = RelImage([s for s in secs if s.executable], secs, far, sym)
        funcs = discover_functions(img, entries)
        em = RelEmitter(set(funcs), dol_funcs, setjmp_addr)
        name = os.path.basename(path).split(".")[0]
        items = sorted(funcs.items())
        header = (f'#include "ppc_context.h"\n#include "recomp_funcs.h"\n#include "rel_{mid}.h"\n'
                  f"extern uint32_t g_rel_base[256];\n"
                  f"#define RB(v) (g_rel_base[{mid}] + ((v) - {h(vb)}))\n\n")
        ctr = f"c->ctr - g_rel_base[{mid}] + {h(vb)}"
        for n in range(0, len(items), args.chunk):
            with open(os.path.join(args.out, f"rel_{mid}_{n // args.chunk:03d}.c"), "w") as f:
                f.write(header)
                for s, e in items[n:n + args.chunk]:
                    f.write(emit_function(em, img, s, e, ctr_expr=ctr))
                    f.write("\n\n")
        with open(os.path.join(args.out, f"rel_{mid}.h"), "w") as f:
            f.write("#pragma once\n#include \"ppc_context.h\"\n")
            for s in sorted(funcs):
                f.write(f"void {fn(s)}(PPCContext* c);\n")
        with open(os.path.join(args.out, f"rel_{mid}_table.c"), "w") as f:
            f.write(f'#include "rel_{mid}.h"\n#include "rel_module.h"\n\n')
            f.write(f"static const PPCFuncEntry funcs[] = {{\n")
            for s in sorted(funcs):
                f.write(f"    {{ {h(s)}, {fn(s)} }},\n")
            f.write("};\n")
            f.write(f'const RelModule g_rel_module_{mid} = {{ {mid}, {h(vb)}, {h(size)}, funcs, {len(funcs)}, "{name}" }};\n')
        total = sum((e - s) // 4 for s, e in items)
        print(f"{name}: module {mid}, {len(funcs)} functions, {total} instructions, "
              f"{sum(em.coverage.values())} untranslated")
        modules.append(mid)

    # registry of every module generated into this directory (kept across runs)
    have = sorted({int(m) for m in re.findall(r"rel_(\d+)_table\.c", " ".join(os.listdir(args.out)))})
    with open(os.path.join(args.out, "rel_registry.c"), "w") as f:
        f.write('#include "rel_module.h"\n\n')
        for m in have:
            f.write(f"extern const RelModule g_rel_module_{m};\n")
        f.write("const RelModule* const g_rel_modules[] = {\n")
        for m in have:
            f.write(f"    &g_rel_module_{m},\n")
        f.write("};\n")
        f.write(f"const unsigned g_rel_module_count = {len(have)};\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
