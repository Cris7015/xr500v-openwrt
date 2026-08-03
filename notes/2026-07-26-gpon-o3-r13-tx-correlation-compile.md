# GPON O3 RX-only r13 — passive MAC/PHY TX correlation

Date: 2026-07-26

Status: **offline implementation, two reproducible clean MIPS builds and two
independent static audits passed. r13 has not been loaded on the XR500v.**

## Why r13 exists

The r11 live run reached local O3 and then stopped on an isolated GPON MAC
debug TX-burst count change `0 -> 1`. That proves a MAC-side event, but does
not establish optical emission. A fresh no-fibre boot subsequently showed the
xPON PHY TX status/frame/burst words all reading zero while GPIO16
`TX_DISABLE` remained asserted.

OEM source established two important limits:

- PHY `0x434` and `0x438` are unlatched shadow counters unless software writes
  the counter-latch trigger;
- the OEM PHY accessor reads each MMIO word twice and retains the second read.

r13 therefore adds another independent MAC-side counter while keeping the PHY
words diagnostic-only.

## New correlation

r13 passively samples:

- GPON MAC `DBG_TX_GEM_CNT` at `0x30c`;
- xPON PHY TX status at `0x40c`;
- raw, unlatched PHY TX frame and burst counters at `0x434` and `0x438`.

The full PHY diagnostic reads each register twice and retains the second
value. Neither a changed nor unchanged PHY raw value can cause an abort,
success, cleanup decision or optical-TX conclusion. The module never reads or
writes the PHY counter latch/clear register.

`DBG_TX_GEM_CNT` is different: missing validity or any delta from its baseline
permanently latches a distinct upstream-activity source, aborts further
progress, requires a physical power cut, makes the exact MAC TX-burst `+1`
cleanup exception ineligible, and prevents returning O3/O4 PLOAM control to
hardware-auto.

## Time-critical boundary

The first audit found a release-blocking omission: the initial r13 draft had
full TX-GEM coverage in ordinary O2/O3 and cleanup guards, but not throughout
the IRQ-disabled O1/two-pop path.

The final source brackets that path with a compact single `0x30c` read at:

- the immediate O1 pre-write guard;
- the post-O1 pre-pop snapshot;
- the exact final boundary before record 0;
- the post-record-0 snapshot;
- the exact boundary before record 1;
- the post-record-1 snapshot.

Only the single MAC read was added inside the bounded IRQ-off section. The six
PHY MMIO reads remain outside it. A delta before O1 disables both FIFO pops; a
delta at any later boundary prevents the next destructive read or subsequent
progress. Specific upstream stop reasons are preserved instead of being
overwritten by a generic FIFO-boundary reason.

## Write-surface audit

Frozen r12 and final r13 have identical normalized write call text:

```text
iowrite32 call sites:       7
regmap_update_bits sites:   2
```

r13 adds no identity, upstream-FIFO, interrupt-enable, interrupt-status,
GPIO/pinctrl, EN7570, laser, APD or PHY counter-latch write.

## Offline validation

Final identities:

```text
source SHA-256:
d66a1af0c0e91e266a1816709216000baa83f24b7e104c532ec2d627d8dc2119

Makefile SHA-256:
7cb21b05fbc4818947ed1ada6a92d53cc845480d8bc7de28ae7a53453e64820c

unstripped module SHA-256:
f8d1bf2e6d884b0aa058fcd2534fdd0863ee364bd06485f96f4a63aac34cbb1a

packaged stripped module SHA-256:
66f99f68ff77af3021d063f33407e0f3c31ab5ab7a8035bac065a7f3f53190a3
```

Two clean builds reproduced both module hashes exactly. The module reports
kernel vermagic `6.12.80 SMP preempt mod_unload MIPS32_R2 32BIT`.
`git diff --check` passes. Strict checkpatch reports zero errors and zero
warnings; its remaining advisory checks are style-only.

The exact source, Makefile and unstripped module are frozen privately under a
mode-`0700` directory with mode-`0600` files.

## Private runner boundary

A separate r13 runner draft is mode `0600`, non-executable and requires
`CONFIRMED_R13_RUNNER_REVIEWED=yes` in addition to the cold-boot, fibre and
35-second physical-off confirmations. Its SHA-256 is:

```text
5343a87d0704ae4de26052a2375b8ab05a5c4245c13526f4ccba3745ddaefc89
```

`bash -n` and ShellCheck pass. Two independent audits checked its frozen
artifact hashes, durable private capture path and terminal oracles. A
controlled unsafe result is accepted only for the exact MAC TX-burst `+1`
shape with sole source mask `0x2`, unchanged TX-GEM across both status output
formats, verified risk-reducing cleanup, retained software PLOAM control and a
mandatory power cut. Any TX-GEM source or printed TX-GEM mismatch fails the
runner. The active runner was not replaced.

## Boundary

r13 is compile/source evidence only. It does not prove optical silence,
emitted light, OLT reception, O4/O5, ONU registration, ranging, GEM/QDMA data,
OMCI or an operational GPON WAN. The module has not been loaded, and no live
fibre test is authorised by this note.

See also:

- [`2026-07-26-gpon-phy-tx-counters-no-fibre-baseline.md`](2026-07-26-gpon-phy-tx-counters-no-fibre-baseline.md)
- [`2026-07-26-gpon-o3-r12-cleanup-hardening.md`](2026-07-26-gpon-o3-r12-cleanup-hardening.md)
- [`2026-07-26-gpon-o3-r11-live-safe-abort.md`](2026-07-26-gpon-o3-r11-live-safe-abort.md)
