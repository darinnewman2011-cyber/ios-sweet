#!/bin/bash
# Build the ZTE K88 shim (clang + lld, no GNU cross toolchain needed).
#   shim/k88/build.sh                         diagnostics (milestones 1+2)
#   PAYLOAD=xnu-k88.payload shim/k88/build.sh XNU loader (tools/mkpayload.py output, linked into the Image)
# Then: python tools/mkqcomboot.py <K88 boot.img> shim/k88/out/Image <jasmine.dtb> OUT.img and write OUT.img
# to the boot partition from TWRP (the K88 has no working fastboot).
set -euo pipefail
[ -z "${PAYLOAD:-}" ] || PAYLOAD="$(cd "$(dirname "$PAYLOAD")" && pwd)/$(basename "$PAYLOAD")"
cd "$(dirname "$0")"
export PATH="/c/Program Files/LLVM/bin:$PATH"
# FB_BASE = the 32 bpp scanout the shim sets up (= SWEET_FB_PA, boot_args.Video)
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
 -mgeneral-regs-only -mstrict-align -Wall -Wextra \
 -DFB_BASE=0x83400000UL -DFB_W=800 -DFB_H=1280 -DCON_SCALE=2 ${EXTRA_CFLAGS:-}"
mkdir -p out
rm -f out/*.o out/Image out/payload.bin
{
	echo '	.section .rodata.payload, "a"'
	echo '	.balign 16'
	echo '	.globl k88_payload'
	echo 'k88_payload:'
	if [ -n "${PAYLOAD:-}" ]; then
		head -c 8 "$PAYLOAD" | grep -q SWEETXNU || { echo "error: $PAYLOAD is not a SWEETXNU payload" >&2; exit 1; }
		cp "$PAYLOAD" out/payload.bin	# relative path: Windows clang can't open /c/... paths
		echo '	.incbin "out/payload.bin"'
	fi
	echo 'k88_payload_end:'
	echo '	.balign 8'
	echo '	.globl k88_payload_size'
	echo 'k88_payload_size:'
	echo '	.quad k88_payload_end - k88_payload'
} > out/payload.S
clang $CFLAGS -c start.S -o out/start.o
clang $CFLAGS -c out/payload.S -o out/payload.o
clang $CFLAGS -c diag.c -o out/diag.o
clang $CFLAGS -c ../console.c -o out/console.o
clang $CFLAGS -c ../fdt.c -o out/fdt.o
clang $CFLAGS -c ../loader.c -o out/loader.o
# The image runs wherever LK puts it: no absolute relocations allowed in C objects.
if command -v llvm-readelf >/dev/null; then readelf="llvm-readelf"; else readelf="llvm-readobj --elf-output-style=GNU"; fi
abs=$($readelf -r out/diag.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -nostdlib -static -N --no-rosegment -T k88.ld out/start.o out/payload.o out/diag.o out/console.o out/fdt.o out/loader.o -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
ls -l out/Image
