// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot EN7570 receiver/APD A2 observer for the TP-Link Archer XR500v.
 *
 * This deliberately non-shipping driver accepts only one exact unit and one
 * exact cold state.  With physical TX_DISABLE retained high, it performs the
 * 15 fixed writes already observed in the isolated reset/RSSI/gain/LOS run,
 * one fixed RX-polarity MMIO write, and the three fixed APD A2 writes.  It
 * then takes 21 finite read-only samples.  There is no retry, worker,
 * arbitrary payload, rollback, disable, TX-current or laser path.  The module
 * self-pins before the first transfer and physical power removal is the only
 * recovery boundary.
 */

#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define PHYSET3			0x0108
#define PHYSET3_TXEN		BIT(5)
#define PHYSET10		0x0124
#define PHYSET10_GPON_MODE	BIT(31)
#define PHYSTA1			0x0130
#define PHYSTA1_PHY_CURR		GENMASK(20, 18)
#define XPON_SETTING		0x0138
#define XPON_SETTING_RETAINED	0x0000014f
#define XPON_SETTING_EN7570	0x0000010f
#define XPON_SETTING_RX_SD_INV	BIT(6)
#define MISC			0x01fc
#define MISC_ROGUE_TX_TEST	BIT(28)
#define PHYRX_STATUS		0x021c
#define PHYRX_SYNC_MASK		GENMASK(3, 0)
#define PHYRX_SYNC_VALUE	0x0a
#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_STA			0x05e0
#define XPON_STA_LOS		BIT(0)
#define XPON_INT_EN		0x05f0

#define EN7570_LA_PWD		0x0014
#define EN7570_SVADC_PD		0x0024
#define EN7570_APD_DAC		0x0030
#define EN7570_SAFE_PROTECT	0x0100
#define EN7570_LOS_CTRL1		0x011c
#define EN7570_LOS_CTRL2		0x0120
#define EN7570_LOS_TIMEOUT_COUNT 0x0128
#define EN7570_LOS_TIMEOUT	0x012c
#define EN7570_LOS_DBG		0x0130
#define EN7570_IBIAS		0x0138
#define EN7570_IMOD		0x0148
#define EN7570_ADC_PROBE		0x0154
#define EN7570_PROBE_CONTROL	0x0158
#define EN7570_ADC_LATCH		0x0159
#define EN7570_APD_OVP_LATCH	0x0164
#define EN7570_ID		0x0170
#define EN7570_SW_RESET		0x0300
#define EN7570_EXPECTED_ID	0x03

#define EN7570_I2C_ADDRESS	0x70
#define XR500V_PON_I2C_BASE	0x1fbf8000
#define XR500V_PON_I2C_SIZE	0x100
#define XR500V_XPON_BASE	0x1faf0000
#define XR500V_XPON_SIZE	0x1000

#define XR500V_FACTORY_LENGTH	0x190
#define XR500V_TX_DISABLE_GPIO	16
#define FIXED_WRITE_COUNT	18
#define PREFIX_WRITE_COUNT	15
#define APD_FIRST_WRITE		15
#define APD_STEP_COUNT		3
#define RX_SAMPLE_COUNT		21
#define RX_SAMPLE_INTERVAL_MS	100

static bool arm_en7570_rx_apd_a2;
module_param(arm_en7570_rx_apd_a2, bool, 0444);
MODULE_PARM_DESC(arm_en7570_rx_apd_a2,
		 "Arm the one-shot, non-transactional XR500v EN7570 RX/APD A2 observer");

static atomic_t rx_apd_sequence_claimed = ATOMIC_INIT(0);

static const u8 xr500v_factory_sha256[SHA256_DIGEST_SIZE] = {
	0x40, 0x1d, 0xfd, 0xae, 0xe7, 0x7c, 0x84, 0x64,
	0x9b, 0xda, 0x10, 0x0f, 0xd5, 0xdd, 0x85, 0xbe,
	0x01, 0xc7, 0xea, 0x12, 0x6d, 0x0a, 0x5c, 0xc2,
	0x11, 0x6b, 0x14, 0x1c, 0x1a, 0x07, 0xa5, 0xe4,
};

static const u8 apd_states[APD_STEP_COUNT + 1][4] = {
	{ 0x00, 0x08, 0x00, 0x00 },
	{ 0x00, 0x08, 0x20, 0x00 },
	{ 0x00, 0x09, 0x20, 0x00 },
	{ 0xa2, 0x09, 0x20, 0x00 },
};


struct fixed_write {
	u16 reg;
	u8 length;
	u8 value[4];
};

/* The sole I2C write whitelist, in the only order the driver accepts. */
static const struct fixed_write fixed_writes[FIXED_WRITE_COUNT] = {
	{ EN7570_SW_RESET, 4, { 0x01, 0x00, 0x00, 0x00 } },
	{ EN7570_LA_PWD, 2, { 0x00, 0x34, 0x00, 0x00 } },
	{ EN7570_LA_PWD, 2, { 0x00, 0x74, 0x00, 0x00 } },
	{ EN7570_SVADC_PD, 1, { 0x02, 0x00, 0x00, 0x00 } },
	{ EN7570_ADC_LATCH, 1, { 0x10, 0x00, 0x00, 0x00 } },
	{ EN7570_LA_PWD, 2, { 0x00, 0x34, 0x00, 0x00 } },
	{ EN7570_ADC_LATCH, 1, { 0x10, 0x00, 0x00, 0x00 } },
	{ EN7570_LA_PWD, 2, { 0x00, 0x24, 0x00, 0x00 } },
	{ EN7570_SVADC_PD, 1, { 0x00, 0x00, 0x00, 0x00 } },
	{ EN7570_LA_PWD, 4, { 0x00, 0x24, 0x05, 0x00 } },
	{ EN7570_LOS_CTRL1, 4, { 0x07, 0x1f, 0x3c, 0x36 } },
	{ EN7570_SVADC_PD, 4, { 0x00, 0x00, 0x01, 0x04 } },
	{ EN7570_SVADC_PD, 4, { 0x00, 0x00, 0x41, 0x04 } },
	{ EN7570_LOS_CTRL2, 4, { 0x05, 0x1f, 0x00, 0x00 } },
	{ EN7570_LOS_CTRL1, 4, { 0x06, 0x1f, 0x1c, 0x10 } },
	{ EN7570_APD_DAC, 4, { 0x00, 0x08, 0x20, 0x00 } },
	{ EN7570_APD_DAC, 4, { 0x00, 0x09, 0x20, 0x00 } },
	{ EN7570_APD_DAC, 1, { 0xa2, 0x00, 0x00, 0x00 } },
};

static const u8 expected_safe_protect[4] = { 0xff, 0x8f, 0xff, 0x0f };
static const u8 all_zero[4];

struct en7570_guard_reg {
	const char *name;
	u16 reg;
	u8 expected[4];
};

/* Exact post-LOS values for the controls rechecked around every APD write. */
static const struct en7570_guard_reg en7570_tx_guards[] = {
	{ "tiamux", 0x0000, { 0x08, 0x00, 0x10, 0x02 } },
	{ "mpd_targets", 0x0004, { 0x00, 0x02, 0x00, 0x00 } },
	{ "t1delay", 0x0008, { 0x99, 0x00, 0x00, 0x20 } },
	{ "tx_sd", 0x000c, { 0x40, 0x00, 0x00, 0x00 } },
	{ "la_pwd", 0x0014, { 0x00, 0x24, 0x05, 0x00 } },
	{ "bgcken", 0x001c, { 0x55, 0x55, 0x55, 0xa5 } },
	{ "pi_tgen", 0x0020, { 0x06, 0x00, 0x00, 0x00 } },
	{ "p0_cs1", 0x0134, { 0x00, 0x02, 0x04, 0x10 } },
	{ "p0_cs3", 0x013c, { 0x30, 0x12, 0x00, 0x10 } },
	{ "p0_latch", 0x0140, { 0x00, 0x00, 0x00, 0x00 } },
	{ "p1_cs1", 0x0144, { 0x00, 0x02, 0x04, 0x10 } },
	{ "p1_cs3", 0x014c, { 0x30, 0x12, 0x00, 0x10 } },
	{ "p1_latch", 0x0150, { 0x00, 0x00, 0x00, 0x00 } },
	{ "rogue_tx", 0x0168, { 0x34, 0x02, 0x00, 0x00 } },
	{ "erc_filter", 0x016c, { 0x3f, 0x2f, 0x0f, 0x00 } },
	{ "sw_reset", 0x0300, { 0x00, 0x00, 0x00, 0x00 } },
};

struct en7570_state_reg {
	const char *name;
	u16 reg;
	u8 cold[4];
};

/* Phase-21 28-register map plus the separately audited Tx-SD register. */
static const struct en7570_state_reg en7570_state_regs[] = {
	{ "tiamux", 0x0000, { 0x08, 0x00, 0x10, 0x02 } },
	{ "mpd_targets", 0x0004, { 0x00, 0x02, 0x00, 0x00 } },
	{ "t1delay", 0x0008, { 0x99, 0x00, 0x00, 0x20 } },
	{ "tx_sd", 0x000c, { 0x40, 0x00, 0x00, 0x00 } },
	{ "la_pwd", 0x0014, { 0x00, 0x24, 0x00, 0x00 } },
	{ "bgcken", 0x001c, { 0x55, 0x55, 0x55, 0xa5 } },
	{ "pi_tgen", 0x0020, { 0x06, 0x00, 0x00, 0x00 } },
	{ "svadc_pd", 0x0024, { 0x00, 0x00, 0x01, 0x00 } },
	{ "apd_dac", 0x0030, { 0x00, 0x08, 0x00, 0x00 } },
	{ "safe_protect", 0x0100, { 0xff, 0x8f, 0xff, 0x0f } },
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
	{ "adc_probe", 0x0154, { 0x00, 0x00, 0x00, 0x00 } },
	{ "probe_control", 0x0158, { 0x00, 0x00, 0x00, 0x00 } },
	{ "apd_ovp_latch", 0x0164, { 0x00, 0x00, 0x00, 0x00 } },
	{ "rogue_tx", 0x0168, { 0x34, 0x02, 0x00, 0x00 } },
	{ "erc_filter", 0x016c, { 0x3f, 0x2f, 0x0f, 0x00 } },
	{ "sw_reset", 0x0300, { 0x00, 0x00, 0x00, 0x00 } },
};

enum full_snapshot_stage {
	SNAPSHOT_COLD,
	SNAPSHOT_POST_RESET,
	SNAPSHOT_POST_LOS,
	SNAPSHOT_FINAL,
};

struct full_snapshot {
	int result;
	int reg_result[ARRAY_SIZE(en7570_state_regs)];
	u8 value[ARRAY_SIZE(en7570_state_regs)][4];
	int gpio_direction;
	int gpio_value;
	int gpio_raw_value;
	bool gpio_active_low;
	u32 physet3;
	u32 physet10;
	u32 physta1;
	u32 xpon_setting;
	u32 misc;
	u32 phy_rx_status;
	u32 prbs_tx;
	u32 test_frame;
	u32 xpon_sta;
	u32 xpon_int_en;
};

struct rx_sample {
	int result;
	u8 ovp[4];
	u8 apd[4];
	u8 safe_protect[4];
	u8 ibias[4];
	u8 imod[4];
	u8 los_ctrl1[4];
	u8 los_ctrl2[4];
	u8 los_debug[4];
	u8 los_timeout[4];
	int gpio_direction;
	int gpio_value;
	int gpio_raw_value;
	bool gpio_active_low;
	u32 physet3;
	u32 physet10;
	u32 physta1;
	u32 xpon_setting;
	u32 misc;
	u32 phy_rx_status;
	u32 prbs_tx;
	u32 test_frame;
	u32 xpon_sta;
	u32 xpon_int_en;
};

struct apd_snapshot {
	int result;
	int gpio_direction;
	int gpio_value;
	int gpio_raw_value;
	bool gpio_active_low;
	bool ovp_terminal_sampled;
	int apd_result;
	int ovp_result;
	int ovp_terminal_result;
	int safe_result;
	int ibias_result;
	int imod_result;
	int tx_guard_result[ARRAY_SIZE(en7570_tx_guards)];
	u8 apd[4];
	u8 ovp[4];
	u8 ovp_terminal[4];
	u8 safe_protect[4];
	u8 ibias[4];
	u8 imod[4];
	u8 tx_guard[ARRAY_SIZE(en7570_tx_guards)][4];
	u32 physet3;
	u32 physet10;
	u32 physta1;
	u32 xpon_setting;
	u32 misc;
	u32 phy_rx_status;
	u32 prbs_tx;
	u32 test_frame;
	u32 xpon_sta;
	u32 xpon_int_en;
};

struct apd_step_result {
	struct apd_snapshot pre;
	struct apd_snapshot post;
	int pre_verify_result;
	int write_result;
	int post_verify_result;
	bool write_attempted;
};

struct xr500v_rx_apd_observer {
	struct device *dev;
	void __iomem *xpon_base;
	struct gpio_desc *tx_disable;
	struct i2c_client *en7570;
	struct dentry *debugfs_dir;
	struct dentry *debugfs_status;
	struct apd_step_result steps[APD_STEP_COUNT];
	struct full_snapshot cold;
	struct full_snapshot post_reset;
	struct full_snapshot post_los;
	struct full_snapshot final;
	struct rx_sample samples[RX_SAMPLE_COUNT];
	bool write_attempted[FIXED_WRITE_COUNT];
	int write_result[FIXED_WRITE_COUNT];
	u8 silicon_id;
	u8 factory_digest[SHA256_DIGEST_SIZE];
	u8 rssi_latch_initial;
	u8 rssi_latch_second;
	u8 rssi_cal_la[4];
	u8 rssi_cal_svadc[4];
	u8 rssi_cal_adc[4];
	u8 rssi_cal_probe[4];
	u8 rssi_gain_la[4];
	u8 los_trigger_readback[4];
	size_t factory_length;
	u16 rssi_vref;
	u16 rssi_v;
	int tx_disable_gpio;
	int tx_disable_offset;
	int adapter_retries_saved;
	unsigned int i2c_write_attempts;
	unsigned int mmio_write_attempts;
	unsigned int samples_taken;
	int sequence_result;
	int halted_step;
	int cold_verify_result;
	int post_reset_verify_result;
	int post_los_verify_result;
	int final_verify_result;
	int sample_result;
	int polarity_result;
	u32 xpon_setting_before;
	u32 xpon_setting_after;
	bool polarity_attempted;
	bool factory_hash_matched;
	bool adapter_retries_restored;
	bool i2c_bus_locked;
	bool module_pinned;
	bool physical_powercut_required;
};

static u32 xpon_read(struct xr500v_rx_apd_observer *observer, u32 reg)
{
	return ioread32(observer->xpon_base + reg);
}

static int en7570_read(struct xr500v_rx_apd_observer *observer, u16 reg,
		       void *value, int length)
{
	u8 pointer[2] = { reg >> 8, reg & 0xff };
	struct i2c_msg messages[2] = {
		{ .addr = observer->en7570->addr, .len = sizeof(pointer),
		  .buf = pointer },
		{ .addr = observer->en7570->addr, .flags = I2C_M_RD,
		  .len = length, .buf = value },
	};
	int ret;

	if (!observer->i2c_bus_locked)
		return -EPERM;

	/* The observer owns I2C_LOCK_SEGMENT for every EN7570 access. */
	ret = __i2c_transfer(observer->en7570->adapter, messages,
			     ARRAY_SIZE(messages));
	if (ret < 0)
		return ret;

	return ret == ARRAY_SIZE(messages) ? 0 : -EIO;
}

static int en7570_read4(struct xr500v_rx_apd_observer *observer, u16 reg,
			u8 value[4])
{
	return en7570_read(observer, reg, value, 4);
}

static void remember_error(int *result, int candidate)
{
	if (!*result && candidate)
		*result = candidate;
}

static int gpio_xpon_gate(struct xr500v_rx_apd_observer *observer)
{
	int direction;
	int logical;
	int raw;

	if (gpiod_is_active_low(observer->tx_disable))
		return -EPERM;
	direction = gpiod_get_direction(observer->tx_disable);
	logical = gpiod_get_value_cansleep(observer->tx_disable);
	raw = gpiod_get_raw_value_cansleep(observer->tx_disable);
	if (direction < 0)
		return direction;
	if (logical < 0)
		return logical;
	if (raw < 0)
		return raw;
	if (direction != 0 || logical != 1 || raw != 1)
		return -EPERM;
	if (xpon_read(observer, PHYSET3) & PHYSET3_TXEN)
		return -EPERM;
	if (!(xpon_read(observer, PHYSET10) & PHYSET10_GPON_MODE))
		return -EPERM;
	if (xpon_read(observer, MISC) & MISC_ROGUE_TX_TEST)
		return -EPERM;
	if (xpon_read(observer, BISTCTL_PRBS_TX_EN) ||
	    xpon_read(observer, TEST_FRAME_EN) ||
	    xpon_read(observer, XPON_INT_EN))
		return -EPERM;

	return 0;
}

/* Fast gate immediately before each fixed I2C or MMIO write. */
static int fast_tx_gate(struct xr500v_rx_apd_observer *observer)
{
	u8 value[4];
	int ret;

	if (!observer->i2c_bus_locked || !observer->module_pinned)
		return -EPERM;
	ret = gpio_xpon_gate(observer);
	if (ret)
		return ret;
	/* OVP is deliberately the first EN7570 read in every fast gate. */
	ret = en7570_read4(observer, EN7570_APD_OVP_LATCH, value);
	if (ret || memcmp(value, all_zero, sizeof(value)))
		return ret ?: -EPERM;
	ret = en7570_read4(observer, EN7570_SAFE_PROTECT, value);
	if (ret || memcmp(value, expected_safe_protect, sizeof(value)))
		return ret ?: -EPERM;
	ret = en7570_read4(observer, EN7570_IBIAS, value);
	if (ret || memcmp(value, all_zero, sizeof(value)))
		return ret ?: -EPERM;
	ret = en7570_read4(observer, EN7570_IMOD, value);
	if (ret || memcmp(value, all_zero, sizeof(value)))
		return ret ?: -EPERM;

	return 0;
}

static void full_snapshot_capture(struct xr500v_rx_apd_observer *observer,
				  struct full_snapshot *snapshot)
{
	int i;

	memset(snapshot, 0, sizeof(*snapshot));
	for (i = 0; i < ARRAY_SIZE(en7570_state_regs); i++) {
		snapshot->reg_result[i] =
			en7570_read4(observer, en7570_state_regs[i].reg,
				     snapshot->value[i]);
		remember_error(&snapshot->result, snapshot->reg_result[i]);
	}

	snapshot->gpio_direction = gpiod_get_direction(observer->tx_disable);
	snapshot->gpio_value = gpiod_get_value_cansleep(observer->tx_disable);
	snapshot->gpio_raw_value =
		gpiod_get_raw_value_cansleep(observer->tx_disable);
	snapshot->gpio_active_low = gpiod_is_active_low(observer->tx_disable);
	if (snapshot->gpio_direction < 0)
		remember_error(&snapshot->result, snapshot->gpio_direction);
	if (snapshot->gpio_value < 0)
		remember_error(&snapshot->result, snapshot->gpio_value);
	if (snapshot->gpio_raw_value < 0)
		remember_error(&snapshot->result, snapshot->gpio_raw_value);

	snapshot->physet3 = xpon_read(observer, PHYSET3);
	snapshot->physet10 = xpon_read(observer, PHYSET10);
	snapshot->physta1 = xpon_read(observer, PHYSTA1);
	snapshot->xpon_setting = xpon_read(observer, XPON_SETTING);
	snapshot->misc = xpon_read(observer, MISC);
	snapshot->phy_rx_status = xpon_read(observer, PHYRX_STATUS);
	snapshot->prbs_tx = xpon_read(observer, BISTCTL_PRBS_TX_EN);
	snapshot->test_frame = xpon_read(observer, TEST_FRAME_EN);
	snapshot->xpon_sta = xpon_read(observer, XPON_STA);
	snapshot->xpon_int_en = xpon_read(observer, XPON_INT_EN);
}

static int full_snapshot_verify(struct xr500v_rx_apd_observer *observer,
				const struct full_snapshot *snapshot,
				enum full_snapshot_stage stage)
{
	static const u8 post_los_la[] = { 0x00, 0x24, 0x05, 0x00 };
	static const u8 post_los_svadc[] = { 0x00, 0x00, 0x41, 0x04 };
	static const u8 post_los_ctrl1[] = { 0x06, 0x1f, 0x1c, 0x10 };
	static const u8 post_los_ctrl2[] = { 0x05, 0x1f, 0x22, 0x00 };
	static const u8 post_los_timeout[] = { 0x3e, 0x00, 0x00, 0x00 };
	u8 expected[4];
	int i;

	if (snapshot->result)
		return snapshot->result;
	if (snapshot->gpio_active_low || snapshot->gpio_direction != 0 ||
	    snapshot->gpio_value != 1 || snapshot->gpio_raw_value != 1)
		return -EPERM;
	if (snapshot->physet3 & PHYSET3_TXEN ||
	    !(snapshot->physet10 & PHYSET10_GPON_MODE) ||
	    snapshot->misc & MISC_ROGUE_TX_TEST || snapshot->prbs_tx ||
	    snapshot->test_frame || snapshot->xpon_int_en)
		return -EPERM;
	if (snapshot->xpon_setting !=
	    (stage == SNAPSHOT_FINAL ? XPON_SETTING_EN7570 :
	     XPON_SETTING_RETAINED))
		return -EPERM;

	for (i = 0; i < ARRAY_SIZE(en7570_state_regs); i++) {
		const u16 reg = en7570_state_regs[i].reg;

		memcpy(expected, en7570_state_regs[i].cold, sizeof(expected));
		if (stage == SNAPSHOT_POST_LOS || stage == SNAPSHOT_FINAL) {
			if (reg == EN7570_LA_PWD)
				memcpy(expected, post_los_la, 4);
			else if (reg == EN7570_SVADC_PD)
				memcpy(expected, post_los_svadc, 4);
			else if (reg == EN7570_LOS_CTRL1)
				memcpy(expected, post_los_ctrl1, 4);
			else if (reg == EN7570_LOS_CTRL2)
				memcpy(expected, post_los_ctrl2, 4);
			else if (reg == EN7570_LOS_TIMEOUT)
				memcpy(expected, post_los_timeout, 4);
			else if (reg == EN7570_ADC_PROBE) {
				expected[0] = observer->rssi_v & 0xff;
				expected[1] = observer->rssi_v >> 8;
			}
		}
		if (stage == SNAPSHOT_FINAL && reg == EN7570_APD_DAC)
			memcpy(expected, apd_states[APD_STEP_COUNT], 4);
		/* RX outcomes are observations after APD, not safety gates. */
		if (stage == SNAPSHOT_FINAL && reg == EN7570_LOS_CTRL2) {
			if (memcmp(snapshot->value[i], expected, 2) ||
			    snapshot->value[i][3] != expected[3])
				return -EPERM;
			continue;
		}
		if (stage == SNAPSHOT_FINAL &&
		    (reg == EN7570_LOS_TIMEOUT_COUNT ||
		     reg == EN7570_LOS_TIMEOUT))
			continue;
		if (memcmp(snapshot->value[i], expected, sizeof(expected))) {
			dev_err(observer->dev,
				"snapshot mismatch %s@0x%04x: %4ph != %4ph\n",
				en7570_state_regs[i].name, reg,
				snapshot->value[i], expected);
			return -EPERM;
		}
	}

	return 0;
}

static void capture_tx_guards(struct xr500v_rx_apd_observer *observer,
			      struct apd_snapshot *snapshot)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++) {
		snapshot->tx_guard_result[i] =
			en7570_read4(observer, en7570_tx_guards[i].reg,
				     snapshot->tx_guard[i]);
		remember_error(&snapshot->result,
			       snapshot->tx_guard_result[i]);
	}
}

static void capture_critical(struct xr500v_rx_apd_observer *observer,
			     struct apd_snapshot *snapshot, bool post_attempt)
{
	snapshot->gpio_direction = gpiod_get_direction(observer->tx_disable);
	snapshot->gpio_value = gpiod_get_value_cansleep(observer->tx_disable);
	snapshot->gpio_raw_value =
		gpiod_get_raw_value_cansleep(observer->tx_disable);
	snapshot->gpio_active_low = gpiod_is_active_low(observer->tx_disable);
	if (snapshot->gpio_direction < 0)
		remember_error(&snapshot->result, snapshot->gpio_direction);
	if (snapshot->gpio_value < 0)
		remember_error(&snapshot->result, snapshot->gpio_value);
	if (snapshot->gpio_raw_value < 0)
		remember_error(&snapshot->result, snapshot->gpio_raw_value);

	snapshot->physet3 = xpon_read(observer, PHYSET3);
	snapshot->physet10 = xpon_read(observer, PHYSET10);
	snapshot->physta1 = xpon_read(observer, PHYSTA1);
	snapshot->xpon_setting = xpon_read(observer, XPON_SETTING);
	snapshot->misc = xpon_read(observer, MISC);
	snapshot->phy_rx_status = xpon_read(observer, PHYRX_STATUS);
	snapshot->prbs_tx = xpon_read(observer, BISTCTL_PRBS_TX_EN);
	snapshot->test_frame = xpon_read(observer, TEST_FRAME_EN);
	snapshot->xpon_sta = xpon_read(observer, XPON_STA);
	snapshot->xpon_int_en = xpon_read(observer, XPON_INT_EN);

	/* OVP is the first post-attempt read and the final pre-attempt read. */
	if (post_attempt) {
		snapshot->ovp_result =
			en7570_read4(observer, EN7570_APD_OVP_LATCH,
				     snapshot->ovp);
		remember_error(&snapshot->result, snapshot->ovp_result);
	}
	snapshot->apd_result =
		en7570_read4(observer, EN7570_APD_DAC, snapshot->apd);
	remember_error(&snapshot->result, snapshot->apd_result);
	snapshot->safe_result =
		en7570_read4(observer, EN7570_SAFE_PROTECT,
			     snapshot->safe_protect);
	remember_error(&snapshot->result, snapshot->safe_result);
	snapshot->ibias_result =
		en7570_read4(observer, EN7570_IBIAS, snapshot->ibias);
	remember_error(&snapshot->result, snapshot->ibias_result);
	snapshot->imod_result =
		en7570_read4(observer, EN7570_IMOD, snapshot->imod);
	remember_error(&snapshot->result, snapshot->imod_result);
	if (!post_attempt) {
		snapshot->ovp_result =
			en7570_read4(observer, EN7570_APD_OVP_LATCH,
				     snapshot->ovp);
		remember_error(&snapshot->result, snapshot->ovp_result);
	}
}

/*
 * Capture every requested readback even if an earlier I2C read failed.  Slow
 * static TX guards come first for a preflight, leaving APD/OVP immediately
 * before the write.  After a write, OVP/APD are captured before the guards.
 */
static void apd_snapshot_capture(struct xr500v_rx_apd_observer *observer,
				 struct apd_snapshot *snapshot,
				 bool post_attempt)
{
	memset(snapshot, 0, sizeof(*snapshot));
	if (!post_attempt)
		capture_tx_guards(observer, snapshot);
	capture_critical(observer, snapshot, post_attempt);
	if (post_attempt) {
		capture_tx_guards(observer, snapshot);
		/* Preserve the immediate sample and catch a later soft-start latch. */
		snapshot->ovp_terminal_sampled = true;
		snapshot->ovp_terminal_result =
			en7570_read4(observer, EN7570_APD_OVP_LATCH,
				     snapshot->ovp_terminal);
		remember_error(&snapshot->result,
			       snapshot->ovp_terminal_result);
	}
}

static int apd_snapshot_verify(struct xr500v_rx_apd_observer *observer,
			       const struct apd_snapshot *snapshot,
			       const u8 expected_apd[4])
{
	int i;

	if (snapshot->result)
		return snapshot->result;
	if (snapshot->gpio_active_low || snapshot->gpio_direction != 0 ||
	    snapshot->gpio_value != 1 || snapshot->gpio_raw_value != 1) {
		dev_err(observer->dev,
			"TX_DISABLE proof failed: active_low=%u direction=%d logical=%d raw=%d\n",
			snapshot->gpio_active_low, snapshot->gpio_direction,
			snapshot->gpio_value, snapshot->gpio_raw_value);
		return -EPERM;
	}
	if (memcmp(snapshot->apd, expected_apd, sizeof(snapshot->apd))) {
		dev_err(observer->dev, "APD state mismatch: %4ph != %4ph\n",
			snapshot->apd, expected_apd);
		return -EPERM;
	}
	if (memcmp(snapshot->ovp, all_zero, sizeof(snapshot->ovp))) {
		dev_err(observer->dev, "APD OVP state is not zero: %4ph\n",
			snapshot->ovp);
		return -EPERM;
	}
	if (snapshot->ovp_terminal_sampled &&
	    memcmp(snapshot->ovp_terminal, all_zero,
		   sizeof(snapshot->ovp_terminal))) {
		dev_err(observer->dev,
			"terminal APD OVP state is not zero: %4ph\n",
			snapshot->ovp_terminal);
		return -EPERM;
	}
	if (memcmp(snapshot->safe_protect, expected_safe_protect,
		   sizeof(snapshot->safe_protect))) {
		dev_err(observer->dev,
			"SAFE_PROTECT mismatch: %4ph != %4ph\n",
			snapshot->safe_protect, expected_safe_protect);
		return -EPERM;
	}
	if (memcmp(snapshot->ibias, all_zero, sizeof(snapshot->ibias)) ||
	    memcmp(snapshot->imod, all_zero, sizeof(snapshot->imod))) {
		dev_err(observer->dev,
			"TX current state is not zero: Ibias=%4ph Imod=%4ph\n",
			snapshot->ibias, snapshot->imod);
		return -EPERM;
	}
	for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++) {
		if (!memcmp(snapshot->tx_guard[i],
			    en7570_tx_guards[i].expected,
			    sizeof(snapshot->tx_guard[i])))
			continue;
		dev_err(observer->dev,
			"TX guard mismatch %s@0x%04x: %4ph != %4ph\n",
			en7570_tx_guards[i].name, en7570_tx_guards[i].reg,
			snapshot->tx_guard[i], en7570_tx_guards[i].expected);
		return -EPERM;
	}
	if (snapshot->physet3 & PHYSET3_TXEN) {
		dev_err(observer->dev, "xPON TXEN is active\n");
		return -EPERM;
	}
	if (!(snapshot->physet10 & PHYSET10_GPON_MODE)) {
		dev_err(observer->dev, "xPON block is not in GPON mode\n");
		return -EPERM;
	}
	if (snapshot->xpon_setting != XPON_SETTING_EN7570) {
		dev_err(observer->dev,
			"xPON RX polarity mismatch: %08x != %08x\n",
			snapshot->xpon_setting, XPON_SETTING_EN7570);
		return -EPERM;
	}
	if (snapshot->misc & MISC_ROGUE_TX_TEST) {
		dev_err(observer->dev, "xPON rogue-TX test is active\n");
		return -EPERM;
	}
	if (snapshot->prbs_tx || snapshot->test_frame ||
	    snapshot->xpon_int_en) {
		dev_err(observer->dev,
			"xPON TX/test/IRQ gates are active: PRBS=%08x test=%08x irq=%08x\n",
			snapshot->prbs_tx, snapshot->test_frame,
			snapshot->xpon_int_en);
		return -EPERM;
	}

	return 0;
}

/* This is the only EN7570 register-data write helper in the driver. */
static int en7570_fixed_write_once(struct xr500v_rx_apd_observer *observer,
				   unsigned int step)
{
	const struct fixed_write *operation;
	u8 data[6];
	struct i2c_msg message = {
		.addr = observer->en7570->addr,
		.buf = data,
	};
	int ret;

	if (!observer->i2c_bus_locked || !observer->module_pinned ||
	    step >= FIXED_WRITE_COUNT ||
	    observer->i2c_write_attempts != step ||
	    observer->write_attempted[step])
		return -EPERM;
	ret = fast_tx_gate(observer);
	if (ret)
		return ret;

	operation = &fixed_writes[step];
	data[0] = operation->reg >> 8;
	data[1] = operation->reg & 0xff;
	memcpy(data + 2, operation->value, operation->length);
	message.len = 2 + operation->length;

	observer->write_attempted[step] = true;
	observer->i2c_write_attempts++;
	ret = __i2c_transfer(observer->en7570->adapter, &message, 1);
	if (ret < 0)
		observer->write_result[step] = ret;
	else
		observer->write_result[step] = ret == 1 ? 0 : -EIO;
	return observer->write_result[step];
}

static int en7570_read_expect(struct xr500v_rx_apd_observer *observer,
			      u16 reg, void *value, int length,
			      const u8 *expected)
{
	int ret;

	ret = en7570_read(observer, reg, value, length);
	if (ret)
		return ret;
	return memcmp(value, expected, length) ? -EIO : 0;
}

static int run_reset_rssi_gain_los(struct xr500v_rx_apd_observer *observer)
{
	static const u8 zero1[] = { 0x00 };
	static const u8 la_0034[] = { 0x00, 0x34 };
	static const u8 la_0074[] = { 0x00, 0x74 };
	static const u8 svadc_02[] = { 0x02 };
	static const u8 cal_la[] = { 0x00, 0x24, 0x00, 0x00 };
	static const u8 cal_svadc[] = { 0x00, 0x00, 0x01, 0x00 };
	static const u8 cal_probe[] = { 0x00, 0x00, 0x00, 0x00 };
	static const u8 gain_la[] = { 0x00, 0x24, 0x05, 0x00 };
	static const u8 los_trigger_done[] = { 0x06, 0x1f, 0x3c, 0x36 };
	u8 value[4];
	int ret;

	ret = en7570_fixed_write_once(observer, 0);
	if (ret)
		return ret;
	usleep_range(10000, 12000);
	full_snapshot_capture(observer, &observer->post_reset);
	observer->post_reset_verify_result =
		full_snapshot_verify(observer, &observer->post_reset,
				     SNAPSHOT_POST_RESET);
	if (observer->post_reset_verify_result)
		return observer->post_reset_verify_result;

	/* The latch is outside the 29-register map and must start clear. */
	ret = en7570_read_expect(observer, EN7570_ADC_LATCH,
				  &observer->rssi_latch_initial, 1, zero1);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 1);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_LA_PWD, value, 2,
				  la_0034);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 2);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_LA_PWD, value, 2,
				  la_0074);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 3);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_SVADC_PD, value, 1,
				  svadc_02);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 4);
	if (ret)
		return ret;
	ret = en7570_read(observer, EN7570_ADC_PROBE, value, 2);
	if (ret)
		return ret;
	observer->rssi_vref = value[0] | (u16)value[1] << 8;
	if (observer->rssi_vref != 0x020a)
		return -ERANGE;
	ret = en7570_read_expect(observer, EN7570_LA_PWD, value, 2,
				  la_0074);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 5);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_LA_PWD, value, 2,
				  la_0034);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_SVADC_PD, value, 1,
				  svadc_02);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_ADC_LATCH,
				  &observer->rssi_latch_second, 1, zero1);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 6);
	if (ret)
		return ret;
	ret = en7570_read(observer, EN7570_ADC_PROBE, value, 2);
	if (ret)
		return ret;
	observer->rssi_v = value[0] | (u16)value[1] << 8;
	if ((observer->rssi_v != 0x0284 && observer->rssi_v != 0x0285) ||
	    observer->rssi_v - observer->rssi_vref < 0x007a ||
	    observer->rssi_v - observer->rssi_vref > 0x007b)
		return -ERANGE;
	ret = en7570_read_expect(observer, EN7570_LA_PWD, value, 2,
				  la_0034);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 7);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 8);
	if (ret)
		return ret;

	ret = en7570_read_expect(observer, EN7570_LA_PWD,
				  observer->rssi_cal_la, 4, cal_la);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_SVADC_PD,
				  observer->rssi_cal_svadc, 4, cal_svadc);
	if (ret)
		return ret;
	ret = en7570_read(observer, EN7570_ADC_PROBE,
			   observer->rssi_cal_adc, 4);
	if (ret || observer->rssi_cal_adc[0] != (observer->rssi_v & 0xff) ||
	    observer->rssi_cal_adc[1] != (observer->rssi_v >> 8) ||
	    observer->rssi_cal_adc[2] || observer->rssi_cal_adc[3])
		return ret ?: -EIO;
	ret = en7570_read_expect(observer, EN7570_PROBE_CONTROL,
				  observer->rssi_cal_probe, 4, cal_probe);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 9);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_LA_PWD,
				  observer->rssi_gain_la, 4, gain_la);
	if (ret)
		return ret;

	ret = en7570_fixed_write_once(observer, 10);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 11);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 12);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 13);
	if (ret)
		return ret;
	ret = en7570_read_expect(observer, EN7570_LOS_CTRL1,
				  observer->los_trigger_readback, 4,
				  los_trigger_done);
	if (ret)
		return ret;
	ret = en7570_fixed_write_once(observer, 14);
	if (ret)
		return ret;

	usleep_range(20000, 25000);
	full_snapshot_capture(observer, &observer->post_los);
	observer->post_los_verify_result =
		full_snapshot_verify(observer, &observer->post_los,
				     SNAPSHOT_POST_LOS);
	return observer->post_los_verify_result;
}

/* The only MMIO write helper: one fixed RX signal-detect polarity change. */
static int write_en7570_rx_polarity(struct xr500v_rx_apd_observer *observer)
{
	int ret;

	if (observer->mmio_write_attempts || observer->polarity_attempted)
		return -EPERM;
	ret = fast_tx_gate(observer);
	if (ret)
		return ret;
	observer->xpon_setting_before = xpon_read(observer, XPON_SETTING);
	if (observer->xpon_setting_before != XPON_SETTING_RETAINED)
		return -EPERM;

	observer->polarity_attempted = true;
	observer->mmio_write_attempts++;
	iowrite32(XPON_SETTING_EN7570, observer->xpon_base + XPON_SETTING);
	observer->xpon_setting_after = xpon_read(observer, XPON_SETTING);
	if (observer->xpon_setting_after != XPON_SETTING_EN7570 ||
	    ((observer->xpon_setting_before ^ observer->xpon_setting_after) &
	     ~XPON_SETTING_RX_SD_INV))
		return -EIO;
	return gpio_xpon_gate(observer);
}

static int rx_sample_read4(struct xr500v_rx_apd_observer *observer,
			   struct rx_sample *sample, u16 reg, u8 value[4])
{
	sample->result = en7570_read4(observer, reg, value);
	return sample->result;
}

static void rx_sample_capture(struct xr500v_rx_apd_observer *observer,
			      struct rx_sample *sample)
{
	memset(sample, 0, sizeof(*sample));
	/* Safety ordering is intentional: OVP is always the first sample read. */
	if (rx_sample_read4(observer, sample, EN7570_APD_OVP_LATCH,
			    sample->ovp) ||
	    rx_sample_read4(observer, sample, EN7570_APD_DAC, sample->apd) ||
	    rx_sample_read4(observer, sample, EN7570_SAFE_PROTECT,
			    sample->safe_protect) ||
	    rx_sample_read4(observer, sample, EN7570_IBIAS, sample->ibias) ||
	    rx_sample_read4(observer, sample, EN7570_IMOD, sample->imod) ||
	    rx_sample_read4(observer, sample, EN7570_LOS_CTRL1,
			    sample->los_ctrl1) ||
	    rx_sample_read4(observer, sample, EN7570_LOS_CTRL2,
			    sample->los_ctrl2) ||
	    rx_sample_read4(observer, sample, EN7570_LOS_DBG,
			    sample->los_debug) ||
	    rx_sample_read4(observer, sample, EN7570_LOS_TIMEOUT,
			    sample->los_timeout))
		return;

	sample->gpio_direction = gpiod_get_direction(observer->tx_disable);
	sample->gpio_value = gpiod_get_value_cansleep(observer->tx_disable);
	sample->gpio_raw_value =
		gpiod_get_raw_value_cansleep(observer->tx_disable);
	sample->gpio_active_low = gpiod_is_active_low(observer->tx_disable);
	if (sample->gpio_direction < 0)
		sample->result = sample->gpio_direction;
	else if (sample->gpio_value < 0)
		sample->result = sample->gpio_value;
	else if (sample->gpio_raw_value < 0)
		sample->result = sample->gpio_raw_value;
	if (sample->result)
		return;

	sample->physet3 = xpon_read(observer, PHYSET3);
	sample->physet10 = xpon_read(observer, PHYSET10);
	sample->physta1 = xpon_read(observer, PHYSTA1);
	sample->xpon_setting = xpon_read(observer, XPON_SETTING);
	sample->misc = xpon_read(observer, MISC);
	sample->phy_rx_status = xpon_read(observer, PHYRX_STATUS);
	sample->prbs_tx = xpon_read(observer, BISTCTL_PRBS_TX_EN);
	sample->test_frame = xpon_read(observer, TEST_FRAME_EN);
	sample->xpon_sta = xpon_read(observer, XPON_STA);
	sample->xpon_int_en = xpon_read(observer, XPON_INT_EN);
}

static int rx_sample_verify(const struct rx_sample *sample)
{
	static const u8 los1[] = { 0x06, 0x1f, 0x1c, 0x10 };
	static const u8 los2[] = { 0x05, 0x1f, 0x22, 0x00 };

	if (sample->result)
		return sample->result;
	if (memcmp(sample->ovp, all_zero, 4) ||
	    memcmp(sample->apd, apd_states[APD_STEP_COUNT], 4) ||
	    memcmp(sample->safe_protect, expected_safe_protect, 4) ||
	    memcmp(sample->ibias, all_zero, 4) ||
	    memcmp(sample->imod, all_zero, 4) ||
	    memcmp(sample->los_ctrl1, los1, 4) ||
	    memcmp(sample->los_ctrl2, los2, 2) ||
	    sample->los_ctrl2[3] != los2[3])
		return -EPERM;
	if (sample->gpio_active_low || sample->gpio_direction != 0 ||
	    sample->gpio_value != 1 || sample->gpio_raw_value != 1)
		return -EPERM;
	if (sample->physet3 & PHYSET3_TXEN ||
	    !(sample->physet10 & PHYSET10_GPON_MODE) ||
	    sample->xpon_setting != XPON_SETTING_EN7570 ||
	    sample->misc & MISC_ROGUE_TX_TEST || sample->prbs_tx ||
	    sample->test_frame || sample->xpon_int_en)
		return -EPERM;
	return 0;
}

static int run_rx_samples(struct xr500v_rx_apd_observer *observer)
{
	unsigned int i;
	int ret;

	for (i = 0; i < RX_SAMPLE_COUNT; i++) {
		if (i)
			msleep(RX_SAMPLE_INTERVAL_MS);
		rx_sample_capture(observer, &observer->samples[i]);
		observer->samples_taken = i + 1;
		ret = rx_sample_verify(&observer->samples[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int verify_factory_hash(struct xr500v_rx_apd_observer *observer)
{
	struct crypto_shash *sha256;
	struct nvmem_cell *cell;
	void *factory;
	int ret;

	cell = devm_nvmem_cell_get(observer->dev, "factory-bob");
	if (IS_ERR(cell))
		return dev_err_probe(observer->dev, PTR_ERR(cell),
				     "factory-bob NVMEM cell unavailable\n");

	factory = nvmem_cell_read(cell, &observer->factory_length);
	if (IS_ERR(factory))
		return dev_err_probe(observer->dev, PTR_ERR(factory),
				     "cannot read factory-bob NVMEM cell\n");
	if (observer->factory_length != XR500V_FACTORY_LENGTH) {
		ret = -EMSGSIZE;
		dev_err(observer->dev,
			"factory-bob length is 0x%zx, expected 0x%x\n",
			observer->factory_length, XR500V_FACTORY_LENGTH);
		goto out_free;
	}

	sha256 = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(sha256)) {
		ret = PTR_ERR(sha256);
		dev_err(observer->dev, "cannot allocate SHA-256: %d\n", ret);
		goto out_free;
	}
	ret = crypto_shash_tfm_digest(sha256, factory,
				      observer->factory_length,
				       observer->factory_digest);
	crypto_free_shash(sha256);
	if (ret) {
		dev_err(observer->dev, "cannot hash factory-bob: %d\n", ret);
		goto out_free;
	}

	observer->factory_hash_matched =
		!memcmp(observer->factory_digest, xr500v_factory_sha256,
			sizeof(observer->factory_digest));
	if (!observer->factory_hash_matched) {
		dev_err(observer->dev,
			"factory-bob SHA-256 does not match this XR500v\n");
		ret = -EKEYREJECTED;
	}

out_free:
	kfree(factory);
	return ret;
}

static void en7570_put_client(void *data)
{
	struct i2c_client *client = data;

	put_device(&client->dev);
}

static void apd_snapshot_show(struct seq_file *s, const char *label,
			      const struct apd_snapshot *snapshot,
			      int verify_result)
{
	int i;

	seq_printf(s,
		   "%s: capture=%d verify=%d gpio(active_low=%u direction=%d logical=%d raw=%d)\n",
		   label, snapshot->result, verify_result,
		   snapshot->gpio_active_low, snapshot->gpio_direction,
		   snapshot->gpio_value, snapshot->gpio_raw_value);
	seq_printf(s,
		   "  apd=%4ph(r=%d) ovp=%4ph(r=%d) terminal=%4ph(r=%d sampled=%u)\n",
		   snapshot->apd, snapshot->apd_result,
		   snapshot->ovp, snapshot->ovp_result,
		   snapshot->ovp_terminal, snapshot->ovp_terminal_result,
		   snapshot->ovp_terminal_sampled);
	seq_printf(s, "  safe=%4ph(r=%d)\n", snapshot->safe_protect,
		   snapshot->safe_result);
	seq_printf(s,
		   "  ibias=%4ph(r=%d) imod=%4ph(r=%d)\n",
		   snapshot->ibias, snapshot->ibias_result,
		   snapshot->imod, snapshot->imod_result);
	for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++)
		seq_printf(s, "  guard_%s@%04x=%4ph(r=%d)\n",
			   en7570_tx_guards[i].name, en7570_tx_guards[i].reg,
			   snapshot->tx_guard[i], snapshot->tx_guard_result[i]);
	seq_printf(s,
		   "  physet3=%08x txen=%u physet10=%08x gpon=%u physta1=%08x setting=%08x misc=%08x rogue=%u rx=%08x sta=%08x prbs=%08x test=%08x irq=%08x\n",
		   snapshot->physet3,
		   !!(snapshot->physet3 & PHYSET3_TXEN), snapshot->physet10,
		   !!(snapshot->physet10 & PHYSET10_GPON_MODE),
		   snapshot->physta1, snapshot->xpon_setting, snapshot->misc,
		   !!(snapshot->misc & MISC_ROGUE_TX_TEST),
		   snapshot->phy_rx_status, snapshot->xpon_sta,
		   snapshot->prbs_tx, snapshot->test_frame,
		   snapshot->xpon_int_en);
}

static void full_snapshot_show(struct seq_file *s, const char *label,
			       const struct full_snapshot *snapshot,
			       int verify_result)
{
	int i;

	seq_printf(s,
		   "%s: capture=%d verify=%d gpio(active_low=%u direction=%d logical=%d raw=%d)\n",
		   label, snapshot->result, verify_result,
		   snapshot->gpio_active_low, snapshot->gpio_direction,
		   snapshot->gpio_value, snapshot->gpio_raw_value);
	for (i = 0; i < ARRAY_SIZE(en7570_state_regs); i++)
		seq_printf(s, "  %-18s@%04x=%4ph(r=%d)\n",
			   en7570_state_regs[i].name, en7570_state_regs[i].reg,
			   snapshot->value[i], snapshot->reg_result[i]);
	seq_printf(s,
		   "  xpon physet3=%08x physet10=%08x physta1=%08x fsm=%u setting=%08x misc=%08x rx=%08x sync=%x sync_ok=%u rx_hi=%02x fec=%u sta=%08x los=%u prbs=%08x test=%08x irq=%08x\n",
		   snapshot->physet3, snapshot->physet10, snapshot->physta1,
		   (u32)((snapshot->physta1 & PHYSTA1_PHY_CURR) >> 18),
		   snapshot->xpon_setting, snapshot->misc,
		   snapshot->phy_rx_status,
		   (u32)(snapshot->phy_rx_status & PHYRX_SYNC_MASK),
		   (u32)((snapshot->phy_rx_status & PHYRX_SYNC_MASK) ==
			 PHYRX_SYNC_VALUE),
		   (snapshot->phy_rx_status >> 8) & 0xff,
		   (u32)!!(snapshot->phy_rx_status & BIT(15)),
		   snapshot->xpon_sta, !!(snapshot->xpon_sta & XPON_STA_LOS),
		   snapshot->prbs_tx, snapshot->test_frame,
		   snapshot->xpon_int_en);
}

static void rx_sample_show(struct seq_file *s, unsigned int index,
			   const struct rx_sample *sample)
{
	seq_printf(s,
		   "sample_%02u: result=%d ovp=%4ph apd=%4ph safe=%4ph ibias=%4ph imod=%4ph los1=%4ph los2=%4ph losdbg=%4ph timeout=%4ph\n",
		   index, sample->result, sample->ovp, sample->apd,
		   sample->safe_protect, sample->ibias, sample->imod,
		   sample->los_ctrl1, sample->los_ctrl2, sample->los_debug,
		   sample->los_timeout);
	seq_printf(s,
		   "  gpio(active_low=%u direction=%d logical=%d raw=%d) physet3=%08x physet10=%08x physta1=%08x fsm=%u setting=%08x misc=%08x rx=%08x sync=%x sync_ok=%u rx_hi=%02x fec=%u sta=%08x los=%u prbs=%08x test=%08x irq=%08x\n",
		   sample->gpio_active_low, sample->gpio_direction,
		   sample->gpio_value, sample->gpio_raw_value,
		   sample->physet3, sample->physet10, sample->physta1,
		   (u32)((sample->physta1 & PHYSTA1_PHY_CURR) >> 18),
		   sample->xpon_setting, sample->misc, sample->phy_rx_status,
		   (u32)(sample->phy_rx_status & PHYRX_SYNC_MASK),
		   (u32)((sample->phy_rx_status & PHYRX_SYNC_MASK) ==
			 PHYRX_SYNC_VALUE),
		   (sample->phy_rx_status >> 8) & 0xff,
		   (u32)!!(sample->phy_rx_status & BIT(15)), sample->xpon_sta,
		   !!(sample->xpon_sta & XPON_STA_LOS), sample->prbs_tx,
		   sample->test_frame, sample->xpon_int_en);
}

static int apd_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_rx_apd_observer *observer = s->private;
	int i;

	seq_puts(s, "operation:             fixed OEM EN7570 reset/RSSI/gain/LOS + RX polarity + APD A2\n");
	seq_printf(s, "silicon_id:            0x%02x\n", observer->silicon_id);
	seq_printf(s, "factory_length:        0x%zx\n",
		   observer->factory_length);
	seq_printf(s, "factory_sha256:        %*phN\n", SHA256_DIGEST_SIZE,
		   observer->factory_digest);
	seq_printf(s, "factory_hash_matched:  %s\n",
		   observer->factory_hash_matched ? "yes" : "NO");
	seq_printf(s, "tx_disable_gpio:       %d (hardware offset %d)\n",
		   observer->tx_disable_gpio, observer->tx_disable_offset);
	seq_printf(s, "module_pinned:         %s\n",
		   observer->module_pinned ? "yes" : "NO");
	seq_printf(s, "adapter_retries:       saved=%d during=0 restored=%s\n",
		   observer->adapter_retries_saved,
		   observer->adapter_retries_restored ? "yes" : "NO");
	seq_printf(s, "i2c_write_attempts:    %u / %u maximum\n",
		   observer->i2c_write_attempts, FIXED_WRITE_COUNT);
	seq_printf(s, "mmio_write_attempts:   %u / 1 maximum\n",
		   observer->mmio_write_attempts);
	seq_printf(s, "sequence_result:       %d\n", observer->sequence_result);
	seq_printf(s, "halted_step:           %d\n", observer->halted_step);
	seq_printf(s,
		   "rssi_oracle:          vref=0x%04x v=0x%04x delta=0x%04x latch_initial=%02x latch_second=%02x\n",
		   observer->rssi_vref, observer->rssi_v,
		   observer->rssi_v >= observer->rssi_vref ?
		   observer->rssi_v - observer->rssi_vref : 0,
		   observer->rssi_latch_initial, observer->rssi_latch_second);
	seq_printf(s,
		   "rssi_readbacks:       la=%4ph svadc=%4ph adc=%4ph probe=%4ph gain_la=%4ph los_trigger=%4ph\n",
		   observer->rssi_cal_la, observer->rssi_cal_svadc,
		   observer->rssi_cal_adc, observer->rssi_cal_probe,
		   observer->rssi_gain_la, observer->los_trigger_readback);
	seq_printf(s,
		   "rx_polarity:          attempted=%s result=%d before=%08x after=%08x\n",
		   observer->polarity_attempted ? "yes" : "no",
		   observer->polarity_result, observer->xpon_setting_before,
		   observer->xpon_setting_after);
	full_snapshot_show(s, "cold", &observer->cold,
			   observer->cold_verify_result);
	full_snapshot_show(s, "post_reset", &observer->post_reset,
			   observer->post_reset_verify_result);
	full_snapshot_show(s, "post_los", &observer->post_los,
			   observer->post_los_verify_result);
	full_snapshot_show(s, "final", &observer->final,
			   observer->final_verify_result);
	for (i = 0; i < FIXED_WRITE_COUNT; i++)
		seq_printf(s,
			   "write_%02d: reg=%04x len=%u payload=%*ph attempted=%s result=%d\n",
			   i + 1, fixed_writes[i].reg, fixed_writes[i].length,
			   fixed_writes[i].length, fixed_writes[i].value,
			   observer->write_attempted[i] ? "yes" : "no",
			   observer->write_result[i]);
	for (i = 0; i < APD_STEP_COUNT; i++) {
		const struct apd_step_result *step = &observer->steps[i];

		seq_printf(s, "apd_step_%d_write:      attempted=%s result=%d\n",
			   i + 1, step->write_attempted ? "yes" : "no",
			   step->write_result);
		apd_snapshot_show(s, "  pre", &step->pre,
				  step->pre_verify_result);
		apd_snapshot_show(s, "  post", &step->post,
				  step->post_verify_result);
	}
	seq_printf(s, "samples_taken:         %u / %u\n",
		   observer->samples_taken, RX_SAMPLE_COUNT);
	seq_printf(s, "sample_result:         %d\n", observer->sample_result);
	for (i = 0; i < observer->samples_taken; i++)
		rx_sample_show(s, i, &observer->samples[i]);
	seq_printf(s, "tx_disable_asserted:   %s\n",
		   !gpiod_is_active_low(observer->tx_disable) &&
		   gpiod_get_direction(observer->tx_disable) == 0 &&
		   gpiod_get_value_cansleep(observer->tx_disable) == 1 &&
		   gpiod_get_raw_value_cansleep(observer->tx_disable) == 1 ?
		   "yes" : "NO");
	seq_printf(s, "physical_powercut_required: %s\n",
		   observer->physical_powercut_required ? "yes" : "no");
	seq_puts(s, "observer_retry_count:  0\n");
	seq_puts(s, "software_rollback:     impossible/not attempted\n");
	seq_puts(s, "esd_or_deglitch_write: no\n");
	seq_puts(s, "thermal_or_periodic_apd: no\n");
	seq_puts(s, "tx_current_laser_tgen: no\n");
	seq_puts(s, "arbitrary_write_path:  no\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apd_status);

static void apd_results_init(struct xr500v_rx_apd_observer *observer)
{
	int i;

	observer->sequence_result = -ECANCELED;
	observer->halted_step = 0;
	observer->cold.result = -ECANCELED;
	observer->post_reset.result = -ECANCELED;
	observer->post_los.result = -ECANCELED;
	observer->final.result = -ECANCELED;
	observer->cold_verify_result = -ECANCELED;
	observer->post_reset_verify_result = -ECANCELED;
	observer->post_los_verify_result = -ECANCELED;
	observer->final_verify_result = -ECANCELED;
	observer->sample_result = -ECANCELED;
	observer->polarity_result = -ECANCELED;
	for (i = 0; i < FIXED_WRITE_COUNT; i++)
		observer->write_result[i] = -ECANCELED;
	for (i = 0; i < APD_STEP_COUNT; i++) {
		observer->steps[i].pre.result = -ECANCELED;
		observer->steps[i].post.result = -ECANCELED;
		observer->steps[i].pre_verify_result = -ECANCELED;
		observer->steps[i].write_result = -ECANCELED;
		observer->steps[i].post_verify_result = -ECANCELED;
	}
}

static int rx_apd_run_pinned_sequence(struct xr500v_rx_apd_observer *observer)
{
	int ret;
	int i;

	/* Globally consume the one sequence allowed for this module load. */
	if (atomic_cmpxchg(&rx_apd_sequence_claimed, 0, 1))
		return -EBUSY;

	/* The first transfer attempt is the irreversible boundary. */
	__module_get(THIS_MODULE);
	observer->module_pinned = true;
	observer->physical_powercut_required = true;

	ret = run_reset_rssi_gain_los(observer);
	if (ret)
		goto halted;

	observer->polarity_result = write_en7570_rx_polarity(observer);
	if (observer->polarity_result) {
		ret = observer->polarity_result;
		goto halted;
	}

	for (i = 0; i < APD_STEP_COUNT; i++) {
		struct apd_step_result *step = &observer->steps[i];
		unsigned int write_index = APD_FIRST_WRITE + i;

		apd_snapshot_capture(observer, &step->pre, false);
		step->pre_verify_result =
			apd_snapshot_verify(observer, &step->pre, apd_states[i]);
		if (step->pre_verify_result) {
			ret = step->pre_verify_result;
			goto halted;
		}

		step->write_result =
			en7570_fixed_write_once(observer, write_index);
		step->write_attempted = observer->write_attempted[write_index];

		/* Always capture a post-attempt snapshot, including on bus error. */
		apd_snapshot_capture(observer, &step->post, true);
		step->post_verify_result =
			apd_snapshot_verify(observer, &step->post,
					    apd_states[i + 1]);
		if (step->write_result || step->post_verify_result) {
			ret = step->write_result ?: step->post_verify_result;
			goto halted;
		}
	}

	full_snapshot_capture(observer, &observer->final);
	observer->final_verify_result =
		full_snapshot_verify(observer, &observer->final, SNAPSHOT_FINAL);
	if (observer->final_verify_result) {
		ret = observer->final_verify_result;
		goto halted;
	}

	observer->sample_result = run_rx_samples(observer);
	if (observer->sample_result) {
		ret = observer->sample_result;
		observer->halted_step = FIXED_WRITE_COUNT + 1;
		goto halted_no_capture;
	}

	observer->sequence_result = 0;
	observer->halted_step = 0;
	return 0;

halted:
	observer->halted_step = observer->i2c_write_attempts ?:
				observer->polarity_attempted ? PREFIX_WRITE_COUNT + 1 : 1;
	/* No recovery write is legal.  Preserve a best-effort terminal map. */
	if (observer->final.result == -ECANCELED)
		full_snapshot_capture(observer, &observer->final);
halted_no_capture:
	observer->sequence_result = ret;

	/* Once claimed, every exit is pinned and probe must remain successful. */
	return 0;
}

static int xr500v_rx_apd_observer_probe(struct platform_device *pdev)
{
	struct xr500v_rx_apd_observer *observer;
	struct gpio_chip *gpio_chip;
	struct device_node *node;
	struct resource pon_i2c_resource;
	struct resource *xpon_resource;
	int ret;

	if (!arm_en7570_rx_apd_a2)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "module RX/APD A2 opt-in is absent\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-rx-apd-a2"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT RX/APD A2 opt-in is absent\n");

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;
	observer->dev = &pdev->dev;
	apd_results_init(observer);

	/* Reserve and prove the retained board TX kill without changing it. */
	observer->tx_disable = devm_gpiod_get(&pdev->dev, "tx-disable",
					      GPIOD_ASIS);
	if (IS_ERR(observer->tx_disable))
		return dev_err_probe(&pdev->dev, PTR_ERR(observer->tx_disable),
				     "cannot acquire physical TX_DISABLE\n");
	if (gpiod_is_active_low(observer->tx_disable))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "TX_DISABLE must be declared active-high\n");

	/* Reject a syntactically valid DT phandle to any line except GPIO16. */
	gpio_chip = gpiod_to_chip(observer->tx_disable);
	observer->tx_disable_gpio = desc_to_gpio(observer->tx_disable);
	if (!gpio_chip || !gpio_chip->label || gpio_chip->base < 0 ||
	    observer->tx_disable_gpio < gpio_chip->base ||
	    strcmp(gpio_chip->label, "tc3162-gpio"))
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "TX_DISABLE is not on the TC3162 GPIO bank\n");
	observer->tx_disable_offset =
		observer->tx_disable_gpio - gpio_chip->base;
	if (observer->tx_disable_offset != XR500V_TX_DISABLE_GPIO)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "TX_DISABLE hardware offset is %d, expected %d\n",
				     observer->tx_disable_offset,
				     XR500V_TX_DISABLE_GPIO);

	/*
	 * tc3162 .direction_output enables the pad before writing its latch and
	 * is not glitch-free.  Require the cold boot state to be output/raw-high
	 * already; never call a GPIO direction or set operation here.
	 */
	if (gpiod_get_direction(observer->tx_disable) != 0 ||
	    gpiod_get_value_cansleep(observer->tx_disable) != 1 ||
	    gpiod_get_raw_value_cansleep(observer->tx_disable) != 1)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "retained physical TX_DISABLE raw-high proof failed\n");

	xpon_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!xpon_resource || xpon_resource->start != XR500V_XPON_BASE ||
	    resource_size(xpon_resource) != XR500V_XPON_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "xPON resource is not exactly 0x%x/0x%x\n",
				     XR500V_XPON_BASE, XR500V_XPON_SIZE);
	observer->xpon_base = devm_ioremap_resource(&pdev->dev, xpon_resource);
	if (IS_ERR(observer->xpon_base))
		return PTR_ERR(observer->xpon_base);

	node = of_parse_phandle(pdev->dev.of_node, "airoha,en7570", 0);
	if (!node)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "airoha,en7570 phandle is absent\n");
	if (!of_device_is_compatible(node, "airoha,en7570-diag")) {
		of_node_put(node);
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "EN7570 phandle compatible mismatch\n");
	}
	observer->en7570 = of_find_i2c_device_by_node(node);
	of_node_put(node);
	if (!observer->en7570)
		return -EPROBE_DEFER;
	ret = devm_add_action_or_reset(&pdev->dev, en7570_put_client,
				       observer->en7570);
	if (ret)
		return ret;
	if (!i2c_check_functionality(observer->en7570->adapter, I2C_FUNC_I2C))
		return dev_err_probe(&pdev->dev, -EOPNOTSUPP,
				     "EN7570 adapter lacks raw I2C transfers\n");
	if ((observer->en7570->flags & I2C_CLIENT_TEN) ||
	    observer->en7570->addr != EN7570_I2C_ADDRESS)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "EN7570 endpoint flags=0x%x addr=0x%02x, expected 7-bit 0x%02x\n",
				     observer->en7570->flags,
				     observer->en7570->addr, EN7570_I2C_ADDRESS);
	if (!observer->en7570->adapter->dev.of_node ||
	    !of_device_is_compatible(observer->en7570->adapter->dev.of_node,
				     "mediatek,mt7621-i2c") ||
	    of_address_to_resource(observer->en7570->adapter->dev.of_node, 0,
				   &pon_i2c_resource) ||
	    pon_i2c_resource.start != XR500V_PON_I2C_BASE ||
	    resource_size(&pon_i2c_resource) != XR500V_PON_I2C_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "EN7570 is not on the exact XR500v PON-I2C controller\n");

	ret = verify_factory_hash(observer);
	if (ret)
		return ret;

	/*
	 * No other kernel client or i2c-dev transfer may interleave with the
	 * identity, final cold preflight, writes, or readbacks.  Calling the
	 * locking i2c_transfer() while this is held would deadlock; all observer
	 * accesses use __i2c_transfer().
	 */
	i2c_lock_bus(observer->en7570->adapter, I2C_LOCK_SEGMENT);
	observer->adapter_retries_saved = observer->en7570->adapter->retries;
	observer->en7570->adapter->retries = 0;
	observer->i2c_bus_locked = true;

	ret = en7570_read(observer, EN7570_ID,
			  &observer->silicon_id, sizeof(observer->silicon_id));
	if (ret || observer->silicon_id != EN7570_EXPECTED_ID) {
		ret = ret ?: -ENODEV;
		dev_err(&pdev->dev, "EN7570 identity mismatch: 0x%02x (%d)\n",
			observer->silicon_id, ret);
		goto out_unlock;
	}

	/* The locked 29-register map is the final cold-state authority. */
	full_snapshot_capture(observer, &observer->cold);
	observer->cold_verify_result =
		full_snapshot_verify(observer, &observer->cold, SNAPSHOT_COLD);
	if (observer->cold_verify_result) {
		ret = observer->cold_verify_result;
		dev_err(&pdev->dev, "exact RX/APD cold preflight failed: %d\n",
			ret);
		goto out_unlock;
	}

	ret = rx_apd_run_pinned_sequence(observer);
	if (ret) {
		observer->sequence_result = ret;
		observer->halted_step = 1;
		goto out_unlock;
	}

out_unlock:
	observer->i2c_bus_locked = false;
	observer->en7570->adapter->retries = observer->adapter_retries_saved;
	observer->adapter_retries_restored = true;
	i2c_unlock_bus(observer->en7570->adapter, I2C_LOCK_SEGMENT);
	if (ret)
		return ret;

	observer->debugfs_dir =
		debugfs_create_dir("xr500v-en7570-rx-apd-a2-observer", NULL);
	if (IS_ERR_OR_NULL(observer->debugfs_dir)) {
		dev_err(&pdev->dev,
			"debugfs directory unavailable after RX/APD attempt: %d\n",
			PTR_ERR_OR_ZERO(observer->debugfs_dir));
		observer->debugfs_dir = NULL;
	} else {
		observer->debugfs_status =
			debugfs_create_file("status", 0444, observer->debugfs_dir,
					    observer, &apd_status_fops);
		if (IS_ERR_OR_NULL(observer->debugfs_status))
			dev_err(&pdev->dev,
				"debugfs status unavailable after RX/APD attempt: %d\n",
				PTR_ERR_OR_ZERO(observer->debugfs_status));
	}

	platform_set_drvdata(pdev, observer);
	dev_warn(&pdev->dev,
		 "EN7570 RX/APD A2 result %d after %u I2C and %u MMIO write attempt(s), %u sample(s); TX_DISABLE retained; physical power removal required\n",
		 observer->sequence_result, observer->i2c_write_attempts,
		 observer->mmio_write_attempts, observer->samples_taken);
	return 0;
}

static void xr500v_rx_apd_observer_remove(struct platform_device *pdev)
{
	struct xr500v_rx_apd_observer *observer = platform_get_drvdata(pdev);

	debugfs_remove_recursive(observer->debugfs_dir);
	dev_emerg(&pdev->dev,
		  "RX/APD observer removal has no rollback; remove physical power now\n");
}

static const struct of_device_id xr500v_rx_apd_observer_of_match[] = {
	{ .compatible = "econet,en751221-en7570-rx-apd-a2-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_rx_apd_observer_of_match);

static struct platform_driver xr500v_rx_apd_observer_driver = {
	.probe = xr500v_rx_apd_observer_probe,
	.remove = xr500v_rx_apd_observer_remove,
	.driver = {
		.name = "xr500v-en7570-rx-apd-a2-observer",
		.of_match_table = xr500v_rx_apd_observer_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(xr500v_rx_apd_observer_driver);

MODULE_DESCRIPTION("Guarded XR500v EN7570 fixed RX/APD A2 observer");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
