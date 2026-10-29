// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink FEC core — see include/p2p/fec.h for the contract.
#include "p2p/fec.h"

#include <cstring>
#include <stdexcept>

namespace p2p {
namespace {

// ---------------------------------------------------------------- GF(256) --
// Log / antilog tables for alpha = 2 over 0x11d, built at compile time. The
// antilog table is doubled so exp[log a + log b] needs no reduction mod 255.
struct GfTables {
  uint8_t exp[512];
  uint8_t log[256];
  constexpr GfTables() : exp{}, log{} {
    unsigned v = 1;
    for (unsigned i = 0; i < 255; i++) {
      exp[i] = uint8_t(v);
      exp[i + 255] = uint8_t(v);
      log[v] = uint8_t(i);
      v <<= 1;
      if (v & 0x100) v ^= 0x11d;
    }
    exp[510] = exp[0];
    exp[511] = exp[1];
    log[0] = 0;  // undefined; every caller checks for zero first
  }
};
constexpr GfTables kGf{};

inline uint8_t gf_mul(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) return 0;
  return kGf.exp[kGf.log[a] + kGf.log[b]];
}

inline uint8_t gf_inv(uint8_t a) {  // a != 0
  return kGf.exp[255 - kGf.log[a]];
}

// dst[i] ^= c * src[i] for i < len. The hot loop of both encode and decode:
// one 256-entry multiply-by-c table from the log/antilog pair, then a plain
// table walk. Stack only.
void gf_addmul(uint8_t* dst, const uint8_t* src, uint8_t c, size_t len) {
  if (c == 0) return;
  if (c == 1) {
    for (size_t i = 0; i < len; i++) dst[i] ^= src[i];
    return;
  }
  uint8_t t[256];
  t[0] = 0;
  const unsigned lc = kGf.log[c];
  for (unsigned x = 1; x < 256; x++) t[x] = kGf.exp[lc + kGf.log[x]];
  for (size_t i = 0; i < len; i++) dst[i] ^= t[src[i]];
}

// In-place Gauss-Jordan inversion of an m x m matrix (row-major, stride m)
// over GF(256). Row pivoting only; the row swaps are undone as column swaps in
// reverse order at the end. Returns false on a singular matrix (cannot happen
// for any k-row subset of the Vandermonde-derived matrix, but a refusal beats
// writing garbage). perm[] is caller scratch of m entries.
bool gf_invert(uint8_t* a, size_t m, uint8_t* perm) {
  for (size_t c = 0; c < m; c++) {
    size_t p = c;
    while (p < m && a[p * m + c] == 0) p++;
    if (p == m) return false;
    perm[c] = uint8_t(p);
    if (p != c) {
      for (size_t j = 0; j < m; j++) {
        uint8_t tmp = a[c * m + j];
        a[c * m + j] = a[p * m + j];
        a[p * m + j] = tmp;
      }
    }
    uint8_t* rowc = a + c * m;
    const uint8_t inv = gf_inv(rowc[c]);
    rowc[c] = 1;
    for (size_t j = 0; j < m; j++) rowc[j] = gf_mul(rowc[j], inv);
    for (size_t r = 0; r < m; r++) {
      if (r == c) continue;
      uint8_t* rowr = a + r * m;
      const uint8_t f = rowr[c];
      if (f == 0) continue;
      rowr[c] = 0;
      for (size_t j = 0; j < m; j++) rowr[j] ^= gf_mul(f, rowc[j]);
    }
  }
  for (size_t c = m; c-- > 0;) {
    const size_t p = perm[c];
    if (p == c) continue;
    for (size_t r = 0; r < m; r++) {
      uint8_t tmp = a[r * m + c];
      a[r * m + c] = a[r * m + p];
      a[r * m + p] = tmp;
    }
  }
  return true;
}

}  // namespace

// ------------------------------------------------------------------ codec --
RsCodec::RsCodec(uint8_t k, uint8_t n) : k_(k), n_(n) {
  if (k == 0 || n < k) throw std::invalid_argument("RsCodec: need 1 <= k <= n <= 255");
  const size_t K = k, N = n;
  // zfec Vandermonde: row 0 = [1 0 .. 0] (x = 0), row i+1 = [alpha^(i*c)] (x = alpha^i).
  std::vector<uint8_t> vdm(N * K, 0);
  vdm[0] = 1;
  for (size_t i = 1; i < N; i++)
    for (size_t c = 0; c < K; c++) vdm[i * K + c] = kGf.exp[((i - 1) * c) % 255];
  // Systematic transform: invert the top k x k block, multiply the repair rows
  // through it so the top block becomes the identity (never stored).
  std::vector<uint8_t> top(vdm.begin(), vdm.begin() + ptrdiff_t(K * K));
  std::vector<uint8_t> perm(K);
  if (!gf_invert(top.data(), K, perm.data()))
    throw std::logic_error("RsCodec: Vandermonde block not invertible");
  enc_.assign((N - K) * K, 0);
  for (size_t r = 0; r < N - K; r++) {
    const uint8_t* vrow = vdm.data() + (K + r) * K;
    uint8_t* erow = enc_.data() + r * K;
    for (size_t c = 0; c < K; c++) {
      uint8_t acc = 0;
      for (size_t j = 0; j < K; j++) acc ^= gf_mul(vrow[j], top[j * K + c]);
      erow[c] = acc;
    }
  }
}

size_t RsCodec::count_present(const bool* present, size_t n) {
  size_t cnt = 0;
  for (size_t i = 0; i < n; i++) cnt += present[i] ? 1 : 0;
  return cnt;
}

void RsCodec::encode(const uint8_t* const* src, uint8_t* const* repair, size_t sym_len) const {
  const size_t K = k_, R = size_t(n_) - K;
  for (size_t r = 0; r < R; r++) {
    uint8_t* out = repair[r];
    const uint8_t* row = repair_row(r);
    std::memset(out, 0, sym_len);
    for (size_t j = 0; j < K; j++) gf_addmul(out, src[j], row[j], sym_len);
  }
}

bool RsCodec::decode(const uint8_t* const* syms, const bool* present, uint8_t* const* out_sources,
                     size_t sym_len) const {
  const size_t K = k_, N = n_;
  if (count_present(present, N) < K) return false;

  // Missing sources M (what we reconstruct) and the repair rows R we spend on
  // them, one per missing source. Any k present rows would do; preferring the
  // present sources keeps the inversion at |M| x |M| instead of k x k.
  uint8_t miss[kRsMaxSymbols], rep[kRsMaxSymbols];
  size_t m = 0;
  for (size_t i = 0; i < K; i++) {
    if (!present[i]) miss[m++] = uint8_t(i);
    else if (syms[i] == nullptr) return false;
  }
  if (m == 0) return true;
  size_t r = 0;
  for (size_t i = K; i < N && r < m; i++) {
    if (!present[i]) continue;
    if (syms[i] == nullptr) return false;
    rep[r++] = uint8_t(i);
  }
  if (r < m) return false;  // unreachable given the count, kept as the write guard
  for (size_t u = 0; u < m; u++)
    if (out_sources[miss[u]] == nullptr) return false;

  // With S the present sources: repair_R[t] = sum_j E[R[t]][j] s_j, so
  //   y_t := repair_R[t] + sum_{j in S} E[R[t]][j] s_j = sum_u sub[t][u] s_M[u]
  // and s_M[u] = sum_t inv[u][t] y_t. Expanding y_t makes every output a
  // single pass over the k present symbols with no intermediate buffer:
  //   s_M[u] = sum_t inv[u][t] repair_R[t]
  //          + sum_{j in S} (sum_t inv[u][t] E[R[t]][j]) s_j.
  uint8_t sub[kRsMaxSymbols * kRsMaxSymbols];
  uint8_t perm[kRsMaxSymbols];
  for (size_t t = 0; t < m; t++) {
    const uint8_t* row = repair_row(size_t(rep[t]) - K);
    for (size_t u = 0; u < m; u++) sub[t * m + u] = row[miss[u]];
  }
  if (!gf_invert(sub, m, perm)) return false;

  for (size_t u = 0; u < m; u++) {
    uint8_t* out = out_sources[miss[u]];
    const uint8_t* inv_row = sub + u * m;
    std::memset(out, 0, sym_len);
    for (size_t t = 0; t < m; t++) gf_addmul(out, syms[rep[t]], inv_row[t], sym_len);
    for (size_t j = 0; j < K; j++) {
      if (!present[j]) continue;
      uint8_t c = 0;
      for (size_t t = 0; t < m; t++) c ^= gf_mul(inv_row[t], repair_row(size_t(rep[t]) - K)[j]);
      gf_addmul(out, syms[j], c, sym_len);
    }
  }
  return true;
}

size_t block_symbol_len(size_t frame_len, uint8_t k) {
  if (k == 0) return 0;
  const size_t total = 12 + frame_len;  // wire.h kBlockPrefixLen + frame bytes
  size_t len = (total + k - 1) / k;
  len = (len + 3) & ~size_t(3);
  return len < 32 ? 32 : len;
}

}  // namespace p2p
