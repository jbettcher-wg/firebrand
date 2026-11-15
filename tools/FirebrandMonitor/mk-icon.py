#!/usr/bin/env python3
"""FirebrandMonitor.icns from icon.svg, in the forms Mac OS X 10.5 reads.

    tools/FirebrandMonitor/mk-icon.py

Renders icon.svg with rsvg-convert at 16, 32, 48, 128, 256 and 512 pixels
(ImageMagick's magick turns each into raw RGBA) and writes the classic
entries: is32/s8mk, il32/l8mk, ih32/h8mk, it32/t8mk (RGB in the per-channel
PackBits form, the mask as 8-bit alpha), and ic08/ic09 as PNG (256 and 512;
10.5 reads PNG there). Run it on a host with both tools after changing the
SVG; the .icns is committed, so the board's build needs neither.

Copyright (c) 2026 Jordan Bettcher
SPDX-License-Identifier: MIT
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SVG = os.path.join(HERE, "icon.svg")
OUT = os.path.join(HERE, "FirebrandMonitor.icns")


def render_png(size, path):
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), SVG, "-o", path], check=True)


def rgba(png, size):
    raw = subprocess.run(["magick", png, "-depth", "8", "RGBA:-"], check=True, capture_output=True).stdout
    if len(raw) != size * size * 4:
        sys.exit(f"{png}: {len(raw)} bytes, not {size}x{size} RGBA")
    return raw


def packbits(channel):
    """Apple's icns RLE: 0x00-0x7f n+1 literal bytes; 0x80-0xff (n - 125) repeats of the next byte."""
    out, i, n = bytearray(), 0, len(channel)
    while i < n:
        run = 1
        while i + run < n and run < 130 and channel[i + run] == channel[i]:
            run += 1
        if run >= 3:
            out += bytes([run + 125, channel[i]])
            i += run
            continue
        start = i
        while i < n and i - start < 128:
            if i + 2 < n and channel[i] == channel[i + 1] == channel[i + 2]:
                break
            i += 1
        out.append(i - start - 1)
        out += channel[start:i]
    return bytes(out)


def classic(raw, size, rle_prefix=b""):
    """(RGB entry data, mask data): the alpha-weighted RGB isn't needed; the mask carries alpha."""
    r, g, b, a = raw[0::4], raw[1::4], raw[2::4], raw[3::4]
    data = rle_prefix + packbits(r) + packbits(g) + packbits(b)
    return data, bytes(a)


def main():
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        pngs = {}
        for size in (16, 32, 48, 128, 256, 512):
            pngs[size] = os.path.join(tmp, f"{size}.png")
            render_png(size, pngs[size])
        for size, rgb_type, mask_type, prefix in ((16, b"is32", b"s8mk", b""), (32, b"il32", b"l8mk", b""),
                                                  (48, b"ih32", b"h8mk", b""), (128, b"it32", b"t8mk", b"\0\0\0\0")):
            data, mask = classic(rgba(pngs[size], size), size, prefix)
            entries.append((rgb_type, data))
            entries.append((mask_type, mask))
        for size, typ in ((256, b"ic08"), (512, b"ic09")):
            with open(pngs[size], "rb") as f:
                entries.append((typ, f.read()))
    body = b"".join(t + struct.pack(">I", len(d) + 8) + d for t, d in entries)
    with open(OUT, "wb") as f:
        f.write(b"icns" + struct.pack(">I", len(body) + 8) + body)
    print(f"{OUT}: {len(body) + 8} bytes, {len(entries)} entries")


if __name__ == "__main__":
    main()
