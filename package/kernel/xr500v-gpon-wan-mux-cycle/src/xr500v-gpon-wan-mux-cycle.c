// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP-Link Archer XR500v / EcoNet EN751221 reversible WAN-mux cycle.
 *
 * The initial GPON MAC read stalled while SCU_WAN_CONF selected ATM (mode 3).
 * This narrowly scoped lab module proves that the mux itself can select GPON
 * and return to the frozen original value without touching the GPON MAC.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/ktime.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/seq_file.h>
#include <linux/stop_machine.h>

#define EN751221_SCU_WAN_CONF		0x070
#define EN751221_SCU_RESET_CTRL2		0x830
#define EN751221_SCU_RESET_CTRL1		0x834

#define WAN_MODE			GENMASK(2, 0)
#define WAN_MODE_GPON			0
#define WAN_MODE_ATM			3
#define RESET2_XPON_PHY			BIT(0)
#define RESET1_QDMA1			BIT(1)
#define RESET1_QDMA2			BIT(2)
#define RESET1_FE			BIT(21)
#define RESET1_XPON_MAC			BIT(31)
#define REQUIRED_RELEASED_RESET1	(RESET1_QDMA1 | RESET1_QDMA2 | \
					 RESET1_FE | RESET1_XPON_MAC)

struct xr500v_gpon_wan_mux_cycle {
	u32 wan_before;
	u32 wan_gpon;
	u32 wan_after;
	u32 reset_ctrl2;
	u32 reset_ctrl1;
	u64 cycle_ns;
	struct dentry *debugfs_dir;
};

static bool allow_wan_mux_cycle;
module_param(allow_wan_mux_cycle, bool, 0400);
MODULE_PARM_DESC(allow_wan_mux_cycle,
		 "Arm one reversible ATM -> GPON -> ATM SCU mux cycle");

static struct xr500v_gpon_wan_mux_cycle result;

struct xr500v_gpon_wan_mux_cycle_context {
	struct regmap *scu;
	bool update_attempted;
	int restore_err;
	int final_read_err;
};

static int status_show(struct seq_file *s, void *unused)
{
	seq_puts(s, "mode: manually armed reversible WAN-mux cycle\n");
	seq_puts(s, "sequence: ATM(3) -> GPON(0) -> ATM(3)\n");
	seq_puts(s, "gpon_mac_mmio_attempted: no\n");
	seq_puts(s, "gpon_mac_mmio_reads: 0\n");
	seq_puts(s, "scu_writes_performed: 2\n");
	seq_puts(s, "other_mmio_writes_performed: 0\n");
	seq_puts(s, "irq_or_poller_registered: no\n");
	seq_puts(s, "reset_clock_gpio_i2c_changes: 0\n");
	seq_puts(s, "laser_apd_phy_changes: 0\n");
	seq_puts(s, "fibre_required: no\n");

	seq_puts(s, "\nreadback:\n");
	seq_printf(s, "  wan_before: 0x%08x (mode %u)\n", result.wan_before,
		   (u32)FIELD_GET(WAN_MODE, result.wan_before));
	seq_printf(s, "  wan_gpon:   0x%08x (mode %u)\n", result.wan_gpon,
		   (u32)FIELD_GET(WAN_MODE, result.wan_gpon));
	seq_printf(s, "  wan_after:  0x%08x (mode %u)\n", result.wan_after,
		   (u32)FIELD_GET(WAN_MODE, result.wan_after));
	seq_printf(s, "  reset_ctrl2: 0x%08x\n", result.reset_ctrl2);
	seq_printf(s, "  reset_ctrl1: 0x%08x\n", result.reset_ctrl1);
	seq_printf(s, "  measured_cycle_ns: %llu\n", result.cycle_ns);
	seq_puts(s, "  original_mode_restored: yes\n");
	seq_puts(s, "  non_mode_bits_preserved: yes\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int xr500v_gpon_wan_mux_cycle_stopped(void *data)
{
	struct xr500v_gpon_wan_mux_cycle_context *ctx = data;
	u32 original_mode;
	u64 cycle_start;
	int err = 0;

	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF,
			  &result.wan_before);
	if (err)
		return err;
	err = regmap_read(ctx->scu, EN751221_SCU_RESET_CTRL2,
			  &result.reset_ctrl2);
	if (err)
		return err;
	err = regmap_read(ctx->scu, EN751221_SCU_RESET_CTRL1,
			  &result.reset_ctrl1);
	if (err)
		return err;

	original_mode = FIELD_GET(WAN_MODE, result.wan_before);
	if (original_mode != WAN_MODE_ATM)
		return -EUCLEAN;

	if ((result.reset_ctrl2 & RESET2_XPON_PHY) ||
	    (result.reset_ctrl1 & REQUIRED_RELEASED_RESET1))
		return -EBUSY;

	cycle_start = ktime_get_ns();
	ctx->update_attempted = true;
	err = regmap_update_bits(ctx->scu, EN751221_SCU_WAN_CONF, WAN_MODE,
				 WAN_MODE_GPON);
	if (err)
		goto restore_mode;

	err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF, &result.wan_gpon);
	if (!err && FIELD_GET(WAN_MODE, result.wan_gpon) != WAN_MODE_GPON)
		err = -EIO;

restore_mode:
	ctx->restore_err = regmap_update_bits(ctx->scu,
					      EN751221_SCU_WAN_CONF, WAN_MODE,
					      original_mode);
	result.cycle_ns = ktime_get_ns() - cycle_start;

	ctx->final_read_err = regmap_read(ctx->scu, EN751221_SCU_WAN_CONF,
					  &result.wan_after);

	if (ctx->restore_err)
		return ctx->restore_err;
	if (ctx->final_read_err)
		return ctx->final_read_err;
	if (result.wan_after != result.wan_before)
		return -EIO;

	return err;
}

static int __init xr500v_gpon_wan_mux_cycle_init(void)
{
	struct xr500v_gpon_wan_mux_cycle_context ctx = {};
	int err;

	if (!allow_wan_mux_cycle) {
		pr_err("xr500v-gpon-wan-mux-cycle: refused: allow_wan_mux_cycle=1 is required\n");
		return -EPERM;
	}

	if (!of_machine_is_compatible("tplink,archer-xr500v") ||
	    !of_machine_is_compatible("econet,en751221"))
		return -ENODEV;

	ctx.scu = syscon_regmap_lookup_by_compatible("econet,en751221-scu");
	if (IS_ERR(ctx.scu))
		return PTR_ERR(ctx.scu);
	if (regmap_might_sleep(ctx.scu))
		return -EOPNOTSUPP;

	/* Stop both EN751221 VPEs while the shared WAN selector is GPON. */
	err = stop_machine(xr500v_gpon_wan_mux_cycle_stopped, &ctx, NULL);

	if (ctx.restore_err) {
		pr_emerg("xr500v-gpon-wan-mux-cycle: failed to restore ATM mode: %d\n",
			 ctx.restore_err);
		return ctx.restore_err;
	}
	if (ctx.final_read_err)
		return ctx.final_read_err;
	if (ctx.update_attempted && result.wan_after != result.wan_before) {
		pr_emerg("xr500v-gpon-wan-mux-cycle: ATM restore readback mismatch: 0x%08x\n",
			 result.wan_after);
		return -EIO;
	}
	if (err)
		return err;

	result.debugfs_dir =
		debugfs_create_dir("xr500v-gpon-wan-mux-cycle", NULL);
	if (IS_ERR(result.debugfs_dir))
		return PTR_ERR(result.debugfs_dir);
	if (!result.debugfs_dir)
		return -ENOMEM;

	if (IS_ERR_OR_NULL(debugfs_create_file("status", 0400,
					       result.debugfs_dir, NULL,
					       &status_fops))) {
		err = -ENOMEM;
		goto remove_debugfs;
	}

	pr_info("xr500v-gpon-wan-mux-cycle: GPON mux selected and original ATM mode restored; GPON MAC untouched\n");
	return 0;

remove_debugfs:
	debugfs_remove_recursive(result.debugfs_dir);
	return err;
}

static void __exit xr500v_gpon_wan_mux_cycle_exit(void)
{
	debugfs_remove_recursive(result.debugfs_dir);
	pr_info("xr500v-gpon-wan-mux-cycle: unloaded\n");
}

module_init(xr500v_gpon_wan_mux_cycle_init);
module_exit(xr500v_gpon_wan_mux_cycle_exit);

MODULE_DESCRIPTION("Reversible EN751221 GPON WAN-mux cycle for XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
