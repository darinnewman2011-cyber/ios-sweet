#!/usr/bin/env python3
"""Pull the kernel's screenshots (/private/var/tmp/sweetshot-NN-WxH.bgra, xnu 0927) from the A8 root and make PNGs.

  sweetshot.py OUTDIR            (needs the phone in TWRP; reads via hfs-remote.py, read-only)
"""
import os, re, struct, subprocess, sys, zlib

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
tools = os.path.dirname(os.path.abspath(__file__))
ls = subprocess.run([sys.executable, os.path.join(tools, 'hfs-remote.py'), 'ls', '/private/var/tmp'],
                    capture_output=True, text=True, check=True).stdout
for line in ls.splitlines():
    m = re.search(r'(sweetshot-(\d+)-(\d+)x(\d+)\.bgra)$', line)
    if not m:
        continue
    name, w, h = m.group(1), int(m.group(3)), int(m.group(4))
    raw_path = os.path.join(out, name)
    subprocess.run([sys.executable, os.path.join(tools, 'hfs-remote.py'), 'get', '/private/var/tmp/' + name, raw_path], check=True)
    d = open(raw_path, 'rb').read()
    if len(d) < w * h * 4:
        print('short', name, len(d))
        continue
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        px = d[y * w * 4:(y + 1) * w * 4]
        rgb = bytearray(w * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = px[2::4], px[1::4], px[0::4]      # BGRA -> RGB
        rows += rgb
    png = os.path.join(out, name.replace('.bgra', '.png'))
    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b))
    with open(png, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(bytes(rows), 6)) + chunk(b'IEND', b''))
    os.remove(raw_path)
    print(png)
