/* BssTable — what a scan found, and which of it is worth joining.
 *
 * A station hears the same BSS dozens of times a second and several BSSes at
 * once. This collapses that stream into one record per BSSID, keeps the
 * freshest view of each, and answers the only question the association state
 * machine actually asks: given an SSID, which BSS should I join?
 *
 * Fixed capacity, no allocation per beacon, and it takes frames straight off
 * the air — every field comes from `parse_beacon`, which bounds-checks
 * everything and resets its output so a frame that omits an element cannot
 * leave the previous BSS's value in place.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not scan: it has no notion of
 * channels, dwell times or probe requests, because those need a radio and this
 * file is meant to be testable without one. A scanner drives the radio and
 * feeds the frames here.
 */
#ifndef DEVOURER_STA_BSS_TABLE_H
#define DEVOURER_STA_BSS_TABLE_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "sta/Dot11.h"

namespace devourer {
namespace sta {

struct BssEntry {
  BssInfo info;
  /* The most recent RSSI in dBm. Signed and initialised to the floor rather
   * than to zero: zero dBm is an enormous signal, and an entry that has never
   * been given an RSSI must not win a comparison against one that has. */
  int8_t rssi = -128;
  uint32_t last_seen_ms = 0;
  uint32_t frames = 0;      /* beacons and probe responses that fed this entry */
};

class BssTable {
 public:
  /* Sixteen. A crowded band shows more than that, but this exists to find one
   * named network, not to render a survey, and the eviction rule below keeps
   * the useful entries. */
  static constexpr int kMaxBss = 16;

  static constexpr int capacity() { return kMaxBss; }
  int count() const { return count_; }

  void clear() {
    for (int i = 0; i < kMaxBss; i++) used_[i] = false;
    count_ = 0;
  }

  const BssEntry* at(int i) const {
    if (i < 0 || i >= kMaxBss || !used_[i]) return nullptr;
    return &slots_[i];
  }

  const BssEntry* find(const uint8_t bssid[6]) const {
    const int i = index_of(bssid);
    return i < 0 ? nullptr : &slots_[i];
  }

  /* Fold one beacon or probe response into the table.
   *
   * Returns the entry, or null when the frame is not one of those two
   * subtypes or does not parse. THE SUBTYPE CHECK IS NOT COSMETIC: every
   * management frame has the same 24-byte header, so handing a deauth or an
   * association response to parse_beacon reads its fixed fields as a
   * timestamp and a capability and invents a BSS out of them.
   */
  const BssEntry* observe(const uint8_t* frame, size_t len, int8_t rssi,
                          uint32_t now_ms) {
    BssInfo info;

    if (!frame || len < 24) return nullptr;
    if (frame[0] != kFcBeacon && frame[0] != kFcProbeResp) return nullptr;
    if (!parse_beacon(frame, len, &info)) return nullptr;

    int i = index_of(info.bssid);
    if (i < 0) i = allocate(now_ms);
    if (i < 0) return nullptr;

    const uint32_t seen = used_[i] ? slots_[i].frames : 0;
    const bool fresh = !used_[i];

    if (fresh) {
      used_[i] = true;
      count_++;
      slots_[i] = BssEntry{};
    }
    slots_[i].info = info;
    slots_[i].rssi = rssi;
    slots_[i].last_seen_ms = now_ms;
    slots_[i].frames = seen + 1;
    return &slots_[i];
  }

  /* Drop entries not heard from in `max_age_ms`.
   *
   * A station that keeps a BSS forever will happily try to associate with one
   * that went off the air ten minutes ago, and then report "no response" as
   * though the AP were broken. Returns how many were dropped.
   */
  int expire(uint32_t now_ms, uint32_t max_age_ms) {
    int dropped = 0;

    for (int i = 0; i < kMaxBss; i++) {
      if (!used_[i]) continue;
      /* Unsigned subtraction, so a now_ms that has wrapped past an entry's
       * timestamp yields a huge age and expires it, rather than a negative
       * one that never does. */
      if ((uint32_t)(now_ms - slots_[i].last_seen_ms) >= max_age_ms) {
        used_[i] = false;
        count_--;
        dropped++;
      }
    }
    return dropped;
  }

  /* The BSS to join for this SSID: the strongest one this station can
   * actually speak to.
   *
   * "Can speak to" is `rsn_ccmp_psk`, which parse_beacon computes as CCMP
   * among the pairwise suites, PSK among the AKMs, and management-frame
   * protection NOT required — 802.11w is unimplemented here, and a BSS that
   * requires it will refuse the association after a successful authentication,
   * which is a confusing place to fail. Skipping it in selection turns that
   * into "no candidate".
   *
   * WPA2-PSK only, on purpose: it is the one suite this project speaks, and a
   * `require_rsn` argument with one caller would be configuration for a value
   * that does not vary. An open-network selector can be a second function when
   * something needs one.
   *
   * Ties break on the most recently heard, so a stale entry never beats a live
   * one at equal signal.
   */
  const BssEntry* select(const std::string& ssid) const {
    const BssEntry* best = nullptr;

    for (int i = 0; i < kMaxBss; i++) {
      if (!used_[i]) continue;
      const BssEntry& e = slots_[i];
      if (e.info.ssid != ssid) continue;
      if (!e.info.rsn_ccmp_psk) continue;
      if (!best || e.rssi > best->rssi ||
          (e.rssi == best->rssi && e.last_seen_ms > best->last_seen_ms))
        best = &e;
    }
    return best;
  }

 private:
  int index_of(const uint8_t bssid[6]) const {
    for (int i = 0; i < kMaxBss; i++)
      if (used_[i] && std::memcmp(slots_[i].info.bssid, bssid, 6) == 0)
        return i;
    return -1;
  }

  /* A free slot, or the one worth losing.
   *
   * REFUSING WHEN FULL IS THE WRONG ANSWER. Sixteen neighbours heard before
   * the target network would make the table permanently useless, and a scan
   * in a block of flats hits that in a second. The victim is the entry heard
   * from longest ago, with the weakest signal breaking a tie — the two things
   * that make a BSS least likely to be the one being looked for.
   */
  int allocate(uint32_t now_ms) {
    for (int i = 0; i < kMaxBss; i++)
      if (!used_[i]) return i;

    int victim = 0;
    for (int i = 1; i < kMaxBss; i++) {
      const uint32_t age_v = (uint32_t)(now_ms - slots_[victim].last_seen_ms);
      const uint32_t age_i = (uint32_t)(now_ms - slots_[i].last_seen_ms);

      if (age_i > age_v ||
          (age_i == age_v && slots_[i].rssi < slots_[victim].rssi))
        victim = i;
    }
    used_[victim] = false;
    count_--;
    return victim;
  }

  BssEntry slots_[kMaxBss];
  bool used_[kMaxBss] = {false};
  int count_ = 0;
};

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_BSS_TABLE_H */
