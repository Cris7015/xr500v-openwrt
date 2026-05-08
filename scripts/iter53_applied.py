#!/usr/bin/env python3
"""iter53: parchar mt7530.c para usar valores stock OEM cuando xtal detection
devuelve 0 (caso EN751221 MCM mode donde MTRAP no expone XTAL bits correctos).

Stock OEM eth.ko trgmii_interface_init escribe:
  ssc_delta = 0x57
  ncpo1     = 0x1d00   (NO 0x1400 que upstream usa para MT7530+25MHz)
"""
import sys, os

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/drivers/net/dsa/mt7530.c"

src = open(FN).read()

if "ITER53_EN751221_TRGMII" in src:
    print("[i] iter53 ya aplicado")
    sys.exit(0)

# Modificar el bloque de detección xtal en mt7530_setup_port6
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

	/* ITER53_EN751221_TRGMII: en MCM mode dentro del SoC EN751221,
	 * MT753X_MTRAP no devuelve los XTAL bits correctos (lee 0).
	 * Con xtal=0 ncpo1 quedaba uninitialized (UB). Stock OEM eth.ko
	 * trgmii_interface_init usa ssc_delta=0x57 y ncpo1=0x1d00 — esos
	 * son los valores que necesita el TRGMII RX clock del SoC para
	 * latch correcto.
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
    sys.exit("ERROR: ancla mt7530 setup_port6 no encontrada")

src = src.replace(old, new, 1)

open(FN, "w").write(src)
print("[+] iter53 mt7530.c parchado")
