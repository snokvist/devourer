#ifndef DEVOURER_AC_TX_QUEUE_H
#define DEVOURER_AC_TX_QUEUE_H

#include <algorithm>
#include <cstdint>

namespace devourer {

/* The access-category (AC) queue registers behind DeviceConfig::Tx::
 * data_on_be / be_fast_retry / ac_lifetime_us. The addresses and field
 * layouts are the halmac 88xx ones, identical on the RTL8733B, Jaguar2
 * (8822B/8821C) and Jaguar3 (8822C/8822E), the three backends that apply
 * them. Pure arithmetic plus one generic apply, so the headless selftest
 * (tests/ac_tx_queue_selftest.cpp) pins the bits without a device. */

inline constexpr uint16_t kRegEdcaBeParam = 0x0508;
/* REG_LIFETIME_EN: bits 0..3 enable the packet lifetime of the VO, VI, BE
 * and BK queues. The other bits belong to other features (bit 4
 * EN_CTRL_RTYBIT, bit 5 the BA parser) and are preserved. The MGMT/HIGH
 * queues have no lifetime here. */
inline constexpr uint16_t kRegLifetimeEn = 0x0426;
inline constexpr uint8_t kLifetimeEnAc = 0x0f;
/* REG_PKT_LIFE_TIME: [15:0] the VO/VI lifetime, [31:16] the BE/BK one. */
inline constexpr uint16_t kRegPktLifeTime = 0x04c0;
/* Lifetime unit: 256 us on all three generations (the vendor drivers'
 * tx_aclt_unit_factor 8 x 32 us; halmac converts with `us >> 8`). */
inline constexpr unsigned kLifetimeUnitUs = 256;

/* An EDCA parameter word ([7:0] AIFS, [11:8] ECWmin, [15:12] ECWmax,
 * [26:16] TXOP limit in 32 us units) with the TXOP limit cleared and ECWmax
 * pulled down to ECWmin; AIFS and ECWmin are kept. Retries of an
 * ACK-requested frame then re-air after one AIFS plus a CWmin backoff each,
 * instead of holding a TXOP and doubling the window up to CWmax. */
constexpr uint32_t edca_fast_retry(uint32_t edca) {
  return (edca & 0x00000fffu) | ((edca & 0x00000f00u) << 4);
}

/* REG_PKT_LIFE_TIME for a lifetime in microseconds, the same value in both
 * halves: rounded up to the unit, clamped to the 16-bit field (~16.8 s).
 * 0 us stays 0 (callers do not enable a zero lifetime). */
constexpr uint32_t pkt_life_time_reg(unsigned us) {
  const uint32_t units = static_cast<uint32_t>(std::min<uint64_t>(
      0xffffu, (uint64_t{us} + kLifetimeUnitUs - 1) / kLifetimeUnitUs));
  return (units << 16) | units;
}

/* Programs the AC-queue options of `tx` (a DeviceConfig::Tx) through a
 * register accessor with RtlAdapter's rtw_read<T>/rtw_write<T> members
 * (spelled out with `template`: the rtw_read8/... macros cannot be used on a
 * dependent type). Called once per TX bring-up, after the backend's EDCA
 * init and before a configured SetAmpduMode is re-applied, so the A-MPDU
 * mode saves and restores the value set here. Neither option set: no
 * register access. */
template <class Dev, class TxCfg>
void apply_ac_tx_queue(Dev &dev, const TxCfg &tx) {
  if (tx.be_fast_retry)
    dev.template rtw_write<uint32_t>(
        kRegEdcaBeParam,
        edca_fast_retry(dev.template rtw_read<uint32_t>(kRegEdcaBeParam)));
  if (tx.ac_lifetime_us > 0) {
    dev.template rtw_write<uint32_t>(kRegPktLifeTime,
                                     pkt_life_time_reg(tx.ac_lifetime_us));
    dev.template rtw_write<uint8_t>(
        kRegLifetimeEn,
        static_cast<uint8_t>(dev.template rtw_read<uint8_t>(kRegLifetimeEn) |
                             kLifetimeEnAc));
  }
}

} // namespace devourer

#endif /* DEVOURER_AC_TX_QUEUE_H */
