#!/usr/bin/env python3
"""Decode the WLBT firmware's mxlog records from an A8 console log (xnu 0182 "SWEET A8 MXLOG:" lines).

Usage: a8-mxlog.py <console log> [log-strings.bin]

Record layout (Samsung mxlog.c, phase 5): header word = sync 0x9669 << 16 | level << 8 | n,
then n words: timestamp, offset of the format string in debug/common/log-strings.bin, args.
A %s argument is itself an offset into log-strings.bin.
"""
import re
import sys

LEVELS = ["ERROR", "WARN", "MAJOR", "MINOR", "DETAIL"]
CONV = re.compile(r"%([-+ #0]*)(\d+|\*)?(?:\.(\d+))?(hh|h|ll|l|z|j|t)?([diouxXcsp%])")


def cstr(blob, off):
    if off >= len(blob):
        return "<bad offset %#x>" % off
    end = blob.find(b"\0", off)
    return blob[off:end if end >= 0 else len(blob)].decode("latin-1")


def fmt(blob, f, args):
    out, ai = [], 0
    pos = 0
    for m in CONV.finditer(f):
        out.append(f[pos:m.start()])
        pos = m.end()
        flags, width, prec, length, conv = m.groups()
        if conv == "%":
            out.append("%")
            continue
        a = args[ai] if ai < len(args) else 0
        ai += 1
        spec = "%" + (flags or "") + (width if width and width != "*" else "") + ("." + prec if prec else "")
        if conv == "s":
            out.append((spec + "s") % cstr(blob, a))
        elif conv in "di":
            out.append((spec + "d") % (a - (1 << 32) if a & 0x80000000 else a))
        elif conv == "c":
            out.append(chr(a & 0xFF))
        elif conv == "p":
            out.append("%#x" % a)
        else:
            out.append((spec + conv) % a)
    out.append(f[pos:])
    return "".join(out)


def main():
    log = open(sys.argv[1], "rb").read().decode("latin-1")
    sp = sys.argv[2] if len(sys.argv) > 2 else __file__.rsplit("/", 2)[0] + "/../_tmp/vendor-fw/wifi/debug/common/log-strings.bin"
    try:
        blob = open(sp, "rb").read()
    except OSError:
        blob = open("/Users/darin/iOS/_tmp/vendor-fw/wifi/debug/common/log-strings.bin", "rb").read()
    words = []
    for m in re.finditer(r"SWEET A8 MXLOG: ((?:[0-9a-f]{8} ){8})/(\d+)", log):
        ws = [int(x, 16) for x in m.group(1).split()]
        words.extend(ws[: int(m.group(2))])
    i = 0
    while i < len(words):
        h = words[i]
        if h >> 16 != 0x9669:
            i += 1
            continue
        lvl, n = (h >> 8) & 0xFF, h & 0xFF
        rec = words[i + 1 : i + 1 + n]
        i += 1 + n
        if len(rec) < 2:
            continue
        ts, off, args = rec[0], rec[1], rec[2:]
        print("%10u %-6s %s" % (ts, LEVELS[lvl] if lvl < len(LEVELS) else lvl, fmt(blob, cstr(blob, off), args).rstrip()))


if __name__ == "__main__":
    main()
