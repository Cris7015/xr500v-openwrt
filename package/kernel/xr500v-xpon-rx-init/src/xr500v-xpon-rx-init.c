// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compile-only guarded RX bring-up prototype for the XR500v xPON PHY.
 *
 * This driver intentionally has no matching node in the shipping DTS and the
 * package has no autoload entry.  A future test image must replace the passive
 * diagnostic node, opt in through both the DT property and module parameter,
 * and provide the active-high physical TX_DISABLE GPIO.
 *
 * The only xPON PHY write implemented here clears PHYSET3.ESD_PRO, matching
 * the OEM phy_dev_init() and Merbanan pon_phy_esd_deglitch_clear().  The write
 * helper cannot set TXEN and rejects a pre-existing TXEN state.  No EN7570,
 * MAC, QDMA, counter, interrupt, GPON/EPON mode, PLL or reset access exists.
 */

#include <linux/bitfield.h>
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>

#define PHYSET3			0x0108
#define   PHYSET3_TXEN		BIT(5)
#define   PHYSET3_ESD_PRO	BIT(2)

#define PHYSET10		0x0124
#define   PHYSET10_GPON_MODE	BIT(31)

#define MISC			0x01fc
#define   MISC_ROGUE_ONU_TX_TEST_MODE BIT(28)

#define BISTCTL_PRBS_TX_EN	0x04a4
#define TEST_FRAME_EN		0x0510
#define XPON_INT_EN		0x05f0

struct xr500v_xpon_rx_init {
	struct device *dev;
	void __iomem *base;
	struct gpio_desc *tx_disable;
	bool esd_was_set;
	bool change_applied;
};

static bool arm_rx_init;
module_param(arm_rx_init, bool, 0444);
MODULE_PARM_DESC(arm_rx_init,
	"Arm the compile-only RX init prototype (also requires DT opt-in)");

static bool rollback_on_remove = true;
module_param(rollback_on_remove, bool, 0444);
MODULE_PARM_DESC(rollback_on_remove,
	"Restore the original ESD_PRO bit when the prototype is removed");

static u32 rx_read(struct xr500v_xpon_rx_init *priv, u32 reg)
{
	return ioread32(priv->base + reg);
}

/*
 * The sole PHY write primitive.  It can modify only ESD_PRO and always writes
 * TXEN low.  Refuse to touch a state in which TXEN was already high so a bug
 * elsewhere cannot be hidden by this prototype.
 */
static int rx_write_esd_pro(struct xr500v_xpon_rx_init *priv, bool set)
{
	u32 old = rx_read(priv, PHYSET3);
	u32 new;

	if (old & PHYSET3_TXEN)
		return -EPERM;

	new = set ? old | PHYSET3_ESD_PRO : old & ~PHYSET3_ESD_PRO;
	new &= ~PHYSET3_TXEN;
	iowrite32(new, priv->base + PHYSET3);

	return 0;
}

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

	if (!arm_rx_init)
		return dev_err_probe(&pdev->dev, -EPERM,
				     "arm_rx_init=1 was not supplied\n");
	if (!device_property_read_bool(&pdev->dev,
				       "econet,allow-rx-only-init"))
		return dev_err_probe(&pdev->dev, -EPERM,
				     "DT RX-only opt-in is absent\n");

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;
	priv->dev = &pdev->dev;

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
	priv->esd_was_set = !!(set3 & PHYSET3_ESD_PRO);

	ret = rx_write_esd_pro(priv, false);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
				     "cannot clear signal-detect deglitch\n");
	priv->change_applied = true;

	ret = rx_preflight(priv);
	if (ret) {
		rx_write_esd_pro(priv, priv->esd_was_set);
		priv->change_applied = false;
		return dev_err_probe(&pdev->dev, ret, "RX-only postflight failed\n");
	}

	dev_info(&pdev->dev,
		 "RX-only stage 1 applied: ESD_PRO cleared, TX_DISABLE asserted, TXEN low\n");
	platform_set_drvdata(pdev, priv);
	return 0;
}

static void xr500v_xpon_rx_init_remove(struct platform_device *pdev)
{
	struct xr500v_xpon_rx_init *priv = platform_get_drvdata(pdev);

	if (rollback_on_remove && priv->change_applied)
		rx_write_esd_pro(priv, priv->esd_was_set);
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

MODULE_DESCRIPTION("Compile-only guarded XR500v xPON RX init prototype");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
