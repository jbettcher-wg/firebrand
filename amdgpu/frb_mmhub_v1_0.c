/*
 * mmhub_v1_0.c: the memory hub's VM (MMHUB 0, Vega 10 and 20's MMHUB
 * 9.x) ported from Linux 7.1.1 function by function, bare metal (no SR-IOV
 * arms, no SAW: Vega has no ISP). Separate from frb_gfxhub_v1_0.c because
 * the two hubs' register headers use the same names, as in Linux.
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
#include "frb_soc15.h"
#include "include/asic_reg/mmhub/mmhub_1_0_offset.h"
#include "include/asic_reg/mmhub/mmhub_1_0_sh_mask.h"
#include "include/asic_reg/mmhub/mmhub_1_0_default.h"

#define MTYPE_UC	3	/* vega10_enum.h */

/* mmhub_v1_0_get_fb_location: the FB aperture the VBIOS/IFWI chose (Vega 20: MC 0x80_0000_0000) */
uint64_t frb_mmhub_v1_0_get_fb_location(struct frb_device *adev)
{
	uint64_t base = RREG32_SOC15(MMHUB, 0, mmMC_VM_FB_LOCATION_BASE);
	uint64_t top = RREG32_SOC15(MMHUB, 0, mmMC_VM_FB_LOCATION_TOP);

	base &= MC_VM_FB_LOCATION_BASE__FB_BASE_MASK;
	base <<= 24;
	top &= MC_VM_FB_LOCATION_TOP__FB_TOP_MASK;
	top <<= 24;
	adev->gmc.fb_start = base;
	adev->gmc.fb_end = top;
	return base;
}

static void mmhub_v1_0_setup_vm_pt_regs(struct frb_device *adev, uint32_t vmid, uint64_t page_table_base)
{
	struct frb_vmhub_regs *hub = &adev->vmhub[AMDGPU_MMHUB0(0)];

	WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32, hub->ctx_addr_distance * vmid,
			    lower_32_bits(page_table_base));
	WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32, hub->ctx_addr_distance * vmid,
			    upper_32_bits(page_table_base));
}

/* amdgpu_gmc_pd_addr(adev->gart.bo): the table's physical address, valid */
static uint64_t gart_pd_addr(struct frb_device *adev)
{
	return frb_gmc_v9_0_vram_mc2pa(adev, adev->gart.table_addr) | 1 /* AMDGPU_PTE_VALID */;
}

static void mmhub_v1_0_init_gart_aperture_regs(struct frb_device *adev)
{
	mmhub_v1_0_setup_vm_pt_regs(adev, 0, gart_pd_addr(adev));

	WREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_START_ADDR_LO32, (uint32_t)(adev->gmc.gart_start >> 12));
	WREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_START_ADDR_HI32, (uint32_t)(adev->gmc.gart_start >> 44));
	WREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_END_ADDR_LO32, (uint32_t)(adev->gmc.gart_end >> 12));
	WREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_END_ADDR_HI32, (uint32_t)(adev->gmc.gart_end >> 44));
}

static void mmhub_v1_0_init_system_aperture_regs(struct frb_device *adev)
{
	uint64_t value;
	uint32_t tmp;

	/* Program the AGP BAR */
	WREG32_SOC15(MMHUB, 0, mmMC_VM_AGP_BASE, 0);
	WREG32_SOC15(MMHUB, 0, mmMC_VM_AGP_BOT, adev->gmc.agp_start >> 24);
	WREG32_SOC15(MMHUB, 0, mmMC_VM_AGP_TOP, adev->gmc.agp_end >> 24);

	/* Program the system aperture low logical page number. */
	WREG32_SOC15(MMHUB, 0, mmMC_VM_SYSTEM_APERTURE_LOW_ADDR,
			 min_t(uint64_t, adev->gmc.fb_start, adev->gmc.agp_start) >> 18);
	WREG32_SOC15(MMHUB, 0, mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
			 max_t(uint64_t, adev->gmc.fb_end, adev->gmc.agp_end) >> 18);

	/* Set default page address. */
	value = frb_gmc_v9_0_vram_mc2pa(adev, adev->mem_scratch_gpu_addr);
	WREG32_SOC15(MMHUB, 0, mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, (uint32_t)(value >> 12));
	WREG32_SOC15(MMHUB, 0, mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_MSB, (uint32_t)(value >> 44));

	/* Program "protection fault". */
	WREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, (uint32_t)(adev->dummy_page_addr >> 12));
	WREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_HI32, (uint32_t)((uint64_t)adev->dummy_page_addr >> 44));

	tmp = RREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_CNTL2);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL2, ACTIVE_PAGE_MIGRATION_PTE_READ_RETRY, 1);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_CNTL2, tmp);
}

static void mmhub_v1_0_init_tlb_regs(struct frb_device *adev)
{
	uint32_t tmp;

	/* Setup TLB control */
	tmp = RREG32_SOC15(MMHUB, 0, mmMC_VM_MX_L1_TLB_CNTL);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, MTYPE, MTYPE_UC);	/* XXX for emulation. */
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ATC_EN, 1);
	WREG32_SOC15(MMHUB, 0, mmMC_VM_MX_L1_TLB_CNTL, tmp);
}

static void mmhub_v1_0_init_cache_regs(struct frb_device *adev)
{
	uint32_t tmp;

	/* Setup L2 cache */
	tmp = RREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 1);
	/* XXX for emulation, Refer to closed source code.*/
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, L2_PDE0_CACHE_TAG_GENERATION_MODE, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, PDE_FAULT_CLASSIFICATION, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, IDENTITY_MODE_FRAGMENT_SIZE, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL, tmp);

	tmp = RREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL2);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL2, tmp);

	tmp = mmVM_L2_CNTL3_DEFAULT;
	if (adev->gmc.translate_further) {
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, 12);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 9);
	} else {
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, 9);
		tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, 6);
	}
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL3, tmp);

	tmp = mmVM_L2_CNTL4_DEFAULT;
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_PDE_REQUEST_PHYSICAL, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_PTE_REQUEST_PHYSICAL, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CNTL4, tmp);
}

static void mmhub_v1_0_enable_system_domain(struct frb_device *adev)
{
	uint32_t tmp;

	tmp = RREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_CONTEXT0_CNTL, tmp);
}

static void mmhub_v1_0_disable_identity_aperture(struct frb_device *adev)
{
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_LO32, 0XFFFFFFFF);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR_HI32, 0x0000000F);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_LO32, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR_HI32, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_LO32, 0);
	WREG32_SOC15(MMHUB, 0, mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET_HI32, 0);
}

static void mmhub_v1_0_setup_vmid_config(struct frb_device *adev)
{
	struct frb_vmhub_regs *hub = &adev->vmhub[AMDGPU_MMHUB0(0)];
	unsigned int num_level, block_size;
	uint32_t tmp;
	int i;

	num_level = adev->vm_manager.num_level;
	block_size = adev->vm_manager.block_size;
	if (adev->gmc.translate_further)
		num_level -= 1;
	else
		block_size -= 9;

	for (i = 0; i <= 14; i++) {
		tmp = RREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_CNTL, i * hub->ctx_distance);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, num_level);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE, block_size);
		/* Send no-retry XNACK on fault to suppress VM fault storm. */
		tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RETRY_PERMISSION_OR_INVALID_PAGE_FAULT,
				    !adev->gmc.noretry);
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_CNTL, i * hub->ctx_distance, tmp);
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_PAGE_TABLE_START_ADDR_LO32, i * hub->ctx_addr_distance, 0);
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_PAGE_TABLE_START_ADDR_HI32, i * hub->ctx_addr_distance, 0);
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_PAGE_TABLE_END_ADDR_LO32, i * hub->ctx_addr_distance,
				    lower_32_bits(adev->vm_manager.max_pfn - 1));
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_CONTEXT1_PAGE_TABLE_END_ADDR_HI32, i * hub->ctx_addr_distance,
				    upper_32_bits(adev->vm_manager.max_pfn - 1));
	}
}

static void mmhub_v1_0_program_invalidation(struct frb_device *adev)
{
	struct frb_vmhub_regs *hub = &adev->vmhub[AMDGPU_MMHUB0(0)];
	unsigned int i;

	for (i = 0; i < 18; ++i) {
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_INVALIDATE_ENG0_ADDR_RANGE_LO32, i * hub->eng_addr_distance, 0xffffffff);
		WREG32_SOC15_OFFSET(MMHUB, 0, mmVM_INVALIDATE_ENG0_ADDR_RANGE_HI32, i * hub->eng_addr_distance, 0x1f);
	}
}

int frb_mmhub_v1_0_gart_enable(struct frb_device *adev)
{
	/* GART Enable. */
	mmhub_v1_0_init_gart_aperture_regs(adev);
	mmhub_v1_0_init_system_aperture_regs(adev);
	mmhub_v1_0_init_tlb_regs(adev);
	mmhub_v1_0_init_cache_regs(adev);
	mmhub_v1_0_enable_system_domain(adev);
	mmhub_v1_0_disable_identity_aperture(adev);
	mmhub_v1_0_setup_vmid_config(adev);
	mmhub_v1_0_program_invalidation(adev);
	return 0;
}

void frb_mmhub_v1_0_set_fault_enable_default(struct frb_device *adev, int value)
{
	uint32_t tmp;

	tmp = RREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, PDE1_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, PDE2_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, TRANSLATE_FURTHER_PROTECTION_FAULT_ENABLE_DEFAULT,
			    value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, NACK_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	if (!value) {
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_NO_RETRY_FAULT, 1);
		tmp = REG_SET_FIELD(tmp, VM_L2_PROTECTION_FAULT_CNTL, CRASH_ON_RETRY_FAULT, 1);
	}
	WREG32_SOC15(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_CNTL, tmp);
}

void frb_mmhub_v1_0_init(struct frb_device *adev)
{
	struct frb_vmhub_regs *hub = &adev->vmhub[AMDGPU_MMHUB0(0)];

	hub->ctx0_ptb_addr_lo32 = SOC15_REG_OFFSET(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
	hub->ctx0_ptb_addr_hi32 = SOC15_REG_OFFSET(MMHUB, 0, mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32);
	hub->vm_inv_eng0_sem = SOC15_REG_OFFSET(MMHUB, 0, mmVM_INVALIDATE_ENG0_SEM);
	hub->vm_inv_eng0_req = SOC15_REG_OFFSET(MMHUB, 0, mmVM_INVALIDATE_ENG0_REQ);
	hub->vm_inv_eng0_ack = SOC15_REG_OFFSET(MMHUB, 0, mmVM_INVALIDATE_ENG0_ACK);
	hub->vm_context0_cntl = SOC15_REG_OFFSET(MMHUB, 0, mmVM_CONTEXT0_CNTL);
	hub->vm_l2_pro_fault_status = SOC15_REG_OFFSET(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_STATUS);
	hub->vm_l2_pro_fault_cntl = SOC15_REG_OFFSET(MMHUB, 0, mmVM_L2_PROTECTION_FAULT_CNTL);

	hub->ctx_distance = mmVM_CONTEXT1_CNTL - mmVM_CONTEXT0_CNTL;
	hub->ctx_addr_distance = mmVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 - mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
	hub->eng_distance = mmVM_INVALIDATE_ENG1_REQ - mmVM_INVALIDATE_ENG0_REQ;
	hub->eng_addr_distance = mmVM_INVALIDATE_ENG1_ADDR_RANGE_LO32 - mmVM_INVALIDATE_ENG0_ADDR_RANGE_LO32;
}

/* --- clock gating (mmhub_v1_0_set_clockgating, CHIP_VEGA20) ---------------------- */

static void mmhub_v1_0_update_medium_grain_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data, def1, data1, def2, data2;

	def  = data  = RREG32_SOC15(MMHUB, 0, mmATC_L2_MISC_CG);

	/* not Raven */
	def1 = data1 = RREG32_SOC15(MMHUB, 0, mmDAGB0_CNTL_MISC2);
	def2 = data2 = RREG32_SOC15(MMHUB, 0, mmDAGB1_CNTL_MISC2);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_MGCG)) {
		data |= ATC_L2_MISC_CG__ENABLE_MASK;

		data1 &= ~(DAGB0_CNTL_MISC2__DISABLE_WRREQ_CG_MASK |
			   DAGB0_CNTL_MISC2__DISABLE_WRRET_CG_MASK |
			   DAGB0_CNTL_MISC2__DISABLE_RDREQ_CG_MASK |
			   DAGB0_CNTL_MISC2__DISABLE_RDRET_CG_MASK |
			   DAGB0_CNTL_MISC2__DISABLE_TLBWR_CG_MASK |
			   DAGB0_CNTL_MISC2__DISABLE_TLBRD_CG_MASK);

		data2 &= ~(DAGB1_CNTL_MISC2__DISABLE_WRREQ_CG_MASK |
			   DAGB1_CNTL_MISC2__DISABLE_WRRET_CG_MASK |
			   DAGB1_CNTL_MISC2__DISABLE_RDREQ_CG_MASK |
			   DAGB1_CNTL_MISC2__DISABLE_RDRET_CG_MASK |
			   DAGB1_CNTL_MISC2__DISABLE_TLBWR_CG_MASK |
			   DAGB1_CNTL_MISC2__DISABLE_TLBRD_CG_MASK);
	} else {
		data &= ~ATC_L2_MISC_CG__ENABLE_MASK;

		data1 |= (DAGB0_CNTL_MISC2__DISABLE_WRREQ_CG_MASK |
			  DAGB0_CNTL_MISC2__DISABLE_WRRET_CG_MASK |
			  DAGB0_CNTL_MISC2__DISABLE_RDREQ_CG_MASK |
			  DAGB0_CNTL_MISC2__DISABLE_RDRET_CG_MASK |
			  DAGB0_CNTL_MISC2__DISABLE_TLBWR_CG_MASK |
			  DAGB0_CNTL_MISC2__DISABLE_TLBRD_CG_MASK);

		data2 |= (DAGB1_CNTL_MISC2__DISABLE_WRREQ_CG_MASK |
			  DAGB1_CNTL_MISC2__DISABLE_WRRET_CG_MASK |
			  DAGB1_CNTL_MISC2__DISABLE_RDREQ_CG_MASK |
			  DAGB1_CNTL_MISC2__DISABLE_RDRET_CG_MASK |
			  DAGB1_CNTL_MISC2__DISABLE_TLBWR_CG_MASK |
			  DAGB1_CNTL_MISC2__DISABLE_TLBRD_CG_MASK);
	}

	if (def != data)
		WREG32_SOC15(MMHUB, 0, mmATC_L2_MISC_CG, data);

	if (def1 != data1)
		WREG32_SOC15(MMHUB, 0, mmDAGB0_CNTL_MISC2, data1);

	if (def2 != data2)
		WREG32_SOC15(MMHUB, 0, mmDAGB1_CNTL_MISC2, data2);
}

static void mmhub_v1_0_update_medium_grain_light_sleep(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32_SOC15(MMHUB, 0, mmATC_L2_MISC_CG);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_LS))
		data |= ATC_L2_MISC_CG__MEM_LS_ENABLE_MASK;
	else
		data &= ~ATC_L2_MISC_CG__MEM_LS_ENABLE_MASK;

	if (def != data)
		WREG32_SOC15(MMHUB, 0, mmATC_L2_MISC_CG, data);
}

/* mmhub_v1_0_set_clockgating (bare metal, CHIP_VEGA20) */
void frb_mmhub_v1_0_set_clockgating(struct frb_device *adev, int gate)
{
	mmhub_v1_0_update_medium_grain_clock_gating(adev, gate);
	mmhub_v1_0_update_medium_grain_light_sleep(adev, gate);
}
