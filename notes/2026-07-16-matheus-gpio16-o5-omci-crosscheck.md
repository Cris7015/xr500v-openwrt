# Matheus EN7523/EN7571 GPIO16, O5 and OMCI cross-check

Date: 2026-07-16

Status: **external EN7523/EN7571 testing now proves that a correctly armed
optical transmitter can complete GPON activation through O5 and expose OMCI;
the GPIO16 and safe-circuit ordering also matches the XR500v OEM design**

This note records only derived hardware and driver facts.  The supplied UART
logs contain optical and interface identities and must remain private.  No raw
identity, serial-number payload or MAC value is reproduced here.

## Sources inspected

- Matheus' issue comments 3877--3879:
  <https://sirherobrine23.com.br/airoha_en7523/kernel/issues/5#issuecomment-3879>
- EN7523 comparison from `e1ad8e258738652e95711e19efa3046c0416fb43`
  through `6c712c0a4f8938ede3ca4422bed911be6bd99abb`:
  <https://sirherobrine23.com.br/airoha_en7523/kernel/compare/e1ad8e258738652e95711e19efa3046c0416fb43...6c712c0a4f8938ede3ca4422bed911be6bd99abb>
- Current combined branch head at inspection time,
  `a548dd3f940f203dec72ef1570aa059169b0f6a7`.
- Three local XX530v UART captures and `askey_userfs.tar.gz`, received on
  2026-07-16.  They were listed/read as data only and never executed.
- The XR500v EN751221/EN7570 OEM GPL tree under
  `/home/cristuu/tools/xr500v/en751221-linux26`.

## GPIO16 is the physical active-high TX disable

The relevant `7529_62led.conf` profile in the supplied userfs contains:

```text
LED_PHY_TX_POWER_DISABLE = 42
42  16  1  0  1
```

The same mapping is present in its 7523 dual/GU profiles.  Some generic LED
profiles in the archive map logical entry 42 elsewhere, so the selected board
profile matters; the 7529 board profile used for this device maps it to
GPIO16.

Logical `LED_PHY_VCC_DISABLE` entry 102 is unused in that profile.  The
XR500v OEM profile also leaves 102 unused.  This supports Matheus' conclusion
that GPIO16 is the actual `TX_DISABLE` line rather than control for a separate
BOSA 3.3 V rail; it is consistent with the optical supply being always on.

The older UART capture shows GPIO16 owned as `tx-disable`, output low.  This
confirms that the external line is active-high: high inhibits optical TX and
low releases the external inhibit.

The XR500v OEM `led.conf` independently contains the exact same mapping.  Its
`phy_tx_ctl(PHY_ENABLE)` calls `ledTurnOff(42)`, which drives the active-high
disable low; `PHY_DISABLE` calls `ledTurnOn(42)`, driving it high.  This agrees
with the existing XR500v DTS and live gpiolib observations.

## GPIO low is necessary but not sufficient

The first supplied run already had `tx_disable=0` and GPIO16 output low, but
its laser bias and modulation-current controls remained zero.  It repeatedly
entered O3 and never reached O4.

In the two later runs:

- the EN7571 calibration cell was loaded;
- bias and modulation-current controls became stably nonzero;
- downstream Upstream-Overhead and Extended-Burst-Length PLOAM were decoded;
- the MAC transmitted a serial-number response which the OLT actually heard;
- the OLT assigned an ONU ID and the state machine entered O4.

O4 is the important proof: it cannot be explained merely by a software
`sent` flag in the ONU MAC.  The OLT had to receive enough of the upstream
burst to associate and answer it.

Those three private attachments stop at O4.  The later public issue capture is
the separate evidence for the complete sequence: Upstream Overhead, Extended
Burst Length, O3, Assign ONU-ID, O4 ranging, Ranging Time/EqD programming and
O5.  Comment 3879 then reports observed OMCI traffic.  The public run still
uses an incomplete data path and subsequently stops, so this is activation
and management-plane proof rather than a finished HGU networking stack.

## Exact driver changes which unlocked TX

Commit `343e2abba0f2f8b2d7a873444684d793c8e98fb9` changes the EN7523-family
pinctrl driver so a GPIO request asserts the SoC `FORCE_GPIO_EN` bit and makes
pinmux ownership non-strict.  This is required on that ARM SoC so requesting
GPIO16 really routes the pad as GPIO.

The board wiring itself is in the matching OpenWrt change
`5a6a81e6a3aada84deee9d88b6df6b71410bf3a0`: `en7523.dtsi` describes
GPIO16 as an active-high `tx-disable-gpios` line while the default PON pin
group also owns that pad.  The new force-GPIO callback is what resolves that
ownership overlap.

The following xPON update adds an explicit LDDLA transmitter rearm after the
external TX disable has been released:

1. release the external `TX_DISABLE` line;
2. set the EN7571 DCL reset-release bit;
3. pulse/reset the EN7571 safe-circuit latch with the preserved-byte mask
   `(old & 0xbf) | 0x40`;
4. continue normal GPON enable.

It also corrects the EN7571 xPON setting from `0x10f` to `0x14f` and adds
activation/PHY-counter diagnostics.  The combined branch commit contains
more diagnostics than the shorter comparison commit, but the rearm sequence
is the same.

The ordering is not accidental.  The Linux SFP state machine first calls its
TX-enable operation, which releases the external `TX_DISABLE`; only on the
following state transition does it invoke the module-start callback which now
calls the LDDLA rearm.  This matches both the driver's stated precondition and
the observed GPIO-low-before-rearm behaviour.

## Direct XR500v relevance

The EN7523 pinctrl patch itself must not be copied into the EN751221 target:
the XR500v is MIPS and uses the TC3162 GPIO block, not the newer Airoha ARM
pinctrl implementation.  We already have working ownership and exact
direction/level checks for EN751221 GPIO16.

The sequencing concept *is* directly relevant.  The XR500v OEM performs:

```text
release GPIO16 TX_DISABLE
queue EN7570 safe-circuit reset
```

Its EN7570 `mt7570_safe_circuit_reset()` uses the same safe-register operation
as Matheus' EN7571 fix:

```text
SAFE_PROTECT byte 1 = (old & 0xbf) | 0x40
```

This independently validates that the safe-circuit reset belongs after the
physical inhibit is released.  Before any XR500v TX experiment we still need
to identify and audit the EN7570 equivalent of the extra DCL rearm, load the
per-unit TX calibration, and preserve the OEM ordering for bias, modulation,
burst timing and `PHYSET3.TXEN`.

The current shared LDDLA code recognizes an EN7570 device while locating the
optical controller, but only the EN7571 operations table implements the new
`tx_rearm` callback.  Applying the patch unchanged to EN7570 would therefore
return `-EOPNOTSUPP` at module start and abort GPON enable.  The XR500v needs a
separate EN7570 callback based on its existing safe-reset implementation and
audited against the OEM sequence; the EN7571 DCL write must not be assumed to
be register-compatible.

## Effect on the current plan

The current XR500v phase-36 boundary does not change: it remains RX-only,
keeps GPIO16 high and `PHYSET3.TXEN` clear, and should privately pop exactly
one 12-byte downstream PLOAM record.  Matheus' state-machine traces make that
capture more valuable because its message type can now be compared directly
with the expected O2 activation messages without enabling TX.

After that capture, work separates into two tracks:

1. port the downstream PLOAM decoding and O2/O3/O4/O5 state-machine ordering;
2. build a separately audited TX bring-up which loads calibration, proves the
   GPIO/EN7570 rearm sequence and only then permits a bounded burst.

The second track must not be tried blindly on the ISP fibre.  The present
phase-36 experiment is safe on the connected fibre because both independent
TX kills remain asserted; any phase which releases GPIO16 or enables TX must
be treated as a new boundary and preferably use a controllable lab OLT.
