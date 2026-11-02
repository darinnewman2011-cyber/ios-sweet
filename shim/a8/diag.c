/*
 * Galaxy A8 (2018) bring-up diagnostic.
 * Milestone 1 (done 2026-11-01): what S-Boot hands over, on the panel.
 * Milestone 2: GIC-400 (GICv2) virtual-timer and SGI interrupts on the boot
 * CPU, PSCI version, and PSCI CPU_ON for the other seven cores.
 * No storage is touched. Every step presents the frame before it starts, so
 * if something hangs the screen shows how far it got.
 */
#include <stdint.h>
#include "../console.h"
#include "../fdt.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" :: "r"((uint64_t)(v)))

/* GIC-400 (GICv2), non-secure view: group and security bits are RAZ/WI. */
#define GICD_BASE	0x12301000UL	/* distributor */
#define GICC_BASE	0x12302000UL	/* CPU interface */
#define GICD_CTLR	0x000		/* NS view: bit 0 = EnableGrp1 */
#define GICD_ISENABLER0	0x100
#define GICD_ICENABLER0	0x180
#define GICD_ISPENDR0	0x200
#define GICD_IPRIORITYR	0x400
#define GICD_SGIR	0xf00
#define  SGIR_SELF	(2u << 24)	/* TargetListFilter: requesting CPU only */
#define GICC_CTLR	0x000
#define GICC_PMR	0x004
#define GICC_IAR	0x00c
#define GICC_EOIR	0x010
#define VTIMER_INTID	27		/* DT: virtual timer = PPI 11 */
#define SGI_INTID	1

/* PSCI v0.1: the DT lists only cpu_suspend/cpu_off/cpu_on. */
#define PSCI_VERSION	0x84000000UL	/* not in the DT list: -1 = not supported */
#define PSCI_CPU_OFF	0x84000002UL
#define PSCI_CPU_ON	0xc4000003UL

/* Logical CPU order, as JP_CORES in tools/mkadt.py: six A53s, then two A73s. */
static const uint32_t a8_mpidr[8] = { 0x100, 0x101, 0x102, 0x103, 0x200, 0x201, 0x000, 0x001 };
#define MPIDR_SLOT(m)	((((m) >> 8) & 0xff) * 4 + ((m) & 0xff))	/* same as start.S */

/*
 * DECON-F, command mode with hardware TE trigger. Offsets and bits from
 * Samsung's dpu_7885 driver (regs-decon.h / decon_reg.c). The panel only
 * shows a new frame after DECON is re-armed, and TRIG_AUTO_MASK_EN masks
 * the trigger again after every frame, so each update needs a8_present().
 */
#define DECON_BASE		0x14860000UL
#define DECON_GLOBAL_CONTROL	0x0000
#define  GC_RUN_STATUS		(1u << 4)
#define  GC_DECON_EN		(1u << 1)
#define  GC_DECON_EN_F		(1u << 0)
#define DECON_SHADOW_UPDATE	0x0060
#define  SHADOW_UPDATE_GLOBAL	(1u << 31)
#define DECON_TRIG_CONTROL	0x0070		/* HW_SW_TRIG_CONTROL */
#define  HW_TRIG_MASK_DECON	(1u << 4)
#define  HW_TRIG_EN		(1u << 0)
#define DPP0_DMA_BASE		0x148B1000UL	/* channel holding the S-Boot framebuffer */
#define IDMA_IN_BASE_ADDR_Y	0x0040

#define FB_SIZE		(FB_H * FB_STRIDE)	/* S-Boot reserves room for two buffers */

/* PMU software reset (verified 2026-11-01: reboots the SoC). */
#define PMU_SWRESET	0x11C80400UL

extern char secondary_entry[];

static volatile uint32_t irq_count, irq_other, irq_last, irq_src;
static volatile uint64_t sec_mpidr[12];	/* indexed by MPIDR_SLOT, written by each secondary */

static uint32_t mmio32(uintptr_t a)
{
	return *(volatile uint32_t *)a;
}

static void wr32(uintptr_t a, uint32_t v)
{
	*(volatile uint32_t *)a = v;
}

static void wr8(uintptr_t a, uint8_t v)
{
	*(volatile uint8_t *)a = v;
}

static uint64_t ticks_per_sec(void)
{
	uint64_t f = rsr("cntfrq_el0");

	return f ? f : 26000000;
}

static uint64_t now(void)
{
	__asm__ volatile("isb");
	return rsr("cntvct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;

	while (now() < end)
		;
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/* Copy the frame into S-Boot's second buffer (only needed if DPP0 scans that one). */
static void fb_mirror(void)
{
	volatile uint64_t *src = (volatile uint64_t *)FB_BASE;
	volatile uint64_t *dst = (volatile uint64_t *)(FB_BASE + FB_SIZE);

	for (uint64_t i = 0; i < FB_SIZE / 8; i++)
		dst[i] = src[i];
}

/* decon_reg_start(): enable, request a global shadow update, wait for RUN, unmask the HW trigger. */
static int a8_present(void)
{
	uint64_t f = ticks_per_sec(), t0;
	int ok = 1;

	if (mmio32(DPP0_DMA_BASE + IDMA_IN_BASE_ADDR_Y) != (uint32_t)FB_BASE)
		fb_mirror();
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DECON_BASE + DECON_GLOBAL_CONTROL,
	     mmio32(DECON_BASE + DECON_GLOBAL_CONTROL) | GC_DECON_EN | GC_DECON_EN_F);
	wr32(DECON_BASE + DECON_SHADOW_UPDATE,
	     mmio32(DECON_BASE + DECON_SHADOW_UPDATE) | SHADOW_UPDATE_GLOBAL);
	t0 = now();
	while (!(mmio32(DECON_BASE + DECON_GLOBAL_CONTROL) & GC_RUN_STATUS))
		if (now() - t0 > f / 50) {	/* driver waits 20 ms */
			ok = 0;
			break;
		}
	wr32(DECON_BASE + DECON_TRIG_CONTROL,
	     (mmio32(DECON_BASE + DECON_TRIG_CONTROL) & ~HW_TRIG_MASK_DECON) | HW_TRIG_EN);
	return ok;
}

#ifdef A8_PROBE_RESET
/* EXTRA_CFLAGS=-DA8_PROBE_RESET=N: reboot N seconds after the last line, as a sign of life. */
static void __attribute__((noreturn)) pmu_reset_after(unsigned s)
{
	delay_ms(s * 1000);
	wr32(PMU_SWRESET, 1);
	for (;;)
		__asm__ volatile("wfe");
}
#endif

/* ---- interrupts ---- */

static void irq_handler(void)
{
	uint32_t iar = mmio32(GICC_BASE + GICC_IAR);
	uint32_t id = iar & 0x3ff;

	if (id >= 1020)
		return;	/* spurious */
	irq_last = id;
	if (id == irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", ticks_per_sec() / 100);	/* next one in 10 ms */
	wr32(GICC_BASE + GICC_EOIR, iar);
}

/* Run one interrupt source for 300 ms and report what the GIC delivered. */
static void irq_window(const char *label, uint32_t src)
{
	uint32_t pend, tctl = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", ticks_per_sec() / 100);
		wsr("cntv_ctl_el0", 1);
	} else {
		wr32(GICD_BASE + GICD_SGIR, SGIR_SELF | src);
	}
	/* sample with IRQs still masked, after the first timer period */
	delay_ms(20);
	pend = mmio32(GICD_BASE + GICD_ISPENDR0);
	if (src == VTIMER_INTID)
		tctl = rsr("cntv_ctl_el0");	/* bit 2 = ISTATUS: the timer condition is met */
	__asm__ volatile("msr daifclr, #2" ::: "memory");
	delay_ms(300);
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	con_hex2(label, "irqs=", irq_count, "last=", irq_last);
	con_hex2(" pre", "pend=", pend, "tctl=", tctl | irq_other << 16);
}

static void test_gic(void)
{
	uint32_t dctlr = mmio32(GICD_BASE + GICD_CTLR);
	uint32_t en;

	con_str("GIC", "configuring...");
	a8_present();
	wr32(GICD_BASE + GICD_CTLR, dctlr | 1);
	con_hex2("GICD", "was=", dctlr, "now=", mmio32(GICD_BASE + GICD_CTLR));
	en = mmio32(GICD_BASE + GICD_ISENABLER0);
	wr8(GICD_BASE + GICD_IPRIORITYR + VTIMER_INTID, 0xa0);
	wr8(GICD_BASE + GICD_IPRIORITYR + SGI_INTID, 0xa0);
	wr32(GICD_BASE + GICD_ISENABLER0, (1u << VTIMER_INTID) | (1u << SGI_INTID));
	wr32(GICC_BASE + GICC_PMR, 0xf0);
	wr32(GICC_BASE + GICC_CTLR, mmio32(GICC_BASE + GICC_CTLR) | 1);
	con_hex2("ENABLE", "was=", en, "now=", mmio32(GICD_BASE + GICD_ISENABLER0));
	con_hex2("PRIO", "i27=", mmio32(GICD_BASE + GICD_IPRIORITYR + 24) >> 24,
		 "pmr=", mmio32(GICC_BASE + GICC_PMR));
	a8_present();
	irq_window("VTIMER", VTIMER_INTID);
	a8_present();
	irq_window("SGI1", SGI_INTID);
	wr32(GICD_BASE + GICD_ICENABLER0, (1u << VTIMER_INTID) | (1u << SGI_INTID));
	a8_present();
}

/* ---- SMP ---- */

void secondary_main(void)
{
	uint64_t m = rsr("mpidr_el1");

	sec_mpidr[MPIDR_SLOT(m)] = m | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);	/* returns only on failure; start.S then parks in wfe */
}

static void test_smp(void)
{
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	uint32_t alive = 0;

	for (int i = 0; i < 8; i++) {
		char label[] = "CPU0";

		label[3] = '0' + i;
		if (a8_mpidr[i] == self) {
			con_str(label, "boot cpu");
			continue;
		}
		con_hex2(label, "mpidr=", a8_mpidr[i], 0, 0);
		a8_present();	/* if CPU_ON hangs, this is the last line */
		con_hex2(label, "ret=", (uint32_t)smc(PSCI_CPU_ON, a8_mpidr[i],
						     (uintptr_t)secondary_entry, i), 0, 0);
	}
	delay_ms(500);
	for (int i = 0; i < 12; i++)
		if (sec_mpidr[i])
			alive |= 1u << i;
	/* expected 0x3e3: slots 0,1 (A73), 5-7 (A53 0x101-0x103), 8,9 (A53 0x200/0x201) */
	con_hex2("SMP", "alive=", alive, "want=", 0x3e3);
}

/* ---- main ---- */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

void a8_main(uintptr_t dtb)
{
	uint64_t s = 0;

	con_clear(BLUE);
	con_str("A8 SHIM", "milestone 2: gic + smp");
	con_hex2("CPU", "el=", rsr("CurrentEL") >> 2, "mpidr=", rsr("mpidr_el1"));
	con_hex2("ID", "midr=", rsr("midr_el1"), "sctlr=", rsr("sctlr_el1"));
	if (dtb && be32(dtb) == 0xd00dfeed)
		fdt_chosen_u64(dtb, "linux,initrd-start", &s);
	con_hex2("BOOT", "dtb=", dtb, "initrd=", s);
	/* DECON state as S-Boot left it, captured before we touch it. */
	con_hex2("DECON", "gc=", mmio32(DECON_BASE + DECON_GLOBAL_CONTROL),
		 "trig=", mmio32(DECON_BASE + DECON_TRIG_CONTROL));
	con_str("PRESENT", a8_present() ? "run ok" : "no run status");
	test_gic();
	con_str("PSCI", "calling version...");
	a8_present();
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), 0, 0);
	a8_present();
	test_smp();
#ifdef A8_PROBE_RESET
	con_str("DONE", "PMU reset soon");
	a8_present();
	pmu_reset_after(A8_PROBE_RESET);
#endif
	con_str("DONE", "halted. hold power+vol down");
	a8_present();
}

void a8_exc(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(RED);
	con_str("EXC", "unexpected exception");
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X30", regs[31]);	/* vent stashed x29/x30 just above the 30-slot frame */
	a8_present();
	for (;;)
		__asm__ volatile("wfe");
}
