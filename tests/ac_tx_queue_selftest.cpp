/* Headless guard for src/AcTxQueue.h, the register arithmetic behind
 * DeviceConfig::Tx::be_fast_retry and ac_lifetime_us:
 *   - edca_fast_retry: TXOP limit cleared, CWmax pulled down to CWmin,
 *     AIFS and CWmin kept, on the vendor BE words of every generation that
 *     applies it;
 *   - pkt_life_time_reg: 256 us units rounded up, both halves equal,
 *     clamped to the 16-bit field;
 *   - apply_ac_tx_queue against a fake register file: nothing touched with
 *     both options off, and REG_LIFETIME_EN's other bits preserved.
 * Prints the failing case and exits nonzero. */
#include <cstdint>
#include <cstdio>
#include <map>

#include "AcTxQueue.h"

static int g_fail = 0;

#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      ++g_fail;                                                                \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                         \
      std::printf(__VA_ARGS__);                                                \
      std::printf("\n");                                                       \
    }                                                                          \
  } while (0)

/* RtlAdapter's accessor shape: rtw_read<T> / rtw_write<T>. */
struct FakeRegs {
  std::map<uint16_t, uint32_t> r;
  int writes = 0;
  template <typename T> T rtw_read(uint16_t a) { return static_cast<T>(r[a]); }
  template <typename T> bool rtw_write(uint16_t a, T v) {
    r[a] = v;
    ++writes;
    return true;
  }
};

struct Tx {
  bool be_fast_retry = false;
  unsigned ac_lifetime_us = 0;
};

static void test_edca() {
  using devourer::edca_fast_retry;
  /* RTL8733B / Jaguar1 / Jaguar3 vendor BE: TXOP 0x5e, CW 15..1023, AIFS
   * 0x2b. Jaguar2's power-on BE (TXOP already 0). Jaguar3 5 MHz BE. */
  CHECK(edca_fast_retry(0x005ea42bu) == 0x0000442bu, "8733B/J3 BE -> 0x%08x",
        edca_fast_retry(0x005ea42bu));
  CHECK(edca_fast_retry(0x0000a42bu) == 0x0000442bu, "J2 BE -> 0x%08x",
        edca_fast_retry(0x0000a42bu));
  CHECK(edca_fast_retry(0x005ea48eu) == 0x0000448eu, "J3 5M BE -> 0x%08x",
        edca_fast_retry(0x005ea48eu));
  /* Every word: TXOP 0, ECWmax == ECWmin, AIFS and ECWmin unchanged. */
  for (uint32_t w : {0x00000000u, 0xffffffffu, 0x002fa226u, 0x0000a44fu,
                     0x07ff3f01u}) {
    const uint32_t f = edca_fast_retry(w);
    CHECK((f >> 16) == 0, "0x%08x: TXOP left 0x%x", w, f >> 16);
    CHECK(((f >> 12) & 0xf) == ((w >> 8) & 0xf), "0x%08x: ECWmax 0x%x", w,
          (f >> 12) & 0xf);
    CHECK((f & 0xfff) == (w & 0xfff), "0x%08x: AIFS/ECWmin changed", w);
  }
}

static void test_lifetime() {
  using devourer::pkt_life_time_reg;
  CHECK(pkt_life_time_reg(30000) == 0x00760076u, "30 ms -> 0x%08x",
        pkt_life_time_reg(30000)); /* 117.19 units, rounded up */
  CHECK(pkt_life_time_reg(256) == 0x00010001u, "256 us -> 0x%08x",
        pkt_life_time_reg(256));
  CHECK(pkt_life_time_reg(1) == 0x00010001u, "1 us rounds up to one unit");
  CHECK(pkt_life_time_reg(0) == 0, "0 us -> 0x%08x", pkt_life_time_reg(0));
  CHECK(pkt_life_time_reg(0xffffu * 256) == 0xffffffffu, "max field");
  CHECK(pkt_life_time_reg(0xffffffffu) == 0xffffffffu, "clamped, no wrap");
}

static void test_apply() {
  FakeRegs d;
  d.r[devourer::kRegEdcaBeParam] = 0x005ea42bu;
  d.r[devourer::kRegLifetimeEn] = 0x30; /* bits 4, 5 belong to others */
  Tx off;
  devourer::apply_ac_tx_queue(d, off);
  CHECK(d.writes == 0, "both off: %d register writes", d.writes);

  Tx on;
  on.be_fast_retry = true;
  on.ac_lifetime_us = 60000;
  devourer::apply_ac_tx_queue(d, on);
  CHECK(d.r[devourer::kRegEdcaBeParam] == 0x0000442bu, "BE 0x%08x",
        d.r[devourer::kRegEdcaBeParam]);
  CHECK(d.r[devourer::kRegLifetimeEn] == 0x3f, "LIFETIME_EN 0x%02x",
        d.r[devourer::kRegLifetimeEn]);
  CHECK(d.r[devourer::kRegPktLifeTime] == 0x00eb00ebu, "PKT_LIFE_TIME 0x%08x",
        d.r[devourer::kRegPktLifeTime]);
}

int main() {
  test_edca();
  test_lifetime();
  test_apply();
  if (g_fail) {
    std::printf("ac_tx_queue_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("ac_tx_queue_selftest: OK\n");
  return 0;
}
