# GPON FWRDY digital-handoff live-fibre run

Date: 2026-07-15

## Result

The isolated RX-only digital handoff completed successfully once with the
authorised live fibre connected.  Asserting only `PHYSET2.FWRDY` after
clearing `PHYSET3.ESD_PRO` did not reach PHY ready or downstream sync, but it
did cause an immediate internal state transition:

```text
preflight:       PHYSTA1[20:18] = 3, FWRDY = 0, PHYRDY = 0, RX_SYNC = 0
active at 5 us:  PHYSTA1[20:18] = 2, FWRDY = 1, PHYRDY = 0, RX_SYNC = 1
active at 5 s:   PHYSTA1[20:18] = 2, FWRDY = 1, PHYRDY = 0, RX_SYNC = 1
post-rollback:   PHYSTA1[20:18] = 2, FWRDY = 0, PHYRDY = 0, RX_SYNC != 0xa
```

Here `RX_SYNC` denotes the low nibble of `PHYRX_STATUS`; the only valid OEM
sync value is `0xa`.  The observed active value was `0x1`, not sync.  The OEM
binary compiled for this unit tests `PHYSTA1[20:18] == 0x6` for PHY ready, so
state `2` is also not ready.

This establishes that FWRDY is a real digital handoff and is necessary enough
to move the cold PHY state machine, but it is not sufficient to bring up the
receiver.  The next missing dependency remains before PLOAM and ONU identity:
the complete, safely bounded receiver analogue/APD preparation and/or its
associated reset/init ordering.

No ONU identity was programmed or included in the repository.

## Exact build and review

The non-autoload lab module was built against the running OpenWrt 6.12.80
kernel:

```text
C source SHA-256:
73f5cf2585f64012efb10c4dc86cb573d6d9f4ab2a7610a0db42c7cccc4aafd0

MIPS module SHA-256:
a5c6df95ea954092dcbd31c2c148dde32c3444b91b843393f576edc336ab7a43

vermagic:
6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

Before upload:

- the fail-closed source auditor passed;
- all 30 mutation tests passed;
- strict `checkpatch.pl` reported zero errors, warnings or checks;
- the build-directory source was byte-identical to the audited source;
- binary inspection found exactly two `iowrite32()` relocation sites, at
  `PHYSET3 + 0x108` and `PHYSET2 + 0x104`;
- no I2C, GPIO mutation, reset, clock, IRQ, GPON-MAC or SCU write path existed;
- a second independent review returned GO for the guarded live run.

## Preflight

The router retained boot ID `19791d27-098b-4a62-bb07-0911cd397abd`.  The
phase-27 experimental DT opt-in, exact XR500v root compatible and per-unit
factory NVMEM hash gates all passed.  Immediately before the run:

```text
driver:                  xr500v-gpon-diag
PHYSET2:                 0x00003c00
PHYSET3:                 0x4581e114
PHYSET10:                0xff000000
XPON_SETTING:            0x0000014f
PHYSTA1 state:           0x3
firmware_ready:          no
PHYRDY:                  no
RX_SYNC:                 0x0
TXEN:                    off
TX_DISABLE:              output-high/asserted
rogue/PRBS/test/IRQ TX:  all inactive
WAN-QDMA GPON IRQ 22:    0/0
```

The external EN7570 was still at its exact cold APD state
`00 08 00 00`, its OVP latch was zero and the passive diagnostic reported
zero register-data writes.

## Finite live sequence

The complete MMIO sequence was four single-bit writes:

```text
PHYSET3: 0x4581e114 -> 0x4581e110  (clear ESD_PRO)
PHYSET2: 0x00003c00 -> 0x00003c01  (set FWRDY)
PHYSET2: 0x00003c01 -> 0x00003c00  (clear FWRDY)
PHYSET3: 0x4581e110 -> 0x4581e114  (restore ESD_PRO)
```

Six samples were taken at nominal 0, 10, 100, 500, 2000 and 5000 ms.  Their
actual timestamps were 5 us, 10.232 ms, 100.322 ms, 507.322 ms, 2000.318 ms
and 5003.319 ms.  Every sample passed every guard and remained identical in
the important state fields:

```text
PHYSET2:           0x00003c01 (FWRDY=1, PHYRDY=0)
PHYSET3:           0x4581e110 (TXEN=0)
PHYSTA1:           0x000b1919 (state 2, not ready)
PHYRX_STATUS:      0x00000401 (RX low nibble 1, not sync 0xa)
XPON_STA:          0x00000000
XPON_INT_STA:      0x00000004
XPON_INT_EN:       0x00000000
TX_DISABLE:        direction/value/raw = 0/1/1
```

The result block reported:

```text
sequence_result:             0
rollback_write_result:       0
rollback_result:             0
rollback_complete:           yes
physical_powercut_required:  no
factory_hash_matched:        yes
samples_taken:               6/6
mmio_writes:                 4
```

All five individually reported power-cut reasons were `no`.

## Restoration and retained read-only state

The module was unloaded, its target copy was deleted and
`xr500v-gpon-diag` was rebound.  The boot ID did not change.  IRQ 22 remained
`0/0`, every TX barrier remained closed and the EN7570 still reported zero
writes with the same cold APD and OVP values.

All writable xPON baseline words were restored exactly.  Two hardware status
fields nevertheless retained the state-machine transition after rollback:

```text
PHYSTA1:       0x000f1919 -> 0x000b1919  (state 3 -> state 2)
PHYRX_STATUS:  low nibble 0x0 -> 0x1, later raw word 0x00000421
```

Four passive observations at 0, 1, 5 and 10 seconds after restoring the
diagnostic all remained at state `2`, `PHYRDY=0` and RX low byte `0x21`.
These are hardware-owned status fields, not unrolled control writes; their
retention does not request a safety power cut.  A cold boot should nevertheless
be used before the next active experiment so that it begins again from the
documented state-3 baseline.

## Boundary for the next phase

Repeating isolated FWRDY would add no information.  A future active phase must
start after a cold boot and combine the digital handoff only with a newly
audited, finite RX analogue/APD preparation derived from the already validated
OEM primitives.  It must retain the same physical and digital TX barriers,
must not enable GPON MAC interrupts or transmit, and must have a mandatory
post-capture cold-power recovery plan.  The consumed phase-24 through phase-27
one-shot experiments must not simply be rerun.
