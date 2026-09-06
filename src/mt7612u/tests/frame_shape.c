/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Frame-shape tests: 802.11 header length, and the RX L2-pad fold that
 * depends on it. No hardware, no privileges - mt_rx_parse() only reads the
 * device struct for the chainmask and the EEPROM gain terms.
 *
 * Both cases here are regressions, not hypotheticals:
 *
 *  - hdrlen treated only RTS and PS-Poll as 16-byte control frames, leaving
 *    BlockAckReq, BlockAck and the two CF-End subtypes at 10. On TX that
 *    inserts the L2 pad ten bytes in, i.e. inside the frame.
 *  - the RX fold moved a fixed 24 bytes. L2PAD is only ever set when the
 *    header is *not* 4-aligned - 26 bytes (QoS) or 30 (4-address) - so the
 *    last two header bytes were left behind and overwritten by the pad. On
 *    a QoS frame those two bytes are the QoS Control field, so every TID
 *    and ack-policy read as zero.
 */
#include <stdio.h>
#include <string.h>
#include "internal.h"

static int fails;

static void expect_hdrlen(const char *what, uint8_t b0, uint8_t b1, int want)
{
	uint8_t fc[2] = { b0, b1 };
	int got = mt_hdrlen_from_fc(fc);

	if (got != want) {
		printf("  FAIL %-28s fc=%02x%02x want %d got %d\n",
		       what, b0, b1, want, got);
		fails++;
	}
}

static void test_hdrlen(void)
{
	printf("mt_hdrlen_from_fc:\n");

	/* management */
	expect_hdrlen("beacon",              0x80, 0x00, 24);
	expect_hdrlen("action +Order",       0xd0, 0x80, 28);

	/* control: 16 by default, 10 only for CTS and ACK */
	expect_hdrlen("BlockAckReq",         0x84, 0x00, 16);
	expect_hdrlen("BlockAck",            0x94, 0x00, 16);
	expect_hdrlen("PS-Poll",             0xa4, 0x00, 16);
	expect_hdrlen("RTS",                 0xb4, 0x00, 16);
	expect_hdrlen("CTS",                 0xc4, 0x00, 10);
	expect_hdrlen("ACK",                 0xd4, 0x00, 10);
	expect_hdrlen("CF-End",              0xe4, 0x00, 16);
	expect_hdrlen("CF-End+CF-Ack",       0xf4, 0x00, 16);

	/* data */
	expect_hdrlen("data 3-addr",         0x08, 0x00, 24);
	expect_hdrlen("QoS data",            0x88, 0x00, 26);
	expect_hdrlen("QoS data +Order",     0x88, 0x80, 30);
	expect_hdrlen("data 4-addr",         0x08, 0x03, 30);
	expect_hdrlen("QoS data 4-addr",     0x88, 0x03, 32);
}

/*
 * Build [FCE 4][RXWI 32][26-byte QoS header][2 pad][body] and parse it.
 * The QoS Control field sits at header offset 24..25.
 */
static void test_rx_l2pad(void)
{
	struct mt7612u_dev d;
	uint8_t buf[256];
	const uint8_t *frame = NULL;
	struct mt7612u_rx_info info;
	static const uint8_t body[] = "payload";
	const int hdrlen = 26, pad = 2;
	const int mpdu = hdrlen + (int)sizeof body;
	uint8_t *hdr;
	uint32_t rxinfo = MT_RXINFO_L2PAD;
	uint32_t ctl = FIELD_PREP(MT_RXWI_CTL_MPDU_LEN, (uint32_t)mpdu);
	int n, len;

	printf("mt_rx_parse, L2 pad on a QoS frame:\n");

	memset(&d, 0, sizeof d);
	d.chainmask = 0x0202;
	memset(buf, 0, sizeof buf);

	for (int i = 0; i < 4; i++) buf[MT_DMA_HDR_LEN + i] = (uint8_t)(rxinfo >> (8 * i));
	for (int i = 0; i < 4; i++) buf[MT_DMA_HDR_LEN + 4 + i] = (uint8_t)(ctl >> (8 * i));
	/* rate word: OFDM, index 0 - not what this test is about, but it must
	 * decode without tripping the VHT/HT index maths. */
	buf[MT_DMA_HDR_LEN + 10] = 0x00;
	buf[MT_DMA_HDR_LEN + 11] = 0x20;

	hdr = buf + MT_DMA_HDR_LEN + MT_RXWI_LEN;
	hdr[0] = 0x88;                 /* QoS data */
	hdr[1] = 0x00;
	memset(hdr + 4, 0xff, 6);      /* addr1 */
	hdr[24] = 0x07;                /* QoS Control: TID 7 ... */
	hdr[25] = 0x00;                /* ... normal ack policy */
	memset(hdr + hdrlen, 0xaa, pad);            /* the pad the MAC inserted */
	memcpy(hdr + hdrlen + pad, body, sizeof body);
	n = MT_DMA_HDR_LEN + MT_RXWI_LEN + hdrlen + pad + (int)sizeof body;

	len = mt_rx_parse(&d, buf, n, &frame, &info);

	if (len != mpdu) {
		printf("  FAIL length: want %d got %d\n", mpdu, len);
		fails++;
		return;
	}
	if (frame[24] != 0x07 || frame[25] != 0x00) {
		printf("  FAIL QoS Control zeroed by the pad fold: %02x %02x\n",
		       frame[24], frame[25]);
		fails++;
	}
	if (memcmp(frame + hdrlen, body, sizeof body) != 0) {
		printf("  FAIL body misaligned after the fold\n");
		fails++;
	}
	if (frame[0] != 0x88) {
		printf("  FAIL frame control lost: %02x\n", frame[0]);
		fails++;
	}

	/*
	 * Negative control. Redo the fold the old way - a fixed 24 bytes - and
	 * confirm it produces exactly the corruption described above. Without
	 * this, a test that merely passes proves nothing about what it caught.
	 */
	{
		uint8_t again[256];
		uint8_t *h;

		memcpy(again, buf, sizeof again);
		h = again + MT_DMA_HDR_LEN + MT_RXWI_LEN;
		/* re-lay the pre-fold bytes, since mt_rx_parse mutated buf */
		memset(h, 0, hdrlen + pad + sizeof body);
		h[0] = 0x88;
		h[24] = 0x07;
		h[25] = 0x00;
		memset(h + hdrlen, 0xaa, pad);
		memcpy(h + hdrlen + pad, body, sizeof body);

		memmove(h + 2, h, 24);          /* the old, fixed-24 fold */
		if (h[2 + 24] == 0x07) {
			printf("  FAIL negative control: the old fold preserved "
			       "QoS Control, so this test could not have caught it\n");
			fails++;
		}
	}
}

/*
 * Radiotap VHT bandwidth: the code names a width *and* a sub-channel within
 * it, so "40 (20L)" is a 20 MHz frame. Mapping 1-3 to 40 and >=4 to 80 aired
 * a requested 20-in-40 at 40 MHz.
 */
static void test_vht_bandwidth(void)
{
	/* code -> expected width, from the radiotap VHT bandwidth table */
	static const struct { uint8_t code; enum mt7612u_bw bw; const char *what; } cases[] = {
		{  0, MT7612U_BW_20, "20"        }, {  1, MT7612U_BW_40, "40"        },
		{  2, MT7612U_BW_20, "40 (20L)"  }, {  3, MT7612U_BW_20, "40 (20U)"  },
		{  4, MT7612U_BW_80, "80"        }, {  5, MT7612U_BW_40, "80 (40L)"  },
		{  6, MT7612U_BW_40, "80 (40U)"  }, {  7, MT7612U_BW_20, "80 (20LL)" },
		{  8, MT7612U_BW_20, "80 (20LU)" }, {  9, MT7612U_BW_20, "80 (20UL)" },
		{ 10, MT7612U_BW_20, "80 (20UU)" },
	};
	/* radiotap: present = VHT(21) only; then 12 bytes of VHT at offset 8,
	 * 2-byte aligned. known = BANDWIDTH, bandwidth byte at VHT+3. */
	uint8_t buf[8 + 12 + 32];
	struct mt7612u_tx_rate r;

	printf("radiotap VHT bandwidth:\n");
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		memset(buf, 0, sizeof buf);
		buf[2] = 20;                    /* radiotap length */
		buf[4] = 0x00; buf[5] = 0x00;
		buf[6] = 0x20; buf[7] = 0x00;   /* present bit 21 = VHT */
		buf[8] = 0x40;                  /* known: BANDWIDTH */
		buf[11] = cases[i].code;        /* VHT+3 = bandwidth */
		buf[12] = 0x10;                 /* mcs_nss: MCS1 NSS0 -> nss 1 */

		if (mt_radiotap_parse(buf, sizeof buf, &r) != 20) {
			printf("  FAIL code %u: header not parsed\n", cases[i].code);
			fails++;
			continue;
		}
		if (r.bw != cases[i].bw) {
			printf("  FAIL code %2u %-10s want bw %d got %d\n",
			       cases[i].code, cases[i].what, (int)cases[i].bw, (int)r.bw);
			fails++;
		}
	}
}

int main(void)
{
	test_hdrlen();
	test_rx_l2pad();
	test_vht_bandwidth();
	printf("frame_shape: %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
