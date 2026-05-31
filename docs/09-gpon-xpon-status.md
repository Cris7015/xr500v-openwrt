## Summary

GPON is the one major subsystem of the Archer XR500v that does **not** work under the OpenWrt port. The key fact for this subsystem is that this is not for lack of source code: the OEM xPON/GPON driver for the EN751221 exists as full, readable C in the same 2.6.36 `tclinux_phoenix` OEM tree the [VoIP/FXS driver](06-voip-fxs-telephony.md) was reconstructed from — roughly 55,000 lines across `xpon` (~43,700 LOC) and `xpon_phy` (~11,700 LOC), including a ~210 KB MAC register header (`epon_mac_reg_c_header_en7521.h`) with ~1,574 register definitions for exactly this chip, and covering both EPON (MPCP) and GPON (OMCI) modes. GPON is unported because of scale and testability, not missing or blob code:

1. **Scale** — the optical stack is on the order of 25-30x the effort the VoIP bring-up took (MAC + PHY/SerDes + laser calibration + MPCP/OMCI + real-time upstream TDMA synchronization).
2. **A head-end requirement** — a GPON ONU cannot be brought up on a desk; it needs an OLT to negotiate with, plus an ISP-side registration (ONU serial + password) to authenticate.

The OpenWrt port already declares the reset plumbing for the block (the SoC's `XPON_MAC_RST`/`XPON_PHY_RST` resets are listed on the Ethernet node, and the PON MAC's interrupt is part of the shared QDMA/Ethernet interrupt model), but no driver consumes it. Upstream (Caleb DeLisle / `cjdelisle`) keeps PON officially in scope for the EcoNet EN751221 project while flagging it as "very risky... possibly unrealistic," with every milestone unstarted.

## What "GPON" means on this device

The XR500v is an OEM Home Gateway (HGW) with an optical WAN: a GPON ONU (Optical Network Unit) on the fiber port, not a copper Ethernet WAN. The OEM firmware confirms this — the stock `eth.ko` defaults bake in `wan_itf=nas10`, i.e. the WAN interface is the PON-attached NAS interface, **not** an Ethernet port (see [OEM recon](10-stock-firmware-access.md)). The optical path is a fundamentally different data plane from the LAN side: where the LAN goes through the [nested dual MT7530 switch](04-ethernet-dsa.md), the WAN side is the EN751221's on-die **xPON MAC** fed by an **xPON PHY/SerDes** driving the optical transceiver (laser + photodiode).

The front panel reflects this: the board DTS defines a dedicated `green:gpon` LED on GPIO 2 (`led-2 { label = "green:gpon"; gpios = <&gpio 2 GPIO_ACTIVE_LOW>; linux,default-trigger = "default-on"; }`), which under OpenWrt is a static `default-on` placeholder since nothing drives PON link state.

## The OEM driver source exists (and is not blobs)

The most important fact for this subsystem: the GPON driver is **available as source**, not as opaque firmware blobs. It lives in `cjdelisle/EN751221-Linux26` — a mirror of the OEM `tclinux_phoenix` 2.6.36 tree for the EN751221, the same tree that yielded `le9641.c` for the VoIP work.

Location and scale of the optical stack within that tree:

| Component | Path | Size | Role |
|---|---|---|---|
| xPON MAC driver | `modules/private/xpon/` | 36 `.c` + 49 `.h`, ~43,700 LOC | MAC driver (`src/xpondrv.c` ~2,226 LOC), `ponmgr` daemon, `pon_vlan` lib |
| xPON PHY driver | `modules/private/xpon_phy/` | ~11,700 LOC | SerDes / PON-PHY / laser driver |
| MAC register map | `inc/epon/epon_mac_reg_c_header_en7521.h` | ~210 KB, ~1,574 defines | Complete MAC register header **for the en7521 (= EN751221, this chip)** |
| EPON path | `src/epon/epon_mpcp.c`, `epon_main.c` | — | MPCP (Multi-Point Control Protocol) for EPON |
| GPON path | `src/gpon/gpon_omcis.c`, `omci_oam_monitor.c`, `gpon_power_management.c` | — | OMCI (ONT Management and Control Interface) for GPON |

So both PON modes (EPON and GPON) are present in source. There are also pre-compiled `.ko` artifacts in the OEM image (`xpon.ko`, `ponvlan.ko`, `xpon_igmp.ko`), but the corresponding source is in the tree — the situation is analogous to the SLIC: the register map and the control logic are readable, so this is a port task in principle, not a black-box RE task.

The MAC register header is the GPON analogue of the SLIC's VP-API — it is the full map of MAC registers for the exact silicon in this device. That removes the single biggest unknown the VoIP work had to fight (in the VoIP case, the PCM controller body shipped only as a blob and had to be reconstructed from its headers).

## Why it remains unported

Having the source does **not** make GPON cheap. Two independent walls keep it impractical.

### Wall 1 — scale (~25-30x the VoIP effort)

The VoIP bring-up was a control-plane driver: a ~2K-LOC SLIC driver plus G.711 companding, talked to over a serial bus, with the hardest single piece being the PCM/TDM DMA engine. GPON is a **real-time optical stack** with several large, interdependent pieces, each of which is roughly the size of the entire VoIP project or larger:

- **xPON MAC** — framing, GEM port / T-CONT handling, the DBA (Dynamic Bandwidth Allocation) interface, queue/scheduling.
- **xPON PHY / SerDes** — bring-up and tuning of the high-speed serial lanes to the optical front-end.
- **Laser calibration** — driving the optical transmitter: bias/modulation current calibration, typically with per-unit calibration data in flash, an I²C-attached laser driver, and an SFP/diagnostics-style monitoring model (the EcoNet hardware notes describe EN7570/EN7571-class laser drivers and an SFP-8472-style upstream model).
- **MPCP (EPON) / OMCI (GPON)** — the management/registration protocols the ONU uses to be discovered and provisioned by the OLT.
- **Upstream TDMA synchronization** — the ONU may only transmit in time slots the OLT grants. Getting upstream burst timing right is a hard real-time constraint with no software analogue on the LAN side.

The VoIP bring-up, for comparison, ran ~13 documented cycles before the first audible ring. GPON is a much deeper stack, and several of its pieces (laser calibration, upstream TDMA) cannot be iterated by register pokes the way the SLIC ZSI handshake was.

### Wall 2 — it cannot be tested on a desk

This is the decisive blocker. A GPON ONU is half of a point-to-multipoint optical link; the other half is the **OLT** (Optical Line Terminal) head-end in the ISP's network. The ONU only becomes useful once it has:

1. An **OLT to negotiate with** — without a head-end there is nothing to range against, no grants, no link. The upstream framing is "build, lease, or borrow" an OLT.
2. An **ISP registration** — GPON ONUs authenticate to the OLT, typically with an ONU/ONT **serial number + password** provisioned on the operator side. Bringing the optical WAN up means presenting credentials the operator has on file for a registered ONU.

So even a perfectly ported driver could not be validated without an OLT plus operator cooperation. This is categorically different from every other subsystem on this device — LAN, WiFi, USB, and even VoIP were all testable in isolation on the bench. GPON is not.

## Upstream stance (cjdelisle / EcoNet EN751221 project)

PON **is** in the official scope of the EcoNet EN751221 OpenWrt project. The project's stated goal is "everything except DSL and VoIP" — which is precisely why VoIP was reconstructed independently here. But the maintainer explicitly marks the optical work as **"very risky, particularly the xPON... possibly unrealistic,"** and all the PON milestones are tagged `:soon:` (i.e. unstarted). The relevant design notes live in the `cjdelisle/econet-linux-wiki` under `hardware/EN7523/PON.md`, covering the optical chain, the EN7570/EN7571 laser drivers, and the SFP-8472 upstream model. A sibling-chip reference for the same driver family exists in the ARM-based EN7581 GPL drop (`cjdelisle/EcoNet-IOPSYS-GPL-5.4.55`, `arch/arm/mach-econet/ecnt_xpon.c`).

In short: this is not a closed door upstream — it is an open but very expensive one that nobody has walked through.

## What is already plumbed in the OpenWrt port

The port does not implement GPON, but it does **declare the SoC-level resets** for the block, because the PON MAC physically hangs off the same Ethernet/QDMA complex as the GMACs. In `target/linux/econet/dts/en751221.dtsi`, the `ethernet@1fb50000` node lists the xPON resets alongside the frame-engine and QDMA resets:

```dts
ethernet: ethernet@1fb50000 {
    compatible = "econet,en751221-eth";
    reg = <0x1fb50000 0x8000>;

    resets = <&scuclk EN751221_FE_RST>,
             <&scuclk EN751221_FE_QDMA1_RST>,
             <&scuclk EN751221_FE_QDMA2_RST>,
             <&scuclk EN751221_XPON_MAC_RST>,
             <&scuclk EN751221_XPON_PHY_RST>;
    reset-names = "fe", "qdma0", "qdma1",
                  "xpon-mac", "xpon-phy";
    ...
};
```

Notes on this:

- `EN751221_XPON_MAC_RST` and `EN751221_XPON_PHY_RST` are the SoC reset lines for the xPON MAC and xPON PHY respectively, driven through the SCU clock/reset controller (`scuclk` at `0x1fb00000`). These macro definitions come from the **mainline kernel** `dt-bindings` for the EN751221 SCU (downloaded at build time), not from this overlay repo — the overlay only references them.
- The PON MAC sharing the Ethernet/QDMA region is why these resets sit on the Ethernet node rather than on a separate PON node: the frame engine, the two QDMAs, and the PON MAC are one hardware complex. The PON-MAC interrupt is part of the same QDMA/Ethernet interrupt model (the OEM sources reference a `GPON_INT` source on the QDMA for this reason; this is awareness in the shared interrupt model, not a functioning PON path). The [econet-eth driver](04-ethernet-dsa.md) brings these resets out of assert as part of Ethernet init but does nothing PON-specific.
- There is **no** xPON MAC node, **no** xPON PHY node, **no** laser/SerDes node, and **no** PON driver in the port. Nothing consumes the optical hardware. The `green:gpon` LED in the board DTS is a static placeholder.

That is the full extent of what is wired in: the reset lines are named and asserted as a side effect of Ethernet bring-up, and the interrupt source is part of the shared QDMA model. Everything above the SoC-reset level — MAC, PHY, laser, MPCP/OMCI, TDMA — is absent.

## Status and outlook

GPON is best described as out of reach for now, not "almost there." The source advantage is real and meaningful (the same advantage that made VoIP tractable), but it is outweighed by raw scale and by the hard physical requirement of an OLT plus an operator-side registration. Unlike every other subsystem documented on this wiki, no amount of bench iteration can validate a GPON port — it needs head-end equipment and ISP cooperation that a hobbyist RE effort does not have. The plumbing the port carries today (`XPON_MAC_RST`/`XPON_PHY_RST`, and the shared QDMA interrupt model that includes the PON MAC) is the correct foundation if anyone ever does attempt it, but the subsystem should be considered unported and, realistically, the least likely of the remaining gaps to be closed.

## Cross-references

- [Ethernet / DSA](04-ethernet-dsa.md) — the econet-eth driver, QDMA, and the nested switch the LAN side uses (the PON MAC shares this complex).
- [VoIP / FXS SLIC](06-voip-fxs-telephony.md) — the subsystem that demonstrated the "OEM source exists, port it" approach; the GPON driver lives in the same 2.6.36 OEM tree.
- [OEM firmware recon](10-stock-firmware-access.md) — `wan_itf=nas10` and the rest of the OEM WAN/PON configuration.
- [Overview / specs](Home.md) — where GPON sits in the overall subsystem status table.