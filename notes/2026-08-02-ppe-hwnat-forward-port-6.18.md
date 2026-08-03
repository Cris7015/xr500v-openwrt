# PPE/HW-NAT forward-port to Linux 6.18 and persistent install

Date: 2026-08-02

## Outcome

The PPE hardware NAT/offload engine, developed against the Linux 6.12 tree,
now runs on the Linux 6.18 XR500v port. It was validated with real traffic on
a TP-Link Archer XR500v v1 and then installed persistently to NAND.

Headline numbers, both measured on the device:

| Path | Throughput | Router CPU |
| --- | ---: | ---: |
| LAN-to-LAN, NAT | 890 Mbit/s | 0.5-4 % |
| PPPoE WAN | 701 / 681 Mbit/s | not sampled |

For scale: software forwarding on this single-core 34Kc tops out around
150-200 Mbit/s, and the OEM firmware reaches roughly 718 Mbit/s over the same
PPPoE line. Reaching 945 Mbit/s through the conduit while the CPU stays
essentially idle is only possible with the engine doing the work.

## Why the port was cheap

Both trees pin the same out-of-tree driver revision:

```text
cjdelisle/econet_eth.git @ c2f855cf5bdcb2dfc536d865d21bc793a346ce44
```

So the 6.12 patch chain applies against identical sources. Of its 35 relevant
patches (350-395, excluding the gsw DSA-VLAN ones and the TX/BQL fixes this
tree already carries as `140-tx-bql-ownership`), **33 applied unchanged**.

The two that did not were trivial:

- `Kbuild` — a context shift, since `090-bundle-pcs-mtk-lynxi` inserts two
  lines above the object list. Only `econet_ppe.o` had to be added.
- one hunk of the WHNAT TX probe — obsolete here, because
  `140-tx-bql-ownership` already keeps probe skbs out of BQL by passing a
  `NULL` txq, which is the cleaner fix.

**No kernel API adjustments were needed at all.** `flow_block_cb_*`,
`flow_cls_offload` and `flow_offload_replace/destroy/stats` are unchanged
between 6.12 and 6.18.

The result is squashed into
`package/kernel/econet-eth/patches/200-ppe-hwnat-offload.patch`.

## Two traps worth recording

**Do not skip "probe" patches in a long chain.** `351-ppe-phase2a-rx-tag-dump`
looks disposable, but the block it adds is *context* for `355`, which creates
`econet_ppe.{c,h}`. Dropping it makes everything downstream fail.

**Dropping WHNAT creates more conflicts, not fewer.** `393` and `394` depend on
context introduced by the WHNAT patches, so porting the whole chain is easier
than porting a subset.

## Evidence that the engine is real

Register writes are read back and compared at init:

```text
XR500v PPE 1a: foe_phys=0x0dc00000 TB_BASE_rb=0x0dc00000
               TB_CFG_w=0x0001cfbc TB_CFG_rb=0x0001cfbc
               GLO_CFG_rb=0x0000060d TB_USED_rb=0x00000000
XR500v PPE: engine armed (flowtable BIND)
               GDM1_FWD=0x03f04004 FLOW_CFG=0x0600f700
```

nftables accepts the flowtable with hardware offload, which fails outright if
no device backs it:

```text
flowtable ft {
        hook ingress priority filter
        devices = { "lan1", "lan2", "lan3", "lan4" }
        flags offload
}
```

A table scan during the test showed real client flows, including entries in
state 2 (BIND) with the test client as source.

The `362`/`363` gsw DSA-VLAN patches were **not** ported and turned out not to
be required.

## Measuring offloaded traffic

Offloaded packets never reach the Linux netdev, so **interface counters stay
flat while the link is saturated**. During a 1.16 GB speedtest `pppoe-wan`
reported `RX=0 MB`; the DSA user ports behave the same way. This is a signature
of offload, not an absence of traffic.

Verify the path by other means: the public IP seen by the client matched the
`pppoe-wan` address exactly. The conduit (`eth0`) does still count, so an
automatic trigger should watch that.

## Persistent installation

Installed with `sysupgrade -n` from this release's own initramfs, which is what
`platform.sh` requires — it refuses to run when `openwrt_ubi` is absent, and
the previously flashed build did not have that partition.

The UBI overlay path, until now never exercised end to end, completed cleanly:

```text
ubiformat: mtd10, 64.0 MiB, 512 eraseblocks
ubi0: good PEBs: 512, bad PEBs: 0, corrupted PEBs: 0
UBIFS: mounted UBI device 0, volume 0, name "rootfs_data"  (58 MiB)
[xr500v-ubi] rootfs_data is ready for mount_root
mount_root: switching to ubifs overlay
en75_bmt: blocks: total 1024, user 930, factory_bad: 0, worn: 0
```

The partition table is not new: it matches the 6.12 port (`openwrt_ubi`, 64 MB
at `0x3000000`) and leaves slot A untouched, so `bflag set 0` remains a working
rollback.

Running afterwards: `r0+35642-b606756243`, kernel `6.18.39`, from NAND.

### Caveats

- `nohup` does not exist on the device; run `sysupgrade` directly.
- `sysupgrade -n` resets the whole configuration. All four ports return to the
  bridge — which re-bridges the WAN modem into the home LAN — and the default
  DHCP server on `br-lan` comes back. Audit both after every `-n`.

## Still open

- Wi-Fi offload (WHNAT) is included in the port but untested on 6.18.
- GPON and FXS remain separate and fail-closed.
