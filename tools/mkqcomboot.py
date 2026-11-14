#!/usr/bin/env python3
"""Wrap an arm64 Image into a Qualcomm LK (aboot) boot image for `fastboot boot`.

    mkqcomboot.py TEMPLATE_BOOT.img Image DTB OUT.img [--cmdline TEXT]

Layout follows the device's own boot image (TEMPLATE): Android boot image v0
header with the same load addresses, page size, os_version and (unless
--cmdline is given) cmdline. The kernel is gzip(Image) followed by the DTB, the
"Image.gz-dtb" form LK expects when dt_size is 0: LK inflates the kernel and
looks for appended DTBs right after the gzip stream, then picks the one whose
qcom,msm-id/board-id match the board. The ramdisk is a single zero page (the
shim never reads it). The id field is SHA-1 over the parts, as mkbootimg does.
"""
import argparse, gzip, hashlib, struct, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('template'); ap.add_argument('image'); ap.add_argument('dtb'); ap.add_argument('out')
    ap.add_argument('--cmdline')
    a = ap.parse_args()
    t = open(a.template, 'rb').read(4096)
    if t[:8] != b'ANDROID!':
        sys.exit('template is not an Android boot image')
    (_, kaddr, _, raddr, _, saddr, tags, page, dt_size, osver) = struct.unpack_from('<10I', t, 8)
    if dt_size:
        sys.exit('template uses a separate dt.img (dt_size != 0); this tool only does appended DTBs')
    img = open(a.image, 'rb').read()
    if img[56:60] != b'ARM\x64':
        sys.exit('not an arm64 Image (no ARM\x64 magic)')
    dtb = open(a.dtb, 'rb').read()
    if dtb[:4] != b'\xd0\x0d\xfe\xed':
        sys.exit('DTB has no FDT magic')
    kernel = gzip.compress(img, 9, mtime=0) + dtb
    ramdisk = bytes(page)
    cmdline = a.cmdline.encode() if a.cmdline is not None else t[64:64 + 512].split(b'\0')[0]
    if len(cmdline) >= 512:
        sys.exit('cmdline too long')
    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b''):
        sha.update(blob); sha.update(struct.pack('<I', len(blob)))
    hdr = bytearray(page)
    hdr[0:8] = b'ANDROID!'
    struct.pack_into('<10I', hdr, 8, len(kernel), kaddr, len(ramdisk), raddr, 0, saddr, tags, page, 0, osver)
    hdr[64:64 + len(cmdline)] = cmdline
    hdr[576:576 + 20] = sha.digest()
    pad = lambda b: b + bytes(-len(b) % page)
    out = bytes(hdr) + pad(kernel) + pad(ramdisk)
    open(a.out, 'wb').write(out)
    print('%s: %d bytes, kernel %d (Image %d + DTB %d), page %#x, kernel_addr %#x'
          % (a.out, len(out), len(kernel), len(img), len(dtb), page, kaddr))

main()
