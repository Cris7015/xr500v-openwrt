## Summary

> **2026-07-10 update:** the upstream situation changed materially after this
> page was written.  Merbanan's `econet-eth-mainline` branch now contains
> compile-tested EN7512/EN7521 PON PHY, EN7570/71/72 LDDLA, and GPON/EPON MAC
> drivers, including a PLOAM O1-O5 state machine.  The branch still lacks a
> functioning WAN QDMA data path and OMCI stack; its GPON/EPON DT nodes remain
> disabled and its xPON MAC IRQ is explicitly a placeholder.  A read-only live
> probe on this XR500v confirmed the PON PHY CSR block at `0x1faf0000`, GPON mode,
> inactive TX, and no RX sync.  See
> [`notes/2026-07-10-gpon-no-olt-phase0.md`](../notes/2026-07-10-gpon-no-olt-phase0.md)
> for the raw register snapshot, EN7570 calibration endian issue and the OEM
> WAN-QDMA interrupt/callback model.  Phase 1 then enabled the standard
> MT7621-compatible I2C block and passively identified the optical LDDLA at
> address `0x70` as **EN7570, silicon ID `0x03`, variant `0x01`**.  That work
> also found and fixed a real big-endian FIFO packing bug in `i2c-mt7621.c`.
> See
> [`notes/2026-07-10-gpon-no-olt-phase1-en7570.md`](../notes/2026-07-10-gpon-no-olt-phase1-en7570.md).
> Phase 2 cross-checked the OEM status routine and added passive reads for raw
> LOS, rogue-ONU, Tx-SD and Tx-fault bits.  The live unit reports Tx-fault set,
> with rogue-ONU and Tx-SD clear.  Its uninitialised LOS bit is **not** evidence
> of an optical signal.  See
> [`notes/2026-07-10-gpon-no-olt-phase2-passive-status.md`](../notes/2026-07-10-gpon-no-olt-phase2-passive-status.md).
> Phase 3 made the xPON MMIO probe a persistent DT platform driver, extended
> the TX safety snapshot, and identified the OEM physical TX-disable gate as
> active-high GPIO16.  The live router now confirms both independent inhibits:
> GPIO16 is an asserted output and `PHYSET3.TXEN` is clear.  An audit also shows
> why Merbanan's current PON-PHY driver cannot be loaded as a passive probe: its
> `probe()` performs mode/reset/counter/IRQ writes.  See
> [`notes/2026-07-10-gpon-no-olt-phase3-xpon-platform-baseline.md`](../notes/2026-07-10-gpon-no-olt-phase3-xpon-platform-baseline.md).
> Phase 4 split the OEM/Merbanan combined init and produced a compile-only,
> fail-closed RX stage.  Its sole PHY operation clears the signal-detect
> deglitch bit while physically asserting GPIO16 TX-disable and forcing TXEN
> low.  The module is intentionally absent from the shipping image, autoload
> and DTB, and was not loaded on the router.  See
> [`notes/2026-07-11-gpon-no-olt-phase4-rx-init-compile-only.md`](../notes/2026-07-11-gpon-no-olt-phase4-rx-init-compile-only.md).
> Phase 5 ran that isolated stage on the lab router without fibre.  The sole
> write changed `PHYSET3` from `0x4581e114` to `0x4581e110`, leaving TXEN low,
> GPIO16 TX-disable asserted, and every TX generator and xPON interrupt off.
> Module removal restored `0x4581e114`; the complete before/after register dumps
> were identical, EN7570 recorded no writes, and PPPoE stayed operational.  The
> router was finally restored to the stable phase-3 image, where the active
> module is absent.  See
> [`notes/2026-07-11-gpon-no-olt-phase5-esd-active-rollback.md`](../notes/2026-07-11-gpon-no-olt-phase5-esd-active-rollback.md).
> Phase 6 isolated the OEM EN7570 receive LOS/SD polarity.  The opt-in module
> changed only `XPON_SETTING` bit 6 (`0x14f` to the OEM EN7570 value `0x10f`),
> while `PHYSET3`, all TX gates, EN7570 I2C state and PPPoE remained unchanged.
> Removal restored `0x14f` and the complete before/after register dumps were
> identical.  Without downstream light this proves safe reversibility, not yet
> which polarity is functionally correct, so the shipping image still retains
> `0x14f`.  See
> [`notes/2026-07-11-gpon-no-olt-phase6-en7570-rx-polarity.md`](../notes/2026-07-11-gpon-no-olt-phase6-en7570-rx-polarity.md).
> Phase 7 expanded the persistent read-only map across GPON synchronisation and
> FEC.  The live block already retains coherent PSYNC/superframe thresholds,
> has its descrambler and FEC decoder enabled, and has all test/reset controls
> clear; only receive counters are disabled.  Repeated snapshots were identical,
> so no further write was justified without downstream light.  See
> [`notes/2026-07-12-gpon-no-olt-phase7-rx-digital-baseline.md`](../notes/2026-07-12-gpon-no-olt-phase7-rx-digital-baseline.md).
> Phase 8 connected a live Movistar GPON fibre while physical TX-disable stayed
> asserted.  Neither polarity, ESD deglitch nor enabled RX counters produced a
> codeword or sync.  A same-boot fibre connected/disconnected EN7570 comparison
> was identical: its LOS calibration/ADC path has not been initialised, so the
> optical signal is not yet visible to the digital PHY.  The next boundary is a
> strictly RX-only, rollback-capable subset of EN7570 LOS analogue setup.  See
> [`notes/2026-07-12-gpon-phase8-live-fibre-rx-boundary.md`](../notes/2026-07-12-gpon-phase8-live-fibre-rx-boundary.md).
> Phase 9 implemented that isolated LOS prototype, but phase 10's first
> fibre-disconnected run proved the EN7570 calibration trigger is
> non-transactional: visible controls rolled back, while LOS state, an
> autonomous status byte and its timeout survived even a software reboot.
> Only a physical power cycle restored baseline, so the stage is now hard
> quarantined.  Phase 11 then separated the OEM init sequence by dependency:
> ADC/RSSI calibration feeds DDMI and ERC/MPD belongs to TX; neither is a LOS
> data dependency.  The likely missing boundary is the EN7570 whole-device
> reset state, which remains too broad for a live test.  Pointer-only reads
> established its retained TIAMUX, LA_PWD, bandgap, ERC and reset baseline.
> See
> [`notes/2026-07-12-gpon-phase10-en7570-los-nontransactional.md`](../notes/2026-07-12-gpon-phase10-en7570-los-nontransactional.md)
> and
> [`notes/2026-07-12-gpon-phase11-en7570-dependency-audit.md`](../notes/2026-07-12-gpon-phase11-en7570-dependency-audit.md).

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

### Wall 2 — it cannot be fully tested on an isolated desk

This is the decisive blocker for end-to-end work. A GPON ONU is half of a point-to-multipoint optical link; the other half is the **OLT** (Optical Line Terminal) head-end in the ISP's network. The ONU only becomes useful once it has:

1. An **OLT to negotiate with** — without a head-end there is nothing to range against, no grants, no link. The upstream framing is "build, lease, or borrow" an OLT.
2. An **ISP registration** — GPON ONUs authenticate to the OLT, typically with an ONU/ONT **serial number + password** provisioned on the operator side. Bringing the optical WAN up means presenting credentials the operator has on file for a registered ONU.

The lab now has intermittent access to the live Movistar fibre drop.  It is
not a controlled OLT, so GPON still cannot be validated in isolation the way
LAN, WiFi, USB or VoIP can.  Phase 23 did establish that the stock firmware's
existing ONU identity is accepted on that drop: stock reached O5, completed
OMCI activity and established its PPPoE service.  This provides a real
end-to-end oracle, but it does not authorise arbitrary OpenWrt transmission or
replace the missing OpenWrt PLOAM, OMCI and WAN-QDMA integration.

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
- There is now a diagnostic-only PON-I2C node at `0x1fbf8000` and a passive
  EN7570 client at address `0x70`.  The client reads silicon ID/variant plus
  the raw OEM LOS, rogue-ONU, Tx-SD and Tx-fault status bits, plus passive
  reset/RX-front-end/ADC/LOS/ERC context.  It has no reset, initialisation,
  calibration, latch-clear, laser, APD, ADC or DDMI write path.
  Its current passive map also inventories MPD targets, burst/TGEN controls,
  P0/P1 current-loop controls and APD state so a future whole-device-reset
  experiment can require a known fail-closed precondition.
  Because the analogue block remains uninitialised, these states are not a
  claim of optical link.  There is still **no** functional xPON MAC node, xPON
  PHY/SerDes driver, PLOAM data path or OMCI stack.  The `green:gpon` LED
  remains a static placeholder.
- A second diagnostic-only platform node owns the xPON PHY window at
  `0x1faf0000`.  It persistently snapshots mode/FSM/sync, analogue status,
  interrupt state and all known TX generators without writing or latching
  anything.  It also reserves GPIO16 `TX_DISABLE` with `GPIOD_ASIS`; the live
  state is output-high/asserted while `PHYSET3.TXEN`, PRBS, test-frame and
  rogue-test enables are clear.  This is the fail-closed baseline for a later
  RX-only init prototype, not an active PHY driver.
- A separate compile-only RX-init package exists outside the device image.  It
  requires independent module-parameter and DT opt-ins plus asserted physical
  TX-disable.  Its mutually exclusive stages cover the reversible
  `PHYSET3.ESD_PRO` clear, EN7570 RX polarity, passive RX counters, and an
  isolated EN7570 receive/LOS setup.  Hardware testing of the latter proved
  that its calibration trigger changes autonomous state which visible-register
  rollback and even a software reboot do not undo.  That stage is now
  quarantined and always returns `-EOPNOTSUPP`; the earlier ESD, polarity and
  counter stages remain guarded.  The factory thresholds (`0x1c/0x10`) remain
  documented, and the passive diagnostic now reads `SVADC_PD`.  That RX-init
  package has no APD, laser, TGEN, Tx-SD, DDMI, reset, MAC or interrupt path. No
  experimental DT compatible or opt-in is present in shipping firmware.
  A still-separate reset-audit package models non-transactional EN7570
  experiments.  It requires an exact phase-12 baseline, independent module/DT
  gates and all TX inhibits, and self-pins before its first data write.  It has
  no shipping DT match or autoload; its guarded modes have only been installed
  manually in audited temporary images.
  The reset observer was subsequently executed once from the exact phase-12
  baseline.  Its OEM four-byte trigger succeeded and self-cleared without
  changing any of 28 visible EN7570 groups or the TX safety state.  A required
  physical power cycle and a passive-image sysupgrade restored the normal DT;
  the reset-only experiment must not be repeated.  It does not yet show whether
  reset clears phase-10 autonomous LOS residue, because dirty state was not
  recreated.  See
  [`notes/2026-07-13-gpon-phase14-en7570-reset-live.md`](../notes/2026-07-13-gpon-phase14-en7570-reset-live.md).
  Phase 16 then executed a distinct reset-then-LOS observer.  Reset again left
  all 28 visible groups unchanged; the five isolated LOS writes succeeded and
  produced the same autonomous LOS/timeout state as phase 10 while every TX
  barrier remained safe.  This proves phase 10 failed its rollback model, not
  LOS programming, and closes the no-fibre reset dependency question.  The
  next useful boundary is an initialized live-fibre RX-only A/B test.  See
  [`notes/2026-07-13-gpon-phase16-reset-then-los-live.md`](../notes/2026-07-13-gpon-phase16-reset-then-los-live.md).
  Phase 17 performed that A/B test: connected and disconnected fibre produced
  the same EN7570 LOS state.  OEM stock then reached `RX_SYNC=0xa`, active
  GPON/OMCI traffic and service on the same fibre, proving the physical path and
  OLT are good.  A full stock EN7570 dump matched all three programmed LOS
  blocks but exposed the strongest omitted stable receiver candidate,
  `LA_PWD[18:16]=5`.  The compile-only observer now has a third guarded mode
  which isolates exactly that one RSSI-gain RMW before the proven LOS sequence;
  it still has no ADC/RSSI calibration, APD, current, TGEN, laser, MAC or QDMA
  path.  See
  [`notes/2026-07-13-gpon-phase17-live-fibre-oem-oracle.md`](../notes/2026-07-13-gpon-phase17-live-fibre-oem-oracle.md)
  and
  [`notes/2026-07-13-gpon-phase18-rssi-gain-los-compile-only.md`](../notes/2026-07-13-gpon-phase18-rssi-gain-los-compile-only.md).
  Phase 19 executed that exact seven-write observer once with live fibre.  The
  RSSI RMW was perfectly isolated and all TX gates passed, but the LOS block
  still entered `LOS_DBG[3]=0x89` with timeout `0x3e`.  Connected,
  disconnected and reconnected 20-sample series were identical in those state
  fields.  A warm reboot retained the external EN7570 state; only the required
  physical power cycle restored the cold baseline.  The router was finally
  returned to the passive DT with no experimental module or opt-in.  This
  closes static RSSI gain as the missing dependency and moves the next
  boundary into a specifically justified RX analogue calibration prerequisite.
  See
  [`notes/2026-07-13-gpon-phase19-rssi-gain-los-live.md`](../notes/2026-07-13-gpon-phase19-rssi-gain-los-live.md).
  Phase 20 adds a fourth, still compile-only and mutually exclusive observer
  mode for the last bounded RX-only prerequisite before a separate APD safety
  study.  It reproduces only the OEM transient RSSI Vref/V sampling, validates
  the same-device ADC oracle, restores and directly reads back its three
  control groups, then permits the audited static gain and LOS sequence only
  if every transfer and TX gate succeeds.  Its maximum is 15 writes.  Directed
  readbacks between calibration and LOS avoid erasing a possible transient
  effect, while the previous static-gain mode retains its full intermediate
  snapshot.  This mode has no APD, full ADC-bandgap calibration, ERC/MPD,
  current, laser, xPON MMIO, MAC or QDMA path.  See
  [`notes/2026-07-13-gpon-phase20-rssi-calibration-gain-los-compile-only.md`](../notes/2026-07-13-gpon-phase20-rssi-calibration-gain-los-compile-only.md).
  Phase 21 executed that exact 15-write observer once.  Vref `0x020a`, V
  `0x0285` and delta `0x007b` reproduced the same-device stock oracle, proving
  that the transient RSSI calibration itself ran correctly.  Nevertheless,
  connected, disconnected and reconnected fibre all produced 20/20 asserted
  LOS samples with `LOS_DBG[3]=0x89` and timeout `0x3e`.  The passive image was
  restored; a warm reboot again retained the external-chip state and the
  required physical power cycle restored the exact cold baseline.  This
  closes reset, transient RSSI calibration, static gain and LOS as sufficient
  prerequisites.  Any APD experiment is a separate high-voltage safety phase
  which must begin with a read-only rail/register/failure-mode audit.  See
  [`notes/2026-07-13-gpon-phase21-rssi-calibration-gain-los-live.md`](../notes/2026-07-13-gpon-phase21-rssi-calibration-gain-los-live.md).
  Phase 22 audited the separate APD/high-voltage boundary without issuing an
  APD or other register-data write.  The exact factory matrix yields the OEM
  initial code `0xa2`; the functional stock dump later contained
  `b3 09 20 00`.  The OEM APD core is only soft-start, control-enable and one
  DAC byte, but it has no OVP handling, validated clamp or software shutdown.
  A current merbanan proposal also reverses this board's hot/cold slope words
  and reads the wrong legacy step offset, so it must not be copied literally.
  The passive EN7570 diagnostic now exposes all 193 aligned groups from
  `0x000` through `0x300`.  Live comparison found 176 identical groups and 17
  known differences: TX controls/state, receiver operations already tested,
  and APD; it found no hidden stable RX prerequisite.  The router remained at
  the exact cold APD/TX baseline.  APD is now a credible, narrowly modelled
  receiver dependency, but live high-voltage testing remains gated on a stock
  transition/electrical safety oracle.  See
  [`notes/2026-07-13-gpon-phase22-apd-safety-audit-passive-map.md`](../notes/2026-07-13-gpon-phase22-apd-safety-audit-passive-map.md).
  Phase 23 cold-booted stock on the authorised live fibre and established the
  missing APD transition oracle.  A directed read found `b1 09 20 00`, then
  90/90 samples held `b2 09 20 00`; all 91 OVP samples were zero and the
  codes exactly match the factory temperature equation.  More importantly,
  stock held `RX_SYNC=0xa` and `G_ACTIVATION=0x5` (O5), completed OMCI
  activity and established PPPoE over the PON-backed `nas1_0`.  Thus the
  fibre, ONU registration and stock data path are proven end to end.  No
  OpenWrt APD write occurred; OVP zero is a software-latch oracle, not an
  electrical rail measurement.  See
  [`notes/2026-07-13-gpon-phase23-stock-apd-o5-oracle.md`](../notes/2026-07-13-gpon-phase23-stock-apd-o5-oracle.md).
  Phase 24 then executed a new APD-only observer exactly once with fibre
  disconnected.  It reproduced the per-unit OEM bootstrap
  `00 08 00 00 -> 00 08 20 00 -> 00 09 20 00 -> a2 09 20 00` with three
  successful transfers and exact pre/post readback.  Nine ordered OVP reads
  remained zero; all 16 analogue/TX/reset guards remained unchanged, while
  GPIO16 stayed asserted, the Ibias/Imod control registers stayed zero and
  all observed xPON TX/test/IRQ gates stayed inactive.  A required physical
  power cut restored the cold EN7570 state, after which the passive image and
  normal DT were restored and verified through another cold boot.
  This validates only the isolated initial APD bootstrap: fibre reception,
  physical rail voltage, the thermal worker and complete PHY bring-up remain
  unproved.  See
  [`notes/2026-07-13-gpon-phase24-apd-a2-live.md`](../notes/2026-07-13-gpon-phase24-apd-a2-live.md).

That is the full extent of what is wired in: the reset lines are named and asserted as a side effect of Ethernet bring-up, and the interrupt source is part of the shared QDMA model. Everything above the SoC-reset level — MAC, PHY, laser, MPCP/OMCI, TDMA — is absent.

## Status and outlook

GPON is still unported, but no-OLT bench work is useful for identifying and
validating the individual hardware blocks.  It has confirmed the xPON PHY CSR
window and the EN7570 control interface without enabling OpenWrt TX.  The live
drop now supplies a stock end-to-end oracle: the OEM system reaches O5, runs
OMCI and carries PPPoE service with its authorised identity.  OpenWrt has now
also reproduced the minimum initial APD sequence in isolation while retaining
all observed TX barriers, but it has not yet demonstrated optical reception.
Reproducing the stock outcome still requires a complete, safe receiver/PHY
bring-up, thermal APD policy, PLOAM, burst timing, GEM/OMCI and WAN-QDMA
integration.  The current probes are a sound foundation, not yet a working
OpenWrt optical WAN.

## Cross-references

- [Ethernet / DSA](04-ethernet-dsa.md) — the econet-eth driver, QDMA, and the nested switch the LAN side uses (the PON MAC shares this complex).
- [VoIP / FXS SLIC](06-voip-fxs-telephony.md) — the subsystem that demonstrated the "OEM source exists, port it" approach; the GPON driver lives in the same 2.6.36 OEM tree.
- [OEM firmware recon](10-stock-firmware-access.md) — `wan_itf=nas10` and the rest of the OEM WAN/PON configuration.
- [Overview / specs](Home.md) — where GPON sits in the overall subsystem status table.
