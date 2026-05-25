# XR500v OpenWrt port

OpenWrt port for the **TP-Link Archer XR500v** GPON router — SoC: EcoNet **EN751221** (MIPS 34Kc, big-endian).

## Status (2026-05-25)

| Function | State |
|---|---|
| Boot to console (UART, A/B slot flash) | ✅ |
| USB 2.0 | ✅ |
| PCIe / WiFi (MT7612 / mt76x2, 5 GHz AP) | ✅ |
| **Ethernet LAN — 4 ports via DSA** | ✅ **working** |
| Bridge (4 LAN + WiFi) + persistent config + internet | ✅ |
| LAN throughput | ⚠️ TX capped ~5 Mbps — open problem (see notes) |
| WAN / xPON (GPON fiber) | ❌ not supported (separate MAC block) |
| Telephone (2× RJ11 / VoIP) | ❌ not supported (separate SLIC) |

**The breakthrough:** the EN751221 has **two cascaded MT7530 switches** — an on-die one (MMIO @ `0x1fb58000`, only port5 cascade + port6 CPU) and an external MCM one (over MDIO @ `0x1f`) that carries the 4 real LAN ports. Modeling this as a **nested DSA tree** (the MCM as a child of the on-die switch's MDIO bus) makes all 4 LAN ports + WiFi work, bridged, with internet — *without* needing the unpublished MDIO-master code that previously blocked this. This closes the long-standing "❓ DSA Ethernet Switch Support" item for EN751221.

## Hardware
- SoC: EcoNet EN751221 (MIPS 34Kc, BE), SPI-NAND, ~256 MB RAM.
- Switch: dual MT7530 — on-die (MMIO `0x1fb58000`) + MCM (MDIO `0x1f`). 4× GE LAN.
- WiFi: MediaTek MT7612 (mt76x2, PCIe).
- WAN: GPON (own MAC, not in OpenWrt). Phone: VoIP SLIC (not in OpenWrt).

## Key technical notes
- **Nested DSA topology** — `target/linux/econet/dts/en751221.dtsi`: on-die `switch@1fb58000` with a `mdio { switch@1f (mediatek,mcm) }` child. MCM user ports use internal PHYs at MDIO **1–4** (port0/PHY0 has no RJ45 jack — possibly the internal GPON uplink, left disabled).
- **Port mapping is reversed** (carcasa ≠ Linux): físico LAN1→`lan1` (MCM port4), LAN2→`lan2` (port3), LAN3→`lan3` (port2), LAN4→`lan4` (port1). Labels corrected in `en751221_tplink_archer-xr500v.dts`.
- **Tagger** — econet-eth ships its own `mtk-tag.ko` (compiled from `gsw/tag-mtk.c`), *not* the kernel's `tag_mtk` (which is `=m` and never loads). It has a device-agnostic `mtk_conduit_find_user`. The MTK tag carries the MCM source port (verified `port=1` for the cable).
- **Operational gotcha** — the DSA conduit `eth0` must **NOT** be a member of `br-lan` (its bridge rx_handler bypasses the DSA tagger). `br-lan` = `lan1..lan4` (+ WiFi) only.
- **Flash** — only from stock OEM telnet `:2323` (mtd write from a *running* OpenWrt corrupts the NAND). Trendchip header patch required; keep `@0x6c = 0x80020000` (LZMA stub addr, not the ELF entry).
- **Persistent network** — `br-lan` bridge = lan1..lan4, static IP, DHCP server disabled (the device is a switch/AP on an existing LAN; avoid rogue DHCP). The DSA bridge only assembles correctly on a **clean boot**, not via `network restart`.

## econet-eth driver patches (`package/kernel/econet-eth/patches`)
- `210-mcm-reset-optional` — the MCM has no exposed reset register; make it optional.
- `220-mdio-bus-pre-dsa-register` — register the MCM's PHYs before `dsa_register_switch` (else `-ENODEV` on the user ports).
- `240-trgmii-cascade-cal` — port of the OEM `macMT7530doP6Cal` TRGMII cascade calibration (sysfs trigger). **Diagnostic outcome: the cascade TRGMII eye is clean (wide window 1–45); not the throughput bottleneck.**
- `250-qdma-tx-no-taildrop`, `260-qdma-disable-egress-ratelimit` — throughput experiments. **No effect** (kept for the record; see notes).

## Open problem — LAN TX throughput ~5 Mbps
TX (egress) caps at ~5 Mbps / ~450 packets-per-second (~2.2 ms per-packet completion latency) on a clean gigabit link, with the CPU idle, zero drops counted (netdev / switch MIB / qdisc / collisions / pause all 0), and packets egressing cleanly. RX (UDP, no return ACKs) reaches 245 Mbps; TCP both directions is limited to ~5 Mbps by the slow return ACKs.

**Ruled out (with evidence):** WiFi RF (866 Mbps link), CPU (idle, load <0.1), cascade TRGMII (clean eye), QDMA tail-drop, descriptor count (128), QDMA egress rate-limit registers. **Localized** to the SoC QDMA/PSE/GDMA TX completion path. **Next step (not yet done):** a register-level diff of the QDMA/PSE/GDMA init in `econet_qdma.c` vs the OEM SDK (`oem_src/ether/en7512/eth_lan.c` `qdma_reg_init`, which sets `QDMA_TxBufCtrl chnThreshold=6 totalThreshold=24`) — rather than further trial patches.

## Build / flash
This repo is the **XR500v overlay** for an OpenWrt tree with target `econet/en751221`. Apply with `scripts/apply-xr500v-files.sh <openwrt_dir>`, then build, trendchip-patch the sysupgrade, and flash slot B from stock OEM telnet `:2323` (`bflag set 1`, power-cycle). See `scripts/` and `notes/`.

Built on EcoNet `econet/en751221` (Caleb DeLisle's port, merged to OpenWrt mainline 2025).
