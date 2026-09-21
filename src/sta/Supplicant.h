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
 *   - a well-formed EAPOL-Key this role does not handle ignored
 *   - our own CryptoOps failing, which is not the
 *     frame's fault and must not read as an attack      crypto_errors
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
 *
 * AND A THIRD, FOUND BY REVIEW OF THE FIRST DRAFT OF THIS FILE. Message 1
 * carries no MIC - anyone who can hear the BSSID can build one - and the
 * first draft let it advance the replay counter. One forged frame quoting
 * 2^64-1 therefore refused every genuine EAPOL-Key for the rest of the
 * association, silently: the station stayed Connected and keyed while its
 * rekey path was dead, so the next GTK rotation simply stopped working. The
 * counter now advances ONLY where a MIC has verified. What an unauthenticated
 * message 1 can still do is replace the candidate PTK of a handshake in
 * flight - wpa_supplicant has the same exposure, and it costs an association
 * attempt, not a working link.
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
  };

  /* What on_eapol() did with the frame. `Reply` means `out` holds an EAPOL
   * frame body for the caller to wrap in a to-DS data frame and send. */
  enum class Verdict : uint8_t {
    Ignored,      /* a well-formed EAPOL-Key this role does not handle */
    Malformed,
    CryptoError,  /* our own CryptoOps failed; not the frame's fault */
    MicFailed,
    Replayed,
    Retransmit,   /* equal counter, same message: `out` holds the same reply */
    OutOfState,
    Reply,        /* `out` holds msg2, msg4, or group msg2 */
  };

  ~Supplicant() { forget(); }

  /* Begin a handshake.
   *
   * `snonce` IS SUPPLIED BY THE CALLER, and this is deliberate rather than
   * lazy. `libdevourer` has no random-number dependency and CryptoOps offers
   * none, so the alternative is a stub that looks like entropy and is not.
   * Making it an argument puts the requirement where somebody has to read it:
   * THE SNONCE MUST BE UNPREDICTABLE, AND FRESH PER ASSOCIATION. A constant, a
   * counter or a timestamp makes the PTK derivable from the air by anyone who
   * knows the PSK - which on a PSK network is every other station.
   */
  void start(CryptoOps& crypto, const uint8_t pmk[32], const uint8_t own[6],
             const uint8_t bssid[6], const uint8_t snonce[32]) {
    forget();
    crypto_ = &crypto;
    std::memcpy(pmk_, pmk, 32);
    std::memcpy(own_, own, 6);
    std::memcpy(bssid_, bssid, 6);
    std::memcpy(snonce_, snonce, 32);
    state_ = State::WaitMsg1;
  }

  /* Drop every key this object holds. Called by start() and the destructor;
   * a caller that is finished early may call it directly. */
  void forget() {
    secure_wipe(pmk_, sizeof pmk_);
    secure_wipe(snonce_, sizeof snonce_);
    secure_wipe(anonce_, sizeof anonce_);
    secure_wipe(ptk_, sizeof ptk_);
    secure_wipe(cand_ptk_, sizeof cand_ptk_);
    secure_wipe(gtk_, sizeof gtk_);
    if (!last_reply_.empty())
      secure_wipe(last_reply_.data(), last_reply_.size());
    last_reply_.clear();
    state_ = State::Idle;
    crypto_ = nullptr;
    rx_replay_ = 0;
    rx_replay_set_ = false;
    answered_replay_ = 0;
    answered_ = false;
    answered_kind_ = Kind::None;
    ptk_valid_ = false;
    cand_valid_ = false;
    gtk_valid_ = false;
    gtk_len_ = 0;
    gtk_key_id_ = 0;
  }

  /* Feed one EAPOL frame — the bytes after the LLC/SNAP header of an 0x888E
   * data frame. */
  Verdict on_eapol(const uint8_t* eapol, size_t len,
                   std::vector<uint8_t>* out) {
    EapolKey k;

    if (!crypto_ || state_ == State::Idle) return note(Verdict::OutOfState);
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

    const Kind kind = classify(k);
    if (kind == Kind::None) return note(Verdict::Ignored);

    /* THE REPLAY GATE, ahead of every branch below so no message type added
     * later can forget it.
     *
     * `rx_replay_` is the last counter this station AUTHENTICATED - it moves
     * in on_msg3 and on_group1 and nowhere else. Message 1 has no MIC, so
     * letting it move this would let one forged frame refuse every genuine
     * message for the rest of the association. That is not hypothetical: the
     * first draft did exactly that, and two independent reviews found it.
     *
     * Strictly greater is required to install anything. Equal is answered
     * with the cached reply and installs NOTHING, which is what lets an
     * authenticator that does not increment on retransmission (802.11-2016
     * 12.7.6.4 permits either) finish its handshake; installing on one is the
     * PR #335 defect. The cached reply is returned only for the SAME message
     * type that produced it, so a msg1 replayed at a msg3's counter does not
     * get a msg4 back. */
    if (rx_replay_set_ && k.replay <= rx_replay_)
      return retransmit_or_replay(k, kind, out);
    /* A repeat of something answered but never authenticated: a retransmitted
     * message 1. */
    if (answered_ && k.replay == answered_replay_ && kind == answered_kind_ &&
        !last_reply_.empty()) {
      retransmits++;
      if (out) *out = last_reply_;
      return Verdict::Retransmit;
    }

    switch (kind) {
      case Kind::Msg1: return on_msg1(k, out);
      case Kind::Msg3: return on_msg3(k, out);
      case Kind::Group1: return on_group1(k, out);
      default: return note(Verdict::Ignored);
    }
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
  /* The last counter this station AUTHENTICATED, not the last it saw. */
  uint64_t replay_counter() const { return rx_replay_; }

  uint32_t mic_failures = 0;
  uint32_t replays = 0;
  uint32_t retransmits = 0;
  uint32_t malformed = 0;
  uint32_t out_of_state = 0;
  uint32_t ignored = 0;
  uint32_t crypto_errors = 0;

 private:
  /* An MSDU is 2304 bytes; key data larger than that never crossed a link. */
  static constexpr size_t kMaxKeyData = 2048;

  enum class Kind : uint8_t { None, Msg1, Msg3, Group1 };

  static Kind classify(const EapolKey& k) {
    if (k.pairwise()) {
      if (k.ack() && !k.has_mic()) return Kind::Msg1;
      if (k.ack() && k.has_mic() && k.install()) return Kind::Msg3;
      return Kind::None;
    }
    if (k.ack() && k.has_mic() && k.secure()) return Kind::Group1;
    return Kind::None;
  }

  Verdict note(Verdict v) {
    switch (v) {
      case Verdict::Malformed: malformed++; break;
      case Verdict::CryptoError: crypto_errors++; break;
      case Verdict::MicFailed: mic_failures++; break;
      case Verdict::Replayed: replays++; break;
      case Verdict::OutOfState: out_of_state++; break;
      case Verdict::Ignored: ignored++; break;
      default: break;
    }
    return v;
  }

  Verdict retransmit_or_replay(const EapolKey& k, Kind kind,
                               std::vector<uint8_t>* out) {
    if (k.replay == rx_replay_ && kind == answered_kind_ && answered_ &&
        k.replay == answered_replay_ && !last_reply_.empty()) {
      retransmits++;
      if (out) *out = last_reply_;
      return Verdict::Retransmit;
    }
    return note(Verdict::Replayed);
  }

  /* Message 1: ANonce, no MIC, nothing to verify.
   *
   * NOTHING INSTALLED IS TOUCHED, AND THE STATE DOES NOT GO BACKWARDS. The
   * PTK is derived into a CANDIDATE and the live one is left alone; a station
   * that is already Done stays Done, because on_group1 requires Done and an
   * unauthenticated frame must not be able to switch the group-rekey path
   * off. The candidate is promoted in on_msg3, after a MIC computed with it
   * verifies.
   */
  Verdict on_msg1(const EapolKey& k, std::vector<uint8_t>* out) {
    std::memcpy(anonce_, k.nonce, 32);
    if (!derive_ptk(*crypto_, pmk_, bssid_, own_, anonce_, snonce_, cand_ptk_))
      return note(Verdict::CryptoError);
    cand_valid_ = true;

    std::vector<uint8_t> rsn;
    append_rsn_ccmp_psk(rsn);
    /* The key data carries the whole RSN element, EID and length included -
     * a conforming authenticator compares it with the one in the association
     * request. */
    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiPairwise | kKiMic), 16, k.replay,
        snonce_, nullptr, rsn.data(), rsn.size(), crypto_, cand_ptk_);
    if (e.empty()) return note(Verdict::CryptoError);

    answer(Kind::Msg1, k.replay, e);
    if (state_ == State::WaitMsg1) state_ = State::WaitMsg3;
    if (out) *out = e;
    return Verdict::Reply;
  }

  /* Message 3: the GTK, and the confirmation that the authenticator holds the
   * same PTK. Everything is checked before anything is installed. */
  Verdict on_msg3(const EapolKey& k, std::vector<uint8_t>* out) {
    /* Gated on the CANDIDATE, not on the state: a message 1 that arrived on a
     * working association leaves the state at Done deliberately, and its
     * message 3 still has to be processable. */
    if (!cand_valid_) return note(Verdict::OutOfState);

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
    if (!k.encrypted() || k.key_data_len == 0) {
      /* Unencrypted key data in message 3 is a downgrade - the GTK would be
       * in the clear - and message 3 with no key data at all carries no GTK,
       * which in RSN it always does. Both are refused rather than completed
       * into a station that is keyed with no group key.
       *
       * DELETING THIS CHECK CHANGES NO OUTCOME, and that is recorded rather
       * than hidden: a mutation removing it survives the whole test suite,
       * because plaintext key data fails the AES unwrap below on its
       * integrity check and an absent one fails the length check. It stays
       * because "the unwrap happened to refuse it" is a different reason from
       * "we do not accept an unprotected GTK", and only one of those survives
       * a future edit to the unwrap path. */
      return note(Verdict::Malformed);
    }
    if (k.key_data_len < 16 || (k.key_data_len % 8) != 0)
      return note(Verdict::Malformed);
    std::vector<uint8_t> plain(k.key_data_len - 8);
    if (!crypto_->aes_key_unwrap(cand_ptk_ + 16, 16, k.key_data,
                                 k.key_data_len, plain.data()))
      return note(Verdict::Malformed);
    /* Absent and Malformed are both refusals HERE - see the note at
     * KdeResult. A message 3 with no GTK would otherwise complete the
     * handshake into a station that can decrypt no broadcast at all, with
     * nothing counted and nothing to look at. */
    if (find_gtk_kde(plain.data(), plain.size(), &g) != KdeResult::Found) {
      secure_wipe(plain.data(), plain.size());
      return note(Verdict::Malformed);
    }

    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiPairwise | kKiMic | kKiSecure), 16,
        k.replay, nullptr, nullptr, nullptr, 0, crypto_, cand_ptk_);
    if (e.empty()) {
      secure_wipe(plain.data(), plain.size());
      return note(Verdict::CryptoError);
    }

    /* INSTALL LAST. Up to here a failure has cost nothing. */
    std::memcpy(ptk_, cand_ptk_, 48);
    ptk_valid_ = true;
    install_gtk(g);
    authenticated(k.replay);
    answer(Kind::Msg3, k.replay, e);
    state_ = State::Done;
    secure_wipe(plain.data(), plain.size());
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
    if (find_gtk_kde(plain.data(), plain.size(), &g) != KdeResult::Found) {
      secure_wipe(plain.data(), plain.size());
      return note(Verdict::Malformed);
    }

    std::vector<uint8_t> e = build_eapol_key(
        (uint16_t)(kKeyDescVersionCcmp | kKiMic | kKiSecure), 16, k.replay,
        nullptr, nullptr, nullptr, 0, crypto_, ptk_);
    if (e.empty()) {
      secure_wipe(plain.data(), plain.size());
      return note(Verdict::CryptoError);
    }

    install_gtk(g);
    authenticated(k.replay);
    answer(Kind::Group1, k.replay, e);
    secure_wipe(plain.data(), plain.size());
    if (out) *out = e;
    return Verdict::Reply;
  }

  void install_gtk(const GtkKde& g) {
    std::memcpy(gtk_, g.gtk, g.gtk_len);
    gtk_len_ = g.gtk_len;
    gtk_key_id_ = g.key_id;
    gtk_valid_ = true;
  }

  /* THE ONLY PLACE rx_replay_ MOVES, and both call sites have verified a MIC
   * before reaching it. */
  void authenticated(uint64_t replay) {
    rx_replay_ = replay;
    rx_replay_set_ = true;
  }

  /* The retransmission cache, kept with the message type it answered so a
   * different message quoting the same counter cannot collect it. */
  void answer(Kind kind, uint64_t replay, const std::vector<uint8_t>& reply) {
    answered_kind_ = kind;
    answered_replay_ = replay;
    answered_ = true;
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
  bool cand_valid_ = false;
  bool gtk_valid_ = false;
  uint64_t rx_replay_ = 0;
  bool rx_replay_set_ = false;
  uint64_t answered_replay_ = 0;
  bool answered_ = false;
  Kind answered_kind_ = Kind::None;
  std::vector<uint8_t> last_reply_;
};

}  // namespace sta
}  // namespace devourer

#endif /* DEVOURER_STA_SUPPLICANT_H */
