# GPON bring-up — phase 24 live EN7570 APD A2 observation

Date: 2026-07-13

## Result

The guarded OpenWrt observer executed the isolated OEM EN7570 APD bootstrap
sequence exactly once on the fibre-disconnected XR500v:

```text
00 08 00 00 -> 00 08 20 00 -> 00 09 20 00 -> a2 09 20 00
```

All three I2C transfers and every directed readback succeeded.  The APD state
after each transfer was exact, the OVP latch remained zero, GPIO16 physical
`TX_DISABLE` remained output-high, all 16 analogue/TX/reset guards were
unchanged, the Ibias/Imod control registers remained zero, and every observed
xPON TX/test/interrupt gate remained inactive.

This is a successful observation of the minimum per-unit OEM APD bootstrap on
real silicon.  It is not a fibre-reception result, an APD-rail voltage
measurement or a working GPON path.  The router was physically powered off
immediately after capture, restored to the audited passive image, cold-cycled
again and left at the exact observed passive safety baseline.

The A2-only experiment must not be repeated: its bounded question is now
answered.

## Question and safety boundary

Phase 22 reduced the OEM APD path to two control-bit updates and one DAC-byte
write, and proved that this unit's factory block yields initial code `0xa2`.
Phase 22's functional stock dump established code `b3`.  Phase 23 then
directly observed temperature-derived codes `b1` and `b2` with all 91
corresponding OVP reads zero, plus GPON O5, OMCI activity and PPPoE service on
the authorised live fibre.

Phase 24 asked only whether OpenWrt could reproduce the immediate OEM APD
bootstrap while keeping the rest of the optical transmitter in its cold,
fail-closed state.  Fibre reception, LOS initialisation, thermal APD updates,
current/laser setup, PLOAM, OMCI and the PON data path were deliberately out of
scope.

The observer was a separate one-shot module.  It exposed no writable ABI or
variable payload and allowed only these fixed writes to register `0x0030`:

1. four bytes `00 08 20 00`;
2. four bytes `00 09 20 00`;
3. one byte `a2`.

It used zero adapter retries, incremented its attempt counter before every
transfer, read APD and safety state after every attempt, stopped on the first
error, never attempted rollback, globally consumed its one sequence and
self-pinned before the first write.  Physical power removal was the only
permitted recovery boundary.

## Implementation and image audit

The observer implementation is commit `d6dc2fc386` and remains a manual,
non-autoload package:

```text
source SHA-256:
f348388cb0b1406093edf674efd245d1f49937891f246e804f43fcee8d3f52f5

module SHA-256:
c35cdb63fe7bf9c16f80cc57d62138bde0b874b5351a74792156dc1c0017e594

module size:            360212 bytes
vermagic:               6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

The temporary image changed only the xPON diagnostic node to the exact
experimental compatible, added the immutable DT opt-in, referenced the
existing EN7570 client and exposed the exact 0x190-byte factory BOB NVMEM
cell.  The observer itself was absent from SquashFS, the image manifest and
all autoload directories; it was copied to `/tmp` after the cold boot.

```text
temporary image SHA-256:
3fb0182532f8f2e1f3812c17aec4e0696b0b53fa2e3eb81aa2375ffe9a2d2ec7

temporary DTB SHA-256:
20de4c0de6e70da925eb39d5278e2e1a0befae518152878f1934d21597fdb2ee

image size:              11095630 bytes
compressed kernel:        2948042 bytes
safe kernel headroom:      197174 bytes
rootfs file offset:        0x300200
required 512-byte gap:     present and all zero
TrendChip header/entry:    valid / 0x80020000
```

Independent audits checked the exact PON-I2C controller and resource,
7-bit address `0x70`, EN7570 silicon ID `0x03`, xPON MMIO resource, GPIO chip
and hardware offset 16, per-unit factory SHA-256, all fixed payloads, pinning,
one-shot semantics and every error path before the module was built or loaded.

## Cold preflight

The fibre was physically disconnected.  The external EN7570 received a
physical power-off interval of at least 30 seconds before the experimental
image booted.  No warm `reboot` or bootloader `jump` was accepted as a
substitute because the EN7570 is known to retain state across warm boots.

Immediately before upload, the live unit reported:

```text
DT compatible:          econet,en751221-en7570-apd-a2-experimental
DT allow property:      present
observer module/status: absent / absent
EN7570 ID:              0x03
factory block:          0x190 bytes, exact per-unit hash
APD:                    00 08 00 00
OVP:                    00 00 00 00
SAFE:                   ff 8f ff 0f
Ibias / Imod:           00 00 00 00 / 00 00 00 00
GPIO16 TX_DISABLE:      output-high / asserted
xPON TXEN:              clear
rogue / PRBS / test:    disabled
xPON interrupt enable:  0x00
diagnostic writes:      0
```

The module copied to `/tmp/phase24-apd-a2.ko` was 360212 bytes and its remote
SHA-256 matched the audited artifact before the armed load.

## One-shot execution

For auditability, the historical one-shot invocation was recorded below.  The
A2-only boundary is now closed: **do not execute this command again**.

```text
insmod /tmp/phase24-apd-a2.ko arm_en7570_apd_a2=1
```

It was not removed, retried or loaded a second time.  The strict result was:

```text
INSMOD_RC:             0
silicon_id:            0x03
factory_length:        0x190
factory_hash_matched:  yes
module_pinned:         yes
adapter_retries:       saved=3, during=0, restored=yes
i2c_write_attempts:    3 / 3 maximum
sequence_result:       0
halted_step:           0
physical_powercut:     required
```

The observed transition was:

| Step | Payload | Pre APD | Write | Post APD | Pre/post capture and verify |
|---:|---|---|---:|---|---:|
| 1 | `00 08 20 00` | `00 08 00 00` | `0` | `00 08 20 00` | all `0` |
| 2 | `00 09 20 00` | `00 08 20 00` | `0` | `00 09 20 00` | all `0` |
| 3 | `a2` | `00 09 20 00` | `0` | `a2 09 20 00` | all `0` |

The final byte is not arbitrary.  For this unit, the OEM's initial
`BOSA_temperature = 20 degC`, `47.42 V` 25-degree target, `0.14 V/degC` cold
slope, `30.00 V` legacy zero and `0.103 V/code` step calculate a software
target of `46.72 V` and truncated code 162, or `0xa2`.  This is a calculated
target; no physical rail voltage was measured.

## Safety observations

Six full snapshots were recorded: pre and post for each step.

| Check | Observation |
|---|---|
| APD reads | exact expected state in all 6 snapshots |
| OVP reads | 9 actual reads, all `00 00 00 00`, all result `0` |
| SAFE | `ff 8f ff 0f` in all 6 snapshots |
| Ibias / Imod | both four-byte zero in all 6 snapshots |
| GPIO16 | active-high, output, logical/raw high in all 6 snapshots |
| analogue/TX/reset guards | 16/16 unchanged in all 6 snapshots, 96 successful readbacks |
| xPON TXEN / rogue | clear in all 6 snapshots |
| PRBS / test-frame / IRQ enable | zero in all 6 snapshots |
| retries / worker / arbitrary path | zero / absent / absent |

The nine real OVP reads were three pre-write samples, three immediate
post-write samples and three second post-write samples taken after the rest of
each postflight guard traversal.  The latter are later in execution order but
are not a timed settle or soak measurement; there was no deliberate delay.

The 16 invariant guards covered TIAMUX, MPD targets, T1 delay, Tx-SD,
LA power/gain, bandgap/clock state, PI/TGEN, P0 and P1 CS1/CS3/current latches,
rogue-TX control, ERC filter and software reset.  The final kernel message
reported sequence result zero after three attempts with `TX_DISABLE`
retained.  No mismatch or observer error appeared.

## Physical recovery and passive restoration

As soon as the complete status and kernel tail were captured, the router was
physically powered off for at least 30 seconds.  No module removal, software
reboot or rollback was attempted after the first APD write.

The next cold boot of the temporary image proved that physical power removed
the external state:

```text
observer module/status: absent / absent
APD:                    00 08 00 00
OVP:                    00 00 00 00
passive safety fields:  exact cold baseline
EN7570 data writes:     0
```

The known passive phase-14 image was then copied by explicit name, hashed on
the router and accepted by `sysupgrade -T` before installation:

```text
passive image SHA-256:
0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d

image size:              11095474 bytes
passive DTB SHA-256:     08bc473008ce8eb1a769457c0d587200907fa6cc8137db691544b0feaeb82d62
compressed kernel:        2947970 bytes
safe kernel headroom:      197246 bytes
rootfs file offset:        0x300200
required 512-byte gap:     present
TrendChip header:          valid
sysupgrade image test:     rc=0
```

The stage-2 wrapper closed SSH and printed its known transient `ubus ...
Connection failed` message after `Commencing upgrade`; the device then
flashed, rebooted and returned on the passive DT.  The warm verification found
no experimental compatible, allow property, module, status, autoload or
overlay copy.

A second physical power-off interval of at least 30 seconds was then applied.
The final cold passive boot reported:

```text
DT compatible:           econet,en751221-xpon-phy-diag
experimental allow:      absent
observer module/status:  absent / absent
overlay observer files:  0
APD:                     00 08 00 00
OVP:                     00 00 00 00
SAFE[0:1]:               ff 8f
Ibias / Imod controls:   zero / zero
GPIO16 TX_DISABLE:       output-high / asserted
xPON TXEN:               clear
rogue / PRBS / test:     disabled / zero / zero
xPON interrupt enable:   0x00
passive MMIO writes:     0
EN7570 data writes:      0
```

The raw captures remain local-only.  The live file includes boot-time MAC
addresses, and neither full log is needed to reproduce the public result:

```text
phase24_apd_a2_live_20260713_074939.txt
size: 23598 bytes, 397 lines
SHA-256: 4b98a179ea0ef897355a0ca0a7a7b70a3e8f3180a8a197804df301b5b6f1a013

phase24_final_passive_cold_20260713_080023.txt
size: 3313 bytes, 109 lines
SHA-256: 8ebdfeb97725b5d2fbe98434222c79634c0cd178a4d2b65ce7790bf2d9e929a8
```

The live capture's router clock printed `2026-07-13T01:14:43Z`, while the host
filename and modification time were approximately `07:49 -03`.  The router
clock was not synchronised, so this note records both literally and makes no
derived wall-clock claim.

## What this proves and what it does not

Proved:

1. OpenWrt can execute the minimum OEM APD bootstrap on this EN7570 with the
   exact per-unit initial DAC code and exact readback after every transfer.
2. No observed TX/current-control/reset guard moved and no observed xPON
   transmit or test path opened during the sequence.
3. The undocumented OVP latch remained zero in all nine ordered samples.
4. A physical power cycle restored every observed external-chip passive
   safety field to its cold baseline.
5. The passive DT, with no experimental opt-in or module, was restored and
   independently verified after another cold boot.

Not proved:

1. Fibre was disconnected, so this run did not test LOS, RX sync, GPON O5 or
   whether APD is sufficient for receiver operation.
2. No physical APD voltage, electrical limit or OVP threshold was measured;
   a zero software latch does not certify arbitrary codes or long-term safety.
3. The experiment did not run the OEM thermal worker or advance `a2` to the
   functional stock `b1`/`b2`/`b3` range.
4. It did not combine APD with the reset, RSSI calibration/gain and LOS
   operations from earlier phases, nor initialise the complete stock PHY.
5. It did not add PLOAM, burst timing, GEM, OMCI or WAN-QDMA integration.

## Next boundary

Do not repeat A2-only: the transition and its immediate safety observations
are fully characterised.  The next useful scientific question is a separately
implemented and independently audited, live-fibre RX-only observation which
keeps physical and digital TX disabled while combining only those receiver
prerequisites justified by phases 16 through 24.

That is not a routine extension of this module.  It needs a new compile-only
design, explicit handling of the non-transactional EN7570 state, a one-shot
physical-power recovery plan and a clear decision about initial-only A2 versus
the OEM thermal APD worker.  TX bias/modulation current, MPD/ERC, TGEN, laser
release, PLOAM and the data path remain separate boundaries.
