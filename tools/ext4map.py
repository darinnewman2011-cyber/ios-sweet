#!/usr/bin/env python3
"""Map an ext4 file to partition blocks, read over adb from TWRP (read-only).

    ext4map.py /dev/block/mmcblk0p29 INODE

Prints the inode flags and one line per extent: file block, length, partition
block, written/unwritten. Same walk as the A8 kernel's file-backed root
(bsd/dev/sweet_ufs.c, SWEET_JACKPOTLTE): superblock, group descriptor, inode,
extent tree. Used to check a root image before booting it.
"""
import struct
import subprocess
import sys

dev, ino = sys.argv[1], int(sys.argv[2])


def rd(off, n):
    # dd in 512-byte units so any offset works with toybox dd
    s, e = off // 512, (off + n + 511) // 512
    out = subprocess.run(['adb', 'exec-out', f'dd if={dev} bs=512 skip={s} count={e - s} 2>/dev/null'],
                         capture_output=True, check=True).stdout
    return out[off - s * 512:off - s * 512 + n]


sb = rd(1024, 1024)
magic, = struct.unpack_from('<H', sb, 0x38)
assert magic == 0xEF53, hex(magic)
bsz = 1024 << struct.unpack_from('<I', sb, 0x18)[0]
ipg, = struct.unpack_from('<I', sb, 0x28)
isz, = struct.unpack_from('<H', sb, 0x58)
incompat, = struct.unpack_from('<I', sb, 0x60)
first, = struct.unpack_from('<I', sb, 0x14)
dsz = struct.unpack_from('<H', sb, 0xFE)[0] if incompat & 0x80 else 32
print(f'block {bsz}, inodes/group {ipg}, inode size {isz}, desc size {dsz}, incompat {incompat:#x}')

g, idx = divmod(ino - 1, ipg)
gd = rd((first + 1) * bsz + g * dsz, dsz)
itab = struct.unpack_from('<I', gd, 8)[0] | ((struct.unpack_from('<I', gd, 0x28)[0] << 32) if dsz >= 64 else 0)
inode = rd(itab * bsz + idx * isz, isz)
mode, = struct.unpack_from('<H', inode, 0)
size = struct.unpack_from('<I', inode, 4)[0] | (struct.unpack_from('<I', inode, 0x6C)[0] << 32)
flags, = struct.unpack_from('<I', inode, 0x20)
print(f'inode {ino}: mode {mode:o}, size {size}, flags {flags:#x}'
      f'{" ENCRYPTED" if flags & 0x800 else ""}{" EXTENTS" if flags & 0x80000 else " NO-EXTENTS"}'
      f'{" INLINE" if flags & 0x10000000 else ""}')


def walk(node):
    m, n, _, depth = struct.unpack_from('<HHHH', node, 0)
    assert m == 0xF30A, hex(m)
    for i in range(n):
        e = node[12 + 12 * i:24 + 12 * i]
        if depth == 0:
            lblk, ln, hi, lo = struct.unpack('<IHHI', e)
            unw = ln > 32768
            yield lblk, ln - 32768 if unw else ln, (hi << 32) | lo, unw
        else:
            lblk, lo, hi, _ = struct.unpack('<IIHH', e)
            yield from walk(rd(((hi << 32) | lo) * bsz, bsz))


total = 0
for lblk, ln, pblk, unw in walk(inode[0x28:0x28 + 60]):
    total += ln
    print(f'  file blk {lblk:>9} len {ln:>6} -> part blk {pblk:>9}{"  UNWRITTEN" if unw else ""}')
print(f'{total} blocks mapped ({total * bsz} bytes)')
