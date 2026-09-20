# Station mode — scope, MT7612U first

devourer can *be* an access point (`docs/ap-mode.md`, `docs/mt7612u-ap-mode.md`).
It cannot *join* one. This document scopes the other half: devourer as an
infrastructure **station** — scan, authenticate, associate, WPA2-PSK 4-way as
the supplicant, encrypted data plane — on the MT7612U, with the one seam that
makes a Realtek arm a port rather than a rewrite.

It began as a scope with nothing implemented. Phases 1 and 2 are now closed
on hardware — see `docs/station-mode-handoff.md` for status and
`docs/mt7612u-station-identity.md` for the measurements, whose retraction
section should be read before any figure from it is quoted. The target the
pair serves was decided on 2026-09-20 and is recorded below under "The target:
an ordinary BSS, with the AP bridging".

## Prior art, and what it tells us

### PR #335 — `apfpv-station-mode`, closed, withdrawn

A third-party PR (July 2026) added a complete WPA2/CCMP station on Jaguar1:
scan → auth → assoc → 4-way → DHCP → stream, +6114 lines over 39 files, two
new CMake targets (`apfpv`, `apfpv_station`). It was bench-validated on an
RTL8812AU against a real OpenIPC AP. The author closed it after review
("the approach here isn't right for upstream").

The review is the useful part, because every item is a constraint on any
successor:

| # | Review finding | Constraint it sets |
|---|---|---|
| 1 | EAPOL-Key MIC never verified (M3, rekey) | **KRACK-class.** Verify the MIC before any state change, always |
| 2 | Replay gate used `<`, not `<=` | Equal-counter replay must be rejected |
| 3 | MSVC build red (`min`/`max` macro collision) | Windows is first-class; CI gates it |
| 4 | Android NDK build red (missing includes under `#if __ANDROID__`) | Host builds preprocess the block away and hide it |
| 5 | 40+ `getenv` sites in library sources | *The library reads no environment* — `DeviceConfig` + `examples/common/env_config.cpp` |
| 6 | `fprintf(stderr)` throughout, one printing GTK bytes | Two-plane logging; never log key material |
| 7 | Generation-agnostic core reached into `jaguar1/`, hardcoded 8812AU registers | Backend-specific work goes behind `IRadio`, not into shared code |
| 8 | Assoc-request truncated its HT/VHT/ExtCap tail on the non-default cipher path | Frame builders need unit coverage |
| 9 | Hand-rolled AES/CCM/PBKDF2/PRF with **zero KATs** | Highest-leverage gap named in the review |
| 10 | WIP AP-side authenticator shipped in the same PR | One unverified 4-way per PR, not two |

Items 1, 2 and 9 are the ones that matter most: a station's handshake is the
only place in this project where a bug is a *security* bug rather than a wrong
number.

The PR also carried two genuinely valuable hardware findings that are
independent of its architecture, and worth salvaging on their own:

- `SetStationRxFilter` — the kernel-matched RCR/RXFLTMAP a Realtek station
  needs; the permissive monitor filter drops management frames the auth/assoc
  handshake depends on.
- A close-range **DIG saturation** fix: once linked, track the receiver's gain
  floor/ceiling off the live beacon RSSI instead of the always-monitor-mode
  bounds, or a strong nearby AP storms the false-alarm counter and the
  front-end goes deaf — a trivially strong link that receives nothing.

Provenance note: the branch is a closed PR from an outside contributor who
withdrew it. Treat it as a reference implementation to read, and agree
attribution with the author before lifting code.

### The AP work — what it proves about portability

`docs/mt7612u-ap-mode.md` records the load-bearing result: `tests/ap_responder.cpp`
and `tests/ap_wpa2.cpp` run **unchanged** on MT7612U. No backend branch, no
`#ifdef`. They need exactly five `IRadio` methods — `InitWrite`, `StartBeacon`,
`StartRxLoop`, `send_packet`, `StopBeacon` — and no others.

That is the portability architecture, and it already exists. A station does not
need a plugin layer or a per-backend station class; it needs the same five
methods plus **one new pair**, and then it too runs unchanged on any backend
that implements them. Section "The one backend seam" below is that pair.

Everything AP-side lives in `tests/` as harnesses, not as library API — a
deliberate scope statement in `docs/ap-mode.md`. The station should land the
same way, for the same reason.

### Issue #425 and the standing key-API decision

MT7612U has per-station CCMP key hardware and devourer reaches none of it
(`MT_SKEY`/`MT_SKEY_MODE` are defined and zeroed at init; `MT_WCID_KEY` and
`MT_WCID_IV` are not even declared in `src/mt7612u/regs.h`). The maintainer's guidance on #424 is to **not**
design an `IRadio` key surface off one backend — key lifetime, GTK vs PTK,
rekey and replay-counter ownership are a much larger contract than a feature
flag, and expensive to undo once callers exist.

The local `bench/mt7612u-ccmp` branch (uncommitted at the time of writing:
`docs/mt7612u-ccmp-benchmark.md`, `tests/ccmp_software.h`, `tests/ccmp_sw_bench.cpp`)
closes the throughput half of that question on x86: fresh-context OpenSSL
AES-CCM costs 0.48/0.47 µs per 64-byte frame and 1.26 µs per 1500-byte frame,
about **0.47% of one core per direction** at the MT7612U's measured
44.55 Mbit/s injected-data ceiling. The on-air cell measured 1.88 µs TX /
3.47 µs RX per frame — about 16 ms of cipher across a 10-second traffic
window, with AP process load 2.4% of a core. The adversarial counterpart: this
is an x86 NucBox with OpenSSL AES-NI, and says nothing about an ARM SoC
without crypto extensions.

**Consequence for this scope: station mode is built entirely on software CCMP,
and adds no key API.** That is not a compromise — it is the measured answer,
and it removes the single largest piece of work from the critical path.

## What a station needs that an AP does not

| Phase | AP side (exists) | Station side (new) |
|---|---|---|
| Discovery | airs beacons, answers probe requests | **scans**: dwell per channel, parse beacons/probe responses into a BSS table; or skip it on a configured channel+BSSID |
| Identity | `StartBeacon` programs MACID/BSSID as a side effect | **nothing arms it** — a station has no beacon. See "the one backend seam" |
| RX filter | monitor filter (deliberately permissive) | managed-station filter: own-BSS only, DUP handling decided |
| Auth/assoc | parses requests, builds responses | builds requests, parses responses (mirror image) |
| WPA2 | 4-way **authenticator** (`tests/ap_wpa2.cpp`) | 4-way **supplicant** — same primitives, opposite role, and the half where a bug is a CVE |
| Data plane | DHCP/ARP/ICMP **server** | DHCP **client**, ARP/ICMP client |
| Supervision | n/a (clients come and go) | beacon-loss timeout, deauth handling, reconnect |
| TSF | owns it, stamps beacons | would adopt the AP's — **explicitly out of scope**, see below |

## The target: an ordinary BSS, with the AP bridging

Decided 2026-09-20. Until now this document scoped station mode as *driver
capability* — "devourer can be an AP; it cannot join one" — and never said what
the pair is for. That was adequate through Phase 2 and is not adequate for
Phase 3, because the data plane's shape falls out of it.

**The target is a normal AP/station setup**, with the AP relaying between its
associated stations. Not a bespoke point-to-point arrangement, and not raw-frame
parity with the AP half.

**With one stated exception, and it is not a small one: power save must be off
on the client.** An earlier draft of this section said "a BSS that an unmodified
client joins and uses", which is not what the fleet policy below delivers and
quietly reversed a measured position. `docs/ap-mode.md:121-137` records
`power_save on` giving **0/60** pings with the link dropping mid-run, and says
in as many words that the 14/14 gate "does **not** certify these harnesses
against a default-configured Linux station, because Linux defaults to power
save on". `docs/mt7612u-ap-mode.md:196-201` already chose the same answer:
advertise DTIM=1, buffer nothing, **do not support power-saving clients**.

So the honest target is **an ordinary BSS for clients we configure**. Serving a
genuinely unmodified client means implementing DTIM buffering, which stays out
of scope. Do not let "ordinary BSS" drift back into "any client".

This section was adversarially reviewed twice on the day it was written, and
roughly half of its first draft was wrong. What follows is the corrected
version; the subsection "What the review changed" at the end records the
errors, because several were the kind that read as reasonable.

### What that means in 802.11 terms

An AP is not a transparent Ethernet switch. The address layout differs by
direction, so relaying A → B is a header rewrite:

| direction | addr1 (RA) | addr2 (TA) | addr3 |
|---|---|---|---|
| station A → AP (**to-DS**) | BSSID | A (SA) | B (DA) |
| AP → station B (**from-DS**) | B (DA) | BSSID | A (SA) |

Both reviewers verified this against 802.11-2016 Table 9-26. Two gaps in the
code, and **both exist in both data-plane harnesses** — the encrypted one is
the relevant target, since the BSS is encrypted:

- **TX needs no library change.** `sta::data_hdr_from_ds(sta, bssid, src, ...)`
  (`src/sta/Dot11.h:583`) already takes the source address as a parameter. Both
  callers pass `kBssid` — `tests/ap_wpa2.cpp:385-386` and
  `tests/ap_responder.cpp:92` — so every frame claims the AP as originator.
  Relaying means passing A's address instead.
- **RX has no destination at all.** `tests/ap_wpa2.cpp:465-466` and
  `tests/ap_responder.cpp:213-214` read addr1 and addr2 and never read addr3,
  so the AP cannot distinguish "addressed to me" from "addressed to another
  station". There is no relay path because there is no destination to branch
  on.

### The part that actually costs: you cannot relay ciphertext

A frame from A is encrypted under **A's PTK**. Delivering it to B means
decrypting with A's PTK and re-encrypting with **B's PTK**, under a fresh PN in
B's TX space. The CCMP AAD authenticates addr1/addr2/addr3
(`src/sta/Ccmp.h:86`) and the nonce carries A2 (`src/sta/Ccmp.h:112-117`), so
the rewritten header invalidates both — ciphertext cannot be passed through.
So the per-station table is not merely a multi-client feature; it is the
precondition for switching at all.

**Uplink is always pairwise, including for broadcast payloads.** A non-AP
station addresses every frame to the AP, so addr1 = BSSID, which is
individually addressed at L2 no matter what addr3 says. Group keys protect
frames whose *RA* is a group address, which only the AP's downlink produces.
The evidence is in-tree: there is exactly one decrypt path,
`ccmp_decrypt(..., g_ptk + 32, ...)` at `tests/ap_wpa2.cpp:518`, and encrypted
DHCP already works — DHCP DISCOVER carries a broadcast DA and is decrypted
with the pairwise TK. One review argued the up path should select the GTK for
group traffic; it should not, and this paragraph exists so the point is not
re-litigated.

### How many stations: an inference, not a measurement

There is no hardware obstacle, and the AP-side record already says so with
numbers: **the WCID table is 256 entries and SKEY is per-BSS in hardware, so
the silicon supports many clients; extending is harness work, not driver work**
(`docs/mt7612u-ap-mode.md:225-228`). A first draft of this section hedged this
into an inference while failing to cite that record at all — the hedge was
misplaced, because R8's open WCID question is about the **station** role, and
this target is the AP role.

Two honest qualifications remain. The auto-ACK mechanism itself (one ACK-match
address serving any number of stations, since every station sets RA = the AP)
was measured with **one peer** (`docs/mt7612u-station-identity.md`). And
`docs/mt7612u-ccmp-benchmark.md:152-158` lists "several concurrent stations" as
one of the triggers for revisiting the software-CCMP decision — that is a
trigger to re-measure the CPU cost, not a claim that multi-client needs
hardware keys.

What is certain is that the single-station state is software, and that it is
spread across **three** harnesses, not one: `tests/ap_wpa2.cpp:107-115`
(`g_sta`, `g_ptk`, `g_gtk`, `g_state`, `g_replay`), `tests/ap_responder.cpp`,
and `tests/ul_trigger_ap.cpp:61,65` (its own `g_sta`, its own single lease).

### The GTK moves from "future work" to prerequisite

Group-addressed downlink is encrypted with the **GTK**, which today is
generated and delivered in msg3's GTK KDE (`tests/ap_wpa2.cpp:302, 306`) and
then **never used to encrypt anything** — those three lines are its only
references. A client installs a group key that nothing will ever arrive under.

**ARP is broadcast**, so station-to-station IP never gets off the ground
without a GTK transmit path. Three concrete defects to fix, not one:

1. **Key id.** Msg3 advertises the GTK at key id 1 (`tests/ap_wpa2.cpp:305`),
   while the only data TX path hardcodes key id 0 with the pairwise key
   (`tests/ap_wpa2.cpp:394-396`). A group frame sent at key id 0 is looked up
   as the pairwise key at the station and fails its MIC.
2. **One GTK per BSS.** `g_gtk` is regenerated on every 4-way
   (`tests/ap_wpa2.cpp:302`), so a second station's handshake silently revokes
   the first station's group key.
3. **A group PN space**, separate from every pairwise one.

**What still breaks without it**, stated precisely: IPv6 ND (NS/RS/RA), DHCPv6,
mDNS, SSDP. **DHCPv4 does not break under the current gateway model** — the
userspace responder sets addr1 = the station (`tests/ap_responder.cpp:92-93`,
`tests/ap_wpa2.cpp:385-386`) even though the IP destination is broadcast, so
the frame is individually addressed and pairwise-protected. That stops being
true the moment step 8's TAP fronts a real DHCP server: its OFFER leaves
group-addressed at L2 and needs the GTK path. The claim is scoped to today's
model, and an earlier draft stated it unscoped.

**Proxy ARP is not the escape hatch the first draft claimed.** Two reasons.
There is no IP-to-MAC binding table to answer from: the DHCP server leases a
single hardcoded `192.168.99.2` to whichever station asks
(`tests/ap_wpa2.cpp:404`, `tests/ap_responder.cpp:101`), with no pool. And
RFC 1027 proxy ARP answers with the *proxy's own* MAC, which makes the AP an
L3 next hop — a Linux bridge delivers a frame addressed to its own MAC to the
local stack, not out another port, so it would not exercise the relay path at
all. The correct cheap substitute is an **ARP responder keyed on the
association table that answers with B's real MAC**. That is a different
mechanism, it does decouple the relay work from the GTK work, and it still
requires a real address pool first.

### Do not build a switch — but the kernel will not be one either

The first draft proposed a TAP in a Linux bridge and claimed intra-BSS relay
would "fall out of the kernel bridge at no extra cost". **Both reviewers
independently showed that is false**, and the reason is decisive: a Linux
bridge never forwards a frame back out its ingress port — for flooded frames as
well as learned ones, so it does not depend on B having been learned yet. With
one TAP carrying the whole BSS, A → B arrives on the only port B could be
reached through, and the bridge will not send it back out. There is no
intra-BSS relay from the bridge.

One TAP *per station* would give the bridge distinct ports to switch between,
but then the DA lookup is redundant (the port identifies the station) and a
flooded broadcast is delivered once per TAP, so the AP would air the same group
frame N times unless it coalesces. It also needs TAP creation on association.

**Decision: one TAP for the BSS. Intra-BSS relay is ours.** The topology is:

- **up:** decrypt with the sending station's PTK, strip the 802.11 header,
  emit 802.3. If the DA is another associated station, relay it directly
  (below). Otherwise push it to the TAP.
- **down:** pull 802.3 from the TAP, look up the DA in the association table,
  encrypt with that station's PTK — or the GTK for a group address — and send
  from-DS.
- **intra-BSS:** association-table lookup plus a re-encrypt. This is devourer's
  forwarding logic and there is no honest way to describe it as free. It is
  also what real APs do rather than bouncing through the bridge, which is why
  `ap_isolate` exists as a knob to disable it.

What the TAP genuinely buys is **host-stack access and an upstream port** — not
switching. The association table is the forwarder; saying otherwise was the
first draft's central error.

**The TAP collides with the userspace IP responders the moment it exists**, and
this is not only a bridging problem. As soon as a TAP carries the BSS, the host
stack answers ARP and ICMP for that interface alongside the responders in
`tests/ap_responder.cpp` and `tests/ap_wpa2.cpp`. Step 8 must say which one
owns each protocol, even in the gateway model where no upstream port exists.

**The gateway/bridge models are not interchangeable.** Today the AP *is* the
network: its own subnet `192.168.99.0/24`, a userspace DHCP server, and ARP and
ICMP responders for its own IP. A transparent bridge to an upstream interface
is a different model — the BSS joins the upstream subnet, and those userspace
responders become either dead code or conflicting L2 endpoints. Bridging
upstream therefore does **not** cost nothing; it is a replacement for the
current gateway, and `docs/ap-mode.md` still lists routing/NAT as out of scope.
Which model the product wants is **not decided here.**

**Where the code lives: `tests/`, or `tools/` — not `examples/`.** The first
draft argued for `examples/` on the grounds that a Linux-only, `CAP_NET_ADMIN`
binary should stay out of the library. The conclusion was right and the
destination was wrong: `examples/` targets are declared in the root
`CMakeLists.txt` and are the cross-platform, CI-built plane, which is exactly
where a Linux-only privileged binary forces the conditional compilation the
argument set out to avoid. `tests/ap_wpa2.cpp` and `ap_responder.cpp` are not
CMake targets at all — they are hand-built — which is what makes `tests/` a
natural home, alongside `sta_client.cpp`. That argument is weaker than the
`examples/` half: `tests/` is not purely hand-built either (`CMakeLists.txt`
builds `reglat` and the ctest selftests from it). `tools/`, where
`tun_p2p.py` already lives, is the other defensible answer.

There is also prior art the first draft missed: **`tools/precoder/tun_p2p.py`**
already opens `/dev/net/tun` under `CAP_NET_ADMIN` (`:147-151`) and drives
`streamtx` / `rxdemo` as subprocesses (elsewhere in the same file). It is TUN (L3) rather than TAP, and it
documents its own single-peer scope. So the repo's established home for
privileged host-side netdev glue is `tools/`, and a single-peer forwarder is
already the accepted shape for this kind of tool.

The one piece that belongs in `src/sta/` is the pure 802.11 ↔ 802.3
translation. Less of it exists than the first draft claimed:
`sta::append_llc_snap` (`src/sta/Dot11.h:557`) encodes the 8-byte LLC/SNAP
header and `tests/ap_responder.cpp:251-257` decodes it, but **neither builds
nor parses the 14-byte Ethernet II header**, which is the actual work. Android
needs the identical conversion and can never have a netdev (no `CAP_NET_ADMIN`
without root; `VpnService` is layer-3, needs consent, and captures the whole
device's traffic), so the helper has two consumers either way.

### Frame-level machinery the relay needs, and does not have

"The payload is unchanged — strip the header, push 802.3" is true only for a
simple, unfragmented, non-aggregated MSDU:

- **Duplicate detection.** Relaying without a per-TA/TID sequence cache
  (802.11-2016, "Duplicate detection and recovery" — clause number
  deliberately not quoted here; an earlier draft cited §10.3.4.6 and review
  could not confirm it) forwards A's retransmission to B a second time
  whenever the AP's ACK to A is lost. The phase table above asserts "DUP
  handling decided"; the decision is not stated anywhere. And hardware will
  not do it for us: BIT(7) `MT_RX_FILTR_CFG_DUP` is set in the init value, but
  `mt7612u_set_monitor_rx()` is what clears it
  (`src/mt7612u/beacon.cpp:494-498`) and `StartRxLoop` calls that
  unconditionally for every role (`src/mt7612u/Mt7612uRadio.cpp:410-412`). So
  the AP harnesses run with DUP **clear, always** — a software per-TA/TID
  cache is the answer, and nothing in the order of work currently turns the
  hardware filter on either.
- **Fragmentation.** A fragmented MSDU must be reassembled before it becomes an
  802.3 frame. `Ccmp.h:88` deliberately preserves the fragment number in the
  AAD, so the information is there; nothing uses it.
- **A-MSDU.** A QoS frame with the A-MSDU Present bit carries *several* 802.3
  subframes, not one payload.

Each needs an implementation or an explicit, documented refusal.

### A CCMP defect this design would have inherited — FIXED 2026-09-20

**Fixed as Phase 2b.1.** `ccmp_nonce()` now takes the header, exactly as
`ccmp_aad()` does, and derives the Flags octet from it: Priority (the QoS TID)
in b0..b3, Management in b4. Both call sites fail closed on a header too short
for what its frame control claims. `tests/ccmp_gen_vectors.py` carried the
identical misreading and was fixed with it.

**Evidence, and its limit.** Regenerating the vectors changed **only** the two
QoS vectors (`qos_tid5`, `four_addr_qos`) — every non-QoS vector is
byte-identical, so the change is confined to the frames the defect touched.
Reintroducing `nonce[0] = 0` as a mutation fails eight checks: four vector
cells and four direct assertions in the new `test_nonce_flags()`. The
round-trip cell passes under the mutation, which is the point — encrypt and
decrypt share the wrong nonce, so a round trip is structurally blind to this.

**What is NOT proven: interop.** The fix is verified by construction and by
mutation, not against another implementation. The honest reason is worth
recording, because it is also why the defect survived: **this AP advertises
neither WMM nor HT** (no WMM IE anywhere in `tests/ap_responder.cpp` or
`tests/ap_wpa2.cpp`, and `kEidHtCaps` is defined in `src/sta/Dot11.h:72` but
never used), so a conforming station associates non-QoS and never sends a QoS
frame. The one test that could have caught this independently cannot run
against this AP at all. Until WMM is advertised, the Annex J vectors are the
only available independent check, and an on-air cell with a station sending
TID 1..7 is the gate that should be attached to the WMM work.

### The defect, as originally recorded

Found by the protocol review, pre-existing and **not** introduced by this
decision, but load-bearing for it:

`ccmp_nonce()` (`src/sta/Ccmp.h:112-117`) sets the nonce flags octet to 0
unconditionally. 802.11-2016 §12.5.3.3.4 defines it as Priority (b0..b3) |
Management (b4) — Linux builds it as `qos_tid | (is_mgmt << 4)`. The AAD *does*
carry the TID (`Ccmp.h:101-103`); only the nonce omits it.

**This is a live defect in merged code, not a latent one.** Phase 1
deliberately fixed the *AAD* half of exactly this bug —
`tests/ap_wpa2.cpp:508-515` carries the TID into the AAD and its comment says
the previous behaviour "is wrong for QoS and survived only because the
validated runs used a station that associated legacy and sent non-QoS data. A
real 802.11n station sends QoS, and every one of its frames would have failed
the MIC with no diagnostic at either end." The nonce half of the same bug was
missed, so that sentence is **still true today**: a TID 1-7 QoS frame still
fails its MIC with no diagnostic.

It is masked only by the rig: the AP airs non-QoS data
(`tests/ap_wpa2.cpp:388`), ordinary client traffic is TID 0 (for which flags 0
is coincidentally correct), and the 14/14 gate ran against a legacy,
non-QoS station — so the gate is structurally unable to see it. It surfaces on
TID 1-7, **the video and voice access categories**, which is the traffic this
project exists to carry, and the exposure widens under this target from our own
TID-0 fleet to any client with WMM.

The KATs cannot catch it. `tests/ccmp_gen_vectors.py:109-111` builds the nonce
as `bytes([0]) + a2 + pn`, the same misreading as the C, so the vectors are
self-consistent with the defect. The selftest's own header claims the framing
was "transcribed ... independently of the header under test" — but independence
of *implementation* (python-cryptography vs OpenSSL) is not independence of
*interpretation*, and the same misreading was made on both sides. The header
already names the fix: the official IEEE Annex J vectors, "a drop-in
replacement when someone has it to hand".

A second consequence is genuinely latent. The standard keeps PN
counters **per TID**; this tree uses one counter (`tests/ap_wpa2.cpp:368`), so
no nonce repeats today. Adopting per-TID PNs — which the per-station table work
invites — without fixing the nonce would make two frames on different TIDs
share a nonce under the same key and A2, which is CCM keystream reuse. **Fix
the nonce before introducing per-TID PN counters.**

### Power save off is a fleet policy, and it composes

Group-addressed delivery normally carries a second obligation: buffer group
frames and release them after DTIM beacons. **Power save is off by policy on
this fleet**, so group frames can be transmitted immediately and that
obligation does not arise. The policy makes the GTK work materially smaller
than it would otherwise be.

It is enforceable on anything we control (`iw dev X set power_save off`,
NetworkManager `wifi.powersave=2`, our own embedded stations). It is not
enforceable on an unrooted phone or a third-party device, so **provisioning a
craft from an arbitrary phone is not a supported use case** unless buffering is
implemented later.

Because it is policy rather than accident, the AP should fail *loudly* when it
is violated instead of silently shedding traffic. `tests/ap_wpa2.cpp:503`
already tests `fc1 & 0x40` for PROTECTED; the PM bit is `fc1 & 0x10` at the
same site. One branch converts "0/60 pings and the link drops" into a named
diagnostic.

### The station RX filter — a Phase 2 concern, not a Phase 3 one

A bridging BSS wants own-BSS filtering rather than the permissive monitor
filter, and `StartRxLoop` installs the monitor value unconditionally for every
role (`src/mt7612u/Mt7612uRadio.cpp:412`). The first draft called changing it
"the first thing Phase 3 should change". That is wrong twice over:

- Phase 3 is defined as "pure station logic, **offline**"
  (`docs/station-mode-plan.md:562`). A backend register change is Phase 2
  territory.
- Changing `StartRxLoop` unconditionally would regress every monitor consumer
  and the AP harnesses, and "The one backend seam" below already weighed this
  and chose the `SetStationIdentity` ordering contract precisely to keep the AP
  path byte-identical.

So the filter belongs under the existing seam contract, as a role-selected
value — not as an edit to the shared RX entry point.

### Order of work

These are now **Phase 2b** in `docs/station-mode-plan.md`, with a gate and
acceptance evidence per item and a structural property for the phase. Recording
them only here, as the first draft did, violated that document's own rule 4
(`docs/station-mode-plan.md:32-33`) — eight steps, three of them
security-relevant, with no gate. The list below is the rationale; the plan is
the authority on ordering and acceptance.

1. **Fix the CCMP nonce** (`src/sta/Ccmp.h:113`) and re-source the vectors from
   IEEE Annex J. **First, not second** — an earlier draft had this second,
   behind the per-station table, which is the one ordering that can *create*
   the hazard this step removes: the table's "TX PN" invites per-TID counters,
   and per-TID PNs over a TID-blind nonce is keystream reuse. If for any reason
   the table lands first, it must pin **one PN per station, never per TID**,
   until this step is done.
2. **Per-station table** — association state, AID, PTK, TX PN, and a
   `CcmpReplay` instance each, across all three harnesses. Precondition for
   everything below. Per-station PN and replay state deserve the same care as
   the 4-way: a mistake there is a security bug, not a wrong number. **Home:**
   `src/sta/`, under the same purity contract as the rest of that directory —
   pure, backend-agnostic, ctest-gated, no `IRadio` and no OS calls. It is the
   largest item here and the first draft failed to say where it goes.
3. **Pass the real SA** and **read addr3** in `ap_wpa2.cpp` (and
   `ap_responder.cpp`). Cheap; unblocks relay.
4. **A real DHCP address pool** with a binding table.
5. **An association-table ARP responder** that answers with the target
   station's real MAC. Named at "Proxy ARP is not the escape hatch" as the
   correct cheap substitute, and the first draft's step list forgot to build
   it. Depends on 4.
6. **GTK transmit path**: key id 1, one GTK per BSS, its own PN space.
7. **Intra-BSS relay** — association-table lookup plus re-encrypt, with
   duplicate detection and an explicit position on fragmentation and A-MSDU.
8. **TAP forwarder** in `tests/` or `tools/`, with the 802.11 ↔ 802.3
   translation (including the Ethernet II header) as a pure helper in
   `src/sta/`.

### What this decision does NOT settle

- **Gateway or transparent bridge.** The AP is a gateway today; bridging
  upstream replaces that model and migrates the userspace DHCP/ARP/ICMP
  responders. Undecided.
- **Whether multi-client is really only a software problem.** An inference from
  a single-peer measurement; R8's WCID question is open.
- **Latency, CPU and copy budget.** Every relayed frame costs a decrypt, a
  re-encrypt, and — when it goes via the TAP — two more kernel transitions, on
  top of the two userspace traversals software CCMP already forces. The only
  throughput evidence in this document is x86 with AES-NI and says nothing
  about an ARM SoC without crypto extensions. Unquantified, and the largest
  unmeasured risk in the design.
- **Whether the target kernels have TUN/TAP compiled in.** Unverified on the
  SigmaStar/OpenIPC builds.
- **Android has no bridge**, so its in-process path needs a small explicit
  relay. Realistically one peer, so a lookup rather than a switch.
- **The TIM bitmap is one byte.** `sta::append_tim` sets `1u << aid` for AID
  1..7 only (`src/sta/Dot11.h:254-255`). Real client counts need the partial
  virtual bitmap with an offset. Latent while power save is off.
- **4-address frames are not required** for plain station clients
  (`tests/ap_wpa2.cpp:534-537` is already aware of them). But a station that
  *itself* bridges sends an SA that is not its own MAC, and that case needs
  4-address frames or MAC learning on the wireless side. Out of scope, and the
  reason the "an AP needs no MAC learning" claim is only true for plain
  clients.
- **No client-count target.** "More than one" is the bar; how many the software
  path sustains is unmeasured. Note the FPV path itself is point-to-point, so
  multi-client is an operator requirement rather than something the video use
  case demands — recorded so the minimal-implementation rule is applied with
  that in view.

### What the review changed

Two adversarial reviews ran against the first draft. Every finding below was
verified against the tree before being accepted; one was rejected.

| # | First draft said | Corrected to |
|---|---|---|
| 1 | Intra-BSS relay falls out of the kernel bridge at no cost | A bridge never forwards out its ingress port — with one TAP there is no relay at all. The association table is the forwarder |
| 2 | "No forwarding table, learning or ageing of our own" | Contradicted the DA lookup two lines above it. Removed |
| 3 | The TAP belongs in `examples/` | `examples/` is the CMake cross-platform plane — the worst place for a Linux-only privileged binary. `tests/` or `tools/`, per `tun_p2p.py` |
| 4 | Only `ap_responder.cpp:92` claims the AP as originator | `ap_wpa2.cpp:385-386` does too, and is the relevant harness |
| 5 | Single-station state is "one harness" | Three: `ap_wpa2`, `ap_responder`, `ul_trigger_ap` |
| 6 | Multi-client is "purely software, no hardware obstacle" | An inference from a one-peer measurement; R8's WCID question is open |
| 7 | The AP holds every station's IP-to-MAC binding | It leases one hardcoded address with no pool; and RFC 1027 proxy ARP would make the AP an L3 next hop the bridge will not forward |
| 8 | Bridging upstream "costs nothing extra" | It replaces the gateway model and migrates the userspace IP services |
| 9 | Half the 802.3 translation already exists | LLC/SNAP only; the Ethernet II header is absent |
| 10 | The RX filter is "the first thing Phase 3 should change" | Phase 3 is offline; it is a Phase 2 backend concern under the existing seam contract |
| 11 | *(absent)* | The CCMP nonce omits the QoS TID, and the KATs share the misreading |
| 12 | *(absent)* | Duplicate detection, fragmentation and A-MSDU are unaddressed |
| 13 | *(absent)* | GTK needs key id 1, one key per BSS, and its own PN space |

**Rejected:** that the up path should decrypt group traffic with the GTK. A
station's uplink is always addressed to the AP, so it is pairwise-protected
regardless of the payload's DA. A non-AP STA in an infrastructure BSS has no
transmit GTK at all — for it the GTK is receive-only. Encrypted DHCP, whose
DISCOVER carries a broadcast DA, already works through the single pairwise
decrypt path at `tests/ap_wpa2.cpp:518`. Note precisely what that evidence
shows: not that the AP *chose* pairwise, since there is no key-selection
branch to choose with — that absence is gap #2 above — but that a station's
broadcast-DA frame arrives pairwise-protected and decrypts under the PTK.

## Risks, measured and unmeasured

### R1 — the unicast cliff. ANSWERED: Phase 0 PASS, and it did not gate.

**Answered 2026-08 by Phase 0** (`docs/station-mode-phase0.md`): a
station-shaped transmitter runs at **2084 fps with a 99.9% ACK rate**, 71% of
its matched broadcast control. The 40x cliff is real and is a property of
**TX-only injection**, not of the part and not of a station.
`docs/mt7612u-tx-retry.md:24-38` carries the arm that R1 says does not exist —
`ucast peer ownSA Normal`, 655 fps, 40/40, **0 retries**, MAC receiver on.

The rest of this entry is the pre-measurement reasoning. It is kept because the
mechanism it describes is correct and still explains the TX-only case; its
instruction to measure before writing supplicant code has been carried out.

### R1, as originally written — the unicast cliff

`docs/mt7612u.md` measures injected TX on this part:

| configuration | fps | Mbit/s |
|---|---|---|
| bcast QoS wcid=ff | 3037 | 34.01 |
| **ucast QoS wcid=ff** | **75** | **0.83** |
| ucast QoS wcid=1 + AMPDU | 85 | 0.95 |

A 40× cliff, attributed to the MAC arming an ACK timeout for a peer that never
answers. Clearing `txwi.ack_ctl`'s REQ bit does not prevent it; nor does QoS
Ack Policy = No Ack. The doc's own conclusion: *"A one-way injected link must
address frames to broadcast or multicast."*

**A station's entire data plane is unicast to the AP.** The plausible reading is
that the cliff is the *timeout*, not unicast itself, and that a real AP which
actually ACKs removes it — but that is a hypothesis, and it has not been
measured in either direction. The ACK-responder cell in the same document
measured the *reverse* path (an MT7612U responding with 3500+ ACKs to an 8812AU
stimulus); nothing has measured MT7612U **transmitting** unicast to a peer that
answers.

If the cliff survives an ACKing peer, MT7612U station mode is a management-plane
demonstration and not a usable link, and the project should be re-scoped or
started on a Realtek backend instead. **Measure this before writing any
supplicant code** — see Phase 0.

### R2 — scan cost

Channel switch on this part: **526 ms** full (with the firmware calibration
burst), **48 ms** with calibration skipped, against 0.5–2.5 ms on the Realtek
generations. A full passive scan of 2.4 + 5 GHz at a 100 ms dwell is 4–20 s
depending on the path taken.

Mitigations, in order of value: a configured channel + BSSID skips the scan
entirely (the FPV case knows its channel); reconnect re-arms on the last known
channel without rescanning (PR #335 did exactly this); a scan list narrows to
the band in use. A generic "scan everything" station on this part will feel
broken, and should be presented as what it is.

### R3 — A-MPDU RX reordering

A real AP will aggregate towards the station. MT7612U delivers MPDUs; the
reorder window is software, living in mac80211 rather than in mt76. PR #335 found this the
hard way — *"required once the AP aggregates; without it, out-of-order
subframes are dropped."*

Two options, and the cheap one is legitimate: **decline ADDBA** (the AP falls
back to non-aggregated data — lower uplink throughput, correct behaviour), or
implement a reorder buffer. `docs/mt7612u-ap-mode.md` already names declining
BlockAck as the recommended AP-side workaround for the same reason. Start by
declining; implement reorder only if measured throughput demands it.

### R4 — retune-during-RX data races

`docs/mt7612u.md` open-list item 1 records two ThreadSanitizer findings in the
C library when a channel change runs while the RX ring is up: `mt_read_rx_gain()`
rewriting per-channel gain under `mt_rx_parse()` (wrong RSSI on frames parsed
mid-retune), and a synchronous control transfer reaching `libusb_free_transfer()`
while the async ring's event thread holds that transfer's mutex (a
use-after-destroy inside libusb).

A scanning station drives exactly this path. Until those are fixed, the scan
loop must tune with the ring **stopped** — stop ring, tune, start ring, dwell —
which costs a ring restart per channel on top of R2. That is a real cost and it
belongs in the Phase 0 estimate. The same document notes the shipping Jaguar1
path produces 8 TSan reports under the same stress, so this is a shared gap
rather than a MediaTek regression.

### R5 — ANSWERED: the BSSID registers do not gate a station's receive

*Measured in Phase 2 — `docs/mt7612u-station-identity.md`. Arm F, with both
`MT_MAC_BSSID` and the APC slot deliberately WRONG, receives 5877 unicast
frames against 6250 with nothing programmed, under the managed filter. A wrong
value is harmless for receive on this part.*

*Twice-corrected. The first draft asserted a mechanism the register value
contradicts. Review round 1's "correction" then asserted a SECOND wrong
mechanism — preserved below with its error marked, because it is the reason
the first measurement was taken in the wrong configuration and believed.*

`docs/mt7612u-ap-mode.md` finding 2: under `MBSS_MODE=3` mt76 derives the APC
slot index from the address — `idx = 1 + (((mbss_base[0] ^ addr[0]) >> 2) & 7)`
for a locally-administered address, 0 otherwise — and on the AP side getting it
wrong was **silent**: "beacons perfectly, acknowledges nobody."

~~What this does not license is the obvious extrapolation. The managed-station
filter `mt_mac_start()` leaves is `0x00015f97`, and these are drop bits: bit 3
`MT_RX_FILTR_CFG_OTHER_BSS` is clear, so other-BSS frames are accepted, not
dropped. A wrong APC slot therefore cannot deafen a station by that route.~~

**That reasoning is wrong, and it did damage.** Bit **2** (`PROMISC`) is SET in
`0x00015f97`, and in mt76 bit 2 is the one mapped to `FIF_OTHER_BSS`. Reading
only bit 3 and concluding the filter was not in play is why the Phase 2 gate
was allowed to overwrite that filter with the monitor value and why six
identical arms read as an answer. Two wrong mechanisms in a row on the same
register, the second introduced by a review round correcting the first.

**The measured answer** (`docs/mt7612u-station-identity.md`): with
`0x00015f97` genuinely in force, programming the BSSID correctly, wrongly, or
not at all makes no difference to what a managed station receives — broadcast
or unicast. So a station does not need the APC slot, and `SetStationIdentity`
on MT7612U does not write it.

Two gaps remain, and they are the reason this is "measured" rather than
"closed": the gate still does **not read the APC slot back** (the original
version of this section asked for exactly that), and upstream mt76 carries a
per-slot enable bit at BIT(16) of the slot-0 high register that this tree does
not define and no arm sets. If that enable is real here, arms C–F may never
have enabled the slot they wrote.

### R6 — MT_MAC_ADDR would gain a third co-owner

`docs/mt7612u-ap-mode.md` finding 1: this MAC has no responder register. The
auto-response engine matches address 1 against `MT_MAC_ADDR_DW0/DW1`, gated by
`MT_AUTO_RSP_EN`, which init already leaves on. So arming an ACK responder
means *retargeting the port identity*, and `MT_MAC_ADDR` today has two users —
the beacon and `SetAckResponder` — sharing one register and one save slot, with
ownership belonging to whoever wrote last and explicit hand-off on both paths.

Station arming is a third writer of the same register, and the one that must
**not** move it: a station's port identity is its own MAC, which is where init
leaves it. This is the highest-risk part of the backend work, and the reason
the seam below is `SetStationIdentity` and not a reuse of `SetAckResponder` —
calling `SetAckResponder(bssid)` on a station would move `MT_MAC_ADDR` to the
AP's address and break ACK for its own traffic.

~~Good news in the same finding: because the engine matches on the port
identity and `MT_AUTO_RSP_EN` is on from init, an MT7612U station auto-ACKs
the AP's unicast frames with no call at all.~~

**ANSWERED 2026-09-20** by a third method, after the two below failed: ask
the transmitter. A Realtek peer injects unicast at the MAC and reads its own
per-frame CCX reports — 1279 frames, 100% acknowledged at 0.45 mean retries
with nothing armed, against three controls pinned at the 12-retry limit.
`docs/mt7612u-station-identity.md` has the table and its limits; note it is a
**one-peer** result, which is why the target section above treats "any number
of stations" as an inference rather than a measurement.

The rest of this entry is the historical record of the two failed methods.

**It was a register reading, and Phase 2's first attempt failed to measure
it.** The gate's
single-variable control — clear `MT_AUTO_RSP_EN`, hold reception constant —
does not move the retried-copy count, so that method is void on this rig and
auto-ACK is **unmeasured**. An earlier Phase 2 revision claimed to have
measured it at 0.8% against a 98% control; that control ran promiscuous and is
withdrawn. `docs/mt7612u-station-identity.md` has the detail.

What Phase 2 *did* establish is stronger for this risk than the auto-ACK
claim was: under the managed filter, moving `MT_MAC_ADDR` takes a station's
reception from 103 frames to **zero**. The port identity gates what a station
receives, not merely whether it acknowledges — so the prohibition on moving it
stands, on better evidence than it was written with.

### R8 — the work items the first draft missed

Found in review round 1; each is real and none is in the phase table above
without this section.

- **802.11 TX sequence numbering.** `mt_tx_build` sets `MT_TXWI_ACK_CTL_NSEQ`
  — "let the MAC assign the sequence number" — **only for beacons**
  (`src/mt7612u/tx.cpp:169-172`). Every other TX path airs whatever the caller
  put in the header, and both AP harnesses hardcode it to zero
  (`tests/ap_responder.cpp:152-157`, `tests/ap_wpa2.cpp`). That survives on the
  AP side because it airs few frames; a station's data plane runs into the AP's
  duplicate-detection logic, where a pinned seq=0 is precisely what gets
  dropped. The station needs a sequence counter — host-side, in `src/sta/`,
  where it is backend-neutral and testable — or the backend must set NSEQ for
  data and management frames too. Decide which in Phase 2; do not discover it
  on air.
- **Peer WCID binding.** Every `IRadio` TX path airs with `wcid = 0xff`, the
  no-station index (`src/mt7612u/radiotap.cpp:243,314`). `mt_wcid_setup` exists
  but its only caller in the tree is the bring-up rate-LUT gate. Whether a
  station *needs* a WCID entry with software CCMP and no hardware rate LUT is
  an open question — Phase 0 can answer part of it, since the published table
  measured `wcid=1` (51 fps) as *worse* than `wcid=0xff` (75 fps) against a
  dead peer. But "most of the register work is already in the tree" was an
  overstatement, and this is the item that makes it one.
- **Station-side ARQ is unported and unmeasured.** `tx_retry_limit_ok` is
  false on MT7612U (`src/mt7612u/Mt7612uRadio.cpp:1124`) and `docs/mt7612u.md`
  notes no test drives the hardware retry counter. Phase 0 asks whether an
  ACKing peer restores throughput; it does **not** establish that the MAC
  retransmits the station's own frames when an ACK is missed. That is a second
  question against the same rig and it belongs in the same session.
- **Declining ADDBA is not free.** R3 calls it the cheap option, and it is —
  but it still means parsing ADDBA Action frames and airing a Response with a
  refusal status, which is work that has to appear in `src/sta/Dot11Mgmt`.
  Separately, the station's own uplink cannot aggregate regardless:
  `Mt7612uRadio::SetAmpduMode` returns false
  (`src/mt7612u/Mt7612uRadio.cpp:783-795`) even though the silicon can.
- **Association capability and width negotiation.** "Mirror image of the AP
  side" undersells it: a station must parse the AP's DS Parameter, HT
  Operation and VHT Operation elements and pick a compatible width and rate
  set. MT7612U's 80 MHz and VHT paths are explicitly unexercised on air
  (`docs/mt7612u.md` open list).
- **Beacon-loss supervision must be host-timed.** `hw_rx_timestamp` is false on
  MT7612U (`src/mt7612u/Mt7612uRadio.cpp:1108`), so there is no MAC-latched
  per-frame timestamp to measure beacon spacing against. Use the host clock and
  say so; do not reach for `ReadTsf`.
- **Narrowband APs are unjoinable on this part.** `MT_RATE_BW` encodes only
  20/40/80, so an MT7612U station cannot join this project's own 5/10 MHz FPV
  links. That is not merely "no FHSS" — it constrains the Phase 5
  devourer-to-devourer cell to 20 MHz and upward, and it is worth stating
  beside the FPV motivation rather than leaving it to be discovered.

### R7 — build-matrix constraints, from other issues

- MSVC and mingw are in CI and Windows is first-class (PR #335 died partly here).
- **No `std::jthread` in new code** — issue #426: it is an incomplete feature on
  libc++ and breaks consumers. `std::thread` plus an explicit stop flag. The
  tree is not clean here: `src/jaguar1/RtlJaguarDevice.cpp:1670,1683` and
  `src/rtl8733b/Rtl8733bDevice.cpp:189,193` use it today, which is what #426 is
  open about. The rule is "do not add a third site", not "the tree has none".
- `DEVOURER_MT7612U` defaults **OFF**; anything MT-specific sits behind
  `#if defined(DEVOURER_HAVE_MT7612U)` and the pure layer builds regardless.
- Headless selftests are CMake targets and run in every CI job; the AP
  harnesses are hand-built with `g++`. A station's *pure* layer therefore gets
  ctest coverage and its orchestrator does not.

## Structure

Three layers, mapping onto conventions this repo already has.

### 1. `src/sta/` — pure, backend-agnostic, ctest-gated

Namespace `devourer::sta`. `src/hopset/` and `src/chanmig/` each assert purity
about themselves (no device access, no env reads); `src/sensing/CLAUDE.md`
states the fuller contract a helper subtree keeps — owns no thread, performs no
sleep, takes no clock of record, reads no env. `src/sta/` should promise the
union: **no device access, no environment, no clock of record, no threads, no
sockets.** That is what makes them testable without hardware, and a
station's protocol logic is the most test-worthy code in this scope.

- ``src/sta/Dot11.h` (header-only)` — build probe-request / auth / assoc-request; parse
  beacon, probe-response, auth, assoc-response, deauth, disassoc. IE walker.
  RSN IE build and parse. **Shared with the AP side**: `ap_responder.cpp` and
  `ap_wpa2.cpp` open-code the mirror images of these today. Converting them is
  a follow-up with a ready-made regression gate (`tests/mt7612u_ap_onair.sh`,
  baseline 14/14 on ch36 and 14/14 on ch6).
- `BssTable.h` — scan results: BSSID, SSID, channel, capability, RSN, rate set,
  RSSI, last-seen. Fed parsed frames; touches no device.
- `StationSm.h/.cpp` — the association state machine as a pure transition
  function over `(event, now_ms)` returning actions (*send this frame*, *arm
  this timer*, *done*). Timers and retry budgets are values; the caller sleeps,
  exactly as `src/sensing/DwellExecutor` splits `begin`/`barrier`/`finish`.
  States: `Idle → Scanning → Authenticating → Associating → Handshaking →
  Connected → {Deauthed, BeaconLost} → Reconnecting`.
- `Eapol.h/.cpp`, `Wpa2Supplicant.h/.cpp` — the supplicant half of the 4-way.
  **MIC verified before any state change. Replay gate `<=`. Constant-time MIC
  compare. Keys zeroized on teardown. Never logged.** (PR #335 review items
  1, 2, and the minor list.)
- `Ccmp.h` — the 802.11 CCMP *framing* (AAD construction, nonce, CCMP header,
  PN handling), written over `CryptoOps` and **not** over OpenSSL. RFC-3610 and
  802.11 CCMP known-answer tests in ctest. This closes review item 9 for both
  roles at once.

**Crypto dependency.** `tests/ap_wpa2.cpp` links OpenSSL (`-lcrypto`) and the
extracted `ccmp_software.h` is OpenSSL EVP — there is no hand-rolled crypto to
inherit, which removes the largest single risk PR #335 carried. (`ap_responder.cpp`,
the open-network harness, links no crypto at all — review round 1's correction.)

To keep `libdevourer` dependency-free, `src/sta/` takes a small `CryptoOps`
interface (PBKDF2, HMAC-SHA1, AES key unwrap, AES-CCM) and **nothing under
`src/sta/` includes an OpenSSL header**. The harness supplies the OpenSSL
implementation; the selftest supplies a KAT-verified one. An embedded consumer
can substitute mbedTLS without touching the state machine. Promoting
`ccmp_software.h` therefore means promoting its *framing*, with its EVP body
staying in the harness as one `CryptoOps` method.

**Where the benchmark artifacts are: in the tree.**
`docs/mt7612u-ccmp-benchmark.md`, `tests/ccmp_software.h` and
`tests/ccmp_sw_bench.cpp` were landed by `1139782` and are all present here.
This paragraph previously said they were uncommitted working files on a
`bench/mt7612u-ccmp` branch "not in this worktree", and that the "0.47% of one
core" figure was "a working note, not a citation" — both stale, and the second
sat awkwardly beside this document's own use of that figure to justify building
station mode entirely on software CCMP. The figure is citable. What it is
**not** is an ARM number: it is x86 with AES-NI, which is why CPU budget
remains listed as the largest unmeasured risk in the target section above.

### 2. The one backend seam — `IRadio`

An AP gets its port identity armed as a side effect of `StartBeacon`
(`docs/ap-mode.md`: *"the ACK engine matches `REG_MACID`, which `StartBeacon`
sets to the BSSID"*). A station has no beacon, so nothing arms it. That is the
entire gap, and it is one method pair:

```cpp
/* Arm the MAC as an infrastructure station: `own` is the address the die
 * auto-ACKs and answers for, `bssid` is the BSS it belongs to. Mirror of
 * SetAckResponder, which arms an AP-shaped identity and must NOT be used
 * here — on a die that matches on port identity (MT7612U),
 * SetAckResponder(bssid) would move the port address off `own` and break
 * ACK for the station's own traffic.
 *
 * ORDERING. Call this AFTER StartRxLoop. StartRxLoop programs the receive
 * filter as its last bring-up step and would otherwise overwrite this call's
 * filter; the reverse order silently leaves a monitor filter on a station.
 * Implementations must document which filter value they leave behind.
 *
 * Clear restores the pre-arm identity AND the pre-arm filter value, and
 * returns whether it verified that restore — false is not proof of passive
 * state, exactly as SetAckResponder's contract says. Returns false where the
 * backend has not ported it. */
virtual bool SetStationIdentity(const devourer::MacAddr& own,
                                const devourer::MacAddr& bssid) { return false; }
virtual bool ClearStationIdentity() { return false; }
```

Plus `AdapterCaps::station_mode_ok`, doc-commented at its declaration.

The ordering clause is not a style preference. `Mt7612uRadio::StartRxLoop`
calls `mt7612u_set_monitor_rx()` unconditionally, deliberately, *after*
`mt7612u_start()` has written mt76's managed value — the comment at
`src/mt7612u/Mt7612uRadio.cpp:410-412` explains that writing it earlier is
simply overwritten. A station arming before `StartRxLoop` gets the monitor
filter and no diagnostic. Either the contract carries the ordering, or
`StartRxLoop` stops writing the filter unconditionally; the contract is the
cheaper change and the one that keeps the AP path byte-identical.

`ClearStationIdentity` returns `bool` rather than `void` — review round 1's
finding. A void clear cannot report a failed restore, and on a die where the
response engine matches on whatever address is left programmed, an unreported
failed restore is a radio that keeps answering for an address the caller
believes it released.

**On the two-backend question.** `docs/mt7612u-ap-mode.md` and issue #425 set
the rule that a new `IRadio` contract should be shaped by two backends, not
one. Review round 1 correctly pointed out that this scope invokes that rule
against the key API while proposing a seam that ships with a single
implementation, Phase 6 being deferred.

The honest position: the Realtek shape is **known** but not **built here**.
PR #335's `src/apfpv/StationMode.cpp` is a working, bench-measured port of the
kernel `hw_var_set_opmode` STATION path on an 8812AU — MACID = `own`, BSSID
register = `bssid`, `net_type` = INFRA — plus `SetStationRxFilter` for the
RCR/RXFLTMAP half. That is evidence about the shape, and it is more than the
key API has. It is not the same as a second implementation in this tree.

So the seam is provisional until Phase 6, and the plan says so: if the Realtek
arm forces the signature to change, that is the seam being wrong, and the
correction lands before anything outside `tests/` depends on it. The way to
stop that costing anything is to keep the only consumer a harness — which is
what layer 3 does. An alternative worth weighing at Phase 2's gate is landing
Phase 6 *with* Phase 2 rather than after it, at the cost of a bigger first PR.

The MT7612U side is smaller than the Realtek side but not trivial:
`mt_ap_set_bssid(d, idx, bssid)` already exists from the AP path, the port
identity and MBSS base stay where init left them (R6), and the filter moves to
the managed value — but the APC slot's role for a station is unknown (R5) and
peer WCID binding is untouched (R8).

### 3. `tests/sta_client.cpp` — the orchestrator

Everything impure: the RX thread wiring, the OpenSSL `CryptoOps`, the scan
driver, the DHCP client, ARP/ICMP, the supervisor and reconnect. A harness, not
library API — mirroring `docs/ap-mode.md`'s explicit scope statement, and the
shape upstream has already accepted twice.

On MT7612U the threading is already safe: `Mt7612uRadio`'s `RxQueue` moves
frames off the libusb event thread onto the `StartRxLoop` thread precisely so
that a transmitting processor cannot wedge the part below USB level. On the
Realtek generations the same rule applies as for the AP harnesses —
`send_packet` must run **off** the RX event thread.

Promote the orchestrator into `src/sta/` only when a second backend has shipped
and the shape has stopped moving. The `src/sensing/` CLAUDE.md is the template
for what a device-touching helper subtree has to promise if it ever does.

## Out of scope, and why

- **A key API / hardware CCMP.** Issue #425 standing decision; software CCMP is
  measured sufficient (`docs/mt7612u-ccmp-benchmark.md`).
- **TSF adoption and power save.** `WriteTsf` returns false on MT7612U
  (#430), and the target is an always-on client — power save off is now a
  stated fleet policy, see the target section. `docs/mt7612u-ap-mode.md`
  already identifies pre-TBTT work as the one place the USB-userspace shape
  genuinely fights the protocol. The **TIM element itself is no longer out of
  scope** — `sta::append_tim` exists and AP beacons carry one — but it
  truthfully advertises "nothing buffered", because nothing is. Buffering
  remains out of scope.
- **WPA3/SAE, 802.11w/PMF, 802.1X/EAP.** PSK only.
- **Roaming, BSS transition, multi-BSS, concurrent AP+STA.**
- **Rate control.** Fixed rate, optionally picked once from beacon RSSI. There
  is no firmware rate control on this part — `txwi.rate` airs verbatim.
- **FHSS on MT7612U.** Channel switch is 48–526 ms; out of reach by two orders
  of magnitude.
- **A-MPDU RX reorder**, initially — decline ADDBA (R3).

## Phasing

**Phase 0 — the gate. Measure the unicast cliff against an ACKing peer.**
No new code. One MT7612U runs `tests/ap_responder.cpp` (which arms the identity
and auto-ACKs); the other runs `txdemo` with unicast QoS data addressed to it.
Compare fps against a broadcast arm **in the same session, same channel, same
rate** — the repo's standing rule about adversarial counterparts applies, and
`docs/mt7612u.md`'s existing table is the baseline to beat. Additionally repeat
against a real hostapd AP, so the result is not an artefact of devourer's own
responder. Outcome: go / re-scope / move to Realtek first. This is the cheapest
question in the whole scope and the only one that can end it.

**Phase 1 — shared frame + crypto layer. No new behaviour.**
Promote `tests/ccmp_software.h` to `src/sta/Ccmp.h` with KATs in ctest. Extract
`Dot11Mgmt` from the AP harnesses' open-coded builders and parsers; switch
`ap_responder.cpp` and `ap_wpa2.cpp` onto it. Regression gate:
`tests/mt7612u_ap_onair.sh` must still read 14/14 on ch36 and 14/14 on ch6.
Lands independently of everything else and is useful on its own.

**Phase 2 — the backend seam.**
`SetStationIdentity` / `ClearStationIdentity` on `IRadio`, the MT7612U
implementation, `AdapterCaps::station_mode_ok`. The register-ownership
hand-off with the beacon and the ACK responder (R6) is the risk; cover it the
way `tests/ack_responder_selftest.cpp` covers its counterpart, plus a register
regcheck script that proves the arm/disarm round trip on hardware and reads the
APC slot back (R5).

**Phase 3 — pure station logic, offline.**
`BssTable`, `StationSm`, `Eapol`, `Wpa2Supplicant`, over `CryptoOps`. Headless
ctest: the full 4-way against a recorded authenticator transcript, plus explicit
negative cells for the two PR #335 defects — **a forged MIC must be rejected**,
and **an equal-counter replayed group rekey must be rejected**. No hardware.

**Phase 4 — the harness.**
`tests/sta_client.cpp` plus `tests/mt7612u_sta_onair.sh`, graded pass/fail per
cell in the style of `mt7612u_ap_onair.sh`: `open`, `wpa2`, `reconnect`,
`bench`.

**Phase 5 — validation, independent witness first.**
Against **hostapd on non-MediaTek silicon** before anything else. The AP work's
stated weakness is that its station was the same silicon
(`docs/mt7612u-ap-mode.md`, "What this does not show"); the station work should
not repeat it. Then devourer-to-devourer (`ap_wpa2.cpp` on a second adapter) —
the FPV shape. Then throughput and latency, each number with its adversarial
counterpart in the same breath.

**Phase 6 — deferred, separate issue: the Realtek arm.**
`SetStationIdentity` on Jaguar1/2/3, using PR #335's `StationMode.cpp` as the
reference. Its `SetStationRxFilter` and DIG-saturation findings are worth
landing whether or not the rest of that PR is ever revisited.

## What to file

1. An issue for Phase 0 alone — *"MT7612U: does the unicast TX cliff survive an
   ACKing peer?"* — because it gates the rest and is answerable in a day.
2. An epic for station mode referencing #335 (closed prior art and its review),
   #425 (no key API), and this document.
3. Phase 1 as its own issue; it is independently useful and unblocks both roles.
