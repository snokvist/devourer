# Jaguar3 TX page ring — the beacon overwrite, the fix, and what is still open

**Status 2026-09-25.** Root cause found and fixed in `222bcf9`
(`HalmacJaguar3MacInit::terminate_acq_ring`). This file is the record of the
open items the fix leaves, and the ready-to-run plan for the one that would
turn the fix from "matches the vendor's end state" into "matches the vendor's
mechanism": the `GENERAL_INFO` H2C.

The investigation narrative, the ruled-out hypotheses and the three corrected
claims live in `docs/station-mode-plan.md` (Phase 5). The durable per-chip
facts live in `src/jaguar3/CLAUDE.md`. This file does not repeat them.

## The defect in one paragraph

The TX FIFO is 2048 pages of 128 bytes, chained by the LLT. The auto-LLT init
links every page to the next, `0 -> 1 -> ... -> 2047`, so the data ring runs
on into the reserved region, and page `rsvd_boundary` (1938) is where the
beacon lives. Under sustained TX with a beacon armed, the data allocator is
eventually handed page 1938 and overwrites the beacon; the next TBTT reads a
data frame as a beacon descriptor and `TXDMA_STATUS` latches
`BIT_TXPKTBUF_REQ_ERR`. The chip transmits nothing more for the life of the
process. The rtl88x2cu vendor driver's chip, read live on the same adapter,
differs from ours in exactly one LLT entry: `LLT[1937] = 0`. The fix writes
that entry.

| RTL8812CU AP, MCS7, ch6 | before | after |
|---|---|---|
| downlink goodput (~1% loss) | 0.445 Mbit/s | 24.3 Mbit/s |
| our beacon under downlink load | 8% of idle | 100% |
| flood ping | ~21 rt/s, 78–89% loss | 357 rt/s, 1.1% loss |

## Open items

### 1. How the vendor's chip gets `LLT[1937] = 0` — the GENERAL_INFO H2C

**Why it matters.** `terminate_acq_ring` reproduces the vendor's END STATE by
writing the LLT through the packet-buffer debug window. It does not reproduce
the vendor's MECHANISM, and the vendor source never writes the LLT: it runs
the same auto-init we do. Something else produces the terminator. If it is
the firmware, then (a) our direct write may race or conflict with firmware
LLT management we are not modelling, and (b) other firmware behaviour keyed
on the same information is also missing.

**The lead, and why it is the right one.** `hal/hal_halmac.c` in the vendor
tree, `rtw_halmac_init_hal`, in order:

1. `init_mac_flow` — which contains `priority_queue_cfg_8822c` and the
   auto-LLT init devourer ports faithfully;
2. `_drv_enable_trx`;
3. **`_send_general_info`** → `send_general_info_88xx`
   (`halmac_88xx/halmac_fw_88xx.c:1047`), which sends two H2C *packets* —
   `GENERAL_INFO` then `PHYDM_INFO` — and then **polls the H2C queue element
   in the reserved region** (`rsvd_h2cq_addr << 7`, via `dump_fifo_88xx`)
   until it reads `(b0 & 0x7F) == 0x01 && b1 == 0xFF`, i.e. until the
   firmware has consumed them;
4. `rtw_hal_init_mac_register`.

`GENERAL_INFO`'s only field is `FW_TX_BOUNDARY` — the firmware's TX buffer
offset *within the reserved region*. It is sent immediately after the LLT
init. That is precisely the information, at precisely the moment, a firmware
would need to carve the reserved pages out of the data ring. devourer sends
neither packet: it has no H2C-packet path at all, only the HMEBOX register
mailbox.

**What is already known — the exact bytes.** From a usbmon capture of the
vendor driver on this adapter (bus 5, EP `0x05`, 80-byte bulk-OUT = 48-byte
descriptor + 32-byte H2C packet; frames 2331/2332 of that capture):

```
descriptor  DW0 0x00000020  TXPKTSIZE=32  OFFSET=0   (all else 0)
            DW1 0x00001300  QSEL=0x13 (HALMAC_TXDESC_QSEL_H2C_CMD)
            DW7 checksum
            -> bulk-OUT on endpoint 0x05 (HIGH)

GENERAL_INFO  01 ff 0d 00 0c 00 00 00  00 00 38 00 00 00 00 00  00 ... (32 B)
              cat=0x01 cmd=0xFF sub=0x0D  total_len=12  seq=0
              content dword @0x08: FW_TX_BOUNDARY (bits 16..23) = 0x38 = 56

PHYDM_INFO    01 ff 11 00 10 00 01 00  03 02 05 33 00 07 00 00  00 ... (32 B)
              sub=0x11  total_len=16  seq=1
              content: rfe_type 3, rf_type 2, cut 5, ... (matches devourer's
              own bring-up log: rfe_type=0x03, cut=5)
```

`FW_TX_BOUNDARY = rsvd_fw_txbuf_addr - rsvd_boundary`. devourer's layout
(`HalmacJaguar3MacInit::priority_queue_cfg`): `2048 - 50 (CSI) - 4
(FW_TXBUF) = 1994`, and `1994 - 1938 = 56 = 0x38` — the identical value. The
packet can be sent byte-for-byte.

The capture also shows a steady stream of `sub=0x08` packets (`CFG_PARAM`,
the firmware's register-write offload, ACK bit set, seq incrementing). They
are not part of this question.

**The plan, in order, each step with its own pass/fail:**

1. **An H2C-packet transmit path.** 48-byte descriptor (`TXPKTSIZE=32`,
   `QSEL=0x13`, `OFFSET` as captured), 32-byte payload, bulk-OUT on the HIGH
   endpoint, a sequence counter. A headless cell that builds the two packets
   and compares them byte-for-byte to the capture above. *Pass: identical
   bytes.*
2. **Send them where the vendor does** — after `init_mac_cfg` and the TRX
   enable, before the MAC register tables — and poll the H2C-queue element at
   `rsvd_h2cq_addr` (1986) through `ReadPacketBuffer` for the consumed
   marker. *Pass: the firmware consumes them (marker seen within the
   vendor's 100-poll budget).*
3. **The A/B that answers the question.** With `terminate_acq_ring` disabled
   (a temporary build flag, not a shipped knob), read `LLT[1937]` after
   bring-up, with and without the two packets. *If it reads 0 only with them,
   the firmware is the mechanism.* If it reads `0x792` either way, it is not,
   and item 1 closes as "not GENERAL_INFO".
4. **Then decide.** If the firmware does it: send `GENERAL_INFO` in bring-up
   and keep `terminate_acq_ring` as a verified assertion (read the entry, log
   if the firmware did not terminate it) rather than a write. If not: keep the
   direct write and record the negative.
5. Whatever the outcome, re-run the on-air regression set (below).

**What to watch for.** The vendor checks `fw_ver.h2c_version >= 4` before
sending (and warns below 14). devourer's firmware blob's H2C version should
be read before relying on the packet path. And the HMEBOX path and the
H2C-packet path are different firmware queues: the existing rule in
`src/jaguar3/CLAUDE.md` about the HMEBOX box counter does not cover the new
one — it needs its own sequence counter, owned in one place.

### 2. The 8822E is untested

`HalmacJaguar3MacInit` serves both Jaguar3 dies. The fix is written in terms
of the die's own `rsvd_boundary`, so it is structurally correct for the
8822E, but no 8812EU/8822EU was on the bench. *To close:* the `beacons` and
`thru` cells of `tests/sta_d2d_onair.sh` with an 8822E as the AP (the harness
takes `AP_VID`/`AP_PID`/`AP_SYSFS`), and the bring-up log line
`TX page ring terminated at page N` checked for the 8822E's N.

### 3. Jaguar1 and Jaguar2 are unchecked

Both use `first_bulk_out_ep()` for every frame and the Jaguar3 QSEL comment
says it "mirrors Jaguar1 inject". Neither has been checked for the queue
mapping or for a data ring that reaches its beacon page. *To close:* the
`DEVOURER_AP_INJECT` + `DEVOURER_AP_PKTBUF` probe from `tests/ap_wpa2.cpp`
needs a `ReadPacketBuffer` implementation on those backends (the halmac
window addressing is the same family); then inject past one ring traversal
with a beacon armed and watch for a TX-DMA fault. Jaguar2 is a HalMAC part
and the likelier of the two to share the defect.

### 4. The `wpa2` acceptance cell is flaky at 6M on ch6

It fails its 16-of-20 threshold about one run in three (14/20 twice in six
runs). At MCS7 the same cell reads 19–20/20 every time, and 6M on ch6 was
already this lossy before the fix (27% at 10 pps, measured the same day), so
it is not a regression: the threshold was calibrated on ch36 (4–5% loss) and
the cell runs on ch6. Options, not chosen — they change what the gate means:
run the acceptance cells at MCS7; raise the sample to 50 pings with a
correspondingly derived threshold; or move `wpa2` to a cleaner channel.

### 5. Why 6M legacy is less reliable than MCS7 on this bench

Unexplained, and backwards from physics — 6M is the more robust rate. Same
pair, same channel, same minutes: MCS7 one-way loss ~1%, 6M round-trip loss
10–30%. Candidates not yet separated: the legacy-rate TX descriptor fields on
either end (retry limit, `DISDATAFB`), a CCA/EDCA interaction specific to
longer frames on a busy band, or the MT7612U's legacy RX path. A `thru` ladder
at `TX_RATE=6M` against one at `MCS7`, one direction at a time, splits which
end.

### 6. No link-layer retransmission

Neither `ap_wpa2` nor `sta_client` arms `SetAckResponder`, so every lost frame
stays lost. `ARQ=1` in `tests/sta_d2d_onair.sh` arms the AP's responder and
has not yet been measured. With the downlink now working, it is measurable.

## The on-air regression set for any change here

```sh
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh beacons   # 3/3; ours 100% under downlink
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh thru      # both directions ~20 Mbit/s
sudo TX_RATE=MCS7 tests/sta_d2d_onair.sh flood     # 3/3; books close; ~350 rt/s
sudo tests/sta_d2d_onair.sh all                    # 21 checks (see item 4)
```

Plus a station-free stress that needs no association:
`DEVOURER_AP_INJECT=2000 DEVOURER_AP_PKTBUF=1` on `tests/ap_wpa2.cpp` — zero
send failures, no `TXDMA_STATUS` transition, `LLT[1937]=0` in the probe. And
`txdemo` with `DEVOURER_TX_FRAMES=8000 DEVOURER_TX_GAP_US=0`: `tx.stats`
carries `txdma_status`, which must stay 0.

## The answer-key technique

What broke this open was running the **vendor driver on the same adapter** and
reading its live state: `/proc/net/rtl88x2cu/<if>/mac_reg_dump` (whole MAC
window), `fifo_dump` (`echo "<sel> <hex off> <size>"`, sel 0 = TX FIFO,
4 = LLT), `read_reg`, plus usbmon for its descriptors. The vendor driver cannot
move its phy into a netns (`-95`), so run hostapd in the root namespace and
put the devourer station in the netns instead (`/tmp`-scratch
`vendor_ap.sh` did exactly this; worth landing as a harness if used again).
