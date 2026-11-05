#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""phxshot2png.py: turn a `sysctl -b debug.phxshot` dump (PhxBringup b41+: 'PSHT', width, height, VRAM offset, then
B8G8R8A8 rows) into a PNG, standard library only.  Usage: phxshot2png.py shot.raw [out.png] [x y w h]  (optional crop)"""
import struct, sys, zlib

def main():
    src = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.png'
    data = open(src, 'rb').read()
    magic, w, h, off = struct.unpack_from('<4I', data)
    if magic != 0x54485350:
        sys.exit(f'{src}: not a phxshot dump (magic 0x{magic:08x})')
    x0, y0, cw, ch = (int(v) for v in sys.argv[3:7]) if len(sys.argv) >= 7 else (0, 0, w, h)
    px = memoryview(data)[16:]
    rows = bytearray()
    for y in range(y0, y0 + ch):
        line = px[(y * w + x0) * 4:(y * w + x0 + cw) * 4]
        rgb = bytearray(cw * 3)
        rgb[0::3] = line[2::4]; rgb[1::3] = line[1::4]; rgb[2::3] = line[0::4]   # BGRA -> RGB
        rows += b'\0' + rgb
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b'')
    open(out, 'wb').write(png)
    print(f'{out}: {cw}x{ch} from {w}x{h} at VRAM 0x{off:x}')

main()
