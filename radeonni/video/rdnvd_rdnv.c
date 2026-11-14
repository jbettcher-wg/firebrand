/*
 * rdnvd over the kext's typed video client ('rdnv', hw/rdn_video.h): the rdnvd.h interface with no Mesa in it.
 *
 * mesa/target/rdnvd.c drives UVD through Mesa's r600 decoder, so it needs a TeraScale card with the r600 GL screen.
 * This file gives programs the same interface on any card whose kext offers 'rdnv' sessions (the GCN cards, Hawaii
 * UVD 4.2 on the T4240 first): the H.264 layer (video/rdnvd_h264.c) finds the pictures and keeps the display order,
 * the kext builds the firmware's messages from a fixed-width picture description (hw/rdn_uvd_h264.c), decodes,
 * and leaves the picture as NV12 in one of two shared output slots. The picture is copied out of the slot at once
 * (the firmware keeps its references itself; the slots must go back for the next decode), so a struct rdnvd_frame
 * here is a picture in system memory, not in video memory.
 *
 * Differences from the r600 library, all from the kext's 'rdnv' contract:
 *   - one session in the whole system (max_sessions 1) and one decode at a time; rdnvd_h264_room() is "the
 *     decoder is free", not a video memory estimate;
 *   - frame pictures only: no MBAFF (the kext refuses mb_adaptive_frame_field_flag) and no field pictures;
 *   - Baseline, Main and High (profile_idc 66, 77, 100; 8-bit 4:2:0 High variants are sent as 100), coded size
 *     64x64 to 1920x1088, levels 1.0 to 4.2 (a stream that says a higher level but fits level 4.2's frame and
 *     DPB limits is sent as 4.2), at most 4 MB of slice data per picture;
 *   - a picture the firmware reports as failed ends the kext's session (it is sticky): this library opens a new
 *     one and skips pictures until the next IDR picture.
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/time.h>

#include <mach/mach.h>
#include <IOKit/IOKitLib.h>

#include "rdn_user.h"
#include "rdn_video.h"
#include "rdn_uvd_h264.h"
#include "rdnvd.h"
#include "rdnvd_h264.h"

#define RDNVD_PUBLIC __attribute__((visibility("default")))

#ifdef __LP64__
typedef mach_vm_address_t map_addr_t;
typedef mach_vm_size_t map_size_t;
#else
typedef vm_address_t map_addr_t;
typedef vm_size_t map_size_t;
#endif

struct rdnvd {
	io_service_t service;
};

struct frame_buf {
	struct rdnvd_frame frame;		/* first: a frame pointer is a frame_buf pointer */
	struct rdnvd_session *session;		/* NULL once the session is closed */
	uint8_t *nv12;				/* coded_width x coded_height luma, then interleaved chroma */
	size_t bytes;
	int refs;
	struct frame_buf *next;			/* the session's list of all its pictures */
	struct frame_buf *next_free;
};

struct rdnvd_session {
	struct rdnvd *d;
	struct rdnvd_h264 *parser;
	rdnvd_output_fn output;
	void *user;

	/* the kext's session */
	io_connect_t conn;
	int created;
	struct rdn_video_token token;
	uint32_t next_request;
	uint8_t *upload;
	size_t upload_bytes;
	uint8_t *slot[RDN_VIDEO_OUTPUT_SLOTS];
	size_t slot_bytes[RDN_VIDEO_OUTPUT_SLOTS];
	struct rdn_uvd_h264_config config;
	int have_config;
	int need_idr;				/* a decode failed: nothing is sent until an IDR picture */

	unsigned width, height;			/* coded, in samples */
	unsigned crop_left, crop_right, crop_top, crop_bottom;
	unsigned reorder_delay;
	unsigned decoded;			/* pictures decoded so far (rdnvd_h264_decoded) */

	struct frame_buf *all, *free;
	double t_decode, t_copy;		/* RDNVD_DEBUG: ms in the kext's DECODE and in copying slots out */
	struct frame_buf *captured;		/* rdnvd_h264_decode_au: the picture of the call in progress */
	int captured_count;
	int error;
};

static int debug_on(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("RDNVD_DEBUG") != NULL;
	return on;
}

static double now_ms(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

#define LOG(...) do { if (debug_on()) fprintf(stderr, "rdnvd: " __VA_ARGS__); } while (0)

/* ---- the kext's calls (old-style methods: LP64 takes the IOConnectCall* forms) --------------------- */

static kern_return_t call_struct(io_connect_t c, uint32_t sel, const void *in, size_t in_size, void *out, size_t out_size)
{
#ifdef __LP64__
	size_t got = out_size;
	kern_return_t kr = IOConnectCallStructMethod(c, sel, in, in_size, out, out ? &got : NULL);
#else
	IOByteCount got = out_size;
	kern_return_t kr;

	if (in && out)
		kr = IOConnectMethodStructureIStructureO(c, sel, in_size, &got, (void *)in, out);
	else if (in)
		kr = IOConnectMethodScalarIStructureI(c, sel, 0, in_size, (void *)in);
	else
		kr = IOConnectMethodScalarIStructureO(c, sel, 0, &got, out);
#endif
	if (!kr && out && got != out_size)
		return kIOReturnUnderrun;
	return kr;
}

static int query(io_service_t service, struct rdn_video_query *q)
{
	io_connect_t c = IO_OBJECT_NULL;
	kern_return_t kr;

	memset(q, 0, sizeof(*q));
	if (IOServiceOpen(service, mach_task_self(), RDN_VIDEO_UC_TYPE, &c))
		return -1;
	kr = call_struct(c, RDN_VIDEO_QUERY, NULL, 0, q, sizeof(*q));
	IOServiceClose(c);
	return kr ? -1 : 0;
}

static void *map(io_connect_t c, uint32_t type, size_t *bytes)
{
	map_addr_t at = 0;
	map_size_t size = 0;

	if (IOConnectMapMemory(c, type, mach_task_self(), &at, &size, kIOMapAnywhere) || !at)
		return NULL;
	*bytes = (size_t)size;
	return (void *)(uintptr_t)at;
}

static void unmap(io_connect_t c, uint32_t type, void *at)
{
	if (at)
		IOConnectUnmapMemory(c, type, mach_task_self(), (map_addr_t)(uintptr_t)at);
}

/* End the kext's session (DESTROY, then close the connection: one CREATE per connection). */
static void hw_close(struct rdnvd_session *s)
{
	unsigned i;

	if (!s->conn)
		return;
	unmap(s->conn, RDN_VIDEO_MEMORY_UPLOAD, s->upload);
	for (i = 0; i < RDN_VIDEO_OUTPUT_SLOTS; i++)
		unmap(s->conn, RDN_VIDEO_MEMORY_OUTPUT0 + i, s->slot[i]);
	if (s->created) {
		struct rdn_video_destroy destroy;

		memset(&destroy, 0, sizeof(destroy));
		destroy.version = RDN_VIDEO_API_VERSION;
		destroy.bytes = sizeof(destroy);
		destroy.session = s->token;
		call_struct(s->conn, RDN_VIDEO_DESTROY, &destroy, sizeof(destroy), NULL, 0);
	}
	IOServiceClose(s->conn);
	s->conn = IO_OBJECT_NULL;
	s->created = 0;
	s->upload = NULL;
	memset(s->slot, 0, sizeof(s->slot));
}

static int hw_open(struct rdnvd_session *s)
{
	struct rdn_video_create create;
	struct rdn_video_session out;
	kern_return_t kr;
	unsigned i;

	if (s->conn)
		return 0;
	if (!s->have_config)
		return RDNVD_FAILED;
	if (IOServiceOpen(s->d->service, mach_task_self(), RDN_VIDEO_UC_TYPE, &s->conn)) {
		s->conn = IO_OBJECT_NULL;
		return RDNVD_FAILED;
	}
	memset(&create, 0, sizeof(create));
	create.version = RDN_VIDEO_API_VERSION;
	create.bytes = sizeof(create);
	create.config = s->config;
	/* RDNVD_VALIDATION=1 (root, EnableUVDValidation): a validation session, whose first decodes the kext times in its log */
	if (getenv("RDNVD_VALIDATION"))
		create.flags = RDN_VIDEO_CREATE_VALIDATION;
	memset(&out, 0, sizeof(out));
	kr = call_struct(s->conn, RDN_VIDEO_CREATE, &create, sizeof(create), &out, sizeof(out));
	if (kr) {
		LOG("CREATE %ux%u profile %u level %u refs %u: 0x%x\n", s->config.coded_width, s->config.coded_height,
		    s->config.profile_idc, s->config.level_idc, s->config.max_num_ref_frames, kr);
		IOServiceClose(s->conn);
		s->conn = IO_OBJECT_NULL;
		return kr == kIOReturnUnsupported ? RDNVD_UNSUPPORTED : kr == kIOReturnNoMemory ? RDNVD_NO_MEMORY : RDNVD_FAILED;
	}
	s->created = 1;
	s->token = out.session;
	s->next_request = 1;
	s->upload = map(s->conn, RDN_VIDEO_MEMORY_UPLOAD, &s->upload_bytes);
	for (i = 0; i < RDN_VIDEO_OUTPUT_SLOTS; i++)
		s->slot[i] = map(s->conn, RDN_VIDEO_MEMORY_OUTPUT0 + i, &s->slot_bytes[i]);
	if (!s->upload || !s->slot[0] || !s->slot[1] || out.format != RDN_VIDEO_FORMAT_NV12 ||
	    out.coded_width != s->config.coded_width || out.coded_height != s->config.coded_height) {
		hw_close(s);
		return RDNVD_FAILED;
	}
	LOG("session %ux%u profile %u level %u refs %u\n", s->config.coded_width, s->config.coded_height,
	    s->config.profile_idc, s->config.level_idc, s->config.max_num_ref_frames);
	return 0;
}

/* ---- what the kext takes -------------------------------------------------------------- */

static const uint8_t level_table[] = { 10, 11, 12, 13, 20, 21, 22, 30, 31, 32, 40, 41, 42 };

/* The kext's session description for a sequence; 0 if the card does not decode it. */
static int config_for(const struct rdnvd_h264_sps *sps, struct rdn_uvd_h264_config *c)
{
	struct rdn_uvd_h264_layout layout;
	unsigned i, level = sps->level_idc;
	int known = 0;

	if (sps->chroma_format_idc != 1 || sps->separate_colour_plane_flag || sps->bit_depth_luma_minus8 ||
	    sps->bit_depth_chroma_minus8 || !sps->frame_mbs_only_flag || sps->mb_adaptive_frame_field_flag)
		return 0;
	memset(c, 0, sizeof(*c));
	c->version = RDN_UVD_H264_VERSION;
	if (sps->profile_idc == 66 || sps->profile_idc == 77)
		c->profile_idc = sps->profile_idc;
	else if (sps->profile_idc >= 100)
		c->profile_idc = 100;			/* High 10/4:2:2/4:4:4 with 8-bit 4:2:0 content: High */
	else
		return 0;				/* Extended: data partitioning, SP/SI */
	c->coded_width = sps->width_mbs * 16;
	c->coded_height = sps->height_mbs * 16;
	c->max_num_ref_frames = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
	c->chroma_format_idc = 1;
	c->frame_mbs_only_flag = 1;
	if (c->coded_width < 64 || c->coded_height < 64)
		return 0;				/* the kext refuses them: unchanged output on Turks */
	/* Level 1b (11 with constraint_set3 in Baseline/Main) is not in the firmware's table. */
	if (level == 11 && sps->profile_idc != 100 && (sps->constraint_flags & 0x10))
		level = 9;
	for (i = 0; i < sizeof(level_table); i++)
		known |= level_table[i] == level;
	/*
	 * A level the firmware's table lacks (1b, 5.x) is sent as 4.2 when the picture and its references fit 4.2's
	 * limits, and so is a stream with more references than its own level allows (RDNVD_NO_LEVEL_CLAMP: neither).
	 */
	if (!known && getenv("RDNVD_NO_LEVEL_CLAMP"))
		return 0;
	c->level_idc = known ? level : 42;
	if (!rdn_uvd_h264_layout_init(c, RDN_UVD_H264_TARGET_FIELDS, &layout))
		return 1;
	if (c->level_idc == 42 || getenv("RDNVD_NO_LEVEL_CLAMP"))
		return 0;
	c->level_idc = 42;
	return !rdn_uvd_h264_layout_init(c, RDN_UVD_H264_TARGET_FIELDS, &layout);
}

/* The raster position of each place of the zig-zag scan (8.5.6, 8.5.7). */
static const uint8_t zigzag4[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };
static const uint8_t zigzag8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14,
	21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53,
	60, 61, 54, 47, 55, 62, 63
};

/* The picture description the kext packs into the firmware's message (what Mesa's radeon_uvd.c sends). */
static int picture_for(struct rdnvd_session *s, const struct rdnvd_h264_picture *pic, struct rdn_uvd_h264_picture *p)
{
	const struct rdnvd_h264_sps *sps = pic->sps;
	const struct rdnvd_h264_pps *pps = pic->pps;
	unsigned i, j, n;

	if (pps->num_slice_groups_minus1)
		return RDNVD_UNSUPPORTED;		/* FMO: the kext takes no slice groups */
	memset(p, 0, sizeof(*p));
	p->version = RDN_UVD_H264_VERSION;
	p->sps_info_flags = (uint32_t)sps->direct_8x8_inference_flag << 0 | (uint32_t)sps->frame_mbs_only_flag << 2 |
			    (uint32_t)sps->delta_pic_order_always_zero_flag << 3;
	p->pps_info_flags = (uint32_t)pps->transform_8x8_mode_flag << 0 | (uint32_t)pps->redundant_pic_cnt_present_flag << 1 |
			    (uint32_t)pps->constrained_intra_pred_flag << 2 |
			    (uint32_t)pps->deblocking_filter_control_present_flag << 3 |
			    (uint32_t)(pps->weighted_bipred_idc & 3) << 4 | (uint32_t)pps->weighted_pred_flag << 6 |
			    (uint32_t)pps->bottom_field_pic_order_in_frame_present_flag << 7 |
			    (uint32_t)pps->entropy_coding_mode_flag << 8;
	p->log2_max_frame_num_minus4 = sps->log2_max_frame_num_minus4;
	p->pic_order_cnt_type = sps->pic_order_cnt_type;
	p->log2_max_pic_order_cnt_lsb_minus4 = sps->log2_max_pic_order_cnt_lsb_minus4;
	p->pic_init_qp_minus26 = pps->pic_init_qp_minus26;
	p->pic_init_qs_minus26 = pps->pic_init_qs_minus26;
	p->chroma_qp_index_offset = pps->chroma_qp_index_offset;
	p->second_chroma_qp_index_offset = pps->second_chroma_qp_index_offset;
	p->num_ref_idx_l0_active_minus1 = pic->num_ref_idx_active_minus1[0];
	p->num_ref_idx_l1_active_minus1 = pic->num_ref_idx_active_minus1[1];
	p->frame_num = pic->frame_num;
	n = pic->num_refs;
	if (n > s->config.max_num_ref_frames)
		n = s->config.max_num_ref_frames;
	p->reference_count = n;
	for (i = 0; i < n; i++) {
		p->frame_num_list[i] = pic->refs[i].frame_num;
		p->field_order_cnt_list[i][0] = pic->refs[i].poc[0];
		p->field_order_cnt_list[i][1] = pic->refs[i].poc[1];
	}
	p->field_order_cnt[0] = pic->poc[0];
	p->field_order_cnt[1] = pic->poc[1];
	/* The lists in force (PPS over SPS over flat), from scan order to raster as VA-API gives them; 8x8: Y intra, Y inter. */
	for (i = 0; i < 6; i++)
		for (j = 0; j < 16; j++)
			p->scaling_list_4x4[i][zigzag4[j]] = pic->scaling_list_4x4[i][j];
	for (i = 0; i < 2; i++)
		for (j = 0; j < 64; j++)
			p->scaling_list_8x8[i][zigzag8[j]] = pic->scaling_list_8x8[i][j];
	return 0;
}

/* ---- pictures --------------------------------------------------------------------------- */

static void frame_unlink(struct rdnvd_session *s, struct frame_buf *f)
{
	struct frame_buf **p;

	for (p = &s->all; *p; p = &(*p)->next)
		if (*p == f) {
			*p = f->next;
			return;
		}
}

static struct frame_buf *frame_get(struct rdnvd_session *s, size_t bytes)
{
	struct frame_buf *f;

	while ((f = s->free) != NULL) {
		s->free = f->next_free;
		if (f->bytes == bytes) {
			f->refs = 1;
			return f;
		}
		frame_unlink(s, f);		/* of an older sequence */
		free(f->nv12);
		free(f);
	}
	f = calloc(1, sizeof(*f));
	if (!f)
		return NULL;
	f->nv12 = malloc(bytes);
	if (!f->nv12) {
		free(f);
		return NULL;
	}
	f->bytes = bytes;
	f->session = s;
	f->refs = 1;
	f->next = s->all;
	s->all = f;
	return f;
}

static void frame_unref(struct frame_buf *f)
{
	struct rdnvd_session *s = f->session;

	if (--f->refs > 0)
		return;
	if (s) {
		f->next_free = s->free;
		s->free = f;
		return;
	}
	free(f->nv12);
	free(f);
}

static int on_sequence(void *user, const struct rdnvd_h264_sps *sps)
{
	struct rdnvd_session *s = user;
	struct rdn_uvd_h264_config c;

	if (!config_for(sps, &c))
		return RDNVD_UNSUPPORTED;
	s->width = sps->width_mbs * 16;
	s->height = sps->height_mbs * 16;
	s->crop_left = sps->crop_left;
	s->crop_right = sps->crop_right;
	s->crop_top = sps->crop_top;
	s->crop_bottom = sps->crop_bottom;
	s->reorder_delay = sps->vui_bitstream_restriction ? sps->max_num_reorder_frames + 1 : sps->dpb_frames;
	if (s->have_config && !memcmp(&c, &s->config, sizeof(c)) && s->conn)
		return 0;
	/* A new sequence: the old pictures have all been output; the firmware gets a new decoder. */
	hw_close(s);
	s->config = c;
	s->have_config = 1;
	s->need_idr = 0;
	return hw_open(s);
}

static int on_decode(void *user, const struct rdnvd_h264_picture *pic, void **surface)
{
	struct rdnvd_session *s = user;
	static const uint8_t start_code[3] = { 0, 0, 1 };
	struct rdn_video_decode dec;
	struct rdn_video_output out;
	struct rdn_video_release rel;
	struct frame_buf *f;
	size_t at = 0, planes;
	unsigned i, row;
	kern_return_t kr;
	double t0, t1;
	int r;

	if (s->need_idr && !pic->idr)
		return RDNVD_FAILED;
	if (!s->conn && (r = hw_open(s)) != 0)
		return r;
	memset(&dec, 0, sizeof(dec));
	if ((r = picture_for(s, pic, &dec.picture)) != 0)
		return r;
	for (i = 0; i < pic->num_slices; i++) {
		if (at + 3 + pic->slices[i].size > s->upload_bytes || at + 3 + pic->slices[i].size > RDN_VIDEO_MAX_UPLOAD_BYTES) {
			LOG("picture of more than %zu bytes\n", s->upload_bytes);
			return RDNVD_UNSUPPORTED;
		}
		memcpy(s->upload + at, start_code, 3);
		memcpy(s->upload + at + 3, pic->slices[i].nal, pic->slices[i].size);
		at += 3 + pic->slices[i].size;
	}
	if (!at)
		return RDNVD_BAD_STREAM;
	dec.version = RDN_VIDEO_API_VERSION;
	dec.bytes = sizeof(dec);
	dec.session = s->token;
	dec.request_sequence = s->next_request;
	dec.bitstream_bytes = (uint32_t)at;
	memset(&out, 0, sizeof(out));
	t0 = now_ms();
	kr = call_struct(s->conn, RDN_VIDEO_DECODE, &dec, sizeof(dec), &out, sizeof(out));
	t1 = now_ms();
	s->t_decode += t1 - t0;
	if (kr) {
		/* The kext ends a session whose picture failed: a new one, from the next IDR picture. */
		syslog(LOG_NOTICE, "rdnvd: decode %u failed (0x%x); waiting for an IDR picture", s->next_request, kr);
		LOG("decode %u (frame_num %u, %s) failed: 0x%x\n", s->next_request, pic->frame_num, pic->idr ? "IDR" : "non-IDR", kr);
		hw_close(s);
		s->need_idr = 1;
		return RDNVD_FAILED;
	}
	s->next_request = out.request_sequence + 1;
	s->need_idr = 0;
	planes = (size_t)s->width * s->height * 3 / 2;
	f = frame_get(s, planes);
	if (f && out.slot < RDN_VIDEO_OUTPUT_SLOTS && out.coded_width == s->width && out.coded_height == s->height &&
	    out.y_offset + (size_t)out.y_stride * s->height <= s->slot_bytes[out.slot] &&
	    out.uv_offset + (size_t)out.uv_stride * s->height / 2 <= s->slot_bytes[out.slot]) {
		const uint8_t *src = s->slot[out.slot];

		if (out.y_stride == s->width && out.uv_stride == s->width && out.uv_offset == out.y_offset + (size_t)s->width * s->height) {
			memcpy(f->nv12, src + out.y_offset, planes);
		} else {
			for (row = 0; row < s->height; row++)
				memcpy(f->nv12 + (size_t)row * s->width, src + out.y_offset + (size_t)row * out.y_stride, s->width);
			for (row = 0; row < s->height / 2; row++)
				memcpy(f->nv12 + (size_t)s->width * s->height + (size_t)row * s->width,
				       src + out.uv_offset + (size_t)row * out.uv_stride, s->width);
		}
	} else if (f) {
		frame_unref(f);
		f = NULL;
	}
	s->t_copy += now_ms() - t1;
	memset(&rel, 0, sizeof(rel));
	rel.version = RDN_VIDEO_API_VERSION;
	rel.bytes = sizeof(rel);
	rel.session = s->token;
	rel.output_sequence = out.output_sequence;
	rel.slot = out.slot;
	call_struct(s->conn, RDN_VIDEO_RELEASE, &rel, sizeof(rel), NULL, 0);
	if (!f)
		return RDNVD_NO_MEMORY;
	f->frame.coded_width = s->width;
	f->frame.coded_height = s->height;
	f->frame.crop_left = s->crop_left;
	f->frame.crop_top = s->crop_top;
	f->frame.width = s->width - s->crop_left - s->crop_right;
	f->frame.height = s->height - s->crop_top - s->crop_bottom;
	f->frame.pts = pic->pts;
	*surface = f;
	s->decoded++;
	if (s->captured_count++ == 0) {
		f->refs++;
		s->captured = f;
	}
	return 0;
}

static void on_output(void *user, void *surface, uint64_t pts, const struct rdnvd_h264_sps *sps)
{
	struct rdnvd_session *s = user;
	struct frame_buf *f = surface;

	if (!s->output)
		return;
	f->refs++;				/* the caller's */
	f->frame.pts = pts;
	f->frame.coded_width = sps->width_mbs * 16;
	f->frame.coded_height = sps->height_mbs * 16;
	f->frame.crop_left = sps->crop_left;
	f->frame.crop_top = sps->crop_top;
	f->frame.width = f->frame.coded_width - sps->crop_left - sps->crop_right;
	f->frame.height = f->frame.coded_height - sps->crop_top - sps->crop_bottom;
	s->output(s->user, &f->frame);
}

static void on_release(void *user, void *surface)
{
	frame_unref(surface);
}

static const struct rdnvd_h264_callbacks session_callbacks = { on_sequence, on_decode, on_output, on_release };

/* ---- the interface ------------------------------------------------------------------------ */

static int nop_sequence(void *user, const struct rdnvd_h264_sps *sps)
{
	(void)user;
	(void)sps;
	return 0;
}

RDNVD_PUBLIC int rdnvd_h264_supported(const uint8_t *avcc, size_t avcc_size, unsigned *width, unsigned *height)
{
	static const struct rdnvd_h264_callbacks cb = { nop_sequence, NULL, NULL, NULL };
	struct rdnvd_h264 *p = rdnvd_h264_parser_new(&cb, NULL);
	const struct rdnvd_h264_sps *sps;
	struct rdn_uvd_h264_config c;
	int ok = 0;

	if (!p)
		return 0;
	if (rdnvd_h264_parser_avcc(p, avcc, avcc_size) >= 0 && (sps = rdnvd_h264_parser_first_sps(p)) != NULL &&
	    config_for(sps, &c)) {
		ok = 1;
		if (width)
			*width = sps->width_mbs * 16 - sps->crop_left - sps->crop_right;
		if (height)
			*height = sps->height_mbs * 16 - sps->crop_top - sps->crop_bottom;
	}
	rdnvd_h264_parser_free(p);
	return ok;
}

RDNVD_PUBLIC struct rdnvd *rdnvd_open(void)
{
	struct rdnvd *d;
	struct rdn_video_query q;
	io_service_t service;

	if (getenv("RDNVD_DISABLE"))
		return NULL;
	service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(RDN_UC_SERVICE_CLASS));
	if (!service)
		return NULL;
	if (query(service, &q) || q.version != RDN_VIDEO_API_VERSION || !(q.flags & RDN_VIDEO_CAP_READY) ||
	    !(q.flags & RDN_VIDEO_CAP_PRODUCTION)) {
		LOG("no production video sessions (flags 0x%x reason %u)\n", q.flags, q.unavailable_reason);
		IOObjectRelease(service);
		return NULL;
	}
	d = calloc(1, sizeof(*d));
	if (!d) {
		IOObjectRelease(service);
		return NULL;
	}
	d->service = service;
	return d;
}

RDNVD_PUBLIC void rdnvd_close(struct rdnvd *d)
{
	if (!d)
		return;
	IOObjectRelease(d->service);
	free(d);
}

RDNVD_PUBLIC struct rdnvd_session *rdnvd_h264_open(struct rdnvd *d, const uint8_t *avcc, size_t avcc_size,
						   rdnvd_output_fn output, void *user, unsigned *nal_length_size)
{
	struct rdnvd_session *s;

	if (!d)
		return NULL;
	s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	s->d = d;
	s->output = output;
	s->user = user;
	s->parser = rdnvd_h264_parser_new(&session_callbacks, s);
	if (!s->parser) {
		free(s);
		return NULL;
	}
	if (nal_length_size)
		*nal_length_size = 0;
	if (avcc) {
		int r = rdnvd_h264_parser_avcc(s->parser, avcc, avcc_size);
		const struct rdnvd_h264_sps *sps = r < 0 ? NULL : rdnvd_h264_parser_first_sps(s->parser);
		struct rdn_uvd_h264_config c;

		if (r < 0 || !sps || !config_for(sps, &c)) {
			rdnvd_h264_parser_free(s->parser);
			free(s);
			return NULL;
		}
		if (nal_length_size)
			*nal_length_size = (unsigned)r;
	}
	return s;
}

/* One decoder in the system: "room" is whether the kext's decoder is free (or this program's session has it). */
static int decoder_free(struct rdnvd *d, unsigned width, unsigned height)
{
	struct rdn_video_query q;

	if (!d || query(d->service, &q))
		return 0;
	return (q.flags & RDN_VIDEO_CAP_READY) && (q.flags & RDN_VIDEO_CAP_PRODUCTION) && !(q.flags & RDN_VIDEO_CAP_BUSY) &&
	       width <= q.max_width && height <= q.max_height;
}

RDNVD_PUBLIC int rdnvd_h264_room(struct rdnvd *d, unsigned width, unsigned height)
{
	return decoder_free(d, width, height);
}

RDNVD_PUBLIC int rdnvd_h264_room_for(struct rdnvd_session *s)
{
	const struct rdnvd_h264_sps *sps;

	if (!s)
		return 0;
	if (s->conn)
		return 1;
	sps = rdnvd_h264_parser_first_sps(s->parser);
	return sps && decoder_free(s->d, sps->width_mbs * 16, sps->height_mbs * 16);
}

RDNVD_PUBLIC int rdnvd_h264_prepare(struct rdnvd_session *s)
{
	const struct rdnvd_h264_sps *sps = rdnvd_h264_parser_first_sps(s->parser);
	int r;

	if (s->conn)
		return 0;
	if (!sps)
		return RDNVD_BAD_STREAM;
	r = on_sequence(s, sps);
	if (r)
		return r;
	rdnvd_h264_parser_set_sequence(s->parser, sps);
	return 0;
}

RDNVD_PUBLIC int rdnvd_h264_feed(struct rdnvd_session *s, const uint8_t *data, size_t size, uint64_t pts,
				 unsigned nal_length_size, int end_of_au)
{
	int r, r2 = 0;

	r = nal_length_size ? rdnvd_h264_parser_feed_avcc(s->parser, data, size, nal_length_size, pts) :
			      rdnvd_h264_parser_feed_annexb(s->parser, data, size, pts);
	if (end_of_au)
		r2 = rdnvd_h264_parser_end_au(s->parser);
	return r ? r : r2;
}

RDNVD_PUBLIC int rdnvd_h264_decode_au(struct rdnvd_session *s, const uint8_t *data, size_t size,
				      unsigned nal_length_size, struct rdnvd_frame **frame)
{
	int r;

	s->captured = NULL;
	s->captured_count = 0;
	r = rdnvd_h264_feed(s, data, size, 0, nal_length_size, 1);
	*frame = s->captured ? &s->captured->frame : NULL;
	s->captured = NULL;
	return r;
}

RDNVD_PUBLIC unsigned rdnvd_h264_decoded(struct rdnvd_session *s)
{
	return s ? s->decoded : 0;
}

RDNVD_PUBLIC unsigned rdnvd_h264_reorder_delay(struct rdnvd_session *s)
{
	return s->reorder_delay;
}

RDNVD_PUBLIC int rdnvd_h264_flush(struct rdnvd_session *s, int drop)
{
	return rdnvd_h264_parser_flush(s->parser, drop);
}

RDNVD_PUBLIC void rdnvd_h264_close(struct rdnvd_session *s)
{
	struct frame_buf *f, *next;

	if (!s)
		return;
	rdnvd_h264_parser_free(s->parser);	/* releases what the parser holds */
	if (s->decoded)
		LOG("%u pictures: %.2f ms a picture in DECODE, %.2f ms copying it out of the slot\n", s->decoded,
		    s->t_decode / s->decoded, s->t_copy / s->decoded);
	hw_close(s);
	/* Pictures the caller still holds stay until released (then they are freed); the rest go now. */
	for (f = s->all; f; f = next) {
		next = f->next;
		if (f->refs > 0) {
			f->session = NULL;
		} else {
			free(f->nv12);
			free(f);
		}
	}
	free(s);
}

RDNVD_PUBLIC int rdnvd_frame_read(struct rdnvd_frame *frame, uint8_t *y, size_t y_stride, uint8_t *uv, size_t uv_stride)
{
	struct frame_buf *f = (struct frame_buf *)frame;
	const uint8_t *src_y, *src_uv;
	unsigned row, cw = frame->coded_width;

	if (!f->nv12 || frame->crop_left + frame->width > cw || frame->crop_top + frame->height > frame->coded_height)
		return RDNVD_FAILED;
	src_y = f->nv12 + (size_t)frame->crop_top * cw + frame->crop_left;
	src_uv = f->nv12 + (size_t)cw * frame->coded_height + (size_t)(frame->crop_top / 2) * cw + (frame->crop_left & ~1u);
	if (y_stride == cw && frame->width == cw && !frame->crop_left) {
		memcpy(y, src_y, (size_t)cw * frame->height);
	} else {
		for (row = 0; row < frame->height; row++)
			memcpy(y + row * y_stride, src_y + (size_t)row * cw, frame->width);
	}
	if (uv_stride == cw && frame->width == cw && !frame->crop_left) {
		memcpy(uv, src_uv, (size_t)cw * ((frame->height + 1) / 2));
	} else {
		for (row = 0; row < (frame->height + 1) / 2; row++)
			memcpy(uv + row * uv_stride, src_uv + (size_t)row * cw, (frame->width + 1) & ~1u);
	}
	return 0;
}

RDNVD_PUBLIC int rdnvd_frame_read_2vuy(struct rdnvd_frame *frame, uint8_t *dst, size_t stride)
{
	struct frame_buf *f = (struct frame_buf *)frame;
	unsigned row, x, cw = frame->coded_width;

	if (!f->nv12 || frame->crop_left + frame->width > cw || frame->crop_top + frame->height > frame->coded_height)
		return RDNVD_FAILED;
	for (row = 0; row < frame->height; row++) {
		const uint8_t *ys = f->nv12 + (size_t)(frame->crop_top + row) * cw + frame->crop_left;
		const uint8_t *cs = f->nv12 + (size_t)cw * frame->coded_height +
				    (size_t)((frame->crop_top + row) / 2) * cw + (frame->crop_left & ~1u);
		uint8_t *o = dst + row * stride;

		for (x = 0; x + 1 < frame->width; x += 2) {
			o[2 * x + 0] = cs[x];
			o[2 * x + 1] = ys[x];
			o[2 * x + 2] = cs[x + 1];
			o[2 * x + 3] = ys[x + 1];
		}
	}
	return 0;
}

RDNVD_PUBLIC void rdnvd_frame_release(struct rdnvd_frame *frame)
{
	if (frame)
		frame_unref((struct frame_buf *)frame);
}
