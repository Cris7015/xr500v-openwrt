# GPON bring-up — phase 14 live EN7570 software-reset observation

Date: 2026-07-13

## Result

The guarded OEM four-byte EN7570 software-reset operation was executed once on
the fibre-disconnected lab router.  Every precondition matched phase 12, the
I2C transfer succeeded, all immediate post-reset reads succeeded, and the xPON
TX postflight remained fail-closed.

The reset trigger self-cleared, but none of the 28 visible EN7570 register
groups changed.  Five seconds later the filtered passive snapshot was still
bit-identical to the original clean baseline.

This proves that the isolated software-reset pulse is safe under the current
GPIO16/TXEN/test/IRQ barriers when starting from a clean EN7570 state.  It does
not yet prove whether the pulse clears the autonomous LOS residue observed in
phase 10, because that dirty state was not recreated for this test.

No LOS, ADC, RSSI, APD, TGEN, current, DDMI or interrupt init was executed.

## Image and preflight audit

The temporary image changed only the xPON diagnostic node's compatible and DT
allow property.  The reset module remained absent from SquashFS and all
autoload directories and was copied to `/tmp` only after boot.

```text
temporary image SHA-256: 5234de2397bec4b7e957f40b110e6810dd428a7c287c61f552d11d31e5aa1b3a
reset module SHA-256:    a6c5d11b07d242a3e78fce2bb0ba25544c17e7241b0ec07bf19ea7301688270d
temporary DTB SHA-256:   08f6dd9d057408220a3313edf2c114bb57963bb911762ac135446c680aa8b1ca
image size:              11095462 bytes
compressed kernel:       2948030 bytes
kernel headroom:         197186 bytes
rootfs file offset:      0x300200
512-byte gap:            present
TrendChip header:        valid
```

Before flashing, the filtered EN7570 snapshot matched the phase-12 hash:

```text
7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
```

The same hash was obtained after the temporary image boot.  A negative gate
test loaded the module without its arm parameter; probe returned `-EPERM`
before GPIO/MMIO/I2C access.  The module was removed and the baseline hash was
again identical before the armed load.

## Armed observation

The one permitted reset transaction reported:

```text
reset payload:          01 00 00 00
I2C write attempts:     1
reset result:           0
post-snapshot result:   0
TX postflight result:   0
module pinned:          yes
GPIO16 TX_DISABLE:      asserted
xPON TXEN:              clear
```

All 28 before/after register rows were equal.  The newly expanded four-byte
values also established:

```text
SAFE_PROTECT: ff 8f ff 0f
ROGUE_TX:     34 02 00 00
```

The `SW_RESET` register read `00 00 00 00` both before and immediately after,
consistent with a write-one self-clearing trigger.  The only observed movement
was the already-known free-running LOS debug state; it is excluded from the
stable baseline hash.

Five seconds later:

```text
filtered EN7570 hash: 7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
TX_DISABLE:           asserted
TXEN:                 clear
module refcount:      1 (self-pinned)
```

## Physical recovery and passive restoration

A physical power cycle was performed as required.  The module pin disappeared
and the clean baseline hash returned unchanged.  No software reboot was used
as a substitute for the recovery boundary.

The build tree was then restored to the normal passive compatible and a second
fully audited image was installed:

```text
passive image SHA-256: 0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d
passive DTB SHA-256:   08bc473008ce8eb1a769457c0d587200907fa6cc8137db691544b0feaeb82d62
image size:            11095474 bytes
compressed kernel:     2947970 bytes
kernel headroom:       197246 bytes
TrendChip/layout:      valid
```

Final live state:

```text
DT compatible:        econet,en751221-xpon-phy-diag
experimental allow:   absent
reset module:         absent
filtered EN7570 hash: 7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
xPON TXEN:            clear
GPIO16 TX_DISABLE:    asserted
PRBS/test-frame/IRQ:  off / off / zero
passive MMIO writes:  0
```

## Next boundary

Do not repeat the same reset-only test: it is now fully characterized.  A
meaningful next experiment must answer a new question, such as whether an OEM
reset immediately preceding the isolated LOS trigger changes the phase-10
autonomous result.  That must be a combined one-shot observer with no register
rollback claim, a physical-power-cycle recovery plan, and no ADC/RSSI/TX/APD
expansion unless static analysis demonstrates a specific dependency.

