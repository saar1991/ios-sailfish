#!/bin/bash
# Build the WP210 XNU boot image.
# Usage: ./build.sh STOCK_BOOT.img PAYLOAD OUT.img
set -euo pipefail
X=aarch64-linux-gnu-
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S wp210.c console.c fdt.c loader.c; do ${X}gcc $CFLAGS -c $f -o out/${f%.*}.o; done
abs=$(${X}readelf -r out/wp210.o out/console.o out/fdt.o out/loader.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations" >&2; exit 1; }
${X}gcc $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/wp210.o out/console.o out/fdt.o out/loader.o -o out/shim.elf
${X}objcopy -O binary out/shim.elf out/Image
size=$(${X}nm out/shim.elf | awk '$3=="_end"{print $1}')
truncate -s $((16#$size)) out/Image
cat "$2" >> out/Image
# image_size must cover the appended payload so LK places nothing on top of it
python3 -c "import struct,os;f=open('out/Image','r+b');f.seek(16);f.write(struct.pack('<Q',os.path.getsize('out/Image')))"
gzip -9 -n -c out/Image > out/Image.gz
python3 mkboot.py out/Image.gz "$1" "$3"
