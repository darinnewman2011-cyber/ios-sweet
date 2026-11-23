#!/usr/bin/env python3
"""Turn an iOS root's unified log store into a .logarchive macOS `log show` accepts.

  a8-logarchive.py ROOT_MOUNT OUT.logarchive
then: /usr/bin/log show --archive OUT.logarchive --predicate 'process == "wifid"'

Copies private/var/db/diagnostics (Persist, Special, Signpost, HighVolume, timesync) and private/var/db/uuidtext,
then writes Info.plist (OSArchiveVersion 4, ArchiveIdentifier, EndTimeRef and per-store OldestTimeRef) from the
timesync boot records (0xbbb0: boot UUID @8, boot wall time ns @32) and sync records ('Ts': continuous @8, wall @16).
"""
import os, plistlib, shutil, struct, sys, uuid

root, out = sys.argv[1], sys.argv[2]
diag, uuidtext = os.path.join(root, 'private/var/db/diagnostics'), os.path.join(root, 'private/var/db/uuidtext')
if os.path.exists(out):
    shutil.rmtree(out)
shutil.copytree(diag, out, symlinks=True, ignore=shutil.ignore_patterns('*.log', '*.txt', '*.jsonl'))
for e in os.listdir(uuidtext):
    s = os.path.join(uuidtext, e)
    d = os.path.join(out, e)
    if os.path.isdir(s):
        shutil.copytree(s, d, dirs_exist_ok=True)
    else:
        shutil.copy2(s, d)

refs = []      # (boot uuid, continuous, wall ns)
ts = os.path.join(out, 'timesync')
for name in sorted(os.listdir(ts)):
    b = open(os.path.join(ts, name), 'rb').read()
    off, boot = 0, None
    while off + 8 <= len(b):
        sig = struct.unpack_from('<H', b, off)[0]
        if sig == 0xbbb0:
            boot = str(uuid.UUID(bytes=b[off + 8:off + 24])).upper()
            wall = struct.unpack_from('<q', b, off + 32)[0]
            refs.append((boot, 0, wall))
            off += 48
        elif b[off:off + 4] == b'Ts \x00':
            cont, wall = struct.unpack_from('<Qq', b, off + 8)
            if boot:
                refs.append((boot, cont, wall))
            off += 32
        else:
            off += 4
if not refs:
    sys.exit('no timesync records')

def ref(r):
    return {'UUID': r[0], 'ContinuousTime': r[1], 'WallTime': r[2]}

oldest, newest = refs[0], refs[-1]
info = {'OSArchiveVersion': 4, 'ArchiveIdentifier': str(uuid.uuid4()).upper(), 'EndTimeRef': ref(newest)}
for k in ('PersistMetadata', 'SpecialMetadata', 'SignpostMetadata', 'HighVolumeMetadata', 'LiveMetadata'):
    info[k] = {'OldestTimeRef': ref(oldest)}
plistlib.dump(info, open(os.path.join(out, 'Info.plist'), 'wb'))
print(f'{out}: {len(refs)} time refs, boots {len({r[0] for r in refs})}, last boot {newest[0]}')
