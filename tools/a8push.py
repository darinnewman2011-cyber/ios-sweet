#!/usr/bin/env python3
"""Copy a disk image to the A8 (TWRP, adb) as a sparse file.

    a8push.py IMAGE /data/sweet/root.img

USERDATA has less free space than the iOS root image's size, and simg2img
writes the empty parts as zeros. This truncates the target to the image size
and writes only the 1 MiB blocks that hold data (dd seek, conv=notrunc), so
all-zero stretches stay holes. The kernel's file-backed root (patch 0097)
reads holes as zeros. Finally the phone's SHA-256 is compared with the PC's.
"""
import hashlib
import subprocess
import sys

MB = 1 << 20
GAP = 4 * MB            # zero stretches shorter than this are written anyway
CHUNK = 64 * MB         # bytes per adb call

src, dst = sys.argv[1], sys.argv[2]


def adb(cmd, data=None):
    return subprocess.run(['adb', 'exec-in' if data is not None else 'shell', cmd], input=data,
                          capture_output=True, check=True).stdout


h = hashlib.sha256()
runs = []               # [start_mb, end_mb) holding data
with open(src, 'rb') as f:
    i = 0
    while True:
        b = f.read(MB)
        if not b:
            break
        h.update(b)
        if b.count(0) != len(b):
            if runs and i * MB - runs[-1][1] * MB < GAP:
                runs[-1][1] = i + 1
            else:
                runs.append([i, i + 1])
        i += 1
    size = f.tell()
digest = h.hexdigest()
data_mb = sum(e - s for s, e in runs)
print(f'{src}: {size} bytes, sha256 {digest}, {len(runs)} data runs, {data_mb} MiB to send', flush=True)

adb(f'rm -f {dst} && truncate -s {size} {dst}')
sent = 0
with open(src, 'rb') as f:
    for s, e in runs:
        for c in range(s * MB, e * MB, CHUNK):
            n = min(CHUNK, e * MB - c, size - c)
            f.seek(c)
            adb(f'dd of={dst} bs={MB} seek={c // MB} conv=notrunc 2>/dev/null', f.read(n))
            sent += n
            print(f'\r{sent >> 20}/{data_mb} MiB', end='', flush=True)
print()
out = adb(f'sync; ls -ls {dst}; sha256sum {dst}; stat -c "inode %i" {dst}').decode()
print(out.strip())
if digest not in out:
    sys.exit('SHA-256 MISMATCH on the phone')
print('OK: phone copy matches')
