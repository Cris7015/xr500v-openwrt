# GPON no-OLT bring-up — phase 0 read-only PHY identification

Date: 2026-07-10

## Goal and safety boundary

The first GPON bring-up step deliberately touched only the EN751221 xPON PHY
CSR block.  No OLT or optical fibre was used.  The test did **not** initialise
the EN7570, enable the laser, alter APD voltage/current, release xPON resets,
enable interrupts, clear counters, or flash firmware.

The diagnostic module has no write handler and makes no `iowrite32()` calls.
It maps physical `0x1faf0000..0x1faf0fff`, reads a curated register list and
exports two read-only debugfs files:

```text
/sys/kernel/debug/xr500v-gpon/status
/sys/kernel/debug/xr500v-gpon/regs
```

Source: `package/kernel/xr500v-gpon-diag/`.

## Live target baseline

The target was the Archer XR500v running the functional OpenWrt port:

```text
Linux 6.12.80, MIPS 34Kc big-endian, SMP
OpenWrt SNAPSHOT r34203-f3605b31fb
econet/en751221
```

No I2C adapter or xPON driver was registered.  The existing Ethernet driver
owned interrupt sources 21 and 22; no separate xPON MAC IRQ was active.

## Result

The physical register window is real and matches both the OEM xPON PHY source
and the register layout in merbanan's 2026 PON PHY driver.  The first live
snapshot was:

```text
0x0104 PHYSET2          0x00003c00
0x0108 PHYSET3          0x4581e114
0x0124 PHYSET10         0xff000000
0x0130 PHYSTA1          0x000f1919
0x0138 XPON_SETTING     0x0000014f
0x013c ANASTA1          0x0000f957
0x0140 ANACAL1          0xf0061310
0x0144 ANACAL2          0x00296064
0x021c PHYRX_STATUS     0x00000400
0x0230 XP_ERRCNT_EN     0x00000000
0x0234 XP_ERRCNT_CTL    0x00000000
0x0238 ERR_BYTE_CNT     0x00000000
0x023c ERR_CODE_CNT     0x00000000
0x0240 NOSOL_CODE_CNT   0x00000000
0x0244 RX_CODE_CNT      0x00000000
0x05e0 XPON_STA         0x00000000
0x05f0 XPON_INT_EN      0x00000000
0x05f8 XPON_INT_STA     0x00000004
```

Decoded state:

- `PHYSET10.GPON_MODE=1`: the block retains GPON mode.
- Firmware-ready and PHY-ready are clear; the PHY FSM is state `0x3`, not the
  ready state `0x6`.
- RX sync is absent (`PHYRX_STATUS[7:0]=0`, expected sync value `0x0a`).
- PHY TX enable is clear.  This test therefore did not find the SerDes TX path
  active.
- RX/TX PLL-lock and several analogue calibration-complete bits retain state.
  These do not imply a working optical link while the LDDLA is inactive.
- xPON interrupts are disabled.  Pending status `0x04` is the transceiver TX
  fault indication, consistent with the optical front-end not being brought
  up.
- `XPON_STA.LOS=0` is not evidence of received light.  The LOS polarity and
  signal are not meaningful until the EN7570 and board pin configuration are
  active.

The final module loaded, produced the snapshot, decoded the pending TX-fault
bit, unloaded, removed its debugfs files, and left Ethernet, PPPoE and router
reachability operational.  Its SHA-256 was:

```text
839622a86d864dd362cd808c0040febdae027666bce71a9fc2e6a31dc7e8dfff
```

## EN7570 calibration finding

The stock rootfs contains `/etc/7570_bob.conf`, exactly 400 bytes / 100 words,
which is the per-unit EN7570 calibration matrix.  Word offset `0x94` contains
the GPON magic `0x07050700`.  OEM boot logs independently confirm `EN7570
found`, `FLASH matrix got`, GPON TX calibration and successful GPON initialisation.

The OEM file is a raw array produced on the big-endian MIPS system.  Merbanan's
new LDDLA loader currently imports firmware words with `get_unaligned_le32()`.
The OEM blob therefore must not be installed verbatim as
`airoha/en7570_cal.bin`: each 32-bit word needs explicit BE-to-LE conversion and
the converted image must be validated before any driver is allowed to program
laser current or APD voltage.

## xPON MAC IRQ / datapath finding

Merbanan's compile-tested DT currently uses interrupt 25 as an explicit
placeholder for the GPON/EPON MAC.  The EN751221 OEM driver does not register a
standalone GPON MAC IRQ.  It registers `gpon_isr` through the WAN QDMA callback:

```c
QDMA_API_REGISTER_HOOKFUNC(ECNT_QDMA_WAN,
                           QDMA_CALLBACK_GPON_MAC_HANDLER,
                           gpon_isr);
```

The separately observed OEM IRQ 19 is the dying-gasp interrupt.  The mainline
model should consequently route GPON MAC events and Ethernet/GEM frames through
the WAN QDMA instance rather than assigning an unverified direct IRQ to the MAC
node.

The stock boot also reports ONU type 2 (HGU).  For the XR500v the first useful
network model can therefore be HGU-only: a PON WAN netdev backed by WAN QDMA,
with Linux VLAN/PPPoE/routing above it.  Generic SFU bridging can remain out of
scope until the HGU data path works.

## Next safe phase

1. Add and test a converter/validator for the 100-word EN7570 calibration blob.
2. Add an EN7570 **diagnostic-only** probe: identify I2C address `0x70` and read
   passive status/DDMI without reset or analogue/laser programming.
3. Add the PON PHY DT node at `0x1faf0000`, initially disabled; do not use an
   unverified IRQ.
4. Design the GPON MAC callback and packet path on WAN QDMA using the OEM
   `QDMA_CALLBACK_GPON_MAC_HANDLER`, descriptor GEM metadata and the existing
   stock GPON/PPE captures.
5. Only after calibration and TX-disable controls are independently verified,
   consider a fibre/OLT PLOAM O1-to-O5 test.
