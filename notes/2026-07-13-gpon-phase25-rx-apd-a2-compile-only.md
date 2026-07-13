# GPON bring-up — phase 25 guarded RX/APD A2 observer (compile only)

Date: 2026-07-13

## Status

The phase-25 one-shot observer is implemented, independently audited, built
and packaged into a dedicated experimental XR500v image.  It has **not** been
loaded on hardware yet.

This phase combines only operations already bounded by earlier live runs on
this same unit:

1. the phase-16 EN7570 reset and LOS path;
2. the phase-21 exact RSSI calibration oracle and phase-19 gain state;
3. the phase-6 EN7570 RX signal-detect polarity;
4. the phase-24 three-write APD bootstrap ending at factory-derived code
   `0xa2`.

The combined run asks whether the receiver can report useful LOS, sync or FEC
state with the authorised fibre connected while all optical-transmitter paths
remain fail-closed.  It does not implement PLOAM, O5, OMCI or a network data
path, and it must not be described as GPON service.

## Immutable write budget

The module exposes no writable debugfs or sysfs ABI and accepts no payload.
Its sole module parameter is the read-only boolean
`arm_en7570_rx_apd_a2=1`; the matching device-tree node has a distinct
experimental compatible and immutable opt-in.

The complete I2C whitelist is exactly 18 writes, in this order:

| Step | Register | Length | Fixed bytes |
|---:|---:|---:|---|
| 1 | `0x0300` | 4 | `01 00 00 00` |
| 2 | `0x0014` | 2 | `00 34` |
| 3 | `0x0014` | 2 | `00 74` |
| 4 | `0x0024` | 1 | `02` |
| 5 | `0x0159` | 1 | `10` |
| 6 | `0x0014` | 2 | `00 34` |
| 7 | `0x0159` | 1 | `10` |
| 8 | `0x0014` | 2 | `00 24` |
| 9 | `0x0024` | 1 | `00` |
| 10 | `0x0014` | 4 | `00 24 05 00` |
| 11 | `0x011c` | 4 | `07 1f 3c 36` |
| 12 | `0x0024` | 4 | `00 00 01 04` |
| 13 | `0x0024` | 4 | `00 00 41 04` |
| 14 | `0x0120` | 4 | `05 1f 00 00` |
| 15 | `0x011c` | 4 | `06 1f 1c 10` |
| 16 | `0x0030` | 4 | `00 08 20 00` |
| 17 | `0x0030` | 4 | `00 09 20 00` |
| 18 | `0x0030` | 1 | `a2` |

The sole MMIO write changes only xPON `XPON_SETTING` RX-SD polarity:

```text
0x0000014f -> 0x0000010f
```

It is attempted after the LOS prefix and before APD.  The driver proves that
no other bit changed.  There is no ESD/deglitch write, current setup, laser or
TGEN path, thermal APD worker, retry, rollback or arbitrary write helper.

## Exact oracle and finite observation

The reset is followed by a fixed 10–12 ms settle.  The calibration must then
reproduce this same-unit phase-21 oracle exactly:

```text
Vref:        0x020a
V:           0x0284 or 0x0285
V - Vref:    0x007a or 0x007b
```

LOS receives a fixed 20–25 ms settle.  Before APD, the strict post-LOS state
still requires `LOS_CTRL2=05 1f 22 00`, `LOS_TIMEOUT=3e 00 00 00` and a zero
timeout count.  The timeout and autonomous byte 2 of `LOS_CTRL2` are outcomes,
not gates, after APD; bytes 0, 1 and 3 remain guarded.

If all stages pass, the observer records sample zero and 20 more samples at
fixed 100 ms intervals.  OVP is the first EN7570 read in every sample.  Every
sample also records and gates APD, SAFE, Ibias, Imod, LOS controls, GPIO16
physical `TX_DISABLE`, xPON TXEN, rogue mode, PRBS, test-frame and interrupt
enable.  It observes LOS debug, timeout, xPON LOS, PHY FSM, RX sync and the OEM
FEC bit without using receiver outcomes as safety gates.  Any read or safety
error stops the finite series.

## Non-transactional safety boundary

Before the first write, the driver requires all of the following exact
identities and cold values:

- the XR500v experimental compatible and DT opt-in;
- xPON MMIO resource `0x1faf0000/0x1000`;
- PON I2C controller resource `0x1fbf8000/0x100`;
- EN7570 address `0x70` and silicon ID `0x03`;
- the exact 0x190-byte per-unit factory BOB cell and its SHA-256;
- the complete 29-register cold EN7570 map;
- `SAFE=ff 8f ff 0f`, zero OVP, Ibias and Imod;
- GPIO16 active-high, output and raw/logical high;
- GPON mode with TXEN, rogue mode, PRBS, test frames and IRQ enable off.

The adapter retry count is forced to zero while the bus segment is locked,
and only raw `__i2c_transfer()` is used.  The global one-shot is atomically
consumed and the module self-pins before the first transfer attempt.  Every
attempt is counted before touching the bus.  There is no retry and no recovery
write on failure; status preserves the failing/terminal snapshot.  Once any
write is attempted, physical power removal for at least 30 seconds is the only
accepted recovery boundary.

## Source and module audit

The package is manual, non-shipping and non-autoload:

```text
package:
package/kernel/xr500v-en7570-rx-apd-a2-observer

source SHA-256:
fe9def9e08e549fc8c27b2a20c47256ae99c62933fb4d7d2446cbca4cbf413ee

module SHA-256:
719abab06b4f4f452606ade119ec7f2e53f4b70b0f1f378a8a4fcf65ec0b0c36

module size: 415616 bytes
vermagic:    6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
dependency:  i2c-core
parameter:   arm_en7570_rx_apd_a2:bool
```

The canonical source, build-tree package source and kernel build-directory
source are byte-identical.  The final module was built after that source.
Strict checkpatch reports zero errors and zero warnings; the remaining 17
messages are non-fatal style checks, primarily continuation alignment.
`git diff --check` is clean.

Two independent final audits found no electrical-safety or sequencing
blocker.  They specifically rechecked the 18-byte sequence, sole polarity
write, strict pre-APD timeout state, autonomous post-APD masks, OVP-first
sampling, full TX guards, error exits, one-shot/pinning, FEC bit 15 and build
provenance.  The only accepted diagnostic nit is that `halted_step` can name a
pending stage imprecisely; it cannot change execution or permit another write.

## Experimental image audit

The image changes only the temporary xPON observer binding and immutable
opt-in.  The DTB contains:

```text
compatible = "econet,en751221-en7570-rx-apd-a2-experimental";
reg = <0x1faf0000 0x1000>;
econet,allow-en7570-rx-apd-a2;
tx-disable-gpios = <&gpio 16 0>;
airoha,en7570 = <&en7570>;
nvmem-cell-names = "factory-bob";

factory-bob@20000 {
        reg = <0x20000 0x190>;
};
```

Artifacts:

```text
patched image SHA-256:
8c04673900bdb3662b3033089bf0fdd44f528aaea99812d186823b03b8d2c24c

patched image size:       11095622 bytes
raw image SHA-256:         288ce1875fb97be9dd437b22051f394e878ce2db00bb0988bf4c0d3b09461b34
compressed kernel SHA-256: 642da2768f8ff9dec27f5f9001519c1b804693689b51a2c07650880141980607
compressed kernel size:    2948049 bytes
safe kernel headroom:       197167 bytes
temporary DTB SHA-256:     2087a714d3aa5a97c89ae976dd7ef3af10a3f7fcc31f981be5b260f52fc2b80d
temporary DTB size:         11713 bytes
```

The XR500v validator and supplemental byte checks pass:

```text
TrendChip magic:       4c 3d 2e 1f aa 55 aa 55
decompress/entry:      0x80020000 / 0x80020000
kernel partition:      0x00300000
header size:           0x00000200
rootfs size:           0x01000000
tail magic:            55 aa 01 01
SquashFS file offset:  0x300200
512-byte gap:          present and all zero
gap SHA-256:           076a27c79e5ace2a3d47f9dd2e83e4ff6ea8872b3c2218f66c92b89b55f36560
```

The DTB embedded in the kernel is byte-identical to the audited DTB.  The
observer is absent from SquashFS, both image manifests, device packages and
autoload directories.  It must be copied to `/tmp` only after a cold passive
preflight.

## Live protocol, not yet executed

The fibre stays disconnected through image installation and the mandatory
physical cold boot.  Before copying the module, the live DT and cold EN7570,
GPIO and xPON safety state must be checked again.  Only after that passes may
the authorised Movistar fibre be connected.

The future one-shot invocation is:

```text
insmod /tmp/phase25-rx-apd-a2.ko arm_en7570_rx_apd_a2=1
```

Status and the kernel tail must be captured immediately.  The router must then
be physically powered off for at least 30 seconds, regardless of success,
failure, SSH loss or ambiguous output.  There must be no `rmmod`, software
reboot, retry or rollback after the first write.  A cold-reset proof and final
restoration to the known phase-14 passive image are required before this phase
can be closed.
