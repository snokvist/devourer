/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * libusb transport for MT7612U. Replaces mt76/usb.c's vendor-request and URB
 * plumbing; the wire encoding is identical (verified against usbmon, see
 * ../../INVESTIGATION.md §11).
 */
#include <string.h>
#include <time.h>
#include "internal.h"

#define REQ_IN   (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
#define REQ_OUT  (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE)
/* mt76's MT_VEND_REQ_TOUT_MS / MT_VEND_REQ_MAX_RETRY. The product of the two
 * is the worst-case cost of one register access, so it bounds every poll
 * loop below - which is why the timeout is 300 ms and not something longer. */
#define CTRL_TIMEOUT_MS 300
#define VEND_RETRIES 10

void mt_usleep(unsigned us)
{
	struct timespec ts = { .tv_sec = us / 1000000, .tv_nsec = (us % 1000000) * 1000 };
	nanosleep(&ts, NULL);
}

static uint64_t now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

int mt_vendor_req(struct mt7612u_dev *d, uint8_t req, uint8_t type,
                  uint16_t val, uint16_t idx, void *buf, size_t len)
{
	int rc = LIBUSB_ERROR_OTHER;

	for (int i = 0; i < VEND_RETRIES; i++) {
		rc = libusb_control_transfer(d->h, type, req, val, idx,
		                             (unsigned char *)buf, (uint16_t)len,
		                             CTRL_TIMEOUT_MS);
		if (rc >= 0 || rc == LIBUSB_ERROR_NO_DEVICE)
			return rc;
		mt_usleep(5000);
	}
	ERR("vendor req %02x idx %04x failed: %s", req, idx, libusb_error_name(rc));
	return rc;
}

/* Address bits 31:30 select the space, exactly as mt76's __mt76u_rr/wr do. */
static uint8_t rd_req(uint32_t addr)
{
	if (addr & MT_VEND_TYPE_EEPROM) return MT_VEND_READ_EEPROM;
	if (addr & MT_VEND_TYPE_CFG)    return MT_VEND_READ_CFG;
	return MT_VEND_MULTI_READ;
}

static uint8_t wr_req(uint32_t addr)
{
	if (addr & MT_VEND_TYPE_CFG) return MT_VEND_WRITE_CFG;
	return MT_VEND_MULTI_WRITE;
}

/*
 * A register read that reports failure separately from the value. This matters
 * because 0xffffffff is a legitimate read on this part - MT_MAC_CSR0 returns
 * it while the core is still coming up - so it cannot double as a sentinel.
 * Returns 0 and fills *val on success, -1 on a transport failure.
 */
int mt_rr_chk(struct mt7612u_dev *d, uint32_t addr, uint32_t *val)
{
	uint8_t req = rd_req(addr), b[4] = { 0 };
	uint32_t a = addr & ~MT_VEND_TYPE_MASK;

	if (mt_vendor_req(d, req, REQ_IN, (uint16_t)(a >> 16), (uint16_t)a,
	                  b, sizeof b) != (int)sizeof b) {
		d->io_err++;
		return -1;
	}
	*val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
	       ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
	return 0;
}

/* Convenience form for the places that genuinely cannot act on a failure
 * (logging, one-shot identity reads). It still bumps d->io_err, so a caller
 * that cares can notice after the fact. */
uint32_t mt_rr(struct mt7612u_dev *d, uint32_t addr)
{
	uint32_t v;

	return mt_rr_chk(d, addr, &v) ? ~0u : v;
}

void mt_wr(struct mt7612u_dev *d, uint32_t addr, uint32_t val)
{
	uint8_t req = wr_req(addr), b[4];
	uint32_t a = addr & ~MT_VEND_TYPE_MASK;

	b[0] = val & 0xff; b[1] = (val >> 8) & 0xff;
	b[2] = (val >> 16) & 0xff; b[3] = (val >> 24) & 0xff;
	mt_vendor_req(d, req, REQ_OUT, (uint16_t)(a >> 16), (uint16_t)a, b, sizeof b);

	/* Oracle-diff log: same shape decode.py renders from usbmon. */
	if (d->wrlog)
		fprintf(d->wrlog, "req=0x%02x addr=0x%04x data=%02x%02x%02x%02x\n",
		        req, (unsigned)(a & 0xffff), b[0], b[1], b[2], b[3]);
}

/*
 * Read-modify-write. A failed read MUST NOT be written back: mt_rr's ~0u would
 * turn the operation into "set every bit", and the addresses this is used on
 * (MT_WLAN_FUN_CTRL, MT_MAC_SYS_CTRL, the BBP AGC block) are exactly the ones
 * where that is destructive. Returns 0 on success, -1 if nothing was written.
 */
int mt_rmw(struct mt7612u_dev *d, uint32_t addr, uint32_t mask, uint32_t val)
{
	uint32_t cur;

	if (mt_rr_chk(d, addr, &cur)) {
		ERR("rmw 0x%05x skipped: read failed", addr & ~MT_VEND_TYPE_MASK);
		return -1;
	}
	mt_wr(d, addr, (cur & ~mask) | val);
	return 0;
}

/*
 * Poll against a real deadline rather than a count of sleeps. One register
 * access can itself cost up to VEND_RETRIES * CTRL_TIMEOUT_MS, so counting
 * iterations would let a caller asking for 200 ms block for seconds.
 */
int mt_poll(struct mt7612u_dev *d, uint32_t addr, uint32_t mask,
            uint32_t val, int timeout_us)
{
	uint64_t deadline = now_us() + (uint64_t)(timeout_us < 0 ? 0 : timeout_us);

	for (;;) {
		uint32_t cur;

		if (mt_rr_chk(d, addr, &cur)) {
			ERR("poll 0x%05x aborted: read failed",
			    addr & ~MT_VEND_TYPE_MASK);
			return 0;
		}
		if ((cur & mask) == val)
			return 1;
		if (now_us() >= deadline)
			return 0;
		mt_usleep(1000);
	}
}

void mt_single_wr(struct mt7612u_dev *d, uint8_t req, uint16_t off, uint32_t val)
{
	mt_vendor_req(d, req, REQ_OUT, (uint16_t)(val & 0xffff), off, NULL, 0);
	mt_vendor_req(d, req, REQ_OUT, (uint16_t)(val >> 16), (uint16_t)(off + 2), NULL, 0);
}

int mt_bulk(struct mt7612u_dev *d, uint8_t ep, void *buf, int len,
            int *xfered, unsigned timeout_ms)
{
	int n = 0;
	int rc = libusb_bulk_transfer(d->h, ep, (unsigned char *)buf, len,
	                              &n, timeout_ms);
	if (xfered) *xfered = n;
	return rc;
}

/* mt76x02_wait_for_mac(): MAC_CSR0 reads 0 or ~0 until the core is alive.
 * Both are legitimate values here, which is why this uses the checked read -
 * a transport failure is a different condition from "still coming up". */
int mt_wait_for_mac(struct mt7612u_dev *d)
{
	for (int i = 0; i < 500; i++) {
		uint32_t v;

		if (!mt_rr_chk(d, MT_MAC_CSR0, &v) && v != 0 && v != ~0u)
			return 1;
		mt_usleep(5000);
	}
	return 0;
}

int mt_open(struct mt7612u_dev *d, const char **err)
{
	int rc;

	if (libusb_init(&d->ctx)) { if (err) *err = "libusb_init failed"; return -1; }

	d->h = libusb_open_device_with_vid_pid(d->ctx, MT7612U_VID, MT7612U_PID);
	if (!d->h) {
		if (err) *err = "MT7612U not found or permission denied (try sudo)";
		libusb_exit(d->ctx); d->ctx = NULL;
		return -1;
	}

	d->kernel_was_attached = libusb_kernel_driver_active(d->h, 0) == 1;
	if (d->kernel_was_attached) {
		rc = libusb_detach_kernel_driver(d->h, 0);
		if (rc) {
			if (err) *err = "could not detach mt76x2u";
			goto fail;
		}
		LOG("detached kernel driver from interface 0");
	}

	/* A USB port reset before claiming. Without it the chip keeps whatever
	 * FCE/DMA state the previous run left behind, and the next firmware
	 * upload times out mid-chunk - reproducible after a few init cycles.
	 * devourer does the same thing on open for the same reason. */
	rc = libusb_reset_device(d->h);
	if (rc == LIBUSB_ERROR_NOT_FOUND) {
		/* Re-enumerated under a new address: reopen and re-detach. */
		libusb_close(d->h);
		mt_usleep(200000);
		d->h = libusb_open_device_with_vid_pid(d->ctx, MT7612U_VID, MT7612U_PID);
		if (!d->h) {
			if (err) *err = "device vanished after USB reset";
			libusb_exit(d->ctx); d->ctx = NULL;
			return -1;
		}
		if (libusb_kernel_driver_active(d->h, 0) == 1)
			libusb_detach_kernel_driver(d->h, 0);
	} else if (rc) {
		LOG("warning: USB reset returned %s", libusb_error_name(rc));
	}

	rc = libusb_claim_interface(d->h, 0);
	if (rc) {
		if (err) *err = "could not claim interface 0 (another process using it?)";
		goto fail;
	}

	/* 0.5 dB units, as mt76's txpower_conf = power_level * 2. 20 dBm is a
	 * conservative seed; mt7612u_set_txpower() overrides it. */
	if (!d->txpower_conf)
		d->txpower_conf = 40;

	d->rev = mt_rr(d, MT_ASIC_VERSION);
	if ((d->rev >> 16) != 0x7612) {
		if (err) *err = "not an MT7612 (unexpected MT_ASIC_VERSION)";
		libusb_release_interface(d->h, 0);
		goto fail;
	}
	return 0;

fail:
	if (d->kernel_was_attached)
		libusb_attach_kernel_driver(d->h, 0);
	libusb_close(d->h); d->h = NULL;
	libusb_exit(d->ctx); d->ctx = NULL;
	return -1;
}

void mt_close(struct mt7612u_dev *d)
{
	if (d->wrlog) { fclose(d->wrlog); d->wrlog = NULL; }
	if (d->mculog) { fclose(d->mculog); d->mculog = NULL; }
	if (d->h) {
		libusb_release_interface(d->h, 0);
		if (d->kernel_was_attached && !d->keep_detached) {
			if (libusb_attach_kernel_driver(d->h, 0) == 0)
				LOG("reattached kernel driver");
		}
		libusb_close(d->h); d->h = NULL;
	}
	if (d->ctx) { libusb_exit(d->ctx); d->ctx = NULL; }
}

/* Block write, as mt76u_copy(): one MULTI_WRITE per batch, wValue 0.
 * The kernel uses this for the WCID address table (8 B) and the shared-key
 * table (32 B) - 192 transfers that would otherwise be ~700 4-byte writes. */
void mt_wr_copy(struct mt7612u_dev *d, uint32_t offset, const void *data, int len)
{
	const uint8_t *p = data;
	uint8_t buf[64];

	/* The hardware wants whole 32-bit words, but only `len` bytes belong to
	 * the caller. Round the *transfer* up and zero-fill the tail; rounding
	 * `len` up instead reads past the end of the caller's buffer. */
	for (int i = 0; i < len; ) {
		int n = len - i, xfer;

		if (n > (int)sizeof buf) n = (int)sizeof buf;
		xfer = (n + 3) & ~3;
		memset(buf, 0, (size_t)xfer);
		memcpy(buf, p + i, (size_t)n);
		if (mt_vendor_req(d, MT_VEND_MULTI_WRITE, REQ_OUT, 0,
		                  (uint16_t)(offset + i), buf, (size_t)xfer) < 0)
			return;
		i += n;
	}
}

/* --- public lifecycle helpers that belong with the transport --- */

void mt7612u_keep_detached(struct mt7612u_dev *d, int keep)
{
	if (d) d->keep_detached = keep;
}

uint32_t mt7612u_asic_version(const struct mt7612u_dev *d)
{
	return d ? d->rev : 0;
}

const uint8_t *mt7612u_mac_addr(const struct mt7612u_dev *d)
{
	return d ? d->macaddr : NULL;
}
