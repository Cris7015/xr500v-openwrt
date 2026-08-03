# GPON bring-up — phase 40 RX-only O2 same-FIFO two-record pop

Date: 2026-07-19

Status: **completed successfully: two complete downstream PLOAM records were
removed sequentially from one FIFO snapshot, with exact `9 -> 6 -> 3`
accounting and every receive-only barrier retained**

## Purpose

Phases 38 and 39 each removed one complete three-word downstream record, but
they ran after separate cold boots and therefore could not establish any
relationship between those two captures. Phase 40 kept the same fail-closed
boundary while allowing at most two OEM-sized pops from one nine-word FIFO
snapshot. The third complete record had to remain queued and untouched.

## Safety boundary

Phase 40 required the guarded phase-28 EN7570/xPON receiver handoff, a valid
O2 downstream trigger, and an exact O1 restoration before any FIFO-data read.
It then required:

1. a final nine-word FIFO/status pair and full transmit guard before record 0;
2. exactly three reads, followed by an exact `9 -> 6` transition;
3. another independent six-word FIFO/status pair and full transmit guard;
4. exactly three more reads, followed by an exact `6 -> 3` transition; and
5. exact ATM restoration with the final record still queued.

The module registered or enabled no GPON IRQ, wrote no ONU identity or
upstream FIFO, entered no state beyond O2, and made no PHY, laser, APD or
EN7570 transmit write. GPIO16 kept physical `TX_DISABLE` asserted,
`PHYSET3.TXEN` remained clear, and the GPON TX-burst counter remained zero.

## Frozen artifacts

The independently checked artifacts used for the live run were:

```text
source SHA-256:
c810f4c2c0ca8932ea2a63b42ecc79aebee89906818b0ced22369edd05b63fd8

kernel module SHA-256:
4f8ccddba00a7bb4bd1d819e975de93c71a803d16f5aa3d9cda89f80c2fab560

package SHA-256:
e7602dedd45e3e1fbdf1d1eeeea7b9298cc8bcfbda40ae6282a9bdc07eec59d3

private runner SHA-256:
a6255e25b09a217b42afa974f8dd31737ea49239aab22e4dc083b6a3dc2a55e3

ordinary capture and backup SHA-256:
94a26127a53f9bf702078238a152c4af2af9b7334fa65a9ceed17f9f80f2c1e8
```

The separately stored private record capture and its backup were also
byte-identical and mode `0600`, but their contents and fingerprint are not
published.

## Live result

After the physical cold boot, phase 28 passed all 21 receiver-handoff samples.
Phase 40 then completed with:

```text
worker state:              done
status:                    two-pop-complete-restored
sequence result:           0
records captured/valid:    2 / 2
FIFO data-register reads:  6
FIFO word count:           9 -> 6 -> 3
third record retained:     yes
restore unsafe:            0
GPON IRQ enabled:          no
GPON TX bursts:            0
O1/ATM restored:           yes
```

The observer accepted only the separately characterised early trigger shape
after independently verifying the stage-A transition. It restored O1 once
before either destructive read, verified the second FIFO boundary after the
first record, and repeated every MAC and physical transmit guard before
removing the second record.

Primary and backup copies of both the ordinary capture and the private record
capture matched byte for byte. The ordinary capture contains no raw PLOAM
record values. No raw private value was printed during retrieval or analysis.

## Private high-level classification

The SDK defines the first two bytes of each three-word downstream record as
the destination and message identifier. The same interpretation reproduces
the independently established phase-38 **Upstream Overhead** and phase-39
**Extended Burst Length** classifications, providing a layout cross-check
without publishing either record.

Under that checked layout, both phase-40 entries classify as broadcast
**Upstream Overhead** messages. The two complete 12-byte entries are identical
to each other, although they are not byte-for-byte copies of the phase-38
entry. No raw words, decoded payload fields, optical identity or
provider-specific value is published here.

## What phase 40 adds

Phase 40 proves that two complete entries can be dequeued consecutively from
the same already-populated hardware FIFO while preserving exact three-word
accounting, leaving the third entry queued, and retaining every transmit
barrier. In this capture, the observed FIFO dequeue sequence begins with two
identical Upstream Overhead entries.

This establishes only the dequeue sequence of those two entries in this FIFO
snapshot. Because both were already queued before the first CPU read, the
MMIO pop timings do not measure their optical inter-arrival spacing and do not
prove a recurring OLT cadence or a universal activation-message sequence.
The type and relationship of the untouched third entry remain unknown.

It still does not validate PLOAM responses, O3/O5, laser
calibration/transmit, GEM/QDMA, OMCI, or an operational optical WAN.

## Next boundary

Phase 28 is non-transactional. Any later experiment requires another physical
power cut of at least 35 seconds and must retain the same private-capture and
fail-closed transmit constraints. A possible next bounded receive experiment
is a separately audited three-record pop that verifies `9 -> 6 -> 3 -> 0` and
classifies the third entry without enabling any response or transmit path.
