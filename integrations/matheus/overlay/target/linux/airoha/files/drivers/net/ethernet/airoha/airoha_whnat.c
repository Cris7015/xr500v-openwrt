// SPDX-License-Identifier: GPL-2.0-only
/* EN751221 WHNAT port: optional mt76 pre-GRO -> LAN QDMA/PPE and FCPU return.
 * Based on the XR500v 2026-08-09 path. No per-packet local-address classifier,
 * no channel-7 rate limiter, no optical/MMIO setup here. Disabled by default.
 */
#include <linux/module.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/rcupdate.h>
#include <linux/mutex.h>
#include <linux/unaligned.h>
#include "airoha_eth.h"
#include "airoha_whnat.h"

#define WHNAT_VIFS 16
#define WHNAT_FCPU_REASON 0x16
static struct net_device __rcu *vifs[WHNAT_VIFS];
static struct econet_ppe __rcu *active_ppe;
static struct econet_qdma __rcu *active_qdma;
static DEFINE_MUTEX(vif_lock);
static bool whnat_enable;
/* First hardware test is bounded; UINT_MAX enables the tested-style workload
 * only after ownership/completion/return counters have been checked. */
static unsigned int whnat_max_packets = 32;
static atomic_t handed = ATOMIC_INIT(0), completed = ATOMIC_INIT(0);
static atomic_t returned = ATOMIC_INIT(0), downstream = ATOMIC_INIT(0);
static atomic_t fallback = ATOMIC_INIT(0);
module_param(whnat_enable, bool, 0644);
module_param(whnat_max_packets, uint, 0644);
static int counter_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", (unsigned int)atomic_read(kp->arg));
}
static const struct kernel_param_ops counter_ops = { .get = counter_get };
module_param_cb(whnat_handed, &counter_ops, &handed, 0444);
module_param_cb(whnat_completed, &counter_ops, &completed, 0444);
module_param_cb(whnat_returned, &counter_ops, &returned, 0444);
module_param_cb(whnat_downstream, &counter_ops, &downstream, 0444);
module_param_cb(whnat_fallback, &counter_ops, &fallback, 0444);

bool airoha_whnat_active(void)
{
	return READ_ONCE(whnat_enable) && rcu_access_pointer(active_ppe);
}
void airoha_whnat_complete(void) { atomic_inc(&completed); }

void airoha_whnat_set_ppe(struct econet_ppe *ppe)
{
	rcu_assign_pointer(active_ppe, ppe);
}
void airoha_whnat_clear_ppe(struct econet_ppe *ppe)
{
	int i;
	if (rcu_access_pointer(active_ppe) != ppe) return;
	RCU_INIT_POINTER(active_ppe, NULL);
	synchronize_net();
	for (i = 0; i < WHNAT_VIFS; i++) en75_whnat_unregister_vif(i);
}
void airoha_whnat_set_qdma(struct econet_qdma *qdma)
{
	rcu_assign_pointer(active_qdma, qdma);
}
void airoha_whnat_clear_qdma(struct econet_qdma *qdma)
{
	if (rcu_access_pointer(active_qdma) != qdma) return;
	RCU_INIT_POINTER(active_qdma, NULL);
	synchronize_net();
}

void en75_whnat_register_vif(struct net_device *dev, int idx)
{
	struct net_device *old;
	if (!dev || idx < 0 || idx >= WHNAT_VIFS || !rcu_access_pointer(active_ppe)) return;
	mutex_lock(&vif_lock);
	old = rcu_dereference_protected(vifs[idx], lockdep_is_held(&vif_lock));
	dev_hold(dev);
	rcu_assign_pointer(vifs[idx], dev);
	synchronize_net();
	if (old) dev_put(old);
	mutex_unlock(&vif_lock);
}
EXPORT_SYMBOL_GPL(en75_whnat_register_vif);

void en75_whnat_unregister_vif(int idx)
{
	struct net_device *old;
	struct econet_ppe *ppe;
	if (idx < 0 || idx >= WHNAT_VIFS) return;
	mutex_lock(&vif_lock);
	old = rcu_dereference_protected(vifs[idx], lockdep_is_held(&vif_lock));
	RCU_INIT_POINTER(vifs[idx], NULL);
	synchronize_net();
	rcu_read_lock_bh();
	ppe = rcu_dereference_bh(active_ppe);
	if (ppe) airoha_whnat_invalidate_vif(ppe, idx);
	rcu_read_unlock_bh();
	if (old) dev_put(old);
	mutex_unlock(&vif_lock);
}
EXPORT_SYMBOL_GPL(en75_whnat_unregister_vif);

int airoha_whnat_vif_index(struct net_device *dev)
{
	int i, result = -1;
	if (!dev) return -1;
	rcu_read_lock();
	for (i = 0; i < WHNAT_VIFS; i++)
		if (rcu_dereference(vifs[i]) == dev) { result = i; break; }
	rcu_read_unlock();
	return result;
}

bool en75_whnat_upstream_early(struct sk_buff *skb)
{
	struct econet_ppe *ppe;
	struct econet_qdma *qdma;
	struct net_device *original;
	int idx, ret;
	bool taken = false;
	if (!READ_ONCE(whnat_enable) || !skb || !skb->dev ||
	    skb->protocol != htons(ETH_P_IP) || skb_is_gso(skb) || skb_vlan_tag_present(skb))
		return false;
	idx = airoha_whnat_vif_index(skb->dev);
	if (idx < 0 || !skb_mac_header_was_set(skb) ||
	    skb->data - skb_mac_header(skb) != ETH_HLEN)
		return false;
	rcu_read_lock_bh();
	ppe = rcu_dereference_bh(active_ppe);
	qdma = rcu_dereference_bh(active_qdma);
	if (!ppe || !qdma || !READ_ONCE(ppe->armed) ||
	    (unsigned int)atomic_read(&handed) >= READ_ONCE(whnat_max_packets))
		goto out;
	if ((skb_is_nonlinear(skb) && skb_linearize(skb)) ||
	    skb_cow_head(skb, ETH_HLEN + VLAN_HLEN))
		goto out;
	original = skb->dev;
	skb_push(skb, ETH_HLEN);
	skb_push(skb, VLAN_HLEN);
	memmove(skb->data, skb->data + VLAN_HLEN, 2 * ETH_ALEN);
	put_unaligned_be16(ETH_P_8021Q, skb->data + 12);
	put_unaligned_be16(idx, skb->data + 14);
	/* No BQL/netdev ownership for CPU-to-PPE packets. */
	skb->dev = NULL;
	ret = airoha_whnat_xmit(qdma, skb);
	if (ret >= 0) {
		atomic_inc(&handed);
		taken = true;
	} else {
		memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
		skb_pull(skb, VLAN_HLEN + ETH_HLEN);
		skb->dev = original;
	}
out:
	if (!taken) atomic_inc(&fallback);
	rcu_read_unlock_bh();
	return taken;
}
EXPORT_SYMBOL_GPL(en75_whnat_upstream_early);

void airoha_whnat_batch_init(struct airoha_whnat_batch *batch)
{
	int i;
	for (i = 0; i < WHNAT_VIFS; i++) {
		__skb_queue_head_init(&batch->queue[i]);
		batch->dev[i] = NULL;
	}
}

void airoha_whnat_batch_flush(struct airoha_whnat_batch *batch)
{
	int i;
	for (i = 0; i < WHNAT_VIFS; i++) {
		struct net_device *dev = batch->dev[i];
		struct sk_buff_head *q = &batch->queue[i];
		struct sk_buff *skb, *head = NULL, *tail = NULL;
		if (!dev) continue;
		/* Same bounded noqueue/no-packet-tap batching as the known-good
		 * driver; otherwise retain ordinary per-skb qdisc processing. */
		if (q->qlen > 1 && (dev->priv_flags & IFF_NO_QUEUE) && !dev_nit_active(dev)) {
			while ((skb = __skb_dequeue(q))) {
				if (!head) head = skb; else tail->next = skb;
				tail = skb;
			}
			tail->next = NULL;
			dev_queue_xmit(head);
		} else {
			while ((skb = __skb_dequeue(q))) dev_queue_xmit(skb);
		}
		dev_put(dev);
		batch->dev[i] = NULL;
	}
}

bool airoha_whnat_rx(struct sk_buff *skb, struct napi_struct *napi,
		     u8 qdma_id, u8 sport, u8 reason, struct airoha_whnat_batch *batch)
{
	struct net_device *dev;
	u16 proto, idx;
	bool bound = reason == WHNAT_FCPU_REASON;
	if (!rcu_access_pointer(active_ppe)) return false;
	if (skb_headlen(skb) < ETH_HLEN + VLAN_HLEN) return false;
	proto = get_unaligned_be16(skb->data + 12);
	if (bound) {
		if (proto != ETH_P_8021Q && proto != 0x5678) return false;
	} else if (qdma_id != 0 || sport != 0 || proto != ETH_P_8021Q) {
		return false;
	}
	idx = get_unaligned_be16(skb->data + 14) & VLAN_VID_MASK;
	if (idx >= WHNAT_VIFS) return false;
	rcu_read_lock_bh();
	dev = batch && batch->dev[idx] ? batch->dev[idx] : rcu_dereference_bh(vifs[idx]);
	if (!dev || !netif_running(dev)) {
		rcu_read_unlock_bh();
		return false;
	}
	memmove(skb->data + VLAN_HLEN, skb->data, 2 * ETH_ALEN);
	skb_pull(skb, VLAN_HLEN);
	skb->dev = dev;
	if (bound) {
		skb_reset_mac_header(skb);
		skb_set_network_header(skb, ETH_HLEN);
		skb->protocol = eth_hdr(skb)->h_proto;
		atomic_inc(&downstream);
		if (batch) {
			if (!batch->dev[idx]) { dev_hold(dev); batch->dev[idx] = dev; }
			__skb_queue_tail(&batch->queue[idx], skb);
		} else {
			dev_queue_xmit(skb);
		}
	} else {
		skb->protocol = eth_type_trans(skb, dev);
		atomic_inc(&returned);
		/* Resume below mt76's pre-GRO hook: never inject this return again. */
		napi_gro_receive(napi, skb);
	}
	rcu_read_unlock_bh();
	return true;
}
