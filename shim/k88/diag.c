/*
 * ZTE K88 (MSM8952 / Snapdragon 617) bring-up diagnostic, milestones 1+2 of the
 * A8 path in one boot, ordered by risk (everything above a hang stays on screen):
 *   1. what LK hands over: EL, MPIDR/MIDR, SCTLR, ID registers, CNTFRQ, FDT, initrd;
 *      the MDSS pipe that scans LK's continuous-splash framebuffer (address,
 *      stride, format), colour bars;
 *   2. GICv2 (qcom,msm-qgic2) virtual-timer and SGI interrupts, PSCI version,
 *      PSCI CPU_ON for the other seven Cortex-A53s;
 *   3. read-only register dump of the eMMC host (sdhci-msm), no commands sent.
 * Booted with `fastboot boot` (nothing is flashed). Any unexpected exception
 * paints a red screen with ESR/ELR/FAR.
 *
 * Display: the panel is a video-mode DSI panel (BOE JD9366, 800x1280), so the
 * MDP keeps scanning LK's buffer on its own: no trigger is needed (unlike the
 * A8's command-mode panel). LK's splash is usually 24 bpp; shim/console.c draws
 * 32 bpp, so it draws into a shadow buffer and k88_present() packs it into the
 * scanned buffer. A 32 bpp scanout is drawn into directly.
 */
#include <stdint.h>
#include "../console.h"
#include "../fdt.h"

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" :: "r"((uint64_t)(v)))

/* GICv2, from the K88's boot.img DTB (interrupt-controller@b000000) */
#define GICD_BASE	0x0b000000UL
#define GICC_BASE	0x0b002000UL
#define GICD_CTLR	0x000
#define GICD_TYPER	0x004
#define GICD_IIDR	0x008
#define GICD_ISENABLER0	0x100
#define GICD_ICENABLER0	0x180
#define GICD_ISPENDR0	0x200
#define GICD_IPRIORITYR	0x400
#define GICD_SGIR	0xf00
#define  SGIR_SELF	(2u << 24)
#define GICC_CTLR	0x000
#define GICC_PMR	0x004
#define GICC_IAR	0x00c
#define GICC_EOIR	0x010
#define GICC_IIDR	0x0fc
/* DT timer: PPIs 2, 3, 4, 1 = secure phys, non-secure phys, virtual, hyp -> virtual = INTID 20 (guess until this runs) */
#define VTIMER_INTID	20
#define SGI_INTID	1

/* PSCI 1.0 with standard function IDs (dmesg: "PSCIv1.0 detected", "standard PSCI v0.2 function IDs") */
#define PSCI_VERSION	0x84000000UL
#define PSCI_CPU_OFF	0x84000002UL
#define PSCI_CPU_ON	0xc4000003UL

/* DT order: cpu@100..103 (cluster1 in cpu-map), cpu@0..3 */
static const uint32_t k88_mpidr[8] = { 0x100, 0x101, 0x102, 0x103, 0x000, 0x001, 0x002, 0x003 };
#define MPIDR_SLOT(m)	((((m) >> 8) & 0xff) * 4 + ((m) & 0xff))	/* same as start.S */

/*
 * MDSS (qcom,mdss_mdp@1a00000). SSPP register layout of MDP5; whether the
 * pipe offsets count from 0x1a00000 or from 0x1a01000 (mdp-reg-offset) is
 * not settled, so both are probed and the plausible one wins.
 */
#define MDSS_BASE	0x01a00000UL
#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30
#define SSPP_SRC_UNPACK		0x34
static const uint32_t pipe_off[4] = { 0x15000, 0x17000, 0x25000, 0x05000 };	/* RGB0, RGB1, DMA0, VIG0 */
#define SPLASH_PA	0x83000000UL	/* reserved-memory splash_region, 20 MiB */
#define SHADOW_PA	0xa0000000UL	/* plain System RAM, far from LK's load addresses */

/* eMMC host, sdhci-msm (sdhci@7824000): hc_mem 0x7824900, core_mem 0x7824000 */
#define SDHC_BASE	0x07824900UL
#define SDCORE_BASE	0x07824000UL

extern char secondary_entry[];

static volatile uint32_t irq_count, irq_other, irq_last, irq_src;
static volatile uint64_t sec_mpidr[12];

static uintptr_t fb_pa = SPLASH_PA;	/* scanned buffer */
static uint32_t fb_stride = FB_W * 3;	/* bytes */
static int fb_direct;			/* 32 bpp scanout: drawn into directly */

static uint32_t mmio32(uintptr_t a) { return *(volatile uint32_t *)a; }
static void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

static uint64_t ticks_per_sec(void)
{
	uint64_t f = rsr("cntfrq_el0");

	return f ? f : 19200000;
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

/* ---- display ---- */

/* Pack the 32 bpp shadow into the 24 bpp scanout, 4 pixels = 3 words (bytes B,G,R per pixel). */
static void k88_present(void)
{
	if (fb_direct) {
		__asm__ volatile("dsb sy" ::: "memory");
		return;
	}
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *s = (volatile uint32_t *)(SHADOW_PA + (uintptr_t)y * FB_STRIDE);
		volatile uint32_t *d = (volatile uint32_t *)(fb_pa + (uintptr_t)y * fb_stride);

		for (int x = 0; x < FB_W; x += 4, s += 4, d += 3) {
			uint32_t p0 = s[0] & 0xffffff, p1 = s[1] & 0xffffff, p2 = s[2] & 0xffffff, p3 = s[3] & 0xffffff;

			d[0] = p0 | p1 << 24;
			d[1] = p1 >> 8 | p2 << 16;
			d[2] = p2 >> 16 | p3 << 8;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

struct pipe_seen { uint32_t base, addr, stride, fmt, size, unpack; };
static struct pipe_seen pipes[8];

/* Find the pipe that scans LK's splash: a DRAM address and a stride of 800 * 3 or 800 * 4. */
static void find_scanout(void)
{
	int best = -1;

	for (int b = 0; b < 2; b++)
		for (int i = 0; i < 4; i++) {
			uintptr_t p = MDSS_BASE + (b ? 0x1000 : 0) + pipe_off[i];
			struct pipe_seen *ps = &pipes[b * 4 + i];
			uint32_t st;

			ps->base = (uint32_t)p;
			ps->addr = mmio32(p + SSPP_SRC0_ADDR);
			st = mmio32(p + SSPP_SRC_YSTRIDE0);
			ps->stride = st;
			ps->fmt = mmio32(p + SSPP_SRC_FORMAT);
			ps->size = mmio32(p + SSPP_SRC_SIZE);
			ps->unpack = mmio32(p + SSPP_SRC_UNPACK);
			st &= 0xffff;
			if (best < 0 && ps->addr >= 0x80000000u && (st == FB_W * 3 || st == FB_W * 4))
				best = b * 4 + i;
		}
	if (best >= 0) {
		fb_pa = pipes[best].addr;
		fb_stride = pipes[best].stride & 0xffff;
	}
	fb_direct = fb_stride == FB_W * 4;
	con_fb = (volatile uint32_t *)(fb_direct ? fb_pa : SHADOW_PA);
}

static void colour_bars(void)
{
	static const uint32_t c[4] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };

	for (int y = FB_H - 96; y < FB_H - 16; y++)
		for (int x = 0; x < FB_W; x++)
			con_fb[y * (FB_STRIDE / 4) + x] = c[x * 4 / FB_W];
}

/* ---- interrupts ---- */

static void irq_handler(void)
{
	uint32_t iar = mmio32(GICC_BASE + GICC_IAR);
	uint32_t id = iar & 0x3ff;

	if (id >= 1020)
		return;
	irq_last = id;
	if (id == irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", ticks_per_sec() / 100);
	wr32(GICC_BASE + GICC_EOIR, iar);
}

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
	delay_ms(20);
	pend = mmio32(GICD_BASE + GICD_ISPENDR0);	/* shows the INTID the timer really uses if it is not 20 */
	if (src == VTIMER_INTID)
		tctl = rsr("cntv_ctl_el0");
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

	con_hex2("GICD", "typer=", mmio32(GICD_BASE + GICD_TYPER), "iidr=", mmio32(GICD_BASE + GICD_IIDR));
	con_hex2("GICC", "ctlr=", mmio32(GICC_BASE + GICC_CTLR), "iidr=", mmio32(GICC_BASE + GICC_IIDR));
	k88_present();
	wr32(GICD_BASE + GICD_CTLR, dctlr | 1);
	con_hex2("GICD", "was=", dctlr, "now=", mmio32(GICD_BASE + GICD_CTLR));
	en = mmio32(GICD_BASE + GICD_ISENABLER0);
	wr8(GICD_BASE + GICD_IPRIORITYR + VTIMER_INTID, 0xa0);
	wr8(GICD_BASE + GICD_IPRIORITYR + SGI_INTID, 0xa0);
	wr32(GICD_BASE + GICD_ISENABLER0, (1u << VTIMER_INTID) | (1u << SGI_INTID));
	wr32(GICC_BASE + GICC_PMR, 0xf0);
	wr32(GICC_BASE + GICC_CTLR, mmio32(GICC_BASE + GICC_CTLR) | 1);
	con_hex2("ENABLE", "was=", en, "now=", mmio32(GICD_BASE + GICD_ISENABLER0));
	con_hex2("PRIO", "vt=", mmio32(GICD_BASE + GICD_IPRIORITYR + (VTIMER_INTID & ~3u)) >> (8 * (VTIMER_INTID & 3)) & 0xff,
		 "pmr=", mmio32(GICC_BASE + GICC_PMR));
	k88_present();
	irq_window("VTIMER", VTIMER_INTID);
	k88_present();
	irq_window("SGI1", SGI_INTID);
	wr32(GICD_BASE + GICD_ICENABLER0, (1u << VTIMER_INTID) | (1u << SGI_INTID));
	k88_present();
}

/* ---- SMP ---- */

void secondary_main(void)
{
	uint64_t m = rsr("mpidr_el1");

	sec_mpidr[MPIDR_SLOT(m) % 12] = m | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
}

static void test_smp(void)
{
	uint64_t self = rsr("mpidr_el1") & 0xffffff;
	uint32_t alive = 0, want = 0, fail = 0, rc_bad = 0;

	for (int i = 0; i < 8; i++) {
		uint32_t rc;

		if (k88_mpidr[i] == self)
			continue;
		want |= 1u << MPIDR_SLOT(k88_mpidr[i]);
		rc = (uint32_t)smc(PSCI_CPU_ON, k88_mpidr[i], (uintptr_t)secondary_entry, i);
		if (rc) {
			if (!fail)
				rc_bad = rc;
			fail |= 1u << i;	/* index into k88_mpidr[] */
		}
	}
	con_hex2("CPU_ON", "fail=", fail, "rc=", rc_bad);
	delay_ms(500);
	for (int i = 0; i < 12; i++)
		if (sec_mpidr[i])
			alive |= 1u << i;
	con_hex2("SMP", "alive=", alive, "want=", want);	/* bit = Aff1 * 4 + Aff0 */
}

/* ---- main ---- */

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

void k88_main(uintptr_t dtb)
{
	uint64_t s = 0;

	find_scanout();
	con_clear(BLUE);
	colour_bars();	/* expected left to right: red, green, blue, white */
	con_str("K88 SHIM", "milestone 1+2");
	con_hex2("CPU", "el=", rsr("CurrentEL") >> 2, "mpidr=", rsr("mpidr_el1"));
	con_hex2("ID", "midr=", rsr("midr_el1"), "sctlr=", rsr("sctlr_el1"));
	con_hex2("ID2", "pfr0=", rsr("id_aa64pfr0_el1"), "mmfr0=", rsr("id_aa64mmfr0_el1"));
	con_hex2("ID3", "isar0=", rsr("id_aa64isar0_el1"), "frq=", rsr("cntfrq_el0"));
	if (dtb && be32(dtb) == 0xd00dfeed)
		fdt_chosen_u64(dtb, "linux,initrd-start", &s);
	con_hex2("BOOT", "dtb=", dtb, "initrd=", s);
	con_hex2("DTB", "magic=", dtb ? be32(dtb) : 0, "size=", dtb ? be32(dtb + 4) : 0);
	con_hex2("FB", "pa=", fb_pa, "stride=", fb_stride);
	k88_present();
	for (int i = 0; i < 8; i++) {
		char label[] = "P0";

		label[1] = '0' + i;	/* 0-3 = RGB0, RGB1, DMA0, VIG0 from 0x1a00000; 4-7 the same from 0x1a01000 */
		if (pipes[i].addr)
			con_hex2(label, "a=", pipes[i].addr, "st=", pipes[i].stride);
		if (pipes[i].addr == fb_pa)
			con_hex2(" fmt", "f=", pipes[i].fmt, "up=", pipes[i].unpack);
	}
	k88_present();
	test_gic();
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), 0, 0);
	k88_present();
	test_smp();
	k88_present();
	con_hex2("SDHC", "ver=", mmio32(SDHC_BASE + 0xfc), "pst=", mmio32(SDHC_BASE + 0x24));
	con_hex2("SDHC2", "ctl=", mmio32(SDHC_BASE + 0x28), "clk=", mmio32(SDHC_BASE + 0x2c));
	con_hex2("SDHC3", "ctl2=", mmio32(SDHC_BASE + 0x3c), "cap=", mmio32(SDHC_BASE + 0x40));
	con_hex2("SDCORE", "mode=", mmio32(SDCORE_BASE + 0x78), "ver=", mmio32(SDCORE_BASE + 0x50));
	con_str("DONE", "halted. hold power ~10 s");
	k88_present();
}

void k88_exc(uint64_t slot, uint64_t *regs)
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
	con_hex("X30", regs[31]);
	k88_present();
	for (;;)
		__asm__ volatile("wfe");
}
