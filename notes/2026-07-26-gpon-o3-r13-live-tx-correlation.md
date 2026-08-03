# GPON O3 RX-only r13 — live TX-correlation result

Date: 2026-07-26

Status: **r13 reached local O3 and reproduced the downstream record set, then
failed closed on a combined MAC TX-burst-count and PLOAMu-FIFO-underrun
indication. O1 and the ATM WAN mux were restored, but the formatter and forced
software-PLOAM control remained pinned; a physical power cut followed.**

This is not evidence of optical transmission, optical silence, OLT acceptance
or a usable ONU data path.

## Cold-boot boundary

The first r13 live attempt had stopped in the phase-28 RSSI oracle and was
followed by a physical cold boot. An invocation made too early during that
boot failed in read-only SSH preflight with `No route to host`; it recorded
`mutation_may_have_started=0` and did not require another power cycle.

The same audited runner was then invoked after the router became reachable on
the new boot. It waited until uptime 240 seconds, phase 28 passed, and the
frozen r13 module loaded:

```text
r13 source:
d66a1af0c0e91e266a1816709216000baa83f24b7e104c532ec2d627d8dc2119

r13 unstripped module:
f8d1bf2e6d884b0aa058fcd2534fdd0863ee364bd06485f96f4a63aac34cbb1a
```

## O3 receive result

The bounded sequence reached local O3 with GPIO16 `TX_DISABLE` asserted,
GPON interrupts disabled and the provider identity unchanged. It retained six
complete private downstream records, classified without publishing their raw
contents:

```text
record_count:             6
Upstream Overhead:        3
Extended Burst Length:    3
other:                    0
SN_Request observed:      yes
internal SN-send IRQ:     no
ONU-ID ever valid:        no
```

The six private records are byte-for-byte reproducible against the earlier
r11 capture. They remain mode `0600` outside the public repository.

## New correlation evidence

The MAC debug burst counter again changed exactly `0 -> 1`. r13 additionally
sampled the directly readable MAC TX-GEM counter 2,912 times; it remained zero
at baseline, O3 abort and cleanup:

```text
MAC TX_BST_CNT:  0 -> 1
MAC TX_GEM_CNT:  0 -> 0
TX/SN-send IRQ:  none
upstream writes: none
```

The raw PHY diagnostics changed as follows:

```text
TX status:       0x00000000 -> 0x00bc0001
TX frame count:  0x00000000 -> 0x00000000
TX burst count:  0x00000000 -> 0x00000000
```

Those PHY registers were deliberately read without touching the OEM latch
trigger. They are raw, unlatched and diagnostic-only; neither changed nor
unchanged values prove anything about optical emission. The only TX-status
bit interpreted by the available OEM PHY source is bit 15 (`PHY_TX_FEC`);
that bit remained clear in `0x00bc0001`. The remaining changed status bits are
undocumented here.

The PLOAMu FIFO status also changed:

```text
0x00800080 -> 0x80800080
```

The EN7521/EN751221 OEM register header identifies bit 31 as
`ploamu_fifo_udrn`, bits 23:16 as minimum available entries and bits 7:0 as
currently available entries. Both availability fields remained `128`; the
only observed delta was the underrun bit. Thus the permanent upstream-source
latch was:

```text
0x00000006 = MAC TX_BURST + PLOAMU
```

The OEM `gponDevSetO3O4PloamCtrl()` implementation confirms that bit 0 clear
selects hardware control and bit 0 set selects software control. Its normal
upstream PLOAM send routine performs exactly three writes to the PLOAMu data
register. Because r13 forced software control and performed zero upstream-FIFO
writes, the observation is most consistent with the MAC accounting a
scheduled burst while requesting a software response from an empty FIFO. The
status value alone does not establish that words were queued or consumed, and
it is not proof that the PHY or laser emitted a burst.

An exhaustive search of the available OEM source found no write or clear of
`G_PLOAMu_FIFO_STS`; production code only reads it for a CSR dump. Generated
field `SET` macros do not document register access semantics, and the DVT
writable-register test omits both PLOAM FIFO status registers. There is
therefore no source evidence that bit 31 is read-clear or write-one-to-clear,
so r13 correctly did not write it. A full MAC reset is the only source-backed
clean boundary, and the lab continues to require the stronger physical cold
power cycle.

## Fail-closed cleanup

r12/r13 only admit the exact isolated `TX_BST_CNT +1` pattern for
risk-reducing formatter/O1 cleanup. The additional PLOAMu source made this run
ineligible:

```text
cleanup TX-burst latch attempts: 0
formatter restored:             no
software PLOAM control restored:no
sequence result:                -1
runner result:                  rc=129, unsafe-pinned
```

The module nevertheless restored the local activation state to O1 and returned
the WAN mux from GPON to ATM. All final physical/digital guards passed and
GPIO16 remained asserted. It performed no identity, upstream-FIFO,
GPIO/pinctrl, EN7570, laser/APD or Extended-Burst formatter writes.

Because the programmed formatter and forced software-PLOAM control remained
resident, a physical power cut was mandatory. The router was observed
unreachable in three checks beginning at
`2026-07-26T04:01:42-03:00`.

## Capture integrity

The ordinary capture and its backup are mode `0600`, byte-identical, 21,366
bytes, and have SHA-256:

```text
de4122cf853ba1614224cf3da67fa45a75a1f9b067b6f1a8af8e995fa21f9faa
```

The private-record artifact was deliberately marked invalid for public/result
purposes because the terminal state was unsafe-pinned. Its primary and backup
copies are still mode `0600`, byte-identical, 963 bytes, and have SHA-256:

```text
78fde9e51f5235533e358835cfd4a9a76b09029566399a5e876ddb04f3f997cc
```

## Next bounded step

Before another live run, a source-only successor may distinguish the exact
bit-31-only PLOAMu underrun from FIFO occupancy movement. Any cleanup exception
must still require all of the following simultaneously:

- exact stable `TX_BST_CNT +1`;
- unchanged TX-GEM count and no TX/SN-send interrupt;
- PLOAMu status equal to baseline plus only bit 31, with unchanged available
  and minimum-available fields;
- unchanged identity, GPIO16, xPON guards, activation state and WAN mux;
- no upstream write;
- permanent failure result and mandatory physical power cut.

That would only permit risk-reducing restoration. It still could not establish
optical silence or authorise an upstream/ONU test on the ISP network.
