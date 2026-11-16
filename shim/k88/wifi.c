/*
 * ZTE K88 Wi-Fi bring-up (WCNSS / Pronto), built into diag.c with -DK88_WIFI.
 * Copyright (c) 2026 Darin Newman. Written for ios-sweet; no third-party code.
 * Register addresses and call numbers are interface facts from the K88's own
 * device tree (docs/WIFI_K88.md) and the published SCM/SMEM/SMD interfaces.
 *
 * Stage 1 (this file, read-mostly probe, needs no firmware):
 *   - TrustZone SCM: which calling convention answers, whether the PIL
 *     calls exist, whether PAS supports peripheral 6 (Pronto);
 *   - the RPM-owned PM8950 rails the Wi-Fi core and its RF chip use
 *     (SPMI reads only);
 *   - SMEM: header, partition table, the APPS<->WCNSS partition, any SMD
 *     channel already allocated on edge 6.
 * Nothing here writes outside our own log; the only SMCs are queries.
 *
 * Included from diag.c (uses its static helpers: mmio32, wr32, delay_ms,
 * con_hex2, con_str, spmi_rd).
 */

/* ---- SCM (TrustZone) ---- */

#define SCM_OWNER_SIP		(2u << 24)
#define SCM_FN(svc, cmd)	((((svc) & 0xffu) << 8) | ((cmd) & 0xffu))
#define SCM_FAST		0x80000000u
#define SCM_64			0x40000000u
#define SCM_INTERRUPTED		1

struct scm_res { int64_t r0; uint64_t r1, r2, r3; };

static uint32_t scm_conv;	/* SCM_64 or 0 (32-bit convention); ~0 = none answered */

static struct scm_res scm_raw(uint64_t fn, uint64_t info, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = info;
	register uint64_t x2 __asm__("x2") = a0;
	register uint64_t x3 __asm__("x3") = a1;
	register uint64_t x4 __asm__("x4") = a2;
	register uint64_t x5 __asm__("x5") = a3;
	register uint64_t x6 __asm__("x6") = 0;
	struct scm_res r;

	for (;;) {
		__asm__ volatile("smc #0"
				 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6)
				 :
				 : "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
				   "x16", "x17", "memory");
		if (x0 != SCM_INTERRUPTED)
			break;
		/* TZ was interrupted: call again with x0 = 1, x6 = its session word, other args as returned */
		x0 = SCM_INTERRUPTED;
	}
	r.r0 = (int64_t)x0;
	r.r1 = x1;
	r.r2 = x2;
	r.r3 = x3;
	return r;
}

/* nargs plus 2-bit types per arg (0 = value, 1 = RO buffer, 2 = RW buffer) */
#define SCM_ARGS(n, t0, t1)	((n) | ((t0) << 4) | ((t1) << 6))

static struct scm_res scm_call(int fast, uint32_t svc, uint32_t cmd, uint32_t info,
			       uint64_t a0, uint64_t a1, uint64_t a2)
{
	uint64_t fn = (fast ? SCM_FAST : 0) | scm_conv | SCM_OWNER_SIP | SCM_FN(svc, cmd);

	return scm_raw(fn, info, a0, a1, a2, 0);
}

static int scm_avail(uint32_t svc, uint32_t cmd)
{
	struct scm_res r = scm_call(1, 6, 1, SCM_ARGS(1, 0, 0), SCM_FN(svc, cmd) | SCM_OWNER_SIP, 0, 0);

	return r.r0 ? -1 : (int)(r.r1 & 1);
}

static void scm_probe(void)
{
	struct scm_res r;

	for (int i = 0; i < 2; i++) {
		scm_conv = i == 0 ? SCM_64 : 0;
		r = scm_call(1, 6, 1, SCM_ARGS(1, 0, 0), SCM_FN(6, 1) | SCM_OWNER_SIP, 0, 0);
		con_hex2(i == 0 ? "SCM64" : "SCM32", "r0=", (uint32_t)r.r0, "r1=", (uint32_t)r.r1);
		if (r.r0 == 0 && r.r1 == 1)
			break;
		scm_conv = ~0u;
	}
	if (scm_conv == ~0u) {
		con_str("SCM", "no SMCCC convention answered (legacy TZ?)");
		return;
	}
	con_hex2("PIL av", "init=", scm_avail(2, 1), "mem=", scm_avail(2, 2));
	con_hex2("PIL av", "auth=", scm_avail(2, 5), "shut=", scm_avail(2, 6));
	con_hex2("PIL av", "supp=", scm_avail(2, 7), "mssrst=", scm_avail(2, 10));
	r = scm_call(0, 2, 7, SCM_ARGS(1, 0, 0), 6, 0, 0);
	con_hex2("PAS 6", "r0=", (uint32_t)r.r0, "supported=", (uint32_t)r.r1);
	r = scm_call(0, 2, 7, SCM_ARGS(1, 0, 0), 1, 0, 0);
	con_hex2("PAS 1", "r0=", (uint32_t)r.r0, "supported=", (uint32_t)r.r1);
}

/* ---- PM8950 rails (RPM-owned, read only) ---- */

struct rail { char name[8]; uint16_t addr; };
static const struct rail wifi_rails[] = {
	{ "S2 cx", 0x1700 }, { "L3 mx", 0x4200 }, { "L5 px", 0x4400 },
	{ "L7 xo", 0x4600 }, { "L9 pa", 0x4800 }, { "L19 rfa", 0x5200 },
};

static void rail_probe(void)
{
	for (unsigned i = 0; i < sizeof(wifi_rails) / sizeof(wifi_rails[0]); i++) {
		unsigned a = wifi_rails[i].addr;
		/* 0x04 type, 0x05 subtype, 0x08 status (bit 7 = output OK), 0x40/0x41 voltage, 0x46 enable (bit 7) */
		uint32_t id = spmi_rd(1, a + 4) | spmi_rd(1, a + 5) << 8 | spmi_rd(1, a + 8) << 16;
		uint32_t v = spmi_rd(1, a + 0x40) | spmi_rd(1, a + 0x41) << 8 | spmi_rd(1, a + 0x45) << 16 |
			     spmi_rd(1, a + 0x46) << 24;

		con_hex2(wifi_rails[i].name, "tst=", id, "vme=", v);
	}
}

/* ---- SMEM ---- */

#define SMEM_BASE	0x86300000UL
#define SMEM_SIZE	0x00100000UL
#define SMEM_TOC_MAGIC	0x434f5424u	/* "$TOC" */
#define SMEM_PART_MAGIC	0x54525024u	/* "$PRT" */
#define WCNSS_PID	4		/* SMEM host id of WCNSS (smp2p-wcnss remote-pid) */
#define SMD_EDGE_WCNSS	6

static uintptr_t smem_part;	/* APPS<->WCNSS partition header, 0 = none */
static uint32_t smem_part_size;

/* find an item in a private partition (uncached list); returns its address or 0 */
static uintptr_t smem_part_item(uintptr_t part, uint32_t item, uint32_t *size)
{
	uint32_t end = mmio32(part + 12);	/* offset_free_uncached */
	uintptr_t e = part + 0x20;		/* first uncached entry, after the 32-byte header */

	while (e < part + end) {
		uint32_t w0 = mmio32(e), sz = mmio32(e + 4), pad = mmio32(e + 8);
		uint32_t hdrpad = pad >> 16, datapad = pad & 0xffff;

		if ((w0 & 0xffff) != 0xa5a5)
			return 0;	/* canary broken */
		if ((w0 >> 16) == item) {
			if (size)
				*size = sz - datapad;
			return e + 16 + hdrpad;
		}
		e += 16 + hdrpad + sz;
	}
	return 0;
}

static uintptr_t smem_get(uint32_t item, uint32_t *size)
{
	if (smem_part)
		return smem_part_item(smem_part, item, size);
	/* legacy global heap: toc[item] after proc_comm[4] (64 B) + version[32] (128 B) + 4 words */
	uintptr_t t = SMEM_BASE + 64 + 128 + 16 + 16 * item;

	if (item >= 512 || !(mmio32(t) & 1))
		return 0;
	if (size)
		*size = mmio32(t + 8);
	return SMEM_BASE + mmio32(t + 4);
}

static void smem_probe(void)
{
	uintptr_t tab = SMEM_BASE + SMEM_SIZE - 0x1000;
	uint32_t n;

	con_hex2("SMEM", "ver7=", mmio32(SMEM_BASE + 64 + 4 * 7), "init=", mmio32(SMEM_BASE + 64 + 128));
	con_hex2(" heap", "free=", mmio32(SMEM_BASE + 64 + 132), "avail=", mmio32(SMEM_BASE + 64 + 136));
	con_hex2(" toc", "magic=", mmio32(tab), "n=", mmio32(tab + 8));
	if (mmio32(tab) != SMEM_TOC_MAGIC)
		goto chans;
	n = mmio32(tab + 8);
	for (uint32_t i = 0; i < n && i < 16; i++) {
		uintptr_t e = tab + 32 + 48 * i;	/* offset, size, flags, host0 + host1 << 16, cacheline, 7 reserved words */
		uint32_t hosts = mmio32(e + 12);

		con_hex2(" part", "off=", mmio32(e), "hosts=", hosts);
		if (((hosts & 0xffff) == 0 && (hosts >> 16) == WCNSS_PID) ||
		    ((hosts >> 16) == 0 && (hosts & 0xffff) == WCNSS_PID)) {
			smem_part = SMEM_BASE + mmio32(e);
			smem_part_size = mmio32(e + 4);
		}
	}
	if (smem_part)
		con_hex2(" wcnss", "magic=", mmio32(smem_part), "free=", mmio32(smem_part + 12));
chans:
	/* SMD allocation tables: item 13 (and 266), 64 entries of name[20], cid, flags, ref */
	for (int t = 0; t < 2; t++) {
		uint32_t sz = 0;
		uintptr_t a = smem_get(t ? 266 : 13, &sz);

		con_hex2(t ? " tbl266" : " tbl13", "pa=", (uint32_t)a, "size=", sz);
		for (int i = 0; a && i < 64; i++) {
			uintptr_t e = a + 32 * i;
			uint32_t flags = mmio32(e + 24);
			char name[21];

			if (!mmio32(e + 28) || !mmio32(e))
				continue;
			for (int k = 0; k < 20; k++)
				name[k] = *(volatile char *)(e + k);
			name[20] = 0;
			con_hex2(name, "cid=", mmio32(e + 20), "flags=", flags);
		}
	}
}


/* ---- stage 2: boot Pronto through TrustZone PAS (needs WIFIBLOB) ---- */

extern const char k88_wifiblob[], k88_wifiblob_end[];

#define PAS_WCNSS	6
#define META_PA		0x9e000000UL	/* 4 KiB aligned scratch in plain RAM for the PAS metadata */
#define PRONTO_PMU	0x0a21b000UL	/* qcom,pronto reg: PMU block */
#define PMU_CFG		(PRONTO_PMU + 0x1004)
#define PMU_SPARE	(PRONTO_PMU + 0x1088)
#define  SPARE_NVBIN_DLND	(1u << 25)
#define  PMU_XO_CFG		(1u << 3)
#define  PMU_XO_EN		(1u << 4)
#define  PMU_GC_BUS_MUX_TOP	(1u << 5)
#define  PMU_XO_CFG_STS		(1u << 6)
#define  PMU_IRIS_RESET		(1u << 7)
#define  PMU_IRIS_RESET_STS	(1u << 8)
#define APCS_IPC	0x0b011008UL
#define IPC_WCNSS_SMD	(1u << 17)

static uint32_t blob32(uint32_t off) { return *(const volatile uint32_t *)(k88_wifiblob + off); }

static void copy_out(uintptr_t dst, const char *src, uint32_t n)
{
	/* the MMU is off, so RAM is Device memory: aligned word stores, then bytes */
	uint32_t i = 0;

	if (!((dst | (uintptr_t)src) & 3))
		for (; i + 4 <= n; i += 4)
			*(volatile uint32_t *)(dst + i) = *(const volatile uint32_t *)(src + i);
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = *(const volatile uint8_t *)(src + i);
}

static void zero_out(uintptr_t dst, uint32_t n)
{
	uint32_t i = 0;

	for (; i < n && ((dst + i) & 3); i++)
		*(volatile uint8_t *)(dst + i) = 0;
	for (; i + 4 <= n; i += 4)
		*(volatile uint32_t *)(dst + i) = 0;
	for (; i < n; i++)
		*(volatile uint8_t *)(dst + i) = 0;
}

static int wait_clear(uintptr_t reg, uint32_t bit, unsigned ms)
{
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;

	while (mmio32(reg) & bit)
		if (now() > end)
			return -1;
	return 0;
}

/*
 * PMU: NV-download capability bit, then the Iris (RF chip) XO set-up before the core
 * starts: XO enable at 19.2 MHz, Iris reset, XO configuration.
 */
static void iris_xo_setup(void)
{
	uint32_t v;
	int r1, r2;

	con_hex2("PMU", "cfg=", mmio32(PMU_CFG), "spare=", mmio32(PMU_SPARE));
	wr32(PMU_SPARE, mmio32(PMU_SPARE) | SPARE_NVBIN_DLND);
	wr32(PMU_CFG, 0);
	v = PMU_GC_BUS_MUX_TOP | PMU_XO_EN;	/* XO mode bits 2:1 = 0: 19.2 MHz */
	wr32(PMU_CFG, v);
	wr32(PMU_CFG, v | PMU_IRIS_RESET);
	r1 = wait_clear(PMU_CFG, PMU_IRIS_RESET_STS, 100);
	wr32(PMU_CFG, v);
	wr32(PMU_CFG, v | PMU_XO_CFG);
	r2 = wait_clear(PMU_CFG, PMU_XO_CFG_STS, 100);
	wr32(PMU_CFG, v & ~(PMU_GC_BUS_MUX_TOP | PMU_XO_CFG));
	delay_ms(20);
	con_hex2(" iris", "rst=", (uint32_t)r1, "xocfg=", (uint32_t)r2);
	con_hex2(" now", "cfg=", mmio32(PMU_CFG), "spare=", mmio32(PMU_SPARE));
}

static int pronto_boot(void)
{
	uint32_t nseg = blob32(8), meta_off = blob32(12), meta_len = blob32(16);
	uint32_t lo = blob32(28), hi = blob32(32);
	struct scm_res r;

	if (k88_wifiblob_end - k88_wifiblob < 64 || blob32(0) != 0x5738384bu) {	/* "K88W" */
		con_str("PIL", "no WIFIBLOB linked in");
		return -1;
	}
	con_hex2("BLOB", "segs=", nseg, "meta=", meta_len);
	con_hex2(" range", "lo=", lo, "hi=", hi);
	copy_out(META_PA, k88_wifiblob + meta_off, meta_len);
	__asm__ volatile("dsb sy" ::: "memory");
	r = scm_call(0, 2, 1, SCM_ARGS(2, 0, 2), PAS_WCNSS, META_PA, 0);
	con_hex2("PAS init", "r0=", (uint32_t)r.r0, "r1=", (uint32_t)r.r1);
	if (r.r0 || r.r1)
		return -2;
	r = scm_call(0, 2, 2, SCM_ARGS(3, 0, 0), PAS_WCNSS, lo, hi - lo);
	con_hex2("PAS mem", "r0=", (uint32_t)r.r0, "r1=", (uint32_t)r.r1);
	if (r.r0 || r.r1)
		return -3;
	for (uint32_t i = 0; i < nseg; i++) {
		uint32_t pa = blob32(40 + 16 * i), fsz = blob32(44 + 16 * i);
		uint32_t msz = blob32(48 + 16 * i), off = blob32(52 + 16 * i);

		copy_out(pa, k88_wifiblob + off, fsz);
		zero_out(pa + fsz, msz - fsz);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	con_str("PIL", "segments copied");
	k88_present();
	iris_xo_setup();
	k88_present();
	r = scm_call(0, 2, 5, SCM_ARGS(1, 0, 0), PAS_WCNSS, 0, 0);
	con_hex2("PAS auth", "r0=", (uint32_t)r.r0, "r1=", (uint32_t)r.r1);
	k88_present();
	return (r.r0 || r.r1) ? -4 : 0;
}

/* ---- SMD on edge 6 ---- */

struct smd_ch {
	uintptr_t info;		/* tx info at +0, rx info at +isz */
	uint32_t isz;		/* 20 (byte flags) or 44 (word flags) */
	uintptr_t txf, rxf;
	uint32_t fsz;
};

enum { F_DSR, F_CTS, F_CD, F_RI, F_HEAD, F_TAIL, F_STATE, F_BLOCKREADINTR };

static uintptr_t smd_side(struct smd_ch *c, int rx) { return c->info + (rx ? c->isz : 0); }
#define SMD_STATE(c, rx)	smd_side(c, rx)
#define SMD_TAIL(c, rx)		(smd_side(c, rx) + ((c)->isz == 44 ? 36 : 12))
#define SMD_HEAD(c, rx)		(smd_side(c, rx) + ((c)->isz == 44 ? 40 : 16))

static void smd_setflag(struct smd_ch *c, int rx, int n, uint32_t v)
{
	if (c->isz == 44)
		wr32(smd_side(c, rx) + 4 + 4 * n, v);
	else
		wr8(smd_side(c, rx) + 4 + n, (uint8_t)v);
}

static void smd_kick(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(APCS_IPC, IPC_WCNSS_SMD);
}

static void smd_set_state(struct smd_ch *c, uint32_t st)
{
	int open = st == 2;

	smd_setflag(c, 0, F_DSR, open);
	smd_setflag(c, 0, F_CTS, open);
	smd_setflag(c, 0, F_CD, open);
	wr32(SMD_STATE(c, 0), st);
	smd_setflag(c, 0, F_STATE, 1);
	smd_kick();
}

/* find channel WANT on edge 6 in the APPS<->WCNSS partition; 0 = found */
static int smd_find(const char *want, struct smd_ch *c)
{
	uint32_t sz = 0;
	uintptr_t a = smem_get(13, &sz);

	for (int i = 0; a && i < 64; i++) {
		uintptr_t e = a + 32 * i;
		int k;

		if (!mmio32(e + 28) || (mmio32(e + 24) & 0xff) != SMD_EDGE_WCNSS)
			continue;
		for (k = 0; want[k] && *(volatile char *)(e + k) == want[k]; k++)
			;
		if (want[k] || *(volatile char *)(e + k))
			continue;

		uint32_t cid = mmio32(e + 20), isz = 0, fsz = 0;
		uintptr_t f;

		c->info = smem_get(14 + cid, &isz);
		f = smem_get(338 + cid, &fsz);
		if (!c->info || !f)
			return -2;
		c->isz = isz / 2;
		c->fsz = fsz / 2;
		c->txf = f;
		c->rxf = f + c->fsz;
		con_hex2(want, "cid=", cid, "isz=", isz);
		con_hex2(" fifo", "pa=", (uint32_t)f, "size=", c->fsz);
		return 0;
	}
	return -1;
}

static int smd_open(struct smd_ch *c)
{
	/* reset our side, then OPENING (1), then OPENED (2); wait for the remote's OPENED */
	wr32(SMD_STATE(c, 0), 0);
	for (int n = F_DSR; n <= F_TAIL; n++)
		smd_setflag(c, 0, n, 0);
	smd_setflag(c, 0, F_STATE, 1);
	smd_setflag(c, 0, F_BLOCKREADINTR, 1);
	wr32(SMD_HEAD(c, 0), 0);
	wr32(SMD_TAIL(c, 1), 0);
	smd_kick();
	smd_set_state(c, 1);
	smd_set_state(c, 2);
	for (int i = 0; i < 2000; i++) {
		if (mmio32(SMD_STATE(c, 1)) == 2)
			return 0;
		delay_ms(1);
	}
	con_hex2("SMD open", "remote=", mmio32(SMD_STATE(c, 1)), "local=", mmio32(SMD_STATE(c, 0)));
	return -1;
}

static void fifo_put(struct smd_ch *c, uint32_t *head, const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		wr8(c->txf + *head, p[i]);
		*head = (*head + 1) & (c->fsz - 1);
	}
}

static int smd_send(struct smd_ch *c, const void *data, uint32_t len)
{
	uint32_t hdr[5] = { len, 0, 0, 0, 0 };
	uint32_t head = mmio32(SMD_HEAD(c, 0)), need = len + 20;

	for (int i = 0;; i++) {
		uint32_t tail = mmio32(SMD_TAIL(c, 0));
		uint32_t avail = (c->fsz - 1) - ((head - tail) & (c->fsz - 1));

		if (avail >= need)
			break;
		if (i == 1000)
			return -1;
		delay_ms(1);
	}
	fifo_put(c, &head, (const uint8_t *)hdr, 20);
	fifo_put(c, &head, data, len);
	__asm__ volatile("dsb sy" ::: "memory");
	smd_setflag(c, 0, F_HEAD, 1);
	wr32(SMD_HEAD(c, 0), head);
	smd_kick();
	return 0;
}

static uint8_t fifo_get8(struct smd_ch *c, uint32_t *tail)
{
	uint8_t v = *(volatile uint8_t *)(c->rxf + *tail);

	*tail = (*tail + 1) & (c->fsz - 1);
	return v;
}

/* one packet into buf (at most cap bytes kept); returns its length, -1 on timeout */
static int smd_recv(struct smd_ch *c, uint8_t *buf, uint32_t cap, unsigned ms)
{
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;
	uint32_t tail, len = 0;

	for (;;) {
		tail = mmio32(SMD_TAIL(c, 1));
		if (((mmio32(SMD_HEAD(c, 1)) - tail) & (c->fsz - 1)) >= 20)
			break;
		if (now() > end)
			return -1;
	}
	for (int i = 0; i < 20; i++) {
		uint8_t b = fifo_get8(c, &tail);

		if (i < 4)
			len |= (uint32_t)b << (8 * i);
	}
	while (((mmio32(SMD_HEAD(c, 1)) - tail) & (c->fsz - 1)) < len)
		if (now() > end)
			return -1;
	for (uint32_t i = 0; i < len; i++) {
		uint8_t b = fifo_get8(c, &tail);

		if (i < cap)
			buf[i] = b;
	}
	wr32(SMD_TAIL(c, 1), tail);
	smd_setflag(c, 1, F_TAIL, 1);
	smd_setflag(c, 1, F_HEAD, 0);
	smd_kick();
	return (int)len;
}

/* ---- WCNSS_CTRL: version, NV download ---- */

#define WCNSS_VERSION_REQ	0x01000000u
#define WCNSS_VERSION_RESP	0x01000001u
#define WCNSS_NV_REQ		0x01000002u
#define WCNSS_NV_RESP		0x01000003u
#define WCNSS_CBC_DONE		0x0100000cu
#define NV_FRAG			3072

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void ctrl_dump(const char *what, const uint8_t *b, int n)
{
	con_hex2(what, "type=", n >= 4 ? le32(b) : 0, "len=", (uint32_t)n);
	if (n >= 12)
		con_hex2(" body", "w2=", le32(b + 8), "w3=", n >= 16 ? le32(b + 12) : 0);
}

static uint32_t msgw[(16 + NV_FRAG) / 4];

static void wcnss_ctrl(void)
{
	uint8_t *msg = (uint8_t *)msgw, rx[64];
	struct smd_ch c;
	int n, i;

	for (i = 0; i < 5000; i++) {
		if (smd_find("WCNSS_CTRL", &c) == 0)
			break;
		delay_ms(1);
	}
	if (i == 5000) {
		con_str("SMD", "no WCNSS_CTRL channel after 5 s");
		return;
	}
	if (smd_open(&c))
		return;
	con_str("SMD", "WCNSS_CTRL open");
	/* anything the firmware sends on its own first */
	while ((n = smd_recv(&c, rx, sizeof(rx), 300)) >= 0)
		ctrl_dump("CTRL rx", rx, n);
	msgw[0] = WCNSS_VERSION_REQ;
	msgw[1] = 8;
	smd_send(&c, msg, 8);
	n = smd_recv(&c, rx, sizeof(rx), 3000);
	ctrl_dump("VERSION", rx, n);
	if (n == 12)
		con_hex2(" ver", "maj.min=", rx[8] << 8 | rx[9], "ver.rev=", rx[10] << 8 | rx[11]);
	k88_present();

	uint32_t nv_off = blob32(20), nv_len = blob32(24), seq = 0;

	for (uint32_t done = 0; done < nv_len; done += NV_FRAG, seq++) {
		uint32_t f = nv_len - done < NV_FRAG ? nv_len - done : NV_FRAG;

		msgw[0] = WCNSS_NV_REQ;
		msgw[1] = 16 + f;
		msgw[2] = seq | (done + f >= nv_len ? 1u << 16 : 0);	/* u16 seq, u16 last */
		msgw[3] = f;
		for (uint32_t k = 0; k < f; k++)
			msg[16 + k] = *(const volatile uint8_t *)(k88_wifiblob + nv_off + done + k);
		if (smd_send(&c, msg, 16 + f)) {
			con_hex2("NV", "tx full at seq=", seq, 0, 0);
			return;
		}
	}
	con_hex2("NV", "sent frags=", seq, "bytes=", nv_len);
	for (i = 0; i < 4; i++) {
		n = smd_recv(&c, rx, sizeof(rx), 5000);
		if (n < 0)
			break;
		ctrl_dump("CTRL rx", rx, n);
		if (n >= 9 && le32(rx) == WCNSS_NV_RESP)
			con_hex2(" nv", "status=", rx[8], 0, 0);
		if (n >= 4 && le32(rx) == WCNSS_CBC_DONE)
			con_str(" cbc", "cold boot calibration done");
	}
	k88_present();
}

static void list_channels(void)
{
	uint32_t sz = 0;
	uintptr_t a = smem_get(13, &sz);

	for (int i = 0; a && i < 64; i++) {
		uintptr_t e = a + 32 * i;
		char name[21];

		if (!mmio32(e + 28) || !mmio32(e))
			continue;
		for (int k = 0; k < 20; k++)
			name[k] = *(volatile char *)(e + k);
		name[20] = 0;
		con_hex2(name, "cid=", mmio32(e + 20), "flags=", mmio32(e + 24));
	}
}

/* crash reason string the firmware leaves in SMEM item 422 */
static void crash_reason(void)
{
	uint32_t sz = 0;
	uintptr_t a = smem_get(422, &sz), part = smem_part;
	char s[48];
	uint32_t k;

	if (!a) {	/* not in the WCNSS partition: try the global heap */
		smem_part = 0;
		a = smem_get(422, &sz);
		smem_part = part;
	}
	if (!a || !sz || !*(volatile char *)a)
		return;
	for (k = 0; k < sizeof(s) - 1 && k < sz && *(volatile char *)(a + k); k++)
		s[k] = *(volatile char *)(a + k);
	s[k] = 0;
	con_str("CRASH", s);
}

/* ---- top ---- */

static void test_wifi(void)
{
	con_str("WIFI", "stage 1 probe");
	con_hex2("GCC", "vote=", mmio32(0x01845004), 0, 0);
	k88_present();
	scm_probe();
	k88_present();
	rail_probe();
	k88_present();
	smem_probe();
	k88_present();
	if (scm_conv == ~0u)
		return;
	/* the Iris 5-wire bus: TLMM GPIO 76-80, function 1, pull-up, 6 mA (DT wcnss_default) */
	for (unsigned g = 76; g <= 80; g++)
		gpio_cfg(g, 1, 3, 6, 0);
	con_str("WIFI", "stage 2: PAS boot");
	if (pronto_boot() == 0) {
		/* wait for the firmware to allocate its SMD channels */
		for (int i = 0; i < 100 && !smem_get(13, 0); i++)
			delay_ms(50);
		con_hex2("SMEM", "tbl13=", (uint32_t)smem_get(13, 0), "free=", smem_part ? mmio32(smem_part + 12) : 0);
		delay_ms(500);
		list_channels();
		crash_reason();
		k88_present();
		wcnss_ctrl();
		list_channels();
		crash_reason();
		k88_present();
	}
	struct scm_res r = scm_call(0, 2, 6, SCM_ARGS(1, 0, 0), PAS_WCNSS, 0, 0);
	con_hex2("PAS shut", "r0=", (uint32_t)r.r0, "r1=", (uint32_t)r.r1);
}
