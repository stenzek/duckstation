#!/usr/bin/env python3
"""Wrap a PS-X EXE in a little-endian MIPS-I ELF for static analysis.

Usage:
  python3 scripts/psx_exe_to_elf.py game.exe
  python3 scripts/psx_exe_to_elf.py game.exe -o /tmp/game.elf
  llvm-objdump -d /tmp/game.elf
  mipsel-linux-gnu-objdump -d /tmp/game.elf
  readelf -h -l -S -s /tmp/game.elf

Requires only Python 3. GNU objdump must have MIPS target support.
The payload becomes one writable/executable .text section: PS-X EXE does
not record the original code/data boundaries, symbols, or debug information.
Only the payload size declared in the header is loaded; trailing padding is
ignored. The original header is retained in a non-allocated section.
_gp, __initial_sp (when specified), and .reginfo preserve register metadata
for analysis; this does not add startup code to initialize those registers.
"""

import argparse
import struct
import sys
from dataclasses import dataclass
from pathlib import Path


PS_EXE_HEADER_SIZE = 0x800
ELF_HEADER = struct.Struct("<16sHHIIIIIHHHHHH")
PROGRAM_HEADER = struct.Struct("<8I")
SECTION_HEADER = struct.Struct("<10I")
SYMBOL = struct.Struct("<IIIBBH")
SHN_ABS = 0xFFF1
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_NOBITS = 8
SHF_WRITE = 1
SHF_ALLOC = 2
SHF_EXECINSTR = 4


@dataclass
class Section:
    name: str
    section_type: int
    flags: int = 0
    address: int = 0
    data: bytes = b""
    size: int = 0
    link: int = 0
    info: int = 0
    alignment: int = 1
    entry_size: int = 0
    offset: int = 0


def build_elf(entry: int, sections: list[Section]) -> bytes:
    """Serialize ELF32 headers, load segments, and an analysis section table."""
    load_sections = [section for section in sections if section.flags & SHF_ALLOC]
    image = bytearray(ELF_HEADER.size + PROGRAM_HEADER.size * len(load_sections))
    names = bytearray(b"\0")
    name_offsets = []
    for section in sections:
        name_offsets.append(len(names))
        names.extend(section.name.encode("ascii") + b"\0")
    name_offsets.append(len(names))
    names.extend(b".shstrtab\0")
    sections.append(Section(".shstrtab", SHT_STRTAB, data=bytes(names)))

    for section in sections:
        image.extend(b"\0" * (-len(image) % section.alignment))
        section.offset = len(image)
        if section.section_type != SHT_NOBITS:
            section.size = len(section.data)
            image.extend(section.data)

    image.extend(b"\0" * (-len(image) % 4))
    section_table_offset = len(image)
    image.extend(bytes(SECTION_HEADER.size))  # Mandatory null section.
    for name_offset, section in zip(name_offsets, sections):
        image.extend(SECTION_HEADER.pack(
            name_offset, section.section_type, section.flags, section.address,
            section.offset, section.size, section.link, section.info,
            section.alignment, section.entry_size,
        ))

    # ELF32, little endian, System V, ET_EXEC, EM_MIPS, MIPS-I/o32/noreorder.
    ELF_HEADER.pack_into(
        image, 0, b"\x7fELF\x01\x01\x01" + bytes(9), 2, 8, 1, entry,
        ELF_HEADER.size, section_table_offset, 0x1001, ELF_HEADER.size,
        PROGRAM_HEADER.size, len(load_sections), SECTION_HEADER.size,
        len(sections) + 1, len(sections),
    )
    for index, section in enumerate(load_sections):
        flags = 4  # PF_R
        if section.flags & SHF_WRITE:
            flags |= 2  # PF_W
        if section.flags & SHF_EXECINSTR:
            flags |= 1  # PF_X
        PROGRAM_HEADER.pack_into(
            image, ELF_HEADER.size + index * PROGRAM_HEADER.size,
            1, section.offset, section.address, section.address,
            0 if section.section_type == SHT_NOBITS else section.size,
            section.size, flags, section.alignment,
        )
    return bytes(image)


def convert(data: bytes) -> bytes:
    """Convert a complete PS-X EXE, rejecting truncated or wrapping ranges."""
    if len(data) < PS_EXE_HEADER_SIZE:
        raise ValueError("input is smaller than the 0x800-byte PS-X EXE header")
    if data[:8] != b"PS-X EXE":
        raise ValueError("input does not have the PS-X EXE signature")
    entry, gp, load_address, payload_size, _, _, bss_address, bss_size, stack_base, stack_offset = (
        struct.unpack_from("<10I", data, 0x10)
    )
    if payload_size == 0:
        raise ValueError("PS-X EXE header declares an empty payload")
    if payload_size > len(data) - PS_EXE_HEADER_SIZE:
        raise ValueError(
            f"truncated payload: header declares {payload_size} bytes, "
            f"but only {len(data) - PS_EXE_HEADER_SIZE} are available"
        )
    payload_end = load_address + payload_size
    bss_end = bss_address + bss_size
    if payload_end > 0x100000000 or bss_end > 0x100000000:
        raise ValueError("payload or zero-fill range wraps the 32-bit address space")

    sections = [Section(
        ".text", SHT_PROGBITS, SHF_WRITE | SHF_ALLOC | SHF_EXECINSTR,
        load_address, data[PS_EXE_HEADER_SIZE:PS_EXE_HEADER_SIZE + payload_size],
    )]
    # The loader clears memory before loading the payload. Any overlapping
    # payload bytes therefore take precedence over the zero-fill range.
    bss_ranges = [
        (bss_address, min(bss_end, load_address)),
        (max(bss_address, payload_end), bss_end),
    ]
    bss_count = 0
    for start, end in bss_ranges:
        if start < end:
            name = ".bss" if bss_count == 0 else f".bss.{bss_count}"
            sections.append(Section(name, SHT_NOBITS, SHF_WRITE | SHF_ALLOC, start, size=end - start))
            bss_count += 1

    # Register masks are unknown; retain the initial GP in the standard MIPS
    # register-info format, as well as in a convenient absolute symbol.
    sections.append(Section(".reginfo", 0x70000006, data=struct.pack("<6I", 0, 0, 0, 0, 0, gp),
                            alignment=4, entry_size=24))
    sections.append(Section(".psx_exe_header", SHT_PROGBITS, data=data[:PS_EXE_HEADER_SIZE]))

    symbol_names = bytearray(b"\0")
    symbols = bytearray(SYMBOL.size)  # Mandatory null symbol; all others are global.

    def add_symbol(name: str, value: int, info: int, section_index: int) -> None:
        name_offset = len(symbol_names)
        symbol_names.extend(name.encode("ascii") + b"\0")
        symbols.extend(SYMBOL.pack(name_offset, value, 0, info, 0, section_index))

    add_symbol("_start", entry, 0x12, 1 if load_address <= entry < payload_end else SHN_ABS)
    add_symbol("_gp", gp, 0x10, SHN_ABS)
    if stack_base:
        add_symbol("__initial_sp", (stack_base + stack_offset) & 0xFFFFFFFF, 0x10, SHN_ABS)
    sections.append(Section(".strtab", SHT_STRTAB, data=bytes(symbol_names)))
    sections.append(Section(".symtab", SHT_SYMTAB, data=bytes(symbols), link=len(sections),
                            info=1, alignment=4, entry_size=SYMBOL.size))
    return build_elf(entry, sections)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("input", type=Path, help="PS-X EXE file")
    parser.add_argument("-o", "--output", type=Path, help="output ELF (default: input with an .elf suffix)")
    args = parser.parse_args()
    output = args.output if args.output is not None else args.input.with_suffix(".elf")
    try:
        if args.input.resolve() == output.resolve() or (output.exists() and args.input.samefile(output)):
            raise ValueError("input and output must be different files")
        image = convert(args.input.read_bytes())
        output.write_bytes(image)
    except (OSError, ValueError) as error:
        print(f"{parser.prog}: error: {error}", file=sys.stderr)
        return 1
    entry = struct.unpack_from("<I", image, 24)[0]
    print(f"Created {output} ({len(image)} bytes, entry 0x{entry:08x})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
