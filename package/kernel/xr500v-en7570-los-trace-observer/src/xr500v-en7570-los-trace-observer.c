// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot EN7570 terminal LOS trace observer for the TP-Link XR500v.
 *
 * This deliberately non-shipping driver accepts only one exact unit and one
 * exact cold state. With physical TX_DISABLE retained high, it performs only
 * the 15 fixed writes already observed in the reset/RSSI/gain/LOS prefix and
 * then takes 12 finite timestamped read-only samples. There is no xPON MMIO
 * write, APD write, retry, worker, arbitrary payload, rollback, disable,
 * TX-current or laser path. The module self-pins before the first transfer;
 * physical power removal is the only recovery boundary after any attempt.
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
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
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
#define EN7570_LOS_CAL_TIMER	0x0124
#define EN7570_LOS_TIMEOUT_COUNT 0x0128
#define EN7570_LOS_TIMEOUT	0x012c
#define EN7570_LOS_DBG		0x0130
#define EN7570_IBIAS		0x0138
#define EN7570_IMOD		0x0148
#define EN7570_ADC_PROBE		0x0154
#define EN7570_PROBE_CONTROL	0x0158
#define EN7570_ADC_LATCH		0x0159
#define EN7570_VARIANT		0x015c
#define EN7570_APD_OVP_LATCH	0x0164
#define EN7570_ID		0x0170
#define EN7570_SW_RESET		0x0300
#define EN7570_EXPECTED_ID	0x03
#define EN7570_EXPECTED_VARIANT	0x01

#define EN7570_I2C_ADDRESS	0x70
#define XR500V_PON_I2C_BASE	0x1fbf8000
#define XR500V_PON_I2C_SIZE	0x100
#define XR500V_XPON_BASE	0x1faf0000
#define XR500V_XPON_SIZE	0x1000

#define XR500V_FACTORY_LENGTH	0x190
#define XR500V_TX_DISABLE_GPIO	16
#define FIXED_WRITE_COUNT	15
#define TRACE_SAMPLE_COUNT	12
#define RSSI_VREF_MIN		0x020a
#define RSSI_VREF_MAX		0x020b
#define RSSI_V_MIN		0x0284
#define RSSI_V_MAX		0x0286
#define RSSI_DELTA_MIN		0x007a
#define RSSI_DELTA_MAX		0x007b

static bool arm_en7570_los_trace;
module_param(arm_en7570_los_trace, bool, 0444);
MODULE_PARM_DESC(arm_en7570_los_trace,
		 "Arm the one-shot, non-transactional XR500v EN7570 terminal LOS trace observer");

static atomic_t los_trace_sequence_claimed = ATOMIC_INIT(0);

static const u8 xr500v_factory_sha256[SHA256_DIGEST_SIZE] = {
	0x40, 0x1d, 0xfd, 0xae, 0xe7, 0x7c, 0x84, 0x64,
	0x9b, 0xda, 0x10, 0x0f, 0xd5, 0xdd, 0x85, 0xbe,
	0x01, 0xc7, 0xea, 0x12, 0x6d, 0x0a, 0x5c, 0xc2,
	0x11, 0x6b, 0x14, 0x1c, 0x1a, 0x07, 0xa5, 0xe4,
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
};

static const u8 expected_apd[4] = { 0x00, 0x08, 0x00, 0x00 };
static const u8 expected_safe_protect[4] = { 0xff, 0x8f, 0xff, 0x0f };
static const u8 all_zero[4];

struct en7570_state_reg {
	const char *name;
	u16 reg;
	u8 cold[4];
};

struct en7570_guard_reg {
	const char *name;
	u16 reg;
	u8 expected[4];
};

/* Exact post-LOS values for the static TX controls checked in full samples. */
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
	{ "los_timer", EN7570_LOS_CAL_TIMER, { 0xff, 0xff, 0xff, 0xff } },
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
	SNAPSHOT_TERMINAL,
};

enum trace_guard_level {
	TRACE_GUARD_DENSE,
	TRACE_GUARD_CRITICAL,
	TRACE_GUARD_FULL,
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

struct los_trace_sample {
	int result;
	int verify_result;
	int outcome_result;
	int los_ctrl2_result;
	int los_timer_result;
	int timeout_count_result;
	int timeout_result;
	int los_debug_result;
	int los_ctrl1_result;
	int svadc_result;
	int apd_result;
	int ovp_result;
	int safe_result;
	int ibias_result;
	int imod_result;
	int tx_guard_result[ARRAY_SIZE(en7570_tx_guards)];
	enum trace_guard_level guard_level;
	u32 target_ms;
	u64 start_ns;
	u64 los_ctrl2_done_ns;
	u64 end_ns;
	u8 los_ctrl2[4];
	u8 los_timer[4];
	u8 timeout_count[4];
	u8 timeout[4];
	u8 los_debug[4];
	u8 los_ctrl1[4];
	u8 svadc[4];
	u8 apd[4];
	u8 ovp[4];
	u8 safe[4];
	u8 ibias[4];
	u8 imod[4];
	u8 tx_guard[ARRAY_SIZE(en7570_tx_guards)][4];
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

struct xr500v_los_trace_observer {
	struct device *dev;
	void __iomem *xpon_base;
	struct gpio_desc *tx_disable;
	struct i2c_client *en7570;
	struct mutex status_lock; /* protects published status during probe */
	struct dentry *debugfs_dir;
	struct dentry *debugfs_status;
	struct full_snapshot cold;
	struct full_snapshot post_reset;
	struct full_snapshot terminal;
	struct los_trace_sample samples[TRACE_SAMPLE_COUNT];
	bool write_attempted[FIXED_WRITE_COUNT];
	int write_result[FIXED_WRITE_COUNT];
	u64 write_start_ns[FIXED_WRITE_COUNT];
	u64 write_done_ns[FIXED_WRITE_COUNT];
	u64 prefix_end_ns;
	u8 silicon_id;
	u8 silicon_variant;
	u8 factory_digest[SHA256_DIGEST_SIZE];
	u8 rssi_latch_initial;
	u8 rssi_latch_second;
	u8 rssi_cal_la[4];
	u8 rssi_cal_svadc[4];
	u8 rssi_cal_adc[4];
	u8 rssi_cal_probe[4];
	u8 rssi_gain_la[4];
	u8 los_trigger_readback[4];
	u8 first_byte2;
	u8 final_byte2;
	size_t factory_length;
	u16 rssi_vref;
	u16 rssi_v;
	int tx_disable_gpio;
	int tx_disable_offset;
	int adapter_retries_saved;
	unsigned int i2c_write_attempts;
	unsigned int samples_taken;
	unsigned int outcome_22_count;
	unsigned int outcome_23_count;
	unsigned int outcome_other_count;
	unsigned int transition_count;
	int sequence_result;
	int halted_step;
	int halted_sample;
	int cold_verify_result;
	int post_reset_verify_result;
	int terminal_verify_result;
	int terminal_outcome_result;
	int trace_result;
	int outcome_result;
	bool first_byte2_valid;
	bool factory_hash_matched;
	bool adapter_retries_restored;
	bool i2c_bus_locked;
	bool module_pinned;
	bool physical_powercut_required;
};

static u32 xpon_read(struct xr500v_los_trace_observer *observer, u32 reg)
{
	return ioread32(observer->xpon_base + reg);
}

static int en7570_read(struct xr500v_los_trace_observer *observer, u16 reg,
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

static int en7570_read4(struct xr500v_los_trace_observer *observer, u16 reg,
			u8 value[4])
{
	return en7570_read(observer, reg, value, 4);
}

static void remember_error(int *result, int candidate)
{
	if (!*result && candidate)
		*result = candidate;
}

static int gpio_xpon_gate(struct xr500v_los_trace_observer *observer)
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
static int fast_tx_gate(struct xr500v_los_trace_observer *observer)
{
	u8 value[4];
	int ret;

	if (!observer->i2c_bus_locked || !observer->module_pinned)
		return -EPERM;
	if (observer->silicon_id != EN7570_EXPECTED_ID ||
	    observer->silicon_variant != EN7570_EXPECTED_VARIANT)
		return -ENODEV;
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

static void full_snapshot_capture(struct xr500v_los_trace_observer *observer,
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

static int full_snapshot_verify(struct xr500v_los_trace_observer *observer,
				const struct full_snapshot *snapshot,
				enum full_snapshot_stage stage)
{
	static const u8 terminal_la[] = { 0x00, 0x24, 0x05, 0x00 };
	static const u8 terminal_svadc[] = { 0x00, 0x00, 0x41, 0x04 };
	static const u8 terminal_ctrl1[] = { 0x06, 0x1f, 0x1c, 0x10 };
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
	if (snapshot->xpon_setting != XPON_SETTING_RETAINED)
		return -EPERM;

	for (i = 0; i < ARRAY_SIZE(en7570_state_regs); i++) {
		const u16 reg = en7570_state_regs[i].reg;

		memcpy(expected, en7570_state_regs[i].cold, sizeof(expected));
		if (stage == SNAPSHOT_TERMINAL) {
			if (reg == EN7570_LA_PWD) {
				memcpy(expected, terminal_la, 4);
			} else if (reg == EN7570_SVADC_PD) {
				memcpy(expected, terminal_svadc, 4);
			} else if (reg == EN7570_LOS_CTRL1) {
				memcpy(expected, terminal_ctrl1, 4);
			} else if (reg == EN7570_ADC_PROBE) {
				expected[0] = observer->rssi_v & 0xff;
				expected[1] = observer->rssi_v >> 8;
			}
		}
		/*
		 * LOS_CTRL2 byte 2, the calibration timer and timeout registers are
		 * terminal outcomes. They are recorded raw and never authorize work.
		 */
		if (stage == SNAPSHOT_TERMINAL && reg == EN7570_LOS_CTRL2) {
			if (snapshot->value[i][0] != 0x05 ||
			    snapshot->value[i][1] != 0x1f ||
			    snapshot->value[i][3] != 0x00)
				return -EPERM;
			continue;
		}
		if (stage == SNAPSHOT_TERMINAL &&
		    (reg == EN7570_LOS_CAL_TIMER ||
		     reg == EN7570_LOS_TIMEOUT_COUNT ||
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

/* This is the only EN7570 register-data write helper in the driver. */
static int en7570_fixed_write_once(struct xr500v_los_trace_observer *observer,
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
	observer->write_start_ns[step] = ktime_get_ns();
	ret = __i2c_transfer(observer->en7570->adapter, &message, 1);
	observer->write_done_ns[step] = ktime_get_ns();
	if (ret < 0)
		observer->write_result[step] = ret;
	else
		observer->write_result[step] = ret == 1 ? 0 : -EIO;
	return observer->write_result[step];
}

static int en7570_read_expect(struct xr500v_los_trace_observer *observer,
			      u16 reg, void *value, int length,
			      const u8 *expected)
{
	int ret;

	ret = en7570_read(observer, reg, value, length);
	if (ret)
		return ret;
	return memcmp(value, expected, length) ? -EIO : 0;
}

static int run_reset_rssi_gain_los_prefix(struct xr500v_los_trace_observer *observer)
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
	if (observer->rssi_vref < RSSI_VREF_MIN ||
	    observer->rssi_vref > RSSI_VREF_MAX)
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
	if (observer->rssi_v < RSSI_V_MIN || observer->rssi_v > RSSI_V_MAX ||
	    observer->rssi_v <= observer->rssi_vref ||
	    observer->rssi_v - observer->rssi_vref < RSSI_DELTA_MIN ||
	    observer->rssi_v - observer->rssi_vref > RSSI_DELTA_MAX)
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

	observer->prefix_end_ns = observer->write_done_ns[FIXED_WRITE_COUNT - 1];
	return 0;
}

struct trace_schedule_entry {
	u32 target_ms;
	enum trace_guard_level guard_level;
};

static const struct trace_schedule_entry trace_schedule[TRACE_SAMPLE_COUNT] = {
	{ 0, TRACE_GUARD_DENSE },
	{ 5, TRACE_GUARD_DENSE },
	{ 10, TRACE_GUARD_DENSE },
	{ 20, TRACE_GUARD_DENSE },
	{ 50, TRACE_GUARD_CRITICAL },
	{ 100, TRACE_GUARD_FULL },
	{ 250, TRACE_GUARD_FULL },
	{ 500, TRACE_GUARD_FULL },
	{ 1000, TRACE_GUARD_FULL },
	{ 2000, TRACE_GUARD_FULL },
	{ 5000, TRACE_GUARD_FULL },
	{ 10000, TRACE_GUARD_FULL },
};

static void wait_until_trace_target(struct xr500v_los_trace_observer *observer,
				    u32 target_ms)
{
	u64 deadline = observer->prefix_end_ns +
		       (u64)target_ms * NSEC_PER_MSEC;

	for (;;) {
		u64 now = ktime_get_ns();
		u64 remaining;
		unsigned int sleep_us;

		if (now >= deadline)
			return;
		remaining = deadline - now;
		if (remaining > 25 * NSEC_PER_MSEC) {
			msleep(div_u64(remaining - 10 * NSEC_PER_MSEC,
				       NSEC_PER_MSEC));
			continue;
		}
		sleep_us = max_t(unsigned int, 1,
				 div_u64(remaining, NSEC_PER_USEC));
		usleep_range(sleep_us, sleep_us + 100);
	}
}

static void trace_sample_capture(struct xr500v_los_trace_observer *observer,
				 struct los_trace_sample *sample,
				 u32 target_ms,
				 enum trace_guard_level guard_level)
{
	int i;

	memset(sample, 0, sizeof(*sample));
	sample->verify_result = -ECANCELED;
	sample->outcome_result = -ECANCELED;
	sample->timeout_count_result = -ECANCELED;
	sample->los_ctrl1_result = -ECANCELED;
	sample->svadc_result = -ECANCELED;
	sample->apd_result = -ECANCELED;
	sample->ovp_result = -ECANCELED;
	sample->safe_result = -ECANCELED;
	sample->ibias_result = -ECANCELED;
	sample->imod_result = -ECANCELED;
	for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++)
		sample->tx_guard_result[i] = -ECANCELED;
	sample->guard_level = guard_level;
	sample->target_ms = target_ms;
	sample->start_ns = ktime_get_ns();

	/* LOS_CTRL2 is intentionally the first I2C read in every trace sample. */
	sample->los_ctrl2_result =
		en7570_read4(observer, EN7570_LOS_CTRL2, sample->los_ctrl2);
	remember_error(&sample->result, sample->los_ctrl2_result);
	sample->los_ctrl2_done_ns = ktime_get_ns();
	sample->los_timer_result =
		en7570_read4(observer, EN7570_LOS_CAL_TIMER, sample->los_timer);
	remember_error(&sample->result, sample->los_timer_result);
	sample->timeout_result =
		en7570_read4(observer, EN7570_LOS_TIMEOUT, sample->timeout);
	remember_error(&sample->result, sample->timeout_result);
	sample->los_debug_result =
		en7570_read4(observer, EN7570_LOS_DBG, sample->los_debug);
	remember_error(&sample->result, sample->los_debug_result);

	if (guard_level >= TRACE_GUARD_CRITICAL) {
		sample->timeout_count_result =
			en7570_read4(observer, EN7570_LOS_TIMEOUT_COUNT,
				     sample->timeout_count);
		remember_error(&sample->result,
			       sample->timeout_count_result);
		sample->los_ctrl1_result =
			en7570_read4(observer, EN7570_LOS_CTRL1,
				     sample->los_ctrl1);
		remember_error(&sample->result, sample->los_ctrl1_result);
		sample->svadc_result =
			en7570_read4(observer, EN7570_SVADC_PD, sample->svadc);
		remember_error(&sample->result, sample->svadc_result);

		/* Re-prove the critical optical barriers at each checkpoint. */
		sample->ovp_result =
			en7570_read4(observer, EN7570_APD_OVP_LATCH,
				     sample->ovp);
		remember_error(&sample->result, sample->ovp_result);
		sample->apd_result =
			en7570_read4(observer, EN7570_APD_DAC, sample->apd);
		remember_error(&sample->result, sample->apd_result);
		sample->safe_result =
			en7570_read4(observer, EN7570_SAFE_PROTECT,
				     sample->safe);
		remember_error(&sample->result, sample->safe_result);
		sample->ibias_result =
			en7570_read4(observer, EN7570_IBIAS, sample->ibias);
		remember_error(&sample->result, sample->ibias_result);
		sample->imod_result =
			en7570_read4(observer, EN7570_IMOD, sample->imod);
		remember_error(&sample->result, sample->imod_result);
	}
	if (guard_level == TRACE_GUARD_FULL) {
		for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++) {
			sample->tx_guard_result[i] =
				en7570_read4(observer, en7570_tx_guards[i].reg,
					     sample->tx_guard[i]);
			remember_error(&sample->result,
				       sample->tx_guard_result[i]);
		}
	}

	sample->gpio_direction = gpiod_get_direction(observer->tx_disable);
	sample->gpio_value = gpiod_get_value_cansleep(observer->tx_disable);
	sample->gpio_raw_value =
		gpiod_get_raw_value_cansleep(observer->tx_disable);
	sample->gpio_active_low = gpiod_is_active_low(observer->tx_disable);
	if (sample->gpio_direction < 0)
		remember_error(&sample->result, sample->gpio_direction);
	if (sample->gpio_value < 0)
		remember_error(&sample->result, sample->gpio_value);
	if (sample->gpio_raw_value < 0)
		remember_error(&sample->result, sample->gpio_raw_value);

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
	sample->end_ns = ktime_get_ns();
}

static int trace_sample_verify(const struct los_trace_sample *sample)
{
	static const u8 los1[] = { 0x06, 0x1f, 0x1c, 0x10 };
	static const u8 svadc[] = { 0x00, 0x00, 0x41, 0x04 };
	int i;

	if (sample->guard_level < TRACE_GUARD_DENSE ||
	    sample->guard_level > TRACE_GUARD_FULL ||
	    sample->outcome_result != -ECANCELED)
		return -EPERM;
	if (sample->result)
		return sample->result;
	if (sample->los_ctrl2[0] != 0x05 ||
	    sample->los_ctrl2[1] != 0x1f ||
	    sample->los_ctrl2[3] != 0x00)
		return -EPERM;
	if (sample->guard_level == TRACE_GUARD_DENSE) {
		if (sample->timeout_count_result != -ECANCELED ||
		    sample->los_ctrl1_result != -ECANCELED ||
		    sample->svadc_result != -ECANCELED ||
		    sample->apd_result != -ECANCELED ||
		    sample->ovp_result != -ECANCELED ||
		    sample->safe_result != -ECANCELED ||
		    sample->ibias_result != -ECANCELED ||
		    sample->imod_result != -ECANCELED)
			return -EPERM;
	} else {
		if (sample->timeout_count_result || sample->los_ctrl1_result ||
		    sample->svadc_result || sample->apd_result ||
		    sample->ovp_result || sample->safe_result ||
		    sample->ibias_result || sample->imod_result)
			return -EPERM;
		if (memcmp(sample->los_ctrl1, los1, sizeof(los1)) ||
		    memcmp(sample->svadc, svadc, sizeof(svadc)) ||
		    memcmp(sample->apd, expected_apd, sizeof(sample->apd)) ||
		    memcmp(sample->ovp, all_zero, sizeof(sample->ovp)) ||
		    memcmp(sample->safe, expected_safe_protect,
			   sizeof(sample->safe)) ||
		    memcmp(sample->ibias, all_zero, sizeof(sample->ibias)) ||
		    memcmp(sample->imod, all_zero, sizeof(sample->imod)))
			return -EPERM;
	}
	for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++) {
		if (sample->guard_level == TRACE_GUARD_FULL) {
			if (sample->tx_guard_result[i] ||
			    memcmp(sample->tx_guard[i],
				   en7570_tx_guards[i].expected,
				   sizeof(sample->tx_guard[i])))
				return -EPERM;
		} else if (sample->tx_guard_result[i] != -ECANCELED) {
			return -EPERM;
		}
	}
	if (sample->gpio_active_low || sample->gpio_direction != 0 ||
	    sample->gpio_value != 1 || sample->gpio_raw_value != 1)
		return -EPERM;
	if (sample->physet3 & PHYSET3_TXEN ||
	    !(sample->physet10 & PHYSET10_GPON_MODE) ||
	    sample->xpon_setting != XPON_SETTING_RETAINED ||
	    sample->misc & MISC_ROGUE_TX_TEST || sample->prbs_tx ||
	    sample->test_frame || sample->xpon_int_en)
		return -EPERM;
	return 0;
}

static void classify_outcome_byte(struct xr500v_los_trace_observer *observer,
				  u8 byte2)
{
	if (!observer->first_byte2_valid) {
		observer->first_byte2 = byte2;
		observer->first_byte2_valid = true;
	} else if (observer->final_byte2 != byte2) {
		observer->transition_count++;
	}
	observer->final_byte2 = byte2;
	if (byte2 == 0x22) {
		observer->outcome_22_count++;
	} else if (byte2 == 0x23) {
		observer->outcome_23_count++;
	} else {
		observer->outcome_other_count++;
		observer->outcome_result = -ERANGE;
	}
}

static void classify_trace_outcome(struct xr500v_los_trace_observer *observer,
				   struct los_trace_sample *sample)
{
	sample->outcome_result =
		(sample->los_ctrl2[2] == 0x22 ||
		 sample->los_ctrl2[2] == 0x23) ? 0 : -ERANGE;
	classify_outcome_byte(observer, sample->los_ctrl2[2]);
}

static int classify_terminal_outcome(struct xr500v_los_trace_observer *observer)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(en7570_state_regs); i++) {
		if (en7570_state_regs[i].reg != EN7570_LOS_CTRL2)
			continue;
		if (observer->terminal.reg_result[i])
			return observer->terminal.reg_result[i];
		classify_outcome_byte(observer, observer->terminal.value[i][2]);
		return (observer->terminal.value[i][2] == 0x22 ||
			observer->terminal.value[i][2] == 0x23) ? 0 : -ERANGE;
	}
	return -ENOENT;
}

static int run_los_trace(struct xr500v_los_trace_observer *observer)
{
	unsigned int i;
	int ret;

	observer->outcome_result = 0;
	for (i = 0; i < TRACE_SAMPLE_COUNT; i++) {
		wait_until_trace_target(observer, trace_schedule[i].target_ms);
		trace_sample_capture(observer, &observer->samples[i],
				     trace_schedule[i].target_ms,
				     trace_schedule[i].guard_level);
		observer->samples_taken = i + 1;
		observer->samples[i].verify_result =
			trace_sample_verify(&observer->samples[i]);
		ret = observer->samples[i].verify_result;
		if (ret) {
			observer->halted_sample = i;
			return ret;
		}
		classify_trace_outcome(observer, &observer->samples[i]);
	}
	return 0;
}

static int verify_factory_hash(struct xr500v_los_trace_observer *observer)
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
		   "  xpon physet3=%08x physet10=%08x physta1=%08x fsm=%u setting=%08x misc=%08x rx=%08x sync=%x sync_ok=%u rx_hi=%02x bit15=%u sta=%08x los=%u prbs=%08x test=%08x irq=%08x\n",
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

static const char *trace_guard_level_name(enum trace_guard_level guard_level)
{
	switch (guard_level) {
	case TRACE_GUARD_DENSE:
		return "dense";
	case TRACE_GUARD_CRITICAL:
		return "critical";
	case TRACE_GUARD_FULL:
		return "full";
	default:
		return "invalid";
	}
}

static void trace_sample_show(struct seq_file *s,
			      struct xr500v_los_trace_observer *observer,
			      unsigned int index,
			      const struct los_trace_sample *sample)
{
	int i;
	u64 start_us = sample->start_ns >= observer->prefix_end_ns ?
		div_u64(sample->start_ns - observer->prefix_end_ns,
			NSEC_PER_USEC) : 0;
	u64 los2_done_us = sample->los_ctrl2_done_ns >= observer->prefix_end_ns ?
		div_u64(sample->los_ctrl2_done_ns - observer->prefix_end_ns,
			NSEC_PER_USEC) : 0;
	u64 end_us = sample->end_ns >= observer->prefix_end_ns ?
		div_u64(sample->end_ns - observer->prefix_end_ns,
			NSEC_PER_USEC) : 0;

	seq_printf(s,
		   "sample_%02u: level=%s capture=%d verify=%d outcome=%d target_ms=%u start_us=%llu los2_done_us=%llu end_us=%llu los2=%4ph(r=%d) timer=%4ph(r=%d) count=%4ph(r=%d) timeout=%4ph(r=%d) debug=%4ph(r=%d) los1=%4ph(r=%d) svadc=%4ph(r=%d)\n",
		   index, trace_guard_level_name(sample->guard_level),
		   sample->result, sample->verify_result,
		   sample->outcome_result,
		   sample->target_ms, (unsigned long long)start_us,
		   (unsigned long long)los2_done_us, (unsigned long long)end_us,
		   sample->los_ctrl2, sample->los_ctrl2_result,
		   sample->los_timer, sample->los_timer_result,
		   sample->timeout_count, sample->timeout_count_result,
		   sample->timeout, sample->timeout_result,
		   sample->los_debug, sample->los_debug_result,
		   sample->los_ctrl1, sample->los_ctrl1_result,
		   sample->svadc, sample->svadc_result);
	if (sample->guard_level >= TRACE_GUARD_CRITICAL)
		seq_printf(s,
			   "  safety ovp=%4ph(r=%d) apd=%4ph(r=%d) safe=%4ph(r=%d) ibias=%4ph(r=%d) imod=%4ph(r=%d)\n",
			   sample->ovp, sample->ovp_result,
			   sample->apd, sample->apd_result,
			   sample->safe, sample->safe_result,
			   sample->ibias, sample->ibias_result,
			   sample->imod, sample->imod_result);
	else
		seq_puts(s, "  safety EN7570=not-sampled (dense level)\n");
	if (sample->guard_level == TRACE_GUARD_FULL) {
		for (i = 0; i < ARRAY_SIZE(en7570_tx_guards); i++)
			seq_printf(s, "  guard_%s@%04x=%4ph(r=%d)\n",
				   en7570_tx_guards[i].name,
				   en7570_tx_guards[i].reg,
				   sample->tx_guard[i],
				   sample->tx_guard_result[i]);
	} else {
		seq_puts(s, "  static_tx_guards=not-sampled\n");
	}
	seq_printf(s,
		   "  gpio(active_low=%u direction=%d logical=%d raw=%d) physet3=%08x physet10=%08x physta1=%08x fsm=%u setting=%08x misc=%08x rx=%08x sync=%x sync_ok=%u rx_hi=%02x bit15=%u sta=%08x los=%u prbs=%08x test=%08x irq=%08x\n",
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

static int los_trace_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_los_trace_observer *observer = s->private;
	int i;

	mutex_lock(&observer->status_lock);
	seq_puts(s, "operation:             fixed OEM EN7570 reset/RSSI/gain/LOS prefix + terminal timestamped trace\n");
	seq_puts(s, "trace_policy:          dense@0/5/10/20 critical@50 full@100/250/500/1000/2000/5000/10000 ms\n");
	seq_printf(s, "silicon_id:            0x%02x\n", observer->silicon_id);
	seq_printf(s, "silicon_variant:       0x%02x\n", observer->silicon_variant);
	seq_printf(s, "factory_length:        0x%zx\n", observer->factory_length);
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
	seq_puts(s, "mmio_write_attempts:   0 / 0 maximum\n");
	seq_puts(s, "apd_write_attempts:    0 / 0 maximum\n");
	seq_printf(s, "sequence_result:       %d\n", observer->sequence_result);
	seq_printf(s, "halted_step:           %d\n", observer->halted_step);
	seq_printf(s, "halted_sample:         %d\n", observer->halted_sample);
	seq_printf(s, "trace_result:          %d\n", observer->trace_result);
	seq_printf(s, "outcome_result:        %d\n", observer->outcome_result);
	seq_printf(s, "terminal_outcome:      %d\n",
		   observer->terminal_outcome_result);
	seq_printf(s,
		   "outcome_observations:  22=%u 23=%u other=%u transitions=%u first=%02x final=%02x valid=%u (samples plus terminal)\n",
		   observer->outcome_22_count, observer->outcome_23_count,
		   observer->outcome_other_count, observer->transition_count,
		   observer->first_byte2, observer->final_byte2,
		   observer->first_byte2_valid);
	seq_printf(s, "prefix_end_ns:         %llu\n",
		   (unsigned long long)observer->prefix_end_ns);
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
	full_snapshot_show(s, "cold", &observer->cold,
			   observer->cold_verify_result);
	full_snapshot_show(s, "post_reset", &observer->post_reset,
			   observer->post_reset_verify_result);
	full_snapshot_show(s, "terminal", &observer->terminal,
			   observer->terminal_verify_result);
	for (i = 0; i < FIXED_WRITE_COUNT; i++)
		seq_printf(s,
			   "write_%02d: reg=%04x len=%u payload=%*ph attempted=%s result=%d start_ns=%llu done_ns=%llu\n",
			   i + 1, fixed_writes[i].reg, fixed_writes[i].length,
			   fixed_writes[i].length, fixed_writes[i].value,
			   observer->write_attempted[i] ? "yes" : "no",
			   observer->write_result[i],
			   (unsigned long long)observer->write_start_ns[i],
			   (unsigned long long)observer->write_done_ns[i]);
	seq_printf(s, "samples_taken:         %u / %u\n",
		   observer->samples_taken, TRACE_SAMPLE_COUNT);
	for (i = 0; i < observer->samples_taken; i++)
		trace_sample_show(s, observer, i, &observer->samples[i]);
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
	seq_puts(s, "periodic_worker:       no\n");
	seq_puts(s, "xpon_mmio_write:       no\n");
	seq_puts(s, "apd_write:             no\n");
	seq_puts(s, "tx_current_laser_tgen: no\n");
	seq_puts(s, "arbitrary_write_path:  no\n");
	mutex_unlock(&observer->status_lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(los_trace_status);

static void los_trace_results_init(struct xr500v_los_trace_observer *observer)
{
	int i;

	observer->sequence_result = -ECANCELED;
	observer->halted_step = 0;
	observer->halted_sample = -1;
	observer->cold.result = -ECANCELED;
	observer->post_reset.result = -ECANCELED;
	observer->terminal.result = -ECANCELED;
	observer->cold_verify_result = -ECANCELED;
	observer->post_reset_verify_result = -ECANCELED;
	observer->terminal_verify_result = -ECANCELED;
	observer->terminal_outcome_result = -ECANCELED;
	observer->trace_result = -ECANCELED;
	observer->outcome_result = -ECANCELED;
	for (i = 0; i < FIXED_WRITE_COUNT; i++)
		observer->write_result[i] = -ECANCELED;
}

static int los_trace_run(struct xr500v_los_trace_observer *observer)
{
	int ret;

	/* Globally consume the one sequence allowed for this module load. */
	if (atomic_cmpxchg(&los_trace_sequence_claimed, 0, 1))
		return -EBUSY;

	/* The first transfer attempt is the irreversible boundary. */
	__module_get(THIS_MODULE);
	observer->module_pinned = true;
	observer->physical_powercut_required = true;

	ret = run_reset_rssi_gain_los_prefix(observer);
	if (ret)
		goto halted;

	observer->trace_result = run_los_trace(observer);
	if (observer->trace_result) {
		ret = observer->trace_result;
		goto halted;
	}

	full_snapshot_capture(observer, &observer->terminal);
	observer->terminal_verify_result =
		full_snapshot_verify(observer, &observer->terminal,
				     SNAPSHOT_TERMINAL);
	if (observer->terminal_verify_result) {
		ret = observer->terminal_verify_result;
		observer->halted_step = FIXED_WRITE_COUNT;
		goto halted_no_capture;
	}
	observer->terminal_outcome_result = classify_terminal_outcome(observer);
	if (observer->terminal_outcome_result &&
	    observer->terminal_outcome_result != -ERANGE) {
		ret = observer->terminal_outcome_result;
		observer->halted_step = FIXED_WRITE_COUNT;
		goto halted_no_capture;
	}

	/* Unknown byte 2 is a recorded outcome, not an electrical failure. */
	observer->sequence_result = 0;
	observer->halted_step = 0;
	return 0;

halted:
	observer->halted_step = observer->i2c_write_attempts ?: 1;
	/* No recovery write is legal. Preserve a best-effort terminal map. */
	if (observer->terminal.result == -ECANCELED) {
		full_snapshot_capture(observer, &observer->terminal);
		if (observer->i2c_write_attempts == FIXED_WRITE_COUNT) {
			observer->terminal_verify_result =
				full_snapshot_verify(observer, &observer->terminal,
						     SNAPSHOT_TERMINAL);
			if (!observer->terminal_verify_result)
				observer->terminal_outcome_result =
					classify_terminal_outcome(observer);
		}
	}
halted_no_capture:
	observer->sequence_result = ret;
	/* Once claimed, every exit is pinned and probe must remain successful. */
	return 0;
}

static int xr500v_los_trace_observer_probe(struct platform_device *pdev)
{
	struct xr500v_los_trace_observer *observer;
	struct gpio_chip *gpio_chip;
	struct device_node *node;
	struct resource pon_i2c_resource;
	struct resource *xpon_resource;
	bool status_locked = false;
	int ret;

	if (!arm_en7570_los_trace)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "module LOS trace opt-in is absent\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-los-trace"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT LOS trace opt-in is absent\n");

	observer = devm_kzalloc(&pdev->dev, sizeof(*observer), GFP_KERNEL);
	if (!observer)
		return -ENOMEM;
	observer->dev = &pdev->dev;
	mutex_init(&observer->status_lock);
	los_trace_results_init(observer);

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
	ret = en7570_read(observer, EN7570_VARIANT,
			  &observer->silicon_variant,
			  sizeof(observer->silicon_variant));
	if (ret || observer->silicon_variant != EN7570_EXPECTED_VARIANT) {
		ret = ret ?: -ENODEV;
		dev_err(&pdev->dev,
			"EN7570 variant mismatch: 0x%02x, expected 0x%02x (%d)\n",
			observer->silicon_variant, EN7570_EXPECTED_VARIANT, ret);
		goto out_unlock;
	}

	/* The locked 29-register map is the final cold-state authority. */
	full_snapshot_capture(observer, &observer->cold);
	observer->cold_verify_result =
		full_snapshot_verify(observer, &observer->cold, SNAPSHOT_COLD);
	if (observer->cold_verify_result) {
		ret = observer->cold_verify_result;
		dev_err(&pdev->dev, "exact terminal LOS trace cold preflight failed: %d\n",
			ret);
		goto out_unlock;
	}

	/*
	 * Establish the read-only evidence channel before the irreversible first
	 * write. Readers block on status_lock until the complete result is stable.
	 */
	mutex_lock(&observer->status_lock);
	status_locked = true;
	observer->debugfs_dir =
		debugfs_create_dir("xr500v-en7570-los-trace-observer", NULL);
	if (IS_ERR_OR_NULL(observer->debugfs_dir)) {
		ret = IS_ERR(observer->debugfs_dir) ?
			PTR_ERR(observer->debugfs_dir) : -ENODEV;
		observer->debugfs_dir = NULL;
		dev_err(&pdev->dev,
			"debugfs directory unavailable before LOS trace: %d\n",
			ret);
		goto out_unlock;
	}
	observer->debugfs_status =
		debugfs_create_file("status", 0444, observer->debugfs_dir,
				    observer, &los_trace_status_fops);
	if (IS_ERR_OR_NULL(observer->debugfs_status)) {
		ret = IS_ERR(observer->debugfs_status) ?
			PTR_ERR(observer->debugfs_status) : -ENODEV;
		observer->debugfs_status = NULL;
		dev_err(&pdev->dev,
			"debugfs status unavailable before LOS trace: %d\n", ret);
		goto out_unlock;
	}

	ret = los_trace_run(observer);
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
	if (status_locked)
		mutex_unlock(&observer->status_lock);
	if (ret) {
		debugfs_remove_recursive(observer->debugfs_dir);
		observer->debugfs_dir = NULL;
		return ret;
	}

	platform_set_drvdata(pdev, observer);
	dev_warn(&pdev->dev,
		 "EN7570 terminal LOS trace result %d after %u/%u I2C write attempt(s), 0 MMIO/APD write(s), %u/%u sample(s); TX_DISABLE retained; physical power removal required\n",
		 observer->sequence_result, observer->i2c_write_attempts,
		 FIXED_WRITE_COUNT, observer->samples_taken,
		 TRACE_SAMPLE_COUNT);
	return 0;
}

static void xr500v_los_trace_observer_remove(struct platform_device *pdev)
{
	struct xr500v_los_trace_observer *observer = platform_get_drvdata(pdev);

	debugfs_remove_recursive(observer->debugfs_dir);
	dev_emerg(&pdev->dev,
		  "terminal LOS trace observer removal has no rollback; remove physical power now\n");
}

static const struct of_device_id xr500v_los_trace_observer_of_match[] = {
	{ .compatible = "econet,en751221-en7570-los-trace-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_los_trace_observer_of_match);

static struct platform_driver xr500v_los_trace_observer_driver = {
	.probe = xr500v_los_trace_observer_probe,
	.remove = xr500v_los_trace_observer_remove,
	.driver = {
		.name = "xr500v-en7570-los-trace-observer",
		.of_match_table = xr500v_los_trace_observer_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(xr500v_los_trace_observer_driver);

MODULE_DESCRIPTION("Guarded XR500v EN7570 fixed terminal LOS trace observer");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
