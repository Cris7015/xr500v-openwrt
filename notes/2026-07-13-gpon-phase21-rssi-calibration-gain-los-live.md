# GPON bring-up — phase 21 live RSSI-calibration/gain/LOS result

Date: 2026-07-13

## Question

Phase 19 proved that the isolated static OEM RSSI gain was not sufficient to
make the EN7570 LOS block distinguish the live Movistar fibre from an open
optical input.  Phase 20 implemented the last bounded receiver-only operation
before a separate APD/high-voltage study: the transient OEM RSSI Vref/V
calibration, followed only on success by the already-audited static gain and
LOS setup.

Phase 21 executed that phase-20 observer exactly once to answer two questions:

1. Does the transient calibration reproduce the ADC values observed under
   stock on this same EN7570?
2. Does traversing that calibration leave an analogue condition which makes
   LOS respond to connected, disconnected and reconnected fibre?

## Image and deployment audit

The exact phase-18 experimental image was reused because the phase-20 module
is copied externally and its kernel and experimental DT are byte-identical to
the already-audited build.  The module was absent from SquashFS and autoload.

```text
experimental image SHA-256:
44a4710bc1f361b3d260da0fe02d2c3ee41e704e9421d2874a8c73a52b8054dc

phase-20 module SHA-256:
67bffe3ca3d2bec790b706f055333ec75cfb7e0ab03f3ebc5c8f6d264ba8a75c

module size:            364748 bytes
compressed kernel:      2948030 bytes
safe kernel headroom:    197186 bytes
rootfs file offset:      0x300200
required 512-byte gap:   present
TrendChip header:        valid
```

The embedded DTB exactly matched the current experimental build, exposed
`econet,en751221-en7570-reset-audit-experimental` and contained the explicit
opt-in.  Neither the image manifest nor SquashFS contained the active module.
The router booted with the exact cold baseline and zero diagnostic writes.

## One-shot execution

The only active command was:

```sh
insmod /tmp/xr500v-en7570-reset-audit-phase20.ko \
  arm_en7570_reset_rssi_cal_los=1
```

It was not repeated and the module was not removed.  Every gate, transfer,
directed readback, oracle check and postflight succeeded:

```text
operation:                 EN7570 reset then RSSI calibration/gain then LOS
I2C write attempts:        15
reset/result/postflight:   0 / 0 / 0
calibration stages:        all 0
static-gain stages:        all 0
LOS stages:                all 0
RSSI ADC Vref:             0x020a
RSSI ADC V:                0x0285
RSSI ADC delta:            0x007b
calibration readback LA:   00 24 00 00
calibration readback ADC:  85 02 00 00
gain readback LA:          00 24 05 00
module pinned:             yes
physical TX_DISABLE:       asserted
xPON TXEN:                 clear
```

The ADC result is an exact member of the same-device stock oracle: eleven
stock boots reported Vref `0x020a`, V `0x0284` or `0x0285` and delta `0x007a`
or `0x007b`.  This proves the transient calibration path, mux selection,
latching, sampling and restoration ran as intended under OpenWrt.

After the static gain and five LOS writes, the final state was coherent and
matched phases 16 and 19:

```text
LA_PWD       00 24 05 00
SVADC_PD     00 00 41 04
LOS_CTRL1    06 1f 1c 10
LOS_CTRL2    05 1f 22 00
LOS_TIMEOUT  3e 00 00 00
APD_DAC      00 08 00 00  (unchanged)
ERC_FILTER   3f 2f 0f 00  (unchanged)
LOS_DBG[3]   89
LOS          asserted
```

No APD, full ADC-bandgap calibration, ERC/MPD, DDMI worker, TGEN,
bias/modulation current, laser, xPON MMIO, interrupt, MAC or QDMA active path
was available to the observer.

## Same-boot fibre A/B/A2

With the module pinned and physical TX disabled, three one-second series were
captured without rebooting or issuing another write:

| Series | Fibre | Samples | LOS asserted | `LOS_DBG[1]` | `LOS_DBG[3]` | Timeout |
|---|---:|---:|---:|---:|---:|---:|
| A | connected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |
| B | disconnected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |
| A2 | reconnected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |

The free-running bytes 0 and 2 varied but did not reproduce a fibre response:

| Series | Byte 0 mean ± population SD, range | Byte 2 mean ± population SD, range |
|---|---:|---:|
| A | 29.30 ± 19.57, 1–63 | 67.80 ± 36.34, 2–122 |
| B | 33.20 ± 19.17, 5–63 | 65.35 ± 31.91, 17–125 |
| A2 | 30.00 ± 18.04, 5–61 | 54.55 ± 30.03, 3–110 |

All distributions overlap strongly.  More importantly, every stable status
field remained identical across the physical A/B/A2 transition.  `LA_PWD`,
`SAFE_PROTECT`, APD, LOS controls, timeout and Tx-fault were unchanged, and
the final active-module audit still reported exactly 15 writes, TX_DISABLE
asserted and xPON TXEN clear.

## Recovery proof

The audited passive phase-14 image was transferred, hashed on the router and
accepted by `sysupgrade -T` before installation:

```text
passive image SHA-256:
0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d

TrendChip header:        valid
rootfs file offset:      0x300200
required 512-byte gap:   present
sysupgrade image test:   rc=0
```

The automatic warm reboot selected the passive DT and removed the opt-in and
active module, but the external EN7570 retained `LA_PWD=...05`, the LOS
controls and timeout `0x3e`.  A physical ten-second power cycle restored the
complete cold baseline:

```text
DT compatible:           econet,en751221-xpon-phy-diag
experimental DT opt-in:  absent
active module/status:    absent
LA_PWD:                  00 24 00 00
SVADC_PD:                00 00 01 00
LOS_CTRL1:               06 08 3c 36
LOS_CTRL2:               10 05 00 00
LOS_TIMEOUT:             00 00 00 00
APD_DAC:                 00 08 00 00
ERC_FILTER:              3f 2f 0f 00
physical TX_DISABLE:     output-high / asserted
xPON TXEN:               clear
rogue/PRBS/test-frame:   disabled
xPON interrupt enable:   0x00
passive MMIO writes:     0
EN7570 data writes:      0
```

The router was left running that passive image with the fibre reconnected.

## Conclusion and next boundary

The transient OEM RSSI calibration is **correctly reproduced but not
sufficient** to make LOS sensitive to downstream light.  It returned the
exact stock ADC oracle, restored its controls and passed every isolation gate,
yet connected, disconnected and reconnected fibre were indistinguishable in
all 60 stable samples.  Repeating reset, calibration, gain or LOS is not
justified.

This closes the last deliberately bounded no-APD receiver experiment.  The
remaining physically plausible dependency is the APD receiver-bias path, but
that is a separate high-voltage safety boundary rather than a routine next
write.  Any active APD phase must begin with a read-only OEM call-graph,
register, board-rail, limit and failure-mode audit; it must not simply copy the
stock APD enable sequence.  TX/current/laser paths remain out of scope.

The implementation and compile audit are recorded in
[`2026-07-13-gpon-phase20-rssi-calibration-gain-los-compile-only.md`](2026-07-13-gpon-phase20-rssi-calibration-gain-los-compile-only.md).

The subsequent read-only APD and full-register audit is recorded in
[`2026-07-13-gpon-phase22-apd-safety-audit-passive-map.md`](2026-07-13-gpon-phase22-apd-safety-audit-passive-map.md).
