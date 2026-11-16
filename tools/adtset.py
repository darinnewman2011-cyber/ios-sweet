#!/usr/bin/env python3
"""Set (or add) properties in one node of an ADT, e.g. MobileGestalt answers in /product.

  adtset.py IN.adt OUT.adt product artwork-device-subtype=u32:2556 artwork-dynamic-displaymode=str:0

Types: u32:N (little-endian), str:TEXT (NUL-terminated), hex:BYTES. The node is matched by its name property
at any depth. Example: artwork-device-subtype 2436 is the iPhone X notch, 2556 the iPhone 14 Pro Dynamic Island.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt


def value(spec):
    t, _, v = spec.partition(':')
    if t == 'u32':
        return struct.pack('<I', int(v, 0))
    if t == 'str':
        return v.encode() + b'\0'
    if t == 'hex':
        return bytes.fromhex(v)
    raise SystemExit(f'bad value {spec!r} (u32:/str:/hex:)')


def find(n, name):
    if dict((k, v) for k, f, v in n.props).get('name') == name.encode() + b'\0':
        return n
    for c in n.children:
        r = find(c, name)
        if r:
            return r
    return None


def main():
    if len(sys.argv) < 5:
        raise SystemExit(__doc__)
    src, dst, name = sys.argv[1:4]
    root, _ = adt.parse(open(src, 'rb').read())
    n = find(root, name)
    if n is None:
        raise SystemExit(f'no node {name!r}')
    for a in sys.argv[4:]:
        k, _, spec = a.partition('=')
        v = value(spec)
        for i, (pk, pf, pv) in enumerate(n.props):
            if pk == k:
                print(f'{k}: {pv!r} -> {v!r}')
                n.props[i] = (k, 0, v)
                break
        else:
            print(f'{k}: added {v!r}')
            n.props.append((k, 0, v))
    open(dst, 'wb').write(adt.serialize(root))


if __name__ == '__main__':
    main()
