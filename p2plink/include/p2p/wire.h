// SPDX-License-Identifier: GPL-2.0-or-later
// p2plink wire format: the 802.11 encapsulation and the p2plink header every
// frame carries. Pure, header-only, no dependencies beyond the C++ stdlib.
//
// Frame on air:
//   [radiotap (devourer build_stream_radiotap)] [802.11 Data hdr 24 B]
//   [P2pHeader 18 B] [payload] ([FCS, appended by the chip])
//
// Addressing (the whole ACK story lives in addr1 — see docs/DESIGN.md):
//   addr1 (DA): group address  -> never ACKed, never retried (video, reports,
//                                 out-of-range IP);
//               peer unicast   -> hardware ACK + retry (probe, in-range IP).
//   addr2 (SA): the sender's own UNICAST MAC (I/G clear) — an ACK responder
//               only answers a unicast TA.
//   addr3:      link id 02:50:32:50:4c:<net_id> — constant per link, lets a
//               receiver drop other links' traffic before parsing.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace p2p {

inline constexpr uint16_t kMagic = 0x5032;  // 'P','2' little-endian
inline constexpr uint8_t kVersion = 1;
inline constexpr size_t kDot11HdrLen = 24;
inline constexpr size_t kHdrLen = 18;
inline constexpr size_t kFcsLen = 4;

// Frame classes. The two bits that matter to the radio are "may this frame be
// unicast" (DA policy, link_ctl.h DaMachine) and "is it droppable".
enum class Type : uint8_t {
  VideoData = 0,   // one FEC source symbol of a video block
  VideoRepair = 1, // one FEC repair symbol of a video block
  IpData = 2,      // one or more length-prefixed IP packets
  Probe = 3,       // unicast presence probe (<= 32 B payload)
  LinkReport = 4,  // ground -> drone, report.h
  Control = 5,     // reserved: authenticated control (channel switch)
};

enum Flags : uint8_t {
  kFlagEnhance = 0x01,  // SVC-T droppable layer (from VencFrameMeta.flags)
  kFlagIdr = 0x02,
  kFlagLastInBlock = 0x04,
  kFlagPacked = 0x08,   // IpData: payload is [u16 len][pkt]... list
};

struct P2pHeader {
  uint8_t type = 0;      // Type
  uint8_t flags = 0;     // Flags
  uint8_t net_id = 0;
  uint8_t rung = 0;      // ladder rung this frame was sent at (ground scores PER per rung)
  uint8_t fec_k = 1;     // block source symbol count (1 for non-video)
  uint8_t fec_n = 1;     // block total symbols
  uint8_t fec_idx = 0;   // symbol index 0..n-1 (>= k is repair)
  uint8_t stream = 0;    // video stream id / IP class (0 ctl,1 interactive,2 bulk)
  uint32_t seq = 0;      // per-type monotonic counter (symbols for video)
  uint16_t block_id = 0; // video block (one encoder frame)
  uint16_t payload_len = 0;
};

inline void put_le16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
inline void put_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
inline uint16_t get_le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
inline uint32_t get_le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline void encode_header(const P2pHeader& h, uint8_t out[kHdrLen]) {
  put_le16(out, kMagic);
  out[2] = kVersion;
  out[3] = h.type;
  out[4] = h.flags;
  out[5] = h.net_id;
  out[6] = h.rung;
  out[7] = h.fec_k;
  out[8] = h.fec_n;
  out[9] = h.fec_idx;
  out[10] = h.stream;
  out[11] = 0;  // reserved
  put_le32(out + 12, h.seq);
  put_le16(out + 16, h.block_id);
  // payload_len is not carried: decode_header() derives it from the MPDU
  // length (the chip validates the FCS, so the length is trustworthy).
}

inline std::optional<P2pHeader> decode_header(const uint8_t* p, size_t len) {
  if (p == nullptr || len < kHdrLen) return std::nullopt;
  if (get_le16(p) != kMagic || p[2] != kVersion) return std::nullopt;
  P2pHeader h;
  h.type = p[3];
  h.flags = p[4];
  h.net_id = p[5];
  h.rung = p[6];
  h.fec_k = p[7];
  h.fec_n = p[8];
  h.fec_idx = p[9];
  h.stream = p[10];
  h.seq = get_le32(p + 12);
  h.block_id = get_le16(p + 16);
  h.payload_len = uint16_t(len - kHdrLen);
  if (h.type > uint8_t(Type::Control)) return std::nullopt;
  if (h.fec_k == 0 || h.fec_n < h.fec_k || h.fec_idx >= h.fec_n) return std::nullopt;
  return h;
}

using Mac = std::array<uint8_t, 6>;

inline bool is_group(const Mac& m) { return (m[0] & 1) != 0; }

// Group DA for the broadcast class: locally administered + group bit, with
// the net_id in the last byte so two links on one channel stay apart at the
// receiver's first byte compare.
inline Mac group_da(uint8_t net_id) { return Mac{0x07, 0x50, 0x32, 0x50, 0x4c, net_id}; }
inline Mac link_bssid(uint8_t net_id) { return Mac{0x02, 0x50, 0x32, 0x50, 0x4c, net_id}; }

// 802.11 Data frame, non-QoS, ToDS/FromDS clear (addr3 = BSSID). Sequence
// control is left 0: Realtek stamps it in hardware (EN_HWSEQ), the MT7612U
// does not need it. `da` decides the ACK policy (see file header).
inline void encode_dot11(const Mac& da, const Mac& sa, const Mac& bssid, uint8_t out[kDot11HdrLen]) {
  out[0] = 0x08; out[1] = 0x00;   // FC: Data, subtype 0
  out[2] = 0x00; out[3] = 0x00;   // duration
  std::memcpy(out + 4, da.data(), 6);
  std::memcpy(out + 10, sa.data(), 6);
  std::memcpy(out + 16, bssid.data(), 6);
  out[22] = 0x00; out[23] = 0x00; // seq ctl (hw)
}

struct Dot11View {
  Mac da, sa, bssid;
  bool retry = false;       // FC1 bit 3: a hardware retransmission copy
  uint16_t seq12 = 0;       // 12-bit hardware sequence number
  const uint8_t* body = nullptr;  // first byte after the 24 B header
  size_t body_len = 0;
};

// Parses a received MPDU (FCS already excluded by the caller via
// rx_pkt_attrib::fcs_present). Accepts Data frames of our shape only.
inline std::optional<Dot11View> parse_dot11(const uint8_t* p, size_t len) {
  if (p == nullptr || len < kDot11HdrLen) return std::nullopt;
  if ((p[0] & 0x0c) != 0x08) return std::nullopt;       // type Data
  if ((p[0] & 0xf0) != 0x00 && (p[0] & 0xf0) != 0x80) return std::nullopt; // subtype 0 or QoS(8)
  if (p[1] & 0x03) return std::nullopt;                  // ToDS/FromDS must be 0
  Dot11View v;
  std::memcpy(v.da.data(), p + 4, 6);
  std::memcpy(v.sa.data(), p + 10, 6);
  std::memcpy(v.bssid.data(), p + 16, 6);
  v.retry = (p[1] & 0x08) != 0;
  v.seq12 = uint16_t(get_le16(p + 22) >> 4);
  size_t hdr = kDot11HdrLen + (((p[0] & 0xf0) == 0x80) ? 2 : 0);
  if (len < hdr) return std::nullopt;
  v.body = p + hdr;
  v.body_len = len - hdr;
  return v;
}

// Video block payload layout (what the FEC source symbols carry, in order):
//   [u32 frame_len][u8 meta[8] = VencFrameMeta bytes][frame bytes][zero pad]
inline constexpr size_t kBlockPrefixLen = 12;

// IpData packed payload: [u16 len][pkt] repeated. Returns the number of
// packets, calling fn(ptr, len) for each; stops at the first malformed entry.
template <class Fn>
inline size_t for_each_packed(const uint8_t* p, size_t len, Fn&& fn) {
  size_t n = 0, off = 0;
  while (off + 2 <= len) {
    uint16_t l = get_le16(p + off);
    off += 2;
    if (l == 0 || off + l > len) break;
    fn(p + off, size_t(l));
    off += l;
    n++;
  }
  return n;
}

}  // namespace p2p
