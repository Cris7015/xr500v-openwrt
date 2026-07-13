# GPON bring-up — phase 26 guarded RX/APD A2 one-LSB observer (compile only)

Date: 2026-07-13

## Status

The phase-26 one-shot observer is implemented, independently audited, built
and packaged into a dedicated experimental XR500v image. It has **not** been
loaded on hardware yet.

Phase 25 stopped safely after its fifth successful I2C write because the live
RSSI Vref was `0x020b`, one LSB above its exact `0x020a` oracle. No gain, LOS,
xPON MMIO, APD or sampling operation ran. Phase 26 changes only that ADC
oracle and gives the artifact a separate package, driver, compatible, DT
opt-in and module parameter. The electrical sequence, safety barriers,
timings and finite observation remain identical to phase 25.

This is still only a guarded receiver experiment. It does not implement
PLOAM, O5, OMCI or a network data path and must not be described as GPON
service.

## Exact one-LSB oracle

After the reset prefix and fixed 10–12 ms settle, the observer accepts exactly
these four same-unit ADC pairs:

| RSSI Vref | RSSI V | V - Vref |
|---:|---:|---:|
| `0x020a` | `0x0284` | `0x007a` |
| `0x020a` | `0x0285` | `0x007b` |
| `0x020b` | `0x0285` | `0x007a` |
| `0x020b` | `0x0286` | `0x007b` |

The predicate separately bounds Vref, V and their delta and rejects `V <=
Vref` before subtraction. Exhaustive enumeration confirmed that no fifth
pair can pass. This is a one-LSB allowance around the measurements already
observed on this physical unit, not a broad range or a retry policy. Phase 25
observed only Vref `0x020b` before aborting; the two V values paired with that
Vref are prospective one-LSB counterparts and `0x0286` has not been observed
live.

## Immutable write budget

The sole read-only module parameter is
`arm_en7570_rx_apd_a2_lsb=1`. The matching device-tree node has the distinct
experimental compatible
`econet,en751221-en7570-rx-apd-a2-lsb-experimental` and immutable opt-in
`econet,allow-en7570-rx-apd-a2-lsb`.

The complete I2C whitelist is still exactly 18 writes, in this order:

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

The sole MMIO write still changes only xPON `XPON_SETTING` RX-SD polarity:

```text
0x0000014f -> 0x0000010f
```

It is attempted after the LOS prefix and before APD, and the driver proves
that no other bit changed. There is no ESD/deglitch write, current setup,
laser or TGEN path, thermal APD worker, retry, rollback or arbitrary write
helper.

## Finite observation and fail-closed boundary

LOS receives the same fixed 20–25 ms settle. Before APD, the guarded state
still requires `LOS_CTRL2=05 1f 22 00`, `LOS_TIMEOUT=3e 00 00 00` and a zero
timeout count. The timeout and autonomous byte 2 of `LOS_CTRL2` are outcomes,
not gates, after APD; bytes 0, 1 and 3 remain guarded.

If every stage passes, sample zero and 20 more samples are recorded at fixed
100 ms intervals. OVP is the first EN7570 read in every sample. Every sample
also records and gates APD, SAFE, Ibias, Imod, LOS controls, GPIO16 physical
`TX_DISABLE`, xPON TXEN, rogue mode, PRBS, test-frame and interrupt enable.
LOS debug, timeout, xPON LOS, PHY FSM, RX sync and the OEM FEC bit are observed
without being used as transmitter-safety gates. Any read or safety error ends
the finite series.

Before the first write, the driver requires all of the following exact
identities and cold values:

- the phase-26 compatible and DT opt-in;
- xPON MMIO resource `0x1faf0000/0x1000`;
- PON I2C controller resource `0x1fbf8000/0x100`;
- EN7570 address `0x70` and silicon ID `0x03`;
- the exact 0x190-byte per-unit factory BOB cell and its SHA-256;
- the complete 29-register cold EN7570 map;
- `SAFE=ff 8f ff 0f`, zero OVP, Ibias and Imod;
- GPIO16 active-high, output and raw/logical high;
- GPON mode with TXEN, rogue mode, PRBS, test frames and IRQ enable off.

The adapter retry count is forced to zero while the bus segment is locked,
and only raw `__i2c_transfer()` is used. The global one-shot is atomically
consumed and the module self-pins before the first transfer attempt. Every
attempt is counted before touching the bus. There is no retry and no recovery
write on failure. Once any write is attempted, physical power removal for at
least 30 seconds is the only accepted recovery boundary.

## Source and module audit

The package is manual, non-shipping and non-autoload:

```text
package:
package/kernel/xr500v-en7570-rx-apd-a2-lsb-observer

package Makefile SHA-256:
5b6f069a5666470283629b093ea784f6dc561d5d8b8e93d923a180bf25963723

module Makefile SHA-256:
c21674c70021a7bb1da2a55aee119e721392dc42a2e80644b15edf87340f4f9b

source SHA-256:
a19236d82692b761a815e8dd77891aa5f5349622bdb7ad75ae5d54a01c7089fe

module SHA-256:
4344e04d99b00e42a403cd2075554eeb5fa8c635a621c7560783cf0093f2cc12

module size: 415880 bytes
ELF:         32-bit MSB MIPS32r2 relocatable
vermagic:    6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
dependency:  i2c-core
parameter:   arm_en7570_rx_apd_a2_lsb:bool
```

The canonical source, OpenWrt package source and kernel build-directory source
are byte-identical. The final module and image were built after that source.
Strict checkpatch reports zero errors and zero warnings; the remaining 17
messages are non-fatal style checks. `git diff --check` is clean.

Two independent implementation audits found no electrical-safety or
sequencing blocker. Literal and normalized comparisons with phase 25 confirm
that only the artifact identity, six oracle constants and two oracle
predicates changed. The 18 writes, one MMIO write, all 16 TX barriers, all 29
cold-state rows, factory hash, timings, one-shot/pinning, zero retries, zero
rollback and 21 samples remain identical.

Two inherited diagnostic limitations are accepted and documented: a
`halted_step` value names the latest write count rather than always the pending
stage, and unexecuted oracle fields have no separate validity bits. Neither
can permit a write, weaken a gate or enable a retry.

`CONFIG_MODULE_STRIPPED=y` means `modinfo` does not retain an OF alias; phase
25 behaves the same way. The source still contains `MODULE_DEVICE_TABLE`, the
exact OF match table and compatible, and the KO retains the compatible
literal. Explicit `driver_override` binding is the planned path.

## Experimental image audit

The temporary DTB changes only the observer binding and immutable opt-in:

```text
compatible = "econet,en751221-en7570-rx-apd-a2-lsb-experimental";
reg = <0x1faf0000 0x1000>;
econet,allow-en7570-rx-apd-a2-lsb;
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
d45b0f6a07f566becd5b60a4f9e4cc4eed30556f509945bb4efb37196e9db19f

patched image size:       11095630 bytes
raw image SHA-256:         90df09d71e32d8f15edcfc736adf9311b23f824aa4db110039fa4c0a9cf4a09a
compressed kernel SHA-256: 675e3f405458ca5bf133a72dab94a02cabfb9cd285e0b0d0ec21bcc5c56d86cf
compressed kernel size:    2948057 bytes
safe kernel headroom:       197159 bytes
temporary DTB SHA-256:     f2daf357bb48a625356d92dc086650d0c7c3ea7f3d2b19654159031ee95b84c5
temporary DTB size:         11721 bytes
SquashFS payload SHA-256:   e558a09e1cdca03d12d163d68b6d9e0a85e0cfdfe2a3dfb0e906086dfdc343e0
```

The XR500v validator and supplemental byte checks pass:

```text
TrendChip magic:       4c 3d 2e 1f aa 55 aa 55
decompress/entry:      0x80020000 / 0x80020000
kernel size hint:      0x01300000
kernel partition:      0x00300000
header size:           0x00000200
rootfs size:           0x01000000
tail magic:            55 aa 01 01
SquashFS file offset:  0x300200
512-byte gap:          present and all zero
gap SHA-256:           076a27c79e5ace2a3d47f9dd2e83e4ff6ea8872b3c2218f66c92b89b55f36560
```

The raw and patched images are byte-identical after the 512-byte header. The
patch changed only 19 bytes in the intended TrendChip fields, with the last
changed byte at offset `0x8f`.

The DTB embedded at the end of the audited kernel artifact is byte-identical
to the standalone DTB. The observer is absent from SquashFS, both image
manifests, `profiles.json`, target `DEVICE_PACKAGES`, the built rootfs and
autoload directories. Its archived KO is byte-identical to the audited KO and
must be copied to `/tmp` only after a cold passive preflight.

The first parallel `make world` exposed only a host-environment issue: the
inherited WSL/Windows `PATH` contained a relative `Files/WindowsApps/...`
entry that GNU `find -execdir` correctly rejected. Rebuilding with a clean,
absolute Linux `PATH` completed successfully and did not require a source
change.

## Planned one-shot protocol

The fibre must remain disconnected through image installation and the
mandatory physical cold boot. The live DT and cold EN7570, GPIO and xPON
safety state must pass again before the authorised Movistar fibre is
connected. The archived KO must be copied to `/tmp`, its remote SHA-256 must
match the audited value, and the phase-26 device must be bound explicitly
through `driver_override`; neither image nor module may autoload it.

The only permitted invocation is:

```text
insmod /tmp/phase26-rx-apd-a2-lsb.ko arm_en7570_rx_apd_a2_lsb=1
```

Immediate bounded status and kernel-tail capture must be followed by physical
power-off for at least 30 seconds regardless of success, failure, SSH loss or
ambiguous output. `rmmod`, software reboot, retry and rollback are prohibited
after the first write. The insertion is permitted exactly once. A cold-reset
proof and final restoration to the known phase-14 passive image are required
before this phase can be closed.
