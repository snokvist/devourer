// SPDX-License-Identifier: GPL-2.0-or-later
// p2p_tx — the drone end of the p2plink (docs/DESIGN.md "Roles": Drone).
//
// Threads:
//   (a) the adapter's RX worker (inside p2p::Adapter; its callback only queues),
//   (b) ONE TX thread owning Adapter::send: runs the TxScheduler dequeue ->
//       bind (rate / FEC / DA at dequeue, never at enqueue) -> send ->
//       on_sent loop and airs the probe every --probe-ms,
//   (c) the main loop: poll() over the encoder ring, the tun, the adapter's
//       RX queue and a 20 ms timer; feeds the scheduler, drains link reports
//       and TX reports into the LinkController / DaMachine, publishes stats,
//       and performs the drone's own 20<->40 MHz tune change,
//   (d) an encoder actuation thread (popen is blocking; the main loop is not
//       allowed to be).
// Every thread is joined in shutdown; nothing here throws across a thread.
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Event.h"
#include "p2p/budget.h"
#include "p2p/fec.h"
#include "p2p/frame_shm.h"
#include "p2p/link_ctl.h"
#include "p2p/radio.h"
#include "p2p/report.h"
#include "p2p/rx_merge.h"
#include "p2p/stats_pub.h"
#include "p2p/tun.h"
#include "p2p/wire.h"

using namespace p2p;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

// The TX scheduler's maximum source-symbol count per block. n stays <= 192 at
// the worst 8/12 ratio, under the RS limit of 255 (fec.h).
constexpr uint8_t kMaxK = 128;
// One IP packet per frame: capacity minus the 2-byte packed length prefix.
constexpr int kTunMtu = int(1400 - kHdrLen - 2);
constexpr uint32_t kPackTimerMs = 5;
constexpr uint32_t kTickMs = 20;
constexpr uint32_t kControlTickMs = 100;
constexpr uint32_t kActuateMinGapMs = 500;

struct Opts {
  std::string adapter;
  int channel = 36;
  int bw = 40;
  bool ht40_plus = true;
  int net_id = 1;
  std::string peer_mac;
  std::string ring;
  std::string tun, tun_addr;
  std::string bitrate_cmd;
  uint32_t control_reserve_kbps = 300;
  int start_rung = 2;
  int fixed_rung = -1;
  bool no_tune20 = false;
  int probe_ms = 100;
  std::optional<uint8_t> wide_igi_floor;
  std::optional<int> txpwr_offset_qdb;
  std::optional<std::string> mt7612u_fw;
  std::string stats_shm = "/p2plink.tx";
  bool ndjson = true;
  // Default warn: devourer logs every successful bulk-OUT at info (one stderr
  // line + flush per frame), a measurable CPU cost at video frame rates.
  // Everything p2plink itself reports at info is also in the NDJSON events.
  Logger::Level log_level = Logger::Level::Warn;
};

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s --adapter VID:PID[@BUS-PORT] --peer-mac XX:XX:XX:XX:XX:XX [options]\n"
      "  --channel N             5 GHz channel (default 36)\n"
      "  --bw 20|40              tune width (default 40; frames pick 20/40 per rung)\n"
      "  --offset +|-            HT40 primary lower (+, default) or upper (-)\n"
      "  --net-id N              link id 0..255 (default 1)\n"
      "  --peer-mac MAC          the ground's ACK target (REQUIRED; p2p_rx prints it)\n"
      "  --ring NAME             encoder frame ring (shm); without it no video is sent\n"
      "  --tun NAME --tun-addr CIDR   IP tunnel (optional)\n"
      "  --bitrate-cmd 'CMD %%u' encoder actuation, %%u = kbps (popen, <= 1 per 500 ms, >5%% change)\n"
      "  --control-reserve-kbps N (default 300)\n"
      "  --start-rung N          (default 2)\n"
      "  --fixed-rung N          bypass the LinkController (bench cells)\n"
      "  --no-tune20             never retune the radio itself to 20 MHz\n"
      "  --probe-ms N            probe period (default 100)\n"
      "  --wide-igi-floor 0xNN   Jaguar3 wide-TX IGI floor\n"
      "  --txpwr-offset-qdb N    TX power offset vs the calibrated table, quarter dB (-40 = -10 dB)\n"
      "  --mt7612u-fw DIR        MT7612U firmware directory\n"
      "  --stats-shm NAME        (default /p2plink.tx)\n"
      "  --no-ndjson             no p2p.stats lines on stdout\n"
      "  --log-level trace|debug|info|warn|error  (default warn)\n",
      argv0);
}

bool parse_level(const std::string& s, Logger::Level* l) {
  if (s == "trace") *l = Logger::Level::Trace;
  else if (s == "debug") *l = Logger::Level::Debug;
  else if (s == "info") *l = Logger::Level::Info;
  else if (s == "warn") *l = Logger::Level::Warn;
  else if (s == "error") *l = Logger::Level::Error;
  else return false;
  return true;
}

bool parse_mac(const std::string& s, Mac* m) {
  unsigned v[6];
  char tail;
  if (std::sscanf(s.c_str(), "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6)
    return false;
  for (int i = 0; i < 6; i++) {
    if (v[i] > 255) return false;
    (*m)[i] = uint8_t(v[i]);
  }
  return true;
}

std::string mac_str(const Mac& m) {
  char b[18];
  std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

devourer::TxMode rung_mode(const Rung& r) {
  devourer::TxMode m;
  m.mode = devourer::TxMode::Mode::HT;
  m.ht_mcs = r.mcs;
  m.bw_mhz = r.bw_mhz;
  m.ldpc = true;
  m.sgi = false;
  return m;
}

// Encoder actuation off the main loop: popen() blocks for the command's
// lifetime, which the main loop cannot afford. Rate-limited to one run per
// kActuateMinGapMs and to a >5% change, so a flapping budget never spawns a
// shell storm.
class Actuator {
 public:
  explicit Actuator(std::string cmd, Logger_t lg) : cmd_(std::move(cmd)), lg_(std::move(lg)) {
    if (!cmd_.empty()) th_ = std::thread([this] { run(); });
  }
  ~Actuator() { stop(); }
  void stop() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
  }
  void target(uint32_t kbps) {
    if (cmd_.empty()) return;
    std::lock_guard<std::mutex> lk(mu_);
    pending_ = kbps;
    cv_.notify_all();
  }

 private:
  void run() {
    uint32_t last_sent = 0;
    uint64_t last_ms = 0;
    for (;;) {
      uint32_t kbps;
      {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait(lk, [this] { return stop_ || pending_.has_value(); });
        if (stop_) return;
        kbps = *pending_;
        pending_.reset();
      }
      const uint64_t now = now_ms();
      const bool big_change = last_sent == 0 || std::abs(int64_t(kbps) - int64_t(last_sent)) * 20 > int64_t(last_sent);
      if (!big_change) continue;
      if (now - last_ms < kActuateMinGapMs) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(kActuateMinGapMs - (now - last_ms)), [this] { return stop_; });
        if (stop_) return;
        if (!pending_) pending_ = kbps;  // retry after the gap unless a newer value arrived
        continue;
      }
      std::string cmd = cmd_;
      for (size_t p; (p = cmd.find("%u")) != std::string::npos;) cmd.replace(p, 2, std::to_string(kbps));
      FILE* f = popen(cmd.c_str(), "r");
      if (!f) {
        lg_->warn("bitrate-cmd: popen failed: {}", std::strerror(errno));
      } else {
        char buf[256];
        while (fgets(buf, sizeof buf, f)) {}
        int rc = pclose(f);
        if (rc != 0) lg_->warn("bitrate-cmd exited {} for {} kbps", rc, kbps);
      }
      last_sent = kbps;
      last_ms = now;
    }
  }
  std::string cmd_;
  Logger_t lg_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::optional<uint32_t> pending_;
  bool stop_ = false;
  std::thread th_;
};

// What the frame carrying a given SW_DEFINE tag was. EVERY sent frame writes
// its slot (before send_packet), so after the 8-bit tag wraps a slot always
// names the latest frame with that tag: a probe whose report was lost cannot
// later claim a unicast IP frame's report.
struct TagSlot {
  enum Kind : uint8_t { None, Probe, Other } kind = None;
  uint32_t seq = 0;
  uint64_t t_ms = 0;
};

struct Report {
  bool has_tag = false;
  uint8_t tag = 0, state = 0, retries = 0;
  bool bmc = false;
};

// Everything the TX thread and the main loop share.
struct Shared {
  Adapter* ad = nullptr;
  Mac own{}, peer{}, group{};
  uint8_t net_id = 1;
  std::atomic<bool> stop{false};

  // Scheduler + its lock (TxScheduler is not thread-safe).
  std::mutex sched_mu;
  std::condition_variable sched_cv;
  TxScheduler sched;

  // Published by the main loop, read at dequeue by the TX thread.
  std::atomic<uint8_t> rung_phy{2};
  std::atomic<uint8_t> tune_bw{40};
  std::atomic<bool> ucast_ctl{false}, ucast_inter{false}, ucast_bulk{false};
  std::atomic<int> probe_ms{100};

  // tag -> sent frame, written by the TX thread, consumed by the main loop.
  std::mutex probe_mu;
  TagSlot tags[256];
  std::atomic<uint32_t> probe_seq{0};
  std::atomic<bool> tags_available{true};

  // TX reports (C2H thread) -> main loop.
  std::mutex rpt_mu;
  std::vector<Report> rpts;

  // Counters (TX thread) for stats.
  std::atomic<uint64_t> frames_sent{0}, frames_failed{0}, video_blocks{0}, oversize_drops{0};
  std::atomic<uint32_t> sym_seq{0}, ip_seq{0};
  std::atomic<uint16_t> block_id{0};
  Logger_t lg;
};

class TxThread {
 public:
  explicit TxThread(Shared& s) : s_(s) {}
  void start() { th_ = std::thread([this] { run(); }); }
  void join() {
    if (th_.joinable()) th_.join();
  }

 private:
  RsCodec& codec(uint8_t k, uint8_t n) {
    uint16_t key = uint16_t((uint16_t(k) << 8) | n);
    auto it = codecs_.find(key);
    if (it == codecs_.end()) it = codecs_.try_emplace(key, k, n).first;
    return it->second;
  }

  // The rung the air carries: the controller's PHY rung, narrowed to R3 when
  // the radio is still tuned to 20 MHz (a 40 MHz frame cannot go out on a
  // 20 MHz tune; R3 has R4's PHY rate at 20 MHz, so nothing is lost while the
  // main loop retunes).
  uint8_t air_rung() const {
    uint8_t r = s_.rung_phy.load(std::memory_order_relaxed);
    if (s_.tune_bw.load(std::memory_order_relaxed) == 20 && kLadder[r].bw_mhz == 40) r = 3;
    return r;
  }

  // Records the frame under its report tag before it is handed to the chip
  // (Adapter::send's hook runs under the TX mutex just before send_packet).
  bool send(const devourer::TxMode& m, const Mac& da, const P2pHeader& h, const uint8_t* p, size_t n,
            TagSlot::Kind kind, uint32_t seq) {
    std::optional<uint8_t> tag;
    const Adapter::TagHook hook = [&](std::optional<uint8_t> t) {
      tag = t;
      if (!t) return;
      std::lock_guard<std::mutex> lk(s_.probe_mu);
      s_.tags[*t] = TagSlot{kind, seq, now_ms()};
    };
    bool ok = s_.ad->send(m, da, s_.own, h, p, n, &hook);
    s_.frames_sent.fetch_add(1, std::memory_order_relaxed);
    if (!ok) {
      s_.frames_failed.fetch_add(1, std::memory_order_relaxed);
      if (tag) {
        // Never aired: no report will come for it.
        std::lock_guard<std::mutex> lk(s_.probe_mu);
        s_.tags[*tag].kind = TagSlot::None;
      }
    }
    if (kind == TagSlot::Probe && !tag && s_.tags_available.exchange(false))
      s_.lg->info("this device echoes no TX-report tag: every non-broadcast tx.report is read as a probe result");
    return ok;
  }

  void send_probe(uint8_t rung) {
    Probe pr;
    pr.seq = s_.probe_seq.fetch_add(1, std::memory_order_relaxed);
    pr.t_ms = uint32_t(now_ms());
    uint8_t buf[kProbeLen];
    encode_probe(pr, buf);
    P2pHeader h;
    h.type = uint8_t(Type::Probe);
    h.net_id = s_.net_id;
    h.rung = rung;
    h.seq = pr.seq;
    send(rung_mode(kLadder[rung]), s_.peer, h, buf, sizeof buf, TagSlot::Probe, pr.seq);
    TxItem it{TxClass::Control, 0, 0, std::vector<uint8_t>(buf, buf + sizeof buf), now_ms(), 0};
    std::lock_guard<std::mutex> lk(s_.sched_mu);
    s_.sched.on_sent(it, kLadder[rung], now_ms());
  }

  void send_video(const TxItem& item, uint8_t rung) {
    const Rung& r = kLadder[rung];
    const size_t S = frame_payload_capacity(r);
    // The scheduler item is the ring blob [8 B VencFrameMeta][frame]; the block
    // prefix is [u32 frame_len][meta][frame], so the frame is the blob minus
    // its meta and the prefix adds 4 bytes to the blob.
    const size_t frame_len = item.payload.size() > shm::kMetaSize ? item.payload.size() - shm::kMetaSize : 0;
    const size_t total = kBlockPrefixLen + frame_len;
    size_t k = (total + S - 1) / S;
    if (k == 0) k = 1;
    if (k > kMaxK) {
      // Dropped whole, counted, never truncated or split across blocks.
      s_.oversize_drops.fetch_add(1, std::memory_order_relaxed);
      if (!warned_oversize_) {
        warned_oversize_ = true;
        s_.lg->warn("video frame of {} B exceeds the block limit k={} x {} B = {} B; dropped (further drops counted only)",
                    item.payload.size(), unsigned(kMaxK), S, kMaxK * S);
      }
      return;
    }
    const size_t repair = std::max<size_t>(1, (k * (r.fec_n - r.fec_k) + r.fec_k - 1) / r.fec_k);
    const uint8_t kk = uint8_t(k), nn = uint8_t(k + repair);

    block_.assign(nn * S, 0);
    put_le32(block_.data(), uint32_t(frame_len));
    std::memcpy(block_.data() + 4, item.payload.data(), std::min<size_t>(shm::kMetaSize, item.payload.size()));
    if (frame_len) std::memcpy(block_.data() + kBlockPrefixLen, item.payload.data() + shm::kMetaSize, frame_len);
    std::vector<const uint8_t*> src(kk);
    std::vector<uint8_t*> rep(repair);
    for (size_t i = 0; i < kk; i++) src[i] = block_.data() + i * S;
    for (size_t i = 0; i < repair; i++) rep[i] = block_.data() + (kk + i) * S;
    codec(kk, nn).encode(src.data(), rep.data(), S);

    const uint16_t bid = s_.block_id.fetch_add(1, std::memory_order_relaxed);
    const devourer::TxMode m = rung_mode(r);
    for (uint8_t idx = 0; idx < nn; idx++) {
      P2pHeader h;
      h.type = uint8_t(idx < kk ? Type::VideoData : Type::VideoRepair);
      h.flags = uint8_t((item.flags & (kFlagEnhance | kFlagIdr)) | (idx + 1 == nn ? kFlagLastInBlock : 0));
      h.net_id = s_.net_id;
      h.rung = rung;
      h.fec_k = kk;
      h.fec_n = nn;
      h.fec_idx = idx;
      h.stream = item.stream;
      h.seq = s_.sym_seq.fetch_add(1, std::memory_order_relaxed);
      h.block_id = bid;
      send(m, s_.group, h, block_.data() + idx * S, S, TagSlot::Other, h.seq);
      if (s_.stop.load(std::memory_order_relaxed)) return;
    }
    s_.video_blocks.fetch_add(1, std::memory_order_relaxed);
  }

  void send_ip(const TxItem& item, uint8_t rung) {
    const bool ucast = item.stream == 0 ? s_.ucast_ctl.load() : item.stream == 1 ? s_.ucast_inter.load()
                                                                                 : s_.ucast_bulk.load();
    P2pHeader h;
    h.type = uint8_t(Type::IpData);
    h.flags = kFlagPacked;
    h.net_id = s_.net_id;
    h.rung = rung;
    h.stream = item.stream;
    h.seq = s_.ip_seq.fetch_add(1, std::memory_order_relaxed);
    send(rung_mode(kLadder[rung]), ucast ? s_.peer : s_.group, h, item.payload.data(), item.payload.size(),
         TagSlot::Other, h.seq);
  }

  void run() {
    uint64_t next_probe = now_ms();
    while (!s_.stop.load(std::memory_order_relaxed)) {
      const uint8_t rung = air_rung();
      const uint64_t now = now_ms();
      if (now >= next_probe) {
        next_probe = now + uint64_t(std::max(10, s_.probe_ms.load()));
        send_probe(rung);
        continue;
      }
      std::optional<TxItem> item;
      {
        std::unique_lock<std::mutex> lk(s_.sched_mu);
        item = s_.sched.dequeue(kLadder[rung], now);
        if (!item) {
          // Nothing eligible: sleep to the probe or a notification. A queued
          // but bucket-starved interactive item is re-checked in 5 ms.
          const bool anything = s_.sched.q_control() || s_.sched.q_video() || s_.sched.q_interactive() ||
                                s_.sched.q_bulk();
          uint64_t until = anything ? std::min(next_probe, now + 5) : next_probe;
          s_.sched_cv.wait_for(lk, std::chrono::milliseconds(until > now ? until - now : 1));
          continue;
        }
      }
      switch (item->cls) {
        case TxClass::Video: send_video(*item, rung); break;
        case TxClass::IpInteractive:
        case TxClass::IpBulk:
        case TxClass::Control: send_ip(*item, rung); break;
      }
      std::lock_guard<std::mutex> lk(s_.sched_mu);
      s_.sched.on_sent(*item, kLadder[rung], now_ms());
    }
  }

  Shared& s_;
  std::thread th_;
  std::map<uint16_t, RsCodec> codecs_;
  std::vector<uint8_t> block_;
  bool warned_oversize_ = false;
};

// Consecutive tun packets of one class are packed into one IpData item up to
// the rung's payload capacity, flushed by the pack timer.
struct Packer {
  std::vector<uint8_t> buf;
  uint64_t first_ms = 0;
  size_t pkts = 0;
};

int run(const Opts& o) {
  auto logger = std::make_shared<Logger>();
  logger->set_level(o.log_level);
  devourer::EventSink& ev = logger->events();

  AdapterSpec spec;
  if (!parse_adapter_spec(o.adapter, &spec)) {
    logger->error("bad --adapter '{}' (want VID:PID or VID:PID@BUS-PORT)", o.adapter);
    return 2;
  }
  Shared sh;
  sh.lg = logger;
  if (!parse_mac(o.peer_mac, &sh.peer) || is_group(sh.peer)) {
    logger->error("--peer-mac must be a unicast MAC (the ground's ack_target_mac)");
    return 2;
  }
  sh.net_id = uint8_t(o.net_id);
  sh.group = group_da(sh.net_id);
  sh.probe_ms = o.probe_ms;
  sh.tune_bw = uint8_t(o.bw);

  RadioCfg rc;
  rc.channel = uint8_t(o.channel);
  rc.bw_mhz = uint8_t(o.bw);
  rc.ht40_plus = o.ht40_plus;
  rc.duplex = true;
  rc.tx_report = true;
  rc.ack_responder = false;
  rc.mt7612u_fw_dir = o.mt7612u_fw;
  rc.wide_igi_floor = o.wide_igi_floor;
  rc.txpwr_offset_qdb = o.txpwr_offset_qdb;
  rc.log_level = o.log_level;

  std::string err;
  auto ad = Adapter::open(spec, rc, 0, sh.net_id, logger, &err);
  if (!ad) {
    logger->error("adapter open failed: {}", err);
    return 1;
  }
  sh.ad = ad.get();

  // Report sink: runs on the C2H thread — copy and leave. Set before bring-up
  // so the very first probe's report is not missed.
  ad->set_tx_report_sink([&sh](const devourer::TxReport& r) {
    if (!r.valid) return;
    Report rp;
    rp.has_tag = true;  // the HalMAC format carries sw_define; a 0 tag is a real tag
    rp.tag = r.sw_define;
    rp.state = r.state;
    rp.retries = r.data_retries;
    rp.bmc = r.bmc;
    std::lock_guard<std::mutex> lk(sh.rpt_mu);
    if (sh.rpts.size() < 4096) sh.rpts.push_back(rp);
  });

  if (!ad->bring_up(&err)) {
    logger->error("bring-up failed: {}", err);
    ad->stop();
    return 1;
  }
  sh.own = ad->mac();

  // Optional inputs.
  std::unique_ptr<shm::FrameRing> ring;
  bool ring_warned = false;
  uint64_t ring_retry_ms = 0;
  std::vector<uint8_t> ring_buf;
  Tun tun;
  bool have_tun = false;
  if (!o.tun.empty()) {
    if (!tun.open(o.tun, kTunMtu, &err)) {
      logger->error("tun: {}", err);
    } else {
      have_tun = true;
      if (!o.tun_addr.empty() && !tun.set_ipv4(o.tun_addr, &err)) logger->warn("tun addr: {}", err);
      if (!tun.set_up(true, &err)) logger->warn("tun up: {}", err);
      logger->info("tun {} up, mtu {}", tun.name(), kTunMtu);
    }
  }
  StatsPublisher pub;
  if (!pub.open(o.stats_shm, o.ndjson, &err)) logger->warn("stats shm: {}", err);
  Actuator actuator(o.bitrate_cmd, logger);

  LinkCfg lc;
  lc.start_rung = uint8_t(std::clamp(o.fixed_rung >= 0 ? o.fixed_rung : o.start_rung, 0, int(kTopRung)));
  const uint64_t t0 = now_ms();
  LinkController ctl(lc, t0);
  DaMachine da;
  const bool fixed = o.fixed_rung >= 0;
  const uint8_t fixed_rung = uint8_t(std::clamp(o.fixed_rung, 0, int(kTopRung)));
  sh.rung_phy = fixed ? fixed_rung : ctl.rung_phy();

  TxThread tx(sh);
  tx.start();

  devourer::Ev(ev, "p2p.ready")
      .t()
      .f("mac", mac_str(sh.own).c_str())
      .f("peer", mac_str(sh.peer).c_str())
      .f("chan", o.channel)
      .f("bw", o.bw)
      .f("chip", ad->chip_name())
      .f("net_id", o.net_id)
      .f("fixed_rung", o.fixed_rung);
  logger->info("p2p_tx up: {} {} ch {} bw {} peer {}", ad->chip_name(), mac_str(sh.own), o.channel, o.bw,
               mac_str(sh.peer));

  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  itimerspec its{};
  its.it_interval.tv_nsec = long(kTickMs) * 1000000L;
  its.it_value.tv_nsec = its.it_interval.tv_nsec;
  timerfd_settime(tfd, 0, &its, nullptr);

  Packer packers[3];
  SeqDedup dedup;
  uint64_t last_ctl_ms = t0, last_stats_ms = t0;
  uint32_t last_budget_kbps = 0;
  uint8_t last_phy = 255, last_budget = 255, last_state = 255, last_da = 255;
  LinkReport last_report;
  bool have_report = false;
  uint64_t last_probe_rx_ms = 0;
  uint64_t retries_sum = 0, retries_n = 0, probes_ok = 0, probes_pinned = 0, reports_total = 0;
  uint32_t stats_seq = 0;
  uint64_t tx_failed_prev = 0, tx_timeouts = 0;
  uint64_t ip_rx_pkts = 0, ip_tx_pkts = 0;

  auto enqueue = [&](TxItem&& it) {
    std::lock_guard<std::mutex> lk(sh.sched_mu);
    bool kept = sh.sched.enqueue(std::move(it), kLadder[sh.rung_phy.load()], now_ms());
    sh.sched_cv.notify_one();
    return kept;
  };
  auto flush_packer = [&](uint8_t cls) {
    Packer& pk = packers[cls];
    if (pk.buf.empty()) return;
    TxItem it{cls == 0 ? TxClass::Control : cls == 1 ? TxClass::IpInteractive : TxClass::IpBulk, cls, kFlagPacked,
              std::move(pk.buf), now_ms(), 0};
    pk.buf.clear();
    pk.pkts = 0;
    enqueue(std::move(it));
  };

  while (!g_stop) {
    const uint64_t now = now_ms();
    // Poll timeout: the pack timer if anything is pending, else the tick.
    int timeout = int(kTickMs);
    for (auto& pk : packers)
      if (!pk.buf.empty()) {
        uint64_t due = pk.first_ms + kPackTimerMs;
        timeout = std::min<int>(timeout, due > now ? int(due - now) : 0);
      }
    pollfd pfd[4];
    int n = 0;
    pfd[n++] = {ad->rx_fd(), POLLIN, 0};
    pfd[n++] = {tfd, POLLIN, 0};
    int ring_idx = -1, tun_idx = -1;
    if (ring) { ring_idx = n; pfd[n++] = {ring->event_fd(), POLLIN, 0}; }
    if (have_tun) { tun_idx = n; pfd[n++] = {tun.fd(), POLLIN, 0}; }
    int pr = poll(pfd, nfds_t(n), timeout);
    if (pr < 0 && errno != EINTR) {
      logger->error("poll: {}", std::strerror(errno));
      break;
    }
    const uint64_t t = now_ms();
    uint64_t scratch;
    if (pfd[0].revents & POLLIN) while (read(ad->rx_fd(), &scratch, sizeof scratch) > 0) {}
    if (pfd[1].revents & POLLIN) while (read(tfd, &scratch, sizeof scratch) > 0) {}

    // --- encoder ring: up to 4 frames per wake -----------------------------
    if (ring) {
      if (ring_idx >= 0 && (pfd[ring_idx].revents & POLLIN)) ring->drain_event();
      if (ring_buf.size() < ring->slot_data_size()) ring_buf.resize(ring->slot_data_size());
      for (int i = 0; i < 4; i++) {
        long len = ring->read_frame(ring_buf.data(), ring_buf.size());
        if (len <= 0) break;
        shm::FrameMeta meta;
        uint8_t flags = 0;
        if (shm::read_meta(ring_buf.data(), size_t(len), &meta)) {
          if (meta.flags & shm::kFlagEnhance) flags |= kFlagEnhance;
          if (meta.flags & shm::kFlagIdr) flags |= kFlagIdr;
        }
        TxItem it{TxClass::Video, 0, flags, std::vector<uint8_t>(ring_buf.begin(), ring_buf.begin() + len), t, 0};
        enqueue(std::move(it));
      }
      if (!ring->producer_alive() && t - ring_retry_ms > 2000) {
        ring_retry_ms = t;
        logger->warn("encoder ring producer gone; waiting for it to come back");
      }
    } else if (!o.ring.empty() && t >= ring_retry_ms) {
      ring_retry_ms = t + 1000;
      ring = shm::FrameRing::attach(o.ring, &err);
      if (!ring && !ring_warned) {
        ring_warned = true;
        logger->warn("encoder ring {}: {} (retrying every second)", o.ring, err);
      } else if (ring) {
        logger->info("encoder ring {} attached, slot {} B", o.ring, ring->slot_data_size());
      }
    }

    // --- tun -> IpData ------------------------------------------------------
    if (have_tun && tun_idx >= 0 && (pfd[tun_idx].revents & POLLIN)) {
      uint8_t pkt[2048];
      for (int i = 0; i < 32; i++) {
        long len = tun.read_packet(pkt, sizeof pkt);
        if (len <= 0) break;
        if (size_t(len) + 2 > frame_payload_capacity(kLadder[0])) continue;  // over the tun MTU: cannot fit one frame
        uint8_t cls = classify_ip(pkt, size_t(len));
        Packer& pk = packers[cls];
        const size_t cap = frame_payload_capacity(kLadder[sh.rung_phy.load()]);
        if (!pk.buf.empty() && pk.buf.size() + 2 + size_t(len) > cap) flush_packer(cls);
        if (pk.buf.empty()) pk.first_ms = t;
        uint8_t lp[2];
        put_le16(lp, uint16_t(len));
        pk.buf.insert(pk.buf.end(), lp, lp + 2);
        pk.buf.insert(pk.buf.end(), pkt, pkt + len);
        pk.pkts++;
        ip_tx_pkts++;
        if (cls == 0) flush_packer(cls);  // control class never waits for the pack timer
      }
    }
    for (uint8_t c = 0; c < 3; c++)
      if (!packers[c].buf.empty() && t >= packers[c].first_ms + kPackTimerMs) flush_packer(c);

    // --- adapter RX: reports and IP from the ground --------------------------
    RxFrame f;
    while (ad->pop_rx(&f, 0)) {
      switch (Type(f.hdr.type)) {
        case Type::LinkReport: {
          auto r = decode_report(f.payload.data(), f.payload.size());
          if (!r) break;
          last_report = *r;
          have_report = true;
          reports_total++;
          // Fed in --fixed-rung mode too: the rungs are pinned below, but the
          // report age, TRACK state and therefore the DA machine need them
          // (first light: without this a pinned drone sat in FLOOR with
          // report_age MAX and could never reach Ucast).
          ctl.on_report(*r, t);
          break;
        }
        case Type::IpData:
          if (!dedup.accept(f.hdr.type, f.hdr.stream, f.hdr.seq)) break;
          if (have_tun)
            for_each_packed(f.payload.data(), f.payload.size(), [&](const uint8_t* p, size_t l) {
              if (tun.write_packet(p, l)) ip_rx_pkts++;
            });
          break;
        case Type::Probe: last_probe_rx_ms = t; break;
        default: break;
      }
    }

    // --- TX reports -> probe results -----------------------------------------
    {
      std::vector<Report> rpts;
      {
        std::lock_guard<std::mutex> lk(sh.rpt_mu);
        rpts.swap(sh.rpts);
      }
      for (const Report& r : rpts) {
        if (r.bmc) continue;
        retries_sum += r.retries;
        retries_n++;
        bool is_probe = false;
        if (sh.tags_available.load()) {
          std::lock_guard<std::mutex> lk(sh.probe_mu);
          TagSlot& slot = sh.tags[r.tag];
          is_probe = slot.kind == TagSlot::Probe;
          slot.kind = TagSlot::None;
        } else {
          is_probe = true;  // no tag echo on this die: every non-bmc report is probe-class
        }
        if (!is_probe) continue;
        const bool ok = r.state == 0;
        ok ? probes_ok++ : probes_pinned++;
        // Fed in --fixed-rung mode too: the ack8 window still drives the DA
        // machine and the stats even when the rungs are pinned.
        ctl.on_probe_result(ok, t);
        da.on_probe_result(ok, t);
      }
    }

    // --- control tick (100 ms) ------------------------------------------------
    if (t - last_ctl_ms >= kControlTickMs) {
      last_ctl_ms = t;
      devourer::TxStats ts = ad->tx_stats();
      if (ts.failed > tx_failed_prev && ts.last_was_timeout) tx_timeouts += ts.failed - tx_failed_prev;
      tx_failed_prev = ts.failed;
      uint32_t q_air;
      {
        std::lock_guard<std::mutex> lk(sh.sched_mu);
        q_air = sh.sched.q_video_air_ms(kLadder[sh.rung_phy.load()]);
      }
      if (!fixed) {
        ctl.tick(t);
        ctl.on_local(q_air, ts.last_was_timeout, t);
      } else {
        ctl.tick(t);  // keeps the probe/report ageing honest for stats; rungs are pinned below
      }
      da.update(ctl.ack8(), ctl.ack8_known(), ctl.state(), t);
      const uint8_t phy = fixed ? fixed_rung : ctl.rung_phy();
      const uint8_t budget = fixed ? fixed_rung : ctl.rung_budget();
      sh.rung_phy.store(phy);
      sh.ucast_ctl = da.allow_unicast(0, ctl.ack8());
      sh.ucast_inter = da.allow_unicast(1, ctl.ack8());
      sh.ucast_bulk = da.allow_unicast(2, ctl.ack8());

      // Drone tune change: 20 MHz after tune20_after_ms at rung <= 3 (drops
      // the wide-TX IGI floor), back to the configured width once the PHY
      // rung is a 40 MHz one. Main loop only; Adapter::retune serialises with
      // the TX thread on the TX mutex.
      if (!o.no_tune20 && o.bw == 40) {
        const uint8_t tb = sh.tune_bw.load();
        bool want20 = fixed ? kLadder[fixed_rung].bw_mhz == 20 : ctl.wants_tune_20mhz(t);
        if (tb == 40 && want20) {
          if (ad->retune(uint8_t(o.channel), 20, o.ht40_plus, &err)) {
            sh.tune_bw = 20;
            devourer::Ev(ev, "p2p.tune").t().f("bw", 20);
          } else {
            logger->warn("retune to 20 MHz failed: {}", err);
          }
        } else if (tb == 20 && kLadder[phy].bw_mhz == 40) {
          if (ad->retune(uint8_t(o.channel), 40, o.ht40_plus, &err)) {
            sh.tune_bw = 40;
            devourer::Ev(ev, "p2p.tune").t().f("bw", 40);
          } else {
            logger->warn("retune to 40 MHz failed: {}", err);
          }
        }
      }

      const uint32_t budget_kbps = video_budget_kbps(kLadder[budget], o.control_reserve_kbps);
      if (phy != last_phy || budget != last_budget || uint8_t(ctl.state()) != last_state) {
        last_phy = phy;
        last_budget = budget;
        last_state = uint8_t(ctl.state());
        devourer::Ev(ev, "p2p.rung")
            .t()
            .f("phy", phy)
            .f("budget", budget)
            .f("state", ctl.state_name())
            .f("ack8", ctl.ack8())
            .f("budget_kbps", budget_kbps);
      }
      if (uint8_t(da.mode()) != last_da) {
        last_da = uint8_t(da.mode());
        devourer::Ev(ev, "p2p.da").t().f("mode", da.mode() == DaMachine::Mode::Ucast ? "ucast" : "bcast");
      }
      if (budget_kbps != last_budget_kbps) {
        last_budget_kbps = budget_kbps;
        actuator.target(budget_kbps);
      }

      // Stats.
      if (t - last_stats_ms >= 100) {
        last_stats_ms = t;
        StatsSnapshot s;
        s.seq = ++stats_seq;
        s.t_ms = t;
        s.channel = uint8_t(o.channel);
        s.bw_mhz = sh.tune_bw.load();
        s.rung = phy;
        s.link_state = uint8_t(ctl.state());
        s.da_mode = uint8_t(da.mode());
        s.ack8 = ctl.ack8();
        s.adapters = have_report ? last_report.adapters : 0;
        const Rung& br = kLadder[budget];
        s.est_capacity_kbps = uint32_t(uint64_t(br.phy_kbps) * br.duty_permille / 1000);
        s.video_budget_kbps = budget_kbps;
        s.ip_budget_kbps = uint32_t(uint64_t(br.phy_kbps) * 150 / 1000);
        {
          std::lock_guard<std::mutex> lk(sh.sched_mu);
          s.tx_duty_permille = sh.sched.tx_duty_permille(t);
          s.q_video_ms = sh.sched.q_video_air_ms(kLadder[phy]);
          s.q_ip_ms = sh.sched.q_ip_air_ms(kLadder[phy]);
          s.q_ctl_frames = uint32_t(sh.sched.q_control());
          auto d = sh.sched.drops();
          s.dropped_video_frames = d.video_frames + uint32_t(sh.oversize_drops.load());
          s.dropped_ip_pkts = d.ip_pkts;
        }
        s.report_age_ms = ctl.report_age_ms(t);
        s.probe_age_ms = last_probe_rx_ms ? uint32_t(t - last_probe_rx_ms) : UINT32_MAX;
        s.retries_mean_centi = retries_n ? uint32_t(retries_sum * 100 / retries_n) : 0;
        s.rssi_best = have_report ? last_report.rssi_best : int16_t(-127);
        s.rssi_mean = have_report ? last_report.rssi_mean : int16_t(-127);
        s.loss_permille = have_report ? last_report.loss_permille : 0;
        s.fec_fail_permille = have_report ? last_report.fec_fail_permille : 0;
        s.fec_k = br.fec_k;
        s.fec_n = br.fec_n;
        s.frame_bytes_hi = uint8_t(br.frame_bytes >> 8);
        s.frame_bytes_lo = uint8_t(br.frame_bytes & 0xff);
        s.uniq = have_report ? last_report.uniq : 0;
        s.diversity = have_report ? last_report.diversity : 0;
        s.tx_submitted = uint32_t(ts.submitted);
        s.tx_failed = uint32_t(ts.failed);
        s.tx_timeouts = uint32_t(tx_timeouts);
        pub.publish(s);
      }
      std::string why;
      if (ad->rx_failed(&why)) {
        logger->error("adapter RX worker died: {} — exiting", why);
        break;
      }
    }
  }

  // Shutdown: TX thread first (it owns send), then the adapter, then the rest.
  sh.stop = true;
  sh.sched_cv.notify_all();
  tx.join();
  actuator.stop();
  Adapter::Counters c = ad->counters();
  ad->stop();
  close(tfd);
  devourer::Ev(ev, "p2p.exit")
      .t()
      .f("frames_sent", (unsigned long long)sh.frames_sent.load())
      .f("frames_failed", (unsigned long long)sh.frames_failed.load())
      .f("video_blocks", (unsigned long long)sh.video_blocks.load())
      .f("oversize_drops", (unsigned long long)sh.oversize_drops.load())
      .f("probes_ok", (unsigned long long)probes_ok)
      .f("probes_pinned", (unsigned long long)probes_pinned)
      .f("reports", (unsigned long long)reports_total)
      .f("ip_tx", (unsigned long long)ip_tx_pkts)
      .f("ip_rx", (unsigned long long)ip_rx_pkts)
      .f("rx_frames", (unsigned long long)c.rx_frames)
      .f("rx_crc", (unsigned long long)c.rx_crc)
      .f("rx_foreign", (unsigned long long)c.rx_foreign)
      .f("rx_dropped_q", (unsigned long long)c.rx_dropped_q)
      .f("tx_timeouts", (unsigned long long)tx_timeouts);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  enum { kChannel = 1000, kBw, kOffset, kNetId, kPeer, kRing, kTun, kTunAddr, kBitrate, kReserve, kStart, kFixed,
         kNoTune20, kProbeMs, kIgi, kTxPwr, kMtFw, kStatsShm, kNoNdjson, kLog, kAdapter, kHelp };
  static const option longopts[] = {
      {"adapter", required_argument, nullptr, kAdapter},   {"channel", required_argument, nullptr, kChannel},
      {"bw", required_argument, nullptr, kBw},             {"offset", required_argument, nullptr, kOffset},
      {"net-id", required_argument, nullptr, kNetId},      {"peer-mac", required_argument, nullptr, kPeer},
      {"ring", required_argument, nullptr, kRing},         {"tun", required_argument, nullptr, kTun},
      {"tun-addr", required_argument, nullptr, kTunAddr},  {"bitrate-cmd", required_argument, nullptr, kBitrate},
      {"control-reserve-kbps", required_argument, nullptr, kReserve},
      {"start-rung", required_argument, nullptr, kStart},  {"fixed-rung", required_argument, nullptr, kFixed},
      {"no-tune20", no_argument, nullptr, kNoTune20},      {"probe-ms", required_argument, nullptr, kProbeMs},
      {"wide-igi-floor", required_argument, nullptr, kIgi},
      {"txpwr-offset-qdb", required_argument, nullptr, kTxPwr}, {"mt7612u-fw", required_argument, nullptr, kMtFw},
      {"stats-shm", required_argument, nullptr, kStatsShm}, {"no-ndjson", no_argument, nullptr, kNoNdjson},
      {"log-level", required_argument, nullptr, kLog},     {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
    switch (c) {
      case kAdapter: o.adapter = optarg; break;
      case kChannel: o.channel = std::atoi(optarg); break;
      case kBw: o.bw = std::atoi(optarg); break;
      case kOffset: o.ht40_plus = std::string(optarg) != "-"; break;
      case kNetId: o.net_id = std::atoi(optarg); break;
      case kPeer: o.peer_mac = optarg; break;
      case kRing: o.ring = optarg; break;
      case kTun: o.tun = optarg; break;
      case kTunAddr: o.tun_addr = optarg; break;
      case kBitrate: o.bitrate_cmd = optarg; break;
      case kReserve: o.control_reserve_kbps = uint32_t(std::atoi(optarg)); break;
      case kStart: o.start_rung = std::atoi(optarg); break;
      case kFixed: o.fixed_rung = std::atoi(optarg); break;
      case kNoTune20: o.no_tune20 = true; break;
      case kProbeMs: o.probe_ms = std::atoi(optarg); break;
      case kIgi: o.wide_igi_floor = uint8_t(std::strtoul(optarg, nullptr, 0)); break;
      case kTxPwr: o.txpwr_offset_qdb = std::atoi(optarg); break;
      case kMtFw: o.mt7612u_fw = std::string(optarg); break;
      case kStatsShm: o.stats_shm = optarg; break;
      case kNoNdjson: o.ndjson = false; break;
      case kLog:
        if (!parse_level(optarg, &o.log_level)) { usage(argv[0]); return 2; }
        break;
      case kHelp: case 'h': usage(argv[0]); return 0;
      default: usage(argv[0]); return 2;
    }
  }
  if (o.adapter.empty() || o.peer_mac.empty()) {
    usage(argv[0]);
    return 2;
  }
  if ((o.bw != 20 && o.bw != 40) || o.channel < 1 || o.channel > 253 || o.net_id < 0 || o.net_id > 255 ||
      o.start_rung < 0 || o.start_rung > int(kTopRung) || o.fixed_rung > int(kTopRung) || o.probe_ms < 10) {
    std::fprintf(stderr, "bad option value\n");
    usage(argv[0]);
    return 2;
  }
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  signal(SIGPIPE, SIG_IGN);
  return run(o);
}
