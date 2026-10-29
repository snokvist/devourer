// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2p/radio.h"

#include <libusb.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <random>
#include <thread>

#include "AdapterCaps.h"
#include "DeviceConfig.h"
#include "DeviceSession.h"  // examples/common: the teardown-order holder
#include "IRadio.h"
#include "IRtlRadio.h"
#include "RadiotapBuilder.h"
#include "RxPacket.h"
#include "SelectedChannel.h"
#include "UsbDeviceLock.h"
#include "UsbOpen.h"
#include "WiFiDriver.h"

namespace p2p {

uint64_t now_ms() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

namespace {

// RX queue bound. 1024 frames is ~0.7 s of a saturated 20 MHz MCS0 stream;
// a consumer that falls further behind than that is not going to catch up on
// the frames it dropped, so the newest is dropped and counted.
constexpr size_t kRxQueueCap = 1024;

// Receive-only bring-up has no completion signal (IRadio::Init is bring-up +
// the blocking worker in one call). Bring-ups measured on the bench (Jaguar3,
// MT7612U) complete within ~2-4 s; a failure throws out of Init earlier than
// that in every observed case, so an exception-free settle of this length is
// taken as "up".
constexpr int kRxOnlySettleMs = 6000;

// devourer carries RSSI as an unsigned byte biased by +110 on every backend
// (src/mt7612u/Mt7612uMapping.h cites LinkHealth.cpp as the authority).
constexpr int kRssiBiasDb = 110;

bool parse_hex16(const std::string& s, uint16_t* out) {
  if (s.empty() || s.size() > 4) return false;
  unsigned v = 0;
  for (char c : s) {
    int d;
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
    else return false;
    v = (v << 4) | unsigned(d);
  }
  *out = uint16_t(v);
  return true;
}

std::string port_path(libusb_device* dev) {
  uint8_t ports[8];
  int n = libusb_get_port_numbers(dev, ports, sizeof ports);
  std::string s;
  for (int i = 0; i < n; i++) {
    if (i) s += '.';
    s += std::to_string(unsigned(ports[i]));
  }
  return s;
}

SelectedChannel make_channel(uint8_t channel, uint8_t bw_mhz, bool ht40_plus) {
  // ChannelOffset follows the demos' HAL_PRIME_CHNL_OFFSET convention
  // (src/ToneMask.h): 1 = LOWER = HT40+ (primary is the lower 20, secondary
  // above), 2 = UPPER = HT40-, 0 for a 20 MHz tune.
  SelectedChannel c{};
  c.Channel = channel;
  c.ChannelWidth = bw_mhz == 40 ? CHANNEL_WIDTH_40 : CHANNEL_WIDTH_20;
  c.ChannelOffset = bw_mhz == 40 ? uint8_t(ht40_plus ? 1 : 2) : uint8_t(0);
  return c;
}

}  // namespace

bool parse_adapter_spec(const std::string& s, AdapterSpec* out) {
  if (!out) return false;
  AdapterSpec a;
  std::string ids = s, topo;
  auto at = s.find('@');
  if (at != std::string::npos) {
    ids = s.substr(0, at);
    topo = s.substr(at + 1);
  }
  auto colon = ids.find(':');
  if (colon == std::string::npos) return false;
  if (!parse_hex16(ids.substr(0, colon), &a.vid) || !parse_hex16(ids.substr(colon + 1), &a.pid)) return false;
  if (!topo.empty()) {
    auto dash = topo.find('-');
    std::string bus = dash == std::string::npos ? topo : topo.substr(0, dash);
    if (bus.empty()) return false;
    for (char c : bus)
      if (c < '0' || c > '9') return false;
    a.bus = std::stoi(bus);
    if (dash != std::string::npos) {
      a.port = topo.substr(dash + 1);
      for (char c : a.port)
        if (!((c >= '0' && c <= '9') || c == '.')) return false;
    }
  }
  *out = a;
  return true;
}

std::string format_adapter_spec(const AdapterSpec& s) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%04x:%04x", s.vid, s.pid);
  std::string r = buf;
  if (s.bus >= 0) {
    r += "@" + std::to_string(s.bus);
    if (!s.port.empty()) r += "-" + s.port;
  }
  return r;
}

struct Adapter::Impl {
  explicit Impl(Logger_t lg) : logger(std::move(lg)), session(logger) {}

  Logger_t logger;
  devourer::DeviceSession session;   // device, lock, handle, context — dies in that order
  IRadio* dev = nullptr;
  IRtlRadio* rtl = nullptr;          // non-null on a Realtek backend
  RadioCfg cfg;
  AdapterSpec spec;
  uint8_t ear = 0, net_id = 0;
  uint8_t nonce = 0;  // P2pHeader::session stamped on every frame this adapter sends
  Mac bssid{};
  Mac own_mac{};
  bool mac_valid = false;
  devourer::AdapterCaps caps;
  std::string chip;

  // RX fan-in: callback -> queue -> application.
  std::mutex q_mu;
  std::condition_variable q_cv;
  std::deque<RxFrame> q;
  int evfd = -1;
  std::atomic<uint64_t> c_frames{0}, c_crc{0}, c_foreign{0}, c_dropped{0};

  // RX worker thread (StartRxLoop or Init) and its failure capture.
  std::thread rx_thread;
  std::mutex rx_mu;
  std::condition_variable rx_cv;
  bool rx_exited = false;
  std::string rx_error;

  // Serialises send_packet with the control plane (retune, ack responder).
  std::mutex tx_mu;
  uint8_t cur_channel = 0, cur_bw = 20;
  bool cur_ht40_plus = true;
  bool up = false;
  std::atomic<bool> stopped{false};  // read under tx_mu by send/retune; set by stop()

  void on_packet(const Packet& p);
  void push(RxFrame&& f);
  void rx_entry(bool duplex);
  void note_rx_exit(const char* what);
};

Adapter::Adapter() = default;
Adapter::~Adapter() { stop(); }

std::unique_ptr<Adapter> Adapter::open(const AdapterSpec& spec, const RadioCfg& cfg, uint8_t ear_index,
                                       uint8_t net_id, Logger_t logger, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = format_adapter_spec(spec) + ": " + m;
    return std::unique_ptr<Adapter>();
  };
  if (!logger) logger = std::make_shared<Logger>();
  logger->set_level(cfg.log_level);
  if (cfg.bw_mhz != 20 && cfg.bw_mhz != 40) return fail("bw must be 20 or 40");

  std::unique_ptr<Adapter> a(new Adapter());
  a->impl_ = std::make_unique<Impl>(logger);
  Impl& im = *a->impl_;
  im.cfg = cfg;
  im.spec = spec;
  im.ear = ear_index;
  im.net_id = net_id;
  im.bssid = link_bssid(net_id);
  {
    std::random_device rd;
    do im.nonce = uint8_t(rd()); while (im.nonce == 0);
  }
  im.cur_channel = cfg.channel;
  im.cur_bw = cfg.bw_mhz;
  im.cur_ht40_plus = cfg.ht40_plus;

  im.evfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (im.evfd < 0) return fail(std::string("eventfd: ") + std::strerror(errno));

  // Own libusb context per adapter (examples/sense pattern): each stack
  // unwinds independently and in the required order.
  libusb_context* ctx = nullptr;
  if (libusb_init(&ctx) < 0) return fail("libusb_init failed");
  im.session.adopt_context(ctx);

  libusb_device** list = nullptr;
  ssize_t n = libusb_get_device_list(ctx, &list);
  if (n < 0) return fail("libusb_get_device_list failed");
  libusb_device* match = nullptr;
  int candidates = 0;
  for (ssize_t i = 0; i < n; i++) {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(list[i], &d) != 0) continue;
    if (d.idVendor != spec.vid || d.idProduct != spec.pid) continue;
    candidates++;
    if (spec.bus >= 0 && libusb_get_bus_number(list[i]) != spec.bus) continue;
    if (!spec.port.empty() && port_path(list[i]) != spec.port) continue;
    if (!match) match = list[i];
  }
  libusb_device_handle* handle = nullptr;
  int rc = match ? libusb_open(match, &handle) : LIBUSB_ERROR_NOT_FOUND;
  libusb_free_device_list(list, 1);
  if (!match) {
    return fail(candidates ? "no device at that bus/port (" + std::to_string(candidates) +
                                 " with this VID:PID elsewhere)"
                           : "no such device — plugged in? (lsusb)");
  }
  if (rc != 0 || !handle)
    return fail(std::string("libusb_open: ") + libusb_error_name(rc) + " (permissions? run as root)");

  // Lock + claim before reset (src/UsbOpen.h). BUSY = another process owns
  // the adapter; the handle is adopted either way so the unwind closes it.
  // The reopen variant (rxdemo's path): a reset that re-enumerates the device
  // replaces `handle` in place instead of leaving it stale.
  std::shared_ptr<devourer::UsbDeviceLock> lock;
  rc = devourer::claim_interface_reset_reopen(ctx, handle, logger, /*do_reset=*/true, lock);
  im.session.adopt_handle(handle, devourer::find_wifi_interface(handle));
  if (rc != 0) {
    return fail(rc == LIBUSB_ERROR_BUSY ? "adapter in use by another process"
                                        : std::string("claim/reset failed: ") + libusb_error_name(rc));
  }
  im.session.adopt_lock(lock);

  // Fixed radio parameters (docs/DESIGN.md "Fixed radio parameters").
  devourer::DeviceConfig dc;
  dc.tx.retry_limit = 3;
  dc.tx.ack_timeout_us = 128;
  dc.tx.report = cfg.tx_report ? 1 : 0;
  // A multi-packet bulk-OUT cancelled on its 20 ms timeout can wedge the
  // TXDMA (DeviceConfig.h, no_cancel_multipkt). Every duplex role here sends
  // frames longer than one USB packet, so that send waits instead.
  dc.tx.no_cancel_multipkt = cfg.duplex;
  // Jaguar3: the RX path must be part of the TX bring-up (decided here, before
  // InitWrite) or StartRxLoop on the same handle is a best-effort retrofit.
  dc.rx.enable_with_tx = cfg.duplex;
  dc.rx.keep_corrupted = false;
  dc.rx.pool_exhaust = devourer::PoolExhaust::Backpressure;
  dc.tx.usb_agg_max = 0;
  if (cfg.wide_igi_floor) dc.tuning.wide_tx_igi_floor = *cfg.wide_igi_floor;
  if (cfg.mt7612u_fw_dir) dc.mt7612u.firmware_dir = *cfg.mt7612u_fw_dir;

  WiFiDriver driver(logger);
  auto dev = driver.CreateRadio(handle, ctx, lock, dc);
  if (!dev) {
    // The factory logged the specific refusal (chip id / PID) on stderr.
    return fail("CreateRadio refused the device: chip not built into this binary "
                "(DEVOURER_* / P2P_WITH_MT7612U), unsupported, or in use");
  }
  im.dev = dev.get();
  im.rtl = dynamic_cast<IRtlRadio*>(im.dev);
  im.session.adopt_device(std::move(dev));
  im.caps = im.dev->GetAdapterCaps();
  im.chip = (im.caps.chip_name && im.caps.chip_name[0]) ? im.caps.chip_name : (im.rtl ? "Realtek" : "unknown");
  logger->info("p2p ear {}: {} ({}) opened, {} mode", unsigned(ear_index), im.chip, format_adapter_spec(spec),
               cfg.duplex ? "duplex" : "rx-only");
  return a;
}

void Adapter::Impl::push(RxFrame&& f) {
  {
    std::lock_guard<std::mutex> lk(q_mu);
    if (q.size() >= kRxQueueCap) {
      c_dropped.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    q.push_back(std::move(f));
  }
  c_frames.fetch_add(1, std::memory_order_relaxed);
  q_cv.notify_one();
  uint64_t one = 1;
  if (::write(evfd, &one, sizeof one) < 0) { /* counter saturated: the reader is already awake */ }
}

// The device callback. Runs on the backend's event thread: parse, filter,
// copy, push — no device call, no lock any device call holds.
void Adapter::Impl::on_packet(const Packet& p) {
  const rx_pkt_attrib& at = p.RxAtrib;
  if (at.pkt_rpt_type != RX_PACKET_TYPE::NORMAL_RX) return;
  if (at.crc_err || at.icv_err) {
    c_crc.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const uint8_t* d = p.Data.data();
  size_t len = p.Data.size();
  // Realtek appends the validated FCS (RCR APPFCS); the MT7612U MAC strips it
  // and the four bytes after the MPDU are NOT a checksum (RxPacket.h).
  if (at.fcs_present) {
    if (len < kFcsLen) return;
    len -= kFcsLen;
  }
  auto v = parse_dot11(d, len);
  if (!v || v->bssid != bssid) {
    c_foreign.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  auto h = decode_header(v->body, v->body_len);
  if (!h || h->net_id != net_id) {
    c_foreign.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  RxFrame f;
  f.ear = ear;
  f.t_ms = now_ms();
  f.rssi_dbm = int8_t(std::max(-127, std::min(127, int(at.rssi[0]) - kRssiBiasDb)));
  f.retry = v->retry;
  f.seq12 = v->seq12;
  f.sa = v->sa;
  f.hdr = *h;
  f.payload.assign(v->body + kHdrLen, v->body + kHdrLen + h->payload_len);
  push(std::move(f));
}

void Adapter::Impl::note_rx_exit(const char* what) {
  std::lock_guard<std::mutex> lk(rx_mu);
  rx_exited = true;
  if (what) rx_error = what;
  rx_cv.notify_all();
}

void Adapter::Impl::rx_entry(bool duplex) {
  auto cb = [this](const Packet& p) { on_packet(p); };
  try {
    if (duplex) {
      dev->StartRxLoop(cb);
    } else {
      dev->Init(cb, make_channel(cfg.channel, cfg.bw_mhz, cfg.ht40_plus));
    }
    note_rx_exit(nullptr);
  } catch (const std::exception& e) {
    logger->error("p2p ear {}: RX worker exited: {}", unsigned(ear), e.what());
    note_rx_exit(e.what());
  } catch (...) {
    note_rx_exit("unknown exception");
  }
}

bool Adapter::bring_up(std::string* err) {
  Impl& im = *impl_;
  if (im.up) return true;
  if (im.cfg.duplex) {
    // A refused bring-up surfaces as an exception on every backend.
    try {
      std::lock_guard<std::mutex> lk(im.tx_mu);
      im.dev->InitWrite(make_channel(im.cfg.channel, im.cfg.bw_mhz, im.cfg.ht40_plus));
      if (im.cfg.txpwr_offset_qdb) {
        const int applied = im.dev->SetTxPowerOffsetQdb(*im.cfg.txpwr_offset_qdb);
        im.logger->info("p2p ear {}: TX power offset {} qdB requested, {} qdB applied", unsigned(im.ear),
                        *im.cfg.txpwr_offset_qdb, applied);
      }
    } catch (const std::exception& e) {
      if (err) *err = std::string("InitWrite: ") + e.what();
      return false;
    }
    im.rx_thread = std::thread([&im] { im.rx_entry(true); });
  } else {
    im.rx_thread = std::thread([&im] { im.rx_entry(false); });
    std::unique_lock<std::mutex> lk(im.rx_mu);
    im.rx_cv.wait_for(lk, std::chrono::milliseconds(kRxOnlySettleMs), [&im] { return im.rx_exited; });
    if (im.rx_exited) {
      if (err) *err = "Init: " + (im.rx_error.empty() ? std::string("RX worker returned at once") : im.rx_error);
      lk.unlock();
      if (im.rx_thread.joinable()) im.rx_thread.join();
      return false;
    }
  }
  im.up = true;

  uint8_t m[6];
  if (im.dev->GetPermanentMacAddress(m)) {
    std::memcpy(im.own_mac.data(), m, 6);
    im.mac_valid = true;
  } else {
    std::random_device rd;
    for (auto& b : im.own_mac) b = uint8_t(rd());
    im.own_mac[0] = 0x02;  // locally administered, unicast
    im.logger->warn("p2p ear {}: no permanent MAC from the backend; using a random locally "
                    "administered one {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                    unsigned(im.ear), im.own_mac[0], im.own_mac[1], im.own_mac[2], im.own_mac[3],
                    im.own_mac[4], im.own_mac[5]);
  }

  if (im.cfg.ack_responder) {
    if (!im.rtl) {
      // The MT7612U auto-ACKs its own factory MAC from bring-up (its port
      // identity register is that address); retargeting it is not wanted.
      im.logger->info("p2p ear {}: {} answers ACKs for its factory MAC without an arm", unsigned(im.ear),
                      im.chip);
    } else {
      devourer::MacAddr ma;
      ma.bytes = im.own_mac;
      bool ok;
      {
        std::lock_guard<std::mutex> lk(im.tx_mu);
        ok = im.dev->SetAckResponder(ma);
      }
      if (!ok) {
        if (err) *err = "SetAckResponder(own MAC) refused";
        return false;
      }
      im.logger->info("p2p ear {}: hardware ACK responder armed on own MAC", unsigned(im.ear));
    }
  }
  return true;
}

bool Adapter::pop_rx(RxFrame* out, int timeout_ms) {
  Impl& im = *impl_;
  std::unique_lock<std::mutex> lk(im.q_mu);
  if (im.q.empty()) {
    if (timeout_ms <= 0) return false;
    im.q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&im] { return !im.q.empty(); });
    if (im.q.empty()) return false;
  }
  *out = std::move(im.q.front());
  im.q.pop_front();
  return true;
}

int Adapter::rx_fd() const { return impl_->evfd; }

bool Adapter::send(const devourer::TxMode& mode, const Mac& da, const Mac& sa, const P2pHeader& h,
                   const uint8_t* payload, size_t len, const TagHook* on_tag) {
  Impl& im = *impl_;
  // NOACK for a group DA (nothing answers; a backend that honours the bit must
  // not wait), ACK-requested for unicast so the hardware retries. Only the
  // MT7612U reads this bit; Realtek derives BMC from addr1 (RadiotapBuilder.h).
  std::vector<uint8_t> frame = devourer::build_stream_radiotap(mode, /*no_ack=*/is_group(da));
  const size_t rt = frame.size();
  frame.resize(rt + kDot11HdrLen + kHdrLen + len);
  encode_dot11(da, sa, im.bssid, frame.data() + rt);
  P2pHeader hs = h;
  hs.session = im.nonce;
  encode_header(hs, frame.data() + rt + kDot11HdrLen);
  if (len) std::memcpy(frame.data() + rt + kDot11HdrLen + kHdrLen, payload, len);

  std::lock_guard<std::mutex> lk(im.tx_mu);
  if (!im.up || im.stopped) return false;
  if (on_tag && *on_tag) (*on_tag)(im.rtl ? im.rtl->NextTxReportTag() : std::nullopt);
  return im.dev->send_packet(frame.data(), frame.size());
}

void Adapter::set_tx_report_sink(std::function<void(const devourer::TxReport&)> sink) {
  impl_->dev->SetTxReportSink(std::move(sink));
}

Mac Adapter::mac() const { return impl_->own_mac; }
bool Adapter::is_realtek() const { return impl_->rtl != nullptr; }
const char* Adapter::chip_name() const { return impl_->chip.c_str(); }
uint8_t Adapter::ear_index() const { return impl_->ear; }
uint8_t Adapter::session() const { return impl_->nonce; }
uint8_t Adapter::channel() const { return impl_->cur_channel; }
uint8_t Adapter::bw_mhz() const { return impl_->cur_bw; }

bool Adapter::retune(uint8_t channel, uint8_t bw_mhz, bool ht40_plus, std::string* err) {
  Impl& im = *impl_;
  if (bw_mhz != 20 && bw_mhz != 40) {
    if (err) *err = "bw must be 20 or 40";
    return false;
  }
  try {
    // Under the TX mutex: a bare SetMonitorChannel is not serialised against
    // send_packet inside the backend (RtlJaguar3Device.h, FastRetune note).
    std::lock_guard<std::mutex> lk(im.tx_mu);
    if (!im.up || im.stopped) return false;
    im.dev->SetMonitorChannel(make_channel(channel, bw_mhz, ht40_plus));
    im.cur_channel = channel;
    im.cur_bw = bw_mhz;
    im.cur_ht40_plus = ht40_plus;
  } catch (const std::exception& e) {
    if (err) *err = std::string("SetMonitorChannel: ") + e.what();
    return false;
  }
  return true;
}

devourer::TxStats Adapter::tx_stats() { return impl_->dev ? impl_->dev->GetTxStats() : devourer::TxStats{}; }

Adapter::Counters Adapter::counters() const {
  const Impl& im = *impl_;
  Counters c;
  c.rx_frames = im.c_frames.load(std::memory_order_relaxed);
  c.rx_crc = im.c_crc.load(std::memory_order_relaxed);
  c.rx_foreign = im.c_foreign.load(std::memory_order_relaxed);
  c.rx_dropped_q = im.c_dropped.load(std::memory_order_relaxed);
  return c;
}

bool Adapter::rx_failed(std::string* why) const {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.rx_mu);
  if (!im.rx_exited || im.rx_error.empty()) return false;
  if (why) *why = im.rx_error;
  return true;
}

void Adapter::stop() {
  if (!impl_) return;
  Impl& im = *impl_;
  if (im.stopped.exchange(true)) return;
  if (im.dev) {
    // Clear the sink first: SetTxReportSink returns only once no earlier sink
    // is executing, so what the application's sink captured may die after.
    im.dev->SetTxReportSink({});
  }
  if (im.rx_thread.joinable()) {
    // StopRxLoop only sets a flag, and StartRxLoop CLEARS that flag on entry
    // (restartable; Jaguar3 and MT7612U alike). A stop that lands before the
    // worker reaches StartRxLoop — an Init still in bring-up, or a thread
    // just spawned — is wiped and the join would wait forever. Re-assert it
    // until the worker is seen to exit.
    std::unique_lock<std::mutex> lk(im.rx_mu);
    while (!im.rx_exited) {
      lk.unlock();
      im.dev->StopRxLoop();
      lk.lock();
      im.rx_cv.wait_for(lk, std::chrono::milliseconds(50), [&im] { return im.rx_exited; });
    }
    lk.unlock();
    im.rx_thread.join();
  }
  if (im.dev && im.up) {
    try {
      std::lock_guard<std::mutex> lk(im.tx_mu);
      im.dev->Stop();
    } catch (const std::exception& e) {
      im.logger->warn("p2p ear {}: Stop(): {}", unsigned(im.ear), e.what());
    }
  }
  im.dev = nullptr;
  im.rtl = nullptr;
  im.session.close();  // device, interface, handle, context
  if (im.evfd >= 0) {
    ::close(im.evfd);
    im.evfd = -1;
  }
}

}  // namespace p2p
