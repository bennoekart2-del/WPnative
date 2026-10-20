"""Broadway (PowerPC 750CL) instruction decoding helpers.

Field names follow the IBM PowerPC Programming Environments Manual.
"""
from __future__ import annotations


def bits(v: int, start: int, end: int) -> int:
    """Extract IBM-numbered bits start..end (bit 0 = MSB) from a 32-bit word."""
    return (v >> (31 - end)) & ((1 << (end - start + 1)) - 1)


def sext(v: int, nbits: int) -> int:
    sign = 1 << (nbits - 1)
    return (v & (sign - 1)) - (v & sign)


class Insn:
    __slots__ = ("addr", "word")

    def __init__(self, addr: int, word: int):
        self.addr = addr
        self.word = word

    @property
    def opcd(self): return bits(self.word, 0, 5)
    @property
    def rd(self): return bits(self.word, 6, 10)
    rs = rd
    frd = rd
    frs = rd
    @property
    def ra(self): return bits(self.word, 11, 15)
    @property
    def rb(self): return bits(self.word, 16, 20)
    @property
    def rc_field(self): return bits(self.word, 21, 25)  # FRC for A-form FP
    @property
    def xo10(self): return bits(self.word, 21, 30)
    @property
    def xo9(self): return bits(self.word, 22, 30)
    @property
    def xo5(self): return bits(self.word, 26, 30)
    @property
    def rc(self): return self.word & 1
    @property
    def oe(self): return bits(self.word, 21, 21)
    @property
    def simm(self): return sext(self.word & 0xFFFF, 16)
    @property
    def uimm(self): return self.word & 0xFFFF
    @property
    def crfd(self): return bits(self.word, 6, 8)
    @property
    def crfs(self): return bits(self.word, 11, 13)
    @property
    def l(self): return bits(self.word, 10, 10)
    @property
    def sh(self): return bits(self.word, 16, 20)
    @property
    def mb(self): return bits(self.word, 21, 25)
    @property
    def me(self): return bits(self.word, 26, 30)
    @property
    def bo(self): return bits(self.word, 6, 10)
    @property
    def bi(self): return bits(self.word, 11, 15)
    @property
    def lk(self): return self.word & 1
    @property
    def aa(self): return bits(self.word, 30, 30)
    @property
    def spr(self):
        return (bits(self.word, 16, 20) << 5) | bits(self.word, 11, 15)
    @property
    def crm(self): return bits(self.word, 12, 19)

    def branch_target(self) -> int | None:
        op = self.opcd
        if op == 18:
            disp = sext(self.word & 0x03FFFFFC, 26)
        elif op == 16:
            disp = sext(self.word & 0xFFFC, 16)
        else:
            return None
        return (disp if self.aa else self.addr + disp) & 0xFFFFFFFF

    # --- classification used by function discovery ---
    def is_call(self) -> bool:
        return self.opcd == 18 and self.lk == 1

    def is_uncond_branch(self) -> bool:
        return self.opcd == 18 and self.lk == 0

    def is_cond_branch(self) -> bool:
        return self.opcd == 16

    def is_return(self) -> bool:
        # blr (unconditional)
        return self.opcd == 19 and self.xo10 == 16 and (self.bo & 0x14) == 0x14 and self.lk == 0

    def is_bctr(self) -> bool:
        return self.opcd == 19 and self.xo10 == 528 and (self.bo & 0x14) == 0x14 and self.lk == 0


def mask32(mb: int, me: int) -> int:
    """PowerPC rotate mask: ones from bit mb to bit me (IBM numbering), wrapping."""
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m
