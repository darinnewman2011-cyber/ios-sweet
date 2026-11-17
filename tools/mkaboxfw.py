#!/usr/bin/env python3
"""mkaboxfw.py DIR OUT: pack the Galaxy A8's ABOX (Calliope) firmware for the kernel (mkpayload --blob SweetABOX=OUT).

DIR holds the files from the phone's own vendor partition (/vendor/firmware): calliope_sram.bin, calliope_dram.bin and
the stock DT's ext_bin files. They are Samsung's and never go into git.

Layout (little endian): 'ABXF', u32 count, then count x {u32 area (0 SRAM, 1 DRAM), u32 offset, u32 size, u32 data},
then the data. Areas/offsets: calliope_*.bin at 0; ext_bin@0..2 as the stock DT (abox node) places them.
"""
import os, struct, sys

PARTS = [  # (file, area, offset)
    ('calliope_sram.bin', 0, 0),
    ('calliope_dram.bin', 1, 0),
    ('tfadsp.bin', 1, 0x502000),
    ('AP_AUDIO_SLSI.bin', 1, 0x7f0000),
    ('APBargeIn_AUDIO_SLSI.bin', 1, 0x7ec000),
]

def main(d, out):
    blobs = [(open(os.path.join(d, f), 'rb').read(), a, o) for f, a, o in PARTS]
    hdr = 8 + 16 * len(blobs)
    table, data = b'', b''
    for b, a, o in blobs:
        table += struct.pack('<4I', a, o, len(b), hdr + len(data))
        data += b + b'\0' * (-len(b) & 15)
    open(out, 'wb').write(b'ABXF' + struct.pack('<I', len(blobs)) + table + data)
    print(out, hdr + len(data), 'bytes,', len(blobs), 'parts')

if __name__ == '__main__':
    main(*sys.argv[1:3])
