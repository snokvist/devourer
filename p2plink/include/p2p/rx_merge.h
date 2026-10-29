// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink receive merge (ground, N ears) — docs/DESIGN.md "RX merge".
//
// Every ear (monitor adapter) hands its video symbols to one BlockMerger.
// Key (stream, block_id, fec_idx): the first copy wins, later copies count as
// `diversity`. Blocks are tracked in block_id order (u16, wrapping — compared
// as an int16 difference) and go through two cursors:
//
//   emit   — a block is EMITTED as soon as it is decodable: all k sources
//            present (fast path, no FEC) or >= k symbols present (RsCodec).
//            Emission is strictly in increasing block_id order; a block that
//            cannot complete is emitted ok=false (declared lost) when, in
//            order: (1) every non-silent ear has delivered a symbol of a NEWER
//            block, (2) a block_id >= this + window_blocks has been seen,
//            (3) gap_dwell_ms elapsed since the block's first symbol.
//   retire — an emitted block stays in the pending set until the same three
//            rules say no more copies can come, so its repair symbols that
//            trail the k-th source still count as `uniq` rather than as loss.
//            At retirement, n - present symbols are accounted as LOST (source
//            and repair alike: this is the post-merge, PRE-FEC symbol loss
//            LinkReport.loss_permille carries). A symbol for a retired or
//            skipped block_id, or older than cursor - window_blocks, is `late`.
//
// Anti-phantom: an ear with no frames for silent_ear_ms while a sibling
// delivered is `silent` — excluded from rule (1) and from `adapters`.
//
// A block of which no symbol ever arrived is invisible here (its k/n/rung are
// unknown); the gap rules are applied to the missing id so the next block is
// not emitted ahead of a copy a slow ear may still bring, and the frame
// consumer sees the block_id gap.
//
// Restart: a block_id more than kResyncBlocks behind the retire cursor is a
// sender restart, not a late copy — the pending set is flushed (emitted or
// declared lost on the next poll) and the cursors rebase on the new id.
//
// One BlockMerger serves one block_id sequence (one video stream); `stream`
// is part of the key only in that a symbol whose stream differs from the
// block's first symbol is rejected.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <vector>

#include "p2p/fec.h"
#include "p2p/report.h"
#include "p2p/wire.h"

namespace p2p {

struct MergeConfig {
  uint32_t gap_dwell_ms = 20;     // rule (3): declare lost after this much time since the first symbol
  uint32_t silent_ear_ms = 200;   // anti-phantom: ear idle this long while a sibling delivers
  uint8_t window_blocks = 4;      // rule (2) and the acceptance window behind the newest block
  uint8_t max_ears = 8;           // ear indices >= this are rejected (<= kMaxEars)
  uint32_t stats_window_ms = 100; // the cadence the owner is expected to call take_window() at
};

struct DecodedBlock {
  uint16_t block_id = 0;
  bool ok = false;           // frame carries the encoder frame; false = declared lost / undecodable
  bool used_fec = false;     // decoded with RsCodec rather than the all-sources fast path
  uint8_t lost_symbols = 0;  // n - symbols present at emission (source and repair alike)
  uint8_t rung = 0;          // ladder rung of the block (its first accepted symbol)
  uint8_t flags = 0;         // OR of the accepted symbols' P2pHeader.flags
  std::array<uint8_t, 8> meta{};    // VencFrameMeta bytes from the block prefix (ok only)
  std::vector<uint8_t> frame;       // encoder frame bytes, empty when !ok
};

struct WindowStats {
  uint32_t uniq = 0;         // symbols accepted (first copies) this window
  uint32_t diversity = 0;    // duplicate copies dropped this window
  uint32_t late = 0;         // symbols behind the cursor / window
  uint32_t rejected = 0;     // malformed: wrong type/ear/len, k/n/stream mismatch
  uint32_t blocks_ok = 0;    // emitted via the fast path
  uint32_t blocks_fec = 0;   // emitted after an RS decode
  uint32_t blocks_failed = 0;  // emitted ok=false
  uint16_t loss_permille = 0;      // lost / (uniq + lost), lost = retired blocks' missing symbols
  uint16_t fec_fail_permille = 0;  // blocks_failed / blocks emitted
  int8_t rssi_best = -127;   // best frame of any ear this window
  int8_t rssi_mean = -127;   // mean of the per-ear means over ears that heard anything
  uint8_t adapters = 0;      // ears that delivered this window and are not silent
  uint8_t rung_seen = 0;     // rung with the most accepted symbols this window
  std::array<uint32_t, 8> rung_ok{};    // accepted symbols per rung
  std::array<uint32_t, 8> rung_lost{};  // missing symbols of retired blocks, at the block's rung
  std::array<uint32_t, 8> ear_rx{};     // frames seen per ear (first copies and duplicates)
};

class BlockMerger {
 public:
  static constexpr uint8_t kMaxEars = 8;
  static constexpr uint8_t kMaxRungs = 8;
  // A block_id this far behind the retire cursor is a sender restart.
  static constexpr int64_t kResyncBlocks = 1024;

  explicit BlockMerger(MergeConfig cfg = {}) : cfg_(cfg) {
    if (cfg_.max_ears > kMaxEars) cfg_.max_ears = kMaxEars;
    if (cfg_.window_blocks == 0) cfg_.window_blocks = 1;
  }

  // One Type::VideoData / Type::VideoRepair symbol as heard by `ear`.
  // `payload` is copied. Returns true when the symbol is newly accepted;
  // false for a duplicate (diversity), a late one, or a rejected one.
  bool on_symbol(uint8_t ear, const P2pHeader& h, const uint8_t* payload, size_t len,
                 int8_t rssi_dbm, uint64_t now_ms) {
    if (h.type != uint8_t(Type::VideoData) && h.type != uint8_t(Type::VideoRepair)) {
      win_.rejected++;
      return false;
    }
    if (ear >= cfg_.max_ears || payload == nullptr || len == 0 || h.fec_k == 0 ||
        h.fec_n < h.fec_k || h.fec_idx >= h.fec_n) {
      win_.rejected++;
      return false;
    }

    // Ear bookkeeping: any well-formed video frame is evidence the ear hears,
    // duplicate or not, and a copy of a newer block advances the ear for rule (1).
    Ear& e = ears_[ear];
    e.seen = true;
    e.last_ms = now_ms;
    e.win_rx++;
    e.win_rssi_sum += rssi_dbm;
    if (!win_.have_rssi || rssi_dbm > win_.rssi_best) win_.rssi_best = rssi_dbm;
    win_.have_rssi = true;

    if (!have_cursor_) {
      have_cursor_ = true;
      highest_u_ = emit_next_u_ = retire_next_u_ = int64_t(h.block_id);
    }
    int64_t u = unwrap(h.block_id);
    if (u < retire_next_u_ - kResyncBlocks) {
      // Sender restart: flush what is pending, rebase on the new id.
      for (auto& kv : pending_) stale_.push_back(std::move(kv.second));
      pending_.clear();
      highest_u_ = emit_next_u_ = retire_next_u_ = u;
    }
    if (!e.has_highest || u > e.highest_u) {
      e.has_highest = true;
      e.highest_u = u;
    }

    auto it = pending_.find(u);
    if (u < retire_next_u_ || highest_u_ - u > int64_t(cfg_.window_blocks) ||
        (it == pending_.end() && u < emit_next_u_)) {
      win_.late++;
      return false;
    }
    if (u > highest_u_) highest_u_ = u;

    if (it == pending_.end()) {
      Block b;
      b.id = h.block_id;
      b.u = u;
      b.stream = h.stream;
      b.k = h.fec_k;
      b.n = h.fec_n;
      b.rung = h.rung;
      b.sym_len = len;
      b.first_ms = now_ms;
      b.sym.resize(h.fec_n);
      it = pending_.emplace(u, std::move(b)).first;
    }
    Block& b = it->second;
    if (b.stream != h.stream || b.k != h.fec_k || b.n != h.fec_n || b.sym_len != len) {
      win_.rejected++;
      return false;
    }
    if (b.present[h.fec_idx]) {
      win_.diversity++;
      total_diversity_++;
      return false;
    }
    b.sym[h.fec_idx].assign(payload, payload + len);
    b.present[h.fec_idx] = true;
    b.n_present++;
    if (h.fec_idx < b.k) b.n_src++;
    b.flags |= h.flags;
    win_.uniq++;
    total_uniq_++;
    if (h.rung < kMaxRungs) win_.rung_ok[h.rung]++;
    return true;
  }

  // Emits (in block_id order) every block that became decodable or lost, and
  // retires the emitted blocks the gap rules have closed.
  void poll(uint64_t now_ms, const std::function<void(const DecodedBlock&)>& emit) {
    for (Block& b : stale_) {
      if (!b.emitted) emit_block(b, !decodable(b), emit);
      account_retire(b);
    }
    stale_.clear();

    // Pass A: emit.
    for (auto it = pending_.lower_bound(emit_next_u_); it != pending_.end(); ++it) {
      Block& b = it->second;
      if (b.u > emit_next_u_) {
        // Ids in (emit_next_u_, b.u) were never heard: hold the next block
        // for them under the same rules, dated from when the gap was noticed.
        if (!gap_closed(emit_next_u_, b.first_ms, now_ms)) break;
        emit_next_u_ = b.u;
      }
      if (decodable(b)) {
        emit_block(b, false, emit);
      } else if (gap_closed(b.u, b.first_ms, now_ms)) {
        emit_block(b, true, emit);
      } else {
        break;
      }
      emit_next_u_ = b.u + 1;
    }

    // Pass B: retire, strictly in order behind the emit cursor.
    for (auto it = pending_.begin(); it != pending_.end();) {
      Block& b = it->second;
      if (!b.emitted) break;
      if (!b.lost && !gap_closed(b.u, b.first_ms, now_ms)) break;
      account_retire(b);
      retire_next_u_ = b.u + 1;
      it = pending_.erase(it);
    }
  }

  // Returns the window counters and resets them. Cumulative totals are
  // total_uniq() / total_diversity().
  WindowStats take_window(uint64_t now_ms) {
    WindowStats w;
    w.uniq = win_.uniq;
    w.diversity = win_.diversity;
    w.late = win_.late;
    w.rejected = win_.rejected;
    w.blocks_ok = win_.blocks_ok;
    w.blocks_fec = win_.blocks_fec;
    w.blocks_failed = win_.blocks_failed;
    w.loss_permille = permille(win_.lost, uint64_t(win_.uniq) + win_.lost);
    w.fec_fail_permille = permille(win_.blocks_failed,
                                   uint64_t(win_.blocks_ok) + win_.blocks_fec + win_.blocks_failed);
    w.rssi_best = win_.have_rssi ? win_.rssi_best : int8_t(-127);
    w.rung_ok = win_.rung_ok;
    w.rung_lost = win_.rung_lost;

    int32_t mean_sum = 0;
    uint32_t mean_n = 0;
    uint32_t best_rung = 0;
    for (uint8_t r = 1; r < kMaxRungs; r++)
      if (w.rung_ok[r] > w.rung_ok[best_rung]) best_rung = r;
    w.rung_seen = uint8_t(best_rung);
    for (uint8_t i = 0; i < cfg_.max_ears; i++) {
      Ear& e = ears_[i];
      w.ear_rx[i] = e.win_rx;
      if (e.win_rx > 0) {
        mean_sum += int32_t(e.win_rssi_sum / int64_t(e.win_rx));
        mean_n++;
        if (!ear_silent(i, now_ms)) w.adapters++;
      }
      e.win_rx = 0;
      e.win_rssi_sum = 0;
    }
    w.rssi_mean = mean_n ? int8_t(mean_sum / int32_t(mean_n)) : int8_t(-127);

    win_ = Win{};
    window_start_ms_ = now_ms;
    return w;
  }

  // Fills a LinkReport from take_window(); the uniq/diversity fields are the
  // cumulative totals, the rung fields read the probe/current rung slots.
  LinkReport make_report(uint32_t epoch, uint64_t now_ms, uint8_t probe_rung, uint8_t cur_rung,
                         uint16_t ip_rx) {
    WindowStats w = take_window(now_ms);
    LinkReport r;
    r.epoch = epoch;
    r.uniq = uint32_t(total_uniq_);
    r.diversity = uint32_t(total_diversity_);
    r.loss_permille = w.loss_permille;
    r.fec_fail_permille = w.fec_fail_permille;
    r.rssi_best = w.rssi_best;
    r.rssi_mean = w.rssi_mean;
    r.adapters = w.adapters;
    r.rung_seen = w.rung_seen;
    r.probe_rung_ok = rung_u16(w.rung_ok, probe_rung);
    r.probe_rung_lost = rung_u16(w.rung_lost, probe_rung);
    r.cur_rung_ok = rung_u16(w.rung_ok, cur_rung);
    r.cur_rung_lost = rung_u16(w.rung_lost, cur_rung);
    r.ip_rx = ip_rx;
    return r;
  }

  uint64_t total_uniq() const { return total_uniq_; }
  uint64_t total_diversity() const { return total_diversity_; }
  size_t pending_blocks() const { return pending_.size(); }
  uint64_t window_start_ms() const { return window_start_ms_; }
  const MergeConfig& config() const { return cfg_; }

  // Anti-phantom: `ear` heard something, nothing for silent_ear_ms, and a
  // sibling delivered since its last frame.
  bool ear_silent(uint8_t ear, uint64_t now_ms) const {
    if (ear >= cfg_.max_ears) return false;
    const Ear& e = ears_[ear];
    if (!e.seen || now_ms < e.last_ms + cfg_.silent_ear_ms) return false;
    for (uint8_t j = 0; j < cfg_.max_ears; j++)
      if (j != ear && ears_[j].seen && ears_[j].last_ms > e.last_ms) return true;
    return false;
  }

 private:
  struct Ear {
    bool seen = false;
    bool has_highest = false;
    uint64_t last_ms = 0;
    int64_t highest_u = 0;
    uint32_t win_rx = 0;
    int64_t win_rssi_sum = 0;
  };

  struct Block {
    uint16_t id = 0;
    int64_t u = 0;
    uint8_t stream = 0, k = 0, n = 0, rung = 0, flags = 0;
    uint8_t n_present = 0, n_src = 0;
    bool emitted = false;
    bool lost = false;  // emitted ok=false: retire as soon as order allows
    size_t sym_len = 0;
    uint64_t first_ms = 0;
    std::array<bool, 256> present{};
    std::vector<std::vector<uint8_t>> sym;
  };

  struct Win {
    uint32_t uniq = 0, diversity = 0, late = 0, rejected = 0;
    uint32_t blocks_ok = 0, blocks_fec = 0, blocks_failed = 0;
    uint64_t lost = 0;
    bool have_rssi = false;
    int8_t rssi_best = -127;
    std::array<uint32_t, 8> rung_ok{};
    std::array<uint32_t, 8> rung_lost{};
  };

  static int16_t bdiff(uint16_t a, uint16_t b) { return int16_t(uint16_t(a - b)); }
  int64_t unwrap(uint16_t id) const { return highest_u_ + bdiff(id, uint16_t(highest_u_)); }

  static uint16_t permille(uint64_t part, uint64_t whole) {
    if (whole == 0) return 0;
    uint64_t v = part * 1000 / whole;
    return uint16_t(v > 1000 ? 1000 : v);
  }
  static uint16_t rung_u16(const std::array<uint32_t, 8>& a, uint8_t rung) {
    if (rung >= kMaxRungs) return 0;
    return uint16_t(a[rung] > 0xffff ? 0xffff : a[rung]);
  }

  static bool decodable(const Block& b) { return b.n_present >= b.k; }

  // Rule (1): every non-silent ear that ever delivered is past `u`.
  bool rule_ears_past(int64_t u, uint64_t now_ms) const {
    bool any = false;
    for (uint8_t i = 0; i < cfg_.max_ears; i++) {
      const Ear& e = ears_[i];
      if (!e.seen || ear_silent(i, now_ms)) continue;
      any = true;
      if (!e.has_highest || e.highest_u <= u) return false;
    }
    return any;
  }
  // Rules (1), (2), (3) in order for block (or missing id) `u` first noticed at `first_ms`.
  bool gap_closed(int64_t u, uint64_t first_ms, uint64_t now_ms) const {
    if (rule_ears_past(u, now_ms)) return true;
    if (highest_u_ - u >= int64_t(cfg_.window_blocks)) return true;
    return now_ms >= first_ms + cfg_.gap_dwell_ms;
  }

  void account_retire(const Block& b) {
    uint32_t lost = uint32_t(b.n) - b.n_present;
    win_.lost += lost;
    if (b.rung < kMaxRungs) win_.rung_lost[b.rung] += lost;
  }

  RsCodec& codec(uint8_t k, uint8_t n) {
    uint16_t key = uint16_t((uint16_t(k) << 8) | n);
    auto it = codecs_.find(key);
    if (it == codecs_.end()) it = codecs_.try_emplace(key, k, n).first;
    return it->second;
  }

  // Reassembles the k source symbols (decoding if needed) and unpacks
  // [u32 frame_len][u8 meta[8]][frame][pad]. False on any inconsistency.
  bool assemble(Block& b, DecodedBlock& d) {
    const size_t k = b.k, n = b.n, L = b.sym_len;
    std::vector<uint8_t> buf(k * L);
    if (b.n_src == b.k) {
      for (size_t i = 0; i < k; i++) std::memcpy(buf.data() + i * L, b.sym[i].data(), L);
    } else {
      std::vector<std::vector<uint8_t>> zero;  // backing for symbols never heard
      std::vector<const uint8_t*> syms(n);
      std::vector<uint8_t*> out(k);
      for (size_t i = 0; i < n; i++) {
        if (b.present[i]) {
          syms[i] = b.sym[i].data();
        } else {
          zero.emplace_back(L, uint8_t(0));
          syms[i] = zero.back().data();
        }
      }
      for (size_t i = 0; i < k; i++) {
        out[i] = buf.data() + i * L;
        if (b.present[i]) std::memcpy(out[i], b.sym[i].data(), L);
      }
      if (!codec(b.k, b.n).decode(syms.data(), b.present.data(), out.data(), L)) return false;
      d.used_fec = true;
    }
    if (buf.size() < kBlockPrefixLen) return false;
    uint32_t frame_len = get_le32(buf.data());
    if (frame_len > buf.size() - kBlockPrefixLen) return false;
    std::memcpy(d.meta.data(), buf.data() + 4, 8);
    d.frame.assign(buf.begin() + kBlockPrefixLen, buf.begin() + kBlockPrefixLen + frame_len);
    return true;
  }

  void emit_block(Block& b, bool as_lost, const std::function<void(const DecodedBlock&)>& emit) {
    DecodedBlock d;
    d.block_id = b.id;
    d.rung = b.rung;
    d.flags = b.flags;
    uint32_t lost = uint32_t(b.n) - b.n_present;
    d.lost_symbols = uint8_t(lost > 255 ? 255 : lost);
    if (!as_lost && assemble(b, d)) {
      d.ok = true;
      if (d.used_fec) win_.blocks_fec++; else win_.blocks_ok++;
    } else {
      d.ok = false;
      d.used_fec = false;
      d.frame.clear();
      d.meta.fill(0);
      win_.blocks_failed++;
    }
    b.emitted = true;
    b.lost = !d.ok;
    emit(d);
  }

  MergeConfig cfg_;
  std::array<Ear, kMaxEars> ears_{};
  std::map<int64_t, Block> pending_;
  std::vector<Block> stale_;
  std::map<uint16_t, RsCodec> codecs_;
  bool have_cursor_ = false;
  int64_t highest_u_ = 0;      // newest block seen (unwrapped)
  int64_t emit_next_u_ = 0;    // lowest block not yet emitted
  int64_t retire_next_u_ = 0;  // lowest block not yet retired
  Win win_{};
  uint64_t window_start_ms_ = 0;
  uint64_t total_uniq_ = 0;
  uint64_t total_diversity_ = 0;
};

// Cross-ear duplicate suppression for the per-type seq counters (IpData,
// Probe, LinkReport): a 4096-wide bitmap per (type, stream). The first sight
// of a seq is accepted; a seq more than 2048 ahead of the newest resets the
// window (forward-jump resync); one more than 4096 behind is refused.
class SeqDedup {
 public:
  static constexpr uint32_t kWindow = 4096;
  static constexpr uint32_t kJump = 2048;

  bool accept(uint8_t type, uint8_t stream, uint32_t seq) {
    Win& w = wins_[uint16_t((uint16_t(type) << 8) | stream)];
    if (!w.init) {
      w.init = true;
      w.newest = seq;
      w.bits.fill(0);
      set(w, seq);
      return true;
    }
    int32_t d = int32_t(seq - w.newest);
    if (d == 0) return false;
    if (d > 0) {
      if (uint32_t(d) > kJump) {
        w.bits.fill(0);
      } else {
        for (uint32_t s = w.newest + 1; s != seq; s++) clear(w, s);
      }
      w.newest = seq;
      set(w, seq);
      return true;
    }
    if (uint32_t(-d) >= kWindow) return false;
    if (test(w, seq)) return false;
    set(w, seq);
    return true;
  }

 private:
  struct Win {
    bool init = false;
    uint32_t newest = 0;
    std::array<uint64_t, kWindow / 64> bits{};
  };
  static void set(Win& w, uint32_t s) { w.bits[(s % kWindow) / 64] |= uint64_t(1) << (s % 64); }
  static void clear(Win& w, uint32_t s) { w.bits[(s % kWindow) / 64] &= ~(uint64_t(1) << (s % 64)); }
  static bool test(const Win& w, uint32_t s) { return (w.bits[(s % kWindow) / 64] >> (s % 64)) & 1; }
  std::map<uint16_t, Win> wins_;
};

}  // namespace p2p
