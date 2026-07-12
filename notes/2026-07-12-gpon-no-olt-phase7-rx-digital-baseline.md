# GPON no-OLT bring-up — phase 7 digital RX/FEC baseline

Date: 2026-07-12

## Result

The persistent read-only xPON diagnostic was extended across the EN751221
digital receive path: GPON synchronisation, superframe/FEC policy, raw counters,
FEC decoder controls and delay/status registers.  The expanded module was
loaded temporarily from `/tmp` on the stable router without a firmware flash or
reboot and without fibre connected.

The important finding is that the receive datapath is not blank or wholly
disabled.  Hardware retains a coherent GPON synchroniser configuration, the
descrambler is enabled, and the FEC decoder is already on:

```text
GPON_PSYNC_CTL:   0x00000029
  M1 limit:       1
  M2 limit:       5
  presync limit:  0
  insync limit:   0
  GSYNC protect:  off

GPON_INDENT_CTL:  0x00001324
  superframe:     lock=3, unlock=1
  FEC threshold:  on=4, off=4
  default FEC:    off
  descrambler:    enabled

FECDEC_CTL:       0x00000001 (decoder enabled)
XP_ERRCNT_EN:     0x00000000 (error/BIP/frame counters disabled)
```

No active init is justified by this phase.  Rewriting FEC enable would be a
no-op.  Merbanan's next core-init operation enables and clears the receive
counters, but with no downstream optical signal that would provide no new
information and would deliberately alter observation state.  It remains
deferred.

## Expanded read-only map

The diagnostic now includes these additional registers:

```text
0x020c GPON_PSYNC_CTL
0x0210 GPON_INDENT_CTL
0x0214 RS_CTL
0x0218 GPON_TEST_CTL
0x0220 PHYRX_MISC_TRIG
0x0224 PHYRX_TEST_DBG_TRIG
0x0248 FEC_SECONDS
0x024c BIP_CNT
0x0250 FRAME_CNT_L
0x0254 FRAME_CNT_H
0x0258 LOF_CNT
0x0260 FECDEC_TESTCTL
0x0264 FECRS_TESTCTL
0x0268 FECDEC_CTL
0x0270 FECDEC_SRAMCTL
0x0290 DUMMY_REG_RX
0x0294 RX_RESET
0x02cc ROUND_TRIP_DELAY_CTRL
0x02d0 ROUND_TRIP_CAL_MASK_CTRL
0x02d4 ROUND_TRIP_DELAY_VALUE
0x02d8 ROUND_TRIP_DELAY_STATIC
0x02dc PSYNC_DET_ALIGN_PHASE
0x02e0 RX_TX_HEAD_TO_HEAD_DELAY
0x03a0 MGMII_PHY_DELAY
```

Reading `regs` does not latch or clear counters.  It performs plain `ioread32`
operations only.  The built module had no write relocations and no IRQ, timer,
reset or I2C symbols:

```text
module SHA-256:       1de16c18d491199101cdf4ab1ddd326ea92ad73e3c4ac12afff81760662c0c40
write relocations:    absent
IRQ/timer/reset/I2C:  absent
checkpatch:           0 errors, 0 warnings
```

## Full live RX snapshot

```text
GPON_PSYNC_CTL          0x00000029
GPON_INDENT_CTL         0x00001324
RS_CTL                  0x00000000
GPON_TEST_CTL           0x00000000
PHYRX_STATUS            0x00000400
PHYRX_MISC_TRIG         0x00000000
PHYRX_TEST_DBG_TRIG     0x00000000
XP_ERRCNT_EN            0x00000000
XP_ERRCNT_CTL           0x00000000
ERR_BYTE_CNT            0x00000000
ERR_CODE_CNT            0x00000000
NOSOL_CODE_CNT          0x00000000
RX_CODE_CNT             0x00000000
FEC_SECONDS             0x00000000
BIP_CNT                 0x00000000
FRAME_CNT_L             0x00000000
FRAME_CNT_H             0x00000000
LOF_CNT                 0x00000000
FECDEC_TESTCTL          0x00000000
FECRS_TESTCTL           0x00000000
FECDEC_CTL              0x00000001
FECDEC_SRAMCTL          0x000000aa
DUMMY_REG_RX            0xff00ff00
RX_RESET                0x00000000
ROUND_TRIP_DELAY_CTRL   0x00000000
ROUND_TRIP_CAL_MASK     0x000061a8
ROUND_TRIP_DELAY        0x00000000
ROUND_TRIP_STATIC       0x00000000
PSYNC_ALIGN_PHASE       0x00000020
RX_TX_HEAD_DELAY        0x00000000
MGMII_PHY_DELAY         0x0000000c
```

The surrounding safety baseline remained unchanged:

```text
mode:                 GPON
phy_fsm_state:        0x3
rx_sync:              no
PHYSET3:              0x4581e114
XPON_SETTING:         0x0000014f
tx_enable:            no
tx_disable_direction: output
tx_disable_asserted:  yes
interrupt_enable:     0x00
```

Two complete expanded snapshots taken two seconds apart were byte-for-byte
identical.  This also confirms that the plain reads do not visibly mutate or
latch the counters.

## OEM and Merbanan comparison

The OEM register header and Merbanan driver agree on the offsets and field
layout for `GPON_PSYNC_CTL`, `GPON_INDENT_CTL`, the receive counters and
`FECDEC_CTL`.

The OEM exposes runtime APIs to change the GPON sync criterion and FEC decoder,
but its `phy_dev_init()` does not overwrite these sync/FEC values.  Merbanan's
current `pon_phy_dev_init()` likewise does not program PSYNC, indentation or
FEC decode; it clears ESD deglitch, enables/clears counters, programs the GPON
TX delimiter and enables interrupts.  The retained live values therefore need
not be replaced merely to imitate either init path.

The only currently disabled RX facility in this block is counter collection.
Merbanan enables bits 0–2 in `XP_ERRCNT_EN` and immediately clears the counters.
That is useful only when there is receive traffic to measure and should not be
combined with unrelated bring-up operations.

## Router restoration

After capture, the temporary module was removed and deleted.  The original
`/lib/modules/6.12.80/xr500v-gpon-diag.ko` from the stable SquashFS was loaded
again.  No sysupgrade or reboot occurred.  PPPoE was up and three Internet
pings completed with zero loss at approximately 20.3 ms average.

The source expansion is retained for the next normal firmware build; it is
read-only and bumps the diagnostic package release to 4.

## Next boundary

There is no useful additional active test without downstream light.  With
fibre connected and GPIO16 TX-disable still asserted, the expanded status can
answer three questions without transmitting:

1. whether `PHYRX_STATUS` reaches sync `0x0a`;
2. whether FEC status/counters react to the downstream GPON stream;
3. whether phase-6 `XPON_SETTING` values `0x14f` and `0x10f` produce the correct
   LOS interpretation.

Counter enable can then be isolated as its own reversible RX-observation stage
if raw downstream activity warrants it.  PLL/reset, interrupts, GPON TX
delimiter/guard, firmware-ready, EN7570 calibration, MAC/PLOAM/QDMA and every
TX control remain deferred.
