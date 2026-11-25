#!/usr/bin/env python3
"""Add the A8 GPU userland and the CoreAnimation Metal 'b3a8' fix to an iOS root image (no kernel changes).

  install-gpu-root.py ROOT.img GPU_OUT_DIR [--dry-run]

GPU_OUT_DIR = a gpu/build.sh output folder (libsweet-gl-loader, libredwood-gl, libOSMesa.8, libredwoodair, RedwoodMetal +
RedwoodMetal.Info.plist, test tools); its deployment target must not be newer than the root's iOS.
Installs /sweet/lib/*.dylib, /sweet/bin/*, /System/Library/Extensions/RedwoodMetal.bundle. Then scans the root's dyld
shared cache for QuartzCore's four_cc_to_mtl_format 'b3a8' case:
    mov wA,#0x6138 ; movk wA,#0x6233,lsl #16   (the 'b3a8' constant)  ... within 8 instructions ...
    mov wB,#0x226                                (MTLPixelFormat 550)
    csel xB, xzr, xB, eq                         (zero it unless a MetalLimits bit is set)
and turns the csel into a nop, only if exactly one such site exists. Every change goes into PATCHES.txt next to the image.
The image is attached read-write without owners (no sudo); files added that way are plain readable files.
"""
import glob, os, shutil, struct, subprocess, sys, tempfile, time

img, gpu = sys.argv[1], sys.argv[2]
dry = '--dry-run' in sys.argv
NOP = bytes.fromhex('1f2003d5')


def find_sites(cache_dir):
    sites = []
    for p in sorted(glob.glob(os.path.join(cache_dir, 'dyld_shared_cache_arm64*'))):
        suf = p.split('dyld_shared_cache_arm64', 1)[1].lstrip('e')
        if suf and not suf[1:].isdigit():
            continue
        d = open(p, 'rb').read()
        # all 'mov wB,#0x226' (MOVZ 32-bit: 0x528044C0 | Rd)
        start = 0
        while True:
            i = d.find(b'\x44\x80\x52', start)        # bytes 1..3 of 0x528044Cx (little endian: Cx 44 80 52)
            if i < 0:
                break
            start = i + 1
            o = i - 1
            if o % 4 or (d[o] & 0xe0) != 0xc0:
                continue
            rb = d[o] & 0x1f
            # csel xB, xzr, xB, eq within the next 2 instructions: 0x9A800000 | Rm<<16 | cond(0)<<12 | Rn(31)<<5 | Rd
            want = 0x9A8003E0 | (rb << 16) | rb
            hit = None
            for k in (4, 8):
                w = struct.unpack_from('<I', d, o + k)[0]
                if w == want or (w == 0xD503201F and hit is None):   # csel, or our nop if already patched
                    hit = o + k
                    if w == want:
                        break
            if hit is None:
                continue
            # the 'b3a8' constant (movk w?,#0x6233,lsl #16 = 0x72AC4660 | Rd) within 8 instructions before
            win = d[max(0, o - 32):o + 32]
            ok = any((struct.unpack_from('<I', win, j)[0] & 0xffffffe0) == 0x72AC4660 for j in range(0, len(win) - 3, 4))
            if ok:
                sites.append((p, hit, d[hit:hit + 4]))
    return sites


mnt = tempfile.mkdtemp(prefix='gpuroot.')
subprocess.run(['hdiutil', 'attach', '-nobrowse', '-noverify', '-mountpoint', mnt, img], check=True, stdout=subprocess.DEVNULL)
log = []
try:
    lib = os.path.join(mnt, 'sweet/lib'); binp = os.path.join(mnt, 'sweet/bin')
    bundle = os.path.join(mnt, 'System/Library/Extensions/RedwoodMetal.bundle')
    if not dry:
        for d in (lib, binp, bundle):
            os.makedirs(d, exist_ok=True)
        for f in ('libsweet-gl-loader.dylib', 'libredwood-gl.dylib', 'libOSMesa.8.dylib', 'libredwoodair.dylib'):
            shutil.copy2(os.path.join(gpu, f), lib); os.chmod(os.path.join(lib, f), 0o755)
        for f in ('metal_test', 'metal_props', 'gl_test', 'sweet-dmesg', 'mali_test'):
            if os.path.exists(os.path.join(gpu, f)):
                shutil.copy2(os.path.join(gpu, f), binp); os.chmod(os.path.join(binp, f), 0o755)
        shutil.copy2(os.path.join(gpu, 'RedwoodMetal'), os.path.join(bundle, 'RedwoodMetal'))
        os.chmod(os.path.join(bundle, 'RedwoodMetal'), 0o755)
        shutil.copy2(os.path.join(gpu, 'RedwoodMetal.Info.plist'), os.path.join(bundle, 'Info.plist'))
        log.append(f'GPU userland from {gpu}: /sweet/lib (4 dylibs), /sweet/bin tools, RedwoodMetal.bundle')
    sites = find_sites(os.path.join(mnt, 'System/Library/Caches/com.apple.dyld'))
    for p, off, cur in sites:
        print(f'  b3a8 site: {os.path.basename(p)} @0x{off:x} = {cur.hex()}')
    if len(sites) == 1:
        p, off, cur = sites[0]
        if cur == NOP:
            log.append(f'b3a8 Metal fix already present: {os.path.basename(p)} @0x{off:x}')
        elif not dry:
            with open(p, 'r+b') as f:
                f.seek(off); f.write(NOP)
            log.append(f'b3a8 Metal fix: {os.path.basename(p)} @0x{off:x} {cur.hex()} -> 1f2003d5 (QuartzCore four_cc_to_mtl_format)')
    else:
        log.append(f'b3a8 Metal fix NOT applied: {len(sites)} candidate sites (need exactly 1)')
finally:
    subprocess.run(['sync'])
    subprocess.run(['hdiutil', 'detach', mnt], stdout=subprocess.DEVNULL)
    os.rmdir(mnt)
stamp = time.strftime('%Y-%m-%d %H:%M')
if not dry:
    with open(os.path.join(os.path.dirname(os.path.abspath(img)), 'PATCHES.txt'), 'a') as f:
        for l in log:
            f.write(f'{stamp}  {l}\n')
for l in log:
    print(l)
