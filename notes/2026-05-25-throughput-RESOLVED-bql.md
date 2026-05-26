# TX throughput RESOLVED — it was BQL (2026-05-25)

**TL;DR:** Wired CPU TX was capped at **~5 Mbps** while RX did **245 Mbps**. Root cause = the
conduit's **BQL (Byte Queue Limits) `dql.limit` collapsing to ~86 bytes** (less than one packet),
because the QDMA signals TX completion **per packet** (done-queue `int_threshold = 1`). dql
auto-tunes its limit to ~1 packet → the stack does XOFF/XON on every packet → TX serializes to ~3
packets in flight → 5 Mbps.

**Fix:** patch `330-bql-min-limit.patch` — after `register_netdev()` in `econet_port.c`, set
`netdev_get_tx_queue(ndev, i)->dql.min_limit = 262144` (256 KiB) on every GDM-port TX queue (under
`#ifdef CONFIG_BQL`).

**Result:** TX **5 Mbps → 161 Mbps (32×)**, UDP 0% loss, persistent out of the box (no sysfs
tweak). Confirmed at `/sys/class/net/eth0/queues/tx-*/byte_queue_limits/limit_min = 262144`.

## How it was found
Per-packet QDMA register guessing (≈10 hypotheses) all failed because **the QDMA was never the
bottleneck**. The breakthrough was instrumenting the **in-flight ring depth** (atomic counter
inc'd in `en75_qdma_xmit`, dec'd in `en75_poll_tx_complete`): max ≈ 24, steady ≈ 3, **drains to 0**
→ the HW completes fast, the limiter is upstream of the ring (xmit rate). Since `en75_dev_xmit`
calls `netdev_tx_sent_queue` and the completion calls `netdev_tx_completed_queue`, BQL is active.
Live test (no reflash): bumping `limit_min` via sysfs jumped TX 5M→125M instantly → BQL confirmed.

`limit_min` sweep (live) to find the knee:

| limit_min | TCP UP |
|-----------|--------|
| 64 KiB    | 71 Mbps |
| **256 KiB** | **161 Mbps (peak)** |
| 1 MiB     | 157 Mbps |
| 4 MiB     | 147 Mbps |

256 KiB is the sweet spot (peak throughput, bounded bufferbloat; higher values slightly regress).

## Ruled out, with evidence — do NOT chase these again
- **WiFi RF** (866 VHT80), **CPU** (idle under load), **cable** (RX 245M same cable, 1000BASE-T symmetric).
- **cascade TRGMII RD_TAP**: ported `macMT7530doP6Cal` (patch 240) — eye is wide/clean (taps 1-45),
  centered taps, no change. Cascade is healthy. (Patch 240 stays; it's LAN infra.)
- **QDMA tail-drop**, **TX descriptor count** (128).
- **QDMA per-channel rate limiter**: `ch_lim_en` (base+0x160) read = 0 (no channel limited);
  `tx_meter_cfg` (0x98) global-enable set but no channels → not limiting. Clearing both = no effect.
  (The earlier "rate limiter root cause" note was **wrong** — superseded by this.)
- **tx_int_delay / rx_int_delay** = 0 (already).
- **WRR scheduler**: live dump `vch_qmode = 0x55555555` = all channels STRICT PRIORITY (not WRR),
  so weights are ignored; programming weights had no effect.
- **PSE buffer threshold** (`buf_usage_cfg` @ +0x90): read `before = 0x80100080` = already enabled
  with chn=16/total=128 (more generous than OEM's 6/24). Not buffer starvation.
- **Cache** (ring is `dmam_alloc_coherent`), **memory barrier** (`en75_wreg` uses `writel`, which
  already does `wmb()` on MIPS).

## Remaining ceiling (~161 Mbps, not a bug)
At 161 Mbps the CPU is still ~81% idle, so there is a separate, lesser limit (likely the cascade
TRGMII TX direction, DSA softirq forwarding, or single-stream). Not critical — the on-board WiFi
caps around ~150 Mbps anyway. Future optimization, not a defect.

EN751221 is the older EcoNet/TrendChip design, **not** Airoha EN7581 — mainline `airoha_eth.c`
register offsets do not apply directly.
