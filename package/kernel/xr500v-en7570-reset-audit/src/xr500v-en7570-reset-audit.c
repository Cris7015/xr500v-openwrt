// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, non-transactional EN7570 reset observation for the XR500v.
 *
 * This driver has no matching shipping DT node and no autoload entry. It
 * accepts only the exact phase-12 retained baseline, asserts physical
 * TX_DISABLE, verifies all xPON transmit paths are inactive, performs the OEM
 * four-byte SW_RESET RMW once, then exposes before/after snapshots. Separate
 * mutually-exclusive modes may subsequently apply the OEM LOS receiver
 * sequence, its single RSSI-gain prerequisite, or the bounded transient RSSI
 * calibration followed by gain and LOS. It does no bandgap ADC calibration,
 * APD, TGEN, current, interrupt or rollback.
 */

#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/delay.h>

#define PHYSET3			0x0108
#define PHYSET3_TXEN		BIT(5)
#define MISC			0x01fc
#define MISC_ROGUE_TX_TEST	BIT(28)
#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_INT_EN		0x05f0

#define EN7570_ID_REG		0x0170
#define EN7570_EXPECTED_ID	0x03
#define EN7570_LA_PWD		0x0014
#define EN7570_SVADC_PD		0x0024
#define EN7570_SAFE_PROTECT	0x0100
#define EN7570_LOS_CTRL1		0x011c
#define EN7570_LOS_CTRL2		0x0120
#define EN7570_ADC_PROBE_STATUS	0x0154
#define EN7570_PROBE_CONTROL	0x0158
#define EN7570_ADC_LATCH		0x0159
#define EN7570_ROGUE_TX		0x0168
#define EN7570_SW_RESET		0x0300

#define EN7570_LOS_CAL_TRIG	BIT(0)
#define EN7570_LOS_STABLE_MASK	GENMASK(4, 0)
#define EN7570_LOS_CONF_MASK	GENMASK(4, 0)
#define EN7570_LOS_COUNT_MASK	GENMASK(6, 0)
#define EN7570_LOS_THRESHOLD_MASK GENMASK(6, 0)
#define EN7570_ADC_REV2_ENABLE	BIT(2)
#define EN7570_ADC_REV1_ENABLE	BIT(6)
#define EN7570_RSSI_GAIN_MASK	GENMASK(2, 0)
#define EN7570_RSSI_GAIN_NORMAL	0x05
#define EN7570_RSSI_CAL_ENABLE	BIT(4)
#define EN7570_RSSI_VMODE_ENABLE	BIT(6)
#define EN7570_ADC_SELECT_MASK	0x1e
#define EN7570_ADC_RSSI_ENABLE	BIT(1)
#define EN7570_ADC_LATCH_ENABLE	BIT(4)
#define EN7570_RSSI_VREF_MIN	0x01e0
#define EN7570_RSSI_VREF_MAX	0x0230
#define EN7570_RSSI_V_MIN	0x0250
#define EN7570_RSSI_V_MAX	0x02c0
#define EN7570_RSSI_DELTA_MIN	0x0060
#define EN7570_RSSI_DELTA_MAX	0x00a0

struct en7570_audit_reg {
	const char *name;
	u16 reg;
	u8 expected[4];
};

static const struct en7570_audit_reg audit_regs[] = {
	{ "tiamux", 0x0000, { 0x08, 0x00, 0x10, 0x02 } },
	{ "mpd_targets", 0x0004, { 0x00, 0x02, 0x00, 0x00 } },
	{ "t1delay", 0x0008, { 0x99, 0x00, 0x00, 0x20 } },
	{ "la_pwd", 0x0014, { 0x00, 0x24, 0x00, 0x00 } },
	{ "bgcken", 0x001c, { 0x55, 0x55, 0x55, 0xa5 } },
	{ "pi_tgen", 0x0020, { 0x06, 0x00, 0x00, 0x00 } },
	{ "svadc_pd", EN7570_SVADC_PD, { 0x00, 0x00, 0x01, 0x00 } },
	{ "apd_dac", 0x0030, { 0x00, 0x08, 0x00, 0x00 } },
	{ "safe_protect", 0x0100, { 0xff, 0x8f, 0x00, 0x00 } },
	{ "los_ctrl1", 0x011c, { 0x06, 0x08, 0x3c, 0x36 } },
	{ "los_ctrl2", 0x0120, { 0x10, 0x05, 0x00, 0x00 } },
	{ "los_timer", 0x0124, { 0xff, 0xff, 0xff, 0xff } },
	{ "los_timeout_count", 0x0128, { 0x00, 0x00, 0x00, 0x00 } },
	{ "los_timeout", 0x012c, { 0x00, 0x00, 0x00, 0x00 } },
	{ "p0_cs1", 0x0134, { 0x00, 0x02, 0x04, 0x10 } },
	{ "p0_cs2", 0x0138, { 0x00, 0x00, 0x00, 0x00 } },
	{ "p0_cs3", 0x013c, { 0x30, 0x12, 0x00, 0x10 } },
	{ "p0_latch", 0x0140, { 0x00, 0x00, 0x00, 0x00 } },
	{ "p1_cs1", 0x0144, { 0x00, 0x02, 0x04, 0x10 } },
	{ "p1_cs2", 0x0148, { 0x00, 0x00, 0x00, 0x00 } },
	{ "p1_cs3", 0x014c, { 0x30, 0x12, 0x00, 0x10 } },
	{ "p1_latch", 0x0150, { 0x00, 0x00, 0x00, 0x00 } },
	{ "adc_probe", EN7570_ADC_PROBE_STATUS,
	  { 0x00, 0x00, 0x00, 0x00 } },
	{ "probe_control", EN7570_PROBE_CONTROL, { 0x00, 0x00, 0x00, 0x00 } },
	{ "apd_ovp_latch", 0x0164, { 0x00, 0x00, 0x00, 0x00 } },
	{ "rogue_tx", 0x0168, { 0x34, 0x02, 0x00, 0x00 } },
	{ "erc_filter", 0x016c, { 0x3f, 0x2f, 0x0f, 0x00 } },
	{ "sw_reset", 0x0300, { 0x00, 0x00, 0x00, 0x00 } },
};

struct xr500v_reset_audit {
	struct device *dev;
	void __iomem *base;
	struct gpio_desc *tx_disable;
	struct i2c_client *en7570;
	struct dentry *debugfs_dir;
	u8 before[ARRAY_SIZE(audit_regs)][4];
	u8 after[ARRAY_SIZE(audit_regs)][4];
	u8 after_rssi[ARRAY_SIZE(audit_regs)][4];
	u8 after_los[ARRAY_SIZE(audit_regs)][4];
	u8 rssi_cal_la_pwd[4];
	u8 rssi_cal_svadc_pd[4];
	u8 rssi_cal_adc_probe[4];
	u8 rssi_cal_probe_control[4];
	u8 rssi_gain_la_pwd[4];
	u8 reset_write[4];
	unsigned int i2c_write_attempts;
	int reset_result;
	int post_snapshot_result;
	int postflight_result;
	int rssi_cal_result;
	int rssi_cal_readback_result;
	int rssi_cal_verify_result;
	int rssi_cal_postflight_result;
	int rssi_result;
	int rssi_readback_result;
	int rssi_snapshot_result;
	int rssi_verify_result;
	int rssi_postflight_result;
	int los_result;
	int los_snapshot_result;
	int los_postflight_result;
	bool module_pinned;
	bool reset_then_los;
	bool rssi_gain;
	bool rssi_calibration;
	bool rssi_cal_attempted;
	bool rssi_attempted;
	bool los_attempted;
	u8 los_high_threshold;
	u8 los_low_threshold;
	u16 rssi_vref;
	u16 rssi_v;
};

static bool arm_en7570_reset_audit;
module_param(arm_en7570_reset_audit, bool, 0444);
MODULE_PARM_DESC(arm_en7570_reset_audit,
		 "Arm the one-shot non-transactional EN7570 reset observation");

static bool arm_en7570_reset_then_los;
module_param(arm_en7570_reset_then_los, bool, 0444);
MODULE_PARM_DESC(arm_en7570_reset_then_los,
		 "Arm one-shot EN7570 reset followed by isolated OEM LOS receiver setup");

static bool arm_en7570_reset_rssi_los;
module_param(arm_en7570_reset_rssi_los, bool, 0444);
MODULE_PARM_DESC(arm_en7570_reset_rssi_los,
		 "Arm one-shot EN7570 reset, OEM RSSI gain, then isolated LOS setup");

static bool arm_en7570_reset_rssi_cal_los;
module_param(arm_en7570_reset_rssi_cal_los, bool, 0444);
MODULE_PARM_DESC(arm_en7570_reset_rssi_cal_los,
		 "Arm one-shot EN7570 reset, transient RSSI calibration/gain, then LOS");

static int audit_preflight(struct xr500v_reset_audit *audit);

static u32 xpon_read(struct xr500v_reset_audit *audit, u32 reg)
{
	return ioread32(audit->base + reg);
}

static int en7570_read(struct i2c_client *client, u16 reg, void *value,
		       int length)
{
	u8 pointer[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[2] = {
		{ .addr = client->addr, .len = sizeof(pointer), .buf = pointer },
		{ .addr = client->addr, .flags = I2C_M_RD,
		  .len = length, .buf = value },
	};
	int ret;

	ret = i2c_transfer(client->adapter, messages, ARRAY_SIZE(messages));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(messages) ? 0 : -EIO;
}

static int en7570_reset_write(struct xr500v_reset_audit *audit,
			      const u8 value[4])
{
	u8 data[6] = { EN7570_SW_RESET >> 8, EN7570_SW_RESET & 0xff };
	struct i2c_msg message = {
		.addr = audit->en7570->addr,
		.len = sizeof(data),
		.buf = data,
	};
	int ret;

	ret = audit_preflight(audit);
	if (ret)
		return ret;

	memcpy(data + 2, value, 4);
	audit->i2c_write_attempts++;
	ret = i2c_transfer(audit->en7570->adapter, &message, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
}

static int en7570_rx_write(struct xr500v_reset_audit *audit, u16 reg,
			   const u8 value[4])
{
	u8 data[6] = { reg >> 8, reg & 0xff };
	struct i2c_msg message = {
		.addr = audit->en7570->addr,
		.len = sizeof(data),
		.buf = data,
	};
	int ret;

	if (reg != EN7570_LA_PWD && reg != EN7570_LOS_CTRL1 && reg != 0x0024 &&
	    reg != EN7570_LOS_CTRL2)
		return -EINVAL;
	ret = audit_preflight(audit);
	if (ret)
		return ret;

	memcpy(data + 2, value, 4);
	audit->i2c_write_attempts++;
	ret = i2c_transfer(audit->en7570->adapter, &message, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static int en7570_cal_write(struct xr500v_reset_audit *audit, u16 reg,
			    const u8 *value, u8 length)
{
	u8 data[6] = { reg >> 8, reg & 0xff };
	struct i2c_msg message = {
		.addr = audit->en7570->addr,
		.len = 2 + length,
		.buf = data,
	};
	int ret;

	if (!((reg == EN7570_LA_PWD && length == 2) ||
	      (reg == EN7570_SVADC_PD && length == 1) ||
	      (reg == EN7570_ADC_LATCH && length == 1)))
		return -EINVAL;
	ret = audit_preflight(audit);
	if (ret)
		return ret;

	memcpy(data + 2, value, length);
	audit->i2c_write_attempts++;
	ret = i2c_transfer(audit->en7570->adapter, &message, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static u8 *audit_value(u8 values[][4], u16 reg)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(audit_regs); i++)
		if (audit_regs[i].reg == reg)
			return values[i];

	return NULL;
}

static int audit_apply_rssi_calibration(struct xr500v_reset_audit *audit)
{
	u8 adc[2];
	u8 la_pwd[2];
	u8 svadc;
	u8 svadc_original;
	u8 latch;
	u16 delta;
	int ret;

	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	if (la_pwd[0] != 0x00 || la_pwd[1] != 0x24)
		return -EPERM;
	ret = en7570_read(audit->en7570, EN7570_SVADC_PD,
			  &svadc, sizeof(svadc));
	if (ret)
		return ret;
	svadc_original = svadc;
	if (svadc_original != 0x00)
		return -EPERM;
	ret = en7570_read(audit->en7570, EN7570_ADC_LATCH,
			  &latch, sizeof(latch));
	if (ret)
		return ret;
	if (latch)
		return -EPERM;

	la_pwd[1] |= EN7570_RSSI_CAL_ENABLE;
	ret = en7570_cal_write(audit, EN7570_LA_PWD,
			       la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;

	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	if (la_pwd[0] != 0x00 || la_pwd[1] != 0x34)
		return -EIO;
	la_pwd[1] |= EN7570_RSSI_VMODE_ENABLE;
	ret = en7570_cal_write(audit, EN7570_LA_PWD,
			       la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;

	svadc = (svadc & ~EN7570_ADC_SELECT_MASK) |
		EN7570_ADC_RSSI_ENABLE;
	ret = en7570_cal_write(audit, EN7570_SVADC_PD,
			       &svadc, sizeof(svadc));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_SVADC_PD,
			  &svadc, sizeof(svadc));
	if (ret)
		return ret;
	if (svadc != EN7570_ADC_RSSI_ENABLE)
		return -EIO;

	latch |= EN7570_ADC_LATCH_ENABLE;
	ret = en7570_cal_write(audit, EN7570_ADC_LATCH,
			       &latch, sizeof(latch));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_ADC_PROBE_STATUS,
			  adc, sizeof(adc));
	if (ret)
		return ret;
	audit->rssi_vref = adc[0] | (u16)adc[1] << 8;

	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	if (la_pwd[0] != 0x00 || la_pwd[1] != 0x74)
		return -EIO;
	la_pwd[1] &= ~EN7570_RSSI_VMODE_ENABLE;
	ret = en7570_cal_write(audit, EN7570_LA_PWD,
			       la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	if (la_pwd[0] != 0x00 || la_pwd[1] != 0x34)
		return -EIO;
	ret = en7570_read(audit->en7570, EN7570_SVADC_PD,
			  &svadc, sizeof(svadc));
	if (ret)
		return ret;
	if (svadc != EN7570_ADC_RSSI_ENABLE)
		return -EIO;

	ret = en7570_read(audit->en7570, EN7570_ADC_LATCH,
			  &latch, sizeof(latch));
	if (ret)
		return ret;
	if (latch)
		return -EIO;
	latch |= EN7570_ADC_LATCH_ENABLE;
	ret = en7570_cal_write(audit, EN7570_ADC_LATCH,
			       &latch, sizeof(latch));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_ADC_PROBE_STATUS,
			  adc, sizeof(adc));
	if (ret)
		return ret;
	audit->rssi_v = adc[0] | (u16)adc[1] << 8;

	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	if (la_pwd[0] != 0x00 || la_pwd[1] != 0x34)
		return -EIO;
	la_pwd[1] &= ~EN7570_RSSI_CAL_ENABLE;
	ret = en7570_cal_write(audit, EN7570_LA_PWD,
			       la_pwd, sizeof(la_pwd));
	if (ret)
		return ret;
	ret = en7570_cal_write(audit, EN7570_SVADC_PD,
			       &svadc_original, sizeof(svadc_original));
	if (ret)
		return ret;

	if (audit->rssi_vref < EN7570_RSSI_VREF_MIN ||
	    audit->rssi_vref > EN7570_RSSI_VREF_MAX ||
	    audit->rssi_v < EN7570_RSSI_V_MIN ||
	    audit->rssi_v > EN7570_RSSI_V_MAX ||
	    audit->rssi_v <= audit->rssi_vref)
		return -ERANGE;
	delta = audit->rssi_v - audit->rssi_vref;
	if (delta < EN7570_RSSI_DELTA_MIN || delta > EN7570_RSSI_DELTA_MAX)
		return -ERANGE;

	return 0;
}

static int audit_read_rssi_calibration_state(struct xr500v_reset_audit *audit)
{
	int ret;

	ret = en7570_read(audit->en7570, EN7570_LA_PWD,
			  audit->rssi_cal_la_pwd, sizeof(audit->rssi_cal_la_pwd));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_SVADC_PD,
			  audit->rssi_cal_svadc_pd,
			  sizeof(audit->rssi_cal_svadc_pd));
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, EN7570_PROBE_CONTROL,
			  audit->rssi_cal_probe_control,
			  sizeof(audit->rssi_cal_probe_control));
	if (ret)
		return ret;
	return en7570_read(audit->en7570, EN7570_ADC_PROBE_STATUS,
			   audit->rssi_cal_adc_probe,
			   sizeof(audit->rssi_cal_adc_probe));
}

static int audit_apply_rssi_gain(struct xr500v_reset_audit *audit,
				 const u8 source[4])
{
	u8 value[4];

	if (!source)
		return -EINVAL;
	memcpy(value, source, sizeof(value));
	value[2] = (value[2] & ~EN7570_RSSI_GAIN_MASK) |
		EN7570_RSSI_GAIN_NORMAL;
	return en7570_rx_write(audit, EN7570_LA_PWD, value);
}

static int audit_apply_los(struct xr500v_reset_audit *audit,
			   u8 source_values[][4])
{
	u8 value[4];
	u8 *source;
	int ret;

	source = audit_value(source_values, EN7570_LOS_CTRL1);
	if (!source)
		return -EINVAL;
	memcpy(value, source, sizeof(value));
	value[0] |= EN7570_LOS_CAL_TRIG;
	value[1] = (value[1] & ~EN7570_LOS_STABLE_MASK) |
		EN7570_LOS_STABLE_MASK;
	ret = en7570_rx_write(audit, EN7570_LOS_CTRL1, value);
	if (ret)
		return ret;

	source = audit_value(source_values, 0x0024);
	if (!source)
		return -EINVAL;
	memcpy(value, source, sizeof(value));
	value[3] |= EN7570_ADC_REV2_ENABLE;
	ret = en7570_rx_write(audit, 0x0024, value);
	if (ret)
		return ret;
	ret = en7570_read(audit->en7570, 0x0024, value, sizeof(value));
	if (ret)
		return ret;
	value[2] |= EN7570_ADC_REV1_ENABLE;
	ret = en7570_rx_write(audit, 0x0024, value);
	if (ret)
		return ret;

	source = audit_value(source_values, EN7570_LOS_CTRL2);
	if (!source)
		return -EINVAL;
	memcpy(value, source, sizeof(value));
	value[1] = (value[1] & ~EN7570_LOS_CONF_MASK) |
		EN7570_LOS_CONF_MASK;
	value[0] = (value[0] & ~EN7570_LOS_COUNT_MASK) | 0x05;
	ret = en7570_rx_write(audit, EN7570_LOS_CTRL2, value);
	if (ret)
		return ret;

	ret = en7570_read(audit->en7570, EN7570_LOS_CTRL1,
			  value, sizeof(value));
	if (ret)
		return ret;
	value[2] = (value[2] & ~EN7570_LOS_THRESHOLD_MASK) |
		audit->los_high_threshold;
	value[3] = (value[3] & ~EN7570_LOS_THRESHOLD_MASK) |
		audit->los_low_threshold;
	return en7570_rx_write(audit, EN7570_LOS_CTRL1, value);
}

static int audit_snapshot(struct xr500v_reset_audit *audit, u8 values[][4])
{
	int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(audit_regs); i++) {
		ret = en7570_read(audit->en7570, audit_regs[i].reg,
				  values[i], sizeof(values[i]));
		if (ret)
			return ret;
	}

	return 0;
}

static int audit_baseline(struct xr500v_reset_audit *audit)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(audit_regs); i++) {
		int length = audit_regs[i].reg == EN7570_SAFE_PROTECT ||
			audit_regs[i].reg == EN7570_ROGUE_TX ? 2 : 4;

		if (memcmp(audit->before[i], audit_regs[i].expected, length)) {
			dev_err(audit->dev,
				"baseline mismatch %s@0x%04x: %4ph != %4ph\n",
				audit_regs[i].name, audit_regs[i].reg,
				audit->before[i], audit_regs[i].expected);
			return -EPERM;
		}
	}

	return 0;
}

static int audit_verify_rssi_calibration(struct xr500v_reset_audit *audit)
{
	u8 *source;

	source = audit_value(audit->after, EN7570_LA_PWD);
	if (!source)
		return -EINVAL;
	if (memcmp(audit->rssi_cal_la_pwd, source, 4)) {
		dev_err(audit->dev, "RSSI calibration LA restore: %4ph != %4ph\n",
			audit->rssi_cal_la_pwd, source);
		return -EIO;
	}
	source = audit_value(audit->after, EN7570_SVADC_PD);
	if (!source)
		return -EINVAL;
	if (memcmp(audit->rssi_cal_svadc_pd, source, 4)) {
		dev_err(audit->dev, "RSSI calibration SVADC restore: %4ph != %4ph\n",
			audit->rssi_cal_svadc_pd, source);
		return -EIO;
	}
	source = audit_value(audit->after, EN7570_PROBE_CONTROL);
	if (!source)
		return -EINVAL;
	if (memcmp(audit->rssi_cal_probe_control, source, 4)) {
		dev_err(audit->dev,
			"RSSI calibration probe-control restore: %4ph != %4ph\n",
			audit->rssi_cal_probe_control, source);
		return -EIO;
	}
	source = audit_value(audit->after, EN7570_ADC_PROBE_STATUS);
	if (!source)
		return -EINVAL;
	if (memcmp(audit->rssi_cal_adc_probe + 2, source + 2, 2)) {
		dev_err(audit->dev,
			"RSSI calibration ADC upper-state mismatch: %4ph != %4ph\n",
			audit->rssi_cal_adc_probe, source);
		return -EIO;
	}

	return 0;
}

static int audit_verify_gain_full(struct xr500v_reset_audit *audit,
				  u8 source_values[][4])
{
	u8 expected[4];
	int i;

	for (i = 0; i < ARRAY_SIZE(audit_regs); i++) {
		int length = audit_regs[i].reg == EN7570_SAFE_PROTECT ||
			audit_regs[i].reg == EN7570_ROGUE_TX ? 2 : 4;

		memcpy(expected, source_values[i], sizeof(expected));
		if (audit_regs[i].reg == EN7570_LA_PWD)
			expected[2] = (expected[2] & ~EN7570_RSSI_GAIN_MASK) |
				EN7570_RSSI_GAIN_NORMAL;
		if (memcmp(audit->after_rssi[i], expected, length)) {
			dev_err(audit->dev,
				"RSSI isolation mismatch %s@0x%04x: %4ph != %4ph\n",
				audit_regs[i].name, audit_regs[i].reg,
				audit->after_rssi[i], expected);
			return -EIO;
		}
	}

	return 0;
}

static int audit_read_rssi_gain_state(struct xr500v_reset_audit *audit)
{
	return en7570_read(audit->en7570, EN7570_LA_PWD,
			   audit->rssi_gain_la_pwd,
			   sizeof(audit->rssi_gain_la_pwd));
}

static int audit_verify_gain_rb(struct xr500v_reset_audit *audit,
				const u8 source[4])
{
	u8 expected[4];

	if (!source)
		return -EINVAL;
	memcpy(expected, source, sizeof(expected));
	expected[2] = (expected[2] & ~EN7570_RSSI_GAIN_MASK) |
		EN7570_RSSI_GAIN_NORMAL;
	if (memcmp(audit->rssi_gain_la_pwd, expected, sizeof(expected))) {
		dev_err(audit->dev, "RSSI gain LA readback: %4ph != %4ph\n",
			audit->rssi_gain_la_pwd, expected);
		return -EIO;
	}

	return 0;
}

static int audit_preflight(struct xr500v_reset_audit *audit)
{
	if (gpiod_get_direction(audit->tx_disable) != 0 ||
	    gpiod_get_value_cansleep(audit->tx_disable) != 1)
		return -EPERM;
	if (xpon_read(audit, PHYSET3) & PHYSET3_TXEN)
		return -EPERM;
	if (xpon_read(audit, MISC) & MISC_ROGUE_TX_TEST)
		return -EPERM;
	if (xpon_read(audit, BISTCTL_PRBS_TX_EN) ||
	    xpon_read(audit, TEST_FRAME_EN) || xpon_read(audit, XPON_INT_EN))
		return -EPERM;

	return 0;
}

static int audit_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_reset_audit *audit = s->private;
	int i;

	seq_printf(s, "operation:             %s\n",
		   audit->rssi_calibration ?
		   "EN7570 reset then RSSI calibration/gain then LOS" :
		   audit->rssi_gain ? "EN7570 reset then RSSI gain then LOS" :
		   audit->reset_then_los ? "EN7570 reset then isolated LOS" :
		   "EN7570 OEM four-byte SW_RESET RMW");
	seq_printf(s, "reset_payload:         %4ph\n", audit->reset_write);
	seq_printf(s, "i2c_write_attempts:    %u\n", audit->i2c_write_attempts);
	seq_printf(s, "reset_result:          %d\n", audit->reset_result);
	seq_printf(s, "post_snapshot_result:  %d\n",
		   audit->post_snapshot_result);
	seq_printf(s, "postflight_result:     %d\n", audit->postflight_result);
	if (audit->rssi_calibration) {
		seq_printf(s, "rssi_cal_attempted:    %s\n",
			   audit->rssi_cal_attempted ? "yes" : "no");
		seq_printf(s, "rssi_cal_result:       %d\n",
			   audit->rssi_cal_result);
		seq_printf(s, "rssi_cal_readback_result: %d\n",
			   audit->rssi_cal_readback_result);
		seq_printf(s, "rssi_cal_verify_result: %d\n",
			   audit->rssi_cal_verify_result);
		seq_printf(s, "rssi_cal_postflight_result: %d\n",
			   audit->rssi_cal_postflight_result);
		seq_printf(s, "rssi_cal_adc:          vref=0x%04x v=0x%04x delta=0x%04x\n",
			   audit->rssi_vref, audit->rssi_v,
			   audit->rssi_v >= audit->rssi_vref ?
			   audit->rssi_v - audit->rssi_vref : 0);
		seq_printf(s, "rssi_cal_readbacks:    la=%4ph svadc=%4ph adc=%4ph probe=%4ph\n",
			   audit->rssi_cal_la_pwd, audit->rssi_cal_svadc_pd,
			   audit->rssi_cal_adc_probe,
			   audit->rssi_cal_probe_control);
	}
	if (audit->rssi_gain) {
		seq_printf(s, "rssi_attempted:        %s\n",
			   audit->rssi_attempted ? "yes" : "no");
		seq_printf(s, "rssi_result:           %d\n", audit->rssi_result);
		if (audit->rssi_calibration) {
			seq_printf(s, "rssi_readback_result:  %d\n",
				   audit->rssi_readback_result);
			seq_printf(s, "rssi_gain_readback:    la=%4ph\n",
				   audit->rssi_gain_la_pwd);
		} else {
			seq_printf(s, "rssi_snapshot_result:  %d\n",
				   audit->rssi_snapshot_result);
		}
		seq_printf(s, "rssi_verify_result:    %d\n",
			   audit->rssi_verify_result);
		seq_printf(s, "rssi_postflight_result: %d\n",
			   audit->rssi_postflight_result);
	}
	seq_printf(s, "los_attempted:         %s\n",
		   audit->los_attempted ? "yes" : "no");
	seq_printf(s, "los_result:            %d\n", audit->los_result);
	seq_printf(s, "los_snapshot_result:   %d\n",
		   audit->los_snapshot_result);
	seq_printf(s, "los_postflight_result: %d\n",
		   audit->los_postflight_result);
	if (audit->reset_then_los)
		seq_printf(s, "los_thresholds:        high=0x%02x low=0x%02x\n",
			   audit->los_high_threshold, audit->los_low_threshold);
	seq_printf(s, "module_pinned:         %s\n",
		   audit->module_pinned ? "yes" : "NO");
	seq_printf(s, "tx_disable_asserted:   %s\n",
		   gpiod_get_value_cansleep(audit->tx_disable) == 1 ? "yes" : "NO");
	seq_printf(s, "xpon_txen:             %s\n",
		   xpon_read(audit, PHYSET3) & PHYSET3_TXEN ? "YES" : "no");
	for (i = 0; i < ARRAY_SIZE(audit_regs); i++) {
		seq_printf(s, "%-18s 0x%04x before=%4ph after=%4ph\n",
			   audit_regs[i].name, audit_regs[i].reg,
			   audit->before[i], audit->after[i]);
		if (audit->rssi_gain && !audit->rssi_calibration)
			seq_printf(s, "%-18s        after_rssi=%4ph\n",
				   "", audit->after_rssi[i]);
		if (audit->reset_then_los)
			seq_printf(s, "%-18s        after_los=%4ph\n",
				   "", audit->after_los[i]);
	}
	seq_printf(s, "los_init:              %s\n",
		   audit->los_attempted ? "isolated OEM sequence" : "no");
	seq_printf(s, "rssi_gain_init:        %s\n",
		   audit->rssi_attempted ? "isolated OEM RMW" : "no");
	seq_printf(s, "adc_rssi_calibration:  %s\n",
		   audit->rssi_cal_attempted ?
		   "isolated OEM RSSI transient only" : "no");
	seq_puts(s, "ddmi_worker_init:      no\n");
	seq_puts(s, "apd_tgen_current_init: no\n");
	seq_puts(s, "software_rollback:     impossible/not attempted\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(audit_status);

static void en7570_put_client(void *data)
{
	struct i2c_client *client = data;

	put_device(&client->dev);
}

static int xr500v_reset_audit_probe(struct platform_device *pdev)
{
	struct xr500v_reset_audit *audit;
	struct device_node *node;
	u32 threshold;
	u8 id = 0;
	int ret;

	if (arm_en7570_reset_audit + arm_en7570_reset_then_los +
	    arm_en7570_reset_rssi_los +
	    arm_en7570_reset_rssi_cal_los != 1)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "select exactly one reset audit mode\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-reset-audit"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT reset opt-in is absent\n");

	audit = devm_kzalloc(&pdev->dev, sizeof(*audit), GFP_KERNEL);
	if (!audit)
		return -ENOMEM;
	audit->dev = &pdev->dev;
	audit->reset_then_los = arm_en7570_reset_then_los ||
		arm_en7570_reset_rssi_los || arm_en7570_reset_rssi_cal_los;
	audit->rssi_gain = arm_en7570_reset_rssi_los ||
		arm_en7570_reset_rssi_cal_los;
	audit->rssi_calibration = arm_en7570_reset_rssi_cal_los;
	if (audit->reset_then_los) {
		ret = device_property_read_u32(&pdev->dev,
					       "airoha,los-high-threshold", &threshold);
		if (ret || threshold > EN7570_LOS_THRESHOLD_MASK)
			return dev_err_probe(&pdev->dev, ret ?: -ERANGE,
				"invalid LOS high threshold\n");
		audit->los_high_threshold = threshold;
		ret = device_property_read_u32(&pdev->dev,
					       "airoha,los-low-threshold", &threshold);
		if (ret || threshold > EN7570_LOS_THRESHOLD_MASK)
			return dev_err_probe(&pdev->dev, ret ?: -ERANGE,
				"invalid LOS low threshold\n");
		audit->los_low_threshold = threshold;
		if (audit->los_high_threshold <= audit->los_low_threshold)
			return dev_err_probe(&pdev->dev, -ERANGE,
				"invalid LOS threshold ordering\n");
	}
	audit->tx_disable = devm_gpiod_get(&pdev->dev, "tx-disable",
					   GPIOD_OUT_HIGH);
	if (IS_ERR(audit->tx_disable))
		return dev_err_probe(&pdev->dev, PTR_ERR(audit->tx_disable),
				     "cannot assert physical TX_DISABLE\n");
	audit->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(audit->base))
		return PTR_ERR(audit->base);

	ret = audit_preflight(audit);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "TX preflight failed\n");

	node = of_parse_phandle(pdev->dev.of_node, "airoha,en7570", 0);
	if (!node)
		return -EINVAL;
	audit->en7570 = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!audit->en7570)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(&pdev->dev, en7570_put_client,
				       audit->en7570);
	if (ret)
		return ret;

	ret = en7570_read(audit->en7570, EN7570_ID_REG, &id, sizeof(id));
	if (ret || id != EN7570_EXPECTED_ID)
		return dev_err_probe(&pdev->dev, ret ?: -ENODEV,
				     "EN7570 identity mismatch: 0x%02x\n", id);
	ret = audit_snapshot(audit, audit->before);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "baseline read failed\n");
	ret = audit_baseline(audit);
	if (ret)
		return ret;

	memcpy(audit->reset_write,
	       audit->before[ARRAY_SIZE(audit_regs) - 1], 4);
	audit->reset_write[0] = (audit->reset_write[0] & 0xf8) | 0x01;
	/*
	 * The first transfer attempt is the non-transactional boundary. From this
	 * point onward probe must succeed and retain GPIO16 even if the adapter
	 * reports an error or every post-reset read fails.
	 */
	__module_get(THIS_MODULE);
	audit->module_pinned = true;
	audit->reset_result = en7570_reset_write(audit, audit->reset_write);
	if (audit->reset_result)
		dev_err(&pdev->dev, "reset transfer result: %d\n",
			audit->reset_result);
	audit->post_snapshot_result = audit_snapshot(audit, audit->after);
	if (audit->post_snapshot_result)
		dev_err(&pdev->dev, "post-reset snapshot result: %d\n",
			audit->post_snapshot_result);
	audit->postflight_result = audit_preflight(audit);
	if (audit->postflight_result)
		dev_err(&pdev->dev, "TX postflight result: %d\n",
			audit->postflight_result);

	if (audit->reset_then_los) {
		u8 (*los_source)[4] = audit->after;
		u8 *rssi_source = audit_value(audit->after, EN7570_LA_PWD);
		int rssi_observation_result = -ECANCELED;

		audit->rssi_cal_result = -ECANCELED;
		audit->rssi_cal_readback_result = -ECANCELED;
		audit->rssi_cal_verify_result = -ECANCELED;
		audit->rssi_cal_postflight_result = -ECANCELED;
		audit->rssi_result = -ECANCELED;
		audit->rssi_readback_result = -ECANCELED;
		audit->rssi_snapshot_result = -ECANCELED;
		audit->rssi_verify_result = -ECANCELED;
		audit->rssi_postflight_result = -ECANCELED;
		audit->los_result = -ECANCELED;
		audit->los_snapshot_result = -ECANCELED;
		audit->los_postflight_result = -ECANCELED;
		if (!audit->reset_result && !audit->post_snapshot_result &&
		    !audit->postflight_result) {
			/* Exceed the 1-2 ms bandgap settling delay used upstream. */
			usleep_range(10000, 12000);
			if (audit->rssi_calibration) {
				audit->rssi_cal_attempted = true;
				audit->rssi_cal_result =
					audit_apply_rssi_calibration(audit);
				if (audit->rssi_cal_result)
					dev_err(&pdev->dev,
						"RSSI calibration result: %d\n",
						audit->rssi_cal_result);
				audit->rssi_cal_readback_result =
					audit_read_rssi_calibration_state(audit);
				if (!audit->rssi_cal_readback_result)
					audit->rssi_cal_verify_result =
						audit_verify_rssi_calibration(audit);
				audit->rssi_cal_postflight_result =
					audit_preflight(audit);
				if (audit->rssi_cal_readback_result)
					dev_err(&pdev->dev,
						"RSSI calibration readback result: %d\n",
						audit->rssi_cal_readback_result);
				if (audit->rssi_cal_verify_result)
					dev_err(&pdev->dev,
						"RSSI calibration isolation result: %d\n",
						audit->rssi_cal_verify_result);
				if (audit->rssi_cal_postflight_result)
					dev_err(&pdev->dev,
						"RSSI calibration TX postflight result: %d\n",
						audit->rssi_cal_postflight_result);
				if (!audit->rssi_cal_result &&
				    !audit->rssi_cal_readback_result &&
				    !audit->rssi_cal_verify_result &&
				    !audit->rssi_cal_postflight_result)
					rssi_source = audit->rssi_cal_la_pwd;
			}
			if (audit->rssi_gain &&
			    (!audit->rssi_calibration ||
			     (!audit->rssi_cal_result &&
			      !audit->rssi_cal_readback_result &&
			      !audit->rssi_cal_verify_result &&
			      !audit->rssi_cal_postflight_result))) {
				audit->rssi_attempted = true;
				audit->rssi_result =
					audit_apply_rssi_gain(audit, rssi_source);
				if (audit->rssi_result)
					dev_err(&pdev->dev,
						"RSSI gain result: %d\n",
						audit->rssi_result);
				if (audit->rssi_calibration) {
					audit->rssi_readback_result =
						audit_read_rssi_gain_state(audit);
					rssi_observation_result =
						audit->rssi_readback_result;
					if (!audit->rssi_readback_result)
						audit->rssi_verify_result =
							audit_verify_gain_rb(audit, rssi_source);
				} else {
					audit->rssi_snapshot_result =
						audit_snapshot(audit, audit->after_rssi);
					rssi_observation_result =
						audit->rssi_snapshot_result;
					if (!audit->rssi_snapshot_result)
						audit->rssi_verify_result =
							audit_verify_gain_full(audit, los_source);
					los_source = audit->after_rssi;
				}
				audit->rssi_postflight_result = audit_preflight(audit);
				if (rssi_observation_result)
					dev_err(&pdev->dev, "RSSI observation result: %d\n",
						rssi_observation_result);
				if (audit->rssi_verify_result)
					dev_err(&pdev->dev, "RSSI isolation result: %d\n",
						audit->rssi_verify_result);
				if (audit->rssi_postflight_result)
					dev_err(&pdev->dev,
						"RSSI TX postflight result: %d\n",
						audit->rssi_postflight_result);
			}
			if (!audit->rssi_gain ||
			    (!audit->rssi_result && !rssi_observation_result &&
			     !audit->rssi_verify_result &&
			     !audit->rssi_postflight_result)) {
				audit->los_attempted = true;
				audit->los_result =
					audit_apply_los(audit, los_source);
				if (audit->los_result)
					dev_err(&pdev->dev,
						"LOS sequence result: %d\n",
						audit->los_result);
				usleep_range(20000, 25000);
				audit->los_snapshot_result =
					audit_snapshot(audit, audit->after_los);
				audit->los_postflight_result =
					audit_preflight(audit);
				if (audit->los_snapshot_result)
					dev_err(&pdev->dev,
						"LOS snapshot result: %d\n",
						audit->los_snapshot_result);
				if (audit->los_postflight_result)
					dev_err(&pdev->dev,
						"LOS TX postflight result: %d\n",
						audit->los_postflight_result);
			}
		}
	}

	audit->debugfs_dir = debugfs_create_dir("xr500v-en7570-reset-audit", NULL);
	if (IS_ERR(audit->debugfs_dir)) {
		dev_err(&pdev->dev, "debugfs unavailable: %ld\n",
			PTR_ERR(audit->debugfs_dir));
		audit->debugfs_dir = NULL;
	} else {
		debugfs_create_file("status", 0444, audit->debugfs_dir, audit,
				    &audit_status_fops);
	}
	platform_set_drvdata(pdev, audit);
	dev_warn(&pdev->dev,
		 "EN7570 %s observed; TX_DISABLE held; physical power cycle is the recovery boundary\n",
		 audit->rssi_calibration ? "reset-RSSI-calibration-LOS" :
		 audit->rssi_gain ? "reset-RSSI-LOS" :
		 audit->reset_then_los ? "reset-then-LOS" : "reset");
	return 0;
}

static void xr500v_reset_audit_remove(struct platform_device *pdev)
{
	struct xr500v_reset_audit *audit = platform_get_drvdata(pdev);

	debugfs_remove_recursive(audit->debugfs_dir);
	dev_warn(&pdev->dev,
		 "reset audit removed without rollback; physical power cycle required\n");
}

static const struct of_device_id xr500v_reset_audit_of_match[] = {
	{ .compatible = "econet,en751221-en7570-reset-audit-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_reset_audit_of_match);

static struct platform_driver xr500v_reset_audit_driver = {
	.probe = xr500v_reset_audit_probe,
	.remove = xr500v_reset_audit_remove,
	.driver = {
		.name = "xr500v-en7570-reset-audit",
		.of_match_table = xr500v_reset_audit_of_match,
	},
};
module_platform_driver(xr500v_reset_audit_driver);

MODULE_DESCRIPTION("Guarded XR500v EN7570 reset/RSSI-calibration/LOS audit");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
