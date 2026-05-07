# UBI overlay attempt — XR500v Phase 2 task 1

Date: 2026-05-07 (madrugada)

## What we tried

1. **iter11**: switched DTS partition `rootfs1` label → `ubi`, recipe `append-rootfs` → `append-ubi`, bootargs `root=ubi0:rootfs rootfstype=squashfs`.
   - Result: kernel auto-attach pre-check `mtd_read(mtd6, 0, 4)` failed to find UBI# magic even though it's on flash.

2. **iter12**: added `ubi.mtd=ubi` to bootargs to bypass the auto-attach pre-check.
   - Result: UBI attached successfully BUT `empty MTD device detected` and `27 bad PEBs / 101 good`. No user volumes recognized.

## Root cause

`mtd writeflash` from stock OEM (the only reliable flash path we have) writes data with the OEM's ECC/OOB layout. When the mainline kernel re-reads via `spi-nand + en75_bmt`, the OOB content is misinterpreted as bad-block markers on 27 of 128 PEBs and EC-header CRC validation likely fails on the rest. UBI sees the device as essentially empty.

## Why stock-flash worked for raw squashfs but breaks UBI

- Raw squashfs: only data is read by the kernel filesystem driver. OOB irrelevant.
- UBI: every PEB has a 64-byte EC header + VID header in DATA area, but UBI also reads OOB to detect bad blocks. If OOB has stale bad-block markers (from OEM layout), UBI rejects those PEBs even though data is fine.

## en75_bmt status

`en75_bmt: BBT not found and econet,can-write-factory-bbt is unset, giving up` — the driver does NOT manage bad-block remapping for us. Without BBT, every block is taken at face value; OEM-style OOB markers leak through.

## Path forward (next session)

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

This requires staging: iter10 (raw squashfs root, has nand-utils) → ubiformat — boot to UBI image. So slot B layout becomes: kernel (mtd6) + UBI partition (mtd7) where iter10's UBI provisioning script populates it.

Alternative: a SECOND-stage flash. iter10 stays on disk; first boot script detects unformatted "ubi" partition and ubiformats + populates from a tarball or recovery image.

## Working state (current)

- iter10 flashed to slot B: kernel + raw XZ squashfs, tmpfs overlay (no persistence)
- bflag set to 0 currently (in stock); user must do bflag set 1 + reboot to enter OpenWrt
- All knowledge captured in memory + repo
