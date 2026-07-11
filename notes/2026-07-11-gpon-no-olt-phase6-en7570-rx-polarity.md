# GPON no-OLT bring-up — phase 6 EN7570 RX LOS/SD polarity

Date: 2026-07-11

## Result

The second isolated RX-only write was executed on the XR500v without fibre.
It changed only the EN7570 receive signal-detect polarity bit in
`XPON_SETTING`:

```text
before:  XPON_SETTING = 0x0000014f  TRANS_RX_SD_INV=set
active:  XPON_SETTING = 0x0000010f  TRANS_RX_SD_INV=clear
remove:  XPON_SETTING = 0x0000014f  TRANS_RX_SD_INV=set
```

`PHYSET3` remained `0x4581e114` throughout, so this phase did not repeat the
phase-5 ESD deglitch write.  Module removal restored the inversion bit and the
complete passive xPON register dump after removal was byte-for-byte identical
to the before snapshot.  PPPoE stayed operational.

The active `0x10f` value matches both places where the OEM EN7570-specific
initialisation programs `PHY_CSR_XPON_SETTING`: the GPON/EPON branches of
`mt7570_init()` and `mt7570_trans_model_setting()`.  Merbanan models this as the
board/transceiver-specific `airoha,rx-sd-inverted` property.  The retained
OpenWrt boot state was `0x14f`; its only difference from the OEM EN7570 value is
bit 6.

This test proves that the isolated input-polarity write is accepted and
reversible.  With no optical signal it cannot prove which interpretation gives
the correct live LOS behaviour.  The normal firmware therefore continues to
retain `0x14f`; `0x10f` is not yet a shipping default.

## Safety classification

`XPON_SETTING[7:4]` contains four transceiver-interface polarity bits:

| Bit | Signal | Phase-6 decision |
|---:|---|---|
| 7 | burst enable | preserve |
| 6 | RX signal detect / LOS | clear for this isolated test |
| 5 | TX fault | preserve |
| 4 | TX signal detect | preserve |

The polarity helper performs a read/modify/write of bit 6 only.  It does not
write EN7570 over I2C and cannot touch burst enable, TX-fault, TX-SD, mode,
counter, interrupt, PLL, reset, MAC or QDMA state.  It refuses to operate if
`PHYSET3.TXEN` is set.

The common preflight and postflight additionally require:

- active-high GPIO16 TX-disable is an asserted output;
- GPON mode is already selected;
- rogue-ONU, PRBS and test-frame transmit modes are off;
- xPON interrupts are disabled;
- the retained `XPON_SETTING` is exactly `0x0000014f` before this stage;
- the result is exactly `0x0000010f`, with no other changed bit.

The module now exposes two mutually exclusive parameters.  Supplying neither
or both fails before mapping/writing:

```text
arm_rx_init=1                 phase-5 ESD_PRO stage
arm_en7570_rx_polarity=1      phase-6 RX LOS/SD polarity stage
```

The test image still required the separate DT compatible and allow property.
There is no module autoload entry.

## Build and binary audit

The test image was built with the active DT node and package selection only in
a temporary overlay.  Those changes were reverted immediately after the
artifact was saved.

```text
image SHA-256:          9af0ba39a88775468c8870f70d38e1d8734eb3dd7fd52119920406f817a4c194
image size:             11096518 bytes
compressed kernel:      2947891 bytes
kernel payload limit:   0x2ffe00 bytes
kernel headroom:        197325 bytes
SquashFS file offset:   0x300200
required 512-byte gap:  present
TrendChip header:       valid
module SHA-256:         6a60f515ac4c386fe1bcf530b1744ef2e7b26129077c236dfd9b47282586f5be
module size:            10996 bytes
module autoload:        absent
iowrite32 relocations:  2 total, one noinline helper per mutually exclusive stage
I2C/reset/IRQ/timer:    no symbols
```

Both helpers were marked `noinline` so the two source write sites also remain
two easily audited binary relocations instead of compiler-generated inline
copies.  Only one helper executes during a successful probe.

## Live sequence

The temporary firmware booted with the active module present but not loaded.
The passive diagnostic was temporarily bound via `driver_override` and
recorded:

```text
PHYSET3:              0x4581e114
XPON_SETTING:         0x0000014f
mode:                 GPON
phy_fsm_state:        0x3
rx_sync:              no
tx_enable:            no
tx_disable_direction: output
tx_disable_asserted:  yes
rogue/prbs/testframe:  off / 0 / 0
interrupt_enable:     0x00
```

After binding the active driver and inserting with
`arm_en7570_rx_polarity=1`, telemetry reported:

```text
stage:                en7570-rx-polarity
physet3_before:       0x4581e114
physet3_after:        0x4581e114
physet3_current:      0x4581e114
esd_pro_before:       set
esd_pro_current:      set
xpon_setting_before:  0x0000014f
xpon_setting_after:   0x0000010f
xpon_setting_current: 0x0000010f
rx_sd_inverted:       no
tx_enable:            no
tx_disable_direction: output
tx_disable_asserted:  yes
gpon_mode:            yes
rogue_onu_test_mode:  no
prbs_tx_enable_raw:   0x00000000
test_frame_enable:    0x00000000
interrupt_enable:     0x00000000
mmio_writes:          1
en7570_access:        no
pll_or_reset_access:  no
```

The independent EN7570 diagnostic still showed silicon ID `0x03`, variant
`0x01`, Tx-fault pending, and zero register writes, reset/init, laser/APD or
ADC/DDMI control.  PPPoE stayed up and three Internet pings completed with zero
loss at about 27.5 ms average.

After `rmmod`, the passive driver was rebound.  `XPON_SETTING` returned to
`0x14f`, `PHYSET3` remained `0x4581e114`, the full register dump compared
identical, and WAN was still up.

## Stable restoration and passive telemetry

The router was sysupgraded back to stable image:

```text
b5d513e7ef47259321a53f2e07432424240d5e7bdcb1142611b784f374ef4792
```

After reboot, the active module was absent, the passive drivers autoloaded,
`PHYSET3=0x4581e114`, `XPON_SETTING=0x14f`, TX remained disabled, PPPoE came up,
and three Internet pings completed with zero loss.

The passive xPON diagnostic source was extended to decode the four polarity
bits as read-only status.  That follow-up compiles cleanly but was not flashed
as part of this test; it will appear in a future normal image build.

## Next boundary

Functional selection of `0x10f` needs downstream light: compare the xPON LOS
bit and EN7570 raw LOS status with the polarity stage off and on while fibre is
connected, still with physical TX-disable asserted.  No upstream registration
or OLT transmission is needed merely to observe downstream LOS/sync, but a real
OLT signal is needed to distinguish polarity from an idle input.

Without fibre, useful next work is read-only: extend the RX register snapshot
to the GPON synchroniser and FEC control block and classify their reset/retained
state against OEM and Merbanan.  EN7570 calibration, PLL/reset, interrupts,
delimiter/guard, firmware-ready, MAC/PLOAM/QDMA and all TX controls remain
deferred.
