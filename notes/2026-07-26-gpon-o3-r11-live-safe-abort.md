# GPON O3 RX-only r11 — first local O3 and safe TX-counter abort

Date: 2026-07-26

Status: **live-fibre RX-only experiment completed once. The MAC reached local
O3 and observed `SN_Request`, then aborted fail-closed on the first internal
TX-burst-counter increment. A physical power cut was completed.**

## Preconditions

The XR500v was physically unpowered for at least 35 seconds, booted with the
authorised fibre connected and left untouched until the one-shot preflight
passed:

- OpenWrt kernel `6.12.80`;
- watchdog offline;
- no non-diagnostic XR500v experimental module loaded;
- exact frozen phase-28 source/module hashes;
- exact frozen O3 r11 source/module hashes;
- at least 240 seconds uptime and two quiet CPU windows;
- GPIO16 `TX_DISABLE` asserted before the transaction.

The r11 identities were:

```text
source SHA-256:
c766a043254f04cc07b353d44084889102661224a56699c06f5111db939b5b74

built module SHA-256:
7bab8b95a6758c9606852340d6c7eb2b528eff3cc26d8e4037e76722238befd9
```

The run explicitly selected `force_software_ploamu_control=1`.

## Sequence reached

The complete preflight and phase-28 receiver handoff passed. The guarded O3
module then:

1. selected the GPON WAN aperture;
2. proved the exact O1/reset/TX-disabled baseline;
3. read `O3_O4_PLOAMU_CTRL` as `0x00000000`;
4. changed only bit 0 and read back `0x00000001`;
5. entered O2 and observed the first downstream GTC frames;
6. accepted the previously characterized nine-word downstream FIFO trigger;
7. restored O1 before two bounded downstream-record pops;
8. verified the exact `9 -> 6 -> 3` FIFO accounting and two identical
   Upstream Overhead records;
9. programmed only the audited formatter words;
10. entered local GPON MAC O3 with the phase-28 PHY still RX-ready, GPIO16
    `TX_DISABLE` asserted and every audited software TX generator disabled.

During 302 guarded O3 polls, six complete downstream records were retained and
classified:

```text
Upstream Overhead:       3
Extended Burst Length:   3
other:                   0
SN_Request observed:     yes
```

No raw PLOAM word or provider identity is included in this note.

## Fail-closed event

The worker stopped immediately when the GPON MAC debug TX-burst counter changed
from 0 to 1:

```text
reason: tx-burst-changed
DBG_TX_BST_CNT: 0 -> 1
```

At that boundary:

- no `PLOAMU_SEND` or `SN_ONU_SEND_O3/O4` interrupt was observed;
- the upstream PLOAM FIFO status remained unchanged;
- no upstream FIFO write occurred;
- no identity, serial-number or ONU-ID write occurred;
- the ONU ID never became valid;
- GPIO16 `TX_DISABLE` remained asserted;
- `PHYSET3` bit 5 remained clear, which OEM source identifies as normal GPON
  burst mode rather than an independent physical TX inhibit;
- no GPIO, pinctrl, EN7570, PHY, laser or APD write occurred;
- Extended Burst Length remained classification-only.

`DBG_TX_BST_CNT` is a MAC-side counter. Its increment proves that the MAC
scheduled or accounted one internal burst opportunity; it does **not** prove
that optical power left the BOSA. The accompanying `SN_Request`, random-delay
change, absent TX IRQs and unchanged upstream FIFO make a scheduled SN slot the
strongest current interpretation. GPIO16 is the confirmed physical kill;
`PHYSET3` bit 5 is only a burst/continuous-mode selector. Conversely, neither
the digital nor GPIO guards are an optical power measurement, so this run does
not claim that emitted power was physically zero.

## Restoration and power cut

The O3 activation word returned to O1, all six formatter words read back equal
to their saved baseline, the WAN mux returned exactly to ATM and the complete
phase-28 physical guard still passed.

The first TX-counter delta is sticky relative to the original baseline. The
r11 cleanup guards therefore reported failure even though the formatter
readbacks matched. The dedicated `0x3c4` restore pre-guard also rejected the
sticky counter, correctly skipped restoring hardware-auto mode and left bit 0
set. The MAC additionally retained only the random-delay field of
`SN_MSG_CFG`; the lab deliberately has no write path for that register.

The module consequently self-pinned, retained its resources and required
physical power removal. The router became unreachable and remained offline for
a separately observed 36-second interval. No `rmmod`, retry or software reboot
was attempted.

## Private provenance

The ordinary status capture and its backup are byte-identical:

```text
SHA-256: d1d8c11c4ebdb485367e92be03bdcb8bda2a015f8de37d65a75f54f2797dabef
mode: 0600
```

The private-record capture and its backup are also byte-identical:

```text
SHA-256: 78fde9e51f5235533e358835cfd4a9a76b09029566399a5e876ddb04f3f997cc
mode: 0600
```

The private-record file is labelled invalid only because the overall
transaction aborted and exact restoration was not proved; its six records
were structurally complete. Raw contents remain private.

## r12 cleanup revision

r11 must not be rerun. The offline r12 cleanup change now:

- preserve the immediate abort on the first TX-burst-counter delta;
- latch any upstream activity permanently for the remainder of that boot;
- accept an exact, stable `baseline + 1` value only for risk-reducing O1 and
  formatter restoration, and only when the same snapshot has no TX interrupt,
  upstream-FIFO change, identity change or physical-guard failure;
- require that latched value to remain identical around every permitted
  cleanup write;
- never restore `0x3c4` to hardware-auto after any activity latch; retain
  software control until physical power removal;
- remain unsafe-pinned and require a power cut if the `SN_MSG_CFG`
  random-delay residual remains;
- never continue the O3 observation after the counter changes.

A separate later read-only experiment may compare the MAC TX-GEM counter and
the PHY TX-status/frame/burst counters. It must not use the OEM counter-latch
write, treat a quiet raw PHY counter as conclusive, or weaken the physical
transmit guards.

r12 has passed clean reproducible MIPS builds and an independent static safety
audit, but it has not been loaded on the router. See
[`notes/2026-07-26-gpon-o3-r12-cleanup-hardening.md`](../notes/2026-07-26-gpon-o3-r12-cleanup-hardening.md).

## What this proves

OpenWrt now has live evidence of the guarded downstream path through local MAC
O3 and `SN_Request` reception. It still does not prove O4/O5, ONU registration,
upstream optical transmission, ranging, GEM/QDMA traffic, OMCI or an operational
GPON WAN.
