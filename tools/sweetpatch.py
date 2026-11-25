#!/usr/bin/env python3
"""Root patches the A8 kernel applies at boot (xnu 0937), carried in the boot image's device tree.

  sweetpatch.py PATCHES.json IN.adt OUT.adt [--root MOUNTED_ROOT]

PATCHES.json:
  {"patches": [
     {"path": "/usr/libexec/usermanagerd", "offset": "0x9abc", "new": "1f2003d5", "old": "e0030032",
      "note": "why this patch exists"}
  ]}

"old" may be left out when --root points at a mounted copy of the same root (e.g. the studio root.img mounted read-only):
the current bytes are read from there. The kernel only writes an entry when the phone's file still holds "old" at
"offset", skips it when "new" is already there, and refuses anything else, logging each result ("SWEET PATCH: ...").
Blob: "SWPATCH1" u32 count, then u16 path_len, path, u64 offset, u32 len, old, new (little endian), stored as
/chosen "sweet-patches".
"""
import json, os, struct, subprocess, sys

src, adt_in, adt_out = sys.argv[1], sys.argv[2], sys.argv[3]
root = sys.argv[sys.argv.index('--root') + 1] if '--root' in sys.argv else None
spec = json.load(open(src))
out = bytearray(b'SWPATCH1' + struct.pack('<I', len(spec['patches'])))
for i, e in enumerate(spec['patches']):
    path = e['path']
    if not path.startswith('/'):
        raise SystemExit(f'entry {i}: path must be absolute: {path}')
    off = int(str(e['offset']), 0)
    new = bytes.fromhex(e['new'])
    if 'old' in e:
        old = bytes.fromhex(e['old'])
    elif root:
        with open(os.path.join(root, path.lstrip('/')), 'rb') as f:
            f.seek(off)
            old = f.read(len(new))
    else:
        raise SystemExit(f'entry {i}: no "old" bytes and no --root to read them from')
    if len(old) != len(new) or not new:
        raise SystemExit(f'entry {i}: old/new length mismatch ({len(old)} vs {len(new)})')
    if root and not os.path.exists(os.path.join(root, path.lstrip('/'))):
        state = 'not in --root copy, not checked'
    elif root:
        p = os.path.join(root, path.lstrip('/'))
        with open(p, 'rb') as f:
            f.seek(off)
            cur = f.read(len(new))
        state = 'already new' if cur == new else ('matches old' if cur == old else 'DIFFERS from old')
    else:
        state = 'not checked'
    pb = path.encode()
    out += struct.pack('<H', len(pb)) + pb + struct.pack('<QI', off, len(new)) + old + new
    print(f'  {path} @0x{off:x} {len(new)} bytes ({state}) {e.get("note", "")}')
here = os.path.dirname(os.path.abspath(__file__))
subprocess.run([sys.executable, '-I', os.path.join(here, 'adtset.py'), adt_in, adt_out, 'chosen',
                f'sweet-patches=hex:{out.hex()}'], check=True, stdout=subprocess.DEVNULL)
print(f'{adt_out}: /chosen sweet-patches = {len(out)} bytes, {len(spec["patches"])} entries')
