"""Nintendo LZ77 decompression (headers 0x10 and 0x11), used for the game's *.lz files.

Format per GBATEK / Dolphin-era tooling: a 4-byte header (type byte + 24-bit size, 0 meaning a
32-bit size follows), then flag bytes each describing 8 literal-or-backreference blocks.
"""
from __future__ import annotations


def decompress(src: bytes) -> bytes:
    kind = src[0]
    size = src[1] | src[2] << 8 | src[3] << 16
    pos = 4
    if size == 0:
        size = int.from_bytes(src[4:8], "little")
        pos = 8
    if kind not in (0x10, 0x11):
        raise ValueError(f"not an LZ10/LZ11 stream (type {kind:#x})")
    out = bytearray()
    while len(out) < size:
        flags = src[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(src[pos])
                pos += 1
                continue
            if kind == 0x10:
                b0, b1 = src[pos], src[pos + 1]
                pos += 2
                length = (b0 >> 4) + 3
                disp = ((b0 & 15) << 8 | b1) + 1
            else:
                b0 = src[pos]
                ind = b0 >> 4
                if ind == 0:
                    length = ((b0 & 15) << 4 | src[pos + 1] >> 4) + 0x11
                    disp = ((src[pos + 1] & 15) << 8 | src[pos + 2]) + 1
                    pos += 3
                elif ind == 1:
                    length = ((b0 & 15) << 12 | src[pos + 1] << 4 | src[pos + 2] >> 4) + 0x111
                    disp = ((src[pos + 2] & 15) << 8 | src[pos + 3]) + 1
                    pos += 4
                else:
                    length = ind + 1
                    disp = ((b0 & 15) << 8 | src[pos + 1]) + 1
                    pos += 2
            for _ in range(length):
                out.append(out[-disp])
    return bytes(out[:size])
