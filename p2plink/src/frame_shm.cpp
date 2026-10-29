// SPDX-License-Identifier: GPL-2.0-or-later
#include "p2p/frame_shm.h"

#include <fcntl.h>
#include <linux/futex.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <ctime>

namespace p2p::shm {
namespace {

constexpr uint32_t kReaderTimeoutNs = 100'000'000;  // 100 ms re-check tick

uint32_t ld32(const uint8_t* b, size_t off, int order) { return __atomic_load_n(reinterpret_cast<const uint32_t*>(b + off), order); }
uint64_t ld64(const uint8_t* b, size_t off, int order) { return __atomic_load_n(reinterpret_cast<const uint64_t*>(b + off), order); }
void st32(uint8_t* b, size_t off, uint32_t v, int order) { __atomic_store_n(reinterpret_cast<uint32_t*>(b + off), v, order); }
void st64(uint8_t* b, size_t off, uint64_t v, int order) { __atomic_store_n(reinterpret_cast<uint64_t*>(b + off), v, order); }

void futex_wait(uint32_t* addr, uint32_t expected, uint32_t timeout_ns) {
  timespec ts{0, static_cast<long>(timeout_ns)};
  syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAIT, expected, &ts, nullptr, 0);
}
void futex_wake(uint32_t* addr, int count) { syscall(SYS_futex, reinterpret_cast<int*>(addr), FUTEX_WAKE, count, nullptr, nullptr, 0); }

size_t align8(size_t v) { return (v + 7) & ~size_t(7); }
std::string shm_name(const std::string& n) { return n.empty() || n[0] == '/' ? n : "/" + n; }
std::string errstr(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

}  // namespace

FrameRing::~FrameRing() {
  if (reader_.joinable()) {
    stop_.store(true, std::memory_order_release);
    if (map_) futex_wake(reinterpret_cast<uint32_t*>(map_ + kHdrFutexSeq), INT32_MAX);
    reader_.join();
  }
  if (map_) munmap(map_, map_len_);
  if (fd_ >= 0) close(fd_);
  if (event_fd_ >= 0) close(event_fd_);
  if (!consumer_ && !name_.empty()) shm_unlink(name_.c_str());
}

std::unique_ptr<FrameRing> FrameRing::create(const std::string& name, uint32_t slots, uint32_t slot_size, std::string* err) {
  if (slots == 0 || (slots & (slots - 1)) != 0) { if (err) *err = "slots must be a power of two"; return nullptr; }
  std::unique_ptr<FrameRing> r(new FrameRing());
  r->name_ = shm_name(name);
  shm_unlink(r->name_.c_str());
  r->fd_ = shm_open(r->name_.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
  if (r->fd_ < 0) { if (err) *err = errstr("shm_open"); return nullptr; }
  fchmod(r->fd_, 0666);
  r->slot_count_ = slots;
  r->slot_data_size_ = slot_size;
  r->slot_stride_ = align8(kSlotLenPrefix + slot_size);
  r->map_len_ = kHeaderSize + size_t(slots) * r->slot_stride_;
  if (ftruncate(r->fd_, static_cast<off_t>(r->map_len_)) != 0) { if (err) *err = errstr("ftruncate"); return nullptr; }
  void* m = mmap(nullptr, r->map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd_, 0);
  if (m == MAP_FAILED) { if (err) *err = errstr("mmap"); return nullptr; }
  r->map_ = static_cast<uint8_t*>(m);
  std::memset(r->map_, 0, r->map_len_);
  st32(r->map_, kHdrMagic, kRingMagic, __ATOMIC_RELAXED);
  st32(r->map_, kHdrVersion, kRingVersion, __ATOMIC_RELAXED);
  st32(r->map_, kHdrSlotCount, slots, __ATOMIC_RELAXED);
  st32(r->map_, kHdrSlotDataSize, slot_size, __ATOMIC_RELAXED);
  st32(r->map_, kHdrTotalSize, static_cast<uint32_t>(r->map_len_), __ATOMIC_RELAXED);
  st32(r->map_, kHdrEpoch, static_cast<uint32_t>(time(nullptr)), __ATOMIC_RELAXED);
  st32(r->map_, kHdrInitComplete, 1, __ATOMIC_RELEASE);
  __atomic_add_fetch(reinterpret_cast<uint32_t*>(r->map_ + kHdrFutexSeq), 1, __ATOMIC_SEQ_CST);
  futex_wake(reinterpret_cast<uint32_t*>(r->map_ + kHdrFutexSeq), INT32_MAX);
  return r;
}

std::unique_ptr<FrameRing> FrameRing::attach(const std::string& name, std::string* err) {
  std::unique_ptr<FrameRing> r(new FrameRing());
  r->consumer_ = true;
  r->name_ = shm_name(name);
  r->fd_ = shm_open(r->name_.c_str(), O_RDWR, 0);
  if (r->fd_ < 0) { if (err) *err = errstr("shm_open"); return nullptr; }
  struct stat st{};
  if (fstat(r->fd_, &st) != 0) { if (err) *err = errstr("fstat"); return nullptr; }
  if (static_cast<size_t>(st.st_size) < kHeaderSize) { if (err) *err = "object smaller than ring header"; return nullptr; }
  r->map_len_ = static_cast<size_t>(st.st_size);
  void* m = mmap(nullptr, r->map_len_, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd_, 0);
  if (m == MAP_FAILED) { if (err) *err = errstr("mmap"); return nullptr; }
  r->map_ = static_cast<uint8_t*>(m);
  if (ld32(r->map_, kHdrInitComplete, __ATOMIC_ACQUIRE) != 1) { if (err) *err = "ring not initialised by producer"; return nullptr; }
  if (ld32(r->map_, kHdrMagic, __ATOMIC_RELAXED) != kRingMagic) { if (err) *err = "bad magic"; return nullptr; }
  if (ld32(r->map_, kHdrVersion, __ATOMIC_RELAXED) != kRingVersion) { if (err) *err = "unsupported ring version"; return nullptr; }
  r->slot_count_ = ld32(r->map_, kHdrSlotCount, __ATOMIC_RELAXED);
  r->slot_data_size_ = ld32(r->map_, kHdrSlotDataSize, __ATOMIC_RELAXED);
  if (r->slot_count_ == 0 || (r->slot_count_ & (r->slot_count_ - 1)) != 0) { if (err) *err = "slot_count not a power of two"; return nullptr; }
  r->slot_stride_ = align8(kSlotLenPrefix + r->slot_data_size_);
  const size_t total = kHeaderSize + size_t(r->slot_count_) * r->slot_stride_;
  if (total != r->map_len_ || ld32(r->map_, kHdrTotalSize, __ATOMIC_RELAXED) != total) { if (err) *err = "ring size mismatch"; return nullptr; }
  r->event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (r->event_fd_ < 0) { if (err) *err = errstr("eventfd"); return nullptr; }
  r->reader_ = std::thread([p = r.get()] { p->reader_loop(); });
  return r;
}

void FrameRing::reader_loop() {
  uint8_t* b = map_;
  auto* seq_addr = reinterpret_cast<uint32_t*>(b + kHdrFutexSeq);
  while (!stop_.load(std::memory_order_acquire)) {
    // Level-triggered: any pending frame nudges the owner; robust to a bump
    // that landed before this thread first parked.
    if (ld64(b, kHdrWriteIdx, __ATOMIC_ACQUIRE) != ld64(b, kHdrReadIdx, __ATOMIC_ACQUIRE)) {
      const uint64_t one = 1;
      (void)!write(event_fd_, &one, sizeof(one));
    }
    st32(b, kHdrConsumerWaiting, 1, __ATOMIC_SEQ_CST);
    const uint32_t seq = ld32(b, kHdrFutexSeq, __ATOMIC_SEQ_CST);
    futex_wait(seq_addr, seq, kReaderTimeoutNs);
    st32(b, kHdrConsumerWaiting, 0, __ATOMIC_SEQ_CST);
  }
}

long FrameRing::read_frame(uint8_t* buf, size_t cap) {
  uint8_t* b = map_;
  const uint64_t w = ld64(b, kHdrWriteIdx, __ATOMIC_ACQUIRE);
  const uint64_t r = ld64(b, kHdrReadIdx, __ATOMIC_RELAXED);
  if (r == w) return 0;
  if (w - r >= slot_count_) ++stats_.ring_full_seen;
  const uint8_t* p = b + kHeaderSize + size_t(r & (slot_count_ - 1)) * slot_stride_;
  uint32_t len;
  std::memcpy(&len, p, sizeof(len));
  if (len > slot_data_size_) {  // corrupt slot: skip, never stall
    ++stats_.bad_slots;
    st64(b, kHdrReadIdx, r + 1, __ATOMIC_RELEASE);
    return -1;
  }
  if (len > cap) { ++stats_.bad_slots; return -1; }  // caller must size to slot_data_size()
  if (len) std::memcpy(buf, p + kSlotLenPrefix, len);
  st64(b, kHdrReadIdx, r + 1, __ATOMIC_RELEASE);
  ++stats_.reads;
  return static_cast<long>(len);
}

bool FrameRing::write_frame(const uint8_t* data, size_t len) {
  uint8_t* b = map_;
  if (len > slot_data_size_) { ++stats_.full_drops; return false; }
  const uint64_t w = ld64(b, kHdrWriteIdx, __ATOMIC_RELAXED);
  const uint64_t r = ld64(b, kHdrReadIdx, __ATOMIC_ACQUIRE);
  if (w - r >= slot_count_) { ++stats_.full_drops; return false; }
  uint8_t* p = b + kHeaderSize + size_t(w & (slot_count_ - 1)) * slot_stride_;
  const uint32_t l = static_cast<uint32_t>(len);
  std::memcpy(p, &l, sizeof(l));
  if (len) std::memcpy(p + kSlotLenPrefix, data, len);
  st64(b, kHdrWriteIdx, w + 1, __ATOMIC_RELEASE);
  __atomic_add_fetch(reinterpret_cast<uint32_t*>(b + kHdrFutexSeq), 1, __ATOMIC_SEQ_CST);
  if (ld32(b, kHdrConsumerWaiting, __ATOMIC_SEQ_CST) != 0) futex_wake(reinterpret_cast<uint32_t*>(b + kHdrFutexSeq), 1);
  ++stats_.writes;
  return true;
}

void FrameRing::drain_event() {
  uint64_t v;
  while (read(event_fd_, &v, sizeof(v)) > 0) {}
}
bool FrameRing::producer_alive() const { return map_ && ld32(map_, kHdrInitComplete, __ATOMIC_ACQUIRE) == 1; }
uint64_t FrameRing::pending() const { return ld64(map_, kHdrWriteIdx, __ATOMIC_ACQUIRE) - ld64(map_, kHdrReadIdx, __ATOMIC_ACQUIRE); }

}  // namespace p2p::shm
