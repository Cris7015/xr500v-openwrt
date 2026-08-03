# XR500v U-Boot RAM Ethernet E4 hardware pass

Date: 2026-07-29

## Outcome

The EN751221 Ethernet/QDMA path in the XR500v RAM-only U-Boot port is now
validated on real hardware from a genuine cold Bootbase/BLDR start.

E4 completed 16/16 consecutive pings to the host.  The test consumed and
reclaimed more TX completions than both the four-entry hardware-forwarding
pool and the 32-entry legacy completion queue, including a clean queue-head
wrap.  LMGR remained at `free=4`, `usage=0`, and the HWFWD empty/low status
bits did not return.

This validates the cold EN751221 FE/QDMA/Ethernet integration used by the
chainloaded U-Boot target.  It does **not** mean GPON O3, OMCI, or the optical
TX path is solved.

## E3 diagnosis

The first cold image could complete one ARP/ICMP exchange, then later sends
stalled even though the software TX descriptor reached `DONE`.

The EN751221 branch enabled TX writeback and used the OEM-sized four-entry
HWFWD pool, but did not allocate or drain the legacy TX completion IRQ queue.
Each CPU transmission therefore retained one HWFWD context until all four
were exhausted.  The common Airoha path and the OEM Bootrom both configure
and advance this queue.

Cold state also confirmed that Bootbase left the integrated P5/P6 and
external MCM P6 switch controls in the dumb-switch state
`PCR=0x00ff0000`, `PVC=0x810000c0`; the earlier warm Linux/DSA state was not
required for the successful round trip.

## E4 fix

The driver now:

- allocates the OEM-sized 32-entry legacy completion queue;
- programs QDMA `IRQ_BASE`, `IRQ_CFG`, `IRQ_CLEAR_LEN`, and `IRQ_STATUS`
  at offsets `0x60` through `0x6c`;
- enables the legacy IRQ queue in global configuration;
- waits for one completion entry after synchronous TX writeback completion;
- advances `IRQ_CLEAR_LEN` once per completed transmission.

The HWFWD pool remains at the OEM value of four.  No ring-lifecycle,
LMGR-retire, tag, NAND, SPI, MTD, or persistent-environment behavior was
added.

## Hardware evidence

- Image loaded by XMODEM at `0xa1000000`, then entered at `0x81000000`.
- Volatile U-Boot MAC: `02:58:50:00:00:01`.
- Volatile U-Boot/host addresses: `192.168.68.221` / `192.168.68.248`.
- Before traffic: IRQ base `0x0fdbffa0`, IRQ config `0x00040020`,
  LMGR `free=4`, `usage=0`.
- Sixteen consecutive pings returned `host 192.168.68.248 is alive`.
- Completion accounting reached 35 TX completions.
- The 32-entry completion head wrapped from `0x17` to `0x03`.
- Final LMGR remained `free=4`, `usage=0`.
- Final QDMA indices were quiescent at `2/2/2/3`.
- Final interrupt status was `0x0000000e`, with no HWFWD
  empty/low bits (`8`/`10`).

The WSL packet capture is not treated as authoritative because WSL was using
Windows mirrored networking.  The UART/QDMA counters and successful U-Boot
ping results are the primary evidence.

## Repositories and artifacts

U-Boot worktree:

`/home/cristuu/tools/xr500v/u-boot-en751221`

E4 XMODEM image:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/build/xr500v-u-boot-ram-eth-e4-irq-reclaim-20260729.xmodem.bin`

- size: `239360` bytes (`0x3a700`)
- SHA-256:
  `a70f525ededa62d3bb50bc72070e38ea55192b01a9bf074dd1fb5dc72c659ff5`

E4 ELF:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/build/xr500v-u-boot-ram-eth-e4-irq-reclaim-20260729.elf`

- size: `3167576` bytes
- SHA-256:
  `39328e9de4c9217b55b8392a3052d467ae246a41935ab4cfa29177447a4daf49`

Stable E4 UART capture:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/build/xr500v-ram-eth-e4-irq-reclaim-20260729.log`

- size: `6007` bytes
- SHA-256:
  `9d254110bff602e35e1b4569615ff4bb6dee3bc68a0e68ed999ea44de56e8cd5`

Detailed test record:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/ETHERNET-RAM-TEST.md`

Validation checklist:

`/home/cristuu/tools/xr500v/u-boot-en751221/contrib/xr500v/HARDWARE-VALIDATION.md`

## Verification

- `git diff --check`: pass.
- `scripts/checkpatch.pl --no-tree -`: 0 errors, 0 warnings.
- XR500v RAM-only build: pass.
- Reference `en7512_reference_defconfig` build, including SPL/binman: pass.
- RAM-only wrapper rejects persistent/network-autoboot features and passed.

## Safety and current boundary

All testing was RAM-only and performed without fibre.  Nothing was written
to NAND and no persistent environment was saved.  A physical power cycle
discards the chainloaded U-Boot instance and returns to the normal Bootbase
selection/OpenWrt path.

The next work item is not another basic Ethernet ping experiment.  The
Ethernet RAM target is hardware-validated.  The remaining choices are to
prepare a clean, reviewable commit/series for the U-Boot port and optionally
harden stale-completion/timeout recovery before discussing upstream
integration.  GPON activation remains a separate workstream.

## Review series prepared

The local U-Boot worktree was cleaned and committed on
`xr500v-ram-chainload`, based on Matheus's
`aba03cf8d8a37b72b5d1aef52f99b060aa7139a3`.

Series tip: `31b0d565`

1. `2e5c4ae3 net: airoha: set EN751221 cascade and CPU-port IPG modes`
2. `b62706d9 net: airoha: reclaim EN751221 synchronous TX completions`
3. `ef34dfed mips: en75xx: add XR500v RAM-only chainload targets`
4. `f13b1063 contrib: xr500v: add RAM-chainload validation helpers`
5. `31b0d565 doc: econet: document XR500v RAM-chainload validation`

Review bundle:

`/home/cristuu/tools/xr500v/u-boot-en751221-review-20260729-31b0d565`

Public repository:

`https://github.com/Cris7015/xr500v-u-boot`

The committed tree passes console-only, RAM-Ethernet, and EN7512 reference
builds.  The generated post-commit transfers are:

- console-only: `0x32800`, SHA-256
  `99dfb48fe861ef06b40a80fa16d2e39647300c016a63794d8c3fdbdedf899654`;
- Ethernet: `0x3a700`, SHA-256
  `e0ab9e97604311c4c816029ed124cfc45f7f73773cb626d308cb9d72873ed4af`.

These hashes differ from the exact hardware-tested E4 artifact because the
embedded U-Boot Git version changed when the clean commits were created.  The
functional driver source is unchanged.

The five commits are authored and signed off as Cristian Papa using the
GitHub-generated no-reply address associated with `Cris7015`.  The branch was
pushed to the separate `github` remote as public `main`; `origin` still points
unchanged to Matheus's server.

The series is suitable for lab review but is not a direct U-Boot-upstream
submission: its base contains experimental TPL/SPL/SPI work and a vendor DDR
blob which these RAM targets do not execute.  Stale-completion/timeout recovery
also remains a separate follow-up before calling the EN751221 TX path generally
robust.

## Public-tip manual reproduction

The post-commit Ethernet artifact was subsequently chainloaded manually on the
XR500v from a genuine cold Bootbase prompt:

- source commit and reported U-Boot version: `31b0d5656830`;
- XMODEM file size: `0x3a700`;
- XMODEM SHA-256:
  `e0ab9e97604311c4c816029ed124cfc45f7f73773cb626d308cb9d72873ed4af`;
- Bootbase result: `received len=3a700`;
- words at `0xa1000000`/`0xa1000004`:
  `0x1000013f`/`0x00000000`;
- model: `TP-Link Archer XR500v v1 (RAM-only Ethernet chainload)`;
- Ethernet device: `airoha-gdm1`.

With RAM-only address `192.168.68.221`, server `192.168.68.248`, and local
MAC `02:58:50:00:00:01`, an initial ping and a 16-command stress loop all
completed with `host 192.168.68.248 is alive`.

The final read-only register snapshot was:

```text
IRQ base/cfg/clear/status:
  0x0fdbffa0 / 0x00040020 / 0x00000000 / 0x00000007
LMGR free/usage:
  4 / 0
QDMA INT_STATUS:
  0x0000000e
QDMA TX_CPU/TX_DMA/RX_CPU/RX_DMA:
  2 / 2 / 2 / 3
```

`IRQ_STATUS` had zero pending entries and head index 7 after crossing the
32-entry queue boundary.  LMGR had returned all four HWFWD contexts,
`INT_STATUS` had neither HWFWD-empty bit 8 nor HWFWD-low bit 10 set, TX
CPU/DMA indices matched, and RX retained its expected one-descriptor sentinel.
This confirms the exact public-tip build, not only the earlier E4 staging
artifact, on EN751221 hardware.  The test remained RAM-only and did not touch
NAND, `bflag`, GPON, or persistent environment.
