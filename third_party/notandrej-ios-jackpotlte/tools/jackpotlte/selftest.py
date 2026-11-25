#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Self-test for the jackpotlte boot pipeline (no device, no kernel needed).

Builds a tiny synthetic Mach-O kernel, runs it through the real tools:
  tools/mkpayload.py  (Mach-O parse + chained-fixup walk + segment table)
  tools/jackpotlte_mkboot.py  (Samsung v0 packer)
  tools/jackpotlte_bootimg_info.py  (unpacker)
and checks the results. This catches pipeline regressions in CI cheaply.

It does NOT prove anything about a real XNU kernel or the phone; it only
proves the packing chain is internally consistent.

Usage: tools/jackpotlte/selftest.py
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)

PAGE = 0x4000


def build_fake_macho(path):
    """A minimal 64-bit Mach-O: __TEXT + __DATA, one LC_UNIXTHREAD entry.

    Layout (file offsets equal vmaddrs, linked at vmaddr 0x100000000):
      [0, 0x4000)  header + load commands + __TEXT bytes
      [0x4000,)    __DATA
    Enough for mkpayload.py to parse segments and the entry PC.
    """
    text_vm, data_vm = 0x100000000, 0x100004000
    text = bytearray(PAGE)
    data = bytearray(0x2000)
    data[0:4] = b'DATA'
    entry_pc = text_vm + 0x1000

    # arm_thread_state64: x0..x28, fp, lr, sp, pc, cpsr (34 qwords)
    ts = [0] * 34
    ts[32] = entry_pc
    state = struct.pack('<34Q', *ts)
    # LC_UNIXTHREAD: cmd, cmdsize, flavor (ARM_THREAD_STATE64=6), count (words)
    unixthread = struct.pack('<IIII', 0x5, 16 + len(state), 6, len(state) // 4) + state

    lc_seg = lambda name, vmaddr, vmsize, fileoff, filesize: (
        struct.pack('<II', 0x19, 72) + name.ljust(16, b'\0') +
        struct.pack('<4Q', vmaddr, vmsize, fileoff, filesize) +
        struct.pack('<IIII', 3, 3, 1, 0))
    lcs = (lc_seg(b'__TEXT', text_vm, PAGE, 0, PAGE) +
           lc_seg(b'__DATA', data_vm, len(data), PAGE, len(data)) +
           unixthread)
    ncmds = 3
    hdr = struct.pack('<8I', 0xfeedfacf, 0x0100000c, 0, 2, ncmds, len(lcs), 0, 0)
    blob = bytearray(hdr + lcs)
    if len(blob) > PAGE:
        sys.exit('selftest: load commands exceed one page')
    blob += b'\0' * (PAGE - len(blob))          # pad to __TEXT file size
    blob += data
    open(path, 'wb').write(blob)
    return dict(text_vm=text_vm, data_vm=data_vm, entry=entry_pc,
                text_size=PAGE, data_size=len(data))


def build_stock_bootimg(path):
    """A stock Samsung boot.img v0 with a DTBH 'dt' section (like the SM-A530F)."""
    page = 4096
    kaddr, raddr, tags = 0x800b4000, 0x89000000, 0x8a000000
    k, r, s = b'\0' * 0x1000, b'\0' * 0x800, b''
    dt = b'DTBH' + b'\0' * 64

    def pad(b):
        return b + b'\0' * (-len(b) % page)

    hdr = bytearray(page)
    hdr[0:8] = b'ANDROID!'
    struct.pack_into('<10I', hdr, 8, len(k), kaddr, len(r), raddr,
                     len(s), 0, tags, page, len(dt), 0)
    hdr[0x30:0x38] = b'jackpot\0'
    open(path, 'wb').write(bytes(hdr) + pad(k) + pad(r) + pad(s) + dt + b'SEANDROIDENFORCE')
    return dict(kaddr=kaddr, raddr=raddr, tags=tags, dtsz=len(dt))


def run(*cmd):
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f'FAIL: {" ".join(cmd)}\n{res.stdout}\n{res.stderr}')
    return res.stdout


def main():
    failures = []
    with tempfile.TemporaryDirectory() as d:
        kern = os.path.join(d, 'kernel')
        stock = os.path.join(d, 'stock.img')
        adt = os.path.join(d, 'jackpotlte.adt')
        payload = os.path.join(d, 'payload.bin')
        boot = os.path.join(d, 'out.img')
        unpack = os.path.join(d, 'unpacked')

        macho = build_fake_macho(kern)
        stockinfo = build_stock_bootimg(stock)

        run(sys.executable, os.path.join(TOOLS, 'mkadt_jackpotlte.py'), adt)
        out = run(sys.executable, os.path.join(TOOLS, 'mkpayload.py'), kern, adt,
                  payload, '--cmdline', 'debug=0xc cpus=1')
        blob = open(payload, 'rb').read()
        if blob[:8] != b'SWEETXNU':
            failures.append('payload magic is not SWEETXNU')
        if len(blob) < 4096:
            failures.append(f'payload suspiciously small: {len(blob)}')
        # entry VA from the header (offset 0x18) must equal the Mach-O entry
        entry_va = struct.unpack_from('<Q', blob, 0x18)[0]
        if entry_va != macho['entry']:
            failures.append(f'payload entry {entry_va:#x} != macho entry {macho["entry"]:#x}')
        # adt_off/adt_size (0x20/0x28) must be inside the blob
        adt_off, adt_size = struct.unpack_from('<2Q', blob, 0x20)
        if not (0 < adt_size and adt_off + adt_size <= len(blob)):
            failures.append('payload ADT bounds out of range')

        # loader is required by the packer; build does not need a real kernel here,
        # so call build.sh without the pack, then pack with the real packer.
        run('bash', os.path.join(ROOT, 'shim', 'jackpotlte', 'build.sh'))
        run('bash', os.path.join(ROOT, 'shim', 'jackpotlte', 'build.sh'),
            stock, payload, boot)
        info = run(sys.executable, os.path.join(TOOLS, 'jackpotlte_bootimg_info.py'),
                   boot)
        img = open(boot, 'rb').read()
        if img[-len(b'SEANDROIDENFORCE'):] != b'SEANDROIDENFORCE':
            failures.append('packed image missing SEANDROIDENFORCE trailer')
        # the header must keep the stock load addresses
        (ksz, kaddr, rsz, raddr, ssz, saddr, tags, page, dtsz, osver) = \
            struct.unpack_from('<10I', img, 8)
        if (kaddr, raddr, tags) != (stockinfo['kaddr'], stockinfo['raddr'], stockinfo['tags']):
            failures.append('packed header did not keep the stock load addresses')
        if ksz < 16384:
            failures.append(f'packed kernel too small: {ksz}')

        # Shim-only probe image: no payload, empty ramdisk, must still pack.
        probe = os.path.join(d, 'probe.img')
        run('bash', os.path.join(ROOT, 'shim', 'jackpotlte', 'build.sh'),
            '--probe', stock, probe)
        pimg = open(probe, 'rb').read()
        (pksz, _, prsz, _, _, _, _, _, _, _) = struct.unpack_from('<10I', pimg, 8)
        if pksz < 16384:
            failures.append(f'probe kernel too small: {pksz}')
        if pimg[-len(b'SEANDROIDENFORCE'):] != b'SEANDROIDENFORCE':
            failures.append('probe image missing SEANDROIDENFORCE trailer')

        print(info.strip())

    if failures:
        print('\nSELFTEST FAILED:')
        for f in failures:
            print('  -', f)
        return 1
    print('\nSELFTEST PASSED (pipeline internally consistent; no device claims)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
