# GPON bring-up — phase 8 live-fibre RX boundary

Date: 2026-07-12

## Result

A live Movistar GPON fibre was connected to the XR500v for the first optical
comparison.  Every test kept the independent active-high GPIO16 TX-disable
asserted, `PHYSET3.TXEN` clear, all TX test generators off and xPON interrupts
disabled.  The router never transmitted, attempted registration or ran PLOAM.

The fibre did not reach the EN751221 digital GPON decoder in the current
uninitialised optical state:

```text
PHY FSM:             0x3 throughout
RX sync:             0x00 throughout
RX FEC status:       0x04 throughout
RX codewords:        0
RX errors/BIP/frame: 0 / 0 / 0
```

The phase separates three previous uncertainties:

1. switching `XPON_SETTING` from retained `0x14f` to OEM EN7570 `0x10f`
   flips the xPON LOS interpretation but does not produce sync;
2. clearing `PHYSET3.ESD_PRO` with fibre present does not produce sync;
3. enabling the RX error/BIP/frame counters for 15 seconds records no digital
   activity at all.

A same-boot connected/disconnected comparison of expanded EN7570 raw status
was also indistinguishable.  The apparent rise previously seen in one raw byte
was internal time/saturation, not proof of received light.  Controlled EN7570
RX/LOS analogue initialisation is therefore the next boundary; MAC, PLOAM and
OMCI are not yet relevant.

## Starting safety state

Before loading any active stage:

```text
PHYSET3:              0x4581e114
XPON_SETTING:         0x0000014f
mode:                 GPON
phy_fsm_state:        0x3
rx_sync:              no (0x00)
loss_of_signal:       no
tx_enable:            no
tx_disable_direction: output
tx_disable_asserted:  yes
rogue/prbs/testframe:  off / 0 / 0
interrupt_enable:     0x00
```

The phase-6 dedicated image was reused because it contains the experimental DT
compatible and explicit allow property but does not autoload the active module:

```text
image SHA-256: 9af0ba39a88775468c8870f70d38e1d8734eb3dd7fd52119920406f817a4c194
```

The current overlay retained no experimental compatible, DT allow property or
`DEVICE_PACKAGES` selection.

## Fibre with retained polarity `0x14f`

Ten consecutive one-second samples reported:

```text
XPON_SETTING:    0x0000014f (TRANS_RX_SD_INV=set)
xPON LOS:        no, 10/10
PHY FSM:         0x3, 10/10
RX sync:         0x00, 10/10
RX FEC status:   0x04, 10/10
EN7570 LOS bit:  0, 10/10
```

The EN7570 `LOS_DBG_RG` raw byte 1 rose through `0x60–0x63` and later saturated
at `0x7f`, initially suggesting an optical response.  The final physical A/B
test below disproved that interpretation.

## Fibre with OEM EN7570 polarity `0x10f`

The active module was rebuilt with live FSM/sync/FEC/LOS telemetry.  It retained
the phase-6 polarity helper and all fail-closed gates:

```text
module SHA-256:        6351c4a09420cba0d0484f2bd18755dd5d965c3774c8c0186ed8618d0a36abad
iowrite32 relocations: 2
checkpatch:            0 errors, 0 warnings
```

With `arm_en7570_rx_polarity=1`, ten samples were identical:

```text
PHYSET3:          0x4581e114
XPON_SETTING:     0x0000010f
RX SD inverted:  no
xPON LOS:         yes
EN7570 LOS bit:   0
PHY FSM:          0x3
RX sync:          0x00
RX FEC status:    0x04
TXEN:             off
GPIO16 disable:   asserted
MMIO writes:      1
```

Clearing bit 6 therefore reverses the interpretation of the EN7570 input level
exactly as expected, but the level itself is not yet a calibrated optical
status.  The OEM writes `0x10f` only as part of a larger EN7570 init which also
configures the LOS circuit.  Applying `0x10f` alone is wrong for the current
uninitialised state and was rolled back to `0x14f`.

## Fibre with ESD deglitch cleared

After polarity rollback, the mutually exclusive phase-5 stage was loaded with
`arm_rx_init=1`.  Ten samples reported:

```text
PHYSET3:         0x4581e114 -> 0x4581e110
ESD_PRO:         set -> clear
XPON_SETTING:    0x0000014f unchanged
xPON LOS:        no
PHY FSM:         0x3
RX sync:         0x00
RX FEC status:   0x04
TXEN:            off
GPIO16 disable:  asserted
MMIO writes:     1
```

ESD deglitch is accepted and safe but is not the missing RX enable.  Removal
restored `PHYSET3=0x4581e114`.

## Isolated RX counter stage

The active prototype gained a third mutually exclusive stage:

```text
arm_rx_counters=1
```

It can modify only `XP_ERRCNT_EN[2:0]`, enabling error, BIP and frame counters.
It never writes `XP_ERRCNT_CTL`, so it cannot latch or clear counters.  Probe
requires the enable register and every observed counter to start at zero.  The
same TX/GPON/test/IRQ gates run before and after the write; remove restores the
original enable bits.

```text
module SHA-256:        d316407af639c5872cd6c61401e27ca23522dd792ab6698dc71cb34bc2fb3518
iowrite32 relocations: 3 total, one noinline helper per exclusive stage
checkpatch:            0 errors, 0 warnings
```

With fibre connected, 15 one-second samples returned:

```text
XP_ERRCNT_EN:  0x00000007
ERR_BYTE_CNT:  0
ERR_CODE_CNT:  0
NOSOL_CODE_CNT:0
RX_CODE_CNT:   0
FEC_SECONDS:   0
BIP_CNT:       0
FRAME_CNT:     0
LOF_CNT:       0
PHY FSM/sync:  0x3 / 0x00
```

Removal restored `XP_ERRCNT_EN=0`.  Because no counter incremented, the full
passive before/after register dumps were byte-for-byte identical.

## Expanded EN7570 passive snapshot

The EN7570 diagnostic was extended with pointer-only reads of LOS calibration
and raw ADC/probe registers.  It does not select an ADC channel, toggle a latch,
start conversion or send register data.

```text
module SHA-256:       0f134b7520595d20190ff4212cdcb256a1e9b0a588fd2e65fff564b37d45f628
write/control symbols: absent
checkpatch:           0 errors, 0 warnings
```

With the fibre connected:

```text
LOS status:               0
LOS_CTRL1:                06 08 3c 36
LOS_CTRL2:                10 05 00 00
LOS calibration timer:    ff ff ff ff
LOS timeout count:        00 00 00 00
LOS timeout:              00 00 00 00
ADC probe raw/unlatched:  00 00 00 00
probe control:            00 00 00 00
LOS debug:                variable-byte0 7f 00 10
```

The `0xffffffff` calibration timer and zero ADC/probe state are consistent with
the OEM LOS/ADC initialisation never having run.

## Same-boot fibre connected/disconnected A/B

After returning to the stable firmware, the expanded EN7570 module remained
loaded while five connected samples and five disconnected samples were taken.
No reboot or module reload occurred between the physical states.

Every stable field was identical:

```text
                       connected          disconnected
LOS status             0                  0
LOS_CTRL1              06 08 3c 36        06 08 3c 36
LOS_CTRL2              10 05 00 00        10 05 00 00
LOS calibration timer  ff ff ff ff        ff ff ff ff
ADC probe              00 00 00 00        00 00 00 00
probe control          00 00 00 00        00 00 00 00
LOS debug byte 1       7f                 7f
```

Only byte 0 of `LOS_DBG_RG` fluctuated among small odd values in both states.
It is not a fibre-presence indicator.  The previous byte-1 transition from
approximately `0x02` after boot toward saturated `0x7f` is likewise internal
progress over time, not an optical A/B response.

## Restoration

The router was restored to stable firmware:

```text
b5d513e7ef47259321a53f2e07432424240d5e7bdcb1142611b784f374ef4792
```

All temporary active modules were removed.  The normal SquashFS passive modules
were loaded again, `PHYSET3=0x4581e114`, `XPON_SETTING=0x14f`, TXEN remained
clear and GPIO16 TX-disable remained asserted.  After the final physical A/B,
the fibre was left disconnected.

## Next boundary

The next prototype must isolate only the EN7570 receive/LOS analogue setup from
the much larger OEM `mt7570_init()`:

1. identify the exact effects of `mt7570_LOS_init()` on `LOS_CTRL1`,
   `SVADC_PD` and `LOS_CTRL2`;
2. source and validate per-unit `flash_LOS_high_thld` and
   `flash_LOS_low_thld` before writing thresholds;
3. keep APD, laser bias/modulation, Tx-SD, TGEN, safe-circuit reset and DDMI
   worker paths forbidden;
4. preserve GPIO16 TX-disable and all existing xPON TX gates;
5. provide register-level rollback before another live-fibre A/B.

Until that RX-only analogue stage is reviewed, top-level PHY resets, PLL reset,
interrupts, GPON delimiter/guard, firmware-ready, MAC, PLOAM, QDMA and OMCI are
still premature.
