/* rx_mpdu.h - how long is the MPDU, really?
 *
 * `Packet::Data` does NOT always end where the 802.11 frame ends. Every
 * Realtek generation sets the MAC's append-FCS bit at init, so the four
 * trailing FCS bytes are part of the delivered buffer; MT7612U's MAC strips
 * them and they are not. `RxAtrib.fcs_present` says which, and
 * `src/RxPacket.h` documents that keeping the FCS where the hardware supplies
 * it is deliberate - the beamforming decoder reads it - so trimming is the
 * consumer's job, not the library's.
 *
 * A consumer that ignores this is not merely four bytes out. It is four bytes
 * out IN A LENGTH THAT FEEDS A MIC CHECK:
 *
 *   - CCMP decrypt takes the MPDU length and derives the ciphertext length
 *     from it. Four extra bytes move the expected MIC four bytes late, so the
 *     tag never matches and every single frame fails to authenticate.
 *   - The AP harnesses' data-plane ledger counts exactly that as "MIC
 *     failures", which is the symptom of a forged or tampered frame. So on
 *     Realtek the ledger would report an attack where there is a length bug -
 *     the most misleading failure mode available, and one nobody would have
 *     questioned because the ledger is what the on-air cells trust.
 *
 * The harnesses have only ever run on MT7612U, where fcs_present is false and
 * the bug is dormant. It would have woken up at Phase 6, against the Realtek
 * arm, as a flood of MIC failures on a link that was working.
 *
 * ONE definition, included by both harnesses. The same fact open-coded in two
 * places is how `fc0 == 0x88` survived being fixed in the shared module.
 */
#ifndef DEVOURER_TEST_RX_MPDU_H
#define DEVOURER_TEST_RX_MPDU_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "RxPacket.h"

namespace devourer {
namespace test {

/* The pure form, so it is testable without fabricating a Packet.
 *
 * Returns 0 rather than underflowing when a buffer is shorter than the FCS it
 * claims to carry. That is not hypothetical politeness: `raw` is unsigned, so
 * `raw - 4` on a 2-byte runt becomes an enormous length, and every bounds
 * check downstream that compares against it would pass. A truncated frame is
 * a thing a radio delivers. */
inline size_t mpdu_len(size_t raw, bool fcs_present) {
  if (!fcs_present)
    return raw;
  return raw >= 4 ? raw - 4 : 0;
}

inline size_t mpdu_len(const Packet &p) {
  return mpdu_len(p.Data.size(), p.RxAtrib.fcs_present);
}

} // namespace test
} // namespace devourer

#endif /* DEVOURER_TEST_RX_MPDU_H */
