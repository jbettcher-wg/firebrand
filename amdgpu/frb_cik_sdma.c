/*
 * cik_sdma.c (Linux 7.0.2), for Sea Islands (the FirePro W8100, Hawaii):
 * the two SDMA engines' gfx rings. Stage sdma runs what amdgpu does for this
 * block at load: early_init (num_instances, cik_sdma_init_microcode:
 * hawaii_sdma.bin and hawaii_sdma1.bin, the headered images; burst NOPs from
 * feature version 20), sw_init (the two rings), hw_init (cik_sdma_start:
 * the microcode by MMIO, halt, context switch, gfx_resume, unhalt, the ring
 * tests), then the IB tests amdgpu runs from late_init
 * (amdgpu_ib_ring_tests). The clock gating amdgpu's late init sets
 * (cik_sdma_set_clockgating_state: MGCG and light sleep) is frb_cik.c's
 * stage sdma's to call. hw_fini halts both engines at unload.
 *
 * Also here: amdgpu_ttm's copy and fill for this engine (amdgpu_copy_buffer,
 * amdgpu_ttm_fill_mem on cik_sdma_emit_copy_buffer / _emit_fill_buffer),
 * polled; GPU (MC) addresses, so VRAM<->GTT works once the GART carries
 * system pages. frbctl `test sdma0|sdma1 ring|ib|copy|fill|gtt`.
 *
 * NOT PORTED: interrupts (the trap and illegal-instruction sources 224, 241,
 * 247; SDMA0_CNTL.TRAP_ENABLE stays off and fences are polled), the RLC
 * compute queues (empty in amdgpu too), the VM PTE functions (one GART, no
 * per-client page tables), the soft reset (frb_reset.c refuses on CIK), the
 * phase quantum's module parameter (its default, 32, is taken).
 *
 * Where Firebrand departs from amdgpu, and why:
 * - PLACEMENT. Rings, writeback and IBs in VRAM (frb_ring.c's objects
 *   region), as on every other family here.
 * - BYTE ORDER: amdgpu's little-endian arrangement. RB_SWAP_ENABLE,
 *   RPTR_WRITEBACK_SWAP_ENABLE and IB_SWAP_ENABLE stay 0 where amdgpu's
 *   __BIG_ENDIAN arm sets them: rings, IBs and the writeback are
 *   little-endian dwords (frb_ring_write, frb_ib_write, frb_wb_read), as on
 *   SDMA 3, SDMA 4 and SI's DMA. RadeonNI never ran SDMA on this card (its
 *   2D path was CP DMA on the gfx ring).
 * - SDMA0_TILING_CONFIG takes gfx_v7_0's gb_addr_config & 0x70 through
 *   frb_gfx_v7_0_get_info (stage gfx ran first), else frb_cik.c's copy.
 * - The fill: amdgpu's FILLSIZE 0, which SDMA 3 runs as a byte fill (the
 *   data's low byte repeated, measured on the RX 550); CIK's is not
 *   measured, so the fill test passes on a byte fill or a whole-dword fill
 *   and says which it was. The GTT test fills with 0x5a5a5a5a, the same
 *   under either.
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
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
 *
 * Authors: Alex Deucher
 */
#include <stddef.h>
#include "frb_ring.h"
#include "frb_ucode.h"
#include "include/cikd.h"
#include "include/asic_reg/bif/bif_4_1_d.h"
#include "include/asic_reg/bif/bif_4_1_sh_mask.h"
#include "include/asic_reg/gca/gfx_7_2_d.h"
#include "include/asic_reg/gca/gfx_7_2_enum.h"
#include "include/asic_reg/gca/gfx_7_2_sh_mask.h"
#include "include/asic_reg/gmc/gmc_7_1_d.h"
#include "include/asic_reg/gmc/gmc_7_1_sh_mask.h"
#include "include/asic_reg/oss/oss_2_0_d.h"
#include "include/asic_reg/oss/oss_2_0_sh_mask.h"

/* amdgpu_drv.c module parameter default */
static const uint32_t amdgpu_sdma_phase_quantum = 32;

/* cik_sdma_buffer_funcs */
#define CIK_SDMA_COPY_MAX_BYTES	0x1fffff
#define CIK_SDMA_COPY_NUM_DW	7
#define CIK_SDMA_FILL_MAX_BYTES	0x1fffff
#define CIK_SDMA_FILL_NUM_DW	5

/* the copy/fill tests: two buffers in the VRAM objects region */
#define FRB_CIK_SDMA_TEST_BYTES	(256u << 10)
#define FRB_CIK_SDMA_TEST_GUARD	256u	/* bytes left untouched at the end of the target */
#define FRB_CIK_SDMA_POISON	0xdeadd00du

/* amdgpu_sdma.h amdgpu_sdma_instance, the fields this port uses */
struct frb_cik_sdma_instance {
	struct frb_ring ring;
	uint32_t fw_version;
	uint32_t feature_version;
	int burst_nop;
	struct frb_ucode u;		/* the image, loaded at every cik_sdma_start */
};

/* amdgpu_sdma.h amdgpu_sdma */
struct frb_cik_sdma {
	struct frb_cik_sdma_instance instance[SDMA_MAX_INSTANCE];
	int num_instances;
	int sw_done;
	int touched;			/* hw_init wrote the engines: hw_fini must halt them */
	uint32_t test_bo[2];		/* BAR0 offsets of the copy/fill buffers, 0 = none yet */
	uint32_t test_runs;
};

static const uint32_t sdma_offsets[SDMA_MAX_INSTANCE] =
{
	SDMA0_REGISTER_OFFSET,
	SDMA1_REGISTER_OFFSET
};

static inline uint32_t lower_32_bits(uint64_t v) { return (uint32_t)v; }
static inline uint32_t upper_32_bits(uint64_t v) { return (uint32_t)(v >> 32); }

/* include/linux/log2.h order_base_2: log2, rounded up */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

/*
 * cik_sdma_init_microcode: amdgpu's hawaii_sdma.bin / hawaii_sdma1.bin
 * (linux-firmware links to bonaire_sdma*.bin; the same bytes as radeon's)
 */
static int cik_sdma_init_microcode(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	const char *chip_name;
	char name[32];
	struct frb_fw fw;
	int err = 0, i;

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		chip_name = "bonaire";
		break;
	case CHIP_HAWAII:
		chip_name = "hawaii";
		break;
	default:
		frb_err(adev, "cik_sdma: no microcode choice for asic 0x%x here", adev->asic_type);
		return -ENODEV;
	}

	for (i = 0; i < sdma->num_instances; i++) {
		frb_fw_name(name, sizeof(name), chip_name, i == 0 ? "_sdma.bin" : "_sdma1.bin");
		fw.data = 0;
		fw.size = 0;
		err = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
		if (err) {
			frb_err(adev, "cik_sdma: Failed to load firmware \"%s\" (%d)", name, err);
			return err;
		}
		err = frb_ucode_parse(adev, name, &fw, &sdma->instance[i].u);
		if (err)
			return err;
		if (sdma->instance[i].u.header_size_bytes < FRB_FWH_SDMA_UCODE_FEATURE_VERSION + 4) {
			frb_err(adev, "%s: no sdma_firmware_header_v1_0", name);
			return -EINVAL;
		}
	}
	return 0;
}

static struct frb_cik_sdma_instance *amdgpu_sdma_get_instance_from_ring(struct frb_ring *ring)
{
	struct frb_cik_sdma *sdma = ring->adev->cik_sdma;

	if (!sdma || ring->me < 0 || ring->me >= sdma->num_instances)
		return NULL;
	return &sdma->instance[ring->me];
}

/* cik_sdma_ring_get_rptr - the current read pointer, from writeback (a byte offset there) */
static uint64_t cik_sdma_ring_get_rptr(struct frb_ring *ring)
{
	uint32_t rptr;

	rptr = frb_wb_read(ring->adev, ring->rptr_offs);	/* *ring->rptr_cpu_addr */

	return (rptr & 0x3fffc) >> 2;
}

/* cik_sdma_ring_get_wptr - the current write pointer, from the register */
static uint64_t cik_sdma_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return (RREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[ring->me]) & 0x3fffc) >> 2;
}

/* cik_sdma_ring_set_wptr - commit the write pointer */
static void cik_sdma_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[ring->me],
	       (uint32_t)(ring->wptr << 2) & 0x3fffc);
}

static void cik_sdma_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	struct frb_cik_sdma_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
	uint32_t i;

	for (i = 0; i < count; i++)
		if (sdma && sdma->burst_nop && (i == 0))
			frb_ring_write(ring, ring->funcs->nop |
				       SDMA_NOP_COUNT(count - 1));
		else
			frb_ring_write(ring, ring->funcs->nop);
}

/* cik_sdma_ring_emit_ib - Schedule an IB in the DMA ring (vmid: 0 for kernel IBs) */
static void cik_sdma_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib,
				  uint32_t vmid, uint32_t flags)
{
	uint32_t extra_bits = vmid & 0xf;

	(void)flags;

	/* IB packet must end on a 8 DW boundary */
	cik_sdma_ring_insert_nop(ring, (4 - lower_32_bits(ring->wptr)) & 7);

	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_INDIRECT_BUFFER, 0, extra_bits));
	frb_ring_write(ring, lower_32_bits(ib->gpu_addr) & 0xffffffe0); /* base must be 32 byte aligned */
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr) & 0xffffffff);
	frb_ring_write(ring, ib->length_dw);
}

/* cik_sdma_ring_emit_hdp_flush - emit an hdp flush on the DMA ring */
static void cik_sdma_ring_emit_hdp_flush(struct frb_ring *ring)
{
	uint32_t extra_bits = (SDMA_POLL_REG_MEM_EXTRA_OP(1) |
			       SDMA_POLL_REG_MEM_EXTRA_FUNC(3)); /* == */
	uint32_t ref_and_mask;

	if (ring->me == 0)
		ref_and_mask = GPU_HDP_FLUSH_DONE__SDMA0_MASK;
	else
		ref_and_mask = GPU_HDP_FLUSH_DONE__SDMA1_MASK;

	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_POLL_REG_MEM, 0, extra_bits));
	frb_ring_write(ring, mmGPU_HDP_FLUSH_DONE << 2);
	frb_ring_write(ring, mmGPU_HDP_FLUSH_REQ << 2);
	frb_ring_write(ring, ref_and_mask); /* reference */
	frb_ring_write(ring, ref_and_mask); /* mask */
	frb_ring_write(ring, (0xfff << 16) | 10); /* retry count, poll interval */
}

/*
 * cik_sdma_ring_emit_fence - emit a fence on the DMA ring (the trap after it
 * raises nothing: TRAP_ENABLE is off, fences are polled)
 */
static void cik_sdma_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq,
				     unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;

	/* write the fence */
	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_FENCE, 0, 0));
	frb_ring_write(ring, lower_32_bits(addr));
	frb_ring_write(ring, upper_32_bits(addr));
	frb_ring_write(ring, lower_32_bits(seq));

	/* optionally write high bits as well */
	if (write64bit) {
		addr += 4;
		frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_FENCE, 0, 0));
		frb_ring_write(ring, lower_32_bits(addr));
		frb_ring_write(ring, upper_32_bits(addr));
		frb_ring_write(ring, upper_32_bits(seq));
	}

	/* generate an interrupt */
	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_TRAP, 0, 0));
}

/* cik_sdma_gfx_stop - stop the gfx async dma engines */
static void cik_sdma_gfx_stop(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	uint32_t rb_cntl;
	int i;

	for (i = 0; i < sdma->num_instances; i++) {
		rb_cntl = RREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i]);
		rb_cntl &= ~SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);
		WREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i], 0);
		sdma->instance[i].ring.ready = 0;
	}
}

/* cik_sdma_rlc_stop - stop the compute async dma engines (empty in amdgpu too) */
static void cik_sdma_rlc_stop(struct frb_device *adev)
{
	/* XXX todo */
	(void)adev;
}

/* cik_ctx_switch_enable - the async dma engines' context switch on (1) or off (0) */
static void cik_ctx_switch_enable(struct frb_device *adev, struct frb_cik_sdma *sdma, int enable)
{
	uint32_t f32_cntl, phase_quantum = 0;
	int i;

	if (amdgpu_sdma_phase_quantum) {
		unsigned value = amdgpu_sdma_phase_quantum;
		unsigned unit = 0;

		while (value > (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				SDMA0_PHASE0_QUANTUM__VALUE__SHIFT)) {
			value = (value + 1) >> 1;
			unit++;
		}
		if (unit > (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
			    SDMA0_PHASE0_QUANTUM__UNIT__SHIFT)) {
			value = (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				 SDMA0_PHASE0_QUANTUM__VALUE__SHIFT);
			unit = (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
				SDMA0_PHASE0_QUANTUM__UNIT__SHIFT);
			frb_info(adev, "clamping sdma_phase_quantum to %uK clock cycles", value << unit);
		}
		phase_quantum =
			value << SDMA0_PHASE0_QUANTUM__VALUE__SHIFT |
			unit  << SDMA0_PHASE0_QUANTUM__UNIT__SHIFT;
	}

	for (i = 0; i < sdma->num_instances; i++) {
		f32_cntl = RREG32(mmSDMA0_CNTL + sdma_offsets[i]);
		if (enable) {
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					AUTO_CTXSW_ENABLE, 1);
			if (amdgpu_sdma_phase_quantum) {
				WREG32(mmSDMA0_PHASE0_QUANTUM + sdma_offsets[i],
				       phase_quantum);
				WREG32(mmSDMA0_PHASE1_QUANTUM + sdma_offsets[i],
				       phase_quantum);
			}
		} else {
			f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					AUTO_CTXSW_ENABLE, 0);
		}

		WREG32(mmSDMA0_CNTL + sdma_offsets[i], f32_cntl);
	}
}

/* cik_sdma_enable - halt (0) or unhalt (1) the async dma engines */
static void cik_sdma_enable(struct frb_device *adev, struct frb_cik_sdma *sdma, int enable)
{
	uint32_t me_cntl;
	int i;

	if (!enable) {
		cik_sdma_gfx_stop(adev, sdma);
		cik_sdma_rlc_stop(adev);
	}

	for (i = 0; i < sdma->num_instances; i++) {
		me_cntl = RREG32(mmSDMA0_F32_CNTL + sdma_offsets[i]);
		if (enable)
			me_cntl &= ~SDMA0_F32_CNTL__HALT_MASK;
		else
			me_cntl |= SDMA0_F32_CNTL__HALT_MASK;
		WREG32(mmSDMA0_F32_CNTL + sdma_offsets[i], me_cntl);
	}
}

/*
 * adev->gfx.config.gb_addr_config, which gfx_v7_0_gpu_early_init set (stage
 * gfx comes first), else frb_cik.c's computation, else Hawaii's golden value
 */
static uint32_t cik_sdma_gb_addr_config(struct frb_device *adev)
{
	struct frb_gfx_info gfx;

	if (!frb_gfx_v7_0_get_info(adev, &gfx) && gfx.gb_addr_config)
		return gfx.gb_addr_config;
	if (adev->cik_gb_addr_config)
		return adev->cik_gb_addr_config;
	return 0x12011003;	/* HAWAII_GB_ADDR_CONFIG_GOLDEN */
}

/*
 * cik_sdma_ring_test_ring - simple async dma engine test: the engine writes
 * a value to a writeback dword (VRAM here, read back after an HDP
 * invalidate by frb_wb_read)
 */
static int cik_sdma_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	unsigned i;
	uint32_t index;
	int r;
	uint32_t tmp;
	uint64_t gpu_addr;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	tmp = 0xCAFEDEAD;
	frb_wb_write(adev, index, tmp);

	r = frb_ring_alloc(ring, 5);
	if (r)
		goto error_free_wb;

	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_WRITE, SDMA_WRITE_SUB_OPCODE_LINEAR, 0));
	frb_ring_write(ring, lower_32_bits(gpu_addr));
	frb_ring_write(ring, upper_32_bits(gpu_addr));
	frb_ring_write(ring, 1); /* number of DWs to follow */
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = frb_wb_read(adev, index);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		frb_err(adev, "%s: ring test: the value is still 0x%08x after %u polls (rptr 0x%llx, wptr 0x%llx, "
			"SDMA0_STATUS_REG %08x, SDMA0_F32_CNTL %08x)", ring->name, tmp, i,
			(unsigned long long)ring->funcs->get_rptr(ring),
			(unsigned long long)ring->funcs->get_wptr(ring),
			RREG32(mmSDMA0_STATUS_REG + sdma_offsets[ring->me]),
			RREG32(mmSDMA0_F32_CNTL + sdma_offsets[ring->me]));
		r = -ETIMEDOUT;
	} else {
		frb_info(adev, "%s: ring test passed after %u polls (rptr 0x%llx)", ring->name, i,
			 (unsigned long long)ring->funcs->get_rptr(ring));
	}

error_free_wb:
	frb_wb_free(adev, index);
	return r;
}

/* cik_sdma_ring_test_ib - test an IB on the DMA engine (the fence polled; the IB is 5 dwords, as amdgpu's) */
static int cik_sdma_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
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
	tmp = 0xCAFEDEAD;
	frb_wb_write(adev, index, tmp);
	memset(&ib, 0, sizeof(ib));
	r = frb_ib_get(adev, 256, &ib);
	if (r)
		goto err0;

	frb_ib_write(adev, &ib, SDMA_PACKET(SDMA_OPCODE_WRITE,
					    SDMA_WRITE_SUB_OPCODE_LINEAR, 0));
	frb_ib_write(adev, &ib, lower_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, upper_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, 1);
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	ib.length_dw = 5;
	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto err1;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		/* the IB stays allocated: the engine may still fetch it */
		frb_err(adev, "%s: IB test: fence %u not signalled (fence 0x%08x, rptr 0x%llx, SDMA0_STATUS_REG %08x)",
			ring->name, seq, frb_fence_read(ring), (unsigned long long)ring->funcs->get_rptr(ring),
			RREG32(mmSDMA0_STATUS_REG + sdma_offsets[ring->me]));
		goto err0;
	}
	tmp = frb_wb_read(adev, index);
	if (tmp == 0xDEADBEEF) {
		r = 0;
		frb_info(adev, "%s: IB test passed (fence %u)", ring->name, seq);
	} else {
		frb_err(adev, "%s: IB test: fence %u signalled but the value is 0x%08x", ring->name,
			seq, tmp);
		r = -EINVAL;
	}

err1:
	frb_ib_free(adev, &ib);
err0:
	frb_wb_free(adev, index);
	return r;
}

/* amdgpu_ring_test_helper */
static int amdgpu_ring_test_helper(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	int r;

	r = cik_sdma_ring_test_ring(ring);
	if (r)
		frb_err(adev, "ring %s test failed (%d)", ring->name, r);
	else
		frb_dbg(adev, "ring test on %s succeeded", ring->name);

	ring->ready = !r;

	return r;
}

/* cik_sdma_gfx_resume - setup and start the async dma engines */
static int cik_sdma_gfx_resume(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	struct frb_ring *ring;
	uint32_t rb_cntl, ib_cntl;
	uint32_t rb_bufsz;
	int i, j, r;

	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;

		for (j = 0; j < 16; j++) {
			frb_cik_srbm_select(adev, 0, 0, 0, j);
			/* SDMA GFX */
			WREG32(mmSDMA0_GFX_VIRTUAL_ADDR + sdma_offsets[i], 0);
			WREG32(mmSDMA0_GFX_APE1_CNTL + sdma_offsets[i], 0);
			/* XXX SDMA RLC - todo */
		}
		frb_cik_srbm_select(adev, 0, 0, 0, 0);

		WREG32(mmSDMA0_TILING_CONFIG + sdma_offsets[i],
		       cik_sdma_gb_addr_config(adev) & 0x70);

		WREG32(mmSDMA0_SEM_INCOMPLETE_TIMER_CNTL + sdma_offsets[i], 0);
		WREG32(mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL + sdma_offsets[i], 0);

		/* Set ring buffer size in dwords */
		rb_bufsz = order_base_2(ring->ring_size / 4);
		rb_cntl = rb_bufsz << 1;
		/*
		 * amdgpu sets RB_SWAP_ENABLE and RPTR_WRITEBACK_SWAP_ENABLE here
		 * under __BIG_ENDIAN; the ring and the writeback are little-endian.
		 */
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i], rb_cntl);

		/* Initialize the ring buffer's read and write pointers */
		WREG32(mmSDMA0_GFX_RB_RPTR + sdma_offsets[i], 0);
		WREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[i], 0);
		WREG32(mmSDMA0_GFX_IB_RPTR + sdma_offsets[i], 0);
		WREG32(mmSDMA0_GFX_IB_OFFSET + sdma_offsets[i], 0);

		/* set the wb address whether it's enabled or not */
		WREG32(mmSDMA0_GFX_RB_RPTR_ADDR_HI + sdma_offsets[i],
		       upper_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFF);
		WREG32(mmSDMA0_GFX_RB_RPTR_ADDR_LO + sdma_offsets[i],
		       (lower_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFC));

		rb_cntl |= SDMA0_GFX_RB_CNTL__RPTR_WRITEBACK_ENABLE_MASK;

		WREG32(mmSDMA0_GFX_RB_BASE + sdma_offsets[i], (uint32_t)(ring->gpu_addr >> 8));
		WREG32(mmSDMA0_GFX_RB_BASE_HI + sdma_offsets[i], (uint32_t)(ring->gpu_addr >> 40));

		ring->wptr = 0;
		WREG32(mmSDMA0_GFX_RB_WPTR + sdma_offsets[i], (uint32_t)(ring->wptr << 2));

		/* enable DMA RB */
		WREG32(mmSDMA0_GFX_RB_CNTL + sdma_offsets[i],
		       rb_cntl | SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK);

		ib_cntl = SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
		/* IB_SWAP_ENABLE (amdgpu, __BIG_ENDIAN) stays off: IBs are little-endian */
		/* enable DMA IBs */
		WREG32(mmSDMA0_GFX_IB_CNTL + sdma_offsets[i], ib_cntl);
	}

	cik_sdma_enable(adev, sdma, 1);

	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		r = amdgpu_ring_test_helper(ring);
		if (r)
			return r;
	}

	return 0;
}

/* cik_sdma_rlc_resume - the compute queues (empty in amdgpu too) */
static int cik_sdma_rlc_resume(struct frb_device *adev)
{
	(void)adev;
	return 0;
}

/*
 * cik_sdma_load_microcode - the SDMA0/1 images by MMIO, each word read
 * little-endian from the file (le32_to_cpup); then the version in the
 * address register, as amdgpu leaves it
 */
static int cik_sdma_load_microcode(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	const struct frb_ucode *u;
	uint32_t fw_size;
	int i, j;

	/* halt the MEs */
	cik_sdma_enable(adev, sdma, 0);

	for (i = 0; i < sdma->num_instances; i++) {
		u = &sdma->instance[i].u;
		if (!u->data)
			return -EINVAL;
		fw_size = u->ucode_size_bytes / 4;
		sdma->instance[i].fw_version = u->ucode_version;
		sdma->instance[i].feature_version = frb_ucode_le32(u, FRB_FWH_SDMA_UCODE_FEATURE_VERSION);
		if (sdma->instance[i].feature_version >= 20)
			sdma->instance[i].burst_nop = 1;
		WREG32(mmSDMA0_UCODE_ADDR + sdma_offsets[i], 0);
		for (j = 0; j < fw_size; j++)
			WREG32(mmSDMA0_UCODE_DATA + sdma_offsets[i], frb_ucode_le32(u, u->ucode_array_offset + 4 * j));
		WREG32(mmSDMA0_UCODE_ADDR + sdma_offsets[i], sdma->instance[i].fw_version);
	}

	return 0;
}

/*
 * cik_sdma_soft_reset (amdgpu: the soft_reset hook) at every hw_init on CIK,
 * both engines, whatever SDMA0_STATUS_REG says (amdgpu resets only an engine
 * whose IDLE is clear). On the W8100 a reload in the same power cycle (the
 * earlier load's hw_fini halted both engines; DPM off, so no ASIC reset on
 * init) left one engine, SDMA0 or SDMA1, with its first ring fetch never
 * returning (STATUS 46cc7042: RB_MC_RREQ_IDLE clear, RPTR 0) although both
 * read idle before the start.
 */
static void cik_sdma_soft_reset(struct frb_device *adev)
{
	uint32_t srbm_soft_reset, s0, s1, tmp;

	s0 = RREG32(mmSDMA0_STATUS_REG + sdma_offsets[0]);
	s1 = RREG32(mmSDMA0_STATUS_REG + sdma_offsets[1]);
	srbm_soft_reset = SRBM_SOFT_RESET__SOFT_RESET_SDMA_MASK | SRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK;
	tmp = RREG32(mmSRBM_SOFT_RESET);
	tmp |= srbm_soft_reset;
	frb_info(adev, "cik_sdma: SDMA0/1_STATUS_REG %08x %08x at hw_init: SRBM_SOFT_RESET=0x%08X",
		 s0, s1, tmp);
	WREG32(mmSRBM_SOFT_RESET, tmp);
	tmp = RREG32(mmSRBM_SOFT_RESET);

	udelay(50);

	tmp &= ~srbm_soft_reset;
	WREG32(mmSRBM_SOFT_RESET, tmp);
	tmp = RREG32(mmSRBM_SOFT_RESET);

	/* Wait a little for things to settle down */
	udelay(50);
	frb_info(adev, "cik_sdma: after the soft reset SDMA0/1_STATUS_REG %08x %08x",
		 RREG32(mmSDMA0_STATUS_REG + sdma_offsets[0]), RREG32(mmSDMA0_STATUS_REG + sdma_offsets[1]));
}

/* cik_sdma_start - setup and start the async dma engines */
static int cik_sdma_start(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	int r;

	r = cik_sdma_load_microcode(adev, sdma);
	if (r)
		return r;

	/* halt the engine before programing */
	cik_sdma_enable(adev, sdma, 0);
	/* enable sdma ring preemption */
	cik_ctx_switch_enable(adev, sdma, 1);

	/* start the gfx rings and rlc compute queues */
	r = cik_sdma_gfx_resume(adev, sdma);
	if (r)
		return r;
	r = cik_sdma_rlc_resume(adev);
	if (r)
		return r;

	return 0;
}

/* cik_sdma_ring_pad_ib - pad the IB to the required number of dw */
static void cik_sdma_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	struct frb_cik_sdma_instance *sdma = amdgpu_sdma_get_instance_from_ring(ring);
	uint32_t pad_count;
	uint32_t i;

	pad_count = (-ib->length_dw) & 7;
	for (i = 0; i < pad_count; i++)
		if (sdma && sdma->burst_nop && (i == 0))
			frb_ib_write(ring->adev, ib,
				     SDMA_PACKET(SDMA_OPCODE_NOP, 0, 0) |
				     SDMA_NOP_COUNT(pad_count - 1));
		else
			frb_ib_write(ring->adev, ib,
				     SDMA_PACKET(SDMA_OPCODE_NOP, 0, 0));
}

/*
 * cik_sdma_ring_emit_pipeline_sync - wait for the ring's last fence (the IB
 * that follows reads what the work before it wrote)
 */
static void cik_sdma_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	/* wait for idle */
	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_POLL_REG_MEM, 0,
					 SDMA_POLL_REG_MEM_EXTRA_OP(0) |
					 SDMA_POLL_REG_MEM_EXTRA_FUNC(3) | /* equal */
					 SDMA_POLL_REG_MEM_EXTRA_M));
	frb_ring_write(ring, addr & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, seq); /* reference */
	frb_ring_write(ring, 0xffffffff); /* mask */
	frb_ring_write(ring, (0xfff << 16) | 4); /* retry count, poll interval */
}

/*
 * Another engine's fence (the GFX ring's, for the accelerator's 2D work):
 * the pipeline sync's packet with the compare function and memory given.
 */
static void cik_sdma_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_POLL_REG_MEM, 0,
					 SDMA_POLL_REG_MEM_EXTRA_OP(0) |
					 SDMA_POLL_REG_MEM_EXTRA_FUNC(func) |
					 SDMA_POLL_REG_MEM_EXTRA_M));
	frb_ring_write(ring, addr & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, (0xfff << 16) | 4);
}

/* cik_sdma_ring_emit_wreg (cik_flush_hdp / cik_invalidate_hdp from the ring) */
static void cik_sdma_ring_emit_wreg(struct frb_ring *ring,
				    uint32_t reg, uint32_t val)
{
	frb_ring_write(ring, SDMA_PACKET(SDMA_OPCODE_SRBM_WRITE, 0, 0xf000));
	frb_ring_write(ring, reg);
	frb_ring_write(ring, val);
}

static const struct frb_ring_funcs cik_sdma_ring_funcs = {
	.align_mask = 0xf,
	.nop = SDMA_PACKET(SDMA_OPCODE_NOP, 0, 0),
	.support_64bit_ptrs = 0,
	.get_rptr = cik_sdma_ring_get_rptr,
	.get_wptr = cik_sdma_ring_get_wptr,
	.set_wptr = cik_sdma_ring_set_wptr,
	.emit_ib = cik_sdma_ring_emit_ib,
	.emit_fence = cik_sdma_ring_emit_fence,
	.emit_hdp_flush = cik_sdma_ring_emit_hdp_flush,
	.insert_nop = cik_sdma_ring_insert_nop,
	.pad_ib = cik_sdma_ring_pad_ib,
	.emit_wreg = cik_sdma_ring_emit_wreg,
	.emit_pipeline_sync = cik_sdma_ring_emit_pipeline_sync,
	.emit_wait_mem = cik_sdma_ring_emit_wait_mem,
};

/* --- the clock gating (amdgpu's late init: GATE; hw_fini's ungate) ----------- */

static void cik_enable_sdma_mgcg(struct frb_device *adev,
				 int enable)
{
	uint32_t orig, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_SDMA_MGCG)) {
		WREG32(mmSDMA0_CLK_CTRL + SDMA0_REGISTER_OFFSET, 0x00000100);
		WREG32(mmSDMA0_CLK_CTRL + SDMA1_REGISTER_OFFSET, 0x00000100);
	} else {
		orig = data = RREG32(mmSDMA0_CLK_CTRL + SDMA0_REGISTER_OFFSET);
		data |= 0xff000000;
		if (data != orig)
			WREG32(mmSDMA0_CLK_CTRL + SDMA0_REGISTER_OFFSET, data);

		orig = data = RREG32(mmSDMA0_CLK_CTRL + SDMA1_REGISTER_OFFSET);
		data |= 0xff000000;
		if (data != orig)
			WREG32(mmSDMA0_CLK_CTRL + SDMA1_REGISTER_OFFSET, data);
	}
}

static void cik_enable_sdma_mgls(struct frb_device *adev,
				 int enable)
{
	uint32_t orig, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_SDMA_LS)) {
		orig = data = RREG32(mmSDMA0_POWER_CNTL + SDMA0_REGISTER_OFFSET);
		data |= 0x100;
		if (orig != data)
			WREG32(mmSDMA0_POWER_CNTL + SDMA0_REGISTER_OFFSET, data);

		orig = data = RREG32(mmSDMA0_POWER_CNTL + SDMA1_REGISTER_OFFSET);
		data |= 0x100;
		if (orig != data)
			WREG32(mmSDMA0_POWER_CNTL + SDMA1_REGISTER_OFFSET, data);
	} else {
		orig = data = RREG32(mmSDMA0_POWER_CNTL + SDMA0_REGISTER_OFFSET);
		data &= ~0x100;
		if (orig != data)
			WREG32(mmSDMA0_POWER_CNTL + SDMA0_REGISTER_OFFSET, data);

		orig = data = RREG32(mmSDMA0_POWER_CNTL + SDMA1_REGISTER_OFFSET);
		data &= ~0x100;
		if (orig != data)
			WREG32(mmSDMA0_POWER_CNTL + SDMA1_REGISTER_OFFSET, data);
	}
}

/* cik_sdma_set_clockgating_state */
void frb_cik_sdma_set_clockgating_state(struct frb_device *adev, int gate)
{
	struct frb_cik_sdma *sdma = adev->cik_sdma;

	if (!sdma || !sdma->touched)
		return;
	cik_enable_sdma_mgcg(adev, gate);
	cik_enable_sdma_mgls(adev, gate);
}

/* --- the buffer functions ------------------------------------------------------ */

/*
 * cik_sdma_emit_copy_buffer - copy buffer using the sDMA engine
 * (byte_count bytes from src_offset to dst_offset, GPU addresses)
 */
static void cik_sdma_emit_copy_buffer(struct frb_device *adev, struct frb_ib *ib,
				      uint64_t src_offset,
				      uint64_t dst_offset,
				      uint32_t byte_count)
{
	frb_ib_write(adev, ib, SDMA_PACKET(SDMA_OPCODE_COPY, SDMA_COPY_SUB_OPCODE_LINEAR, 0));
	frb_ib_write(adev, ib, byte_count);
	frb_ib_write(adev, ib, 0); /* src/dst endian swap */
	frb_ib_write(adev, ib, lower_32_bits(src_offset));
	frb_ib_write(adev, ib, upper_32_bits(src_offset));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
}

/*
 * cik_sdma_emit_fill_buffer - fill buffer using the sDMA engine; the
 * header's extra bits stay 0 as in amdgpu (FILLSIZE 0; see the file header)
 */
static void cik_sdma_emit_fill_buffer(struct frb_device *adev, struct frb_ib *ib,
				      uint32_t src_data,
				      uint64_t dst_offset,
				      uint32_t byte_count)
{
	frb_ib_write(adev, ib, SDMA_PACKET(SDMA_OPCODE_CONSTANT_FILL, 0, 0));
	frb_ib_write(adev, ib, lower_32_bits(dst_offset));
	frb_ib_write(adev, ib, upper_32_bits(dst_offset));
	frb_ib_write(adev, ib, src_data);
	frb_ib_write(adev, ib, byte_count);
}

static struct frb_ring *sdma_ring(struct frb_device *adev, int instance)
{
	struct frb_cik_sdma *sdma = adev->cik_sdma;

	if (!sdma || !sdma->sw_done || instance < 0 || instance >= sdma->num_instances)
		return NULL;
	return &sdma->instance[instance].ring;
}

/* the accelerator's 2D work runs on SDMA0's ring; NULL unless it is up */
struct frb_ring *frb_cik_sdma_ring(struct frb_device *adev, int instance)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	return ring && ring->ready ? ring : NULL;
}

/* cik_sdma_ring_pad_ib, for an IB built elsewhere (frb_rect.c) */
void frb_cik_sdma_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	cik_sdma_ring_pad_ib(ring, ib);
}

/* the job, submitted directly (amdgpu_job_submit_direct) and waited for: pad, schedule, poll */
static int sdma_submit_and_wait(struct frb_ring *ring, struct frb_ib *ib, const char *what)
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

/*
 * amdgpu_copy_buffer (amdgpu_ttm.c) on one SDMA ring, polled: byte_count
 * bytes from src to dst, GPU addresses, split at copy_max_bytes
 */
int frb_cik_sdma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset,
			     uint64_t dst_offset, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = CIK_SDMA_COPY_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * CIK_SDMA_COPY_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size_in_bytes = byte_count < max_bytes ? byte_count : max_bytes;

		cik_sdma_emit_copy_buffer(adev, &ib, src_offset, dst_offset, cur_size_in_bytes);
		src_offset += cur_size_in_bytes;
		dst_offset += cur_size_in_bytes;
		byte_count -= cur_size_in_bytes;
	}
	return sdma_submit_and_wait(ring, &ib, "copy");
}

/* amdgpu_ttm_fill_mem on one SDMA ring, polled: byte_count bytes at dst_addr (cik_sdma_emit_fill_buffer) */
int frb_cik_sdma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data,
			     uint64_t dst_addr, uint32_t byte_count)
{
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t max_bytes = CIK_SDMA_FILL_MAX_BYTES, num_loops, num_dw, i;
	struct frb_ib ib;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	num_loops = (byte_count + max_bytes - 1) / max_bytes;
	num_dw = (num_loops * CIK_SDMA_FILL_NUM_DW + 7) & ~7u;
	r = frb_ib_get(adev, num_dw * 4, &ib);
	if (r)
		return r;
	for (i = 0; i < num_loops; i++) {
		uint32_t cur_size = byte_count < max_bytes ? byte_count : max_bytes;

		cik_sdma_emit_fill_buffer(adev, &ib, src_data, dst_addr, cur_size);
		dst_addr += cur_size;
		byte_count -= cur_size;
	}
	return sdma_submit_and_wait(ring, &ib, "fill");
}

/* --- the copy and fill tests: VRAM to VRAM, checked by CPU readback -------- */

static uint32_t test_pattern(uint32_t seed, uint32_t i)
{
	uint32_t x = (i + 1) * 0x9e3779b1u ^ seed;

	x ^= x >> 15;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	return x;
}

static int sdma_test_buffers(struct frb_device *adev, struct frb_cik_sdma *sdma)
{
	int r, i;

	for (i = 0; i < 2; i++) {
		if (sdma->test_bo[i])
			continue;
		r = frb_vram_obj_alloc(adev, FRB_CIK_SDMA_TEST_BYTES, 4096, &sdma->test_bo[i]);
		if (r)
			return r;
	}
	return 0;
}

/* dst[0, n) must be the pattern (or the fill value), dst[n, total) still the poison */
static int sdma_test_check(struct frb_device *adev, struct frb_ring *ring, const char *what,
			   uint32_t dst, uint32_t n, uint32_t total, uint32_t seed, int fill,
			   uint32_t fill_value)
{
	uint32_t i, got, want, bad = 0, first = 0, first_got = 0, first_want = 0;

	frb_invalidate_hdp(adev);
	for (i = 0; i < total; i++) {
		got = frb_vram_read32(adev, dst + 4 * i);
		want = i >= n ? FRB_CIK_SDMA_POISON : fill ? fill_value : test_pattern(seed, i);
		if (got != want) {
			if (!bad) {
				first = i;
				first_got = got;
				first_want = want;
			}
			bad++;
		}
	}
	if (bad) {
		frb_err(adev, "%s: %s: %u of %u dwords wrong; first at +0x%x: 0x%08x, want 0x%08x%s",
			ring->name, what, bad, total, first * 4, first_got, first_want,
			first >= n ? " (past the end: the guard)" : "");
		return -EIO;
	}
	return 0;
}

static int sdma_test_copy_fill(struct frb_device *adev, int instance, int fill)
{
	struct frb_cik_sdma *sdma = adev->cik_sdma;
	struct frb_ring *ring = sdma_ring(adev, instance);
	uint32_t total = FRB_CIK_SDMA_TEST_BYTES / 4, bytes = FRB_CIK_SDMA_TEST_BYTES - FRB_CIK_SDMA_TEST_GUARD;
	uint32_t src, dst, seed, value, i, first;
	uint64_t t0;
	int r;

	if (!ring || !ring->ready)
		return -ENODEV;
	r = sdma_test_buffers(adev, sdma);
	if (r)
		return r;
	src = sdma->test_bo[0];
	dst = sdma->test_bo[1];
	sdma->test_runs++;
	seed = 0x5d3a0000u ^ ((uint32_t)instance << 12) ^ (sdma->test_runs * 0x01000193u);
	/*
	 * Four different bytes, the low one new each run: a byte fill repeats
	 * the low byte (SDMA 3's FILLSIZE 0), a dword fill keeps all four
	 * (SI's DMA); anything else shows.
	 */
	value = 0xa1b2c300u | ((0xd4u + sdma->test_runs * 0x1du) & 0xff);

	for (i = 0; i < total; i++) {
		if (!fill)
			frb_vram_write32(adev, src + 4 * i, test_pattern(seed, i));
		frb_vram_write32(adev, dst + 4 * i, FRB_CIK_SDMA_POISON);
	}
	/* the CPU's VRAM writes reach the engine through the HDP: frb_ring_commit flushes it */
	t0 = adev->os->time_ms(adev->os->cookie);
	if (fill)
		r = frb_cik_sdma_fill_buffer(adev, instance, value, frb_vram_gpu_addr(adev, dst), bytes);
	else
		r = frb_cik_sdma_copy_buffer(adev, instance, frb_vram_gpu_addr(adev, src),
					     frb_vram_gpu_addr(adev, dst), bytes);
	if (r)
		return r;
	t0 = adev->os->time_ms(adev->os->cookie) - t0;
	if (!fill) {
		r = sdma_test_check(adev, ring, "copy", dst, bytes / 4, total, seed, 0, 0);
		if (r)
			return r;
		frb_info(adev, "%s: copy test passed: %u bytes %llx -> %llx, %u-byte guard intact (%llu ms)",
			 ring->name, bytes, (unsigned long long)frb_vram_gpu_addr(adev, src),
			 (unsigned long long)frb_vram_gpu_addr(adev, dst), FRB_CIK_SDMA_TEST_GUARD,
			 (unsigned long long)t0);
		return 0;
	}
	/* which fill it was: the first dword says, the whole target must agree */
	frb_invalidate_hdp(adev);
	first = frb_vram_read32(adev, dst);
	if (first != value && first != (value & 0xff) * 0x01010101u) {
		frb_err(adev, "%s: fill: src_data 0x%08x landed as 0x%08x: neither a byte nor a dword fill",
			ring->name, value, first);
		return -EIO;
	}
	r = sdma_test_check(adev, ring, "fill", dst, bytes / 4, total, seed, 1, first);
	if (r)
		return r;
	frb_info(adev, "%s: fill test passed: src_data 0x%08x -> 0x%08x over %u bytes at %llx (%s fill, %s), "
		 "%u-byte guard intact (%llu ms)", ring->name, value, first, bytes,
		 (unsigned long long)frb_vram_gpu_addr(adev, dst), first == value ? "a whole-dword" : "a byte",
		 first == value ? "as SI's DMA" : "as SDMA 3", FRB_CIK_SDMA_TEST_GUARD, (unsigned long long)t0);
	return 0;
}

/* --- the stage, the tests by hand, the teardown ------------------------------ */

int frb_cik_sdma_init(struct frb_device *adev)
{
	struct frb_cik_sdma *sdma = adev->cik_sdma;
	struct frb_ring *ring;
	char name[8];
	int r, i;

	if (adev->stage_done < FRB_STAGE_SMU || !adev->aper)
		return -EPERM;
	if (!sdma) {
		sdma = (struct frb_cik_sdma *)adev->os->alloc(adev->os->cookie, sizeof(*sdma));
		if (!sdma)
			return -ENOMEM;
		memset(sdma, 0, sizeof(*sdma));
		adev->cik_sdma = sdma;
	}

	if (!sdma->sw_done) {
		/* cik_sdma_early_init */
		sdma->num_instances = SDMA_MAX_INSTANCE;
		r = cik_sdma_init_microcode(adev, sdma);
		if (r)
			return r;

		/* cik_sdma_sw_init: the rings (no IRQ sources) */
		for (i = 0; i < sdma->num_instances; i++) {
			ring = &sdma->instance[i].ring;
			name[0] = 's'; name[1] = 'd'; name[2] = 'm'; name[3] = 'a';
			name[4] = (char)('0' + i); name[5] = 0;
			r = frb_ring_init(adev, ring, name, &cik_sdma_ring_funcs, 1024);
			if (r)
				return r;
			ring->me = i;	/* cik_sdma_set_ring_funcs */
			frb_ring_clear(ring);
		}
		sdma->sw_done = 1;
	}

	/* cik_sdma_hw_init */
	sdma->touched = 1;
	cik_sdma_soft_reset(adev);
	r = cik_sdma_start(adev, sdma);
	for (i = 0; i < sdma->num_instances; i++)
		frb_info(adev, "%s: microcode version 0x%x, feature %u (%s NOPs), ring at %llx",
			 sdma->instance[i].ring.name, sdma->instance[i].fw_version,
			 sdma->instance[i].feature_version, sdma->instance[i].burst_nop ? "burst" : "single",
			 (unsigned long long)sdma->instance[i].ring.gpu_addr);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests, for these two rings */
	for (i = 0; i < sdma->num_instances; i++) {
		ring = &sdma->instance[i].ring;
		r = cik_sdma_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		if (r) {
			frb_err(adev, "IB test failed on %s (%d).", ring->name, r);
			ring->ready = 0;
			return r;
		}
	}
	return 0;
}

int frb_cik_sdma_test(struct frb_device *adev, int instance, int test)
{
	struct frb_ring *ring = sdma_ring(adev, instance);

	if (!ring || !adev->cik_sdma->touched)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return cik_sdma_ring_test_ring(ring);
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return cik_sdma_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_COPY:
		return sdma_test_copy_fill(adev, instance, 0);
	case FRB_TEST_FILL:
		return sdma_test_copy_fill(adev, instance, 1);
	default:
		return -EINVAL;
	}
}

/* cik_sdma_hw_fini: context switch off, both engines halted, before the driver lets go of VRAM */
void frb_cik_sdma_fini(struct frb_device *adev)
{
	struct frb_cik_sdma *sdma = adev->cik_sdma;

	if (!sdma)
		return;
	if (sdma->touched && sdma->num_instances) {
		cik_ctx_switch_enable(adev, sdma, 0);
		cik_sdma_enable(adev, sdma, 0);
		frb_info(adev, "cik_sdma: both engines halted (hw_fini)");
	}
	adev->os->free(adev->os->cookie, sdma);
	adev->cik_sdma = NULL;
}
