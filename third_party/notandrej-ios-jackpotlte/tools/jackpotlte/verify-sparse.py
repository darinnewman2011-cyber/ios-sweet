#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Verify an Android sparse image's payload against its raw source.

   verify_sparse.py RAW SPARSE

Walks the sparse chunks, checks the layout (every block accounted for) and
compares every data byte with the raw image at the offset it claims, without
writing a second copy of the image anywhere.
"""
import os
import sys

MAGIC = 0xED26FF3A
RAW, FILL, DONT_CARE, CRC32 = 0xCAC1, 0xCAC2, 0xCAC3, 0xCAC4
CHUNK = 1 << 20


def main(raw_path, sparse_path):
    raw_size = os.path.getsize(raw_path)
    raw = open(raw_path, "rb")
    sp = open(sparse_path, "rb")

    head = sp.read(28)
    if len(head) != 28:
        return "error: sparse file is shorter than its header"
    (magic, major, minor, hdr_sz, chunk_sz, blk_sz, total_blks, total_chunks,
     checksum) = __import__("struct").unpack("<IHHHHIIII", head)
    if magic != MAGIC:
        return "error: not an Android sparse image"
    if blk_sz != 4096:
        return "error: block size %d, expected 4096" % blk_sz
    if total_blks * blk_sz != raw_size:
        return ("error: %d blocks of %d = %d bytes, but the raw image is %d"
                % (total_blks, blk_sz, total_blks * blk_sz, raw_size))

    out = 0
    n = 0
    counted = 0
    buf = bytearray(CHUNK)
    while n < total_chunks:
        hdr = sp.read(12)
        if len(hdr) != 12:
            return "error: truncated at chunk %d of %d" % (n, total_chunks)
        ctype, _res, csz, tsz = __import__("struct").unpack("<HHII", hdr)
        if tsz < 12:
            return "error: chunk %d has a size of %d bytes" % (n, tsz)
        n += 1
        counted += csz
        left = tsz - 12
        if ctype == RAW:
            if left != csz * blk_sz:
                return "error: chunk %d is a raw chunk of %d bytes" % (n, left)
            while left:
                take = min(left, CHUNK)
                data = sp.read(take)
                if len(data) != take:
                    return "error: truncated in raw chunk %d" % n
                raw.seek(out)
                if raw.read(take) != data:
                    return "MISMATCH at byte %d (chunk %d)" % (out, n)
                out += take
                left -= take
        elif ctype in (FILL, DONT_CARE, CRC32):
            sp.seek(left, 1)
            if ctype == FILL:
                pass  # a fill chunk's value is not in the raw image's bytes
            out += csz * blk_sz
        else:
            return "error: unknown chunk type %#x at chunk %d" % (ctype, n)

    if out != raw_size:
        return "error: the chunks cover %d bytes, not %d" % (out, raw_size)
    if counted != total_blks:
        return "error: the chunks hold %d blocks, header says %d" % (counted, total_blks)
    return "OK: %d chunks, %d blocks, every data byte matches" % (total_chunks, total_blks)


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    print(main(sys.argv[1], sys.argv[2]))
