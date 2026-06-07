# PPE/HNAT phase 3 — implementation design (2026-06-06)

Output of a 5-agent RE/design pass over mainline `mtk_ppe*.c` + the econet-eth driver.
Phases 1a (FoE table + PPE config, engine off) and 2a (RX crsn dump) are DONE and HW-verified
(`TB_BASE_rb==foe_phys`, `crsn=0x1e PPE_BYPASS`). Software forwarding is CPU-bound at ~168 Mbps
(0% idle, ~80% sirq) with the link able to do 674 Mbps → ~4x headroom for HW offload. Rig = the
San Juan speedtest server (`speedtest.interredes.com.ar:8080/.../random4000x4000.jpg`) routed
through the box from WSL (`ip route add IP via 192.168.68.222`).

## Strategy
Decouple **(A) datapath** (enable engine, steer GDM→PPE) from **(B) offload logic** (FoE entry
builders, RX bind, TC/flowtable), all behind ONE default-off runtime gate (debugfs/module-param)
so it A/B-toggles live without reflashing. The single decisive unknown — *does the EN751221 PPE
forward a SEARCH_MISS back to the CPU (FORWARD_BUILD→CDM→QDMA0) with a valid RX descriptor?* — is
proven with ZERO offload code (just engine-on + DEFAULT-nibble steering, mgmt/ARP/mcast staying on
the CPU nibbles). Only then port the verbatim mt7622-class (NETSYS-v1, FoE V1 **80B**, 16384 entries)
core. Rig is the end-to-end test: no-regression (~168 Mbps) for the steering stage, climbing toward
674 Mbps for the offload stage.

## The GDM steering (the feared unknown — turned out trivial)
`fwd_cfg` reg at **0x100 within each GDM block** (abs 0x500 GDM1/LAN, 0x1500 GDM2/WAN) = 1:1 with
mainline `MTK_GDMA_FWD_CFG(0/1)`. Low 16 bits = four 4-bit fports: DEFAULT(3:0)=transit unicast,
MCAST(7:4), BCAST(11:8), MYMAC(15:12). Today all=0 (`ETX_FPORT_QDMA0_CPU`). PPE = **4**
(`ETX_FPORT_PPE`, qdma_desc.h:118). Full mainline = `0x4444` (=`gdma_to_ppe[0]`). Helper
`en75_set_gdm_port_fwd_cfg(port,val)` already exists (econet_port.c:113). **GATE: only safe after
GLO_CFG.EN=1, else it black-holes all forwarded traffic.**

## Patch order (each testable, gated, reversible until binding)
1. **PPE engine enable** — in `en75_ppe_init` (econet_eth.c, the "GLO_CFG.EN left unset" spot): poll
   GLO_CFG(0x200) BIT31 BUSY clear, write `EN|IP4_L4_CS_DROP|IP4_CS_DROP|FLOW_DROP_UPDATE = 0x20D`
   (verbatim `mtk_ppe_start`). Behind default-OFF gate. GDM stays CPU. Extend dev_info to show
   GLO_CFG_rb. **Test: readback EN=1/BUSY clear; rig speedtest unchanged ~168M (engine on, starved).**
   Zero blast radius. *This is the FIRST patch* (de-risks the GLO_CFG poke that can hang the bus).
2. **Fix per-GDM regs** — en75_init_port (econet_eth.c:244-249): id==2/GDM2 → `port1.regs` (today both
   share port0.regs; latent, harmless now, prerequisite for per-port WAN-vs-LAN steering). RISK: if
   GDM2 isn't at FE+0x1400 the WAN dies — confirm block location on HW first.
3. **Staged steering DEFAULT→PPE(4) on ONE port (WAN)** — the **decisive HW experiment**. Helper
   variant setting only default_fport=4, mymac/bcast/mcast=0. **Test: single LAN↔WAN flow MUST still
   pass (proves miss→CPU works); phase-2a dump shows live crsn/ppe_entry; TB_USED(0x224) climbs;
   SSH/ARP survive; rig ~168M no-regression.** HIGH risk (can blackhole the steered port's unicast).
4. **Port FoE V1 core** — new `econet_ppe.{h,c}` (verbatim mtk_ppe.h structs/enums + ported
   `mtk_foe_entry_prepare`(port_mg=0), set_ipv4_tuple, set_pse_port, hash, `__mtk_foe_entry_commit`
   = memcpy 76B + wmb + ib1 LAST + dma_wmb + CACHE_CTL(0x320) bit9 flush). Refactor the
   `en75_ppe_foe_virt/phys` statics into `struct en75_ppe {foe_virt,phys,lock,foe_check_time[16384],
   foe_flow[],l2_flows}` hung off public `struct en75_eth`. **Hardcode V1 80B / hash_offset=2; delete
   every netsys-v2 arm** (else silent 96B corruption). New Kbuild .o = a package patch (350/351-style).
   Test: compile-only + a debugfs hand-built single-entry commit → TB_USED=1, flow still passes.
5. **RX bind hook** — replace the phase-2a printk (econet_qdma.c:191-198) in the ELSE branch AFTER
   `en75_rx_before_recv()` (so skb->dev/protocol valid): if `crsn==0x0f` (HIT_UNBIND_RATE_REACHED)
   call `econet_ppe_check_skb(eth->ppe, skb, hash)` with the RAW `get_erx_ppe_entry` (the HW slot, NOT
   the jhashed value). Port the rate gate (foe_check_time, HZ/10) + `__mtk_ppe_check_skb` (spin_lock_bh).
   No-op while no flows queued. (Placing it before dev-set = NULL-deref in the DSA branch.)
6. **TC/flowtable front-end** — new `econet_offload.c` porting `mtk_ppe_offload.c` (flow_block,
   mtk_flow_offload_replace/destroy/stats, mangle helpers), trimming WED/WDMA + netsys-v2. Single-PPE.
   `mtk_flow_set_output_device` → compare odev to the two GDM netdevs → pse_port=GDM1(1)/GDM2(2) via
   set_pse_port (IB2 DEST_PORT GENMASK(7,5)); `mtk_foe_entry_set_dsa(etype=BIT(port))` for nested-DSA
   user ports (tagger=DSA_TAG_PROTO_MTK, gsw/tag-mtk.c). Add `.ndo_setup_tc`=en75_setup_tc +
   `hw_features|=NETIF_F_HW_TC` (only when ppe!=NULL). **Test on rig: TB_USED grows, sirq drops,
   throughput climbs 168→toward 674; SSH/ARP/IGMP/mDNS survive.** Nested dual-mt7530 PASSTHROUGH is
   the novel risk vs mainline single-switch.
7. **Full steering 0x4444 + teardown + (optional) accounting** — extend to mymac/bcast/mcast=4 ONLY
   after validating PPE mcast/L2-flood doesn't break ARP/IGMP/mDNS/DHCP (else keep DEFAULT-only as
   production). Add `econet_ppe_stop`/deinit into en75_remove + qdma_unuse (fixes leak + protects the
   MT7603 no-reenum warm-reboot path). accounting=false until MIB layout (BFB52000/0x338) confirmed.

## Live-HW RE still needed (resolve while implementing)
1. **THE gate**: does FORWARD_BUILD(3) miss actually reach CPU via CDM/QDMA0? (only patch-3 answers)
2. Live crsn values with engine ON (confirm 0x0f emitted) — today crsn always 0 (EN=0).
3. GLO_CFG bitset 0x20D sufficient? (capture stock-fw PPE+0x200 dump; check TSID_EN/MCAST_TB_EN).
4. Aging/timestamp counter loc (FE+0x10? eth->regs->fe[4]?) + tick rate (wrong → instant expiry churn).
5. IB2 DEST_PORT numbering GDM1=1/GDM2=2 (HW-verify via a bound-flow FoE readback).
6. GDM2/WAN really at FE+0x1400 (patch-2 prerequisite; else WAN breaks).
7. Are the 4 fwd_cfg nibbles honored per-class, or flat 0x4444? (set only default=4, check bcast→CPU).
8. dmam_alloc_coherent truly non-cached here, or extra dma_sync beyond CACHE_CTL bit9?
9. HW FoE egress tag (set_dsa etype=BIT(port)) × nested dual-mt7530 PASSTHROUGH tagger interaction.

## Top risks
- Steer→PPE while EN=0 black-holes ALL forwarding (enforce P1 before P3, same gate).
- Broken miss-forward → DEFAULT-only still drops the steered port's unmatched unicast (P3 probes it).
- WAN outage from the P2 regs fix if GDM2 ≠ FE+0x1400.
- 0x4444 before mcast/L2 validated breaks ARP/IGMP/mDNS/DHCP — keep DEFAULT-only.
- Silent FoE corruption from any leftover netsys-v2 arm (must be V1/80B).
- Barrier order: ib1 LAST after dma_wmb on the 76B payload.
- WRONG SLOT: commit at `get_erx_ppe_entry` (HW slot), NOT the SW jhash.
- Offloaded flows bypass the CPU TX + the BQL dql.min_limit=262144 floor → the 168M ceiling no longer
  governs; re-measure on the rig.

Full agent reports: workflow run wf_aecf0370-7f7. Prereq to TEST: device back in router-mode
(pppoe wan on lan3) — see [[reference_xr500v_flash_workflow]] / the B2 setup in the ppe memory.
