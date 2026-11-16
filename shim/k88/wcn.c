/*
 * ZTE K88 Wi-Fi stage 3: WCN36xx HAL over SMD WLAN_CTRL, DXE receive rings, a passive scan.
 * Included from wifi.c (uses its SMD helpers and diag.c's console/log helpers).
 *
 * Copyright (c) 2026 Darin Newman.
 * The HAL message layouts come from wcn36xx_hal.h (Linux drivers/net/wireless/ath/wcn36xx/hal.h,
 * unmodified), and the start-up order, configuration values and DXE ring set-up follow the
 * Linux wcn36xx driver (smd.c, dxe.c, txrx.c), which carries this notice:
 *
 * Copyright (c) 2013 Eugene Krasnikov <k.eugene.e@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef uint16_t __le16;
typedef uint32_t __le32;
#ifndef __packed
#define __packed __attribute__((packed))
#endif
#define ETH_ALEN 6
#ifndef BIT
#define BIT(n) (1u << (n))
#endif
#include "wcn36xx_hal.h"


/* DXE register and descriptor values, from wcn36xx dxe.h (same notice as above) */
#define WCN36xx_DXE_CTRL_VLD		BIT(0)
#define WCN36xx_DXE_CTRL_EOP		BIT(3)
#define WCN36xx_DXE_CTRL_SIQ		BIT(5)
#define WCN36xx_DXE_CTRL_PDU_REL	BIT(8)
#define WCN36xx_DXE_CTRL_INT		BIT(17)
#define WCN36xx_DXE_CTRL_SWAP		BIT(20)
#define WCN36xx_DXE_CTRL_XTYPE_SET(x)	((x) << 1)
#define WCN36xx_DXE_CTRL_BTHLD_SEL_SET(x) ((x) << 9)
#define WCN36xx_DXE_CTRL_PRIO_SET(x)	((x) << 13)
#define WCN36xx_DXE_XTYPE_B2H		3
#define WCN36XX_DXE_CTRL_RX_L	(WCN36xx_DXE_CTRL_VLD | WCN36xx_DXE_CTRL_XTYPE_SET(WCN36xx_DXE_XTYPE_B2H) | 	WCN36xx_DXE_CTRL_EOP | WCN36xx_DXE_CTRL_SIQ | WCN36xx_DXE_CTRL_PDU_REL | 	WCN36xx_DXE_CTRL_BTHLD_SEL_SET(6) | WCN36xx_DXE_CTRL_PRIO_SET(5) | WCN36xx_DXE_CTRL_INT | 	WCN36xx_DXE_CTRL_SWAP)
#define WCN36XX_DXE_CTRL_RX_H	(WCN36xx_DXE_CTRL_VLD | WCN36xx_DXE_CTRL_XTYPE_SET(WCN36xx_DXE_XTYPE_B2H) | 	WCN36xx_DXE_CTRL_EOP | WCN36xx_DXE_CTRL_SIQ | WCN36xx_DXE_CTRL_PDU_REL | 	WCN36xx_DXE_CTRL_BTHLD_SEL_SET(8) | WCN36xx_DXE_CTRL_PRIO_SET(6) | WCN36xx_DXE_CTRL_INT | 	WCN36xx_DXE_CTRL_SWAP)
#define WCN36xx_DXE_CH_CTRL_EN		BIT(0)
#define WCN36xx_DXE_CH_CTRL_EOP		BIT(3)
#define WCN36xx_DXE_CH_CTRL_SIQ		BIT(5)
#define WCN36xx_DXE_CH_CTRL_PDU_REL	BIT(8)
#define WCN36xx_DXE_CH_CTRL_INE_ED	BIT(17)
#define WCN36xx_DXE_CH_CTRL_INE_ERR	BIT(18)
#define WCN36xx_DXE_CH_CTRL_INE_DONE	BIT(19)
#define WCN36xx_DXE_CH_CTRL_EDEN	BIT(20)
#define WCN36xx_DXE_CH_CTRL_EDVEN	BIT(21)
#define WCN36xx_DXE_CH_CTRL_ENDIANNESS	BIT(26)
#define WCN36xx_DXE_CH_CTRL_SWAP	BIT(31)
#define WCN36xx_DXE_CH_CTRL_XTYPE_SET(x) ((x) << 1)
#define WCN36xx_DXE_CH_CTRL_BTHLD_SEL_SET(x) ((x) << 9)
#define WCN36xx_DXE_CH_CTRL_PRIO_SET(x)	((x) << 13)
#define WCN36xx_DXE_CH_CTRL_SEL_SET(x)	((x) << 22)
#define WCN36XX_DXE_CH_DEFAULT_CTL_RX(bth, prio, sel) (WCN36xx_DXE_CH_CTRL_EN | 	WCN36xx_DXE_CH_CTRL_XTYPE_SET(WCN36xx_DXE_XTYPE_B2H) | WCN36xx_DXE_CH_CTRL_EOP | 	WCN36xx_DXE_CH_CTRL_SIQ | WCN36xx_DXE_CH_CTRL_PDU_REL | WCN36xx_DXE_CH_CTRL_BTHLD_SEL_SET(bth) | 	WCN36xx_DXE_CH_CTRL_PRIO_SET(prio) | WCN36xx_DXE_CH_CTRL_INE_ED | WCN36xx_DXE_CH_CTRL_INE_ERR | 	WCN36xx_DXE_CH_CTRL_INE_DONE | WCN36xx_DXE_CH_CTRL_EDEN | WCN36xx_DXE_CH_CTRL_EDVEN | 	WCN36xx_DXE_CH_CTRL_SEL_SET(sel) | WCN36xx_DXE_CH_CTRL_ENDIANNESS | WCN36xx_DXE_CH_CTRL_SWAP)
#define WCN36XX_DXE_CH_DEFAULT_CTL_RX_L	WCN36XX_DXE_CH_DEFAULT_CTL_RX(6, 5, 1)
#define WCN36XX_DXE_CH_DEFAULT_CTL_RX_H	WCN36XX_DXE_CH_DEFAULT_CTL_RX(8, 6, 3)
#define WCN36XX_DXE_WQ_RX_L		0xB
#define WCN36XX_DXE_WQ_RX_H		0x4
#define WCN36XX_DXE_REG_CSR_RESET	0x00
#define WCN36XX_DXE_ENCH_ADDR		0x04
#define WCN36XX_DXE_REG_CH_EN		0x08
#define WCN36XX_DXE_INT_MASK_REG	0x18
#define WCN36XX_DXE_INT_CH3_MASK	0x08
#define WCN36XX_DXE_INT_CH1_MASK	0x02
#define WCN36XX_DXE_INT_CH4_MASK	0x10
#define WCN36XX_DXE_INT_CH0_MASK	0x01
#define WCN36XX_DXE_0_INT_CLR		0x30
#define WCN36XX_DXE_0_INT_ED_CLR	0x34
#define WCN36XX_DXE_0_INT_DONE_CLR	0x38
#define WCN36XX_DXE_0_INT_ERR_CLR	0x3C
#define WCN36XX_CH_STAT_INT_DONE_MASK	0x00008000
#define WCN36XX_CH_STAT_INT_ERR_MASK	0x00004000
#define WCN36XX_CH_STAT_INT_ED_MASK	0x00002000
#define WCN36XX_DXE_REG_RESET		0x5c89
#define WCN36XX_DXE_RX_LOW_OFFSET	0x440
#define WCN36XX_DXE_RX_HIGH_OFFSET	0x4C0
#define WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_L	(WCN36XX_DXE_RX_LOW_OFFSET + 0x1C)
#define WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_H	(WCN36XX_DXE_RX_HIGH_OFFSET + 0x1C)
#define WCN36XX_DXE_CH_SRC_ADDR_RX_L	(WCN36XX_DXE_RX_LOW_OFFSET + 0x0C)
#define WCN36XX_DXE_CH_SRC_ADDR_RX_H	(WCN36XX_DXE_RX_HIGH_OFFSET + 0x0C)
#define WCN36XX_DXE_CH_DEST_ADDR_RX_L	(WCN36XX_DXE_RX_LOW_OFFSET + 0x14)
#define WCN36XX_DXE_CH_DEST_ADDR_RX_H	(WCN36XX_DXE_RX_HIGH_OFFSET + 0x14)
#define WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_L	(WCN36XX_DXE_RX_LOW_OFFSET + 0x04)
#define WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_H	(WCN36XX_DXE_RX_HIGH_OFFSET + 0x04)
#define WCN36XX_DXE_REG_CTL_RX_L	WCN36XX_DXE_RX_LOW_OFFSET
#define WCN36XX_DXE_REG_CTL_RX_H	WCN36XX_DXE_RX_HIGH_OFFSET
#define WCN36XX_INT_MASK_CHAN_RX_L	0x00000002
#define WCN36XX_INT_MASK_CHAN_RX_H	0x00000008
#define WCN36XX_CCU_DXE_INT_SELECT_PRONTO 0x10dc

/* ---- HAL request/response over WLAN_CTRL ---- */

static struct smd_ch wlan_ch;
static uint32_t halw[4096 / 4];		/* request buffer */
static uint32_t halrw[1024 / 4];	/* last response */
static uint8_t *const hal = (uint8_t *)halw;
static uint8_t *const halr = (uint8_t *)halrw;
static int halr_len;

static void hal_hdr(uint16_t type, uint32_t len)
{
	for (uint32_t i = 0; i < len && i < sizeof(halw); i++)
		hal[i] = 0;
	((struct wcn36xx_hal_msg_header *)hal)->msg_type = type;
	((struct wcn36xx_hal_msg_header *)hal)->msg_version = WCN36XX_HAL_MSG_VERSION0;
	((struct wcn36xx_hal_msg_header *)hal)->len = len;
}

/* send hal[] (length from its header) and wait for the response type+1; other messages are logged */
static int hal_call(unsigned ms)
{
	uint16_t want = ((struct wcn36xx_hal_msg_header *)hal)->msg_type + 1;
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;

	if (smd_send(&wlan_ch, hal, ((struct wcn36xx_hal_msg_header *)hal)->len))
		return -1;
	while (now() < end) {
		delay_ms(0);	/* pets the watchdog in K88_WDOG_S builds */
		halr_len = smd_recv(&wlan_ch, halr, sizeof(halrw), 50);
		if (halr_len < 0)
			continue;
		uint16_t t = halr[0] | halr[1] << 8;
		if (t == want)
			return 0;
		con_hex2("HAL ind", "type=", t, "len=", (uint32_t)halr_len);
	}
	con_hex2("HAL tmo", "want=", want, 0, 0);
	return -2;
}

static uint32_t hal_status(void)
{
	return halr_len >= 12 ? le32(halr + 8) : 0xffffffffu;
}

static int hal_load_nv(void)
{
	uint32_t nv_off = blob32(20) + 4, nv_len = blob32(24) - 4, frag = 0;	/* skip the is_valid word */

	for (uint32_t done = 0; done < nv_len; done += 3072, frag++) {
		uint32_t n = nv_len - done < 3072 ? nv_len - done : 3072;
		struct wcn36xx_hal_nv_img_download_req_msg *m = (void *)hal;

		hal_hdr(WCN36XX_HAL_DOWNLOAD_NV_REQ, sizeof(*m) + n);
		m->frag_number = frag;
		m->last_fragment = done + n >= nv_len;
		m->nv_img_buffer_size = n;
		for (uint32_t k = 0; k < n; k++)
			hal[sizeof(*m) + k] = *(const volatile uint8_t *)(k88_wifiblob + nv_off + done + k);
		if (hal_call(2000) || hal_status()) {
			con_hex2("HAL nv", "frag=", frag, "status=", hal_status());
			return -1;
		}
	}
	con_hex2("HAL nv", "frags=", frag, "ok=", 1);
	return 0;
}

static const uint32_t start_cfg[][2] = {
	{ WCN36XX_HAL_CFG_CURRENT_TX_ANTENNA, 1 }, { WCN36XX_HAL_CFG_CURRENT_RX_ANTENNA, 1 },
	{ WCN36XX_HAL_CFG_LOW_GAIN_OVERRIDE, 0 }, { WCN36XX_HAL_CFG_POWER_STATE_PER_CHAIN, 785 },
	{ WCN36XX_HAL_CFG_CAL_PERIOD, 5 }, { WCN36XX_HAL_CFG_CAL_CONTROL, 1 },
	{ WCN36XX_HAL_CFG_PROXIMITY, 0 }, { WCN36XX_HAL_CFG_NETWORK_DENSITY, 3 },
	{ WCN36XX_HAL_CFG_MAX_MEDIUM_TIME, 6000 }, { WCN36XX_HAL_CFG_MAX_MPDUS_IN_AMPDU, 64 },
	{ WCN36XX_HAL_CFG_RTS_THRESHOLD, 2347 }, { WCN36XX_HAL_CFG_SHORT_RETRY_LIMIT, 15 },
	{ WCN36XX_HAL_CFG_LONG_RETRY_LIMIT, 15 }, { WCN36XX_HAL_CFG_FRAGMENTATION_THRESHOLD, 8000 },
	{ WCN36XX_HAL_CFG_DYNAMIC_THRESHOLD_ZERO, 5 }, { WCN36XX_HAL_CFG_DYNAMIC_THRESHOLD_ONE, 10 },
	{ WCN36XX_HAL_CFG_DYNAMIC_THRESHOLD_TWO, 15 }, { WCN36XX_HAL_CFG_FIXED_RATE, 0 },
	{ WCN36XX_HAL_CFG_RETRYRATE_POLICY, 4 }, { WCN36XX_HAL_CFG_RETRYRATE_SECONDARY, 0 },
	{ WCN36XX_HAL_CFG_RETRYRATE_TERTIARY, 0 }, { WCN36XX_HAL_CFG_FORCE_POLICY_PROTECTION, 5 },
	{ WCN36XX_HAL_CFG_FIXED_RATE_MULTICAST_24GHZ, 1 }, { WCN36XX_HAL_CFG_FIXED_RATE_MULTICAST_5GHZ, 5 },
	{ WCN36XX_HAL_CFG_DEFAULT_RATE_INDEX_5GHZ, 5 }, { WCN36XX_HAL_CFG_MAX_BA_SESSIONS, 40 },
	{ WCN36XX_HAL_CFG_PS_DATA_INACTIVITY_TIMEOUT, 200 }, { WCN36XX_HAL_CFG_PS_ENABLE_BCN_FILTER, 1 },
	{ WCN36XX_HAL_CFG_PS_ENABLE_RSSI_MONITOR, 1 }, { WCN36XX_HAL_CFG_NUM_BEACON_PER_RSSI_AVERAGE, 20 },
	{ WCN36XX_HAL_CFG_STATS_PERIOD, 10 }, { WCN36XX_HAL_CFG_CFP_MAX_DURATION, 30000 },
	{ WCN36XX_HAL_CFG_FRAME_TRANS_ENABLED, 0 }, { WCN36XX_HAL_CFG_BA_THRESHOLD_HIGH, 128 },
	{ WCN36XX_HAL_CFG_MAX_BA_BUFFERS, 2560 }, { WCN36XX_HAL_CFG_DYNAMIC_PS_POLL_VALUE, 0 },
	{ WCN36XX_HAL_CFG_TX_PWR_CTRL_ENABLE, 1 }, { WCN36XX_HAL_CFG_ENABLE_CLOSE_LOOP, 1 },
	{ WCN36XX_HAL_CFG_ENABLE_LPWR_IMG_TRANSITION, 0 }, { WCN36XX_HAL_CFG_BTC_STATIC_LEN_LE_BT, 120000 },
	{ WCN36XX_HAL_CFG_BTC_STATIC_LEN_LE_WLAN, 30000 }, { WCN36XX_HAL_CFG_MAX_ASSOC_LIMIT, 10 },
	{ WCN36XX_HAL_CFG_ENABLE_MCC_ADAPTIVE_SCHEDULER, 0 }, { WCN36XX_HAL_CFG_ENABLE_DYNAMIC_RA_START_RATE, 133 },
	{ WCN36XX_HAL_CFG_LINK_FAIL_TX_CNT, 1000 },
};

static void con_text_n(const char *label, const uint8_t *s, int n)
{
	char b[48];
	int k;

	for (k = 0; k < n && k < (int)sizeof(b) - 1 && s[k]; k++)
		b[k] = s[k] >= 32 && s[k] < 127 ? (char)s[k] : '?';
	b[k] = 0;
	con_str(label, b);
}

static int hal_start(void)
{
	struct wcn36xx_hal_mac_start_req_msg *m = (void *)hal;
	uint32_t len = sizeof(*m);

	hal_hdr(WCN36XX_HAL_START_REQ, sizeof(*m));
	m->params.type = DRIVER_TYPE_PRODUCTION;
	for (unsigned i = 0; i < sizeof(start_cfg) / sizeof(start_cfg[0]); i++) {
		struct wcn36xx_hal_cfg *e = (void *)(hal + len);

		e->id = start_cfg[i][0];
		e->len = 4;
		e->pad_bytes = 0;
		e->reserve = 0;
		*(uint32_t *)(hal + len + sizeof(*e)) = start_cfg[i][1];
		len += sizeof(*e) + 4;
	}
	m->header.len = len;
	m->params.len = len - sizeof(*m);
	if (hal_call(5000))
		return -1;

	struct wcn36xx_hal_mac_start_rsp_msg *r = (void *)halr;

	con_hex2("HAL start", "status=", r->start_rsp_params.status, "len=", (uint32_t)halr_len);
	con_hex2(" fw", "api=", r->start_rsp_params.version.major << 24 | r->start_rsp_params.version.minor << 16 |
		 r->start_rsp_params.version.version << 8 | r->start_rsp_params.version.revision,
		 "sta/bss=", r->start_rsp_params.stations << 8 | r->start_rsp_params.bssids);
	con_text_n(" wlan", r->start_rsp_params.wlan_version, WCN36XX_HAL_VERSION_LENGTH);
	con_text_n(" crm", r->start_rsp_params.crm_version, WCN36XX_HAL_VERSION_LENGTH);
	return r->start_rsp_params.status ? -2 : 0;
}

static const uint8_t self_mac[6] = { 0x02, 0x00, 0x00, 0x88, 0x00, 0x01 };	/* locally administered */

static int hal_add_sta_self(void)
{
	struct wcn36xx_hal_add_sta_self_req *m = (void *)hal;

	hal_hdr(WCN36XX_HAL_ADD_STA_SELF_REQ, sizeof(*m));
	for (int i = 0; i < 6; i++)
		m->self_addr[i] = self_mac[i];
	if (hal_call(2000))
		return -1;
	con_hex2("HAL self", "status=", hal_status(), "idx=", halr_len >= 13 ? halr[12] : 0xff);
	return hal_status() ? -2 : 0;
}

static int hal_scan_msg(uint16_t type, uint8_t ch)
{
	if (type == WCN36XX_HAL_INIT_SCAN_REQ) {
		struct wcn36xx_hal_init_scan_req_msg *m = (void *)hal;

		hal_hdr(type, sizeof(*m));
		m->mode = HAL_SYS_MODE_SCAN;
	} else if (type == WCN36XX_HAL_FINISH_SCAN_REQ) {
		struct wcn36xx_hal_finish_scan_req_msg *m = (void *)hal;

		hal_hdr(type, sizeof(*m));
		m->mode = HAL_SYS_MODE_SCAN;
		m->oper_channel = ch;
	} else {
		struct wcn36xx_hal_start_scan_req_msg *m = (void *)hal;	/* same layout for END_SCAN */

		hal_hdr(type, sizeof(*m));
		m->scan_channel = ch;
	}
	if (hal_call(2000))
		return -1;
	return hal_status() ? (int)hal_status() : 0;
}

/* ---- DXE receive rings (polled) ---- */

#define DXE_BASE	0x0a202000UL	/* "dxe" */
#define CCU_BASE	0x0a204000UL	/* "ccu" */
#define DXE_DESC_PA	0x9d000000UL
#define DXE_BUF_PA	0x9d100000UL
#define RXL_N		64
#define RXH_N		16
#define PKT_SIZE	0xf20u

struct dxe_ring {
	uintptr_t desc;		/* first descriptor (32 bytes each) */
	uintptr_t buf;		/* first buffer (4 KiB each) */
	unsigned n, head;
	uint32_t ctrl, en_mask, int_mask, status_reg;
};

static struct dxe_ring rxl, rxh;
static uint32_t rx_frames, rx_bad;

static void dxe_ring_init(struct dxe_ring *r, uintptr_t desc, uintptr_t buf, unsigned n, uint32_t ctrl, uint32_t wq)
{
	r->desc = desc;
	r->buf = buf;
	r->n = n;
	r->head = 0;
	r->ctrl = ctrl;
	for (unsigned i = 0; i < n; i++) {
		uintptr_t d = desc + 32 * i;

		wr32(d + 0x00, ctrl);
		wr32(d + 0x04, 0);
		wr32(d + 0x08, wq);					/* src: the BMU work queue */
		wr32(d + 0x0c, (uint32_t)(buf + 0x1000 * i));		/* dst: our buffer */
		wr32(d + 0x10, (uint32_t)(desc + 32 * ((i + 1) % n)));	/* next */
		wr32(d + 0x14, 0);
		wr32(d + 0x18, 0);
		wr32(d + 0x1c, 0);
	}
}

static void dxe_init(void)
{
	wr32(DXE_BASE + WCN36XX_DXE_REG_CSR_RESET, WCN36XX_DXE_REG_RESET);
	wr32(CCU_BASE + WCN36XX_CCU_DXE_INT_SELECT_PRONTO,
	     (WCN36XX_DXE_INT_CH3_MASK | WCN36XX_DXE_INT_CH1_MASK) << 16 |
	     WCN36XX_DXE_INT_CH0_MASK | WCN36XX_DXE_INT_CH4_MASK);

	dxe_ring_init(&rxl, DXE_DESC_PA, DXE_BUF_PA, RXL_N, WCN36XX_DXE_CTRL_RX_L, WCN36XX_DXE_WQ_RX_L);
	rxl.en_mask = WCN36XX_DXE_INT_CH1_MASK;
	rxl.int_mask = WCN36XX_INT_MASK_CHAN_RX_L;
	rxl.status_reg = WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_L;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DXE_BASE + WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_L, (uint32_t)rxl.desc);
	wr32(DXE_BASE + WCN36XX_DXE_CH_SRC_ADDR_RX_L, WCN36XX_DXE_WQ_RX_L);
	wr32(DXE_BASE + WCN36XX_DXE_CH_DEST_ADDR_RX_L, mmio32(rxl.desc + 0x10));	/* as wcn36xx does */
	wr32(DXE_BASE + WCN36XX_DXE_REG_CTL_RX_L, WCN36XX_DXE_CH_DEFAULT_CTL_RX_L);

	dxe_ring_init(&rxh, DXE_DESC_PA + 32 * RXL_N, DXE_BUF_PA + 0x1000 * RXL_N, RXH_N,
		      WCN36XX_DXE_CTRL_RX_H, WCN36XX_DXE_WQ_RX_H);
	rxh.en_mask = WCN36XX_DXE_INT_CH3_MASK;
	rxh.int_mask = WCN36XX_INT_MASK_CHAN_RX_H;
	rxh.status_reg = WCN36XX_DXE_CH_STATUS_REG_ADDR_RX_H;
	__asm__ volatile("dsb sy" ::: "memory");
	wr32(DXE_BASE + WCN36XX_DXE_CH_NEXT_DESC_ADDR_RX_H, (uint32_t)rxh.desc);
	wr32(DXE_BASE + WCN36XX_DXE_CH_SRC_ADDR_RX_H, WCN36XX_DXE_WQ_RX_H);
	wr32(DXE_BASE + WCN36XX_DXE_CH_DEST_ADDR_RX_H, mmio32(rxh.desc + 0x10));
	wr32(DXE_BASE + WCN36XX_DXE_REG_CTL_RX_H, WCN36XX_DXE_CH_DEFAULT_CTL_RX_H);

	wr32(DXE_BASE + WCN36XX_DXE_INT_MASK_REG, mmio32(DXE_BASE + WCN36XX_DXE_INT_MASK_REG) |
	     WCN36XX_INT_MASK_CHAN_RX_L | WCN36XX_INT_MASK_CHAN_RX_H);
	con_hex2("DXE", "ch_en=", mmio32(DXE_BASE + WCN36XX_DXE_REG_CH_EN), "ctl_rxl=",
		 mmio32(DXE_BASE + WCN36XX_DXE_REG_CTL_RX_L));
}

/* ---- what the scan saw ---- */

struct bss_seen { uint8_t bssid[6], ch, ssid_len; int8_t rssi; uint8_t ssid[32]; };
static struct bss_seen seen[40];
static unsigned nseen;

static uint32_t be32w(uintptr_t a) { return __builtin_bswap32(mmio32(a)); }

static void rx_frame(uintptr_t b)
{
	uint32_t w3 = be32w(b + 12), w4 = be32w(b + 16), w7 = be32w(b + 28);
	uint32_t hoff = (w3 >> 16) & 0xff, len = w4 >> 16;
	uintptr_t f = b + hoff;
	uint8_t fc;

	rx_frames++;
	if (hoff < 76 || hoff + len > PKT_SIZE || len < 36) {
		rx_bad++;
		return;
	}
	fc = *(volatile uint8_t *)f;
	if (fc != 0x80 && fc != 0x50)	/* beacon, probe response */
		return;
	uint8_t bssid[6], ch = 0, sl = 0, ssid[32];
	for (int i = 0; i < 6; i++)
		bssid[i] = *(volatile uint8_t *)(f + 16 + i);
	for (uint32_t p = 36; p + 2 <= len;) {
		uint8_t id = *(volatile uint8_t *)(f + p), l = *(volatile uint8_t *)(f + p + 1);

		if (p + 2 + l > len)
			break;
		if (id == 0 && l <= 32) {
			sl = l;
			for (int i = 0; i < l; i++)
				ssid[i] = *(volatile uint8_t *)(f + p + 2 + i);
		} else if (id == 3 && l == 1) {
			ch = *(volatile uint8_t *)(f + p + 2);
		}
		p += 2 + l;
	}
	for (unsigned i = 0; i < nseen; i++) {
		int same = 1;
		for (int k = 0; k < 6; k++)
			same &= seen[i].bssid[k] == bssid[k];
		if (same)
			return;
	}
	if (nseen >= sizeof(seen) / sizeof(seen[0]))
		return;
	struct bss_seen *s = &seen[nseen++];
	for (int k = 0; k < 6; k++)
		s->bssid[k] = bssid[k];
	s->ch = ch;
	s->ssid_len = sl;
	s->rssi = (int8_t)-(100 - (int)((w7 >> 24) & 0xff));
	for (int k = 0; k < sl; k++)
		s->ssid[k] = ssid[k];
}

static void dxe_poll_ring(struct dxe_ring *r)
{
	uint32_t st = mmio32(DXE_BASE + r->status_reg);

	wr32(DXE_BASE + WCN36XX_DXE_0_INT_CLR, r->int_mask);
	if (st & WCN36XX_CH_STAT_INT_ERR_MASK)
		wr32(DXE_BASE + WCN36XX_DXE_0_INT_ERR_CLR, r->int_mask);
	if (st & WCN36XX_CH_STAT_INT_DONE_MASK)
		wr32(DXE_BASE + WCN36XX_DXE_0_INT_DONE_CLR, r->int_mask);
	if (st & WCN36XX_CH_STAT_INT_ED_MASK)
		wr32(DXE_BASE + WCN36XX_DXE_0_INT_ED_CLR, r->int_mask);
	for (unsigned k = 0; k < r->n; k++) {
		uintptr_t d = r->desc + 32 * r->head;

		if (mmio32(d) & WCN36xx_DXE_CTRL_VLD)
			break;
		rx_frame(r->buf + 0x1000 * r->head);
		wr32(d + 0x0c, (uint32_t)(r->buf + 0x1000 * r->head));
		__asm__ volatile("dsb sy" ::: "memory");
		wr32(d, r->ctrl);
		r->head = (r->head + 1) % r->n;
	}
	wr32(DXE_BASE + WCN36XX_DXE_ENCH_ADDR, r->en_mask);
}

static void rx_for(unsigned ms)
{
	uint64_t end = now() + ticks_per_sec() / 1000 * ms;

	while (now() < end) {
		delay_ms(0);
		dxe_poll_ring(&rxl);
		dxe_poll_ring(&rxh);
		/* drain indications so the control FIFO never fills */
		if (smd_recv(&wlan_ch, halr, sizeof(halrw), 0) >= 0)
			con_hex2("HAL ind", "type=", halr[0] | halr[1] << 8, "len=", 0);
	}
}

static void wlan_scan(void)
{
	int r = hal_scan_msg(WCN36XX_HAL_INIT_SCAN_REQ, 0);

	con_hex2("SCAN init", "r=", (uint32_t)r, 0, 0);
	for (uint8_t ch = 1; ch <= 13; ch++) {
		r = hal_scan_msg(WCN36XX_HAL_START_SCAN_REQ, ch);
		if (r) {
			con_hex2("SCAN start", "ch=", ch, "r=", (uint32_t)r);
			continue;
		}
		rx_for(150);
		hal_scan_msg(WCN36XX_HAL_END_SCAN_REQ, ch);
	}
	r = hal_scan_msg(WCN36XX_HAL_FINISH_SCAN_REQ, 1);
	con_hex2("SCAN done", "r=", (uint32_t)r, "frames=", rx_frames);
	con_hex2(" ", "bad=", rx_bad, "bss=", nseen);
	for (unsigned i = 0; i < nseen; i++) {
		char line[48];
		int k = 0;

		line[k++] = '0' + seen[i].ch / 10;
		line[k++] = '0' + seen[i].ch % 10;
		line[k++] = ' ';
		int v = -seen[i].rssi;
		line[k++] = '-';
		line[k++] = '0' + (v / 10) % 10;
		line[k++] = '0' + v % 10;
		line[k++] = ' ';
		for (int j = 0; j < seen[i].ssid_len && k < 46; j++)
			line[k++] = seen[i].ssid[j] >= 32 && seen[i].ssid[j] < 127 ? (char)seen[i].ssid[j] : '?';
		line[k] = 0;
		con_str("BSS", line);
	}
}

static void wlan_stage3(void)
{
	int i;

	for (i = 0; i < 3000; i++) {
		if (smd_find("WLAN_CTRL", &wlan_ch) == 0)
			break;
		delay_ms(1);
	}
	if (i == 3000 || smd_open(&wlan_ch)) {
		con_str("WLAN", "WLAN_CTRL did not open");
		return;
	}
	con_str("WLAN", "WLAN_CTRL open");
	k88_present();
	if (hal_load_nv() || hal_start()) {
		k88_present();
		return;
	}
	k88_present();
	dxe_init();
	if (hal_add_sta_self() == 0) {
		k88_present();
		wlan_scan();
	}
	k88_present();
}
