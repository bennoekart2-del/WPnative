"""Translate Broadway instructions into C statements operating on a PPCContext.

Each guest function becomes one C function `void f_XXXXXXXX(PPCContext* c)`. Branches inside the
function become gotos; `bl` becomes a direct C call; `blr` becomes `return`. Anything not yet
translated becomes a `ppc_unimpl()` call so the output always compiles and the runtime reports the
exact address it hit. `coverage` counts those so we can see what to implement next.
"""
from __future__ import annotations

from collections import Counter

from ppc import Insn, bits, mask32, sext


def h(v) -> str:
    if isinstance(v, SymImm):
        return v.expr
    return f"0x{v & 0xFFFFFFFF:08X}u"


class SymImm:
    """An immediate patched at load time (REL relocation): read from the linked code in guest memory."""

    def __init__(self, expr: str):
        self.expr = expr

    def __lshift__(self, n: int) -> "SymImm":
        return SymImm(f"({self.expr} << {n})")

    def __format__(self, spec: str) -> str:
        return f"(int32_t){self.expr}"

    def __eq__(self, other): return False
    def __hash__(self): return id(self)


def lbl(addr: int) -> str:
    return f"L_{addr:08X}"


def fn(addr: int) -> str:
    return f"f_{addr:08X}"


def ra0(i: Insn) -> str:
    return f"c->r[{i.ra}]" if i.ra else "0u"


def cr0(reg: str) -> str:
    return f"ppc_set_cr0(c, (int32_t){reg});"


class Emitter:
    def __init__(self, known_functions: set[int], setjmp_addr: int | None = None):
        self.known = known_functions
        self.setjmp_addr = setjmp_addr
        self.coverage: Counter[str] = Counter()
        self.called_externally: set[int] = set()

    # ---------------------------------------------------------------- control flow
    def _cond(self, i: Insn) -> tuple[list[str], str]:
        pre, parts = [], []
        bo, bi = i.bo, i.bi
        if not bo & 0x04:
            pre.append("c->ctr--;")
            parts.append("c->ctr == 0" if bo & 0x02 else "c->ctr != 0")
        if not bo & 0x10:
            bit = f"ppc_crbit(c, {bi})"
            parts.append(bit if bo & 0x08 else f"!{bit}")
        return pre, " && ".join(parts) if parts else "1"

    def _goto_or_call(self, target: int, start: int, end: int, link: bool, ret_addr: int) -> str:
        if link:
            self.called_externally.add(target)
            if target == self.setjmp_addr:
                return f"c->lr = {self.addr_expr(ret_addr)}; PPC_SETJMP(c, {fn(target)});"
            return f"c->lr = {self.addr_expr(ret_addr)}; {self.call_expr(target)}"
        if start <= target < end:
            if target <= self.cur:
                return f"PPC_SAFEPOINT(c); goto {lbl(target)};"
            return f"goto {lbl(target)};"
        # branch out of the function: tail call
        self.called_externally.add(target)
        return f"{self.call_expr(target)} return;"

    def addr_expr(self, addr: int) -> str:
        """C expression for a guest code address as the running program sees it (LR values)."""
        return h(addr)

    def call_expr(self, target: int) -> str:
        return f"{fn(target)}(c);"

    # ---------------------------------------------------------------- main entry
    def emit(self, i: Insn, start: int, end: int) -> list[str]:
        self.cur = i.addr
        op = i.opcd
        try:
            handler = getattr(self, f"op{op}", None)
            out = handler(i, start, end) if handler else None
        except NotImplementedError:
            out = None
        if out is None:
            self.coverage[self._name(i)] += 1
            return [f"ppc_unimpl(c, {h(i.addr)}, {h(i.word)});"]
        return out

    @staticmethod
    def _name(i: Insn) -> str:
        op = i.opcd
        if op in (4, 19, 31, 59, 63):
            return f"op{op}/xo{i.xo10}"
        return f"op{op}"

    # ---------------------------------------------------------------- immediate ALU
    def op7(self, i, *_):   # mulli
        return [f"c->r[{i.rd}] = (uint32_t)((int32_t)c->r[{i.ra}] * {i.simm});"]

    def op8(self, i, *_):   # subfic
        return [f"{{ uint64_t t = (uint64_t)(uint32_t)~c->r[{i.ra}] + {h(i.simm)} + 1; "
                f"c->r[{i.rd}] = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}"]

    def op10(self, i, *_):  # cmpli
        return [f"ppc_cmpu(c, {i.crfd}, c->r[{i.ra}], {h(i.uimm)});"]

    def op11(self, i, *_):  # cmpi
        return [f"ppc_cmps(c, {i.crfd}, (int32_t)c->r[{i.ra}], {i.simm});"]

    def _addic(self, i, rc):
        s = [f"{{ uint64_t t = (uint64_t)c->r[{i.ra}] + {h(i.simm)}; "
             f"c->r[{i.rd}] = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}"]
        if rc:
            s.append(cr0(f"c->r[{i.rd}]"))
        return s

    def op12(self, i, *_): return self._addic(i, False)
    def op13(self, i, *_): return self._addic(i, True)

    def op14(self, i, *_):  # addi / li
        return [f"c->r[{i.rd}] = {ra0(i)} + {h(i.simm)};"]

    def op15(self, i, *_):  # addis / lis
        return [f"c->r[{i.rd}] = {ra0(i)} + {h(i.uimm << 16)};"]

    def op16(self, i, start, end):  # bc
        pre, cond = self._cond(i)
        tgt = i.branch_target()
        return pre + [f"if ({cond}) {{ {self._goto_or_call(tgt, start, end, bool(i.lk), i.addr + 4)} }}"]

    def op17(self, i, *_):  # sc
        return [f"ppc_syscall(c, {h(i.addr)});"]

    def op18(self, i, start, end):  # b / bl
        return [self._goto_or_call(i.branch_target(), start, end, bool(i.lk), i.addr + 4)]

    def op19(self, i, start, end):
        xo = i.xo10
        if xo == 16:  # bclr
            pre, cond = self._cond(i)
            if i.lk:
                body = f"{{ uint32_t t = c->lr; c->lr = {self.addr_expr(i.addr + 4)}; ppc_call_indirect(c, t); }}"
            else:
                body = "return;"
            return pre + [f"if ({cond}) {body}"]
        if xo == 528:  # bcctr
            pre, cond = self._cond(i)
            if i.lk:
                body = f"{{ c->lr = {self.addr_expr(i.addr + 4)}; ppc_call_indirect(c, c->ctr); }}"
            else:
                body = "{ BCTR_DISPATCH; }"
            return pre + [f"if ({cond}) {body}"]
        if xo == 0:  # mcrf
            return [f"c->cr[{i.crfd}] = c->cr[{i.crfs}];"]
        if xo == 50:  # rfi: used by the OS to run a routine in real mode, and by exception handlers
            return ["ppc_rfi(c); return;"]
        if xo == 150:  # isync
            return ["/* isync */"]
        crops = {257: "{a} & {b}", 129: "{a} & !{b}", 289: "{a} == {b}", 225: "!({a} & {b})",
                 33: "!({a} | {b})", 449: "{a} | {b}", 417: "{a} | !{b}", 193: "{a} ^ {b}"}
        if xo in crops:
            expr = crops[xo].format(a=f"ppc_crbit(c, {i.ra})", b=f"ppc_crbit(c, {i.rb})")
            return [f"ppc_set_crbit(c, {i.rd}, ({expr}) & 1);"]
        return None

    def op20(self, i, *_):  # rlwimi
        m = mask32(i.mb, i.me)
        return [f"c->r[{i.ra}] = (ppc_rotl(c->r[{i.rs}], {i.sh}) & {h(m)}) | (c->r[{i.ra}] & {h(~m)});"] + \
            ([cr0(f"c->r[{i.ra}]")] if i.rc else [])

    def op21(self, i, *_):  # rlwinm
        m = mask32(i.mb, i.me)
        return [f"c->r[{i.ra}] = ppc_rotl(c->r[{i.rs}], {i.sh}) & {h(m)};"] + \
            ([cr0(f"c->r[{i.ra}]")] if i.rc else [])

    def op23(self, i, *_):  # rlwnm
        m = mask32(i.mb, i.me)
        return [f"c->r[{i.ra}] = ppc_rotl(c->r[{i.rs}], c->r[{i.rb}] & 31) & {h(m)};"] + \
            ([cr0(f"c->r[{i.ra}]")] if i.rc else [])

    def op24(self, i, *_):
        if i.word == 0x60000000:
            return ["/* nop */"]
        return [f"c->r[{i.ra}] = c->r[{i.rs}] | {h(i.uimm)};"]

    def op25(self, i, *_): return [f"c->r[{i.ra}] = c->r[{i.rs}] | {h(i.uimm << 16)};"]
    def op26(self, i, *_): return [f"c->r[{i.ra}] = c->r[{i.rs}] ^ {h(i.uimm)};"]
    def op27(self, i, *_): return [f"c->r[{i.ra}] = c->r[{i.rs}] ^ {h(i.uimm << 16)};"]
    def op28(self, i, *_): return [f"c->r[{i.ra}] = c->r[{i.rs}] & {h(i.uimm)};", cr0(f"c->r[{i.ra}]")]
    def op29(self, i, *_): return [f"c->r[{i.ra}] = c->r[{i.rs}] & {h(i.uimm << 16)};", cr0(f"c->r[{i.ra}]")]

    # ---------------------------------------------------------------- loads / stores (D-form)
    def _ld(self, i, expr_fn, update):
        ea = f"(c->r[{i.ra}] + {h(i.simm)})" if update else f"({ra0(i)} + {h(i.simm)})"
        s = [f"{{ uint32_t ea = {ea}; {expr_fn('ea')} "]
        if update:
            s[0] += f"c->r[{i.ra}] = ea; "
        s[0] += "}"
        return s

    def _int_load(self, i, kind, update):
        return self._ld(i, lambda ea: f"c->r[{i.rd}] = {kind}({ea});", update)

    def _int_store(self, i, kind, update):
        return self._ld(i, lambda ea: f"{kind}({ea}, c->r[{i.rs}]);", update)

    def op32(self, i, *_): return self._int_load(i, "mem_r32", False)
    def op33(self, i, *_): return self._int_load(i, "mem_r32", True)
    def op34(self, i, *_): return self._int_load(i, "mem_r8", False)
    def op35(self, i, *_): return self._int_load(i, "mem_r8", True)
    def op36(self, i, *_): return self._int_store(i, "mem_w32", False)
    def op37(self, i, *_): return self._int_store(i, "mem_w32", True)
    def op38(self, i, *_): return self._int_store(i, "mem_w8", False)
    def op39(self, i, *_): return self._int_store(i, "mem_w8", True)
    def op40(self, i, *_): return self._int_load(i, "mem_r16", False)
    def op41(self, i, *_): return self._int_load(i, "mem_r16", True)
    def op42(self, i, *_): return self._int_load(i, "(uint32_t)(int32_t)(int16_t)mem_r16", False)
    def op43(self, i, *_): return self._int_load(i, "(uint32_t)(int32_t)(int16_t)mem_r16", True)
    def op44(self, i, *_): return self._int_store(i, "mem_w16", False)
    def op45(self, i, *_): return self._int_store(i, "mem_w16", True)

    def op46(self, i, *_):  # lmw
        return [f"{{ uint32_t ea = {ra0(i)} + {h(i.simm)}; for (int r = {i.rd}; r < 32; r++, ea += 4) c->r[r] = mem_r32(ea); }}"]

    def op47(self, i, *_):  # stmw
        return [f"{{ uint32_t ea = {ra0(i)} + {h(i.simm)}; for (int r = {i.rs}; r < 32; r++, ea += 4) mem_w32(ea, c->r[r]); }}"]

    def op48(self, i, *_): return self._ld(i, lambda ea: f"ppc_lfs(c, {i.frd}, {ea});", False)
    def op49(self, i, *_): return self._ld(i, lambda ea: f"ppc_lfs(c, {i.frd}, {ea});", True)
    def op50(self, i, *_): return self._ld(i, lambda ea: f"c->f[{i.frd}].u = mem_r64({ea});", False)
    def op51(self, i, *_): return self._ld(i, lambda ea: f"c->f[{i.frd}].u = mem_r64({ea});", True)
    def op52(self, i, *_): return self._ld(i, lambda ea: f"mem_wf32({ea}, (float)c->f[{i.frs}].d);", False)
    def op53(self, i, *_): return self._ld(i, lambda ea: f"mem_wf32({ea}, (float)c->f[{i.frs}].d);", True)
    def op54(self, i, *_): return self._ld(i, lambda ea: f"mem_w64({ea}, c->f[{i.frs}].u);", False)
    def op55(self, i, *_): return self._ld(i, lambda ea: f"mem_w64({ea}, c->f[{i.frs}].u);", True)

    # ---------------------------------------------------------------- paired-single quantized load/store
    def _psq(self, i, store, update, indexed=False):
        if indexed:
            w, q = bits(i.word, 21, 21), bits(i.word, 22, 24)
            ea = f"{ra0(i)} + c->r[{i.rb}]" if not update else f"c->r[{i.ra}] + c->r[{i.rb}]"
        else:
            w, q = bits(i.word, 16, 16), bits(i.word, 17, 19)
            d = sext(i.word & 0xFFF, 12)
            ea = f"{ra0(i)} + {h(d)}" if not update else f"c->r[{i.ra}] + {h(d)}"
        fnname = "ppc_psq_st" if store else "ppc_psq_l"
        s = f"{{ uint32_t ea = {ea}; {fnname}(c, {i.frd}, ea, {w}, {q}); "
        if update:
            s += f"c->r[{i.ra}] = ea; "
        return [s + "}"]

    def op56(self, i, *_): return self._psq(i, False, False)
    def op57(self, i, *_): return self._psq(i, False, True)
    def op60(self, i, *_): return self._psq(i, True, False)
    def op61(self, i, *_): return self._psq(i, True, True)

    # ---------------------------------------------------------------- opcode 31 (X/XO-form integer)
    def op31(self, i, start, end):
        d, a, b, s = i.rd, i.ra, i.rb, i.rs
        RA, RB, RS, RD = f"c->r[{a}]", f"c->r[{b}]", f"c->r[{s}]", f"c->r[{d}]"
        xo9 = i.xo9
        arith = {
            266: f"{RD} = {RA} + {RB};",
            40: f"{RD} = {RB} - {RA};",
            104: f"{RD} = (uint32_t)-(int32_t){RA};",
            235: f"{RD} = (uint32_t)((int32_t){RA} * (int32_t){RB});",
            75: f"{RD} = (uint32_t)(((int64_t)(int32_t){RA} * (int64_t)(int32_t){RB}) >> 32);",
            11: f"{RD} = (uint32_t)(((uint64_t){RA} * (uint64_t){RB}) >> 32);",
            491: f"{RD} = ppc_divw({RA}, {RB});",
            459: f"{RD} = {RB} ? {RA} / {RB} : 0;",
            10: f"{{ uint64_t t = (uint64_t){RA} + {RB}; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            138: f"{{ uint64_t t = (uint64_t){RA} + {RB} + c->xer_ca; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            202: f"{{ uint64_t t = (uint64_t){RA} + c->xer_ca; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            234: f"{{ uint64_t t = (uint64_t){RA} + c->xer_ca + 0xFFFFFFFFu; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            8: f"{{ uint64_t t = (uint64_t)(uint32_t)~{RA} + {RB} + 1; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            136: f"{{ uint64_t t = (uint64_t)(uint32_t)~{RA} + {RB} + c->xer_ca; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            200: f"{{ uint64_t t = (uint64_t)(uint32_t)~{RA} + c->xer_ca; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
            232: f"{{ uint64_t t = (uint64_t)(uint32_t)~{RA} + c->xer_ca + 0xFFFFFFFFu; {RD} = (uint32_t)t; c->xer_ca = (uint32_t)(t >> 32); }}",
        }
        if xo9 in arith:
            if i.oe:
                return None  # overflow-tracking forms: rare, implement when the coverage report shows them
            return [arith[xo9]] + ([cr0(RD)] if i.rc else [])

        xo = i.xo10
        logical = {
            28: f"{RA} = {RS} & {RB};", 60: f"{RA} = {RS} & ~{RB};", 444: f"{RA} = {RS} | {RB};",
            412: f"{RA} = {RS} | ~{RB};", 316: f"{RA} = {RS} ^ {RB};", 124: f"{RA} = ~({RS} | {RB});",
            476: f"{RA} = ~({RS} & {RB});", 284: f"{RA} = ~({RS} ^ {RB});",
            24: f"{{ uint32_t n = {RB} & 63; {RA} = n > 31 ? 0 : {RS} << n; }}",
            536: f"{{ uint32_t n = {RB} & 63; {RA} = n > 31 ? 0 : {RS} >> n; }}",
            792: f"{RA} = ppc_sraw(c, {RS}, {RB} & 63);",
            824: f"{RA} = ppc_sraw(c, {RS}, {i.sh});",
            26: f"{RA} = ppc_cntlzw({RS});",
            954: f"{RA} = (uint32_t)(int32_t)(int8_t){RS};",
            922: f"{RA} = (uint32_t)(int32_t)(int16_t){RS};",
        }
        if xo in logical:
            return [logical[xo]] + ([cr0(RA)] if i.rc else [])

        if xo == 0:
            return [f"ppc_cmps(c, {i.crfd}, (int32_t){RA}, (int32_t){RB});"]
        if xo == 32:
            return [f"ppc_cmpu(c, {i.crfd}, {RA}, {RB});"]

        ea = f"({ra0(i)} + {RB})"
        eau = f"({RA} + {RB})"
        loads = {23: ("mem_r32", False), 55: ("mem_r32", True), 87: ("mem_r8", False), 119: ("mem_r8", True),
                 279: ("mem_r16", False), 311: ("mem_r16", True),
                 343: ("(uint32_t)(int32_t)(int16_t)mem_r16", False), 375: ("(uint32_t)(int32_t)(int16_t)mem_r16", True),
                 534: ("ppc_bswap32(mem_r32", False), 790: ("ppc_bswap16(mem_r16", False), 20: ("mem_r32", False)}
        if xo in loads:
            kind, upd = loads[xo]
            close = ")" if kind.startswith("ppc_bswap") else ""
            if upd:
                return [f"{{ uint32_t ea = {eau}; {RD} = {kind}(ea){close}; {RA} = ea; }}"]
            return [f"{RD} = {kind}({ea}){close};"]
        stores = {151: ("mem_w32", False), 183: ("mem_w32", True), 215: ("mem_w8", False), 247: ("mem_w8", True),
                  407: ("mem_w16", False), 439: ("mem_w16", True)}
        if xo in stores:
            kind, upd = stores[xo]
            if upd:
                return [f"{{ uint32_t ea = {eau}; {kind}(ea, {RS}); {RA} = ea; }}"]
            return [f"{kind}({ea}, {RS});"]
        if xo == 662:
            return [f"mem_w32({ea}, ppc_bswap32({RS}));"]
        if xo == 918:
            return [f"mem_w16({ea}, ppc_bswap16((uint16_t){RS}));"]
        if xo == 150:  # stwcx.
            return [f"mem_w32({ea}, {RS}); c->cr[0] = 0x2 | c->xer_so;"]
        fpls = {535: f"ppc_lfs(c, {d}, {{ea}});", 567: f"ppc_lfs(c, {d}, {{ea}});",
                599: f"c->f[{d}].u = mem_r64({{ea}});", 631: f"c->f[{d}].u = mem_r64({{ea}});",
                663: f"mem_wf32({{ea}}, (float)c->f[{s}].d);", 695: f"mem_wf32({{ea}}, (float)c->f[{s}].d);",
                727: f"mem_w64({{ea}}, c->f[{s}].u);", 759: f"mem_w64({{ea}}, c->f[{s}].u);",
                983: f"mem_w32({{ea}}, (uint32_t)c->f[{s}].u);"}
        if xo in fpls:
            upd = xo in (567, 631, 695, 759)
            if upd:
                return [f"{{ uint32_t ea = {eau}; {fpls[xo].format(ea='ea')} {RA} = ea; }}"]
            return [fpls[xo].format(ea=ea)]
        if xo == 1014:
            return [f"ppc_dcbz({ea});"]
        if xo in (54, 86, 470, 982, 598, 854, 278, 246):  # cache ops, sync, eieio, dcbt, dcbtst
            return [f"/* cache/sync op {xo} */"]
        if xo == 339:  # mfspr
            return [f"{RD} = ppc_mfspr(c, {i.spr});"]
        if xo == 467:  # mtspr
            return [f"ppc_mtspr(c, {i.spr}, {RS});"]
        if xo == 371:  # mftb
            return [f"{RD} = ppc_mftb(c, {i.spr});"]
        if xo == 19:
            return [f"{RD} = ppc_get_cr(c);"]
        if xo == 144:
            return [f"ppc_mtcrf(c, {i.crm}, {RS});"]
        if xo == 83:
            return [f"{RD} = c->msr;"]
        if xo == 146:
            return [f"ppc_mtmsr(c, {RS});"]
        if xo == 595:  # mfsr
            return [f"{RD} = 0;"]
        if xo == 210:  # mtsr
            return ["/* mtsr */"]
        if xo == 512:  # mcrxr
            return [f"c->cr[{i.crfd}] = (c->xer_so << 3) | (c->xer_ov << 2) | (c->xer_ca << 1); c->xer_so = c->xer_ov = c->xer_ca = 0;"]
        return None

    # ---------------------------------------------------------------- floating point
    def _fp_a(self, i, single):
        d, a, b, cc = i.frd, i.ra, i.rb, i.rc_field
        FA, FB, FC = f"c->f[{a}].d", f"c->f[{b}].d", f"c->f[{cc}].d"
        ops = {18: f"{FA} / {FB}", 20: f"{FA} - {FB}", 21: f"{FA} + {FB}", 25: f"{FA} * {FC}",
               28: f"{FA} * {FC} - {FB}", 29: f"{FA} * {FC} + {FB}",
               30: f"-({FA} * {FC} - {FB})", 31: f"-({FA} * {FC} + {FB})",
               23: f"({FA} >= 0.0) ? {FC} : {FB}", 22: f"sqrt({FB})",
               24: f"1.0 / {FB}", 26: f"1.0 / sqrt({FB})"}
        if i.xo5 not in ops:
            return None
        if single:
            out = [f"ppc_set_ps(c, {d}, (double)(float)({ops[i.xo5]}));"]
        else:
            out = [f"c->f[{d}].d = {ops[i.xo5]};"]
        if i.rc:
            out.append("ppc_set_cr1(c);")
        return out

    def op59(self, i, *_):
        return self._fp_a(i, True)

    def op63(self, i, *_):
        d, a, b = i.frd, i.ra, i.rb
        if i.xo5 >= 18 and i.xo5 != 24:
            return self._fp_a(i, False)
        xo = i.xo10
        simple = {72: f"c->f[{d}].u = c->f[{b}].u;", 40: f"c->f[{d}].u = c->f[{b}].u ^ 0x8000000000000000ull;",
                  264: f"c->f[{d}].u = c->f[{b}].u & 0x7FFFFFFFFFFFFFFFull;",
                  136: f"c->f[{d}].u = c->f[{b}].u | 0x8000000000000000ull;",
                  12: f"ppc_set_ps(c, {d}, (double)(float)c->f[{b}].d);",
                  15: f"c->f[{d}].u = 0xFFF8000000000000ull | (uint32_t)ppc_fctiwz(c->f[{b}].d);",
                  14: f"c->f[{d}].u = 0xFFF8000000000000ull | (uint32_t)ppc_fctiw(c->f[{b}].d);",
                  583: f"c->f[{d}].u = c->fpscr;",
                  711: f"c->fpscr = (uint32_t)c->f[{b}].u;",
                  70: "/* mtfsb0 */", 38: "/* mtfsb1 */", 134: "/* mtfsfi */"}
        if xo in simple:
            return [simple[xo]] + (["ppc_set_cr1(c);"] if i.rc and xo not in (583, 711) else [])
        if xo in (0, 32):
            return [f"ppc_fcmp(c, {i.crfd}, c->f[{a}].d, c->f[{b}].d);"]
        return None

    # ---------------------------------------------------------------- paired singles (opcode 4)
    def op4(self, i, *_):
        d, a, b, cc = i.frd, i.ra, i.rb, i.rc_field
        P0 = lambda r: f"c->f[{r}].d"  # noqa: E731
        P1 = lambda r: f"c->ps1[{r}]"  # noqa: E731
        x5 = i.xo5
        two = {
            21: (lambda p: f"{p(a)} + {p(b)}"), 20: (lambda p: f"{p(a)} - {p(b)}"),
            25: (lambda p: f"{p(a)} * {p(cc)}"), 18: (lambda p: f"{p(a)} / {p(b)}"),
            29: (lambda p: f"{p(a)} * {p(cc)} + {p(b)}"), 28: (lambda p: f"{p(a)} * {p(cc)} - {p(b)}"),
            31: (lambda p: f"-({p(a)} * {p(cc)} + {p(b)})"), 30: (lambda p: f"-({p(a)} * {p(cc)} - {p(b)})"),
            23: (lambda p: f"({p(a)} >= 0.0) ? {p(cc)} : {p(b)}"),
            24: (lambda p: f"1.0 / {p(b)}"), 26: (lambda p: f"1.0 / sqrt({p(b)})"),
        }
        if x5 in two:
            e = two[x5]
            return [f"{{ double t0 = (float)({e(P0)}), t1 = (float)({e(P1)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 12:  # ps_muls0
            return [f"{{ double t0 = (float)({P0(a)} * {P0(cc)}), t1 = (float)({P1(a)} * {P0(cc)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 13:  # ps_muls1
            return [f"{{ double t0 = (float)({P0(a)} * {P1(cc)}), t1 = (float)({P1(a)} * {P1(cc)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 14:  # ps_madds0
            return [f"{{ double t0 = (float)({P0(a)} * {P0(cc)} + {P0(b)}), t1 = (float)({P1(a)} * {P0(cc)} + {P1(b)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 15:  # ps_madds1
            return [f"{{ double t0 = (float)({P0(a)} * {P1(cc)} + {P0(b)}), t1 = (float)({P1(a)} * {P1(cc)} + {P1(b)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 10:  # ps_sum0
            return [f"{{ double t0 = (float)({P0(a)} + {P1(b)}), t1 = {P1(cc)}; c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if x5 == 11:  # ps_sum1
            return [f"{{ double t0 = {P0(cc)}, t1 = (float)({P0(a)} + {P1(b)}); c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        xo6 = bits(i.word, 25, 30)
        if xo6 in (6, 7, 38, 39):
            return self._psq(i, xo6 in (7, 39), xo6 in (38, 39), indexed=True)
        xo = i.xo10
        merges = {528: (P0(a), P0(b)), 560: (P0(a), P1(b)), 592: (P1(a), P0(b)), 624: (P1(a), P1(b)),
                  72: (P0(b), P1(b))}
        if xo in merges:
            x, y = merges[xo]
            return [f"{{ double t0 = {x}, t1 = {y}; c->f[{d}].d = t0; c->ps1[{d}] = t1; }}"]
        if xo == 40:
            return [f"c->f[{d}].d = -{P0(b)}; c->ps1[{d}] = -{P1(b)};"]
        if xo == 264:
            return [f"c->f[{d}].d = fabs({P0(b)}); c->ps1[{d}] = fabs({P1(b)});"]
        if xo == 136:
            return [f"c->f[{d}].d = -fabs({P0(b)}); c->ps1[{d}] = -fabs({P1(b)});"]
        if xo in (0, 32):
            return [f"ppc_fcmp(c, {i.crfd}, {P0(a)}, {P0(b)});"]
        if xo in (64, 96):
            return [f"ppc_fcmp(c, {i.crfd}, {P1(a)}, {P1(b)});"]
        if xo == 1014:  # dcbz_l (locked cache) - treat as dcbz
            return [f"ppc_dcbz({ra0(i)} + c->r[{b}]);"]
        return None
