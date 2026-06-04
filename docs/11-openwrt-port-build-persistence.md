## Summary

This page is the developer / contributor guide for the OpenWrt port of the **TP-Link Archer XR500v** (SoC: EcoNet/Airoha **EN751221**, MIPS 24Kc/34Kc big-endian). A full OpenWrt 6.12 image boots from the device's second flash slot with working LAN (nested DSA), dual-band WiFi, USB and a reconstructed FXS/VoIP stack. The port is **not** a fork of OpenWrt — it is a small *overlay* (a handful of `package/` and `target/` directories plus a `config.seed`) layered on top of Caleb James DeLisle's `cjdelisle/openwrt` tree, which is itself the upstream of the now-mainline `econet` target. This document covers the upstream status, the overlay structure and pinned bases, the build environment, the iterate→build→flash loop, the UBIFS overlay persistence machinery (with its first-boot provisioning step), and the build traps that cost the most time — chiefly the duplicate-package stale-driver trap. For the hardware-level details of each subsystem, see the per-subsystem pages cross-referenced below.

## Upstream econet target status

The **EcoNet EN75xx** platform (which includes the EN751221) was merged into `openwrt/openwrt.git` mainline on **2025-09-11**, commit `73d0f9246042a487faf930a0571bd8c080bbc78f`, authored by Caleb James DeLisle (`cjdelisle`), who maintained the prior community fork. Key facts:

| Item | Value |
|---|---|
| OpenWrt target | `econet` / subtarget `en751221` |
| Kernel | 6.12 |
| SoC DTSI (upstream) | `target/linux/econet/dts/en751221.dtsi` |
| Kernel config | `target/linux/econet/en751221/config-6.12` |
| Linux-MIPS series | patchwork series 960479 (most accepted upstream) |
| Reference devices already supported | Archer VR1200v v2, Nokia G-240G-E, SmartFiber XP8421-B, Zyxel PMG5617GA |

The XR500v itself is **not** in the upstream device list — adding it requires a device-specific DTS (the port adapts the VR1200v v2 / EN751221 reference and then heavily extends it for the XR500v's dual-switch, dual-PCIe and FXS hardware).

**BMT caveat (critical for any custom image):** the EN75xx bootloader inherits TrendChip's custom Bad Block Table system, implemented upstream in `target/linux/econet/files/drivers/mtd/nand/en75_bmt.c` (~1439 lines). If an image is flashed without respecting the BMT/OOB layout, the bootloader can decide the flash is corrupt and rebuild it — a path to a brick. The official target handles this correctly; rolling a raw image without it is dangerous. See **brick taxonomy** and the persistence section below.

## The project is an overlay, not a fork

The repository `Cris7015/xr500v-openwrt` (branch `main`) is an **overlay** applied on top of a *pinned* base. It deliberately contains only the files that differ from upstream, which keeps it easy to rebase onto a newer econet tree.

| Layer | Identity |
|---|---|
| Pinned base tree | `cjdelisle/openwrt` @ `f3605b31fb` (branch `plan-b-nazox1`, tag `iter84-snapshot`) |
| `econet-eth` driver (overlay) | `github.com/cjdelisle/econet_eth` @ `c2f855cf5bdcb2dfc536d865d21bc793a346ce44` (date 2026-02-13) |
| `econet-eth` driver (base default) | `1db74f83` — **older, the origin of the "zombie driver"** (see trap below) |

Overlay contents (everything else is inherited from the base tree):

```
package/kernel/econet-eth/         # eth + nested-DSA driver, Makefile pins c2f855cf,
                                   #   patches 210/220/240/330, files/ (switch init)
package/kernel/econet-pcm/         # FXS/VoIP kernel modules: pcm-en751221 + econet-slic (+ callmgr source)
package/kernel/gpio-tc3162/        # panel LED / GPIO controller (@0x1fbf0200)
package/kernel/mt76/patches/       # 004-mt7603-xr500v-chip-gpio-led.patch
package/kernel/linux/modules/netdevices.mk   # adds TARGET_econet deps + defines kmod-dsa-mt7530
target/linux/econet/dts/           # en751221.dtsi (nested DSA) + en751221_tplink_archer-xr500v.dts
target/linux/econet/en751221/config-6.12     # kernel config (diet)
target/linux/econet/image/en751221.mk        # device tplink_archer-xr500v + image recipe
target/linux/econet/base-files/lib/preinit/79_ubi_attach   # UBIFS overlay attach/warmup hook
target/linux/econet/patches-6.12/  # 700 mac-increment, 910/911 NAND/BMT, 913 mt7603-port0
config.seed                        # diffconfig — seed as .config before make defconfig
scripts/ docs/ notes/ codex-runs/  # tooling + iteration log
```

The `econet-eth` package builds **five** kernel modules from one source tree, which is why the DSA story lives in this one Makefile: `econet-eth.ko` (main GDM/QDMA eth), `econet-gsw.ko` (DSA switch core, an MT7530 fork with EN751221 support), `econet-gsw-mdio.ko` and `econet-gsw-mmio.ko` (MDIO- and MMIO-probe variants), and `mtk-tag.ko` (the DSA "MTK" tag protocol). See **Ethernet & nested DSA** for how these compose into the dual-switch topology.

> **Where the VoIP pieces live.** `kmod-econet-pcm` (the SLIC/PCM kernel modules `pcm-en751221.ko` + `econet-slic.ko`) is listed in the XR500v `DEVICE_PACKAGES` in `target/linux/econet/image/en751221.mk`, so the kernel side ships *inside the image*. Only the **userspace** — the `baresip` SIP stack and the `xr500v-callmgr` daemon that bridges the SLIC hook to baresip's control socket — lives on the writable UBIFS overlay and is started from `rc.local`. See **VoIP / FXS** for the runtime layout.

### config.seed

`.config` is gitignored, so the build selections live in `config.seed` (a `diffconfig`, ~89 lines). Seed it as `.config`, then run `make defconfig`. The load-bearing lines:

```text
CONFIG_TARGET_econet=y
CONFIG_TARGET_econet_en751221=y
CONFIG_TARGET_DEVICE_econet_en751221_DEVICE_tplink_archer-xr500v=y
CONFIG_TARGET_PER_DEVICE_ROOTFS=y
# CONFIG_KERNEL_DEBUG_INFO is not set        # kernel diet — must fit kernel1 (3 MB)
# CONFIG_KERNEL_KALLSYMS is not set          # kernel diet
CONFIG_PACKAGE_kmod-dsa-mt7530=m             # nested DSA child switch
CONFIG_PACKAGE_kmod-mt7603=m                 # 2.4 GHz radio
CONFIG_PACKAGE_kmod-usb-xhci-mtk=y           # USB
CONFIG_PACKAGE_kmod-usb-storage=y
CONFIG_PACKAGE_kmod-fs-vfat=y / -fs-exfat=y
CONFIG_PACKAGE_wpad-basic-mbedtls=y          # WiFi auth
CONFIG_PACKAGE_luci=y ...                     # web UI
```

The kernel-diet lines (`KALLSYMS`/`DEBUG_INFO` off) are not optional: the compressed kernel must fit the on-flash `kernel1` partition, which is **3 MB (3072k)**. With debug symbols in, the LZMA kernel overflows and the `dd conv=sync` (which rounds up to the next 3072k multiple) silently produces an image the bootloader cannot decode. See **256 MB RAM & kernel diet** for the full story.

## Build environment

Builds are done on a dedicated Linux build host (a cloud VM works well), kept separate from the editing workstation. A second machine is used purely for editing and as a git backup of the overlay; it carries no `build_dir`.

| | |
|---|---|
| Build host | any x86-64 Linux box with the OpenWrt build prerequisites |
| OpenWrt tree | `~/openwrt` (base cjdelisle `f3605b31`) |
| Image output | `~/openwrt/bin/targets/econet/en751221/` |
| Access | SSH (key-based); the iterate loop below assumes `<build-host>` is an SSH alias |

> **Develop on SNAPSHOT, not a stable release.** The `econet` target tracks the OpenWrt main/SNAPSHOT branch and a specific kernel point release (e.g. `6.12.87`). Do **not** try to back-port the SNAPSHOT econet target onto a stable OpenWrt release (e.g. one carrying `6.12.71`): the kernel-version skew plus the feed/version locks of a release branch make the target unbuildable, surfacing as `base-files= is not a valid world dependency` during `make defconfig`/`make`. Keep development on SNAPSHOT and only back-port once the device is officially merged upstream and the target lands in a release.

> **Clock caveat:** a remote build host may run on a different timezone (e.g. UTC) than the operator. Run `date` on the build host before declaring an artifact "old" by mtime.

### Clean reconstruction from scratch

The overlay is verified to reproduce the functional image from a clean base:

```bash
git clone https://github.com/cjdelisle/openwrt.git && cd openwrt
git checkout f3605b31fb          # pinned base (plan-b-nazox1)
# apply the overlay (copy package/, target/, config.seed from Cris7015/xr500v-openwrt)
cp /path/to/overlay/config.seed .config
make defconfig
make -j$(nproc)
```

A clean-room run of exactly this (fresh base + overlay + `config.seed` + empty `build_dir`) produced `rc=0` and a working XR500v image (~8.4 MB) containing the correct `econet-eth` (c2f855cf, no zombie), the USB modules, the DSA drivers and tagger, a sub-3 MB kernel, and a DTB with `u3p-dis-msk` (USB) and `reg=<0x0 0x10000000>` (256 MB).

### Workspace gotchas (don't edit the wrong kernel)

The single biggest "where am I editing" trap: `find … -name <file>.c` lists the **toolchain** kernel before the real one. Always filter to the target build dir.

| Role | Path |
|---|---|
| Real kernel (edits compile into the image) | `build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/` |
| Stale / vanilla (never edit) | `build_dir/toolchain-mips_24kc_gcc-14.3.0_musl/linux-6.12.80/` |

Filter `find` by `target-*/linux-econet_en751221` or `grep -l en7528`. And **direct `build_dir` edits are fragile** — they are lost whenever the build dir is re-extracted/reset. Anything meant to persist must become a real patch file under `target/linux/econet/patches-6.12/` (this is exactly how the SCU-clock and en7528 PCIe fixes became patches 912/913). Edit persistent things in the overlay repo (`~/xr500v-openwrt`), which is the committable backup, and push them up to the build host.

## The iterate → build → flash loop

A typical iteration:

```bash
# 1. push a change (a patch file, a DTS edit, or a python patcher)
scp patch.py <build-host>:/tmp/ && ssh <build-host> 'python3 /tmp/patch.py'

# 2. rebuild on the build host
ssh <build-host> 'cd ~/openwrt && \
  make package/kernel/econet-eth/{clean,compile} V=s && \
  make target/linux/install V=s'           # or a full `make -j$(nproc)`

# 3. pull artifacts to the local TFTP root
#    image -> <tftp-root>/iterN_*.bin
```

> **Prefer live module reload over reflashing for iteration.** On the test unit a block started failing `MEMERASE64` after roughly 19 reflash cycles. To be clear this is *not* SPI-NAND wear — the endurance is on the order of ~100k erase cycles and `en75_bmt` reported `worn: 0` — so the cause is unexplained (a grown bad block, or an artifact of the flashing procedure), not endurance. Either way it is a concrete reason to minimise flash writes during iteration. The fastest and least destructive inner loop is to build the changed kernel module on the build host, copy the `.ko` to the device's UBI overlay (`/lib/modules/...`), and `rmmod`/`insmod` it live — no flash write at all. The full slice-and-flash flow below is only needed for kernel/DTB/squashfs changes that cannot be expressed as a module reload.

**Always `make package/kernel/econet-eth/clean` before a fresh build**, and never leave backup directories under `package/` (see trap below). The image lands at:

```
~/openwrt/bin/targets/econet/en751221/openwrt-econet-en751221-tplink_archer-xr500v-squashfs-sysupgrade.bin
```

### Flashing (slot B, from stock only)

The flash flow is intentionally conservative — it goes through the stock OEM telnet, never through running OpenWrt:

```bash
# a. patch the proprietary TrendChip / tplink-v2 header (else the bootloader crashes
#    decoding the image). Verify the ELF entry each build; the patcher's --entry default
#    can drift.
python3 ~/xr500v-openwrt/scripts/patch_trendchip_header.py IN.bin OUT.bin

# b. slice kernel1 + rootfs1, TFTP them in, mtd-write — from STOCK telnet :2323
SRC=OUT.bin ROUTER_IP=<stock-ip> PC_IP=<pc-ip> bash <repo>/…/flash-iter111.sh
#   verify K_RC_0 and R_RC_0

# c. select slot B and COLD power-cycle
#    bflag set 1   (in the bootloader)   ;  then a real power cycle
```

Key rules (each learned during bring-up):

- **Flash only from stock telnet `:2323`.** `mtd write` from running OpenWrt corrupts the NAND.
- **`writeflash` argument order is `file SIZE OFFSET /dev/mtd0`** — inverting size/offset clobbers misc + kernel1 + rootfs1.
- **`bflag set 0` = stock OEM, `bflag set 1` = OpenWrt slot B.** The bootloader's `boot` command crashes; always power-cycle.
- **COLD boot is mandatory** for the MT7603 (2.4 GHz) SCU clock — a warm reboot / `bflag` change does not re-apply it.
- **Do not run `cfg_manager show` (or any `cfg_manager` subcommand) on stock OEM unless you know what it does.** On the stock firmware, `/userfs/bin/cfg_manager show` rewrites the romfile (mtd2) with factory defaults — it logs `Romfile format is wrong, we use default romfile to replace current setting romfile` and overwrites the live configuration. Back up the romfile before touching `cfg_manager`.
- Slot A (stock) stays untouched; the UART path (interrupt bootloader → `bflag set 0` → power-cycle → stock) is the recovery net.

## UBIFS overlay persistence

OpenWrt's writable overlay is a **UBIFS volume on the SPI-NAND**, brought up by a custom preinit hook. The image itself ships only a raw squashfs (`append-rootfs`, **not** `append-ubi`) — the UBI volume is provisioned once on the device, from running OpenWrt.

### Why not append-ubi / flash UBI from stock

Pre-loading a UBI image through stock `mtd writeflash` does **not** work: the OEM `writeflash` lays down data plus OEM-format ECC in the OOB area, and mainline `spi-nand + en75_bmt` reads that OOB for bad-block detection and EC-header CRC. On attach it then reports the written PEBs as bad ("27/128 bad PEBs", "empty MTD device detected"). Empty 0xFF-padded PEBs are fine; written ones are rejected. Therefore the volume must be created with mainline ECC, from inside OpenWrt.

### Kernel + DTS support (already in the overlay)

| Component | What it does |
|---|---|
| DTS `&nand { econet,bmt; econet,can-write-factory-bbt; econet,factory-badblocks = <>; }` | Empty factory-badblocks list bypasses the destructive OOB scan that renumbered the user area every boot |
| DTS `linux,rootfs;` on `rootfs1` (mtd7), label `rootfs_stock` on slot-A `rootfs` | Makes `mtd-split` operate on slot B |
| `910-en75_bmt-block-isbad-override.patch` | Override `mtd->_block_isbad`→0 and `_block_markbad`→no-op (so UBI isn't confused by OOB residue / false markers); clean bbt/bmt init when `count==0`; skip `w_sync_tables` on empty tables (each boot otherwise consumed a reserve block) |
| `911-nand-erase-skip-isbad.patch` | `nanddev_erase` skips the `nanddev_isbad` gate that rejected erases on healthy blocks with OOB residue |
| `79_ubi_attach` preinit hook | `ubiattach` in userspace (kernel-time attach causes OOB issues), `mknod /dev/ubi0_0` fallback from `/sys/class/ubi/ubi0_0/dev`, and a **UBIFS warmup** mount+umount of `rootfs_data` to trigger journal replay so `mount_root` doesn't fail `-EINVAL` after a power cycle |

These two kernel patches plus the preinit hook are exactly what any first EN751221 device combining a UBIFS overlay with `can-write-factory-bbt` needs, and are upstreamable to `cjdelisle/openwrt`.

### First-boot provisioning (the gotcha)

The preinit hook **attaches and mounts** an existing `rootfs_data` volume — it does **not create** one. After a fresh flash the UBI partition is empty, so the hook logs "missing /dev/ubi0_0", `mount_root` falls back to tmpfs, and every `uci commit` is lost on reboot. This is the single most common "why didn't my config persist" symptom.

Provision **once per device**, from running OpenWrt, on the slot-B rootfs partition (the mtd number is the slot-B `rootfs1` — historically `mtd7`; on a repartitioned image with a dedicated `openwrt_ubi` slot it is e.g. `mtd10`, so confirm with `cat /proc/mtd`):

```sh
ubidetach -m <mtd> 2>/dev/null
ubiformat /dev/mtd<mtd> -y
ubiattach -m <mtd>
ubimkvol /dev/ubi0 -N rootfs_data -m      # name MUST be rootfs_data (fstools/mount_root)
sync
reboot
```

After this one-time step the preinit hook finds `/dev/ubi0_0` on every boot, the warmup runs, `mount_root` sets up `overlayfs:/overlay on /` over UBIFS, and config (and live-installed modules — the whole VoIP stack reloads from `/lib/modules` + `rc.local` without reflashing) persists across reboots. Confirmed surviving reboots after the ~28-iteration bring-up.

> A permanent fix is noted as pending: add `if [ ! -e /dev/ubi0_0 ]; then ubimkvol /dev/ubi0 -N rootfs_data -m; fi` to the hook after `ubiattach`, making provisioning automatic. The manual one-time step is kept for now because flashing is occasional.

### Reflashing without losing the overlay

To update kernel/squashfs while preserving the UBI volume, write **only** the 3 MB squashfs region from stock telnet (preserving everything at/after the UBI offset):

```sh
# from stock telnet :2323 — writes only 3 MB at NAND offset 0x1b00000
mtd writeflash /tmp/r1.bin 3145728 28311552 /dev/mtd0
```

Two reasons to minimize reflashing:

- Each rollback to stock OEM lets the stock bootloader's `en75_bmt` scan possibly re-write the BBT and shift the user area, which can damage UBI persistence — so keep round-trips to stock to a minimum.
- Minimise erase/write cycles anyway. On the test unit a block started failing `MEMERASE64` after roughly 19 reflash cycles — this is *not* SPI-NAND endurance wear (~100k-cycle endurance; `en75_bmt` reported `worn: 0`), so it is an unexplained device condition rather than a build issue, but still a concrete reason to favor the live-module-reload workflow (see the iterate loop above) for day-to-day iteration and reserve flashing for kernel/DTB/squashfs changes.

## Build traps and gotchas

### The duplicate-package stale-driver trap

OpenWrt scans **all of `package/`** recursively for any `Makefile` and builds every package it finds. A backup directory such as `package/kernel/econet-eth.iter56-backup/` that defines the *same* `kmod-econet-eth` but pins the *older* commit (`1db74f83`) will therefore be compiled in parallel with the good one, produce a second `.apk`, and — because the rootfs picks one — install the **old** driver. Symptom:

```
econet_eth 1fb50000.ethernet: error -EINVAL: insufficient register space
econet_eth: probe failed -22
```

The cause is a struct-size mismatch with the DTS register window:

| Driver commit | `struct en751221_regs` size | DTS `reg` window |
|---|---|---|
| `c2f855cf` (current) | `0x8000` (`switch_regs` commented out) | `reg = <0x1fb50000 0x8000>` (matches) |
| `1db74f83` (old) | `0x10000` (`switch_regs` present) | needs `0x10000` (no longer matches) |

The driver asserts `resource_size(reg) >= sizeof(struct en751221_regs)`. While the DTB was stale at `0x10000` the two matched and it "worked"; the moment a `target/linux` rebuild regenerated the DTB to `0x8000`, the old `0x10000` driver failed probe. **Rules:**

- Never keep a backup dir under `package/` (no `*.bak`, `*-backup`, `*.iter*`, `*-disabled`). Move backups to `/tmp` or `~`, outside the tree. The same applies to any kernel-tree package that can shadow a `package/` one — e.g. the `mt76` tree: keep exactly one copy in the build, since a duplicate or stale tree silently wins in the rootfs.
- Diagnose with `find . -name "kmod-X*.apk"` (two versions = trouble) and `find . -name X.ko | xargs md5sum` (differing hashes across build dirs).
- Full fix: `mv` the backup out, delete the stale `.apk` in `bin/targets/.../packages/` and `staging_dir/packages/<target>/`, delete the old build dir, **nuke `build_dir/target-*/root-<target>`** (the stale rootfs staging), then rebuild.

### Staleness notes

- `touch package/.../Makefile` forces a repackage **but re-extracts the source** — losing any manual `build_dir` edits. Use patch files, not `build_dir` edits.
- `.ko` files are stripped on install, so the squashfs hash differs from the `build_dir` hash. Compare by content / a marker symbol (e.g. an `EN75DBG` instrumentation string), not by raw md5.
- A diagnostic patch that adds `dev_info(... resource_size ... sizeof ...)` survives re-extracts and is the reliable way to confirm which driver actually loaded at runtime.

### Header / entry point

`scripts/patch_trendchip_header.py` writes the TrendChip fields the bootloader reads after decompression (magic, kernel entry, rootfs offset, sub-magic). Two recurring issues: (1) without running it, the bootloader can't decode the image and crashes; (2) its `--entry` default is effectively hardcoded — verify the ELF entry of each build, or the bootloader crashes on a wrong `CP0_EPC`.

### Network bring-up

The DSA bridge assembles correctly only on a **clean boot**, not via `network restart` / `ifup`. And the DSA conduit `eth0` must **not** be a member of `br-lan` (the bridge rx_handler bypasses the DSA tagger); `br-lan` is `lan1..lan4` (+ WiFi). See **Ethernet & nested DSA**.

## Cross-references

- **Ethernet & nested DSA** — the dual-MT7530 topology, the five `econet-eth` modules, the tagger, and patches 210/220/240/330 (incl. the BQL TX-throughput fix).
- **WiFi (MT7603 + MT7662)** — the dual-PCIe radios, the `004` mt76 LED patch and patch `913`, and the synthetic-EEPROM DTS for the 2.4 GHz radio.
- **USB** — the `u3p-dis-msk` workaround and the USB module set.
- **256 MB RAM & kernel diet** — why `KALLSYMS`/`DEBUG_INFO` must be off and the `dd conv=sync` rounding.
- **Flash layout, slots & brick taxonomy** — partition map, `bflag`, `writeflash` arg order, soft vs hard brick.
- **VoIP / FXS (Le9642 SLIC)** — the `econet-pcm` package and the reconstructed driver stack.
- **Panel LEDs / GPIO (gpio-tc3162)** — the `0x1fbf0200` controller and triggers.