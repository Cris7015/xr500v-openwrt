# GPON bring-up — phase 34 classifies the RX-EOF status

Date: 2026-07-16

Status: **the OEM RX-EOF status cleared once and remained clear while another
24 downstream GTC frames arrived**

## Purpose

Phase 33 counted 8,800 exact downstream GTC frames over 1.1 seconds, but
`INT_STATUS.bit20`—named `PHY_RX_EOF signal error` by the OEM—latched on the
first frame and was deliberately left uncleared.  Phase 34 performed one
bit-20-only W1C operation to distinguish a one-time transition latch from a
per-frame EOF fault.

## Safety boundary

The guarded experiment:

1. repeated the phase-28 EN7570/APD receiver handoff;
2. selected GPON while preserving unrelated SCU bits;
3. changed only the GPON activation field from O1 to O2;
4. wrote exactly `0x00100000` once to `G_INT_STATUS`;
5. restored the complete original O1 value;
6. restored the exact original ATM selector.

It kept `G_INT_ENABLE=0`, never registered an ISR, never read PLOAM FIFO data
and never wrote identity, upstream timing, preambles, PHY, EN7570, APD, GPIO
or laser state.  Physical `TX_DISABLE` remained high and xPON `TXEN` remained
clear.

## Live result

The phase-28 receiver handoff completed all 18 I2C and three bounded MMIO
writes, reached PHY FSM 6, GPON sync `0xa` and FEC, with LOS clear.

Phase 34 then completed and restored cleanly:

```text
status:                    clear-survived-restored
reason:                    rx-eof-clear-survived
sequence_result:           0
activation_restore_result: 0
mac_after_result:          0
restore_result:            0
guard_after_result:        0

WAN mode:       ATM (3) -> GPON (0) -> ATM (3)
activation:     O1 (1) -> O2 (2) -> O1 (1)
O2 hold:        3,167,220 ns
checks:         46
maximum gap:    76,440 ns
terminal gap:   5,355 ns
```

## RX-EOF classification

The first downstream frame arrived at 150,990 ns:

```text
RX_GTC:             0 -> 1
INT_STATUS:         0x22010000 -> 0x22110000
new bit:            0x00100000 (RX-EOF)
```

The observer wrote only `0x00100000` at 156,240 ns.  Its immediate readback at
156,765 ns showed the bit clear:

```text
INT_STATUS after W1C: 0x22010000
other prior bits:     0x22010000
unexpected bit loss:  0x00000000
```

During the following 3,005,625 ns:

```text
additional RX_GTC frames: 24
final RX_GTC:             25
RX-EOF reassertion:       none
```

The 24 post-clear frames match the expected one-frame-per-125-us GPON cadence.
Therefore bit 20 is not reasserting on every valid downstream frame in this
setup.  It is consistent with a one-time mux/first-frame transition latch.
The 150,990-ns first observation is bounded by the polling cadence and is not
an exact optical wire-time measurement.

## PLOAM and error result

Throughout the bounded observation:

- the PLOAMd FIFO level and receive indication remained zero;
- no FIFO data was consumed;
- all HEC and CRC counters remained zero;
- the TX burst counter remained zero;
- no PLOAMu, serial-number, ranging, dying-gasp or late-start event appeared;
- ONU ID, global configuration and PLOAMu state stayed at reset/default;
- all non-target interrupt-status bits survived the W1C operation.

The absence of PLOAMd over only 3 ms is not itself significant.  This phase
classifies the EOF status; it does not repeat the complete 1.1-second discovery
window from phase 33.

## What this changes

The missing activation PLOAM cannot be explained by a continuously recurring
RX-EOF error on every downstream frame.  Header framing and the PHY-to-MAC
frame cadence remain sound, and the EOF event is clearable without disturbing
the other latched status bits.

Three other bits were already sticky before O2 and were deliberately
preserved: `RX_ERR` (bit 16), downstream-FEC-change (bit 25) and `LWI`
(bit 29).  Phase 34 does not classify whether those are one-time events or
recurrent conditions.  Likewise, the superframe values sampled across the
activation transition/restoration are not comparable to the in-window
lockstep measurements from phases 32 and 33.

That follow-up was completed as phase 35.  It retained O2 for 1.1 seconds
after the bit-20 clear, then selectively cleared the remaining safe startup
status subset.  About 238 ms later the MAC raised `PLOAMD_RECV` and reported a
non-empty downstream FIFO, with TX bursts still zero.  See
[`2026-07-16-gpon-phase35-ploamd-fifo-live.md`](2026-07-16-gpon-phase35-ploamd-fifo-live.md).

A raw GPON MAC reset is still not authorised.  The Ethernet driver owns the
shared reset array, and the OEM quiesces PHY, MBI/GDM, CPU traffic and channels
before pulsing that reset.
