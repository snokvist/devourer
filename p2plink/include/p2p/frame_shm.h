// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink: consumer (and a test producer) for the waybeam venc frame ring —
// the POSIX shared-memory SPSC ring the waybeam encoder publishes H.265
// frames into. Layout constants are a byte-for-byte mirror of waybeam-link
// core/include/wblink/frame_shm_format.h (GPL-2.0-or-later), so the same
// encoder binary feeds this link unchanged. Linux only (futex, eventfd).
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace p2p::shm {

inline constexpr uint32_t kRingMagic = 0x5646524D;  // "VFRM"
inline constexpr uint32_t kRingVersion = 2;
inline constexpr size_t kHeaderSize = 192;
inline constexpr size_t kHdrMagic = 0, kHdrVersion = 4, kHdrSlotCount = 8, kHdrSlotDataSize = 12,
                        kHdrTotalSize = 16, kHdrEpoch = 20, kHdrInitComplete = 24, kHdrWriteIdx = 64,
                        kHdrFutexSeq = 72, kHdrHealthMagic = 76, kHdrFullDrops = 80,
                        kHdrReadIdx = 128, kHdrConsumerWaiting = 136;
inline constexpr size_t kSlotLenPrefix = 4;
inline constexpr uint32_t kDefaultSlots = 16;
inline constexpr uint32_t kDefaultSlotSize = 512 * 1024;

// Frame blob = [8 B VencFrameMeta][NAL bytes...]
inline constexpr size_t kMetaSize = 8;
inline constexpr uint8_t kCodecH265 = 0x01;
inline constexpr uint8_t kFlagIdr = 0x01, kFlagGdr = 0x02, kFlagEnhance = 0x04, kFlagSalvaged = 0x08;
struct FrameMeta {
  uint32_t pts = 0;
  uint8_t codec = 0, flags = 0, gdr_pos = 0, gdr_len = 0;
};
inline bool read_meta(const uint8_t* blob, size_t len, FrameMeta* m) {
  if (!blob || !m || len < kMetaSize) return false;
  std::memcpy(&m->pts, blob, 4);
  m->codec = blob[4]; m->flags = blob[5]; m->gdr_pos = blob[6]; m->gdr_len = blob[7];
  return true;
}

class FrameRing {
 public:
  ~FrameRing();
  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;

  // Consumer: attaches to an existing ring (name with or without leading '/').
  // Spawns one reader thread that signals event_fd() whenever frames are
  // pending. nullptr + err on failure.
  static std::unique_ptr<FrameRing> attach(const std::string& name, std::string* err);
  // Producer (test feeder / bench): creates the ring, never blocks on write.
  static std::unique_ptr<FrameRing> create(const std::string& name, uint32_t slots, uint32_t slot_size,
                                           std::string* err);

  // Consumer, owning thread only. >0 bytes copied, 0 empty, -1 bad/oversized slot
  // (skipped) or cap too small (NOT advanced — size buf to slot_data_size()).
  long read_frame(uint8_t* buf, size_t cap);
  // Producer: false if oversize or full (dropped, counted).
  bool write_frame(const uint8_t* data, size_t len);

  int event_fd() const { return event_fd_; }
  void drain_event();
  uint32_t slot_data_size() const { return slot_data_size_; }
  bool producer_alive() const;   // init_complete still set in the mapping
  uint64_t pending() const;      // write_idx - read_idx
  struct Stats { uint64_t reads = 0, writes = 0, full_drops = 0, bad_slots = 0, ring_full_seen = 0; };
  Stats stats() const { return stats_; }

 private:
  FrameRing() = default;
  void reader_loop();
  uint8_t* map_ = nullptr;
  size_t map_len_ = 0;
  uint32_t slot_count_ = 0, slot_data_size_ = 0;
  size_t slot_stride_ = 0;
  int fd_ = -1, event_fd_ = -1;
  bool consumer_ = false;
  std::string name_;
  std::atomic<bool> stop_{false};
  std::thread reader_;
  Stats stats_;
};

}  // namespace p2p::shm
