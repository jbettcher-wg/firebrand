/*
 * gmc_v9_0.c: Vega's memory controller interface (GMC 9, two VM hubs),
 * ported from Linux 7.1.1 function by function for Vega 20 (GC 9.4.0,
 * MMHUB 9.4.0, HDP 4.2.0): sw_init's VRAM and GART placement, hw_init's
 * golden registers, HDP set-up, fault defaults, TLB flushes and the GART
 * on both hubs (frb_gfxhub_v1_0.c, frb_mmhub_v1_0.c).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - FB LOCATION. HBM2 is trained and the FB aperture placed by the IFWI at
 *   power-on (MC 0x80_0000_0000 on the MI50); amdgpu reads it and never
 *   rewrites it, nor does Firebrand.
 * - PLACEMENT. amdgpu allocates its kernel objects top-down from the end of
 *   VRAM (GART table at 0x83_FEF0_0000, mem_scratch 0x83_FEEF_F000 in the
 *   capture). The CPU reaches only BAR0 (256 MB as the platform leaves it;
 *   no resizable BAR on Leopard), so they sit at the top of that window
 *   instead (frb_gmc_v9_0_layout). The registers written are the same ones
 *   in the same order; only those addresses differ, and tests/frb_replay.c
 *   matches them by register (--any).
 * - The GART table (512 MB of GART, 1 MB of 64-bit little-endian entries)
 *   is filled through BAR0 with the dummy page and flags 0, as
 *   amdgpu_gart_unbind leaves an entry ("system bit must be 0 to mean
 *   invalid" from Vega on); amdgpu memsets the bo with the low byte of its
 *   PTE flags instead, which is invalid too.
 * - RadeonNI's address checks guard every address before it reaches a
 *   register: DMA is not translated on this platform (PAMU bypass).
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
#include "include/asic_reg/gc/gc_9_0_sh_mask.h"
#include "include/asic_reg/dce/dce_12_0_offset.h"
#include "include/asic_reg/dce/dce_12_0_sh_mask.h"
#include "include/asic_reg/mmhub/mmhub_1_0_offset.h"
#include "include/asic_reg/athub/athub_1_0_offset.h"
#include "include/asic_reg/athub/athub_1_0_sh_mask.h"
#include "include/asic_reg/hdp/hdp_4_0_offset.h"
#include "include/asic_reg/hdp/hdp_4_0_sh_mask.h"

/* include/uapi/drm/amdgpu_drm.h, amdgpu_gmc.h */
#define AMDGPU_VRAM_TYPE_HBM		6
#define AMDGPU_GMC_HOLE_START		0x0000800000000000ULL
#define four_gb				0x100000000ULL

static const struct soc15_reg_golden golden_settings_mmhub_1_0_0[] = {
	SOC15_REG_GOLDEN_VALUE(MMHUB, 0, mmDAGB1_WRCLI2, 0x00000007, 0xfe5fe0fa),
	SOC15_REG_GOLDEN_VALUE(MMHUB, 0, mmMMEA1_DRAM_WR_CLI2GRP_MAP0, 0x00000030, 0x55555565)
};

static const struct soc15_reg_golden golden_settings_athub_1_0_0[] = {
	SOC15_REG_GOLDEN_VALUE(ATHUB, 0, mmRPB_ARB_CNTL, 0x0000ff00, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(ATHUB, 0, mmRPB_ARB_CNTL2, 0x00ff00ff, 0x00080008)
};

/* amdgpu_gmc_vram_mc2pa: an MC address in VRAM -> the physical address the VM uses */
uint64_t frb_gmc_v9_0_vram_mc2pa(struct frb_device *adev, uint64_t mc_addr)
{
	return mc_addr - adev->gmc.vram_start + adev->vram_base_offset;
}

/* gmc_v9_0_get_invalidate_req */
static uint32_t gmc_v9_0_get_invalidate_req(unsigned int vmid, uint32_t flush_type)
{
	uint32_t req = 0;

	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, PER_VMID_INVALIDATE_REQ, 1 << vmid);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, FLUSH_TYPE, flush_type);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PTES, 1);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE0, 1);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE1, 1);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, INVALIDATE_L2_PDE2, 1);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, INVALIDATE_L1_PTES, 1);
	req = REG_SET_FIELD(req, VM_INVALIDATE_ENG0_REQ, CLEAR_PROTECTION_FAULT_STATUS_ADDR, 0);
	return req;
}

/*
 * gmc_v9_0_flush_gpu_tlb, the path before KIQ exists: invalidation engine
 * 17, with the MMHUB's acquire/release semaphore (gmc_v9_0_use_invalidate_
 * semaphore: MMHUB, bare metal, not Raven), the GFXHUB's dummy read of the
 * request (GC < 9.4.2), then the ACK poll.
 */
void frb_gmc_v9_0_flush_gpu_tlb(struct frb_device *adev, uint32_t vmid, uint32_t vmhub, uint32_t flush_type)
{
	int use_semaphore = vmhub == AMDGPU_MMHUB0(0);
	const unsigned int eng = 17;
	struct frb_vmhub_regs *hub = &adev->vmhub[vmhub];
	uint32_t j, inv_req, tmp, sem, req, ack;

	inv_req = gmc_v9_0_get_invalidate_req(vmid, flush_type);
	sem = hub->vm_inv_eng0_sem + hub->eng_distance * eng;
	req = hub->vm_inv_eng0_req + hub->eng_distance * eng;
	ack = hub->vm_inv_eng0_ack + hub->eng_distance * eng;

	if (use_semaphore) {
		for (j = 0; j < adev->usec_timeout; j++) {
			/* a read return value of 1 means semaphore acquire */
			tmp = RREG32_SOC15_IP_NO_KIQ(MMHUB, sem, 0);
			if (tmp & 0x1)
				break;
			udelay(1);
		}
		if (j >= adev->usec_timeout)
			frb_err(adev, "Timeout waiting for sem acquire in VM flush!");
	}

	WREG32_SOC15_IP_NO_KIQ(MMHUB, req, inv_req, 0);

	/* the dummy read that waits for the ACK register to clear (new fast GRBM interface) */
	if (vmhub == AMDGPU_GFXHUB(0))
		(void)RREG32_NO_KIQ(req);

	for (j = 0; j < adev->usec_timeout; j++) {
		tmp = RREG32_SOC15_IP_NO_KIQ(MMHUB, ack, 0);
		if (tmp & (1 << vmid))
			break;
		udelay(1);
	}

	/* semaphore release after invalidation, write with 0 means semaphore release */
	if (use_semaphore)
		WREG32_SOC15_IP_NO_KIQ(MMHUB, sem, 0, 0);

	if (j >= adev->usec_timeout)
		frb_err(adev, "Timeout waiting for VM flush ACK!");
}

/*
 * Firebrand's placement of amdgpu's VRAM kernel objects on SOC15: the top
 * 16 MB of the CPU window, in amdgpu's top-down order where amdgpu has one
 * (GART table, then mem_scratch). The PSP's and the SMU's buffers follow;
 * the PSP reads its 1 MB firmware buffer by MC address >> 20, so it is
 * 1 MB aligned, and so is the TMR (PSP_TMR_ALIGNMENT).
 */
static int frb_gmc_v9_0_layout(struct frb_device *adev)
{
	struct frb_vram_layout *l = &adev->layout;
	uint64_t window = adev->aper_size;
	uint32_t top;

	if (window > adev->gmc.real_vram_size)
		window = adev->gmc.real_vram_size;
	if (window < (32u << 20)) {
		frb_err(adev, "gmc: a %llu KB CPU window is too small for the driver's VRAM",
			(unsigned long long)(window >> 10));
		return -ERANGE;
	}
	top = (uint32_t)window;
	l->gart_table = top - (1u << 20);			/* 128K entries x 8 */
	l->mem_scratch = l->gart_table - AMDGPU_GPU_PAGE_SIZE;
	l->psp_ring = l->mem_scratch - AMDGPU_GPU_PAGE_SIZE;	/* psp_ring_init: 0x1000 */
	l->psp_cmd = l->psp_ring - AMDGPU_GPU_PAGE_SIZE;
	l->psp_fence = l->psp_cmd - AMDGPU_GPU_PAGE_SIZE;
	l->smu_tables = top - (2u << 20);			/* vega20_smu_init's table bos */
	l->smu_tables_size = 512u << 10;
	l->psp_fw_pri = top - (3u << 20);			/* PSP_1_MEG, 1 MB aligned */
	l->fw_buf = top - (6u << 20);				/* every image the PSP loads */
	l->fw_buf_size = 3u << 20;
	l->psp_tmr = top - (10u << 20);				/* PSP_TMR_SIZE 4 MB, 1 MB aligned */
	l->psp_tmr_size = 4u << 20;
	l->objects = top - (14u << 20);				/* frb_ring.c's VRAM objects (later stages) */
	l->objects_size = 4u << 20;
	/* amdgpu_uvd_sw_init: a VCPU bo per instance (heap, stack, 40 sessions: 2.4 MB) and the 128 KB msg bo */
	l->uvd = top - (20u << 20);
	l->uvd_size = 6u << 20;
	l->reserved_start = l->uvd;
	if (l->psp_fence < l->smu_tables + l->smu_tables_size || (l->psp_fw_pri & 0xfffff) ||
	    (l->psp_tmr & 0xfffff)) {
		frb_err(adev, "gmc: layout overlap or misalignment");
		return -ERANGE;
	}
	return 0;
}

/* gmc_v9_0_vram_gtt_location: the FB the IFWI placed, AGP off, GART best fit (MC 0 here) */
static void gmc_v9_0_vram_gtt_location(struct frb_device *adev)
{
	struct frb_gmc *mc = &adev->gmc;
	uint64_t base = frb_mmhub_v1_0_get_fb_location(adev);
	uint64_t size_af, size_bf, max_mc_address;

	/* amdgpu_gmc_set_agp_default */
	mc->agp_start = 0xffffffffffffULL;
	mc->agp_end = 0;
	mc->agp_size = 0;

	/* amdgpu_gmc_vram_location */
	mc->vram_start = base;
	mc->vram_end = mc->vram_start + mc->mc_vram_size - 1;
	if (mc->real_vram_size < mc->visible_vram_size)
		mc->visible_vram_size = mc->real_vram_size;
	mc->fb_start = mc->vram_start;
	mc->fb_end = mc->vram_end;
	frb_info(adev, "VRAM: %lluM 0x%016llX - 0x%016llX (%lluM used)", (unsigned long long)(mc->mc_vram_size >> 20),
		 (unsigned long long)mc->vram_start, (unsigned long long)mc->vram_end,
		 (unsigned long long)(mc->real_vram_size >> 20));

	/* amdgpu_gmc_gart_location(AMDGPU_GART_PLACEMENT_BEST_FIT) */
	max_mc_address = mc->mc_mask < AMDGPU_GMC_HOLE_START - 1 ? mc->mc_mask : AMDGPU_GMC_HOLE_START - 1;
	size_bf = mc->fb_start;
	size_af = max_mc_address + 1 - ((mc->fb_end + 1 + four_gb - 1) & ~(four_gb - 1));
	if (mc->gart_size > (size_bf > size_af ? size_bf : size_af))
		mc->gart_size = size_bf > size_af ? size_bf : size_af;
	if ((size_bf >= mc->gart_size && size_bf < size_af) || (size_af < mc->gart_size))
		mc->gart_start = 0;
	else
		mc->gart_start = max_mc_address - mc->gart_size + 1;
	mc->gart_start &= ~(four_gb - 1);
	mc->gart_end = mc->gart_start + mc->gart_size - 1;
	frb_info(adev, "GART: %lluM 0x%016llX - 0x%016llX", (unsigned long long)(mc->gart_size >> 20),
		 (unsigned long long)mc->gart_start, (unsigned long long)mc->gart_end);

	/* base offset of vram pages */
	adev->vram_base_offset = frb_gfxhub_v1_0_get_mc_fb_offset(adev);
}

/* gmc_v9_0_mc_init */
static int gmc_v9_0_mc_init(struct frb_device *adev)
{
	adev->gmc.mc_vram_size = (uint64_t)frb_soc15_get_config_memsize(adev) * 1024ULL * 1024ULL;
	adev->gmc.real_vram_size = adev->gmc.mc_vram_size;
	if (!adev->gmc.mc_vram_size)
		return -ENODEV;

	/* no BAR resize on this platform (amdgpu_device_resize_fb_bar): BAR0 as found */
	adev->gmc.aper_size = adev->aper_size;
	adev->gmc.visible_vram_size = adev->gmc.aper_size;

	/* set the gart size: GC 9.4.0 */
	adev->gmc.gart_size = 512ULL << 20;

	gmc_v9_0_vram_gtt_location(adev);
	return 0;
}

/* gmc_v9_0_gart_init + amdgpu_gart_init + amdgpu_gart_table_vram_alloc (no PDB0) */
static int gmc_v9_0_gart_init(struct frb_device *adev)
{
	adev->gart.num_gpu_pages = (uint32_t)(adev->gmc.gart_size / AMDGPU_GPU_PAGE_SIZE);
	adev->gart.table_size = adev->gart.num_gpu_pages * 8;
	adev->gart.table_offset = adev->layout.gart_table;
	adev->gart.table_addr = adev->gmc.vram_start + adev->layout.gart_table;
	frb_info(adev, "[drm] GART: num gpu pages %u, table %u KB at 0x%016llX (BAR0 offset 0x%08x)",
		 adev->gart.num_gpu_pages, adev->gart.table_size >> 10,
		 (unsigned long long)adev->gart.table_addr, adev->gart.table_offset);
	adev->gart.ready = adev->gart.table_size <= (1u << 20);
	return adev->gart.ready ? 0 : -ERANGE;
}

static int gmc_v9_0_sw_init(struct frb_device *adev)
{
	int r, vram_width = 0, vram_type = 0, vram_vendor = 0;

	frb_gfxhub_v1_0_init(adev);
	frb_mmhub_v1_0_init(adev);

	r = frb_atomfirmware_get_vram_info(adev, &vram_width, &vram_type, &vram_vendor);
	if (r)
		return r;
	adev->gmc.vram_width = vram_width;
	adev->gmc.vram_type = vram_type;
	adev->gmc.vram_vendor = vram_vendor;

	/*
	 * GC 9.4.0: amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48), "vm size
	 * is 262144 GB, 4 levels, block size is 9-bit, fragment size is 9-bit"
	 * in the capture; translate_further 0; amdgpu_gmc_noretry_set: 1.
	 */
	adev->vm_manager.max_pfn = (256ULL * 1024 << 30) >> AMDGPU_GPU_PAGE_SHIFT;
	adev->vm_manager.num_level = 3;
	adev->vm_manager.block_size = 9;
	adev->vm_manager.fragment_size = 9;
	adev->gmc.translate_further = 0;
	adev->gmc.noretry = 1;
	adev->gmc.mc_mask = 0xffffffffffffULL; /* 48 bit MC */

	r = gmc_v9_0_mc_init(adev);
	if (r)
		return r;
	r = frb_gmc_v9_0_layout(adev);
	if (r)
		return r;
	adev->mem_scratch_gpu_addr = adev->gmc.vram_start + adev->layout.mem_scratch;
	return gmc_v9_0_gart_init(adev);
}

/*
 * The checks before the first VRAM write and before the GART is switched
 * on (RadeonNI hw/rdn_gart.c scratch_valid and enable_default): BAR0 must be
 * the start of VRAM, every object page aligned and inside both, the GART
 * range clear of VRAM, and the dummy page a bus address the GPU can reach.
 */
static int gmc_v9_0_check_addresses(struct frb_device *adev)
{
	struct frb_vram_layout *l = &adev->layout;
	uint64_t hdp = RREG32_SOC15(HDP, 0, mmHDP_NONSURFACE_BASE) | (uint64_t)RREG32_SOC15(HDP, 0, mmHDP_NONSURFACE_BASE_HI) << 32;
	uint64_t table_end = (uint64_t)l->gart_table + adev->gart.table_size;

	if (hdp << 8 != adev->gmc.vram_start) {
		frb_err(adev, "gmc: HDP_NONSURFACE_BASE %llx does not put BAR0 at the start of VRAM (0x%llx); no VRAM writes",
			(unsigned long long)hdp, (unsigned long long)adev->gmc.vram_start);
		return -EIO;
	}
	if (!adev->aper || (adev->gmc.vram_start & 0xfff) || adev->gmc.vram_start >= (1ULL << 48) ||
	    (l->gart_table & 0xfff) || (l->mem_scratch & 0xfff) ||
	    !frb_vram_ok(adev, l->gart_table, adev->gart.table_size) ||
	    !frb_vram_ok(adev, l->mem_scratch, AMDGPU_GPU_PAGE_SIZE) ||
	    table_end > adev->gmc.real_vram_size ||
	    (l->mem_scratch < table_end && l->gart_table < l->mem_scratch + AMDGPU_GPU_PAGE_SIZE)) {
		frb_err(adev, "gmc: a driver object does not fit the CPU window and VRAM");
		return -ERANGE;
	}
	if (adev->gmc.gart_start <= adev->gmc.vram_end && adev->gmc.vram_start <= adev->gmc.gart_end) {
		frb_err(adev, "gmc: the GART range overlaps VRAM");
		return -ERANGE;
	}
	if (!frb_gart_bus_addr_ok(adev, adev->dummy_page_addr)) {
		frb_err(adev, "gmc: no usable dummy page (bus 0x%llx)", (unsigned long long)adev->dummy_page_addr);
		return -ERANGE;
	}
	return 0;
}

/* The first CPU access to VRAM: patterns through BAR0 over the firmware buffer, read back */
static int gmc_v9_0_vram_test(struct frb_device *adev)
{
	uint32_t base = adev->layout.fw_buf, n = 64u << 10, off, bad = 0, first = 0, want, got;
	int pass;

	for (pass = 0; pass < 2; pass++) {
		for (off = 0; off < n; off += 4)
			frb_vram_write32(adev, base + off, pass ? ~(base + off) : (base + off) * 0x9e3779b1u);
		for (off = 0; off < n; off += 4) {
			want = pass ? ~(base + off) : (base + off) * 0x9e3779b1u;
			got = frb_vram_read32(adev, base + off);
			if (got != want && !bad++)
				first = off;
		}
	}
	for (off = 0; off < n; off += 4)
		frb_vram_write32(adev, base + off, 0);
	frb_info(adev, "gmc: VRAM through BAR0: %u of %u words wrong over %u KB at offset 0x%x%s",
		 bad, 2 * (n / 4), n >> 10, base, bad ? " <- BAD" : "");
	if (bad)
		frb_err(adev, "gmc: first bad word at 0x%x", base + first);
	return bad ? -EIO : 0;
}

/* gmc_v9_0_gart_enable (no PDB0): the GFX hub, then the MMHUB */
static int gmc_v9_0_gart_enable(struct frb_device *adev)
{
	int r;

	if (!adev->gart.ready) {
		frb_err(adev, "No VRAM object for PCIE GART.");
		return -EINVAL;
	}
	r = frb_gfxhub_v1_0_gart_enable(adev);
	if (r)
		return r;
	r = frb_mmhub_v1_0_gart_enable(adev);
	if (r)
		return r;
	frb_info(adev, "[drm] PCIE GART of %uM enabled.", (unsigned int)(adev->gmc.gart_size >> 20));
	frb_info(adev, "[drm] PTB located at 0x%016llX", (unsigned long long)adev->gart.table_addr);
	return 0;
}

static int gmc_v9_0_hw_init(struct frb_device *adev)
{
	uint32_t page;
	int r;

	/* gmc_v9_0_init_golden_registers, MMHUB 9.4.0 */
	frb_soc15_program_register_sequence(adev, golden_settings_mmhub_1_0_0, ARRAY_SIZE(golden_settings_mmhub_1_0_0));
	frb_soc15_program_register_sequence(adev, golden_settings_athub_1_0_0, ARRAY_SIZE(golden_settings_athub_1_0_0));

	/* adev->mode_info.num_crtc: lockout access through the VGA aperture, VGA render off */
	if (adev->num_crtc) {
		WREG32_FIELD15(DCE, 0, VGA_HDP_CONTROL, VGA_MEMORY_DISABLE, 1);
		WREG32_FIELD15(DCE, 0, VGA_RENDER_CONTROL, VGA_VSTATUS_CNTL, 0);
	}

	/* mmhub_v1_0_update_power_gating: no AMD_PG_SUPPORT_MMHUB on Vega 20 */

	/* hdp_v4_0_init_registers (HDP 4.2.0: no HDP_MMHUB_CNTL) */
	WREG32_FIELD15(HDP, 0, HDP_MISC_CNTL, FLUSH_INVALIDATE_CACHE, 1);
	WREG32_SOC15(HDP, 0, mmHDP_NONSURFACE_BASE, (uint32_t)(adev->gmc.vram_start >> 8));
	WREG32_SOC15(HDP, 0, mmHDP_NONSURFACE_BASE_HI, (uint32_t)(adev->gmc.vram_start >> 40));

	/* Firebrand: the checks, the first CPU access to VRAM, the scratch page and the GART table */
	r = gmc_v9_0_check_addresses(adev);
	if (r)
		return r;
	r = gmc_v9_0_vram_test(adev);
	if (r)
		return r;
	for (page = 0; page < AMDGPU_GPU_PAGE_SIZE; page += 4)
		frb_vram_write32(adev, adev->layout.mem_scratch + page, 0);
	for (page = 0; page < adev->gart.num_gpu_pages; page++)
		frb_gart_set_pte(adev, page, adev->dummy_page_addr, 0);

	/* After HDP is initialized, flush HDP. */
	frb_soc15_flush_hdp(adev);

	/* amdgpu_vm_fault_stop is AMDGPU_VM_FAULT_STOP_NEVER by default: value true */
	frb_gfxhub_v1_0_set_fault_enable_default(adev, 1);
	frb_mmhub_v1_0_set_fault_enable_default(adev, 1);
	frb_gmc_v9_0_flush_gpu_tlb(adev, 0, AMDGPU_GFXHUB(0), 0);
	frb_gmc_v9_0_flush_gpu_tlb(adev, 0, AMDGPU_MMHUB0(0), 0);

	/* umc_v6_1 has no init_registers */

	return gmc_v9_0_gart_enable(adev);
}

int frb_gmc_v9_0_sw_init(struct frb_device *adev)
{
	return gmc_v9_0_sw_init(adev);
}

int frb_gmc_v9_0_hw_init(struct frb_device *adev)
{
	int r = gmc_v9_0_hw_init(adev);

	frb_info(adev, "gmc: %u-bit %s, %llu MB at MC 0x%llx (FB offset 0x%llx), BAR0 %llu MB; "
		 "GART %lluM at MC 0x%llx, table at VRAM +0x%x; mem_scratch +0x%x; dummy page 0x%llx",
		 adev->gmc.vram_width, adev->gmc.vram_type == AMDGPU_VRAM_TYPE_HBM ? "HBM" : "memory",
		 (unsigned long long)(adev->gmc.mc_vram_size >> 20), (unsigned long long)adev->gmc.vram_start,
		 (unsigned long long)adev->vram_base_offset, (unsigned long long)(adev->aper_size >> 20),
		 (unsigned long long)(adev->gmc.gart_size >> 20), (unsigned long long)adev->gmc.gart_start,
		 adev->layout.gart_table, adev->layout.mem_scratch, (unsigned long long)adev->dummy_page_addr);
	return r;
}

/* --- clock gating: gmc_v9_0_set_clockgating_state (mmhub_v1_0, athub_v1_0) ------- */

/* athub_v1_0.c athub_update_medium_grain_clock_gating */
static void athub_update_medium_grain_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32_SOC15(ATHUB, 0, mmATHUB_MISC_CNTL);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_MGCG))
		data |= ATHUB_MISC_CNTL__CG_ENABLE_MASK;
	else
		data &= ~ATHUB_MISC_CNTL__CG_ENABLE_MASK;

	if (def != data)
		WREG32_SOC15(ATHUB, 0, mmATHUB_MISC_CNTL, data);
}

/* athub_v1_0.c athub_update_medium_grain_light_sleep */
static void athub_update_medium_grain_light_sleep(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32_SOC15(ATHUB, 0, mmATHUB_MISC_CNTL);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_LS) &&
	    (adev->cg_flags & AMD_CG_SUPPORT_HDP_LS))
		data |= ATHUB_MISC_CNTL__CG_MEM_LS_ENABLE_MASK;
	else
		data &= ~ATHUB_MISC_CNTL__CG_MEM_LS_ENABLE_MASK;

	if (def != data)
		WREG32_SOC15(ATHUB, 0, mmATHUB_MISC_CNTL, data);
}

/* athub_v1_0_set_clockgating (bare metal; Vega 20's ATHUB is in its list: the capture reads it) */
static void athub_v1_0_set_clockgating(struct frb_device *adev, int gate)
{
	athub_update_medium_grain_clock_gating(adev, gate);
	athub_update_medium_grain_light_sleep(adev, gate);
}

void frb_gmc_v9_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	frb_mmhub_v1_0_set_clockgating(adev, gate);

	athub_v1_0_set_clockgating(adev, gate);
}
