/*
 * vi.c, the parts Firebrand has: vi_common_early_init's revision ids,
 * vi_need_reset_on_init, vi_get_config_memsize, vi_common_hw_init, and
 * vi_read_register with its allowlist (what user space may read: the
 * accelerator's RDN_UC_REG_READ for callers that are not administrators),
 * and for UVD vi_set_uvd_clocks (fixed clocks, amdgpu's arm without
 * powerplay) and the UVD context-register window.
 *
 * vi_common_hw_init on Polaris 12 is, in Linux 7.0.2: golden registers
 * (none for POLARIS10/11/12), ASPM (amdgpu_device_should_use_aspm() is
 * false on this platform: the E9173 capture has no PCIE_LC_* writes there),
 * and the doorbell aperture. Firebrand does the same three, so the only
 * register it writes is BIF_DOORBELL_APER_EN.
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
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"
#include "include/asic_reg/smu/smu_7_1_3_sh_mask.h"
#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/gca/gfx_8_0_d.h"
#include "include/asic_reg/uvd/uvd_6_0_d.h"

#define SDMA0_REGISTER_OFFSET	0x0	/* not a register */
#define SDMA1_REGISTER_OFFSET	0x200	/* not a register */

/* vi_get_rev_id, the discrete branch */
static uint32_t vi_get_rev_id(struct frb_device *adev)
{
	return (RREG32(mmPCIE_EFUSE4) & PCIE_EFUSE4__STRAP_BIF_ATI_REV_ID_MASK)
		>> PCIE_EFUSE4__STRAP_BIF_ATI_REV_ID__SHIFT;
}

/* vi_common_early_init: the ids, and the cg/pg flags of the blocks whose gating is ported */
void frb_vi_common_early_init(struct frb_device *adev)
{
	adev->rev_id = vi_get_rev_id(adev);
	adev->external_rev_id = 0xFF;
	adev->family = AMDGPU_FAMILY_VI;
	switch (adev->asic_type) {
	case CHIP_POLARIS11:
		adev->external_rev_id = adev->rev_id + 0x5A;
		break;
	case CHIP_POLARIS10:
		/* as for Polaris 12: only the flags a ported block reads (uvd_v6_0); Linux sets 19 */
		adev->cg_flags = AMD_CG_SUPPORT_UVD_MGCG | AMD_CG_SUPPORT_VCE_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = adev->rev_id + 0x50;
		break;
	case CHIP_POLARIS12:
		/* only the flags a ported block reads (uvd_v6_0) are carried; Linux sets 19 */
		adev->cg_flags = AMD_CG_SUPPORT_UVD_MGCG | AMD_CG_SUPPORT_VCE_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = adev->rev_id + 0x64;
		break;
	case CHIP_VEGAM:
		adev->external_rev_id = adev->rev_id + 0x6E;
		break;
	default:
		break;
	}
}

/*
 * vi_uvd_ctx_rreg / vi_uvd_ctx_wreg: UVD's context registers through
 * UVD_CTX_INDEX/DATA (amdgpu holds uvd_ctx_idx_lock; Firebrand's callers
 * hold the device lock).
 */
uint32_t frb_uvd_ctx_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(mmUVD_CTX_INDEX, ((reg) & 0x1ff));
	return RREG32(mmUVD_CTX_DATA);
}

void frb_uvd_ctx_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(mmUVD_CTX_INDEX, ((reg) & 0x1ff));
	WREG32(mmUVD_CTX_DATA, (v));
}

/*
 * vi_set_uvd_clock: the DFS divider ComputeMemoryEnginePLL (v1.6) gives for
 * `clock` into CG_VCLK_CNTL / CG_DCLK_CNTL (DIR_CNTL_EN cleared), then up to
 * a second for the status bit. Without powerplay these are the UVD clocks
 * (amdgpu_uvd_ring_begin_use's 533/400 MHz arm, the idle work's 0).
 */
static int vi_set_uvd_clock(struct frb_device *adev, uint32_t clock,
			    uint32_t cntl_reg, uint32_t status_reg)
{
	int r, i;
	struct frb_atom_clock_dividers dividers;
	uint32_t tmp;

	r = frb_atombios_get_clock_dividers(adev,
					    FRB_COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
					    clock, 0, &dividers);
	if (r)
		return r;

	tmp = RREG32_SMC(cntl_reg);

	tmp &= ~(CG_DCLK_CNTL__DCLK_DIR_CNTL_EN_MASK |
		 CG_DCLK_CNTL__DCLK_DIVIDER_MASK);
	tmp |= dividers.post_divider;
	WREG32_SMC(cntl_reg, tmp);

	for (i = 0; i < 100; i++) {
		tmp = RREG32_SMC(status_reg);
		if (tmp & CG_DCLK_STATUS__DCLK_STATUS_MASK)
			break;
		mdelay(10);
	}
	if (i == 100)
		return -ETIMEDOUT;
	return 0;
}

/* vi_set_uvd_clocks, the discrete branch: VCLK, then DCLK */
int frb_vi_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk)
{
	int r;

	r = vi_set_uvd_clock(adev, vclk, ixCG_VCLK_CNTL, ixCG_VCLK_STATUS);
	if (r)
		return r;

	r = vi_set_uvd_clock(adev, dclk, ixCG_DCLK_CNTL, ixCG_DCLK_STATUS);
	if (r)
		return r;

	return 0;
}

/* vi_need_reset_on_init: is the SMC already running? */
int frb_vi_need_reset_on_init(struct frb_device *adev)
{
	uint32_t clock_cntl, pc;

	clock_cntl = RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0);
	pc = RREG32_SMC(ixSMC_PC_C);
	frb_info(adev, "SMC: SYSCON_CLOCK_CNTL_0 %08x, PC %08x (%s)", clock_cntl, pc,
		 ((0 == REG_GET_FIELD(clock_cntl, SMC_SYSCON_CLOCK_CNTL_0, ck_disable)) &&
		  (0x20100 <= pc)) ? "running firmware" : "not running firmware");
	if ((0 == REG_GET_FIELD(clock_cntl, SMC_SYSCON_CLOCK_CNTL_0, ck_disable)) &&
	    (0x20100 <= pc))
		return 1;

	return 0;
}

/*
 * vi_get_xclk: the reference clock (10 kHz) as the SMC's clock pins say;
 * not an APU, so it reads CG_CLKPIN_CNTL_2 and CG_CLKPIN_CNTL each call,
 * as amdgpu does (the capture has the reads).
 */
uint32_t frb_vi_get_xclk(struct frb_device *adev)
{
	uint32_t reference_clock = adev->spll_reference_freq;
	uint32_t tmp;

	tmp = RREG32_SMC(ixCG_CLKPIN_CNTL_2);
	if (REG_GET_FIELD(tmp, CG_CLKPIN_CNTL_2, MUX_TCLK_TO_XCLK))
		return 1000;

	tmp = RREG32_SMC(ixCG_CLKPIN_CNTL);
	if (REG_GET_FIELD(tmp, CG_CLKPIN_CNTL, XTALIN_DIVIDE))
		return reference_clock / 4;

	return reference_clock;
}

/* vi_didt_rreg / vi_didt_wreg, vi_gc_cac_rreg / vi_gc_cac_wreg (index, then data) */
uint32_t frb_didt_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(mmDIDT_IND_INDEX, (reg));
	return RREG32(mmDIDT_IND_DATA);
}

void frb_didt_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(mmDIDT_IND_INDEX, (reg));
	WREG32(mmDIDT_IND_DATA, (v));
}

uint32_t frb_gc_cac_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(mmGC_CAC_IND_INDEX, (reg));
	return RREG32(mmGC_CAC_IND_DATA);
}

void frb_gc_cac_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(mmGC_CAC_IND_INDEX, (reg));
	WREG32(mmGC_CAC_IND_DATA, (v));
}

uint32_t frb_vi_get_config_memsize(struct frb_device *adev)
{
	return RREG32(mmCONFIG_MEMSIZE);
}

/* --- vi_asic_reset ---------------------------------------------------------- */

#define PCI_COMMAND		0x04
#define PCI_COMMAND_MASTER	0x4
#define AMDGPU_ASIC_RESET_DATA	0x39d5e86b	/* amdgpu.h */

/*
 * vi_asic_pci_config_reset. The reset is the BIF's: configuration space,
 * the BARs and the link stay (amdgpu restores none of them afterwards),
 * everything behind it starts over, unposted, with the SMC stopped.
 * amdgpu's pci_clear_master/pci_set_master are the command register's
 * bus-master bit here, put back only if it was set (Firebrand turns it on
 * at the first GTT object, not at load).
 */
/*
 * Not amdgpu: what the reset changed, for the log. Registers the earlier
 * load set and a reset clears (the rings' bases, VMID 0's page table, the
 * scratch's ASIC_Init bit), and the SMC's program counter and clock gate.
 */
struct reset_probe {
	uint32_t cp_rb0_base, sdma0_rb_base, vm0_pt_base, scratch7, smc_pc, smc_clock, rlc_cntl;
};

static void reset_probe(struct frb_device *adev, struct reset_probe *p)
{
	p->cp_rb0_base = RREG32(mmCP_RB0_BASE);
	p->sdma0_rb_base = RREG32(mmSDMA0_GFX_RB_BASE);
	p->vm0_pt_base = RREG32(mmVM_CONTEXT0_PAGE_TABLE_BASE_ADDR);
	p->scratch7 = RREG32(mmBIOS_SCRATCH_7);
	p->smc_pc = RREG32_SMC(ixSMC_PC_C);
	p->smc_clock = RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0);
	p->rlc_cntl = RREG32(mmRLC_CNTL);
}

static void reset_probe_log(struct frb_device *adev, const char *when, const struct reset_probe *p)
{
	frb_info(adev, "reset: %s: CP_RB0_BASE %08x SDMA0_GFX_RB_BASE %08x VM_CONTEXT0_PAGE_TABLE_BASE_ADDR %08x "
		 "BIOS_SCRATCH_7 %08x RLC_CNTL %08x SMC PC %08x SYSCON_CLOCK_CNTL_0 %08x", when,
		 p->cp_rb0_base, p->sdma0_rb_base, p->vm0_pt_base, p->scratch7, p->rlc_cntl, p->smc_pc, p->smc_clock);
}

static int vi_asic_pci_config_reset(struct frb_device *adev)
{
	struct reset_probe before, after;
	uint32_t i, cmd;
	int r = -EINVAL;

	reset_probe(adev, &before);
	reset_probe_log(adev, "before", &before);
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
			/* enable BM, if it was */
			if (cmd & PCI_COMMAND_MASTER)
				adev->os->cfg_write32(adev->os->cookie, PCI_COMMAND, cmd);
			adev->has_hw_reset = 1;
			r = 0;
			break;
		}
		udelay(1);
	}
	frb_info(adev, "reset: PCI CONFIG reset: %s after %u us (CONFIG_MEMSIZE %u MB)",
		 r ? "the ASIC did not come back" : "back", 100 + i, RREG32(mmCONFIG_MEMSIZE));
	if (!r) {
		reset_probe(adev, &after);
		reset_probe_log(adev, "after", &after);
	}

	frb_atombios_scratch_regs_engine_hung(adev, 0);

	return r;
}

/*
 * vi_asic_reset_method: BACO when powerplay reports it (smu7_baco), else
 * the PCI config reset. Firebrand has no powerplay yet, and amdgpu's reset
 * on init runs before powerplay has read the PowerPlay table's BACO cap, so
 * it takes the config reset there too.
 */
enum frb_reset_method frb_vi_asic_reset_method(struct frb_device *adev)
{
	(void)adev;
	return FRB_RESET_METHOD_LEGACY;
}

/* vi_asic_reset */
int frb_vi_asic_reset(struct frb_device *adev)
{
	int r;

	if (frb_vi_asic_reset_method(adev) == FRB_RESET_METHOD_BACO) {
		frb_info(adev, "reset: BACO reset");
		r = -ENOSYS;
	} else {
		frb_info(adev, "reset: PCI CONFIG reset");
		r = vi_asic_pci_config_reset(adev);
	}
	if (!r)
		adev->gpu_reset_counter++;
	return r;
}

/* vi_enable_doorbell_aperture */
static void vi_enable_doorbell_aperture(struct frb_device *adev, int enable)
{
	uint32_t tmp;

	tmp = RREG32(mmBIF_DOORBELL_APER_EN);
	if (enable)
		tmp = REG_SET_FIELD(tmp, BIF_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN, 1);
	else
		tmp = REG_SET_FIELD(tmp, BIF_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN, 0);

	WREG32(mmBIF_DOORBELL_APER_EN, tmp);
}

int frb_vi_common_hw_init(struct frb_device *adev)
{
	/* vi_init_golden_registers: nothing for POLARIS10/11/12 */
	/* vi_program_aspm: amdgpu_device_should_use_aspm() false here (see the header) */
	vi_enable_doorbell_aperture(adev, 1);
	return 0;
}

/* --- vi_read_register (amdgpu_info_ioctl's AMDGPU_INFO_READ_MMR_REG) ------- */

struct amdgpu_allowed_register_entry {
	uint32_t reg_offset;
	int grbm_indexed;
};

static const struct amdgpu_allowed_register_entry vi_allowed_read_registers[] = {
	{mmGRBM_STATUS},
	{mmGRBM_STATUS2},
	{mmGRBM_STATUS_SE0},
	{mmGRBM_STATUS_SE1},
	{mmGRBM_STATUS_SE2},
	{mmGRBM_STATUS_SE3},
	{mmSRBM_STATUS},
	{mmSRBM_STATUS2},
	{mmSRBM_STATUS3},
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
 * vi_get_register_value: gfx_v8_0's copies where it keeps them
 * (frb_gfx_v8_0_get_register_value), the register otherwise. Every indexed
 * register on the list is one gfx_v8_0 keeps, so GRBM_GFX_INDEX is never
 * touched here (amdgpu's select_se_sh arm for the others is not needed).
 */
static uint32_t vi_get_register_value(struct frb_device *adev, int indexed, uint32_t se_num,
				      uint32_t sh_num, uint32_t reg_offset)
{
	uint32_t val = 0;

	if (!frb_gfx_v8_0_get_register_value(adev, indexed ? se_num : 0xffffffff,
					     indexed ? sh_num : 0xffffffff, reg_offset, &val))
		return val;
	return RREG32(reg_offset);
}

int frb_vi_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			 uint32_t *value)
{
	uint32_t i;

	*value = 0;
	for (i = 0; i < ARRAY_SIZE(vi_allowed_read_registers); i++) {
		int indexed = vi_allowed_read_registers[i].grbm_indexed;

		if (reg_offset != vi_allowed_read_registers[i].reg_offset)
			continue;

		*value = vi_get_register_value(adev, indexed, se_num, sh_num, reg_offset);
		return 0;
	}
	return -EINVAL;
}

/* --- vi_set_ip_blocks, the Polaris set Firebrand has ----------------------- */

static void vi_gmc_early_init(struct frb_device *adev)
{
	frb_gmc_v8_0_early_init(adev);
}

extern const struct frb_display_funcs frb_dce_v11_0_funcs;	/* frb_dce_v11_0.c */

const struct frb_asic_funcs frb_vi_asic_funcs = {
	.name = "vi",
	.rmmio_bar = 0x24,		/* BAR5 */
	.family = AMDGPU_FAMILY_VI,
	.pcie_index = mmPCIE_INDEX,	/* 0xe */
	.pcie_data = mmPCIE_DATA,
	.early_init = frb_vi_common_early_init,
	.smc_rreg = frb_vi_smc_rreg,
	.smc_wreg = frb_vi_smc_wreg,
	.get_config_memsize = frb_vi_get_config_memsize,
	.need_reset_on_init = frb_vi_need_reset_on_init,
	.asic_reset = frb_vi_asic_reset,
	.common_hw_init = frb_vi_common_hw_init,
	.gmc_early_init = vi_gmc_early_init,
	.gmc_sw_init = frb_gmc_v8_0_sw_init,
	.gmc_hw_init = frb_gmc_v8_0_hw_init,
	.smu_start = frb_smu7_start,
	.display = &frb_dce_v11_0_funcs,
	.gfx_init = frb_gfx_v8_0_init,
	.sdma_init = frb_sdma_v3_0_init,
	.uvd_init = frb_uvd_v6_0_init,
	.read_register = frb_vi_read_register,
	.num_crtc = 6,			/* DCE 11.2 (FirebrandCore's value) */
};
