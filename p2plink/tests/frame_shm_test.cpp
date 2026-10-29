// SPDX-License-Identifier: GPL-2.0-or-later
// Producer/consumer round trip through the venc frame ring in one process.
#include <poll.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "p2p/frame_shm.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } else std::printf("ok   %s\n", #c); } while (0)

int main() {
  using namespace p2p::shm;
  std::string err;
  const std::string name = "/p2plink.test." + std::to_string(getpid());
  auto prod = FrameRing::create(name, 4, 4096, &err);
  CHECK(prod != nullptr);
  if (!prod) { std::printf("%s\n", err.c_str()); return 1; }
  auto cons = FrameRing::attach(name, &err);
  CHECK(cons != nullptr);
  if (!cons) { std::printf("%s\n", err.c_str()); return 1; }
  CHECK(cons->slot_data_size() == 4096 && cons->producer_alive());

  std::vector<uint8_t> frame(kMetaSize + 1000);
  FrameMeta m; m.pts = 1234; m.codec = kCodecH265; m.flags = kFlagIdr; m.gdr_len = 8;
  std::memcpy(frame.data(), &m.pts, 4); frame[4] = m.codec; frame[5] = m.flags; frame[6] = 0; frame[7] = m.gdr_len;
  for (size_t i = kMetaSize; i < frame.size(); i++) frame[i] = uint8_t(i);
  CHECK(prod->write_frame(frame.data(), frame.size()));
  pollfd pfd{cons->event_fd(), POLLIN, 0};
  CHECK(poll(&pfd, 1, 500) == 1);
  cons->drain_event();
  std::vector<uint8_t> buf(cons->slot_data_size());
  long n = cons->read_frame(buf.data(), buf.size());
  CHECK(n == long(frame.size()) && std::memcmp(buf.data(), frame.data(), frame.size()) == 0);
  FrameMeta got; CHECK(read_meta(buf.data(), size_t(n), &got) && got.pts == 1234 && got.flags == kFlagIdr && got.gdr_len == 8);
  CHECK(cons->read_frame(buf.data(), buf.size()) == 0);
  // fill: 4 slots, the 5th write drops
  for (int i = 0; i < 4; i++) CHECK(prod->write_frame(frame.data(), 100));
  CHECK(!prod->write_frame(frame.data(), 100) && prod->stats().full_drops == 1);
  CHECK(cons->pending() == 4);
  for (int i = 0; i < 4; i++) CHECK(cons->read_frame(buf.data(), buf.size()) == 100);
  // oversize is refused by the producer
  std::vector<uint8_t> big(5000, 1);
  CHECK(!prod->write_frame(big.data(), big.size()));
  // undersized consumer buffer does not advance
  CHECK(prod->write_frame(frame.data(), frame.size()));
  uint8_t small[16];
  CHECK(cons->read_frame(small, sizeof small) == -1 && cons->pending() == 1);
  CHECK(cons->read_frame(buf.data(), buf.size()) == long(frame.size()));
  cons.reset();
  prod.reset();
  CHECK(FrameRing::attach(name, &err) == nullptr);  // producer unlinked it
  std::printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}
