#!/usr/bin/env python3
"""Read files from the A8's HFSX root image over adb without pulling the whole 17 GB image (read-only).

  hfs-remote.py ls   /private/var/db/diagnostics
  hfs-remote.py get  /private/var/db/diagnostics OUTDIR      (recursive copy of a file or directory)

Blocks are fetched with `adb exec-out dd if=/data/sweet/root.img` in 256 KiB chunks and cached. Handles HFSX
(case-sensitive, binary name order), data forks with extents-overflow records. decmpfs-compressed files are skipped
(reported). Never writes to the phone.
"""
import os, struct, subprocess, sys

IMG = '/data/sweet/root.img'
CHUNK = 64          # blocks per adb fetch


class Remote:
    def __init__(self):
        self.cache = {}
        hdr = self.raw(0, 4096)[1024:1536]
        assert hdr[:2] in (b'H+', b'HX'), hdr[:2]
        self.bs = struct.unpack('>I', hdr[0x28:0x2c])[0]
        self.ext_fork = self.fork(hdr[0xc0:0x110])
        self.cat_fork = self.fork(hdr[0x110:0x160])
        self.overflow = None
        self.cat = BTree(self, self.cat_fork, 4)

    @staticmethod
    def fork(f):
        size = struct.unpack('>Q', f[:8])[0]
        return size, [struct.unpack('>II', f[16 + 8 * i:24 + 8 * i]) for i in range(8) if struct.unpack('>I', f[20 + 8 * i:24 + 8 * i])[0]]

    def raw(self, off, n):
        return subprocess.run(['adb', 'exec-out', f'dd if={IMG} bs=4096 skip={off // 4096} count={(n + 4095) // 4096} 2>/dev/null'],
                              capture_output=True, check=True).stdout

    def block(self, b):
        c = b - b % CHUNK
        if c not in self.cache:
            if len(self.cache) > 4096:
                self.cache.clear()
            d = subprocess.run(['adb', 'exec-out', f'dd if={IMG} bs={self.bs} skip={c} count={CHUNK} 2>/dev/null'],
                               capture_output=True, check=True).stdout
            for i in range(CHUNK):
                self.cache[c + i] = d[i * self.bs:(i + 1) * self.bs]
        return self.cache[b]

    def read_extents(self, extents, size, start=0, length=None):
        """Read bytes [start, start+length) of a fork given its full extent list."""
        if length is None:
            length = size - start
        out, pos = bytearray(), 0
        want_end = start + length
        for sb, cnt in extents:
            ext_bytes = cnt * self.bs
            if pos + ext_bytes > start and pos < want_end:
                b0 = max(start - pos, 0) // self.bs
                b1 = (min(want_end - pos, ext_bytes) + self.bs - 1) // self.bs
                # fetch big runs directly
                i = b0
                while i < b1:
                    n = min(b1 - i, 2048)
                    if n > CHUNK:
                        d = subprocess.run(['adb', 'exec-out', f'dd if={IMG} bs={self.bs} skip={sb + i} count={n} 2>/dev/null'],
                                           capture_output=True, check=True).stdout
                    else:
                        d = b''.join(self.block(sb + i + k) for k in range(n))
                    out += d
                    i += n
                skip = max(start - pos, 0) - b0 * self.bs
                if skip:
                    del out[:skip]
            pos += ext_bytes
            if pos >= want_end:
                break
        return bytes(out[:length])

    def all_extents(self, fileid, fork_extents, size):
        ext = list(fork_extents)
        have = sum(c for _, c in ext) * self.bs
        if have >= size:
            return ext
        if self.overflow is None:
            sz, fe = self.ext_fork
            data = self.read_extents(fe, sz)
            self.overflow = {}
            nsz = struct.unpack('>H', data[32:34])[0]
            for n in range(sz // nsz):
                node = data[n * nsz:(n + 1) * nsz]
                if node[8] != 0xff:       # leaf kind = -1
                    continue
                nrec = struct.unpack('>H', node[10:12])[0]
                for r in range(nrec):
                    o = struct.unpack('>H', node[nsz - 2 * (r + 1):nsz - 2 * r])[0]
                    kl, ft, _, fid, sb = struct.unpack('>HBBII', node[o:o + 12])
                    if ft != 0:
                        continue
                    recs = [struct.unpack('>II', node[o + 12 + 8 * i:o + 20 + 8 * i]) for i in range(8)]
                    self.overflow.setdefault(fid, []).append((sb, [e for e in recs if e[1]]))
        for sb, recs in sorted(self.overflow.get(fileid, [])):
            ext += recs
        return ext


class BTree:
    def __init__(self, r, fork, keylen_size):
        self.r, (self.size, self.ext) = r, fork
        h = self.r.read_extents(self.ext, self.size, 0, 512)
        self.depth, self.root, _, self.first_leaf, _, self.nsz = struct.unpack('>HIIIIH', h[14:34])
        self.nodes = {}

    def node(self, n):
        if n not in self.nodes:
            self.nodes[n] = self.r.read_extents(self.ext, self.size, n * self.nsz, self.nsz)
        return self.nodes[n]

    def records(self, n):
        d = self.node(n)
        flink, _, kind, height, nrec = struct.unpack('>IIbBH', d[:12])
        offs = [struct.unpack('>H', d[self.nsz - 2 * (i + 1):self.nsz - 2 * i])[0] for i in range(nrec + 1)]
        recs = []
        for i in range(nrec):
            o = offs[i]
            kl = struct.unpack('>H', d[o:o + 2])[0]
            key = d[o + 2:o + 2 + kl]
            pid = struct.unpack('>I', key[:4])[0]
            nl = struct.unpack('>H', key[4:6])[0]
            name = key[6:6 + 2 * nl]
            data_off = o + 2 + kl
            data_off += data_off & 1
            recs.append(((pid, name), d[data_off:offs[i + 1]]))
        return flink, kind, recs

    def find_leaf(self, key):
        n = self.root
        while True:
            flink, kind, recs = self.records(n)
            if kind == -1:
                return n
            child = struct.unpack('>I', recs[0][1][:4])[0]
            for k, v in recs:
                if k <= key:
                    child = struct.unpack('>I', v[:4])[0]
                else:
                    break
            n = child

    def iter_from(self, key):
        n = self.find_leaf(key)
        while n:
            flink, kind, recs = self.records(n)
            for k, v in recs:
                if k >= key:
                    yield k, v
            n = flink


def u16(name):
    return name.encode('utf-16-be')


def lookup(r, path):
    pid, rec = 2, None      # root folder ID is 2; its record has parent 1
    parts = [p for p in path.split('/') if p]
    if not parts:
        return 2, ('dir', 2, None)
    for i, p in enumerate(parts):
        key = (pid, u16(p))
        for k, v in r.cat.iter_from(key):
            if k != key:
                raise FileNotFoundError(path)
            t = struct.unpack('>h', v[:2])[0]
            if t == 1:
                rec = ('dir', struct.unpack('>I', v[8:12])[0], v)
            elif t == 2:
                rec = ('file', struct.unpack('>I', v[8:12])[0], v)
            else:
                raise FileNotFoundError(path)
            break
        pid = rec[1]
    return pid, rec


def listdir(r, dirid):
    for k, v in r.cat.iter_from((dirid, b'')):
        if k[0] != dirid:
            return
        t = struct.unpack('>h', v[:2])[0]
        if t in (1, 2):
            yield k[1].decode('utf-16-be'), t, v


def fileinfo(v):
    fid = struct.unpack('>I', v[8:12])[0]
    flags = struct.unpack('>H', v[2:4])[0]
    bsd_flags = v[0x24 + 5]    # ownerFlags at bsdInfo+5 (bsdInfo at 0x20): UF_COMPRESSED = 0x20
    size, ext = Remote.fork(v[88:168])
    return fid, size, ext, bool(bsd_flags & 0x20)


def get(r, path, out):
    _, rec = lookup(r, path)
    if rec[0] == 'file':
        return getfile(r, rec[2], out)
    os.makedirs(out, exist_ok=True)
    for name, t, v in list(listdir(r, rec[1])):
        if t == 1:
            get(r, path.rstrip('/') + '/' + name, os.path.join(out, name))
        else:
            getfile(r, v, os.path.join(out, name))


def getfile(r, v, out):
    fid, size, ext, comp = fileinfo(v)
    if comp:
        print('compressed, skipped:', out, file=sys.stderr)
        return
    ext = r.all_extents(fid, ext, size)
    with open(out, 'wb') as f:
        f.write(r.read_extents(ext, size) if size else b'')


if __name__ == '__main__':
    r = Remote()
    if sys.argv[1] == 'ls':
        _, rec = lookup(r, sys.argv[2])
        if rec[0] == 'file':
            print(fileinfo(rec[2])[1], sys.argv[2])
        else:
            for name, t, v in listdir(r, rec[1]):
                print(('d' if t == 1 else '-'), (fileinfo(v)[1] if t == 2 else ''), name)
    elif sys.argv[1] == 'get':
        get(r, sys.argv[2], sys.argv[3])
