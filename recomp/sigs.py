#!/usr/bin/env python3
"""Name SDK functions in a DOL using Dolphin's signature database (Data/Sys/totaldb.dsy).

The hash is Dolphin's HashSignatureDB::ComputeCodeChecksum (opcode bits only, operands masked),
so it matches across games built with the same SDK. Usage:
    python3 recomp/sigs.py --dol main.dol --dsy totaldb.dsy --out symbols.json
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from formats import parse_dol  # noqa: E402
from recompile import Image, discover_functions  # noqa: E402


def opcode_key(w: int) -> int:
    op = w & 0xFC000000
    aux = op >> 26
    op2 = op3 = 0
    if aux == 4:
        op2 = w & 0x3F
        if op2 in (0, 8, 16, 21, 22):
            op3 = w & 0x7C0
    elif aux in (7, 8, 10, 11, 12, 13, 14, 15):
        op2 = w & 0x03FF0000
    elif aux in (19, 31, 63):
        op2 = w & 0x7FF
    elif aux == 59:
        op2 = w & 0x3F
        if op2 < 16:
            op3 = w & 0x7C0
    elif 32 <= aux < 56:
        op2 = w & 0x03FF0000
    return op | op2 | op3


def load_dsy(path: str) -> dict[int, tuple[int, str]]:
    with open(path, "rb") as f:
        blob = f.read()
    (n,) = struct.unpack_from("<I", blob, 0)
    db = {}
    for i in range(n):
        cs, size, name = struct.unpack_from("<II128s", blob, 4 + i * 136)
        db[cs] = (size, name.split(b"\0", 1)[0].decode("latin-1"))
    return db


def match(img: Image, starts, db, max_len=0x4000) -> dict[int, str]:
    out = {}
    for s in starts:
        hi = img.range_end(s)
        total = 0
        a = s
        while a < hi and a - s < max_len:
            total = ((total << 17) & 0xFFFE0000) | ((total >> 15) & 0x1FFFF)
            total ^= opcode_key(img.word(a))
            a += 4
            hit = db.get(total)
            if hit and hit[0] == a - s:
                out[s] = hit[1]
                break
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dol", required=True)
    ap.add_argument("--dsy", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    with open(args.dol, "rb") as f:
        dol = parse_dol(f.read())
    img = Image(dol.text, dol.sections)
    funcs = discover_functions(img, [dol.entry])
    names = match(img, sorted(funcs), load_dsy(args.dsy))
    with open(args.out, "w") as f:
        json.dump({f"{k:08X}": v for k, v in sorted(names.items())}, f, indent=1)
    print(f"named {len(names)} of {len(funcs)} functions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
