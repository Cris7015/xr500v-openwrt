# EN751221 GPON PLOAM fail-closed FSM — offline checkpoint

Date: 2026-07-26  
Scope: source, executable host self-tests, clean MIPS builds and static artifact
inspection only  
Router: powered off  
Fibre: connected to the normal Movistar ONT, not the XR500v  
Optical TX, ONU registration and OLT validation: not attempted

## Result

The software-only PLOAM package is now release `r3`:

```text
/home/cristuu/openwrt/package/kernel/xr500v-gpon-ploam-lab
```

This checkpoint closes the protocol-core blockers identified after the r2 wire
decoder:

- every hardware-facing callback returns an error;
- downstream processing, start and notification APIs propagate those errors;
- logical state, ONU-ID, EqD, REI sequence and AES index are committed only
  after the corresponding operation succeeds;
- a failed handler restores the downstream first-of-three filter state, so the
  next identical OLT copy can retry;
- `prepare_state_change` explicitly runs before the core state commit;
- all PLOAM APIs must be serialized by one ordered worker and must never run
  from hard-IRQ context;
- Password, Dying Gasp, ACK and both AES fragments use the EN751221 OEM
  three-copy repetition contract;
- `Request_Key` synchronously generates and loads exactly the same 16-byte key
  that is sent upstream;
- a failure on AES fragment 2 retains the same pending key/index, so the retry
  cannot combine fragments from different keys;
- AES key material is explicitly erased on completion, reset and free;
- `Assign_Alloc_ID` implements only OEM operation `0x01` as allocate and
  `0xff` as deallocate; reserved operations and `alloc_id == onu_id` do not
  mutate T-CONT state but are still ACKed;
- `Ranging_Adjustment` uses an absolute, idempotent callback target, commits
  core EqD only after the ACK succeeds, and rejects addition/subtraction
  overflow with `-ERANGE`;
- O7 emergency state remains sticky across `ploam_reset()` until a valid
  participate message releases it.

No EN751221/EN7570 hardware adapter was added. The package still has no MMIO,
IRQ, FIFO, I2C, GPIO, laser, PHY, netdev, DT match or autoload path.

## Exact upstream contracts exercised

The self-test uses only a synthetic lab ONU-ID, serial and password. It does
not contain the ISP identity or any private r14 record.

| Operation | Numeric three-word message | Copies |
|---|---|---:|
| Password | `38020001 02030405 06070809` | 3 |
| Dying Gasp | `38030000 00000000 00000000` | 3 |
| Reserved Alloc-ID ACK | `38090a38 0a123002 00000000` | 3 |
| AES index 1, fragment 0 | `38050100 00010203 04050607` | 3 |
| AES index 1, fragment 1 | `38050101 08090a0b 0c0d0e0f` | 3 |
| AES index 0, fragment 0 | `38050000 00010203 04050607` | 3 |
| AES index 0, fragment 1 | `38050001 08090a0b 0c0d0e0f` | 3 |

The second successful key request proves index rotation `0 -> 1 -> 0`.

## Executed self-tests

A small host compatibility layer was added under:

```text
/home/cristuu/openwrt/package/kernel/xr500v-gpon-ploam-lab/tests
```

It compiles and executes the exact same three implementation files used by the
kernel module:

```sh
cd /home/cristuu/openwrt
package/kernel/xr500v-gpon-ploam-lab/tests/run-host-selftests.sh
```

Result:

```text
xr500v-gpon-ploam-lab: PASS, RX decoder, logical O1-O7 rollback, OEM upstream contracts, and injected retries; no hardware access
```

The same sources also passed one host build/run with AddressSanitizer and
UndefinedBehaviorSanitizer enabled.

The executable tests cover:

1. Native/swapped FIFO normalization and denormalization.
2. Pure `Upstream_Overhead` and `Extended_Burst_Length` decoding.
3. O1 through O7 logical transitions.
4. OEM first-of-three downstream filtering and seven-byte Ranging-Time
   comparison.
5. Failures entering O2, applying overhead, assigning ONU-ID, programming EqD,
   enabling FEC and preparing O5.
6. Immediate identical-message retry after each failure.
7. Password, Dying Gasp, ACK and AES wire words/repetition counts.
8. Reserved, allocate and deallocate Alloc-ID behavior.
9. Key-generation failure with zero key fragments sent.
10. Failure specifically on AES fragment 2, followed by same-key/same-index
    retry.
11. Alloc-ID callback failure with no ACK until local success.
12. Upstream transport failure and identical-message retry.
13. Ranging-Adjustment ACK failure without double-applying the offset.
14. Ranging-Adjustment addition and subtraction overflow rejection.
15. Sticky O7 reset and release behavior.

`checkpatch.pl --no-tree --strict --file` reports zero errors, warnings or
checks for the final implementation and host compatibility files. ShellCheck
passes the host runner.

## Clean MIPS build evidence

Command:

```sh
cd /home/cristuu/openwrt
make -s \
  package/kernel/xr500v-gpon-ploam-lab/clean \
  package/kernel/xr500v-gpon-ploam-lab/compile \
  CONFIG_PACKAGE_kmod-xr500v-gpon-ploam-lab=m
```

Two consecutive clean OpenWrt/Linux `6.12.80` MIPS builds produced the same
unstripped module SHA-256:

```text
54fc080e79d30c2efdc58ddff0242116e98bb2e0fde45c535bd614ae889c69aa
```

Final staged artifact:

```text
/home/cristuu/openwrt/bin/targets/econet/en751221/xr500v-gpon-ploam-lab-6.12.80.ko
```

The signed APK is release `r3`. Its container hash changes across equivalent
rebuilds because of signing/package metadata, so it is not the reproducibility
identity; the `.ko` hash above is.

`modinfo`:

```text
license:  GPL
depends:
name:     xr500v_gpon_ploam_lab
vermagic: 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

Module size:

```text
text: 16961
data:   360
bss:      0
total: 17321 bytes
```

The generated module symvers file is empty. The only undefined symbols are
ordinary allocation, stack-protection, logging and memory helpers:

```text
__kmalloc_cache_noprof
__stack_chk_fail
__stack_chk_guard
_printk
kfree_sensitive
kmalloc_caches
memcmp
memset
```

Selected final source hashes:

```text
00a71355a9efbbb9a06f416b6add94dd2e5bf5ccb2cefa39c6239b94fcc65168  xr500v-gpon-ploam-lab-main.c
68affb599942831dece95a90f0e524cc8bed0f64f58508eb63fa2a6754b01455  xr500v_ploam.c
549bc854dc8bae4d717b8c453fa7c1f9ae13fb7d799142ee591ff55ac44c1b41  xr500v_ploam.h
a23519f95aeee169876241d70eae0733c363b798ee954ff90f339631aa186a1e  xr500v_ploam_wire.c
2548ca4dc2f6c941b0062b29470d9318adf6f0f22cdc55d175a18465451640e8  xr500v_ploam_wire.h
23f52b79791321ec83a4670d7f74b18bf8b13177f1a1bd54343c150af4c2ea23  tests/run-host-selftests.sh
```

## Deliberate policy difference from the OEM

The OEM attempts Alloc-ID mutation and then sends an ACK even when its local
T-CONT helper reports failure. This core deliberately does not ACK a valid
allocate/deallocate operation until the local callback succeeds.

That is a fail-closed policy difference, not an assertion that the OEM behaves
the same way. Reserved operations still follow the OEM behavior: no mutation,
ACK x3.

All setters that precede an ACK have an explicit idempotency requirement. A
transport failure restores the downstream filter so the same absolute
operation and ACK can be retried.

## Physical-integration blocker

The core is fail-closed at the logical protocol level, but a sequence of
separate hardware callbacks is not automatically a physical transaction. For
example:

- overhead may already be programmed before preparation of O3 fails;
- ONU-ID may already be programmed before preparation of O4 fails;
- EqD and FEC may already be programmed before preparation of O5 fails.

A real adapter therefore needs composite transactional operations or explicit
rollback/readback for those transitions. Until that exists and is audited, the
software FSM must remain disconnected from the EN751221 MAC/EN7570 PHY.

Every real MAC/AES reset must also call `ploam_reset()` so an in-memory pending
key is never reused after the hardware shadow key was lost.

## QDMA lifecycle audit

The existing patches remain useful compile-only scaffolding:

```text
398-en751221-xpon-irq-bit24.patch
399-en751221-xpon-qdma-wan-consumer.patch
400-en751221-xpon-qdma-active-mask-cache.patch
```

The static single-provider/single-consumer path has coherent lock ordering and
no identified use-after-free. It is not ready for a live PLOAM consumer:

1. Provider reprobe silently loses the registered consumer.
2. Core removal needs a global interrupt mask plus `synchronize_irq()`.
3. xPON event delivery lacks mask-on-delivery/rearm and could IRQ-storm.

The bridge package must therefore remain hardware-inert.

## Next safe boundary

Useful work remains offline and does not require the fibre or router:

1. Define transactional O2-to-O3, O3-to-O4 and O4-to-O5 adapter operations,
   including exact rollback/readback contracts.
2. Define a minimal hard-IRQ snapshot plus one ordered-worker event queue.
3. Close QDMA reprobe/remove/mask/rearm lifecycle gaps.
4. Keep optical TX, identity programming and upstream FIFO writes disabled.

No further ISP-fibre RX repetition is justified by this checkpoint. Actual
upstream response, OLT acceptance, O5, GEM, OMCI and WAN traffic require an
isolated controllable OLT/test bench and separate transmitter authorization.
