# GPON bring-up — phase 12 EN7570 pre-reset TX/APD inventory

Date: 2026-07-12

## Result

The passive EN7570 diagnostic was expanded to inventory every register group
that the OEM code uses for burst timing, monitor-photodiode targets, laser
current control, extinction-ratio control and APD setup.  No register-data
write, ADC latch, reset or optical init was performed.

Two captures two seconds apart were identical after excluding only the known
free-running LOS debug line.  The retained bootloader state has zero live
bias/modulation codes, zero power-loop latches, APD control disabled and ERC
start/enable clear.  These observations make a future reset experiment easier
to police, but do not prove what defaults the whole-device reset will select.

Fibre was disconnected.  xPON TXEN stayed clear and active-high GPIO16
TX-disable stayed asserted.

## Passive release 6 snapshot

```text
MPD targets       0x0004  00 02 00 00
T1/T0 delay       0x0008  99 00 00 20
PI/TGEN           0x0020  06 00 00 00
APD DAC/control   0x0030  00 08 00 00
P0 CS1            0x0134  00 02 04 10
P0 CS2 / Ibias    0x0138  00 00 00 00
P0 CS3 / ERC      0x013c  30 12 00 10
P0 latch          0x0140  00 00 00 00
P1 CS1            0x0144  00 02 04 10
P1 CS2 / Imod     0x0148  00 00 00 00
P1 CS3 / ERC      0x014c  30 12 00 10
P1 latch          0x0150  00 00 00 00
APD OVP latch     0x0164  00 00 00 00
```

Relevant bit interpretation from the OEM/merbanan register model:

- APD control (`APD_DAC_CODE.byte1 bit0`) is clear;
- APD soft-start (`byte2 bit5`) is clear;
- the TGEN/ERC enable bit (`T1DELAY.byte3 bit3`) is clear;
- both P0/P1 ERC start bits (`CS3.byte0 bit0`) are clear;
- both 10-bit current codes and both readback latches are zero.

The full stable capture hash, excluding `los_debug_raw`, is:

```text
7f4d5868528dfbf0246467576bd18b08bf7bd0da71d03ddefc5998daaf713690
```

Release-6 module audit:

```text
module SHA-256: 033bbd72f5573b6b3760e482723486256dd0302201301ff9325a5701b8105c64
checkpatch:     0 errors, 0 warnings
kernel:         6.12.80
```

## Reset experiment specification (not executed)

A future reset-only experiment must be modeled as an observation with physical
power-cycle recovery, not as a reversible register write:

1. require a dedicated non-shipping DT compatible and explicit module opt-in;
2. drive GPIO16 TX-disable high before mapping xPON or acquiring EN7570;
3. reject TXEN, rogue-test, PRBS, test-frame or xPON IRQ activity;
4. require the exact phase-12 TX/APD baseline, not merely zero TXEN;
5. snapshot the complete release-6 register set;
6. issue the OEM four-byte RMW at `SW_RESET=0x0300` only once;
7. perform no LOS, ADC, RSSI, TGEN, current, APD or interrupt write afterward;
8. immediately snapshot every register again and keep TX-disable asserted;
9. never claim software rollback; require a person present to remove physical
   power if the resulting state is unexpected.

The active LOS prototype remains quarantined.  No reset-stage code is enabled
or present in the shipping image.

