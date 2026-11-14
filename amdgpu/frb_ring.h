/*
 * The command-submission core the engines share: the subset of
 * amdgpu_ring.c, amdgpu_ib.c, amdgpu_fence.c and amdgpu_device.c's
 * writeback (amdgpu_device_wb_*) that a ring test, an IB test and polled
 * fences need. GFX8 (frb_gfx_v8_0.c) and SDMA v3 (frb_sdma_v3_0.c) supply
 * the per-engine functions, as amdgpu_ring_funcs.
 *
 * PLACEMENT. amdgpu keeps rings, writeback and the IB pool in GTT (system
 * memory through the GART). Until bus mastering is on, Firebrand keeps them
 * in VRAM, in an objects region below the driver's other VRAM (frb_gmc_v8_0.c
 * frb_gmc_layout), which the CPU reaches through BAR0: the engines read and
 * write VRAM through the MC like any other memory. Writes the CPU makes
 * there reach the GPU after an HDP flush (vi_flush_hdp, before every WPTR
 * update); GPU writes are read back after an HDP invalidate.
 *
 * BYTE ORDER. Ring, IB and writeback contents are little-endian dwords
 * (frb_vram_write32); no BUF_SWAP is asked of the engines (docs/byte-order.md).
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_RING_H
#define FRB_RING_H

#include "frb_device.h"

#define FRB_WB_SLOTS		64		/* 256-bit slots, amdgpu's AMDGPU_MAX_WB is 256 */
#define FRB_IB_POOL_SIZE	(256 * 1024)	/* AMDGPU_IB_POOL_SIZE */
#define FRB_FENCE_TIMEOUT_US	(1000 * 1000)

struct frb_ring;
struct frb_ib;

/* amdgpu_ring_funcs, the parts the tests use */
struct frb_ring_funcs {
	uint32_t align_mask;
	uint32_t nop;
	int support_64bit_ptrs;
	uint64_t (*get_rptr)(struct frb_ring *ring);
	uint64_t (*get_wptr)(struct frb_ring *ring);
	void (*set_wptr)(struct frb_ring *ring);
	void (*emit_ib)(struct frb_ring *ring, const struct frb_ib *ib, uint32_t vmid, uint32_t flags);
	void (*emit_fence)(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags);
	void (*emit_hdp_flush)(struct frb_ring *ring);
	void (*insert_nop)(struct frb_ring *ring, uint32_t count);	/* 0: write funcs->nop */
	void (*pad_ib)(struct frb_ring *ring, struct frb_ib *ib);
	void (*emit_wreg)(struct frb_ring *ring, uint32_t reg, uint32_t val);	/* vi_flush/invalidate_hdp */
	/*
	 * amdgpu's emit_pipeline_sync: wait until the ring's own last fence
	 * (sync_seq) has landed, so what follows sees everything before it in
	 * memory. NULL where not ported.
	 */
	void (*emit_pipeline_sync)(struct frb_ring *ring);
	/*
	 * Wait until the dword at addr compares with ref by func (the engines'
	 * WAIT_REG_MEM / POLL_REGMEM encoding: FRB_WAIT_*); another engine's
	 * fence, the way amdgpu waits on a semaphore. NULL where not ported.
	 */
	void (*emit_wait_mem)(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func);
};

/* WAIT_REG_MEM (GFX8) and POLL_REGMEM (SDMA v3) share the compare functions */
#define FRB_WAIT_EQUAL		3
#define FRB_WAIT_GEQUAL		5

/* amdgpu_fence.c AMDGPU_FENCE_FLAG_* */
#define FRB_FENCE_FLAG_64BIT	(1 << 0)
#define FRB_FENCE_FLAG_INT	(1 << 1)

struct frb_ring {
	struct frb_device *adev;
	const struct frb_ring_funcs *funcs;
	char name[16];
	int me, pipe, queue;
	uint32_t ring_size;		/* bytes, a power of two */
	uint32_t buf_mask;		/* dwords - 1 */
	uint64_t ptr_mask;
	uint32_t bar_offset;		/* the buffer, as a BAR0 (VRAM) offset */
	uint64_t gpu_addr;
	uint64_t wptr, wptr_old;
	uint32_t count_dw;
	/* writeback dword indices and their GPU addresses */
	uint32_t rptr_offs, wptr_offs, fence_offs;
	uint64_t rptr_gpu_addr, wptr_gpu_addr, fence_gpu_addr;
	uint32_t sync_seq;		/* the last fence sequence emitted */
	int ready;
};

struct frb_ib {
	void *cpu;			/* host memory (a GTT object), else NULL: VRAM at bar_offset */
	uint32_t bar_offset;
	uint64_t gpu_addr;
	uint32_t length_dw;		/* dwords written */
	uint32_t size_dw;
	uint32_t flags;
};

/* VRAM objects (rings, writeback, IB pool, CSB): a bump allocator over layout.objects */
int frb_vram_obj_alloc(struct frb_device *adev, uint32_t size, uint32_t align, uint32_t *bar_offset);
static inline uint64_t frb_vram_gpu_addr(struct frb_device *adev, uint32_t bar_offset)
{
	return adev->gmc.vram_start + bar_offset;
}

/* amdgpu_device_wb_init / _get: 256-bit slots; returns a dword index */
int frb_wb_init(struct frb_device *adev);
int frb_wb_get(struct frb_device *adev, uint32_t *wb);
void frb_wb_free(struct frb_device *adev, uint32_t wb);		/* amdgpu_device_wb_free */
uint32_t frb_wb_read(struct frb_device *adev, uint32_t wb);	/* HDP invalidated first */
void frb_wb_write(struct frb_device *adev, uint32_t wb, uint32_t v);
uint64_t frb_wb_gpu_addr(struct frb_device *adev, uint32_t wb);

/* vi.c vi_flush_hdp / vi_invalidate_hdp */
void frb_flush_hdp(struct frb_device *adev);
void frb_invalidate_hdp(struct frb_device *adev);

/* amdgpu_ring.c */
int frb_ring_init(struct frb_device *adev, struct frb_ring *ring, const char *name,
		  const struct frb_ring_funcs *funcs, uint32_t max_dw);
int frb_ring_alloc(struct frb_ring *ring, uint32_t ndw);
void frb_ring_write(struct frb_ring *ring, uint32_t v);
void frb_ring_commit(struct frb_ring *ring);
void frb_ring_undo(struct frb_ring *ring);
void frb_ring_clear(struct frb_ring *ring);
void frb_ring_generic_pad_ib(struct frb_ring *ring, struct frb_ib *ib);

/* amdgpu_ib.c: IBs out of one VRAM pool, reset by frb_ib_pool_reset */
int frb_ib_pool_init(struct frb_device *adev);
void frb_ib_pool_reset(struct frb_device *adev);
int frb_ib_get(struct frb_device *adev, uint32_t size_bytes, struct frb_ib *ib);
void frb_ib_write(struct frb_device *adev, struct frb_ib *ib, uint32_t v);
/* amdgpu_ib_free, once the IB's fence has signalled: the newest IB's space comes back at once */
void frb_ib_free(struct frb_device *adev, struct frb_ib *ib);
/* amdgpu_ib_schedule, one IB, then a fence; *seq gets its sequence number */
int frb_ib_schedule(struct frb_ring *ring, struct frb_ib *ib, uint32_t *seq);
/*
 * What an IB must wait for on the GPU before it runs: the ring's previous
 * work (amdgpu's need_pipe_sync, for an IB that reads what the one before
 * it wrote), and another engine's fence at wait_addr reaching wait_seq
 * (>=, unsigned: the caller keeps clear of the wrap). Rings without the
 * functions refuse (-EINVAL).
 */
struct frb_ib_deps {
	int pipeline_sync;
	uint64_t wait_addr;		/* 0: none */
	uint32_t wait_seq;
	/*
	 * No HDP flush packet in the frame: the IB reads nothing the CPU wrote
	 * through BAR0 after frb_ring_commit's own (MMIO) flush, which runs
	 * before every WPTR write anyway.
	 */
	int no_hdp_flush;
};
int frb_ib_schedule_deps(struct frb_ring *ring, struct frb_ib *ib, const struct frb_ib_deps *deps,
			 uint32_t *seq);
/*
 * A frame with no IB: wait for another engine's fence at wait_addr to reach
 * wait_seq (>=), then this ring's fence. Its sequence covers that engine's
 * work up to wait_seq, so one timeline (this ring's) can stand for both.
 */
int frb_ring_fence_after(struct frb_ring *ring, uint64_t wait_addr, uint32_t wait_seq, uint32_t *seq);

/*
 * amdgpu_fence_driver_force_completion: the ring's fence word set to the
 * last sequence emitted, after a reset threw away what was on the ring.
 */
void frb_fence_driver_force_completion(struct frb_ring *ring);

/* amdgpu_fence.c, polled: emit the next sequence, wait for one */
int frb_fence_emit(struct frb_ring *ring, uint32_t *seq, unsigned flags);
uint32_t frb_fence_read(struct frb_ring *ring);
int frb_fence_wait(struct frb_ring *ring, uint32_t seq, uint32_t timeout_us);

#endif
