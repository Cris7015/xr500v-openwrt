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
#include <linux/gpio/consumer.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>

#define PHYSET2		0x0104
#define   PHYSET2_PHYRDY		BIT(2)
#define   PHYSET2_FWRDY		BIT(0)

#define PHYSET3		0x0108
#define   PHYSET3_PLL_EN		BIT(11)
#define   PHYSET3_TXEN		BIT(5)
#define   PHYSET3_XP_PHYRST	BIT(28)
#define   PHYSET3_XP_SRST	BIT(27)

#define PHYSET5		0x0110
#define   PHYSET5_TXBDELAY_SEL	BIT(23)
#define   PHYSET5_TXBIT_DLY_SEL	GENMASK(22, 19)

#define PHYSET10	0x0124
#define   PHYSET10_GPON_MODE	BIT(31)

#define PHYSTA1		0x0130
#define   PHYSTA1_PHY_CURR	GENMASK(20, 18)
#define   PHYSTA1_PHYRDY_STATE	0x6

#define XPON_SETTING	0x0138
#define   XPON_SETTING_BURST_INV	BIT(7)
#define   XPON_SETTING_RX_SD_INV	BIT(6)
#define   XPON_SETTING_TX_FAULT_INV BIT(5)
#define   XPON_SETTING_TX_SD_INV	BIT(4)

#define ANASTA1		0x013c
#define   ANASTA1_RXPLLLOCK	BIT(13)
#define   ANASTA1_TXPLLLOCK	BIT(12)
#define   ANASTA1_IMPCAL_DONE	BIT(9)
#define   ANASTA1_RX_VCOCAL_CPLT BIT(4)
#define   ANASTA1_RXIMP_DONE	BIT(1)
#define   ANASTA1_TXIMP_DONE	BIT(0)

#define ANACAL1		0x0140
#define ANACAL2		0x0144

#define ANAPWD		0x0150
#define   ANAPWD_TX_PD_EN	BIT(11)
#define   ANAPWD_TX_PD		BIT(4)
#define   ANAPWD_TXDRVEN_DIS	BIT(3)
#define   ANAPWD_TX_SER_PWD	BIT(2)
#define   ANAPWD_TX_BENSER_PWD	BIT(1)

#define TDCSTA1		0x01f4

#define MISC		0x01fc
#define   MISC_ROGUE_ONU_TX_TEST_MODE BIT(28)
#define   MISC_TX_MODE_SW_SEL	BIT(27)

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

#define PHYTX_STATUS	0x040c
#define TX_FRAME_COUNTER 0x0434
#define TX_BURST_COUNTER 0x0438
#define BISTCTL_PRBS_TX_EN 0x04a4
#define TEST_FRAME_EN	0x0510

#define XPON_STA	0x05e0
#define   XPON_STA_LOS	BIT(0)
#define GIO1_SETTING	0x05e8
#define GIO2_SETTING	0x05ec
#define XPON_INT_EN	0x05f0
#define XPON_INT_STA	0x05f8
#define   XPON_INT_PHYRDY	BIT(5)
#define   XPON_INT_TX_SD_FAIL	BIT(4)
#define   XPON_INT_TX_FAULT	BIT(2)
#define   XPON_INT_LOF		BIT(1)
#define   XPON_INT_TRANS_LOS	BIT(0)

struct xr500v_gpon_diag {
	struct device *dev;
	void __iomem *base;
	resource_size_t phys_base;
	struct gpio_desc *tx_disable_gpio;
	struct dentry *debugfs_dir;
};

static u32 xpon_read(struct xr500v_gpon_diag *diag, u32 reg)
{
	return ioread32(diag->base + reg);
}

struct xpon_reg_desc {
	const char *name;
	u16 offset;
};

static const struct xpon_reg_desc xpon_regs[] = {
	{ "PHYSET2",       PHYSET2 },
	{ "PHYSET3",       PHYSET3 },
	{ "PHYSET5",       PHYSET5 },
	{ "PHYSET10",      PHYSET10 },
	{ "PHYSTA1",       PHYSTA1 },
	{ "XPON_SETTING",  XPON_SETTING },
	{ "ANASTA1",       ANASTA1 },
	{ "ANACAL1",       ANACAL1 },
	{ "ANACAL2",       ANACAL2 },
	{ "ANAPWD",        ANAPWD },
	{ "TDCSTA1",       TDCSTA1 },
	{ "MISC",          MISC },
	{ "PHYRX_STATUS",  PHYRX_STATUS },
	{ "XP_ERRCNT_EN",  XP_ERRCNT_EN },
	{ "XP_ERRCNT_CTL", XP_ERRCNT_CTL },
	{ "ERR_BYTE_CNT",  ERR_BYTE_CNT },
	{ "ERR_CODE_CNT",  ERR_CODE_CNT },
	{ "NOSOL_CODE_CNT", NOSOL_CODE_CNT },
	{ "RX_CODE_CNT",   RX_CODE_CNT },
	{ "PHYTX_STATUS",  PHYTX_STATUS },
	{ "TX_FRAME_COUNTER", TX_FRAME_COUNTER },
	{ "TX_BURST_COUNTER", TX_BURST_COUNTER },
	{ "BISTCTL_PRBS_TX_EN", BISTCTL_PRBS_TX_EN },
	{ "TEST_FRAME_EN", TEST_FRAME_EN },
	{ "XPON_STA",      XPON_STA },
	{ "GIO1_SETTING",  GIO1_SETTING },
	{ "GIO2_SETTING",  GIO2_SETTING },
	{ "XPON_INT_EN",   XPON_INT_EN },
	{ "XPON_INT_STA",  XPON_INT_STA },
};

static int regs_show(struct seq_file *s, void *unused)
{
	struct xr500v_gpon_diag *diag = s->private;
	unsigned int i;

	seq_printf(s, "physical_base: 0x%08llx\n",
		   (unsigned long long)diag->phys_base);
	for (i = 0; i < ARRAY_SIZE(xpon_regs); i++)
		seq_printf(s, "0x%04x %-16s 0x%08x\n",
			   xpon_regs[i].offset, xpon_regs[i].name,
			   xpon_read(diag, xpon_regs[i].offset));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(regs);

static int status_show(struct seq_file *s, void *unused)
{
	struct xr500v_gpon_diag *diag = s->private;
	u32 set2 = xpon_read(diag, PHYSET2);
	u32 set3 = xpon_read(diag, PHYSET3);
	u32 set5 = xpon_read(diag, PHYSET5);
	u32 set10 = xpon_read(diag, PHYSET10);
	u32 physta1 = xpon_read(diag, PHYSTA1);
	u32 xpon_setting = xpon_read(diag, XPON_SETTING);
	u32 anasta1 = xpon_read(diag, ANASTA1);
	u32 anapwd = xpon_read(diag, ANAPWD);
	u32 misc = xpon_read(diag, MISC);
	u32 rx = xpon_read(diag, PHYRX_STATUS);
	u32 sta = xpon_read(diag, XPON_STA);
	u32 ints = xpon_read(diag, XPON_INT_STA);
	u32 prbs = xpon_read(diag, BISTCTL_PRBS_TX_EN);
	u32 test_frame = xpon_read(diag, TEST_FRAME_EN);
	u32 state = FIELD_GET(PHYSTA1_PHY_CURR, physta1);
	u32 sync = FIELD_GET(PHYRX_SYNC_STATUS, rx);
	int tx_disable_dir;
	int tx_disable_raw;
	int tx_disable_asserted;

	if (diag->tx_disable_gpio) {
		tx_disable_dir = gpiod_get_direction(diag->tx_disable_gpio);
		tx_disable_raw = gpiod_get_raw_value_cansleep(diag->tx_disable_gpio);
		tx_disable_asserted =
			gpiod_get_value_cansleep(diag->tx_disable_gpio);
	} else {
		tx_disable_dir = -ENOENT;
		tx_disable_raw = -ENOENT;
		tx_disable_asserted = -ENOENT;
	}

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
	seq_printf(s, "xpon_setting:         0x%08x\n", xpon_setting);
	seq_printf(s, "burst_enable_inverted: %s\n",
		   xpon_setting & XPON_SETTING_BURST_INV ? "yes" : "no");
	seq_printf(s, "rx_sd_inverted:       %s\n",
		   xpon_setting & XPON_SETTING_RX_SD_INV ? "yes" : "no");
	seq_printf(s, "tx_fault_inverted:    %s\n",
		   xpon_setting & XPON_SETTING_TX_FAULT_INV ? "yes" : "no");
	seq_printf(s, "tx_sd_inverted:       %s\n",
		   xpon_setting & XPON_SETTING_TX_SD_INV ? "yes" : "no");
	seq_printf(s, "tx_enable:            %s\n",
		   set3 & PHYSET3_TXEN ? "YES" : "no");
	if (tx_disable_dir < 0) {
		seq_printf(s, "tx_disable_gpio:      unavailable (%d)\n",
			   tx_disable_dir);
	} else {
		seq_printf(s, "tx_disable_direction: %s\n",
			   tx_disable_dir ? "input" : "output");
		seq_printf(s, "tx_disable_raw:       %s\n",
			   tx_disable_raw > 0 ? "high" :
			   tx_disable_raw == 0 ? "low" : "read-error");
		seq_printf(s, "tx_disable_asserted:  %s\n",
			   tx_disable_asserted > 0 ? "yes" :
			   tx_disable_asserted == 0 ? "NO" : "read-error");
	}
	seq_printf(s, "tx_powerdown_enable:  %s\n",
		   anapwd & ANAPWD_TX_PD_EN ? "yes" : "no");
	seq_printf(s, "tx_powered_down:      %s\n",
		   anapwd & ANAPWD_TX_PD ? "yes" : "no");
	seq_printf(s, "tx_driver_disabled:  %s\n",
		   anapwd & ANAPWD_TXDRVEN_DIS ? "yes" : "no");
	seq_printf(s, "tx_serializer_pwd:   %s\n",
		   anapwd & ANAPWD_TX_SER_PWD ? "yes" : "no");
	seq_printf(s, "tx_burst_ser_pwd:    %s\n",
		   anapwd & ANAPWD_TX_BENSER_PWD ? "yes" : "no");
	seq_printf(s, "rogue_onu_test_mode: %s\n",
		   misc & MISC_ROGUE_ONU_TX_TEST_MODE ? "YES" : "no");
	seq_printf(s, "tx_software_mode:    %s\n",
		   misc & MISC_TX_MODE_SW_SEL ? "yes" : "no");
	seq_printf(s, "prbs_tx_enable_raw:  0x%08x\n", prbs);
	seq_printf(s, "test_frame_enable:   0x%08x\n", test_frame);
	seq_printf(s, "tx_bit_delay:        0x%x (manual=%s)\n",
		   (u32)FIELD_GET(PHYSET5_TXBIT_DLY_SEL, set5),
		   set5 & PHYSET5_TXBDELAY_SEL ? "yes" : "no");
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
		   xpon_read(diag, XPON_INT_EN) & 0xff);
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
	seq_puts(s, "mmio_writes_performed: 0\n");
	seq_puts(s, "counter_latch_or_clear: no\n");
	seq_puts(s, "reset_or_mode_change:  no\n");
	seq_puts(s, "polling_or_irq_handler: no\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(status);

static int xr500v_gpon_diag_probe(struct platform_device *pdev)
{
	struct xr500v_gpon_diag *diag;
	struct resource *res;

	diag = devm_kzalloc(&pdev->dev, sizeof(*diag), GFP_KERNEL);
	if (!diag)
		return -ENOMEM;

	diag->dev = &pdev->dev;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	diag->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(diag->base))
		return PTR_ERR(diag->base);
	diag->phys_base = res->start;
	diag->tx_disable_gpio = devm_gpiod_get_optional(&pdev->dev,
							"tx-disable", GPIOD_ASIS);
	if (IS_ERR(diag->tx_disable_gpio))
		return dev_err_probe(&pdev->dev, PTR_ERR(diag->tx_disable_gpio),
				     "cannot reserve TX_DISABLE GPIO as-is\n");
	platform_set_drvdata(pdev, diag);

	diag->debugfs_dir = debugfs_create_dir("xr500v-gpon", NULL);
	if (IS_ERR(diag->debugfs_dir))
		return PTR_ERR(diag->debugfs_dir);

	debugfs_create_file("status", 0444, diag->debugfs_dir, diag,
			    &status_fops);
	debugfs_create_file("regs", 0444, diag->debugfs_dir, diag,
			    &regs_fops);

	dev_info(&pdev->dev,
		 "read-only xPON PHY diagnostics at %pa; no MMIO writes\n",
		 &diag->phys_base);
	return 0;
}

static void xr500v_gpon_diag_remove(struct platform_device *pdev)
{
	struct xr500v_gpon_diag *diag = platform_get_drvdata(pdev);

	debugfs_remove_recursive(diag->debugfs_dir);
}

static const struct of_device_id xr500v_gpon_diag_of_match[] = {
	{ .compatible = "econet,en751221-xpon-phy-diag" },
	{ }
};
MODULE_DEVICE_TABLE(of, xr500v_gpon_diag_of_match);

static struct platform_driver xr500v_gpon_diag_driver = {
	.probe = xr500v_gpon_diag_probe,
	.remove = xr500v_gpon_diag_remove,
	.driver = {
		.name = "xr500v-gpon-diag",
		.of_match_table = xr500v_gpon_diag_of_match,
	},
};
module_platform_driver(xr500v_gpon_diag_driver);

MODULE_DESCRIPTION("Read-only EN751221 xPON PHY diagnostics for Archer XR500v");
MODULE_AUTHOR("Cris7015 XR500v OpenWrt project");
MODULE_LICENSE("GPL");
