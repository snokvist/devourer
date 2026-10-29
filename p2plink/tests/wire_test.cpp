// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstring>
#include <vector>
#include "p2p/report.h"
#include "p2p/wire.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } else std::printf("ok   %s\n", #c); } while (0)

int main() {
  using namespace p2p;
  P2pHeader h;
  h.type = uint8_t(Type::VideoRepair); h.flags = kFlagLastInBlock | kFlagIdr; h.net_id = 7; h.rung = 3;
  h.fec_k = 8; h.fec_n = 12; h.fec_idx = 10; h.stream = 1; h.seq = 0xdeadbeef; h.block_id = 65535;
  std::vector<uint8_t> f(kHdrLen + 100, 0xaa);
  encode_header(h, f.data());
  auto d = decode_header(f.data(), f.size());
  CHECK(d.has_value());
  CHECK(d->type == h.type && d->flags == h.flags && d->net_id == 7 && d->rung == 3);
  CHECK(d->fec_k == 8 && d->fec_n == 12 && d->fec_idx == 10 && d->stream == 1);
  CHECK(d->seq == 0xdeadbeef && d->block_id == 65535 && d->payload_len == 100);
  f[0] ^= 1; CHECK(!decode_header(f.data(), f.size())); f[0] ^= 1;
  CHECK(!decode_header(f.data(), kHdrLen - 1));
  h.fec_idx = 12; encode_header(h, f.data()); CHECK(!decode_header(f.data(), f.size()));  // idx >= n
  h.fec_idx = 0; h.type = 9; encode_header(h, f.data()); CHECK(!decode_header(f.data(), f.size()));

  Mac sa{0x00, 0x11, 0x22, 0x33, 0x44, 0x55};
  Mac da = group_da(7), bss = link_bssid(7);
  CHECK(is_group(da) && !is_group(bss) && !is_group(sa));
  uint8_t dot[kDot11HdrLen];
  encode_dot11(da, sa, bss, dot);
  std::vector<uint8_t> mpdu(dot, dot + kDot11HdrLen);
  mpdu.insert(mpdu.end(), f.begin(), f.end());
  auto v = parse_dot11(mpdu.data(), mpdu.size());
  CHECK(v.has_value() && v->da == da && v->sa == sa && v->bssid == bss && !v->retry);
  CHECK(v->body_len == f.size() && std::memcmp(v->body, f.data(), f.size()) == 0);
  mpdu[1] |= 0x08; v = parse_dot11(mpdu.data(), mpdu.size()); CHECK(v && v->retry);
  mpdu[0] = 0x40; CHECK(!parse_dot11(mpdu.data(), mpdu.size()));  // probe request rejected
  mpdu[0] = 0x88; v = parse_dot11(mpdu.data(), mpdu.size()); CHECK(v && v->body_len == f.size() - 2);  // QoS: 2 B shorter body
  mpdu[0] = 0x08; mpdu[1] = 0x01; CHECK(!parse_dot11(mpdu.data(), mpdu.size()));  // ToDS

  // packed IP list
  std::vector<uint8_t> packed;
  for (uint16_t l : {40, 1400, 60}) { packed.push_back(uint8_t(l)); packed.push_back(uint8_t(l >> 8)); packed.insert(packed.end(), l, 0x5a); }
  size_t cnt = 0, bytes = 0;
  CHECK(for_each_packed(packed.data(), packed.size(), [&](const uint8_t*, size_t l) { cnt++; bytes += l; }) == 3);
  CHECK(cnt == 3 && bytes == 1500);
  packed.resize(packed.size() - 10);  // truncated last
  CHECK(for_each_packed(packed.data(), packed.size(), [&](const uint8_t*, size_t) {}) == 2);

  LinkReport r; r.epoch = 42; r.uniq = 1000; r.diversity = 900; r.loss_permille = 12; r.fec_fail_permille = 1;
  r.rssi_best = -61; r.rssi_mean = -70; r.adapters = 3; r.rung_seen = 4; r.probe_rung_ok = 50; r.probe_rung_lost = 2;
  r.cur_rung_ok = 500; r.cur_rung_lost = 6; r.ip_rx = 17;
  uint8_t rb[kLinkReportLen]; encode_report(r, rb);
  auto rd = decode_report(rb, sizeof rb);
  CHECK(rd && rd->epoch == 42 && rd->uniq == 1000 && rd->diversity == 900 && rd->loss_permille == 12);
  CHECK(rd->rssi_best == -61 && rd->rssi_mean == -70 && rd->adapters == 3 && rd->rung_seen == 4);
  CHECK(rd->probe_rung_ok == 50 && rd->probe_rung_lost == 2 && rd->cur_rung_ok == 500 && rd->cur_rung_lost == 6 && rd->ip_rx == 17);
  CHECK(!decode_report(rb, kLinkReportLen - 1));
  Probe p{7, 123456}; uint8_t pb[kProbeLen]; encode_probe(p, pb);
  auto pd = decode_probe(pb, sizeof pb); CHECK(pd && pd->seq == 7 && pd->t_ms == 123456);
  std::printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
  return fails ? 1 : 0;
}
