# GPON bring-up — O3 RX-only lab offline compile checkpoint

Date: 2026-07-19

Status: **offline engineering checkpoint only: the experimental O3 RX-only
module now builds cleanly and passes a first static review, but it has not been
loaded, has not touched the router, and must not be used live until the final
independent audit and one-shot private runner exist**

## Purpose

This note hands off a narrow reviewable change set for the TP-Link Archer
XR500v / EcoNet EN751221 GPON work. The target remains receive-only: observe
the downstream O2-to-O3 activation conversation while proving that no optical
upstream transmission was possible.

This is not a live GPON result. It does not demonstrate O3 on the device, does
not authorize optical transmission, and does not replace the required cold-boot
runner or final audit.

## Local checkpoint

The reviewed package was committed in the local build tree as:

```text
f656cb8d20 econet: xr500v: add guarded GPON O3 RX lab module
```

Committed files:

```text
package/kernel/xr500v-gpon-o3-rx-lab/Makefile
package/kernel/xr500v-gpon-o3-rx-lab/src/Makefile
package/kernel/xr500v-gpon-o3-rx-lab/src/xr500v-gpon-o3-rx-lab.c
```

The wider build tree and documentation tree remain intentionally dirty with
unrelated in-progress work. Only the O3 package directory was staged.

## Frozen offline artifacts

```text
source SHA-256:
4309b3d9727fea7155237e157032ea6f66c90b39444f5edfc8afe9afb839ad76

built module SHA-256:
555e84d26ed3ba1d2f6519eedf7c3eff27a360b7143ffd37f1af0b0e7876bda8

package SHA-256:
94b785ba4c22d8b1057109f94615d658218533c714aeaa6410495b25580ea1f7
```

Main artifact paths in the local WSL build tree:

```text
source:
/home/cristuu/openwrt/package/kernel/xr500v-gpon-o3-rx-lab/src/xr500v-gpon-o3-rx-lab.c

built module:
/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/xr500v-gpon-o3-rx-lab/xr500v-gpon-o3-rx-lab.ko

package:
/home/cristuu/openwrt/bin/targets/econet/en751221/packages/kmod-xr500v-gpon-o3-rx-lab-6.12.80-r1.apk

build log:
/tmp/o3rx-build.log
```

## What changed relative to the previous non-compiling WIP

The module was mechanically derived from the proven phase-40 observer and had
not passed a build. This checkpoint fixes the integration blockers without
adding identity, upstream FIFO, IRQ-enable, GPIO, EN7570, laser or APD write
paths.

Implemented fixes:

- removed the undefined `GPON_G_PLOU_DELM_BIT` use; extended-burst calculations
  now use the already captured MAC guard field instead of reading an
  unaudited MAC delimiter register;
- added forward declarations for `capture_gpon_probe()` and
  `capture_gpon_checkpoint()`;
- completed all new `enum o3_rx_stop_reason` names in `stop_reason_name()`;
- replaced the stale two-record `O3_RX_PLOAMD_RECORDS` schema with the
  `max_records_used` / `record_count` schema;
- module init now uses the intended `arm_o3_rx_lab` parameter;
- `max_records` and `observe_ms` are validated before hardware resources are
  taken, then copied to `result.max_records_used` and
  `result.observe_ms_used`;
- added a real `observe_start_ns` so O3 elapsed timestamps no longer use the
  absolute deadline as their origin;
- wired the previously unused O3 stages through `o3_rx_run_o3_lab()`:
  1. validate the initial downstream records;
  2. save and program the audited formatter state;
  3. enter local MAC/PHY O3 while transmit remains physically killed;
  4. run a bounded complete-record drain/classification window;
  5. restore every saved formatter word and the original activation word;
- any final FIFO level not divisible by three is rejected as a partial record;
- every individual O3 record pop has a hard time cap, not only the total
  observation window;
- in local O3, MAC TX-burst-counter and upstream-PLOAM status changes are
  recorded as internal MAC evidence instead of automatically aborting, while
  the physical-kill oracle still has to pass on every boundary;
- added a hard allow-list inside `o3_rx_write_checked()` so the helper cannot
  be reused accidentally for a non-formatter register;
- updated the package description, status output, debugfs private-record
  schema, log strings and module description away from the phase-40/two-pop
  wording.

## Validation performed

Build command used:

```sh
cd /home/cristuu/openwrt
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make package/kernel/xr500v-gpon-o3-rx-lab/clean
env PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  make package/kernel/xr500v-gpon-o3-rx-lab/compile \
  CONFIG_PACKAGE_kmod-xr500v-gpon-o3-rx-lab=m V=s -j"$(nproc)"
```

Result: **PASS**. The build log contains no compiler `error:`, no implicit
function declaration and no stale unused-function failure.

Checkpatch command used:

```sh
cd /home/cristuu/openwrt
./scripts/checkpatch.pl --strict --no-tree -f \
  package/kernel/xr500v-gpon-o3-rx-lab/src/xr500v-gpon-o3-rx-lab.c
```

Result: **0 errors, 0 warnings, 21 checks**. The remaining checks are style /
continuation-alignment complaints, mostly inherited from the phase-40-derived
skeleton; none is a functional warning.

Static write-surface review command used:

```sh
cd /home/cristuu/openwrt
grep -nE 'iowrite32|regmap_(write|update_bits)|gpiod_set|i2c_transfer|INT_ENABLE|PLOAMU|VENDOR_ID|VS_SN' \
  package/kernel/xr500v-gpon-o3-rx-lab
```

Review result:

- no `gpiod_set*()` call;
- no `i2c_transfer()` call;
- no write to `GPON_G_INT_ENABLE` or `XPON_INT_ENABLE`;
- no vendor-ID, serial-number, SN-configuration, ONU-ID, response-time or EQD
  write;
- no upstream-PLOAM FIFO data or control write;
- no EN7570, laser, APD, ibias, imod, rogue-ONU, PRBS or optical-test write;
- SCU mutation is limited to `EN751221_SCU_WAN_CONF` / `WAN_MODE`;
- GPON MAC mutation is limited to activation state, the two inherited selective
  W1C interrupt-status operations, and the formatter allow-list;
- xPON mutation is limited to the three formatter words `0x400`, `0x404` and
  `0x408`.

## Safety boundary retained

The module still requires the guarded phase-28 EN7570/xPON receiver handoff and
still treats GPIO16 as the real external `TX_DISABLE` gate. The intended live
boundary remains:

- GPIO16 / GPIO528 must be output-high in every guard sample;
- `PHYSET3.TXEN` must remain clear;
- rogue-TX, PRBS and test-frame enables must remain zero;
- xPON and GPON interrupt enables must remain zero;
- no ONU identity may become valid;
- no upstream FIFO, IRQ-enable, GPIO/pinctrl or EN7570 write is allowed;
- every modified formatter/activation/WAN word must be restored byte-for-byte;
- if exact restoration cannot be proven, the module must remain unsafe-pinned
  and request a physical power cut.

No raw PLOAM words, decoded payload fields, optical identity, serial numbers,
passwords or provider-specific values are included in this note.

## Important review caveat

The source keeps a conditional extended-burst formatter path: after a complete
downstream record classifies as **Extended Burst Length**, it may program MAC
`0x09c` and xPON `0x408` for the extended preamble state. This matches the
local handoff contract for a later O3/O4 formatter step, but it should be
treated as a explicit review decision because the independent
first-implementation audit allow-list is narrower and calls MAC `0x09c` a new
experiment.

Recommended decision before any live run: either formally approve that
conditional `0x09c` path in the final audit, or disable it for the first O3
RX-only run and only classify Extended Burst Length without programming the
extended preamble.

## What this checkpoint does not prove

It does not prove live O3, O5, OMCI, GEM/QDMA, optical upstream ranging, laser
calibration, TX bursts, IPv4/IPv6 provisioning, or an operational optical WAN.
It only proves that the previously non-compiling O3 RX-only experiment now
builds and passes a first offline safety/style review.

## Suggested reviewer checklist

Before a runner is created, please review the committed diff for:

1. the O3 guard split: strict pre-O3 internal checks versus O3-local internal
   event observation;
2. the formatter allow-list and every `iowrite32()` call site;
3. the conditional Extended Burst Length path noted above;
4. all fail-closed exits: partial FIFO, record limit, timeout, guard failure,
   activation loss, identity change and restoration failure;
5. the private-record schema: raw words must remain root-only and must never
   appear in ordinary status or kernel logs;
6. the restoration proof: formatter words, activation word and WAN mode must
   be exact before any PASS is declared.

Only after that review should a one-shot private runner be written, and the
first live attempt still requires a newly observed physical power-off of at
least 35 seconds.
