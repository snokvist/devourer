#pragma once

/* StationArm - the per-device state behind IRadio::SetStationIdentity on the
 * Realtek generations that share the port-0 register map (Jaguar1/2/3).
 *
 * The register recipe lives in AckResponder.h (arm_station / station_is /
 * restore_station); this holds the one piece of state the seam's contract
 * needs on top of it - the exact pre-arm port snapshot - and the refusal
 * rules, so the three backends carry a call rather than three copies of the
 * logic. The caller supplies the lock (each backend serializes register
 * access its own way) and its own port-0 ownership checks.
 *
 * REFUSALS, all logged, none throwing:
 *   - the seam's argument rule (both unicast, different);
 *   - port 0 already has a net_type set by someone else on the FIRST arm:
 *     a beacon or an ACK responder owns it, and arming over it would take
 *     the port away from them. A re-arm (a new BSSID while armed) is not
 *     refused - it replaces our own arm and keeps the original snapshot, so
 *     Clear still returns to the state before the FIRST arm;
 *   - any readback that does not match. A FAILED arm - first or re-arm -
 *     rolls back to the pre-FIRST-arm snapshot and, once that verifies, is
 *     no longer armed: a re-arm that fails tears down the arm before it
 *     rather than guessing that the old identity still holds, and returns
 *     false so the caller knows the port is passive.
 *
 * ORDERING (IRadio's clause): order-independent in fact on Jaguar1/2/3 - no
 * RX-loop start writes 0x0102/0x0610/0x0618 there (only bring-up, the beacon
 * and the ACK responder do) - so the backends require bring-up and nothing
 * else. */

#include <cstdint>
#include <optional>

#include "AckResponder.h"
#include "IRadio.h"
#include "logger.h"

namespace devourer {

class StationArm {
public:
  bool armed() const { return _restore.has_value(); }

  /* Drop the snapshot WITHOUT touching the chip. Called at the start of a
   * (re-)bring-up: Init/InitWrite power-cycle the MAC, which wipes the port-0
   * state the snapshot describes, so there is nothing left to restore - and
   * a stale snapshot would keep armed() true, refusing every later
   * SetAckResponder/StartBeacon on a chip that holds no station arm. Caller
   * holds the backend's station lock. */
  void forget() { _restore.reset(); }

  bool arm(RtlAdapter &dev, const MacAddr &own, const MacAddr &bssid,
           const Logger_t &log, const char *tag) {
    if (!ack::station_args_ok(own.data(), bssid.data())) {
      log->error("{}: station identity refused: own and BSSID must both be "
                 "unicast and must differ",
                 tag);
      return false;
    }
    if (!_restore) {
      ack::StationRestore snap;
      if (!ack::snapshot_station_restore(dev, snap)) {
        log->error("{}: station identity refused: the port-0 state could "
                   "not be read, so no rollback target exists",
                   tag);
        return false;
      }
      if (snap.net_type != 0) {
        log->error("{}: station identity refused: port 0 already has "
                   "net_type {} (a beacon or an ACK responder owns it, or "
                   "an earlier session left it set)",
                   tag, snap.net_type);
        return false;
      }
      _restore = snap;
    }
    (void)ack::arm_station(dev, own.data(), bssid.data());
    if (ack::station_is(dev, own.data(), bssid.data())) {
      log->info("{}: station identity armed: own "
                "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} BSSID "
                "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x} (net_type=Infra)",
                tag, own.bytes[0], own.bytes[1], own.bytes[2], own.bytes[3],
                own.bytes[4], own.bytes[5], bssid.bytes[0], bssid.bytes[1],
                bssid.bytes[2], bssid.bytes[3], bssid.bytes[4],
                bssid.bytes[5]);
      return true;
    }
    if (ack::restore_station(dev, *_restore)) {
      log->error("{}: station identity arm did not read back; pre-arm port "
                 "state restored and verified",
                 tag);
      _restore.reset();
    } else {
      log->error("{}: station identity arm did not read back AND the "
                 "rollback did not verify; port-0 state is unknown (Clear "
                 "will retry it)",
                 tag);
    }
    return false;
  }

  /* IRadio::ClearStationIdentity: true when nothing was armed (nothing to
   * undo) or the pre-arm state was restored AND read back. */
  bool clear(RtlAdapter &dev, const Logger_t &log, const char *tag) {
    if (!_restore)
      return true;
    if (!ack::restore_station(dev, *_restore)) {
      log->error("{}: station identity clear did not verify; the port may "
                 "still answer for the station address",
                 tag);
      return false;
    }
    _restore.reset();
    log->info("{}: station identity cleared (pre-arm MACID/BSSID/net_type "
              "restored)",
              tag);
    return true;
  }

private:
  std::optional<ack::StationRestore> _restore;
};

} /* namespace devourer */
