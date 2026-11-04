#!/usr/bin/env python3
"""Wrap a Galaxy A8 (2018) boot image into a TWRP-flashable zip.

  mka8flashzip.py BOOT.img OUT.zip [--name ios42]

In TWRP: Install, pick the zip, swipe. The zip's update-binary checks that
BOOT is mmcblk0p12, that the Android BOOT backup (/sdcard/a8bak/BOOT.img) is
there with its known hash, and that the image inside has the hash recorded
here; any failure stops before BOOT is touched. It then writes BOOT, reads it
back, and puts the Android backup back if the readback differs. It writes
BOOT only and never reboots.

The images themselves stay out of git (they carry the phone's own DTBH
bundle, see mka8boot.py): build the zip from an image wherever it is.
Standard library only.
"""
import argparse, hashlib, os, zipfile

BOOT_PART = 32 * 1024 * 1024
ANDROID_BOOT_BACKUP_SHA256 = "c87a1336e8e6ca031b402cce84add38c9ec24172e3293de868cd7b05d6c9a59d"

UPDATE_BINARY = r"""#!/sbin/sh
# TWRP: flash the @NAME@ boot image to the Galaxy A8's BOOT partition, with the usual checks.
# Writes BOOT (mmcblk0p12) only. Does not reboot: reboot from TWRP yourself when it says DONE.
OUTFD=$2
ZIP=$3
WANT=@SHA@
BAK=@BAK@
IMG=/tmp/@NAME@-boot.img
say() { echo "ui_print $1" >> /proc/self/fd/$OUTFD; echo "ui_print" >> /proc/self/fd/$OUTFD; }
fail() { say "ABORT: $1"; say "BOOT was not changed."; rm -f $IMG; exit 1; }

say "@NAME@ -> BOOT"
dev=$(readlink -f /dev/block/by-name/BOOT)
[ "$dev" = /dev/block/mmcblk0p12 ] || fail "BOOT is $dev, not mmcblk0p12"
[ -f /sdcard/a8bak/BOOT.img ] || fail "Android BOOT backup /sdcard/a8bak/BOOT.img is missing"
[ "$(sha256sum /sdcard/a8bak/BOOT.img | cut -d' ' -f1)" = "$BAK" ] || fail "Android BOOT backup hash is wrong"
unzip -p "$ZIP" @NAME@-boot.img > $IMG || fail "could not unpack the image"
[ "$(sha256sum $IMG | cut -d' ' -f1)" = "$WANT" ] || fail "image hash is wrong (bad copy?)"
say "Checks OK, writing BOOT..."
dd if=$IMG of=/dev/block/mmcblk0p12 bs=1048576 2>/dev/null
sync
rb=$(dd if=/dev/block/mmcblk0p12 bs=1048576 count=32 2>/dev/null | sha256sum | cut -d' ' -f1)
rm -f $IMG
if [ "$rb" != "$WANT" ]; then
	say "READBACK MISMATCH: putting Android BOOT back"
	dd if=/sdcard/a8bak/BOOT.img of=/dev/block/mmcblk0p12 bs=1048576 2>/dev/null
	sync
	exit 1
fi
say "DONE: @NAME@ is on BOOT and verified."
say "Reboot > System to start it."
exit 0
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--name", default=None, help="short name shown in TWRP (default: from the image file name)")
    a = ap.parse_args()
    data = open(a.image, "rb").read()
    # the readback hashes the whole 32 MiB partition, so the image must be exactly that size
    if len(data) != BOOT_PART:
        raise SystemExit(f"{a.image} is {len(data)} bytes; a BOOT image is {BOOT_PART} (mka8boot.py pads to it)")
    name = a.name or os.path.basename(a.image).replace("a8-", "").replace("-boot.img", "").replace(".img", "")
    sha = hashlib.sha256(data).hexdigest()
    script = (UPDATE_BINARY.replace("@NAME@", name).replace("@SHA@", sha)
              .replace("@BAK@", ANDROID_BOOT_BACKUP_SHA256))
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("META-INF/com/google/android/update-binary", script)
        z.writestr("META-INF/com/google/android/updater-script", "#MAGISK\n")
        z.writestr(f"{name}-boot.img", data)
    print(f"{a.out}: {name}, image sha256 {sha}")


if __name__ == "__main__":
    main()
