# GPON bring-up — phase 10 EN7570 LOS trigger is non-transactional

Date: 2026-07-12

## Result

The first fibre-disconnected execution of the isolated EN7570 LOS stage failed
closed, but proved that the OEM LOS calibration trigger cannot be modeled as a
reversible register experiment.  Restoring all visible control bytes does not
undo the EN7570's autonomous calibration state.

The stage is therefore quarantined in release 5: selecting
`arm_en7570_los_init=1` now fails before mapping or I2C access with
`-EOPNOTSUPP`.  It must not be tried again until the prerequisite EN7570 reset,
ADC and RSSI sequence is understood and a hardware-reset recovery boundary is
available.

No fibre was connected.  GPIO16 TX-disable remained asserted, xPON TXEN stayed
clear, and every xPON TX generator remained off throughout.

## Temporary image audit

The dedicated image used only build-tree changes:

```text
compatible:             econet,en751221-xpon-rx-init-experimental
DT allow property:      present
EN7570 phandle:         present
LOS thresholds:         high=0x1c low=0x10
active module autoload: absent
active module SHA-256:  c8bdd9cf11f100e3c624fdcee1ec12810ab502156dec7b6360d20ba81233a09d
patched image SHA-256:  a270f3ac97adc56e37f20620a96ba6d5efc3bec8a5f99a563cc00f03f3d0016d
image size:             11102958 bytes
compressed kernel:      2936336 bytes
kernel headroom:        208880 bytes
SquashFS offset:        0x300200
512-byte gap:           present
TrendChip header:       valid
```

The SquashFS was inspected before flashing.  It contained the exact audited
active module, both normal passive diagnostics, and no active-module entry in
`modules.d` or `modules-boot.d`.

An initial image candidate was rejected without flashing because a single-
profile build omitted the device-default packages.  The build was repeated
with `TARGET_MULTI_PROFILE` and `TARGET_PER_DEVICE_ROOTFS`; the final inspected
image contained Wi-Fi, PON-I2C and both diagnostics like the stable image.

## Pre-write baseline

The temporary image booted normally.  The passive xPON driver was bound through
`driver_override` before loading the active module:

```text
PHYSET3:              0x4581e114
XPON_SETTING:         0x0000014f
mode/FSM/sync:        GPON / 3 / 0x00
TXEN:                 clear
GPIO16 TX-disable:    output / asserted
rogue/PRBS/testframe: off / 0 / 0
xPON interrupts:      0
RX counters:          all zero
passive regs SHA-256: 0733ff5474522cfd08e606458f40fbef8d703d79296baa30407b041a1d58a30b
```

The EN7570 baseline immediately before the active stage was:

```text
LOS status:           0
LOS_CTRL1:            06 08 3c 36
LOS_CTRL2:            10 05 00 00
LOS calibration timer:ff ff ff ff
LOS timeout count:    00 00 00 00
LOS timeout:          00 00 00 00
ADC probe/control:    zero
```

## Probe failure and fail-closed response

After unbinding the passive platform driver, the module was loaded with only:

```text
arm_en7570_los_init=1
```

No active debugfs node appeared because probe aborted:

```text
xr500v-xpon-rx-init: EN7570 rollback verification failed
xr500v-xpon-rx-init: error -EIO: cannot apply selected RX stage
```

The active driver did not remain bound.  It was unloaded and the passive xPON
driver was rebound immediately.  All xPON TX safety fields remained identical
to baseline.

The EN7570 visible control bytes were mostly restored, but autonomous fields
were not:

```text
                         before             after failed probe
LOS status               0                  1
LOS_CTRL1                 06 08 3c 36        06 08 3c 36
SVADC_PD                  not captured       00 00 01 00
LOS_CTRL2                 10 05 00 00        10 05 23 00
LOS timeout               00 00 00 00        3e 00 00 00
ADC probe/control         zero               zero
```

`LOS_CTRL2.byte2=0x23` and timeout `0x3e` stayed stable across repeated reads;
only LOS debug bytes continued to vary.  The attempted ADC enable bits were not
left set in `SVADC_PD`, but the initial `LOS_CTRL1` trigger had already started
an internal process.  Register rollback cannot reverse that event.

## Software reboot does not reset EN7570

The router was sysupgraded back to the known stable image:

```text
b5d513e7ef47259321a53f2e07432424240d5e7bdcb1142611b784f374ef4792
```

The stable kernel and passive xPON safety state returned normally, but the
EN7570 state survived the software reboot unchanged:

```text
LOS=1
LOS_CTRL1=06 08 3c 36
SVADC_PD=00 00 01 00
LOS_CTRL2=10 05 23 00
LOS timeout=3e 00 00 00
```

A physical power cycle is required to establish whether board power removal
returns the EN7570 to the earlier LOS=0 / CTRL2 byte2=0 / timeout=0 baseline.
No software reset will be attempted merely to recover this experiment because
the OEM reset sits at the start of a larger optical init and has not yet been
classified for TX safety.

## Missing prerequisites in the isolated model

OEM `mt7570_init()` does not call `mt7570_LOS_level_set()` on the bootloader-
retained device.  Its order is:

1. `mt7570_sw_reset()` — described by OEM as resetting all EN7570 registers;
2. TIAGAIN setup and ERC filter;
3. MPD-current calibration;
4. ADC calibration, explicitly delayed from the software reset;
5. RSSI calibration;
6. GPON mode and RSSI gain init;
7. only then `mt7570_LOS_level_set()`.

The later items mix RX prerequisites with MPD/TX-adjacent calibration.  They
cannot be imported wholesale.  The next design task is to classify the reset
and the minimum RX-only ADC/RSSI prerequisites, including which fields are
autonomous status rather than writable configuration.

## Diagnostic and code changes

The passive EN7570 diagnostic release 4 adds a single pointer-only four-byte
read of `SVADC_PD` at `0x0024`.  It still has no write, reset, latch, ADC
selection, laser, APD or DDMI path.

```text
release-4 module SHA-256: 258019a84442f5ea710de32804afd2e6a624ce22e79e6cd120f3e03989b3f655
I2C transfer relocations: 1 (pointer-only read helper)
checkpatch:               0 errors, 0 warnings
```

The active RX-init release 5 retains the phase-10 code for analysis but rejects
the EN7570 LOS stage before all DT, GPIO, MMIO and I2C access.  The previous ESD,
polarity and counter stages remain independently guarded and unchanged.

```text
release-5 module SHA-256: cd23bef76f80986a597ac4741d66ca2222ecf84117500be1f54292b3c8097d9f
quarantine diagnostic:   embedded in the built module
checkpatch:               0 errors, 0 warnings
```

## Safe end state

At the end of software work:

```text
firmware:              stable b5d513
experimental module:   absent
xPON TXEN:             clear
GPIO16 TX-disable:     asserted
TX generators/IRQ:     off
fibre:                 disconnected / in use by the Movistar router
EN7570 internal state: calibration side effect remains until physical power cycle
```
