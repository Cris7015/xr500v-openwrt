## Summary

The Archer XR500v boots via a proprietary TrendChip/EcoNet-derived bootloader (the `bldr>` prompt) that selects one of two firmware slots through a single byte-flag, `bflag`: `0` boots the stock OEM TCLinux image (slot A), `1` boots OpenWrt (slot B). The kernel partition for each slot is not a plain Linux image but is wrapped in a 512-byte proprietary header that the bootloader parses after LZMA decompression; producing a bootable OpenWrt image requires post-processing the OpenWrt `sysupgrade.bin` with `scripts/patch_trendchip_header.py` so those header fields (magic, kernel entry, rootfs offset/size, sub-magic) are valid — without it the bootloader dereferences garbage and crashes. The NAND is laid out in fixed partitions inherited from the OEM firmware, and OpenWrt adds a dedicated 64 MB UBI partition in the previously-unused free area between 0x3000000 and 0x7000000 without disturbing any OEM partition. A firm rule, learned during development, is that slot B must only be flashed from the **stock firmware** over a telnet shell on port 2323 — writing it from a running OpenWrt corrupts the NAND because the mainline BMT/BBT handling diverges from the OEM's.

This page is the canonical reference for the boot path, the full partition map with offsets, the header format and the patcher, the flashing procedure with its exact argument order, the repartition work, the UART pinout, the bootloader command set, and the brick taxonomy / recovery procedure. The device is recoverable from almost any software mistake (soft brick) via UART, the bootloader prompt, a local TFTP server and full NAND backups; only damaging the `boot` partition or the SoC itself is a true (hard) brick. See [02-hardware-chip-inventory.md](02-hardware-chip-inventory.md) for the SoC/chip inventory and [11-openwrt-port-build-persistence.md](11-openwrt-port-build-persistence.md) for the DTS and driver details.

---

## Boot chip & slot selection (bflag)

The on-NAND bootloader is a TrendChip-lineage `bldr` (prompt `bldr>`). At power-on it prints a banner and offers a very short interception window (~0.1 s); to drop to the prompt you must mash Enter the instant the device is powered. If not interrupted it autoboots: it reads `bflag`, picks the slot, decompresses that slot's kernel and jumps to it.

`bflag` is the slot selector:

| `bflag` | Slot | Boots | Kernel partition | Rootfs partition |
|---------|------|-------|------------------|------------------|
| `0` | A | Stock OEM TCLinux | `kernel`  @ 0x80000 | `rootfs_stock` @ 0x380000 |
| `1` | B | OpenWrt | `kernel1` @ 0x1800000 | `rootfs1` @ 0x1b00000 |

The flag is stored in the `bootflag` partition (a small structure beginning with the ASCII magic `DUAL` followed by status flags). Its offset is believed to be referenced by a hardcoded address inside the bootloader, so this partition must not be moved.

### Changing slots safely

Two bootloader quirks govern slot switching:

1. There is **no working reset/reboot command** from the bootloader prompt. The manual `go` / `boot` command is broken — issuing it after a `set` reliably crashes with `Undefined Exception EPC=81fb06e4`. A physical power-cycle is mandatory to start a slot; never use `go`/`boot`.
2. Hex arguments at the `bldr>` prompt must be given **without** the `0x` prefix.

The only reliable way to change slots is to set the flag and then power-cycle so the bootloader autoboots on its own:

```text
# Cycle 1: power on, mash Enter to catch bldr>
bldr> bflag set 1        # 1 = OpenWrt, 0 = stock OEM
# Cycle 2: power off, power on, DO NOT intercept -> autoboot into the new slot
```

### Bootloader command set

Commands observed at the `bldr>` prompt (hex arguments take no `0x` prefix):

| Command | Purpose |
|---------|---------|
| `bflag get` / `bflag set <0\|1>` | Read / set the boot slot flag (0 = stock A, 1 = OpenWrt B). |
| `imginfo` | Show the `os1:` / `os2:` image headers for both slots. |
| `memrl <addr>` | Read a 32-bit word from a physical/kseg1 address. Reliable for some address ranges; faults on certain peripheral regions (a corrupt TLB-miss handler in the bootloader). |
| `memwl <addr> <value>` | Write a 32-bit word to an address. |
| `xmdm <addr> <len>` | XMODEM upload to `<addr>`. Both arguments are hex **without** the `0x` prefix. |
| `cpufreq <156-450>` | Set the CPU frequency in MHz; values above the stock clock allow overclocking. |
| `flash`, `dump`, `jump <addr>` | Flash op, memory dump, and jump-to-address (the latter shares the crash behaviour of `go`/`boot`). |

There is no `reset`/`reboot`; recovery and slot switching always end in a manual power-cycle.

---

## NAND partition map

The layout is fixed-partition, inherited from the OEM `/proc/mtd` and reproduced in the OpenWrt DTS (`target/linux/econet/dts/en751221_tplink_archer-xr500v.dts`). All offsets/sizes are within `/dev/mtd0` ("ALL"), which the EN751221 BMT exposes as 0x07000000 (112 MB) of user area before its reserve region. The values below are taken from the live DTS and are canonical; some earlier notes rounded `others` to "5 MB" and `bootflag` to a "12-byte" structure — the DTS partition sizes are `0x4e0000` (4.96 MB) and `0x20000` (128 KB) respectively.

| Offset | Size | Label | Slot | Contents / notes |
|--------|------|-------|------|------------------|
| 0x0000000 | 0x040000 (256 KB) | `boot` | — | Bootloader (proprietary `bldr`). Marked `read-only` in DTS. **Damaging this = hard brick.** |
| 0x0040000 | 0x040000 (256 KB) | `romfile` | — | OEM config romfile (XML). `read-only` in OpenWrt. |
| 0x0080000 | 0x300000 (3 MB) | `kernel` | A | Stock OEM kernel (TrendChip header + LZMA). |
| 0x0380000 | 0x1000000 (16 MB) | `rootfs_stock` | A | Stock OEM squashfs (legacy LZMA). `read-only`; renamed from "rootfs" so OpenWrt's mtdsplit does not bind to it. |
| 0x1380000 | 0x480000 (4.5 MB) | `misc` | — | Factory data. ETH MAC @ 0xf100; MT7662 EEPROM @ 0xe0000 (512 B). Exposed as nvmem-cells. `read-only`. |
| 0x1800000 | 0x300000 (3 MB) | `kernel1` | B | **OpenWrt kernel.** |
| 0x1b00000 | 0x1000000 (16 MB) | `rootfs1` | B | **OpenWrt rootfs** (raw squashfs). Marked `linux,rootfs`. |
| 0x2b00000 | 0x4e0000 (4.96 MB) | `others` | — | OEM storage; mostly 0xFF with ~128 KB of encrypted/random data near 0x80000 and 0x120000. Do not overwrite. |
| 0x2fe0000 | 0x020000 (128 KB) | `bootflag` | — | The `bflag` / `DUAL` structure. Bootloader-referenced; do not move. |
| 0x3000000 | 0x4000000 (64 MB) | `openwrt_ubi` | B | **OpenWrt addition.** UBI overlay, provisioned at runtime (see below). Confirmed erased (pure 0xFF) before use. |

Notes:

- The nine OEM partitions (`boot` … `bootflag`, including `misc`) sum to 0x3000000 (48 MB). The region 0x3000000–0x7000000 (64 MB) was empty in the factory image and is where OpenWrt's `openwrt_ubi` lives — so **no OEM partition is touched** by the OpenWrt port.
- In the historical RE backup, the partitions were dumped as `mtd1_boot.bin` … `mtd9_bootflag.bin` (see [Backups & recovery](#backups--recovery-artifacts)); `misc` corresponds to `mtd5_misc.bin`. The numbering there is the dump order, not the DTS partition index.
- The `misc` partition supplies the real factory MAC (offset 0xf100) and MT7662 calibration EEPROM (offset 0xe0000) via DT nvmem cells; see [05-wifi-mt7603-mt7662.md](05-wifi-mt7603-mt7662.md).

### Visual flash map

The dual-boot **A/B** layout at a glance. Slot A keeps the **untouched stock OEM firmware** as a recovery anchor; OpenWrt lives entirely in slot B; the writable overlay is a dedicated UBI partition in the formerly-empty tail. This is **not** a typical OpenWrt layout (most devices replace the firmware outright) — it is exactly what makes the XR500v recoverable from any soft brick.

```
0x0000000 ┌────────────────────────────────────┐
          │  boot           256 KB   bldr       │  RO — damage = HARD BRICK
0x0040000 ├────────────────────────────────────┤
          │  romfile        256 KB   OEM config │  RO
0x0080000 ╞════════════════════════════════════╡ ┐
          │  kernel           3 MB   stock      │ │  🅰 SLOT A
0x0380000 ├────────────────────────────────────┤ │     stock OEM, left
          │  rootfs_stock    16 MB   stock      │ │     intact = recovery
0x1380000 ╞════════════════════════════════════╡ ┘
          │  misc           4.5 MB   EEPROM+MAC │  RO — WiFi cal + MAC (nvmem)
0x1800000 ╞════════════════════════════════════╡ ┐
          │  kernel1          3 MB   OpenWrt    │ │  🅱 SLOT B
0x1b00000 ├────────────────────────────────────┤ │     OpenWrt
          │  rootfs1         16 MB   OpenWrt    │ │     (squashfs · linux,rootfs)
0x2b00000 ╞════════════════════════════════════╡ ┘
          │  others        4.96 MB   reserve    │  OEM data — do not overwrite
0x2fe0000 ├────────────────────────────────────┤
          │  bootflag       128 KB   A/B sel    │  `DUAL` magic · bldr-referenced
0x3000000 ╞════════════════════════════════════╡  ◄── factory free space starts
          │  openwrt_ubi     64 MB   overlay    │  OpenWrt addition · UBI · persistent
0x7000000 └────────────────────────────────────┘
            0x7000000–0x8000000 (16 MB) = BMT reserve / unallocated
```

At runtime `mtdsplit` also exposes a virtual `rootfs_data` sub-partition (the free space *after* the squashfs inside `rootfs1`). It is unused here because the writable overlay is the dedicated `openwrt_ubi` (UBI) volume, not the squashfs tail — so on this device `rootfs_data` stays empty.

### DTS source (canonical)

The map above is declared **once**, as a standard `fixed-partitions` node, in the device tree (`target/linux/econet/dts/en751221_tplink_archer-xr500v.dts`); `/proc/mtd` and this page both derive from it:

```dts
&nand {
	status = "okay";
	econet,bmt;                          /* TrendChip bad-block-table */

	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		partition@0       { label = "boot";         reg = <0x0000000 0x0040000>; read-only; };
		partition@40000   { label = "romfile";      reg = <0x0040000 0x0040000>; read-only; };
		partition@80000   { label = "kernel";       reg = <0x0080000 0x0300000>; read-only; };    /* slot A */
		partition@380000  { label = "rootfs_stock"; reg = <0x0380000 0x1000000>; read-only; };    /* slot A */
		partition@1380000 { label = "misc";         reg = <0x1380000 0x0480000>; read-only;
			nvmem-layout { compatible = "fixed-layout"; /* mac-base @0xf100, MT7662 eeprom @0xe0000 */ };
		};
		partition@1800000 { label = "kernel1";      reg = <0x1800000 0x0300000>; };               /* slot B */
		partition@1b00000 { label = "rootfs1";      reg = <0x1b00000 0x1000000>; linux,rootfs; };  /* slot B */
		partition@2b00000 { label = "others";       reg = <0x2b00000 0x04e0000>; };
		partition@2fe0000 { label = "bootflag";     reg = <0x2fe0000 0x0020000>; };
		partition@3000000 { label = "openwrt_ubi";  reg = <0x3000000 0x4000000>; };               /* overlay */
	};
};
```

---

## TrendChip image header & the patcher

Each slot's kernel partition begins with a **512-byte proprietary header**, then the LZMA payload at offset 0x200. This is *not* the OpenWrt `2RDH`/TRX header — in fact the bootloader's own `mtd` utility rejects `2RDH` images ("Bad trx header"). OpenWrt's `tplink-v2-header` recipe step produces a `03 00 00 00 ver. 2.0` magic the bootloader accepts, but it does **not** fill the TrendChip-specific fields the bootloader reads *after* decompression. Those must be patched in.

The stock kernel header (`mtd3_kernel.bin`) layout, big-endian 32-bit:

| Offset | Field | Stock value | Meaning |
|--------|-------|-------------|---------|
| 0x00 | TP-Link sub-magic | `03 00 00 03 cc cc 22 33 …` | image magic |
| 0x60 | TrendChip primary magic | `4c 3d 2e 1f aa 55 aa 55` | required or bldr crashes |
| 0x68 | decompress address | KERNEL_LOADADDR | where the LZMA wrapper is placed |
| 0x6c | **kernel entry point** | `0x80020000` | address the bldr jumps to after decompress |
| 0x70 | kernel size hint | `0x01300000` | |
| 0x74 | header size | `0x00000200` | = 512 bytes |
| 0x78 | LZMA compressed size | (image-specific) | kept from tplink-v2 |
| 0x7c | **rootfs offset** | `0x00300000` | = kernel partition size (3 MB) |
| 0x80 | **rootfs size** | (image-specific) | OpenWrt uses 0x01000000 (16 MB) |
| 0x88 | padding | `00 00 00 00` | |
| 0x8c | sub-magic | `55 aa 01 01` | required |

If these fields are left at the `tplink-v2-header` defaults (0xFFFFFFFF / 0), the bootloader prints `kernel_rootfs_ptr to FFFFFFFF`, decompresses, then immediately throws `Undefined Exception` (a TLB/KSEG2 dereference into 0xc0000000). The patcher `scripts/patch_trendchip_header.py` rewrites the header in place:

```bash
# After ANY OpenWrt rebuild that regenerates the sysupgrade.bin:
python3 scripts/patch_trendchip_header.py \
    openwrt-...-xr500v-...-sysupgrade.bin sysupgrade_patched.bin \
    --entry 0x80020000
```

It writes the magic at 0x60, the entry at 0x6c, the rootfs offset (0x300000) at 0x7c, the rootfs size (0x1000000) at 0x80, and the sub-magic at 0x8c; the decompress address, header size and LZMA size are kept from the `tplink-v2-header` output.

### Key detail: the kernel entry is always 0x80020000

The entry written at 0x6c must be **`0x80020000`** = `KERNEL_LOADADDR` (from `target/linux/econet/image/Makefile`). This is **not** the "Entry point address" shown by `readelf -h vmlinux` (typically `0x817xxxxx`) — that ELF symbol is the *internal, post-relocation* entry and using it breaks boot. The bootloader decompresses the LZMA blob to `decompress_addr` (= the load address) and jumps to the start of the decompressed image (the `head.S` bootstrap at byte 0), so the entry must equal the load address.

This trap recurred during development: feeding `--entry 0x81792820` (read from the ELF) produced `CP0_EPC=0x81792874` (entry+0x54 — a jump into the middle of the decompressed kernel) and a crash, after which the bootloader auto-fell-back to stock OEM. Always verify the patched header at 0x6c reads `80020000`. The patcher's default is `0` (keep tplink-v2 value), so pass `--entry 0x80020000` explicitly, or confirm the build's load address matches.

The header patch is a manual step: `make target/linux/install` / `make world` regenerate the unpatched `sysupgrade.bin`, so the patcher (and the slot slicing below) must be re-run on every build.

---

## Flashing slot B

### The one hard rule: flash from stock only

Slot B (`kernel1` + `rootfs1`) must be written from the **stock OEM firmware** via its telnet shell on port **2323**, using the OEM `mtd` tools. **Do not** flash from a running OpenWrt with `mtd write`, even though the command exits 0 and an immediate re-read appears to verify.

Cause: the mainline `en75_bmt` driver, without the factory BBT correctly populated, skips bad blocks via per-boot OOB scanning, which makes the logical→physical mapping inconsistent under concurrent writes. The write *claims* success but the data is corrupt on later read. Symptoms of an OpenWrt-side write (seen twice in development): repeated `Skipping bad block at 0x002a0000 [e]` at identical offsets, `Failed to get erase block status` at the end of the loop, and on the next boot `decompression error -- System halted` or `SQUASHFS error: xz decompression failed`. A stock-side write shows a single clean `[e][w]` per block and consistent re-reads.

To get into the stock telnet shell, boot stock (`bflag 0`) and start telnetd (this does not persist across reboot):

```sh
ifconfig br0:1 192.168.1.1 netmask 255.255.255.0 up
/usr/sbin/telnetd -l /bin/sh -p 2323
```

### writeflash argument order — file SIZE OFFSET device

The OEM utility's argument order is **file, SIZE, OFFSET, device** — size comes *before* offset:

```text
/userfs/bin/mtd writeflash <file> <SIZE_BYTES> <OFFSET_BYTES> /dev/mtd0
```

Example, writing the 16 MB `rootfs1` at offset 0x1b00000:

```sh
/userfs/bin/mtd writeflash /tmp/r1.bin 16777216 28311552 /dev/mtd0
#                                      ^SIZE     ^OFFSET (0x1b00000)
```

Inverting these (`28311552 16777216`) starts the write at offset 0x1000000 and writes 16 MB forward, clobbering the tail of `rootfs_stock`, `misc`, `kernel1` and the start of `rootfs1` — a recoverable but costly mistake. Sanity checks before any `writeflash`: compute SIZE and OFFSET independently in hex; if `OFFSET + SIZE > 0x8000000` you are writing past the device; if `OFFSET % 0x20000 != 0` you are mis-aligned (128 KB sectors).

`kernel1` is written by partition **name** (no offset), which is safe and accepts the proprietary header with `-f`:

```sh
/userfs/bin/mtd -f -e kernel1 write /tmp/k1.bin kernel1
```

The `-f` skips the "Bad trx header" validation (the proprietary header is not a TRX); a "not Error" line in the log is a false negative — RC=0 is the truth.

### Slicing the image

The patched `sysupgrade.bin` is split into the two partition payloads:

- `k1.bin` = first 0x300000 (3 MB) of the patched image → `kernel1`.
- `r1.bin` = from the squashfs magic `hsqs` to +0x1000000 (16 MB) → `rootfs1`. The `tplink-v2-header -R 0x400000` recipe places the squashfs at `0x200 + KERNEL_SIZE = 0x300200` for this device (KERNEL_SIZE = 3 MB), **not** at the originally hardcoded 0x400000 — the slicer locates `hsqs` dynamically to be safe.

### End-to-end procedure

`scripts/flash-from-wsl.sh` automates the loop: pull the freshly-built image from the build host, slice `k1.bin`/`r1.bin`, TFTP them to `/tmp` on the device, then `mtd -f -e kernel1 write …` and `mtd writeflash … 16777216 28311552 /dev/mtd0`. Summary:

```text
1. Build OpenWrt -> sysupgrade.bin
2. patch_trendchip_header.py ... --entry 0x80020000   (required)
3. Slice k1.bin (first 3 MB) and r1.bin (from hsqs, 16 MB)
4. Boot stock (bflag 0), start telnetd :2323
5. TFTP k1.bin/r1.bin to /tmp; mtd write kernel1; mtd writeflash rootfs1
6. bflag set 1, power-cycle (no intercept) -> OpenWrt
```

### First-boot UBI provisioning

On a freshly flashed device the `openwrt_ubi` partition is empty, so `mount_root` falls back to tmpfs. UBI is provisioned once per device from the running OpenWrt (it is intentionally **not** pre-loaded from stock — pushing a UBI image through the OEM flash path corrupts the OOB and makes mainline UBI mark good PEBs as bad). After the first OpenWrt boot:

```sh
ubiformat /dev/mtd<openwrt_ubi> -y        # one time
ubiattach -p /dev/mtd<openwrt_ubi>
ubimkvol /dev/ubi0 -N rootfs_data -m      # volume name must be rootfs_data
sync; reboot
```

The volume name must be `rootfs_data` for fstools/`mount_root` compatibility. The preinit hook `lib/preinit/79_ubi_attach` attaches it on every subsequent boot (it greps `/proc/mtd` for `openwrt_ubi`). This yields ~53 MB of persistent UBIFS overlay (vs ~9 MB in the squashfs `rootfs_data` split). The kernel does *not* auto-attach via an `ubi.mtd=` cmdline — userspace attach was found to be consistent where kernel-time attach mismarked OOB. See [11-openwrt-port-build-persistence.md](11-openwrt-port-build-persistence.md) for the BMT/BBT details and the DTS `econet,bmt` / `econet,can-write-factory-bbt` / `econet,factory-badblocks = <>` properties that make persistence stable.

---

## Repartition work (openwrt_ubi)

Before adding persistence, the device was surveyed read-only:

- Stock exposes `/dev/mtd0 = "ALL"` of 0x07000000 (112 MB), the EN751221 BMT user area before its reserve region (the last blocks of the 1024-block device are reserve, not user-usable).
- The nine existing partitions sum to 0x3000000 (48 MB).
- A full dump of 0x3000000–0x7000000 (16 chunks of 4 MB) confirmed the entire 64 MB region was pure 0xFF (erased).

The change was minimal and additive — a single new partition node, no existing partition moved or resized:

```dts
partition@3000000 {
    label = "openwrt_ubi";
    reg = <0x3000000 0x4000000>;   /* 64 MB */
};
```

`linux,rootfs` stays on `rootfs1`; the auto-created squashfs `rootfs_data` split is ignored in favour of `openwrt_ubi`. Because the UBI image is provisioned from the running system, the image recipe (`en751221.mk`) needs no change. Slot A and the bootflag are untouched, preserving the always-available recovery path.

---

## UART pinout & serial recovery

The board exposes four pass-through UART pads in a **vertical column between the SoC and the green GPON connector**, with no silkscreen labels. Serial parameters are **115200 8N1**. A 3.3 V USB-UART adapter (e.g. CP2102 → `/dev/ttyUSB0`) is used; `picocom --logfile` gives clean boot-log capture. The four pads, **top to bottom — from the pad nearest the `TP-GND5` ground test point downward — are `VCC`, `GND`, `RX`, `TX`** (confirmed against a working hookup; GND/VCC were first identified with a multimeter, TX/RX by elimination). Wire the adapter as GND↔GND, adapter-RX↔board-`TX`, adapter-TX↔board-`RX`, and **do not connect `VCC`** (do not drive the board's 3.3 V rail from the adapter).

To intercept the bootloader, mash Enter immediately on power-up — the interception window is only ~0.1 s. UART gives the `bldr>` prompt for slot switching and recovery, and the kernel/OpenWrt console. On the **stock OEM image** the serial console drops to a TCLinux login whose factory-default credentials are `admin` / `1234` (the standard TrendChip HGW default); that lands in a restricted CLI, from which a `reg read` command-injection escalates to a root shell (see [Stock firmware access](10-stock-firmware-access.md)). A typical recovery turns the device back into a network-reachable stock box, then continues over the network:

```sh
# after booting stock via bflag 0:
ifconfig br0:1 192.168.1.1 netmask 255.255.255.0 up
/usr/sbin/telnetd -l /bin/sh -p 2323
```

The relevant PCB marking is a **board revision** identifier, not a per-unit serial: `Archer XR500v Ver:1.1 / IPB-A`. (Each board also carries a separate per-unit serial number; that is unit-specific and not needed for any of the procedures here.)

---

## Brick taxonomy & recovery (canonical)

With UART + an accessible bootloader prompt + a local TFTP server + full NAND backups, essentially every software mistake is a recoverable **soft brick**. Only two things are true **hard bricks**.

### Hard brick (not recoverable software-only)

- **Overwriting the `boot` partition** (0x0–0x40000): the proprietary `bldr` is destroyed, the SoC finds no valid boot code, and there is no UART output. Recovery requires an external SPI/JTAG programmer or a board swap.
- **Physical SoC damage** (over-voltage, bad soldering, reflow accident).

### Soft brick (OS won't boot, board alive)

Corrupt data in **any NAND partition other than `boot`** — a bad `kernel1`/`rootfs1`, a mis-aligned `writeflash`, a bad header patch — is recoverable.

Recovery procedure:

1. Power-cycle and intercept the bootloader (mash Enter, ~0.1 s window) to reach `bldr>`.
2. `bflag set 0` — select the stock slot.
3. Power-cycle **without** intercepting → the bootloader autoboots stock OEM (slot A).
4. Start the stock telnet shell on port 2323 (`telnetd -l /bin/sh -p 2323`).
5. Reflash the damaged partition from the known-good NAND backup.

This has been done many times during development.

### Operating rules

- **Slot A is never touched outside recovery.** Do not write `kernel`, `rootfs_stock`, `misc`, `boot`, or `romfile` during normal work — slot A is the guaranteed path home.
- `bflag set 0` always returns to stock OEM; `bflag set 1` boots slot B.
- Take a fresh NAND backup before any significant flash.
- When proposing NAND-invasive experiments, classify the risk: hard (touching `boot` or the SoC) = stop and rethink; soft = acceptable with a recent backup and live UART.

---

## Backups & recovery artifacts

A complete factory NAND backup exists (9 partition dumps, ~48 MB, with MD5s). On stock firmware, `mtd7`/`mtd8`/`mtd9` have no device nodes and were exfiltrated via `dd if=/dev/mtdblock0 skip=<offset/4096>` from the "ALL" device:

| File | Size | Partition |
|------|------|-----------|
| `mtd1_boot.bin` | 256 K | boot |
| `mtd2_romfile.bin` | 256 K | romfile |
| `mtd3_kernel.bin` | 3 M | kernel (slot A) |
| `mtd4_rootfs.bin` | 16 M | rootfs_stock (slot A) |
| `mtd5_misc.bin` | 4.5 M | misc (factory MAC/EEPROM) |
| `mtd6_kernel1.bin` | 3 M | kernel1 (slot B) |
| `mtd7_rootfs1.bin` | 16 M | rootfs1 (slot B) |
| `mtd8_others.bin` | 4.9 M | others (incl. WiFi calibration) |
| `mtd9_bootflag.bin` | 128 K | bootflag |

MD5 sums are recorded alongside the dumps. These backups are what make the UART recovery path reliable: any slot-A or `misc`/`others` partition can be restored from a known-good image if a flash goes wrong.

---

## Cross-references

- [02-hardware-chip-inventory.md](02-hardware-chip-inventory.md) — SoC, RAM, NAND, chip inventory.
- [11-openwrt-port-build-persistence.md](11-openwrt-port-build-persistence.md) — DTS, image recipe, drivers.
- [05-wifi-mt7603-mt7662.md](05-wifi-mt7603-mt7662.md) — MT7603 / MT7662, factory MAC and EEPROM in `misc`.
- [11-openwrt-port-build-persistence.md](11-openwrt-port-build-persistence.md) — BMT/BBT, UBI provisioning, persistence.
