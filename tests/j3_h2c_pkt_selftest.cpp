/* Headless guard for the Jaguar3 H2C-packet builders (src/jaguar3/H2cPktJaguar3.h).
 *
 * The reference is not a spec reading: it is the vendor rtl88x2cu driver's own
 * bulk-OUT transfers, captured by usbmon on an RTL8812CU (frames 2331/2332,
 * endpoint 0x05) - the GENERAL_INFO and PHYDM_INFO packets it sends right after
 * the MAC init. A builder that drifts from those bytes by one bit fails here,
 * before it reaches a firmware that would silently ignore it.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "jaguar3/H2cPktJaguar3.h"

namespace {

int g_fail = 0;

void unhex(const char *s, uint8_t *out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    unsigned v = 0;
    std::sscanf(s + 2 * i, "%2x", &v);
    out[i] = static_cast<uint8_t>(v);
  }
}

void expect_bytes(const char *name, const uint8_t *got, const char *want_hex,
                  size_t n) {
  uint8_t want[128];
  unhex(want_hex, want, n);
  for (size_t i = 0; i < n; i++) {
    if (got[i] != want[i]) {
      std::printf("FAIL %s: byte %zu is 0x%02x, the vendor sent 0x%02x\n",
                  name, i, got[i], want[i]);
      g_fail++;
      return;
    }
  }
  std::printf("ok   %s (%zu bytes identical to the vendor capture)\n", name, n);
}

void expect(bool c, const char *what) {
  std::printf("%s %s\n", c ? "ok  " : "FAIL", what);
  if (!c)
    g_fail++;
}

/* usbmon, vendor rtl88x2cu on an RTL8812CU, bulk-OUT endpoint 0x05. */
const char *kVendorGeneralInfo =
    "200000000013000000000000000000000000000000000000"
    "000000002013000000000000000000000000000000000000"
    "01ff0d000c000000000038000000000000000000000000000000000000000000";
const char *kVendorPhydmInfo =
    "200000000013000000000000000000000000000000000000"
    "000000002013000000000000000000000000000000000000"
    "01ff110010000100030205330007000000000000000000000000000000000000";

} /* namespace */

int main() {
  using namespace jaguar3;
  uint8_t pkt[H2C_PKT_SIZE];
  uint8_t frame[H2C_USB_FRAME_SIZE];

  /* FW_TX_BOUNDARY 56 = rsvd_fw_txbuf_addr 1994 - rsvd_boundary 1938, the
   * value both the vendor and this backend's page layout produce. Sequence 0:
   * it is the first H2C packet of the session. */
  build_general_info(pkt, 56, 0);
  build_h2c_usb_frame(frame, pkt);
  expect_bytes("GENERAL_INFO", frame, kVendorGeneralInfo, H2C_USB_FRAME_SIZE);

  /* The vendor's values on this adapter: rfe 3, HALMAC_RF_2T2R, cut 5, both
   * paths (A|B = 3) for RX and TX, no external PA, package type 7 (from its
   * MAC-hidden report). Sequence 1 - one counter across both packets. */
  PhydmInfo pi;
  pi.rfe_type = 3;
  pi.rf_type = 2;
  pi.cut = 5;
  pi.rx_ant = 3;
  pi.tx_ant = 3;
  pi.package_type = 7;
  build_phydm_info(pkt, pi, 1);
  build_h2c_usb_frame(frame, pkt);
  expect_bytes("PHYDM_INFO", frame, kVendorPhydmInfo, H2C_USB_FRAME_SIZE);

  /* The fields the capture cannot exercise, because the vendor sent them as
   * zero: the ACK bit, a sequence number past one byte, and the nibble order
   * of the antenna byte when RX and TX differ. */
  build_general_info(pkt, 56, 0x1234);
  expect(pkt[6] == 0x34 && pkt[7] == 0x12, "sequence number is 16-bit LE");
  h2c_pkt_set_hdr(pkt, H2C_SUB_GENERAL_INFO, 4, 0, true);
  expect(pkt[0] == 0x81, "ACK request is bit 7 of byte 0, category kept");
  pi.rx_ant = 1;
  pi.tx_ant = 2;
  pi.mp_mode = true;
  build_phydm_info(pkt, pi, 0);
  expect(pkt[11] == 0x21, "RX antenna in the low nibble, TX in the high");
  expect(pkt[14] == 0x01, "mp_mode is bit 0 of content byte 6");

  if (g_fail) {
    std::printf("%d check(s) failed\n", g_fail);
    return 1;
  }
  std::printf("all H2C packet checks passed\n");
  return 0;
}
