/* The associated-station table — Phase 2b.2.
 *
 * Every AP-side harness in this tree keeps exactly one station's worth of
 * state in file-scope variables: `g_sta`, `g_ptk`, `g_state`, `g_replay`,
 * `g_txpn` and one `CcmpReplay` in tests/ap_wpa2.cpp, with the same shape
 * again in tests/ap_responder.cpp and tests/ul_trigger_ap.cpp. That is the
 * only thing stopping the AP serving more than one client:
 * docs/mt7612u-ap-mode.md records that the WCID table is 256 entries and SKEY
 * is per-BSS in hardware, and CCMP here is software, so there are no key slots
 * to exhaust. The limit is this state, and this header is where it stops being
 * a limit.
 *
 * Pure and backend-agnostic, like the rest of src/sta/: no IRadio, no OS
 * calls, no allocation, nothing to initialise beyond zeroing. Header-only to
 * match Ccmp.h and Dot11.h.
 *
 * TWO INVARIANTS THAT ARE NOT STYLE:
 *
 *  1. ONE TX PN PER STATION, NEVER PER TID. The standard keeps PN counters per
 *     TID, and this table deliberately does not, because per-TID counters over
 *     a TID-blind CCM nonce is keystream reuse. The nonce carries the TID as
 *     of 2026-09-20 (Phase 2b.1), so per-TID counters are no longer unsafe —
 *     but they are still not free, and nothing needs them yet. If you add
 *     them, read the nonce's comment in Ccmp.h first and make sure you
 *     understand why the order of those two changes mattered.
 *
 *  2. THE GTK IS NOT IN HERE. It is per-BSS, and putting it beside per-station
 *     material is exactly how tests/ap_wpa2.cpp came to regenerate it on every
 *     four-way handshake, so a second station's association silently revoked
 *     the first station's group key. `g_gtk` sits on the same declaration line
 *     as `g_anonce`/`g_snonce`/`g_ptk` there. Keep it out.
 */
#ifndef DEVOURER_STA_STATION_TABLE_H
#define DEVOURER_STA_STATION_TABLE_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sta/Ccmp.h"
#include "sta/Dot11.h"

namespace devourer {
namespace sta {

/* Where a station is in its four-way handshake.
 *
 * `WaitMsg4` already holds a usable PTK: a station that received msg3
 * installs its keys and may send protected data BEFORE its msg4 reaches us.
 * The data path must therefore accept from WaitMsg4 onward, not only at Done.
 * That is not a nicety — it is a race the AP harness hit on real hardware. */
enum class HsState : uint8_t {
  Idle = 0,
  WaitMsg2 = 1,
  WaitMsg4 = 2,
  Done = 3,
};

struct Station {
  uint8_t addr[6];
  uint16_t aid;
  HsState state;

  uint8_t anonce[32];
  uint8_t snonce[32];
  uint8_t ptk[48];          /* KCK[0:16] KEK[16:32] TK[32:48] */
  uint8_t eapol_replay[8];  /* the EAPOL-Key replay counter, not CCMP's */

  uint64_t tx_pn;           /* one per station; see invariant 1 */
  CcmpReplay rx_replay;     /* per-TID receive window */

  int hs_tries;             /* four-way retransmissions so far */
  double hs_tx_ms;          /* when the last (re)transmission went out */

  /* True once a PTK exists and protected data may be accepted. */
  bool keyed() const { return state == HsState::WaitMsg4 || state == HsState::Done; }
};

/* Fixed capacity, no allocation.
 *
 * Seven, because AID 1..7 is what the current one-byte TIM bitmap can express
 * (`append_tim` in Dot11.h sets `1u << aid` only for 1..7). Raising this
 * without also implementing the partial virtual bitmap would silently stop
 * advertising buffered traffic for the stations past the seventh. Power save
 * is off by fleet policy so nothing is buffered today, which makes this a
 * latent coupling rather than a live one — but it is a real coupling, and the
 * cap is where it is documented. */
class StationTable {
public:
  static constexpr int kMaxStations = 7;
  static constexpr uint16_t kMinAid = 1;

  StationTable() { clear(); }

  void clear() {
    /* Value-initialise rather than memset: Station holds a CcmpReplay, which
     * has default member initialisers, so it is not trivially copyable and
     * memset on it is -Wclass-memaccess. `Station{}` is also correct by
     * construction instead of correct because the all-zero pattern happens to
     * be a reset replay window. */
    for (int i = 0; i < kMaxStations; i++) {
      slots_[i] = Station{};
      used_[i] = false;
    }
  }

  int count() const {
    int n = 0;
    for (int i = 0; i < kMaxStations; i++)
      if (used_[i]) n++;
    return n;
  }

  /* nullptr when this address is not associated. A group address is never
   * associated and is refused rather than matched against a zeroed slot. */
  Station* find(const uint8_t addr[6]) {
    if (!addr || (addr[0] & 0x01)) return nullptr;
    for (int i = 0; i < kMaxStations; i++)
      if (used_[i] && std::memcmp(slots_[i].addr, addr, 6) == 0)
        return &slots_[i];
    return nullptr;
  }
  const Station* find(const uint8_t addr[6]) const {
    return const_cast<StationTable*>(this)->find(addr);
  }

  /* Associate. Returns the existing record if this address is already here —
   * a re-association must not consume a second AID or orphan the first
   * record, and a station that loses its way through the handshake will
   * re-send an association request. Returns nullptr when full, or when the
   * address is malformed.
   *
   * A fresh record is zeroed and Idle, and its AID is the LOWEST free one, so
   * a table that churns does not walk its AIDs upward out of the TIM's range. */
  Station* add(const uint8_t addr[6]) {
    if (!addr || (addr[0] & 0x01)) return nullptr;
    if (Station* existing = find(addr)) return existing;

    int slot = -1;
    for (int i = 0; i < kMaxStations; i++) {
      if (!used_[i]) { slot = i; break; }
    }
    if (slot < 0) return nullptr;

    Station& s = slots_[slot];
    /* Deliberately redundant with remove()'s wipe: every unused slot is
     * already clean, since remove() and clear() are the only routes to unused
     * and both reset. Kept anyway, because this is key material and because it
     * makes add() correct on its own terms rather than on remove()'s. The cost
     * is that a mutation deleting this line survives the selftest - see the
     * note on test_removal_wipes_key_material. */
    s = Station{};
    std::memcpy(s.addr, addr, 6);
    s.aid = lowest_free_aid();
    s.state = HsState::Idle;
    /* A CCMP transmitter starts at 1: PN 0 is what CcmpReplay refuses. */
    s.tx_pn = 1;
    used_[slot] = true;
    return &s;
  }

  /* Deauthenticate. False when the address was not here — a duplicate deauth
   * is not an error but it is not a removal either, and a caller that counts
   * removals should see one. */
  bool remove(const uint8_t addr[6]) {
    if (!addr) return false;
    for (int i = 0; i < kMaxStations; i++) {
      if (used_[i] && std::memcmp(slots_[i].addr, addr, 6) == 0) {
        slots_[i] = Station{};   /* key material must not linger in a free slot */
        used_[i] = false;
        return true;
      }
    }
    return false;
  }

  /* Iteration for the callers that must touch every station — the four-way
   * retransmission tick, and a group-addressed frame that has to be encrypted
   * once per station until there is a GTK transmit path. */
  Station* at(int i) {
    if (i < 0 || i >= kMaxStations || !used_[i]) return nullptr;
    return &slots_[i];
  }
  static constexpr int capacity() { return kMaxStations; }

private:
  uint16_t lowest_free_aid() const {
    for (uint16_t aid = kMinAid; aid < kMinAid + kMaxStations; aid++) {
      bool taken = false;
      for (int i = 0; i < kMaxStations; i++)
        if (used_[i] && slots_[i].aid == aid) { taken = true; break; }
      if (!taken) return aid;
    }
    return 0; /* unreachable while add() checks capacity first */
  }

  Station slots_[kMaxStations];
  bool used_[kMaxStations];
};

/* ------------------------------------------------------------------ forward
 *
 * WHERE DOES THIS FRAME GO? One pure function, so the answer can be tested
 * without a radio.
 *
 * This logic lived inline in tests/ap_wpa2.cpp, which is in no test target at
 * all - hand-built, never run by ctest - so the relay decision that four Phase
 * 2b gates depend on rested entirely on narrated bench runs. A boundary review
 * named that as the cost of putting forwarding logic in the harness rather
 * than in src/sta/. It also has a second consumer coming: a TAP forwarder needs
 * the identical decision, and writing it twice is how the two drift.
 *
 * Pure: it reads the header and the table and returns a verdict. No I/O, no
 * allocation, no crypto, nothing to mock. */
enum class Disposition : uint8_t {
  RefuseFragmented,  /* More Fragments set - see below */
  RefuseAmsdu,       /* A-MSDU Present set - see below */
  Malformed,         /* header shorter than its own frame control claims */
  Local,             /* destined for the AP itself */
  Group,             /* group destination: answer locally AND flood */
  Relay,             /* destined for another associated station */
  OffBss,            /* destined somewhere this BSS cannot reach */
};

struct ForwardDecision {
  Disposition what;
  const uint8_t* da;   /* into `hdr`; null when Malformed */
};

/* WHY TWO REFUSALS RATHER THAN A REASSEMBLER.
 *
 * CCMP is per-MPDU. A fragment decrypts and passes the replay window on its
 * own - and then fragment 0's plaintext is parsed as a whole MSDU and
 * forwarded with More Fragments CLEARED, while fragments 1..n carry no
 * LLC/SNAP header at all. The peer receives corruption while every counter
 * reads success. An A-MSDU frame fails the same way through a different
 * misparse: its first subframe header is read as LLC/SNAP and the forwarded
 * copy drops the bit that said otherwise.
 *
 * Refusing both converts an unbounded silent-corruption surface into two
 * counters. A-MSDU cannot reach this AP while it advertises neither WMM nor
 * HT - but "unreachable today" is not a reason to forward something wrong,
 * and FRAGMENTATION IS REACHABLE TODAY: it is a legacy feature with no
 * relationship to WMM or HT, and any station with a fragmentation threshold
 * set will use it. See the fragment test below. */
inline ForwardDecision decide_forward(const uint8_t* hdr, size_t hdr_len,
                                      const uint8_t bssid[6],
                                      const StationTable& table) {
  const uint8_t fc0 = hdr[0], fc1 = hdr[1];
  const bool four_addr =
      (fc1 & (kFcToDs | kFcFromDs)) == (kFcToDs | kFcFromDs);
  const bool qos = is_qos_data(fc0);
  const size_t need = 24 + (four_addr ? 6 : 0) + (qos ? 2 : 0);

  if (hdr_len < need) return {Disposition::Malformed, nullptr};
  /* BOTH HALVES OF "this is a fragment".
   *
   * More Fragments alone is not enough: it is CLEAR on the LAST fragment of a
   * fragmented MSDU, which then looked like an ordinary whole frame and was
   * forwarded as one. It carries no LLC/SNAP header - only fragment 0 does -
   * so the peer received the tail of somebody else's MSDU with every counter
   * on this AP reading success. That is the exact silent corruption the
   * refusal exists to prevent, arriving through the door the refusal left
   * open, and the comment above claiming it "cannot reach this AP while it
   * advertises neither WMM nor HT" was wrong twice over: fragmentation has
   * nothing to do with either, and any legacy station with a fragmentation
   * threshold set does this.
   *
   * Found by tests/ap_wpa2_selftest.inc on the day it was written; the cell
   * in tests/station_table_selftest.cpp only ever set the bit. */
  if ((fc1 & kFcMoreFrag) || (hdr[22] & 0x0f))
    return {Disposition::RefuseFragmented, nullptr};
  if (qos && (hdr[four_addr ? 30 : 24] & 0x80))
    return {Disposition::RefuseAmsdu, nullptr};

  const uint8_t* da = data_da(hdr, fc1);
  if (da[0] & 0x01) return {Disposition::Group, da};
  if (std::memcmp(da, bssid, 6) == 0) return {Disposition::Local, da};
  if (table.find(da)) return {Disposition::Relay, da};
  return {Disposition::OffBss, da};
}

} // namespace sta
} // namespace devourer

#endif /* DEVOURER_STA_STATION_TABLE_H */
