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
#include <linux/bitfield.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>

#include "pcm-en751221.h"

struct pcm_dev {
	struct device *dev;
	void __iomem *base;	/* PCM register block (DT reg) */
	void __iomem *scu;	/* SoC SCU @ 0x1fb00000 (chip-id + reset) */
	void __iomem *chip_scu;	/* Chip SCU @ 0x1fa20000 (PCM pinmux/clock) */
	int irq;
	struct dentry *dbg;
	struct mutex selftest_lock;

	/* DMA descriptor rings (phase 2): PCM_DESC_NUM x struct pcm_desc each */
	struct pcm_desc *tx_ring;
	struct pcm_desc *rx_ring;
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
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

/*
 * Minimum PCM clock/pinmux sequence from pcm1.ko init_module():
 *   0x434..0x440: CHIP_SCU+0x104 |= 0x2000
 *   0x444..0x44c: CHIP_SCU+0x0d8 |= 0x1
 *   0x450..0x458: CHIP_SCU+0x148 |= 0x0c
 *   0x474..0x480: non-EN7526C additionally writes 0x8 to +0x0d4 and
 *                  0x00a00301 to +0x0d8.
 *
 * BACK_TO_BACK is internal to INTFACE_CTRL bit 25, so external SLIC pads are
 * not needed for data return. The OEM still enables the PCM/ZSI mux and clock
 * before pcmConfig(); keep that clock sequence for real hardware safety.
 */
static void pcm_enable_clock_iomux(struct pcm_dev *p)
{
	u32 chip = readl(p->scu + SCU_CHIP_ID) & SCU_CHIP_ID_MASK;
	u32 v;

	v = readl(p->chip_scu + CHIP_SCU_IOMUX_CONTROL1);
	writel(v | CHIP_SCU_GPIO_ZSI_ISI, p->chip_scu + CHIP_SCU_IOMUX_CONTROL1);

	v = readl(p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);
	writel(v | CHIP_SCU_PCM_CLK_OUT_EN, p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);

	v = readl(p->chip_scu + CHIP_SCU_PCM_CLK_SRC_SEL);
	writel(v | FIELD_PREP(CHIP_SCU_PCM_ZSI_CLK_SRC,
			      CHIP_SCU_PCM_ZSI_CLK_SRC_VAL),
	       p->chip_scu + CHIP_SCU_PCM_CLK_SRC_SEL);

	if (chip != SCU_CHIP_EN7526C) {
		writel(0x00000008, p->chip_scu + CHIP_SCU_PCM_CLK_DIV);
		writel(0x00a00301, p->chip_scu + CHIP_SCU_PCM_CLK_OUTPUT);
	}
}

static u32 pcm_selftest_iface_ctrl(struct pcm_dev *p)
{
	(void)p;

	return 0x87071306;
}

static void pcm_selftest_fill_desc(struct pcm_desc *d, dma_addr_t dma,
				   u32 ch_valid, u32 samples)
{
	u32 offset = 0;
	int ch;

	memset(d, 0, sizeof(*d));
	for (ch = 0; ch < PCM_SELFTEST_CHANS; ch++) {
		d->buf_addr[ch] = (u32)((dma + offset) & PCM_DMA_ADDR_MASK);
		offset += samples * (ch_valid & BIT(ch) ? 2 : 1);
	}

	dma_wmb();
	d->status = PCM_DESC_OWN |
		    FIELD_PREP(PCM_DESC_CH_VALID, ch_valid) |
		    FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
}

/*
 * Minimal polled BACK_TO_BACK self-test.
 *
 * Descriptor ownership follows the OEM flow:
 *   SW sets status bit31=1 before enabling DMA (rxDescSet() 0x12e4,
 *   pcmSend() 0x7c0). HW clears bit31 when the descriptor is complete; ISR
 *   checks cleared ownership with bgez/bltz in pcm_lb.ko .text.pcmIsr 0x98
 *   and 0x1c0.
 */
static int pcm_loopback_selftest(struct pcm_dev *p)
{
	const size_t buf_sz = PCM_SELFTEST_BUF_BYTES;
	static const u32 slot_cfg[4] = {
		0x10101000, 0x10301020, 0x00480040, 0x00580050,
	};
	dma_addr_t tx_dma, rx_dma;
	void *tx_buf, *rx_buf;
	u32 old_imr, old_dma, old_iface, old_ring_size, dma_ctrl, iface;
	u32 old_tx_slots[4], old_rx_slots[4];
	u32 *tx_words, *rx_words;
	int i, ret = 0;

	mutex_lock(&p->selftest_lock);

	tx_buf = dma_alloc_coherent(p->dev, buf_sz, &tx_dma, GFP_KERNEL);
	if (!tx_buf) {
		ret = -ENOMEM;
		goto out_unlock;
	}

	rx_buf = dma_alloc_coherent(p->dev, buf_sz, &rx_dma, GFP_KERNEL);
	if (!rx_buf) {
		ret = -ENOMEM;
		goto out_free_tx;
	}

	if ((tx_dma | rx_dma) & ~PCM_DMA_ADDR_MASK) {
		ret = -EOVERFLOW;
		goto out_free_rx;
	}

	old_imr = pcm_rd(p, PCM_IMR);
	old_dma = pcm_rd(p, PCM_TX_RX_DMA_CTRL);
	old_iface = pcm_rd(p, PCM_INTFACE_CTRL);
	old_ring_size = pcm_rd(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET);
	for (i = 0; i < 4; i++) {
		old_tx_slots[i] = pcm_rd(p, PCM_TX_TIME_SLOT_CFG0 + i);
		old_rx_slots[i] = pcm_rd(p, PCM_RX_TIME_SLOT_CFG0 + i);
	}

	pcm_wr(p, PCM_IMR, 0);
	dma_ctrl = old_dma & ~PCM_DMA_RX_EN;
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);
	dma_ctrl &= ~PCM_DMA_TX_EN;
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));

	memset(p->tx_ring, 0, PCM_DESC_NUM * sizeof(*p->tx_ring));
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(*p->rx_ring));
	memset(rx_buf, 0, buf_sz);

	tx_words = tx_buf;
	for (i = 0; i < buf_sz / sizeof(u32); i++)
		tx_words[i] = PCM_LOOPBACK_HEADER_PATTERN;

	pcm_enable_clock_iomux(p);
	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x0000009f);
	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, slot_cfg[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, slot_cfg[i]);
	}

	iface = pcm_selftest_iface_ctrl(p);
	pcm_wr(p, PCM_INTFACE_CTRL, iface);
	pcm_wr(p, PCM_INTFACE_CTRL, iface & ~BIT(26));
	usleep_range(5000, 6000);
	pcm_wr(p, PCM_INTFACE_CTRL, iface);
	pcm_wr(p, PCM_IMR, 0x00000028);

	dma_ctrl = old_dma & ~(PCM_DMA_CH_VALID_MASK | PCM_DMA_TX_EN |
			       PCM_DMA_RX_EN);
	dma_ctrl |= FIELD_PREP(PCM_DMA_CH_VALID_MASK, PCM_SELFTEST_CH_VALID);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl);

	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, dma_ctrl | PCM_DMA_TX_EN);
	pcm_selftest_fill_desc(&p->rx_ring[0], rx_dma, PCM_SELFTEST_CH_VALID,
			       PCM_SELFTEST_SAMPLES);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       dma_ctrl | PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
	pcm_selftest_fill_desc(&p->tx_ring[0], tx_dma, PCM_SELFTEST_CH_VALID,
			       PCM_SELFTEST_SAMPLES);
	dma_wmb();
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		if (!(READ_ONCE(p->rx_ring[0].status) & PCM_DESC_OWN))
			break;
		usleep_range(1000, 2000);
	}
	if (i == 500) {
		ret = -ETIMEDOUT;
		goto out_restore;
	}

	dma_rmb();
	rx_words = rx_buf;
	for (i = 0; i < buf_sz / sizeof(u32); i++) {
		if (rx_words[i] != PCM_LOOPBACK_HEADER_PATTERN) {
			ret = -EIO;
			break;
		}
	}

out_restore:
	dma_rmb();
	dev_info(p->dev,
		 "loopback diag: ret=%d isr=0x%08x dma_ctrl=0x%08x txdesc=0x%08x rxdesc=0x%08x rx[0]=0x%08x\n",
		 ret, pcm_rd(p, PCM_ISR), pcm_rd(p, PCM_TX_RX_DMA_CTRL),
		 p->tx_ring[0].status, p->rx_ring[0].status, ((u32 *)rx_buf)[0]);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL, old_dma & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	pcm_wr(p, PCM_INTFACE_CTRL, old_iface);
	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, old_tx_slots[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, old_rx_slots[i]);
	}
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, old_ring_size);
	pcm_wr(p, PCM_IMR, old_imr);
	pcm_wr(p, PCM_ISR, pcm_rd(p, PCM_ISR));
	memset(p->tx_ring, 0, PCM_DESC_NUM * sizeof(*p->tx_ring));
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(*p->rx_ring));

out_free_rx:
	dma_free_coherent(p->dev, buf_sz, rx_buf, rx_dma);
out_free_tx:
	dma_free_coherent(p->dev, buf_sz, tx_buf, tx_dma);
out_unlock:
	mutex_unlock(&p->selftest_lock);
	return ret;
}

/*
 * Allocate the TX/RX DMA descriptor rings and program their physical base
 * addresses. From descInit() in pcm1.ko: each ring is PCM_DESC_NUM entries of
 * sizeof(struct pcm_desc) (36 B -> 540 B), zeroed, base written (as a physical
 * address) to PCM_TX/RX_DESC_RING_BASE. The rings are not yet armed or the DMA
 * enabled here - that is phase 2b.
 */
static int pcm_alloc_rings(struct pcm_dev *p)
{
	size_t sz = PCM_DESC_NUM * sizeof(struct pcm_desc);

	p->tx_ring = dmam_alloc_coherent(p->dev, sz, &p->tx_dma, GFP_KERNEL);
	if (!p->tx_ring)
		return -ENOMEM;
	p->rx_ring = dmam_alloc_coherent(p->dev, sz, &p->rx_dma, GFP_KERNEL);
	if (!p->rx_ring)
		return -ENOMEM;

	memset(p->tx_ring, 0, sz);
	memset(p->rx_ring, 0, sz);

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)p->tx_dma);
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)p->rx_dma);

	dev_info(p->dev, "DMA rings: tx=%pad rx=%pad (%u desc x %zu B)\n",
		 &p->tx_dma, &p->rx_dma, PCM_DESC_NUM, sizeof(struct pcm_desc));
	return 0;
}

/*
 * Exported for the SLIC (ZSI) driver: bring the PCM engine up so it generates
 * continuous PCLK/FSYNC on the external pins, which a ZSI SLIC clocks off.
 *
 * The OEM stock device (voip running) shows the PCM with DMA TX+RX enabled
 * (DMA_CTRL=0x0f000003), INTFACE_CTRL=0xf5071306 and a specific timeslot table;
 * the bit clock only free-runs once DMA is armed. So we arm all descriptors
 * (with dummy buffers), program the caller's timeslots + INTFACE_CTRL, and
 * enable DMA. Left running (not torn down) so the SLIC keeps its clock.
 */
static struct pcm_dev *g_pcm;

int pcm_en751221_zsi_clock_run(u32 intface, const u32 *tx_slots,
			       const u32 *rx_slots)
{
	struct pcm_dev *p = g_pcm;
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	const u32 samples = 80, chv = 0xf;
	int i, ch;

	if (!p)
		return -ENODEV;

	if (!txb) {
		txb = dmam_alloc_coherent(p->dev, 1024, &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, 1024, &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}

	for (i = 0; i < PCM_DESC_NUM; i++) {
		memset(&p->tx_ring[i], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[i], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 4; ch++) {
			p->tx_ring[i].buf_addr[ch] =
				(u32)((txb_dma + ch * 160) & PCM_DMA_ADDR_MASK);
			p->rx_ring[i].buf_addr[ch] =
				(u32)((rxb_dma + ch * 160) & PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[i].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, chv) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
		p->rx_ring[i].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, chv) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, samples);
	}

	for (i = 0; i < 4; i++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + i, tx_slots[i]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, rx_slots[i]);
	}
	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, intface);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, chv) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	/* kick the DMA engine -- without the polling demand it never starts
	 * (this is what the 2b loopback self-test needed; ISR stayed 0 without it) */
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);

	dev_info(p->dev, "ZSI clock run: intface=0x%08x dma_ctrl=0x%08x isr=0x%08x\n",
		 intface, pcm_rd(p, PCM_TX_RX_DMA_CTRL), pcm_rd(p, PCM_ISR));
	return 0;
}
EXPORT_SYMBOL_GPL(pcm_en751221_zsi_clock_run);

/*
 * Capture one ~10ms voice frame from the SLIC over the PCM bus. The SLIC puts
 * its mic audio on timeslot 6 (SLIC TXSLOT=6); the SoC PCM RX DMA grabs it.
 * RX timeslot cfg3 selects slot 6 (0x00380030), descriptor chValid=0x40 (ch6),
 * sampleSize=80 (80 samples = 10ms @ 8kHz u-law). Copies the bytes to `out`.
 * Used by the SLIC driver to prove audio capture works.
 */
int pcm_en751221_capture_rx(u8 *out, int nbytes)
{
	struct pcm_dev *p = g_pcm;
	static void *cap;
	static dma_addr_t cap_dma;
	int i;

	if (!p)
		return -ENODEV;
	if (nbytes > 80)
		nbytes = 80;

	if (!cap) {
		cap = dmam_alloc_coherent(p->dev, 256, &cap_dma, GFP_KERNEL);
		if (!cap)
			return -ENOMEM;
	}
	memset(cap, 0, 256);

	/* RX timeslots: defaults + slot 6 in CFG3 */
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG0, 0x00080000);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG1, 0x00180010);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG2, 0x00280020);
	pcm_wr(p, PCM_RX_TIME_SLOT_CFG3, 0x00380030);

	/* arm RX descriptor 0: own | chValid ch6 (0x40) | 80 samples.
	 * The HW writes channel N's data to buf_addr[N] (see 2b loopback), so the
	 * channel-6 (slot 6 = SLIC mic) buffer goes in buf_addr[6], NOT [0]. */
	memset(p->rx_ring, 0, PCM_DESC_NUM * sizeof(struct pcm_desc));
	p->rx_ring[0].buf_addr[6] = (u32)(cap_dma & PCM_DMA_ADDR_MASK);
	p->rx_ring[0].status = PCM_DESC_OWN |
		FIELD_PREP(PCM_DESC_CH_VALID, 0x40) |
		FIELD_PREP(PCM_DESC_SAMPLE_SIZE, 80);

	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0x40) | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		if (!(READ_ONCE(p->rx_ring[0].status) & PCM_DESC_OWN))
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	memcpy(out, cap, nbytes);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	return (i < 500) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_capture_rx);

/*
 * Diagnostic: capture ALL 8 RX channels at once (chValid 0xff, OEM timeslot
 * map) into out[8*80]. Lets us scan which channel/slot the SLIC mic audio
 * actually lands on. Does NOT touch the SLIC -- assumes line already set up.
 */
int pcm_en751221_capture_allch(u8 *out)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *cap;
	static dma_addr_t cap_dma;
	int i, ch, d, done = -1;

	if (!p)
		return -ENODEV;
	/* one 8ch*80 slab per descriptor so the engine can land on any of them */
	if (!cap) {
		cap = dmam_alloc_coherent(p->dev, PCM_DESC_NUM * 8 * 80 + 64,
					  &cap_dma, GFP_KERNEL);
		if (!cap)
			return -ENOMEM;
	}
	memset(cap, 0, PCM_DESC_NUM * 8 * 80);

	for (i = 0; i < 4; i++)
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + i, oem_slots[i]);

	/*
	 * Arm ALL descriptors valid (OWN + 8ch + own buffer slab). After a
	 * previous capture the DMA engine's internal descriptor pointer is
	 * left advanced; re-writing RING_BASE does not always rewind it, so if
	 * only desc0 were valid the engine could resume on a non-OWN descriptor
	 * and stall forever (-ETIMEDOUT on every call after the first). With
	 * every descriptor valid the engine completes wherever it resumes and
	 * we read back whichever one it filled.
	 */
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((cap_dma + (d * 8 + ch) * 80) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, 80);
	}

	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0)
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0)
		done = 0;
	memcpy(out, (u8 *)cap + done * 8 * 80, 8 * 80);
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_RX_EN);
	return (i < 500) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_capture_allch);

/* PCM frame geometry: the codec is 16-bit linear, so sample_size counts
 * 16-bit samples and each one occupies 2 bytes on the bus (confirmed by the
 * OEM CODEC_LINEAR setting and the pcm_selftest_fill_desc 2-byte stride). */
#define PCM_SAMP_PER_FRAME	80
#define PCM_BYTES_PER_FRAME	(PCM_SAMP_PER_FRAME * 2)

/*
 * Build a recognisable square-wave melody ("Twinkle Twinkle") as 8 kHz signed
 * 16-bit linear samples, big-endian (the SoC is MIPS BE and the SLIC codec is
 * linear). Square wave (chiptune) so it synthesises with pure integer math.
 * Returns a kmalloc'd byte buffer (caller kfree's), length rounded to a frame.
 */
static u8 *pcm_build_melody(int *out_len)
{
	static const u16 notes[][2] = {	/* { freq Hz (0 = rest), ms } */
		{ 523, 280 }, { 523, 280 }, { 784, 280 }, { 784, 280 },
		{ 880, 280 }, { 880, 280 }, { 784, 520 }, {   0,  60 },
		{ 698, 280 }, { 698, 280 }, { 659, 280 }, { 659, 280 },
		{ 587, 280 }, { 587, 280 }, { 523, 520 },
	};
	int n = ARRAY_SIZE(notes), i, j, idx = 0, total = 0;
	u16 phase = 0;
	u8 *buf;

	for (i = 0; i < n; i++)
		total += notes[i][1] * 8;	/* 8 samples per ms @ 8 kHz */
	buf = kmalloc(total * 2 + PCM_BYTES_PER_FRAME, GFP_KERNEL);
	if (!buf)
		return NULL;

	for (i = 0; i < n; i++) {
		u16 f = notes[i][0];
		int samp = notes[i][1] * 8;
		u32 inc = f ? (u32)f * 65536 / 8000 : 0;

		for (j = 0; j < samp; j++) {
			s16 s = 0;

			if (f) {
				phase += inc;
				s = (phase & 0x8000) ? 20000 : -20000;
			}
			buf[idx++] = (u8)(s >> 8);	/* big-endian */
			buf[idx++] = (u8)(s & 0xff);
		}
	}
	while (idx % PCM_BYTES_PER_FRAME) {	/* pad final frame with silence */
		buf[idx++] = 0;
		buf[idx++] = 0;
	}
	*out_len = idx;
	return buf;
}

/*
 * Play the melody out the PCM TX path, broadcast to all 8 channels (chValid
 * 0xff) so it lands on whichever bus slot the SLIC earpiece (RXSLOT) reads.
 * Software-recycles the 15-descriptor ring for the whole tune. Blocks for the
 * duration of the melody (~4 s); meant to be driven from a debugfs read.
 */
int pcm_en751221_play_melody(void)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	u8 *mel;
	int mel_len = 0, pos = 0, d, ch, guard = 0;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev, 8 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	mel = pcm_build_melody(&mel_len);
	if (!mel)
		return -ENOMEM;

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}

	/* prime the TX ring with the first PCM_DESC_NUM frames (160 B each) */
	for (d = 0; d < PCM_DESC_NUM && pos < mel_len; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->tx_ring[d].buf_addr[ch] =
				(u32)((txb_dma + d * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		memcpy((u8 *)txb + d * PCM_BYTES_PER_FRAME, mel + pos,
		       PCM_BYTES_PER_FRAME);
		pos += PCM_BYTES_PER_FRAME;
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	/*
	 * The full-duplex frame engine only advances the TX descriptors when
	 * RX is also enabled (TX-only leaves OWN bits set forever -- ISR shows
	 * frame boundaries but no TX descriptor updates). Arm a throwaway RX
	 * ring into a scratch buffer so the engine clocks, mirroring the
	 * known-good zsi_clock_run dual-direction setup.
	 */
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++)
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + ch * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, 0xf5071306);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	/* refill descriptors as the engine releases them, until the tune ends */
	while (pos < mel_len && guard < 2000) {	/* ~6s no-progress bail-out */
		int progressed = 0;

		for (d = 0; d < PCM_DESC_NUM && pos < mel_len; d++) {
			dma_rmb();
			if (READ_ONCE(p->tx_ring[d].status) & PCM_DESC_OWN)
				continue;
			memcpy((u8 *)txb + d * PCM_BYTES_PER_FRAME, mel + pos,
			       PCM_BYTES_PER_FRAME);
			pos += PCM_BYTES_PER_FRAME;
			dma_wmb();
			p->tx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
			pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
			progressed = 1;
		}
		/* keep the RX side clocking so TX keeps advancing */
		for (d = 0; d < PCM_DESC_NUM; d++) {
			if (READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)
				continue;
			p->rx_ring[d].status = PCM_DESC_OWN |
				FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
				FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		}
		pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);
		if (!progressed) {
			usleep_range(2000, 4000);
			guard++;
		}
	}
	msleep(220);		/* let the last queued frames drain */
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~PCM_DMA_TX_EN);
	kfree(mel);
	return 0;
}
EXPORT_SYMBOL_GPL(pcm_en751221_play_melody);

/*
 * Loopback test: transmit a known ramp (0,1,2,...79) on all 8 TX slots while
 * capturing RX channel 2 at the same time. With the SLIC's TSA loopback armed
 * by the caller, the bytes we send come straight back, so a ramp in out[]
 * proves the SoC TX -> bus -> SoC RX digital path (DMA + slot map) works end
 * to end -- independent of the analog earpiece. Returns ch2 (80 bytes).
 */
int pcm_en751221_loopback_capture(u8 *out, u32 intface)
{
	struct pcm_dev *p = g_pcm;
	static const u32 oem_slots[4] = {
		0x10301020, 0x10501040, 0x10701060, 0x10901080,
	};
	static void *txb, *rxb;
	static dma_addr_t txb_dma, rxb_dma;
	int i, d, ch, done = -1;

	if (!p)
		return -ENODEV;
	if (!txb) {
		txb = dmam_alloc_coherent(p->dev, PCM_BYTES_PER_FRAME + 64,
					  &txb_dma, GFP_KERNEL);
		rxb = dmam_alloc_coherent(p->dev,
					  PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME + 64,
					  &rxb_dma, GFP_KERNEL);
		if (!txb || !rxb)
			return -ENOMEM;
	}
	for (i = 0; i < PCM_BYTES_PER_FRAME; i++)	/* ramp pattern */
		((u8 *)txb)[i] = (u8)i;
	memset(rxb, 0, PCM_DESC_NUM * 8 * PCM_BYTES_PER_FRAME);

	for (d = 0; d < 4; d++) {
		pcm_wr(p, PCM_TX_TIME_SLOT_CFG0 + d, oem_slots[d]);
		pcm_wr(p, PCM_RX_TIME_SLOT_CFG0 + d, oem_slots[d]);
	}
	for (d = 0; d < PCM_DESC_NUM; d++) {
		memset(&p->tx_ring[d], 0, sizeof(struct pcm_desc));
		memset(&p->rx_ring[d], 0, sizeof(struct pcm_desc));
		for (ch = 0; ch < 8; ch++) {
			p->tx_ring[d].buf_addr[ch] =
				(u32)(txb_dma & PCM_DMA_ADDR_MASK);
			p->rx_ring[d].buf_addr[ch] =
				(u32)((rxb_dma + (d * 8 + ch) * PCM_BYTES_PER_FRAME) &
				      PCM_DMA_ADDR_MASK);
		}
		p->tx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
		p->rx_ring[d].status = PCM_DESC_OWN |
			FIELD_PREP(PCM_DESC_CH_VALID, 0xff) |
			FIELD_PREP(PCM_DESC_SAMPLE_SIZE, PCM_SAMP_PER_FRAME);
	}

	pcm_wr(p, PCM_TX_DESC_RING_BASE, (u32)(p->tx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_RX_DESC_RING_BASE, (u32)(p->rx_dma & PCM_DMA_ADDR_MASK));
	pcm_wr(p, PCM_TX_RX_DESC_RING_SIZE_OFFSET, 0x9f);
	pcm_wr(p, PCM_INTFACE_CTRL, intface);
	dma_wmb();
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       FIELD_PREP(PCM_DMA_CH_VALID_MASK, 0xff) |
	       PCM_DMA_TX_EN | PCM_DMA_RX_EN);
	pcm_wr(p, PCM_TX_POLLING_DEMAND, 1);
	pcm_wr(p, PCM_RX_POLLING_DEMAND, 1);

	/* let a few frames loop through, then grab a completed RX descriptor */
	for (i = 0; i < 500; i++) {
		dma_rmb();
		for (d = PCM_DESC_NUM - 1; d >= 0; d--) {
			if (!(READ_ONCE(p->rx_ring[d].status) & PCM_DESC_OWN)) {
				done = d;
				break;
			}
		}
		if (done >= 0 && i > 20)	/* give loopback pipeline time */
			break;
		usleep_range(1000, 2000);
	}
	dma_rmb();
	if (done < 0)
		done = 0;
	memcpy(out, (u8 *)rxb + (done * 8 + 2) * PCM_BYTES_PER_FRAME,
	       PCM_BYTES_PER_FRAME);				/* ch2 */
	pcm_wr(p, PCM_TX_RX_DMA_CTRL,
	       pcm_rd(p, PCM_TX_RX_DMA_CTRL) & ~(PCM_DMA_TX_EN | PCM_DMA_RX_EN));
	return (done >= 0) ? 0 : -ETIMEDOUT;
}
EXPORT_SYMBOL_GPL(pcm_en751221_loopback_capture);

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

static int pcm_loopback_selftest_show(struct seq_file *s, void *unused)
{
	struct pcm_dev *p = s->private;
	int ret = pcm_loopback_selftest(p);

	if (ret)
		seq_printf(s, "BACK_TO_BACK loopback: failed (%d)\n", ret);
	else
		seq_puts(s, "BACK_TO_BACK loopback: ok\n");

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(pcm_loopback_selftest);

static int pcm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pcm_dev *p;
	u32 chip;
	int ret;

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

	p->chip_scu = devm_ioremap(dev, CHIP_SCU_PHYS_BASE, 0x388);
	if (!p->chip_scu)
		return -ENOMEM;

	mutex_init(&p->selftest_lock);

	chip = readl(p->scu + SCU_CHIP_ID) & SCU_CHIP_ID_MASK;
	if (chip != SCU_CHIP_EN751221 && chip != SCU_CHIP_EN7526C)
		dev_warn(dev, "unexpected chip id 0x%08x (continuing)\n", chip);
	else
		dev_info(dev, "EN751221-class SoC (chip id 0x%08x)\n", chip);

	pcm_soft_reset(p);
	pcm_load_defaults(p);

	ret = pcm_alloc_rings(p);
	if (ret)
		return ret;

	/* IRQ is optional in phase 1: a wrong DT number must not block bring-up */
	p->irq = platform_get_irq_optional(pdev, 0);
	if (p->irq > 0) {
		ret = devm_request_irq(dev, p->irq, pcm_irq, 0,
				       dev_name(dev), p);
		if (ret)
			dev_warn(dev, "could not request IRQ %d: %d\n",
				 p->irq, ret);
	}

	p->dbg = debugfs_create_dir("pcm-en751221", NULL);
	debugfs_create_file("regs", 0444, p->dbg, p, &pcm_regs_fops);
	debugfs_create_file("loopback_selftest", 0444, p->dbg, p,
			    &pcm_loopback_selftest_fops);

	platform_set_drvdata(pdev, p);
	g_pcm = p;	/* for pcm_en751221_zsi_clock_run() (SLIC ZSI clock) */
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
