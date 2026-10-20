#!/usr/bin/env python3
"""End-to-end test: assemble a small PowerPC program by hand, wrap it in a DOL, recompile it to C,
build it with the runtime's CPU services, run it, and check the results it writes to guest memory.
No game data involved.
"""
import os
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEXT, DATA = 0x80003100, 0x80002000


class Asm:
    def __init__(self, base):
        self.base, self.items, self.labels = base, [], {}

    def here(self): return self.base + 4 * len(self.items)
    def label(self, n): self.labels[n] = self.here()
    def w(self, f): self.items.append(f)

    def D(self, op, rt, ra, imm): self.w(lambda: (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF))
    def X(self, rt, ra, rb, xo, rc=0): self.w(lambda: (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc)

    def li(self, r, v): self.D(14, r, 0, v)
    def lis(self, r, v): self.D(15, r, 0, v)
    def addi(self, d, a, v): self.D(14, d, a, v)
    def ori(self, a, s, v): self.D(24, s, a, v)
    def lwz(self, d, off, a): self.D(32, d, a, off)
    def stw(self, s, off, a): self.D(36, s, a, off)
    def stwu(self, s, off, a): self.D(37, s, a, off)
    def lfs(self, d, off, a): self.D(48, d, a, off)
    def stfs(self, s, off, a): self.D(52, s, a, off)
    def cmpwi(self, a, v): self.D(11, 0, a, v)
    def add(self, d, a, b): self.X(d, a, b, 266)
    def mr(self, a, s): self.X(s, a, s, 444)
    def lwzx(self, d, a, b): self.X(d, a, b, 23)
    def mfspr(self, d, spr): self.X(d, spr & 31, spr >> 5, 339)
    def mtspr(self, spr, s): self.X(s, spr & 31, spr >> 5, 467)
    def mflr(self, d): self.mfspr(d, 8)
    def mtlr(self, s): self.mtspr(8, s)
    def mtctr(self, s): self.mtspr(9, s)
    def slwi(self, a, s, n): self.w(lambda: (21 << 26) | (s << 21) | (a << 16) | (n << 11) | (0 << 6) | ((31 - n) << 1))
    def fadds(self, d, a, b): self.w(lambda: (59 << 26) | (d << 21) | (a << 16) | (b << 11) | (21 << 1))
    def blr(self): self.w(lambda: 0x4E800020)
    def bctr(self): self.w(lambda: 0x4E800420)

    def _b(self, name, lk):
        at = self.here()
        self.w(lambda: (18 << 26) | ((self.labels[name] - at) & 0x03FFFFFC) | lk)

    def b(self, n): self._b(n, 0)
    def bl(self, n): self._b(n, 1)

    def bc(self, bo, bi, name):
        at = self.here()
        self.w(lambda: (16 << 26) | (bo << 21) | (bi << 16) | ((self.labels[name] - at) & 0xFFFC))

    def build(self): return b"".join(struct.pack(">I", f()) for f in self.items)


def program():
    a = Asm(TEXT)
    a.label("main")
    a.stwu(1, -16, 1); a.mflr(0); a.stw(0, 20, 1)
    a.li(3, 10); a.bl("fib")
    a.lis(4, 0x8000); a.stw(3, 0x2000, 4)                 # fib(10)
    a.li(7, 2); a.bl("switch"); a.stw(3, 0x2008, 4)       # jump table case 2
    a.li(8, 5); a.mtctr(8); a.li(9, 0)
    a.label("loop"); a.addi(9, 9, 3); a.bc(16, 0, "loop")  # bdnz
    a.stw(9, 0x200C, 4)                                   # 5 * 3
    a.lfs(1, 0x2100, 4); a.fadds(2, 1, 1); a.stfs(2, 0x2104, 4)  # 1.25 + 1.25
    a.lwz(0, 20, 1); a.mtlr(0); a.addi(1, 1, 16); a.blr()

    a.label("fib")
    a.li(5, 0); a.li(6, 1); a.cmpwi(3, 0); a.bc(12, 2, "fib_done"); a.mtctr(3)
    a.label("fib_loop"); a.add(7, 5, 6); a.mr(5, 6); a.mr(6, 7); a.bc(16, 0, "fib_loop")
    a.label("fib_done"); a.mr(3, 5); a.blr()

    a.label("switch")
    a.lis(10, 0x8000); a.ori(10, 10, 0x2200); a.slwi(11, 7, 2); a.lwzx(12, 10, 11); a.mtctr(12); a.bctr()
    for n, v in enumerate((100, 200, 300)):
        a.label(f"case{n}"); a.li(3, v); a.blr()
    text = a.build()
    data = bytearray(0x300)
    struct.pack_into(">f", data, 0x100, 1.25)
    for n in range(3):
        struct.pack_into(">I", data, 0x200 + 4 * n, a.labels[f"case{n}"])
    return text, bytes(data), a.labels["main"]


def make_dol(text, data, entry):
    hdr = bytearray(0x100)
    struct.pack_into(">I", hdr, 0x00, 0x100)
    struct.pack_into(">I", hdr, 0x48, TEXT)
    struct.pack_into(">I", hdr, 0x90, len(text))
    struct.pack_into(">I", hdr, 0x1C, 0x100 + len(text))
    struct.pack_into(">I", hdr, 0x64, DATA)
    struct.pack_into(">I", hdr, 0xAC, len(data))
    struct.pack_into(">I", hdr, 0xE0, entry)
    return bytes(hdr) + text + data


HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include "ppc_context.h"
#include "recomp_funcs.h"
extern uint8_t* g_mem;
extern const uint32_t g_recomp_entry;
uint32_t mmio_read(uint32_t a, int s) { (void)a; (void)s; return 0; }
void mmio_write(uint32_t a, uint32_t v, int s) { (void)a; (void)v; (void)s; }
PPCFunc ppc_hle_lookup(uint32_t a) { (void)a; return NULL; }
int main(int argc, char** argv) {
    g_mem = calloc(1, 0x14000000);
    FILE* f = fopen(argv[1], "rb"); if (fread(g_mem + 0x2000, 1, 0x300, f) != 0x300) return 1; fclose(f);
    static PPCContext c; c.r[1] = 0x80400000;
    ppc_call_indirect(&c, g_recomp_entry);
    printf("%u %u %u %g\n", mem_r32(0x80002000), mem_r32(0x80002008), mem_r32(0x8000200C), mem_rf32(0x80002104));
    return 0;
}
"""


def main():
    text, data, entry = program()
    with tempfile.TemporaryDirectory() as tmp:
        dol = os.path.join(tmp, "test.dol")
        with open(dol, "wb") as f:
            f.write(make_dol(text, data, entry))
        with open(os.path.join(tmp, "data.bin"), "wb") as f:
            f.write(data)
        gen = os.path.join(tmp, "gen")
        subprocess.run([sys.executable, os.path.join(ROOT, "recomp", "recompile.py"), "--dol", dol, "--out", gen], check=True)
        with open(os.path.join(tmp, "harness.c"), "w") as f:
            f.write(HARNESS)
        exe = os.path.join(tmp, "t")
        srcs = [os.path.join(gen, n) for n in os.listdir(gen) if n.endswith(".c")]
        subprocess.run(["cc", "-O2", "-Wall", "-Wno-unused-label", "-I", os.path.join(ROOT, "runtime", "include"), "-I", gen,
                        os.path.join(tmp, "harness.c"), os.path.join(ROOT, "runtime", "src", "cpu_services.c"),
                        *srcs, "-lm", "-o", exe], check=True)
        out = subprocess.run([exe, os.path.join(tmp, "data.bin")], capture_output=True, text=True, check=True).stdout.split()
    expect = ["55", "300", "15", "2.5"]
    print("got", out, "expected", expect)
    assert out == expect, out
    print("PASS")


if __name__ == "__main__":
    main()
