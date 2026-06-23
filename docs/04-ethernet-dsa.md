# Ethernet & the DSA Switch

The Archer XR500v routes its four RJ45 LAN jacks through **two cascaded MT7530-class switches**, not one: an *on-die* switch in the EN751221 SoC (reached over MMIO at `0x1fb58000`) whose only active ports are the cascade link (port 5) and the CPU link (port 6 → the frame engine's `gmac0`), and an *external* MT7530 wired as a **Multi-Chip Module (MCM)** reached over MDIO at PHY address `0x1f`, which carries the four user gigabit PHYs.

The stock OEM firmware does **not** use DSA at all. It runs a monolithic `eth.ko` (plus `qdma_lan.ko` and `fe_core.ko`) that exposes `eth0` with `eth0.1..eth0.4` standard 802.1Q sub-interfaces, and a hardware `STAG → 802.1Q VTAG` conversion toggled via `/proc/tc3162/stag_to_vtag`. Reaching the same hardware through Linux DSA therefore required reverse-engineering both the tagger and the switch topology. Mainline DSA (single `mt7530-mmio` / `airoha,en7581-switch`) enumerated the ports but never passed RX traffic; the approach that works models the hardware as a **nested DSA tree** — the MCM switch declared as a child of the on-die switch's MDIO bus — driven by the out-of-tree `econet-eth` bundle (a fork of mainline `mt7530.c` with EN751221 support) plus its own DSA tagger `gsw/tag-mtk.c`. With the conduit (`eth0`) kept out of `br-lan`, all four LAN ports do bidirectional traffic with hardware switching, VLAN offload, and per-port isolation. TX throughput needed a separate BQL fix (5 Mbps → 161 Mbps as a CPU endpoint); LAN-to-LAN switching runs at line rate inside the silicon, and CPU-routed forwarding measured ~590 Mbps in a dedicated router test configuration. This page documents the approach that works alongside the dead ends, since the dead ends are part of the record.

## Hardware topology

```
   RJ45 LAN1..LAN4
        │  (one user GE PHY each, MDIO addr 1..4 on the MCM's MDIO bus)
        ▼
 ┌──────────────────────────┐
 │  External MT7530 (MCM)    │   reached at PHY addr 0x1f on the on-die
 │  ID_EN751221_EXT          │   switch's MDIO master bus
 │  user ports 1..4          │
 │  port6 ──► cascade        │
 └────────────┬─────────────┘
              │ TRGMII cascade (1 Gbps/Full, fixed-link)
              ▼
 ┌──────────────────────────┐
 │  On-die MT7530 (MMIO)     │   @ 0x1fb58000, ID_EN751221
 │  port5 ◄── cascade        │
 │  port6 ──► CPU (gmac0)    │
 └────────────┬─────────────┘
              │ TRGMII (1 Gbps/Full, fixed-link)
              ▼
   Frame engine / QDMA  ──►  CPU conduit netdev "eth0"
```

A frame from a LAN jack traverses: cable → MCM user port → MCM port6 → on-die port5 → on-die port6 → frame engine → `eth0` (the DSA conduit). The reverse path egresses the same way. The on-die switch's `mdio { switch@1f }` sub-node is what makes the MCM a *nested* DSA device: the on-die `mt7530-mmio` driver registers an MDIO master (`devm_of_mdiobus_register`), and the MCM `mt7530-mdio` driver probes as a child of it — so `econet_eth.c` itself never has to provide an MDIO master (it doesn't have one; see the dead-end section).

The GPON/optical WAN and the two FXS/RJ11 telephone ports are **not** part of this switch. Only the four RJ45 jacks pass through DSA. The GPON ONU is a separate block (see the GPON/WAN page); the FXS ports are the Le9642 SLIC over ZSI/PCM (see the VoIP page).

## Switch register / topology map

| Element | Compatible / ID | Address | DSA member | Active ports |
|---|---|---|---|---|
| Frame engine + QDMA | `econet,en751221-eth` | `0x1fb50000` len `0x8000` | (conduit `eth0`) | gmac0 = CPU port |
| On-die switch | `econet,en751221-switch` (ID_EN751221) | `0x1fb58000` len `0x8000` | `<0 0>` | port5 (cascade), port6 (CPU) |
| External MCM switch | `econet,en751221-switch` + `mediatek,mcm` (ID_EN751221_EXT) | PHY `0x1f` on the MDIO master | `<0 1>` | port1..4 (user), port6 (cascade) |
| User PHYs | `ethernet-phy-ieee802.3-c22` | MDIO addr 1, 2, 3, 4 | — | one per LAN jack |

The `ethernet@` region was deliberately narrowed to `0x8000` (not `0x10000`): `0x1fb58000` is the on-die switch's region and is owned by the switch driver, not the eth driver. Overlapping the two caused `-EBUSY`/`insufficient register space` probe failures. The matching change on the driver side removed `switch_regs[]` from the `econet_eth.c` register struct (the fork's "remove dumb switch" change).

## The nested-DSA solution (what actually works)

The working device tree (`target/linux/econet/dts/en751221.dtsi`) declares the dual-switch nest:

```dts
switch_ondie: switch@1fb58000 {
    compatible = "econet,en751221-switch";   /* ID_EN751221, MMIO */
    reg = <0x1fb58000 0x8000>;
    resets = <&scuclk EN751221_GSW_RST>;
    dsa,member = <0 0>;
    ports {
        ondie_port5: port@5 { reg=<5>; phy-mode="trgmii"; link=<&mcm_port6>;
                              fixed-link { speed=<1000>; full-duplex; }; };
        port@6 { reg=<6>; label="cpu"; ethernet=<&gmac0>; phy-mode="trgmii";
                 fixed-link { speed=<1000>; full-duplex; }; };
    };
    mdio {                                    /* MDIO master created by the on-die driver */
        switch0: switch@1f {
            compatible = "econet,en751221-switch";   /* ID_EN751221_EXT */
            reg = <0x1f>;
            mediatek,mcm;
            dsa,member = <0 1>;
            ports {
                switch0port1: port@1 { reg=<1>; phy-mode="gmii"; phy-handle=<&mcmphy1>; };
                /* ...ports 2,3,4 likewise; port@0 disabled (no jack) ... */
                mcm_port6: port@6 { reg=<6>; phy-mode="trgmii"; link=<&ondie_port5>;
                                    fixed-link { speed=<1000>; full-duplex; }; };
            };
            mdio { mcmphy1: ethernet-phy@1 { reg=<1>; }; /* ...2,3,4... */ };
        };
    };
};
```

The board file (`en751221_tplink_archer-xr500v.dts`) then enables/labels the user ports. Key enabling decisions:

1. **MCM user PHYs live at MDIO address 1–4, not 9–12.** Earlier work (and the openwrt-devel thread) suggested 9–12; those turned out to be the *on-die* switch's own PHYs (no jacks). The bootloader's `miir` confirmed the live cable on PHY addr 1 (`BMSR`/`0x796d` = link). Port 0 / PHY 0 has no RJ45 jack (possibly an internal GPON uplink) and is left `disabled`.
2. **`devm_reset_control_get_optional("mcm")`** for the MCM switch (patch `210-mcm-reset-optional.patch`). The external switch's reset register is the one "Benjamin found" on the openwrt-devel thread, but it is **not** exposed in the public SCU reset bindings — only `EN751221_GSW_RST` (=23) exists and that resets the *on-die* switch. Making the reset optional lets the MCM probe relying on the bootloader-initialised state plus `genphy_soft_reset()` (done in `mt7530_port_enable()` for `ID_EN751221_EXT`).
3. **Register the MCM's PHYs *before* `dsa_register_switch()`** (patch `220-mdio-bus-pre-dsa-register.patch`). The MCM's internal PHYs are registered by `mt7530_post_register()` via `devm_of_mdiobus_register()`, which originally ran *after* `dsa_register_switch()`. That meant DSA tried to connect each user port to a PHY device that did not yet exist → `failed to connect to PHY: -ENODEV` on every `lanN`. Moving `mt7530_post_register()` ahead of `dsa_register_switch()` fixes it.
4. **`eth0` (the DSA conduit) must NOT be a member of `br-lan`.** This is the single most operationally important rule. If `eth0` is bridged, the bridge's `rx_handler` intercepts incoming frames *before* the DSA tagger gets to demux them → zero per-port RX. With `eth0` left out (`br-lan` = `lan1..lan4` + WiFi), the tagger runs and frames reach the right `lanN`.

Verified state: `mt7530-mdio econet_gsw-0:1f lan2: Link is Up - 1Gbps/Full`, `cat /sys/class/net/eth0/dsa/tagging` = `mtk`, per-port demux confirmed (ping into LAN4 increments only the `lan4` switch MIB, zero leak to `lan3`), VLAN-aware bridge with 802.1Q offloaded to hardware (no "not offloaded" warning), and ~590 Mbps bidirectional CPU-routed forwarding with 0% loss in a dedicated router test configuration. At the time of writing the public `cjdelisle/econet_eth` code still lists DSA as a TODO in its README ("not currently usable" — TRGMII unstable, needs PHY driver), so this configuration is ahead of the upstream fork on that front.

## The DSA tagger: `gsw/tag-mtk.c` (not the kernel's `tag_mtk.c`)

There are **two** "mtk" DSA taggers in this build and it is easy to patch the wrong one:

| Tagger | Source | Packaging | Loaded? |
|---|---|---|---|
| Kernel mainline | `net/dsa/tag_mtk.c` | `kmod-dsa-mt7530` (=m) | **No** — module, not in squashfs, never loads. Uses `dsa_conduit_find_user(dev, 0, port)` (device hardcoded 0). |
| econet-eth (the real one) | `gsw/tag-mtk.c` | `mtk-tag.ko`, built `=y` into the bundle | **Yes** — appears as `mtk_tag` in `lsmod`. |

Patch `gsw/tag-mtk.c` (via `package/kernel/econet-eth/patches/`), never `net/dsa/tag_mtk.c`. The econet-eth tagger already carries Caleb's `mtk_conduit_find_user(dev, port)`, which is **device-agnostic**: it scans *all* switches in the DSA tree for `dp->index == port`, so the historic "conduit device 0 hardcoded" problem is already solved in the source.

Two behaviors of this tagger close the loop across the cascade:

- **RX demux.** The RX tag carries the *origin user port number* preserved through the whole cascade (MCM port → on-die port5 → on-die port6 → CPU). A debug `printk` showed `mtk_tag_rcv: hdr=0x0001 port=1` for a frame from the cable on MCM port 1 = `lan2`. `mtk_conduit_find_user` resolves that to the right user netdev.
- **TX cross-chip passthrough.** On egress the tagger sets `MTK_HDR_XMIT_PASSTHROUGH` (BIT7) when `dp->ds->index != 0` (i.e. the frame is destined for the *nested* MCM switch, not the top-level on-die switch). That tells the on-die switch to pass the tagged frame through the cascade to the MCM instead of consuming it, so it egresses on the physical jack.

The `econet-switch` init script (`S99econet-switch`) late-loads the modules to avoid an autoload-time `bus=NULL` race: `pcs-mtk-lynxi`, `dsa_core`, then `mtk-tag.ko`, `econet-gsw.ko`, `econet-gsw-mmio.ko`, `econet-gsw-mdio.ko`. Without `mtk-tag.ko`, `dsa_register_switch()` returns `-ENOPROTOOPT` and the switch never comes up (RX = 0).

## The RX-bug investigation and why mainline DSA failed

For ~155 iterations the symptom was the same: ports enumerated and link came up, but **no RX** on any `lanN`. The investigation went through several wrong models:

1. **Single MMIO switch (iter57 onward).** Early work modeled the device as one `mt7530-mmio` switch at `0x1fb58000` and spent 90+ iterations there. This was the wrong switch entirely — the on-die switch has no jacks; the user ports are on the *external* MCM. (Documented as the iter57 mistake.)
2. **STAG vs MTK tag (OEM recon — the key insight).** Stock-OEM analysis over the operator telnet service revealed the OEM does **not** use DSA at all. It loads three monolithic modules — `fe_core.ko` (frame engine core), `qdma_lan.ko` (LAN DMA engine) and `eth.ko` (216 KB switch+eth driver, symbols intact) — and runs a VLAN-on-`eth0` model: `eth0` plus `eth0.1..eth0.4` standard 802.1Q sub-interfaces, with a hardware `STAG → 802.1Q VTAG` conversion toggled by `echo 1 > /proc/tc3162/stag_to_vtag`. The OEM `eth.ko` contains `macEN7512STagInsert/Remove/Enable/Disable`. The authoritative stock port map is `/proc/tc3162/eth_portmap`, which reads switch port0 → LAN4, port1 → LAN3, port2 → LAN2 — i.e. the user PHYs 1–4 are the active ports and the physical labels are inverted at the board level (not by software). The working hypothesis became "the EN751221 special tag isn't byte-identical to MediaTek STAG, so mainline `tag_mtk` doesn't match." That was *partly* right about tagging being the issue, but the actual fix was structural (nested topology + the econet-eth tagger), not a new tag format. This OEM contrast is what makes the eventual DSA result notable: the stock firmware never exercised the switch as a DSA device, so the tagger had to be reverse-engineered rather than reused.
3. **PHY-MAC silicon RX-dead (the false floor).** With the MMIO model, the conclusion was that user-port RX was dead at the silicon level (`PMSR=0`, `RXU=0`). This was an artifact of attacking the wrong switch; it was *not* a silicon defect.

The actual root cause, once the nested dual-switch model was in place, localized cleanly: pinging the router from a PC made the **conduit `eth0` RX counter climb** (frames reached the CPU) while **`lanN` RX stayed pinned at 1** and `ip neigh` was empty (router never learned the PC's MAC). So frames arrived at the CPU but DSA wasn't demuxing them to the user port — a **tag-demux problem, not silicon**. Two fixes resolved it: the device-agnostic `mtk_conduit_find_user` in `gsw/tag-mtk.c` (already present in the source), and pulling `eth0` out of `br-lan` so the bridge stopped stealing frames before the tagger. After that, end-to-end LAN gave 0% packet loss, ~8 ms RTT, with real RX+TX over the physical jack.

**Why mainline DSA didn't work, in short:** mainline gives a *single*-switch `mt7530-mmio` / `airoha,en7581-switch` driver that needs an MDIO master in the eth driver to reach an external switch — and the public `econet_eth.c` fork (`c2f855cf`) does **not** implement that MDIO master (it appears to have been completed privately upstream but was never published). The nested-DSA model sidesteps this entirely: the *on-die* switch driver provides the MDIO master via `devm_of_mdiobus_register`, and the MCM hangs off it as a DSA child. No unpublished code required.

## The econet-eth out-of-tree driver bundle

The XR500v does not use the kernel's in-tree `NET_DSA_MT7530` (it's disabled to avoid symbol collisions and blacklisted in `modprobe.d/99-blacklist-mt7530-upstream.conf`). Instead it builds the `econet-eth` package (`package/kernel/econet-eth/`), a fork of `cjdelisle/econet_eth.git` pinned at `c2f855cf` (`PKG_SOURCE_DATE 2026-02-13`), which compiles five `.ko` files from one tree:

| Module | Role |
|---|---|
| `econet-eth.ko` | main eth driver (GDM/QDMA frame engine) |
| `econet-gsw.ko` | DSA switch core — the `mt7530.c` fork with EN751221 support |
| `econet-gsw-mmio.ko` | MMIO probe variant → drives the on-die `switch@1fb58000` |
| `econet-gsw-mdio.ko` | MDIO probe variant → drives the MCM `switch@0x1f` |
| `mtk-tag.ko` | the real DSA "MTK" tagger (`gsw/tag-mtk.c`) |

It also bundles `pcs-mtk-lynxi.ko`, `mtk-phy-lib.ko`, and `mtk-ge.ko` (the gigabit PHY driver the openwrt-devel thread said was "missing").

`kmod-econet-eth` is pulled in at the **target level** (the EcoNet target's default package set, via `AutoLoad`), not in any board's `DEVICE_PACKAGES` — the per-device `DEVICE_PACKAGES` lines in `image/en751221.mk` carry only WiFi/USB/extras (`kmod-usb3`, `kmod-mt7603`, `kmod-mt76x2`, etc.). The eth/DSA bundle is therefore present on every EcoNet device built from this tree by default.

### The earlier "MDIO emulation" approach (historical, superseded)

The *first* working DSA (iter48) used a different, now-superseded technique: a ~150-line patch to `econet_eth.c` that **emulated an MDIO bus** by translating MDIO read/write callbacks into direct MMIO accesses on the switch register window. The key insight there was that `MT753X_CTRL_PHY_ADDR(addr) = (addr + 1) & 0x1f`, so for `addr = 0x1f` it yields `0x00`; the `mt7530` driver issues MMD writes to phy_addr 0, which the emulation had to accept as a silent no-op. That iter48 build used `fixed-link` on all four user ports (it couldn't read the switch-internal PHYs through the emulated bus) and a separate `kmod-dsa-mt7530` package. That MDIO-emulation model is what the nested-DSA model replaced — the nested model gets a *real* MDIO master from the on-die switch driver and auto-detects the MCM's internal PHYs, so `fixed-link` is only needed on the two TRGMII cascade/CPU links.

## TRGMII cascade calibration

Patch `240-trgmii-cascade-cal.patch` ports the OEM `macMT7530doP6Cal` routine as a sysfs trigger (`en751221_trgmii_cal`). It sweeps the TX-tap DAC (`0x7a50..0x7a70`) and RX-tap DAC (`0x7a10..0x7a30`) registers in both cascade directions (SOC↔EXT), using a `0x55` test pattern plus an error-check toggle (BIT30 of the RX-tap reg) to find a clean eye window, then centers the tap. If no clean window is found it **keeps the existing tap** rather than zeroing it (safe default). In practice the cascade eye was already healthy (wide clean windows, taps 1–45) so this calibration is infrastructure rather than a required fix — but it stays in the build as cascade-link hygiene. (Note: it was investigated as a possible TX-throughput cause and *ruled out* — see below.)

## Port label inversion (physical LANn ≠ Linux lanN)

The plastic case labels and the Linux netdev names are **inverted**. Empirically mapped, cable by cable:

| Case jack | MCM port | PHY (MDIO addr) | Linux label |
|---|---|---|---|
| LAN1 | port4 | 4 | `lan1` |
| LAN2 | port3 | 3 | `lan2` |
| LAN3 | port2 | 2 | `lan3` |
| LAN4 | port1 | 1 | `lan4` |

The four jacks are on MCM **ports 1–4** (not 0–3), and the order is reversed: case `LANn` = MCM `port(5-n)`. This is encoded directly in `en751221_tplink_archer-xr500v.dts` (`switch0port1` → `lan4`, `switch0port2` → `lan3`, `switch0port3` → `lan2`, `switch0port4` → `lan1`; `switch0port0` disabled). Confirmed by physical test: LAN1→`lan1` and LAN4→`lan4`, 0% loss. This matches the stock OEM's own inverted `/proc/tc3162/eth_portmap`, so the inversion is a board-wiring property, not a software artifact.

## Throughput: the TX BQL bottleneck

LAN and forwarding worked, but the **CPU's own cabled TX topped out at ~5 Mbps** while RX did ~245 Mbps — a marked asymmetry. After ruling out a long list of suspects (below), the root cause was **BQL (Byte Queue Limits)**: the QDMA signals TX-done **per packet** (the done-queue `int_threshold = 1`), so the `dql` algorithm auto-tuned its limit down to ~86 bytes — *less than one packet*. The stack then did XOFF/XON per packet, serializing TX to ~3 packets in flight ≈ 5 Mbps. The "xmit→irq ~7 ms" latency and 2–11 ms ping jitter were not hardware latency — they were packets waiting on BQL budget. (The hardware was always fast; RX at 245 Mbps over the same cable confirmed it.)

**Fix** (`330-bql-min-limit.patch`, on `econet_port.c`): after `register_netdev`, set `dql.min_limit = 262144` (256 KB) on every TX queue of the GDM port, under `#ifdef CONFIG_BQL`:

```c
#ifdef CONFIG_BQL
    {
        unsigned int __i;
        for (__i = 0; __i < ndev->num_tx_queues; __i++)
            netdev_get_tx_queue(ndev, __i)->dql.min_limit = 262144;
    }
#endif
```

A live sweep found the knee: 64 KB → 71 Mbps, **256 KB → 161 Mbps (peak)**, 1 MB → 157 Mbps, 4 MB → 147 Mbps. Result: TX **5 → 161 Mbps**, UDP 0% loss, persistent from the flash image (no sysfs tweaking). The diagnostic that confirmed it was an atomic in-flight depth counter on the TX ring (max ~24, steady ~3, **drains to 0**), proving the hardware completes fast and the bottleneck was xmit rate; bumping `limit_min` via sysfs live then moved TX 5→125 Mbps immediately.

**Ruled out with evidence (do not re-chase):** WiFi RF, CPU (idle under load), cable, TRGMII `RD_TAP` (cal gave clean eye, no change), QDMA tail-drop, small ring, QDMA per-channel rate-limiter (`ch_lim_en`=0), `tx_int_delay`=0, WRR scheduler (`vch_qmode`=0x55555555 = all STRICT priority), and PSE buffer thresholds (already generous). The residual ~161 Mbps ceiling as an *endpoint* (CPU 81% idle at that rate) is single-stream/softirq-bound, not a bug.

### What 161 Mbps does and doesn't mean

161 Mbps is the **CPU-as-endpoint** number (a single-stream `iperf3` UDP run with the router as one end, CPU-bound). It is *not* the LAN switching number. Hardware switching between LAN ports (and LAN↔cascade) runs at line rate inside the MT7530 silicon with the CPU idle — to *measure* it you must put a client behind one LAN jack and traffic another, since the per-port hardware MIB (`ethtool -S lanN`, the **CamelCase** `RxBytes`/`TxBytes` counters, not the lowercase software ones) only moves for HW-switched traffic.

Three distinct numbers are easy to conflate, so to be precise about what is verified:

- **HW-switched line rate** — LAN↔LAN / LAN↔cascade inside the silicon, CPU idle; not directly measured (requires a client behind a second LAN jack) but bounded by the gigabit PHYs.
- **CPU-as-endpoint, validated** — a bidirectional `iperf3` run with the device as one TCP endpoint over a single LAN jack measured an **asymmetric ~TX 153 / RX 76 Mbps with the CPU saturated** (the RX software path is the more expensive direction). This is the figure that has actually been validated end-to-end.
- **CPU-routed forwarding** — ~590 Mbps bidirectional, measured in a **dedicated router test configuration** (a LAN-side subnet, a WAN-side interface and NAT, with a host running `iperf3` behind a LAN jack). This is a separate test setup, not the device's normal role.

In the configuration under which it normally runs, the device is an **L2 bridge node** (no firewall, management IP on `br-lan`) and is *not* in any internet forwarding path, so the ~590 Mbps forwarding figure should be read as a capability of the data path under test, not a number observed during day-to-day use.

## PPE / HNAT hardware NAT offload (working)

The device offloads forwarded/NATed traffic to the MediaTek **PPE/HNAT** hardware flow engine — the same one the OEM uses, which the public fork left as a TODO. The EN751221 PPE is **mt7622-class**: `offload_version = 2`, FoE entry **V1 / 80 bytes**, 16384 entries, register block at `BFB50C00` (accounting at `BFB52000`); the mainline `mtk_ppe_regs.h` offsets match the silicon as-is. As of **2026-06-21** the full path is **functional and auto-arms at boot** — marked *experimental* (committed, not yet long-soak-tested; fall back to `flow_offloading_hw=0` if you hit issues).

**Measured (HW offload, CPU idle):**

- **LAN ↔ LAN** (user port): **~929 Mbit/s, wire-speed**; CPU ~95 % idle, ~99 % of traffic on the HW path.
- **WAN → LAN** PPPoE download (over the ethernet-WAN GPON bypass): **~678 Mbit/s** (Movistar line rate), RX-CPU flat.
- LAN → WAN upload is offloaded the same way.

**How it was solved (patches, all committed):**

- **PPE init + auto-arm** (patch 378 core + 381): allocate the 16384×80 B FoE table, program `TB_BASE`/`TB_CFG`/aging/flow_cfg, and **arm the engine inside the flowtable `FLOW_BLOCK_BIND`** so it comes up by itself at boot — no manual poke. `TB_BASE`/`TB_CFG` read back as written → the offsets are correct.
- **Root cause** (patch 393, commit `dc92ba6`): `en75_flow_offload_replace` was **not encoding the egress DSA port**, so `l2.etype` carried no port bitmap, the MT7530 fell back to ARL-by-MAC, and only one port resolved while the rest black-holed — the long-hunted "only p2 works" bug. The fix derives the egress port from `FLOW_ACTION_REDIRECT.dev` (`dsa_port_from_netdev`); the FoE register then shows `etype=0x88 odev=lan2`. The **same egress-port bug** was what blocked the WAN→LAN download, so patch 393 fixed both at once.
- **Symmetric teardown** (patch 394, commit `5c9ef77`): repeatedly toggling `flow_offloading` (none/sw/hw) under load used to hang the router — the BIND armed the engine but the UNBIND never disarmed it. The fix de-steers GDM1 back to the CPU and flushes the FoE on UNBIND.

**Operational notes.** The reliable oracle for "is it offloading" is a **flat RX-CPU under load** (plus the throughput), *not* the `RXHWF` counter — which can read 0 while HW is forwarding. `econet` loads from `modules-boot.d`/squashfs *before* the overlay, so the offload `.ko` must be **flashed into `rootfs1`**; an overlay deploy does not take effect. A separate Wi-Fi half-offload (**WHNAT**) NATs in HW and lets the CPU re-inject to the radio (forwarded UDP ~514 Mbit/s, OEM-class). Full recipe + gotchas live in the fork's `tools/xr500v/whnat/MURAL-HWNAT.md`.

## Cross-references

- **GPON / optical WAN** — the WAN is not on this switch (separate ONU/xPON block).
- **VoIP / FXS (Le9642 SLIC)** — the two RJ11 phone ports, separate from the four DSA LAN ports.
- **WiFi (MT7603 / MT7662)** — `phy0-ap0` / `phy1` are bridged into `br-lan` alongside `lan1..lan4`.
- **Boot / flash / partitions** — slot B (OpenWrt) vs slot A (stock), bootflag, and the UBI overlay used for persistent config.
- **Build / repo** — `cjdelisle/openwrt` base pinned at `f3605b31fb`; always `make package/kernel/econet-eth/clean` before a fresh build, and never leave backup dirs under `package/` (OpenWrt compiles them in parallel and the stale `.ko` can win in the rootfs).

## Build / config notes

- The DSA bridge is rebuilt correctly only on a **clean boot** — `/etc/init.d/network restart` reuses the existing `br-lan` and does *not* re-add the DSA ports or reset `vlan_filtering`. Tweak the network in-band only with a safety-net restore timer, or you risk lockout.
- `/etc/config/network`: `br-lan` = `lan1 lan2 lan3 lan4` (+ WiFi), **no `eth0`**, no static bridge-vlan; management IP on `br-lan`. The board default (`board.d/02_network`) sets `lan = lan1 lan2 lan3 lan4` and a `wan` interface (DHCP).
- Modules `=y` go to the squashfs (`/rom`); modules `=m` go to the persistent UBI overlay and are **not** refreshed by a reflash. Always verify the recompiled `.ko` is actually inside the squashfs before flashing (incremental builds sometimes leave a stale `.ko` out of the image).