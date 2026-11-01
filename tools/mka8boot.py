#!/usr/bin/env python3
"""Wrap an arm64 Image into a Galaxy A8 (2018) / Samsung S-Boot boot.img.

Takes the phone's own boot.img as the template and keeps its legacy v0
header fields, cmdline and its Samsung DTBH device-tree bundle byte for
byte; only the kernel and ramdisk are replaced. S-Boot selects the DTB
for the board revision from that bundle, so it must stay.

  mka8boot.py TEMPLATE_BOOT.img Image OUT.img [--ramdisk RD.gz]

Without --ramdisk a minimal empty cpio.gz is used (S-Boot dislikes an
empty ramdisk). Refuses to write an image larger than the 32 MiB BOOT
partition. Standard library only.
"""
import gzip, hashlib, struct, sys

BOOT_PART = 32 * 1024 * 1024
PAGE_DEFAULT = 2048


def cpio_empty():
    name = b'TRAILER!!!\0'
    hdr = b'070701' + b''.join(b'%08X' % v for v in (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0))
    blob = hdr + name
    blob += b'\0' * (-len(blob) % 4)
    blob += b'\0' * (-len(blob) % 512)
    return gzip.compress(blob, 9, mtime=0)


def pages(n, page):
    return (n + page - 1) // page * page


def boot_id(kernel, ramdisk, second, dt):
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        h.update(blob)
        h.update(struct.pack('<I', len(blob)))
    if dt:
        h.update(dt)
        h.update(struct.pack('<I', len(dt)))
    return h.digest()


def main():
    args = sys.argv[1:]
    rd_path = None
    if '--ramdisk' in args:
        i = args.index('--ramdisk')
        rd_path = args[i + 1]
        del args[i:i + 2]
    if len(args) != 3:
        raise SystemExit(__doc__)
    tmpl_path, image_path, out_path = args
    t = open(tmpl_path, 'rb').read()
    if t[:8] != b'ANDROID!':
        raise SystemExit('template is not an Android boot image')
    ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, dtsz, osv = struct.unpack_from('<10I', t, 8)
    if page != PAGE_DEFAULT:
        print(f'note: template page size is {page}', file=sys.stderr)
    off = page
    k_old = t[off:off + ksz]; off += pages(ksz, page)
    r_old = t[off:off + rsz]; off += pages(rsz, page)
    s_old = t[off:off + ssz]; off += pages(ssz, page)
    dt = t[off:off + dtsz]
    if dtsz and dt[:4] != b'DTBH':
        raise SystemExit('template DT section is not a DTBH bundle')
    # Self-check: does our id formula reproduce the template's own id?
    ok = boot_id(k_old, r_old, s_old, dt) == t[576:596]
    print('template id reproduced:', ok, file=sys.stderr)

    kernel = open(image_path, 'rb').read()
    ramdisk = open(rd_path, 'rb').read() if rd_path else cpio_empty()
    hdr = bytearray(t[:page])
    struct.pack_into('<I', hdr, 8, len(kernel))
    struct.pack_into('<I', hdr, 16, len(ramdisk))
    hdr[576:608] = boot_id(kernel, ramdisk, b'', dt).ljust(32, b'\0')
    out = bytes(hdr)
    for blob in (kernel, ramdisk, b'', dt):
        out += blob + b'\0' * (pages(len(blob), page) - len(blob))
    if len(out) > BOOT_PART:
        raise SystemExit(f'image is {len(out)} bytes, BOOT partition holds {BOOT_PART}')
    out += b'\0' * (BOOT_PART - len(out))   # pad to the partition size like the stock image
    open(out_path, 'wb').write(out)
    print(f'{out_path}: kernel {len(kernel)}, ramdisk {len(ramdisk)}, dt {len(dt)}, total {len(out)}',
          file=sys.stderr)


if __name__ == '__main__':
    main()
