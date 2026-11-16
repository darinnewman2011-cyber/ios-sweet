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

/* ---- top ---- */

static void test_wifi(void)
{
	con_str("WIFI", "stage 1 probe");
	/* GCC APCS_CLOCK_BRANCH_ENA_VOTE: bits 0-2 = crypto AHB/AXI/core (TZ needs them for PAS auth) */
	con_hex2("GCC", "vote=", mmio32(0x01845004), 0, 0);
	k88_present();
	scm_probe();
	k88_present();
	rail_probe();
	k88_present();
	smem_probe();
	k88_present();
}
