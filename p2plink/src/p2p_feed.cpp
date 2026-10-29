// SPDX-License-Identifier: GPL-2.0-or-later
// p2p_feed — a bench producer for the encoder frame ring p2p_tx consumes.
// Creates the ring (16 slots x 512 KiB, the waybeam venc layout) and writes
// [8 B VencFrameMeta][access unit] per frame at the pacing rate, either
// synthetic (random payload with IDR / enhance structure) or an Annex-B
// H.265 file split into access units. Never blocks on a full ring: the write
// drops and is counted, which is what the stats line shows.
#include <getopt.h>
#include <signal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "p2p/frame_shm.h"

using namespace p2p::shm;

namespace {

volatile sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Au {
  std::vector<uint8_t> bytes;
  bool idr = false, enhance = false;
};

// Annex-B parse: start codes 00 00 01 / 00 00 00 01; NAL header 2 bytes
// (type = b0[6:1], TID = b1[2:0] - 1); a VCL NAL (type < 32) whose
// first_slice_segment_in_pic_flag (top bit of the byte after the header) is
// set opens a new AU; VPS/SPS/PPS (32-34) and an AUD (35) prefix the AU that
// follows; IDR = 19/20.
std::vector<Au> split_annexb(const std::vector<uint8_t>& s) {
  std::vector<Au> out;
  std::vector<size_t> starts;  // offsets of each NAL's first start-code byte
  for (size_t i = 0; i + 3 <= s.size(); i++) {
    if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1) {
      size_t sc = (i > 0 && s[i - 1] == 0) ? i - 1 : i;
      starts.push_back(sc);
      i += 2;
    }
  }
  Au cur;
  bool cur_has_vcl = false;
  auto flush = [&] {
    if (!cur.bytes.empty()) out.push_back(std::move(cur));
    cur = Au{};
    cur_has_vcl = false;
  };
  for (size_t n = 0; n < starts.size(); n++) {
    size_t a = starts[n], b = n + 1 < starts.size() ? starts[n + 1] : s.size();
    size_t hdr = a + ((s[a] == 0 && s[a + 1] == 0 && s[a + 2] == 0) ? 4 : 3);
    if (hdr + 2 > b) continue;
    uint8_t type = uint8_t((s[hdr] >> 1) & 0x3f);
    int tid = int(s[hdr + 1] & 0x7) - 1;
    bool vcl = type < 32;
    bool prefix = type >= 32 && type <= 35;
    if (vcl) {
      bool first_slice = hdr + 2 < b && (s[hdr + 2] & 0x80);
      if (first_slice && cur_has_vcl) flush();
      cur_has_vcl = true;
      if (type == 19 || type == 20) cur.idr = true;
      if (tid > 0) cur.enhance = true;
    } else if (prefix && cur_has_vcl) {
      flush();
    }
    cur.bytes.insert(cur.bytes.end(), s.begin() + long(a), s.begin() + long(b));
  }
  flush();
  return out;
}

void usage(const char* argv0) {
  std::fprintf(stderr,
      "usage: %s --ring NAME (--synthetic [--fps N] [--kbps N] [--idr-every N] [--enhance-ratio R]\n"
      "                        | --file X.h265 [--loop] [--fps N])\n"
      "  --ring NAME         frame ring to create (16 slots x 512 KiB)\n"
      "  --synthetic         random payload; IDR frames 6x the mean size, Enhance per ratio\n"
      "  --fps N             pacing (default 30)\n"
      "  --kbps N            synthetic mean bitrate (default 4000)\n"
      "  --idr-every N       synthetic IDR period in frames (default 60)\n"
      "  --enhance-ratio R   share of non-IDR frames flagged Enhance (default 0.5)\n"
      "  --file X.h265       Annex-B stream replayed as access units\n"
      "  --loop              replay the file forever\n",
      argv0);
}

}  // namespace

int main(int argc, char** argv) {
  std::string ring_name, file;
  bool synthetic = false, loop = false;
  int fps = 30, kbps = 4000, idr_every = 60;
  double enhance_ratio = 0.5;
  enum { kRing = 1000, kSyn, kFps, kKbps, kIdr, kEnh, kFile, kLoop, kHelp };
  static const option longopts[] = {{"ring", required_argument, nullptr, kRing},
                                    {"synthetic", no_argument, nullptr, kSyn},
                                    {"fps", required_argument, nullptr, kFps},
                                    {"kbps", required_argument, nullptr, kKbps},
                                    {"idr-every", required_argument, nullptr, kIdr},
                                    {"enhance-ratio", required_argument, nullptr, kEnh},
                                    {"file", required_argument, nullptr, kFile},
                                    {"loop", no_argument, nullptr, kLoop},
                                    {"help", no_argument, nullptr, kHelp},
                                    {nullptr, 0, nullptr, 0}};
  int c;
  while ((c = getopt_long(argc, argv, "h", longopts, nullptr)) != -1) {
    switch (c) {
      case kRing: ring_name = optarg; break;
      case kSyn: synthetic = true; break;
      case kFps: fps = std::atoi(optarg); break;
      case kKbps: kbps = std::atoi(optarg); break;
      case kIdr: idr_every = std::atoi(optarg); break;
      case kEnh: enhance_ratio = std::atof(optarg); break;
      case kFile: file = optarg; break;
      case kLoop: loop = true; break;
      case kHelp: case 'h': usage(argv[0]); return 0;
      default: usage(argv[0]); return 2;
    }
  }
  if (ring_name.empty() || (synthetic == !file.empty()) || fps < 1 || fps > 240 || kbps < 1 || idr_every < 1 ||
      enhance_ratio < 0 || enhance_ratio > 1) {
    usage(argv[0]);
    return 2;
  }
  struct sigaction sa{};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::vector<Au> aus;
  if (!file.empty()) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
      std::fprintf(stderr, "cannot open %s\n", file.c_str());
      return 1;
    }
    std::vector<uint8_t> s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    aus = split_annexb(s);
    if (aus.empty()) {
      std::fprintf(stderr, "%s: no access units found (Annex-B H.265 expected)\n", file.c_str());
      return 1;
    }
    size_t idr = 0, enh = 0;
    for (auto& a : aus) {
      idr += a.idr;
      enh += a.enhance;
    }
    std::fprintf(stderr, "%s: %zu access units (%zu IDR, %zu enhance), %zu bytes\n", file.c_str(), aus.size(), idr,
                 enh, s.size());
  }

  std::string err;
  auto ring = FrameRing::create(ring_name, kDefaultSlots, kDefaultSlotSize, &err);
  if (!ring) {
    std::fprintf(stderr, "ring %s: %s\n", ring_name.c_str(), err.c_str());
    return 1;
  }
  std::fprintf(stderr, "ring %s created: %u slots x %u B, %d fps\n", ring_name.c_str(), kDefaultSlots,
               kDefaultSlotSize, fps);

  std::mt19937 rng(12345);
  // Synthetic sizing: mean frame = kbps*1000/8/fps bytes; an IDR is 6x the
  // mean and the other frames share the remainder so the mean bitrate holds.
  const double mean = double(kbps) * 1000.0 / 8.0 / fps;
  const double non_idr = idr_every > 1 ? (mean * idr_every - 6.0 * mean) / (idr_every - 1) : mean;

  const auto period = std::chrono::nanoseconds(1000000000LL / fps);
  auto next = std::chrono::steady_clock::now();
  auto last_print = next;
  uint64_t frames = 0, bytes = 0, sec_frames = 0, sec_bytes = 0, drops_prev = 0;
  uint32_t pts = 0;
  size_t file_idx = 0;
  std::vector<uint8_t> blob;
  while (!g_stop) {
    bool idr, enhance;
    if (synthetic) {
      idr = frames % uint64_t(idr_every) == 0;
      enhance = !idr && std::uniform_real_distribution<double>(0, 1)(rng) < enhance_ratio;
      double sz = idr ? 6.0 * mean : std::max(64.0, non_idr * std::uniform_real_distribution<double>(0.7, 1.3)(rng));
      size_t n = std::min<size_t>(size_t(sz), kDefaultSlotSize - kMetaSize);
      blob.resize(kMetaSize + n);
      for (size_t i = kMetaSize; i < blob.size(); i++) blob[i] = uint8_t(rng());
    } else {
      if (file_idx >= aus.size()) {
        if (!loop) break;
        file_idx = 0;
      }
      const Au& a = aus[file_idx++];
      idr = a.idr;
      enhance = a.enhance;
      size_t n = std::min<size_t>(a.bytes.size(), kDefaultSlotSize - kMetaSize);
      blob.resize(kMetaSize + n);
      std::memcpy(blob.data() + kMetaSize, a.bytes.data(), n);
    }
    FrameMeta m;
    m.pts = pts;
    m.codec = kCodecH265;
    m.flags = uint8_t((idr ? kFlagIdr : 0) | (enhance ? kFlagEnhance : 0));
    std::memcpy(blob.data(), &m.pts, 4);
    blob[4] = m.codec;
    blob[5] = m.flags;
    blob[6] = m.gdr_pos;
    blob[7] = m.gdr_len;
    pts += uint32_t(90000 / fps);
    ring->write_frame(blob.data(), blob.size());
    frames++;
    sec_frames++;
    bytes += blob.size();
    sec_bytes += blob.size();

    next += period;
    std::this_thread::sleep_until(next);
    auto now = std::chrono::steady_clock::now();
    if (now - last_print >= std::chrono::seconds(1)) {
      last_print = now;
      uint64_t drops = ring->stats().full_drops;
      std::fprintf(stderr, "feed: %llu frames %llu B this second (%.1f kbit/s), %llu drops this second, %llu total frames, %llu total drops\n",
                   (unsigned long long)sec_frames, (unsigned long long)sec_bytes, sec_bytes * 8.0 / 1000.0,
                   (unsigned long long)(drops - drops_prev), (unsigned long long)frames, (unsigned long long)drops);
      drops_prev = drops;
      sec_frames = sec_bytes = 0;
    }
  }
  std::fprintf(stderr, "feed: done, %llu frames, %llu bytes, %llu ring-full drops\n", (unsigned long long)frames,
               (unsigned long long)bytes, (unsigned long long)ring->stats().full_drops);
  return 0;
}
