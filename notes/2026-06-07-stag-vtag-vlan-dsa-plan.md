I have full ground truth now. Every line number and structure in the four slices checks out against the actual tree, and I found two things the slices didn't fully exploit: (1) the **nested-cascade chaining comment at `mt7530.c:2436-2439`** which proves source-port survival, and (2) the FE-side tooling (`ppe_vlan_ctrl`, `ppe_fe`) **already exists** in `econet_eth.c:459-475`. Here is the synthesis spec.

---

# XR500v PPE/HW-NAT unblock — STAG→VTAG implementation spec

## 0. THE VERDICT (read this first)

**Go with the OEM-proven STAG_VPID mechanism (Slices B+C), NOT the mainline pure-VLAN-table rebuild (Slice A).** This is a firm choice, and Slice A is actively *wrong* for our topology. Here is why, and it is the single most important reconciliation in this document:

- Slice A proposes clearing `PORT_SPEC_TAG` and building real per-port 802.1Q VLAN entries (PVID=port, VAWD2 `TAG`). On a **flat single switch** that works. On **our nested dual-MT7530** it **destroys source-port recovery**: all user ports live on the **MCM** switch; every user frame reaches the on-die switch through the **cascade on on-die port 5**. If the on-die switch VLAN-classifies by *its own* ingress port, every LAN frame becomes VID=5 — the original lan1..4 identity is gone. The **only** mechanism that carries the original user-port number across the cascade is `PORT_SPEC_TAG`, and the driver documents this explicitly:

  > `mt7530.c:2436-2439`: *"The re-constituted Special Tag loses the passthrough bit, making it impossible for the OS to distinguish between a packet from port X of the external switch, and a packet from port X of the internal."*

  i.e. the special tag **carries port number X intact across the cascade** (only the which-switch bit is lost; integrators avoid port-number collisions, which is why `mtk_conduit_find_user` is device-agnostic — `tag-mtk.c:89-102`). **`PORT_SPEC_TAG` is load-bearing and must stay set.**

- The OEM keeps `PORT_SPEC_TAG` and only **restamps the special tag's TPID to `0x8100`** via `PVC_P.STAG_VPID[31:16]` (Slice C disasm `0x879c`/`0x6c2c`: `PVC_P(6)=0x81008020`). The 4-byte special tag *becomes* a byte-legal C-VLAN: `[0x8100][TCI=portinfo]`. This is a **replacement, not a stack** — there is no second tag, so **the Golle stacked-tag trap is avoided by construction.** The FE's `CDMA1_VLAN_CTRL=0x81000001` pops it, the PPE sees `0x0800`, and the popped TCI (carrying the source port) lands in the RX descriptor.

So: **keep `PORT_SPEC_TAG`, add `STAG_VPID(0x8100)` on the on-die CPU port (port 6) only, pop with `CDMA1_VLAN_CTRL`, recover the source port from `desc.erx.tci` instead of the inline tag.**

**Honest probability: ~70% this works as-is.** The one thing that can sink it is a nested-specific unknown (does the TPID restamp apply to a tag the on-die port 6 *retrieves from switch memory* rather than generates itself? — §5/§6). The first experiment answers that for ~1 hour of work, before any driver rework, and is fully revertible. Proceed.

---

## 1. THE REGISTER RECIPE (exact writes, reconciled)

Switch register offsets are **switch-internal** (same numbers as the `MT7530_*` macros); the on-die regmap (`mt7530-mmio.c:46-61`, `reg_bits=16/val_bits=32/stride=4`) maps them onto MMIO `0xBFB58000+off`. The relevant port is the **on-die CPU port 6** (`en751221.dtsi` `port@6 ethernet=<&gmac0>`), instance `g_en751221_ondie` (`mt7530.c:2567-2568`).

### 1A. Switch side — on-die CPU port 6 only

| # | Reg | Offset (internal / MMIO phys) | Action | Value |
|---|-----|------|--------|-------|
| 1 | `MT7530_PVC_P(6)` | `0x2610` / `0xBFB5A610` | **read & save** (for revert) | — |
| 2 | `MT7530_PVC_P(6)` | `0x2610` / `0xBFB5A610` | **write** | **`0x81008020`** |

Bit decode of `0x81008020` (per `mt7530.h:300-306`):
- `STAG_VPID[31:16] = 0x8100` ← the TPID the special tag is emitted with. **This is the whole unblock.**
- `BIT(15) = 1` ← unnamed in our header; OEM sets it on every stag_to_vtag port. **Candidate "STAG-insert enable."** Flagged unknown (§6, probe #2).
- `PORT_SPEC_TAG (BIT5) = 1` ← keep special-tag mode (load-bearing for cascade).
- `VLAN_ATTR[7:6]=0` (USER), `PVC_EG_TAG[10:8]=0` (DISABLED), `ACC_FRM[1:0]=0` (ALL).

**Do NOT touch** on-die port 5 (cascade), the MCM switch, or any other port. The inter-switch tag must stay raw or the cascade breaks (§5).

### 1B. FE side — already-existing tooling, no new code

| # | Reg | Offset (fe_base+off) | Tool | Value |
|---|-----|------|------|-------|
| 3 | `CDMA1_VLAN_CTRL` | `0xBFB50400` (`EN75_CDMA1_VLAN_CTRL=0x400`, `econet_ppe.h:208`) | `echo 0x81000001 > .../ppe_vlan_ctrl` (`econet_eth.c:459-475`) | `0x81000001` |
| 4 | `GDMA1_VLAN_CHECK` | `0xBFB50510` | `echo 0x510=1 > .../ppe_fe` (verify first: `echo 0x510 > ppe_fe`) | `1` (OEM) |
| 5 | `CDMA1_FWD_CFG` | `0xBFB50500` | verify bit24 set: `echo 0x500 > ppe_fe` | (already `0x13000000`) |

`0x81000001` = TPID `0x8100` | bit0 (the operative RX-pop "urx" on EN751221 — Slice C confirmed bit0, not the header's `GDM_VLAN_URX=BIT(1)`).

**Net:** writes #1-#5. Only #2 is new switch state; #3 is a one-liner you already have; #4/#5 are verify-and-maybe-poke. **Everything is doable live before writing a single line of driver code** — except the switch poke (#2), which needs the new `gsw_reg` tool (§2).

### Unknowns that need an on-HW probe (do NOT guess — §4 first-light resolves all three)
- **U1 (critical):** does TPID-restamp apply to the memory-retrieved cascade tag on port 6? → first-light: does `tci` become non-zero and vary per source port?
- **U2:** is `BIT(15)` required, or does `STAG_VPID` alone fire? → bisect: try `0x00008020` vs `0x81008020`.
- **U3:** which descriptor half carries the VID — `tci` or `sp_tag` — and what's the VID→port map? → first-light dumps both, vary source port.

---

## 2. TOOLING FIRST — `gsw_reg` switch peek/poke (drafted, ready to paste)

`ppe_fe` reaches FE `0xBFB50000..54000` only; it **cannot** reach the switch at `0xBFB58000`. We need a switch-side equivalent. The `mt7530_read/write` helpers are `static` in `mt7530.c`, and both switch instances are already stashed in file-scope globals (`g_en751221_ondie/mcm`, `mt7530.c:2469-2470`). So the tool lives **inside `mt7530.c`**, modeled exactly on the existing `en751221_trgmii_cal` sysfs attr (`mt7530.c:2546-2556`).

Insert immediately after `DEVICE_ATTR_WO(en751221_trgmii_cal);` (`mt7530.c:2556`):

```c
/* --- XR500v: live switch register peek/poke (the ppe_fe analogue for 0xBFB58000) ---
 * which: 0 = on-die (ID_EN751221, CPU port6 -> gmac0/GDM1), 1 = MCM (ID_EN751221_EXT)
 *   peek:  echo "0 2610"            > gsw_reg ; cat gsw_reg   (or read dmesg)
 *   poke:  echo "0 2610 81008020"   > gsw_reg
 * reg is the switch-INTERNAL offset (same as the MT7530_* macros). */
static u32 g_gsw_reg_last;
static ssize_t gsw_reg_store(struct device *d, struct device_attribute *a,
			     const char *buf, size_t n)
{
	struct mt7530_priv *p;
	unsigned int which, reg, val;
	int got = sscanf(buf, "%u %x %x", &which, &reg, &val);

	if (got < 2)
		return -EINVAL;
	p = which ? g_en751221_mcm : g_en751221_ondie;
	if (!p)
		return -ENODEV;
	if (reg & 0x3 || reg > 0x7ffc)
		return -EINVAL;
	if (got == 3) {
		mt7530_write(p, reg, val);
		dev_info(p->dev, "gsw[%u] poke 0x%04x <= 0x%08x (rb=0x%08x)\n",
			 which, reg, val, mt7530_read(p, reg));
	} else {
		g_gsw_reg_last = mt7530_read(p, reg);
		dev_info(p->dev, "gsw[%u] peek 0x%04x => 0x%08x\n",
			 which, reg, g_gsw_reg_last);
	}
	return n;
}
static ssize_t gsw_reg_show(struct device *d, struct device_attribute *a, char *buf)
{
	return sysfs_emit(buf, "0x%08x\n", g_gsw_reg_last);   /* last peek, for scripts */
}
static DEVICE_ATTR_RW(gsw_reg);
```

Then register it next to the existing one in `mt7530_setup()` (`mt7530.c:2571-2576`), inside the same `g_en751221_cal_sysfs` block:

```c
	if (g_en751221_ondie && g_en751221_mcm && !g_en751221_cal_sysfs) {
		if (device_create_file(priv->dev, &dev_attr_en751221_trgmii_cal) == 0)
			g_en751221_cal_sysfs = 1;
		device_create_file(priv->dev, &dev_attr_gsw_reg);   /* <-- add */
		dev_info(priv->dev, "en751221: trgmii_cal sysfs %sready\n",
			 g_en751221_cal_sysfs ? "" : "NOT ");
	}
```

Find it at runtime: `find /sys/devices -name gsw_reg`. This is the OEM-equivalent of `ethphxcmd … vlanpt` and is the experimental workhorse. **Build/flash this + the rxdump-tci one-liner (§4 step 0) together as the "instrumentation" firmware — neither changes behavior until poked.**

---

## 3. DSA SOURCE-PORT RECOVERY (the mandatory companion, do AFTER parse is proven)

Once `CDMA1_VLAN_CTRL` pops the tag, the 4 inline bytes are gone — `mtk_tag_rcv` (`tag-mtk.c:105-132`) reads `dsa_etype_header_pos_rx` and gets the real L3 ethertype → garbage port → **all LAN RX breaks**. Recovery moves to the descriptor `tci`. **Decision: do it in the driver (`en75_rx_before_recv`), set `skb->dev` to the user port directly, bypass the inline tagger.** This is fewer moving parts than a metadata-tagger and keeps TX untouched. TX (`mtk_tag_xmit`, `tag-mtk.c:24-78`) is **unchanged** — STAG_VPID only affects switch→CPU egress.

### 3A. `econet_eth.c` — replace `en75_rx_before_recv` (`econet_eth.c:158-192`)

```c
/* Map popped VLAN/TCI -> unified DSA switch-port number.
 * EMPIRICAL: confirm the exact relation with first-light (U3). Start with the
 * same low-3-bits the inline tagger used (tag-mtk.c:123 port = hdr & GENMASK(2,0));
 * apply the XR500v logical<->physical remap if first-light shows it. */
static inline int en75_tci_to_swport(u16 tci)
{
	return tci & 0x7;
}

/* Device-agnostic user lookup across the cascaded switches (replicates the
 * static-inline mtk_conduit_find_user in gsw/tag-mtk.c:89-102, which the driver
 * cannot link to). Needs <net/dsa.h> (already pulled in via netdev_uses_dsa). */
static struct net_device *en75_find_user(struct net_device *conduit, int swport)
{
	struct dsa_port *cpu_dp = conduit->dsa_ptr;
	struct dsa_port *dp;

	if (!cpu_dp)
		return NULL;
	list_for_each_entry(dp, &cpu_dp->dst->ports, list)
		if (dp->index == swport && dp->type == DSA_PORT_TYPE_USER)
			return dp->user;
	return NULL;
}

int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb,
			enum etx_fport sport, u16 tci, bool untag)
{
	struct en75_eth_pvt *ep = (struct en75_eth_pvt *) eth;
	struct net_device *conduit = en75_get_sport_dev(ep, sport);

	/* HW-NAT path: FE popped the switch's 0x8100-stamped special tag; the
	 * source port rode in the popped TCI. Route by descriptor, not inline tag. */
	if (untag && netdev_uses_dsa(conduit)) {
		int swport = en75_tci_to_swport(tci);
		struct net_device *user = en75_find_user(conduit, swport);

		if (user) {
			skb->dev = user;
			skb->protocol = eth_type_trans(skb, user);
			skb->offload_fwd_mark = 1;   /* HW already switched within br-lan */
			return 0;
		}
		/* unknown port: fall through to conduit (won't bridge, but won't crash) */
	}

	/* Legacy / non-popped path (unchanged): conduit + inline MTK tagger. */
	skb->dev = conduit;
	skb->protocol = eth_type_trans(skb, conduit);
	return 0;
}
```

### 3B. `econet_eth.h` — signature

```c
-int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb, enum etx_fport sport);
+int en75_rx_before_recv(struct en75_eth *eth, struct sk_buff *skb,
+			enum etx_fport sport, u16 tci, bool untag);
```

### 3C. `econet_qdma.c` — caller (`econet_qdma.c:211-215`)

```c
	sport = get_erx_sport(&desc.msg.erx);
-	if (en75_rx_before_recv(q->qdma->eth, skb, sport))
+	if (en75_rx_before_recv(q->qdma->eth, skb, sport,
+				desc.msg.erx.tci, is_erx_untag(&desc.msg.erx)))
		dev_kfree_skb(skb);
	else
		napi_gro_receive(&q->napi, skb);
```

> If first-light (U3) shows the VID lands in `sp_tag` not `tci`, pass `desc.msg.erx.sp_tag` instead. `offload_fwd_mark=1` is the hardcoded equivalent of `dsa_default_offload_fwd_mark` (`tag-mtk.c:129`); refine to per-bridge later if cross-port leakage appears (§6 probe #5).

---

## 4. ORDERED EXPERIMENT PLAN (UART-primary, auto-revert gated)

**Console policy:** the decisive switch poke **will break LAN/SSH RX** the instant the FE pop is enabled (clean frames → inline tagger misroutes). **Drive and observe over UART** (the device has UART; `rxdump` `pr_info` at `econet_qdma.c:199-204` prints there for *every* received frame **before** routing, so you read the parse result even while LAN RX is dead). For any SSH-driven step, use the self-reverting wrapper below so the box heals in 30 s if you lose the link.

### Step 0 — instrumentation firmware (no behavior change)
Add to `rxdump` (`econet_qdma.c:200-204`) — append `tci`:
```c
		pr_info_ratelimited("XR500v PPE rx: crsn=%u ppe_entry=%u sport=%u ip4=%u ip6=%u l2vld=%u l4f=%u untag=%u sp_tag=%04x tci=%04x\n",
			crsn, hash, get_erx_sport(&desc.msg.erx),
			is_erx_ip4(&desc.msg.erx), is_erx_ip6(&desc.msg.erx),
			is_erx_l2vld(&desc.msg.erx), is_erx_l4f(&desc.msg.erx),
			is_erx_untag(&desc.msg.erx), desc.msg.erx.sp_tag, desc.msg.erx.tci);
```
Plus the `gsw_reg` tool (§2). Build, flash (`patch_trendchip_header.py` + `-patched.bin`, per your flash workflow), boot, **confirm LAN+SSH baseline works.** No DSA-tagger change yet.

### Step 1 — baseline capture (UART)
```sh
GSW=$(find /sys/devices -name gsw_reg)
echo 1 > /sys/module/econet_eth/parameters/ppe_rxdump
echo "0 2610" > $GSW; cat $GSW            # save ORIG PVC_P(6); expect ~0x00000020
```
From a LAN PC start `ping -f router`. UART rxdump should show the diagnosed failure: `crsn=7 ip4=0 l2vld=0 untag=0 ppe_entry=3fff sp_tag=00xx tci=0000`.

### Step 2 — THE FIRST DECISIVE EXPERIMENT (isolates "does the PPE parse?")
**This is the make-or-break test, done before ANY DSA-tagger rework.** Self-reverting wrapper (`~/tools/xr500v/scripts/stag_probe.sh`, run `setsid sh stag_probe.sh &` so it survives SSH drop):
```sh
#!/bin/sh
GSW=$(find /sys/devices -name gsw_reg)
PARM=/sys/module/econet_eth/parameters
ORIG=$(echo "0 2610" > $GSW; cat $GSW)          # save
# APPLY: switch first, then FE pop (OEM order: switch -> CDMA)
echo "0 2610 81008020" > $GSW                   # PVC_P(6) STAG_VPID=0x8100|bit15|SPEC_TAG
echo 0x81000001 > $PARM/ppe_vlan_ctrl           # CDMA1_VLAN_CTRL pop
echo 1 > $PARM/ppe_rxdump
sleep 30                                          # whole seconds only (busybox)
# AUTO-REVERT
echo 0x80000000 > $PARM/ppe_vlan_ctrl           # urx=0 (TPID kept, pop off)
echo "0 2610 $ORIG" > $GSW                       # restore PVC_P(6)
```
Generate traffic during the 30 s window; watch UART.

**SUCCESS SIGNAL (all must hold):**
| Field | Before | After (success) |
|---|---|---|
| `untag` | 0 | **1** (FE actually popped) |
| `ip4` | 0 | **1** (parser sees 0x0800 — *the core unblock*) |
| `l2vld` | 0 | **1** |
| `tci` (or `sp_tag`) | 0000 | **non-zero, and VARIES** when you ping from lan1 vs lan2 vs lan3 (→ source port survived; resolves U1+U3) |
| `ppe_entry` | const `3fff` | **varying** real FoE slot (classifier alive) |
| `crsn` | 7 | still 7 (unbound) — fine; binding is the next phase |

- If `ip4→1` but `tci` is **constant** across ports → U1 failed (restamp doesn't carry cascade port); see §5/§6 mitigation before proceeding.
- If `ip4` stays 0 → bisect U2: rerun with `0x00008020` (STAG_VPID only) then back to `0x81008020`; then try setting STAG_VPID on **on-die port 5** too (`echo "0 2510 8100xxxx"`). If still 0 → §6 probe #1 (OEM-vs-OpenWrt switch dump diff).

### Step 3 — gate: only if Step 2 shows `ip4=1` AND varying `tci`
Implement §3 (RX recovery), build, flash. Bake the §1A switch write into the driver for persistence:

**`mt7530.h:326`** — fix the broken macro (no param today):
```c
-#define  STAG_VPID			(((x) & 0xffff) << 16)
+#define  STAG_VPID(v)			(((v) & 0xffff) << 16)
+#define  PVC_STAG_INS_EN		BIT(15)   /* OEM-literal; bisected in Step 2 */
```
**`mt7530.c:1218-1220`** (`mt753x_upstream_port_enable`):
```c
	mt7530_write(priv, MT7530_PVC_P(port),
		     PORT_SPEC_TAG |
		     ((priv->id == ID_EN751221 && !dsa_is_dsa_port(ds, port)) ?
		      (STAG_VPID(0x8100) | PVC_STAG_INS_EN) : 0) |
		     (dsa_is_dsa_port(ds, port) ? PVC_PASSTHROUGH : 0));
```
**`mt7530.c:1555-1558`** (`mt7530_port_set_vlan_unaware` — the br-lan clobber site that would wipe STAG_VPID):
```c
		mt7530_write(priv, MT7530_PVC_P(cpu_dp->index), PORT_SPEC_TAG
			     | ((priv->id == ID_EN751221) ?
				(STAG_VPID(0x8100) | PVC_STAG_INS_EN) : 0)
			     | PVC_EG_TAG(MT7530_VLAN_EG_CONSISTENT)
			     | VLAN_ATTR(MT7530_VLAN_USER)
			     | MT7530_VLAN_ACC_ALL);
```
Leave `CDMA1_VLAN_CTRL` to your existing `ppe_vlan_ctrl` arming path (or set it at PPE init alongside the offload-enable). **Do not touch `mt7530_hw_vlan_add` STACK→TAG (`mt7530.c:1820`)** — that path is for VLAN-aware bridges (Slice A) which we are not using; leaving it avoids the stacked-tag trap entirely since we never build VAWD2 CPU entries.

**Success:** LAN ping both directions, 0 loss, no cross-port leakage; `rxdump` shows `ip4=1/untag=1`; SSH over LAN survives a reboot.

### Step 4 — bind & measure HW-NAT
Arm `ppe_offload_enabled`, run `bindtest`/`committest`, then iperf3 through (LAN→WAN NAT). Expect `crsn` to leave 7 for bound flows and throughput to climb past the 160 Mbit software ceiling.

---

## 5. NESTED DUAL-SWITCH — how it changes the recipe

| Aspect | Single flat switch (OEM) | Our nested dual-MT7530 |
|---|---|---|
| Where STAG_VPID is written | all ports 0..6 | **on-die port 6 ONLY** (`g_en751221_ondie`) |
| Cascade ports | none | on-die port5 + MCM port6 — **leave raw**, restamping them corrupts the inter-switch tag (`mt7530.c:2421-2444`) |
| Source-port origin | direct ingress port | MCM user port, **carried across cascade in the special tag** (number preserved, passthrough bit lost — `mt7530.c:2436-2439`) |
| Recovery space | port 0..6 | unified port number (`mtk_conduit_find_user` device-agnostic) |

**Why nesting does NOT break the approach:** the chaining comment proves the on-die port 6 special tag already contains the *original MCM user-port number* (that is exactly why today's nested DSA works). STAG_VPID only changes the **egress TPID formatting** of that tag — it does not touch the port-number content. So `tci` should equal the source port, recoverable for DSA.

**The one nested-specific risk (U1):** on-die port 6 in cascade mode *retrieves the tag from switch memory* (`mt7530.c:2433-2434`) rather than generating it. Whether the `STAG_VPID` TPID-stamp is applied to a **memory-retrieved** tag is the single unverified link. The OEM (flat switch) never exercised this path. **Step 2 answers it directly** (varying `tci` = pass). If it fails: the fallback is to also stamp on-die **port 5** (the link port that pops+stores the tag), tried cheaply via `gsw_reg`; if that also fails, the approach hits a wall here (see §6 #1).

---

## 6. TOP 5 RISKS + cheapest probe + honest call

| # | Risk | Cheapest HW probe | Verdict |
|---|------|------------------|---------|
| **1** | **TPID restamp not applied to memory-retrieved cascade tag** (U1) → `tci` constant, no per-port info | Step 2: ping from 2 different LAN ports, compare `tci`. ~10 min, revertible | **The wall, if any.** ~70% it's fine (egress formatting ≠ tag content). If it fails, try STAG_VPID on on-die port5 too; if still flat, this design is blocked and you'd need Caleb's MDIO-master/private path. |
| **2** | `BIT(15)` semantics unknown — `STAG_VPID` alone may not insert | Step 2 bisect `0x00008020` vs `0x81008020` via `gsw_reg`. ~5 min | Low. OEM literal `0x81008020` is known-good; just replicate it. |
| **3** | Wrong descriptor half / VID→port map needs XR500v remap (U3) | Step 0 rxdump prints both `sp_tag`+`tci`; vary source port. Free | Low. Worst case is a lookup table (`en75_tci_to_swport`), exactly like OEM `init_ethernet_port_map`. |
| **4** | The pop/restamp affects **non-IP** (ARP/STP/DHCP) and **WAN GDM2** paths, breaking L2/management | UART: confirm ARP (`untag=1, ip4=0`) still routes via `tci` branch; check GDM2 untouched (only on-die port6/GDM1 changed) | Low. §3 routes by `tci` regardless of `ip4`; GDM2/WAN egress is a different conduit, unaffected. |
| **5** | `offload_fwd_mark=1` hardcode causes cross-port leak or blocks CPU-destined frames in br-lan | After Step 3: ping lan2↔lan3 and verify isolation; tcpdump for leaks | Medium-low. If leak, compute mark per-bridge from the user dp instead of constant 1. |

**Bottom line:** The mechanism is sound and OEM-proven; the Golle stacked-tag trap is **avoided by construction** (replacement, single tag). VID=srcport **is** recoverable on the nested setup — the cascade preserves the port number (`mt7530.c:2436-2439`) and STAG_VPID only reformats the TPID. The entire FE half plus the decisive switch experiment are doable with one instrumentation flash and zero tagger rework, fully revertible over UART. **Recommendation: build Step 0 firmware (gsw_reg + rxdump-tci) and run Step 2 immediately — it converts the 70% into a yes/no in under an hour and tells you, before you write the RX recovery, whether to commit or escalate to Caleb.**

### Key files / lines (all verified this session)
- `gsw/mt7530.c`: `mt753x_upstream_port_enable` **1218-1220** (patch), `mt7530_port_set_vlan_unaware` CPU rewrite **1555-1558** (patch), `mt7530_hw_vlan_add` STACK **1820** (leave), chaining/source-port comment **2421-2444**, globals **2469-2470**, trgmii sysfs pattern **2546-2576** (model gsw_reg on this), id stash **2567-2570**.
- `gsw/mt7530.h`: `STAG_VPID` broken macro **326** (fix), `PVC_P` **300-306**, `PORT_SPEC_TAG`/`PVC_PASSTHROUGH` **301-302**.
- `gsw/mt7530-mmio.c`: regmap **40-61** (proves switch=MMIO `0xBFB58000`, internal-offset addressing).
- `gsw/tag-mtk.c`: `mtk_tag_rcv` inline-port **105-132/123** (breaks post-pop), `mtk_conduit_find_user` **89-102** (replicated in driver), `mtk_tag_xmit` **24-78** (unchanged).
- `econet_qdma.c`: rxdump **199-204** (add tci), rx_before_recv call **211-215** (patch).
- `econet_eth.c`: `en75_rx_before_recv` **158-192** (rewrite), `en75_get_sport_dev` **146-156**, existing FE tools — `ppe_rxdump` **420**, `ppe_vlan_ctrl` **459-475**, `ppe_fe` **477+**.
- `econet_ppe.h`: `EN75_CDMA1_VLAN_CTRL=0x400` **208**.
- `qdma_desc.h`: `qdma_desc_erx` struct **245-251** (`sp_tag`/`tci`), `ERX_IP4/L2VLD/UNTAG` **257/260/330**, accessors **278-342**.
- OEM evidence: `/home/cristuu/tools/xr500v/iter54_prep/stock_eth_disasm.txt` `macEN7512STagEnable` `0x6c2c`, `switch_vlan_setting` `0x879c` (PVC_P=`0x81008100`/`0x81008020`).