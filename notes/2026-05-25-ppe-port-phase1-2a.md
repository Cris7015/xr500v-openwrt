# PPE/HNAT port — phases 1 & 2a (WIP, 2026-05-25)

Goal: HW NAT offload (PPE) to push forwarding from ~160 Mbps (software, single MIPS 34Kc) toward
~600 Mbps (line rate, what the OEM does). This is WIP research — **not** for daily use (the
daily-use image is the one up to patch 330: LAN + 161 Mbps TX). The PPE patches (340/350) add an
init dump + an RX debug dump and currently provide **no throughput benefit yet** (engine is enabled
but the GDM still bypasses it).

## Feasibility: CONFIRMED
The EN751221 PPE is a standard MediaTek-family PPE (16384-entry FoE table, `crsn` + `ppe_entry`
descriptor fields, register layout from mainline `mtk_ppe_regs.h`). Mainline
`mtk_ppe.c`/`mtk_ppe_offload.c` is a near-direct template.

## Phase 1 (patch 340) — PPE init + register-mapping verification — DONE
`en75_ppe_init()` (inline in econet_eth.c, called at end of probe): allocates the FoE table with
`dmam_alloc_coherent` (16384 × 80 B = 1.28 MB), writes TB_BASE/TB_CFG/aging/rate/flow_cfg with the
values from mainline `mtk_ppe_start` (GLO_CFG.EN intentionally left untouched). Readback result:

```
foe_phys=0x02e00000  TB_BASE_rb=0x02e00000   <- MATCH
TB_CFG_w=0x00014fbc  TB_CFG_rb=0x00014fbc     <- MATCH
GLO_CFG_rb=0x0000060d (EN + IP4_*_CS_DROP + FLOW_DROP_UPDATE + UDP_LITE already set at reset)
```

**TB_BASE/TB_CFG readbacks match the written values → the mainline `mtk_ppe_regs.h` offsets apply
verbatim on the EN751221.** Biggest risk (offsets differ) eliminated. No LAN regression. GLO_CFG.EN
is set by default at reset, so the engine is effectively running already; with a zeroed table +
`SEARCH_MISS=FORWARD_BUILD` everything misses → goes to CPU → LAN intact.

## Phase 2a (patch 350) — confirm engine processes ingress — DONE
RX path dumps `crsn` + `ppe_entry` per packet. Result: every RX packet is tagged with
`crsn = 0x1e = MTK_PPE_CPU_REASON_PPE_BYPASS` (constant) and a varying `ppe_entry` hash.

→ The PPE is alive and tags ingress (RX plumbing confirmed — this is what Phase 3's bind trigger
needs), **but the GDM bypasses the PPE** (sends ingress straight to the CPU without a flow lookup).
Expected, because the router is currently a bridge endpoint and routes nothing, and because the GDM
forwarding config still points at the CPU (the `econet_port.c:134` TODO).

## Phase 3 (next, the big one) — not started
1. **GDM→PPE steering**: program the GDM `fwd_cfg` destination so ingress unicast goes to the PPE
   instead of bypass (mainline `MTK_GDMA_FWD_CFG`). Identify the econet `fwd_cfg` dest field.
2. **Flowtable offload**: port `mtk_ppe_offload.c` (register a flow_block via `ndo_setup_tc` /
   `TC_SETUP_FT`, build a V1 `mtk_foe_entry` from a `flow_offload`, commit it).
3. **RX reason**: handle `crsn == 0x0f` (HIT_UNBIND_RATE_REACHED) in the RX path →
   `econet_ppe_check_skb(eth->ppe, skb, hash)` to bind hot flows.
4. **Routing/physical setup (required to test)**: the router must actually route+NAT (WAN port +
   separate LAN subnet + a client behind a LAN port). Only forwarded/NAT'd flows are offload
   candidates. Quick interim win for forwarding: enable software flow offload
   (`uci set firewall.@defaults[0].flow_offloading=1`).

crsn enum (mtk_ppe.h): 0x0d=UN_HIT, 0x0e=HIT_UNBIND, 0x0f=HIT_UNBIND_RATE_REACHED (bind trigger),
0x1e=PPE_BYPASS, 0x1f=INVALID.
