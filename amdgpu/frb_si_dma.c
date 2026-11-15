/*
 * si_dma.c (Linux 7.0.2): Southern Islands' two async DMA engines, the
 * sw_init and hw_init (si_dma_start with the ring tests), the IB test, the
 * buffer functions (copy and constant fill) and hw_fini (si_dma_stop).
 *
 * NOT PORTED: interrupts (the trap sources 224/244), clock and power gating
 * (set_clockgating_state / set_powergating_state, from late init), the VM
 * PTE functions (one GART, no per-client page tables), soft reset (amdgpu
 * has none either).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - PLACEMENT: rings, writeback and IBs in VRAM (frb_ring.c), as on GFX.
 * - BYTE ORDER: rings and IBs are little-endian, so DMA_GFX_RB_CNTL's
 *   RB_SWAP_ENABLE / RPTR_WRITEBACK_SWAP_ENABLE and DMA_GFX_IB_CNTL's
 *   IB_SWAP_ENABLE stay 0, where amdgpu sets them under __BIG_ENDIAN. The
 *   read pointer then lands in the writeback slot little-endian, as
 *   frb_wb_read reads it.
 * - The fill is a dword fill on this engine (CONSTANT_FILL repeats src_data
 *   whole; SDMA 3 repeats its low byte).
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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
#include <stddef.h>
#include "frb_ring.h"
#include "include/sid.h"
#include "include/asic_reg/oss/oss_1_0_d.h"
#include "include/asic_reg/oss/oss_1_0_sh_mask.h"

#define FRB_SI_DMA_TEST_BYTES	(256u << 10)
#define FRB_SI_DMA_TEST_GUARD	256u	/* bytes left untouched at the end of the target */
#define FRB_SI_DMA_POISON	0xdeadd00du

/* si_dma_buffer_funcs */
#define SI_DMA_COPY_MAX_BYTES	0xffff8
#define SI_DMA_COPY_NUM_DW	5
#define SI_DMA_FILL_MAX_BYTES	0xffff8
#define SI_DMA_FILL_NUM_DW	4

struct frb_si_dma {
	struct frb_ring ring[SDMA_MAX_INSTANCE];
	int num_instances;
	int touched;			/* hw_init wrote the engines: hw_fini must stop them */
	uint32_t test_bo[2];
	uint32_t test_runs;
};

static const uint32_t sdma_offsets[SDMA_MAX_INSTANCE] =
{
	DMA0_REGISTER_OFFSET,
	DMA1_REGISTER_OFFSET
};

static inline uint32_t lower_32_bits(uint64_t v) { return (uint32_t)v; }
static inline uint32_t upper_32_bits(uint64_t v) { return (uint32_t)(v >> 32); }

static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

static uint32_t ring_me(struct frb_ring *ring)
{
	return (uint32_t)ring->me;
}

static uint64_t si_dma_ring_get_rptr(struct frb_ring *ring)
{
	/* the writeback holds the byte offset (radeon r600_dma_get_rptr's mask) */
	return (frb_wb_read(ring->adev, ring->rptr_offs) & 0x3fffc) >> 2;
}

static uint64_t si_dma_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return (RREG32(mmDMA_GFX_RB_WPTR + sdma_offsets[ring_me(ring)]) & 0x3fffc) >> 2;
}

static void si_dma_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmDMA_GFX_RB_WPTR + sdma_offsets[ring_me(ring)], (uint32_t)(ring->wptr << 2) & 0x3fffc);
}

static void si_dma_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib, uint32_t vmid, uint32_t flags)
{
	(void)flags;
	/* The indirect buffer packet must end on an 8 DW boundary in the DMA ring.
	 * Pad as necessary with NOPs.
	 */
	while ((lower_32_bits(ring->wptr) & 7) != 5)
		frb_ring_write(ring, DMA_PACKET(DMA_PACKET_NOP, 0, 0, 0, 0));
	frb_ring_write(ring, DMA_IB_PACKET(DMA_PACKET_INDIRECT_BUFFER, vmid, 0));
	frb_ring_write(ring, (uint32_t)(ib->gpu_addr & 0xFFFFFFE0));
	frb_ring_write(ring, (ib->length_dw << 12) | (upper_32_bits(ib->gpu_addr) & 0xFF));
}

static void si_dma_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;

	/* write the fence */
	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_FENCE, 0, 0, 0, 0));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xff));
	frb_ring_write(ring, (uint32_t)seq);
	/* optionally write high bits as well */
	if (write64bit) {
		addr += 4;
		frb_ring_write(ring, DMA_PACKET(DMA_PACKET_FENCE, 0, 0, 0, 0));
		frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
		frb_ring_write(ring, (upper_32_bits(addr) & 0xff));
		frb_ring_write(ring, upper_32_bits(seq));
	}
	/* generate an interrupt */
	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_TRAP, 0, 0, 0, 0));
}

static void si_dma_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	while (ib->length_dw & 0x7)
		frb_ib_write(ring->adev, ib, DMA_PACKET(DMA_PACKET_NOP, 0, 0, 0, 0));
}

static void si_dma_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	/* wait for idle */
	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_POLL_REG_MEM, 0, 0, 0, 0) |
			  (1 << 27)); /* Poll memory */
	frb_ring_write(ring, lower_32_bits(addr));
	frb_ring_write(ring, (0xff << 16) | upper_32_bits(addr)); /* retry, addr_hi */
	frb_ring_write(ring, 0xffffffff); /* mask */
	frb_ring_write(ring, seq); /* value */
	frb_ring_write(ring, (3 << 28) | 0x20); /* func(equal) | poll interval */
}

/* emit_pipeline_sync's poll with another address and compare (FRB_WAIT_*): another engine's fence */
static void si_dma_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_POLL_REG_MEM, 0, 0, 0, 0) | (1 << 27));
	frb_ring_write(ring, lower_32_bits(addr));
	frb_ring_write(ring, (0xff << 16) | upper_32_bits(addr));
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, (func << 28) | 0x20);
}

static void si_dma_ring_emit_wreg(struct frb_ring *ring, uint32_t reg, uint32_t val)
{
	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_SRBM_WRITE, 0, 0, 0, 0));
	frb_ring_write(ring, (0xf << 16) | reg);
	frb_ring_write(ring, val);
}

static const struct frb_ring_funcs si_dma_ring_funcs = {
	.align_mask = 0xf,
	.nop = DMA_PACKET(DMA_PACKET_NOP, 0, 0, 0, 0),
	.support_64bit_ptrs = 0,
	.get_rptr = si_dma_ring_get_rptr,
	.get_wptr = si_dma_ring_get_wptr,
	.set_wptr = si_dma_ring_set_wptr,
	.emit_ib = si_dma_ring_emit_ib,
	.emit_fence = si_dma_ring_emit_fence,
	.emit_hdp_flush = NULL,		/* si_flush_hdp: emit_wreg */
	.insert_nop = NULL,		/* amdgpu_ring_insert_nop */
	.pad_ib = si_dma_ring_pad_ib,
	.emit_wreg = si_dma_ring_emit_wreg,
	.emit_pipeline_sync = si_dma_ring_emit_pipeline_sync,
	.emit_wait_mem = si_dma_ring_emit_wait_mem,
};

static void si_dma_stop(struct frb_device *adev, struct frb_si_dma *dma)
{
	uint32_t rb_cntl;
	int i;

	for (i = 0; i < dma->num_instances; i++) {
		/* dma0 */
		rb_cntl = RREG32(mmDMA_GFX_RB_CNTL + sdma_offsets[i]);
		rb_cntl &= ~DMA_GFX_RB_CNTL__RB_ENABLE_MASK;
		WREG32(mmDMA_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);
		dma->ring[i].ready = 0;
	}
}

static int si_dma_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	unsigned i;
	uint32_t index;
	int r;
	uint32_t tmp = 0;
	uint64_t gpu_addr;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	frb_wb_write(adev, index, 0xCAFEDEAD);

	r = frb_ring_alloc(ring, 4);
	if (r)
		goto error_free_wb;

	frb_ring_write(ring, DMA_PACKET(DMA_PACKET_WRITE, 0, 0, 0, 1));
	frb_ring_write(ring, lower_32_bits(gpu_addr));
	frb_ring_write(ring, upper_32_bits(gpu_addr) & 0xff);
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = frb_wb_read(adev, index);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;
	if (r)
		frb_err(adev, "%s: ring test FAILED: writeback %08x (rptr %u wptr %u, DMA_STATUS_REG %08x)",
			ring->name, tmp, (uint32_t)si_dma_ring_get_rptr(ring), (uint32_t)si_dma_ring_get_wptr(ring),
			RREG32(mmDMA_STATUS_REG + sdma_offsets[ring->me]));
	else
		frb_info(adev, "%s: ring test passed after %u polls", ring->name, i);

error_free_wb:
	frb_wb_free(adev, index);
	return r;
}

static int si_dma_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t index, seq = 0;
	uint32_t tmp = 0;
	uint64_t gpu_addr;
	int r;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	frb_wb_write(adev, index, 0xCAFEDEAD);
	r = frb_ib_get(adev, 256, &ib);
	if (r)
		goto err0;

	frb_ib_write(adev, &ib, DMA_PACKET(DMA_PACKET_WRITE, 0, 0, 0, 1));
	frb_ib_write(adev, &ib, lower_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, upper_32_bits(gpu_addr) & 0xff);
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	ib.length_dw = 4;
	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto err1;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		frb_err(adev, "%s: IB test FAILED: fence %u not signalled (reads %u), writeback %08x", ring->name,
			seq, frb_fence_read(ring), frb_wb_read(adev, index));
		goto err0;	/* no frb_ib_free after a timeout */
	}
	tmp = frb_wb_read(adev, index);
	if (tmp == 0xDEADBEEF)
		r = 0;
	else
		r = -EINVAL;
	if (r)
		frb_err(adev, "%s: IB test FAILED: fence %u signalled, writeback %08x", ring->name, seq, tmp);
	else
		frb_info(adev, "%s: IB test passed (fence %u)", ring->name, seq);

err1:
	frb_ib_free(adev, &ib);
err0:
	frb_wb_free(adev, index);
	return r;
}

static int si_dma_start(struct frb_device *adev, struct frb_si_dma *dma)
{
	struct frb_ring *ring;
	uint32_t rb_cntl, dma_cntl, ib_cntl, rb_bufsz;
	int i, r;
	uint64_t rptr_addr;

	for (i = 0; i < dma->num_instances; i++) {
		ring = &dma->ring[i];

		WREG32(mmDMA_SEM_INCOMPLETE_TIMER_CNTL + sdma_offsets[i], 0);
		WREG32(mmDMA_SEM_WAIT_FAIL_TIMER_CNTL + sdma_offsets[i], 0);

		/* Set ring buffer size in dwords */
		rb_bufsz = order_base_2(ring->ring_size / 4);
		rb_cntl = rb_bufsz << 1;
		/* no RB_SWAP_ENABLE / RPTR_WRITEBACK_SWAP_ENABLE: the ring is little-endian */
		WREG32(mmDMA_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);

		/* Initialize the ring buffer's read and write pointers */
		WREG32(mmDMA_GFX_RB_RPTR + sdma_offsets[i], 0);
		WREG32(mmDMA_GFX_RB_WPTR + sdma_offsets[i], 0);

		rptr_addr = ring->rptr_gpu_addr;

		WREG32(mmDMA_GFX_RB_RPTR_ADDR_LO + sdma_offsets[i], lower_32_bits(rptr_addr));
		WREG32(mmDMA_GFX_RB_RPTR_ADDR_HI + sdma_offsets[i], upper_32_bits(rptr_addr) & 0xFF);

		rb_cntl |= DMA_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK;

		WREG32(mmDMA_GFX_RB_BASE + sdma_offsets[i], (uint32_t)(ring->gpu_addr >> 8));

		/* enable DMA IBs (no IB_SWAP_ENABLE: little-endian IBs) */
		ib_cntl = DMA_GFX_IB_CNTL__IB_ENABLE_MASK | DMA_GFX_IB_CNTL__CMD_VMID_FORCE_MASK;
		WREG32(mmDMA_GFX_IB_CNTL + sdma_offsets[i], ib_cntl);

		dma_cntl = RREG32(mmDMA_CNTL + sdma_offsets[i]);
		dma_cntl &= ~DMA_CNTL__CTXEMPTY_INT_ENABLE_MASK;
		WREG32(mmDMA_CNTL + sdma_offsets[i], dma_cntl);

		ring->wptr = 0;
		WREG32(mmDMA_GFX_RB_WPTR + sdma_offsets[i], (uint32_t)(ring->wptr << 2));
		WREG32(mmDMA_GFX_RB_CNTL + sdma_offsets[i], rb_cntl | DMA_GFX_RB_CNTL__RB_ENABLE_MASK);

		/* amdgpu_ring_test_helper */
		r = si_dma_ring_test_ring(ring);
		ring->ready = !r;
		if (r)
			return r;
	}

	return 0;
}

/* --- the buffer functions ------------------------------------------------------ */

static void si_dma_emit_copy_buffer(struct frb_device *adev, struct frb_ib *ib, uint64_t src_offset,
				    uint64_t dst_offset, uint32_t byte_count)
{
	frb_ib_write(adev, ib, DMA_PACKET(DMA_PACKET_COPY, 1, 0, 0, byte_count));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, lower_32_bits(src_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset) & 0xff);
	frb_ib_write(adev, ib, upper_32_bits(src_offset) & 0xff);
}

static void si_dma_emit_fill_buffer(struct frb_device *adev, struct frb_ib *ib, uint32_t src_data,
				    uint64_t dst_offset, uint32_t byte_count)
{
	frb_ib_write(adev, ib, DMA_PACKET(DMA_PACKET_CONSTANT_FILL, 0, 0, 0, byte_count / 4));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, src_data);
	frb_ib_write(adev, ib, upper_32_bits(dst_offset) << 16);
}

static struct frb_ring *dma_ring(struct frb_device *adev, int instance)
{
	struct frb_si_dma *dma = adev->si_dma;

	if (!dma || instance < 0 || instance >= dma->num_instances)
		return NULL;
	return &dma->ring[instance];
}

struct frb_ring *frb_si_dma_ring(struct frb_device *adev, int instance)
{
	struct frb_ring *ring = dma_ring(adev, instance);

	return ring && ring->ready ? ring : NULL;
}

void frb_si_dma_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	si_dma_ring_pad_ib(ring, ib);
}

static int dma_submit_and_wait(struct frb_ring *ring, struct frb_ib *ib, const char *what)
{
	struct frb_device *adev = ring->adev;
	uint32_t seq = 0;
	int r;

	ring->funcs->pad_ib(ring, ib);
	if (ib->length_dw > ib->size_dw)
		return -ENOMEM;
	r = frb_ib_schedule(ring, ib, &seq);
	if (r)
		return r;
	r = frb_fence_wait(ring, seq, FRB_FENCE_TIMEOUT_US);
	if (r) {
		frb_err(adev, "%s: %s: fence %u not signalled (fence 0x%08x, rptr 0x%llx)", ring->name,
			what, seq, frb_fence_read(ring), (unsigned long long)ring->funcs->get_rptr(ring));
		return r;
	}
	frb_ib_free(adev, ib);
	return 0;
}

/* amdgpu_copy_buffer on one DMA ring, polled; GPU addresses, split at copy_max_bytes */
int frb_si_dma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset, uint64_t dst_offset,
			   uint32_t byte_count)
{
	struct frb_ring *ring = dma_ring(adev, instance);
	uint32_t max_bytes = SI_DMA_COPY_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SI_DMA_COPY_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur = byte_count < max_bytes ? byte_count : max_bytes;

		si_dma_emit_copy_buffer(adev, &ib, src_offset, dst_offset, cur);
		src_offset += cur;
		dst_offset += cur;
		byte_count -= cur;
	}
	return dma_submit_and_wait(ring, &ib, "copy");
}

/* amdgpu_ttm_fill_mem on one DMA ring, polled: src_data repeated (whole dwords) over byte_count bytes */
int frb_si_dma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data, uint64_t dst_addr,
			   uint32_t byte_count)
{
	struct frb_ring *ring = dma_ring(adev, instance);
	uint32_t max_bytes = SI_DMA_FILL_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * SI_DMA_FILL_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur = byte_count < max_bytes ? byte_count : max_bytes;

		si_dma_emit_fill_buffer(adev, &ib, src_data, dst_addr, cur);
		dst_addr += cur;
		byte_count -= cur;
	}
	return dma_submit_and_wait(ring, &ib, "fill");
}

/* --- the copy and fill tests: VRAM to VRAM, checked by CPU readback ---------------- */

static uint32_t test_pattern(uint32_t seed, uint32_t i)
{
	uint32_t x = (i + 1) * 0x9e3779b1u ^ seed;

	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	return x;
}

static int dma_test_copy_fill(struct frb_device *adev, int instance, int fill)
{
	struct frb_si_dma *dma = adev->si_dma;
	struct frb_ring *ring = dma_ring(adev, instance);
	uint32_t total = FRB_SI_DMA_TEST_BYTES / 4, bytes = FRB_SI_DMA_TEST_BYTES - FRB_SI_DMA_TEST_GUARD;
	uint32_t src, dst, seed, value, i, got, want, bad = 0, first = 0, fgot = 0, fwant = 0;
	uint64_t t0;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	for (i = 0; i < 2; i++) {
		if (dma->test_bo[i])
			continue;
		r = frb_vram_obj_alloc(adev, FRB_SI_DMA_TEST_BYTES, 4096, &dma->test_bo[i]);
		if (r)
			return r;
	}
	src = dma->test_bo[0];
	dst = dma->test_bo[1];
	dma->test_runs++;
	seed = 0x5d3a0000u ^ ((uint32_t)instance << 12) ^ (dma->test_runs * 0x01000193u);
	/* four different bytes, new each run: a whole-dword fill keeps all four */
	value = 0xa1b2c300u | ((0xd4u + dma->test_runs * 0x1du) & 0xff);
	for (i = 0; i < total; i++) {
		if (!fill)
			frb_vram_write32(adev, src + 4 * i, test_pattern(seed, i));
		frb_vram_write32(adev, dst + 4 * i, FRB_SI_DMA_POISON);
	}
	t0 = adev->os->time_ms(adev->os->cookie);
	if (fill)
		r = frb_si_dma_fill_buffer(adev, instance, value, frb_vram_gpu_addr(adev, dst), bytes);
	else
		r = frb_si_dma_copy_buffer(adev, instance, frb_vram_gpu_addr(adev, src), frb_vram_gpu_addr(adev, dst),
					   bytes);
	if (r)
		return r;
	t0 = adev->os->time_ms(adev->os->cookie) - t0;
	frb_invalidate_hdp(adev);
	for (i = 0; i < total; i++) {
		got = frb_vram_read32(adev, dst + 4 * i);
		want = i >= bytes / 4 ? FRB_SI_DMA_POISON : fill ? value : test_pattern(seed, i);
		if (got != want && !bad++) {
			first = i;
			fgot = got;
			fwant = want;
		}
	}
	if (bad) {
		frb_err(adev, "%s: %s: %u of %u dwords wrong; first at +0x%x: 0x%08x, want 0x%08x%s", ring->name,
			fill ? "fill" : "copy", bad, total, first * 4, fgot, fwant,
			first >= bytes / 4 ? " (past the end: the guard)" : "");
		return -EIO;
	}
	frb_info(adev, "%s: %s test passed: %u bytes%s, %u-byte guard intact (%llu ms)", ring->name,
		 fill ? "fill" : "copy", bytes, fill ? " of a whole-dword value" : "", FRB_SI_DMA_TEST_GUARD,
		 (unsigned long long)t0);
	return 0;
}

/* --- the stage, the tests by hand, the teardown ----------------------------------- */

int frb_si_dma_init(struct frb_device *adev)
{
	struct frb_si_dma *dma = adev->si_dma;
	struct frb_ring *ring;
	char name[8];
	int r, i;

	if (adev->stage_done < FRB_STAGE_SMU || !adev->aper)
		return -EPERM;
	if (!dma) {
		dma = (struct frb_si_dma *)adev->os->alloc(adev->os->cookie, sizeof(*dma));
		if (!dma)
			return -ENOMEM;
		memset(dma, 0, sizeof(*dma));
		adev->si_dma = dma;
	}

	/* si_dma_early_init */
	dma->num_instances = SDMA_MAX_INSTANCE;

	/* si_dma_sw_init: the rings (no IRQ sources) */
	if (!dma->ring[0].adev) {
		for (i = 0; i < dma->num_instances; i++) {
			ring = &dma->ring[i];
			name[0] = 's'; name[1] = 'd'; name[2] = 'm'; name[3] = 'a';
			name[4] = (char)('0' + i); name[5] = 0;
			r = frb_ring_init(adev, ring, name, &si_dma_ring_funcs, 1024);
			if (r)
				return r;
			ring->me = i;
			frb_ring_clear(ring);
		}
	}

	/* si_dma_hw_init */
	dma->touched = 1;
	r = si_dma_start(adev, dma);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests */
	for (i = 0; i < dma->num_instances; i++) {
		ring = &dma->ring[i];
		r = si_dma_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		if (r) {
			frb_err(adev, "IB test failed on %s (%d).", ring->name, r);
			ring->ready = 0;
			return r;
		}
	}
	return 0;
}

int frb_si_dma_test(struct frb_device *adev, int instance, int test)
{
	struct frb_ring *ring = dma_ring(adev, instance);

	if (!ring || !ring->adev)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return si_dma_ring_test_ring(ring);
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return si_dma_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_COPY:
		return dma_test_copy_fill(adev, instance, 0);
	case FRB_TEST_FILL:
		return dma_test_copy_fill(adev, instance, 1);
	default:
		return -EINVAL;
	}
}

/* si_dma_hw_fini: both engines' rings off, before the driver lets go of VRAM */
void frb_si_dma_fini(struct frb_device *adev)
{
	struct frb_si_dma *dma = adev->si_dma;

	if (!dma)
		return;
	if (dma->touched && dma->num_instances) {
		si_dma_stop(adev, dma);
		frb_info(adev, "si_dma: both engines stopped (hw_fini)");
	}
	adev->os->free(adev->os->cookie, dma);
	adev->si_dma = NULL;
}

/* si_dma_set_clockgating_state (amdgpu's late init: GATE; hw_fini's ungate) */
void frb_si_dma_set_clockgating_state(struct frb_device *adev, int gate)
{
	uint32_t orig, data, offset;
	int i;

	if (gate && (adev->cg_flags & AMD_CG_SUPPORT_SDMA_MGCG)) {
		for (i = 0; i < SDMA_MAX_INSTANCE; i++) {
			offset = sdma_offsets[i];
			orig = data = RREG32(mmDMA_POWER_CNTL + offset);
			data &= ~DMA_POWER_CNTL__MEM_POWER_OVERRIDE_MASK;
			if (data != orig)
				WREG32(mmDMA_POWER_CNTL + offset, data);
			WREG32(mmDMA_CLK_CTRL + offset, 0x00000100);
		}
	} else {
		for (i = 0; i < SDMA_MAX_INSTANCE; i++) {
			offset = sdma_offsets[i];
			orig = data = RREG32(mmDMA_POWER_CNTL + offset);
			data |= DMA_POWER_CNTL__MEM_POWER_OVERRIDE_MASK;
			if (data != orig)
				WREG32(mmDMA_POWER_CNTL + offset, data);

			orig = data = RREG32(mmDMA_CLK_CTRL + offset);
			data = 0xff000000;
			if (data != orig)
				WREG32(mmDMA_CLK_CTRL + offset, data);
		}
	}
}
