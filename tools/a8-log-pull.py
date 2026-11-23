#!/usr/bin/env python3
"""Pull the A8's persistent console log (xnu 0168, boot-arg sweetlog=2) from TWRP.

  a8-log-pull.py OUT.txt

The kernel keeps a 4 MiB console ring (kernel printf + daemons' /dev/console output) in the last 4 MiB + 512 B
of SYSTEM (mmcblk0p19): sector 0 = header "SWEETLOG" u32 version, u32 ring size, u64 total bytes, u64 flush seq;
then the ring (byte n at n % size). Reads only. Checks ro.product.device == jackpotlte first.
"""
import struct, subprocess, sys

ADB = 'adb'
def sh(cmd):
    return subprocess.run([ADB, 'shell', cmd], capture_output=True, text=True, check=True).stdout.strip()

out = sys.argv[1]
if sh('getprop ro.product.device') != 'jackpotlte':
    sys.exit('not the A8 (jackpotlte) in TWRP')
dev = '/dev/block/mmcblk0p19'
sectors = int(sh(f'cat /sys/block/mmcblk0/mmcblk0p19/size'))
ring_sectors = 1 + (4 << 20) // 512
skip = sectors - ring_sectors
raw = subprocess.run([ADB, 'exec-out', f'dd if={dev} bs=512 skip={skip} count={ring_sectors} 2>/dev/null'],
                     capture_output=True, check=True).stdout
if raw[:8] != b'SWEETLOG':
    sys.exit(f'no SWEETLOG header at the end of SYSTEM ({len(raw)} bytes read)')
ver, size, total, seq = struct.unpack_from('<IIQQ', raw, 8)
ring = raw[512:512 + size]
if total <= size:
    text = ring[:total]
else:
    a = total % size
    text = ring[a:] + ring[:a]
open(out, 'wb').write(text.replace(b'\0', b''))
print(f'{out}: {len(text)} bytes (total {total}, flush {seq}, ring {size})')
