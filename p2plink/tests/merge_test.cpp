// SPDX-License-Identifier: GPL-2.0-or-later
// BlockMerger / SeqDedup unit test: real RsCodec blocks through two ears.
#include <array>
#include <cstdio>
#include <functional>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "p2p/fec.h"
#include "p2p/report.h"
#include "p2p/rx_merge.h"
#include "p2p/wire.h"

using namespace p2p;

static int g_fail = 0;
static void check(bool ok, const std::string& what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
  if (!ok) g_fail++;
}
template <class A, class B>
static void check_eq(const A& a, const B& b, const char* what) {  // operands evaluated once
  check(a == b, std::string(what) + " (got " + std::to_string(a) + ", want " + std::to_string(b) + ")");
}
#define CHECK_EQ(a, b, what) check_eq((a), (b), (what))

// One encoder frame packed into a k/n block, exactly as the TX path does it.
struct TestBlock {
  uint16_t id = 0;
  uint8_t k = 0, n = 0, rung = 0, stream = 0;
  size_t sym_len = 0;
  std::vector<uint8_t> frame;
  std::array<uint8_t, 8> meta{};
  std::vector<std::vector<uint8_t>> syms;  // n symbols of sym_len

  P2pHeader hdr(uint8_t idx) const {
    P2pHeader h;
    h.type = idx < k ? uint8_t(Type::VideoData) : uint8_t(Type::VideoRepair);
    h.flags = idx + 1 == n ? uint8_t(kFlagLastInBlock) : 0;
    h.rung = rung;
    h.fec_k = k;
    h.fec_n = n;
    h.fec_idx = idx;
    h.stream = stream;
    h.seq = uint32_t(id) * n + idx;
    h.block_id = id;
    h.payload_len = uint16_t(sym_len);
    return h;
  }
};

static TestBlock make_block(const RsCodec& rs, uint16_t id, size_t frame_len, uint8_t rung,
                            uint32_t seed) {
  TestBlock b;
  b.id = id;
  b.k = rs.k();
  b.n = rs.n();
  b.rung = rung;
  b.sym_len = block_symbol_len(frame_len, rs.k());
  b.frame.resize(frame_len);
  uint32_t x = seed * 2654435761u + 1;
  for (auto& v : b.frame) { x = x * 1103515245u + 12345u; v = uint8_t(x >> 24); }
  for (size_t i = 0; i < 8; i++) b.meta[i] = uint8_t(seed + i);

  std::vector<uint8_t> payload(size_t(b.k) * b.sym_len, 0);
  put_le32(payload.data(), uint32_t(frame_len));
  std::memcpy(payload.data() + 4, b.meta.data(), 8);
  std::memcpy(payload.data() + kBlockPrefixLen, b.frame.data(), frame_len);

  b.syms.assign(b.n, std::vector<uint8_t>(b.sym_len));
  std::vector<const uint8_t*> src(b.k);
  std::vector<uint8_t*> rep(size_t(b.n - b.k));
  for (size_t i = 0; i < b.k; i++) {
    std::memcpy(b.syms[i].data(), payload.data() + i * b.sym_len, b.sym_len);
    src[i] = b.syms[i].data();
  }
  for (size_t r = 0; r < rep.size(); r++) rep[r] = b.syms[b.k + r].data();
  rs.encode(src.data(), rep.data(), b.sym_len);
  return b;
}

static bool feed(BlockMerger& m, uint8_t ear, const TestBlock& b, uint8_t idx, uint64_t now,
                 int8_t rssi = -60) {
  return m.on_symbol(ear, b.hdr(idx), b.syms[idx].data(), b.sym_len, rssi, now);
}

// Feeds every symbol of `b` from every listed ear, except the indices in skip[ear].
static void feed_all(BlockMerger& m, const TestBlock& b, uint64_t now,
                     const std::vector<std::vector<uint8_t>>& skip, int8_t rssi0 = -60,
                     int8_t rssi1 = -70) {
  for (uint8_t idx = 0; idx < b.n; idx++) {
    for (uint8_t ear = 0; ear < skip.size(); ear++) {
      bool sk = false;
      for (uint8_t s : skip[ear]) sk |= (s == idx);
      if (!sk) feed(m, ear, b, idx, now, ear == 0 ? rssi0 : rssi1);
    }
  }
}

static bool frame_matches(const DecodedBlock& d, const TestBlock& b) {
  return d.ok && d.block_id == b.id && d.frame == b.frame && d.meta == b.meta && d.rung == b.rung;
}

struct Sink {
  std::vector<DecodedBlock> out;
  std::function<void(const DecodedBlock&)> fn = [this](const DecodedBlock& d) { out.push_back(d); };
};

int main() {
  RsCodec rs(8, 12);
  const uint8_t R = 5;  // rung every block in this test is "sent" at

  // ---- (a) no loss, both ears -------------------------------------------
  {
    std::printf("-- (a) two ears, no loss\n");
    BlockMerger m;
    Sink s;
    // frame lengths: 11188 -> exactly 1400 B symbols, 11180 -> 1400 with
    // 8 B pad, 11100 -> 1392 (needs padding to the 4 B grid).
    TestBlock b0 = make_block(rs, 100, 11188, R, 1), b1 = make_block(rs, 101, 11180, R, 2),
              b2 = make_block(rs, 102, 11100, R, 3);
    std::printf("    sym_len: %zu %zu %zu\n", b0.sym_len, b1.sym_len, b2.sym_len);
    CHECK_EQ(b0.sym_len, size_t(1400), "(a) 11188 B frame -> 1400 B symbols");
    check(b1.sym_len == 1400 && b1.k * b1.sym_len > kBlockPrefixLen + b1.frame.size(),
          "(a) 11180 B frame pads inside 1400 B symbols");
    check(b2.sym_len % 4 == 0 && b2.sym_len >= 32 && b2.k * b2.sym_len >= kBlockPrefixLen + 11100,
          "(a) block_symbol_len contract (4 B grid, >= 32, holds prefix+frame)");

    uint64_t t = 0;
    for (const TestBlock* b : {&b0, &b1, &b2}) {
      feed_all(m, *b, t, {{}, {}});
      m.poll(t, s.fn);
      t += 5;
    }
    CHECK_EQ(s.out.size(), size_t(3), "(a) three blocks emitted");
    check(s.out.size() == 3 && frame_matches(s.out[0], b0) && frame_matches(s.out[1], b1) &&
              frame_matches(s.out[2], b2),
          "(a) frames + meta round-trip in block order");
    check(s.out.size() == 3 && !s.out[0].used_fec && !s.out[1].used_fec && !s.out[2].used_fec,
          "(a) fast path (no FEC)");
    check(s.out.size() == 3 && s.out[0].lost_symbols == 0 && s.out[2].lost_symbols == 0,
          "(a) lost_symbols 0");
    check(s.out.size() == 3 && (s.out[2].flags & kFlagLastInBlock), "(a) block flags OR'd");
    CHECK_EQ(m.pending_blocks(), size_t(1), "(a) 100/101 retired by rule (1), 102 pending");
    WindowStats w = m.take_window(t);
    CHECK_EQ(w.uniq, 36u, "(a) uniq = 3 x 12 symbols");
    CHECK_EQ(w.diversity, 36u, "(a) diversity = second ear's copies");
    CHECK_EQ(w.late, 0u, "(a) late 0");
    CHECK_EQ(w.rejected, 0u, "(a) rejected 0");
    CHECK_EQ(w.blocks_ok, 3u, "(a) blocks_ok 3");
    CHECK_EQ(w.blocks_fec, 0u, "(a) blocks_fec 0");
    CHECK_EQ(w.blocks_failed, 0u, "(a) blocks_failed 0");
    CHECK_EQ(w.loss_permille, 0, "(a) loss 0");
    CHECK_EQ(w.fec_fail_permille, 0, "(a) fec_fail 0");
    CHECK_EQ(w.adapters, 2, "(a) adapters 2");
    CHECK_EQ(w.rssi_best, -60, "(a) rssi_best = ear0");
    CHECK_EQ(w.rssi_mean, -65, "(a) rssi_mean over ears");
    CHECK_EQ(w.rung_seen, R, "(a) rung_seen");
    CHECK_EQ(w.rung_ok[R], 36u, "(a) rung_ok[R] = uniq");
    CHECK_EQ(w.rung_lost[R], 0u, "(a) rung_lost[R] 0");
    CHECK_EQ(w.ear_rx[0], 36u, "(a) ear_rx[0]");
    CHECK_EQ(w.ear_rx[1], 36u, "(a) ear_rx[1]");
    CHECK_EQ(m.total_uniq(), uint64_t(36), "(a) total_uniq");
    CHECK_EQ(m.total_diversity(), uint64_t(36), "(a) total_diversity");
    WindowStats w2 = m.take_window(t);
    check(w2.uniq == 0 && w2.diversity == 0 && w2.blocks_ok == 0 && w2.rssi_best == -127,
          "(a) take_window resets");
    // A non-video type is rejected.
    P2pHeader ip;
    ip.type = uint8_t(Type::IpData);
    uint8_t junk[4] = {1, 2, 3, 4};
    check(!m.on_symbol(0, ip, junk, 4, -50, t), "(a) IpData refused by on_symbol");
    CHECK_EQ(m.take_window(t).rejected, 1u, "(a) ... and counted as rejected");
  }

  // ---- (b) complementary loss across ears --------------------------------
  {
    std::printf("-- (b) complementary loss\n");
    BlockMerger m;
    Sink s;
    TestBlock a = make_block(rs, 200, 11188, R, 10), b = make_block(rs, 201, 11180, R, 11),
              c = make_block(rs, 202, 11100, R, 12);
    feed_all(m, a, 0, {{}, {}});
    feed_all(m, b, 1, {{0, 1, 2, 3}, {4, 5}});  // union has every symbol
    feed_all(m, c, 2, {{}, {}});
    m.poll(2, s.fn);
    CHECK_EQ(s.out.size(), size_t(3), "(b) three blocks emitted");
    check(s.out.size() == 3 && frame_matches(s.out[1], b), "(b) block 201 frame intact");
    check(s.out.size() == 3 && !s.out[1].used_fec, "(b) 201 via fast path (all sources across ears)");
    check(s.out.size() == 3 && s.out[1].lost_symbols == 0, "(b) 201 lost_symbols 0");
    WindowStats w = m.take_window(3);
    CHECK_EQ(w.uniq, 36u, "(b) uniq 36");
    CHECK_EQ(w.diversity, 30u, "(b) diversity 12 + 6 + 12");
    CHECK_EQ(w.loss_permille, 0, "(b) loss 0");
    CHECK_EQ(w.blocks_ok, 3u, "(b) blocks_ok 3");

    // (b2) overlapping source loss: union is missing sources 0 and 1 -> FEC.
    TestBlock d = make_block(rs, 203, 11188, R, 13), e = make_block(rs, 204, 11180, R, 14);
    feed_all(m, d, 4, {{0, 1, 2, 3}, {0, 1}});
    m.poll(4, s.fn);
#ifdef P2P_FEC_STUB
    std::printf("skip (b2) RS decode: built against the development stub\n");
#else
    CHECK_EQ(s.out.size(), size_t(4), "(b2) block 203 emitted with 10/12 symbols");
    check(s.out.size() == 4 && frame_matches(s.out[3], d), "(b2) 203 frame recovered by RS");
    check(s.out.size() == 4 && s.out[3].used_fec, "(b2) 203 used_fec");
    check(s.out.size() == 4 && s.out[3].lost_symbols == 2, "(b2) 203 lost_symbols 2");
    feed_all(m, e, 5, {{}, {}});  // closes 203 under rule (1)
    m.poll(5, s.fn);
    CHECK_EQ(s.out.size(), size_t(5), "(b2) 204 emitted");
    w = m.take_window(6);
    CHECK_EQ(w.blocks_fec, 1u, "(b2) blocks_fec 1");
    CHECK_EQ(w.blocks_ok, 1u, "(b2) blocks_ok 1");
    CHECK_EQ(w.uniq, 22u, "(b2) uniq 10 + 12");
    CHECK_EQ(w.loss_permille, 2 * 1000 / 24, "(b2) loss = 2 / (22 + 2)");
    CHECK_EQ(w.rung_lost[R], 2u, "(b2) rung_lost[R] 2 (pre-FEC loss)");
    CHECK_EQ(w.fec_fail_permille, 0, "(b2) fec_fail 0: FEC recovered it");
#endif
  }

  // ---- (c) same 5 symbols lost on both ears -> block fails ---------------
  {
    std::printf("-- (c) correlated loss, rule (1)\n");
    BlockMerger m;
    Sink s;
    TestBlock b0 = make_block(rs, 300, 11188, R, 20), b1 = make_block(rs, 301, 11180, R, 21),
              b2 = make_block(rs, 302, 11100, R, 22), b3 = make_block(rs, 303, 11188, R, 23);
    feed_all(m, b0, 0, {{}, {}});
    feed_all(m, b1, 0, {{}, {}});
    feed_all(m, b2, 1, {{1, 3, 5, 9, 11}, {1, 3, 5, 9, 11}});  // 7 present < k
    m.poll(1, s.fn);
    CHECK_EQ(s.out.size(), size_t(2), "(c) 302 not emitted with 7/12");
    check(feed(m, 0, b3, 0, 2), "(c) 303 from ear0 accepted");
    m.poll(2, s.fn);
    CHECK_EQ(s.out.size(), size_t(2), "(c) rule (1) still waits for ear1");
    check(feed(m, 1, b3, 0, 3) == false, "(c) 303 idx0 from ear1 is a duplicate");
    m.poll(3, s.fn);
    CHECK_EQ(s.out.size(), size_t(3), "(c) 302 declared once both ears are past it");
    check(s.out.size() == 3 && !s.out[2].ok && s.out[2].block_id == 302 && s.out[2].frame.empty(),
          "(c) 302 emitted ok=false, empty frame");
    check(s.out.size() == 3 && s.out[2].lost_symbols == 5 && s.out[2].rung == R,
          "(c) 302 lost_symbols 5 at rung R");
    WindowStats w = m.take_window(3);
    CHECK_EQ(w.blocks_ok, 2u, "(c) blocks_ok 2");
    CHECK_EQ(w.blocks_failed, 1u, "(c) blocks_failed 1");
    CHECK_EQ(w.uniq, 32u, "(c) uniq 12 + 12 + 7 + 1 (ear1's 303 copy is a duplicate)");
    CHECK_EQ(w.rung_lost[R], 5u, "(c) rung_lost[R] 5");
    CHECK_EQ(w.loss_permille, 5 * 1000 / 37, "(c) loss = 5 / (32 + 5)");
    CHECK_EQ(w.fec_fail_permille, 1000 / 3, "(c) fec_fail = 1 / 3 blocks");
    CHECK_EQ(m.pending_blocks(), size_t(1), "(c) only 303 pending");
    // (c2) rule (3): a lone block with no newer traffic is declared after the dwell.
    TestBlock b4 = make_block(rs, 304, 11188, R, 24);
    feed_all(m, b4, 10, {{0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}});  // repairs only
    m.poll(10, s.fn);
    size_t before = s.out.size();
    m.poll(29, s.fn);
    CHECK_EQ(s.out.size(), before, "(c2) nothing declared before gap_dwell_ms");
    m.poll(30, s.fn);
    CHECK_EQ(s.out.size(), before + 1, "(c2) 304 declared by rule (3) at +20 ms");
    check(s.out.size() == before + 1 && !s.out.back().ok && s.out.back().lost_symbols == 8,
          "(c2) 304 ok=false, 8 lost");
  }

  // ---- (c3) a block longer on air than the dwell ---------------------------
  // First light: every IDR block (and at R0 most blocks) takes longer than
  // gap_dwell_ms to air. Rule (3) is a QUIET dwell, so symbols trickling in at
  // 7 ms spacing for ~80 ms must still complete the block.
  {
    std::printf("-- (c3) rule (3) dates from the last symbol, not the first\n");
    BlockMerger m;
    Sink s;
    TestBlock b = make_block(rs, 500, 11188, R, 40);
    for (uint8_t idx = 0; idx < b.n; idx++) {
      uint64_t t = 7 * uint64_t(idx);
      feed(m, 0, b, idx, t);
      m.poll(t, s.fn);
      if (idx + 1 < b.k) check(s.out.empty(), "(c3) nothing declared while symbols keep arriving");
    }
    CHECK_EQ(s.out.size(), size_t(1), "(c3) block 500 emitted once");
    check(s.out.size() == 1 && frame_matches(s.out[0], b), "(c3) 500 decoded intact after 7 ms x 8 sources");
    // ...and a block that goes quiet IS declared, gap_dwell_ms after its last symbol.
    TestBlock c = make_block(rs, 501, 11188, R, 41);
    for (uint8_t idx = 0; idx < 5; idx++) feed(m, 0, c, idx, 100 + 7 * idx);  // last at 128
    m.poll(147, s.fn);
    CHECK_EQ(s.out.size(), size_t(1), "(c3) 501 held until 20 ms after its last symbol");
    m.poll(148, s.fn);
    CHECK_EQ(s.out.size(), size_t(2), "(c3) 501 declared at last + 20 ms");
    check(s.out.size() == 2 && !s.out[1].ok, "(c3) 501 ok=false");
  }

  // ---- (d) silent ear ----------------------------------------------------
  {
    std::printf("-- (d) silent ear excluded from rule (1) and adapters\n");
    MergeConfig cfg;
    cfg.gap_dwell_ms = 10000;  // only rule (1) can close a block here
    cfg.window_blocks = 100;
    BlockMerger m(cfg);
    Sink s;
    std::vector<TestBlock> blocks;
    for (uint16_t i = 0; i <= 30; i++) blocks.push_back(make_block(rs, uint16_t(400 + i), 1000 + i, R, 30 + i));
    feed_all(m, blocks[0], 0, {{}, {}});
    m.poll(0, s.fn);
    bool held = true;
    for (uint16_t i = 1; i <= 30; i++) {
      uint64_t t = 10 * i;
      feed_all(m, blocks[i], t, {{}});  // ear0 only
      m.poll(t, s.fn);
      if (t == 100) {
        CHECK_EQ(m.pending_blocks(), size_t(11), "(d) t=100: ear1 not silent yet, rule (1) holds all");
        check(!m.ear_silent(1, t), "(d) t=100: ear1 not silent");
      }
      if (t == 190) CHECK_EQ(m.pending_blocks(), size_t(20), "(d) t=190: still held");
      if (t == 200) {
        check(m.ear_silent(1, t), "(d) t=200: ear1 silent");
        CHECK_EQ(m.pending_blocks(), size_t(1), "(d) t=200: rule (1) stops waiting for ear1");
      }
      if (t > 200) held &= (m.pending_blocks() == 1);
    }
    check(held, "(d) blocks keep retiring on ear0 alone");
    CHECK_EQ(s.out.size(), size_t(31), "(d) all 31 blocks emitted ok");
    bool all_ok = true;
    for (size_t i = 0; i < s.out.size(); i++) all_ok &= frame_matches(s.out[i], blocks[i]);
    check(all_ok, "(d) frames intact and in order");
    check(!m.ear_silent(0, 300), "(d) ear0 not silent");
    WindowStats w = m.take_window(300);
    CHECK_EQ(w.adapters, 1, "(d) adapters 1");
    CHECK_EQ(w.ear_rx[0], 31u * 12u, "(d) ear_rx[0]");
    CHECK_EQ(w.ear_rx[1], 12u, "(d) ear_rx[1]");
    CHECK_EQ(w.loss_permille, 0, "(d) loss 0");
  }

  // ---- (e) late after retire, extra symbol after emit --------------------
  {
    std::printf("-- (e) late symbols\n");
    BlockMerger m;
    Sink s;
    TestBlock b0 = make_block(rs, 500, 11188, R, 40), b1 = make_block(rs, 501, 11180, R, 41),
              b2 = make_block(rs, 502, 11100, R, 42);
    std::vector<uint8_t> repairs = {8, 9, 10, 11};
    feed_all(m, b0, 0, {repairs, repairs});  // sources only
    feed_all(m, b1, 0, {repairs, repairs});
    m.poll(0, s.fn);
    CHECK_EQ(s.out.size(), size_t(2), "(e) 500/501 emitted on their k sources");
    check(feed(m, 0, b1, 8, 1), "(e) trailing repair of an emitted block is uniq, not late");
    m.poll(1, s.fn);
    CHECK_EQ(s.out.size(), size_t(2), "(e) ... and does not emit it twice");
    feed_all(m, b2, 2, {{}, {}});
    m.poll(2, s.fn);
    CHECK_EQ(s.out.size(), size_t(3), "(e) 502 emitted");
    CHECK_EQ(m.pending_blocks(), size_t(1), "(e) 500/501 retired");
    check(!feed(m, 0, b0, 9, 3), "(e) symbol of a retired block refused");
    check(!feed(m, 1, b0, 9, 3), "(e) ... from either ear");
    m.poll(3, s.fn);
    CHECK_EQ(s.out.size(), size_t(3), "(e) retired block not emitted again");
    // Wrong symbol length for a known block is rejected, not late.
    check(!m.on_symbol(0, b2.hdr(8), b2.syms[8].data(), b2.sym_len - 4, -60, 3), "(e) len mismatch refused");
    WindowStats w = m.take_window(4);
    CHECK_EQ(w.late, 2u, "(e) late 2");
    CHECK_EQ(w.rejected, 1u, "(e) rejected 1");
    CHECK_EQ(w.uniq, 8u + 8u + 1u + 12u, "(e) uniq counts the trailing repair");
    CHECK_EQ(w.rung_lost[R], 3u + 4u, "(e) retired 500 (4 missing) + 501 (3 missing) are loss");
  }

  // ---- (f) block_id wrap --------------------------------------------------
  {
    std::printf("-- (f) block_id wrap\n");
    BlockMerger m;
    Sink s;
    uint16_t ids[] = {65534, 65535, 0, 1};
    std::vector<TestBlock> blocks;
    for (uint16_t id : ids) blocks.push_back(make_block(rs, id, 5000 + id % 7, R, 50 + id % 7));
    // Deliver 0 (ear0 only) before 65535 to prove ordering is by modular id,
    // not arrival: ear1 has not advanced past 65535, so rule (1) holds 0.
    feed_all(m, blocks[0], 0, {{}, {}});
    feed_all(m, blocks[2], 1, {{}});
    m.poll(1, s.fn);
    CHECK_EQ(s.out.size(), size_t(1), "(f) 0 held behind the unheard 65535 (gap rule 1 not met)");
    feed_all(m, blocks[1], 2, {{}, {}});
    for (uint8_t idx = 0; idx < blocks[2].n; idx++) feed(m, 1, blocks[2], idx, 3);
    feed_all(m, blocks[3], 3, {{}, {}});
    m.poll(3, s.fn);
    CHECK_EQ(s.out.size(), size_t(4), "(f) four blocks emitted");
    bool order = s.out.size() == 4;
    for (size_t i = 0; order && i < 4; i++) order &= frame_matches(s.out[i], blocks[i]);
    check(order, "(f) emitted 65534, 65535, 0, 1 in modular order");
    check(!feed(m, 0, blocks[0], 3, 4), "(f) 65534 is late after the wrap");
    CHECK_EQ(m.pending_blocks(), size_t(1), "(f) only block 1 pending");
    CHECK_EQ(m.take_window(4).late, 1u, "(f) late 1");
  }

  // ---- (g) SeqDedup -------------------------------------------------------
  {
    std::printf("-- (g) SeqDedup\n");
    SeqDedup d;
    check(d.accept(2, 1, 10), "(g) first sight accepted");
    check(!d.accept(2, 1, 10), "(g) repeat refused");
    check(d.accept(2, 1, 11), "(g) next accepted");
    check(d.accept(2, 1, 9), "(g) older first sight accepted");
    check(!d.accept(2, 1, 9), "(g) older repeat refused");
    check(d.accept(3, 1, 10), "(g) other type independent");
    check(d.accept(2, 0, 10), "(g) other stream independent");
    // forward jump resync
    check(d.accept(2, 1, 11 + 3000), "(g) forward jump accepted");
    check(!d.accept(2, 1, 11 + 3000), "(g) ... repeat refused");
    check(d.accept(2, 1, 11 + 3001), "(g) ... next accepted");
    // sliding window
    SeqDedup e;
    bool seq_ok = true;
    for (uint32_t s = 0; s <= 5000; s++) seq_ok &= e.accept(2, 2, s);
    check(seq_ok, "(g) 5001 sequential seqs all first sight");
    check(!e.accept(2, 2, 4999), "(g) recent repeat refused");
    check(!e.accept(2, 2, 5000 - 4096), "(g) seq outside the 4096 window refused");
    check(!e.accept(2, 2, 5000 - 4095), "(g) seq at the window edge already seen");
    // seq wrap
    SeqDedup f;
    check(f.accept(4, 0, 0xffffffffu), "(g) seq 2^32-1 accepted");
    check(f.accept(4, 0, 0), "(g) seq wraps to 0");
    check(!f.accept(4, 0, 0xffffffffu), "(g) 2^32-1 now a repeat");
    // peer restart: same seqs from a new session are first sights
    SeqDedup r;
    for (uint32_t s = 0; s <= 300; s++) r.accept(5, 0, s, 0x11);
    check(!r.accept(5, 0, 7, 0x11), "(g) old session: seq 7 is a repeat");
    check(r.accept(5, 0, 0, 0x22), "(g) restarted peer (new session): seq 0 accepted");
    check(r.accept(5, 0, 7, 0x22), "(g) ... and seq 7 too");
    check(!r.accept(5, 0, 7, 0x22), "(g) ... once");
  }

  // ---- (h) make_report round trip -----------------------------------------
  {
    std::printf("-- (h) make_report\n");
    BlockMerger m;
    Sink s;
    TestBlock b0 = make_block(rs, 700, 11188, 3, 60), b1 = make_block(rs, 701, 11180, 3, 61),
              b2 = make_block(rs, 702, 11100, 3, 62);
    feed_all(m, b0, 0, {{}, {}}, -55, -62);
    feed_all(m, b1, 0, {{2, 4}, {2, 4}}, -55, -62);  // 2 lost at rung 3 (still decodes)
    feed_all(m, b2, 1, {{}, {}}, -55, -62);
    m.poll(1, s.fn);
    LinkReport r = m.make_report(7, 2, /*probe_rung=*/3, /*cur_rung=*/5, 42);
    uint8_t buf[kLinkReportLen];
    encode_report(r, buf);
    auto back = decode_report(buf, sizeof buf);
    check(back.has_value(), "(h) decode_report");
    if (back) {
      CHECK_EQ(back->epoch, 7u, "(h) epoch");
      CHECK_EQ(back->uniq, 34u, "(h) uniq = total 12 + 10 + 12");
      CHECK_EQ(back->diversity, 34u, "(h) diversity total");
      CHECK_EQ(back->adapters, 2, "(h) adapters");
      CHECK_EQ(back->rssi_best, -55, "(h) rssi_best");
      CHECK_EQ(back->rssi_mean, -58, "(h) rssi_mean");
      CHECK_EQ(back->rung_seen, 3, "(h) rung_seen");
      CHECK_EQ(back->probe_rung_ok, 34, "(h) probe_rung_ok (rung 3)");
      CHECK_EQ(back->probe_rung_lost, 2, "(h) probe_rung_lost (block 701)");
      CHECK_EQ(back->cur_rung_ok, 0, "(h) cur_rung_ok (rung 5 unused)");
      CHECK_EQ(back->cur_rung_lost, 0, "(h) cur_rung_lost");
      CHECK_EQ(back->loss_permille, 2 * 1000 / 36, "(h) loss_permille");
      CHECK_EQ(back->fec_fail_permille, 0, "(h) fec_fail_permille");
      CHECK_EQ(back->ip_rx, 42, "(h) ip_rx");
      check(r.epoch == back->epoch && r.uniq == back->uniq && r.diversity == back->diversity &&
                r.loss_permille == back->loss_permille && r.fec_fail_permille == back->fec_fail_permille &&
                r.rssi_best == back->rssi_best && r.rssi_mean == back->rssi_mean &&
                r.adapters == back->adapters && r.rung_seen == back->rung_seen &&
                r.probe_rung_ok == back->probe_rung_ok && r.probe_rung_lost == back->probe_rung_lost &&
                r.cur_rung_ok == back->cur_rung_ok && r.cur_rung_lost == back->cur_rung_lost &&
                r.ip_rx == back->ip_rx && r.reserved == back->reserved,
            "(h) every LinkReport field round-trips");
    }
  }

  std::printf("%s (%d failures)\n", g_fail ? "FAILED" : "PASSED", g_fail);
  return g_fail ? 1 : 0;
}
