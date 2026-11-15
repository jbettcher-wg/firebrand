/*
 * gmc_v8_0.c: the GMC 8.1 memory controller of Polaris, ported function by
 * function from Linux 7.0.2. Polaris 12 paths only (Tonga/Fiji/APU arms and
 * SR-IOV are not carried).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - PLACEMENT. amdgpu allocates its kernel objects top-down from the end of
 *   VRAM (on the E9173: mem_scratch 0xF4_7FF7_F000, GART table
 *   0xF4_7FF8_0000). The CPU reaches only BAR0 here, 256 MB, so the same
 *   objects sit at the top of that window instead (frb_vram_layout). The
 *   registers written are the same ones in the same order; only those
 *   addresses differ. tests/frb_replay.c runs this file with amdgpu's
 *   addresses and checks every value against the capture.
 * - The GART table is filled through BAR0 with little-endian 64-bit entries
 *   (amdgpu_gmc_set_pte_pde writes them with writeq, which is LE). Every
 *   entry starts unbound, as amdgpu_gart_unbind leaves it: the dummy page,
 *   flags 0. The fill is followed by the HDP flush amdgpu_gart_map does after
 *   CPU writes to the table.
 * - RadeonNI's address checks (hw/rdn_gart.c scratch_valid/enable_default)
 *   guard every address before it reaches a register: DMA is not translated
 *   on this platform (PAMU bypass), so a wrong one corrupts host RAM.
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

#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/gmc/gmc_8_1_sh_mask.h"

#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"

#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/oss/oss_3_0_sh_mask.h"

#include "include/asic_reg/dce/dce_10_0_d.h"
#include "include/asic_reg/dce/dce_10_0_sh_mask.h"

/* vid.h / cikd.h */
#define MC_SEQ_MISC0__MT__MASK	0xf0000000
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
#define AMDGPU_GMC_HOLE_START	0x0000800000000000ULL
#define AMDGPU_VBIOS_VGA_ALLOCATION	(9 * 1024 * 1024)
#define FOUR_GB			0x100000000ULL

/* amdgpu_vm.h: PTE bits (VI) */
#define AMDGPU_PTE_VALID	(1ULL << 0)
#define AMDGPU_PTE_SYSTEM	(1ULL << 1)
#define AMDGPU_PTE_SNOOPED	(1ULL << 2)
#define AMDGPU_PTE_EXECUTABLE	(1ULL << 4)
#define AMDGPU_PTE_READABLE	(1ULL << 5)
#define AMDGPU_PTE_WRITEABLE	(1ULL << 6)

static const uint32_t golden_settings_polaris11_a11[] = {
	mmVM_PRT_APERTURE0_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE1_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE2_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE3_LOW_ADDR, 0x0fffffff, 0x0fffffff
};

static const uint32_t golden_settings_polaris10_a11[] = {
	mmMC_ARB_WTM_GRPWT_RD, 0x00000003, 0x00000000,
	mmVM_PRT_APERTURE0_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE1_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE2_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE3_LOW_ADDR, 0x0fffffff, 0x0fffffff
};

static void gmc_v8_0_init_golden_registers(struct frb_device *adev)
{
	switch (adev->asic_type) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
	case CHIP_VEGAM:
		frb_device_program_register_sequence(adev,
						     golden_settings_polaris11_a11,
						     ARRAY_SIZE(golden_settings_polaris11_a11));
		break;
	case CHIP_POLARIS10:
		frb_device_program_register_sequence(adev,
						     golden_settings_polaris10_a11,
						     ARRAY_SIZE(golden_settings_polaris10_a11));
		break;
	default:
		break;
	}
}

static int gmc_v8_0_wait_for_idle(struct frb_device *adev)
{
	unsigned int i;
	uint32_t tmp;

	for (i = 0; i < adev->usec_timeout; i++) {
		/* read MC_STATUS */
		tmp = RREG32(mmSRBM_STATUS) & (SRBM_STATUS__MCB_BUSY_MASK |
					       SRBM_STATUS__MCB_NON_DISPLAY_BUSY_MASK |
					       SRBM_STATUS__MCC_BUSY_MASK |
					       SRBM_STATUS__MCD_BUSY_MASK |
					       SRBM_STATUS__VMC_BUSY_MASK |
					       SRBM_STATUS__VMC1_BUSY_MASK);
		if (!tmp)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/*
 * gmc_v8_0_init_microcode: which MC image. Polaris 12 reads MC_IO_DEBUG_UP_159
 * (the VBIOS's own MC ucode version, left by ASIC_Init): 0x05b4dc40 means the
 * 32-bit-memory part and polaris12_32_mc.bin. The E9173 reads 0x00b4dc30.
 */
static int gmc_v8_0_init_microcode(struct frb_device *adev)
{
	const char *chip_name;
	uint32_t up159 = 0;
	int err;

	switch (adev->asic_type) {
	case CHIP_POLARIS11:
		chip_name = "polaris11";	/* _k variants not carried */
		break;
	case CHIP_POLARIS10:
		chip_name = "polaris10";
		break;
	case CHIP_POLARIS12:
		if (ASICID_IS_P23(adev->device, adev->revision)) {
			chip_name = "polaris12_k";
		} else {
			WREG32(mmMC_SEQ_IO_DEBUG_INDEX, ixMC_IO_DEBUG_UP_159);
			/* Polaris12 32bit ASIC needs a special MC firmware */
			up159 = RREG32(mmMC_SEQ_IO_DEBUG_DATA);
			if (up159 == 0x05b4dc40)
				chip_name = "polaris12_32";
			else
				chip_name = "polaris12";
		}
		break;
	case CHIP_VEGAM:
		return 0;
	default:
		return -EINVAL;
	}

	frb_fw_name(adev->gmc.fw_name, sizeof(adev->gmc.fw_name), chip_name, "_mc.bin");
	frb_info(adev, "gmc: MC_IO_DEBUG_UP_159 %08x -> %s", up159, adev->gmc.fw_name);
	err = adev->os->firmware(adev->os->cookie, adev->gmc.fw_name,
				 &adev->gmc.fw.data, &adev->gmc.fw.size);
	if (err) {
		frb_err(adev, "mc: Failed to load firmware \"%s\" (%d)", adev->gmc.fw_name, err);
		adev->gmc.fw.data = 0;
		adev->gmc.fw.size = 0;
	}
	return err;
}

/*
 * gmc_v8_0_polaris_mc_load_microcode. The file is little-endian: the I/O
 * register pairs and the program words are read with frb_get_le32 (amdgpu:
 * le32_to_cpup) and reach the registers in CPU order.
 */
static int gmc_v8_0_polaris_mc_load_microcode(struct frb_device *adev)
{
	struct frb_ucode u;
	const uint8_t *fw_data, *io_mc_regs;
	uint32_t data, io_off, io_size;
	int i, ucode_size, regs_size, r;

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

	data = RREG32(mmMC_SEQ_MISC0);
	data &= ~(0x40);
	WREG32(mmMC_SEQ_MISC0, data);

	/* load mc io regs */
	for (i = 0; i < regs_size; i++) {
		WREG32(mmMC_SEQ_IO_DEBUG_INDEX, frb_get_le32(io_mc_regs, 8 * i));
		WREG32(mmMC_SEQ_IO_DEBUG_DATA, frb_get_le32(io_mc_regs, 8 * i + 4));
	}

	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000008);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000010);

	/* load the MC ucode */
	for (i = 0; i < ucode_size; i++)
		WREG32(mmMC_SEQ_SUP_PGM, frb_get_le32(fw_data, 4 * i));

	/* put the engine back into the active state */
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000008);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000004);
	WREG32(mmMC_SEQ_SUP_CNTL, 0x00000001);

	/* wait for training to complete */
	for (i = 0; i < (int)adev->usec_timeout; i++) {
		data = RREG32(mmMC_SEQ_MISC0);
		if (data & 0x80)
			break;
		udelay(1);
	}
	frb_info(adev, "gmc: %s loaded: %d io register pairs, %d program words; "
		 "MC_SEQ_MISC0 %08x after %d polls (bit 7 = trained: %s)",
		 adev->gmc.fw_name, regs_size, ucode_size, data, i,
		 (data & 0x80) ? "yes" : "NO");
	/* amdgpu returns 0 whatever the poll saw; Firebrand refuses to go on */
	return (data & 0x80) ? 0 : -ETIMEDOUT;
}

/* amdgpu_gmc_vram_location / amdgpu_gmc_gart_location (BEST_FIT) / set_agp_default */
static void gmc_v8_0_vram_gtt_location(struct frb_device *adev, struct frb_gmc *mc)
{
	uint64_t base, size_af, size_bf, max_mc_address;

	base = RREG32(mmMC_VM_FB_LOCATION) & 0xFFFF;
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

	/* amdgpu_gmc_gart_location, AMDGPU_GART_PLACEMENT_BEST_FIT */
	max_mc_address = mc->mc_mask < AMDGPU_GMC_HOLE_START - 1 ? mc->mc_mask : AMDGPU_GMC_HOLE_START - 1;
	size_bf = mc->vram_start;
	size_af = max_mc_address + 1 - ((mc->vram_end + 1 + FOUR_GB - 1) & ~(FOUR_GB - 1));
	if (mc->gart_size > (size_bf > size_af ? size_bf : size_af)) {
		frb_info(adev, "limiting GART");
		mc->gart_size = size_bf > size_af ? size_bf : size_af;
	}
	if ((size_bf >= mc->gart_size && size_bf < size_af) || (size_af < mc->gart_size))
		mc->gart_start = 0;
	else
		mc->gart_start = max_mc_address - mc->gart_size + 1;
	mc->gart_start &= ~(FOUR_GB - 1);
	/*
	 * Firebrand: with the accelerator, VMID 0's GART goes below 4 GB, where
	 * RadeonNI had it: its legacy clients get the start as 32 bits
	 * (RDN_UC_GART_INFO; docs/accel-interface.md section 0). Clear of VRAM
	 * (0xF4_0000_0000 on both cards) and of the disabled AGP aperture;
	 * gmc_v8_0_check_addresses checks the overlap again.
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

/* gmc_v8_0_mc_program */
static void gmc_v8_0_mc_program(struct frb_device *adev)
{
	uint32_t tmp;
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

	if (gmc_v8_0_wait_for_idle(adev))
		frb_err(adev, "Wait for MC idle timedout !");

	if (adev->num_crtc) {
		/* Lockout access through VGA aperture*/
		tmp = RREG32(mmVGA_HDP_CONTROL);
		tmp = REG_SET_FIELD(tmp, VGA_HDP_CONTROL, VGA_MEMORY_DISABLE, 1);
		WREG32(mmVGA_HDP_CONTROL, tmp);

		/* disable VGA render */
		tmp = RREG32(mmVGA_RENDER_CONTROL);
		tmp = REG_SET_FIELD(tmp, VGA_RENDER_CONTROL, VGA_VSTATUS_CNTL, 0);
		WREG32(mmVGA_RENDER_CONTROL, tmp);
	}
	/* Update configuration */
	WREG32(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR,
	       (uint32_t)(adev->gmc.vram_start >> 12));
	WREG32(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR,
	       (uint32_t)(adev->gmc.vram_end >> 12));
	WREG32(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR,
	       (uint32_t)(adev->mem_scratch_gpu_addr >> 12));

	WREG32(mmMC_VM_AGP_BASE, 0);
	WREG32(mmMC_VM_AGP_TOP, (uint32_t)(adev->gmc.agp_end >> 22));
	WREG32(mmMC_VM_AGP_BOT, (uint32_t)(adev->gmc.agp_start >> 22));
	if (gmc_v8_0_wait_for_idle(adev))
		frb_err(adev, "Wait for MC idle timedout !");

	WREG32(mmBIF_FB_EN, BIF_FB_EN__FB_READ_EN_MASK | BIF_FB_EN__FB_WRITE_EN_MASK);

	tmp = RREG32(mmHDP_MISC_CNTL);
	tmp = REG_SET_FIELD(tmp, HDP_MISC_CNTL, FLUSH_INVALIDATE_CACHE, 0);
	WREG32(mmHDP_MISC_CNTL, tmp);

	tmp = RREG32(mmHDP_HOST_PATH_CNTL);
	WREG32(mmHDP_HOST_PATH_CNTL, tmp);
}

/* gmc_v8_0_mc_init */
static int gmc_v8_0_mc_init(struct frb_device *adev)
{
	uint32_t tmp;
	int chansize, numchan;

	/*
	 * amdgpu_atombios_get_vram_width() comes first upstream; on this VBIOS
	 * it gives 0 and the capture shows the RAMCFG/CHMAP reads that follow.
	 */
	tmp = RREG32(mmMC_ARB_RAMCFG);
	if (REG_GET_FIELD(tmp, MC_ARB_RAMCFG, CHANSIZE))
		chansize = 64;
	else
		chansize = 32;

	tmp = RREG32(mmMC_SHARED_CHMAP);
	switch (REG_GET_FIELD(tmp, MC_SHARED_CHMAP, NOOFCHAN)) {
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
	tmp = RREG32(mmCONFIG_MEMSIZE);
	/* some boards may have garbage in the upper 16 bits */
	if (tmp & 0xffff0000) {
		frb_info(adev, "Probably bad vram size: 0x%08x", tmp);
		if (tmp & 0xffff)
			tmp &= 0xffff;
	}
	adev->gmc.mc_vram_size = tmp * 1024ULL * 1024ULL;
	adev->gmc.real_vram_size = adev->gmc.mc_vram_size;
	if (!adev->gmc.mc_vram_size)
		return -ENODEV;

	/* no BAR resize on this platform (amdgpu_device_resize_fb_bar) */
	adev->gmc.aper_size = adev->aper_size;
	adev->gmc.visible_vram_size = adev->gmc.aper_size;

	/* set the gart size: POLARIS12, all engines support GPUVM */
	adev->gmc.gart_size = 256ULL << 20;

	gmc_v8_0_vram_gtt_location(adev, &adev->gmc);
	return 0;
}

/* gmc_v8_0_get_vbios_fb_size, for amdgpu_gmc_get_vbios_allocations */
static unsigned int gmc_v8_0_get_vbios_fb_size(struct frb_device *adev)
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
 * Firebrand's placement of amdgpu's VRAM kernel objects: the top 12 MB of
 * the CPU window, in amdgpu's top-down order (see the file header).
 */
static int frb_gmc_layout(struct frb_device *adev)
{
	struct frb_vram_layout *l = &adev->layout;
	uint64_t window = adev->aper_size;

	if (window > adev->gmc.real_vram_size)
		window = adev->gmc.real_vram_size;
	if (window < (16u << 20)) {
		frb_err(adev, "gmc: a %llu KB CPU window is too small for the driver's VRAM",
			(unsigned long long)(window >> 10));
		return -ERANGE;
	}
	l->gart_table_size = 512u << 10;			/* 64K entries x 8 */
	l->gart_table = (uint32_t)window - l->gart_table_size;
	l->mem_scratch = l->gart_table - AMDGPU_GPU_PAGE_SIZE;
	l->toc = l->mem_scratch - 3 * AMDGPU_GPU_PAGE_SIZE;
	l->smu_buffer = (uint32_t)window - (2u << 20);		/* 200 pages, as smu7_init */
	l->smu_buffer_size = 200 * 4096;
	l->fw_buf = (uint32_t)window - (4u << 20);		/* the microcode the SMU loads */
	l->fw_buf_size = 2u << 20;
	l->objects = (uint32_t)window - (8u << 20);		/* frb_ring.c's VRAM objects */
	l->objects_size = 4u << 20;
	/*
	 * UVD's VCPU bo (firmware, heap, stack and 40 sessions: 2.75 MB) and the
	 * kernel's msg bo; amdgpu allocates the VCPU bo top-down in VRAM too
	 * (0xF4_7F80_0000 in the E9173 capture). The CPU writes the firmware.
	 */
	l->uvd = (uint32_t)window - (12u << 20);
	l->uvd_size = 4u << 20;
	l->reserved_start = l->uvd;
	if (l->smu_buffer + l->smu_buffer_size > l->toc) {
		frb_err(adev, "gmc: layout overlap");
		return -ERANGE;
	}
	return 0;
}

static int gmc_v8_0_sw_init(struct frb_device *adev)
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
	 * amdgpu_vm_adjust_size(adev, 64, 9, 1, 40): on this card Linux printed
	 * "vm size is 256 GB, 2 levels, block size is 10-bit, fragment size is
	 * 9-bit"; VM_CONTEXT1_PAGE_TABLE_END_ADDR = max_pfn - 1 = 0x3ffffff.
	 */
	adev->vm_manager.max_pfn = (256ULL << 30) >> AMDGPU_GPU_PAGE_SHIFT;
	adev->vm_manager.block_size = 10;
	adev->vm_manager.fragment_size = 9;
	adev->gmc.mc_mask = 0xffffffffffULL; /* 40 bit MC */

	r = gmc_v8_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "Failed to load mc firmware!");
		return r;
	}

	r = gmc_v8_0_mc_init(adev);
	if (r)
		return r;

	/* amdgpu_gmc_get_vbios_allocations: what the pre-OS display holds */
	vga = gmc_v8_0_get_vbios_fb_size(adev);
	adev->gmc.stolen_vga_size = vga;
	frb_info(adev, "gmc: pre-OS display holds %u KB at the start of VRAM", vga >> 10);

	r = frb_gmc_layout(adev);
	if (r)
		return r;
	adev->mem_scratch_gpu_addr = adev->gmc.vram_start + adev->layout.mem_scratch;
	return frb_gart_init(adev);
}

/*
 * The checks before the first VRAM write and before the GART is switched
 * on (RadeonNI hw/rdn_gart.c scratch_valid and enable_default, and the
 * BAR0 mapping): the window must be the start of VRAM, every object page
 * aligned and inside both, the GART range clear of VRAM, and the dummy page
 * a bus address the GPU can reach.
 */
static int gmc_v8_0_check_addresses(struct frb_device *adev)
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

/*
 * The CPU's first look at VRAM after the MC microcode: patterns through
 * BAR0 over the driver's own region, read back. A write path into one
 * memory chip that drops data (the W7000) shows here.
 */
static int gmc_v8_0_vram_test(struct frb_device *adev)
{
	uint32_t base = adev->layout.fw_buf, n = 64u << 10, off, bad = 0, first = 0, want, got = 0;
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

/* gmc_v8_0_gart_enable */
static int gmc_v8_0_gart_enable(struct frb_device *adev)
{
	uint64_t table_addr;
	uint32_t tmp, field;
	int i;

	if (!adev->gart.ready)
		return -EINVAL;
	table_addr = adev->gart.table_addr;

	/* Setup TLB control */
	tmp = RREG32(mmMC_VM_MX_L1_TLB_CNTL);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_TLB, 1);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_L1_FRAGMENT_PROCESSING, 1);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_ACCESS_MODE, 3);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, ENABLE_ADVANCED_DRIVER_MODEL, 1);
	tmp = REG_SET_FIELD(tmp, MC_VM_MX_L1_TLB_CNTL, SYSTEM_APERTURE_UNMAPPED_ACCESS, 0);
	WREG32(mmMC_VM_MX_L1_TLB_CNTL, tmp);
	/* Setup L2 cache */
	tmp = RREG32(mmVM_L2_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_CACHE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_FRAGMENT_PROCESSING, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_PTE_CACHE_LRU_UPDATE_BY_WRITE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_L2_PDE0_CACHE_LRU_UPDATE_BY_WRITE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, EFFECTIVE_L2_QUEUE_SIZE, 7);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, CONTEXT1_IDENTITY_ACCESS_MODE, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL, ENABLE_DEFAULT_PAGE_OUT_TO_SYSTEM_MEMORY, 1);
	WREG32(mmVM_L2_CNTL, tmp);
	tmp = RREG32(mmVM_L2_CNTL2);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_ALL_L1_TLBS, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL2, INVALIDATE_L2_CACHE, 1);
	WREG32(mmVM_L2_CNTL2, tmp);

	field = adev->vm_manager.fragment_size;
	tmp = RREG32(mmVM_L2_CNTL3);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_ASSOCIATIVITY, 1);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, BANK_SELECT, field);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL3, L2_CACHE_BIGK_FRAGMENT_SIZE, field);
	WREG32(mmVM_L2_CNTL3, tmp);
	/* XXX: set to enable PTE/PDE in system memory */
	tmp = RREG32(mmVM_L2_CNTL4);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_PHYSICAL, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_SHARED, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PDE_REQUEST_SNOOP, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_PHYSICAL, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_SHARED, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT0_PTE_REQUEST_SNOOP, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_PHYSICAL, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_SHARED, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PDE_REQUEST_SNOOP, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_PHYSICAL, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_SHARED, 0);
	tmp = REG_SET_FIELD(tmp, VM_L2_CNTL4, VMC_TAP_CONTEXT1_PTE_REQUEST_SNOOP, 0);
	WREG32(mmVM_L2_CNTL4, tmp);
	/* setup context0 */
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR, (uint32_t)(adev->gmc.gart_start >> 12));
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR, (uint32_t)(adev->gmc.gart_end >> 12));
	WREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR, (uint32_t)(table_addr >> 12));
	WREG32(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR,
	       (uint32_t)(adev->dummy_page_addr >> 12));
	WREG32(mmVM_CONTEXT0_CNTL2, 0);
	tmp = RREG32(mmVM_CONTEXT0_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, ENABLE_CONTEXT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, PAGE_TABLE_DEPTH, 0);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT0_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	WREG32(mmVM_CONTEXT0_CNTL, tmp);

	WREG32(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_LOW_ADDR, 0);
	WREG32(mmVM_L2_CONTEXT1_IDENTITY_APERTURE_HIGH_ADDR, 0);
	WREG32(mmVM_L2_CONTEXT_IDENTITY_PHYSICAL_OFFSET, 0);

	/* empty context1-15 */
	/* set vm size, must be a multiple of 4 */
	WREG32(mmVM_CONTEXT1_PAGE_TABLE_START_ADDR, 0);
	WREG32(mmVM_CONTEXT1_PAGE_TABLE_END_ADDR, (uint32_t)(adev->vm_manager.max_pfn - 1));
	for (i = 1; i < 16; i++) {
		if (i < 8)
			WREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR + i,
			       (uint32_t)(table_addr >> 12));
		else
			WREG32(mmVM_CONTEXT8_PAGE_TABLE_BASE_ADDR + i - 8,
			       (uint32_t)(table_addr >> 12));
	}

	/* enable context1-15 */
	WREG32(mmVM_CONTEXT1_PROTECTION_FAULT_DEFAULT_ADDR,
	       (uint32_t)(adev->dummy_page_addr >> 12));
	WREG32(mmVM_CONTEXT1_CNTL2, 4);
	tmp = RREG32(mmVM_CONTEXT1_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, ENABLE_CONTEXT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_DEPTH, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PAGE_TABLE_BLOCK_SIZE,
			    adev->vm_manager.block_size - 9);
	WREG32(mmVM_CONTEXT1_CNTL, tmp);
	/* gmc_v8_0_set_fault_enable_default(adev, true): amdgpu_vm_fault_stop default */
	tmp = RREG32(mmVM_CONTEXT1_CNTL);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, RANGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, DUMMY_PAGE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, PDE0_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, VALID_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, READ_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, WRITE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	tmp = REG_SET_FIELD(tmp, VM_CONTEXT1_CNTL, EXECUTE_PROTECTION_FAULT_ENABLE_DEFAULT, 1);
	WREG32(mmVM_CONTEXT1_CNTL, tmp);

	/* gmc_v8_0_flush_gpu_tlb(adev, 0, 0, 0): bits 0-15 are the VM contexts0-15 */
	WREG32(mmVM_INVALIDATE_REQUEST, 1 << 0);
	frb_info(adev, "PCIE GART of %uM enabled (table at 0x%016llX).",
		 (unsigned int)(adev->gmc.gart_size >> 20), (unsigned long long)table_addr);
	return 0;
}

/* vi.c vi_flush_hdp (amdgpu_device_flush_hdp) */
static void vi_flush_hdp(struct frb_device *adev)
{
	WREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
	RREG32(mmHDP_MEM_COHERENCY_FLUSH_CNTL);
}

static int gmc_v8_0_hw_init(struct frb_device *adev)
{
	uint32_t page;
	int r;

	gmc_v8_0_init_golden_registers(adev);

	gmc_v8_0_mc_program(adev);

	if (adev->asic_type == CHIP_POLARIS11 ||
	    adev->asic_type == CHIP_POLARIS10 ||
	    adev->asic_type == CHIP_POLARIS12) {
		r = gmc_v8_0_polaris_mc_load_microcode(adev);
		if (r) {
			frb_err(adev, "Failed to load MC firmware!");
			return r;
		}
	}

	/* Firebrand: the checks, the first CPU access to VRAM, the table */
	r = gmc_v8_0_check_addresses(adev);
	if (r)
		return r;
	r = gmc_v8_0_vram_test(adev);
	if (r)
		return r;
	for (page = 0; page < AMDGPU_GPU_PAGE_SIZE; page += 4)
		frb_vram_write32(adev, adev->layout.mem_scratch + page, 0);
	for (page = 0; page < adev->gart.num_gpu_pages; page++)
		frb_gart_set_pte(adev, page, adev->dummy_page_addr, 0);
	vi_flush_hdp(adev);

	return gmc_v8_0_gart_enable(adev);
}

void frb_gmc_v8_0_early_init(struct frb_device *adev)
{
	/* gmc_v8_0_early_init: function pointers and apertures only */
	(void)adev;
}

int frb_gmc_v8_0_sw_init(struct frb_device *adev)
{
	return gmc_v8_0_sw_init(adev);
}

int frb_gmc_v8_0_hw_init(struct frb_device *adev)
{
	int r = gmc_v8_0_hw_init(adev);

	frb_gmc_v8_0_report(adev);
	return r;
}

/* The F2 evidence: what the memory controller says after hw_init. */
void frb_gmc_v8_0_report(struct frb_device *adev)
{
	frb_info(adev, "gmc: %u-bit %s, %llu MB; FB_LOCATION %08x, SYSTEM_APERTURE %08x..%08x "
		 "default %08x, AGP %08x/%08x/%08x, BIF_FB_EN %x, HDP_NONSURFACE_BASE %08x",
		 adev->gmc.vram_width, adev->gmc.vram_type == AMDGPU_VRAM_TYPE_GDDR5 ? "GDDR5" : "memory",
		 (unsigned long long)(adev->gmc.mc_vram_size >> 20),
		 RREG32(mmMC_VM_FB_LOCATION), RREG32(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR),
		 RREG32(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR), RREG32(mmMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR),
		 RREG32(mmMC_VM_AGP_BASE), RREG32(mmMC_VM_AGP_TOP), RREG32(mmMC_VM_AGP_BOT),
		 RREG32(mmBIF_FB_EN), RREG32(mmHDP_NONSURFACE_BASE));
	frb_info(adev, "gmc: MC_SEQ_MISC0 %08x, MC_SEQ_SUP_CNTL %08x, MC_ARB_RAMCFG %08x, "
		 "MC_SHARED_CHMAP %08x, ucode %s version 0x%08x",
		 RREG32(mmMC_SEQ_MISC0), RREG32(mmMC_SEQ_SUP_CNTL), RREG32(mmMC_ARB_RAMCFG),
		 RREG32(mmMC_SHARED_CHMAP), adev->gmc.fw_name, adev->gmc.fw_version);
	frb_info(adev, "gmc: VM_CONTEXT0 %08x..%08x table %08x cntl %08x, VM_L2_CNTL %08x/%08x/%08x, "
		 "MX_L1_TLB %08x, fault default %08x",
		 RREG32(mmVM_CONTEXT0_PAGE_TABLE_START_ADDR), RREG32(mmVM_CONTEXT0_PAGE_TABLE_END_ADDR),
		 RREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR), RREG32(mmVM_CONTEXT0_CNTL),
		 RREG32(mmVM_L2_CNTL), RREG32(mmVM_L2_CNTL2), RREG32(mmVM_L2_CNTL3),
		 RREG32(mmMC_VM_MX_L1_TLB_CNTL), RREG32(mmVM_CONTEXT0_PROTECTION_FAULT_DEFAULT_ADDR));
}
