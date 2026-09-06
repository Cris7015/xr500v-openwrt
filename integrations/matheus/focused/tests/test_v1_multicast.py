#!/usr/bin/env python3
"""Compile the actual upstream V1 RX function with synthetic host stubs.

Usage: test_v1_multicast.py path/to/airoha_ppe.c
No network, device IO, or hardware performance claim.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

source=Path(sys.argv[1]).read_text()
match=re.search(r'static void airoha_ppe_v1_rx_check\([^;{}]+\)\n\{',source)
assert match
end=source.index('\n}',match.end())+2
function=source[match.start():end]
assert function.index('is_multicast_ether_addr') < function.index('airoha_foe_v1_parse_tuple') < function.index('spin_lock_bh')
assert 'is_valid_ether_addr(data.eth.h_dest)' in source
prefix=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16;
#define ETH_HLEN 14
#define READ_ONCE(v) (v)
#define EN751221_PPE_CPU_REASON_NO_FLOW 1
#define AIROHA_PPE_CPU_REASON_HIT_UNBIND 2
#define AIROHA_PPE_CPU_REASON_HIT_UNBIND_RATE_REACHED 3
#define AIROHA_FOE_V1_INVALID_HASH 0xffff
static int parsed, locked, unlocked;
struct sk_buff { unsigned char *data; unsigned int headlen; };
struct airoha_foe_v1_tuple { int unused; };
struct airoha_foe_entry { int value; };
struct airoha_flow_table_entry { u16 hash; struct airoha_foe_entry data; };
struct soc { unsigned int ppe_dram_entries; };
struct eth { struct soc *soc; };
struct airoha_ppe { struct { struct eth *eth; } common; struct { bool armed; int lock,flows; struct airoha_flow_table_entry *foe_owner[8]; } v1; };
#define skb_headlen(skb) ((skb)->headlen)
static bool is_multicast_ether_addr(const unsigned char *p) { return p[0]&1; }
static bool airoha_foe_v1_parse_tuple(struct sk_buff *skb,struct airoha_foe_v1_tuple *t) { parsed++;return skb->headlen>=ETH_HLEN; }
static void spin_lock_bh(int *p) { locked++; }
static void spin_unlock_bh(int *p) { unlocked++; }
#define list_for_each_entry(pos,head,member) for((void)(head),pos=0;pos;pos=0)
static bool airoha_foe_v1_flow_matches_tuple(struct airoha_flow_table_entry *f,struct airoha_foe_v1_tuple *t) {return false;}
static u16 airoha_ppe_v1_find_bind_way(struct airoha_ppe *p,struct airoha_flow_table_entry *f,u16 h,struct airoha_foe_entry *e,bool *v){return h;}
static void airoha_ppe_v1_release_flow_slot(struct airoha_ppe *p,struct airoha_flow_table_entry *f,bool b) {}
static void airoha_ppe_v1_commit_entry(struct airoha_ppe *p,struct airoha_foe_entry *e,u16 h,struct airoha_foe_entry *r) {}
'''
suffix=r'''
int main(void) {
 struct soc soc={8};struct eth eth={&soc};struct airoha_ppe ppe={.common.eth=&eth,.v1.armed=true};
 unsigned char packet[64]={0},before[64];struct sk_buff skb={packet,sizeof(packet)};
 for(int reason=1;reason<=3;reason++) {
  for(int dest=0;dest<256;dest++) {
   memset(packet,0xa5,sizeof(packet));packet[0]=dest;memcpy(before,packet,sizeof(packet));
   parsed=locked=unlocked=0;airoha_ppe_v1_rx_check(&ppe,&skb,0,reason);
   assert(parsed==!(dest&1));assert(locked==parsed && unlocked==locked);
   assert(!memcmp(before,packet,sizeof(packet)));
  }
 }
 for(unsigned int n=0;n<ETH_HLEN;n++) {
  parsed=locked=unlocked=0;skb.headlen=n;skb.data=NULL;
  airoha_ppe_v1_rx_check(&ppe,&skb,0,1);
  assert(parsed==1 && !locked && !unlocked);
 }
 skb.data=packet;skb.headlen=sizeof(packet);parsed=locked=unlocked=0;
 airoha_ppe_v1_rx_check(NULL,&skb,0,1);
 ppe.v1.armed=false;airoha_ppe_v1_rx_check(&ppe,&skb,0,1);ppe.v1.armed=true;
 airoha_ppe_v1_rx_check(&ppe,&skb,8,1);airoha_ppe_v1_rx_check(&ppe,&skb,0,255);
 assert(!parsed && !locked && !unlocked);
 puts("PASS: actual upstream RX function: multicast/broadcast bypass, all first-octet values, unicast path, short headers, inactive/hash/reason guards, unchanged packets");
}
'''
with tempfile.TemporaryDirectory(prefix='airoha-v1-mcast-test-') as temp:
    c=Path(temp)/'test.c';binary=Path(temp)/'test'
    c.write_text(prefix+function+suffix)
    subprocess.run(['gcc','-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
