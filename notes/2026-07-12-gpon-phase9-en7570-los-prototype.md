# GPON bring-up — phase 9 EN7570 LOS calibration and compile-only prototype

Date: 2026-07-12

> **Superseded safety result:** phase 10 proved that the LOS calibration trigger
> changes autonomous EN7570 state which cannot be undone by restoring the
> visible register bytes.  The stage is now quarantined and always returns
> `-EOPNOTSUPP`.  See
> `2026-07-12-gpon-phase10-en7570-los-nontransactional.md`.

## Result

The XR500v's effective per-unit EN7570 calibration was recovered read-only
from factory NAND and traced through the stock userspace and kernel code.  The
LOS thresholds are:

```text
flash_LOS_high_thld (matrix offset 0x20): 0x1c
flash_LOS_low_thld  (matrix offset 0x24): 0x10
GPON magic          (matrix offset 0x94): 0x07050700
```

These are not the generic rootfs-template values `0x30/0x20`.  A fourth,
mutually exclusive stage was added to the existing compile-only RX prototype.
It implements only the receive-side register effects of OEM
`mt7570_LOS_level_set()`, with explicit per-unit thresholds, fail-closed TX
gates, complete register snapshots and read-back-verified rollback.

Nothing from this phase was loaded on the router.  No firmware was flashed and
no EN7570 register was written.  Fibre remained disconnected.

## Authoritative source identity

The local OEM tree and `HyGw/test` were checked before translating the code:

```text
local:      /home/cristuu/tools/xr500v/en751221-linux26
local HEAD: aea3a43562e8d3dc0335624202fde08d713a18c2
online:     https://github.com/HyGw/test
online HEAD:aea3a43562e8d3dc0335624202fde08d713a18c2
```

The two sources are the same revision.  The relevant OEM files are:

```text
tclinux_phoenix/modules/private/xpon_phy/src/mt7570.c
tclinux_phoenix/modules/private/xpon_phy/inc/mt7570_def.h
tclinux_phoenix/modules/private/xpon_phy/inc/mt7570_reg.h
```

## How stock firmware gets the calibration

The stock rootfs starts with a 400-byte template:

```text
/etc/7570_bob.conf
SHA-256: 7fd6cbd850f1c15ba8957839dbe92bdcd4c9e76b10106f7448ef92971d746aad
template high/low: 0x30 / 0x20
```

`/etc/init.d/rcS` does not leave that template untouched:

```sh
cp -f /etc/7570_bob.conf /tmp/
/usr/bin/tppontool readflashtobobwithcheck
```

The stripped big-endian MIPS `tppontool` binary was disassembled.  Its
read-with-check path uses flash address `0x013a0000`; in the XR500v layout that
is `misc + 0x20000`.  It reads the factory block, validates its leading bytes
and writes the calibrated prefix to `/tmp/7570_bob.conf`.  This agrees with the
stock boot log:

```text
PON FLASH CALIB: data[0]=00 data[1]=00 data[2]=02 data[3]=37
read pon calibration flash success!
```

The source `mt7570.c` then reads `/tmp/7570_bob.conf` directly into a
100-element `uint flash_matrix[]` on the big-endian MIPS CPU.  Consequently,
the calibration is a big-endian word array; it must not be decoded as
little-endian.

## NAND validation

Exactly the same 400 bytes were recovered from three independent local dumps
and from the live OpenWrt MTD device using read-only `dd`:

```text
backup_full/mtd5_misc.bin + 0x20000
backup_full/pre-webflash-2026-06-23/mtd4_misc.bin + 0x20000
mtd/mtd5_misc + 0x20000
live /dev/mtd4 + 0x20000

SHA-256: 401dfdaee77c84649bda100fd5dd85be01c7ea126d0a5cc2116b141c1a07a5e4
```

The beginning of the effective matrix is:

```text
0000: 00 00 02 37 00 00 04 eb 00 00 01 20 00 00 01 00
0010: 00 00 00 08 00 00 00 0e 00 00 12 86 00 00 00 14
0020: 00 00 00 1c 00 00 00 10 ff ff ff ff 00 00 00 01
...
0090: ff ff ff ff 07 05 07 00 ff ff ff ff ff ff ff ff
```

The phase-8 live uninitialised `LOS_CTRL1=06 08 3c 36` therefore did not
contain the factory thresholds.  Its `0x3c/0x36` bytes must not be treated as
calibration data.

## Exact OEM RX/LOS sequence

`mt7570_LOS_level_set()` first calls `mt7570_LOS_init()` and then installs the
two matrix thresholds.  The isolated writes are:

```text
LOS_CTRL1 0x011c:
  byte0 = (old & 0xfe) | 0x01     calibration trigger
  byte1 = (old & 0xe0) | 0x1f     analogue-input stable count

SVADC_PD 0x0024:
  byte3 = (old & 0xfb) | 0x04     ADC revision-2 enable
  byte2 = (old & 0xbf) | 0x40     ADC revision-1 enable

LOS_CTRL2 0x0120:
  byte1 = (old & 0xe0) | 0x1f     LOS confidence
  byte0 = (old & 0x80) | 0x05     LOS signal-detect count

LOS_CTRL1 0x011c:
  byte2 = (old & 0x80) | 0x1c     per-unit high threshold
  byte3 = (old & 0x80) | 0x10     per-unit low threshold
```

This sequence is receive/LOS-only.  It does not itself call the later OEM
TGEN, bias/modulation current, MPD, Tx-SD or APD functions.

## Compile-only stage

The package remains absent from `DEVICE_PACKAGES` and has no autoload entry.
The shipping DTS continues to expose only the passive diagnostic compatible;
there is no matching active platform node and no `econet,allow-rx-only-init`
property.  Per-unit data and an EN7570 phandle are present but inert unless a
dedicated experimental image changes the compatible and adds the opt-in.

The new mutually exclusive parameter is:

```text
arm_en7570_los_init=1
```

Probe will fail unless all of the following are true:

1. exactly one experimental stage is armed;
2. the temporary DT has `econet,allow-rx-only-init`;
3. GPIO16 is an output and physical TX-disable is asserted;
4. `PHYSET3.TXEN` is clear;
5. GPON mode is already selected;
6. rogue-ONU, PRBS and test-frame TX modes are off;
7. xPON interrupts are disabled;
8. the EN7570 client exists and ID register `0x0170` returns `0x03`;
9. high/low thresholds exist, fit in seven bits and satisfy high > low;
10. the LOS calibration trigger is not already active.

Before the first write it snapshots all four bytes of `LOS_CTRL1`, `SVADC_PD`
and `LOS_CTRL2`.  Each I2C write rechecks physical TX-disable and TXEN.  The
stage preserves every bit outside the OEM masks, permits the trigger bit to
self-clear, verifies the final values, and restores all twelve original bytes
on remove or any failure.  Rollback is itself read back and byte-compared; a
failure is reported as an error.

Forbidden paths remain absent:

```text
APD voltage/control
laser bias or modulation current
TGEN / PRBS / test-frame generation
Tx-SD setup
DDMI workers or ADC channel selection
safe-circuit reset
PLL or xPON reset
MAC, QDMA, PLOAM and OMCI
```

## Build validation

The clean cross-build succeeded for MIPS big-endian and produced
`kmod-xr500v-xpon-rx-init` release 4:

```text
packaged module SHA-256: c8bdd9cf11f100e3c624fdcee1ec12810ab502156dec7b6360d20ba81233a09d
APK SHA-256:             af20aa1a568373efe1176ac693f551305ed046e0bdac0bd17c88448c6cf9b442
module text/data/bss:    10519 / 472 / 16 bytes
```

The undefined-symbol audit contains the expected I2C, GPIO, MMIO, DT,
debugfs and platform-driver primitives.  It contains no APD, laser, reset,
PLL, QDMA, MAC, IRQ or GPON control symbol.  The module has exactly two
`i2c_transfer` relocations—the single read helper and the single guarded write
helper—and the pre-existing three `iowrite32` relocations, one for each of the
other mutually exclusive xPON stages.

The kernel dependencies were rebuilt cleanly, followed by the final module
package.  The resulting compressed kernel remains within the XR500v limit:

```text
vmlinuz.bin size:        2,936,336 bytes
allowed maximum:         0x2ffe00 = 3,145,216 bytes
remaining margin:        208,880 bytes
vmlinuz SHA-256:         520c07170c99d137ec5b2c562eab91bb5d22d10f8a90368921ccd3067a452acc
```

The preprocessed XR500v DTS compiled to an 11,634-byte DTB.  Decompilation
confirmed the passive `airoha,en7570-diag` client, its phandle and the inert
`0x1c/0x10` threshold properties.  The only `dtc` messages were two existing
`avoid_unnecessary_addr_size` warnings in the switch tree; there were no new
errors or GPON warnings.

No resulting artifact is authorised for live loading by this note.

`checkpatch.pl` result for the final source:

```text
0 errors, 0 warnings
```

## Next controlled experiment

The next live test, if performed, should use a dedicated temporary image with
the active compatible and DT opt-in, but no autoload.  The safe order is:

1. boot with fibre disconnected and re-confirm every TX gate;
2. capture passive xPON and EN7570 baselines;
3. load only the LOS stage and record its before/current register snapshots;
4. observe LOS debug/state first without fibre;
5. connect fibre while TX-disable remains asserted and compare EN7570 LOS,
   xPON LOS, FSM, sync and passive counters;
6. disconnect fibre, remove the module and require exact rollback;
7. restore the stable firmware.

Even a successful optical RX/LOS response would not authorise enabling the
laser or attempting O5.  It would only prove that the receive analogue boundary
has been crossed; SerDes/digital sync is the following isolated boundary.
