# Iteration Log

Log of what was tried, what happened, and what was learned.

## 2026-05-06: Project bootstrap
- Repo created, initial structure.

## 2026-05-06: Task 4 — Clone repos on Azure
- Cloned cjdelisle/openwrt fork + our repo on the Azure VM.
- ⚠️ **Econet support is in branch `econet-integration-tree-apr27-2026`, NOT in main.**
  - `git checkout econet-integration-tree-apr27-2026` after cloning.
  - Scripts/setup should be updated to perform this checkout automatically.
- Last cjdelisle commit (on that branch): `f3605b31fb econet: add EN751627 subtarget and Zyxel EX3301-T0 board`
- DTS files available for EN751221: vr1200v-v2, smartfiber_xp8421-b, nokia_g240g-e, zyxel_pmg5617ga, generic
- VR1200v v2 DTS = 128 lines (our starting point for XR500v)
- Feeds update + install OK

## 2026-05-06: Task 5 — Baseline VR1200v build
- Build pipeline VALIDATED on Azure VM (8 vCPU, 32GB RAM)
- Branch: `econet-integration-tree-apr27-2026`
- Output image: `openwrt-econet-en751221-tplink_archer-vr1200v-v2-squashfs-sysupgrade.bin`
- Build size: **8,487,580 bytes (8.09 MB)**
- Reference local vr1200v_sysupgrade.bin size: 7,814,770 bytes (7.45 MB) — delta +672KB (8.6%), acceptable
- No errors in `make -j8`; warnings only from python3-pysocks/unidecode in feed packages (benign)
- Total build duration: ~25 min (VM clock UTC); toolchain GCC 14.3.0 + Linux 6.12.80 + packages
- Additional outputs: initramfs-kernel.bin (6.7MB), manifest, sha256sums, profiles.json
- ✅ Pipeline OK — ready for Task 6: create the XR500v DTS

## 2026-05-06: Task 8 — XR500v DTS created
- Copied from en751221_tplink_archer-vr1200v-v2.dts
- compatible string updated to "tplink,archer-xr500v"
- model updated to "TP-Link Archer XR500v v1"
- Partition layout adjusted to the real XR500v layout (3MB kernel, 16MB rootfs)
  - boot=256K, romfile=256K, kernel=3MB, rootfs=16MB, misc=4.5MB
  - kernel1=3MB, rootfs1=16MB, others=4.9MB, bootflag=128K
  - openwrt_ubi=~62.75MB (0x3000000-0x6ec0000, with BMT reserve)
- NVMEM layout in misc partition removed (XR500v offsets unknown)
- gmac0 nvmem-cells commented out (MAC address NVMEM not mapped yet)
- wifi@pcie0/pcie1: kept with compatible=mediatek,mt76 but without nvmem-cells (EEPROM/MAC unknown)
- CPP preprocessing OK (449 lines post-include), dtc compilation OK with 1 pre-existing warning in dtsi
- DTS committed: 418736a042fac128c8b715f5edacfcd0b25d5e37

## 2026-05-06: Task 9 — Image Makefile registers XR500v
- Device/tplink_archer-xr500v block added to target/linux/econet/image/en751221.mk
- KERNEL_SIZE=3072k, IMAGE_SIZE=16384k, BLOCKSIZE=128k
- IMAGES=sysupgrade.bin with recipe: append-kernel | lzma | tclinux-trx
- TARGET_DEVICES += tplink_archer-xr500v
- Design decision: tclinux-trx used instead of tplink-v2-header because the XR500v
  is an OEM TCLinux device (not native TP-Link firmware); TPLINK_HWID unknown (encrypted firmware).
- Makefile dry-run (-n) OK: "Nothing to be done for all"

## 2026-05-06: Task 11 — First XR500v build ✅
- sysupgrade.bin image: 6166960 bytes
- initramfs-kernel.bin image: 5887402 bytes
- Incremental build without errors
- Total bin/targets/econet/en751221/ contains both VR1200v (baseline) and XR500v (ours)

## 2026-05-06: Task 12 — XR500v image verification ⚠️ CRITICAL FINDING
- Image size: 6166960 bytes (5.88 MB)
- Header magic first 4 bytes: `32 52 44 48` = **'2RDH' (TRX2 format)**
- Squashfs offset: 0x2f4782 (3098498 bytes)
- LZMA offset: 0x100

### ❌ BLOCKER IDENTIFIED:
The sysupgrade.bin image uses the `tclinux-trx` recipe (decided in Task 9), which generates a **TRX2 ('2RDH') wrapper**. 
**THE BOOTLOADER EXPLICITLY REJECTS this format** (notes from the original task).

### Comparison with reference images:
| Image | Header | TRX2? | LZMA@ | Squashfs@ | Status |
|--------|--------|-------|-------|-----------|--------|
| VR1200v sysupgrade | `03 00 00 00 'ver. 2.0'` | NO | 0x200 | 0x400000 | ✅ Accepted by bldr |
| Stock XR500v (mtd3) | `03 00 00 03 ...` | NO | 0x200 | - | ✅ In production |
| **NEW XR500v sysupgrade** | **'2RDH'** | **YES** | **0x100** | **0x2f4782** | **❌ REJECTED** |

### Required action:
- Change the image recipe in `target/linux/econet/image/en751221.mk`
- Options investigated in Task 9:
  - `tplink-v2-header`: used TPLINK_HWID (unknown, encrypted FW)
  - `tclinux-trx`: produces TRX2 wrapper (current, REJECTED)
- Needed: investigate which recipe generates the `03 00 00 ...` format without TRX2
  - Possible: switch to `append-kernel-lzma | append-squashfs` without tclinux-trx wrapper
  - Or find the VR1200v recipe that generated its accepted format

## 2026-05-06: Task 12 Fix -- Image recipe corrected
- IMAGE/sysupgrade.bin recipe changed from tclinux-trx (produces "2RDH" rejected by bldr)
  to tplink-v2-header with TPLINK_HVERSION=3 (produces "03 00 00 00 ver. 2.0")
- TPLINK_FLASHLAYOUT=16Mmtk, TPLINK_HWID=0x0ec60001 (dummy, bldr does not validate HWID)
- New header magic first 16 bytes: 030000007665722e20322e3000ffffff
- Image size: 6214858 bytes (6.0 MB)
- Header before: 32524448 ("2RDH", rejected) | Header now: 03000000 ("\x03\x00\x00\x00", accepted)
- Image at: /mnt/c/tftp/openwrt-xr500v-iter2.bin

## 2026-05-06: Task 13 — Flash slot B with XR500v iter2
- Image: openwrt-xr500v-iter2.bin (with corrected tplink-v2 header)
- Image layout: [512B file_hdr][3MB-512B kernel_data][512B zeros][2.93MB rootfs squashfs]
- kernel1_v3.bin = src[0:0x300000]: includes file header as partition header (03 00 00 00 ver. 2.0)
- rootfs1_v3.bin = src[0x300200:] padded to 16MB: starts with squashfs hsqs magic
- kernel1 (mtd6): tftp 3MB OK → /userfs/bin/mtd -f -e kernel1 write → K_RC_0
- rootfs1 (mtd7 via mtd0 offset 0x1b00000): tftp 16MB OK → /userfs/bin/mtd writeflash → R_RC_0
- writeflash output: 128 sectors [e][w] per sector, "writeflash: total write 0x1000000 bytes"
- Readback verification kernel1 (mtd6): 030000007665722e20322e3000ffffff ✓ MATCH
- Readback verification rootfs1 (mtd0+0x1b00000): 687371730803000027abf06900000400 = 'hsqs' ✓ MATCH
- Slot A (mtd3/mtd4) intact: not touched at any point
- Next: power cycle + bldr bflag set 1 + autoboot (Task 14)

## 2026-05-06: Task 14 — Boot test iter 1 ❌ CRASH
- tplink-v2 header ACCEPTED by bldr (improvement over previous iter)
- LZMA decompress successful: kernel loaded at 0x80020000
- Kernel CRASH at startup: EPC=0x81fb91a8, BADVADDR=0xc0000000, CAUSE=8 (Bus Error)
- Same crash as in previous session with VR1200v sysupgrade — independent of the custom XR500v DTS
- Hypothesis: crash in EcoNet platform code or in en751221.dtsi (hardcoded peripheral access)
- 0xc0000000 = MIPS KSEG2, peripheral does not exist or is at a different address on XR500v vs kernel assumption
- Next: Codex analysis to identify the function at EPC=0x81fb91a8

## 2026-05-06: Task 16 — Iteration loop summary

| iter | change | result |
|------|--------|--------|
| 1 | Initial build, PCIe enabled, no TrendChip header | bldr crash EPC=0x81fb91a8 BADVADDR=0xc0000000 |
| 2 | Disabled pcie0/pcie1 (Codex hypothesis) | Same crash — PCIe was not the cause |
| 3 | Patched TrendChip header (magic + entry=0x8176d140) | Bldr jumped to kernel_entry too early → RI exception |
| 4 | entry=0x80020000 (wrapper start) | Kernel booted, but panic mounting stock LZMA rootfs from mtd3 |
| 5 | DTS: moved linux,rootfs to mtd7 (rootfs1) | Mounted mtd7, failed: rootfs flashed garbage |
| 6 | Fixed flash script: rootfs slice from 0x300200 not 0x400000 | **Shell reached, PoC complete** |

## 2026-05-06: Task N — PoC validated

- Linux 6.12.80 boots from slot B (mtd6 kernel + mtd7 rootfs)
- DTS recognized: "TP-Link Archer XR500v v1"
- Squashfs XZ root mounted readonly
- procd init runs, BusyBox shell available, eth0 port LAN registered
- Slot A intact (bflag=0 returns to stock OEM firmware)

### Key insights captured
1. The bldr 512-byte header has TrendChip-specific fields that tplink-v2-header recipe doesn't fill — must patch post-build with magic at 0x60, entry at 0x6c, rootfs offset at 0x7c
2. Kernel entry must be the WRAPPER start (= KERNEL_LOADADDR = 0x80020000), not the final vmlinux entry — wrapper does inner LZMA decompress before jumping to real kernel
3. `linux,rootfs` must be on mtd7 (slot B rootfs1) since stock mtd3 has unsupported LZMA squashfs
4. flash-from-wsl.sh had a bug: assumed rootfs at offset 0x400000 but tplink-v2 recipe puts it at 0x300200 (header 0x200 + KERNEL_SIZE 0x300000)

## 2026-05-10: Task R — RX bug deep dive iter97-iter108 (path A + Op 2)

Massive 12+ hour session attacking the community-unresolved RX bug on lan1-4. WAN+WiFi worked; user ports had carrier UP but RXU=0 (no frames reach switch fabric MAC). Final state: PMCR exact match with OEM stock, but RX still 0 — confirmed bug is at PHY↔MAC hardware-internal level, invisible from MMIO registers.

### Iterations

| iter | change | result |
|------|--------|--------|
| iter97 | Codex round 1 (PHY 9-12) — broke econet_eth probe with -EINVAL | Source corruption from partial patch state |
| iter98a-d | DTS PHY remap 1-4 (per OEM ethphxcmd miir) + /delete-node/ &switch0port4 + &switch0phy4 | DSA enumerates 4 user ports, link UP 1Gbps |
| iter99 | DSA_TAG_PROTO_MTK → 8021Q (naive switch) | Fails silently — kernel without CONFIG_NET_DSA_TAG_8021Q standalone |
| iter100 | Revert iter99 → baseline iter98d | Stable |
| iter101 | port0.regs.stag_en = 1 (enable HW STAG insertion on RX) | Frames start arriving at CPU but with fport=13 (unexpected) |
| iter102 | Full OEM macEN7512STagEnable: CDMA_VLAN_CTRL[0]=1 + fwd_cfg[24]=1 + stag_en=1 + vlan=0x81000001 | Hex dump reveals MTK STAG inline format 00 05 00 00 (port=5 = WAN, never 0-3) |
| iter103-104 | Debug log sp_tag + skb hex dump first 32 bytes | sp_tag=0 (descriptor empty); STAG is inline post-srcMAC. tag_mtk decodes correctly, only WAN frames arrive |
| iter105 | REMOVE iter84 PCR forcing 0x00ff (was L2-bridging LAN↔WAN bypass CPU); let DSA program PCR=0x004d | PC via LAN has no internet (L2 bridge cut), but RXU still 0 |
| iter106 | Codex round 1 fix: PMCR bits 13/14 (PMCR_MAC_RX_EN/TX_EN) + bits 4/5 (FORCE_RX/TX_FC_EN) | Bits set OK (PMCR=0x5e330 when link UP) but RXU=0 still |
| iter107 | Codex round 2 + DeepSeek-V4 Pro: PVID per port 1..7, MFC=0x404040e0, CPU PVC bit 5 PORT_SPEC_TAG → 0x81008120 | All registers match OEM, RX persists 0 |
| iter108 | Clear PMCR bit 15 MT7530_FORCE_MODE → PMCR=0x56330 (**exact match OEM**) | **Perfect register-level OEM match. RXU=0 STILL.** Bug confirmed sub-register layer |

### Hard wall conclusions

1. **All visible registers replicated**: PMCR (0x56330), PCR (0x004d DSA matrix), PVC (0x81008100 user + 0x81008120 CPU with PORT_SPEC_TAG), PVID (1..7 per port), MFC (0x404040e0), TPID (0x8100 all ports), stag_en=1, fwd_cfg l2lu_stag_2cpu set, vlan reg=0x81000001 — all byte-for-byte identical to OEM stock dump.
2. **ethtool -S lan1 shows ZERO RX activity** in any direction. Even RxCrcErr/RxAlignErr/RxFragErr = 0 → switch MAC never sees frames from PHY.
3. **TX same problem**: 19 packets sent from Linux → TxBytes counter on switch = 0. MAC neither RX nor TX on user ports.
4. **WAN port (port@5 RGMII fixed-link)** works perfectly because RGMII bypasses internal PHY-MAC pipeline entirely.
5. **Bug is at PHY-MAC switch fabric interface** — invisible from MDIO/MMIO. Calibration values, clock gating, or hidden reset sequence the OEM eth.ko binary blob does.

### Other key findings

- **Inverted enclosure ↔ Linux port labeling**: jack LAN1 = Linux lan4, LAN2 = lan3, LAN3 = lan2, LAN4 = lan1 (confirmed via OEM  + live cable-plug tests)
- **PHY MDIO addresses real**: 1, 2, 3, 4 (NOT 0-3 from base dtsi, NOT 9-12 from econet-linux wiki). Confirmed via OEM .
- **MTK STAG format**: standard 4-byte inline header after src MAC:  (port 5 = WAN in our captures). Decoded correctly by mainline  with .
- **patch_trendchip_header.py default entry**: changed from 0x8176d140 (iter91-specific) to 0x80020000 (= KERNEL_LOADADDR, always correct for OpenWrt MIPS pipeline).
- **OEM uses 802.1Q VLAN model, NOT STAG**: stock OEM creates eth0 + eth0.1..eth0.4 sub-interfaces via Linux 802.1Q stack.  flag in OEM eth.ko enables this mode.

### Path forward (not implemented, ideas for next session)

1. **JTAG hardware tracing** — capture OEM init register write sequence with exact timing
2. **Port OEM eth.ko as out-of-tree platform driver** — abandon DSA, replicate  model exactly  
3. **Exhaustive disasm** — analyze , , calibration value loading from factory partition
4. **Clock controller comparison** — diff MFD_SYSCON regs vs OEM pre-init state
5. **SDK_tcetherphy_7512.c reading** — 315KB vendor SDK reference source has function definitions for , ,  that may reveal missing init step

### Regressions in iter108

- WiFi PCIe binding broken (mt76 modules loaded, /sys/class/ieee80211/ empty, no PCIe enumeration in dmesg). Likely a dropped kernel CONFIG. Fixable next session.

