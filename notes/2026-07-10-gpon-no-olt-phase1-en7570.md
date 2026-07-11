# GPON no-OLT bring-up — phase 1 EN7570 identification

Date: 2026-07-10

## Result

The Archer XR500v has an EN7570 LDDLA at 7-bit I2C address `0x70`.  A clean
OpenWrt boot identifies it passively as:

```text
i2c-mt7621 1fbf8000.i2c: clock 100 kHz
xr500v-en7570-diag 0-0070: EN7570 identified: silicon ID 0x03,
                           variant 0x01; passive mode
```

The live read-only status is:

```text
/sys/kernel/debug/xr500v-en7570/status

i2c_bus:              0
i2c_address:          0x70
silicon_id:           0x03 (EN7570)
variant:              0x01
register_data_writes: 0
reset_or_init:        no
laser_or_apd_control: no
adc_or_ddmi_control:  no
```

No fibre or OLT was connected.  The driver did not reset or initialise the
EN7570, load the calibration matrix, change laser current or APD voltage,
select ADC channels, start DDMI conversion, or touch the xPON MAC/PHY resets.
Ethernet and PPPoE returned normally after both test boots.

## Integration

The board now has a 20 MHz fixed clock and an MT7621-compatible I2C controller
at `0x1fbf8000`, operating at 100 kHz.  The already-loaded `gpio-tc3162`
driver leaves the required EN751221 IOMUX bits set (`CHIP_SCUL+0x104` was
observed as `0x0000a0ad`, including `PON_I2C_MODE` bit 0 and `GPIO_PON_MODE`
bit 15), so Merbanan's new full pinctrl driver was not imported.

Persistent files:

- `target/linux/econet/patches-6.12/921-i2c-mt7621-enable-econet.patch`
- `target/linux/econet/patches-6.12/922-i2c-mt7621-optional-reset.patch`
- `target/linux/econet/patches-6.12/923-i2c-mt7621-endian-safe-data.patch`
- `package/kernel/i2c-mt7621-econet/`
- `package/kernel/xr500v-en7570-diag/`
- `target/linux/econet/dts/en751221_tplink_archer-xr500v.dts`

## Big-endian controller bug found and fixed

Merbanan's `econet-eth-mainline` tree is compile-tested and reuses
`i2c-mt7621.c`.  The first boot registered the bus and got an ACK at `0x70`,
but both ID registers appeared as zero.  Address `0x38` was tested only to
rule out a vendor 8-bit-address convention and correctly returned NACK.

The root cause was CPU endianness.  The upstream driver used `memcpy()` between
the I2C byte buffer and the controller's two `u32` FIFO registers.  That gives
the required byte-0-in-bits-7:0 layout only on a little-endian CPU.  On the
XR500v's big-endian MIPS CPU, write bytes landed in the high word lanes and
readback copied the high zero byte instead of the low data byte.

Patch 923 replaces both copies with explicit shifts:

```c
data[k / 4] |= (u32)buf[k] << (8 * (k % 4));
buf[k] = data[k / 4] >> (8 * (k % 4));
```

With only that change, the same diagnostic returned the OEM-expected ID
`0x03` and variant `0x01`.  The OEM source independently performs this ID read
before `mt7570_sw_reset()`, so no analogue initialisation is required merely
to identify the silicon.

## Firmware/image safety validation

The final persistent image was checked before sysupgrade:

```text
compressed kernel:     2,947,688 bytes
safe payload limit:    0x2ffe00 (3 MiB partition minus 512-byte header)
headroom:              197,528 bytes
SquashFS file offset:  0x300200
TrendChip rootfs field:0x300000
TrendChip header:      valid
```

`scripts/validate_xr500v_image.py` now enforces those conditions, including
the easily missed 512-byte difference between the bootloader-relative rootfs
offset and its physical position in the combined image.  `build-local.sh`
runs it before and after `patch_trendchip_header.py`.

The persistent controller module installed in squashfs has SHA-256:

```text
314dd3b75fe0b251567991cab126a59cd590beb4a349c1dfc98b6a5903fec2af
```

## What can be done next without an OLT

The next no-OLT step should remain receive-side and reversible:

1. Compare the OEM `SIF_X_Read`/EN7570 static-register set with Merbanan's
   register map and classify registers that are genuinely read-only.
2. Add passive LOS/fault/status reads only where the OEM map proves that a read
   has no clear-on-read or ADC-mux side effect.
3. Port the xPON PHY as a disabled-by-default module and first validate reset,
   mode and ready-state transitions with TX forcibly inhibited.
4. Model the GPON MAC callback through WAN QDMA; do not adopt the placeholder
   standalone IRQ 25.

Temperature, optical power and other DDMI values are not yet a passive next
step: the OEM/experimental drivers obtain them by writing ADC mux and latch
controls.  PLOAM O1-to-O5 and OMCI still require an OLT or operator fibre.

Phase 2 completed items 1 and 2 for the OEM information-status subset; see
[`2026-07-10-gpon-no-olt-phase2-passive-status.md`](2026-07-10-gpon-no-olt-phase2-passive-status.md).
