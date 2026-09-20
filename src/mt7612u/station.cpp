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
	uint8_t port[6];
	uint32_t rsp = 0;

	if (!dev || !own || !bssid)
		return -1;

	/* Both must be real unicast addresses, and they must differ - a station
	 * whose own address is its BSSID is not a station. */
	if ((own[0] & 0x01) || (bssid[0] & 0x01))
		return -1;
	if (memcmp(own, bssid, 6) == 0)
		return -1;

	/*
	 * `own` must already BE the port identity. This is a check and not a
	 * write, and the difference is the measurement in the file header: this
	 * register is co-owned with the beacon path and the ACK responder, and
	 * moving it is precisely what takes a station from 0.8% to 98% retried
	 * downlink frames.
	 *
	 * Refusing is the useful behaviour. A caller asking us to be an address
	 * the MAC is not holding has either armed an ACK responder or a beacon
	 * underneath itself, or has the wrong adapter - and in every one of
	 * those cases quietly writing the register would produce a station that
	 * half works, which is the failure mode this part specialises in.
	 */
	if (sta_read_port_identity(dev, port) != 0) {
		WARN("station identity refused: could not read the MAC's port "
		     "identity back, so there is nothing to verify `own` against");
		return -1;
	}
	if (memcmp(port, own, 6) != 0) {
		WARN("station identity refused: the MAC's port identity is "
		     "%02x:%02x:%02x:%02x:%02x:%02x, not the requested "
		     "%02x:%02x:%02x:%02x:%02x:%02x. Something else owns it (a "
		     "beacon or an ACK responder); moving it here would stop this "
		     "station acknowledging its own traffic.",
		     port[0], port[1], port[2], port[3], port[4], port[5],
		     own[0], own[1], own[2], own[3], own[4], own[5]);
		return -1;
	}

	/*
	 * The auto-response engine's enable. init leaves this on, and the
	 * measured auto-ACK depends on it, so a station that finds it clear is
	 * not going to work and should say so now rather than at the first lost
	 * downlink frame. Read-only: if it is off, something deliberate turned
	 * it off and silently re-enabling it would hide that.
	 */
	if (mt_rr_chk(dev, MT_AUTO_RSP_CFG, &rsp) != 0) {
		/* Fail CLOSED, like the port-identity check two blocks up. The
		 * first version of this used `== 0 && !(rsp & EN)`, so a stalled
		 * EP0 read skipped the check entirely and armed anyway - the two
		 * adjacent reads had opposite failure semantics and neither said
		 * so. If we could not find out whether this MAC will acknowledge
		 * anything, we do not get to claim it will. */
		WARN("station identity refused: could not read MT_AUTO_RSP_CFG, so "
		     "whether this MAC will acknowledge unicast addressed to it is "
		     "unknown");
		return -1;
	}
	if (!(rsp & MT_AUTO_RSP_EN)) {
		WARN("station identity refused: MT_AUTO_RSP_EN is CLEAR (cfg %08x) "
		     "- this MAC will not acknowledge unicast addressed to it, and "
		     "an AP will retransmit every downlink frame until it gives up",
		     rsp);
		return -1;
	}

	/*
	 * NOT written, deliberately: MT_MAC_BSSID and the MT_MAC_APC_BSSID slot
	 * table. Measured to make no difference to what a managed station
	 * receives - correct, wrong or absent - and MT_MAC_BSSID already has two
	 * owners (mac_setaddr at init and the beacon path). Adding a third
	 * writer to a register that is not needed would recreate the exact
	 * co-ownership hazard this seam was split out to avoid, in exchange for
	 * nothing measurable.
	 *
	 * The BSSID is still recorded, because the host needs it - every frame
	 * this station transmits carries it as addr3 - and because a later
	 * revision that finds a hardware use for it (power save, TIM parsing,
	 * per-BSS key lookup: all untested) should find the value already here.
	 */
	memcpy(dev->sta_bssid, bssid, 6);
	dev->sta_armed = 1;
	return 0;
}

void mt7612u_clear_station_identity(struct mt7612u_dev *dev)
{
	if (!dev)
		return;
	/* Nothing to undo in hardware - this seam never wrote any. That is a
	 * property of this part and not a promise of the interface. */
	memset(dev->sta_bssid, 0, sizeof dev->sta_bssid);
	dev->sta_armed = 0;
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
	if (!dev || !dev->sta_armed)
		return;
	WARN("station identity DROPPED: %s is moving MT_MAC_ADDR away from this "
	     "station's own address. This MAC acknowledges unicast by matching "
	     "address 1 against that register, so the station stops being "
	     "acknowledged from here on - measured as 0.8%% -> 98%% retried "
	     "downlink frames. Re-arm the station identity after %s releases it.",
	     who, who);
	dev->sta_armed = 0;
	memset(dev->sta_bssid, 0, sizeof dev->sta_bssid);
}

int mt7612u_station_bssid(struct mt7612u_dev *dev, uint8_t out[6])
{
	if (!dev || !out || !dev->sta_armed)
		return -1;
	memcpy(out, dev->sta_bssid, 6);
	return 0;
}
