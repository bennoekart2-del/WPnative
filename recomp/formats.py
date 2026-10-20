"""Parsers for the Wii executable formats: DOL (main executable) and REL (relocatable modules).

Layouts follow the public documentation on wiibrew.org and Dolphin's Core/Boot/DolReader.cpp.
Nothing here contains game data; it only reads files the user supplies.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field


@dataclass
class Section:
    name: str
    file_offset: int
    address: int
    size: int
    executable: bool
    data: bytes = b""


@dataclass
class Dol:
    sections: list[Section]
    bss_address: int
    bss_size: int
    entry: int

    @property
    def text(self) -> list[Section]:
        return [s for s in self.sections if s.executable]


def parse_dol(blob: bytes) -> Dol:
    if len(blob) < 0x100:
        raise ValueError("file too small to be a DOL")
    offs = struct.unpack_from(">18I", blob, 0x00)
    addrs = struct.unpack_from(">18I", blob, 0x48)
    sizes = struct.unpack_from(">18I", blob, 0x90)
    bss_addr, bss_size, entry = struct.unpack_from(">3I", blob, 0xD8)
    sections = []
    for i in range(18):
        if sizes[i] == 0:
            continue
        is_text = i < 7
        name = f".text{i}" if is_text else f".data{i - 7}"
        off, size = offs[i], sizes[i]
        if off + size > len(blob):
            raise ValueError(f"{name} runs past end of file")
        sections.append(Section(name, off, addrs[i], size, is_text, blob[off:off + size]))
    return Dol(sections, bss_addr, bss_size, entry)


@dataclass
class RelSection:
    index: int
    offset: int
    size: int
    executable: bool


@dataclass
class RelReloc:
    offset: int        # offset within the target section
    type: int          # R_PPC_* relocation type
    section: int       # section being patched
    addend: int        # offset within the symbol's section
    target_section: int


@dataclass
class Rel:
    module_id: int
    sections: list[RelSection]
    # imports[module_id] -> list of relocs that reference that module (0 = main DOL)
    imports: dict[int, list[RelReloc]] = field(default_factory=dict)
    prolog: tuple[int, int] = (0, 0)   # (section, offset)
    epilog: tuple[int, int] = (0, 0)
    unresolved: tuple[int, int] = (0, 0)
    bss_size: int = 0
    data: bytes = b""


# Relocation types used by REL files (R_PPC_* plus the R_DOLPHIN_* control types)
R_PPC_NONE, R_PPC_ADDR32, R_PPC_ADDR24, R_PPC_ADDR16, R_PPC_ADDR16_LO = 0, 1, 2, 3, 4
R_PPC_ADDR16_HI, R_PPC_ADDR16_HA, R_PPC_ADDR14, R_PPC_REL24, R_PPC_REL14 = 5, 6, 7, 10, 11
R_DOLPHIN_NOP, R_DOLPHIN_SECTION, R_DOLPHIN_END = 201, 202, 203


def parse_rel(blob: bytes) -> Rel:
    (module_id, _next, _prev, num_sections, section_info_off, _name_off, _name_size,
     _version, bss_size, rel_off, imp_off, imp_size, prolog_s, epilog_s, unres_s, _bss_s,
     prolog, epilog, unresolved) = struct.unpack_from(">IIIIIIIIIIIIBBBBIII", blob, 0)
    sections = []
    for i in range(num_sections):
        off, size = struct.unpack_from(">II", blob, section_info_off + i * 8)
        sections.append(RelSection(i, off & ~1, size, bool(off & 1)))
    rel = Rel(module_id, sections, prolog=(prolog_s, prolog), epilog=(epilog_s, epilog),
              unresolved=(unres_s, unresolved), bss_size=bss_size, data=blob)
    for i in range(imp_size // 8):
        imp_module, imp_rel_off = struct.unpack_from(">II", blob, imp_off + i * 8)
        relocs, pos, section, cursor = [], imp_rel_off, 0, 0
        while True:
            delta, rtype, rsec, addend = struct.unpack_from(">HBBI", blob, pos)
            pos += 8
            if rtype == R_DOLPHIN_END:
                break
            if rtype == R_DOLPHIN_SECTION:
                section, cursor = rsec, 0
                continue
            cursor += delta
            if rtype != R_DOLPHIN_NOP:
                relocs.append(RelReloc(cursor, rtype, section, addend, rsec))
        rel.imports[imp_module] = relocs
    return rel
