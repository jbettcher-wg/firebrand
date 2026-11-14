/*
 * See frb_rect.h: rectangles on SDMA v3 for the accelerator's 2D work.
 * The clipping and the order-independent move follow RadeonNI
 * (RadeonNIAccel.cpp screenClip/screenCopy, hw/rdn_cp.c rdn_cp_copy_rects);
 * the packets are SDMA v3's (tonga_sdma_pkt_open.h), laid out as Mesa's
 * radeonsi emits them for GFX8 (cik_sdma.c, si_sdma_clear_buffer).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_rect.h"
#include "include/tonga_sdma_pkt_open.h"

static void emit(struct frb_rect_ib *ib, uint32_t v)
{
	frb_put_le32(ib->buf, 4 * ib->len_dw, v);
	ib->len_dw++;
}

static uint32_t room(const struct frb_rect_ib *ib)
{
	return ib->cap_dw > ib->len_dw ? ib->cap_dw - ib->len_dw : 0;
}

int frb_rect_surface_ok(const struct frb_rect_surface *s)
{
	return s && !(s->gpu_addr & 3) && s->width && s->height && s->pitch >= s->width &&
	       s->pitch <= FRB_RECT_MAX && s->height <= FRB_RECT_MAX;
}

static int inside(const struct frb_rect_surface *s, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	return w && h && x < s->width && y < s->height && w <= s->width - x && h <= s->height - y;
}

int frb_rect_clip(const struct frb_rect_surface *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h)
{
	if (*x < 0) {
		*w += *x;
		*x = 0;
	}
	if (*y < 0) {
		*h += *y;
		*y = 0;
	}
	if (*x + *w > (int32_t)s->width)
		*w = (int32_t)s->width - *x;
	if (*y + *h > (int32_t)s->height)
		*h = (int32_t)s->height - *y;
	return *w > 0 && *h > 0;
}

int frb_rect_clip_copy(const struct frb_rect_surface *s, int32_t *sx, int32_t *sy, int32_t *dx, int32_t *dy,
		       int32_t *w, int32_t *h)
{
	int32_t cx = *dx, cy = *dy;

	if (!frb_rect_clip(s, &cx, &cy, w, h))
		return 0;
	*sx += cx - *dx;
	*sy += cy - *dy;
	*dx = cx;
	*dy = cy;
	cx = *sx;
	cy = *sy;
	if (!frb_rect_clip(s, &cx, &cy, w, h))
		return 0;
	*dx += cx - *sx;
	*dy += cy - *sy;
	*sx = cx;
	*sy = cy;
	return 1;
}

static int meet(uint32_t ax, uint32_t ay, uint32_t bx, uint32_t by, uint32_t w1, uint32_t h1, uint32_t w2,
		uint32_t h2)
{
	return ax < bx + w2 && bx < ax + w1 && ay < by + h2 && by < ay + h1;
}

int frb_rect_moves_overlap(const struct frb_rect_move *m, uint32_t n)
{
	uint32_t i, j;

	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			if (meet(m[i].dx, m[i].dy, m[j].sx, m[j].sy, m[i].w, m[i].h, m[j].w, m[j].h))
				return 1;
	return 0;
}

/*
 * CONSTANT_FILL, FILLSIZE 2: count bytes (a multiple of 4, GFX8's count is
 * not minus one) of the dword `data`, stored little-endian.
 */
static void emit_fill(struct frb_rect_ib *ib, uint64_t dst, uint32_t data, uint32_t bytes)
{
	emit(ib, SDMA_PKT_CONSTANT_FILL_HEADER_OP(SDMA_OP_CONST_FILL) |
		 SDMA_PKT_CONSTANT_FILL_HEADER_FILLSIZE(2));
	emit(ib, (uint32_t)dst);
	emit(ib, (uint32_t)(dst >> 32));
	emit(ib, data);
	emit(ib, SDMA_PKT_CONSTANT_FILL_COUNT_COUNT(bytes & ~3u));
}

/*
 * COPY_LINEAR_SUB_WINDOW with 4-byte elements: x, y in elements, pitches
 * and the extent minus one (GFX8; GFX7 took the extent as it is), depth 1,
 * no swap, no cache hints.
 */
static void emit_sub_window(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t dx, uint32_t dy,
			    const struct frb_rect_surface *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_OP(SDMA_OP_COPY) |
		 SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_SUB_OP(SDMA_SUBOP_COPY_LINEAR_SUB_WIND) |
		 SDMA_PKT_COPY_LINEAR_SUBWIN_HEADER_ELEMENTSIZE(2));
	emit(ib, (uint32_t)src->gpu_addr);
	emit(ib, (uint32_t)(src->gpu_addr >> 32));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_3_SRC_X(sx) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_3_SRC_Y(sy));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_4_SRC_Z(0) |
		 SDMA_PKT_COPY_LINEAR_SUBWIN_DW_4_SRC_PITCH(src->pitch - 1));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_5_SRC_SLICE_PITCH(src->pitch * src->height - 1));
	emit(ib, (uint32_t)dst->gpu_addr);
	emit(ib, (uint32_t)(dst->gpu_addr >> 32));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_8_DST_X(dx) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_8_DST_Y(dy));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_9_DST_Z(0) |
		 SDMA_PKT_COPY_LINEAR_SUBWIN_DW_9_DST_PITCH(dst->pitch - 1));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_10_DST_SLICE_PITCH(dst->pitch * dst->height - 1));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_X(w - 1) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_Y(h - 1));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_12_RECT_Z(0));
}

int frb_rect_emit_fill(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t pixel, uint32_t *rows)
{
	uint64_t at;
	uint32_t data, n;

	*rows = 0;
	if (!frb_rect_surface_ok(dst) || !inside(dst, x, y, w, h))
		return -EINVAL;
	/* the engine stores the dword little-endian; the pixel's bytes as this host stores it */
	data = frb_get_le32(&pixel, 0);
	at = dst->gpu_addr + ((uint64_t)y * dst->pitch + x) * 4;
	if (x == 0 && w == dst->pitch) {
		/* whole rows: one run, in packets of at most fill_max_bytes */
		uint64_t left = (uint64_t)h * w * 4;
		uint32_t need = (uint32_t)((left + FRB_RECT_FILL_MAX_BYTES - 1) / FRB_RECT_FILL_MAX_BYTES);

		if (room(ib) < need * FRB_RECT_FILL_DW)
			return -ENOSPC;
		while (left) {
			uint32_t bytes = left > FRB_RECT_FILL_MAX_BYTES ? FRB_RECT_FILL_MAX_BYTES : (uint32_t)left;

			emit_fill(ib, at, data, bytes);
			at += bytes;
			left -= bytes;
		}
		*rows = h;
		return 0;
	}
	n = room(ib) / FRB_RECT_FILL_DW;
	if (!n)
		return -ENOSPC;
	if (n > h)
		n = h;
	for (*rows = 0; *rows < n; (*rows)++, at += (uint64_t)dst->pitch * 4)
		emit_fill(ib, at, data, w * 4);
	return 0;
}

int frb_rect_emit_copy(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t dx, uint32_t dy,
		       const struct frb_rect_surface *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
	if (!frb_rect_surface_ok(dst) || !frb_rect_surface_ok(src) || !inside(dst, dx, dy, w, h) ||
	    !inside(src, sx, sy, w, h))
		return -EINVAL;
	if (room(ib) < FRB_RECT_COPY_DW)
		return -ENOSPC;
	emit_sub_window(ib, dst, dx, dy, src, sx, sy, w, h);
	return 0;
}

int frb_rect_emit_moves(struct frb_rect_ib *ib, const struct frb_rect_surface *s,
			const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n,
			int phase)
{
	uint32_t i;

	if (!frb_rect_surface_ok(s) || phase < 0 || phase > 2)
		return -EINVAL;
	if (phase && (!frb_rect_surface_ok(scratch) || scratch->width < s->width || scratch->height < s->height))
		return -EINVAL;
	for (i = 0; i < n; i++)
		if (!inside(s, m[i].sx, m[i].sy, m[i].w, m[i].h) || !inside(s, m[i].dx, m[i].dy, m[i].w, m[i].h))
			return -EINVAL;
	if (!phase && frb_rect_moves_overlap(m, n))
		return -EINVAL;
	if (room(ib) < n * FRB_RECT_COPY_DW)
		return -ENOSPC;
	for (i = 0; i < n; i++) {
		if (phase == 0)
			emit_sub_window(ib, s, m[i].dx, m[i].dy, s, m[i].sx, m[i].sy, m[i].w, m[i].h);
		else if (phase == 1)
			emit_sub_window(ib, scratch, m[i].sx, m[i].sy, s, m[i].sx, m[i].sy, m[i].w, m[i].h);
		else
			emit_sub_window(ib, s, m[i].dx, m[i].dy, scratch, m[i].sx, m[i].sy, m[i].w, m[i].h);
	}
	return 0;
}
