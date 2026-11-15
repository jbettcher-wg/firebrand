/*
 * See frb_uvd_h264.h. The checks are RadeonNI's hw/rdn_uvd_h264.c
 * (valid_config, valid_picture, bitstream_size) unchanged, so a stream the
 * client library accepts is one this accepts; the message and IB are Mesa
 * 26.2.4's ac_uvd_dec.c for Polaris and Vega (uvd_build_create_cmd,
 * uvd_build_decode_cmd, build_avc_msg, ac_uvd_dec_dpb_size,
 * calc_ctx_size_avc) at struct ruvd_msg's byte offsets.
 *
 * One deliberate difference from Mesa 26: decoded_pic_idx is the picture's
 * frame_num, as Mesa's radeonsi radeon_uvd.c had it through the years it
 * drove Polaris (and r600 and RadeonNI still do); Mesa 26 passes a slot
 * index the 'rdnv' protocol does not carry.
 *
 * Copyright 2011, 2025 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_uvd_h264.h"
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <string.h>
#endif

/* struct ruvd_msg byte offsets */
enum {
	MSG_SIZE = 0, MSG_TYPE = 4, MSG_HANDLE = 8, MSG_FEEDBACK_NUMBER = 12,
	/* body.create */
	C_STREAM_TYPE = 16, C_WIDTH = 28, C_HEIGHT = 32, C_DPB_SIZE = 40,
	/* body.decode */
	D_STREAM_TYPE = 16, D_DECODE_FLAGS = 20, D_WIDTH = 24, D_HEIGHT = 28,
	D_DPB_SIZE = 36, D_DPB_RESERVED = 44, D_DB_PITCH = 52, D_BSD_SIZE = 88,
	D_DT_PITCH = 112, D_DT_FIELD_MODE = 124, D_DT_LUMA_TOP = 128, D_DT_CHROMA_TOP = 136,
	D_DT_WA_CHROMA_TOP = 152, D_CODEC = 224, D_EXTENSION_SUPPORT = 3296,
	/* struct ruvd_h264, from D_CODEC */
	H_PROFILE = 0, H_LEVEL = 4, H_SPS_FLAGS = 8, H_PPS_FLAGS = 12, H_CHROMA_FORMAT = 16,
	H_BIT_DEPTH_LUMA = 17, H_BIT_DEPTH_CHROMA = 18, H_LOG2_MAX_FRAME_NUM = 19,
	H_POC_TYPE = 20, H_LOG2_MAX_POC_LSB = 21, H_NUM_REF_FRAMES = 22,
	H_INIT_QP = 24, H_INIT_QS = 25, H_CHROMA_QP_OFFSET = 26, H_SECOND_CHROMA_QP_OFFSET = 27,
	H_NUM_SLICE_GROUPS = 28, H_SLICE_GROUP_MAP_TYPE = 29, H_NUM_REF_IDX_L0 = 30, H_NUM_REF_IDX_L1 = 31,
	H_SLICE_GROUP_CHANGE_RATE = 32, H_SCALING4 = 36, H_SCALING8 = 132, H_FRAME_NUM = 260,
	H_FRAME_NUM_LIST = 264, H_CURR_POC = 328, H_REF_POC = 336, H_DECODED_PIC = 464
};

/*
 * The GPCOM registers, PACKET0 by dword: Mesa's RUVD_GPCOM_VCPU_CMD 0xEF0C,
 * DATA0 0xEF10, DATA1 0xEF14, RUVD_ENGINE_CNTL 0xEF18, RUVD_NO_OP 0xEFFC
 * (bytes) are reg_base 0x3800 + these; its RUVD_*_SOC15 0x2070c, 0x20710,
 * 0x20714, 0x20718 are 0x7e00 + these.
 */
#define RUVD_PKT0(hw, reg)	((uint32_t)((hw)->reg_base + (reg)) & 0xffff)

static void put_le32(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
	out[2] = (uint8_t)(value >> 16);
	out[3] = (uint8_t)(value >> 24);
}

static void put_le16(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)value;
	out[1] = (uint8_t)(value >> 8);
}

static uint64_t align_up(uint64_t value, uint32_t alignment)
{
	return (value + alignment - 1) & ~((uint64_t)alignment - 1);
}

static int zero_words(const uint32_t *p, unsigned count)
{
	unsigned i;

	for (i = 0; i < count; i++)
		if (p[i])
			return 0;
	return 1;
}

/* RadeonNI hw/rdn_uvd_h264.c valid_config, unchanged */
static int valid_config(const struct rdn_uvd_h264_config *c)
{
	uint32_t fs, dpb_mbs;
	uint64_t width_mbs, height_mbs, mbs;

	if (!c || c->version != RDN_UVD_H264_VERSION ||
	    (c->profile_idc != 66 && c->profile_idc != 77 && c->profile_idc != 100) ||
	    !c->coded_width || c->coded_width > RDN_UVD_H264_MAX_WIDTH ||
	    !c->coded_height || c->coded_height > RDN_UVD_H264_MAX_HEIGHT ||
	    (c->coded_width & 15) || (c->coded_height & 15) ||
	    c->max_num_ref_frames > RDN_UVD_H264_MAX_REFS ||
	    c->chroma_format_idc != 1 || c->bit_depth_luma_minus8 ||
	    c->bit_depth_chroma_minus8 || c->frame_mbs_only_flag != 1 ||
	    c->mb_adaptive_frame_field_flag || !zero_words(c->reserved, 3))
		return 0;
	/* H.264 table A-1: MaxFS and MaxDpbMbs. Frame rate/bitrate need stream timing. */
	switch (c->level_idc) {
	case 10: fs = 99; dpb_mbs = 396; break;
	case 11: fs = 396; dpb_mbs = 900; break;
	case 12: case 13: case 20: fs = 396; dpb_mbs = 2376; break;
	case 21: fs = 792; dpb_mbs = 4752; break;
	case 22: case 30: fs = 1620; dpb_mbs = 8100; break;
	case 31: fs = 3600; dpb_mbs = 18000; break;
	case 32: fs = 5120; dpb_mbs = 20480; break;
	case 40: case 41: fs = 8192; dpb_mbs = 32768; break;
	case 42: fs = 8704; dpb_mbs = 34816; break;
	default: return 0;
	}
	width_mbs = c->coded_width / 16;
	height_mbs = c->coded_height / 16;
	mbs = width_mbs * height_mbs;
	return mbs <= fs && width_mbs * width_mbs <= 8u * fs &&
	       height_mbs * height_mbs <= 8u * fs &&
	       mbs * c->max_num_ref_frames <= dpb_mbs;
}

/* RadeonNI hw/rdn_uvd_h264.c valid_picture, unchanged */
int frb_uvd_h264_valid_picture(const struct rdn_uvd_h264_config *c, const struct rdn_uvd_h264_picture *p)
{
	uint32_t max_frame_num;
	unsigned i, j;

	if (!p || p->version != RDN_UVD_H264_VERSION ||
	    (p->sps_info_flags & ~13u) || !(p->sps_info_flags & 4) ||
	    (p->pps_info_flags & ~511u) || ((p->pps_info_flags >> 4) & 3) == 3 ||
	    (c->profile_idc != 100 && (p->pps_info_flags & 1)) ||
	    (c->profile_idc == 66 && (p->pps_info_flags & 256)) ||
	    p->log2_max_frame_num_minus4 > 12 || p->pic_order_cnt_type > 2 ||
	    p->log2_max_pic_order_cnt_lsb_minus4 > 12 ||
	    p->pic_init_qp_minus26 < -26 || p->pic_init_qp_minus26 > 25 ||
	    p->pic_init_qs_minus26 < -26 || p->pic_init_qs_minus26 > 25 ||
	    p->chroma_qp_index_offset < -12 || p->chroma_qp_index_offset > 12 ||
	    p->second_chroma_qp_index_offset < -12 || p->second_chroma_qp_index_offset > 12 ||
	    p->num_slice_groups_minus1 || p->slice_group_map_type ||
	    p->slice_group_change_rate_minus1 || p->num_ref_idx_l0_active_minus1 > 15 ||
	    p->num_ref_idx_l1_active_minus1 > 15 || p->field_pic_flag ||
	    p->bottom_field_flag || p->reference_count > c->max_num_ref_frames ||
	    !zero_words(p->reserved, 4))
		return 0;
	max_frame_num = 1u << (p->log2_max_frame_num_minus4 + 4);
	if (p->frame_num >= max_frame_num)
		return 0;
	for (i = 0; i < RDN_UVD_H264_MAX_REFS; i++) {
		if (p->frame_num_list[i] >= max_frame_num)
			return 0;
		if (i >= p->reference_count &&
		    (p->frame_num_list[i] || p->field_order_cnt_list[i][0] ||
		     p->field_order_cnt_list[i][1]))
			return 0;
	}
	for (i = 0; i < 6; i++)
		for (j = 0; j < 16; j++)
			if (!p->scaling_list_4x4[i][j])
				return 0;
	for (i = 0; i < 2; i++)
		for (j = 0; j < 64; j++)
			if (!p->scaling_list_8x8[i][j])
				return 0;
	return 1;
}

/*
 * The sizes: ac_uvd_dec_dpb_size (Polaris: 17 pictures, the macroblock
 * context and IT surface not in it), calc_ctx_size_avc (with 17
 * references, the most the session can hold), the session context, and
 * a linear target. Mesa's DPB alignment is 16 before Vega and 32 from
 * Vega 10 on (ac_uvd_dec_dpb_alignment), applied to the 16-aligned size.
 */
int frb_uvd_h264_layout_init(const struct frb_uvd_h264_hw *hw, const struct rdn_uvd_h264_config *c,
			     struct frb_uvd_h264_layout *out)
{
	struct frb_uvd_h264_layout l;
	uint64_t image, width_in_mb, height_in_mb, dpb, ctx, pitch, target, packed;

	if (!out || !hw || (hw->dpb_alignment != 16 && hw->dpb_alignment != 32) || !valid_config(c))
		return -1;
	memset(&l, 0, sizeof(l));
	l.hw = *hw;
	image = align_up(align_up(c->coded_width, 16), hw->dpb_alignment) *
		align_up(align_up(c->coded_height, 16), hw->dpb_alignment);
	image += image / 2;
	image = align_up(image, 1024);
	width_in_mb = align_up(c->coded_width, 16) / 16;
	height_in_mb = align_up(align_up(c->coded_height, 16) / 16, 2);
	dpb = image * FRB_UVD_H264_MAX_REFS;
	ctx = FRB_UVD_H264_MAX_REFS * align_up(width_in_mb * height_in_mb * 192, 256);
	pitch = align_up(c->coded_width, 256);
	target = pitch * c->coded_height * 3 / 2;
	packed = (uint64_t)c->coded_width * c->coded_height * 3 / 2;
	if (align_up(dpb, 4096) > 0x7fffffff || align_up(ctx, 4096) > 0x7fffffff ||
	    align_up(target, 4096) > 0x7fffffff)
		return -1;
	l.width = c->coded_width;
	l.height = c->coded_height;
	l.db_pitch = (uint32_t)align_up(c->coded_width, hw->dpb_alignment);
	l.image_bytes = (uint32_t)image;
	l.dpb_bytes = (uint32_t)dpb;
	l.ctx_bytes = (uint32_t)ctx;
	l.session_bytes = FRB_UVD_H264_SESSION_BYTES;
	l.target_pitch = (uint32_t)pitch;
	l.target_rows = c->coded_height;
	l.chroma_offset = (uint32_t)(pitch * c->coded_height);
	l.target_bytes = (uint32_t)target;
	l.packed_nv12_bytes = (uint32_t)packed;
	*out = l;
	return 0;
}

int frb_uvd_h264_bitstream_size(uint32_t bytes, uint32_t *padded)
{
	if (!padded || !bytes || bytes > RDN_UVD_H264_MAX_BITSTREAM_BYTES)
		return -1;
	*padded = (uint32_t)align_up(bytes, 128);
	return 0;
}

static void message_init(uint8_t *out, uint32_t type, uint32_t handle)
{
	memset(out, 0, FRB_UVD_H264_MSG_BYTES);
	put_le32(out + MSG_SIZE, FRB_UVD_H264_MSG_BYTES);
	put_le32(out + MSG_TYPE, type);
	put_le32(out + MSG_HANDLE, handle);
}

/*
 * uvd_build_create_cmd: the stream type and the largest picture; and the
 * DPB size, which Mesa 26 leaves out but radeonsi's radeon_uvd.c (the code
 * that drove Polaris for years) and the kernel's own create message send.
 */
int frb_uvd_h264_pack_create(const struct rdn_uvd_h264_config *c, const struct frb_uvd_h264_layout *l,
			     uint32_t handle, uint8_t *out)
{
	if (!out || !handle || !l || !valid_config(c) || l->width != c->coded_width ||
	    l->height != c->coded_height)
		return -1;
	message_init(out, RDN_UVD_H264_MSG_CREATE, handle);
	put_le32(out + C_STREAM_TYPE, FRB_UVD_CODEC_H264_PERF);
	put_le32(out + C_WIDTH, c->coded_width);
	put_le32(out + C_HEIGHT, c->coded_height);
	put_le32(out + C_DPB_SIZE, l->dpb_bytes);
	return 0;
}

int frb_uvd_h264_pack_destroy(uint32_t handle, uint8_t *out)
{
	if (!out || !handle)
		return -1;
	message_init(out, RDN_UVD_H264_MSG_DESTROY, handle);
	return 0;
}

/* uvd_build_decode_cmd + build_avc_msg */
int frb_uvd_h264_pack_decode(const struct rdn_uvd_h264_config *c, const struct rdn_uvd_h264_picture *p,
			     const struct frb_uvd_h264_layout *l, uint32_t handle, uint32_t feedback,
			     uint32_t bitstream_bytes, uint8_t *out)
{
	uint8_t *h;
	uint32_t padded;
	unsigned i;

	if (!out || !handle || !feedback || !l || !valid_config(c) || l->width != c->coded_width ||
	    l->height != c->coded_height || !frb_uvd_h264_valid_picture(c, p) ||
	    frb_uvd_h264_bitstream_size(bitstream_bytes, &padded))
		return -1;
	message_init(out, RDN_UVD_H264_MSG_DECODE, handle);
	put_le32(out + MSG_FEEDBACK_NUMBER, feedback);
	put_le32(out + D_STREAM_TYPE, FRB_UVD_CODEC_H264_PERF);
	put_le32(out + D_DECODE_FLAGS, 1);
	put_le32(out + D_WIDTH, c->coded_width);
	put_le32(out + D_HEIGHT, c->coded_height);
	put_le32(out + D_BSD_SIZE, padded);
	put_le32(out + D_DPB_SIZE, l->dpb_bytes);
	put_le32(out + D_DPB_RESERVED, l->ctx_bytes);
	put_le32(out + D_DB_PITCH, l->db_pitch);
	out[D_EXTENSION_SUPPORT] = 1;
	put_le32(out + D_DT_PITCH, l->target_pitch);
	put_le32(out + D_DT_FIELD_MODE, 0);
	put_le32(out + D_DT_LUMA_TOP, 0);
	put_le32(out + D_DT_CHROMA_TOP, l->chroma_offset);
	/* family >= CHIP_STONEY */
	put_le32(out + D_DT_WA_CHROMA_TOP, l->target_pitch / 2);
	/* GFX9: dt_wa_chroma_bottom_offset is the target's swizzle mode; linear (ADDR_SW_LINEAR) is 0, as stored */

	h = out + D_CODEC;
	put_le32(h + H_PROFILE, c->profile_idc == 66 ? 0 : c->profile_idc == 77 ? 1 : 2);
	put_le32(h + H_LEVEL, c->level_idc);
	put_le32(h + H_SPS_FLAGS, p->sps_info_flags);
	put_le32(h + H_PPS_FLAGS, p->pps_info_flags);
	h[H_CHROMA_FORMAT] = (uint8_t)c->chroma_format_idc;
	h[H_BIT_DEPTH_LUMA] = (uint8_t)c->bit_depth_luma_minus8;
	h[H_BIT_DEPTH_CHROMA] = (uint8_t)c->bit_depth_chroma_minus8;
	h[H_LOG2_MAX_FRAME_NUM] = (uint8_t)p->log2_max_frame_num_minus4;
	h[H_POC_TYPE] = (uint8_t)p->pic_order_cnt_type;
	h[H_LOG2_MAX_POC_LSB] = (uint8_t)p->log2_max_pic_order_cnt_lsb_minus4;
	h[H_NUM_REF_FRAMES] = (uint8_t)c->max_num_ref_frames;
	h[H_INIT_QP] = (uint8_t)p->pic_init_qp_minus26;
	h[H_INIT_QS] = (uint8_t)p->pic_init_qs_minus26;
	h[H_CHROMA_QP_OFFSET] = (uint8_t)p->chroma_qp_index_offset;
	h[H_SECOND_CHROMA_QP_OFFSET] = (uint8_t)p->second_chroma_qp_index_offset;
	h[H_NUM_SLICE_GROUPS] = (uint8_t)p->num_slice_groups_minus1;
	h[H_SLICE_GROUP_MAP_TYPE] = (uint8_t)p->slice_group_map_type;
	h[H_NUM_REF_IDX_L0] = (uint8_t)p->num_ref_idx_l0_active_minus1;
	h[H_NUM_REF_IDX_L1] = (uint8_t)p->num_ref_idx_l1_active_minus1;
	put_le16(h + H_SLICE_GROUP_CHANGE_RATE, p->slice_group_change_rate_minus1);
	memcpy(h + H_SCALING4, p->scaling_list_4x4, sizeof(p->scaling_list_4x4));
	memcpy(h + H_SCALING8, p->scaling_list_8x8, sizeof(p->scaling_list_8x8));
	put_le32(h + H_FRAME_NUM, p->frame_num);
	for (i = 0; i < 16; i++) {
		put_le32(h + H_FRAME_NUM_LIST + i * 4, p->frame_num_list[i]);
		put_le32(h + H_REF_POC + i * 8, (uint32_t)p->field_order_cnt_list[i][0]);
		put_le32(h + H_REF_POC + i * 8 + 4, (uint32_t)p->field_order_cnt_list[i][1]);
	}
	put_le32(h + H_CURR_POC, (uint32_t)p->field_order_cnt[0]);
	put_le32(h + H_CURR_POC + 4, (uint32_t)p->field_order_cnt[1]);
	put_le32(h + H_DECODED_PIC, p->frame_num);
	return 0;
}

/* build_avc_msg's ruvd_avc_its (H264_PERF): the 4x4 lists, then the 8x8 */
void frb_uvd_h264_pack_its(const struct rdn_uvd_h264_picture *p, uint8_t *out)
{
	memset(out, 0, FRB_UVD_H264_ITS_BYTES);
	memcpy(out, p->scaling_list_4x4, sizeof(p->scaling_list_4x4));
	memcpy(out + sizeof(p->scaling_list_4x4), p->scaling_list_8x8, sizeof(p->scaling_list_8x8));
}

/* send_cmd: DATA0 = low address, DATA1 = high, CMD = cmd << 1 */
static void send_cmd_hw(const struct frb_uvd_h264_hw *hw, uint32_t *w, uint32_t *n, uint32_t cmd, uint64_t va)
{
	w[(*n)++] = RUVD_PKT0(hw, FRB_UVD_H264_GPCOM_VCPU_DATA0);
	w[(*n)++] = (uint32_t)va;
	w[(*n)++] = RUVD_PKT0(hw, FRB_UVD_H264_GPCOM_VCPU_DATA1);
	w[(*n)++] = (uint32_t)(va >> 32);
	w[(*n)++] = RUVD_PKT0(hw, FRB_UVD_H264_GPCOM_VCPU_CMD);
	w[(*n)++] = cmd << 1;
}
#define send_cmd(w, n, cmd, va)	send_cmd_hw(hw, (w), (n), (cmd), (va))

uint32_t frb_uvd_h264_build_ib(uint32_t type, const struct frb_uvd_h264_hw *hw,
			       const struct frb_uvd_h264_buffers *b, uint32_t out[FRB_UVD_H264_IB_MAX_DW])
{
	uint32_t n = 0;

	if (!hw || !b || !b->msg || (b->msg & 63) || type > RDN_UVD_H264_MSG_DESTROY)
		return 0;
	if (type == RDN_UVD_H264_MSG_DESTROY) {
		send_cmd(out, &n, FRB_UVD_CMD_MSG_BUFFER, b->msg);
	} else if (type == RDN_UVD_H264_MSG_CREATE) {
		if (!b->session)
			return 0;
		send_cmd(out, &n, FRB_UVD_CMD_SESSION_CONTEXT, b->session);
		send_cmd(out, &n, FRB_UVD_CMD_MSG_BUFFER, b->msg);
	} else {
		if (!b->session || !b->dpb || !b->ctx || !b->its || !b->feedback || !b->bitstream ||
		    !b->target)
			return 0;
		send_cmd(out, &n, FRB_UVD_CMD_SESSION_CONTEXT, b->session);
		send_cmd(out, &n, FRB_UVD_CMD_MSG_BUFFER, b->msg);
		send_cmd(out, &n, FRB_UVD_CMD_DPB_BUFFER, b->dpb);
		send_cmd(out, &n, FRB_UVD_CMD_CONTEXT_BUFFER, b->ctx);
		send_cmd(out, &n, FRB_UVD_CMD_ITSCALING_TABLE, b->its);
		send_cmd(out, &n, FRB_UVD_CMD_FEEDBACK_BUFFER, b->feedback);
		send_cmd(out, &n, FRB_UVD_CMD_BITSTREAM_BUFFER, b->bitstream);
		send_cmd(out, &n, FRB_UVD_CMD_DECODING_TARGET, b->target);
		out[n++] = RUVD_PKT0(hw, FRB_UVD_H264_ENGINE_CNTL);
		out[n++] = 1;
	}
	while (n & 15) {
		out[n++] = RUVD_PKT0(hw, FRB_UVD_H264_NO_OP);
		out[n++] = 0;
	}
	return n;
}
