# EN751221 xPON WAN-QDMA lifecycle and one-shot rearm — offline checkpoint

Date: 2026-07-26  
Scope: source audit, clean MIPS builds, OpenWrt package builds and static
artifact inspection only  
Router: not accessed  
Fibre: connected to the normal Movistar ONT, not the XR500v  
PON MAC/PHY activation, optical TX, ONU registration and OLT validation: not
attempted

## Result

The compile-only EN751221 WAN-QDMA xPON ingress seam now has a complete
single-provider/single-consumer lifecycle contract:

```text
/home/cristuu/openwrt/package/kernel/econet-eth/patches/401-en751221-xpon-consumer-lifecycle-rearm.patch
/home/cristuu/openwrt/package/kernel/econet-eth/files/include/econet-xpon-qdma.h
/home/cristuu/openwrt/package/kernel/xr500v-en751221-xpon-qdma-observer
```

Both `econet-eth` and the observer are release `r2`.

This closes the three QDMA lifecycle blockers recorded after the fail-closed
PLOAM r3 checkpoint:

1. Logical consumer registration now survives provider absence and reprobe.
2. Removal masks and acknowledges QDMA IRQs, waits for in-flight handlers and
   releases requested IRQs before destroying NAPI/page pools.
3. Each delivered xPON cause bit is one-shot: the IRQ path masks it before the
   callback, and only a source-draining worker may explicitly rearm it.

No PLOAM core, MAC, PHY, FIFO or optical transmitter was connected to this
interface.

## Contract

The public callback is:

```c
void (*event)(void *priv, u32 generation, u32 events);
```

The exported process-context operations are:

```c
int en75_xpon_qdma_register_consumer(
	const struct en75_xpon_qdma_consumer *consumer);
void en75_xpon_qdma_unregister_consumer(
	const struct en75_xpon_qdma_consumer *consumer);
int en75_xpon_qdma_rearm_events(
	const struct en75_xpon_qdma_consumer *consumer,
	u32 generation, u32 events);
```

The implemented rules are:

- only one logical consumer and one WAN-QDMA provider may exist;
- registration succeeds and persists even when the provider is absent;
- a later provider probe automatically attaches that registration;
- the callback runs in hard-IRQ context with the QDMA IRQ spinlock held;
- the callback may only update atomic state and schedule work;
- delivered bits are recorded as pending and masked before the callback;
- a worker wanting a later delivery must drain the underlying MAC/PHY cause
  before calling `rearm_events()`;
- leaving a bit masked is valid and fail-closed;
- rearm checks consumer identity, the exact pending mask and a nonzero
  attachment generation;
- the generation changes for provider reprobe and for unregister/re-register
  on the same provider;
- stale work therefore returns `-ESTALE` instead of rearming a replacement;
- unregister detaches under the IRQ lock and waits for any hard-IRQ callback;
- the consumer must then cancel its own deferred work before freeing state.

The generation was deliberately made an **attachment epoch**, not merely a
provider ID. During final review, the weaker provider-only scheme was found to
permit this ABA sequence:

```text
old callback queues work
-> unregister
-> same callback/priv/mask registers again on the same provider
-> old work sees the same identity and provider generation
-> old work can rearm a new event
```

Incrementing the epoch on every attachment closes that window.

## IRQ and teardown hardening

Each QDMA IRQ now records whether `devm_request_irq()` actually succeeded.
Teardown acts only on those requested IRQs and performs this order:

1. Disable QDMA IRQ/DMA configuration globally.
2. Under each IRQ spinlock, clear cached and hardware masks.
3. W1C-acknowledge all QDMA status words and read them back.
4. Drop the spinlock.
5. Call `synchronize_irq()`.
6. Call `devm_free_irq()` and clear the requested flag.
7. Only then destroy RX/TX NAPI, page pools and the dummy NAPI netdev.

Provider unpublish occurs before the `users` early return. The logical consumer
is retained for a later provider, but its provider-local callback snapshot is
gone before teardown continues.

Partial-probe cleanup was also hardened:

- RX cleanup tests the DMA address and buffer before
  `virt_to_head_page()`;
- RX NAPI is deleted only if it was added;
- an IRQ failure cannot synchronize or free an unrequested IRQ.

The observed lock order is:

```text
qdma->lock
  -> en75_xpon_wan_qdma_lock
    -> irq->lock_irq
```

`synchronize_irq()` is never called while the IRQ spinlock is held. The hard
IRQ path takes only `irq->lock_irq`, so holding the global mutex across the
lifetime barrier does not deadlock and prevents a new attachment during that
barrier.

## Observer semantics

The observer is deliberately narrower than a future PHY/MAC consumer:

- it is non-autoloaded;
- loading it without `observe_wan_phy=1` leaves xPON causes disabled;
- it subscribes only to external PHY cause bit 24;
- it has no PON MMIO, GPIO, I2C, FIFO, PLOAM, OMCI, QDMA TX or transmitter
  operation;
- it reports the most recent attachment generation and coalesced accounting;
- it deliberately does **not** rearm because it cannot drain the underlying
  PHY source.

With explicit opt-in it can therefore receive at most one delivery of its PHY
bit per attachment epoch. This is intentional fail-closed behavior, not a
repeating event monitor. Generation history is not retained.

The observer references only:

```text
en75_xpon_qdma_register_consumer
en75_xpon_qdma_unregister_consumer
```

It does not reference `en75_xpon_qdma_rearm_events`.

## Offline verification

Strict checks completed with zero errors, warnings or checks:

```sh
cd /home/cristuu/openwrt

./scripts/checkpatch.pl --strict \
  package/kernel/econet-eth/patches/401-en751221-xpon-consumer-lifecycle-rearm.patch

./scripts/checkpatch.pl --strict -f \
  package/kernel/econet-eth/files/include/econet-xpon-qdma.h

./scripts/checkpatch.pl --strict -f \
  package/kernel/xr500v-en751221-xpon-qdma-observer/src/xr500v-en751221-xpon-qdma-observer.c
```

Two consecutive clean `prepare` plus external-module MIPS builds produced
identical unstripped module hashes:

```text
545fa5395e41081db543cc813ab0209cb1d119961e4778476d2b6df3d631df48  econet-eth.ko
66b65771aec8f9d4ae7ffe3c4ad46d4f0ec98c3cf265bb77d74abda6b85f123d  xr500v-en751221-xpon-qdma-observer.ko
```

The OpenWrt package targets also completed successfully and produced:

```text
/home/cristuu/openwrt/bin/targets/econet/en751221/packages/kmod-econet-eth-6.12.80.2026.02.13~c2f855cf-r2.apk
/home/cristuu/openwrt/bin/targets/econet/en751221/packages/kmod-xr500v-en751221-xpon-qdma-observer-6.12.80-r2.apk
```

Current APK snapshot hashes are:

```text
3479e5e9ad5af958f30250f129651a8b84db6c0d98ba5e505b2279ca900b317e  kmod-econet-eth-6.12.80.2026.02.13~c2f855cf-r2.apk
40aeef6738c6d6b47ec09ec4b3332920eb9c7e76ed15a8add8e593b830fc0537  kmod-xr500v-en751221-xpon-qdma-observer-6.12.80-r2.apk
```

These APK hashes identify the current signed package snapshots, not a
reproducibility identity; package/signing metadata may change them.

Current package-build module hashes are:

```text
30cd3314ba1882d3e9c43b55df46a985fd8858340fb462b86ddc6c2d1cf3cab1  econet-eth.ko
cca9990cf33fc93178cc5012da4697e7b8720c370f44a7350d677408fd2f0404  xr500v-en751221-xpon-qdma-observer.ko
```

The core exports all three lifecycle symbols. The observer has:

```text
license:  GPL
depends:  econet-eth
name:     xr500v_en751221_xpon_qdma_observer
vermagic: 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

Selected source hashes:

```text
15a5038084342c685918a7981897a625454af359b68eab1a98346073623667ea  401-en751221-xpon-consumer-lifecycle-rearm.patch
61fc80ee0273ea16a8312f2f32f728b6c6d2002c759f085952c4899964071ab7  econet-xpon-qdma.h
```

OpenWrt printed its pre-existing out-of-sync `.config` warning and unrelated
optional-feed dependency warnings. Neither package target failed.

## What this does not prove

This checkpoint proves patch application, MIPS compilation/linking, exported
symbol shape, teardown ordering by source audit and fail-closed observer
boundaries.

It does **not** prove:

- that bit 24 is asserted correctly on live EN751221 hardware;
- that a PHY or GPON MAC cause can be drained and rearmed;
- PLOAM FIFO ordering through this IRQ seam;
- O2-to-O3 transition;
- upstream burst timing or optical TX;
- OLT reception, ONU registration, O5, GEM, OMCI or WAN traffic.

No module from this checkpoint was installed or loaded on the XR500v.

## Next safe boundary

The next useful step is still offline:

1. Define one ordered PHY/MAC worker that owns source drain and rearm.
2. Give its O2-to-O3, O3-to-O4 and O4-to-O5 operations explicit
   rollback/readback contracts.
3. Connect it first to synthetic or injected event tests, not the live fibre.
4. Keep the observer one-shot and keep all optical TX, ISP identity programming
   and upstream FIFO writes disabled.

Live use should wait until that consumer can prove:

```text
IRQ callback
-> queue exactly one work item
-> drain/ack the underlying source
-> complete or roll back the state transition
-> rearm the same attachment generation
```

An isolated controllable OLT/test bench remains required before any upstream
transmission or O3/O5 claim.
