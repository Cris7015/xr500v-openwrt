I now have full ground truth on the P4 API and all four target files. Here is the integrated master plan.

---

# XR500v PPE Phase 5 — Master Implementation Plan (flow binding + TC/flowtable offload)

Synthesized from slices A–D, **re-grounded against the actual tree** (`econet-eth-2026.02.13~c2f855cf`, P4 API at `/home/cristuu/tools/xr500v/ppe-p4/`). Three corrections to the slice assumptions, applied throughout:

- **The FoE table is `dmam_alloc_coherent`** (`econet_eth.c:403`), phys is dynamic — the "0x02e00000" in the brief is a stale observed value, never hardcode it. `ppe->foe_phys` comes from the allocator.
- **`ports[]` lives in the *private* `en75_eth_pvt`** (`econet_eth.c:97`), not the public `struct en75_eth` (`econet_eth.h:162`). Other TUs reach the conduit only through `ppe->conduit`, set inside `en75_ppe_init` (which already runs with `en75_eth_pvt*` in scope).
- **There is a dedicated `ppe_accounting` MMIO block** (`regs->ppe_accounting`, BFB52000, `econet_eth.c:130`) separate from `regs->ppe` — that is where deferred MIB lands, no new ioremap needed.
- **Reuse P4's `en75_foe_entry_commit` as the HW-write primitive** (`econet_ppe.c:113`). Do **not** introduce a second `__en75_foe_entry_commit` (Slice A duplicated it). It already does payload-then-ib1-then-`dma_wmb`-then-cache-clear correctly; the only gap is the hardcoded `timestamp=0`, fixed in patch 358.

---

## 1. Patch layout (ordered, each builds + flashes + tests behind a default-off gate)

**Recommendation: SPLIT the RX-check from the flowtable.** The RX-bind core lands in **357**, the automatic TC/flowtable in **359**. They are bridged by a *manual* register-one-flow debug param in 357, so the high-risk egress/tag question (Slice D) and the register→bind mechanism are both validated on real traffic **before** any TC plumbing exists. This isolates "did the special tag blackhole it?" from "did the flowtable build the wrong entry?" — you never debug both at once.

| # | Goal (smallest safe increment) | Gate / how it's inert | Forwarding blast radius |
|---|---|---|---|
| **356** | Refactor 3 file-statics → `struct en75_ppe` hung off `struct en75_eth`; alloc `foe_flow[8192]` + `foe_check_time[16384]` + lock + `conduit` + `fe_base`. **No behavior change.** | `ppe_engine`/`ppe_test` still work identically; nothing reads `foe_flow` yet | zero (pure plumbing) |
| **357** | FoE-flow core (`register`/`match`/`__check_skb`/`clear`/`flush`) + V1 builders (`set_dsa`/`set_queue`/`set_pppoe`/`set_vlan`) + RX-bind hook in `econet_qdma.c` + manual `ppe_committest` (direct STATIC commit) and `ppe_bindtest` (register + arm gate) | RX hook gated by `en75_ppe_offload_enabled` (default 0); core funcs are non-static so no `-Wunused`; only the test params exercise it | zero until a test param is poked |
| **358** | Wire `en75_eth_timestamp()` to the EN751221 FE free-running counter; `en75_foe_entry_commit` stamps live time | Only affects entries committed while a test is armed | zero until armed |
| **359** | `econet_ppe_offload.c`: TC/flowtable (`replace`/`destroy`/`stats`/`cmd`), `ndo_setup_tc`, `NETIF_F_HW_TC` on GDM1, `en75_flow_resolve_dest`, `ppe_offload` module-param gate + `flow_indr_dev_register` | `ppe_offload` default 0; `NETIF_F_HW_TC` advertised but the gate decides whether flows bind | zero until `ppe_offload=1` |
| **360** (deferred) | MIB byte/packet accounting via `ppe_accounting` block for accurate `tc`/`conntrack` stats | additive; idle-based keepalive already works without it | none |

Hard prerequisite chain: **356 → 357 → (358 ∥ 359)**. 358 and 359 are independent but 359's dynamic flows will age out in seconds without 358, so **flash 358 before turning 359's firehose on**.

---

## 2. Files & function lists per patch

### Patch 356 — `struct en75_ppe` plumbing
- **`econet_eth.h`**: fwd-decl `struct en75_ppe;`; add `struct en75_ppe *ppe;` to `struct en75_eth` (line 162-164).
- **`econet_ppe.h`**: define `struct en75_ppe` (full, §3); `EN75_PPE_HASH_OFFSET 2`, `EN75_PPE_NUM_FLOWS (EN75_PPE_ENTRIES/2)` = 8192; proto `struct en75_ppe *en75_ppe_attach(struct device *, void __iomem *base, void __iomem *fe_base, void *foe_virt, dma_addr_t foe_phys, struct net_device *conduit);`.
- **`econet_ppe.c`**: `en75_ppe_attach()` — `devm_kzalloc` the struct, `devm_kcalloc` `foe_flow` (8192) and `foe_check_time` (16384 × u16), `spin_lock_init(&ppe->lock)`, stash pointers.
- **`econet_eth.c`**: delete `en75_ppe_foe_virt`/`en75_ppe_foe_phys`/`en75_ppe_base` statics; add `static struct en75_ppe *en75_the_ppe;`; in `en75_ppe_init` call `en75_ppe_attach(eth->pub.dev, base, (void __iomem*)eth->regs->fe, tbl, phys, eth->ports[0])`, store to `eth->pub.ppe` and `en75_the_ppe`; rewrite the `ppe_engine`/`ppe_test` param bodies to use `en75_the_ppe->base`/`->foe_virt`; `WARN_ON(...fport != ETX_FPORT_GDM1)`.

### Patch 357 — FoE-flow core + builders + RX hook + manual tests
- **`econet_ppe.h`**: `struct en75_flow_entry` (§3); `extern bool en75_ppe_offload_enabled;`; protos `en75_foe_entry_register`, `en75_foe_entry_unregister`, `en75_foe_entry_clear`, `__en75_ppe_check_skb`, `en75_ppe_flush_all`, `en75_foe_entry_get_stats`; new builders `en75_foe_entry_set_dsa(entry,port,passthrough)`, `en75_foe_entry_set_queue(entry,q)`, `en75_foe_entry_set_pppoe(entry,sid)`, `en75_foe_entry_set_vlan(entry,vid)`; the `static inline en75_ppe_check_skb()` rate-limiter (calls `__en75_ppe_check_skb`).
- **`econet_ppe.c`**: `bool en75_ppe_offload_enabled;` (default 0); `en75_flow_entry_match()`; `en75_foe_entry_register()`; `__en75_ppe_check_skb()`; `en75_foe_entry_clear()`/`unregister()`; `en75_ppe_flush_all()`; `en75_foe_entry_get_stats()`; the 4 builders; test helpers `en75_ppe_committest(ppe, sip,dip,sport,dport,proto, dsa_port,passthrough)` (direct commit, sets `MTK_FOE_IB1_STATIC`) and `en75_ppe_bindtest_register(ppe, …same…)` (register + `WRITE_ONCE(en75_ppe_offload_enabled,true)`).
- **`econet_qdma.c`**: `#include "econet_ppe.h"`; the RX-hook reorder/replace (§5).
- **`econet_eth.c`**: add `module_param_cb(ppe_committest…)` and `module_param_cb(ppe_bindtest…)` that call the two helpers via `en75_the_ppe` (compiled-in test 5-tuple constants, edited per first-light run).

### Patch 358 — aging timestamp
- **`econet_ppe.c`**: `static u16 en75_eth_timestamp(struct en75_ppe *ppe)` → `readl(ppe->fe_base + EN75_FE_TS_OFF) & MTK_FOE_IB1_BIND_TIMESTAMP`; `en75_foe_entry_commit` calls it instead of `timestamp=0`.
- **`econet_ppe.h`**: `#define EN75_FE_TS_OFF 0x0010` (mainline `mtk_eth_timestamp` reg — **CONFIRM on HW**, §8).

### Patch 359 — automatic TC/flowtable offload
- **NEW `econet_ppe_offload.c`**: `struct en75_flow_data`; `en75_flow_mangle_eth/ports/ipv4`; `en75_flow_resolve_dest` (§4); `en75_flow_offload_replace` (§6); `en75_flow_offload_destroy/stats/cmd`; `en75_ppe_setup_tc_block` + `_cb`; indirect: `en75_ppe_indr_setup_tc_cb`/`_block_cleanup`; `en75_ppe_offload_disable`; `module_param_cb(ppe_offload…)`.
- **`econet_ppe.h`**: protos `en75_ppe_setup_tc_block`, `en75_ppe_offload_disable`; `extern bool en75_ppe_engine_is_on(void); extern bool en75_ppe_steer_is_on(void); extern struct en75_ppe *en75_get_ppe(void);`.
- **`econet_eth.c`**: `bool en75_ppe_engine_is_on(void){ return en75_ppe_engine; }`; `struct en75_ppe *en75_get_ppe(void){ return en75_the_ppe; }`; call `en75_ppe_offload_disable(en75_the_ppe)` first in `en75_remove`.
- **`econet_port.c`**: `bool en75_ppe_steer_is_on(void){ return en75_ppe_steer; }`; `en75_dev_setup_tc()`; add `.ndo_setup_tc` to `en75_netdev_ops` (line 456-464); `NETIF_F_HW_TC` on GDM1 in `en75_alloc_gdm_port` (line 510).
- **`Kbuild`**: `econet-eth-y := … econet_ppe.o econet_ppe_offload.o`.

---

## 3. Final reconciled structs (A ⊕ C)

```c
/* econet_ppe.h */
#define EN75_PPE_HASH_OFFSET	2				/* mt7622/V1; matches hash<<=1 */
#define EN75_PPE_NUM_FLOWS	(EN75_PPE_ENTRIES / EN75_PPE_HASH_OFFSET)	/* 8192 */

struct en75_ppe {
	struct device		*dev;
	void __iomem		*base;		/* = eth->regs->ppe  (PPE reg block)        */
	void __iomem		*fe_base;	/* = eth->regs->fe   (aging timestamp ctr)  */
	void			*foe_virt;	/* 16384*80B coherent FoE table (P4 alloc)  */
	dma_addr_t		foe_phys;
	struct net_device	*conduit;	/* GDM1 / eth0 (= eth->ports[0])            */

	spinlock_t		lock;		/* protects foe_flow + FoE HW writes; taken
						 * spin_lock_bh in BOTH RX(softirq) + offload(proc) */
	struct hlist_head	*foe_flow;	/* [EN75_PPE_NUM_FLOWS] = 8192 SW flow buckets */
	u16			*foe_check_time;/* [EN75_PPE_ENTRIES] = 16384, raw-hash indexed
						 * (separate devm_kcalloc, NOT 32KB inline)        */

	struct rhashtable	flow_table;	/* en75_flow_entry keyed by tc cookie (359)  */
	struct list_head	block_cb_list;	/* tc block cbs (359)                         */
};

struct en75_flow_entry {
	struct hlist_node	list;	/* foe_flow[hash>>1] linkage (RX bind)        */
	struct rhash_head	node;	/* flow_table, keyed by cookie (359)          */
	unsigned long		cookie;
	struct mtk_foe_entry	data;	/* prepared 80B FoE template (P4 struct)      */
	u16			hash;	/* 0xffff until __en75_ppe_check_skb binds it */
	u64			prev_packets, prev_bytes;	/* stats deltas (360)         */
	u64			packets, bytes;
};
```

Reconciliation calls made: **single lock named `lock`** (drop A's per-file `ppe_lock`); **`foe_virt`** (P4's name, drop A's `foe_table`); **`foe_check_time` is a heap pointer** not a 32KB inline array (avoids an order-3 `kzalloc`); **dropped A's `type`/`ppe_index`** from the flow entry (always L4 / single PPE — match is IPv4-only constant); **kept C's `node`/stats** so the offload + stats paths compile; **added `fe_base`** in 356 so 358 is a one-liner.

---

## 4. Port-identity decision (Slice D) — concrete recommendation + first-light experiment

**Recommendation, committed:** every offloaded wired flow gets **`IB2.DEST_PORT = GDM1 = 1`** (single conduit; GDM2 dead), **plus** an explicit MTK special tag via `en75_foe_entry_set_dsa(entry, dp->index, dp->ds->index != 0)`. The novel divergence from mainline is the **PASSTHROUGH bit (BIT 7)** for any egress port on the **cascaded second MT7530** (`dp->ds->index != 0`) — mainline is single-switch and never sets it; without it, downstream-port flows **bind (crsn=HIT, TB_USED++) but silently blackhole**.

```c
/* econet_ppe.c (patch 357) — V1 special-tag encoder, nested-switch aware.
 * Mirrors gsw/tag-mtk.c mtk_tag_xmit; cf mainline mtk_foe_entry_set_dsa
 * (mtk_ppe.c:384-399) which OMITS BIT(7). */
void en75_foe_entry_set_dsa(struct mtk_foe_entry *entry, int port, bool passthrough)
{
	struct mtk_foe_mac_info *l2 = &entry->ipv4.l2;
	u16 tag = BIT(port) & 0x3f;			/* DP bitmap, GENMASK(5,0) */

	if (passthrough)
		tag |= BIT(7);				/* MTK_HDR_XMIT_PASSTHROUGH (2nd switch) */
	l2->etype = tag;

	if (!(entry->ib1 & MTK_FOE_IB1_BIND_VLAN_LAYER))
		entry->ib1 |= FIELD_PREP(MTK_FOE_IB1_BIND_VLAN_LAYER, 1);
	else
		l2->etype |= BIT(8);			/* combined special-tag + 802.1Q */
	entry->ib1 &= ~MTK_FOE_IB1_BIND_VLAN_TAG;
}
```

```c
/* econet_ppe_offload.c (patch 359) — egress identity resolver. */
struct en75_flow_dest { u8 pse_port; u8 dsa_port; bool has_dsa; bool passthrough; int queue; };

static int en75_flow_resolve_dest(struct en75_ppe *ppe, struct net_device *dev,
				  struct en75_flow_dest *d)
{
	memset(d, 0, sizeof(*d));
	d->pse_port = EN75_PSE_GDM1_PORT;		/* 1 */
	if (!dev)
		return -ENODEV;
#if IS_ENABLED(CONFIG_NET_DSA)
	{
		struct dsa_port *dp = dsa_port_from_netdev(dev);
		if (!IS_ERR(dp)) {
			if (dp->cpu_dp->tag_ops->proto != DSA_TAG_PROTO_MTK)
				return -EOPNOTSUPP;		/* must be gsw/tag-mtk.c */
			if (dsa_port_to_conduit(dp) != ppe->conduit)
				return -EOPNOTSUPP;		/* nested tree => one conduit (GDM1) */
			d->has_dsa     = true;
			d->dsa_port    = dp->index;		/* per-switch port idx */
			d->passthrough = (dp->ds->index != 0);	/* cascaded switch */
			d->queue       = 3 + dp->index;		/* mainline rule (mtk_ppe_offload.c:235) */
			return 0;
		}
	}
#endif
	if (dev == ppe->conduit) { d->queue = d->pse_port - 1; return 0; } /* bare eth0 */
	return -EOPNOTSUPP;				/* GDM2 / br-lan / foreign */
}
```

**First-light experiment (do this in 357, before any TC):** hand-commit ONE entry for a **real** live flow (read 5-tuple from `conntrack -L`, next-hop MACs from `ip neigh`, egress `dp->index`/`dp->ds->index`), with `MTK_FOE_IB1_STATIC` so aging is out of the picture, and use **"does the far side keep receiving"** as the oracle. Run it as a 3-way A/B against a **switch-0** egress port first (isolates passthrough from tag):

| Variant | `set_dsa` | passthrough | PASS ⇒ |
|---|---|---|---|
| A | none (pse_port=GDM1 only) | — | switch FDB-forwards untagged CPU frames → tag optional (unlikely; ship simplest) |
| B | `BIT(port)` | no | standard tag works on switch-0 → `set_dsa` mandatory |
| C | `BIT(port)\|BIT(7)` | yes | required on switch-1 ports |

Prior: **A fails, B passes**. Then B-vs-C against a **switch-1** port confirms C. This directly sets the `passthrough = (dp->ds->index != 0)` rule. Prefer **internet→LAN (no PPPoE push)** for the very first commit — it defers the fragile PPPoE-over-DSA egress encoding to a later run.

---

## 5. Literal RX-hook diff — `econet_qdma.c` `en75_qdma_rx_process_one` (lines 184-205)

**BEFORE** (verbatim from the tree):
```c
	__skb_put(skb, len);
	skb_mark_for_recycle(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	hash = get_erx_ppe_entry(&desc.msg.erx);
	skb_set_hash(skb, jhash_1word(hash, 0),
		     PKT_HASH_TYPE_L4);
	{ /* XR500v PPE 2a: confirm the HW tags crsn/ppe_entry on RX */
	  u8 __crsn = get_erx_crsn(&desc.msg.erx);
	  pr_info_ratelimited("XR500v PPE rx: crsn=%u ppe_entry=%u sport=%u\n",
		  __crsn, hash, get_erx_sport(&desc.msg.erx)); }

	/* TODO: When we begin supporting the PPE, we will handle
	 *       PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED here. */

	sport = get_erx_sport(&desc.msg.erx);
	if (en75_rx_before_recv(q->qdma->eth, skb, sport))
		dev_kfree_skb(skb);
	else
		napi_gro_receive(&q->napi, skb);

	return;
```

**AFTER:**
```c
	__skb_put(skb, len);
	skb_mark_for_recycle(skb);
	skb->ip_summed = CHECKSUM_UNNECESSARY;

	hash = get_erx_ppe_entry(&desc.msg.erx);
	skb_set_hash(skb, jhash_1word(hash, 0),
		     PKT_HASH_TYPE_L4);

	/* en75_rx_before_recv() assigns skb->dev + runs eth_type_trans(); it is
	 * also the only step that can reject the skb. Keep it FIRST. */
	sport = get_erx_sport(&desc.msg.erx);
	if (en75_rx_before_recv(q->qdma->eth, skb, sport)) {
		dev_kfree_skb(skb);
		return;
	}

	/* PPE RX bind: a steered-but-unbound flow is kicked to the CPU. On
	 * EN751221 that crsn is NO_FLOW(0x07) (mainline normally uses
	 * HIT_UNBIND_RATE_REACHED(0x0f) -- accept both). 'hash' is the RAW
	 * engine FoE slot. Runs in threaded-NAPI: en75_ppe_check_skb() takes
	 * ppe->lock with spin_lock_bh and does NOT consume the skb. */
	if (READ_ONCE(en75_ppe_offload_enabled) && q->qdma->eth->ppe) {
		u8 crsn = get_erx_crsn(&desc.msg.erx);

		if (crsn == MTK_PPE_CPU_REASON_NO_FLOW ||
		    crsn == MTK_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED)
			en75_ppe_check_skb(q->qdma->eth->ppe, skb, hash);
	}

	napi_gro_receive(&q->napi, skb);
	return;
```

Drops the 2a printk + stale `else`; keeps `skb_set_hash`; matches mainline `mtk_poll_rx` ordering (set dev/eth_type_trans → ppe_check_skb → napi_gro_receive).

**`ndo_setup_tc` wiring (`econet_port.c`, patch 359):**
```c
static int en75_dev_setup_tc(struct net_device *dev, enum tc_setup_type type, void *type_data)
{
	struct en75_gdm_port *port = netdev_priv(dev);

	if (port->fport != ETX_FPORT_GDM1)	/* GDM1 is the conduit for every lanN + pppoe-wan;
						 * DSA forwards TC_SETUP_FT here. GDM2 unused. */
		return -EOPNOTSUPP;
	switch (type) {
	case TC_SETUP_BLOCK:
	case TC_SETUP_FT:
		return en75_ppe_setup_tc_block(en75_get_ppe(), dev, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
/* en75_netdev_ops += .ndo_setup_tc = en75_dev_setup_tc, */
```
```c
/* en75_alloc_gdm_port, replacing `ndev->hw_features = 0;` (line 510) */
	ndev->hw_features = 0;
	if (fport == ETX_FPORT_GDM1)
		ndev->hw_features |= NETIF_F_HW_TC;	/* conduit only */
	ndev->features |= ndev->hw_features;
```
After DSA attaches, **verify `ethtool -k eth0 | grep hw-tc-offload` is still `on`**; if DSA strips it, re-assert in `en75_dev_init` (ndo_init runs after the netdev is built).

---

## 6. The load-bearing function — `en75_flow_offload_replace` (full, P4-adapted)

```c
/* econet_ppe_offload.c (patch 359) — V1 IPv4 HNAPT/ROUTE + NAT, GDM1 conduit only.
 * Two-stage: this REGISTERS the NAT-aware orig+new tuple in foe_flow; the RX hook
 * (__en75_ppe_check_skb) performs the real HW bind on the first matching packet.
 * NO HW write happens here. */
#define EN75_PSE_GDM1_PORT	1

static int en75_flow_offload_replace(struct en75_ppe *ppe, struct flow_cls_offload *f)
{
	struct flow_rule *rule = flow_cls_offload_flow_rule(f);
	struct flow_action_entry *act;
	struct en75_flow_data data = {};
	struct en75_flow_dest dst;
	struct mtk_foe_entry foe;
	struct en75_flow_entry *entry;
	struct net_device *odev = NULL;
	u16 addr_type = 0;
	u8 l4proto = 0;
	int offload_type, err, i;

	if (rhashtable_lookup(&ppe->flow_table, &f->cookie, en75_flow_ht_params))
		return -EEXIST;

	if (!flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_META))
		return -EOPNOTSUPP;		/* single PPE: no ppe_idx remap (v2-only) */

	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_CONTROL)) {
		struct flow_match_control m;
		flow_rule_match_control(rule, &m);
		addr_type = m.key->addr_type;
		if (flow_rule_has_control_flags(m.mask->flags, f->common.extack))
			return -EOPNOTSUPP;
	} else {
		return -EOPNOTSUPP;
	}
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic m;
		flow_rule_match_basic(rule, &m);
		l4proto = m.key->ip_proto;
	} else {
		return -EOPNOTSUPP;
	}
	if (addr_type != FLOW_DISSECTOR_KEY_IPV4_ADDRS)
		return -EOPNOTSUPP;		/* V1 router: IPv4 only (L2/bridge + IPv6 dropped) */
	offload_type = MTK_PPE_PKT_TYPE_IPV4_HNAPT;

	/* PASS 1: egress L2 rewrite + redirect target + encap push */
	flow_action_for_each(i, act, &rule->action) {
		switch (act->id) {
		case FLOW_ACTION_MANGLE:
			if (act->mangle.htype == FLOW_ACT_MANGLE_HDR_TYPE_ETH)
				en75_flow_mangle_eth(act, &data.eth);
			break;
		case FLOW_ACTION_REDIRECT:
			odev = act->dev;
			break;
		case FLOW_ACTION_CSUM:
		case FLOW_ACTION_VLAN_POP:
			break;
		case FLOW_ACTION_VLAN_PUSH:
			if (data.vlan.num + data.pppoe.num == 2 ||
			    act->vlan.proto != htons(ETH_P_8021Q))
				return -EOPNOTSUPP;
			data.vlan.vlans[data.vlan.num].id = act->vlan.vid;
			data.vlan.num++;
			break;
		case FLOW_ACTION_PPPOE_PUSH:
			if (data.pppoe.num == 1 || data.vlan.num == 2)
				return -EOPNOTSUPP;
			data.pppoe.sid = act->pppoe.sid;
			data.pppoe.num++;
			break;
		default:
			return -EOPNOTSUPP;
		}
	}

	if (!is_valid_ether_addr(data.eth.h_source) ||
	    !is_valid_ether_addr(data.eth.h_dest))
		return -EINVAL;

	/* bake egress MACs (pse_port resolved later; pass 0 now) */
	err = en75_foe_entry_prepare(&foe, offload_type, l4proto, 0,
				     data.eth.h_source, data.eth.h_dest);
	if (err)
		return err;

	/* ORIG (pre-NAT) 5-tuple from match keys -> hashed + matched on RX */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports p;
		flow_rule_match_ports(rule, &p);
		data.src_port = p.key->src;
		data.dst_port = p.key->dst;
	} else {
		return -EOPNOTSUPP;
	}
	{
		struct flow_match_ipv4_addrs a;
		flow_rule_match_ipv4_addrs(rule, &a);
		data.src_addr = a.key->src;
		data.dst_addr = a.key->dst;
		en75_foe_entry_set_ipv4_tuple(&foe, false,	/* -> ipv4.orig */
			data.src_addr, data.src_port, data.dst_addr, data.dst_port);
	}

	/* PASS 2: NAT L3/L4 mangle -> mutate data.* to POST-NAT values */
	flow_action_for_each(i, act, &rule->action) {
		if (act->id != FLOW_ACTION_MANGLE)
			continue;
		switch (act->mangle.htype) {
		case FLOW_ACT_MANGLE_HDR_TYPE_TCP:
		case FLOW_ACT_MANGLE_HDR_TYPE_UDP:
			err = en75_flow_mangle_ports(act, &data); break;
		case FLOW_ACT_MANGLE_HDR_TYPE_IP4:
			err = en75_flow_mangle_ipv4(act, &data); break;
		case FLOW_ACT_MANGLE_HDR_TYPE_ETH:
			break;			/* applied in PASS 1 */
		default:
			return -EOPNOTSUPP;
		}
		if (err)
			return err;
	}

	/* NEW (post-NAT) 5-tuple -> ipv4.new */
	en75_foe_entry_set_ipv4_tuple(&foe, true,
		data.src_addr, data.src_port, data.dst_addr, data.dst_port);

	/* egress encap stack */
	for (i = 0; i < data.vlan.num; i++)
		en75_foe_entry_set_vlan(&foe, data.vlan.vlans[i].id);
	if (data.pppoe.num == 1)
		en75_foe_entry_set_pppoe(&foe, data.pppoe.sid);

	/* PORT IDENTITY (Slice D): odev -> GDM1 + special tag + queue */
	err = en75_flow_resolve_dest(ppe, odev, &dst);
	if (err)
		return err;
	if (dst.has_dsa)
		en75_foe_entry_set_dsa(&foe, dst.dsa_port, dst.passthrough);
	en75_foe_entry_set_queue(&foe, dst.queue);
	en75_foe_entry_set_pse_port(&foe, dst.pse_port);	/* GDM1=1 */

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->cookie = f->cookie;
	memcpy(&entry->data, &foe, sizeof(entry->data));

	err = en75_foe_entry_register(ppe, entry);	/* hash + hlist_add; NO HW write */
	if (err < 0)
		goto free;
	err = rhashtable_insert_fast(&ppe->flow_table, &entry->node, en75_flow_ht_params);
	if (err < 0)
		goto unreg;
	return 0;

unreg:
	en75_foe_entry_unregister(ppe, entry);
free:
	kfree(entry);
	return err;
}
```

**Ordering invariants baked in** (the easy way to break this): eth-mangle (PASS 1) must precede `prepare` — wrong, it precedes `prepare`'s *consumption* of `data.eth`, so PASS 1 fills `data.eth` then `prepare` reads it: correct as written. Orig tuple is captured into `foe.ipv4.orig` (by value) *before* PASS 2 mutates `data.*`; new tuple read *after*. `set_dsa`/`set_pppoe`/`set_vlan` touch only egress `l2`/`ib1` (not the orig tuple) so they don't perturb the hash.

Supporting core (the register + bind, `econet_ppe.c`, patch 357):
```c
int en75_foe_entry_register(struct en75_ppe *ppe, struct en75_flow_entry *e)
{
	u32 hash = en75_ppe_hash_entry(&e->data);	/* already <<1, even, [0..16383] */
	e->hash = 0xffff;
	spin_lock_bh(&ppe->lock);
	hlist_add_head(&e->list, &ppe->foe_flow[hash / EN75_PPE_HASH_OFFSET]);	/* bucket=hash>>1 */
	spin_unlock_bh(&ppe->lock);
	return 0;
}

static bool en75_flow_entry_match(struct en75_flow_entry *e, struct mtk_foe_entry *foe)
{
	/* IPv4-only: key = orig 5-tuple, ends at ipv4.ib2 (=16); compare 12B after ib1 */
	if ((foe->ib1 ^ e->data.ib1) & MTK_FOE_IB1_UDP)
		return false;
	return !memcmp(&e->data.data, &foe->data,
		       offsetof(struct mtk_foe_entry, ipv4.ib2) - sizeof(foe->ib1));
}

void __en75_ppe_check_skb(struct en75_ppe *ppe, struct sk_buff *skb, u16 hash)
{
	struct mtk_foe_entry *hwe =
		(struct mtk_foe_entry *)((u8 *)ppe->foe_virt + (u32)hash * EN75_FOE_ENTRY_SIZE);
	struct hlist_head *head = &ppe->foe_flow[hash / EN75_PPE_HASH_OFFSET];
	struct en75_flow_entry *e;
	struct hlist_node *n;
	bool found = false;

	spin_lock_bh(&ppe->lock);
	if (FIELD_GET(MTK_FOE_IB1_STATE, hwe->ib1) == MTK_FOE_STATE_BIND)
		goto out;				/* engine already bound this slot */
	hlist_for_each_entry_safe(e, n, head, list) {
		if (found || !en75_flow_entry_match(e, hwe)) {
			if (e->hash != 0xffff)		/* drop a stale dup binding */
				en75_foe_entry_clear(ppe, e, false);
			continue;
		}
		e->hash = hash;				/* RAW engine slot */
		en75_foe_entry_commit(ppe->foe_virt, ppe->base, &e->data, hash); /* P4 HW write */
		found = true;
	}
out:
	spin_unlock_bh(&ppe->lock);
}
/* en75_foe_entry_clear() here must NOT re-take ppe->lock (caller holds it);
 * give it a __locked variant, and have the public en75_foe_entry_unregister()
 * take the lock + call the __locked body + hlist_del_init(). */
```

---

## 7. Ordered HW validation per patch (router-mode, speedtest rig)

Standing setup before each flash: netconsole→.8 primary + ramoops (per panic-capture playbook); all `ppe_*` params default 0; recovery = **power cycle** (the `boot` cmd crashes), bflag stays 1 (OpenWrt). UBI overlay survives flash.

**356 (plumbing):**
1. Flash, boot. `dmesg | grep "PPE 1a"` — same `TB_BASE_rb`/`TB_CFG_rb`/`GLO_CFG_rb` as before the refactor.
2. `echo 1 > …/ppe_test` → the P4 readback line still prints `ib1=21400000 ib2=007c0020 … etype=0800`. **Success = byte-identical readback** (proves the struct move didn't disturb the FoE write path).
3. Normal LAN↔WAN traffic unaffected. Rollback: power cycle (params off ⇒ clean).

**357 (core + RX hook + manual tests):** *engine + steer must be ON for these.*
1. `echo 1 > ppe_engine; echo 1 > ppe_steer` → `dmesg`: `GLO_CFG=0x…20d`, steer "→PPE(4)". Baseline: the RX printk is gone; instead start a download and confirm `en75_ppe_offload_enabled` is still 0 ⇒ traffic flows normally on CPU (SEARCH_MISS→FORWARD_BUILD→CPU per packet).
2. **Egress/tag (Slice D variant B, switch-0 port):** read a live `internet→LAN-host` flow from `conntrack -L`/`ip neigh`, set the compiled-in constants, `echo 1 > ppe_committest`. Start a single long iperf3/large download to that host.
   - **PASS:** throughput holds for >30 s, `sirq` on the box drops, the committed slot's `crsn` reads **HIT** (add a transient counter or one-shot printk), `TB_USED` (0x224) shows the slot.
   - **FAIL (tag mis-deliver):** the instant it binds, throughput→0 **while crsn shows HIT** ⇒ wrong egress tag. Re-run variant A then C. A debugfs "clear slot" unbinds and the flow must recover (proves the bind caused it).
3. **Register→bind mechanism:** clear, then `echo 1 > ppe_bindtest` (registers the same flow + arms the gate). Generate matching traffic.
   - **PASS:** first packet logs `crsn=NO_FLOW`, then the slot flips to STATE=BIND, throughput sustains, subsequent packets stop hitting the CPU. **This proves `__en75_ppe_check_skb` end-to-end.**
4. Rollback: `echo 0 > ppe_bindtest`/`ppe_steer`/`ppe_engine`, or power cycle.

**358 (timestamp):** repeat 357-step-2 with a **non-STATIC** commit. **PASS = the bound slot survives >2 s** (past `BIND_AGE` ~1 s windows) with crsn staying HIT; **FAIL = it ages out in <1 s** ⇒ wrong FE counter offset (re-probe `EN75_FE_TS_OFF`). Confirm `readl(fe_base+offset)` is monotonic and wraps at 15 bits.

**359 (auto offload):** *engine + steer + 358 on.*
1. Configure the kernel flowtable (nft `flowtable f { hook ingress … devices = { eth0 } ; flags offload }` + `flow add @f`), `ppe_offload=1`. `dmesg`: "offload ON".
2. **Internet→LAN download** (switch-0 host) via the speedtest rig: open a sustained transfer.
   - **PASS:** after the first few packets, `sirq` collapses (offload took over), throughput climbs toward the 160M→target ceiling, `TB_USED` grows with live flows, `conntrack` shows `[OFFLOAD]`. The 1-CPU MIPS softirq headroom freeing up is the headline signal.
   - **FAIL:** connection stalls on bind (egress) — fall back to 357's per-variant isolation.
3. **LAN→WAN (pppoe-wan):** the PPPoE-over-DSA egress path — start an upload. PASS = upload holds + `[OFFLOAD]`; this is the highest-risk encap, watch for one-directional stalls.
4. **Bidirectional speedtest** (the San Juan rig with a *known-good* server — per the throughput-diagnosis note, a bad server fakes bufferbloat). PASS = down+up both offloaded, ping stable, 0 loss.
5. `echo 0 > ppe_offload` → `en75_ppe_flush_all` drains binds, every flow returns to CPU **without dropping connections** (seamless A/B). Rollback: power cycle is always safe (default-off).

---

## 8. Top 5 risks + mitigations, and HW must-confirms

1. **Special-tag mis-delivery on the cascaded MT7530 (rank #1, Slice D).** Bound flow egressing a switch-1 port blackholes silently while crsn=HIT/TB_USED++. *Mitigation:* the PASSTHROUGH `BIT(7)` in `en75_foe_entry_set_dsa` + the 357 single-flow A/B/C oracle **before** any TC. Start on switch-0 + internet→LAN to isolate.
2. **FE aging-timestamp register unknown (`EN75_FE_TS_OFF`).** If wrong, every dynamic bind ages out in <1 s → flows thrash CPU↔bind, looks like "offload does nothing." *Mitigation:* patch 358 isolates it with a non-STATIC survival test; 357 uses STATIC so egress validation isn't contaminated by aging. **MUST confirm on HW:** the FE counter offset (mainline `0x0010 & GENMASK(14,0)`) is the *same* free-running counter the PPE scan-aging keys on.
3. **`spin_lock_bh` correctness under threaded NAPI.** RX runs in a kthread with BH disabled (`napi_dev->threaded=true`, `econet_qdma.c:1139`); the offload path is process ctx. Both take `ppe->lock` via `spin_lock_bh` — safe, and the hard-IRQ handler never touches `foe_flow`. *Mitigation:* keep `en75_foe_entry_clear` in a `__locked` variant so `__en75_ppe_check_skb` (lock held) doesn't re-enter; never call `en75_foe_entry_commit`'s `readl/writel` cache-clear with IRQs that could deadlock — it's MMIO, fine under `spin_lock_bh`.
4. **`NETIF_F_HW_TC` stripped by DSA conduit attach.** DSA can rewrite conduit features after our probe; if it clears it, the flowtable never offers blocks. *Mitigation:* set it on GDM1 only, and **verify `ethtool -k eth0`** post-attach; re-assert in `en75_dev_init` if needed. **MUST confirm on HW.**
5. **PPPoE-over-DSA egress encoding (LAN→WAN).** The `[special-tag][PPPoE][IP]` egress entry combines `set_dsa` + `set_pppoe` + possibly `set_vlan` — the most fragile l2/ib1 layering. *Mitigation:* validate WAN→LAN (no PPPoE) first; bring up LAN→WAN as a separate 359 sub-step with its own single-flow oracle; if it stalls, suspect `etype`/`vlan_layer` collision between `set_pppoe` and `set_dsa`.

**Additional HW must-confirms before trusting the design:**
- **`IB2.DEST_PORT` numeric for GDM1 == 1** (P4 test + RX `sport=1` agree, but readback a *bound* flow's ib2 once).
- **`SEARCH_MISS=FORWARD_BUILD` actually writes the parsed unbind entry** into the FoE slot (TB_CFG `3<<4` is set in `en75_ppe_init`) — the entire match-on-RX mechanism depends on the engine populating `hwe` with the parsed 5-tuple. Confirm by dumping a slot after steered miss traffic and seeing a non-zero UNBIND entry.
- **`MTK_FOE_IB1_STATIC` is honored** (357's STATIC test assumes it suppresses aging); if not, use a tight re-commit during first light.
- **The RX `get_erx_ppe_entry()` value is the engine's raw FoE hash slot** (not a pre-divided index) — `__en75_ppe_check_skb` uses it raw for `hwe` and `>>1` for the bucket. If the desc reports an already-divided value, both indexings shift. Confirm against a committed slot whose hash you computed in SW.

**Key file paths (absolute):** core/builders/test → `/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-2026.02.13~c2f855cf/econet_ppe.{c,h}`; new offload TU → `…/econet_ppe_offload.c`; RX hook → `…/econet_qdma.c` (lines 184-205); struct/plumbing → `…/econet_eth.{c,h}` (`en75_ppe_init` @395, `en75_remove` @276, `struct en75_eth` @162); ndo/features/steer → `…/econet_port.c` (`en75_netdev_ops` @456, `hw_features` @510, `ppe_steer` @151-175); `…/Kbuild` (line 2). P4 reference impl → `/home/cristuu/tools/xr500v/ppe-p4/econet_ppe.{c,h}`. Mainline → `/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/drivers/net/ethernet/mediatek/mtk_ppe{,_offload}.c`.