# XR500v OpenWrt main initramfs RAM hardware test

Date: 2026-07-31

## Outcome

The minimal TP-Link Archer XR500v v1 DTS/profile RFC boots current OpenWrt
main on real EN751221 hardware.  The exact board-specific initramfs reached a
working OpenWrt shell, and the single flat Ethernet path carried bidirectional
traffic with matching payload hashes.

This changes the RFC status from compile-tested to hardware-tested for the
minimal RAM-boot scope.  It does **not** validate persistent installation,
the complete dual-switch/DSA topology, all four LAN ports, GPON, or the other
omitted peripherals.

## Exact source and artifacts

OpenWrt worktree:

`/home/cristuu/tools/xr500v/openwrt-xr500v-upstream`

- OpenWrt base: `c9833b993d`
- XR500v RFC: `42ff4e7b1a274a726026dd12a78303bc479baf8f`
- branch: `econet-xr500v-v1-rfc`
- kernel: Linux `6.18.39`

Raw OpenWrt initramfs kernel:

`bin/targets/econet/en751221/openwrt-econet-en751221-tplink_archer-xr500v-v1-initramfs-kernel.bin`

- size: `6405452` bytes (`0x61bd4c`)
- SHA-256:
  `30548ff8eac12449a802a70c61b6d6af9bc393512fce8b74c1f479fe4a358566`
- raw MIPS vmlinuz with appended DTB
- linked load/entry address: `0x80020000`

Legacy uImage wrapper used for checked U-Boot loading:

`/mnt/c/tftp/xr500v-rfc-initramfs.uImage`

- size: `6405516` bytes (`0x61bd8c`)
- SHA-256:
  `6b34a82ed66072bb87ebe41f8da7c37e6c1d390ec5b4a3e9bcd4f44e1e85e723`
- image name: `OpenWrt-XR500v-initramfs`
- load/entry address: `0x80020000`
- `iminfo`: header recognized and data checksum `OK`

RAM-only U-Boot source:

- source commit: `31b0d565683013ab30245128f205a21afb03b82b`
- temporary netboot variant enabled only `CONFIG_CMD_TFTPBOOT`
- XMODEM size: `245376` bytes (`0x3be80`)
- XMODEM SHA-256:
  `76407bb934c321d0ca74335c9c50790ffc4fbd22b6200532807a5450ff5d7e81`
- MTD, SPI, SPL/TPL, `saveenv`, BOOTP, DHCP, NFS, SNTP, and WGET remained
  disabled in the RAM-only target

## RAM-only boot path

The router started from a cold proprietary Bootbase/BLDR boot.  The netboot
U-Boot was transferred with 128-byte XMODEM into RAM and checked before entry:

```text
bldr> xmdm a1000000 3be80
received len=3be80
bldr> memrl a1000000
A1000000: 1000013F
bldr> memrl a1000004
A1000004: 0
bldr> jump 81000000
```

U-Boot identified the EN751221, 255 MiB usable RAM, and `airoha-gdm1`.
Its temporary network configuration reached the host at `192.168.68.248`.
The checked uImage was then loaded into a non-overlapping container address:

```text
=> setenv ipaddr 192.168.68.221
=> setenv serverip 192.168.68.248
=> setenv netmask 255.255.255.0
=> setenv netretry no
=> setenv ethrotate no
=> ping 192.168.68.248
host 192.168.68.248 is alive
=> tftpboot 83000000 xr500v-rfc-initramfs.uImage
Bytes transferred = 6405516 (61bd8c hex)
=> iminfo 83000000
Verifying Checksum ... OK
=> bootm 83000000
```

`bootm` copied the kernel to `0x80020000`; the vmlinuz decompressed Linux to
`0x81000000` and copied the appended device tree to `0x823b0780`.

No `bflag`, NAND/MTD write, or persistent environment command was used.

## Hardware results

### Core boot and userspace: pass

- machine: `TP-Link Archer XR500v v1`
- board name: `tplink,archer-xr500v-v1`
- system: `EcoNet-EN75xx`
- RAM: `262144 KiB`, with `237212 KiB` available after kernel reservations
- root filesystem: embedded `initramfs`/tmpfs
- UART: `ttyS0` at `0x1fbf0000`, IRQ 9
- OpenWrt: `SNAPSHOT r0+35621-c9833b993d`
- procd, ubus, and the root console completed normally
- no panic, oops, call trace, or driver timeout appeared

The kernel is SMP-capable but this DTS run enumerated one CPU only.  A second
VPE/core is not validated by this result.

### SPI NAND and partition geometry: read-only pass, persistent fail

Linux detected the GigaDevice/F50L1G SPI NAND as 128 MiB with 128 KiB erase
blocks, 2048-byte pages, and 64-byte OOB.  The expected stock partition map
was created:

```text
bootloader  0x0000000-0x0040000  read-only
romfile     0x0040000-0x0080000  read-only
kernel      0x0080000-0x0380000  read-only
rootfs_stock 0x0380000-0x1380000 read-only
misc        0x1380000-0x1800000  read-only
kernel1     0x1800000-0x1b00000
rootfs1     0x1b00000-0x2b00000
others      0x2b00000-0x2fe0000  read-only
bootflag    0x2fe0000-0x3000000  read-only
```

The serious remaining blocker is:

```text
en75_bmt: BBT not found and econet,can-write-factory-bbt is unset, giving up
```

Bootbase finds its own BMT/BBT, while current Linux skips a group of reserve
blocks and cannot find a compatible table.  Therefore this run proves NAND
detection and read-only NVMEM access only.  It does not authorize OpenWrt
MTD writes, sysupgrade, BBT creation, or a persistent image.

Although MTD marked `rootfs1` as the candidate root and found a
`rootfs_data` split, `ubus call system board` reported `rootfs_type` as
`initramfs` and `df` showed tmpfs at `/`.  The stock rootfs was not mounted.

### PCIe and MT7662: probe pass, RF untested

- PCIe1 trained and enumerated endpoint `14c3:7662`.
- BAR assignment completed.
- `mt76x2e` loaded the ROM patch and firmware.
- the driver reported `Firmware running!` and created `phy0`.
- `phy0` advertised managed, AP, AP/VLAN, monitor, mesh, and P2P modes.

No virtual interface was created and no scan, AP, association, RF, throughput,
or calibration test was performed.  This is a probe result only.

### USB: host probe pass, device traffic untested

The MediaTek xHCI controller registered USB 2 and USB 3 root hubs.  No USB
peripheral was attached, so storage and sustained USB traffic remain untested.

### Linux Ethernet: pass for one inherited flat path

`econet_eth` registered one LAN netdevice with the factory MAC obtained from
NVMEM.  The value is redacted from the public log.  `eth0` and `br-lan` were
UP/LOWER_UP and `/sys/class/net/eth0/carrier` returned `1`.

After adding the temporary RAM-only address `192.168.68.222/24` to `br-lan`:

- router to host: 8/8 ICMP replies, 0% packet loss;
- host to router: 8/8 ICMP replies, 0% packet loss;
- Dropbear SSH returned `ubus call system board` successfully;
- a 6405516-byte uImage copied host to router matched SHA-256;
- reading the same payload router to host matched SHA-256 again;
- a second full RX transfer increased `rx_bytes` without increasing
  `rx_dropped`;
- `rx_errors=0`, `tx_errors=0`, and `tx_dropped=0` after the sustained checks;
- the 42 accumulated RX drops remained unchanged during the controlled second
  transfer.

The SSH timings are not treated as throughput measurements.  This test used
one physical cable/path after the RAM U-Boot Ethernet driver had initialized
the hardware for TFTP.  It does not prove independent cold Linux switch
initialization, all four user ports, switching between LAN ports, VLANs, or
the intended dual-MT7530 DSA topology.

## Expected warnings and non-results

- `econet_eth` is still an out-of-tree module and taints the kernel.
- `eth0` reports `operstate=unknown` despite carrier and verified traffic.
- U-Boot generated a random temporary Ethernet MAC and rejected overwriting
  it after registration; Linux later obtained the correct factory MAC.
- `Initrd not found or empty` is expected because the initramfs is embedded in
  the kernel rather than supplied as a separate initrd.
- the PCIe dependency cycle and invalid initial bridge window were recovered
  by the PCI core before the MT7662 initialized.
- `Firmware Version: 0.0.00` was followed by `Firmware running!`; firmware
  execution, not RF operation, is the supported conclusion.
- GPON/EN7570, MT7603, FXS/PCM, LEDs, buttons, DSA, and persistent boot were
  omitted from this minimal RFC and were not tested.

The fibre link and private ISP identity were not used, copied, programmed, or
published by this test.

## Evidence

Original local UART capture:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/build/xr500v-openwrt-rfc-ramboot-reseat-20260731.log`

- size at capture close: `31487` bytes
- lines: `568`
- SHA-256:
  `4568b7a633724289bc20183f0c4de982335f5bceca7c4b3ca1e4b5eb7b385c8c`

Public sanitized copy:

`docs/boot-logs/2026-07-31-openwrt-master-xr500v-initramfs-ram.log`

The public copy normalizes CRLF and redacts the random U-Boot MAC, the factory
MAC, and the local build user/host.  It contains the complete BLDR, U-Boot,
kernel, userspace, device inspection, and UART-side ping transcript.

The host-side SSH checks were captured separately in the live test record and
summarized above; they do not appear on UART.

## Current boundary and next work

The minimal DTS/profile can now be described as hardware-tested for RAM boot,
console, read-only board/NVMEM identification, MT7662/USB probe, and one flat
Ethernet path.

Before any persistent installation claim:

1. resolve or explicitly model the Bootbase BMT/BBT format without creating a
   new factory BBT on this device;
2. implement and validate the BLDR-compatible TrendChip header, 3 MiB kernel
   constraint, and 512-byte rootfs boundary;
3. cold-test Linux Ethernet without relying on U-Boot network initialization;
4. integrate and validate the dual-switch DSA topology and all four LAN ports;
5. separately add/test MT7603, LEDs/buttons, xPON, and FXS where appropriate.

Until then, the safe recovery path remains a physical power cycle back to the
installed Bootbase-selected firmware.
