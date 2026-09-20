/*
 * StationIdentity.h - the decision half of SetStationIdentity, with no I/O.
 *
 * station.cpp reads three things off the chip (the port identity, whether that
 * read worked, and MT_AUTO_RSP_CFG) and then decides whether to arm. The
 * reading needs a device; the deciding does not, and every defect this logic
 * has had was in the deciding:
 *
 *   - the MT_AUTO_RSP_EN check failed OPEN on a failed register read while the
 *     port-identity check beside it failed CLOSED, so a stalled EP0 transfer
 *     armed a station whose ability to acknowledge was unknown;
 *   - the port-identity read relied on zero-initialised locals, so a failed
 *     read produced the address 00:00:00:00:00:00 and refused for the wrong
 *     reason, reporting a port identity the MAC never held.
 *
 * Neither is visible from a register trace and neither needs hardware to
 * catch. Splitting them out is what lets tests/mt7612u_station_selftest.cpp
 * exercise every branch in ctest - which the Phase 2 plan asked for and the
 * first implementation did not provide.
 *
 * Pure: no device type, no register access, no logging. Safe to include from
 * a test with nothing but -I src/mt7612u.
 */
#ifndef MT7612U_STATION_IDENTITY_H
#define MT7612U_STATION_IDENTITY_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum mt7612u_sta_verdict {
	MT7612U_STA_OK = 0,
	/* A caller mistake: null pointers, a multicast address, or `own` and
	 * `bssid` the same - a station whose own address is its BSSID is not a
	 * station. */
	MT7612U_STA_BAD_ARGS,
	MT7612U_STA_MULTICAST,
	MT7612U_STA_SAME_ADDR,
	/* Could not read the chip. Refused, not assumed: if we cannot find out
	 * whether this MAC will acknowledge anything, we do not get to claim it
	 * will. */
	MT7612U_STA_READ_FAILED,
	/* `own` is not the address the MAC is holding. Something else owns the
	 * port identity - a beacon, or an ACK responder - and moving it here
	 * would make this station deaf: measured, reception goes to zero.
	 * docs/mt7612u-station-identity.md. */
	MT7612U_STA_PORT_MISMATCH,
	/* The auto-response engine is switched off. Whatever did that did it
	 * deliberately, so this refuses rather than silently re-enabling it. */
	MT7612U_STA_AUTO_RSP_OFF
};

/*
 * `port_ok` / `rsp_ok` are whether the corresponding register read SUCCEEDED,
 * not whether its value is acceptable. Passing 0 for either must refuse -
 * that asymmetry is the bug this file exists to make testable.
 *
 * `auto_rsp_en_mask` is MT_AUTO_RSP_EN, passed in so this header needs no
 * register definitions.
 */
static inline enum mt7612u_sta_verdict
mt7612u_sta_decide(const uint8_t *own, const uint8_t *bssid,
                   const uint8_t *port, int port_ok,
                   uint32_t auto_rsp_cfg, int rsp_ok,
                   uint32_t auto_rsp_en_mask)
{
	if (!own || !bssid || !port)
		return MT7612U_STA_BAD_ARGS;
	if ((own[0] & 0x01) || (bssid[0] & 0x01))
		return MT7612U_STA_MULTICAST;
	if (memcmp(own, bssid, 6) == 0)
		return MT7612U_STA_SAME_ADDR;
	if (!port_ok)
		return MT7612U_STA_READ_FAILED;
	if (memcmp(port, own, 6) != 0)
		return MT7612U_STA_PORT_MISMATCH;
	if (!rsp_ok)
		return MT7612U_STA_READ_FAILED;
	if (!(auto_rsp_cfg & auto_rsp_en_mask))
		return MT7612U_STA_AUTO_RSP_OFF;
	return MT7612U_STA_OK;
}

/*
 * The ownership hand-off, as a pure state machine.
 *
 * SetStationIdentity's check is one-shot: it verifies the port identity when
 * it arms and has no further say. A beacon or an ACK responder armed LATER
 * moves MT_MAC_ADDR out from under a live station, which is the measured
 * "reception goes to zero" failure with no diagnostic at all - and it is the
 * ordering a real caller is likelier to hit than the one the arm-time check
 * covers.
 *
 * This does not veto those paths. They are older, they have their own callers,
 * and a station arm is not entitled to refuse them. What it does is stop a
 * stale `armed` flag from going on claiming a station is configured after its
 * identity has been taken.
 */
struct mt7612u_sta_state {
	uint8_t bssid[6];
	int armed;
};

static inline void mt7612u_sta_arm(struct mt7612u_sta_state *s,
                                   const uint8_t *bssid)
{
	memcpy(s->bssid, bssid, 6);
	s->armed = 1;
}

static inline void mt7612u_sta_clear(struct mt7612u_sta_state *s)
{
	memset(s->bssid, 0, sizeof s->bssid);
	s->armed = 0;
}

/* Returns 1 if a live station identity was just invalidated, so the caller
 * knows whether to say anything. Idempotent: taking the identity twice is not
 * two losses. */
static inline int mt7612u_sta_identity_taken(struct mt7612u_sta_state *s)
{
	if (!s->armed)
		return 0;
	mt7612u_sta_clear(s);
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* MT7612U_STATION_IDENTITY_H */
