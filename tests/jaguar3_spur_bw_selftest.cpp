/* Headless guard for the 8822E spur-elimination state across a bandwidth
 * change (RadioManagementJaguar3).
 *
 * phydm_spur_eliminate_8822e is keyed on (central channel, BANDWIDTH): the
 * same channel is a spur combo at one width and spur-free at another. That
 * makes a SAME-CHANNEL bandwidth toggle — FastSetBandwidth's whole job — able
 * to cross the boundary, which the lean path used to ignore: 20 -> 5/10 on
 * ch153 left a 20 MHz-tone notch punched into the narrowband passband, and
 * 5/10 -> 20 arrived on a spur channel with no notch at all.
 *
 * Neither failure is visible without an SDR, so the invariants that decide
 * whether the fast path reprograms the notch are pinned here instead:
 *   - the 14-combo table is exactly the vendor's,
 *   - no channel is a spur combo at more than one width in {20, 5, 10},
 *     which is what makes the gate exact rather than merely safe, and
 *   - the shipped gate (spur_state_changes_8822e — the same call
 *     fast_set_bandwidth makes, not a copy) fires on exactly the toggles
 *     that change the programmed state.
 */
#include <cstdio>

#include "jaguar3/RadioManagementJaguar3.h"

using RM = jaguar3::RadioManagementJaguar3;

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

/* The vendor table (phydm_spur_eliminate_8822e), transcribed independently of
 * the implementation so a silent edit to either side fails here. */
struct Combo {
  uint8_t ch;
  ChannelWidth_t bw;
};
static const Combo kVendorCombos[] = {
    {153, CHANNEL_WIDTH_20}, {161, CHANNEL_WIDTH_20}, {169, CHANNEL_WIDTH_20},
    {151, CHANNEL_WIDTH_40}, {159, CHANNEL_WIDTH_40}, {167, CHANNEL_WIDTH_40},
    {155, CHANNEL_WIDTH_80}, {171, CHANNEL_WIDTH_80},
    {54, CHANNEL_WIDTH_40},  {102, CHANNEL_WIDTH_40}, {118, CHANNEL_WIDTH_40},
    {58, CHANNEL_WIDTH_80},  {106, CHANNEL_WIDTH_80}, {122, CHANNEL_WIDTH_80},
};
static constexpr int kVendorCount =
    static_cast<int>(sizeof(kVendorCombos) / sizeof(kVendorCombos[0]));

/* Every width the driver can be programmed at. */
static const ChannelWidth_t kAllBw[] = {CHANNEL_WIDTH_5, CHANNEL_WIDTH_10,
                                        CHANNEL_WIDTH_20, CHANNEL_WIDTH_40,
                                        CHANNEL_WIDTH_80};
/* The subset FastSetBandwidth toggles between (a 40/80 endpoint takes the
 * full set_channel_bwmode, so it never reaches the gate). */
static const ChannelWidth_t kFastBw[] = {CHANNEL_WIDTH_20, CHANNEL_WIDTH_5,
                                         CHANNEL_WIDTH_10};

static bool in_vendor_table(uint8_t ch, ChannelWidth_t bw) {
  for (const Combo &c : kVendorCombos)
    if (c.ch == ch && c.bw == bw)
      return true;
  return false;
}

static const char *bwname(ChannelWidth_t bw) {
  switch (bw) {
  case CHANNEL_WIDTH_5: return "5";
  case CHANNEL_WIDTH_10: return "10";
  case CHANNEL_WIDTH_20: return "20";
  case CHANNEL_WIDTH_40: return "40";
  case CHANNEL_WIDTH_80: return "80";
  default: return "?";
  }
}

int main() {
  /* --- 1. The table is exactly the vendor's, over the whole channel range the
   * extended synthesizer reaches (1..253) at every width. --- */
  int found = 0;
  for (int ch = 0; ch <= 255; ++ch) {
    for (ChannelWidth_t bw : kAllBw) {
      const bool got = RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), bw);
      const bool want = in_vendor_table(static_cast<uint8_t>(ch), bw);
      CHECK(got == want, "is_spur_combo_8822e(ch=%d, bw=%s) = %d, want %d", ch,
            bwname(bw), got, want);
      if (got)
        ++found;
    }
  }
  CHECK(found == kVendorCount, "table has %d combos, want %d", found,
        kVendorCount);

  /* --- 2. Within the fast path's {20, 5, 10} set no channel is a spur combo
   * at more than one width. This is what makes the gate EXACT: "at least one
   * endpoint is a spur combo" and "the two endpoints differ" coincide, so the
   * gate never reprograms a state that was already correct. If a future table
   * edit adds a 5 or 10 MHz spur entry this fires, and the gate should be
   * re-derived rather than left to over-fire silently. --- */
  for (int ch = 0; ch <= 255; ++ch) {
    int n = 0;
    for (ChannelWidth_t bw : kFastBw)
      if (RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), bw))
        ++n;
    CHECK(n <= 1, "ch %d is a spur combo at %d of {20,5,10} widths", ch, n);
  }
  /* Concretely: 5 and 10 MHz carry no spur entries at all today. */
  for (int ch = 0; ch <= 255; ++ch) {
    CHECK(!RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), CHANNEL_WIDTH_5),
          "unexpected 5 MHz spur combo at ch %d", ch);
    CHECK(!RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), CHANNEL_WIDTH_10),
          "unexpected 10 MHz spur combo at ch %d", ch);
  }

  /* --- 3. The shipped gate fires on exactly the toggles that change the
   * programmed spur state, for every channel and every ordered {20,5,10}
   * pair. `want` is derived from the table, not from the gate's own
   * expression, so rewriting the gate (|| -> &&, a dropped endpoint) fails
   * here. --- */
  for (int ch = 0; ch <= 255; ++ch) {
    for (ChannelWidth_t from : kFastBw) {
      for (ChannelWidth_t to : kFastBw) {
        if (from == to)
          continue; /* FastSetBandwidth short-circuits a no-op toggle */
        const bool a = RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), from);
        const bool b = RM::is_spur_combo_8822e(static_cast<uint8_t>(ch), to);
        const bool want = (a != b); /* the state actually differs */
        const bool got = RM::spur_state_changes_8822e(
            static_cast<uint8_t>(ch), from, to);
        CHECK(got == want, "gate(ch=%d, %s->%s) = %d, want %d", ch,
              bwname(from), bwname(to), got, want);
      }
    }
  }

  /* --- 4. Named anchors for the three channels this actually affects, in both
   * directions — the regression the fix closes. --- */
  for (uint8_t ch : {uint8_t(153), uint8_t(161), uint8_t(169)}) {
    CHECK(RM::spur_state_changes_8822e(ch, CHANNEL_WIDTH_20, CHANNEL_WIDTH_5),
          "ch %u 20->5 must reprogram (leaving a spur combo)", ch);
    CHECK(RM::spur_state_changes_8822e(ch, CHANNEL_WIDTH_5, CHANNEL_WIDTH_20),
          "ch %u 5->20 must reprogram (entering a spur combo)", ch);
    CHECK(RM::spur_state_changes_8822e(ch, CHANNEL_WIDTH_20, CHANNEL_WIDTH_10),
          "ch %u 20->10 must reprogram", ch);
    CHECK(RM::spur_state_changes_8822e(ch, CHANNEL_WIDTH_10, CHANNEL_WIDTH_20),
          "ch %u 10->20 must reprogram", ch);
    CHECK(!RM::spur_state_changes_8822e(ch, CHANNEL_WIDTH_5, CHANNEL_WIDTH_10),
          "ch %u 5->10 stays spur-free — no reprogram", ch);
  }

  /* --- 5. The 40/80-only spur channels must NOT drag the fast path into work:
   * their entries are unreachable from a {20,5,10} toggle, so a 20<->5 hop on
   * ch151 (a 40 MHz spur combo) costs nothing. --- */
  for (uint8_t ch : {uint8_t(151), uint8_t(159), uint8_t(167), uint8_t(155),
                     uint8_t(171), uint8_t(54), uint8_t(102), uint8_t(118),
                     uint8_t(58), uint8_t(106), uint8_t(122)}) {
    for (ChannelWidth_t from : kFastBw)
      for (ChannelWidth_t to : kFastBw)
        if (from != to)
          CHECK(!RM::spur_state_changes_8822e(ch, from, to),
                "ch %u %s->%s must not reprogram (its entry is 40/80 only)",
                ch, bwname(from), bwname(to));
  }

  /* --- 6. A plain non-spur channel never reprograms, at any toggle. --- */
  for (uint8_t ch : {uint8_t(36), uint8_t(40), uint8_t(44), uint8_t(48),
                     uint8_t(149), uint8_t(157), uint8_t(165), uint8_t(6)}) {
    for (ChannelWidth_t from : kFastBw)
      for (ChannelWidth_t to : kFastBw)
        if (from != to)
          CHECK(!RM::spur_state_changes_8822e(ch, from, to),
                "ch %u %s->%s must not reprogram (spur-free channel)", ch,
                bwname(from), bwname(to));
  }

  if (g_fail == 0)
    std::printf("jaguar3_spur_bw_selftest: all checks passed\n");
  else
    std::printf("jaguar3_spur_bw_selftest: %d FAILED\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
