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
#define VP886_R_RCNPCN_PCN_LE9642 0x75	/* le9662.c: le9642 = rcn 0x08 / pcn 0x75 */

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
	/* the wrapper ack only means the wrapper sent; the SLIC needs time to
	 * clock the byte over the PCM bus (8 kHz frames). Without this gap the
	 * SLIC replies garbage (0xf1). 5 ms verified live; ~125 us/frame so safe. */
	usleep_range(5000, 6000);
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
	usleep_range(5000, 6000);			/* inter-byte gap (see zsi_write_byte) */
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
	/* ZSI read sub-command: consumes the framing byte so the first read
	 * byte is the real register data (verified live: 0x06 aligns RCN=0x08). */
	ret = zsi_write_byte(0x06);
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
 * Enable the PCM clock output (SCU 0x1fa200d8). The ZSI control read does NOT
 * need the PCM DMA -- the ZSI wrapper self-clocks. Live testing showed that
 * arming the DMA actually PREVENTED the SLIC from answering (the SLIC replied
 * only with NO DMA running). So we just enable the PCLK output; no rings.
 */
#define SCU_PCM_CLK_OUTPUT	0x0d8
#define SCU_PCM_CLK_OUTPUT_VAL	0x00a00301	/* OEM-stock value */

/*
 * Configure the PCM as master so it generates PCLK/FSYNC for the SLIC:
 * clock output enable + the OEM timeslot table + INTFACE_CTRL (0xf5071306,
 * master mode, no loopback). NO DMA -- arming the DMA was what blocked the
 * SLIC; only this clock-master config is needed for the ZSI control channel.
 */
static void pcm_zsi_clock_setup(void)
{
	int i;

	writel(SCU_PCM_CLK_OUTPUT_VAL, sd.chip_scu + SCU_PCM_CLK_OUTPUT);
	for (i = 0; i < 4; i++) {
		writel(pcm_tx_slots[i], sd.pcm + PCM_TX_TIME_SLOT_CFG0 + i * 4);
		writel(pcm_rx_slots[i], sd.pcm + PCM_RX_TIME_SLOT_CFG0 + i * 4);
	}
	writel(pcm_intface_ctrl, sd.pcm + PCM_INTFACE_CTRL);
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

/*
 * Phase 3b: device profile (Raw MPI section of DEV_PROFILE_100V_BB_124_ZSI).
 * Each entry is an MPI command (opcode + data bytes) written straight to the
 * SLIC over ZSI. Validated by poking: written from the good (post-detect)
 * state with NO HWRESET, the SLIC survives and the Device Mode reads back the
 * written value. The 0xf6/0xe4/0xe6 commands start the buck-boost switcher.
 */
/*
 * Raw MPI sections of the ZLR964124_Le9641 BB profiles (ZSI mode), written
 * straight over ZSI. Each opcode self-delimits (the SLIC parses the stream),
 * so we just stream the bytes. All validated by poking (SLIC survives, device
 * mode reads back, AC 80-byte command accepted).
 */
static const u8 dev_mpi[] = {	/* DEV_PROFILE_100V_BB_124_ZSI: PCLK, slot, dev mode, switcher */
	0x46, 0x02, 0x44, 0x46, 0x5e, 0x14, 0x00, 0xf6, 0x95, 0x00,
	0x58, 0x30, 0x5c, 0x30, 0xe4, 0x44, 0x92, 0x0a, 0xe6, 0x60,
};
static const u8 dc_mpi[] = {	/* DC_FXS_miSLIC_BB_DEF: DC feed */
	0xc6, 0x92, 0x27,
};
static const u8 ac_mpi[] = {	/* AC_FXS_RF14_600R_DEF_LE9641: AC impedance coeffs */
	0xa4, 0x00, 0xf4, 0x4c, 0x01, 0x49, 0xca, 0xf5, 0x98, 0xaa, 0x7b, 0xab,
	0x2c, 0xa3, 0x25, 0xa5, 0x24, 0xb2, 0x3d, 0x9a, 0x2a, 0xaa, 0xa6, 0x9f,
	0x01, 0x8a, 0x1d, 0x01, 0xa3, 0xa0, 0x2e, 0xb2, 0xb2, 0xba, 0xac, 0xa2,
	0xa6, 0xcb, 0x3b, 0x45, 0x88, 0x2a, 0x20, 0x3c, 0xbc, 0x4e, 0xa6, 0x2b,
	0xa5, 0x2b, 0x3e, 0xba, 0x8f, 0x82, 0xa8, 0x71, 0x80, 0xa9, 0xf0, 0x50,
	0x00, 0x86, 0x2a, 0x42, 0xa1, 0xcb, 0x1b, 0xa3, 0xa8, 0xfb, 0x87, 0xaa,
	0xfb, 0x9f, 0xa9, 0xf0, 0x96, 0x2e, 0x01, 0x00,
};
static const u8 ring_mpi[] = {	/* RING_ZL880_BB90V_DEF: ringing generator */
	0xc0, 0x08, 0x00, 0x00, 0x00, 0x44, 0x3a, 0x9d, 0x00, 0x00, 0x00, 0x00,
};

/* Stream one raw MPI section to the SLIC in a single ZSI session. */
static int slic_write_mpi(const char *what, const u8 *b, int n)
{
	int i, ret;

	sd.where = what;
	zsi_begin();
	for (i = 0; i < n; i++) {
		ret = zsi_write_byte(b[i]);
		if (ret)
			return ret;
	}
	return 0;
}

/* Load device + DC + AC + ring profiles (Raw MPI sections). */
static int slic_load_profiles(void)
{
	int ret;

	ret = slic_write_mpi("dev", dev_mpi, sizeof(dev_mpi));
	if (ret)
		return ret;
	ret = slic_write_mpi("dc", dc_mpi, sizeof(dc_mpi));
	if (ret)
		return ret;
	ret = slic_write_mpi("ac", ac_mpi, sizeof(ac_mpi));
	if (ret)
		return ret;
	return slic_write_mpi("ring", ring_mpi, sizeof(ring_mpi));
}

/* Full 3b init: bring-up (3a) + verify + load device profile. */
static int slic_init(u8 id[2], u8 *devmode_out)
{
	int ret;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);

	ret = zsi_mpi_read(VP886_EC_1, VP886_R_RCNPCN_RD, id, 2);
	if (ret)
		goto out;
	ret = slic_load_profiles();
	if (ret)
		goto out;
	/* read the Device Mode register back to confirm the profile took */
	ret = zsi_mpi_read(VP886_EC_1, 0x5f, devmode_out, 1);
out:
	mutex_unlock(&sd.lock);
	return ret;
}

extern int pcm_en751221_capture_rx(u8 *out, int nbytes);

/*
 * SLIC line up for voice. Values are the OEM ground truth (le9641.c) + what
 * Codex proved live to make the earpiece audible:
 *   - switcher HP (0x6f), feed active
 *   - TX slot 4, RX slot 4: OEM uses timeSlotIdx 2 << 1 = 4 for 16-bit LINEAR
 *     (16-bit = 2 8-bit slots). With slot 6 the PCM TX never reaches the bus
 *     (TSA loopback all-zero); slot 4 lands the audio on DMA ch0.
 *   - OPFUNC codec = LINEAR (0x80), NOT u-law -- OEM logs "CODEC_LINEAR".
 *   - GR (receive/earpiece gain, MPI 0x82) = 0x4000 unity; without it the
 *     earpiece RX path is muted.
 */
static int slic_audio_setup(void)
{
	u8 v;
	int ret;

	/* switcher HP (simple direct write -- the robust CALCTRL sequence breaks it) */
	{ static const u8 sw[] = { 0xe6, 0x6f }; ret = slic_write_mpi("sw-hp", sw, sizeof(sw)); if (ret) return ret; }
	/* feed active (no codec yet) */
	ret = zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	if (ret) return ret;
	{ static const u8 st[] = { 0x56, 0x03 }; slic_write_mpi("active", st, sizeof(st)); }

	/* TX slot 4 (mic -> bus, DMA ch0), RX slot 4 (earpiece <- bus) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ static const u8 tx[] = { 0x40, 0x04 }; slic_write_mpi("txslot", tx, sizeof(tx)); }
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ static const u8 rx[] = { 0x42, 0x04 }; slic_write_mpi("rxslot", rx, sizeof(rx)); }

	/* 16-bit LINEAR codec: OPFUNC = (old & ~0xC0) | 0x80 */
	ret = zsi_mpi_read(VP886_EC_1, 0x61, &v, 1);
	if (ret) return ret;
	v = (v & ~0xc0) | 0x80;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ u8 of[2] = { 0x60, v }; slic_write_mpi("opfunc", of, 2); }

	/* GR receive (earpiece) gain = 0x4000 unity (MPI write opcode 0x82) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ static const u8 gr[] = { 0x82, 0x40, 0x00 }; slic_write_mpi("gr", gr, sizeof(gr)); }

	/* OPCOND: clear CUT_TX|CUT_RX|TSA_LOOPBACK */
	ret = zsi_mpi_read(VP886_EC_1, 0x71, &v, 1);
	if (ret) return ret;
	v &= ~0xc4;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond", oc, 2); }

	/* STATE = active + codec (0x23) */
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ static const u8 st[] = { 0x56, 0x23 }; slic_write_mpi("active+codec", st, sizeof(st)); }
	return 0;
}

static int slic_audio_show(struct seq_file *s, void *unused)
{
	u8 buf[80];
	u8 txslot = 0xff, opfunc = 0xff, st = 0xff;
	int ret, i, mn = 255, mx = 0, nonzero = 0;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	/* load profiles, then audio line-up */
	slic_load_profiles();
	ret = slic_audio_setup();
	zsi_mpi_read(VP886_EC_1, 0x41, &txslot, 1);
	zsi_mpi_read(VP886_EC_1, 0x61, &opfunc, 1);
	zsi_mpi_read(VP886_EC_1, 0x57, &st, 1);
	mutex_unlock(&sd.lock);

	memset(buf, 0, sizeof(buf));
	if (!ret)
		ret = pcm_en751221_capture_rx(buf, sizeof(buf));

	for (i = 0; i < (int)sizeof(buf); i++) {
		if (buf[i] < mn) mn = buf[i];
		if (buf[i] > mx) mx = buf[i];
		if (buf[i] != buf[0]) nonzero++;
	}
	seq_printf(s, "slic: txslot=0x%02x opfunc=0x%02x state=0x%02x  capture ret=%d\n",
		   txslot, opfunc, st, ret);
	seq_printf(s, "rx samples min=0x%02x max=0x%02x spread=%d varying=%d/80\n",
		   mn, mx, mx - mn, nonzero);
	seq_printf(s, "first16: %16ph\n", buf);
	if (mx - mn > 4)
		seq_puts(s, "AUDIO present (samples vary -- mic signal captured)\n");
	else
		seq_puts(s, "no audio variation (silence / phone on-hook / wrong slot)\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_audio);

extern int pcm_en751221_capture_allch(u8 *out);
extern int pcm_en751221_play_melody(void);
extern int pcm_en751221_loopback_capture(u8 *out, u32 intface);

/* Persistent line-up: do it once, then rx_scan captures without re-resetting. */
static bool slic_audio_up;

static int slic_audio_setup_show(struct seq_file *s, void *unused)
{
	int ret;
	u8 tx = 0xff, st = 0xff, sig = 0xff;

	mutex_lock(&sd.lock);
	zsi_hw_init();
	zsi_slic_reset();
	writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
	slic_load_profiles();
	ret = slic_audio_setup();
	zsi_mpi_read(VP886_EC_1, 0x41, &tx, 1);
	zsi_mpi_read(VP886_EC_1, 0x57, &st, 1);
	zsi_mpi_read(VP886_EC_1, 0x4d, &sig, 1);
	mutex_unlock(&sd.lock);
	slic_audio_up = (ret == 0);
	seq_printf(s, "audio setup ret=%d  txslot=0x%02x state=0x%02x hook=%d\n",
		   ret, tx, st, sig & 1);
	seq_puts(s, slic_audio_up ? "line UP -- now: cat rx_scan (with noise)\n"
				  : "setup FAILED\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_audio_setup);

/* Scan all 8 RX channels -- show which one carries the SLIC mic audio. */
static int rx_scan_show(struct seq_file *s, void *unused)
{
	static u8 buf[8 * 80];
	int ret, ch, i, mn, mx;

	ret = pcm_en751221_capture_allch(buf);
	seq_printf(s, "capture_allch ret=%d\n", ret);
	for (ch = 0; ch < 8; ch++) {
		u8 *b = buf + ch * 80;

		mn = 255; mx = 0;
		for (i = 0; i < 80; i++) {
			if (b[i] < mn) mn = b[i];
			if (b[i] > mx) mx = b[i];
		}
		seq_printf(s, " ch%d slot? min=0x%02x max=0x%02x spread=%3d %s  %8ph\n",
			   ch, mn, mx, mx - mn,
			   (mx - mn > 4) ? "<== SIGNAL" : "         ", b);
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(rx_scan);

/*
 * Play the melody out the earpiece. Line up the SLIC if needed (audio_setup
 * now bakes the working slot-4 / linear / GR values) then loop the tune ~27s.
 * Does NOT re-poke TXSLOT/RXSLOT/OPFUNC after setup, so live poke experiments
 * survive a play.
 */
static int slic_play_show(struct seq_file *s, void *unused)
{
	int i, ret = 0;

	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	mutex_unlock(&sd.lock);

	for (i = 0; i < 6; i++) {
		ret = pcm_en751221_play_melody();
		if (ret)
			break;
	}
	seq_printf(s, "play_melody loops=%d ret=%d (escuchá el auricular)\n",
		   i, ret);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_play);

/*
 * Digital loopback diagnostic: arm the SLIC TSA loopback (OPCOND bit 0x04) so
 * what the SoC transmits returns on the PCM bus, then TX a ramp + capture RX
 * ch2. A ramp coming back proves the TX digital path/slot map works and the
 * silence is purely the analog earpiece.
 */
static int count_ramp(const u8 *b)
{
	int i, n = 0;

	for (i = 1; i < 80; i++)
		if (b[i] == (u8)(b[i - 1] + 1))
			n++;
	return n;
}

static int slic_loopback_show(struct seq_file *s, void *unused)
{
	u8 out[160], v = 0xff;
	int ret;

	/* TEST 1: PCM-internal loopback (INTFACE bit25), SLIC out of the path.
	 * Proves the TX DMA in this code path actually transmits a ramp. */
	memset(out, 0, sizeof(out));
	ret = pcm_en751221_loopback_capture(out, 0xf5071306 | (1u << 25));
	seq_printf(s, "[internal] ret=%d ramp=%2d/79 %s\n", ret, count_ramp(out),
		   (count_ramp(out) > 60) ? "TX DMA OK" : "TX DMA broken");
	seq_printf(s, "  ch2: %32ph\n", out);

	/* TEST 2: SLIC TSA loopback (INTFACE normal, OPCOND bit 0x04). Proves
	 * the SoC TX -> bus -> SLIC -> bus -> SoC RX path. */
	mutex_lock(&sd.lock);
	if (!slic_audio_up) {
		zsi_hw_init();
		zsi_slic_reset();
		writel(readl(sd.zsi + ZSI_EN) | ZSI_EN_VAL, sd.zsi + ZSI_EN);
		slic_load_profiles();
		slic_audio_up = (slic_audio_setup() == 0);
	}
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ static const u8 rx[] = { 0x42, 0x06 }; slic_write_mpi("rxslot6", rx, sizeof(rx)); }
	zsi_mpi_read(VP886_EC_1, 0x71, &v, 1);
	v |= 0x04;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond-lb", oc, 2); }
	mutex_unlock(&sd.lock);

	memset(out, 0, sizeof(out));
	ret = pcm_en751221_loopback_capture(out, 0xf5071306);

	mutex_lock(&sd.lock);
	zsi_mpi_read(VP886_EC_1, 0x71, &v, 1);
	v &= ~0x04;
	zsi_write(CSLAC_EC_REG_WRT, (const u8[]){ VP886_EC_1 }, 1);
	{ u8 oc[2] = { 0x70, v }; slic_write_mpi("opcond-rst", oc, 2); }
	mutex_unlock(&sd.lock);

	seq_printf(s, "[slic-tsa] ret=%d ramp=%2d/79 %s\n", ret, count_ramp(out),
		   (count_ramp(out) > 60) ? "SLIC BUS PATH OK" : "no ramp via SLIC");
	seq_printf(s, "  ch2: %32ph\n", out);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_loopback);

static int slic_init_show(struct seq_file *s, void *unused)
{
	u8 id[2] = { 0xee, 0xee };
	u8 devmode = 0xee;
	int ret = slic_init(id, &devmode);

	seq_printf(s, "init ret=%d  RCNPCN=%02x %02x (expect 08 75)  devmode=0x%02x (expect 0x14)\n",
		   ret, id[0], id[1], devmode);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9642 && devmode == 0x14)
		seq_puts(s, "Le9642 init + device/DC/AC/ring profiles loaded: OK\n");
	else
		seq_puts(s, "init: FAILED\n");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slic_init);

#define SLIC_DETECT_NBYTES	4

static int slic_detect_show(struct seq_file *s, void *unused)
{
	u8 id[SLIC_DETECT_NBYTES];
	int ret;

	memset(id, 0xee, sizeof(id));
	ret = slic_detect(id, sizeof(id));

	seq_printf(s, "RCNPCN(ZSI) ret=%d bytes= %*ph (expect 0x08 0x75)\n",
		   ret, (int)sizeof(id), id);
	if (!ret && id[0] == VP886_R_RCNPCN_RCN &&
	    id[1] == VP886_R_RCNPCN_PCN_LE9642)
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
	debugfs_create_file("slic_init", 0444, sd.dbg, NULL, &slic_init_fops);
	debugfs_create_file("audio_capture", 0444, sd.dbg, NULL, &slic_audio_fops);
	debugfs_create_file("audio_setup", 0444, sd.dbg, NULL, &slic_audio_setup_fops);
	debugfs_create_file("rx_scan", 0444, sd.dbg, NULL, &rx_scan_fops);
	debugfs_create_file("play", 0444, sd.dbg, NULL, &slic_play_fops);
	debugfs_create_file("tx_loopback", 0444, sd.dbg, NULL, &slic_loopback_fops);
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
