CROSS ?= aarch64-linux-gnu-
CFLAGS = -O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -fno-stack-protector -Wall
all: shim-boot.img
CFLAGS += -DUNGATE_KICK -DDUALPIPE -DCONSOLE
shim.elf: start.S shim.c link.ld
	$(CROSS)gcc $(CFLAGS) -T link.ld -o $@ start.S shim.c -static -Wl,--build-id=none
shim.bin: shim.elf
	$(CROSS)objcopy -O binary $< $@
shim-boot.img: shim.bin
	gzip -9 -n -c shim.bin > shim.bin.gz
	python3 mkboot.py shim.bin.gz $(STOCK_BOOT) $@
clean:
	rm -f shim.elf shim.bin shim-boot.img
