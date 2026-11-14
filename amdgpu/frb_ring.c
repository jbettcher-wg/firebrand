/*
 * See frb_ring.h: the amdgpu_ring / amdgpu_ib / amdgpu_fence / writeback
 * subset, with Linux's names in the comments. Polled fences only (no IH
 * ring yet): amdgpu_fence_wait_polling's arm.
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "frb_ring.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/oss/oss_3_0_d.h"

#define AMDGPU_SCHED_HW_SUBMISSION	2	/* amdgpu_sched_hw_submission's default */

static uint32_t roundup_pow2(uint32_t v)
{
	uint32_t p = 1;

	while (p < v)
		p <<= 1;
	return p;
}

/* --- VRAM objects ---------------------------------------------------------- */

int frb_vram_obj_alloc(struct frb_device *adev, uint32_t size, uint32_t align, uint32_t *bar_offset)
{
	struct frb_vram_layout *l = &adev->layout;
	uint32_t off;

	if (!align)
		align = 256;
	if (!l->objects_size)
		return -ENODEV;
	off = (l->objects + adev->obj_used + align - 1) & ~(align - 1);
	if (off + size > l->objects + l->objects_size) {
		frb_err(adev, "vram objects: %u bytes do not fit (%u of %u used)", size, adev->obj_used,
			l->objects_size);
		return -ENOMEM;
	}
	adev->obj_used = off + size - l->objects;
	*bar_offset = off;
	return 0;
}

/* --- HDP ------------------------------------------------------------------- */

void frb_flush_hdp(struct frb_device *adev)
{
	WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}

void frb_invalidate_hdp(struct frb_device *adev)
{
	WREG32(mmHDP_DEBUG0, 1);
	RREG32(mmHDP_DEBUG0);
}

/* --- writeback (amdgpu_device_wb_init / _get) ------------------------------- */

int frb_wb_init(struct frb_device *adev)
{
	uint32_t i;
	int r;

	if (adev->wb_offset)
		return 0;
	r = frb_vram_obj_alloc(adev, FRB_WB_SLOTS * 32, 4096, &adev->wb_offset);
	if (r)
		return r;
	adev->wb_used = 0;
	for (i = 0; i < FRB_WB_SLOTS * 8; i++)
		frb_vram_write32(adev, adev->wb_offset + 4 * i, 0);
	return 0;
}

int frb_wb_get(struct frb_device *adev, uint32_t *wb)
{
	uint32_t i;

	for (i = 0; i < FRB_WB_SLOTS; i++) {
		if (!(adev->wb_used & (1ULL << i))) {
			adev->wb_used |= 1ULL << i;
			*wb = i << 3;	/* amdgpu: the slot's first dword */
			return 0;
		}
	}
	return -EINVAL;
}

/* amdgpu_device_wb_free */
void frb_wb_free(struct frb_device *adev, uint32_t wb)
{
	wb >>= 3;
	if (wb < FRB_WB_SLOTS)
		adev->wb_used &= ~(1ULL << wb);
}

uint32_t frb_wb_read(struct frb_device *adev, uint32_t wb)
{
	frb_invalidate_hdp(adev);
	return frb_vram_read32(adev, adev->wb_offset + 4 * wb);
}

void frb_wb_write(struct frb_device *adev, uint32_t wb, uint32_t v)
{
	frb_vram_write32(adev, adev->wb_offset + 4 * wb, v);
}

uint64_t frb_wb_gpu_addr(struct frb_device *adev, uint32_t wb)
{
	return frb_vram_gpu_addr(adev, adev->wb_offset) + 4ULL * wb;
}

/* --- rings (amdgpu_ring.c) ------------------------------------------------- */

int frb_ring_init(struct frb_device *adev, struct frb_ring *ring, const char *name,
		  const struct frb_ring_funcs *funcs, uint32_t max_dw)
{
	uint32_t i;
	int r;

	r = frb_wb_init(adev);
	if (r)
		return r;
	memset(ring, 0, sizeof(*ring));
	ring->adev = adev;
	ring->funcs = funcs;
	for (i = 0; name[i] && i + 1 < sizeof(ring->name); i++)
		ring->name[i] = name[i];
	if (frb_wb_get(adev, &ring->rptr_offs) || frb_wb_get(adev, &ring->wptr_offs) ||
	    frb_wb_get(adev, &ring->fence_offs))
		return -EINVAL;
	ring->rptr_gpu_addr = frb_wb_gpu_addr(adev, ring->rptr_offs);
	ring->wptr_gpu_addr = frb_wb_gpu_addr(adev, ring->wptr_offs);
	ring->fence_gpu_addr = frb_wb_gpu_addr(adev, ring->fence_offs);
	frb_wb_write(adev, ring->fence_offs, 0);

	ring->ring_size = roundup_pow2(max_dw * 4 * AMDGPU_SCHED_HW_SUBMISSION);
	ring->buf_mask = (ring->ring_size / 4) - 1;
	ring->ptr_mask = funcs->support_64bit_ptrs ? 0xffffffffffffffffULL : ring->buf_mask;
	r = frb_vram_obj_alloc(adev, ring->ring_size, 4096, &ring->bar_offset);
	if (r)
		return r;
	ring->gpu_addr = frb_vram_gpu_addr(adev, ring->bar_offset);
	frb_info(adev, "ring %s: %u bytes at %llx (BAR0 0x%08x); wb rptr %u wptr %u fence %u",
		 ring->name, ring->ring_size, (unsigned long long)ring->gpu_addr, ring->bar_offset,
		 ring->rptr_offs, ring->wptr_offs, ring->fence_offs);
	return 0;
}

int frb_ring_alloc(struct frb_ring *ring, uint32_t ndw)
{
	ndw = (ndw + ring->funcs->align_mask) & ~ring->funcs->align_mask;
	if (ndw > ring->buf_mask)
		return -ENOMEM;
	ring->count_dw = ndw;
	ring->wptr_old = ring->wptr;
	return 0;
}

void frb_ring_write(struct frb_ring *ring, uint32_t v)
{
	frb_vram_write32(ring->adev, ring->bar_offset + 4 * (uint32_t)(ring->wptr & ring->buf_mask), v);
	ring->wptr++;
	ring->wptr &= ring->ptr_mask;
	ring->count_dw--;
}

static void insert_nop(struct frb_ring *ring, uint32_t count)
{
	uint32_t i;

	if (ring->funcs->insert_nop) {
		ring->funcs->insert_nop(ring, count);
		return;
	}
	for (i = 0; i < count; i++)
		frb_ring_write(ring, ring->funcs->nop);
}

/* amdgpu_ring_commit: pad to the fetch size, then the WPTR; the HDP flush first (VRAM rings) */
void frb_ring_commit(struct frb_ring *ring)
{
	uint32_t count = ring->funcs->align_mask + 1 - ((uint32_t)ring->wptr & ring->funcs->align_mask);

	count &= ring->funcs->align_mask;
	if (count)
		insert_nop(ring, count);
	frb_flush_hdp(ring->adev);
	ring->funcs->set_wptr(ring);
}

void frb_ring_undo(struct frb_ring *ring)
{
	ring->wptr = ring->wptr_old;
}

void frb_ring_clear(struct frb_ring *ring)
{
	uint32_t i;

	for (i = 0; i <= ring->buf_mask; i++)
		frb_vram_write32(ring->adev, ring->bar_offset + 4 * i, ring->funcs->nop);
}

void frb_ring_generic_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	while (ib->length_dw & ring->funcs->align_mask)
		frb_ib_write(ring->adev, ib, ring->funcs->nop);
}

/* --- IBs (amdgpu_ib.c) ----------------------------------------------------- */

int frb_ib_pool_init(struct frb_device *adev)
{
	if (adev->ib_pool_offset)
		return 0;
	adev->ib_pool_used = 0;
	return frb_vram_obj_alloc(adev, FRB_IB_POOL_SIZE, 4096, &adev->ib_pool_offset);
}

void frb_ib_pool_reset(struct frb_device *adev)
{
	adev->ib_pool_used = 0;
}

int frb_ib_get(struct frb_device *adev, uint32_t size_bytes, struct frb_ib *ib)
{
	uint32_t off;
	int r = frb_ib_pool_init(adev);

	if (r)
		return r;
	off = (adev->ib_pool_used + 255) & ~255u;
	if (off + size_bytes > FRB_IB_POOL_SIZE)
		return -ENOMEM;
	adev->ib_pool_used = off + size_bytes;
	memset(ib, 0, sizeof(*ib));
	ib->bar_offset = adev->ib_pool_offset + off;
	ib->gpu_addr = frb_vram_gpu_addr(adev, ib->bar_offset);
	ib->size_dw = size_bytes / 4;
	return 0;
}

void frb_ib_write(struct frb_device *adev, struct frb_ib *ib, uint32_t v)
{
	if (ib->length_dw < ib->size_dw) {
		if (ib->cpu)
			frb_put_le32(ib->cpu, 4 * ib->length_dw, v);	/* GTT: snooped, no flush */
		else
			frb_vram_write32(adev, ib->bar_offset + 4 * ib->length_dw, v);
	}
	ib->length_dw++;
}

/*
 * amdgpu_ib_free. The pool is a bump allocator (no fenced suballocator), so
 * only the newest IB can give its space back; call this once its fence has
 * signalled (never after a timeout: the engine may still fetch it). Older
 * space returns at frb_ib_pool_reset.
 */
void frb_ib_free(struct frb_device *adev, struct frb_ib *ib)
{
	uint32_t off = ib->bar_offset - adev->ib_pool_offset;

	if (ib->size_dw && off + ib->size_dw * 4 == adev->ib_pool_used)
		adev->ib_pool_used = off;
	ib->size_dw = 0;
}

/*
 * amdgpu_ib_schedule for one IB on a kernel ring (no job): the HDP flush
 * (amdgpu_device_flush_hdp: the ring's own packet, else vi_flush_hdp), the
 * IB, the HDP invalidate (amdgpu_device_invalidate_hdp -> vi_invalidate_hdp,
 * a register write from the ring when it has emit_wreg), then the fence.
 */
int frb_ib_schedule(struct frb_ring *ring, struct frb_ib *ib, uint32_t *seq)
{
	return frb_ib_schedule_deps(ring, ib, NULL, seq);
}

/*
 * The same frame, after the waits deps asks for: the pipeline sync first,
 * as amdgpu_ib_schedule emits it (need_pipe_sync), then the wait on the
 * other engine, then the HDP flush (unless deps says the commit's is
 * enough), the IB, the invalidate and the fence.
 */
int frb_ib_schedule_deps(struct frb_ring *ring, struct frb_ib *ib, const struct frb_ib_deps *deps,
			 uint32_t *seq)
{
	struct frb_device *adev = ring->adev;
	int r;

	if (!ring->ready)
		return -EINVAL;
	if (deps && ((deps->pipeline_sync && !ring->funcs->emit_pipeline_sync) ||
		     (deps->wait_addr && !ring->funcs->emit_wait_mem)))
		return -EINVAL;
	r = frb_ring_alloc(ring, 256);
	if (r)
		return r;
	if (deps && deps->pipeline_sync)
		ring->funcs->emit_pipeline_sync(ring);
	if (deps && deps->wait_addr)
		ring->funcs->emit_wait_mem(ring, deps->wait_addr, deps->wait_seq, FRB_WAIT_GEQUAL);
	/* no_hdp_flush: frb_ring_commit flushes the HDP by MMIO before the WPTR moves */
	if (!deps || !deps->no_hdp_flush) {
		if (ring->funcs->emit_hdp_flush)
			ring->funcs->emit_hdp_flush(ring);
		else if (ring->funcs->emit_wreg)
			ring->funcs->emit_wreg(ring, mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
		else
			frb_flush_hdp(adev);
	}
	ring->funcs->emit_ib(ring, ib, 0, ib->flags);
	if (ring->funcs->emit_wreg)
		ring->funcs->emit_wreg(ring, mmHDP_DEBUG0, 1);
	else
		frb_invalidate_hdp(adev);
	*seq = ++ring->sync_seq;
	ring->funcs->emit_fence(ring, ring->fence_gpu_addr, *seq, FRB_FENCE_FLAG_INT);
	frb_ring_commit(ring);
	return 0;
}

int frb_ring_fence_after(struct frb_ring *ring, uint64_t wait_addr, uint32_t wait_seq, uint32_t *seq)
{
	int r;

	if (!ring->ready || !ring->funcs->emit_wait_mem)
		return -EINVAL;
	r = frb_ring_alloc(ring, 64);
	if (r)
		return r;
	ring->funcs->emit_wait_mem(ring, wait_addr, wait_seq, FRB_WAIT_GEQUAL);
	*seq = ++ring->sync_seq;
	ring->funcs->emit_fence(ring, ring->fence_gpu_addr, *seq, FRB_FENCE_FLAG_INT);
	frb_ring_commit(ring);
	return 0;
}

/* --- fences (amdgpu_fence.c, polled) --------------------------------------- */

int frb_fence_emit(struct frb_ring *ring, uint32_t *seq, unsigned flags)
{
	*seq = ++ring->sync_seq;
	ring->funcs->emit_fence(ring, ring->fence_gpu_addr, *seq, flags | FRB_FENCE_FLAG_INT);
	return 0;
}

void frb_fence_driver_force_completion(struct frb_ring *ring)
{
	frb_wb_write(ring->adev, ring->fence_offs, ring->sync_seq);
	frb_flush_hdp(ring->adev);
}

uint32_t frb_fence_read(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->fence_offs);
}

/* amdgpu_fence_wait_polling: until the sequence passes seq (wrap-safe) */
int frb_fence_wait(struct frb_ring *ring, uint32_t seq, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	uint32_t t;

	for (t = 0; t <= timeout_us; t += 2) {
		if ((int32_t)(frb_fence_read(ring) - seq) >= 0)
			return 0;
		udelay(2);
	}
	return -ETIMEDOUT;
}
