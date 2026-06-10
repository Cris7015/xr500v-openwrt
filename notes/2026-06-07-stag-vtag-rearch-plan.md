# STAG→VTAG re-arquitectura — plan ejecutable (2026-06-07)

## Por qué (conclusión de builds #22-28)
El bind del PPE está 100% correcto (table-8192, slot↔descriptor alineado, state=2, ib2,
timestamp, bind sequence — todo confirmado contra el disasm del hw_nat.ko OEM). El ÚNICO
blocker: el engine guarda el `sip` levemente mal (`.64` vs `.68` real) → su lookup del paquete
entrante no matchea su propia entry → MISS → rebuildea unbind → nunca HITea.

Descartado por comparación OEM-vs-mío (en vivo): NO es el GDM/FE/CDMA (todos los regs matchean
o no importan; CDMA_VLAN_CTRL=0x81000001 == OEM y aún así da .64). **La diferencia está en el
SWITCH**: el OEM emite un **VLAN tag estándar** al CPU (STAG→VTAG); yo emito el **special-tag
MTK** (gsw_stag solo re-stampea el TPID a 0x8100 pero mantiene el formato special-tag, cuyo
encoding de puerto ≠ TCI de VLAN) → el PPE parsea los 4 bytes del tag distinto → IP corrido.

## Defs MT7530 (gsw/mt7530.h)
- `MT7530_PVC_P(x) = 0x2010 + x*0x100` → CPU port6 = **0x2610**
- `PORT_SPEC_TAG = BIT(5)` (0x20) — emitir special-tag (HOY seteado en el CPU port)
- `PVC_EG_TAG(x) = (x&7)<<8`, `PVC_EG_TAG_MASK = 0x700` — egress tag control del PVC
- `VLAN_ATTR(x) = (x&3)<<6`, MT7530_VLAN_USER, etc.
- `MT7530_PCR_P(x) = 0x2004 + x*0x100`; `EG_TAG(x)=(x&3)<<28` (PCR); `EG_CON=BIT(29)`
- Egress: UNTAG=0, TAG=2, STACK=3
- `mt753x_upstream_port_enable` (mt7530.c:1211): hoy escribe `PVC_P(cpu) = PORT_SPEC_TAG | PVC_PASSTHROUGH`
- DSA-link (MCM cascade) port: `PVC_P = VLAN_ATTR(USER) | PORT_SPEC_TAG` (1271) → copia el
  special-tag del MCM al tag emitido al CPU (así sobrevive el cascade).

## El cambio (multi-archivo)
1. **mt7530.c — CPU port6 (`mt753x_upstream_port_enable`)**: en vez de `PORT_SPEC_TAG`,
   poner el CPU port en **egress VLAN-tagged**: clear PORT_SPEC_TAG; set `PVC_EG_TAG(TAG)` +
   `EG_TAG(MT7530_VLAN_EGRESS_TAG)` en PCR. → el CPU port egresa con VLAN 0x8100 + VID.
2. **Per-port PVID**: lan1..4 → VID únicos (ej. 1..4) para que el VID lleve el puerto origen.
   El cascade MCM→on-die debe preservar el VID (VLAN-aware en ambos switches).
3. **gsw/tag-mtk.c — el tagger**: RX leer el puerto del **VID del descriptor (tci)** en vez del
   special-tag del frame; TX insertar VLAN(VID=dest port) en vez del special-tag.
   ALT: cambiar el dsa tagging protocol a `tag_8021q` (mainline DSA VLAN-based) — más limpio
   pero toca el registro del tagger. Verificar si tag_8021q está en el kernel 6.12.
4. **GDM/CDMA urx** (CDMA_VLAN_CTRL=0x81000001, YA seteado por ppe_stag): popea el VLAN al
   descriptor tci. Confirmar que con un VLAN real (no special-tag) sí popea (untag→1).
5. El PPE entonces ve un frame VLAN estándar → parsea el IP en el offset correcto → guarda .68
   → matchea su propia entry → HIT. + el bind_by_scan/flowtable ya hecho → throughput.

## Riesgo / safety
- ROMPE el DSA actual (que costó 157 iter). Hacer GATEADO+reversible si se puede, o con safety
  net: `nohup sh -c 'sleep 150; [ -f /tmp/net_ok ]||/root/netfix.sh' &` + `touch /tmp/net_ok`.
- Backup serial: `sudo picocom -b 115200 /dev/ttyUSB0`. Recovery: /root/netfix.sh.
- Validación incremental: primero confirmar que el PPE guarda .68 con un VLAN real (aunque el
  data-path se rompa, el foedump del engine es independiente), DESPUÉS arreglar el tagger.
- NO se puede validar barato sin romper el tagger (clear PORT_SPEC_TAG rompe el RX demux) → el
  cambio del tagger y del switch van juntos.

## ⚠️ REALIZACIÓN CLAVE (scope mayor) — el RX demux cambia de arquitectura
El PPE necesita el VLAN POPEADO (frame limpio → parsea el IP bien); el tagger DSA actual
necesita el VLAN PRESENTE en el frame (lee el puerto de ahí). Conflicto. El OEM lo resuelve:
CDMA urx popea el VLAN al **descriptor tci**, el PPE ve el frame limpio, y el eth driver hace
el **demux por el tci del descriptor** (get_erx_tci → puerto), NO por el tagger del frame.
→ build #29 NO es solo config de switch: es un **cambio de arquitectura del RX demux**
(descriptor-tci-based en econet_qdma.c, bypaseando/reescribiendo el tagger gsw/tag-mtk.c).
Registros confirmados (gsw/mt7530.h): VAWD1(0x94)=VLAN_VALID|VTAG_EN|PORT_MEM(bits23:16);
VAWD2(0x98)=egress-tag 2b/puerto (UNTAG=0/TAG=2); VTCR(0x90) WR_VID; PPBV1_P(x)=0x2014+x*0x100
(G0_PORT_VID=PVID); ETAG_CTRL_P(p,x)=(x&3)<<(p<<1). g_en751221_ondie/_mcm = los 2 switch priv.
PASOS (multi-iter, cada uno testeable, gateado por un flag g_en75_vtag default-off):
 (1) MCM: PVID 1-4 en lan ports + VLAN table (VID 1-4, member={user,cascade}, untag user).
 (2) on-die: CPU port6 clear PORT_SPEC_TAG + egress TAG; cascade ports VLAN-aware; VLAN table.
 (3) CDMA urx ya ON (ppe_stag) → popea el VLAN al descriptor tci.
 (4) econet_qdma.c RX: leer tci → mapear VID→user netdev (demux), bypaseando el tagger.
 (5) TX: el driver setea sp_tag/VID del descriptor = dest port (o VLAN insert).
ES TODO-O-NADA en el DSA (rompe el LAN si queda a medias) → safety net reboot-watchdog 110s +
picocom. Probable multi-flash con debug del cascade. La parte DIFÍCIL (diagnóstico) RESUELTA.

## ⚠️ CORRECCIÓN 2026-06-07 (post-relectura memoria): la validación UNTAGGED ya falló
El `vtag_validate.sh` (clear PORT_SPEC_TAG → frame SIN tag) **ya se probó = INCONCLUSIVE**
(memoria línea 58: rompió tanto el cascade DSA que NO se estableció el flujo, 0 entries, sport→1).
Un frame untagged NO sirve: sin tag el cascade nested colapsa y nada llega bien al GDM.
→ La validación correcta usa un **VLAN 802.1Q REAL** (mantiene un tag que el cascade lleva al GDM,
pero parseado en el offset correcto). Diferencia clave con el untagged.

## Validación SIN flash (build #27 ya tiene gsw_reg)
`gsw_reg` pokea cualquier registro del MT7530 → replico la config VLAN real vía pokes, sin rebuild.
Script: `~/tools/xr500v/scripts/gsw_vtag_validate.sh` (detached, reboot-watchdog 120s). Pokea
VAWD1=0x50600003 / VAWD2=0x2000 / VTCR WR_VID1 (VLAN1: port5+port6, port6 egress-TAGGED) +
PVC_P(6)=0x100 (clear PORT_SPEC_TAG) + PPBV1_P(5)=PVID1, con ppe_stag(FE VLAN-aware)+engine+steer.
NECESITA: router-mode + flujo ruteado (curl desde WSL: `sudo ip route replace 143.0.170.144/32 via
192.168.68.222` + loop curl --local-port 50019 a :8080). VERDICT: foedump w1=c0a844f8(.68)=FIX vs
c0a840f8(.64)=sigue shifteado. Si da .68 → byte-lane confirmado con VLAN real → vale el re-arch full.

## Switch-half YA construida (gsw_vtag, gateado, flash-safe)
Patch 360 extendido con `gsw_vtag` (DEVICE_ATTR_WO en gsw/mt7530.c, gen_gsw_reg.py): mismo efecto
que los pokes pero bakeado+reusable (echo 1 = VLAN real, echo 0 = revert special-tag). COMPILA
(econet-gsw.ko LD OK). Default = no-aplicado (current DSA intacto) → flashear es seguro. Es el
PASO 2 del plan de arriba; faltan PASO 4 (tag-mtk.c RX demux por VID/tci) + PASO 5 (TX VLAN insert).

## Estado a retomar
Device en build #27, AHORA en **bridge/AP mode** (br-lan=lan1/2/4+phy0/1-ap0, 0 wifi clients) — NO
router. Para CUALQUIER test PPE hay que volver a router-mode (cambia red del user; gotcha Movistar
hold-down). Tooling: gsw_reg/gsw_stag/gsw_vtag(nuevo), ppe_fe, rxdump/foedump/scan/bindscan, gates.
Disasm OEM en ~/tools/xr500v/hwnat-disasm/. Captura engine en ~/tools/xr500v/ppe-p4/engine_foe_capture_b23.txt.
Memoria: xr500v-ppe-hwnat-port-600mbps.
