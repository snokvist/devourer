"""Turn a hwsim capture into C test vectors for src/sta/Ccmp.h.

This script does NO crypto.  It only cuts the radiotap header off, picks one
protected QoS data frame per (direction, TID), and writes the raw bytes out.
That is deliberate: if the vectors' expected values came from a Python
reimplementation of CCMP they would encode the same reading of the spec the
header does, which is exactly how the zero-flags nonce stayed green through a
full vector suite.  Here the expected value IS the byte stream the Linux
kernel put on the air, and the oracle is the MIC.
"""
import struct
import sys
from collections import OrderedDict

pcap = sys.argv[1]
tk = bytes.fromhex(sys.argv[2])
out_path = sys.argv[3]
assert len(tk) == 16


# radiotap field (size, alignment) by present-bit, enough to reach Flags (bit
# 1) past whatever else a capture happens to carry. hwsim emits two different
# present words in one capture, so the offset of Flags is NOT fixed.
_RT_FIELDS = {
    0: (8, 8), 1: (1, 1), 2: (1, 1), 3: (4, 2), 4: (2, 2), 5: (1, 1),
    6: (1, 1), 7: (2, 2), 8: (2, 2), 9: (2, 2), 10: (1, 1), 11: (1, 1),
    12: (1, 1), 13: (1, 1), 14: (2, 2), 15: (2, 2), 16: (1, 1), 17: (8, 4),
    18: (3, 1), 19: (8, 4), 20: (12, 2), 21: (12, 8),
}


def radiotap_flags(pkt):
    """The radiotap Flags octet, or None when the header does not carry it."""
    rt_len = struct.unpack('<H', pkt[2:4])[0]
    words, off = [], 4
    while True:
        w = struct.unpack('<I', pkt[off:off + 4])[0]
        words.append(w)
        off += 4
        if not (w & 0x80000000):
            break
    if not (words[0] & 0x02):
        return None
    for word_idx, w in enumerate(words):
        for bit in range(31):
            if not (w & (1 << bit)):
                continue
            idx = word_idx * 32 + bit
            if idx not in _RT_FIELDS:
                return None          # an extension we cannot skip safely
            size, align = _RT_FIELDS[idx]
            pad = (-(off - 0)) % align
            off += pad
            if idx == 1:
                return pkt[off]
            off += size
            if off > rt_len:
                return None
    return None


blob = open(pcap, 'rb').read()
magic = blob[:4]
if magic == b'\xd4\xc3\xb2\xa1':
    endian = '<'
elif magic == b'\xa1\xb2\xc3\xd4':
    endian = '>'
else:
    raise SystemExit('not a classic pcap: %r' % magic)
link = struct.unpack(endian + 'I', blob[20:24])[0]
assert link == 127, 'expected DLT_IEEE802_11_RADIOTAP, got %d' % link

off = 24
picked = OrderedDict()
seen = 0
while off + 16 <= len(blob):
    _, _, incl, _ = struct.unpack(endian + 'IIII', blob[off:off + 16])
    off += 16
    pkt = blob[off:off + incl]
    off += incl
    if len(pkt) < 8:
        continue
    rt_len = struct.unpack('<H', pkt[2:4])[0]
    flags = radiotap_flags(pkt)
    # A capture that appends the FCS would make every MPDU four bytes long and
    # every MIC check fail - the exact failure tests/rx_mpdu.h is about.
    assert flags is not None and not (flags & 0x10), \
        'radiotap says the frame carries an FCS; trim it before extracting'
    mpdu = pkt[rt_len:]
    if len(mpdu) < 26:
        continue
    fc0, fc1 = mpdu[0], mpdu[1]
    if fc0 != 0x88:          # QoS data, subtype 8
        continue
    if not (fc1 & 0x40):     # Protected
        continue
    tods, fromds = bool(fc1 & 0x01), bool(fc1 & 0x02)
    if tods == fromds:       # neither 4-address nor IBSS here
        continue
    tid = mpdu[24] & 0x0f
    a1 = mpdu[4:10]
    if a1[0] & 0x01:         # group frames use the GTK, not this TK
        continue
    seen += 1
    key = ('up' if tods else 'down', tid)
    if key in picked:
        continue
    # a2 is the transmitter address for a 3-address frame either way.
    picked[key] = dict(mpdu=mpdu, a2=mpdu[10:16], tid=tid,
                       direction=key[0])

if len(picked) != 16:
    raise SystemExit('wanted 16 vectors (2 directions x 8 TIDs), got %d '
                     'from %d candidate frames' % (len(picked), seen))


def carr(b):
    body = ', '.join('0x%02x' % x for x in b)
    lines, cur = [], '    '
    for tok in body.split(', '):
        if len(cur) + len(tok) + 2 > 76:
            lines.append(cur.rstrip())
            cur = '    '
        cur += tok + ', '
    lines.append(cur.rstrip().rstrip(','))
    return '\n'.join(lines)


with open(out_path, 'w') as f:
    f.write('''/* ccmp_kernel_vectors.h - CCMP frames produced by the LINUX KERNEL.
 *
 * GENERATED, do not edit by hand. See tests/ccmp_capture_vectors.sh, which
 * builds a two-radio mac80211_hwsim rig, runs hostapd (WMM on) and
 * wpa_supplicant over it, sends one datagram per 802.11 user priority in each
 * direction, and cuts these bytes out of the capture. No hardware.
 *
 * WHY THESE EXIST. tests/ccmp_vectors.h pins the CIPHER against a third
 * implementation, but its generator and src/sta/Ccmp.h share one author's
 * reading of the framing rules - which is precisely how a CCM nonce with a
 * zero flags octet stayed green through the whole suite. These vectors are
 * not a second reading of the specification. They are the actual bytes
 * mac80211 put on the air, with the MIC as the oracle: if our AAD or nonce
 * differs from the kernel's by a single bit, the tag does not verify.
 *
 * The TID is the point. The devourer AP advertises neither WMM nor HT, so no
 * station ever sends it a QoS data frame and TID 1..7 were unreachable on the
 * bench. Here all eight appear, in both directions.
 *
 * KEY MATERIAL IS FROM A THROWAWAY LAB PSK ("ccmpvectors123") on virtual
 * radios; it protects nothing.
 */
#ifndef DEVOURER_TEST_CCMP_KERNEL_VECTORS_H
#define DEVOURER_TEST_CCMP_KERNEL_VECTORS_H

#include <cstddef>
#include <cstdint>

namespace devourer {
namespace test {

/* The pairwise TK wpa_supplicant derived for this association. */
static const uint8_t kKernelTk[16] = {
''')
    f.write(carr(tk))
    f.write('''
};

struct KernelCcmpVector {
  const char* name;      /* direction and TID, for a failure message */
  uint8_t tid;           /* the TID in the QoS control field */
  size_t hdr_len;        /* 26: a 3-address QoS data header */
  const uint8_t* mpdu;   /* the whole protected MPDU, no FCS */
  size_t mpdu_len;
};

''')
    names = []
    for (d, tid), v in picked.items():
        sym = 'kKv_%s_tid%d' % (d, tid)
        names.append((sym, d, tid, len(v['mpdu'])))
        f.write('static const uint8_t %s[] = {\n%s\n};\n\n'
                % (sym, carr(v['mpdu'])))
    f.write('static const KernelCcmpVector kKernelCcmpVectors[] = {\n')
    for sym, d, tid, n in names:
        f.write('    {"%s tid %d", %d, 26, %s, %d},\n' % (d, tid, tid, sym, n))
    f.write('''};

static const size_t kKernelCcmpVectorCount =
    sizeof(kKernelCcmpVectors) / sizeof(kKernelCcmpVectors[0]);

}  // namespace test
}  // namespace devourer

#endif /* DEVOURER_TEST_CCMP_KERNEL_VECTORS_H */
''')
print('wrote %d vectors from %d candidate frames -> %s'
      % (len(picked), seen, out_path))
