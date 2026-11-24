#!/usr/bin/env python3
"""Decode the WLBT firmware's module-4 debug records (xnu 0183 "fw 0004" + "fw hex" lines) in an A8 console log.

Usage: a8-fwtrace.py <console log> [symbols.dbg]

Record layout, worked out from captured joins (byte 0 high nibble = number of 16-bit words after the header):
  b1 == 0x01: FSM record, header 6 bytes; b3 0xd0 = signal (sender pid, ?, signal id, ?, receiver pid),
              b3 0xd1 = transition (pid, from state, to state, ?, triggering signal id)
  b1 == 0x09: trace record, header 5 bytes (module id, trace id, ?), args = 16-bit words
  b1 == 0x0d: header 4 bytes, args = 16-bit words
Names come from the firmware's own debug/wlan/symbols.dbg (signal ids, pids with state names, trace defs).
"""
import re
import sys

SYM = "/Users/darin/iOS/_tmp/vendor-fw/wifi/debug/wlan/symbols.dbg"


def load(path):
    sig, pids, faults, mods, traces = {}, {}, {}, {}, {}
    sect = None
    for line in open(path, encoding="latin-1"):
        line = line.rstrip("\n")
        m = re.match(r"^([a-z_]+) (\d+)$", line)
        if m:
            sect = m.group(1)
            continue
        if line.startswith("#") or not line.strip():
            continue
        p = line.split()
        try:
            k = int(p[0], 16)
        except ValueError:
            continue
        if sect == "signalid":
            sig[k] = p[1]
        elif sect == "pid":
            pids[k] = (p[1], p[3:])
        elif sect == "faultid":
            faults[k] = p[1]
        elif sect == "debug_ids":
            mods[k] = p[1]
        elif sect == "trace_def" and len(p) >= 4:
            try:
                traces[(k, int(p[1], 16))] = (p[2], p[4:])
            except ValueError:
                pass
    return sig, pids, faults, mods, traces


def main():
    sig, pids, faults, mods, traces = load(sys.argv[2] if len(sys.argv) > 2 else SYM)

    def pn(v):
        return pids[v][0] if v in pids else "%#06x" % v

    def st(pid, s):
        names = pids.get(pid, (None, []))[1]
        return names[s] if s < len(names) else str(s)

    def sn(v):
        return sig.get(v, "%#06x" % v)

    bufs, cur = [], None
    for l in open(sys.argv[1], encoding="latin-1"):
        if "SWEET A8 WIFI: fw 0004" in l:
            cur = []
            bufs.append(cur)
        elif cur is not None and "fw hex +" in l:
            cur.extend(int(x, 16) for x in l.split(":", 2)[2].split())
        elif "SWEET A8 WIFI:" in l and "fw hex" not in l:
            cur = None
            m = re.search(r"SWEET A8 WIFI: (connect request.*|join .*|firmware fault (0x[0-9a-f]+) arg (0x[0-9a-f]+).*)", l)
            if m:
                txt = m.group(1)
                if m.group(2):
                    txt += "  [%s]" % faults.get(int(m.group(2), 16), "?")
                bufs.append(("note", txt))
    for b in bufs:
        if isinstance(b, tuple):
            print("== " + b[1])
            continue
        i = 0
        while i + 2 <= len(b):
            h, t = b[i], b[i + 1]
            n = h >> 4
            hdr = 6 if t == 0x01 else 5 if t == 0x09 else 4
            L = hdr + 2 * n
            r = b[i:i + L]
            if len(r) < L:
                print("   (truncated)")
                break
            w = [r[hdr + 2 * k] | r[hdr + 2 * k + 1] << 8 for k in range(n)]
            if t == 0x01 and r[3] == 0xD1 and n >= 5:
                print("   %-22s %s -> %s  (on %s)" % (pn(w[0]), st(w[0], w[1]), st(w[0], w[2]), sn(w[4])))
            elif t == 0x01 and r[3] == 0xD0 and n >= 5:
                print("   %-22s sends %s to %s" % (pn(w[0]), sn(w[2]), pn(w[4])))
            elif t == 0x09:
                name, args = traces.get((r[2], r[3]), ("%s/%#x" % (mods.get(r[2], "%#x" % r[2]), r[3]), []))
                kv = ", ".join("%s=%#x" % (args[k] if k < len(args) else "a%d" % k, w[k]) for k in range(n))
                print("   trace %s(%s)" % (name, kv))
            else:
                print("   rec %02x %02x: %s" % (h, t, " ".join("%#06x" % x for x in w)))
            i += L


if __name__ == "__main__":
    main()
