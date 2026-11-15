/*
 * uvd_v7_0.c (Linux 7.1.1), for Vega 20 (UVD 7.2, two instances): the
 * decode rings. Stage uvd runs what amdgpu does for this block at load:
 * early_init (both instances' harvest fuses), sw_init (amdgpu_uvd_sw_init:
 * the firmware header and each instance's VCPU bo; the rings;
 * amdgpu_uvd_resume, which only zeroes the bos: the PSP loaded the image
 * for UVD and UVD1 into its TMR in stage smu), hw_init (uvd_v7_0_start for
 * every instance, then per instance the ring test, whose ring_alloc is
 * begin_use: with powerplay the SMU's DPM_UVD feature on, and the semaphore
 * timeouts), then the IB tests amdgpu runs from late_init
 * (amdgpu_uvd_ring_test_ib on uvd_0 and uvd_1) and the idle work amdgpu
 * runs a second later (DPM_UVD off; the VCPUs stay up: uvd_v7_0 has no
 * power gating). The rings are in VM mode (uvd_v7_0_ring_vm_funcs), every
 * IB in VMID 0. The video sessions decode on the first instance present.
 *
 * DEVIATIONS from amdgpu, each with its register-level consequence:
 * - NO ENCODE RINGS. uvd_v7_0_start sets up two encode rings per instance
 *   (UVD_RB_RPTR/WPTR/BASE_LO/BASE_HI/SIZE and their 2 twins) and hw_init
 *   tests them; they are out of Firebrand's scope: none of those writes
 *   (the registers keep their reset values: empty rings at 0).
 * - NO VCE. vce_v4_0 follows this block on Linux; it is not ported.
 * - PLACEMENT. The rings and their writeback are in VRAM (frb_ring.c), the
 *   VCPU bos in the layout's UVD region (frb_uvd.c): RBC_RB_64BIT_BAR and
 *   the VCPU_CACHE1/2_64BIT_BARs carry other addresses, and
 *   RBC_RB_RPTR_ADDR (the ring's upper address bits >> 2) is 0x20 for VRAM
 *   where Linux's GTT ring gives 0. The firmware's address
 *   (VCPU_CACHE_64BIT_BAR) is what the PSP answered LOAD_IP_FW with
 *   (resp.fw_addr), inside Firebrand's TMR. frb_ring_commit flushes the HDP
 *   by MMIO before every WPTR write (a VRAM ring); Linux's GTT ring needs
 *   none (its emit_hdp_flush is empty too).
 * - BYTE ORDER. Under __BIG_ENDIAN amdgpu sets UVD_LMI_SWAP_CNTL 0xa
 *   (swap 8 in 32 for RB and IB fetches) and writes the ring and IBs in
 *   the CPU's order. Firebrand's default writes them little-endian and
 *   leaves the swap 0, as everything else it hands the engines (the
 *   little-endian capture's value); `swapped` takes amdgpu's arm instead
 *   (FRB_TEST_SWAP_PROBE runs both). Messages are little-endian either way.
 * - NO INTERRUPTS. UVD_MASTINT_EN gets VCPU_EN|SYS_EN as on Linux, but no
 *   IH ring exists to take the trap; fences are polled.
 * - The IB tests and the idle work run at the end of the stage, not from
 *   late_init and a delayed work item.
 * - ONE INSTANCE, when asked (FirebrandUVDOneInstance / frb_uvd_one=1):
 *   instance 1 is treated as its harvest fuse would have it, so none of its
 *   registers is written. Off by default: both start, as on Linux.
 * - No SR-IOV arm (mmsch), no suspend/resume.
 *
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include "frb_uvd.h"
#include "frb_psp.h"
#include "frb_soc15.h"
#include "include/asic_reg/uvd/uvd_7_0_offset.h"
#include "include/asic_reg/uvd/uvd_7_0_sh_mask.h"

#define mmUVD_PG0_CC_UVD_HARVESTING                                                                    0x00c7
#define mmUVD_PG0_CC_UVD_HARVESTING_BASE_IDX                                                           1
/* UVD_PG0_CC_UVD_HARVESTING */
#define UVD_PG0_CC_UVD_HARVESTING__UVD_DISABLE__SHIFT                                                         0x1
#define UVD_PG0_CC_UVD_HARVESTING__UVD_DISABLE_MASK                                                           0x00000002L

#define UVD7_MAX_HW_INSTANCES_VEGA20			2

#define PACKET0(reg, n)		((((uint32_t)(n) & 0x3FFF) << 16) | ((reg) & 0xFFFF))	/* soc15d.h, type 0 */

/* include/linux/log2.h order_base_2: log2, rounded up */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

static int uvd_v7_0_start(struct frb_device *adev);
static void uvd_v7_0_stop(struct frb_device *adev);

#define amdgpu_ring_write(ring, v)	frb_uvd_ring_write((ring), (v))

/* --- ring functions --------------------------------------------------------- */

/**
 * uvd_v7_0_ring_get_rptr - get read pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware read pointer
 */
static uint64_t uvd_v7_0_ring_get_rptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32_SOC15(UVD, ring->me, mmUVD_RBC_RB_RPTR);
}

/**
 * uvd_v7_0_ring_get_wptr - get write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Returns the current hardware write pointer
 */
static uint64_t uvd_v7_0_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32_SOC15(UVD, ring->me, mmUVD_RBC_RB_WPTR);
}

/**
 * uvd_v7_0_ring_set_wptr - set write pointer
 *
 * @ring: amdgpu_ring pointer
 *
 * Commits the write pointer to the hardware
 */
static void uvd_v7_0_ring_set_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32_SOC15(UVD, ring->me, mmUVD_RBC_RB_WPTR, lower_32_bits(ring->wptr));
}

/**
 * uvd_v7_0_ring_emit_fence - emit an fence & trap command
 *
 * @ring: amdgpu_ring pointer
 * @addr: address
 * @seq: sequence number
 * @flags: fence related flags
 *
 * Write a fence and a trap command to the ring.
 */
static void uvd_v7_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq,
				     unsigned flags)
{
	struct frb_device *adev = ring->adev;

	(void)flags;	/* WARN_ON(flags & AMDGPU_FENCE_FLAG_64BIT) */

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_CONTEXT_ID), 0));
	amdgpu_ring_write(ring, (uint32_t)seq);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA0), 0));
	amdgpu_ring_write(ring, addr & 0xffffffff);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA1), 0));
	amdgpu_ring_write(ring, upper_32_bits(addr) & 0xff);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_CMD), 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA0), 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA1), 0));
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_CMD), 0));
	amdgpu_ring_write(ring, 2);
}

/**
 * uvd_v7_0_ring_emit_hdp_flush - skip HDP flushing
 *
 * @ring: amdgpu_ring pointer
 */
static void uvd_v7_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	/* The firmware doesn't seem to like touching registers at this point. */
	(void)ring;
}

/**
 * uvd_v7_0_ring_test_ring - register write test
 *
 * @ring: amdgpu_ring pointer
 *
 * Test if we can successfully write to the context register
 */
static int uvd_v7_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32_SOC15(UVD, ring->me, mmUVD_CONTEXT_ID, 0xCAFEDEAD);
	/* amdgpu_ring_alloc: begin_use first */
	frb_uvd_ring_begin_use(ring);
	r = frb_ring_alloc(ring, 3);
	if (r)
		return r;

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_CONTEXT_ID), 0));
	amdgpu_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32_SOC15(UVD, ring->me, mmUVD_CONTEXT_ID);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout) {
		frb_err(adev, "%s: ring test failed: UVD_CONTEXT_ID 0x%08x, RBC_RB_RPTR 0x%llx, WPTR 0x%llx, "
			"UVD_STATUS 0x%08x", ring->name, tmp, (unsigned long long)uvd_v7_0_ring_get_rptr(ring),
			(unsigned long long)ring->wptr, RREG32_SOC15(UVD, ring->me, mmUVD_STATUS));
		r = -ETIMEDOUT;
	} else {
		frb_info(adev, "%s: ring test passed (%u us)", ring->name, i);
	}

	return r;
}

/**
 * uvd_v7_0_ring_emit_ib - execute indirect buffer
 *
 * @ring: amdgpu_ring pointer
 * @ib: indirect buffer to execute
 * @vmid: the job's VMID (0: the kernel's)
 * @flags: unused
 *
 * Write ring commands to execute the indirect buffer
 */
static void uvd_v7_0_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib,
				  uint32_t vmid, uint32_t flags)
{
	struct frb_device *adev = ring->adev;

	(void)flags;

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_LMI_RBC_IB_VMID), 0));
	amdgpu_ring_write(ring, vmid);

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_LMI_RBC_IB_64BIT_BAR_LOW), 0));
	amdgpu_ring_write(ring, lower_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH), 0));
	amdgpu_ring_write(ring, upper_32_bits(ib->gpu_addr));
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_RBC_IB_SIZE), 0));
	amdgpu_ring_write(ring, ib->length_dw);
}

static void uvd_v7_0_ring_emit_wreg(struct frb_ring *ring,
				    uint32_t reg, uint32_t val)
{
	struct frb_device *adev = ring->adev;

	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA0), 0));
	amdgpu_ring_write(ring, reg << 2);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_DATA1), 0));
	amdgpu_ring_write(ring, val);
	amdgpu_ring_write(ring,
		PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_GPCOM_VCPU_CMD), 0));
	amdgpu_ring_write(ring, 8);
}

static void uvd_v7_0_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	struct frb_device *adev = ring->adev;
	uint32_t i;

	/* WARN_ON(ring->wptr % 2 || count % 2) */
	if ((ring->wptr & 1) || (count & 1))
		frb_err(adev, "%s: odd NOP fill (wptr %llu, %u dwords)", ring->name,
			(unsigned long long)ring->wptr, count);

	for (i = 0; i < count / 2; i++) {
		amdgpu_ring_write(ring, PACKET0(SOC15_REG_OFFSET(UVD, ring->me, mmUVD_NO_OP), 0));
		amdgpu_ring_write(ring, 0);
	}
}

/* amdgpu_ring_generic_pad_ib on Linux (ring->funcs->nop is 0 there); the kernel's msg IBs are 16 dwords already */
static void uvd_v7_0_ring_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	frb_uvd_ib_pad(ring->adev, ib);
}

/*
 * uvd_v7_0_ring_vm_funcs. Not carried: patch_cs_in_place (user IBs for the
 * second instance: the sessions build theirs for the instance they run on),
 * emit_vm_flush and emit_reg_wait (VMID 0 only).
 */
static const struct frb_ring_funcs uvd_v7_0_ring_vm_funcs = {
	.align_mask = 0xf,
	.support_64bit_ptrs = 0,
	.get_rptr = uvd_v7_0_ring_get_rptr,
	.get_wptr = uvd_v7_0_ring_get_wptr,
	.set_wptr = uvd_v7_0_ring_set_wptr,
	.emit_ib = uvd_v7_0_ring_emit_ib,
	.emit_fence = uvd_v7_0_ring_emit_fence,
	.emit_hdp_flush = uvd_v7_0_ring_emit_hdp_flush,
	.insert_nop = uvd_v7_0_ring_insert_nop,
	.pad_ib = uvd_v7_0_ring_pad_ib,
	.emit_wreg = uvd_v7_0_ring_emit_wreg,
};

/* --- early/sw/hw init ------------------------------------------------------- */

static int uvd_v7_0_early_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;

	uvd->harvest_config = 0;
	if (adev->asic_type == CHIP_VEGA20) {
		uint32_t harvest;
		uint32_t i;

		uvd->num_uvd_inst = UVD7_MAX_HW_INSTANCES_VEGA20;
		for (i = 0; i < uvd->num_uvd_inst; i++) {
			harvest = RREG32_SOC15(UVD, i, mmUVD_PG0_CC_UVD_HARVESTING);
			if (harvest & UVD_PG0_CC_UVD_HARVESTING__UVD_DISABLE_MASK) {
				uvd->harvest_config |= 1 << i;
			}
		}
		if (uvd->harvest_config == (AMDGPU_UVD_HARVEST_UVD0 |
					    AMDGPU_UVD_HARVEST_UVD1))
			/* both instances are harvested, disable the block */
			return -ENOENT;
		/* not amdgpu: one instance on request, the second left as a harvested one is */
		if (adev->uvd_one_inst && !uvd->harvest_config) {
			uvd->harvest_config = AMDGPU_UVD_HARVEST_UVD1;
			frb_info(adev, "UVD(1) is left alone (FirebrandUVDOneInstance / frb_uvd_one=1)");
		}
	} else {
		uvd->num_uvd_inst = 1;
	}

	/* uvd_v7_0_set_ring_funcs: VM mode; no enc rings, no IRQ funcs here */
	{
		uint32_t i;

		for (i = 0; i < uvd->num_uvd_inst; i++) {
			if (uvd->harvest_config & (1 << i))
				continue;
			frb_info(adev, "UVD(%u) is enabled in VM mode", i);
		}
	}
	return 0;
}

/* adev->firmware.ucode[AMDGPU_UCODE_ID_UVD / _UVD1].tmr_mc_addr: psp_cmd_submit_buf kept resp.fw_addr */
static int uvd_v7_0_tmr_addr(struct frb_device *adev, uint32_t inst)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_psp *psp = adev->psp;
	uint32_t type = inst ? GFX_FW_TYPE_UVD1 : GFX_FW_TYPE_UVD, i;

	if (!psp)
		return -ENODEV;
	for (i = 0; i < psp->num_images && i < psp->loaded; i++) {
		if (psp->img[i].fw_type != type)
			continue;
		uvd->inst[inst].tmr_mc_addr_lo = psp->img[i].tmr_lo;
		uvd->inst[inst].tmr_mc_addr_hi = psp->img[i].tmr_hi;
		if (!psp->img[i].tmr_lo && !psp->img[i].tmr_hi)
			break;
		return 0;
	}
	frb_err(adev, "uvd: the PSP has no %s image loaded (no TMR address for it)", inst ? "UVD1" : "UVD");
	return -ENOENT;
}

static int uvd_v7_0_sw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	uint32_t j;
	int r;

	/* no UVD TRAP / UVD ENC TRAP sources: no IH ring */
	r = frb_uvd_sw_init(adev);
	if (r)
		return r;

	/* AMDGPU_FW_LOAD_PSP: Linux queues the image for UVD and UVD1 here; stage smu's PSP loaded both */
	for (j = 0; j < uvd->num_uvd_inst; j++) {
		if (uvd->harvest_config & (1 << j))
			continue;
		r = uvd_v7_0_tmr_addr(adev, j);
		if (r)
			return r;
	}

	for (j = 0; j < uvd->num_uvd_inst; j++) {
		char name[16];

		if (uvd->harvest_config & (1 << j))
			continue;
		ring = &uvd->inst[j].ring;
		if (!ring->funcs) {
			name[0] = 'u'; name[1] = 'v'; name[2] = 'd'; name[3] = '_';
			name[4] = (char)('0' + j); name[5] = 0;
			r = frb_ring_init(adev, ring, name, &uvd_v7_0_ring_vm_funcs, 512);
			if (r)
				return r;
			ring->me = (int)j;
		}
		/* the encode rings (uvd_enc_%d.%d) are not ported */
	}

	return frb_uvd_resume(adev);
}

/**
 * uvd_v7_0_hw_init - start and test UVD block
 *
 * Initialize the hardware, boot up the VCPU and do some testing
 */
static int uvd_v7_0_hw_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	uint32_t tmp;
	uint32_t j;
	int r;

	uvd->touched = 1;
	r = uvd_v7_0_start(adev);
	if (r)
		goto done;

	for (j = 0; j < uvd->num_uvd_inst; ++j) {
		if (uvd->harvest_config & (1 << j))
			continue;
		ring = &uvd->inst[j].ring;

		/* amdgpu_ring_test_helper */
		r = uvd_v7_0_ring_test_ring(ring);
		ring->ready = !r;
		if (r)
			goto done;

		frb_uvd_ring_begin_use(ring);
		r = frb_ring_alloc(ring, 10);
		if (r) {
			frb_err(adev, "ring alloc failed (%d).", r);
			goto done;
		}

		tmp = PACKET0(SOC15_REG_OFFSET(UVD, j,
			mmUVD_SEMA_WAIT_FAULT_TIMEOUT_CNTL), 0);
		amdgpu_ring_write(ring, tmp);
		amdgpu_ring_write(ring, 0xFFFFF);

		tmp = PACKET0(SOC15_REG_OFFSET(UVD, j,
			mmUVD_SEMA_WAIT_INCOMPLETE_TIMEOUT_CNTL), 0);
		amdgpu_ring_write(ring, tmp);
		amdgpu_ring_write(ring, 0xFFFFF);

		tmp = PACKET0(SOC15_REG_OFFSET(UVD, j,
			mmUVD_SEMA_SIGNAL_INCOMPLETE_TIMEOUT_CNTL), 0);
		amdgpu_ring_write(ring, tmp);
		amdgpu_ring_write(ring, 0xFFFFF);

		/* Clear timeout status bits */
		amdgpu_ring_write(ring, PACKET0(SOC15_REG_OFFSET(UVD, j,
			mmUVD_SEMA_TIMEOUT_STATUS), 0));
		amdgpu_ring_write(ring, 0x8);

		amdgpu_ring_write(ring, PACKET0(SOC15_REG_OFFSET(UVD, j,
			mmUVD_SEMA_CNTL), 0));
		amdgpu_ring_write(ring, 3);

		frb_ring_commit(ring);

		/* the encode rings' tests: not ported */
	}
done:
	if (!r)
		frb_info(adev, "UVD initialized successfully.");

	return r;
}

/**
 * uvd_v7_0_hw_fini - stop the hardware block
 *
 * Stop the UVD block, mark ring as not ready any more
 */
static int uvd_v7_0_hw_fini(struct frb_device *adev)
{
	/* cancel_delayed_work_sync(&adev->uvd.idle_work): no delayed work here */
	uvd_v7_0_stop(adev);

	return 0;
}

/**
 * uvd_v7_0_mc_resume - memory controller programming
 *
 * Let the UVD memory controller know it's offsets
 */
static void uvd_v7_0_mc_resume(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t size = AMDGPU_UVD_FIRMWARE_SIZE(uvd);
	struct frb_gfx_info gfx;
	uint32_t gb_addr_config;
	uint32_t offset;
	uint32_t i;

	/* adev->gfx.config.gb_addr_config: gfx_v9_0's (stage gfx), else Vega 20's golden value */
	gb_addr_config = frb_gfx_v9_0_get_info(adev, &gfx) ? 0x2a114042 : gfx.gb_addr_config;

	for (i = 0; i < uvd->num_uvd_inst; ++i) {
		if (uvd->harvest_config & (1 << i))
			continue;
		if (uvd->psp_load) {
			WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
				uvd->inst[i].tmr_mc_addr_lo);
			WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
				uvd->inst[i].tmr_mc_addr_hi);
			WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_OFFSET0, 0);
			offset = 0;
		} else {
			WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW,
				lower_32_bits(uvd->inst[i].gpu_addr));
			WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH,
				upper_32_bits(uvd->inst[i].gpu_addr));
			offset = size;
			WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_OFFSET0,
					AMDGPU_UVD_FIRMWARE_OFFSET >> 3);
		}

		WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_SIZE0, size);

		WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_LOW,
				lower_32_bits(uvd->inst[i].gpu_addr + offset));
		WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE1_64BIT_BAR_HIGH,
				upper_32_bits(uvd->inst[i].gpu_addr + offset));
		WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_OFFSET1, (1 << 21));
		WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_SIZE1, AMDGPU_UVD_HEAP_SIZE);

		WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_LOW,
				lower_32_bits(uvd->inst[i].gpu_addr + offset + AMDGPU_UVD_HEAP_SIZE));
		WREG32_SOC15(UVD, i, mmUVD_LMI_VCPU_CACHE2_64BIT_BAR_HIGH,
				upper_32_bits(uvd->inst[i].gpu_addr + offset + AMDGPU_UVD_HEAP_SIZE));
		WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_OFFSET2, (2 << 21));
		WREG32_SOC15(UVD, i, mmUVD_VCPU_CACHE_SIZE2,
				AMDGPU_UVD_STACK_SIZE + (AMDGPU_UVD_SESSION_SIZE * 40));

		WREG32_SOC15(UVD, i, mmUVD_UDEC_ADDR_CONFIG,
				gb_addr_config);
		WREG32_SOC15(UVD, i, mmUVD_UDEC_DB_ADDR_CONFIG,
				gb_addr_config);
		WREG32_SOC15(UVD, i, mmUVD_UDEC_DBW_ADDR_CONFIG,
				gb_addr_config);

		WREG32_SOC15(UVD, i, mmUVD_GP_SCRATCH4, uvd->max_handles);
	}
}

/**
 * uvd_v7_0_start - start UVD block
 *
 * Setup and start the UVD block
 */
static int uvd_v7_0_start(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *ring;
	uint32_t rb_bufsz, tmp;
	uint32_t lmi_swap_cntl;
	uint32_t mp_swap_cntl;
	uint32_t k;
	int i, j, r = 0;

	for (k = 0; k < uvd->num_uvd_inst; ++k) {
		if (uvd->harvest_config & (1 << k))
			continue;
		/* disable DPG */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_POWER_STATUS), 0,
				~UVD_POWER_STATUS__UVD_PG_MODE_MASK);
	}

	/* disable byte swapping */
	lmi_swap_cntl = 0;
	mp_swap_cntl = 0;

	uvd_v7_0_mc_resume(adev);

	for (k = 0; k < uvd->num_uvd_inst; ++k) {
		if (uvd->harvest_config & (1 << k))
			continue;
		ring = &uvd->inst[k].ring;
		/* disable clock gating */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_CGC_CTRL), 0,
				~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK);

		/* disable interupt */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_MASTINT_EN), 0,
				~UVD_MASTINT_EN__VCPU_EN_MASK);

		/* stall UMC and register bus before resetting VCPU */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_LMI_CTRL2),
				UVD_LMI_CTRL2__STALL_ARB_UMC_MASK,
				~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);
		mdelay(1);

		/* put LMI, VCPU, RBC etc... into reset */
		WREG32_SOC15(UVD, k, mmUVD_SOFT_RESET,
			UVD_SOFT_RESET__LMI_SOFT_RESET_MASK |
			UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK |
			UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK |
			UVD_SOFT_RESET__RBC_SOFT_RESET_MASK |
			UVD_SOFT_RESET__CSM_SOFT_RESET_MASK |
			UVD_SOFT_RESET__CXW_SOFT_RESET_MASK |
			UVD_SOFT_RESET__TAP_SOFT_RESET_MASK |
			UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);
		mdelay(5);

		/* initialize UVD memory controller */
		WREG32_SOC15(UVD, k, mmUVD_LMI_CTRL,
			(0x40 << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT) |
			UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK |
			UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK |
			UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK |
			UVD_LMI_CTRL__REQ_MODE_MASK |
			0x00100000L);

		/* amdgpu's #ifdef __BIG_ENDIAN arm, when asked for: swap (8 in 32) RB and IB */
		if (uvd->swapped) {
			lmi_swap_cntl = 0xa;
			mp_swap_cntl = 0;
		}
		WREG32_SOC15(UVD, k, mmUVD_LMI_SWAP_CNTL, lmi_swap_cntl);
		WREG32_SOC15(UVD, k, mmUVD_MP_SWAP_CNTL, mp_swap_cntl);

		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_MUXA0, 0x40c2040);
		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_MUXA1, 0x0);
		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_MUXB0, 0x40c2040);
		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_MUXB1, 0x0);
		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_ALU, 0);
		WREG32_SOC15(UVD, k, mmUVD_MPC_SET_MUX, 0x88);

		/* take all subblocks out of reset, except VCPU */
		WREG32_SOC15(UVD, k, mmUVD_SOFT_RESET,
				UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(5);

		/* enable VCPU clock */
		WREG32_SOC15(UVD, k, mmUVD_VCPU_CNTL,
				UVD_VCPU_CNTL__CLK_EN_MASK);

		/* enable UMC */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_LMI_CTRL2), 0,
				~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);

		/* boot up the VCPU */
		WREG32_SOC15(UVD, k, mmUVD_SOFT_RESET, 0);
		mdelay(10);

		for (i = 0; i < 10; ++i) {
			uint32_t status = 0;

			for (j = 0; j < 100; ++j) {
				status = RREG32_SOC15(UVD, k, mmUVD_STATUS);
				if (status & 2)
					break;
				mdelay(10);
			}
			r = 0;
			if (status & 2)
				break;

			frb_err(adev, "UVD(%u) not responding, trying to reset the VCPU!!!", k);
			WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_SOFT_RESET),
					UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK,
					~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
			mdelay(10);
			WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_SOFT_RESET), 0,
					~UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
			mdelay(10);
			r = -1;
		}

		if (r) {
			frb_err(adev, "UVD(%u) not responding, giving up!!!", k);
			return -ETIMEDOUT;
		}
		/* enable master interrupt */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_MASTINT_EN),
			(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK),
			~(UVD_MASTINT_EN__VCPU_EN_MASK|UVD_MASTINT_EN__SYS_EN_MASK));

		/* clear the bit 4 of UVD_STATUS */
		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_STATUS), 0,
				~(2 << UVD_STATUS__VCPU_REPORT__SHIFT));

		/* force RBC into idle state */
		rb_bufsz = order_base_2(ring->ring_size);
		tmp = REG_SET_FIELD(0, UVD_RBC_RB_CNTL, RB_BUFSZ, rb_bufsz);
		tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_BLKSZ, 1);
		tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_FETCH, 1);
		tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_WPTR_POLL_EN, 0);
		tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_NO_UPDATE, 1);
		tmp = REG_SET_FIELD(tmp, UVD_RBC_RB_CNTL, RB_RPTR_WR_EN, 1);
		WREG32_SOC15(UVD, k, mmUVD_RBC_RB_CNTL, tmp);

		/* set the write pointer delay */
		WREG32_SOC15(UVD, k, mmUVD_RBC_RB_WPTR_CNTL, 0);

		/* set the wb address */
		WREG32_SOC15(UVD, k, mmUVD_RBC_RB_RPTR_ADDR,
				(upper_32_bits(ring->gpu_addr) >> 2));

		/* program the RB_BASE for ring buffer */
		WREG32_SOC15(UVD, k, mmUVD_LMI_RBC_RB_64BIT_BAR_LOW,
				lower_32_bits(ring->gpu_addr));
		WREG32_SOC15(UVD, k, mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH,
				upper_32_bits(ring->gpu_addr));

		/* Initialize the ring buffer's read and write pointers */
		WREG32_SOC15(UVD, k, mmUVD_RBC_RB_RPTR, 0);

		ring->wptr = RREG32_SOC15(UVD, k, mmUVD_RBC_RB_RPTR);
		WREG32_SOC15(UVD, k, mmUVD_RBC_RB_WPTR,
				lower_32_bits(ring->wptr));

		WREG32_P(SOC15_REG_OFFSET(UVD, k, mmUVD_RBC_RB_CNTL), 0,
				~UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK);

		/* ring_enc[0] and [1] (UVD_RB_*, UVD_RB_*2): the encode rings are not ported (see the header) */

		uvd->inst[k].starts++;
		frb_info(adev, "uvd: VCPU %u up (start %u): firmware at %02x%08x, ring %llx, %u bytes, LMI swap %u",
			 k, uvd->inst[k].starts, uvd->inst[k].tmr_mc_addr_hi, uvd->inst[k].tmr_mc_addr_lo,
			 (unsigned long long)ring->gpu_addr, ring->ring_size, lmi_swap_cntl);
	}
	return 0;
}

/**
 * uvd_v7_0_stop - stop UVD block
 *
 * stop the UVD block
 */
static void uvd_v7_0_stop(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint8_t i = 0;

	for (i = 0; i < uvd->num_uvd_inst; ++i) {
		if (uvd->harvest_config & (1 << i))
			continue;
		/* force RBC into idle state */
		WREG32_SOC15(UVD, i, mmUVD_RBC_RB_CNTL, 0x11010101);

		/* Stall UMC and register bus before resetting VCPU */
		WREG32_P(SOC15_REG_OFFSET(UVD, i, mmUVD_LMI_CTRL2),
				UVD_LMI_CTRL2__STALL_ARB_UMC_MASK,
				~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);
		mdelay(1);

		/* put VCPU into reset */
		WREG32_SOC15(UVD, i, mmUVD_SOFT_RESET,
				UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
		mdelay(5);

		/* disable VCPU clock */
		WREG32_SOC15(UVD, i, mmUVD_VCPU_CNTL, 0x0);

		/* Unstall UMC and register bus */
		WREG32_P(SOC15_REG_OFFSET(UVD, i, mmUVD_LMI_CTRL2), 0,
				~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);
	}
}

/*
 * uvd_v7_0_set_clockgating_state is empty ("needed for driver unload") and
 * uvd_v7_0 has no set_powergating_state: nothing to port; the SMU's DPM_UVD
 * feature is the block's power control (frb_uvd.c, frb_vega20_hwmgr.c).
 */

/* --- the stage, the tests by hand, the teardown ----------------------------- */

int frb_uvd_v7_0_init(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t j;
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

	r = uvd_v7_0_early_init(adev);
	if (r) {
		frb_err(adev, "uvd: the harvest fuses say this card has no UVD");
		return r;
	}
	r = uvd_v7_0_sw_init(adev);
	if (r)
		return r;
	r = uvd_v7_0_hw_init(adev);
	if (r)
		return r;

	/* amdgpu_device_ip_late_init's amdgpu_ib_ring_tests, for these rings */
	for (j = 0; j < uvd->num_uvd_inst; j++) {
		if (uvd->harvest_config & (1 << j))
			continue;
		r = frb_uvd_ring_test_ib(&uvd->inst[j].ring, FRB_FENCE_TIMEOUT_US);
		if (r) {
			frb_err(adev, "IB test failed on %s (%d).", uvd->inst[j].ring.name, r);
			uvd->inst[j].ring.ready = 0;
			return r;
		}
	}

	/* a second later on Linux: amdgpu_uvd_idle_work_handler */
	r = frb_uvd_idle_work_handler(adev);
	if (r)
		frb_err(adev, "uvd: idle work: %d", r);
	return r;
}

/* every instance's ring test (and with ib, its msg IB test); the first failure */
static int uvd_test_rings(struct frb_device *adev, int ring_test, int ib)
{
	struct frb_uvd *uvd = adev->uvd;
	uint32_t j;
	int r = 0;

	for (j = 0; j < uvd->num_uvd_inst && !r; j++) {
		struct frb_ring *ring = &uvd->inst[j].ring;

		if ((uvd->harvest_config & (1 << j)) || !ring->funcs)
			continue;
		if (ring_test) {
			r = uvd_v7_0_ring_test_ring(ring);
			ring->ready = !r;
		}
		if (!r && ib) {
			if (!ring->ready)
				return -ENODEV;
			r = frb_uvd_ring_test_ib(ring, FRB_FENCE_TIMEOUT_US);
		}
	}
	return r;
}

/*
 * FRB_TEST_SWAP_PROBE: the VCPUs stopped and started again under each LMI
 * swap setting (uvd_v7_0_stop / uvd_v7_0_start, as amdgpu's suspend and
 * resume do; the idle work does not stop a UVD 7 VCPU), with the block's
 * clocks up (begin_use) throughout; the ring tests and the msg IB tests
 * run, then the default (little-endian, swap 0) is put back the same way
 * and the idle work runs. 0 when the default passes; the log says what
 * each did.
 */
static int uvd_swap_probe(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;
	struct frb_ring *first = frb_uvd_ring(adev);
	int pass, r = 0, rs[2] = { -1, -1 }, rr[2] = { -1, -1 }, ri[2] = { -1, -1 };
	uint32_t j;

	if (!first)
		return -ENODEV;
	frb_uvd_ring_begin_use(first);
	for (pass = 0; pass < 3; pass++) {
		int m = pass < 2 ? pass : 0;	/* the third pass puts the default back */

		uvd_v7_0_stop(adev);
		for (j = 0; j < uvd->num_uvd_inst; j++)
			if (!(uvd->harvest_config & (1 << j)) && uvd->inst[j].ring.funcs) {
				/* a stopped VCPU writes no fence; uvd_v7_0_start puts the ring back at 0 */
				frb_fence_driver_force_completion(&uvd->inst[j].ring);
				uvd->inst[j].ring.ready = 0;
			}
		uvd->swapped = m;
		r = uvd_v7_0_start(adev);
		if (pass == 2) {
			if (!r)
				r = uvd_test_rings(adev, 1, 0);
			break;
		}
		rs[m] = r;
		rr[m] = r ? -1 : uvd_test_rings(adev, 1, 0);
		ri[m] = rr[m] ? -1 : uvd_test_rings(adev, 0, 1);
		frb_info(adev, "uvd: swap probe: %s ring and IBs, UVD_LMI_SWAP_CNTL %s: start %d, ring test %d, "
			 "IB test %d", m ? "host-order" : "little-endian", m ? "0xa" : "0", rs[m], rr[m], ri[m]);
	}
	if (!r)
		r = frb_uvd_idle_work_handler(adev);
	return r ? r : (rs[0] || rr[0] || ri[0] ? -EIO : 0);
}

int frb_uvd_v7_0_test(struct frb_device *adev, int test)
{
	struct frb_uvd *uvd = adev->uvd;

	if (!uvd || (!uvd->inst[0].ring.funcs && !uvd->inst[1].ring.funcs))
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return uvd_test_rings(adev, 1, 0);
	case FRB_TEST_IB:
		return uvd_test_rings(adev, 0, 1);
	case FRB_TEST_SWAP_PROBE:
		return uvd_swap_probe(adev);
	case FRB_TEST_IDLE:
		return frb_uvd_idle_work_handler(adev);
	default:
		return -EINVAL;
	}
}

/* uvd_v7_0_hw_fini at unload, before the driver lets go of VRAM */
void frb_uvd_v7_0_fini(struct frb_device *adev)
{
	struct frb_uvd *uvd = adev->uvd;

	if (!uvd)
		return;
	if (uvd->touched) {
		uvd_v7_0_hw_fini(adev);
		frb_info(adev, "uvd: VCPUs stopped (hw_fini)");
	}
	adev->os->free(adev->os->cookie, uvd);
	adev->uvd = NULL;
}
