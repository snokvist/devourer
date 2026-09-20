/* Headless guard for src/sta/Dot11.h — the management-frame module both roles
 * share.
 *
 * The properties worth testing here are the ones whose failure is silent on
 * air. A malformed IE walk reads off the end of a hostile frame; a builder
 * that emits the right bytes in the wrong order produces a frame the peer
 * ignores without comment; a station that never assigns a sequence number
 * feeds the AP's duplicate detector and watches its own traffic vanish.
 *
 * Every case below is either a round-trip through the module's own parser
 * (build → parse → compare) or an assertion about exact wire bytes. The
 * round-trips would pass on a self-consistently wrong implementation, so the
 * byte-exact assertions carry the weight and the round-trips catch the rest.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sta/Dot11.h"

namespace {

using namespace devourer::sta;

int g_fail = 0;

void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("FAIL: %s\n", what);
    g_fail++;
  }
}

const uint8_t kBssid[6] = {0x02, 0x42, 0x75, 0x05, 0xd6, 0x00};
const uint8_t kOwn[6] = {0x40, 0xa5, 0xef, 0x5a, 0x32, 0xf8};

/* The header's address order IS the difference between the two roles, so it
 * gets an exact-bytes test rather than a round-trip. */
void test_mgmt_hdr() {
  std::vector<uint8_t> m = mgmt_hdr(kFcAuth, kBssid, kOwn, kBssid);

  check(m.size() == 24, "management header is 24 bytes");
  check(m[0] == kFcAuth, "fc0 carries type+subtype");
  check(m[1] == 0, "fc1 starts clear");
  check(std::memcmp(m.data() + 4, kBssid, 6) == 0, "addr1 is the DA");
  check(std::memcmp(m.data() + 10, kOwn, 6) == 0, "addr2 is the SA");
  check(std::memcmp(m.data() + 16, kBssid, 6) == 0, "addr3 is the BSSID");
  check(m[22] == 0 && m[23] == 0, "sequence control starts zero");
}

/* Sequence assignment: the low 4 bits are the fragment number and must stay
 * clear, and the counter must wrap at 12 bits rather than overflow into them. */
void test_seq() {
  std::vector<uint8_t> m = mgmt_hdr(kFcAuth, kBssid, kOwn, kBssid);

  assign_seq(m, 1);
  check(m[22] == 0x10 && m[23] == 0x00, "seq 1 lands above the fragment nibble");
  assign_seq(m, 0x0fff);
  check(m[22] == 0xf0 && m[23] == 0xff, "the maximum sequence number packs");
  assign_seq(m, 0x1001);
  check(m[22] == 0x10 && m[23] == 0x00, "a sequence number wraps at 12 bits");

  SeqCounter c;
  check(c.next() == 0 && c.next() == 1, "the counter starts at 0 and advances");
  for (int i = 0; i < 4093; i++) c.next();
  check(c.next() == 4095, "the counter reaches 4095");
  check(c.next() == 0, "the counter wraps to 0, not to 4096");

  /* A short buffer must be refused, not written past. */
  std::vector<uint8_t> tiny(10, 0);
  assign_seq(tiny, 7);
  check(tiny[0] == 0, "assign_seq refuses a buffer too short to hold a header");
}

/* The IE walker is the one function here that reads attacker-controlled
 * lengths. A truncated element must end the walk, never read past the end. */
void test_ie_walk() {
  std::vector<uint8_t> m;
  size_t len = 0;

  append_ssid(m, "devourerAP");
  append_supported_rates(m);
  append_ds_params(m, 149);

  const uint8_t* p = find_ie(m.data(), m.size(), kEidSsid, &len);
  check(p && len == 10 && std::memcmp(p, "devourerAP", 10) == 0,
        "SSID element round-trips");
  p = find_ie(m.data(), m.size(), kEidDsParams, &len);
  check(p && len == 1 && p[0] == 149, "DS Parameter Set carries the channel");
  check(find_ie(m.data(), m.size(), kEidVhtCaps, &len) == nullptr,
        "an absent element reports absent");

  /* An element claiming more bytes than the buffer holds. */
  const uint8_t truncated[] = {kEidSsid, 200, 'a', 'b'};
  check(find_ie(truncated, sizeof truncated, kEidSsid, &len) == nullptr,
        "an element longer than the buffer is refused, not read");
  /* And one whose header itself is cut off. */
  const uint8_t stub[] = {kEidSsid};
  check(find_ie(stub, sizeof stub, kEidSsid, &len) == nullptr,
        "a one-byte element header is refused");
  /* A zero-length element must not stall the walk. */
  const uint8_t empty_then_ds[] = {kEidSsid, 0, kEidDsParams, 1, 36};
  p = find_ie(empty_then_ds, sizeof empty_then_ds, kEidDsParams, &len);
  check(p && len == 1 && p[0] == 36,
        "a zero-length element does not stall the walk");
}

/* The RSN element's exact bytes matter in both directions: an AP advertises
 * them and a station echoes them back. */
/* Golden bytes. The rate sets and the RSN element are what two on-air
 * validated AP harnesses emit; the refactor's whole claim is that these bytes
 * did not change, and until now that rested on review rather than a test. */
void test_golden_bytes() {
  static const uint8_t k24[] = {0x01, 0x08, 0x82, 0x84, 0x8b,
                                0x96, 0x24, 0x30, 0x48, 0x6c};
  static const uint8_t k5[] = {0x01, 0x08, 0x8c, 0x12, 0x98,
                               0x24, 0xb0, 0x48, 0x60, 0x6c};
  static const uint8_t kSsidIe[] = {0x00, 0x0a, 'd', 'e', 'v', 'o',
                                    'u', 'r', 'e', 'r', 'A', 'P'};
  static const uint8_t kDs[] = {0x03, 0x01, 0x24};
  std::vector<uint8_t> m;

  append_supported_rates(m);
  check(m.size() == sizeof k24 && std::memcmp(m.data(), k24, sizeof k24) == 0,
        "2.4 GHz Supported Rates bytes are unchanged");
  m.clear();
  append_supported_rates_5g(m);
  check(m.size() == sizeof k5 && std::memcmp(m.data(), k5, sizeof k5) == 0,
        "5 GHz Supported Rates bytes are unchanged");
  m.clear();
  check(append_ssid(m, "devourerAP"), "the SSID element builds");
  check(m.size() == sizeof kSsidIe &&
            std::memcmp(m.data(), kSsidIe, sizeof kSsidIe) == 0,
        "SSID element bytes are unchanged");
  m.clear();
  append_ds_params(m, 36);
  check(m.size() == sizeof kDs && std::memcmp(m.data(), kDs, sizeof kDs) == 0,
        "DS Parameter Set bytes are unchanged");

  /* Over-length bodies must be refused, not truncated into a corrupt frame. */
  m.clear();
  check(!append_ssid(m, std::string(33, 'x')), "a 33-byte SSID is refused");
  check(m.empty(), "a refused element emits nothing at all");
  m.clear();
  std::vector<uint8_t> big(256, 0);
  check(!append_ie(m, kEidSsid, big.data(), big.size()),
        "an IE body over 255 bytes is refused");
  check(m.empty(), "a refused IE emits nothing at all");
}

void test_rsn() {
  std::vector<uint8_t> m;
  size_t len = 0;

  append_rsn_ccmp_psk(m);
  const uint8_t* p = find_ie(m.data(), m.size(), kEidRsn, &len);
  check(p && len == 20, "RSN element is 20 bytes");
  if (!p) return;
  check(p[0] == 0x01 && p[1] == 0x00, "RSN version 1");
  check(p[2] == 0x00 && p[3] == 0x0f && p[4] == 0xac && p[5] == 0x04,
        "group cipher is CCMP");
  check(p[10] == 0xac && p[11] == 0x04, "pairwise cipher is CCMP");
  check(p[16] == 0xac && p[17] == 0x02, "AKM is PSK");
  /* The COUNT fields. A swapped count makes parse_beacon reject real APs, and
   * a build/parse round-trip would not notice because both sides share it. */
  check(p[6] == 0x01 && p[7] == 0x00, "exactly one pairwise cipher suite");
  check(p[12] == 0x01 && p[13] == 0x00, "exactly one AKM suite");
  check(p[8] == 0x00 && p[9] == 0x0f, "pairwise suite OUI 00-0F-AC");
  check(p[14] == 0x00 && p[15] == 0x0f, "AKM suite OUI 00-0F-AC");
  check(p[18] == 0x00 && p[19] == 0x00, "RSN capabilities are zero (no MFP)");
}

/* Build a beacon the way an AP does, parse it the way a station will. */
void test_beacon_roundtrip() {
  // SA deliberately DIFFERENT from the BSSID. With both set to kBssid the
  // "BSSID comes from addr3" assertion below passed even if the parser read
  // addr2 - it pinned nothing, which the review caught.
  static const uint8_t kOtherSa[6] = {0x06, 0x06, 0x06, 0x06, 0x06, 0x06};
  std::vector<uint8_t> m = mgmt_hdr(kFcBeacon, (const uint8_t*)"\xff\xff\xff\xff\xff\xff",
                                    kOtherSa, kBssid);
  for (int i = 0; i < 8; i++) m.push_back(0); /* timestamp */
  put_le16(m, 100);                            /* beacon interval */
  put_le16(m, 0x0011);                         /* ESS | Privacy */
  append_ssid(m, "devourerAP");
  append_supported_rates_5g(m);
  append_ds_params(m, 36);
  append_rsn_ccmp_psk(m);

  BssInfo b;
  check(parse_beacon(m.data(), m.size(), &b), "a beacon parses");
  check(std::memcmp(b.bssid, kBssid, 6) == 0, "BSSID comes from addr3");
  check(std::memcmp(b.bssid, kOtherSa, 6) != 0,
        "BSSID is NOT addr2 (the test can tell them apart)");
  check(b.ssid == "devourerAP", "SSID round-trips");
  check(b.beacon_interval_tu == 100, "beacon interval round-trips");
  check(b.capability == 0x0011, "capability round-trips");
  check(b.privacy, "the Privacy bit is decoded");
  check(b.channel == 36, "the channel comes from the DS Parameter Set");
  check(b.has_rsn && b.rsn_ccmp_psk, "our own RSN element is recognised");

  /* An RSN element we do not implement must be reported as such rather than
   * associated with and failed later. TKIP pairwise (…ac 02 in the cipher
   * position) is the classic case. */
  std::vector<uint8_t> tkip = m;
  size_t rl = 0;
  uint8_t* r = const_cast<uint8_t*>(find_ie(tkip.data() + 36, tkip.size() - 36,
                                            kEidRsn, &rl));
  check(r != nullptr, "setup: found the RSN element to corrupt");
  if (r) {
    r[11] = 0x02; /* pairwise CCMP -> TKIP */
    BssInfo b2;
    check(parse_beacon(tkip.data(), tkip.size(), &b2), "setup: still parses");
    check(b2.has_rsn && !b2.rsn_ccmp_psk,
          "an unsupported RSN suite is rejected, not silently accepted");
  }

  /* Too short to hold the fixed body. */
  BssInfo b3;
  check(!parse_beacon(m.data(), 30, &b3), "a truncated beacon is refused");

  /* A REUSED BssInfo must not carry the previous BSS's fields. A scan loop
   * does exactly this, and a frame missing the SSID/DS/RSN elements would
   * otherwise report the last BSS's values as this one's. */
  std::vector<uint8_t> bare = mgmt_hdr(kFcBeacon,
                                       (const uint8_t*)"\xff\xff\xff\xff\xff\xff",
                                       kOtherSa, kOwn);
  for (int i = 0; i < 8; i++) bare.push_back(0);
  put_le16(bare, 200);
  put_le16(bare, 0x0001); /* ESS, no Privacy, and no IEs at all */
  check(parse_beacon(bare.data(), bare.size(), &b), "a bare beacon parses");
  check(b.ssid.empty(), "a reused BssInfo does not keep the old SSID");
  check(b.channel == 0, "a reused BssInfo does not keep the old channel");
  check(!b.has_rsn, "a reused BssInfo does not keep the old RSN flag");
  check(!b.rsn_ccmp_psk, "a reused BssInfo does not keep the old RSN verdict");
  check(!b.privacy, "a reused BssInfo does not keep the old Privacy bit");
  check(b.beacon_interval_tu == 200, "the new beacon's own fields are set");

  /* An RSN element that stops before the Capabilities field cannot be judged
   * safe to join, and an MFP-required BSS must be surfaced rather than
   * associated with and failed later. */
  std::vector<uint8_t> mfp = m;
  size_t ml = 0;
  uint8_t* mr = const_cast<uint8_t*>(find_ie(mfp.data() + 36, mfp.size() - 36,
                                             kEidRsn, &ml));
  check(mr && ml == 20, "setup: RSN element found");
  if (mr && ml == 20) {
    mr[18] = 0x40; /* RSN Capabilities: MFPR */
    BssInfo b4;
    check(parse_beacon(mfp.data(), mfp.size(), &b4), "setup: parses");
    check(b4.rsn_mfp_required,
          "a BSS that REQUIRES management-frame protection is flagged");
  }
}

void test_station_builders() {
  std::vector<uint8_t> pr = build_probe_req(kOwn, "devourerAP", 36, true);
  size_t len = 0;

  check(pr[0] == kFcProbeReq, "probe request subtype");
  check(pr[4] == 0xff && pr[9] == 0xff, "probe request addr1 is broadcast");
  check(std::memcmp(pr.data() + 10, kOwn, 6) == 0, "probe request SA is ours");
  const uint8_t* p = find_ie(pr.data() + 24, pr.size() - 24, kEidSsid, &len);
  check(p && len == 10, "a directed probe carries the SSID");
  p = find_ie(pr.data() + 24, pr.size() - 24, kEidSupportedRates, &len);
  check(p && len == 8 && p[0] == 0x8c,
        "a 5 GHz probe advertises OFDM basic rates, not CCK");

  std::vector<uint8_t> wildcard = build_probe_req(kOwn, "", 0, false);
  p = find_ie(wildcard.data() + 24, wildcard.size() - 24, kEidSsid, &len);
  check(p && len == 0, "a wildcard probe carries an EMPTY SSID element");
  check(find_ie(wildcard.data() + 24, wildcard.size() - 24, kEidDsParams,
                &len) == nullptr,
        "channel 0 omits the DS Parameter Set");

  AuthFields af;
  std::vector<uint8_t> au = build_auth_req(kOwn, kBssid);
  check(parse_auth(au.data(), au.size(), &af), "auth request parses");
  check(af.algorithm == 0, "open-system authentication");
  check(af.seq == 1, "the station's auth is sequence 1");
  check(af.status == 0, "auth request status is 0");
  check(std::memcmp(au.data() + 4, kBssid, 6) == 0, "auth is addressed to the AP");

  /* The capability/RSN agreement an AP checks. */
  std::vector<uint8_t> ar = build_assoc_req(kOwn, kBssid, "devourerAP", true, true);
  check(ar[0] == kFcAssocReq, "assoc request subtype");
  check(get_le16(ar.data() + 24) == 0x0011,
        "an RSN assoc request sets ESS and Privacy together");
  check(find_ie(ar.data() + 28, ar.size() - 28, kEidRsn, &len) != nullptr,
        "an RSN assoc request carries the RSN element");

  std::vector<uint8_t> open = build_assoc_req(kOwn, kBssid, "devourerAP", false, false);
  check(get_le16(open.data() + 24) == 0x0001,
        "an open assoc request claims ESS without Privacy");
  check(find_ie(open.data() + 28, open.size() - 28, kEidRsn, &len) == nullptr,
        "an open assoc request carries no RSN element");

  std::vector<uint8_t> dr = build_deauth(kOwn, kBssid, 3);
  uint16_t reason = 0;
  check(dr[0] == kFcDeauth, "deauth subtype");
  check(parse_reason(dr.data(), dr.size(), &reason) && reason == 3,
        "deauth carries its reason code");
}

void test_assoc_resp_parse() {
  std::vector<uint8_t> m = mgmt_hdr(kFcAssocResp, kOwn, kBssid, kBssid);
  put_le16(m, 0x0011);
  put_le16(m, 0);
  put_le16(m, 0xc001); /* AID 1 with both top bits set, as on the wire */
  append_supported_rates_5g(m);

  AssocRespFields f;
  check(parse_assoc_resp(m.data(), m.size(), &f), "assoc response parses");
  check(f.status == 0, "status 0 is success");
  check(f.aid == 1, "the AID has its two top bits masked off");

  AssocRespFields g;
  check(!parse_assoc_resp(m.data(), 24, &g),
        "an assoc response with no body is refused");
}

/* Data-frame direction and header length. The QoS +2 is the offset error that
 * silently drops every QoS frame. */
void test_data_frames() {
  const uint8_t dest[6] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  std::vector<uint8_t> up = data_hdr_to_ds(kBssid, kOwn, dest, true);

  check(up[1] == (kFcToDs | kFcProtected), "uplink sets to-DS and Protected");
  check(std::memcmp(up.data() + 4, kBssid, 6) == 0, "uplink addr1 is the BSSID");
  check(std::memcmp(up.data() + 10, kOwn, 6) == 0, "uplink addr2 is us");
  check(std::memcmp(up.data() + 16, dest, 6) == 0, "uplink addr3 is the dest");

  std::vector<uint8_t> down = data_hdr_from_ds(kOwn, kBssid, dest, false);
  check(down[1] == kFcFromDs, "downlink sets from-DS only");
  check(std::memcmp(down.data() + 4, kOwn, 6) == 0, "downlink addr1 is the STA");
  check(std::memcmp(down.data() + 10, kBssid, 6) == 0,
        "downlink addr2 is the BSSID");

  /* The builders and the length function must not drift apart. */
  check(up.size() == data_hdr_len(up[0], up[1]),
        "data_hdr_to_ds's size matches data_hdr_len");
  check(down.size() == data_hdr_len(down[0], down[1]),
        "data_hdr_from_ds's size matches data_hdr_len");

  /* Every QoS data subtype, not just QoS Data. A real station sends QoS Null
   * (0xc8); an exact fc0 == 0x88 test reads its body two bytes early. */
  check(is_qos_data(0x88), "QoS Data is QoS");
  check(is_qos_data(0xc8), "QoS Null is QoS");
  check(is_qos_data(0x98), "QoS Data+CF-Ack is QoS");
  check(!is_qos_data(0x08), "plain Data is not QoS");
  check(!is_qos_data(0x48), "Null (non-QoS) is not QoS");
  check(!is_qos_data(0x80), "a Beacon is not QoS data");
  check(data_hdr_len(0xc8, kFcToDs) == 26, "QoS Null carries the QoS field");
  /* HT Control rides the Order bit on a QoS frame, and means something else
   * on a non-QoS one. */
  check(data_hdr_len(kFcQosData, kFcToDs | 0x80) == 30,
        "QoS + Order adds the 4-byte HT Control field");
  check(data_hdr_len(kFcData, kFcToDs | 0x80) == 24,
        "Order on a non-QoS frame adds nothing");

  check(data_hdr_len(kFcData, kFcToDs) == 24, "a 3-address data header is 24");
  check(data_hdr_len(kFcQosData, kFcToDs) == 26, "QoS data adds 2 bytes");
  check(data_hdr_len(kFcData, kFcToDs | kFcFromDs) == 30,
        "a 4-address frame adds 6 bytes");
  check(data_hdr_len(kFcQosData, kFcToDs | kFcFromDs) == 32,
        "4-address QoS adds both");

  std::vector<uint8_t> llc;
  append_llc_snap(llc, 0x0800);
  check(llc.size() == 8 && llc[0] == 0xaa && llc[1] == 0xaa && llc[2] == 0x03,
        "LLC/SNAP prefix");
  check(llc[6] == 0x08 && llc[7] == 0x00, "ethertype is big-endian in SNAP");
}


/* RSN elements as they actually appear in the field.
 *
 * The module used to byte-compare a canonical one-pairwise/one-AKM layout, so
 * it reported every mixed-mode WPA/WPA2 AP and every WPA3-transition AP as
 * unusable and a station would have skipped BSSes it could join. These cases
 * are the shapes that were rejected, plus the malformed ones that must still
 * be refused. */
namespace rsn {

std::vector<uint8_t> build(uint16_t ver, const std::vector<uint32_t>& group,
                           const std::vector<uint32_t>& pairwise,
                           const std::vector<uint32_t>& akm,
                           bool caps, uint16_t capval) {
  std::vector<uint8_t> v;
  auto suite = [&](uint32_t t) {
    v.push_back(0x00); v.push_back(0x0f); v.push_back(0xac);
    v.push_back((uint8_t)t);
  };
  put_le16(v, ver);
  for (uint32_t g : group) suite(g);
  if (!pairwise.empty() || !akm.empty() || caps) {
    put_le16(v, (uint16_t)pairwise.size());
    for (uint32_t c : pairwise) suite(c);
  }
  if (!akm.empty() || caps) {
    put_le16(v, (uint16_t)akm.size());
    for (uint32_t a : akm) suite(a);
  }
  if (caps) put_le16(v, capval);
  return v;
}

}  // namespace rsn

void test_rsn_real_world() {
  RsnInfo r;

  /* The canonical one-of-each element, which always worked. */
  auto plain = rsn::build(1, {4}, {4}, {2}, true, 0x0000);
  check(parse_rsn(plain.data(), plain.size(), &r), "canonical RSN parses");
  check(r.group_ccmp && r.pairwise_ccmp && r.akm_psk, "canonical is CCMP/PSK");
  check(!r.mfp_required && !r.mfp_capable, "canonical has no MFP");

  /* MIXED MODE: TKIP group, TKIP+CCMP pairwise, PSK. Rejected before; a very
   * common real AP. The group cipher is TKIP, so this is NOT joinable by this
   * project - but the PAIRWISE search must still find CCMP. */
  auto mixed = rsn::build(1, {2}, {2, 4}, {2}, true, 0x0000);
  check(parse_rsn(mixed.data(), mixed.size(), &r), "mixed-mode RSN parses");
  check(r.pairwise_count == 2, "two pairwise suites are counted");
  check(r.pairwise_ccmp, "CCMP is found AMONG several pairwise suites");
  check(!r.group_ccmp, "a TKIP group cipher is reported as not CCMP");

  /* WPA2-only AP that still lists two pairwise suites (CCMP first). */
  auto two_cc = rsn::build(1, {4}, {4, 2}, {2}, true, 0x0000);
  check(parse_rsn(two_cc.data(), two_cc.size(), &r), "parses");
  check(r.group_ccmp && r.pairwise_ccmp && r.akm_psk,
        "CCMP group with a TKIP fallback pairwise is joinable");

  /* WPA3 TRANSITION: CCMP, AKM = PSK + PSK-SHA256, MFP capable but not
   * required. Rejected before; extremely common now. */
  auto trans = rsn::build(1, {4}, {4}, {2, 6}, true, 0x0080);
  check(parse_rsn(trans.data(), trans.size(), &r), "WPA3-transition parses");
  check(r.akm_count == 2, "two AKMs are counted");
  check(r.akm_psk, "PSK is found AMONG several AKMs");
  check(r.mfp_capable && !r.mfp_required, "MFP capable, not required");

  /* WPA3-ONLY: SAE AKM, MFP required. Must NOT be reported as joinable. */
  auto sae = rsn::build(1, {4}, {4}, {8}, true, 0x00c0);
  check(parse_rsn(sae.data(), sae.size(), &r), "WPA3-only parses");
  check(!r.akm_psk, "SAE is not PSK");
  check(r.mfp_required, "WPA3-only requires MFP");

  /* A vendor OUI must not be mistaken for an 802.11 suite of the same type. */
  std::vector<uint8_t> vendor = plain;
  vendor[8] = 0x00; vendor[9] = 0x50; vendor[10] = 0xf2;  /* pairwise OUI */
  check(parse_rsn(vendor.data(), vendor.size(), &r), "vendor-OUI RSN parses");
  check(!r.pairwise_ccmp, "a vendor OUI is not 00-0F-AC CCMP");

  /* Truncation and malformed counts: refuse, never read past the end. */
  check(!parse_rsn(plain.data(), 1, &r), "a 1-byte RSN body is refused");
  auto bad_ver = rsn::build(2, {4}, {4}, {2}, true, 0);
  check(!parse_rsn(bad_ver.data(), bad_ver.size(), &r),
        "an unknown RSN version is refused");
  std::vector<uint8_t> overrun = plain;
  overrun[6] = 0xff; overrun[7] = 0xff;  /* pairwise count = 65535 */
  check(!parse_rsn(overrun.data(), overrun.size(), &r),
        "a pairwise count that overruns the element is refused");
  std::vector<uint8_t> akm_overrun = plain;
  akm_overrun[12] = 0xff; akm_overrun[13] = 0xff;
  check(!parse_rsn(akm_overrun.data(), akm_overrun.size(), &r),
        "an AKM count that overruns the element is refused");

  /* A short-but-legal element stops early and leaves later flags false. */
  auto group_only = rsn::build(1, {4}, {}, {}, false, 0);
  check(parse_rsn(group_only.data(), group_only.size(), &r),
        "an element with only a group cipher is legal");
  check(r.group_ccmp && !r.pairwise_ccmp && !r.akm_psk,
        "absent lists leave their flags false");

  /* And the end-to-end verdict through parse_beacon: a WPA3-transition BSS
   * must now be JOINABLE, and an MFP-required one must not. */
  auto mkbeacon = [&](const std::vector<uint8_t>& ie) {
    std::vector<uint8_t> m = mgmt_hdr(kFcBeacon,
                                      (const uint8_t*)"\xff\xff\xff\xff\xff\xff",
                                      kOwn, kBssid);
    for (int i = 0; i < 8; i++) m.push_back(0);
    put_le16(m, 100);
    put_le16(m, 0x0011);
    append_ssid(m, "ap");
    append_ie(m, kEidRsn, ie.data(), ie.size());
    return m;
  };
  BssInfo b;
  auto bt = mkbeacon(trans);
  check(parse_beacon(bt.data(), bt.size(), &b), "transition beacon parses");
  check(b.rsn_ccmp_psk,
        "a WPA3-TRANSITION BSS is joinable (it was rejected before)");
  auto bm = mkbeacon(mixed);
  check(parse_beacon(bm.data(), bm.size(), &b), "mixed beacon parses");
  check(!b.rsn_ccmp_psk, "a TKIP-group BSS is not joinable");
  auto bs = mkbeacon(sae);
  check(parse_beacon(bs.data(), bs.size(), &b), "WPA3-only beacon parses");
  check(!b.rsn_ccmp_psk && b.rsn_mfp_required,
        "an MFP-REQUIRED BSS is not joinable and says why");
}

/* Data frames carry sequence numbers too. The management-frame fix was the
 * visible half; the data plane is the one that actually feeds a duplicate
 * detector in volume. */
void test_data_seq() {
  const uint8_t dest[6] = {1, 2, 3, 4, 5, 6};

  std::vector<uint8_t> a = data_hdr_to_ds(kBssid, kOwn, dest, true, 1);
  check(a[22] == 0x10 && a[23] == 0x00, "uplink carries its sequence number");
  std::vector<uint8_t> b = data_hdr_from_ds(kOwn, kBssid, dest, false, 0x0fff);
  check(b[22] == 0xf0 && b[23] == 0xff, "downlink carries its sequence number");
  std::vector<uint8_t> c = data_hdr_to_ds(kBssid, kOwn, dest, true);
  check(c[22] == 0 && c[23] == 0, "the default is still zero");
  std::vector<uint8_t> d = data_hdr_to_ds(kBssid, kOwn, dest, true, 0x1001);
  check(d[22] == 0x10 && d[23] == 0x00, "a data sequence wraps at 12 bits");
}

}  // namespace

int main() {
  test_mgmt_hdr();
  test_seq();
  test_ie_walk();
  test_golden_bytes();
  test_rsn();
  test_beacon_roundtrip();
  test_station_builders();
  test_assoc_resp_parse();
  test_data_frames();
  test_rsn_real_world();
  test_data_seq();

  if (g_fail) {
    std::printf("dot11_selftest: %d failure(s)\n", g_fail);
    return 1;
  }
  std::printf("dot11_selftest: OK\n");
  return 0;
}
