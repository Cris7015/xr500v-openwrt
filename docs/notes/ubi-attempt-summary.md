# UBI overlay attempt — XR500v Phase 2 task 1

Date: 2026-05-07 (madrugada)

## What we tried

1. **iter11**: switched DTS partition `rootfs1` label → `ubi`, recipe `append-rootfs` → `append-ubi`, bootargs `root=ubi0:rootfs rootfstype=squashfs`.
   - Result: kernel auto-attach pre-check `mtd_read(mtd6, 0, 4)` failed to find UBI# magic even though it's on flash.

2. **iter12**: added `ubi.mtd=ubi` to bootargs to bypass the auto-attach pre-check.
   - Result: UBI attached successfully BUT `empty MTD device detected` and `27 bad PEBs / 101 good`. No user volumes recognized.

## Root cause

At the time of this May 2026 experiment, `mtd writeflash` from stock OEM was
the only reliable flash path available. It wrote data with the OEM's ECC/OOB
layout; when the then-current mainline kernel re-read via `spi-nand + en75_bmt`,
the OOB content was misinterpreted as bad-block markers on 27 of 128 PEBs and
EC-header CRC validation likely failed on the rest. UBI saw the device as
essentially empty. This is historical: current builds have a board-specific,
BMT-aware sysupgrade path for `kernel1`/`rootfs1` and keep `openwrt_ubi`
separate.

## Why stock-flash worked for raw squashfs but breaks UBI

- Raw squashfs: only data is read by the kernel filesystem driver. OOB irrelevant.
- UBI: every PEB has a 64-byte EC header + VID header in DATA area, but UBI also reads OOB to detect bad blocks. If OOB has stale bad-block markers (from OEM layout), UBI rejects those PEBs even though data is fine.

## Historical en75_bmt status

At the time, the kernel printed
`en75_bmt: BBT not found and econet,can-write-factory-bbt is unset, giving up`.
That statement describes the iter11/iter12 image, not current firmware.

## Historical path forward (superseded)

Provisioning UBI **from inside running OpenWrt** instead of from stock OEM bootloader. Sequence:

1. Boot a known-working OpenWrt image (like iter10 — kernel + raw squashfs in slot B) — already have it.
2. From the running OpenWrt, run:
   ```
   ubiformat /dev/mtd_for_data_partition -y
   ubiattach -m N
   ubimkvol /dev/ubi0 -N rootfs -s SIZE -t static --image=squashfs.bin
   ubimkvol /dev/ubi0 -N rootfs_data -m
   ```
3. Reboot — kernel reads the freshly-formatted UBI.
4. The format-from-OpenWrt path uses mainline drivers end-to-end → consistent OOB → no false bad PEBs.

This proposed staging design was not adopted. It would have turned `rootfs1`
itself into UBI and is retained here only as experiment history.

Alternative: a SECOND-stage flash. iter10 stays on disk; first boot script detects unformatted "ubi" partition and ubiformats + populates from a tarball or recovery image.

## Resolution / current design

- Slot B remains `kernel1` plus raw SquashFS `rootfs1`.
- A dedicated 64 MiB `openwrt_ubi` partition occupies the separately verified
  free region and contains the `rootfs_data` UBIFS overlay.
- DTS enables BMT reconstruction with `econet,can-write-factory-bbt` and an
  explicit empty `econet,factory-badblocks` list for this physical unit.
- Preinit attaches and, when necessary, self-provisions `openwrt_ubi`; it does
  not depend on a stock-written UBI image.
- The board-specific sysupgrade path updates only `kernel1`/`rootfs1` and
  preserves `openwrt_ubi` unless `-n` is explicitly selected.
- A cold phase-27 boot on 2026-07-14 reported 512 good PEBs, zero bad/corrupted
  PEBs, maximum erase counter 2 and a healthy mounted `rootfs_data` volume.
