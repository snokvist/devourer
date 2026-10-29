// SPDX-License-Identifier: GPL-2.0-or-later
// p2p_rx — the ground end of the p2plink (docs/DESIGN.md "Roles": Ground).
//
// 1..N adapters ("ears"), each on its own libusb context. Slots:
//   --ack-target IDX  the ear whose MAC the drone's probes address. Realtek:
//                     SetAckResponder(own MAC) after bring-up. MT7612U: its
//                     MAC auto-ACKs its factory address from bring-up, so
//                     nothing is armed and that is what is logged.
//   --uplink IDX      the ear that transmits (duplex): LinkReports at
//                     --report-hz and IpData from the ground tun, both at
//                     20 MHz / LDPC / group DA — decodable at the drone's
//                     floor rung. `none` = pure spectator.
// Bring-up is SEQUENTIAL: an MT7612U does not reliably come up on a saturated
// channel (src/mt7612u/CLAUDE.md), so start the ground before the drone and
// let each ear have the air to itself while it comes up.
#include <arpa/inet.h>
#include <getopt.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Event.h"
#include "p2p/budget.h"
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

constexpr int kTunMtu = int(1400 - kHdrLen - 2);
constexpr uint32_t kTickMs = 10;
constexpr uint32_t kStatsMs = 100;
constexpr size_t kUdpChunk = 1400;

struct Opts {
  std::vector<std::string> adapters;
  int ack_target = 0;
  int uplink = 0;  // -1 = none
  int channel = 36;
  int bw = 40;
  bool ht40_plus = true;
  int net_id = 1;
  std::string out_udp, out_shm;
  std::string tun, tun_addr;
  int report_hz = 10;
  int report_mcs = 0;
  std::optional<int> txpwr_offset_qdb;
  std::optional<std::string> mt7612u_fw;
  std::string stats_shm = "/p2plink.rx";
  bool ndjson = true;
  // Default warn: devourer logs every successful bulk-OUT at info (one stderr
  // line + flush per frame), a measurable CPU cost at video frame rates.
  // Everything p2plink itself reports at info is also in the NDJSON events.
  Logger::Level log_level = Logger::Level::Warn;
};

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s --adapter SPEC [--adapter SPEC ...] [options]\n"
      "  --adapter VID:PID[@BUS-PORT]  one ear per flag; ear index = order given\n"
      "  --ack-target IDX        ear whose MAC the drone probes (default 0)\n"
      "  --uplink IDX|none       ear that sends reports + IP (default 0); none = spectator\n"
      "  --channel N --bw 20|40 --offset +|-   (defaults 36, 40, +)\n"
      "  --net-id N              (default 1)\n"
      "  --out-udp HOST:PORT     decoded frames as a byte stream, <= 1400 B datagrams\n"
      "  --out-shm NAME          decoded frames into a frame ring ([8 B meta][frame])\n"
      "  --tun NAME --tun-addr CIDR   IP tunnel (ground IP is group-addressed in v1)\n"
      "  --report-hz N           LinkReport rate (default 10)\n"
      "  --report-mcs N          report HT MCS at 20 MHz LDPC (default 0)\n"
      "  --txpwr-offset-qdb N    uplink TX power offset, quarter dB (-40 = -10 dB)\n"
      "  --mt7612u-fw DIR        MT7612U firmware directory\n"
      "  --stats-shm NAME        (default /p2plink.rx)\n"
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

std::string mac_str(const Mac& m) {
  char b[18];
  std::snprintf(b, sizeof b, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
  return b;
}

class UdpOut {
 public:
  bool open(const std::string& hostport, std::string* err) {
    auto colon = hostport.rfind(':');
    if (colon == std::string::npos) { *err = "want HOST:PORT"; return false; }
    std::string host = hostport.substr(0, colon), port = hostport.substr(colon + 1);
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) { *err = gai_strerror(rc); return false; }
    fd_ = socket(res->ai_family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd_ < 0 || connect(fd_, res->ai_addr, res->ai_addrlen) != 0) {
      *err = std::strerror(errno);
      freeaddrinfo(res);
      return false;
    }
    freeaddrinfo(res);
    return true;
  }
  ~UdpOut() { if (fd_ >= 0) close(fd_); }
  void send_frame(const uint8_t* p, size_t n) {
    for (size_t off = 0; off < n; off += kUdpChunk) {
      size_t l = std::min(kUdpChunk, n - off);
      if (::send(fd_, p + off, l, MSG_DONTWAIT) < 0) drops_++;
    }
  }
  uint64_t drops() const { return drops_; }

 private:
  int fd_ = -1;
  uint64_t drops_ = 0;
};

int run(const Opts& o) {
  auto logger = std::make_shared<Logger>();
  logger->set_level(o.log_level);
  devourer::EventSink& ev = logger->events();
  const uint8_t net_id = uint8_t(o.net_id);
  std::string err;

  // Open every ear first (claim + reset + CreateRadio), then bring them up one
  // at a time.
  std::vector<std::unique_ptr<Adapter>> ears;
  for (size_t i = 0; i < o.adapters.size(); i++) {
    AdapterSpec spec;
    if (!parse_adapter_spec(o.adapters[i], &spec)) {
      logger->error("bad --adapter '{}'", o.adapters[i]);
      return 2;
    }
    RadioCfg rc;
    rc.channel = uint8_t(o.channel);
    rc.bw_mhz = uint8_t(o.bw);
    rc.ht40_plus = o.ht40_plus;
    rc.duplex = int(i) == o.uplink;
    rc.tx_report = false;
    rc.ack_responder = int(i) == o.ack_target;
    rc.mt7612u_fw_dir = o.mt7612u_fw;
    if (rc.duplex) rc.txpwr_offset_qdb = o.txpwr_offset_qdb;
    rc.log_level = o.log_level;
    auto ad = Adapter::open(spec, rc, uint8_t(i), net_id, logger, &err);
    if (!ad) {
      logger->error("ear {}: {}", i, err);
      for (auto it = ears.rbegin(); it != ears.rend(); ++it) (*it)->stop();
      return 1;
    }
    ears.push_back(std::move(ad));
  }
  for (size_t i = 0; i < ears.size(); i++) {
    if (!ears[i]->bring_up(&err)) {
      logger->error("ear {} bring-up failed: {}", i, err);
      for (auto it = ears.rbegin(); it != ears.rend(); ++it) (*it)->stop();
      return 1;
    }
    logger->info("ear {} up: {} {}", i, ears[i]->chip_name(), mac_str(ears[i]->mac()));
  }
  Adapter* uplink = o.uplink >= 0 ? ears[size_t(o.uplink)].get() : nullptr;
  const Mac ack_mac = ears[size_t(o.ack_target)]->mac();
  const Mac group = group_da(net_id);
  // The probe DA must be exactly one ear: a second ear with the same MAC
  // would ACK too (DESIGN.md "Assert probe DA == ack_target permanent MAC and
  // DA ∉ other ears").
  for (size_t i = 0; i < ears.size(); i++)
    if (int(i) != o.ack_target && ears[i]->mac() == ack_mac)
      logger->warn("ear {} shares the ACK target's MAC {} — two responders on one address", i, mac_str(ack_mac));

  {
    devourer::Ev e(ev, "p2p.ready");
    e.t().f("ack_target_mac", mac_str(ack_mac).c_str()).f("chan", o.channel).f("bw", o.bw).f("net_id", o.net_id)
        .f("uplink", o.uplink).f("ears", (long long)ears.size());
    for (size_t i = 0; i < ears.size(); i++) {
      std::string k = "ear" + std::to_string(i);
      e.f(k.c_str(), (std::string(ears[i]->chip_name()) + " " + mac_str(ears[i]->mac())).c_str());
    }
  }
  logger->info("p2p_rx up on ch {} bw {}; drone --peer-mac {}", o.channel, o.bw, mac_str(ack_mac));

  // Outputs.
  std::unique_ptr<UdpOut> udp;
  if (!o.out_udp.empty()) {
    udp = std::make_unique<UdpOut>();
    if (!udp->open(o.out_udp, &err)) {
      logger->error("out-udp {}: {}", o.out_udp, err);
      udp.reset();
    }
  }
  std::unique_ptr<shm::FrameRing> out_ring;
  if (!o.out_shm.empty()) {
    out_ring = shm::FrameRing::create(o.out_shm, shm::kDefaultSlots, shm::kDefaultSlotSize, &err);
    if (!out_ring) logger->error("out-shm {}: {}", o.out_shm, err);
  }
  Tun tun;
  bool have_tun = false;
  if (!o.tun.empty()) {
    if (!tun.open(o.tun, kTunMtu, &err)) {
      logger->error("tun: {}", err);
    } else {
      have_tun = true;
      if (!o.tun_addr.empty() && !tun.set_ipv4(o.tun_addr, &err)) logger->warn("tun addr: {}", err);
      if (!tun.set_up(true, &err)) logger->warn("tun up: {}", err);
      if (!uplink) logger->warn("tun without an uplink: ground packets have nowhere to go");
    }
  }
  StatsPublisher pub;
  if (!pub.open(o.stats_shm, o.ndjson, &err)) logger->warn("stats shm: {}", err);

  int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  itimerspec its{};
  its.it_interval.tv_nsec = long(kTickMs) * 1000000L;
  its.it_value.tv_nsec = its.it_interval.tv_nsec;
  timerfd_settime(tfd, 0, &its, nullptr);

  MergeConfig mc;
  mc.max_ears = uint8_t(std::min<size_t>(ears.size(), BlockMerger::kMaxEars));
  BlockMerger merger(mc);
  SeqDedup dedup;
  devourer::TxMode report_mode;
  report_mode.mode = devourer::TxMode::Mode::HT;
  report_mode.ht_mcs = uint8_t(o.report_mcs);
  report_mode.bw_mhz = 20;
  report_mode.ldpc = true;

  const uint64_t report_period = uint64_t(std::max(1, 1000 / std::max(1, o.report_hz)));
  uint64_t t0 = now_ms(), last_report_ms = t0, last_stats_ms = t0, last_probe_ms = 0;
  uint32_t epoch = 0, stats_seq = 0;
  uint16_t ip_rx_window = 0;
  uint8_t rung_seen = 0;
  LinkReport last_rep;
  uint64_t blocks_ok = 0, blocks_fec = 0, blocks_failed = 0, frames_out_bytes = 0, probes = 0, ip_rx = 0,
           ip_tx = 0, tun_fail = 0, reports_sent = 0, reports_failed = 0, out_ring_drops = 0;
  uint32_t ip_seq = 0;
  uint8_t last_k = 0, last_n = 0;
  uint64_t tx_failed_prev = 0, tx_timeouts = 0;

  auto emit = [&](const DecodedBlock& b) {
    if (!b.ok) {
      blocks_failed++;
      return;
    }
    b.used_fec ? blocks_fec++ : blocks_ok++;
    frames_out_bytes += b.frame.size();
    if (udp) udp->send_frame(b.frame.data(), b.frame.size());
    if (out_ring) {
      std::vector<uint8_t> blob(shm::kMetaSize + b.frame.size());
      std::memcpy(blob.data(), b.meta.data(), shm::kMetaSize);
      std::memcpy(blob.data() + shm::kMetaSize, b.frame.data(), b.frame.size());
      if (!out_ring->write_frame(blob.data(), blob.size())) out_ring_drops++;
    }
  };

  std::vector<pollfd> pfd;
  while (!g_stop) {
    pfd.clear();
    for (auto& e : ears) pfd.push_back({e->rx_fd(), POLLIN, 0});
    pfd.push_back({tfd, POLLIN, 0});
    // The tun is read only when there is an uplink to send it on; polling an
    // fd that is never drained would spin this loop at 100 % CPU.
    const bool poll_tun = have_tun && uplink;
    if (poll_tun) pfd.push_back({tun.fd(), POLLIN, 0});
    int pr = poll(pfd.data(), nfds_t(pfd.size()), int(kTickMs));
    if (pr < 0 && errno != EINTR) {
      logger->error("poll: {}", std::strerror(errno));
      break;
    }
    const uint64_t t = now_ms();
    uint64_t scratch;
    for (size_t i = 0; i < ears.size(); i++)
      if (pfd[i].revents & POLLIN) while (read(ears[i]->rx_fd(), &scratch, sizeof scratch) > 0) {}
    if (pfd[ears.size()].revents & POLLIN) while (read(tfd, &scratch, sizeof scratch) > 0) {}

    // --- ears -----------------------------------------------------------------
    RxFrame f;
    for (auto& e : ears) {
      while (e->pop_rx(&f, 0)) {
        switch (Type(f.hdr.type)) {
          case Type::VideoData:
          case Type::VideoRepair:
            merger.on_symbol(f.ear, f.hdr, f.payload.data(), f.payload.size(), f.rssi_dbm, t);
            last_k = f.hdr.fec_k;
            last_n = f.hdr.fec_n;
            break;
          case Type::IpData:
            if (!dedup.accept(f.hdr.type, f.hdr.stream, f.hdr.seq)) break;
            for_each_packed(f.payload.data(), f.payload.size(), [&](const uint8_t* p, size_t l) {
              if (have_tun && tun.write_packet(p, l)) {
                ip_rx++;
                if (ip_rx_window < 0xffff) ip_rx_window++;
              } else {
                tun_fail++;
              }
            });
            break;
          case Type::Probe:
            if (!dedup.accept(f.hdr.type, f.hdr.stream, f.hdr.seq)) break;
            if (probes++ == 0)
              logger->info("first probe from {} at rung {} rssi {} dBm (ear {})", mac_str(f.sa), f.hdr.rung,
                           int(f.rssi_dbm), f.ear);
            last_probe_ms = t;
            break;
          default: break;
        }
      }
    }
    merger.poll(t, emit);

    // --- ground tun -> IpData, group DA, one packet per frame ------------------
    if (poll_tun && (pfd.back().revents & POLLIN)) {
      uint8_t pkt[2048];
      for (int i = 0; i < 32; i++) {
        long len = tun.read_packet(pkt, sizeof pkt);
        if (len <= 0) break;
        if (size_t(len) + 2 > frame_payload_capacity(kLadder[0])) continue;
        std::vector<uint8_t> payload(2 + size_t(len));
        put_le16(payload.data(), uint16_t(len));
        std::memcpy(payload.data() + 2, pkt, size_t(len));
        P2pHeader h;
        h.type = uint8_t(Type::IpData);
        h.flags = kFlagPacked;
        h.net_id = net_id;
        h.stream = classify_ip(pkt, size_t(len));
        h.seq = ip_seq++;
        if (uplink->send(report_mode, group, uplink->mac(), h, payload.data(), payload.size(), nullptr)) ip_tx++;
      }
    }

    // --- LinkReport -------------------------------------------------------------
    if (t - last_report_ms >= report_period) {
      last_report_ms = t;
      // cur_rung is the previous window's rung_seen: the merger computes this
      // window's inside make_report, after the rung slots are read.
      const uint8_t probe_rung = uint8_t(std::min<int>(rung_seen + 1, kTopRung));
      last_rep = merger.make_report(++epoch, t, probe_rung, rung_seen, ip_rx_window);
      rung_seen = last_rep.rung_seen;
      ip_rx_window = 0;
      if (uplink) {
        uint8_t buf[kLinkReportLen];
        encode_report(last_rep, buf);
        P2pHeader h;
        h.type = uint8_t(Type::LinkReport);
        h.net_id = net_id;
        h.rung = rung_seen;
        h.seq = epoch;
        if (uplink->send(report_mode, group, uplink->mac(), h, buf, sizeof buf, nullptr)) reports_sent++;
        else reports_failed++;
      }
    }

    // --- stats -------------------------------------------------------------------
    if (t - last_stats_ms >= kStatsMs) {
      last_stats_ms = t;
      StatsSnapshot s;
      s.seq = ++stats_seq;
      s.t_ms = t;
      s.channel = uint8_t(o.channel);
      s.bw_mhz = uint8_t(o.bw);
      s.rung = rung_seen;
      s.adapters = last_rep.adapters;
      s.report_age_ms = uint32_t(t - last_report_ms);
      s.probe_age_ms = last_probe_ms ? uint32_t(t - last_probe_ms) : UINT32_MAX;
      s.rssi_best = last_rep.rssi_best;
      s.rssi_mean = last_rep.rssi_mean;
      s.loss_permille = last_rep.loss_permille;
      s.fec_fail_permille = last_rep.fec_fail_permille;
      s.fec_k = last_k;
      s.fec_n = last_n;
      s.frame_bytes_hi = uint8_t(1400 >> 8);
      s.frame_bytes_lo = uint8_t(1400 & 0xff);
      s.uniq = uint32_t(merger.total_uniq());
      s.diversity = uint32_t(merger.total_diversity());
      s.dropped_video_frames = uint32_t(blocks_failed);
      s.dropped_ip_pkts = uint32_t(tun_fail);
      if (uplink) {
        auto ts = uplink->tx_stats();
        s.tx_submitted = uint32_t(ts.submitted);
        s.tx_failed = uint32_t(ts.failed);
        // Same accounting as the drone: failures that landed while the last
        // failure was a bulk-OUT timeout (chip FIFO full).
        if (ts.failed > tx_failed_prev && ts.last_was_timeout) tx_timeouts += ts.failed - tx_failed_prev;
        tx_failed_prev = ts.failed;
        s.tx_timeouts = uint32_t(tx_timeouts);
      }
      pub.publish(s);
      for (auto& e : ears) {
        std::string why;
        if (e->rx_failed(&why)) logger->error("ear {} RX worker died: {}", e->ear_index(), why);
      }
    }
  }

  // Ears stop in reverse order of bring-up.
  for (auto it = ears.rbegin(); it != ears.rend(); ++it) (*it)->stop();
  close(tfd);
  devourer::Ev e(ev, "p2p.exit");
  e.t().f("blocks_ok", (unsigned long long)blocks_ok).f("blocks_fec", (unsigned long long)blocks_fec)
      .f("blocks_failed", (unsigned long long)blocks_failed).f("frame_bytes", (unsigned long long)frames_out_bytes)
      .f("uniq", (unsigned long long)merger.total_uniq()).f("diversity", (unsigned long long)merger.total_diversity())
      .f("probes", (unsigned long long)probes).f("reports_sent", (unsigned long long)reports_sent)
      .f("reports_failed", (unsigned long long)reports_failed).f("ip_rx", (unsigned long long)ip_rx)
      .f("ip_tx", (unsigned long long)ip_tx).f("out_ring_drops", (unsigned long long)out_ring_drops)
      .f("udp_drops", (unsigned long long)(udp ? udp->drops() : 0));
  for (auto& ear : ears) {
    auto c = ear->counters();
    std::string k = "ear" + std::to_string(ear->ear_index());
    e.f((k + "_rx").c_str(), (unsigned long long)c.rx_frames).f((k + "_crc").c_str(), (unsigned long long)c.rx_crc)
        .f((k + "_foreign").c_str(), (unsigned long long)c.rx_foreign)
        .f((k + "_dropq").c_str(), (unsigned long long)c.rx_dropped_q);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Opts o;
  enum { kAdapter = 1000, kAck, kUplink, kChannel, kBw, kOffset, kNetId, kUdp, kShm, kTun, kTunAddr, kHz, kMcs,
         kTxPwr, kMtFw, kStatsShm, kNoNdjson, kLog, kHelp };
  static const option longopts[] = {
      {"adapter", required_argument, nullptr, kAdapter},  {"ack-target", required_argument, nullptr, kAck},
      {"uplink", required_argument, nullptr, kUplink},    {"channel", required_argument, nullptr, kChannel},
      {"bw", required_argument, nullptr, kBw},            {"offset", required_argument, nullptr, kOffset},
      {"net-id", required_argument, nullptr, kNetId},     {"out-udp", required_argument, nullptr, kUdp},
      {"out-shm", required_argument, nullptr, kShm},      {"tun", required_argument, nullptr, kTun},
      {"tun-addr", required_argument, nullptr, kTunAddr}, {"report-hz", required_argument, nullptr, kHz},
      {"report-mcs", required_argument, nullptr, kMcs},   {"mt7612u-fw", required_argument, nullptr, kMtFw},
      {"txpwr-offset-qdb", required_argument, nullptr, kTxPwr}, {"stats-shm", required_argument, nullptr, kStatsShm}, {"no-ndjson", no_argument, nullptr, kNoNdjson},
      {"log-level", required_argument, nullptr, kLog},    {"help", no_argument, nullptr, kHelp},
      {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
    switch (c) {
      case kAdapter: o.adapters.emplace_back(optarg); break;
      case kAck: o.ack_target = std::atoi(optarg); break;
      case kUplink: o.uplink = std::string(optarg) == "none" ? -1 : std::atoi(optarg); break;
      case kChannel: o.channel = std::atoi(optarg); break;
      case kBw: o.bw = std::atoi(optarg); break;
      case kOffset: o.ht40_plus = std::string(optarg) != "-"; break;
      case kNetId: o.net_id = std::atoi(optarg); break;
      case kUdp: o.out_udp = optarg; break;
      case kShm: o.out_shm = optarg; break;
      case kTun: o.tun = optarg; break;
      case kTunAddr: o.tun_addr = optarg; break;
      case kHz: o.report_hz = std::atoi(optarg); break;
      case kMcs: o.report_mcs = std::atoi(optarg); break;
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
  if (o.adapters.empty()) {
    usage(argv[0]);
    return 2;
  }
  const int n = int(o.adapters.size());
  if (n > int(BlockMerger::kMaxEars) || o.ack_target < 0 || o.ack_target >= n || o.uplink >= n || o.uplink < -1 ||
      (o.bw != 20 && o.bw != 40) || o.channel < 1 || o.channel > 253 || o.net_id < 0 || o.net_id > 255 ||
      o.report_hz < 1 || o.report_hz > 100 || o.report_mcs < 0 || o.report_mcs > 7) {
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
