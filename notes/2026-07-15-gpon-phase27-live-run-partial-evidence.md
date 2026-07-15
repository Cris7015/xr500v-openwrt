# GPON bring-up — phase 27 live run with partial retained evidence

Date: 2026-07-15

Status: **the one-shot electrical sequence completed once; the detailed
debugfs report was not retained because the local capture directory already
existed; the mandatory physical power cut and disconnected-fibre cold recovery
both completed; this sequence must never be repeated**

## Cold and live-fibre preflight

The router was physically off for at least 35 seconds and booted with fibre
disconnected.  The new boot ID was
`11ca42fc-89d6-456b-861d-4ca6a98943a1`.  The phase-27 DT compatible and
`econet,allow-en7570-los-trace` opt-in were present, while the xPON device was
unbound and no phase-27 module or status existed.

The per-unit factory cell matched the observer's frozen identity.  EN7570
ID/variant were `03/01`; APD remained `00 08 00 00`, OVP and software reset
were zero, LOS_CTRL1/2 were `06 08 3c 36` / `10 05 00 00`, and the passive
diagnostic reported zero register-data writes.  UBI had zero bad PEBs,
maximum erase counter 2 and `rootfs_data` state `OK`.

The read-only xPON diagnostic was bound through `driver_override`.  It showed
GPON mode/FSM 3, retained `XPON_SETTING=0x14f`, no RX sync, TX disabled,
GPIO16 configured output/raw-high/asserted, PRBS/test/interrupt gates zero and
zero MMIO writes.  Two more passive reads after the operator connected fibre
retained every TX and write barrier.

The final standalone module was copied to `/tmp/phase27-los-trace.ko` without
loading it and verified as:

```text
size    417092 bytes
sha256  31ab9ee5892baabe7e80ee1105da538dbaa5b3a4da199edf09079e5c90d43e11
```

## Single invocation

With the operator physically beside the power cord, the passive diagnostic
was unbound, `driver_override` was cleared, and the final module was loaded
once with `arm_en7570_los_trace=1`.  `insmod` returned zero.  The retained
kernel message was:

```text
[ 2893.963547] xr500v-en7570-los-trace-observer 1faf0000.xpon-phy:
EN7570 terminal LOS trace result 0 after 15/15 I2C write attempt(s),
0 MMIO/APD write(s), 12/12 sample(s); TX_DISABLE retained;
physical power removal required
```

This proves that all 15 fixed receiver/LOS writes completed, all 12 scheduled
safety-guarded samples completed, the terminal snapshot passed its immutable
map checks, no MMIO or APD write path ran, and physical TX_DISABLE remained
asserted.  `sequence_result=0` deliberately covers both a recognised
LOS_CTRL2 byte-2 outcome and an accepted `-ERANGE` scientific outcome.  The
one-line message therefore does **not** reveal the twelve byte-2 values,
transitions, RSSI readbacks or final scientific classification.

## Detailed-capture failure

The local output path
`/home/cristuu/tools/xr500v/private-captures/phase27-20260715-021228` had been
created during preparation.  The capture tool then called
`mkdir(..., exist_ok=False)` before its first SSH status read and raised
`FileExistsError`.  Its fail-closed `finally` printed the physical-cut order,
but the directory remained empty: no `status.raw`, logs, validation manifest
or checksums were written.

This was a local capture-orchestration error, not a router or observer error.
No detailed sample series can be reconstructed after power removal, and the
electrical one-shot must not be repeated to replace it.  Only the terminal
kernel line above is retained as run evidence.

## Mandatory recovery

The operator physically removed power immediately and kept the router off for
at least 35 seconds.  Fibre was disconnected while power remained off, then
the router was booted without fibre.  The recovery boot ID was
`33cecd0a-f8ab-414d-9fbd-1e49f78bcecb`.

At 59 seconds uptime:

- the staged KO, phase-27 module and debugfs status were absent;
- the xPON platform device was unbound and `driver_override` was null;
- the complete EN7570 cold map returned to its preflight values;
- APD/OVP/software-reset and register-data-write counters were zero;
- UBI again reported zero bad PEBs, erase counter 2 and volume state `OK`.

The read-only xPON diagnostic was rebound for a final recovery check.  It
reported GPON/FSM 3, no sync, `XPON_SETTING=0x14f`, TX off, GPIO16
raw-high/asserted, PRBS/test/IRQ zero, and zero MMIO or EN7570 data writes.
The router is therefore safely recovered and may remain powered without
fibre.

## Capture-tool hardening

`phase27_capture_status.py` now accepts either a nonexistent output path or an
existing **empty directory**.  It still rejects a regular file or any
non-empty directory, so prior evidence cannot be overwritten.  Every accepted
directory is atomically claimed with an exclusive, durable `.capture.claim`
file before the first SSH read; concurrent capturers cannot both proceed or
reach `os.replace()`.  Regression coverage exercises new and existing-empty
directories, occupied directories, regular files and two simultaneous claims
while retaining the physical-cut instruction on every exit path.

## Scientific boundary

Phase 27 establishes successful execution and safety recovery, but not the
LOS_CTRL2 transition series.  Future work may use the terminal completion as
evidence that the guarded prefix and sampler are electrically viable.  It
must not claim whether byte 2 was `0x22`, `0x23`, mixed or another accepted
value, and it must not rerun this one-shot sequence.
