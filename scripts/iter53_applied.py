#!/usr/bin/env python3
"""iter53: patch mt7530.c to use OEM stock values when xtal detection
returns 0 (EN751221 MCM mode where MTRAP does not expose the correct XTAL bits).

Stock OEM eth.ko trgmii_interface_init writes:
  ssc_delta = 0x57
  ncpo1     = 0x1d00   (NOT 0x1400 which upstream uses for MT7530+25MHz)
"""
import sys, os

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/drivers/net/dsa/mt7530.c"

src = open(FN).read()

if "ITER53_EN751221_TRGMII" in src:
    print("[i] iter53 already applied")
    sys.exit(0)

# Modify the xtal detection block in mt7530_setup_port6
old = """	xtal = mt7530_read(priv, MT753X_MTRAP) & MT7530_XTAL_MASK;

	if (xtal == MT7530_XTAL_25MHZ)
		ssc_delta = 0x57;
	else
		ssc_delta = 0x87;

	if (priv->id == ID_MT7621) {
		/* PLL frequency: 125MHz: 1.0GBit */
		if (xtal == MT7530_XTAL_40MHZ)
			ncpo1 = 0x0640;
		if (xtal == MT7530_XTAL_25MHZ)
			ncpo1 = 0x0a00;
	} else { /* PLL frequency: 250MHz: 2.0Gbit */
		if (xtal == MT7530_XTAL_40MHZ)
			ncpo1 = 0x0c80;
		if (xtal == MT7530_XTAL_25MHZ)
			ncpo1 = 0x1400;
	}"""

new = """	xtal = mt7530_read(priv, MT753X_MTRAP) & MT7530_XTAL_MASK;

	/* ITER53_EN751221_TRGMII: in MCM mode inside the EN751221 SoC,
	 * MT753X_MTRAP does not return the correct XTAL bits (reads 0).
	 * With xtal=0 ncpo1 was left uninitialized (UB). Stock OEM eth.ko
	 * trgmii_interface_init uses ssc_delta=0x57 and ncpo1=0x1d00 — those
	 * are the values the SoC TRGMII RX clock needs for correct latching.
	 */
	if (xtal == 0) {
		ssc_delta = 0x57;
		ncpo1 = 0x1d00;
	} else {
		if (xtal == MT7530_XTAL_25MHZ)
			ssc_delta = 0x57;
		else
			ssc_delta = 0x87;

		if (priv->id == ID_MT7621) {
			if (xtal == MT7530_XTAL_40MHZ)
				ncpo1 = 0x0640;
			else
				ncpo1 = 0x0a00;
		} else {
			if (xtal == MT7530_XTAL_40MHZ)
				ncpo1 = 0x0c80;
			else
				ncpo1 = 0x1400;
		}
	}"""

if old not in src:
    sys.exit("ERROR: mt7530 setup_port6 anchor not found")

src = src.replace(old, new, 1)

open(FN, "w").write(src)
print("[+] iter53 mt7530.c patched")
