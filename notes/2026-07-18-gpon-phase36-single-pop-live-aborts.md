# GPON bring-up — phase 36 live aborts before FIFO access

Date: 2026-07-18

Status: **two live attempts aborted safely before any downstream FIFO data
read; both results are useful scheduler/baseline characterization**

## Common safety result

Both attempts used the frozen phase-28 and phase-36 modules documented in the
compile-only note. Fibre was connected before a physical cold boot, the
watchdog was offline, both local and remote module hashes matched, and no
experimental module was present before either run.

Neither attempt armed or accessed `GPON_G_PLOAMD_RDATA`:

- `fifo_data_reads=0`;
- `record_captured=0` and `private_record_available=0`;
- GPON transmit interrupts remained disabled;
- physical `TX_DISABLE=1`, `PHYSET3.TXEN=0`, rogue/test/PRBS TX clear;
- the GPON TX-burst counter remained zero;
- the GPON selector was restored exactly to ATM;
- every final physical guard passed and `restore_unsafe=0`.

No raw PLOAM word or operator identity was captured or logged.

## Attempt 1: early-boot scheduler gap

The first run started at approximately 69 seconds of uptime, while late
`procd` work was still attempting duplicate loads of the switch modules.  It
entered O2, observed the first GTC and completed the selective BIT20 W1C, but
aborted during stage A:

```text
status: aborted-restored
reason: poll-gap-exceeded
sequence_result: -62 (MIPS ETIME)
stage_a_polls: 387
stage_b_polls: 0
max_check_gap_ns: 61924590
poll_hard_gap_ns: 25000000
```

The 61.924590 ms gap was scheduler latency rather than an optical failure.
The exact O1 and ATM restoration succeeded.  The private primary and backup
captures match:

```text
SHA-256: afede7a18e3446a4adc01a3ff1ed41f546a25d94025f72903924ef36c5f2f4ab
```

## Attempt 2: safe pre-O2 RX race

The second cold boot was allowed to settle beyond 195 seconds.  It did not
encounter scheduler latency.  Instead, the O1 baseline capture found that
downstream GTC had already advanced during the short interval after selecting
the GPON mux:

```text
activation:  O1
RX_GTC:      94
INT_STATUS:  0x22110000
expected:    0x22010000
extra bit:   0x00100000 (RX_EOF_ERR/BIT20)
```

BIT20 is already part of `PHASE36_SAFE_STARTUP_MASK` and is the exact sampled
W1C target used by stage A.  Nevertheless, phase 36 deliberately allowed only
the stage-B subset at the pre-O2 checkpoint, so it rejected the safe early
frame before writing activation:

```text
status: aborted-restored
reason: pre-observation-precondition
sequence_result: -135 (MIPS EUCLEAN)
activation_write_attempted: 0
gpon_mac_mmio_writes: 0
checks: 0
```

Only the GPON mux selection and exact ATM restoration occurred.  The private
primary and backup captures match:

```text
SHA-256: a2e6ae53548f3e0cfe81fde4c8b5a5984598d9ce4216c96a88a22e119bdd3fa7
```

## Next boundary

Do not weaken the 25 ms observation-gap guard.  A minimal successor should
accept exactly either known-safe O1 baseline:

```text
0x22010000  stage-B safe subset only
0x22110000  same subset plus the already proven BIT20 RX latch
```

No other interrupt bit may be admitted.  The existing first-GTC, stage-A
sampled BIT20 W1C, stage-B sampled W1C, exact trigger, three-read limit,
transmit kills and full restoration path remain unchanged.  This addresses a
receive timing race without adding a write or relaxing any TX invariant.
