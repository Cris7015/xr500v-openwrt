# GPON bring-up — phase 33 observes a complete O2 cycle

Date: 2026-07-16

Status: **downstream GTC framing remained exact for 1.1 seconds in O2, but the
reset/default GPON MAC exposed no PLOAMd FIFO or receive indication**

## Purpose

Phase 32 proved that the EN7570, xPON PHY and GPON MAC count one downstream
GTC frame every 125 us.  Its 240-ms interval did not cover the longest initial
PLOAM delays seen on other test benches.

Phase 33 repeated the exact same guarded and reversible experiment for one
continuous 1.1-second O2 interval.  It introduced no new hardware write.

## Safety boundary

The complete write surface remained:

1. select GPON in the SCU WAN selector while preserving unrelated bits;
2. change only the GPON activation field from O1 to O2;
3. restore the complete original O1 register value;
4. restore the exact original ATM selector.

The module did not enable or clear IRQs, read FIFO data, program identity,
change global configuration, configure upstream response timing or preambles,
enter O3, or write PHY, EN7570, APD, GPIO or laser state.

It checked the complete receive-only guard every 75 us and rejected a measured
gap above 125 us.  A FIFO-level or PLOAM receive indication would have caused
an early successful stop, but only after all safety checks passed.

## Live result

The phase-28 EN7570/APD receiver handoff first passed all 18 I2C writes, three
bounded xPON writes and 21 guarded samples.  It reached PHY FSM 6, GPON sync
`0xa` and FEC with LOS clear, physical `TX_DISABLE` high and
`PHYSET3.TXEN=0`.

Phase 33 then completed and restored cleanly:

```text
status:                    timeline-complete-restored
reason:                    timeline-complete
trigger:                   none
sequence_result:           0
o2_limit_result:           0
activation_restore_result: 0
mac_after_result:          0
terminal_gap_result:       0
restore_result:            0
guard_after_result:        0

WAN mode:       ATM (3) -> GPON (0) -> ATM (3)
activation:     O1 (1) -> O2 (2) -> O1 (1)
O2 hold:        1,100,014,020 ns
samples:        23 / 23
polls/checks:   10,222 / 10,245
maximum gap:    111,720 ns
terminal gap:   5,145 ns
```

## Exact downstream cadence

`DBG_RX_GTC_CNT` remained exact for the complete observation:

```text
elapsed       RX_GTC
125 us             1
1 ms               8
100 ms           800
240 ms         1,920
256 ms         2,048
512 ms         4,096
768 ms         6,144
1,024 ms       8,192
1,056 ms       8,448
1,100 ms       8,800
```

After the first valid frame, the downstream-superframe counter advanced in
exact lockstep.  RX CRC, all three HEC counters, RX GEM and TX burst count
remained zero.  RX GEM being zero in O2 does not by itself test the GEM/QDMA
data path.

`rx_eof_err_int` and downstream-FEC-change appeared at the first frame, as in
phase 32, and remained sticky.  The following 8,799 frames were perfectly
counted with no HEC or CRC error.  Because interrupt status was deliberately
never cleared, this result cannot yet distinguish a one-time transition latch
from `rx_eof_err_int` reasserting on every later frame.

## PLOAM result

For all 10,245 checks:

- the PLOAMd FIFO level stayed zero;
- `INT_STATUS.ploamd_recv` stayed clear;
- no FIFO data register was consumed;
- interrupt enable stayed zero;
- ONU ID, global configuration and PLOAMu FIFO stayed unchanged;
- no PLOAMu, serial-number, ranging, dying-gasp, late-start or TX-burst event
  appeared;
- the physical and xPON transmit inhibits remained asserted.

## What this changes

The 240-ms duration of phase 32 is no longer a sufficient explanation relative
to the known external traces.  Phase 33 exceeded their approximately 205-ms,
768-ms and 917-ms first-PLOAM delays and covered one 1.024-second reference
cadence plus 76 ms of margin.  It does not prove that this ISP OLT necessarily
transmits a discovery PLOAM within the same interval.

This proves that the optical receiver, xPON synchronization and PHY-to-MAC
GTC counters remain locked over 8,800 frames.  It does not prove that the
reset/default GPON MAC has its EOF handshake, payload acceptance, PLOAM
decoder, filtering and FIFO path fully configured.  The OEM names sticky
`INT_STATUS.bit20` the `PHY_RX_EOF signal error` interrupt.  A minimal
follow-up candidate is to clear only that W1C bit after the first frame and
observe for 1–3 ms whether it immediately reasserts, still with IRQ and every
TX path disabled.

The follow-up OEM audit found that `G_INT_ENABLE` is an interrupt mask, not an
apparent parser/FIFO gate: the FIFO accessors are independent, the outer QDMA
IRQ callback is enabled separately, and phase 33 itself latched new status
bits while `G_INT_ENABLE=0`.  Therefore enabling bit 0 is not justified.  The
bit-20-only W1C test is the smallest next diagnostic, although it is unlikely
by itself to make a PLOAM appear.

The strongest remaining OEM initialization-order difference is more
substantive: after selecting GPON, stock pulses the GPON MAC reset for 1 us
before initializing and entering operation.  It is not yet an authorised
follow-up: the Ethernet driver owns the shared reset array, and stock first
quiesces PHY, MBI/GDM and CPU traffic.  A raw reset pulse would bypass both
ownership and the OEM safety sequence.  It needs a separate integration design
after the bit-20 diagnostic.

No O3, identity, transmit or FIFO-data experiment is justified yet.
