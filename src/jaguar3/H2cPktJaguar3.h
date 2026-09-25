#ifndef JAGUAR3_H2C_PKT_H
#define JAGUAR3_H2C_PKT_H

/* H2C PACKETS - the firmware command path that is not the HMEBOX mailbox.
 *
 * halmac has two host-to-firmware channels. The HMEBOX registers carry the
 * short 8-byte commands this backend has always used. H2C *packets* are
 * 32-byte frames the host sends through the TX path like any other frame -
 * a 48-byte TX descriptor with QSEL 0x13 (HALMAC_TXDESC_QSEL_H2C_CMD), on
 * the HIGH bulk-OUT endpoint - which the MAC files into the H2C queue in the
 * reserved region (rsvd_h2cq_addr) for the firmware to pick up.
 *
 * Header (halmac_fw_offload_h2c_nic.h, set_h2c_pkt_hdr_88xx):
 *   byte 0  category 0x01 (bits 0..6), ACK request (bit 7)
 *   byte 1  cmd id 0xFF
 *   2..3    sub-command id, LE
 *   4..5    total length = 8 + content size, LE
 *   6..7    sequence number, LE - halmac's h2c_info.seq_num, ONE counter for
 *           every H2C packet, independent of the HMEBOX box index.
 * Content starts at byte 8.
 *
 * The builders are pure so a headless test can hold them to the vendor
 * driver's bytes, captured by usbmon on an RTL8812CU. */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "FrameParserJaguar3.h"

namespace jaguar3 {

constexpr size_t H2C_PKT_SIZE = 32;     /* H2C_PKT_SIZE_88XX */
constexpr size_t H2C_PKT_HDR_SIZE = 8;  /* H2C_PKT_HDR_SIZE_88XX */
constexpr uint8_t QSEL_H2C_CMD = 0x13;  /* HALMAC_TXDESC_QSEL_H2C_CMD */

constexpr uint16_t H2C_SUB_GENERAL_INFO = 0x0D; /* SUB_CMD_ID_GENERAL_INFO */
constexpr uint16_t H2C_SUB_PHYDM_INFO = 0x11;   /* SUB_CMD_ID_PHYDM_INFO */

inline void h2c_pkt_set_hdr(uint8_t *pkt, uint16_t sub_cmd,
                            uint16_t content_size, uint16_t seq, bool ack) {
  const uint16_t total = static_cast<uint16_t>(H2C_PKT_HDR_SIZE + content_size);
  pkt[0] = static_cast<uint8_t>(0x01 | (ack ? 0x80 : 0x00));
  pkt[1] = 0xFF;
  pkt[2] = static_cast<uint8_t>(sub_cmd);
  pkt[3] = static_cast<uint8_t>(sub_cmd >> 8);
  pkt[4] = static_cast<uint8_t>(total);
  pkt[5] = static_cast<uint8_t>(total >> 8);
  pkt[6] = static_cast<uint8_t>(seq);
  pkt[7] = static_cast<uint8_t>(seq >> 8);
}

/* proc_send_general_info_88xx. The one field is FW_TX_BOUNDARY =
 * rsvd_fw_txbuf_addr - rsvd_boundary: where the firmware's own TX buffer sits,
 * counted in pages from the start of the reserved region. */
inline void build_general_info(uint8_t *pkt, uint8_t fw_tx_boundary,
                               uint16_t seq) {
  std::memset(pkt, 0, H2C_PKT_SIZE);
  pkt[H2C_PKT_HDR_SIZE + 2] = fw_tx_boundary; /* dword 0x08, bits 16..23 */
  h2c_pkt_set_hdr(pkt, H2C_SUB_GENERAL_INFO, 4, seq, false);
}

/* halmac_general_info as proc_send_phydm_info_88xx consumes it. rf_type is
 * halmac's enum (HALMAC_RF_2T2R = 2); the antenna fields are bb_path bitmaps
 * (A = 1, B = 2). */
struct PhydmInfo {
  uint8_t rfe_type = 0;
  uint8_t rf_type = 0;
  uint8_t cut = 0;
  uint8_t rx_ant = 0;
  uint8_t tx_ant = 0;
  uint8_t ext_pa = 0;
  uint8_t package_type = 0;
  bool mp_mode = false;
};

inline void build_phydm_info(uint8_t *pkt, const PhydmInfo &i, uint16_t seq) {
  std::memset(pkt, 0, H2C_PKT_SIZE);
  uint8_t *c = pkt + H2C_PKT_HDR_SIZE;
  c[0] = i.rfe_type;
  c[1] = i.rf_type;
  c[2] = i.cut;
  c[3] = static_cast<uint8_t>((i.rx_ant & 0x0F) | ((i.tx_ant & 0x0F) << 4));
  c[4] = i.ext_pa;
  c[5] = i.package_type;
  c[6] = i.mp_mode ? 1 : 0;
  h2c_pkt_set_hdr(pkt, H2C_SUB_PHYDM_INFO, 8, seq, false);
}

/* The USB bulk-OUT buffer for one H2C packet (usb_write_data_not_xmitframe):
 * a zeroed descriptor with only TXPKTSIZE and QSEL set, then the checksum.
 * No OFFSET - the vendor leaves it 0 on this path, and the capture agrees. */
constexpr size_t H2C_USB_FRAME_SIZE = TXDESC_SIZE_8822C + H2C_PKT_SIZE;

inline void build_h2c_usb_frame(uint8_t *out, const uint8_t *pkt) {
  std::memset(out, 0, TXDESC_SIZE_8822C);
  SET_TX_DESC_TXPKTSIZE_8822C(out, H2C_PKT_SIZE);
  SET_TX_DESC_QSEL_8822C(out, QSEL_H2C_CMD);
  cal_txdesc_chksum_8822c(out);
  std::memcpy(out + TXDESC_SIZE_8822C, pkt, H2C_PKT_SIZE);
}

} /* namespace jaguar3 */

#endif /* JAGUAR3_H2C_PKT_H */
