/* SPDX-License-Identifier: BSD-3-Clause-Clear */
/*
 * Async TX and RX rings over libusb, plus the event thread that drives them.
 * This is what mt76 gets from URBs and NAPI; here it is one pthread calling
 * libusb_handle_events plus two pools of libusb_transfer.
 *
 * RX: MT_RX_RING transfers permanently in flight on EP 4 IN. A completion
 * parses the RXWI and resubmits immediately, so the endpoint is never idle -
 * which is also what keeps the chip from wedging (BRINGUP-RESULTS.md).
 *
 * TX: a pool of MT_TX_RING transfers on EP 4 OUT with a free list. Submitting
 * does not wait for the wire; mt7612u_tx only blocks when every slot is in
 * flight, which is the back-pressure point.
 */
#include <stdlib.h>
#include <string.h>
#include "internal.h"

static void *evt_thread(void *arg)
{
	struct mt7612u_dev *d = arg;
	struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };

	while (d->a && d->a->running)
		libusb_handle_events_timeout_completed(d->ctx, &tv, NULL);
	return NULL;
}

static void LIBUSB_CALL rx_done(struct libusb_transfer *t)
{
	struct mt_slot *s = t->user_data;
	struct mt7612u_dev *d = s->d;
	struct mt_async *a = d->a;

	if (t->status == LIBUSB_TRANSFER_COMPLETED) {
		const uint8_t *frame = NULL;
		struct mt7612u_rx_info info;
		int len = mt_rx_parse(d, t->buffer, t->actual_length, &frame, &info);

		if (len > 0) {
			a->rx_frames++;
			if (a->cb)
				a->cb(a->cb_user, frame, (size_t)len, &info);
		}
	} else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
		a->rx_err++;
	}

	if (a->rx_active && t->status != LIBUSB_TRANSFER_CANCELLED) {
		if (libusb_submit_transfer(t))
			a->rx_err++;
	} else {
		a->rx_inflight--;
	}
}

static void LIBUSB_CALL tx_done(struct libusb_transfer *t)
{
	struct mt_slot *s = t->user_data;
	struct mt_async *a = s->d->a;

	if (t->status == LIBUSB_TRANSFER_COMPLETED &&
	    t->actual_length == t->length)
		a->tx_done_n++;
	else
		a->tx_err++;

	pthread_mutex_lock(&a->tx_lock);
	a->tx_busy[s->idx] = 0;
	a->tx_inflight--;
	pthread_cond_signal(&a->tx_cv);
	pthread_mutex_unlock(&a->tx_lock);
}

int mt_async_start(struct mt7612u_dev *d, mt7612u_rx_cb cb, void *user)
{
	struct mt_async *a;

	if (d->a) return 0;
	a = calloc(1, sizeof *a);
	if (!a) return -1;
	d->a = a;
	a->cb = cb;
	a->cb_user = user;
	pthread_mutex_init(&a->tx_lock, NULL);
	pthread_cond_init(&a->tx_cv, NULL);

	for (int i = 0; i < MT_TX_RING; i++) {
		a->tx_slot[i].d = d;
		a->tx_slot[i].idx = i;
		a->tx[i] = libusb_alloc_transfer(0);
		if (!a->tx[i]) goto fail;
	}
	for (int i = 0; i < MT_RX_RING; i++) {
		a->rx_slot[i].d = d;
		a->rx_slot[i].idx = i;
		a->rx[i] = libusb_alloc_transfer(0);
		if (!a->rx[i]) goto fail;
	}

	a->running = 1;
	if (pthread_create(&a->evt, NULL, evt_thread, d)) { a->running = 0; goto fail; }

	if (cb) {
		a->rx_active = 1;
		for (int i = 0; i < MT_RX_RING; i++) {
			libusb_fill_bulk_transfer(a->rx[i], d->h, MT_EP_IN_PKT_RX,
			                          a->rx_buf[i], MT_RX_BUFSZ,
			                          rx_done, &a->rx_slot[i], 0);
			if (libusb_submit_transfer(a->rx[i])) {
				ERR("could not submit RX transfer %d", i);
				goto fail;
			}
			a->rx_inflight++;
		}
		LOG("async: %d RX transfers in flight, %d TX slots",
		    MT_RX_RING, MT_TX_RING);
	} else {
		LOG("async: %d TX slots (RX ring not started)", MT_TX_RING);
	}
	return 0;

fail:
	mt_async_stop(d);
	return -1;
}

void mt_async_stop(struct mt7612u_dev *d)
{
	struct mt_async *a = d->a;

	if (!a) return;

	a->rx_active = 0;
	for (int i = 0; i < MT_RX_RING; i++)
		if (a->rx[i]) libusb_cancel_transfer(a->rx[i]);

	/* Let the in-flight TX drain before tearing the event thread down. */
	pthread_mutex_lock(&a->tx_lock);
	for (int spins = 0; a->tx_inflight && spins < 200; spins++) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		ts.tv_nsec += 10000000;
		if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
		pthread_cond_timedwait(&a->tx_cv, &a->tx_lock, &ts);
	}
	pthread_mutex_unlock(&a->tx_lock);

	for (int spins = 0; a->rx_inflight && spins < 100; spins++)
		mt_usleep(10000);

	a->running = 0;
	pthread_join(a->evt, NULL);

	for (int i = 0; i < MT_TX_RING; i++)
		if (a->tx[i]) libusb_free_transfer(a->tx[i]);
	for (int i = 0; i < MT_RX_RING; i++)
		if (a->rx[i]) libusb_free_transfer(a->rx[i]);
	pthread_mutex_destroy(&a->tx_lock);
	pthread_cond_destroy(&a->tx_cv);
	free(a);
	d->a = NULL;
}

/*
 * Hand a fully framed buffer to the TX pool. Blocks only when every slot is
 * in flight. Returns 0 on submit, -1 on error.
 */
int mt_async_tx_submit(struct mt7612u_dev *d, const uint8_t *buf, int len)
{
	struct mt_async *a = d->a;
	int idx = -1;

	if (!a || len > MT_TX_BUFSZ) return -1;

	pthread_mutex_lock(&a->tx_lock);
	for (;;) {
		for (int i = 0; i < MT_TX_RING; i++)
			if (!a->tx_busy[i]) { idx = i; break; }
		if (idx >= 0) break;
		pthread_cond_wait(&a->tx_cv, &a->tx_lock);
	}
	a->tx_busy[idx] = 1;
	a->tx_inflight++;
	pthread_mutex_unlock(&a->tx_lock);

	memcpy(a->tx_buf[idx], buf, (size_t)len);
	libusb_fill_bulk_transfer(a->tx[idx], d->h, MT_EP_OUT_AC_BE,
	                          a->tx_buf[idx], len, tx_done,
	                          &a->tx_slot[idx], 1000);
	if (libusb_submit_transfer(a->tx[idx])) {
		pthread_mutex_lock(&a->tx_lock);
		a->tx_busy[idx] = 0;
		a->tx_inflight--;
		pthread_mutex_unlock(&a->tx_lock);
		a->tx_err++;
		return -1;
	}
	a->tx_submitted++;
	return 0;
}

int mt7612u_rx_start(struct mt7612u_dev *d, mt7612u_rx_cb cb, void *user)
{
	if (!cb) return -1;
	return mt_async_start(d, cb, user);
}

int mt7612u_rx_stop(struct mt7612u_dev *d)
{
	mt_async_stop(d);
	return 0;
}
