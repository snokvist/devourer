// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2p/stats_pub.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace p2p {
namespace {
// Layout: [u32 magic][u32 seq (odd = writing)][StatsSnapshot]
constexpr uint32_t kStatsMagic = 0x50325354;  // "P2ST"
constexpr size_t kLen = 8 + sizeof(StatsSnapshot);
}  // namespace

StatsPublisher::~StatsPublisher() {
  if (map_) munmap(map_, map_len_);
  if (!name_.empty()) shm_unlink(name_.c_str());
}

bool StatsPublisher::open(const std::string& shm_name, bool ndjson, std::string* err) {
  ndjson_ = ndjson;
  if (shm_name.empty()) return true;
  name_ = shm_name[0] == '/' ? shm_name : "/" + shm_name;
  int fd = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) { if (err) *err = std::string("shm_open: ") + strerror(errno); name_.clear(); return false; }
  if (ftruncate(fd, kLen) != 0) { if (err) *err = strerror(errno); close(fd); return false; }
  void* m = mmap(nullptr, kLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) { if (err) *err = strerror(errno); return false; }
  map_ = static_cast<uint8_t*>(m);
  map_len_ = kLen;
  std::memset(map_, 0, kLen);
  __atomic_store_n(reinterpret_cast<uint32_t*>(map_), kStatsMagic, __ATOMIC_RELEASE);
  return true;
}

void StatsPublisher::publish(const StatsSnapshot& s) {
  if (map_) {
    auto* seq = reinterpret_cast<uint32_t*>(map_ + 4);
    uint32_t v = __atomic_load_n(seq, __ATOMIC_RELAXED);
    __atomic_store_n(seq, v + 1, __ATOMIC_RELEASE);   // odd: writing
    std::memcpy(map_ + 8, &s, sizeof(s));
    __atomic_store_n(seq, v + 2, __ATOMIC_RELEASE);   // even: stable
  }
  if (ndjson_) {
    std::string line = to_ndjson(s);
    line.push_back('\n');
    fwrite(line.data(), 1, line.size(), stdout);
    fflush(stdout);
  }
}

bool StatsPublisher::read(const std::string& shm_name, StatsSnapshot* out) {
  std::string n = shm_name[0] == '/' ? shm_name : "/" + shm_name;
  int fd = shm_open(n.c_str(), O_RDONLY, 0);
  if (fd < 0) return false;
  void* m = mmap(nullptr, kLen, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (m == MAP_FAILED) return false;
  auto* b = static_cast<const uint8_t*>(m);
  bool ok = false;
  if (__atomic_load_n(reinterpret_cast<const uint32_t*>(b), __ATOMIC_ACQUIRE) == kStatsMagic) {
    for (int i = 0; i < 8 && !ok; i++) {
      uint32_t s1 = __atomic_load_n(reinterpret_cast<const uint32_t*>(b + 4), __ATOMIC_ACQUIRE);
      if (s1 & 1) continue;
      std::memcpy(out, b + 8, sizeof(*out));
      __atomic_thread_fence(__ATOMIC_ACQUIRE);
      uint32_t s2 = __atomic_load_n(reinterpret_cast<const uint32_t*>(b + 4), __ATOMIC_RELAXED);
      ok = (s1 == s2);
    }
  }
  munmap(m, kLen);
  return ok;
}

std::string StatsPublisher::to_ndjson(const StatsSnapshot& s) {
  char buf[1024];
  int n = snprintf(buf, sizeof(buf),
      "{\"ev\":\"p2p.stats\",\"seq\":%u,\"t_ms\":%llu,\"chan\":%u,\"bw\":%u,\"rung\":%u,\"state\":%u,"
      "\"da_mode\":%u,\"ack8\":%u,\"adapters\":%u,\"cap_kbps\":%u,\"video_kbps\":%u,\"ip_kbps\":%u,"
      "\"duty_permille\":%u,\"q_video_ms\":%u,\"q_ip_ms\":%u,\"q_ctl\":%u,\"report_age_ms\":%u,"
      "\"probe_age_ms\":%u,\"retries_centi\":%u,\"rssi_best\":%d,\"rssi_mean\":%d,\"loss_permille\":%u,"
      "\"fec_fail_permille\":%u,\"fec_k\":%u,\"fec_n\":%u,\"frame_bytes\":%u,\"uniq\":%u,\"diversity\":%u,"
      "\"drop_video\":%u,\"drop_ip\":%u,\"tx_submitted\":%u,\"tx_failed\":%u,\"tx_timeouts\":%u}",
      s.seq, static_cast<unsigned long long>(s.t_ms), s.channel, s.bw_mhz, s.rung, s.link_state, s.da_mode,
      s.ack8, s.adapters, s.est_capacity_kbps, s.video_budget_kbps, s.ip_budget_kbps, s.tx_duty_permille,
      s.q_video_ms, s.q_ip_ms, s.q_ctl_frames, s.report_age_ms, s.probe_age_ms, s.retries_mean_centi,
      s.rssi_best, s.rssi_mean, s.loss_permille, s.fec_fail_permille, s.fec_k, s.fec_n,
      unsigned(s.frame_bytes_hi) << 8 | s.frame_bytes_lo, s.uniq, s.diversity, s.dropped_video_frames,
      s.dropped_ip_pkts, s.tx_submitted, s.tx_failed, s.tx_timeouts);
  return std::string(buf, n > 0 ? size_t(n) : 0);
}

}  // namespace p2p
