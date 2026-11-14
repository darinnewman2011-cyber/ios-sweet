/*
 * ZTE K88 (MSM8952 / Snapdragon 617) bring-up diagnostic, milestones 1+2 of the
 * A8 path in one boot, ordered by risk (everything above a hang stays on screen):
 *   1. what LK hands over: EL, MPIDR/MIDR, SCTLR, ID registers, CNTFRQ, FDT, initrd;
 *      the MDSS pipe that scans LK's continuous-splash framebuffer (address,
 *      stride, format), colour bars;
 *   2. GICv2 (qcom,msm-qgic2) virtual-timer and SGI interrupts, PSCI version,
 *      PSCI CPU_ON for the other seven Cortex-A53s;
 *   3. eMMC host (sdhci-msm): register dump, then SDHCI mode on, card found/woken, two sectors read.
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
#include "../loader.h"

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
 * MDSS (qcom,mdss_mdp@1a00000). SSPP register layout of MDP5. diag3 showed
 * the pipe offsets count from 0x1a00000 (RGB0 at 0x1a15000 scans LK's splash);
 * the CTL offsets (DT qcom,mdss-ctl-off) count from the same base.
 */
#define MDSS_BASE	0x01a00000UL
#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC_XY		0x08
#define SSPP_OUT_SIZE		0x0c
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_FORMAT		0x30
#define SSPP_SRC_UNPACK		0x34
#define SSPP_SRC_OP_MODE	0x38
static const uint32_t pipe_off[4] = { 0x15000, 0x17000, 0x25000, 0x05000 };	/* RGB0, RGB1, DMA0, VIG0 */
static const uint32_t pipe_flush[4] = { 1u << 3, 1u << 4, 1u << 11, 1u << 0 };	/* CTL_FLUSH bits */
static const uint32_t ctl_off[3] = { 0x2000, 0x2200, 0x2400 };
#define CTL_LAYER0		0x00
#define CTL_TOP			0x14
#define CTL_FLUSH		0x18
/*
 * XRGB8888, bytes B,G,R,X in memory (= a8r8g8b8 little endian, what XNU's
 * framebuffer console writes): Linux mdss_mdp_format_setup() packing with
 * bits G/B/R/A = 8 (3), bpp 4, unpack count 4, tight; elements B,G,R,A.
 */
#define FMT_XRGB8888		0x000236ffu
#define UNPACK_BGRA		0x03020001u
#define SPLASH_PA	0x83000000UL	/* reserved-memory splash_region, 20 MiB */
#define FB32_PA		0x83400000UL	/* our 32 bpp scanout: 2 MiB aligned, in the splash carve-out (= SWEET_FB_PA) */
#define SHADOW_PA	0x90000000UL	/* plain System RAM below the kernel's bank (0xA0000000 up) */

/*
 * Log channel: the console text also goes into the console zone of Linux's
 * ramoops (cmdline ramoops.mem_address=0x9fd00000 mem_size=0x200000
 * console_size=0x100000, record_size 4 KiB, ecc 0; TWRP's iomem shows the zone
 * at 0x9fdfe000). After a reset that keeps DRAM, TWRP shows it as
 * /sys/fs/pstore/console-ramoops. persistent_ram_buffer: sig, start, size, data.
 */
#define PSTORE_CON	0x9fdfe000UL
#define PSTORE_SIZE	0x100000u
#define PRZ_SIG		0x43474244u	/* "DBGC" */

/* Reboot into TWRP: LK's RECOVERY_MODE cookie in IMEM restart_reason, then drop PS_HOLD. */
#define IMEM_RESTART	0x0860065cUL
#define RESTART_RECOVERY 0x77665502u
#define PSHOLD		0x004ab000UL
#ifndef K88_REBOOT_S
#define K88_REBOOT_S	20	/* seconds after the last line; 0 = halt */
#endif

/* eMMC host, sdhci-msm (sdhci@7824000): hc_mem 0x7824900, core_mem 0x7824000 */
#define SDHC_BASE	0x07824900UL
#define SDCORE_BASE	0x07824000UL

extern char secondary_entry[];


static volatile uint32_t irq_count, irq_other, irq_last, irq_src;
static volatile uint64_t sec_mpidr[12];

static uintptr_t fb_pa = SPLASH_PA;	/* scanned buffer */
static int fb_pipe = -1;		/* index into pipes[] that scans it */
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

static void k88_present(void);

/* ---- log ---- */

/*
 * LK writes its own log into this zone on the next boot, over the first few KB
 * (seen 2026-11-13: diag2's first lines were replaced by LK's). Our text
 * starts after LOG_SKIP bytes of newlines.
 */
#define LOG_SKIP	0x4000u
static uint32_t log_n;

static void log_init(void)
{
	wr32(PSTORE_CON + 0, PRZ_SIG);
	wr32(PSTORE_CON + 4, 0);
	for (log_n = 0; log_n < LOG_SKIP; log_n++)
		wr8(PSTORE_CON + 12 + log_n, 10);	/* newline */
	wr32(PSTORE_CON + 8, log_n);
}

void con_tee(const char *s)
{
	for (; *s && log_n < PSTORE_SIZE - 12; s++, log_n++)
		wr8(PSTORE_CON + 12 + log_n, (uint8_t)*s);
	wr32(PSTORE_CON + 8, log_n);	/* size; start stays 0 (no wrap) */
	__asm__ volatile("dsb sy" ::: "memory");
}

static void __attribute__((noreturn)) reboot_twrp(void)
{
	wr32(IMEM_RESTART, RESTART_RECOVERY);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(PSHOLD, 0);
	for (;;)
		__asm__ volatile("wfe");
}

static void __attribute__((noreturn)) finish(void)
{
	if (K88_REBOOT_S) {
		con_str("REBOOT", "to TWRP in a few s");
		k88_present();
		delay_ms(K88_REBOOT_S * 1000);
		wr32(IMEM_RESTART, RESTART_RECOVERY);
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(PSHOLD, 0);
		delay_ms(2000);
		con_str("REBOOT", "PS_HOLD did not reset");
	} else {
		con_str("DONE", "halted. hold power ~10 s");
	}
	k88_present();
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- display ---- */

/* Pack the 32 bpp shadow into the 24 bpp scanout, 4 pixels = 3 words (bytes B,G,R per pixel). */
static void k88_present(void)
{
	if (fb_direct) {
		__asm__ volatile("dsb sy" ::: "memory");
		return;
	}
	if ((fb_pa | fb_stride) & 3) {
		/* LK may point the pipe at an unaligned offset (diag2: a word store at 0x83200036 took an
		 * alignment fault; with the MMU off all of DRAM is Device memory): go byte by byte. */
		for (int y = 0; y < FB_H; y++) {
			volatile uint32_t *s = (volatile uint32_t *)(SHADOW_PA + (uintptr_t)y * FB_STRIDE);
			uintptr_t d = fb_pa + (uintptr_t)y * fb_stride;

			for (int x = 0; x < FB_W; x++, d += 3) {
				uint32_t p = s[x];

				wr8(d, (uint8_t)p);
				wr8(d + 1, (uint8_t)(p >> 8));
				wr8(d + 2, (uint8_t)(p >> 16));
			}
		}
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
		fb_pipe = best;
	}
	fb_direct = fb_stride == FB_W * 4;
	con_fb = (volatile uint32_t *)(fb_direct ? fb_pa : SHADOW_PA);
}

/*
 * Point the pipe that scans LK's splash at a 32 bpp buffer at FB32_PA (black,
 * full panel size, same position and flip), then flush the CTL that drives it.
 * Returns 1 if the pipe took the new address.
 */
static uint32_t fb32_ctl, fb32_before, fb32_after;

static int scanout_32bpp(void)
{
	if (fb_pipe < 0 || fb_pipe >= 4)
		return 0;	/* only the 0x1a00000-based pipes are real (diag3) */
	uintptr_t p = MDSS_BASE + pipe_off[fb_pipe];
	uintptr_t ctl = MDSS_BASE + ctl_off[0];

	for (int i = 0; i < 3; i++)	/* the CTL with layers staged is the one in use */
		if (mmio32(MDSS_BASE + ctl_off[i] + CTL_LAYER0)) {
			ctl = MDSS_BASE + ctl_off[i];
			break;
		}
	fb32_ctl = (uint32_t)ctl;
	for (uintptr_t a = FB32_PA; a < FB32_PA + (uintptr_t)FB_H * FB_W * 4; a += 8)
		*(volatile uint64_t *)a = 0xff000000ff000000ull;
	__asm__ volatile("dsb sy" ::: "memory");
	fb32_before = mmio32(p + SSPP_SRC0_ADDR);
	wr32(p + SSPP_SRC_FORMAT, FMT_XRGB8888);
	wr32(p + SSPP_SRC_UNPACK, UNPACK_BGRA);
	wr32(p + SSPP_SRC_YSTRIDE0, FB_W * 4);
	wr32(p + SSPP_SRC0_ADDR, (uint32_t)FB32_PA);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(ctl + CTL_FLUSH, pipe_flush[fb_pipe]);
	delay_ms(50);	/* a few frames at 60 Hz */
	fb32_after = mmio32(p + SSPP_SRC0_ADDR);
	fb_pa = FB32_PA;
	fb_stride = FB_W * 4;
	fb_direct = 1;
	con_fb = (volatile uint32_t *)FB32_PA;
	return fb32_after == (uint32_t)FB32_PA;
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

/*
 * Milestone 3: eMMC read through sdhci-msm. LK's exit path leaves HC_MODE
 * (core + 0x78) at 0, i.e. the controller in its legacy mode (diag3/4), and
 * may have put the card to sleep (CMD7 deselect, CMD5). Enable SDHCI mode,
 * find the card by CMD13, wake it with CMD5 + CMD7 if needed, read sectors.
 */
#define HC(o)		(SDHC_BASE + (o))
#define R48		0x1aU	/* 48-bit response, CRC and index check */
#define R48B		0x1bU	/* with busy */

static uint32_t sd_wait(uint32_t mask, unsigned ms)
{
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;
	uint32_t st;

	while (!((st = mmio32(HC(0x30))) & (mask | 0x8000U)))
		if (now() > end)
			return 0;
	return st;
}

static void sd_reset(void)
{
	wr8(HC(0x2f), 6);
	for (int i = 0; i < 1000 && (mmio32(HC(0x2c)) & 0x06000000U); i++)
		delay_ms(1);
	wr32(HC(0x30), 0xffffffffU);
}

/* returns the interrupt status (0 = no response), *r = RESP0 */
static uint32_t sd_cmd(uint32_t idx, uint32_t arg, uint32_t cmdr, uint32_t xfer, uint32_t *r)
{
	uint32_t st;

	for (int i = 0; i < 100 && (mmio32(HC(0x24)) & 3); i++)
		delay_ms(1);
	wr32(HC(0x30), 0xffffffffU);
	wr32(HC(0x08), arg);
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(HC(0x0c), ((idx << 8 | cmdr) << 16) | xfer);
	st = sd_wait(1, 100);
	*r = mmio32(HC(0x10));
	if (!st || (st & 0x8000U)) {
		uint32_t e = st;
		sd_reset();
		return e ? e : 0;
	}
	if ((cmdr & 3) == 3) {	/* busy: wait for transfer complete (DAT0 released) */
		sd_wait(2, 1000);
	}
	wr32(HC(0x30), 3);
	return st;
}

/* one sector; returns 0 on success */
static int sd_read(uint32_t lba, uint32_t *buf)
{
	uint32_t r, st;

	wr32(HC(0x04), (1U << 16) | 512);
	st = sd_cmd(17, lba, R48 | 0x20U, 0x10U, &r);
	if (!(st & 1))
		return 1;
	st = sd_wait(0x20, 500);
	if (!(st & 0x20) || (st & 0x8000U)) {
		con_hex2(" rd", "int=", st, "pst=", mmio32(HC(0x24)));
		sd_reset();
		return 2;
	}
	wr32(HC(0x30), 0x20);
	for (int i = 0; i < 128; i++)
		buf[i] = mmio32(HC(0x20));
	st = sd_wait(2, 500);
	wr32(HC(0x30), 0xffffffffU);
	return (st & 2) ? 0 : 3;
}

/*
 * sdhci-msm ignores the SDHCI clock divider: the card clock is GCC's SDCC1
 * apps clock (RCG at 0x1842004, MSM8916/8952 GCC layout). Rates as in the
 * GCC frequency tables: 400 kHz = XO 19.2 MHz / 12 * 1/4 (M/N), 50 MHz =
 * GPLL0 800 MHz / 16.
 */
#define GCC_SDCC1_CMD	0x01842004UL
#define GCC_SDCC1_CFG	0x01842008UL
#define GCC_SDCC1_M	0x0184200cUL
#define GCC_SDCC1_N	0x01842010UL
#define GCC_SDCC1_D	0x01842014UL
#define GCC_SDCC1_CBCR	0x01842018UL

static void sd_rate(int fast)
{
	wr32(HC(0x2c), mmio32(HC(0x2c)) & ~4U);		/* SD clock off while switching */
	if (fast) {
		wr32(GCC_SDCC1_CFG, (1U << 8) | 31);		/* GPLL0, /16, no M/N */
	} else {
		wr32(GCC_SDCC1_M, 1);
		wr32(GCC_SDCC1_N, ~(4U - 1) & 0xff);
		wr32(GCC_SDCC1_D, ~4U & 0xff);
		wr32(GCC_SDCC1_CFG, (2U << 12) | (0U << 8) | 23);	/* dual edge M/N, XO, /12 */
	}
	wr32(GCC_SDCC1_CMD, mmio32(GCC_SDCC1_CMD) | 1);	/* UPDATE */
	for (int i = 0; i < 100 && (mmio32(GCC_SDCC1_CMD) & 1); i++)
		delay_ms(1);
	wr32(HC(0x2c), mmio32(HC(0x2c)) | 4);
	delay_ms(2);
	con_hex2(" rate", "cmd=", mmio32(GCC_SDCC1_CMD), "cfg=", mmio32(GCC_SDCC1_CFG));
}

/* acknowledge sdhci-msm power-control requests (core PWRCTL status 0xdc, clear 0xe4, ctl 0xe8) */
static void sd_pwrack(const char *what)
{
	uint32_t ps = 0;

	for (int i = 0; i < 50 && !(ps = mmio32(SDCORE_BASE + 0xdc)); i++)
		delay_ms(1);
	if (ps) {
		wr32(SDCORE_BASE + 0xe4, ps);
		wr32(SDCORE_BASE + 0xe8, ((ps & 3) ? 1 : 0) | ((ps & 0xc) ? 4 : 0));
	}
	con_hex2(" pwrack", what, 0, "st=", ps);
}

static void test_mmc(void)
{
	static uint32_t sec[128];
	uint32_t r = 0, st, rca = 0;

	con_hex2("MMC", "mode=", mmio32(SDCORE_BASE + 0x78), "pst=", mmio32(HC(0x24)));
	con_hex2(" vend", "spec=", mmio32(HC(0x10c)), "pwrst=", mmio32(SDCORE_BASE + 0xdc));
	wr32(HC(0x10c), 0xa1c);			/* VENDOR_SPECIFIC_FUNC default, as LK's sdhci_msm_init */
	/* as LK's sdhci_mode_enable + sdhci_init: HC mode, FF_CLK_SW_RST_DIS, power IRQs, reset all */
	wr32(SDCORE_BASE + 0x78, mmio32(SDCORE_BASE + 0x78) | 1 | (1U << 13));
	wr32(SDCORE_BASE + 0xe0, 0xf);
	delay_ms(2);
	wr8(HC(0x2f), 1);				/* SOFT_RESET_ALL */
	for (int i = 0; i < 100 && (mmio32(HC(0x2c)) & 0x01000000U); i++)
		delay_ms(1);
	sd_pwrack("rst");
	wr32(HC(0x11c), mmio32(HC(0x40)) | (1U << 26));	/* VENDOR_CAPABILITIES0: 1.8 V */
	con_hex2(" caps", "cap=", mmio32(HC(0x40)), "spec=", mmio32(HC(0x10c)));
	wr32(HC(0x38), 0);				/* polled */
	wr32(HC(0x34), 0x03ff0033U);
	wr32(HC(0x30), 0xffffffffU);
	wr8(HC(0x29), 0x0b);				/* bus power on, 1.8 V */
	sd_pwrack("on");
	wr8(HC(0x28), 0);				/* 1-bit, normal speed, no DMA */
	wr32(HC(0x3c), mmio32(HC(0x3c)) & 0x0000ffffU);	/* HOST_CTRL2 0 */
	con_hex2(" gcc", "cfg=", mmio32(GCC_SDCC1_CFG), "cbcr=", mmio32(GCC_SDCC1_CBCR));
	wr32(HC(0x2c), (0x0eU << 16) | 1);			/* internal clock, timeout */
	for (int i = 0; i < 100 && !(mmio32(HC(0x2c)) & 2); i++)
		delay_ms(1);
	sd_rate(0);
	con_hex2(" hc", "pwr=", mmio32(HC(0x28)), "clk=", mmio32(HC(0x2c)));
	con_hex2(" pst", "pst=", mmio32(HC(0x24)), "ctl2=", mmio32(HC(0x3c)));
	for (uint32_t a = 1; a <= 3 && !rca; a++) {
		st = sd_cmd(13, a << 16, R48, 0, &r);
		con_hex2(" CMD13", "int=", st, "r1=", r);
		if ((st & 1) && !(st & 0x8000U) && ((r >> 9) & 15) == 4)
			rca = a;
	}
	for (uint32_t a = 1; a <= 3 && !rca; a++) {
		st = sd_cmd(5, a << 16, R48B, 0, &r);	/* awake: bit 15 = 0 */
		con_hex2(" CMD5", "int=", st, "r1=", r);
		if (!(st & 1) || (st & 0x8000U))
			continue;
		st = sd_cmd(7, a << 16, R48B, 0, &r);
		con_hex2(" CMD7", "int=", st, "r1=", r);
		st = sd_cmd(13, a << 16, R48, 0, &r);
		con_hex2(" CMD13", "int=", st, "r1=", r);
		if ((st & 1) && !(st & 0x8000U) && ((r >> 9) & 15) == 4)
			rca = a;
	}
	if (!rca) {
		/* full init: CMD0, CMD1 until ready, CMD2, CMD3 (RCA 1), CMD7 */
		uint32_t ocr = 0;
		wr8(HC(0x28), mmio32(HC(0x28)) & ~0x22U);	/* 1-bit bus */
		st = sd_cmd(0, 0, 0, 0, &r);
		con_hex2(" CMD0", "int=", st, 0, 0);
		delay_ms(2);
		for (int i = 0; i < 1000; i++) {
			st = sd_cmd(1, 0x40ff8080U, 0x02, 0, &ocr);
			if ((st & 1) && (ocr & 0x80000000U))
				break;
			delay_ms(1);
		}
		con_hex2(" CMD1", "int=", st, "ocr=", ocr);
		st = sd_cmd(2, 0, 0x09, 0, &r);
		con_hex2(" CMD2", "int=", st, "cid=", r);
		st = sd_cmd(3, 1U << 16, R48, 0, &r);
		con_hex2(" CMD3", "int=", st, "r1=", r);
		st = sd_cmd(7, 1U << 16, R48B, 0, &r);
		con_hex2(" CMD7", "int=", st, "r1=", r);
		st = sd_cmd(13, 1U << 16, R48, 0, &r);
		con_hex2(" CMD13", "int=", st, "r1=", r);
		if ((st & 1) && !(st & 0x8000U) && ((r >> 9) & 15) == 4)
			rca = 1;
	}
	if (rca) {
		/* high speed (EXT_CSD[185] = 1) first: the card may still be in HS200; then 8-bit, 50 MHz */
		st = sd_cmd(6, (3U << 24) | (185U << 16) | (1U << 8), R48B, 0, &r);
		con_hex2(" HS", "int=", st, "r1=", r);
		st = sd_cmd(6, (3U << 24) | (183U << 16) | (2U << 8), R48B, 0, &r);
		con_hex2(" W8", "int=", st, "r1=", r);
		wr8(HC(0x28), 0x24);			/* 8-bit, high speed */
		sd_rate(1);
		st = sd_cmd(13, rca << 16, R48, 0, &r);
		con_hex2(" CMD13", "int=", st, "r1=", r);
	}
	con_hex2("MMC", "rca=", rca, "clk=", mmio32(HC(0x2c)));
	if (!rca)
		return;
	st = sd_read(1, sec);
	con_hex2(" LBA1", "err=", st, "w0=", sec[0]);
	con_hex2(" ", "w1=", sec[1], "parts=", sec[20]);
	st = sd_read(1179648 + 2, sec);	/* the iOS root's HFS+ volume header (k88push.py) */
	con_hex2(" HFS", "err=", st, "sig=", sec[0]);
	con_hex2(" ", "blksz=", sec[10], "blocks=", sec[11]);
}

static uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

/* ---- XNU loader (PAYLOAD=... build.sh) ---- */

/* payload.S (build.sh): the SWEETXNU payload linked into this Image, or nothing. */
extern const char k88_payload[];
extern const uint64_t k88_payload_size;

/* LK turns the caches off before the jump; clean+invalidate anyway, as on the A8. */
void loader_hook_dst(uintptr_t pa, uint64_t len)
{
	uint64_t line = 4u << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = pa & ~(line - 1); a < pa + len; a += line)
		__asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void k88_boot_xnu(int fb32)
{
	con_str("K88 SHIM", "xnu loader");
	con_hex2("CPU", "el=", rsr("CurrentEL") >> 2, "mpidr=", rsr("mpidr_el1"));
	con_hex2("FB32", "ok=", fb32, "pa=", fb_pa);
	con_hex2("PAYLOAD", "at=", (uintptr_t)k88_payload, "size=", (uint32_t)k88_payload_size);
	const char *why = loader_boot_payload((uintptr_t)k88_payload, (uintptr_t)k88_payload + k88_payload_size);
	con_str("LOADER", why);
	finish();
}

void k88_main(uintptr_t dtb)
{
	uint64_t s = 0;
	int fb32;

	log_init();
	con_tee("K88 SHIM started\n");
	find_scanout();
	/* the scanout as LK left it, before we change it */
	if (fb_pipe >= 0) {
		uintptr_t p = MDSS_BASE + pipe_off[fb_pipe % 4];
		char line[] = "LKPIPE n";

		line[7] = '0' + fb_pipe;
		con_tee(line);
		con_hex2(" ", "size=", mmio32(p + SSPP_SRC_SIZE), "out=", mmio32(p + SSPP_OUT_SIZE));
		con_hex2(" ", "xy=", mmio32(p + SSPP_SRC_XY), "op=", mmio32(p + SSPP_SRC_OP_MODE));
	}
	fb32 = scanout_32bpp();
	con_clear(BLUE);
	if (k88_payload_size)
		k88_boot_xnu(fb32);
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
	con_hex2("FB32", "ok=", fb32, "ctl=", fb32_ctl);
	con_hex2(" addr", "was=", fb32_before, "now=", fb32_after);
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
	k88_present();
	test_mmc();
	k88_present();
	finish();
}

void k88_exc(uint64_t slot, uint64_t *regs)
{
	static int nested;

	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	if (nested++) {
		/* faulted again while reporting (e.g. in k88_present): log only, then reboot */
		con_tee("EXC again while reporting: ");
		con_hex("ELR", rsr("elr_el1"));
		reboot_twrp();
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
	finish();
}
