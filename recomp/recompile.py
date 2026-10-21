#!/usr/bin/env python3
"""Static recompiler driver: DOL (+ RELs) -> C source files for the runtime.

Usage:
    python3 recomp/recompile.py --dol path/to/main.dol --out build/gen [--rel module.rel ...]

Input is the user's own extracted disc (Dolphin: right click the game > Properties > Filesystem >
right click the disc > Extract Entire Disc). sys/main.dol is the main executable.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from emit import Emitter, fn, h, lbl  # noqa: E402
from formats import parse_dol  # noqa: E402
from ppc import Insn  # noqa: E402


class Image:
    """Flat view of every executable section, indexed by guest address."""

    def __init__(self, sections, all_sections=()):
        self.ranges = [(s.address, s.address + s.size, s.data) for s in sections]
        self.all = [(s.address, s.address + s.size, s.data) for s in (all_sections or sections)]

    def data_word(self, addr: int):
        for lo, hi, data in self.all:
            if lo <= addr and addr + 4 <= hi:
                return struct.unpack_from(">I", data, addr - lo)[0]
        return None

    def contains(self, addr: int) -> bool:
        return any(lo <= addr < hi for lo, hi, _ in self.ranges)

    def insn(self, addr: int, emit: bool = False) -> Insn:
        """Decoded instruction at addr. Subclasses (REL images) add relocation info."""
        return Insn(addr, self.word(addr))

    def word(self, addr: int) -> int:
        for lo, hi, data in self.ranges:
            if lo <= addr < hi:
                return struct.unpack_from(">I", data, addr - lo)[0]
        raise KeyError(hex(addr))

    def range_end(self, addr: int) -> int:
        for lo, hi, _ in self.ranges:
            if lo <= addr < hi:
                return hi
        raise KeyError(hex(addr))


def jump_table_targets(img: Image, start: int, bctr_addr: int) -> list[int]:
    """Find the targets of a `lis/addi; lwzx; mtctr; bctr` jump table (CodeWarrior's switch pattern).

    Tracks lis/addi/ori constants and the last `cmplwi rN, max` bound in the instructions before
    the bctr, then reads table entries from the data sections.
    """
    consts: dict[int, int] = {}
    table, bound = None, None
    for a in range(start, bctr_addr, 4):
        i = img.insn(a)
        op = i.opcd
        if op == 15 and i.ra == 0:
            consts[i.rd] = (i.uimm << 16) & 0xFFFFFFFF
        elif op == 14 and i.ra in consts:
            consts[i.rd] = (consts[i.ra] + i.simm) & 0xFFFFFFFF
        elif op == 24 and i.rs in consts:
            consts[i.ra] = consts[i.rs] | i.uimm
        elif op == 10:
            bound = i.uimm
        elif op == 31 and i.xo10 == 23:
            for r in (i.ra, i.rb):
                if r in consts:
                    table = consts[r]
        elif op in (14, 15, 24, 32, 21) and i.rd in consts:
            consts.pop(i.rd, None)
    if table is None:
        return []
    out = []
    limit = (bound + 1) if bound is not None else 1024
    for n in range(limit):
        t = img.data_word(table + 4 * n)
        if t is None or t & 3 or not (start <= t < start + 0x10000) or not img.contains(t):
            break
        out.append(t)
    return out


def _ends_block(w: int) -> bool:
    i = Insn(0, w)
    return w == 0 or i.is_return() or i.is_bctr() or i.is_uncond_branch() or w == 0x4C000064  # rfi


def pointer_candidates(img: Image) -> set[int]:
    """Functions reached only through pointers: callbacks, vtables, real-mode routines.

    Collect text addresses built in code (lis + addi/ori) or stored in data, and keep the ones that
    sit right after the end of a block, so pointers into the middle of a function (jump tables) don't
    split it.
    """
    cands = set()
    for lo, hi, data in img.ranges:
        hi_reg: dict[int, int] = {}
        for off in range(0, hi - lo, 4):
            i = img.insn(lo + off)
            if i.opcd == 15 and i.ra == 0:
                hi_reg[i.rd] = i.uimm << 16
            elif i.opcd == 14 and i.ra in hi_reg:
                cands.add((hi_reg[i.ra] + i.simm) & 0xFFFFFFFF)
            elif i.opcd == 24 and i.rs in hi_reg:
                cands.add(hi_reg[i.rs] | i.uimm)
    for lo, hi, data in img.all:
        for off in range(0, (hi - lo) & ~3, 4):
            cands.add(struct.unpack_from(">I", data, off)[0])
    # switch jump-table entries look exactly like code pointers; they are labels, not functions
    jt = set()
    for lo, hi, data in img.ranges:
        for off in range(0, hi - lo, 4):
            a = lo + off
            if img.insn(a).is_bctr():
                jt.update(jump_table_targets(img, max(lo, a - 0x100), a))
    out = set()
    for t in cands:
        if t in jt:
            continue
        if t & 3 or not img.contains(t) or img.word(t) == 0:
            continue
        if not img.contains(t - 4) or _ends_block(img.word(t - 4)):
            out.add(t)
    return out


def discover_functions(img: Image, entries: list[int]) -> dict[int, int]:
    """Return {start: end} covering all code, with no overlaps.

    Linear sweep over each text section: a function starts where the previous one ended (after
    zero padding) or at any `bl` target, and ends at the first blr / bctr / unconditional branch
    that no earlier internal branch jumps past. Tail-call targets that land inside another function
    become new starts and the sweep is repeated until nothing changes.
    """
    starts = set(e for e in entries if img.contains(e))
    for lo, hi, data in img.ranges:
        for off in range(0, hi - lo, 4):
            i = img.insn(lo + off)
            if i.is_call():
                t = i.branch_target()
                if img.contains(t):
                    starts.add(t)
    starts |= pointer_candidates(img)
    while True:
        funcs: dict[int, int] = {}
        for lo, hi, _ in img.ranges:
            a = lo
            while a < hi:
                if img.word(a) == 0:
                    a += 4
                    continue
                s, reach = a, a
                while a < hi:
                    if a != s and a in starts:
                        break
                    i = img.insn(a)
                    if i.word == 0:
                        break
                    t = i.branch_target()
                    if t is not None and not i.lk:
                        if s <= t < hi and t not in starts and t > a:
                            reach = max(reach, t)
                    if i.is_bctr():
                        for jt in jump_table_targets(img, s, a):
                            reach = max(reach, jt)
                    a += 4
                    if a > reach and (i.is_return() or i.is_bctr() or i.is_uncond_branch()):
                        break
                funcs[s] = a
        # every branch that leaves its function must land on a function start
        missing = set()
        for fs, fe in funcs.items():
            for a in range(fs, fe, 4):
                i = img.insn(a)
                t = i.branch_target()
                if t is not None and not (fs <= t < fe) and t not in funcs and img.contains(t):
                    missing.add(t)
        if not missing:
            return funcs
        starts |= missing


def emit_function(em: Emitter, img: Image, start: int, end: int, ctr_expr: str = "c->ctr") -> str:
    insns = [img.insn(a, emit=True) for a in range(start, end, 4)]
    targets = {start}
    has_bctr = False
    for i in insns:
        t = i.branch_target()
        if t is not None and not i.lk and start <= t < end:
            targets.add(t)
        if i.opcd == 19 and i.xo10 == 528 and not i.lk:
            has_bctr = True
    out = [f"void {fn(start)}(PPCContext* c) {{"]
    if has_bctr:
        # bctr can land anywhere in the function (switch jump tables): dispatch on every address.
        cases = " ".join(f"case {h(i.addr)}: goto {lbl(i.addr)};" for i in insns)
        out.append(f"#define BCTR_DISPATCH switch ({ctr_expr}) {{ {cases} default: ppc_call_indirect(c, c->ctr); return; }}")
    else:
        out.append("#define BCTR_DISPATCH ppc_call_indirect(c, c->ctr); return;")
    for i in insns:
        if has_bctr or i.addr in targets:
            out.append(f"{lbl(i.addr)}:;")
        for line in em.emit(i, start, end):
            out.append(f"    {line}")
    last = insns[-1] if insns else None
    if last is not None and not (last.is_return() or last.is_uncond_branch() or last.word == 0x4C000064
                                 or (last.is_bctr())):
        # falls through into the next function (e.g. CodeWarrior's _savegpr_N / _restgpr_N chains)
        em.called_externally.add(end)
        out.append(f"    {fn(end)}(c); return;")
    out.append("#undef BCTR_DISPATCH")
    out.append("}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--hle", help="JSON {\"80138290\": \"hle_OSSaveContext\", ...}: replace these functions")
    ap.add_argument("--chunk", type=int, default=2000, help="functions per generated .c file")
    args = ap.parse_args()

    with open(args.dol, "rb") as f:
        dol = parse_dol(f.read())
    img = Image(dol.text, dol.sections)
    funcs = discover_functions(img, [dol.entry])
    # Any guest address that is also in the table but outside known code would be a stub.
    externals = set()
    hle, setjmp_addr = {}, None
    if args.hle:
        with open(args.hle) as f:
            cfg = json.load(f)
        hle = {int(k, 16): v for k, v in cfg.items() if not k.startswith("_")}
        setjmp_addr = int(cfg["_setjmp"], 16) if "_setjmp" in cfg else None
    em = Emitter(set(funcs), setjmp_addr)
    os.makedirs(args.out, exist_ok=True)

    items = sorted(funcs.items())
    files = []
    for n in range(0, len(items), args.chunk):
        path = os.path.join(args.out, f"recomp_{n // args.chunk:03d}.c")
        with open(path, "w") as f:
            f.write('#include "ppc_context.h"\n#include "recomp_funcs.h"\n\n')
            for s, e in items[n:n + args.chunk]:
                if s in hle and hle[s].startswith("wrap:"):  # keep the original as f_X_orig; the native code calls it
                    name = hle[s][5:]
                    f.write(emit_function(em, img, s, e).replace(f"void {fn(s)}(", f"void {fn(s)}_orig(", 1))
                    f.write(f"\n\nvoid {name}(PPCContext* c);\nvoid {fn(s)}(PPCContext* c) {{ {name}(c); }}")
                elif s in hle:
                    f.write(f"void {hle[s]}(PPCContext* c);\nvoid {fn(s)}(PPCContext* c) {{ {hle[s]}(c); }}")
                else:
                    f.write(emit_function(em, img, s, e))
                f.write("\n\n")
        files.append(path)
    externals = em.called_externally - set(funcs)

    with open(os.path.join(args.out, "recomp_funcs.h"), "w") as f:
        f.write("#pragma once\n#include \"ppc_context.h\"\n")
        for s in sorted(set(funcs) | externals):
            f.write(f"void {fn(s)}(PPCContext* c);\n")
    with open(os.path.join(args.out, "recomp_table.c"), "w") as f:
        f.write('#include "recomp_funcs.h"\n\n')
        for s in sorted(externals):
            f.write(f"void {fn(s)}(PPCContext* c) {{ ppc_call_indirect(c, {h(s)}); }}\n")
        f.write("\nconst PPCFuncEntry g_recomp_funcs[] = {\n")
        for s in sorted(funcs):
            f.write(f"    {{ {h(s)}, {fn(s)} }},\n")
        f.write("};\n")
        f.write(f"const unsigned g_recomp_func_count = {len(funcs)};\n")
        f.write(f"const uint32_t g_recomp_entry = {h(dol.entry)};\n")

    total = sum(len(range(s, e, 4)) for s, e in items)
    unimpl = sum(em.coverage.values())
    report = {
        "entry": hex(dol.entry),
        "functions": len(funcs),
        "instructions": total,
        "untranslated": unimpl,
        "coverage_percent": round(100 * (1 - unimpl / max(total, 1)), 3),
        "top_untranslated": Counter(em.coverage).most_common(40),
        "sections": [{"name": s.name, "addr": hex(s.address), "size": s.size} for s in dol.sections],
        "bss": [hex(dol.bss_address), dol.bss_size],
    }
    with open(os.path.join(args.out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print(f"{len(funcs)} functions, {total} instructions, {report['coverage_percent']}% translated")
    return 0


if __name__ == "__main__":
    sys.exit(main())
