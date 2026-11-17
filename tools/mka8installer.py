#!/usr/bin/env python3
"""Build the Galaxy A8 TWRP system installer and lay out a USB drive for it.

  mka8installer.py zip OUT.zip
      the installer zip (flash it from TWRP: Install > zip). It reads the system from the USB drive.
  mka8installer.py drive DIR NAME ROOT.img BOOT.img INODE
      writes DIR/install.txt for ROOT.img + BOOT.img (already copied into DIR, the drive's A8-iOS folder) after
      hashing both. INODE is the sweetrootino= baked into BOOT.img.

The installer (tools/a8-installer/update-binary) writes ROOT in place over /data/sweet/root.img (same inode and
blocks, so the boot image's sweetrootino stays right) and BOOT to mmcblk0p12, verifying both. Volume keys choose.
"""
import hashlib, os, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                return h.hexdigest()
            h.update(b)


def make_zip(out):
    script = open(os.path.join(HERE, 'a8-installer', 'update-binary'), 'rb').read().replace(b'\r\n', b'\n')
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('META-INF/com/google/android/update-binary', script)
        z.writestr('META-INF/com/google/android/updater-script', '#MAGIC\n')
    print(out, os.path.getsize(out), 'bytes')


def make_drive(d, name, root, boot, inode):
    for f in (root, boot):
        if os.path.dirname(os.path.abspath(f)) != os.path.abspath(d):
            sys.exit(f'{f} must already be in {d}')
    txt = (f'NAME={name}\nROOT={os.path.basename(root)}\nROOT_SHA={sha256(root)}\n'
           f'BOOT={os.path.basename(boot)}\nBOOT_SHA={sha256(boot)}\nINODE={inode}\n')
    open(os.path.join(d, 'install.txt'), 'w', newline='\n').write(txt)
    print(txt, end='')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'zip':
        make_zip(sys.argv[2])
    elif len(sys.argv) == 7 and sys.argv[1] == 'drive':
        make_drive(*sys.argv[2:])
    else:
        sys.exit(__doc__)
