// SPDX-License-Identifier: GPL-2.0-only
/*
 * Software-only integration test for the XR500v GPON PLOAM core.
 *
 * This module deliberately has no hardware-facing code.  It verifies the
 * normalized FIFO-word contract and drives synthetic OLT messages through
 * O1 -> O2 -> O3 -> O4 -> O5 -> O6 using callback-only state.
 */

#include <linux/errno.h>
#include <linux/module.h>
#include <linux/string.h>

#include "xr500v_gpon_fifo.h"
#include "xr500v_ploam.h"

#define LAB_ONU_ID	0x38
#define LAB_EQD		0x00001234

struct lab_ctx {
	enum gpon_state state;
	u8 onu_id;
	u8 guard_bits;
	u32 byte_delay;
	u32 bit_delay;
	u32 state_changes;
	u32 overhead_calls;
	u32 set_onu_id_calls;
	u32 set_eqd_calls;
	u32 adjust_eqd_calls;
	u32 fec_calls;
	u32 t3_calls;
};

static int lab_expect(bool condition, const char *what)
{
	if (condition)
		return 0;

	pr_err("xr500v-gpon-ploam-lab: failed: %s\n", what);
	return -EINVAL;
}

static void lab_send_upstream(void *priv, const struct ploam_msg *msg,
			      int times)
{
	(void)priv;
	(void)msg;
	(void)times;
}

static void lab_set_onu_id(void *priv, u8 onu_id)
{
	struct lab_ctx *ctx = priv;

	ctx->onu_id = onu_id;
	ctx->set_onu_id_calls++;
}

static void lab_set_eqd_o4(void *priv, u32 byte_delay, u32 bit_delay)
{
	struct lab_ctx *ctx = priv;

	ctx->byte_delay = byte_delay;
	ctx->bit_delay = bit_delay;
	ctx->set_eqd_calls++;
}

static void lab_adjust_eqd_o5(void *priv, u32 new_eqd)
{
	struct lab_ctx *ctx = priv;

	ctx->byte_delay = new_eqd & ~7u;
	ctx->bit_delay = new_eqd & 7u;
	ctx->adjust_eqd_calls++;
}

static void lab_enable_us_fec(void *priv)
{
	struct lab_ctx *ctx = priv;

	ctx->fec_calls++;
}

static void lab_set_overhead(void *priv, u8 guard_bits, u8 t1_pbits,
			     u8 t2_pbits, u8 t3_pbits, const u8 delim[3],
			     bool delay_mode, u16 delay_time)
{
	struct lab_ctx *ctx = priv;

	ctx->guard_bits = guard_bits;
	ctx->overhead_calls++;
	(void)t1_pbits;
	(void)t2_pbits;
	(void)t3_pbits;
	(void)delim;
	(void)delay_mode;
	(void)delay_time;
}

static void lab_set_t3_preamble(void *priv, u8 o3_t3, u8 o5_t3)
{
	struct lab_ctx *ctx = priv;

	ctx->t3_calls++;
	(void)o3_t3;
	(void)o5_t3;
}

static void lab_set_key_switch_time(void *priv, u32 superframe)
{
	(void)priv;
	(void)superframe;
}

static void lab_request_new_key(void *priv)
{
	(void)priv;
}

static void lab_set_ber_interval(void *priv, u32 interval_ms)
{
	(void)priv;
	(void)interval_ms;
}

static void lab_set_omci_gem(void *priv, u16 gem_port_id, bool valid)
{
	(void)priv;
	(void)gem_port_id;
	(void)valid;
}

static void lab_set_gem_encryption(void *priv, u16 port_id, u8 mode)
{
	(void)priv;
	(void)port_id;
	(void)mode;
}

static void lab_set_alloc_id(void *priv, u16 alloc_id, bool allocate)
{
	(void)priv;
	(void)alloc_id;
	(void)allocate;
}

static void lab_state_changed(void *priv, enum gpon_state state)
{
	struct lab_ctx *ctx = priv;

	ctx->state = state;
	ctx->state_changes++;
}

static void lab_deactivate(void *priv)
{
	(void)priv;
}

static const struct ploam_ops lab_ops = {
	.send_upstream = lab_send_upstream,
	.set_onu_id = lab_set_onu_id,
	.set_eqd_o4 = lab_set_eqd_o4,
	.adjust_eqd_o5 = lab_adjust_eqd_o5,
	.enable_us_fec = lab_enable_us_fec,
	.set_overhead = lab_set_overhead,
	.set_t3_preamble = lab_set_t3_preamble,
	.set_key_switch_time = lab_set_key_switch_time,
	.request_new_key = lab_request_new_key,
	.set_ber_interval = lab_set_ber_interval,
	.set_omci_gem = lab_set_omci_gem,
	.set_gem_encryption = lab_set_gem_encryption,
	.set_alloc_id = lab_set_alloc_id,
	.state_changed = lab_state_changed,
	.deactivate = lab_deactivate,
};

static void lab_pack(struct ploam_msg *msg, u8 onu_id, u8 type,
		     const u8 content[PLOAM_CONTENT_LEN])
{
	msg->value[0] = ((u32)onu_id << 24) | ((u32)type << 16) |
			((u32)content[0] << 8) | content[1];
	msg->value[1] = ((u32)content[2] << 24) |
			((u32)content[3] << 16) |
			((u32)content[4] << 8) | content[5];
	msg->value[2] = ((u32)content[6] << 24) |
			((u32)content[7] << 16) |
			((u32)content[8] << 8) | content[9];
}

static int lab_test_fifo_normalization(void)
{
	const u32 normalized = 0xff012000;
	const u32 c1_logged_after_wrong_swap = 0x002001ff;
	const u32 en7523_raw_inferred = swab32(c1_logged_after_wrong_swap);
	u32 word;
	int ret;

	/*
	 * Commit c1bfa842 logged 0x002001ff after be32_to_cpu().  Undoing that
	 * conversion gives the raw EN7523 register value 0xff012000, already in
	 * the numeric layout expected by the PLOAM core.  Commit 4ed3e5fb's
	 * removal of the conversion is therefore consistent with this capture.
	 */
	word = xr500v_gpon_fifo_normalize(en7523_raw_inferred,
					  XR500V_GPON_FIFO_WORD_NATIVE);
	ret = lab_expect(word == normalized,
			 "EN7523 raw FIFO-word reconstruction");
	if (ret)
		return ret;

	word = xr500v_gpon_fifo_normalize(c1_logged_after_wrong_swap,
					  XR500V_GPON_FIFO_WORD_SWAB32);
	ret = lab_expect(word == normalized,
			 "c1 bootlog wrong-swap reproduction");
	if (ret)
		return ret;

	word = xr500v_gpon_fifo_denormalize(normalized,
					    XR500V_GPON_FIFO_WORD_NATIVE);
	return lab_expect(word == en7523_raw_inferred,
			 "FIFO TX denormalization symmetry");
}

static int lab_test_activation_fsm(void)
{
	static const u8 serial[8] = { 'X', 'R', '5', '0', '0', 'V', '0', '1' };
	static const u8 password[10] = { 0 };
	struct lab_ctx ctx = { };
	struct ploam_priv *ploam;
	struct ploam_msg msg;
	u8 content[PLOAM_CONTENT_LEN] = { };
	int ret;

	ploam = ploam_alloc(&lab_ops, &ctx, serial, password);
	if (!ploam)
		return -ENOMEM;

	ret = lab_expect(ploam_get_state(ploam) == GPON_O1_INITIAL,
			 "initial O1 state");
	if (ret)
		goto out;

	ploam_start(ploam);
	ret = lab_expect(ploam_get_state(ploam) == GPON_O2_STANDBY &&
			 ctx.state == GPON_O2_STANDBY,
			 "O1 to O2 start transition");
	if (ret)
		goto out;

	/* Normalized form of the Upstream_Overhead vector in Matheus' log. */
	msg.value[0] = 0xff012000;
	msg.value[1] = 0x00aaab59;
	msg.value[2] = 0x83200000;
	ploam_handle_downstream(ploam, &msg);
	ret = lab_expect(ploam_get_state(ploam) == GPON_O3_SERIAL_NUMBER &&
			 ctx.overhead_calls == 1 && ctx.guard_bits == 0x20,
			 "broadcast Upstream_Overhead moves O2 to O3");
	if (ret)
		goto out;

	/* OEM semantics: process copies 1 and 4, suppress copies 2 and 3. */
	memset(content, 0, sizeof(content));
	content[0] = 0x18;
	content[1] = 0x20;
	lab_pack(&msg, PLOAM_ONU_BCAST, PLOAM_DOWN_EXTENDED_BURST_LEN,
		 content);
	ploam_handle_downstream(ploam, &msg);
	ploam_handle_downstream(ploam, &msg);
	ploam_handle_downstream(ploam, &msg);
	ploam_handle_downstream(ploam, &msg);
	ret = lab_expect(ctx.t3_calls == 2,
			 "first-of-three PLOAM deduplication");
	if (ret)
		goto out;

	memset(content, 0, sizeof(content));
	content[0] = LAB_ONU_ID;
	memcpy(&content[1], serial, sizeof(serial));
	lab_pack(&msg, PLOAM_ONU_BCAST, PLOAM_DOWN_ASSIGN_ONU_ID, content);
	ploam_handle_downstream(ploam, &msg);
	ret = lab_expect(ploam_get_state(ploam) == GPON_O4_RANGING &&
			 ctx.onu_id == LAB_ONU_ID && ctx.set_onu_id_calls == 1,
			 "Assign_ONU-ID moves O3 to O4");
	if (ret)
		goto out;

	memset(content, 0, sizeof(content));
	content[3] = 0x12;
	content[4] = 0x34;
	lab_pack(&msg, LAB_ONU_ID, PLOAM_DOWN_RANGING_TIME, content);
	ploam_handle_downstream(ploam, &msg);
	ret = lab_expect(ploam_get_state(ploam) == GPON_O5_OPERATION &&
			 ctx.set_eqd_calls == 1 && ctx.fec_calls == 1 &&
			 ctx.byte_delay == (LAB_EQD & ~7u) &&
			 ctx.bit_delay == (LAB_EQD & 7u),
			 "Ranging_Time moves O4 to O5 with EqD and FEC");
	if (ret)
		goto out;

	/*
	 * Change only content[4], the seventh protocol byte.  The XR500v OEM
	 * compares it for Ranging_Time deduplication; the upstream draft did not.
	 */
	content[4] = 0x35;
	lab_pack(&msg, LAB_ONU_ID, PLOAM_DOWN_RANGING_TIME, content);
	ploam_handle_downstream(ploam, &msg);
	ret = lab_expect(ctx.adjust_eqd_calls == 1 &&
			 ploam_get_eqd(ploam) == 0x00001235 &&
			 ctx.byte_delay == 0x00001230 && ctx.bit_delay == 5,
			 "Ranging_Time seventh-byte dedup boundary");
	if (ret)
		goto out;

	ploam_notify_los(ploam);
	ret = lab_expect(ploam_get_state(ploam) == GPON_O6_POPUP &&
			 ctx.state == GPON_O6_POPUP && ctx.state_changes == 5,
			 "LOS moves O5 to O6");

out:
	ploam_free(ploam);
	return ret;
}

static int __init xr500v_gpon_ploam_lab_init(void)
{
	int ret;

	ret = lab_test_fifo_normalization();
	if (ret)
		return ret;

	ret = lab_test_activation_fsm();
	if (ret)
		return ret;

	pr_info("xr500v-gpon-ploam-lab: PASS, software-only O1-O6 and OEM dedup vectors; no hardware access\n");
	return 0;
}

static void __exit xr500v_gpon_ploam_lab_exit(void)
{
	pr_info("xr500v-gpon-ploam-lab: unloaded\n");
}

module_init(xr500v_gpon_ploam_lab_init);
module_exit(xr500v_gpon_ploam_lab_exit);

MODULE_AUTHOR("XR500v OpenWrt project");
MODULE_DESCRIPTION("Software-only EN751221 GPON PLOAM integration lab");
MODULE_LICENSE("GPL");
