#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""phxshot.py <out.png> [x y w h]: grab what HUBP0 scans (sysctl debug.phxshot on the box named by $HOST, default 'box') as a PNG, optionally cropped.
Note: debug.phxshot reads the console surface (A); with GPU present on, the screen is a VRAM slot and the shot can lag.
PHXSHOT_SYSCTL=debug.phxfb1_shot (PhxAccel m7head2b+): the M7 S3a phantom head 1's surface instead (same format).
PHXSHOT_RAW=<file> (10-30): convert a raw shot already on this Mac instead of grabbing."""
import os, struct, subprocess, sys, tempfile, zlib

out = sys.argv[1]
crop = list(map(int, sys.argv[2:6])) if len(sys.argv) >= 6 else None
HOST = os.environ.get('HOST', 'box')  # ssh name of the machine running the kext
name = os.environ.get('PHXSHOT_SYSCTL', 'debug.phxshot')
assert name.replace('.', '').replace('_', '').isalnum(), 'bad sysctl name'
if os.environ.get('PHXSHOT_RAW'):
    d = open(os.environ['PHXSHOT_RAW'], 'rb').read()
else:
    subprocess.run(['ssh', '-n', HOST, 'sudo -n sh -c "sysctl -b %s > /tmp/ps.raw"' % name], check=True)
    tmp = tempfile.mktemp(suffix='.raw')
    subprocess.run(['scp', '-q', HOST + ':/tmp/ps.raw', tmp], check=True)
    d = open(tmp, 'rb').read(); os.unlink(tmp)
magic, W, H, _ = struct.unpack_from('<4I', d)
assert magic == 0x54485350, 'not a phxshot'
x0, y0, w, h = crop or (0, 0, W, H)
rows = bytearray()
for y in range(y0, y0 + h):
    src = d[16 + (y * W + x0) * 4:16 + (y * W + x0 + w) * 4]
    line = bytearray(w * 3); line[0::3] = src[2::4]; line[1::3] = src[1::4]; line[2::3] = src[0::4]
    rows += b'\0' + line
def chunk(t, b): return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
open(out, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                      + chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))
print(out, w, h)
