# GPON bring-up — phase 34 RX-EOF W1C diagnostic prepared

Date: 2026-07-16

Status: **offline build and independent audit complete; subsequently run
successfully**

Live result:
[`2026-07-16-gpon-phase34-eof-w1c-live.md`](2026-07-16-gpon-phase34-eof-w1c-live.md).

## Why this phase exists

Phase 33 held the reset/default GPON MAC in O2 for 1.1 seconds and counted
exactly 8,800 downstream GTC frames with no HEC or CRC errors.  The PLOAMd
FIFO and receive indication remained empty, while
`INT_STATUS.bit20`—named `PHY_RX_EOF signal error` by the OEM—latched on the
first frame and stayed set.

Because phase 33 deliberately never cleared interrupt status, it could not
distinguish a one-time mux/first-frame event from a fault that reasserts on
every downstream frame.

## OEM audit

The OEM ISR treats `G_INT_STATUS` as write-one-to-clear: it masks the sampled
status and writes the active bits back.  OEM initialization clears all status
with `0xffffffff`.

The same audit classified `G_INT_ENABLE` as an interrupt mask rather than an
apparent parser/FIFO gate.  FIFO access is independent, the outer QDMA IRQ
callback is enabled separately, and phase 33 latched new status bits while
`G_INT_ENABLE=0`.  Phase 34 therefore does not enable any GPON interrupt.

## Exact experiment

After the phase-28 receiver handoff has again reached PHY_READY, GPON sync and
FEC with every transmit inhibit asserted, phase 34 will:

1. select the GPON WAN mux and verify its readback;
2. require the reset/default O1 MAC state, empty PLOAMd FIFO,
   `G_INT_ENABLE=0`, and `INT_STATUS.bit20=0`;
3. change only the activation field from O1 to O2;
4. wait for both the first `RX_GTC` increment and bit 20;
5. write exactly `BIT(20)` once to `G_INT_STATUS`;
6. observe status and frame count for 3 ms at less than 125-us intervals;
7. restore the complete original O1 value and then the exact ATM mux.

If PLOAMd appears through FIFO level or interrupt bit 0, the observer records
it and restores immediately without reading FIFO data.  It also detects if the
single W1C unexpectedly removes any pre-existing status bit other than bit 20.

`INT_STATUS` itself is a diagnostic latch and cannot be restored after a W1C
write.  Functional state, activation, WAN mux and all transmit barriers remain
the restoration criteria.  The phase-28 physical-power cleanup rule still
applies after a live attempt.

## Audited write surface

- three GPON MAC writes:
  - O1 to O2 at activation offset `0xbc`;
  - one `0x00100000` W1C at interrupt-status offset `0x008`;
  - exact original O1 restoration at `0xbc`;
- two SCU WAN-mode updates: ATM to GPON and exact ATM restoration;
- zero writes to interrupt enable, FIFO, identity, upstream timing, PHY,
  EN7570, APD, GPIO or laser state;
- zero FIFO-data reads and no registered ISR.

## Frozen artifacts

```text
source SHA-256:
388e71c85e9c61a15ef18fcbb99914847f108146217d17534c5f30e858f03b0a

kernel module SHA-256:
9e27f44cdda1cfc895502b29772c63166d837821f560545e1f767160c9c4b4bb

kernel module size:
306,932 bytes
```

The module has the correct Linux 6.12.80 MIPS32r2 big-endian vermagic.
`checkpatch --strict` reported zero errors, warnings or checks.  Static
disassembly confirmed exactly three `iowrite32` calls and two WAN-mode
`regmap_update_bits` calls.

## What came after

Phase 34 found that bit 20 remained clear while valid GTC frames continued,
showing that it did not reassert per frame and was consistent with a one-time
transition latch.  Phase 35 then extended the post-clear observation and
exposed both `PLOAMD_RECV` and a non-empty downstream FIFO without enabling
IRQs or TX.  See
[`2026-07-16-gpon-phase35-ploamd-fifo-live.md`](2026-07-16-gpon-phase35-ploamd-fifo-live.md).

The OEM also resets the GPON MAC after selecting the GPON mux, but that is not
an authorised write.  The Ethernet driver owns the shared reset
array, and stock first quiesces PHY, MBI/GDM, CPU traffic and channels.  A raw
SCU reset pulse would bypass both ownership and the OEM safety sequence, so it
requires a separate integration design.
