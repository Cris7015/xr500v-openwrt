# GPON bring-up — phase 19 live RSSI-gain/LOS result

Date: 2026-07-13

## Question

Phase 17 proved that the live Movistar fibre and the XR500v optical hardware
are good under OEM stock, while reset plus the isolated OEM LOS sequence under
OpenWrt could not distinguish connected from disconnected fibre.  The stock
EN7570 dump exposed one stable receiver delta immediately before LOS setup:
`LA_PWD[18:16] = 5`, programmed by `mt7570_RSSI_gain_init()`.

Phase 19 executed the phase-18 guarded observer once to answer the narrow
question: is that single RSSI-gain RMW the missing prerequisite which makes
the already-proven LOS sequence sensitive to downstream light?

## Image and flash audit

The temporary image used only the experimental xPON compatible and explicit
DT opt-in.  The reset-audit package and module remained absent from its
SquashFS and autoload; the exact module was copied to `/tmp` after boot.

```text
experimental image SHA-256:
44a4710bc1f361b3d260da0fe02d2c3ee41e704e9421d2874a8c73a52b8054dc

module SHA-256:
591f4feefa6f2137fec7e452bf457a1fddd542c8f3d93057329afb4dd8545760

compressed kernel:      2948030 bytes
safe kernel headroom:    197186 bytes
rootfs file offset:      0x300200
required 512-byte gap:   present
TrendChip header:        valid
```

The first stock-side automation attempt correctly performed no NAND writes
because TFTP was unavailable, but the old host script still printed a false
success banner.  After enabling TFTP, both payloads were transferred with
explicit return-code and size checks, written to slot B with separate MTD
return-code gates, then read back in full:

```text
kernel1:  3145728 bytes, MTD RC=0, readback SHA-256
          065041e64a38eb10d58c3421daa3bd24d2e5e6780b0baf38cd357bbaa5d674dd
rootfs1: 16777216 bytes, MTD RC=0, readback SHA-256
          dec5a53f683c0d1a2ae9cd831a59d94580f0aa22fb0749f005fe92ddbb8d6dc8
```

Each readback was byte-identical to its source payload.  In particular,
`rootfs1` began with `hsqs`: the 512-byte separation belongs to the combined
sysupgrade file layout (`hsqs` at file offset `0x300200`) and is intentionally
removed when the standalone rootfs partition payload is sliced.

## Cold baseline

The first OpenWrt boot was a physical power cycle with fibre connected.  The
shipping diagnostic module was passive and the experimental module was not
present.  The critical retained groups were the exact phase-12 cold baseline:

```text
LA_PWD       00 24 00 00
SVADC_PD     00 00 01 00
LOS_CTRL1    06 08 3c 36
LOS_CTRL2    10 05 00 00
LOS_TIMEOUT  00 00 00 00
ERC_FILTER   3f 2f 0f 00
```

Twenty rapid passive reads reported the raw EN7570 LOS bit clear.  That is not
evidence of light before analogue initialisation: phases 8 and 17 already
showed that the cold/uninitialised bit does not track fibre presence.  The
guarded module subsequently compared all 28 groups and all TX preconditions
before crossing the first-write boundary.

## One-shot execution

The only active command was:

```sh
insmod /tmp/xr500v-en7570-reset-audit.ko \
  arm_en7570_reset_rssi_los=1
```

Every gate and operation succeeded:

```text
operation:               EN7570 reset then RSSI gain then LOS
reset payload:           01 00 00 00
I2C write attempts:      7
reset result/snapshot/TX postflight:       0 / 0 / 0
RSSI attempted:          yes
RSSI result/snapshot/isolation/postflight: 0 / 0 / 0 / 0
LOS attempted:           yes
LOS result/snapshot/postflight:            0 / 0 / 0
thresholds:              high=0x1c low=0x10
module pinned:           yes
physical TX_DISABLE:     asserted
xPON TXEN:               clear
```

The reset again changed none of the 28 visible groups.  The RSSI isolation
gate proved that its sole delta was `LA_PWD 00 24 00 00 -> 00 24 05 00`.
The five LOS writes then produced the same coherent state as phase 16:

```text
SVADC_PD     00 00 41 04
LOS_CTRL1    06 1f 1c 10
LOS_CTRL2    05 1f 22 00
LOS_TIMEOUT  3e 00 00 00
LOS_DBG[3]   89
LOS          asserted
```

No ADC/RSSI calibration or sampling, MPD calibration, ERC setup, DDMI, APD,
TGEN, current, laser, MAC, QDMA or xPON MMIO write was available to this mode.

## Same-boot fibre A/B/A

With the module pinned and physical TX disabled, three one-second series were
captured without rebooting or issuing another write:

| Series | Fibre | Samples | LOS asserted | `LOS_DBG[1]` | `LOS_DBG[3]` | Timeout |
|---|---:|---:|---:|---:|---:|---:|
| A | connected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |
| B | disconnected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |
| A2 | reconnected | 20 | 20/20 | `00` (20/20) | `89` (20/20) | `3e000000` (20/20) |

The free-running bytes 0 and 2 varied in all three series:

| Series | Byte 0 mean ± population SD, range | Byte 2 mean ± population SD, range |
|---|---:|---:|
| A | 31.80 ± 16.49, 1–55 | 79.80 ± 32.20, 18–123 |
| B | 29.60 ± 17.39, 5–63 | 64.70 ± 33.35, 10–122 |
| A2 | 32.30 ± 16.31, 3–61 | 64.80 ± 38.99, 8–120 |

Byte 0 has strongly overlapping distributions.  Byte 2 was higher in the
first connected series, but the reconnected A2 mean was effectively identical
to disconnected B despite similarly broad ranges.  This is temporal noise or
drift, not a reproducible fibre response.  `LA_PWD` stayed `00 24 05 00`,
`SAFE_PROTECT` stayed `ff 8f`, and Tx-fault stayed asserted throughout.

## Recovery proof

The audited passive phase-14 image was installed with sysupgrade:

```text
SHA-256: 0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d
```

The automatic warm reboot selected the passive DT and removed every active
software path, but the EN7570 still retained `LA_PWD=...05`, the LOS controls
and timeout `3e`.  This is direct same-session confirmation that a warm reboot
is not a recovery boundary for this external chip.  A physical power cycle
then restored the complete cold baseline shown above.

Final state on the router:

```text
DT compatible:           econet,en751221-xpon-phy-diag
experimental DT opt-in:  absent
active module/status:    absent
physical TX_DISABLE:     asserted
xPON TXEN:               clear
passive MMIO writes:     0
EN7570 data writes:      0
```

## Conclusion

The isolated OEM RSSI-gain RMW is **not sufficient** to make LOS sensitive to
the live fibre.  It executed exactly as intended and did not change the phase-16
outcome.  The missing dependency lies earlier/deeper in the receiver analogue
initialisation or calibration path; another reset, another isolated LOS run,
or a different static gain value is not justified by these results.

Any next active phase must first identify a bounded RX-only prerequisite from
the OEM call graph and calibration inputs.  TX/APD/current/laser paths remain
out of scope, and every non-transactional experiment still requires a physical
power-cycle recovery plus restoration of the passive image.
