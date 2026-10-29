// SPDX-License-Identifier: GPL-2.0-or-later
// LinkController + DaMachine: timeline tests against docs/DESIGN.md.
#include <cstdint>
#include <cstdio>
#include "p2p/link_ctl.h"

using p2p::DaMachine;
using p2p::kLadder;
using p2p::LinkCfg;
using p2p::LinkController;
using p2p::LinkReport;
using State = LinkController::State;

static int g_fail = 0;
static void check(bool ok, const char* what, uint64_t t = 0) {
  std::printf("%s  %-70s t=%llu\n", ok ? "ok  " : "FAIL", what, (unsigned long long)t);
  if (!ok) g_fail++;
}

static LinkReport rep(uint32_t epoch, uint32_t uniq, uint16_t loss, int8_t rssi = -60, uint16_t pok = 50,
                      uint16_t plost = 0, uint16_t cok = 100, uint16_t clost = 0) {
  LinkReport r;
  r.epoch = epoch;
  r.uniq = uniq;
  r.loss_permille = loss;
  r.rssi_best = rssi;
  r.rssi_mean = rssi;
  r.probe_rung_ok = pok;
  r.probe_rung_lost = plost;
  r.cur_rung_ok = cok;
  r.cur_rung_lost = clost;
  return r;
}

// Builds an R6 controller fed good reports every 100 ms from t=0 to t=1000
// (the last report, "B"). Returns epoch/uniq counters through refs.
static LinkController warm_r6(uint32_t& ep, uint32_t& uniq, bool probes_from_start = false) {
  LinkCfg cfg;
  cfg.start_rung = 6;
  LinkController lc(cfg, 0);
  ep = 0;
  uniq = 0;
  for (uint64_t t = 0; t <= 1000; t += 100) {
    lc.on_report(rep(++ep, uniq += 100, 0, -60), t);
    if (probes_from_start) lc.on_probe_result(true, t);
    lc.tick(t);
  }
  return lc;
}
static constexpr uint64_t B = 1000;  // time of the last warm-up report

static void test_blind_descent() {
  std::puts("-- (1) blind descent R6 -> floor");
  uint32_t ep, uniq;
  LinkController lc = warm_r6(ep, uniq);
  check(lc.state() == State::Track && lc.rung_phy() == 6 && lc.rung_budget() == 6, "warm: Track R6/R6", B);
  for (uint64_t t = B + 100; t <= B + 2500; t += 100) {
    lc.tick(t);
    if (t == B + 400) check(lc.state() == State::Track && lc.rung_budget() == 6, "B+400: still Track R6", t);
    if (t == B + 500)
      check(lc.state() == State::Blind && lc.rung_budget() == 4 && lc.rung_phy() == 6, "B+500: Blind budget R4 phy R6", t);
    if (t == B + 900) check(lc.rung_phy() == 6 && lc.rung_budget() == 4, "B+900: phy still R6, budget R4", t);
    if (t == B + 1000) check(lc.rung_phy() == 4 && lc.rung_budget() == 2, "B+1000: phy R4 budget R2", t);
    if (t == B + 1500) check(lc.rung_budget() == 0 && lc.rung_phy() == 2, "B+1500: budget R0 (floor clamp) phy R2", t);
    if (t == B + 2000) check(lc.rung_phy() == 0 && lc.state() == State::Floor, "B+2000: phy R0 -> Floor", t);
    if (t == B + 2500) check(lc.rung_phy() == 0 && lc.rung_budget() == 0 && lc.state() == State::Floor, "B+2500: Floor stays", t);
  }
  check(lc.report_age_ms(B + 2500) == 2500, "report_age 2500");
  LinkController fresh(LinkCfg{}, 0);
  check(fresh.report_age_ms(10) == UINT32_MAX, "report_age never = UINT32_MAX");
  check(fresh.state() == State::Track && fresh.rung_phy() == 2, "fresh: Track at start_rung 2");
  fresh.tick(499);
  check(fresh.state() == State::Track, "fresh: still Track at 499 (construction grace)");
  fresh.tick(500);
  check(fresh.state() == State::Blind && fresh.rung_budget() == 0 && fresh.rung_phy() == 2,
        "fresh: Blind at 500, budget floor, phy pending", 500);
}

// Returns the controller in HOLD at R2/R2 at time B+1500 (probes ok from B+600).
static LinkController hold_at_r2(uint32_t& ep, uint32_t& uniq) {
  LinkController lc = warm_r6(ep, uniq);
  for (uint64_t t = B + 100; t <= B + 1500; t += 100) {
    if (t >= B + 600) lc.on_probe_result(true, t);
    lc.tick(t);
    if (t == B + 1000) check(lc.state() == State::Blind && lc.rung_phy() == 4 && lc.rung_budget() == 2, "B+1000: Blind phy R4 budget R2 (known 5)", t);
    if (t == B + 1100)
      check(lc.state() == State::Hold && lc.ack8() == 6 && lc.ack8_known() == 6 && lc.rung_budget() == 2 && lc.rung_phy() == 4,
            "B+1100: Hold at ack8 6/6 known; budget R2, phy R4 pending", t);
  }
  check(lc.state() == State::Hold && lc.rung_phy() == 2 && lc.rung_budget() == 2, "B+1500: Hold, in-flight step completed R2/R2", B + 1500);
  return lc;
}

static void test_hold() {
  std::puts("-- (2) HOLD stops the descent");
  uint32_t ep, uniq;
  LinkController lc = hold_at_r2(ep, uniq);
  for (uint64_t t = B + 1600; t <= B + 11500; t += 100) {
    lc.on_probe_result(true, t);
    lc.tick(t);
  }
  check(lc.state() == State::Hold && lc.rung_phy() == 2 && lc.rung_budget() == 2, "B+11500: still Hold R2/R2, no promotion", B + 11500);
  check(lc.ack8() == 8 && lc.ack8_known() == 8, "ack8 8/8");

  // Probes ACKed all along: Hold at the report timeout, no initial drop.
  LinkController lc2 = warm_r6(ep, uniq, /*probes_from_start=*/true);
  for (uint64_t t = B + 100; t <= B + 3000; t += 100) {
    lc2.on_probe_result(true, t);
    lc2.tick(t);
    if (t == B + 500) check(lc2.state() == State::Hold && lc2.rung_phy() == 6 && lc2.rung_budget() == 6, "B+500: straight to Hold at R6/R6", t);
  }
  check(lc2.state() == State::Hold && lc2.rung_phy() == 6 && lc2.rung_budget() == 6, "B+3000: Hold R6/R6", B + 3000);
}

static void test_hold_to_descend() {
  std::puts("-- (3) HOLD -> descend when probes pin (ack8 <= 2) or go silent");
  uint32_t ep, uniq;
  LinkController lc = hold_at_r2(ep, uniq);
  uint64_t t = B + 1600;
  for (; t <= B + 2000; t += 100) {  // 5 pinned: ack8 8 -> 3, still Hold (hysteresis)
    lc.on_probe_result(false, t);
    lc.tick(t);
  }
  check(lc.ack8() == 3 && lc.state() == State::Hold && lc.rung_phy() == 2, "B+2000: ack8 3, Hold (between 2 and 6)", B + 2000);
  lc.on_probe_result(false, t);  // 6th pinned -> ack8 2
  lc.tick(t);
  check(lc.ack8() == 2 && lc.state() == State::Blind && lc.rung_budget() == 0 && lc.rung_phy() == 2,
        "B+2100: ack8 2 -> Blind, budget floor now, phy R2", t);
  for (t += 100; t <= B + 2600; t += 100) lc.tick(t);
  check(lc.rung_phy() == 0 && lc.state() == State::Floor, "B+2600: phy floor -> Floor", B + 2600);

  LinkController ls = hold_at_r2(ep, uniq);
  for (t = B + 1600; t <= B + 2400; t += 100) ls.tick(t);  // no probes: last at B+1500
  check(ls.state() == State::Hold && ls.rung_phy() == 2, "B+2400: 900 ms silence, still Hold", B + 2400);
  ls.tick(B + 2500);
  check(ls.state() == State::Blind && ls.rung_budget() == 0, "B+2500: 1 s probe silence -> Blind, descending", B + 2500);
}

static void test_exit_to_track() {
  std::puts("-- (4) exit Blind/Hold -> Track on a fresh report, rungs unchanged");
  uint32_t ep, uniq;
  LinkController lc = hold_at_r2(ep, uniq);
  lc.on_report(rep(++ep, uniq += 100, 0), B + 1600);
  check(lc.state() == State::Track && lc.rung_phy() == 2 && lc.rung_budget() == 2, "fresh report: Track, R2/R2 kept", B + 1600);
  lc.on_report(rep(ep, uniq, 0), B + 1700);  // same epoch: not fresh
  lc.on_report(rep(ep - 1, uniq, 0), B + 1700);
  check(lc.report_age_ms(B + 1700) == 100, "stale/duplicate epoch ignored (age 100)");
  // A Floor state exits the same way.
  LinkController lf = warm_r6(ep, uniq);
  for (uint64_t t = B + 100; t <= B + 2500; t += 100) lf.tick(t);
  check(lf.state() == State::Floor, "pre: Floor");
  lf.on_report(rep(++ep, uniq += 100, 0), B + 2600);
  check(lf.state() == State::Track && lf.rung_phy() == 0 && lf.rung_budget() == 0, "Floor -> Track at R0/R0 (no snap-back)", B + 2600);
  // Epoch wrap tolerance.
  LinkController lw(LinkCfg{}, 0);
  lw.on_report(rep(0xFFFFFFF0u, 0, 0), 0);
  lw.on_report(rep(0x00000005u, 100, 0), 100);
  check(lw.report_age_ms(100) == 0, "epoch wrap 0xFFFFFFF0 -> 5 counts as fresh");
}

static void test_acute() {
  std::puts("-- (5) acute loss -> floor (budget now, phy after lead)");
  LinkController lc(LinkCfg{}, 0);
  uint32_t ep = 0, uniq = 0;
  for (uint64_t t = 0; t <= 500; t += 100) { lc.on_report(rep(++ep, uniq += 100, 0), t); lc.tick(t); }
  lc.on_report(rep(++ep, uniq += 100, 300), 600);
  check(lc.state() == State::Track && lc.rung_budget() == 0 && lc.rung_phy() == 2, "600: budget floor now, phy R2", 600);
  for (uint64_t t = 700; t <= 1000; t += 100) lc.tick(t);
  check(lc.rung_phy() == 2, "1000: phy still R2 (lead not elapsed)", 1000);
  lc.tick(1100);
  check(lc.rung_phy() == 0 && lc.rung_budget() == 0 && lc.state() == State::Track, "1100: phy floor, Track", 1100);

  LinkController ln(LinkCfg{}, 0);
  ep = 0; uniq = 0;
  ln.on_report(rep(++ep, uniq += 100, 0), 0);
  ln.on_report(rep(++ep, uniq += 10, 300), 100);  // uniq delta 10 < 32
  check(ln.rung_budget() == 2 && ln.debug().persistent_score == 1, "uniq delta 10: no acute, persistent +1", 100);
}

static void test_persistent_and_lock() {
  std::puts("-- (6) persistent loss -> one demote, vacated rung locked 30 s");
  LinkController lc(LinkCfg{}, 0);
  uint32_t ep = 0, uniq = 0;
  uint64_t t = 0;
  for (int i = 0; i < 4; i++, t += 100) { lc.on_report(rep(++ep, uniq += 100, 60), t); lc.tick(t); }
  check(lc.rung_phy() == 2 && lc.rung_budget() == 2 && lc.debug().persistent_score == 4, "4 reports at 60: score 4, no demote", 300);
  lc.on_report(rep(++ep, uniq += 100, 60), t);  // t = 400, 5th
  check(lc.rung_budget() == 1 && lc.rung_phy() == 2 && lc.debug().persistent_score == 0, "5th: budget R1 now, phy R2, score reset", t);
  check(lc.debug().rung_locked_until[2] == 400 + 30000, "rung 2 locked until 30400");
  for (t += 100; t <= 900; t += 100) lc.tick(t);
  check(lc.rung_phy() == 1, "900: phy R1", 900);
  // Perfect reports with every promote condition met except the lock.
  bool promoted_early = false;
  for (t = 1000; t < 30400; t += 100) {
    lc.on_report(rep(++ep, uniq += 100, 0, -60), t);
    lc.tick(t);
    if (lc.rung_phy() != 1) promoted_early = true;
  }
  check(!promoted_early && lc.rung_phy() == 1 && lc.rung_budget() == 1, "no promote into locked R2 before 30400", 30300);
  lc.on_report(rep(++ep, uniq += 100, 0, -60), 30400);
  check(lc.rung_phy() == 2 && lc.rung_budget() == 1, "30400: lock expired -> phy R2, budget lags", 30400);
  lc.tick(30900);
  check(lc.rung_budget() == 2, "30900: budget R2", 30900);
  // Clean reports leak the score down.
  LinkController ld(LinkCfg{}, 0);
  ep = 0; uniq = 0;
  ld.on_report(rep(++ep, uniq += 100, 60), 0);
  ld.on_report(rep(++ep, uniq += 100, 60), 100);
  ld.on_report(rep(++ep, uniq += 100, 0), 200);
  check(ld.debug().persistent_score == 1, "+1 +1 -1 = 1");
}

static void run_promote(const char* label, int8_t rssi, uint16_t pok, uint16_t plost, uint16_t cok, uint16_t clost,
                        bool expect_promote) {
  LinkController lc(LinkCfg{}, 0);
  uint32_t ep = 0, uniq = 0;
  uint64_t first_change = 0;
  for (uint64_t t = 0; t <= 10000; t += 100) {
    lc.on_report(rep(++ep, uniq += 100, 0, rssi, pok, plost, cok, clost), t);
    lc.tick(t);
    if (!first_change && lc.rung_phy() != 2) first_change = t;
    if (expect_promote) {
      if (t == 4900) check(lc.rung_phy() == 2, "4900: dwell not met, still R2", t);
      if (t == 5000) check(lc.rung_phy() == 3 && lc.rung_budget() == 2, "5000: promote phy R3 now, budget R2", t);
      if (t == 5400) check(lc.rung_budget() == 2, "5400: budget still R2", t);
      if (t == 5500) check(lc.rung_budget() == 3, "5500: budget R3 (lag)", t);
      if (t == 9900) check(lc.rung_phy() == 3, "9900: no skip, still R3", t);
      if (t == 10000) check(lc.rung_phy() == 4 && lc.rung_budget() == 3, "10000: second promote R4 after new dwell", t);
    }
  }
  if (!expect_promote) check(first_change == 0 && lc.rung_phy() == 2, label, 10000);
}

static void test_promote() {
  std::puts("-- (7) promote: 10 good reports + rssi margin + probe evidence + 5 s dwell");
  run_promote("promote", -70, 50, 0, 100, 0, true);
  run_promote("no probe evidence (0 frames at probe rung) -> no promote", -70, 0, 0, 100, 0, false);
  run_promote("rssi -80 < -84+6 -> no promote", -80, 50, 0, 100, 0, false);
  run_promote("probe PER 0.5 > 2 x 0.01 -> no promote", -70, 10, 10, 99, 1, false);
  run_promote("probe PER 0.02 <= 2 x 0.01 -> promote", -70, 98, 2, 99, 1, true);
  run_promote("cur PER 0 and probe lost 1 -> no promote", -70, 99, 1, 100, 0, false);
  // A 9-good streak broken by one 30-permille report restarts the count.
  LinkController lc(LinkCfg{}, 0);
  uint32_t ep = 0, uniq = 0;
  for (uint64_t t = 0; t <= 6000; t += 100) {
    const uint16_t loss = (t % 1000 == 900) ? 30 : 0;
    lc.on_report(rep(++ep, uniq += 100, loss, -70), t);
    lc.tick(t);
  }
  check(lc.rung_phy() == 2, "streak broken every 10th report: never 10 consecutive -> R2", 6000);
}

static void test_local() {
  std::puts("-- (8) local q_air / tx_timeout -> immediate demote");
  LinkController lc(LinkCfg{}, 0);
  lc.on_local(60, false, 100);
  check(lc.rung_phy() == 2, "q_air 60 (not >60): no demote", 100);
  lc.on_local(70, false, 100);
  check(lc.rung_phy() == 1 && lc.rung_budget() == 1 && lc.state() == State::Track, "q_air 70: R1/R1 at once", 100);
  lc.on_local(0, true, 200);
  check(lc.rung_phy() == 1, "tx_timeout 100 ms later: within gap, no second demote", 200);
  lc.on_local(0, true, 600);
  check(lc.rung_phy() == 0 && lc.rung_budget() == 0, "tx_timeout at 600: R0/R0", 600);
  lc.on_local(500, true, 1200);
  check(lc.rung_phy() == 0 && lc.state() == State::Track, "at floor: stays R0, Track (reports grace)", 1200);
}

static void test_tune20() {
  std::puts("-- (9) wants_tune_20mhz");
  LinkCfg c3; c3.start_rung = 3;
  LinkController l3(c3, 0);
  check(!l3.wants_tune_20mhz(2999), "R3 at 2999: false");
  check(l3.wants_tune_20mhz(3000), "R3 at 3000: true");
  LinkCfg c4; c4.start_rung = 4;
  LinkController l4(c4, 0);
  check(!l4.wants_tune_20mhz(100000), "R4: false");
  l4.on_local(70, false, 100);
  check(l4.rung_phy() == 3 && !l4.wants_tune_20mhz(3099), "R4->R3 at 100: false at 3099");
  check(l4.wants_tune_20mhz(3100), "true at 3100");
  // Promote back to R4 clears it at once.
  LinkController lp(c3, 0);
  uint32_t ep = 0, uniq = 0;
  for (uint64_t t = 0; t <= 5000; t += 100) { lp.on_report(rep(++ep, uniq += 100, 0, -70), t); lp.tick(t); }
  check(lp.rung_phy() == 4 && !lp.wants_tune_20mhz(5000), "promoted to R4 at 5000: false immediately", 5000);
}

static void test_da_machine() {
  std::puts("-- (10) DaMachine");
  DaMachine da;
  check(da.mode() == DaMachine::Mode::Bcast, "starts Bcast");
  check(!da.allow_unicast(0, 8), "Bcast: no unicast even for ctl");
  da.update(8, 8, State::Track, 0);
  da.update(8, 8, State::Track, 999);
  check(da.mode() == DaMachine::Mode::Bcast, "ack8 8 held 999 ms: still Bcast", 999);
  da.update(8, 8, State::Track, 1000);
  check(da.mode() == DaMachine::Mode::Ucast, "ack8 8 held 1000 ms in Track: Ucast", 1000);
  check(da.allow_unicast(0, 7) && da.allow_unicast(1, 7), "Ucast: ctl + interactive unicast");
  check(!da.allow_unicast(2, 7) && da.allow_unicast(2, 8), "bulk unicast only at ack8 == 8");
  // Min dwell: pinned x2 at 1100 does not flip; the live condition flips at 2000.
  da.on_probe_result(false, 1100);
  da.on_probe_result(false, 1150);
  check(da.mode() == DaMachine::Mode::Ucast, "2 pinned within dwell: still Ucast", 1150);
  da.update(6, 8, State::Track, 1999);
  check(da.mode() == DaMachine::Mode::Ucast, "1999: dwell not over", 1999);
  da.update(6, 8, State::Track, 2000);
  check(da.mode() == DaMachine::Mode::Bcast, "2000: dwell over, pinned 2 -> Bcast", 2000);
  // Ucast -> Bcast on 2 pinned, after dwell.
  DaMachine db;
  db.update(8, 8, State::Track, 0);
  db.update(8, 8, State::Track, 1000);
  check(db.mode() == DaMachine::Mode::Ucast, "db Ucast at 1000", 1000);
  db.on_probe_result(false, 2500);
  check(db.mode() == DaMachine::Mode::Ucast, "1 pinned: Ucast", 2500);
  db.on_probe_result(false, 2600);
  check(db.mode() == DaMachine::Mode::Bcast, "2 consecutive pinned: Bcast", 2600);
  // An ok between pinned resets the run.
  DaMachine dc;
  dc.update(8, 8, State::Track, 0);
  dc.update(8, 8, State::Track, 1000);
  dc.on_probe_result(false, 2500);
  dc.on_probe_result(true, 2600);
  dc.on_probe_result(false, 2700);
  check(dc.mode() == DaMachine::Mode::Ucast, "pinned, ok, pinned: not consecutive -> Ucast", 2700);
  dc.update(5, 8, State::Track, 2800);
  check(dc.mode() == DaMachine::Mode::Bcast, "ack8 5 -> Bcast", 2800);
  // Not Ucast outside Track.
  DaMachine dd;
  for (uint64_t t = 0; t <= 3000; t += 100) dd.update(8, 8, State::Hold, t);
  check(dd.mode() == DaMachine::Mode::Bcast, "ack8 8 for 3 s in Hold: Bcast", 3000);
  dd.update(8, 8, State::Track, 3100);
  dd.update(8, 8, State::Track, 4099);
  check(dd.mode() == DaMachine::Mode::Bcast, "hold restarts in Track: 999 ms -> Bcast", 4099);
  dd.update(8, 8, State::Track, 4100);
  check(dd.mode() == DaMachine::Mode::Ucast, "4100: Ucast", 4100);
  dd.update(8, 8, State::Blind, 6000);
  check(dd.mode() == DaMachine::Mode::Bcast, "state Blind -> Bcast", 6000);
  // Min dwell on the way up: Bcast at 6000 -> condition held from 6100, flip at >= 7100 (hold) and >= 7000 (dwell).
  dd.update(8, 8, State::Track, 6100);
  dd.update(8, 8, State::Track, 7099);
  check(dd.mode() == DaMachine::Mode::Bcast, "7099: hold 999 -> Bcast", 7099);
  dd.update(8, 8, State::Track, 7100);
  check(dd.mode() == DaMachine::Mode::Ucast, "7100: Ucast", 7100);

  // fps-independence: controller + DA driven by probes only (no IP traffic).
  std::puts("-- (10b) fps independence: peer vanishes, Bcast within ~1 s on probes alone");
  LinkController lc(LinkCfg{}, 0);
  DaMachine de;
  uint32_t ep = 0, uniq = 0;
  uint64_t t = 0;
  for (; t < 5000; t += 100) {
    lc.on_report(rep(++ep, uniq += 100, 0, -60, 0, 0), t);  // no probe evidence: no promote noise
    lc.on_probe_result(true, t);
    lc.tick(t);
    de.on_probe_result(true, t);
    de.update(lc.ack8(), lc.ack8_known(), lc.state(), t);
  }
  check(de.mode() == DaMachine::Mode::Ucast && lc.state() == State::Track, "4900: Ucast in Track", 4900);
  const uint64_t vanish = t;  // 5000: no more reports, probes pin
  uint64_t flipped_at = 0;
  for (; t <= vanish + 2000; t += 100) {
    lc.on_probe_result(false, t);
    lc.tick(t);
    de.on_probe_result(false, t);
    de.update(lc.ack8(), lc.ack8_known(), lc.state(), t);
    if (!flipped_at && de.mode() == DaMachine::Mode::Bcast) flipped_at = t;
  }
  std::printf("      peer vanished at %llu, Bcast at %llu\n", (unsigned long long)vanish, (unsigned long long)flipped_at);
  check(flipped_at != 0 && flipped_at - vanish <= 1000, "Bcast within 1 s of the peer vanishing (no IP traffic)");
  check(flipped_at == vanish + 100, "flipped on the 2nd pinned probe (vanish + 100)");
  check(lc.state() == State::Floor || lc.state() == State::Blind, "controller descended (Blind/Floor)");
}

int main() {
  test_blind_descent();
  test_hold();
  test_hold_to_descend();
  test_exit_to_track();
  test_acute();
  test_persistent_and_lock();
  test_promote();
  test_local();
  test_tune20();
  test_da_machine();
  std::printf("%s: %d failure(s)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
