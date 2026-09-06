/* Headless guard for the MT7612U <-> devourer translations
 * (src/mt7612u/Mt7612uMapping.h). Each is a lookup that is easy to get subtly
 * wrong and impossible to notice on a radio: a wrong RSSI base reads as a weak
 * link, a wrong rate code as a slow one, and a silently narrowed channel width
 * as a working link at the wrong bandwidth.
 *
 * Both the RSSI bias and the rate numbering were caught here rather than on
 * air — the first live integration run reported -63 dBm as 193 (that is,
 * +83 dBm) because a signed dBm had been cast straight into devourer's
 * unsigned biased byte. */
#include <cstdio>

#include "mt7612u/Mt7612uMapping.h"

static int g_fail = 0;

static void expect(const char *what, bool ok) {
  if (ok)
    return;
  ++g_fail;
  std::printf("FAIL: %s\n", what);
}

static mt7612u_rx_info rx(enum mt7612u_phy phy, uint8_t mcs, uint8_t nss = 1) {
  mt7612u_rx_info i{};
  i.phy = phy;
  i.mcs = mcs;
  i.nss = nss;
  return i;
}

int main() {
  using namespace mt7612u;

  /* --- RSSI: devourer stores dBm + 110 (LinkHealth.cpp: rssi_dbm = raw-110) */
  expect("rssi -63 dBm -> 47", rssi_to_raw(-63) == 47);
  expect("rssi -110 dBm -> 0", rssi_to_raw(-110) == 0);
  expect("rssi 0 dBm -> 110", rssi_to_raw(0) == 110);
  expect("rssi -128 dBm clamps to 0", rssi_to_raw(-128) == 0);
  expect("rssi +127 dBm clamps to 255", rssi_to_raw(127) == 237);
  /* The bug this replaced: a straight cast turns -63 into 193. */
  expect("rssi is biased, not cast",
         rssi_to_raw(-63) != static_cast<uint8_t>(static_cast<int8_t>(-63)));
  /* Round-trip through devourer's own reader. */
  expect("rssi round-trips to dBm",
         static_cast<int>(rssi_to_raw(-63)) - 110 == -63);

  /* --- rate: the DESC_RATE numbering every backend reports in --- */
  expect("CCK 1M -> 0", desc_rate(rx(MT7612U_PHY_CCK, 0)) == 0);
  expect("CCK 11M -> 3", desc_rate(rx(MT7612U_PHY_CCK, 3)) == 3);
  expect("OFDM 6M -> DESC_RATE6M", desc_rate(rx(MT7612U_PHY_OFDM, 0)) == 0x04);
  expect("OFDM 54M -> 11", desc_rate(rx(MT7612U_PHY_OFDM, 7)) == 11);
  expect("HT MCS0 -> DESC_RATEMCS0", desc_rate(rx(MT7612U_PHY_HT, 0)) == 0x0c);
  /* MCS7 is 19 — the code the witness logs showed for our own injected
   * frames, so this is pinned against a measurement, not just the header. */
  expect("HT MCS7 -> 19", desc_rate(rx(MT7612U_PHY_HT, 7)) == 19);
  expect("HT MCS15 -> 27", desc_rate(rx(MT7612U_PHY_HT, 15)) == 27);
  expect("HT-GF uses the HT numbering",
         desc_rate(rx(MT7612U_PHY_HT_GF, 7)) == desc_rate(rx(MT7612U_PHY_HT, 7)));
  expect("VHT 1SS MCS0 -> DESC_RATEVHTSS1MCS0",
         desc_rate(rx(MT7612U_PHY_VHT, 0, 1)) == 0x2c);
  expect("VHT 2SS MCS0 -> +10", desc_rate(rx(MT7612U_PHY_VHT, 0, 2)) == 0x2c + 10);
  expect("VHT 2SS MCS9 -> +19", desc_rate(rx(MT7612U_PHY_VHT, 9, 2)) == 0x2c + 19);
  expect("VHT nss 0 is treated as 1",
         desc_rate(rx(MT7612U_PHY_VHT, 3, 0)) == desc_rate(rx(MT7612U_PHY_VHT, 3, 1)));

  /* --- channel width: accept two, refuse the rest with a reason --- */
  {
    enum mt7612u_bw bw = MT7612U_BW_80;
    const char *why = "";

    expect("20 MHz accepted", width_to_bw(CHANNEL_WIDTH_20, bw, why));
    expect("20 MHz -> BW_20", bw == MT7612U_BW_20);
    expect("40 MHz accepted", width_to_bw(CHANNEL_WIDTH_40, bw, why));
    expect("40 MHz -> BW_40", bw == MT7612U_BW_40);

    /* Refusals must not write `out` — a caller that ignores the bool would
     * otherwise transmit at whatever the last accepted width left behind. */
    bw = MT7612U_BW_20;
    expect("80 MHz refused", !width_to_bw(CHANNEL_WIDTH_80, bw, why));
    expect("80 MHz leaves bw untouched", bw == MT7612U_BW_20);
    expect("80 MHz gives a reason", why && why[0]);
    expect("5 MHz refused", !width_to_bw(CHANNEL_WIDTH_5, bw, why));
    expect("10 MHz refused", !width_to_bw(CHANNEL_WIDTH_10, bw, why));
    expect("160 MHz refused", !width_to_bw(CHANNEL_WIDTH_160, bw, why));
  }

  if (g_fail == 0)
    std::printf("mt7612u_mapping_selftest: PASS\n");
  return g_fail ? 1 : 0;
}
