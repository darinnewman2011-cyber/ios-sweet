#!/bin/bash
# Build the A8 diagnostic shim (clang + lld, no GNU cross toolchain needed).
set -euo pipefail
cd "$(dirname "$0")"
CFLAGS="--target=aarch64-none-elf -O2 -ffreestanding -fno-builtin -nostdlib -fno-pic -fno-stack-protector \
 -mgeneral-regs-only -mstrict-align -Wall -Wextra \
 -DFB_BASE=0xEC000000UL -DFB_H=2220"
mkdir -p out
clang $CFLAGS -c start.S -o out/start.o
clang $CFLAGS -c diag.c -o out/diag.o
clang $CFLAGS -c ../console.c -o out/console.o
clang $CFLAGS -c ../fdt.c -o out/fdt.o
# The image runs wherever S-Boot puts it: no absolute relocations allowed in C objects.
abs=$(llvm-readelf -r out/diag.o out/console.o out/fdt.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
ld.lld -nostdlib -static -N --no-rosegment -T a8.ld out/start.o out/diag.o out/console.o out/fdt.o -o out/shim.elf
llvm-objcopy -O binary out/shim.elf out/Image
size=$(llvm-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}' 2>/dev/null || true)
if [ -z "$size" ]; then size=$(python3 -I -c "import subprocess,sys;o=subprocess.check_output(['llvm-nm','out/shim.elf'],text=True);print(int([l.split()[0] for l in o.splitlines() if l.split()[2]=='_end'][0],16))"); fi
truncate -s "$size" out/Image
ls -l out/Image
