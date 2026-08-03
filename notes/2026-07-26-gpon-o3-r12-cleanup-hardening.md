# GPON O3 RX-only r12 — latched cleanup hardening

Date: 2026-07-26

Status: **offline implementation, clean MIPS build and independent static
audit passed. r12 has not been loaded on the XR500v.**

## Why r12 exists

The one-shot r11 live run reached local O3, observed `SN_Request` and retained
six complete downstream records. It then aborted immediately when the GPON MAC
debug TX-burst counter changed `0 -> 1`.

No TX/SN-send interrupt, upstream-FIFO delta or upstream write accompanied
that change. OEM source distinguishes this MAC counter from the PHY TX
frame/burst counters, so the strongest current interpretation is that the MAC
scheduled or accounted the random-delay SN slot. This is not proof that the
BOSA emitted optical power.

r11 materially restored O1 and all six formatter words, but its cleanup guard
continued comparing the sticky MAC count against the original zero baseline.
It therefore reported restoration failure and correctly retained software
PLOAM control until the subsequent physical power cut.

## OEM safety correction

GPIO16 `TX_DISABLE` is the confirmed physical transmitter kill. The previous
description of `PHYSET3` bit 5 as `TXEN` was incorrect: OEM code clears this bit
when selecting normal GPON burst mode. r12 retains the exact bit-5 value as a
mode/configuration invariant, not as an independent physical inhibit.

## r12 behavior

r12 does not permit the observation to continue after any upstream indication.
It introduces a permanent per-run latch for:

- any TX-related GPON interrupt;
- any change in the MAC debug TX-burst counter;
- any change in the upstream PLOAM FIFO status.

Every such event still aborts immediately, marks the module unsafe, requires a
physical power cut and forbids returning `O3_O4_PLOAMU_CTRL` to hardware-auto.
The latch cannot be cleared by a later quiet sample.

Only the exact already-observed `baseline + 1` MAC-counter case may enable
risk-reducing cleanup, and only when the same snapshot also proves:

- GPIO16 remains output-high;
- the phase-28 xPON owner and complete physical/configuration guard pass;
- the MAC is still in O3;
- GPON interrupt enable is zero and no TX interrupt is present;
- the upstream FIFO is unchanged;
- software PLOAM control remains forced;
- identity/global configuration is unchanged;
- no unsafe or downstream-overrun status is present.

That exception permits only O3-to-O1 and exact formatter restoration. Every
pre-write guard is authoritative: a GPIO, xPON, mux, state, identity, control,
IRQ, FIFO or second counter change blocks the write. The exception never
permits continued observation, a passing result or restoration of
hardware-auto mode.

`G_SN_MSG_CFG` remains read-only. A random-delay residual still pins the module
and requires a physical power cut.

## Write-surface audit

r11 and r12 contain the same write call sites:

```text
iowrite32 call sites:       7
regmap_update_bits sites:   2
```

The normalized call text is identical. r12 adds guards, latches and status
reporting; it does not add a GPIO, pinctrl, EN7570, PHY/laser/APD, identity,
upstream-FIFO, interrupt-enable or interrupt-status write.

## Offline validation

Final identities:

```text
source SHA-256:
897ff8b6fcdd5bc4b02359e0b4ccb44f019082d7ba35aeda9ec7640cc6e2bb94

Makefile SHA-256:
8cf5180d0add2bd4d618e8c3339d57650730030338d65fb504e24a8335d61e4a

unstripped module SHA-256:
fa0e1466c64eb001c7972c3c70131c560cb51c53e472a7258bc246ad32123b8b

packaged stripped module SHA-256:
5134edabc9d57df112378a4e1b983b9eb40d355035de4edccc56798f561aa019
```

Two clean builds reproduced both module hashes exactly. The module reports
kernel vermagic `6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT` and retains
the two explicit opt-ins `arm_o3_rx_lab` and
`force_software_ploamu_control`.

`git diff --check` passes. Strict checkpatch reports 0 errors and 0 warnings;
its 28 advisory formatting checks are inherited style issues and do not alter
the audited behavior.

An independent audit found and closed one real pre-build issue: the
post-guard diagnostic could observe a TX event after an earlier guard failure
without latching it. The final source now converts all three diagnostic
observations into the same permanent upstream-activity latch before cleanup.
The final logic audit passed after that correction; the only subsequent source
change was a comment changing “physical kills” to the accurate singular
`TX_DISABLE` wording.

## Private runner boundary

The exact source and unstripped module above are frozen in a root-private
staging set. A separate r12 runner draft is mode `0600`, non-executable and
requires an additional `CONFIRMED_R12_RUNNER_REVIEWED=yes` latch. Its SHA-256
is:

```text
5f2d468813a36c5b3cd19d36c450307fd4f703a96dfaab980a4a99a9c733ddb5
```

`bash -n` and ShellCheck pass. Its unsafe-pinned oracle accepts only the exact
`0 -> 1` counter shape, the TX-burst-only latch source, stable cleanup count,
exact O1/formatter restoration, retained software PLOAM control and mandatory
power cut. It deliberately skips private-record retrieval on that path. The
active r11 runner was not replaced.

## Boundary

r12 is compile-only evidence. It does not prove optical silence, O4/O5, ONU
registration, ranging, GEM/QDMA traffic, OMCI or an operational GPON WAN.
The frozen runner remains a draft and has not been executed. Any later run
still requires a newly observed physical power-off of at least 35 seconds.
