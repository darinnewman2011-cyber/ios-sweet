#!/usr/bin/env python3
"""Pack the K88's WCNSS (Pronto) firmware and NV file into one blob for shim/k88 (-DK88_WIFI, WIFIBLOB=...).

Copyright (c) 2026 Darin Newman. Written for ios-sweet.

    python tools/k88wifiblob.py FWDIR NVFILE OUT.blob

FWDIR holds wcnss.mdt and wcnss.b00..bNN (from the tablet's modem partition, image/), or a single wcnss.mbn.
The firmware files stay out of git.

Blob layout (little endian):
    0   "K88WIFI1"
    8   u32 count of segments
    12  u32 metadata offset, 16 u32 metadata size   (ELF header + program headers, then the hash segment:
                                                     what TrustZone's PAS init call checks)
    20  u32 NV offset,       24 u32 NV size
    28  u32 min paddr,       32 u32 max paddr (end of the highest segment)
    36  u32 relocatable (1 if any segment has the relocatable flag)
    40  segments: paddr, filesz, memsz, data offset   (16 bytes each)
    data, each item 4 KiB aligned
"""
import os
import struct
import sys

PT_LOAD = 1
MDT_TYPE_MASK = 7 << 24
MDT_TYPE_HASH = 2 << 24
MDT_RELOCATABLE = 1 << 27


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    fwdir, nvpath, out = sys.argv[1:]
    mbn = os.path.join(fwdir, 'wcnss.mbn')
    mdt = os.path.join(fwdir, 'wcnss.mdt')
    whole = open(mbn if os.path.exists(mbn) else mdt, 'rb').read()
    if whole[:4] != b'\x7fELF' or whole[4] != 1:
        sys.exit('not a 32-bit ELF')
    e_phoff, = struct.unpack_from('<I', whole, 28)
    e_phentsize, e_phnum = struct.unpack_from('<HH', whole, 42)
    ph = [struct.unpack_from('<8I', whole, e_phoff + i * e_phentsize) for i in range(e_phnum)]
    # p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align
    hashi = [i for i, p in enumerate(ph) if (p[6] & MDT_TYPE_MASK) == MDT_TYPE_HASH]
    if not hashi:
        sys.exit('no hash segment')
    hashi = hashi[0]

    def segdata(i):
        p = ph[i]
        seg = os.path.join(fwdir, 'wcnss.b%02d' % i)
        if os.path.exists(seg) and not os.path.exists(mbn):
            d = open(seg, 'rb').read()
        else:
            d = whole[p[1]:p[1] + p[4]]
        if len(d) < p[4]:
            sys.exit('segment %d short: %d < %d' % (i, len(d), p[4]))
        return d[:p[4]]

    ehdr_size = ph[0][4]
    hash_size = ph[hashi][4]
    if len(whole) == ehdr_size + hash_size:
        hashdata = whole[ehdr_size:ehdr_size + hash_size]
    else:
        hashdata = segdata(hashi)
    meta = whole[:ehdr_size] + hashdata

    segs = []
    reloc = 0
    for i, p in enumerate(ph):
        if p[0] != PT_LOAD or (p[6] & MDT_TYPE_MASK) == MDT_TYPE_HASH or p[5] == 0:
            continue
        reloc |= 1 if p[6] & MDT_RELOCATABLE else 0
        segs.append((p[3], p[4], p[5], segdata(i) if p[4] else b''))
    lo = min(s[0] for s in segs)
    hi = max(s[0] + s[2] for s in segs)
    nv = open(nvpath, 'rb').read()

    def align(n):
        return (n + 0xfff) & ~0xfff

    hdr_len = align(40 + 16 * len(segs))
    blob = bytearray(hdr_len)
    items = [meta, nv] + [s[3] for s in segs]
    offs = []
    for it in items:
        offs.append(len(blob))
        blob += it
        blob += b'\0' * (align(len(blob)) - len(blob))
    struct.pack_into('<8s8I', blob, 0, b'K88WIFI1', len(segs), offs[0], len(meta), offs[1], len(nv), lo, hi, reloc)
    for k, s in enumerate(segs):
        struct.pack_into('<4I', blob, 40 + 16 * k, s[0], s[1], s[2], offs[2 + k])
    open(out, 'wb').write(blob)
    print('%s: %d bytes, meta %d (hash seg %d, %d bytes), %d segments %#x-%#x%s, NV %d' %
          (out, len(blob), len(meta), hashi, hash_size, len(segs), lo, hi, ' relocatable' if reloc else '', len(nv)))
    for s in segs:
        print('  paddr %#010x filesz %#8x memsz %#8x' % s[:3])


if __name__ == '__main__':
    main()
