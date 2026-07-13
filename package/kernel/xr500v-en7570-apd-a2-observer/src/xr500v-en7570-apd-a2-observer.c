// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot EN7570 APD A2 observer for the TP-Link Archer XR500v.
 *
 * This deliberately non-shipping driver has no matching normal DT node and
 * no autoload entry.  It accepts only this unit's immutable factory-block
 * hash and exact cold hardware state, holds physical TX_DISABLE high, and
 * issues at most the three fixed-width writes used by the OEM APD start:
 *
 *   0x0030 <- 00 08 20 00 (four bytes)
 *   0x0030 <- 00 09 20 00 (four bytes)
 *   0x0030 <- a2          (one byte)
 *
 * APD, OVP, SAFE_PROTECT, bias/modulation and every xPON TX gate are captured
 * and checked immediately before and after each attempted transfer.  There
 * is no retry, worker, arbitrary payload, software rollback or disable path.
 * The module self-pins before its first write attempt and requires physical
 * power removal as the recovery boundary.
 */

#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/atomic.h>
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
#define MISC			0x01fc
#define MISC_ROGUE_TX_TEST	BIT(28)
#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_INT_EN		0x05f0

#define EN7570_APD_DAC		0x0030
#define EN7570_SAFE_PROTECT	0x0100
#define EN7570_IBIAS		0x0138
#define EN7570_IMOD		0x0148
#define EN7570_APD_OVP_LATCH	0x0164
#define EN7570_ID		0x0170
#define EN7570_EXPECTED_ID	0x03

#define EN7570_I2C_ADDRESS	0x70
#define XR500V_PON_I2C_BASE	0x1fbf8000
#define XR500V_PON_I2C_SIZE	0x100
#define XR500V_XPON_BASE	0x1faf0000
#define XR500V_XPON_SIZE	0x1000

#define XR500V_FACTORY_LENGTH	0x190
#define XR500V_TX_DISABLE_GPIO	16
#define APD_STEP_COUNT		3

static bool arm_en7570_apd_a2;
module_param(arm_en7570_apd_a2, bool, 0444);
MODULE_PARM_DESC(arm_en7570_apd_a2,
		 "Arm the one-shot, non-transactional XR500v EN7570 APD A2 observer");

static atomic_t apd_sequence_claimed = ATOMIC_INIT(0);

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

struct apd_write {
	u8 length;
	u8 value[4];
};

static const struct apd_write apd_writes[APD_STEP_COUNT] = {
	{ .length = 4, .value = { 0x00, 0x08, 0x20, 0x00 } },
	{ .length = 4, .value = { 0x00, 0x09, 0x20, 0x00 } },
	{ .length = 1, .value = { 0xa2, 0x00, 0x00, 0x00 } },
};

static const u8 expected_safe_protect[4] = { 0xff, 0x8f, 0xff, 0x0f };
static const u8 all_zero[4];

struct en7570_guard_reg {
	const char *name;
	u16 reg;
	u8 expected[4];
};

/* Exact cold values for the remaining TX analogue and reset controls. */
static const struct en7570_guard_reg en7570_tx_guards[] = {
	{ "tiamux", 0x0000, { 0x08, 0x00, 0x10, 0x02 } },
	{ "mpd_targets", 0x0004, { 0x00, 0x02, 0x00, 0x00 } },
	{ "t1delay", 0x0008, { 0x99, 0x00, 0x00, 0x20 } },
	{ "tx_sd", 0x000c, { 0x40, 0x00, 0x00, 0x00 } },
	{ "la_pwd", 0x0014, { 0x00, 0x24, 0x00, 0x00 } },
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
	u32 misc;
	u32 prbs_tx;
	u32 test_frame;
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

struct xr500v_apd_observer {
	struct device *dev;
	void __iomem *xpon_base;
	struct gpio_desc *tx_disable;
	struct i2c_client *en7570;
	struct dentry *debugfs_dir;
	struct apd_step_result steps[APD_STEP_COUNT];
	u8 silicon_id;
	u8 factory_digest[SHA256_DIGEST_SIZE];
	size_t factory_length;
	int tx_disable_gpio;
	int tx_disable_offset;
	int adapter_retries_saved;
	unsigned int i2c_write_attempts;
	int sequence_result;
	int halted_step;
	bool factory_hash_matched;
	bool adapter_retries_restored;
	bool i2c_bus_locked;
	bool module_pinned;
	bool physical_powercut_required;
};

static u32 xpon_read(struct xr500v_apd_observer *observer, u32 reg)
{
	return ioread32(observer->xpon_base + reg);
}

static int en7570_read(struct xr500v_apd_observer *observer, u16 reg,
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

static int en7570_read4(struct xr500v_apd_observer *observer, u16 reg,
			u8 value[4])
{
	return en7570_read(observer, reg, value, 4);
}

static void remember_error(int *result, int candidate)
{
	if (!*result && candidate)
		*result = candidate;
}

static void capture_tx_guards(struct xr500v_apd_observer *observer,
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

static void capture_critical(struct xr500v_apd_observer *observer,
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
	snapshot->misc = xpon_read(observer, MISC);
	snapshot->prbs_tx = xpon_read(observer, BISTCTL_PRBS_TX_EN);
	snapshot->test_frame = xpon_read(observer, TEST_FRAME_EN);
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
static void apd_snapshot_capture(struct xr500v_apd_observer *observer,
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

static int apd_snapshot_verify(struct xr500v_apd_observer *observer,
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

/* This is the only register-data write helper in the driver. */
static int en7570_apd_write_once(struct xr500v_apd_observer *observer,
				 unsigned int step)
{
	const struct apd_write *operation;
	u8 data[6] = { EN7570_APD_DAC >> 8, EN7570_APD_DAC & 0xff };
	struct i2c_msg message = {
		.addr = observer->en7570->addr,
		.buf = data,
	};
	int ret;

	if (!observer->i2c_bus_locked || !observer->module_pinned ||
	    step >= APD_STEP_COUNT ||
	    observer->i2c_write_attempts != step)
		return -EPERM;
	operation = &apd_writes[step];
	memcpy(data + 2, operation->value, operation->length);
	message.len = 2 + operation->length;

	observer->i2c_write_attempts++;
	ret = __i2c_transfer(observer->en7570->adapter, &message, 1);
	if (ret < 0)
		return ret;
	return ret == 1 ? 0 : -EIO;
}

static int verify_factory_hash(struct xr500v_apd_observer *observer)
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
		   "  physet3=%08x txen=%u physet10=%08x gpon=%u misc=%08x rogue=%u prbs=%08x test=%08x irq=%08x\n",
		   snapshot->physet3,
		   !!(snapshot->physet3 & PHYSET3_TXEN), snapshot->physet10,
		   !!(snapshot->physet10 & PHYSET10_GPON_MODE), snapshot->misc,
		   !!(snapshot->misc & MISC_ROGUE_TX_TEST), snapshot->prbs_tx,
		   snapshot->test_frame, snapshot->xpon_int_en);
}

static int apd_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_apd_observer *observer = s->private;
	int i;

	seq_puts(s, "operation:             fixed OEM EN7570 APD A2 sequence\n");
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
		   observer->i2c_write_attempts, APD_STEP_COUNT);
	seq_printf(s, "sequence_result:       %d\n", observer->sequence_result);
	seq_printf(s, "halted_step:           %d\n", observer->halted_step);
	for (i = 0; i < APD_STEP_COUNT; i++) {
		const struct apd_step_result *step = &observer->steps[i];

		seq_printf(s, "step_%d_payload:        %*ph\n", i + 1,
			   apd_writes[i].length, apd_writes[i].value);
		seq_printf(s, "step_%d_write:          attempted=%s result=%d\n",
			   i + 1, step->write_attempted ? "yes" : "no",
			   step->write_result);
		apd_snapshot_show(s, "  pre", &step->pre,
				  step->pre_verify_result);
		apd_snapshot_show(s, "  post", &step->post,
				  step->post_verify_result);
	}
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
	seq_puts(s, "worker_or_periodic_apd: no\n");
	seq_puts(s, "arbitrary_write_path:  no\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(apd_status);

static void apd_results_init(struct xr500v_apd_observer *observer)
{
	int i;

	observer->sequence_result = -ECANCELED;
	observer->halted_step = 0;
	for (i = 0; i < APD_STEP_COUNT; i++) {
		observer->steps[i].pre.result = -ECANCELED;
		observer->steps[i].post.result = -ECANCELED;
		observer->steps[i].pre_verify_result = -ECANCELED;
		observer->steps[i].write_result = -ECANCELED;
		observer->steps[i].post_verify_result = -ECANCELED;
	}
}

static int apd_run_pinned_sequence(struct xr500v_apd_observer *observer)
{
	int i;

	/* Globally consume the one sequence allowed for this module load. */
	if (atomic_cmpxchg(&apd_sequence_claimed, 0, 1))
		return -EBUSY;

	/* The first transfer attempt is the irreversible boundary. */
	__module_get(THIS_MODULE);
	observer->module_pinned = true;
	observer->physical_powercut_required = true;

	for (i = 0; i < APD_STEP_COUNT; i++) {
		struct apd_step_result *step = &observer->steps[i];

		if (i) {
			apd_snapshot_capture(observer, &step->pre, false);
			step->pre_verify_result =
				apd_snapshot_verify(observer, &step->pre,
						    apd_states[i]);
		}
		if (step->pre_verify_result) {
			observer->sequence_result = step->pre_verify_result;
			observer->halted_step = i + 1;
			break;
		}

		step->write_attempted = true;
		step->write_result = en7570_apd_write_once(observer, i);

		/* Always capture a post-attempt snapshot, including on bus error. */
		apd_snapshot_capture(observer, &step->post, true);
		step->post_verify_result =
			apd_snapshot_verify(observer, &step->post,
					    apd_states[i + 1]);
		if (step->write_result || step->post_verify_result) {
			observer->sequence_result = step->write_result ?:
				step->post_verify_result;
			observer->halted_step = i + 1;
			break;
		}
	}

	if (i == APD_STEP_COUNT) {
		observer->sequence_result = 0;
		observer->halted_step = 0;
	}

	/* Once claimed, every exit is pinned and probe must remain successful. */
	return 0;
}

static int xr500v_apd_observer_probe(struct platform_device *pdev)
{
	struct xr500v_apd_observer *observer;
	struct gpio_chip *gpio_chip;
	struct device_node *node;
	struct resource pon_i2c_resource;
	struct resource *xpon_resource;
	int ret;

	if (!arm_en7570_apd_a2)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "module APD A2 opt-in is absent\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-apd-a2"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT APD A2 opt-in is absent\n");

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

	/* Step one pre-snapshot is the locked exact APD/TX cold preflight. */
	apd_snapshot_capture(observer, &observer->steps[0].pre, false);
	observer->steps[0].pre_verify_result =
		apd_snapshot_verify(observer, &observer->steps[0].pre,
				    apd_states[0]);
	if (observer->steps[0].pre_verify_result) {
		ret = observer->steps[0].pre_verify_result;
		dev_err(&pdev->dev, "exact APD/TX cold preflight failed: %d\n",
			ret);
		goto out_unlock;
	}

	ret = apd_run_pinned_sequence(observer);
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
		debugfs_create_dir("xr500v-en7570-apd-a2-observer", NULL);
	if (IS_ERR(observer->debugfs_dir)) {
		dev_err(&pdev->dev, "debugfs unavailable after APD attempt: %ld\n",
			PTR_ERR(observer->debugfs_dir));
		observer->debugfs_dir = NULL;
	} else {
		debugfs_create_file("status", 0444, observer->debugfs_dir,
				    observer, &apd_status_fops);
	}

	platform_set_drvdata(pdev, observer);
	dev_warn(&pdev->dev,
		 "EN7570 APD A2 sequence result %d after %u write attempt(s); TX_DISABLE retained; physical power removal required\n",
		 observer->sequence_result, observer->i2c_write_attempts);
	return 0;
}

static void xr500v_apd_observer_remove(struct platform_device *pdev)
{
	struct xr500v_apd_observer *observer = platform_get_drvdata(pdev);

	debugfs_remove_recursive(observer->debugfs_dir);
	dev_emerg(&pdev->dev,
		  "APD observer removal has no rollback; remove physical power now\n");
}

static const struct of_device_id xr500v_apd_observer_of_match[] = {
	{ .compatible = "econet,en751221-en7570-apd-a2-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_apd_observer_of_match);

static struct platform_driver xr500v_apd_observer_driver = {
	.probe = xr500v_apd_observer_probe,
	.remove = xr500v_apd_observer_remove,
	.driver = {
		.name = "xr500v-en7570-apd-a2-observer",
		.of_match_table = xr500v_apd_observer_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(xr500v_apd_observer_driver);

MODULE_DESCRIPTION("Guarded XR500v EN7570 fixed APD A2 observer");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
