# XR500v OpenWrt port

OpenWrt port for the **TP-Link Archer XR500v** GPON router — SoC: EcoNet **EN751221** (MIPS 34Kc, big-endian).

## Status (2026-05-26) — FUNCIONAL ✅

| Function | State |
|---|---|
| Boot to console (UART, A/B slot flash) | ✅ |
| **Ethernet LAN — 4 ports via DSA** | ✅ working (HW switching ~gigabit, CPU idle) |
| PCIe / WiFi (MT7662 / mt76x2, 5 GHz AP) | ✅ |
| **USB** (xHCI MediaTek + storage) | ✅ pendrive USB2 = `/dev/sda` |
| **256 MB RAM** | ✅ (244 MB usable) |
| **LAN TX throughput** | ✅ 161 Mbps como endpoint (fix BQL, antes ~5M) |
| Bridge (4 LAN + WiFi) + persistent config + internet | ✅ |
| WAN / xPON (GPON fiber) | ❌ not supported (separate MAC block) |
| Telephone (2× RJ11 / VoIP) | ❌ not supported (separate SLIC) |

**The breakthrough:** the EN751221 has **two cascaded MT7530 switches** — an on-die one (MMIO @ `0x1fb58000`, only port5 cascade + port6 CPU) and an external MCM one (over MDIO @ `0x1f`) that carries the 4 real LAN ports. Modeling this as a **nested DSA tree** (the MCM as a child of the on-die switch's MDIO bus) makes all 4 LAN ports + WiFi work, bridged, with internet — *without* needing the unpublished MDIO-master code that previously blocked this.

## Hardware
- SoC: EcoNet EN751221 (MIPS 34Kc, BE, ~600 MHz). SPI-NAND 128 MB. **256 MB DDR3**.
- Switch: dual MT7530 — on-die (MMIO `0x1fb58000`) + MCM (MDIO `0x1f`). 4× GE LAN.
- WiFi: MediaTek MT7662 (mt76x2, PCIe). USB: xHCI (MediaTek).
- WAN: GPON (own MAC, not in OpenWrt). Phone: VoIP SLIC (not in OpenWrt).

## Cómo reconstruir (este repo es un OVERLAY sobre cjdelisle/openwrt)

```bash
git clone https://github.com/cjdelisle/openwrt.git && cd openwrt
git checkout f3605b31fb            # base pineada (branch plan-b-nazox1, tag iter84-snapshot)
cp -r <este-repo>/{package,target} .      # aplicar overlay
cp <este-repo>/config.seed .config        # selecciones USB + dieta de kernel + target
./scripts/feeds update -a && ./scripts/feeds install -a
make defconfig && make -j$(nproc)
```

**Base pineada:** `cjdelisle/openwrt` @ `f3605b31fb` (branch `plan-b-nazox1`).
**`config.seed`** captura lo que vive en `.config` (gitignoreado): paquetes USB (`kmod-usb3`, `usb-storage`, `usb-xhci-mtk`, `fs-vfat`, `fs-exfat`) + dieta de kernel (`KALLSYMS`/`DEBUG_INFO` off, necesaria para que el kernel comprimido entre en la partición `kernel1` de 3 MB).

> ⚠️ **Lección aprendida (importante):** correr SIEMPRE `make package/kernel/econet-eth/clean` antes de un build fresco, y **NO dejar directorios de backup** (`*-bak`, `*-disabled`, `*.iter*`) bajo `package/`: OpenWrt escanea todo `package/` y compila esos drivers viejos en paralelo, pisando el bueno en el rootfs (nos costó horas de "se rompió el ethernet").

## Key technical notes
- **Nested DSA topology** — `target/linux/econet/dts/en751221.dtsi`: on-die `switch@1fb58000` con un `mdio { switch@1f (mediatek,mcm) }` hijo. PHYs de usuario del MCM en MDIO **1–4** (port0/PHY0 sin jack RJ45).
- **Port mapping invertido** (carcasa ≠ Linux): físico LAN1→`lan1` (MCM port4), LAN2→`lan2` (port3), LAN3→`lan3` (port2), LAN4→`lan4` (port1). Corregido en `en751221_tplink_archer-xr500v.dts`.
- **USB** — nodo `usb@1fb90000` (mt8173-xhci). El puerto USB3 no tiene T-PHY cableado → `STS1_U3_MAC_RST` nunca sale de reset → `host_enable` timeout `-145`. Fix: `mediatek,u3p-dis-msk = <0x1>` (deshabilita U3; el puerto USB2 queda activo). El driver `xhci-mtk` NO consume `phys`.
- **256 MB** — `memory@0 reg = <0x0 0x10000000>` en el board dts. Requiere disable INITRAMFS + dieta de kernel (sino el kernel comprimido no entra en `kernel1`=3 MB; el `dd conv=sync` redondea al múltiplo de 3072k).
- **Tagger** — econet-eth trae su propio `mtk-tag.ko` (de `gsw/tag-mtk.c`), *no* el `tag_mtk` del kernel. `mtk_conduit_find_user` device-agnostic.
- **Gotcha operativo** — el conduit DSA `eth0` NO debe ser miembro de `br-lan` (su rx_handler de bridge bypasea el tagger DSA). `br-lan` = `lan1..lan4` (+ WiFi).
- **Flash** — solo desde telnet stock OEM `:2323` (mtd write desde OpenWrt corriendo corrompe la NAND). Patch de header trendchip requerido.
- **Red persistente** — el bridge DSA se arma bien solo en **boot limpio**, no con `network restart`.

## econet-eth driver patches (`package/kernel/econet-eth/patches/`)
- `210-mcm-reset-optional` — el MCM no expone reset register; hacerlo opcional.
- `220-mdio-bus-pre-dsa-register` — registrar las PHYs del MCM antes de `dsa_register_switch` (sino `-ENODEV` en los puertos de usuario).
- `240-trgmii-cascade-cal` — port del `macMT7530doP6Cal` TRGMII del SDK OEM.
- `330-bql-min-limit` — **fix de throughput TX**: `dql.min_limit=262144` en las txq del GDM. El BQL colapsaba a ~86 bytes (QDMA señaliza TX-done por paquete) → TX 5M → 161M (32×).

*(Las patches PPE WIP 340/350 están en el historial git, commit `39fb218`. Se quitaron del build funcional: el forwarding ya da ~590M por software, HW NAT offload no es necesario.)*

## Estructura
- `package/kernel/econet-eth/` — driver eth + DSA (patches sobre el fork `cjdelisle/econet_eth` @ `c2f855cf`) + `files/` (init del switch).
- `package/kernel/linux/modules/netdevices.mk` — agrega `TARGET_econet` a deps + define `kmod-dsa-mt7530`.
- `target/linux/econet/` — DTS (SoC dtsi + board XR500v), `en751221/config-6.12`, image recipe, base-files, patches NAND.
- `config.seed` — selecciones de build (sembrar como `.config`).
- `scripts/`, `docs/`, `notes/` — herramientas y log de iteraciones.

Built on EcoNet `econet/en751221` (Caleb DeLisle's port, merged to OpenWrt mainline 2025).
