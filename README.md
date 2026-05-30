# XR500v OpenWrt port

OpenWrt port for the **TP-Link Archer XR500v** GPON router — SoC: EcoNet **EN751221** (MIPS 34Kc, big-endian).

## Status (2026-05-30) — FUNCIONAL ✅

| Function | State |
|---|---|
| Boot to console (UART, A/B slot flash) | ✅ |
| **Ethernet LAN — 4 ports via DSA** | ✅ working (HW switching ~gigabit, CPU idle) |
| **PCIe / WiFi — dual band** | ✅ 5 GHz AP (MT7662 / `mt76x2e`) + 2.4 GHz AP (MT7603 / `mt7603e`) |
| **USB** (xHCI MediaTek + storage) | ✅ pendrive USB2 = `/dev/sda` |
| **256 MB RAM** | ✅ (244 MB usable) |
| **LAN TX throughput** | ✅ 161 Mbps como endpoint (fix BQL, antes ~5M) |
| Bridge (4 LAN + WiFi) + persistent config + internet | ✅ |
| **Telephone / VoIP (RJ11 FXS)** | ✅ working — clean bidirectional SIP calls + ring/answer/hangup (reconstructed SLIC driver) |
| WAN / xPON (GPON fiber) | ❌ not supported (separate MAC block) |

**The breakthrough:** the EN751221 has **two cascaded MT7530 switches** — an on-die one (MMIO @ `0x1fb58000`, only port5 cascade + port6 CPU) and an external MCM one (over MDIO @ `0x1f`) that carries the 4 real LAN ports. Modeling this as a **nested DSA tree** (the MCM as a child of the on-die switch's MDIO bus) makes all 4 LAN ports + WiFi work, bridged, with internet — *without* needing the unpublished MDIO-master code that previously blocked this.

## Hardware
- SoC: EcoNet EN751221 (MIPS 34Kc, BE, ~600 MHz). SPI-NAND 128 MB. **256 MB DDR3**.
- Switch: dual MT7530 — on-die (MMIO `0x1fb58000`) + MCM (MDIO `0x1f`). 4× GE LAN.
- WiFi: **two** MediaTek PCIe radios — MT7662 (5 GHz, `mt76x2e`) + MT7603 (2.4 GHz, `mt7603e`). USB: xHCI (MediaTek).
- WAN: GPON (own MAC, not in OpenWrt). Phone: Microsemi **Le9642** SLIC over ZSI — reconstructed driver (see VoIP section).

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
- **WiFi 2.4 GHz (MT7603)** — a *second* PCIe radio, separate from the 5 GHz MT7662. Two walls: enumeration needed the OEM global PCIe reset replicated at boot (the `mt7512_pcie_reset` sequence), and the `mt7603e` driver hung on the MCU EEPROM upload until a synthetic EEPROM was inlined in the DTS (`mediatek,eeprom-data` on the `wifi@0,0` node). 5 GHz (MT7662) runs on `mt76x2e`.
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

## VoIP / Telephone (FXS)  ✅

The XR500v's RJ11 phone jacks use a **Microsemi Le9642** SLIC (VE886/VP886 family)
over **ZSI** (Zarlink Serial Interface, multiplexed on the PCM bus) — "not
supported" by the EcoNet OpenWrt project. The full FXS/VoIP stack was reverse-
engineered and rebuilt from scratch: the SoC PCM/TDM DMA engine, the ZSI
transport, the SLIC profiles / line-state / ring, G.711 audio, and a SIP
integration. A point-to-point SIP call (PC softphone ↔ Philips DECT handset)
works clean **both directions**, with real **ring / answer / hangup**.

`package/kernel/econet-pcm/` builds two kernel modules — **`pcm-en751221`** (the
SoC PCM/TDM controller @ `0x1fbd0000`) and **`econet-slic`** (the Le9642 over ZSI
@ `0x1fbd1000`) — plus a baresip audio module exposing `/dev/xr500v-voice`
(16-bit linear, 8 kHz) and `xr500v-callmgr.c`, a small userspace daemon for the
call flow.

### Key technical notes
- **DC feed current** — the cordless DECT base needs more loop current than the
  600R AC profile default (ILA 0x07 = 25 mA); the mic stays marginal at the
  voice ADC until raised. `slic_audio_setup` bumps the DCFEED ILA to 38 mA
  (`feed_ila` param) — this alone removed the capture noise and hum.
- **u-law, not 16-bit linear** — the Le9642 drives only 8 bits per PCM timeslot
  even in "linear" mode, so 16-bit linear gave 8-bit resolution (quiet speech
  quantized to silence → choppy). The codec runs **G.711 u-law** and the voice
  thread (de)compands to/from 16-bit linear for the char device; u-law's log
  companding preserves quiet speech. The SLIC uses different clock-slot offsets
  for TX-capture (low byte) vs RX-playback (high byte) — the `tx_msb` param
  selects the playback byte.
- **OPFUNC before TXSLOT** — the codec mode must be written before the timeslots
  latch, or only one 8-bit slot is allocated.
- **Real call flow** (`xr500v-callmgr`) — ties the SLIC hook (debugfs) to
  baresip's `ctrl_tcp`: an incoming call rings the handset, going off-hook
  accepts, on-hook hangs up (no leftover line tone). The ring voltage (STATE
  0x07, 70–90 V) corrupts the SIGREG hook bit, so it rings with a cadence and
  samples the hook only in the gaps, debounced.
- **Persistence** — modules reload live without reflashing (the SLIC state only
  resets on a cold boot). The whole stack (fixed `.ko` + baresip + callmgr)
  persists via the UBIFS overlay (`/lib/modules`, `/root/bsdeploy`,
  `/root/voip-start.sh` launched from `rc.local`) — no firmware reflash needed.

The SLIC bring-up history (ZSI handshake, profiles, the ring milestone, the
codec saga) lives in the git history and `notes/`.

## Estructura
- `package/kernel/econet-eth/` — driver eth + DSA (patches sobre el fork `cjdelisle/econet_eth` @ `c2f855cf`) + `files/` (init del switch).
- `package/kernel/econet-pcm/` — VoIP/FXS: `pcm-en751221` (PCM/TDM) + `econet-slic` (Le9642 over ZSI) kernel modules, the baresip `xr500v` audio module, and `xr500v-callmgr.c` (the ring/answer/hangup daemon).
- `package/kernel/linux/modules/netdevices.mk` — agrega `TARGET_econet` a deps + define `kmod-dsa-mt7530`.
- `target/linux/econet/` — DTS (SoC dtsi + board XR500v), `en751221/config-6.12`, image recipe, base-files, patches NAND.
- `config.seed` — selecciones de build (sembrar como `.config`).
- `scripts/`, `docs/`, `notes/` — herramientas y log de iteraciones.

Built on EcoNet `econet/en751221` (Caleb DeLisle's port, merged to OpenWrt mainline 2025).
