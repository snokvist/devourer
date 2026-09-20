/*
 * station.cpp — the MT7612U half of IRadio::SetStationIdentity.
 *
 * This file is small, and it is small BECAUSE of a measurement rather than in
 * spite of one. docs/mt7612u-station-identity.md has the numbers; the short
 * version is that on this part a station needs almost nothing programmed, and
 * the one thing it must not do is the thing that looks most like the job.
 *
 * WHAT WAS MEASURED (bringup's `sta` and `staack` gates, against hostapd on
 * independent silicon):
 *
 *   - The BSSID registers do not gate a managed station's receive. Six arms
 *     across MT_MAC_BSSID and the APC slot table, including one with BOTH
 *     deliberately programmed WRONG, all received the same beacons and the
 *     same unicast addressed to us: 5877 frames wrong vs 6250 with nothing
 *     programmed. So this function does NOT write them.
 *
 *   - Moving MT_MAC_ADDR makes a station DEAF. With the managed receive
 *     filter in force, retargeting the port identity took reception of the
 *     AP's unicast from 103 frames to ZERO. So this function does NOT write
 *     MT_MAC_ADDR either - it CHECKS it, and refuses if it has moved.
 *
 * That second number is the whole reason this seam exists separately from
 * mt7612u_set_ack_responder(). Arming an ACK responder on this part retargets
 * the port identity, so SetAckResponder(bssid) on a station would silence AND
 * deafen it. A station must leave MT_MAC_ADDR exactly where MAC bring-up put
 * it.
 *
 * NOTE what is NOT among the reasons: whether this MAC auto-ACKs. The scope
 * document asserts it does, from a register reading, and an earlier revision
 * of this header quoted "0.8% vs 98.0% retried" as measurement of it. That
 * control turned out to have run with the MONITOR filter installed by
 * mistake and is withdrawn; the clean single-variable control (clear
 * MT_AUTO_RSP_EN, hold reception constant) does not move, so the method is
 * void and auto-ACK is UNMEASURED. Nothing here depends on the answer - this
 * function makes no call either way - but do not repeat the claim.
 *
 * So the useful work here is refusal and verification, not configuration.
 */
#include <string.h>

#include "StationIdentity.h"
#include "internal.h"
#include "regs.h"

/*
 * Read the port identity back out of the hardware. This is what the
 * auto-response engine matches an incoming frame's address 1 against, and on
 * this part it is the entire mechanism behind a station's auto-ACK.
 */
static int sta_read_port_identity(struct mt7612u_dev *d, uint8_t out[6])
{
	uint32_t dw0 = 0, dw1 = 0;

	/* Checked, not assumed. mt_rr_chk() leaves *val untouched when the
	 * transfer fails, so the old form's zero-initialised locals turned a
	 * failed read into the address 00:00:00:00:00:00 - which then failed
	 * the comparison and refused for the wrong reason, reporting a port
	 * identity the MAC never held. */
	if (mt_rr_chk(d, MT_MAC_ADDR_DW0, &dw0) != 0 ||
	    mt_rr_chk(d, MT_MAC_ADDR_DW1, &dw1) != 0)
		return -1;
	out[0] = (uint8_t)(dw0 & 0xff);
	out[1] = (uint8_t)((dw0 >> 8) & 0xff);
	out[2] = (uint8_t)((dw0 >> 16) & 0xff);
	out[3] = (uint8_t)((dw0 >> 24) & 0xff);
	out[4] = (uint8_t)(dw1 & 0xff);
	out[5] = (uint8_t)((dw1 >> 8) & 0xff);
	return 0;
}

int mt7612u_set_station_identity(struct mt7612u_dev *dev,
                                 const uint8_t own[6], const uint8_t bssid[6])
{
	uint8_t port[6] = { 0 };
	uint32_t rsp = 0;
	int port_ok, rsp_ok;
	enum mt7612u_sta_verdict v;

	if (!dev)
		return -1;

	/* Read, then decide. The deciding is in StationIdentity.h so that every
	 * branch below - including both failed-read paths, which is where this
	 * logic has been wrong twice - is exercised headlessly by
	 * tests/mt7612u_station_selftest.cpp rather than only on a device. */
	port_ok = (sta_read_port_identity(dev, port) == 0);
	rsp_ok  = (mt_rr_chk(dev, MT_AUTO_RSP_CFG, &rsp) == 0);

	v = mt7612u_sta_decide(own, bssid, port, port_ok, rsp,
	                       rsp_ok, MT_AUTO_RSP_EN);
	switch (v) {
	case MT7612U_STA_OK:
		break;
	case MT7612U_STA_BAD_ARGS:
		WARN("station identity refused: null address");
		return -1;
	case MT7612U_STA_MULTICAST:
		WARN("station identity refused: own and bssid must both be unicast");
		return -1;
	case MT7612U_STA_SAME_ADDR:
		WARN("station identity refused: own == bssid");
		return -1;
	case MT7612U_STA_READ_FAILED:
		WARN("station identity refused: could not read the MAC back, so "
		     "there is nothing to verify against - refusing rather than "
		     "arming a station whose ability to receive and acknowledge is "
		     "unknown");
		return -1;
	case MT7612U_STA_PORT_MISMATCH:
		WARN("station identity refused: the MAC's port identity is "
		     "%02x:%02x:%02x:%02x:%02x:%02x, not the requested "
		     "%02x:%02x:%02x:%02x:%02x:%02x. Something else owns it (a "
		     "beacon or an ACK responder); moving it here would make this "
		     "station DEAF - measured, reception goes to zero.",
		     port[0], port[1], port[2], port[3], port[4], port[5],
		     own[0], own[1], own[2], own[3], own[4], own[5]);
		return -1;
	case MT7612U_STA_AUTO_RSP_OFF:
		WARN("station identity refused: MT_AUTO_RSP_EN is CLEAR (cfg %08x) "
		     "- the auto-response engine is switched off", rsp);
		return -1;
	}

	/*
	 * NOT written, deliberately: MT_MAC_ADDR, MT_MAC_BSSID and the
	 * MT_MAC_APC_BSSID slot table. The first must not move - that is the
	 * measured "reception goes to zero" failure. The other two make no
	 * measurable difference to what a managed station receives, correct or
	 * wrong, and MT_MAC_BSSID already has two owners; a third writer on a
	 * register nothing needs would recreate the hazard this seam exists to
	 * avoid. docs/mt7612u-station-identity.md.
	 *
	 * The BSSID is recorded because a later revision that finds a hardware
	 * use for it (power save, TIM parsing, per-BSS key lookup: all
	 * untested) should find the value already here.
	 */
	mt7612u_sta_arm(&dev->sta, bssid);
	return 0;
}

void mt7612u_clear_station_identity(struct mt7612u_dev *dev)
{
	if (!dev)
		return;
	/* Nothing to undo in hardware - this seam never wrote any. That is a
	 * property of this part and not a promise of the interface. */
	mt7612u_sta_clear(&dev->sta);
}

/*
 * Called by every path that is about to move MT_MAC_ADDR.
 *
 * SetStationIdentity's check is one-shot: it verifies the port identity at
 * arm time and then has no further say. Nothing stopped an ACK responder or a
 * beacon armed AFTERWARDS from moving the register out from under a live
 * station, which is the measured 0.8% -> 98% failure with no diagnostic at
 * all - and it is the ordering a real caller is more likely to hit than the
 * one gate_staid checks.
 *
 * This does not refuse. The beacon and responder paths are older, have their
 * own callers, and a station arm is not entitled to veto them. What it does
 * is make the consequence audible and stop the stale `sta_armed` from
 * claiming a station is still configured when its identity has been taken.
 */
void mt7612u_station_identity_lost(struct mt7612u_dev *dev, const char *who)
{
	if (!dev)
		return;
	if (!mt7612u_sta_identity_taken(&dev->sta))
		return;
	WARN("station identity DROPPED: %s is moving MT_MAC_ADDR away from this "
	     "station's own address. Measured on this part, that takes the "
	     "station's reception of the AP's unicast to ZERO - it goes deaf, "
	     "not merely silent. Re-arm the station identity after %s releases "
	     "it.", who, who);
}

int mt7612u_station_bssid(struct mt7612u_dev *dev, uint8_t out[6])
{
	if (!dev || !out || !dev->sta.armed)
		return -1;
	memcpy(out, dev->sta.bssid, 6);
	return 0;
}
