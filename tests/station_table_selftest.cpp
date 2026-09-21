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


/* ---------------------------------------------------------- decide_forward
 *
 * The relay decision, which four Phase 2b gates depend on and which had NO
 * headless coverage: it lived inline in tests/ap_wpa2.cpp, a file in no test
 * target, so every branch rested on a narrated two-adapter bench run. These
 * cells are what that run cannot be asked to do repeatedly - the refusals in
 * particular, which no station on the bench can even produce, because the AP
 * advertises neither WMM nor HT. */

using devourer::sta::Disposition;
using devourer::sta::decide_forward;

/* Build a to-DS data header: addr1 = BSSID, addr2 = SA, addr3 = DA. */
static void to_ds(uint8_t* h, const uint8_t* bssid, const uint8_t* sa,
                  const uint8_t* da, bool qos = false) {
  std::memset(h, 0, 26);
  h[0] = qos ? 0x88 : 0x08;
  h[1] = devourer::sta::kFcToDs;
  std::memcpy(h + 4, bssid, 6);
  std::memcpy(h + 10, sa, 6);
  std::memcpy(h + 16, da, 6);
}

void test_forward_decision() {
  const uint8_t BSSID[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};
  const uint8_t GRP[6]   = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const uint8_t MCAST[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
  const uint8_t OFF[6]   = {0x02, 0x99, 0x99, 0x99, 0x99, 0x99};
  uint8_t h[32];

  StationTable t;
  t.add(kA);
  t.add(kB);

  /* --- the four destinations ------------------------------------------- */
  to_ds(h, BSSID, kA, BSSID);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::Local,
        "a frame for the AP's own address is Local");

  to_ds(h, BSSID, kA, kB);
  {
    devourer::sta::ForwardDecision d = decide_forward(h, 24, BSSID, t);
    check(d.what == Disposition::Relay, "a frame for another station is Relay");
    check(d.da && std::memcmp(d.da, kB, 6) == 0, "...and da points at addr3");
  }

  to_ds(h, BSSID, kA, OFF);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::OffBss,
        "a frame for an unassociated address is OffBss");

  to_ds(h, BSSID, kA, GRP);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::Group,
        "a broadcast destination is Group");
  to_ds(h, BSSID, kA, MCAST);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::Group,
        "a multicast destination is Group too - the low bit of octet 0, not ff:ff");

  /* A station that deauthenticates stops being a relay target. This is the
   * coupling between the table's lifetime and the forwarding decision, and it
   * is the one a bench run would have to tear down an adapter to show. */
  to_ds(h, BSSID, kA, kB);
  t.remove(kB);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::OffBss,
        "after B deauthenticates, a frame for B is no longer relayable");
  t.add(kB);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::Relay,
        "...and is relayable again once B re-associates");

  /* --- the refusals ----------------------------------------------------- */
  to_ds(h, BSSID, kA, kB);
  h[1] |= devourer::sta::kFcMoreFrag;
  check(decide_forward(h, 24, BSSID, t).what == Disposition::RefuseFragmented,
        "More Fragments is refused rather than forwarded");

  /* THE LAST FRAGMENT, which has More Fragments CLEAR and a nonzero fragment
   * number. This cell set only the bit until 2026-09-21, so the tail of a
   * fragmented MSDU - no LLC/SNAP header, not a whole MSDU - was forwarded as
   * an ordinary frame and the peer received corruption while every counter
   * read success. Fragmentation needs no WMM and no HT; a legacy station with
   * a fragmentation threshold produces this on its own. */
  h[1] &= (uint8_t)~devourer::sta::kFcMoreFrag;
  h[22] = (uint8_t)((h[22] & 0xf0) | 0x03);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::RefuseFragmented,
        "the LAST fragment is refused too, not forwarded as a whole MSDU");
  h[22] &= 0xf0;
  check(decide_forward(h, 24, BSSID, t).what != Disposition::RefuseFragmented,
        "...and an unfragmented frame is still not refused");

  /* The refusal must win over the destination: a FRAGMENTED frame for the AP
   * itself is still not something to hand to a parser expecting a whole MSDU. */
  to_ds(h, BSSID, kA, BSSID);
  h[1] |= devourer::sta::kFcMoreFrag;
  check(decide_forward(h, 24, BSSID, t).what == Disposition::RefuseFragmented,
        "a fragmented frame for the AP is refused, not Local");

  to_ds(h, BSSID, kA, kB, /*qos=*/true);
  h[24] = 0x80;                       /* A-MSDU Present */
  check(decide_forward(h, 26, BSSID, t).what == Disposition::RefuseAmsdu,
        "A-MSDU Present is refused rather than forwarded");

  /* A QoS frame WITHOUT the A-MSDU bit is ordinary traffic. If the mask were
   * wrong - testing the TID nibble, say - this would refuse everything. */
  to_ds(h, BSSID, kA, kB, /*qos=*/true);
  h[24] = 0x05;                       /* TID 5, no A-MSDU */
  check(decide_forward(h, 26, BSSID, t).what == Disposition::Relay,
        "a QoS frame with a TID but no A-MSDU bit still relays");

  /* The A-MSDU bit is read at offset 30 on a four-address frame. Reading it
   * at 24 there would take an address byte for a flags byte. */
  std::memset(h, 0, sizeof h);
  h[0] = 0x88;
  h[1] = devourer::sta::kFcToDs | devourer::sta::kFcFromDs;
  std::memcpy(h + 16, kB, 6);
  h[24] = 0x80;                       /* an ADDRESS byte, not the QoS Control */
  h[30] = 0x00;                       /* the real QoS Control: no A-MSDU */
  check(decide_forward(h, 32, BSSID, t).what == Disposition::Relay,
        "four-address: the A-MSDU bit is read at 30, not 24");
  h[30] = 0x80;
  check(decide_forward(h, 32, BSSID, t).what == Disposition::RefuseAmsdu,
        "four-address: the real A-MSDU bit at 30 is honoured");

  /* --- fails closed on a short header ----------------------------------- */
  to_ds(h, BSSID, kA, kB, /*qos=*/true);
  check(decide_forward(h, 24, BSSID, t).what == Disposition::Malformed,
        "a QoS header declared as 24 bytes is Malformed, not guessed at");

  /* A HEADER TOO SHORT FOR A FRAME CONTROL. decide_forward derived the DS
   * bits and the QoS flag before checking the length, so a caller that got
   * the length wrong caused a one-byte overread. One-byte HEAP buffers,
   * because a short length over a long buffer proves nothing - the read
   * succeeds and the verdict is Malformed either way. The overread is a
   * heap-buffer-overflow the `build-sanitizers` CI job fails on; IN A
   * NON-SANITIZED BUILD THIS ARM CANNOT FAIL, and that is stated rather than
   * implied. */
  {
    std::vector<uint8_t> one(1, 0x88);
    std::vector<uint8_t> none;

    check(decide_forward(one.data(), 1, BSSID, t).what ==
              Disposition::Malformed,
          "a one-byte header is Malformed without being read past");
    check(decide_forward(none.data(), 0, BSSID, t).what ==
              Disposition::Malformed,
          "a zero-length header is Malformed without being read");
  }
  check(decide_forward(h, 24, BSSID, t).da == nullptr,
        "...and hands back no destination to act on");
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
  test_forward_decision();

  if (failures) {
    std::fprintf(stderr, "station_table_selftest: %d failure(s)\n", failures);
    return 1;
  }
  std::printf("station_table_selftest: all checks passed\n");
  return 0;
}
