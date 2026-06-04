#!/usr/bin/env python3
"""iter58: port GE PHY analog cal from the 2.6 SDK kernel to kernel 6.12.

Source: cjdelisle/EN751221-Linux26 tcetherphy_7512.c lines 8569-9105 (doGePhyALLAnalogCal + GECal_Rext)

API mapping kernel 2.6 SDK -> kernel 6.12:
- tcMiiStationWrite(phy, reg, val)  -> phy_write(phydev, reg, val)
- tcMiiStationRead(phy, reg)         -> phy_read(phydev, reg)
- mtEMiiRegWrite(phy, dev, reg, val) -> phy_write_mmd(phydev, dev, reg, val)
- mtEMiiRegRead(phy, dev, reg)       -> phy_read_mmd(phydev, dev, reg)
- udelay(N)                          -> udelay(N)
- regReadWord/regWriteWord chipscu   -> SKIPPED (ACD/simldo tweak, optional)

Inserts the cal code into gsw/mt7530.c, called from en751221_setup_port
when the PHY is first configured.
"""
import sys

FN = "/home/cristuu/openwrt/build_dir/target-mips_24kc_musl/linux-econet_en751221/econet-eth-2026.02.13~c2f855cf/gsw/mt7530.c"

src = open(FN).read()

if "ITER58_PHY_ANALOG_CAL" in src:
    print("[i] iter58 already applied")
    sys.exit(0)

# Insert the cal code BEFORE en751221_setup_port (which calls them)
needle = "/* Setup ports 0..=4 on EN751221 switch."
if needle not in src:
    sys.exit("ERROR: en751221_setup_port anchor not found")

new_block = r'''/* === ITER58_PHY_ANALOG_CAL ============================================
 * GE PHY analog calibration ported from cjdelisle/EN751221-Linux26 SDK
 * (kernel 2.6 tcetherphy_7512.c lines 8569+).
 *
 * Without this calibration, the PHY auto-negotiates link UP but TX signal
 * is electrically out of spec (wrong impedance, amplitude, offset).  Frames
 * get corrupted at the physical layer, so the receiving end (SoC GMAC via
 * TRGMII for our switch port@6) drops them and cdm_rxcpu stays at 0.
 *
 * Cal sequence per PHY:
 *   1. Rext cal — calibrates external reference resistor (sweep zcal_ctrl
 *      until comparator flips, store result)
 *   2. R50 cal — calibrates 50-ohm termination per pair A/B/C/D
 *   3. Tx offset cal — calibrates DAC offset per pair
 *   4. Tx amp cal — calibrates TX amplitude per pair
 *   5. Rx offset cal — HW-driven, single trigger
 */

#define EN_ANACAL_INIT       0x01
#define EN_ANACAL_ERROR      0xFD
#define EN_ANACAL_SATURATION 0xFE
#define EN_ANACAL_FINISH     0xFF

#define EN_DAC_IN_0V         0x000
#define EN_DAC_IN_2V         0x0f0

static const u8 ZCAL_TO_R50ohm_TBL_100[64] = {
	127, 127, 127, 127, 127, 127, 126, 123, 120, 117, 114, 112, 110, 107, 105, 103,
	101, 99,  97,  79,  77,  75,  74,  72,  70,  69,  67,  66,  65,  47,  46,  45,
	43,  42,  41,  40,  39,  38,  37,  36,  34,  34,  33,  32,  15,  14,  13,  12,
	11,  10,  10,  9,   8,   7,   7,   6,   5,   4,   4,   3,   2,   2,   1,   1,
};

static u8 en751221_anacal_wait(struct phy_device *phydev, u32 delay)
{
	int v;

	phy_write_mmd(phydev, 0x1e, 0x017c, 0x0001); /* da_calin_flag high */
	udelay(delay);
	v = phy_read_mmd(phydev, 0x1e, 0x017b);
	phy_write_mmd(phydev, 0x1e, 0x017c, 0x0000); /* da_calin_flag low */
	if (v < 0)
		return 0;
	return (u8)(v & 0x1);
}

/* Returns 0 on success, 1 if cal needs retry (caller should loop) */
static int en751221_phy_cal_rext(struct phy_device *phydev)
{
	u8 status, zcal_ctrl, cnt = 0;
	int polarity, comp_init, v;

	phy_write_mmd(phydev, 0x1e, 0x00db, 0x1110);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0000);
	phy_write_mmd(phydev, 0x1e, 0x00e1, 0x0000);

	zcal_ctrl = 0x20;
	phy_write_mmd(phydev, 0x1e, 0x00e0, zcal_ctrl);

	status = en751221_anacal_wait(phydev, 100);
	if (status == 0) {
		phydev_warn(phydev, "Rext cal: initial wait fail\n");
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x0000);
		return 1;
	}

	v = phy_read_mmd(phydev, 0x1e, 0x017a);
	comp_init = (v >> 8) & 0x1;
	polarity = comp_init ? -1 : 1;

	while (status < EN_ANACAL_ERROR) {
		cnt++;
		zcal_ctrl += polarity;
		phy_write_mmd(phydev, 0x1e, 0x00e0, zcal_ctrl);
		status = en751221_anacal_wait(phydev, 100);

		if (status == 0) {
			status = EN_ANACAL_ERROR;
			break;
		}
		v = phy_read_mmd(phydev, 0x1e, 0x017a);
		if (((v >> 8) & 0x1) != comp_init) {
			status = EN_ANACAL_FINISH;
			break;
		}
		if (zcal_ctrl == 0x3F || zcal_ctrl == 0x00) {
			status = EN_ANACAL_SATURATION;
			break;
		}
	}

	if (status == EN_ANACAL_ERROR) {
		zcal_ctrl = 0x20;
		phy_write_mmd(phydev, 0x1e, 0x00e0, zcal_ctrl);
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x0000);
		return 1;
	}

	phy_write_mmd(phydev, 0x1e, 0x00e0, zcal_ctrl);
	phy_write_mmd(phydev, 0x1e, 0x00e0, ((u16)zcal_ctrl << 8) | zcal_ctrl);
	phy_write_mmd(phydev, 0x1f, 0x0115, (zcal_ctrl & 0x3f) >> 3);
	phy_write_mmd(phydev, 0x1e, 0x00db, 0x0000);

	phydev_info(phydev, "Rext cal done (cnt=%d zcal=0x%02x)\n", cnt, zcal_ctrl);
	return 0;
}

static void en751221_phy_cal_r50_pair(struct phy_device *phydev, int pair)
{
	u8 status, zcal_ctrl, cnt = 0;
	int polarity, comp_init, v;
	u16 dev1e_e0_r5;

	zcal_ctrl = 0x20;
	v = phy_read_mmd(phydev, 0x1e, 0x00e0);
	dev1e_e0_r5 = (v < 0 ? 0 : v) & ~0x003f;
	phy_write_mmd(phydev, 0x1e, 0x00e0, dev1e_e0_r5 | zcal_ctrl);

	switch (pair) {
	case 0:
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x1101);
		phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0000);
		break;
	case 1:
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x1100);
		phy_write_mmd(phydev, 0x1e, 0x00dc, 0x1000);
		break;
	case 2:
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x1100);
		phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0100);
		break;
	default:
		phy_write_mmd(phydev, 0x1e, 0x00db, 0x1100);
		phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0010);
		break;
	}

	status = en751221_anacal_wait(phydev, 20);
	if (status == 0) {
		phydev_warn(phydev, "R50 pair %c: initial wait fail\n", 'A' + pair);
		return;
	}

	v = phy_read_mmd(phydev, 0x1e, 0x017a);
	comp_init = (v >> 8) & 0x1;
	polarity = comp_init ? -1 : 1;

	while (status < EN_ANACAL_ERROR) {
		cnt++;
		zcal_ctrl += polarity;
		phy_write_mmd(phydev, 0x1e, 0x00e0, dev1e_e0_r5 | zcal_ctrl);
		status = en751221_anacal_wait(phydev, 20);
		if (status == 0) {
			status = EN_ANACAL_ERROR;
			break;
		}
		v = phy_read_mmd(phydev, 0x1e, 0x017a);
		if (((v >> 8) & 0x1) != comp_init) {
			status = EN_ANACAL_FINISH;
			break;
		}
		if (zcal_ctrl == 0x3F || zcal_ctrl == 0x00) {
			status = EN_ANACAL_SATURATION;
			break;
		}
	}

	if (status == EN_ANACAL_ERROR)
		return;

	zcal_ctrl = ZCAL_TO_R50ohm_TBL_100[zcal_ctrl] - 2;

	switch (pair) {
	case 0:
		v = phy_read_mmd(phydev, 0x1e, 0x0174) & ~0x7f00;
		phy_write_mmd(phydev, 0x1e, 0x0174,
			      v | (((u16)zcal_ctrl << 8) & 0xff00) | 0x8000);
		break;
	case 1:
		v = phy_read_mmd(phydev, 0x1e, 0x0174) & ~0x007f;
		phy_write_mmd(phydev, 0x1e, 0x0174,
			      v | (zcal_ctrl & 0x00ff) | 0x0080);
		break;
	case 2:
		v = phy_read_mmd(phydev, 0x1e, 0x0175) & ~0x7f00;
		phy_write_mmd(phydev, 0x1e, 0x0175,
			      v | (((u16)zcal_ctrl << 8) & 0xff00) | 0x8000);
		break;
	default:
		v = phy_read_mmd(phydev, 0x1e, 0x0175) & ~0x007f;
		phy_write_mmd(phydev, 0x1e, 0x0175,
			      v | (zcal_ctrl & 0x00ff) | 0x0080);
		break;
	}
	phydev_info(phydev, "R50 pair %c done (cnt=%d zcal=0x%02x)\n",
		    'A' + pair, cnt, zcal_ctrl);
}

static void en751221_phy_cal_tx_offset_pair(struct phy_device *phydev, int pair)
{
	static const u16 dac_in_regs[4][2] = {
		{ 0x017d, 0x0181 },
		{ 0x017e, 0x0182 },
		{ 0x017f, 0x0183 },
		{ 0x0180, 0x0184 },
	};
	static const u16 dd_bits[4]   = { 0x1000, 0x0100, 0x0010, 0x0001 };
	static const u16 ofs_regs[4]  = { 0x0172, 0x0172, 0x0173, 0x0173 };
	static const u8  ofs_shifts[4] = { 8, 0, 8, 0 };
	int polarity, comp_init, tx_offset_temp, v;
	u16 reg_temp, mask, cal_temp;
	u8 status, cnt = 0;

	tx_offset_temp = 0;

	phy_write_mmd(phydev, 0x1e, 0x00dd, dd_bits[pair]);
	phy_write_mmd(phydev, 0x1e, dac_in_regs[pair][0], 0x8000 | EN_DAC_IN_0V);
	phy_write_mmd(phydev, 0x1e, dac_in_regs[pair][1], 0x8000 | EN_DAC_IN_0V);

	mask = 0x3f << ofs_shifts[pair];
	v = phy_read_mmd(phydev, 0x1e, ofs_regs[pair]);
	reg_temp = (v < 0 ? 0 : v) & ~mask;
	phy_write_mmd(phydev, 0x1e, ofs_regs[pair],
		      reg_temp | ((u16)tx_offset_temp << ofs_shifts[pair]));

	status = en751221_anacal_wait(phydev, 20);
	if (status == 0) {
		phydev_warn(phydev, "Tx offset pair %c: initial wait fail\n", 'A' + pair);
		return;
	}

	v = phy_read_mmd(phydev, 0x1e, 0x017a);
	comp_init = (v >> 8) & 0x1;
	polarity = comp_init ? -1 : 1;

	while (status < EN_ANACAL_ERROR) {
		cnt++;
		tx_offset_temp += polarity;
		if (tx_offset_temp >= 0)
			cal_temp = tx_offset_temp;
		else
			cal_temp = (1 << 5) | abs(tx_offset_temp);
		phy_write_mmd(phydev, 0x1e, ofs_regs[pair],
			      reg_temp | ((u16)cal_temp << ofs_shifts[pair]));
		status = en751221_anacal_wait(phydev, 20);
		if (status == 0) {
			status = EN_ANACAL_ERROR;
			break;
		}
		v = phy_read_mmd(phydev, 0x1e, 0x017a);
		if (((v >> 8) & 0x1) != comp_init) {
			status = EN_ANACAL_FINISH;
			break;
		}
		if (tx_offset_temp == -31 || tx_offset_temp == 31) {
			status = EN_ANACAL_SATURATION;
			break;
		}
	}

	if (status == EN_ANACAL_ERROR) {
		tx_offset_temp = 0;
		phy_write_mmd(phydev, 0x1e, ofs_regs[pair],
			      reg_temp | ((u16)tx_offset_temp << ofs_shifts[pair]));
		return;
	}
	phydev_info(phydev, "Tx offset pair %c done (cnt=%d val=0x%02x)\n",
		    'A' + pair, cnt, cal_temp);
}

static void en751221_phy_cal_tx_amp_pair(struct phy_device *phydev, int pair)
{
	static const u16 dac_in_regs[4][2] = {
		{ 0x017d, 0x0181 },
		{ 0x017e, 0x0182 },
		{ 0x017f, 0x0183 },
		{ 0x0180, 0x0184 },
	};
	static const u16 dd_bits[4]      = { 0x1000, 0x0100, 0x0010, 0x0001 };
	static const u16 amp_regs[4]     = { 0x0012, 0x0017, 0x0019, 0x0021 };
	static const u16 amp_regs_100[4] = { 0x0016, 0x0018, 0x0020, 0x0022 };
	static const u16 mask_a          = 0xfc00;
	static const u16 mask_bcd        = 0x3f00;
	static const u8 amp_shifts[4]    = { 10, 8, 8, 8 };
	int polarity, comp_init, v;
	u8 status, tx_amp_temp, cnt = 0;
	u16 reg_temp, mask;

	tx_amp_temp = 0x20;

	phy_write_mmd(phydev, 0x1e, 0x00dd, dd_bits[pair]);
	phy_write_mmd(phydev, 0x1e, dac_in_regs[pair][0], 0x8000 | EN_DAC_IN_2V);
	phy_write_mmd(phydev, 0x1e, dac_in_regs[pair][1], 0x8000 | EN_DAC_IN_2V);

	mask = (pair == 0) ? mask_a : mask_bcd;
	v = phy_read_mmd(phydev, 0x1e, amp_regs[pair]);
	reg_temp = (v < 0 ? 0 : v) & ~mask;
	phy_write_mmd(phydev, 0x1e, amp_regs[pair],
		      reg_temp | ((u16)tx_amp_temp << amp_shifts[pair]));

	status = en751221_anacal_wait(phydev, 20);
	if (status == 0) {
		phydev_warn(phydev, "Tx amp pair %c: initial wait fail\n", 'A' + pair);
		return;
	}

	v = phy_read_mmd(phydev, 0x1e, 0x017a);
	comp_init = (v >> 8) & 0x1;
	polarity = comp_init ? -1 : 1;

	while (status < EN_ANACAL_ERROR) {
		cnt++;
		tx_amp_temp += polarity;
		phy_write_mmd(phydev, 0x1e, amp_regs[pair],
			      reg_temp | ((u16)tx_amp_temp << amp_shifts[pair]));
		status = en751221_anacal_wait(phydev, 100);
		if (status == 0) {
			status = EN_ANACAL_ERROR;
			break;
		}
		v = phy_read_mmd(phydev, 0x1e, 0x017a);
		if (((v >> 8) & 0x1) != comp_init) {
			status = EN_ANACAL_FINISH;
			v = phy_read_mmd(phydev, 0x1e, amp_regs[pair]);
			reg_temp = (v < 0 ? 0 : v) & ~0xff00;
			phy_write_mmd(phydev, 0x1e, amp_regs[pair],
				      reg_temp | (((u16)(tx_amp_temp + 0x2)) << amp_shifts[pair]));
			break;
		}
		if (tx_amp_temp == 0x3f || tx_amp_temp == 0x00) {
			status = EN_ANACAL_SATURATION;
			break;
		}
	}

	if (status == EN_ANACAL_ERROR) {
		tx_amp_temp = 0x20;
		phy_write_mmd(phydev, 0x1e, amp_regs[pair],
			      reg_temp | ((u16)tx_amp_temp << amp_shifts[pair]));
		return;
	}

	v = phy_read_mmd(phydev, 0x1e, amp_regs_100[pair]);
	reg_temp = (v < 0 ? 0 : v) & ~0x003f;
	if (status == EN_ANACAL_SATURATION || tx_amp_temp == 0)
		phy_write_mmd(phydev, 0x1e, amp_regs_100[pair], reg_temp | 0x0010);
	else
		phy_write_mmd(phydev, 0x1e, amp_regs_100[pair],
			      reg_temp | (tx_amp_temp + 14));

	v = phy_read_mmd(phydev, 0x1e, amp_regs_100[pair]);
	reg_temp = (v < 0 ? 0 : v) & ~0xff00;
	if (status == EN_ANACAL_SATURATION || tx_amp_temp == 0)
		phy_write_mmd(phydev, 0x1e, amp_regs_100[pair],
			      reg_temp | (0x4 << amp_shifts[pair]));
	else
		phy_write_mmd(phydev, 0x1e, amp_regs_100[pair],
			      reg_temp | (((u16)(tx_amp_temp + 0x9)) << amp_shifts[pair]));

	phydev_info(phydev, "Tx amp pair %c done (cnt=%d val=0x%02x)\n",
		    'A' + pair, cnt, tx_amp_temp);
}

static void en751221_phy_analog_cal(struct phy_device *phydev)
{
	int reg0_temp, dev1e_145_temp, retry, pair;

	phydev_info(phydev, "ITER58: starting GE PHY analog cal\n");

	phy_write(phydev, 0x1f, 0x0000); /* page 0 */
	reg0_temp = phy_read(phydev, MII_BMCR);
	phy_write(phydev, MII_BMCR, 0x0140); /* AN dis, FD, 1G */

	phy_write_mmd(phydev, 0x1f, 0x0100, 0xc000);
	dev1e_145_temp = phy_read_mmd(phydev, 0x1e, 0x0145);
	phy_write_mmd(phydev, 0x1e, 0x0145, 0x1010);
	phy_write_mmd(phydev, 0x1e, 0x0185, 0x0000);

	/* Rext cal — retry up to 5 times if needed */
	for (retry = 0; retry < 5; retry++) {
		if (en751221_phy_cal_rext(phydev) == 0)
			break;
		phydev_warn(phydev, "Rext cal retry %d\n", retry + 1);
	}

	/* R50 cal pairs A..D */
	phy_write_mmd(phydev, 0x1e, 0x00db, 0x1100);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0000);
	for (pair = 0; pair < 4; pair++)
		en751221_phy_cal_r50_pair(phydev, pair);
	phy_write_mmd(phydev, 0x1e, 0x00db, 0x0000);

	/* Tx offset cal */
	phy_write_mmd(phydev, 0x1e, 0x00db, 0x0100);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0001);
	phy_write_mmd(phydev, 0x1e, 0x0096, 0x8000);
	phy_write_mmd(phydev, 0x1e, 0x003e, 0xf808);
	for (pair = 0; pair < 4; pair++)
		en751221_phy_cal_tx_offset_pair(phydev, pair);
	phy_write_mmd(phydev, 0x1e, 0x017d, 0);
	phy_write_mmd(phydev, 0x1e, 0x017e, 0);
	phy_write_mmd(phydev, 0x1e, 0x017f, 0);
	phy_write_mmd(phydev, 0x1e, 0x0180, 0);
	phy_write_mmd(phydev, 0x1e, 0x0181, 0);
	phy_write_mmd(phydev, 0x1e, 0x0182, 0);
	phy_write_mmd(phydev, 0x1e, 0x0183, 0);
	phy_write_mmd(phydev, 0x1e, 0x0184, 0);
	phy_write_mmd(phydev, 0x1e, 0x00db, 0);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0);
	phy_write_mmd(phydev, 0x1e, 0x003e, 0);
	phy_write_mmd(phydev, 0x1e, 0x00dd, 0);

	/* Tx amp cal */
	phy_write_mmd(phydev, 0x1e, 0x00db, 0x1100);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0x0001);
	phy_write_mmd(phydev, 0x1e, 0x00e1, 0x0010);
	phy_write_mmd(phydev, 0x1e, 0x003e, 0xf808);
	for (pair = 0; pair < 4; pair++)
		en751221_phy_cal_tx_amp_pair(phydev, pair);
	phy_write_mmd(phydev, 0x1e, 0x017d, 0);
	phy_write_mmd(phydev, 0x1e, 0x017e, 0);
	phy_write_mmd(phydev, 0x1e, 0x017f, 0);
	phy_write_mmd(phydev, 0x1e, 0x0180, 0);
	phy_write_mmd(phydev, 0x1e, 0x0181, 0);
	phy_write_mmd(phydev, 0x1e, 0x0182, 0);
	phy_write_mmd(phydev, 0x1e, 0x0183, 0);
	phy_write_mmd(phydev, 0x1e, 0x0184, 0);
	phy_write_mmd(phydev, 0x1e, 0x00db, 0);
	phy_write_mmd(phydev, 0x1e, 0x00dc, 0);
	phy_write_mmd(phydev, 0x1e, 0x003e, 0);
	phy_write_mmd(phydev, 0x1e, 0x00dd, 0);

	/* Rx offset cal — HW driven, single trigger */
	{
		int v;
		phy_write_mmd(phydev, 0x1e, 0x0096, 0x8000);
		phy_write_mmd(phydev, 0x1e, 0x0037, 0x0033);
		v = phy_read_mmd(phydev, 0x1e, 0x0039);
		phy_write_mmd(phydev, 0x1e, 0x0039, (v < 0 ? 0 : v) & ~0x4800);
		v = phy_read_mmd(phydev, 0x1f, 0x0107);
		phy_write_mmd(phydev, 0x1f, 0x0107, (v < 0 ? 0 : v) & ~0x1000);
		v = phy_read_mmd(phydev, 0x1e, 0x0171);
		phy_write_mmd(phydev, 0x1e, 0x0171, ((v < 0 ? 0 : v) & ~0x0180) | 0x0180);
		v = phy_read_mmd(phydev, 0x1e, 0x0039);
		phy_write_mmd(phydev, 0x1e, 0x0039, (v < 0 ? 0 : v) | 0x2000);
		phy_write_mmd(phydev, 0x1e, 0x0039, (v < 0 ? 0 : v) & ~0x2000);
		mdelay(10);
		v = phy_read_mmd(phydev, 0x1e, 0x0171);
		phy_write_mmd(phydev, 0x1e, 0x0171, (v < 0 ? 0 : v) & ~0x0180);
	}

	/* Restore original BMCR + a few regs */
	if (reg0_temp >= 0)
		phy_write(phydev, MII_BMCR, reg0_temp);
	phy_write_mmd(phydev, 0x1f, 0x0100, 0x0000);
	if (dev1e_145_temp >= 0)
		phy_write_mmd(phydev, 0x1e, 0x0145, dev1e_145_temp);

	phydev_info(phydev, "ITER58: GE PHY analog cal complete\n");
}
/* === END ITER58_PHY_ANALOG_CAL === */

'''
src = src.replace(needle, new_block + needle, 1)

# Add a call to en751221_phy_analog_cal in en751221_setup_port
old_setup = """static void
en751221_setup_port(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dp = dsa_to_port(ds, port);
	struct phy_device *phy = dp->user->phydev;

	if (!phy)
		return;

	/* This is mostly undocumented magic. */

	phy_write(phy, MII_BMCR, BMCR_RESET);"""
new_setup = """static void
en751221_setup_port(struct dsa_switch *ds, int port)
{
	struct dsa_port *dp = dp = dsa_to_port(ds, port);
	struct phy_device *phy = dp->user->phydev;
	static unsigned long en751221_phy_cal_done; /* bitmask of ports done */

	if (!phy)
		return;

	/* ITER58: run analog cal once per port (Rext, R50, Tx offset/amp, Rx offset).
	 * Without this, link comes UP but TX signal is electrically out of spec
	 * and frames are corrupted at the cable, dropped by receiver. */
	if (!(en751221_phy_cal_done & BIT(port))) {
		en751221_phy_analog_cal(phy);
		en751221_phy_cal_done |= BIT(port);
	}

	/* This is mostly undocumented magic. */

	phy_write(phy, MII_BMCR, BMCR_RESET);"""
assert old_setup in src, "en751221_setup_port body anchor not found"
src = src.replace(old_setup, new_setup, 1)

open(FN, "w").write(src)
print("[+] iter58 mt7530.c patched with GE PHY analog cal")
