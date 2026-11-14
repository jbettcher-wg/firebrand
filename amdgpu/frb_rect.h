/*
 * Rectangles on the SDMA engine: the accelerator's screen fills, copies and
 * region moves, and its surface copies (docs/accel-interface.md G2). The
 * IBs are built here, in host memory, as little-endian dwords; the kext
 * submits them on SDMA0 (FirebrandAccel.cpp). Nothing here touches the card,
 * so the host tests run it against a model of the engine (tests/frb_unit.c).
 *
 * Packets (tonga_sdma_pkt_open.h, SDMA v3):
 *   COPY_LINEAR_SUB_WINDOW  one rectangle, any pitches, 4-byte elements;
 *                           Mesa's cik_sdma_copy_texture layout for GFX8
 *                           (pitch, slice pitch and the extent minus one)
 *   CONSTANT_FILL           FILLSIZE 2: a dword fill (Mesa's
 *                           SDMA_CONSTANT_FILL_EXTRA_SIZE(2)); FILLSIZE 0, what
 *                           amdgpu uses, repeats the data's low byte
 *                           (frb_sdma_v3_0.c), which a pixel whose four bytes
 *                           differ cannot use. One packet a row, one for a
 *                           run of whole rows.
 *
 * Surfaces are 32 bits a pixel, linear, at dword-aligned GPU addresses. The
 * engine copies bytes unchanged, so a surface keeps the CPU's byte order
 * (the scanout is big-endian XRGB, swapped 8-in-32 by the display).
 *
 * MOVES WITHIN ONE SURFACE. The engine gives no order between the reads
 * and writes of one packet, or of two packets in one IB. When no
 * destination meets a source, every rectangle goes straight across. Else
 * the moves go through a scratch surface the size of this one (RadeonNI's
 * rdn_blit_move, rdn_cp_copy_rects): phase 1 copies every source to the
 * same place in the scratch, phase 2 copies each from there to its
 * destination. The two phases are separate IBs with a pipeline sync between
 * them, so the order of the rectangles cannot matter.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_RECT_H
#define FRB_RECT_H

#include "../shared/frb_os.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOSPC
#define ENOSPC 28
#endif

#define FRB_RECT_MAX		16384u		/* pitch, width, height, x + w, y + h: the packet's 14-bit fields */
#define FRB_RECT_COPY_DW	13u		/* COPY_LINEAR_SUB_WINDOW */
#define FRB_RECT_FILL_DW	5u		/* CONSTANT_FILL */
#define FRB_RECT_FILL_MAX_BYTES	0x3fffe0u	/* sdma_v3_0's fill_max_bytes */

struct frb_rect_surface {
	uint64_t gpu_addr;		/* pixel (0, 0) */
	uint32_t pitch;			/* pixels a row */
	uint32_t width, height;
};

/* a source rectangle and where it goes, inside the surfaces (after clipping) */
struct frb_rect_move {
	uint32_t sx, sy, dx, dy, w, h;
};

/* an IB being built: little-endian dwords in host memory */
struct frb_rect_ib {
	uint8_t *buf;
	uint32_t len_dw, cap_dw;
};

/* usable: dword-aligned, pitch >= width, every field inside the packet's */
int frb_rect_surface_ok(const struct frb_rect_surface *s);

/*
 * Cut a rectangle to the surface (RadeonNI's screenClip); 0 if nothing is
 * left. Coordinates and sizes must lie in [-32768, 32767] (the caller
 * checks; RadeonNI's screenRange), so no sum overflows.
 */
int frb_rect_clip(const struct frb_rect_surface *s, int32_t *x, int32_t *y, int32_t *w, int32_t *h);
/*
 * A copy inside s: cut the destination and carry the source along, then
 * the source and carry the destination (RadeonNI's screenCopy); 0 if
 * nothing is left.
 */
int frb_rect_clip_copy(const struct frb_rect_surface *s, int32_t *sx, int32_t *sy, int32_t *dx, int32_t *dy,
		       int32_t *w, int32_t *h);
/* does any move's destination meet any move's source (its own included)? */
int frb_rect_moves_overlap(const struct frb_rect_move *m, uint32_t n);

/*
 * Fill rows [y, y + h) of the rectangle at x, w pixels wide, with pixel:
 * the 32-bit value as this host stores it (the engine stores a dword
 * little-endian, so on a big-endian host the data is byte-swapped here).
 * As many rows as fit in the IB are emitted; *rows gets their count (a
 * run of whole rows counts as all of them). -ENOSPC: not even one fits.
 */
int frb_rect_emit_fill(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t x, uint32_t y,
		       uint32_t w, uint32_t h, uint32_t pixel, uint32_t *rows);
/* one rectangle from src to dst (different surfaces, or ones that do not overlap); -ENOSPC, -EINVAL */
int frb_rect_emit_copy(struct frb_rect_ib *ib, const struct frb_rect_surface *dst, uint32_t dx, uint32_t dy,
		       const struct frb_rect_surface *src, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h);
/*
 * Moves inside surface s (see above). phase 0: straight across (only when
 * !frb_rect_moves_overlap); 1: every source to the scratch; 2: from the
 * scratch to every destination. The scratch must be at least as wide and
 * high as s. All or nothing: -ENOSPC when they do not all fit.
 */
int frb_rect_emit_moves(struct frb_rect_ib *ib, const struct frb_rect_surface *s,
			const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n,
			int phase);

#endif
