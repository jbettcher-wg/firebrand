/*
 * See frb_rect.h: rectangles on SDMA v3 and v4 for the accelerator's 2D
 * work. The clipping and the order-independent move follow RadeonNI
 * (RadeonNIAccel.cpp screenClip/screenCopy, hw/rdn_cp.c rdn_cp_copy_rects);
 * the packets are SDMA v3's (tonga_sdma_pkt_open.h), laid out as Mesa's
 * radeonsi emits them for GFX8 (cik_sdma.c, si_sdma_clear_buffer), with
 * SDMA v4's two differences (vega10_sdma_pkt_open.h; ac_cmdbuf_sdma.c):
 * the sub-window's pitch field and the fill's count less one, and CIK's
 * (cikd.h; Mesa's cik_sdma.c for GFX7): the sub-window's extent as it is.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_rect.h"
#include "include/tonga_sdma_pkt_open.h"

/*
 * vega10_sdma_pkt_open.h's SDMA_PKT_COPY_LINEAR_SUBWIN_DW_4_SRC_PITCH and
 * DW_9_DST_PITCH (the header cannot be included beside tonga's: the same
 * names): 19 bits at 13. Every other field this file uses is laid out the
 * same in both headers.
 */
#define SDMA4_SUBWIN_PITCH(x)	(((x) & 0x0007FFFFu) << 13)

/* sid.h's DMA packets (SI), kept here so this file needs no register headers */
#define SI_DMA_PACKET(cmd, b, t, s, n)	((((cmd) & 0xF) << 28) | (((b) & 0x1) << 26) | \
					 (((t) & 0x1) << 23) | (((s) & 0x1) << 22) | (((n) & 0xFFFFF) << 0))
#define SI_DMA_PACKET_COPY		0x3
#define SI_DMA_PACKET_CONSTANT_FILL	0xd

static void emit(struct frb_rect_ib *ib, uint32_t v)
{
	if (ib->buf)	/* no buffer: only count (frb_rect_moves_ibs) */
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
 * CONSTANT_FILL, FILLSIZE 2: count bytes (a multiple of 4) of the dword
 * `data`, stored little-endian. SDMA 3 takes the count as it is (GFX8),
 * SDMA 4 less one (amdgpu's and Mesa's fills from SDMA 4.0 on).
 */
static void emit_fill(struct frb_rect_ib *ib, uint64_t dst, uint32_t data, uint32_t bytes)
{
	bytes &= ~3u;
	emit(ib, SDMA_PKT_CONSTANT_FILL_HEADER_OP(SDMA_OP_CONST_FILL) |
		 SDMA_PKT_CONSTANT_FILL_HEADER_FILLSIZE(2));
	emit(ib, (uint32_t)dst);
	emit(ib, (uint32_t)(dst >> 32));
	emit(ib, data);
	emit(ib, SDMA_PKT_CONSTANT_FILL_COUNT_COUNT(ib->sdma4 ? bytes - 1 : bytes));
}

/* DW_4 / DW_9: z 0 and the pitch less one, in the engine's field */
static uint32_t subwin_pitch(const struct frb_rect_ib *ib, uint32_t pitch)
{
	if (ib->sdma4)
		return SDMA4_SUBWIN_PITCH(pitch - 1);
	return SDMA_PKT_COPY_LINEAR_SUBWIN_DW_4_SRC_PITCH(pitch - 1);
}

/*
 * COPY_LINEAR_SUB_WINDOW with 4-byte elements: x, y in elements, pitches
 * and the extent minus one (GFX8 and GFX9; GFX7, ib->cik, takes the extent
 * and the depth as they are: w, h, 1), depth 1, no swap, no cache hints.
 * SDMA 3's DW_4 and DW_9 pitch fields are laid out alike (SRC_PITCH stands
 * for both); SDMA 4's: subwin_pitch. CIK's w and h are at most
 * FRB_RECT_CIK_MAX_EXTENT (emit_rect_copy cuts).
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
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_4_SRC_Z(0) | subwin_pitch(ib, src->pitch));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_5_SRC_SLICE_PITCH(src->pitch * src->height - 1));
	emit(ib, (uint32_t)dst->gpu_addr);
	emit(ib, (uint32_t)(dst->gpu_addr >> 32));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_8_DST_X(dx) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_8_DST_Y(dy));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_9_DST_Z(0) | subwin_pitch(ib, dst->pitch));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_10_DST_SLICE_PITCH(dst->pitch * dst->height - 1));
	if (ib->cik) {
		emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_X(w) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_Y(h));
		emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_12_RECT_Z(1));
		return;
	}
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_X(w - 1) | SDMA_PKT_COPY_LINEAR_SUBWIN_DW_11_RECT_Y(h - 1));
	emit(ib, SDMA_PKT_COPY_LINEAR_SUBWIN_DW_12_RECT_Z(0));
}

/* CIK: how many sub-windows an extent of n takes (its field holds 16383 at most); 1 elsewhere */
static uint32_t cik_pieces(const struct frb_rect_ib *ib, uint32_t n)
{
	if (!ib->cik || n <= FRB_RECT_CIK_MAX_EXTENT)
		return 1;
	return (n + FRB_RECT_CIK_MAX_EXTENT - 1) / FRB_RECT_CIK_MAX_EXTENT;
}

/* SI DMA: a linear copy of `bytes` (si_dma_emit_copy_buffer) */
static void emit_si_copy(struct frb_rect_ib *ib, uint64_t dst, uint64_t src, uint32_t bytes)
{
	emit(ib, SI_DMA_PACKET(SI_DMA_PACKET_COPY, 1, 0, 0, bytes));
	emit(ib, (uint32_t)dst);
	emit(ib, (uint32_t)src);
	emit(ib, (uint32_t)(dst >> 32) & 0xff);
	emit(ib, (uint32_t)(src >> 32) & 0xff);
}

/* SI DMA: `bytes` (a multiple of 4) of the dword `data` (si_dma_emit_fill_buffer) */
static void emit_si_fill(struct frb_rect_ib *ib, uint64_t dst, uint32_t data, uint32_t bytes)
{
	emit(ib, SI_DMA_PACKET(SI_DMA_PACKET_CONSTANT_FILL, 0, 0, 0, bytes / 4));
	emit(ib, (uint32_t)dst);
	emit(ib, data);
	emit(ib, (uint32_t)(dst >> 32) << 16);
}

/* the dwords a rectangle copy takes on this IB's engine */
static uint32_t copy_dw(const struct frb_rect_ib *ib, const struct frb_rect_surface *dst,
			const struct frb_rect_surface *src, uint32_t dx, uint32_t sx, uint32_t w, uint32_t h)
{
	uint64_t bytes;

	if (!ib->si_dma)
		return FRB_RECT_COPY_DW * cik_pieces(ib, w) * cik_pieces(ib, h);
	if (dx == 0 && sx == 0 && w == dst->pitch && w == src->pitch) {
		bytes = (uint64_t)w * h * 4;
		return (uint32_t)((bytes + FRB_RECT_SI_MAX_BYTES - 1) / FRB_RECT_SI_MAX_BYTES) * FRB_RECT_SI_COPY_DW;
	}
	return h * FRB_RECT_SI_COPY_DW;	/* a row is at most 16384 * 4 bytes: one packet */
}

/* one rectangle, as copy_dw counted it */
static void emit_rect_copy(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t dx, uint32_t dy,
			   const struct frb_rect_surface *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
	uint64_t d = dst->gpu_addr + ((uint64_t)dy * dst->pitch + dx) * 4;
	uint64_t s = src->gpu_addr + ((uint64_t)sy * src->pitch + sx) * 4;
	uint32_t i;

	if (!ib->si_dma) {
		uint32_t cx, cy, cw, ch;

		/* one packet; on CIK, one per piece of at most 16383 a side (cik_pieces) */
		for (cy = 0; cy < h; cy += ch) {
			ch = ib->cik && h - cy > FRB_RECT_CIK_MAX_EXTENT ? FRB_RECT_CIK_MAX_EXTENT : h - cy;
			for (cx = 0; cx < w; cx += cw) {
				cw = ib->cik && w - cx > FRB_RECT_CIK_MAX_EXTENT ? FRB_RECT_CIK_MAX_EXTENT : w - cx;
				emit_sub_window(ib, dst, dx + cx, dy + cy, src, sx + cx, sy + cy, cw, ch);
			}
		}
		return;
	}
	if (dx == 0 && sx == 0 && w == dst->pitch && w == src->pitch) {
		uint64_t left = (uint64_t)w * h * 4;

		while (left) {
			uint32_t bytes = left > FRB_RECT_SI_MAX_BYTES ? FRB_RECT_SI_MAX_BYTES : (uint32_t)left;

			emit_si_copy(ib, d, s, bytes);
			d += bytes;
			s += bytes;
			left -= bytes;
		}
		return;
	}
	for (i = 0; i < h; i++, d += (uint64_t)dst->pitch * 4, s += (uint64_t)src->pitch * 4)
		emit_si_copy(ib, d, s, w * 4);
}

int frb_rect_emit_fill(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t pixel, uint32_t *rows)
{
	uint64_t at;
	uint32_t data, n, fill_dw, max_bytes;

	*rows = 0;
	if (!frb_rect_surface_ok(dst) || !inside(dst, x, y, w, h))
		return -EINVAL;
	/* the engine stores the dword little-endian; the pixel's bytes as this host stores it */
	data = frb_get_le32(&pixel, 0);
	at = dst->gpu_addr + ((uint64_t)y * dst->pitch + x) * 4;
	fill_dw = ib->si_dma ? FRB_RECT_SI_FILL_DW : FRB_RECT_FILL_DW;
	max_bytes = ib->si_dma ? FRB_RECT_SI_MAX_BYTES : ib->cik ? FRB_RECT_CIK_FILL_MAX_BYTES : FRB_RECT_FILL_MAX_BYTES;
	if (x == 0 && w == dst->pitch) {
		/* whole rows: one run, in packets of at most fill_max_bytes */
		uint64_t left = (uint64_t)h * w * 4;
		uint32_t need = (uint32_t)((left + max_bytes - 1) / max_bytes);

		if (room(ib) < need * fill_dw)
			return -ENOSPC;
		while (left) {
			uint32_t bytes = left > max_bytes ? max_bytes : (uint32_t)left;

			if (ib->si_dma)
				emit_si_fill(ib, at, data, bytes);
			else
				emit_fill(ib, at, data, bytes);
			at += bytes;
			left -= bytes;
		}
		*rows = h;
		return 0;
	}
	n = room(ib) / fill_dw;
	if (!n)
		return -ENOSPC;
	if (n > h)
		n = h;
	for (*rows = 0; *rows < n; (*rows)++, at += (uint64_t)dst->pitch * 4) {
		if (ib->si_dma)
			emit_si_fill(ib, at, data, w * 4);
		else
			emit_fill(ib, at, data, w * 4);
	}
	return 0;
}

int frb_rect_emit_copy(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t dx, uint32_t dy,
		       const struct frb_rect_surface *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h)
{
	if (!frb_rect_surface_ok(dst) || !frb_rect_surface_ok(src) || !inside(dst, dx, dy, w, h) ||
	    !inside(src, sx, sy, w, h))
		return -EINVAL;
	if (room(ib) < copy_dw(ib, dst, src, dx, sx, w, h))
		return -ENOSPC;
	emit_rect_copy(ib, dst, dx, dy, src, sx, sy, w, h);
	return 0;
}

int frb_rect_emit_moves(struct frb_rect_ib *ib, const struct frb_rect_surface *s,
			const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n,
			int phase)
{
	uint32_t i, need;

	if (!frb_rect_surface_ok(s) || phase < 0 || phase > 2)
		return -EINVAL;
	if (phase && (!frb_rect_surface_ok(scratch) || scratch->width < s->width || scratch->height < s->height))
		return -EINVAL;
	for (i = 0; i < n; i++)
		if (!inside(s, m[i].sx, m[i].sy, m[i].w, m[i].h) || !inside(s, m[i].dx, m[i].dy, m[i].w, m[i].h))
			return -EINVAL;
	if (!phase && frb_rect_moves_overlap(m, n))
		return -EINVAL;
	need = 0;
	for (i = 0; i < n; i++) {
		if (phase == 0)
			need += copy_dw(ib, s, s, m[i].dx, m[i].sx, m[i].w, m[i].h);
		else if (phase == 1)
			need += copy_dw(ib, scratch, s, m[i].sx, m[i].sx, m[i].w, m[i].h);
		else
			need += copy_dw(ib, s, scratch, m[i].dx, m[i].sx, m[i].w, m[i].h);
	}
	if (room(ib) < need)
		return -ENOSPC;
	for (i = 0; i < n; i++) {
		if (phase == 0)
			emit_rect_copy(ib, s, m[i].dx, m[i].dy, s, m[i].sx, m[i].sy, m[i].w, m[i].h);
		else if (phase == 1)
			emit_rect_copy(ib, scratch, m[i].sx, m[i].sy, s, m[i].sx, m[i].sy, m[i].w, m[i].h);
		else
			emit_rect_copy(ib, s, m[i].dx, m[i].dy, scratch, m[i].sx, m[i].sy, m[i].w, m[i].h);
	}
	return 0;
}

/* one move of a phase: its destination and source surfaces and corners */
static void phase_move(const struct frb_rect_surface *s, const struct frb_rect_surface *scratch,
		       const struct frb_rect_move *m, int phase, const struct frb_rect_surface **dst, uint32_t *dx,
		       uint32_t *dy, const struct frb_rect_surface **src, uint32_t *sx, uint32_t *sy)
{
	*dst = phase == 1 ? scratch : s;
	*src = phase == 2 ? scratch : s;
	*dx = phase == 1 ? m->sx : m->dx;
	*dy = phase == 1 ? m->sy : m->dy;
	*sx = m->sx;
	*sy = m->sy;
}

int frb_rect_emit_moves_from(struct frb_rect_ib *ib, const struct frb_rect_surface *s,
			     const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n,
			     int phase, uint32_t *at, uint32_t *row)
{
	const struct frb_rect_surface *dst, *src;
	uint32_t i, dx, dy, sx, sy, before = ib->len_dw;

	if (!frb_rect_surface_ok(s) || phase < 0 || phase > 2 || *at > n)
		return -EINVAL;
	if (phase && (!frb_rect_surface_ok(scratch) || scratch->width < s->width || scratch->height < s->height))
		return -EINVAL;
	for (i = 0; i < n; i++)
		if (!inside(s, m[i].sx, m[i].sy, m[i].w, m[i].h) || !inside(s, m[i].dx, m[i].dy, m[i].w, m[i].h))
			return -EINVAL;
	if (!phase && frb_rect_moves_overlap(m, n))
		return -EINVAL;
	while (*at < n) {
		const struct frb_rect_move *mv = &m[*at];
		uint32_t left = mv->h - *row, k, need;

		phase_move(s, scratch, mv, phase, &dst, &dx, &dy, &src, &sx, &sy);
		need = copy_dw(ib, dst, src, dx, sx, mv->w, left);
		if (room(ib) >= need) {
			emit_rect_copy(ib, dst, dx, dy + *row, src, sx, sy + *row, mv->w, left);
			(*at)++;
			*row = 0;
			continue;
		}
		/* SI's DMA copies a row a packet: as many rows as fit, the rest in the next IB */
		if (!ib->si_dma)
			break;
		k = room(ib) / FRB_RECT_SI_COPY_DW;
		if (k > left)
			k = left;
		if (!k)
			break;
		emit_rect_copy(ib, dst, dx, dy + *row, src, sx, sy + *row, mv->w, k);
		*row += k;
		break;
	}
	return *at < n && ib->len_dw == before ? -ENOSPC : 0;
}

int frb_rect_moves_ibs(const struct frb_rect_ib *like, const struct frb_rect_surface *s,
		       const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n, int phase)
{
	struct frb_rect_ib ib = *like;
	uint32_t at = 0, row = 0;
	int count = 0, r;

	ib.buf = 0;
	while (at < n) {
		ib.len_dw = 0;
		r = frb_rect_emit_moves_from(&ib, s, scratch, m, n, phase, &at, &row);
		if (r)
			return r;
		count++;
	}
	return count;
}
