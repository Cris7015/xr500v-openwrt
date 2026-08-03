# GPON O3 RX-only r14 — exact combined-latch cleanup

Date: 2026-07-26

Status: **r14 reached local O3, reproduced the bounded downstream record
classification and failed closed on the already-characterized combined
`TX_BST +1` plus PLOAMu bit-31 underrun indication. Unlike r13, the exact
combined class passed every cleanup boundary, so the formatter, O1 activation
state and ATM WAN mux were restored. Forced software-PLOAM control remains
pinned by policy; the mandatory physical cold power cut was completed and
verified after the capture.**

This is not evidence of optical transmission, optical silence, OLT acceptance,
ONU registration or a usable data path.

## Offline release boundary

The r14 source extends r13 only enough to classify the exact stable combined
event for risk-reducing cleanup:

```text
source SHA-256:
f6233ebfd891c73668a2fd1e6b9e8db6a4b91cb01c7b558e3cad80c89a3b81dc

unstripped module SHA-256:
2c00c5cde9f58fb574afa71a3bbbabc1bd5e2515632aad7dc70f86bbb2fcf211

packaged stripped module SHA-256:
bc10cbca08557c6539e5465154b97e5eef841fc0f04b9a68488f510c61b92b34
```

The two module forms reproduced byte-for-byte across two clean MIPS builds.
An independent source audit found and closed one release blocker: each cleanup
pre/post guard now recomputes
`G_INT_STATUS & ~O3_RX_O3_ALLOWED_STATUS`, permanently accumulates any unsafe
bit and makes the class ineligible. The final source retained the same nine
write call sites as r13 and added no PLOAMu status/FIFO write or clear.

The private runner was separately audited. A second release blocker was found
and closed before execution: private records are now retrievable only after
both return code zero and the exact normal `O3_RX_TRANSACTION_PASS` marker.
Every unsafe, failed, timed-out or unconfirmed outcome skips that endpoint.
The exact executed runner SHA-256 was:

```text
b217070556df7266441f2ca973036e6d3cf4530322778642ec6461418f20105e
```

## Cold-boot and live boundary

The router had remained physically off for more than the required 35 seconds.
The runner observed uptime 641 seconds on the new boot, a stable boot ID,
watchdog offline, no experimental module and no stale debugfs endpoint.
Phase 28 then passed its full receiver oracle before r14 loaded.

The bounded r14 run reached local O3 with GPIO16 `TX_DISABLE` asserted,
GPON interrupts disabled and identity registers unchanged. It classified six
complete downstream records without exposing their raw contents:

```text
record_count:          6
Upstream Overhead:     3
Extended Burst Length: 3
other:                 0
SN_Request observed:   yes
internal SN-send IRQ:  no
ONU-ID ever valid:     no
```

## Exact combined class

The same internal event seen under r13 reproduced:

```text
MAC TX_BST_CNT:       0 -> 1
MAC TX_GEM_CNT:       0 -> 0
PLOAMu FIFO status:   0x00800080 -> 0x80800080
TX/SN-send IRQ:       none
upstream FIFO writes: none
```

The PLOAMu availability and minimum-availability fields both remained 128;
only the OEM-defined underrun bit 31 changed. r14 therefore admitted exactly:

```text
cleanup class:   tx-burst-plus-one-ploamu-bit31-only
source mask:     0x00000006
accepted delta:  0x80000000
```

It performed 15 class cleanup guard checks. The TX-burst count, PLOAMu status,
downstream FIFO status and TX-GEM count remained stable. Every full IRQ
boundary was clean:

```text
unsafe_last:  0x00000000
unsafe_mask:  0x00000000
changed_again:0
allowed_mask: 0x22110055
```

The raw, unlatched PHY diagnostics again showed status `0x00bc0001` while
their frame and burst words remained zero. These values are non-decisional;
they prove neither optical emission nor optical silence.

## Risk-reducing cleanup

r14 restored the exact saved formatter, returned the activation field to O1
and returned the WAN mux to ATM:

```text
formatter restored: yes
activation after:   O1
WAN mux:             0x00000003 -> 0x00000000 -> 0x00000003
readback failures:  0
```

The only reported restore-guard failure is the intentional refusal to return
O3/O4 PLOAM control to hardware-auto after an upstream latch. Bit 0 therefore
remains software-controlled until cold power removal. The transaction remains
failed by design:

```text
sequence_result:           -EPERM
worker/status:             unsafe-pinned
runner result:             observed-powercycle-required
cold power cycle required: yes
```

There were zero IRQ-status, IRQ-enable, identity, upstream-FIFO,
GPIO/pinctrl, non-formatter xPON, EN7570, laser or APD writes. Because the
terminal state was unsafe-pinned, the runner deliberately did not retrieve
the private-record endpoint.

## Capture integrity

The ordinary capture and its backup are mode `0600`, byte-identical, 18,983
bytes and have SHA-256:

```text
f61a0f6d98ffde7fa43d620c996a4198c080a86404a906b03f0e1ec3404254db
```

No r14 private-record artifact was created.

## Verified power removal

The router became unreachable immediately after the physical power-off
request. It remained unreachable across checks from
`2026-07-26T04:31:08-03:00` through `2026-07-26T04:32:16-03:00`, exceeding
the required 35-second cold-reset interval. No warm reboot or module unload
was used.

## Interpretation and next boundary

r14 closes the cleanup question raised by r13: the combined stable event can
be handled without leaving the formatter or WAN mux programmed. It also
reproduces the interpretation that the MAC scheduled one response opportunity
against an empty software-controlled PLOAMu FIFO. That is an internal MAC
observation, not an optical result.

Repeating this same ISP-fibre run would add little evidence. Further RX-only
work can move the proven receiver, downstream-PLOAM parsing and fail-closed
state handling into the driver architecture. Testing a real upstream response,
ONU registration, O5, GEM or OMCI requires an isolated controllable OLT/test
bench and a separately audited transmitter-calibration and burst-timing path;
it is not authorised by this RX-only result.
