# GPON bring-up — phase 36 single downstream PLOAM pop

Date: 2026-07-16

Status: **compiled and statically audited; the live experiment has not run**

## Purpose

Phase 35 proved that the reset/default EN751221 GPON MAC can raise
`PLOAMD_RECV` in O2 and expose a `0x00090009` downstream FIFO status: nine
32-bit words, or three complete fixed-size downstream PLOAM records.  It
deliberately did not read the destructive FIFO data register.

Phase 36 crosses exactly that one remaining RX-only boundary.  On the same
strict trigger it restores O1 while retaining the GPON mux, reads exactly one
three-word record, and requires the FIFO status to change from nine current
words to six while retaining the recorded maximum and reporting no overrun.
It then restores the exact ATM selector.

Matheus' independent EN7523/EN7571 traces now show that the first activation
traffic should include Upstream-Overhead and Extended-Burst-Length messages.
That makes the private record type immediately useful for cross-checking, but
the phase-36 module still invokes no parser and makes no assumption about the
payload.

## Safety boundary

The module inherits every live-proven phase-35 gate and adds stricter checks
around the destructive read:

- the phase-28 EN7570 RX handoff driver must own the experimental PHY node;
- GPIO16 physical active-high `TX_DISABLE` must be output-high;
- `PHYSET3.TXEN`, rogue TX, PRBS, test-frame TX and both xPON/GPON interrupt
  enables must remain clear;
- PHY_READY, GPON sync, FEC and LOS-clear must remain true;
- ONU ID, global configuration, upstream FIFO and TX-burst count must remain
  at the exact phase-35 defaults;
- the only accepted trigger is `INT_STATUS=PLOAMD_RECV` together with
  `PLOAMD_FIFO_STS=0x00090009` while activation is O2;
- exact O1 is written and read back before any FIFO data access;
- a bounded IRQ-off section contains the O1 readback, three and only three
  `PLOAMD_RDATA` reads, and immediate FIFO/guard checks;
- the FIFO must become exactly `0x00090006`;
- the WAN mux is verified both immediately before and immediately after the
  pop, then restored to the exact original ATM value;
- any unknown interrupt, TX indication, counter change, mux change, slow
  boundary or failed postcheck rejects the record as unverified.

The module still performs only these writes:

1. ATM-to-GPON and exact GPON-to-ATM WAN-mode updates;
2. O1-to-O2 and exact O2-to-O1 activation writes;
3. the two already live-proven selective W1C operations from phase 35.

It has no identity, upstream FIFO, response-time, preamble, PHY, GPIO,
EN7570, APD, bias, modulation-current, laser, reset or QDMA write path.

This remains safe to run with the fibre connected because both independent
transmit kills remain asserted.  It is not an O3/TX experiment.

## Private capture design

The normal `status` endpoint reports only booleans, timings, checks and FIFO
metadata.  It never prints the three raw words.  Kernel logs also contain no
raw PLOAM data.

A separate root-only debugfs file named `private_record` exposes the three
hardware-order `u32` reads only after the worker is terminal.  It explicitly
labels them as MIPS big-endian values without host byte reinterpretation.  If
the pop happened but a later check failed, the file preserves the words with
`record_valid=0` so a required power-cycle cannot erase the only copy.

The private host runner is:

```text
/home/cristuu/.local/share/xr500v-lab/private/run-phase36-o2-single-pop.sh
```

It never sends `private_record` through `tee` or stdout.  A second SSH read is
redirected directly into a mode-0600 file under the private capture directory,
then copied to the separate private backup directory.  Only paths and matching
SHA-256 values reach the ordinary capture log.  Retrieval is attempted before
handling an unsafe-pinned result or requesting a power-cycle.

## Build and audit result

OpenWrt package build against the current MIPS big-endian 6.12.80 tree
completed without compiler or modpost diagnostics.

```text
source SHA-256:
4c4e8863afe34fa945d14ba470e7864078eb7d0e21f351cd620df784b48a0b6e

frozen kernel module SHA-256:
d97cb8950d20139a0f4996d899656a636bc39d002038ca081edf87677d4be6cb

APK SHA-256:
088130e727274fb953990aeeed841e280b2384a979039027b860b3021c0d5fe1

private runner SHA-256:
613e6f7dacf9fe7fe58a159bbd88972376bbcf5d52af7a485565d23a515d0816

module file size:
350,700 bytes (unstripped debug artifact)

module text/data/bss:
23,147 / 360 / 1,792 bytes
```

`checkpatch --strict` reports zero errors, warnings or checks.  `bash -n` and
ShellCheck report no runner issue.  Source audit finds three static
`iowrite32` call sites only: exact activation restoration, the masked
selective-W1C helper (called at most twice) and O2 entry.  The three FIFO
accesses are `ioread32`, not writes.  The only SCU write primitive is the
WAN-mode `regmap_update_bits` used for selection and exact restoration.

The module is transferred temporarily and is not part of the compressed
kernel or a sysupgrade image, so this test does not consume the XR500v's
roughly 3 MiB kernel payload budget or alter its required 512-byte image gap.

## Frozen artifacts

```text
/home/cristuu/openwrt/bin/targets/econet/en751221/xr500v-en7570-rx-handoff-observer-phase28.ko
/home/cristuu/openwrt/bin/targets/econet/en751221/xr500v-gpon-mac-o2-single-pop-observer-phase36.ko
/home/cristuu/openwrt/bin/targets/econet/en751221/packages/kmod-xr500v-gpon-mac-o2-single-pop-observer-6.12.80-r1.apk
```

## Live-test precondition

The next run requires a real cold boot and the existing phase-28-compatible
OpenWrt image.  Fibre may remain connected.  The private runner refuses to
start without `CONFIRMED_COLD_BOOT=yes`, rechecks the offline watchdog twice,
verifies both module hashes locally and remotely, and aborts on any existing
experimental module.

No live command was issued while preparing this phase.
