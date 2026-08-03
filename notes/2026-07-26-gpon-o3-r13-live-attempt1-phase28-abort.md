# GPON O3 RX-only r13 — live attempt 1 stopped in phase 28

Date: 2026-07-26

Status: **fail-closed phase-28 RSSI-oracle abort. r13 was never loaded and the
GPON MAC O2/O3 sequence did not run. A physical power cut followed.**

## Invocation boundary

The exact audited r13 runner and frozen artifacts were selected after a
confirmed cold boot and fibre connection. All local and uploaded SHA-256
checks passed, as did the read-only and mutation preflights.

Phase 28 loaded successfully, but its receiver-handoff oracle rejected the
run:

```text
i2c_write_attempts:  5 / 18 maximum
mmio_write_attempts: 0 / 3 maximum
sequence_result:     -34
halted_step:         5
rssi_oracle:         vref=0x0209 v=0x0000 delta=0x0000
samples_taken:       0 / 21
```

RX-polarity, APD and digital-handoff operations were not attempted.
`TX_DISABLE` remained asserted. The runner stopped with `rc=116` before
loading r13, before entering O2 or O3, and before any downstream FIFO read.

## Known signature

This is byte-for-byte the same relevant phase-28 failure signature previously
observed on the first phase-39 attempt:

```text
vref=0x0209
v=0x0000
delta=0x0000
5 / 18 I2C writes
0 / 3 MMIO writes
halted_step=5
sequence_result=-34
0 / 21 samples
```

That earlier result also required a physical power cut; its next cold-boot
attempt passed phase 28 with `vref=0x020a`, `v=0x0285` and `delta=0x007b`.
The repeat signature therefore does not justify weakening the RSSI guard or
changing r13. One fresh physical cold-boot retry is the bounded next action.

## Capture integrity

The ordinary runner capture and backup are mode `0600`, byte-identical and
have SHA-256:

```text
93fb8655b00d1dde052848783d63a2eb7d1bb3685a68ffba80506970ae4dd8b6
```

The separately preserved full phase-28 status and its backup are also mode
`0600`, byte-identical and have SHA-256:

```text
30dcc378e099115e3e1ff4668aee2a45fc12a84b9266b45631d4fb916db7da9f
```

No private downstream record was available because r13 never started.

## Recovery

The router became unreachable after a requested physical power cut, observed
at `2026-07-26T03:54:31-03:00`. It must remain off for at least 35 seconds
before one retry. The fibre may remain connected during the power cycle.

This attempt provides no new O3, TX-GEM, PHY-counter, optical-transmit, ONU,
GEM/QDMA or OMCI evidence.
