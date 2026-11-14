#!/bin/bash
# Read the ZTE K88's hardware facts over adb into backup/k88/ (gitignored: the live DT and
# boot image can hold serial numbers). Picks the one attached device that is not the A8,
# and never prints adb serials. Uses `su` where available; without root some files are empty.
set -uo pipefail
ADB="${ADB:-/c/Users/Darin/Downloads/ONN/Vid stuff/platform-tools-latest-windows (2)/platform-tools/adb.exe}"
export MSYS_NO_PATHCONV=1
cd "$(dirname "$0")/.."
OUT=backup/k88; mkdir -p "$OUT"
mapfile -t devs < <("$ADB" devices -l | awk 'NR>1 && NF && $0 !~ /jackpotlte/ {print $1}')
[ "${#devs[@]}" = 1 ] || { echo "need exactly one non-A8 device, found ${#devs[@]}" >&2; exit 1; }
export ANDROID_SERIAL="${devs[0]}"
a() { "$ADB" shell "$@" 2>&1 | tr -d '\r'; }
SU=""
if a id | grep -q 'uid=0'; then echo "root: yes (adb shell is root, e.g. TWRP)"
elif a 'su -c id' | grep -q 'uid=0'; then SU="su -c"; echo "root: yes (su)"; else echo "root: no"; fi
ROOT=$(a id | grep -c 'uid=0' || true); [ -n "$SU" ] && ROOT=1
r() { if [ -n "$SU" ]; then a "su -c '$1'"; else a "$1"; fi; }

a getprop > "$OUT/getprop.txt"
grep -E 'ro.product.(model|device|board|name)|ro.board.platform|ro.build.(fingerprint|version.release)|ro.boot.(hardware|bootloader|verifiedbootstate|slot_suffix|flash.locked)|ro.bootimage|ro.treble|ro.hardware' "$OUT/getprop.txt" \
	| grep -viE 'serial' > "$OUT/props-summary.txt"
r 'cat /proc/cpuinfo' > "$OUT/cpuinfo.txt"
r 'cat /proc/iomem' > "$OUT/iomem.txt"
r 'cat /proc/meminfo' > "$OUT/meminfo.txt"
redact() { sed -E 's/(androidboot\.serialno|serialno|androidboot\.emmcid|androidboot\.wifimacaddr|androidboot\.btmacaddr)=[^ ]*/\1=<redacted>/g'; }
r 'cat /proc/cmdline' | redact > "$OUT/cmdline.txt"
r 'cat /proc/interrupts' > "$OUT/interrupts.txt"
r 'cat /proc/partitions' > "$OUT/partitions.txt"
r 'ls -l /dev/block/bootdevice/by-name/ /dev/block/platform/*/by-name/ 2>/dev/null' > "$OUT/by-name.txt"
r 'ls -l /dev/block/bootdevice' > "$OUT/bootdevice.txt"
r 'dmesg' | redact > "$OUT/dmesg.txt"
r 'cat /sys/class/graphics/fb0/name /sys/class/graphics/fb0/virtual_size /sys/class/graphics/fb0/bits_per_pixel /sys/class/graphics/fb0/stride /sys/class/graphics/fb0/msm_fb_panel_info /sys/class/graphics/fb0/msm_fb_type 2>&1' > "$OUT/fb0.txt"
r 'for f in /sys/class/mmc_host/mmc*/mmc*/{name,type,cid,ext_csd,manfid}; do echo "$f: $(cat $f 2>/dev/null | head -c 80)"; done; cat /sys/kernel/debug/mmc0/ios 2>/dev/null' \
	| grep -v -E '/(cid|serial):' > "$OUT/mmc.txt"
r 'getenforce; uname -a; cat /proc/version' > "$OUT/kernel.txt"
r 'cat /sys/devices/soc0/soc_id /sys/devices/soc0/hw_platform /sys/devices/soc0/platform_version /sys/devices/soc0/revision 2>&1' > "$OUT/soc0.txt"
r 'cat /proc/bus/input/devices' > "$OUT/input.txt"
r 'ls /sys/bus/i2c/devices/; for d in /sys/bus/i2c/devices/*; do echo "$d $(cat $d/name 2>/dev/null)"; done' > "$OUT/i2c.txt"
# live device tree (binary; the TWRP kernel has none, booted Android may), through /data/local/tmp
r 'cat /sys/firmware/fdt > /data/local/tmp/k88-live.dtb 2>/dev/null; chmod 644 /data/local/tmp/k88-live.dtb'
"$ADB" pull /data/local/tmp/k88-live.dtb "$OUT/live.dtb" >/dev/null 2>&1 || true
[ -s "$OUT/live.dtb" ] || rm -f "$OUT/live.dtb"
# boot partition (read-only dd), only with root
if [ "$ROOT" = 1 ]; then
	r 'dd if=/dev/block/bootdevice/by-name/boot of=/data/local/tmp/k88-boot.img bs=1M 2>/dev/null; chmod 644 /data/local/tmp/k88-boot.img'
	"$ADB" pull /data/local/tmp/k88-boot.img "$OUT/boot.img" >/dev/null 2>&1 || true
	r 'rm -f /data/local/tmp/k88-boot.img'
fi
r 'rm -f /data/local/tmp/k88-live.dtb'
ls -l "$OUT"
