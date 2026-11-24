#!/usr/bin/env python3
"""
a8xlate - ahead-of-time arm64e -> ARMv8.0 code translator for the Galaxy A8 (Cortex-A53/A73) iOS port.

It contains no Apple code: it reads the user's own files (Mach-O binaries, or a dyld shared cache and its
subcaches), rewrites the instructions an ARMv8.0 core cannot execute, and writes the result to a copy.

Commands
  a8xlate.py scan  PATH...                       count untranslatable instructions by kind (read-only)
  a8xlate.py translate SRC OUT [--jobs N] [--cache DIR]
                                                 copy SRC (a file or a directory) to OUT, rewrite OUT in place
  a8xlate.py selftest                            check every rewrite against hand-encoded instructions

Stage 1 (this file) does every rewrite that fits in place, 1:1:
  RETAA/RETAB                      -> RET
  BRAA/BRAB/BRAAZ/BRABZ            -> BR  Xn
  BLRAA/BLRAB/BLRAAZ/BLRABZ        -> BLR Xn
  LDRAA/LDRAB                      -> LDR (unsigned offset) / LDUR / LDR pre-index, when the offset fits
  PAC*/AUT* register forms, XPAC*  -> NOP   (pointers simply stay unsigned, consistently everywhere)
  PACGA Xd,Xn,Xm                   -> EOR Xd,Xn,Xm (any deterministic value works as long as it's consistent)
  LDAPR/LDAPRB/LDAPRH (RCpc)       -> LDAR/LDARB/LDARH (stronger ordering, same result)
Hint-space PAC (PACIASP, AUTIBSP, BTI, ...) already executes as NOP on ARMv8.0 and is left alone.

Reported for stage 2 (out-of-line helper routines, not rewritten here):
  LSE atomics (CAS/CASP/LDADD.../SWP), LDAPUR, FJCVTZS, SDOT/UDOT (FP16 arithmetic detection: TODO),
  and LDRAA/LDRAB whose offsets don't fit a plain load.

Translation is parallel (default 6 worker processes) and cached: each code range's bytes are hashed, and the
patch list for that hash is stored in the cache directory, so unchanged code is never translated twice.
Rewriting code invalidates code signatures; the A8 kernel does not enforce them (or re-sign with ldid).
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import mmap
import os
import shutil
import struct
import sys

VERSION = 1
NOP = 0xD503201F

# ----------------------------------------------------------------------------------------------------------
# instruction classifier / rewriter
# ----------------------------------------------------------------------------------------------------------


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


def rewrite(w):
    """Return (kind, new_word_or_None). kind is None for ordinary ARMv8.0 instructions."""
    # --- pointer authentication, branches / returns
    if w in (0xD65F0BFF, 0xD65F0FFF):                       # RETAA, RETAB
        return "pac-ret", 0xD65F03C0
    if (w & 0xFFFFF800) == 0xD71F0800:                      # BRAA/BRAB Xn, Xm
        return "pac-br", 0xD61F0000 | (w & 0x3E0)
    if (w & 0xFFFFF800) == 0xD73F0800:                      # BLRAA/BLRAB Xn, Xm
        return "pac-blr", 0xD63F0000 | (w & 0x3E0)
    if (w & 0xFFFFF81F) == 0xD61F081F:                      # BRAAZ/BRABZ Xn
        return "pac-br", 0xD61F0000 | (w & 0x3E0)
    if (w & 0xFFFFF81F) == 0xD63F081F:                      # BLRAAZ/BLRABZ Xn
        return "pac-blr", 0xD63F0000 | (w & 0x3E0)
    if w in (0xD69F0BFF, 0xD69F0FFF):                       # ERETAA/ERETAB (kernel only)
        return "pac-eret", 0xD69F03E0
    # --- pointer authentication, data processing (register forms, incl. Z variants) and strips
    if (w & 0xFFFFC000) == 0xDAC10000:                      # PACIA..AUTDZB
        return "pac-data", NOP
    if (w & 0xFFFFFBE0) == 0xDAC143E0:                      # XPACI / XPACD
        return "pac-data", NOP
    if (w & 0xFFE0FC00) == 0x9AC03000:                      # PACGA Xd, Xn, Xm|SP
        rm = (w >> 16) & 31
        if rm == 31:
            return "pacga-sp", None
        return "pac-data", 0xCA000000 | (w & 0x1F03FF)       # EOR Xd, Xn, Xm
    # --- LDRAA / LDRAB
    if (w & 0xFF200400) == 0xF8200400:
        rt, rn = w & 31, (w >> 5) & 31
        off = sext(((w >> 22) & 1) << 9 | ((w >> 12) & 0x1FF), 10) * 8
        if (w >> 11) & 1:                                   # pre-index with writeback
            if -256 <= off <= 255:
                return "pac-ldr", 0xF8400C00 | ((off & 0x1FF) << 12) | (rn << 5) | rt
            return "pac-ldr-far", None
        if 0 <= off <= 32760:
            return "pac-ldr", 0xF9400000 | ((off // 8) << 10) | (rn << 5) | rt
        if -256 <= off < 0:
            return "pac-ldr", 0xF8400000 | ((off & 0x1FF) << 12) | (rn << 5) | rt
        return "pac-ldr-far", None
    # --- RCpc loads (ARMv8.3): LDAPR{B,H} -> LDAR{B,H}; this sits inside the LSE encoding space, so test first
    if (w & 0x3FFFFC00) == 0x38BFC000:
        size = w >> 30
        return "rcpc", (size << 30) | 0x08DFFC00 | (w & 0x3FF)
    # --- LSE atomics (ARMv8.1): need LDXR/STXR loops -> stage 2
    if (w & 0x3FA07C00) == 0x08A07C00:                      # CAS{A,L,AL}{B,H}
        return "lse-cas", None
    if (w & 0xBFA07C00) == 0x08207C00:                      # CASP{A,L,AL}
        return "lse-casp", None
    if (w & 0x3F200C00) == 0x38200000:                      # LDADD/LDCLR/LDEOR/LDSET/LDSMAX.../SWP (+ ST aliases)
        return "lse-amo", None
    # --- other post-v8.0 instructions -> stage 2
    if (w & 0x3F200C00) == 0x19000000:                      # LDAPUR / STLUR family (ARMv8.4)
        return "rcpc2", None
    if (w & 0xFFFFFC00) == 0x1E7E0000:                      # FJCVTZS
        return "fjcvtzs", None
    if (w & 0xBFE0FC00) == 0x0E809400 or (w & 0xBFC0F400) == 0x0F80E000:   # SDOT/UDOT (vector, by element)
        return "dotprod", None
    return None, None


def translate_words(buf):
    """buf: bytes of code. Returns (patches [(byte_offset, new_word)], counts {kind: n})."""
    patches, counts = [], {}
    n = len(buf) // 4
    words = struct.unpack_from("<%dI" % n, buf)
    for i, w in enumerate(words):
        kind, new = rewrite(w)
        if kind is None:
            continue
        counts[kind] = counts.get(kind, 0) + 1
        if new is not None and new != w:
            patches.append((i * 4, new))
    return patches, counts


# ----------------------------------------------------------------------------------------------------------
# file formats: Mach-O (thin / fat) and dyld shared caches (with subcache files)
# ----------------------------------------------------------------------------------------------------------
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
CPU_TYPE_ARM64 = 0x0100000C
S_ATTR_PURE_INSTRUCTIONS = 0x80000000
S_ATTR_SOME_INSTRUCTIONS = 0x00000400


def macho_code_ranges(data, base=0):
    """Code sections of a 64-bit Mach-O starting at data[base:]. Returns [(file_offset, size, name)]."""
    if struct.unpack_from("<I", data, base)[0] != MH_MAGIC_64:
        return []
    cputype, cpusub, ftype, ncmds, sizeofcmds = struct.unpack_from("<iiIII", data, base + 4)
    if cputype != CPU_TYPE_ARM64:
        return []
    out, off = [], base + 32
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", data, off)
        if cmd == 0x19:                                     # LC_SEGMENT_64
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            s = off + 72
            for _ in range(nsects):
                sect = data[s:s + 16].split(b"\0")[0].decode("latin-1")
                seg = data[s + 16:s + 32].split(b"\0")[0].decode("latin-1")
                addr, size, foff = struct.unpack_from("<QQI", data, s + 32)
                flags = struct.unpack_from("<I", data, s + 64)[0]
                if flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS) and size and foff:
                    out.append((base + foff, size & ~3, "%s,%s" % (seg, sect), addr))
                s += 80
        off += cmdsize
    return out


def file_code_ranges(path):
    """[(path, file_offset, size, label)] for a Mach-O or fat file; [] if neither."""
    with open(path, "rb") as f:
        head = f.read(8)
        if len(head) < 8:
            return []
        magic_be = struct.unpack(">I", head[:4])[0]
        magic_le = struct.unpack("<I", head[:4])[0]
        if magic_le != MH_MAGIC_64 and magic_be != FAT_MAGIC:
            return []
        f.seek(0)
        data = f.read()
    ranges = []
    if magic_be == FAT_MAGIC:
        n = struct.unpack_from(">I", data, 4)[0]
        for k in range(n):
            ctype, csub, foff, size, align = struct.unpack_from(">iiIII", data, 8 + 20 * k)
            if ctype == CPU_TYPE_ARM64:
                ranges += [(path, o, s, "%s %s" % (os.path.basename(path), lab))
                           for o, s, lab, _ in macho_code_ranges(data, foff)]
    else:
        ranges = [(path, o, s, "%s %s" % (os.path.basename(path), lab)) for o, s, lab, _ in macho_code_ranges(data)]
    return ranges


def dsc_files(path):
    """The main cache and its subcaches for a dyld_shared_cache_* main file."""
    d, b = os.path.split(path)
    fs = [os.path.join(d, x) for x in sorted(os.listdir(d))
          if x == b or (x.startswith(b + ".") and not x.endswith((".map", ".symbols", ".atlas")))]
    return fs


def dsc_code_ranges(main):
    """Code sections of every image in a dyld shared cache (iOS 15+ layout with subcache files)."""
    files = dsc_files(main)
    amap = []                                               # (address, size, file, file_offset)
    for f in files:
        with open(f, "rb") as fh:
            h = fh.read(0x200)
        if not h.startswith(b"dyld_v1"):
            continue
        moff, mcnt = struct.unpack_from("<II", h, 0x10)
        with open(f, "rb") as fh:
            fh.seek(moff)
            m = fh.read(32 * mcnt)
        for k in range(mcnt):
            addr, size, foff, maxp, initp = struct.unpack_from("<QQQII", m, 32 * k)
            amap.append((addr, size, f, foff))

    def a2f(a):
        for addr, size, f, foff in amap:
            if addr <= a < addr + size:
                return f, foff + (a - addr)
        return None, None

    with open(main, "rb") as fh:
        h = fh.read(0x200)
    moff = struct.unpack_from("<I", h, 0x10)[0]
    if moff >= 0x1C8:
        ioff, icnt = struct.unpack_from("<II", h, 0x1C0)
    else:
        ioff, icnt = struct.unpack_from("<II", h, 0x18)
    with open(main, "rb") as fh:
        mdata = fh.read()
    ranges, opened = [], {}
    for k in range(icnt):
        addr, mtime, inode, poff, pad = struct.unpack_from("<QQQII", mdata, ioff + 32 * k)
        name = mdata[poff:poff + 256].split(b"\0")[0].decode("latin-1").rsplit("/", 1)[-1]
        f, foff = a2f(addr)
        if f is None:
            continue
        if f not in opened:
            with open(f, "rb") as fh:
                opened[f] = fh.read()
        data = opened[f]
        # in a cache, section offsets are not file offsets: map each section's address instead
        for o, size, lab, saddr in macho_code_ranges(data, foff):
            sf, sfo = a2f(saddr)
            if sf is not None:
                ranges.append((sf, sfo, size, "%s %s" % (name, lab)))
    return ranges


def relabel_arm64(path):
    """arm64e -> arm64 in Mach-O and fat headers (the A8 kernel is a plain arm64 build and won't exec arm64e).
    Returns the number of headers changed. CPU_SUBTYPE_ARM64E is 2, with the PTRAUTH ABI flag in the top bits."""
    n = 0
    with open(path, "r+b") as f:
        head = f.read(8)
        if len(head) < 8:
            return 0
        if struct.unpack(">I", head[:4])[0] == FAT_MAGIC:
            cnt = struct.unpack(">I", head[4:8])[0]
            for k in range(cnt):
                f.seek(8 + 20 * k)
                ctype, csub, foff = struct.unpack(">iII", f.read(12))
                if ctype == CPU_TYPE_ARM64 and (csub & 0xFF) == 2:
                    f.seek(8 + 20 * k + 4)
                    f.write(struct.pack(">I", 0))
                    n += 1
                if ctype == CPU_TYPE_ARM64:
                    n += _relabel_thin(f, foff)
        else:
            n += _relabel_thin(f, 0)
    return n


def _relabel_thin(f, base):
    f.seek(base)
    h = f.read(12)
    if len(h) < 12 or struct.unpack("<I", h[:4])[0] != MH_MAGIC_64:
        return 0
    ctype, csub = struct.unpack("<iI", h[4:12])
    if ctype == CPU_TYPE_ARM64 and (csub & 0xFF) == 2:
        f.seek(base + 8)
        f.write(struct.pack("<I", 0))
        return 1
    return 0


def collect_ranges(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, dirs, files in os.walk(p):
                for fn in sorted(files):
                    fp = os.path.join(root, fn)
                    if os.path.islink(fp) or not os.path.isfile(fp):
                        continue
                    if fn.startswith("dyld_shared_cache_") and "." not in fn:
                        out += dsc_code_ranges(fp)
                    elif not fn.startswith("dyld_shared_cache_"):
                        try:
                            out += file_code_ranges(fp)
                        except (OSError, struct.error):
                            pass
        elif os.path.basename(p).startswith("dyld_shared_cache_"):
            out += dsc_code_ranges(p)
        else:
            out += file_code_ranges(p)
    return out


# ----------------------------------------------------------------------------------------------------------
# parallel work + cache
# ----------------------------------------------------------------------------------------------------------
CHUNK = 4 << 20


def split(ranges):
    for f, o, s, lab in ranges:
        p = 0
        while p < s:
            n = min(CHUNK, s - p)
            yield (f, o + p, n, lab)
            p += n


def work(job):
    f, off, size, lab, cache, write = job
    with open(f, "r+b" if write else "rb") as fh:
        mm = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_WRITE if write else mmap.ACCESS_READ)
        buf = mm[off:off + size]
        key = hashlib.sha256(b"a8xlate%d" % VERSION + buf).hexdigest()
        cp = os.path.join(cache, key[:2], key + ".json") if cache else None
        hit = False
        if cp and os.path.exists(cp):
            with open(cp) as c:
                d = json.load(c)
            patches, counts, hit = d["p"], d["c"], True
        else:
            patches, counts = translate_words(buf)
            if cp:
                os.makedirs(os.path.dirname(cp), exist_ok=True)
                tmp = cp + ".%d" % os.getpid()
                with open(tmp, "w") as c:
                    json.dump({"p": patches, "c": counts}, c)
                os.replace(tmp, cp)
        if write:
            for o, w in patches:
                struct.pack_into("<I", mm, off + o, w)
            mm.flush()
        mm.close()
    return lab, size, counts, len(patches), hit


def run(ranges, jobs, cache, write):
    total, totals, patched, hits, nbytes = 0, {}, 0, 0, 0
    work_items = [(f, o, s, lab, cache, write) for f, o, s, lab in split(ranges)]
    with cf.ProcessPoolExecutor(max_workers=jobs) as ex:
        for i, (lab, size, counts, npatch, hit) in enumerate(ex.map(work, work_items, chunksize=4)):
            nbytes += size
            patched += npatch
            hits += hit
            for k, v in counts.items():
                totals[k] = totals.get(k, 0) + v
            if (i + 1) % 200 == 0:
                print("  %d/%d chunks, %.0f MB of code" % (i + 1, len(work_items), nbytes / 1e6), file=sys.stderr)
    return work_items, nbytes, totals, patched, hits


def report(nbytes, totals, patched, hits, chunks):
    ins = nbytes // 4
    stage1 = sum(v for k, v in totals.items() if k in ("pac-ret", "pac-br", "pac-blr", "pac-eret", "pac-data",
                                                        "pac-ldr", "rcpc"))
    stage2 = sum(totals.values()) - stage1
    print("code: %.1f MB, %d instructions in %d chunks (%d from cache)" % (nbytes / 1e6, ins, chunks, hits))
    for k in sorted(totals, key=lambda k: -totals[k]):
        print("  %-12s %10d  %6.3f%%" % (k, totals[k], 100.0 * totals[k] / max(ins, 1)))
    print("in place (stage 1): %d (%.2f%%)   rewritten: %d" % (stage1, 100.0 * stage1 / max(ins, 1), patched))
    print("needs helpers (stage 2): %d (%.3f%%)" % (stage2, 100.0 * stage2 / max(ins, 1)))


# ----------------------------------------------------------------------------------------------------------
# self-test
# ----------------------------------------------------------------------------------------------------------
def selftest():
    cases = [
        (0xD65F0FFF, "pac-ret", 0xD65F03C0),               # retab -> ret
        (0xD65F0BFF, "pac-ret", 0xD65F03C0),               # retaa -> ret
        (0xD71F0A11, "pac-br", 0xD61F0200),                # braa x16, x17 -> br x16
        (0xD73F0D01, "pac-blr", 0xD63F0100),               # blrab x8, x1 -> blr x8
        (0xD63F091F, "pac-blr", 0xD63F0100),               # blraaz x8 -> blr x8
        (0xD61F0E1F, "pac-br", 0xD61F0200),                # brabz x16 -> br x16
        (0xDAC10230, "pac-data", NOP),                     # pacia x16, x17
        (0xDAC11E11, "pac-data", NOP),                     # autdb x17, x16
        (0xDAC123F0, "pac-data", NOP),                     # paciza x16
        (0xDAC143F0, "pac-data", NOP),                     # xpaci x16
        (0x9AC23020, "pac-data", 0xCA020020),              # pacga x0, x1, x2 -> eor x0, x1, x2
        (0xF8201430, "pac-ldr", 0xF9400430),               # ldraa x16, [x1, #8] -> ldr x16, [x1, #8]
        (0xF87FF430, "pac-ldr", 0xF85F8030),               # ldraa x16, [x1, #-8] -> ldur x16, [x1, #-8]
        (0xF8201C30, "pac-ldr", 0xF8408C30),               # ldraa x16, [x1, #8]! -> ldr x16, [x1, #8]!
        (0xF8BFC020, "rcpc", 0xC8DFFC20),                  # ldapr x0, [x1] -> ldar x0, [x1]
        (0x38BFC020, "rcpc", 0x08DFFC20),                  # ldaprb w0, [x1] -> ldarb w0, [x1]
        (0xC8E0FC41, "lse-cas", None),                     # casal x0, x1, [x2]
        (0xF8E10040, "lse-amo", None),                     # ldaddal x1, x0, [x2]
        (0xB8A08020, "lse-amo", None),                     # swpa w0, w0, [x1]
        (0x1E7E0020, "fjcvtzs", None),                     # fjcvtzs w0, d1
        (0x4E829420, "dotprod", None),                     # sdot v0.4s, v1.16b, v2.16b
        (0xD503233F, None, None),                          # paciasp (hint space: already a NOP on v8.0)
        (0xD50323BF, None, None),                          # autiasp
        (0xD65F03C0, None, None),                          # ret
        (0xF9400420, None, None),                          # ldr x0, [x1, #8]
        (0xC85FFC20, None, None),                          # ldaxr x0, [x1]
    ]
    bad = 0
    for w, kind, new in cases:
        k, n = rewrite(w)
        ok = k == kind and n == new
        bad += not ok
        print("%s %08x -> %-10s %s" % ("ok " if ok else "BAD", w, k, ("%08x" % n) if n is not None else "-"))
    print("selftest: %d/%d ok" % (len(cases) - bad, len(cases)))
    return bad == 0


# ----------------------------------------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="arm64e -> ARMv8.0 ahead-of-time translator (stage 1)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("paths", nargs="+")
    s.add_argument("--jobs", type=int, default=6)
    t = sub.add_parser("translate")
    t.add_argument("src")
    t.add_argument("out")
    t.add_argument("--jobs", type=int, default=6)
    t.add_argument("--cache", default=os.path.expanduser("~/.a8xlate-cache"))
    t.add_argument("--keep-arm64e", action="store_true", help="don't relabel headers as plain arm64")
    ip = sub.add_parser("inplace", help="translate a directory (e.g. a mounted root image) in place")
    ip.add_argument("dir")
    ip.add_argument("--jobs", type=int, default=6)
    ip.add_argument("--cache", default=os.path.expanduser("~/.a8xlate-cache"))
    ip.add_argument("--keep-arm64e", action="store_true")
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        sys.exit(0 if selftest() else 1)
    if a.cmd == "scan":
        ranges = collect_ranges(a.paths)
        chunks, nbytes, totals, patched, hits = run(ranges, a.jobs, None, False)
        report(nbytes, totals, patched, hits, len(chunks))
        return
    if a.cmd == "inplace":
        ranges = collect_ranges([a.dir])
        chunks, nbytes, totals, patched, hits = run(ranges, a.jobs, a.cache, True)
        report(nbytes, totals, patched, hits, len(chunks))
        if not a.keep_arm64e:
            files = sorted(set(f for f, o, s, lab in ranges))
            n = sum(relabel_arm64(f) for f in files)
            print("relabelled arm64e -> arm64: %d headers in %d files" % (n, len(files)))
        return
    if os.path.exists(a.out):
        sys.exit("output %s exists; refusing to overwrite" % a.out)
    if os.path.isdir(a.src):
        shutil.copytree(a.src, a.out, symlinks=True, copy_function=lambda s, d: os.system("cp -c '%s' '%s'" % (s, d)) == 0 or shutil.copy2(s, d))
    else:
        if os.system("cp -c '%s' '%s'" % (a.src, a.out)) != 0:
            shutil.copy2(a.src, a.out)
    if os.path.isdir(a.src):
        ranges = collect_ranges([a.out])
    elif os.path.basename(a.src).startswith("dyld_shared_cache_"):
        for f in dsc_files(a.src)[1:]:
            dst = os.path.join(os.path.dirname(a.out), os.path.basename(a.out) + f[len(a.src):])
            if os.system("cp -c '%s' '%s'" % (f, dst)) != 0:
                shutil.copy2(f, dst)
        ranges = dsc_code_ranges(a.out)
    else:
        ranges = file_code_ranges(a.out)
    chunks, nbytes, totals, patched, hits = run(ranges, a.jobs, a.cache, True)
    report(nbytes, totals, patched, hits, len(chunks))
    if not a.keep_arm64e:
        files = sorted(set(f for f, o, s, lab in ranges))
        relabeled = 0
        for f in files:
            try:
                relabeled += relabel_arm64(f)
            except OSError:
                pass
        print("relabelled arm64e -> arm64: %d headers in %d files" % (relabeled, len(files)))


if __name__ == "__main__":
    main()
