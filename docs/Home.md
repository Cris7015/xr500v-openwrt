# TP-Link Archer XR500v — Teardown & OpenWrt Port

This is, to our knowledge, the first public technical documentation of the **TP-Link
Archer XR500v** (v1) — a GPON home gateway sold in Latin America. TP-Link publishes
only marketing specs for this device and states that it cannot run OpenWrt; this document
shows otherwise. The XR500v boots a mainline OpenWrt 6.12 image to a working router
today, with Ethernet, dual-band Wi-Fi, USB, the full 256 MB of RAM, and the two
RJ11 telephone ports (FXS) functioning. The one subsystem not ported is the GPON
optical WAN — see [GPON / xPON Status](09-gpon-xpon-status.md) for why.

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
table below is current as of **2026-06-22**, and now reflects the working hardware NAT
offload (PPE/HNAT) and the FXS telephony, both validated since the earlier notes.
"Working" means observed on the running device; facts that were only partially verified are
labelled as such inline. Throughput figures are noted with the test configuration they were
measured in, because the device's role and the measurement path materially change the number.

---

## What works under OpenWrt

| Subsystem | Status | Page | Notes |
|---|---|---|---|
| Boot to console (UART, A/B slot) | Working | 03 | Slot A = stock OEM, slot B = OpenWrt; selected by `bflag` |
| Ethernet — 4× GbE LAN | Working | 04 | Nested dual-switch DSA; HW switching at near line rate, CPU idle |
| LAN TX throughput (device as endpoint) | Working | 04 | ~161 Mbps after the BQL fix (was ~5 Mbps); see throughput note below |
| Software forwarding | Working | 04 | ~590 Mbps bidirectional in a router test configuration (CPU-bound software path) |
| **HW-NAT — PPE flow offload** | Working (experimental) | 04 | Auto-arms at boot; **LAN↔LAN ~929 Mbit/s wire-speed**, WAN→LAN PPPoE download ~678 Mbps, CPU idle. Not yet long-soak-tested |
| **Wi-Fi HW forwarding (WHNAT)** | Working (experimental) | 05 | PPE NATs in HW + CPU re-injects to the radio; forwarded UDP ~514 Mbit/s (OEM-class) |
| Wi-Fi 5 GHz | Working | 05 | MT7662 / `mt76x2e`, AP VHT80, up to 20 dBm from factory EEPROM |
| Wi-Fi 2.4 GHz | Working | 05 | MT7603 / `mt7603e`, second PCIe radio; required OEM PCIe reset + synthetic EEPROM |
| USB | Working | 07 | xHCI; USB2 mass storage = `/dev/sda`; USB3 has no wired T-PHY |
| 256 MB RAM | Working | 07 | 244 MB usable; needs DTS fix + kernel diet to fit the 3 MB kernel slot |
| Bridge + DHCP + firewall + internet | Working | 03 | Standard OpenWrt config over the DSA LAN bridge |
| Telephony — 2× FXS (RJ11) | Working | 06 | Le9642 SLIC, from-scratch driver; clean SIP calls, ring/answer/hangup |
| Front-panel LEDs | Working | 08 | All 10: 8 via the SoC GPIO block, plus the two Wi-Fi LEDs driven natively from the radio drivers (5 GHz via mt76 `led-sources=<2>`; 2.4 GHz via a small `mt7603` chip-GPIO patch) |
| GPON / xPON optical WAN | Not supported | 09 | Separate on-die MAC; needs an OLT head-end and ISP registration to even test |

> **Throughput note.** The two figures above measure different things and should not be
> compared directly. The ~590 Mbps software-forwarding number was measured in a dedicated
> router test configuration with a client behind a LAN port; in normal use the device sat
> as an L2 node on a switch and was not in the internet path. The validated end-to-end
> figure with the device acting as the iperf3 *endpoint* is asymmetric and CPU-bound:
> roughly **TX 153 / RX 76 Mbps** over a single LAN port (CPU saturated, RX software path
> the more expensive direction). Those are the **software** path; the **PPE hardware NAT
> offload is now integrated and auto-arms at boot**, so routed/forwarded flows run at
> wire-speed with the CPU idle — LAN↔LAN ~929 Mbit/s, WAN→LAN PPPoE download ~678 Mbps.

---

## Hardware specifications

| Component | Detail |
|---|---|
| **SoC** | EcoNet / Airoha **EN751221** (en7521 / en7528 family), MIPS **34Kc**, big-endian |
| **CPU clock** | ~600 MHz (high-precision timer clock 200 MHz) |
| **RAM** | 256 MB DDR3-1066 (clock as reported by the bootloader; the DRAM part marking was not read) — ~244 MB usable under OpenWrt; `memory@0 reg = <0x0 0x10000000>` |
| **Flash** | SPI-NAND ~128 MB raw (~112 MB usable after TrendChip BMT reserve), 128 KB erase blocks |
| **Boot layout** | Dual A/B slots — A = stock OEM, B = OpenWrt — selected by `bflag` |
| **Ethernet switch** | Dual cascaded MT7530-class: on-die @ `0x1fb58000` (MMIO) + external MCM @ MDIO `0x1f`; 4× GbE LAN |
| **2.4 GHz Wi-Fi** | MediaTek **MT7603** (`14c3:7603`), 2T2R 11b/g/n; PCIe domain 0, port0 @ `0x1fb81000`; `mt7603e` |
| **5 GHz Wi-Fi** | MediaTek **MT7662 / MT76x2** (`14c3:7662`), 2T2R 11a/n/ac; PCIe domain 1, port1 @ `0x1fb83000`; `mt76x2e` |
| **FXS / telephony** | Microsemi/Microchip **Le9642** (VE886/VP886 family) dual SLIC, over ZSI; on-die PCM engine @ `0x1fbd0000` |
| **WAN** | **GPON ONU** on optical fiber (on-die xPON MAC; not supported under OpenWrt) |
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
| 03 | [Boot, Partitions & Flashing](03-boot-partitions-flashing.md) | A/B slots, `bflag`, NAND layout, safe flashing procedure |
| 04 | [Ethernet & the DSA Switch](04-ethernet-dsa.md) | Nested dual-MT7530 DSA tree, port mapping, tagger, throughput |
| 05 | [Wi-Fi: MT7603 + MT7662](05-wifi-mt7603-mt7662.md) | Two PCIe radios, EEPROM/MAC sourcing, enumeration walls |
| 06 | [VoIP / FXS Telephony](06-voip-fxs-telephony.md) | Le9642 SLIC over ZSI, PCM/TDM, reconstructed driver, SIP stack |
| 07 | [USB, RAM & Other Peripherals](07-usb-ram-peripherals.md) | xHCI, 256 MB unlock, GPIO controller, buttons |
| 08 | [Front-Panel LEDs](08-front-panel-leds.md) | GPIO map, pad-enable quirk, the Wi-Fi LED reverse-engineering (all 10 working) |
| 09 | [GPON / xPON Status](09-gpon-xpon-status.md) | The one unported subsystem and why it is hard |
| 10 | [Stock Firmware Access & Security Notes](10-stock-firmware-access.md) | Restricted CLI, root injection, accounts, firmware verification |
| 11 | [OpenWrt Port, Build & Persistence](11-openwrt-port-build-persistence.md) | Developer guide: tree layout, build host, image recipe, UBI overlay |

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
