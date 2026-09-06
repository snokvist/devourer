#ifndef MT7612U_MAPPING_H
#define MT7612U_MAPPING_H

#include <cstdint>

#include "SelectedChannel.h"
#include "ieee80211_radiotap.h" /* DESC_RATE* */
#include "mt7612u/mt7612u.h"

/*
 * The pure translations between MT7612U's own descriptor vocabulary and the
 * one every other backend here reports in. They live in a header, apart from
 * the device class, because each is a lookup that is easy to get subtly wrong
 * and impossible to notice on a radio: a wrong RSSI base reads as a weak
 * link, a wrong rate code reads as a slow one. tests/mt7612u_mapping_selftest
 * pins all three.
 */
namespace mt7612u {

/* devourer carries RSSI as an unsigned byte biased by 110 —
 * LinkHealth.cpp:8 is the authority: `rssi_dbm = rssi_raw - 110`. The HAL
 * reports true dBm, so the bias has to be added, not cast around. Casting a
 * signed -63 dBm straight into the byte yields 193, i.e. +83 dBm. */
inline constexpr int kRssiBiasDb = 110;

inline uint8_t rssi_to_raw(int8_t dbm) {
  const int raw = static_cast<int>(dbm) + kRssiBiasDb;
  if (raw < 0)
    return 0;
  if (raw > 255)
    return 255;
  return static_cast<uint8_t>(raw);
}

/* mt7612u_rx_info -> the DESC_RATE numbering consumers read, so a caller does
 * not need to know which chip a frame came from. */
inline uint16_t desc_rate(const struct mt7612u_rx_info &info) {
  switch (info.phy) {
  case MT7612U_PHY_CCK:
    /* DESC_RATE1M..11M are 0..3, in the same order as the CCK indices. */
    return static_cast<uint16_t>(info.mcs & 0x3);
  case MT7612U_PHY_OFDM:
    return static_cast<uint16_t>(DESC_RATE6M + (info.mcs & 0x7));
  case MT7612U_PHY_HT:
  case MT7612U_PHY_HT_GF:
    /* HT folds NSS into the MCS number on both sides, so this is a straight
     * offset for MCS 0-31. */
    return static_cast<uint16_t>(DESC_RATEMCS0 + info.mcs);
  case MT7612U_PHY_VHT: {
    const uint8_t nss = info.nss ? info.nss : 1;
    return static_cast<uint16_t>(DESC_RATEVHTSS1MCS0 + (nss - 1) * 10 +
                                 info.mcs);
  }
  }
  return 0;
}

/* SelectedChannel width -> the two widths this port implements. Anything
 * wider or narrower is refused rather than silently narrowed: 80 MHz is
 * silicon-capable but the width maths is not ported, and 5/10 MHz has no
 * encoding in the rate word at all. */
inline bool width_to_bw(ChannelWidth_t w, enum mt7612u_bw &out,
                        const char *&why) {
  switch (w) {
  case CHANNEL_WIDTH_20:
    out = MT7612U_BW_20;
    return true;
  case CHANNEL_WIDTH_40:
    out = MT7612U_BW_40;
    return true;
  case CHANNEL_WIDTH_80:
    why = "80 MHz: silicon-capable, width maths not ported";
    return false;
  case CHANNEL_WIDTH_5:
  case CHANNEL_WIDTH_10:
    why = "5/10 MHz narrowband: MT_RATE_BW has no encoding for it";
    return false;
  default:
    why = "unsupported channel width";
    return false;
  }
}

} // namespace mt7612u

#endif /* MT7612U_MAPPING_H */
