/*
 * uvd_v6_0.c (Linux 7.0.2), for Polaris 12 (UVD 6.3): the decode ring.
 * Stage uvd runs what amdgpu does for this block at load: early_init (the
 * harvest fuse), sw_init (amdgpu_uvd_sw_init: firmware and VCPU bo; the
 * ring; amdgpu_uvd_resume), hw_init (vi_set_uvd_clocks(100 MHz), clock
 * gating off, MGCG on, the ring test, whose ring_alloc is begin_use:
 * 533/400 MHz and uvd_v6_0_start; the semaphore timeouts), then the IB
 * test amdgpu runs from late_init (amdgpu_uvd_ring_test_ib) and the idle
 * work amdgpu runs a second later (clocks 0, the VCPU stopped, gated).
 * The ring is in VM mode (uvd_v6_0_ring_vm_funcs: Polaris and later),
 * every IB in VMID 0.
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - NO ENCODE RINGS. uvd_v6_0_enc_support() is true for this firmware
 *   (1.130 >= 1.130.16) and Linux sets up two encode rings in
 *   uvd_v6_0_start (UVD_RB_RPTR/WPTR/BASE_LO/BASE_HI/SIZE and their 2
 *   twins) and tests them; they are out of Firebrand's scope: none of
 *   those writes, no "UVD ENC".
 * - PLACEMENT. The ring and its writeback are in VRAM (frb_ring.c), the
 *   VCPU bo in the layout's UVD region (frb_uvd.c): RBC_RB_64BIT_BAR,
 *   RBC_RB_RPTR_ADDR (upper bits >> 2: 0x3d for VRAM, 0x3f for Linux's
 *   GTT) and VCPU_CACHE_64BIT_BAR carry other addresses. frb_ring_commit
 *   flushes the HDP by MMIO before every WPTR write (a VRAM ring); Linux's
 *   GTT ring needs none (its emit_hdp_flush is empty too).
 * - CLOCKS, the powerplay arm: see frb_uvd.c. Where the capture has SMC
 *   messages (UVD DPM) this writes CG_VCLK_CNTL/CG_DCLK_CNTL.
 * - BYTE ORDER. Under __BIG_ENDIAN amdgpu sets UVD_LMI_SWAP_CNTL 0xa
 *   (swap 8 in 32 for RB and IB fetches) and writes the ring and IBs in
 *   the CPU's order. Firebrand's default writes them little-endian and
 *   leaves the swap 0, as everything else it hands the engines (the
 *   little-endian capture's value); `swapped` takes amdgpu's arm instead
 *   (FRB_TEST_SWAP_PROBE runs both). Messages are little-endian either way.
 * - NO INTERRUPTS. UVD_MASTINT_EN gets VCPU_EN|SYS_EN as on Linux, but no
 *   IH ring exists to take the trap; fences are polled.
 * - The IB test and the idle work run at the end of the stage, not from
 *   late_init and a delayed work item.
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
 *
 * Authors: Christian König <christian.koenig@amd.com>
 */
#include "frb_uvd.h"
#include "include/asic_reg/uvd/uvd_6_0_d.h"
#include "include/asic_reg/uvd/uvd_6_0_sh_mask.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/oss/oss_3_0_sh_mask.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"
#include "include/asic_reg/smu/smu_7_1_3_sh_mask.h"
#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/bif/bif_5_0_d.h"

#define PACKET0(reg, n)		((((uint32_t)(n) & 0x3FFF) << 16) | ((reg) & 0xFFFF))	/* vid.h, type 0 */

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

static int uvd_v6_0_start(struct frb_device *adev);
static void uvd_v6_0_stop(struct frb_device *adev);
static void uvd_v6_0_set_sw_clock_gating(struct frb_device *adev);
static void uvd_v6_0_enable_mgcg(struct frb_device *adev, int enable);

/* --- the ring's byte order: frb_uvd.c (frb_uvd_ring_write, frb_uvd_ib_write, frb_uvd_ib_pad) --- */

#define amdgpu_ring_write(ring, v)	frb_uvd_ring_write((ring), (v))

/* --- ring functions --------------------------------------------------------- */

/**
 * uvd_v6_0_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint64_t uvd_v6_0_ring_get_rptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_RPTR);
}

/**
 * uvd_v6_0_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint64_t uvd_v6_0_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_WPTR);
}

/**
 * uvd_v6_0_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void uvd_v6_0_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));
}

/**
 * uvd_v6_0_ring_emit_fence - emit an fence & trap command
 *
 * @ring: amdgpu_ring pointer
 * @addr: address
 * @seq: sequence number
 * @flags: fence related flags
 *
 * Write a fence and a trap command to the ring.
 */
static void uvd_v6_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq,
				     unsigned flags)
{
	(void)flags;	/* WARN_ON(flags & AMDGPU_FENCE_FLAG_64BIT) */

	amdgpu_ring_write(ring, PACKET0(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, (uint32_t)seq);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, addr & 0xffffffff);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, upper_32_bits(addr) & 0xff);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 2);
}

/**
 * uvd_v6_0_ring_emit_hdp_flush - skip HDP flushing
 *
 * @ring: amdgpu_ring pointer
 */
static void uvd_v6_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	/* The firmware doesn't seem to like touching registers at this point. */
	(void)ring;
}

/**
 * uvd_v6_0_ring_test_ring - register write test
 *
 * @ring: amdgpu_ring pointer
 *
 * Test if we can successfully write to the context register
 */
static int uvd_v6_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32(mmUVD_CONTEXT_ID, 0xCAFEDEAD);
	/* amdgpu_ring_alloc: begin_use first */
	frb_uvd_ring_begin_use(ring);
	r = frb_ring_alloc(ring, 3);
	if (r)
		return r;

	amdgpu_ring_write(ring, PACKET0(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(mmUVD_CONTEXT_ID);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		frb_err(adev, "%s: ring test failed: UVD_CONTEXT_ID 0x%08x, RBC_RB_RPTR 0x%llx, WPTR 0x%llx, "
			"UVD_STATUS 0x%08x", ring->name, tmp, (unsigned long long)uvd_v6_0_ring_get_rptr(ring),
			(unsigned long long)ring->wptr, RREG32(mmUVD_STATUS));
		r = -ETIMEDOUT;
	} else {
		frb_info(adev, "%s: ring test passed (%u us)", ring->name, i);
	}

	return r;
}

/**
 * uvd_v6_0_ring_emit_ib - execute indirect buffer
 *
 * @ring: amdgpu_ring pointer
 * @ib: indirect buffer to execute
 * @vmid: the job's VMID (0: the kernel's)
 * @flags: unused
 *
 * Write ring commands to execute the indirect buffer
 */
static void uvd_v6_0_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib,
				  uint32_t vmid, uint32_t flags)
{
	(void)flags;

	amdgpu_ring_write(ring, PACKET0(mmUVD_LMI_RBC_IB_VMID, 0));
	amdgpu_ring_write(ring, vmid);

	amdgpu_ring_write(ring, PACKET0(mmUVD_LMI_RBC_IB_64BIT_BAR_LOW, 0));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH, 0));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_RBC_IB_SIZE, 0));
	amdgpu_ring_write(ring, ib->length_dw);
}

static void uvd_v6_0_ring_emit_wreg(struct frb_ring *ring, uint32_t reg, uint32_t val)
{
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, reg << 2);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, val);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 0x8);
}

static void uvd_v6_0_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, PACKET0(mmUVD_GP_SCRATCH8, 0));
	amdgpu_ring_write(ring, 0xffffffff); /* mask */
	amdgpu_ring_write(ring, PACKET0(mmUVD_GP_SCRATCH9, 0));
	amdgpu_ring_write(ring, seq);
	amdgpu_ring_write(ring, PACKET0(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 0xE);
}

static void uvd_v6_0_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	uint32_t i;

	/* WARN_ON(ring->wptr % 2 || count % 2) */
	if ((ring->wptr & 1) || (count & 1))
		frb_err(ring->adev, "%s: odd NOP fill (wptr %llu, %u dwords)", ring->name,
			(unsigned long long)ring->wptr, count);

	for (i = 0; i < count / 2; i++) {
		amdgpu_ring_write(ring, PACKET0(mmUVD_NO_OP, 0));
		amdgpu_ring_write(ring, 0);
	}
}

static void uvd_v6_0_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	frb_uvd_ib_pad(ring->adev, ib);
}

/* uvd_v6_0_ring_vm_funcs (CHIP_POLARIS10 and later); no parse_cs in VM mode */
static const struct frb_ring_funcs uvd_v6_0_ring_vm_funcs = {
	.align_mask = 0xf,
	.nop = PACKET0(mmUVD_NO_OP, 0),
	.support_64bit_ptrs = 0,
	.get_rptr = uvd_v6_0_ring_get_rptr,
	.get_wptr = uvd_v6_0_ring_get_wptr,
	.set_wptr = uvd_v6_0_ring_set_wptr,
	.emit_ib = uvd_v6_0_ring_emit_ib,
	.emit_fence = uvd_v6_0_ring_emit_fence,
	.emit_hdp_flush = uvd_v6_0_ring_emit_hdp_flush,
	.insert_nop = uvd_v6_0_ring_insert_nop,
	.pad_ib = uvd_v6_0_ring_pad_ib,
	.emit_wreg = uvd_v6_0_ring_emit_wreg,
	.emit_pipeline_sync = uvd_v6_0_ring_emit_pipeline_sync,
};

/* --- early/sw/hw init ------------------------------------------------------- */

static int uvd_v6_0_early_init(struct frb_device *adev)
{
	adev->uvd->num_uvd_inst = 1;
	adev->uvd->harvest_config = 0;
	if (RREG32_SMC(ixCC_HARVEST_FUSES) & CC_HARVEST_FUSES__UVD_DISABLE_MASK)
		return -ENOENT;

	/* uvd_v6_0_set_ring_funcs: VM mode; no enc rings, no IRQ funcs here */
	frb_info(adev, "UVD is enabled in VM mode");
	return 0;
}

static int uvd_v6_0_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	int r;

	/* no UVD TRAP source: no IH ring */
	r = frb_uvd_sw_init(adev);
	if (r)
		return r;

	/* !uvd_v6_0_enc_support's arm, whatever the firmware: the encode rings are not ported */
	frb_info(adev, "UVD ENC is disabled (not ported)");

	ring = &uvd->inst[0].ring;
	if (!ring->funcs) {
		r = frb_ring_init(adev, ring, "uvd", &uvd_v6_0_ring_vm_funcs, 512);
		if (r)
			return r;
	}

	return frb_uvd_resume(adev);
}

/**
 * uvd_v6_0_hw_init - start and test UVD block
 *
 * Initialize the hardware, boot up the VCPU and do some testing
 */
static int uvd_v6_0_hw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	uint32_t tmp;
	int r;

	uvd->touched = 1;
	frb_vi_set_uvd_clocks(adev, 10000, 10000);
	frb_uvd_v6_0_set_clockgating_state(adev, 0);
	uvd_v6_0_enable_mgcg(adev, 1);

	/* amdgpu_ring_test_helper */
	r = uvd_v6_0_ring_test_ring(ring);
	ring->ready = !r;
	if (r)
		goto done;

	frb_uvd_ring_begin_use(ring);
	r = frb_ring_alloc(ring, 10);
	if (r) {
		frb_err(adev, "ring alloc failed (%d).", r);
		goto done;
	}

	tmp = PACKET0(mmUVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0(mmUVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0(mmUVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	/* Clear timeout status bits */
	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_TIMEOUT_STATUS, 0));
	amdgpu_ring_write(ring, 0x8);

	amdgpu_ring_write(ring, PACKET0(mmUVD_SEMA_CNTL, 0));
	amdgpu_ring_write(ring, 3);

	frb_ring_commit(ring);

done:
	if (!r)
		frb_info(adev, "UVD initialized successfully.");

	return r;
}

/**
 * uvd_v6_0_hw_fini - stop the hardware block
 *
 * Stop the UVD block, mark ring as not ready any more
 */
static int uvd_v6_0_hw_fini(struct frb_device *adev)
{
	if (RREG32(mmUVD_STATUS) != 0)
		uvd_v6_0_stop(adev);

	return 0;
}

/**
 * uvd_v6_0_mc_resume - memory controller programming
 *
 * Let the UVD memory controller know it's offsets
 */
static void uvd_v6_0_mc_resume(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_gfx_info gfx;
	uint32_t gb_addr_config;
	uint64_t offset;
	uint32_t size;

	/* gfx.config.gb_addr_config: gfx_v8_0's (stage gfx), else its golden Polaris value */
	gb_addr_config = frb_gfx_v8_0_get_info(adev, &gfx) ? 0x22011002 : gfx.gb_addr_config;

	/* program memory controller bits 0-27 */
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
			lower_32_bits(uvd->inst[0].gpu_addr));
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
			upper_32_bits(uvd->inst[0].gpu_addr));

	offset = AMDGPU_UVD_FIRMWARE_OFFSET;
	size = AMDGPU_UVD_FIRMWARE_SIZE(uvd);
	WREG32(mmUVD_VCPU_CACHE_OFFSET0, (uint32_t)(offset >> 3));
	WREG32(mmUVD_VCPU_CACHE_SIZE0, size);

	offset += size;
	size = AMDGPU_UVD_HEAP_SIZE;
	WREG32(mmUVD_VCPU_CACHE_OFFSET1, (uint32_t)(offset >> 3));
	WREG32(mmUVD_VCPU_CACHE_SIZE1, size);

	offset += size;
	size = AMDGPU_UVD_STACK_SIZE +
	       (AMDGPU_UVD_SESSION_SIZE * uvd->max_handles);
	WREG32(mmUVD_VCPU_CACHE_OFFSET2, (uint32_t)(offset >> 3));
	WREG32(mmUVD_VCPU_CACHE_SIZE2, size);

	WREG32(mmUVD_UDEC_ADDR_CONFIG, gb_addr_config);
	WREG32(mmUVD_UDEC_DB_ADDR_CONFIG, gb_addr_config);
	WREG32(mmUVD_UDEC_DBW_ADDR_CONFIG, gb_addr_config);

	WREG32(mmUVD_GP_SCRATCH4, uvd->max_handles);
}

/**
 * uvd_v6_0_start - start UVD block
 *
 * Setup and start the UVD block
 */
static int uvd_v6_0_start(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	uint32_t rb_bufsz, tmp;
	uint32_t lmi_swap_cntl;
	uint32_t mp_swap_cntl;
	int i, j, r;

	/* disable DPG */
	WREG32_P(mmUVD_POWER_STATUS, 0, ~UVD_POWER_STATUS__UVD_PG_MODE_MASK);

	/* disable byte swapping */
	lmi_swap_cntl = 0;
	mp_swap_cntl = 0;

	uvd_v6_0_mc_resume(adev);

	/* disable interupt */
	WREG32_FIELD(UVD_MASTINT_EN, VCPU_EN, 0);

	/* stall UMC and register bus before resetting VCPU */
	WREG32_FIELD(UVD_LMI_CTRL2, STALL_ARB_UMC, 1);
	mdelay(1);

	/* put LMI, VCPU, RBC etc... into reset */
	WREG32(mmUVD_SOFT_RESET,
		UVD_SOFT_RESET__LMI_SOFT_RESET_MASK |
		UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK |
		UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK |
		UVD_SOFT_RESET__RBC_SOFT_RESET_MASK |
		UVD_SOFT_RESET__CSM_SOFT_RESET_MASK |
		UVD_SOFT_RESET__CXW_SOFT_RESET_MASK |
		UVD_SOFT_RESET__TAP_SOFT_RESET_MASK |
		UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);
	mdelay(5);

	/* take UVD block out of reset */
	WREG32_FIELD(SRBM_SOFT_RESET, SOFT_RESET_UVD, 0);
	mdelay(5);

	/* initialize UVD memory controller */
	WREG32(mmUVD_LMI_CTRL,
		(0x40 << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) |
		UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK |
		UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
		UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK |
		UVD_LMI_CTRL__REQ_MODE_MASK |
		UVD_LMI_CTRL__DISABLE_ON_FWV_FAIL_MASK);

	/* amdgpu's #ifdef __BIG_ENDIAN arm, when asked for: swap (8 in 32) RB and IB */
	if (uvd->swapped) {
		lmi_swap_cntl = 0xa;
		mp_swap_cntl = 0;
	}
	WREG32(mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);
	WREG32(mmUVD_MP_SWAP_CNTL, mp_swap_cntl);

	WREG32(mmUVD_MPC_SET_MUXA0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXA1, 0x0);
	WREG32(mmUVD_MPC_SET_MUXB0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXB1, 0x0);
	WREG32(mmUVD_MPC_SET_ALU, 0);
	WREG32(mmUVD_MPC_SET_MUX, 0x88);

	/* take all subblocks out of reset, except VCPU */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
	mdelay(5);

	/* enable VCPU clock */
	WREG32(mmUVD_VCPU_CNTL, UVD_VCPU_CNTL__CLK_EN_MASK);

	/* enable UMC */
	WREG32_FIELD(UVD_LMI_CTRL2, STALL_ARB_UMC, 0);

	/* boot up the VCPU */
	WREG32(mmUVD_SOFT_RESET, 0);
	mdelay(10);

	r = -1;
	for (i = 0; i < 10; ++i) {
		uint32_t status = 0;

		for (j = 0; j < 100; ++j) {
			status = RREG32(mmUVD_STATUS);
			if (status & 2)
				break;
			mdelay(10);
		}
		r = 0;
		if (status & 2)
			break;

		frb_err(adev, "UVD not responding, trying to reset the VCPU!!!");
		WREG32_FIELD(UVD_SOFT_RESET, VCPU_SOFT_RESET, 1);
		mdelay(10);
		WREG32_FIELD(UVD_SOFT_RESET, VCPU_SOFT_RESET, 0);
		mdelay(10);
		r = -1;
	}

	if (r) {
		frb_err(adev, "UVD not responding, giving up!!!");
		return -ETIMEDOUT;
	}
	/* enable master interrupt */
	WREG32_P(mmUVD_MASTINT_EN,
		(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK),
		~(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK));

	/* clear the bit 4 of UVD_STATUS */
	WREG32_P(mmUVD_STATUS, 0, ~(2 << UVD_STATUS__VCPU_REPORT__SHIFT));

	/* force RBC into idle state */
	rb_bufsz = order_base_2(ring->ring_size);
	tmp = REG_SET_FIELD(0, UVD_RBC_RB_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BLKSZ, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_FETCH, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_WPTR_POLL_EN, 0);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_UPDATE, 1);
	tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_RPTR_WR_EN, 1);
	WREG32(mmUVD_RBC_RB_CNTL, tmp);

	/* set the write pointer delay */
	WREG32(mmUVD_RBC_RB_WPTR_CNTL, 0);

	/* set the wb address */
	WREG32(mmUVD_RBC_RB_RPTR_ADDR, (upper_32_bits(ring->gpu_addr) >> 2));

	/* program the RB_BASE for ring buffer */
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW,
			lower_32_bits(ring->gpu_addr));
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH,
			upper_32_bits(ring->gpu_addr));

	/* Initialize the ring buffer's read and write pointers */
	WREG32(mmUVD_RBC_RB_RPTR, 0);

	ring->wptr = RREG32(mmUVD_RBC_RB_RPTR);
	WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));

	WREG32_FIELD(UVD_RBC_RB_CNTL, RB_NO_FETCH, 0);

	/* uvd_v6_0_enc_support(): the encode rings are not ported (see the header) */

	uvd->inst[0].starts++;
	frb_info(adev, "uvd: VCPU up (start %u): ring %llx, %u bytes, LMI swap %u", uvd->inst[0].starts,
		 (unsigned long long)ring->gpu_addr, ring->ring_size, lmi_swap_cntl);
	return 0;
}

/**
 * uvd_v6_0_stop - stop UVD block
 *
 * stop the UVD block
 */
static void uvd_v6_0_stop(struct frb_device *adev)
{
	/* force RBC into idle state */
	WREG32(mmUVD_RBC_RB_CNTL, 0x11010101);

	/* Stall UMC and register bus before resetting VCPU */
	WREG32_P(mmUVD_LMI_CTRL2, 1 << 8, ~(1 << 8));
	mdelay(1);

	/* put VCPU into reset */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
	mdelay(5);

	/* disable VCPU clock */
	WREG32(mmUVD_VCPU_CNTL, 0x0);

	/* Unstall UMC and register bus */
	WREG32_P(mmUVD_LMI_CTRL2, 0, ~(1 << 8));

	WREG32(mmUVD_STATUS, 0);
}

/* --- idle, soft reset ------------------------------------------------------- */

static int uvd_v6_0_is_idle(struct frb_device *adev)
{
	return !(RREG32(mmSRBM_STATUS) & SRBM_STATUS__UVD_BUSY_MASK);
}

static int uvd_v6_0_wait_for_idle(struct frb_device *adev)
{
	unsigned i;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (uvd_v6_0_is_idle(adev))
			return 0;
	}
	return -ETIMEDOUT;
}

/* --- clock and power gating ------------------------------------------------- */

static void uvd_v6_0_enable_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t data1, data3;

	data1 = RREG32(mmUVD_SUVD_CGC_GATE);
	data3 = RREG32(mmUVD_CGC_GATE);

	data1 |= UVD_SUVD_CGC_GATE__SRE_MASK |
		     UVD_SUVD_CGC_GATE__SIT_MASK |
		     UVD_SUVD_CGC_GATE__SMP_MASK |
		     UVD_SUVD_CGC_GATE__SCM_MASK |
		     UVD_SUVD_CGC_GATE__SDB_MASK |
		     UVD_SUVD_CGC_GATE__SRE_H264_MASK |
		     UVD_SUVD_CGC_GATE__SRE_HEVC_MASK |
		     UVD_SUVD_CGC_GATE__SIT_H264_MASK |
		     UVD_SUVD_CGC_GATE__SIT_HEVC_MASK |
		     UVD_SUVD_CGC_GATE__SCM_H264_MASK |
		     UVD_SUVD_CGC_GATE__SCM_HEVC_MASK |
		     UVD_SUVD_CGC_GATE__SDB_H264_MASK |
		     UVD_SUVD_CGC_GATE__SDB_HEVC_MASK;

	if (enable) {
		data3 |= (UVD_CGC_GATE__SYS_MASK       |
			UVD_CGC_GATE__UDEC_MASK      |
			UVD_CGC_GATE__MPEG2_MASK     |
			UVD_CGC_GATE__RBC_MASK       |
			UVD_CGC_GATE__LMI_MC_MASK    |
			UVD_CGC_GATE__LMI_UMC_MASK   |
			UVD_CGC_GATE__IDCT_MASK      |
			UVD_CGC_GATE__MPRD_MASK      |
			UVD_CGC_GATE__MPC_MASK       |
			UVD_CGC_GATE__LBSI_MASK      |
			UVD_CGC_GATE__LRBBM_MASK     |
			UVD_CGC_GATE__UDEC_RE_MASK   |
			UVD_CGC_GATE__UDEC_CM_MASK   |
			UVD_CGC_GATE__UDEC_IT_MASK   |
			UVD_CGC_GATE__UDEC_DB_MASK   |
			UVD_CGC_GATE__UDEC_MP_MASK   |
			UVD_CGC_GATE__WCB_MASK       |
			UVD_CGC_GATE__JPEG_MASK      |
			UVD_CGC_GATE__SCPU_MASK      |
			UVD_CGC_GATE__JPEG2_MASK);
		/* only in pg enabled, we can gate clock to vcpu*/
		if (adev->pg_flags & AMD_PG_SUPPORT_UVD)
			data3 |= UVD_CGC_GATE__VCPU_MASK;

		data3 &= ~UVD_CGC_GATE__REGS_MASK;
	} else {
		data3 = 0;
	}

	WREG32(mmUVD_SUVD_CGC_GATE, data1);
	WREG32(mmUVD_CGC_GATE, data3);
}

static void uvd_v6_0_set_sw_clock_gating(struct frb_device *adev)
{
	uint32_t data, data2;

	data = RREG32(mmUVD_CGC_CTRL);
	data2 = RREG32(mmUVD_SUVD_CGC_CTRL);


	data &= ~(UVD_CGC_CTRL__CLK_OFF_DELAY_MASK |
		  UVD_CGC_CTRL__CLK_GATE_DLY_TIMER_MASK);


	data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK |
		(1 << REG_FIELD_SHIFT(UVD_CGC_CTRL, CLK_GATE_DLY_TIMER)) |
		(4 << REG_FIELD_SHIFT(UVD_CGC_CTRL, CLK_OFF_DELAY));

	data &= ~(UVD_CGC_CTRL__UDEC_RE_MODE_MASK |
			UVD_CGC_CTRL__UDEC_CM_MODE_MASK |
			UVD_CGC_CTRL__UDEC_IT_MODE_MASK |
			UVD_CGC_CTRL__UDEC_DB_MODE_MASK |
			UVD_CGC_CTRL__UDEC_MP_MODE_MASK |
			UVD_CGC_CTRL__SYS_MODE_MASK |
			UVD_CGC_CTRL__UDEC_MODE_MASK |
			UVD_CGC_CTRL__MPEG2_MODE_MASK |
			UVD_CGC_CTRL__REGS_MODE_MASK |
			UVD_CGC_CTRL__RBC_MODE_MASK |
			UVD_CGC_CTRL__LMI_MC_MODE_MASK |
			UVD_CGC_CTRL__LMI_UMC_MODE_MASK |
			UVD_CGC_CTRL__IDCT_MODE_MASK |
			UVD_CGC_CTRL__MPRD_MODE_MASK |
			UVD_CGC_CTRL__MPC_MODE_MASK |
			UVD_CGC_CTRL__LBSI_MODE_MASK |
			UVD_CGC_CTRL__LRBBM_MODE_MASK |
			UVD_CGC_CTRL__WCB_MODE_MASK |
			UVD_CGC_CTRL__VCPU_MODE_MASK |
			UVD_CGC_CTRL__JPEG_MODE_MASK |
			UVD_CGC_CTRL__SCPU_MODE_MASK |
			UVD_CGC_CTRL__JPEG2_MODE_MASK);
	data2 &= ~(UVD_SUVD_CGC_CTRL__SRE_MODE_MASK |
			UVD_SUVD_CGC_CTRL__SIT_MODE_MASK |
			UVD_SUVD_CGC_CTRL__SMP_MODE_MASK |
			UVD_SUVD_CGC_CTRL__SCM_MODE_MASK |
			UVD_SUVD_CGC_CTRL__SDB_MODE_MASK);

	WREG32(mmUVD_CGC_CTRL, data);
	WREG32(mmUVD_SUVD_CGC_CTRL, data2);
}

static void uvd_v6_0_enable_mgcg(struct frb_device *adev, int enable)
{
	uint32_t orig, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_UVD_MGCG)) {
		data = RREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL);
		data |= 0xfff;
		WREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	} else {
		data = RREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL);
		data &= ~0xfff;
		WREG32_UVD_CTX(ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data &= ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	}
}

/* uvd_v6_0_set_clockgating_state: gate nonzero = AMD_CG_STATE_GATE */
int frb_uvd_v6_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	int enable = gate != 0;

	if (enable) {
		/* wait for STATUS to clear */
		if (uvd_v6_0_wait_for_idle(adev))
			return -EBUSY;
		uvd_v6_0_enable_clock_gating(adev, 1);
		/* enable HW gates because UVD is idle */
/*		uvd_v6_0_set_hw_clock_gating(adev); */
	} else {
		/* disable HW gating and enable Sw gating */
		uvd_v6_0_enable_clock_gating(adev, 0);
	}
	uvd_v6_0_set_sw_clock_gating(adev);
	return 0;
}

/*
 * uvd_v6_0_set_powergating_state: gate nonzero = AMD_PG_STATE_GATE.
 *
 * This doesn't actually powergate the UVD block.
 * That's done in the dpm code via the SMC.  This
 * just re-inits the block as necessary.  The actual
 * gating still happens in the dpm code.  We should
 * revisit this when there is a cleaner line between
 * the smc and the hw blocks
 */
int frb_uvd_v6_0_set_powergating_state(struct frb_device *adev, int gate)
{
	int ret = 0;

	WREG32(mmUVD_POWER_STATUS, UVD_POWER_STATUS__UVD_PG_EN_MASK);

	if (gate) {
		uvd_v6_0_stop(adev);
	} else {
		ret = uvd_v6_0_start(adev);
		if (ret)
			goto out;
	}

out:
	return ret;
}

/* --- the stage, the tests by hand, the teardown ----------------------------- */

int frb_uvd_v6_0_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	int r;

	if (adev->stage_done < FRB_STAGE_SMU || !adev->aper || !adev->atom)
		return -EPERM;
	if (!uvd) {
		uvd = (struct frb_uvd *)adev->os->alloc(adev->os->cookie, sizeof(*uvd));
		if (!uvd)
			return -ENOMEM;
		memset(uvd, 0, sizeof(*uvd));
		adev->uvd = uvd;
	}

	r = uvd_v6_0_early_init(adev);
	if (r) {
		frb_err(adev, "uvd: the harvest fuses say this card has no UVD");
		return r;
	}
	r = uvd_v6_0_sw_init(adev);
	if (r)
		return r;
	r = uvd_v6_0_hw_init(adev);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests, for this ring */
	r = frb_uvd_ring_test_ib(&uvd->inst[0].ring, FRB_FENCE_TIMEOUT_US);
	if (r) {
		frb_err(adev, "IB test failed on %s (%d).", uvd->inst[0].ring.name, r);
		uvd->inst[0].ring.ready = 0;
		return r;
	}

	/* a second later on Linux: amdgpu_uvd_idle_work_handler */
	r = frb_uvd_idle_work_handler(adev);
	if (r)
		frb_err(adev, "uvd: idle work: %d", r);
	return r;
}

/*
 * FRB_TEST_SWAP_PROBE: the VCPU restarted under each LMI swap setting, the
 * ring test and the msg IB test run, the default (little-endian, swap 0)
 * restored. 0 when the default passes; the log says what each did.
 */
static int uvd_swap_probe(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	int mode, r, rr[2], ri[2];

	for (mode = 0; mode < 2; mode++) {
		r = frb_uvd_idle_work_handler(adev);
		if (r)
			return r;
		uvd->swapped = mode;
		rr[mode] = uvd_v6_0_ring_test_ring(ring);
		ring->ready = !rr[mode];
		ri[mode] = rr[mode] ? -1 : frb_uvd_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		frb_info(adev, "uvd: swap probe: %s ring and IBs, UVD_LMI_SWAP_CNTL %s: ring test %d, IB test %d",
			 mode ? "host-order" : "little-endian", mode ? "0xa" : "0", rr[mode], ri[mode]);
		if (rr[mode] || ri[mode]) {
			/*
			 * a ring that fetched garbage: stop the VCPU (hw_fini) and
			 * take its fences as done (the VCPU will not write them now)
			 */
			uvd_v6_0_stop(adev);
			frb_fence_driver_force_completion(ring);
			uvd->powered = 0;
		}
	}
	/* back to the default */
	r = frb_uvd_idle_work_handler(adev);
	uvd->swapped = 0;
	ring->ready = 1;
	if (!r)
		r = uvd_v6_0_ring_test_ring(ring);
	ring->ready = !r;
	if (!r)
		r = frb_uvd_idle_work_handler(adev);
	return r ? r : (rr[0] || ri[0] ? -EIO : 0);
}

int frb_uvd_v6_0_test(struct frb_device *adev, int test)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	int r;

	if (!uvd || !uvd->inst[0].ring.funcs)
		return -ENODEV;
	ring = &uvd->inst[0].ring;
	switch (test) {
	case FRB_TEST_RING:
		r = uvd_v6_0_ring_test_ring(ring);
		ring->ready = !r;
		return r;
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return frb_uvd_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_SWAP_PROBE:
		return uvd_swap_probe(adev);
	case FRB_TEST_IDLE:
		return frb_uvd_idle_work_handler(adev);
	default:
		return -EINVAL;
	}
}

/* uvd_v6_0_hw_fini at unload, before the driver lets go of VRAM */
void frb_uvd_v6_0_fini(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;

	if (!uvd)
		return;
	if (uvd->touched) {
		uvd_v6_0_hw_fini(adev);
		frb_info(adev, "uvd: VCPU stopped (hw_fini)");
	}
	adev->os->free(adev->os->cookie, uvd);
	adev->uvd = NULL;
}
