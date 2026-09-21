/* Headless guard for src/sta/BssTable.h — what a scan found, and which of it
 * is worth joining.
 *
 * Pure and header-only like the rest of src/sta/, so this needs neither
 * OpenSSL nor libusb and runs in every CI job.
 *
 * The load-bearing cells are the two that decide whether a station can
 * connect at all: `select()` must pick a BSS this station can actually finish
 * a handshake with, and `observe()` must not invent one out of a frame that
 * is not a beacon. Both were written as assertions about behaviour a caller
 * depends on, not about the shape of the struct.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sta/BssTable.h"

namespace {

using devourer::sta::BssEntry;
using devourer::sta::BssTable;

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

/* A beacon as a real AP airs one: the 24-byte header, the 12-byte fixed body,
 * then the elements in the standard's order. Built with the same helpers the
 * AP harnesses use, so a change to those shows up here. */
std::vector<uint8_t> beacon(const uint8_t bssid[6], const std::string& ssid,
                            uint8_t chan, bool rsn, bool privacy = true) {
  static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  std::vector<uint8_t> m = devourer::sta::mgmt_hdr(devourer::sta::kFcBeacon,
                                                   bcast, bssid, bssid);

  m.insert(m.end(), 8, 0);                       /* timestamp */
  devourer::sta::put_le16(m, 100);               /* beacon interval, TU */
  devourer::sta::put_le16(m,
                          (uint16_t)(0x0001 | (privacy && rsn ? 0x0010 : 0)));
  devourer::sta::append_ssid(m, ssid);
  devourer::sta::append_supported_rates(m);
  devourer::sta::append_ds_params(m, chan);
  if (rsn) devourer::sta::append_rsn_ccmp_psk(m);
  return m;
}

void test_observe_and_dedupe() {
  BssTable t;
  const uint8_t a[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t b[6] = {0x02, 0, 0, 0, 0, 0x02};

  check(t.count() == 0, "a fresh table is empty");

  std::vector<uint8_t> f = beacon(a, "one", 6, true);
  const BssEntry* e = t.observe(f.data(), f.size(), -40, 1000);
  check(e != nullptr, "a beacon is observed");
  check(t.count() == 1, "...as one entry");
  check(e->info.ssid == "one" && e->info.channel == 6, "...parsed");
  check(e->info.rsn_ccmp_psk, "...and recognised as WPA2-PSK/CCMP");
  check(e->rssi == -40 && e->last_seen_ms == 1000 && e->frames == 1,
        "...with its RSSI, timestamp and frame count");

  /* THE SAME BSS AGAIN IS NOT A SECOND ENTRY. Without this a station hears a
   * beacon ten times a second and fills a sixteen-slot table in under two
   * seconds, then evicts the network it was looking for. */
  e = t.observe(f.data(), f.size(), -55, 1100);
  check(t.count() == 1, "the same BSSID does not create a second entry");
  check(e->rssi == -55 && e->last_seen_ms == 1100 && e->frames == 2,
        "...it refreshes the existing one");

  std::vector<uint8_t> g = beacon(b, "two", 11, true);
  t.observe(g.data(), g.size(), -70, 1200);
  check(t.count() == 2, "a different BSSID is a second entry");
  check(t.find(a) != nullptr && t.find(b) != nullptr, "both are findable");
  const uint8_t absent[6] = {0x02, 0, 0, 0, 0, 0x09};
  check(t.find(absent) == nullptr, "an unheard BSSID is not");
}

/* A frame that is not a beacon or probe response must be refused. Every
 * management frame shares the same 24-byte header, so handing a deauth to
 * parse_beacon reads its reason code as part of a timestamp and invents a
 * BSS. */
void test_only_beacons_are_observed() {
  BssTable t;
  const uint8_t a[6] = {0x02, 0, 0, 0, 0, 0x01};

  std::vector<uint8_t> d =
      devourer::sta::build_deauth(a, a, 7);
  check(t.observe(d.data(), d.size(), -40, 1) == nullptr,
        "a deauth is not observed as a BSS");

  std::vector<uint8_t> f = beacon(a, "one", 6, true);
  f[0] = devourer::sta::kFcAssocResp;
  check(t.observe(f.data(), f.size(), -40, 1) == nullptr,
        "an association response is not observed as a BSS");

  f[0] = devourer::sta::kFcProbeResp;
  check(t.observe(f.data(), f.size(), -40, 1) != nullptr,
        "a probe response IS observed - same body layout as a beacon");
  check(t.count() == 1, "...into one entry");

  /* A runt. parse_beacon refuses anything shorter than its fixed part, and
   * observe must not reach it with a 24-byte buffer either. */
  check(t.observe(f.data(), 24, -40, 1) == nullptr, "a runt is refused");
  check(t.observe(f.data(), 10, -40, 1) == nullptr, "a 10-byte frame is refused");
  check(t.observe(nullptr, 100, -40, 1) == nullptr, "null is refused");
}

void test_expire() {
  BssTable t;
  const uint8_t a[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t b[6] = {0x02, 0, 0, 0, 0, 0x02};

  std::vector<uint8_t> f = beacon(a, "one", 6, true);
  std::vector<uint8_t> g = beacon(b, "two", 6, true);
  t.observe(f.data(), f.size(), -40, 1000);
  t.observe(g.data(), g.size(), -40, 5000);

  check(t.expire(5100, 10000) == 0, "nothing expires before its age");
  check(t.count() == 2, "...and the table is unchanged");
  check(t.expire(6000, 2000) == 1, "the older entry expires");
  check(t.count() == 1 && t.find(a) == nullptr && t.find(b) != nullptr,
        "...and it is the right one");

  /* A station that keeps a BSS forever will try to associate with one that
   * went off the air ten minutes ago and then report the AP as broken. */
  check(t.expire(100000, 2000) == 1, "everything stale expires");
  check(t.count() == 0, "...leaving nothing");

  /* THE CLOCK WRAPS. `now_ms` is a uint32_t, so about every fifty days it
   * passes zero, and the subtraction has to be unsigned.
   *
   * A SMALL AGE ACROSS THE WRAP DOES NOT DISTINGUISH THE TWO: 0x00001000 -
   * 0xfffff000 is 0x2000 either way, positive in both readings. The first
   * version of this cell tested exactly that and a mutation to int32_t
   * survived it. The difference only appears once the age passes 2^31 ms -
   * about twenty-five days - where the signed reading goes NEGATIVE and the
   * entry never expires again. */
  t.observe(f.data(), f.size(), -40, 0xfffff000u);
  check(t.count() == 1, "an entry seen just before the wrap");
  check(t.expire(0x00001000u, 2000) == 1, "expires just after the wrap");

  t.observe(f.data(), f.size(), -40, 0);
  check(t.expire(0x90000000u, 2000) == 1,
        "an entry twenty-eight days old expires - the age must be UNSIGNED, "
        "or it reads as negative and nothing is ever dropped again");
  check(t.count() == 0, "...leaving the table empty");
}

void test_select() {
  BssTable t;
  const uint8_t weak[6] = {0x02, 0, 0, 0, 0, 0x01};
  const uint8_t strong[6] = {0x02, 0, 0, 0, 0, 0x02};
  const uint8_t open_bss[6] = {0x02, 0, 0, 0, 0, 0x03};
  const uint8_t other[6] = {0x02, 0, 0, 0, 0, 0x04};

  std::vector<uint8_t> f1 = beacon(weak, "net", 6, true);
  std::vector<uint8_t> f2 = beacon(strong, "net", 36, true);
  std::vector<uint8_t> f3 = beacon(open_bss, "net", 1, false, false);
  std::vector<uint8_t> f4 = beacon(other, "elsewhere", 6, true);
  t.observe(f1.data(), f1.size(), -80, 1000);
  t.observe(f2.data(), f2.size(), -35, 1000);
  t.observe(f3.data(), f3.size(), -20, 1000);   /* strongest, and unusable */
  t.observe(f4.data(), f4.size(), -10, 1000);   /* strongest, wrong SSID */

  const BssEntry* s = t.select("net");
  check(s != nullptr, "a candidate is found");
  check(s && std::memcmp(s->info.bssid, strong, 6) == 0,
        "the STRONGEST joinable BSS wins");
  check(s && s->info.channel == 36, "...and carries its channel");

  /* THE THREE NEGATIVE ARMS, each of which the positive one would hide. */
  check(t.select("absent") == nullptr, "an SSID nobody airs has no candidate");
  const BssEntry* o = t.find(open_bss);
  check(o && !o->info.rsn_ccmp_psk,
        "the open BSS is in the table but not joinable");
  check(s && std::memcmp(s->info.bssid, open_bss, 6) != 0,
        "...so it is not selected despite the strongest signal");
  check(s && std::memcmp(s->info.bssid, other, 6) != 0,
        "a stronger BSS with another SSID is not selected");

  /* A tie breaks on the most recently heard, so a stale entry never beats a
   * live one at equal signal. */
  BssTable u;
  const uint8_t p[6] = {0x02, 0, 0, 0, 0, 0x0a};
  const uint8_t q[6] = {0x02, 0, 0, 0, 0, 0x0b};
  std::vector<uint8_t> g1 = beacon(p, "tie", 6, true);
  std::vector<uint8_t> g2 = beacon(q, "tie", 6, true);
  u.observe(g1.data(), g1.size(), -50, 1000);
  u.observe(g2.data(), g2.size(), -50, 2000);
  const BssEntry* w = u.select("tie");
  check(w && std::memcmp(w->info.bssid, q, 6) == 0,
        "at equal signal the fresher entry wins");
}

/* 802.11w is not implemented here. A BSS that REQUIRES management-frame
 * protection will accept the authentication and then refuse the association,
 * which is a confusing place to fail; skipping it in selection turns that
 * into an honest "no candidate". */
void test_mfp_required_is_skipped() {
  BssTable t;
  const uint8_t a[6] = {0x02, 0, 0, 0, 0, 0x01};
  std::vector<uint8_t> f = beacon(a, "net", 6, true);

  /* Set RSN capabilities bit 6 (MFPR) in the element the builder emitted.
   * The capabilities field is the last two octets of that element. */
  size_t ie_len = 0;
  const uint8_t* rsn = devourer::sta::find_ie(f.data() + 36, f.size() - 36,
                                              devourer::sta::kEidRsn, &ie_len);
  check(rsn != nullptr && ie_len >= 2, "the beacon carries an RSN element");
  if (!rsn) return;
  uint8_t* caps = const_cast<uint8_t*>(rsn) + ie_len - 2;
  caps[0] = (uint8_t)(caps[0] | 0x40);

  const BssEntry* e = t.observe(f.data(), f.size(), -30, 1000);
  check(e && e->info.rsn_mfp_required, "MFPR is parsed out of the beacon");
  check(e && !e->info.rsn_ccmp_psk, "...and makes the BSS unjoinable");
  check(t.select("net") == nullptr, "...so selection finds no candidate");
}

/* Full is not the same as broken. Sixteen neighbours heard before the target
 * network would make the table permanently useless, and a scan in a block of
 * flats hits that in a second. */
void test_eviction_when_full() {
  BssTable t;
  const int cap = BssTable::capacity();

  for (int i = 0; i < cap; i++) {
    const uint8_t id[6] = {0x02, 0, 0, 0, 0, (uint8_t)(0x10 + i)};
    std::vector<uint8_t> f = beacon(id, "filler", 6, true);
    /* Ascending timestamps, so slot 0 is the oldest. */
    t.observe(f.data(), f.size(), -50, (uint32_t)(1000 + i));
  }
  check(t.count() == cap, "the table fills to capacity");

  const uint8_t oldest[6] = {0x02, 0, 0, 0, 0, 0x10};
  const uint8_t wanted[6] = {0x02, 0, 0, 0, 0, 0x77};
  std::vector<uint8_t> f = beacon(wanted, "target", 6, true);
  const BssEntry* e = t.observe(f.data(), f.size(), -30, 2000);

  check(e != nullptr, "a new BSS is still admitted when the table is full");
  check(t.find(wanted) != nullptr, "...and is findable");
  check(t.find(oldest) == nullptr, "...having evicted the least recently heard");
  check(t.count() == cap, "...with the count unchanged");
  check(t.select("target") != nullptr,
        "the network being looked for survives a crowded band");

  t.clear();
  check(t.count() == 0 && t.find(wanted) == nullptr, "clear() empties it");
}

/* THE EVICTION RULE IS AN ATTACK WITHOUT set_wanted().
 *
 * The victim is the entry heard from longest ago. A real AP beacons about ten
 * times a second, so its age is nearly always zero-ish - but an attacker
 * emitting beacons for sixteen fabricated BSSIDs as fast as the radio allows
 * keeps every fabricated entry at age zero and makes the GENUINE AP the
 * oldest, every time. The table thrashes and select() returns nothing for
 * most of the window in which the station is trying to associate. */
void test_wanted_ssid_survives_a_flood() {
  BssTable t;
  const uint8_t target[6] = {0x02, 0, 0, 0, 0, 0x77};
  std::vector<uint8_t> want = beacon(target, "target", 6, true);

  /* THE NEGATIVE ARM FIRST, so the positive one is not a coincidence: with no
   * wanted SSID set, the flood evicts the network being looked for. */
  t.observe(want.data(), want.size(), -30, 1000);
  for (int i = 0; i < BssTable::capacity() * 2; i++) {
    const uint8_t id[6] = {0x02, 0, 0, 0, 1, (uint8_t)i};
    std::vector<uint8_t> f = beacon(id, "flood", 6, true);
    t.observe(f.data(), f.size(), -30, (uint32_t)(2000 + i));
  }
  check(t.select("target") == nullptr,
        "unprotected, a flood evicts the wanted network");

  /* And with it set, the same flood cannot touch it. */
  BssTable u;
  u.set_wanted("target");
  u.observe(want.data(), want.size(), -30, 1000);
  for (int i = 0; i < BssTable::capacity() * 4; i++) {
    const uint8_t id[6] = {0x02, 0, 0, 0, 1, (uint8_t)i};
    std::vector<uint8_t> f = beacon(id, "flood", 6, true);
    u.observe(f.data(), f.size(), -30, (uint32_t)(2000 + i));
  }
  check(u.select("target") != nullptr,
        "with set_wanted(), the flood cannot evict it");
  check(u.find(target) != nullptr, "...and it is still findable by BSSID");
  check(u.count() == BssTable::capacity(),
        "...and the table is still full of the flood, as it should be");

  /* The protection is not a leak: a second BSS airing the wanted SSID is
   * admitted, because that is a real roaming candidate. */
  const uint8_t second[6] = {0x02, 0, 0, 0, 0, 0x78};
  std::vector<uint8_t> also = beacon(second, "target", 36, true);
  u.observe(also.data(), also.size(), -20, 9000);
  check(u.find(second) != nullptr, "a second BSS for the wanted SSID is admitted");
  check(u.find(target) != nullptr, "...without evicting the first");
}

}  // namespace

int main() {
  test_observe_and_dedupe();
  test_only_beacons_are_observed();
  test_expire();
  test_select();
  test_mfp_required_is_skipped();
  test_eviction_when_full();
  test_wanted_ssid_survives_a_flood();

  if (g_fail) {
    std::printf("bss_table_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("bss_table_selftest: OK\n");
  return 0;
}
