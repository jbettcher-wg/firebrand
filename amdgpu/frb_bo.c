/*
 * See frb_bo.h: VRAM heaps and a client's buffer table for the
 * accelerator's legacy clients. Not thread-safe; the kext holds its lock.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"
#include "frb_bo.h"

#define FOUR_GB		0x100000000ULL

/* --- VRAM heaps ------------------------------------------------------------ */

int frb_vram_mgr_init(struct frb_vram_mgr *m, struct frb_os *os, uint64_t vis_start, uint64_t vis_end,
		      uint64_t invis_start, uint64_t invis_end)
{
	memset(m, 0, sizeof(*m));
	m->os = os;
	if (vis_end > vis_start) {
		if (frb_mem_init(&m->visible, os, vis_start, vis_end - vis_start))
			return -ENOMEM;
		m->vis_start = vis_start;
		m->vis_end = vis_end;
	}
	if (invis_end > invis_start) {
		if (frb_mem_init(&m->invisible, os, invis_start, invis_end - invis_start)) {
			frb_vram_mgr_fini(m);
			return -ENOMEM;
		}
		m->invis_start = invis_start;
		m->invis_end = invis_end;
	}
	m->ready = 1;
	return 0;
}

void frb_vram_mgr_fini(struct frb_vram_mgr *m)
{
	if (m->vis_end > m->vis_start)
		frb_mem_fini(&m->visible);
	if (m->invis_end > m->invis_start)
		frb_mem_fini(&m->invisible);
	m->vis_start = m->vis_end = m->invis_start = m->invis_end = 0;
	m->ready = 0;
}

int frb_vram_mgr_alloc(struct frb_vram_mgr *m, uint64_t size, uint64_t align, uint32_t where,
		       uint64_t *offset, uint64_t *size_out)
{
	struct frb_mem *heap;
	uint64_t lo, hi, off = 0;

	if (!m->ready || !size || size > (1ULL << 40) || (align & (align - 1)))
		return -EINVAL;
	if (align < FRB_VRAM_MIN_ALIGN)
		align = FRB_VRAM_MIN_ALIGN;
	size = (size + FRB_VRAM_MIN_ALIGN - 1) & ~(uint64_t)(FRB_VRAM_MIN_ALIGN - 1);
	if (where & FRB_VRAM_INVISIBLE) {
		heap = &m->invisible;
		lo = m->invis_start;
		hi = m->invis_end;
	} else {
		heap = &m->visible;
		lo = m->vis_start;
		hi = m->vis_end;
	}
	if ((where & FRB_VRAM_BELOW_4G) && hi > FOUR_GB)
		hi = FOUR_GB;
	if (hi <= lo || frb_mem_alloc_range(heap, size, align, lo, hi, &off))
		return -ENOMEM;
	if (where & FRB_VRAM_INVISIBLE)
		m->invis_used += size;
	else
		m->vis_used += size;
	*offset = off;
	if (size_out)
		*size_out = size;
	return 0;
}

void frb_vram_mgr_free(struct frb_vram_mgr *m, uint64_t offset, uint64_t size)
{
	if (!m->ready)
		return;
	if (m->invis_end > m->invis_start && offset >= m->invis_start && offset < m->invis_end) {
		frb_mem_free(&m->invisible, offset);
		m->invis_used -= size <= m->invis_used ? size : m->invis_used;
	} else if (m->vis_end > m->vis_start && offset >= m->vis_start && offset < m->vis_end) {
		frb_mem_free(&m->visible, offset);
		m->vis_used -= size <= m->vis_used ? size : m->vis_used;
	}
}

/* --- a client's buffers ------------------------------------------------------ */

void frb_bo_table_init(struct frb_bo_table *t, struct frb_os *os)
{
	memset(t, 0, sizeof(*t));
	t->os = os;
}

void frb_bo_table_fini(struct frb_bo_table *t)
{
	if (t->e)
		t->os->free(t->os->cookie, t->e);
	t->e = NULL;
	t->count = t->cap = 0;
}

/* the first entry whose offset is >= offset */
static uint32_t lower_bound(const struct frb_bo_table *t, uint64_t offset)
{
	uint32_t lo = 0, hi = t->count;

	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;

		if (t->e[mid].offset < offset)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int frb_bo_table_add(struct frb_bo_table *t, uint64_t offset, uint64_t size)
{
	uint32_t i;

	if (!size || offset + size < offset)
		return -EINVAL;
	i = lower_bound(t, offset);
	if ((i < t->count && t->e[i].offset < offset + size) ||
	    (i > 0 && t->e[i - 1].offset + t->e[i - 1].size > offset))
		return -EINVAL;
	if (t->count == t->cap) {
		uint32_t cap = t->cap ? t->cap * 2 : 64;
		struct frb_bo_entry *e;

		if (cap < t->cap || cap > (1u << 24))
			return -ENOMEM;
		e = (struct frb_bo_entry *)t->os->alloc(t->os->cookie, cap * sizeof(*e));
		if (!e)
			return -ENOMEM;
		if (t->count)
			memcpy(e, t->e, t->count * sizeof(*e));
		if (t->e)
			t->os->free(t->os->cookie, t->e);
		t->e = e;
		t->cap = cap;
	}
	if (i < t->count)
		memmove(&t->e[i + 1], &t->e[i], (t->count - i) * sizeof(*t->e));
	t->e[i].offset = offset;
	t->e[i].size = size;
	t->count++;
	return 0;
}

int frb_bo_table_remove(struct frb_bo_table *t, uint64_t offset, uint64_t *size)
{
	uint32_t i = lower_bound(t, offset);

	if (i >= t->count || t->e[i].offset != offset)
		return -ENOENT;
	if (size)
		*size = t->e[i].size;
	t->count--;
	if (i < t->count)
		memmove(&t->e[i], &t->e[i + 1], (t->count - i) * sizeof(*t->e));
	return 0;
}

int frb_bo_table_contains(const struct frb_bo_table *t, uint64_t offset, uint64_t len)
{
	uint32_t i;

	if (!len || offset + len < offset)
		return 0;
	/* the last entry that starts at or below offset */
	i = lower_bound(t, offset + 1);
	if (!i)
		return 0;
	i--;
	return offset >= t->e[i].offset && offset + len <= t->e[i].offset + t->e[i].size;
}
