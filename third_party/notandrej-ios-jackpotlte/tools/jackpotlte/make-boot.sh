#!/bin/bash
# SPDX-License-Identifier: MIT
# Kernel Mach-O -> Galaxy A8 2018 (SM-A530F, jackpotlte) BOOT image.
#   make-boot.sh KERNEL STOCK_BOOT.img OUT.img [boot-args...]
# Default boot-args: one CPU, verbose console, no watchdog; no root device
# yet, so the first trials end in a root-mount panic by design.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../.." && pwd)
kernel=$(realpath "$1"); stock=$(realpath "$2"); out=$(realpath -m "$3")
shift 3
if [ ! -s "$kernel" ]; then
	echo "error: kernel $kernel missing or empty" >&2
	exit 1
fi
if [ ! -s "$stock" ]; then
	echo "error: stock boot image $stock missing or empty" >&2
	exit 1
fi
args=${*:-debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 wdt_test=no_user_checkin}
work=$repo/shim/jackpotlte/out
mkdir -p "$work"
python3 "$repo/tools/mkadt_jackpotlte.py" "$work/jackpotlte.adt"
python3 "$repo/tools/mkpayload.py" "$kernel" "$work/jackpotlte.adt" "$work/payload.bin" --cmdline "$args"
bash "$repo/shim/jackpotlte/build.sh" "$stock" "$work/payload.bin" "$out"
python3 "$repo/tools/jackpotlte_bootimg_info.py" "$out" | head -5
