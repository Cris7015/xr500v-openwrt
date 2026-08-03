# GPON O3 RX-only — software PLOAM control gate

Date: 2026-07-26

Status: **OEM source finding and r11 implementation/audit. r11 was subsequently
loaded exactly once; its live result is documented separately.**

## Why the previous O3 run stopped

The guarded O3 RX module read GPON MAC register `0x3c4` as zero after selecting
the GPON WAN aperture. It then restored the WAN mux and stopped before O2,
formatter writes or downstream FIFO reads.

That rejection was intentional. With bit 0 clear, the GPON MAC owns O3/O4
upstream PLOAM autonomously. It can construct the serial-number response and
raise its “sent in O3/O4” status without a software write to the upstream FIFO.
A later OEM-source audit corrected an earlier safety interpretation: GPIO16
`TX_DISABLE` is the confirmed physical TX kill, whereas `PHYSET3` bit 5 selects
burst versus continuous mode and is normally clear for GPON burst operation.
An RX-only experiment therefore must not treat bit 5 as an independent inhibit
or depend only on GPIO state when the MAC is attempting upstream work.

## OEM source evidence

The EN751221 GPL register layout names the word `O3_O4_PLOAMU_CTRL`. The
OpenWrt lab maps the GPON MAC window at `0x1fb64000`, making its local offset
`0x3c4`.

Relevant OEM files:

```text
en751221-linux26/tclinux_phoenix/modules/private/xpon/inc/gpon/gpon_reg.h
en751221-linux26/tclinux_phoenix/modules/private/xpon/inc/gpon/gpon_mac_reg_c_header_en7521.h
```

The register has two named fields:

```text
bit 0: o3_o4_ploamu_ctrl
bit 8: ploamu_ind_ctrl
```

The GPL getter and setter remove the ambiguity:

```c
*sel = (o3_o4_ploamu_ctrl.Bits.o3_o4_ploamu_ctrl == 0) ?
       GPON_HW : GPON_SW;

o3_o4_ploamu_ctrl.Bits.o3_o4_ploamu_ctrl =
       (sel == GPON_HW) ? 0 : 1;
```

They are in:

```text
en751221-linux26/tclinux_phoenix/modules/private/xpon/src/gpon/gpon_dev.c
```

Therefore:

- bit 0 clear means hardware/autonomous O3/O4 PLOAM control;
- bit 0 set means software O3/O4 PLOAM control;
- bit 8 and every reserved bit are unrelated and must remain unchanged.

The ordinary OEM initialization paths do not appear to call the setter. The
setter is exposed through the GPON management/debug ioctl and accepts
`o3_o4_ploam sw` or `o3_o4_ploam hw`, so software mode is a vendor-exposed
register state rather than a guessed undocumented value.

The OEM interrupt layout separately reports `SN_Request`, `SN_ONU sent in O3`
and `SN_ONU sent in O4`. Its software PLOAM send routine, by contrast, writes
three words to `G_PLOAMu_WDATA`. This is consistent with the hardware mode
generating the O3/O4 serial-number response internally.

## Narrow RX-only transition

r11 keeps the existing default rejection. A new read-only module parameter
provides a second explicit opt-in before any hardware-auto baseline can be
changed.

After proving O1, reset/invalid ONU identity, a quiet upstream FIFO, zero GPON
interrupt enable, unchanged TX-burst count, GPIO16 high and `PHYSET3` bit 5
clear in its expected GPON burst-mode state, the only new write is:

```text
new_0x3c4 = old_0x3c4 | BIT(0)
```

The complete word must read back exactly. Bit 8 and all reserved bits must
match the saved word. The forced word then becomes an invariant in every O2
and O3 poll/guard.

While it is forced:

- no upstream FIFO data/control write is allowed;
- no identity, serial-number or SN-configuration write is allowed;
- no GPON/xPON IRQ enable write is allowed;
- no write to GPON interrupt status is allowed;
- no GPIO, pinctrl, EN7570, laser, APD or PHY-TX write is allowed;
- `PLOAMU_SEND`, `SN_ONU_SEND_O3/O4`, any TX-burst increment, or any upstream
  FIFO-status change aborts immediately;
- downstream `SN_Request` may be recorded only as a non-secret boolean event.

On every exit, local GPON activation must first be proven back in O1. Only
then may the complete original `0x3c4` word be restored, followed by the WAN
mux returning to ATM. If O1 cannot be proven, hardware-auto must not be
restored: the saved hardware-auto word is not written. The current `0x3c4`
readback is retained as evidence rather than assuming which state survived.
The module remains unsafe-pinned, TX remains physically killed and a physical
power cut is required.

## Expected limit

This can characterize the downstream O2-to-local-O3 conversation and confirm
arrival of `SN_Request` without allowing the MAC to answer. It deliberately
cannot advance the OLT to O4/O5 because no serial-number response is sent.

An actual O3-to-O4 response is a separate TX experiment requiring a controlled
OLT, calibrated laser path, identity policy and burst-timing validation. It is
not authorized by this RX-only change.

## Runner boundary

The private one-shot runner remains pinned to earlier frozen artifacts. r11
has passed two independent write-surface, guard and restoration audits, but
the active runner has not been updated merely because the module compiles. A
separate r11 draft now uses the frozen source/`.ko`, checks the new control-word
and restoration fields, and has passed syntax, ShellCheck and an independent
oracle audit. It remains non-executable and has an extra release latch before
any router contact. A later fibre-connected cold-boot window is still required.

## r11 offline evidence

r11 removes the inherited GPON interrupt-status W1C operations and keeps
`0x09c` read-only. Its general formatter allow-list does not include `0x3c4`;
the bit-0 RMW and exact O1-gated restoration each use a dedicated path.

Two clean MIPS builds reproduced the source and `.ko` byte for byte:

```text
source SHA-256:
c766a043254f04cc07b353d44084889102661224a56699c06f5111db939b5b74

built module SHA-256:
7bab8b95a6758c9606852340d6c7eb2b528eff3cc26d8e4037e76722238befd9
```

`modinfo` confirms `force_software_ploamu_control:bool`. Checkpatch reports
0 errors and 0 warnings (24 style checks), and `git diff --check` passes. APK
container hashes differed between equivalent builds, so only source and `.ko`
are reproducible identity evidence.

## Live follow-up

r11 was later run exactly once after a verified cold boot. It reached local O3,
observed `SN_Request`, retained three Upstream Overhead and three Extended Burst
Length records, then aborted immediately when the MAC debug TX-burst counter
changed `0 -> 1`. No TX/SN-send interrupt, upstream-FIFO delta or upstream
write was observed, and GPIO16 remained asserted. OEM source now supports
interpreting that isolated delta as a scheduled SN slot, not proof of optical
emission, but the transaction remains fail-closed and required a physical
power cut. See
[`notes/2026-07-26-gpon-o3-r11-live-safe-abort.md`](../notes/2026-07-26-gpon-o3-r11-live-safe-abort.md).
