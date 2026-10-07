# Migrating the Archer XR500v v1 to U-Boot + UBI

_Status (October 2026): two ways to migrate. **Without a serial console**, from OpenWrt running on
the OEM bootloader (`xr500v-migrate-uboot`, new in the October 2026 release): tested end to end on
the developer's unit on 7 October 2026, twice, as other users would run it: from an older OEM
release upgraded to this one, and from a fresh install over the stock firmware (OEM image →
migration → U-Boot → OpenWrt with the same configuration, GPON, WiFi and VoIP working). **With a serial
console**, from U-Boot loaded by the BootROM: performed on the same unit on 19 September 2026 and
still the rescue path. Either way it is a **one-way** change for practical purposes. Read
everything before touching the router._

The October 2026 release is the **last one for the OEM bootloader**. Later releases will only ship
the U-Boot layout, so this guide is how a router on the OEM bootloader keeps getting updates.

## What this does, and why you may not want it

The OEM bootloader (`bldr` / Bootbase, in the first 256 KiB of the NAND) is replaced by U-Boot with
the EN751221 DDR stage in front of it, and the remaining 127 MiB become one UBI partition. OpenWrt
then boots as a FIT image from a UBI volume, exactly like any modern NAND target: `sysupgrade`,
`fw_printenv`, a recovery TFTP boot from the bootloader, and the whole flash for you.

What you lose:

- the stock TP-Link firmware slot, the web recovery and the OEM dual-slot scheme. There is no going
  back with a button; see [Return to stock](#return-to-stock);
- the safety of never having written to the bootloader blocks. If the first 1 MiB ends up wrong the
  board only talks through its BootROM over UART.

What you keep: everything in the factory `misc` area (LAN/WAN MAC, EN7570 optical calibration, MT7662
EEPROM) is copied into a UBI volume and the kernel reads it from there through nvmem. The OEM
bootloader itself is kept in another volume. GPON works on the new layout with the factory
calibration.

Which units: **only units whose UART log at power-on shows both**
`EN751221 at Mon Aug 16 ... 2021 version 1.1 free bootbase` and `bmt pool size: 94`. The pool size
depends on the unit's NAND, not on the bootloader ([chapter 3](03-boot-partitions-flashing.md)).
Units with the December 2019 bootloader (`... Tue Dec 3 ... 2019 ...`) are **not covered**. OpenWrt's
OEM image refuses to use the flash of any other unit (its bad-block driver checks the 94-block pool
and stops), so on such a unit it does not boot at all: do not flash it there.

## Files

From the release assets (check `SHA256SUMS`):

| File | Purpose |
|---|---|
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-tcboot.bin` (1 MiB) | `tcboot`: DDR stage + U-Boot, the image written to the first MiB |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-squashfs-sysupgrade.bin` | OpenWrt FIT for the U-Boot layout (`sysupgrade` there) |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-initramfs-kernel.bin` | OpenWrt initramfs FIT, booted over TFTP by U-Boot (recovery) |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-oem-squashfs-sysupgrade.bin` | OpenWrt for the OEM bootloader (slot B + `openwrt_ubi`), carries `xr500v-migrate-uboot` |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-oem-stock-webflash.bin` (19 MiB) | the same, in the layout the stock web interface accepts |
| `u-boot-xr500v-ram.bin` (~720 KiB) | plain U-Boot, loaded into RAM through `bootext.bin` (UART method and rescue) |
| `openwrt-airoha-en751221-tplink_archer-xr500v-v1-bootext.bin` (33 KiB) | BootROM rescue: calibrates the DRAM like the flash bootloader, then receives U-Boot by XMODEM |

## Which path

| The router runs | Do this |
|---|---|
| the stock TP-Link firmware | flash `...-oem-stock-webflash.bin` from the stock web interface (firmware upgrade page), then follow the next row |
| OpenWrt on the OEM bootloader, an older release | `sysupgrade` to `...-oem-squashfs-sysupgrade.bin` of this release (keeps the configuration), then the next row |
| OpenWrt on the OEM bootloader, this release | [Migrating without a serial console](#migrating-without-a-serial-console) |
| OpenWrt on U-Boot already (September release, U-Boot lane) | `sysupgrade` to `...-squashfs-sysupgrade.bin`. The bootloader stays as it is; [updating it](#updating-u-boot) is optional |

After a web flash from stock, OpenWrt starts with the defaults (LAN `192.168.1.1/24` with DHCP, WiFi
off) and its overlay in RAM: the `openwrt_ubi` area is prepared by the first `sysupgrade` of an OEM
image (`sysupgrade -n` with the same file), or by the migration, which does not need it and keeps
what you configured meanwhile. For this release the web flash image was written to slot B from the
stock firmware's shell with its own `mtd` tool, which is what the web page does after its check,
and the OEM bootloader accepted it (it checks an MD5 over the whole slot) and booted it; the stock
web page itself was not exercised again. Its check uses the same salted MD5 as the June and
August web flash images, which it accepted.

## Layouts

OEM layout (what OpenWrt on the OEM bootloader shows in `/proc/mtd`):

| MTD | Offset | Size | Content |
|---|---|---|---|
| bootloader | 0x0 | 256 KiB | Bootbase / `bldr` |
| romfile, kernel, rootfs_stock | 0x40000 | 19.25 MiB | stock TP-Link firmware (slot A) |
| **misc** | 0x1380000 | 4.5 MiB | **MAC @0xf100, EN7570 calibration @0x20000, MT7662 EEPROM @0xe0000** |
| kernel1, rootfs1 | 0x1800000 | 19 MiB | OpenWrt slot B |
| others, bootflag | 0x2b00000 | ~5 MiB | OEM data, boot slot flag |
| openwrt_ubi | 0x3000000 | 64 MiB | OpenWrt overlay |
| migrate_uboot, migrate_ubi | 0x0, 0x100000 | 1 MiB, ~115 MiB | only for `xr500v-migrate-uboot`: the future `u-boot` and `ubi`, over everything above |
| (reserve) | 0x7440000 | 11.75 MiB | OEM bad-block management (BMT) reserve, not an MTD |

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

## Migrating without a serial console

From OpenWrt of this release on the OEM bootloader, with SSH access. Copy the two U-Boot files to
`/tmp` on the router (`scp -O`; `/tmp` needs about 40 MiB free), then:

```sh
cd /tmp
xr500v-migrate-uboot check openwrt-airoha-en751221-tplink_archer-xr500v-v1-tcboot.bin \
	openwrt-airoha-en751221-tplink_archer-xr500v-v1-squashfs-sysupgrade.bin
```

`check` writes nothing to the flash. It verifies:

- the board, and that the flash is the OEM layout of this release (with `migrate_uboot` and
  `migrate_ubi`);
- the OEM bad block table as the kernel found it at boot: no factory bad or remapped block among the
  first eight, which the BootROM reads (the table summary is kept in `/tmp/xr500v-bmt.log`);
- `tcboot.bin`: 1 MiB, the EN751221 boot header, U-Boot with this board's device tree;
- the sysupgrade image: a FIT with OpenWrt metadata for `tplink,archer-xr500v-v1`;
- the factory data it is about to keep: the MAC address, the EN7570 calibration magic and the MT7662
  EEPROM in `misc`, the TrendChip boot code in the OEM bootloader.

It prints the sha256 of both files (compare them with `SHA256SUMS`) and leaves the factory data in
`/tmp/xr500v-migrate/`. **Copy it off the router now**, from your computer, and keep it:

```sh
scp -O root@192.168.1.1:/tmp/xr500v-migrate/misc.bin root@192.168.1.1:/tmp/xr500v-migrate/oem-bootloader.bin .
```

Then migrate (`-n` to start with a fresh configuration instead of the current one):

```sh
xr500v-migrate-uboot run openwrt-airoha-en751221-tplink_archer-xr500v-v1-tcboot.bin \
	openwrt-airoha-en751221-tplink_archer-xr500v-v1-squashfs-sysupgrade.bin
```

It repeats the checks, asks you to type `migrate`, and runs `sysupgrade` on the U-Boot image, which
the OEM image only accepts once `xr500v-migrate-uboot` has staged it. Your SSH session closes as with
any `sysupgrade`. About a minute later the router reboots into U-Boot, and U-Boot into OpenWrt.
**Do not power it off meanwhile.** In order:

1. U-Boot is written to the first MiB and read back. If it cannot be verified, the original first
   MiB (OEM bootloader, romfile, start of slot A) is written back, nothing else is touched, and the
   router reboots into the OEM bootloader and the same OpenWrt.
2. From here on, U-Boot boots whatever happens next. The rest of the flash becomes one UBI device:
   first `misc` and `tcboot_oem` with the data checked above, then `ubootenv` and `ubootenv2`
   (empty, with the volume IDs U-Boot's own `format_ubi_part` gives them), then `fit` and
   `rootfs_data` as a U-Boot `sysupgrade` leaves them, with the configuration.
3. Every volume is read back.

On its first boot U-Boot saves its environment to both `ubootenv` volumes; from then on `fw_setenv`
is safe. Then check the result as in [Check](#check).

If the router does not come back with OpenWrt but U-Boot is there (it lights the power LED and
answers no ping), U-Boot is trying its TFTP recovery: see [If something goes wrong](#if-something-goes-wrong).

## Migrating with a serial console

The September method: U-Boot is loaded into RAM through the BootROM, and creates the volumes itself
from dumps you fetch over TFTP. It needs:

- a **3.3 V UART** adapter (115200 8N1) soldered to the four unlabelled pads in a column between the
  SoC and the green GPON connector, and a way to send **1K-XMODEM with CRC** (the script below, or
  `sx -k` / TeraTerm / any terminal that does it cleanly);
- a **TFTP server** reachable from a LAN port (defaults below assume the server is `192.168.1.10`
  and the router `192.168.1.1`; change them with `setenv` if you must);
- OpenWrt on the OEM layout with SSH access, to take the dumps;
- one to two hours, a power switch within reach, and no fibre service you need during that time.

### Step 0 — back up the whole NAND (from the running OpenWrt, OEM layout)

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

Also confirm the unit is an August-2021-bootloader / BMT94 one with no factory bad blocks in the user area:

```sh
dmesg | grep -i -E 'bmt|reserve'      # expect reserve 94, factory_bad: 0
```

U-Boot's default environment fetches the bootloader as `bootloaderfile` and the initramfs as
`bootfile`. Put copies in the TFTP root under those names:

```sh
cp openwrt-airoha-en751221-tplink_archer-xr500v-v1-tcboot.bin \
	/srv/tftp/openwrt-airoha-en751221-tplink_archer-xr500v-v1-uboot-bootloader.bin
cp openwrt-airoha-en751221-tplink_archer-xr500v-v1-initramfs-kernel.bin /srv/tftp/
```

### Step 1 — prove the rescue path before you need it

Hold **RESET** while powering on. The BootROM (mask ROM, cannot be broken by software) prints its
banner, goes quiet for about 20 seconds, then prints `done` and repeats. The recovery is entered by
typing **`x` during the silence before a `done`**; after the next `done` the ROM sends `C` and waits
for a 1K-XMODEM/CRC transfer of `bootext.bin`. That calibrates the DRAM exactly as the flash
bootloader would (the ROM's own setup is not good enough for U-Boot's Ethernet) and shows a menu:
**`x` loads U-Boot into RAM; never press `b`, which writes the flash.** Then it receives `u-boot.bin`.

The script does the whole dance, stops at the U-Boot prompt and logs the UART:

```sh
python3 scripts/bootext-rescue.py /dev/ttyUSB0 \
	openwrt-airoha-en751221-tplink_archer-xr500v-v1-bootext.bin u-boot-xr500v-ram.bin rescue.log
```

Tested with this release's files on the developer's unit on 7 October 2026. The BootROM now and then
throws `Undefined Exception` right after a transfer, whatever the file; the script waits for the
next cycle and sends again. Other pitfalls: any stray byte after the first `C` makes the ROM fall
back to the checksum protocol and abort; a bare Enter counts as a key; once in the ROM loop neither
`reset` nor the watchdog leave it, **only a power cut** (a system started through the rescue also
falls back into it on a warm reboot); an empty Enter at the U-Boot prompt repeats the last
command.

You should reach `U-Boot>` with `DRAM: 256 MiB`, the ESMT SPI NAND detected and working Ethernet
(`ping $serverip`). Nothing has been written. If this step does not work, stop here: it is your only
way back after the next step.

### Step 2 — migrate (from the U-Boot loaded in RAM in step 1)

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

`format_ubi_part` fetches and verifies `xr500v-misc.bin` and `xr500v-bootloader.bin`, detaches UBI,
erases the `ubi` partition (skipping the marked blocks), creates `ubootenv`, `ubootenv2`, `misc`
(written from the dump), `tcboot_oem` (written from the dump), then runs `upgrade_uboot`, which
fetches the 1 MiB bootloader image, checks its `6578` magic and writes it over the OEM bootloader.
About 40 seconds. Expected tail of the log:

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
is normal here: the environment volumes are still empty), `Writing to UBI... done` and
`Writing to redundant UBI... done` (U-Boot saves its environment on this first boot), and, since
there is no OpenWrt yet, `Install openwrt` followed by a TFTP boot of `bootfile`.

### Step 3 — install OpenWrt on the new layout

With the initramfs running (LAN at 192.168.1.1, no password), copy the FIT sysupgrade image and
install it. Configuration is **not** carried over from an initramfs (upstream behaviour: the root is
a tmpfs), so either start fresh or pass a backup taken on the old system:

```sh
sysupgrade -T /tmp/openwrt-airoha-en751221-tplink_archer-xr500v-v1-squashfs-sysupgrade.bin
sysupgrade -n  /tmp/openwrt-airoha-en751221-tplink_archer-xr500v-v1-squashfs-sysupgrade.bin   # fresh configuration
# or: sysupgrade -f /tmp/backup-old-router.tar.gz /tmp/openwrt-...-squashfs-sysupgrade.bin
```

`sysupgrade` creates the `fit` and `rootfs_data` volumes and reboots. From then on U-Boot boots the
FIT from UBI on its own. Later upgrades from the installed system keep the configuration as usual.

## Check

```sh
cat /proc/mtd                     # u-boot, ubi
ubinfo -a | grep -E 'Name|Size'   # ubootenv, ubootenv2, misc, tcboot_oem, fit, rootfs_data
mount | grep -E ' / |overlay'     # /dev/fit0 (squashfs) and ubi0:rootfs_data (ubifs)
fw_printenv >/dev/null            # must not warn about a bad CRC (see below)
fw_printenv bootcmd               # ends in "run boot_ubi"
cat /sys/class/net/eth0/address   # factory MAC, not a random one
iw dev                            # both radios (EEPROM from the misc volume)
```

If `fw_printenv` warns `Bad CRC, using default environment`, the environment has never been saved
(only bootloaders of the September release, which did not save it by themselves). **Do not run
`fw_setenv` then**: with no valid copy it starts from its own generic default environment
(`bootcmd=run distro_bootcmd`, `loadaddr=0x0`, a made-up MAC address), stores that, and on the next
boot U-Boot stops at its prompt instead of booting OpenWrt. Save U-Boot's own environment from the
UART instead: reboot, press a key at `Hit any key to stop autoboot`, then `saveenv`, `saveenv` (the
first writes `ubootenv`, the second `ubootenv2`) and power-cycle the router. Once `fw_printenv` no
longer warns about a bad CRC, `fw_setenv` is safe, for example to keep your TFTP server address:

```sh
fw_setenv serverip 192.168.1.10
```

## Updating U-Boot

A router migrated with the September release keeps that bootloader across `sysupgrade`. To install
this release's U-Boot from U-Boot itself (UART): put `tcboot.bin` in the TFTP root under the name in
`bootloaderfile` (see step 0) and choose "Upgrade u-boot" in the boot menu, or `run upgrade_uboot`.
It checks the `6578` magic before erasing.

## If something goes wrong

- **No OpenWrt, U-Boot present, no UART** (power LED on, no ping): U-Boot found no `fit` volume and
  is fetching `openwrt-airoha-en751221-tplink_archer-xr500v-v1-initramfs-kernel.bin` by TFTP from
  `192.168.1.10`, as `192.168.1.1`. Give a computer on a LAN port the address `192.168.1.10/24`, serve
  that file over TFTP and power-cycle the router: the initramfs comes up at 192.168.1.1. If
  `ubinfo -a` there still shows the `misc` volume, `sysupgrade -n` the U-Boot image and you are done.
  If it does not (the migration stopped before writing it), recreate the volumes from the files you
  copied off the router (put them in `/tmp`), then `sysupgrade -n`. Both were tested on 7 October
  2026: with the `fit` volume removed, U-Boot fell back to TFTP by itself, and these commands, run
  from that initramfs, rebuilt the volumes before `sysupgrade -n` (the TFTP addresses came from a
  saved environment there; the `192.168.1.x` defaults apply when none was saved):

  ```sh
  ubidetach -p /dev/mtd1 2>/dev/null; ubiformat /dev/mtd1 -y && ubiattach -p /dev/mtd1
  ubimkvol /dev/ubi0 -n 2 -N misc -s 4718592 -t static && ubiupdatevol /dev/ubi0_2 /tmp/misc.bin
  ubimkvol /dev/ubi0 -n 3 -N tcboot_oem -s 262144 -t static && ubiupdatevol /dev/ubi0_3 /tmp/oem-bootloader.bin
  ubimkvol /dev/ubi0 -n 0 -N ubootenv -s 126976 -t static
  ubimkvol /dev/ubi0 -n 1 -N ubootenv2 -s 126976 -t static
  ```

- **U-Boot prompt but no OpenWrt** (with a UART): start a TFTP server with `bootfile` and run
  `run boot_tftp`, then do step 3. To wipe and start over from the bootloader: `run format_ubi_part`
  (it needs the two dumps on the TFTP server).
- **U-Boot stops at `## Error: "distro_bootcmd" not defined`**: `fw_setenv` was run while the
  environment had never been saved, and stored its generic default environment (see
  [Check](#check)). From the UART: `env default -a`, `saveenv`, `saveenv`, then power-cycle. OpenWrt
  and its configuration are untouched.
- **No bootloader output at all**: step 1 (RESET at power-on, `bootext.bin` + U-Boot in RAM), then
  `run upgrade_uboot` to rewrite the 1 MiB bootloader from TFTP.

## Return to stock

Done once, on the developer's unit, on 7 October 2026: from U-Boot over UART, the whole NAND was
erased (skipping its bad blocks) and the OEM layout written back, after which the OEM bootloader
printed `BMT & BBT Init Success` and booted the previous OpenWrt from slot B. It needed more than
the partition dumps of step 0:

- the first MiB and the OEM bad-block tables (the BBT block and the BMT copies at the end of the
  reserve) written **raw, with their OOB bytes**, from dumps taken in U-Boot with `mtd read.raw.oob`
  before migrating. Without them the OEM bootloader finds no table;
- every other OEM partition from the Linux dumps of step 0, with ECC (`mtd write`).

The migration without a serial console cannot take the raw dumps (the reserve is hidden from Linux
by the OEM bad-block driver), so after it a return to stock is **not covered**. Treat the migration as
permanent.

## Notes

- U-Boot has no LZMA, so the FIT carries the self-decompressing `vmlinuz.bin` uncompressed, and the
  kernel takes the device tree from U-Boot (an appended DTB would hide `chosen/u-boot,version`, which
  `fitblk` needs to map the rootfs).
- The OEM bootloader LZMA-decompresses at most 0x2ffe00 bytes from slot B, so the OEM image carries
  the same self-decompressing kernel and its device tree inside the bootloader's LZMA.
- The migration writes the flash through the OEM bad-block driver. UBI finds its blocks wherever
  they are, so blocks the table moved do not matter; only the first MiB has to be where the BootROM
  reads it, which `check` makes sure of.
- The power LED is driven by the bootloader on the OEM path; U-Boot lights it itself, and the new
  device tree defines it as `green:power`.
- Bootloader sources: `airoha/u-boot` on the airoha Gitea (board, ESMT F50L1G41A identification,
  timebase fix, UBI detach before `format_ubi_part`). `bootext.bin` (DRAM calibration and the XMODEM
  receiver) comes from `airoha/airoha_mips_dramc`, built with the release.
