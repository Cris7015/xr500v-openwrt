// SPDX-License-Identifier: GPL-2.0-only
/*
 * TP-Link Archer XR500v / EcoNet EN751221 xPON PHY diagnostics.
 *
 * This deliberately small module is a read-only bring-up aid.  It maps the
 * XPON PHY CSR window used by the OEM driver and exposes decoded state through
 * debugfs.  There are no write paths: no reset, mode, interrupt, laser, APD,
 * calibration, or counter registers are modified.
 */

#include <linux/bitfield.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#define XR500V_XPON_PHY_BASE_DEFAULT	0x1faf0000
#define XR500V_XPON_PHY_SIZE		0x1000

#define PHYSET2		0x0104
#define   PHYSET2_PHYRDY		BIT(2)
#define   PHYSET2_FWRDY		BIT(0)

#define PHYSET3		0x0108
#define   PHYSET3_PLL_EN		BIT(11)
#define   PHYSET3_TXEN		BIT(5)
#define   PHYSET3_XP_PHYRST	BIT(28)
#define   PHYSET3_XP_SRST	BIT(27)

#define PHYSET10	0x0124
#define   PHYSET10_GPON_MODE	BIT(31)

#define PHYSTA1		0x0130
#define   PHYSTA1_PHY_CURR	GENMASK(20, 18)
#define   PHYSTA1_PHYRDY_STATE	0x6

#define XPON_SETTING	0x0138

#define ANASTA1		0x013c
#define   ANASTA1_RXPLLLOCK	BIT(13)
#define   ANASTA1_TXPLLLOCK	BIT(12)
#define   ANASTA1_IMPCAL_DONE	BIT(9)
#define   ANASTA1_RX_VCOCAL_CPLT BIT(4)
#define   ANASTA1_RXIMP_DONE	BIT(1)
#define   ANASTA1_TXIMP_DONE	BIT(0)

#define ANACAL1		0x0140
#define ANACAL2		0x0144

#define PHYRX_STATUS	0x021c
#define   PHYRX_FEC_STATUS	GENMASK(15, 8)
#define   PHYRX_SYNC_STATUS	GENMASK(7, 0)
#define   PHYRX_SYNC_VALUE	0x0a

#define XP_ERRCNT_EN	0x0230
#define XP_ERRCNT_CTL	0x0234
#define ERR_BYTE_CNT	0x0238
#define ERR_CODE_CNT	0x023c
#define NOSOL_CODE_CNT	0x0240
#define RX_CODE_CNT	0x0244

#define XPON_STA	0x05e0
#define   XPON_STA_LOS	BIT(0)
#define XPON_INT_EN	0x05f0
#define XPON_INT_STA	0x05f8
#define   XPON_INT_PHYRDY	BIT(5)
#define   XPON_INT_TX_SD_FAIL	BIT(4)
#define   XPON_INT_TX_FAULT	BIT(2)
#define   XPON_INT_LOF		BIT(1)
#define   XPON_INT_TRANS_LOS	BIT(0)

struct xr500v_gpon_diag {
	void __iomem *base;
	struct dentry *debugfs_dir;
};

static struct xr500v_gpon_diag diag;
static unsigned long phy_base = XR500V_XPON_PHY_BASE_DEFAULT;
module_param(phy_base, ulong, 0444);
MODULE_PARM_DESC(phy_base, "physical base of the EN751221 xPON PHY CSR block");

static u32 xpon_read(u32 reg)
{
	return ioread32(diag.base + reg);
}

struct xpon_reg_desc {
	const char *name;
	u16 offset;
};

static const struct xpon_reg_desc xpon_regs[] = {
	{ "PHYSET2",       PHYSET2 },
	{ "PHYSET3",       PHYSET3 },
	{ "PHYSET10",      PHYSET10 },
	{ "PHYSTA1",       PHYSTA1 },
	{ "XPON_SETTING",  XPON_SETTING },
	{ "ANASTA1",       ANASTA1 },
	{ "ANACAL1",       ANACAL1 },
	{ "ANACAL2",       ANACAL2 },
	{ "PHYRX_STATUS",  PHYRX_STATUS },
	{ "XP_ERRCNT_EN",  XP_ERRCNT_EN },
	{ "XP_ERRCNT_CTL", XP_ERRCNT_CTL },
	{ "ERR_BYTE_CNT",  ERR_BYTE_CNT },
	{ "ERR_CODE_CNT",  ERR_CODE_CNT },
	{ "NOSOL_CODE_CNT", NOSOL_CODE_CNT },
	{ "RX_CODE_CNT",   RX_CODE_CNT },
	{ "XPON_STA",      XPON_STA },
	{ "XPON_INT_EN",   XPON_INT_EN },
	{ "XPON_INT_STA",  XPON_INT_STA },
};

static int regs_show(struct seq_file *s, void *unused)
{
	unsigned int i;

	seq_printf(s, "physical_base: 0x%08lx\n", phy_base);
	for (i = 0; i < ARRAY_SIZE(xpon_regs); i++)
		seq_printf(s, "0x%04x %-16s 0x%08x\n",
			   xpon_regs[i].offset, xpon_regs[i].name,
			   xpon_read(xpon_regs[i].offset));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int status_show(struct seq_file *s, void *unused)
{
	u32 set2 = xpon_read(PHYSET2);
	u32 set3 = xpon_read(PHYSET3);
	u32 set10 = xpon_read(PHYSET10);
	u32 physta1 = xpon_read(PHYSTA1);
	u32 anasta1 = xpon_read(ANASTA1);
	u32 rx = xpon_read(PHYRX_STATUS);
	u32 sta = xpon_read(XPON_STA);
	u32 ints = xpon_read(XPON_INT_STA);
	u32 state = FIELD_GET(PHYSTA1_PHY_CURR, physta1);
	u32 sync = FIELD_GET(PHYRX_SYNC_STATUS, rx);

	seq_printf(s, "mode:                 %s\n",
		   set10 & PHYSET10_GPON_MODE ? "GPON" : "EPON/unknown");
	seq_printf(s, "firmware_ready:       %s\n",
		   set2 & PHYSET2_FWRDY ? "yes" : "no");
	seq_printf(s, "phy_ready_bit:        %s\n",
		   set2 & PHYSET2_PHYRDY ? "yes" : "no");
	seq_printf(s, "phy_fsm_state:        0x%x%s\n", state,
		   state == PHYSTA1_PHYRDY_STATE ? " (ready)" : "");
	seq_printf(s, "rx_sync:              %s (raw 0x%02x)\n",
		   sync == PHYRX_SYNC_VALUE ? "yes" : "no", sync);
	seq_printf(s, "rx_fec_status:        0x%02x\n",
		   (u32)FIELD_GET(PHYRX_FEC_STATUS, rx));
	seq_printf(s, "loss_of_signal:       %s\n",
		   sta & XPON_STA_LOS ? "yes" : "no");
	seq_printf(s, "tx_enable:            %s\n",
		   set3 & PHYSET3_TXEN ? "YES" : "no");
	seq_printf(s, "pll_enable:           %s\n",
		   set3 & PHYSET3_PLL_EN ? "yes" : "no");
	seq_printf(s, "phy_reset_asserted:   %s\n",
		   set3 & PHYSET3_XP_PHYRST ? "yes" : "no");
	seq_printf(s, "soft_reset_asserted:  %s\n",
		   set3 & PHYSET3_XP_SRST ? "yes" : "no");
	seq_printf(s, "rx_pll_lock:          %s\n",
		   anasta1 & ANASTA1_RXPLLLOCK ? "yes" : "no");
	seq_printf(s, "tx_pll_lock:          %s\n",
		   anasta1 & ANASTA1_TXPLLLOCK ? "yes" : "no");
	seq_printf(s, "impedance_cal_done:   %s\n",
		   anasta1 & ANASTA1_IMPCAL_DONE ? "yes" : "no");
	seq_printf(s, "rx_vco_cal_done:      %s\n",
		   anasta1 & ANASTA1_RX_VCOCAL_CPLT ? "yes" : "no");
	seq_printf(s, "rx_impedance_done:    %s\n",
		   anasta1 & ANASTA1_RXIMP_DONE ? "yes" : "no");
	seq_printf(s, "tx_impedance_done:    %s\n",
		   anasta1 & ANASTA1_TXIMP_DONE ? "yes" : "no");
	seq_printf(s, "interrupt_enable:     0x%02x\n",
		   xpon_read(XPON_INT_EN) & 0xff);
	seq_printf(s, "interrupt_status:     0x%02x\n", ints & 0xff);
	seq_printf(s, "irq_phy_ready:        %s\n",
		   ints & XPON_INT_PHYRDY ? "pending" : "no");
	seq_printf(s, "irq_tx_sd_fail:       %s\n",
		   ints & XPON_INT_TX_SD_FAIL ? "pending" : "no");
	seq_printf(s, "irq_tx_fault:         %s\n",
		   ints & XPON_INT_TX_FAULT ? "pending" : "no");
	seq_printf(s, "irq_loss_of_frame:    %s\n",
		   ints & XPON_INT_LOF ? "pending" : "no");
	seq_printf(s, "irq_transceiver_los:  %s\n",
		   ints & XPON_INT_TRANS_LOS ? "pending" : "no");
	seq_puts(s, "writes_performed:     0\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int __init xr500v_gpon_diag_init(void)
{
	diag.base = ioremap(phy_base, XR500V_XPON_PHY_SIZE);
	if (!diag.base)
		return -ENOMEM;

	diag.debugfs_dir = debugfs_create_dir("xr500v-gpon", NULL);
	if (IS_ERR(diag.debugfs_dir)) {
		iounmap(diag.base);
		return PTR_ERR(diag.debugfs_dir);
	}

	debugfs_create_file("status", 0444, diag.debugfs_dir, NULL,
			    &status_fops);
	debugfs_create_file("regs", 0444, diag.debugfs_dir, NULL,
			    &regs_fops);

	pr_info("xr500v-gpon-diag: read-only xPON PHY diagnostics at 0x%08lx\n",
		phy_base);
	return 0;
}

static void __exit xr500v_gpon_diag_exit(void)
{
	debugfs_remove_recursive(diag.debugfs_dir);
	iounmap(diag.base);
	pr_info("xr500v-gpon-diag: unloaded\n");
}

module_init(xr500v_gpon_diag_init);
module_exit(xr500v_gpon_diag_exit);

MODULE_DESCRIPTION("Read-only EN751221 xPON PHY diagnostics for Archer XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
