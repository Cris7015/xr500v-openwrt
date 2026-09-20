# TP-Link Archer XR500v — Teardown & OpenWrt Port

> **Reference documentation, refreshed 20 September 2026.** Chapters 02–11 were
> written during the 2026 bring-up and keep the reverse-engineering detail that
> made the port possible; each now opens with a *Where it stands now* box that
> says what changed since it was written. Chapter 12 (U-Boot + UBI migration) is
> current. For the published images and their exact scope see the
> [project overview](../README.md) and the
> [releases](https://github.com/Cris7015/xr500v-openwrt/releases).

This is, to our knowledge, the first public technical documentation of the **TP-Link
Archer XR500v** (v1) — a GPON home gateway sold in Latin America. TP-Link publishes
only marketing specs for this device and states that it cannot run OpenWrt; this document
shows otherwise. Today (September 2026) the XR500v runs OpenWrt with Linux 6.18 as a
complete GPON gateway: fibre WAN with hardware NAT, four gigabit ports, dual-band Wi-Fi,
USB, the full 256 MB of RAM and both RJ11 telephone ports, on either the OEM bootloader
or a U-Boot + UBI layout. See [GPON / xPON Status](09-gpon-xpon-status.md) for how the
optical WAN went from "not ported" to working.

Everything here documents the owner's own device. The hardware identification, register
maps, and firmware details were obtained by reverse-engineering: NAND dumps, OEM source
review, live register reads on the running device, and many build/flash iterations. Where
a fact was only partially verified, it is marked as such rather than overstated.

> **The XR500v is not TP-Link silicon.** It is an OEM TrendChip/TCLinux HGW (Home Gateway)
> rebadged by TP-Link. The stock romfile still carries `Vendor="TC"`, the SoC is an
> EcoNet/Airoha **EN751221** (MIPS, big-endian), and the default/undocumented operator
> service accounts (`telecomadmin` / `useradmin`) remain in the romfile under the TP-Link
> login skin. See
> [Stock firmware access & security notes](10-stock-firmware-access.md).

---

## Status legend / last-verified

These pages were drafted from point-in-time reverse-engineering notes of differing ages,
so individual claims reflect the state at the time each was written. The overall status
table below is current as of **2026-09-20** (release `v2026.09.20-r21`, Linux 6.18.41,
built from the Matheus `airoha_en7523` tree); the per-chapter *Where it stands now* boxes
carry the same date.
"Working" means observed on the running device; facts that were only partially verified are
labelled as such inline. Throughput figures are noted with the test configuration they were
measured in, because the device's role and the measurement path materially change the number.

---

## What works under OpenWrt

| Subsystem | Status | Page | Notes |
|---|---|---|---|
| Boot / flash / sysupgrade | Working | 03, 12 | OEM bootloader layout (slot B + 64 MiB UBI overlay, stock kept) **or** U-Boot + UBI (FIT from a UBI volume, whole flash, one-way migration) |
| CPU | Working | 07 | Both MIPS 34Kc VPEs (SMP), HZ=1000, hardware watchdog and lockup detectors |
| Ethernet — 4× GbE LAN | Working | 04 | Dual cascaded MT7530 DSA over the `airoha_eth` gen1 driver; TRGMII tap fixed like the factory firmware; conduit TX-stall fix |
| **HW-NAT — PPE flow offload** | Working | 04 | nftables flowtable offload; **LAN↔LAN ~929 Mbit/s**, fibre PPPoE ~668/664 Mbit/s, CPU idle |
| Wi-Fi HW forwarding (WHNAT) | Working (opt-in) | 05 | PPE NATs in hardware, CPU re-injects to the radio; disabled unless `xr500v-whnat.main.enabled=1` |
| Wi-Fi 5 GHz | Working | 05 | MT7662 / `mt76x2e`, EEPROM and MAC from the factory `misc` area via nvmem |
| Wi-Fi 2.4 GHz | Working | 05 | MT7603 / `mt7603e`, `eeprom-data` + OTP merge, PCIe port0 quirks as a kernel patch |
| USB | Working | 07 | xHCI; USB2 mass storage; the USB3 port has no wired T-PHY |
| 256 MB RAM | Working | 07 | ~244 MB usable |
| **GPON / xPON optical WAN** | **Working** | 09 | O5, OMCI (LuCI page), PPPoE on a Movistar Argentina OLT; APD bias root cause fixed; 15 h with 0 BIP errors; other OLTs untested by us; not upstream |
| Telephony — 2× FXS (RJ11) | Working | 06 | Le9642 SLIC, PCM engine served by IRQ, shared ring group, ring trip, clean hang-up; provider (IMS) and local SIP calls; no in-call DTMF / caller ID / call waiting yet |
| Front-panel LEDs and buttons | Working | 08 | All 10 LEDs (two driven by the radio drivers); power LED lit by U-Boot on the new layout |
| Upstream status | Partial | 02, 11 | Linux mainline: platform, INTC, timer, clocks, PCIe. OpenWrt `econet` target: SoC only, no XR500v. XR500v support, xPON, PPE-for-MIPS and DSA live in the Matheus `airoha_en7523` tree |

> **Throughput note.** The figures are observations with the configuration they were
> measured in, not guaranteed performance. Wire-speed forwarding needs the PPE offload
> (on by default for wired flows since r13); the software path on the 34Kc is a few
> hundred Mbit/s and CPU-bound. The 668/664 Mbit/s fibre figure is a PPPoE speedtest over
> GPON with hardware offload (r13); the 929 Mbit/s figure is routed LAN↔LAN iperf3.

---

## Hardware specifications

| Component | Detail |
|---|---|
| **SoC** | EcoNet / Airoha **EN751221** (en7521 / en7528 family), MIPS **34Kc**, big-endian |
| **CPU clock** | 900 MHz (CP0 count 450 MHz, measured while fixing the U-Boot timebase; earlier notes said ~600 MHz) |
| **RAM** | 256 MB DDR3-1066 (clock as reported by the bootloader; the DRAM part marking was not read) — ~244 MB usable under OpenWrt; `memory@0 reg = <0x0 0x10000000>` |
| **Flash** | SPI-NAND 128 MiB (ESMT F50L1G41A), 128 KiB erase blocks; ~112 MiB usable under the OEM BMT layout, 127 MiB UBI on the U-Boot layout |
| **Boot layout** | OEM: dual A/B slots (A = stock, B = OpenWrt, selected by `bflag`) plus a 64 MiB UBI overlay; or U-Boot + one UBI partition (chapter 12) |
| **Ethernet switch** | Dual cascaded MT7530-class: on-die @ `0x1fb58000` (MMIO) + external MCM @ MDIO `0x1f`; 4× GbE LAN |
| **2.4 GHz Wi-Fi** | MediaTek **MT7603** (`14c3:7603`), 2T2R 11b/g/n; PCIe domain 0, port0 @ `0x1fb81000`; `mt7603e` |
| **5 GHz Wi-Fi** | MediaTek **MT7662 / MT76x2** (`14c3:7662`), 2T2R 11a/n/ac; PCIe domain 1, port1 @ `0x1fb83000`; `mt76x2e` |
| **FXS / telephony** | Microsemi/Microchip **Le9642** (VE886/VP886 family) dual SLIC, over ZSI; on-die PCM engine @ `0x1fbd0000` |
| **WAN** | **GPON ONU** on optical fibre: on-die xPON MAC + EN7570 optics, working under OpenWrt since September 2026 (chapter 09) |
| **USB** | MediaTek xHCI @ `0x1fb90000` (USB2 active; USB3 has no wired T-PHY) |
| **GPIO / LED block** | TrendChip **TC3162** controller @ `0x1fbf0200`, 64 GPIOs; drives 8 of the 10 panel LEDs (the 2 Wi-Fi LEDs are inside the radio chips) |
| **OEM identity** | TrendChip TCLinux HGW (`Vendor="TC"`, `ProductName="HGW"`), rebadged by TP-Link |
| **Stock firmware** | TCLinux Fw 7.1.2.7 / FWVer 3.10.0.24, Linux 3.18.21, plaintext XML romfile |

---

## Documentation pages

| # | Page | Covers |
|---|---|---|
| 01 | **Home** (this page) | Overview, status, specs, methodology |
| 02 | [Hardware & Chip Inventory](02-hardware-chip-inventory.md) | Full chip list, bus map, physical addresses |
| 03 | [Boot, Partitions & Flashing](03-boot-partitions-flashing.md) | OEM bootloader layout: A/B slots, `bflag`, NAND layout, safe flashing |
| 04 | [Ethernet & the DSA Switch](04-ethernet-dsa.md) | Nested dual-MT7530 DSA tree, port mapping, tagger, throughput |
| 05 | [Wi-Fi: MT7603 + MT7662](05-wifi-mt7603-mt7662.md) | Two PCIe radios, EEPROM/MAC sourcing, enumeration walls |
| 06 | [VoIP / FXS Telephony](06-voip-fxs-telephony.md) | Le9642 SLIC over ZSI, PCM/TDM, reconstructed driver, SIP stack |
| 07 | [USB, RAM & Other Peripherals](07-usb-ram-peripherals.md) | xHCI, 256 MB unlock, GPIO controller, buttons |
| 08 | [Front-Panel LEDs](08-front-panel-leds.md) | GPIO map, pad-enable quirk, the Wi-Fi LED reverse-engineering (all 10 working) |
| 09 | [GPON / xPON Status](09-gpon-xpon-status.md) | Where the optical WAN stands (working since September 2026) and the bring-up history |
| 10 | [Stock Firmware Access & Security Notes](10-stock-firmware-access.md) | Restricted CLI, root injection, accounts, firmware verification |
| 11 | [OpenWrt Port, Build & Persistence](11-openwrt-port-build-persistence.md) | Developer guide: the current recipe-based builds and the earlier overlay |
| 12 | [U-Boot + UBI migration](12-uboot-ubi-migration.md) | Advanced, one-way: replace the OEM bootloader with U-Boot, UBI layout, BootROM rescue, FIT sysupgrade |

---

## How this was made (RE methodology)

The port was built by reverse-engineering the owner's device, not from any vendor SDK or
NDA material. The work followed roughly this path:

- **Firmware & config recovery** — NAND partitions dumped over the stock OEM access path
  (an unauthenticated root telnet on port 2323 left up by the dev firmware, plus a
  command-injection on the restricted CLI). The romfile is plaintext XML, which exposed
  the OEM identity, accounts, and default config. See
  [Stock Firmware Access & Security Notes](10-stock-firmware-access.md).
- **Chip identification** — read from PCB silkscreen, PCI vendor/device IDs on the live
  bus (`14c3:7603`, `14c3:7662`), SLIC chip-id over ZSI (`RCN=0x08`/`PCN=0x75`), and
  cross-referenced against the EN751221 OEM 2.6.36 / 3.18 source tree.
- **Register-level RE** — physical memory was read on both stock (a static MIPS mmap
  helper) and OpenWrt (a `tc3162_poke` debugfs hook that `ioremap`s any phys address,
  including PCIe BARs) to confirm switch, PCIe, PCM, SLIC and GPIO register behaviour
  against the OEM driver disassembly.
- **Iterative bring-up** — each subsystem was brought up by build → flash slot B → boot →
  observe → patch, over 150+ iterations. The hard problems (LAN RX through the cascaded
  switch, 2.4 GHz enumeration, the from-scratch SLIC voice path) were solved by matching
  the OEM driver's exact sequences rather than guessing.
- **Safety** — current builds use the board-specific BMT-aware `sysupgrade`
  path with a validated TrendChip-patched image; never issue raw `mtd write`
  commands manually. Stock OEM telnet/web and UART + TFTP remain the recovery
  paths that keep most failure modes soft-brick rather than hard-brick.

A running iteration log lives in the repo under `notes/` and `docs/notes/`.

---

## Credits & links

- **This port (overlay repo):** [`Cris7015/xr500v-openwrt`](https://github.com/Cris7015/xr500v-openwrt)
  — an overlay (`package/` + `target/`) on top of cjdelisle's tree, not a full fork.
- **Pinned base:** [`cjdelisle/openwrt`](https://github.com/cjdelisle/openwrt) @ `f3605b31fb`
  (branch `plan-b-nazox1`).
- **Ethernet driver:** [`cjdelisle/econet_eth`](https://github.com/cjdelisle/econet_eth)
  @ `c2f855cf` (out-of-tree bundle; provides the DSA glue and the `mtk-tag.ko` tagger).
- **Upstream OpenWrt econet target:** the EN751221 platform was merged into mainline
  OpenWrt on 2025-09-11 (kernel 6.12), by Caleb James DeLisle (cjdelisle). The XR500v
  needs its own board DTS on top of that target.

The nested dual-switch DSA insight (modeling the external MCM switch as a child of the
on-die switch's MDIO bus) is what makes the LAN work without the unpublished MDIO-master
code, and is documented in [Ethernet & the DSA Switch](04-ethernet-dsa.md).
