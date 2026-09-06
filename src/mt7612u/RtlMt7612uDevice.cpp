#include "RtlMt7612uDevice.h"

#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#include <cstdlib>
#include <filesystem>

#include "RxPacket.h"
#include "RxQuality.h"
#include "SignalStop.h"
#include "Mt7612uMapping.h"

namespace {

/*
 * Where mt7662_rom_patch.bin and mt7662.bin live.
 *
 * Unlike the Realtek backends, whose firmware is generated into hal/ and
 * compiled in, MediaTek's ships in linux-firmware and is zstd-compressed on
 * most distributions - so it cannot be vendored here and cannot be assumed
 * ready at a fixed path. Resolved in the order a caller would expect, and the
 * failure message names every place that was tried rather than just saying no.
 */
std::string resolve_fw_dir(const Logger_t &logger) {
  std::vector<std::string> tried;

  if (const char *env = std::getenv("DEVOURER_MT7612U_FW_DIR"))
    tried.emplace_back(env);
  tried.emplace_back("/lib/firmware/mediatek");
  tried.emplace_back("firmware"); /* the bring-up harness's own directory */

  std::error_code ec;
  for (const std::string &dir : tried) {
    const std::filesystem::path patch =
        std::filesystem::path(dir) / "mt7662_rom_patch.bin";
    const std::filesystem::path fw = std::filesystem::path(dir) / "mt7662.bin";
    if (std::filesystem::exists(patch, ec) && std::filesystem::exists(fw, ec)) {
      logger->info("MT7612U firmware from {}", dir);
      return dir;
    }
  }

  std::string all;
  for (const std::string &dir : tried)
    all += (all.empty() ? "" : ", ") + dir;
  logger->error("MT7612U firmware (mt7662_rom_patch.bin + mt7662.bin) not "
                "found in: {}. They ship zstd-compressed in linux-firmware; "
                "decompress them and point DEVOURER_MT7612U_FW_DIR at the "
                "directory.",
                all);
  return tried.back();
}

} // namespace

RtlMt7612uDevice::RtlMt7612uDevice(
    libusb_device_handle *handle, libusb_context *ctx,
    std::shared_ptr<devourer::UsbDeviceLock> usb_lock, Logger_t logger,
    devourer::DeviceConfig cfg)
    : _handle(handle), _ctx(ctx), _usb_lock(std::move(usb_lock)),
      _logger(std::move(logger)), _cfg(std::move(cfg)) {}

RtlMt7612uDevice::~RtlMt7612uDevice() {
  if (_dev) {
    mt7612u_close(_dev);
    _dev = nullptr;
  }
}

/* --- bring-up ------------------------------------------------------------ */

void RtlMt7612uDevice::bring_up(SelectedChannel channel) {
  enum mt7612u_bw bw = MT7612U_BW_20;
  const char *why = "";

  if (!mt7612u::width_to_bw(channel.ChannelWidth, bw, why))
    throw std::runtime_error(std::string("MT7612U channel width refused: ") +
                             why);

  if (!_dev) {
    const char *err = nullptr;
    /* Adopts the caller's handle: WiFiDriver already opened, reset and
     * claimed it, and holds the exclusive lock this class carries. */
    const std::string fw_dir = resolve_fw_dir(_logger);
    _dev = mt7612u_open_handle(_handle, _ctx, fw_dir.c_str(), &err);
    if (!_dev)
      throw std::runtime_error(std::string("MT7612U bring-up failed: ") +
                               (err ? err : "unknown"));
    _logger->info("MT7612U up: ASIC 0x{:08x}", mt7612u_asic_version(_dev));
    if (_txpwr_dbm != 20)
      mt7612u_set_txpower(_dev, _txpwr_dbm);
  }

  if (mt7612u_set_channel(_dev, channel.Channel, bw) != 0)
    throw std::runtime_error("MT7612U channel set failed");
  _channel = channel;
}

void RtlMt7612uDevice::Init(Action_ParsedRadioPacket packetProcessor,
                            SelectedChannel channel) {
  {
    std::lock_guard<std::recursive_mutex> lock(_mu);
    bring_up(channel);
  }
  StartRxLoop(std::move(packetProcessor));
}

void RtlMt7612uDevice::InitWrite(SelectedChannel channel) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  bring_up(channel);
  /* No RX ring, so mt7612u_start() enables TX only — which is the whole
   * point of this entry point and also what keeps the chip out of the
   * undrained-receiver wedge. */
  if (mt7612u_start(_dev) != 0)
    throw std::runtime_error("MT7612U MAC start failed");
}

/* --- RX ------------------------------------------------------------------ */

void RtlMt7612uDevice::rx_trampoline(void *user, const void *frame, size_t len,
                                     const struct mt7612u_rx_info *info) {
  static_cast<RtlMt7612uDevice *>(user)->on_rx(frame, len, info);
}

void RtlMt7612uDevice::on_rx(const void *frame, size_t len,
                             const struct mt7612u_rx_info *info) {
  if (!_rx_processor)
    return;

  Packet packet{};
  packet.RxAtrib.pkt_len = static_cast<uint16_t>(len);
  packet.RxAtrib.crc_err = info->crc_err != 0;
  packet.RxAtrib.seq_num = info->seq;
  packet.RxAtrib.data_rate = mt7612u::desc_rate(*info);
  packet.RxAtrib.bw = static_cast<uint8_t>(info->bw);
  packet.RxAtrib.stbc = info->stbc;
  packet.RxAtrib.ldpc = info->ldpc;
  packet.RxAtrib.sgi = info->sgi;
  packet.RxAtrib.paggr = info->ampdu != 0;
  packet.RxAtrib.pkt_rpt_type = RX_PACKET_TYPE::NORMAL_RX;
  /* dBm + 110, the unsigned byte LinkHealth.cpp:8 reads back. A straight
   * cast of a signed -63 dBm gives 193, i.e. a reported +83 dBm. */
  for (int i = 0; i < 4; ++i)
    packet.RxAtrib.rssi[i] = mt7612u::rssi_to_raw(info->rssi[i]);
  if (len >= 2) {
    const uint8_t *f = static_cast<const uint8_t *>(frame);
    packet.RxAtrib.qos = (f[0] & 0x0c) == 0x08 && (f[0] & 0x80) != 0;
  }

  /* The span points into the ring buffer the libusb event thread owns and
   * reuses the moment this returns, so the processor must not retain it —
   * the same contract every other backend's parser has. const_cast because
   * Packet::Data is a mutable span and the buffer genuinely is ours.
   *
   * NOTE: unlike every Realtek backend, this span carries NO trailing FCS.
   * The MAC strips it; the four bytes past MPDU_LEN in the DMA buffer are
   * the FCE info trailer, not a checksum (CRC-32 matched them on 0 of 4263
   * frames — see docs/mt7612u.md). */
  packet.Data = std::span<uint8_t>(
      const_cast<uint8_t *>(static_cast<const uint8_t *>(frame)), len);

  _rx_frames.fetch_add(1, std::memory_order_relaxed);
  if (info->rssi[0]) {
    _rssi_sum.fetch_add(static_cast<uint64_t>(info->rssi[0] + 128),
                        std::memory_order_relaxed);
    _rssi_n.fetch_add(1, std::memory_order_relaxed);
    int prev = _rssi_max.load(std::memory_order_relaxed);
    while (info->rssi[0] > prev &&
           !_rssi_max.compare_exchange_weak(prev, info->rssi[0],
                                            std::memory_order_relaxed)) {
    }
  }
  if (info->noise_valid) {
    _snr_sum.fetch_add(info->snr_db, std::memory_order_relaxed);
    _noise_sum.fetch_add(info->noise, std::memory_order_relaxed);
    _snr_n.fetch_add(1, std::memory_order_relaxed);
    int lo = _snr_min.load(std::memory_order_relaxed);
    while (info->snr_db < lo &&
           !_snr_min.compare_exchange_weak(lo, info->snr_db,
                                           std::memory_order_relaxed)) {
    }
  }
  _rx_processor(packet);
}

void RtlMt7612uDevice::StartRxLoop(Action_ParsedRadioPacket packetProcessor) {
  {
    std::lock_guard<std::recursive_mutex> lock(_mu);
    if (!_dev)
      throw std::runtime_error("MT7612U RX loop requires initialized hardware");
    if (_rx_active.load())
      throw std::runtime_error("MT7612U RX loop is already active");
    _rx_processor = std::move(packetProcessor);
    _rx_stop = false;

    /* Ring first, receiver second. With MAC RX on and nothing draining the
     * bulk-IN endpoint this part wedges below the USB level and only a
     * physical replug recovers it. mt7612u_start() enforces the same
     * ordering; this is not belt-and-braces so much as not fighting it. */
    if (mt7612u_rx_start(_dev, &RtlMt7612uDevice::rx_trampoline, this) != 0)
      throw std::runtime_error("MT7612U RX ring failed to start");
    if (mt7612u_start(_dev) != 0) {
      mt7612u_rx_stop(_dev);
      throw std::runtime_error("MT7612U MAC start failed");
    }
    /* After mt7612u_start(), which rewrites the filter to mt76's managed-mode
     * value. Without this the receiver drops control frames and anything not
     * addressed here: measured on ambient 2.4 GHz traffic, 0 OFDM frames of
     * 244 against 103 of 402 through the same silicon with the monitor filter
     * set. Rtl8733bDevice::Init calls configure_monitor_rx for the same
     * reason. */
    if (mt7612u_set_monitor_rx(_dev, _cfg.rx.keep_corrupted ? 1 : 0) != 0)
      _logger->warn("MT7612U monitor RX filter not applied");
    /* Arms the channel timers and zeroes the MIB counters that GetRxQuality
     * drains. Without it ch_busy never counts. */
    mt7612u_link_stats_start(_dev);
    _rx_active = true;
  }

  _logger->info("MT7612U monitor RX on channel {}", _channel.Channel);

  /* The C layer drives RX from its own libusb event thread, so this loop has
   * nothing to poll — it exists to give StartRxLoop the blocking contract
   * every other backend has, and to notice Stop() and SIGINT. */
  while (!_rx_stop.load() && !g_devourer_should_stop)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

  StopRxLoop();
}

void RtlMt7612uDevice::StopRxLoop() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  _rx_stop = true;
  if (!_rx_active.exchange(false))
    return;
  if (_dev) {
    mt7612u_stop(_dev);
    mt7612u_rx_stop(_dev);
  }
  _logger->info("MT7612U RX stopped after {} frames",
                _rx_frames.load(std::memory_order_relaxed));
}

/* --- channel ------------------------------------------------------------- */

void RtlMt7612uDevice::SetMonitorChannel(SelectedChannel channel) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  enum mt7612u_bw bw = MT7612U_BW_20;
  const char *why = "";

  if (!_dev) {
    _channel = channel; /* remembered until bring-up */
    return;
  }
  if (!mt7612u::width_to_bw(channel.ChannelWidth, bw, why)) {
    _logger->error("MT7612U channel width refused: {}", why);
    return;
  }
  if (mt7612u_set_channel(_dev, channel.Channel, bw) != 0) {
    _logger->error("MT7612U channel set to {} failed", channel.Channel);
    return;
  }
  _channel = channel;
}

SelectedChannel RtlMt7612uDevice::GetSelectedChannel() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  return _channel;
}

/* --- TX ------------------------------------------------------------------ */

bool RtlMt7612uDevice::send_packet(const uint8_t *packet, size_t length) {
  if (!_dev)
    return false;
  return mt7612u_send_packet(_dev, packet, length) == 0;
}

size_t RtlMt7612uDevice::send_packets(const TxPacketView *pkts, size_t count) {
  if (!_dev || !pkts)
    return 0;
  /* TxPacketView and mt7612u_tx_view are the same two fields in the same
   * order, but a reinterpret_cast across a language boundary is the kind of
   * thing that breaks silently when one side gains a member. Copy. */
  std::vector<struct mt7612u_tx_view> views(count);
  for (size_t i = 0; i < count; ++i) {
    views[i].data = pkts[i].data;
    views[i].len = pkts[i].len;
  }
  return mt7612u_send_packets(_dev, views.data(), count);
}

/* --- capabilities -------------------------------------------------------- */

devourer::TxCaps RtlMt7612uDevice::GetTxCaps() {
  devourer::TxCaps c{};
  c.supported = true;
  c.n_ss = 2;
  c.stbc_ok = true;
  c.ldpc_ok = true;
  c.sgi_ok = true;
  /* 40, not 80: the rate word encodes 80 but the channel-width maths is not
   * ported, so a caller must not be told it can ask for it. */
  c.bw_max_mhz = 40;
  return c;
}

devourer::TxPowerCaps RtlMt7612uDevice::GetTxPowerCaps() {
  devourer::TxPowerCaps c{};
  c.supported = true;
  /* index_max 0 = the dBm model, per the field's own contract. There is no
   * TXAGC index on this part: TX power is an absolute dBm limit that feeds
   * the per-rate table, plus a 4-bit per-frame trim in the descriptor. */
  c.index_max = 0;
  c.step_qdb = 2; /* the limit is carried in 0.5 dB units */
  c.step_measured = false;
  c.offset_min_qdb = -80; /* down to 0 dBm from the 20 dBm default */
  c.offset_max_qdb = 40;  /* up to 30 dBm, the API's own ceiling */
  c.rate_diffs = false;
  return c;
}

devourer::AdapterCaps RtlMt7612uDevice::GetAdapterCaps() {
  devourer::AdapterCaps c{};
  c.supported = true;
  c.chip_name = "MT7612U";
  c.marketing_names = "MT7612U/MT7662U";
  c.chip_id = 0; /* no SYS_CFG2 equivalent — dispatch is VID:PID */
  c.generation = devourer::ChipGeneration::Mediatek;
  c.variant = "MT7612U";
  c.transport = "usb";
  c.tx_chains = 2;
  c.rx_chains = 2;
  c.tx = GetTxCaps();
  c.txpwr = GetTxPowerCaps();
  c.bw_mask = devourer::kBw20 | devourer::kBw40;
  c.tune_5g = {true, 5180, 5825};
  c.tune_2g4 = {true, 2412, 2484};
  c.characterized_5g = c.tune_5g;
  c.characterized_2g4 = c.tune_2g4;
  c.ldpc_rx_ht = true;
  c.ldpc_rx_vht = true;
  c.ldpc_rx_flag = true; /* the RXWI carries the per-frame LDPC bit */
  c.per_chain_rssi = true;
  c.hw_rx_timestamp = false; /* the RXWI TSF field is not parsed here */
  c.hw_beacon_txtsf = false; /* no hardware beacon function ported */
  /* Measured on air: 0 frames at the stimulus radio unarmed, 3500+ armed. */
  c.ack_responder_ok = true;
  /* Unmeasured, so false rather than optimistic — nothing here drives the
   * hardware retry counter. */
  c.tx_retry_limit_ok = false;
  c.narrowband_ok = false; /* MT_RATE_BW is two bits, three values */
  /* Measured 526 ms full / 48 ms with calibration skipped, against 0.5-2.5 ms
   * on the Realtek parts: the RF plane lives behind the MCU. Not "fast". */
  c.fastretune_ok = false;
  c.per_packet_txpower = true;
  c.per_pkt_txpwr_steps = 0;
  c.per_pkt_txpwr_step_qdb = 4; /* MT_TX_PWR_ADJ is a 4-bit dB trim */
  c.per_pkt_txpwr_min_qdb = -32;
  c.per_pkt_txpwr_max_qdb = 28;
  c.per_pkt_txpwr_measured = false;
  c.vht_2g4_ok = false; /* unmeasured on this part */
  return c;
}

/* --- TX power ------------------------------------------------------------ */

void RtlMt7612uDevice::SetTxPower(uint8_t power) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  /* Deliberately NOT forwarded to SetTxPowerIndexOverride like the base
   * class does: there is no TXAGC index here, so the argument is read as the
   * dBm limit it actually maps to. */
  _txpwr_dbm = static_cast<int>(power);
  if (_dev && mt7612u_set_txpower(_dev, _txpwr_dbm) != 0)
    _logger->error("MT7612U TX power {} dBm refused (valid range 0-30)",
                   _txpwr_dbm);
}

void RtlMt7612uDevice::SetTxPowerIndexOverride(int idx) {
  _logger->error("MT7612U has no TXAGC index to override (asked for {}); "
                 "TX power here is an absolute dBm limit — use SetTxPower()",
                 idx);
}

int RtlMt7612uDevice::SetTxPowerOffsetQdb(int qdb) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  const devourer::TxPowerCaps caps = GetTxPowerCaps();

  if (qdb < caps.offset_min_qdb)
    qdb = caps.offset_min_qdb;
  if (qdb > caps.offset_max_qdb)
    qdb = caps.offset_max_qdb;
  /* The limit is carried in 0.5 dB units, so a quarter-dB request quantizes
   * to the nearest even qdB. Report what was applied, not what was asked. */
  const int applied = (qdb / 2) * 2;
  const int dbm = _txpwr_dbm + applied / 4;

  if (_dev && mt7612u_set_txpower(_dev, dbm) != 0) {
    _logger->error("MT7612U TX power offset {} qdB -> {} dBm refused", applied,
                   dbm);
    return 0;
  }
  _txpwr_offset_qdb = applied;
  return applied;
}

devourer::TxPowerState RtlMt7612uDevice::GetTxPowerState() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  devourer::TxPowerState s{};
  s.valid = _dev != nullptr;
  s.flat_index = -1; /* no index model */
  s.offset_qdb = static_cast<int16_t>(_txpwr_offset_qdb);
  s.offset_steps = static_cast<int16_t>(_txpwr_offset_qdb / 2);
  s.hw_readback = false;
  return s;
}

/* --- TX mode / aggregation ----------------------------------------------- */

void RtlMt7612uDevice::SetTxMode(const devourer::TxMode &mode) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  struct mt7612u_tx_rate r {};

  switch (mode.mode) {
  case devourer::TxMode::Mode::Legacy:
    /* 500 kbps units -> the CCK/OFDM index the rate word carries. */
    switch (mode.legacy_rate_500kbps) {
    case 2: r.phy = MT7612U_PHY_CCK; r.mcs = 0; break;
    case 4: r.phy = MT7612U_PHY_CCK; r.mcs = 1; break;
    case 11: r.phy = MT7612U_PHY_CCK; r.mcs = 2; break;
    case 22: r.phy = MT7612U_PHY_CCK; r.mcs = 3; break;
    case 12: r.phy = MT7612U_PHY_OFDM; r.mcs = 0; break;
    case 18: r.phy = MT7612U_PHY_OFDM; r.mcs = 1; break;
    case 24: r.phy = MT7612U_PHY_OFDM; r.mcs = 2; break;
    case 36: r.phy = MT7612U_PHY_OFDM; r.mcs = 3; break;
    case 48: r.phy = MT7612U_PHY_OFDM; r.mcs = 4; break;
    case 72: r.phy = MT7612U_PHY_OFDM; r.mcs = 5; break;
    case 96: r.phy = MT7612U_PHY_OFDM; r.mcs = 6; break;
    case 108: r.phy = MT7612U_PHY_OFDM; r.mcs = 7; break;
    default:
      _logger->error("MT7612U: legacy rate {} (500 kbps units) is not an "
                     "802.11 rate; falling back to 6 Mbps",
                     mode.legacy_rate_500kbps);
      r.phy = MT7612U_PHY_OFDM;
      r.mcs = 0;
      break;
    }
    r.nss = 1;
    break;
  case devourer::TxMode::Mode::HT:
    r.phy = MT7612U_PHY_HT;
    r.mcs = mode.ht_mcs;
    r.nss = static_cast<uint8_t>(1 + (mode.ht_mcs >> 3));
    break;
  case devourer::TxMode::Mode::VHT:
    r.phy = MT7612U_PHY_VHT;
    r.mcs = mode.vht_mcs;
    r.nss = mode.vht_nss ? mode.vht_nss : 1;
    break;
  case devourer::TxMode::Mode::HE:
    _logger->error("MT7612U is 802.11ac silicon: HE has no encoding in the "
                   "rate word. TX mode not applied.");
    return;
  }

  r.bw = mode.bw_mhz >= 40 ? MT7612U_BW_40 : MT7612U_BW_20;
  if (mode.bw_mhz > 40)
    _logger->warn("MT7612U: {} MHz requested, narrowing to 40 (80 MHz width "
                  "maths not ported)",
                  mode.bw_mhz);
  r.sgi = mode.sgi;
  r.ldpc = mode.ldpc;
  r.stbc = mode.stbc;
  r.no_ack = 1;

  _tx_default = r;
  _tx_mode_set = true;
}

void RtlMt7612uDevice::ClearTxMode() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  _tx_mode_set = false;
  _tx_default = {};
}

bool RtlMt7612uDevice::SetAmpduMode(const devourer::AmpduMode &mode) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  /* Aggregation on this part is per-frame descriptor state, set on the TXWI
   * at build time rather than programmed into the MAC — so there is nothing
   * to write here and nothing that can fail. Measured: paggr 0/352 with the
   * flag clear against 326/326 with it set, 34.03 -> 44.55 Mbit/s at 1400
   * bytes. Wiring it through send_packet needs the radiotap A-MPDU field
   * plumbed first, which is not done, so this refuses rather than storing a
   * mode nothing reads. */
  (void)mode;
  _logger->error("MT7612U: A-MPDU works on this part (docs/mt7612u.md) but is "
                 "not wired through send_packet yet — refusing rather than "
                 "accepting a mode that would not reach the air");
  return false;
}

void RtlMt7612uDevice::ClearAmpduMode() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  _ampdu = {};
}

devourer::AmpduMode RtlMt7612uDevice::GetAmpduMode() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  return _ampdu;
}

/* --- ACK responder ------------------------------------------------------- */

bool RtlMt7612uDevice::SetAckResponder(const devourer::MacAddr &mac) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  if (!_dev)
    return false;
  return mt7612u_set_ack_responder(_dev, mac.data()) == 0;
}

void RtlMt7612uDevice::ClearAckResponder() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  if (_dev)
    mt7612u_clear_ack_responder(_dev);
}

/* --- misc ---------------------------------------------------------------- */

uint64_t RtlMt7612uDevice::ReadTsf() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  return _dev ? mt7612u_read_tsf(_dev) : 0;
}

void RtlMt7612uDevice::WriteTsf(uint64_t tsf) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  if (_dev)
    mt7612u_write_tsf(_dev, tsf);
}

bool RtlMt7612uDevice::GetPermanentMacAddress(uint8_t out[6]) {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  if (!_dev)
    return false;
  const uint8_t *mac = mt7612u_mac_addr(_dev);
  if (!mac)
    return false;
  std::memcpy(out, mac, 6);
  return true;
}

devourer::TxStats RtlMt7612uDevice::GetTxStats() {
  devourer::TxStats s{};
  if (!_dev)
    return s;
  struct mt7612u_stats st {};
  mt7612u_get_stats(_dev, &st);
  s.submitted = st.tx_submitted;
  s.failed = st.tx_err;
  return s;
}

/*
 * Window RX quality.
 *
 * What this part can and cannot fill in is worth being explicit about,
 * because the empty fields are not laziness:
 *
 *  - SNR and the noise floor ARE available, from RXWI byte 14 - the
 *    `rssi[2]` slot mt76 declares and never reads. Established by
 *    measurement: it is signal-independent (flat across a 43 dB span on
 *    ch36) and orders with the MAC's own false-CCA count across channels.
 *    A reading below -100 dBm is under the thermal floor of a 20 MHz
 *    channel and is treated as no estimate, which is what a quiet channel
 *    carrying only a very strong local transmitter produces.
 *  - No EVM. `bbp_rxinfo[4]`, the other thing mt76 declares and never reads,
 *    measured as two words of zero plus a duplicate of the same two RSSI
 *    values across a 30 dB transmit sweep, so `evm_valid` stays false
 *    rather than carrying a plausible-looking fiction.
 *  - `fa_ofdm` is real: MT_RX_STAT_1's false-CCA count, which is energy that
 *    started a receive and never became a frame. mt76's own AGC loop treats
 *    >800 per interval as interfered and <10 as clean
 *    (mt76x02_phy.c:178-182), so the number has a calibrated meaning.
 *  - The MIB counters are read-and-clear in hardware, which suits the drained
 *    -window contract here exactly.
 */
devourer::RxQuality RtlMt7612uDevice::GetRxQuality() {
  std::lock_guard<std::recursive_mutex> lock(_mu);
  devourer::RxQuality q{};

  if (!_dev)
    return q;

  struct mt7612u_link_stats st {};
  if (mt7612u_link_stats(_dev, &st) == 0) {
    q.energy_valid = true;
    q.fa_ofdm = st.rx_false_cca;
    q.cca_ofdm = st.ch_busy;
  }

  const uint64_t n = _rssi_n.exchange(0, std::memory_order_relaxed);
  const uint64_t sum = _rssi_sum.exchange(0, std::memory_order_relaxed);
  const int peak = _rssi_max.exchange(-127, std::memory_order_relaxed);

  q.frames = static_cast<uint32_t>(n);
  if (n) {
    q.valid = true;
    q.rssi_mean_dbm = static_cast<int>(sum / n) - 128;
    q.rssi_max_dbm = peak;
  }

  const uint64_t sn = _snr_n.exchange(0, std::memory_order_relaxed);
  const int64_t ssum = _snr_sum.exchange(0, std::memory_order_relaxed);
  const int64_t nsum = _noise_sum.exchange(0, std::memory_order_relaxed);
  const int smin = _snr_min.exchange(127, std::memory_order_relaxed);
  if (sn) {
    q.snr_mean_db = static_cast<double>(ssum) / static_cast<double>(sn);
    q.snr_min_db = smin;
    q.noise_floor_dbm = static_cast<double>(nsum) / static_cast<double>(sn);
    q.nf_valid = true;
    q.abs_noise_floor_dbm = static_cast<int8_t>(q.noise_floor_dbm);
    q.abs_nf_valid = true;
  }
  return q;
}

void RtlMt7612uDevice::SetCcaMode(bool disabled) {
  /* Refuses rather than no-ops, per the interface contract. MT7612U does have
   * an ED-CCA enable (MT_TXOP_CTRL_CFG / MT_TXOP_ED_CCA_EN, which mac_stop
   * already clears), but "disable CCA" on the Realtek backends means a
   * specific, measured set of writes, and nothing here has been measured
   * against an on-air carrier-sense test. Claiming it on the strength of one
   * plausible-looking bit is how an unverified regulatory-adjacent behaviour
   * ships. */
  _logger->error("MT7612U: SetCcaMode({}) not implemented — the ED-CCA enable "
                 "exists but no on-air carrier-sense measurement backs it",
                 disabled);
}

void RtlMt7612uDevice::Stop() {
  StopRxLoop();
  std::lock_guard<std::recursive_mutex> lock(_mu);
  if (_dev) {
    mt7612u_stop(_dev);
    mt7612u_close(_dev);
    _dev = nullptr;
  }
}
