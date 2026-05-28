// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet/TrendChip EN751221 PCM/TDM controller driver.
 *
 * Phase 1: probe, map the register block, verify the SoC, soft-reset the PCM
 * engine, load the reset-default register values and keep all interrupts
 * masked. TX/RX descriptor rings, the char/ALSA interface and the SLIC glue
 * are added in later phases.
 *
 * Register sequences were reconstructed from the OEM Trendchip 2.6.36 source
 * (pcmdriver.h) and disassembly of the stock pcm1.ko.
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>

#include "pcm-en751221.h"

struct pcm_dev {
	struct device *dev;
	void __iomem *base;	/* PCM register block (DT reg) */
	void __iomem *scu;	/* SoC SCU @ 0x1fb00000 (chip-id + reset) */
	int irq;
	struct dentry *dbg;
};

static inline u32 pcm_rd(struct pcm_dev *p, enum pcm_reg r)
{
	return readl(p->base + PCM_REG(r));
}

static inline void pcm_wr(struct pcm_dev *p, enum pcm_reg r, u32 v)
{
	writel(v, p->base + PCM_REG(r));
}

/* Reset-default register values (OEM enum pcm_reg_num comments). */
static const u32 pcm_reg_defaults[PCM_REG_COUNT] = {
	[PCM_INTFACE_CTRL]		= 0x0100040a,
	[PCM_TX_TIME_SLOT_CFG0]		= 0x00080000,
	[PCM_TX_TIME_SLOT_CFG1]		= 0x00180010,
	[PCM_TX_TIME_SLOT_CFG2]		= 0x00280020,
	[PCM_TX_TIME_SLOT_CFG3]		= 0x00380030,
	[PCM_RX_TIME_SLOT_CFG0]		= 0x00080000,
	[PCM_RX_TIME_SLOT_CFG1]		= 0x00180010,
	[PCM_RX_TIME_SLOT_CFG2]		= 0x00280020,
	[PCM_RX_TIME_SLOT_CFG3]		= 0x00380030,
	[PCM_TX_RX_DESC_RING_SIZE_OFFSET] = 0x000000c0,
	[PCM_TX_RX_DMA_CTRL]		= 0x0f000000,
	/* ISR/IMR/polling/desc-ring-base default to 0 */
};

/*
 * Soft-reset the PCM0 engine via the SCU. Reconstructed from softReset() in
 * pcm1.ko: toggle bit 0x800 of SCU+0x834 around a short delay.
 */
static void pcm_soft_reset(struct pcm_dev *p)
{
	u32 v = readl(p->scu + SCU_PCM_RESET);

	writel(v | SCU_PCM0_RESET_BIT, p->scu + SCU_PCM_RESET);
	usleep_range(1000, 2000);
	writel(v & ~SCU_PCM0_RESET_BIT, p->scu + SCU_PCM_RESET);
	usleep_range(1000, 2000);
}

static void pcm_load_defaults(struct pcm_dev *p)
{
	int i;

	for (i = 0; i < PCM_REG_COUNT; i++)
		pcm_wr(p, i, pcm_reg_defaults[i]);

	/* keep all sources masked until the descriptor rings exist (phase 2) */
	pcm_wr(p, PCM_IMR, 0);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));	/* W1C any pending */
}

static irqreturn_t pcm_irq(int irq, void *data)
{
	struct pcm_dev *p = data;
	u32 isr = pcm_rd(p, PCM_ISR);

	if (!isr)
		return IRQ_NONE;

	pcm_wr(p, PCM_ISR, isr);		/* acknowledge */
	dev_dbg(p->dev, "PCM IRQ, ISR=0x%08x\n", isr);
	return IRQ_HANDLED;
}

static int pcm_regs_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	int i;

	for (i = 0; i < PCM_REG_COUNT; i++)
		seq_printf(s, "reg[%2d] @0x%02x = 0x%08x\n",
			   i, PCM_REG(i), pcm_rd(p, i));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_regs);

static int pcm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pcm_dev *p;
	u32 chip;

	p = devm_kzalloc(dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	p->dev = dev;

	p->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(p->base))
		return PTR_ERR(p->base);

	p->scu = devm_ioremap(dev, SCU_PHYS_BASE, 0x970);
	if (!p->scu)
		return -ENOMEM;

	chip = readl(p->scu + SCU_CHIP_ID) & SCU_CHIP_ID_MASK;
	if (chip != SCU_CHIP_EN751221 && chip != SCU_CHIP_EN7526C)
		dev_warn(dev, "unexpected chip id 0x%08x (continuing)\n", chip);
	else
		dev_info(dev, "EN751221-class SoC (chip id 0x%08x)\n", chip);

	pcm_soft_reset(p);
	pcm_load_defaults(p);

	/* IRQ is optional in phase 1: a wrong DT number must not block bring-up */
	p->irq = platform_get_irq_optional(pdev, 0);
	if (p->irq > 0) {
		int ret = devm_request_irq(dev, p->irq, pcm_irq, 0,
					   dev_name(dev), p);
		if (ret)
			dev_warn(dev, "could not request IRQ %d: %d\n",
				 p->irq, ret);
	}

	p->dbg = debugfs_create_dir("pcm-en751221", NULL);
	debugfs_create_file("regs", 0444, p->dbg, p, &pcm_regs_fops);

	platform_set_drvdata(pdev, p);
	dev_info(dev, "PCM/TDM controller ready (phase 1: reset + defaults)\n");
	return 0;
}

static void pcm_remove(struct platform_device *pdev)
{
	struct pcm_dev *p = platform_get_drvdata(pdev);

	debugfs_remove_recursive(p->dbg);
	pcm_wr(p, PCM_IMR, 0);
}

static const struct of_device_id pcm_of_match[] = {
	{ .compatible = "econet,en751221-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, pcm_of_match);

static struct platform_driver pcm_driver = {
	.probe = pcm_probe,
	.remove = pcm_remove,
	.driver = {
		.name = "pcm-en751221",
		.of_match_table = pcm_of_match,
	},
};
module_platform_driver(pcm_driver);

MODULE_DESCRIPTION("EcoNet/TrendChip EN751221 PCM/TDM controller");
MODULE_LICENSE("GPL");
