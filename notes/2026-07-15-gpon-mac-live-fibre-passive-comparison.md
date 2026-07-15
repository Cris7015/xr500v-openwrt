# GPON MAC live-fibre passive comparison

Date: 2026-07-15

## Result

The already-audited, mux-guarded GPON MAC snapshot was run once with the
authorised live fibre connected.  Its six values were byte-for-byte identical
to the earlier fibre-disconnected baseline:

```text
WAN_CONF:                0x00000003 -> 0x00000000 -> 0x00000003
G_ONU_ID:                0x000000ff  (valid=no, ONU-ID=0xff)
G_GBL_CFG:               0x00000034  (US FEC off, block size 52)
G_INT_ENABLE:            0x00000000  (all GPON MAC IRQ sources masked)
G_PLOAMu_FIFO_STS:       0x00800080  (128 available/min, no underrun)
G_PLOAMd_FIFO_STS:       0x00000000  (empty, no overrun)
G_ACTIVATION_ST:         0x00000001  (O1)
measured_cycle_ns:       9555
```

The result closes the passive MAC comparison: merely connecting the working
fibre does not make downstream data visible to the OpenWrt GPON MAC while the
EN7570 receiver remains in its cold, uninitialised state.  This is a receiver
bring-up boundary, not yet a serial-number, PLOAM or OMCI problem.

No ONU identity was programmed or included in the capture or repository.

## Preflight and passive observation

The router was still on OpenWrt Linux 6.12.80 with boot ID
`19791d27-098b-4a62-bb07-0911cd397abd`.  Before the MAC snapshot, ten passive
samples all reported:

```text
firmware_ready:       no
phy_fsm_state:        0x3
rx_sync:              no (raw 0x00)
rx_fec_status:        0x04
TXEN:                 off
TX_DISABLE:           output-high/asserted
xPON diagnostic writes: 0
EN7570 diagnostic writes: 0
```

`LOS_DBG` byte 0 varied over the finite sample set while bytes 1 through 3
remained `02 00 10`.  That variation is not evidence of light: the same odd
byte-0 pattern was already observed in the cold receiver and phase 8 showed no
connected/disconnected distinction in the same boot.  The authoritative
downstream indicators remained `RX_SYNC=0`, FSM `0x3`, an empty downstream
PLOAM FIFO and activation O1.

The raw xPON snapshot also confirmed that the diagnostic's `pll_enable: no`
line refers only to `PHYSET3.bit11`.  The running value was
`PHYSET3=0x4581e114`, while `ANASTA1` still reported both RX and TX PLL lock.
The EN751221 OEM source and binary likewise leave bit 11 clear; it is not the
identified blocker.

## Guarded live run

The exact previously exercised module was reused:

```text
artifact: /tmp/xr500v-gpon-mac-snapshot-build/xr500v-gpon-mac-snapshot.ko
sha256:   b6c7ac31b4888a201851553e7a9d628ab0104e9af2da1a25f91a7e573fc1f75a
```

Before upload, its strict source auditor and all eight mutation tests passed
again.  The target SHA-256 matched.  The module then used `stop_machine()` for
the already-proven sequence `ATM(3) -> GPON(0) -> six reads -> ATM(3)` and
restored the complete WAN-mux word exactly.

After the run:

- the boot ID was unchanged;
- WAN-QDMA GPON IRQ 22 remained `0/0`;
- no GPON MAC write, FIFO-data read, IRQ handler or poller occurred;
- no EN7570, APD, PHY, laser, GPIO or reset change occurred;
- `PHYSET3.TXEN` remained clear and physical `TX_DISABLE` remained asserted;
- the module was unloaded and its target copy was removed.

## Local evidence

The raw captures are retained outside this repository:

```text
/home/cristuu/tools/xr500v/private-captures/gpon-mac-fibre-20260715/

aea0a2a6a22a9dda6e57a267e2f001c0653aabdc95d9879d90c8384948fa6908  passive-10-samples.txt
c97e64e776aacb2782f087c95d37653e662330bb44da0c9283236cc2d328efaf  preflight.txt
fb2a3b5a0011a35332a5a7e498ad5c5553fb0c8fbea3ad6effffc2f104aa91c3  live-snapshot.txt
```

## Follow-up boundary

The isolated FWRDY follow-up was subsequently implemented, audited and run
once with the authorised live fibre.  It completed all six samples and exact
single-bit rollback without touching EN7570 I2C, APD, PLL/reset, interrupts,
the GPON MAC or ONU identity.  FWRDY moved `PHYSTA1[20:18]` from state `3` to
state `2` within the first 5 us sample, proving the handoff has a real effect,
but it never asserted PHYRDY, reached the OEM ready state `6` or produced the
valid RX-sync nibble `0xa`.

This closes isolated FWRDY as sufficient receiver bring-up.  Any next active
phase must begin from a new cold boot and combine this now-proven digital step
with a separately audited finite analogue/APD preparation; repeating this
probe or the consumed phase-24 through phase-27 one-shots would add no useful
evidence.  See
[`notes/2026-07-15-gpon-fwrdy-digital-handoff-live.md`](2026-07-15-gpon-fwrdy-digital-handoff-live.md).
