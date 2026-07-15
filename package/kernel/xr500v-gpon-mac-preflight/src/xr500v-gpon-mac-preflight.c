// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP-Link Archer XR500v / EcoNet EN751221 GPON MAC bus preflight.
 *
 * A live, read-only access to the GPON MAC register window at 0x1fb64000
 * stalled the EN751221 bus before any value returned.  This replacement probe
 * therefore reads only the already-active SCU syscon and never maps or touches
 * the GPON MAC window.  It records the WAN mux and reset prerequisites needed
 * to explain that stall before another GPON access is designed.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>

#define EN751221_SCU_WAN_CONF		0x070
#define EN751221_SCU_RESET_CTRL2		0x830
#define EN751221_SCU_RESET_CTRL1		0x834

#define WAN_MODE			GENMASK(2, 0)
#define WAN_MODE_GPON			0
#define RESET2_XPON_PHY			BIT(0)
#define RESET1_QDMA1			BIT(1)
#define RESET1_QDMA2			BIT(2)
#define RESET1_FE			BIT(21)
#define RESET1_XPON_MAC			BIT(31)

struct xr500v_gpon_mac_preflight {
	u32 wan_conf;
	u32 reset_ctrl2;
	u32 reset_ctrl1;
	struct dentry *debugfs_dir;
};

static struct xr500v_gpon_mac_preflight preflight;

static const char *reset_state(u32 value, u32 mask)
{
	return value & mask ? "asserted" : "released";
}

static int status_show(struct seq_file *s, void *unused)
{
	u32 mode = FIELD_GET(WAN_MODE, preflight.wan_conf);

	seq_puts(s, "mode: SCU-only read-only preflight\n");
	seq_puts(s, "gpon_mac_mmio_attempted: no\n");
	seq_puts(s, "gpon_mac_mmio_reads: 0\n");
	seq_puts(s, "mmio_writes_performed: 0\n");
	seq_puts(s, "irq_or_poller_registered: no\n");
	seq_puts(s, "reset_clock_gpio_i2c_changes: 0\n");
	seq_puts(s, "laser_apd_phy_changes: 0\n");
	seq_puts(s, "fibre_required: no\n");

	seq_puts(s, "\nscu_snapshot:\n");
	seq_printf(s, "  WAN_CONF[0x070]:    0x%08x\n", preflight.wan_conf);
	seq_printf(s, "  RESET_CTRL2[0x830]: 0x%08x\n",
		   preflight.reset_ctrl2);
	seq_printf(s, "  RESET_CTRL1[0x834]: 0x%08x\n",
		   preflight.reset_ctrl1);

	seq_puts(s, "\ndecoded:\n");
	seq_printf(s, "  wan_mode: %u%s\n", mode,
		   mode == WAN_MODE_GPON ? " (GPON)" : " (not GPON)");
	seq_printf(s, "  xpon_phy_reset: %s\n",
		   reset_state(preflight.reset_ctrl2, RESET2_XPON_PHY));
	seq_printf(s, "  xpon_mac_reset: %s\n",
		   reset_state(preflight.reset_ctrl1, RESET1_XPON_MAC));
	seq_printf(s, "  fe_reset: %s\n",
		   reset_state(preflight.reset_ctrl1, RESET1_FE));
	seq_printf(s, "  qdma1_reset: %s\n",
		   reset_state(preflight.reset_ctrl1, RESET1_QDMA1));
	seq_printf(s, "  qdma2_reset: %s\n",
		   reset_state(preflight.reset_ctrl1, RESET1_QDMA2));
	seq_printf(s, "  mac_read_prerequisites_visible: %s\n",
		   mode == WAN_MODE_GPON &&
		   !(preflight.reset_ctrl2 & RESET2_XPON_PHY) &&
		   !(preflight.reset_ctrl1 & RESET1_XPON_MAC) ?
		   "yes (additional hidden prerequisite still possible)" : "no");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int __init xr500v_gpon_mac_preflight_init(void)
{
	struct regmap *scu;
	int err;

	if (!of_machine_is_compatible("tplink,archer-xr500v") ||
	    !of_machine_is_compatible("econet,en751221"))
		return -ENODEV;

	scu = syscon_regmap_lookup_by_compatible("econet,en751221-scu");
	if (IS_ERR(scu))
		return PTR_ERR(scu);

	err = regmap_read(scu, EN751221_SCU_WAN_CONF, &preflight.wan_conf);
	if (err)
		return err;
	err = regmap_read(scu, EN751221_SCU_RESET_CTRL2,
			  &preflight.reset_ctrl2);
	if (err)
		return err;
	err = regmap_read(scu, EN751221_SCU_RESET_CTRL1,
			  &preflight.reset_ctrl1);
	if (err)
		return err;

	preflight.debugfs_dir =
		debugfs_create_dir("xr500v-gpon-mac-preflight", NULL);
	if (IS_ERR(preflight.debugfs_dir))
		return PTR_ERR(preflight.debugfs_dir);
	if (!preflight.debugfs_dir)
		return -ENOMEM;

	if (IS_ERR_OR_NULL(debugfs_create_file("status", 0400,
					       preflight.debugfs_dir, NULL,
					       &status_fops))) {
		err = -ENOMEM;
		goto remove_debugfs;
	}

	pr_info("xr500v-gpon-mac-preflight: SCU snapshot captured; GPON MAC untouched; 0 writes\n");
	return 0;

remove_debugfs:
	debugfs_remove_recursive(preflight.debugfs_dir);
	return err;
}

static void __exit xr500v_gpon_mac_preflight_exit(void)
{
	debugfs_remove_recursive(preflight.debugfs_dir);
	pr_info("xr500v-gpon-mac-preflight: unloaded\n");
}

module_init(xr500v_gpon_mac_preflight_init);
module_exit(xr500v_gpon_mac_preflight_exit);

MODULE_DESCRIPTION("SCU-only GPON MAC bus preflight for the XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
