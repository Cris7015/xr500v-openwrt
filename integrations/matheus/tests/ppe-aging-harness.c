/* Freestanding regression harness for the actual extracted kernel functions.
 * Run as both i386 (target-width unsigned long) and x86_64. No MMIO writes,
 * networking, libc or router access. 100% stubbed device state.
 */
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
#define HZ 1000UL
#define ENODEV 19
#define ENOENT 2
#define EN751221_PPE_INVALID_HASH 65535
#define AIROHA_FOE_STATE_INVALID 0
#define AIROHA_FOE_STATE_UNBIND 1
#define AIROHA_FOE_STATE_BIND 2
#define AIROHA_FOE_STATE_FIN 3
#define AIROHA_FOE_IB1_BIND_STATE 0x30000000U
#define AIROHA_FOE_IB1_BIND_TIMESTAMP 0x7fffU
#define REG_FE_FOE_TS 0x10
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctz(mask))
#define READ_ONCE(value) (value)
#define time_after(a, b) ((long)((b) - (a)) < 0)
#define FLOW_ACTION_HW_STATS_DELAYED 1
#define ETH_HLEN 14

struct soc { u32 ppe_dram_entries; };
struct eth { struct soc *soc; };
struct econet_flow_entry { unsigned long cookie, lastused; u16 hash; };
struct econet_ppe {
    int armed, lock;
    struct { struct eth *eth; } common;
    struct econet_flow_entry *foe_owner[4], *test_flow;
    u32 slots[4], rx_multicast_skip;
};
struct net_device { struct econet_ppe *ppe; };
struct flow_stats { unsigned long lastused; };
struct flow_cls_offload { unsigned long cookie; struct flow_stats stats; };
struct sk_buff { u8 *data; u32 headlen; };
static unsigned long jiffies;
static u32 hw_now, mmio_reads, dram_reads, scans;
static int locks, bad_lock;
static void lockdep_check(void) { if (locks != 1) bad_lock = 1; }
#define lockdep_assert_held(lock) lockdep_check()
#define spin_lock_bh(lock) (++locks)
#define spin_unlock_bh(lock) (--locks)
#define dma_rmb() ((void)0)
static u32 *econet_ppe_slot(struct econet_ppe *ppe, u16 hash)
{ ++dram_reads; return &ppe->slots[hash]; }
static u32 airoha_fe_rr(struct eth *eth, u32 reg)
{ ++mmio_reads; return hw_now; }
static struct econet_ppe *econet_ppe_from_netdev(struct net_device *dev)
{ return dev->ppe; }
static struct econet_flow_entry *econet_ppe_find_flow(struct econet_ppe *ppe, unsigned long cookie)
{ return ppe->test_flow && ppe->test_flow->cookie == cookie ? ppe->test_flow : (void *)0; }
static void flow_stats_update(struct flow_stats *stats, int bytes, int packets,
                             int drops, unsigned long lastused, int mode)
{ stats->lastused = lastused; }
static u32 skb_headlen(struct sk_buff *skb) { return skb->headlen; }
static int is_multicast_ether_addr(const u8 *data) { return data[0] & 1; }

#include "ppe-aging-extracted.inc"

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)
static int run_tests(void)
{
    struct soc soc = { 4 };
    struct eth eth = { &soc };
    struct econet_flow_entry flow = { 123, 50000, EN751221_PPE_INVALID_HASH };
    struct econet_flow_entry other = { 456, 0, 0 };
    struct econet_ppe ppe = { .armed = 1, .common.eth = &eth, .test_flow = &flow };
    struct net_device dev = { &ppe };
    struct flow_cls_offload cls = { .cookie = 123 };
    u8 packet[16] = { 0x01, 0, 0x5e };
    struct sk_buff skb = { packet, 14 };
    unsigned long ts;
    u32 i;

    /* Multicast/broadcast: return before list scan; do not modify skb. */
    test_multicast_gate(&ppe, &skb);
    CHECK(ppe.rx_multicast_skip == 1 && scans == 0 && packet[0] == 1);
    packet[0] = 0xff; test_multicast_gate(&ppe, &skb);
    CHECK(ppe.rx_multicast_skip == 2 && scans == 0);
    packet[0] = 0x5c; test_multicast_gate(&ppe, &skb);
    CHECK(scans == 1 && ppe.rx_multicast_skip == 2);
    /* Short header must not dereference even an invalid data pointer. */
    skb.data = (void *)0; skb.headlen = 13; test_multicast_gate(&ppe, &skb);
    CHECK(scans == 2 && ppe.rx_multicast_skip == 2);

    jiffies = 100000;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 50000);
    jiffies = 200000;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 50000);
    CHECK(!dram_reads && !mmio_reads); /* unbound query cannot invent activity */
    dev.ppe = (void *)0; CHECK(econet_flow_offload_stats(&dev, &cls) == -ENODEV);
    dev.ppe = &ppe; cls.cookie = 999;
    CHECK(econet_flow_offload_stats(&dev, &cls) == -ENOENT && locks == 0);
    cls.cookie = 123; flow.hash = 0; ppe.foe_owner[0] = &other;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && !dram_reads);
    ppe.foe_owner[0] = &flow; flow.hash = 4;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && !dram_reads);
    flow.hash = 0; ppe.armed = 0;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && !dram_reads);
    ppe.armed = 1;

    /* Hardware-only traffic refreshes lastused; repeated queries do not. */
    jiffies = 100000; hw_now = 100; ppe.slots[0] = 0x20000000 | 95;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 95000);
    jiffies = 120000; hw_now = 120;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 95000);
    jiffies = 140000; hw_now = 140;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 95000);
    CHECK(cls.stats.lastused + 30000 < jiffies); /* flow can really expire */
    ppe.slots[0] = 0x20000000 | 139;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 139000);
    flow.lastused = 139500;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 139500);
    ppe.slots[0] = 0x30000000 | 140;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 140000);
    ppe.slots[0] = 0x10000000 | 150; jiffies = 150000; hw_now = 150;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 140000);
    ppe.slots[0] = 0;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 140000);

    /* Hardware clock wraps at 32768 seconds, not 65536; KA bit is ignored. */
    jiffies = 100000; hw_now = 0xabcd8002; flow.lastused = 50000;
    ppe.slots[0] = 0x20008000 | 32766;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 96000);
    CHECK(ppe.slots[0] == (0x20008000 | 32766));

    /* A long fully accelerated transfer survives without any software RX.
     * Once hardware traffic stops, further stats queries stop refreshing it.
     */
    flow.lastused = 50000;
    for (i = 100; i <= 700; i += 5) {
        jiffies = i * HZ; hw_now = i; ppe.slots[0] = 0x20000000 | i;
        CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == jiffies);
    }
    for (i = 705; i <= 800; i += 5) {
        jiffies = i * HZ; hw_now = i;
        CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 700 * HZ);
    }
    CHECK(cls.stats.lastused + 30000 < jiffies);

    /* Exercise every representable hardware idle interval across rollover. */
    for (i = 0; i <= AIROHA_FOE_IB1_BIND_TIMESTAMP; ++i) {
        flow.lastused = 1; jiffies = 40000000;
        hw_now = (32766 + i) & AIROHA_FOE_IB1_BIND_TIMESTAMP;
        ppe.slots[0] = 0x20000000 | 32766;
        CHECK(econet_flow_offload_stats(&dev, &cls) == 0);
        CHECK(cls.stats.lastused == jiffies - i * HZ);
    }

    /* Native unsigned-long wrap: real 32-bit executable, also tested64. */
    jiffies = 2000; hw_now = 10; flow.lastused = ~0UL - 2000;
    ppe.slots[0] = 0x20000000 | 9;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == 1000);
    jiffies = ~0UL - 1000; flow.lastused = ~0UL - 2000;
    hw_now = 12; ppe.slots[0] = 0x20000000 | 10; ts = flow.lastused;
    CHECK(econet_flow_offload_stats(&dev, &cls) == 0 && cls.stats.lastused == ts);
    CHECK(locks == 0 && !bad_lock);
    return 0;
}

__attribute__((force_align_arg_pointer, noreturn)) void _start(void)
{
    int status = run_tests();
#ifdef __i386__
    __asm__ volatile("int $0x80" : : "a"(1), "b"(status) : "memory");
#else
    __asm__ volatile("syscall" : : "a"(60), "D"(status) : "rcx", "r11", "memory");
#endif
    __builtin_unreachable();
}
