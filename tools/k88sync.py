#!/usr/bin/env python3
"""Bring a local copy of the K88's iOS root and the eMMC back in step, 64 MiB chunk by chunk (adb, TWRP).

    k88sync.py pull IMAGE OFFSET_SECTORS [--adb PATH] [--from MIB]
    k88sync.py push IMAGE OFFSET_SECTORS END_SECTORS [--adb PATH] [--from MIB]

Both compare the MD5 of every chunk on the tablet (/dev/block/mmcblk0 from byte OFFSET_SECTORS * 512) with
the same chunk of IMAGE and touch only the chunks that differ:
  pull: copies the tablet's chunk into IMAGE (in place; IMAGE must already hold an older copy of the root,
        e.g. the image last written with tools/k88push.py, so most chunks already match),
  push: writes IMAGE's chunk to the tablet, reads it back and checks the MD5.
So after iOS has run, `pull` fetches only what it changed; after a small edit in WSL, `push` writes only the
edited chunks. Unmount /cache, /system and /data in TWRP first. Pick the device with ANDROID_SERIAL when
more than one is connected (and never print the serial).
"""
import hashlib
import os
import re
import subprocess
import sys

MB = 1 << 20
CHUNK = 64 * MB

args = sys.argv[1:]
adb_path = 'adb'
if '--adb' in args:
    i = args.index('--adb')
    adb_path = args[i + 1]
    del args[i:i + 2]
start_mb = 0
if '--from' in args:
    i = args.index('--from')
    start_mb = int(args[i + 1])
    del args[i:i + 2]
if len(args) < 3 or args[0] not in ('pull', 'push') or (args[0] == 'push' and len(args) < 4):
    raise SystemExit(__doc__)
mode, img, off_s = args[0], args[1], int(args[2], 0)
size = os.path.getsize(img)
if (off_s * 512) % MB or size % MB:
    raise SystemExit('offset and image size must be MiB aligned')
if mode == 'push' and off_s + size // 512 > int(args[3], 0):
    raise SystemExit('the image does not fit before END_SECTORS')
seek0 = off_s * 512 // MB


def scrub(text):
    # adb names the device by its serial in errors ("device 'XXXX' not found"): never print it
    return re.sub(r"device '[^']*'", "device '<serial>'", text)


def adb(cmd):
    r = subprocess.run([adb_path, 'shell', cmd], capture_output=True)
    if r.returncode:
        raise SystemExit(scrub(f'adb {cmd!r} failed: {r.stderr.decode(errors="replace")}'
                               f' (rerun with --from {cur_mb})'))
    return r.stdout.decode(errors='replace').replace('\r', '')


def tablet_md5(mb, n):
    return adb(f'dd if=/dev/block/mmcblk0 bs=1048576 skip={seek0 + mb} count={n} 2>/dev/null | md5sum').split()[0]


cur_mb = start_mb
tmp = os.path.join(os.environ.get('TEMP', '.'), 'k88sync.bin')
changed = 0
with open(img, 'r+b' if mode == 'pull' else 'rb') as f:
    pos = start_mb * MB
    while pos < size:
        cur_mb = pos // MB
        f.seek(pos)
        data = f.read(min(CHUNK, size - pos))
        mb, n = pos // MB, len(data) // MB
        local = hashlib.md5(data).hexdigest()
        remote = tablet_md5(mb, n)
        if remote != local:
            changed += 1
            if mode == 'pull':
                adb(f'dd if=/dev/block/mmcblk0 of=/tmp/k88sync.bin bs=1048576 skip={seek0 + mb} count={n} 2>/dev/null')
                r = subprocess.run([adb_path, 'pull', '/tmp/k88sync.bin', tmp], capture_output=True)
                if r.returncode:
                    raise SystemExit(scrub(f'adb pull failed: {r.stderr.decode(errors="replace")}'))
                adb('rm /tmp/k88sync.bin')
                got = open(tmp, 'rb').read()
                if hashlib.md5(got).hexdigest() != remote or len(got) != len(data):
                    raise SystemExit(f'chunk at {mb} MiB: pulled copy does not match the tablet')
                f.seek(pos)
                f.write(got)
                f.flush()
                print(f'{mb:6d} MiB pulled', flush=True)
            else:
                with open(tmp, 'wb') as t:
                    t.write(data)
                r = subprocess.run([adb_path, 'push', tmp, '/tmp/k88sync.bin'], capture_output=True)
                if r.returncode:
                    raise SystemExit(scrub(f'adb push failed: {r.stderr.decode(errors="replace")}'))
                adb(f'dd if=/tmp/k88sync.bin of=/dev/block/mmcblk0 bs=1048576 seek={seek0 + mb} conv=notrunc,fsync'
                    ' 2>/dev/null; rm /tmp/k88sync.bin')
                if tablet_md5(mb, n) != local:
                    raise SystemExit(f'chunk at {mb} MiB: read back wrong after the write')
                print(f'{mb:6d} MiB pushed and verified', flush=True)
        pos += len(data)
        if mb % 1024 == 0:
            print(f'  ... {100 * pos // size}% compared, {changed} differ so far', flush=True)
print(f'done: {changed} of {size // CHUNK} chunks {"pulled" if mode == "pull" else "pushed"}')
