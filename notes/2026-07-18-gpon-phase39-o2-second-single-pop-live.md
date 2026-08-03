# GPON bring-up — phase 39 RX-only O2 second single-record FIFO pop

Date: 2026-07-18

Status: **completed successfully after an earlier fail-closed phase-28 RSSI
oracle abort**

## Purpose

Phase 38 established the bounded, receive-only hardware semantics for one
downstream PLOAM FIFO record: the MAC queues complete three-word records and
the OEM-sized read removes exactly one of them. Phase 39 repeated that
experiment from a separate physical cold boot. Its purpose was to obtain one
additional downstream record without broadening the experiment into PLOAM
processing, ONU response, O3, or transmit.

As in phase 38, the raw record was deliberately retained only in the private
lab evidence. This note records only its high-level classification: phase 38
classified its private record as **Upstream Overhead**, whereas phase 39
classified the separately captured record as **Extended Burst Length**. No
raw words, decoded fields, optical identity, or provider-specific data are
published here.

## First attempt: RSSI-oracle abort

The first phase-39 invocation correctly stopped inside phase 28 before the
phase-39 observer could be loaded. The strict RSSI precondition sampled Vref
as `0x0209`, outside the accepted receiver-handoff oracle. The handoff halted
at step 5 with `sequence_result=-34`.

The abort was bounded and fail-closed:

- only the five fixed I2C prefix writes had been attempted (`5 / 18`);
- no GPON-MAC MMIO handoff write had been attempted (`0 / 3`);
- RX-polarity and digital-handoff operations were not attempted;
- no downstream FIFO read or phase-39 observer execution occurred; and
- physical TX disable stayed asserted and the run required a physical power
  cut.

This result was not treated as retryable in software. A physical power cut
was performed before the successful retry below.

## Cold-boot retry and bounded receive capture

After the cold boot, phase 28 passed all 21 receiver-handoff samples and its
strict preconditions. Phase 39 then completed with `sequence_result=0` and
`single-pop-complete-restored`.

The final observer result was:

```text
worker state:                  done
restore unsafe:                0
final pre-pop pair valid:      yes
FIFO data-register reads:      3
FIFO word count:               9 -> 6
record captured and valid:     yes
private record verified:       yes
```

The record was observed in O2. The observer accepted only the previously
characterised early downstream trigger residue after independently verifying
the stage-A transition; the accepted status was the exact known
`0x22010001` shape, not a broadened mask. It restored O1 before the destructive
reads, confirmed the same FIFO/status pair immediately before the pop, and
performed exactly three downstream data-register reads.

The GPON-MAC write surface was still only the O2 activation write, the
stage-A bit-20 selective W1C, and the exact O1 restoration. The later stage-B
clear was not attempted on this trigger. WAN mux updates remained limited to
the exact ATM-to-GPON and GPON-to-ATM transitions.

## Transmit and restoration proof

The retry preserved all receive-only guarantees:

- GPON IRQ registration and enable writes: none;
- GPON TX-burst counter: zero;
- O3 writes, ONU identity/global-response writes and upstream FIFO writes:
  none;
- PHY, laser, APD, EN7570 and GPIO writes outside the guarded phase-28
  receiver handoff: none; and
- physical `TX_DISABLE` stayed asserted while `PHYSET3.TXEN` remained clear.

The final checks returned the activation state to O1 and the WAN selector to
ATM exactly. The physical and digital TX guards passed before the FIFO pop,
after it, and after restoration.

## What phase 39 adds

Together, phases 38 and 39 show that the RX-only O2 boundary can consume
separate complete downstream PLOAM records while preserving the audited
three-word FIFO accounting and every transmit barrier. The two private
high-level classifications are distinct activation-class messages captured on
separate cold boots. They demonstrate receiver/FIFO visibility of more than
one message type, but do not establish their on-wire order or cadence.

This remains receive-side characterization only. It does not authorise or
validate PLOAM responses, laser calibration/transmit, O3/O5, GEM/QDMA, OMCI,
or an operational optical WAN.

## Next boundary

Phase 28 is non-transactional. Any later GPON lab experiment must begin with
another physical cold power cut and retain the same fail-closed transmit and
private-capture constraints.
