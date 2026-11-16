/*
 * uvd_v4_2.c (Linux 7.0.2), for Sea Islands (the FirePro W8100, Hawaii):
 * the decode ring. Stage uvd on CIK runs what amdgpu does for this block at
 * load: sw_init (amdgpu_uvd_sw_init: firmware and VCPU bo; the ring;
 * amdgpu_uvd_resume), hw_init (MGCG, cik_set_uvd_clocks(100 MHz), the ring
 * test, whose ring_alloc is begin_use: with CI DPM the SMC's UVD levels and
 * uvd_v4_2_start through set_powergating_state, without it 533/400 MHz and
 * the start; the semaphore timeouts), then the IB test amdgpu runs from
 * late_init and the idle work (the VCPU stopped, UVD DPM off or the clocks
 * to 0). The block functions are frb_uvd_v3_1.c's, which carries amdgpu's
 * uvd_v3_1.c: uvd_v4_2.c is the same file but for LMI_CTRL 0x203108, the
 * MGCG memory mask 0xfff, mc_resume inside start, no firmware validation,
 * and the start/stop driven by set_powergating_state as on UVD 6.
 *
 * RadeonNI decoded H.264 bit-exactly on this card's UVD 4.2 (2026-11-09,
 * RadeonNI's notes: radeon's uvd_v4_2/v1_0 sequence, 24
 * pictures/s at 1080p with its own cik_set_uvd_clocks and no UVD DPM).
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - PLACEMENT. As on SI: UVD before 5.0 addresses 32 bits plus a 4-bit
 *   (ADDR_EXT) and an 8-bit (EXT40) segment, so the VCPU bo, the msg bo, the
 *   ring and everything a decode names sit in VRAM's first 256 MB
 *   (amdgpu_uvd_force_into_uvd_segment; FirebrandVideo places the client's
 *   buffers there, address_64_bit false).
 * - CLOCKS. With CI DPM (on by default) amdgpu_dpm_enable_uvd's arm is
 *   radeon's ci_dpm_powergate_uvd (frb_ci_dpm.c: the UVD boot level, the
 *   enabled-level mask and UVDDPM_Enable/Disable; mclk level 0 held off while
 *   UVD runs), with the block started and stopped around it as
 *   smu7_powergate_uvd does on VI; without it cik_set_uvd_clocks (frb_cik.c).
 * - BYTE ORDER. Ring and IBs little-endian, UVD_LMI_SWAP_CNTL 0 (amdgpu's
 *   __BIG_ENDIAN arm is 0xa with host-order rings; `swapped` takes it).
 * - NO INTERRUPTS: fences are polled. No soft reset.
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
#include "frb_pm.h"
#include "include/asic_reg/uvd/uvd_4_2_d.h"
#include "include/asic_reg/uvd/uvd_4_2_sh_mask.h"
#include "include/asic_reg/oss/oss_2_0_d.h"
#include "include/asic_reg/oss/oss_2_0_sh_mask.h"

#define PACKET0_UVD(reg, n)	((((uint32_t)(n) & 0x3FFF) << 16) | ((reg) & 0xFFFF))	/* cikd.h PACKET0, type 0 */

static inline uint32_t lower_32_bits(uint64_t v) { return (uint32_t)v; }
static inline uint32_t upper_32_bits(uint64_t v) { return (uint32_t)(v >> 32); }

static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

#define amdgpu_ring_write(ring, v)	frb_uvd_ring_write((ring), (v))

static void uvd_v4_2_mc_resume(struct frb_device *adev);

/* --- ring functions --------------------------------------------------------- */

static uint64_t uvd_v4_2_ring_get_rptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_RPTR);
}

static uint64_t uvd_v4_2_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_WPTR);
}

static void uvd_v4_2_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));
}

static void uvd_v4_2_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib, uint32_t vmid, uint32_t flags)
{
	(void)vmid;
	(void)flags;
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_RBC_IB_BASE, 0));
	amdgpu_ring_write(ring, (uint32_t)ib->gpu_addr);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_RBC_IB_SIZE, 0));
	amdgpu_ring_write(ring, ib->length_dw);
}

static void uvd_v4_2_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags)
{
	(void)flags;	/* no 64-bit fences on this ring */
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, (uint32_t)seq);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, (uint32_t)(addr & 0xffffffff));
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, upper_32_bits(addr) & 0xff);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_DATA0, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_DATA1, 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_GPCOM_VCPU_CMD, 0));
	amdgpu_ring_write(ring, 2);
}

static int uvd_v4_2_ring_test_ring(struct frb_ring *ring)
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

	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_CONTEXT_ID, 0));
	amdgpu_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(mmUVD_CONTEXT_ID);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;
	if (r)
		frb_err(adev, "uvd: ring test FAILED: UVD_CONTEXT_ID %08x (rptr %u wptr %u, UVD_STATUS %08x)", tmp,
			RREG32(mmUVD_RBC_RB_RPTR), RREG32(mmUVD_RBC_RB_WPTR), RREG32(mmUVD_STATUS));
	else
		frb_info(adev, "uvd: ring test passed (%u us)", i);
	return r;
}

static void uvd_v4_2_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count / 2; i++) {
		amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_NO_OP, 0));
		amdgpu_ring_write(ring, 0);
	}
}

static void uvd_v4_2_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	frb_uvd_ib_pad(ring->adev, ib);
}

static const struct frb_ring_funcs uvd_v4_2_ring_funcs = {
	.align_mask = 0xf,
	.support_64bit_ptrs = 0,
	.get_rptr = uvd_v4_2_ring_get_rptr,
	.get_wptr = uvd_v4_2_ring_get_wptr,
	.set_wptr = uvd_v4_2_ring_set_wptr,
	.emit_ib = uvd_v4_2_ring_emit_ib,
	.emit_fence = uvd_v4_2_ring_emit_fence,
	.insert_nop = uvd_v4_2_ring_insert_nop,
	.pad_ib = uvd_v4_2_ring_pad_ib,
};

/* --- the block ----------------------------------------------------------------- */

static void uvd_v4_2_set_dcm(struct frb_device *adev, int sw_mode)
{
	uint32_t tmp, tmp2;

	WREG32_FIELD(UVD_CGC_GATE, REGS, 0);

	tmp = RREG32(mmUVD_CGC_CTRL);
	tmp &= ~(UVD_CGC_CTRL__CLK_OFF_DELAY_MASK | UVD_CGC_CTRL__CLK_GATE_DLY_TIMER_MASK);
	tmp |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK |
		(1 << UVD_CGC_CTRL__CLK_GATE_DLY_TIMER__SHIFT) |
		(4 << UVD_CGC_CTRL__CLK_OFF_DELAY__SHIFT);

	if (sw_mode) {
		tmp &= ~0x7ffff800;
		tmp2 = UVD_CGC_CTRL2__DYN_OCLK_RAMP_EN_MASK |
			UVD_CGC_CTRL2__DYN_RCLK_RAMP_EN_MASK |
			(7 << UVD_CGC_CTRL2__GATER_DIV_ID__SHIFT);
	} else {
		tmp |= 0x7ffff800;
		tmp2 = 0;
	}

	WREG32(mmUVD_CGC_CTRL, tmp);
	frb_uvd_ctx_wreg(adev, ixUVD_CGC_CTRL2, tmp2);
}

static void uvd_v4_2_mc_resume(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_gfx_info gfx;
	uint64_t addr;
	uint32_t size;

	/* program the VCPU memory controller bits 0-27 */
	addr = (uvd->inst[0].gpu_addr + AMDGPU_UVD_FIRMWARE_OFFSET) >> 3;
	size = AMDGPU_UVD_FIRMWARE_SIZE(uvd) >> 3;
	WREG32(mmUVD_VCPU_CACHE_OFFSET0, (uint32_t)addr);
	WREG32(mmUVD_VCPU_CACHE_SIZE0, size);

	addr += size;
	size = AMDGPU_UVD_HEAP_SIZE >> 3;
	WREG32(mmUVD_VCPU_CACHE_OFFSET1, (uint32_t)addr);
	WREG32(mmUVD_VCPU_CACHE_SIZE1, size);

	addr += size;
	size = (AMDGPU_UVD_STACK_SIZE +
		(AMDGPU_UVD_SESSION_SIZE * uvd->max_handles)) >> 3;
	WREG32(mmUVD_VCPU_CACHE_OFFSET2, (uint32_t)addr);
	WREG32(mmUVD_VCPU_CACHE_SIZE2, size);

	/* bits 28-31 */
	addr = (uvd->inst[0].gpu_addr >> 28) & 0xF;
	WREG32(mmUVD_LMI_ADDR_EXT, (uint32_t)((addr << 12) | (addr << 0)));

	/* bits 32-39 */
	addr = (uvd->inst[0].gpu_addr >> 32) & 0xFF;
	WREG32(mmUVD_LMI_EXT40_ADDR, (uint32_t)(addr | (0x9 << 16) | (0x1u << 31)));

	frb_gfx_get_info(adev, &gfx);
	WREG32(mmUVD_UDEC_ADDR_CONFIG, gfx.gb_addr_config);
	WREG32(mmUVD_UDEC_DB_ADDR_CONFIG, gfx.gb_addr_config);
	WREG32(mmUVD_UDEC_DBW_ADDR_CONFIG, gfx.gb_addr_config);
}

static int uvd_v4_2_start(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	uint32_t rb_bufsz;
	int i, j, r = 0;
	uint32_t tmp, status = 0;
	/* disable byte swapping (amdgpu's __BIG_ENDIAN arm: 0xa with host-order rings) */
	uint32_t lmi_swap_cntl = uvd->swapped ? 0xa : 0;
	uint32_t mp_swap_cntl = 0;

	/* set uvd busy */
	WREG32_P(mmUVD_STATUS, 1<<2, ~(1<<2));

	uvd_v4_2_set_dcm(adev, 1);
	WREG32(mmUVD_CGC_GATE, 0);

	/* take UVD block out of reset */
	WREG32_P(mmSRBM_SOFT_RESET, 0, ~SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK);
	mdelay(5);

	/* enable VCPU clock */
	WREG32(mmUVD_VCPU_CNTL,  1 << 9);

	/* disable interrupt */
	WREG32_P(mmUVD_MASTINT_EN, 0, ~(1 << 1));

	WREG32(mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);
	WREG32(mmUVD_MP_SWAP_CNTL, mp_swap_cntl);

	/* initialize UVD memory controller */
	WREG32(mmUVD_LMI_CTRL, 0x203108);

	tmp = RREG32(mmUVD_MPC_CNTL);
	WREG32(mmUVD_MPC_CNTL, tmp | 0x10);

	WREG32(mmUVD_MPC_SET_MUXA0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXA1, 0x0);
	WREG32(mmUVD_MPC_SET_MUXB0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXB1, 0x0);
	WREG32(mmUVD_MPC_SET_ALU, 0);
	WREG32(mmUVD_MPC_SET_MUX, 0x88);

	uvd_v4_2_mc_resume(adev);

	tmp = frb_uvd_ctx_rreg(adev, ixUVD_LMI_CACHE_CTRL);
	frb_uvd_ctx_wreg(adev, ixUVD_LMI_CACHE_CTRL, tmp & (~0x10));

	/* enable UMC */
	WREG32_P(mmUVD_LMI_CTRL2, 0, ~(1 << 8));

	WREG32_P(mmUVD_SOFT_RESET, 0, ~UVD_SOFT_RESET__LMI_SOFT_RESET_MASK);

	WREG32_P(mmUVD_SOFT_RESET, 0, ~UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);

	WREG32_P(mmUVD_SOFT_RESET, 0, ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);

	mdelay(10);

	for (i = 0; i < 10; ++i) {
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
		WREG32_P(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK,
				 ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(10);
		WREG32_P(mmUVD_SOFT_RESET, 0, ~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(10);
		r = -1;
	}

	if (r) {
		frb_err(adev, "UVD not responding, giving up!!! (UVD_STATUS %08x)", status);
		return r;
	}

	/* enable interrupt */
	WREG32_P(mmUVD_MASTINT_EN, 3<<1, ~(3 << 1));

	WREG32_P(mmUVD_STATUS, 0, ~(1<<2));

	/* force RBC into idle state */
	WREG32(mmUVD_RBC_RB_CNTL, 0x11010101);

	/* Set the write pointer delay */
	WREG32(mmUVD_RBC_RB_WPTR_CNTL, 0);

	/* Program the 4GB memory segment for rptr and ring buffer */
	WREG32(mmUVD_LMI_EXT40_ADDR, upper_32_bits(ring->gpu_addr) |
		   (0x7 << 16) | (0x1u << 31));

	/* Initialize the ring buffer's read and write pointers */
	WREG32(mmUVD_RBC_RB_RPTR, 0x0);

	ring->wptr = RREG32(mmUVD_RBC_RB_RPTR);
	WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));

	/* set the ring address */
	WREG32(mmUVD_RBC_RB_BASE, (uint32_t)ring->gpu_addr);

	/* Set ring buffer size */
	rb_bufsz = order_base_2(ring->ring_size);
	rb_bufsz = (0x1 << 8) | rb_bufsz;
	WREG32_P(mmUVD_RBC_RB_CNTL, rb_bufsz, ~0x11f1f);

	uvd->inst[0].starts++;
	frb_info(adev, "uvd: VCPU up (start %u): ring %llx, %u bytes, LMI swap %u", uvd->inst[0].starts,
		 (unsigned long long)ring->gpu_addr, ring->ring_size, lmi_swap_cntl);
	return 0;
}

static void uvd_v4_2_stop(struct frb_device *adev)
{
	uint32_t i, j;
	uint32_t status = 0;

	WREG32(mmUVD_RBC_RB_CNTL, 0x11010101);

	for (i = 0; i < 10; ++i) {
		for (j = 0; j < 100; ++j) {
			status = RREG32(mmUVD_STATUS);
			if (status & 2)
				break;
			mdelay(1);
		}
		if (status & 2)
			break;
	}

	for (i = 0; i < 10; ++i) {
		for (j = 0; j < 100; ++j) {
			status = RREG32(mmUVD_LMI_STATUS);
			if (status & 0xf)
				break;
			mdelay(1);
		}
		if (status & 0xf)
			break;
	}

	/* Stall UMC and register bus before resetting VCPU */
	WREG32_P(mmUVD_LMI_CTRL2, 1 << 8, ~(1 << 8));

	for (i = 0; i < 10; ++i) {
		for (j = 0; j < 100; ++j) {
			status = RREG32(mmUVD_LMI_STATUS);
			if (status & 0x240)
				break;
			mdelay(1);
		}
		if (status & 0x240)
			break;
	}

	WREG32_P(0x3D49, 0, ~(1 << 2));

	WREG32_P(mmUVD_VCPU_CNTL, 0, ~(1 << 9));

	/* put LMI, VCPU, RBC etc... into reset */
	WREG32(mmUVD_SOFT_RESET, UVD_SOFT_RESET__LMI_SOFT_RESET_MASK |
		UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK |
		UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);

	WREG32(mmUVD_STATUS, 0);

	uvd_v4_2_set_dcm(adev, 0);
}

static void uvd_v4_2_enable_mgcg(struct frb_device *adev, int enable)
{
	uint32_t orig, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_UVD_MGCG)) {
		data = frb_uvd_ctx_rreg(adev, ixUVD_CGC_MEM_CTRL);
		data |= 0xfff;
		frb_uvd_ctx_wreg(adev, ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	} else {
		data = frb_uvd_ctx_rreg(adev, ixUVD_CGC_MEM_CTRL);
		data &= ~0xfff;
		frb_uvd_ctx_wreg(adev, ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data &= ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	}
}


/* uvd_v4_2_set_clockgating_state: empty in amdgpu */
int frb_uvd_v4_2_set_clockgating_state(struct frb_device *adev, int gate)
{
	(void)adev;
	(void)gate;
	return 0;
}

/*
 * uvd_v4_2_set_powergating_state: gate nonzero = AMD_PG_STATE_GATE. "This
 * doesn't actually powergate the UVD block. That's done in the dpm code via
 * the SMC." Hawaii's pg_flags are 0, so no PGFSM: stop or start.
 */
int frb_uvd_v4_2_set_powergating_state(struct frb_device *adev, int gate)
{
	if (gate) {
		uvd_v4_2_stop(adev);
		return 0;
	}
	return uvd_v4_2_start(adev);
}

/* uvd_v4_2_sw_init: amdgpu_uvd_sw_init (the firmware, the VCPU and msg bos), the ring, amdgpu_uvd_resume */
static int uvd_v4_2_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	int r;

	r = frb_uvd_sw_init(adev);
	if (r)
		return r;
	if (!uvd->inst[0].ring.funcs) {
		r = frb_ring_init(adev, &uvd->inst[0].ring, "uvd", &uvd_v4_2_ring_funcs, 512);
		if (r)
			return r;
	}
	return frb_uvd_resume(adev);
}

static int uvd_v4_2_hw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	uint32_t tmp;
	int r;

	uvd->touched = 1;
	uvd_v4_2_enable_mgcg(adev, 1);
	/* amdgpu_asic_set_uvd_clocks(10000, 10000), DPM or not, as amdgpu */
	r = frb_cik_set_uvd_clocks(adev, 10000, 10000);
	if (r)
		frb_err(adev, "uvd: cik_set_uvd_clocks(100, 100 MHz) failed (%d)", r);

	/* amdgpu_ring_test_helper (its ring_alloc is begin_use: the block started) */
	r = uvd_v4_2_ring_test_ring(ring);
	ring->ready = !r;
	if (r) {
		frb_err(adev, "UVD ring test failed (%d).", r);
		goto done;
	}

	frb_uvd_ring_begin_use(ring);
	r = frb_ring_alloc(ring, 10);
	if (r) {
		frb_err(adev, "ring alloc failed (%d).", r);
		goto done;
	}

	tmp = PACKET0_UVD(mmUVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0_UVD(mmUVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	tmp = PACKET0_UVD(mmUVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL, 0);
	amdgpu_ring_write(ring, tmp);
	amdgpu_ring_write(ring, 0xFFFFF);

	/* Clear timeout status bits */
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_SEMA_TIMEOUT_STATUS, 0));
	amdgpu_ring_write(ring, 0x8);

	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_SEMA_CNTL, 0));
	amdgpu_ring_write(ring, 3);

	frb_ring_commit(ring);

done:
	if (!r)
		frb_info(adev, "UVD initialized successfully.");

	return r;
}

static int uvd_v4_2_hw_fini(struct frb_device *adev)
{
	if (RREG32(mmUVD_STATUS) != 0)
		uvd_v4_2_stop(adev);
	return 0;
}

/* --- the stage, the tests by hand, the teardown ------------------------------------ */

int frb_uvd_v4_2_init(struct frb_device *adev)
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
	/* uvd_v4_2_early_init: one instance */
	uvd->num_uvd_inst = 1;
	uvd->harvest_config = 0;
	r = uvd_v4_2_sw_init(adev);
	if (r)
		return r;
	r = uvd_v4_2_hw_init(adev);
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

/* FRB_TEST_SWAP_PROBE: UVD 6's, the VCPU restarted under each LMI swap setting */
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
		rr[mode] = uvd_v4_2_ring_test_ring(ring);
		ring->ready = !rr[mode];
		ri[mode] = rr[mode] ? -1 : frb_uvd_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		frb_info(adev, "uvd: swap probe: %s ring and IBs, UVD_LMI_SWAP_CNTL %s: ring test %d, IB test %d",
			 mode ? "host-order" : "little-endian", mode ? "0xa" : "0", rr[mode], ri[mode]);
		if (rr[mode] || ri[mode]) {
			uvd_v4_2_stop(adev);
			frb_fence_driver_force_completion(ring);
			uvd->powered = 0;
		}
	}
	r = frb_uvd_idle_work_handler(adev);
	uvd->swapped = 0;
	ring->ready = 1;
	if (!r)
		r = uvd_v4_2_ring_test_ring(ring);
	ring->ready = !r;
	if (!r)
		r = frb_uvd_idle_work_handler(adev);
	return r ? r : (rr[0] || ri[0] ? -EIO : 0);
}

int frb_uvd_v4_2_test(struct frb_device *adev, int test)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	int r;

	if (!uvd || !uvd->inst[0].ring.funcs)
		return -ENODEV;
	ring = &uvd->inst[0].ring;
	switch (test) {
	case FRB_TEST_RING:
		r = uvd_v4_2_ring_test_ring(ring);
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

/* uvd_v4_2_hw_fini at unload, before the driver lets go of VRAM */
void frb_uvd_v4_2_fini(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;

	if (!uvd)
		return;
	if (uvd->touched) {
		if (frb_pm_enabled(adev))
			frb_pm_enable_uvd(adev, 0);
		uvd_v4_2_hw_fini(adev);
		frb_info(adev, "uvd: VCPU stopped (hw_fini)");
	}
	adev->os->free(adev->os->cookie, uvd);
	adev->uvd = NULL;
}
