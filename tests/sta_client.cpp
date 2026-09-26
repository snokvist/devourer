/* sta_client.cpp — devourer as an 802.11 infrastructure STATION.
 *
 * The mirror of tests/ap_responder.cpp and tests/ap_wpa2.cpp: where those
 * serve a BSS, this one JOINS one. Scan, authenticate, associate, run the
 * WPA2-PSK four-way as the supplicant, and carry encrypted data to and from
 * the host through a TAP device.
 *
 * THE ACCEPTANCE PROPERTY OF THIS FILE IS THAT IT CONTAINS NO BACKEND BRANCH.
 * docs/station-mode-plan.md states it as the gate: "the station harness must
 * contain no backend branch ... a Realtek station arm must therefore be *only*
 * an IRadio method implementation, never a change to the harness. If a later
 * backend forces a harness edit, the seam was wrong and the gate that passed
 * it failed." So there is no `if (mediatek)` anywhere below, and the two
 * places where the silicon genuinely differs are handled through the library:
 *
 *   - the trailing FCS, via tests/rx_mpdu.h (Realtek delivers one, MT7612U
 *     does not). Getting this wrong is not a four-byte cosmetic error - it
 *     moves the expected MIC four bytes late, so EVERY protected frame fails
 *     to authenticate and the ledger below reports an attack.
 *   - the station identity, via IRadio::SetStationIdentity, gated on
 *     AdapterCaps::station_mode_ok rather than on a chip test.
 *
 * WHAT THIS OWNS THAT src/sta/ DELIBERATELY DOES NOT. Phase 3 shipped the
 * station logic with no radio, no clock and no sockets, and carried three
 * things forward for this file to answer. They are answered here and nowhere
 * else, which is the point:
 *
 *   - THE SCANNER. BssTable parses beacons and ranks them; nothing wires it
 *     to StationSm, because that needs channels and dwell times. scan_step()
 *     is that wiring.
 *   - THE RECONNECT POLICY. StationSm notices beacon loss and fails; it does
 *     not re-join, because WHEN to retry is the integrator's decision and not
 *     the protocol's. supervise() is this harness's answer, and it is a
 *     policy, not a law.
 *   - THE DATA PLANE. CCMP framing is library code; which key, which PN space
 *     and which replay window is a per-association decision this file makes.
 *
 * THE STATION'S OWN ADDRESS IS NOT A CHOICE. On MT7612U the auto-response
 * engine matches a received frame's address 1 against MT_MAC_ADDR, so a
 * station that transmits from any address other than the one MAC bring-up
 * programmed is DEAF - measured at 103 frames -> 0 in src/mt7612u/station.cpp.
 * `own` therefore comes from GetPermanentMacAddress and is never invented.
 *
 * AND THE RECEIVE PATH IS PROMISCUOUS. Mt7612uRadio::StartRxLoop calls
 * mt7612u_set_monitor_rx() unconditionally, so most of a busy channel arrives
 * here and StationSm::on_rx is the only address filter in the system. That is
 * why it counts rx_not_our_bss / rx_not_for_us / rx_ignored / rx_malformed,
 * and why those counters are printed at every exit: they are the difference
 * between "the AP never answered" and "we never heard the AP at all".
 *
 * Build (ctest builds this as StaClientSelftest; the on-air script builds it
 * the same way tests/mt7612u_ap_onair.sh builds its binaries):
 *   g++ -std=c++20 -O2 -Isrc -Itests -Iexamples/common tests/sta_client.cpp \
 *     examples/common/env_config.cpp examples/common/usb_select.cpp \
 *     build/libdevourer.a $(pkg-config --cflags --libs libusb-1.0) \
 *     -lcrypto -lpthread -o build/sta_client
 * Run:
 *   sudo DEVOURER_VID=0x0e8d DEVOURER_PID=0x7612 DEVOURER_CHANNEL=6 \
 *        DEVOURER_STA_SSID=devourerAP DEVOURER_STA_PSK=devourer123 \
 *        DEVOURER_STA_TAP=dvsta0 build/sta_client 60
 * Headless:
 *   build/sta_client --self-test        (no device, no root, no airtime)
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <net/if_arp.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <libusb.h>
#include <openssl/rand.h>

#include "RadiotapBuilder.h"
#include "RxPacket.h"
#include "SelectedChannel.h"
#include "TxMode.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"
#include "env_config.h"
#include "logger.h"
#include "openssl_crypto_ops.h"
#include "rx_mpdu.h"
#include "sta/BssTable.h"
#include "sta/Ccmp.h"
#include "sta/Dot11.h"
#include "sta/StationSm.h"
#include "usb_select.h"

namespace {

using devourer::sta::BssEntry;
using devourer::sta::BssTable;
using devourer::sta::StationSm;

/* ---- configuration ----------------------------------------------------- */

std::string g_ssid = "devourerAP";
std::string g_psk;                 /* empty means an OPEN network */
uint8_t g_chan = 6;
/* The channels scan_step() sweeps while unassociated. Defaults to the one
 * configured channel, which is what the bench uses and what the scope
 * document calls "skip it on a configured channel+BSSID"; a list turns this
 * into a real sweep. Never swept while associated - retuning under a live
 * association is how a station loses one. */
std::vector<uint8_t> g_scan_chans;
uint32_t g_scan_dwell_ms = 250;
/* Reconnect is ON by default because an FPV link that gives up on the first
 * lost beacon is useless. The `reconnect` cell of the on-air harness turns
 * the AP off and back on and reads this. */
bool g_reconnect = true;
uint32_t g_rejoin_backoff_ms = 1000;
bool g_ccmp_profile = false;

/* ---- the radio side, which the headless cells never touch --------------- */

IRadio* g_dev = nullptr;
std::vector<uint8_t> g_rt;
std::mutex g_q_mu;
std::vector<std::vector<uint8_t>> g_q;
std::atomic<uint64_t> g_sent{0}, g_send_fail{0}, g_q_drop{0};

/* ---- the station core, under one mutex --------------------------------- */

std::mutex g_mu;
/* Defined below, next to the profiling counters it feeds. Declared here
 * because g_crypto IS one: it was a plain OpenSslCryptoOps until the first
 * bench run reported ccmp_tx_ns_per_frame=0 over 3365 encrypted round trips
 * - the profiling subclass existed and nothing ever instantiated it. */
struct ProfilingCrypto;
BssTable g_bss;
StationSm g_sm;
uint8_t g_own[6] = {0};
devourer::sta::SeqCounter g_data_seq;

/* THE TRANSMIT PN SPACE IS PER-ASSOCIATION, and it starts at 1 because PN 0
 * is never valid (CcmpReplay refuses it, and so does every conforming peer).
 * Reset in on_association() rather than at startup: a second association
 * derives a new PTK, and reusing a PN under a new key is not a replay
 * problem, it is a keystream-reuse problem. */
uint64_t g_tx_pn = 1;
devourer::sta::CcmpReplay g_rx_replay;       /* pairwise, per TID */
/* 802.11 duplicate detection for the AP's unicast frames: a retransmission a
 * lost ACK caused is dropped before decrypt and counted as a duplicate, not a
 * replay. Group frames are never retried and would clobber the cache. */
devourer::sta::DupDetector g_rx_dup;
std::atomic<uint64_t> g_dup_drop{0};
devourer::sta::CcmpReplay g_group_replay;    /* the GTK's own PN space */
/* WHICH KEYS THIS PN STATE BELONGS TO. Generations rather than copies of the
 * key: the supplicant already holds both, and a harness keeping its own copy
 * of the pairwise key is one more place for it to leak from. */
uint32_t g_ptk_gen_seen = 0;
uint32_t g_gtk_gen_seen = 0;

/* the scan/join policy's own state */
size_t g_scan_idx = 0;
uint32_t g_scan_switch_ms = 0;
uint32_t g_next_join_ms = 0;
int g_join_attempts = 0;
bool g_gave_up = false;
/* Entering Failed is an EVENT, and the supervisor is called on every loop
 * iteration - so without a latch it would count one lost link as hundreds of
 * reconnect attempts and re-arm the backoff on every pass. */
bool g_failed_noted = false;
/* And a SECOND latch, because the first one is cleared by every join attempt
 * so that a later loss can be noticed at all. Without this, every failed
 * retry while the AP is off counts as another lost link: an on-air run that
 * dropped the link exactly ONCE reported reconnects=6, which is the number
 * of times it TRIED, not the number of times it lost anything. */
bool g_was_associated = false;

/* ---- the ledger --------------------------------------------------------- */

std::atomic<uint64_t> g_beacons{0}, g_probe_tx{0};
std::atomic<uint64_t> g_joins{0}, g_associations{0}, g_reconnects{0};
std::atomic<uint64_t> g_enc_rx{0}, g_mic_fail{0}, g_replays{0};
std::atomic<uint64_t> g_group_rx{0}, g_plain_rx{0}, g_rx_short{0};
/* ONE COUNTER PER DIRECTION, because one counter for both cannot state
 * either books. `g_tap_drop` used to serve the radio->host path (a frame the
 * host cannot be given) AND the host->radio path (not connected, foreign
 * source, cipher refused), so "did everything the host handed us go somewhere
 * named?" was not a question the ledger could answer: the sum it needed
 * included drops from the opposite direction. Found by review, after a
 * harness asserted that identity and it held only because both of the
 * confounding terms happened to be zero. */
std::atomic<uint64_t> g_tap_tx{0}, g_tap_rx{0}, g_tap_drop{0},
    g_tap_down_drop{0};
/* Every frame handed to enqueue(), so the transmit chain closes:
 * queued == aired + queue dropped + send failed. Without it the ledger has
 * the losses but not the total they are losses FROM, and management frames
 * make `encrypted + plaintext` the wrong total. */
std::atomic<uint64_t> g_q_in{0};
/* The periodic counter dump - see the emission site in main(). Zero is off,
 * which is every existing caller, so no figure already recorded moves. */
uint32_t g_tick_ms = 0;
/* DEVOURER_STA_ARM=0: never call SetStationIdentity. A CONTROL, not a mode -
 * it is how the harness shows what the arm itself buys (STA_ARM in
 * tests/sta_d2d_onair.sh): everything else about the run is unchanged. */
bool g_arm = true;
uint32_t g_last_tick = 0;
std::atomic<uint64_t> g_tx_enc{0}, g_tx_enc_fail{0}, g_tx_plain{0};
std::atomic<uint64_t> g_crc_err{0}, g_amsdu_drop{0}, g_frag_drop{0};
/* The group rekey, which rides INSIDE the cipher and so is counted apart from
 * the four-way's cleartext EAPOL. Zero here on a link the AP rekeys is the
 * signature of the defect that made this path exist. */
std::atomic<uint64_t> g_eapol_enc_rx{0}, g_eapol_enc_tx{0};
/* How many times each key has actually been installed. A rekey that the
 * harness fails to notice is invisible in every other counter here - the
 * link stays Connected and the frames simply stop arriving. */
std::atomic<uint64_t> g_ptk_installs{0}, g_gtk_installs{0};
/* Protected frames we hold NO KEY FOR - a group frame at an unadvertised key
 * id, or under a GTK whose length is not CCMP's. Counted apart from a MIC
 * failure because they are a different fact: one says the peer is using a key
 * we were never given, the other says someone is tampering. They also used to
 * be indistinguishable in the mutation sweep, which is how the guard that
 * produces this counter survived one. */
std::atomic<uint64_t> g_no_key{0};
std::atomic<uint64_t> g_ccmp_tx_frames{0}, g_ccmp_tx_ns{0};
std::atomic<uint64_t> g_ccmp_rx_frames{0}, g_ccmp_rx_ns{0};

int g_tap_fd = -1;

/* A CLEAN STOP, because the ledger below IS the diagnostic and a harness
 * that prints nothing when interrupted is no use at all. The on-air script
 * ends a cell by killing this process once it has taken its measurements, and
 * without a handler that meant the run's counters - the only thing that can
 * distinguish "we never heard the AP" from "we heard it and it said no" -
 * were lost exactly when a cell had failed and someone wanted to know why.
 *
 * sig_atomic_t and a bare store: the only thing legal in a handler. */
volatile std::sig_atomic_t g_stop = 0;
extern "C" void on_signal(int) { g_stop = 1; }

/* ---- small helpers ------------------------------------------------------ */

uint32_t now_ms() {
  static const auto t0 = std::chrono::steady_clock::now();
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - t0)
      .count();
}

/* A CryptoOps that times the cipher, so the on-air bench cell can report the
 * per-frame CPU cost the way the AP harness's DEVOURER_CCMP_PROFILE does.
 * Only aes_ccm is overridden; everything else is the base implementation,
 * which is the one complete CryptoOps in the tree. */
struct ProfilingCrypto : devourer::test::OpenSslCryptoOps {
  bool aes_ccm(bool encrypt, const uint8_t key[16], const uint8_t nonce[13],
               const uint8_t* aad, size_t aad_len, const uint8_t* in,
               size_t in_len, uint8_t* out, uint8_t* tag) override {
    /* DEVOURER_CCMP_PROFILE off is the normal path and pays nothing but a
     * predictable branch. */
    if (!g_ccmp_profile)
      return devourer::test::OpenSslCryptoOps::aes_ccm(
          encrypt, key, nonce, aad, aad_len, in, in_len, out, tag);
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = devourer::test::OpenSslCryptoOps::aes_ccm(
        encrypt, key, nonce, aad, aad_len, in, in_len, out, tag);
    const uint64_t ns = (uint64_t)std::chrono::duration_cast<
        std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
        .count();
    if (encrypt) {
      g_ccmp_tx_frames.fetch_add(1, std::memory_order_relaxed);
      g_ccmp_tx_ns.fetch_add(ns, std::memory_order_relaxed);
    } else {
      g_ccmp_rx_frames.fetch_add(1, std::memory_order_relaxed);
      g_ccmp_rx_ns.fetch_add(ns, std::memory_order_relaxed);
    }
    return ok;
  }
};

/* THE ONE the whole harness uses. */
ProfilingCrypto g_crypto;

/* Unicast frames request an ACK by default, so the MT7612U's hardware
 * retries until the AP answers (15 deep) - what any station does. Until
 * 2026-09-26 every frame went NOACK (the stream builder's default) and this
 * station's uplink never retransmitted anything. DEVOURER_STA_ACK=0 restores
 * that, to reproduce the older single-shot figures. Group-addressed frames
 * stay NOACK either way: nobody ACKs them. */
std::vector<uint8_t> g_rt_ack;

void enqueue(std::vector<uint8_t> mpdu) {
  /* addr1's I/G bit: a group address is never ACKed. */
  const bool unicast = mpdu.size() >= 10 && (mpdu[4] & 0x01) == 0;
  const std::vector<uint8_t>& rt = (unicast && !g_rt_ack.empty()) ? g_rt_ack : g_rt;
  std::vector<uint8_t> f;
  f.reserve(rt.size() + mpdu.size());
  f.insert(f.end(), rt.begin(), rt.end());
  f.insert(f.end(), mpdu.begin(), mpdu.end());
  std::lock_guard<std::mutex> lk(g_q_mu);
  g_q_in.fetch_add(1);
  /* Bounded, like the AP's. Everything queued here is produced in response to
   * a received frame or a timer, so an unbounded queue is an unbounded
   * allocation the air controls. */
  if (g_q.size() < 128) g_q.push_back(std::move(f));
  else g_q_drop.fetch_add(1);
}

/* The dBm conversion this tree uses everywhere (src/LinkHealth.cpp,
 * src/RxQuality.h): dBm ~= raw - 110. It is NOT a backend branch - the raw
 * PWDB convention is shared, and MT7612U's mapping layer converts into it
 * (Mt7612uMapping.h's rssi_to_raw adds the same 110).
 *
 * Only the ORDERING is load-bearing here: BssTable uses RSSI to rank BSSes
 * heard by one radio in one scan, so any monotone mapping picks the same
 * winner. A raw 0 means the PHY reported nothing, which must not outrank a
 * real reading - hence the floor rather than -110. */
int8_t rssi_dbm(uint8_t raw) {
  if (raw == 0) return -128;
  const int dbm = (int)raw - 110;
  return (int8_t)(dbm < -128 ? -128 : (dbm > 127 ? 127 : dbm));
}

/* ---- keys and per-association state ------------------------------------- */

/* Called under g_mu when the station reaches Connected on a NEW association.
 *
 * THE PER-KEY STATE IS NOT RESET HERE, and that is the point. It used to be,
 * and "are we connected now" is the wrong event: a PTK or GTK rekey happens
 * with the association already up and the machine already Connected, so a
 * reset hung off this transition misses every one of them. note_keys() owns
 * it, keyed on the supplicant's install generations, and a re-association is
 * simply one more key install. */
void on_association() {
  g_failed_noted = false;
  g_was_associated = true;
  g_associations.fetch_add(1);
}

/* A REKEY RESTARTS A PN SPACE, and the window has to restart with it.
 *
 * Both directions, for both keys. The AP's new key starts at PN 1, so a
 * window left at the old key's head rejects every frame until the PN climbs
 * back within 64 of it - a link that reports itself keyed and carries
 * nothing, which is the hardest failure on this list to diagnose from the
 * outside. And OUR transmit PN under a new key must restart too, because
 * continuing it is not a replay problem, it is keystream reuse.
 *
 * Caller holds g_mu. */
void note_keys() {
  const devourer::sta::Supplicant& sup = g_sm.supplicant();

  if (sup.ptk_valid() && sup.ptk_generation() != g_ptk_gen_seen) {
    g_ptk_gen_seen = sup.ptk_generation();
    g_tx_pn = 1;
    g_rx_replay.reset();
    g_rx_dup.reset();
    g_ptk_installs.fetch_add(1);
  }
  if (sup.gtk_valid() && sup.gtk_len() == 16 &&
      sup.gtk_generation() != g_gtk_gen_seen) {
    g_gtk_gen_seen = sup.gtk_generation();
    g_group_replay.reset();
    g_gtk_installs.fetch_add(1);
  }
}

/* Declared here and defined below with the rest of the transmit path: the
 * receive path needs it for the group rekey's answer, which is encrypted
 * exactly as a data frame is. */
bool air_msdu(const uint8_t* msdu, size_t len, const uint8_t da[6],
              const uint8_t* tk = nullptr);

/* ---- UP: one received MPDU --------------------------------------------- */

/* Hand a decrypted (or never-encrypted) MSDU to the host.
 *
 * Returns false when it was not something the host can be given - a
 * non-ethertype LLC encoding, or a frame too big for the buffer. Caller
 * holds g_mu. */
bool tap_up(const uint8_t* da, const uint8_t* sa, const uint8_t* msdu,
            size_t len) {
  uint8_t eth[2048];
  const size_t n = devourer::sta::msdu_to_eth(da, sa, msdu, len, eth,
                                              sizeof eth);
  if (n == 0) { g_tap_drop.fetch_add(1); return false; }
  if (g_tap_fd < 0) return true;          /* no TAP: counted, not an error */
  if (::write(g_tap_fd, eth, n) == (ssize_t)n) { g_tap_tx.fetch_add(1); return true; }
  g_tap_drop.fetch_add(1);
  return false;
}

/* THE RECEIVE DECISION, with no Packet and no radio in it, so every branch
 * below is reachable from `sta_client --self-test`.
 *
 * `mpdu`/`len` is the true MPDU with no FCS - see rx_mpdu.h for why that
 * sentence is the difference between a working link and a flood of MIC
 * failures on Realtek. */
void rx_frame(const uint8_t* mpdu, size_t len, int8_t rssi, uint32_t now) {
  std::lock_guard<std::mutex> l(g_mu);

  if (len < 24) { g_rx_short.fetch_add(1); return; }

  /* The scan folds in EVERY beacon and probe response on the channel,
   * including ones from BSSes we are not talking to - that is what a scan is.
   * StationSm::on_rx sees the same frame and ignores everything that is not
   * our BSS, so the two do not have to agree about which AP matters. */
  if (mpdu[0] == devourer::sta::kFcBeacon ||
      mpdu[0] == devourer::sta::kFcProbeResp) {
    if (g_bss.observe(mpdu, len, rssi, now)) g_beacons.fetch_add(1);
  }

  const StationSm::State before = g_sm.state();
  g_sm.on_rx(mpdu, len, now);
  if (before != StationSm::State::Connected && g_sm.connected())
    on_association();
  if (g_sm.connected()) note_keys();

  /* Everything past here is the data plane, and it runs only on a live
   * association. A protected frame that arrives before one cannot be
   * decrypted with a key we do not have. */
  if (!g_sm.connected()) return;

  const uint8_t fc0 = mpdu[0], fc1 = mpdu[1];
  if (fc0 != devourer::sta::kFcData && !devourer::sta::is_qos_data(fc0)) return;
  /* A station receives from the DS. A frame with ToDS set is another
   * station's uplink that our promiscuous receiver happened to hear. */
  if (!(fc1 & devourer::sta::kFcFromDs) || (fc1 & devourer::sta::kFcToDs))
    return;
  if (std::memcmp(mpdu + 10, g_sm.bssid(), 6) != 0) return;
  const bool to_us = std::memcmp(mpdu + 4, g_own, 6) == 0;
  const bool group = (mpdu[4] & 0x01) != 0;
  if (!to_us && !group) return;

  /* FRAGMENTS AND A-MSDUs ARE REFUSED, NOT MISREAD. Neither is reassembled
   * here, and handing half an MSDU to the host as a whole one produces a
   * frame that looks well-formed and decodes to nonsense. Counted so the
   * ledger can say which. */
  if ((fc1 & devourer::sta::kFcMoreFrag) || (mpdu[22] & 0x0f)) {
    g_frag_drop.fetch_add(1);
    return;
  }
  const size_t hlen = devourer::sta::data_hdr_len(fc0, fc1);
  if (len < hlen) { g_rx_short.fetch_add(1); return; }
  if (devourer::sta::is_qos_data(fc0) && (mpdu[24] & 0x80)) {
    g_amsdu_drop.fetch_add(1);
    return;
  }

  const uint8_t* da = devourer::sta::data_da(mpdu, fc1);
  const uint8_t* sa = devourer::sta::data_sa(mpdu, fc1);
  /* THE QoS CONTROL FIELD IS AT A FIXED OFFSET, NOT AT hlen - 2.
   *
   * It follows the addresses; HT Control follows IT. So on a QoS frame with
   * the Order bit set, data_hdr_len() is 30 and `hlen - 2` lands inside HT
   * CONTROL - the replay window would then be indexed by four bits of a
   * link-adaptation field. Two real TIDs sharing a window reject each
   * other's frames as replays, and a genuine replay lands in a window that
   * has not seen its PN. Ccmp.h gets this right and this did not.
   *
   * 24 and not `four_addr ? 30 : 24` because a 4-address frame cannot reach
   * here at all: the FromDS/ToDS test above accepts only from-the-DS
   * frames. */
  const int tid = devourer::sta::is_qos_data(fc0)
                      ? (mpdu[24] & 0x0f)
                      : devourer::sta::CcmpReplay::kNonQosTid;

  if (!(mpdu[4] & 0x01) &&
      g_rx_dup.is_duplicate((fc1 & devourer::sta::kFcRetry) != 0,
                            (uint16_t)(mpdu[22] | (mpdu[23] << 8)), tid)) {
    g_dup_drop.fetch_add(1);
    return;
  }

  if (!(fc1 & devourer::sta::kFcProtected)) {
    /* An unprotected data frame on a WPA2 link is not ours to forward: the
     * whole point of the key exchange is that everything after it is
     * protected, and accepting plaintext would let anyone on the channel
     * inject into the host's stack. EAPOL is the one exception and
     * StationSm::on_rx has already consumed it above. */
    if (g_sm.security() != StationSm::Security::Open) return;
    g_plain_rx.fetch_add(1);
    if (len > hlen) tap_up(da, sa, mpdu + hlen, len - hlen);
    return;
  }
  if (g_sm.security() == StationSm::Security::Open) return;

  /* THE CCMP HEADER AND THE MIC HAVE TO BE THERE BEFORE ANYTHING READS THEM.
   *
   * The only guard above this was `len < hlen`, and the key-id read below
   * touches mpdu[hlen + 3] - so a 24-byte protected data frame, which any
   * station on the channel can air with our AP's address in addr2, read
   * three bytes past the end of the receive buffer. On MT7612U that is past
   * the allocation, because this part strips the FCS; on Realtek it landed
   * in the four trailing FCS bytes, which is the only reason it was latent.
   * The proof of length lived inside ccmp_decrypt, five lines too late.
   *
   * And a frame this short is MALFORMED, not forged: counting it as a MIC
   * failure fills the "someone is tampering" counter with every runt on the
   * channel, on a receiver that runs promiscuous. */
  if (devourer::sta::ccmp_decrypted_len(len, hlen) == 0) {
    g_rx_short.fetch_add(1);
    return;
  }
  g_enc_rx.fetch_add(1);
  const uint8_t key_id = devourer::sta::ccmp_key_id(mpdu + hlen);
  /* Key id 0 is the pairwise key; the AP advertises the GTK at a different
   * index in message 3 and airs group traffic under it. Choosing by the
   * frame's own key id rather than by its address is what the standard says
   * and is also more robust: an AP may unicast under the group key during a
   * rekey. */
  /* KEY ID 0 IS THE PAIRWISE KEY, by the convention every AP in practice
   * follows and mac80211 states outright - hostapd's group key number starts
   * at 1 and toggles 1<->2. An AP that installed its GTK at index 0 would
   * have its group traffic looked up under the pairwise key and counted as
   * MIC failures; none does, and this is written down rather than defended
   * against, because defending would mean guessing. */
  const devourer::sta::Supplicant& sup = g_sm.supplicant();
  const bool pairwise = key_id == 0;
  const uint8_t* tk = pairwise ? sup.tk() : sup.gtk();
  /* A GTK that is not 16 bytes is not a CCMP key. find_gtk_kde accepts 16 to
   * 32 - the range the KDE allows across ciphers - so an AP can legitimately
   * hand us one this data plane cannot use, and handing the cipher the first
   * 16 bytes of it would produce a MIC failure that reads as an attack. */
  if (!pairwise && (!sup.gtk_valid() || sup.gtk_len() != 16)) {
    g_no_key.fetch_add(1);
    return;
  }
  if (!pairwise && key_id != sup.gtk_key_id()) {
    g_no_key.fetch_add(1);
    return;
  }

  std::vector<uint8_t> plain(devourer::sta::ccmp_decrypted_len(len, hlen));
  size_t plain_len = 0;
  uint64_t pn = 0;
  /* A PAIRWISE REKEY COSTS AT MOST ONE FRAME HERE, and that is the protocol
   * and not a defect. 802.11-2016 12.7.6.5: the supplicant installs the new
   * PTK when message 3 arrives, the authenticator only once it has ACCEPTED
   * message 4 - so for one round trip the AP is still transmitting under the
   * old key while this side has already switched. A data frame that lands in
   * that window fails its MIC.
   *
   * MEASURED, 2026-09-21: one MIC failure across six pairwise rekeys in
   * ~150 s against hostapd with wpa_ptk_rekey=25, with the ping at 0% loss
   * throughout - so it is a lost frame, not a lost link. NOT DEFENDED
   * AGAINST, deliberately: keeping the old key alive for a grace period
   * means a second replay window for its PN space and a second key in
   * memory, to save one frame per rekey on a setting whose hostapd default
   * is "never". The on-air cell asserts MIC failures <= pairwise rekeys,
   * which is this theory's exact prediction and a far sharper test than
   * zero would be. */
  if (!devourer::sta::ccmp_decrypt(g_crypto, tk, mpdu, len, hlen, mpdu + 10,
                                   plain.data(), plain.size(), &plain_len,
                                   &pn)) {
    g_mic_fail.fetch_add(1);
    return;
  }
  /* THE PN IS ADMITTED ONLY AFTER THE MIC VERIFIED. Accepting it first would
   * let anyone on the channel advance the window with garbage and lock the
   * real AP out - the same rule the four-way's replay counter follows, and
   * the one PR #335 got wrong in both places. */
  devourer::sta::CcmpReplay& win = pairwise ? g_rx_replay : g_group_replay;
  if (!win.accept(pn, pairwise ? tid : devourer::sta::CcmpReplay::kNonQosTid)) {
    g_replays.fetch_add(1);
    return;
  }
  if (!pairwise) g_group_rx.fetch_add(1);

  /* AN EAPOL-KEY FRAME INSIDE THE CIPHER IS THE GROUP REKEY, and it is the
   * state machine's, not the host's. Handing it to the TAP instead is what
   * this harness did on its first WPA2 run against hostapd, which answered
   * none of its four message 1s and then threw the station off the BSS -
   * see StationSm::on_decrypted_msdu.
   *
   * Only a frame addressed to US: a group-addressed EAPOL-Key is not part of
   * any handshake this station is in. */
  /* AN EAPOL-KEY FRAME IS NEVER THE HOST'S, whoever it is addressed to. A
   * group-addressed one is not part of any handshake this station is in, so
   * it is dropped rather than handed up - writing an 0x888e frame onto the
   * TAP gives the host stack a link-layer control frame the supplicant
   * owns. */
  const bool is_eapol = devourer::sta::is_ethertype_snap(plain.data(),
                                                         plain_len) &&
                        plain[6] == 0x88 && plain[7] == 0x8e;
  /* Not to us, or not under the PAIRWISE key: either way it is not part of a
   * handshake this station is in. An EAPOL-Key arriving under the GROUP key
   * is something every station on the BSS could have forged, and answering
   * it under a key the authenticator is not expecting is worse than
   * ignoring it. */
  if (is_eapol && (!to_us || !pairwise)) return;

  /* THE ANSWER GOES OUT UNDER THE KEY THE REQUEST CAME IN UNDER, which for a
   * PTK rekey is the OLD pairwise key and not the one message 3 is about to
   * install. The authenticator does not switch its own key until it has
   * ACCEPTED message 4, so a message 4 encrypted under the new TK is a frame
   * it cannot read - and the rekey then fails exactly the way the GROUP
   * rekey did before d0f3729, with the link reporting itself healthy.
   *
   * COPIED BEFORE on_decrypted_msdu(), not after. `tk` points into the
   * supplicant, and message 3 installs the new key through that very
   * pointer - so a copy taken afterwards is the NEW key wearing the old
   * key's name. The headless cell caught exactly that. */
  uint8_t tk_in[16];
  if (pairwise) std::memcpy(tk_in, tk, 16);

  std::vector<uint8_t> reply;
  if (to_us && g_sm.on_decrypted_msdu(plain.data(), plain_len, now, &reply)) {
    g_eapol_enc_rx.fetch_add(1);
    if (!reply.empty()) {
      std::vector<uint8_t> out;
      devourer::sta::append_llc_snap(out, 0x888e);
      out.insert(out.end(), reply.begin(), reply.end());
      /* Addressed to the BSSID: the AP is both the receiver and the
       * destination of an EAPOL-Key frame. A non-pairwise EAPOL-Key is
       * already refused above, so tk_in is always the one that was set. */
      if (air_msdu(out.data(), out.size(), g_sm.bssid(), tk_in))
        g_eapol_enc_tx.fetch_add(1);
    }
    /* Only now: a rekey has installed a new key and the PN spaces restart
     * with it. Deliberately AFTER the reply was encrypted. */
    note_keys();
    devourer::sta::secure_wipe(tk_in, sizeof tk_in);
    return;
  }
  devourer::sta::secure_wipe(tk_in, sizeof tk_in);
  tap_up(da, sa, plain.data(), plain_len);
}

/* ---- DOWN: one MSDU onto the air --------------------------------------- */

/* Frame and (on a protected link) encrypt one MSDU for `da`, and queue it.
 * Returns false when the cipher refused. Caller holds g_mu.
 *
 * Shared by the host's traffic and by the group rekey's answer, which is the
 * reason it is a function: the rekey reply must be encrypted exactly the way
 * a data frame is, and a second copy of this would be a second chance to get
 * the PN space wrong. */
bool air_msdu(const uint8_t* msdu, size_t len, const uint8_t da[6],
              const uint8_t* tk) {
  const bool protect = g_sm.security() != StationSm::Security::Open;
  /* Null means "whatever is installed now", which is what ordinary traffic
   * wants. A rekey's answer passes the key its request arrived under - see
   * the note at the call site. */
  if (!tk) tk = g_sm.supplicant().tk();
  std::vector<uint8_t> hdr = devourer::sta::data_hdr_to_ds(
      g_sm.bssid(), g_own, da, protect, g_data_seq.next());

  if (!protect) {
    hdr.insert(hdr.end(), msdu, msdu + len);
    g_tx_plain.fetch_add(1);
    enqueue(std::move(hdr));
    return true;
  }
  std::vector<uint8_t> f(devourer::sta::ccmp_encrypted_len(hdr.size(), len));
  const size_t n = devourer::sta::ccmp_encrypt(
      g_crypto, tk, hdr.data(), hdr.size(), g_own,
      g_tx_pn, /*key_id=*/0, msdu, len, f.data(), f.size());
  if (n == 0) { g_tx_enc_fail.fetch_add(1); return false; }
  /* Only after the cipher succeeded: a PN burned on a frame that was never
   * aired is harmless, but a PN reused because the failure path skipped the
   * increment is not. */
  g_tx_pn++;
  f.resize(n);
  g_tx_enc.fetch_add(1);
  enqueue(std::move(f));
  return true;
}

/* ---- DOWN: one Ethernet frame from the host ---------------------------- */

/* Lifted out of the reader thread's lambda for the same reason the AP's is:
 * so the headless cells can drive it with no TAP device and no thread. */
void tap_down_one(const uint8_t* eth, size_t len) {
  uint8_t msdu[2048], da[6], sa[6];
  /* COUNTED FIRST, and that ordering is the whole point of the counter.
   * "from host" has to mean every frame the host handed us, or the books
   * below cannot be closed: a malformed frame used to be dropped BEFORE this
   * line, so it was a loss that no total included. */
  g_tap_rx.fetch_add(1);
  const size_t m = devourer::sta::eth_to_msdu(eth, len, msdu, sizeof msdu, da,
                                              sa);
  if (m == 0) { g_tap_down_drop.fetch_add(1); return; }

  std::lock_guard<std::mutex> l(g_mu);
  if (!g_sm.connected()) { g_tap_down_drop.fetch_add(1); return; }

  /* THE SOURCE ADDRESS MUST BE OURS. A station's uplink carries its own
   * address in addr2, and the AP matches that against the association it
   * holds; a frame claiming someone else's is refused by any AP worth using
   * and would be an injection tool on one that is not. A TAP with the wrong
   * MAC is the ordinary cause, which is worth being able to see. */
  if (std::memcmp(sa, g_own, 6) != 0) { g_tap_down_drop.fetch_add(1); return; }

  if (!air_msdu(msdu, m, da)) g_tap_down_drop.fetch_add(1);
}

/* ---- the scan and the reconnect policy ---------------------------------- */

/* Both of these are THIS FILE'S POLICY and not the protocol's. src/sta/ has
 * neither, on purpose: a station that re-joins instantly drains a battery
 * against an AP that is off, and one that waits a minute is useless on an FPV
 * link. The numbers below suit this bench and are env-overridable. */

/* Which channel the radio should be on right now, and a directed probe
 * request when we are looking for a BSS we have not heard. Returns the
 * channel it wants. */
uint8_t scan_step(uint32_t now) {
  if (g_scan_chans.empty()) return g_chan;
  if (g_scan_chans.size() == 1) return g_scan_chans[0];
  if ((uint32_t)(now - g_scan_switch_ms) >= g_scan_dwell_ms) {
    g_scan_switch_ms = now;
    g_scan_idx = (g_scan_idx + 1) % g_scan_chans.size();
  }
  return g_scan_chans[g_scan_idx];
}

/* A directed probe request for the SSID we want, on the channel we are on.
 * Passive scanning alone finds a BSS in one beacon interval; a probe finds a
 * hidden one and shortens the wait on a swept channel. Caller holds g_mu. */
void probe(uint8_t chan) {
  std::vector<uint8_t> m =
      devourer::sta::build_probe_req(g_own, g_ssid, chan, chan > 14);
  if (m.empty()) return;
  devourer::sta::assign_seq(m, g_data_seq.next());
  g_probe_tx.fetch_add(1);
  enqueue(std::move(m));
}

/* The policy Phase 3 refused to guess at. Returns the channel the radio
 * should be tuned to. Caller must NOT hold g_mu. */
uint8_t supervise(uint32_t now) {
  std::lock_guard<std::mutex> l(g_mu);

  const StationSm::State st = g_sm.state();
  if (st != StationSm::State::Idle && st != StationSm::State::Failed)
    return g_sm.channel() ? g_sm.channel() : g_chan;

  if (g_gave_up) return g_chan;

  /* The transition INTO Failed, handled once. */
  if (st == StationSm::State::Failed && !g_failed_noted) {
    g_failed_noted = true;
    /* Counted separately from a first join, so the on-air `reconnect` cell
     * can assert that a RECOVERY happened rather than that a first
     * association eventually did. ONCE PER LOST LINK, not once per retry. */
    if (g_was_associated) {
      g_was_associated = false;
      g_reconnects.fetch_add(1);
    }
    if (!g_reconnect) { g_gave_up = true; return g_chan; }
    /* THE BACKOFF IS FOR A RE-JOIN, NOT FOR THE FIRST ATTEMPT. Arming it
     * unconditionally made every run - including the very first - sit idle
     * for a whole backoff before it would even look at the scan table. */
    g_next_join_ms = now + g_rejoin_backoff_ms;
  }
  if (g_next_join_ms && (int32_t)(now - g_next_join_ms) < 0)
    return scan_step(now);

  /* THE TABLE IS AGED FIRST. Without this a station will happily try to
   * associate with a BSS that went off the air minutes ago and then report
   * "no response" as though the AP were broken. Ten seconds is ~100 beacon
   * intervals - long enough that a few lost beacons mean nothing. */
  g_bss.expire(now, 10000);

  const bool open = g_sm.security() == StationSm::Security::Open;
  const BssEntry* e = open ? g_bss.select_open(g_ssid) : g_bss.select(g_ssid);
  const uint8_t chan = scan_step(now);
  if (!e) {
    probe(chan);
    g_next_join_ms = now + 200;      /* probe again shortly, do not spin */
    return chan;
  }

  uint8_t snonce[32];
  /* FRESH FOR EVERY ATTEMPT. Reusing one across associations makes the PTK a
   * function of the ANonce alone, which is why StationSm takes it as an
   * argument to join() rather than deriving it once in configure(). */
  if (!open && RAND_bytes(snonce, sizeof snonce) != 1) {
    std::fprintf(stderr, "  RAND_bytes failed - refusing to join with a "
                         "predictable SNonce\n");
    g_gave_up = true;
    return g_chan;
  }
  g_join_attempts++;
  g_joins.fetch_add(1);
  g_next_join_ms = 0;
  g_failed_noted = false;
  if (!g_sm.join(*e, open ? nullptr : snonce, now)) {
    /* join() refused the BSS itself - wrong cipher, MFP required, no PMK. It
     * will refuse the same entry again, so back off rather than spin. */
    g_next_join_ms = now + g_rejoin_backoff_ms;
  }
  return e->info.channel ? e->info.channel : chan;
}

/* ---- TAP ---------------------------------------------------------------- */

int tap_open(const char* name, const uint8_t mac[6]) {
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) { perror("  TAP: open /dev/net/tun"); return -1; }
  struct ifreq ifr;
  std::memset(&ifr, 0, sizeof ifr);
  ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
  std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", name);
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    perror("  TAP: TUNSETIFF (CAP_NET_ADMIN?)");
    ::close(fd);
    return -1;
  }
  /* THE TAP MUST CARRY THE RADIO'S MAC. Every frame the host sends leaves
   * with addr2 = our 802.11 address, and the AP matches that against the
   * association; a TAP with a random kernel-assigned MAC produces Ethernet
   * frames whose source is not us, which tap_down_one then refuses. Setting
   * it here rather than in the shell script means the rule cannot be
   * forgotten by a caller. */
  struct ifreq set;
  std::memset(&set, 0, sizeof set);
  std::snprintf(set.ifr_name, IFNAMSIZ, "%s", ifr.ifr_name);
  set.ifr_hwaddr.sa_family = ARPHRD_ETHER;
  std::memcpy(set.ifr_hwaddr.sa_data, mac, 6);
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s >= 0) {
    if (::ioctl(s, SIOCSIFHWADDR, &set) < 0)
      perror("  TAP: SIOCSIFHWADDR");
    ::close(s);
  }
  std::fprintf(stderr,
               "  TAP: %s open with %02x:%02x:%02x:%02x:%02x:%02x - the host "
               "stack owns ARP/ICMP/DHCP\n",
               ifr.ifr_name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return fd;
}

/* ---- the radio adapter --------------------------------------------------- */

void on_rx(const Packet& p) {
  /* NOT p.Data.size(): on every Realtek generation that buffer still carries
   * the four trailing FCS bytes. See tests/rx_mpdu.h - a four-byte error here
   * is reported by the ledger below as a MIC failure, i.e. as an attack. */
  const size_t mlen = devourer::test::mpdu_len(p);
  if (p.RxAtrib.crc_err) { g_crc_err.fetch_add(1); return; }
  if (mlen < 24) { g_rx_short.fetch_add(1); return; }
  rx_frame(p.Data.data(), mlen, rssi_dbm(p.RxAtrib.rssi[0]), now_ms());
}

const char* state_name(StationSm::State s) {
  switch (s) {
    case StationSm::State::Idle: return "Idle";
    case StationSm::State::Authenticating: return "Authenticating";
    case StationSm::State::Associating: return "Associating";
    case StationSm::State::FourWay: return "FourWay";
    case StationSm::State::Connected: return "Connected";
    case StationSm::State::Failed: return "Failed";
  }
  return "?";
}

const char* fail_name(StationSm::Failure f) {
  switch (f) {
    case StationSm::Failure::None: return "none";
    case StationSm::Failure::AuthTimeout: return "auth-timeout";
    case StationSm::Failure::AuthRefused: return "auth-refused";
    case StationSm::Failure::AssocTimeout: return "assoc-timeout";
    case StationSm::Failure::AssocRefused: return "assoc-refused";
    case StationSm::Failure::Deauthenticated: return "deauthenticated";
    case StationSm::Failure::HandshakeTimeout: return "handshake-timeout";
    case StationSm::Failure::BeaconLost: return "beacon-lost";
    case StationSm::Failure::NoPmk: return "no-pmk";
    case StationSm::Failure::NotConfigured: return "not-configured";
  }
  return "?";
}

/* THE LEDGER, printed at every exit whether the run worked or not.
 *
 * The AP harness learned this the hard way: its on-air runs could not tell an
 * AP that never RECEIVED an encrypted frame from one that received and failed
 * to decrypt them, so a 100%-loss result had no diagnosis attached. The
 * station's version of that question is sharper, because the receive path is
 * promiscuous and most of a busy channel lands in StationSm::on_rx - so "we
 * heard nothing" and "we heard the wrong AP" and "we heard our AP and it said
 * no" are three different lines here. */
void report() {
  std::lock_guard<std::mutex> l(g_mu);
  std::fprintf(stderr, "state=%s", state_name(g_sm.state()));
  if (g_sm.state() == StationSm::State::Failed)
    std::fprintf(stderr, " reason=%s status=%u", fail_name(g_sm.fail_reason()),
                 g_sm.status());
  std::fprintf(stderr, " aid=%u keyed=%d bss_known=%d\n", g_sm.aid(),
               (int)g_sm.keyed(), g_bss.count());
  std::fprintf(stderr,
               "  join: beacons observed=%llu, probes sent=%llu, joins=%llu,"
               " associations=%llu, reconnects=%llu\n",
               (unsigned long long)g_beacons.load(),
               (unsigned long long)g_probe_tx.load(),
               (unsigned long long)g_joins.load(),
               (unsigned long long)g_associations.load(),
               (unsigned long long)g_reconnects.load());
  std::fprintf(stderr,
               "  station rx: auth_tx=%u assoc_tx=%u eapol_tx=%u eapol_rx=%u"
               " beacons=%u\n",
               g_sm.auth_tx, g_sm.assoc_tx, g_sm.eapol_tx, g_sm.eapol_rx,
               g_sm.beacons_rx);
  std::fprintf(stderr,
               "  refused by the address filter: not-our-bss=%u,"
               " not-for-us=%u, ignored=%u, malformed=%u, tx-dropped=%u"
               " (protected data, handled here: %u)\n",
               g_sm.rx_not_our_bss, g_sm.rx_not_for_us, g_sm.rx_ignored,
               g_sm.rx_malformed, g_sm.tx_dropped, g_sm.rx_protected);
  const devourer::sta::Supplicant& sup = g_sm.supplicant();
  std::fprintf(stderr,
               "  rekeys (EAPOL inside the cipher): received=%llu,"
               " answered=%llu; keys installed: PTK=%llu GTK=%llu\n",
               (unsigned long long)g_eapol_enc_rx.load(),
               (unsigned long long)g_eapol_enc_tx.load(),
               (unsigned long long)g_ptk_installs.load(),
               (unsigned long long)g_gtk_installs.load());
  std::fprintf(stderr,
               "  four-way: mic_failures=%u replays=%u retransmits=%u"
               " malformed=%u out_of_state=%u ignored=%u crypto_errors=%u\n",
               sup.mic_failures, sup.replays, sup.retransmits, sup.malformed,
               sup.out_of_state, sup.ignored, sup.crypto_errors);
  std::fprintf(stderr,
               "  data plane: encrypted rx=%llu (group=%llu), plaintext rx="
               "%llu, MIC failures=%llu, replays rejected=%llu,"
               " duplicates dropped=%llu, no key for it=%llu\n",
               (unsigned long long)g_enc_rx.load(),
               (unsigned long long)g_group_rx.load(),
               (unsigned long long)g_plain_rx.load(),
               (unsigned long long)g_mic_fail.load(),
               (unsigned long long)g_replays.load(),
               (unsigned long long)g_dup_drop.load(),
               (unsigned long long)g_no_key.load());
  std::fprintf(stderr,
               "  refused before the host: fragmented=%llu, A-MSDU=%llu,"
               " short=%llu, crc_err=%llu\n",
               (unsigned long long)g_frag_drop.load(),
               (unsigned long long)g_amsdu_drop.load(),
               (unsigned long long)g_rx_short.load(),
               (unsigned long long)g_crc_err.load());
  /* TWO IDENTITIES, and they are printed in the shape that lets a caller
   * check them rather than believe them:
   *   from host == encrypted + plaintext + dropped down
   *   queued    == aired + queue dropped + send failed
   * Both hold exactly, because the TAP reader is stopped and the queue
   * drained before this runs. */
  std::fprintf(stderr,
               "  TAP: to host=%llu, from host=%llu, dropped up=%llu,"
               " dropped down=%llu\n",
               (unsigned long long)g_tap_tx.load(),
               (unsigned long long)g_tap_rx.load(),
               (unsigned long long)g_tap_drop.load(),
               (unsigned long long)g_tap_down_drop.load());
  std::fprintf(stderr,
               "  tx: encrypted=%llu (cipher refused %llu), plaintext=%llu,"
               " queued=%llu, aired=%llu, send failed=%llu, queue dropped=%llu\n",
               (unsigned long long)g_tx_enc.load(),
               (unsigned long long)g_tx_enc_fail.load(),
               (unsigned long long)g_tx_plain.load(),
               (unsigned long long)g_q_in.load(),
               (unsigned long long)g_sent.load(),
               (unsigned long long)g_send_fail.load(),
               (unsigned long long)g_q_drop.load());
  if (g_ccmp_profile)
    std::fprintf(stderr,
                 "{\"ev\":\"ccmp.profile\",\"path\":\"software\","
                 "\"tx_frames\":%llu,\"tx_ns\":%llu,"
                 "\"rx_frames\":%llu,\"rx_ns\":%llu}\n",
                 (unsigned long long)g_ccmp_tx_frames.load(),
                 (unsigned long long)g_ccmp_tx_ns.load(),
                 (unsigned long long)g_ccmp_rx_frames.load(),
                 (unsigned long long)g_ccmp_rx_ns.load());
}

std::vector<uint8_t> parse_chan_list(const char* s) {
  std::vector<uint8_t> v;
  while (*s) {
    char* end = nullptr;
    const long n = std::strtol(s, &end, 10);
    if (end == s) break;
    if (n > 0 && n < 256) v.push_back((uint8_t)n);
    s = (*end == ',') ? end + 1 : end;
  }
  return v;
}

int self_test();

}  // namespace

int main(int argc, char** argv) {
  /* HEADLESS FIRST, before libusb is touched, so ctest needs no adapter, no
   * root and no airtime - the same shape ap_wpa2 --self-test has. */
  if (argc > 1 && std::strcmp(argv[1], "--self-test") == 0) return self_test();

  const int sec = argc > 1 ? std::atoi(argv[1]) : 60;
  if (const char* s = std::getenv("DEVOURER_STA_SSID")) g_ssid = s;
  if (const char* k = std::getenv("DEVOURER_STA_PSK")) g_psk = k;
  if (const char* c = std::getenv("DEVOURER_CHANNEL")) g_chan = (uint8_t)std::atoi(c);
  if (const char* c = std::getenv("DEVOURER_STA_SCAN_CHANNELS"))
    g_scan_chans = parse_chan_list(c);
  if (g_scan_chans.empty()) g_scan_chans.push_back(g_chan);
  if (const char* d = std::getenv("DEVOURER_STA_SCAN_DWELL_MS"))
    g_scan_dwell_ms = (uint32_t)std::strtoul(d, nullptr, 10);
  if (const char* r = std::getenv("DEVOURER_STA_RECONNECT"))
    g_reconnect = std::strcmp(r, "0") != 0;
  if (const char* b = std::getenv("DEVOURER_STA_BACKOFF_MS"))
    g_rejoin_backoff_ms = (uint32_t)std::strtoul(b, nullptr, 10);
  if (const char* p = std::getenv("DEVOURER_CCMP_PROFILE"))
    g_ccmp_profile = std::strcmp(p, "0") != 0;
  if (const char* t = std::getenv("DEVOURER_STA_TICK_MS"))
    g_tick_ms = (uint32_t)std::strtoul(t, nullptr, 10);
  if (const char* a = std::getenv("DEVOURER_STA_ARM"))
    g_arm = std::strcmp(a, "0") != 0;

  auto logger = std::make_shared<Logger>();
  apply_logging_env(*logger);
  libusb_context* ctx = nullptr;
  libusb_init(&ctx);
  libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
  static const uint16_t pids[] = {0x7612};
  auto* h = open_selected_usb(ctx, logger, pids, 1);
  if (!h) return 1;
  std::shared_ptr<devourer::UsbDeviceLock> lk;
  if (devourer::claim_interface_then_reset(
          h, devourer::find_wifi_interface(h), logger, true, lk) != 0)
    return 1;
  WiFiDriver wifi(logger);
  auto dev = wifi.CreateRadio(h, ctx, lk, devourer_config_from_env());
  g_dev = dev.get();
  if (!g_dev) return 1;

  /* THE RATE EVERY FRAME AIRS AT, and until now it was 6M legacy, hardcoded,
   * with no way to ask for anything else. That is a reasonable default - it
   * is the most robust OFDM rate there is, and a link that will not come up
   * at 6M has a problem worth seeing - but it is also a 6 Mbit/s ceiling on
   * a part that does 80 MHz VHT, and every throughput figure this project
   * has ever quoted for a station link was measured under it.
   *
   * There is no rate control here and there is not going to be: this is a
   * test harness, and picking a rate per frame from link statistics is the
   * integrator's job (docs/station-mode-scope.md says so about the scanner
   * and the reconnect policy for the same reason). What the harness owes is
   * the ability to ASK, so the ceiling can be measured rather than assumed. */
  const char* rate_s = std::getenv("DEVOURER_TX_RATE");
  if (!rate_s || !*rate_s) rate_s = "6M";
  g_rt = devourer::build_stream_radiotap(devourer::parse_tx_mode_str(rate_s));
  if (const char* a = std::getenv("DEVOURER_STA_ACK"); !a || !*a || std::strcmp(a, "0") != 0)
    g_rt_ack = devourer::build_stream_radiotap(devourer::parse_tx_mode_str(rate_s),
                                               /*no_ack=*/false);
  std::fprintf(stderr, "  TX rate: %s, unicast %s\n", rate_s,
               g_rt_ack.empty() ? "NOACK (no retries)" : "ACK-requested (hardware retries)");
  g_dev->InitWrite(SelectedChannel{g_chan, 0, CHANNEL_WIDTH_20});

  /* THE STATION'S ADDRESS IS THE ADAPTER'S, NOT A CHOICE. See the note at the
   * top of this file: on MT7612U a station that transmits from any other
   * address is deaf, because the auto-response engine matches address 1
   * against MT_MAC_ADDR. Refusing outright is better than running with an
   * invented address and reporting a link that cannot work.
   *
   * AFTER InitWrite, not before. CreateRadio performs no bring-up - the
   * device handle behind this call does not exist until InitWrite has run,
   * and asking early returns false, which this then reports as "the radio
   * does not know its own MAC". Measured, on the first on-air run. */
  if (!g_dev->GetPermanentMacAddress(g_own)) {
    std::fprintf(stderr,
                 "sta_client: the radio does not report its MAC address - a "
                 "station cannot invent one, see src/mt7612u/station.cpp\n");
    return 1;
  }

  {
    std::lock_guard<std::mutex> l(g_mu);
    /* set_wanted BEFORE any beacon is folded in: without it a neighbour (or
     * an attacker) airing sixteen fabricated BSSIDs keeps every fabricated
     * entry at age zero and makes the genuine AP the oldest, every time. */
    g_bss.set_wanted(g_ssid);
    const bool ok = g_psk.empty()
                        ? g_sm.configure_open(g_ssid, g_own)
                        : g_sm.configure(g_crypto, g_ssid, g_psk.c_str(), g_own);
    if (!ok) {
      std::fprintf(stderr, "sta_client: configure failed\n");
      return 1;
    }
  }

  if (const char* t = std::getenv("DEVOURER_STA_TAP"))
    g_tap_fd = tap_open(t, g_own);

  std::thread rx([&] { g_dev->StartRxLoop(on_rx); });

  /* AFTER StartRxLoop, which is IRadio's stated ordering rule and not a
   * style preference: a backend may program the receive filter when the RX
   * loop starts and overwrite anything an earlier call wrote (MT7612U does
   * exactly this). Gated on the capability flag rather than on a nullptr or a
   * chip test - that is what station_mode_ok is for, and it is the one line
   * a second backend has to satisfy instead of editing this file. */
  const devourer::AdapterCaps caps = g_dev->GetAdapterCaps();
  /* NOT ARMED HERE. The BSSID is not known until a BSS has been selected, so
   * the only call that can be made at startup is SetStationIdentity(own,
   * own) - which the seam REFUSES, correctly and by contract ("`own` and
   * `bssid` must both be unicast and must differ"). The first on-air run
   * made exactly that call and logged "station identity refused: own ==
   * bssid", which looks like a capability failure and is a harness bug. The
   * identity is armed in the loop below, once, for the BSS actually joined. */

  std::thread tap_rd;
  if (g_tap_fd >= 0) {
    tap_rd = std::thread([&] {
      uint8_t eth[2048];
      for (;;) {
        const ssize_t got = ::read(g_tap_fd, eth, sizeof eth);
        if (got <= 0) return;
        tap_down_one(eth, (size_t)got);
      }
    });
  }

  std::fprintf(stderr,
               "sta_client up: own %02x:%02x:%02x:%02x:%02x:%02x ssid '%s' "
               "%s ch%u station_mode_ok=%d arm=%d\n",
               g_own[0], g_own[1], g_own[2], g_own[3], g_own[4], g_own[5],
               g_ssid.c_str(), g_psk.empty() ? "OPEN" : "WPA2-PSK", g_chan,
               (int)caps.station_mode_ok, (int)g_arm);

  uint8_t tuned = g_chan;
  uint8_t bssid_armed[6] = {0};
  /* A REFUSED arm is retried, a few times, a second apart. It used to be
   * recorded as done before the call, so one transient failure - a Jaguar2
   * control read fails about once a minute under RX load - left the whole
   * association unarmed with a single log line (review, Phase 6). */
  constexpr int kArmTries = 5;
  int arm_tries = 0;
  bool arm_ok = false;
  uint32_t next_arm_ms = 0;
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(sec);
  while (!g_stop && std::chrono::steady_clock::now() < end) {
    const uint32_t now = now_ms();
    const uint8_t want = supervise(now);
    if (want && want != tuned) {
      /* Retuning is only ever reached while unassociated - supervise()
       * returns the joined channel once the machine has left Idle/Failed. */
      g_dev->SetMonitorChannel(SelectedChannel{want, 0, CHANNEL_WIDTH_20});
      tuned = want;
    }
    bool arm_now = false;
    {
      std::lock_guard<std::mutex> l(g_mu);
      g_sm.tick(now);
      /* Arm the identity for the BSS we actually joined, once, when it
       * changes. On MT7612U this writes no register and only verifies that
       * the port identity has not moved; on a backend where it does write
       * one, this is where it belongs - DECIDED here, MADE below. */
      if (g_arm && caps.station_mode_ok &&
          g_sm.state() != StationSm::State::Idle) {
        if (std::memcmp(bssid_armed, g_sm.bssid(), 6) != 0) {
          std::memcpy(bssid_armed, g_sm.bssid(), 6);
          arm_tries = 0;
          arm_ok = false;
          next_arm_ms = now;
        }
        if (!arm_ok && arm_tries < kArmTries &&
            (int32_t)(now - next_arm_ms) >= 0) {
          ++arm_tries;
          next_arm_ms = now + 1000;
          arm_now = true;
        }
      }
      std::vector<uint8_t> f;
      while (g_sm.pop_tx(&f)) {
        devourer::sta::assign_seq(f, g_data_seq.next());
        enqueue(std::move(f));
      }
    }
    /* THE ARM IS MADE OUTSIDE g_mu, and this is not tidiness: holding it
     * here DEADLOCKED the first Realtek-station run (Phase 6, 8812CU,
     * 2026-09-26 - gdb on the hung process). The arm is synchronous USB
     * control I/O, and a libusb synchronous transfer waits on the thread
     * handling libusb events - which on every backend here is the RX thread,
     * sitting inside on_rx -> rx_frame, blocked on g_mu. The MT7612U arm
     * makes almost no transfers, so the window never opened there; the
     * Realtek arm makes ~15. The rule is IRadio's (StartRxLoop): no device
     * call while holding a lock the RX callback takes. Still BEFORE the send
     * below, so the auth that just left the state machine airs armed. */
    if (arm_now) {
      const devourer::MacAddr own{{g_own[0], g_own[1], g_own[2], g_own[3],
                                   g_own[4], g_own[5]}};
      const devourer::MacAddr bss{{bssid_armed[0], bssid_armed[1],
                                   bssid_armed[2], bssid_armed[3],
                                   bssid_armed[4], bssid_armed[5]}};
      arm_ok = g_dev->SetStationIdentity(own, bss);
      std::fprintf(stderr,
                   "  station identity %s for BSSID "
                   "%02x:%02x:%02x:%02x:%02x:%02x (attempt %d/%d)\n",
                   arm_ok ? "armed" : "REFUSED", bssid_armed[0],
                   bssid_armed[1], bssid_armed[2], bssid_armed[3],
                   bssid_armed[4], bssid_armed[5], arm_tries, kArmTries);
    }
    std::vector<std::vector<uint8_t>> batch;
    { std::lock_guard<std::mutex> l(g_q_mu); batch.swap(g_q); }
    for (auto& f : batch) {
      if (g_dev->send_packet(f.data(), f.size())) g_sent.fetch_add(1);
      else g_send_fail.fetch_add(1);
    }
    /* A TIME SERIES, not just an exit ledger.
     *
     * Every counter this harness has is printed once, at exit, which answers
     * "what happened" and not "when". Two questions needed "when" and could
     * not be asked: whether the station's BEACON RECEPTION collapses while
     * the link is loaded - and in which direction - and whether anything
     * drifts over a soak. A run that ends with associations=4 does not say
     * whether they were spread over an hour or all in one bad second.
     *
     * Off unless asked for, and one line per tick on the event plane, so a
     * consumer can diff consecutive ticks without parsing the ledger. */
    if (g_tick_ms) {
      const uint32_t now_t = now;
      if (now_t - g_last_tick >= g_tick_ms) {
        g_last_tick = now_t;
        std::lock_guard<std::mutex> l(g_mu);
        std::printf(
            "{\"ev\":\"sta.tick\",\"t_ms\":%u,\"state\":\"%s\","
            "\"beacons\":%llu,\"beacons_ours\":%u,"
            "\"associations\":%llu,\"reconnects\":%llu,"
            "\"enc_rx\":%llu,\"enc_tx\":%llu,\"mic_fail\":%llu,"
            "\"replays\":%llu,\"tap_to_host\":%llu,\"tap_from_host\":%llu,"
            "\"aired\":%llu,\"send_fail\":%llu,\"q_drop\":%llu}\n",
            now_t, state_name(g_sm.state()),
            /* BOTH, and the pair is what makes the tick worth emitting.
             * `beacons` is every BSS the scanner saw; `beacons_ours` is only
             * the one we joined. A receiver too busy to decode beacons loses
             * BOTH at the same rate. An AP that has stopped transmitting
             * them loses only ours. Either counter alone cannot tell those
             * apart, and the cell that reads this had exactly that hole. */
            (unsigned long long)g_beacons.load(),
            g_sm.beacons_rx,
            (unsigned long long)g_associations.load(),
            (unsigned long long)g_reconnects.load(),
            (unsigned long long)g_enc_rx.load(),
            (unsigned long long)g_tx_enc.load(),
            (unsigned long long)g_mic_fail.load(),
            (unsigned long long)g_replays.load(),
            (unsigned long long)g_tap_tx.load(),
            (unsigned long long)g_tap_rx.load(),
            (unsigned long long)g_sent.load(),
            (unsigned long long)g_send_fail.load(),
            (unsigned long long)g_q_drop.load());
        std::fflush(stdout);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  /* LEAVE CLEANLY. An association this side simply abandons stays alive at
   * the AP until it times the station out, holding an AID and - on this
   * project's own AP - a slot in a seven-entry table. */
  {
    std::lock_guard<std::mutex> l(g_mu);
    g_sm.leave();
    std::vector<uint8_t> f;
    while (g_sm.pop_tx(&f)) {
      devourer::sta::assign_seq(f, g_data_seq.next());
      enqueue(std::move(f));
    }
  }
  /* THE TAP CLOSES BEFORE THE LEDGER IS PRINTED, not after, and the queue is
   * drained after that. Otherwise the reader thread can encrypt and enqueue a
   * frame between the last drain and report(), which lands in `encrypted` and
   * in nothing else - and the two identities the ledger now states would be
   * off by however many frames were in flight at the instant it printed. */
  if (g_tap_fd >= 0) { ::close(g_tap_fd); g_tap_fd = -1; }
  if (tap_rd.joinable()) tap_rd.join();
  {
    std::vector<std::vector<uint8_t>> batch;
    { std::lock_guard<std::mutex> l(g_q_mu); batch.swap(g_q); }
    /* COUNTED, like every other send. This drain used to discard its result,
     * so a failure here was the one transmit loss the ledger could not
     * report - the same shape as the AP's uncounted queue cap. */
    for (auto& f : batch) {
      if (g_dev->send_packet(f.data(), f.size())) g_sent.fetch_add(1);
      else g_send_fail.fetch_add(1);
    }
  }
  /* The clear's result is the only way to learn the rollback did not land
   * (IRadio: the port may keep answering for `own`), so it is printed. */
  if (caps.station_mode_ok)
    std::fprintf(stderr, "  station identity clear: %s\n",
                 g_dev->ClearStationIdentity() ? "restored (verified)"
                                               : "NOT VERIFIED");

  report();
  g_dev->StopRxLoop();
  if (rx.joinable()) rx.join();
  return 0;
}

/* The headless cells. Included rather than linked because everything they
 * drive is in this file's anonymous namespace; see the note at the top of
 * that file. */
#include "sta_client_selftest.inc"
