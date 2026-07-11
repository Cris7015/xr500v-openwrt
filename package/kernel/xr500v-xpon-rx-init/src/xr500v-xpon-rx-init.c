// SPDX-License-Identifier: GPL-2.0-only
/*
 * Guarded RX bring-up prototype for the XR500v xPON PHY.
 *
 * This driver intentionally has no matching node in the shipping DTS and the
 * package has no autoload entry.  A dedicated test image must replace the passive
 * diagnostic node, opt in through both the DT property and module parameter,
 * and provide the active-high physical TX_DISABLE GPIO.
 *
 * The mutually exclusive stages either clear PHYSET3.ESD_PRO, matching the OEM
 * and Merbanan deglitch operation, or clear only XPON_SETTING.TRANS_RX_SD_INV,
 * matching the OEM EN7570 polarity.  Neither helper can set TXEN and both
 * reject a pre-existing TXEN state.  No EN7570 I2C, MAC, QDMA, counter,
 * interrupt, GPON/EPON mode, PLL or reset access exists.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/seq_file.h>

#define PHYSET3			0x0108
#define   PHYSET3_TXEN		BIT(5)
#define   PHYSET3_ESD_PRO	BIT(2)

#define PHYSET10		0x0124
#define   PHYSET10_GPON_MODE	BIT(31)

#define XPON_SETTING		0x0138
#define   XPON_SETTING_TRANS_RX_SD_INV BIT(6)
#define   XPON_SETTING_XR500V_RETAINED 0x0000014f
#define   XPON_SETTING_EN7570	0x0000010f

#define MISC			0x01fc
#define   MISC_ROGUE_ONU_TX_TEST_MODE BIT(28)

#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_INT_EN		0x05f0

enum xr500v_rx_stage {
	XR500V_RX_STAGE_ESD,
	XR500V_RX_STAGE_EN7570_POLARITY,
};

struct xr500v_xpon_rx_init {
	struct device *dev;
	void __iomem *base;
	struct gpio_desc *tx_disable;
	struct dentry *debugfs_dir;
	enum xr500v_rx_stage stage;
	u32 physet3_before;
	u32 physet3_after;
	u32 xpon_setting_before;
	u32 xpon_setting_after;
	unsigned int mmio_writes;
	bool esd_was_set;
	bool change_applied;
};

static bool arm_rx_init;
module_param(arm_rx_init, bool, 0444);
MODULE_PARM_DESC(arm_rx_init,
	"Arm the ESD_PRO RX stage (mutually exclusive; also requires DT opt-in)");

static bool arm_en7570_rx_polarity;
module_param(arm_en7570_rx_polarity, bool, 0444);
MODULE_PARM_DESC(arm_en7570_rx_polarity,
	"Arm the EN7570 RX LOS/SD polarity stage (mutually exclusive; also requires DT opt-in)");

static bool rollback_on_remove = true;
module_param(rollback_on_remove, bool, 0444);
MODULE_PARM_DESC(rollback_on_remove,
	"Restore the selected stage's original bit when the prototype is removed");

static u32 rx_read(struct xr500v_xpon_rx_init *priv, u32 reg)
{
	return ioread32(priv->base + reg);
}

/*
 * The sole PHY write primitive.  It can modify only ESD_PRO and always writes
 * TXEN low.  Refuse to touch a state in which TXEN was already high so a bug
 * elsewhere cannot be hidden by this prototype.
 */
static noinline int rx_write_esd_pro(struct xr500v_xpon_rx_init *priv, bool set)
{
	u32 old = rx_read(priv, PHYSET3);
	u32 new;

	if (old & PHYSET3_TXEN)
		return -EPERM;

	new = set ? old | PHYSET3_ESD_PRO : old & ~PHYSET3_ESD_PRO;
	new &= ~PHYSET3_TXEN;
	iowrite32(new, priv->base + PHYSET3);
	priv->mmio_writes++;

	return 0;
}

/*
 * The only write primitive for the EN7570 polarity stage.  It can modify only
 * the RX signal-detect inversion bit.  The burst, TX-fault and TX-SD polarity
 * bits are preserved, and a live TXEN state makes the write fail closed.
 */
static noinline int
rx_write_en7570_polarity(struct xr500v_xpon_rx_init *priv, bool inverted)
{
	u32 old;
	u32 new;

	if (rx_read(priv, PHYSET3) & PHYSET3_TXEN)
		return -EPERM;

	old = rx_read(priv, XPON_SETTING);
	new = inverted ? old | XPON_SETTING_TRANS_RX_SD_INV :
			 old & ~XPON_SETTING_TRANS_RX_SD_INV;
	iowrite32(new, priv->base + XPON_SETTING);
	priv->mmio_writes++;

	return 0;
}

static int rx_rollback(struct xr500v_xpon_rx_init *priv)
{
	if (priv->stage == XR500V_RX_STAGE_ESD)
		return rx_write_esd_pro(priv, priv->esd_was_set);

	return rx_write_en7570_polarity(priv,
		priv->xpon_setting_before & XPON_SETTING_TRANS_RX_SD_INV);
}

static int rx_status_show(struct seq_file *s, void *unused)
{
	struct xr500v_xpon_rx_init *priv = s->private;
	u32 set3 = rx_read(priv, PHYSET3);
	u32 misc = rx_read(priv, MISC);
	int direction = gpiod_get_direction(priv->tx_disable);
	int asserted = gpiod_get_value_cansleep(priv->tx_disable);

	seq_printf(s, "stage:                %s\n",
		   priv->stage == XR500V_RX_STAGE_ESD ? "esd-pro" :
		   "en7570-rx-polarity");
	seq_printf(s, "physet3_before:       0x%08x\n", priv->physet3_before);
	seq_printf(s, "physet3_after:        0x%08x\n", priv->physet3_after);
	seq_printf(s, "physet3_current:      0x%08x\n", set3);
	seq_printf(s, "esd_pro_before:       %s\n",
		   priv->esd_was_set ? "set" : "clear");
	seq_printf(s, "esd_pro_current:      %s\n",
		   set3 & PHYSET3_ESD_PRO ? "set" : "clear");
	seq_printf(s, "xpon_setting_before:  0x%08x\n",
		   priv->xpon_setting_before);
	seq_printf(s, "xpon_setting_after:   0x%08x\n",
		   priv->xpon_setting_after);
	seq_printf(s, "xpon_setting_current: 0x%08x\n",
		   rx_read(priv, XPON_SETTING));
	seq_printf(s, "rx_sd_inverted:       %s\n",
		   rx_read(priv, XPON_SETTING) & XPON_SETTING_TRANS_RX_SD_INV ?
		   "yes" : "no");
	seq_printf(s, "tx_enable:            %s\n",
		   set3 & PHYSET3_TXEN ? "YES" : "no");
	seq_printf(s, "tx_disable_direction: %s\n",
		   direction == 0 ? "output" : direction == 1 ? "input" : "error");
	seq_printf(s, "tx_disable_asserted:  %s\n",
		   asserted == 1 ? "yes" : asserted == 0 ? "NO" : "error");
	seq_printf(s, "gpon_mode:            %s\n",
		   rx_read(priv, PHYSET10) & PHYSET10_GPON_MODE ? "yes" : "NO");
	seq_printf(s, "rogue_onu_test_mode: %s\n",
		   misc & MISC_ROGUE_ONU_TX_TEST_MODE ? "YES" : "no");
	seq_printf(s, "prbs_tx_enable_raw:  0x%08x\n",
		   rx_read(priv, BISTCTL_PRBS_TX_EN));
	seq_printf(s, "test_frame_enable:   0x%08x\n",
		   rx_read(priv, TEST_FRAME_EN));
	seq_printf(s, "interrupt_enable:    0x%08x\n",
		   rx_read(priv, XPON_INT_EN));
	seq_printf(s, "mmio_writes:          %u\n", priv->mmio_writes);
	seq_puts(s, "en7570_access:         no\n");
	seq_puts(s, "pll_or_reset_access:  no\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rx_status);

static int rx_preflight(struct xr500v_xpon_rx_init *priv)
{
	int direction;
	int asserted;
	u32 set3;

	direction = gpiod_get_direction(priv->tx_disable);
	if (direction != 0) {
		dev_err(priv->dev, "TX_DISABLE is not an output: %d\n", direction);
		return direction < 0 ? direction : -EPERM;
	}

	asserted = gpiod_get_value_cansleep(priv->tx_disable);
	if (asserted != 1) {
		dev_err(priv->dev, "TX_DISABLE is not asserted: %d\n", asserted);
		return asserted < 0 ? asserted : -EPERM;
	}

	set3 = rx_read(priv, PHYSET3);
	if (set3 & PHYSET3_TXEN) {
		dev_err(priv->dev, "TXEN is active\n");
		return -EPERM;
	}
	if (!(rx_read(priv, PHYSET10) & PHYSET10_GPON_MODE)) {
		dev_err(priv->dev, "PHY is not in GPON mode\n");
		return -EPERM;
	}
	if (rx_read(priv, MISC) & MISC_ROGUE_ONU_TX_TEST_MODE) {
		dev_err(priv->dev, "rogue-ONU TX test mode is active\n");
		return -EPERM;
	}
	if (rx_read(priv, BISTCTL_PRBS_TX_EN)) {
		dev_err(priv->dev, "PRBS transmitter is active\n");
		return -EPERM;
	}
	if (rx_read(priv, TEST_FRAME_EN)) {
		dev_err(priv->dev, "test-frame transmitter is active\n");
		return -EPERM;
	}
	if (rx_read(priv, XPON_INT_EN)) {
		dev_err(priv->dev, "xPON interrupts are already enabled\n");
		return -EPERM;
	}

	return 0;
}

static int xr500v_xpon_rx_init_probe(struct platform_device *pdev)
{
	struct xr500v_xpon_rx_init *priv;
	u32 set3;
	int ret;

	if (arm_rx_init == arm_en7570_rx_polarity)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "select exactly one armed RX stage\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-rx-only-init"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT RX-only opt-in is absent\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;
	priv->stage = arm_rx_init ? XR500V_RX_STAGE_ESD :
		XR500V_RX_STAGE_EN7570_POLARITY;

	/* Assert the physical, active-high TX_DISABLE gate before mapping PHY. */
	priv->tx_disable = devm_gpiod_get(&pdev->dev, "tx-disable",
					 GPIOD_OUT_HIGH);
	if (IS_ERR(priv->tx_disable))
		return dev_err_probe(&pdev->dev, PTR_ERR(priv->tx_disable),
				     "cannot assert physical TX_DISABLE\n");

	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	ret = rx_preflight(priv);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "RX-only preflight failed\n");

	set3 = rx_read(priv, PHYSET3);
	priv->physet3_before = set3;
	priv->esd_was_set = !!(set3 & PHYSET3_ESD_PRO);
	priv->xpon_setting_before = rx_read(priv, XPON_SETTING);
	if (priv->stage == XR500V_RX_STAGE_EN7570_POLARITY &&
	    priv->xpon_setting_before != XPON_SETTING_XR500V_RETAINED)
		return dev_err_probe(&pdev->dev, -EPERM,
			"unexpected retained XPON_SETTING: 0x%08x\n",
			priv->xpon_setting_before);

	if (priv->stage == XR500V_RX_STAGE_ESD)
		ret = rx_write_esd_pro(priv, false);
	else
		ret = rx_write_en7570_polarity(priv, false);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot apply selected RX stage\n");
	priv->change_applied = true;
	priv->physet3_after = rx_read(priv, PHYSET3);
	priv->xpon_setting_after = rx_read(priv, XPON_SETTING);

	ret = rx_preflight(priv);
	if (!ret && priv->stage == XR500V_RX_STAGE_ESD &&
	    ((priv->physet3_before ^ priv->physet3_after) & ~PHYSET3_ESD_PRO ||
	     priv->physet3_after & PHYSET3_ESD_PRO))
		ret = -EIO;
	if (!ret && priv->stage == XR500V_RX_STAGE_EN7570_POLARITY &&
	    (priv->xpon_setting_after != XPON_SETTING_EN7570 ||
	     ((priv->xpon_setting_before ^ priv->xpon_setting_after) &
	      ~XPON_SETTING_TRANS_RX_SD_INV)))
		ret = -EIO;
	if (ret) {
		rx_rollback(priv);
		priv->change_applied = false;
		return dev_err_probe(&pdev->dev, ret, "RX-only postflight failed\n");
	}

	priv->debugfs_dir = debugfs_create_dir("xr500v-xpon-rx-init", NULL);
	if (IS_ERR(priv->debugfs_dir)) {
		rx_rollback(priv);
		priv->change_applied = false;
		return PTR_ERR(priv->debugfs_dir);
	}
	debugfs_create_file("status", 0444, priv->debugfs_dir, priv,
			    &rx_status_fops);

	dev_info(&pdev->dev, "RX-only %s stage applied; TX_DISABLE asserted, TXEN low\n",
		 priv->stage == XR500V_RX_STAGE_ESD ? "ESD_PRO" :
		 "EN7570 polarity");
	platform_set_drvdata(pdev, priv);
	return 0;
}

static void xr500v_xpon_rx_init_remove(struct platform_device *pdev)
{
	struct xr500v_xpon_rx_init *priv = platform_get_drvdata(pdev);

	debugfs_remove_recursive(priv->debugfs_dir);
	if (rollback_on_remove && priv->change_applied)
		rx_rollback(priv);
}

static const struct of_device_id xr500v_xpon_rx_init_of_match[] = {
	{ .compatible = "econet,en751221-xpon-rx-init-experimental" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_xpon_rx_init_of_match);

static struct platform_driver xr500v_xpon_rx_init_driver = {
	.probe = xr500v_xpon_rx_init_probe,
	.remove = xr500v_xpon_rx_init_remove,
	.driver = {
		.name = "xr500v-xpon-rx-init",
		.of_match_table = xr500v_xpon_rx_init_of_match,
	},
};
module_platform_driver(xr500v_xpon_rx_init_driver);

MODULE_DESCRIPTION("Guarded XR500v xPON RX init prototype");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
