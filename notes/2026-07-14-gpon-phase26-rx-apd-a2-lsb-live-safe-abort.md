# GPON bring-up — phase 26 live RX/APD A2 one-LSB safe abort

Date: 2026-07-14

## Result

The guarded phase-26 observer was loaded exactly once with the authorised
Movistar fibre connected.  The expanded one-LSB RSSI oracle passed with the
same pair previously seen in the successful phase-21 receiver run:

```text
RSSI Vref:       0x020a
RSSI V:          0x0285
V - Vref:        0x007b
latch initial:   00
latch second:    00
```

All 15 fixed I2C writes through the LOS trigger completed successfully.  The
post-LOS snapshot then stopped the sequence fail-closed because the autonomous
third byte of `LOS_CTRL2` was `0x23`, one bit above the deliberately exact
`0x22` oracle:

```text
i2c_write_attempts:  15 / 18 maximum
mmio_write_attempts: 0 / 1 maximum
sequence_result:     -1 (-EPERM)
halted_step:         15
samples_taken:       0 / 21
LOS_CTRL2 actual:    05 1f 23 00
LOS_CTRL2 oracle:    05 1f 22 00
```

The stop occurred before the xPON RX-polarity MMIO write and before every APD
write and finite receive sample.  There was no ESD/deglitch, current, laser or
TGEN write.  GPIO16 physical `TX_DISABLE` stayed output-high/asserted, xPON
TXEN stayed clear, and every observed TX/test/interrupt gate remained closed.

This run resolves the immediate phase-25 RSSI blocker: the bounded one-LSB ADC
oracle can pass with live fibre and the fixed gain/LOS prefix can then execute
to its terminal gate.  It does **not** answer whether the APD sequence can
observe the fibre, nor does it test APD A2, RX polarity, PLOAM, O5, OMCI or a
network data path.

## Preflight and invocation

The dedicated experimental image was installed while the fibre was
disconnected.  Its remote SHA-256 matched the audited local artifact and
`sysupgrade -T` accepted it.  A physical power cut of at least 35 seconds was
then followed by a fibre-disconnected cold boot.

The cold preflight required and observed:

```text
compatible:             econet,en751221-en7570-rx-apd-a2-lsb-experimental
immutable DT opt-in:    present
observer module/status: absent / absent
EN7570 ID:              0x03
factory block/hash:     exact
complete cold map:      exact
APD:                    00 08 00 00
OVP:                    00 00 00 00
SAFE:                   ff 8f ff 0f
GPIO16 TX_DISABLE:      output-high / asserted
xPON setting:           0x0000014f
TXEN/rogue/PRBS/test:   off / off / zero / zero
xPON interrupt enable:  0x00
passive data writes:    0
```

The read-only xPON diagnostic was unbound and the platform driver override was
set to the unique phase-26 observer name.  The archived module was copied to
`/tmp`; its remote SHA-256 was exact, and it remained unloaded until the fibre
was connected.

The single historical invocation was:

```text
insmod /tmp/phase26-rx-apd-a2-lsb.ko arm_en7570_rx_apd_a2_lsb=1
```

`insmod` returned zero because the observer deliberately preserves its pinned
platform instance after claiming a non-transactional sequence, allowing
debugfs to expose terminal evidence.  The authoritative operation result is
`sequence_result=-1`, not the loader return code.  The command was not repeated
and the module was not removed.

## Exact execution boundary

The reset write succeeded and self-cleared.  The complete post-reset snapshot
matched the exact cold EN7570 map and every GPIO/xPON guard.  Both directed RSSI
ADC reads and their latches then passed the four-pair phase-26 oracle, followed
by gain and LOS programming:

| Attempt | Register | Fixed payload | Result |
|---:|---:|---|---:|
| 1 | `0x0300` | `01 00 00 00` | 0 |
| 2 | `0x0014` | `00 34` | 0 |
| 3 | `0x0014` | `00 74` | 0 |
| 4 | `0x0024` | `02` | 0 |
| 5 | `0x0159` | `10` | 0 |
| 6 | `0x0014` | `00 34` | 0 |
| 7 | `0x0159` | `10` | 0 |
| 8 | `0x0014` | `00 24` | 0 |
| 9 | `0x0024` | `00` | 0 |
| 10 | `0x0014` | `00 24 05 00` | 0 |
| 11 | `0x011c` | `07 1f 3c 36` | 0 |
| 12 | `0x0024` | `00 00 01 04` | 0 |
| 13 | `0x0024` | `00 00 41 04` | 0 |
| 14 | `0x0120` | `05 1f 00 00` | 0 |
| 15 | `0x011c` | `06 1f 1c 10` | 0 |
| 16 | `0x0030` | `00 08 20 00` | not attempted (`-ECANCELED`) |
| 17 | `0x0030` | `00 09 20 00` | not attempted (`-ECANCELED`) |
| 18 | `0x0030` | `a2` | not attempted (`-ECANCELED`) |

`halted_step=15` is the count of already attempted writes; it does not mean
that write 16 started.  RX polarity and all three APD steps also retained
their `-ECANCELED` sentinels.  Zero-filled fields belonging to those
unexecuted stages are not hardware observations.

Immediately before the post-LOS equality check, the terminal map was:

```text
LA_PWD:             00 24 05 00
SVADC_PD:           00 00 41 04
ADC_PROBE:          85 02 00 00
LOS_CTRL1:          06 1f 1c 10
LOS_CTRL2:          05 1f 23 00
LOS_TIMEOUT_COUNT:  00 00 00 00
LOS_TIMEOUT:        3e 00 00 00
APD_DAC:            00 08 00 00
APD_OVP_LATCH:      00 00 00 00
SAFE_PROTECT:       ff 8f ff 0f
```

The sole snapshot mismatch was bit 0 of `LOS_CTRL2.byte2`: `0x22 -> 0x23`.
The phase-26 post-LOS verifier compares all four bytes, so it returned
`-EPERM` before the polarity call.  Consequently the observed attempt totals
of 15 I2C and zero MMIO writes establish the exact boundary independently of
the stage label.

The kernel result was:

```text
snapshot mismatch los_ctrl2@0x0120: 05 1f 23 00 != 05 1f 22 00
EN7570 RX/APD A2 LSB result -1 after 15 I2C and 0 MMIO write
attempt(s), 0 sample(s); TX_DISABLE retained; physical power removal required
```

## What `LOS_CTRL2.byte2` does and does not prove

The OEM `mt7570_LOS_init()` code read-modify-writes only the documented LOS SD
count and confidence fields in bytes 0 and 1 of `LOS_CTRL2`.  It does not
program or interpret byte 2.  The public OEM headers available in this tree do
not name byte-2 bit 0.  The OEM's displayed LOS value instead comes from
`LOS_DBG_RG` at `0x0130`, byte 3 bit 0, so this `0x0120` bit is not the
documented LOS indicator.  The evidence supports treating it as an autonomous
result of the LOS block, not writable configuration; the exact analogue or
calibration meaning remains an inference.

This unit has now produced both terminal values:

- stock captures and the successful phase-21 live receiver prefix observed
  `05 1f 22 00`;
- the failed phase-10 isolated LOS experiment observed a byte-2 value of
  `0x23` which remained stable across repeated reads, and this phase-26 run
  observed `0x23` again; both also captured timeout `0x3e`.

Those observations prove that exact `0x22` is not invariant after the fixed
LOS prefix.  They do **not** identify bit 0 as success, failure, timeout,
optical power, LOS assertion or fibre presence.  One live run cannot separate
those hypotheses, and this note deliberately does not assign the undocumented
bit a semantic name.

## Terminal safety evidence

The best-effort terminal snapshot captured successfully after the mismatch:

```text
GPIO16:                   active-high, output, logical/raw high
APD:                      00 08 00 00 (unchanged baseline; no APD write)
OVP:                      00 00 00 00
SAFE:                     ff 8f ff 0f
Ibias / Imod controls:    zero / zero
xPON setting:             0x0000014f (no polarity write)
xPON TXEN:                clear
rogue / PRBS / test:      off / zero / zero
xPON interrupt enable:    zero
MMIO write attempts:      0
APD steps attempted:      0 / 3
finite samples taken:     0 / 21
adapter retries:          saved=3, during=0, restored=yes
observer retries:         0
```

There was no software rollback because the reset/LOS prefix is intentionally
classified as non-transactional.  No recovery write, retry, `rmmod` or
software reboot was attempted.

## Physical recovery and passive restoration

Immediately after the bounded evidence capture, the XR500v was physically
powered off for at least 35 seconds and the fibre was disconnected.  The next
cold boot of the experimental image proved that power removal restored the
complete external state:

```text
LA_PWD:                 00 24 00 00
SVADC_PD:               00 00 01 00
ADC_PROBE:              00 00 00 00
APD / OVP:              00 08 00 00 / 00 00 00 00
SAFE:                   ff 8f ff 0f
GPIO16 TX_DISABLE:      output-high / asserted
TXEN/rogue/PRBS/test:   off / off / zero / zero
diagnostic writes:      0
```

The audited phase-14 passive image was then uploaded, remotely hashed and
accepted by `sysupgrade -T` before installation:

```text
passive image SHA-256:
0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d

passive image size: 11095474 bytes
```

Warm verification found the passive compatible, no experimental opt-in,
module, status or overlay, and the exact cold optical map.  A second physical
power cut of at least 35 seconds was applied.  The final cold boot again
proved:

```text
compatible:              econet,en751221-xpon-phy-diag
experimental allow:      absent
experimental module:     absent
experimental status:     absent
experimental overlay:    0 files
EN7570 ID:               0x03
complete cold map:       exact
APD / OVP:               00 08 00 00 / 00 00 00 00
GPIO16 TX_DISABLE:       output-high / asserted
TXEN/rogue/PRBS/test:    off / off / zero / zero
xPON interrupt enable:   zero
passive MMIO/I2C writes: 0 / 0
```

The router is therefore back on the known passive image with no retained
phase-26 state.  The fibre may remain disconnected from the XR500v and be used
by the normal Movistar modem.

## Interpretation and next boundary

Phase 26 closes two questions cleanly:

1. the one-LSB RSSI oracle accepts a historically observed live-fibre pair;
2. the post-LOS exact-byte gate still prevents polarity/APD execution when an
   autonomous outcome differs from the historical singleton.

This exact artifact must not be loaded again.  The defensible next experiment
is not a phase-26 retry and not an immediate relaxation of the active APD
sequence.  It is a separately named, terminal LOS-characterisation observer
which would execute the identical fixed 15-write prefix once, stop before all
MMIO/APD writes, and record a finite time series of:

- all four `LOS_CTRL2` bytes;
- `LOS_TIMEOUT_COUNT` and `LOS_TIMEOUT`;
- LOS status/debug fields and the already-established TX barriers;
- elapsed time, with an explicitly planned fibre condition.

Only repeated evidence could justify a later active observer accepting byte-2
bit 0 as autonomous.  If that evidence supports the hypothesis, the narrow
candidate comparison would be `(actual_byte2 & 0xfe) == 0x22`, while keeping
bytes 0, 1 and 3 and every other safety field exact.  That mask is a proposal,
not an authorised or implemented phase.

## Artifact provenance

```text
canonical implementation commit:
d6bc9e904e2b447e0fd33ed62a3b80229457958f

source SHA-256:
a19236d82692b761a815e8dd77891aa5f5349622bdb7ad75ae5d54a01c7089fe

module SHA-256:
4344e04d99b00e42a403cd2075554eeb5fa8c635a621c7560783cf0093f2cc12

experimental image SHA-256:
d45b0f6a07f566becd5b60a4f9e4cc4eed30556f509945bb4efb37196e9db19f

experimental image size: 11095630 bytes
compressed kernel size:   2948057 bytes
safe kernel headroom:      197159 bytes
```

The local-only key-field capture is deliberately reduced rather than a full
SSH or boot transcript; unrelated network and service output was omitted:

```text
phase26_rx_apd_a2_lsb_live_key_capture_20260714_035544.txt
size: 4236 bytes, 124 lines
SHA-256: c288273c41b08693247c4bf31e804c8a2f116710a3fd716df62c8944368bfa9d
```

The immutable write design and image audit remain in
[`2026-07-13-gpon-phase26-rx-apd-a2-lsb-compile-only.md`](2026-07-13-gpon-phase26-rx-apd-a2-lsb-compile-only.md).
The earlier `0x23` residue is documented in
[`2026-07-12-gpon-phase10-en7570-los-nontransactional.md`](2026-07-12-gpon-phase10-en7570-los-nontransactional.md),
and the prior live `0x22` observation in
[`2026-07-13-gpon-phase21-rssi-calibration-gain-los-live.md`](2026-07-13-gpon-phase21-rssi-calibration-gain-los-live.md).
