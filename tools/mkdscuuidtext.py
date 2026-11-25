#!/usr/bin/env python3
"""Build the unified-log uuidtext 'dsc' string file for a dyld shared cache, so macOS `log show` can decode
messages from shared-cache libraries ("<compose failure [shared UUID]>" otherwise).

  mkdscuuidtext.py CACHE_DIR/dyld_shared_cache_arm64e OUT_UUIDTEXT_DIR
  -> OUT_UUIDTEXT_DIR/dsc/<CACHEUUID>

Format (version 2, 'hcsd'): header {magic u32, major u16=2, minor u16=0, nranges u32, nuuids u32},
ranges {v_off u64, data_off u32, size u32, uuid_index u64}, uuids {text_off u64, text_size u32, uuid[16],
path_off u32}, then string blobs and paths. Offsets are relative to the cache's first mapping address.
"""
import glob, os, struct, sys, uuid as uuidm

main = sys.argv[1]
out = sys.argv[2]

files = {}


def mappings(path):
    with open(path, 'rb') as f:
        h = f.read(0x200)
        mo, mc = struct.unpack('<II', h[0x10:0x18])
        f.seek(mo)
        return [struct.unpack('<QQQII', f.read(32)) for _ in range(mc)]


maps = []        # (addr, size, fileoff, path)
for p in sorted(glob.glob(main + '*')):
    if p.endswith(('.map', '.atlas', '.symbols')):
        continue
    for a, sz, fo, _, _ in mappings(p):
        maps.append((a, sz, fo, p))
base = min(m[0] for m in mappings(main))
with open(main, 'rb') as f:
    h = f.read(0x400)
cache_uuid = uuidm.UUID(bytes=h[0x58:0x68])
io, ic = struct.unpack('<II', h[0x1c0:0x1c8])


def read(addr, n):
    for a, sz, fo, p in maps:
        if a <= addr < a + sz:
            if p not in files:
                files[p] = open(p, 'rb')
            f = files[p]
            f.seek(fo + addr - a)
            return f.read(min(n, a + sz - addr))
    return None


with open(main, 'rb') as f:
    f.seek(io)
    raw = f.read(ic * 32)
    imgs = []
    for i in range(ic):
        addr, _, _, po, _ = struct.unpack('<QQQII', raw[i * 32:i * 32 + 32])
        f.seek(po)
        imgs.append((addr, f.read(512).split(b'\0')[0]))

ranges, uuids, blobs = [], [], []
for idx, (addr, path) in enumerate(imgs):
    hdr = read(addr, 32)
    if not hdr or struct.unpack('<I', hdr[:4])[0] != 0xfeedfacf:
        continue
    ncmds, sizeofcmds = struct.unpack('<II', hdr[16:24])
    lc = read(addr + 32, sizeofcmds)
    off, text_vm, text_sz, u = 0, None, None, None
    secs = []
    for _ in range(ncmds):
        cmd, cs = struct.unpack('<II', lc[off:off + 8])
        if cmd == 0x19:
            segname = lc[off + 8:off + 24].rstrip(b'\0')
            vm, vs = struct.unpack('<QQ', lc[off + 24:off + 40])
            nsect = struct.unpack('<I', lc[off + 64:off + 68])[0]
            if segname == b'__TEXT':
                text_vm, text_sz = vm, vs
            for s in range(nsect):
                so = off + 72 + 80 * s
                sn = lc[so:so + 16].rstrip(b'\0')
                sa, ss = struct.unpack('<QQ', lc[so + 32:so + 48])
                if sn in (b'__cstring', b'__oslogstring') and ss:
                    secs.append((sa, ss))
        elif cmd == 0x1b:
            u = lc[off + 8:off + 24]
        off += cs
    if text_vm is None or u is None:
        continue
    ui = len(uuids)
    uuids.append((text_vm - base, text_sz, u, path))
    for sa, ss in secs:
        data = read(sa, ss)
        if data:
            ranges.append((sa - base, len(data), ui))
            blobs.append(data)

nr, nu = len(ranges), len(uuids)
hdr_size = 16 + nr * 24 + nu * 32
data_off = hdr_size
range_recs, uuid_recs, body = [], [], bytearray()
for (voff, sz, ui), data in zip(ranges, blobs):
    range_recs.append(struct.pack('<QIIQ', voff, data_off + len(body), sz, ui))
    body += data
for toff, tsz, u, path in uuids:
    uuid_recs.append(struct.pack('<QI16sI', toff, tsz, u, data_off + len(body)))
    body += path + b'\0'
os.makedirs(os.path.join(out, 'dsc'), exist_ok=True)
dst = os.path.join(out, 'dsc', cache_uuid.hex.upper())
with open(dst, 'wb') as f:
    f.write(struct.pack('<IHHII', 0x64736368, 2, 0, nr, nu))
    f.write(b''.join(range_recs))
    f.write(b''.join(uuid_recs))
    f.write(body)
print(dst, cache_uuid, nr, 'ranges', nu, 'images', len(body) // (1 << 20), 'MiB')
