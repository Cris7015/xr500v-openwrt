// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet/TrendChip EN751221 Le9642 SLIC ZSI driver -- phase 3a (chip detect).
 *
 * The XR500v wires its two Le9642 SLICs over ZSI (Zarlink Serial Interface),
 * NOT plain SPI -- the OEM does `insmod slic3.ko type=ZSI`. ZSI multiplexes the
 * SLIC control channel over the PCM bus: control bytes go through a ZSI wrapper
 * at 0x1fbd1000 (+id*0x2000), while the PCM engine must be generating PCLK/FSYNC
 * for the SLIC to clock its responses. So this driver drives the ZSI wrapper
 * AND brings the PCM up in ZSI clock mode.
 *
 * Reconstructed from spi.ko (ZSI_bytes_read/write), slic3_main.c (the type=ZSI
 * init sequence) and pcm1.ko (timeSlotCfgReinit). The ZSI wrapper handshake was
 * verified live by register-poking before this driver was written: the wrapper
 * ack (bit1) and rx-ready (bit2) bits respond correctly; the remaining piece is
 * the PCM clock, which is what this module adds.
 *
 * Detect runs on demand via debugfs (opt-in, re-runnable). The PCM clock
 * timeslot table and INTFACE_CTRL are debugfs-writable so they can be swept on
 * the live device without reflashing.
 */
#include <linux/module.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/mutex.h>

#define DRV_NAME "econet-slic"

/* MMIO bases (phys). */
#define CHIP_SCU_PHYS		0x1fa20000	/* pinmux/clock */
#define CHIP_SCU_SIZE		0x1000
#define SYS_PHYS		0x1fb00000	/* SCU: chip-id, spi-mode, gpio/reset */
#define SYS_SIZE		0x1000
#define PCM_PHYS		0x1fbd0000	/* PCM engine (clock/fsync for ZSI) */
#define PCM_SIZE		0x1000
#define ZSI_PHYS		0x1fbd1000	/* ZSI wrapper (+ id*0x2000) */
#define ZSI_SIZE		0x1000

/* chip_scu offsets */
#define SCU_IOMUX_CONTROL1	0x104
#define SCU_PCM_CLK_SRC_SEL	0x148
#define IOMUX1_SLIC_MASK	0x7d00
#define IOMUX1_GPIO_ZSI_ISI	0x2000	/* ZSI/ISI pin routing (vs 0x7400 SPI) */
#define PCM_ZSI_CLK_SOURCE	0x0c	/* PCM_CLK_SOURCE_SELECT bits[3:2]=1 */

/* sys/SCU offsets */
#define SYS_CHIP_ID		0x064
#define SYS_SPI_MODE		0x094	/* [3:0] interface mode; ZSI route = 0x5 */
#define SYS_GPIO_DATA		0x834	/* SLIC reset: bit0 + bit17 (NOT bit25) */
#define SYS_CHIP_EN7526C	0x00080000
#define SYS_MODE_ZSI		0x5

/* PCM engine registers (same map as econet-pcm). */
#define PCM_INTFACE_CTRL	0x00
#define PCM_TX_TIME_SLOT_CFG0	0x04	/* 0x04..0x10 */
#define PCM_RX_TIME_SLOT_CFG0	0x14	/* 0x14..0x20 */

/* ZSI wrapper registers (0x1fbd1000 + id*0x2000). */
#define ZSI_CFG			0x000	/* (old & 0xe0ffffef) | 0x0f000010 */
#define ZSI_TXRX		0x004	/* write cmd/data byte */
#define ZSI_CTL			0x008	/* bit0 read-req, bit1 tx-ack, bit2 rx-ready */
#define ZSI_RX			0x00c	/* received byte (low 8 bits) */
#define ZSI_EN			0x010	/* block enable = 0x19 */

#define ZSI_CFG_MASK		0xe0ffffefu
#define ZSI_CFG_VAL		0x0f030313u	/* OEM-stock observed value (voip running) */
#define ZSI_EN_VAL		0x19
#define ZSI_CTL_READ_REQ	BIT(0)
#define ZSI_CTL_TX_ACK		BIT(1)
#define ZSI_CTL_RX_READY	BIT(2)

/* MPI / Le9642 (VP886) constants. */
#define CSLAC_EC_REG_WRT	0x4a
#define VP886_EC_1		0x01
#define VP886_R_RCNPCN_RD	0x73
#define VP886_R_RCNPCN_LEN	2
#define VP886_R_RCNPCN_RCN	0x08
#define VP886_R_RCNPCN_PCN_LE9641 0x49

#define ZSI_POLL_ITERS		20000	/* x1us = up to 20ms per wait */

struct slic_dev {
	void __iomem *chip_scu;
	void __iomem *sys;
	void __iomem *pcm;
	void __iomem *zsi;
	struct dentry *dbg;
	struct mutex lock;
	u8 cs;
	const char *where;	/* phase tag for timeout reporting */
};

static struct slic_dev sd;

/*
 * PCM clock config for ZSI mode. These set the PCM engine generating PCLK/FSYNC
 * so the SLIC has a clock. timeSlotCfgReinit(2) in the OEM computes the slot
 * regs; INTFACE_CTRL must enable the engine WITHOUT loopback (clock goes to the
 * external pins, not internal). Debugfs-writable to sweep on live HW.
 *
 * Defaults: INTFACE_CTRL without loopback (bit25 clear) + config-valid (bit31);
 * timeslots seeded from the 2b loopback values. CONFIRM against an OEM-stock
 * register dump (voip running) -- these are the best estimate pre-dump.
 */
/* Values read from the OEM-stock device with voip running (ground truth). */
static u32 pcm_intface_ctrl = 0xf5071306;
static u32 pcm_tx_slots[4] = { 0x10301020, 0x10501040, 0x10701060, 0x10901080 };
static u32 pcm_rx_slots[4] = { 0x10301020, 0x10501040, 0x10701060, 0x10901080 };
static u32 zsi_cfg_val = ZSI_CFG_VAL;	/* debugfs-tunable */

static int zsi_poll_set(u32 mask)
{
	int i;

	for (i = 0; i < ZSI_POLL_ITERS; i++) {
		if (readl(sd.zsi + ZSI_CTL) & mask)
			return 0;
		udelay(1);
	}
	pr_err(DRV_NAME ": TIMEOUT @%s waiting ZSI_CTL&0x%x (ctl=0x%08x)\n",
	       sd.where, mask, readl(sd.zsi + ZSI_CTL));
	return -ETIMEDOUT;
}

/* ZSI: write one byte (cmd or data). Verified-by-poke handshake. */
static int zsi_write_byte(u8 v)
{
	int ret;

	writel(v, sd.zsi + ZSI_TXRX);
	ret = zsi_poll_set(ZSI_CTL_TX_ACK);
	if (ret)
		return ret;
	writel(ZSI_CTL_TX_ACK, sd.zsi + ZSI_CTL);	/* W1C the ack */
	return 0;
}

/* ZSI: clock in and read one byte. */
static int zsi_read_byte(u8 *out)
{
	int ret;

	writel(ZSI_CTL_READ_REQ, sd.zsi + ZSI_CTL);
	ret = zsi_poll_set(ZSI_CTL_RX_READY);
	if (ret)
		return ret;
	*out = readl(sd.zsi + ZSI_RX) & 0xff;
	writel(ZSI_CTL_RX_READY, sd.zsi + ZSI_CTL);	/* W1C the ready */
	return 0;
}

static void zsi_begin(void)
{
	/* OEM stock shows ZSI_CFG = 0x0f030313 with voip running; write it. */
	writel(zsi_cfg_val, sd.zsi + ZSI_CFG);
}

static int zsi_write(u8 cmd, const u8 *buf, u8 len)
{
	int ret;
	u8 i;

	sd.where = "zsi_write";
	zsi_begin();
	ret = zsi_write_byte(cmd);
	if (ret)
		return ret;
	for (i = 0; i < len; i++) {
		ret = zsi_write_byte(buf[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int zsi_read(u8 cmd, u8 *buf, u8 len)
{
	int ret;
	u8 i;

	sd.where = "zsi_read";
	zsi_begin();
	ret = zsi_write_byte(cmd);
	if (ret)
		return ret;
	for (i = 0; i < len; i++) {
		ret = zsi_read_byte(&buf[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/*
 * Bring the PCM engine up (DMA running) so it generates continuous PCLK/FSYNC,
 * which a ZSI SLIC clocks off. Implemented in econet-pcm (it owns the DMA
 * device + the HW-validated ring/descriptor logic).
 */
extern int pcm_en751221_zsi_clock_run(u32 intface, const u32 *tx_slots,
				      const u32 *rx_slots);

static int pcm_zsi_clock_setup(void)
{
	int ret = pcm_en751221_zsi_clock_run(pcm_intface_ctrl,
					     pcm_tx_slots, pcm_rx_slots);
	if (ret)
		pr_warn(DRV_NAME ": pcm clock run failed: %d\n", ret);
	return ret;
}

/* type=ZSI init from slic3_main.c: route, iomux GPIO_ZSI_ISI, ZSI clock src. */
static void zsi_hw_init(void)
{
	u32 v;

	/* interface route = ZSI (0x1fb00094 [3:0] = 0x5) */
	v = readl(sd.sys + SYS_SPI_MODE);
	writel((v & ~0xfu) | SYS_MODE_ZSI, sd.sys + SYS_SPI_MODE);

	/* pinmux: GPIO_ZSI_ISI (clear SLIC field, set ZSI) */
	v = readl(sd.chip_scu + SCU_IOMUX_CONTROL1);
	writel((v & ~IOMUX1_SLIC_MASK) | IOMUX1_GPIO_ZSI_ISI,
	       sd.chip_scu + SCU_IOMUX_CONTROL1);

	/* PCM clock source = ZSI (OEM stock low nibble = 0x1c, not just 0x0c) */
	v = readl(sd.chip_scu + SCU_PCM_CLK_SRC_SEL);
	writel(v | 0x1c, sd.chip_scu + SCU_PCM_CLK_SRC_SEL);

	/* PCM clock running (PCLK/FSYNC for the SLIC) */
	pcm_zsi_clock_setup();

	/* ZSI wrapper block enable + config */
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
}

/* ZSI SLIC reset: 0x1fb00834 bit0 then bit17 (EN751221). NOT bit25 (that's SPI). */
static void zsi_slic_reset(void)
{
	u32 g, chip;

	g = readl(sd.sys + SYS_GPIO_DATA);
	writel(g | BIT(0), sd.sys + SYS_GPIO_DATA);
	msleep(5);
	writel(g & ~BIT(0), sd.sys + SYS_GPIO_DATA);

	chip = readl(sd.sys + SYS_CHIP_ID) & 0xffff0000;
	if (chip != SYS_CHIP_EN7526C) {
		g = readl(sd.sys + SYS_GPIO_DATA);
		writel(g | BIT(17), sd.sys + SYS_GPIO_DATA);
		msleep(5);
		writel(g & ~BIT(17), sd.sys + SYS_GPIO_DATA);
	}
	msleep(10);	/* let the SLIC core boot after reset */
}

/* MPI register read over ZSI: write [EC_REG_WRT, ec] then read `len` of `reg`. */
static int zsi_mpi_read(u8 ec, u8 reg, u8 *buf, u8 len)
{
	int ret;

	ret = zsi_write(CSLAC_EC_REG_WRT, &ec, 1);
	if (ret)
		return ret;
	return zsi_read(reg, buf, len);
}

static int slic_detect(u8 *buf, u8 n)
{
	int ret;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	/* re-assert wrapper config after reset */
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	ret = zsi_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, buf, n);
	mutex_unlock(&sd.lock);
	return ret;
}

#define SLIC_DETECT_NBYTES	4

static int slic_detect_show(struct seq_file *s, void *unused)
{
	u8 id[SLIC_DETECT_NBYTES];
	int ret;

	memset(id, 0xee, sizeof(id));
	ret = slic_detect(id, sizeof(id));

	seq_printf(s, "RCNPCN(ZSI) ret=%d bytes= %*ph (expect 0x08 0x49)\n",
		   ret, (int)sizeof(id), id);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9641)
		seq_puts(s, "Le9642 detected: OK\n");
	else
		seq_puts(s, "Le9642 detect: FAILED\n");

	seq_printf(s, "pcm: intface=0x%08x txslot=%4ph rxslot=%4ph\n",
		   readl(sd.pcm + PCM_INTFACE_CTRL),
		   sd.pcm + PCM_TX_TIME_SLOT_CFG0, sd.pcm + PCM_RX_TIME_SLOT_CFG0);
	seq_printf(s, "zsi: cfg=0x%08x ctl=0x%08x en=0x%08x  sys094=0x%08x iomux=0x%08x\n",
		   readl(sd.zsi + ZSI_CFG), readl(sd.zsi + ZSI_CTL),
		   readl(sd.zsi + ZSI_EN), readl(sd.sys + SYS_SPI_MODE),
		   readl(sd.chip_scu + SCU_IOMUX_CONTROL1));

	pr_info(DRV_NAME ": detect ret=%d bytes=%*ph\n", ret, (int)sizeof(id), id);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_detect);

static int __init econet_slic_init(void)
{
	sd.chip_scu = ioremap(CHIP_SCU_PHYS, CHIP_SCU_SIZE);
	sd.sys = ioremap(SYS_PHYS, SYS_SIZE);
	sd.pcm = ioremap(PCM_PHYS, PCM_SIZE);
	sd.zsi = ioremap(ZSI_PHYS, ZSI_SIZE);
	if (!sd.chip_scu || !sd.sys || !sd.pcm || !sd.zsi)
		goto err;

	mutex_init(&sd.lock);
	sd.cs = 0;
	sd.where = "idle";

	sd.dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("slic_detect", 0444, sd.dbg, NULL, &slic_detect_fops);
	debugfs_create_u8("cs", 0644, sd.dbg, &sd.cs);
	debugfs_create_x32("pcm_intface", 0644, sd.dbg, &pcm_intface_ctrl);
	debugfs_create_x32("zsi_cfg", 0644, sd.dbg, &zsi_cfg_val);
	{
		static const char *tx_n[4] = { "tx_slot0", "tx_slot1", "tx_slot2", "tx_slot3" };
		static const char *rx_n[4] = { "rx_slot0", "rx_slot1", "rx_slot2", "rx_slot3" };
		int i;

		for (i = 0; i < 4; i++) {
			debugfs_create_x32(tx_n[i], 0644, sd.dbg, &pcm_tx_slots[i]);
			debugfs_create_x32(rx_n[i], 0644, sd.dbg, &pcm_rx_slots[i]);
		}
	}

	pr_info(DRV_NAME ": loaded ZSI mode (cat /sys/kernel/debug/%s/slic_detect)\n",
		DRV_NAME);
	return 0;
err:
	if (sd.zsi)
		iounmap(sd.zsi);
	if (sd.pcm)
		iounmap(sd.pcm);
	if (sd.sys)
		iounmap(sd.sys);
	if (sd.chip_scu)
		iounmap(sd.chip_scu);
	return -ENOMEM;
}

static void __exit econet_slic_exit(void)
{
	debugfs_remove_recursive(sd.dbg);
	iounmap(sd.zsi);
	iounmap(sd.pcm);
	iounmap(sd.sys);
	iounmap(sd.chip_scu);
}

module_init(econet_slic_init);
module_exit(econet_slic_exit);

MODULE_DESCRIPTION("EcoNet EN751221 Le9642 SLIC ZSI chip detect (phase 3a)");
MODULE_LICENSE("GPL");
