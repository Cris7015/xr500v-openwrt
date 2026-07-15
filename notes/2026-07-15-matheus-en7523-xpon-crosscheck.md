# Matheus EN7523/EN7571 xPON cross-check and Linux 6.12 PLOAM checkpoint

Date: 2026-07-15

Status: **public upstream inspected; hardware-independent PLOAM core ported,
cross-compiled and runtime-verified on XR500v Linux 6.12; no PON hardware
access occurred**

## Sources and privacy boundary

Matheus Sampaio Queiroga supplied an Askey RTF8225VW boot log and pointed to
his public EN7523 kernel tree:

- branch: <https://sirherobrine23.com.br/airoha_en7523/kernel/src/branch/airoha_en7523_eth>
- mail-time commit: `c1bfa8424c6ec91e2501955ae76dad33d6abb968`
- inspected current commit: `4ed3e5fbc9b31d3b834615b9793b5ed643116e9b`

The supplied log is retained only on the local workstation because its early
boot section contains device identity and GPON credentials.  No serial,
password, MAC address, per-unit calibration payload or other identity value
is copied here.

## What the Askey run proves

The EN7529DT/EN7571 system gets materially beyond compile-only bring-up:

- the digital xPON PHY at `0x1faf0000` reports ready with no LOS;
- the EN7571 is identified at I2C address `0x70` and attached through a
  virtual SFP/DDMI model;
- the GPON MAC at `0x1fb60000` creates `pon0` and `omci0`;
- its FE/MBI path is released, IRQ 48 is enabled and optical link is reported;
- repeatable downstream PLOAM FIFO records arrive for more than two minutes.

The run remains in O2, but the attached file was captured at Unix timestamp
`1784071559` (2026-07-14 20:25:59 -03), five minutes after `c1bfa842` was
committed.  That revision applied `be32_to_cpu()` after `readl()` and logged
the first word as `0x002001ff`.  Reversing that conversion reconstructs the
register value `0xff012000`, whose fields are:

```text
ONU-ID       0xff  broadcast
message type 0x01  Upstream_Overhead
```

The same check changes the logged `0x386014ff` into `0xff146038`, a broadcast
`Extended_Burst_Length` (`0x14`).  Both are valid GPON downstream messages;
the logged post-conversion forms are not.  Current commit `4ed3e5fb` removes
the conversion on FIFO read/write, which is consistent with the evidence.
A fresh live log is still required before claiming that O2 now advances.

## Transferable and non-transferable pieces

Directly reusable:

- `airoha_ploam.c/.h`: hardware-independent O1-O7 state machine, downstream
  parsing, upstream construction, ranging/EqD, T-CONT/GEM/OMCI callbacks;
- the exact EN7521 interrupt layout, independently matched against the
  XR500v OEM `gpon_mac_reg_c_header_en7521.h`;
- GPON FIFO, activation register and timer structure as a hardware-adapter
  reference;
- the separation between optical frontend, digital PHY, PON MAC and packet
  datapath.

Not directly reusable:

- EN7523/EN7571 PHY writes beyond the XR500v's proven low-offset xPON window;
- EN7571 analogue defaults, calibration or APD/laser policy;
- the dedicated EN7523 MAC IRQ: the EN751221 OEM path demultiplexes GPON
  events through the shared WAN-QDMA callback;
- `airoha_eth_set_xpon_mode()` / `airoha_eth_set_xpon_datapath()`, because
  the XR500v uses the older out-of-tree `econet-eth` FE/QDMA driver;
- the current GPON/OMCI netdev data path, whose TX callbacks still consume
  and free packets without submitting a QDMA descriptor.

The common digital evidence remains strong: both systems expose the PHY at
`0x1faf0000` and retain `XPON_SETTING=0x14f`.  The Askey reports
`PHYSET3=0x4581e110`; the XR500v cold passive value is `0x4581e114`, and the
already tested XR-only ESD step clears exactly bit 2 to reach `...110`.

## Linux 6.12 integration checkpoint

Only the hardware-independent PLOAM layer was imported into the isolated
worktree branch `wip/matheus-xpon-6.12` as:

```text
package/kernel/xr500v-gpon-ploam-lab/
```

A software-only harness normalizes explicit per-SoC FIFO word order and
drives synthetic messages through:

```text
O1 -> O2 -> O3 -> O4 -> O5 -> O6
```

The O2-to-O3 input is the reconstructed broadcast `Upstream_Overhead` vector
from the Askey log.  Assign-ONU-ID and Ranging-Time are synthetic and contain
no operational credential.  The package has no autoload, DT match, MMIO,
IRQ, I2C, GPIO, PHY, laser, APD, netdev or packet path.

Cross-checking the imported deduplication filter against the XR500v OEM
`should_ignore_ploam_msg()` exposed one real boundary error in the newer
draft: for `Ranging_Time`, the OEM compares seven protocol bytes
(`ONU-ID`, type and `content[0..4]`), while the draft mask compared only six.
The local core now uses `0xffffff00` for the second word, and the harness
proves that changing only `content[4]` reaches the O5 EqD-adjustment callback.
It also proves the OEM triple-copy cadence explicitly: copies 1 and 4 are
handled while copies 2 and 3 are suppressed.

It cross-compiles cleanly against the XR500v's real Linux `6.12.80` build as
an ELF32 big-endian MIPS32r2 module.  `modinfo` reports the matching
`6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT` vermagic and no module
dependency.  After normalizing inherited comment/alignment style, both the
core and harness pass strict checkpatch with zero errors, warnings or checks.
Undefined-symbol inspection contains only allocator, memory, printk and
stack-protector primitives, confirming the absence of hardware accessors.

Final archived artifact for this checkpoint:

```text
path    /home/cristuu/openwrt/bin/targets/econet/en751221/
        xr500v-gpon-ploam-lab-6.12.80.ko
size    321344 bytes (unstripped)
sha256  b7b7dadc2e2d74af2ed460fbd6f447c537fbcf5ae294f6a14be611c0858c8e6a
```

Because it is a separate module and is not selected into the image, it
consumes none of the `0x2ffe00` decompressed-kernel budget.

## Runtime validation on the XR500v

The exact module above was copied temporarily to the running XR500v and its
remote SHA-256 was verified before loading.  The router was running the
matching `6.12.80 SMP PREEMPT` kernel.  The phase-27 experimental xPON
platform device remained unbound and the passive EN7570 diagnostic reported
`register_data_writes: 0`.

One `insmod` executed only the in-memory vectors.  It registered no device and
reported:

```text
xr500v-gpon-ploam-lab: PASS, software-only O1-O6 and OEM dedup vectors; no hardware access
```

The module was then removed normally and logged `unloaded`.  The temporary KO
was deleted.  Final checks showed the module and file absent, the xPON device
still unbound, and EN7570 register-data writes still zero.  The boot ID was
unchanged throughout the test.  No fibre state, power cycle or optical
recovery action was required for this software-only test.

## Next boundary

1. Treat phase 27 as a consumed one-shot with terminal-only evidence; do not
   repeat it.  Its exact boundary is recorded in
   `2026-07-15-gpon-phase27-live-run-partial-evidence.md`.
2. Build an EN751221-specific, initially passive GPON MAC adapter with
   explicit `soc_data` for FIFO word order and shared QDMA interrupt demux.
3. Connect the proven PLOAM state machine only after the EN7570/digital PHY
   reaches optical receive sync with TX still fail-closed.
4. Add real GEM/OMCI/QDMA transport after O5; the present EN7523 netdev
   placeholders cannot carry PPPoE by themselves.
