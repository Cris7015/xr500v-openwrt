# GPON bring-up — phase 38 RX-only O2 single-record FIFO pop

Date: 2026-07-18

Status: **completed successfully: one downstream PLOAM FIFO record was
captured under an RX-only boundary, and the OEM three-word pop semantics were
verified without enabling GPON transmit**

## Purpose

Phase 35 proved that the reset/default GPON MAC can raise `PLOAMD_RECV` in O2
and queue nine 32-bit words: three complete OEM-sized downstream PLOAM record
slots.  Phase 36 deliberately stopped before FIFO access when its timing and
baseline preconditions were not exact.

Phase 38 was the separately audited destructive successor.  It was allowed to
remove exactly one three-word record only after all of the following held:

1. phase 28 had completed the guarded EN7570/xPON receiver handoff with every
   transmit kill retained;
2. the MAC entered O2, saw downstream GTC progress, and completed the two
   previously verified selective W1C operations;
3. `PLOAMD_RECV` and a complete nine-word downstream FIFO appeared;
4. O1 was restored before the destructive read; and
5. a final read-only FIFO/status pair still matched the trigger immediately
   before the three reads.

The observer accepted only the normal exact trigger (`INT_STATUS=0x00000001`)
or one previously characterized early-residue shape after an independently
verified stage-A transition.  No broader interrupt mask was admitted.

## Safety boundary

The frozen module retained the prior receive-only limits:

- exact ATM-to-GPON and GPON-to-ATM WAN-mode changes only;
- O1-to-O2 and exact O2-to-O1 activation writes;
- at most the two sampled, selective startup-status W1Cs;
- exactly three reads of the downstream FIFO data register, with no parser;
- no GPON IRQ registration or enable writes;
- no ONU identity, serial-number, global-response, upstream FIFO, burst
  timing, PHY, EN7570, APD, GPIO, laser, reset or QDMA write.

Physical `TX_DISABLE` remained asserted, `PHYSET3.TXEN` remained clear, and
rogue/test/PRBS transmit generators remained clear.  The GPON TX-burst counter
stayed zero.  The module restored O1 and ATM exactly before completion.

The independently built artifacts used for the run were:

```text
source SHA-256:
bbfacbb5ef76e6e90e3e398bc27f8d45e9b339e2df9b08b3f81c50a1ea9aeb7f

kernel module SHA-256:
99751539ac65512b0057968ef512f4052f1efd2a881cc6433d028967926473b8

kernel module size:
353,104 bytes
```

`checkpatch --strict` reported zero errors, warnings and checks before the
live run.  The primary and backup run captures match this SHA-256, but remain
private because the captured record is operator-specific:

```text
f82a5028e829a72eca009d1431f523a5dc827d1519472e63e0bcbd1bbd3556c1
```

## Live result

The phase-28 receiver handoff completed before the phase-38 observer ran.
Phase 38 then completed its whole sequence with `sequence_result=0`:

```text
worker state:                  done
status:                        single-pop-complete-restored
reason:                        single-pop-complete
restore_unsafe:                0
accepted trigger status:       0x00000001
early-residue path used:       no
final pre-pop pair valid:      yes
GPON transmit interrupts:      disabled
GPON TX bursts:                0
```

At the trigger, the downstream FIFO reported nine current words, nine maximum
words and no overrun.  O1 was restored before the pop.  The final pre-pop
snapshot matched the trigger; exactly three FIFO data-register reads then
changed the current word count from nine to six while maximum-used remained
nine and overrun remained clear:

```text
FIFO status before: 0x00090009
FIFO status after:  0x00090006
data-register reads: 3
```

The record passed the observer's structural validation and was retained only
in the private lab capture with a matching private backup.  This note does not
contain the raw PLOAM words, any decoded contents, or any optical identity.

All restoration checks passed: the activation returned to O1, the WAN selector
returned to ATM, and the final MAC/physical guards reported success.

## What this proves

This is the first OpenWrt live proof on the XR500v that the reset/default GPON
MAC can both queue and safely consume one complete downstream PLOAM record in
O2 under a strictly receive-only experiment.  It confirms the OEM FIFO width
and one-record pop accounting (`9 -> 6` 32-bit words) in hardware, not merely
from source review.

It does **not** validate a PLOAM parser, an ONU response, O3/O5 progression,
laser calibration/transmit, GEM/QDMA, OMCI, or an operational optical WAN.
The captured record is deliberately not decoded or published here.

## Next boundary

Phase 28 is non-transactional, so any further GPON experiment requires a
physical cold power cycle before it is loaded again.  If another destructive
receive experiment is justified, it should remain separately audited and
bounded to the next complete FIFO record; it must preserve the same exact
trigger, pre-pop consistency, transmit kills and private-capture rules.
