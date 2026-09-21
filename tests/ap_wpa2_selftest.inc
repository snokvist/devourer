/* ap_wpa2_selftest.inc — the headless half of tests/ap_wpa2.cpp.
 *
 * WHY THIS IS AN .inc AND NOT A .cpp. Everything it drives is `static` in
 * ap_wpa2.cpp: on_rx(), the station table, the handshake, the CCMP transmit
 * paths, the ledger counters. A separate translation unit cannot reach any of
 * it, and making them extern would export a harness's internals for anything
 * to link against. So this file is textually included at the end of
 * ap_wpa2.cpp and reached through `ap_wpa2 --self-test`, which returns before
 * libusb is touched. It is not a header; do not include it anywhere else.
 *
 * WHY IT EXISTS. Phase 2b closed with this written down as its largest
 * carried-forward defect: "tests/ap_wpa2.cpp has NO ctest target — the
 * handshake, relay crypto and TAP paths are bench-only." Four gates rested on
 * narrated on-air runs, and a narrated run cannot be re-run by someone else,
 * cannot bisect, and does not fail when a later refactor breaks it. Phase
 * 2b.3 lifted the forwarding DECISION into a pure function with its own
 * ctest; this covers what was left, in place, without lifting anything else.
 *
 * WHAT IT DRIVES. Real synthetic frames into the real on_rx(), and the real
 * transmit queue read back out. It plays the SUPPLICANT: derives the PTK from
 * the same PSK, builds msg2 and msg4 with a correct MIC, encrypts data frames
 * under the PTK it negotiated, and decrypts what the AP airs back. Nothing is
 * stubbed and nothing is mocked — the only missing piece is the radio.
 *
 * WHAT IT DOES NOT COVER, stated rather than implied:
 *   - USB, the beacon, and everything below send_packet(). Those need a
 *     device and stay in tests/mt7612u_ap_onair.sh.
 *   - The four-way's interoperability. This side derives the PTK with
 *     compute_ptk_into() in both roles, so the KDF cannot disagree with
 *     itself here; wpa_supplicant on air is what pins that.
 *   - The TAP's reader THREAD. tap_down_one() is called directly; the thread
 *     around it in main() is a read() loop and nothing else.
 */

namespace selftest {

static int g_fail = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stdout, "FAIL: %s\n", what);
    g_fail++;
  }
}

static const uint8_t kStaA[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x01};
static const uint8_t kStaB[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x02};
static const uint8_t kBroadcast6[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
static const uint8_t kOffBss[6] = {0x02, 0x99, 0x99, 0x99, 0x99, 0x99};

/* ---- feeding the AP ----------------------------------------------------- */

/* MT7612U's MAC strips the FCS, which is the only configuration this harness
 * has ever run in; tests/rx_mpdu.h owns that distinction and on_rx() reads it
 * through mpdu_len(). Building the Packet here rather than in each cell keeps
 * the two from drifting. */
static void feed(std::vector<uint8_t>& mpdu) {
  Packet p{};
  p.RxAtrib.fcs_present = false;
  p.RxAtrib.crc_err = false;
  p.RxAtrib.pkt_len = (uint16_t)mpdu.size();
  p.Data = std::span<uint8_t>(mpdu.data(), mpdu.size());
  on_rx(p);
}

/* Everything the AP queued since the last call. enqueue() prefixes the
 * radiotap header, which is empty in this mode because InitWrite never ran —
 * asserted in self_test() rather than assumed, because a non-empty g_rt would
 * silently shift every offset below.
 *
 * Empty vectors are dropped: two call sites enqueue whatever ccmp_tx()
 * returned without checking, and a cipher refusal is an empty frame. */
static std::vector<std::vector<uint8_t>> drain() {
  std::lock_guard<std::mutex> lk(g_q_mu);
  std::vector<std::vector<uint8_t>> out;
  for (std::vector<uint8_t>& f : g_q)
    if (!f.empty()) out.push_back(std::move(f));
  g_q.clear();
  return out;
}

/* ---- the supplicant side ------------------------------------------------ */

/* RFC 3394 unwrap — the station's half of the GTK delivery in msg3. The AP
 * wraps; nothing in this tree unwrapped until now, so what msg3 actually
 * carried was an assumption. */
static int aes_unwrap(const uint8_t* kek, const uint8_t* in, int inlen,
                      uint8_t* out) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  int ol = 0, tmp = 0, ok;

  EVP_CIPHER_CTX_set_flags(c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  EVP_DecryptInit_ex(c, EVP_aes_128_wrap(), NULL, kek, NULL);
  ok = EVP_DecryptUpdate(c, out, &ol, in, inlen);
  if (ok) ok = EVP_DecryptFinal_ex(c, out + ol, &tmp);
  EVP_CIPHER_CTX_free(c);
  return ok ? ol + tmp : -1;
}

/* An EAPOL-Key frame from the STATION. Deliberately its own builder rather
 * than a call to the AP's eapol_frame(): that one takes a Station out of the
 * AP's own table and writes the AP's replay counter, so reusing it would have
 * the AP talking to itself and would not notice if it started writing a field
 * at the wrong offset. The offsets here were read off 802.11-2016 12.7.2. */
static std::vector<uint8_t> sup_eapol(uint16_t keyinfo, const uint8_t replay[8],
                                      const uint8_t snonce[32],
                                      const uint8_t kck[16]) {
  std::vector<uint8_t> e(99, 0);
  e[0] = 2;                                  /* 802.1X-2004 */
  e[1] = 3;                                  /* EAPOL-Key */
  e[2] = 0; e[3] = 95;                       /* body length, no key data */
  e[4] = 2;                                  /* RSN key descriptor */
  e[5] = (uint8_t)(keyinfo >> 8);
  e[6] = (uint8_t)(keyinfo & 0xff);
  e[7] = 0; e[8] = 16;                       /* key length */
  std::memcpy(e.data() + 9, replay, 8);
  if (snonce) std::memcpy(e.data() + 17, snonce, 32);
  e[97] = 0; e[98] = 0;                      /* key data length */
  if (kck) {
    unsigned int l;
    uint8_t d[20];
    std::memset(e.data() + 81, 0, 16);
    HMAC(EVP_sha1(), kck, 16, e.data(), e.size(), d, &l);
    std::memcpy(e.data() + 81, d, 16);
  }
  return e;
}

/* Wrap a payload in a to-DS 802.11 data frame from `sta`. */
static std::vector<uint8_t> sta_llc_frame(const uint8_t sta[6],
                                          const uint8_t dest[6],
                                          uint16_t ethertype,
                                          const std::vector<uint8_t>& payload) {
  std::vector<uint8_t> m =
      devourer::sta::data_hdr_to_ds(kBssid, sta, dest, /*protect=*/false, 1);
  devourer::sta::append_llc_snap(m, ethertype);
  m.insert(m.end(), payload.begin(), payload.end());
  return m;
}

/* An association request. on_rx() reads nothing out of the body, but a real
 * one carries capability, listen interval and the station's RSN IE, and a
 * bare 24-byte header would not resemble what the branch actually sees. */
static std::vector<uint8_t> assoc_req(const uint8_t sta[6]) {
  std::vector<uint8_t> m = devourer::sta::mgmt_hdr(0x00, kBssid, sta, kBssid);
  devourer::sta::assign_seq(m, 1);
  m.insert(m.end(), {0x11, 0x00, 0x0a, 0x00});        /* cap, listen interval */
  const std::vector<uint8_t>& r = rsn_ie();
  m.insert(m.end(), r.begin(), r.end());
  return m;
}

/* The first queued EAPOL-Key frame, or null. A from-DS non-QoS data header is
 * 24 bytes, then 8 of LLC/SNAP. */
static const uint8_t* find_eapol(const std::vector<std::vector<uint8_t>>& q,
                                 size_t* len_out) {
  for (const std::vector<uint8_t>& f : q) {
    if (f.size() < 24 + 8 + 99) continue;
    if (f[0] != 0x08) continue;
    if (!(f[24] == 0xaa && f[30] == 0x88 && f[31] == 0x8e)) continue;
    *len_out = f.size() - 32;
    return f.data() + 32;
  }
  return nullptr;
}

static uint16_t eapol_keyinfo(const uint8_t* e) {
  return (uint16_t)((e[5] << 8) | e[6]);
}

/* A station that has finished the 4-way. Everything the data-plane cells need
 * lives here, so they do not each re-derive it. */
struct Supplicant {
  uint8_t addr[6];
  uint8_t anonce[32];
  uint8_t snonce[32];
  uint8_t ptk[48];
  uint8_t replay[8];
  uint8_t gtk[16];
  uint8_t gtk_keyid;
  uint16_t aid;
  uint64_t tx_pn;

  const uint8_t* kck() const { return ptk; }
  const uint8_t* kek() const { return ptk + 16; }
  const uint8_t* tk() const { return ptk + 32; }
};

/* Drive a whole association and 4-way for one station, checking each step.
 * Returns false at the first thing that did not happen, so a later cell does
 * not run against half a handshake and produce a second, derived failure. */
static bool associate(Supplicant& s, const uint8_t addr[6], const char* tag) {
  char what[128];
  std::memcpy(s.addr, addr, 6);
  s.tx_pn = 1;

  std::vector<uint8_t> ar = assoc_req(addr);
  feed(ar);
  std::vector<std::vector<uint8_t>> q = drain();

  /* The association response carries the AID this station was allocated. It
   * used to be hardcoded to 1, which is true by accident with one station. */
  const std::vector<uint8_t>* resp = nullptr;
  for (const std::vector<uint8_t>& f : q)
    if (f.size() >= 30 && f[0] == 0x10) resp = &f;
  std::snprintf(what, sizeof what, "%s: association response", tag);
  if (!resp) { check(false, what); return false; }
  s.aid = (uint16_t)((((*resp)[29] << 8) | (*resp)[28]) & 0x3fff);

  size_t elen = 0;
  const uint8_t* e = find_eapol(q, &elen);
  std::snprintf(what, sizeof what, "%s: msg1 follows the association", tag);
  if (!e) { check(false, what); return false; }
  std::snprintf(what, sizeof what, "%s: msg1 is pairwise+ack without a MIC",
                tag);
  check((eapol_keyinfo(e) & 0x0108) == 0x0008, what);

  std::memcpy(s.anonce, e + 17, 32);
  std::memcpy(s.replay, e + 9, 8);
  for (int i = 0; i < 32; i++) s.snonce[i] = (uint8_t)(0x40 + i + addr[5]);
  /* compute_ptk_into() is the AP's own derivation. Both roles run the same
   * function of the same inputs by definition, so this pins the handshake
   * plumbing, not the KDF; the KDF is pinned on air against wpa_supplicant. */
  compute_ptk_into(s.addr, s.anonce, s.snonce, s.ptk);

  /* msg2: pairwise + MIC, no install, not secure. */
  std::vector<uint8_t> m2 =
      sta_llc_frame(addr, kBssid, 0x888e,
                    sup_eapol(0x010a, s.replay, s.snonce, s.kck()));
  feed(m2);
  q = drain();
  e = find_eapol(q, &elen);
  std::snprintf(what, sizeof what, "%s: msg2 is answered with msg3", tag);
  if (!e) { check(false, what); return false; }
  std::snprintf(what, sizeof what, "%s: msg3 is install+mic+secure+encrypted",
                tag);
  check((eapol_keyinfo(e) & 0x1340) == 0x1340, what);
  std::snprintf(what, sizeof what, "%s: msg3's MIC verifies under our KCK",
                tag);
  check(check_mic_kck(e, (int)elen, s.kck()), what);

  /* THE GTK, READ BACK. Nothing in this tree ever unwrapped msg3's key data,
   * so "the station installs the GTK" was an assumption. Phase 2b.2 fixed a
   * defect where a second association silently revoked the first station's
   * group key; this is the assertion that would have caught it. */
  const int kdlen = (e[97] << 8) | e[98];
  std::snprintf(what, sizeof what, "%s: msg3 carries wrapped key data", tag);
  if (kdlen <= 8 || (int)elen < 99 + kdlen) { check(false, what); return false; }
  uint8_t kd[512];
  const int n = kdlen <= (int)sizeof kd ? aes_unwrap(s.kek(), e + 99, kdlen, kd)
                                        : -1;
  std::snprintf(what, sizeof what, "%s: msg3's key data unwraps under our KEK",
                tag);
  if (n <= 0) { check(false, what); return false; }

  const uint8_t* kde = nullptr;
  for (int i = 0; i + 8 + 16 <= n; i++)
    if (kd[i] == 0xdd && kd[i + 2] == 0x00 && kd[i + 3] == 0x0f &&
        kd[i + 4] == 0xac && kd[i + 5] == 0x01) {
      kde = kd + i;
      break;
    }
  std::snprintf(what, sizeof what, "%s: the key data contains a GTK KDE", tag);
  if (!kde) { check(false, what); return false; }
  s.gtk_keyid = (uint8_t)(kde[6] & 0x03);
  std::memcpy(s.gtk, kde + 8, 16);
  std::snprintf(what, sizeof what, "%s: the delivered GTK is the BSS's GTK",
                tag);
  check(std::memcmp(s.gtk, g_gtk, 16) == 0, what);

  /* msg4: MIC + secure, echoing msg3's counter. */
  std::memcpy(s.replay, e + 9, 8);
  std::vector<uint8_t> m4 = sta_llc_frame(
      addr, kBssid, 0x888e, sup_eapol(0x030a, s.replay, nullptr, s.kck()));
  feed(m4);
  drain();

  std::lock_guard<std::mutex> l(g_hs_mu);
  devourer::sta::Station* st = g_stas.find(addr);
  std::snprintf(what, sizeof what, "%s: the 4-way completes", tag);
  check(st && st->state == devourer::sta::HsState::Done, what);
  std::snprintf(what, sizeof what, "%s: the AP holds the same PTK we derived",
                tag);
  check(st && std::memcmp(st->ptk, s.ptk, 48) == 0, what);
  return st && st->state == devourer::sta::HsState::Done;
}

/* A protected data frame from a station, exactly as the air would deliver it.
 * `tid` < 0 builds a non-QoS frame.
 *
 * `frag` has to be set BEFORE encryption and not poked into the finished
 * frame: the CCMP AAD masks the sequence number but KEEPS the fragment
 * number, so editing byte 22 afterwards produces a MIC failure rather than
 * the fragment the caller wanted. */
static std::vector<uint8_t> sta_encrypted(Supplicant& s, const uint8_t dest[6],
                                          const std::vector<uint8_t>& msdu,
                                          int tid, uint8_t frag = 0) {
  std::vector<uint8_t> hdr =
      devourer::sta::data_hdr_to_ds(kBssid, s.addr, dest, /*protect=*/false, 7);
  hdr[22] = (uint8_t)((hdr[22] & 0xf0) | (frag & 0x0f));
  if (tid >= 0) {
    hdr[0] = 0x88;                     /* QoS Data */
    hdr.push_back((uint8_t)(tid & 0x0f));
    hdr.push_back(0);
  }
  std::vector<uint8_t> out(
      devourer::sta::ccmp_encrypted_len(hdr.size(), msdu.size()));
  const size_t n = devourer::sta::ccmp_encrypt(
      g_crypto, s.tk(), hdr.data(), hdr.size(), s.addr, s.tx_pn++, 0,
      msdu.data(), msdu.size(), out.data(), out.size());
  out.resize(n);
  return out;
}

/* An MSDU: LLC/SNAP plus a recognisable body. */
static std::vector<uint8_t> msdu(uint16_t ethertype, uint8_t mark, size_t n) {
  std::vector<uint8_t> m = {0xaa, 0xaa, 0x03, 0, 0, 0,
                            (uint8_t)(ethertype >> 8),
                            (uint8_t)(ethertype & 0xff)};
  for (size_t i = 0; i < n; i++) m.push_back((uint8_t)(mark + i));
  return m;
}

/* Decrypt one frame the AP aired. Returns the MSDU, or empty. */
static std::vector<uint8_t> ap_decrypt(const std::vector<uint8_t>& f,
                                       const uint8_t tk[16],
                                       const uint8_t a2[6], uint64_t* pn) {
  const size_t hlen = devourer::sta::data_hdr_len(f[0], f[1]);
  std::vector<uint8_t> pt(devourer::sta::ccmp_decrypted_len(f.size(), hlen));
  size_t ptlen = 0;
  if (!devourer::sta::ccmp_decrypt(g_crypto, tk, f.data(), f.size(), hlen, a2,
                                   pt.data(), pt.size(), &ptlen, pn))
    return {};
  pt.resize(ptlen);
  return pt;
}

/* ---- the cells ---------------------------------------------------------- */

/* Two stations, each with its own PTK, its own PN space and its own receive
 * window, sharing one group key. The 2b.2 phase gate, until now observable
 * only by watching two laptops associate. */
static void test_handshake(Supplicant& a, Supplicant& b) {
  check(associate(a, kStaA, "A"), "A completed its handshake");
  check(associate(b, kStaB, "B"), "B completed its handshake");
  check(a.aid != b.aid, "the two stations got different AIDs");
  check(std::memcmp(a.ptk, b.ptk, 48) != 0, "the two PTKs differ");
  check(std::memcmp(a.gtk, b.gtk, 16) == 0,
        "both stations were given the SAME GTK");
  check(a.gtk_keyid == 1, "the GTK is delivered at key id 1");
}

/* NEGATIVE, and the one that matters most: a forged msg2 must change nothing.
 *
 * The replay counter msg2 has to quote is readable out of cleartext msg1, so
 * anyone can produce a well-formed msg2 — the MIC is the ONLY thing between a
 * forgery and the station's key material. An earlier version derived straight
 * into Station::ptk and checked the MIC afterwards, so a forgery overwrote an
 * in-flight key before anything authenticated it. */
static void test_forged_msg2_rejected() {
  static const uint8_t kStaC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x03};
  Supplicant c{};
  uint8_t before[48];

  std::vector<uint8_t> ar = assoc_req(kStaC);
  feed(ar);
  std::vector<std::vector<uint8_t>> q = drain();
  size_t elen = 0;
  const uint8_t* e = find_eapol(q, &elen);
  if (!e) { check(false, "forged msg2: the station got a msg1"); return; }
  std::memcpy(c.addr, kStaC, 6);
  std::memcpy(c.anonce, e + 17, 32);
  std::memcpy(c.replay, e + 9, 8);
  std::memset(c.snonce, 0x5a, 32);
  compute_ptk_into(c.addr, c.anonce, c.snonce, c.ptk);

  {
    std::lock_guard<std::mutex> l(g_hs_mu);
    devourer::sta::Station* st = g_stas.find(kStaC);
    if (!st) { check(false, "forged msg2: the station has a record"); return; }
    std::memcpy(before, st->ptk, 48);
  }

  /* Correct in every respect except one flipped MIC bit. */
  std::vector<uint8_t> ek = sup_eapol(0x010a, c.replay, c.snonce, c.kck());
  ek[81] ^= 0x01;
  std::vector<uint8_t> m2 = sta_llc_frame(kStaC, kBssid, 0x888e, ek);
  feed(m2);
  q = drain();

  check(find_eapol(q, &elen) == nullptr, "a forged msg2 MIC produces NO msg3");

  std::lock_guard<std::mutex> l(g_hs_mu);
  devourer::sta::Station* st = g_stas.find(kStaC);
  check(st && st->state == devourer::sta::HsState::WaitMsg2,
        "a forged msg2 leaves the handshake where it was");
  check(st && std::memcmp(st->ptk, before, 48) == 0,
        "a forged msg2 does NOT touch the station's key material");
  check(st && !st->keyed(), "a forged msg2 does not key the station");
  if (st) {
    uint8_t gone[6];
    std::memcpy(gone, st->addr, 6);
    g_stas.remove(gone);
  }
}

/* NEGATIVE: the key replay counter is a WINDOW, and both of its edges hold.
 *
 * send_msg1/send_msg3 bump the counter on every transmission, retransmissions
 * included, so demanding equality drops the legitimate msg2 of a station that
 * answered an earlier copy — that regression was shipped once. A window that
 * accepts everything is not a check at all, which is what this pins: a
 * counter above anything the AP has sent, and one older than the window, are
 * both refused, while the current one still works.
 *
 * Reaching the old edge needs the counter above kHsMaxTries, so this drives
 * hs_tick()'s retransmissions by moving the station's own deadline into the
 * past rather than by sleeping for them. */
static void test_replay_counter_window() {
  static const uint8_t kStaD[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x04};
  Supplicant d{};
  uint8_t anonce1[32];

  std::vector<uint8_t> ar = assoc_req(kStaD);
  feed(ar);
  std::vector<std::vector<uint8_t>> q = drain();
  size_t elen = 0;
  const uint8_t* e = find_eapol(q, &elen);
  if (!e) { check(false, "replay window: the station got a msg1"); return; }
  std::memcpy(d.addr, kStaD, 6);
  std::memcpy(d.anonce, e + 17, 32);
  std::memcpy(anonce1, e + 17, 32);
  std::memset(d.snonce, 0x33, 32);

  /* Three retransmissions: hs_tries reaches kHsMaxTries, which is the last
   * transmission before the give-up latch frees the slot. */
  for (int i = 0; i < kHsMaxTries - 1; i++) {
    {
      std::lock_guard<std::mutex> l(g_hs_mu);
      devourer::sta::Station* st = g_stas.find(kStaD);
      if (!st) { check(false, "replay window: the record survives"); return; }
      st->hs_tx_ms = now_ms() - 10000.0;
    }
    hs_tick();
    q = drain();
    e = find_eapol(q, &elen);
    if (!e) { check(false, "replay window: msg1 was retransmitted"); return; }
    check(std::memcmp(e + 17, anonce1, 32) == 0,
          "a retransmitted msg1 carries the SAME ANonce");
  }
  std::memcpy(d.replay, e + 9, 8);
  check(replay_ctr(d.replay) == (uint64_t)kHsMaxTries,
        "the counter advanced once per transmission");
  compute_ptk_into(d.addr, d.anonce, d.snonce, d.ptk);

  uint8_t ahead[8], stale[8];
  std::memcpy(ahead, d.replay, 8);
  ahead[7] = (uint8_t)(ahead[7] + 1);        /* never sent by this AP */
  std::memcpy(stale, d.replay, 8);
  stale[7] = (uint8_t)(stale[7] - (uint8_t)kHsMaxTries);  /* one past the edge */

  std::vector<uint8_t> f1 =
      sta_llc_frame(kStaD, kBssid, 0x888e,
                    sup_eapol(0x010a, ahead, d.snonce, d.kck()));
  feed(f1);
  check(find_eapol(drain(), &elen) == nullptr,
        "a counter the AP never issued is refused, MIC or no MIC");

  std::vector<uint8_t> f2 =
      sta_llc_frame(kStaD, kBssid, 0x888e,
                    sup_eapol(0x010a, stale, d.snonce, d.kck()));
  feed(f2);
  check(find_eapol(drain(), &elen) == nullptr,
        "a counter older than the window is refused");

  /* And the window is not simply shut: the oldest counter still inside it is
   * accepted, which is the property the equality check broke. */
  uint8_t oldest[8];
  std::memcpy(oldest, d.replay, 8);
  oldest[7] = (uint8_t)(oldest[7] - (uint8_t)(kHsMaxTries - 1));
  std::vector<uint8_t> f3 =
      sta_llc_frame(kStaD, kBssid, 0x888e,
                    sup_eapol(0x010a, oldest, d.snonce, d.kck()));
  feed(f3);
  check(find_eapol(drain(), &elen) != nullptr,
        "the OLDEST counter still in the window is accepted");

  std::lock_guard<std::mutex> l(g_hs_mu);
  uint8_t gone[6];
  std::memcpy(gone, kStaD, 6);
  g_stas.remove(gone);
}

/* A duplicate msg2 arriving after the handshake finished must not restart
 * anything. `state != WaitMsg2` is what enforces it; without that guard a
 * captured msg2 replayed later re-derives the PTK and resets the receive
 * window, which hands an attacker a fresh replay window for every data frame
 * they ever captured. */
static void test_duplicate_msg2_ignored(Supplicant& a) {
  std::vector<uint8_t> m2 =
      sta_llc_frame(a.addr, kBssid, 0x888e,
                    sup_eapol(0x010a, a.replay, a.snonce, a.kck()));
  feed(m2);
  size_t elen = 0;
  check(find_eapol(drain(), &elen) == nullptr,
        "a msg2 replayed after the handshake produces no msg3");
  std::lock_guard<std::mutex> l(g_hs_mu);
  devourer::sta::Station* st = g_stas.find(a.addr);
  check(st && st->state == devourer::sta::HsState::Done,
        "...and leaves the station keyed");
}

/* INTRA-BSS RELAY, the 2b.7 gate. A sends to B; the AP must decrypt under A's
 * key and re-encrypt under B's, because the ciphertext cannot be forwarded:
 * the AAD authenticates addr1..addr3 and the nonce carries A2. */
static void test_relay(Supplicant& a, Supplicant& b) {
  const uint64_t before_relayed = g_relayed.load();
  const std::vector<uint8_t> body = msdu(0x0800, 0x10, 40);

  std::vector<uint8_t> f = sta_encrypted(a, b.addr, body, -1);
  feed(f);
  std::vector<std::vector<uint8_t>> q = drain();

  check(g_relayed.load() == before_relayed + 1, "the frame was relayed");
  if (q.size() != 1) { check(false, "the relay aired exactly one frame"); return; }

  const std::vector<uint8_t>& r = q[0];
  /* 802.11-2016 Table 9-26, from-DS: addr1 = DA, addr2 = BSSID, addr3 = SA.
   * addr2 stays the BSSID because the AP is the transmitter, which is also
   * why the nonce's A2 is the BSSID and not A's address. */
  check((r[1] & 0x03) == 0x02, "the relayed frame is from-DS");
  check(std::memcmp(r.data() + 4, b.addr, 6) == 0, "addr1 is B");
  check(std::memcmp(r.data() + 10, kBssid, 6) == 0, "addr2 is the BSSID");
  check(std::memcmp(r.data() + 16, a.addr, 6) == 0, "addr3 is A, not the AP");

  uint64_t pn = 0;
  check(ap_decrypt(r, b.tk(), kBssid, &pn) == body,
        "B decrypts it, and the MSDU is unchanged");
  /* The wrong key must not work — otherwise a relay that forgot to re-key
   * would still look like a pass above. */
  check(ap_decrypt(r, a.tk(), kBssid, &pn).empty(),
        "A's key does NOT decrypt the frame relayed to B");

  /* THE PN MUST MOVE, and it must move in B's own space.
   *
   * Every assertion above passes with `pn` pinned to a constant - a mutation
   * that did exactly that survived this whole file on 2026-09-21 - and two
   * frames under one key at one PN is keystream reuse, which is the worst
   * thing a cipher can be asked to do. Nothing had ever looked at the PN. */
  std::vector<uint8_t> f2 = sta_encrypted(a, b.addr, body, -1);
  feed(f2);
  std::vector<std::vector<uint8_t>> q2 = drain();
  if (q2.size() != 1) { check(false, "a second frame was relayed"); return; }
  uint64_t pn2 = 0;
  check(ap_decrypt(q2[0], b.tk(), kBssid, &pn2) == body,
        "the second relayed frame decrypts too");
  check(pn2 > pn, "each relayed frame draws a FRESH PN from B's space");
}

/* A replayed data frame must be refused, and must not be relayed a second
 * time. Duplicate detection is this, by construction: an 802.11
 * retransmission carries the same PN. */
static void test_data_replay_refused(Supplicant& a, Supplicant& b) {
  const std::vector<uint8_t> body = msdu(0x0800, 0x60, 24);
  std::vector<uint8_t> f = sta_encrypted(a, b.addr, body, -1);

  feed(f);
  drain();
  const uint64_t relayed = g_relayed.load();
  const uint64_t replays = g_replayed.load();

  std::vector<uint8_t> again = f;
  feed(again);
  std::vector<std::vector<uint8_t>> q = drain();

  check(g_replayed.load() == replays + 1, "the replayed frame was counted");
  check(g_relayed.load() == relayed, "...and was NOT relayed a second time");
  check(q.empty(), "...and nothing was aired for it");
}

/* A tampered frame is a MIC failure, not a decode error. The ledger's "MIC
 * failures" line is what an on-air run is read for, so it has to move for the
 * right reason. */
static void test_mic_failure_counted(Supplicant& a, Supplicant& b) {
  const uint64_t before = g_mic_fail.load();
  std::vector<uint8_t> f = sta_encrypted(a, b.addr, msdu(0x0800, 0x70, 24), -1);
  f[f.size() - 12] ^= 0x40;                 /* inside the ciphertext */
  feed(f);
  check(g_mic_fail.load() == before + 1, "a tampered frame is a MIC failure");
  check(drain().empty(), "...and nothing is aired for it");
}

/* A QoS uplink, which NO STATION ON THE BENCH CAN SEND THIS AP: it advertises
 * neither WMM nor HT, so the QoS receive path — the 26-byte header length,
 * the per-TID replay window, the relay of a QoS frame — is unreachable on
 * air. Here it is reachable.
 *
 * WHAT THIS CELL CANNOT SEE, because it is a round trip: whether the AAD and
 * the CCM nonce are RIGHT. Both ends here call ccmp_encrypt/ccmp_decrypt, so
 * a wrong TID in the nonce is wrong identically in both directions and the
 * MIC still verifies — which is precisely how the 2b.1 defect survived, and a
 * mutation reintroducing it survived this file too. The thing that catches it
 * is test_kernel_vectors() in tests/ccmp_selftest.cpp, which checks our
 * framing against frames the Linux kernel encrypted. This cell proves the
 * WIRING; that one proves the FRAMING. */
static void test_qos_uplink(Supplicant& a, Supplicant& b) {
  const uint64_t before = g_relayed.load();
  const std::vector<uint8_t> body = msdu(0x0800, 0x90, 32);

  /* No assertion that f[0] == 0x88 here: sta_encrypted() sets that byte, so
   * it would be a property of the fixture and could not fail whatever the AP
   * did. What proves the AP took the QoS path is the relay below - a 26-byte
   * header read as 24 puts the CCMP header two bytes early and the MIC
   * fails. */
  std::vector<uint8_t> f = sta_encrypted(a, b.addr, body, /*tid=*/5);
  feed(f);
  std::vector<std::vector<uint8_t>> q = drain();
  check(g_relayed.load() == before + 1,
        "a TID 5 QoS uplink decrypts and is relayed");
  if (q.size() == 1) {
    uint64_t pn = 0;
    check(ap_decrypt(q[0], b.tk(), kBssid, &pn) == body, "...with the MSDU intact");
  } else {
    check(false, "...and aired exactly one frame");
  }

  /* The per-TID window: the same PN on a DIFFERENT TID is not a replay. One
   * shared counter drops legitimate frames the moment two TIDs interleave,
   * which is routine as soon as voice or video shares a link. */
  const uint64_t replays = g_replayed.load();
  const uint64_t relayed = g_relayed.load();
  a.tx_pn--;                                 /* re-use the PN just consumed */
  std::vector<uint8_t> other = sta_encrypted(a, b.addr, body, /*tid=*/6);
  feed(other);
  drain();
  check(g_replayed.load() == replays,
        "the same PN on another TID is not a replay");
  /* AND IT WAS ACTUALLY FORWARDED. "the replay counter did not move" is also
   * true of a frame dropped for a completely different reason - a MIC
   * failure, a refused relay - so on its own it is a negative arm that passes
   * for the wrong reason. */
  check(g_relayed.load() == relayed + 1,
        "...it was decrypted and relayed, not merely not-counted");
}

/* Group traffic. Until 2b.6 the GTK was delivered and then never used to
 * encrypt anything, so broadcast simply did not exist on this BSS. */
static void test_group_flood(Supplicant& a, Supplicant& b) {
  const uint64_t before = g_group_tx.load();
  const std::vector<uint8_t> body = msdu(0x88b5, 0xa0, 28);

  std::vector<uint8_t> f = sta_encrypted(a, kBroadcast6, body, -1);
  feed(f);
  std::vector<std::vector<uint8_t>> q = drain();
  check(g_group_tx.load() == before + 1, "the broadcast was flooded once");

  const std::vector<uint8_t>* g = nullptr;
  for (const std::vector<uint8_t>& c : q)
    if (c.size() > 32 && (c[4] & 0x01)) g = &c;
  if (!g) { check(false, "a group-addressed frame was aired"); return; }
  check(((*g)[1] & 0x40) != 0, "the group frame is protected");
  /* KEY ID 1. msg3 advertises the GTK there; a group frame sent at key id 0
   * is looked up as the PAIRWISE key at the station and fails its MIC with no
   * diagnostic at either end. */
  check((((*g)[24 + 3] >> 6) & 0x03) == 1, "the group frame is at key id 1");
  check(std::memcmp(g->data() + 16, a.addr, 6) == 0, "addr3 is the sender");

  uint64_t pn = 0;
  check(ap_decrypt(*g, b.gtk, kBssid, &pn) == body,
        "B decrypts it under the GTK it was given");
  check(ap_decrypt(*g, b.tk(), kBssid, &pn).empty(),
        "and NOT under its pairwise key");
}

/* Fragmentation and A-MSDU are refused AND COUNTED, which is the position the
 * 2b.7 gate asked for: neither is implemented, and a silent drop is
 * indistinguishable from a link problem. */
static void test_refusals(Supplicant& a, Supplicant& b) {
  const uint64_t frag = g_frag_drop.load(), amsdu = g_amsdu_drop.load();

  std::vector<uint8_t> f =
      sta_encrypted(a, b.addr, msdu(0x0800, 0xb0, 16), -1, /*frag=*/1);
  feed(f);
  check(g_frag_drop.load() == frag + 1, "a fragment is refused and counted");

  /* The A-MSDU bit can be set after encryption: the AAD and the nonce both
   * mask the QoS control down to the TID, so it is not authenticated. The
   * fragment number above is, which is why that one is built in. */
  std::vector<uint8_t> m =
      sta_encrypted(a, b.addr, msdu(0x0800, 0xb0, 16), /*tid=*/0);
  m[24] |= 0x80;
  feed(m);
  check(g_amsdu_drop.load() == amsdu + 1, "an A-MSDU is refused and counted");
  check(drain().empty(), "neither refusal aired anything");
}

/* The local responders: with no TAP, an ARP request for the AP's own address
 * is answered, encrypted under the asking station's key. The only cell that
 * exercises ccmp_tx() and handle_plain(). */
static void test_local_arp(Supplicant& a) {
  std::vector<uint8_t> arp = {0x00, 0x01, 0x08, 0x00, 0x06, 0x04, 0x00, 0x01};
  uint8_t sip[4];
  {
    std::lock_guard<std::mutex> l(g_hs_mu);
    devourer::sta::Station* st = g_stas.find(a.addr);
    if (!st) { check(false, "ARP: the station is in the table"); return; }
    sta_ip(*st, sip);
  }
  arp.insert(arp.end(), a.addr, a.addr + 6);   /* sender hardware address */
  arp.insert(arp.end(), sip, sip + 4);         /* sender protocol address */
  arp.insert(arp.end(), 6, 0x00);              /* target hardware address */
  arp.insert(arp.end(), kApIp, kApIp + 4);     /* ...asking for the AP */

  std::vector<uint8_t> body = msdu(0x0806, 0, 0);
  body.insert(body.end(), arp.begin(), arp.end());
  std::vector<uint8_t> f = sta_encrypted(a, kBroadcast6, body, -1);
  feed(f);
  std::vector<std::vector<uint8_t>> q = drain();

  bool answered = false;
  for (const std::vector<uint8_t>& c : q) {
    if (c.size() < 40 || (c[4] & 0x01)) continue;   /* skip the group flood */
    uint64_t pn = 0;
    const std::vector<uint8_t> pt = ap_decrypt(c, a.tk(), kBssid, &pn);
    if (pt.size() >= 8 + 28 && pt[6] == 0x08 && pt[7] == 0x06 &&
        pt[8 + 7] == 0x02 &&                            /* ARP reply */
        std::memcmp(pt.data() + 8 + 8, kBssid, 6) == 0) /* ...from the AP */
      answered = true;
  }
  check(answered, "the AP answers ARP for its own address, encrypted");
}

/* The TAP, both directions, WITHOUT a TAP device: a pipe stands in for the
 * fd. What is under test is the translation and the routing decision, not
 * /dev/net/tun — and a pipe needs no CAP_NET_ADMIN, so this runs in an
 * ordinary CI job where opening a real TAP would not. */
static void test_tap(Supplicant& a, Supplicant& b) {
  int fds[2];
  if (::pipe(fds) != 0) { check(false, "tap: pipe()"); return; }

  const int saved = g_tap_fd;
  g_tap_fd = fds[1];

  /* UP: a frame for somewhere off this BSS goes to the host. */
  const uint64_t before = g_tap_tx.load();
  const std::vector<uint8_t> body = msdu(0x0800, 0xc0, 20);
  std::vector<uint8_t> f = sta_encrypted(a, kOffBss, body, -1);
  feed(f);
  drain();
  check(g_tap_tx.load() == before + 1, "an off-BSS frame reaches the TAP");

  uint8_t eth[2048];
  const ssize_t got = ::read(fds[0], eth, sizeof eth);
  check(got == (ssize_t)(14 + body.size() - 8),
        "...as an Ethernet frame: 14 bytes of header for 8 of LLC/SNAP");
  if (got > 14) {
    check(std::memcmp(eth, kOffBss, 6) == 0, "...with the real destination");
    check(std::memcmp(eth + 6, a.addr, 6) == 0, "...and the real source");
    check(eth[12] == 0x08 && eth[13] == 0x00, "...and the SNAP ethertype");
    check(std::memcmp(eth + 14, body.data() + 8, body.size() - 8) == 0,
          "...and the payload unchanged");
  }

  /* DOWN: an Ethernet frame from the host for station B becomes a protected
   * 802.11 frame under B's key. */
  std::vector<uint8_t> down;
  down.insert(down.end(), b.addr, b.addr + 6);
  down.insert(down.end(), kOffBss, kOffBss + 6);
  down.push_back(0x08); down.push_back(0x00);
  for (int i = 0; i < 30; i++) down.push_back((uint8_t)(0xd0 + i));
  tap_down_one(down.data(), down.size());
  std::vector<std::vector<uint8_t>> q = drain();
  if (q.size() != 1) {
    check(false, "the host's frame became exactly one 802.11 frame");
  } else {
    uint64_t pn = 0;
    const std::vector<uint8_t> pt = ap_decrypt(q[0], b.tk(), kBssid, &pn);
    check(pt.size() == down.size() - 14 + 8,
          "...whose MSDU is the payload behind a fresh LLC/SNAP header");
    check(pt.size() > 8 &&
              std::memcmp(pt.data() + 8, down.data() + 14,
                          down.size() - 14) == 0,
          "...with the payload unchanged");
    check(std::memcmp(q[0].data() + 16, kOffBss, 6) == 0,
          "...and addr3 is the host-side source, not the AP");
  }

  /* An address on no station goes nowhere, rather than being flooded: a
   * flooded unicast is that frame leaked to every station on the BSS. */
  const uint64_t dropped = g_tap_drop.load();
  std::vector<uint8_t> nowhere = down;
  nowhere[5] = 0x7f;
  tap_down_one(nowhere.data(), nowhere.size());
  check(drain().empty(), "a frame for an unknown unicast address is not aired");
  check(g_tap_drop.load() == dropped + 1, "...and is counted as a drop");

  g_tap_fd = saved;
  ::close(fds[0]);
  ::close(fds[1]);
}


/* THE PHASE 3 STATION, ASSOCIATING WITH THE PHASE 2b AP.
 *
 * Everything else in this file plays the supplicant by hand. This cell runs
 * the real one — devourer::sta::StationSm driving devourer::sta::Supplicant —
 * against the authenticator in this file, entirely in memory.
 *
 * WHY IT IS WORTH ITS OWN CELL. The two implementations share no code and
 * were written months apart from the same clause of the same standard. The
 * authenticator here hand-rolls every EAPOL offset inline; src/sta/Eapol.h
 * names them as constants and parses with bounds checks. Each one's unit test
 * uses a fixture built by its own author, so neither can catch a shared
 * misreading — this is the only place where two independent readings have to
 * agree on the wire, and a disagreement shows up as a handshake that does not
 * complete rather than as a subtle difference nobody looks at.
 *
 * It is also the only cell that exercises the station's ASSOCIATION path:
 * authentication, association, the AID, and the four-way in sequence.
 */
static void test_library_station_associates() {
  using devourer::sta::StationSm;
  static const uint8_t kStaS[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x05};
  devourer::sta::BssTable bss;
  StationSm sm;
  uint8_t snonce[32];
  std::vector<uint8_t> f;

  /* The station finds the BSS the way a real one does: out of a beacon. The
   * beacon is the one this AP actually airs, element for element. */
  std::vector<uint8_t> b = {0,0,0x0a,0,0,0x80,0,0,0x08,0,
      0x80,0,0,0, 0xff,0xff,0xff,0xff,0xff,0xff,
      kBssid[0],kBssid[1],kBssid[2],kBssid[3],kBssid[4],kBssid[5],
      kBssid[0],kBssid[1],kBssid[2],kBssid[3],kBssid[4],kBssid[5],
      0,0, 0,0,0,0,0,0,0,0, 100,0, 0x11,0x00};
  append_ies(b, true, /*beacon=*/true);
  /* THE FIRST TEN BYTES ARE NOT 802.11. What main() hands StartBeacon is a
   * MediaTek beacon-offload header followed by the MPDU, and the 802.11 frame
   * control is at offset 10 - which is why the literal above starts 0x00 0x00
   * 0x0a and the beacon subtype 0x80 appears twice. A parser fed the buffer
   * from byte 0 reads that header as a frame control and refuses it, which is
   * exactly what happened the first time this cell ran. */
  const size_t kBcnHdr = 10;
  const devourer::sta::BssEntry* e =
      bss.observe(b.data() + kBcnHdr, b.size() - kBcnHdr, -40, 0);
  check(e != nullptr, "library station: the AP's own beacon parses");
  if (!e) return;
  check(e->info.ssid == kSsid, "library station: the SSID matches");
  check(e->info.rsn_ccmp_psk,
        "library station: the AP's RSN element reads as WPA2-PSK/CCMP");
  check(bss.select(kSsid) == e, "library station: it is the selected BSS");

  /* A fixed SNonce: this is a test, and Supplicant::start says in as many
   * words that a real one must be unpredictable. */
  for (int i = 0; i < 32; i++) snonce[i] = (uint8_t)(0xc0 + i);
  check(sm.configure(g_crypto, kSsid, g_psk, kStaS),
        "library station: configure derives the PMK from the AP's own PSK");
  check(sm.join(*e, snonce, 0), "library station: join starts");

  /* Pump both queues until neither moves. No clock advances, so nothing here
   * can pass because of a timeout. */
  for (int round = 0; round < 12; round++) {
    bool moved = false;

    while (sm.pop_tx(&f)) {
      moved = true;
      feed(f);
    }
    std::vector<std::vector<uint8_t>> q = drain();
    for (std::vector<uint8_t>& m : q) {
      moved = true;
      sm.on_rx(m.data(), m.size(), 0);
    }
    if (!moved) break;
  }

  check(sm.state() == StationSm::State::Connected,
        "THE LIBRARY STATION ASSOCIATES WITH THIS AP");
  check(sm.aid() != 0, "library station: it was given an AID");
  check(sm.keyed(), "library station: it is keyed");

  /* The two sides must hold the same key material. A handshake that completes
   * on two different PTKs is the failure this cell exists to catch, and
   * "Connected" alone would not see it. */
  std::lock_guard<std::mutex> l(g_hs_mu);
  devourer::sta::Station* st = g_stas.find(kStaS);
  check(st != nullptr, "library station: the AP has a record for it");
  if (!st) return;
  check(st->state == devourer::sta::HsState::Done,
        "library station: the AP considers the four-way complete");
  check(std::memcmp(st->ptk, sm.supplicant().ptk(), 48) == 0,
        "library station: BOTH SIDES DERIVED THE SAME PTK");
  check(sm.supplicant().gtk_valid() &&
            std::memcmp(sm.supplicant().gtk(), g_gtk, 16) == 0,
        "library station: it unwrapped this BSS's GTK from message 3");
  check(sm.supplicant().gtk_key_id() == 1,
        "library station: at key id 1, which is what this AP transmits on");
  check(st->aid == sm.aid(),
        "library station: the AID it recorded is the one the AP allocated");
  g_stas.remove(const_cast<uint8_t*>(kStaS));
}

}  // namespace selftest

/* Headless. Returns before libusb, the radio or the beacon are touched. */
static int self_test() {
  using namespace selftest;

  /* enqueue() prefixes g_rt, which InitWrite fills in and nothing does here.
   * If that ever stops being true, every offset below shifts silently. */
  if (!g_rt.empty()) {
    std::fprintf(stdout, "FAIL: the radiotap prefix is not empty\n");
    return 1;
  }

  Supplicant a{}, b{};
  test_handshake(a, b);
  test_forged_msg2_rejected();
  test_replay_counter_window();
  test_duplicate_msg2_ignored(a);
  test_relay(a, b);
  test_data_replay_refused(a, b);
  test_mic_failure_counted(a, b);
  test_qos_uplink(a, b);
  test_group_flood(a, b);
  test_refusals(a, b);
  test_local_arp(a);
  test_tap(a, b);
  test_library_station_associates();

  if (g_fail) {
    std::fprintf(stdout, "ap_wpa2 --self-test: %d failure(s)\n", g_fail);
    return 1;
  }
  std::fprintf(stdout, "ap_wpa2 --self-test: OK\n");
  return 0;
}
