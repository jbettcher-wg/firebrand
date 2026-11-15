/*
 * soc15.c, nbio_v7_4.c, hdp_v4_0.c and vega20_reg_init.c, the parts Vega 20
 * needs up to stage smu (Linux 7.1.1, the kernel of the Vega 20 capture):
 *
 * - vega20_reg_base_init: vega20_ip_offset.h's bases into adev->reg_offset;
 * - soc15_common_early_init: the revision (nbio_v7_4_get_rev_id) and
 *   external revision, the register remap hole (nbio_v7_4_set_reg_remap);
 * - soc15_common_hw_init: ASPM (amdgpu_device_should_use_aspm() is false
 *   here: no PCIE_LC_* writes in the capture), nbio_v7_4_init_registers
 *   (nothing for NBIO 7.4.0), the HDP flush registers remapped into the hole
 *   at the end of the register BAR, the doorbell aperture, and the SDMA
 *   doorbell ranges;
 * - hdp_v4_0 (HDP 4.2.0 on Vega 20): amdgpu_hdp_generic_flush through the
 *   remapped HDP_MEM_FLUSH_CNTL and the read cache invalidate;
 * - soc15_common_set_clockgating_state for NBIO 7.4.0 (BIF light sleep
 *   through SMN, the HDP memories' power control, MP0's DRM gating and light
 *   sleep, the ROM clock; DF has no flag on Vega 20);
 * - the SMN window (PCIE_INDEX2/DATA2), soc15_get_config_memsize, the
 *   survey's register reads and the GPU temperature.
 *
 * Copyright 2016-2019 Advanced Micro Devices, Inc.
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
#include "frb_psp.h"
#include "frb_display.h"
#include "include/asic_reg/nbio/nbio_7_4_offset.h"
#include "include/asic_reg/nbio/nbio_7_4_sh_mask.h"
#include "include/asic_reg/hdp/hdp_4_0_offset.h"
#include "include/asic_reg/hdp/hdp_4_0_sh_mask.h"
#include "include/asic_reg/mp/mp_11_0_offset.h"
#include "include/asic_reg/thm/thm_11_0_2_offset.h"
#include "include/asic_reg/thm/thm_11_0_2_sh_mask.h"
#include "include/asic_reg/mmhub/mmhub_1_0_offset.h"
#include "include/asic_reg/mmhub/mmhub_1_0_sh_mask.h"
#include "include/asic_reg/smuio/smuio_11_0_0_offset.h"
#include "include/asic_reg/smuio/smuio_11_0_0_sh_mask.h"
#include "include/asic_reg/nbio/nbio_7_4_0_smn.h"

/* --- vega20_reg_init.c: vega20_ip_offset.h's IP_BASEs ----------------------- */

static const struct {
	uint32_t hwip;
	uint32_t base[FRB_HWIP_INST][FRB_HWIP_SEG];
} vega20_bases[] = {
	{ ATHUB_HWIP,	{ { 0x00000C20 } } },
	{ CLK_HWIP,	{ { 0x00016C00, 0x00016E00, 0x00017000, 0x00017200, 0x0001B000, 0x0001B200 } } },
	{ DCE_HWIP,	{ { 0x00000012, 0x000000C0, 0x000034C0 } } },
	{ DF_HWIP,	{ { 0x00007000 } } },
	{ GC_HWIP,	{ { 0x00002000, 0x0000A000 } } },
	{ HDP_HWIP,	{ { 0x00000F20 } } },
	{ MMHUB_HWIP,	{ { 0x0001A000 } } },
	{ MP0_HWIP,	{ { 0x00016000 } } },
	{ MP1_HWIP,	{ { 0x00016000 } } },
	{ NBIO_HWIP,	{ { 0x00000000, 0x00000014, 0x00000D20, 0x00010400 } } },
	{ NBIF_HWIP,	{ { 0x00000000, 0x00000014, 0x00000D20, 0x00010400 } } },	/* = NBIO's */
	{ OSSSYS_HWIP,	{ { 0x000010A0 } } },
	{ SDMA0_HWIP,	{ { 0x00001260 } } },
	{ SDMA1_HWIP,	{ { 0x00001860 } } },
	{ SMUIO_HWIP,	{ { 0x00016800, 0x00016A00 } } },
	{ THM_HWIP,	{ { 0x00016600 } } },
	{ UMC_HWIP,	{ { 0x00014000 } } },
	{ UVD_HWIP,	{ { 0x00007800, 0x00007E00 }, { 0, 0x00009000 } } },
	{ VCE_HWIP,	{ { 0x00007E00 } } },	/* "Adjust VCE_BASE to make vce_4_1 use vce_4_0 offset header files" */
	{ RSMU_HWIP,	{ { 0x00012000 } } },
};

void frb_vega20_reg_base_init(struct frb_device *adev)
{
	unsigned int i;

	memset(adev->reg_offset, 0, sizeof(adev->reg_offset));
	for (i = 0; i < ARRAY_SIZE(vega20_bases); i++)
		memcpy(adev->reg_offset[vega20_bases[i].hwip], vega20_bases[i].base,
		       sizeof(vega20_bases[i].base));
}

/* --- soc15.c --------------------------------------------------------------- */

/* soc15_program_register_sequence (no RLCG: bare metal) */
void frb_soc15_program_register_sequence(struct frb_device *adev, const struct soc15_reg_golden *regs,
					 uint32_t array_size)
{
	uint32_t tmp, reg, i;

	for (i = 0; i < array_size; ++i) {
		const struct soc15_reg_golden *entry = &regs[i];

		reg = adev->reg_offset[entry->hwip][entry->instance][entry->segment] + entry->reg;
		if (entry->and_mask == 0xffffffff) {
			tmp = entry->or_mask;
		} else {
			tmp = RREG32(reg);
			tmp &= ~(entry->and_mask);
			tmp |= (entry->or_mask & entry->and_mask);
		}
		WREG32(reg, tmp);
	}
}

/* amdgpu_device_wait_on_rreg */
int frb_device_wait_on_rreg(struct frb_device *adev, uint32_t inst, uint32_t reg_addr, const char *reg_name,
			    uint32_t expected_value, uint32_t mask)
{
	uint32_t old_ = 0, tmp_ = RREG32(reg_addr), loop = adev->usec_timeout;

	(void)inst;
	while ((tmp_ & (mask)) != (expected_value)) {
		if (old_ != tmp_) {
			loop = adev->usec_timeout;
			old_ = tmp_;
		} else {
			udelay(1);
		}
		tmp_ = RREG32(reg_addr);
		loop--;
		if (!loop) {
			frb_err(adev, "Register(%u) [%s] failed to reach value 0x%08x != 0x%08x",
				inst, reg_name, (uint32_t)expected_value, (uint32_t)(tmp_ & (mask)));
			return -ETIMEDOUT;
		}
	}
	return 0;
}

/*
 * nbio_v7_4_set_reg_remap: MMIO_REG_HOLE_OFFSET, the last 4 KB of the 512 KB
 * register BAR (amdgpu: 0x80000 - PAGE_SIZE with 4 KB pages; the capture's
 * REMAP_HDP_MEM_FLUSH_CNTL is 0x7f000). KFD_MMIO_REMAP_HDP_MEM/REG_FLUSH_CNTL
 * are 0 and 4 into it.
 */
#define MMIO_REG_HOLE_OFFSET			(0x80000 - 4096)
#define KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL	0
#define KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL	4

/* soc15_common_early_init, Vega 20 (GC 9.4.0): rev id, external rev id, the gating flags it carries */
void frb_soc15_common_early_init(struct frb_device *adev)
{
	uint32_t tmp;

	frb_vega20_reg_base_init(adev);
	/* nbio_v7_4_get_rev_id */
	tmp = RREG32_SOC15(NBIO, 0, mmRCC_DEV0_EPF0_STRAP0);
	tmp &= RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0_MASK;
	tmp >>= RCC_DEV0_EPF0_STRAP0__STRAP_ATI_REV_ID_DEV0_F0__SHIFT;
	adev->rev_id = tmp;
	adev->family = AMDGPU_FAMILY_AI;
	/* IP_VERSION(9, 4, 0): Linux's 18 flags; frb_soc15_set_cg_state gates by them after stage sdma */
	adev->cg_flags = AMD_CG_SUPPORT_GFX_MGCG |
		AMD_CG_SUPPORT_GFX_MGLS |
		AMD_CG_SUPPORT_GFX_CGCG |
		AMD_CG_SUPPORT_GFX_CGLS |
		AMD_CG_SUPPORT_GFX_3D_CGCG |
		AMD_CG_SUPPORT_GFX_3D_CGLS |
		AMD_CG_SUPPORT_GFX_CP_LS |
		AMD_CG_SUPPORT_MC_LS |
		AMD_CG_SUPPORT_MC_MGCG |
		AMD_CG_SUPPORT_SDMA_MGCG |
		AMD_CG_SUPPORT_SDMA_LS |
		AMD_CG_SUPPORT_BIF_MGCG |
		AMD_CG_SUPPORT_BIF_LS |
		AMD_CG_SUPPORT_HDP_MGCG |
		AMD_CG_SUPPORT_HDP_LS |
		AMD_CG_SUPPORT_ROM_MGCG |
		AMD_CG_SUPPORT_VCE_MGCG |
		AMD_CG_SUPPORT_UVD_MGCG;
	adev->cg_flags &= ~adev->cg_exclude;
	adev->pg_flags = 0;
	adev->external_rev_id = adev->rev_id + 0x28;
}

/*
 * The HD Audio function's straps. RCC_DEV0_EPF1_STRAP0 is not in
 * nbio_7_4_offset.h; Vega 20's strap block has nbio_2_3's layout
 * (RCC_DEV0_EPF0_STRAP0 at 0x11, read on the card as 0x316066a1;
 * RCC_DEV0_PORT_STRAP1 at 0x08 reads the switch downstream port's id 14a1),
 * so function 1's STRAP0 is 0x1a. Both NBIO base 2 (dword 0xd20).
 *
 * What hides function 1 is RCC_BIF_STRAP0's STRAP_AUD_PIN (bits 19:18), the
 * BIF's copy of ATI's AUD[1:0] pin strap: 0 is "no audio function", and then
 * function 0's header type reads single-function and function 1 does not
 * answer, although the BIF holds its configuration space (SMN 0x10141000
 * reads ab201002). The MI50 has it 0 (RCC_BIF_STRAP0 0x00000ae8). The
 * register is writable (RCC_BIF_STRAP1 WRITE_DISABLE 0) and takes effect at
 * once, with no reset: on the MI50, 2026-11-15, 0xd20 = 0x000c0ae8 turned the
 * header type to 0x80 and function 1's id to ab201002 on a running card.
 * PERST# (a warm reboot) puts it back to 0. PCIEP_STRAP_MISC's
 * STRAP_MULTI_FUNC_EN (0x53), which an earlier version of this wrote, is in
 * the downstream port's block and does nothing for the endpoint.
 */
#define FRB_mmRCC_DEV0_EPF1_STRAP0			0x001a
#define FRB_mmRCC_DEV0_EPF1_STRAP0_BASE_IDX		2
#define FRB_EPF1_STRAP0__DEVICE_ID_MASK			0x0000ffff
#define FRB_EPF1_STRAP0__FUNC_EN_MASK			0x10000000
#define FRB_RCC_BIF_STRAP0__STRAP_AUD_PIN_MASK		0x000c0000	/* nbio_2_3_sh_mask.h */
#define FRB_HDA_DEVICE_ID				0xab20	/* amdgpu's Vega 20 HD Audio (snd_hda_intel 0xab20) */

int frb_soc15_audio_function_strap(struct frb_device *adev, int write)
{
	uint32_t f1, bif;

	f1 = RREG32_SOC15(NBIO, 0, FRB_mmRCC_DEV0_EPF1_STRAP0);
	bif = RREG32_SOC15(NBIO, 0, mmRCC_BIF_STRAP0);
	frb_info(adev, "audio function: RCC_DEV0_EPF1_STRAP0 %08x (device %04x, %s), RCC_BIF_STRAP0 %08x "
		 "(STRAP_AUD_PIN %u)", f1, f1 & FRB_EPF1_STRAP0__DEVICE_ID_MASK,
		 (f1 & FRB_EPF1_STRAP0__FUNC_EN_MASK) ? "enabled" : "disabled", bif,
		 (bif & FRB_RCC_BIF_STRAP0__STRAP_AUD_PIN_MASK) >> 18);
	if (!(f1 & FRB_EPF1_STRAP0__FUNC_EN_MASK) || (f1 & FRB_EPF1_STRAP0__DEVICE_ID_MASK) != FRB_HDA_DEVICE_ID)
		return FRB_AUDIOFN_NONE;
	if (bif & FRB_RCC_BIF_STRAP0__STRAP_AUD_PIN_MASK)
		return FRB_AUDIOFN_ALREADY;
	if (!write)
		return FRB_AUDIOFN_LEFT;
	/* 3: audio for DisplayPort and HDMI, as a card with outputs is strapped */
	WREG32_SOC15(NBIO, 0, mmRCC_BIF_STRAP0, bif | FRB_RCC_BIF_STRAP0__STRAP_AUD_PIN_MASK);
	bif = RREG32_SOC15(NBIO, 0, mmRCC_BIF_STRAP0);
	frb_info(adev, "audio function: RCC_BIF_STRAP0 written, reads %08x", bif);
	return (bif & FRB_RCC_BIF_STRAP0__STRAP_AUD_PIN_MASK) ? FRB_AUDIOFN_WRITTEN : FRB_AUDIOFN_FAILED;
}

/* soc15_common_hw_init, bare metal, no ASPM */
int frb_soc15_common_hw_init(struct frb_device *adev)
{
	uint32_t reg, doorbell_range;
	int i;

	/* nbio_v7_4_remap_hdp_registers */
	WREG32_SOC15(NBIO, 0, mmREMAP_HDP_MEM_FLUSH_CNTL, MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL);
	WREG32_SOC15(NBIO, 0, mmREMAP_HDP_REG_FLUSH_CNTL, MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL);
	/* nbio_v7_4_enable_doorbell_aperture */
	WREG32_FIELD15(NBIO, 0, RCC_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN, 1);
	/*
	 * soc15_sdma_doorbell_range_init -> nbio_v7_4_sdma_doorbell_range for
	 * both engines: vega20_doorbell_index_init's sdma_engine[0/1] (0x100,
	 * 0x10A) << 1, sdma_doorbell_range 20. Firebrand rings no doorbell yet;
	 * the range costs nothing and keeps the NBIO as amdgpu leaves it.
	 */
	for (i = 0; i < 2; i++) {
		static const uint32_t sdma_engine[2] = { 0x100, 0x10A };

		reg = i + SOC15_REG_OFFSET(NBIO, 0, mmBIF_SDMA0_DOORBELL_RANGE);
		doorbell_range = RREG32(reg);
		doorbell_range = REG_SET_FIELD(doorbell_range, BIF_SDMA0_DOORBELL_RANGE, OFFSET, sdma_engine[i] << 1);
		doorbell_range = REG_SET_FIELD(doorbell_range, BIF_SDMA0_DOORBELL_RANGE, SIZE, 20);
		WREG32(reg, doorbell_range);
	}
	return 0;
}

/* soc15_get_config_memsize -> nbio_v7_4_get_memsize */
uint32_t frb_soc15_get_config_memsize(struct frb_device *adev)
{
	return RREG32_SOC15(NBIO, 0, mmRCC_CONFIG_MEMSIZE);
}

/* soc15_get_xclk: the SPLL reference (100 MHz on Vega 20's VBIOS), 10 kHz */
uint32_t frb_soc15_get_xclk(struct frb_device *adev)
{
	return adev->spll_reference_freq;
}

/* --- hdp_v4_0.c ------------------------------------------------------------ */

/* amdgpu_hdp_generic_flush (no ring): the remapped HDP_MEM_FLUSH_CNTL, then nbio's get_memsize as the read-back */
void frb_soc15_flush_hdp(struct frb_device *adev)
{
	WREG32((MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL) >> 2, 0);
	(void)RREG32_SOC15(NBIO, 0, mmRCC_CONFIG_MEMSIZE);
}

/* hdp_v4_0_invalidate_hdp (no ring) */
void frb_soc15_invalidate_hdp(struct frb_device *adev)
{
	WREG32_SOC15_NO_KIQ(HDP, 0, mmHDP_READ_CACHE_INVALIDATE, 1);
	(void)RREG32_SOC15_NO_KIQ(HDP, 0, mmHDP_READ_CACHE_INVALIDATE);
}

/* the ring arms of both: the register amdgpu_ring_emit_wreg writes (flush: value 0, invalidate: 1) */
uint32_t frb_soc15_hdp_flush_reg(struct frb_device *adev)
{
	(void)adev;
	return (MMIO_REG_HOLE_OFFSET + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL) >> 2;
}

uint32_t frb_soc15_hdp_invalidate_reg(struct frb_device *adev)
{
	return SOC15_REG_OFFSET(HDP, 0, mmHDP_READ_CACHE_INVALIDATE);
}

/* --- clock gating (soc15_common_set_clockgating_state, NBIO 7.4.0) ------------ */

/* soc15.c's and hdp_v4_0.c's own definitions (not in the asic_reg headers) */
#define mmMP0_MISC_CGTT_CTRL0					0x01b9
#define mmMP0_MISC_CGTT_CTRL0_BASE_IDX				0
#define mmMP0_MISC_LIGHT_SLEEP_CTRL				0x01ba
#define mmMP0_MISC_LIGHT_SLEEP_CTRL_BASE_IDX			0
#define mmHDP_MEM_POWER_CTRL					0x00d4
#define HDP_MEM_POWER_CTRL__IPH_MEM_POWER_CTRL_EN_MASK		0x00000001L
#define HDP_MEM_POWER_CTRL__IPH_MEM_POWER_LS_EN_MASK		0x00000002L
#define HDP_MEM_POWER_CTRL__RC_MEM_POWER_CTRL_EN_MASK		0x00010000L
#define HDP_MEM_POWER_CTRL__RC_MEM_POWER_LS_EN_MASK		0x00020000L
#define mmHDP_MEM_POWER_CTRL_BASE_IDX				0

/* nbio_v7_4_update_medium_grain_light_sleep: PCIE_CNTL2's memory light sleep, through SMN */
static void nbio_v7_4_update_medium_grain_light_sleep(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32_SMN(smnPCIE_CNTL2);
	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_BIF_LS)) {
		data |= (PCIE_CNTL2__SLV_MEM_LS_EN_MASK |
			 PCIE_CNTL2__MST_MEM_LS_EN_MASK |
			 PCIE_CNTL2__REPLAY_MEM_LS_EN_MASK);
	} else {
		data &= ~(PCIE_CNTL2__SLV_MEM_LS_EN_MASK |
			  PCIE_CNTL2__MST_MEM_LS_EN_MASK |
			  PCIE_CNTL2__REPLAY_MEM_LS_EN_MASK);
	}

	if (def != data)
		WREG32_SMN(smnPCIE_CNTL2, data);
}

/* hdp_v4_0_update_clock_gating, HDP 4.2.0's arm (HDP_MEM_POWER_CTRL) */
static void hdp_v4_0_update_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32(SOC15_REG_OFFSET(HDP, 0, mmHDP_MEM_POWER_CTRL));

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_HDP_LS))
		data |= HDP_MEM_POWER_CTRL__IPH_MEM_POWER_CTRL_EN_MASK |
			HDP_MEM_POWER_CTRL__IPH_MEM_POWER_LS_EN_MASK |
			HDP_MEM_POWER_CTRL__RC_MEM_POWER_CTRL_EN_MASK |
			HDP_MEM_POWER_CTRL__RC_MEM_POWER_LS_EN_MASK;
	else
		data &= ~(HDP_MEM_POWER_CTRL__IPH_MEM_POWER_CTRL_EN_MASK |
			  HDP_MEM_POWER_CTRL__IPH_MEM_POWER_LS_EN_MASK |
			  HDP_MEM_POWER_CTRL__RC_MEM_POWER_CTRL_EN_MASK |
			  HDP_MEM_POWER_CTRL__RC_MEM_POWER_LS_EN_MASK);

	if (def != data)
		WREG32(SOC15_REG_OFFSET(HDP, 0, mmHDP_MEM_POWER_CTRL), data);
}

/* soc15_update_drm_clock_gating (no DRM_MGCG on Vega 20: the override bits stay set) */
static void soc15_update_drm_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32(SOC15_REG_OFFSET(MP0, 0, mmMP0_MISC_CGTT_CTRL0));

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_DRM_MGCG))
		data &= ~0xff000000u;
	else
		data |= 0xff000000u;

	if (def != data)
		WREG32(SOC15_REG_OFFSET(MP0, 0, mmMP0_MISC_CGTT_CTRL0), data);
}

/* soc15_update_drm_light_sleep (no DRM_LS on Vega 20: bit 0 is cleared either way, as Linux does) */
static void soc15_update_drm_light_sleep(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	def = data = RREG32(SOC15_REG_OFFSET(MP0, 0, mmMP0_MISC_LIGHT_SLEEP_CTRL));

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_DRM_LS))
		data |= 1;
	else
		data &= ~1u;

	if (def != data)
		WREG32(SOC15_REG_OFFSET(MP0, 0, mmMP0_MISC_LIGHT_SLEEP_CTRL), data);
}

/* smuio_v11_0_update_rom_clock_gating (Vega 20's SMUIO is 11.0.0; not an APU) */
static void smuio_v11_0_update_rom_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	if (!(adev->cg_flags & AMD_CG_SUPPORT_ROM_MGCG))
		return;

	def = data = RREG32_SOC15(SMUIO, 0, mmCGTT_ROM_CLK_CTRL0);

	if (enable)
		data &= ~(CGTT_ROM_CLK_CTRL0__SOFT_OVERRIDE0_MASK |
			  CGTT_ROM_CLK_CTRL0__SOFT_OVERRIDE1_MASK);
	else
		data |= CGTT_ROM_CLK_CTRL0__SOFT_OVERRIDE0_MASK |
			CGTT_ROM_CLK_CTRL0__SOFT_OVERRIDE1_MASK;

	if (def != data)
		WREG32_SOC15(SMUIO, 0, mmCGTT_ROM_CLK_CTRL0, data);
}

/*
 * soc15_common_set_clockgating_state, NBIO 7.4.0: nbio_v7_4's medium-grain
 * clock gating is an empty TODO in Linux; df_v3_6's needs DF_MGCG, which
 * Vega 20 does not have.
 */
void frb_soc15_common_set_clockgating_state(struct frb_device *adev, int gate)
{
	/* nbio_v7_4_update_medium_grain_clock_gating: nothing */
	nbio_v7_4_update_medium_grain_light_sleep(adev, gate);
	hdp_v4_0_update_clock_gating(adev, gate);
	soc15_update_drm_clock_gating(adev, gate);
	soc15_update_drm_light_sleep(adev, gate);
	smuio_v11_0_update_rom_clock_gating(adev, gate);
	/* df_v3_6_update_medium_grain_clock_gating: no AMD_CG_SUPPORT_DF_MGCG */
}

/* --- SMN ------------------------------------------------------------------- */

/*
 * amdgpu_device_indirect_rreg/wreg with nbio_v7_4's PCIE_INDEX2/PCIE_DATA2:
 * index store, index read-back (posts it), data. The e6500 must not move
 * the data access ahead of the index store; the kext's register accessors
 * order each access (byte-order.md, indirect windows).
 */
uint32_t frb_soc15_smn_rreg(struct frb_device *adev, uint32_t reg)
{
	uint32_t index = SOC15_REG_OFFSET(NBIO, 0, mmPCIE_INDEX2), data = SOC15_REG_OFFSET(NBIO, 0, mmPCIE_DATA2);

	WREG32(index, reg);
	(void)RREG32(index);
	return RREG32(data);
}

void frb_soc15_smn_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	uint32_t index = SOC15_REG_OFFSET(NBIO, 0, mmPCIE_INDEX2), data = SOC15_REG_OFFSET(NBIO, 0, mmPCIE_DATA2);

	WREG32(index, reg);
	(void)RREG32(index);
	WREG32(data, v);
	(void)RREG32(data);
}

/* --- the survey and the temperature ---------------------------------------- */

/*
 * vega20_thermal_get_temperature: CG_MULT_THERMAL_STATUS.CTF_TEMP, 9 bits of
 * whole degrees. A plain MMIO read in the THM block, valid without the SMU
 * (the passive MI50 needs watching from the first load on).
 */
int frb_soc15_get_temperature(struct frb_device *adev)
{
	uint32_t temp = RREG32_SOC15(THM, 0, mmCG_MULT_THERMAL_STATUS);

	temp = (temp & CG_MULT_THERMAL_STATUS__CTF_TEMP_MASK) >> CG_MULT_THERMAL_STATUS__CTF_TEMP__SHIFT;
	return (int)(temp & 0x1ff);
}

/*
 * The card as found, by reads amdgpu makes before its first write
 * (amdgpu_discovery's C2PMSG_33 and RCC_CONFIG_MEMSIZE, the BIOS scratch
 * registers, mmhub_v1_0_get_fb_location, the sOS sign of life), and the
 * temperature.
 */
void frb_soc15_survey_registers(struct frb_device *adev)
{
	uint32_t s[8], i, c33, c35, c81, base, top, memsize;

	for (i = 0; i < 8; i++)
		s[i] = RREG32(adev->bios_scratch_reg_offset + i);
	frb_info(adev, "BIOS_SCRATCH_0..7 (at 0x%x): %08x %08x %08x %08x %08x %08x %08x %08x",
		 adev->bios_scratch_reg_offset, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
	c33 = RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_33);
	c35 = RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35);
	c81 = RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_81);
	memsize = RREG32_SOC15(NBIO, 0, mmRCC_CONFIG_MEMSIZE);
	base = RREG32_SOC15(MMHUB, 0, mmMC_VM_FB_LOCATION_BASE);
	top = RREG32_SOC15(MMHUB, 0, mmMC_VM_FB_LOCATION_TOP);
	frb_info(adev, "ASIC_Init complete (BIOS_SCRATCH_7 bit 9): %s; RCC_CONFIG_MEMSIZE %u MB; "
		 "IFWI done (C2PMSG_33 bit 31): %s (%08x); PSP bootloader ready (C2PMSG_35 bit 31): %s (%08x); "
		 "sOS alive (C2PMSG_81): %s (%08x)",
		 (s[7] & 0x200) ? "yes" : "no", memsize, (c33 & 0x80000000) ? "yes" : "no", c33,
		 (c35 & 0x80000000) ? "yes" : "no", c35, c81 ? "yes, an earlier driver loaded it" : "no", c81);
	frb_info(adev, "MMHUB FB location %08x..%08x: MC 0x%llx..0x%llx; GPU temperature %d C",
		 base, top, (unsigned long long)((uint64_t)(base & MC_VM_FB_LOCATION_BASE__FB_BASE_MASK) << 24),
		 (unsigned long long)(((uint64_t)(top & MC_VM_FB_LOCATION_TOP__FB_TOP_MASK) << 24) | 0xffffff),
		 frb_soc15_get_temperature(adev));
}

/* --- soc15_set_ip_blocks, the Vega 20 set Firebrand has ------------------- */

/*
 * The SOC15 family's table (frb_asic_funcs_of for Vega 10/12/20), as SI's
 * fold gave SI and VI theirs. frb_device_init runs SOC15's stages through
 * its own stage_soc15 (post without the reset on init, gmc_v9_0, the PSP
 * and SMU11, DCE 12, gfx v9, sdma v4, uvd v7), so the stage hooks here are
 * the same calls, for the table's completeness. The SMC and PCIE windows are
 * the ones the frb-vega20 branch reached through VI's: PCIE_INDEX2/DATA2
 * (0xe/0xf, as VI's PCIE_INDEX/DATA) for registers past the BAR, and VI's
 * SMC pair (SMU11 itself talks through MP1's mailbox).
 */

const struct frb_asic_funcs frb_soc15_asic_funcs = {
	.name = "soc15",
	.rmmio_bar = 0x24,		/* BAR5 (512 KB on Vega 20) */
	.family = AMDGPU_FAMILY_AI,
	.pcie_index = 0xe,		/* nbio_v7_4 PCIE_INDEX2 */
	.pcie_data = 0xf,		/* PCIE_DATA2 */
	.early_init = frb_soc15_common_early_init,
	.smc_rreg = frb_vi_smc_rreg,
	.smc_wreg = frb_vi_smc_wreg,
	.get_config_memsize = frb_soc15_get_config_memsize,
	.need_reset_on_init = NULL,	/* soc15_need_reset_on_init: false on bare metal */
	.asic_reset = NULL,		/* no mode1/BACO port; recovery is per engine (frb_reset.c) */
	.common_hw_init = frb_soc15_common_hw_init,
	.gmc_early_init = NULL,
	.gmc_sw_init = frb_gmc_v9_0_sw_init,
	.gmc_hw_init = frb_gmc_v9_0_hw_init,
	.smu_start = frb_soc15_stage_smu,
	.display = &frb_dce_v12_0_funcs,
	.gfx_init = frb_gfx_v9_0_init,
	.sdma_init = frb_sdma_v4_0_init,
	.uvd_init = frb_uvd_v7_0_init,
	.read_register = frb_soc15_read_register,
	.num_crtc = 6,			/* the value the frb-vega20 branch set (gmc_v9_0 reads it) */
};
