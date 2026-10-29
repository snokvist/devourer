// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink link control (docs/DESIGN.md: Ladder, Sensors, LinkController
// states, DaMachine). Pure and header-only: time is an argument (uint64_t
// monotonic ms), no clocks, no threads, no I/O, no devourer.
//
// Two rungs are published at any time:
//   rung_phy()    what the air carries NOW (bw / MCS / frame size);
//   rung_budget() what the encoder bitrate + FEC k/n follow.
// A demote lowers the budget first and the PHY `bitrate_lead_ms` later (the
// encoder has to be below the new capacity before the MCS steps down); a
// promote raises the PHY first and the budget `bitrate_lead_ms` later. Both
// converge on one committed rung; a pending change is applied by tick() (or
// by any other input, which all advance the clock the same way).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include "p2p/report.h"

namespace p2p {

// One ladder rung (DESIGN.md "Ladder"). LDPC + LGI always; 5 GHz only.
struct Rung {
  uint8_t bw_mhz;          // 20 or 40 (per-frame radiotap on the drone)
  uint8_t mcs;             // HT single-stream MCS
  uint32_t phy_kbps;       // nominal PHY rate at this bw/MCS (LGI)
  uint8_t fec_k;           // source symbols per video block
  uint8_t fec_n;           // total symbols per video block
  uint16_t duty_permille;  // airtime cap the video budget is derived from
  int8_t rssi_floor_dbm;   // minimum rssi_best for this rung to be viable
  uint16_t frame_bytes;    // 802.11 body per frame (P2pHeader + payload)
};

inline constexpr std::array<Rung, 7> kLadder = {{
    Rung{20, 0, 6500, 8, 12, 600, -92, 1400},    // R0 FLOOR
    Rung{20, 1, 13000, 8, 12, 600, -89, 1400},   // R1
    Rung{20, 2, 19500, 8, 12, 600, -87, 1400},   // R2
    Rung{20, 3, 26000, 8, 11, 600, -84, 1400},   // R3
    Rung{40, 1, 26000, 8, 11, 550, -86, 1400},   // R4
    Rung{40, 3, 52000, 8, 10, 500, -81, 1400},   // R5
    Rung{40, 5, 104000, 8, 10, 450, -74, 1400},  // R6
}};
inline constexpr uint8_t kFloorRung = 0;
inline constexpr uint8_t kTopRung = 6;
static_assert(kLadder.size() == kTopRung + 1);

// Video budget(r) = PHY x duty x k/n x 0.97 - control reserve, floored at 0.
inline uint32_t video_budget_kbps(const Rung& r, uint32_t control_reserve_kbps) {
  uint64_t v = uint64_t(r.phy_kbps) * r.duty_permille * r.fec_k * 97;
  v /= 1000ull * (r.fec_n ? r.fec_n : 1) * 100;
  return v > control_reserve_kbps ? uint32_t(v - control_reserve_kbps) : 0;
}

struct LinkCfg {
  uint32_t report_timeout_ms = 500;      // TRACK while report_age < this
  uint32_t blind_step_ms = 500;          // BLIND cadence (-2 rungs per step)
  uint32_t bitrate_lead_ms = 500;        // budget leads demotes / lags promotes by this
  uint32_t promote_dwell_ms = 5000;      // min time since the last rung change
  uint32_t promote_reports = 10;         // consecutive fresh good reports needed
  uint16_t promote_loss_permille = 20;   // "good" report: loss below this
  int8_t promote_rssi_margin_db = 6;     // rssi_best >= floor(r+1) + margin
  // sample_rung() opens this long before the promote dwell ends, so the
  // reports that decide the promote already carry r+1 PER evidence.
  uint32_t promote_sample_lead_ms = 1000;
  uint16_t acute_loss_permille = 200;    // acute demote to floor at/above this ...
  uint32_t acute_min_uniq = 32;          // ... when the window carried >= this many symbols
  uint16_t persistent_loss_permille = 45;// leaky score +1 per report at/above this, -1 below
  int persistent_score_trip = 5;         // score at which one rung is demoted
  uint32_t rung_lock_ms = 30000;         // the vacated rung stays locked this long
  uint32_t q_air_demote_ms = 60;         // local: video queue airtime above this -> r-1
  uint8_t hold_ack_min = 6;              // BLIND -> HOLD when ack8 >= this (>= 4 known)
  uint8_t descend_ack_max = 2;           // HOLD -> BLIND when ack8 <= this (>= 4 known)
  uint32_t probe_silence_ms = 1000;      // ... or when no probe result for this long
  uint32_t tune20_after_ms = 3000;       // wants_tune_20mhz after this long at rung <= 3
  uint8_t start_rung = 2;
  // Not in DESIGN.md: a local (q_air / tx_timeout) demote raises the airtime
  // of everything still queued, so the same sample would demote again on the
  // very next call and cascade to the floor in a few ticks. One local demote
  // per this many ms gives the encoder re-target time to drain the queue.
  uint32_t local_demote_gap_ms = 500;
};

class LinkController {
 public:
  enum class State : uint8_t { Track = 0, Blind = 1, Hold = 2, Floor = 3 };

  explicit LinkController(LinkCfg cfg = {}, uint64_t now_ms = 0)
      : cfg_(cfg),
        rung_(std::min<uint8_t>(cfg.start_rung, kTopRung)),
        phy_(rung_),
        budget_(rung_),
        construct_ms_(now_ms),
        last_change_ms_(now_ms) {
    locked_.fill(0);
    ack_.fill(-1);
    if (phy_ <= 3) le3_since_ = now_ms;
  }

  // ---- inputs -------------------------------------------------------------

  // The ground restarted (its session nonce changed): its epoch and uniq
  // counters began again, so the next report is accepted as the first one
  // instead of being refused as stale until the new epoch overtakes the old.
  // Changes nothing else: state, rungs and report age move only on the report.
  void on_peer_restart() { has_report_ = false; }

  // A LinkReport counts only when its epoch is new: strictly greater than the
  // last one modulo 2^32 (so a counter wrap is not a 4-billion-report gap).
  // Any fresh report leaves BLIND/HOLD/FLOOR for TRACK without snapping the
  // rungs back; an in-flight (already budget-committed) PHY step completes.
  void on_report(const LinkReport& r, uint64_t now_ms) {
    advance(now_ms);
    if (has_report_ && int32_t(r.epoch - last_epoch_) <= 0) return;
    // uniq is cumulative; the first report has no previous one to difference
    // against and carries no acute evidence, and neither does one whose uniq
    // went BACKWARDS (a restarted ground whose epoch overtook the old one
    // before on_peer_restart could be called) — never an unsigned underflow.
    const uint32_t uniq_delta = (has_report_ && r.uniq >= last_uniq_) ? r.uniq - last_uniq_ : 0;
    has_report_ = true;
    last_epoch_ = r.epoch;
    last_uniq_ = r.uniq;
    last_report_ms_ = now_ms;
    state_ = State::Track;

    // Acute: the window saw real traffic and lost a fifth of it.
    if (r.loss_permille >= cfg_.acute_loss_permille && uniq_delta >= cfg_.acute_min_uniq) {
      score_ = 0;
      good_streak_ = 0;
      if (rung_ != kFloorRung || phy_ != kFloorRung || budget_ != kFloorRung)
        commit(kFloorRung, /*budget_leads=*/true, now_ms);
      return;
    }

    // Persistent: leaky score, clamped to [0, trip].
    if (r.loss_permille >= cfg_.persistent_loss_permille) {
      score_ = std::min(score_ + 1, cfg_.persistent_score_trip);
      good_streak_ = 0;
    } else {
      score_ = std::max(score_ - 1, 0);
      good_streak_ = (r.loss_permille < cfg_.promote_loss_permille) ? good_streak_ + 1 : 0;
    }
    if (cfg_.persistent_score_trip > 0 && score_ >= cfg_.persistent_score_trip) {
      score_ = 0;
      if (rung_ > kFloorRung) {
        const uint8_t vacated = rung_;
        commit(uint8_t(rung_ - 1), /*budget_leads=*/true, now_ms);
        locked_[vacated] = now_ms + cfg_.rung_lock_ms;
      }
      return;
    }

    // Promote: never skip a rung, never into a locked rung, only once the
    // previous change has fully settled and the dwell has elapsed.
    if (good_streak_ >= cfg_.promote_reports && rung_ < kTopRung && converged() &&
        now_ms - last_change_ms_ >= cfg_.promote_dwell_ms) {
      const uint8_t next = uint8_t(rung_ + 1);
      const bool rssi_ok =
          int(r.rssi_best) >= int(kLadder[next].rssi_floor_dbm) + int(cfg_.promote_rssi_margin_db);
      if (rssi_ok && probe_evidence_ok(r, rung_, next) && locked_[next] <= now_ms)
        commit(next, /*budget_leads=*/false, now_ms);
    }
  }

  // The rung the drone should air video REPAIR symbols at so the ground's
  // reports carry r+1 PER for the next promote decision; nullopt = air repair
  // at the current rung. Open in TRACK on a settled, unlocked, strictly faster
  // next rung from promote_sample_lead_ms before the dwell ends. A lost repair
  // costs nothing while the block's sources arrive (DESIGN.md "Promotion
  // evidence").
  std::optional<uint8_t> sample_rung(uint64_t now_ms) const {
    if (state_ != State::Track || state_age_ms(now_ms) >= cfg_.report_timeout_ms) return std::nullopt;
    if (rung_ >= kTopRung || !converged()) return std::nullopt;
    const uint8_t next = uint8_t(rung_ + 1);
    if (kLadder[next].phy_kbps <= kLadder[rung_].phy_kbps || locked_[next] > now_ms) return std::nullopt;
    const uint32_t lead = std::min(cfg_.promote_sample_lead_ms, cfg_.promote_dwell_ms);
    if (now_ms < last_change_ms_ || now_ms - last_change_ms_ + lead < cfg_.promote_dwell_ms) return std::nullopt;
    return next;
  }

  // tx.report of a PROBE frame: ok = delivered (state 0), !ok = retry-pinned.
  // A probe whose report never arrived is UNKNOWN and is simply not fed.
  void on_probe_result(bool ok, uint64_t now_ms) {
    ack_[ack_head_] = ok ? 1 : 0;
    ack_head_ = uint8_t((ack_head_ + 1) % ack_.size());
    has_probe_ = true;
    last_probe_ms_ = now_ms;
    advance(now_ms);  // re-evaluates HOLD with the new result before any step
  }

  // Local sensors: video queue depth in airtime at the current rung, and the
  // chip's TX FIFO timeout flag. Either demotes one rung immediately (both
  // PHY and budget), in every state, at most once per local_demote_gap_ms.
  void on_local(uint32_t q_air_ms, bool tx_timeout, uint64_t now_ms) {
    advance(now_ms);
    if (!(q_air_ms > cfg_.q_air_demote_ms || tx_timeout)) return;
    if (has_local_demote_ && now_ms - last_local_demote_ms_ < cfg_.local_demote_gap_ms) return;
    if (rung_ == kFloorRung && phy_ == kFloorRung && budget_ == kFloorRung) return;
    has_local_demote_ = true;
    last_local_demote_ms_ = now_ms;
    commit_both(uint8_t(rung_ > kFloorRung ? rung_ - 1 : kFloorRung), now_ms);
    settle_floor();
  }

  // Call at least every 100 ms: applies due PHY/budget steps, detects the
  // report timeout, runs the BLIND cadence and the HOLD evaluation.
  void tick(uint64_t now_ms) { advance(now_ms); }

  // ---- outputs ------------------------------------------------------------

  uint8_t rung_phy() const { return phy_; }
  uint8_t rung_budget() const { return budget_; }
  const Rung& phy() const { return kLadder[phy_]; }
  const Rung& budget() const { return kLadder[budget_]; }
  State state() const { return state_; }
  const char* state_name() const {
    switch (state_) {
      case State::Track: return "TRACK";
      case State::Blind: return "BLIND";
      case State::Hold: return "HOLD";
      case State::Floor: return "FLOOR";
    }
    return "?";
  }

  // ok count among the last up-to-8 reported probe results.
  uint8_t ack8() const {
    uint8_t n = 0;
    for (int8_t s : ack_) n += (s == 1);
    return n;
  }
  // how many of the 8 slots hold a result yet (0..8).
  uint8_t ack8_known() const {
    uint8_t n = 0;
    for (int8_t s : ack_) n += (s >= 0);
    return n;
  }

  uint32_t report_age_ms(uint64_t now_ms) const {
    if (!has_report_) return UINT32_MAX;
    if (now_ms <= last_report_ms_) return 0;
    const uint64_t age = now_ms - last_report_ms_;
    return age > UINT32_MAX ? UINT32_MAX : uint32_t(age);
  }

  // The drone retunes its own radio to 20 MHz after tune20_after_ms spent
  // continuously at rung_phy <= 3 (drops the wide-RX IGI floor); false the
  // moment rung_phy >= 4.
  bool wants_tune_20mhz(uint64_t now_ms) const {
    return le3_since_ != kNever && now_ms >= le3_since_ &&
           now_ms - le3_since_ >= cfg_.tune20_after_ms;
  }

  struct Debug {
    int persistent_score;
    uint32_t consecutive_good_reports;
    uint64_t rung_locked_until[7];
    uint64_t last_change_ms;
  };
  Debug debug() const {
    Debug d{};
    d.persistent_score = score_;
    d.consecutive_good_reports = good_streak_;
    for (size_t i = 0; i < locked_.size(); i++) d.rung_locked_until[i] = locked_[i];
    d.last_change_ms = last_change_ms_;
    return d;
  }

 private:
  // Committed-rung bookkeeping ------------------------------------------------
  void set_phy(uint8_t r, uint64_t now_ms) {
    phy_ = r;
    if (r <= 3) {
      if (le3_since_ == kNever) le3_since_ = now_ms;
    } else {
      le3_since_ = kNever;
    }
  }

  // One rung action. budget_leads: budget now, PHY after the lead (demote);
  // otherwise PHY now, budget after the lead (promote). Replaces any pending
  // step: the newest decision owns both trajectories.
  void commit(uint8_t r, bool budget_leads, uint64_t now_ms) {
    rung_ = r;
    pend_phy_ = kNoPend;
    pend_budget_ = kNoPend;
    if (budget_leads) {
      budget_ = r;
      if (phy_ != r) {
        pend_phy_ = r;
        pend_phy_due_ = now_ms + cfg_.bitrate_lead_ms;
      }
    } else {
      set_phy(r, now_ms);
      if (budget_ != r) {
        pend_budget_ = r;
        pend_budget_due_ = now_ms + cfg_.bitrate_lead_ms;
      }
    }
    last_change_ms_ = now_ms;
    good_streak_ = 0;
    score_ = 0;
    apply_pending(now_ms);  // a zero lead applies at once
  }

  void commit_both(uint8_t r, uint64_t now_ms) {
    rung_ = r;
    pend_phy_ = kNoPend;
    pend_budget_ = kNoPend;
    budget_ = r;
    set_phy(r, now_ms);
    last_change_ms_ = now_ms;
    good_streak_ = 0;
    score_ = 0;
  }

  bool converged() const { return pend_phy_ == kNoPend && pend_budget_ == kNoPend && phy_ == rung_ && budget_ == rung_; }

  void apply_pending(uint64_t now_ms) {
    if (pend_phy_ != kNoPend && now_ms >= pend_phy_due_) {
      set_phy(pend_phy_, now_ms);
      pend_phy_ = kNoPend;
    }
    if (pend_budget_ != kNoPend && now_ms >= pend_budget_due_) {
      budget_ = pend_budget_;
      pend_budget_ = kNoPend;
    }
  }

  // Probe-rung PER <= 2 x current-rung PER, from this report's counters.
  // No frames at the probe rung is no evidence, which vetoes. A step that is
  // not faster on air (R3 -> R4: same 26 Mb/s, 20 -> 40 MHz) needs none — it
  // promotes on loss + rssi + dwell alone (and its r+1 frames could not air
  // on a 20 MHz tune anyway).
  static bool probe_evidence_ok(const LinkReport& r, uint8_t cur, uint8_t next) {
    if (kLadder[next].phy_kbps <= kLadder[cur].phy_kbps) return true;
    const uint64_t p_tot = uint64_t(r.probe_rung_ok) + r.probe_rung_lost;
    const uint64_t c_tot = uint64_t(r.cur_rung_ok) + r.cur_rung_lost;
    if (p_tot == 0) return false;
    if (c_tot == 0) return r.probe_rung_lost == 0;  // current PER taken as 0
    // lost_p / tot_p <= 2 * lost_c / tot_c
    return uint64_t(r.probe_rung_lost) * c_tot <= 2 * uint64_t(r.cur_rung_lost) * p_tot;
  }

  // State machine ----------------------------------------------------------------
  // Age used for the TRACK/BLIND decision: construction counts as the last
  // "report" so a node that never hears the ground holds start_rung for one
  // timeout and then descends, instead of being born in BLIND.
  uint64_t state_age_ms(uint64_t now_ms) const {
    const uint64_t ref = has_report_ ? last_report_ms_ : construct_ms_;
    return now_ms > ref ? now_ms - ref : 0;
  }
  bool probe_fresh(uint64_t now_ms) const {
    return has_probe_ && now_ms >= last_probe_ms_ && now_ms - last_probe_ms_ < cfg_.probe_silence_ms;
  }

  void advance(uint64_t now_ms) {
    apply_pending(now_ms);
    if (state_ == State::Track && state_age_ms(now_ms) >= cfg_.report_timeout_ms) {
      state_ = State::Blind;
      blind_next_ms_ = now_ms;  // first step (budget := r-2) is due at once
    }
    if (state_ == State::Track) return;

    // HOLD evaluation (hysteresis between hold_ack_min and descend_ack_max).
    const uint8_t known = ack8_known(), ok = ack8();
    const bool fresh = probe_fresh(now_ms);
    const bool hold_ok = fresh && known >= 4 && ok >= cfg_.hold_ack_min;
    const bool hold_fail = !fresh || (known >= 4 && ok <= cfg_.descend_ack_max);
    if (state_ == State::Hold) {
      if (hold_fail) {
        state_ = State::Blind;
        blind_next_ms_ = now_ms;  // keep descending right away
      }
    } else if (state_ == State::Blind && hold_ok) {
      state_ = State::Hold;  // ground hears us; its reports don't reach us
    }

    // BLIND cadence: budget -= 2 now, PHY follows after the lead; one step
    // per call even when a tick is late (the next one re-anchors).
    if (state_ == State::Blind && now_ms >= blind_next_ms_) {
      if (rung_ > kFloorRung) {
        const uint8_t next = uint8_t(rung_ >= 2 ? rung_ - 2 : kFloorRung);
        commit(next, /*budget_leads=*/true, now_ms);
      }
      blind_next_ms_ = now_ms + cfg_.blind_step_ms;
    }
    settle_floor();
  }

  // FLOOR = a report-less state that has fully reached rung 0.
  void settle_floor() {
    if (state_ != State::Track && rung_ == kFloorRung && phy_ == kFloorRung && budget_ == kFloorRung &&
        pend_phy_ == kNoPend && pend_budget_ == kNoPend)
      state_ = State::Floor;
  }

  LinkCfg cfg_;
  State state_ = State::Track;
  uint8_t rung_;     // committed rung both outputs converge to
  uint8_t phy_;
  uint8_t budget_;
  // Pending rung steps. Plain sentinels rather than std::optional: GCC's
  // -O3 inliner reports a spurious -Wmaybe-uninitialized on the optional's
  // engaged flag when the controller is a local in the caller.
  static constexpr uint8_t kNoPend = 0xff;
  uint8_t pend_phy_ = kNoPend, pend_budget_ = kNoPend;
  uint64_t pend_phy_due_ = 0, pend_budget_due_ = 0;

  bool has_report_ = false;
  uint32_t last_epoch_ = 0, last_uniq_ = 0;
  uint64_t last_report_ms_ = 0;
  uint64_t construct_ms_;

  int score_ = 0;
  uint32_t good_streak_ = 0;
  std::array<uint64_t, 7> locked_;
  uint64_t last_change_ms_;
  bool has_local_demote_ = false;
  uint64_t last_local_demote_ms_ = 0;

  std::array<int8_t, 8> ack_;  // -1 empty, 0 pinned, 1 ok
  uint8_t ack_head_ = 0;
  bool has_probe_ = false;
  uint64_t last_probe_ms_ = 0;

  uint64_t blind_next_ms_ = 0;
  static constexpr uint64_t kNever = UINT64_MAX;  // sentinel, see kNoPend
  uint64_t le3_since_ = kNever;
};

// DA policy for the IP and control classes (video is ALWAYS group DA).
//   BCAST -> UCAST: ack8 >= to_ucast_ack_min continuously for to_ucast_hold_ms
//                   AND the LinkController is in TRACK.
//   UCAST -> BCAST: pinned_trip consecutive retry-pinned probes, or
//                   ack8 <= to_bcast_ack_max, or state != TRACK.
// Each direction honours min_dwell_ms (the very first flip has no dwell to
// honour). IP-bulk is unicast only at ack8 == 8. The probe is the sensor, so
// the machine is independent of the IP traffic rate: with no IP at all it
// still flips to BCAST two probes after the peer vanishes.
// Namespace-scope (not nested) so its default member initializers can serve
// as the constructor's default argument on GCC; DaMachine::Cfg aliases it.
struct DaCfg {
  uint8_t to_ucast_ack_min = 7;
  uint32_t to_ucast_hold_ms = 1000;
  uint8_t to_bcast_ack_max = 5;
  int pinned_trip = 2;
  uint32_t min_dwell_ms = 1000;
};

class DaMachine {
 public:
  enum class Mode : uint8_t { Bcast = 0, Ucast = 1 };
  using Cfg = DaCfg;

  explicit DaMachine(Cfg cfg = {}) : cfg_(cfg) {}

  void on_probe_result(bool ok, uint64_t now_ms) {
    pinned_ = ok ? 0 : pinned_ + 1;
    evaluate(now_ms);
  }
  void update(uint8_t ack8, uint8_t ack8_known, LinkController::State st, uint64_t now_ms) {
    ack8_ = ack8;
    known_ = ack8_known;
    st_ = st;
    evaluate(now_ms);
  }

  Mode mode() const { return mode_; }

  // ip_class: 0 control, 1 interactive, 2 bulk (P2pHeader::stream for IpData).
  bool allow_unicast(uint8_t ip_class, uint8_t ack8) const {
    if (mode_ != Mode::Ucast) return false;
    return ip_class != 2 || ack8 == 8;
  }

 private:
  void evaluate(uint64_t now_ms) {
    const bool dwell_ok = !flipped_ || (now_ms >= mode_since_ms_ && now_ms - mode_since_ms_ >= cfg_.min_dwell_ms);
    if (mode_ == Mode::Bcast) {
      const bool cond = st_ == LinkController::State::Track && ack8_ >= cfg_.to_ucast_ack_min &&
                        pinned_ < cfg_.pinned_trip;
      if (!cond) {
        cond_since_ = kNever;
        return;
      }
      if (cond_since_ == kNever) cond_since_ = now_ms;
      if (now_ms - cond_since_ >= cfg_.to_ucast_hold_ms && dwell_ok) flip(Mode::Ucast, now_ms);
    } else {
      const bool cond = st_ != LinkController::State::Track || pinned_ >= cfg_.pinned_trip ||
                        ack8_ <= cfg_.to_bcast_ack_max;
      if (cond && dwell_ok) flip(Mode::Bcast, now_ms);
    }
  }
  void flip(Mode m, uint64_t now_ms) {
    mode_ = m;
    mode_since_ms_ = now_ms;
    flipped_ = true;
    cond_since_ = kNever;
  }

  Cfg cfg_;
  Mode mode_ = Mode::Bcast;
  bool flipped_ = false;
  uint64_t mode_since_ms_ = 0;
  static constexpr uint64_t kNever = UINT64_MAX;
  uint64_t cond_since_ = kNever;
  int pinned_ = 0;
  uint8_t ack8_ = 0, known_ = 0;
  LinkController::State st_ = LinkController::State::Blind;
};

}  // namespace p2p
