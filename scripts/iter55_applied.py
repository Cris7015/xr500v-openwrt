#!/usr/bin/env python3
"""iter55: port the full trgmii_interface_init pre-cal from the stock OEM eth.ko.

Stock OEM performs a long sequence of writes BEFORE macMT7530doP6Cal:
- PLL_GROUPx writes via MDIO (already covered by upstream mt7530_setup_port6 + iter53 patch)
- PMCR_P(5) = 0x9a30a (force-link 1G FDX, port 5)
- TRGMII RX clock toggle on 0x7a40 bit 28
- 0x7a00 |= 0x80000000 (TRGMII enable)
- Pad strength regs 0x7a54..0x7a7c
- RX delay defaults 0x7a14..0x7a34 = 0x03227700
- TX channel defaults 0x7a10..0x7a30 = 4
- 0x7830 = 1
- PMCR_P(5) = 0x9a30b (link UP)
- 0x7840 |= 0xf
- 0x250c, 0x260c = 0x000fff10
- PMCR_P(5) = 0xe430b (final force)

Without these writes, the macMT7530doP6Cal ErrChk loop receives no feedback
because the "test bus" is not enabled.
"""
import sys

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/drivers/net/dsa/mt7530.c"

src = open(FN).read()

if "ITER55_TRGMII_INIT" in src:
    print("[i] iter55 already applied")
    sys.exit(0)

# Insert the pre-cal function before mt7530_p6_cal
needle1 = "/* ITER54_P6_CAL: TRGMII RX delay calibration ported from stock OEM eth.ko\n"
if needle1 not in src:
    sys.exit("ERROR: iter54 anchor not found — apply iter54 first")

new_func = """/* ITER55_TRGMII_INIT: pre-cal setup ported from stock OEM trgmii_interface_init.
 * Writes the TRGMII pad/clock/test-bus registers needed before P6 cal can work.
 * Without this, macMT7530doP6Cal ErrChk feedback returns 0 (no test pattern
 * reflection in bits[23:16] of test reg).
 */
static void mt7530_trgmii_pre_cal_setup(struct mt7530_priv *priv)
{
\tu32 v;

\tdev_info(priv->dev, "TRGMII pre-cal setup (port 5 + pads + test bus)\\n");

\t/* PMCR_P(5) = 0x9a30a: port 5 force-link 1Gbps FDX */
\tmt7530_write(priv, MT7530_PMCR_P(5), 0x9a30a);
\tmsleep(5);

\t/* TRGMII RX clock toggle: bit 28 in 0x7a40 (TXCTRL) */
\tv = mt7530_read(priv, 0x7a40);
\tmt7530_write(priv, 0x7a40, v | BIT(28));
\tmsleep(5);
\tmt7530_write(priv, 0x7a40, v & ~BIT(28));

\t/* 0x7a00 (RCK_CTRL) bit 31: TRGMII enable / test bus on */
\tv = mt7530_read(priv, 0x7a00);
\tmt7530_write(priv, 0x7a00, v | BIT(31));

\t/* TRGMII pad strength: 0x7a54, 0x7a5c, 0x7a64, 0x7a6c, 0x7a74 = 0x88; 0x7a7c = 0x77 */
\tmt7530_write(priv, 0x7a54, 0x88);
\tmt7530_write(priv, 0x7a5c, 0x88);
\tmt7530_write(priv, 0x7a64, 0x88);
\tmt7530_write(priv, 0x7a6c, 0x88);
\tmt7530_write(priv, 0x7a74, 0x88);
\tmt7530_write(priv, 0x7a7c, 0x77);

\t/* 0x7a04 (RCK_RTT) |= 0x03020000 */
\tv = mt7530_read(priv, 0x7a04);
\tmt7530_write(priv, 0x7a04, v | 0x03020000);

\t/* RX delay defaults: 0x7a14, 0x7a1c, 0x7a24, 0x7a2c, 0x7a34 = 0x03227700 */
\tmt7530_write(priv, 0x7a14, 0x03227700);
\tmt7530_write(priv, 0x7a1c, 0x03227700);
\tmt7530_write(priv, 0x7a24, 0x03227700);
\tmt7530_write(priv, 0x7a2c, 0x03227700);
\tmt7530_write(priv, 0x7a34, 0x03227700);

\t/* TX channel defaults: 0x7a10, 0x7a18, 0x7a20, 0x7a28, 0x7a30 = 4 */
\tmt7530_write(priv, 0x7a10, 4);
\tmt7530_write(priv, 0x7a18, 4);
\tmt7530_write(priv, 0x7a20, 4);
\tmt7530_write(priv, 0x7a28, 4);
\tmt7530_write(priv, 0x7a30, 4);

\t/* TRGMII enable bit at 0x7830 */
\tmt7530_write(priv, 0x7830, 1);

\t/* PMCR_P(5) bit 0 set: link UP */
\tmt7530_write(priv, MT7530_PMCR_P(5), 0x9a30b);

\t/* 0x7840 low 4 bits: extra enables */
\tv = mt7530_read(priv, 0x7840);
\tmt7530_write(priv, 0x7840, v | 0xf);

\t/* 0x250c, 0x260c = 0x000fff10 (PMSR-related?) */
\tmt7530_write(priv, 0x250c, 0x000fff10);
\tmt7530_write(priv, 0x260c, 0x000fff10);

\t/* Final PMCR_P(5) = 0xe430b (stock value post-init) */
\tmt7530_write(priv, MT7530_PMCR_P(5), 0xe430b);

\tmsleep(5);
}

"""

src = src.replace(needle1, new_func + needle1, 1)

# Call pre-cal before mt7530_p6_cal
needle2 = """\t/* ITER54: TRGMII RX delay calibration - critical for EN751221 SoC GMAC
\t * to latch incoming frames from switch. */
\tmt7530_p6_cal(priv);"""

new_call = """\t/* ITER55: TRGMII pre-cal setup (PMCR_P5, pads, test-bus regs) */
\tmt7530_trgmii_pre_cal_setup(priv);

\t/* ITER54: TRGMII RX delay calibration - critical for EN751221 SoC GMAC
\t * to latch incoming frames from switch. */
\tmt7530_p6_cal(priv);"""

if needle2 not in src:
    sys.exit("ERROR: mt7530_p6_cal call anchor not found")

src = src.replace(needle2, new_call, 1)

open(FN, "w").write(src)
print("[+] iter55 mt7530.c patched with trgmii_interface_init pre-cal")
