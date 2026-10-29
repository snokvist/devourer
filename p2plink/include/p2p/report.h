// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink LinkReport (ground -> drone, Type::LinkReport payload) and the
// local StatsSnapshot the reporting layer publishes to apps. Pure.
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include "p2p/wire.h"

namespace p2p {

// Sent by the ground at 10 Hz as a GROUP-addressed, NOACK frame at the basic
// rate. Secondary uplink sensor: the primary is the probe's hardware ACK
// (DaMachine / LinkController.ack8). Every field is "since the last report"
// unless marked cumulative. 32 bytes.
struct LinkReport {
  uint32_t epoch = 0;        // monotonic report counter (freshness)
  uint32_t uniq = 0;         // cumulative unique video symbols delivered post-merge
  uint32_t diversity = 0;    // cumulative duplicate copies dropped by the merge
  uint16_t loss_permille = 0;    // post-merge, pre-FEC symbol loss, this window
  uint16_t fec_fail_permille = 0;// blocks that could not be decoded, this window
  int8_t rssi_best = -127;   // best ear, dBm, this window
  int8_t rssi_mean = -127;   // mean over ears that heard anything
  uint8_t adapters = 0;      // ears currently delivering (anti-phantom excluded)
  uint8_t rung_seen = 0;     // rung of most frames received this window
  uint16_t probe_rung_ok = 0;    // frames received at the probe (candidate) rung
  uint16_t probe_rung_lost = 0;  // frames inferred lost at the probe rung (seq gaps)
  uint16_t cur_rung_ok = 0;
  uint16_t cur_rung_lost = 0;
  uint16_t ip_rx = 0;        // IP packets delivered to tun this window
  uint16_t reserved = 0;
};
inline constexpr size_t kLinkReportLen = 32;

inline void encode_report(const LinkReport& r, uint8_t out[kLinkReportLen]) {
  put_le32(out + 0, r.epoch);
  put_le32(out + 4, r.uniq);
  put_le32(out + 8, r.diversity);
  put_le16(out + 12, r.loss_permille);
  put_le16(out + 14, r.fec_fail_permille);
  out[16] = uint8_t(r.rssi_best);
  out[17] = uint8_t(r.rssi_mean);
  out[18] = r.adapters;
  out[19] = r.rung_seen;
  put_le16(out + 20, r.probe_rung_ok);
  put_le16(out + 22, r.probe_rung_lost);
  put_le16(out + 24, r.cur_rung_ok);
  put_le16(out + 26, r.cur_rung_lost);
  put_le16(out + 28, r.ip_rx);
  put_le16(out + 30, r.reserved);
}

inline std::optional<LinkReport> decode_report(const uint8_t* p, size_t len) {
  if (p == nullptr || len < kLinkReportLen) return std::nullopt;
  LinkReport r;
  r.epoch = get_le32(p + 0);
  r.uniq = get_le32(p + 4);
  r.diversity = get_le32(p + 8);
  r.loss_permille = get_le16(p + 12);
  r.fec_fail_permille = get_le16(p + 14);
  r.rssi_best = int8_t(p[16]);
  r.rssi_mean = int8_t(p[17]);
  r.adapters = p[18];
  r.rung_seen = p[19];
  r.probe_rung_ok = get_le16(p + 20);
  r.probe_rung_lost = get_le16(p + 22);
  r.cur_rung_ok = get_le16(p + 24);
  r.cur_rung_lost = get_le16(p + 26);
  r.ip_rx = get_le16(p + 28);
  r.reserved = get_le16(p + 30);
  return r;
}

// Probe payload (Type::Probe, unicast to the ACK target). 8 bytes.
struct Probe {
  uint32_t seq = 0;
  uint32_t t_ms = 0;  // sender monotonic ms, for the ground's own latency view
};
inline constexpr size_t kProbeLen = 8;
inline void encode_probe(const Probe& p, uint8_t out[kProbeLen]) { put_le32(out, p.seq); put_le32(out + 4, p.t_ms); }
inline std::optional<Probe> decode_probe(const uint8_t* p, size_t len) {
  if (len < kProbeLen) return std::nullopt;
  return Probe{get_le32(p), get_le32(p + 4)};
}

// What the reporting layer publishes locally (shm snapshot + NDJSON line).
// Fields are plain integers so the struct is a stable C layout for a shm
// consumer; a version bump changes kSnapshotVersion.
inline constexpr uint32_t kSnapshotVersion = 1;
struct StatsSnapshot {
  uint32_t version = kSnapshotVersion;
  uint32_t seq = 0;              // bumped on every publish
  uint64_t t_ms = 0;             // monotonic ms
  // radio
  uint8_t channel = 0, bw_mhz = 0, rung = 0, link_state = 0; // link_state: LinkController::State
  uint8_t da_mode = 0, ack8 = 0, adapters = 0, reserved0 = 0;
  // budget (kbit/s of payload, by the budget.h airtime model: capacity at the
  // duty cap before FEC, video budget after FEC + reserve, IP at the interactive share)
  uint32_t est_capacity_kbps = 0, video_budget_kbps = 0, ip_budget_kbps = 0;
  uint32_t tx_duty_permille = 0;
  // queues (airtime ms at the current rung)
  uint32_t q_video_ms = 0, q_ip_ms = 0, q_ctl_frames = 0;
  // feedback
  uint32_t report_age_ms = 0, probe_age_ms = 0;
  uint32_t retries_mean_centi = 0;   // mean hardware retries x100 over the last window
  int16_t rssi_best = -127, rssi_mean = -127;
  uint16_t loss_permille = 0, fec_fail_permille = 0;
  uint8_t fec_k = 0, fec_n = 0, frame_bytes_hi = 0, frame_bytes_lo = 0;
  uint32_t uniq = 0, diversity = 0, dropped_video_frames = 0, dropped_ip_pkts = 0;
  uint32_t tx_submitted = 0, tx_failed = 0, tx_timeouts = 0;
};

}  // namespace p2p
