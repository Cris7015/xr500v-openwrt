# GPON bring-up — phase 27 live preparation without fibre

Date: 2026-07-14

Status: **historical preparation record; the one-shot was executed once on
2026-07-15 and must not be repeated; see
`2026-07-15-gpon-phase27-live-run-partial-evidence.md`**

## Scope and safety boundary

The router had been physically off for several hours and booted the passive
phase-14 diagnostic image with the fibre disconnected.  That genuinely cold
state was used for a final read-only baseline.  No experimental module was
present or loaded and both passive diagnostics reported zero writes.

After the staging session, the user reported another physical power-off.  The
verified `/tmp/phase27-los-trace.ko` copy described below was therefore lost
with tmpfs and was no longer assumed to be present on the router.  At this
preparation checkpoint, that meant the host artifact would first need another
cold passive preflight and remote hash check.  That later happened before the
single invocation recorded in the 2026-07-15 run note; it is not permission
for another invocation.

This preparation did **not** execute the phase-27 sequence.  The phase-27
module is deliberately absent from the image and has no autoload entry.  The
then-future invocation was a separate, explicit, one-shot action requiring
live fibre, an operator next to the power cord and a mandatory physical power
cut afterwards.  It has now been consumed.

## Final artifact re-audit

The complete phase-27 artifact audit passed again before installation:

```text
patched image:
  size    11095666 bytes
  sha256  3ca86942dfb6299e45a21efdd850f360c45b343b87e357f8c2eec71209ddef7c

unstripped standalone module:
  size    417092 bytes
  sha256  31ab9ee5892baabe7e80ee1105da538dbaa5b3a4da199edf09079e5c90d43e11
```

Rechecked properties included:

- decompressed kernel exactly matches the build artifact;
- kernel size 2,948,049 bytes, leaving 197,167 bytes of the permitted budget;
- final DTB is byte-identical to the standalone DTB;
- TrendChip magic, entry point and rootfs pointer are valid;
- the complete `0x300000..0x3001ff` 512-byte gap is zero;
- SquashFS starts at `0x300200`;
- raw-to-patched differences are restricted to 19 header bytes before
  `0x90`, with everything from `0x200` onward byte-identical;
- the observer is absent from SquashFS, manifests and device packages;
- the package remains `=m` with no `AUTOLOAD`;
- the writer requires exact EN7570 identity/variant `0x03/0x01` before the
  cold map and rechecks the cached identity in every per-write fast gate;
- `scripts/audit_phase27_los_trace.py` passes.

The generated build-output `sha256sums` file still contained hashes from an
earlier phase-27 build.  Its two phase-27 rows were corrected to the final
image/module hashes above, after which `sha256sum -c --ignore-missing` passed.
The binaries themselves were already correct.

## Cold passive baseline before installation

The phase-14 boot reported the expected untouched EN7570 state:

```text
silicon ID / variant:  03 / 01
APD_DAC:               00 08 00 00
APD_OVP_LATCH:         00 00 00 00
SAFE_PROTECT:          ff 8f ff 0f
LOS_CTRL1:             06 08 3c 36
LOS_CTRL2:             10 05 00 00
LOS timer:             ff ff ff ff
LOS timeout/count:     both zero
Ibias / Imod:          both zero
software reset:        zero
register-data writes:  0
```

The xPON diagnostic simultaneously showed GPON mode, retained setting
`0x14f`, TXEN/rogue/PRBS/test/interrupt-enable all clear and physical
TX-disable GPIO 16 asserted high.  UBI reported zero bad PEBs, maximum erase
counter 2 and `rootfs_data` state `OK`.

## Installation from running OpenWrt

The image was copied to `/tmp/phase27-los-trace-patched.bin`.  Its remote size
and SHA-256 matched the local artifact, and the board-specific image check
accepted it:

```text
sysupgrade -T /tmp/phase27-los-trace-patched.bin
```

The actual upgrade used normal `sysupgrade`, **without `-n`**:

```text
sysupgrade /tmp/phase27-los-trace-patched.bin
```

This is the intended XR500v path.  `platform_check_image()` requires SquashFS
at `0x300200` and the TrendChip magic; `platform_do_upgrade()` pivots to RAM
and writes only the `kernel1` and `rootfs1` slices through the BMT-aware NAND
driver.  Normal sysupgrade preserves the separate `openwrt_ubi` overlay.
Using `-n` would explicitly erase that overlay and was therefore not used.

The SSH command ended when sysupgrade closed all sessions, as expected.  The
router returned at `192.168.68.222` after the warm reboot.

## Warm-boot verification

The new DT is active:

```text
compatible = econet,en751221-en7570-los-trace-experimental
econet,allow-en7570-los-trace = present
platform device 1faf0000.xpon-phy = unbound
driver_override = (null)
```

Every disarmed-state check passed:

```text
find /lib/modules -name '*los-trace*'       -> no files
lsmod | grep los-trace                      -> no module
phase-27 debugfs status                     -> absent
xr500v-en7570 passive register-data writes  -> 0
```

The warm passive EN7570 map remained exactly at the cold baseline values,
including APD `00 08 00 00`, OVP zero, SAFE `ff 8f ff 0f`, LOS_CTRL2
`10 05 00 00`, Ibias/Imod zero and software reset zero.  The overlay remained
mounted from `/dev/ubi0_0`; UBI still reported zero bad PEBs, erase counter 2
and volume state `OK`.

This warm reboot alone did not satisfy the experiment's cold-state
requirement.  It was subsequently followed by a physical power removal of at
least 35 seconds with fibre still disconnected and the complete passive
preflight below.

## Physical cold-boot and disconnected-fibre preflight

The operator removed power for at least 35 seconds and booted again without
fibre.  The new boot ID was
`9ae2d387-d1b4-436f-b832-849851bd0099`; the first complete baseline below was
read at 424 seconds uptime.  The experimental DT compatible and immutable
opt-in were active, but the platform device was unbound, the phase-27 status
was absent and no LOS-trace module existed in SquashFS or `lsmod`.

The per-unit 400-byte factory cell was read directly from `misc+0x20000` and
matched the observer's frozen identity:

```text
401dfdaee77c84649bda100fd5dd85be01c7ea126d0a5cc2116b141c1a07a5e4
```

The passive EN7570 map returned ID/variant `03/01`, the complete clean values
listed above, and `register_data_writes: 0`.  UBI recovered the journal after
the physical cut and then reported zero bad/corrupted PEBs, maximum erase
counter 2 and the `rootfs_data` volume mounted normally.

The existing read-only `xr500v-gpon-diag` driver was then selected through
`driver_override` and bound to `1faf0000.xpon-phy`.  Its cold report showed:

```text
mode / FSM / sync:       GPON / 3 / no
XPON_SETTING:            0x0000014f
TXEN / rogue / PRBS:     off / off / 0
test frame / IRQ enable: 0 / 0
TX_DISABLE GPIO16:       output / raw high / asserted
MMIO writes:             0
counter clear/latch:     no
reset or mode change:    no
```

Finally, the standalone module was copied with legacy SCP (the target has no
SFTP server) to `/tmp/phase27-los-trace.ko` and verified remotely:

```text
size    417092 bytes
sha256  31ab9ee5892baabe7e80ee1105da538dbaa5b3a4da199edf09079e5c90d43e11
```

The first staged build was subsequently superseded after the EN757x source
cross-check proved that ID `0x03` is shared by EN7570 and EN7571.  The final
module above also reads `0x015c` and requires this unit's exact variant
`0x01`; it was rebuilt, re-audited and copied over the old tmpfs file without
loading either build.  The final remote size is **417092 bytes**.

It was **not** loaded.  The passive GPON diagnostic remains bound and the
phase-27 debugfs status remains absent.

## Read-only evidence capturer

`scripts/phase27_capture_status.py` was added for the then-future one-shot.  It has
two modes:

```text
phase27_capture_status.py validate STATUS.raw
phase27_capture_status.py capture --host root@192.168.68.222 --output-dir DIR
```

`capture` only reads the phase-27 debugfs status, metadata, `dmesg` and
`logread` over SSH.  It contains no module load, driver bind/override, reboot,
poweroff, MMIO, GPIO or I2C action.  Raw output is written before validation,
then accompanied by `validation.json`, `manifest.json` and `SHA256SUMS`.
The output path may be new or an existing empty directory; files and non-empty
directories are rejected without overwriting them.  An exclusive durable
`.capture.claim` file atomically assigns every accepted directory to one
capturer before SSH, preventing concurrent `os.replace()` calls.
The status is atomically written and `fsync`ed first; the tool then prints the
physical-cut instruction before attempting the supplementary log reads.  A
timeout, missing status, SSH execution failure or malformed report remains
fail-closed: the manifest still orders a physical cut and never suggests a
retry once the output directory is accepted.  Every local path/write/fsync
failure still prints the physical-cut instruction before propagating the
error.  A rejected occupied path deliberately receives no manifest because
prior evidence must not be modified.

The validator independently checks the frozen 15-write table, contiguous
attempt prefix, MIPS `-ECANCELED` value (`-158`), timestamps, all three exact
29-register snapshot layouts and fixed values, the complete 12-sample line
schema, dense/critical/full guard sets, the driver state machine, APD/MMIO
zero-write assertions, GPIO/xPON gates and their derived-field consistency,
recomputed byte-2 counts/transitions, and the mandatory physical-power-cut
flag.  It parses the kernel's real
space-separated `%ph` byte format.  `-ERANGE` (`-34`) is treated as a new
scientific outcome rather than an electrical failure.

A structurally coherent aborted acquisition also exits successfully as an
evidence capture; its `classification` remains `aborted-needs-powercut`.
This avoids making a valid one-shot abort look retryable.  Only malformed or
unsafe evidence returns a failing validator exit status.

Twenty-three offline contract tests pass:

```text
capture script sha256  d8b0134d4225fdea9a57a1cad11d4620c99f341d1ed903ce52f13a255ebeed33
test suite sha256      4bfb270a56ca9f85eb884abaaa77fe05b1b65ae5f6a1fa177f3cf5166d82e02e
```

```text
python3 -m unittest -v tests/test_phase27_capture_status.py

complete known transition                 PASS
terminal unknown scientific outcome       PASS
valid partial-prefix abort                 PASS
trace abort before first accepted sample   PASS
tampered fixed write rejected              PASS
unsafe sample GPIO rejected                PASS
missing physical-power-cut boundary        PASS
snapshot layout / terminal ADC tamper       PASS
truncated or malformed sample body          PASS
impossible success/result state rejected    PASS
zero-write fast-gate abort needs powercut   PASS
failed-write result causality               PASS
rejected-sample block remains complete      PASS
post-reset capture/verify causality         PASS
SSH/local failure still orders physical cut PASS
reported derived-field tamper rejected      PASS
decimal GPIO errno preserved                PASS
partial-prefix RSSI causality                PASS
synchronous sample chronology                PASS
real ECANCELED snapshot result                PASS
nonzero rejected RSSI read forces ERANGE      PASS
exact EN7570 variant 0x01 required             PASS
atomic capture-directory single owner          PASS
```

An independent post-run review found a concurrent empty-directory claim race
and stale instructions that could imply phase 27 remained runnable.  The
exclusive durable claim, concurrency regression and consumed-one-shot wording
now close both issues.  Remote capture commands remain read-only and every
ambiguous failure retains the physical-cut boundary.

The canonical flashing documentation was also reconciled with the now-tested
board-specific BMT-aware sysupgrade implementation.  Routine upgrades use
`sysupgrade -T` followed by `sysupgrade` on the TrendChip-patched image; raw
manual `mtd write` remains forbidden, while stock telnet/web and UART/TFTP are
the recovery and first-install paths.

The host validator now decompresses the LZMA stream, compares the optional
`kernel.bin`, enforces the 3 MiB diet, proves the 512-byte gap is all zero,
checks SquashFS `bytes_used` and the 16 MiB bound, and validates the complete
TrendChip header including the fixed `0x80020000` wrapper entry.  The target's
canonical `platform_check_image()` source now independently checks the fixed
layout/header/gap for the **next image build**.  The already-installed phase-27
image still contains the earlier target-side magic/SquashFS gate; its complete
header/gap/SquashFS contract was instead re-proved offline by the strengthened
host validator.  Five destructive-contract regression tests pass in
`tests/test_validate_xr500v_image.py`; a wrong entry, nonzero gap, truncated
SquashFS or wrong TrendChip rootfs size is rejected.

## Stop point

This preparation stop point has been consumed.  Phase 27 ran exactly once on
2026-07-15, followed by the mandatory physical cut and a successful cold
recovery.  Do not restage or invoke this observer again.  The terminal kernel
summary was retained but the detailed debugfs report was lost to a local
capture-directory error; the exact evidence boundary and capture-tool fix are
recorded in
[`2026-07-15-gpon-phase27-live-run-partial-evidence.md`](2026-07-15-gpon-phase27-live-run-partial-evidence.md).
