// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink FEC core: a systematic Reed-Solomon ERASURE code over GF(256).
//
// Field: GF(2^8), primitive polynomial 0x11d (x^8+x^4+x^3+x^2+1), generator
// alpha = 2 — the zfec / wfb-ng field. Encoding matrix: the zfec Vandermonde
// construction (row 0 = [1 0 .. 0], row i+1 = [alpha^(i*c)]) made systematic
// by multiplying through the inverse of its top k x k block, so symbols
// 0..k-1 are the sources verbatim and k..n-1 are repair. A block encoded here
// is byte-for-byte what zfec's fec_encode() emits for the same (k, n).
//
// Erasure-only: the caller knows which symbols are missing (the chip's FCS
// validates every delivered MPDU), so there is no error location step.
// Pure C++20, no dependencies. The constructor precomputes the repair rows on
// the heap; encode()/decode() never allocate, never throw, and are
// thread-safe on a shared const codec. decode() keeps its elimination
// workspace on the stack: k*k bytes at most, 64 B for the k = 8 ladder, 65 KB
// at the k = 255 limit.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace p2p {

inline constexpr size_t kRsMaxSymbols = 255;  // n <= 255 (GF(256) has 255 nonzero points)

class RsCodec {
 public:
  // Precomputes the generator rows for repair symbols k..n-1.
  // Requires 1 <= k <= n <= 255; anything else throws std::invalid_argument
  // (construction is not the hot path — refusing beats encoding garbage).
  RsCodec(uint8_t k, uint8_t n);

  uint8_t k() const { return k_; }
  uint8_t n() const { return n_; }

  // src[k] -> repair[n-k]; every symbol is sym_len bytes. A no-op when k == n.
  void encode(const uint8_t* const* src, uint8_t* const* repair, size_t sym_len) const;

  // syms[n]: pointer per symbol index (nullptr when missing), present[n] flags.
  // Reconstructs every MISSING SOURCE symbol (index < k) into out_sources[i]
  // (buffers provided by the caller for missing indices; others may be nullptr).
  // Returns false (writes nothing) when fewer than k symbols are present, or
  // when a present symbol / a needed output buffer is nullptr.
  // Present source symbols are never touched: the caller's fast path (all k
  // sources arrived) is a decode() that finds nothing missing and returns true.
  bool decode(const uint8_t* const* syms, const bool* present, uint8_t* const* out_sources,
              size_t sym_len) const;

  // Helper: count of present symbols.
  static size_t count_present(const bool* present, size_t n);

 private:
  // Row r (0 <= r < n-k) of the repair part of the encoding matrix, k bytes:
  // repair[r] = sum_j row[j] * src[j].
  const uint8_t* repair_row(size_t r) const { return enc_.data() + r * k_; }

  uint8_t k_;
  uint8_t n_;
  std::vector<uint8_t> enc_;  // (n-k) * k coefficients, row-major
};

// Block-level helper used by the TX and RX paths (DESIGN.md "Video block
// payload", wire.h kBlockPrefixLen):
//   source payload = [u32 frame_len][u8 meta[8]][frame bytes][zero pad to k*sym_len]
// sym_len for a frame = ceil((12 + frame_len) / k), rounded up to a multiple
// of 4, min 32. Returns 0 for k == 0 (an invalid block, never a usable length).
size_t block_symbol_len(size_t frame_len, uint8_t k);

}  // namespace p2p
