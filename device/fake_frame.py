#!/usr/bin/env python3
"""Generate a deterministic Phase 0 RGBA framebuffer frame without Pillow."""
import gzip
import sys
import time

WIDTH = 1264
HEIGHT = 1680
EXPECTED = WIDTH * HEIGHT * 4

FONT = {
    "0": ("11111", "10001", "10011", "10101", "11001", "10001", "11111"),
    "1": ("00100", "01100", "00100", "00100", "00100", "00100", "01110"),
    "2": ("11110", "00001", "00001", "01110", "10000", "10000", "11111"),
    "3": ("11110", "00001", "00001", "01110", "00001", "00001", "11110"),
    "4": ("10010", "10010", "10010", "11111", "00010", "00010", "00010"),
    "5": ("11111", "10000", "10000", "11110", "00001", "00001", "11110"),
    "6": ("01110", "10000", "10000", "11110", "10001", "10001", "01110"),
    "7": ("11111", "00001", "00010", "00100", "01000", "01000", "01000"),
    "8": ("01110", "10001", "10001", "01110", "10001", "10001", "01110"),
    "9": ("01110", "10001", "10001", "01111", "00001", "00001", "01110"),
    "A": ("01110", "10001", "10001", "11111", "10001", "10001", "10001"),
    "E": ("11111", "10000", "10000", "11110", "10000", "10000", "11111"),
    "H": ("10001", "10001", "10001", "11111", "10001", "10001", "10001"),
    "O": ("01110", "10001", "10001", "10001", "10001", "10001", "01110"),
    "P": ("11110", "10001", "10001", "11110", "10000", "10000", "10000"),
    "S": ("01111", "10000", "10000", "01110", "00001", "00001", "11110"),
    "": ("00000",) * 7,
    "-": ("00000", "00000", "00000", "11111", "00000", "00000", "00000"),
    ":": ("00000", "00100", "00100", "00000", "00100", "00100", "00000"),
    " ": ("00000",) * 7,
}


def put_text(frame, text, x, y, scale=8):
    advance = 6 * scale
    for char in text:
        glyph = FONT.get(char, FONT[" "])
        for row, bits in enumerate(glyph):
            for col, bit in enumerate(bits):
                if bit == "1":
                    for yy in range(scale):
                        py = y + row * scale + yy
                        if py >= HEIGHT:
                            continue
                        start = ((py * WIDTH) + x + col * scale) * 4
                        for xx in range(scale):
                            px = x + col * scale + xx
                            if 0 <= px < WIDTH:
                                frame[start + xx * 4:start + xx * 4 + 4] = b"\x10\x10\x10\xff"
        x += advance


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else "fake-frame.raw.gz"
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime()).upper()
    frame = bytearray(b"\xf0\xf0\xf0\xff" * (WIDTH * HEIGHT))
    # A dark border and a large, readable label make the panel update obvious.
    for y in range(24):
        for x in range(WIDTH):
            for yy in (y, HEIGHT - 1 - y):
                frame[(yy * WIDTH + x) * 4:(yy * WIDTH + x) * 4 + 4] = b"\x10\x10\x10\xff"
    for x in range(24):
        for y in range(HEIGHT):
            for xx in (x, WIDTH - 1 - x):
                frame[(y * WIDTH + xx) * 4:(y * WIDTH + xx) * 4 + 4] = b"\x10\x10\x10\xff"
    put_text(frame, "PHASE 0", 280, 620, 14)
    put_text(frame, stamp, 150, 820, 10)
    if len(frame) != EXPECTED:
        raise RuntimeError(f"generated {len(frame)} bytes, expected {EXPECTED}")
    with gzip.open(output, "wb", compresslevel=6) as stream:
        stream.write(frame)
    print(f"raw={len(frame)} output={output} timestamp={stamp}")


if __name__ == "__main__":
    main()
