# GPON bring-up — phase 13 guarded EN7570 reset audit (compile-only)

> **Hardware result:** phase 14 executed this reset observer once.  The pulse
> succeeded and self-cleared without changing any visible EN7570 register or TX
> safety state.  See
> `2026-07-13-gpon-phase14-en7570-reset-live.md`.

Date: 2026-07-12

## Result

A dedicated one-shot EN7570 software-reset observation driver now exists, but
was not copied to, loaded on, or matched by the router.  It is deliberately a
separate package from the quarantined LOS prototype so reset testing cannot
accidentally repeat LOS, ADC or RSSI work.

The router was revalidated before implementation against phase 12:

```text
firmware/kernel:       stable OpenWrt / 6.12.80
passive r6 local SHA:  033bbd72f5573b6b3760e482723486256dd0302201301ff9325a5701b8105c64
passive r6 router SHA: 033bbd72f5573b6b3760e482723486256dd0302201301ff9325a5701b8105c64
EN7570 baseline:       exact phase-12 match
xPON TXEN:             clear
GPIO16 TX_DISABLE:     asserted output
TX tests / IRQ:        off / zero
```

No EN7570 data write occurred in this phase.

## Independent gates

The reset audit cannot probe unless all of these conditions hold:

1. its package is installed manually (no autoload and absent from device
   packages);
2. a temporary DT changes the platform compatible to
   `econet,en751221-en7570-reset-audit-experimental`;
3. the temporary DT adds `econet,allow-en7570-reset-audit`;
4. the module is loaded with `arm_en7570_reset_audit=1`;
5. GPIO16 can be driven output-high before xPON/EN7570 access;
6. TXEN, rogue-ONU test, PRBS, test-frame and xPON IRQ enable are all clear;
7. EN7570 ID is exactly `0x03`;
8. all 28 retained control/status groups match the phase-12 baseline byte for
   byte (only the two known valid bytes are compared for SAFE_PROTECT and
   ROGUE_ONU status).

The shipping DTS contains neither the compatible nor the allow property.

## Sole write

The only data-write helper has a fixed register pointer for `SW_RESET=0x0300`.
It rereads the four-byte reset register, applies the OEM operation:

```c
reset[0] = (reset[0] & 0xf8) | 0x01;
```

and submits the complete four-byte payload once.  There is no generic write
API and no MMIO write primitive.  LOS, ADC, RSSI, APD, TGEN, current and
interrupt initialisation are absent.

## Fail-closed review correction

The first compile exposed a design error during review, before deployment: a
post-reset snapshot failure would have made `probe()` fail, causing devres to
release GPIO16.  The final design treats the first I2C transfer attempt as the
non-transactional boundary:

- it self-pins the module with `__module_get(THIS_MODULE)` immediately before
  the transfer, preventing accidental `rmmod`;
- it counts the transfer attempt before calling the adapter, because an error
  return does not prove that the slave saw no bytes;
- after that point, read/postflight/debugfs failures are recorded but never
  fail probe or release GPIO16;
- it makes no software rollback claim and explicitly requires physical power
  removal for recovery.

## Build audit

The final source passed checkpatch and both the direct kernel build and normal
OpenWrt package build for the router kernel.

```text
checkpatch:          0 errors, 0 warnings (351 lines)
OpenWrt package:     kmod-xr500v-en7570-reset-audit-6.12.80-r1.apk
packaged module SHA: a6c5d11b07d242a3e78fce2bb0ba25544c17e7241b0ec07bf19ea7301688270d
autoload entry:      none
shipping DT match:  none
router deployment:  none
```

## Next boundary

Do not execute this stage merely because the router is reachable.  A person
must be beside the router and ready to remove physical power.  Before building
the temporary image, repeat the phase-12 stable snapshot hash and audit the
DTB, SquashFS contents, module hash, kernel `0x2ffe00` ceiling, rootfs
`0x300200` offset, 512-byte gap and TrendChip header.
