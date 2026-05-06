# XR500v OpenWrt PoC

OpenWrt mainline port for **TP-Link Archer XR500v v1** (SoC: EcoNet EN751221).

**Status:** PoC en desarrollo. Goal: kernel arranca a shell por UART.

**Hardware target:**
- SoC: EcoNet EN751221 (MIPS 34Kc V5.8 dual-core, MIPS32 BE)
- RAM: 256 MB DDR3
- NAND: 128 MB SPI NAND (F50L1G)
- WiFi: MediaTek MT7603 + MT7612 (mt76 driver)

**Approach:** DTS forked from Archer VR1200v v2 (same SoC family in cjdelisle's fork) with XR500v-specific adjustments (partition layout, MAC mappings, GPIO mappings).

**Reference:** Built on top of [cjdelisle/openwrt](https://github.com/cjdelisle/openwrt) target `econet/en751221`.

See `docs/` for the design spec and `notes/iteration-log.md` for development history.
