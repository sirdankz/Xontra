#!/usr/bin/env python3
"""Generate original Xontra dashboard icons without reading cartridge data."""
import os
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'Platform' / 'xbox' / 'gen'

def bitmap(size):
    """An original gold X and three blue network nodes on a navy field."""
    pixels = bytearray()
    padding = bytes((-size * 3) % 4)
    for y in range(size - 1, -1, -1):
        for x in range(size):
            u, v = x * 128 // size, y * 128 // size
            colour = (12, 23 + v // 12, 43 + v // 8)
            if 4 <= min(u, v, 127-u, 127-v) <= 5: colour = (46, 91, 120)
            if 27 <= u <= 100 and 24 <= v <= 97 and (abs(u-v) <= 6 or abs(u+v-127) <= 6):
                colour = (255, 205, 98)
            if 30 <= u <= 98 and 107 <= v <= 108: colour = (46, 117, 154)
            if any(abs(u-node) <= 4 for node in (31, 64, 97)) and 104 <= v <= 112:
                colour = (94, 206, 235)
            pixels.extend(reversed(colour))
        pixels.extend(padding)
    header = struct.pack('<2sIHHI', b'BM', 54 + len(pixels), 0, 0, 54)
    info = struct.pack('<IiiHHIIiiII', 40, size, size, 1, 24, 0, len(pixels), 2835, 2835, 0, 0)
    return header + info + pixels

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for stem, size in [('titleimage', 128), ('saveimage', 64)]:
        temporary = OUT / (stem + '.bmp.tmp')
        temporary.write_bytes(bitmap(size)); os.replace(temporary, OUT / (stem + '.bmp'))
        # Force the bundler to use this original artwork, including on an
        # incremental build that previously used cartridge-derived icons.
        generated = OUT / (stem + '.xbx')
        if generated.exists(): generated.unlink()
    print('make_icons: original Xontra icons generated; no ROM or screenshots read')

if __name__ == '__main__': main()
