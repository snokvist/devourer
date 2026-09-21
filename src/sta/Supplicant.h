/* Supplicant — the station half of the WPA2-PSK key exchange.
 *
 * The opposite role to the authenticator in tests/ap_wpa2.cpp, and written
 * from the standard rather than by mirroring that code, so the two are
 * independent implementations that a test can run against each other.
 *
 * Reactive and timer-free. The authenticator retransmits message 1 and
 * message 3 (802.11-2016 12.7.6.4); a supplicant answers what arrives and
 * never retransmits on its own, so there is no clock in here and no schedule
 * to get wrong.
 *
 * WHAT IT REFUSES, and why each refusal is a counter rather than a silent
 * drop - a handshake that does not complete has to say which rule stopped it,
 * or the only diagnosis available is "it did not associate":
 *
 *   - a MIC that does not verify                        mic_failures
 *   - a key replay counter that does not advance        replays
 *   - a retransmission (equal counter): answered again, installs nothing
 *                                                       retransmits
 *   - a message arriving in a state that cannot use it  out_of_state
 *   - anything malformed, over-long, or a descriptor
 *     version whose MIC is a different algorithm        malformed
 *
 * THE TWO DEFECTS THIS IS BUILT NOT TO REPEAT, both of which shipped in a
 * reviewed PR (#335) and are the phase's acceptance criteria:
 *
 *   1. A FORGED EAPOL-Key MIC MUST BE REJECTED. Message 3 carries the GTK and
 *      confirms the PTK; accepting it without a verified MIC hands an attacker
 *      the ability to install key material. Nothing here touches the installed
 *      keys until a MIC verifies under a key derived from the PMK.
 *
 *   2. AN EQUAL-COUNTER REPLAYED GROUP REKEY MUST BE REJECTED. Replaying a
 *      captured group message 1 reinstalls an OLD GTK - which, with its own PN
 *      space reset, is keystream reuse across every group frame since. Only a
 *      STRICTLY GREATER counter installs anything.
 */
#ifndef DEVOURER_STA_SUPPLICANT_H
#define DEVOURER_STA_SUPPLICANT_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "sta/CryptoOps.h"
#include "sta/Dot11.h"
#include "sta/Eapol.h"

namespace devourer {
namespace sta {

class Supplicant {
 public:
  enum class State : uint8_t {
    Idle,       /* not started */
    WaitMsg1,   /* started; nothing derived yet */
    WaitMsg3,   /* msg1 seen, candidate PTK derived, msg2 sent */
    Done,       /* PTK and GTK installed, msg4 sent */
    Failed,     /* an unrecoverable protocol error; start() again */
  };

  /* What on_eapol() did with the frame. `Reply` means `out` holds an EAPOL
   * frame body for the caller to wrap in a to-DS data frame and send. */
  enum class Verdict : uint8_t {
    Ignored,      /* not an EAPOL-Key this role handles */
    Malformed,
    MicFailed,
    Replayed,
    Retransmit,   /* equal counter: `out` holds the same reply as before */
    OutOfState,
    Reply,        /* `out` holds msg2, msg4, or group msg2 */
  };

  /* Begin a handshake.
   *
   * `snonce` IS SUPPLIED BY THE CALLER, and this is deliberate rather than
   * lazy. `libdevourer` has no random-number dependency and CryptoOps offers
   * none, so the alternative is a stub that looks like entropy and is not.
   * Making it an argument puts the requirement where somebody has to read it:
   * THE SNONCE MUST BE UNPREDICTABLE. A constant, a counter or a timestamp
   * makes the PTK derivable from the air by anyone who knows the PSK - which
   * on a PSK network is every other station.
   */
  void start(CryptoOps& crypto, const uint8_t pmk[32], const uint8_t own[6],
             const uint8_t bssid[6], const uint8_t snonce[32]) {
    crypto_ = &crypto;
    std::memcpy(pmk_, pmk, 32);
    std::memcpy(own_, own, 6);
    std::memcpy(bssid_, bssid, 6);
    std::memcpy(snonce_, snonce, 32);
    state_ = State::WaitMsg1;
    rx_replay_ = 0;
    rx_replay_set_ = false;
    ptk_valid_ = false;
    gtk_valid_ = false;
    last_reply_.clear();
    std::memset(ptk_, 0, sizeof ptk_);
    std::memset(cand_ptk_, 0, sizeof cand_ptk_);
    std::memset(anonce_, 0, sizeof anonce_);
  }

  /* Feed one EAPOL frame — the bytes after the LLC/SNAP header of an 0x888E
   * data frame. */
  Verdict on_eapol(const uint8_t* eapol, size_t len,
                   std::vector<uint8_t>* out) {
    EapolKey k;

    if (!crypto_ || state_ == State::Idle || state_ == State::Failed)
      return note(Verdict::OutOfState);
    if (!parse_eapol_key(eapol, len, &k)) return note(Verdict::Malformed);
    /* Version 1 is TKIP's HMAC-MD5 and version 3 is AES-CMAC. Treating either
     * as version 2 means computing the MIC with the wrong algorithm, and the
     * only symptom would be "the MIC failed" — a diagnosis that sends the
     * reader looking at the key rather than at the cipher suite. */
    if (k.version != kKeyDescVersionCcmp) return note(Verdict::Malformed);
    /* A Request bit set is a SUPPLICANT-to-authenticator frame. Receiving one
     * means something is echoing our own traffic back at us. */
    if (k.request()) return note(Verdict::Malformed);
    /* Key data longer than an MSDU can hold is not a frame anyone sent. */
    if (k.key_data_len > kMaxKeyData) return note(Verdict::Malformed);

    /* THE REPLAY GATE, ahead of every branch below so no message type can be
     * added later that forgets it. Strictly greater installs; equal is a
     * retransmission and is answered but installs NOTHING; lower is refused.
     *
     * Answering an equal counter is what lets an authenticator that does not
     * increment on retransmission (802.11-2016 12.7.6.4 permits either) finish
     * its handshake. Installing on one is the PR #335 defect. */
    if (rx_replay_set_ && k.replay <= rx_replay_) {
      if (k.replay == rx_replay_ && !last_reply_.empty()) {
        retransmits++;
        if (out) *out = last_reply_;
        return Verdict::Retransmit;
      }
      return note(Verdict::Replayed);
    }

    if (k.pairwise()) {
      if (k.ack() && !k.has_mic()) return on_msg1(k, out);
      if (k.ack() && k.has_mic() && k.install()) return on_msg3(k, out);
      return note(Verdict::Ignored);
    }
    if (k.ack() && k.has_mic() && k.secure()) return on_group1(k, out);
    return note(Verdict::Ignored);
  }

  State state() const { return state_; }
  bool ptk_valid() const { return ptk_valid_; }
  bool gtk_valid() const { return gtk_valid_; }
  /* KCK[0:16] KEK[16:32] TK[32:48] */
  const uint8_t* ptk() const { return ptk_; }
  const uint8_t* tk() const { return ptk_ + 32; }
  const uint8_t* gtk() const { return gtk_; }
  size_t gtk_len() const { return gtk_len_; }
  uint8_t gtk_key_id() const { return gtk_key_id_; }
  uint64_t replay_counter() const { return rx_replay_; }
  const uint8_t* snonce() const { return snonce_; }

  uint32_t mic_failures = 0;
  uint32_t replays = 0;
  uint32_t retransmits = 0;
  uint32_t malformed = 0;
  uint32_t out_of_state = 0;

 private:
  /* An MSDU is 2304 bytes; key data larger than that never crossed a link. */
  static constexpr size_t kMaxKeyData = 2048;

  Verdict note(Verdict v) {
    switch (v) {
      case Verdict::Malformed: malformed++; break;
      case Verdict::MicFailed: mic_failures++; break;
      case Verdict::Replayed: replays++; break;
      case Verdict::OutOfState: out_of_state++; break;
      default: break;
    }
    return v;
  }

  /* Message 1: ANonce, no MIC, nothing to verify. Accepted in WaitMsg1 and in
   * Done — an authenticator may rekey the pairwise key at any time.
   *
   * NOTHING INSTALLED IS TOUCHED. The PTK is derived into a CANDIDATE and the
   * live one is left alone, because message 1 is unauthenticated: anyone can
   * send it, and a station that re-derived over its working key would lose the
   * link to a single forged frame. The candidate is promoted in on_msg3, after
   * a MIC computed with it verifies.
   */
  Verdict on_msg1(const EapolKey& k, std::vector<uint8_t>* out) {
    if (state_ != State::WaitMsg1 && state_ != State::WaitMsg3 &&
        state_ != State::Done)
      return note(Verdict::OutOfState);
    std::memcpy(anonce_, k.nonce, 32);
    if (!derive_ptk(*crypto_, pmk_, bssid_, own_, anonce_, snonce_, cand_ptk_))
      return note(Verdict::Malformed);

    std::vector<uint8_t> rsn;
    append_rsn_ccmp_psk(rsn);
    /* append_rsn_ccmp_psk writes a whole element; the key data carries the
     * element, EID and length included. */
    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiPairwise | kKiMic), 16, k.replay,
        snonce_, nullptr, rsn.data(), rsn.size(), crypto_, cand_ptk_);
    if (e.empty()) return note(Verdict::Malformed);

    accept(k.replay, e);
    state_ = State::WaitMsg3;
    if (out) *out = e;
    return Verdict::Reply;
  }

  /* Message 3: the GTK, and the confirmation that the authenticator holds the
   * same PTK. Everything is checked before anything is installed. */
  Verdict on_msg3(const EapolKey& k, std::vector<uint8_t>* out) {
    if (state_ != State::WaitMsg3) return note(Verdict::OutOfState);

    /* 12.7.6.4: the ANonce in message 3 must equal the one in message 1, or
     * the authenticator is not the party we derived against. Checked BEFORE
     * the MIC so a mix-and-match is refused as what it is rather than as a
     * key mismatch. */
    if (std::memcmp(k.nonce, anonce_, 32) != 0)
      return note(Verdict::Malformed);

    /* THE FORGERY GATE. Verified with the CANDIDATE KCK, which exists only
     * because message 1 named an ANonce and we hold the PMK. */
    if (!eapol_mic_ok(*crypto_, cand_ptk_, k)) return note(Verdict::MicFailed);

    /* Message 3's key data is AES-key-wrapped with the KEK. An unwrap is an
     * integrity check in its own right, so a failure here is not a decode
     * hiccup — it is a frame whose MIC verified but whose key data did not,
     * which should not be possible and is refused rather than parsed. */
    GtkKde g;
    bool have_gtk = false;
    if (k.encrypted() && k.key_data_len) {
      if (k.key_data_len < 16 || (k.key_data_len % 8) != 0)
        return note(Verdict::Malformed);
      std::vector<uint8_t> plain(k.key_data_len - 8);
      if (!crypto_->aes_key_unwrap(cand_ptk_ + 16, 16, k.key_data,
                                   k.key_data_len, plain.data()))
        return note(Verdict::Malformed);
      have_gtk = find_gtk_kde(plain.data(), plain.size(), &g);
    } else if (k.key_data_len) {
      /* Unencrypted key data in message 3 is a downgrade: the GTK would be in
       * the clear. Refuse rather than read it. */
      return note(Verdict::Malformed);
    }

    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiPairwise | kKiMic | kKiSecure), 16,
        k.replay, nullptr, nullptr, nullptr, 0, crypto_, cand_ptk_);
    if (e.empty()) return note(Verdict::Malformed);

    /* INSTALL LAST. Up to here a failure has cost nothing. */
    std::memcpy(ptk_, cand_ptk_, 48);
    ptk_valid_ = true;
    if (have_gtk) install_gtk(g);
    accept(k.replay, e);
    state_ = State::Done;
    if (out) *out = e;
    return Verdict::Reply;
  }

  /* Group key handshake, message 1: a new GTK under the KEK, MIC'd with the
   * KCK. Needs an installed PTK; one arriving before the 4-way has finished is
   * out of state, not merely unverifiable. */
  Verdict on_group1(const EapolKey& k, std::vector<uint8_t>* out) {
    if (state_ != State::Done || !ptk_valid_)
      return note(Verdict::OutOfState);
    if (!eapol_mic_ok(*crypto_, ptk_, k)) return note(Verdict::MicFailed);
    if (!k.encrypted() || k.key_data_len < 16 || (k.key_data_len % 8) != 0)
      return note(Verdict::Malformed);

    std::vector<uint8_t> plain(k.key_data_len - 8);
    if (!crypto_->aes_key_unwrap(ptk_ + 16, 16, k.key_data, k.key_data_len,
                                 plain.data()))
      return note(Verdict::Malformed);
    GtkKde g;
    if (!find_gtk_kde(plain.data(), plain.size(), &g))
      return note(Verdict::Malformed);

    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiMic | kKiSecure), 16, k.replay,
        nullptr, nullptr, nullptr, 0, crypto_, ptk_);
    if (e.empty()) return note(Verdict::Malformed);

    install_gtk(g);
    accept(k.replay, e);
    if (out) *out = e;
    return Verdict::Reply;
  }

  void install_gtk(const GtkKde& g) {
    std::memcpy(gtk_, g.gtk, g.gtk_len);
    gtk_len_ = g.gtk_len;
    gtk_key_id_ = g.key_id;
    gtk_valid_ = true;
  }

  /* One place that advances the replay counter and caches the reply, so the
   * two can never disagree about which message the cached reply answers. */
  void accept(uint64_t replay, const std::vector<uint8_t>& reply) {
    rx_replay_ = replay;
    rx_replay_set_ = true;
    last_reply_ = reply;
  }

  CryptoOps* crypto_ = nullptr;
  State state_ = State::Idle;
  uint8_t pmk_[32] = {0};
  uint8_t own_[6] = {0};
  uint8_t bssid_[6] = {0};
  uint8_t snonce_[32] = {0};
  uint8_t anonce_[32] = {0};
  uint8_t ptk_[48] = {0};
  uint8_t cand_ptk_[48] = {0};
  uint8_t gtk_[32] = {0};
  size_t gtk_len_ = 0;
  uint8_t gtk_key_id_ = 0;
  bool ptk_valid_ = false;
  bool gtk_valid_ = false;
  uint64_t rx_replay_ = 0;
  bool rx_replay_set_ = false;
  std::vector<uint8_t> last_reply_;
};

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_SUPPLICANT_H */
