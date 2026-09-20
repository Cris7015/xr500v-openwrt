# Migrating the Archer XR500v v1 to U-Boot + UBI

_Status: performed once on the developer's unit on 19 September 2026, running r21 in production since
(GPON, WiFi, VoIP; 8/8 warm reboots and 4/4 power cuts in the soak). It is an **advanced, one-way**
procedure. Read everything before touching the router._

## What this does, and why you may not want it

The OEM bootloader (`bldr` / Bootbase, in the first 256 KiB of the NAND) is replaced by U-Boot with
the EN751221 DDR stage in front of it, and the remaining 127 MiB become one UBI partition. OpenWrt
then boots as a FIT image from a UBI volume, exactly like any modern NAND target: `sysupgrade`,
`fw_printenv`, a recovery TFTP boot from the bootloader prompt, and the whole flash for you.

What you lose:

- the stock TP-Link firmware slot, the web recovery and the OEM dual-slot scheme. There is no going
  back with a button; going back means a UART, the full NAND backup you are about to make, and the
  ROM recovery described below (a return to stock has **not** been exercised by us);
- the safety of never having written to the bootloader blocks. If the first 1 MiB ends up wrong the
  board only talks through its BootROM over UART.

What you keep: everything in the factory `misc` area (LAN/WAN MAC, EN7570 optical calibration, MT7662
EEPROM) is copied into a UBI volume and the kernel reads it from there through nvmem. GPON works on
the new layout with the factory calibration; that was the first thing checked after the migration.

Requirements:

- a **3.3 V UART** adapter (115200 8N1) soldered to the four unlabelled pads in a column between the
  SoC and the green GPON connector, and a way to send **1K-XMODEM with CRC** (the script below, or
  `sx -k` / TeraTerm / any terminal that does it cleanly);
- a **TFTP server** reachable from a LAN port (defaults below assume the server is `192.168.1.10`
  and the router `192.168.1.1`; change them with `setenv` if you must);
- OpenWrt already installed on the OEM layout (any `v2026.08.09`+ or r-image, BMT94 units only) with
  SSH access, to take the dumps. 2019/BMT81 units: **not covered**, do not try;
- one to two hours, a power switch within reach, and no fibre service you need during that time.

## Files

From the release assets (check `SHA256SUMS`):

| File | Purpose | sha256 |
|---|---|---|
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-uboot-bootloader.bin` (1 MiB) | `tcboot`: DDR stage + U-Boot, the image written to the `u-boot` partition | `5c32fe39ce97d01a573af057a96760e52d2689e510c77dcb0567a6f0df719a2a` |
| `u-boot-xr500v-ram.bin` (~720 KiB) | plain U-Boot, loaded into RAM by the BootROM chainloader (rescue and migration) | `38fac603f32945039fb683436a5d05b471d3200f93069d77e1e55c010bfb0194` |
| `en751221-chainloader-ddr.bin` (24 KiB) | BootROM XMODEM chainloader that runs the DDR stage, then receives U-Boot | `8735e7bbabf8740d286b274aea043ce351941dec87df5b3646485c2bf9eb83e3` |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-initramfs-kernel.bin` | r21 initramfs FIT, booted over TFTP by U-Boot | see `SHA256SUMS` |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-ubootmod-squashfs-sysupgrade.bin` | r21 FIT sysupgrade for the new layout | see `SHA256SUMS` |

Put the bootloader, initramfs and sysupgrade images in the TFTP root **under exactly those names**:
U-Boot's default environment refers to them as `bootloaderfile` and `bootfile`.

## Layouts

OEM layout (what the running OpenWrt shows in `/proc/mtd`):

| MTD | Offset | Size | Content |
|---|---|---|---|
| bootloader | 0x0 | 256 KiB | Bootbase / `bldr` |
| romfile, kernel, rootfs_stock | 0x40000 | 19.25 MiB | stock TP-Link firmware (slot A) |
| **misc** | 0x1380000 | 4.5 MiB | **MAC @0xf100, EN7570 calibration @0x20000, MT7662 EEPROM @0xe0000** |
| kernel1, rootfs1 | 0x1800000 | 19 MiB | OpenWrt slot B |
| others, bootflag | 0x2b00000 | ~5 MiB | OEM data, boot slot flag |
| openwrt_ubi | 0x3000000 | 64 MiB | OpenWrt overlay |
| (reserve) | 0x7000000 | 16 MiB | OEM bad-block management (BMT) reserve, not an MTD |

New layout:

| Partition / volume | Size | Content |
|---|---|---|
| `u-boot` (MTD) | 1 MiB | `tcboot`: DDR stage + U-Boot |
| `ubi` (MTD) | 127 MiB | UBI, with the volumes below |
| `ubootenv`, `ubootenv2` | 124 KiB each | redundant U-Boot environment (`fw_printenv`) |
| `misc` | 4.5 MiB | copy of the OEM misc area, same offsets (nvmem cells) |
| `tcboot_oem` | 256 KiB | copy of the OEM bootloader, kept for a possible return |
| `fit` | dynamic | OpenWrt kernel + rootfs FIT, created by `sysupgrade` |
| `rootfs_data` | rest | overlay, created by `sysupgrade` |

## Step 0 — back up the whole NAND (from the running OpenWrt, OEM layout)

Dump every partition and verify the checksums on both ends. Keep the dumps off the router, in two
places. `scripts/nand-dump-verify.sh` in this repository does exactly this over SSH:

```sh
mkdir xr500v-nand-$(date +%Y%m%d) && cd $_
bash /path/to/nand-dump-verify.sh 192.168.1.1      # one file per MTD + router/local md5 comparison
```

Two of those dumps are inputs of the migration. Rename copies of them into the TFTP root:

```sh
cp mtd4_misc.bin        /srv/tftp/xr500v-misc.bin          # 4718592 bytes
cp mtd0_bootloader.bin  /srv/tftp/xr500v-bootloader.bin    # 262144 bytes
```

Why dumps and not a direct read from U-Boot: the OEM kernel maps the flash through its own bad-block
table. U-Boot's SPI-NAND driver reads the OEM areas as garbage in places, so the factory data is taken
under the OEM-aware Linux driver, and U-Boot **verifies** it (sizes, MAC not erased, calibration magic
`0x07050700` at `0x20094`, EEPROM `0x7662` at `0xe0000`, `6578` at `0xc` of the bootloader) before it
erases anything. If any check fails, nothing is erased.

Also confirm the unit is a BMT94 one with no factory bad blocks in the user area:

```sh
dmesg | grep -i -E 'bmt|reserve'      # expect reserve 94, factory_bad: 0
```

## Step 1 — prove the rescue path before you need it

Hold **RESET** while powering on. The BootROM (mask ROM, cannot be broken by software) prints its
banner, goes quiet for about 20 seconds, then prints `done` and repeats. The recovery is entered by
typing **`x` during the silence before a `done`**; after the next `done` the ROM sends `C` and waits
for a 1K-XMODEM/CRC transfer of the chainloader. The chainloader then runs the DDR stage (PLL and DRAM
exactly as the flash bootloader would) and itself sends `C` for the U-Boot binary.

The script does the whole dance and logs the UART:

```sh
python3 scripts/bootrom-xrecover-ddr.py /dev/ttyUSB0 en751221-chainloader-ddr.bin u-boot-xr500v-ram.bin rescue.log
```

Pitfalls seen while developing it: any stray byte after the first `C` makes the ROM fall back to the
checksum protocol and abort; a bare Enter counts as a key; once in the ROM loop neither `reset` nor
the watchdog leave it, **only a power cut**; an empty Enter at the U-Boot prompt repeats the last
command.

You should reach `U-Boot>` with `DRAM: 256 MiB`, the ESMT SPI NAND detected and working Ethernet
(`ping $serverip`). Nothing has been written. If this step does not work, stop here: it is your only
way back after the next step.

## Step 2 — migrate (from the U-Boot loaded in RAM in step 1)

At the `U-Boot>` prompt, still in RAM:

```
setenv serverip 192.168.1.10      # your TFTP server, if different
setenv ipaddr 192.168.1.1
ping $serverip
mtd list                          # u-boot 1 MiB + ubi expected
mtd bad                           # only blocks inside the old BMT reserve (>= 0x7800000) are acceptable
run load_oem_backups && echo BACKUPS OK   # dry run of the verification, nothing written
run format_ubi_part
```

`format_ubi_part` fetches and verifies `xr500v-misc.bin` and `xr500v-bootloader.bin`, erases the `ubi`
partition (skipping the marked blocks), creates `ubootenv`, `ubootenv2`, `misc` (written from the
dump), `tcboot_oem` (written from the dump), then runs `upgrade_uboot`, which fetches the 1 MiB
bootloader image, checks its `6578` magic and writes it over the OEM bootloader. About 40 seconds.
Expected tail of the log on the developer's unit:

```
Creating static volume misc of size 4718592
4718592 bytes written to volume misc
Creating static volume tcboot_oem of size 262144
262144 bytes written to volume tcboot_oem
Bytes transferred = 1048576 (100000 hex)
Erasing 0x00000000 ... 0x000fffff (8 eraseblock(s))
```

Optional read-back before rebooting (compare with `crc32` of the files on your PC):

```
mw.b 0x84000000 0 0x100000; mtd read u-boot 0x84000000 0 0x100000; crc32 0x84000000 0x100000
mw.b 0x84000000 0 0x480000; ubi read 0x84000000 misc 0x480000; crc32 0x84000000 0x480000
```

Now **cut the power** (do not type `reset`: the ROM path stays latched) and power on normally. The
flash bootloader should print, in order, `EN751221 DRAMC v1.2.2`, `calibration status: 0`,
`Econet flash loader`, then the U-Boot banner, `Loading Environment from UBI...` (a bad-CRC warning
is normal until the first `saveenv`), and, since there is no OpenWrt yet, `Install openwrt` followed
by a TFTP boot of `bootfile`.

## Step 3 — install OpenWrt on the new layout

With the initramfs running (LAN at 192.168.1.1, no password), copy the FIT sysupgrade image and
install it. Configuration is **not** carried over from an initramfs (upstream behaviour: the root is
a tmpfs), so either start fresh or pass a backup taken on the old system:

```sh
sysupgrade -T /tmp/openwrt-airoha-en751221-tplink_archer-xr500v-v1-ubootmod-squashfs-sysupgrade.bin
sysupgrade -n  /tmp/openwrt-...-ubootmod-squashfs-sysupgrade.bin         # fresh configuration
# or: sysupgrade -f /tmp/backup-old-router.tar.gz /tmp/openwrt-...-ubootmod-squashfs-sysupgrade.bin
```

`sysupgrade` creates the `fit` and `rootfs_data` volumes and reboots. From then on U-Boot boots the
FIT from UBI on its own (about 105 s to SSH). Later upgrades from the installed system keep the
configuration as usual.

## Step 4 — check

```sh
cat /proc/mtd                     # u-boot, ubi
ubinfo -a | grep -E 'Name|Size'   # ubootenv, ubootenv2, misc, tcboot_oem, fit, rootfs_data
mount | grep -E ' / |overlay'     # /dev/fit0 (squashfs) and ubi0:rootfs_data (ubifs)
fw_printenv bootcmd               # run boot_ubi
cat /sys/class/net/eth0/address   # factory MAC, not a random one
iw dev                            # both radios (EEPROM from the misc volume)
```

Save a first environment so the bad-CRC warning disappears and your TFTP addresses persist:

```sh
fw_setenv serverip 192.168.1.10
```

## If something goes wrong

- **U-Boot prompt but no OpenWrt** (`fit` missing or broken): start a TFTP server with `bootfile`
  and run `run boot_tftp`, then redo step 3. To wipe and start over from the bootloader:
  `run format_ubi_part` again (it needs the two dumps on the TFTP server).
- **No bootloader output at all**: step 1 (RESET at power-on, chainloader + U-Boot in RAM), then
  `run upgrade_uboot` to rewrite the 1 MiB bootloader from TFTP.
- **Return to stock**: possible in principle from the RAM U-Boot with the full dumps and the
  `tcboot_oem` volume, but it has **not been done**; the OEM bad-block table and the OOB layout make it
  more than `mtd write` of the dumps. Treat the migration as permanent.

## Notes

- U-Boot has no LZMA, so the FIT carries the self-decompressing `vmlinuz.bin` uncompressed, and the
  kernel takes the device tree from U-Boot (an appended DTB would hide `chosen/u-boot,version`, which
  `fitblk` needs to map the rootfs).
- The power LED is driven by the bootloader on the OEM path; U-Boot lights it itself, and the new
  device tree defines it as `green:power`.
- Bootloader sources: `airoha/u-boot` pull request 8 on Matheus's Gitea (board, ESMT F50L1G41A
  identification, BootROM chainloader with DDR stage, timebase fix). OpenWrt side: `airoha/openwrt`
  pull request 67 (layout, nvmem cells, FIT recipe, `sysupgrade` path).
