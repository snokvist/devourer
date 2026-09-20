// ap_wpa2.cpp — devourer as a WPA2-PSK AP. STATUS: the RSN 4-way handshake
// COMPLETES with a real Linux station — wpa_supplicant reports "WPA: Key
// negotiation completed ... [PTK=CCMP GTK=CCMP]" + CTRL-EVENT-CONNECTED, and the
// AP logs msg2-MIC-verified -> msg3 -> msg4-OK. This is the deepest driver-protocol
// milestone (WPA2 key negotiation). It needs no CCMP (EAPOL-Key frames are
// cleartext, MIC-protected), so it works WITHOUT the hardware crypto engine — the
// crypto (PBKDF2/PRF/HMAC-MIC/AES-key-wrap) is openssl in userspace. Association
// reuses the open-AP path (probe/auth/assoc); the beacon/probe/assoc carry an RSN
// IE (WPA2-PSK-CCMP); after assoc the AP runs the authenticator: msg1(ANonce) ->
// msg2(SNonce,MIC) -> derive PTK, verify MIC -> msg3(GTK,MIC) -> msg4.
//
// ENCRYPTED DATA plane too: after the handshake the AP decrypts the station's CCMP
// data frames (software AES-CCM with the TK = PTK[32:48]) and answers ARP + ICMP +
// DHCP ENCRYPTED, so a real station leases 192.168.99.2 over encrypted DHCP (dhcpcd:
// "leased") AND pings the AP over WPA2/CCMP at 0% loss (~2.5 ms RTT). The station
// runs hardware CCMP, the AP software CCMP — they interoperate. So this is a COMPLETE
// zero-config WPA2-PSK AP: associate -> 4-way -> encrypted DHCP -> encrypted IP.
// (HW CCMP offload would need porting the J3 security TX/RX descriptor fields.)
//
// Details that mattered: (1) msg3 key-data pad is 0xDD then 0x00s (a 2nd 0xDD
// mis-parses as a KDE and the station rejects msg3); (2) a prior WRONG_KEY failure
// temp-disables the SSID in wpa_supplicant — cold-cycle the station for a clean run;
// (3) CCMP AAD masks FC subtype/retry/pm/md + sets protected, and masks the seq
// number (keep frag); the nonce is 0|A2|PN(6, big-endian).
//
// Build: g++ -std=c++20 -O2 -Isrc -Iexamples/common tests/ap_wpa2.cpp \
//   examples/common/env_config.cpp examples/common/usb_select.cpp build/libdevourer.a \
//   $(pkg-config --cflags --libs libusb-1.0) -lcrypto -lpthread -o build/ap_wpa2
// Run: sudo DEVOURER_VID=0x2357 DEVOURER_PID=0x012d DEVOURER_CHANNEL=6 \
//   DEVOURER_WPA2_PSK=devourer123 DEVOURER_BCN_TU=25 DEVOURER_TX_WITH_RX=thread \
//   build/ap_wpa2 [sec]
#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <unistd.h>
#include <libusb.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include "RadiotapBuilder.h"
#include "sta/Ccmp.h"
#include "sta/StationTable.h"
#include "sta/Dot11.h"
#include "RxPacket.h"
#include "SelectedChannel.h"
#include "TxMode.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "env_config.h"
#include "logger.h"
#include "usb_select.h"
#include "ccmp_software.h"
#include "rx_mpdu.h"

static const uint8_t kBssid[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};
static const char* kSsid = "devourerAP";
static IRadio* g_dev = nullptr;
static std::vector<uint8_t> g_rt;
static uint8_t g_chan = 6;
static const char* g_psk = "devourer123";
static std::atomic<uint64_t> g_sent{0};
static bool g_ccmp_profile = false;
static std::atomic<uint64_t> g_ccmp_tx_frames{0}, g_ccmp_tx_bytes{0}, g_ccmp_tx_ns{0};
static std::atomic<uint64_t> g_ccmp_rx_frames{0}, g_ccmp_rx_bytes{0}, g_ccmp_rx_ns{0};
static std::mutex g_q_mu;
static std::vector<std::vector<uint8_t>> g_q;

// WPA2-PSK / CCMP RSN IE (group=CCMP, pairwise=CCMP, akm=PSK).
// The RSN element, built once by src/sta/Dot11.h and reused for both the
// beacon/probe advertisement and the msg3 key data. It used to be a literal
// here AND a builder there; the bytes agreed, but nothing enforced that, and a
// drift would only have shown up as a station refusing its own AP.
static const std::vector<uint8_t>& rsn_ie() {
  static const std::vector<uint8_t> ie = [] {
    std::vector<uint8_t> v;
    devourer::sta::append_rsn_ccmp_psk(v);
    return v;
  }();
  return ie;
}

// Per-station 4-way state (single client for the demo).
//
// THE AUTHENTICATOR RETRANSMITS. It used to send msg1 and msg3 exactly once,
// which means a single frame lost in the air stalled the handshake forever:
// the station waits for a message that will never come again, and this side
// sits in "4-way in progress" until the run times out. On a real link that is
// not an edge case - it is what the acceptance harness's wpa2 cell failed on,
// and 802.11-2016 12.7.6.4 requires the retransmission that was missing.
//
// Two rules the retransmission has to obey, both of which a naive "just call
// send_msg1() again" gets wrong:
//   - msg1 must carry the SAME ANonce, and msg3 the SAME GTK. Regenerating
//     either would derive a different PTK from the one the station already
//     installed, or install a group key this AP will not use.
//   - the Key Replay Counter must be INCREMENTED on every retransmission, and
//     the MIC recomputed over it, so the station can tell copies apart.
// Hence the `first` flag: it selects "generate fresh material" and nothing
// else. Everything after it is rebuilt per transmission.
// Per-station state lives in the table now (Phase 2b.2). Everything that was
// a file-scope singleton here - g_sta, g_anonce, g_snonce, g_ptk, g_replay,
// g_state, g_txpn and the CCMP receive window - is a field of
// devourer::sta::Station, one record per associated station.
static devourer::sta::StationTable g_stas;   // guarded by g_hs_mu

// The GTK is NOT per-station, and this line is why the distinction matters.
// It used to sit on the same declaration as g_anonce/g_snonce/g_ptk and was
// regenerated inside send_msg3(first=true) - once per four-way. With one
// station that was invisible. With two, the second station's handshake
// silently revoked the first station's group key. It is generated ONCE, for
// the BSS, before the radio comes up.
static uint8_t g_gtk[16];
// Guards every field above. The RX callback and the main loop's retransmit
// tick both touch them now; before the timer existed only the RX thread did.
static std::mutex g_hs_mu;
// src/sta/Station holds its retransmission timestamp as a plain double so the
// table stays free of <chrono> and of any OS notion of time; the harness owns
// the clock.
static double now_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
static constexpr int kHsMaxTries = 4;                   // 1 + 3 retransmissions
static constexpr auto kHsTimeout = std::chrono::milliseconds(250);

static bool profiled_ccmp(bool encrypt, const uint8_t* key, const uint8_t* nonce,
                          const uint8_t* aad, int aadlen, const uint8_t* input,
                          int input_len, uint8_t* output, uint8_t* tag) {
  if (!g_ccmp_profile)
    return devourer::test::ccmp_software(encrypt, key, nonce, aad, aadlen,
                                         input, input_len, output, tag);
  const auto before = std::chrono::steady_clock::now();
  bool ok = devourer::test::ccmp_software(encrypt, key, nonce, aad, aadlen,
                                          input, input_len, output, tag);
  uint64_t ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - before).count();
  auto& frames = encrypt ? g_ccmp_tx_frames : g_ccmp_rx_frames;
  auto& bytes = encrypt ? g_ccmp_tx_bytes : g_ccmp_rx_bytes;
  auto& elapsed = encrypt ? g_ccmp_tx_ns : g_ccmp_rx_ns;
  frames.fetch_add(1, std::memory_order_relaxed);
  bytes.fetch_add(static_cast<uint64_t>(input_len), std::memory_order_relaxed);
  elapsed.fetch_add(ns, std::memory_order_relaxed);
  return ok;
}

static void enqueue(std::vector<uint8_t> mpdu) {
  std::vector<uint8_t> f; f.reserve(g_rt.size() + mpdu.size());
  f.insert(f.end(), g_rt.begin(), g_rt.end());
  f.insert(f.end(), mpdu.begin(), mpdu.end());
  std::lock_guard<std::mutex> lk(g_q_mu);
  if (g_q.size() < 128) g_q.push_back(std::move(f));
}
static devourer::sta::SeqCounter g_seq;
// (da=sta, sa=bssid, bssid) for an AP answering; a station swaps the first two.
// These now carry a real sequence number - see the note in ap_responder.cpp.
static std::vector<uint8_t> mgmt_hdr(uint8_t fc, const uint8_t* sta) {
  std::vector<uint8_t> m = devourer::sta::mgmt_hdr(fc, sta, kBssid, kBssid);
  devourer::sta::assign_seq(m, g_seq.next());
  return m;
}
/* `beacon` adds the TIM. It is deliberately NOT added to probe or
 * association responses: 802.11-2016 9.4.2.6 puts the TIM in the Beacon
 * frame body only, and a TIM in a probe response is a malformed frame that
 * some stations will reject outright. The element order below is the
 * standard's: SSID, Supported Rates, DS Parameter Set, TIM, then RSN. */
/* Refuse to run if the TIM wiring is wrong.
 *
 * tests/dot11_selftest.cpp covers append_tim() itself and MODELS this wiring
 * with a local lambda - so deleting the `beacon` flag here would leave that
 * test green. This reads what THIS harness builds. Same drift that let
 * `fc0 == 0x88` survive being fixed in the shared module, and that let the
 * ap_onair witness selftest diverge from the harness it guards.
 *
 * Runs BEFORE the USB open, deliberately: the first version ran after it and
 * an injected defect went uncaught on a host with no adapter, which is the
 * only place a wiring check is cheap to run.
 *
 * The TIM belongs in the BEACON ONLY (802.11-2016 9.4.2.6); in a probe
 * response it is a malformed frame some stations reject outright. */
static bool tim_wiring_ok(const std::vector<uint8_t>& beacon_ies,
                          const std::vector<uint8_t>& probe_ies) {
  size_t n = 0;
  const bool in_beacon = devourer::sta::find_ie(
      beacon_ies.data(), beacon_ies.size(), devourer::sta::kEidTim, &n) != nullptr;
  const bool in_probe = devourer::sta::find_ie(
      probe_ies.data(), probe_ies.size(), devourer::sta::kEidTim, &n) != nullptr;
  if (!in_beacon)
    fprintf(stderr, "FATAL: the beacon carries no TIM element\n");
  if (in_probe)
    fprintf(stderr, "FATAL: a TIM leaked into the probe response\n");
  return in_beacon && !in_probe;
}

static void append_ies(std::vector<uint8_t>& m, bool ssid, bool beacon = false) {
  if (ssid) devourer::sta::append_ssid(m, kSsid);
  // Band-correct Supported Rates: CCK+OFDM on 2.4 GHz, OFDM-only on 5 GHz. CCK
  // basic rates (1/2/5.5/11) do not exist on 5 GHz — advertising them makes a
  // 5 GHz station skip the BSS ("rate sets do not match"), so no association.
  // Byte-identical to what this harness carried; now shared with the station
  // side so the two cannot drift apart unnoticed.
  if (g_chan <= 14) devourer::sta::append_supported_rates(m);
  else devourer::sta::append_supported_rates_5g(m);
  devourer::sta::append_ds_params(m, (uint8_t)g_chan);
  if (beacon) devourer::sta::append_tim(m);
  m.insert(m.end(), rsn_ie().begin(), rsn_ie().end());   // RSN IE -> advertise WPA2
}

// --- WPA2 crypto (openssl) --------------------------------------------------
static void prf(const uint8_t* key, int klen, const char* label,
                const uint8_t* data, int dlen, uint8_t* out, int olen) {
  int ll = (int)strlen(label);
  for (int i = 0, gen = 0; gen < olen; ++i, gen += 20) {
    std::vector<uint8_t> b(ll + 1 + dlen + 1);
    memcpy(b.data(), label, ll); b[ll] = 0;
    memcpy(b.data()+ll+1, data, dlen); b[ll+1+dlen] = (uint8_t)i;
    unsigned int l; uint8_t d[20];
    HMAC(EVP_sha1(), key, klen, b.data(), b.size(), d, &l);
    int c = (olen-gen < 20) ? olen-gen : 20; memcpy(out+gen, d, c);
  }
}
static void compute_ptk(devourer::sta::Station& st) {
  const uint8_t *aa = kBssid, *sa = st.addr;
  uint8_t b[76]; int p = 0;
  const uint8_t* mn = memcmp(aa,sa,6) < 0 ? aa : sa;
  const uint8_t* mx = memcmp(aa,sa,6) < 0 ? sa : aa;
  memcpy(b+p, mn, 6); p+=6; memcpy(b+p, mx, 6); p+=6;
  const uint8_t* nn = memcmp(st.anonce,st.snonce,32) < 0 ? st.anonce : st.snonce;
  const uint8_t* nx = memcmp(st.anonce,st.snonce,32) < 0 ? st.snonce : st.anonce;
  memcpy(b+p, nn, 32); p+=32; memcpy(b+p, nx, 32); p+=32;
  uint8_t pmk[32];
  PKCS5_PBKDF2_HMAC(g_psk, strlen(g_psk), (const unsigned char*)kSsid,
                    strlen(kSsid), 4096, EVP_sha1(), 32, pmk);
  prf(pmk, 32, "Pairwise key expansion", b, p, st.ptk, 48);
}
// MIC over the EAPOL frame with the MIC field (offset 81, 16 bytes) zeroed.
static void set_mic(std::vector<uint8_t>& e, const devourer::sta::Station& st) {
  memset(e.data()+81, 0, 16);
  unsigned int l; uint8_t d[20];
  HMAC(EVP_sha1(), st.ptk, 16 /*KCK*/, e.data(), e.size(), d, &l);
  memcpy(e.data()+81, d, 16);
}
static bool check_mic(const uint8_t* e, int len,
                      const devourer::sta::Station& st) {
  std::vector<uint8_t> t(e, e+len);
  uint8_t got[16]; memcpy(got, t.data()+81, 16);
  memset(t.data()+81, 0, 16);
  unsigned int l; uint8_t d[20];
  HMAC(EVP_sha1(), st.ptk, 16, t.data(), t.size(), d, &l);
  return memcmp(got, d, 16) == 0;
}
// AES key wrap (RFC 3394) with the KEK (PTK bytes 16..31), for msg3 key data.
static int aes_wrap(const uint8_t* kek, const uint8_t* in, int inlen, uint8_t* out) {
  EVP_CIPHER_CTX* c = EVP_CIPHER_CTX_new();
  EVP_CIPHER_CTX_set_flags(c, EVP_CIPHER_CTX_FLAG_WRAP_ALLOW);
  EVP_EncryptInit_ex(c, EVP_aes_128_wrap(), NULL, kek, NULL);
  int ol = 0, tmp = 0;
  EVP_EncryptUpdate(c, out, &ol, in, inlen);
  EVP_EncryptFinal_ex(c, out+ol, &tmp); ol += tmp;
  EVP_CIPHER_CTX_free(c);
  return ol;
}

// Build an EAPOL-Key data frame (from-DS) to the station.
static std::vector<uint8_t> eapol_frame(uint16_t keyinfo, const uint8_t* nonce,
                                        const uint8_t* keydata, int kdlen, bool mic,
                                        const devourer::sta::Station& st) {
  std::vector<uint8_t> e(99, 0);
  e[0]=2; e[1]=3;                                       // EAPOL v2, type Key
  int blen = 95 + kdlen; e[2]=blen>>8; e[3]=blen&0xff;
  e[4]=2;                                               // RSN key descriptor
  e[5]=keyinfo>>8; e[6]=keyinfo&0xff;
  e[7]=0; e[8]=16;                                      // key length 16
  memcpy(e.data()+9, st.eapol_replay, 8);
  if (nonce) memcpy(e.data()+17, nonce, 32);
  e[97]=kdlen>>8; e[98]=kdlen&0xff;
  if (keydata && kdlen) e.insert(e.end(), keydata, keydata+kdlen);
  if (mic) set_mic(e, st);
  // wrap in 802.11 data (from-DS) + LLC/SNAP ethertype 0x888e
  // Sequence-numbered like every other data frame. These carry the handshake
  // and a retransmission of one feeds the station's duplicate detector; they
  // were missed when the data planes were fixed.
  std::vector<uint8_t> m = devourer::sta::data_hdr_from_ds(
      st.addr, kBssid, kBssid, /*protect=*/false, g_seq.next());
  devourer::sta::append_llc_snap(m, 0x888e);
  m.insert(m.end(), e.begin(), e.end());
  return m;
}
/* Caller holds g_hs_mu. `first` = generate a fresh ANonce; a retransmission
 * must reuse it or the station's PTK will not match ours. */
static void send_msg1(devourer::sta::Station& st, bool first) {
  if (first) { RAND_bytes(st.anonce, 32); st.hs_tries = 0; }
  for (int i=7;i>=0;--i) if (++st.eapol_replay[i]) break;   // bump replay counter
  enqueue(eapol_frame(0x008a, st.anonce, nullptr, 0, false, st)); // ver2|pair|ack
  st.state = devourer::sta::HsState::WaitMsg2;
  st.hs_tx_ms = now_ms();
  ++st.hs_tries;
  fprintf(stderr, "  WPA2: sent msg1 (ANonce) to %02x:%02x:%02x:%02x:%02x:%02x%s\n",
          st.addr[0],st.addr[1],st.addr[2],st.addr[3],st.addr[4],st.addr[5],
          first ? "" : " [retransmit]");
}
/* Caller holds g_hs_mu.
 *
 * `first` no longer touches the GTK. It used to run RAND_bytes(g_gtk) here,
 * which was correct-looking for one station and wrong for two: the second
 * station's handshake handed it a fresh group key and silently revoked the
 * first station's. The BSS generates its GTK once, before the radio comes up.
 * The replay counter still advances and the MIC is recomputed on every
 * transmission, which is what 802.11-2016 12.7.6.4 asks for. */
static void send_msg3(devourer::sta::Station& st, bool first) {
  if (first) { st.hs_tries = 0; }
  // key data = RSN IE + GTK KDE, padded to /8, then AES-wrapped with the KEK.
  std::vector<uint8_t> kd(rsn_ie().begin(), rsn_ie().end());
  uint8_t gtkkde[24] = {0xdd,0x16,0x00,0x0f,0xac,0x01,0x01,0x00};
  memcpy(gtkkde+8, g_gtk, 16);
  kd.insert(kd.end(), gtkkde, gtkkde+24);
  if (kd.size() % 8) {                                  // 802.11i pad: 0xDD then 0x00s
    kd.push_back(0xdd);
    while (kd.size() % 8) kd.push_back(0x00);
  }
  std::vector<uint8_t> wrapped(kd.size()+8);
  int wl = aes_wrap(st.ptk+16, kd.data(), kd.size(), wrapped.data());
  for (int i=7;i>=0;--i) if (++st.eapol_replay[i]) break;
  enqueue(eapol_frame(0x13ca, st.anonce, wrapped.data(), wl, true, st));  // install|ack|mic|secure|enc
  st.state = devourer::sta::HsState::WaitMsg4;
  st.hs_tx_ms = now_ms();
  ++st.hs_tries;
  fprintf(stderr, "  WPA2: sent msg3 (GTK, MIC) — 4-way in progress%s\n",
          first ? "" : " [retransmit]");
}

/* Called from the main loop. Resends whichever message this side is still
 * waiting on, up to kHsMaxTries transmissions in total. */
static void hs_tick() {
  using devourer::sta::HsState;
  const double timeout_ms =
      std::chrono::duration<double, std::milli>(kHsTimeout).count();
  std::lock_guard<std::mutex> l(g_hs_mu);
  /* Every station retransmits on its own schedule. A single shared deadline
   * would let one station's handshake reset another's timer. */
  for (int i = 0; i < g_stas.capacity(); i++) {
    devourer::sta::Station* st = g_stas.at(i);
    if (!st) continue;
    if (st->state != HsState::WaitMsg2 && st->state != HsState::WaitMsg4) continue;
    if (now_ms() - st->hs_tx_ms < timeout_ms) continue;
    if (st->hs_tries >= kHsMaxTries) {
      if (st->hs_tries == kHsMaxTries) {
        ++st->hs_tries;   // latch, so this prints once per station
        fprintf(stderr, "  WPA2: gave up after %d transmissions of msg%d"
                        " to %02x:%02x:%02x:%02x:%02x:%02x\n",
                kHsMaxTries, st->state == HsState::WaitMsg2 ? 1 : 3,
                st->addr[0],st->addr[1],st->addr[2],
                st->addr[3],st->addr[4],st->addr[5]);
      }
      continue;
    }
    if (st->state == HsState::WaitMsg2) send_msg1(*st, false);
    else                                send_msg3(*st, false);
  }
}

// --- CCMP data plane (software AES-CCM) so the station pings encrypted --------
static const uint8_t kApIp[4] = {192, 168, 99, 1};
// src/sta/Ccmp.h takes its cipher as a vtable so libdevourer stays free of
// OpenSSL. This is the harness's side of that seam, and it routes through
// profiled_ccmp() so the `bench` cell's per-frame timing is unaffected.
struct HarnessCrypto : devourer::sta::CryptoOps {
  bool aes_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
               const uint8_t* aad, size_t aad_len, const uint8_t* in,
               size_t in_len, uint8_t* out, uint8_t* tag) override {
    return profiled_ccmp(encrypt, key, nonce, aad, (int)aad_len, in,
                         (int)in_len, out, tag);
  }
  bool hmac_sha1(const uint8_t*, size_t, const uint8_t*, size_t,
                 uint8_t[20]) override { return false; }
  bool pbkdf2_sha1(const char*, const uint8_t*, size_t, unsigned, uint8_t*,
                   size_t) override { return false; }
  bool aes_key_unwrap(const uint8_t*, size_t, const uint8_t*, size_t,
                      uint8_t*) override { return false; }
};
static HarnessCrypto g_crypto;
// The replay window is now a deployed control rather than a tested fixture.
// It is reset on every fresh PTK install: a new key is a new PN space.
// Data-plane visibility. The one thing the on-air runs could not answer was
// whether encrypted frames were arriving at all, because nothing counted them.
static std::atomic<uint64_t> g_enc_rx{0}, g_mic_fail{0}, g_replayed{0};
/* Frames whose DESTINATION is not this AP. Until Phase 2b.3 nothing read
 * addr3, so every decrypted frame was handed to the local IP responders no
 * matter who it was addressed to - harmless while the AP is the only thing on
 * the BSS worth addressing, and not harmless once there is a second station.
 * Counted rather than relayed: the relay is 2b.7, and a counter that moves is
 * how that gate will be read. */
static std::atomic<uint64_t> g_to_peer{0};      /* DA is another associated station */
static std::atomic<uint64_t> g_to_elsewhere{0}; /* DA is off-BSS entirely */

static uint16_t csum16(const uint8_t* d, int len) {
  uint32_t s = 0; for (int i=0;i+1<len;i+=2) s += (d[i]<<8)|d[i+1];
  if (len&1) s += d[len-1]<<8; while (s>>16) s=(s&0xffff)+(s>>16); return (uint16_t)~s;
}
// The CCMP AAD/nonce/header rules now live in src/sta/Ccmp.h, known-answer
// tested against vectors from a third implementation (ctest ccmp_framing).
// They used to be inline here and in nobody's test.
// Encrypt an AP->STA payload (LLC/SNAP+eth+data) into a CCMP data frame.
/* Caller holds g_hs_mu: every path into here runs inside the RX callback's
 * data branch, which takes the lock to look the station up in the first
 * place. An unkeyed or unknown destination emits nothing rather than airing a
 * frame under someone else's key. */
static std::vector<uint8_t> ccmp_tx(const uint8_t* sta, uint16_t eth,
                                    const uint8_t* pl, int plen) {
  devourer::sta::Station* st = g_stas.find(sta);
  if (!st || !st->keyed()) return {};
  std::vector<uint8_t> pt = {0xaa,0xaa,0x03,0,0,0,(uint8_t)(eth>>8),(uint8_t)(eth&0xff)};
  pt.insert(pt.end(), pl, pl+plen);
  // The data plane carries a sequence number now. The management-frame fix was
  // the visible half; THIS is the path that feeds a peer's duplicate detector
  // in volume, and it was still pinned at 0. Built by the shared helper so the
  // AP's downlink and a station's uplink cannot disagree about the layout.
  std::vector<uint8_t> hdr = devourer::sta::data_hdr_from_ds(
      sta, kBssid, kBssid, /*protect=*/true, g_seq.next());
  uint64_t pn = st->tx_pn++;   /* one PN space per station */
  // Non-QoS AAD (qos_tid defaults to -1): this AP airs plain data frames, and
  // that is what has been validated on air. A station sending QoS data needs
  // the other form - the two are not interchangeable, and ctest ccmp_framing
  // asserts that a frame built under one does not verify under the other.
  std::vector<uint8_t> m(devourer::sta::ccmp_encrypted_len(hdr.size(),
                                                           pt.size()));
  size_t n = devourer::sta::ccmp_encrypt(g_crypto, st->ptk + 32, hdr.data(),
                                         hdr.size(), kBssid, pn, 0, pt.data(),
                                         pt.size(), m.data(), m.size());
  // A zero return means the cipher refused. The old code ignored the result
  // and aired a frame with an uninitialised MIC; emitting nothing is the
  // honest failure, and the caller drops an empty vector.
  m.resize(n);
  return m;
}
// DHCP OFFER/ACK payload (IP+UDP+BOOTP) leasing 192.168.99.2 — encrypted by ccmp_tx.
static const uint8_t kLeaseIp[4] = {192,168,99,2};
static std::vector<uint8_t> dhcp_payload(const uint8_t* sta, const uint8_t* xid, uint8_t mt) {
  std::vector<uint8_t> b(236, 0);
  b[0]=2; b[1]=1; b[2]=6; memcpy(&b[4],xid,4);
  memcpy(&b[16],kLeaseIp,4); memcpy(&b[20],kApIp,4); memcpy(&b[28],sta,6);
  const uint8_t opt[]={0x63,0x82,0x53,0x63, 53,1,mt, 54,4,kApIp[0],kApIp[1],kApIp[2],kApIp[3],
      51,4,0,1,0x51,0x80, 1,4,255,255,255,0, 3,4,kApIp[0],kApIp[1],kApIp[2],kApIp[3],
      6,4,kApIp[0],kApIp[1],kApIp[2],kApIp[3], 255};
  b.insert(b.end(), opt, opt+sizeof(opt));
  int ul=8+(int)b.size();
  std::vector<uint8_t> pl(28,0);
  pl[0]=0x45; int tot=20+ul; pl[2]=tot>>8; pl[3]=tot&0xff; pl[8]=64; pl[9]=17;
  memcpy(&pl[12],kApIp,4); pl[16]=pl[17]=pl[18]=pl[19]=0xff;
  uint16_t ic=csum16(pl.data(),20); pl[10]=ic>>8; pl[11]=ic&0xff;
  pl[20]=0; pl[21]=67; pl[22]=0; pl[23]=68; pl[24]=ul>>8; pl[25]=ul&0xff;
  pl.insert(pl.end(), b.begin(), b.end());
  return pl;
}
// Handle a decrypted L2 payload (LLC/SNAP + eth): answer ARP + ICMP + DHCP, encrypted.
static void handle_plain(const uint8_t* sta, const uint8_t* d, int len) {
  if (len < 8 || d[0]!=0xaa) return;                    // not LLC/SNAP (e.g. IPv6 ND)
  uint16_t eth = (d[6]<<8)|d[7]; const uint8_t* pl = d+8; int pllen = len-8;
  if (eth==0x0806 && pllen>=28) {                        // ARP
    if (((pl[6]<<8)|pl[7])==1 && memcmp(pl+24,kApIp,4)==0) {
      uint8_t a[28]={0,1,8,0,6,4,0,2, kBssid[0],kBssid[1],kBssid[2],kBssid[3],kBssid[4],kBssid[5],
        kApIp[0],kApIp[1],kApIp[2],kApIp[3], pl[8],pl[9],pl[10],pl[11],pl[12],pl[13],
        pl[14],pl[15],pl[16],pl[17]};
      enqueue(ccmp_tx(sta,0x0806,a,28));
    }
  } else if (eth==0x0800 && pllen>=28) {                 // IPv4/ICMP
    const uint8_t* ip=pl; int ihl=(ip[0]&0x0f)*4;
    if (ip[9]==1 && pllen>=ihl+8 && memcmp(ip+16,kApIp,4)==0 && ip[ihl]==8) {  // ICMP echo
      std::vector<uint8_t> r(pl, pl+pllen);
      memcpy(r.data()+12,kApIp,4); memcpy(r.data()+16,ip+12,4);
      r[10]=r[11]=0; uint16_t ic=csum16(r.data(),ihl); r[10]=ic>>8; r[11]=ic&0xff;
      r[ihl]=0; r[ihl+2]=r[ihl+3]=0;
      uint16_t cc=csum16(r.data()+ihl,pllen-ihl); r[ihl+2]=cc>>8; r[ihl+3]=cc&0xff;
      enqueue(ccmp_tx(sta,0x0800,r.data(),pllen));
    } else if (ip[9]==17 && pllen>=ihl+8+240) {          // UDP -> DHCP
      const uint8_t* udp=ip+ihl;
      if (((udp[2]<<8)|udp[3])==67) {
        const uint8_t* dh=udp+8; const uint8_t* end=pl+pllen; uint8_t m=0;
        for (const uint8_t* o=dh+240; o+1<end && *o!=0xff; ) {
          if (*o==0){o++;continue;} if (*o==53 && o+2<end) m=o[2]; o+=2+o[1]; }
        uint8_t reply = (m==1)?2 : (m==3)?5 : 0;
        if (reply) { auto dp=dhcp_payload(sta, dh+4, reply);
          enqueue(ccmp_tx(sta,0x0800,dp.data(),(int)dp.size())); }
      }
    }
  }
}

static void on_rx(const Packet& p) {
  /* NOT p.Data.size(): on every Realtek generation that buffer still carries
   * the four trailing FCS bytes, and feeding that length to a CCMP decrypt
   * puts the expected MIC four bytes late so EVERY frame fails to
   * authenticate - which this harness's ledger would then report as MIC
   * failures, i.e. as an attack. See tests/rx_mpdu.h. */
  const size_t mlen = devourer::test::mpdu_len(p);
  if (mlen < 24 || p.RxAtrib.crc_err) return;
  const uint8_t fc0 = p.Data[0], fc1 = p.Data[1];
  const uint8_t* a1 = p.Data.data() + 4;
  const uint8_t* sta = p.Data.data() + 10;
  bool to_us = std::memcmp(a1, kBssid, 6) == 0;
  bool bcast = (a1[0] & 0x01) != 0;

  if (fc0 == 0x40) {                                    // probe-req
    if (!bcast && !to_us) return;
    auto m = mgmt_hdr(0x50, sta);
    m.insert(m.end(), {0,0,0,0,0,0,0,0, 0x64,0x00, 0x11,0x00});  // cap: ESS+Privacy
    append_ies(m, true); enqueue(std::move(m));
  } else if (fc0 == 0xb0 && to_us) {                    // auth
    auto m = mgmt_hdr(0xb0, sta);
    m.insert(m.end(), {0,0, 0x02,0x00, 0,0}); enqueue(std::move(m));
    fprintf(stderr, "  AUTH from %02x:%02x:%02x:%02x:%02x:%02x\n",
            sta[0],sta[1],sta[2],sta[3],sta[4],sta[5]);
  } else if ((fc0 == 0x00 || fc0 == 0x20) && to_us) {   // (re)assoc
    /* Allocate BEFORE answering, because the association response has to carry
     * the AID we allocated. It used to hardcode `0x01,0xc0` - AID 1 - which
     * was true by accident while the AP served one station and tells every
     * station it is AID 1 now that it serves several. The AID is what a TIM
     * bitmap indexes, so two stations sharing one is not cosmetic the moment
     * power save stops being off by policy.
     *
     * One lock for the whole branch. enqueue() takes g_q_mu underneath it,
     * which is the same g_hs_mu -> g_q_mu order hs_tick uses. */
    std::lock_guard<std::mutex> l(g_hs_mu);
    /* add() returns the existing record for a re-association, so a station
     * that loops back through assoc restarts its handshake instead of
     * consuming a second AID. */
    devourer::sta::Station* st = g_stas.add(sta);
    if (!st) {
      fprintf(stderr, "  ASSOC refused: the station table is full (%d)\n",
              g_stas.capacity());
      return;
    }
    auto m = mgmt_hdr(0x10, sta);
    const uint16_t aid_field = (uint16_t)(0xc000 | st->aid);   /* AID | the two reserved top bits */
    m.insert(m.end(), {0x11,0x00, 0x00,0x00,
                       (uint8_t)(aid_field & 0xff), (uint8_t)(aid_field >> 8)});
    append_ies(m, false); enqueue(std::move(m));
    fprintf(stderr, "  ASSOC from %02x:%02x:%02x:%02x:%02x:%02x (aid=%u) -> start 4-way\n",
            sta[0],sta[1],sta[2],sta[3],sta[4],sta[5], st->aid);
    memset(st->eapol_replay, 0, 8);
    st->state = devourer::sta::HsState::Idle;
    send_msg1(*st, true);
  } else if ((fc0 == 0xc0 || fc0 == 0xa0) && to_us) {   // deauth / disassoc
    /* WITHOUT THIS THE TABLE ONLY EVER GROWS. StationTable::remove() had no
     * caller outside its own selftest, so seven distinct addresses filled the
     * table permanently and every station after them got "the station table is
     * full". That is not an attack - Android randomises its MAC per network by
     * default, so it is ordinary client behaviour - and it was an effective
     * regression: before the table, an eighth station simply overwrote the
     * single g_sta.
     *
     * Freeing the record also wipes its key material, which is the other half
     * of what a deauth should mean. */
    std::lock_guard<std::mutex> l(g_hs_mu);
    if (g_stas.remove(sta))
      fprintf(stderr, "  %s from %02x:%02x:%02x:%02x:%02x:%02x -> slot freed (%d left)\n",
              fc0 == 0xc0 ? "DEAUTH" : "DISASSOC",
              sta[0],sta[1],sta[2],sta[3],sta[4],sta[5], g_stas.count());
  } else if ((fc0 == 0x08 || devourer::sta::is_qos_data(fc0)) &&
             (fc1 & 0x01) && to_us) {                   // data to-DS
    // data_hdr_len(), not `fc0 == 0x88`: QoS Null (0xc8) is a frame real
    // stations send, and an exact test gives it a 24-byte header. The TID read
    // below and the AAD would then both come from the wrong offset - and with
    // is_qos_data() used for the TID and an exact test for the length, the two
    // would actively disagree.
    int hlen = (int)devourer::sta::data_hdr_len(fc0, fc1);
    // The lock is held across the whole protected-data branch: it guards the
    // table the sender is looked up in, the key the frame is decrypted with,
    // that station's replay window, and - through handle_plain -> ccmp_tx -
    // its TX PN. hs_tick() takes the same lock and then enqueues, so the
    // order is always g_hs_mu before g_q_mu and there is no inversion.
    std::unique_lock<std::mutex> dl(g_hs_mu, std::defer_lock);
    // Station::keyed() is WaitMsg4 or Done, NOT Done alone: the PTK exists
    // once msg3 has been sent, and a station that received msg3 installs its
    // keys and can put protected data on the air before its msg4 reaches us.
    // Gating on the completed handshake dropped those frames.
    devourer::sta::Station* sender = nullptr;
    if (fc1 & 0x40) { dl.lock(); sender = g_stas.find(sta); }
    if ((fc1 & 0x40) && sender && sender->keyed()) {    // PROTECTED (CCMP) data
      int len = (int)mlen;
      if (len < hlen + 8 + 8) return;                   // hdr + CCMP hdr + MIC
      const uint8_t* d = p.Data.data();
      // The header length is passed explicitly, so a QoS frame's AAD includes
      // its TID as 802.11-2016 12.5.3.3.3 requires. This is a deliberate
      // CORRECTNESS change, not byte-identity: the previous code folded the
      // QoS Control field out and built a 22-byte AAD for every frame, which
      // is wrong for QoS and survived only because the validated runs used a
      // station that associated legacy and sent non-QoS data. A real 802.11n
      // station sends QoS, and every one of its frames would have failed the
      // MIC with no diagnostic at either end.
      std::vector<uint8_t> pt(len);
      size_t ptlen = 0;
      uint64_t pn = 0;
      if (devourer::sta::ccmp_decrypt(g_crypto, sender->ptk + 32, d, (size_t)len,
                                      (size_t)hlen, sta, pt.data(), &ptlen,
                                      &pn)) {
        // Replay check, AFTER the MIC verifies and never before: admitting a
        // PN from an unauthenticated frame would let anyone advance the window
        // and lock out the real peer. The TID comes from the QoS header when
        // there is one, because 802.11 keeps one counter per TID.
        //
        // The QoS Control field is at offset 24 for a 3-address frame and 30
        // for a 4-address one - ccmp_aad() in the same module already encodes
        // that, and this site did not: on a 4-address QoS frame it read
        // addr4[0] and used the low nibble of an ADDRESS as the TID. Not a
        // replay bypass (addr4 is authenticated, so a replay maps to the same
        // wrong window and is still refused) but it pollutes another TID's
        // window and can drop legitimate frames. These BSSes air 3-address
        // frames only, so it was latent.
        const bool four_addr =
            (fc1 & (devourer::sta::kFcToDs | devourer::sta::kFcFromDs)) ==
            (devourer::sta::kFcToDs | devourer::sta::kFcFromDs);
        const size_t qoff = four_addr ? 30 : 24;
        const int tid = devourer::sta::is_qos_data(fc0)
                            ? (d[qoff] & 0x0f)
                            : devourer::sta::CcmpReplay::kNonQosTid;
        if (sender->rx_replay.accept(pn, tid)) {
          /* WHO IS THIS FOR? addr3 on a to-DS frame, which nothing in this
           * tree read before 2b.3. A group DA - a station's broadcast ARP, its
           * DHCP DISCOVER - still reaches the local responders, because those
           * are exactly the requests this AP answers. */
          const uint8_t* da = devourer::sta::data_da(d, fc1);
          if (devourer::sta::data_da_is_group(d, fc1) ||
              std::memcmp(da, kBssid, 6) == 0) {
            handle_plain(sta, pt.data(), (int)ptlen);    // decrypted -> ARP/ICMP
          } else if (g_stas.find(da)) {
            /* Destined for another station on this BSS. 2b.7 will relay it;
             * for now it is dropped, which is what the old code did too - it
             * just did it by accident, inside responders that ignore anything
             * not addressed to the AP's own IP. */
            g_to_peer.fetch_add(1);
          } else {
            g_to_elsewhere.fetch_add(1);
          }
        } else {
          g_replayed.fetch_add(1);
        }
      } else {
        g_mic_fail.fetch_add(1);
      }
      g_enc_rx.fetch_add(1);
      return;
    }
    /* RELEASE BEFORE FALLING THROUGH. A protected frame from a station we
     * hold no key for skips the block above with the lock still held, and the
     * EAPOL path below takes g_hs_mu itself. std::mutex is not recursive, so
     * keeping it here self-deadlocks on any ciphertext whose first eight
     * bytes happen to look like an EAPOL LLC/SNAP header. Unlikely, reachable,
     * and introduced by the Phase 2b.2 rewiring - caught in self-review
     * because the on-air gate could not run. */
    if (dl.owns_lock()) dl.unlock();
    if ((int)mlen < hlen + 8) return;
    const uint8_t* llc = p.Data.data() + hlen;
    if (!(llc[0]==0xaa && llc[6]==0x88 && llc[7]==0x8e)) return;  // EAPOL
    const uint8_t* e = llc + 8; int elen = (int)mlen - (hlen + 8);
    if (elen < 99 || e[1] != 3) return;                 // EAPOL-Key
    uint16_t ki = (e[5]<<8) | e[6];
    if ((ki & 0x0008) && (ki & 0x0100) && !(ki & 0x0040) && !(ki & 0x0200)) {
      // msg2: pairwise + MIC, no install/secure -> SNonce + MIC
      std::lock_guard<std::mutex> l(g_hs_mu);
      devourer::sta::Station* st = g_stas.find(sta);
      // Not associated, or not waiting on msg2: a duplicate msg2 must not
      // re-run this, and an EAPOL frame from a station with no record is not
      // a handshake at all.
      if (!st || st->state != devourer::sta::HsState::WaitMsg2) return;
      /* THE KEY REPLAY COUNTER, CHECKED. 802.11-2016 12.7.6.3: msg2 must echo
       * the counter this AP put in msg1. Until now neither this branch nor the
       * msg4 one read the field at all (it is at e+9..e+16, which is where
       * eapol_frame writes it) - acceptance rested on the key-info bits and
       * the MIC alone. The comment above this handshake claims the counter
       * lets copies be told apart; that was only ever half-wired, because the
       * station could and this AP could not. Pre-existing, not introduced by
       * the per-station rewiring: the single-g_sta code did not check it
       * either. */
      if (memcmp(e+9, st->eapol_replay, 8) != 0) {
        fprintf(stderr, "  WPA2: msg2 key replay counter mismatch - dropped\n");
        return;
      }
      memcpy(st->snonce, e+17, 32);
      compute_ptk(*st);
      if (!check_mic(e, elen, *st)) { fprintf(stderr, "  WPA2: msg2 MIC FAIL\n"); return; }
      fprintf(stderr, "  WPA2: msg2 OK (SNonce, MIC verified) — PTK derived\n");
      // The PN space belongs to the KEY, so the replay window resets where a
      // new PTK is derived - here - and nowhere else. It used to reset on
      // msg4, which is a MIC-only cleartext frame an attacker can capture and
      // replay at will: the MIC still verifies under the same PTK, the window
      // resets mid-session, and every captured data frame becomes admissible
      // again. That defeats the control entirely. A legitimate msg4
      // retransmission did the same thing by accident.
      st->rx_replay.reset();
      st->tx_pn = 1;          // a new key is a new PN space, both directions
      send_msg3(*st, true);
    } else if ((ki & 0x0100) && (ki & 0x0200)) {        // msg4: MIC + secure
      std::lock_guard<std::mutex> l(g_hs_mu);
      devourer::sta::Station* st = g_stas.find(sta);
      /* 12.7.6.5: msg4 must echo msg3's counter. Same gap as msg2 above. */
      if (st && memcmp(e+9, st->eapol_replay, 8) != 0) {
        fprintf(stderr, "  WPA2: msg4 key replay counter mismatch - dropped\n");
        return;
      }
      if (st && st->state == devourer::sta::HsState::WaitMsg4 &&
          check_mic(e, elen, *st)) {
        st->state = devourer::sta::HsState::Done;
        fprintf(stderr, "  WPA2: msg4 OK — 4-WAY HANDSHAKE COMPLETE (station keyed)\n");
      }
    }
  }
}

int main(int argc, char** argv) {
  int sec = argc > 1 ? atoi(argv[1]) : 60;
  {   /* before the radio: a wiring check that needs one is no check at all */
    std::vector<uint8_t> b, pr;
    append_ies(b, true, /*beacon=*/true);
    append_ies(pr, true);
    if (!tim_wiring_ok(b, pr)) return 1;
  }
  /* One GTK for the BSS, before the radio comes up. Generating it inside the
   * four-way - which is what this harness did until Phase 2b.2 - hands the
   * second station a fresh group key and revokes the first station's. */
  RAND_bytes(g_gtk, 16);
  if (const char* c = std::getenv("DEVOURER_CHANNEL")) g_chan = (uint8_t)atoi(c);
  if (const char* k = std::getenv("DEVOURER_WPA2_PSK")) g_psk = k;
  if (const char* p = std::getenv("DEVOURER_CCMP_PROFILE"))
    g_ccmp_profile = std::strcmp(p, "0") != 0;
  auto logger = std::make_shared<Logger>(); apply_logging_env(*logger);
  libusb_context* ctx = nullptr; libusb_init(&ctx);
  libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
  static const uint16_t pids[] = {0xc812};
  auto* h = open_selected_usb(ctx, logger, pids, 1);
  if (!h) return 1;
  std::shared_ptr<devourer::UsbDeviceLock> lk;
  if (devourer::claim_interface_then_reset(h, devourer::find_wifi_interface(h), logger, true, lk) != 0) return 1;
  WiFiDriver wifi(logger);
  auto dev = wifi.CreateRadio(h, ctx, lk, devourer_config_from_env());
  g_dev = dev.get(); if (!g_dev) return 1;
  g_rt = devourer::build_stream_radiotap(devourer::parse_tx_mode_str("6M"));
  g_dev->InitWrite(SelectedChannel{g_chan, 0, CHANNEL_WIDTH_20});
  int tu = 25; if (const char* i = std::getenv("DEVOURER_BCN_TU")) tu = atoi(i);
  std::vector<uint8_t> bcn = {0,0,0x0a,0,0,0x80,0,0,0x08,0,
      0x80,0,0,0, 0xff,0xff,0xff,0xff,0xff,0xff,
      kBssid[0],kBssid[1],kBssid[2],kBssid[3],kBssid[4],kBssid[5],
      kBssid[0],kBssid[1],kBssid[2],kBssid[3],kBssid[4],kBssid[5],
      0,0, 0,0,0,0,0,0,0,0, (uint8_t)(tu&0xff),(uint8_t)(tu>>8), 0x11,0x00};
  append_ies(bcn, true, /*beacon=*/true);
  bool bok = g_dev->StartBeacon(bcn.data(), bcn.size(), tu);
  std::thread rx([&]{ g_dev->StartRxLoop(on_rx); });
  fprintf(stderr, "ap_wpa2 up: SSID %s WPA2-PSK '%s' ch%d beacon=%s\n",
          kSsid, g_psk, g_chan, bok ? "OK" : "FAIL");
  auto end = std::chrono::steady_clock::now() + std::chrono::seconds(sec);
  while (std::chrono::steady_clock::now() < end) {
    hs_tick();                                   // 4-way retransmissions
    std::vector<std::vector<uint8_t>> batch;
    { std::lock_guard<std::mutex> l(g_q_mu); batch.swap(g_q); }
    for (auto& f : batch) if (g_dev->send_packet(f.data(), f.size())) g_sent.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> l(g_hs_mu);
    fprintf(stderr, "sent=%llu stations=%d", (unsigned long long)g_sent.load(),
            g_stas.count());
    for (int i = 0; i < g_stas.capacity(); i++)
      if (devourer::sta::Station* st = g_stas.at(i))
        fprintf(stderr, " [aid=%u %02x:%02x:%02x:%02x:%02x:%02x 4way_state=%d]",
                st->aid, st->addr[0],st->addr[1],st->addr[2],
                st->addr[3],st->addr[4],st->addr[5], (int)st->state);
    fprintf(stderr, "\n");
  }
  if (g_ccmp_profile) {
    fprintf(stderr,
            "{\"ev\":\"ccmp.profile\",\"path\":\"software\","
            "\"tx_frames\":%llu,\"tx_bytes\":%llu,\"tx_ns\":%llu,"
            "\"rx_frames\":%llu,\"rx_bytes\":%llu,\"rx_ns\":%llu}\n",
            (unsigned long long)g_ccmp_tx_frames.load(),
            (unsigned long long)g_ccmp_tx_bytes.load(),
            (unsigned long long)g_ccmp_tx_ns.load(),
            (unsigned long long)g_ccmp_rx_frames.load(),
            (unsigned long long)g_ccmp_rx_bytes.load(),
            (unsigned long long)g_ccmp_rx_ns.load());
  }
  /* The data-plane ledger. The on-air runs could not tell an AP that never
   * RECEIVED an encrypted frame from one that received and failed to decrypt
   * them, because nothing counted either - so a 100%-ping-loss result had no
   * diagnosis attached. Printed unconditionally, at every exit. */
  /* CAVEAT for a Realtek AP: Packet::Data carries a trailing FCS whenever
   * RxAttrib.fcs_present is set, which every Realtek generation sets and
   * MT7612U clears. on_rx does not trim it, so on Realtek every protected
   * frame is 4 bytes long and would be counted here as a MIC failure rather
   * than as the length bug it is. Fix the trim before trusting this ledger on
   * anything but MediaTek. */
  fprintf(stderr,
          "  addressing: to this AP=%llu, to a peer station=%llu"
          " (relay is 2b.7), off-BSS=%llu\n",
          (unsigned long long)(g_enc_rx.load() - g_to_peer.load()
                               - g_to_elsewhere.load()),
          (unsigned long long)g_to_peer.load(),
          (unsigned long long)g_to_elsewhere.load());
  fprintf(stderr,
          "  data plane: encrypted frames received=%llu, MIC failures=%llu, "
          "replays rejected=%llu, frames sent=%llu\n",
          (unsigned long long)g_enc_rx.load(),
          (unsigned long long)g_mic_fail.load(),
          (unsigned long long)g_replayed.load(),
          (unsigned long long)g_sent.load());

  /* Retried, and the failure reported. StopBeacon can now genuinely fail (an
   * EP0 stall during teardown), IRadio.h says such a failure "must be retried
   * ... before its shared port is reused", and `_exit(0)` below means there is
   * no destructor coming to try again. A beacon that survives here survives
   * the process. */
  if (g_dev) {
    bool silenced = false;
    for (int i = 0; i < 3 && !silenced; ++i) silenced = g_dev->StopBeacon();
    if (!silenced)
      fprintf(stderr, "WARNING: the beacon could not be stopped - it is still "
                      "airing; power-cycle the adapter\n");
  }
  _exit(0);
}
