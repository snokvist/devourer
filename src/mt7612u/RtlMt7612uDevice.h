#ifndef RTL_MT7612U_DEVICE_H
#define RTL_MT7612U_DEVICE_H

#include <libusb.h>

#include <atomic>
#include <memory>
#include <mutex>

#include "DeviceConfig.h"
#include "IRtlDevice.h"
#include "SelectedChannel.h"
#include "UsbDeviceLock.h"
#include "logger.h"

#include "mt7612u/mt7612u.h"

/*
 * MediaTek MT7612U behind IRtlDevice.
 *
 * The interface is vendor-neutral in substance — Init / InitWrite /
 * StartRxLoop / send_packet / SetMonitorChannel / GetAdapterCaps say nothing
 * about Realtek — so this backend sits behind it as it stands rather than
 * behind a renamed copy.
 *
 * What it does NOT do is reuse RtlAdapter. That transport is shaped around
 * 16-bit Realtek registers and Realtek bulk endpoints; MT7612U is 32-bit
 * registers over EP0 vendor requests plus an in-band MCU on EP8/EP5, with
 * firmware to upload before any of it answers. The C library under this
 * directory is that transport, and the class below owns one `mt7612u_dev`.
 *
 * Divergences a caller must know about, all measured (docs/mt7612u.md):
 *
 *  - `Packet::Data` carries NO trailing FCS. Every Realtek parser here
 *    includes it; this MAC strips it and hands up only the MPDU. A consumer
 *    that trims four bytes at its protocol boundary would eat payload.
 *  - There is no TXAGC index. TX power is an absolute dBm limit plus a 4-bit
 *    per-frame trim, so GetTxPowerCaps reports the dBm model (index_max = 0)
 *    and SetTxPowerIndexOverride refuses rather than pretending.
 *  - FastRetune is not fast: 48 ms with calibration skipped, against
 *    0.5–2.5 ms on the Realtek parts, because the RF plane lives behind the
 *    MCU. fastretune_ok stays false and the base-class full path is used.
 *  - The receiver must never run with nothing draining the bulk-IN endpoint;
 *    the chip wedges below the USB level and only a replug recovers it. The C
 *    layer enforces the ordering and this class does not reach around it.
 */
class RtlMt7612uDevice : public IRtlDevice {
public:
  RtlMt7612uDevice(libusb_device_handle *handle, libusb_context *ctx,
                   std::shared_ptr<devourer::UsbDeviceLock> usb_lock,
                   Logger_t logger, devourer::DeviceConfig cfg = {});
  ~RtlMt7612uDevice() override;

  RtlMt7612uDevice(const RtlMt7612uDevice &) = delete;
  RtlMt7612uDevice &operator=(const RtlMt7612uDevice &) = delete;

  /* Brings the chip up, tunes, then runs the RX loop until Stop(). */
  void Init(Action_ParsedRadioPacket packetProcessor,
            SelectedChannel channel) override;
  /* Brings the chip up and tunes, TX only — the receiver stays off. */
  void InitWrite(SelectedChannel channel) override;
  void StartRxLoop(Action_ParsedRadioPacket packetProcessor) override;
  void StopRxLoop() override;
  void SetMonitorChannel(SelectedChannel channel) override;
  SelectedChannel GetSelectedChannel() override;

  bool send_packet(const uint8_t *packet, size_t length) override;
  size_t send_packets(const TxPacketView *pkts, size_t count) override;

  devourer::AdapterCaps GetAdapterCaps() override;
  devourer::TxCaps GetTxCaps() override;
  devourer::TxPowerCaps GetTxPowerCaps() override;

  void SetTxPower(uint8_t power) override;
  void SetTxPowerIndexOverride(int idx) override;
  int SetTxPowerOffsetQdb(int qdb) override;
  devourer::TxPowerState GetTxPowerState() override;

  void SetTxMode(const devourer::TxMode &mode) override;
  void ClearTxMode() override;

  bool SetAmpduMode(const devourer::AmpduMode &mode) override;
  void ClearAmpduMode() override;
  devourer::AmpduMode GetAmpduMode() override;

  bool SetAckResponder(const devourer::MacAddr &mac) override;
  void ClearAckResponder() override;

  uint64_t ReadTsf() override;
  void WriteTsf(uint64_t tsf) override;

  bool GetPermanentMacAddress(uint8_t out[6]) override;
  devourer::TxStats GetTxStats() override;
  devourer::RxQuality GetRxQuality() override;

  void SetCcaMode(bool disabled) override;
  void Stop() override;

private:
  void bring_up(SelectedChannel channel);
  static void rx_trampoline(void *user, const void *frame, size_t len,
                            const struct mt7612u_rx_info *info);
  void on_rx(const void *frame, size_t len, const struct mt7612u_rx_info *info);

  libusb_device_handle *_handle;
  libusb_context *_ctx;
  std::shared_ptr<devourer::UsbDeviceLock> _usb_lock;
  Logger_t _logger;
  devourer::DeviceConfig _cfg;

  /* Guards the control plane: bring-up, channel sets and the power knobs.
   * The RX callback runs on the C layer's libusb event thread and does not
   * take it — it only reads _rx_processor, which is written before the ring
   * starts and not touched again until after it stops. */
  std::recursive_mutex _mu;
  struct mt7612u_dev *_dev = nullptr;
  SelectedChannel _channel{};
  Action_ParsedRadioPacket _rx_processor;
  std::atomic<bool> _rx_stop{false};
  std::atomic<bool> _rx_active{false};
  std::atomic<uint64_t> _rx_frames{0};
  /* RSSI window for GetRxQuality, drained on read like every other backend's.
   * Written only by the libusb event thread, read only under _mu. */
  std::atomic<uint64_t> _rssi_sum{0};
  std::atomic<uint64_t> _rssi_n{0};
  std::atomic<int> _rssi_max{-127};

  devourer::AmpduMode _ampdu{};
  struct mt7612u_tx_rate _tx_default {};
  bool _tx_mode_set = false;
  int _txpwr_dbm = 20;    /* the absolute base mt7612u_set_txpower takes */
  int _txpwr_offset_qdb = 0;
};

#endif /* RTL_MT7612U_DEVICE_H */
