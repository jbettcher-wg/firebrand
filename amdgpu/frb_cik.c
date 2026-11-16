/*
 * cik.c: the CIK common block, ported from Linux 7.0.2 (amdgpu): register
 * windows (SMC index/data 0, PCIE index 0xe), rev ids and the clock/power
 * gating flags (cik_common_early_init), the golden registers, the PCI config
 * reset, cik_common_hw_init, cik_srbm_select, cik_read_register's allowlist
 * (over gfx_v7_0's cached copies), and stage sdma with amdgpu's late-init
 * clock gating (cik_sdma_stage). Only Hawaii has its golden tables here: the
 * family table has a row for the FirePro W8100 and no other CIK card, and a
 * chip with no row never gets past the survey.
 *
 * Where Firebrand differs from amdgpu (as frb_si.c does on SI):
 * - pre_asic_init (amdgpu: none on CIK) is RadeonNI's measured requirement
 *   on this card (RadeonNI.cpp bringUp): the
 *   W8100's ASIC_Init calls SetEngineClock (ROM 0xBB9E), which reads the
 *   SMU's "I own the engine clock" flag at SMC 0xC0200000 through
 *   SMC_IND_INDEX_1 and otherwise programs the SPLL through the same pair;
 *   with the SMC's clock disabled every SMC-space register reads 0 and it
 *   spins on CG_SPLL_STATUS until the interpreter gives up. On a PC the
 *   x86 POST has started the SMU before; here nothing has. So, before a
 *   POST: log the SMC's control registers and clear CK_DISABLE, as RadeonNI
 *   does on every unposted entry.
 * - cik_pcie_gen3_enable reads the link and changes nothing; cik_program_aspm
 *   is skipped (amdgpu_device_should_use_aspm() is false here), as on SI/VI.
 * - need_reset_on_init: amdgpu's (the SMC is running firmware), and, as on
 *   SI, an earlier driver's state in this power cycle: a running CP, or the
 *   memory controller's FB aperture not covering CONFIG_MEMSIZE (RadeonNI
 *   moves VRAM to 0 and maps the first 1 GB of the W8100's 8, FB_LOCATION
 *   0x003f0000; a warm reboot keeps it).
 *
 * The golden tables are amdgpu's verbatim; RadeonNI wrote radeon's (the
 * same values, byte offsets) on this card and logged how many read back as
 * written right after each write ("36 of 36, rlc/common 3 of 5, clock
 * gating 94 of 107, pg/spm 0 of 1"); cik_program_and_count counts the same
 * way so the two logs compare.
 *
 * Copyright 2012 Advanced Micro Devices, Inc.
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
#include "frb_display.h"
#include "frb_ucode.h"
#include "include/cikd.h"
#include "include/asic_reg/smu/smu_7_0_1_d.h"
#include "include/asic_reg/smu/smu_7_0_1_sh_mask.h"
#include "include/asic_reg/bif/bif_4_1_d.h"
#include "include/asic_reg/bif/bif_4_1_sh_mask.h"
#include "include/asic_reg/gca/gfx_7_2_d.h"
#include "include/asic_reg/gca/gfx_7_2_sh_mask.h"
#include "include/asic_reg/gmc/gmc_7_1_d.h"
#include "include/asic_reg/gmc/gmc_7_1_sh_mask.h"
#include "include/asic_reg/oss/oss_2_0_d.h"
#include "include/asic_reg/oss/oss_2_0_sh_mask.h"
#include "include/asic_reg/dce/dce_8_0_d.h"

#define AMDGPU_ASIC_RESET_DATA	0x39d5e86b	/* amdgpu.h */
#define PCI_COMMAND		0x04
#define PCI_COMMAND_MASTER	0x4

/* RadeonNI hw/rdn_cik_smc.c: the second index/data pair, which the VBIOS's tables use */
#define CIK_SMC_IND_INDEX_1	0x82
#define CIK_SMC_IND_DATA_1	0x83
#define CIK_SMU_SCLK_OWNER	0xC0200000	/* the flag SetEngineClock tests (bit 0) */
#define CIK_CG_SPLL_FUNC_CNTL	0xC0500140	/* ixCG_SPLL_FUNC_CNTL */
#define CIK_CG_SPLL_STATUS	0xC0500150	/* ixCG_SPLL_STATUS */

static const uint32_t hawaii_golden_spm_registers[] =
{
	0xc200, 0xe0ffffff, 0xe0000000
};

static const uint32_t hawaii_golden_common_registers[] =
{
	0xc200, 0xffffffff, 0xe0000000,
	0xa0d4, 0xffffffff, 0x3a00161a,
	0xa0d5, 0xffffffff, 0x0000002e,
	0x2684, 0xffffffff, 0x00018208,
	0x263e, 0xffffffff, 0x12011003
};

static const uint32_t hawaii_golden_registers[] =
{
	0xcd5, 0x00000333, 0x00000333,
	0x2684, 0x00010000, 0x00058208,
	0x260c, 0xffffffff, 0x00000000,
	0x260d, 0xf00fffff, 0x00000400,
	0x260e, 0x0002021c, 0x00020200,
	0x31e, 0x00000080, 0x00000000,
	0x16ec, 0x000000f0, 0x00000070,
	0x16f0, 0xf0311fff, 0x80300000,
	0xd43, 0x00810000, 0x408af000,
	0x1c0c, 0x31000111, 0x00000011,
	0xbd2, 0x73773777, 0x12010001,
	0x848, 0x0000007f, 0x0000001b,
	0x877, 0x00007fb6, 0x00002191,
	0xd8a, 0x0000003f, 0x0000000a,
	0xd8b, 0x0000003f, 0x0000000a,
	0xab9, 0x00073ffe, 0x000022a2,
	0x903, 0x000007ff, 0x00000000,
	0x22fc, 0x00002001, 0x00000001,
	0x22c9, 0xffffffff, 0x00ffffff,
	0xc281, 0x0000ff0f, 0x00000000,
	0xa293, 0x07ffffff, 0x06000000,
	0xf9e, 0x00000001, 0x00000002,
	0x31da, 0x00000008, 0x00000008,
	0x31dc, 0x00000f00, 0x00000800,
	0x31dd, 0x00000f00, 0x00000800,
	0x31e6, 0x00ffffff, 0x00ff7fbf,
	0x31e7, 0x00ffffff, 0x00ff7faf,
	0x2300, 0x000000ff, 0x00000800,
	0x390, 0x00001fff, 0x00001fff,
	0x2418, 0x0000007f, 0x00000020,
	0x2542, 0x00010000, 0x00010000,
	0x2b80, 0x00100000, 0x000ff07c,
	0x2b05, 0x000003ff, 0x0000000f,
	0x2b04, 0xffffffff, 0x7564fdec,
	0x2b03, 0xffffffff, 0x3120b9a8,
	0x2b02, 0x20000000, 0x0f9c0000
};

static const uint32_t hawaii_mgcg_cgcg_init[] =
{
	0x3108, 0xffffffff, 0xfffffffd,
	0xc200, 0xffffffff, 0xe0000000,
	0xf0a8, 0xffffffff, 0x00000100,
	0xf082, 0xffffffff, 0x00000100,
	0xf0b0, 0xffffffff, 0x00000100,
	0xf0b2, 0xffffffff, 0x00000100,
	0xf0b1, 0xffffffff, 0x00000100,
	0x1579, 0xffffffff, 0x00200100,
	0xf0a0, 0xffffffff, 0x00000100,
	0xf085, 0xffffffff, 0x06000100,
	0xf088, 0xffffffff, 0x00000100,
	0xf086, 0xffffffff, 0x06000100,
	0xf081, 0xffffffff, 0x00000100,
	0xf0b8, 0xffffffff, 0x00000100,
	0xf089, 0xffffffff, 0x00000100,
	0xf080, 0xffffffff, 0x00000100,
	0xf08c, 0xffffffff, 0x00000100,
	0xf08d, 0xffffffff, 0x00000100,
	0xf094, 0xffffffff, 0x00000100,
	0xf095, 0xffffffff, 0x00000100,
	0xf096, 0xffffffff, 0x00000100,
	0xf097, 0xffffffff, 0x00000100,
	0xf098, 0xffffffff, 0x00000100,
	0xf09f, 0xffffffff, 0x00000100,
	0xf09e, 0xffffffff, 0x00000100,
	0xf084, 0xffffffff, 0x06000100,
	0xf0a4, 0xffffffff, 0x00000100,
	0xf09d, 0xffffffff, 0x00000100,
	0xf0ad, 0xffffffff, 0x00000100,
	0xf0ac, 0xffffffff, 0x00000100,
	0xf09c, 0xffffffff, 0x00000100,
	0xc200, 0xffffffff, 0xe0000000,
	0xf008, 0xffffffff, 0x00010000,
	0xf009, 0xffffffff, 0x00030002,
	0xf00a, 0xffffffff, 0x00040007,
	0xf00b, 0xffffffff, 0x00060005,
	0xf00c, 0xffffffff, 0x00090008,
	0xf00d, 0xffffffff, 0x00010000,
	0xf00e, 0xffffffff, 0x00030002,
	0xf00f, 0xffffffff, 0x00040007,
	0xf010, 0xffffffff, 0x00060005,
	0xf011, 0xffffffff, 0x00090008,
	0xf012, 0xffffffff, 0x00010000,
	0xf013, 0xffffffff, 0x00030002,
	0xf014, 0xffffffff, 0x00040007,
	0xf015, 0xffffffff, 0x00060005,
	0xf016, 0xffffffff, 0x00090008,
	0xf017, 0xffffffff, 0x00010000,
	0xf018, 0xffffffff, 0x00030002,
	0xf019, 0xffffffff, 0x00040007,
	0xf01a, 0xffffffff, 0x00060005,
	0xf01b, 0xffffffff, 0x00090008,
	0xf01c, 0xffffffff, 0x00010000,
	0xf01d, 0xffffffff, 0x00030002,
	0xf01e, 0xffffffff, 0x00040007,
	0xf01f, 0xffffffff, 0x00060005,
	0xf020, 0xffffffff, 0x00090008,
	0xf021, 0xffffffff, 0x00010000,
	0xf022, 0xffffffff, 0x00030002,
	0xf023, 0xffffffff, 0x00040007,
	0xf024, 0xffffffff, 0x00060005,
	0xf025, 0xffffffff, 0x00090008,
	0xf026, 0xffffffff, 0x00010000,
	0xf027, 0xffffffff, 0x00030002,
	0xf028, 0xffffffff, 0x00040007,
	0xf029, 0xffffffff, 0x00060005,
	0xf02a, 0xffffffff, 0x00090008,
	0xf02b, 0xffffffff, 0x00010000,
	0xf02c, 0xffffffff, 0x00030002,
	0xf02d, 0xffffffff, 0x00040007,
	0xf02e, 0xffffffff, 0x00060005,
	0xf02f, 0xffffffff, 0x00090008,
	0xf030, 0xffffffff, 0x00010000,
	0xf031, 0xffffffff, 0x00030002,
	0xf032, 0xffffffff, 0x00040007,
	0xf033, 0xffffffff, 0x00060005,
	0xf034, 0xffffffff, 0x00090008,
	0xf035, 0xffffffff, 0x00010000,
	0xf036, 0xffffffff, 0x00030002,
	0xf037, 0xffffffff, 0x00040007,
	0xf038, 0xffffffff, 0x00060005,
	0xf039, 0xffffffff, 0x00090008,
	0xf03a, 0xffffffff, 0x00010000,
	0xf03b, 0xffffffff, 0x00030002,
	0xf03c, 0xffffffff, 0x00040007,
	0xf03d, 0xffffffff, 0x00060005,
	0xf03e, 0xffffffff, 0x00090008,
	0x30c6, 0xffffffff, 0x00020200,
	0xcd4, 0xffffffff, 0x00000200,
	0x570, 0xffffffff, 0x00000400,
	0x157a, 0xffffffff, 0x00000000,
	0xbd4, 0xffffffff, 0x00000902,
	0xf000, 0xffffffff, 0x96940200,
	0x21c2, 0xffffffff, 0x00900100,
	0x3109, 0xffffffff, 0x0020003f,
	0xe, 0xffffffff, 0x0140001c,
	0xf, 0x000f0000, 0x000f0000,
	0x88, 0xffffffff, 0xc060000c,
	0x89, 0xc0000fff, 0x00000100,
	0x3e4, 0xffffffff, 0x00000100,
	0x3e6, 0x00000101, 0x00000000,
	0x82a, 0xffffffff, 0x00000104,
	0x1579, 0xff000fff, 0x00000100,
	0xc33, 0xc0000fff, 0x00000104,
	0x3079, 0x00000001, 0x00000001,
	0x3403, 0xff000ff0, 0x00000100,
	0x3603, 0xff000ff0, 0x00000100
};

/* --- register windows (cik_smc_rreg, cik_pcie_rreg) ------------------------- */

static uint32_t cik_smc_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(mmSMC_IND_INDEX_0, (reg));
	return RREG32(mmSMC_IND_DATA_0);
}

static void cik_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(mmSMC_IND_INDEX_0, (reg));
	WREG32(mmSMC_IND_DATA_0, (v));
}

/* the VBIOS's own pair (SMC_IND_INDEX_1), for the log only: what SetEngineClock sees */
static uint32_t cik_smc_rreg1(struct frb_device *adev, uint32_t reg)
{
	WREG32(CIK_SMC_IND_INDEX_1, (reg));
	return RREG32(CIK_SMC_IND_DATA_1);
}

/*
 * cik_srbm_select: switches the instance of the registers instanced per VMID
 * or per ME/pipe/queue (gfx_v7_0's SH_MEM loop, cik_sdma's GFX_VIRTUAL_ADDR
 * and APE1_CNTL loop). The caller holds the driver's lock (Linux: srbm_mutex).
 */
void frb_cik_srbm_select(struct frb_device *adev, uint32_t me, uint32_t pipe, uint32_t queue, uint32_t vmid)
{
	uint32_t srbm_gfx_cntl =
		(((pipe << SRBM_GFX_CNTL__PIPEID__SHIFT) & SRBM_GFX_CNTL__PIPEID_MASK)|
		((me << SRBM_GFX_CNTL__MEID__SHIFT) & SRBM_GFX_CNTL__MEID_MASK)|
		((vmid << SRBM_GFX_CNTL__VMID__SHIFT) & SRBM_GFX_CNTL__VMID_MASK)|
		((queue << SRBM_GFX_CNTL__QUEUEID__SHIFT) & SRBM_GFX_CNTL__QUEUEID_MASK));
	WREG32(mmSRBM_GFX_CNTL, srbm_gfx_cntl);
}

/* --- cik_common_early_init: reads only (the survey runs it) ----------------- */

static uint32_t cik_get_rev_id(struct frb_device *adev)
{
	return (RREG32(mmCC_DRM_ID_STRAPS) & CC_DRM_ID_STRAPS__ATI_REV_ID_MASK)
		>> CC_DRM_ID_STRAPS__ATI_REV_ID__SHIFT;
}

static void cik_common_early_init(struct frb_device *adev)
{
	adev->rev_id = cik_get_rev_id(adev);
	adev->external_rev_id = 0xFF;
	adev->family = AMDGPU_FAMILY_CI;
	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CGTS_LS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_MC_LS |
			AMD_CG_SUPPORT_MC_MGCG |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = adev->rev_id + 0x14;
		break;
	case CHIP_HAWAII:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			/*AMD_CG_SUPPORT_GFX_CGCG |*/
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_MC_LS |
			AMD_CG_SUPPORT_MC_MGCG |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = 0x28;
		break;
	default:
		break;
	}
	frb_info(adev, "cik: rev_id %u, external_rev_id 0x%x, cg_flags 0x%llx", adev->rev_id,
		 adev->external_rev_id, (unsigned long long)adev->cg_flags);
}

/* --- cik_init_golden_registers ---------------------------------------------- */

/*
 * amdgpu_device_program_register_sequence, with RadeonNI's count of the
 * entries that read back as written right after their write
 * (rdn_si_program_registers): a fingerprint to compare with RadeonNI's log
 * on the same card, not a pass/fail (some of these are write-only, indexed
 * or self-clearing).
 */
static uint32_t cik_program_and_count(struct frb_device *adev, const uint32_t *regs, uint32_t n)
{
	uint32_t i, ok = 0;

	if (n % 3)
		return 0;
	for (i = 0; i < n; i += 3) {
		uint32_t reg = regs[i], and_mask = regs[i + 1], or_mask = regs[i + 2], tmp;

		if (and_mask == 0xffffffff) {
			tmp = or_mask;
		} else {
			tmp = RREG32(reg);
			tmp &= ~and_mask;
			tmp |= or_mask;
		}
		WREG32(reg, tmp);
		if (RREG32(reg) == tmp)
			ok++;
	}
	return ok;
}

static int cik_init_golden_registers(struct frb_device *adev)
{
	uint32_t a, b, c, d;

	switch (adev->asic_type) {
	case CHIP_HAWAII:
		/* mgcg_cgcg_init, golden, golden_common, golden_spm: amdgpu's order */
		c = cik_program_and_count(adev, hawaii_mgcg_cgcg_init, ARRAY_SIZE(hawaii_mgcg_cgcg_init));
		a = cik_program_and_count(adev, hawaii_golden_registers, ARRAY_SIZE(hawaii_golden_registers));
		b = cik_program_and_count(adev, hawaii_golden_common_registers,
					  ARRAY_SIZE(hawaii_golden_common_registers));
		d = cik_program_and_count(adev, hawaii_golden_spm_registers, ARRAY_SIZE(hawaii_golden_spm_registers));
		frb_info(adev, "cik: Hawaii golden registers %u of %u, common %u of %u, mgcg/cgcg %u of %u, "
			 "spm %u of %u read back as written (RadeonNI on this card: 36/36, 3/5, 94/107, 0/1)",
			 a, (uint32_t)ARRAY_SIZE(hawaii_golden_registers) / 3,
			 b, (uint32_t)ARRAY_SIZE(hawaii_golden_common_registers) / 3,
			 c, (uint32_t)ARRAY_SIZE(hawaii_mgcg_cgcg_init) / 3,
			 d, (uint32_t)ARRAY_SIZE(hawaii_golden_spm_registers) / 3);
		return 0;
	default:
		frb_err(adev, "cik: no golden registers for this chip here (only Hawaii has a row)");
		return -ENODEV;
	}
}

/* --- PCIe: cik_pcie_gen3_enable's reads (see the header) -------------------- */

static void cik_pcie_report(struct frb_device *adev)
{
	uint32_t speed_cntl = RREG32_PCIE(ixPCIE_LC_SPEED_CNTL);
	uint32_t width_cntl = RREG32_PCIE(ixPCIE_LC_LINK_WIDTH_CNTL);
	uint32_t rate = (speed_cntl & PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE_MASK) >>
		PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE__SHIFT;
	static const uint8_t lanes[8] = { 0, 1, 2, 4, 8, 12, 16, 0 };

	frb_info(adev, "cik: PCIe link at gen %u x%u (LC_SPEED_CNTL %08x, LC_LINK_WIDTH_CNTL %08x); "
		 "speed and ASPM left as they are", rate + 1,
		 lanes[(width_cntl & PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD_MASK) >>
		       PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD__SHIFT], speed_cntl, width_cntl);
}

/* cik_common_hw_init */
static int cik_common_hw_init(struct frb_device *adev)
{
	int r;

	/* move the golden regs per IP block */
	r = cik_init_golden_registers(adev);
	if (r)
		return r;
	/* enable pcie gen2/3 link: read and left (see the header) */
	cik_pcie_report(adev);
	/* cik_program_aspm: amdgpu_device_should_use_aspm() is false here */
	return 0;
}

static uint32_t cik_get_config_memsize(struct frb_device *adev)
{
	return RREG32(mmCONFIG_MEMSIZE);
}

/* the SMC's control registers, as RadeonNI's rdn_cik_smc_probe logs them */
static void cik_smc_report(struct frb_device *adev, const char *when)
{
	frb_info(adev, "cik: SMC %s: RESET_CNTL %08x CLOCK_CNTL_0 %08x MISC_CNTL %08x PC_C %08x FIRMWARE_FLAGS %08x; "
		 "0xC0200000 %08x (pair 1 %08x), CG_SPLL_FUNC_CNTL %08x, CG_SPLL_STATUS %08x; ACCESS_CNTL %08x", when,
		 RREG32_SMC(ixSMC_SYSCON_RESET_CNTL), RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0),
		 RREG32_SMC(ixSMC_SYSCON_MISC_CNTL), RREG32_SMC(ixSMC_PC_C), RREG32_SMC(ixFIRMWARE_FLAGS),
		 RREG32_SMC(CIK_SMU_SCLK_OWNER), cik_smc_rreg1(adev, CIK_SMU_SCLK_OWNER),
		 RREG32_SMC(CIK_CG_SPLL_FUNC_CNTL), RREG32_SMC(CIK_CG_SPLL_STATUS), RREG32(mmSMC_IND_ACCESS_CNTL));
}

/*
 * cik_need_reset_on_init (amdgpu: the SMC running its firmware), and an
 * earlier driver's state in this power cycle (see the header): a running
 * CP, or an FB aperture that does not cover CONFIG_MEMSIZE.
 */
static int cik_need_reset_on_init(struct frb_device *adev)
{
	uint32_t clock_cntl, pc, cp, fb, memsize, fb_mb;
	int smc, cp_running, fb_moved;

	/* check if the SMC is already running */
	clock_cntl = RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0);
	pc = RREG32_SMC(ixSMC_PC_C);
	smc = (0 == REG_GET_FIELD(clock_cntl, SMC_SYSCON_CLOCK_CNTL_0, ck_disable)) && (0x20100 <= pc);

	cp = RREG32(mmCP_ME_CNTL);
	cp_running = !(cp & CP_ME_CNTL__ME_HALT_MASK);
	fb = RREG32(mmMC_VM_FB_LOCATION);
	memsize = RREG32(mmCONFIG_MEMSIZE);
	fb_mb = ((((fb >> 16) & 0xffff) - (fb & 0xffff)) + 1) << 4;
	/* an unposted card reads CONFIG_MEMSIZE 0: nothing to compare with yet */
	fb_moved = memsize && memsize != 0xffffffff && fb_mb != memsize;
	frb_info(adev, "cik: SMC CLOCK_CNTL_0 %08x PC %08x (%s); CP_ME_CNTL %08x (%s); MC_VM_FB_LOCATION %08x "
		 "(%u MB) for CONFIG_MEMSIZE %u MB%s; BIOS_SCRATCH_7 %08x", clock_cntl, pc,
		 smc ? "running firmware" : "not running firmware", cp,
		 cp_running ? "RUNNING: an earlier driver" : "halted, as the VBIOS leaves it", fb, fb_mb, memsize,
		 fb_moved ? " (MOVED: an earlier driver)" : "", RREG32(mmBIOS_SCRATCH_7));
	return smc || cp_running || fb_moved;
}

/* --- cik_asic_reset: cik_asic_pci_config_reset (no DPM, so not BACO) -------- */

static int cik_asic_reset(struct frb_device *adev)
{
	uint32_t i, cmd;
	int r = -EINVAL;

	frb_info(adev, "reset: PCI CONFIG reset");
	frb_atombios_scratch_regs_engine_hung(adev, 1);

	/* disable BM */
	cmd = adev->os->cfg_read32(adev->os->cookie, PCI_COMMAND) & 0xffff;
	if (cmd & PCI_COMMAND_MASTER)
		adev->os->cfg_write32(adev->os->cookie, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);
	/* reset (amdgpu_device_pci_config_reset) */
	adev->os->cfg_write32(adev->os->cookie, 0x7c, AMDGPU_ASIC_RESET_DATA);

	udelay(100);

	/* wait for asic to come out of reset */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmCONFIG_MEMSIZE) != 0xffffffff) {
			/* enable BM (only if it was: frb_vi.c) */
			if (cmd & PCI_COMMAND_MASTER)
				adev->os->cfg_write32(adev->os->cookie, PCI_COMMAND, cmd);
			adev->has_hw_reset = 1;
			r = 0;
			break;
		}
		udelay(1);
	}
	frb_info(adev, "reset: PCI CONFIG reset: %s after %u us", r ? "the ASIC did not come back" : "back", 100 + i);
	frb_atombios_scratch_regs_engine_hung(adev, 0);
	if (!r)
		adev->gpu_reset_counter++;
	return r;
}

/*
 * Before ASIC_Init on an unposted card: the SMC's clock on (see the header;
 * RadeonNI.cpp bringUp, "with the SMC clock on"). RadeonNI's second step,
 * starting the SMU firmware and posting again when ASIC_Init still fails,
 * is not carried: stage post fails and says so.
 */
static void cik_pre_asic_init(struct frb_device *adev)
{
	uint32_t tmp;

	cik_smc_report(adev, "before POST");
	tmp = RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0);
	if (tmp & SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK) {
		WREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0, tmp & ~SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK);
		cik_smc_report(adev, "with the SMC clock on");
	} else {
		frb_info(adev, "cik: the SMC's clock is already on (CK_DISABLE clear)");
	}
}

/* --- gfx_v7_0_gpu_early_init / constants_init's address configuration ------- */

#define BONAIRE_GB_ADDR_CONFIG_GOLDEN	0x12010001	/* gfx_v7_0.c */
#define HAWAII_GB_ADDR_CONFIG_GOLDEN	0x12011003

/*
 * GB_ADDR_CONFIG and the copies the CPU's HDP window and the display's
 * DMIF keep (gfx_v7_0_constants_init: GB_ADDR_CONFIG, HDP_ADDR_CONFIG,
 * DMIF_ADDR_CALC; cik_sdma and uvd_v4_2 write theirs in their own
 * hw_init). As on SI, stage dce runs it before the display (Firebrand's
 * stage order puts dce before gfx; amdgpu's IP order has gfx first). The
 * row size from MC_ARB_RAMCFG (the dGPU arm of gfx_v7_0_gpu_early_init).
 * RadeonNI on this card: 0x12011003, "row 2 KB from RAMCFG 0x00007262",
 * the VBIOS leaving GB 0x10000000 DMIF 0x00011003 HDP 0x12010001.
 */
int frb_cik_gfx_addr_config(struct frb_device *adev)
{
	uint32_t gb_addr_config, mc_arb_ramcfg, tmp, row_kb, before[4];

	switch (adev->asic_type) {
	case CHIP_HAWAII:
		gb_addr_config = HAWAII_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_BONAIRE:
		gb_addr_config = BONAIRE_GB_ADDR_CONFIG_GOLDEN;
		break;
	default:
		frb_err(adev, "cik: no address configuration for this chip here");
		return -ENODEV;
	}
	before[0] = RREG32(mmGB_ADDR_CONFIG);
	before[1] = RREG32(mmDMIF_ADDR_CONFIG);
	before[2] = RREG32(mmDMIF_ADDR_CALC);
	before[3] = RREG32(mmHDP_ADDR_CONFIG);

	mc_arb_ramcfg = RREG32(mmMC_ARB_RAMCFG);
	tmp = (mc_arb_ramcfg & MC_ARB_RAMCFG__NOOFCOLS_MASK) >> MC_ARB_RAMCFG__NOOFCOLS__SHIFT;
	row_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (row_kb > 4)
		row_kb = 4;
	/* fix up row size */
	gb_addr_config &= ~GB_ADDR_CONFIG__ROW_SIZE_MASK;
	switch (row_kb) {
	case 1:
	default:
		gb_addr_config |= (0 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	case 2:
		gb_addr_config |= (1 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	case 4:
		gb_addr_config |= (2 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	}
	adev->cik_gb_addr_config = gb_addr_config;

	WREG32(mmGRBM_CNTL, (0xff << GRBM_CNTL__READ_TIMEOUT__SHIFT));
	WREG32(mmGB_ADDR_CONFIG, gb_addr_config);
	WREG32(mmHDP_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMIF_ADDR_CALC, gb_addr_config);
	frb_info(adev, "cik: address config %08x (rows %u KB from MC_ARB_RAMCFG %08x); the VBIOS had GB %08x "
		 "DMIF %08x CALC %08x HDP %08x", gb_addr_config, row_kb, mc_arb_ramcfg,
		 before[0], before[1], before[2], before[3]);
	return 0;
}

/* --- stage smu on CIK -------------------------------------------------------- */

/*
 * On CIK, as on SI, the SMC has no start of its own before DPM: amdgpu's
 * powerplay (ci_smumgr's ci_start_smu) and radeon's ci_dpm_enable upload the
 * image (ci_upload_firmware) and start it with its tables, which is the DPM
 * step after stage sdma. Stage smu picks the image the way amdgpu does
 * (hawaii_k_smc.bin for rev 0x80, hawaii_smc.bin otherwise; Bonaire's
 * kicker list), checks it, and reports whether the SMC runs firmware
 * (RadeonNI after POST: the VBIOS's own code, PC_C around 0x3180-0x3258,
 * ci_is_smc_running says no). Reads and the firmware file only.
 */
int frb_cik_smc_stage(struct frb_device *adev)
{
	const char *chip_name;
	struct frb_fw fw = { 0, 0 };
	struct frb_ucode u;
	char name[32];
	uint32_t start;
	int r;

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		if ((adev->revision == 0x80) || (adev->revision == 0x81) || (adev->device == 0x665f))
			chip_name = "bonaire_k";
		else
			chip_name = "bonaire";
		break;
	case CHIP_HAWAII:
		if (adev->revision == 0x80)
			chip_name = "hawaii_k";
		else
			chip_name = "hawaii";
		break;
	default:
		frb_err(adev, "cik: no SMC image choice for this chip here");
		return -ENODEV;
	}
	frb_fw_name(name, sizeof(name), chip_name, "_smc.bin");
	r = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (r) {
		frb_err(adev, "cik_smc: Failed to load firmware. err = %d \"%s\"", r, name);
		return r;
	}
	r = frb_ucode_parse(adev, name, &fw, &u);
	if (r)
		return r;
	start = frb_ucode_le32(&u, FRB_FWH_SMC_UCODE_START_ADDR);
	frb_info(adev, "cik: SMC image %s, %u bytes of microcode at SMC 0x%x (version 0x%08x); it is loaded and "
		 "started by DPM with its tables", name, u.ucode_size_bytes, start, u.ucode_version);
	cik_smc_report(adev, "at stage smu");
	return 0;
}

/* cik_get_xclk (cik.c): the reference clock (10 kHz); not an APU */
uint32_t frb_cik_get_xclk(struct frb_device *adev)
{
	uint32_t reference_clock = adev->spll_reference_freq;

	if (RREG32_SMC(ixCG_CLKPIN_CNTL) & CG_CLKPIN_CNTL__XTALIN_DIVIDE_MASK)
		return reference_clock / 4;
	return reference_clock;
}

/* --- cik_read_register (cik.c): what user space may read ------------------------ */

static const struct { uint32_t reg_offset; int grbm_indexed; } cik_allowed_read_registers[] = {
	{mmGRBM_STATUS},
	{mmGRBM_STATUS2},
	{mmGRBM_STATUS_SE0},
	{mmGRBM_STATUS_SE1},
	{mmGRBM_STATUS_SE2},
	{mmGRBM_STATUS_SE3},
	{mmSRBM_STATUS},
	{mmSRBM_STATUS2},
	{mmSDMA0_STATUS_REG + SDMA0_REGISTER_OFFSET},
	{mmSDMA0_STATUS_REG + SDMA1_REGISTER_OFFSET},
	{mmCP_STAT},
	{mmCP_STALLED_STAT1},
	{mmCP_STALLED_STAT2},
	{mmCP_STALLED_STAT3},
	{mmCP_CPF_BUSY_STAT},
	{mmCP_CPF_STALLED_STAT1},
	{mmCP_CPF_STATUS},
	{mmCP_CPC_BUSY_STAT},
	{mmCP_CPC_STALLED_STAT1},
	{mmCP_CPC_STATUS},
	{mmGB_ADDR_CONFIG},
	{mmMC_ARB_RAMCFG},
	{mmGB_TILE_MODE0},
	{mmGB_TILE_MODE1},
	{mmGB_TILE_MODE2},
	{mmGB_TILE_MODE3},
	{mmGB_TILE_MODE4},
	{mmGB_TILE_MODE5},
	{mmGB_TILE_MODE6},
	{mmGB_TILE_MODE7},
	{mmGB_TILE_MODE8},
	{mmGB_TILE_MODE9},
	{mmGB_TILE_MODE10},
	{mmGB_TILE_MODE11},
	{mmGB_TILE_MODE12},
	{mmGB_TILE_MODE13},
	{mmGB_TILE_MODE14},
	{mmGB_TILE_MODE15},
	{mmGB_TILE_MODE16},
	{mmGB_TILE_MODE17},
	{mmGB_TILE_MODE18},
	{mmGB_TILE_MODE19},
	{mmGB_TILE_MODE20},
	{mmGB_TILE_MODE21},
	{mmGB_TILE_MODE22},
	{mmGB_TILE_MODE23},
	{mmGB_TILE_MODE24},
	{mmGB_TILE_MODE25},
	{mmGB_TILE_MODE26},
	{mmGB_TILE_MODE27},
	{mmGB_TILE_MODE28},
	{mmGB_TILE_MODE29},
	{mmGB_TILE_MODE30},
	{mmGB_TILE_MODE31},
	{mmGB_MACROTILE_MODE0},
	{mmGB_MACROTILE_MODE1},
	{mmGB_MACROTILE_MODE2},
	{mmGB_MACROTILE_MODE3},
	{mmGB_MACROTILE_MODE4},
	{mmGB_MACROTILE_MODE5},
	{mmGB_MACROTILE_MODE6},
	{mmGB_MACROTILE_MODE7},
	{mmGB_MACROTILE_MODE8},
	{mmGB_MACROTILE_MODE9},
	{mmGB_MACROTILE_MODE10},
	{mmGB_MACROTILE_MODE11},
	{mmGB_MACROTILE_MODE12},
	{mmGB_MACROTILE_MODE13},
	{mmGB_MACROTILE_MODE14},
	{mmGB_MACROTILE_MODE15},
	{mmCC_RB_BACKEND_DISABLE, 1},
	{mmGC_USER_RB_BACKEND_DISABLE, 1},
	{mmGB_BACKEND_MAP, 0},
	{mmPA_SC_RASTER_CONFIG, 1},
	{mmPA_SC_RASTER_CONFIG_1, 1},
};

/*
 * cik_get_register_value: gfx_v7_0's copies where it keeps them (the RB
 * registers per SE/SH, GB_ADDR_CONFIG, MC_ARB_RAMCFG, the tiling and
 * macrotiling tables), the register otherwise (also before stage gfx). Every
 * indexed register on the list is one gfx_v7_0 keeps, so GRBM_GFX_INDEX is
 * never touched here (amdgpu's select_se_sh arm for the others is not
 * needed).
 */
static int cik_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			     uint32_t *value)
{
	uint32_t i;

	*value = 0;
	for (i = 0; i < ARRAY_SIZE(cik_allowed_read_registers); i++) {
		int indexed = cik_allowed_read_registers[i].grbm_indexed;

		if (reg_offset != cik_allowed_read_registers[i].reg_offset)
			continue;
		if (frb_gfx_v7_0_get_register_value(adev, indexed ? se_num : 0xffffffff,
						    indexed ? sh_num : 0xffffffff, reg_offset, value))
			*value = RREG32(reg_offset);
		return 0;
	}
	return -EINVAL;
}

/*
 * Stage sdma on CIK: cik_sdma's hw_init, then what amdgpu_device_ip_late_init
 * does with the blocks up: amdgpu_device_set_cg_state(GATE) in IP-block
 * order (cik_common: nothing; gmc_v7_0: MC MGCG/LS, BIF LS, HDP MGCG/LS;
 * gfx_v7_0: MGCG under the RLC, CGCG off, as Hawaii's cg_flags have no
 * GFX_CGCG; cik_sdma: MGCG and LS). The golden table leaves
 * RLC_CGCG_CGLS_CTRL at 0x0020003f (CGCG/CGLS on with no RLC set-up behind
 * them, what RadeonNI found held the SMC at its top level on SI);
 * gfx_v7_0_rlc_resume clears those bits and the gate keeps them clear.
 * frb_cg=0 leaves the gating as hw_init left it. RadeonNI ported
 * cik_init_cg but never ran it on the card (its CIK default is off).
 */
static int cik_sdma_stage(struct frb_device *adev)
{
	int r = frb_cik_sdma_init(adev);

	if (r || adev->no_cg)
		return r;
	frb_gmc_v7_0_set_clockgating_state(adev, 1);
	frb_gfx_v7_0_set_clockgating_state(adev, 1);
	frb_cik_sdma_set_clockgating_state(adev, 1);
	adev->cg_on = 1;
	frb_info(adev, "cik: clock gating (cg_flags 0x%llx): RLC_CGTT_MGCG_OVERRIDE %08x, RLC_CGCG_CGLS_CTRL %08x, "
		 "CGTS_SM_CTRL_REG %08x, HDP_HOST_PATH_CNTL %08x, SDMA0_CLK_CTRL %08x, SDMA0_POWER_CNTL %08x",
		 (unsigned long long)adev->cg_flags, RREG32(mmRLC_CGTT_MGCG_OVERRIDE), RREG32(mmRLC_CGCG_CGLS_CTRL),
		 RREG32(mmCGTS_SM_CTRL_REG), RREG32(mmHDP_HOST_PATH_CNTL), RREG32(mmSDMA0_CLK_CTRL),
		 RREG32(mmSDMA0_POWER_CNTL));
	return 0;
}

/* --- cik_set_uvd_clocks (cik.c): VCLK and DCLK through their DFS dividers ---- */

/*
 * cik_set_uvd_clock: the post divider ComputeMemoryEnginePLL (1.6) gives for
 * `clock` into CG_VCLK_CNTL / CG_DCLK_CNTL (DIR_CNTL_EN cleared), then up to
 * a second for the status bit (frb_vi.c's vi_set_uvd_clock is the same
 * function). With CI DPM the SMC sets these from the UVD levels.
 */
static int cik_set_uvd_clock(struct frb_device *adev, uint32_t clock, uint32_t cntl_reg, uint32_t status_reg)
{
	struct frb_atom_clock_dividers dividers;
	uint32_t tmp;
	int r, i;

	r = frb_atombios_get_clock_dividers(adev, FRB_COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
					    clock, 0, &dividers);
	if (r)
		return r;

	tmp = RREG32_SMC(cntl_reg);
	tmp &= ~(CG_DCLK_CNTL__DCLK_DIR_CNTL_EN_MASK |
		 CG_DCLK_CNTL__DCLK_DIVIDER_MASK);
	tmp |= dividers.post_divider;
	WREG32_SMC(cntl_reg, tmp);

	for (i = 0; i < 100; i++) {
		if (RREG32_SMC(status_reg) & CG_DCLK_STATUS__DCLK_STATUS_MASK)
			break;
		mdelay(10);
	}
	if (i == 100)
		return -ETIMEDOUT;
	return 0;
}

int frb_cik_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk)
{
	int r;

	r = cik_set_uvd_clock(adev, vclk, ixCG_VCLK_CNTL, ixCG_VCLK_STATUS);
	if (r)
		return r;
	return cik_set_uvd_clock(adev, dclk, ixCG_DCLK_CNTL, ixCG_DCLK_STATUS);
}

/* --- cik_set_ip_blocks (Hawaii): the blocks Firebrand has -------------------- */

const struct frb_asic_funcs frb_cik_asic_funcs = {
	.name = "cik",
	.rmmio_bar = 0x24,		/* BAR5 (amdgpu_device_init: from Bonaire on) */
	.family = AMDGPU_FAMILY_CI,
	.pcie_index = mmPCIE_INDEX,	/* 0xe, bif_4_1_d.h */
	.pcie_data = mmPCIE_DATA,
	.early_init = cik_common_early_init,
	.smc_rreg = cik_smc_rreg,
	.smc_wreg = cik_smc_wreg,
	.get_config_memsize = cik_get_config_memsize,
	.need_reset_on_init = cik_need_reset_on_init,
	.pre_asic_init = cik_pre_asic_init,
	.asic_reset = cik_asic_reset,
	.common_hw_init = cik_common_hw_init,
	.gmc_early_init = NULL,
	.gmc_sw_init = frb_gmc_v7_0_sw_init,
	.gmc_hw_init = frb_gmc_v7_0_hw_init,
	.smu_start = frb_cik_smc_stage,
	.display = &frb_dce_v8_0_funcs,	/* dce_v8_0 (frb_dce_v8_0.c): runs frb_cik_gfx_addr_config first */
	.gfx_init = frb_gfx_v7_0_init,
	.sdma_init = cik_sdma_stage,
	.uvd_init = frb_uvd_v4_2_init,
	.read_register = cik_read_register,
	.num_crtc = 6,			/* dce_v8_0_get_num_crtc: Bonaire/Hawaii */
};
