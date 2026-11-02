#!/bin/bash
# Build the A8 diagnostic shim (clang + lld, no GNU cross toolchain needed).
set -euo pipefail
[ -z "${PAYLOAD:-}" ] || PAYLOAD="$(cd "$(dirname "$PAYLOAD")" && pwd)/$(basename "$PAYLOAD")"
cd "$(dirname "$0")"
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
 -mgeneral-regs-only -mstrict-align -Wall -Wextra \
 -DFB_BASE=0xEC000000UL -DFB_H=2220 ${EXTRA_CFLAGS:-}"
# Proof-of-entry probe (reboots after N seconds): EXTRA_CFLAGS=-DA8_PROBE_RESET=4 shim/a8/build.sh
# XNU loader: PAYLOAD=out/xnu.payload shim/a8/build.sh (tools/mkpayload.py output, linked into the Image;
# S-Boot loads the stock 22 MB kernel the same way). Without PAYLOAD the diagnostics run.
mkdir -p out
rm -f out/payload.bin
{
	echo '	.section .rodata.payload, "a"'
	echo '	.balign 16'
	echo '	.globl a8_payload'
	echo 'a8_payload:'
	if [ -n "${PAYLOAD:-}" ]; then
		head -c 8 "$PAYLOAD" | grep -q SWEETXNU || { echo "error: $PAYLOAD is not a SWEETXNU payload" >&2; exit 1; }
		cp "$PAYLOAD" out/payload.bin	# relative path: Windows clang can't open /c/... paths
		echo '	.incbin "out/payload.bin"'
	fi
	echo 'a8_payload_end:'
	echo '	.balign 8'
	echo '	.globl a8_payload_size'
	echo 'a8_payload_size:'
	echo '	.quad a8_payload_end - a8_payload'
} > out/payload.S
clang $CFLAGS -c start.S -o out/start.o
clang $CFLAGS -c out/payload.S -o out/payload.o
clang $CFLAGS -c diag.c -o out/diag.o
clang $CFLAGS -c ../console.c -o out/console.o
clang $CFLAGS -c ../fdt.c -o out/fdt.o
clang $CFLAGS -c ../loader.c -o out/loader.o
# The image runs wherever S-Boot puts it: no absolute relocations allowed in C objects.
# (The Windows LLVM installer ships llvm-readobj but no llvm-readelf alias.)
if command -v llvm-readelf >/dev/null; then readelf="llvm-readelf"; else readelf="llvm-readobj --elf-output-style=GNU"; fi
relocs=$($readelf -r out/diag.o out/console.o out/fdt.o out/loader.o)
abs=$(grep -c ABS64 <<<"$relocs" || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -nostdlib -static -N --no-rosegment -T a8.ld out/start.o out/payload.o out/diag.o out/console.o out/fdt.o out/loader.o -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}' 2>/dev/null || true)
if [ -z "$size" ]; then size=$(python3 -I -c "import subprocess,sys;o=subprocess.check_output(['llvm-nm','out/shim.elf'],text=True);print(int([l.split()[0] for l in o.splitlines() if l.split()[2]=='_end'][0],16))"); fi
truncate -s "$size" out/Image
ls -l out/Image
