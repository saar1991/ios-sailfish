/*
 * Oukitel WP210 Pro (MT6895) XNU loader. LK enters at EL1, MMU/caches off,
 * x0 = DTB. The SWEETXNU payload (tools/mkpayload.py) is appended to this
 * Image at _end; loader.c copies the kernel to the ADT's dram-base
 * (0x1c0000000, a carve-out-free bank), places the ADT and boot_args behind
 * it and jumps. The MediaTek watchdog stays off: a watchdog reset makes LK
 * enter its memory-dump mode.
 */
#include <stdint.h>
#include "console.h"
#include "loader.h"

#define WDT_MODE	0x1C007000UL
#define WDT_KEY		0x22000000u

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

extern char _end[];

static void hang(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t dtb)
{
	*(volatile uint32_t *)WDT_MODE = WDT_KEY;	/* watchdog off */
	__asm__ volatile("dsb sy" ::: "memory");

	con_init();
	con_clear(0xff000000u);
	con_str("WP210", "XNU loader");
	con_skip();
	con_hex("DTB", dtb);
	con_hex("EL", (rsr("CurrentEL") >> 2) & 3);
	con_hex("PAYLOAD", (uintptr_t)_end);
	con_flush();

	/* rd_end only bounds the overlap check: LK's DTB sits above the payload */
	const char *why = loader_boot_rd((uintptr_t)_end, 0x47c80000UL);
	con_str("LOADER FAILED", why);
	con_flush();
	hang();
}

void secondary_main(uint64_t idx)
{
	(void)idx;
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	(void)regs;
	con_skip();
	con_hex("EXCEPTION", slot);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_flush();
	hang();
}
