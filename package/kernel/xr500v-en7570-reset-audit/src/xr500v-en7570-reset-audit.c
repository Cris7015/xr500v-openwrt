// SPDX-License-Identifier: GPL-2.0-only
/*
 * One-shot, non-transactional EN7570 reset observation for the XR500v.
 *
 * This driver has no matching shipping DT node and no autoload entry. It
 * accepts only the exact phase-12 retained baseline, asserts physical
 * TX_DISABLE, verifies all xPON transmit paths are inactive, performs the OEM
 * four-byte SW_RESET RMW once, then exposes before/after snapshots. It does no
 * LOS, ADC, RSSI, APD, TGEN, current, interrupt or rollback operation.
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

#define PHYSET3			0x0108
#define PHYSET3_TXEN		BIT(5)
#define MISC			0x01fc
#define MISC_ROGUE_TX_TEST	BIT(28)
#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_INT_EN		0x05f0

#define EN7570_ID_REG		0x0170
#define EN7570_EXPECTED_ID	0x03
#define EN7570_SAFE_PROTECT	0x0100
#define EN7570_ROGUE_TX		0x0168
#define EN7570_SW_RESET		0x0300

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
	{ "svadc_pd", 0x0024, { 0x00, 0x00, 0x01, 0x00 } },
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
	{ "adc_probe", 0x0154, { 0x00, 0x00, 0x00, 0x00 } },
	{ "probe_control", 0x0158, { 0x00, 0x00, 0x00, 0x00 } },
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
	u8 reset_write[4];
	unsigned int i2c_write_attempts;
	int reset_result;
	int post_snapshot_result;
	int postflight_result;
	bool module_pinned;
};

static bool arm_en7570_reset_audit;
module_param(arm_en7570_reset_audit, bool, 0444);
MODULE_PARM_DESC(arm_en7570_reset_audit,
	"Arm the one-shot non-transactional EN7570 reset observation");

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

	if (gpiod_get_direction(audit->tx_disable) != 0 ||
	    gpiod_get_value_cansleep(audit->tx_disable) != 1 ||
	    xpon_read(audit, PHYSET3) & PHYSET3_TXEN)
		return -EPERM;

	memcpy(data + 2, value, 4);
	audit->i2c_write_attempts++;
	ret = i2c_transfer(audit->en7570->adapter, &message, 1);
	if (ret < 0)
		return ret;
	if (ret != 1)
		return -EIO;

	return 0;
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

	seq_puts(s, "operation:             EN7570 OEM four-byte SW_RESET RMW\n");
	seq_printf(s, "reset_payload:         %4ph\n", audit->reset_write);
	seq_printf(s, "i2c_write_attempts:    %u\n", audit->i2c_write_attempts);
	seq_printf(s, "reset_result:          %d\n", audit->reset_result);
	seq_printf(s, "post_snapshot_result:  %d\n",
		   audit->post_snapshot_result);
	seq_printf(s, "postflight_result:     %d\n", audit->postflight_result);
	seq_printf(s, "module_pinned:         %s\n",
		   audit->module_pinned ? "yes" : "NO");
	seq_printf(s, "tx_disable_asserted:   %s\n",
		   gpiod_get_value_cansleep(audit->tx_disable) == 1 ? "yes" : "NO");
	seq_printf(s, "xpon_txen:             %s\n",
		   xpon_read(audit, PHYSET3) & PHYSET3_TXEN ? "YES" : "no");
	for (i = 0; i < ARRAY_SIZE(audit_regs); i++)
		seq_printf(s, "%-18s 0x%04x before=%4ph after=%4ph\n",
			   audit_regs[i].name, audit_regs[i].reg,
			   audit->before[i], audit->after[i]);
	seq_puts(s, "los_adc_rssi_init:     no\n");
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
	u8 id;
	int ret;

	if (!arm_en7570_reset_audit)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "module reset opt-in is absent\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-en7570-reset-audit"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT reset opt-in is absent\n");

	audit = devm_kzalloc(&pdev->dev, sizeof(*audit), GFP_KERNEL);
	if (!audit)
		return -ENOMEM;
	audit->dev = &pdev->dev;
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
		 "EN7570 reset observed; TX_DISABLE held; physical power cycle is the recovery boundary\n");
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

MODULE_DESCRIPTION("Guarded XR500v EN7570 whole-device reset audit");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
