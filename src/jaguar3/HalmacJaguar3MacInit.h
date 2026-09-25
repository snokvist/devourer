#ifndef HALMAC_8822C_MAC_INIT_H
#define HALMAC_8822C_MAC_INIT_H

#include "logger.h"
#include "RtlAdapter.h"
#include "SelectedChannel.h"

namespace jaguar3 {

/* HalmacJaguar3MacInit — faithful userspace port of HalMAC's post-DLFW MAC
 * configuration for the RTL8822C (init_mac_cfg_88xx -> init_trx/protocol/edca/
 * wmac, from the OpenHD/rtl88x2cu vendor tree, hal/halmac/halmac_88xx/
 * halmac_8822c/halmac_init_8822c.c).
 *
 * Scope of this port: USB interface, 3 bulk-OUT endpoints (the RTL8812CU /
 * WDN1300H topology: EP 0x05/0x06/0x08), HALMAC_TRX_MODE_NORMAL. The EDCA /
 * WMAC / SIFS register sets branch on bandwidth so 5/10 MHz narrowband gets the
 * correct MAC timing (not just the baseband divider). */
class HalmacJaguar3MacInit {
public:
  HalmacJaguar3MacInit(RtlAdapter device, Logger_t logger);

  /* pre_init_system_cfg_8822c: pinmux / LED / GPIO + BB-RF disabled. Runs
   * BEFORE the mac power switch (card_en_flow). */
  void pre_init_system_cfg();

  /* init_system_cfg_8822c: DDMA enable, SYS_FUNC_EN, PHY_REQ_DELAY (bw), disable
   * boot-from-flash. Runs AFTER the power switch, BEFORE DLFW. `cut` selects the
   * B-cut ANAPAR workaround. */
  void init_system_cfg(ChannelWidth_t bw, uint8_t cut);

  /* enable_bb_rf_88xx: toggle SYS_FUNC_EN BB + RF_CTRL + WLRF1 RF-on bits. */
  void enable_bb_rf(bool enable);

  /* init_usb_cfg_88xx: USB RX-DMA mode + burst (REG_RXDMA_MODE) + TX drop-data.
   * Required for the chip to push RX frames to the bulk-IN endpoint. */
  void init_usb_cfg();

  /* init_mac_cfg_88xx: trx -> protocol -> edca -> wmac. Returns false on the
   * LLT auto-init timeout (the only failure path in the ported flow). */
  bool init_mac_cfg(ChannelWidth_t bw);

  /* Reserved-page boundary computed by priority_queue_cfg (txff_pg_num -
   * rsvd_pg_num) — the head page of the beacon/rsvd region, where the beacon is
   * downloaded and BCN_HEAD points. Valid after init_mac_cfg. */
  uint16_t rsvd_boundary() const { return _rsvd_boundary; }
  /* The reserved region's layout, from the same allocation (valid after
   * init_mac_cfg): the firmware's TX buffer page, the H2C-packet queue's head
   * page and its size in pages. GENERAL_INFO carries the first as an offset
   * from rsvd_boundary; the H2C-packet path polls the second. */
  uint16_t rsvd_fw_txbuf_addr() const { return _rsvd_fw_txbuf_addr; }
  uint16_t rsvd_h2cq_addr() const { return _rsvd_h2cq_addr; }
  uint16_t rsvd_h2cq_pages() const;

  /* One dword of on-chip packet memory through the debug window (halmac
   * read_buf_88xx addressing). window_base 0x780 = TX FIFO, 0x650 = LLT;
   * byte_off is 4-aligned. Saves and restores REG_PKTBUF_DBG_CTRL. */
  uint32_t read_pktbuf32(uint16_t window_base, uint32_t byte_off);
  /* LLT entry `page` - the page that follows it in its chain. */
  uint32_t llt_entry(uint16_t page) { return read_pktbuf32(0x650, page * 4u); }

private:
  bool init_trx_cfg();
  bool priority_queue_cfg();
  /* LLT[rsvd_boundary - 1] := 0 - see the definition. */
  bool terminate_acq_ring(uint16_t rsvd_boundary);
  void init_h2c();
  void init_protocol_cfg();
  void init_edca_cfg(ChannelWidth_t bw);
  void init_wmac_cfg(ChannelWidth_t bw);
  void init_txq_ctrl();
  void init_sifs_ctrl(ChannelWidth_t bw);
  void init_rate_fallback_ctrl();
  void cfg_mac_clk();

  uint16_t _rsvd_boundary = 0;
  uint16_t _rsvd_fw_txbuf_addr = 0;
  uint16_t _rsvd_h2cq_addr = 0;
  RtlAdapter _device;
  Logger_t _logger;
};

} /* namespace jaguar3 */

#endif /* HALMAC_8822C_MAC_INIT_H */
