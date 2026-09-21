/* StationSm — the association state machine: scan result in, connected out.
 *
 * Authenticate, associate, run the four-way, and notice when any of it stops
 * working. It owns a Supplicant and drives it; it does not reimplement any of
 * the key exchange.
 *
 * NO CLOCK AND NO RADIO. Time arrives as a `now_ms` argument and frames arrive
 * through on_rx(); frames to send come out of pop_tx(). That is what makes the
 * retransmission and timeout behaviour testable at all — the AP harness's
 * equivalent logic reads a steady_clock, and the result was that its retry
 * schedule could only ever be observed on a bench.
 *
 * WHAT IT IS NOT. Not a scanner: it has no notion of channels or dwell times,
 * because those need a radio. Feed it beacons through a BssTable and hand it
 * the entry to join.
 */
#ifndef DEVOURER_STA_STATION_SM_H
#define DEVOURER_STA_STATION_SM_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "sta/BssTable.h"
#include "sta/CryptoOps.h"
#include "sta/Dot11.h"
#include "sta/Eapol.h"
#include "sta/Supplicant.h"

namespace devourer {
namespace sta {

class StationSm {
 public:
  enum class State : uint8_t {
    Idle,            /* nothing in progress */
    Authenticating,  /* auth request sent */
    Associating,     /* association request sent */
    FourWay,         /* associated; the key exchange is running */
    Connected,       /* keyed, and the data plane may run */
    Failed,          /* gave up, or was thrown off; see fail_reason() */
  };

  /* Why the machine is in Failed. `status` carries the 802.11 status code of
   * a refused authentication or association, or the reason code of a deauth,
   * so "it did not connect" always comes with the number the AP gave. */
  enum class Failure : uint8_t {
    None,
    AuthTimeout,
    AuthRefused,
    AssocTimeout,
    AssocRefused,
    Deauthenticated,
    HandshakeTimeout,
    BeaconLost,
    NoPmk,
    NotConfigured,
  };

  /* Three transmissions of each management frame, 300 ms apart. An AP that
   * has not answered three probes in a second is not going to. */
  /* WHICH KIND OF BSS THIS STATION IS CONFIGURED FOR.
   *
   * Not a hypothetical second mode: without an open path a station that never
   * reaches Connected cannot say whether authentication/association or the
   * key exchange is what failed, because on a WPA2 BSS the two halves come up
   * together or not at all. The AP side of this tree has had the same ladder
   * since the beginning - tests/ap_responder.cpp is the open AP and
   * tests/ap_wpa2.cpp the protected one - and the station half did not.
   *
   * Open costs this file almost nothing: it skips the four-way and takes no
   * CryptoOps, which is the same property the open AP harness has (it links
   * no crypto library at all). */
  enum class Security : uint8_t {
    Open,
    Wpa2Psk,
  };

  static constexpr int kMaxTries = 3;
  static constexpr uint32_t kMgmtTimeoutMs = 300;
  /* The authenticator drives the four-way and retransmits it; this side only
   * answers, so its timeout is a give-up, not a retry schedule. */
  static constexpr uint32_t kHandshakeTimeoutMs = 3000;
  /* Ten beacon intervals at the usual 100 TU. Long enough that a few lost
   * beacons mean nothing, short enough that a station does not sit Connected
   * to an AP that has been switched off. */
  static constexpr uint32_t kBeaconLossMs = 1024;
  /* THE TRANSMIT QUEUE IS BOUNDED. Three authentication retries plus one
   * in-flight EAPOL reply is the most this machine legitimately owes, and
   * every frame in here is produced in response to a received one - so an
   * unbounded queue is an unbounded allocation an attacker controls. */
  static constexpr size_t kMaxTxQueue = 8;

  ~StationSm() { secure_wipe(pmk_, sizeof pmk_); }

  bool configure(CryptoOps& crypto, const std::string& ssid, const char* psk,
                 const uint8_t own[6]) {
    crypto_ = &crypto;
    ssid_ = ssid;
    std::memcpy(own_, own, 6);
    /* PBKDF2 once, here, rather than per association attempt: it is 4096
     * HMAC-SHA1 iterations and the answer only depends on the passphrase and
     * the SSID, neither of which changes between retries.
     *
     * THE SNONCE IS NOT HERE, and that is the whole reason this comment
     * exists. It used to be, next to the PMK, and the two have opposite
     * lifetimes: the PMK is fixed for the network and the SNonce must be
     * fresh for every association. A caller doing the obvious thing -
     * configure once, join repeatedly - would have reused one nonce across
     * every attempt and every roam, making the PTK a function of the ANonce
     * alone. It is an argument to join(). */
    have_pmk_ = pmk_from_psk(crypto, psk, ssid, pmk_);
    security_ = Security::Wpa2Psk;
    /* CONFIGURED EVEN WHEN THE PMK DERIVATION FAILED, on purpose. Gating this
     * on have_pmk_ would make the NoPmk branch in join() unreachable - the
     * caller would get NotConfigured, which names the wrong thing - and a
     * caller that ignores this return value is exactly the one that needs the
     * accurate diagnosis. */
    configured_ = true;
    return have_pmk_;
  }

  /* The same station on an OPEN BSS: no PSK, no PMK, no four-way, and no
   * CryptoOps - which is why this overload takes none. See the Security enum
   * for why an open path is worth having at all.
   *
   * It is a separate function rather than a null `psk` because a null
   * passphrase reads like a caller's mistake, and because the WPA2 form needs
   * a CryptoOps this one has no use for. */
  bool configure_open(const std::string& ssid, const uint8_t own[6]) {
    crypto_ = nullptr;
    security_ = Security::Open;
    ssid_ = ssid;
    std::memcpy(own_, own, 6);
    /* A station reconfigured from WPA2 to open must not keep the old PMK
     * sitting in memory for the rest of the process's life.
     *
     * NO TEST PINS THIS, AND NONE CAN. have_pmk_ is cleared either way, so
     * every observable behaviour is identical with the wipe deleted - a
     * mutation removing it survives the whole suite, which is recorded here
     * rather than hidden. It is the same defence-in-depth rule the destructor
     * follows, and its value is against a core dump, not against a caller. */
    secure_wipe(pmk_, sizeof pmk_);
    have_pmk_ = false;
    configured_ = true;
    return true;
  }

  /* Begin an association with this BSS.
   *
   * `snonce` must be UNPREDICTABLE AND FRESH FOR THIS ATTEMPT - see
   * Supplicant::start, which explains at length why this library takes it
   * rather than inventing it. */
  bool join(const BssEntry& bss, const uint8_t snonce[32], uint32_t now_ms) {
    if (!configured_) { fail(Failure::NotConfigured, 0); return false; }
    /* Refuse a BSS this station cannot finish with, rather than authenticating
     * and discovering it at the four-way. BssTable::select already filters on
     * this; join() is also reachable with a hand-picked entry. */
    if (security_ == Security::Wpa2Psk) {
      if (!have_pmk_) { fail(Failure::NoPmk, 0); return false; }
      if (!bss.info.rsn_ccmp_psk) { fail(Failure::AssocRefused, 0); return false; }
      /* The SNonce is only read on this path, so it is only required on this
       * path - but a WPA2 join without one would start the supplicant with a
       * nonce of whatever was in the buffer, which for a caller that
       * configures once and joins repeatedly is the PREVIOUS association's.
       * See the comment in configure() about why it is an argument at all. */
      if (!snonce) { fail(Failure::NotConfigured, 0); return false; }
    } else if (bss.info.privacy) {
      /* An open station cannot carry traffic on a BSS that encrypts it. The
       * Privacy bit is set by WEP, WPA and RSN alike, so this one test covers
       * every protected BSS without parsing any of them. Refusing here rather
       * than at the data plane is the difference between "no candidate" and
       * an association that succeeds and then passes nothing. */
      fail(Failure::AssocRefused, 0);
      return false;
    }

    /* THE QUEUE IS CLEARED. Without this, frames still queued for the BSS we
     * gave up on are transmitted at the one we just joined - addressed to the
     * old BSSID, on the new channel, after the radio has retuned. Every test
     * in station_sm_selftest drained the queue between steps, so nothing saw
     * it until a review built the case that does not. */
    tx_.clear();
    std::memcpy(bssid_, bss.info.bssid, 6);
    if (security_ == Security::Wpa2Psk) std::memcpy(snonce_, snonce, 32);
    channel_ = bss.info.channel;
    aid_ = 0;
    fail_ = Failure::None;
    status_ = 0;
    sup_.forget();
    state_ = State::Authenticating;
    tries_ = 0;
    last_beacon_ms_ = now_ms;
    send_auth(now_ms);
    return true;
  }

  /* Leave cleanly: tell the AP, drop the keys, and go back to Idle.
   *
   * An association this side simply abandons stays alive at the AP until it
   * times the station out, holding an AID and, on this project's own AP, a
   * slot in a seven-entry table. */
  void leave(uint16_t reason = 3) {
    if (state_ == State::Idle) return;
    if (state_ != State::Authenticating) {
      std::vector<uint8_t> m = build_deauth(own_, bssid_, reason);
      assign_seq(m, seq_.next());
      queue(std::move(m));
    }
    sup_.forget();
    state_ = State::Idle;
    fail_ = Failure::None;
    aid_ = 0;
  }

  /* One received frame. `len` is the true MPDU length with no FCS. */
  void on_rx(const uint8_t* frame, size_t len, uint32_t now_ms) {
    if (!frame || len < 24) return;
    if (state_ == State::Idle || state_ == State::Failed) return;

    const uint8_t fc0 = frame[0], fc1 = frame[1];
    const uint8_t* a1 = frame + 4;
    const uint8_t* a2 = frame + 10;

    /* EVERYTHING must come from the BSS we are talking to and be addressed to
     * this station (or broadcast). Without the addr2 check, any frame from any
     * AP on the channel drives this machine.
     *
     * THE DROPS ARE COUNTED. On real hardware this is the only address filter
     * in the system - the MT7612U RX path runs promiscuous - so most of a busy
     * channel lands here, and a station that connects to nothing has to be
     * able to say whether it heard its AP at all. */
    if (std::memcmp(a2, bssid_, 6) != 0) { rx_not_our_bss++; return; }
    const bool to_us = std::memcmp(a1, own_, 6) == 0;
    const bool bcast = (a1[0] & 0x01) != 0;
    if (!to_us && !bcast) { rx_not_for_us++; return; }

    /* A beacon from our own BSS is the liveness signal. Counted before the
     * switch because it is the one frame type that matters in every state. */
    if (fc0 == kFcBeacon || fc0 == kFcProbeResp) {
      beacons_rx++;
      last_beacon_ms_ = now_ms;
      return;
    }

    switch (fc0) {
      case kFcAuth:
        if (to_us) on_auth(frame, len, now_ms);
        return;
      case kFcAssocResp:
      case kFcReassocResp:
        if (to_us) on_assoc_resp(frame, len, now_ms);
        return;
      case kFcDeauth:
      case kFcDisassoc: {
        uint16_t reason = 0;

        /* ACCEPTED UNAUTHENTICATED, and that is a known cost rather than an
         * oversight: 802.11w is not implemented here, so there is no way to
         * tell a real deauthentication from a forged one, and a station that
         * ignored them would stay associated to an AP that has forgotten it.
         * Recorded in the scope document as the price of no MFP. */
        parse_reason(frame, len, &reason);
        fail(Failure::Deauthenticated, reason);
        return;
      }
      default:
        break;
    }

    /* Data frames: the only one this machine cares about is EAPOL. Anything
     * else is somebody's traffic, not a protocol error, so it is counted
     * separately from a frame that was addressed wrongly. */
    if (fc0 != kFcData && !is_qos_data(fc0)) { rx_ignored++; return; }
    if (!(fc1 & kFcFromDs) || (fc1 & kFcToDs)) { rx_ignored++; return; }
    /* The four-way is never protected: the keys it carries are what
     * protection would need. */
    if (fc1 & kFcProtected) { rx_ignored++; return; }
    const size_t hlen = data_hdr_len(fc0, fc1);
    if (len < hlen + kLlcSnapLen) { rx_malformed++; return; }
    const uint8_t* llc = frame + hlen;
    if (!(llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03)) {
      rx_ignored++;
      return;
    }
    if (!(llc[6] == 0x88 && llc[7] == 0x8e)) { rx_ignored++; return; }
    on_eapol(llc + kLlcSnapLen, len - hlen - kLlcSnapLen, now_ms);
  }

  /* Drive timeouts and retransmissions. Call it as often as convenient; it
   * does nothing until a deadline has passed. */
  void tick(uint32_t now_ms) {
    const uint32_t since = (uint32_t)(now_ms - last_tx_ms_);

    switch (state_) {
      case State::Authenticating:
        if (since < kMgmtTimeoutMs) return;
        if (tries_ >= kMaxTries) { fail(Failure::AuthTimeout, 0); return; }
        send_auth(now_ms);
        return;
      case State::Associating:
        if (since < kMgmtTimeoutMs) return;
        if (tries_ >= kMaxTries) { fail(Failure::AssocTimeout, 0); return; }
        send_assoc(now_ms);
        return;
      case State::FourWay:
        /* No retransmission: the authenticator owns that schedule. This is
         * only the give-up, and it is measured from the last thing that
         * actually moved the handshake forward. */
        if (since >= kHandshakeTimeoutMs) fail(Failure::HandshakeTimeout, 0);
        return;
      case State::Connected:
        /* BEACON SUPERVISION. Without it Connected has no exit but a deauth,
         * and an AP that is switched off leaves the station reporting a link
         * that does not exist - the caller sees keyed() forever and has no
         * hook to notice. A beacon is the cheapest liveness signal there is;
         * the station is already receiving them. */
        if ((uint32_t)(now_ms - last_beacon_ms_) >= kBeaconLossMs)
          fail(Failure::BeaconLost, 0);
        return;
      default:
        return;
    }
  }

  /* Take one frame to transmit, oldest first. Returns false when empty. */
  bool pop_tx(std::vector<uint8_t>* out) {
    if (tx_.empty()) return false;
    if (out) *out = std::move(tx_.front());
    tx_.erase(tx_.begin());
    return true;
  }

  size_t pending_tx() const { return tx_.size(); }
  static constexpr size_t tx_capacity() { return kMaxTxQueue; }
  State state() const { return state_; }
  Failure fail_reason() const { return fail_; }
  uint16_t status() const { return status_; }
  uint16_t aid() const { return aid_; }
  uint8_t channel() const { return channel_; }
  const uint8_t* bssid() const { return bssid_; }
  const Supplicant& supplicant() const { return sup_; }
  bool keyed() const { return state_ == State::Connected && sup_.ptk_valid(); }
  Security security() const { return security_; }
  /* Associated and able to carry data. On a WPA2 BSS that is keyed(); on an
   * open one there is no key, so a data plane gated on keyed() would never
   * transmit at all. This is the predicate a caller wants. */
  bool connected() const {
    return state_ == State::Connected &&
           (security_ == Security::Open || sup_.ptk_valid());
  }

  uint32_t auth_tx = 0;
  uint32_t assoc_tx = 0;
  uint32_t eapol_tx = 0;
  uint32_t eapol_rx = 0;
  uint32_t beacons_rx = 0;
  uint32_t rx_not_our_bss = 0;
  uint32_t rx_not_for_us = 0;
  uint32_t rx_ignored = 0;
  uint32_t rx_malformed = 0;
  uint32_t tx_dropped = 0;

 private:
  /* One place that enforces the bound, so no future sender can forget it. */
  void queue(std::vector<uint8_t> m) {
    if (tx_.size() >= kMaxTxQueue) { tx_dropped++; return; }
    tx_.push_back(std::move(m));
  }

  void send_auth(uint32_t now_ms) {
    std::vector<uint8_t> m = build_auth_req(own_, bssid_);
    assign_seq(m, seq_.next());
    queue(std::move(m));
    last_tx_ms_ = now_ms;
    tries_++;
    auth_tx++;
  }

  void send_assoc(uint32_t now_ms) {
    /* The band comes from the BSS's own DS Parameter Set, because the rate
     * set differs: a 5 GHz association request carrying 802.11b rates is
     * refused. Channel 0 means the beacon omitted the element, and 2.4 GHz is
     * the safe default - its rate set is the superset. */
    std::vector<uint8_t> m =
        build_assoc_req(own_, bssid_, ssid_,
                        /*rsn=*/security_ == Security::Wpa2Psk,
                        /*five_ghz=*/channel_ > 14);
    /* build_assoc_req returns an empty vector for an SSID it cannot encode.
     * Sending a truncated association request would be worse than failing. */
    if (m.empty()) { fail(Failure::AssocRefused, 0); return; }
    assign_seq(m, seq_.next());
    queue(std::move(m));
    last_tx_ms_ = now_ms;
    tries_++;
    assoc_tx++;
  }

  void on_auth(const uint8_t* frame, size_t len, uint32_t now_ms) {
    AuthFields a;

    if (state_ != State::Authenticating) return;
    if (!parse_auth(frame, len, &a)) return;
    /* Open System only. A Shared Key response is a four-frame exchange this
     * does not implement, and treating its sequence 2 as success would send an
     * association request into a state the AP is not in. */
    if (a.algorithm != 0) { fail(Failure::AuthRefused, a.status); return; }
    if (a.seq != 2) return;
    if (a.status != 0) { fail(Failure::AuthRefused, a.status); return; }

    state_ = State::Associating;
    tries_ = 0;
    send_assoc(now_ms);
  }

  void on_assoc_resp(const uint8_t* frame, size_t len, uint32_t now_ms) {
    AssocRespFields r;

    if (state_ != State::Associating) return;
    if (!parse_assoc_resp(frame, len, &r)) return;
    if (r.status != 0) { fail(Failure::AssocRefused, r.status); return; }
    /* AID 0 is not a valid association identifier; an AP that answers success
     * with one has not actually allocated anything. */
    if (r.aid == 0) { fail(Failure::AssocRefused, r.status); return; }

    aid_ = r.aid;
    last_tx_ms_ = now_ms;
    last_beacon_ms_ = now_ms;
    /* An open association is complete the moment the AP accepts it - there is
     * no key exchange to wait for, so FourWay would be a state nothing could
     * ever leave. */
    if (security_ == Security::Open) {
      state_ = State::Connected;
      return;
    }
    state_ = State::FourWay;
    sup_.start(*crypto_, pmk_, own_, bssid_, snonce_);
  }

  void on_eapol(const uint8_t* eapol, size_t len, uint32_t now_ms) {
    std::vector<uint8_t> reply;

    /* An EAPOL-Key frame on an open link is never ours: the supplicant was
     * never started, so it holds no PMK and has nothing to verify a MIC
     * against. Counted as ignored rather than dropped silently, because "the
     * AP is trying to key us and we are configured open" is a configuration
     * mismatch worth being able to see. */
    if (security_ != Security::Wpa2Psk) { rx_ignored++; return; }
    if (state_ != State::FourWay && state_ != State::Connected) return;
    eapol_rx++;
    const Supplicant::Verdict v = sup_.on_eapol(eapol, len, &reply);
    if (v != Supplicant::Verdict::Reply &&
        v != Supplicant::Verdict::Retransmit)
      return;
    if (reply.empty()) return;

    std::vector<uint8_t> m = data_hdr_to_ds(bssid_, own_, bssid_,
                                            /*protect=*/false, seq_.next());
    append_llc_snap(m, 0x888e);
    m.insert(m.end(), reply.begin(), reply.end());
    queue(std::move(m));
    eapol_tx++;

    /* The deadline moves only when the handshake moved. A retransmission we
     * answered again is not progress, and letting it push the give-up out
     * would let a stuck authenticator hold this state open forever. */
    if (v == Supplicant::Verdict::Reply) last_tx_ms_ = now_ms;
    if (sup_.state() == Supplicant::State::Done && sup_.ptk_valid() &&
        state_ == State::FourWay) {
      state_ = State::Connected;
      last_beacon_ms_ = now_ms;
    }
  }

  void fail(Failure why, uint16_t status) {
    state_ = State::Failed;
    fail_ = why;
    status_ = status;
  }

  CryptoOps* crypto_ = nullptr;
  Security security_ = Security::Wpa2Psk;
  bool configured_ = false;
  State state_ = State::Idle;
  Failure fail_ = Failure::None;
  std::string ssid_;
  uint8_t own_[6] = {0};
  uint8_t bssid_[6] = {0};
  uint8_t snonce_[32] = {0};
  uint8_t pmk_[32] = {0};
  bool have_pmk_ = false;
  uint8_t channel_ = 0;
  uint16_t aid_ = 0;
  uint16_t status_ = 0;
  int tries_ = 0;
  uint32_t last_tx_ms_ = 0;
  uint32_t last_beacon_ms_ = 0;
  SeqCounter seq_;
  Supplicant sup_;
  std::vector<std::vector<uint8_t>> tx_;
};

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_STATION_SM_H */
