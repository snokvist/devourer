// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink airtime model and the single-TX-thread strict-priority scheduler
// (docs/DESIGN.md "Budget / scheduler"). Pure and header-only: time is an
// argument, no clocks, threads or I/O, no devourer.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>
#include "p2p/link_ctl.h"
#include "p2p/wire.h"

namespace p2p {

// ---- airtime model ----------------------------------------------------------
//
//   airtime = PLCP + ceil4( (8 * (24 + body + 4) + 22) / PHY ) + IFS
//
// PLCP: HT mixed-mode preamble, one spatial stream — L-STF 8 + L-LTF 8 +
//       L-SIG 4 + HT-SIG 8 + HT-STF 4 + HT-LTF 4 = 36 us (same at 20/40 MHz).
// data: the 802.11 header (24 B) and FCS (4 B) are added to the body the
//       caller passes (P2pHeader + payload, i.e. Rung::frame_bytes); the
//       16-bit SERVICE field and 6 tail bits are counted too (LDPC has no
//       tail bits — kept as a margin); rounded up to whole 4 us OFDM symbols.
// IFS:  ~50 us for SIFS + the mean DIFS/backoff before the next frame — the
//       per-frame share of medium access on an otherwise quiet channel. An
//       ACKed unicast frame would add SIFS + ACK (~44 us at 6 Mb/s) on top;
//       the scheduler charges the same model to every class and accepts that
//       bias (unicast probes/IP are a small slice of the airtime).
// The model is for budgets and queue bounds (tens of frames), not per-frame
// timing: it ignores short-GI, A-MPDU (off here) and retries.
inline constexpr uint32_t kPlcpHtMixedUs = 36;
inline constexpr uint32_t kIfsBackoffUs = 50;
inline constexpr uint32_t kServiceTailBits = 22;

inline uint32_t frame_airtime_us(const Rung& r, size_t body_bytes) {
  const uint64_t bits = 8ull * (uint64_t(body_bytes) + kDot11HdrLen + kFcsLen) + kServiceTailBits;
  const uint64_t kbps = r.phy_kbps ? r.phy_kbps : 1;
  uint64_t data_us = (bits * 1000 + kbps - 1) / kbps;  // bits / (kbit/s) = ms -> x1000 us
  data_us = (data_us + 3) / 4 * 4;                      // whole OFDM symbols
  return uint32_t(kPlcpHtMixedUs + data_us + kIfsBackoffUs);
}

// Payload bytes one frame carries at this rung.
inline size_t frame_payload_capacity(const Rung& r) {
  return r.frame_bytes > kHdrLen ? size_t(r.frame_bytes - kHdrLen) : size_t(1);
}

// Repair symbols for a k-source video block at this rung: the rung's ratio,
// ceil(k x (n_r - k_r) / k_r), floored at kMinRepair so a small frame is not
// one loss from death (a 1..4-symbol block at R5/R6 would otherwise carry a
// single repair; waybeam-link's min_r = 2, same reason). The TX encoder and the
// airtime charge below both use this, so the scheduler charges what airs.
inline constexpr size_t kMinRepair = 2;
inline size_t video_repair_count(const Rung& r, size_t k) {
  const size_t kr = r.fec_k ? r.fec_k : 1;
  const size_t ratio = (k * (r.fec_n > r.fec_k ? r.fec_n - r.fec_k : 0) + kr - 1) / kr;
  return std::max(kMinRepair, ratio);
}

// Airtime of one video item (one encoder frame -> one FEC block, encoded at
// DEQUEUE): ceil(payload / capacity) source symbols plus video_repair_count()
// repairs, each a full frame_bytes frame.
inline uint32_t video_item_airtime_us(const Rung& r, size_t payload_bytes) {
  const size_t cap = frame_payload_capacity(r);
  size_t syms = (payload_bytes + cap - 1) / cap;
  if (syms == 0) syms = 1;
  const uint64_t total = syms + video_repair_count(r, syms);
  return uint32_t(uint64_t(frame_airtime_us(r, r.frame_bytes)) * total);
}

// Airtime of one IP or control item: one frame when it fits (the common
// case — one IP packet per frame), else as many full frames as it spans.
inline uint32_t single_item_airtime_us(const Rung& r, size_t payload_bytes) {
  const size_t cap = frame_payload_capacity(r);
  if (payload_bytes <= cap) return frame_airtime_us(r, kHdrLen + payload_bytes);
  const uint64_t frames = (uint64_t(payload_bytes) + cap - 1) / cap;
  return uint32_t(uint64_t(frame_airtime_us(r, r.frame_bytes)) * frames);
}

// ---- budgets ----------------------------------------------------------------
//
// A share of wall-clock airtime, in the SAME model the scheduler charges, so
// a rung's duty cap and the tx_duty_permille the scheduler reports are one
// quantity: feeding exactly the budget fills duty_permille of the 100 ms
// duty window (C7: PHY x duty, which ignores the 86 us PLCP + IFS every frame
// pays, fed duty 707 permille at R5 against a 500 cap, 814 at R6 against 450).

// Payload kbps that share_permille of airtime carries in full frame_bytes
// frames at this rung: frames/s = share x 1000 / airtime_us, each carrying
// frame_payload_capacity bytes.
inline uint32_t payload_kbps_at_share(const Rung& r, uint32_t share_permille) {
  const uint64_t air = frame_airtime_us(r, r.frame_bytes);
  return uint32_t(uint64_t(share_permille) * frame_payload_capacity(r) * 8 / air);
}

// Video budget(r) = payload at the duty cap x k/n x 0.97 - control reserve,
// floored at 0. The 0.97 is margin for what the model does not see per
// block: the last source symbol's padding and n rounded up (a 1-symbol block
// is 1+1, not 1.25).
inline uint32_t video_budget_kbps(const Rung& r, uint32_t control_reserve_kbps) {
  const uint64_t air = frame_airtime_us(r, r.frame_bytes);
  uint64_t v = uint64_t(r.duty_permille) * frame_payload_capacity(r) * 8 * r.fec_k * 97;
  v /= air * (r.fec_n ? r.fec_n : 1) * 100;
  return v > control_reserve_kbps ? uint32_t(v - control_reserve_kbps) : 0;
}

// ---- scheduler --------------------------------------------------------------

enum class TxClass : uint8_t { Control = 0, IpInteractive = 1, Video = 2, IpBulk = 3 };

struct TxItem {
  TxClass cls;
  uint8_t stream;               // video stream id / IP class
  uint8_t flags;                // p2p::Flags; video: kFlagEnhance / kFlagIdr matter here
  std::vector<uint8_t> payload;
  uint64_t enq_ms;
  uint32_t id;
};

struct SchedCfg {
  uint32_t video_queue_air_ms = 60;         // video queue bound, in airtime at the current rung
  uint16_t interactive_share_permille = 150;// interactive token bucket: share of wall-clock airtime
  uint32_t bucket_window_ms = 100;          // bucket capacity window; also the duty window
  size_t control_max = 64;                  // control queue, items (tail-drop)
  size_t ip_max = 256;                      // per IP class queue, items (tail-drop)
};

// Strict priority: control > interactive (token bucket) > video > bulk.
//   control      never dropped by the video policy; the queue itself tail-drops
//                at control_max (an overfull control queue is a sender bug).
//   interactive  eligible while the bucket holds its airtime; the bucket refills
//                at interactive_share_permille us per ms, capped at
//                share x bucket_window_ms (15 ms of airtime per 100 ms by
//                default), and is charged at the CURRENT rung in on_sent().
//   video        bounded to video_queue_air_ms of airtime at the current rung
//                (re-checked at dequeue, since a demote inflates what is
//                queued). Over the bound it drops, newest first: the newest
//                kFlagEnhance item, else the newest non-IDR item, else the
//                newest IDR — but never the last item (a single oversize frame
//                is sent, not starved).
//   bulk         goes only when nothing above is eligible: control empty,
//                interactive empty or bucket-starved, video empty.
// Rate, FEC and DA bind at dequeue, never at enqueue: the scheduler returns
// whole items and the caller encodes them against the rung in force then.
// Protocol per item: dequeue() -> send -> on_sent() (which charges the bucket
// and the duty window); dequeue() does not charge.
class TxScheduler {
 public:
  explicit TxScheduler(SchedCfg cfg = {}) : cfg_(cfg), tokens_us_(int64_t(capacity_us())) {}

  // false = the passed item was dropped (queue policy); drop counters updated.
  bool enqueue(TxItem item, const Rung& cur, uint64_t now_ms) {
    (void)now_ms;
    switch (item.cls) {
      case TxClass::Control:
        if (ctl_.size() >= cfg_.control_max) {
          drops_.control++;
          return false;
        }
        ctl_.push_back(std::move(item));
        return true;
      case TxClass::IpInteractive:
        if (inter_.size() >= cfg_.ip_max) {
          drops_.ip_pkts++;
          return false;
        }
        inter_.push_back(std::move(item));
        return true;
      case TxClass::IpBulk:
        if (bulk_.size() >= cfg_.ip_max) {
          drops_.ip_pkts++;
          return false;
        }
        bulk_.push_back(std::move(item));
        return true;
      case TxClass::Video:
        video_.push_back(std::move(item));
        return trim_video(cur, /*report_newest=*/true);
    }
    return false;
  }

  std::optional<TxItem> dequeue(const Rung& cur, uint64_t now_ms) {
    refill(now_ms);
    trim_video(cur, false);
    if (!ctl_.empty()) return pop(ctl_);
    if (!inter_.empty() && tokens_us_ >= int64_t(single_item_airtime_us(cur, inter_.front().payload.size())))
      return pop(inter_);
    if (!video_.empty()) return pop(video_);
    if (!bulk_.empty()) return pop(bulk_);
    return std::nullopt;
  }

  // Charges the item's airtime at the current rung to the interactive bucket
  // (interactive items only) and to the duty window.
  void on_sent(const TxItem& item, const Rung& cur, uint64_t now_ms) {
    refill(now_ms);
    const uint32_t air = item_airtime_us(item, cur);
    if (item.cls == TxClass::IpInteractive) tokens_us_ -= int64_t(air);
    sent_.emplace_back(now_ms, air);
    prune_sent(now_ms);
  }

  uint32_t q_video_air_ms(const Rung& cur) const { return uint32_t(q_video_air_us(cur) / 1000); }
  uint32_t q_ip_air_ms(const Rung& cur) const {
    uint64_t us = 0;
    for (const auto& it : inter_) us += single_item_airtime_us(cur, it.payload.size());
    for (const auto& it : bulk_) us += single_item_airtime_us(cur, it.payload.size());
    return uint32_t(us / 1000);
  }
  size_t q_control() const { return ctl_.size(); }
  size_t q_video() const { return video_.size(); }
  size_t q_interactive() const { return inter_.size(); }
  size_t q_bulk() const { return bulk_.size(); }

  // Airtime sent in (now - bucket_window_ms, now] over the window.
  uint32_t tx_duty_permille(uint64_t now_ms) const {
    const uint64_t window = cfg_.bucket_window_ms ? cfg_.bucket_window_ms : 1;
    const uint64_t since = now_ms >= window ? now_ms - window : 0;
    uint64_t us = 0;
    for (const auto& s : sent_)
      if (s.first > since && s.first <= now_ms) us += s.second;
    return uint32_t(us / window);  // us / ms = permille
  }

  struct Drops {
    uint32_t video_frames = 0;   // every video item dropped (enhance included)
    uint32_t video_enhance = 0;  // of those, kFlagEnhance items
    uint32_t ip_pkts = 0;
    uint32_t control = 0;
  };
  Drops drops() const { return drops_; }

 private:
  uint64_t capacity_us() const {
    return uint64_t(cfg_.bucket_window_ms) * 1000 * cfg_.interactive_share_permille / 1000;
  }
  void refill(uint64_t now_ms) {
    if (!tokens_init_) {
      tokens_init_ = true;
      tokens_at_ms_ = now_ms;
      return;
    }
    if (now_ms <= tokens_at_ms_) return;
    // share permille of 1000 us per ms = share us per ms.
    tokens_us_ += int64_t((now_ms - tokens_at_ms_) * cfg_.interactive_share_permille);
    tokens_us_ = std::min(tokens_us_, int64_t(capacity_us()));
    tokens_at_ms_ = now_ms;
  }
  void prune_sent(uint64_t now_ms) {
    const uint64_t window = cfg_.bucket_window_ms;
    while (!sent_.empty() && sent_.front().first + window <= now_ms) sent_.pop_front();
  }
  static uint32_t item_airtime_us(const TxItem& it, const Rung& cur) {
    return it.cls == TxClass::Video ? video_item_airtime_us(cur, it.payload.size())
                                    : single_item_airtime_us(cur, it.payload.size());
  }
  uint64_t q_video_air_us(const Rung& cur) const {
    uint64_t us = 0;
    for (const auto& it : video_) us += video_item_airtime_us(cur, it.payload.size());
    return us;
  }
  static TxItem pop(std::deque<TxItem>& q) {
    TxItem it = std::move(q.front());
    q.pop_front();
    return it;
  }

  // Enforce the video airtime bound. Returns whether the newest item (the one
  // just pushed when report_newest) survived.
  bool trim_video(const Rung& cur, bool report_newest) {
    bool newest_alive = true;
    const uint64_t bound_us = uint64_t(cfg_.video_queue_air_ms) * 1000;
    while (video_.size() > 1 && q_video_air_us(cur) > bound_us) {
      auto victim = video_.end();
      for (auto it = video_.rbegin(); it != video_.rend(); ++it)
        if ((it->flags & kFlagEnhance) && !(it->flags & kFlagIdr)) { victim = std::prev(it.base()); break; }
      if (victim == video_.end())
        for (auto it = video_.rbegin(); it != video_.rend(); ++it)
          if (!(it->flags & kFlagIdr)) { victim = std::prev(it.base()); break; }
      if (victim == video_.end()) victim = std::prev(video_.end());  // all IDR: newest
      if (report_newest && newest_alive && victim == std::prev(video_.end())) newest_alive = false;
      drops_.video_frames++;
      if (victim->flags & kFlagEnhance) drops_.video_enhance++;
      video_.erase(victim);
    }
    return newest_alive;
  }

  SchedCfg cfg_;
  std::deque<TxItem> ctl_, inter_, video_, bulk_;
  int64_t tokens_us_;
  bool tokens_init_ = false;
  uint64_t tokens_at_ms_ = 0;
  std::deque<std::pair<uint64_t, uint32_t>> sent_;  // (ms, airtime us)
  Drops drops_;
};

}  // namespace p2p
