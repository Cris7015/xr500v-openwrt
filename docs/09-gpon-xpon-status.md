## Summary

> **2026-07-26 offline WAN-QDMA lifecycle update:** `econet-eth` r2 now keeps
> one logical xPON consumer across WAN-QDMA provider absence/reprobe, assigns a
> fresh attachment generation on both reprobe and re-registration, masks each
> delivered cause bit before its hard-IRQ callback, and exposes an explicit
> source-drain-then-rearm API. Teardown now masks/ACKs and synchronizes only
> successfully requested IRQs before NAPI/page-pool destruction, including
> partial-probe failures. A matching non-autoloaded observer r2 intentionally
> remains one-shot because it cannot drain the PHY source. Strict checks,
> static symbol inspection and two clean Linux-6.12.80 MIPS builds passed with
> identical direct-build hashes. The modules were packaged but not installed
> or loaded; no PON MAC/PHY, FIFO, PLOAM or optical-TX path is connected. See
> [`notes/2026-07-26-en751221-xpon-qdma-lifecycle-rearm-offline.md`](../notes/2026-07-26-en751221-xpon-qdma-lifecycle-rearm-offline.md).
>
> **2026-07-26 offline PLOAM r3 update:** the hardware-independent lab core now
> propagates callback errors, rolls back logical/dedup state for identical-copy
> retries, implements the EN751221 OEM upstream repetition and Alloc-ID
> contracts, retains one AES key/index across a partial two-fragment send, and
> checks EqD overflow. Exact O1–O7, failure-injection and wire-vector tests now
> execute on the host from the same three source files, including ASan/UBSan,
> instead of merely compiling into a MIPS module. Two clean Linux-6.12.80 MIPS
> builds reproduced `.ko` SHA-256
> `54fc080e79d30c2efdc58ddff0242116e98bb2e0fde45c535bd614ae889c69aa`.
> The module still has no hardware path and was not loaded. Physical transition
> atomicity remains a blocker; the follow-on QDMA lifecycle checkpoint closed
> its compile-only reprobe/remove/mask/rearm contract, but no real PHY/MAC
> consumer is attached. See
> [`notes/2026-07-26-gpon-ploam-fail-closed-fsm-offline.md`](../notes/2026-07-26-gpon-ploam-fail-closed-fsm-offline.md).
>
> **2026-07-19 live RX/O2 update:** phases 28–40 completed the guarded OEM
> EN7570/APD receiver handoff, reached stable xPON PHY_READY with GPON sync
> `0xa` and FEC, and then observed the GPON MAC with physical TX disabled.
> Passive 1.36-ms and 27.26-ms windows stayed in O1; an explicit, reversible
> O1-to-O2 test then held O2 for 1.1 seconds and restored O1/ATM exactly.  The
> GTC counter advanced exactly 8,800 frames—one every 125 us—and the
> downstream-superframe counter matched it, proving sustained PHY-to-MAC
> synchronization over a complete 1.024-second reference cadence.  HEC/CRC
> and TX-burst counters remained zero.  Phase 34 then showed that the sampled
> RX-EOF bit remained clear for later frames after a selective W1C.  Phase 35
> extended that diagnostic and observed `PLOAMD_RECV` with nine queued 32-bit
> words—three OEM-sized PLOAM records—while IRQs and TX remained disabled.
> O1/ATM and every TX barrier were restored exactly; FIFO data was not
> consumed.  Phase 38 then restored O1 before a single, audited three-word
> FIFO pop, verified the OEM `9 -> 6` word-count transition and restored ATM
> exactly. Phase 39 repeated that pop from a separate cold boot after its
> first attempt safely stopped at the phase-28 RSSI oracle; the retry captured
> a second, separately classified downstream record with the same TX barriers.
> Phase 40 then removed two complete records from one FIFO snapshot, verified
> the exact `9 -> 6 -> 3` accounting and left the third record queued. Both
> privately classify as identical Upstream Overhead messages. This establishes
> their observed FIFO dequeue sequence, not their optical spacing or a
> recurring OLT cadence. Raw records remain private; O3, identity and all
> upstream timing/TX paths remain forbidden. See
> [`notes/2026-07-19-gpon-phase40-o2-two-pop-live.md`](../notes/2026-07-19-gpon-phase40-o2-two-pop-live.md),
> [`notes/2026-07-18-gpon-phase39-o2-second-single-pop-live.md`](../notes/2026-07-18-gpon-phase39-o2-second-single-pop-live.md)
> and [`notes/2026-07-18-gpon-phase38-o2-single-pop-live.md`](../notes/2026-07-18-gpon-phase38-o2-single-pop-live.md).
> Earlier FIFO-discovery evidence is in
> [`notes/2026-07-16-gpon-phase35-ploamd-fifo-live.md`](../notes/2026-07-16-gpon-phase35-ploamd-fifo-live.md).
>
> **2026-07-14 EN757x source update:** the archived
> [`Sirherobrine23/airoha_xpon_en757x`](https://github.com/Sirherobrine23/airoha_xpon_en757x)
> remains useful corroborating material.  Its first modern Linux patch is not a
> functioning data path: `ndo_start_xmit()` only dumps and frees the skb, with
> no RX/QDMA, operational PLOAM or OMCI integration.  Its `mt7570_reg.h` is
> byte-identical to the XR500v GPL copy, while its EN757x identification rule
> corroborates this unit's exact `ID=0x03, variant=0x01` as EN7570.  Phase 27
> was consequently hardened to require both bytes before every possible write;
> ID `0x03` alone is shared with EN7571 and is no longer accepted.  See
> [`notes/2026-07-14-en757x-repository-crosscheck.md`](../notes/2026-07-14-en757x-repository-crosscheck.md)
> for that source comparison.
>
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
> active-high GPIO16. The live router confirms GPIO16 as an asserted output
> while `PHYSET3` bit 5 is clear. A later OEM audit corrected the latter's
> meaning: clear selects normal GPON burst mode; it is not a second physical
> inhibit. An audit also shows
> why Merbanan's current PON-PHY driver cannot be loaded as a passive probe: its
> `probe()` performs mode/reset/counter/IRQ writes.  See
> [`notes/2026-07-10-gpon-no-olt-phase3-xpon-platform-baseline.md`](../notes/2026-07-10-gpon-no-olt-phase3-xpon-platform-baseline.md).
> Phase 4 split the OEM/Merbanan combined init and produced a compile-only,
> fail-closed RX stage.  Its sole PHY operation clears the signal-detect
> deglitch bit while physically asserting GPIO16 TX-disable and retaining
> `PHYSET3` bit 5 clear. The module is intentionally absent from the shipping
> image, autoload
> and DTB, and was not loaded on the router.  See
> [`notes/2026-07-11-gpon-no-olt-phase4-rx-init-compile-only.md`](../notes/2026-07-11-gpon-no-olt-phase4-rx-init-compile-only.md).
> Phase 5 ran that isolated stage on the lab router without fibre.  The sole
> write changed `PHYSET3` from `0x4581e114` to `0x4581e110`, leaving bit 5 clear,
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
  state is output-high/asserted while `PHYSET3` bit 5, PRBS, test-frame and
  rogue-test enables are clear. The bit-5 state is retained as a mode/config
  invariant, not counted as an independent TX kill. This is the fail-closed
  baseline for a later
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
  Phase 25 then loaded the combined observer once with the authorised fibre
  connected.  The complete cold and post-reset gates passed, and its first
  five fixed I2C writes all returned success.  The following first calibration
  Vref sample was `0x020b`, outside the immutable `0x020a` oracle, so the
  observer returned `-ERANGE` and stopped fail-closed.  The remaining 13 I2C
  writes, RSSI gain/LOS, the RX-polarity MMIO write, all APD writes and all 21
  RX samples were not attempted.  GPIO16 `TX_DISABLE` remained asserted,
  while the observed xPON TX, rogue, PRBS, test-frame and interrupt gates
  remained inactive.  An immediate 35-second physical power cut restored the
  complete cold external state; the phase-14 passive image and normal DT were
  then restored and verified after a second cold boot.  This validates the
  strict abort path, not optical reception, APD operation or the cause of the
  one-LSB Vref difference.  See
  [`notes/2026-07-13-gpon-phase25-rx-apd-a2-live-safe-abort.md`](../notes/2026-07-13-gpon-phase25-rx-apd-a2-live-safe-abort.md).
  Phases 28–31 subsequently completed the combined receiver handoff and
  crossed the first MAC state-machine boundary.  Phase 28 completed all 18
  fixed EN7570 writes, three bounded xPON writes and 21/21 guarded samples,
  reaching PHY FSM 6, GPON sync `0xa` and FEC with TX disabled.  Phases 29 and
  30 observed the reset/default MAC in O1 for up to 27.26 ms; only
  `rx_eof_err_int` appeared.  Phase 31 then changed only the activation field
  O1-to-O2, sampled O2 nine times over 3.005 ms and restored O1 and the exact
  ATM mux.  During that 3.005-ms window the PLOAMd FIFO remained empty and no
  serial-number, ranging, upstream or TX event appeared.  An OEM source audit
  then confirmed the stock ordering `PHY_READY -> O2 -> gpon_enable()` and
  found no additional downstream parser-enable write before O2.  See
  [`notes/2026-07-16-gpon-phase28-31-rx-handoff-o2-live.md`](../notes/2026-07-16-gpon-phase28-31-rx-handoff-o2-live.md).
  Phase 32 extended the same audited O2 transition to 240 ms and added only
  read-only framing counters.  `DBG_RX_GTC_CNT` advanced exactly 1,920 frames
  and `DBG_DS_SPF_CNT` matched the one-frame-per-125-us cadence, with zero
  HEC/CRC errors and zero TX bursts.  The PLOAMd FIFO nevertheless remained
  empty throughout the measured window.  See
  [`notes/2026-07-16-gpon-phase32-o2-framing-live.md`](../notes/2026-07-16-gpon-phase32-o2-framing-live.md).
  Phase 33 retained the identical write surface and observed one continuous
  1.1-second O2 interval.  It received exactly 8,800 GTC frames, including
  8,192 at the 1.024-second checkpoint, with the downstream-superframe counter
  in lockstep and all HEC/CRC/TX counters zero.  No PLOAMd FIFO or receive
  indication appeared in 10,245 guarded checks.  The sticky
  `PHY_RX_EOF signal error` status was not cleared, so this run cannot yet tell
  whether it was a single mux-transition event or reasserted on later frames.
  O1, ATM and every transmit inhibit were restored exactly.  See
  [`notes/2026-07-16-gpon-phase33-o2-cycle-live.md`](../notes/2026-07-16-gpon-phase33-o2-cycle-live.md).
  A follow-up OEM audit classified `G_INT_ENABLE` as an IRQ mask rather than a
  parser/FIFO enable and found that status bits can latch while it is zero.
  The next minimal diagnostic is therefore a bit-20-only W1C clear followed by
  a 1–3-ms receive-only observation.  The main remaining stock-order delta is
  a 1-us GPON MAC reset pulse after selecting the GPON mux, but that pulse is
  not yet safe to reproduce: the Ethernet driver owns the shared reset array
  and stock quiesces PHY, MBI/GDM and CPU traffic first.
  Phase 34 then cleared only bit 20 after the first sampled post-O2 RX-GTC
  advance.  It remained clear across 24 additional frames over 3.006 ms,
  while all other latched status bits were preserved.  This proves that the
  indication was not reasserting per frame in that window and is consistent
  with a one-time transition latch.  PLOAMd remained absent in this short
  classification window and O1, ATM and every TX barrier were restored.  See
  [`notes/2026-07-16-gpon-phase34-eof-w1c-live.md`](../notes/2026-07-16-gpon-phase34-eof-w1c-live.md).
  Phase 35 combined that clear with the complete 1.1-second observation, then
  selectively cleared only the remaining safe startup-status subset
  `0x22010000`.  About 238 ms later the reset/default O2 MAC raised
  `PLOAMD_RECV` and reported PLOAMd FIFO status `0x00090009`: nine current
  32-bit words, maximum-used nine and no overrun.  The OEM consumes exactly
  three words per 12-byte PLOAM record, so this is three complete record slots,
  although their contents and distinctness are not yet known.  The observer
  did not consume FIFO data, enabled no IRQ and generated zero TX bursts.  O1,
  ATM and every physical/digital TX barrier were restored exactly.  This is
  the first OpenWrt proof that the MAC PLOAM recognizer/FIFO receive path works
  beyond GTC framing.  It does not yet establish whether the second status
  clear caused the event or merely preceded the next OLT discovery cycle.  See
  [`notes/2026-07-16-gpon-phase35-ploamd-fifo-live.md`](../notes/2026-07-16-gpon-phase35-ploamd-fifo-live.md).
  Phase 38 was the separately audited destructive successor.  It accepted the
  exact normal `PLOAMD_RECV` trigger, restored O1 before reading the FIFO, and
  required an identical final FIFO/status pair immediately before the pop.
  Exactly three downstream data-register reads changed the hardware count from
  nine to six 32-bit words, with maximum-used still nine and no overrun.  The
  record passed structural validation and is retained only in the private lab
  capture; no raw word, identity or decoded content is published.  IRQs and
  TX remained disabled, the TX-burst counter stayed zero, and ATM plus every
  physical/digital TX barrier were restored exactly.  See
  [`notes/2026-07-18-gpon-phase38-o2-single-pop-live.md`](../notes/2026-07-18-gpon-phase38-o2-single-pop-live.md).
  Phase 39 deliberately repeated that destructive receive boundary only after
  a fresh physical cold boot. Its first attempt stopped safely in phase 28
  when the strict RSSI Vref gate did not match: no MAC handoff, FIFO read or
  phase-39 observer execution followed. The cold-boot retry passed all 21
  phase-28 samples, accepted only the separately characterized early trigger
  residue, restored O1 before its one three-word pop, and verified the same
  `9 -> 6` accounting. Physical TX disable remained asserted, `PHYSET3` bit 5
  remained in GPON burst mode, GPON IRQs and TX remained disabled, and O1/ATM
  were restored exactly.
  The phase-38 and phase-39 records are privately classified at a high level
  as Upstream Overhead and Extended Burst Length respectively. They were
  captured on separate cold boots, so this does not establish their on-wire
  order or cadence; no raw record or optical identity is published. See
  [`notes/2026-07-18-gpon-phase39-o2-second-single-pop-live.md`](../notes/2026-07-18-gpon-phase39-o2-second-single-pop-live.md).
  Phase 40 extended the boundary from another fresh cold boot. It restored O1
  once before any FIFO-data read, removed two consecutive three-word records
  from the same snapshot, and independently verified the FIFO/status pair and
  every transmit guard around both pops. The hardware count changed exactly
  `9 -> 6 -> 3`, leaving the third complete record queued. Both private entries
  classify as identical Upstream Overhead broadcasts. This is their observed
  dequeue sequence in one already-populated FIFO; the CPU/MMIO pop timings are
  not optical cadence measurements and the untouched third entry is still
  unknown. GPON IRQs and TX remained disabled, the TX-burst counter stayed
  zero, and O1/ATM plus every physical/digital TX barrier were restored
  exactly. See
  [`notes/2026-07-19-gpon-phase40-o2-two-pop-live.md`](../notes/2026-07-19-gpon-phase40-o2-two-pop-live.md).
  A later guarded O3 attempt then stopped before O2 because
  `O3_O4_PLOAMU_CTRL` (`0x3c4`) was in hardware-auto mode. OEM source confirms
  that bit 0 clear permits the MAC to construct the O3/O4 upstream
  serial-number response autonomously, so continuing would have violated the
  RX-only contract even with GPIO16 asserted and `PHYSET3` bit 5 clear. The r11
  compile-only successor adds a second default-off opt-in which, only after an
  exact O1/reset/TX-disabled baseline, may set `old | BIT(0)` and hold software
  control as an invariant. It removes every GPON interrupt-status write and
  aborts on any TX-status, TX-counter or upstream-FIFO movement. The original
  word can be restored only after proving O1 and before returning the WAN mux
  to ATM. r11 passed two offline audits and reproducible source/`.ko` builds.
  Its later one-shot live run reached local O3, observed `SN_Request` and
  retained three Upstream Overhead plus three Extended Burst Length records.
  It then aborted immediately on an isolated MAC debug TX-burst count
  `0 -> 1`; no TX/SN-send interrupt, upstream-FIFO delta or upstream write was
  observed, and GPIO16 remained asserted. OEM source supports interpreting the
  isolated count as a scheduled SN slot rather than proof of optical emission,
  but the run remained fail-closed and ended in a verified physical power cut.
  See
  [`notes/2026-07-26-gpon-o3-software-ploam-gate.md`](../notes/2026-07-26-gpon-o3-software-ploam-gate.md)
  and
  [`notes/2026-07-26-gpon-o3-r11-live-safe-abort.md`](../notes/2026-07-26-gpon-o3-r11-live-safe-abort.md).
  The compile-only r12 successor permanently latches any TX IRQ, MAC
  TX-counter delta or upstream-FIFO delta. It may tolerate only the exact,
  stable `+1` MAC count for risk-reducing O1/formatter cleanup; any upstream
  latch still forbids restoring hardware-auto PLOAM control and requires a
  power cut. r12 adds no write call site and has passed reproducible clean
  MIPS builds plus an independent static audit. It has not been loaded on the
  router. See
  [`notes/2026-07-26-gpon-o3-r12-cleanup-hardening.md`](../notes/2026-07-26-gpon-o3-r12-cleanup-hardening.md).
  The r13 successor adds the directly readable MAC
  `DBG_TX_GEM_CNT` as another permanent upstream latch. It also records the
  PHY TX status/frame/burst words with the OEM double-read pattern, but leaves
  them raw and unlatched: changed and unchanged values are diagnostic-only and
  cannot affect abort, cleanup or success. The time-critical O1/two-pop window
  uses only compact MAC reads at every pre/post boundary; it never touches the
  PHY counter latch. An independent audit found and closed one missing
  IRQ-off boundary before the final source passed two audits and two
  reproducible clean MIPS builds. r13 preserves r12's exact write surface. See
  [`notes/2026-07-26-gpon-o3-r13-tx-correlation-compile.md`](../notes/2026-07-26-gpon-o3-r13-tx-correlation-compile.md).
  Its first live invocation stopped earlier in phase 28 on the already-known
  strict RSSI-oracle signature (`vref=0x0209`, zero RSSI, five I2C writes and
  zero MMIO writes/samples). r13 itself never loaded; a physical power cut was
  performed before the bounded cold-boot retry. See
  [`notes/2026-07-26-gpon-o3-r13-live-attempt1-phase28-abort.md`](../notes/2026-07-26-gpon-o3-r13-live-attempt1-phase28-abort.md).
  The retry passed phase 28 and loaded r13. It reproduced local O3,
  `SN_Request` and the same private three-plus-three downstream record
  classification. The MAC TX-burst counter again changed `0 -> 1`, while
  2,912 samples of `DBG_TX_GEM_CNT` remained zero and the raw, unlatched PHY
  TX frame/burst words remained zero. The PLOAMu FIFO status additionally
  changed `0x00800080 -> 0x80800080`: only the OEM-defined bit-31 underrun
  flag moved, while both availability fields stayed at 128. That combined
  source (`TX_BURST + PLOAMU`) correctly disqualified r12's isolated-`+1`
  cleanup exception. O1 and ATM were restored, but the formatter and forced
  software-PLOAM control remained pinned, so the run ended `unsafe-pinned`
  and a physical power cut was verified. No TX/SN-send IRQ, TX-GEM movement,
  upstream write or identity change was observed; none of these counters prove
  optical transmission or silence. See
  [`notes/2026-07-26-gpon-o3-r13-live-tx-correlation.md`](../notes/2026-07-26-gpon-o3-r13-live-tx-correlation.md).
  The r14 successor then admitted only that exact stable combined class for
  risk-reducing cleanup: `TX_BST_CNT +1`, the bit-31-only PLOAMu underrun,
  unchanged FIFO occupancy fields and TX-GEM count, no TX IRQ or upstream
  write, and no unsafe IRQ bit at any cleanup boundary. Its one-shot live run
  reproduced local O3, `SN_Request` and the same private three-plus-three
  downstream classification. All 15 class guards passed, the formatter was
  restored exactly, activation returned to O1 and the WAN mux returned to
  ATM. Software-PLOAM control intentionally remained pinned after the
  upstream latch, so the run still ended `unsafe-pinned` and required physical
  power removal, which was then verified for more than 35 seconds. No private
  record was retrieved from that unsafe result.
  This closes the r13 cleanup question but remains internal-MAC/RX evidence,
  not proof of optical transmission, silence or OLT acceptance. See
  [`notes/2026-07-26-gpon-o3-r14-live-exact-cleanup.md`](../notes/2026-07-26-gpon-o3-r14-live-exact-cleanup.md).
  An offline-only follow-up then extracted a namespaced, hardware-independent
  three-word PLOAM wire layer. It classifies and decodes only
  `Upstream_Overhead` and `Extended_Burst_Length`, the two downstream classes
  established by r14, and compiles synthetic three-plus-three copies through
  the OEM first-of-three filter. The decoder has no MMIO, IRQ, FIFO, GPIO,
  I2C, identity or hardware-TX path and exports no kernel ABI. Two clean
  Linux-6.12.80 MIPS builds produced the same `.ko`; the module was not loaded
  on the router, so this is compile/static evidence rather than a runtime
  result. The lab FSM now consumes those semantic decoders directly and checks
  every decoded field, avoiding a second manual implementation. A subsequent
  r3 checkpoint closed logical error propagation/retry, ordered-worker,
  repetition, synchronous/pending AES-key, Alloc-ID and EqD-overflow contracts
  and executed the exact sources on the host. It remains lab-only and
  disconnected from hardware because the multi-callback O2/O3/O4/O5
  transitions are not yet physical transactions. A separate two-line QDMA fix
  keeps the cached IRQ mask equal to the active hardware mask, so xPON consumer
  registration cannot revive an unrelated pending NAPI interrupt; provider
  reprobe/removal and event mask/rearm are still incomplete and there is no
  live bit-16/24 delivery evidence. See
  [`notes/2026-07-26-gpon-ploam-wire-decoder-offline.md`](../notes/2026-07-26-gpon-ploam-wire-decoder-offline.md)
  and
  [`notes/2026-07-26-gpon-ploam-fail-closed-fsm-offline.md`](../notes/2026-07-26-gpon-ploam-fail-closed-fsm-offline.md).

That is the current experimental extent: receiver bring-up, a reversible O2
observation, two separately booted single-record FIFO pops, one same-FIFO
two-record pop, and guarded local-O3 RX observations work only through manually
loaded, fail-closed lab modules.
There is still no shipping GPON MAC/PHY driver, PLOAM state machine, laser
transmit support, GEM/QDMA data path or OMCI integration.

## Status and outlook

GPON is still unported, but the live work has now demonstrated optical
reception through the EN7570 and xPON PHY: OpenWrt reaches PHY_READY, GPON sync
and FEC while every transmit barrier remains asserted.  The stock firmware
still supplies the end-to-end oracle—O5, OMCI and PPPoE service with its
authorised identity—whereas OpenWrt now demonstrates both exact downstream GTC
framing and a live `PLOAMD_RECV`/non-empty-FIFO event through the reset/default
MAC in O2, with IRQs and TX disabled. Phases 38 and 39 completed two
OEM-audited, RX-only single-record pops from separate cold boots. Phase 40
then completed two bounded pops in one capture, verified `9 -> 6 -> 3`, and
left the third entry queued while every TX barrier remained asserted. Its two
private entries are identical Upstream Overhead broadcasts; this is FIFO
dequeue evidence, not an optical timing measurement. The first phase-39
attempt additionally demonstrated the fail-closed RSSI-oracle abort before
any MAC handoff or FIFO read. A stage-A-only long control remains useful to
test whether clearing bits 16/25/29 was causal; any further destructive
receive capture needs a fresh physical cold boot and the same fail-closed
limits. A raw reset-register experiment is still neither needed nor
authorised.
The r13 O3 retry further separated the internal MAC indications: a stable
`TX_BST_CNT +1` coincided with a bit-31-only PLOAMu FIFO underrun, while
`TX_GEM_CNT` and the raw PHY frame/burst words stayed at zero. This supports
an internal scheduled/empty-FIFO event, not an optical conclusion. A
source-only refinement then used that exact pattern solely to make failure
cleanup less stateful. r14 restored the formatter, O1 and ATM under exact
stable guards while retaining software PLOAM control and the mandatory cold
power-cycle result.
Reproducing the stock outcome still requires a transactional hardware adapter
for the now-tested software PLOAM core, transmitter calibration and burst
timing, GEM/OMCI and WAN-QDMA integration.
The current probes now establish a receiver, framing and downstream-PLOAM
foundation, not yet a working OpenWrt optical WAN.

## Cross-references

- [Ethernet / DSA](04-ethernet-dsa.md) — the econet-eth driver, QDMA, and the nested switch the LAN side uses (the PON MAC shares this complex).
- [VoIP / FXS SLIC](06-voip-fxs-telephony.md) — the subsystem that demonstrated the "OEM source exists, port it" approach; the GPON driver lives in the same 2.6.36 OEM tree.
- [OEM firmware recon](10-stock-firmware-access.md) — `wan_itf=nas10` and the rest of the OEM WAN/PON configuration.
- [Overview / specs](Home.md) — where GPON sits in the overall subsystem status table.
