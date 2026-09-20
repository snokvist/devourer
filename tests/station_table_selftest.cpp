/* Headless guard for src/sta/StationTable.h — Phase 2b.2.
 *
 * The gate for this phase item is "a two-station fixture the old single-g_sta
 * code cannot satisfy", and test_two_stations_are_independent() below is
 * literally that: two associated stations whose PTKs, TX PN spaces and CCMP
 * receive windows do not touch each other. Run any of it against the
 * file-scope `g_sta`/`g_ptk`/`g_txpn`/`g_ccmp_replay` shape in
 * tests/ap_wpa2.cpp and it fails, because there is only one of each.
 *
 * What this does NOT cover: anything on air. A table that holds two stations
 * correctly says nothing about whether the AP can serve two stations — that
 * needs the association path, per-station encrypt/decrypt and the relay, which
 * are Phase 2b.3 onward. This file pins the container's contract only.
 */
#include <cstdio>
#include <cstring>

#include "sta/StationTable.h"

static int failures = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    failures++;
  }
}

namespace {

using devourer::sta::HsState;
using devourer::sta::Station;
using devourer::sta::StationTable;

const uint8_t kA[6] = {0x02, 0xaa, 0x00, 0x00, 0x00, 0x01};
const uint8_t kB[6] = {0x02, 0xaa, 0x00, 0x00, 0x00, 0x02};
const uint8_t kC[6] = {0x02, 0xaa, 0x00, 0x00, 0x00, 0x03};
const uint8_t kGroup[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
const uint8_t kZero[6] = {0, 0, 0, 0, 0, 0};

void test_empty() {
  StationTable t;
  check(t.count() == 0, "a fresh table is empty");
  check(t.find(kA) == nullptr, "find on an empty table returns nullptr");
  check(!t.remove(kA), "removing from an empty table reports no removal");
  check(t.at(0) == nullptr, "at() on an empty table returns nullptr");
}

void test_add_find_remove() {
  StationTable t;
  Station* a = t.add(kA);
  check(a != nullptr, "a station can be added");
  check(t.count() == 1, "adding one station gives count 1");
  check(t.find(kA) == a, "find returns the record that add returned");
  check(t.find(kB) == nullptr, "find on an absent address returns nullptr");
  check(a->state == HsState::Idle, "a new station starts Idle");
  check(!a->keyed(), "a new station is not keyed");

  check(t.remove(kA), "removing a present station reports a removal");
  check(t.count() == 0, "the table is empty again");
  check(!t.remove(kA), "a duplicate deauth reports no removal");
}

/* A re-association must not consume a second AID or orphan the first record.
 * A station that loses its way through the four-way will re-send an
 * association request, so this is the common case, not the odd one. */
void test_reassociation_is_idempotent() {
  StationTable t;
  Station* first = t.add(kA);
  first->state = HsState::Done;
  Station* again = t.add(kA);
  check(again == first, "re-adding the same address returns the same record");
  check(t.count() == 1, "re-association does not add a second record");
  check(again->aid == first->aid, "re-association keeps the same AID");
}

void test_aid_allocation() {
  StationTable t;
  check(t.add(kA)->aid == 1, "the first station gets AID 1");
  check(t.add(kB)->aid == 2, "the second station gets AID 2");
  check(t.add(kC)->aid == 3, "the third station gets AID 3");

  /* Lowest free, not next highest: a churning table must not walk its AIDs
   * up out of the one-byte TIM bitmap's 1..7 range. */
  t.remove(kB);
  const uint8_t kD[6] = {0x02, 0xaa, 0x00, 0x00, 0x00, 0x04};
  check(t.add(kD)->aid == 2, "a freed AID is reused before a higher one");
}

void test_capacity() {
  StationTable t;
  uint8_t addr[6] = {0x02, 0xbb, 0x00, 0x00, 0x00, 0x00};
  for (int i = 0; i < StationTable::kMaxStations; i++) {
    addr[5] = (uint8_t)(i + 1);
    check(t.add(addr) != nullptr, "every station up to capacity is accepted");
  }
  check(t.count() == StationTable::kMaxStations, "the table fills to capacity");
  addr[5] = 0xfe;
  check(t.add(addr) == nullptr, "a station past capacity is refused");
  check(t.count() == StationTable::kMaxStations, "a refused add changes nothing");

  /* Every AID handed out must be inside the TIM bitmap's range, or the cap
   * is not doing the job its comment claims. */
  for (int i = 0; i < StationTable::capacity(); i++) {
    Station* s = t.at(i);
    check(s != nullptr && s->aid >= 1 && s->aid <= 7,
          "every AID is within the one-byte TIM bitmap's 1..7");
  }
}

void test_malformed_addresses_refused() {
  StationTable t;
  check(t.add(nullptr) == nullptr, "a null address is refused");
  check(t.add(kGroup) == nullptr, "a group address is never associated");
  check(t.find(kGroup) == nullptr, "a group address never matches");
  /* An all-zero address must not match a slot that has not been used. */
  check(t.find(kZero) == nullptr, "the all-zero address matches no free slot");
  check(t.count() == 0, "no refused address was stored");
}

/* THE GATE FOR THIS PHASE ITEM.
 *
 * Two stations, each with its own key material, its own TX PN space and its
 * own CCMP receive window. A single-station implementation fails every one of
 * these: the second association overwrites the first's PTK, the shared PN
 * counter makes one station's traffic advance the other's, and the shared
 * replay window makes station B's perfectly ordinary PN 1..5 look like a
 * replay of station A's. */
void test_two_stations_are_independent() {
  StationTable t;
  Station* a = t.add(kA);
  Station* b = t.add(kB);
  check(a != nullptr && b != nullptr && a != b, "two stations get two records");

  std::memset(a->ptk, 0xA5, sizeof a->ptk);
  std::memset(b->ptk, 0x5A, sizeof b->ptk);
  a->state = HsState::Done;
  b->state = HsState::WaitMsg2;

  check(std::memcmp(a->ptk, b->ptk, 48) != 0,
        "the two stations hold different PTKs");
  check(a->ptk[0] == 0xA5 && b->ptk[0] == 0x5A,
        "neither station's key material overwrote the other's");
  check(a->keyed() && !b->keyed(),
        "handshake state is per-station, not shared");

  /* TX PN spaces are independent, and both start at 1 - never 0, which
   * CcmpReplay refuses outright. */
  check(a->tx_pn == 1 && b->tx_pn == 1, "each station's TX PN starts at 1");
  for (int i = 0; i < 10; i++) a->tx_pn++;
  check(a->tx_pn == 11 && b->tx_pn == 1,
        "one station's transmissions do not advance the other's PN");

  /* The receive windows are independent. This is the one that bites hardest
   * in a shared implementation: B's first five frames carry PN 1..5, exactly
   * like A's, and a shared window rejects all of them as replays. */
  for (uint64_t pn = 1; pn <= 5; pn++)
    check(a->rx_replay.accept(pn, 0), "station A's PNs are accepted");
  for (uint64_t pn = 1; pn <= 5; pn++)
    check(b->rx_replay.accept(pn, 0),
          "station B's identical PNs are accepted, because its window is its own");

  /* And each window still does its own job: a genuine replay is refused. */
  check(!a->rx_replay.accept(3, 0), "station A still rejects its own replay");
  check(!b->rx_replay.accept(3, 0), "station B still rejects its own replay");
}

/* A freed slot must not hand the next station the previous one's key
 * material.
 *
 * READ THE LIMIT BEFORE TRUSTING THIS. The table wipes in BOTH remove() and
 * add(), and those two wipes are redundant: every unused slot is already
 * zeroed, because remove() and clear() are the only ways a slot becomes
 * unused and both zero it. So this cell catches "nothing wipes" but NOT
 * "one of the two wipes was deleted" - a mutation removing add()'s memset
 * SURVIVES it, verified 2026-09-20.
 *
 * The redundancy is kept deliberately rather than trimmed. This is key
 * material, the carve-out in the minimal-implementation rule covers exactly
 * this, and a future change to remove() that marks a slot unused without
 * zeroing it would otherwise leak a PTK into the next station. The honest
 * position is that the redundancy costs one untestable mutation and buys a
 * failure mode that cannot happen; the cost is recorded here rather than
 * papered over. */
void test_removal_wipes_key_material() {
  StationTable t;
  Station* a = t.add(kA);
  std::memset(a->ptk, 0xA5, sizeof a->ptk);
  std::memset(a->anonce, 0xA5, sizeof a->anonce);
  a->state = HsState::Done;
  a->tx_pn = 9999;
  a->rx_replay.accept(500, 0);
  t.remove(kA);

  /* The next station lands in the slot that was just freed. */
  Station* b = t.add(kB);
  check(b != nullptr, "a station can take a freed slot");
  uint8_t zeros[48] = {0};
  check(std::memcmp(b->ptk, zeros, 48) == 0,
        "a reused slot carries no previous PTK");
  check(std::memcmp(b->anonce, zeros, 32) == 0,
        "a reused slot carries no previous ANonce");
  check(b->state == HsState::Idle, "a reused slot starts Idle");
  check(b->tx_pn == 1, "a reused slot's TX PN restarts at 1");
  check(b->rx_replay.accept(1, 0),
        "a reused slot's replay window does not reject from the old high-water mark");
}

/* Churn: the failure mode an adversarial review found in the AP harness, which
 * had no deauth handler at all, so StationTable::remove() had no caller outside
 * this file and the table only ever grew. Seven distinct addresses filled it
 * permanently. Android randomises its MAC per network by default, so that is
 * ordinary client behaviour rather than an attack.
 *
 * The table itself was never the defect - this cell proves the container
 * supports indefinite churn - but a container whose free path is never called
 * is a leak, so the property is pinned here and the caller was added. */
void test_churn_does_not_exhaust_the_table() {
  StationTable t;
  uint8_t addr[6] = {0x02, 0xcc, 0x00, 0x00, 0x00, 0x00};

  /* Fill it. */
  for (int i = 0; i < StationTable::kMaxStations; i++) {
    addr[5] = (uint8_t)(i + 1);
    check(t.add(addr) != nullptr, "the table fills");
  }

  /* Now churn far past capacity: each new address only fits because the
   * previous one was freed. Forty rounds through a seven-slot table. */
  for (int round = 0; round < 40; round++) {
    /* Free whatever currently holds the lowest slot, then admit a new MAC. */
    Station* victim = t.at(0);
    check(victim != nullptr, "a full table has an occupant to evict");
    /* check() reports and carries on, so this has to return rather than
     * dereference - a regression in the fill above would otherwise segfault
     * the suite instead of failing it. */
    if (!victim) return;
    uint8_t v[6];
    std::memcpy(v, victim->addr, 6);
    check(t.remove(v), "the occupant is removed");

    uint8_t fresh[6] = {0x02, 0xdd, 0x00, 0x00,
                        (uint8_t)(round >> 8), (uint8_t)(round & 0xff)};
    Station* s = t.add(fresh);
    check(s != nullptr, "a fresh address is admitted after an eviction");
    if (!s) return;
    check(s->aid >= 1 && s->aid <= 7, "its AID stays inside the TIM range");
    check(s->tx_pn == 1, "its PN space starts fresh");
  }
  check(t.count() == StationTable::kMaxStations,
        "the table is still exactly full after 40 rounds of churn");
}

/* WaitMsg4 must count as keyed: a station that received msg3 installs its
 * keys and can send protected data before its msg4 arrives. Accepting only at
 * Done drops those frames. */
void test_keyed_accepts_wait_msg4() {
  StationTable t;
  Station* a = t.add(kA);
  a->state = HsState::Idle;     check(!a->keyed(), "Idle is not keyed");
  a->state = HsState::WaitMsg2; check(!a->keyed(), "WaitMsg2 is not keyed");
  a->state = HsState::WaitMsg4; check(a->keyed(), "WaitMsg4 IS keyed");
  a->state = HsState::Done;     check(a->keyed(), "Done is keyed");
}

} // namespace

int main() {
  test_empty();
  test_add_find_remove();
  test_reassociation_is_idempotent();
  test_aid_allocation();
  test_capacity();
  test_malformed_addresses_refused();
  test_two_stations_are_independent();
  test_removal_wipes_key_material();
  test_churn_does_not_exhaust_the_table();
  test_keyed_accepts_wait_msg4();

  if (failures) {
    std::fprintf(stderr, "station_table_selftest: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("station_table_selftest: all checks passed\n");
  return 0;
}
