/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * mt7612u-hal - minimal userspace HAL for MediaTek MT7612U over libusb.
 *
 * Descriptor layouts and register sequences are derived from openwrt/mt76
 * (mt76x2/, mt76x02*), Copyright (C) 2016 Felix Fietkau, (C) 2018 Lorenzo
 * Bianconi / Stanislaw Gruszka.
 *
 * Design notes and the evidence behind every constant here: ../INVESTIGATION.md
 */
#ifndef MT7612U_H
#define MT7612U_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MT7612U_VID 0x0e8d
#define MT7612U_PID 0x7612

/* txwi.rate bits 15:13 - the PHY type the frame is sent with. */
enum mt7612u_phy {
	MT7612U_PHY_CCK   = 0,
	MT7612U_PHY_OFDM  = 1,
	MT7612U_PHY_HT    = 2,
	MT7612U_PHY_HT_GF = 3,
	MT7612U_PHY_VHT   = 4,
};

/* txwi.rate bits 8:7. A frame may narrow below the channel width, not exceed it. */
enum mt7612u_bw {
	MT7612U_BW_20 = 0,
	MT7612U_BW_40 = 1,
	MT7612U_BW_80 = 2,
};

/*
 * Per-packet PHY selection. Every field here maps into the 16-bit txwi.rate
 * word plus one bit of txwi.ack_ctl, so all of it is genuinely per frame -
 * nothing is cached in firmware or in a per-station rate table.
 */
struct mt7612u_tx_rate {
	enum mt7612u_phy phy;
	uint8_t  mcs;      /* legacy index, HT MCS 0-31, or VHT MCS 0-9 */
	uint8_t  nss;      /* 1 or 2; folded into the rate index for HT/VHT */
	enum mt7612u_bw bw;
	unsigned sgi  : 1;
	unsigned ldpc : 1;
	unsigned stbc : 1;  /* hardware honours it only at nss == 1 */
	unsigned no_ack : 1;
	int8_t   power_adj; /* txwi.ctl2 MT_TX_PWR_ADJ, 4-bit relative offset */
};

/* Filled from the 32-byte RXWI in front of every received frame. */
struct mt7612u_rx_info {
	enum mt7612u_phy phy;
	uint8_t  mcs;
	uint8_t  nss;
	enum mt7612u_bw bw;
	unsigned sgi  : 1;
	unsigned ldpc : 1;
	unsigned stbc : 1;
	unsigned crc_err : 1;
	unsigned ampdu   : 1;
	int8_t   rssi[4];      /* per chain, already EEPROM-corrected */
	uint8_t  n_chains;
	uint16_t mpdu_len;
	uint16_t seq;
};

struct mt7612u_dev;

/*
 * open: claims the interface, detaching mt76x2u if it holds it, then runs the
 * full power-on, firmware load and MAC/PHY init. fw_dir may be NULL for the
 * system default. Returns NULL on failure; err (optional) receives a message.
 */
struct mt7612u_dev *mt7612u_open(const char *fw_dir, const char **err);
void mt7612u_close(struct mt7612u_dev *dev);

/* Reattaches the kernel driver on close unless this is set. */
void mt7612u_keep_detached(struct mt7612u_dev *dev, int keep);

/*
 * Channel + width. Issues CMD_SWITCH_CHANNEL_OP and the firmware calibration
 * burst, so it is not cheap - it is a setup call, not a per-frame one.
 */
int mt7612u_set_channel(struct mt7612u_dev *dev, unsigned chan, enum mt7612u_bw bw);

/* Absolute TX power base, dBm. Per-frame trim is mt7612u_tx_rate.power_adj. */
int mt7612u_set_txpower(struct mt7612u_dev *dev, int dbm);

/* 0x202 = 2T2R (default), 0x101 = 1T1R. Global; takes effect at next channel set. */
int mt7612u_set_chainmask(struct mt7612u_dev *dev, uint16_t chainmask);

int mt7612u_start(struct mt7612u_dev *dev);  /* enable MAC TX+RX */
int mt7612u_stop(struct mt7612u_dev *dev);

/*
 * Inject one complete 802.11 frame (no FCS - the MAC appends it). Builds the
 * TXWI + TXINFO and submits a single bulk transfer. Fire-and-forget.
 */
int mt7612u_tx(struct mt7612u_dev *dev, const void *frame, size_t len,
               const struct mt7612u_tx_rate *rate);

/*
 * RX callback, invoked from the libusb event thread. frame excludes the RXWI.
 * Must not block and must not call back into the device.
 */
typedef void (*mt7612u_rx_cb)(void *user, const void *frame, size_t len,
                              const struct mt7612u_rx_info *info);
int mt7612u_rx_start(struct mt7612u_dev *dev, mt7612u_rx_cb cb, void *user);
int mt7612u_rx_stop(struct mt7612u_dev *dev);

/*
 * Radiotap-framed inject, matching devourer's send_packet() contract: one
 * buffer holding a radiotap header followed by the 802.11 MPDU, with the
 * per-frame rate carried in the header.
 */
int mt7612u_send_packet(struct mt7612u_dev *dev, const void *buf, size_t len);

/* One radiotap-framed MPDU, as handed to mt7612u_send_packets(). */
struct mt7612u_tx_view { const uint8_t *data; size_t len; };

/*
 * Submit several frames in one call. MT7612U chains them into a single
 * bulk-OUT transfer via the TXDMA's "next valid" bit, so a burst costs one USB
 * transaction instead of one per frame. Returns the number accepted.
 */
size_t mt7612u_send_packets(struct mt7612u_dev *dev,
                            const struct mt7612u_tx_view *pkts, size_t count);

/*
 * Hardware ACK responder: make the MAC answer frames addressed to `mac` with
 * a SIFS-timed ACK, with no host involvement. `mac` must be unicast.
 * Returns 0 on success, negative when unsupported or the arm cannot be
 * verified. Clear is best effort.
 */
int  mt7612u_set_ack_responder(struct mt7612u_dev *dev, const uint8_t mac[6]);
void mt7612u_clear_ack_responder(struct mt7612u_dev *dev);

/* TSF, the hardware microsecond clock. Two register reads. */
uint64_t mt7612u_read_tsf(struct mt7612u_dev *dev);
void     mt7612u_write_tsf(struct mt7612u_dev *dev, uint64_t tsf);

/* What this adapter can do, so a caller need not assume. */
struct mt7612u_caps {
	const char *chip_name;
	uint32_t rev;
	uint8_t  nss_rx, nss_tx;
	uint8_t  bw_mask;          /* bit0 = 20, bit1 = 40, bit2 = 80 MHz */
	uint16_t band_5g_min_mhz, band_5g_max_mhz;
	uint16_t band_2g_min_mhz, band_2g_max_mhz;
	unsigned ampdu_tx : 1;     /* aggregation works on injected frames */
	unsigned per_chain_rssi : 1;
	unsigned narrowband : 1;   /* 5/10 MHz - not available on this part */
	unsigned fast_retune : 1;  /* sub-ms channel change - not on this part */
};
void mt7612u_get_caps(const struct mt7612u_dev *dev, struct mt7612u_caps *caps);

/* Identity, for logging and for refusing to run on an unexpected revision. */
uint32_t mt7612u_asic_version(const struct mt7612u_dev *dev); /* 0x76120044 here */
const uint8_t *mt7612u_mac_addr(const struct mt7612u_dev *dev);

#ifdef __cplusplus
}
#endif
#endif /* MT7612U_H */
