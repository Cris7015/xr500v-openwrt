# GPON bring-up — phase 18 isolated RSSI-gain/LOS observer (compile-only)

Date: 2026-07-13

## Purpose

Phase 17 proved that reset plus the exact OEM LOS bytes does not sense light,
while stock on the same fibre reaches receive sync and service.  A complete
stock EN7570 dump showed the same LOS registers but one stable receiver delta:
`LA_PWD[18:16] = 5`, written by `mt7570_RSSI_gain_init()` immediately before
LOS initialisation.

The existing non-shipping reset-audit module now has a third mutually-exclusive
mode, `arm_en7570_reset_rssi_los=1`.  This phase is compile-only; no new DT or
image has yet been deployed.

## Fixed sequence

1. require the exact phase-12 28-group baseline and all existing TX gates;
2. self-pin and submit the one OEM four-byte software-reset RMW;
3. snapshot every group and repeat TX postflight;
4. wait 10–12 ms;
5. read `LA_PWD`, change only byte 2 bits 2:0 from zero to five, and issue one
   four-byte write to `0x014`;
6. snapshot all 28 groups, require `LA_PWD = 00 24 05 00`, require every other
   audited byte to match the post-reset snapshot, and repeat TX postflight;
7. cancel LOS on any RSSI transfer, snapshot, isolation or TX-safety error;
8. apply the already-proven five LOS writes with thresholds `0x1c/0x10`;
9. snapshot again, repeat TX postflight and remain pinned with GPIO16 asserted.

The maximum is seven I2C data writes: one reset, one RSSI-gain RMW and five LOS
writes.  The generic write helper rejects every address except the four fixed
RX registers used by those paths.

## Explicit exclusions

The mode contains no ADC calibration, RSSI calibration/sampling, DDMI worker,
MPD calibration, ERC setup, mode switch, PLL/reset MMIO, interrupt enable,
TGEN, APD, bias/modulation current, laser enable, MAC, QDMA or rollback path.
The previous reset-only and reset-then-LOS modes remain available but cannot be
selected together with the new mode.

## Compile audit

```text
package release:        3
kernel:                 6.12.80 SMP MIPS32_R2
module size:            346976 bytes
module SHA-256:         591f4feefa6f2137fec7e452bf457a1fddd542c8f3d93057329afb4dd8545760
depends:                i2c-core
autoload:               none
device image package:   absent
shipping DT match:      absent
router deployment:      none
```

The module compiled successfully with the sanitized build environment and
exposes exactly these three boolean opt-ins:

```text
arm_en7570_reset_audit
arm_en7570_reset_then_los
arm_en7570_reset_rssi_los
```

Before live execution, a temporary DT/image still needs the full XR500v audit:
compressed kernel below `0x2ffe00`, rootfs at `0x300200`, intact 512-byte gap,
valid TrendChip header, no module in rootfs/autoload, and the exact experimental
compatible/DT opt-in.  Because OEM stock has initialized the external EN7570,
the next OpenWrt boot must be a physical power cycle rather than the warm
Bootbase jump before the phase-12 baseline can be required safely.
