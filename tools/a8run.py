#!/usr/bin/env python3
"""Talk to gpu/src/sweet_run.c (launchd job com.sweet.run) from TWRP, which has no HFS+: write /sweet/run.cmd and
read /sweet/run.log straight through their 4 KiB blocks inside /data/sweet/root.img.

    a8run.py prepare DIR             make DIR/run.cmd (64 KiB) and DIR/run.log (1 MiB): every block carries a marker
    a8run.py map IMAGE MAP.json      find the marked blocks in a root image that has them installed (in WSL, local copy)
    a8run.py cmd MAP.json FILE       write FILE (up to 4000 bytes of commands) into run.cmd's first block on the phone
    a8run.py log MAP.json [OUT]      read run.log from the phone (to OUT or stdout)

The files are only ever overwritten in place (sweet-run uses pwrite inside the fixed size), so HFS+ keeps their blocks
where `map` found them. `cmd` checks the block's marker on the phone before writing it.
"""
import json
import os
import subprocess
import sys

B = 4096
CMD_BLOCKS, LOG_BLOCKS = 16, 256
MARK = b'SWEETRUN %s %04d\n'
ADB = os.environ.get('ADB', 'adb')
DEFAULT_CMD = b'''# sweet-run commands: tools/a8run.py cmd MAP.json FILE writes them on the phone
delay 30
timeout 120
/sweet/bin/mali_test 100
/sweet/bin/gl_test 1
'''


def block(kind, i, text=b''):
    """A block: text, NUL padding, the marker in the last 32 bytes (after a NUL, so sweet-run never reads it)."""
    mark = MARK % (kind.encode(), i)
    assert len(text) <= B - 64
    return text + b'\0' * (B - 32 - len(text)) + mark + b'\0' * (32 - len(mark))


def prepare(d):
    with open(os.path.join(d, 'run.cmd'), 'wb') as f:
        f.write(block('cmd', 0, DEFAULT_CMD) + b''.join(block('cmd', i) for i in range(1, CMD_BLOCKS)))
    with open(os.path.join(d, 'run.log'), 'wb') as f:
        f.write(b''.join(block('log', i) for i in range(LOG_BLOCKS)))


def find(image, out):
    want = {}
    for kind, count in (('cmd', CMD_BLOCKS), ('log', LOG_BLOCKS)):
        for i in range(count):
            want[MARK % (kind.encode(), i)] = (kind, i)
    found = {'cmd': [None] * CMD_BLOCKS, 'log': [None] * LOG_BLOCKS}
    with open(image, 'rb') as f:
        off = 0
        while True:
            chunk = f.read(256 << 20)
            if not chunk:
                break
            j = chunk.find(b'SWEETRUN ')
            while j != -1:
                if j % B == B - 32:
                    m = chunk[j:j + 18]
                    if m in want:
                        kind, i = want[m]
                        if found[kind][i] is not None:
                            sys.exit(f'{kind} block {i} found twice')
                        found[kind][i] = (off + j - (B - 32)) // B
                j = chunk.find(b'SWEETRUN ', j + 1)
            off += len(chunk)
    for kind in found:
        if None in found[kind]:
            sys.exit(f'{kind}: blocks {[i for i, b in enumerate(found[kind]) if b is None]} not found')
    with open(out, 'w') as f:
        json.dump(found, f)
    print(f'run.cmd blocks start {found["cmd"][0]}, run.log blocks {found["log"][0]}..{found["log"][-1]}')


def shell(script, binary=False):
    return subprocess.run([ADB, 'exec-out', script], check=True, capture_output=True).stdout


def read_blocks(blocks):
    data = b''
    for k in range(0, len(blocks), 64):
        data += shell('; '.join(f'dd if=/data/sweet/root.img bs={B} skip={b} count=1 2>/dev/null' for b in blocks[k:k + 64]))
    if len(data) != len(blocks) * B:
        sys.exit(f'read {len(data)} bytes, wanted {len(blocks) * B}')
    return data


def cmd(mapfile, src):
    blk = json.load(open(mapfile))['cmd'][0]
    text = open(src, 'rb').read().replace(b'\r\n', b'\n')
    if read_blocks([blk])[B - 32:B - 32 + 18] != MARK % (b'cmd', 0):
        sys.exit('run.cmd block 0 has no marker on the phone: wrong map or root.img changed')
    new = block('cmd', 0, text)
    tmp = 'run-cmd-block.bin'
    open(tmp, 'wb').write(new)
    subprocess.run([ADB, 'push', tmp, '/data/local/tmp/run-cmd-block.bin'], check=True, capture_output=True)
    os.remove(tmp)
    shell(f'dd if=/data/local/tmp/run-cmd-block.bin of=/data/sweet/root.img bs={B} seek={blk} count=1 conv=notrunc '
          f'2>/dev/null; sync; rm /data/local/tmp/run-cmd-block.bin')
    if read_blocks([blk]) != new:
        sys.exit('run.cmd block 0 reads back different')
    print(f'run.cmd: {len(text)} bytes written')


def log(mapfile, out=None):
    data = read_blocks(json.load(open(mapfile))['log'])
    text = data.split(b'\0', 1)[0]       # sweet-run clears the whole file (markers too), then writes from 0
    if out:
        open(out, 'wb').write(text)
    else:
        sys.stdout.buffer.write(text)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a[:1] == ['prepare'] and len(a) == 2:
        prepare(a[1])
    elif a[:1] == ['map'] and len(a) == 3:
        find(a[1], a[2])
    elif a[:1] == ['cmd'] and len(a) == 3:
        cmd(a[1], a[2])
    elif a[:1] == ['log'] and len(a) in (2, 3):
        log(*a[1:])
    else:
        sys.exit(__doc__)
