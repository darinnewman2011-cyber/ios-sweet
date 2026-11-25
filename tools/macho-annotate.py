#!/usr/bin/env python3
"""Disassemble an address range of a Mach-O (arm64) and annotate adrp+add/ldr pairs with the C strings, CFStrings
and Objective-C selector names they point at, and bl targets with stub symbol names. For reading daemons on the
translated iOS roots (what does this function log, call and decide?).

  macho-annotate.py BINARY START END        (hex addresses, e.g. 0x100036200 0x100036400)
"""
import re, struct, subprocess, sys

binp, start, end = sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16)
data = open(binp, 'rb').read()

# segments: vmaddr -> fileoff
segs = []
ncmds = struct.unpack('<I', data[16:20])[0]
off = 32
for _ in range(ncmds):
    cmd, cs = struct.unpack('<II', data[off:off + 8])
    if cmd == 0x19:
        vm, vs, fo, fs = struct.unpack('<QQQQ', data[off + 24:off + 56])
        segs.append((vm, vs, fo, fs))
    off += cs


def at(va, n):
    for vm, vs, fo, fs in segs:
        if vm <= va < vm + vs:
            o = fo + va - vm
            return data[o:o + n] if o < fo + fs else None
    return None


def cstr(va):
    b = at(va, 300)
    if not b:
        return None
    s = b.split(b'\0')[0]
    if len(s) >= 2 and all(32 <= c < 127 or c in (9, 10) for c in s):
        return s.decode()
    return None


def describe(va):
    s = cstr(va)
    if s:
        return repr(s)
    p = at(va, 32)       # CFString: isa, flags, ptr, len  /  selref: pointer to name
    if p and len(p) >= 8:
        ptr = struct.unpack('<Q', p[:8])[0] & 0xFFFFFFFFF
        if 0x100000000 <= ptr < 0x200000000:
            s = cstr(ptr)
            if s:
                return 'sel/ptr ' + repr(s)
        if len(p) >= 32:
            ptr2 = struct.unpack('<Q', p[16:24])[0] & 0xFFFFFFFFF
            s = cstr(ptr2)
            if s:
                return '@' + repr(s)
    return None


stubs = {}
for line in subprocess.run(['otool', '-Iv', binp], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r'(0x[0-9a-f]+)\s+\d+\s+(\S+)', line)
    if m:
        stubs[int(m.group(1), 16)] = m.group(2)

dis = subprocess.run(['xcrun', 'llvm-objdump', '-d', '--no-show-raw-insn', f'--start-address={start:#x}',
                      f'--stop-address={end:#x}', binp], capture_output=True, text=True).stdout
pages = {}
for line in dis.splitlines():
    m = re.match(r'\s*([0-9a-f]+):\s+(\S+)\s*(.*)', line)
    if not m:
        continue
    addr, op, args = int(m.group(1), 16), m.group(2), m.group(3)
    note = ''
    if op == 'adrp':
        r = re.match(r'(x\d+),\s*0x([0-9a-f]+)', args)
        if r:
            pages[r.group(1)] = int(r.group(2), 16)
    elif op in ('add', 'ldr'):
        r = re.match(r'(x\d+),\s*\[?(x\d+),\s*#(0x[0-9a-f]+|\d+)', args)
        if r and r.group(2) in pages:
            va = pages[r.group(2)] + int(r.group(3), 0)
            d = describe(va) if op == 'add' else None
            if op == 'ldr':
                p = at(va, 8)
                if p:
                    tgt = struct.unpack('<Q', p)[0] & 0xFFFFFFFFF
                    d = describe(tgt) or describe(va)
            if d:
                note = '   ; ' + d
    elif op in ('bl', 'b'):
        r = re.match(r'0x([0-9a-f]+)', args)
        if r and int(r.group(1), 16) in stubs:
            note = '   ; -> ' + stubs[int(r.group(1), 16)]
    args = re.sub(r'\s*<[^>]*>', '', args)
    print(f'{addr:x}: {op:8s} {args}{note}')
