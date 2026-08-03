# GPON bring-up — phase 32 proves downstream GTC framing in O2

Date: 2026-07-16

Status: **the EN7570/xPON receiver is delivering correctly framed downstream
GTC traffic to the GPON MAC; no downstream activation PLOAM was observed
during a guarded 240-ms O2 window**

## Purpose

Phase 31 proved that the reset/default GPON MAC could be changed reversibly
from O1 to O2 for 3.005 ms while physical and PHY transmit inhibits remained
asserted.  That interval was sufficient for immediate stability but too short
to distinguish an OLT activation schedule from a broken PHY-to-MAC framing
path.

The EN751221 OEM source supplied an important ordering constraint:

```text
PHY_READY -> O2 -> gpon_enable() -> IRQ/QDMA -> identity/key
```

There is no additional downstream PLOAM-parser enable before O2.  The other
writes in the OEM O2 branch configure upstream response time, preambles and
bit delay, so they remained outside the experiment.

## Safety boundary

Phase 32 retained the exact phase-31 write surface:

- one preserved-register O1-to-O2 write at GPON MAC offset `0x0bc`;
- one exact O1 restoration at the same offset;
- one SCU WAN selector update from ATM to GPON;
- one exact selector restoration to ATM.

It did not write IRQ status/enable, identity, global configuration, FIFO data,
response time, preambles, PHY, EN7570, APD or laser state, and had no O3 write.
It never read the destructive `PLOAMd_RDATA` register.

The nominal observation ended at 240 ms, with a 245-ms capture cutoff and a
hard 250-ms complete-O2 limit.  Safety checks ran with a 75-us delay and
rejected any measured check gap above 125 us, including the first and final
gaps.  The module also stopped immediately on a downstream PLOAM indication,
activation-state change, enabled IRQ, transmit event, TX-burst-counter change,
protected-register change or physical/xPON guard failure.

Before the live test, the MIPS module passed strict checkpatch and binary
inspection.  Its ELF contained exactly two `iowrite32` relocations at
GPON `+0x0bc` and two WAN-selector `regmap_update_bits` calls.

## Live result

The phase-28 EN7570/APD receiver handoff first completed all 18 I2C writes,
three bounded xPON writes and 21/21 guarded samples.  It reached PHY FSM 6,
GPON sync `0xa` and FEC with GPIO16 `TX_DISABLE` high and
`PHYSET3.TXEN=0`.

Phase 32 then completed all checkpoints and restorations:

```text
status:                    timeline-complete-restored
sequence_result:           0
o2_limit_result:           0
activation_restore_result: 0
mac_after_result:          0
terminal_gap_result:       0
restore_result:            0
guard_after_result:        0

WAN mode:       ATM (3) -> GPON (0) -> ATM (3)
activation:     O1 (1) -> O2 (2) -> O1 (1)
O2 hold:        240,007,005 ns
samples:        14 / 14
polls/checks:   2,227 / 2,241
maximum gap:    111,510 ns
terminal gap:   5,250 ns
```

## Framing counters

`DBG_RX_GTC_CNT` advanced exactly once per 125-us GPON frame:

```text
elapsed       RX_GTC
125 us             1
250 us             2
500 us             4
1 ms               8
10 ms             80
50 ms            400
100 ms           800
150 ms         1,200
200 ms         1,600
240 ms         1,920
```

`DBG_DS_SPF_CNT` advanced at the same rate after its first immediate post-mux
sample: between 125 us and 240 ms, both downstream-superframe and GTC counters
advanced by 1,919.  The first superframe value was discontinuous and is treated
as a selector/synchronizer transient.

The receive error/data counters remained zero:

```text
RX GEM:                 0
RX CRC errors:          0
HEC one-bit errors:     0
HEC two-bit errors:     0
HEC uncorrectable:      0
```

`rx_eof_err_int` latched once at the first 125-us boundary.  Because GTC and
superframe counters then advanced perfectly with no HEC or CRC errors, it is a
one-time transition indication rather than evidence of broken continuous
framing.

## PLOAM and transmit result

Throughout the 2,241 checks:

- PLOAMd FIFO remained empty and `ploamd_recv` remained clear;
- PLOAMu FIFO and the invalid reset ONU ID did not change;
- interrupt enable remained zero;
- `DBG_TX_BST_CNT` remained zero;
- PLOAMu, SN O3/O4, dying-gasp and late-start TX events remained clear;
- physical `TX_DISABLE=1`, `TXEN=0`, sync and FEC remained stable.

## What this changes

Phase 32 rules out a missing PHY-to-MAC GTC-framing path.  OpenWrt now has
live evidence for:

1. EN7570/APD receiver initialization;
2. xPON PHY_READY, GPON sync and FEC;
3. GPON MAC reception of one correctly framed GTC frame every 125 us;
4. exact, transmitter-disabled O1/O2 and WAN-selector restoration.

What remained unobserved was a downstream activation PLOAM during this
particular 240-ms window.  Phase 33 subsequently repeated the same audited
write surface continuously for 1.1 seconds.  It received exactly 8,800 GTC
frames with zero HEC/CRC or TX bursts, but the PLOAMd FIFO and receive
indication remained clear.  This rules out the 240-ms duration as the sole
explanation and moves the next investigation to the OEM receive-side GPON MAC
initialization.  See
[`2026-07-16-gpon-phase33-o2-cycle-live.md`](2026-07-16-gpon-phase33-o2-cycle-live.md).
