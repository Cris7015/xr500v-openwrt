# GPON bring-up — phase 27 terminal LOS trace compile-only

Date: 2026-07-14

Status: **compiled and audited; never loaded; router not modified by this phase**

> **Same-day hardening amendment:** the later EN757x source cross-check proved
> that ID `0x03` is shared by EN7570 and EN7571.  The final phase-27 build now
> also reads `0x015c`, requires this XR500v's exact variant `0x01` before the
> cold map, publishes it in the evidence and checks the cached identity in
> every per-write fast gate.  The write table and timing sequence are
> unchanged.  The final hashes below reflect that rebuild; live staging and
> installation state are recorded separately in
> [`2026-07-14-gpon-phase27-live-preparation-no-fibre.md`](2026-07-14-gpon-phase27-live-preparation-no-fibre.md).

## Result

Phase 27 is a one-shot, deliberately non-shipping observer for characterising
the undocumented third byte of EN7570 `LOS_CTRL2` at `0x0120`.  It reproduces
only the exact 15-write reset, same-unit RSSI, gain and LOS prefix already
reached safely in phase 26.  It then performs a finite read-only trace.  It has:

- exactly 15 possible EN7570 I2C writes;
- zero APD writes;
- zero xPON MMIO writes;
- zero GPIO writes or direction changes;
- zero retries, rollback writes, workers, timers or arbitrary payload paths;
- an exact EN7570 `ID/variant = 0x03/0x01` read-only gate before any write;
- 12 timestamped samples followed by one terminal 29-register snapshot;
- GPIO and xPON barriers revalidated in every sample, critical EN7570 barriers
  at 50 ms and later, and all 16 static TX guards at 100 ms and later;
- a read-only debugfs evidence channel established before the first write;
- one global invocation, module self-pinning and a mandatory physical-power-cut
  recovery boundary after any transfer attempt.

The full OpenWrt image, standalone module and package compiled successfully.
The image satisfies the XR500v kernel diet, 512-byte gap and TrendChip header
requirements.  Nothing from phase 27 was flashed or loaded during this
compile-only stage.

## Why this trace is needed

The unit has produced both `LOS_CTRL2.byte2` outcomes under both fibre
conditions:

| Outcome | Fibre disconnected | Fibre connected |
|---|---|---|
| `0x22` | phase 16 | stock and phases 17, 19 and 21 |
| `0x23` | phase 10 residue | phase 26 |

That is already enough to reject a direct interpretation such as
"`0x22` means no fibre" or "`0x23` means fibre present".  The OEM
`mt7570_LOS_init()` routine reads all four bytes, intentionally changes only
the documented count/confidence fields in bytes 0 and 1, and writes all four
bytes back.  It neither deliberately sets nor interprets byte 2.  The OEM LOS
display instead uses `LOS_DBG` `0x0130`, byte 3 bit 0.

Phase 27 therefore treats byte 2 as an **outcome to observe**, never as a
configuration gate or permission for another write.  The goal is to determine
whether `0x22` and `0x23` are stable, transient or a timed transition after the
LOS trigger.  This phase does not assign either value a semantic name.

## Exact irreversible prefix

The sole write table is byte-identical to phase 26 writes 1 through 15:

| Attempt | Register | Length | Fixed payload |
|---:|---:|---:|---|
| 1 | `0x0300` | 4 | `01 00 00 00` |
| 2 | `0x0014` | 2 | `00 34` |
| 3 | `0x0014` | 2 | `00 74` |
| 4 | `0x0024` | 1 | `02` |
| 5 | `0x0159` | 1 | `10` |
| 6 | `0x0014` | 2 | `00 34` |
| 7 | `0x0159` | 1 | `10` |
| 8 | `0x0014` | 2 | `00 24` |
| 9 | `0x0024` | 1 | `00` |
| 10 | `0x0014` | 4 | `00 24 05 00` |
| 11 | `0x011c` | 4 | `07 1f 3c 36` |
| 12 | `0x0024` | 4 | `00 00 01 04` |
| 13 | `0x0024` | 4 | `00 00 41 04` |
| 14 | `0x0120` | 4 | `05 1f 00 00` |
| 15 | `0x011c` | 4 | `06 1f 1c 10` |

The APD `0x0030` writes from phases 25/26 are not present in the table or in
any call site.  The only register-data-write helper accepts the next fixed
table index, checks the full fast TX gate, records the attempt before the raw
transfer and cannot retry.

The RSSI acceptance oracle remains deliberately unit-specific and exhaustive:

```text
020a / 0284
020a / 0285
020b / 0285
020b / 0286
```

The two ADC readings must form one of those four pairs.  Both latch readbacks
are separate exact-`00` checks; they are not values in the pair oracle.

## Timestamped trace model

`prefix_end_ns` is the completion timestamp of the acknowledged write 15.
The nominal absolute deadlines relative to that origin are:

```text
0, 5, 10, 20, 50, 100, 250, 500, 1000, 2000, 5000, 10000 ms
```

Every sample records `start_ns` immediately before its first `LOS_CTRL2` read,
`los_ctrl2_done_ns` immediately after that read, and `end_ns`.  The debugfs
report converts all three to
microseconds relative to `prefix_end_ns`.  A late sample is executed
immediately and remains visibly late; deadlines never accumulate relative to
the preceding sample.

The PON I2C bus is intentionally fixed at 100 kHz, so the trace uses three
immutable levels instead of trying to read the full map at every deadline:

| Level | Targets | I2C reads | Wire-time lower bound |
|---|---|---:|---:|
| dense | 0, 5, 10, 20 ms | 4 | about 2.88 ms |
| critical | 50 ms | 12 | about 8.64 ms |
| full | 100 ms and later | 28 | about 20.16 ms |

This removes the guaranteed `>20 ms` backlog of the initial all-full design.
The 5 ms point has theoretical margin but can still be late because of
controller, scheduler and MIPS overhead.  `start_us` and `los2_done_us` remain
the authoritative measurements, so any real lateness is explicit rather than
silently attributed to the nominal target.

`LOS_CTRL2` is always the first I2C read.  The levels are cumulative:

1. dense reads LOS timer `0x0124`, timeout `0x012c` and LOS debug `0x0130`;
2. critical adds timeout count `0x0128`, `LOS_CTRL1`, `SVADC`, OVP latch,
   unchanged APD DAC, SAFE protect, Ibias and Imod;
3. full adds all 16 static post-LOS TX guards;
4. every level captures retained physical TX-disable GPIO state and read-only
   xPON state.

No sample is counted scientifically until its capture and safety verification
both pass.  A failed sample retains separate `capture`, `verify` and raw
`outcome` results, records `halted_sample`, stops the series and preserves a
best-effort terminal map.

## Result semantics

The output deliberately separates execution from observation:

- `sequence_result=0` means the permitted writes, reads and all safety checks
  completed; it does not assert that byte 2 was stable.
- `trace_result=0` means all 12 samples were captured and safety-verified.
- `outcome_result=0` means every accepted byte-2 observation was one of the
  previously seen values `0x22` or `0x23`.
- `outcome_result=-ERANGE` means at least one accepted observation was a new
  value; it is a scientific result, not an electrical failure.
- `transition_count` records changes between consecutive accepted
  observations.  A valid `22 -> 23 -> 22` acquisition therefore has
  `sequence_result=0`, `outcome_result=0` and `transition_count=2`.
- `outcome_22_count`, `outcome_23_count`, `outcome_other_count`, `first_byte2`
  and `final_byte2` include verified samples plus the verified terminal map.

Bytes 0, 1 and 3 of `LOS_CTRL2` remain exact safety/configuration checks:
`05 1f ?? 00`.  Byte 2 can never authorise a write or another stage.

## Staged electrical evidence

Every accepted sample requires the active-high TX-disable pad to remain an
output with logical/raw value 1.  It also requires xPON TXEN, rogue, PRBS, test
and interrupt-enable paths to remain clear while GPON mode and the retained
xPON setting remain exact.  Those physical/digital barriers are inexpensive
MMIO/GPIO reads and therefore remain in the dense 0/5/10/20 ms samples.

The fast gate immediately before each of the 15 fixed writes retains the phase
26 OVP, SAFE, Ibias, Imod and GPIO/xPON checks, and additionally rejects a
cached identity other than exact `0x03/0x01` without adding another I2C
transfer to the timing-sensitive prefix.
Cold and post-reset full maps independently prove the unchanged APD DAC.  There
are no writes at all after the LOS trigger, so the early dense reads do not
open a new actuation path.

The critical 50 ms sample and every later full sample revalidate:

```text
APD_DAC:        00 08 00 00
APD_OVP_LATCH:  00 00 00 00
SAFE_PROTECT:   ff 8f ff 0f
IBIAS:          00 00 00 00
IMOD:           00 00 00 00
```

Each full sample from 100 ms onward additionally re-reads and verifies the
exact 16 phase-26 static TX guards: TIAMUX, MPD targets, T1 delay, Tx-SD, LA
power, BGCKEN, PI TGEN, both P0/P1 current-source control/latch groups,
rogue-TX, ERC filter and self-cleared software reset.  Omitted fields retain
`-ECANCELED`, and the level verifier rejects any accidental read or missing
read inconsistent with the immutable schedule.

## Evidence-channel and recovery invariants

The debugfs directory and read-only `status` file are created before
`los_trace_run()`.  Creation is fail-closed: if either object is unavailable,
the probe returns without attempting a write.  A mutex blocks readers until
the complete result and restored adapter retry count are stable.

Immediately before the first permitted register-data write the driver globally
claims the single sequence, self-pins the module and sets
`physical_powercut_required=true`.  The I2C segment remains locked around the
identity check, final cold snapshot, complete prefix, trace and terminal
snapshot; adapter retries are forced to zero and restored only after the
sequence.  There is no software rollback.  Any post-claim failure keeps the
platform instance alive so its evidence can be read, but physical power
removal remains mandatory.

## Temporary experimental binding

Only the disposable OpenWrt build tree was changed from the passive binding:

```dts
compatible = "econet,en751221-en7570-los-trace-experimental";
reg = <0x1faf0000 0x1000>;
econet,allow-en7570-los-trace;
tx-disable-gpios = <&gpio 16 0>;
airoha,en7570 = <&en7570>;
nvmem-cell-names = "factory-bob";
```

The DTB at the end of `kernel.bin` is byte-identical to the standalone
11,713-byte DTB, and the outer image's LZMA stream decompresses exactly to that
kernel artifact.  The canonical board DTS remains passive; only the new
package, audit script and this compile-only record are canonical changes.

## Source and module audit

The canonical source, OpenWrt package source and copied kernel build-directory
source are byte-identical after the final build.

```text
driver source SHA-256:
4330f03079aab27cda17151701ef4781df714d0eeac7fa1f43020aecca86ecec

package Makefile SHA-256:
4b055ceb9476a1c9b9a4659a68415de1af0253c6310664c25857c8fc5ff2180a

source Makefile SHA-256:
235405d858bbfa4a42f8c42cc3318c705784a97ba1736b49a4661567d00e52ca

static audit SHA-256:
a66265c6125ffd3fb32c81283859cfdfb5d847303768ee9d671294002c9f5d67

unstripped KO SHA-256:
31ab9ee5892baabe7e80ee1105da538dbaa5b3a4da199edf09079e5c90d43e11
unstripped KO size: 417092 bytes

installable KO SHA-256:
4f4d568f6d1b25491efe4fa2ebcf4377834bccb39bdf8478b43f83b2f764b152
installable KO size: 28268 bytes

APK SHA-256:
d9b0384545d84db08172fc2b44cbb1dffa5eb2edf79837fbaadf62ce0b175bfa
```

`modinfo` reports kernel `6.12.80`, MIPS32r2 SMP/preempt, dependency only on
`i2c-core`, and the sole parameter `arm_en7570_los_trace:bool`.  Undefined
symbol inspection contains `__i2c_transfer` and `ioread32`, as intended, and
contains no `iowrite`, `writel`, `writeb`, `gpiod_set`, regulator, worker,
timer or kthread path.  `seq_write` is an internal `seq_printf` dependency;
the actual file operations come from `DEFINE_SHOW_ATTRIBUTE` and the only
debugfs file is mode `0444`.

The reproducible static audit reports:

```text
fixed_writes: 15 exact; APD/MMIO write paths absent
trace_schedule: 0:dense 5:dense 10:dense 20:dense 50:critical 100:full 250:full 500:full 1000:full 2000:full 5000:full 10000:full
RSSI oracle pairs: 020a/0284 020a/0285 020b/0285 020b/0286
dense=4 reads; critical=12 reads; full=28 reads; LOS_CTRL2 always first
critical checkpoints validate APD/OVP/SAFE/currents; full adds 16 TX guards
evidence channel precedes writes; verdict precedes outcome classification
exact EN7570 identity 03/01 is read before the cold map and guarded before every write
```

Three auditor contract tests in `tests/test_audit_phase27_los_trace.py`,
including mutations, prove the auditor rejects removal of the probe ID check
or either half of the cached per-write identity pair.  This prevents the
static PASS message from overclaiming a weakened `03/01` guard.

Strict checkpatch reports zero errors and zero warnings; 15 inherited
alignment-only style checks remain.  `git diff --check` is clean.  Independent
protocol, timing and artifact reviews found no remaining safety blocker.  The
100 kHz timing-resolution limitation above is retained explicitly as a
scientific caveat.

## Image audit

The package is configured `=m`, has no `AUTOLOAD` and is absent from the final
SquashFS, autoload directory, target manifests, `profiles.json` and
`DEVICE_PACKAGES`.  The phase-26 observer is also absent.  The experimental KO
must be copied manually after a cold passive preflight.

```text
patched image:
/home/cristuu/openwrt/bin/targets/econet/en751221/
openwrt-econet-en751221-tplink_archer-xr500v-squashfs-sysupgrade-phase27-los-trace-patched.bin

patched image SHA-256:
3ca86942dfb6299e45a21efdd850f360c45b343b87e357f8c2eec71209ddef7c

raw image SHA-256:
19843292f86f0cb5249d65cb2dab99a360bed722a5934e27badca06ffd6676af

decompressed kernel SHA-256:
5f5f8dd25b874d0ce78b77e87ba51d1ba81eec12d795a2e4fc5abefe2455c5a1
decompressed kernel size: 2948049 bytes
safe kernel headroom:     197167 bytes

standalone DTB SHA-256:
cb97e9e7719ec6dfdcfe19f4d387fa353612c6fe043aa1d8f455ec38f95b9775

SquashFS payload SHA-256:
a08c4b835634713cff5c605b3168e1b2bbf9533c1fe518fc44ebae61c138c7e0
```

The validator and supplemental byte checks report:

```text
file size:              11095666 bytes
kernel payload limit:   0x2ffe00
rootfs file offset:     0x300200
512-byte gap:           present and all zero
gap SHA-256:            076a27c79e5ace2a3d47f9dd2e83e4ff6ea8872b3c2218f66c92b89b55f36560
TrendChip header:       valid
raw/patched difference: 19 bytes, all inside the 512-byte header
last changed byte:      file position 144 / zero-based offset 0x8f
post-header payload:    byte-identical
```

The archived unstripped module is:

```text
/home/cristuu/openwrt/bin/targets/econet/en751221/
xr500v-en7570-los-trace-observer-phase27.ko
```

It is byte-identical to the audited build KO.

## Deliberately not executed

No phase-27 image was installed, no module was copied to the router, no
`insmod` was issued and no EN7570 or xPON state was changed during this stage.
The router can remain on the known passive phase-14 OpenWrt image and the fibre
can remain in the household Movistar router until a separately controlled live
window.

The future live procedure must begin fibre-disconnected with a real cold power
cycle, validate the exact passive TX barriers again, copy and hash the archived
KO, connect fibre only after the preflight, invoke the module exactly once,
capture debugfs immediately, and finish with a physical power removal.  It must
never retry or unload the pinned module in the same power session.
