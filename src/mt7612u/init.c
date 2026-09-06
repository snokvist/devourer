/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Power-on, MAC reset and MAC start/stop. Ported verbatim from
 * mt76/mt76x2/usb_init.c, usb_mac.c and init.c - deliberately NOT minimised.
 * See PLAN.md: trimming this sequence is a post-Gate-E activity, because a
 * 95%-correct init answers every register read and still radiates nothing.
 */
#include <string.h>
#include "internal.h"
#include "initvals.h"

/* ---- power ---- */

static void set_wlan_state(struct mt7612u_dev *d, int enable)
{
	uint32_t val = mt_rr(d, MT_WLAN_FUN_CTRL);

	if (enable)
		val |= MT_WLAN_FUN_CTRL_WLAN_EN | MT_WLAN_FUN_CTRL_WLAN_CLK_EN;
	else
		val &= ~(MT_WLAN_FUN_CTRL_WLAN_EN | MT_WLAN_FUN_CTRL_WLAN_CLK_EN);

	mt_wr(d, MT_WLAN_FUN_CTRL, val);
	mt_usleep(20);
}

static void reset_wlan(struct mt7612u_dev *d, int enable)
{
	uint32_t val;

	if (!enable) { set_wlan_state(d, enable); return; }

	val = mt_rr(d, MT_WLAN_FUN_CTRL);
	val &= ~MT_WLAN_FUN_CTRL_FRC_WL_ANT_SEL;

	if (val & MT_WLAN_FUN_CTRL_WLAN_EN) {
		val |= MT_WLAN_FUN_CTRL_WLAN_RESET_RF;
		mt_wr(d, MT_WLAN_FUN_CTRL, val);
		mt_usleep(20);
		val &= ~MT_WLAN_FUN_CTRL_WLAN_RESET_RF;
	}
	mt_wr(d, MT_WLAN_FUN_CTRL, val);
	mt_usleep(20);

	set_wlan_state(d, enable);
}

static void power_on_rf_patch(struct mt7612u_dev *d)
{
	mt_set(d, CFG_ADDR(0x130), BIT(0) | BIT(16));
	mt_usleep(1);
	mt_clear(d, CFG_ADDR(0x1c), 0xff);
	mt_set(d, CFG_ADDR(0x1c), 0x30);
	mt_wr(d, CFG_ADDR(0x14), 0x484f);
	mt_usleep(1);
	mt_set(d, CFG_ADDR(0x130), BIT(17));
	mt_usleep(200);
	mt_clear(d, CFG_ADDR(0x130), BIT(16));
	mt_usleep(100);
	mt_set(d, CFG_ADDR(0x14c), BIT(19) | BIT(20));
}

static void power_on_rf(struct mt7612u_dev *d, int unit)
{
	int shift = unit ? 8 : 0;
	uint32_t val = (BIT(1) | BIT(3) | BIT(4) | BIT(5)) << shift;

	mt_set(d, CFG_ADDR(0x130), BIT(0) << shift);   /* RF BG */
	mt_usleep(20);
	mt_set(d, CFG_ADDR(0x130), val);               /* RFDIG LDO/AFE/ABB/ADDA */
	mt_usleep(20);
	mt_clear(d, CFG_ADDR(0x130), BIT(2) << shift); /* RFDIG -> internal LDO */
	mt_usleep(20);

	power_on_rf_patch(d);
	mt_set(d, 0x530, 0xf);
}

static void power_on(struct mt7612u_dev *d)
{
	uint32_t val;

	mt_set(d, CFG_ADDR(MT_CFG_MTC_CTRL), MT_WLAN_MTC_CTRL_MTCMOS_PWR_UP);

	val = MT_WLAN_MTC_CTRL_STATE_UP | MT_WLAN_MTC_CTRL_PWR_ACK |
	      MT_WLAN_MTC_CTRL_PWR_ACK_S;
	if (!mt_poll(d, CFG_ADDR(MT_CFG_MTC_CTRL), val, val, 1000))
		LOG("warning: MTCMOS power-up did not ack");

	mt_clear(d, CFG_ADDR(MT_CFG_MTC_CTRL), 0x7fu << 16);
	mt_usleep(20);
	mt_clear(d, CFG_ADDR(MT_CFG_MTC_CTRL), 0xfu << 24);
	mt_usleep(20);
	mt_set(d, CFG_ADDR(MT_CFG_MTC_CTRL), 0xfu << 24);
	mt_clear(d, CFG_ADDR(MT_CFG_MTC_CTRL), 0xfff);

	mt_clear(d, CFG_ADDR(0x1204), BIT(3));  /* AD/DA power down off */
	mt_set(d, CFG_ADDR(0x80), BIT(0));      /* WLAN function enable */
	mt_clear(d, CFG_ADDR(0x64), BIT(18));   /* release BBP soft reset */

	power_on_rf(d, 0);
	power_on_rf(d, 1);
}

static void init_dma(struct mt7612u_dev *d)
{
	uint32_t val = mt_rr(d, CFG_ADDR(MT_USB_U3DMA_CFG));

	val |= MT_USB_DMA_CFG_RX_DROP_OR_PAD | MT_USB_DMA_CFG_RX_BULK_EN |
	       MT_USB_DMA_CFG_TX_BULK_EN;
	/* Aggregation off: one URB carries exactly one RX frame, which is what
	 * makes the RX path in rx.c a straight parse with no de-aggregation. */
	val &= ~MT_USB_DMA_CFG_RX_BULK_AGG_EN;
	mt_wr(d, CFG_ADDR(MT_USB_U3DMA_CFG), val);
}

/* ---- MAC ---- */

static void mac_fixup_xtal(struct mt7612u_dev *d)
{
	int8_t offset = 0;
	uint16_t eep = mt_ee(d, MT_EE_XTAL_TRIM_2);

	offset = eep & 0x7f;
	if ((eep & 0xff) == 0xff)
		offset = 0;
	else if (eep & 0x80)
		offset = (int8_t)-offset;

	eep >>= 8;
	if (eep == 0x00 || eep == 0xff) {
		eep = mt_ee(d, MT_EE_XTAL_TRIM_1) & 0xff;
		if (eep == 0x00 || eep == 0xff)
			eep = 0x14;
	}
	eep &= 0x7f;

	mt_rmw(d, CFG_ADDR(MT_XO_CTRL5), MT_XO_CTRL5_C2_VAL,
	       FIELD_PREP(MT_XO_CTRL5_C2_VAL, (uint32_t)(eep + offset)));
	mt_set(d, CFG_ADDR(MT_XO_CTRL6), MT_XO_CTRL6_C2_CTRL);

	mt_wr(d, 0x504, 0x06000000);
	mt_wr(d, 0x50c, 0x08800000);
	mt_usleep(5000);
	mt_wr(d, 0x504, 0x0);

	/* SIFS 16us -> 13us */
	mt_rmw(d, MT_XIFS_TIME_CFG, MT_XIFS_TIME_CFG_OFDM_SIFS,
	       FIELD_PREP(MT_XIFS_TIME_CFG_OFDM_SIFS, 0xd));
	mt_rmw(d, MT_BKOFF_SLOT_CFG, MT_BKOFF_SLOT_CFG_CC_DELAY,
	       FIELD_PREP(MT_BKOFF_SLOT_CFG_CC_DELAY, 1));

	mt_clear(d, MT_FCE_L2_STUFF, MT_FCE_L2_STUFF_WR_MPDU_LEN_EN);

	switch (FIELD_GET(MT_EE_NIC_CONF_2_XTAL_OPTION, mt_ee(d, MT_EE_NIC_CONF_2))) {
	case 0: mt_wr(d, MT_XO_CTRL7, 0x5c1fee80); break;
	case 1: mt_wr(d, MT_XO_CTRL7, 0x5c1feed0); break;
	default: break;
	}
}

static void mac_reset(struct mt7612u_dev *d)
{
	mt_wr(d, MT_WPDMA_GLO_CFG, BIT(4) | BIT(5));
	mt_wr(d, MT_PBF_TX_MAX_PCNT, 0xefef3f1f);
	mt_wr(d, MT_PBF_RX_MAX_PCNT, 0xfebf);

	for (unsigned i = 0; i < sizeof mt7612u_mac_initvals / sizeof mt7612u_mac_initvals[0]; i++)
		mt_wr(d, mt7612u_mac_initvals[i].reg, mt7612u_mac_initvals[i].val);

	mt_wr(d, MT_TX_LINK_CFG, 0x1020);
	mt_wr(d, MT_AUTO_RSP_CFG, 0x13);
	mt_wr(d, MT_MAX_LEN_CFG, 0x2f00);

	mt_wr(d, MT_WMM_AIFSN, 0x2273);
	mt_wr(d, MT_WMM_CWMIN, 0x2344);
	mt_wr(d, MT_WMM_CWMAX, 0x34aa);

	mt_clear(d, MT_MAC_SYS_CTRL,
	         MT_MAC_SYS_CTRL_RESET_CSR | MT_MAC_SYS_CTRL_RESET_BBP);

	/* is_mt7612(): coexistence off. */
	mt_clear(d, MT_COEXCFG0, MT_COEXCFG0_COEX_EN);

	mt_set(d, MT_EXT_CCA_CFG, 0xf000);
	mt_clear(d, MT_TX_ALC_CFG_4, BIT(31));

	mac_fixup_xtal(d);
}

static void mac_setaddr(struct mt7612u_dev *d)
{
	const uint8_t *a = d->macaddr;
	uint8_t zero[8] = { 0 };
	uint32_t dw0 = (uint32_t)a[0] | ((uint32_t)a[1] << 8) |
	               ((uint32_t)a[2] << 16) | ((uint32_t)a[3] << 24);
	uint32_t dw1 = (uint32_t)a[4] | ((uint32_t)a[5] << 8);

	mt_wr(d, MT_MAC_ADDR_DW0, dw0);
	/* NOTE: the U2ME byte (bits 23:16) is write-only on this silicon - it
	 * always reads back 0. Do not use it as a write-probe. */
	mt_wr(d, MT_MAC_ADDR_DW1, dw1 | FIELD_PREP(MT_MAC_ADDR_DW1_U2ME_MASK, 0xff));

	mt_wr(d, MT_MAC_BSSID_DW0, dw0);
	mt_wr(d, MT_MAC_BSSID_DW1, dw1 |
	      FIELD_PREP(MT_MAC_BSSID_DW1_MBSS_MODE, 3) |
	      MT_MAC_BSSID_DW1_MBSS_LOCAL_BIT);
	mt_rmw(d, MT_MAC_BSSID_DW1, MT_MAC_BSSID_DW1_MBEACON_N,
	       FIELD_PREP(MT_MAC_BSSID_DW1_MBEACON_N, 7));

	/* mt76x02_mac_set_bssid() masks the index to 3 bits, so this covers
	 * APC BSSID slots 0..7 at 0x1090..0x10cc. Do NOT guess this base:
	 * 0x1200 is MT_MAC_STATUS, and zeroing 128 bytes from there walks over
	 * live MAC registers. */
	for (int i = 0; i < 16; i++) {
		int idx = i & 7;

		mt_wr(d, MT_MAC_APC_BSSID_L(idx), 0);
		mt_rmw(d, MT_MAC_APC_BSSID_H(idx), MT_MAC_APC_BSSID_H_ADDR, 0);
	}
	(void)zero;
}

static void wcid_and_key_clear(struct mt7612u_dev *d)
{
	uint8_t zero32[32] = { 0 };

	for (int i = 0; i < 256; i++) {
		mt_wr(d, MT_WCID_ATTR(i), 0);
		if (i < 128)
			mt_wr_copy(d, MT_WCID_ADDR(i), zero32, 8);
	}
	for (int bss = 0; bss < 16; bss++) {
		for (int k = 0; k < 4; k++) {
			uint32_t v = mt_rr(d, MT_SKEY_MODE(bss));

			v &= ~(MT_SKEY_MODE_MASK << MT_SKEY_MODE_SHIFT(bss, k));
			mt_wr(d, MT_SKEY_MODE(bss), v);
			mt_wr_copy(d, MT_SKEY(bss, k), zero32, 32);
		}
	}
}

/*
 * Drain and discard whatever is sitting in the RX bulk endpoint. The MAC will
 * happily fill its RX pool with ambient frames the moment RX is enabled, and
 * nothing else in this HAL reads EP 4 unless the caller asked for RX - a full
 * pool is the leading suspect for the chip wedging after repeated TX cycles
 * (see BRINGUP-RESULTS.md). Cheap insurance either way.
 */
void mt_rx_flush(struct mt7612u_dev *d)
{
	uint8_t buf[4096];
	int n;

	for (int i = 0; i < 64; i++) {
		if (mt_bulk(d, MT_EP_IN_PKT_RX, buf, sizeof buf, &n, 10) || n == 0)
			break;
	}
}

int mt_mac_start(struct mt7612u_dev *d, int enable_rx)
{
	mt_wr(d, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX);
	if (!mt_poll(d, MT_WPDMA_GLO_CFG,
	             MT_WPDMA_GLO_CFG_TX_DMA_BUSY | MT_WPDMA_GLO_CFG_RX_DMA_BUSY,
	             0, 200000)) {
		ERR("mac_start: WPDMA stayed busy");
		return -1;
	}
	mt_wr(d, MT_RX_FILTR_CFG, 0x00015f97);
	/* Only turn the receiver on when the caller will actually drain EP 4.
	 * mt76's mac_start always sets both bits, but mt76 also keeps RX URBs
	 * permanently queued; a TX-only injector that never reads has no such
	 * backstop. */
	mt_wr(d, MT_MAC_SYS_CTRL, MT_MAC_SYS_CTRL_ENABLE_TX |
	      (enable_rx ? MT_MAC_SYS_CTRL_ENABLE_RX : 0));
	return 0;
}

int mt_mac_stop(struct mt7612u_dev *d)
{
	uint32_t rts_cfg = mt_rr(d, MT_TX_RTS_CFG);

	mt_rx_flush(d);

	mt_wr(d, MT_TX_RTS_CFG, rts_cfg & ~MT_TX_RTS_CFG_RETRY_LIMIT);
	mt_clear(d, MT_TXOP_CTRL_CFG, MT_TXOP_ED_CCA_EN);
	mt_clear(d, MT_TXOP_HLDR_ET, MT_TXOP_HLDR_TX40M_BLK_EN);

	for (int i = 0; i < 2000; i++) {
		if (!(mt_rr(d, CFG_ADDR(MT_USB_U3DMA_CFG)) & MT_USB_DMA_CFG_TX_BUSY) && i > 10)
			break;
		mt_usleep(75);
	}
	mt_clear(d, MT_MAC_SYS_CTRL,
	         MT_MAC_SYS_CTRL_ENABLE_RX | MT_MAC_SYS_CTRL_ENABLE_TX);

	for (int i = 0; i < 1000; i++) {
		if (!(mt_rr(d, MT_MAC_STATUS) & MT_MAC_STATUS_TX) &&
		    !mt_rr(d, MT_BBP(IBI, 12)))
			break;
		mt_usleep(15);
	}
	if (!mt_poll(d, MT_MAC_STATUS, MT_MAC_STATUS_RX, 0, 200000))
		LOG("warning: MAC RX failed to stop");

	mt_wr(d, MT_TX_RTS_CFG, rts_cfg);
	return 0;
}

void mt_phy_set_rxpath(struct mt7612u_dev *d)
{
	uint32_t val = mt_rr(d, MT_BBP(AGC, 0));

	val &= ~BIT(4);
	if ((d->chainmask & 0xf) == 2)
		val |= BIT(3);
	else
		val &= ~BIT(3);
	mt_wr(d, MT_BBP(AGC, 0), val);
	(void)mt_rr(d, MT_BBP(AGC, 0));
}

void mt_phy_set_txdac(struct mt7612u_dev *d)
{
	if (((d->chainmask >> 8) & 0xf) == 2)
		mt_set(d, MT_BBP(TXBE, 5), 0x3);
	else
		mt_clear(d, MT_BBP(TXBE, 5), 0x3);
}

/* Exposed so the bringup gate can check whether our reset actually clears the
 * firmware-running state - the closest thing to a cold boot available here,
 * since no hub on this host supports per-port power switching. */
void mt_power_cycle(struct mt7612u_dev *d)
{
	reset_wlan(d, 1);
	power_on(d);
}

int mt_init_hardware(struct mt7612u_dev *d, const char *fw_dir)
{
	mt_power_cycle(d);

	if (!mt_wait_for_mac(d)) { ERR("MAC not ready after power on"); return -1; }

	if (mt_fw_init(d, fw_dir))
		return -1;

	if (!mt_poll(d, MT_WPDMA_GLO_CFG,
	             MT_WPDMA_GLO_CFG_TX_DMA_BUSY | MT_WPDMA_GLO_CFG_RX_DMA_BUSY,
	             0, 100000)) {
		ERR("WPDMA busy after firmware load");
		return -1;
	}
	if (!mt_wait_for_mac(d)) { ERR("MAC not ready after firmware"); return -1; }

	init_dma(d);

	if (mt_mcu_function_select(d, Q_SELECT, 1)) return -1;
	if (mt_mcu_set_radio_state(d, 1)) return -1;

	mac_reset(d);
	mac_setaddr(d);

	if (!mt_poll(d, MT_MAC_STATUS, MT_MAC_STATUS_TX | MT_MAC_STATUS_RX, 0, 100000))
		LOG("warning: TX/RX not idle before table clear");

	wcid_and_key_clear(d);

	/* Free-run the TSF counter. mt76 turns this on as part of beacon
	 * configuration, which an injector otherwise skips entirely - but
	 * without it MT_TSF_TIMER_DW0/DW1 read zero forever and ReadTsf is
	 * useless. TIMER_EN only; no TBTT, no beacon transmission. */
	mt_set(d, MT_BEACON_TIME_CFG, MT_BEACON_TIME_CFG_TIMER_EN);

	mt_rmw(d, MT_US_CYC_CFG, MT_US_CYC_CNT, FIELD_PREP(MT_US_CYC_CNT, 0x1e));
	mt_wr(d, MT_TXOP_CTRL_CFG, 0x583f);

	if (mt_mcu_load_cr(d, MT_RF_BBP_CR, 0, 0)) return -1;

	mt_phy_set_rxpath(d);
	mt_phy_set_txdac(d);

	/* Leave no half-full RX ring behind for the next run to inherit. */
	mt_rx_flush(d);

	return mt_mac_stop(d);
}
