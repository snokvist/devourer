// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink devourer glue: one USB adapter = one libusb context + one claimed
// handle + one IRadio, brought up either duplex (InitWrite, then StartRxLoop
// on an internal thread — the TX+RX-on-one-handle contract in src/IRadio.h)
// or receive-only (Init on an internal thread). Every device method the
// application needs is wrapped here so the application never holds a
// devourer header, and so the two thread rules the library imposes are kept
// in one file:
//
//   1. The RX callback runs on the backend's event thread (Realtek Async
//      ring: the libusb pump; MT7612U: the StartRxLoop thread). It must never
//      make a device call and must never block on a lock a device call holds
//      (IRadio::StartRxLoop, the deadlock contract). Here it only parses,
//      copies into an RxFrame and pushes to a bounded queue.
//   2. The control plane (SetMonitorChannel, SetAckResponder, ...) is a
//      single-thread affair and send_packet is serialised with it under one
//      mutex, so a retune never races a frame half-way into the TXDMA.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "TxMode.h"    // devourer::TxMode   (devourer src/, PUBLIC include of the library)
#include "TxReport.h"  // devourer::TxReport
#include "TxStats.h"   // devourer::TxStats
#include "logger.h"    // Logger_t
#include "p2p/wire.h"

namespace p2p {

// "VID:PID" or "VID:PID@BUS-PORT": hex ids, decimal bus, dotted port path as
// `lsusb -t` / sysfs spell it ("1.4"). Bus/port narrow the match when two
// adapters share VID:PID (and, on every RTL88x2 measured, the serial too).
struct AdapterSpec {
  uint16_t vid = 0, pid = 0;
  int bus = -1;       // -1 = any
  std::string port;   // dotted, empty = any
};
bool parse_adapter_spec(const std::string& s, AdapterSpec* out);
std::string format_adapter_spec(const AdapterSpec& s);

struct RadioCfg {
  uint8_t channel = 36;
  uint8_t bw_mhz = 40;          // 20 | 40 (the tune width; frames pick their own in radiotap)
  bool ht40_plus = true;        // primary channel is the lower 20 MHz of the 40 (offset 1)
  bool duplex = false;          // InitWrite + RX thread; false = Init (receive-only ear)
  bool tx_report = false;       // DeviceConfig tx.report = 1: per-frame CCX reports
  bool ack_responder = false;   // SetAckResponder(own MAC) after bring-up (Realtek only)
  std::optional<std::string> mt7612u_fw_dir;
  std::optional<uint8_t> wide_igi_floor;  // DeviceConfig tuning.wide_tx_igi_floor (Jaguar3)
  Logger::Level log_level = Logger::Level::Info;
};

// One received p2plink frame, already filtered to this link (bssid ==
// link_bssid(net_id)) and FCS-clean. `payload` is the bytes after P2pHeader.
struct RxFrame {
  uint8_t ear = 0;
  uint64_t t_ms = 0;        // host monotonic ms at the callback
  int8_t rssi_dbm = -127;   // RxAtrib.rssi[0] - 110 (every backend reports raw = dBm + 110)
  bool retry = false;       // 802.11 Retry bit (a hardware retransmission copy)
  uint16_t seq12 = 0;
  Mac sa{};
  P2pHeader hdr;
  std::vector<uint8_t> payload;
};

class Adapter {
 public:
  ~Adapter();
  Adapter(const Adapter&) = delete;
  Adapter& operator=(const Adapter&) = delete;

  // libusb_init (own context) -> find by VID:PID[/bus/port] -> open -> claim +
  // reset (devourer::claim_interface_then_reset) -> WiFiDriver::CreateRadio.
  // Nothing is brought up yet. nullptr + err on failure, including a chip the
  // factory refuses because its support is not compiled into this binary.
  static std::unique_ptr<Adapter> open(const AdapterSpec& spec, const RadioCfg& cfg, uint8_t ear_index,
                                       uint8_t net_id, Logger_t logger, std::string* err);

  // Duplex: InitWrite (synchronous; a refused bring-up is an exception on
  // every backend and becomes false + err), then StartRxLoop on the internal
  // RX thread. Receive-only: Init(cb, channel) on the internal thread — that
  // call is bring-up + the blocking RX worker in one, so there is no
  // synchronous completion to wait for; this waits a bounded settle time for
  // an early failure and otherwise reports success, and a later failure is
  // visible through rx_failed(). Blocks for the settle time: the ground brings
  // its ears up one at a time on purpose (an MT7612U bring-up does not
  // complete under a saturated channel, so each one gets the air to itself).
  bool bring_up(std::string* err);

  // Pops one frame; false on timeout. rx_fd() is an eventfd written on every
  // push, so an application can poll() it beside its tun / ring / timer fds
  // and then drain with pop_rx(out, 0) until it returns false.
  bool pop_rx(RxFrame* out, int timeout_ms);
  int rx_fd() const;

  // Builds radiotap (build_stream_radiotap(mode, no_ack = group DA)) + 802.11
  // Data header (bssid = link_bssid(net_id)) + P2pHeader + payload and calls
  // send_packet under the adapter's TX mutex. When `on_tag` is given it is
  // called under that mutex immediately BEFORE send_packet with the SW_DEFINE
  // tag this frame will carry (IRtlRadio::NextTxReportTag; nullopt when the
  // die echoes none or tx_report is off), so the caller's tag -> frame record
  // exists before the TxReport can possibly arrive. The hook must not call
  // back into this Adapter. Returns send_packet's result. Never call from the
  // RX callback path (it is not — the application owns both ends).
  using TagHook = std::function<void(std::optional<uint8_t>)>;
  bool send(const devourer::TxMode& mode, const Mac& da, const Mac& sa, const P2pHeader& h,
            const uint8_t* payload, size_t len, const TagHook* on_tag);

  // Forwards to IRadio::SetTxReportSink. The sink runs on the backend's C2H
  // thread (Jaguar3: the coex thread or the RX worker): enqueue only, and set
  // it before bring_up so the first reports are not missed.
  void set_tx_report_sink(std::function<void(const devourer::TxReport&)> sink);

  // GetPermanentMacAddress after bring-up; a backend without one gets a
  // locally administered random unicast MAC (02:xx:...), logged once. Valid
  // after bring_up().
  Mac mac() const;
  bool is_realtek() const;
  const char* chip_name() const;
  uint8_t ear_index() const;

  // Full SetMonitorChannel (not FastRetune: the width changes). Serialised
  // with send() on the TX mutex, so a frame in flight completes first and no
  // send starts until the retune returns. Main-loop call only.
  bool retune(uint8_t channel, uint8_t bw_mhz, bool ht40_plus, std::string* err);
  uint8_t channel() const;
  uint8_t bw_mhz() const;

  devourer::TxStats tx_stats();

  struct Counters {
    uint64_t rx_frames = 0;    // delivered to the queue
    uint64_t rx_crc = 0;       // crc_err / icv_err frames dropped
    uint64_t rx_foreign = 0;   // not ours: not Data, other bssid, no P2pHeader
    uint64_t rx_dropped_q = 0; // queue full (newest dropped)
  };
  Counters counters() const;

  // True once the internal RX thread has exited on an exception (a bring-up
  // or worker failure after bring_up returned); `why` gets the message.
  bool rx_failed(std::string* why) const;

  // StopRxLoop, join the RX thread, then the DeviceSession teardown order
  // (device, interface, handle, context). Idempotent; the destructor calls it.
  void stop();

 private:
  Adapter();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Shared clock for every timestamp in this layer: CLOCK_MONOTONIC in ms.
uint64_t now_ms();

}  // namespace p2p
