# GPON bring-up — phase 35 exposes the downstream PLOAM FIFO

Date: 2026-07-16

Status: **the reset/default GPON MAC raised `PLOAMD_RECV` and reported a
non-empty downstream PLOAM FIFO in O2, with every transmit barrier retained**

## Purpose

Phases 32 and 33 proved exact downstream GTC framing for up to 1.1 seconds,
but no activation PLOAM appeared.  Phase 34 then showed that
`G_INT_STATUS.bit20` (`PHY_RX_EOF signal error` in the OEM source) did not
reassert on later frames after a selective clear.  The result is consistent
with a clearable first-frame/mux-transition latch rather than a recurrent
per-frame fault.

Phase 35 combined the proven 1.1-second observation with two sampled,
selective W1C operations:

1. clear only bit 20 after the first sampled post-O2 RX-GTC advance;
2. if PLOAM remained absent for 1.1 seconds, clear only the still-latched safe
   startup subset `RX_ERR` (bit 16), downstream-FEC-change (bit 25) and `LWI`
   (bit 29);
3. continue observing for up to 15 seconds without reading FIFO data.

The second clear was an A/B diagnostic, not an assumption that interrupt
status is a parser enable.

## Safety boundary

The audited module used a dedicated self-pinned kthread and performed only:

- ATM-to-GPON and exact GPON-to-ATM WAN-mode updates;
- O1-to-O2 and exact O2-to-O1 activation writes;
- one sampled bit-20 W1C;
- one sampled W1C limited to bits 16, 25 and 29.

It kept `G_INT_ENABLE=0`, registered no ISR and never read the downstream FIFO
data register.  It made no identity, global-response, preamble, upstream
timing, PHY, EN7570, APD, GPIO, laser, reset or QDMA write.  Physical
`TX_DISABLE` remained asserted, xPON `TXEN` remained clear and the GPON TX
burst counter remained zero.

The final frozen artifacts were independently rebuilt and audited:

```text
source SHA-256:
72042a3a151d37eb4d347c145941cf3cab5e5e07e2ec03af7b762aff81e163bf

kernel module SHA-256:
d7cdde3f1008989c7370e6759d36ca78f4ed61498c3d2c24d19c924da006dc57

kernel module size:
330,132 bytes
```

`checkpatch --strict` reported zero errors, warnings or checks.  Static
disassembly confirmed one selective W1C helper called only with
`0x00100000` and `0x22010000`, the O2/O1 writes, and the two WAN-mode updates.

## Live result

The phase-28 receiver handoff again completed all 18 I2C and three bounded
xPON writes, reaching PHY FSM 6, GPON sync `0xa` and FEC with LOS clear and
both transmit kills active.

Phase 35 then completed with all restoration checks clear:

```text
status:                    downstream-progress-restored
reason:                    downstream-progress
sequence_result:           0
restore_unsafe:            0
activation_restore_result: 0
mac_after_result:          0
restore_result:            0
guard_after_result:        0

WAN mode:       ATM (3) -> GPON (0) -> ATM (3)
activation:     O1 (1) -> O2 (2) -> O1 (1)
O2 hold:        1,339,666,860 ns
TX bursts:      0
```

The first GTC observation occurred at 256,830 ns:

```text
RX_GTC:       1 -> 3
INT_STATUS:   0x22010000 -> 0x22110000
new bit:      0x00100000
```

Stage A sampled and wrote only bit 20 at 264,600 ns.  The immediate readback
945 ns later was:

```text
pre:                  0x22110000
W1C target:           0x00100000
post:                 0x22010000
non-target bit loss:  0x00000000
```

No PLOAM indication appeared during the following 1.1 seconds.  At
1,101,672,180 ns, stage B sampled and wrote the remaining safe startup subset:

```text
pre:                  0x22010000
W1C target:           0x22010000
post:                 0x00000000
readback latency:     735 ns
non-target bit loss:  0x00000000
```

At 1,339,658,040 ns—about 238 ms after that second clear—the observer detected
both correlated downstream status/FIFO indications:

```text
INT_STATUS:       0x00000001 (`PLOAMD_RECV`)
PLOAMd FIFO:      0x00090009
current words:    9
maximum words:    9
FIFO overrun:     0
RX_GTC:           10,718
TX burst count:   0
```

The EN751221 OEM `gponDevGetPloamMsg()` implementation confirms that the
current-used field counts 32-bit words and that one fixed downstream PLOAM
record is exactly three words (12 bytes).  It reads `G_PLOAMd_RDATA` three
times per record and repeats while the pre-read depth is greater than three.
The observed current depth of 9 therefore represents three complete
PLOAM-record slots.  Their raw contents, message types and whether they are
distinct or repeated broadcasts remain unknown because this phase consumed no
FIFO word.

The trigger contained no unknown, TX or error interrupt bit.  O1 and the exact
ATM selector were restored immediately, while ONU ID, global configuration
and the upstream FIFO remained at their reset/default values.

## What this proves

This is the first OpenWrt live proof that the EN751221 GPON MAC downstream
PLOAM receive path is functional past GTC framing:

- the PHY-to-MAC receiver is synchronized;
- the reset/default MAC can recognize a downstream PLOAM event in O2;
- the PLOAMd FIFO can become non-empty without enabling GPON IRQs;
- no optical or internal upstream burst was generated.

It closes the earlier hypothesis that a hidden downstream parser-enable write
must occur before O2.

## Causality caveat

The result does **not** by itself prove that clearing bits 16, 25 and 29 caused
the FIFO event.  The event occurred 238 ms after stage B, but an OLT discovery
cycle could also have delivered the relevant downstream traffic at that time.
A stage-A-only 15-second control run could separate timing coincidence from a
real status-latch dependency.

That control is useful for understanding the hardware, but it is no longer
required to prove basic PLOAM reception: phase 35 already observed both
`PLOAMD_RECV` and a non-empty FIFO.

## Next boundary

The next useful implementation step is a separately audited, RX-only FIFO
capture which:

1. repeats the proven cold receiver handoff and bounded O2 entry;
2. waits for the same FIFO/status trigger;
3. restores O1 while retaining the GPON mux, as phase 35 proved that the FIFO
   remains populated across that activation restoration;
4. reads exactly one three-word record and verifies the current depth changes
   from 9 to 6 while maximum-used remains 9 and overrun remains clear;
5. records raw words only in the private lab capture, without invoking any
   PLOAM parser or publishing operator-specific payload values;
6. restores ATM without enabling IRQs or touching identity/TX state.

Reading the FIFO is destructive to its queued receive state, so its width,
pop semantics and entry count were verified against the EN751221 OEM source
before defining that next experiment.  A later bounded full capture can verify
`9 -> 6 -> 3 -> 0` and compare the three raw records.  Transmitter calibration,
serial-number response, O3, GEM/QDMA and OMCI remain outside this phase.
