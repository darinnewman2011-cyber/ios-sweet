#!/bin/bash
# Build the ZTE K88 diagnostic shim (clang + lld, no GNU cross toolchain needed).
# Then: python tools/mkqcomboot.py <K88 boot.img> shim/k88/out/Image <jasmine.dtb> shim/k88/out/k88-diag-boot.img
# and `fastboot boot shim/k88/out/k88-diag-boot.img` (nothing is flashed).
set -euo pipefail
cd "$(dirname "$0")"
export PATH="/c/Program Files/LLVM/bin:$PATH"
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
 -mgeneral-regs-only -mstrict-align -Wall -Wextra \
 -DFB_BASE=0x83000000UL -DFB_W=800 -DFB_H=1280 -DCON_SCALE=2 ${EXTRA_CFLAGS:-}"
mkdir -p out
rm -f out/*.o out/Image
clang $CFLAGS -c start.S -o out/start.o
clang $CFLAGS -c diag.c -o out/diag.o
clang $CFLAGS -c ../console.c -o out/console.o
clang $CFLAGS -c ../fdt.c -o out/fdt.o
# The image runs wherever LK puts it: no absolute relocations allowed in C objects.
if command -v llvm-readelf >/dev/null; then readelf="llvm-readelf"; else readelf="llvm-readobj --elf-output-style=GNU"; fi
abs=$($readelf -r out/diag.o out/console.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -nostdlib -static -N --no-rosegment -T k88.ld out/start.o out/diag.o out/console.o out/fdt.o -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
ls -l out/Image
