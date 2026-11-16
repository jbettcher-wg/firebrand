/*
 * Buffer placement for the accelerator (docs/accel-interface.md 3.5): the
 * part of amdgpu_vram_mgr.c / amdgpu_object.c the legacy clients need.
 *
 *   frb_vram_mgr   VRAM the clients allocate from, in two heaps: the
 *                  CPU-visible one (between the scanout surfaces and the
 *                  driver's own region at the top of the BAR0 window) and
 *                  the invisible one (from the window's end to the end of
 *                  VRAM). Every buffer is one contiguous range; no buddy
 *                  splitting, no eviction. shared/frb_mem.c (RadeonNI's
 *                  range allocator) underneath.
 *   frb_bo_table   one client's buffers, sorted by offset: what it may free
 *                  and where its command buffers may lie.
 *
 * Offsets count from the start of VRAM (BAR0 offset 0); the GPU address of
 * an offset is gmc.vram_start + offset (VMID 0, the system aperture).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_BO_H
#define FRB_BO_H

#include "../shared/frb_os.h"
#include "../shared/frb_mem.h"

/* where a buffer may go */
#define FRB_VRAM_VISIBLE	(1u << 0)	/* inside the CPU window */
#define FRB_VRAM_INVISIBLE	(1u << 1)	/* beyond it (falls back to nothing: no eviction) */
#define FRB_VRAM_BELOW_4G	(1u << 2)	/* offset + size <= 4 GB (32-bit offsets) */

#define FRB_VRAM_MIN_ALIGN	4096u

struct frb_vram_mgr {
	struct frb_os *os;
	struct frb_mem visible, invisible;
	uint64_t vis_start, vis_end;		/* [start, end); end == start: none */
	uint64_t invis_start, invis_end;
	uint64_t vis_used, invis_used;		/* bytes handed out */
	int ready;
};

/* 0, or -ENOMEM. A heap with end <= start is left out. */
int frb_vram_mgr_init(struct frb_vram_mgr *m, struct frb_os *os, uint64_t vis_start, uint64_t vis_end,
		      uint64_t invis_start, uint64_t invis_end);
void frb_vram_mgr_fini(struct frb_vram_mgr *m);
/*
 * size is rounded up to 4 KB, align to at least 4 KB (a power of two, else
 * -EINVAL). where: FRB_VRAM_VISIBLE or FRB_VRAM_INVISIBLE, plus
 * FRB_VRAM_BELOW_4G. *size_out gets the rounded size.
 */
int frb_vram_mgr_alloc(struct frb_vram_mgr *m, uint64_t size, uint64_t align, uint32_t where,
		       uint64_t *offset, uint64_t *size_out);
/*
 * frb_vram_mgr_alloc with two more limits, for UVD before 5.0 (SI, CIK),
 * which takes no buffer across a 256 MB segment and wants its message and
 * feedback in the VCPU's (amdgpu_uvd_cs_pass2): the buffer lies inside one
 * window of `window` bytes (a power of two) of GPU address, gpu_base +
 * offset, and inside the VRAM offsets [lo, hi) (hi == 0: no such limit).
 * The windows are tried lowest first.
 */
int frb_vram_mgr_alloc_windowed(struct frb_vram_mgr *m, uint64_t size, uint64_t align, uint32_t where,
				uint64_t gpu_base, uint64_t window, uint64_t lo, uint64_t hi,
				uint64_t *offset, uint64_t *size_out);
/* gives back what frb_vram_mgr_alloc handed out at offset, size bytes */
void frb_vram_mgr_free(struct frb_vram_mgr *m, uint64_t offset, uint64_t size);

struct frb_bo_entry {
	uint64_t offset, size;
};

struct frb_bo_table {
	struct frb_os *os;
	struct frb_bo_entry *e;			/* sorted by offset, never overlapping */
	uint32_t count, cap;
};

void frb_bo_table_init(struct frb_bo_table *t, struct frb_os *os);
void frb_bo_table_fini(struct frb_bo_table *t);
/* 0, -EINVAL (overlaps an entry) or -ENOMEM */
int frb_bo_table_add(struct frb_bo_table *t, uint64_t offset, uint64_t size);
/* the entry that starts at offset: 0 and its size, or -ENOENT */
int frb_bo_table_remove(struct frb_bo_table *t, uint64_t offset, uint64_t *size);
/* does [offset, offset + len) lie inside one entry? (len > 0) */
int frb_bo_table_contains(const struct frb_bo_table *t, uint64_t offset, uint64_t len);

#endif
