# RTL8822E (RFE-21) 5 GHz TXGAPK / RF-reg-0x0 routing investigation

Status: **root cause of one real bug found + fixed + register-verified; the
field MCS5-7 failure is NOT yet explained by it — investigation ongoing.**
Do **not** upstream this as an "MCS5-7 fix" until the field failure is
reproduced and a fix is proven to recover it.

Chip: RTL8822E (Jaguar3, `ChipVariant::C8822E`), RFE type 21, 5 GHz-only USB
adapter (VID 0x0bda). Cross-reference driver: vendor kernel
`rtl88x2eu-20230815` (which reaches MCS5-7 reliably).

## Symptom

The 8822e transmits cleanly at MCS0/2/4 (BPSK/QPSK/16-QAM) but 64-QAM
(MCS5/6/7) is reported as RF garbage in the field (near-total RX loss),
power-invariant. On a **healthy** bench at low power / short bursts / ~30 cm it
delivers MCS5-7 fine — so the failure is condition-dependent, not present in a
cold low-power burst.

## Bug found and fixed: RF register 0x0 was routed through the wrong window

On 8822e, **every RF register except 0x0** is written through the direct BB
window `0x3c00`/`0x4c00` (`RF_WIN`). **RF reg 0x0 is special** — it must be
written through the legacy 3-wire "FON" path `0x1808`/`0x4108` as a full-DWORD
write with the RF address in bits [27:20]. The vendor dispatcher
`config_phydm_write_rf_reg_8822e` special-cases exactly this; devourer's
`Halrf8822e::rf_write` did not — it sent reg 0x0 through `0x3c00` like the
others, so **every `rf_write(path, 0x0, …)` was a silent no-op** on the actual
RF register (it scribbled BB reg `0x3c00` instead).

The most visible consequence is in TXGAPK: `txgapk_save_all` selects the RF TX
gain-table read index with `rf_write(path, 0x0, 0x0ff, idx)` and then reads reg
`0x5f`. With the index write a no-op, **every one of the 11 reads returns the
index-0 LUT entry**. On the RFE-21 5 GHz gain LUT, index-0 is `0x000`, so all
three 5 GHz bands read back all-zero; devourer's zero-guard then **skips**
applying TXGAPK on 5 GHz. (2.4 GHz index-0 is non-zero, so 2.4 GHz read back a
non-zero *constant* — the tell-tale "all 11 gain indices identical" anomaly.)

### Fix

`src/jaguar3/Halrf8822e.cpp` `rf_write` — port the `reg_addr == 0x0` branch of
`config_phydm_write_rf_reg_8822e`: for reg 0x0, read-merge partial masks (read
via the `0x3c00` window, which is correct for reads) and write the full DWORD to
`0x1808`/`0x4108`.

An escape hatch `DEVOURER_RF0_LEGACY=1` restores the old (broken) direct-window
routing for A/B testing.

### Passive verification (no TX)

With `DEVOURER_GAINTAB_DBG=1`, the 5 GHz gain-table readback in `txgapk_save_all`
changes from all-zero to the kernel's exact per-index ramp:

```
before fix (band2/3/4):  000 000 000 000 000 000 000 000 000 000 000
after  fix (band2/3/4):  000 000 002 005 008 00b 028 02b 02e 031 034
```

These match the kernel radio-table 0x3f values `{000,000,080,140,200,2c0,a00,
ac0,b80,c40,d00}` shifted right by 6 (the write path stores `0x3f = value<<6`),
entry-for-entry. The "TXGAPK skip write-back" guard no longer fires.
adapter-doctor stays HEALTHY on both the fixed adapter and its RX partner.

## Honest limitation: this fix does NOT unlock MCS5-7

A gated TX A/B (EU→CU, ~30 cm, floor TXAGC index 24, 8 s bursts, 3 rounds,
interleaved fix vs `DEVOURER_RF0_LEGACY=1`, adapter-doctor HEALTHY throughout)
showed **MCS5-7 deliver with both routings** — no advantage from the fix:

```
  MCS       FIX     LEGACY(old)
  MCS5      132        79
  MCS6       54        84
  MCS7       39        97      (noise-level; legacy even higher)
```

Reason: the RF **radio table already loads a correct 5 GHz gain ramp** via
`0x33`/`0x3f` (which use the working `0x3c00` window). TXGAPK only adds *fine
per-index offsets* on top of that ramp. Legacy (guard-skip) keeps the ramp; the
fix keeps the ramp + small offsets — a negligible live-gain delta at low power.
So TXGAPK-skip was never large enough to move MCS7 from failing to working.

The original catastrophic MCS7 loss does **not** reproduce on the healthy
bench (MCS7 delivers either way), which points at a **condition-dependent**
cause (sustained / thermal / higher-power) rather than this one-shot cal.

## Why keep the fix anyway

It is a genuine correctness bug and a real divergence from the vendor driver:
reg 0x0 must use the FON path, and `rf_write` is shared by *all* rf-0x0 writes
(not only TXGAPK). Fixing it makes TXGAPK match the kernel exactly and removes a
whole class of silent no-op RF writes. It is register-verified and does not
perturb RX or init health. It should be contributed as a **correctness fix**,
not advertised as resolving MCS5-7.

## Open question / next direction

The kernel reaches MCS5-7 reliably; devourer doesn't in the field. The suspected
remaining gap is a **condition-dependent TX-EVM mechanism** devourer omits —
e.g. thermal TX-power/gain tracking under sustained TX, periodic re-calibration
in a watchdog, or an RF-init step whose absence only bites under load. Next step
is to **reproduce** the failure (a bench condition where legacy/stock devourer
MCS7 actually fails) and only then prove a fix recovers it.

## Repro scripts (external, in the waybeam bench workspace)

`scratchpad-link/`: `iqk_verdict.sh` (IQK fail_step), `rf0_tx_ab.sh` /
`rf0_mcs7_ab.sh` (gated TX A/B, fix vs `RF0_LEGACY`), `canary_postreplug.sh`
(adapter-doctor). All TX is operator-gated, floor power, doctor before/after —
the 8822e is burn-sensitive.
