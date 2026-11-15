/*
 * gmc_v6_0.c: the GMC 6 memory controller of Southern Islands, ported
 * function by function from Linux 7.0.2. Discrete parts only (the APU arms
 * are not carried), VMID 0's GART for the kernel's own use.
 *
 * Where Firebrand differs from amdgpu, and why (as frb_gmc_v8_0.c):
 * - PLACEMENT. amdgpu allocates its kernel objects top-down from the end of
 *   VRAM; the CPU reaches only BAR0 here, 256 MB, so they sit at the top of
 *   that window (frb_vram_layout). SI's GART is 1 GB on Verde (UVD and VCE
 *   do not support GPUVM, gmc_v6_0_mc_init), so its table is 2 MB.
 * - The GART table is written through BAR0 as little-endian 64-bit entries,
 *   every one unbound (the dummy page, flags 0) before the GART goes on,
 *   then the HDP flush.
 * - RadeonNI's address checks guard every address before it reaches a
 *   register (DMA is not translated by an IOMMU here).
 * - The MC microcode: amdgpu loads it only when the sequencer is not
 *   running (MC_SEQ_SUP_CNTL.RUN), i.e. after a cold ASIC_Init that left it
 *   stopped; it waits for TRAIN_DONE on both channels and returns 0 either
 *   way. Firebrand refuses to go on when training did not finish, as on VI.
 *   RadeonNI's measurements on this card (hw/rdn_si.c): a cold boot loads
 *   and trains (si58_mc.bin, 8122 words with the header); a warm one finds
 *   it running.
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
#include "frb_device.h"
#include "frb_ucode.h"

#include "include/asic_reg/bif/bif_3_0_d.h"
#include "include/asic_reg/bif/bif_3_0_sh_mask.h"
#include "include/asic_reg/oss/oss_1_0_d.h"
#include "include/asic_reg/oss/oss_1_0_sh_mask.h"
#include "include/asic_reg/gmc/gmc_6_0_d.h"
#include "include/asic_reg/gmc/gmc_6_0_sh_mask.h"
#include "include/asic_reg/dce/dce_6_0_d.h"
#include "include/asic_reg/dce/dce_6_0_sh_mask.h"
#include "include/si_enums.h"

#define MC_SEQ_MISC0__MT__MASK   0xf0000000
#define MC_SEQ_MISC0__MT__GDDR1  0x10000000
#define MC_SEQ_MISC0__MT__DDR2   0x20000000
#define MC_SEQ_MISC0__MT__GDDR3  0x30000000
#define MC_SEQ_MISC0__MT__GDDR4  0x40000000
#define MC_SEQ_MISC0__MT__GDDR5  0x50000000
#define MC_SEQ_MISC0__MT__HBM    0x60000000
#define MC_SEQ_MISC0__MT__DDR3   0xB0000000

/* amdgpu_mode.h / amdgpu_gmc.h */
#define AMDGPU_VRAM_TYPE_UNKNOWN 0
#define AMDGPU_VRAM_TYPE_GDDR1 1
#define AMDGPU_VRAM_TYPE_DDR2  2
#define AMDGPU_VRAM_TYPE_GDDR3 3
#define AMDGPU_VRAM_TYPE_GDDR4 4
#define AMDGPU_VRAM_TYPE_GDDR5 5
#define AMDGPU_VRAM_TYPE_HBM   6
#define AMDGPU_VRAM_TYPE_DDR3  7
#define AMDGPU_VBIOS_VGA_ALLOCATION	(9 * 1024 * 1024)

/* gmc_v6_0_is_idle */
static int gmc_v6_0_wait_for_idle(struct frb_device *adev)
{
	unsigned int i;

	for (i = 0; i < adev->usec_timeout; i++) {
		uint32_t tmp = RREG32(mmSRBM_STATUS);

		if (!(tmp & (SRBM_STATUS__MCB_BUSY_MASK | SRBM_STATUS__MCB_NON_DISPLAY_BUSY_MASK |
			     SRBM_STATUS__MCC_BUSY_MASK | SRBM_STATUS__MCD_BUSY_MASK | SRBM_STATUS__VMC_BUSY_MASK)))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* gmc_v6_0_init_microcode: "this memory configuration requires special firmware" */
static int gmc_v6_0_init_microcode(struct frb_device *adev)
{
	const char *chip_name;
	uint32_t misc0;
	int err;

	switch (adev->asic_type) {
	case CHIP_TAHITI:	chip_name = "tahiti"; break;
	case CHIP_PITCAIRN:	chip_name = "pitcairn"; break;
	case CHIP_VERDE:	chip_name = "verde"; break;
	case CHIP_OLAND:	chip_name = "oland"; break;
	case CHIP_HAINAN:	chip_name = "hainan"; break;
	default:		return -EINVAL;
	}

	misc0 = RREG32(mmMC_SEQ_MISC0);
	if (((misc0 & 0xff000000) >> 24) == 0x58)
		chip_name = "si58";

	frb_fw_name(adev->gmc.fw_name, sizeof(adev->gmc.fw_name), chip_name, "_mc.bin");
	frb_info(adev, "gmc: MC_SEQ_MISC0 %08x -> %s", misc0, adev->gmc.fw_name);
	err = adev->os->firmware(adev->os->cookie, adev->gmc.fw_name,
				 &adev->gmc.fw.data, &adev->gmc.fw.size);
	if (err) {
		frb_err(adev, "si_mc: Failed to load firmware \"%s\" (%d)", adev->gmc.fw_name, err);
		adev->gmc.fw.data = 0;
		adev->gmc.fw.size = 0;
	}
	return err;
}

/*
 * gmc_v6_0_mc_load_microcode. The file is little-endian (the amdgpu-headered
 * one): the I/O register pairs and the program words go out with
 * frb_get_le32 (le32_to_cpup) in CPU order.
 */
static int gmc_v6_0_mc_load_microcode(struct frb_device *adev)
{
	struct frb_ucode u;
	const uint8_t *fw_data, *io_mc_regs;
	uint32_t running, io_off, io_size, d0 = 0, d1 = 0;
	int i, regs_size, ucode_size, r;

	if (!adev->gmc.fw.data)
		return -EINVAL;
	r = frb_ucode_parse(adev, adev->gmc.fw_name, &adev->gmc.fw, &u);
	if (r)
		return r;

	adev->gmc.fw_version = u.ucode_version;
	io_size = frb_ucode_le32(&u, FRB_FWH_MC_IO_DEBUG_SIZE);
	io_off = frb_ucode_le32(&u, FRB_FWH_MC_IO_DEBUG_OFFSET);
	if (io_off > u.file_size || io_size > u.file_size - io_off || (io_size & 7)) {
		frb_err(adev, "%s: io debug array %u bytes at %u does not fit", adev->gmc.fw_name,
			io_size, io_off);
		return -EINVAL;
	}
	regs_size = io_size / (4 * 2);
	io_mc_regs = u.data + io_off;
	ucode_size = u.ucode_size_bytes / 4;
	fw_data = u.data + u.ucode_array_offset;

	running = RREG32(mmMC_SEQ_SUP_CNTL) & MC_SEQ_SUP_CNTL__RUN_MASK;
	if (running) {
		frb_info(adev, "gmc: the MC sequencer is running (MC_SEQ_SUP_CNTL %08x, TRAIN_WAKEUP %08x): "
			 "%s not loaded, as amdgpu does", RREG32(mmMC_SEQ_SUP_CNTL),
			 RREG32(mmMC_SEQ_TRAIN_WAKEUP_CNTL), adev->gmc.fw_name);
		return 0;
	}

	/* reset the engine and set to writable */
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000008);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000010);

	/* load mc io regs */
	for (i = 0; i < regs_size; i++) {
		WREG32(mmMC_SEQ_IO_DEBUG_INDEX, frb_get_le32(io_mc_regs, 8 * i));
		WREG32(mmMC_SEQ_IO_DEBUG_DATA, frb_get_le32(io_mc_regs, 8 * i + 4));
	}
	/* load the MC ucode */
	for (i = 0; i < ucode_size; i++)
		WREG32(mmMC_SEQ_SUP_PGM, frb_get_le32(fw_data, 4 * i));

	/* put the engine back into the active state */
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000008);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000004);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000001);

	/* wait for training to complete */
	for (i = 0; i < (int)adev->usec_timeout; i++) {
		d0 = RREG32(mmMC_SEQ_TRAIN_WAKEUP_CNTL);
		if (d0 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D0_MASK)
			break;
		udelay(1);
	}
	for (i = 0; i < (int)adev->usec_timeout; i++) {
		d1 = RREG32(mmMC_SEQ_TRAIN_WAKEUP_CNTL);
		if (d1 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D1_MASK)
			break;
		udelay(1);
	}
	frb_info(adev, "gmc: %s loaded: %d io register pairs, %d program words; TRAIN_WAKEUP %08x "
		 "(D0 %s, D1 %s), MC_SEQ_SUP_CNTL %08x", adev->gmc.fw_name, regs_size, ucode_size, d1,
		 (d0 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D0_MASK) ? "trained" : "NOT trained",
		 (d1 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D1_MASK) ? "trained" : "NOT trained",
		 RREG32(mmMC_SEQ_SUP_CNTL));
	/* amdgpu returns 0 whatever the polls saw; Firebrand refuses to go on */
	return ((d0 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D0_MASK) &&
		(d1 & MC_SEQ_TRAIN_WAKEUP_CNTL__TRAIN_DONE_D1_MASK)) ? 0 : -ETIMEDOUT;
}

/* amdgpu_gmc_vram_location / amdgpu_gmc_gart_location (PLACEMENT_LOW) / set_agp_default */
static void gmc_v6_0_vram_gtt_location(struct frb_device *adev, struct frb_gmc *mc)
{
	uint64_t base = RREG32(mmMC_VM_FB_LOCATION) & 0xFFFF;

	base <<= 24;

	/* amdgpu_gmc_set_agp_default */
	mc->agp_start = 0xffffffffffffULL;
	mc->agp_end = 0;
	mc->agp_size = 0;

	/* amdgpu_gmc_vram_location */
	mc->vram_start = base;
	mc->vram_end = mc->vram_start + mc->mc_vram_size - 1;
	if (mc->real_vram_size < mc->visible_vram_size)
		mc->visible_vram_size = mc->real_vram_size;
	frb_info(adev, "VRAM: %lluM 0x%016llX - 0x%016llX (%lluM used)",
		 (unsigned long long)(mc->mc_vram_size >> 20), (unsigned long long)mc->vram_start,
		 (unsigned long long)mc->vram_end, (unsigned long long)(mc->real_vram_size >> 20));

	/* amdgpu_gmc_gart_location, AMDGPU_GART_PLACEMENT_LOW */
	mc->gart_start = 0;
	/*
	 * Firebrand: with the accelerator, VMID 0's GART at 0x4000_0000, where
	 * RadeonNI had it on this card ("GART of 1024 MB enabled at GPU address
	 * 0x40000000"); its legacy clients take the start as 32 bits.
	 */
	if (adev->gart_legacy && FRB_GART_LEGACY_START + mc->gart_size <= mc->vram_start) {
		frb_info(adev, "GART: moved from 0x%016llX to 0x%016llX for the accelerator's legacy clients",
			 (unsigned long long)mc->gart_start, (unsigned long long)FRB_GART_LEGACY_START);
		mc->gart_start = FRB_GART_LEGACY_START;
	}
	mc->gart_end = mc->gart_start + mc->gart_size - 1;
	frb_info(adev, "GART: %lluM 0x%016llX - 0x%016llX",
		 (unsigned long long)(mc->gart_size >> 20), (unsigned long long)mc->gart_start,
		 (unsigned long long)mc->gart_end);
}

/* gmc_v6_0_mc_program */
static void gmc_v6_0_mc_program(struct frb_device *adev)
{
	int i, j;

	/* Initialize HDP */
	for (i = 0, j = 0; i < 32; i++, j += 0x6) {
		WREG32((0xb05 + j), 0x00000000);
		WREG32((0xb06 + j), 0x00000000);
		WREG32((0xb07 + j), 0x00000000);
		WREG32((0xb08 + j), 0x00000000);
		WREG32((0xb09 + j), 0x00000000);
	}
	WREG32(mmHDP_REG_COHERENCY_FLUSH_CNTL, 0);

	if (gmc_v6_0_wait_for_idle(adev))
		frb_err(adev, "Wait for MC idle timedout !");

	if (adev->num_crtc) {
		uint32_t tmp;

		/* Lockout access through VGA aperture*/
		tmp = RREG32(mmVGA_HDP_CONTROL);
		tmp |= VGA_HDP_CONTROL__VGA_MEMORY_DISABLE_MASK;
		WREG32(mmVGA_HDP_CONTROL, tmp);

		/* disable VGA render */
		tmp = RREG32(mmVGA_RENDER_CONTROL);
		tmp &= VGA_RENDER_CONTROL__VGA_VSTATUS_CNTL_MASK;
		WREG32(mmVGA_RENDER_CONTROL, tmp);
	}
	/* Update configuration */
	WREG32(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR, (uint32_t)(adev->gmc.vram_start >> 12));
	WREG32(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR, (uint32_t)(adev->gmc.vram_end >> 12));
	WREG32(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR, (uint32_t)(adev->mem_scratch_gpu_addr >> 12));
	WREG32(mmMC_VM_AGP_BASE, 0);
	WREG32(mmMC_VM_AGP_TOP, (uint32_t)(adev->gmc.agp_end >> 22));
	WREG32(mmMC_VM_AGP_BOT, (uint32_t)(adev->gmc.agp_start >> 22));

	if (gmc_v6_0_wait_for_idle(adev))
		frb_err(adev, "Wait for MC idle timedout !");
}

/* gmc_v6_0_mc_init */
static int gmc_v6_0_mc_init(struct frb_device *adev)
{
	uint32_t tmp;
	int chansize, numchan;

	tmp = RREG32(mmMC_ARB_RAMCFG);
	if (tmp & (1 << 11))
		chansize = 16;
	else if (tmp & MC_ARB_RAMCFG__CHANSIZE_MASK)
		chansize = 64;
	else
		chansize = 32;

	tmp = RREG32(mmMC_SHARED_CHMAP);
	switch ((tmp & MC_SHARED_CHMAP__NOOFCHAN_MASK) >> MC_SHARED_CHMAP__NOOFCHAN__SHIFT) {
	case 0:
	default:
		numchan = 1;
		break;
	case 1:
		numchan = 2;
		break;
	case 2:
		numchan = 4;
		break;
	case 3:
		numchan = 8;
		break;
	case 4:
		numchan = 3;
		break;
	case 5:
		numchan = 6;
		break;
	case 6:
		numchan = 10;
		break;
	case 7:
		numchan = 12;
		break;
	case 8:
		numchan = 16;
		break;
	}
	adev->gmc.vram_width = numchan * chansize;
	/* size in MB on si */
	adev->gmc.mc_vram_size = RREG32(mmCONFIG_MEMSIZE) * 1024ULL * 1024ULL;
	adev->gmc.real_vram_size = RREG32(mmCONFIG_MEMSIZE) * 1024ULL * 1024ULL;
	if (!adev->gmc.mc_vram_size || RREG32(mmCONFIG_MEMSIZE) == 0xffffffff)
		return -ENODEV;

	/* no BAR resize on this platform (amdgpu_device_resize_fb_bar) */
	adev->gmc.aper_size = adev->aper_size;
	adev->gmc.visible_vram_size = adev->gmc.aper_size;

	/* set the gart size */
	switch (adev->asic_type) {
	case CHIP_HAINAN:    /* no MM engines */
	default:
		adev->gmc.gart_size = 256ULL << 20;
		break;
	case CHIP_VERDE:    /* UVD, VCE do not support GPUVM */
	case CHIP_TAHITI:   /* UVD, VCE do not support GPUVM */
	case CHIP_PITCAIRN: /* UVD, VCE do not support GPUVM */
	case CHIP_OLAND:    /* UVD, VCE do not support GPUVM */
		adev->gmc.gart_size = 1024ULL << 20;
		break;
	}
	/* + adev->pm.smu_prv_buffer_size: 0 (amdgpu_smu_memory_pool_size defaults to 0) */

	gmc_v6_0_vram_gtt_location(adev, &adev->gmc);
	return 0;
}

/* gmc_v6_0_get_vbios_fb_size, for amdgpu_gmc_get_vbios_allocations */
static unsigned int gmc_v6_0_get_vbios_fb_size(struct frb_device *adev)
{
	uint32_t d1vga_control = RREG32(mmD1VGA_CONTROL);
	unsigned int size;

	if (REG_GET_FIELD(d1vga_control, D1VGA_CONTROL, D1VGA_MODE_ENABLE)) {
		size = AMDGPU_VBIOS_VGA_ALLOCATION;
	} else {
		uint32_t viewport = RREG32(mmVIEWPORT_SIZE);

		size = (REG_GET_FIELD(viewport, VIEWPORT_SIZE, VIEWPORT_HEIGHT) *
			REG_GET_FIELD(viewport, VIEWPORT_SIZE, VIEWPORT_WIDTH) *
			4);
	}
	return size;
}

/*
 * Firebrand's placement of amdgpu's VRAM kernel objects at the top of the
 * CPU window: the 2 MB GART table, the scratch page, then the regions the
 * later stages take (frb_ring.c's objects, UVD's VCPU bo), as on VI. SI has
 * no SMU TOC or SMU-loaded microcode (si_dpm writes the SMC's SRAM through
 * its index window), so those are empty.
 */
static int gmc_v6_0_layout(struct frb_device *adev)
{
	struct frb_vram_layout *l = &adev->layout;
	uint64_t window = adev->aper_size;

	if (window > adev->gmc.real_vram_size)
		window = adev->gmc.real_vram_size;
	if (window < (32u << 20)) {
		frb_err(adev, "gmc: a %llu KB CPU window is too small for the driver's VRAM",
			(unsigned long long)(window >> 10));
		return -ERANGE;
	}
	memset(l, 0, sizeof(*l));
	l->gart_table_size = (uint32_t)(adev->gmc.gart_size / AMDGPU_GPU_PAGE_SIZE) * 8;
	l->gart_table = (uint32_t)window - l->gart_table_size;
	l->mem_scratch = l->gart_table - AMDGPU_GPU_PAGE_SIZE;
	l->objects = (uint32_t)window - (8u << 20);
	l->objects_size = 4u << 20;
	l->uvd = (uint32_t)window - (12u << 20);
	l->uvd_size = 4u << 20;
	l->reserved_start = l->uvd;
	if (l->objects + l->objects_size > l->mem_scratch || l->uvd + l->uvd_size > l->objects) {
		frb_err(adev, "gmc: layout overlap");
		return -ERANGE;
	}
	return 0;
}

static int gmc_v6_0_sw_init(struct frb_device *adev)
{
	uint32_t tmp, vga;
	int r;

	tmp = RREG32(mmMC_SEQ_MISC0);
	tmp &= MC_SEQ_MISC0__MT__MASK;
	switch (tmp) {
	case MC_SEQ_MISC0__MT__GDDR1: adev->gmc.vram_type = AMDGPU_VRAM_TYPE_GDDR1; break;
	case MC_SEQ_MISC0__MT__DDR2:  adev->gmc.vram_type = AMDGPU_VRAM_TYPE_DDR2; break;
	case MC_SEQ_MISC0__MT__GDDR3: adev->gmc.vram_type = AMDGPU_VRAM_TYPE_GDDR3; break;
	case MC_SEQ_MISC0__MT__GDDR4: adev->gmc.vram_type = AMDGPU_VRAM_TYPE_GDDR4; break;
	case MC_SEQ_MISC0__MT__GDDR5: adev->gmc.vram_type = AMDGPU_VRAM_TYPE_GDDR5; break;
	case MC_SEQ_MISC0__MT__HBM:   adev->gmc.vram_type = AMDGPU_VRAM_TYPE_HBM; break;
	case MC_SEQ_MISC0__MT__DDR3:  adev->gmc.vram_type = AMDGPU_VRAM_TYPE_DDR3; break;
	default:		      adev->gmc.vram_type = AMDGPU_VRAM_TYPE_UNKNOWN; break;
	}

	/*
	 * amdgpu_vm_adjust_size(adev, 64, 9, 1, 40): 64 GB of VM, one level,
	 * 9-bit blocks; amdgpu's own vm_size default for SI.
	 */
	adev->vm_manager.max_pfn = (64ULL << 30) >> AMDGPU_GPU_PAGE_SHIFT;
	adev->vm_manager.block_size = 9;
	adev->vm_manager.fragment_size = 9;
	adev->gmc.mc_mask = 0xffffffffffULL;	/* 40 bit MC */

	r = gmc_v6_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "Failed to load mc firmware!");
		return r;
	}

	r = gmc_v6_0_mc_init(adev);
	if (r)
		return r;

	vga = gmc_v6_0_get_vbios_fb_size(adev);
	adev->gmc.stolen_vga_size = vga;
	frb_info(adev, "gmc: pre-OS display holds %u KB at the start of VRAM", vga >> 10);

	r = gmc_v6_0_layout(adev);
	if (r)
		return r;
	adev->mem_scratch_gpu_addr = adev->gmc.vram_start + adev->layout.mem_scratch;
	return frb_gart_init(adev);
}

/* the checks before the first VRAM write and the GART (frb_gmc_v8_0.c's) */
static int gmc_v6_0_check_addresses(struct frb_device *adev)
{
	struct frb_vram_layout *l = &adev->layout;
	uint32_t hdp = RREG32(mmHDP_NONSURFACE_BASE);
	uint64_t table_end = (uint64_t)l->gart_table + adev->gart.table_size;

	if ((uint64_t)hdp << 8 != adev->gmc.vram_start) {
		frb_err(adev, "gmc: HDP_NONSURFACE_BASE %08x does not put BAR0 at the start of VRAM "
			"(0x%llx); no VRAM writes", hdp, (unsigned long long)adev->gmc.vram_start);
		return -EIO;
	}
	if (!adev->aper || (adev->gmc.vram_start & 0xfff) ||
	    adev->gmc.vram_start >= (1ULL << 40) ||
	    adev->gmc.mc_vram_size > (1ULL << 40) - adev->gmc.vram_start ||
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
		frb_err(adev, "gmc: no usable dummy page (bus 0x%llx)",
			(unsigned long long)adev->dummy_page_addr);
		return -ERANGE;
	}
	return 0;
}

/* the CPU's first look at VRAM after the MC microcode (frb_gmc_v8_0.c's) */
static int gmc_v6_0_vram_test(struct frb_device *adev)
{
	uint32_t base = adev->layout.objects, n = 64u << 10, off, bad = 0, first = 0, want, got = 0;
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

/* gmc_v6_0_set_fault_enable_default */
static void gmc_v6_0_set_fault_enable_default(struct frb_device *adev, int value)
{
	uint32_t tmp;

	tmp = RREG32(mmVM_CONTEXT1_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, value);
	WREG32(mmVM_CONTEXT1_CNTL, tmp);
}

/* gmc_v6_0_gart_enable */
static int gmc_v6_0_gart_enable(struct frb_device *adev)
{
	uint64_t table_addr;
	uint32_t field;
	int i;

	if (!adev->gart.ready)
		return -EINVAL;
	table_addr = adev->gart.table_addr;

	/* Setup TLB control */
	WREG32(mmMC_VM_MX_L1_TLB_CNTL,
	       (0xA << 7) |
	       MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_TLB_MASK |
	       MC_VM_MX_L1_TLB_CNTL__ENABLE_L1_FRAGMENT_PROCESSING_MASK |
	       MC_VM_MX_L1_TLB_CNTL__SYSTEM_ACCESS_MODE_MASK |
	       MC_VM_MX_L1_TLB_CNTL__ENABLE_ADVANCED_DRIVER_MODEL_MASK |
	       (0UL << MC_VM_MX_L1_TLB_CNTL__SYSTEM_APERTURE_UNMAPPED_ACCESS__SHIFT));
	/* Setup L2 cache */
	WREG32(mmVM_L2_CNTL,
	       VM_L2_CNTL__ENABLE_L2_CACHE_MASK |
	       VM_L2_CNTL__ENABLE_L2_FRAGMENT_PROCESSING_MASK |
	       VM_L2_CNTL__ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE_MASK |
	       VM_L2_CNTL__ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE_MASK |
	       (7UL << VM_L2_CNTL__EFFECTIVE_L2_QUEUE_SIZE__SHIFT) |
	       (1UL << VM_L2_CNTL__CONTEXT1_IDENTITY_ACCESS_MODE__SHIFT));
	WREG32(mmVM_L2_CNTL2,
	       VM_L2_CNTL2__INVALIDATE_ALL_L1_TLBS_MASK |
	       VM_L2_CNTL2__INVALIDATE_L2_CACHE_MASK);

	field = adev->vm_manager.fragment_size;
	WREG32(mmVM_L2_CNTL3,
	       VM_L2_CNTL3__L2_CACHE_BIGK_ASSOCIATIVITY_MASK |
	       (field << VM_L2_CNTL3__BANK_SELECT__SHIFT) |
	       (field << VM_L2_CNTL3__L2_CACHE_BIGK_FRAGMENT_SIZE__SHIFT));
	/* setup context0 */
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR, (uint32_t)(adev->gmc.gart_start >> 12));
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR, (uint32_t)(adev->gmc.gart_end >> 12));
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR, (uint32_t)(table_addr >> 12));
	WREG32(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR,
	       (uint32_t)(adev->dummy_page_addr >> 12));
	WREG32(mmVM_CONTEXT0_CNTL2, 0);
	WREG32(mmVM_CONTEXT0_CNTL,
	       VM_CONTEXT0_CNTL__ENABLE_CONTEXT_MASK |
	       (0UL << VM_CONTEXT0_CNTL__PAGE_TABLE_DEPTH__SHIFT) |
	       VM_CONTEXT0_CNTL__RANGE_PROTECTION_FAULT_ENABLE_DEFAULT_MASK);

	WREG32(0x575, 0);
	WREG32(0x576, 0);
	WREG32(0x577, 0);

	/* empty context1-15 */
	/* set vm size, must be a multiple of 4 */
	WREG32(mmVM_CONTEXT1_PAGE_TABLE_START_ADDR, 0);
	WREG32(mmVM_CONTEXT1_PAGE_TABLE_END_ADDR, (uint32_t)(adev->vm_manager.max_pfn - 1));
	for (i = 1; i < 16; i++) {
		if (i < 8)
			WREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + i, (uint32_t)(table_addr >> 12));
		else
			WREG32(mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + i - 8, (uint32_t)(table_addr >> 12));
	}

	/* enable context1-15 */
	WREG32(mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR,
	       (uint32_t)(adev->dummy_page_addr >> 12));
	WREG32(mmVM_CONTEXT1_CNTL2, 4);
	WREG32(mmVM_CONTEXT1_CNTL,
	       VM_CONTEXT1_CNTL__ENABLE_CONTEXT_MASK |
	       (1UL << VM_CONTEXT1_CNTL__PAGE_TABLE_DEPTH__SHIFT) |
	       ((adev->vm_manager.block_size - 9)
	       << VM_CONTEXT1_CNTL__PAGE_TABLE_BLOCK_SIZE__SHIFT));
	/* amdgpu_vm_fault_stop default: AMDGPU_VM_FAULT_STOP_NEVER */
	gmc_v6_0_set_fault_enable_default(adev, 1);

	/* gmc_v6_0_flush_gpu_tlb(adev, 0, 0, 0) */
	WREG32(mmVM_INVALIDATE_REQUEST, 1 << 0);
	frb_info(adev, "PCIE GART of %uM enabled (table at 0x%016llX).",
		 (unsigned int)(adev->gmc.gart_size >> 20), (unsigned long long)table_addr);
	return 0;
}

/* si_flush_hdp (amdgpu_device_flush_hdp) */
static void si_flush_hdp(struct frb_device *adev)
{
	WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}

/* The F2 evidence: what the memory controller says after hw_init. */
static void gmc_v6_0_report(struct frb_device *adev)
{
	frb_info(adev, "gmc: %u-bit %s, %llu MB; FB_LOCATION %08x, SYSTEM_APERTURE %08x..%08x "
		 "default %08x, AGP %08x/%08x/%08x, BIF_FB_EN %x, HDP_NONSURFACE_BASE %08x",
		 adev->gmc.vram_width, adev->gmc.vram_type == AMDGPU_VRAM_TYPE_GDDR5 ? "GDDR5" : "memory",
		 (unsigned long long)(adev->gmc.mc_vram_size >> 20),
		 RREG32(mmMC_VM_FB_LOCATION), RREG32(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR),
		 RREG32(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR), RREG32(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR),
		 RREG32(mmMC_VM_AGP_BASE), RREG32(mmMC_VM_AGP_TOP), RREG32(mmMC_VM_AGP_BOT),
		 RREG32(mmBIF_FB_EN), RREG32(mmHDP_NONSURFACE_BASE));
	frb_info(adev, "gmc: MC_SEQ_MISC0 %08x, MC_SEQ_SUP_CNTL %08x, TRAIN_WAKEUP %08x, MC_ARB_RAMCFG %08x, "
		 "MC_SHARED_CHMAP %08x, ucode %s version 0x%08x",
		 RREG32(mmMC_SEQ_MISC0), RREG32(mmMC_SEQ_SUP_CNTL), RREG32(mmMC_SEQ_TRAIN_WAKEUP_CNTL),
		 RREG32(mmMC_ARB_RAMCFG), RREG32(mmMC_SHARED_CHMAP), adev->gmc.fw_name, adev->gmc.fw_version);
	frb_info(adev, "gmc: VM_CONTEXT0 %08x..%08x table %08x cntl %08x, VM_L2_CNTL %08x/%08x/%08x, "
		 "MX_L1_TLB %08x, fault default %08x",
		 RREG32(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR), RREG32(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR),
		 RREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR), RREG32(mmVM_CONTEXT0_CNTL),
		 RREG32(mmVM_L2_CNTL), RREG32(mmVM_L2_CNTL2), RREG32(mmVM_L2_CNTL3),
		 RREG32(mmMC_VM_MX_L1_TLB_CNTL), RREG32(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR));
}

static int gmc_v6_0_hw_init(struct frb_device *adev)
{
	uint32_t page;
	int r;

	gmc_v6_0_mc_program(adev);

	r = gmc_v6_0_mc_load_microcode(adev);
	if (r) {
		frb_err(adev, "Failed to load MC firmware!");
		return r;
	}

	/* Firebrand: the checks, the first CPU access to VRAM, the table */
	r = gmc_v6_0_check_addresses(adev);
	if (r)
		return r;
	r = gmc_v6_0_vram_test(adev);
	if (r)
		return r;
	for (page = 0; page < AMDGPU_GPU_PAGE_SIZE; page += 4)
		frb_vram_write32(adev, adev->layout.mem_scratch + page, 0);
	for (page = 0; page < adev->gart.num_gpu_pages; page++)
		frb_gart_set_pte(adev, page, adev->dummy_page_addr, 0);
	si_flush_hdp(adev);

	return gmc_v6_0_gart_enable(adev);
}

int frb_gmc_v6_0_sw_init(struct frb_device *adev)
{
	return gmc_v6_0_sw_init(adev);
}

int frb_gmc_v6_0_hw_init(struct frb_device *adev)
{
	int r = gmc_v6_0_hw_init(adev);

	gmc_v6_0_report(adev);
	return r;
}

/* --- clock gating (gmc_v6_0_set_clockgating_state, from amdgpu's late init) --- */

static const uint32_t mc_cg_registers[] = {
	mmMC_HUB_MISC_HUB_CG,
	mmMC_HUB_MISC_SIP_CG,
	mmMC_HUB_MISC_VM_CG,
	mmMC_XPB_CLK_GAT,
	mmATC_MISC_CG,
	mmMC_CITF_MISC_WR_CG,
	mmMC_CITF_MISC_RD_CG,
	mmMC_CITF_MISC_VM_CG,
	mmVM_L2_CG,
};

static const uint32_t mc_cg_ls_en[] = {
	MC_HUB_MISC_HUB_CG__MEM_LS_ENABLE_MASK,
	MC_HUB_MISC_SIP_CG__MEM_LS_ENABLE_MASK,
	MC_HUB_MISC_VM_CG__MEM_LS_ENABLE_MASK,
	MC_XPB_CLK_GAT__MEM_LS_ENABLE_MASK,
	ATC_MISC_CG__MEM_LS_ENABLE_MASK,
	MC_CITF_MISC_WR_CG__MEM_LS_ENABLE_MASK,
	MC_CITF_MISC_RD_CG__MEM_LS_ENABLE_MASK,
	MC_CITF_MISC_VM_CG__MEM_LS_ENABLE_MASK,
	VM_L2_CG__MEM_LS_ENABLE_MASK,
};

static const uint32_t mc_cg_en[] = {
	MC_HUB_MISC_HUB_CG__ENABLE_MASK,
	MC_HUB_MISC_SIP_CG__ENABLE_MASK,
	MC_HUB_MISC_VM_CG__ENABLE_MASK,
	MC_XPB_CLK_GAT__ENABLE_MASK,
	ATC_MISC_CG__ENABLE_MASK,
	MC_CITF_MISC_WR_CG__ENABLE_MASK,
	MC_CITF_MISC_RD_CG__ENABLE_MASK,
	MC_CITF_MISC_VM_CG__ENABLE_MASK,
	VM_L2_CG__ENABLE_MASK,
};

static void gmc_v6_0_enable_mc_ls(struct frb_device *adev,
				  int enable)
{
	int i;
	uint32_t orig, data;

	for (i = 0; i < ARRAY_SIZE(mc_cg_registers); i++) {
		orig = data = RREG32(mc_cg_registers[i]);
		if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_LS))
			data |= mc_cg_ls_en[i];
		else
			data &= ~mc_cg_ls_en[i];
		if (data != orig)
			WREG32(mc_cg_registers[i], data);
	}
}

static void gmc_v6_0_enable_mc_mgcg(struct frb_device *adev,
				    int enable)
{
	int i;
	uint32_t orig, data;

	for (i = 0; i < ARRAY_SIZE(mc_cg_registers); i++) {
		orig = data = RREG32(mc_cg_registers[i]);
		if (enable && (adev->cg_flags & AMD_CG_SUPPORT_MC_MGCG))
			data |= mc_cg_en[i];
		else
			data &= ~mc_cg_en[i];
		if (data != orig)
			WREG32(mc_cg_registers[i], data);
	}
}

static void gmc_v6_0_enable_bif_mgls(struct frb_device *adev,
				     int enable)
{
	uint32_t orig, data;

	orig = data = RREG32_PCIE(ixPCIE_CNTL2);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_BIF_LS)) {
		data = REG_SET_FIELD(data, PCIE_CNTL2, SLV_MEM_LS_EN, 1);
		data = REG_SET_FIELD(data, PCIE_CNTL2, MST_MEM_LS_EN, 1);
		data = REG_SET_FIELD(data, PCIE_CNTL2, REPLAY_MEM_LS_EN, 1);
		data = REG_SET_FIELD(data, PCIE_CNTL2, SLV_MEM_AGGRESSIVE_LS_EN, 1);
	} else {
		data = REG_SET_FIELD(data, PCIE_CNTL2, SLV_MEM_LS_EN, 0);
		data = REG_SET_FIELD(data, PCIE_CNTL2, MST_MEM_LS_EN, 0);
		data = REG_SET_FIELD(data, PCIE_CNTL2, REPLAY_MEM_LS_EN, 0);
		data = REG_SET_FIELD(data, PCIE_CNTL2, SLV_MEM_AGGRESSIVE_LS_EN, 0);
	}

	if (orig != data)
		WREG32_PCIE(ixPCIE_CNTL2, data);
}

static void gmc_v6_0_enable_hdp_mgcg(struct frb_device *adev,
				     int enable)
{
	uint32_t orig, data;

	orig = data = RREG32(mmHDP_HOST_PATH_CNTL);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_HDP_MGCG))
		data = REG_SET_FIELD(data, HDP_HOST_PATH_CNTL, CLOCK_GATING_DIS, 0);
	else
		data = REG_SET_FIELD(data, HDP_HOST_PATH_CNTL, CLOCK_GATING_DIS, 1);

	if (orig != data)
		WREG32(mmHDP_HOST_PATH_CNTL, data);
}

static void gmc_v6_0_enable_hdp_ls(struct frb_device *adev,
				   int enable)
{
	uint32_t orig, data;

	orig = data = RREG32(mmHDP_MEM_POWER_LS);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_HDP_LS))
		data = REG_SET_FIELD(data, HDP_MEM_POWER_LS, LS_ENABLE, 1);
	else
		data = REG_SET_FIELD(data, HDP_MEM_POWER_LS, LS_ENABLE, 0);

	if (orig != data)
		WREG32(mmHDP_MEM_POWER_LS, data);
}


void frb_gmc_v6_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	/* not an APU */
	gmc_v6_0_enable_mc_mgcg(adev, gate);
	gmc_v6_0_enable_mc_ls(adev, gate);
	gmc_v6_0_enable_bif_mgls(adev, gate);
	gmc_v6_0_enable_hdp_mgcg(adev, gate);
	gmc_v6_0_enable_hdp_ls(adev, gate);
}
