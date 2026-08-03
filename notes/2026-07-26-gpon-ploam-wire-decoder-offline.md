# EN751221 GPON downstream PLOAM wire decoder — offline checkpoint

> **Superseded blocker status:** this r2 note remains the decoder/build
> evidence. Its PLOAM blockers 1–5 were subsequently closed in the software-only
> r3 core. See
> [`2026-07-26-gpon-ploam-fail-closed-fsm-offline.md`](2026-07-26-gpon-ploam-fail-closed-fsm-offline.md).
> Hardware transactional callbacks and QDMA lifecycle remain unresolved.

Date: 2026-07-26  
Scope: source, clean MIPS builds and static artifact inspection only  
Router: powered off  
Fibre: connected to the normal Movistar ONT, not the XR500v  
Optical TX/OLT validation: not attempted and not established

## Result

A hardware-independent downstream PLOAM wire layer now exists in:

- `/home/cristuu/openwrt/package/kernel/xr500v-gpon-ploam-lab/src/xr500v_ploam_wire.c`
- `/home/cristuu/openwrt/package/kernel/xr500v-gpon-ploam-lab/src/xr500v_ploam_wire.h`

It provides namespaced helpers for the normalized three-word/twelve-byte
message form, header/content packing and unpacking, classification, and pure
decoding of only the two downstream message types established at class level
by the guarded r14 RX experiment:

- `Upstream_Overhead` (`0x01`)
- `Extended_Burst_Length` (`0x14`)

The decoder does not map or access MMIO, read a FIFO, register an IRQ, touch
GPIO/I2C/PHY state, contain an optical identity, or provide hardware upstream
transport. The pack helper only transforms the shared numeric message form.
The functions are linked into the software-only lab module but are not kernel
exports.

The existing full PLOAM state machine was updated to share the namespaced
message/pack/unpack contract and to consume these two semantic decoders
directly. It remains lab-only and is **not** suitable for an EN751221 hardware
adapter yet.

## Synthetic tests compiled into the lab

The module-init self-test now contains:

1. The public/reference normalized `Upstream_Overhead` vector:

   ```text
   ff012000 00aaab59 83200000
   ```

   Expected fields:

   ```text
   ONU-ID:       ff (broadcast)
   type:         01
   guard bits:   20
   preamble 3:   aa
   delimiter:    ab 59 83
   delay mode:   1
   delay time:   0000
   ```

2. A synthetic `Extended_Burst_Length` vector with:

   ```text
   O3 preamble 3: 18
   O5 preamble 3: 20
   ```

3. A negative cross-decoder check returning `-EINVAL`.

4. A synthetic `3 × Upstream_Overhead + 3 ×
   Extended_Burst_Length` sequence. The existing OEM first-of-three filter
   must deliver each message type exactly once. The callback checks all
   overhead preambles, the delimiter, delay fields and both EBL values before
   the rest of the software-only O1–O6 activation test continues. This
   matches only the r14 record-class counts; the payloads are
   public-reference/synthetic.

No private r14 record payload was added to source or documentation. The
self-test code compiled successfully but was not loaded or executed on the
powered-off router during this checkpoint.

## Clean-build evidence

Command:

```sh
cd /home/cristuu/openwrt
make package/kernel/xr500v-gpon-ploam-lab/{clean,compile} \
  CONFIG_PACKAGE_kmod-xr500v-gpon-ploam-lab=m V=s
```

This passed twice against OpenWrt's EN751221 Linux `6.12.80` tree. Both clean
builds produced the same module SHA-256:

```text
49a1c75ae9df7aaa544051fc00b9f0af91c9ea94d69fde5d9b183b1c0e99893c
```

Artifacts:

```text
/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/xr500v-gpon-ploam-lab/xr500v-gpon-ploam-lab.ko
/home/cristuu/openwrt/bin/targets/econet/en751221/xr500v-gpon-ploam-lab-6.12.80.ko
```

The second path had contained a July 15 r1 module. It was explicitly replaced
with the final r2 build and now has the same SHA-256 shown above.

`modinfo`:

```text
license:  GPL
depends:
name:     xr500v_gpon_ploam_lab
vermagic: 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

The only undefined module symbols are ordinary kernel allocation, logging and
memory helpers:

```text
__kmalloc_cache_noprof
__stack_chk_fail
__stack_chk_guard
_printk
kfree
kmalloc_caches
memcmp
memset
```

The generated module symvers file is empty, confirming that the lab exports no
kernel ABI. `checkpatch.pl --no-tree --strict --terse -f` reported no errors,
warnings or checks for the final wire source/header, PLOAM core/header or lab
main file.

Source hashes at this checkpoint:

```text
a23519f95aeee169876241d70eae0733c363b798ee954ff90f339631aa186a1e  xr500v_ploam_wire.c
2548ca4dc2f6c941b0062b29470d9318adf6f0f22cdc55d175a18465451640e8  xr500v_ploam_wire.h
1248afa05d948316a4e0f1326646496b4476921da3eefa13bc18aa1fd7039805  xr500v-gpon-ploam-lab-main.c
1839552f2a733b5f1cb36e676b90ca7264d450b9b5a89c9790411a1d3e852bf0  xr500v_ploam.c
7e31bbb35e2d7acb8cce07c54b112e224f677fc16a8a1237acd8459693512737  xr500v_ploam.h
```

The APK is release `r2`. Its signature/package metadata makes the APK hash
non-reproducible across rebuilds, so the stable `.ko` hash above is the
reproducibility reference.

## Stale bridge artifact corrected

The prior compiled `xr500v-en751221-xpon-bridge.ko` predated its current
hardware-inert source and still contained the old QDMA observer dependency.
It was clean-built again without changing source:

```sh
make package/kernel/xr500v-en751221-xpon-bridge/{clean,compile} \
  CONFIG_PACKAGE_kmod-xr500v-en751221-xpon-bridge=m V=s
```

Current module SHA-256:

```text
cb3629e597a46f4afe36753c6afb23efbc5e3773446d8c02f03ade85ffaa9d46
```

The replacement artifact has:

```text
depends:
parm: request_hardware_enable:bool
```

It no longer contains `observe_qdma_wan` or
`econet_xpon_qdma_*`. This proves only that the artifact now matches the inert
source; it does not make the bridge a driver or grant hardware authority.

## QDMA active-mask cache fix

The gated WAN-QDMA consumer exposed an offline race: while an RX/TX NAPI was
pending, the handler masked its completion bit only in hardware. The cached
`irqmask` still contained the bit, so registering or unregistering the xPON
consumer could write that stale cache back and prematurely revive the
unrelated completion IRQ.

The minimal fix is kept separate in:

```text
/home/cristuu/openwrt/package/kernel/econet-eth/patches/400-en751221-xpon-qdma-active-mask-cache.patch
SHA256 6b1c947b2c01d472368ef494b8e3a0fa33e92402a65b15d6080cd0214524f3ad
```

It changes the IRQ handler from a hardware-only temporary mask to:

```c
irq->irqmask[i] &= ~disable_int;
en75_wreg(irq->irqmask[i], irq->mask_reg[i]);
```

NAPI completion already changes only its own cached bit and then writes the
resulting full mask. The patch adds no event source, register, callback or
hardware authority. `checkpatch.pl` passed and two clean `econet-eth` builds
produced the same module SHA-256:

```text
cb50bc1f32dedf10fcf76e679121c4bae60418120bf6dbe3a1370dc24f69c78e
```

This is compile/static evidence only. Real bit-16/24 delivery and provider
lifecycle/rebind remain unvalidated.

## Remaining blockers

The complete software PLOAM state machine must stay disconnected from hardware
until at least these contracts are fixed and tested independently:

1. Hardware callbacks need error returns so the state machine cannot advance
   after a failed register or FIFO operation.
2. Any IRQ ingress must be minimal and hand work to an ordered worker with an
   explicit state/locking contract.
3. Upstream repetition counts must be reconciled with the EN751221 OEM
   implementation.
4. `Request_Key` needs an unambiguous synchronous key-generation/load
   contract.
5. Reserved `Assign_Alloc_ID` values must not be interpreted as deallocation.
6. RX word order does not validate upstream FIFO word order.
7. The QDMA active-mask cache is now fixed offline, but provider
   lifecycle/rebind and actual bit-16/24 delivery remain unvalidated.
8. Optical TX, burst timing, OLT acceptance, GEM, OMCI and the WAN data path
   remain unvalidated.

## Next safe boundary

The next useful work remains offline:

- harden the PLOAM state-machine callback/error and upstream contracts;
- audit the gated QDMA provider lifecycle without enabling xPON sources;
- keep the r14 live source and its evidence frozen;
- do not deploy the full state machine or enable TX on the ISP fibre.

No fibre connection to the XR500v is required for those steps.
