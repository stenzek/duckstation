#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2019-2026 Connor McLaughlin <stenzek@gmail.com>
# SPDX-License-Identifier: CC-BY-NC-ND-4.0

"""Compute DuckStation texture replacement filenames for a TIM image.

Prints the names (without the .png extension) that the hardware renderer's texture cache would use when the game
uploads the TIM's pixel data to VRAM:

  vram-write-<XXH3_128>                 Replaces the VRAM upload itself (TextureReplacements/EnableVRAMWriteReplacements).
  texupload-<mode>-<src>-<pal>-...      Texture-from-VRAM-write replacement, one per palette in the CLUT, covering the
                                        whole image at offset 0,0 with the full palette range (needs the texture cache).

texpage-* names depend on the contents of the whole texture page and the UVs the game draws with, so they cannot be
derived from a TIM file. Requires the xxhash module (python3-xxhash / pip install xxhash). A standalone C++ version
with identical output is in tim_texture_hash.cpp.
"""

import argparse
import struct
import sys

import xxhash

EPILOG = """caveats:
  - The game must upload the pixel block with a single LoadImage. For texupload names, uploads split into strips
    still match if the texture cache coalesces them back into the same rectangle.
  - Uploads with the mask bit forced on (set_mask) change the texupload source hash; mask-checked uploads are never
    replaced as VRAM writes.
  - The palette must be in VRAM, unmodified, at the TIM's CLUT position when the texture is drawn.
  - Rename P4/P8/C16 to STP4/STP8/STC16 if the replacement should use semi-transparent alpha handling.
  - Uploads that wrap around the right or bottom edge of VRAM will not match.
"""

VRAM_WIDTH = 1024
VRAM_HEIGHT = 512
MODE_NAMES = ("P4", "P8", "C16")
MODE_SHIFTS = (2, 1, 0)
PALETTE_WIDTHS = (16, 256, 0)


class TIMError(Exception):
    pass


def read_section(data, pos, what):
    if pos + 12 > len(data):
        raise TIMError(f"{what} section header truncated at offset 0x{pos:X}")
    size, x, y, w, h = struct.unpack_from("<IHHHH", data, pos)
    expected_size = 12 + w * h * 2
    if pos + expected_size > len(data):
        raise TIMError(
            f"{what} data truncated ({w}x{h} needs {w * h * 2} bytes, {len(data) - pos - 12} available)"
        )
    if size != expected_size:
        warn(f"{what} section size is {size}, expected {expected_size}")
    return x, y, w, h, data[pos + 12 : pos + expected_size]


def warn(msg):
    print(f"warning: {msg}", file=sys.stderr)


def get_names(data, mode_override, verbose):
    if len(data) < 8:
        raise TIMError("file too small for a TIM header")
    tim_id, version, reserved, flags = struct.unpack_from("<BBHI", data, 0)
    if tim_id != 0x10 or version != 0:
        raise TIMError(f"not a TIM (ID {tim_id:02X}h, version {version:02X}h)")
    if reserved != 0:
        raise TIMError(f"compressed TIM (reserved field {reserved}) is not supported")
    if (flags & ~0xF) != 0:
        warn(f"reserved flag bits are set ({flags:08X}h)")

    tim_type = flags & 7
    has_clut = (flags & 8) != 0
    pos = 8

    clut = None
    if has_clut:
        clut = read_section(data, pos, "CLUT")
        pos += 12 + clut[2] * clut[3] * 2

    px, py, pw, ph, pixels = read_section(data, pos, "pixel")
    if verbose:
        print(f"type {tim_type}, pixels {pw}x{ph} halfwords at ({px},{py})", file=sys.stderr)
        if clut:
            print(f"CLUT {clut[2]}x{clut[3]} at ({clut[0]},{clut[1]})", file=sys.stderr)

    if pw == 0 or ph == 0:
        warn("TIM has no pixel data")
        return []

    if (px % VRAM_WIDTH) + pw > VRAM_WIDTH or (py % VRAM_HEIGHT) + ph > VRAM_HEIGHT:
        warn(f"pixel rect ({px},{py} {pw}x{ph}) wraps around VRAM, names will not match")

    h128 = xxhash.xxh3_128_intdigest(pixels)
    names = [f"vram-write-{h128 >> 64:016X}{h128 & 0xFFFFFFFFFFFFFFFF:016X}"]

    if mode_override is not None:
        mode = MODE_NAMES.index(mode_override)
    elif tim_type < 3:
        mode = tim_type
    else:
        warn(f"TIM type {tim_type} has no texture mode, use --mode to generate texupload names")
        return names

    src_hash = xxhash.xxh3_64_intdigest(pixels)
    prefix = f"texupload-{MODE_NAMES[mode]}-{src_hash:016X}"
    suffix = f"{pw}x{ph}-0-0-{pw << MODE_SHIFTS[mode]}x{ph}"

    if mode == 2:
        names.append(f"{prefix}-{suffix}")
        return names

    if clut is None:
        warn("TIM has no CLUT, cannot generate texupload names for a palette texture")
        return names

    cx, cy, cw, ch, cdata = clut
    if cx % 16 != 0:
        warn(f"CLUT X {cx} is not a multiple of 16, the GPU cannot use palettes at this position")

    pal_width = PALETTE_WIDTHS[mode]
    for row in range(ch):
        for start in range(0, cw, pal_width):
            count = min(pal_width, cw - start)
            vram_x = (cx + start) % VRAM_WIDTH
            if count == pal_width:
                # HashPalette() only hashes up to the right edge of VRAM.
                hash_count = min(count, VRAM_WIDTH - vram_x)
            elif vram_x + count > VRAM_WIDTH:
                warn(f"partial palette at ({vram_x},{cy + row}) crosses the edge of VRAM, skipping")
                continue
            else:
                hash_count = count
            offset = (row * cw + start) * 2
            pal_hash = xxhash.xxh3_64_intdigest(cdata[offset : offset + hash_count * 2])
            name = f"{prefix}-{pal_hash:016X}-{suffix}-P0-{count - 1}"
            if name not in names:
                names.append(name)

    return names


def main():
    parser = argparse.ArgumentParser(
        description="Compute DuckStation texture replacement filenames for TIM images.",
        epilog=EPILOG,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("files", nargs="+", help="TIM file(s)")
    parser.add_argument("--offset", type=lambda x: int(x, 0), default=0, help="byte offset of the TIM in each file")
    parser.add_argument("--mode", choices=MODE_NAMES, help="override the texture mode from the TIM type field")
    parser.add_argument("-v", "--verbose", action="store_true", help="print parsed header information to stderr")
    args = parser.parse_args()

    result = 0
    for path in args.files:
        if len(args.files) > 1:
            print(f"# {path}")
        try:
            with open(path, "rb") as f:
                data = f.read()
        except OSError as e:
            print(f"error: {path}: {e.strerror}", file=sys.stderr)
            result = 1
            continue

        try:
            for name in get_names(data[args.offset :], args.mode, args.verbose):
                print(name)
        except TIMError as e:
            print(f"error: {path}: {e}", file=sys.stderr)
            result = 1

    return result


if __name__ == "__main__":
    sys.exit(main())
