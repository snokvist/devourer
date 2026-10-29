// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink FEC core test: encode / erase / decode round trips over the ladder's
// (k, n) pairs plus the degenerate shapes. Plain main, no framework; every
// check prints, the process returns 1 on any failure.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "p2p/fec.h"

namespace {

int g_fail = 0;
int g_total = 0;

void check(bool ok, const char* what) {
  g_total++;
  if (!ok) g_fail++;
  std::printf("[%s] %s\n", ok ? " ok " : "FAIL", what);
}

// Deterministic xorshift so a failing pattern reproduces.
struct Rng {
  uint64_t s = 0x9e3779b97f4a7c15ull;
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return uint32_t(s);
  }
  void fill(uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) p[i] = uint8_t(next());
  }
};

// One block: encode k random sources into n symbols, erase `erase[]`, decode,
// compare every source against the original. Returns true when the round trip
// is exact and decode reported success.
bool round_trip(const p2p::RsCodec& c, size_t sym_len, const std::vector<uint8_t>& erase, Rng& rng,
                bool expect_ok = true) {
  const size_t k = c.k(), n = c.n();
  std::vector<std::vector<uint8_t>> orig(n, std::vector<uint8_t>(sym_len));
  std::vector<const uint8_t*> src(k);
  std::vector<uint8_t*> repair(n - k);
  for (size_t i = 0; i < k; i++) {
    rng.fill(orig[i].data(), sym_len);
    src[i] = orig[i].data();
  }
  for (size_t i = k; i < n; i++) repair[i - k] = orig[i].data();
  c.encode(src.data(), repair.data(), sym_len);

  std::vector<bool> present(n, true);
  for (uint8_t e : erase) present[e] = false;
  std::vector<char> pres_c(n);  // decode wants const bool*, vector<bool> has no data()
  for (size_t i = 0; i < n; i++) pres_c[i] = present[i] ? 1 : 0;

  const uint8_t kFill = 0xA5;
  std::vector<std::vector<uint8_t>> out(k, std::vector<uint8_t>(sym_len, kFill));
  std::vector<const uint8_t*> syms(n, nullptr);
  std::vector<uint8_t*> outp(k, nullptr);
  for (size_t i = 0; i < n; i++)
    if (present[i]) syms[i] = orig[i].data();
  for (size_t i = 0; i < k; i++)
    if (!present[i]) outp[i] = out[i].data();

  const bool ok =
      c.decode(syms.data(), reinterpret_cast<const bool*>(pres_c.data()), outp.data(), sym_len);
  if (!expect_ok) {
    // Must refuse AND leave every output buffer untouched.
    bool untouched = true;
    for (size_t i = 0; i < k; i++)
      for (size_t b = 0; b < sym_len; b++) untouched &= out[i][b] == kFill;
    return !ok && untouched;
  }
  if (!ok) return false;
  for (size_t i = 0; i < k; i++) {
    if (present[i]) continue;
    if (std::memcmp(out[i].data(), orig[i].data(), sym_len) != 0) return false;
  }
  return true;
}

std::string pattern_str(const std::vector<uint8_t>& e) {
  std::string s = "{";
  for (size_t i = 0; i < e.size(); i++) s += (i ? "," : "") + std::to_string(e[i]);
  return s + "}";
}

}  // namespace

int main() {
  Rng rng;
  char buf[256];

  // (a) k=8, n=12: every erasure pattern of exactly 4 of 12 (495 of them),
  //     which covers all-repair-present, only-source-present and every mix.
  {
    p2p::RsCodec c(8, 12);
    check(c.k() == 8 && c.n() == 12, "k=8,n=12 identity");
    size_t patterns = 0, bad = 0;
    std::vector<uint8_t> worst;
    for (int a = 0; a < 12; a++)
      for (int b = a + 1; b < 12; b++)
        for (int cc = b + 1; cc < 12; cc++)
          for (int d = cc + 1; d < 12; d++) {
            std::vector<uint8_t> e{uint8_t(a), uint8_t(b), uint8_t(cc), uint8_t(d)};
            patterns++;
            if (!round_trip(c, 1400, e, rng)) {
              bad++;
              if (worst.empty()) worst = e;
            }
          }
    std::snprintf(buf, sizeof buf, "k=8,n=12 erase 4: %zu/%zu patterns exact%s%s", patterns - bad,
                  patterns, bad ? " first bad " : "", bad ? pattern_str(worst).c_str() : "");
    check(bad == 0 && patterns == 495, buf);
    // The three named shapes explicitly, with a non-multiple-of-4 symbol size.
    check(round_trip(c, 1403, {0, 1, 2, 3}, rng), "k=8,n=12 erase sources 0-3 (all repair present)");
    check(round_trip(c, 1403, {8, 9, 10, 11}, rng), "k=8,n=12 erase all repair (only source present)");
    check(round_trip(c, 1403, {1, 5, 9, 11}, rng), "k=8,n=12 erase mixed {1,5,9,11}");
    check(round_trip(c, 32, {7, 8, 9, 10}, rng), "k=8,n=12 erase {7,8,9,10} at sym_len 32");
    // Fewer erasures than repair symbols: surplus repair must not confuse it.
    check(round_trip(c, 1400, {2}, rng), "k=8,n=12 erase 1 source");
    check(round_trip(c, 1400, {0, 7}, rng), "k=8,n=12 erase 2 sources");
  }

  // (b) k=8, n=11: every erasure of 3 of 11 (165 patterns).
  {
    p2p::RsCodec c(8, 11);
    size_t patterns = 0, bad = 0;
    for (int a = 0; a < 11; a++)
      for (int b = a + 1; b < 11; b++)
        for (int cc = b + 1; cc < 11; cc++) {
          patterns++;
          if (!round_trip(c, 1400, {uint8_t(a), uint8_t(b), uint8_t(cc)}, rng)) bad++;
        }
    std::snprintf(buf, sizeof buf, "k=8,n=11 erase 3: %zu/%zu patterns exact", patterns - bad,
                  patterns);
    check(bad == 0 && patterns == 165, buf);
  }

  // (c) fewer than k present: false, and nothing written.
  {
    p2p::RsCodec c(8, 12);
    check(round_trip(c, 1400, {0, 1, 2, 3, 4}, rng, false), "k=8,n=12 erase 5 -> false, no write");
    check(round_trip(c, 1400, {0, 1, 2, 8, 9, 10, 11}, rng, false),
          "k=8,n=12 erase 7 -> false, no write");
    std::vector<uint8_t> all(12);
    for (uint8_t i = 0; i < 12; i++) all[i] = i;
    check(round_trip(c, 1400, all, rng, false), "k=8,n=12 erase all -> false, no write");
    bool p[12] = {true, false, true, true, false, false, true, true, false, true, true, true};
    check(p2p::RsCodec::count_present(p, 12) == 8, "count_present counts 8 of 12");
    check(p2p::RsCodec::count_present(p, 0) == 0, "count_present over 0 symbols is 0");
  }

  // (d) k=1, n=2: repetition. The repair symbol must equal the source.
  {
    p2p::RsCodec c(1, 2);
    uint8_t s[64], r[64];
    rng.fill(s, sizeof s);
    const uint8_t* src[1] = {s};
    uint8_t* rep[1] = {r};
    c.encode(src, rep, sizeof s);
    check(std::memcmp(s, r, sizeof s) == 0, "k=1,n=2 repair symbol equals the source");
    check(round_trip(c, 64, {0}, rng), "k=1,n=2 erase source, recover from repair");
    check(round_trip(c, 64, {1}, rng), "k=1,n=2 erase repair, nothing to do");
    check(round_trip(c, 64, {0, 1}, rng, false), "k=1,n=2 erase both -> false");
  }

  // (e) k=n: no repair. encode writes nothing; decode with all present is fine.
  {
    p2p::RsCodec c(8, 8);
    uint8_t s[8][16];
    const uint8_t* src[8];
    for (int i = 0; i < 8; i++) {
      rng.fill(s[i], 16);
      src[i] = s[i];
    }
    uint8_t* no_repair[1] = {nullptr};
    c.encode(src, no_repair, 16);  // must not touch repair[0]
    check(true, "k=n encode is a no-op (did not dereference repair[])");
    check(round_trip(c, 16, {}, rng), "k=n decode with all present -> true");
    check(round_trip(c, 16, {3}, rng, false), "k=n any erasure -> false, no write");
  }

  // (f) block_symbol_len: ceil((12+len)/k) -> multiple of 4 -> min 32.
  {
    check(p2p::block_symbol_len(0, 8) == 32, "block_symbol_len(0, 8) == 32");
    check(p2p::block_symbol_len(1, 8) == 32, "block_symbol_len(1, 8) == 32");
    check(p2p::block_symbol_len(11200, 8) == 1404, "block_symbol_len(11200, 8) == 1404");
    check(p2p::block_symbol_len(244, 8) == 32, "block_symbol_len(244, 8) == 32 (exactly 32)");
    check(p2p::block_symbol_len(245, 8) == 36, "block_symbol_len(245, 8) == 36 (one past)");
    check(p2p::block_symbol_len(100, 1) == 112, "block_symbol_len(100, 1) == 112");
    check(p2p::block_symbol_len(100, 0) == 0, "block_symbol_len(_, 0) == 0 (invalid k)");
    // The padded block holds the prefix + frame for the ladder's frame sizes.
    bool fits = true;
    for (size_t len : {0u, 1u, 11u, 12u, 13u, 999u, 11200u, 60000u})
      fits &= p2p::block_symbol_len(len, 8) * 8 >= 12 + len;
    check(fits, "block_symbol_len * k always covers 12 + frame_len");
  }

  // (h) wider shapes to exercise pivoting and the big-k stack path.
  {
    p2p::RsCodec c(16, 24);
    size_t bad = 0;
    for (int it = 0; it < 200; it++) {
      std::vector<uint8_t> e;
      std::vector<bool> used(24, false);
      while (e.size() < 8) {
        uint8_t i = uint8_t(rng.next() % 24);
        if (used[i]) continue;
        used[i] = true;
        e.push_back(i);
      }
      if (!round_trip(c, 300, e, rng)) bad++;
    }
    std::snprintf(buf, sizeof buf, "k=16,n=24 random erase 8 x200: %zu bad", bad);
    check(bad == 0, buf);

    p2p::RsCodec big(200, 255);
    std::vector<uint8_t> e;
    for (int i = 0; i < 55; i++) e.push_back(uint8_t(i * 4 + (i & 1)));  // 55 spread erasures
    check(round_trip(big, 64, e, rng), "k=200,n=255 erase 55 spread");
    e.clear();
    for (int i = 0; i < 55; i++) e.push_back(uint8_t(i));
    check(round_trip(big, 64, e, rng), "k=200,n=255 erase sources 0-54 (all repair used)");

    bool threw = false;
    try {
      p2p::RsCodec bad_k(0, 4);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw, "RsCodec(0, 4) throws invalid_argument");
    threw = false;
    try {
      p2p::RsCodec bad_n(9, 8);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    check(threw, "RsCodec(9, 8) throws invalid_argument");
  }

  // (g) throughput sanity: encode + decode 1000 blocks of 8 x 1400 B, 4 erased.
  {
    p2p::RsCodec c(8, 12);
    const size_t sym_len = 1400, blocks = 1000;
    std::vector<std::vector<uint8_t>> sym(12, std::vector<uint8_t>(sym_len));
    std::vector<std::vector<uint8_t>> out(8, std::vector<uint8_t>(sym_len));
    const uint8_t* src[8];
    uint8_t* rep[4];
    const uint8_t* syms[12];
    uint8_t* outp[8] = {};
    bool present[12];
    for (int i = 0; i < 8; i++) {
      rng.fill(sym[i].data(), sym_len);
      src[i] = sym[i].data();
    }
    for (int i = 0; i < 4; i++) rep[i] = sym[8 + i].data();
    for (int i = 0; i < 12; i++) {
      present[i] = !(i == 1 || i == 4 || i == 6 || i == 9);  // 3 sources + 1 repair lost
      syms[i] = present[i] ? sym[i].data() : nullptr;
    }
    for (int i = 0; i < 8; i++)
      if (!present[i]) outp[i] = out[i].data();

    auto t0 = std::chrono::steady_clock::now();
    for (size_t b = 0; b < blocks; b++) c.encode(src, rep, sym_len);
    auto t1 = std::chrono::steady_clock::now();
    bool all_ok = true;
    for (size_t b = 0; b < blocks; b++) all_ok &= c.decode(syms, present, outp, sym_len);
    auto t2 = std::chrono::steady_clock::now();
    const double enc_s = std::chrono::duration<double>(t1 - t0).count();
    const double dec_s = std::chrono::duration<double>(t2 - t1).count();
    const double src_mb = double(blocks * 8 * sym_len) / 1e6;
    std::printf("[info] throughput 8/12 x %zu blocks of 8x%zu B: encode %.1f ms (%.0f MB/s source), "
                "decode(3 src + 1 rep lost) %.1f ms (%.0f MB/s source)%s\n",
                blocks, sym_len, enc_s * 1e3, src_mb / enc_s, dec_s * 1e3, src_mb / dec_s,
                all_ok ? "" : " [decode returned false!]");
    check(all_ok && std::memcmp(out[1].data(), sym[1].data(), sym_len) == 0,
          "throughput run decoded correctly");
  }

  std::printf("%d/%d checks passed\n", g_total - g_fail, g_total);
  return g_fail ? 1 : 0;
}
