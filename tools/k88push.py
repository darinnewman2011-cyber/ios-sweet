#!/usr/bin/env python3
"""Write a disk image raw onto the K88's eMMC from TWRP (adb), at a fixed offset.

    k88push.py IMAGE OFFSET_SECTORS END_SECTORS [--from MIB] [--adb PATH]

The K88's iOS root is the eMMC range from cache's first sector to userdata's
last (xnu patch 0137); the GPT stays as it is. The image goes to
/dev/block/mmcblk0 at byte OFFSET_SECTORS * 512, in 64 MiB chunks:
stretches that are all zeros are written on the tablet from /dev/zero (an
HFS+ file may own zero blocks, so nothing is skipped), the rest is streamed
with `adb push` to /tmp and dd (exec-in on this TWRP's adbd goes through a
pty and mangles binary data). Each chunk is read back and its MD5 compared, and the
whole image must end before END_SECTORS. Unmount /cache, /system and /data in
TWRP first. --from restarts at a chunk boundary (MiB into the image).
"""
import hashlib
import os
import subprocess
import sys

MB = 1 << 20
CHUNK = 64 * MB

args = sys.argv[1:]
adb_path = 'adb'
start_mb = 0
if '--adb' in args:
    i = args.index('--adb')
    adb_path = args[i + 1]
    del args[i:i + 2]
if '--from' in args:
    i = args.index('--from')
    start_mb = int(args[i + 1])
    del args[i:i + 2]
src, off_s, end_s = args[0], int(args[1], 0), int(args[2], 0)
size = os.path.getsize(src)
if (off_s * 512) % MB or size % MB or start_mb * MB % CHUNK:
    raise SystemExit('offset, image size and --from must be MiB / chunk aligned')
if off_s + size // 512 > end_s:
    raise SystemExit(f'image ({size // 512} sectors) does not fit before sector {end_s}')
seek0 = off_s * 512 // MB


def adb(cmd):
    r = subprocess.run([adb_path, 'shell', cmd], capture_output=True)
    if r.returncode:
        raise SystemExit(f'adb {cmd!r} failed: {r.stderr.decode(errors="replace")}')
    return r.stdout.decode(errors='replace').replace('\r', '')


zero = bytes(CHUNK)
tmp = os.path.join(os.environ.get('TEMP', '.'), 'k88chunk.bin')
with open(src, 'rb') as f:
    f.seek(start_mb * MB)
    pos = start_mb * MB
    while pos < size:
        for attempt in range(5):
            try:
                f.seek(pos)
                data = f.read(min(CHUNK, size - pos))
                break
            except OSError as e:        # the \wsl.localhost share sometimes fails a read
                print(f'read at {pos // MB} MiB failed ({e}), retrying', flush=True)
                f.close()
                f = open(src, 'rb')
        else:
            raise SystemExit(f'cannot read the image at {pos // MB} MiB')
        mb, n = pos // MB, len(data) // MB
        seek = seek0 + mb
        if data == zero[:len(data)]:
            adb(f'dd if=/dev/zero of=/dev/block/mmcblk0 bs=1048576 seek={seek} count={n} conv=notrunc,fsync 2>/dev/null')
            kind = 'zero'
        else:
            with open(tmp, 'wb') as t:
                t.write(data)
            r = subprocess.run([adb_path, 'push', tmp, '/tmp/k88chunk.bin'], capture_output=True)
            if r.returncode:
                raise SystemExit(f'adb push failed: {r.stderr.decode(errors="replace")}')
            adb(f'dd if=/tmp/k88chunk.bin of=/dev/block/mmcblk0 bs=1048576 seek={seek} conv=notrunc,fsync 2>/dev/null;'
                ' rm /tmp/k88chunk.bin')
            kind = 'data'
        got = adb(f'dd if=/dev/block/mmcblk0 bs=1048576 skip={seek} count={n} 2>/dev/null | md5sum').split()[0]
        want = hashlib.md5(data).hexdigest()
        if got != want:
            raise SystemExit(f'chunk at {mb} MiB: tablet {got}, image {want}; rerun with --from {mb}')
        pos += len(data)
        print(f'{mb:6d} MiB {kind} ok ({100 * pos // size}%)', flush=True)
print('all chunks verified')
