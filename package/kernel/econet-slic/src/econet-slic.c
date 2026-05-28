// SPDX-License-Identifier: GPL-2.0
/*
 * EcoNet/TrendChip EN751221 Le9641 SLIC SPI/MPI driver -- phase 3a (chip detect).
 *
 * Reads the Le9641 RCNPCN device-id register over the SoC's "new" Zarlink SLIC
 * SPI block at 0x1fbd4000 to confirm we can talk to the SLIC. Reconstructed
 * from the OEM spi.ko (SPI_slic_check / New_SPI_bytes_*_zarlink / SPI_cfg /
 * SPI_Reset, disassembled) and le9641.c (le9641_deviceVerify). Register offsets
 * and control values were cross-checked against the spi.ko disassembly.
 *
 * The detect runs on demand via debugfs (cat .../econet-slic/slic_detect), not
 * at module load, so it is opt-in and re-runnable.
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
#define CHIP_SCU_PHYS		0x1fa20000	/* pinmux/clock; map big to reach 0xff00 */
#define CHIP_SCU_SIZE		0x10000
#define SYS_PHYS		0x1fb00000	/* SCU: chip-id, spi-mode, gpio/reset */
#define SYS_SIZE		0x1000
#define SLIC_SPI_PHYS		0x1fbd4000	/* "new" Zarlink SLIC SPI block */
#define SLIC_SPI_SIZE		0x1000

/* chip_scu offsets */
#define SCU_IOMUX_CONTROL1	0x104
#define SCU_PCM_CLK_SRC_SEL	0x148
#define SCU_SPICFG_FF00		0xff00	/* SPI_cfg path, 0x1fb0008c bit29 == 0 */
#define SCU_SPICFG_00CC		0x0cc	/* SPI_cfg path, bit29 == 1 */
#define IOMUX1_SPI_SLIC_MASK	0x7d00
#define IOMUX1_GPIO_SPI_SLIC	0x7400
#define PCM_ZSI_CLK_SOURCE	0x0c

/* sys/SCU offsets */
#define SYS_CHIP_ID		0x064
#define SYS_SPI_MODE		0x094	/* [4:0] mode; BIT(5) = new-SLIC path */
#define SYS_SPICFG_8C		0x08c
#define SYS_GPIO_DATA		0x834	/* SLIC reset bits 0/17, HW reset bit 25 */
#define SYS_SPI_MODE_NEW	BIT(5)
#define SYS_CHIP_EN7526C	0x00080000

/* SLIC SPI block (0x1fbd4000) offsets */
#define SLIC_GLOBAL		0x004
#define SLIC_MODE		0x014
#define SLIC_INIT_BUSY		0x018
#define SLIC_ENABLE		0x020
#define SLIC_DONE		0x024
#define SLIC_CTL		0x028
#define SLIC_CTL_BUSY		0x02c
#define SLIC_CTL_ACK		0x030
#define SLIC_TX_BUSY		0x034
#define SLIC_TX			0x038
#define SLIC_RX_BUSY		0x03c
#define SLIC_RX_ACK		0x040
#define SLIC_RX			0x044
#define SLIC_NEWBLK_EN		0x098	/* SPI_cfg: |= 1 for arg 1/2 */
#define SLIC_CS_SELECT		0x0e4

/* CTL command words (confirmed against spi.ko disasm). */
#define CTL_PREP		0x0201
#define CTL_CMD_READ		0x2001
#define CTL_CMD_WRITE		0x1001
#define CTL_DATA_READ		0x1801
#define CTL_BYTE		0x0001
#define CTL_TRAILER		0x0405

/* MPI / Le9641 (VP886) constants. */
#define CSLAC_EC_REG_WRT	0x4a
#define VP886_EC_1		0x01
#define VP886_R_RCNPCN_RD	0x73
#define VP886_R_RCNPCN_LEN	2
#define VP886_R_RCNPCN_RCN	0x08
#define VP886_R_RCNPCN_PCN_LE9641 0x49

#define SLIC_POLL_ITERS		20000	/* x1us = up to 20ms per wait */

struct slic_dev {
	void __iomem *chip_scu;
	void __iomem *sys;
	void __iomem *spi;
	struct dentry *dbg;
	struct mutex lock;
	u8 cs;			/* SLIC chip-select / devNum */
};

static struct slic_dev sd;

/* phase 3a instrumentation: which phase we're in, for timeout reporting. */
static const char *slic_where = "idle";

/* poll a *_busy style register: wait until it reads 0. */
static int slic_wait_clear(u32 off)
{
	int i;

	for (i = 0; i < SLIC_POLL_ITERS; i++) {
		if (!readl(sd.spi + off))
			return 0;
		udelay(1);
	}
	pr_err(DRV_NAME ": TIMEOUT wait_clear @%s reg[0x%03x]=0x%08x\n",
	       slic_where, off, readl(sd.spi + off));
	return -ETIMEDOUT;
}

/* poll the done register (0x24): wait until it reads non-zero. */
static int slic_wait_done(void)
{
	int i;

	for (i = 0; i < SLIC_POLL_ITERS; i++) {
		if (readl(sd.spi + SLIC_DONE))
			return 0;
		udelay(1);
	}
	pr_err(DRV_NAME ": TIMEOUT wait_done @%s reg[0x024]=0x%08x\n",
	       slic_where, readl(sd.spi + SLIC_DONE));
	return -ETIMEDOUT;
}

/* issue one CTL command word and wait for the engine to consume it. */
static int slic_ctl(u32 cmd)
{
	int ret;

	writel(cmd, sd.spi + SLIC_CTL);
	ret = slic_wait_clear(SLIC_CTL_BUSY);
	if (ret)
		return ret;
	writel(1, sd.spi + SLIC_CTL_ACK);
	return 0;
}

static int slic_xfer_begin(bool read)
{
	int ret;

	slic_where = read ? "begin/rd" : "begin/wr";
	writel(0, sd.spi + SLIC_GLOBAL);
	ret = slic_wait_clear(SLIC_INIT_BUSY);
	if (ret)
		return ret;
	writel(9, sd.spi + SLIC_MODE);
	writel(1, sd.spi + SLIC_ENABLE);
	writel(sd.cs, sd.spi + SLIC_CS_SELECT);

	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	return slic_ctl(read ? CTL_CMD_READ : CTL_CMD_WRITE);
}

static void slic_xfer_end(void)
{
	writel(0, sd.spi + SLIC_MODE);
	writel(0, sd.spi + SLIC_ENABLE);
	writel(1, sd.spi + SLIC_GLOBAL);
}

/* shift out one byte on TX. */
static int slic_tx_byte(u8 v)
{
	int ret;

	slic_where = "tx_byte";
	ret = slic_wait_clear(SLIC_TX_BUSY);
	if (ret)
		return ret;
	writel(v, sd.spi + SLIC_TX);
	ret = slic_ctl(CTL_BYTE);
	if (ret)
		return ret;
	writel(CTL_TRAILER, sd.spi + SLIC_CTL);
	return slic_wait_done();
}

/* data-phase write byte (after the command byte). */
static int slic_data_write(u8 v)
{
	int ret;

	slic_where = "data_wr";
	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	ret = slic_ctl(CTL_CMD_WRITE);
	if (ret)
		return ret;
	return slic_tx_byte(v);
}

/* data-phase read byte. */
static int slic_data_read(u8 *out)
{
	int ret;

	slic_where = "data_rd";
	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	ret = slic_ctl(CTL_PREP);
	if (ret)
		return ret;
	ret = slic_ctl(CTL_DATA_READ);
	if (ret)
		return ret;
	ret = slic_wait_clear(SLIC_RX_BUSY);
	if (ret)
		return ret;
	*out = readl(sd.spi + SLIC_RX) & 0xff;
	writel(1, sd.spi + SLIC_RX_ACK);
	ret = slic_ctl(CTL_BYTE);
	if (ret)
		return ret;
	writel(CTL_TRAILER, sd.spi + SLIC_CTL);
	return slic_wait_done();
}

static int slic_spi_write(u8 cmd, const u8 *buf, u8 len)
{
	int ret;
	u8 i;

	ret = slic_xfer_begin(false);
	if (ret)
		goto out;
	ret = slic_tx_byte(cmd);
	if (ret)
		goto out;
	for (i = 0; i < len; i++) {
		ret = slic_data_write(buf[i]);
		if (ret)
			goto out;
	}
out:
	slic_xfer_end();
	return ret;
}

static int slic_spi_read(u8 cmd, u8 *buf, u8 len)
{
	int ret;
	u8 i;

	ret = slic_xfer_begin(true);
	if (ret)
		goto out;
	ret = slic_tx_byte(cmd);
	if (ret)
		goto out;
	for (i = 0; i < len; i++) {
		ret = slic_data_read(&buf[i]);
		if (ret)
			goto out;
	}
out:
	slic_xfer_end();
	return ret;
}

/* SPI_cfg: pinmux, clock, the 0x1fb0008c bit29 branch, and new-block enable. */
static void slic_spi_cfg(void)
{
	u32 v;

	v = readl(sd.chip_scu + SCU_IOMUX_CONTROL1);
	writel((v & ~IOMUX1_SPI_SLIC_MASK) | IOMUX1_GPIO_SPI_SLIC,
	       sd.chip_scu + SCU_IOMUX_CONTROL1);

	v = readl(sd.chip_scu + SCU_PCM_CLK_SRC_SEL);
	writel(v | PCM_ZSI_CLK_SOURCE, sd.chip_scu + SCU_PCM_CLK_SRC_SEL);

	if (readl(sd.sys + SYS_SPICFG_8C) & BIT(29)) {
		v = readl(sd.chip_scu + SCU_SPICFG_00CC);
		writel((v & 0xffff) | 0x00010000, sd.chip_scu + SCU_SPICFG_00CC);
	} else {
		v = readl(sd.chip_scu + SCU_SPICFG_FF00);
		writel((v & ~0x7u) | 0x3, sd.chip_scu + SCU_SPICFG_FF00);
	}

	v = readl(sd.spi + SLIC_NEWBLK_EN);
	writel(v | 0x1, sd.spi + SLIC_NEWBLK_EN);

	v = readl(sd.sys + SYS_SPI_MODE);
	if (!(v & SYS_SPI_MODE_NEW))
		writel(v | SYS_SPI_MODE_NEW, sd.sys + SYS_SPI_MODE);
}

/* SPI_Reset: SLIC reset pulse on the SoC GPIO/reset register. */
static void slic_reset(void)
{
	u32 mode, gpio, chip;

	mode = readl(sd.sys + SYS_SPI_MODE);
	writel((mode & ~0xfu) | 0x5, sd.sys + SYS_SPI_MODE);

	/* HW reset pulse (BIT25) */
	gpio = readl(sd.sys + SYS_GPIO_DATA);
	writel(gpio | BIT(25), sd.sys + SYS_GPIO_DATA);
	usleep_range(5000, 6000);
	writel(gpio & ~BIT(25), sd.sys + SYS_GPIO_DATA);
	usleep_range(5000, 6000);

	/* SLIC reset pulse (BIT0) */
	gpio = readl(sd.sys + SYS_GPIO_DATA);
	writel(gpio | BIT(0), sd.sys + SYS_GPIO_DATA);
	msleep(5);
	writel(gpio & ~BIT(0), sd.sys + SYS_GPIO_DATA);

	/* second SLIC reset bit (BIT17), skipped on EN7526C */
	chip = readl(sd.sys + SYS_CHIP_ID) & 0xffff0000;
	if (chip != SYS_CHIP_EN7526C) {
		gpio = readl(sd.sys + SYS_GPIO_DATA);
		writel(gpio | BIT(17), sd.sys + SYS_GPIO_DATA);
		msleep(5);
		writel(gpio & ~BIT(17), sd.sys + SYS_GPIO_DATA);
	}
}

/* MPI register read: write [EC_REG_WRT, ec] then read `len` bytes of `reg`. */
static int slic_mpi_read(u8 ec, u8 reg, u8 *buf, u8 len)
{
	int ret;

	ret = slic_spi_write(CSLAC_EC_REG_WRT, &ec, 1);
	if (ret)
		return ret;
	return slic_spi_read(reg, buf, len);
}

static int slic_detect(u8 id[VP886_R_RCNPCN_LEN])
{
	int ret;

	mutex_lock(&sd.lock);
	slic_spi_cfg();
	slic_reset();
	ret = slic_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, id, VP886_R_RCNPCN_LEN);
	mutex_unlock(&sd.lock);
	return ret;
}

static int slic_detect_show(struct seq_file *s, void *unused)
{
	u8 id[VP886_R_RCNPCN_LEN] = { 0xff, 0xff };
	int ret = slic_detect(id);

	seq_printf(s, "RCNPCN read ret=%d rcn=0x%02x pcn=0x%02x (expect rcn=0x%02x pcn=0x%02x)\n",
		   ret, id[0], id[1], VP886_R_RCNPCN_RCN, VP886_R_RCNPCN_PCN_LE9641);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9641)
		seq_puts(s, "Le9641 detected: OK\n");
	else
		seq_puts(s, "Le9641 detect: FAILED\n");

	seq_printf(s, "last_phase=%s\n", slic_where);
	seq_printf(s,
		   "spi post-regs: global=0x%x mode=0x%x init_busy=0x%x enable=0x%x done=0x%x ctl=0x%x ctl_busy=0x%x tx_busy=0x%x rx_busy=0x%x newblk=0x%x cs=0x%x\n",
		   readl(sd.spi + SLIC_GLOBAL), readl(sd.spi + SLIC_MODE),
		   readl(sd.spi + SLIC_INIT_BUSY), readl(sd.spi + SLIC_ENABLE),
		   readl(sd.spi + SLIC_DONE), readl(sd.spi + SLIC_CTL),
		   readl(sd.spi + SLIC_CTL_BUSY), readl(sd.spi + SLIC_TX_BUSY),
		   readl(sd.spi + SLIC_RX_BUSY), readl(sd.spi + SLIC_NEWBLK_EN),
		   readl(sd.spi + SLIC_CS_SELECT));
	seq_printf(s, "sys: spi_mode[0x94]=0x%x gpio[0x834]=0x%x  iomux[0x104]=0x%x\n",
		   readl(sd.sys + SYS_SPI_MODE), readl(sd.sys + SYS_GPIO_DATA),
		   readl(sd.chip_scu + SCU_IOMUX_CONTROL1));

	pr_info(DRV_NAME ": detect ret=%d rcn=0x%02x pcn=0x%02x last_phase=%s\n",
		ret, id[0], id[1], slic_where);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_detect);

static int __init econet_slic_init(void)
{
	sd.chip_scu = ioremap(CHIP_SCU_PHYS, CHIP_SCU_SIZE);
	sd.sys = ioremap(SYS_PHYS, SYS_SIZE);
	sd.spi = ioremap(SLIC_SPI_PHYS, SLIC_SPI_SIZE);
	if (!sd.chip_scu || !sd.sys || !sd.spi)
		goto err;

	mutex_init(&sd.lock);
	sd.cs = 0;

	sd.dbg = debugfs_create_dir(DRV_NAME, NULL);
	debugfs_create_file("slic_detect", 0444, sd.dbg, NULL, &slic_detect_fops);
	debugfs_create_u8("cs", 0644, sd.dbg, &sd.cs);

	pr_info(DRV_NAME ": loaded (cat /sys/kernel/debug/%s/slic_detect to probe Le9641)\n",
		DRV_NAME);
	return 0;
err:
	if (sd.spi)
		iounmap(sd.spi);
	if (sd.sys)
		iounmap(sd.sys);
	if (sd.chip_scu)
		iounmap(sd.chip_scu);
	return -ENOMEM;
}

static void __exit econet_slic_exit(void)
{
	debugfs_remove_recursive(sd.dbg);
	iounmap(sd.spi);
	iounmap(sd.sys);
	iounmap(sd.chip_scu);
}

module_init(econet_slic_init);
module_exit(econet_slic_exit);

MODULE_DESCRIPTION("EcoNet EN751221 Le9641 SLIC SPI/MPI chip detect (phase 3a)");
MODULE_LICENSE("GPL");
