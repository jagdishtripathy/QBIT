#!/usr/bin/env python3
"""
Convert a .qgif binary file (raw or RLE-compressed) to a C PROGMEM header file.
Output format matches sys_scx.h / sys_idle.h (RLE-compressed AnimatedGIF struct).

Usage: python3 qgif2header.py <input.qgif> <output.h> <varname>
Example: python3 qgif2header.py sys_idle.qgif sys_idle.h sys_idle
"""

import sys
import struct


def rle_encode(frame: bytes) -> bytes:
    """RLE encoder matching qgifRleDecode() in firmware/include/gif_types.h."""
    out = bytearray()
    n = len(frame)
    i = 0
    while i < n:
        b = frame[i]
        run_len = 1
        while i + run_len < n and run_len < 127 and frame[i + run_len] == b:
            run_len += 1
        if run_len >= 3:
            out.append(0x80 | run_len)
            out.append(b)
            i += run_len
        else:
            lit_start = i
            lit_len = 0
            while i < n and lit_len < 127:
                r = 1
                while i + r < n and r < 3 and frame[i + r] == frame[i]:
                    r += 1
                if r >= 3:
                    break
                i += 1
                lit_len += 1
            out.append(lit_len)
            out.extend(frame[lit_start : lit_start + lit_len])
    return bytes(out)


def rle_decode(comp: bytes, expected_len: int = 1024) -> bytes:
    out = bytearray()
    i = 0
    n = len(comp)
    while i < n and len(out) < expected_len:
        ctrl = comp[i]
        i += 1
        if ctrl & 0x80:
            count = ctrl & 0x7F
            val = comp[i]
            i += 1
            out.extend([val] * count)
        else:
            count = ctrl
            out.extend(comp[i : i + count])
            i += count
    return bytes(out[:expected_len])


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <input.qgif> <output.h> <varname>")
        sys.exit(1)

    infile, outfile, varname = sys.argv[1], sys.argv[2], sys.argv[3]
    guard = varname.upper() + "_H"
    prefix = varname

    with open(infile, "rb") as f:
        data = f.read()

    # Parse header
    frame_count = data[0]
    width = struct.unpack_from("<H", data, 1)[0]
    raw_height = struct.unpack_from("<H", data, 3)[0]
    is_rle = (raw_height & 0x8000) != 0
    height = raw_height & 0x7FFF

    print(f"Frames: {frame_count}, Size: {width}x{height} (input RLE={is_rle})")

    # Parse delays
    delays = []
    offset = 5
    for i in range(frame_count):
        d = struct.unpack_from("<H", data, offset)[0]
        delays.append(d)
        offset += 2

    frame_size = (width // 8) * height  # 1024 for 128x64
    frames = []
    if is_rle:
        off_table = [
            struct.unpack_from("<I", data, offset + i * 4)[0]
            for i in range(frame_count + 1)
        ]
        data_start = offset + (frame_count + 1) * 4
        for i in range(frame_count):
            comp = data[data_start + off_table[i] : data_start + off_table[i + 1]]
            frames.append(rle_decode(comp, frame_size))
    else:
        for i in range(frame_count):
            frame = data[offset : offset + frame_size]
            if len(frame) != frame_size:
                print(f"Error: frame {i} truncated ({len(frame)} < {frame_size})")
                sys.exit(1)
            frames.append(frame)
            offset += frame_size

    rle_offsets = [0]
    rle_blob = bytearray()
    for frame in frames:
        comp = rle_encode(frame)
        assert rle_decode(comp, frame_size) == frame
        rle_blob.extend(comp)
        rle_offsets.append(len(rle_blob))

    FC = f"{prefix.upper()}_FRAME_COUNT"
    W = f"{prefix.upper()}_WIDTH"
    H = f"{prefix.upper()}_HEIGHT"
    RLE_SIZE = f"{prefix.upper()}_RLE_SIZE"

    with open(outfile, "w", newline="\n") as out:
        out.write(f"#ifndef {guard}\n")
        out.write(f"#define {guard}\n\n")
        out.write("#include <stdint.h>\n")
        out.write("#include <pgmspace.h>\n")
        out.write('#include "gif_types.h"\n\n')
        out.write(f"#define {FC} {frame_count}\n")
        out.write(f"#define {W} {width}\n")
        out.write(f"#define {H} {height}\n")
        out.write(f"#define {RLE_SIZE} {len(rle_blob)}\n\n")

        delay_str = ", ".join(str(d) for d in delays)
        out.write(f"const uint16_t {prefix}_delays[{FC}] = {{{delay_str}}};\n\n")

        off_str = ", ".join(str(o) for o in rle_offsets)
        out.write(f"PROGMEM const uint32_t {prefix}_rle_offsets[{FC} + 1] = {{{off_str}}};\n\n")

        out.write(f"PROGMEM const uint8_t {prefix}_rle_data[{RLE_SIZE}] = {{\n")
        for row_start in range(0, len(rle_blob), 16):
            chunk = rle_blob[row_start : row_start + 16]
            hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
            comma = "," if row_start + 16 < len(rle_blob) else ""
            out.write(f"    {hex_vals}{comma}\n")
        out.write("};\n\n")

        out.write(f"const AnimatedGIF {prefix}_gif = {{\n")
        out.write(f"    {FC},\n")
        out.write(f"    {W},\n")
        out.write(f"    {H},\n")
        out.write(f"    {prefix}_delays,\n")
        out.write("    nullptr,\n")
        out.write(f"    {prefix}_rle_data,\n")
        out.write(f"    {prefix}_rle_offsets\n")
        out.write("};\n\n")
        out.write(f"#endif // {guard}\n")

    print(
        f"Written: {outfile} ({len(rle_blob):,} bytes RLE vs {frame_count * frame_size:,} bytes raw)"
    )


if __name__ == "__main__":
    main()
