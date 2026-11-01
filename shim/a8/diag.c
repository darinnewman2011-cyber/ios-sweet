/*
 * Galaxy A8 (2018) bring-up diagnostic, milestone 1.
 * Prints what S-Boot handed over (exception level, CPU, FDT, initrd, GICv2
 * identity) on the S-Boot framebuffer, then halts. No storage is touched.
 * Order matters: anything that could hang (the PSCI call) is printed last.
 */
#include <stdint.h>
#include "../console.h"
#include "../fdt.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })

#define GICD_BASE	0x12301000UL	/* GIC-400 distributor (GICv2) */
#define GICC_BASE	0x12302000UL	/* CPU interface */

static uint32_t mmio32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

#ifdef A8_PROBE_RESET
/*
 * Proof-of-entry probe for a command-mode panel that keeps showing the S-Boot
 * splash: wait a few seconds, then reset through the PMU. If the phone
 * reboots by itself, S-Boot jumped to us. SWRESET (PMU 0x11C80000 + 0x400,
 * bit 0) is the exynos7 syscon-reboot value from mainline; unverified here.
 */
#define PMU_SWRESET	0x11C80400UL

static void probe_reset(void)
{
	uint64_t f = rsr("cntfrq_el0");
	uint64_t t0 = rsr("cntvct_el0");

	if (!f)
		f = 26000000;
	while (rsr("cntvct_el0") - t0 < A8_PROBE_RESET * f)
		;
	*(volatile uint32_t *)PMU_SWRESET = 1;
	for (;;)
		__asm__ volatile("wfe");
}
#endif

static uint64_t smc(uint64_t fn)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = 0;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
	__asm__ volatile("smc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static void bars(int y0)
{
	/* Colour order check: red, green, blue, white (a8r8g8b8). */
	static const uint32_t col[4] = { RED, GREEN, 0xff0000ffU, WHITE };
	for (int i = 0; i < 4; i++)
		for (int y = y0; y < y0 + 80 && y < FB_H; y++)
			for (int x = i * (FB_W / 4); x < (i + 1) * (FB_W / 4); x++)
				con_fb[y * (FB_STRIDE / 4) + x] = col[i];
}

void a8_main(uintptr_t dtb)
{
	uint64_t s, e;

	con_clear(BLUE);
	con_str("A8 SHIM", "jackpotlte alive");
	con_skip();
	con_hex("EL", rsr("CurrentEL") >> 2);
	con_hex("MPIDR", rsr("mpidr_el1"));
	con_hex("MIDR", rsr("midr_el1"));
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("PFR0", rsr("id_aa64pfr0_el1"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex("DTB", dtb);
	if (dtb) {
		con_hex("FDT", be32(dtb));
		con_hex("FDTSZ", be32(dtb + 4));
		if (fdt_chosen_u64(dtb, "linux,initrd-start", &s) &&
		    fdt_chosen_u64(dtb, "linux,initrd-end", &e)) {
			con_hex("INITRD", s);
			con_hex("INITSZ", e - s + 1);
		} else {
			con_str("INITRD", "not found");
		}
	}
	con_hex("GICDCTL", mmio32(GICD_BASE + 0x000));
	con_hex("GICDTYP", mmio32(GICD_BASE + 0x004));
	con_hex("GICDIID", mmio32(GICD_BASE + 0x008));
	con_hex("GICCCTL", mmio32(GICC_BASE + 0x000));
	con_hex("GICCIID", mmio32(GICC_BASE + 0x0fc));
	bars(FB_H - 160);
#ifdef A8_PROBE_RESET
	con_str("PROBE", "resetting via PMU");
	probe_reset();
#endif
	con_str("PSCI", "calling smc...");
	con_hex("PSCIVER", smc(0x84000000UL));	/* PSCI_VERSION; -1 means unsupported */
	con_str("DONE", "halted. hold power+vol down");
}

void a8_exc(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
	con_clear(RED);
	con_str("EXC", "unexpected exception");
	con_hex("SLOT", slot);
	con_hex("ESR", esr);
	con_hex("ELR", elr);
	con_hex("FAR", far);
}
