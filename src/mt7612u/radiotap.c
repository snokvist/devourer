/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Minimal radiotap TX parser, so a caller can hand us the same
 * "radiotap header + 802.11 MPDU" buffer devourer's send_packet() takes
 * instead of filling a struct.
 *
 * Only the inject-relevant fields are decoded; everything else is skipped by
 * the alignment/size table, which is what makes skipping correct rather than
 * lucky. Field order, alignment and size follow the radiotap spec.
 */
#include <string.h>
#include "internal.h"

/* {align, size} per radiotap bit index. size 0 = unknown -> stop parsing. */
static const struct { uint8_t align, size; } rt_field[] = {
	{ 8, 8 }, /*  0 TSFT              */ { 1, 1 }, /*  1 FLAGS   */
	{ 1, 1 }, /*  2 RATE              */ { 2, 4 }, /*  3 CHANNEL */
	{ 2, 2 }, /*  4 FHSS              */ { 1, 1 }, /*  5 DBM_ANTSIGNAL */
	{ 1, 1 }, /*  6 DBM_ANTNOISE      */ { 2, 2 }, /*  7 LOCK_QUALITY */
	{ 2, 2 }, /*  8 TX_ATTENUATION    */ { 2, 2 }, /*  9 DB_TX_ATTENUATION */
	{ 1, 1 }, /* 10 DBM_TX_POWER      */ { 1, 1 }, /* 11 ANTENNA */
	{ 1, 1 }, /* 12 DB_ANTSIGNAL      */ { 1, 1 }, /* 13 DB_ANTNOISE */
	{ 2, 2 }, /* 14 RX_FLAGS          */ { 2, 2 }, /* 15 TX_FLAGS */
	{ 1, 1 }, /* 16 RTS_RETRIES       */ { 1, 1 }, /* 17 DATA_RETRIES */
	{ 0, 0 }, /* 18 (unused)          */ { 1, 3 }, /* 19 MCS */
	{ 4, 8 }, /* 20 AMPDU_STATUS      */ { 2, 12 },/* 21 VHT */
	{ 8, 12 },/* 22 TIMESTAMP         */ { 2, 12 },/* 23 HE */
};
#define RT_RATE 2
#define RT_DBM_TX_POWER 10
#define RT_TX_FLAGS 15
#define RT_MCS 19
#define RT_VHT 21
#define RT_TX_FLAGS_NOACK 0x0008

static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Legacy radiotap RATE is in 500 kbps units; the hardware wants an index. */
static int legacy_rate_index(uint8_t r500, enum mt7612u_phy *phy)
{
	switch (r500) {
	case 2:  *phy = MT7612U_PHY_CCK;  return 0;   /* 1 Mbps   */
	case 4:  *phy = MT7612U_PHY_CCK;  return 1;   /* 2        */
	case 11: *phy = MT7612U_PHY_CCK;  return 2;   /* 5.5      */
	case 22: *phy = MT7612U_PHY_CCK;  return 3;   /* 11       */
	case 12: *phy = MT7612U_PHY_OFDM; return 0;   /* 6        */
	case 18: *phy = MT7612U_PHY_OFDM; return 1;   /* 9        */
	case 24: *phy = MT7612U_PHY_OFDM; return 2;   /* 12       */
	case 36: *phy = MT7612U_PHY_OFDM; return 3;   /* 18       */
	case 48: *phy = MT7612U_PHY_OFDM; return 4;   /* 24       */
	case 72: *phy = MT7612U_PHY_OFDM; return 5;   /* 36       */
	case 96: *phy = MT7612U_PHY_OFDM; return 6;   /* 48       */
	case 108:*phy = MT7612U_PHY_OFDM; return 7;   /* 54       */
	default: *phy = MT7612U_PHY_OFDM; return 0;
	}
}

/*
 * Parse a radiotap header into a tx_rate. Returns the header length, or 0 if
 * the buffer is not a usable radiotap header.
 */
int mt_radiotap_parse(const uint8_t *buf, size_t len, struct mt7612u_tx_rate *r)
{
	uint32_t present[8];
	unsigned n_present = 0, bit = 0;
	size_t rlen, off;

	if (!buf || len < 8 || buf[0] != 0) return 0;      /* version must be 0 */
	rlen = rd16(buf + 2);
	if (rlen < 8 || rlen > len) return 0;

	/* Walk the extended present bitmaps. */
	off = 4;
	do {
		if (off + 4 > rlen || n_present == 8) return 0;
		present[n_present] = rd32(buf + off);
		off += 4;
	} while (present[n_present++] & 0x80000000u);

	memset(r, 0, sizeof *r);
	r->phy = MT7612U_PHY_OFDM;
	r->nss = 1;
	r->bw  = MT7612U_BW_20;

	for (unsigned w = 0; w < n_present; w++) {
		for (unsigned b = 0; b < 31; b++, bit++) {
			const uint8_t *p;
			uint8_t align, size;

			if (!(present[w] & (1u << b)))
				continue;
			if (bit >= sizeof rt_field / sizeof rt_field[0])
				return (int)rlen;          /* unknown tail: stop */
			align = rt_field[bit].align;
			size  = rt_field[bit].size;
			if (!size)
				return (int)rlen;
			off = (off + align - 1) & ~((size_t)align - 1);
			if (off + size > rlen)
				return (int)rlen;
			p = buf + off;
			off += size;

			switch (bit) {
			case RT_RATE:
				if (!(present[0] & (1u << RT_MCS)))
					r->mcs = (uint8_t)legacy_rate_index(p[0], &r->phy);
				break;
			case RT_TX_FLAGS:
				if (rd16(p) & RT_TX_FLAGS_NOACK) r->no_ack = 1;
				break;
			case RT_DBM_TX_POWER:
				r->power_adj = 0;   /* absolute dBm is a device-level knob */
				break;
			case RT_MCS: {
				uint8_t known = p[0], flags = p[1];

				r->phy = MT7612U_PHY_HT;
				r->mcs = p[2];
				r->nss = (uint8_t)(1 + (p[2] >> 3));
				if ((known & 0x02) && ((flags & 0x03) == 1))
					r->bw = MT7612U_BW_40;
				if (known & 0x04) r->sgi  = (flags >> 2) & 1;
				if (known & 0x10) r->ldpc = (flags >> 4) & 1;
				if (known & 0x20) r->stbc = ((flags >> 5) & 3) ? 1 : 0;
				break;
			}
			case RT_VHT: {
				uint16_t known = rd16(p);
				uint8_t flags = p[2], bwc = p[3], mcs_nss = p[4], coding = p[8];

				r->phy = MT7612U_PHY_VHT;
				r->mcs = (uint8_t)(mcs_nss >> 4);
				r->nss = (uint8_t)(mcs_nss & 0x0f);
				if (!r->nss) r->nss = 1;
				if (known & 0x0004) r->stbc = flags & 1;

				if (flags & 0x04)   r->sgi = 1;
				if (coding & 0x01)  r->ldpc = 1;
				r->bw = bwc == 0 ? MT7612U_BW_20
				      : (bwc <= 3 ? MT7612U_BW_40 : MT7612U_BW_80);
				break;
			}
			default:
				break;
			}
		}
	}
	return (int)rlen;
}

/*
 * devourer's send_packet() contract: one buffer, radiotap header followed by
 * the 802.11 MPDU. Per-frame rate comes from the header.
 */
int mt7612u_send_packet(struct mt7612u_dev *d, const void *buf, size_t len)
{
	struct mt7612u_tx_rate r;
	const uint8_t *p = buf;
	int rlen = mt_radiotap_parse(p, len, &r);

	if (rlen <= 0 || (size_t)rlen >= len) {
		ERR("send_packet: no usable radiotap header");
		return -1;
	}
	return mt7612u_tx(d, p + rlen, len - (size_t)rlen, &r);
}

/*
 * devourer's send_packets() contract: several radiotap-framed MPDUs in one
 * call. MT7612U packs them into a single bulk-OUT transfer using
 * MT_TXD_INFO_NEXT_VLD, so a burst costs one USB transaction rather than one
 * per frame. Returns the number accepted.
 */
size_t mt7612u_send_packets(struct mt7612u_dev *d,
                            const struct mt7612u_tx_view *pkts, size_t count)
{
	uint8_t buf[MT_USB_AGG_BUF];
	size_t sent = 0, i = 0;

	if (!pkts) return 0;

	while (i < count) {
		size_t off = 0, n_in_buf = 0, j;
		size_t idx[MT_USB_AGG_MAX];

		/* Pass 1: pick the frames that fit in one transfer. */
		while (i < count && n_in_buf < MT_USB_AGG_MAX) {
			size_t need;

			if (!pkts[i].data || pkts[i].len < 8) { i++; continue; }
			need = pkts[i].len + 32;
			if (off + need + 4 > sizeof buf) break;
			idx[n_in_buf++] = i;
			off += need;      /* upper bound; pass 2 uses the real size */
			i++;
		}
		if (!n_in_buf) break;

		/* Pass 2: build them back to back. NEXT_VLD on every block except
		 * the last, and only the last carries the 4-byte zero trailer. */
		off = 0;
		for (j = 0; j < n_in_buf; j++) {
			struct mt7612u_tx_rate r;
			const uint8_t *p = pkts[idx[j]].data;
			size_t plen = pkts[idx[j]].len;
			int last = (j + 1 == n_in_buf);
			int rlen = mt_radiotap_parse(p, plen, &r);
			int blk;

			if (rlen <= 0 || (size_t)rlen >= plen) continue;
			blk = mt_tx_build(d, buf + off, sizeof buf - off,
			                  p + rlen, plen - (size_t)rlen, &r, 0xff, 0,
			                  !last, last);
			if (blk < 0) break;
			off += (size_t)blk;
		}
		if (!off) break;

		if (d->a) {
			if (mt_async_tx_submit(d, buf, (int)off) == 0) sent += n_in_buf;
		} else {
			int n = 0;

			if (mt_bulk(d, MT_EP_OUT_AC_BE, buf, (int)off, &n, 500) == 0)
				sent += n_in_buf;
		}
	}
	return sent;
}
