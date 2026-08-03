# GPON bring-up — phases 28–31 receiver handoff and guarded O2 observation

Date: 2026-07-16

Status: **the OpenWrt EN7570/xPON receive chain reaches stable GPON sync and
FEC; the GPON MAC can be held reversibly in O2, but no downstream PLOAM was
queued during the measured 3.005-ms O2 window**

## Safety boundary

These tests used a live GPON fibre only as a downstream optical source.  They
did not program an ONU identity and did not permit OpenWrt optical
transmission.  Every active experiment required all of the following:

- the exact Archer XR500v / EN751221 DT binding;
- exact EN7570 silicon ID `0x03`, variant `0x01` and per-unit factory-data
  hash;
- physical GPIO16 `TX_DISABLE` already configured output-high;
- `PHYSET3.TXEN=0`;
- rogue-TX, PRBS and test-frame controls clear;
- xPON interrupt enable zero;
- no identity, PLOAM FIFO-data, IRQ or upstream-timing write path.

The phase-28 EN7570 receiver sequence is non-transactional.  Its module pins
itself before the first I2C transfer and requires a physical power removal of
at least 35 seconds after the run.  A warm Linux reboot is not a substitute.

## Phase 28 — complete guarded receiver handoff

Phase 28 combined the already audited OEM receive prerequisites into one
one-shot observer:

1. EN7570 software reset;
2. transient RSSI Vref/V calibration;
3. static RSSI gain and LOS programming;
4. the three-step APD `A2` bootstrap;
5. EN7570 receive-polarity selection;
6. xPON `ESD_PRO` clear and `FWRDY` set;
7. 21 guarded receive samples.

The live run completed all 18 fixed I2C writes and three xPON MMIO writes with
zero error:

```text
sequence_result: 0
halted_step:     0
sample_result:   0
samples_taken:  21 / 21
```

All 21 samples retained the same safety and receiver state:

```text
GPIO16 TX_DISABLE:  raw high / asserted
PHYSET2:            0x00003c01 (FWRDY=1)
PHYSET3:            0x4581e110 (TXEN=0)
PHYSTA1:            0x001b1919 (FSM 6 / PHY_READY)
XPON_SETTING:       0x0000010f
RX sync:            0xa
FEC:                enabled
LOS:                clear
PRBS/test/IRQ:      zero
```

This is the first OpenWrt run on the XR500v to reproduce the bounded OEM
EN7570/APD receive prerequisites and reach stable digital GPON sync/FEC while
retaining both physical and PHY transmit inhibits.  Ibias and Imod remained
zero because transmitter calibration and current programming were explicitly
outside the experiment.

## Phase 29 — synchronized 1-ms MAC window

Phase 29 temporarily changed only the shared SCU WAN selector:

```text
ATM (3) -> GPON (0) -> ATM (3)
```

It took two GPON MAC snapshots approximately 1.36 ms apart, performed zero
GPON MAC writes and restored the exact original selector.  Both snapshots
remained in O1 with an invalid `0xff` ONU ID, disabled interrupts and empty
PLOAM FIFOs.  MAC interrupt status changed from `0x22010000` to `0x22110000`:
bit 20, `rx_eof_err_int`, appeared after the selector change.  This is a
receive-side EOF error indication, not evidence of a valid GTC frame or a
decoded downstream PLOAM.

## Phase 30 — longer passive MAC timeline

Phase 30 repeated the same read-only experiment with nine snapshots.  The
nominal 20-ms schedule measured 27.258630 ms on the live MIPS target.  All
nine snapshots remained in O1 with the same empty FIFOs and disabled IRQs.
`rx_eof_err_int` appeared at the second sample and no additional event arrived
during the remaining window.

This extended the passive no-event observation from 1.36 ms to 27.26 ms, but
did not cover a full OLT activation schedule.  The OEM SDK supplied the useful
state-machine fact: after PHY_READY, software explicitly writes
`G_ACTIVATION_ST` from O1 to O2.  The reset/default MAC does not make that
transition autonomously.

## Phase 31 — reversible O1 to O2 observation

Phase 31 isolated exactly that activation-field write.  Before the live run,
both source and MIPS module were audited to prove that the only GPON MAC
writes were preserved-register O1-to-O2 and exact O1 restoration.  The only
SCU writes selected GPON and restored ATM.  The module contained no register
or API path for identity, IRQ enable, FIFO data, preambles, response time, bit
delay, PHY, EN7570, APD or laser writes.

The first build was rejected before use because its nominal 5-ms schedule
would restore O1 only after the final sample.  The final build instead used
absolute elapsed time, ended its nine-sample timeline at 3 ms, prohibited a
new sample at or after 4 ms and recorded an error if the complete O2 hold
exceeded 5 ms.  It also aborted immediately on any unexpected activation
state, enabled IRQ, transmit event, identity/global change or PLOAMu FIFO
change, and verified the physical/PHY guard unconditionally after restoration.

The live execution passed every result and restoration check:

```text
sequence_result:           0
o2_limit_result:           0
activation_restore_result: 0
restore_result:            0
final_read_result:         0
guard_after_result:        0

WAN mode:       ATM (3) -> GPON (0) -> ATM (3)
activation:     O1 (1) -> O2 (2) -> O1 (1)
O2 hold:        3,005,205 ns
selector cycle: 3,020,115 ns
samples:        9 / 9
```

Every O2 sample retained:

```text
ONU ID:          0x000000ff (invalid)
global config:   0x00000034
interrupt enable: 0x00000000
PLOAMu FIFO:     0x00800080 (empty)
PLOAMd FIFO:     0x00000000 (empty)
activation:      O2
```

There was no PLOAMd receive, PLOAMu send, serial-number request/response,
ranging request, late-start or burst-signal event.  Status again changed only
from `0x22010000` to `0x22110000` near the first 125-us boundary.  The added
bit was only `rx_eof_err_int`; because the same transition occurred while the
MAC remained in O1 during phase 30, it is not evidence of an O2-specific event
or a valid PLOAM.  Both xPON guards and the post-run GPIO report retained
`TX_DISABLE=1`, `TXEN=0`, FSM 6, sync `0xa`, FEC enabled, LOS clear and all
test/IRQ controls zero.

## OEM ordering cross-check

The EN751221 GPL source confirms that the stock control flow is
`PHY_READY -> O2 -> gpon_enable()`.  `gpon_detect_phy_ready()` changes O1 to
O2 before `gpon_enable()` repeats general device initialization, registers the
ISR/QDMA path and writes the ONU identity and keys.  There is no additional
GPON-MAC downstream/PLOAM-parser enable before O2.

The other writes in the OEM O2 branch are upstream parameters: extended
preamble, bit delay and response time.  General initialization clears and
enables interrupts and configures dying gasp, GEM/data counters, sleep/ToD,
DBA and transmit timing.  None is justified for an RX-only observer.  In
particular, IRQ must remain disabled: the OEM ISR consumes `PLOAMd_RDATA`, and
processing `Upstream_Overhead` can enable TX and advance to O3.

## What this proves

The live hardware now establishes all of the following under OpenWrt:

1. the EN7570/APD and xPON PHY receive path can reach stable GPON sync/FEC;
2. the GPON MAC aperture can observe that live receiver state safely;
3. the reset/default MAC remains in O1 until software explicitly selects O2;
4. the activation field can be changed to O2 and restored exactly without
   enabling transmission;
5. activation state alone is not sufficient for the reset/default MAC to
   queue a downstream PLOAM during the measured approximately 3-ms window.

It does **not** prove that downstream PLOAM is absent for every longer O2
interval.  Three milliseconds contains about 24 GPON frame periods, which is
enough to test immediate stability but not necessarily a complete OLT
activation cycle.  It also does not justify entering O3.  O3 is the transmit
danger boundary: the hardware can autonomously emit serial-number bursts there
even if software treats the corresponding interrupts as notifications.

## Next boundary

The OEM audit found no missing downstream-only configuration write before O2.
Any next candidate must remain smaller than a general MAC init and must
continue to omit:

- O3 or any later activation state;
- ONU serial/password or ONU-ID programming;
- IRQ enable/registration;
- destructive PLOAMd FIFO-data reads;
- PLOAMu FIFO writes;
- response time, preamble, bit-delay or burst timing;
- PHY, EN7570, APD, laser-current or TGEN writes.

The next staged experiment is therefore a longer repetition of the
already-audited O1-to-O2 write, initially bounded to 250 ms, without adding
configuration writes.  It must keep TX physically blocked, never read/pop
`PLOAMd_RDATA`, stop on the first FIFO event, and sample the read-only
`DBG_RX_GTC_CNT`, `DBG_DS_SPF_CNT`, `DBG_RX_GEM_CNT`,
`DBG_RX_CRC_ERR_CNT`, HEC-error and `DBG_TX_BST_CNT` counters alongside the
existing guards.  This will distinguish a short observation from a PHY-to-MAC
framing problem.  O3 remains outside the test boundary.

That follow-up was completed as phase 32.  The 240-ms live run proved exact
one-frame-per-125-us GTC and downstream-superframe progress with zero HEC/CRC
errors and zero TX bursts.  See
[`2026-07-16-gpon-phase32-o2-framing-live.md`](2026-07-16-gpon-phase32-o2-framing-live.md).
