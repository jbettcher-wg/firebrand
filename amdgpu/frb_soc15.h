/*
 * SOC15 (Vega and later): registers addressed per IP block and instance
 * through base offsets, as amdgpu's soc15_common.h does, and the soc15.c,
 * nbio_v7_4.c, hdp_v4_0.c and vega20_reg_init.c pieces Firebrand has.
 *
 * A SOC15 register is named by its IP block (GC, MMHUB, NBIO, MP0, ...), an
 * instance and the header's offset, whose segment (reg##_BASE_IDX) selects
 * one of the block's base addresses: SOC15_REG_OFFSET(ip, inst, mmREG) is
 * adev->reg_offset[ip##_HWIP][inst][mmREG_BASE_IDX] + mmREG, a dword index
 * into the register BAR, which frb_rreg/frb_wreg take as on VI. The bases
 * are vega20_ip_offset.h's, filled in by frb_vega20_reg_base_init.
 *
 * The register headers under include/asic_reg/ for SOC15 are trimmed copies
 * of Linux's (tools/mk-soc15-headers.py): they hold only the names this
 * directory uses.
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
#ifndef FRB_SOC15_H
#define FRB_SOC15_H

#include "frb_device.h"

/* amdgpu_ip.h enum amd_hw_ip_block_type (7.1.1 values) */
enum amd_hw_ip_block_type {
	GC_HWIP = 1,
	HDP_HWIP,
	SDMA0_HWIP,
	SDMA1_HWIP,
	SDMA2_HWIP,
	SDMA3_HWIP,
	SDMA4_HWIP,
	SDMA5_HWIP,
	SDMA6_HWIP,
	SDMA7_HWIP,
	LSDMA_HWIP,
	MMHUB_HWIP,
	ATHUB_HWIP,
	NBIO_HWIP,
	MP0_HWIP,
	MP1_HWIP,
	UVD_HWIP,
	VCN_HWIP = UVD_HWIP,
	JPEG_HWIP = VCN_HWIP,
	VCN1_HWIP,
	VCE_HWIP,
	VPE_HWIP,
	DF_HWIP,
	DCE_HWIP,
	OSSSYS_HWIP,
	SMUIO_HWIP,
	PWR_HWIP,
	NBIF_HWIP,
	THM_HWIP,
	CLK_HWIP,
	UMC_HWIP,
	RSMU_HWIP,
	XGMI_HWIP,
	DCI_HWIP,
	PCIE_HWIP,
	ISP_HWIP,
	ATU_HWIP,
	AIGC_HWIP,
	MAX_HWIP
};

#define IP_VERSION_FULL(mj, mn, rv, var, srev) \
	(((mj) << 24) | ((mn) << 16) | ((rv) << 8) | ((var) << 4) | (srev))
#define IP_VERSION(mj, mn, rv) IP_VERSION_FULL(mj, mn, rv, 0, 0)

/* soc15_common.h, without the SR-IOV (RLCG) and KIQ arms: bare metal, no KIQ yet */
#define SOC15_REG_OFFSET(ip, inst, reg)	(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg)
#define SOC15_REG_OFFSET1(ip, inst, reg, offset) \
	(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg) + (offset))

/* each macro pastes reg##_BASE_IDX itself: nested, the argument would be expanded first (as in Linux) */
#define RREG32_SOC15(ip, inst, reg)		RREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg)
#define RREG32_SOC15_NO_KIQ(ip, inst, reg)	RREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg)
#define RREG32_SOC15_OFFSET(ip, inst, reg, offset) \
	RREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg) + (offset))
#define RREG32_SOC15_IP(ip, reg)		RREG32(reg)
#define RREG32_SOC15_IP_NO_KIQ(ip, reg, inst)	RREG32(reg)
#define WREG32_SOC15(ip, inst, reg, value) \
	WREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg, (value))
#define WREG32_SOC15_NO_KIQ(ip, inst, reg, value) \
	WREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg, (value))
#define WREG32_SOC15_RLC(ip, inst, reg, value) \
	WREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + reg, (value))
#define WREG32_SOC15_OFFSET(ip, inst, reg, offset, value) \
	WREG32(adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg) + (offset), (value))
#define WREG32_SOC15_IP(ip, reg, value)		WREG32((reg), (value))
#define WREG32_SOC15_IP_NO_KIQ(ip, reg, value, inst) WREG32((reg), (value))
#define WREG32_RLC(reg, value)			WREG32((reg), (value))
#define WREG32_FIELD15(ip, idx, reg, field, val)					\
	WREG32(adev->reg_offset[ip##_HWIP][idx][mm##reg##_BASE_IDX] + mm##reg,		\
	       (RREG32(adev->reg_offset[ip##_HWIP][idx][mm##reg##_BASE_IDX] + mm##reg) &	\
		~REG_FIELD_MASK(reg, field)) | (val) << REG_FIELD_SHIFT(reg, field))
#define SOC15_WAIT_ON_RREG(ip, inst, reg, expected_value, mask) \
	frb_device_wait_on_rreg(adev, inst, adev->reg_offset[ip##_HWIP][inst][reg##_BASE_IDX] + (reg), #reg, \
				(expected_value), (mask))

#define lower_32_bits(n)	((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n)	((uint32_t)(((uint64_t)(n)) >> 32))
#define min_t(t, a, b)		((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define max_t(t, a, b)		((t)(a) > (t)(b) ? (t)(a) : (t)(b))

/* soc15.h: golden settings */
struct soc15_reg_golden {
	uint32_t hwip;
	uint32_t instance;
	uint32_t segment;
	uint32_t reg;
	uint32_t and_mask;
	uint32_t or_mask;
};
#define SOC15_REG_GOLDEN_VALUE(ip, inst, reg, and_mask, or_mask) \
	{ ip##_HWIP, inst, reg##_BASE_IDX, reg, and_mask, or_mask }

/* amdgpu_device_wait_on_rreg: 0, or -ETIMEDOUT after adev->usec_timeout polls (logged) */
int frb_device_wait_on_rreg(struct frb_device *adev, uint32_t inst, uint32_t reg_addr, const char *reg_name,
			    uint32_t expected_value, uint32_t mask);

/* vega20_reg_init.c */
void frb_vega20_reg_base_init(struct frb_device *adev);

/* soc15.c */
void frb_soc15_program_register_sequence(struct frb_device *adev, const struct soc15_reg_golden *regs,
					 uint32_t array_size);
void frb_soc15_common_early_init(struct frb_device *adev);
int frb_soc15_common_hw_init(struct frb_device *adev);
uint32_t frb_soc15_get_config_memsize(struct frb_device *adev);
uint32_t frb_soc15_get_xclk(struct frb_device *adev);		/* 10 kHz */
/* soc15_read_register (frb_engines.c): frb_soc15_asic_funcs.read_register */
int frb_soc15_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			    uint32_t *value);
/* the survey's reads (frb_atomfirmware.c prints the VBIOS part) */
void frb_soc15_survey_registers(struct frb_device *adev);
/* THM CG_MULT_THERMAL_STATUS.CTF_TEMP (vega20_thermal_get_temperature), degrees C */
int frb_soc15_get_temperature(struct frb_device *adev);

/* frb_soc15_audio_function_strap: frb_device.h (the kext calls it) */

/* hdp_v4_0.c, the flush through nbio_v7_4's remapped register (amdgpu_device_flush_hdp) */
void frb_soc15_flush_hdp(struct frb_device *adev);
void frb_soc15_invalidate_hdp(struct frb_device *adev);
/* their ring arms (amdgpu_ring_emit_wreg): the register to write */
uint32_t frb_soc15_hdp_flush_reg(struct frb_device *adev);
uint32_t frb_soc15_hdp_invalidate_reg(struct frb_device *adev);

/*
 * SMN (System Management Network) through PCIE_INDEX2/DATA2, as
 * amdgpu_device_indirect_rreg/wreg (soc15_pcie_rreg/wreg). Linux reaches
 * these with readl/writel on the index pair directly, so its register
 * traces do not show them; the replay models them (tests/frb_replay.c).
 */
uint32_t frb_soc15_smn_rreg(struct frb_device *adev, uint32_t reg);
void frb_soc15_smn_wreg(struct frb_device *adev, uint32_t reg, uint32_t v);
#define RREG32_SMN(reg)		frb_soc15_smn_rreg(adev, (reg))
#define WREG32_SMN(reg, v)	frb_soc15_smn_wreg(adev, (reg), (v))

/* gmc_v9_0.c (frb_gmc_v9_0.c), gfxhub_v1_0.c, mmhub_v1_0.c */
int frb_gmc_v9_0_sw_init(struct frb_device *adev);
int frb_gmc_v9_0_hw_init(struct frb_device *adev);
void frb_gmc_v9_0_flush_gpu_tlb(struct frb_device *adev, uint32_t vmid, uint32_t vmhub, uint32_t flush_type);
uint64_t frb_gmc_v9_0_vram_mc2pa(struct frb_device *adev, uint64_t mc_addr);
/* amdgpu_vmhub, the fields gmc_v9_0 uses: struct frb_vmhub_regs (frb_device.h) */
#define AMDGPU_GFXHUB(x)	(x)
#define AMDGPU_MMHUB0(x)	(8 + (x))
void frb_gfxhub_v1_0_init(struct frb_device *adev);
uint64_t frb_gfxhub_v1_0_get_mc_fb_offset(struct frb_device *adev);
int frb_gfxhub_v1_0_gart_enable(struct frb_device *adev);
void frb_gfxhub_v1_0_set_fault_enable_default(struct frb_device *adev, int value);
void frb_mmhub_v1_0_init(struct frb_device *adev);
uint64_t frb_mmhub_v1_0_get_fb_location(struct frb_device *adev);
int frb_mmhub_v1_0_gart_enable(struct frb_device *adev);
void frb_mmhub_v1_0_set_fault_enable_default(struct frb_device *adev, int value);
void frb_mmhub_v1_0_set_clockgating(struct frb_device *adev, int gate);	/* mmhub_v1_0_set_clockgating */

/* amdgpu_atomfirmware.c (frb_atomfirmware.c) */
void frb_atomfirmware_scratch_regs_init(struct frb_device *adev);
int frb_atomfirmware_allocate_fb_scratch(struct frb_device *adev);
int frb_atomfirmware_get_clock_info(struct frb_device *adev);
int frb_atomfirmware_get_vram_info(struct frb_device *adev, int *vram_width, int *vram_type, int *vram_vendor);
void frb_atomfirmware_survey(struct frb_device *adev);
/* a data/command table's offset and revision by its v2_1 master-list index (0: absent) */
int frb_atomfirmware_data_table(struct frb_device *adev, int index, uint8_t *frev, uint8_t *crev, uint16_t *offset);

#endif /* FRB_SOC15_H */
