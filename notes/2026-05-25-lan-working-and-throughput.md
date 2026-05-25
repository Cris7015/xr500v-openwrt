# 2026-05-25 — LAN working (nested DSA) + TX throughput investigation

## Summary
After ~155 iterations of the wrong (single-switch MMIO) model, the LAN was cracked by
switching to a **nested dual-DSA-switch** topology. All 4 LAN ports + WiFi now work,
bridged, with internet, persistent across reboot. The remaining issue is LAN TX
throughput (~5 Mbps), localized but not yet root-caused.

## How the LAN was cracked
1. **Nested DSA tree.** EN751221 = two cascaded MT7530s: on-die (MMIO `0x1fb58000`,
   only port5 cascade + port6 CPU) + external MCM (MDIO `0x1f`, the 4 LAN ports).
   The on-die switch (mt7530-mmio) creates its MDIO bus via `devm_of_mdiobus_register`;
   the MCM (mt7530-mdio, `mediatek,mcm`) is a **child of that bus**. This avoids needing
   an MDIO master in `econet_eth.c` (the public fork lacks it — the old blocker).
2. **PHY addresses.** MCM user PHYs at MDIO **1–4** (not 0–3, not 9–12). port0/PHY0 has
   no jack. Confirmed via bootloader `miir` (cable PHY shows BMSR link bit).
3. **ethernet@ reg** must be `<0x1fb50000 0x8000>` (not 0x10000) so it doesn't overlap
   the switch MMIO region (EBUSY otherwise).
4. **Patches 210 + 220** (mcm reset optional, register PHYs before dsa_register_switch).
5. **Tagger.** The real tagger is econet-eth's `gsw/tag-mtk.c` → `mtk-tag.ko` (`=y`), with
   a device-agnostic `mtk_conduit_find_user`. The kernel's `net/dsa/tag_mtk.c`
   (kmod-dsa-mt7530, `=m`) is NOT loaded — don't patch it.
6. **eth0 out of br-lan.** The default config put the DSA conduit `eth0` in `br-lan` (via a
   leftover `bridge-vlan`), whose bridge rx_handler bypassed the DSA tagger → 0 demux.
   Removing eth0 from the bridge was the final unlock (RX demux started, tag `port=1`).
7. **Labels.** Physical jack ↔ Linux port is reversed; fixed in the xr500v.dts
   (port1=lan4 … port4=lan1) so físico LANn = lanN. Added MCM port4/phy4.

End-to-end verified: `ping` 0% loss both ways, WiFi clients online, survives reboot.

## TX throughput investigation (open problem)
Measured with iperf3 (musl-mips binary on the router) against an iperf3 server on a
RPi5 (192.168.68.1, vmbr0 — LAN side, not vmbr1=WAN).

### Firm facts
- Router **TX ~5 Mbps** (UDP TX = 5M; TCP both ways ~5–7M, limited by the slow return
  ACKs traversing TX). **RX (UDP) = 245 Mbps.**
- HW TX egresses ~450 pps (~2.2 ms/packet) on a clean gigabit link (a frame is ~12 µs).
  Packets DO egress (switch `TxBytes` climbs), CPU 90% idle, **zero drops anywhere**
  (eth0 tx_dropped, switch TxDrop, qdisc, collisions, pause = all 0). IRQ ~152/s (fine).

### Hypotheses tested and ruled out (each a build/flash cycle)
1. CPU — idle.  2. generic WED/offload.  3. **cascade TRGMII RD_TAP** — ported
   `macMT7530doP6Cal`; windows are wide/clean (1–45), centering taps changed nothing →
   cascade eye is healthy.  4. **QDMA tail-drop** (250) — no change.  5. ring too small —
   `num_tx_descs=128` (fine).  6. **QDMA egress rate-limit regs** (260; read
   `meter=0x80020fa0 limit=0x7d param=0x40000000`, zeroed them) — no change.

### Localization & next step
TX completion latency ~2.2 ms/packet in the SoC **QDMA/PSE/GDMA TX** pipeline. Not the
cascade, CPU, WiFi, or the registers tried. **Next step (option 1):** systematic
register-level diff of the QDMA/PSE/GDMA init — `econet_qdma.c` (Caleb) vs the OEM
`oem_src/ether/en7512/eth_lan.c` `qdma_reg_init` (which configures `QDMA_TxBufCtrl
chnThreshold=6 totalThreshold=24 mode=ENABLE` and per-queue congestion thresholds that
the OpenWrt driver leaves unset) — instead of more trial patches. Caleb's commit notes
"not currently usable because TRGMII is unstable", so a TX-path gap in the port is plausible.

## References
- OEM TRGMII cal: `oem_src/tcphy/mtkswitch_api_krl.c:5038` (`macMT7530doP6Cal`).
- OEM QDMA init: `oem_src/ether/en7512/eth_lan.c` (`qdma_reg_init`).
- Build VM, flash flow, recovery: `scripts/`.
