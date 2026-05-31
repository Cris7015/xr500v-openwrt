## Summary

The TP-Link Archer XR500v v1 is built around an **EcoNet (now Airoha) EN751221** GPON home-gateway SoC — a MIPS, big-endian part from the EN75xx (en7521/en7528) family, *not* TP-Link or any of the usual Broadcom/Realtek/Qualcomm silicon. The board is in fact an OEM TCLinux/TrendChip HGW reference design rebadged by TP-Link: the stock firmware's plaintext romfile still carries `Vendor="TC"`, `SWVer="TCLinux Fw 7.1.2.7"`, default Hinet/Taiwan DNS, and default/undocumented operator service accounts. The SoC integrates the CPU, a dual cascaded MT7530-class Ethernet switch, two PCIe root ports (each carrying a separate MediaTek WiFi radio — MT7603 for 2.4 GHz, MT7662 for 5 GHz), an xHCI USB controller, a TDM/PCM block driving a Microsemi Le9642 dual SLIC over ZSI for the two FXS phone ports, and a GPON ONU on the optical WAN. The board has **256 MB of DDR3** (the stock DTS under-declared 128 MB; the full amount was unlocked for OpenWrt) and a **~128 MB SPI-NAND** with a TrendChip-derived bad-block-table (BMT) scheme. This page is the chip-level inventory and board map, with the register/bus addresses recovered during the reverse-engineering of the owner's own device.

> Cross-references: the boot/flash layout is detailed in the partitioning and dual-boot page; the switch/DSA bring-up in the Ethernet/DSA page; the two WiFi radios in the WiFi page; the SLIC/VoIP block in the telephony page.

---

## Register & MMIO address map

A consolidated view of the physical base addresses referenced throughout this wiki. All are in the SoC's `0x1fxxxxxx` KSEG1 physical range except the two PCIe device BARs, which are assigned in the `0x2xxxxxxx` window. Addresses are taken from the live device tree (`en751221.dtsi`) and the reconstructed drivers; the two BARs are read back from the running endpoints.

| Block | Physical base | Notes |
|---|---|---|
| SPI / NAND controller | `0x1fa10000` (len `0x140`) | `airoha,en7523-spi` → `spi-nand` |
| Chip SCU (syscon / IOMUX) | `0x1fa20000` (len `0x388`) | `econet,en751221-chip-scu` |
| └ IOMUX control 1 | `0x1fa20104` | pinmux register within the chip-SCU block (used for the panel-LED and ZSI pad muxing) |
| USB IPPC | `0x1fa80700` | xHCI MAC/IPPC companion block |
| PCIe PHY1 (5 GHz path) | `0x1fac0000` (len `0x1000`) | `econet,en751221-pcie-phy1` |
| PCIe PHY0 (2.4 GHz path) | `0x1faf2000` (len `0x1000`) | `econet,en751221-pcie-phy0` |
| Clock/reset controller (SCU) | `0x1fb00000` (len `0x970`) | `econet,en751221-scu`; PCIe/system reset bits live here (poked as `0x1fb000xx`) |
| INTC (interrupt controller) | `0x1fb40000` (len `0x100`) | `econet,en751221-intc` |
| Ethernet frame engine | `0x1fb50000` (len `0x8000`) | `econet,en751221-eth` (gmac0/gmac1); width narrowed to `0x8000` to avoid the switch region |
| On-die switch | `0x1fb58000` (len `0x8000`) | MT7530-class, MMIO |
| PCIe config (pciecfg) | `0x1fb80000` (len `0x1000`) | `mediatek,generic-pciecfg` |
| PCIe root port 0 | `0x1fb81000` (len `0x1000`) | domain 0 → MT7603 (2.4 GHz) |
| PCIe root port 1 | `0x1fb83000` (len `0x1000`) | domain 1 → MT7662 (5 GHz) |
| USB xHCI | `0x1fb90000` (len `0x4000`) | `mediatek,mt8173-xhci` |
| PCM / TDM | `0x1fbd0000` (len `0x1000`) | `econet,en751221-pcm` (drives the SLIC) |
| ZSI wrapper (SLIC bus) | `0x1fbd1000` (`+ id·0x2000`) | ZSI MPI transport to the Le9642 |
| UART (console) | `0x1fbf0000` (len `0x30`) | `ns16550`, 115200 8N1 |
| GPIO (tc3162) | `0x1fbf0200` (len `0x80`) | `tplink,tc3162-gpio`, 64 GPIOs |
| High-precision timer | `0x1fbf0400` (len `0x100`) | `econet,en751221-timer` |
| MT7603 BAR0 (2.4 GHz radio) | `0x20000000` | PCIe device BAR (same under stock and OpenWrt) |
| MT7662 BAR0 (5 GHz radio) | `0x20100000` (stock) / `0x28000000` (OpenWrt) | PCIe device BAR; stock and mainline assign different windows, same register offsets |

The numbers above are repeated in context in the per-block sections that follow.

---

## 1. The EN751221 SoC

### Vendor and family

| Property | Value |
|---|---|
| Marketed vendor | EcoNet Technologies (acquired by / branded **Airoha**) |
| Part family | EN75xx — referenced as **EN751221**, also **EN7521 / EN7528** in PCIe/clock compatibles |
| CPU core | MIPS **34Kc** (DTS `compatible = "mips,mips34Kc"`), big-endian |
| CPU clock | ~600 MHz (the bldr supports `cpufreq 156…450`, suggesting an adjustable range; exact stock MHz not captured) |
| Endianness | **Big-endian** — important for every register dump, EEPROM read, and cross-compile (`mips-openwrt-linux-musl-`) |
| High-precision timer clock | 200 MHz fixed (`hpt_clock`) |

The OpenWrt toolchain triple is `mips_24kc_musl`, but that is only the **build-target base ISA** — the 24Kc is binary-compatible and used as the OpenWrt target name. The actual core declared by the device tree (`cpu@0`, `compatible = "mips,mips34Kc"`) is the **34Kc**. The distinction matters only for the DTS `cpu@0` node; userspace and the kernel are built against the 24kc target.

### en7521 / en7528 family relationship

The same physical SoC is referenced under several EcoNet/Airoha part numbers across the firmware, datasheets, and OpenWrt sources. They are the same family member with different naming conventions:

- **`econet,en751221`** — the SoC-level compatible used by the OpenWrt `econet` target and the device tree root.
- **`econet,en7528-pcie`** / `mtk_pcie_startup_port_en7528` — the PCIe root-complex controllers are documented under the **EN7528** name (the PCIe IP shares the EN7528 register/link map; e.g. `EN7528_LINKUP_REG = 0x50`, `EN7528_HOST_MODE = 0x00804201`).
- **EN7521 / EN7521S/F** — appears in the OEM PCIe-PHY init source as a variant selector.

For practical purposes, treat EN751221 ≈ EN7521 ≈ EN7528 as one chip family. Sibling devices that share the SoC and were used as porting references include the **TP-Link Archer VR1200v v2**, **Nokia G-240G-E**, **SmartFiber XP8421-B**, **Zyxel PMG5617GA**, and the dual-WiFi **VC220-G3u**.

### Mainline OpenWrt status

The EcoNet EN75xx platform was merged into `openwrt/openwrt.git` mainline on **2025-09-11** (commit `73d0f9246042a487faf930a0571bd8c080bbc78f`, author Caleb James DeLisle / cjdelisle), targeting **kernel 6.12**. Most of the SoC support also landed in linux-mips upstream. The OpenWrt target is named **`econet`** with subtarget **`en751221`**.

The XR500v itself is **not** an upstream-supported device. This port is an **overlay on top of `cjdelisle/openwrt` (pinned at `f3605b31fb`, branch `plan-b-nazox1`)**, adding a board-specific DTS, the dual-switch DSA model, the MT7603 2.4 GHz bring-up, the USB fix, the 256 MB unlock, and the reconstructed FXS/VoIP driver. The base cjdelisle tree ships the `en751221.dtsi`, the EN7528 PCIe support (patch `912-…`), and the en75_bmt NAND driver.

> Note: TP-Link's marketing material states the XR500v cannot run OpenWrt. In practice it boots OpenWrt mainline (kernel 6.12) from slot B with working Ethernet, dual-band WiFi, USB, 256 MB RAM, and the FXS phone ports.

### Major MMIO blocks

The SoC's on-chip peripherals (from `en751221.dtsi`), all in the `0x1fxxxxxx` KSEG1 physical range:

| Block | Address | Compatible / role |
|---|---|---|
| SPI / NAND controller | `0x1fa10000` (len 0x140) | `airoha,en7523-spi` → child `spi-nand` |
| Chip SCU (syscon) | `0x1fa20000` (len 0x388) | `econet,en751221-chip-scu` — IOMUX/pinmux (e.g. `0x1fa20104`), clock-src bits |
| PCIe PHY1 (5 GHz path) | `0x1fac0000` (len 0x1000) | `econet,en751221-pcie-phy1` |
| PCIe PHY0 (2.4 GHz path) | `0x1faf2000` (len 0x1000) | `econet,en751221-pcie-phy0` |
| Interrupt controller (INTC) | `0x1fb40000` (len 0x100) | `econet,en751221-intc` (has "shadow interrupt" quirks) |
| Clock / reset controller (SCU) | `0x1fb00000` (len 0x970) | `econet,en751221-scu` — clocks + resets, also the PCIe/system reset bits |
| Ethernet frame engine | `0x1fb50000` (len 0x8000) | `econet,en751221-eth` (gmac0/gmac1) |
| On-die switch | `0x1fb58000` (len 0x8000) | `econet,en751221-switch` (MT7530-class, MMIO) |
| PCIe config (pciecfg) | `0x1fb80000` (len 0x1000) | `mediatek,generic-pciecfg` |
| PCIe root port 0 | `0x1fb81000` (len 0x1000) | `econet,en7528-pcie`, domain 0 → MT7603 (2.4 GHz) |
| PCIe root port 1 | `0x1fb83000` (len 0x1000) | `econet,en7528-pcie`, domain 1 → MT7662 (5 GHz) |
| USB xHCI | `0x1fb90000` (len 0x4000) + IPPC `0x1fa80700` | `mediatek,mt8173-xhci` |
| PCM / TDM | `0x1fbd0000` (len 0x1000) | `econet,en751221-pcm` (drives the SLIC) |
| ZSI wrapper (SLIC bus) | `0x1fbd1000` | ZSI MPI transport to the Le9642 |
| UART (console) | `0x1fbf0000` (len 0x30) | `ns16550`, 115200 8N1 |
| GPIO (tc3162) | `0x1fbf0200` (len 0x80) | `tplink,tc3162-gpio`, 64 GPIOs |
| High-precision timer | `0x1fbf0400` (len 0x100) | `econet,en751221-timer` |

Because the OpenWrt kernel is built **without `CONFIG_DEVMEM`**, physical registers are read/written through a debugfs poke exported by the `gpio-tc3162` driver:

```sh
# read a physical register (result goes to dmesg)
echo "0x1fb00088" > /sys/kernel/debug/tc3162_poke
# write a physical register
echo "0x1fb00088 0x77c08000" > /sys/kernel/debug/tc3162_poke
```

On the **stock OEM** firmware (kernel 3.18, which *does* have `CONFIG_DEVMEM` but no `/dev/mem` node and no busybox `mknod`), the same was done with a small static MIPS-BE helper that creates the char node itself (`mknod /tmp/.mem c 1 1`) and `mmap`s it. See the reverse-engineering/tooling page.

---

## 2. The OEM-rebadge story (TCLinux / TrendChip HGW)

The "TP-Link Archer XR500v v1" is a **rebadged OEM home gateway**, not an original TP-Link design. The evidence lives in the stock firmware's `romfile` partition, which is stored as **plaintext XML**:

| Romfile field | Value |
|---|---|
| `Vendor` | `"TC"` (TrendChip) |
| `SWVer` | `"TCLinux Fw 7.1.2.7"` and `"PT632-CT-V1.0"` |
| `Manufacturer` | `"Mediatek"` |
| `ModelName` | `"Mediatek-DMS"` |
| Default DNS | `168.95.1.1` (Hinet / Taiwan) |
| Default SSIDs | `ChinaNet-AP1` |
| TR-069 ACS | `devacs.edatahome.com` |
| Operator service accounts | `telecomadmin/<vendor-default-password>`, `useradmin/1234` (default/undocumented operator defaults) |

So the stack is a **TCLinux / TrendChip HGW** reference firmware running on EcoNet silicon, with a MediaTek-supplied driver base. TP-Link layered its own web-admin login on top (the owner's unit accepts the admin password `<router-password>` at that layer), but the rest of the HGW firmware — the TR-069 client, the operator service accounts, the romfile format — is intact underneath.

This is why the device behaves like a generic EcoNet/TrendChip GPON gateway and not like a typical Archer: the partition layout, the `tc3162` proc interface (`/proc/tc3162/…`), the `slic3` SLIC driver, the monolithic `eth.ko` switch driver, and the bootloader's `bflag`/`writeflash`/`cpufreq` commands are all TrendChip/EcoNet conventions, not TP-Link ones.

---

## 3. Memory: 256 MB DDR3 (and how it was unlocked)

The board physically carries **256 MB of DDR3**. The DRAM speed grade (commonly cited as DDR3-1066) is **bootloader-reported only — the memory part itself was not read**. The bootloader confirms the size at power-on:

```
DRAM size=256MB
Memory size 256MB
```

However, the device tree (inherited from the porting base) declared only **128 MB**, so OpenWrt left half the RAM unused. Restoring the full amount required three coordinated changes — it was not a one-line DTS edit, because the larger memory map collided with the tiny NAND kernel partition:

1. **DTS memory node** — `en751221_tplink_archer-xr500v.dts`:
   ```dts
   memory@0 {
       device_type = "memory";
       reg = <0x00000000 0x10000000>;   /* 256 MB */
   };
   ```
2. **Disable `CONFIG_TARGET_ROOTFS_INITRAMFS`** — it was embedding the rootfs into the kernel image (via `CONFIG_INITRAMFS_SOURCE`), bloating the compressed kernel.
3. **Kernel "diet"** — `CONFIG_KERNEL_KALLSYMS=n` + `CONFIG_KERNEL_DEBUG_INFO=n`, so the LZMA-compressed kernel fits under **3072 KB**.

The hard constraint is the **`kernel1` NAND partition = exactly 3 MB**, and the image recipe rounds the kernel up to the next 3072 KB multiple (`dd … bs=3072k conv=sync`). A kernel even a few KB over 3072 KB rounds to 6 MB and no longer fits the slot. With the diet applied, the compressed kernel stays under 3 MB, fits `kernel1`, and the device boots with **MemTotal ≈ 249980 kB (244 MB usable)** — up from the previous ~117 MB.

---

## 4. Flash: SPI-NAND with TrendChip BMT

| Property | Value |
|---|---|
| Type | SPI-NAND, ~128 MB raw (~112 MB usable user area after BMT reserve) |
| Erase block | 128 KB |
| Controller | `airoha,en7523-spi` SPI master → `spi-nand` device |
| Bad-block scheme | **TrendChip BMT** (`en75_bmt.c`, ~1439 LoC in the econet target) |

The flash uses a **custom Bad Block Table (BMT)** inherited by the EN75xx bootloader from TrendChip. The BMT keeps the bad-block status in a table in the NAND reserve area rather than purely from per-block OOB markers. This is critical: if firmware is flashed without respecting the BMT, the bootloader may consider the NAND corrupt and try to rebuild it — a brick risk. The OpenWrt `en75_bmt` driver implements it correctly, and the XR500v DTS enables it explicitly:

```dts
&nand {
    econet,bmt;
    econet,can-write-factory-bbt;
    econet,factory-badblocks = <>;   /* empty -> skip destructive OOB rescan */
};
```

The empty `factory-badblocks` list and a pair of kernel patches (`910-en75_bmt-block-isbad-override`, `911-nand-erase-skip-isbad`) were needed so that UBI's ECC-in-OOB writes don't get misread as new bad blocks on every boot — otherwise the UBIFS overlay's logical→physical mapping drifts and the overlay "loses" data after reboots.

The NAND is split into a **dual-slot A/B layout**: slot A holds the stock OEM kernel+squashfs (kept read-only and recoverable), slot B holds OpenWrt, plus a `misc` factory-data partition, a `bootflag` partition (the A/B selector), and a dedicated 64 MB **`openwrt_ubi`** partition carved from the free area for the UBIFS overlay. The full table is on the partitioning/boot page; the factory-data offsets used by this inventory are: **ETH MAC at `misc` + `0xF100`** and **MT7662 EEPROM at `misc` + `0xE0000`**.

---

## 5. Full chip inventory

| Chip / block | Part | Role | Bus / interface | OpenWrt driver | Status |
|---|---|---|---|---|---|
| **SoC** | EcoNet/Airoha **EN751221** | CPU + integrated peripherals | — (MIPS 34Kc BE) | `econet` target | Working |
| **DRAM** | DDR3 (256 MB) | System RAM | on-SoC DDR ctrl | — | Working, 244 MB usable |
| **NAND** | SPI-NAND (~128 MB) | Boot + rootfs + overlay | SPI (`0x1fa10000`) | `spi-nand` + `en75_bmt` | Working |
| **Ethernet switch (on-die)** | MT7530-class, ID `EN751221` | Cascade port5 + CPU port6 only | MMIO `0x1fb58000` | DSA `econet,en751221-switch` | Working |
| **Ethernet switch (external)** | MT7530-class (MCM), ID `EN751221_EXT` | 4× user GbE LAN | MDIO addr `0x1f` (MCM) | DSA `mediatek,mcm` child | Working |
| **2.4 GHz WiFi** | MediaTek **MT7603** (`14c3:7603`) | 802.11b/g/n 2×2 | PCIe domain 0 (port0 `0x1fb81000`) | `mt7603e` | Working as AP (after OEM PCIe reset replicated + synthetic EEPROM) |
| **5 GHz WiFi** | MediaTek **MT7662 / MT76x2** (`14c3:7662`) | 802.11ac 2×2 VHT80 | PCIe domain 1 (port1 `0x1fb83000`) | `mt76x2e` | Working as AP, factory EEPROM |
| **FXS / SLIC** | Microsemi/Microchip **Le9642** (VE886/VP886) | 2× analog phone line | ZSI over PCM (`0x1fbd1000`) | `econet-slic` (reconstructed) | Working |
| **PCM / TDM** | EN751221 on-SoC | Codec/voice DMA to SLIC | MMIO `0x1fbd0000` | `pcm-en751221` (reconstructed) | Working |
| **WAN / GPON** | GPON ONU + xPON MAC | Optical fiber WAN | xPON MAC block (own) | — | Not in OpenWrt |
| **USB** | MediaTek xHCI | USB host (storage) | `0x1fb90000` | `xhci-mtk` | Working, USB2 (`/dev/sda`); USB3 unwired |
| **GPIO / LEDs** | TrendChip **tc3162** GPIO | Panel LEDs + PCIe power lines | MMIO `0x1fbf0200` | `gpio-tc3162` | Working, 8/10 LEDs |
| **UART** | ns16550 (on-SoC) | Serial console | `0x1fbf0000` | `8250`/`ns16550` | Working, 115200 8N1 |

### Notes on individual chips

**MT7603 (2.4 GHz)** and **MT7662 (5 GHz)** are two *separate* PCIe endpoint chips, one behind each of the SoC's two PCIe root ports — confirmed in the stock `/proc/bus/pci/devices`. Under OpenWrt they enumerate as `phy0`/`phy1` on domains `0000:01:00.0` (MT7603) and `0001:…` (MT7662). The MT7603 BAR0 is `0x20000000` (same on stock and OpenWrt). The MT7662 BAR0 is `0x20100000` under the stock OEM driver and `0x28000000` under OpenWrt (mainline assigns a different window; the register offsets are identical). The MT7662's factory calibration EEPROM (chip ID `0x7662` at byte 0) lives in the `misc` partition at `0xE0000`; the MT7603 has no stored EEPROM (its efuse holds only partial RF cal — XTAL/temp/TX-power-start — and no MAC), so a synthetic EEPROM is inlined in the DTS via `mediatek,eeprom-data`. WiFi MACs derive from the ETH MAC at `misc + 0xF100` (5 GHz uses it directly, 2.4 GHz uses `+1` via `mac-address-increment`). See the WiFi page for the full bring-up.

**Le9642 SLIC** — a Microsemi/Microchip dual-channel (VE886/VP886-family) subscriber line interface for the two RJ11 FXS ports, reached over **ZSI** (a serial MPI transport multiplexed on the PCM bus, wrapped at `0x1fbd1000 + id·0x2000`). Live readback returns RCN=`0x08`, PCN=`0x75`. The stock driver is `slic3.ko`; OpenWrt support is a from-scratch reconstruction. See the telephony/VoIP page.

**Ethernet switch — dual cascaded MT7530.** This was the single hardest part of the port. The EN751221 has **two** MT7530-class switches in cascade, *not* one (confirmed from the stock OEM `/proc/tc3162/gsw_link_st`, which prints both an "External switch" and an "Internal switch"):

```
External switch (MCM, over MDIO @0x1f): ports 1..4 = user LANs, port6 = cascade out
        │ (TRGMII cascade)
        ▼
Internal switch (on-die, MMIO @0x1fb58000): port5 = cascade in, port6 = CPU (TRGMII to SoC gmac0)
```

For roughly 137 iterations the project (and the upstream econet effort) attacked the on-die/internal switch, whose ports 0–4 are an empty fabric. The key insight was to model the topology as a **nested DSA tree** — the external MCM switch as a child of the on-die switch's MDIO bus — which makes the 4 LAN ports work. The chassis port labels are inverted relative to the Linux `lanN` names. Details on the Ethernet/DSA page.

**GPON / WAN.** The optical WAN is a GPON ONU served by the SoC's own xPON MAC/PHY block (the eth node declares `XPON_MAC_RST` / `XPON_PHY_RST` reset lines). This block is **not supported in OpenWrt** — it is a separate MAC from the Ethernet frame engine, the OEM xPON/GPON driver is a large stack, and there is no OLT to register against in a typical OpenWrt deployment. The four GbE LAN ports and WiFi are fully functional; fiber WAN is the one subsystem left to the stock firmware.

**USB.** A MediaTek xHCI controller (compatible `mediatek,mt8173-xhci`). The USB3 port has no wired/powered T-PHY, so `STS1_U3_MAC_RST` never leaves reset and `host_enable` times out (`-145`); the fix is `mediatek,u3p-dis-msk = <0x1>` in the DTS, which disables U3 and leaves the **USB2** port active (a pendrive enumerates as `/dev/sda`).

**tc3162 GPIO block.** The TrendChip GPIO controller at `0x1fbf0200` drives the front-panel LEDs (GPON, WPS, USB, alarm, phone1/2, internet) and — importantly — was also implicated in the PCIe power/reset path during the MT7603 bring-up. 8 of the 10 panel LEDs work via `gpio-leds`; the two WiFi-band LEDs are *inside* the MT7603/MT7662 chips (driven by the mt76 LED callbacks), not by these SoC GPIOs. Panel-LED operation also depends on the IOMUX control 1 register (`0x1fa20104`) being set at probe time.

---

## 6. Board-level block diagram

```
                              ┌──────────────────────────────────────────┐
                              │   EcoNet/Airoha EN751221 SoC             │
   256 MB DDR3 ───────────────┤  MIPS 34Kc BE, ~600 MHz                  │
   SPI-NAND ~128 MB ──SPI─────┤  @0x1fa10000 (airoha,en7523-spi)         │
   UART console ──────────────┤  ns16550 @0x1fbf0000 (115200 8N1)        │
                              │                                          │
   ┌── PCIe domain 0 ─────────┤  pcie@0x1fb81000  ──▶ MT7603 (2.4 GHz)   │
   │   (port0, phy@0x1faf2000)│                       PCIe, mt7603e      │
   │                          │                                          │
   ┌── PCIe domain 1 ─────────┤  pcie@0x1fb83000  ──▶ MT7662 (5 GHz)     │
   │   (port1, phy@0x1fac0000)│                       PCIe, mt76x2e      │
   │                          │                                          │
   │   USB host ──────────────┤  xHCI @0x1fb90000  ──▶ USB2 port (sda)   │
   │                          │                                          │
   │   Ethernet FE/QDMA ──────┤  eth @0x1fb50000 (gmac0, TRGMII)         │
   │                          │        │ TRGMII CPU port                  │
   │                          │        ▼                                  │
   │   On-die MT7530 ─────────┤  switch @0x1fb58000 (MMIO)               │
   │     port6=CPU            │     port5 ◀── cascade ──┐                │
   │                          │                          │ (TRGMII)       │
   │                          │   external MT7530 (MCM) ─┘ @MDIO 0x1f     │
   │                          │     port1..4 ──▶ 4× RJ45 GbE LAN          │
   │                          │                                          │
   │   PCM/TDM ───────────────┤  pcm @0x1fbd0000 ──ZSI@0x1fbd1000──▶      │
   │                          │                    Le9642 SLIC ──▶ 2× RJ11│
   │                          │                                          │
   │   xPON MAC/PHY ──────────┤  GPON ONU ──▶ optical WAN (stock only)   │
   │                          │                                          │
   │   tc3162 GPIO ───────────┤  @0x1fbf0200 ──▶ panel LEDs, PCIe pwr    │
                              └──────────────────────────────────────────┘
```

---

## 7. Quick reference

- **Architecture:** MIPS 34Kc, **big-endian** — every memory/EEPROM read is BE; cross-compile with `mips-openwrt-linux-musl-`.
- **Read a physical register (OpenWrt):** `echo "0xADDR" > /sys/kernel/debug/tc3162_poke` (result in `dmesg`); add a value to write.
- **Boot select:** bootloader `bflag` — `0` = stock OEM (slot A), `1` = OpenWrt (slot B). The `boot` command is unreliable; power-cycle instead. A **cold boot** is required for the MT7603 (2.4 GHz) PCIe reset to take effect.
- **Recovery:** UART → interrupt bldr → `bflag set 0` → power-cycle returns to intact stock firmware. Flash only from the stock telnet shell on port `:2323` (writing NAND from a running OpenWrt corrupts it).
