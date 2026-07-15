/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XR500V_GPON_FIFO_H
#define _XR500V_GPON_FIFO_H

#include <linux/swab.h>
#include <linux/types.h>

/*
 * MMIO accessor endianness and PLOAM byte lanes are separate properties.
 * Keep the post-read normalization explicit in per-SoC match data instead of
 * inferring it from the CPU endianness.  The protocol core always receives a
 * numeric word with ONU-ID in bits 31:24 and message type in bits 23:16.
 */
enum xr500v_gpon_fifo_word_order {
	XR500V_GPON_FIFO_WORD_NATIVE,
	XR500V_GPON_FIFO_WORD_SWAB32,
};

static inline u32
xr500v_gpon_fifo_normalize(u32 raw,
			   enum xr500v_gpon_fifo_word_order order)
{
	return order == XR500V_GPON_FIFO_WORD_SWAB32 ? swab32(raw) : raw;
}

static inline u32
xr500v_gpon_fifo_denormalize(u32 word,
			     enum xr500v_gpon_fifo_word_order order)
{
	return xr500v_gpon_fifo_normalize(word, order);
}

#endif /* _XR500V_GPON_FIFO_H */
