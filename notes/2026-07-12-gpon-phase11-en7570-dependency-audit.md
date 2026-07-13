# GPON bring-up — phase 11 EN7570 dependency audit and passive baseline

Date: 2026-07-12

## Result

The OEM EN7570 init order is not a dependency chain for LOS.  Static analysis
shows three separate domains that happen to share one legacy init function:

1. `mt7570_sw_reset()` resets the complete EN7570 register file;
2. ADC and RSSI calibration produce software conversion constants for DDMI;
3. `mt7570_LOS_init()` directly starts an autonomous LOS calibration engine.

Neither `ADC_slope`, `ADC_offset`, nor `RSSI_factor` is consumed by the LOS
setup path.  TIAGAIN configures the receiver front-end, but the OEM comment
requiring it before calibration refers specifically to MPD-current calibration.
ERC filter setup and MPD-current calibration are transmitter/monitor-diode
work and are not LOS prerequisites.

This means the phase-10 failure was not caused by omitting ADC/RSSI software
constants.  The likely missing boundary is the full-device reset/default state
expected by the autonomous LOS engine.  That reset is itself non-transactional
and remains unsafe to test until its effect on laser/APD/TX defaults is known.

No active EN7570 write was performed in this phase.  Fibre was disconnected.

## Sources audited

- OEM tree: `en751221-linux26`, commit
  `aea3a4359ca9034357fca59faee3c206140e736c`;
- OEM files: `mt7570.c`, `mt7570_reg.h`, and `phy_api.h`;
- merbanan `econet-eth-mainline`, commit
  `2e410ea62cfdfcae4979dc1e3f17e55238f9037b`;
- local phase-10 active prototype and passive EN7570 diagnostic.

## Dependency classification

### RX/LOS configuration

- `mt7570_TIAGAIN_set()` changes `TIAMUX+1[7:6]` from calibration flash.  It is
  relevant to receiver gain but has no data dependency on LOS thresholds.
- `mt7570_RSSI_gain_init()` sets `LA_PWD+2[2:0]=5`.  It selects the normal RSSI
  front-end gain and is independent of the ADC calibration result.
- `mt7570_LOS_init()` writes LOS trigger/stability, ADC revision, confidence,
  and count fields.  Triggering it starts autonomous state that visible-byte
  rollback cannot undo.

### DDMI-only calibration

- ADC calibration samples the 1.76 V and 0.875 V bandgaps and computes
  `ADC_slope`/`ADC_offset` in software.
- RSSI calibration samples Vref/V and computes `RSSI_factor` in software.
- Those values are used by temperature, voltage, and optical-power reporting,
  not by `mt7570_LOS_init()` or threshold programming.

### TX/APD forbidden for isolated RX work

- ERC filter programming targets the transmitter extinction-ratio loop.
- MPD-current calibration selects the monitor-photodiode ADC path.
- TGEN, bias/modulation currents, MPD targets, Tx-SD and APD init all occur
  after LOS in the OEM mode branch and must not be imported into an RX-only
  experiment.

## Reset audit

OEM `mt7570_sw_reset()` reads four bytes at `0x0300`, applies
`byte0 = (byte0 & 0xf8) | 0x01`, and writes all four bytes back.  Its own
description says that it resets all EN7570 registers.  A normal OpenWrt reboot
did not clear phase-10 autonomous LOS state; only removal of physical power did.

The current merbanan implementation instead writes the single fixed byte
`0x01`.  It therefore does not yet preserve the OEM's upper three bits or its
four-byte transaction semantics.  This is another reason not to use that reset
implementation as a live reference yet.

The merbanan ADC helper also clears the selected mux field after sampling rather
than restoring the byte captured before selection, and error exits can skip
restore entirely.  This is compatible with a full fresh init assumption, but
is not transactional on a bootloader-retained device such as the XR500v.

## Passive diagnostic release 5

Release 5 adds pointer-only reads for the registers needed by this audit:

```text
TIAMUX            0x0000  08 00 10 02
LA_PWD            0x0014  00 24 00 00
BGCKEN             0x001c  55 55 55 a5
ERC_FILTER_CTRL    0x016c  3f 2f 0f 00
SW_RESET           0x0300  00 00 00 00
```

The existing expanded baseline remained:

```text
LOS                0
LOS_CTRL1          06 08 3c 36
SVADC_PD           00 00 01 00
LOS_CTRL2          10 05 00 00
LOS timeout/count  00 00 00 00 / 00 00 00 00
ADC probe/control  00 00 00 00 / 00 00 00 00
```

Two captures two seconds apart had identical SHA-256 after excluding only the
known free-running `los_debug_raw` line:

```text
65f17e8c4e9a3232f0bd35366d44a5fb3d167f42fe12c9aaa52bbe62d80f104a
```

The module contains one I2C data-path relocation, `i2c_transfer`, and the
driver constructs only a two-byte register-pointer write followed by a read.
It has no data-write helper.  Build and audit results:

```text
module SHA-256: 840e306cac22deeca3d8905d0dbaba220b67ed5e2d2a1ba1ec923a19e8cdc235
checkpatch:     0 errors, 0 warnings
kernel:         6.12.80
```

## Safe end state and next boundary

The router remains on stable firmware.  The release-5 passive module is loaded
temporarily from `/tmp`.  xPON TXEN is clear, GPIO16 TX-disable is asserted,
PRBS/test-frame generation is off, interrupt enable is zero, and all RX/TX
counters are zero.

Do not retry isolated LOS and do not issue EN7570 software reset yet.  The next
safe work item is compile-only design of a reset-aware state machine with:

1. a pre-reset inventory of every laser/APD/TX control byte;
2. an independently asserted external TX-disable before reset;
3. immediate post-reset readback before any optical init write;
4. physical-power-cycle recovery treated as the only proven rollback.

