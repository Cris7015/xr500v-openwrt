/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _AIROHA_WHNAT_H
#define _AIROHA_WHNAT_H
#include <linux/netdevice.h>
struct econet_ppe;
struct econet_qdma;
struct econet_foe_entry;
struct napi_struct;
struct airoha_whnat_batch {
	struct sk_buff_head queue[16];
	struct net_device *dev[16];
};
void airoha_whnat_batch_init(struct airoha_whnat_batch *batch);
void airoha_whnat_batch_flush(struct airoha_whnat_batch *batch);
bool airoha_whnat_active(void);
int airoha_whnat_vif_index(struct net_device *dev);
void airoha_whnat_set_ppe(struct econet_ppe *ppe);
void airoha_whnat_clear_ppe(struct econet_ppe *ppe);
void airoha_whnat_set_qdma(struct econet_qdma *qdma);
void airoha_whnat_clear_qdma(struct econet_qdma *qdma);
int airoha_whnat_xmit(struct econet_qdma *qdma, struct sk_buff *skb);
bool airoha_whnat_rx(struct sk_buff *skb, struct napi_struct *napi,
		     u8 qdma_id, u8 sport, u8 reason, struct airoha_whnat_batch *batch);
void airoha_whnat_complete(void);
void airoha_whnat_invalidate_vif(struct econet_ppe *ppe, int idx);
void en75_whnat_register_vif(struct net_device *dev, int idx);
void en75_whnat_unregister_vif(int idx);
bool en75_whnat_upstream_early(struct sk_buff *skb);
#endif
