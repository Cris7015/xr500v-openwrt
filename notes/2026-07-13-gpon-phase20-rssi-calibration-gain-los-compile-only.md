# GPON bring-up — phase 20 RSSI-calibration/gain/LOS observer (compile-only)

Date: 2026-07-13

## Purpose

Phase 19 proved with a live connected/disconnected/reconnected fibre series
that the isolated static OEM RSSI gain is not sufficient to make the EN7570
LOS block sense downstream light.  The last bounded RX-only operation before
the OEM moves into mode setup and later APD power is its transient RSSI
calibration.

Release 4 of the non-shipping reset-audit module adds a fourth mutually
exclusive mode:

```text
arm_en7570_reset_rssi_cal_los=1
```

This phase only implements and compiles that observer.  It does not add a DT
node, build or flash an image, load a module, write the router, or claim a live
hardware result.

## Exact bounded sequence

After the existing exact 28-group baseline, TX preflight, one OEM reset write
and 10–12 ms settling interval, the new mode reproduces only
`mt7570_RSSI_calibration()`:

| Step | Register | Width | Transition |
|---:|---:|---:|---|
| 1 | `LA_PWD 0x014` | 2 | `00 24 -> 00 34`, RSSI calibration on |
| 2 | `LA_PWD 0x014` | 2 | `00 34 -> 00 74`, RSSI V-mode on |
| 3 | `SVADC_PD 0x024` | 1 | `00 -> 02`, select RSSI ADC |
| 4 | `ADC_LATCH 0x159` | 1 | set bit 4, then read Vref at `0x154` |
| 5 | `LA_PWD 0x014` | 2 | `00 74 -> 00 34`, RSSI V-mode off |
| 6 | `ADC_LATCH 0x159` | 1 | set bit 4, then read V at `0x154` |
| 7 | `LA_PWD 0x014` | 2 | `00 34 -> 00 24`, calibration off |
| 8 | `SVADC_PD 0x024` | 1 | `02 -> 00`, restore ADC mux |

All three initial values are read and validated before the first calibration
write.  Intermediate readbacks require `LA_PWD` to pass through exactly
`00 34`, `00 74` and `00 34`, require the RSSI ADC selector to remain `02`,
and require the latch to self-clear before it is retriggered.

Eleven captured stock boots of this same XR500v reported Vref `0x020a` and V
`0x0284` or `0x0285`.  The observer therefore rejects results outside these
deliberately bounded windows:

```text
Vref:   0x01e0 .. 0x0230
V:      0x0250 .. 0x02c0
V-Vref: 0x0060 .. 0x00a0
```

After restoration it reads back only `LA_PWD`, `SVADC_PD`, `ADC_PROBE` and
`PROBE_CONTROL`.  The three control groups must equal the post-reset snapshot;
the read-only ADC result may change only in its low 16 bits.  This directed
check replaces a 28-group intermediate snapshot so a possible transient
analogue effect is not consumed by dozens of unrelated I2C reads.

Only if calibration, bounded values, restoration, directed readback and TX
postflight all pass does the mode issue the already-audited gain write.  It
then reads back only `LA_PWD`, requires gain five exactly, repeats TX
postflight, immediately applies the five proven LOS writes, waits 20–25 ms and
takes the final full snapshot.  The older RSSI-gain mode retains its complete
intermediate snapshot and verification unchanged.

The successful maximum is exactly 15 EN7570 data-write attempts:

```text
1 reset + 8 transient RSSI calibration + 1 RSSI gain + 5 LOS = 15
```

Any transfer, value, restoration, readback, isolation or TX-gate error cancels
the remaining gain/LOS stages.  Every individual write now rechecks physical
GPIO16 TX_DISABLE, xPON TXEN, rogue-TX test, PRBS TX, test-frame TX and xPON
interrupt enable immediately before the I2C transfer.

## Write whitelist and exclusions

Partial calibration writes are accepted only for these exact address/length
pairs:

```text
LA_PWD    0x014 / 2 bytes
SVADC_PD  0x024 / 1 byte
ADC_LATCH 0x159 / 1 byte
```

The existing four-byte RX helper remains restricted to `LA_PWD`, `SVADC_PD`,
`LOS_CTRL1` and `LOS_CTRL2`; reset remains restricted to `SW_RESET`.

There is no full ADC-bandgap calibration, TIA-gain programming, ERC, MPD,
DDMI worker, APD, TGEN, bias/modulation current, laser enable, xPON MMIO write,
interrupt enable, MAC, QDMA, OMCI or software rollback path.  The module stays
pinned after the first attempted write, holds TX_DISABLE asserted and treats a
physical power cycle as the only recovery boundary.

## Compile audit

The package was cleaned and rebuilt with the Windows paths removed:

```sh
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make -j4 package/xr500v-en7570-reset-audit/compile
```

```text
package release:             4
kernel/vermagic:             6.12.80 SMP preempt MIPS32_R2 32BIT
source SHA-256:              b4504d2eb9b978e9a01803bad2729551735a0a8d82cffbfbd3758419a41d7309
build module size:           364748 bytes
build module SHA-256:        67bffe3ca3d2bec790b706f055333ec75cfb7e0ab03f3ebc5c8f6d264ba8a75c
packaged module size:        21576 bytes
packaged module SHA-256:     cd8ad43d12a1f854f9e5bc97c16502730aa48370ebd50c84b714e0dae4a0761d
APK size:                    9980 bytes
APK SHA-256:                 fa71acb063ac1a1fbd37f451c89be5485f9be9ba07d19f80825d06365bcd75b8
depends:                     i2c-core
checkpatch strict:           0 errors, 0 warnings, 0 checks
git diff --check:            clean
autoload:                    none
shipping DT match/opt-in:    absent
device image build:          none
router deployment:           none
```

`modinfo` exposes exactly the four mutually exclusive boolean parameters:

```text
arm_en7570_reset_audit
arm_en7570_reset_then_los
arm_en7570_reset_rssi_los
arm_en7570_reset_rssi_cal_los
```

## Interpretation limit and next boundary

The OEM stores Vref and V to calculate `RSSI_factor`, but that software value
does not feed LOS setup anywhere in the available call graph.  This experiment
therefore tests only the weak possibility that traversing the calibration
leaves a useful hidden analogue condition.  A negative live result would not
justify repeating it or adding unrelated ERC/MPD/TIA operations; it would move
the physical receiver focus to APD power and its separate high-voltage safety
audit.

Before any live execution, a temporary image still needs the complete XR500v
audit: compressed kernel below `0x2ffe00`, SquashFS at combined-file offset
`0x300200`, intact 512-byte gap, valid TrendChip header, no module in rootfs or
autoload, and only the explicit experimental compatible/DT opt-in.  The router
currently remains on the passive safe image.

Phase 18 introduced the static gain observer and phase 19 closed it live:
[`phase 18`](2026-07-13-gpon-phase18-rssi-gain-los-compile-only.md) and
[`phase 19`](2026-07-13-gpon-phase19-rssi-gain-los-live.md).
