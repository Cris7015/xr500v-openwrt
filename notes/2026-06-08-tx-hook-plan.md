# TX-hook (modelo OEM FoeBindToPpe) — plan de implementación

## El breakthrough (por qué)
El engine SÍ lee DRAM[FOE_ENTRY_NUM] (el slot del descriptor RX), pero nuestras entries
están INCOMPLETAS → no son binds HW-forwardeables válidos → las ignora/rebuildea. El OEM
(`PpeTxHandler@ra_nat.c:1715` → `FoeBindToPpe@1029`) llena la entry COMPLETA desde el paquete
de EGRESS real en el TX path. Probado: el shotgun (8192 slots con entries incompletas) NO
HITeó (back-to-back limpio); el atomic dump confirmó DRAM[734]=nuestro-BIND pero crsn=15.

## Qué llena FoeBindToPpe que a nosotros nos falta (de ra_nat.c 1029-1480)
Del paquete de egress (skb en TX, post-routing/NAT/pppoe):
- **MACs egress**: dest_mac = next-hop (BRAS/gw), src_mac = MAC del router en egress (del eth hdr del skb TX).
- **pppoe_id** = session id (del pppoe hdr); **bfib1.psn=1** si hay pppoe.
- **new tuple (NAT'd)**: new_sip/new_sport = IP/port pppoe-wan (post-SNAT), new_dip/new_dport = dst.
- **bfib1**: vlan_layer, psn (pppoe), vpm, dvp, drm, **state=BIND**, ttl, **cah=1**, time_stamp=RegRead(FOE_TS).
- **iblk2 / ib2**: DEST_PORT = fpidx (el GDM/puerto egress), PORT_MG/PORT_AG=0x3f.
- etype si remark.

Nuestro V1 struct YA tiene los campos (econet_ppe.h): MTK_FOE_IB1_BIND_PPPOE(b19),
BIND_VLAN_LAYER(18:16), BIND_VLAN_TAG(b20), BIND_TTL(b24), BIND_CACHE(b22); mac_info
{dest_mac,src_mac,pppoe_id,etype,vlan1}; ib2 DEST_PORT(7:5)/PORT_MG/PORT_AG. Builders:
en75_foe_entry_{prepare,set_ipv4_tuple(egress),set_pse_port,set_pppoe,set_dsa,set_vlan,set_queue}.
OJO: los nombres bfib1 del OEM (RA_HWNAT) ≠ posiciones ib1 mainline en algunos bits — VERIFICAR
cada bit contra mtk_ppe.h V1 (vpm/dvp/drm no existen en mainline V1 igual; mapear o probar sin).

## Integración (3 puntos)
1. **RX (econet_qdma.c, donde se llama en75_ppe_check_skb)**: cuando crsn=HIT_UNBIND_RATE +
   offload_enabled, guardar el slot (hash) en un global `en75_tx_bind_slot` + arm `en75_tx_bind_arm`.
   (Propagación skb→TX no sirve con NAT+pppoe: el skb se transforma; usar global para el flujo de test.)
2. **TX (econet_port.c en75_dev_xmit, ANTES de en75_qdma_xmit)**: si armado + el skb es IP al dst
   de test (143.0.170.144) saliendo por el conduit, parsear el egress (saltar DSA special-tag 4B,
   parsear pppoe si está, IP/UDP), construir la entry COMPLETA con los builders + mac_info del eth
   hdr + pppoe_id, commit BIND en `en75_tx_bind_slot`, desarmar. Port de FoeBindToPpe (parse L2/pppoe/L3).
3. **Patches**: el RX-store toca el patch del check_skb (356?); el TX-hook es código nuevo en
   econet_port.c → nuevo patch (358?) o extender gen. La función de bind reusa econet_ppe.c.

## Validación
Flujo test ruteado por pppoe-wan (.68.248:50019→143.0.170.144:8080). Con el TX-hook armado:
crsn=15 debe CAER (el engine HW-forwardea) + el flujo deja de puntear al CPU. A diferencia del
shotgun, la entry ahora tiene egress/NAT/MAC completos → bind válido.

## Simplificación para first-light
Single test flow, global slot (no propagación skb). Si pppoe complica, primero probar la
hipótesis con una entry "completa-ish" hardcodeada (leer del device: pppoe-wan IP, peer/BRAS MAC
`ip -d link show pppoe-wan`, session id de `/sys/class/net/pppoe-wan` o pppd) via committest
extendido con params new_*/pppoe_sid/egress_mac, ANTES del TX-hook full. Si esa entry HITea =
hipótesis confirmada → vale el TX-hook completo.

## Estado
Device en build #41 (tiene ppe_atomic + el rank-1 commit + cah_clear_one). Router-mode pppoe-wan.
Memoria: project_xr500v_ppe_hwnat_port.md. Disasm: ~/tools/xr500v/hwnat-disasm + rt-n56u-hwnat/ra_nat.c.
