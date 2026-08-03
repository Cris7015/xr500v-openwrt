# GPON PHY TX counters — passive no-fibre baseline

Date: 2026-07-26

Status: **live router, no fibre, read-only diagnostic completed and removed.
No MMIO write, counter latch/clear, reset or mode change occurred.**

## Purpose

The r11 O3 run stopped when the GPON MAC debug TX-burst counter changed
`0 -> 1`. That MAC counter is separate from the xPON PHY transmitter status,
frame counter and burst counter. This baseline verifies how the three PHY
registers read on a fresh ordinary OpenWrt boot without fibre before adding
them to a later correlated O3 snapshot.

## Clean boot

The router booted kernel `6.12.80` with a new boot ID. Only the two permanent
read-only diagnostic modules were loaded; phase28, O3 and the other
experimental modules were absent.

The DT node at `0x1faf0000` was unbound. The existing read-only
`xr500v-gpon-diag` platform driver was selected temporarily through
`driver_override`, exactly as in the earlier phase-27 passive inventory. Its
probe maps the xPON window and reserves GPIO16 with `GPIOD_ASIS`; it has no
hardware-write path.

## Two passive samples

Two complete samples separated by two seconds agreed:

```text
PHY mode:                    GPON
PHY FSM:                     3
RX sync:                     no
GPIO16 TX_DISABLE:           output-high / asserted
PHYSET3:                     0x4581e114
PHY TX_STATUS 0x40c:         0x00000000
PHY TX_FRAME_COUNTER 0x434:  0x00000000
PHY TX_BURST_COUNTER 0x438:  0x00000000
PRBS TX:                     0x00000000
test-frame TX:               0x00000000
xPON interrupt enable:       0x00000000
```

The diagnostic's historical `tx_enable` label for `PHYSET3` bit 5 is stale.
OEM source shows that this bit selects continuous versus burst mode; clear is
the expected GPON burst-mode state. GPIO16 remains the confirmed physical
transmitter kill.

The diagnostic reported:

```text
mmio_writes_performed: 0
counter_latch_or_clear: no
reset_or_mode_change: no
```

It was then unbound, its `driver_override` was cleared back to null, and its
debugfs directory disappeared. The phase28 node is free again.

## Capture integrity

The root-private capture and its backup are byte-identical:

```text
SHA-256:
a488150b87fb9aa951c3e16ea9cece846c93e6dc2b6951782f6b3b3ea639ab2c

mode:
0600
```

No provider identity or raw downstream PLOAM record is included here.

## Interpretation limit

OEM helpers set `TX_TEST_TRIG` bit 3 before reading the two PHY counters. This
experiment deliberately did not perform that latch write. Without it,
`0x434/0x438` are unlatched shadow values whose freshness is not guaranteed:

- neither a raw delta nor a quiet value may drive an abort, success or cleanup
  decision;
- `TX_STATUS` bit 15 reports upstream FEC state, not TX activity;
- even a properly latched digital count could not establish emitted optical
  power without an independent optical measurement.

The OEM MMIO accessor also performs two volatile reads and returns the second.
A later passive diagnostic should preserve that behavior and label the results
explicitly `raw_unlatched`.

The compile-only r13 revision now retains the three PHY words only as
non-decisional diagnostics from the existing mapping. GPON MAC
`DBG_TX_GEM_CNT`, by contrast, is a directly readable cumulative MAC counter
and any delta fails closed. r13 does not introduce a second driver, a
`TX_TEST_TRIG` write or any new TX permission. See
[`2026-07-26-gpon-o3-r13-tx-correlation-compile.md`](2026-07-26-gpon-o3-r13-tx-correlation-compile.md).
