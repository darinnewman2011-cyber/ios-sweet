#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read and extend the iOS display-mode asset catalog from an iOS root.

   car-mode.py --catalog Assets.car --list
   car-mode.py --catalog Assets.car --verify 1080x2220 --scale 3
   car-mode.py --catalog Assets.car --add 1080x2220 --scale 3

What this edits
---------------
`System/Library/PrivateFrameworks/UIKitCore.framework/BoundingPathData.bundle/
Assets.car` is a BOMStore holding one rendition per iPhone display mode.  It is
the list UIKit consults for a mode's *bounding path*: UIKitCore builds the
name with the C format string at UIKitCore+0x119db13,

    BoundingPathBitmap-%ldx%ld-%ldx%ld-%.2fx

(native width, native height, points width, points height, display scale) and
loads it as `UIKit_BoundingPathData` from `BoundingPathData.bundle`
(`UIKitCore+0x11e8fa6`, `+0x11e8fbd`).  31 names -> 23 renditions, keyed on
`(scale, idiom, identifier, element=85, part=181)`; each rendition is a
layout-1000 ("Data") CSI whose payload is a DWAR-wrapped `bplist00`
NSKeyedArchiver of `_UIBoundingPathBitmap`.

--add appends a mode.  It is idempotent, writes atomically, then re-reads the
file from disk and refuses (non-zero) unless every part of the entry reads
back.  It does NOT touch any other rendition: every existing block keeps its
offset and bytes, and only the three B+ tree roots, the CARHEADER rendition
count, the vars table and the block index move.

Limit that matters
------------------
The per-mode *payload* (`_UIBoundingPathBitmap` -> `rows`, `skipRegion.*`) is
the rounded-corner node list for that exact point size.  That geometry cannot
be computed here: it needs the device's screen corner radius and the node
encoding.  --add therefore writes a structurally complete entry whose path is
a plain rectangle (`nodeCount` 0, `skipRegion` = the whole bitmap).  That is
correct for a screen with no rounded corners and is self-consistent; whether
UIKit accepts it for this panel is a boot question, not an offline one.
"""
import argparse
import os
import plistlib
import struct
import sys
import tempfile

BOM_MAGIC = b"BOMStore"
ASSET_REL = ("System/Library/PrivateFrameworks/UIKitCore.framework/"
             "BoundingPathData.bundle/Assets.car")

# The layout-1000 TVL observed on every BoundingPathBitmap rendition in CoreUI
# 805: tag 1004 = (float 0.0, float 1.0), tag 1006 = uint32 1.  It carries no
# size, so a new rendition reuses it verbatim.
BP_TVL = bytes.fromhex("ec03000008000000000000000000803f"
                       "ee0300000400000001000000")
assert len(BP_TVL) == 28

ELEMENT = 85          # kRenditionKeyElementBitmap
PART = 181            # kRenditionKeyPartImage
ID_ATTR = 17
ID_ELEMENT = 1
ID_PART = 2
LAYOUT_DATA = 1000
PIXEL_DATA = 0x44415441   # 'ATAD' on disk == 'DATA' LE
SCALE_IN_KEY = 1          # every rendition key in this catalog carries scale 1
IDIOM_PHONE = 1


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


def le16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def le32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class Catalog:
    """Minimal BOMStore / CAR reader (container big-endian, CSI little-endian)."""

    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        if d[:8] != BOM_MAGIC:
            raise ValueError("%s: not a BOMStore" % path)
        self.version = be32(d, 8)
        self.nblocks = be32(d, 12)
        self.indexoff = be32(d, 16)
        self.indexlen = be32(d, 20)
        self.varsoff = be32(d, 24)
        self.varslen = be32(d, 28)
        self.indexcount = be32(d, self.indexoff)
        self.blocks = [
            (be32(d, self.indexoff + 4 + 8 * i),
             be32(d, self.indexoff + 8 + 8 * i))
            for i in range(self.indexcount)
        ]
        self.vars = []            # ordered [(name, block_id)]
        self.vars_len_computed = None
        o = self.varsoff
        n = be32(d, o)
        p = o + 4
        for _ in range(n):
            bid = be32(d, p)
            p += 4
            klen = d[p]
            p += 1
            self.vars.append((d[p:p + klen].decode("latin-1"), bid))
            p += klen
        self.vars_len_computed = p - o
        self.varmap = dict(self.vars)

    def block(self, bid):
        o, ln = self.blocks[bid]
        if o == 0xFFFFFFFF or ln == 0:
            raise ValueError("block %d is empty/free" % bid)
        return self.data[o:o + ln]

    def tree_entries(self, name):
        """[(key_block_id_or_inline_u32, value_block_id)] in stored order."""
        hdr = self.block(self.varmap[name])
        if hdr[:4] != b"tree":
            raise ValueError("%s: not a tree" % name)
        root = be32(hdr, 8)
        out = []
        stack = [root]
        seen = set()
        while stack:
            bid = stack.pop()
            if bid in seen:
                continue
            seen.add(bid)
            nd = self.block(bid)
            leaf = struct.unpack_from(">H", nd, 0)[0]
            cnt = struct.unpack_from(">H", nd, 2)[0]
            fwd = be32(nd, 4)
            if leaf:
                for i in range(cnt):
                    v = be32(nd, 12 + 8 * i)
                    k = be32(nd, 16 + 8 * i)
                    out.append((k, v))
                if fwd:
                    stack.append(fwd)
            else:
                for i in range(cnt):
                    stack.append(be32(nd, 12 + 8 * i))
        return out

    def tree_header(self, name):
        return self.block(self.varmap[name])


def packed_key(ident):
    return struct.pack("<HHHHH", SCALE_IN_KEY, IDIOM_PHONE, ident,
                       ELEMENT, PART)


def facet_value(ident):
    return (struct.pack("<HHH", 0, 0, 3) +
            struct.pack("<HH", ID_ELEMENT, ELEMENT) +
            struct.pack("<HH", ID_PART, PART) +
            struct.pack("<HH", ID_ATTR, ident))


def bitmap_descriptor():
    # Copied from the catalog's own BoundingPathBitmap BITMAPKEYS descriptors.
    return struct.pack("<IIIIII", 1, 0, 0x18, 5, 2, 2) + b"\xff" * 12


def build_leaf(items):
    """items: [(value_id, key_field_u32, sort_key_bytes)] -> leaf node bytes."""
    items = sorted(items, key=lambda t: t[2])
    out = struct.pack(">HHII", 1, len(items), 0, 0)
    for value_id, key_field, _ in items:
        out += struct.pack(">II", value_id, key_field)
    return out


def retarget_tree_header(hdr, root, count):
    b = bytearray(hdr)
    struct.pack_into(">I", b, 8, root)
    struct.pack_into(">I", b, 16, count)
    return bytes(b)


def render_plist(width, height):
    """A _UIBoundingPathBitmap with a rectangular (no-corner) path."""
    obj = {
        "$version": 100000,
        "$archiver": "NSKeyedArchiver",
        "$top": {"root": plistlib.UID(1)},
        "$objects": [
            "$null",
            {
                "height": height,
                "$class": plistlib.UID(2),
                "nodeCount": 0,
                "width": width,
                "rows": b"",
                "skipRegion.y": 0,
                "maximumNodesPerRow": 5,
                "skipRegion.height": height,
            },
            {"$classname": "_UIBoundingPathBitmap",
             "$classes": ["_UIBoundingPathBitmap", "NSObject"]},
        ],
    }
    return plistlib.dumps(obj, fmt=plistlib.FMT_BINARY, sort_keys=True)


def build_csi(name, width, height, tvl=BP_TVL):
    pl = render_plist(width, height)
    body = b"DWAR" + struct.pack("<II", 0, len(pl)) + pl
    hdr = struct.pack("<III", 0x43545349, 1, 0)              # ISTC, v1, flags
    hdr += struct.pack("<IIIIIII", 0, 0, 100, PIXEL_DATA, 0, 0, LAYOUT_DATA)
    hdr += name.encode("ascii").ljust(128, b"\0")[:128]
    hdr += struct.pack("<IIII", len(tvl), 1, 0, len(body))
    if len(hdr) != 184:
        raise AssertionError("CSI header is %d bytes, expect 184" % len(hdr))
    return hdr + tvl + body


def asset_name(canvas, points, scale):
    return "BoundingPathBitmap-%dx%d-%dx%d-%.2fx" % (
        canvas[0], canvas[1], points[0], points[1], scale)


def find_mode(cat, name):
    """Return (ident, rendition_key_block, csi) or None."""
    for k, v in cat.tree_entries("FACETKEYS"):
        if cat.block(k) != name.encode("ascii"):
            continue
        val = cat.block(v)
        n = le16(val, 4)
        pairs = [(le16(val, 6 + 4 * i), le16(val, 8 + 4 * i)) for i in range(n)]
        ident = dict(pairs).get(ID_ATTR)
        if ident is None:
            return None
        for rk, rv in cat.tree_entries("RENDITIONS"):
            kb = cat.block(rk)
            if le16(kb, 4) == ident:
                return ident, rk, cat.block(rv)
        return ident, None, None
    return None


def describe(cat):
    rows = []
    for k, v in cat.tree_entries("FACETKEYS"):
        name = cat.block(k).decode("latin-1")
        val = cat.block(v)
        n = le16(val, 4)
        ident = dict((le16(val, 6 + 4 * i), le16(val, 8 + 4 * i))
                     for i in range(n)).get(ID_ATTR)
        rows.append((name, ident))
    return rows


def csi_plist(csi):
    if len(csi) < 184:
        return None
    tvl = le32(csi, 168)
    body = csi[184 + tvl:]
    if body[:4] != b"DWAR":
        return None
    pl = body[12:12 + le32(body, 8)]
    try:
        return plistlib.loads(pl)
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Adding one mode
# ---------------------------------------------------------------------------

def add_mode(path, canvas, points, scale, verbose=False):
    cat = Catalog(path)
    name = asset_name(canvas, points, scale)
    have = find_mode(cat, name)
    if have and have[1] is not None:
        pl = csi_plist(have[2]) if have[2] else None
        if pl and (pl["$objects"][1].get("width"),
                   pl["$objects"][1].get("height")) == canvas:
            if verbose:
                print("car-mode: %s already present (identifier %d)"
                      % (name, have[0]))
            return 0
        raise SystemExit("car-mode: %s exists but is not %dx%d; refusing to "
                         "rewrite a foreign entry" % (name, canvas[0], canvas[1]))

    # Identifier: a fixed, unused 16-bit value.  0x2220 first, then upwards.
    used = set()
    for _, v in cat.tree_entries("FACETKEYS"):
        val = cat.block(v)
        n = le16(val, 4)
        for i in range(n):
            if le16(val, 6 + 4 * i) == ID_ATTR:
                used.add(le16(val, 8 + 4 * i))
    ident = None
    for cand in [0x2220] + list(range(0x2221, 0x10000)):
        if cand not in used:
            ident = cand
            break
    if ident is None:
        raise SystemExit("car-mode: no free 16-bit rendition identifier")

    # Free block slots above the last real block.
    real = [i for i, (o, ln) in enumerate(cat.blocks)
            if o != 0xFFFFFFFF and not (o == 0 and ln == 0)]
    base = max(real) + 1
    need = 11
    count = cat.indexcount
    if base + need > count:
        count = max(256, ((base + need) + 3) // 4 * 4)
    if base + need > count:
        raise SystemExit("car-mode: catalog has no free block slots")

    # --- new blocks -------------------------------------------------------
    new = {}                       # block_id -> bytes
    i = base

    def take(data):
        nonlocal i
        bid = i
        new[bid] = data
        i += 1
        return bid

    csi_id = take(build_csi(name + ".data", canvas[0], canvas[1]))
    fkey_id = take(name.encode("ascii"))
    fval_id = take(facet_value(ident))
    rkey_id = take(packed_key(ident))
    bval_id = take(bitmap_descriptor())

    # RENDITIONS leaf: existing (value,key) + ours, sorted by key bytes.
    rend = [(v, k, cat.block(k)) for k, v in cat.tree_entries("RENDITIONS")]
    rend.append((csi_id, rkey_id, packed_key(ident)))
    leaf = build_leaf([(v, k, kb) for v, k, kb in rend])
    rend_leaf = take(leaf)
    rend_hdr = take(retarget_tree_header(
        cat.tree_header("RENDITIONS"), rend_leaf, len(rend)))

    # FACETKEYS leaf: sorted by name bytes.
    facet = [(v, k, cat.block(k)) for k, v in cat.tree_entries("FACETKEYS")]
    facet.append((fval_id, fkey_id, name.encode("ascii")))
    leaf = build_leaf([(v, k, kb) for v, k, kb in facet])
    facet_leaf = take(leaf)
    facet_hdr = take(retarget_tree_header(
        cat.tree_header("FACETKEYS"), facet_leaf, len(facet)))

    # BITMAPKEYS leaf: inline key = identifier (big-endian for the sort).
    bmap = [(v, k, struct.pack(">I", k))
            for k, v in cat.tree_entries("BITMAPKEYS")]
    bmap.append((bval_id, ident, struct.pack(">I", ident)))
    leaf = build_leaf([(v, k, sk) for v, k, sk in bmap])
    bmap_leaf = take(leaf)
    bmap_hdr = take(retarget_tree_header(
        cat.tree_header("BITMAPKEYS"), bmap_leaf, len(bmap)))

    max_id = i - 1
    count = max(count, ((max_id + 1) + 3) // 4 * 4)

    # --- rebuild the container -------------------------------------------
    out = bytearray(cat.data)
    # CARHEADER renditionCount at +16.  The BOMStore *wrapper* is big-endian
    # but every CAR block payload is little-endian (coreuiVersion reads
    # `25 03` == 805 and renditionCount `17 00` == 23), so this field is an LE
    # u32 even though it sits in a block indexed by a BE table.
    ho, hl = cat.blocks[cat.varmap["CARHEADER"]]
    old_renditions = len(cat.tree_entries("RENDITIONS"))
    struct.pack_into("<I", out, ho + 16, old_renditions + 1)

    # Keep every existing block where it is; append the new ones at the first
    # 16-byte boundary after the last existing block.
    end = max(o + ln for o, ln in cat.blocks if o != 0xFFFFFFFF)
    pos = (end + 15) & ~15
    out = out[:pos]
    offsets = {}
    for bid in sorted(new):
        pos = (len(out) + 15) & ~15
        out += b"\0" * (pos - len(out))
        offsets[bid] = (pos, len(new[bid]))
        out += new[bid]

    # vars table (same order, three ids retargeted)
    retarget = {"RENDITIONS": rend_hdr, "FACETKEYS": facet_hdr,
                "BITMAPKEYS": bmap_hdr}
    varsoff = len(out)
    vt = struct.pack(">I", len(cat.vars))
    for nm, bid in cat.vars:
        bid = retarget.get(nm, bid)
        nb = nm.encode("latin-1")
        vt += struct.pack(">I", bid) + bytes([len(nb)]) + nb
    out += vt

    # block index
    indexoff = (len(out) + 3) & ~3
    out += b"\0" * (indexoff - len(out))
    idx = struct.pack(">I", count)
    for bid in range(count):
        if bid in offsets:
            o, ln = offsets[bid]
        elif bid < cat.indexcount:
            o, ln = cat.blocks[bid]
        else:
            o, ln = 0, 0
        idx += struct.pack(">II", o, ln)
    out += idx
    out += b"\0" * 20                      # observed 20-byte BOMStore trailer

    out[0:8] = BOM_MAGIC
    struct.pack_into(">I", out, 8, 1)            # version
    struct.pack_into(">I", out, 12, max_id)      # nblocks
    struct.pack_into(">I", out, 16, indexoff)
    struct.pack_into(">I", out, 20, len(idx) + 20)
    struct.pack_into(">I", out, 24, varsoff)
    struct.pack_into(">I", out, 28, len(vt))

    # --- verify before writing -------------------------------------------
    verify_bytes(bytes(out), name, canvas, points, scale, ident)

    d = os.path.dirname(os.path.abspath(path))
    fd, tmp = tempfile.mkstemp(prefix=".Assets.car.", dir=d)
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(out)
        os.replace(tmp, path)
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)

    # --- verify what actually landed on disk -----------------------------
    verify_file(path, name, canvas, points, scale, ident)
    print("car-mode: added %s (identifier %d)" % (name, ident))
    return 0


def _check(cat, name, canvas, points, scale, ident):
    # The count CoreUI reads first: it must equal the number of renditions,
    # in the block's own little-endian encoding.
    want_count = len(cat.tree_entries("RENDITIONS"))
    got_count = le32(cat.block(cat.varmap["CARHEADER"]), 16)
    if got_count != want_count:
        raise SystemExit("car-mode: FAIL CARHEADER renditionCount %d != %d "
                         "renditions" % (got_count, want_count))
    have = find_mode(cat, name)
    if not have:
        raise SystemExit("car-mode: FAIL %s not found after write" % name)
    got_ident, rk, csi = have
    if got_ident != ident:
        raise SystemExit("car-mode: FAIL %s identifier %d != %d"
                         % (name, got_ident, ident))
    if rk is None or csi is None:
        raise SystemExit("car-mode: FAIL %s has no rendition" % name)
    kb = cat.block(rk)
    if [le16(kb, 2 * j) for j in range(5)] != [SCALE_IN_KEY, IDIOM_PHONE,
                                               ident, ELEMENT, PART]:
        raise SystemExit("car-mode: FAIL %s rendition key is wrong" % name)
    pl = csi_plist(csi)
    if pl is None:
        raise SystemExit("car-mode: FAIL %s payload is not a bplist" % name)
    root = pl["$objects"][1]
    if (root.get("width"), root.get("height")) != canvas:
        raise SystemExit("car-mode: FAIL %s bitmap %rx%r != %r"
                         % (name, root.get("width"), root.get("height"),
                            canvas))
    oname = csi[40:168].split(b"\0")[0].decode("latin-1")
    if oname != name + ".data":
        raise SystemExit("car-mode: FAIL %s CSI name %r" % (name, oname))
    if not any(k == ident for k, _ in cat.tree_entries("BITMAPKEYS")):
        raise SystemExit("car-mode: FAIL %s has no BITMAPKEYS entry" % name)


def verify_bytes(data, name, canvas, points, scale, ident):
    p = os.path.join(tempfile.gettempdir(), ".car-mode-verify.car")
    with open(p, "wb") as f:
        f.write(data)
    try:
        _check(Catalog(p), name, canvas, points, scale, ident)
    finally:
        os.unlink(p)


def verify_file(path, name, canvas, points, scale, ident):
    _check(Catalog(path), name, canvas, points, scale, ident)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--catalog", help="path to Assets.car")
    ap.add_argument("--root", help="iOS root; implies --catalog")
    ap.add_argument("--list", action="store_true", help="print the mode list")
    ap.add_argument("--verify", metavar="CANVAS",
                    help="canvas WxH, e.g. 1080x2220")
    ap.add_argument("--add", metavar="CANVAS",
                    help="canvas WxH to add a bounding-path mode for")
    ap.add_argument("--scale", type=int, default=3)
    ap.add_argument("--points", help="points WxH (default: canvas / scale)")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv[1:])

    path = a.catalog
    if a.root:
        path = os.path.join(a.root, ASSET_REL)
    if not path:
        return "car-mode.py: pass --catalog or --root"
    if not os.path.exists(path):
        return "car-mode.py: no catalog at %s" % path

    cat = Catalog(path)
    if a.list:
        for name, ident in describe(cat):
            print("%-56s %6d" % (name, ident))
        return 0

    if not (a.add or a.verify):
        return "car-mode.py: pass --list, --verify WxH or --add WxH"

    canvas = tuple(int(x) for x in (a.add or a.verify).split("x"))
    points = (tuple(int(x) for x in a.points.split("x")) if a.points
              else (canvas[0] // a.scale, canvas[1] // a.scale))
    name = asset_name(canvas, points, a.scale)

    if a.add:
        return add_mode(path, canvas, points, a.scale, a.verbose)

    have = find_mode(cat, name)
    if not have:
        print("car-mode: %s is NOT in %s" % (name, path))
        return 1
    ident, rk, csi = have
    pl = csi_plist(csi) if csi else None
    dims = None
    if pl:
        dims = (pl["$objects"][1].get("width"), pl["$objects"][1].get("height"))
    print("car-mode: %s present: identifier %d, bitmap %s"
          % (name, ident, "%dx%d" % dims if dims else "?"))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv) or 0)