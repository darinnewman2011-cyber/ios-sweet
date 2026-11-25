#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write the OpenGL ES dispatch table of one iOS build, from its dyld shared cache.

  eagl-slots.py CACHE_DIR [--label TEXT] [-o FILE]
  eagl-slots.py --hfs-image IMAGE [--label TEXT] [-o FILE]

Why this exists: iOS's OpenGL ES is not called by name.  `-[EAGLContext
getMacroContextPrivate]` hands out a table of function pointers, and the
caller -- CoreAnimation's window-server renderer, for one -- calls
`entry(table[0], arguments...)`.  The offsets in that table are a property of
the build's OpenGLES.framework: nothing in the SDK says what they are.  This
tool reads them off the framework's own code.

Each exported `_gl*` function in OpenGLES.framework is a trampoline:

    mrs  x8, tpidrro_el0        ; per-thread state
    ldr  x8, [x8, #0xf0]
    cbz  x8, error
    mov  x1, x0                 ; shift the arguments up by one
    ldr  xN, [x8, #IMM]         ; the function to call
    ldr  x0, [x8, #0x20]        ; the context, the first argument
    br   xN

The table handed out starts at +0x20, so the table offset of an entry is
IMM - 0x20.  Two exported names can share one entry (an APPLE/EXT alias), and
one entry can have no exported name at all.

The output is one line per entry, `OFFSET name [name...]`, sorted by offset,
with a header that names the cache it was read from.  It is data about the
build (offsets and entry names), not Apple code.

CACHE_DIR is the directory holding the split cache: `dyld_shared_cache_arm64`
plus its `.01`, `.02`, ... subcaches (and `.dylddata` / `.dyldlinkedit`), as
an extracted iOS root or an IPSW restore image has them.  `--hfs-image` reads
the same files in place out of an HFS+ iOS root image instead, for a host
without room to extract them (needs pytsk3; the cache files are hard links,
so they are found as `[HFS+ Private Data]/iNode*` by their `dyld_v1` magic).

Needs: python3 and capstone (pip install capstone).  pytsk3 only for
--hfs-image.
"""
import argparse
import os
import re
import struct
import sys

try:
    import capstone
except ImportError:
    sys.exit("eagl-slots.py: needs capstone (pip install capstone)")

DYLD_MAGIC = b"dyld_v1"
IMAGE_NAME = "/System/Library/Frameworks/OpenGLES.framework/OpenGLES"
CONTEXT_OFF = 0x20          # where the table holds the EAGLContext, first argument
TLS_OFF = 0xF0              # where the thread's state pointer lives


class FileNode:
    """One cache file, read by offset."""

    def __init__(self, name, size, read):
        self.name, self.size, self._read = name, size, read

    def read(self, off, n):
        if off < 0 or off >= self.size or n <= 0:
            return b""
        return self._read(off, min(n, self.size - off))


def dir_nodes(d):
    """The split cache files of an extracted cache directory."""
    names = sorted(n for n in os.listdir(d) if n.startswith("dyld_shared_cache_arm64"))
    nodes = []
    for n in names:
        if n.endswith(".symbols"):      # names, no code; not part of the mapping
            continue
        p = os.path.join(d, n)
        if not os.path.isfile(p) or os.path.getsize(p) < 32:
            continue
        f = open(p, "rb")

        def read(off, size, f=f):
            f.seek(off)
            return f.read(size)

        nodes.append(FileNode(n, os.path.getsize(p), read))
    return nodes


def hfs_nodes(image):
    """The cache files of an HFS+ iOS root image, read in place.

    They are hard links (`mkiosroot.sh` links the cryptex's cache into the OS
    root), and their link records confuse a normal HFS+ walk, so they are
    found as the private-data inodes that begin with the cache magic.
    """
    try:
        import pytsk3
    except ImportError:
        sys.exit("eagl-slots.py: --hfs-image needs pytsk3 (pip install pytsk3)")
    fs = pytsk3.FS_Info(pytsk3.Img_Info(image))
    private = None
    for e in fs.open_dir("/"):
        if "Private Data" in e.info.name.name.decode(errors="replace"):
            private = e.info.meta.addr
    if private is None:
        sys.exit("eagl-slots.py: no [HFS+ Private Data] directory in %s" % image)
    nodes = []
    for e in fs.open_dir(inode=private):
        m = e.info.meta
        if not m or m.size < 32:
            continue
        f = fs.open_meta(inode=m.addr)
        if not f.read_random(0, 16).startswith(DYLD_MAGIC):
            continue
        name = e.info.name.name.decode(errors="replace")

        def read(off, size, f=f):
            return f.read_random(off, size)

        nodes.append(FileNode(name, m.size, read))
    return nodes


class Cache:
    """The mapping tables and image list of a split shared cache."""

    def __init__(self, nodes):
        self.maps = []
        self.files = []
        for nd in nodes:
            hdr = nd.read(0, 32)
            if not hdr.startswith(DYLD_MAGIC):
                continue
            mo, mc = struct.unpack_from("<II", hdr, 0x10)
            for i in range(mc):
                a, s, o, _, _ = struct.unpack("<QQQII", nd.read(mo + 32 * i, 32))
                if s:
                    self.maps.append((a, s, nd, o))
            self.files.append(nd)
        self.images = {}
        for nd in self.files:
            io, ic = struct.unpack("<II", nd.read(0x1C0, 8))
            if not ic or not io or io >= nd.size:
                continue
            for i in range(ic):
                a, _, _, po, _ = struct.unpack("<QQQII", nd.read(io + 32 * i, 32))
                name = nd.read(po, 512)
                name = name[:name.find(b"\0")].decode(errors="replace")
                self.images[name] = a
            if self.images:     # the main cache is the one with an image list
                self.main = nd
                break
        else:
            raise SystemExit("eagl-slots.py: no image list in these cache files")

    def read(self, addr, n):
        for a, s, nd, o in self.maps:
            if a <= addr < a + s:
                return nd.read(o + addr - a, n)
        raise KeyError("address %#x is in no mapped subcache" % addr)


class Image:
    """The load commands and exports of one image of the cache."""

    def __init__(self, cache, name):
        if name not in cache.images:
            raise SystemExit("eagl-slots.py: no %s in this cache" % name)
        self.c = cache
        self.base = cache.images[name]
        _, _, _, _, ncmds, _, _, _ = struct.unpack("<IIIIIIII", cache.read(self.base, 32))
        self.segs, self.exports = {}, None
        p = self.base + 32
        for _ in range(ncmds):
            cmd, cs = struct.unpack("<II", cache.read(p, 8))
            if cmd == 0x19:                                     # LC_SEGMENT_64
                seg = cache.read(p + 8, 16).rstrip(b"\0").decode()
                vm, vs, fo, _ = struct.unpack("<QQQQ", cache.read(p + 24, 32))
                self.segs[seg] = (vm, vs, fo)
            elif cmd == 0x80000033:                             # LC_DYLD_EXPORTS_TRIE
                self.exports = struct.unpack("<II", cache.read(p + 8, 8))
            elif cmd in (0x80000022, 0x22):                     # LC_DYLD_INFO(_ONLY)
                d = struct.unpack("<10I", cache.read(p + 8, 40))
                self.exports = (d[8], d[9])
            p += cs

    def le_addr(self, fileoff):
        vm, _, fo = self.segs["__LINKEDIT"]
        return vm + fileoff - fo

    def export_list(self):
        """name -> address, from the export trie."""
        off, size = self.exports
        data = self.c.read(self.le_addr(off), size)
        out = {}

        def uleb(i):
            r = s = 0
            while True:
                b = data[i]
                i += 1
                r |= (b & 0x7F) << s
                s += 7
                if b < 0x80:
                    return r, i

        def walk(i, pfx):
            ts, j = uleb(i)
            if ts:
                flags, k = uleb(j)
                if not flags & 8:              # not a re-export
                    delta, _ = uleb(k)
                    out[pfx] = self.base + delta
            j += ts                            # i2 + ts
            n = data[j]
            j += 1
            for _ in range(n):
                e = data.index(b"\0", j)
                label = data[j:e].decode(errors="replace")
                j = e + 1
                child, j = uleb(j)
                walk(child, pfx + label)

        walk(0, "")
        return out


def table_offset(md, code, addr):
    """The table offset a trampoline jumps through, or None.

    The trampoline keeps the thread's state in whichever register it read
    tpidrro_el0 into (x8 when the arguments fit in registers, x9 with a frame
    when they do not), loads the entry from [state + IMM], puts the context
    from [state + 0x20] in x0, and jumps or calls (br / blr).
    """
    state, loads = None, []
    for ins in md.disasm(code, addr):
        if ins.mnemonic in ("br", "blr", "ret"):
            break
        if ins.mnemonic == "mrs" and ins.op_str.endswith("tpidrro_el0"):
            state = ins.op_str.split(",")[0].strip()
            continue
        if state is None:
            continue
        m = re.match(r"^(x\d+), \[%s, #(0x[0-9a-f]+|\d+)\]$" % state, ins.op_str)
        if ins.mnemonic != "ldr" or not m:
            continue
        reg, imm = m.group(1), int(m.group(2), 0)
        if reg == state or imm == CONTEXT_OFF:
            continue
        loads.append(imm)
        if len(loads) > 4:
            break
    return loads[0] - CONTEXT_OFF if len(loads) == 1 else None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("cache", nargs="?", help="directory of dyld_shared_cache_arm64*")
    ap.add_argument("--hfs-image", help="read the cache in place from this HFS+ image")
    ap.add_argument("--label", default="", help="build named in the header")
    ap.add_argument("-o", "--output", help="write here instead of stdout")
    a = ap.parse_args()
    if bool(a.cache) == bool(a.hfs_image):
        ap.error("give either CACHE_DIR or --hfs-image")

    nodes = hfs_nodes(a.hfs_image) if a.hfs_image else dir_nodes(a.cache)
    if not nodes:
        sys.exit("eagl-slots.py: no dyld cache files found")
    cache = Cache(nodes)
    img = Image(cache, IMAGE_NAME)

    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.detail = False
    entries, unknown = {}, []
    for name, addr in sorted(img.export_list().items()):
        if not name.startswith("_gl"):
            continue
        off = table_offset(md, cache.read(addr, 96), addr)
        if off is None:
            unknown.append(name)
        elif off >= 0:
            entries.setdefault(off, []).append(name[1:])

    lines = []
    if a.label:
        lines.append("# %s: the OpenGL ES dispatch table of -[EAGLContext getMacroContextPrivate]" % a.label)
    else:
        lines.append("# The OpenGL ES dispatch table of -[EAGLContext getMacroContextPrivate]")
    lines.append("# Generated by tools/jackpotlte/eagl-slots.py from this build's dyld")
    lines.append("# shared cache.  An entry is called as entry(table[0], arguments...);")
    lines.append("# the table's entry 0 is the EAGLContext.  Do not edit.")
    for off in sorted(entries):
        lines.append("%#06x %s" % (off, " ".join(entries[off])))
    text = "\n".join(lines) + "\n"
    if a.output:
        with open(a.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    print("eagl-slots.py: %d entries, %d names%s" % (
        len(entries), sum(len(v) for v in entries.values()),
        "; no trampoline: " + " ".join(unknown) if unknown else ""), file=sys.stderr)


if __name__ == "__main__":
    main()