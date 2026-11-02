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

/*
 * eMMC: Synopsys DW-MSHC v270a with Samsung extensions (dw_mmc.h,
 * dw_mmc-exynos.h in the dpu_7885-era kernel). S-Boot has already brought
 * the card up (it loads boot.img from it), so this only issues read-side
 * commands: CMD13 SEND_STATUS, CMD7 SELECT (only if the card is in standby)
 * and CMD17 READ_SINGLE_BLOCK, in PIO mode. Nothing that writes the card.
 */
#define MMC_BASE	0x13500000UL
#define MMC_CTRL	0x000
#define  CTRL_FIFO_RESET	(1u << 1)
#define  CTRL_DMA_ENABLE	(1u << 5)
#define  CTRL_USE_IDMAC		(1u << 25)
#define MMC_CTYPE	0x018
#define MMC_BLKSIZ	0x01c
#define MMC_BYTCNT	0x020
#define MMC_CMDARG	0x028
#define MMC_CMD		0x02c
#define  CMD_START		(1u << 31)
#define  CMD_USE_HOLD_REG	(1u << 29)	/* Samsung: set when CLKSEL drive phase != 0 */
#define  CMD_PRV_DAT_WAIT	(1u << 13)
#define  CMD_DAT_EXP		(1u << 9)
#define  CMD_RESP_CRC		(1u << 8)
#define  CMD_RESP_EXP		(1u << 6)
#define MMC_RESP0	0x030
#define MMC_RINTSTS	0x044
#define  INT_RXDR		(1u << 5)
#define  INT_DATA_OVER		(1u << 3)
#define  INT_CMD_DONE		(1u << 2)
#define  INT_RTO		(1u << 8)
#define  INT_ERROR		0xbfc2u		/* SDMMC_INT_ERROR */
#define MMC_STATUS	0x048
#define  STATUS_BUSY		(1u << 9)
#define  STATUS_FCNT(s)		(((s) >> 17) & 0x1fff)
#define MMC_VERID	0x06c
#define MMC_HCON	0x070
#define MMC_UHS_REG	0x074
#define MMC_BMOD	0x080
#define  BMOD_IDMAC_ENABLE	(1u << 7)
#define MMC_CLKSEL	0x0a4		/* CONFIG_MMC_DW_64BIT_DESC=y: after BUFADDRU */
#define MMC_FIFO	0x200		/* DATA_240A_OFFSET; 64-bit entries, read as two 32-bit words */
#define MMC_MPSEND0	0x1204		/* "protector" (SMU): secure view may fault, read last */
#define MMC_MPSCTRL0	0x120c

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
		con_hex2(label, "mpidr=", a8_mpidr[i], "ret=",
			 (uint32_t)smc(PSCI_CPU_ON, a8_mpidr[i], (uintptr_t)secondary_entry, i));
	}
	delay_ms(500);
	for (int i = 0; i < 12; i++)
		if (sec_mpidr[i])
			alive |= 1u << i;
	/* expected 0x3e3: slots 0,1 (A73), 5-7 (A53 0x101-0x103), 8,9 (A53 0x200/0x201) */
	con_hex2("SMP", "alive=", alive, "want=", 0x3e3);
}

/* ---- eMMC, read-only ---- */

static uint64_t mmc_blk[512 / 8];	/* one sector; 8-byte aligned for GPT fields */
static uint32_t mmc_hold;		/* CMD_USE_HOLD_REG or 0 */
static uint32_t mmc_fifo64;		/* 64-bit FIFO entries */

static uint32_t mmc_rd(uint32_t r)
{
	return mmio32(MMC_BASE + r);
}

static void mmc_wr(uint32_t r, uint32_t v)
{
	wr32(MMC_BASE + r, v);
}

/* Wait until (reg & mask) == want, up to ms milliseconds. */
static int mmc_wait(uint32_t r, uint32_t mask, uint32_t want, unsigned ms)
{
	uint64_t t0 = now(), lim = ticks_per_sec() / 1000 * ms;

	while ((mmc_rd(r) & mask) != want)
		if (now() - t0 > lim)
			return 0;
	return 1;
}

/* Send one command; returns RINTSTS (0xffffffff if the CIU never took it). */
static uint32_t mmc_cmd(uint32_t idx, uint32_t arg, uint32_t flags, uint32_t *resp)
{
	uint32_t st;

	mmc_wait(MMC_STATUS, STATUS_BUSY, 0, 100);
	mmc_wr(MMC_RINTSTS, 0xffffffff);
	mmc_wr(MMC_CMDARG, arg);
	__asm__ volatile("dsb sy" ::: "memory");
	mmc_wr(MMC_CMD, CMD_START | mmc_hold | flags | idx);
	if (!mmc_wait(MMC_CMD, CMD_START, 0, 10))
		return 0xffffffff;
	mmc_wait(MMC_RINTSTS, INT_CMD_DONE, INT_CMD_DONE, 100);
	st = mmc_rd(MMC_RINTSTS);
	*resp = mmc_rd(MMC_RESP0);
	return st;
}

/* CMD17 one 512-byte sector into mmc_blk through the FIFO; returns RINTSTS, 0 = no error bits. */
static uint32_t mmc_read(uint32_t lba)
{
	uint32_t st, resp, n = 0;
	uint64_t t0, lim = ticks_per_sec() / 2;
	volatile uint32_t *w = (volatile uint32_t *)mmc_blk;

	mmc_wr(MMC_CTRL, mmc_rd(MMC_CTRL) | CTRL_FIFO_RESET);
	mmc_wait(MMC_CTRL, CTRL_FIFO_RESET, 0, 10);
	mmc_wr(MMC_BLKSIZ, 512);
	mmc_wr(MMC_BYTCNT, 512);
	st = mmc_cmd(17, lba, CMD_RESP_EXP | CMD_RESP_CRC | CMD_DAT_EXP | CMD_PRV_DAT_WAIT, &resp);
	if (st == 0xffffffff || (st & INT_ERROR))
		return st;
	t0 = now();
	for (;;) {
		uint32_t s = mmc_rd(MMC_STATUS), r = mmc_rd(MMC_RINTSTS);

		/* FIFO count is in host-data-width entries (HCON[9:7]: 1 = 32-bit, 2 = 64-bit) */
		for (uint32_t c = STATUS_FCNT(s); c && n < 128; c--) {
			w[n++] = mmio32(MMC_BASE + MMC_FIFO);
			if (mmc_fifo64)
				w[n++] = mmio32(MMC_BASE + MMC_FIFO + 4);
		}
		mmc_wr(MMC_RINTSTS, INT_RXDR);
		if (r & INT_ERROR)
			return r | 0x80000000u;
		if ((r & INT_DATA_OVER) && n >= 128)
			return 0;
		if (now() - t0 > lim)
			return r | 0x40000000u;	/* timeout: shows what had arrived */
	}
}

static uint16_t blk16(unsigned off)
{
	return ((volatile uint16_t *)mmc_blk)[off / 2];
}

static uint32_t blk32(unsigned off)
{
	return ((volatile uint32_t *)mmc_blk)[off / 4];
}

static uint64_t blk64(unsigned off)
{
	return mmc_blk[off / 8];
}

static void test_mmc(void)
{
	uint32_t st, resp = 0, rca, state, ents_lba, nents, boot_lba = 0;

	con_hex2("MMC", "ctrl=", mmc_rd(MMC_CTRL), "verid=", mmc_rd(MMC_VERID));
	con_hex2("MMC2", "hcon=", mmc_rd(MMC_HCON), "clksel=", mmc_rd(MMC_CLKSEL));
	con_hex2("MMC3", "ctype=", mmc_rd(MMC_CTYPE), "uhs=", mmc_rd(MMC_UHS_REG));
	con_hex2("MMC4", "bmod=", mmc_rd(MMC_BMOD), "status=", mmc_rd(MMC_STATUS));
	a8_present();
	if ((mmc_rd(MMC_CLKSEL) >> 16) & 7)
		mmc_hold = CMD_USE_HOLD_REG;
	mmc_fifo64 = ((mmc_rd(MMC_HCON) >> 7) & 7) == 2;
	/* PIO: the FIFO is read by the CPU, no IDMAC descriptors */
	mmc_wr(MMC_CTRL, mmc_rd(MMC_CTRL) & ~(CTRL_USE_IDMAC | CTRL_DMA_ENABLE));
	mmc_wr(MMC_BMOD, mmc_rd(MMC_BMOD) & ~BMOD_IDMAC_ENABLE);

	/* S-Boot's RCA is unknown: the right one answers, the others time out (RTO) */
	for (rca = 1; rca <= 3; rca++) {
		st = mmc_cmd(13, rca << 16, CMD_RESP_EXP | CMD_RESP_CRC, &resp);
		if (st != 0xffffffff && !(st & INT_RTO))
			break;
	}
	state = (resp >> 9) & 0xf;	/* 3 stby, 4 tran */
	con_hex2("CARD", "rca=", rca, "r1=", resp);
	if (rca <= 3 && state == 3) {
		mmc_cmd(7, rca << 16, CMD_RESP_EXP | CMD_RESP_CRC, &resp);
		mmc_cmd(13, rca << 16, CMD_RESP_EXP | CMD_RESP_CRC, &resp);
		con_hex2("CMD7", "r1=", resp, 0, 0);
	}
	a8_present();

	st = mmc_read(0);
	con_hex2("LBA0", "rint=", st, "sig=", blk16(510));	/* protective MBR: 0xAA55 */
	a8_present();
	if (st)
		goto smu;
	st = mmc_read(1);
	con_hex2("GPT", "rint=", st, "magic=", blk32(0));	/* "EFI " = 0x20494645 */
	if (st || blk64(0) != 0x5452415020494645ULL)		/* "EFI PART" */
		goto smu;
	ents_lba = (uint32_t)blk64(72);
	nents = blk32(80);
	con_hex2("GPT2", "ents=", ents_lba, "n=", nents);
	/* find "BOOT" (UTF-16LE name at +56 in each 128-byte entry) */
	for (uint32_t i = 0; i < nents && i < 64 && !boot_lba; i++) {
		unsigned off = (i % 4) * 128;

		if (i % 4 == 0 && mmc_read(ents_lba + i / 4))
			break;
		if (blk16(off + 56) == 'B' && blk16(off + 58) == 'O' && blk16(off + 60) == 'O' &&
		    blk16(off + 62) == 'T' && blk16(off + 64) == 0)
			boot_lba = (uint32_t)blk64(off + 32);
	}
	con_hex2("BOOTPART", "lba=", boot_lba, 0, 0);
	a8_present();
	if (!boot_lba)
		goto smu;
	st = mmc_read(boot_lba);
	/* our own boot.img: "ANDR" = 0x52444e41, kernel_size = this shim's Image size */
	con_hex2("BOOTHDR", "magic=", blk32(0), "ksize=", blk32(8));
	if (st)
		con_hex2("BOOTRD", "rint=", st, 0, 0);
smu:
	con_str("SMU", "reading protector...");
	a8_present();	/* if this read faults, the red screen shows FAR */
	con_hex2("SMU", "ctrl0=", mmc_rd(MMC_MPSCTRL0), "end0=", mmc_rd(MMC_MPSEND0));
	a8_present();
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
	con_str("A8 SHIM", "milestone 3: emmc read");
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
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), 0, 0);
	test_smp();
	a8_present();
	test_mmc();
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
