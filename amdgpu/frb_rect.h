/*
 * Rectangles on the SDMA engine: the accelerator's screen fills, copies and
 * region moves, and its surface copies (docs/accel-interface.md G2). The
 * IBs are built here, in host memory, as little-endian dwords; the kext
 * submits them on SDMA0 (FirebrandAccel.cpp). Nothing here touches the card,
 * so the host tests run it against a model of the engine (tests/frb_unit.c).
 *
 * Packets (tonga_sdma_pkt_open.h, SDMA v3; vega10_sdma_pkt_open.h, SDMA v4):
 *   COPY_LINEAR_SUB_WINDOW  one rectangle, any pitches, 4-byte elements;
 *                           Mesa's cik_sdma_copy_texture layout for GFX8
 *                           (pitch, slice pitch and the extent minus one).
 *                           SDMA 4 moves the pitches to bits 31:13 (19 bits;
 *                           SDMA 3: 29:16, 14 bits), the rest unchanged
 *                           (Mesa's ac_emit_sdma_copy_linear_sub_window,
 *                           pitch_shift 13 from SDMA_4_0)
 *   CONSTANT_FILL           FILLSIZE 2: a dword fill (Mesa's
 *                           SDMA_CONSTANT_FILL_EXTRA_SIZE(2)); FILLSIZE 0, what
 *                           amdgpu uses, repeats the data's low byte
 *                           (frb_sdma_v3_0.c), which a pixel whose four bytes
 *                           differ cannot use. One packet a row, one for a
 *                           run of whole rows. The count is in bytes; SDMA 4
 *                           takes it less one (sdma_v4_0_emit_fill_buffer,
 *                           Mesa's ac_emit_sdma_constant_fill), SDMA 3 as it is.
 * The IB says which engine it is for (frb_rect_ib.sdma4; the kext sets it
 * from frb_is_soc15). Everything else here is the same on both.
 *
 * Sea Islands' SDMA (cik_sdma.c, cikd.h SDMA_PACKET; frb_rect_ib.cik): the
 * same opcodes and fields as SDMA 3 (SDMA_PACKET's sub-opcode in 15:8 and
 * extra bits in 31:16 are tonga's SUB_OP and header bits; FILLSIZE is
 * extra 15:14 = header 31:30), with two differences:
 *   COPY_LINEAR_SUB_WINDOW  the extent as it is, not less one, and the
 *                           depth 1 (Mesa's cik_sdma_copy_texture for GFX7;
 *                           ac_emit_sdma_copy_linear_sub_window takes one
 *                           off only from SDMA 2.4, VI's). The 14-bit
 *                           fields then hold 16383 at most, so a rectangle
 *                           16384 wide or high goes in two packets
 *   CONSTANT_FILL           packets of at most 0x1fffe0 bytes (cik_sdma's
 *                           fill_max_bytes is 0x1fffff); FILLSIZE 2, the count
 *                           as it is (cik_sdma_emit_fill_buffer's), as on
 *                           SDMA 3
 *
 * Southern Islands' DMA engine (si_dma.c, sid.h DMA_PACKET; frb_rect_ib.si_dma):
 *   COPY                    linear, a byte count (amdgpu's si_dma_emit_copy_buffer):
 *                           no sub-window packet, so one per row, or one for
 *                           a run of whole rows when both surfaces have the
 *                           rectangle's width as their pitch
 *   CONSTANT_FILL           a dword count of the dword `data` (si_dma's fill
 *                           repeats the whole dword), one per row or run
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
#define FRB_RECT_FILL_MAX_BYTES	0x3fffe0u	/* sdma_v3_0's fill_max_bytes (sdma_v4_0's is 1 << 22) */
#define FRB_RECT_SI_COPY_DW	5u		/* SI DMA COPY (one row or run) */
#define FRB_RECT_SI_FILL_DW	4u		/* SI DMA CONSTANT_FILL */
#define FRB_RECT_SI_MAX_BYTES	0xffff8u	/* si_dma's copy_max_bytes and fill_max_bytes */
#define FRB_RECT_CIK_FILL_MAX_BYTES 0x1fffe0u	/* cik_sdma's fill_max_bytes (0x1fffff), 32-byte aligned */
#define FRB_RECT_CIK_MAX_EXTENT	0x3fffu		/* CIK: the sub-window's 14-bit extent, not less one */

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
	int sdma4;			/* for SDMA 4 (SOC15): its pitch field and count less one */
	int si_dma;			/* for SI's DMA engine (si_dma.c): its COPY and CONSTANT_FILL */
	int cik;			/* for CIK's SDMA (cik_sdma.c): SDMA 3's packets, the extent as it is */
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

/*
 * The same, resumable, for moves that need more than one IB (SI's DMA has no
 * sub-window copy: a row a packet): from move *at, row *row on, as much as
 * fits; *at and *row say where the next IB goes on. -ENOSPC when nothing
 * fit; 0 when something did (or nothing was left).
 */
int frb_rect_emit_moves_from(struct frb_rect_ib *ib, const struct frb_rect_surface *s,
			     const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n,
			     int phase, uint32_t *at, uint32_t *row);
/* how many IBs of like's room and engine one phase of the moves takes, or -errno */
int frb_rect_moves_ibs(const struct frb_rect_ib *like, const struct frb_rect_surface *s,
		       const struct frb_rect_surface *scratch, const struct frb_rect_move *m, uint32_t n, int phase);

#endif
