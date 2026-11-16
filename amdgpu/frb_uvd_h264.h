/*
 * H.264 decode messages for UVD 3.1 (SI), UVD 6 (Polaris) and UVD 7 (Vega 20), behind
 * RadeonNI's 'rdnv' protocol: the client sends the same host-order
 * rdn_uvd_h264_config and rdn_uvd_h264_picture it sends RadeonNI
 * (RadeonNI's hw/rdn_uvd_h264.h, checked here exactly as there), and
 * this file turns them into what the firmware takes, after Mesa 26.2.4's
 * ac_uvd_dec.c (the arm for CHIP_POLARIS10 and later):
 *
 *  - stream type H264_PERF (7), as Mesa uses from Tonga on;
 *  - a 128 KB session context buffer (RUVD_CMD_SESSION_CONTEXT_BUFFER) sent
 *    with the create and every decode message;
 *  - the macroblock context in a context buffer of its own
 *    (RUVD_CMD_CONTEXT_BUFFER, msg dpb_reserved = its size: Linux's
 *    use_ctx_buf), so the DPB is 17 pictures and nothing else;
 *  - the scaling lists also in an IT scaling table (RUVD_CMD_ITSCALING_TABLE_BUFFER);
 *  - a linear progressive NV12 target (dt_pitch, chroma at
 *    dt_chroma_top_offset, dt_wa_chroma_top_offset = dt_pitch / 2 from
 *    Stoney on), db_pitch the width aligned to 16.
 *
 * Before Tonga (UVD 3.1 on SI: struct frb_uvd_h264_hw.legacy), Mesa's other
 * arm, which radeonsi's radeon_uvd.c and RadeonNI's hw/rdn_uvd_h264.c (Turks)
 * build the same way:
 *  - stream type H264 (0);
 *  - no session context buffer, no context buffer, no IT scaling table: the
 *    DPB holds the 17 pictures, the macroblock context (17 x mbs x 192) and
 *    the IT surface (mbs x 32), ac_uvd_dec_dpb_size's family < POLARIS10
 *    arm; dpb_reserved 0; the scaling lists only in the message;
 *  - no dt_wa_chroma_top_offset (family < CHIP_STONEY);
 *  - the IB names msg, DPB, feedback, bitstream and target (Mesa 26's order).
 *
 * What Mesa does differently from CHIP_VEGA10 on (struct frb_uvd_h264_hw):
 *  - the GPCOM registers at their SOC15 addresses (RUVD_*_SOC15: the UVD
 *    instance's segment-1 base + the same offsets), so the IB's PACKET0s
 *    take the register base of the instance the session decodes on;
 *  - ac_uvd_dec_dpb_alignment is 32, not 16: a DPB picture is
 *    align(width, 32) x align(height, 32), and db_pitch align(width, 32);
 *  - on GFX9 dt_pitch is the surface's pitch and
 *    dt_wa_chroma_bottom_offset its swizzle mode: 0 here, ADDR_SW_LINEAR,
 *    for the same linear target (pitch a multiple of 256).
 *
 * Every message field is stored little-endian at its byte offset in
 * struct ruvd_msg; the compiler's layout and the host's byte order never
 * enter. No hardware access, no allocation: the kext does those.
 *
 * Copyright 2011, 2025 Advanced Micro Devices, Inc. (message layout, sizes)
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_UVD_H264_H
#define FRB_UVD_H264_H

#include <stdint.h>
#include <stddef.h>
#include "rdn_uvd_h264.h"

#define FRB_UVD_H264_MSG_BYTES		3556u	/* sizeof(struct ruvd_msg), its size field */
#define FRB_UVD_H264_SESSION_BYTES	(128u * 1024u)	/* UVD_SESSION_CONTEXT_SIZE */
#define FRB_UVD_H264_FEEDBACK_BYTES	2048u	/* FB_BUFFER_SIZE (Polaris: x1) */
#define FRB_UVD_H264_ITS_BYTES		992u	/* IT_SCALING_TABLE_SIZE */
#define FRB_UVD_H264_MAX_REFS		17u	/* NUM_H264_REFS + 1: the DPB and the context */
#define FRB_UVD_H264_IB_MAX_DW		64u

/* ruvd commands to the VCPU (GPCOM_VCPU_CMD takes them shifted left by one) */
#define FRB_UVD_CMD_MSG_BUFFER		0x000
#define FRB_UVD_CMD_DPB_BUFFER		0x001
#define FRB_UVD_CMD_DECODING_TARGET	0x002
#define FRB_UVD_CMD_FEEDBACK_BUFFER	0x003
#define FRB_UVD_CMD_SESSION_CONTEXT	0x005
#define FRB_UVD_CMD_BITSTREAM_BUFFER	0x100
#define FRB_UVD_CMD_ITSCALING_TABLE	0x204
#define FRB_UVD_CMD_CONTEXT_BUFFER	0x206

#define FRB_UVD_CODEC_H264		0u	/* before Tonga */
#define FRB_UVD_CODEC_H264_PERF		7u

/* the UVD block the messages and IBs are for */
struct frb_uvd_h264_hw {
	uint32_t reg_base;		/* dword index the GPCOM registers are relative to (frb_uvd_reg_base) */
	uint32_t dpb_alignment;		/* ac_uvd_dec_dpb_alignment: 16 before Vega, 32 from Vega 10 on */
	uint32_t legacy;		/* family < CHIP_TONGA: stream type H264, the legacy DPB (above) */
};
#define FRB_UVD_H264_HW_UVD3		{ 0x3800, 16, 1 }	/* UVD_BASE_SI; SI */
#define FRB_UVD_H264_HW_UVD6		{ 0x3800, 16, 0 }	/* UVD_BASE_SI */
#define FRB_UVD_H264_HW_VEGA20		{ 0x7e00, 32, 0 }	/* UVD_BASE__INST0_SEG1 */
/* the GPCOM registers from reg_base (uvd_6_0_d.h less UVD_BASE_SI = uvd_7_0_offset.h) */
#define FRB_UVD_H264_GPCOM_VCPU_CMD	0x03c3
#define FRB_UVD_H264_GPCOM_VCPU_DATA0	0x03c4
#define FRB_UVD_H264_GPCOM_VCPU_DATA1	0x03c5
#define FRB_UVD_H264_ENGINE_CNTL	0x03c6
#define FRB_UVD_H264_NO_OP		0x03ff

/* a session's sizes, derived from its config alone (kernel-owned; never a client's) */
struct frb_uvd_h264_layout {
	struct frb_uvd_h264_hw hw;
	uint32_t width, height;		/* coded, multiples of 16 */
	uint32_t db_pitch;		/* align(width, dpb_alignment) */
	uint32_t image_bytes;		/* one NV12 picture, aligned to 1 KB */
	uint32_t dpb_bytes;		/* 17 pictures (legacy: + the macroblock context and IT surface) */
	uint32_t ctx_bytes;		/* the macroblock context: 17 x align(mbs x 192, 256); legacy 0 */
	uint32_t session_bytes;		/* the session context; legacy 0 */
	uint32_t target_pitch;		/* align(width, 256) */
	uint32_t target_rows;		/* height */
	uint32_t chroma_offset;		/* pitch x rows */
	uint32_t target_bytes;		/* pitch x rows x 3 / 2 */
	uint32_t packed_nv12_bytes;	/* width x height x 3 / 2 */
};

/* GPU addresses a decode IB names; all disjoint, 64-bit, the msg 64-byte aligned (legacy: no session, ctx, its) */
struct frb_uvd_h264_buffers {
	uint64_t session, msg, dpb, ctx, its, feedback, bitstream, target;
};

/* RadeonNI's checks (valid_config): the same configs pass, and nothing else */
int frb_uvd_h264_layout_init(const struct frb_uvd_h264_hw *hw, const struct rdn_uvd_h264_config *c,
			     struct frb_uvd_h264_layout *l);
int frb_uvd_h264_valid_picture(const struct rdn_uvd_h264_config *c, const struct rdn_uvd_h264_picture *p);
/* 128-byte aligned size of a bitstream (the rest zero), as RadeonNI */
int frb_uvd_h264_bitstream_size(uint32_t bytes, uint32_t *padded);

/* msg into out[FRB_UVD_H264_MSG_BYTES], little-endian; 0 or -1 */
int frb_uvd_h264_pack_create(const struct rdn_uvd_h264_config *c, const struct frb_uvd_h264_layout *l,
			     uint32_t handle, uint8_t *out);
int frb_uvd_h264_pack_decode(const struct rdn_uvd_h264_config *c, const struct rdn_uvd_h264_picture *p,
			     const struct frb_uvd_h264_layout *l, uint32_t handle, uint32_t feedback,
			     uint32_t bitstream_bytes, uint8_t *out);
int frb_uvd_h264_pack_destroy(uint32_t handle, uint8_t *out);
/* the IT scaling table, out[FRB_UVD_H264_ITS_BYTES] */
void frb_uvd_h264_pack_its(const struct rdn_uvd_h264_picture *p, uint8_t *out);

/*
 * The IB's dwords as numbers (the caller stores them in the ring's byte
 * order: frb_uvd_ib_write), NO_OP-padded to 16. type: RDN_UVD_H264_MSG_*.
 * Create and destroy name only session and msg (destroy only msg, as
 * Linux's and old radeonsi's); decode all eight. Legacy: no session in
 * create, decode msg, DPB, feedback, bitstream and target. Returns the
 * count, or 0 when a buffer is missing or misaligned.
 */
uint32_t frb_uvd_h264_build_ib(uint32_t type, const struct frb_uvd_h264_hw *hw,
			       const struct frb_uvd_h264_buffers *b, uint32_t out[FRB_UVD_H264_IB_MAX_DW]);

#endif
