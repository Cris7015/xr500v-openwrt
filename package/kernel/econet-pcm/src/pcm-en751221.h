/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Register map for the EcoNet/TrendChip EN751221 PCM/TDM controller.
 *
 * Reconstructed from the OEM Trendchip 2.6.36 pcmdriver.h plus disassembly of
 * the stock pcm1.ko (mips-linux-gnu-objdump). The OEM header used KSEG1 virtual
 * addresses (0xbfbd0000 ...); here we use physical addresses since the driver
 * ioremaps them / takes the block from the DT reg resource.
 *
 * Two instances exist on the SoC:
 *   PCM0 @ phys 0x1fbd0000  (IRQ 12, reset bit 0x800)
 *   PCM1 @ phys 0x1fbd2000  (IRQ 34, reset bit 0x10)
 */
#ifndef _PCM_EN751221_H
#define _PCM_EN751221_H

#include <linux/bits.h>

/*
 * PCM register file: 17 sequential 32-bit registers at base + n*4.
 * Default (reset) values are from the OEM enum pcm_reg_num comments.
 */
enum pcm_reg {
	PCM_INTFACE_CTRL = 0,		/* default 0x0100040a */
	PCM_TX_TIME_SLOT_CFG0,		/* default 0x00080000 */
	PCM_TX_TIME_SLOT_CFG1,		/* default 0x00180010 */
	PCM_TX_TIME_SLOT_CFG2,		/* default 0x00280020 */
	PCM_TX_TIME_SLOT_CFG3,		/* default 0x00380030 */
	PCM_RX_TIME_SLOT_CFG0,		/* default 0x00080000 */
	PCM_RX_TIME_SLOT_CFG1,		/* default 0x00180010 */
	PCM_RX_TIME_SLOT_CFG2,		/* default 0x00280020 */
	PCM_RX_TIME_SLOT_CFG3,		/* default 0x00380030 */
	PCM_ISR,			/* 0x24 - interrupt status */
	PCM_IMR,			/* 0x28 - interrupt mask */
	PCM_TX_POLLING_DEMAND,		/* 0x2c */
	PCM_RX_POLLING_DEMAND,		/* 0x30 */
	PCM_TX_DESC_RING_BASE,		/* 0x34 */
	PCM_RX_DESC_RING_BASE,		/* 0x38 */
	PCM_TX_RX_DESC_RING_SIZE_OFFSET,/* 0x3c default 0x000000c0 */
	PCM_TX_RX_DMA_CTRL,		/* 0x40 default 0x0f000000 */
	PCM_REG_COUNT
};

#define PCM_REG(n)		((n) * 4)

/* DMA control bits (PCM_TX_RX_DMA_CTRL @ 0x40), from disassembly of dmaEnable() */
#define PCM_DMA_TX_EN		BIT(0)
#define PCM_DMA_RX_EN		BIT(1)

/* Interrupt bits (PCM_ISR / PCM_IMR) */
#define PCM_INT_TX_RX_FRAME_BOUNDARY	BIT(0)
#define PCM_INT_TX_DESC_UPDATE		BIT(2)
#define PCM_INT_RX_DESC_UPDATE		BIT(3)
#define PCM_INT_END_OF_TX_DESC		BIT(4)
#define PCM_INT_END_OF_RX_DESC		BIT(5)
#define PCM_INT_TX_BUF_UNDER_RUN	BIT(6)
#define PCM_INT_RX_BUF_OVER_RUN		BIT(7)
#define PCM_INT_AHB_BUS_ERR		BIT(8)

/*
 * SoC SCU block (DT: scu syscon@1fb00000, 0x970 long).
 * Holds the chip-id and the PCM soft-reset bit.
 */
#define SCU_PHYS_BASE		0x1fb00000
#define SCU_CHIP_ID		0x064	/* (val & 0xffff0000) == 0x00070000 -> EN751221 */
#define SCU_PCM_RESET		0x834	/* bit 0x800 resets PCM0 */

#define SCU_CHIP_ID_MASK	0xffff0000
#define SCU_CHIP_EN751221	0x00070000
#define SCU_CHIP_EN7526C	0x00080000
#define SCU_PCM0_RESET_BIT	0x800

/*
 * Chip SCU block (DT: chip_scu syscon@1fa20000, 0x388 long).
 * Holds the PCM/SLIC pinmux and clock source select. Used from phase 2.
 */
#define CHIP_SCU_PHYS_BASE	0x1fa20000
#define CHIP_SCU_IOMUX_CONTROL1	0x104
#define CHIP_SCU_PCM_CLK_OUTPUT	0x0d8
#define CHIP_SCU_PCM_CLK_SRC_SEL 0x148

#define MAX_BUF_NUM		8
#define MAX_CH_NUM		8

/* TX descriptor status word (PCM_TX/RX_DESC_RING_BASE entries). */
union pcm_desc_status {
	struct {
		u32 ownership : 1;
		u32 reserved : 7;
		u32 ch_valid : 8;
		u32 reserved2 : 6;
		u32 sample_size : 10;
	} bits;
	u32 value;
};

/* One DMA descriptor: status + up to MAX_BUF_NUM buffer pointers. */
struct pcm_desc {
	u32 status;
	u32 buf_addr[MAX_BUF_NUM];
};

/* Time-slot config register layout (PCM_TX/RX_TIME_SLOT_CFGn). */
union pcm_slot_cfg {
	struct {
		u32 reserved : 3;
		u32 bit_width_next : 1;
		u32 reserved2 : 2;
		u32 bit_counter_next : 10;
		u32 reserved3 : 3;
		u32 bit_width : 1;
		u32 reserved4 : 2;
		u32 bit_counter : 10;
	} bits;
	u32 value;
};

#endif /* _PCM_EN751221_H */
