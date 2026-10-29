// SPDX-License-Identifier: GPL-2.0-or-later
// Airtime model + TxScheduler: priority, interactive bucket, video bound, duty.
#include <cstdint>
#include <cstdio>
#include <vector>
#include "p2p/budget.h"

using namespace p2p;

static int g_fail = 0;
static void check(bool ok, const char* what) {
  std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) g_fail++;
}

static TxItem item(TxClass cls, uint32_t id, size_t bytes, uint8_t flags = 0, uint64_t t = 0) {
  return TxItem{cls, 0, flags, std::vector<uint8_t>(bytes, 0xAB), t, id};
}

static void test_airtime() {
  std::puts("-- airtime model");
  const uint32_t a0 = frame_airtime_us(kLadder[0], 1400);
  const uint32_t a6 = frame_airtime_us(kLadder[6], 1400);
  const uint32_t ovh = kPlcpHtMixedUs + kIfsBackoffUs;
  std::printf("      R0 1400 B: %u us (data %u)  R6 1400 B: %u us (data %u)  overhead %u\n", a0, a0 - ovh, a6, a6 - ovh, ovh);
  check(a0 - ovh >= 1740 && a0 - ovh <= 1790, "R0 1400 B data time ~1.76 ms (1428 B MPDU at 6.5 Mb/s)");
  check(a6 - ovh >= 100 && a6 - ovh <= 120, "R6 1400 B data time ~0.11 ms");
  check(a0 == 1850 && a6 == 194, "exact: R0 1850 us, R6 194 us");
  check((a0 - ovh) % 4 == 0 && (a6 - ovh) % 4 == 0, "data time is whole 4 us symbols");
  check(frame_airtime_us(kLadder[0], 0) == ovh + 40, "0 B body: 28 B hdr+FCS -> 40 us + overhead");
  check(frame_airtime_us(kLadder[1], 1400) < a0 && frame_airtime_us(kLadder[5], 1400) > a6, "monotonic in PHY rate");
  check(frame_airtime_us(kLadder[4], 1400) < frame_airtime_us(kLadder[3], 1400), "R4 (HT40 MCS1, 27 Mb/s) shorter than R3 (HT20 MCS3, 26)");
  check(video_item_airtime_us(kLadder[0], 1000) == a0 * 2, "1-symbol block at R0 = 12/8 -> 2 frames (ceil)");
  check(video_item_airtime_us(kLadder[0], 8 * 1382) == a0 * 12, "8-symbol block at R0 -> 12 frames");
  check(single_item_airtime_us(kLadder[6], 1382) == a6, "IP packet filling a frame = one frame");
  check(single_item_airtime_us(kLadder[6], 1383) == 2 * a6, "IP packet one byte over spans two frames");
  check(video_budget_kbps(kLadder[0], 0) == 2318 && video_budget_kbps(kLadder[5], 0) == 14395 &&
            video_budget_kbps(kLadder[6], 0) == 19900,
        "video_budget_kbps R0 2318, R5 14395, R6 19900 (airtime model, not PHY x duty)");
  check(video_budget_kbps(kLadder[0], 300) == 2018 && video_budget_kbps(kLadder[0], 5000) == 0, "control reserve subtracts, floors at 0");
  check(payload_kbps_at_share(kLadder[6], 1000) == 1382u * 8 * 1000 / a6, "payload at full airtime = frames/s x 1382 B");
}

// Feeding exactly the budget, in whole k_r-symbol blocks, charges the duty
// window with 0.95..1.0 of the rung's duty cap: the budget and the meter are
// the same airtime model (C7 fed PHY x duty and read 707 permille at R5 against
// a 500 cap).
static void test_budget_fills_duty() {
  std::puts("-- budget fills the duty cap, in the scheduler's own charge");
  for (size_t i = 0; i < kLadder.size(); i++) {
    const Rung& r = kLadder[i];
    const uint64_t block_bytes = uint64_t(r.fec_k) * frame_payload_capacity(r);
    const double blocks_per_s = video_budget_kbps(r, 0) * 1000.0 / 8 / double(block_bytes);
    const double duty = blocks_per_s * video_item_airtime_us(r, size_t(block_bytes)) / 1000.0;
    std::printf("      R%zu budget %u kbps -> duty %.0f permille (cap %u)\n", i, video_budget_kbps(r, 0), duty,
                unsigned(r.duty_permille));
    check(duty <= r.duty_permille && duty >= 0.95 * r.duty_permille, "duty within [0.95, 1.0] x cap");
  }
}

static void test_priority() {
  std::puts("-- strict priority");
  TxScheduler s;
  const Rung& r = kLadder[6];
  check(s.enqueue(item(TxClass::IpBulk, 1, 500), r, 0), "enqueue bulk");
  check(s.enqueue(item(TxClass::Video, 2, 5000), r, 0), "enqueue video");
  check(s.enqueue(item(TxClass::Control, 3, 8), r, 0), "enqueue control");
  check(s.enqueue(item(TxClass::IpInteractive, 4, 200), r, 0), "enqueue interactive");
  check(s.q_control() == 1, "q_control 1");
  uint32_t order[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; i++) {
    auto it = s.dequeue(r, 10);
    if (!it) break;
    order[i] = it->id;
    s.on_sent(*it, r, 10);
  }
  check(order[0] == 3 && order[1] == 4 && order[2] == 2 && order[3] == 1, "control, interactive, video, bulk");
  check(!s.dequeue(r, 10).has_value(), "empty -> nullopt");
  // Control before everything, every time it is queued.
  s.enqueue(item(TxClass::Video, 10, 1000), r, 20);
  s.enqueue(item(TxClass::Control, 11, 8), r, 20);
  auto it = s.dequeue(r, 20);
  check(it && it->id == 11, "late control still first");
}

static void test_interactive_bucket() {
  std::puts("-- interactive token bucket (15 % of window airtime)");
  TxScheduler s;
  const Rung& r = kLadder[6];
  const uint32_t per = single_item_airtime_us(r, 1000);
  for (uint32_t i = 0; i < 200; i++) s.enqueue(item(TxClass::IpInteractive, i, 1000), r, 1000);
  s.enqueue(item(TxClass::Video, 900, 1000), r, 1000);
  uint64_t sent_us = 0;
  uint32_t n = 0;
  for (;;) {
    auto it = s.dequeue(r, 1000);
    if (!it || it->cls != TxClass::IpInteractive) {
      check(it && it->cls == TxClass::Video, "bucket exhausted -> video goes next");
      if (it) s.on_sent(*it, r, 1000);
      break;
    }
    s.on_sent(*it, r, 1000);
    sent_us += per;
    n++;
  }
  std::printf("      %u interactive x %u us = %llu us of 15000\n", n, per, (unsigned long long)sent_us);
  check(sent_us <= 15000 && sent_us + per > 15000, "interactive airtime capped at 15 ms per 100 ms");
  check(!s.dequeue(r, 1000).has_value(), "nothing eligible at the same instant (interactive starved, video empty, no bulk)");
  auto again = s.dequeue(r, 1050);
  check(again && again->cls == TxClass::IpInteractive, "50 ms later: 7.5 ms refilled, interactive flows");
  s.on_sent(*again, r, 1050);
  // Bulk goes while interactive is bucket-starved and video is empty.
  TxScheduler s2;
  for (uint32_t i = 0; i < 200; i++) s2.enqueue(item(TxClass::IpInteractive, i, 1000), r, 0);
  s2.enqueue(item(TxClass::IpBulk, 500, 1000), r, 0);
  bool bulk_seen = false;
  for (int i = 0; i < 300; i++) {
    auto it = s2.dequeue(r, 0);
    if (!it) break;
    s2.on_sent(*it, r, 0);
    if (it->cls == TxClass::IpBulk) { bulk_seen = true; break; }
  }
  check(bulk_seen && s2.q_interactive() > 0, "bulk takes the leftover once interactive is starved");
  check(s2.q_ip_air_ms(r) > 0, "q_ip_air_ms counts the queued interactive");
  // Charging is at the CURRENT rung: the same packet costs more at R0.
  check(single_item_airtime_us(kLadder[0], 1000) > 8 * per, "R0 airtime per packet >> R6 (bucket admits fewer)");
}

static void test_video_bound() {
  std::puts("-- video queue bound (60 ms airtime, drop newest enhance first, never IDR while non-IDR exists)");
  TxScheduler s;
  const Rung& r = kLadder[0];
  const uint32_t per_us = video_item_airtime_us(r, 1000);  // 1 source symbol -> 2 frames = 3700 us
  const size_t fit = 60000 / per_us;                        // 16 whole blocks
  std::printf("      block airtime at R0 %u us, %zu fit under 60 ms\n", per_us, fit);
  check(fit == 16, "16 one-symbol blocks fit in 60 ms at R0");
  uint32_t id = 0;
  check(s.enqueue(item(TxClass::Video, id++, 1000, kFlagIdr), r, 0), "IDR accepted");
  for (int i = 0; i < 7; i++) check(s.enqueue(item(TxClass::Video, id++, 1000, 0), r, 0), "base accepted");
  for (int i = 0; i < 8; i++) check(s.enqueue(item(TxClass::Video, id++, 1000, kFlagEnhance), r, 0), "enhance accepted");
  check(s.q_video() == 16 && s.q_video_air_ms(r) == 59, "16 queued, 59 ms");
  // 17th (enhance, newest) -> over the bound, it is the victim.
  check(!s.enqueue(item(TxClass::Video, id++, 1000, kFlagEnhance), r, 0), "17th enhance dropped (newest)");
  {
    auto d = s.drops();
    check(d.video_frames == 1 && d.video_enhance == 1, "drops: 1 frame, 1 enhance");
  }
  // A base frame over the bound evicts the newest queued enhance instead.
  check(s.enqueue(item(TxClass::Video, 100, 1000, 0), r, 0), "base over bound: accepted");
  check(s.q_video() == 16 && s.drops().video_enhance == 2, "newest enhance evicted for it");
  for (uint32_t i = 101; i < 108; i++) s.enqueue(item(TxClass::Video, i, 1000, 0), r, 0);
  check(s.drops().video_enhance == 9 && s.drops().video_frames == 9, "all 8 queued enhance evicted by base frames");
  // No enhance left: a new base frame is the newest non-IDR -> dropped itself.
  check(!s.enqueue(item(TxClass::Video, 200, 1000, 0), r, 0), "base over bound, no enhance: newest non-IDR (itself) dropped");
  check(s.drops().video_frames == 10 && s.drops().video_enhance == 9, "drops 10/9");
  check(s.q_video() == 16 && s.q_video_air_ms(r) <= 60, "queue within bound");
  auto first = s.dequeue(r, 0);
  check(first && (first->flags & kFlagIdr) && first->id == 0, "IDR survived at the head");
  // A new IDR over the bound evicts a non-IDR, never itself while a non-IDR exists.
  check(s.enqueue(item(TxClass::Video, 300, 1000, kFlagIdr), r, 0), "IDR pushed over bound: accepted");
  check(s.enqueue(item(TxClass::Video, 301, 1000, kFlagIdr), r, 0), "second IDR accepted, a base frame goes instead");
  check(s.drops().video_frames == 11, "one base frame evicted for it");
  size_t idr = 0, base = 0;
  for (;;) {
    auto it = s.dequeue(r, 0);
    if (!it) break;
    if (it->flags & kFlagIdr) idr++; else base++;
  }
  check(idr == 2 && base == 14, "drained: 2 IDR + 14 base");
  // All-IDR queue still honours the bound (newest IDR dropped, never the last item).
  TxScheduler s3;
  for (uint32_t i = 0; i < 40; i++) s3.enqueue(item(TxClass::Video, i, 1000, kFlagIdr), r, 0);
  check(s3.q_video() == 16 && s3.q_video_air_ms(r) <= 60 && s3.drops().video_frames == 24, "all-IDR: bounded at 16, 24 dropped");
  // One oversize frame is kept (bound is a queue bound, not a frame limit).
  TxScheduler s4;
  check(s4.enqueue(item(TxClass::Video, 1, 60000, kFlagIdr), r, 0) && s4.q_video_air_ms(r) > 60, "lone 60 kB IDR kept over the bound");
  // Demote inflates the queue: the bound is re-checked at dequeue.
  TxScheduler s5;
  for (uint32_t i = 0; i < 20; i++) s5.enqueue(item(TxClass::Video, i, 1000, i >= 10 ? kFlagEnhance : 0), kLadder[6], 0);
  check(s5.q_video() == 20 && s5.q_video_air_ms(kLadder[6]) < 10, "20 blocks at R6 are < 10 ms, all accepted");
  auto deq = s5.dequeue(r, 0);  // now at R0: 20 x 3.7 ms = 74 ms > 60
  check(deq.has_value() && deq->id == 0 && s5.q_video() == 15, "dequeue at R0: trimmed to 16 then popped the head");
  check(s5.drops().video_frames == 4 && s5.drops().video_enhance == 4, "the 4 evicted were the newest enhance");
  check(s5.q_video_air_ms(r) <= 60, "within bound at R0");
  // Control is never dropped by the video policy.
  TxScheduler s6;
  s6.enqueue(item(TxClass::Control, 1, 8), r, 0);
  for (uint32_t i = 0; i < 40; i++) s6.enqueue(item(TxClass::Video, i, 1000, 0), r, 0);
  check(s6.q_control() == 1 && s6.drops().control == 0, "control untouched by video trimming");
}

static void test_bulk_starves() {
  std::puts("-- bulk starves while video is queued");
  TxScheduler s;
  const Rung& r = kLadder[6];
  for (uint32_t i = 0; i < 5; i++) s.enqueue(item(TxClass::Video, i, 1000), r, 0);
  for (uint32_t i = 10; i < 15; i++) s.enqueue(item(TxClass::IpBulk, i, 1000), r, 0);
  bool ok = true;
  for (int i = 0; i < 5; i++) {
    auto it = s.dequeue(r, 0);
    ok = ok && it && it->cls == TxClass::Video;
    if (it) s.on_sent(*it, r, 0);
  }
  check(ok, "five dequeues: all video");
  auto it = s.dequeue(r, 0);
  check(it && it->cls == TxClass::IpBulk && it->id == 10, "then bulk, FIFO");
  s.enqueue(item(TxClass::Video, 99, 1000), r, 0);
  it = s.dequeue(r, 0);
  check(it && it->cls == TxClass::Video, "new video jumps ahead of the remaining bulk");
}

static void test_queue_limits() {
  std::puts("-- control / IP tail-drop");
  SchedCfg c;
  c.control_max = 2;
  c.ip_max = 2;
  TxScheduler s(c);
  const Rung& r = kLadder[6];
  check(s.enqueue(item(TxClass::Control, 1, 8), r, 0) && s.enqueue(item(TxClass::Control, 2, 8), r, 0), "2 control ok");
  check(!s.enqueue(item(TxClass::Control, 3, 8), r, 0) && s.drops().control == 1, "3rd control dropped, counted");
  check(s.enqueue(item(TxClass::IpBulk, 1, 8), r, 0) && s.enqueue(item(TxClass::IpBulk, 2, 8), r, 0), "2 bulk ok");
  check(!s.enqueue(item(TxClass::IpBulk, 3, 8), r, 0) && s.drops().ip_pkts == 1, "3rd bulk dropped, counted");
  check(s.enqueue(item(TxClass::IpInteractive, 1, 8), r, 0) && s.enqueue(item(TxClass::IpInteractive, 2, 8), r, 0), "interactive has its own 2");
  check(!s.enqueue(item(TxClass::IpInteractive, 3, 8), r, 0) && s.drops().ip_pkts == 2, "3rd interactive dropped");
}

static void test_duty() {
  std::puts("-- tx duty over the window");
  TxScheduler s;
  const Rung& r = kLadder[6];
  const uint32_t a = single_item_airtime_us(r, 1382);  // 194 us
  for (uint32_t i = 0; i < 10; i++) s.on_sent(item(TxClass::Control, i, 1382), r, 1000);
  std::printf("      10 x %u us in 100 ms -> %u permille\n", a, s.tx_duty_permille(1000));
  check(s.tx_duty_permille(1000) == 10 * a / 100, "duty = sum us / window ms");
  check(s.tx_duty_permille(1099) == 10 * a / 100, "still counted at +99 ms");
  check(s.tx_duty_permille(1100) == 0, "out of the window at +100 ms");
  for (uint32_t i = 0; i < 50; i++) s.on_sent(item(TxClass::Video, i, 1000), kLadder[0], 2000 + i);  // 50 x 3700 us
  check(s.tx_duty_permille(2049) == 50 * 3700 / 100, "50 R0 blocks: 1850 permille (over-committed is reported, not clamped)");
  check(s.tx_duty_permille(2149) == 0, "window moved on");
  check(s.tx_duty_permille(500) == 0, "nothing before the first send");
}

int main() {
  test_airtime();
  test_priority();
  test_interactive_bucket();
  test_video_bound();
  test_bulk_starves();
  test_queue_limits();
  test_duty();
  test_budget_fills_duty();
  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
