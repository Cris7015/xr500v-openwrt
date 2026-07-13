# GPON bring-up — phase 22 APD safety audit and passive register map

Date: 2026-07-13

## Result

Phase 21 reproduced reset, transient RSSI calibration, static RSSI gain and
LOS programming, but proved that they were insufficient receiver
prerequisites.  Phase 22 therefore treated APD as a new high-voltage boundary
and performed no APD, ADC, laser, current, TGEN, xPON MMIO or NAND write.

The audit produced three useful results:

1. the OEM APD path is now reduced to two control bits and one DAC byte;
2. the exact per-unit calibration explains both the OEM initial code `0xa2`
   and the later stock code `0xb3`;
3. a live passive dump of every aligned group in the OEM diagnostic range
   found no unknown receiver register missing from the earlier targeted
   inventory.

APD is therefore a credible remaining receiver dependency, but a live APD
write remains **no-go** until its electrical/OVP boundary or a sufficiently
strong stock transition oracle is established.

## OEM APD call graph and isolation

The relevant OEM source is:

```text
/home/cristuu/tools/xr500v/en751221-linux26
```

The full `mt7570_init()` call graph is not usable as an OpenWrt shortcut.  In
addition to receiver setup it enables the PON PHY mode and programs TGEN,
laser bias/modulation currents, MPD targets, Tx-SD and loop control before the
driver releases board-level TX controls.  Those operations remain explicitly
out of scope.

The APD-only portion is separate:

```text
mt7570_APD_initialization()
  read  0x0030, 4 bytes
  set   byte 2 bit 5              soft-start
  write 0x0030, 4 bytes
  read  0x0030, 4 bytes
  set   byte 1 bit 0              APD control enable
  write 0x0030, 4 bytes

mt7570_APD_control()
  calculate one DAC byte
  write 0x0030, 1 byte
```

References are `mt7570.c:1752-1761`, `mt7570.c:1776-1921` and
`mt7570.c:1935-1938`; the register and bits are defined by
`mt7570_reg.h:18` and `mt7570_def.h:28-33`.

Starting from the proven cold baseline, the OEM state sequence is:

| Step | APD bytes at `0x0030` | Meaning |
|---:|---|---|
| cold | `00 08 00 00` | APD control and soft-start clear |
| 1 | `00 08 20 00` | soft-start set |
| 2 | `00 09 20 00` | APD control enabled |
| 3 | `a2 09 20 00` | initial per-unit DAC code |

There is no delay, polling, readback validation or OVP check between those
OEM writes.  The periodic worker later refreshes temperature on its separate
10-second schedule and updates the DAC every `T_APD` (20 seconds in this
unit); deinit does not disable APD.  I2C errors are ignored and the apparent
four-segment `> 0xff` clamp is ineffective because the destination is already
an unsigned byte.  The legacy branch used by this unit has no effective
underflow, overflow or zero-step validation.

`APD_OVP_LATCH` at `0x0164` is only defined in the OEM tree.  Outside the
generic register dump it is never explicitly read or interpreted; it is never
cleared or acted upon, and no local datasheet describes its bits, threshold or
clear semantics.  There is no software rollback claim: the external EN7570
is known to retain state across warm reboot, so recovery after an active APD
write would begin with physical power removal.

## Exact calibration for this XR500v

The generic rootfs `/etc/7570_bob.conf` must not be used.  Stock replaces it
with the factory block before the xPON module consumes it.  The same effective
400-byte block is present at `misc + 0x20000` in three local backups and was
previously verified read-only on the live MTD:

```text
SHA-256:
401dfdaee77c84649bda100fd5dd85be01c7ea126d0a5cc2116b141c1a07a5e4
```

Its APD and temperature words are:

| Offset | Raw value | Interpretation |
|---:|---:|---|
| `0x010` | `0x00000008` | hot slope `0.08 V/degC` |
| `0x014` | `0x0000000e` | cold slope `0.14 V/degC` |
| `0x018` | `0x00001286` | `47.42 V` at `25 degC` |
| `0x01c` | `0x00000014` | update every 20 seconds |
| `0x030` | `0x00000067` | legacy step `0.103 V/code` |
| `0x034` | `0x00000bb8` | legacy code-zero voltage `30.00 V` |
| `0x080` | `0x0c93130d` | temperature slope/offset |
| `0x084` | `0x00960008` | environment/BOSA offsets |

The first OEM call uses its initial `BOSA_temperature = 20 degC`:

```text
V = 47.42 - 0.14 * (25 - 20) = 46.72 V
code = trunc((46.72 - 30.00) / 0.103) = 162 = 0xa2
```

The full stock dump later observed `b3 09 20 00`.  In the per-unit linear
model, code `0xb3` represents approximately `48.437 V`; allowing for integer
truncation, that is consistent with a calculated BOSA temperature of roughly
`37.7-39.0 degC`.  This is a software target, not a measurement of the APD
rail.  Also, code `0x00` would map to about 30 V **if APD control were enabled**;
the cold OpenWrt state keeps APD control disabled.

## Audit of the merbanan proposal

The branch discussed during bring-up was inspected at its current head:

```text
repository: merbanan/openwrt
branch:     econet-eth-mainline
commit:     2e410ea62cfdfcae4979dc1e3f17e55238f9037b
date:       2026-07-03
patch:      target/linux/econet/patches-6.18/741-net-phy-airoha-lddla.patch
```

Its overall LDDLA model is useful, but its EN7570 APD arithmetic must not be
copied for this board:

- it assigns factory `slope1` to the cold/down slope and `slope2` to the
  hot/up slope; the XR500v OEM does the opposite;
- in legacy mode it reads the step from flash offset `0x038` or uses the
  `0.09375 V/code` default, while this unit stores the real step at `0x030`
  and the zero voltage at `0x034`.

With the same initial 20 degC state, that implementation would calculate
approximately code `0xb5`, not the OEM's `0xa2`.  At an operating temperature
near the stock observation it can reach roughly `0xcd`, well above the known
stock `0xb3`.  This is a board-specific safety bug, not just a rounding
difference.  The branch describes itself as compile-tested and should remain
a structural reference until these calibration semantics are corrected.

## Passive full-map observer

The OEM diagnostic `mt7570_register_dump(length)` performs only a four-byte
read at `(index << 2)` for every index.  Stock used length `0xc1`, covering 193
aligned groups from `0x000` through `0x300`.

Release 7 of `xr500v-en7570-diag` now mirrors that range in a separate,
read-only debugfs file:

```text
/sys/kernel/debug/xr500v-en7570/registers
```

The file mode is `0444`.  Its only bus helper is the existing two-message
`en7570_read()`: a 16-bit register-pointer message followed by an `I2C_M_RD`
message.  There is no register-data payload, `.write` handler, calibration,
reset or worker.  Without an EN7570 datasheet it is impossible to prove that
every undocumented status bit lacks read-to-clear semantics; the OEM itself
uses the same complete dump, and no such side effect appears in its source.

Build audit:

```text
package: kmod-xr500v-en7570-diag-6.12.80-r7.apk
APK SHA-256:
a1b71c9328966395fe8ad5387a797d0c661967988e97c0d928dc1a41835a43a0

loaded unstripped module SHA-256:
e2452d235bd54f394f175c1454ab471beb889317bc1ef06f584481550880f9fb
loaded file size: 335072 bytes
runtime core size: 16384 bytes
vermagic: 6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT
```

The undefined-symbol audit contains `i2c_transfer` but no SMBus or I2C
register-write helper.  The module compiled without a source warning.  No
firmware image was rebuilt or flashed in this phase, so the 3 MiB compressed
kernel limit, 512-byte rootfs gap and TrendChip header were not involved.

## Live passive capture and stock comparison

The new module was copied over SSH stdin, hash-verified, and temporarily
replaced the existing passive diagnostic.  It is loaded from `/tmp`; the
installed release remains unchanged and will return after reboot.

Several complete traversals were consumed while displaying, counting,
versioning and comparing the map.  Every traversal returned 193/193 groups
with zero read errors.  Postflight APD, OVP, all TX gates and both diagnostic
write counters remained at their exact cold values.

The exact raw-I2C-order inputs and comparator result are versioned beside this
note:

| Artifact | SHA-256 |
|---|---|
| [`OpenWrt passive map`](2026-07-13-gpon-phase22-en7570-openwrt-passive-dump.txt) | `2899c200f8afff7e63e348c34931c244cdf50459c4564668e0ee411d99f13d50` |
| [`functional stock map`](2026-07-13-gpon-phase22-en7570-stock-functional-dump.txt) | `b1b5aa2a5b0a01bf5bcf996f99352f7a952ece05afe4c1accaba3b9183203f02` |
| [`comparison result`](2026-07-13-gpon-phase22-en7570-stock-diff.txt) | `118a9e4af4198439c5bf2716a405a31b08dd93213bf8e75b088969ae55efb05d` |

Against the same-device functional stock dump:

```text
same:       176 / 193 groups
different:  17 / 193 groups
unknown:      0
```

Every difference belongs to a previously identified block:

| Register | OpenWrt passive | Functional stock | Classification |
|---:|---|---|---|
| `0x004` | `00 02 00 00` | `00 01 20 01` | MPD target / TX |
| `0x008` | `99 00 00 20` | `9a 5f 5c 08` | TGEN timing/counters / TX |
| `0x00c` | `40 00 00 00` | `33 00 00 00` | Tx-SD / TX |
| `0x010` | `00 00 00 00` | `5c 5f 00 00` | sampled T0C/T1C |
| `0x014` | `00 24 00 00` | `00 24 05 00` | RSSI gain, already tested |
| `0x024` | `00 00 01 00` | `00 00 41 04` | LOS ADC REV1/REV2 controls, already tested |
| `0x030` | `00 08 00 00` | `b3 09 20 00` | APD receiver bias |
| `0x100` | `ff 8f ff 0f` | `ff 0f ff 0f` | safe/Tx-fault state |
| `0x11c` | `06 08 3c 36` | `06 1f 1c 10` | LOS setup, already tested |
| `0x120` | `10 05 00 00` | `05 1f 22 00` | LOS setup, already tested |
| `0x130` | dynamic | `2e 18 65 88` | LOS/debug status |
| `0x138` | `00 00 00 00` | `37 02 00 00` | P0 bias/current / TX |
| `0x13c` | `30 12 00 10` | `35 12 4b 82` | P0 loop/MPD / TX |
| `0x148` | `00 00 00 00` | `eb 04 00 00` | P1 modulation/current / TX |
| `0x14c` | `30 12 00 10` | `35 12 fc 84` | P1 loop/MPD / TX |
| `0x154` | `00 00 00 00` | `79 01 00 00` | last ADC sample |
| `0x16c` | `3f 2f 0f 00` | `ff a7 58 00` | ERC / TX monitor-loop |

Phase 21 already reproduced the transient RSSI oracle and programmed the
stock RSSI-gain and LOS groups without observing fibre reception.  The other
unmatched control groups are TX, sampled state, or software-only temperature
inputs for APD computation.  The full map therefore found no additional
stable RX configuration which can reasonably precede APD.

## Final state and decision

The router was left on the normal passive OpenWrt DT with live fibre connected
and the release-7 observer loaded only from `/tmp`:

```text
APD_DAC:                 00 08 00 00
APD_OVP_LATCH:           00 00 00 00
SAFE_PROTECT[0:1]:       ff 8f
Ibias / Imod:            00 00 / 00 00
TX_DISABLE:              output-high / asserted
xPON TXEN:               clear
rogue / PRBS / test:     disabled
xPON interrupt enable:   0x00
xPON MMIO writes:        0
EN7570 register writes:  0
```

The good result is that APD can be modeled as a very small, receiver-only
operation.  A future compile-only experiment could hard-code the factory
initial `0xa2` and expose no arbitrary DAC parameter, worker, DDMI, laser or
TX path.  The software-minimized equivalent would target exactly three bytes:

```text
0x0032: 00 -> 20
0x0031: 08 -> 09
0x0030: 00 -> a2
```

That design is not implemented or deployed by this phase.  Before a live
write, the preferred evidence is a stock cold-boot trace of `0x0030/0x0164`
over several 20-second cycles and, ideally, identification/measurement of the
board APD rail and its OVP limit.  A first live phase must use exact baseline,
calibration-hash and TX gates, read back APD/OVP after every byte, have no
software rollback claim, and begin with fibre disconnected and immediate
physical power-cut capability.
