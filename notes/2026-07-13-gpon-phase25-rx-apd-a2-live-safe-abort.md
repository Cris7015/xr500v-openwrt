# GPON bring-up — phase 25 live RX/APD A2 safe abort

Date: 2026-07-13

## Result

The guarded phase-25 observer was loaded exactly once with the authorised
Movistar fibre connected.  It stopped at the first RSSI ADC oracle check,
after five successful fixed I2C writes, because Vref was `0x020b` rather than
the deliberately exact `0x020a` accepted by this historical artifact.

```text
i2c_write_attempts:  5 / 18 maximum
mmio_write_attempts: 0 / 1 maximum
sequence_result:     -34 (-ERANGE)
halted_step:         5
RSSI Vref:           0x020b
```

The stop was early and fail-closed.  There was no RSSI V sample, gain or LOS
programming, xPON polarity write, APD write, receive sample series, ESD write,
current setup or TX-current/laser/TGEN write.  GPIO16 physical `TX_DISABLE`
stayed output-high/asserted, xPON TXEN stayed clear and all observed
TX/test/interrupt gates remained inactive.

This run did **not** answer whether the combined receiver/APD sequence can see
the fibre.  It proved that the immutable oracle stopped the non-transactional
sequence at the first unexpected ADC result; the terminal-capture path then
preserved the post-abort evidence.

## Preflight and invocation

The temporary phase-25 image was installed while the fibre was disconnected.
Its remote size and SHA-256 matched the audited local artifact, and
`sysupgrade -T` returned zero.  After the warm image verification, the router
was physically powered off for 35 seconds and booted without fibre.

The cold preflight required and observed:

```text
compatible:             econet,en751221-en7570-rx-apd-a2-experimental
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
set to the unique observer name.  The module was then copied to `/tmp`; its
remote size and SHA-256 were exact, and it remained unloaded until the fibre
was connected.

The single historical invocation was:

```text
insmod /tmp/phase25-rx-apd-a2.ko arm_en7570_rx_apd_a2=1
```

`insmod` returned zero because the observer intentionally keeps its pinned
platform instance alive after a claimed non-transactional sequence, so that
debugfs can expose the terminal evidence.  The authoritative operation result
is `sequence_result=-34`, not the loader return code.  The command was not
repeated and the module was not removed.

## Exact execution boundary

The reset write succeeded, self-cleared, and the complete post-reset snapshot
again matched all 29 cold EN7570 groups plus every GPIO/xPON guard.  The four
RSSI-prefix writes which followed also succeeded:

| Attempt | Register | Fixed payload | Result |
|---:|---:|---|---:|
| 1 | `0x0300` | `01 00 00 00` | 0 |
| 2 | `0x0014` | `00 34` | 0 |
| 3 | `0x0014` | `00 74` | 0 |
| 4 | `0x0024` | `02` | 0 |
| 5 | `0x0159` | `10` | 0 |

The directed ADC read after the successful latch transfer returned
little-endian bytes `0b 02`, or `0x020b`.  The exact phase-25 check rejected it
immediately.  No write from attempt 6 through 18 was issued.  Because the
sequence stopped there, the later directed latch read was not reached and its
zero-filled status field is not evidence of self-clear.

This means the terminal analogue controls correctly remained at their
in-progress calibration state until physical power removal:

```text
LA_PWD:     00 74 00 00
SVADC_PD:   02 00 01 00
ADC_PROBE:  0b 02 00 00
```

That state is expected after this precise prefix and is not a rollback
failure: rollback was forbidden by design.  The status rows for later stages
retain `-ECANCELED` sentinels and zero-filled storage because those stages were
never captured; they are not hardware observations.

## Terminal safety evidence

The best-effort terminal map itself captured successfully.  It showed:

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

The kernel result was:

```text
EN7570 RX/APD A2 result -34 after 5 I2C and 0 MMIO write attempt(s),
0 sample(s); TX_DISABLE retained; physical power removal required
```

Thus the fibre-connected run never crossed the LOS, polarity, APD or finite
post-APD RX-sampling boundaries.  It issued no TX-current, laser or TGEN
write, and all observed transmitter barriers stayed closed.

## Physical recovery and passive restoration

Immediately after status and the kernel result were captured, the XR500v was
physically powered off for 35 seconds.  There was no `rmmod`, software reboot,
retry or recovery write.  The fibre was returned to the normal Movistar modem.

The next fibre-disconnected cold boot of the experimental image proved that
power removal restored the complete external state:

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

The warm verification found the passive compatible, no experimental opt-in,
module, status or overlay file, and the exact cold optical map.  A second
35-second physical power cut was applied.  The final cold boot again proved:

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
phase-25 state.

## Interpretation and next boundary

Earlier evidence comprised eleven stock boots with Vref `0x020a` and the
successful phase-21 live-fibre OpenWrt run with Vref `0x020a`, V `0x0285` and
delta `0x007b`.  Phase 25 adds one successful directed ADC read at `0x020b`,
exactly one count higher.  One sample cannot attribute that difference to
optical power, temperature or any particular cause.  Phase 21 previously
returned `0x020a` with fibre connected, so the phase-25 value cannot be
attributed to fibre connection alone.

The scientifically supported conclusion is narrower: an exact singleton
Vref oracle is not stable across every run of this unit.  The safety gate did
what it was designed to do, but phase 25's receiver question remains open.

This exact artifact must not be loaded again.  If a new phase is justified,
one deliberately narrow hypothesis for a separately named and audited
observer is a common-mode one-count extension which retains the historical
delta:

```text
Vref:        0x020a or 0x020b
V:           0x0284, 0x0285 or 0x0286
V - Vref:    0x007a or 0x007b
```

Together these constraints admit the historically observed pairs
`(0x020a, 0x0284)` and `(0x020a, 0x0285)`, plus the prospective
one-count-shifted pairs `(0x020b, 0x0285)` and `(0x020b, 0x0286)`.  Phase 25
did not sample V, and `0x0286` has not been observed; it is included only as
the mathematical counterpart of retaining delta `0x007b` when Vref is
`0x020b`.  This is therefore a testable proposed oracle, not a phase-25
measurement or conclusion.  The delta remains the primary
same-calibration-pair constraint.

This candidate changes no write, timing, APD code, TX gate or recovery rule.
It still requires a fresh source hash, independent safety review, rebuild,
experimental image audit and cold preflight; it is not authorised by this note
alone.

## Artifact provenance

```text
canonical implementation commit:
c749e831e897b509e87cee1ca10a364e7ef55e49

source SHA-256:
fe9def9e08e549fc8c27b2a20c47256ae99c62933fb4d7d2446cbca4cbf413ee

module SHA-256:
719abab06b4f4f452606ade119ec7f2e53f4b70b0f1f378a8a4fcf65ec0b0c36

experimental image SHA-256:
8c04673900bdb3662b3033089bf0fdd44f528aaea99812d186823b03b8d2c24c
```

The local-only key-field capture is intentionally not a full boot transcript;
unrelated network identifiers and boot noise were omitted when it was
preserved:

```text
phase25_rx_apd_a2_live_key_capture_20260713_095202.txt
size: 4407 bytes, 109 lines
SHA-256: 18341ecad4824e3531bd960a884578cb788750db599ecf37aae088efcb96eaf9
```

The compile/image audit and immutable 18-write design remain in
[`2026-07-13-gpon-phase25-rx-apd-a2-compile-only.md`](2026-07-13-gpon-phase25-rx-apd-a2-compile-only.md).
