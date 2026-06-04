#!/usr/bin/env python3
"""iter54: port macMT7530doP6Cal to the kernel mt7530.c."""
import sys

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/linux-6.12.80/drivers/net/dsa/mt7530.c"

src = open(FN).read()

if "ITER54_P6_CAL" in src:
    print("[i] iter54 already applied")
    sys.exit(0)

needle1 = "/* Setup port 6 interface mode and TRGMII TX circuit */\n"
if needle1 not in src:
    sys.exit("ERROR: setup_port6 comment anchor not found")

new_func = """/* ITER54_P6_CAL: TRGMII RX delay calibration ported from stock OEM eth.ko
 * macMT7530doP6Cal. Sweeps delay 1..127 per channel, finds window where test
 * pattern 0x55 reads back AND ErrChk returns 0, applies average of window.
 * Without this, SoC GMAC can't latch frames coming from switch via TRGMII.
 */
static int mt7530_p6_cal(struct mt7530_priv *priv)
{
\tstatic const u32 test_regs[4]  = {0x7a10, 0x7a18, 0x7a20, 0x7a28};
\tstatic const u32 reset_regs[4] = {0x7a50, 0x7a58, 0x7a60, 0x7a68};
\tint ch, delay;
\tint failed_channels = 0;
\tu32 v;

\tdev_info(priv->dev, "starting TRGMII P6 RX delay calibration\\n");

\t/* Enable cal mode: set bit 31 in 0x7a40 (TXCTRL) */
\tv = mt7530_read(priv, 0x7a40);
\tmt7530_write(priv, 0x7a40, v | BIT(31));

\tfor (ch = 0; ch < 4; ch++) {
\t\tu32 test_reg = test_regs[ch];
\t\tu32 reset_reg = reset_regs[ch];
\t\tint state = 1;  /* 1=search start, 2=in window, 3=window found */
\t\tint win_start = 0, win_end = 0;
\t\tu32 read_back, err;

\t\t/* Set test pattern 0x55 in reset_reg low 8 bits */
\t\tv = mt7530_read(priv, reset_reg);
\t\tmt7530_write(priv, reset_reg, (v & ~0xffu) | 0x55);

\t\tfor (delay = 1; delay < 128; delay++) {
\t\t\t/* Set delay value in test_reg low 7 bits */
\t\t\tv = mt7530_read(priv, test_reg);
\t\t\tmt7530_write(priv, test_reg, (v & ~0x7fu) | delay);

\t\t\t/* ErrChk: toggle bit 30 to trigger test */
\t\t\tv = mt7530_read(priv, test_reg);
\t\t\tmt7530_write(priv, test_reg, v | BIT(30));
\t\t\tmt7530_write(priv, test_reg, v & ~BIT(30));

\t\t\t/* Read result: err in bits[11:8], pattern in bits[23:16] */
\t\t\tv = mt7530_read(priv, test_reg);
\t\t\terr = (v >> 8) & 0xf;
\t\t\tread_back = (v >> 16) & 0xff;

\t\t\tif (state == 1) {
\t\t\t\tif (read_back == 0x55 && err == 0) {
\t\t\t\t\twin_start = delay;
\t\t\t\t\tstate = 2;
\t\t\t\t}
\t\t\t} else if (state == 2) {
\t\t\t\tif (read_back != 0x55 || err > 0) {
\t\t\t\t\twin_end = delay - 1;
\t\t\t\t\tstate = 3;
\t\t\t\t}
\t\t\t}
\t\t}

\t\tif (state == 3 && win_start != win_end) {
\t\t\tint avg = (win_start + win_end) / 2;
\t\t\tv = mt7530_read(priv, test_reg);
\t\t\tmt7530_write(priv, test_reg,
\t\t\t\t     (v & ~0x7fu) | (avg & 0x7f));
\t\t\tdev_info(priv->dev,
\t\t\t\t \"P6 cal ch%d window 0x%02x..0x%02x avg=0x%02x\\n\",
\t\t\t\t ch, win_start, win_end, avg);
\t\t} else {
\t\t\tdev_warn(priv->dev,
\t\t\t\t \"P6 cal ch%d FAILED (state=%d, start=0x%x end=0x%x)\\n\",
\t\t\t\t ch, state, win_start, win_end);
\t\t\tfailed_channels++;
\t\t}
\t}

\t/* Disable cal mode: clear bit 31 in 0x7a40 */
\tv = mt7530_read(priv, 0x7a40);
\tmt7530_write(priv, 0x7a40, v & ~BIT(31));

\tdev_info(priv->dev, \"TRGMII P6 RX cal done: %d/4 channels failed\\n\",
\t\t failed_channels);

\treturn failed_channels;
}

"""

src = src.replace(needle1, new_func + needle1, 1)

needle2 = """\t/* Enable the MT7530 TRGMII clocks */
\tcore_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_TRGMIICK_EN);
}"""

new_call = """\t/* Enable the MT7530 TRGMII clocks */
\tcore_set(priv, CORE_TRGMII_GSW_CLK_CG, REG_TRGMIICK_EN);

\t/* ITER54: TRGMII RX delay calibration - critical for EN751221 SoC GMAC
\t * to latch incoming frames from switch. */
\tmt7530_p6_cal(priv);
}"""

if needle2 not in src:
    sys.exit("ERROR: setup_port6 end anchor not found")

src = src.replace(needle2, new_call, 1)

open(FN, "w").write(src)
print("[+] iter54 mt7530.c patched with macMT7530doP6Cal port")
