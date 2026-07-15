// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP-Link Archer XR500v / EcoNet EN751221 passive GPON MAC snapshot.
 *
 * A one-register live probe proved that the GPON MAC is readable only after
 * SCU_WAN_CONF selects GPON.  This follow-up reads six OEM-documented status
 * registers inside the same globally quiesced ATM -> GPON -> ATM sequence.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/ioport.h>
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

#define GPON_MAC_PHYS			0x1fb64000
#define GPON_MAC_SIZE			0x000000c0

#define GPON_G_ONU_ID			0x000
#define GPON_G_GBL_CFG			0x004
#define GPON_G_INT_ENABLE		0x00c
#define GPON_G_PLOAMU_FIFO_STS		0x050
#define GPON_G_PLOAMD_FIFO_STS		0x058
#define GPON_G_ACTIVATION_ST		0x0bc

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

#define ONU_ID_VALID			BIT(15)
#define ONU_ID_VALUE			GENMASK(7, 0)
#define GBL_CFG_US_FEC_ENABLE		BIT(16)
#define GBL_CFG_SR_BLOCK_SIZE		GENMASK(7, 0)
#define PLOAM_FIFO_LEVEL		GENMASK(7, 0)
#define PLOAM_FIFO_WATERMARK		GENMASK(23, 16)
#define PLOAMU_FIFO_UNDERRUN		BIT(31)
#define PLOAMD_FIFO_OVERRUN		BIT(31)
#define ACTIVATION_STATE		GENMASK(2, 0)

struct xr500v_gpon_mac_snapshot {
	u32 wan_before;
	u32 wan_gpon;
	u32 wan_after;
	u32 reset_ctrl2;
	u32 reset_ctrl1;
	u32 onu_id;
	u32 global_config;
	u32 interrupt_enable;
	u32 ploamu_fifo_status;
	u32 ploamd_fifo_status;
	u32 activation_status;
	u64 cycle_ns;
	struct dentry *debugfs_dir;
};

struct xr500v_gpon_mac_snapshot_context {
	struct regmap *scu;
	void __iomem *base;
	bool update_attempted;
	int restore_err;
	int final_read_err;
};

static bool allow_snapshot;
module_param(allow_snapshot, bool, 0400);
MODULE_PARM_DESC(allow_snapshot,
		 "Arm one mux-guarded six-register GPON MAC snapshot");

static struct xr500v_gpon_mac_snapshot result;

static int status_show(struct seq_file *s, void *unused)
{
	seq_puts(s, "mode: manually armed mux-guarded passive snapshot\n");
	seq_puts(s, "sequence: ATM(3) -> GPON(0) -> six reads -> ATM(3)\n");
	seq_puts(s, "gpon_mac_mmio_reads: 6\n");
	seq_puts(s, "gpon_mac_mmio_writes: 0\n");
	seq_puts(s, "scu_writes_performed: 2\n");
	seq_puts(s, "mapping_live: no\n");
	seq_puts(s, "irq_or_poller_registered: no\n");
	seq_puts(s, "reset_clock_gpio_i2c_changes: 0\n");
	seq_puts(s, "laser_apd_phy_changes: 0\n");
	seq_puts(s, "fifo_data_reads: 0\n");
	seq_puts(s, "fibre_required: no\n");

	seq_puts(s, "\nscu_readback:\n");
	seq_printf(s, "  wan_before: 0x%08x (mode %u)\n", result.wan_before,
		   (u32)FIELD_GET(WAN_MODE, result.wan_before));
	seq_printf(s, "  wan_gpon:   0x%08x (mode %u)\n", result.wan_gpon,
		   (u32)FIELD_GET(WAN_MODE, result.wan_gpon));
	seq_printf(s, "  wan_after:  0x%08x (mode %u)\n", result.wan_after,
		   (u32)FIELD_GET(WAN_MODE, result.wan_after));
	seq_printf(s, "  reset_ctrl2: 0x%08x\n", result.reset_ctrl2);
	seq_printf(s, "  reset_ctrl1: 0x%08x\n", result.reset_ctrl1);

	seq_puts(s, "\ngpon_mac_snapshot:\n");
	seq_printf(s, "  G_ONU_ID[0x00]:          0x%08x\n", result.onu_id);
	seq_printf(s, "  G_GBL_CFG[0x04]:         0x%08x\n",
		   result.global_config);
	seq_printf(s, "  G_INT_ENABLE[0x0c]:      0x%08x\n",
		   result.interrupt_enable);
	seq_printf(s, "  G_PLOAMu_FIFO_STS[0x50]: 0x%08x\n",
		   result.ploamu_fifo_status);
	seq_printf(s, "  G_PLOAMd_FIFO_STS[0x58]: 0x%08x\n",
		   result.ploamd_fifo_status);
	seq_printf(s, "  G_ACTIVATION_ST[0xbc]:   0x%08x\n",
		   result.activation_status);

	seq_puts(s, "\ndecoded:\n");
	seq_printf(s, "  onu_id_valid: %s\n",
		   result.onu_id & ONU_ID_VALID ? "yes" : "no");
	seq_printf(s, "  onu_id: 0x%02x\n",
		   (u32)FIELD_GET(ONU_ID_VALUE, result.onu_id));
	seq_printf(s, "  upstream_fec_enable: %s\n",
		   result.global_config & GBL_CFG_US_FEC_ENABLE ? "yes" : "no");
	seq_printf(s, "  superframe_block_size: %u\n",
		   (u32)FIELD_GET(GBL_CFG_SR_BLOCK_SIZE,
				  result.global_config));
	seq_printf(s, "  ploamu_fifo_available: %u\n",
		   (u32)FIELD_GET(PLOAM_FIFO_LEVEL,
				  result.ploamu_fifo_status));
	seq_printf(s, "  ploamu_fifo_min_available: %u\n",
		   (u32)FIELD_GET(PLOAM_FIFO_WATERMARK,
				  result.ploamu_fifo_status));
	seq_printf(s, "  ploamu_fifo_underrun: %s\n",
		   result.ploamu_fifo_status & PLOAMU_FIFO_UNDERRUN ?
		   "yes" : "no");
	seq_printf(s, "  ploamd_fifo_used: %u\n",
		   (u32)FIELD_GET(PLOAM_FIFO_LEVEL,
				  result.ploamd_fifo_status));
	seq_printf(s, "  ploamd_fifo_max_used: %u\n",
		   (u32)FIELD_GET(PLOAM_FIFO_WATERMARK,
				  result.ploamd_fifo_status));
	seq_printf(s, "  ploamd_fifo_overrun: %s\n",
		   result.ploamd_fifo_status & PLOAMD_FIFO_OVERRUN ?
		   "yes" : "no");
	seq_printf(s, "  activation_state: O%u\n",
		   (u32)FIELD_GET(ACTIVATION_STATE, result.activation_status));
	seq_printf(s, "  measured_cycle_ns: %llu\n", result.cycle_ns);
	seq_puts(s, "  original_mode_restored: yes\n");
	seq_puts(s, "  non_mode_bits_preserved: yes\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int xr500v_gpon_mac_snapshot_stopped(void *data)
{
	struct xr500v_gpon_mac_snapshot_context *ctx = data;
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
	if (err || FIELD_GET(WAN_MODE, result.wan_gpon) != WAN_MODE_GPON) {
		if (!err)
			err = -EIO;
		goto restore_mode;
	}

	result.onu_id = ioread32(ctx->base + GPON_G_ONU_ID);
	result.global_config = ioread32(ctx->base + GPON_G_GBL_CFG);
	result.interrupt_enable = ioread32(ctx->base + GPON_G_INT_ENABLE);
	result.ploamu_fifo_status =
		ioread32(ctx->base + GPON_G_PLOAMU_FIFO_STS);
	result.ploamd_fifo_status =
		ioread32(ctx->base + GPON_G_PLOAMD_FIFO_STS);
	result.activation_status =
		ioread32(ctx->base + GPON_G_ACTIVATION_ST);

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

static int __init xr500v_gpon_mac_snapshot_init(void)
{
	struct xr500v_gpon_mac_snapshot_context ctx = {};
	struct resource *region;
	int err;

	if (!allow_snapshot) {
		pr_err("xr500v-gpon-mac-snapshot: refused: allow_snapshot=1 is required\n");
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

	region = request_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE,
				    KBUILD_MODNAME);
	if (!region)
		return -EBUSY;

	ctx.base = ioremap(GPON_MAC_PHYS, GPON_MAC_SIZE);
	if (!ctx.base) {
		err = -ENOMEM;
		goto release_region;
	}

	err = stop_machine(xr500v_gpon_mac_snapshot_stopped, &ctx, NULL);
	iounmap(ctx.base);
	release_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE);

	if (ctx.restore_err) {
		pr_emerg("xr500v-gpon-mac-snapshot: failed to restore ATM mode: %d\n",
			 ctx.restore_err);
		return ctx.restore_err;
	}
	if (ctx.final_read_err)
		return ctx.final_read_err;
	if (ctx.update_attempted && result.wan_after != result.wan_before) {
		pr_emerg("xr500v-gpon-mac-snapshot: ATM restore mismatch: 0x%08x\n",
			 result.wan_after);
		return -EIO;
	}
	if (err)
		return err;

	result.debugfs_dir = debugfs_create_dir("xr500v-gpon-mac-snapshot",
						NULL);
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

	pr_info("xr500v-gpon-mac-snapshot: six passive reads completed; exact ATM mux restored; 0 GPON writes\n");
	return 0;

remove_debugfs:
	debugfs_remove_recursive(result.debugfs_dir);
	return err;

release_region:
	release_mem_region(GPON_MAC_PHYS, GPON_MAC_SIZE);
	return err;
}

static void __exit xr500v_gpon_mac_snapshot_exit(void)
{
	debugfs_remove_recursive(result.debugfs_dir);
	pr_info("xr500v-gpon-mac-snapshot: unloaded\n");
}

module_init(xr500v_gpon_mac_snapshot_init);
module_exit(xr500v_gpon_mac_snapshot_exit);

MODULE_DESCRIPTION("Mux-guarded passive EN751221 GPON MAC snapshot for XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
