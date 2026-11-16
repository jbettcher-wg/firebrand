/*
 * uvd_v3_1.c (Linux 7.0.2), for Southern Islands (the R7 450, Cape Verde):
 * the decode ring. Stage uvd on SI runs what amdgpu does for this block at
 * load: sw_init (amdgpu_uvd_sw_init: firmware and VCPU bo; the ring;
 * amdgpu_uvd_resume; the firmware's validation key), hw_init (mc_resume,
 * MGCG, the UVD clocks, fw_validate, uvd_v3_1_start, the ring test, the
 * semaphore timeouts), then the IB test amdgpu runs from late_init and the
 * idle work (the UVD clocks back to bypass).
 *
 * UVD 3.1 never booted under RadeonNI (its VCPU never answered on this
 * card); amdgpu's uvd_v3_1 differs from radeon's uvd_v3_1/v2_2 start by the
 * firmware validation (UVD_FW_START with the key the image carries, then
 * UVD_FW_STATUS DONE and PASS) and by setting the UVD clocks before it
 * (si_set_uvd_clocks: the UPLL out of bypass at 533/400 MHz, frb_si.c).
 * card-support-fold-scope.md names fw_validate as the likely missing step.
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - PLACEMENT. The VCPU bo and the msg bo are in the layout's UVD region and
 *   the ring and writeback in the objects region (frb_ring.c), all in VRAM's
 *   first 256 MB: UVD 3.1 addresses 32 bits plus a 4-bit (ADDR_EXT) and an
 *   8-bit (EXT40) segment, so everything it fetches shares one segment, as
 *   amdgpu_uvd_force_into_uvd_segment arranges for the msg bo.
 * - CLOCKS. As amdgpu: with SI DPM (on by default) amdgpu_dpm_enable_uvd's SI
 *   arm before fw_validate and in begin_use, the VBIOS's UVD power state
 *   (frb_si_dpm_enable_uvd: its VCLK/DCLK through the UPLL, the engine and
 *   memory clocks held), and the performance state again in the idle work;
 *   without DPM amdgpu_asic_set_uvd_clocks(53300, 40000) and (0, 0):
 *   frb_si_set_uvd_clocks.
 * - THE DECODE CLIENT (kext/Firebrand/FirebrandVideo.cpp): UVD 3.1 takes
 *   28-bit addresses in the 256 MB segment UVD_LMI_ADDR_EXT names per
 *   client; every buffer a decode names sits in the VCPU bo's segment, as
 *   Linux places them (amdgpu_uvd_force_into_uvd_segment).
 * - BYTE ORDER. Ring and IBs little-endian, UVD_LMI_SWAP_CNTL 0 (amdgpu's
 *   __BIG_ENDIAN arm is 0xa with host-order rings); `swapped` takes amdgpu's
 *   arm (FRB_TEST_SWAP_PROBE runs both), as on UVD 6.
 * - NO INTERRUPTS: fences are polled.
 *
 * On the R7 450 since 2026-11-15 (docs/si-bring-up.md, "UVD 3.1").
 *
 * Copyright 2020 Advanced Micro Devices, Inc.
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
 * Authors: Sonny Jiang <sonny.jiang@amd.com>
 */
#include "frb_uvd.h"
#include "frb_pm.h"
#include "include/sid.h"
#include "include/asic_reg/uvd/uvd_3_1_d.h"
#include "include/asic_reg/uvd/uvd_3_1_sh_mask.h"
#include "include/asic_reg/oss/oss_1_0_d.h"
#include "include/asic_reg/oss/oss_1_0_sh_mask.h"

#define PACKET0_UVD(reg, n)	((((uint32_t)(n) & 0x3FFF) << 16) | ((reg) & 0xFFFF))	/* sid.h PACKET0, type 0 */

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

/* --- ring functions --------------------------------------------------------- */

static uint64_t uvd_v3_1_ring_get_rptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_RPTR);
}

static uint64_t uvd_v3_1_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmUVD_RBC_RB_WPTR);
}

static void uvd_v3_1_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));
}

static void uvd_v3_1_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib, uint32_t vmid, uint32_t flags)
{
	(void)vmid;
	(void)flags;
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_RBC_IB_BASE, 0));
	amdgpu_ring_write(ring, (uint32_t)ib->gpu_addr);
	amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_RBC_IB_SIZE, 0));
	amdgpu_ring_write(ring, ib->length_dw);
}

static void uvd_v3_1_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags)
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

static int uvd_v3_1_ring_test_ring(struct frb_ring *ring)
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

static void uvd_v3_1_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count / 2; i++) {
		amdgpu_ring_write(ring, PACKET0_UVD(mmUVD_NO_OP, 0));
		amdgpu_ring_write(ring, 0);
	}
}

static void uvd_v3_1_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	frb_uvd_ib_pad(ring->adev, ib);
}

static const struct frb_ring_funcs uvd_v3_1_ring_funcs = {
	.align_mask = 0xf,
	.support_64bit_ptrs = 0,
	.get_rptr = uvd_v3_1_ring_get_rptr,
	.get_wptr = uvd_v3_1_ring_get_wptr,
	.set_wptr = uvd_v3_1_ring_set_wptr,
	.emit_ib = uvd_v3_1_ring_emit_ib,
	.emit_fence = uvd_v3_1_ring_emit_fence,
	.insert_nop = uvd_v3_1_ring_insert_nop,
	.pad_ib = uvd_v3_1_ring_pad_ib,
};

/* --- the block ----------------------------------------------------------------- */

static void uvd_v3_1_set_dcm(struct frb_device *adev, int sw_mode)
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

static void uvd_v3_1_mc_resume(struct frb_device *adev)
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

static int uvd_v3_1_fw_validate(struct frb_device *adev)
{
	int i;
	uint32_t keysel = adev->uvd->keyselect;

	WREG32(mmUVD_FW_START, keysel);

	for (i = 0; i < 10; ++i) {
		mdelay(10);
		if (RREG32(mmUVD_FW_STATUS) & UVD_FW_STATUS__DONE_MASK)
			break;
	}

	if (i == 10)
		return -ETIMEDOUT;

	if (!(RREG32(mmUVD_FW_STATUS) & UVD_FW_STATUS__PASS_MASK))
		return -EINVAL;

	for (i = 0; i < 10; ++i) {
		mdelay(10);
		if (!(RREG32(mmUVD_FW_STATUS) & UVD_FW_STATUS__BUSY_MASK))
			break;
	}

	if (i == 10)
		return -ETIMEDOUT;

	return 0;
}

static int uvd_v3_1_start(struct frb_device *adev)
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

	uvd_v3_1_set_dcm(adev, 1);
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
	WREG32(mmUVD_LMI_CTRL, 0x40 | (1 << 8) | (1 << 13) |
		(1 << 21) | (1 << 9) | (1 << 20));

	tmp = RREG32(mmUVD_MPC_CNTL);
	WREG32(mmUVD_MPC_CNTL, tmp | 0x10);

	WREG32(mmUVD_MPC_SET_MUXA0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXA1, 0x0);
	WREG32(mmUVD_MPC_SET_MUXB0, 0x40c2040);
	WREG32(mmUVD_MPC_SET_MUXB1, 0x0);
	WREG32(mmUVD_MPC_SET_ALU, 0);
	WREG32(mmUVD_MPC_SET_MUX, 0x88);

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

static void uvd_v3_1_stop(struct frb_device *adev)
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

	uvd_v3_1_set_dcm(adev, 0);
}

static void uvd_v3_1_enable_mgcg(struct frb_device *adev, int enable)
{
	uint32_t orig, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_UVD_MGCG)) {
		data = frb_uvd_ctx_rreg(adev, ixUVD_CGC_MEM_CTRL);
		data |= 0x3fff;
		frb_uvd_ctx_wreg(adev, ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data |= UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	} else {
		data = frb_uvd_ctx_rreg(adev, ixUVD_CGC_MEM_CTRL);
		data &= ~0x3fff;
		frb_uvd_ctx_wreg(adev, ixUVD_CGC_MEM_CTRL, data);

		orig = data = RREG32(mmUVD_CGC_CTRL);
		data &= ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK;
		if (orig != data)
			WREG32(mmUVD_CGC_CTRL, data);
	}
}

/*
 * uvd_v3_1_sw_init: amdgpu_uvd_sw_init (the firmware, the VCPU and msg bos),
 * the ring, amdgpu_uvd_resume, and the firmware validation key: at byte
 * 192 + 16 of the image a length, and the key that length further on, both
 * little-endian (amdgpu's memcpy on a little-endian host).
 */
static int uvd_v3_1_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	const uint8_t *img;
	uint32_t ucode_len;
	int r;

	r = frb_uvd_sw_init(adev);
	if (r)
		return r;
	if (!uvd->inst[0].ring.funcs) {
		r = frb_ring_init(adev, &uvd->inst[0].ring, "uvd", &uvd_v3_1_ring_funcs, 512);
		if (r)
			return r;
	}
	r = frb_uvd_resume(adev);
	if (r)
		return r;

	img = uvd->fw.data + uvd->ucode.ucode_array_offset;
	if (uvd->ucode.ucode_size_bytes < 192 + 16 + 4)
		return -EINVAL;
	ucode_len = frb_get_le32(img, 192 + 16);
	if (ucode_len > uvd->ucode.ucode_size_bytes - (192 + 16) - 4)
		return -EINVAL;
	uvd->keyselect = frb_get_le32(img, 192 + 16 + ucode_len);
	frb_info(adev, "uvd: firmware validation key 0x%08x (at +%u)", uvd->keyselect, 192 + 16 + ucode_len);
	return 0;
}

static int uvd_v3_1_hw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring = &uvd->inst[0].ring;
	uint32_t tmp;
	int r;

	uvd->touched = 1;
	uvd_v3_1_mc_resume(adev);
	uvd_v3_1_enable_mgcg(adev, 1);

	/*
	 * Make sure UVD is powered during FW validation.
	 * It's going to be automatically powered off after the ring test.
	 */
	if (frb_pm_enabled(adev))
		frb_pm_enable_uvd(adev, 1);
	else
		frb_si_set_uvd_clocks(adev, 53300, 40000);
	uvd->powered = 1;

	r = uvd_v3_1_fw_validate(adev);
	if (r) {
		frb_err(adev, "UVD Firmware validate fail (%d): UVD_FW_STATUS %08x, key 0x%08x.", r,
			RREG32(mmUVD_FW_STATUS), uvd->keyselect);
		return r;
	}
	frb_info(adev, "uvd: firmware validated (UVD_FW_STATUS %08x)", RREG32(mmUVD_FW_STATUS));

	r = uvd_v3_1_start(adev);
	if (r)
		return r;

	/* amdgpu_ring_test_helper */
	r = uvd_v3_1_ring_test_ring(ring);
	ring->ready = !r;
	if (r) {
		frb_err(adev, "UVD ring test failed (%d).", r);
		goto done;
	}

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

static int uvd_v3_1_hw_fini(struct frb_device *adev)
{
	if (RREG32(mmUVD_STATUS) != 0)
		uvd_v3_1_stop(adev);
	return 0;
}

/* --- the stage, the tests by hand, the teardown ------------------------------------ */

int frb_uvd_v3_1_init(struct frb_device *adev)
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
	/* uvd_v3_1_early_init: one instance, no harvest check on SI (amdgpu_uvd's inst[0]) */
	uvd->num_uvd_inst = 1;
	uvd->harvest_config = 0;
	r = uvd_v3_1_sw_init(adev);
	if (r)
		return r;
	r = uvd_v3_1_hw_init(adev);
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

int frb_uvd_v3_1_test(struct frb_device *adev, int test)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	int r;

	if (!uvd || !uvd->inst[0].ring.funcs)
		return -ENODEV;
	ring = &uvd->inst[0].ring;
	switch (test) {
	case FRB_TEST_RING:
		r = uvd_v3_1_ring_test_ring(ring);
		ring->ready = !r;
		return r;
	case FRB_TEST_IB:
		if (!ring->ready)
			return -ENODEV;
		return frb_uvd_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_IDLE:
		return frb_uvd_idle_work_handler(adev);
	default:
		return -EINVAL;
	}
}

void frb_uvd_v3_1_fini(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;

	if (!uvd)
		return;
	if (uvd->touched) {
		uvd_v3_1_hw_fini(adev);
		frb_si_set_uvd_clocks(adev, 0, 0);
		frb_info(adev, "uvd: VCPU stopped (hw_fini)");
	}
	adev->os->free(adev->os->cookie, uvd);
	adev->uvd = NULL;
}
