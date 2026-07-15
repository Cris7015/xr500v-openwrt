// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guarded, finite digital receiver handoff probe for the TP-Link XR500v.
 *
 * This lab-only driver accepts one exact cold state.  With the physical and
 * digital transmitter barriers proved closed, it temporarily clears only
 * PHYSET3.ESD_PRO and asserts only PHYSET2.FWRDY.  Six read-only samples are
 * taken over five seconds, after which FWRDY and ESD_PRO are restored in that
 * order and every writable guard is verified.  PHYSET2.PHYRDY is a hardware
 * status bit and is observed separately from the rollback proof.  There is no
 * EN7570, APD, laser, reset, PLL, counter, interrupt, GPON MAC or SCU access
 * path.
 */

#include <crypto/hash.h>
#include <crypto/sha2.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#define PHYSET2			0x0104
#define PHYSET2_FW_READY	BIT(0)
#define PHYSET2_PHY_READY	BIT(2)
#define PHYSET3			0x0108
#define PHYSET3_ESD_PRO		BIT(2)
#define PHYSET3_TXEN		BIT(5)
#define PHYSET10		0x0124
#define PHYSTA1			0x0130
#define XPON_SETTING		0x0138
#define ANASTA1			0x013c
#define MISC			0x01fc
#define MISC_ROGUE_TX_TEST	BIT(28)
#define PHYRX_STATUS		0x021c
#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_STA			0x05e0
#define XPON_INT_EN		0x05f0
#define XPON_INT_STA		0x05f8

#define XR500V_XPON_BASE	0x1faf0000
#define XR500V_XPON_SIZE	0x1000
#define XR500V_TX_DISABLE_GPIO	16
#define XR500V_FACTORY_LENGTH	0x190

#define COLD_PHYSET2		0x00003c00
#define ACTIVE_PHYSET2		(COLD_PHYSET2 | PHYSET2_FW_READY)
#define COLD_PHYSET3		0x4581e114
#define ACTIVE_PHYSET3		(COLD_PHYSET3 & ~PHYSET3_ESD_PRO)
#define COLD_PHYSET10		0xff000000
#define COLD_XPON_SETTING	0x0000014f

#define HANDOFF_SAMPLE_COUNT	6
#define PHY_READY_SETTLE_MS	500

static bool arm_digital_handoff;
module_param(arm_digital_handoff, bool, 0444);
MODULE_PARM_DESC(arm_digital_handoff,
		 "Arm the finite XR500v ESD_PRO/FWRDY digital receiver probe");

static const u8 xr500v_factory_sha256[SHA256_DIGEST_SIZE] = {
	0x40, 0x1d, 0xfd, 0xae, 0xe7, 0x7c, 0x84, 0x64,
	0x9b, 0xda, 0x10, 0x0f, 0xd5, 0xdd, 0x85, 0xbe,
	0x01, 0xc7, 0xea, 0x12, 0x6d, 0x0a, 0x5c, 0xc2,
	0x11, 0x6b, 0x14, 0x1c, 0x1a, 0x07, 0xa5, 0xe4,
};

static const unsigned int handoff_sample_target_ms[HANDOFF_SAMPLE_COUNT] = {
	0, 10, 100, 500, 2000, 5000,
};

struct handoff_sample {
	unsigned int target_ms;
	u64 actual_us;
	int verify_result;
	int gpio_direction;
	int gpio_value;
	int gpio_raw_value;
	u32 physet2;
	u32 physet3;
	u32 physet10;
	u32 physta1;
	u32 xpon_setting;
	u32 anasta1;
	u32 misc;
	u32 phyrx_status;
	u32 prbs_tx;
	u32 test_frame;
	u32 xpon_sta;
	u32 xpon_int_en;
	u32 xpon_int_sta;
};

struct xr500v_digital_handoff {
	struct device *dev;
	void __iomem *base;
	struct gpio_desc *tx_disable;
	struct mutex status_lock; /* protects the published finite report */
	struct dentry *debugfs_dir;
	struct dentry *debugfs_status;
	struct handoff_sample samples[HANDOFF_SAMPLE_COUNT];
	u8 factory_digest[SHA256_DIGEST_SIZE];
	size_t factory_length;
	u32 before_physet2;
	u32 before_physet3;
	u32 rollback_immediate_physet2;
	u32 final_physet2;
	u32 final_physet3;
	u32 final_physet10;
	u32 final_physta1;
	u32 final_xpon_setting;
	u32 final_anasta1;
	u32 final_misc;
	u32 final_phyrx_status;
	u32 final_prbs_tx;
	u32 final_test_frame;
	u32 final_xpon_sta;
	u32 final_xpon_int_en;
	u32 final_xpon_int_sta;
	u64 handoff_start_ns;
	unsigned int samples_taken;
	unsigned int mmio_writes;
	int sequence_result;
	int rollback_write_result;
	int rollback_result;
	int phy_ready_settle_result;
	int final_gpio_direction;
	int final_gpio_value;
	int final_gpio_raw_value;
	bool factory_hash_matched;
	bool esd_rollback_needed;
	bool fw_ready_rollback_needed;
	bool rollback_complete;
	bool physical_powercut_required;
};

static u32 xpon_read(struct xr500v_digital_handoff *probe, u32 reg)
{
	return ioread32(probe->base + reg);
}

static int tx_disable_guard(struct xr500v_digital_handoff *probe)
{
	int direction;
	int logical;
	int raw;

	if (gpiod_is_active_low(probe->tx_disable))
		return -EPERM;
	direction = gpiod_get_direction(probe->tx_disable);
	logical = gpiod_get_value_cansleep(probe->tx_disable);
	raw = gpiod_get_raw_value_cansleep(probe->tx_disable);
	if (direction < 0)
		return direction;
	if (logical < 0)
		return logical;
	if (raw < 0)
		return raw;

	return direction == 0 && logical == 1 && raw == 1 ? 0 : -EPERM;
}

static int static_guard(struct xr500v_digital_handoff *probe,
			u32 expected_physet2, u32 expected_physet3,
			bool allow_phy_ready)
{
	u32 physet2;
	int ret;

	ret = tx_disable_guard(probe);
	if (ret)
		return ret;
	physet2 = xpon_read(probe, PHYSET2);
	if ((!allow_phy_ready && physet2 != expected_physet2) ||
	    (allow_phy_ready &&
	     (physet2 & ~PHYSET2_PHY_READY) != expected_physet2) ||
	    xpon_read(probe, PHYSET3) != expected_physet3 ||
	    xpon_read(probe, PHYSET10) != COLD_PHYSET10 ||
	    xpon_read(probe, XPON_SETTING) != COLD_XPON_SETTING)
		return -EPERM;
	if (expected_physet3 & PHYSET3_TXEN)
		return -EPERM;
	if (xpon_read(probe, MISC) & MISC_ROGUE_TX_TEST ||
	    xpon_read(probe, BISTCTL_PRBS_TX_EN) ||
	    xpon_read(probe, TEST_FRAME_EN) ||
	    xpon_read(probe, XPON_INT_EN))
		return -EPERM;

	return 0;
}

/* Sole PHYSET3 write primitive: callers can change only ESD_PRO. */
static noinline int update_esd_pro_bit(struct xr500v_digital_handoff *probe,
				       bool set)
{
	u32 old;
	u32 new;

	old = xpon_read(probe, PHYSET3);
	new = set ? old | PHYSET3_ESD_PRO : old & ~PHYSET3_ESD_PRO;
	if (old == new)
		return set ? 0 : -EALREADY;
	if ((old ^ new) & ~PHYSET3_ESD_PRO)
		return -EPERM;
	iowrite32(new, probe->base + PHYSET3);
	probe->mmio_writes++;

	return xpon_read(probe, PHYSET3) == new ? 0 : -EIO;
}

/* Sole PHYSET2 write primitive: callers can change only FW_READY. */
static noinline int update_fw_ready_bit(struct xr500v_digital_handoff *probe,
					bool set)
{
	u32 old;
	u32 new;
	u32 readback;

	old = xpon_read(probe, PHYSET2);
	new = set ? old | PHYSET2_FW_READY : old & ~PHYSET2_FW_READY;
	if (old == new)
		return set ? -EALREADY : 0;
	if ((old ^ new) & ~PHYSET2_FW_READY)
		return -EPERM;
	if (set)
		probe->handoff_start_ns = ktime_get_ns();
	iowrite32(new, probe->base + PHYSET2);
	probe->mmio_writes++;
	readback = xpon_read(probe, PHYSET2);
	if (!!(readback & PHYSET2_FW_READY) != set)
		return -EIO;
	if ((readback ^ new) & ~PHYSET2_PHY_READY)
		return -EIO;

	return 0;
}

/* Advancement remains fail-closed behind every physical and digital guard. */
static int apply_esd_disable(struct xr500v_digital_handoff *probe)
{
	int ret;

	ret = static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false);
	if (ret)
		return ret;
	probe->esd_rollback_needed = true;

	return update_esd_pro_bit(probe, false);
}

static int apply_fw_ready(struct xr500v_digital_handoff *probe)
{
	int ret;

	ret = static_guard(probe, COLD_PHYSET2, ACTIVE_PHYSET3, false);
	if (ret)
		return ret;
	probe->fw_ready_rollback_needed = true;
	ret = update_fw_ready_bit(probe, true);
	if (ret)
		return ret;

	return 0;
}

/*
 * Rollback is deliberately strictly decreasing and cannot be refused because
 * a guard that triggered the abort changed: clear FWRDY, then restore ESD.
 * The raw helpers preserve every bit other than their single named control.
 */
static int rollback_fw_ready_disable(struct xr500v_digital_handoff *probe)
{
	return update_fw_ready_bit(probe, false);
}

static int rollback_esd_restore(struct xr500v_digital_handoff *probe)
{
	return update_esd_pro_bit(probe, true);
}

static int verify_sample(const struct handoff_sample *sample)
{
	if (sample->gpio_direction < 0)
		return sample->gpio_direction;
	if (sample->gpio_value < 0)
		return sample->gpio_value;
	if (sample->gpio_raw_value < 0)
		return sample->gpio_raw_value;
	if (sample->gpio_direction != 0 || sample->gpio_value != 1 ||
	    sample->gpio_raw_value != 1)
		return -EPERM;
	if ((sample->physet2 & ~PHYSET2_PHY_READY) != ACTIVE_PHYSET2 ||
	    sample->physet3 != ACTIVE_PHYSET3 ||
	    sample->physet10 != COLD_PHYSET10 ||
	    sample->xpon_setting != COLD_XPON_SETTING)
		return -EPERM;
	if (sample->physet3 & PHYSET3_TXEN ||
	    sample->misc & MISC_ROGUE_TX_TEST || sample->prbs_tx ||
	    sample->test_frame || sample->xpon_int_en)
		return -EPERM;

	return 0;
}

static void capture_sample(struct xr500v_digital_handoff *probe,
			   struct handoff_sample *sample,
			   unsigned int target_ms)
{
	sample->target_ms = target_ms;
	sample->actual_us = div_u64(ktime_get_ns() - probe->handoff_start_ns,
				    NSEC_PER_USEC);
	sample->gpio_direction = gpiod_get_direction(probe->tx_disable);
	sample->gpio_value = gpiod_get_value_cansleep(probe->tx_disable);
	sample->gpio_raw_value =
		gpiod_get_raw_value_cansleep(probe->tx_disable);
	sample->physet2 = xpon_read(probe, PHYSET2);
	sample->physet3 = xpon_read(probe, PHYSET3);
	sample->physet10 = xpon_read(probe, PHYSET10);
	sample->physta1 = xpon_read(probe, PHYSTA1);
	sample->xpon_setting = xpon_read(probe, XPON_SETTING);
	sample->anasta1 = xpon_read(probe, ANASTA1);
	sample->misc = xpon_read(probe, MISC);
	sample->phyrx_status = xpon_read(probe, PHYRX_STATUS);
	sample->prbs_tx = xpon_read(probe, BISTCTL_PRBS_TX_EN);
	sample->test_frame = xpon_read(probe, TEST_FRAME_EN);
	sample->xpon_sta = xpon_read(probe, XPON_STA);
	sample->xpon_int_en = xpon_read(probe, XPON_INT_EN);
	sample->xpon_int_sta = xpon_read(probe, XPON_INT_STA);
	sample->verify_result = verify_sample(sample);
}

static void wait_until_target(struct xr500v_digital_handoff *probe,
			      unsigned int target_ms)
{
	u64 deadline = probe->handoff_start_ns +
		       (u64)target_ms * NSEC_PER_MSEC;
	u64 now;
	u64 remaining_us;

	for (;;) {
		now = ktime_get_ns();
		if (now >= deadline)
			return;
		remaining_us = div_u64(deadline - now, NSEC_PER_USEC);
		if (remaining_us > 20000)
			msleep(min_t(u64, div_u64(remaining_us, 1000), 100));
		else
			usleep_range(max_t(u64, remaining_us, 1),
				     max_t(u64, remaining_us, 1) + 200);
	}
}

static void capture_final_state(struct xr500v_digital_handoff *probe)
{
	probe->final_gpio_direction = gpiod_get_direction(probe->tx_disable);
	probe->final_gpio_value =
		gpiod_get_value_cansleep(probe->tx_disable);
	probe->final_gpio_raw_value =
		gpiod_get_raw_value_cansleep(probe->tx_disable);
	probe->final_physet2 = xpon_read(probe, PHYSET2);
	probe->final_physet3 = xpon_read(probe, PHYSET3);
	probe->final_physet10 = xpon_read(probe, PHYSET10);
	probe->final_physta1 = xpon_read(probe, PHYSTA1);
	probe->final_xpon_setting = xpon_read(probe, XPON_SETTING);
	probe->final_anasta1 = xpon_read(probe, ANASTA1);
	probe->final_misc = xpon_read(probe, MISC);
	probe->final_phyrx_status = xpon_read(probe, PHYRX_STATUS);
	probe->final_prbs_tx = xpon_read(probe, BISTCTL_PRBS_TX_EN);
	probe->final_test_frame = xpon_read(probe, TEST_FRAME_EN);
	probe->final_xpon_sta = xpon_read(probe, XPON_STA);
	probe->final_xpon_int_en = xpon_read(probe, XPON_INT_EN);
	probe->final_xpon_int_sta = xpon_read(probe, XPON_INT_STA);
}

static int verify_final_state(const struct xr500v_digital_handoff *probe)
{
	if (probe->final_gpio_direction < 0)
		return probe->final_gpio_direction;
	if (probe->final_gpio_value < 0)
		return probe->final_gpio_value;
	if (probe->final_gpio_raw_value < 0)
		return probe->final_gpio_raw_value;
	if (probe->final_gpio_direction != 0 || probe->final_gpio_value != 1 ||
	    probe->final_gpio_raw_value != 1)
		return -EPERM;
	if ((probe->final_physet2 & ~PHYSET2_PHY_READY) !=
	    COLD_PHYSET2 ||
	    probe->final_physet3 != COLD_PHYSET3 ||
	    probe->final_physet10 != COLD_PHYSET10 ||
	    probe->final_xpon_setting != COLD_XPON_SETTING)
		return -EIO;
	if (probe->final_physet3 & PHYSET3_TXEN ||
	    probe->final_misc & MISC_ROGUE_TX_TEST ||
	    probe->final_prbs_tx || probe->final_test_frame ||
	    probe->final_xpon_int_en)
		return -EPERM;

	return 0;
}

static int rollback_handoff(struct xr500v_digital_handoff *probe)
{
	int write_ret = 0;
	int err;

	if (probe->fw_ready_rollback_needed) {
		err = rollback_fw_ready_disable(probe);
		if (err && !write_ret)
			write_ret = err;
	}
	if (probe->esd_rollback_needed) {
		err = rollback_esd_restore(probe);
		if (err && !write_ret)
			write_ret = err;
	}

	probe->rollback_write_result = write_ret;
	probe->rollback_immediate_physet2 = xpon_read(probe, PHYSET2);
	if (!(probe->rollback_immediate_physet2 & PHYSET2_FW_READY) &&
	    probe->rollback_immediate_physet2 & PHYSET2_PHY_READY)
		msleep(PHY_READY_SETTLE_MS);
	capture_final_state(probe);
	if (probe->final_physet2 & PHYSET2_FW_READY)
		probe->phy_ready_settle_result = -ECANCELED;
	else if (probe->final_physet2 & PHYSET2_PHY_READY)
		probe->phy_ready_settle_result = -ETIMEDOUT;
	else
		probe->phy_ready_settle_result = 0;
	probe->rollback_result = verify_final_state(probe);
	probe->rollback_complete = !probe->rollback_result;
	probe->physical_powercut_required = probe->rollback_result != 0;

	return probe->rollback_result;
}

static void run_handoff(struct xr500v_digital_handoff *probe)
{
	unsigned int i;
	int ret;

	probe->before_physet2 = xpon_read(probe, PHYSET2);
	probe->before_physet3 = xpon_read(probe, PHYSET3);
	ret = static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false);
	if (ret)
		goto out;

	ret = apply_esd_disable(probe);
	if (ret)
		goto rollback;
	ret = static_guard(probe, COLD_PHYSET2, ACTIVE_PHYSET3, false);
	if (ret)
		goto rollback;

	ret = apply_fw_ready(probe);
	if (ret)
		goto rollback;
	ret = static_guard(probe, ACTIVE_PHYSET2, ACTIVE_PHYSET3, true);
	if (ret)
		goto rollback;

	for (i = 0; i < HANDOFF_SAMPLE_COUNT; i++) {
		wait_until_target(probe, handoff_sample_target_ms[i]);
		capture_sample(probe, &probe->samples[i],
			       handoff_sample_target_ms[i]);
		probe->samples_taken = i + 1;
		if (probe->samples[i].verify_result) {
			ret = probe->samples[i].verify_result;
			goto rollback;
		}
	}

rollback:
	probe->sequence_result = ret;
	probe->rollback_result = rollback_handoff(probe);
	return;
out:
	probe->sequence_result = ret;
	probe->rollback_result = -ECANCELED;
}

static int verify_factory_hash(struct xr500v_digital_handoff *probe)
{
	struct crypto_shash *sha256;
	struct nvmem_cell *cell;
	void *factory;
	int ret;

	cell = devm_nvmem_cell_get(probe->dev, "factory-bob");
	if (IS_ERR(cell))
		return dev_err_probe(probe->dev, PTR_ERR(cell),
				     "factory-bob NVMEM cell unavailable\n");
	factory = nvmem_cell_read(cell, &probe->factory_length);
	if (IS_ERR(factory))
		return dev_err_probe(probe->dev, PTR_ERR(factory),
				     "cannot read factory-bob NVMEM cell\n");
	if (probe->factory_length != XR500V_FACTORY_LENGTH) {
		ret = -EMSGSIZE;
		goto out_free;
	}

	sha256 = crypto_alloc_shash("sha256", 0, 0);
	if (IS_ERR(sha256)) {
		ret = PTR_ERR(sha256);
		goto out_free;
	}
	ret = crypto_shash_tfm_digest(sha256, factory,
				      probe->factory_length,
				      probe->factory_digest);
	crypto_free_shash(sha256);
	if (ret)
		goto out_free;
	probe->factory_hash_matched =
		!memcmp(probe->factory_digest, xr500v_factory_sha256,
			sizeof(probe->factory_digest));
	if (!probe->factory_hash_matched)
		ret = -EKEYREJECTED;

out_free:
	kfree(factory);
	return ret;
}

static int handoff_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_digital_handoff *probe = s->private;
	unsigned int i;

	mutex_lock(&probe->status_lock);
	seq_puts(s, "mode: manually armed finite ESD_PRO/FWRDY probe\n");
	seq_printf(s, "sequence_result: %d\n", probe->sequence_result);
	seq_printf(s, "rollback_write_result: %d\n",
		   probe->rollback_write_result);
	seq_printf(s, "rollback_result: %d\n", probe->rollback_result);
	seq_printf(s, "rollback_complete: %s\n",
		   probe->rollback_complete ? "yes" : "no");
	seq_printf(s, "physical_powercut_required: %s\n",
		   probe->physical_powercut_required ? "yes" : "no");
	seq_printf(s, "factory_hash_matched: %s\n",
		   probe->factory_hash_matched ? "yes" : "no");
	seq_printf(s, "mmio_writes: %u (maximum 4; success path 4)\n",
		   probe->mmio_writes);
	seq_puts(s, "i2c_en7570_apd_laser_writes: 0\n");
	seq_puts(s, "reset_pll_counter_irq_mac_scu_writes: 0\n");
	seq_printf(s, "PHYSET2: 0x%08x -> 0x%08x -> 0x%08x\n",
		   probe->before_physet2, (u32)ACTIVE_PHYSET2,
		   probe->final_physet2);
	seq_printf(s, "rollback_PHYSET2: immediate=0x%08x final=0x%08x FWRDY=%u PHYRDY=%u\n",
		   probe->rollback_immediate_physet2, probe->final_physet2,
		   !!(probe->final_physet2 & PHYSET2_FW_READY),
		   !!(probe->final_physet2 & PHYSET2_PHY_READY));
	seq_printf(s, "PHYSET2.PHYRDY: immediate=%u settled=%u coarse_500ms_result=%d\n",
		   !!(probe->rollback_immediate_physet2 & PHYSET2_PHY_READY),
		   !!(probe->final_physet2 & PHYSET2_PHY_READY),
		   probe->phy_ready_settle_result);
	seq_printf(s, "PHYSET3: 0x%08x -> 0x%08x -> 0x%08x\n",
		   probe->before_physet3, (u32)ACTIVE_PHYSET3,
		   probe->final_physet3);
	seq_printf(s, "final_guard: PHYSET10=0x%08x XPON_SETTING=0x%08x MISC=0x%08x\n",
		   probe->final_physet10, probe->final_xpon_setting,
		   probe->final_misc);
	seq_printf(s, "final_guard: PRBS_TX=0x%08x TEST_FRAME=0x%08x XPON_INT_EN=0x%08x\n",
		   probe->final_prbs_tx, probe->final_test_frame,
		   probe->final_xpon_int_en);
	seq_printf(s, "final_status: PHYSTA1=0x%08x state[20:18]=%u ANASTA1=0x%08x PHYRX_STATUS=0x%08x\n",
		   probe->final_physta1,
		   (probe->final_physta1 >> 18) & 0x7,
		   probe->final_anasta1,
		   probe->final_phyrx_status);
	seq_printf(s, "final_status: XPON_STA=0x%08x XPON_INT_STA=0x%08x\n",
		   probe->final_xpon_sta, probe->final_xpon_int_sta);
	seq_printf(s, "final_gpio_direction/value/raw: %d/%d/%d\n",
		   probe->final_gpio_direction, probe->final_gpio_value,
		   probe->final_gpio_raw_value);
	seq_printf(s, "powercut_reason_fwrdy_set: %s\n",
		   probe->final_physet2 & PHYSET2_FW_READY ? "yes" : "no");
	seq_printf(s, "powercut_reason_esd_not_restored: %s\n",
		   probe->final_physet3 & PHYSET3_ESD_PRO ? "no" : "yes");
	seq_printf(s, "powercut_reason_gpio_tx_barrier_unsafe: %s\n",
		   probe->final_gpio_direction == 0 &&
		   probe->final_gpio_value == 1 &&
		   probe->final_gpio_raw_value == 1 ? "no" : "yes");
	seq_printf(s, "powercut_reason_digital_tx_barrier_unsafe: %s\n",
		   probe->final_physet3 & PHYSET3_TXEN ||
		   probe->final_misc & MISC_ROGUE_TX_TEST ||
		   probe->final_prbs_tx || probe->final_test_frame ||
		   probe->final_xpon_int_en ? "yes" : "no");
	seq_printf(s, "powercut_reason_writable_baseline_mismatch: %s\n",
		   (probe->final_physet2 & ~PHYSET2_PHY_READY) !=
		   COLD_PHYSET2 || probe->final_physet3 != COLD_PHYSET3 ||
		   probe->final_physet10 != COLD_PHYSET10 ||
		   probe->final_xpon_setting != COLD_XPON_SETTING ?
		   "yes" : "no");
	seq_printf(s, "samples_taken: %u/%u\n", probe->samples_taken,
		   HANDOFF_SAMPLE_COUNT);

	for (i = 0; i < probe->samples_taken; i++) {
		const struct handoff_sample *sample = &probe->samples[i];

		seq_printf(s, "sample[%u]: target_ms=%u actual_us=%llu verify=%d\n",
			   i, sample->target_ms,
			   (unsigned long long)sample->actual_us,
			   sample->verify_result);
		seq_printf(s, "  PHYSET2=0x%08x PHYSET3=0x%08x PHYSTA1=0x%08x state[20:18]=%u\n",
			   sample->physet2, sample->physet3, sample->physta1,
			   (sample->physta1 >> 18) & 0x7);
		seq_printf(s, "  PHYSET2.PHYRDY=%u PHYSET10=0x%08x XPON_SETTING=0x%08x\n",
			   !!(sample->physet2 & PHYSET2_PHY_READY),
			   sample->physet10, sample->xpon_setting);
		seq_printf(s, "  PHYRX_STATUS=0x%08x XPON_STA=0x%08x\n",
			   sample->phyrx_status, sample->xpon_sta);
		seq_printf(s, "  XPON_INT_STA=0x%08x ANASTA1=0x%08x\n",
			   sample->xpon_int_sta, sample->anasta1);
		seq_printf(s, "  MISC=0x%08x PRBS_TX=0x%08x TEST_FRAME=0x%08x XPON_INT_EN=0x%08x\n",
			   sample->misc, sample->prbs_tx, sample->test_frame,
			   sample->xpon_int_en);
		seq_printf(s, "  gpio_direction/value/raw=%d/%d/%d\n",
			   sample->gpio_direction, sample->gpio_value,
			   sample->gpio_raw_value);
	}
	mutex_unlock(&probe->status_lock);

	return 0;
}

static int handoff_status_open(struct inode *inode, struct file *file)
{
	return single_open(file, handoff_status_show, inode->i_private);
}

static const struct file_operations handoff_status_fops = {
	.owner = THIS_MODULE,
	.open = handoff_status_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int xr500v_digital_handoff_probe(struct platform_device *pdev)
{
	struct xr500v_digital_handoff *probe;
	struct gpio_chip *gpio_chip;
	struct resource *resource;
	int gpio_number;
	int ret;

	if (!arm_digital_handoff)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "module digital handoff opt-in is absent\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-los-trace"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "experimental DT opt-in is absent\n");
	if (!of_machine_is_compatible("tplink,archer-xr500v"))
		return dev_err_probe(&pdev->dev, -ENODEV,
				     "machine is not the TP-Link Archer XR500v\n");

	probe = devm_kzalloc(&pdev->dev, sizeof(*probe), GFP_KERNEL);
	if (!probe)
		return -ENOMEM;
	probe->dev = &pdev->dev;
	mutex_init(&probe->status_lock);

	probe->tx_disable = devm_gpiod_get(&pdev->dev, "tx-disable", GPIOD_ASIS);
	if (IS_ERR(probe->tx_disable))
		return dev_err_probe(&pdev->dev, PTR_ERR(probe->tx_disable),
				     "cannot acquire physical TX_DISABLE\n");
	if (gpiod_is_active_low(probe->tx_disable))
		return -EINVAL;
	gpio_chip = gpiod_to_chip(probe->tx_disable);
	gpio_number = desc_to_gpio(probe->tx_disable);
	if (!gpio_chip || !gpio_chip->label || gpio_chip->base < 0 ||
	    gpio_number < gpio_chip->base ||
	    strcmp(gpio_chip->label, "tc3162-gpio") ||
	    gpio_number - gpio_chip->base != XR500V_TX_DISABLE_GPIO)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "TX_DISABLE is not exact TC3162 GPIO16\n");
	ret = tx_disable_guard(probe);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "physical TX_DISABLE is not retained high\n");

	resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!resource || resource->start != XR500V_XPON_BASE ||
	    resource_size(resource) != XR500V_XPON_SIZE)
		return dev_err_probe(&pdev->dev, -EINVAL,
				     "xPON resource is not exact\n");
	probe->base = devm_ioremap_resource(&pdev->dev, resource);
	if (IS_ERR(probe->base))
		return PTR_ERR(probe->base);

	ret = verify_factory_hash(probe);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "factory identity gate failed\n");
	ret = static_guard(probe, COLD_PHYSET2, COLD_PHYSET3, false);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "exact digital cold-state gate failed\n");

	probe->debugfs_dir =
		debugfs_create_dir("xr500v-xpon-digital-handoff-probe", NULL);
	if (IS_ERR_OR_NULL(probe->debugfs_dir))
		return -ENODEV;
	probe->debugfs_status =
		debugfs_create_file("status", 0444, probe->debugfs_dir, probe,
				    &handoff_status_fops);
	if (IS_ERR_OR_NULL(probe->debugfs_status)) {
		debugfs_remove_recursive(probe->debugfs_dir);
		return -ENODEV;
	}

	mutex_lock(&probe->status_lock);
	run_handoff(probe);
	mutex_unlock(&probe->status_lock);
	if (!probe->esd_rollback_needed && probe->sequence_result) {
		ret = probe->sequence_result;
		debugfs_remove_recursive(probe->debugfs_dir);
		return ret;
	}

	platform_set_drvdata(pdev, probe);
	if (probe->physical_powercut_required)
		dev_emerg(&pdev->dev,
			  "digital handoff rollback failed (%d); remove physical power now\n",
			  probe->rollback_result);
	else
		dev_info(&pdev->dev,
			 "digital handoff result=%d samples=%u/%u writes=%u; writable rollback complete (PHYRDY settle=%d)\n",
			 probe->sequence_result, probe->samples_taken,
			 HANDOFF_SAMPLE_COUNT, probe->mmio_writes,
			 probe->phy_ready_settle_result);

	return 0;
}

static void xr500v_digital_handoff_remove(struct platform_device *pdev)
{
	struct xr500v_digital_handoff *probe = platform_get_drvdata(pdev);

	debugfs_remove_recursive(probe->debugfs_dir);
	if (probe->physical_powercut_required)
		dev_emerg(&pdev->dev,
			  "removing probe with failed rollback; physical power cut remains required\n");
}

static const struct of_device_id xr500v_digital_handoff_of_match[] = {
	{ .compatible = "econet,en751221-en7570-los-trace-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_digital_handoff_of_match);

static struct platform_driver xr500v_digital_handoff_driver = {
	.probe = xr500v_digital_handoff_probe,
	.remove = xr500v_digital_handoff_remove,
	.driver = {
		.name = "xr500v-xpon-digital-handoff-probe",
		.of_match_table = xr500v_digital_handoff_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(xr500v_digital_handoff_driver);

MODULE_DESCRIPTION("Guarded XR500v xPON ESD_PRO/FWRDY digital handoff probe");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
