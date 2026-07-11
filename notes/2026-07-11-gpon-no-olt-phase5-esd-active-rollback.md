# GPON no-OLT bring-up — phase 5 isolated RX write and rollback

Date: 2026-07-11

## Result

The phase-4 RX-only prototype was executed on the XR500v, without fibre
connected.  It passed every fail-closed gate, performed exactly one MMIO write,
and changed only `PHYSET3.ESD_PRO`:

```text
before:  PHYSET3 = 0x4581e114  ESD_PRO=set
active:  PHYSET3 = 0x4581e110  ESD_PRO=clear
remove:  PHYSET3 = 0x4581e114  ESD_PRO=set
```

Module removal exercised the default rollback path.  The complete passive
xPON register dump after removal was byte-for-byte identical to the dump taken
before insertion.  The router was then returned to the phase-3 stable image;
the experimental module is not present in that image.

This proves only that the isolated signal-detect deglitch operation is accepted
and reversible on this silicon.  With no downstream light, RX sync and PHY-ready
correctly remained absent.  It does not establish a GPON link, O5, a data path,
or OMCI support.

## Dedicated test image

The active node and package were enabled only long enough to build a dedicated
test image.  The overlay was reverted immediately after the artifact was saved,
so the experimental compatible, allow property and package selection are not
part of the shipping tree.

```text
image SHA-256:          1ea8883c02c093ebfa680c5819e15f98e2b40b97ceeb4f7aec987b95261fcab0
image size:             11096118 bytes
compressed kernel:      2947891 bytes
kernel payload limit:   0x2ffe00 bytes
kernel headroom:        197325 bytes
SquashFS file offset:   0x300200
required 512-byte gap:  present
TrendChip header:       valid
module SHA-256:         4713d2e59169e8e2a6472d939caa4995b1c0145fa63d54f4412124aeb66b1d14
module size:            9604 bytes
module autoload:        absent
```

The module remained dual opt-in: DT had to contain both the experimental
compatible and `econet,allow-rx-only-init`, and insertion still required
`arm_rx_init=1`.  A debugfs status file recorded the original, post-write and
current register values plus the write count and all TX safety gates.

## Live sequence

The temporary image booted normally.  As expected, neither the active module
nor the passive platform diagnostic bound automatically: the active module had
no autoload entry, while the passive driver's compatible no longer matched the
temporary node.  EN7570 diagnostics continued to bind normally.

Before the write, the passive driver was bound through `driver_override` and
captured the normal phase-3 baseline:

```text
mode:                 GPON
phy_fsm_state:        0x3
rx_sync:              no
PHYSET3:              0x4581e114
tx_enable:            no
tx_disable_direction: output
tx_disable_asserted:  yes
rogue/prbs/testframe:  off / 0 / 0
interrupt_enable:     0x00
```

It was then unbound, the active driver was selected with `driver_override`, and
the module was inserted with `arm_rx_init=1`.  Its live telemetry reported:

```text
physet3_before:       0x4581e114
physet3_after:        0x4581e110
physet3_current:      0x4581e110
esd_pro_before:       set
esd_pro_current:      clear
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

During the active interval the EN7570 still reported silicon ID `0x03`, variant
`0x01`, Tx-fault set, and zero register-data writes, reset/init, laser/APD, and
ADC/DDMI control.  PPPoE stayed up and three pings to `1.1.1.1` completed with
zero loss at about 23.7 ms average.

After `rmmod`, the passive driver was rebound.  `PHYSET3` returned to
`0x4581e114`; TX remained disabled and the complete register dump compared
identical with the before snapshot.

## Stable restoration

The router was finally sysupgraded back to the saved stable image:

```text
SHA-256: b5d513e7ef47259321a53f2e07432424240d5e7bdcb1142611b784f374ef4792
```

After reboot, the persistent passive probes autoloaded and bound again,
`PHYSET3=0x4581e114`, GPIO16 remained asserted, TX remained off, the active
module was absent, PPPoE came up, and three Internet pings completed with zero
loss.  No fibre was connected at any point.

## Next boundary

Do not infer that more of the combined OEM/Merbanan initialisation is now safe.
The next RX-side step must be isolated and reviewed independently.  In
particular, EN7570 init/calibration, PLL or block resets, polarity programming,
interrupt enabling, GPON delimiter/guard programming, firmware-ready, MAC,
PLOAM, QDMA and every TX control remain out of scope.  Fibre is not needed for
another register-safety experiment, but it will be required to validate LOS,
RX sync and any progression toward PLOAM O5.
