/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * RX path. RX_BULK_AGG_EN is left off in init_dma(), so one bulk transfer
 * carries exactly one frame and this is a straight parse - no de-aggregation.
 *
 * Wire layout (INVESTIGATION.md §9):
 *   [FCE info 4B][RXWI 32B][802.11 frame]
 */
#include <string.h>
#include "internal.h"

#define RX_BUF_SIZE 4096

static uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t get_le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

/* Decode the 16-bit rate word - the same encoding the TX path writes. */
static void decode_rate(uint16_t rate, struct mt7612u_rx_info *out)
{
	uint32_t idx = FIELD_GET(MT_RATE_INDEX, rate);

	out->phy  = (enum mt7612u_phy)FIELD_GET(MT_RATE_PHY, rate);
	out->bw   = (enum mt7612u_bw)FIELD_GET(MT_RATE_BW, rate);
	out->sgi  = !!(rate & MT_RATE_SGI);
	out->ldpc = !!(rate & MT_RATE_LDPC);
	out->stbc = !!(rate & MT_RATE_STBC);

	switch (out->phy) {
	case MT7612U_PHY_VHT:
		out->mcs = FIELD_GET(MT_RATE_VHT_IDX, idx);
		out->nss = FIELD_GET(MT_RATE_VHT_NSS, idx) + 1;
		break;
	case MT7612U_PHY_HT:
	case MT7612U_PHY_HT_GF:
		out->mcs = (uint8_t)idx;
		out->nss = (uint8_t)(1 + (idx >> 3));
		break;
	default:
		out->mcs = (uint8_t)idx;
		out->nss = 1;
		break;
	}
}

/*
 * Parse one completed RX buffer. Returns the 802.11 frame length (excluding
 * the RXWI), or 0 if the buffer holds nothing usable. `frame` receives a
 * pointer into `buf`. Shared by the synchronous reader and the async ring, so
 * both decode identically.
 */
int mt_rx_parse(struct mt7612u_dev *d, uint8_t *buf, int n,
                const uint8_t **frame, struct mt7612u_rx_info *info)
{
	int len, pad = 0;
	uint32_t rxinfo, ctl;
	const uint8_t *rxwi;

	if (n < MT_DMA_HDR_LEN + MT_RXWI_LEN) return 0;

	rxwi   = buf + MT_DMA_HDR_LEN;
	rxinfo = get_le32(rxwi);
	ctl    = get_le32(rxwi + 4);

	memset(info, 0, sizeof *info);
	info->mpdu_len = (uint16_t)FIELD_GET(MT_RXWI_CTL_MPDU_LEN, ctl);
	info->seq      = (uint16_t)(get_le16(rxwi + 8) >> 4);
	info->crc_err  = !!(rxinfo & MT_RXINFO_CRCERR);
	info->ampdu    = !!(rxinfo & MT_RXINFO_AMPDU);
	decode_rate(get_le16(rxwi + 10), info);

	/* Per-chain RSSI is a fixed 4-byte field. Correction terms come from
	 * the EEPROM (mt76x02_mac_get_rssi); with them at zero these are the
	 * raw chip values, which is still enough to compare two chains. */
	info->n_chains = (uint8_t)((d->chainmask & 0xf) > 1 ? 2 : 1);
	for (int c = 0; c < 4; c++)
		info->rssi[c] = (int8_t)((int8_t)rxwi[12 + c] +
		                         (c < 2 ? d->cal.rssi_offset[c] : 0) -
		                         d->cal.lna_gain);

	if (rxinfo & MT_RXINFO_L2PAD)
		pad = 2;

	len = (int)info->mpdu_len;
	if (len > n - MT_DMA_HDR_LEN - MT_RXWI_LEN - pad)
		len = n - MT_DMA_HDR_LEN - MT_RXWI_LEN - pad;
	if (len < 0) return 0;

	*frame = buf + MT_DMA_HDR_LEN + MT_RXWI_LEN;
	/* Fold the L2 pad out by moving the header down over it. */
	if (pad && len > 24) {
		memmove(buf + MT_DMA_HDR_LEN + MT_RXWI_LEN + 2,
		        buf + MT_DMA_HDR_LEN + MT_RXWI_LEN, 24);
		*frame = buf + MT_DMA_HDR_LEN + MT_RXWI_LEN + 2;
	}
	return len;
}

int mt_rx_one(struct mt7612u_dev *d, uint8_t *buf, int bufsize,
              const uint8_t **frame, struct mt7612u_rx_info *info,
              unsigned timeout_ms)
{
	int n = 0, rc = mt_bulk(d, MT_EP_IN_PKT_RX, buf, bufsize, &n, timeout_ms);

	if (rc == LIBUSB_ERROR_TIMEOUT) return 0;
	if (rc) return -1;
	return mt_rx_parse(d, buf, n, frame, info);
}
