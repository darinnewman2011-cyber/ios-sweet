#!/usr/bin/env python3
"""Write an empty, valid HFS+ journal into an image made by mkfs.hfsplus.

    hfs-jnl-init.py IMAGE [JHDR_SIZE]

Linux mkfs.hfsplus -J leaves the journal info block flagged NeedInit and the
journal header zeroed, so XNU's hfs_late_journal_init() calls journal_create()
and writes the header at mount. The A8 root device is read-only, so that write
fails (EROFS) and root does not mount. This writes the header the way
journal_create() would (hfs-627.100.6 core/hfs_journal.c: magic JNLx, native
little-endian, start = end = jhdr_size, checksum over the bytes before
sequence_num) and clears NeedInit, so XNU opens the journal without writing.
JHDR_SIZE must match the device's logical block size at mount (512 here: the
volume is below 2^31 sectors, so hfs_mountfs keeps 512).
"""
import struct
import sys

img = sys.argv[1]
jhdr_size = int(sys.argv[2]) if len(sys.argv) > 2 else 512


def calc_checksum(b):
    c = 0
    for x in b:
        c = ((c << 8) ^ (c + x)) & 0xFFFFFFFF
    return ~c & 0xFFFFFFFF


with open(img, 'r+b') as f:
    f.seek(1024)
    vh = f.read(512)
    sig, = struct.unpack('>2s', vh[:2])
    attr, = struct.unpack('>I', vh[4:8])
    jib_blk, = struct.unpack('>I', vh[12:16])
    bs, = struct.unpack('>I', vh[40:44])
    if sig not in (b'H+', b'HX') or not attr & 0x2000:
        sys.exit(f'not a journaled HFS+ volume ({sig}, attr {attr:#x})')
    f.seek(jib_blk * bs)
    jib = bytearray(f.read(52))
    flags, = struct.unpack('>I', jib[:4])
    off, size = struct.unpack('>QQ', jib[36:52])
    if not flags & 1:
        sys.exit(f'journal not in this volume (flags {flags:#x})')
    if size % jhdr_size or not 256 * 1024 <= size <= 1 << 30:
        sys.exit(f'journal size {size:#x} unusable')
    blhdr_size = 16 * 1024            # block_list_header; recomputed by size_up_tbuffer at open
    hdr = struct.pack('<IIqqqiIi', 0x4A4E4C78, 0x12345678, jhdr_size, jhdr_size, size, blhdr_size, 0, jhdr_size)
    hdr = hdr[:36] + struct.pack('<I', calc_checksum(hdr)) + hdr[40:]
    hdr += struct.pack('<I', 1)       # sequence_num
    assert len(hdr) == 48
    f.seek(off)
    f.write(hdr + bytes(jhdr_size - len(hdr)))
    struct.pack_into('>I', jib, 0, flags & ~4)
    f.seek(jib_blk * bs)
    f.write(bytes(jib[:4]))
    print(f'journal at {off:#x} size {size:#x}: header written (jhdr {jhdr_size}), JIB flags {flags:#x} -> {flags & ~4:#x}')
    print(f'patched byte ranges: [{jib_blk * bs:#x}, +4) [{off:#x}, +{jhdr_size})')
