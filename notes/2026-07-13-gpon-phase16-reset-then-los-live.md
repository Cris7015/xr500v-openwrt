# GPON bring-up — phase 16 live reset-then-LOS observation

Date: 2026-07-13

## Result

The fibre-disconnected one-shot `reset -> isolated LOS` observer completed
without transport, snapshot or TX-safety errors.  The reset produced the same
unchanged clean snapshot as phase 14.  The subsequent five OEM LOS writes then
completed successfully and started the same autonomous LOS state seen in phase
10.

Therefore an omitted EN7570 reset was not the cause of phase 10.  Phase 10's
`-EIO` reported the impossibility of visible-register rollback after the LOS
trigger; it did not show that the LOS programming itself failed.

The no-fibre reset/LOS dependency question is now closed and must not be
repeated.  The next meaningful observation requires downstream light while
physical TX-disable remains asserted.

## Audited artifacts

```text
compile-only commit:   e7b0918417
temporary image SHA:   6e686de1e73847b06e7f5345087ba225087b8c07b350c798b87fcf3361cf0f22
module SHA:            3eb48c5a75011b969c30bd771d2d7fd95fccdf2db4278793e1ae0938cd52085d
temporary DTB SHA:     08f6dd9d057408220a3313edf2c114bb57963bb911762ac135446c680aa8b1ca
compressed kernel:     2948030 bytes
kernel headroom:       197186 bytes
rootfs offset/gap:     0x300200 / present
TrendChip header:      valid
active module in rootfs/autoload: no / no
```

The temporary image booted with the exact clean filtered baseline:

```text
7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
```

## Execution

```text
I2C write attempts:     6 (one reset + five LOS)
reset result:           0
post-reset snapshot:    0
post-reset TX postflight:0
LOS attempted:          yes
LOS result:             0
post-LOS snapshot:      0
post-LOS TX postflight: 0
module pinned:          yes
GPIO16 TX_DISABLE:      asserted
xPON TXEN:              clear
thresholds:             high=0x1c low=0x10
```

All 28 groups were identical before and after reset.  After LOS, only the
expected RX/LOS fields and autonomous state changed:

```text
                         clean/post-reset   after LOS
SVADC_PD                00 00 01 00        00 00 41 04
LOS_CTRL1               06 08 3c 36        06 1f 1c 10
LOS_CTRL2               10 05 00 00        05 1f 22 00
LOS timeout             00 00 00 00        3e 00 00 00
LOS status              0                   1
```

The trigger bit self-cleared.  Five seconds later those fields were stable.
Bias and modulation codes remained zero, APD remained disabled, rogue-ONU and
Tx-SD remained clear, TXEN stayed clear, and GPIO16 stayed asserted.

This differs from phase 10 only because phase 16 intentionally did not attempt
rollback.  Phase 10 restored the visible configuration bytes but could not undo
LOS status/timeout/autonomous state.  Phase 16 demonstrates that retaining the
programmed LOS configuration is the coherent outcome of the sequence.

## Recovery and final state

The required physical power cycle restored the exact clean baseline.  The
already-audited phase-14 passive image was reused rather than rebuilt:

```text
passive image SHA:      0a1404a191f2dea8782da3fe4add19c04653fbba06061c2bb6126f9b85736b4d
DT compatible:          econet,en751221-xpon-phy-diag
experimental allow:     absent
active module:           absent
filtered EN7570 hash:    7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
TXEN / TX_DISABLE:       clear / asserted
PRBS / test frame / IRQ: off / off / zero
passive MMIO writes:     0
```

## Next boundary

No additional fibre-disconnected reset/LOS run is justified.  The next test is
a live-fibre A/B observation with TX physically disabled:

1. capture clean fibre-disconnected baseline;
2. apply the now-characterized isolated LOS configuration once;
3. capture the initialized no-light state;
4. connect downstream fibre without changing software state;
5. observe LOS debug/status and xPON RX sync/counters only;
6. disconnect fibre and physically power-cycle before returning it to service.

This still requires no OLT control for the receive-only observation, but it
temporarily interrupts the household fibre connection.

