# MT7612U CCMP offload: benchmark and decision gate

This answers issue #425 without first committing devourer to a key API. There
are two measurements because “hardware crypto is faster” can mean two different
things:

1. `CcmpSwBench` measures the exact software AES-CCM primitive used by
   `tests/ap_wpa2.cpp`, with USB and airtime removed. It is the **maximum CPU
   saving** hardware CCMP could provide.
2. `tests/mt7612u_ap_onair.sh bench` measures the existing software path in a
   real encrypted request/reply stream. It records delivered packets, AP
   process CPU, and time spent inside CCMP. This says whether crypto is on the
   critical path once RX, libusb, TX and airtime are present.

There is deliberately no fake “hardware” arm. The open AP is not a valid
hardware-CCMP proxy: it changes station setup, RX framing and the bytes on air.
Once a test-only MT7612U key/WCID path exists, it must use the same on-air cell
and emit `ccmp.profile` with `path:"hardware"`; only then is the A/B meaningful.

## Run it

Build and run the CPU ceiling on every host that may carry the dongle,
especially the slowest ARM target:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDEVOURER_MT7612U=ON
cmake --build build --target CcmpSwBench
build/CcmpSwBench --millis 1000 --radio-mbps 44.55
```

It can also be built without the rest of devourer on a small target (use that
target's production OpenSSL, not a host cross-build library):

```bash
c++ -O2 -std=c++20 -Itests tests/ccmp_sw_bench.cpp \
  $(pkg-config --cflags --libs openssl) -o ccmp-sw-bench
./ccmp-sw-bench --millis 2000
```

The JSONL rows make results from different CPUs easy to retain. Record the SoC,
core type, clock/governor, OpenSSL build and whether ARM AES instructions are
available next to them. Run the production configuration first; a separate
OpenSSL `no-asm` build is a useful worst-case control. The benchmark reports
CPU and wall time independently, so preemption or throttling is visible rather
than charged to AES. The operation
creates and frees an EVP context for every frame, exactly like the AP. It
includes key/nonce/AAD setup and tag generation/checking, but excludes vector
allocation, 802.11 framing, USB and radio time. `ctest -R ccmp_software` runs a
roundtrip plus corrupt-tag rejection, not the timing test.
`--radio-mbps` projects the percentage of one core at that payload rate, per
direction. Summing encrypt and decrypt gives the conservative full-duplex
crypto budget; it overstates small-frame load when airtime overhead prevents
the requested payload rate.

For the real MediaTek path, use the same two-MT7612U rig and environment as the
AP acceptance test:

```bash
sudo AP_SYSFS=5-1 STA_SYSFS=2-1 CH=36 \
  BENCH_SECS=15 BENCH_PAYLOAD=1400 tests/mt7612u_ap_onair.sh bench
```

The station sends a flood of encrypted ICMP echo requests; devourer decrypts
each request and encrypts each reply. Results and raw logs go under `OUT`
(default `/tmp/mt7612u-ap-onair`). Repeat small and large payloads, on both
bands, with cold cycles between cells:

```bash
for size in 64 256 512 1024 1400; do
  sudo env AP_SYSFS=5-1 STA_SYSFS=2-1 CH=36 AP_VBUS=3-2.3.4:3 \
    BENCH_SECS=15 BENCH_PAYLOAD=$size OUT=/tmp/ccmp-$size \
    tests/mt7612u_ap_onair.sh bench
done
```

Do at least five interleaved software/hardware repetitions when the hardware
arm exists. Compare medians, retain failed/lost cells, pin the AP process to
the same CPU, and hold channel, rate, frame size, aggregation, beacon interval,
station and placement fixed. Report:

- delivered request/reply packets per second and loss;
- AP process core-percent and system-wide idle-subtracted core-percent;
- CCMP nanoseconds per TX and RX frame;
- latency distribution, not only average;
- wall-plug power or SoC package energy and thermal throttling on embedded hosts;
- results at one client and at the intended maximum client count.

## Current CPU-ceiling result

On the x86-64 NucBox bench host (OpenSSL 3.4.1, 2026-09-14), fresh-context
software CCMP measured 0.48/0.47 µs encrypt/decrypt for 64-byte payloads and
1.26/1.26 µs for 1500-byte payloads. The latter is about 9.5 Gbit/s in either
direction. At MT7612U's measured 44.55 Mbit/s injected-data ceiling, 1500-byte
CCMP consumes roughly **0.47% of one core per direction**. Even at 4,000 small
frames/s, 64-byte CCMP is roughly 0.2% of one core per direction.

That is already enough for one scoped conclusion: **hardware CCMP is not a
throughput project on this x86 host**. USB/airtime and the current AP's fixed
6M data rate dominate by orders of magnitude. It does not answer for an ARM
host without crypto extensions; run the same binary there before generalising.

The full on-air software cell on the same host delivered 2,982 of 2,994
1400-byte ping replies in 10 seconds (298.2 replies/s, 0.40% loss). The AP used
2.4% of one core above its paired idle window; system-wide incremental load was
0.035 cores. Profiled CCMP averaged 1.88 µs on TX and 3.47 µs on RX. The cipher
itself accounted for about 16 ms across that 10-second traffic window, so most
AP CPU was RX/TX/USB/framing rather than AES-CCM.

That run also caught and fixed a benchmark-invalidating rig bug: the AP tools
opened the first matching VID:PID even though the shell harness named AP and
station sysfs paths. With two identical MT7612Us, the AP could claim the
station. The AP harnesses now use strict `DEVOURER_USB_BUS`/`_PORT` selection
derived from `AP_SYSFS`; the selected run logged bus 5 port 1 while the kernel
station remained at 8-1.

## Functionality and engineering value

Hardware CCMP can still be worthwhile, but the useful parts are broader than
AES throughput:

- A real WCID/station table is prerequisite plumbing. It also gives the AP a
  natural home for per-peer rate state, retry/ACK reporting and encrypted
  A-MPDU work. Those may be more valuable than the cipher cycles themselves.
- Per-MPDU encryption integrated with the MAC makes PN/IV insertion and
  retransmission semantics match the hardware retry engine. This becomes
  important with aggregation and multiple queues.
- Offload can reduce latency tails, CPU wakeups, energy and contention on small
  embedded CPUs even when peak throughput is unchanged. Measure those; an x86
  throughput-only result cannot reject that benefit.
- The shared-key slots can carry group traffic, while WCID keys carry pairwise
  traffic. That is useful for a multi-client AP, but it does not by itself
  implement group traffic or rekey policy.

Things hardware CCMP does **not** automatically provide:

- GTK rekey still needs authenticator state, EAPOL-Key exchange, overlap of old
  and new keys, and a group-PN policy. Software CCMP can implement this too;
  key slots are an enabler, not the feature.
- Replay protection and fragment handling remain host-visible design work. RX
  keeps the IV/PN, strips MIC/MMIC, reports PN length/decrypt status, and mt76
  deliberately treats fragments differently. Never infer security from a
  successful decrypt bit alone.
- Installing a key in the dongle is not key isolation: the host still derives,
  owns and transfers the key. It may reduce hot-path copies, but this is not a
  secure enclave.
- It does not add WPA3/GCMP, management-frame protection, a multi-client AP
  state machine, RX reorder, or power-save/TIM support.

## Decision gate

Implement the cross-backend key API only if at least one of these is measured
or is an accepted product requirement:

- software CCMP exceeds 10% of one core at the required bidirectional load, or
  causes material p99 latency, power or thermal regressions;
- encrypted throughput is at least 10% below the same-path hardware arm and
  crypto time is shown to be the cause;
- encrypted A-MPDU, several concurrent stations, or group-key traffic requires
  the WCID/SKEY machinery;
- GTK rekey/group traffic is required and the team explicitly chooses hardware
  key-slot management as part of that implementation.

Otherwise keep software CCMP. The current recommendation is **do not implement
hardware CCMP for CPU or throughput on the measured x86 rig; leave the general
decision open until the slowest intended low-power SoC is measured**. On that
SoC, the CPU, latency-tail, thermal and energy gates above are the decision—not
the desktop number. Revisit offload as part of a station-table/encrypted-
aggregation or embedded-power project, and shape the public key contract
against MediaTek plus a Realtek backend as issue #425 requires.
