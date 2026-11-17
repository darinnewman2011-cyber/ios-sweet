#!/usr/bin/env python3
"""Give an A8 (jackpotlte) ADT the iPad 7 (iPad7,11 / J171AP) identity, for the iPadOS userland.

    adt-ipad.py IN.adt J171AP.adt OUT.adt

IN is the A8 tree with the D22AP identity (e.g. jackpotlte-product.adt). The hardware nodes stay as they are; the
root's compatible/model/target-type/target-sub-type become J171AP's, and the /product node gets the real J171AP
tree's /product properties (iBoot's syscfg placeholders and node bookkeeping left out, as mkadt.py does). J171AP.adt
is an Apple artifact (DeviceTree.j171ap.im4p from the iPadOS IPSW, `ipsw img4 im4p extract`); keep it out of git.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt


def props(n):
    return dict((k, v) for k, f, v in n.props)


def product(root):
    for c in root.children:
        if props(c).get('name') == b'product\0':
            return c
    return None


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    src, ref, dst = sys.argv[1:]
    root, _ = adt.parse(open(src, 'rb').read())
    j171, _ = adt.parse(open(ref, 'rb').read())
    jr = props(j171)
    assert jr.get('model') == b'iPad7,11\0', jr.get('model')

    compat = [c for c in props(root)['compatible'].split(b'\0') if c]
    compat = [b'J171AP', b'iPad7,11'] + [c for c in compat if c not in (b'D22AP', b'iPhone10,3')]
    new = {'compatible': b'\0'.join(compat) + b'\0', 'model': jr['model'],
           'target-type': jr['target-type'], 'target-sub-type': jr['target-sub-type']}
    for i, (k, f, v) in enumerate(root.props):
        if k in new:
            print(f'/{k}: {v!r} -> {new[k]!r}')
            root.props[i] = (k, 0, new[k])

    pa, pj = product(root), product(j171)
    assert pa is not None and pj is not None
    keep = [(k, f, v) for k, f, v in pa.props if k in ('name', 'AAPL,phandle')]
    pa.props = keep + [(k, 0, v) for k, f, v in pj.props if not f and k not in ('name', 'AAPL,phandle')]
    pp = props(pa)
    print(f'/product: {len(pa.props) - len(keep)} J171AP properties, product-name {pp.get("product-name")!r}, '
          f'idiom {pp.get("artwork-device-idiom")!r}')
    open(dst, 'wb').write(adt.serialize(root))


if __name__ == '__main__':
    main()
