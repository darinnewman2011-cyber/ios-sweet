#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Convert an Android sparse image to a raw image **with holes**.

Why this exists: /usr/bin/simg2img on this host writes the output densely
(every "don't care" and zero "fill" chunk becomes real zero bytes on disk).
The 16 GiB jackpotlte iOS root therefore needs a full 16 GiB of free space to
un-sparse, and this laptop's root filesystem does not have that. The original
raw only occupied ~11.65 GiB on disk because ~4.35 GiB of it was holes.

This writer seeks over don't-care chunks and zero fills instead of writing
them, so the result reads back as zeros (same as the original raw) but only
allocates the non-zero regions.

Format reference: "Android sparse image format" -- the file header is 28 bytes
(magic 0xED26FF3A, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz, total_blks,
total_chunks, image_checksum) and each chunk header is 12 bytes (chunk_type,
reserved, chunk_sz in blocks, total_sz in bytes including the header).

    ./unsparse-holey.py --check SPARSE          # report sizes, write nothing
    ./unsparse-holey.py SPARSE OUT

Verify the result with:  ls -ls OUT   (allocated must be < apparent size)
and a filesystem header read, e.g. bytes at 1024 are 'H+' for HFS+.
"""

import argparse
import os
import struct
import sys

MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_FILL = 0xCAC2
CHUNK_DONTCARE = 0xCAC3
CHUNK_CRC32 = 0xCAC4

HDR = struct.Struct("<IHHHHIIII")      # file header, 28 bytes
CHDR = struct.Struct("<HHII")          # chunk header, 12 bytes


def parse(sparse_path):
    """Yield (type, nbytes, payload_offset) for each chunk, plus the totals."""
    f = open(sparse_path, "rb")
    hdr = f.read(HDR.size)
    if len(hdr) < HDR.size:
        raise SystemExit("truncated sparse header")
    (magic, major, minor, file_hdr_sz, chunk_hdr_sz, blk_sz,
     total_blks, total_chunks, _checksum) = HDR.unpack(hdr)
    if magic != MAGIC:
        raise SystemExit(f"bad magic 0x{magic:08x} (want 0x{MAGIC:08x})")
    if file_hdr_sz != HDR.size or chunk_hdr_sz != CHDR.size:
        raise SystemExit(f"unexpected header sizes {file_hdr_sz}/{chunk_hdr_sz}")
    return f, blk_sz, total_blks, total_chunks


def run(sparse_path, out_path, check_only):
    f, blk_sz, total_blks, total_chunks = parse(sparse_path)
    f.seek(HDR.size)

    out = None if check_only else open(out_path, "wb")
    written = skipped = 0
    pos = 0

    def emit(data):
        nonlocal written, pos
        out.write(data)
        written += len(data)
        pos += len(data)

    def skip(n):
        nonlocal skipped, pos
        if out is not None:
            out.seek(pos + n)
        skipped += n
        pos += n

    for n in range(total_chunks):
        ch = f.read(CHDR.size)
        if len(ch) < CHDR.size:
            raise SystemExit(f"chunk {n}: truncated header")
        ctype, _res, chunk_sz, total_sz = CHDR.unpack(ch)
        nbytes = chunk_sz * blk_sz

        if ctype == CHUNK_RAW:
            if total_sz != nbytes + CHDR.size:
                raise SystemExit(f"chunk {n}: raw size {total_sz} != {nbytes + CHDR.size}")
            if check_only:
                f.seek(nbytes, os.SEEK_CUR)
                written += nbytes
                pos += nbytes
            else:
                left = nbytes
                while left:
                    buf = f.read(min(left, 8 << 20))
                    if not buf:
                        raise SystemExit(f"chunk {n}: truncated raw data")
                    emit(buf)
                    left -= len(buf)

        elif ctype == CHUNK_FILL:
            if total_sz != CHDR.size + 4:
                raise SystemExit(f"chunk {n}: fill size {total_sz}")
            val = f.read(4)
            if len(val) < 4:
                raise SystemExit(f"chunk {n}: truncated fill value")
            if val == b"\0\0\0\0":
                skip(nbytes)                       # hole
            elif check_only:
                written += nbytes
                pos += nbytes
            else:
                unit = val * (1 << 16)             # 256 KiB of the pattern
                left = nbytes
                while left:
                    piece = unit[:min(left, len(unit))]
                    emit(piece)
                    left -= len(piece)

        elif ctype == CHUNK_DONTCARE:
            if total_sz != CHDR.size:
                raise SystemExit(f"chunk {n}: dontcare size {total_sz}")
            skip(nbytes)

        elif ctype == CHUNK_CRC32:
            if total_sz != CHDR.size + 4:
                raise SystemExit(f"chunk {n}: crc size {total_sz}")
            f.seek(4, os.SEEK_CUR)

        else:
            raise SystemExit(f"chunk {n}: unknown type 0x{ctype:04x}")

    total = total_blks * blk_sz
    if pos != total:
        raise SystemExit(f"chunk sum {pos} != image size {total}")
    if out is not None:
        out.truncate(total)                        # size without allocating
        out.close()
        st = os.stat(out_path)
        print(f"wrote {out_path}")
        print(f"  apparent size : {total} bytes ({total / 2**30:.2f} GiB)")
        print(f"  allocated     : {st.st_blocks * 512} bytes "
              f"({st.st_blocks * 512 / 2**30:.2f} GiB)")
        print(f"  holes         : {skipped} bytes ({skipped / 2**30:.2f} GiB)")
    else:
        print(f"apparent size  : {total} bytes ({total / 2**30:.2f} GiB)")
        print(f"would write    : {written} bytes ({written / 2**30:.2f} GiB)")
        print(f"would skip     : {skipped} bytes ({skipped / 2**30:.2f} GiB)")
        print(f"free needed    : {written} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true", help="report sizes, write nothing")
    ap.add_argument("sparse")
    ap.add_argument("out", nargs="?")
    a = ap.parse_args()
    if not a.check and not a.out:
        ap.error("OUT is required unless --check")
    run(a.sparse, a.out, a.check)


if __name__ == "__main__":
    main()