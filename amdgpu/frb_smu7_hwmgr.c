/*
 * pm/powerplay/hwmgr/smu7_hwmgr.c (Linux 7.1.1), the Polaris 12 paths: the
 * backend init (DPM defaults, EVV leakage voltages, the dependency tables),
 * setup_asic, the DPM enable (smu7_enable_dpm_tasks) and disable, the power
 * state machinery (adjust rules, set_power_state_tasks, the display gap),
 * the forced levels, the sensors.
 *
 * Linux 7.1.1 is the kernel the E9173 capture ran. It differs from 7.0.2 in
 * this file by the display-voltage request (smu7_apply_minimum_dce_voltage_
 * request, from the display clock, replaces phm_apply_dal_min_voltage_request,
 * whose dal_power_level nothing set) and the sclk cap quirk table; the
 * capture's VddC_Request messages are 7.1's.
 *
 * Deviations:
 * - VBIOS reads: smu7_get_pp_table_entry_callback_func_v1 reads the state's
 *   MCLK/SCLK records little-endian (Linux reads ulMclk and ulSclk with no
 *   conversion, relying on its LE host); smu7_dpm_patch_boot_state reads
 *   FirmwareInfo little-endian at offsetof() positions.
 * - Not ported: the v0 (CI) table paths, overdrive/ODN (PP_OVERDRIVE_MASK is
 *   off in amdgpu's default feature mask), the power profile modes, BACO,
 *   irq handlers, clock-level printing, watermark ranges, CAC buffer info.
 * - amdgpu's device fields come from Firebrand's frb_device: the PCIe caps
 *   (adev->pcie_gen_mask/mlw_mask, amdgpu_device_get_pcie_info's), the GFX
 *   config (num_se), VRAM width/type, pg_flags (no UVD/VCE power gating:
 *   Firebrand drives neither), ac_power (always on AC).
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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
#include <stddef.h>
#include "frb_smu7_hwmgr.h"
#include "frb_polaris10_smumgr.h"
#include "include/pp/smu7_ppsmc.h"
#include "include/pp/smu7_dyn_defaults.h"
#include "include/pp/pptable_v1_0.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"
#include "include/asic_reg/smu/smu_7_1_3_sh_mask.h"
#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/gmc/gmc_8_1_sh_mask.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"
#include "include/asic_reg/dce/dce_10_0_d.h"
#include "include/asic_reg/dce/dce_10_0_sh_mask.h"
#include "include/asic_reg/gca/gfx_8_0_d.h"
#include "include/asic_reg/gca/gfx_8_0_sh_mask.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/oss/oss_3_0_sh_mask.h"
#include "../shared/atom/atombios.h"

#define MC_CG_ARB_FREQ_F0           0x0a
#define MC_CG_ARB_FREQ_F1           0x0b
#define MC_CG_ARB_FREQ_F2           0x0c
#define MC_CG_ARB_FREQ_F3           0x0d

#define MC_CG_SEQ_DRAMCONF_S0       0x05
#define MC_CG_SEQ_DRAMCONF_S1       0x06
#define MC_CG_SEQ_YCLK_SUSPEND      0x04
#define MC_CG_SEQ_YCLK_RESUME       0x0a

#define SMC_CG_IND_START            0xc0030000
#define SMC_CG_IND_END              0xc0040000

#define MEM_FREQ_LOW_LATENCY        25000
#define MEM_FREQ_HIGH_LATENCY       80000

#define MEM_LATENCY_HIGH            45
#define MEM_LATENCY_LOW             35
#define MEM_LATENCY_ERR             0xFFFF

#define MC_SEQ_MISC0_GDDR5_SHIFT 28
#define MC_SEQ_MISC0_GDDR5_MASK  0xf0000000
#define MC_SEQ_MISC0_GDDR5_VALUE 5

#define PCIE_BUS_CLK                10000
#define TCLK                        (PCIE_BUS_CLK / 10)

#define ixPWR_SVI2_PLANE1_LOAD                     0xC0200280
#define PWR_SVI2_PLANE1_LOAD__PSI1_MASK                    0x00000020L
#define PWR_SVI2_PLANE1_LOAD__PSI0_EN_MASK                 0x00000040L
#define PWR_SVI2_PLANE1_LOAD__PSI1__SHIFT                  0x00000005
#define PWR_SVI2_PLANE1_LOAD__PSI0_EN__SHIFT               0x00000006

#define LEAKAGE_ID_MSB			463
#define LEAKAGE_ID_LSB			454

#define PPSMC_MSG_SetVBITimeout_VEGAM    ((uint16_t) 0x310)

#define STRAP_EVV_REVISION_MSB		2211
#define STRAP_EVV_REVISION_LSB		2208

/** Values for the CG_THERMAL_CTRL::DPM_EVENT_SRC field. */
enum DPM_EVENT_SRC {
	DPM_EVENT_SRC_ANALOG = 0,
	DPM_EVENT_SRC_EXTERNAL = 1,
	DPM_EVENT_SRC_DIGITAL = 2,
	DPM_EVENT_SRC_ANALOG_OR_EXTERNAL = 3,
	DPM_EVENT_SRC_DIGITAL_OR_EXTERNAL = 4
};

#define ixDIDT_SQ_EDC_CTRL                         0x0013
#define ixDIDT_SQ_EDC_THRESHOLD                    0x0014
#define ixDIDT_SQ_EDC_STALL_PATTERN_1_2            0x0015
#define ixDIDT_SQ_EDC_STALL_PATTERN_3_4            0x0016
#define ixDIDT_SQ_EDC_STALL_PATTERN_5_6            0x0017
#define ixDIDT_SQ_EDC_STALL_PATTERN_7              0x0018

#define ixDIDT_TD_EDC_CTRL                         0x0053
#define ixDIDT_TD_EDC_THRESHOLD                    0x0054
#define ixDIDT_TD_EDC_STALL_PATTERN_1_2            0x0055
#define ixDIDT_TD_EDC_STALL_PATTERN_3_4            0x0056
#define ixDIDT_TD_EDC_STALL_PATTERN_5_6            0x0057
#define ixDIDT_TD_EDC_STALL_PATTERN_7              0x0058

#define ixDIDT_TCP_EDC_CTRL                        0x0073
#define ixDIDT_TCP_EDC_THRESHOLD                   0x0074
#define ixDIDT_TCP_EDC_STALL_PATTERN_1_2           0x0075
#define ixDIDT_TCP_EDC_STALL_PATTERN_3_4           0x0076
#define ixDIDT_TCP_EDC_STALL_PATTERN_5_6           0x0077
#define ixDIDT_TCP_EDC_STALL_PATTERN_7             0x0078

#define ixDIDT_DB_EDC_CTRL                         0x0033
#define ixDIDT_DB_EDC_THRESHOLD                    0x0034
#define ixDIDT_DB_EDC_STALL_PATTERN_1_2            0x0035
#define ixDIDT_DB_EDC_STALL_PATTERN_3_4            0x0036
#define ixDIDT_DB_EDC_STALL_PATTERN_5_6            0x0037
#define ixDIDT_DB_EDC_STALL_PATTERN_7              0x0038

static const uint32_t DIDTEDCConfig_P12[] = {
    ixDIDT_SQ_EDC_STALL_PATTERN_1_2,
    ixDIDT_SQ_EDC_STALL_PATTERN_3_4,
    ixDIDT_SQ_EDC_STALL_PATTERN_5_6,
    ixDIDT_SQ_EDC_STALL_PATTERN_7,
    ixDIDT_SQ_EDC_THRESHOLD,
    ixDIDT_SQ_EDC_CTRL,
    ixDIDT_TD_EDC_STALL_PATTERN_1_2,
    ixDIDT_TD_EDC_STALL_PATTERN_3_4,
    ixDIDT_TD_EDC_STALL_PATTERN_5_6,
    ixDIDT_TD_EDC_STALL_PATTERN_7,
    ixDIDT_TD_EDC_THRESHOLD,
    ixDIDT_TD_EDC_CTRL,
    ixDIDT_TCP_EDC_STALL_PATTERN_1_2,
    ixDIDT_TCP_EDC_STALL_PATTERN_3_4,
    ixDIDT_TCP_EDC_STALL_PATTERN_5_6,
    ixDIDT_TCP_EDC_STALL_PATTERN_7,
    ixDIDT_TCP_EDC_THRESHOLD,
    ixDIDT_TCP_EDC_CTRL,
    ixDIDT_DB_EDC_STALL_PATTERN_1_2,
    ixDIDT_DB_EDC_STALL_PATTERN_3_4,
    ixDIDT_DB_EDC_STALL_PATTERN_5_6,
    ixDIDT_DB_EDC_STALL_PATTERN_7,
    ixDIDT_DB_EDC_THRESHOLD,
    ixDIDT_DB_EDC_CTRL,
    0xFFFFFFFF // End of list
};

/* pp_thermal.h */
static const struct PP_TemperatureRange SMU7ThermalPolicy[] = {
	{-273150,  99000, 99000, -273150, 99000, 99000, -273150, 99000, 99000},
	{ 120000, 120000, 120000, 120000, 120000, 120000, 120000, 120000, 120000},
};

static const unsigned long PhwVIslands_Magic = (unsigned long)(PHM_VIslands_Magic);
static int smu7_force_clock_level(struct pp_hwmgr *hwmgr,
		enum pp_clock_type type, uint32_t mask);
static int smu7_notify_has_display(struct pp_hwmgr *hwmgr);

static struct smu7_power_state *cast_phw_smu7_power_state(
				  struct pp_hw_power_state *hw_ps)
{
	PP_ASSERT_WITH_CODE((PhwVIslands_Magic == hw_ps->magic),
				"Invalid Powerstate Type!",
				 return NULL);

	return (struct smu7_power_state *)hw_ps;
}

static const struct smu7_power_state *cast_const_phw_smu7_power_state(
				 const struct pp_hw_power_state *hw_ps)
{
	PP_ASSERT_WITH_CODE((PhwVIslands_Magic == hw_ps->magic),
				"Invalid Powerstate Type!",
				 return NULL);

	return (const struct smu7_power_state *)hw_ps;
}

/**
 * smu7_get_mc_microcode_version - Find the MC microcode version and store it in the HwMgr struct
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_get_mc_microcode_version(struct pp_hwmgr *hwmgr)
{
	cgs_write_register(hwmgr->device, mmMC_SEQ_IO_DEBUG_INDEX, 0x9F);

	hwmgr->microcode_version_info.MC = cgs_read_register(hwmgr->device, mmMC_SEQ_IO_DEBUG_DATA);

	return 0;
}

static uint16_t smu7_get_current_pcie_speed(struct pp_hwmgr *hwmgr)
{
	uint32_t speedCntl = 0;

	/* mmPCIE_PORT_INDEX rename as mmPCIE_INDEX */
	speedCntl = cgs_read_ind_register(hwmgr->device, CGS_IND_REG__PCIE,
			ixPCIE_LC_SPEED_CNTL);
	return((uint16_t)PHM_GET_FIELD(speedCntl,
			PCIE_LC_SPEED_CNTL, LC_CURRENT_DATA_RATE));
}

static int smu7_get_current_pcie_lane_number(struct pp_hwmgr *hwmgr)
{
	uint32_t link_width;

	/* mmPCIE_PORT_INDEX rename as mmPCIE_INDEX */
	link_width = PHM_READ_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__PCIE,
			PCIE_LC_LINK_WIDTH_CNTL, LC_LINK_WIDTH_RD);

	PP_ASSERT_WITH_CODE((7 >= link_width),
			"Invalid PCIe lane width!", return 0);

	return decode_pcie_lane_width(link_width);
}

/**
 * smu7_enable_smc_voltage_controller - Enable voltage control
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always PP_Result_OK
 */
static int smu7_enable_smc_voltage_controller(struct pp_hwmgr *hwmgr)
{
	if (hwmgr->chip_id >= CHIP_POLARIS10 &&
	    hwmgr->chip_id <= CHIP_VEGAM) {
		PHM_WRITE_VFPF_INDIRECT_FIELD(hwmgr->device,
				CGS_IND_REG__SMC, PWR_SVI2_PLANE1_LOAD, PSI1, 0);
		PHM_WRITE_VFPF_INDIRECT_FIELD(hwmgr->device,
				CGS_IND_REG__SMC, PWR_SVI2_PLANE1_LOAD, PSI0_EN, 0);
	}

	if (hwmgr->feature_mask & PP_SMC_VOLTAGE_CONTROL_MASK)
		smum_send_msg_to_smc(hwmgr, PPSMC_MSG_Voltage_Cntl_Enable, NULL);

	return 0;
}

/**
 * smu7_voltage_control - Checks if we want to support voltage control
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 */
static bool smu7_voltage_control(const struct pp_hwmgr *hwmgr)
{
	const struct smu7_hwmgr *data =
			(const struct smu7_hwmgr *)(hwmgr->backend);

	return (SMU7_VOLTAGE_CONTROL_NONE != data->voltage_control);
}

/**
 * smu7_enable_voltage_control - Enable voltage control
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_enable_voltage_control(struct pp_hwmgr *hwmgr)
{
	/* enable voltage control */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			GENERAL_PWRMGT, VOLT_PWRMGT_EN, 1);

	return 0;
}

/**
 * smu7_construct_voltage_tables - Create Voltage Tables.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_construct_voltage_tables(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)hwmgr->pptable;
	int result = 0;
	uint32_t tmp;

	if (SMU7_VOLTAGE_CONTROL_BY_GPIO == data->mvdd_control) {
		result = atomctrl_get_voltage_table_v3(hwmgr,
				VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_GPIO_LUT,
				&(data->mvdd_voltage_table));
		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to retrieve MVDD table.",
				return result);
	} else if (SMU7_VOLTAGE_CONTROL_BY_SVID2 == data->mvdd_control) {
		result = phm_get_svi2_mvdd_voltage_table(hwmgr, &(data->mvdd_voltage_table),
				table_info->vdd_dep_on_mclk);

		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to retrieve SVI2 MVDD table from dependency table.",
				return result;);
	}

	if (SMU7_VOLTAGE_CONTROL_BY_GPIO == data->vddci_control) {
		result = atomctrl_get_voltage_table_v3(hwmgr,
				VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_GPIO_LUT,
				&(data->vddci_voltage_table));
		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to retrieve VDDCI table.",
				return result);
	} else if (SMU7_VOLTAGE_CONTROL_BY_SVID2 == data->vddci_control) {
		result = phm_get_svi2_vddci_voltage_table(hwmgr, &(data->vddci_voltage_table),
				table_info->vdd_dep_on_mclk);
		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to retrieve SVI2 VDDCI table from dependency table.",
				return result);
	}

	if (SMU7_VOLTAGE_CONTROL_BY_SVID2 == data->vdd_gfx_control) {
		/* VDDGFX has only SVI2 voltage control */
		result = phm_get_svi2_vdd_voltage_table(&(data->vddgfx_voltage_table),
					table_info->vddgfx_lookup_table);
		PP_ASSERT_WITH_CODE((0 == result),
			"Failed to retrieve SVI2 VDDGFX table from lookup table.", return result;);
	}


	if (SMU7_VOLTAGE_CONTROL_BY_GPIO == data->voltage_control) {
		result = atomctrl_get_voltage_table_v3(hwmgr,
					VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_GPIO_LUT,
					&data->vddc_voltage_table);
		PP_ASSERT_WITH_CODE((0 == result),
			"Failed to retrieve VDDC table.", return result;);
	} else if (SMU7_VOLTAGE_CONTROL_BY_SVID2 == data->voltage_control) {

		result = phm_get_svi2_vdd_voltage_table(&(data->vddc_voltage_table),
			table_info->vddc_lookup_table);

		PP_ASSERT_WITH_CODE((0 == result),
			"Failed to retrieve SVI2 VDDC table from dependency table.", return result;);
	}

	tmp = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_VDDC);
	PP_ASSERT_WITH_CODE(
			(data->vddc_voltage_table.count <= tmp),
		"Too many voltage values for VDDC. Trimming to fit state table.",
			phm_trim_voltage_table_to_fit_state_table(tmp,
						&(data->vddc_voltage_table)));

	tmp = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_VDDGFX);
	PP_ASSERT_WITH_CODE(
			(data->vddgfx_voltage_table.count <= tmp),
		"Too many voltage values for VDDC. Trimming to fit state table.",
			phm_trim_voltage_table_to_fit_state_table(tmp,
						&(data->vddgfx_voltage_table)));

	tmp = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_VDDCI);
	PP_ASSERT_WITH_CODE(
			(data->vddci_voltage_table.count <= tmp),
		"Too many voltage values for VDDCI. Trimming to fit state table.",
			phm_trim_voltage_table_to_fit_state_table(tmp,
					&(data->vddci_voltage_table)));

	tmp = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_MVDD);
	PP_ASSERT_WITH_CODE(
			(data->mvdd_voltage_table.count <= tmp),
		"Too many voltage values for MVDD. Trimming to fit state table.",
			phm_trim_voltage_table_to_fit_state_table(tmp,
						&(data->mvdd_voltage_table)));

	return 0;
}

/**
 * smu7_program_static_screen_threshold_parameters - Programs static screed detection parameters
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_program_static_screen_threshold_parameters(
							struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	/* Set static screen threshold unit */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			CG_STATIC_SCREEN_PARAMETER, STATIC_SCREEN_THRESHOLD_UNIT,
			data->static_screen_threshold_unit);
	/* Set static screen threshold */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			CG_STATIC_SCREEN_PARAMETER, STATIC_SCREEN_THRESHOLD,
			data->static_screen_threshold);

	return 0;
}

/**
 * smu7_enable_display_gap - Setup display gap for glitch free memory clock switching.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always  0
 */
static int smu7_enable_display_gap(struct pp_hwmgr *hwmgr)
{
	uint32_t display_gap =
			cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC,
					ixCG_DISPLAY_GAP_CNTL);

	display_gap = PHM_SET_FIELD(display_gap, CG_DISPLAY_GAP_CNTL,
			DISP_GAP, DISPLAY_GAP_IGNORE);

	display_gap = PHM_SET_FIELD(display_gap, CG_DISPLAY_GAP_CNTL,
			DISP_GAP_MCHG, DISPLAY_GAP_VBLANK);

	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			ixCG_DISPLAY_GAP_CNTL, display_gap);

	return 0;
}

/**
 * smu7_program_voting_clients - Programs activity state transition voting clients
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always  0
 */
static int smu7_program_voting_clients(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	int i;

	/* Clear reset for voting clients before enabling DPM */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			SCLK_PWRMGT_CNTL, RESET_SCLK_CNT, 0);
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			SCLK_PWRMGT_CNTL, RESET_BUSY_CNT, 0);

	for (i = 0; i < 8; i++)
		cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
					ixCG_FREQ_TRAN_VOTING_0 + i * 4,
					data->voting_rights_clients[i]);
	return 0;
}

static int smu7_clear_voting_clients(struct pp_hwmgr *hwmgr)
{
	int i;

	/* Reset voting clients before disabling DPM */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			SCLK_PWRMGT_CNTL, RESET_SCLK_CNT, 1);
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			SCLK_PWRMGT_CNTL, RESET_BUSY_CNT, 1);

	for (i = 0; i < 8; i++)
		cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
				ixCG_FREQ_TRAN_VOTING_0 + i * 4, 0);

	return 0;
}

/* Copy one arb setting to another and then switch the active set.
 * arb_src and arb_dest is one of the MC_CG_ARB_FREQ_Fx constants.
 */
static int smu7_copy_and_switch_arb_sets(struct pp_hwmgr *hwmgr,
		uint32_t arb_src, uint32_t arb_dest)
{
	uint32_t mc_arb_dram_timing;
	uint32_t mc_arb_dram_timing2;
	uint32_t burst_time;
	uint32_t mc_cg_config;

	switch (arb_src) {
	case MC_CG_ARB_FREQ_F0:
		mc_arb_dram_timing  = cgs_read_register(hwmgr->device, mmMC_ARB_DRAM_TIMING);
		mc_arb_dram_timing2 = cgs_read_register(hwmgr->device, mmMC_ARB_DRAM_TIMING2);
		burst_time = PHM_READ_FIELD(hwmgr->device, MC_ARB_BURST_TIME, STATE0);
		break;
	case MC_CG_ARB_FREQ_F1:
		mc_arb_dram_timing  = cgs_read_register(hwmgr->device, mmMC_ARB_DRAM_TIMING_1);
		mc_arb_dram_timing2 = cgs_read_register(hwmgr->device, mmMC_ARB_DRAM_TIMING2_1);
		burst_time = PHM_READ_FIELD(hwmgr->device, MC_ARB_BURST_TIME, STATE1);
		break;
	default:
		return -EINVAL;
	}

	switch (arb_dest) {
	case MC_CG_ARB_FREQ_F0:
		cgs_write_register(hwmgr->device, mmMC_ARB_DRAM_TIMING, mc_arb_dram_timing);
		cgs_write_register(hwmgr->device, mmMC_ARB_DRAM_TIMING2, mc_arb_dram_timing2);
		PHM_WRITE_FIELD(hwmgr->device, MC_ARB_BURST_TIME, STATE0, burst_time);
		break;
	case MC_CG_ARB_FREQ_F1:
		cgs_write_register(hwmgr->device, mmMC_ARB_DRAM_TIMING_1, mc_arb_dram_timing);
		cgs_write_register(hwmgr->device, mmMC_ARB_DRAM_TIMING2_1, mc_arb_dram_timing2);
		PHM_WRITE_FIELD(hwmgr->device, MC_ARB_BURST_TIME, STATE1, burst_time);
		break;
	default:
		return -EINVAL;
	}

	mc_cg_config = cgs_read_register(hwmgr->device, mmMC_CG_CONFIG);
	mc_cg_config |= 0x0000000F;
	cgs_write_register(hwmgr->device, mmMC_CG_CONFIG, mc_cg_config);
	PHM_WRITE_FIELD(hwmgr->device, MC_ARB_CG, CG_ARB_REQ, arb_dest);

	return 0;
}

static int smu7_reset_to_default(struct pp_hwmgr *hwmgr)
{
	return smum_send_msg_to_smc(hwmgr, PPSMC_MSG_ResetToDefaults, NULL);
}

/**
 * smu7_initial_switch_from_arbf0_to_f1 - Initial switch from ARB F0->F1
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 * This function is to be called from the SetPowerState table.
 */
static int smu7_initial_switch_from_arbf0_to_f1(struct pp_hwmgr *hwmgr)
{
	return smu7_copy_and_switch_arb_sets(hwmgr,
			MC_CG_ARB_FREQ_F0, MC_CG_ARB_FREQ_F1);
}

static int smu7_force_switch_to_arbf0(struct pp_hwmgr *hwmgr)
{
	uint32_t tmp;

	tmp = (cgs_read_ind_register(hwmgr->device,
			CGS_IND_REG__SMC, ixSMC_SCRATCH9) &
			0x0000ff00) >> 8;

	if (tmp == MC_CG_ARB_FREQ_F0)
		return 0;

	return smu7_copy_and_switch_arb_sets(hwmgr,
			tmp, MC_CG_ARB_FREQ_F0);
}

static uint16_t smu7_override_pcie_speed(struct pp_hwmgr *hwmgr)
{
	struct frb_device *adev = hwmgr->adev;
	uint16_t pcie_gen = 0;

	if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN4 &&
	    adev->pcie_gen_mask & CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN4)
		pcie_gen = 3;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3 &&
		adev->pcie_gen_mask & CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3)
		pcie_gen = 2;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2 &&
		adev->pcie_gen_mask & CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2)
		pcie_gen = 1;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1 &&
		adev->pcie_gen_mask & CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1)
		pcie_gen = 0;

	return pcie_gen;
}

static uint16_t smu7_override_pcie_width(struct pp_hwmgr *hwmgr)
{
	struct frb_device *adev = hwmgr->adev;
	uint16_t pcie_width = 0;

	if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X16)
		pcie_width = 16;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X12)
		pcie_width = 12;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X8)
		pcie_width = 8;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X4)
		pcie_width = 4;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X2)
		pcie_width = 2;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X1)
		pcie_width = 1;

	return pcie_width;
}

static int smu7_setup_default_pcie_table(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	struct phm_ppt_v1_pcie_table *pcie_table = NULL;

	uint32_t i, max_entry;
	uint32_t tmp;

	PP_ASSERT_WITH_CODE((data->use_pcie_performance_levels ||
			data->use_pcie_power_saving_levels), "No pcie performance levels!",
			return -EINVAL);

	if (table_info != NULL)
		pcie_table = table_info->pcie_table;

	if (data->use_pcie_performance_levels &&
			!data->use_pcie_power_saving_levels) {
		data->pcie_gen_power_saving = data->pcie_gen_performance;
		data->pcie_lane_power_saving = data->pcie_lane_performance;
	} else if (!data->use_pcie_performance_levels &&
			data->use_pcie_power_saving_levels) {
		data->pcie_gen_performance = data->pcie_gen_power_saving;
		data->pcie_lane_performance = data->pcie_lane_power_saving;
	}
	tmp = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_LINK);
	phm_reset_single_dpm_table(&data->dpm_table.pcie_speed_table,
					tmp,
					MAX_REGULAR_DPM_NUMBER);

	if (pcie_table != NULL) {
		/* max_entry is used to make sure we reserve one PCIE level
		 * for boot level (fix for A+A PSPP issue).
		 * If PCIE table from PPTable have ULV entry + 8 entries,
		 * then ignore the last entry.*/
		max_entry = (tmp < pcie_table->count) ? tmp : pcie_table->count;
		for (i = 1; i < max_entry; i++) {
			phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, i - 1,
					get_pcie_gen_support(data->pcie_gen_cap,
							pcie_table->entries[i].gen_speed),
					get_pcie_lane_support(data->pcie_lane_cap,
							pcie_table->entries[i].lane_width));
		}
		data->dpm_table.pcie_speed_table.count = max_entry - 1;
		smum_update_smc_table(hwmgr, SMU_BIF_TABLE);
	} else {
		/* Hardcode Pcie Table */
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 0,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Min_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 1,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Min_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 2,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Max_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 3,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Max_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 4,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Max_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, 5,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Max_PCIEGen),
				get_pcie_lane_support(data->pcie_lane_cap,
						PP_Max_PCIELane));

		data->dpm_table.pcie_speed_table.count = 6;
	}
	/* Populate last level for boot PCIE level, but do not increment count. */
	if (hwmgr->chip_family == AMDGPU_FAMILY_CI) {
		for (i = 0; i <= data->dpm_table.pcie_speed_table.count; i++)
			phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table, i,
				get_pcie_gen_support(data->pcie_gen_cap,
						PP_Max_PCIEGen),
				data->vbios_boot_state.pcie_lane_bootup_value);
	} else {
		phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table,
			data->dpm_table.pcie_speed_table.count,
			get_pcie_gen_support(data->pcie_gen_cap,
					PP_Min_PCIEGen),
			get_pcie_lane_support(data->pcie_lane_cap,
					PP_Max_PCIELane));

		if (data->pcie_dpm_key_disabled)
			phm_setup_pcie_table_entry(&data->dpm_table.pcie_speed_table,
				data->dpm_table.pcie_speed_table.count,
				smu7_override_pcie_speed(hwmgr), smu7_override_pcie_width(hwmgr));
	}
	return 0;
}

static int smu7_reset_dpm_tables(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	memset(&(data->dpm_table), 0x00, sizeof(data->dpm_table));

	phm_reset_single_dpm_table(
			&data->dpm_table.sclk_table,
				smum_get_mac_definition(hwmgr,
					SMU_MAX_LEVELS_GRAPHICS),
					MAX_REGULAR_DPM_NUMBER);
	phm_reset_single_dpm_table(
			&data->dpm_table.mclk_table,
			smum_get_mac_definition(hwmgr,
				SMU_MAX_LEVELS_MEMORY), MAX_REGULAR_DPM_NUMBER);

	phm_reset_single_dpm_table(
			&data->dpm_table.vddc_table,
				smum_get_mac_definition(hwmgr,
					SMU_MAX_LEVELS_VDDC),
					MAX_REGULAR_DPM_NUMBER);
	phm_reset_single_dpm_table(
			&data->dpm_table.vddci_table,
			smum_get_mac_definition(hwmgr,
				SMU_MAX_LEVELS_VDDCI), MAX_REGULAR_DPM_NUMBER);

	phm_reset_single_dpm_table(
			&data->dpm_table.mvdd_table,
				smum_get_mac_definition(hwmgr,
					SMU_MAX_LEVELS_MVDD),
					MAX_REGULAR_DPM_NUMBER);
	return 0;
}

static int smu7_setup_dpm_tables_v1(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	uint32_t i;

	struct phm_ppt_v1_clock_voltage_dependency_table *dep_sclk_table;
	struct phm_ppt_v1_clock_voltage_dependency_table *dep_mclk_table;

	if (table_info == NULL)
		return -EINVAL;

	dep_sclk_table = table_info->vdd_dep_on_sclk;
	dep_mclk_table = table_info->vdd_dep_on_mclk;

	PP_ASSERT_WITH_CODE(dep_sclk_table != NULL,
			"SCLK dependency table is missing.",
			return -EINVAL);
	PP_ASSERT_WITH_CODE(dep_sclk_table->count >= 1,
			"SCLK dependency table count is 0.",
			return -EINVAL);

	PP_ASSERT_WITH_CODE(dep_mclk_table != NULL,
			"MCLK dependency table is missing.",
			return -EINVAL);
	PP_ASSERT_WITH_CODE(dep_mclk_table->count >= 1,
			"MCLK dependency table count is 0",
			return -EINVAL);

	/* Initialize Sclk DPM table based on allow Sclk values */
	data->dpm_table.sclk_table.count = 0;
	for (i = 0; i < dep_sclk_table->count; i++) {
		if (i == 0 || data->dpm_table.sclk_table.dpm_levels[data->dpm_table.sclk_table.count - 1].value !=
						dep_sclk_table->entries[i].clk) {

			data->dpm_table.sclk_table.dpm_levels[data->dpm_table.sclk_table.count].value =
					dep_sclk_table->entries[i].clk;

			data->dpm_table.sclk_table.dpm_levels[data->dpm_table.sclk_table.count].enabled =
					i == 0;
			data->dpm_table.sclk_table.count++;
		}
	}
	if (hwmgr->platform_descriptor.overdriveLimit.engineClock == 0)
		hwmgr->platform_descriptor.overdriveLimit.engineClock = dep_sclk_table->entries[i-1].clk;
	/* Initialize Mclk DPM table based on allow Mclk values */
	data->dpm_table.mclk_table.count = 0;
	for (i = 0; i < dep_mclk_table->count; i++) {
		if (i == 0 || data->dpm_table.mclk_table.dpm_levels
				[data->dpm_table.mclk_table.count - 1].value !=
						dep_mclk_table->entries[i].clk) {
			data->dpm_table.mclk_table.dpm_levels[data->dpm_table.mclk_table.count].value =
							dep_mclk_table->entries[i].clk;
			data->dpm_table.mclk_table.dpm_levels[data->dpm_table.mclk_table.count].enabled =
							i == 0;
			data->dpm_table.mclk_table.count++;
		}
	}

	if (hwmgr->platform_descriptor.overdriveLimit.memoryClock == 0)
		hwmgr->platform_descriptor.overdriveLimit.memoryClock = dep_mclk_table->entries[i-1].clk;
	return 0;
}

static int smu7_setup_default_dpm_tables(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	smu7_reset_dpm_tables(hwmgr);

	smu7_setup_dpm_tables_v1(hwmgr);

	smu7_setup_default_pcie_table(hwmgr);

	/* save a copy of the default DPM table */
	memcpy(&(data->golden_dpm_table), &(data->dpm_table),
			sizeof(struct smu7_dpm_table));

	/* initialize ODN table: no overdrive on Firebrand (od_enabled is false) */
	return 0;
}

static int smu7_enable_vrhot_gpio_interrupt(struct pp_hwmgr *hwmgr)
{

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_RegulatorHot))
		return smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_EnableVRHotGPIOInterrupt,
				NULL);

	return 0;
}

static int smu7_enable_sclk_control(struct pp_hwmgr *hwmgr)
{
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, SCLK_PWRMGT_CNTL,
			SCLK_PWRMGT_OFF, 0);
	return 0;
}

static int smu7_enable_ulv(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (data->ulv_supported)
		return smum_send_msg_to_smc(hwmgr, PPSMC_MSG_EnableULV, NULL);

	return 0;
}

static int smu7_disable_ulv(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (data->ulv_supported)
		return smum_send_msg_to_smc(hwmgr, PPSMC_MSG_DisableULV, NULL);

	return 0;
}

static int smu7_enable_deep_sleep_master_switch(struct pp_hwmgr *hwmgr)
{
	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_SclkDeepSleep)) {
		if (smum_send_msg_to_smc(hwmgr, PPSMC_MSG_MASTER_DeepSleep_ON, NULL))
			PP_ASSERT_WITH_CODE(false,
					"Attempt to enable Master Deep Sleep switch failed!",
					return -EINVAL);
	} else {
		if (smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_MASTER_DeepSleep_OFF,
				NULL)) {
			PP_ASSERT_WITH_CODE(false,
					"Attempt to disable Master Deep Sleep switch failed!",
					return -EINVAL);
		}
	}

	return 0;
}

static int smu7_disable_deep_sleep_master_switch(struct pp_hwmgr *hwmgr)
{
	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_SclkDeepSleep)) {
		if (smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_MASTER_DeepSleep_OFF,
				NULL)) {
			PP_ASSERT_WITH_CODE(false,
					"Attempt to disable Master Deep Sleep switch failed!",
					return -EINVAL);
		}
	}

	return 0;
}

static int smu7_disable_sclk_vce_handshake(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t soft_register_value = 0;
	uint32_t handshake_disables_offset = data->soft_regs_start
				+ smum_get_offsetof(hwmgr,
					SMU_SoftRegisters, HandshakeDisables);

	soft_register_value = cgs_read_ind_register(hwmgr->device,
				CGS_IND_REG__SMC, handshake_disables_offset);
	soft_register_value |= SMU7_VCE_SCLK_HANDSHAKE_DISABLE;
	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			handshake_disables_offset, soft_register_value);
	return 0;
}

static int smu7_disable_handshake_uvd(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t soft_register_value = 0;
	uint32_t handshake_disables_offset = data->soft_regs_start
				+ smum_get_offsetof(hwmgr,
					SMU_SoftRegisters, HandshakeDisables);

	soft_register_value = cgs_read_ind_register(hwmgr->device,
				CGS_IND_REG__SMC, handshake_disables_offset);
	soft_register_value |= smum_get_mac_definition(hwmgr,
					SMU_UVD_MCLK_HANDSHAKE_DISABLE);
	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			handshake_disables_offset, soft_register_value);
	return 0;
}

static int smu7_enable_sclk_mclk_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct frb_device *adev = hwmgr->adev;	/* udelay */

	/* enable SCLK dpm */
	if (!data->sclk_dpm_key_disabled) {
		if (hwmgr->chip_id >= CHIP_POLARIS10 &&
		    hwmgr->chip_id <= CHIP_VEGAM)
			smu7_disable_sclk_vce_handshake(hwmgr);

		PP_ASSERT_WITH_CODE(
		(0 == smum_send_msg_to_smc(hwmgr, PPSMC_MSG_DPM_Enable, NULL)),
		"Failed to enable SCLK DPM during DPM Start Function!",
		return -EINVAL);
	}

	/* enable MCLK dpm */
	if (0 == data->mclk_dpm_key_disabled) {
		if (!(hwmgr->feature_mask & PP_UVD_HANDSHAKE_MASK))
			smu7_disable_handshake_uvd(hwmgr);

		PP_ASSERT_WITH_CODE(
				(0 == smum_send_msg_to_smc(hwmgr,
						PPSMC_MSG_MCLKDPM_Enable,
						NULL)),
				"Failed to enable MCLK DPM during DPM Start Function!",
				return -EINVAL);

		if ((hwmgr->chip_family == AMDGPU_FAMILY_CI) ||
		    (hwmgr->chip_id == CHIP_POLARIS10) ||
		    (hwmgr->chip_id == CHIP_POLARIS11) ||
		    (hwmgr->chip_id == CHIP_POLARIS12) ||
		    (hwmgr->chip_id == CHIP_TONGA) ||
		    (hwmgr->chip_id == CHIP_TOPAZ))
			PHM_WRITE_FIELD(hwmgr->device, MC_SEQ_CNTL_3, CAC_EN, 0x1);


		if (hwmgr->chip_family == AMDGPU_FAMILY_CI) {
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d30, 0x5);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d3c, 0x5);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d80, 0x100005);
			udelay(10);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d30, 0x400005);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d3c, 0x400005);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, 0xc0400d80, 0x500005);
		} else {
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC0_CNTL, 0x5);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC1_CNTL, 0x5);
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_CPL_CNTL, 0x100005);
			udelay(10);
			if (hwmgr->chip_id == CHIP_VEGAM) {
				cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC0_CNTL, 0x400009);
				cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC1_CNTL, 0x400009);
			} else {
				cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC0_CNTL, 0x400005);
				cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_MC1_CNTL, 0x400005);
			}
			cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixLCAC_CPL_CNTL, 0x500005);
		}
	}

	return 0;
}

static int smu7_start_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	/*enable general power management */

	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, GENERAL_PWRMGT,
			GLOBAL_PWRMGT_EN, 1);

	/* enable sclk deep sleep */

	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, SCLK_PWRMGT_CNTL,
			DYNAMIC_PM_EN, 1);

	/* prepare for PCIE DPM */

	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			data->soft_regs_start +
			smum_get_offsetof(hwmgr, SMU_SoftRegisters,
						VoltageChangeTimeout), 0x1000);
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__PCIE,
			SWRST_COMMAND_1, RESETLC, 0x0);

	if (hwmgr->chip_family == AMDGPU_FAMILY_CI)
		cgs_write_register(hwmgr->device, 0x1488,
			(cgs_read_register(hwmgr->device, 0x1488) & ~0x1));

	if (smu7_enable_sclk_mclk_dpm(hwmgr)) {
		pr_err("Failed to enable Sclk DPM and Mclk DPM!");
		return -EINVAL;
	}

	/* enable PCIE dpm */
	if (0 == data->pcie_dpm_key_disabled) {
		PP_ASSERT_WITH_CODE(
				(0 == smum_send_msg_to_smc(hwmgr,
						PPSMC_MSG_PCIeDPM_Enable,
						NULL)),
				"Failed to enable pcie DPM during DPM Start Function!",
				return -EINVAL);
	} else {
		PP_ASSERT_WITH_CODE(
				(0 == smum_send_msg_to_smc(hwmgr,
						PPSMC_MSG_PCIeDPM_Disable,
						NULL)),
				"Failed to disable pcie DPM during DPM Start Function!",
				return -EINVAL);
	}

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
				PHM_PlatformCaps_Falcon_QuickTransition)) {
		PP_ASSERT_WITH_CODE((0 == smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_EnableACDCGPIOInterrupt,
				NULL)),
				"Failed to enable AC DC GPIO Interrupt!",
				);
	}

	return 0;
}

static int smu7_disable_sclk_mclk_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	/* disable SCLK dpm */
	if (!data->sclk_dpm_key_disabled) {
		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to disable SCLK DPM when DPM is disabled",
				return 0);
		smum_send_msg_to_smc(hwmgr, PPSMC_MSG_DPM_Disable, NULL);
	}

	/* disable MCLK dpm */
	if (!data->mclk_dpm_key_disabled) {
		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to disable MCLK DPM when DPM is disabled",
				return 0);
		smum_send_msg_to_smc(hwmgr, PPSMC_MSG_MCLKDPM_Disable, NULL);
	}

	return 0;
}

static int smu7_stop_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	/* disable general power management */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, GENERAL_PWRMGT,
			GLOBAL_PWRMGT_EN, 0);
	/* disable sclk deep sleep */
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, SCLK_PWRMGT_CNTL,
			DYNAMIC_PM_EN, 0);

	/* disable PCIE dpm */
	if (!data->pcie_dpm_key_disabled) {
		PP_ASSERT_WITH_CODE(
				(smum_send_msg_to_smc(hwmgr,
						PPSMC_MSG_PCIeDPM_Disable,
						NULL) == 0),
				"Failed to disable pcie DPM during DPM Stop Function!",
				return -EINVAL);
	}

	smu7_disable_sclk_mclk_dpm(hwmgr);

	PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
			"Trying to disable voltage DPM when DPM is disabled",
			return 0);

	smum_send_msg_to_smc(hwmgr, PPSMC_MSG_Voltage_Cntl_Disable, NULL);

	return 0;
}

static void smu7_set_dpm_event_sources(struct pp_hwmgr *hwmgr, uint32_t sources)
{
	bool protection;
	enum DPM_EVENT_SRC src;

	switch (sources) {
	default:
		pr_err("Unknown throttling event sources.");
		fallthrough;
	case 0:
		protection = false;
		/* src is unused */
		break;
	case (1 << PHM_AutoThrottleSource_Thermal):
		protection = true;
		src = DPM_EVENT_SRC_DIGITAL;
		break;
	case (1 << PHM_AutoThrottleSource_External):
		protection = true;
		src = DPM_EVENT_SRC_EXTERNAL;
		break;
	case (1 << PHM_AutoThrottleSource_External) |
			(1 << PHM_AutoThrottleSource_Thermal):
		protection = true;
		src = DPM_EVENT_SRC_DIGITAL_OR_EXTERNAL;
		break;
	}
	/* Order matters - don't enable thermal protection for the wrong source. */
	if (protection) {
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, CG_THERMAL_CTRL,
				DPM_EVENT_SRC, src);
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, GENERAL_PWRMGT,
				THERMAL_PROTECTION_DIS,
				!phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_ThermalController));
	} else
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC, GENERAL_PWRMGT,
				THERMAL_PROTECTION_DIS, 1);
}

static int smu7_enable_auto_throttle_source(struct pp_hwmgr *hwmgr,
		PHM_AutoThrottleSource source)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (!(data->active_auto_throttle_sources & (1 << source))) {
		data->active_auto_throttle_sources |= 1 << source;
		smu7_set_dpm_event_sources(hwmgr, data->active_auto_throttle_sources);
	}
	return 0;
}

static int smu7_enable_thermal_auto_throttle(struct pp_hwmgr *hwmgr)
{
	return smu7_enable_auto_throttle_source(hwmgr, PHM_AutoThrottleSource_Thermal);
}

static int smu7_disable_auto_throttle_source(struct pp_hwmgr *hwmgr,
		PHM_AutoThrottleSource source)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (data->active_auto_throttle_sources & (1 << source)) {
		data->active_auto_throttle_sources &= ~(1 << source);
		smu7_set_dpm_event_sources(hwmgr, data->active_auto_throttle_sources);
	}
	return 0;
}

static int smu7_disable_thermal_auto_throttle(struct pp_hwmgr *hwmgr)
{
	return smu7_disable_auto_throttle_source(hwmgr, PHM_AutoThrottleSource_Thermal);
}

static int smu7_pcie_performance_request(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	data->pcie_performance_request = true;

	return 0;
}

static int smu7_program_edc_didt_registers(struct pp_hwmgr *hwmgr,
					   const uint32_t *cac_config_regs,
					   AtomCtrl_EDCLeakgeTable *edc_leakage_table)
{
	uint32_t data, i = 0;

	while (cac_config_regs[i] != 0xFFFFFFFF) {
		data = edc_leakage_table->DIDT_REG[i];
		cgs_write_ind_register(hwmgr->device,
				       CGS_IND_REG__DIDT,
				       cac_config_regs[i],
				       data);
		i++;
	}

	return 0;
}

static int smu7_populate_edc_leakage_registers(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	int ret = 0;

	if (!data->disable_edc_leakage_controller &&
	    data->edc_hilo_leakage_offset_from_vbios.usEdcDidtLoDpm7TableOffset &&
	    data->edc_hilo_leakage_offset_from_vbios.usEdcDidtHiDpm7TableOffset) {
		ret = smu7_program_edc_didt_registers(hwmgr,
						      DIDTEDCConfig_P12,
						      &data->edc_leakage_table);
		if (ret)
			return ret;

		ret = smum_send_msg_to_smc(hwmgr,
					   (PPSMC_Msg)PPSMC_MSG_EnableEDCController,
					   NULL);
	} else {
		ret = smum_send_msg_to_smc(hwmgr,
					   (PPSMC_Msg)PPSMC_MSG_DisableEDCController,
					   NULL);
	}

	return ret;
}

static void smu7_populate_umdpstate_clocks(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_dpm_table *golden_dpm_table = &data->golden_dpm_table;
	int32_t tmp_sclk, count, percentage;

	if (golden_dpm_table->mclk_table.count == 1) {
		percentage = 70;
		hwmgr->pstate_mclk = golden_dpm_table->mclk_table.dpm_levels[0].value;
	} else {
		percentage = 100 * golden_dpm_table->sclk_table.dpm_levels[golden_dpm_table->sclk_table.count - 1].value /
				golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 1].value;
		hwmgr->pstate_mclk = golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 2].value;
	}

	tmp_sclk = hwmgr->pstate_mclk * percentage / 100;

	{
		struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
		struct phm_ppt_v1_clock_voltage_dependency_table *vdd_dep_on_sclk =
			table_info->vdd_dep_on_sclk;

		for (count = vdd_dep_on_sclk->count - 1; count >= 0; count--) {
			if (tmp_sclk >= vdd_dep_on_sclk->entries[count].clk) {
				hwmgr->pstate_sclk = vdd_dep_on_sclk->entries[count].clk;
				break;
			}
		}
		if (count < 0)
			hwmgr->pstate_sclk = vdd_dep_on_sclk->entries[0].clk;

		hwmgr->pstate_sclk_peak =
			vdd_dep_on_sclk->entries[vdd_dep_on_sclk->count - 1].clk;
	}

	hwmgr->pstate_mclk_peak =
		golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 1].value;

	/* make sure the output is in Mhz */
	hwmgr->pstate_sclk /= 100;
	hwmgr->pstate_mclk /= 100;
	hwmgr->pstate_sclk_peak /= 100;
	hwmgr->pstate_mclk_peak /= 100;
}

static int smu7_enable_dpm_tasks(struct pp_hwmgr *hwmgr)
{
	int tmp_result = 0;
	int result = 0;

	if (smu7_voltage_control(hwmgr)) {
		tmp_result = smu7_enable_voltage_control(hwmgr);
		PP_ASSERT_WITH_CODE(tmp_result == 0,
				"Failed to enable voltage control!",
				result = tmp_result);

		tmp_result = smu7_construct_voltage_tables(hwmgr);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to construct voltage tables!",
				result = tmp_result);
	}
	smum_initialize_mc_reg_table(hwmgr);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_EngineSpreadSpectrumSupport))
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
				GENERAL_PWRMGT, DYN_SPREAD_SPECTRUM_EN, 1);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ThermalController))
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
				GENERAL_PWRMGT, THERMAL_PROTECTION_DIS, 0);

	tmp_result = smu7_program_static_screen_threshold_parameters(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to program static screen threshold parameters!",
			result = tmp_result);

	tmp_result = smu7_enable_display_gap(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable display gap!", result = tmp_result);

	tmp_result = smu7_program_voting_clients(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to program voting clients!", result = tmp_result);

	tmp_result = smum_process_firmware_header(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to process firmware header!", result = tmp_result);

	if (hwmgr->chip_id != CHIP_VEGAM) {
		tmp_result = smu7_initial_switch_from_arbf0_to_f1(hwmgr);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to initialize switch from ArbF0 to F1!",
				result = tmp_result);
	}

	result = smu7_setup_default_dpm_tables(hwmgr);
	PP_ASSERT_WITH_CODE(0 == result,
			"Failed to setup default DPM tables!", return result);

	tmp_result = smum_init_smc_table(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to initialize SMC table!", result = tmp_result);

	tmp_result = smu7_enable_vrhot_gpio_interrupt(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable VR hot GPIO interrupt!", result = tmp_result);

	if (hwmgr->chip_id >= CHIP_POLARIS10 &&
	    hwmgr->chip_id <= CHIP_VEGAM) {
		tmp_result = smu7_notify_has_display(hwmgr);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to enable display setting!", result = tmp_result);
	} else {
		smum_send_msg_to_smc(hwmgr, (PPSMC_Msg)PPSMC_NoDisplay, NULL);
	}

	if (hwmgr->chip_id >= CHIP_POLARIS10 &&
	    hwmgr->chip_id <= CHIP_VEGAM) {
		tmp_result = smu7_populate_edc_leakage_registers(hwmgr);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to populate edc leakage registers!", result = tmp_result);
	}

	tmp_result = smu7_enable_sclk_control(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable SCLK control!", result = tmp_result);

	tmp_result = smu7_enable_smc_voltage_controller(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable voltage control!", result = tmp_result);

	tmp_result = smu7_enable_ulv(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable ULV!", result = tmp_result);

	tmp_result = smu7_enable_deep_sleep_master_switch(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable deep sleep master switch!", result = tmp_result);

	tmp_result = smu7_enable_didt_config(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to enable deep sleep master switch!", result = tmp_result);

	tmp_result = smu7_start_dpm(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to start DPM!", result = tmp_result);

	tmp_result = smu7_enable_smc_cac(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable SMC CAC!", result = tmp_result);

	tmp_result = smu7_enable_power_containment(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable power containment!", result = tmp_result);

	tmp_result = smu7_power_control_set_level(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to power control set level!", result = tmp_result);

	tmp_result = smu7_enable_thermal_auto_throttle(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable thermal auto throttle!", result = tmp_result);

	tmp_result = smu7_pcie_performance_request(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"pcie performance request failed!", result = tmp_result);

	smu7_populate_umdpstate_clocks(hwmgr);

	return 0;
}

static int smu7_avfs_control(struct pp_hwmgr *hwmgr, bool enable)
{
	if (!hwmgr->avfs_supported)
		return 0;

	if (enable) {
		if (!PHM_READ_VFPF_INDIRECT_FIELD(hwmgr->device,
				CGS_IND_REG__SMC, FEATURE_STATUS, AVS_ON)) {
			PP_ASSERT_WITH_CODE(!smum_send_msg_to_smc(
					hwmgr, PPSMC_MSG_EnableAvfs, NULL),
					"Failed to enable AVFS!",
					return -EINVAL);
		}
	} else if (PHM_READ_VFPF_INDIRECT_FIELD(hwmgr->device,
			CGS_IND_REG__SMC, FEATURE_STATUS, AVS_ON)) {
		PP_ASSERT_WITH_CODE(!smum_send_msg_to_smc(
				hwmgr, PPSMC_MSG_DisableAvfs, NULL),
				"Failed to disable AVFS!",
				return -EINVAL);
	}

	return 0;
}

static int smu7_update_avfs(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (!hwmgr->avfs_supported)
		return 0;

	if (data->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_VDDC) {
		smu7_avfs_control(hwmgr, false);
	} else if (data->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_SCLK) {
		smu7_avfs_control(hwmgr, false);
		smu7_avfs_control(hwmgr, true);
	} else {
		smu7_avfs_control(hwmgr, true);
	}

	return 0;
}

static int smu7_disable_dpm_tasks(struct pp_hwmgr *hwmgr)
{
	int tmp_result, result = 0;

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ThermalController))
		PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
				GENERAL_PWRMGT, THERMAL_PROTECTION_DIS, 1);

	tmp_result = smu7_disable_power_containment(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable power containment!", result = tmp_result);

	tmp_result = smu7_disable_smc_cac(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable SMC CAC!", result = tmp_result);

	tmp_result = smu7_disable_didt_config(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable DIDT!", result = tmp_result);

	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			CG_SPLL_SPREAD_SPECTRUM, SSEN, 0);
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			GENERAL_PWRMGT, DYN_SPREAD_SPECTRUM_EN, 0);

	tmp_result = smu7_disable_thermal_auto_throttle(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable thermal auto throttle!", result = tmp_result);

	tmp_result = smu7_avfs_control(hwmgr, false);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable AVFS!", result = tmp_result);

	tmp_result = smu7_stop_dpm(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to stop DPM!", result = tmp_result);

	tmp_result = smu7_disable_deep_sleep_master_switch(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable deep sleep master switch!", result = tmp_result);

	tmp_result = smu7_disable_ulv(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to disable ULV!", result = tmp_result);

	tmp_result = smu7_clear_voting_clients(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to clear voting clients!", result = tmp_result);

	tmp_result = smu7_reset_to_default(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to reset to default!", result = tmp_result);

	tmp_result = smum_stop_smc(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to stop smc!", result = tmp_result);

	tmp_result = smu7_force_switch_to_arbf0(hwmgr);
	PP_ASSERT_WITH_CODE((tmp_result == 0),
			"Failed to force to switch arbf0!", result = tmp_result);

	return result;
}

static void smu7_init_dpm_defaults(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	struct frb_device *adev = hwmgr->adev;
	uint8_t tmp1, tmp2;
	uint16_t tmp3 = 0;

	data->dll_default_on = false;
	data->mclk_dpm0_activity_target = 0xa;
	data->vddc_vddgfx_delta = 300;
	data->static_screen_threshold = SMU7_STATICSCREENTHRESHOLD_DFLT;
	data->static_screen_threshold_unit = SMU7_STATICSCREENTHRESHOLDUNIT_DFLT;
	data->voting_rights_clients[0] = SMU7_VOTINGRIGHTSCLIENTS_DFLT0;
	data->voting_rights_clients[1] = SMU7_VOTINGRIGHTSCLIENTS_DFLT1;
	data->voting_rights_clients[2] = SMU7_VOTINGRIGHTSCLIENTS_DFLT2;
	data->voting_rights_clients[3] = SMU7_VOTINGRIGHTSCLIENTS_DFLT3;
	data->voting_rights_clients[4] = SMU7_VOTINGRIGHTSCLIENTS_DFLT4;
	data->voting_rights_clients[5] = SMU7_VOTINGRIGHTSCLIENTS_DFLT5;
	data->voting_rights_clients[6] = SMU7_VOTINGRIGHTSCLIENTS_DFLT6;
	data->voting_rights_clients[7] = SMU7_VOTINGRIGHTSCLIENTS_DFLT7;

	data->mclk_dpm_key_disabled = hwmgr->feature_mask & PP_MCLK_DPM_MASK ? false : true;
	data->sclk_dpm_key_disabled = hwmgr->feature_mask & PP_SCLK_DPM_MASK ? false : true;
	data->pcie_dpm_key_disabled = !(hwmgr->feature_mask & PP_PCIE_DPM_MASK);
	/* need to set voltage control types before EVV patching */
	data->voltage_control = SMU7_VOLTAGE_CONTROL_NONE;
	data->vddci_control = SMU7_VOLTAGE_CONTROL_NONE;
	data->mvdd_control = SMU7_VOLTAGE_CONTROL_NONE;
	data->enable_tdc_limit_feature = true;
	data->enable_pkg_pwr_tracking_feature = true;
	data->force_pcie_gen = PP_PCIEGenInvalid;
	data->ulv_supported = hwmgr->feature_mask & PP_ULV_MASK ? true : false;
	data->current_profile_setting.bupdate_sclk = 1;
	data->current_profile_setting.sclk_up_hyst = 0;
	data->current_profile_setting.sclk_down_hyst = 100;
	data->current_profile_setting.sclk_activity = SMU7_SCLK_TARGETACTIVITY_DFLT;
	data->current_profile_setting.bupdate_mclk = 1;
	if (hwmgr->chip_id >= CHIP_POLARIS10) {
		if (adev->gmc.vram_width == 256) {
			data->current_profile_setting.mclk_up_hyst = 10;
			data->current_profile_setting.mclk_down_hyst = 60;
			data->current_profile_setting.mclk_activity = 25;
		} else if (adev->gmc.vram_width == 128) {
			data->current_profile_setting.mclk_up_hyst = 5;
			data->current_profile_setting.mclk_down_hyst = 16;
			data->current_profile_setting.mclk_activity = 20;
		} else if (adev->gmc.vram_width == 64) {
			data->current_profile_setting.mclk_up_hyst = 3;
			data->current_profile_setting.mclk_down_hyst = 16;
			data->current_profile_setting.mclk_activity = 20;
		}
	} else {
		data->current_profile_setting.mclk_up_hyst = 0;
		data->current_profile_setting.mclk_down_hyst = 100;
		data->current_profile_setting.mclk_activity = SMU7_MCLK_TARGETACTIVITY_DFLT;
	}
	hwmgr->workload_mask = 1 << hwmgr->workload_prority[PP_SMC_POWER_PROFILE_FULLSCREEN3D];
	hwmgr->power_profile_mode = PP_SMC_POWER_PROFILE_FULLSCREEN3D;
	hwmgr->default_power_profile_mode = PP_SMC_POWER_PROFILE_FULLSCREEN3D;

	if (hwmgr->chip_id  == CHIP_HAWAII) {
		data->thermal_temp_setting.temperature_low = 94500;
		data->thermal_temp_setting.temperature_high = 95000;
		data->thermal_temp_setting.temperature_shutdown = 104000;
	} else {
		data->thermal_temp_setting.temperature_low = 99500;
		data->thermal_temp_setting.temperature_high = 100000;
		data->thermal_temp_setting.temperature_shutdown = 104000;
	}

	data->fast_watermark_threshold = 100;
	if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
			VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_SVID2))
		data->voltage_control = SMU7_VOLTAGE_CONTROL_BY_SVID2;
	else if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
			VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_GPIO_LUT))
		data->voltage_control = SMU7_VOLTAGE_CONTROL_BY_GPIO;

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ControlVDDGFX)) {
		if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
			VOLTAGE_TYPE_VDDGFX, VOLTAGE_OBJ_SVID2)) {
			data->vdd_gfx_control = SMU7_VOLTAGE_CONTROL_BY_SVID2;
		}
	}

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_EnableMVDDControl)) {
		if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
				VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_GPIO_LUT))
			data->mvdd_control = SMU7_VOLTAGE_CONTROL_BY_GPIO;
		else if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
				VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_SVID2))
			data->mvdd_control = SMU7_VOLTAGE_CONTROL_BY_SVID2;
	}

	if (SMU7_VOLTAGE_CONTROL_NONE == data->vdd_gfx_control)
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ControlVDDGFX);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ControlVDDCI)) {
		if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
				VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_GPIO_LUT))
			data->vddci_control = SMU7_VOLTAGE_CONTROL_BY_GPIO;
		else if (atomctrl_is_voltage_controlled_by_gpio_v3(hwmgr,
				VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_SVID2))
			data->vddci_control = SMU7_VOLTAGE_CONTROL_BY_SVID2;
	}

	if (data->mvdd_control == SMU7_VOLTAGE_CONTROL_NONE)
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
				PHM_PlatformCaps_EnableMVDDControl);

	if (data->vddci_control == SMU7_VOLTAGE_CONTROL_NONE)
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
				PHM_PlatformCaps_ControlVDDCI);

	data->vddc_phase_shed_control = 1;
	if ((hwmgr->chip_id == CHIP_POLARIS12) ||
	    ASICID_IS_P20(adev->device, adev->revision) ||
	    ASICID_IS_P21(adev->device, adev->revision) ||
	    ASICID_IS_P30(adev->device, adev->revision) ||
	    ASICID_IS_P31(adev->device, adev->revision)) {
		if (data->voltage_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
			atomctrl_get_svi2_info(hwmgr, VOLTAGE_TYPE_VDDC, &tmp1, &tmp2,
							&tmp3);
			tmp3 = (tmp3 >> 5) & 0x3;
			data->vddc_phase_shed_control = ((tmp3 << 1) | (tmp3 >> 1)) & 0x3;
		}
	} else if (hwmgr->chip_family == AMDGPU_FAMILY_CI) {
		data->vddc_phase_shed_control = 1;
	}

	if ((hwmgr->feature_mask & PP_CLOCK_STRETCH_MASK)
		&& (table_info->cac_dtp_table->usClockStretchAmount != 0))
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_ClockStretcher);

	data->pcie_gen_performance.max = PP_PCIEGen1;
	data->pcie_gen_performance.min = PP_PCIEGen3;
	data->pcie_gen_power_saving.max = PP_PCIEGen1;
	data->pcie_gen_power_saving.min = PP_PCIEGen3;
	data->pcie_lane_performance.max = 0;
	data->pcie_lane_performance.min = 16;
	data->pcie_lane_power_saving.max = 0;
	data->pcie_lane_power_saving.min = 16;


	/* adev->pg_flags: 0 on Polaris 12 (vi_common_early_init), so neither
	 * UVDPowerGating nor VCEPowerGating */

	data->disable_edc_leakage_controller = true;
	if (((adev->asic_type == CHIP_POLARIS10) && hwmgr->is_kicker) ||
	    ((adev->asic_type == CHIP_POLARIS11) && hwmgr->is_kicker) ||
	    (adev->asic_type == CHIP_POLARIS12) ||
	    (adev->asic_type == CHIP_VEGAM))
		data->disable_edc_leakage_controller = false;

	if (!atomctrl_is_asic_internal_ss_supported(hwmgr)) {
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_MemorySpreadSpectrumSupport);
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_EngineSpreadSpectrumSupport);
	}

	if ((adev->device == 0x699F) &&
	    (adev->revision == 0xCF)) {
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
				PHM_PlatformCaps_PowerContainment);
		data->enable_tdc_limit_feature = false;
		data->enable_pkg_pwr_tracking_feature = false;
		data->disable_edc_leakage_controller = true;
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_ClockStretcher);
	}
}

static int smu7_calculate_ro_range(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct frb_device *adev = hwmgr->adev;
	uint32_t asicrev1, evv_revision, max = 0, min = 0;

	atomctrl_read_efuse(hwmgr, STRAP_EVV_REVISION_LSB, STRAP_EVV_REVISION_MSB,
			&evv_revision);

	atomctrl_read_efuse(hwmgr, 568, 579, &asicrev1);

	if (ASICID_IS_P20(adev->device, adev->revision) ||
	    ASICID_IS_P30(adev->device, adev->revision)) {
		min = 1200;
		max = 2500;
	} else if (ASICID_IS_P21(adev->device, adev->revision) ||
		   ASICID_IS_P31(adev->device, adev->revision)) {
		min = 900;
		max = 2100;
	} else if (hwmgr->chip_id == CHIP_POLARIS10) {
		if (adev->subsys_vendor == 0x106B) {
			min = 1000;
			max = 2300;
		} else {
			if (evv_revision == 0) {
				min = 1000;
				max = 2300;
			} else if (evv_revision == 1) {
				if (asicrev1 == 326) {
					min = 1200;
					max = 2500;
					/* TODO: PATCH RO in VBIOS */
				} else {
					min = 1200;
					max = 2000;
				}
			} else if (evv_revision == 2) {
				min = 1200;
				max = 2500;
			}
		}
	} else {
		min = 1100;
		max = 2100;
	}

	data->ro_range_minimum = min;
	data->ro_range_maximum = max;

	/* TODO: PATCH RO in VBIOS here */

	return 0;
}

/**
 * smu7_get_evv_voltages - Get Leakage VDDC based on leakage ID.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_get_evv_voltages(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint16_t vv_id;
	uint16_t vddc = 0;
	uint16_t vddgfx = 0;
	uint16_t i, j;
	uint32_t sclk = 0;
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)hwmgr->pptable;
	struct phm_ppt_v1_clock_voltage_dependency_table *sclk_table = NULL;

	if (hwmgr->chip_id == CHIP_POLARIS10 ||
	    hwmgr->chip_id == CHIP_POLARIS11 ||
	    hwmgr->chip_id == CHIP_POLARIS12)
		smu7_calculate_ro_range(hwmgr);

	for (i = 0; i < SMU7_MAX_LEAKAGE_COUNT; i++) {
		vv_id = ATOM_VIRTUAL_VOLTAGE_ID0 + i;

		if (data->vdd_gfx_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
			if ((hwmgr->pp_table_version == PP_TABLE_V1)
			    && !phm_get_sclk_for_voltage_evv(hwmgr,
						table_info->vddgfx_lookup_table, vv_id, &sclk)) {
				if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
							PHM_PlatformCaps_ClockStretcher)) {
					sclk_table = table_info->vdd_dep_on_sclk;

					for (j = 1; j < sclk_table->count; j++) {
						if (sclk_table->entries[j].clk == sclk &&
								sclk_table->entries[j].cks_enable == 0) {
							sclk += 5000;
							break;
						}
					}
				}
				if (0 == atomctrl_get_voltage_evv_on_sclk
				    (hwmgr, VOLTAGE_TYPE_VDDGFX, sclk,
				     vv_id, &vddgfx)) {
					/* need to make sure vddgfx is less than 2v or else, it could burn the ASIC. */
					PP_ASSERT_WITH_CODE((vddgfx < 2000 && vddgfx != 0), "Invalid VDDGFX value!", return -EINVAL);

					/* the voltage should not be zero nor equal to leakage ID */
					if (vddgfx != 0 && vddgfx != vv_id) {
						data->vddcgfx_leakage.actual_voltage[data->vddcgfx_leakage.count] = vddgfx;
						data->vddcgfx_leakage.leakage_id[data->vddcgfx_leakage.count] = vv_id;
						data->vddcgfx_leakage.count++;
					}
				} else {
					pr_info("Error retrieving EVV voltage value!\n");
				}
			}
		} else {
			if (!phm_get_sclk_for_voltage_evv(hwmgr,
					table_info->vddc_lookup_table, vv_id, &sclk)) {
				if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_ClockStretcher)) {
					if (table_info == NULL)
						return -EINVAL;
					sclk_table = table_info->vdd_dep_on_sclk;

					for (j = 1; j < sclk_table->count; j++) {
						if (sclk_table->entries[j].clk == sclk &&
								sclk_table->entries[j].cks_enable == 0) {
							sclk += 5000;
							break;
						}
					}
				}

				if (phm_get_voltage_evv_on_sclk(hwmgr,
							VOLTAGE_TYPE_VDDC,
							sclk, vv_id, &vddc) == 0) {
					if (vddc >= 2000 || vddc == 0)
						return -EINVAL;
				} else {
					pr_debug("failed to retrieving EVV voltage!\n");
					continue;
				}

				/* the voltage should not be zero nor equal to leakage ID */
				if (vddc != 0 && vddc != vv_id) {
					data->vddc_leakage.actual_voltage[data->vddc_leakage.count] = (uint16_t)(vddc);
					data->vddc_leakage.leakage_id[data->vddc_leakage.count] = vv_id;
					data->vddc_leakage.count++;
				}
			}
		}
	}

	return 0;
}

/**
 * smu7_patch_ppt_v1_with_vdd_leakage - Change virtual leakage voltage to actual value.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * @voltage: pointer to changing voltage
 * @leakage_table: pointer to leakage table
 */
static void smu7_patch_ppt_v1_with_vdd_leakage(struct pp_hwmgr *hwmgr,
		uint16_t *voltage, struct smu7_leakage_voltage *leakage_table)
{
	uint32_t index;

	/* search for leakage voltage ID 0xff01 ~ 0xff08 */
	for (index = 0; index < leakage_table->count; index++) {
		/* if this voltage matches a leakage voltage ID */
		/* patch with actual leakage voltage */
		if (leakage_table->leakage_id[index] == *voltage) {
			*voltage = leakage_table->actual_voltage[index];
			break;
		}
	}

	if (*voltage > ATOM_VIRTUAL_VOLTAGE_ID0)
		pr_info("Voltage value looks like a Leakage ID but it's not patched\n");
}

/**
 * smu7_patch_lookup_table_with_leakage - Patch voltage lookup table by EVV leakages.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * @lookup_table: pointer to voltage lookup table
 * @leakage_table: pointer to leakage table
 * Return:     always 0
 */
static int smu7_patch_lookup_table_with_leakage(struct pp_hwmgr *hwmgr,
		phm_ppt_v1_voltage_lookup_table *lookup_table,
		struct smu7_leakage_voltage *leakage_table)
{
	uint32_t i;

	for (i = 0; i < lookup_table->count; i++)
		smu7_patch_ppt_v1_with_vdd_leakage(hwmgr,
				&lookup_table->entries[i].us_vdd, leakage_table);

	return 0;
}

static int smu7_patch_clock_voltage_limits_with_vddc_leakage(
		struct pp_hwmgr *hwmgr, struct smu7_leakage_voltage *leakage_table,
		uint16_t *vddc)
{
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	smu7_patch_ppt_v1_with_vdd_leakage(hwmgr, (uint16_t *)vddc, leakage_table);
	hwmgr->dyn_state.max_clock_voltage_on_dc.vddc =
			table_info->max_clock_voltage_on_dc.vddc;
	return 0;
}

static int smu7_patch_voltage_dependency_tables_with_lookup_table(
		struct pp_hwmgr *hwmgr)
{
	uint8_t entry_id;
	uint8_t voltage_id;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);

	struct phm_ppt_v1_clock_voltage_dependency_table *sclk_table =
			table_info->vdd_dep_on_sclk;
	struct phm_ppt_v1_clock_voltage_dependency_table *mclk_table =
			table_info->vdd_dep_on_mclk;
	struct phm_ppt_v1_mm_clock_voltage_dependency_table *mm_table =
			table_info->mm_dep_table;

	if (data->vdd_gfx_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
		for (entry_id = 0; entry_id < sclk_table->count; ++entry_id) {
			voltage_id = sclk_table->entries[entry_id].vddInd;
			sclk_table->entries[entry_id].vddgfx =
				table_info->vddgfx_lookup_table->entries[voltage_id].us_vdd;
		}
	} else {
		for (entry_id = 0; entry_id < sclk_table->count; ++entry_id) {
			voltage_id = sclk_table->entries[entry_id].vddInd;
			sclk_table->entries[entry_id].vddc =
				table_info->vddc_lookup_table->entries[voltage_id].us_vdd;
		}
	}

	for (entry_id = 0; entry_id < mclk_table->count; ++entry_id) {
		voltage_id = mclk_table->entries[entry_id].vddInd;
		mclk_table->entries[entry_id].vddc =
			table_info->vddc_lookup_table->entries[voltage_id].us_vdd;
	}

	for (entry_id = 0; entry_id < mm_table->count; ++entry_id) {
		voltage_id = mm_table->entries[entry_id].vddcInd;
		mm_table->entries[entry_id].vddc =
			table_info->vddc_lookup_table->entries[voltage_id].us_vdd;
	}

	return 0;

}

static int phm_add_voltage(struct pp_hwmgr *hwmgr,
			phm_ppt_v1_voltage_lookup_table *look_up_table,
			phm_ppt_v1_voltage_lookup_record *record)
{
	uint32_t i;

	PP_ASSERT_WITH_CODE((NULL != look_up_table),
		"Lookup Table empty.", return -EINVAL);
	PP_ASSERT_WITH_CODE((0 != look_up_table->count),
		"Lookup Table empty.", return -EINVAL);

	i = smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_VDDGFX);
	PP_ASSERT_WITH_CODE((i >= look_up_table->count),
		"Lookup Table is full.", return -EINVAL);

	/* This is to avoid entering duplicate calculated records. */
	for (i = 0; i < look_up_table->count; i++) {
		if (look_up_table->entries[i].us_vdd == record->us_vdd) {
			if (look_up_table->entries[i].us_calculated == 1)
				return 0;
			break;
		}
	}

	look_up_table->entries[i].us_calculated = 1;
	look_up_table->entries[i].us_vdd = record->us_vdd;
	look_up_table->entries[i].us_cac_low = record->us_cac_low;
	look_up_table->entries[i].us_cac_mid = record->us_cac_mid;
	look_up_table->entries[i].us_cac_high = record->us_cac_high;
	/* Only increment the count when we're appending, not replacing duplicate entry. */
	if (i == look_up_table->count)
		look_up_table->count++;

	return 0;
}

static int smu7_calc_voltage_dependency_tables(struct pp_hwmgr *hwmgr)
{
	uint8_t entry_id;
	struct phm_ppt_v1_voltage_lookup_record v_record;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *pptable_info = (struct phm_ppt_v1_information *)(hwmgr->pptable);

	phm_ppt_v1_clock_voltage_dependency_table *sclk_table = pptable_info->vdd_dep_on_sclk;
	phm_ppt_v1_clock_voltage_dependency_table *mclk_table = pptable_info->vdd_dep_on_mclk;

	if (data->vdd_gfx_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
		for (entry_id = 0; entry_id < sclk_table->count; ++entry_id) {
			if (sclk_table->entries[entry_id].vdd_offset & (1 << 15))
				v_record.us_vdd = sclk_table->entries[entry_id].vddgfx +
					sclk_table->entries[entry_id].vdd_offset - 0xFFFF;
			else
				v_record.us_vdd = sclk_table->entries[entry_id].vddgfx +
					sclk_table->entries[entry_id].vdd_offset;

			sclk_table->entries[entry_id].vddc =
				v_record.us_cac_low = v_record.us_cac_mid =
				v_record.us_cac_high = v_record.us_vdd;

			phm_add_voltage(hwmgr, pptable_info->vddc_lookup_table, &v_record);
		}

		for (entry_id = 0; entry_id < mclk_table->count; ++entry_id) {
			if (mclk_table->entries[entry_id].vdd_offset & (1 << 15))
				v_record.us_vdd = mclk_table->entries[entry_id].vddc +
					mclk_table->entries[entry_id].vdd_offset - 0xFFFF;
			else
				v_record.us_vdd = mclk_table->entries[entry_id].vddc +
					mclk_table->entries[entry_id].vdd_offset;

			mclk_table->entries[entry_id].vddgfx = v_record.us_cac_low =
				v_record.us_cac_mid = v_record.us_cac_high = v_record.us_vdd;
			phm_add_voltage(hwmgr, pptable_info->vddgfx_lookup_table, &v_record);
		}
	}
	return 0;
}

static int smu7_calc_mm_voltage_dependency_table(struct pp_hwmgr *hwmgr)
{
	uint8_t entry_id;
	struct phm_ppt_v1_voltage_lookup_record v_record;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *pptable_info = (struct phm_ppt_v1_information *)(hwmgr->pptable);
	phm_ppt_v1_mm_clock_voltage_dependency_table *mm_table = pptable_info->mm_dep_table;

	if (data->vdd_gfx_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
		for (entry_id = 0; entry_id < mm_table->count; entry_id++) {
			if (mm_table->entries[entry_id].vddgfx_offset & (1 << 15))
				v_record.us_vdd = mm_table->entries[entry_id].vddc +
					mm_table->entries[entry_id].vddgfx_offset - 0xFFFF;
			else
				v_record.us_vdd = mm_table->entries[entry_id].vddc +
					mm_table->entries[entry_id].vddgfx_offset;

			/* Add the calculated VDDGFX to the VDDGFX lookup table */
			mm_table->entries[entry_id].vddgfx = v_record.us_cac_low =
				v_record.us_cac_mid = v_record.us_cac_high = v_record.us_vdd;
			phm_add_voltage(hwmgr, pptable_info->vddgfx_lookup_table, &v_record);
		}
	}
	return 0;
}

static int smu7_sort_lookup_table(struct pp_hwmgr *hwmgr,
		struct phm_ppt_v1_voltage_lookup_table *lookup_table)
{
	uint32_t table_size, i, j;
	table_size = lookup_table->count;

	PP_ASSERT_WITH_CODE(0 != lookup_table->count,
		"Lookup table is empty", return -EINVAL);

	/* Sorting voltages */
	for (i = 0; i < table_size - 1; i++) {
		for (j = i + 1; j > 0; j--) {
			if (lookup_table->entries[j].us_vdd <
					lookup_table->entries[j - 1].us_vdd) {
				phm_ppt_v1_voltage_lookup_record t = lookup_table->entries[j - 1];

				lookup_table->entries[j - 1] = lookup_table->entries[j];
				lookup_table->entries[j] = t;
			}
		}
	}

	return 0;
}

static int smu7_complete_dependency_tables(struct pp_hwmgr *hwmgr)
{
	int result = 0;
	int tmp_result;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);

	if (data->vdd_gfx_control == SMU7_VOLTAGE_CONTROL_BY_SVID2) {
		tmp_result = smu7_patch_lookup_table_with_leakage(hwmgr,
			table_info->vddgfx_lookup_table, &(data->vddcgfx_leakage));
		if (tmp_result != 0)
			result = tmp_result;

		smu7_patch_ppt_v1_with_vdd_leakage(hwmgr,
			&table_info->max_clock_voltage_on_dc.vddgfx, &(data->vddcgfx_leakage));
	} else {

		tmp_result = smu7_patch_lookup_table_with_leakage(hwmgr,
				table_info->vddc_lookup_table, &(data->vddc_leakage));
		if (tmp_result)
			result = tmp_result;

		tmp_result = smu7_patch_clock_voltage_limits_with_vddc_leakage(hwmgr,
				&(data->vddc_leakage), &table_info->max_clock_voltage_on_dc.vddc);
		if (tmp_result)
			result = tmp_result;
	}

	tmp_result = smu7_patch_voltage_dependency_tables_with_lookup_table(hwmgr);
	if (tmp_result)
		result = tmp_result;

	tmp_result = smu7_calc_voltage_dependency_tables(hwmgr);
	if (tmp_result)
		result = tmp_result;

	tmp_result = smu7_calc_mm_voltage_dependency_table(hwmgr);
	if (tmp_result)
		result = tmp_result;

	tmp_result = smu7_sort_lookup_table(hwmgr, table_info->vddgfx_lookup_table);
	if (tmp_result)
		result = tmp_result;

	tmp_result = smu7_sort_lookup_table(hwmgr, table_info->vddc_lookup_table);
	if (tmp_result)
		result = tmp_result;

	return result;
}

static int smu7_find_highest_vddc(struct pp_hwmgr *hwmgr)
{
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	struct phm_ppt_v1_clock_voltage_dependency_table *allowed_sclk_vdd_table =
						table_info->vdd_dep_on_sclk;
	struct phm_ppt_v1_voltage_lookup_table *lookup_table =
						table_info->vddc_lookup_table;
	uint16_t highest_voltage;
	uint32_t i;

	highest_voltage = allowed_sclk_vdd_table->entries[allowed_sclk_vdd_table->count - 1].vddc;

	for (i = 0; i < lookup_table->count; i++) {
		if (lookup_table->entries[i].us_vdd < ATOM_VIRTUAL_VOLTAGE_ID0 &&
		    lookup_table->entries[i].us_vdd > highest_voltage)
			highest_voltage = lookup_table->entries[i].us_vdd;
	}

	return highest_voltage;
}

static int smu7_set_private_data_based_on_pptable_v1(struct pp_hwmgr *hwmgr)
{
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);

	struct phm_ppt_v1_clock_voltage_dependency_table *allowed_sclk_vdd_table =
						table_info->vdd_dep_on_sclk;
	struct phm_ppt_v1_clock_voltage_dependency_table *allowed_mclk_vdd_table =
						table_info->vdd_dep_on_mclk;

	PP_ASSERT_WITH_CODE(allowed_sclk_vdd_table != NULL,
		"VDD dependency on SCLK table is missing.",
		return -EINVAL);
	PP_ASSERT_WITH_CODE(allowed_sclk_vdd_table->count >= 1,
		"VDD dependency on SCLK table has to have is missing.",
		return -EINVAL);

	PP_ASSERT_WITH_CODE(allowed_mclk_vdd_table != NULL,
		"VDD dependency on MCLK table is missing",
		return -EINVAL);
	PP_ASSERT_WITH_CODE(allowed_mclk_vdd_table->count >= 1,
		"VDD dependency on MCLK table has to have is missing.",
		return -EINVAL);

	table_info->max_clock_voltage_on_ac.sclk =
		allowed_sclk_vdd_table->entries[allowed_sclk_vdd_table->count - 1].clk;
	table_info->max_clock_voltage_on_ac.mclk =
		allowed_mclk_vdd_table->entries[allowed_mclk_vdd_table->count - 1].clk;
	if (hwmgr->chip_id >= CHIP_POLARIS10 && hwmgr->chip_id <= CHIP_VEGAM)
		table_info->max_clock_voltage_on_ac.vddc =
			smu7_find_highest_vddc(hwmgr);
	else
		table_info->max_clock_voltage_on_ac.vddc =
			allowed_sclk_vdd_table->entries[allowed_sclk_vdd_table->count - 1].vddc;
	table_info->max_clock_voltage_on_ac.vddci =
		allowed_mclk_vdd_table->entries[allowed_mclk_vdd_table->count - 1].vddci;

	hwmgr->dyn_state.max_clock_voltage_on_ac.sclk = table_info->max_clock_voltage_on_ac.sclk;
	hwmgr->dyn_state.max_clock_voltage_on_ac.mclk = table_info->max_clock_voltage_on_ac.mclk;
	hwmgr->dyn_state.max_clock_voltage_on_ac.vddc = table_info->max_clock_voltage_on_ac.vddc;
	hwmgr->dyn_state.max_clock_voltage_on_ac.vddci = table_info->max_clock_voltage_on_ac.vddci;

	return 0;
}

static int smu7_patch_voltage_workaround(struct pp_hwmgr *hwmgr)
{
	struct phm_ppt_v1_information *table_info =
		       (struct phm_ppt_v1_information *)(hwmgr->pptable);
	struct phm_ppt_v1_clock_voltage_dependency_table *dep_mclk_table;
	struct phm_ppt_v1_voltage_lookup_table *lookup_table;
	uint32_t i;
	uint32_t hw_revision, sub_vendor_id, sub_sys_id;
	struct frb_device *adev = hwmgr->adev;

	if (table_info != NULL) {
		dep_mclk_table = table_info->vdd_dep_on_mclk;
		lookup_table = table_info->vddc_lookup_table;
	} else
		return 0;

	hw_revision = adev->revision;
	sub_sys_id = adev->subsys_device;
	sub_vendor_id = adev->subsys_vendor;

	if (adev->device == 0x67DF && hw_revision == 0xC7 &&
	    ((sub_sys_id == 0xb37 && sub_vendor_id == 0x1002) ||
	     (sub_sys_id == 0x4a8 && sub_vendor_id == 0x1043) ||
	     (sub_sys_id == 0x9480 && sub_vendor_id == 0x1682))) {

		PHM_WRITE_VFPF_INDIRECT_FIELD(hwmgr->device,
					      CGS_IND_REG__SMC,
					      PWR_CKS_CNTL,
					      CKS_STRETCH_AMOUNT,
					      0x3);

		if (lookup_table->entries[dep_mclk_table->entries[dep_mclk_table->count-1].vddInd].us_vdd >= 1000)
			return 0;

		for (i = 0; i < lookup_table->count; i++) {
			if (lookup_table->entries[i].us_vdd < 0xff01 && lookup_table->entries[i].us_vdd >= 1000) {
				dep_mclk_table->entries[dep_mclk_table->count-1].vddInd = (uint8_t) i;
				return 0;
			}
		}
	}
	return 0;
}

static int smu7_thermal_parameter_init(struct pp_hwmgr *hwmgr)
{
	struct pp_atomctrl_gpio_pin_assignment gpio_pin_assignment;
	uint32_t temp_reg;
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);


	if (atomctrl_get_pp_assign_pin(hwmgr, VDDC_PCC_GPIO_PINID, &gpio_pin_assignment)) {
		temp_reg = cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCNB_PWRMGT_CNTL);
		switch (gpio_pin_assignment.uc_gpio_pin_bit_shift) {
		case 0:
			temp_reg = PHM_SET_FIELD(temp_reg, CNB_PWRMGT_CNTL, GNB_SLOW_MODE, 0x1);
			break;
		case 1:
			temp_reg = PHM_SET_FIELD(temp_reg, CNB_PWRMGT_CNTL, GNB_SLOW_MODE, 0x2);
			break;
		case 2:
			temp_reg = PHM_SET_FIELD(temp_reg, CNB_PWRMGT_CNTL, GNB_SLOW, 0x1);
			break;
		case 3:
			temp_reg = PHM_SET_FIELD(temp_reg, CNB_PWRMGT_CNTL, FORCE_NB_PS1, 0x1);
			break;
		case 4:
			temp_reg = PHM_SET_FIELD(temp_reg, CNB_PWRMGT_CNTL, DPM_ENABLED, 0x1);
			break;
		default:
			break;
		}
		cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCNB_PWRMGT_CNTL, temp_reg);
	}

	if (table_info == NULL)
		return 0;

	if (table_info->cac_dtp_table->usDefaultTargetOperatingTemp != 0 &&
		hwmgr->thermal_controller.advanceFanControlParameters.ucFanControlMode) {
		hwmgr->thermal_controller.advanceFanControlParameters.usFanPWMMinLimit =
			(uint16_t)hwmgr->thermal_controller.advanceFanControlParameters.ucMinimumPWMLimit;

		hwmgr->thermal_controller.advanceFanControlParameters.usFanPWMMaxLimit =
			(uint16_t)hwmgr->thermal_controller.advanceFanControlParameters.usDefaultMaxFanPWM;

		hwmgr->thermal_controller.advanceFanControlParameters.usFanPWMStep = 1;

		hwmgr->thermal_controller.advanceFanControlParameters.usFanRPMMaxLimit = 100;

		hwmgr->thermal_controller.advanceFanControlParameters.usFanRPMMinLimit =
			(uint16_t)hwmgr->thermal_controller.advanceFanControlParameters.ucMinimumPWMLimit;

		hwmgr->thermal_controller.advanceFanControlParameters.usFanRPMStep = 1;

		table_info->cac_dtp_table->usDefaultTargetOperatingTemp = (table_info->cac_dtp_table->usDefaultTargetOperatingTemp >= 50) ?
								(table_info->cac_dtp_table->usDefaultTargetOperatingTemp - 50) : 0;

		table_info->cac_dtp_table->usOperatingTempMaxLimit = table_info->cac_dtp_table->usDefaultTargetOperatingTemp;
		table_info->cac_dtp_table->usOperatingTempStep = 1;
		table_info->cac_dtp_table->usOperatingTempHyst = 1;

		hwmgr->thermal_controller.advanceFanControlParameters.usMaxFanPWM =
			       hwmgr->thermal_controller.advanceFanControlParameters.usDefaultMaxFanPWM;

		hwmgr->thermal_controller.advanceFanControlParameters.usMaxFanRPM =
			       hwmgr->thermal_controller.advanceFanControlParameters.usDefaultMaxFanRPM;

		hwmgr->dyn_state.cac_dtp_table->usOperatingTempMinLimit =
			       table_info->cac_dtp_table->usOperatingTempMinLimit;

		hwmgr->dyn_state.cac_dtp_table->usOperatingTempMaxLimit =
			       table_info->cac_dtp_table->usOperatingTempMaxLimit;

		hwmgr->dyn_state.cac_dtp_table->usDefaultTargetOperatingTemp =
			       table_info->cac_dtp_table->usDefaultTargetOperatingTemp;

		hwmgr->dyn_state.cac_dtp_table->usOperatingTempStep =
			       table_info->cac_dtp_table->usOperatingTempStep;

		hwmgr->dyn_state.cac_dtp_table->usTargetOperatingTemp =
			       table_info->cac_dtp_table->usTargetOperatingTemp;
		if (hwmgr->feature_mask & PP_OD_FUZZY_FAN_CONTROL_MASK)
			phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_ODFuzzyFanControlSupport);
	}

	return 0;
}

static int smu7_hwmgr_backend_fini(struct pp_hwmgr *hwmgr)
{
	frb_pp_free(hwmgr, hwmgr->dyn_state.vddc_dependency_on_display_clock);
	hwmgr->dyn_state.vddc_dependency_on_display_clock = NULL;
	frb_pp_free(hwmgr, hwmgr->backend);
	hwmgr->backend = NULL;

	return 0;
}

static int smu7_update_edc_leakage_table(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t efuse;
	uint16_t offset;
	int ret = 0;

	if (data->disable_edc_leakage_controller)
		return 0;

	ret = atomctrl_get_edc_hilo_leakage_offset_table(hwmgr,
							 &data->edc_hilo_leakage_offset_from_vbios);
	if (ret)
		return ret;

	if (data->edc_hilo_leakage_offset_from_vbios.usEdcDidtLoDpm7TableOffset &&
	    data->edc_hilo_leakage_offset_from_vbios.usEdcDidtHiDpm7TableOffset) {
		atomctrl_read_efuse(hwmgr, LEAKAGE_ID_LSB, LEAKAGE_ID_MSB, &efuse);
		if (efuse < data->edc_hilo_leakage_offset_from_vbios.usHiLoLeakageThreshold)
			offset = data->edc_hilo_leakage_offset_from_vbios.usEdcDidtLoDpm7TableOffset;
		else
			offset = data->edc_hilo_leakage_offset_from_vbios.usEdcDidtHiDpm7TableOffset;

		ret = atomctrl_get_edc_leakage_table(hwmgr,
						     &data->edc_leakage_table,
						     offset);
		if (ret)
			return ret;
	}

	return ret;
}

static int smu7_init_voltage_dependency_on_display_clock_table(struct pp_hwmgr *hwmgr)
{
	struct phm_clock_voltage_dependency_table *table;

	/* Firebrand always has the DCE block (stage dce) */
	table = frb_pp_zalloc(hwmgr, sizeof(*table) + 4 * sizeof(table->entries[0]));
	if (!table)
		return -ENOMEM;

	if (hwmgr->chip_id >= CHIP_POLARIS10) {
		table->entries[0].clk = 38918;
		table->entries[1].clk = 45900;
		table->entries[2].clk = 66700;
		table->entries[3].clk = 113200;

		table->entries[0].v = 700;
		table->entries[1].v = 740;
		table->entries[2].v = 800;
		table->entries[3].v = 900;
	} else {
		if (hwmgr->chip_family == AMDGPU_FAMILY_CZ) {
			table->entries[0].clk = 35200;
			table->entries[1].clk = 35200;
			table->entries[2].clk = 46700;
			table->entries[3].clk = 64300;
		} else {
			table->entries[0].clk = 0;
			table->entries[1].clk = 35200;
			table->entries[2].clk = 54000;
			table->entries[3].clk = 62500;
		}

		table->entries[0].v = 0;
		table->entries[1].v = 720;
		table->entries[2].v = 810;
		table->entries[3].v = 900;
	}

	table->count = 4;
	hwmgr->dyn_state.vddc_dependency_on_display_clock = table;
	return 0;
}

static void smu7_set_sclk_cap(struct pp_hwmgr *hwmgr)
{
	struct frb_device *adev = hwmgr->adev;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	data->sclk_cap = 0xffffffff;

	if (hwmgr->od_enabled)
		return;

	/* R9 390X board: last sclk dpm level is unstable, use lower sclk */
	if (adev->device == 0x67B0 &&
	    adev->subsys_vendor == 0x1043)
		data->sclk_cap = 104000; /* 1040 MHz */

	if (data->sclk_cap != 0xffffffff)
		frb_info(adev, "sclk cap: %u kHz on quirky ASIC", data->sclk_cap * 10);
}

static int smu7_hwmgr_backend_init(struct pp_hwmgr *hwmgr)
{
	struct frb_device *adev = hwmgr->adev;
	struct smu7_hwmgr *data;
	int result = 0;

	data = frb_pp_zalloc(hwmgr, sizeof(struct smu7_hwmgr));
	if (data == NULL)
		return -ENOMEM;

	hwmgr->backend = data;
	smu7_set_sclk_cap(hwmgr);
	smu7_patch_voltage_workaround(hwmgr);
	smu7_init_dpm_defaults(hwmgr);

	/* Get leakage voltage based on leakage ID. */
	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_EVV)) {
		result = smu7_get_evv_voltages(hwmgr);
		if (result) {
			pr_info("Get EVV Voltage Failed.  Abort Driver loading!\n");
			frb_pp_free(hwmgr, hwmgr->backend);
			hwmgr->backend = NULL;
			return -EINVAL;
		}
	} else {
		/* smu7_get_elb_voltages: not ported (Polaris always has EVV) */
		pr_err("non-EVV leakage voltages are not ported\n");
	}

	result = smu7_init_voltage_dependency_on_display_clock_table(hwmgr);
	if (result)
		goto fail;

	smu7_complete_dependency_tables(hwmgr);
	smu7_set_private_data_based_on_pptable_v1(hwmgr);

	data->is_tlu_enabled = false;

	hwmgr->platform_descriptor.hardwareActivityPerformanceLevels =
							SMU7_MAX_HARDWARE_POWERLEVELS;
	hwmgr->platform_descriptor.hardwarePerformanceLevels = 2;
	hwmgr->platform_descriptor.minimumClocksReductionPercentage = 50;

	data->pcie_gen_cap = adev->pcie_gen_mask;
	if (data->pcie_gen_cap & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3)
		data->pcie_spc_cap = 20;
	else
		data->pcie_spc_cap = 16;
	data->pcie_lane_cap = adev->pcie_mlw_mask;

	hwmgr->platform_descriptor.vbiosInterruptId = 0x20000400; /* IRQ_SOURCE1_SW_INT */
	/* The true clock step depends on the frequency, typically 4.5 or 9 MHz. Here we use 5. */
	hwmgr->platform_descriptor.clockStep.engineClock = 500;
	hwmgr->platform_descriptor.clockStep.memoryClock = 500;
	smu7_thermal_parameter_init(hwmgr);

	result = smu7_update_edc_leakage_table(hwmgr);
	if (result)
		goto fail;

	return 0;
fail:
	smu7_hwmgr_backend_fini(hwmgr);
	return result;
}

static int smu7_force_dpm_highest(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t level, tmp;

	if (!data->pcie_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.pcie_dpm_enable_mask) {
			level = 0;
			tmp = data->dpm_level_enable_mask.pcie_dpm_enable_mask;
			while (tmp >>= 1)
				level++;

			if (level)
				smum_send_msg_to_smc_with_parameter(hwmgr,
						PPSMC_MSG_PCIeDPM_ForceLevel, level,
						NULL);
		}
	}

	if (!data->sclk_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.sclk_dpm_enable_mask) {
			level = 0;
			tmp = data->dpm_level_enable_mask.sclk_dpm_enable_mask;
			while (tmp >>= 1)
				level++;

			if (level)
				smum_send_msg_to_smc_with_parameter(hwmgr,
						PPSMC_MSG_SCLKDPM_SetEnabledMask,
						(1 << level),
						NULL);
		}
	}

	if (!data->mclk_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.mclk_dpm_enable_mask) {
			level = 0;
			tmp = data->dpm_level_enable_mask.mclk_dpm_enable_mask;
			while (tmp >>= 1)
				level++;

			if (level)
				smum_send_msg_to_smc_with_parameter(hwmgr,
						PPSMC_MSG_MCLKDPM_SetEnabledMask,
						(1 << level),
						NULL);
		}
	}

	return 0;
}

static uint32_t smu7_lookup_vddc_from_dispclk(struct pp_hwmgr *hwmgr)
{
	const struct amd_pp_display_configuration *cfg = hwmgr->display_config;
	const struct phm_clock_voltage_dependency_table *vddc_dep_on_dispclk =
			hwmgr->dyn_state.vddc_dependency_on_display_clock;
	uint32_t i;

	if (!vddc_dep_on_dispclk || !vddc_dep_on_dispclk->count ||
	    !cfg || !cfg->num_display || !cfg->display_clk)
		return 0;

	/* Start from 1 because ClocksStateUltraLow should not be used according to DC. */
	for (i = 1; i < vddc_dep_on_dispclk->count; ++i)
		if (vddc_dep_on_dispclk->entries[i].clk >= cfg->display_clk)
			return vddc_dep_on_dispclk->entries[i].v;

	return vddc_dep_on_dispclk->entries[vddc_dep_on_dispclk->count - 1].v;
}

static void smu7_apply_minimum_dce_voltage_request(struct pp_hwmgr *hwmgr)
{
	uint32_t req_vddc = smu7_lookup_vddc_from_dispclk(hwmgr);

	smum_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_VddC_Request,
			req_vddc * VOLTAGE_SCALE,
			NULL);
}

static int smu7_upload_dpm_level_enable_mask(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	smu7_apply_minimum_dce_voltage_request(hwmgr);

	if (!data->sclk_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.sclk_dpm_enable_mask)
			smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_SCLKDPM_SetEnabledMask,
					data->dpm_level_enable_mask.sclk_dpm_enable_mask,
					NULL);
	}

	if (!data->mclk_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.mclk_dpm_enable_mask)
			smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_MCLKDPM_SetEnabledMask,
					data->dpm_level_enable_mask.mclk_dpm_enable_mask,
					NULL);
	}

	return 0;
}

static int smu7_unforce_dpm_levels(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (!smum_is_dpm_running(hwmgr))
		return -EINVAL;

	if (!data->pcie_dpm_key_disabled) {
		smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_PCIeDPM_UnForceLevel,
				NULL);
	}

	return smu7_upload_dpm_level_enable_mask(hwmgr);
}

static int smu7_force_dpm_lowest(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data =
			(struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t level;

	if (!data->sclk_dpm_key_disabled)
		if (data->dpm_level_enable_mask.sclk_dpm_enable_mask) {
			level = phm_get_lowest_enabled_level(hwmgr,
							      data->dpm_level_enable_mask.sclk_dpm_enable_mask);
			smum_send_msg_to_smc_with_parameter(hwmgr,
							    PPSMC_MSG_SCLKDPM_SetEnabledMask,
							    (1 << level),
							    NULL);

	}

	if (!data->mclk_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.mclk_dpm_enable_mask) {
			level = phm_get_lowest_enabled_level(hwmgr,
							      data->dpm_level_enable_mask.mclk_dpm_enable_mask);
			smum_send_msg_to_smc_with_parameter(hwmgr,
							    PPSMC_MSG_MCLKDPM_SetEnabledMask,
							    (1 << level),
							    NULL);
		}
	}

	if (!data->pcie_dpm_key_disabled) {
		if (data->dpm_level_enable_mask.pcie_dpm_enable_mask) {
			level = phm_get_lowest_enabled_level(hwmgr,
							      data->dpm_level_enable_mask.pcie_dpm_enable_mask);
			smum_send_msg_to_smc_with_parameter(hwmgr,
							    PPSMC_MSG_PCIeDPM_ForceLevel,
							    (level),
							    NULL);
		}
	}

	return 0;
}

static int smu7_get_profiling_clk(struct pp_hwmgr *hwmgr, enum amd_dpm_forced_level level,
				uint32_t *sclk_mask, uint32_t *mclk_mask, uint32_t *pcie_mask)
{
	uint32_t percentage;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_dpm_table *golden_dpm_table = &data->golden_dpm_table;
	int32_t tmp_mclk;
	int32_t tmp_sclk;
	int32_t count;

	if (golden_dpm_table->mclk_table.count < 1)
		return -EINVAL;

	percentage = 100 * golden_dpm_table->sclk_table.dpm_levels[golden_dpm_table->sclk_table.count - 1].value /
			golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 1].value;

	if (golden_dpm_table->mclk_table.count == 1) {
		percentage = 70;
		tmp_mclk = golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 1].value;
		*mclk_mask = golden_dpm_table->mclk_table.count - 1;
	} else {
		tmp_mclk = golden_dpm_table->mclk_table.dpm_levels[golden_dpm_table->mclk_table.count - 2].value;
		*mclk_mask = golden_dpm_table->mclk_table.count - 2;
	}

	tmp_sclk = tmp_mclk * percentage / 100;

	{
		struct phm_ppt_v1_information *table_info =
				(struct phm_ppt_v1_information *)(hwmgr->pptable);

		for (count = table_info->vdd_dep_on_sclk->count-1; count >= 0; count--) {
			if (tmp_sclk >= table_info->vdd_dep_on_sclk->entries[count].clk) {
				*sclk_mask = count;
				break;
			}
		}
		if (count < 0 || level == AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK)
			*sclk_mask = 0;

		if (level == AMD_DPM_FORCED_LEVEL_PROFILE_PEAK)
			*sclk_mask = table_info->vdd_dep_on_sclk->count - 1;
	}

	if (level == AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK)
		*mclk_mask = 0;
	else if (level == AMD_DPM_FORCED_LEVEL_PROFILE_PEAK)
		*mclk_mask = golden_dpm_table->mclk_table.count - 1;

	*pcie_mask = data->dpm_table.pcie_speed_table.count - 1;

	return 0;
}


static int smu7_force_dpm_level(struct pp_hwmgr *hwmgr,
				enum amd_dpm_forced_level level)
{
	int ret = 0;
	uint32_t sclk_mask = 0;
	uint32_t mclk_mask = 0;
	uint32_t pcie_mask = 0;

	switch (level) {
	case AMD_DPM_FORCED_LEVEL_HIGH:
		ret = smu7_force_dpm_highest(hwmgr);
		break;
	case AMD_DPM_FORCED_LEVEL_LOW:
		ret = smu7_force_dpm_lowest(hwmgr);
		break;
	case AMD_DPM_FORCED_LEVEL_AUTO:
		ret = smu7_unforce_dpm_levels(hwmgr);
		break;
	case AMD_DPM_FORCED_LEVEL_PROFILE_STANDARD:
	case AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK:
	case AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK:
	case AMD_DPM_FORCED_LEVEL_PROFILE_PEAK:
		ret = smu7_get_profiling_clk(hwmgr, level, &sclk_mask, &mclk_mask, &pcie_mask);
		if (ret)
			return ret;
		smu7_force_clock_level(hwmgr, PP_SCLK, 1<<sclk_mask);
		smu7_force_clock_level(hwmgr, PP_MCLK, 1<<mclk_mask);
		smu7_force_clock_level(hwmgr, PP_PCIE, 1<<pcie_mask);
		break;
	case AMD_DPM_FORCED_LEVEL_MANUAL:
	case AMD_DPM_FORCED_LEVEL_PROFILE_EXIT:
	default:
		break;
	}

	if (!ret) {
		if (level == AMD_DPM_FORCED_LEVEL_PROFILE_PEAK && hwmgr->dpm_level != AMD_DPM_FORCED_LEVEL_PROFILE_PEAK)
			smu7_fan_ctrl_set_fan_speed_pwm(hwmgr, 255);
		else if (level != AMD_DPM_FORCED_LEVEL_PROFILE_PEAK && hwmgr->dpm_level == AMD_DPM_FORCED_LEVEL_PROFILE_PEAK)
			smu7_fan_ctrl_reset_fan_speed_to_default(hwmgr);
	}
	return ret;
}

static int smu7_get_power_state_size(struct pp_hwmgr *hwmgr)
{
	return sizeof(struct smu7_power_state);
}

static int smu7_vblank_too_short(struct pp_hwmgr *hwmgr,
				 uint32_t vblank_time_us)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t switch_limit_us;

	switch (hwmgr->chip_id) {
	case CHIP_POLARIS10:
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		if (hwmgr->is_kicker || (hwmgr->chip_id == CHIP_POLARIS12))
			switch_limit_us = data->is_memory_gddr5 ? 450 : 150;
		else
			switch_limit_us = data->is_memory_gddr5 ? 200 : 150;
		break;
	case CHIP_VEGAM:
		switch_limit_us = 30;
		break;
	default:
		switch_limit_us = data->is_memory_gddr5 ? 450 : 150;
		break;
	}

	if (vblank_time_us < switch_limit_us)
		return true;
	else
		return false;
}

static int smu7_apply_state_adjust_rules(struct pp_hwmgr *hwmgr,
				struct pp_power_state *request_ps,
			const struct pp_power_state *current_ps)
{
	struct smu7_power_state *smu7_ps;
	uint32_t sclk;
	uint32_t mclk;
	struct PP_Clocks minimum_clocks = {0};
	bool disable_mclk_switching;
	bool disable_mclk_switching_for_frame_lock;
	bool disable_mclk_switching_for_display;
	const struct phm_clock_and_voltage_limits *max_limits;
	uint32_t i;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	int32_t count;
	int32_t stable_pstate_sclk = 0, stable_pstate_mclk = 0;
	uint32_t latency;
	bool latency_allowed = false;

	smu7_ps = cast_phw_smu7_power_state(&request_ps->hardware);
	if (!smu7_ps)
		return -EINVAL;

	data->battery_state = (PP_StateUILabel_Battery ==
			request_ps->classification.ui_label);
	data->mclk_ignore_signal = false;

	/* adev->pm.ac_power: a desktop card, always on AC */
	max_limits = &(hwmgr->dyn_state.max_clock_voltage_on_ac);

	/* Cap clock DPM tables at DC MAX if it is in DC. */
	if (0) {
		for (i = 0; i < smu7_ps->performance_level_count; i++) {
			if (smu7_ps->performance_levels[i].memory_clock > max_limits->mclk)
				smu7_ps->performance_levels[i].memory_clock = max_limits->mclk;
			if (smu7_ps->performance_levels[i].engine_clock > max_limits->sclk)
				smu7_ps->performance_levels[i].engine_clock = max_limits->sclk;
		}
	}

	minimum_clocks.engineClock = hwmgr->display_config->min_core_set_clock;
	minimum_clocks.memoryClock = hwmgr->display_config->min_mem_set_clock;

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_StablePState)) {
		max_limits = &(hwmgr->dyn_state.max_clock_voltage_on_ac);
		stable_pstate_sclk = (max_limits->sclk * 75) / 100;

		for (count = table_info->vdd_dep_on_sclk->count - 1;
				count >= 0; count--) {
			if (stable_pstate_sclk >=
					table_info->vdd_dep_on_sclk->entries[count].clk) {
				stable_pstate_sclk =
						table_info->vdd_dep_on_sclk->entries[count].clk;
				break;
			}
		}

		if (count < 0)
			stable_pstate_sclk = table_info->vdd_dep_on_sclk->entries[0].clk;

		stable_pstate_mclk = max_limits->mclk;

		minimum_clocks.engineClock = stable_pstate_sclk;
		minimum_clocks.memoryClock = stable_pstate_mclk;
	}

	disable_mclk_switching_for_frame_lock = phm_cap_enabled(
				    hwmgr->platform_descriptor.platformCaps,
				    PHM_PlatformCaps_DisableMclkSwitchingForFrameLock);

	disable_mclk_switching_for_display = ((1 < hwmgr->display_config->num_display) &&
						!hwmgr->display_config->multi_monitor_in_sync) ||
						(hwmgr->display_config->num_display &&
						smu7_vblank_too_short(hwmgr, hwmgr->display_config->min_vblank_time));

	disable_mclk_switching = disable_mclk_switching_for_frame_lock ||
					 disable_mclk_switching_for_display;

	if (hwmgr->display_config->num_display == 0) {
		if (hwmgr->chip_id >= CHIP_POLARIS10 && hwmgr->chip_id <= CHIP_VEGAM)
			data->mclk_ignore_signal = true;
		else
			disable_mclk_switching = false;
	}

	sclk = smu7_ps->performance_levels[0].engine_clock;
	mclk = smu7_ps->performance_levels[0].memory_clock;

	if (disable_mclk_switching &&
	    (!(hwmgr->chip_id >= CHIP_POLARIS10 &&
	    hwmgr->chip_id <= CHIP_VEGAM)))
		mclk = smu7_ps->performance_levels
		[smu7_ps->performance_level_count - 1].memory_clock;

	if (sclk < minimum_clocks.engineClock)
		sclk = (minimum_clocks.engineClock > max_limits->sclk) ?
				max_limits->sclk : minimum_clocks.engineClock;

	if (mclk < minimum_clocks.memoryClock)
		mclk = (minimum_clocks.memoryClock > max_limits->mclk) ?
				max_limits->mclk : minimum_clocks.memoryClock;

	smu7_ps->performance_levels[0].engine_clock = sclk;
	smu7_ps->performance_levels[0].memory_clock = mclk;

	smu7_ps->performance_levels[1].engine_clock =
		(smu7_ps->performance_levels[1].engine_clock >=
				smu7_ps->performance_levels[0].engine_clock) ?
						smu7_ps->performance_levels[1].engine_clock :
						smu7_ps->performance_levels[0].engine_clock;

	if (disable_mclk_switching) {
		if (mclk < smu7_ps->performance_levels[1].memory_clock)
			mclk = smu7_ps->performance_levels[1].memory_clock;

		if (hwmgr->chip_id >= CHIP_POLARIS10 && hwmgr->chip_id <= CHIP_VEGAM) {
			if (disable_mclk_switching_for_display) {
				/* Find the lowest MCLK frequency that is within
				 * the tolerable latency defined in DAL
				 */
				latency = hwmgr->display_config->dce_tolerable_mclk_in_active_latency;
				for (i = 0; i < data->mclk_latency_table.count; i++) {
					if (data->mclk_latency_table.entries[i].latency <= latency) {
						latency_allowed = true;

						if ((data->mclk_latency_table.entries[i].frequency >=
								smu7_ps->performance_levels[0].memory_clock) &&
						    (data->mclk_latency_table.entries[i].frequency <=
								smu7_ps->performance_levels[1].memory_clock)) {
							mclk = data->mclk_latency_table.entries[i].frequency;
							break;
						}
					}
				}
				if ((i >= data->mclk_latency_table.count - 1) && !latency_allowed) {
					data->mclk_ignore_signal = true;
				} else {
					data->mclk_ignore_signal = false;
				}
			}

			if (disable_mclk_switching_for_frame_lock)
				mclk = smu7_ps->performance_levels[1].memory_clock;
		}

		smu7_ps->performance_levels[0].memory_clock = mclk;

		if (!(hwmgr->chip_id >= CHIP_POLARIS10 &&
		      hwmgr->chip_id <= CHIP_VEGAM))
			smu7_ps->performance_levels[1].memory_clock = mclk;
	} else {
		if (smu7_ps->performance_levels[1].memory_clock <
				smu7_ps->performance_levels[0].memory_clock)
			smu7_ps->performance_levels[1].memory_clock =
					smu7_ps->performance_levels[0].memory_clock;
	}

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_StablePState)) {
		for (i = 0; i < smu7_ps->performance_level_count; i++) {
			smu7_ps->performance_levels[i].engine_clock = stable_pstate_sclk;
			smu7_ps->performance_levels[i].memory_clock = stable_pstate_mclk;
			smu7_ps->performance_levels[i].pcie_gen = data->pcie_gen_performance.max;
			smu7_ps->performance_levels[i].pcie_lane = data->pcie_gen_performance.max;
		}
	}
	return 0;
}

static uint32_t smu7_dpm_get_mclk(struct pp_hwmgr *hwmgr, bool low)
{
	struct pp_power_state  *ps;
	struct smu7_power_state  *smu7_ps;

	if (hwmgr == NULL)
		return -EINVAL;

	ps = hwmgr->request_ps;

	if (ps == NULL)
		return -EINVAL;

	smu7_ps = cast_phw_smu7_power_state(&ps->hardware);

	if (low)
		return smu7_ps->performance_levels[0].memory_clock;
	else
		return smu7_ps->performance_levels
				[smu7_ps->performance_level_count-1].memory_clock;
}

static uint32_t smu7_dpm_get_sclk(struct pp_hwmgr *hwmgr, bool low)
{
	struct pp_power_state  *ps;
	struct smu7_power_state  *smu7_ps;

	if (hwmgr == NULL)
		return -EINVAL;

	ps = hwmgr->request_ps;

	if (ps == NULL)
		return -EINVAL;

	smu7_ps = cast_phw_smu7_power_state(&ps->hardware);

	if (low)
		return smu7_ps->performance_levels[0].engine_clock;
	else
		return smu7_ps->performance_levels
				[smu7_ps->performance_level_count-1].engine_clock;
}

static int smu7_dpm_patch_boot_state(struct pp_hwmgr *hwmgr,
					struct pp_hw_power_state *hw_ps)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_power_state *ps = (struct smu7_power_state *)hw_ps;
	const uint8_t *fw_info;
	uint16_t size;
	uint8_t frev, crev;
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);

	/* First retrieve the Boot clocks and VDDC from the firmware info table.
	 * We assume here that fw_info is unchanged if this call fails.
	 */
	fw_info = smu_atom_get_data_table(hwmgr->adev, index,
			&size, &frev, &crev);
	if (!fw_info)
		/* During a test, there is no firmware info table. */
		return 0;

	/* Patch the state. */
	data->vbios_boot_state.sclk_bootup_value =
			FRB_VB32(fw_info, ATOM_FIRMWARE_INFO_V2_2, ulDefaultEngineClock);
	data->vbios_boot_state.mclk_bootup_value =
			FRB_VB32(fw_info, ATOM_FIRMWARE_INFO_V2_2, ulDefaultMemoryClock);
	data->vbios_boot_state.mvdd_bootup_value =
			FRB_VB16(fw_info, ATOM_FIRMWARE_INFO_V2_2, usBootUpMVDDCVoltage);
	data->vbios_boot_state.vddc_bootup_value =
			FRB_VB16(fw_info, ATOM_FIRMWARE_INFO_V2_2, usBootUpVDDCVoltage);
	data->vbios_boot_state.vddci_bootup_value =
			FRB_VB16(fw_info, ATOM_FIRMWARE_INFO_V2_2, usBootUpVDDCIVoltage);
	data->vbios_boot_state.pcie_gen_bootup_value =
			smu7_get_current_pcie_speed(hwmgr);

	data->vbios_boot_state.pcie_lane_bootup_value =
			(uint16_t)smu7_get_current_pcie_lane_number(hwmgr);

	/* set boot power state */
	ps->performance_levels[0].memory_clock = data->vbios_boot_state.mclk_bootup_value;
	ps->performance_levels[0].engine_clock = data->vbios_boot_state.sclk_bootup_value;
	ps->performance_levels[0].pcie_gen = data->vbios_boot_state.pcie_gen_bootup_value;
	ps->performance_levels[0].pcie_lane = data->vbios_boot_state.pcie_lane_bootup_value;

	return 0;
}

static int smu7_get_number_of_powerplay_table_entries(struct pp_hwmgr *hwmgr)
{
	return get_number_of_powerplay_table_entries_v1_0(hwmgr);
}

/*
 * The state's two levels from the VBIOS. ulMclk and ulSclk are read
 * little-endian (Linux reads them raw: its LE host makes that right).
 */
static int smu7_get_pp_table_entry_callback_func_v1(struct pp_hwmgr *hwmgr,
		const uint8_t *state, struct pp_power_state *power_state,
		const uint8_t *pp_table, uint32_t classification_flag)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_power_state  *smu7_power_state =
			(struct smu7_power_state *)(&(power_state->hardware));
	struct smu7_performance_level *performance_level;
	const uint8_t *state_entry = state;
	const uint8_t *powerplay_table = pp_table;
	const uint8_t *sclk_dep_table = powerplay_table +
			FRB_VB16(powerplay_table, ATOM_Tonga_POWERPLAYTABLE, usSclkDependencyTableOffset);
	const uint8_t *mclk_dep_table = powerplay_table +
			FRB_VB16(powerplay_table, ATOM_Tonga_POWERPLAYTABLE, usMclkDependencyTableOffset);
	uint8_t sclk_rev = FRB_VB8(sclk_dep_table, PPTable_Generic_SubTable_Header, ucRevId);
	uint32_t caps = FRB_VB32(state_entry, ATOM_Tonga_State, ulCapsAndSettings);

#define ST8(f)		FRB_VB8(state_entry, ATOM_Tonga_State, f)
#define MCLK(i)		FRB_VB32(FRB_VB_ENTRY(mclk_dep_table, ATOM_Tonga_MCLK_Dependency_Table, entries, \
					      ATOM_Tonga_MCLK_Dependency_Record, (i)), ATOM_Tonga_MCLK_Dependency_Record, ulMclk)
#define TSCLK(i)	FRB_VB32(FRB_VB_ENTRY(sclk_dep_table, ATOM_Tonga_SCLK_Dependency_Table, entries, \
					      ATOM_Tonga_SCLK_Dependency_Record, (i)), ATOM_Tonga_SCLK_Dependency_Record, ulSclk)
#define PSCLK(i)	FRB_VB32(FRB_VB_ENTRY(sclk_dep_table, ATOM_Polaris_SCLK_Dependency_Table, entries, \
					      ATOM_Polaris_SCLK_Dependency_Record, (i)), ATOM_Polaris_SCLK_Dependency_Record, ulSclk)

	/* The following fields are not initialized here: id orderedList allStatesList */
	power_state->classification.ui_label =
			(FRB_VB16(state_entry, ATOM_Tonga_State, usClassification) &
			ATOM_PPLIB_CLASSIFICATION_UI_MASK) >>
			ATOM_PPLIB_CLASSIFICATION_UI_SHIFT;
	power_state->classification.flags = classification_flag;
	/* NOTE: There is a classification2 flag in BIOS that is not being used right now */

	power_state->classification.temporary_state = false;
	power_state->classification.to_be_deleted = false;

	power_state->validation.disallowOnDC =
			(0 != (caps & ATOM_Tonga_DISALLOW_ON_DC));

	power_state->pcie.lanes = 0;

	power_state->display.disableFrameModulation = false;
	power_state->display.limitRefreshrate = false;
	power_state->display.enableVariBright =
			(0 != (caps & ATOM_Tonga_ENABLE_VARIBRIGHT));

	power_state->validation.supportedPowerLevels = 0;
	power_state->uvd_clocks.VCLK = 0;
	power_state->uvd_clocks.DCLK = 0;
	power_state->temperatures.min = 0;
	power_state->temperatures.max = 0;

	performance_level = &(smu7_power_state->performance_levels
			[smu7_power_state->performance_level_count++]);

	PP_ASSERT_WITH_CODE(
			(smu7_power_state->performance_level_count < smum_get_mac_definition(hwmgr, SMU_MAX_LEVELS_GRAPHICS)),
			"Performance levels exceeds SMC limit!",
			return -EINVAL);

	PP_ASSERT_WITH_CODE(
			(smu7_power_state->performance_level_count <
					hwmgr->platform_descriptor.hardwareActivityPerformanceLevels),
			"Performance levels exceeds Driver limit!",
			return -EINVAL);

	/* Performance levels are arranged from low to high. */
	performance_level->memory_clock = MCLK(ST8(ucMemoryClockIndexLow));
	if (sclk_rev == 0)
		performance_level->engine_clock = TSCLK(ST8(ucEngineClockIndexLow));
	else if (sclk_rev == 1)
		performance_level->engine_clock = PSCLK(ST8(ucEngineClockIndexLow));
	performance_level->pcie_gen = get_pcie_gen_support(data->pcie_gen_cap,
			ST8(ucPCIEGenLow));
	performance_level->pcie_lane = get_pcie_lane_support(data->pcie_lane_cap,
			ST8(ucPCIELaneLow));

	performance_level = &(smu7_power_state->performance_levels
			[smu7_power_state->performance_level_count++]);
	performance_level->memory_clock = MCLK(ST8(ucMemoryClockIndexHigh));

	if (sclk_rev == 0)
		performance_level->engine_clock = TSCLK(ST8(ucEngineClockIndexHigh));
	else if (sclk_rev == 1)
		performance_level->engine_clock = PSCLK(ST8(ucEngineClockIndexHigh));

	performance_level->pcie_gen = get_pcie_gen_support(data->pcie_gen_cap,
			ST8(ucPCIEGenHigh));
	performance_level->pcie_lane = get_pcie_lane_support(data->pcie_lane_cap,
			ST8(ucPCIELaneHigh));
#undef ST8
#undef MCLK
#undef TSCLK
#undef PSCLK

	return 0;
}

static int smu7_get_pp_table_entry_v1(struct pp_hwmgr *hwmgr,
		unsigned long entry_index, struct pp_power_state *state)
{
	int result;
	struct smu7_power_state *ps;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	struct phm_ppt_v1_clock_voltage_dependency_table *dep_mclk_table =
			table_info->vdd_dep_on_mclk;

	state->hardware.magic = PHM_VIslands_Magic;

	ps = (struct smu7_power_state *)(&state->hardware);

	result = get_powerplay_table_entry_v1_0(hwmgr, entry_index, state,
			smu7_get_pp_table_entry_callback_func_v1);

	/* This is the earliest time we have all the dependency table and the VBIOS boot state
	 * as PP_Tables_GetPowerPlayTableEntry retrieves the VBIOS boot state
	 * if there is only one VDDCI/MCLK level, check if it's the same as VBIOS boot state
	 */
	if (dep_mclk_table != NULL && dep_mclk_table->count == 1) {
		if (dep_mclk_table->entries[0].clk !=
				data->vbios_boot_state.mclk_bootup_value)
			pr_debug("Single MCLK entry VDDCI/MCLK dependency table "
					"does not match VBIOS boot MCLK level");
		if (dep_mclk_table->entries[0].vddci !=
				data->vbios_boot_state.vddci_bootup_value)
			pr_debug("Single VDDCI entry VDDCI/MCLK dependency table "
					"does not match VBIOS boot VDDCI level");
	}

	/* set DC compatible flag if this state supports DC */
	if (!state->validation.disallowOnDC)
		ps->dc_compatible = true;

	if (state->classification.flags & PP_StateClassificationFlag_ACPI)
		data->acpi_pcie_gen = ps->performance_levels[0].pcie_gen;

	ps->uvd_clks.vclk = state->uvd_clocks.VCLK;
	ps->uvd_clks.dclk = state->uvd_clocks.DCLK;

	if (!result) {
		uint32_t i;

		switch (state->classification.ui_label) {
		case PP_StateUILabel_Performance:
			data->use_pcie_performance_levels = true;
			for (i = 0; i < ps->performance_level_count; i++) {
				if (data->pcie_gen_performance.max <
						ps->performance_levels[i].pcie_gen)
					data->pcie_gen_performance.max =
							ps->performance_levels[i].pcie_gen;

				if (data->pcie_gen_performance.min >
						ps->performance_levels[i].pcie_gen)
					data->pcie_gen_performance.min =
							ps->performance_levels[i].pcie_gen;

				if (data->pcie_lane_performance.max <
						ps->performance_levels[i].pcie_lane)
					data->pcie_lane_performance.max =
							ps->performance_levels[i].pcie_lane;
				if (data->pcie_lane_performance.min >
						ps->performance_levels[i].pcie_lane)
					data->pcie_lane_performance.min =
							ps->performance_levels[i].pcie_lane;
			}
			break;
		case PP_StateUILabel_Battery:
			data->use_pcie_power_saving_levels = true;

			for (i = 0; i < ps->performance_level_count; i++) {
				if (data->pcie_gen_power_saving.max <
						ps->performance_levels[i].pcie_gen)
					data->pcie_gen_power_saving.max =
							ps->performance_levels[i].pcie_gen;

				if (data->pcie_gen_power_saving.min >
						ps->performance_levels[i].pcie_gen)
					data->pcie_gen_power_saving.min =
							ps->performance_levels[i].pcie_gen;

				if (data->pcie_lane_power_saving.max <
						ps->performance_levels[i].pcie_lane)
					data->pcie_lane_power_saving.max =
							ps->performance_levels[i].pcie_lane;

				if (data->pcie_lane_power_saving.min >
						ps->performance_levels[i].pcie_lane)
					data->pcie_lane_power_saving.min =
							ps->performance_levels[i].pcie_lane;
			}
			break;
		default:
			break;
		}
	}
	return 0;
}

static int smu7_get_pp_table_entry(struct pp_hwmgr *hwmgr,
		unsigned long entry_index, struct pp_power_state *state)
{
	return smu7_get_pp_table_entry_v1(hwmgr, entry_index, state);
}

static int smu7_get_gpu_power(struct pp_hwmgr *hwmgr, u32 *query)
{
	struct frb_device *adev = hwmgr->adev;
	int i;
	u32 tmp = 0;

	if (!query)
		return -EINVAL;

	/*
	 * PPSMC_MSG_GetCurrPkgPwr is not supported on:
	 *  - Hawaii
	 *  - Bonaire
	 *  - Fiji
	 *  - Tonga
	 */
	(void)adev;
	{	/* Polaris has it */
		smum_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_GetCurrPkgPwr, 0, &tmp);
		*query = tmp;

		if (tmp != 0)
			return 0;
	}

	smum_send_msg_to_smc(hwmgr, PPSMC_MSG_PmStatusLogStart, NULL);
	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
							ixSMU_PM_STATUS_95, 0);

	for (i = 0; i < 10; i++) {
		mdelay(500);
		smum_send_msg_to_smc(hwmgr, PPSMC_MSG_PmStatusLogSample, NULL);
		tmp = cgs_read_ind_register(hwmgr->device,
						CGS_IND_REG__SMC,
						ixSMU_PM_STATUS_95);
		if (tmp != 0)
			break;
	}
	*query = tmp;

	return 0;
}

static int smu7_read_sensor(struct pp_hwmgr *hwmgr, int idx,
			    void *value, int *size)
{
	uint32_t sclk, mclk, activity_percent;
	uint32_t offset, val_vid;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	int ret = 0;

	/* size must be at least 4 bytes for all sensors */
	if (*size < 4)
		return -EINVAL;

	switch (idx) {
	case AMDGPU_PP_SENSOR_GFX_SCLK:
		ret = smum_send_msg_to_smc(hwmgr, PPSMC_MSG_API_GetSclkFrequency, &sclk);
		if (ret)
			return ret;
		*((uint32_t *)value) = sclk;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_GFX_MCLK:
		ret = smum_send_msg_to_smc(hwmgr, PPSMC_MSG_API_GetMclkFrequency, &mclk);
		if (ret)
			return ret;
		*((uint32_t *)value) = mclk;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_GPU_LOAD:
	case AMDGPU_PP_SENSOR_MEM_LOAD:
		offset = data->soft_regs_start + smum_get_offsetof(hwmgr,
								SMU_SoftRegisters,
								(idx == AMDGPU_PP_SENSOR_GPU_LOAD) ?
								AverageGraphicsActivity :
								AverageMemoryActivity);

		activity_percent = cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, offset);
		activity_percent += 0x80;
		activity_percent >>= 8;
		*((uint32_t *)value) = activity_percent > 100 ? 100 : activity_percent;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_GPU_TEMP:
		*((uint32_t *)value) = smu7_thermal_get_temperature(hwmgr);
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_UVD_POWER:
		*((uint32_t *)value) = data->uvd_power_gated ? 0 : 1;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_VCE_POWER:
		*((uint32_t *)value) = data->vce_power_gated ? 0 : 1;
		*size = 4;
		return 0;
	case AMDGPU_PP_SENSOR_GPU_INPUT_POWER:
		return smu7_get_gpu_power(hwmgr, (uint32_t *)value);
	case AMDGPU_PP_SENSOR_GPU_AVG_POWER:
		return -EOPNOTSUPP;
	case AMDGPU_PP_SENSOR_VDDGFX:
		if ((data->vr_config & VRCONF_VDDGFX_MASK) ==
		    (VR_SVI2_PLANE_2 << VRCONF_VDDGFX_SHIFT))
			val_vid = PHM_READ_INDIRECT_FIELD(hwmgr->device,
					CGS_IND_REG__SMC, PWR_SVI2_STATUS, PLANE2_VID);
		else
			val_vid = PHM_READ_INDIRECT_FIELD(hwmgr->device,
					CGS_IND_REG__SMC, PWR_SVI2_STATUS, PLANE1_VID);

		*((uint32_t *)value) = (uint32_t)convert_to_vddc(val_vid);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int smu7_find_dpm_states_clocks_in_dpm_table(struct pp_hwmgr *hwmgr, const void *input)
{
	const struct phm_set_power_state_input *states =
			(const struct phm_set_power_state_input *)input;
	const struct smu7_power_state *smu7_ps =
			cast_const_phw_smu7_power_state(states->pnew_state);
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_single_dpm_table *sclk_table = &(data->dpm_table.sclk_table);
	uint32_t sclk = smu7_ps->performance_levels
			[smu7_ps->performance_level_count - 1].engine_clock;
	struct smu7_single_dpm_table *mclk_table = &(data->dpm_table.mclk_table);
	uint32_t mclk = smu7_ps->performance_levels
			[smu7_ps->performance_level_count - 1].memory_clock;
	struct PP_Clocks min_clocks = {0};
	uint32_t i;

	for (i = 0; i < sclk_table->count; i++) {
		if (sclk == sclk_table->dpm_levels[i].value)
			break;
	}

	if (i >= sclk_table->count) {
		if (sclk > sclk_table->dpm_levels[i-1].value) {
			data->need_update_smu7_dpm_table |= DPMTABLE_OD_UPDATE_SCLK;
			sclk_table->dpm_levels[i-1].value = sclk;
		}
	} else {
	/* TODO: Check SCLK in DAL's minimum clocks
	 * in case DeepSleep divider update is required.
	 */
		if (data->display_timing.min_clock_in_sr != min_clocks.engineClockInSR &&
			(min_clocks.engineClockInSR >= SMU7_MINIMUM_ENGINE_CLOCK ||
				data->display_timing.min_clock_in_sr >= SMU7_MINIMUM_ENGINE_CLOCK))
			data->need_update_smu7_dpm_table |= DPMTABLE_UPDATE_SCLK;
	}

	for (i = 0; i < mclk_table->count; i++) {
		if (mclk == mclk_table->dpm_levels[i].value)
			break;
	}

	if (i >= mclk_table->count) {
		if (mclk > mclk_table->dpm_levels[i-1].value) {
			data->need_update_smu7_dpm_table |= DPMTABLE_OD_UPDATE_MCLK;
			mclk_table->dpm_levels[i-1].value = mclk;
		}
	}

	if (data->display_timing.num_existing_displays != hwmgr->display_config->num_display)
		data->need_update_smu7_dpm_table |= DPMTABLE_UPDATE_MCLK;

	return 0;
}

static uint16_t smu7_get_maximum_link_speed(struct pp_hwmgr *hwmgr,
		const struct smu7_power_state *smu7_ps)
{
	uint32_t i;
	uint32_t sclk, max_sclk = 0;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_dpm_table *dpm_table = &data->dpm_table;

	for (i = 0; i < smu7_ps->performance_level_count; i++) {
		sclk = smu7_ps->performance_levels[i].engine_clock;
		if (max_sclk < sclk)
			max_sclk = sclk;
	}

	for (i = 0; i < dpm_table->sclk_table.count; i++) {
		if (dpm_table->sclk_table.dpm_levels[i].value == max_sclk)
			return (uint16_t) ((i >= dpm_table->pcie_speed_table.count) ?
					dpm_table->pcie_speed_table.dpm_levels
					[dpm_table->pcie_speed_table.count - 1].value :
					dpm_table->pcie_speed_table.dpm_levels[i].value);
	}

	return 0;
}

static int smu7_request_link_speed_change_before_state_change(
		struct pp_hwmgr *hwmgr, const void *input)
{
	const struct phm_set_power_state_input *states =
			(const struct phm_set_power_state_input *)input;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	const struct smu7_power_state *smu7_nps =
			cast_const_phw_smu7_power_state(states->pnew_state);
	const struct smu7_power_state *polaris10_cps =
			cast_const_phw_smu7_power_state(states->pcurrent_state);

	uint16_t target_link_speed = smu7_get_maximum_link_speed(hwmgr, smu7_nps);
	uint16_t current_link_speed;

	if (data->force_pcie_gen == PP_PCIEGenInvalid)
		current_link_speed = smu7_get_maximum_link_speed(hwmgr, polaris10_cps);
	else
		current_link_speed = data->force_pcie_gen;

	data->force_pcie_gen = PP_PCIEGenInvalid;
	data->pspp_notify_required = false;

	if (target_link_speed > current_link_speed) {
		switch (target_link_speed) {
#ifdef CONFIG_ACPI
		case PP_PCIEGen3:
			if (0 == amdgpu_acpi_pcie_performance_request(hwmgr->adev, PCIE_PERF_REQ_GEN3, false))
				break;
			data->force_pcie_gen = PP_PCIEGen2;
			if (current_link_speed == PP_PCIEGen2)
				break;
			fallthrough;
		case PP_PCIEGen2:
			if (0 == amdgpu_acpi_pcie_performance_request(hwmgr->adev, PCIE_PERF_REQ_GEN2, false))
				break;
			fallthrough;
#endif
		default:
			data->force_pcie_gen = smu7_get_current_pcie_speed(hwmgr);
			break;
		}
	} else {
		if (target_link_speed < current_link_speed)
			data->pspp_notify_required = true;
	}

	return 0;
}

static int smu7_freeze_sclk_mclk_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (0 == data->need_update_smu7_dpm_table)
		return 0;

	if ((0 == data->sclk_dpm_key_disabled) &&
		(data->need_update_smu7_dpm_table &
			(DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK))) {
		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to freeze SCLK DPM when DPM is disabled",
				);
		PP_ASSERT_WITH_CODE(0 == smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_SCLKDPM_FreezeLevel,
				NULL),
				"Failed to freeze SCLK DPM during FreezeSclkMclkDPM Function!",
				return -EINVAL);
	}

	if ((0 == data->mclk_dpm_key_disabled) &&
		!data->mclk_ignore_signal &&
		(data->need_update_smu7_dpm_table &
		 DPMTABLE_OD_UPDATE_MCLK)) {
		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to freeze MCLK DPM when DPM is disabled",
				);
		PP_ASSERT_WITH_CODE(0 == smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_MCLKDPM_FreezeLevel,
				NULL),
				"Failed to freeze MCLK DPM during FreezeSclkMclkDPM Function!",
				return -EINVAL);
	}

	return 0;
}

static int smu7_populate_and_upload_sclk_mclk_dpm_levels(
		struct pp_hwmgr *hwmgr, const void *input)
{
	int result = 0;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct smu7_dpm_table *dpm_table = &data->dpm_table;
	uint32_t count;
	struct smu7_odn_dpm_table *odn_table = &(data->odn_dpm_table);
	struct phm_odn_clock_levels *odn_sclk_table = &(odn_table->odn_core_clock_dpm_levels);
	struct phm_odn_clock_levels *odn_mclk_table = &(odn_table->odn_memory_clock_dpm_levels);

	if (0 == data->need_update_smu7_dpm_table)
		return 0;

	if (hwmgr->od_enabled && data->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_SCLK) {
		for (count = 0; count < dpm_table->sclk_table.count; count++) {
			dpm_table->sclk_table.dpm_levels[count].enabled = odn_sclk_table->entries[count].enabled;
			dpm_table->sclk_table.dpm_levels[count].value = odn_sclk_table->entries[count].clock;
		}
	}

	if (hwmgr->od_enabled && data->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK) {
		for (count = 0; count < dpm_table->mclk_table.count; count++) {
			dpm_table->mclk_table.dpm_levels[count].enabled = odn_mclk_table->entries[count].enabled;
			dpm_table->mclk_table.dpm_levels[count].value = odn_mclk_table->entries[count].clock;
		}
	}

	if (data->need_update_smu7_dpm_table &
			(DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK)) {
		result = smum_populate_all_graphic_levels(hwmgr);
		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to populate SCLK during PopulateNewDPMClocksStates Function!",
				return result);
	}

	if (data->need_update_smu7_dpm_table &
			(DPMTABLE_OD_UPDATE_MCLK | DPMTABLE_UPDATE_MCLK)) {
		/*populate MCLK dpm table to SMU7 */
		result = smum_populate_all_memory_levels(hwmgr);
		PP_ASSERT_WITH_CODE((0 == result),
				"Failed to populate MCLK during PopulateNewDPMClocksStates Function!",
				return result);
	}

	return result;
}

static int smu7_trim_single_dpm_states(struct pp_hwmgr *hwmgr,
			  struct smu7_single_dpm_table *dpm_table,
			uint32_t low_limit, uint32_t high_limit)
{
	uint32_t i;

	/* force the trim if mclk_switching is disabled to prevent flicker */
	bool force_trim = (low_limit == high_limit);
	for (i = 0; i < dpm_table->count; i++) {
	/*skip the trim if od is enabled*/
		if ((!hwmgr->od_enabled || force_trim)
			&& (dpm_table->dpm_levels[i].value < low_limit
			|| dpm_table->dpm_levels[i].value > high_limit))
			dpm_table->dpm_levels[i].enabled = false;
		else
			dpm_table->dpm_levels[i].enabled = true;
	}

	return 0;
}

static int smu7_trim_dpm_states(struct pp_hwmgr *hwmgr,
		const struct smu7_power_state *smu7_ps)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t high_limit_count;

	PP_ASSERT_WITH_CODE((smu7_ps->performance_level_count >= 1),
			"power state did not have any performance level",
			return -EINVAL);

	high_limit_count = (1 == smu7_ps->performance_level_count) ? 0 : 1;

	smu7_trim_single_dpm_states(hwmgr,
			&(data->dpm_table.sclk_table),
			smu7_ps->performance_levels[0].engine_clock,
			smu7_ps->performance_levels[high_limit_count].engine_clock);

	smu7_trim_single_dpm_states(hwmgr,
			&(data->dpm_table.mclk_table),
			smu7_ps->performance_levels[0].memory_clock,
			smu7_ps->performance_levels[high_limit_count].memory_clock);

	return 0;
}

static int smu7_generate_dpm_level_enable_mask(
		struct pp_hwmgr *hwmgr, const void *input)
{
	int result = 0;
	const struct phm_set_power_state_input *states =
			(const struct phm_set_power_state_input *)input;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	const struct smu7_power_state *smu7_ps =
			cast_const_phw_smu7_power_state(states->pnew_state);


	result = smu7_trim_dpm_states(hwmgr, smu7_ps);
	if (result)
		return result;

	data->dpm_level_enable_mask.sclk_dpm_enable_mask =
			phm_get_dpm_level_enable_mask_value(&data->dpm_table.sclk_table);
	data->dpm_level_enable_mask.mclk_dpm_enable_mask =
			phm_get_dpm_level_enable_mask_value(&data->dpm_table.mclk_table);
	data->dpm_level_enable_mask.pcie_dpm_enable_mask =
			phm_get_dpm_level_enable_mask_value(&data->dpm_table.pcie_speed_table);

	return 0;
}

static int smu7_unfreeze_sclk_mclk_dpm(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (0 == data->need_update_smu7_dpm_table)
		return 0;

	if ((0 == data->sclk_dpm_key_disabled) &&
		(data->need_update_smu7_dpm_table &
		(DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK))) {

		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to Unfreeze SCLK DPM when DPM is disabled",
				);
		PP_ASSERT_WITH_CODE(0 == smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_SCLKDPM_UnfreezeLevel,
				NULL),
			"Failed to unfreeze SCLK DPM during UnFreezeSclkMclkDPM Function!",
			return -EINVAL);
	}

	if ((0 == data->mclk_dpm_key_disabled) &&
		!data->mclk_ignore_signal &&
		(data->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK)) {

		PP_ASSERT_WITH_CODE(true == smum_is_dpm_running(hwmgr),
				"Trying to Unfreeze MCLK DPM when DPM is disabled",
				);
		PP_ASSERT_WITH_CODE(0 == smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_MCLKDPM_UnfreezeLevel,
				NULL),
		    "Failed to unfreeze MCLK DPM during UnFreezeSclkMclkDPM Function!",
		    return -EINVAL);
	}

	data->need_update_smu7_dpm_table &= DPMTABLE_OD_UPDATE_VDDC;

	return 0;
}

static int smu7_notify_link_speed_change_after_state_change(
		struct pp_hwmgr *hwmgr, const void *input)
{
	const struct phm_set_power_state_input *states =
			(const struct phm_set_power_state_input *)input;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	const struct smu7_power_state *smu7_ps =
			cast_const_phw_smu7_power_state(states->pnew_state);
	uint16_t target_link_speed = smu7_get_maximum_link_speed(hwmgr, smu7_ps);
	uint8_t  request;

	if (data->pspp_notify_required) {
		if (target_link_speed == PP_PCIEGen3)
			request = PCIE_PERF_REQ_GEN3;
		else if (target_link_speed == PP_PCIEGen2)
			request = PCIE_PERF_REQ_GEN2;
		else
			request = PCIE_PERF_REQ_GEN1;

		if (request == PCIE_PERF_REQ_GEN1 &&
				smu7_get_current_pcie_speed(hwmgr) > 0)
			return 0;

#ifdef CONFIG_ACPI
		if (amdgpu_acpi_pcie_performance_request(hwmgr->adev, request, false)) {
			if (PP_PCIEGen2 == target_link_speed)
				pr_info("PSPP request to switch to Gen2 from Gen3 Failed!");
			else
				pr_info("PSPP request to switch to Gen1 from Gen2 Failed!");
		}
#endif
	}

	return 0;
}

static int smu7_notify_no_display(struct pp_hwmgr *hwmgr)
{
	return (smum_send_msg_to_smc(hwmgr, (PPSMC_Msg)PPSMC_NoDisplay, NULL) == 0) ?  0 : -EINVAL;
}

static int smu7_notify_has_display(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (hwmgr->feature_mask & PP_VBI_TIME_SUPPORT_MASK) {
		if (hwmgr->chip_id == CHIP_VEGAM)
			smum_send_msg_to_smc_with_parameter(hwmgr,
					(PPSMC_Msg)PPSMC_MSG_SetVBITimeout_VEGAM, data->frame_time_x2,
					NULL);
		else
			smum_send_msg_to_smc_with_parameter(hwmgr,
					(PPSMC_Msg)PPSMC_MSG_SetVBITimeout, data->frame_time_x2,
					NULL);
		data->last_sent_vbi_timeout = data->frame_time_x2;
	}

	return (smum_send_msg_to_smc(hwmgr, (PPSMC_Msg)PPSMC_HasDisplay, NULL) == 0) ?  0 : -EINVAL;
}

static int smu7_notify_smc_display(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	int result = 0;

	if (data->mclk_ignore_signal)
		result = smu7_notify_no_display(hwmgr);
	else
		result = smu7_notify_has_display(hwmgr);

	return result;
}

static int smu7_set_power_state_tasks(struct pp_hwmgr *hwmgr, const void *input)
{
	int tmp_result, result = 0;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	tmp_result = smu7_find_dpm_states_clocks_in_dpm_table(hwmgr, input);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to find DPM states clocks in DPM table!",
			result = tmp_result);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_PCIEPerformanceRequest)) {
		tmp_result =
			smu7_request_link_speed_change_before_state_change(hwmgr, input);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to request link speed change before state change!",
				result = tmp_result);
	}

	tmp_result = smu7_freeze_sclk_mclk_dpm(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to freeze SCLK MCLK DPM!", result = tmp_result);

	tmp_result = smu7_populate_and_upload_sclk_mclk_dpm_levels(hwmgr, input);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to populate and upload SCLK MCLK DPM levels!",
			result = tmp_result);

	/*
	 * If a custom pp table is loaded, set DPMTABLE_OD_UPDATE_VDDC flag.
	 * That effectively disables AVFS feature.
	 */
	if (hwmgr->hardcode_pp_table != NULL)
		data->need_update_smu7_dpm_table |= DPMTABLE_OD_UPDATE_VDDC;

	tmp_result = smu7_update_avfs(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to update avfs voltages!",
			result = tmp_result);

	tmp_result = smu7_generate_dpm_level_enable_mask(hwmgr, input);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to generate DPM level enabled mask!",
			result = tmp_result);

	tmp_result = smum_update_sclk_threshold(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to update SCLK threshold!",
			result = tmp_result);

	tmp_result = smu7_unfreeze_sclk_mclk_dpm(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to unfreeze SCLK MCLK DPM!",
			result = tmp_result);

	tmp_result = smu7_upload_dpm_level_enable_mask(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to upload DPM level enabled mask!",
			result = tmp_result);

	tmp_result = smu7_notify_smc_display(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to notify smc display settings!",
			result = tmp_result);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_PCIEPerformanceRequest)) {
		tmp_result =
			smu7_notify_link_speed_change_after_state_change(hwmgr, input);
		PP_ASSERT_WITH_CODE((0 == tmp_result),
				"Failed to notify link speed change after state change!",
				result = tmp_result);
	}
	data->apply_optimized_settings = false;
	return result;
}

static int smu7_set_max_fan_pwm_output(struct pp_hwmgr *hwmgr, uint16_t us_max_fan_pwm)
{
	hwmgr->thermal_controller.
	advanceFanControlParameters.usMaxFanPWM = us_max_fan_pwm;

	return smum_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetFanPwmMax, us_max_fan_pwm,
			NULL);
}

static int
smu7_notify_smc_display_config_after_ps_adjustment(struct pp_hwmgr *hwmgr)
{
	return 0;
}

/**
 * smu7_program_display_gap - Programs the display gap
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always OK
 */
static int smu7_program_display_gap(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	uint32_t display_gap = cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_DISPLAY_GAP_CNTL);
	uint32_t display_gap2;
	uint32_t pre_vbi_time_in_us;
	uint32_t frame_time_in_us;
	uint32_t ref_clock, refresh_rate;

	display_gap = PHM_SET_FIELD(display_gap, CG_DISPLAY_GAP_CNTL, DISP_GAP, (hwmgr->display_config->num_display > 0) ? DISPLAY_GAP_VBLANK_OR_WM : DISPLAY_GAP_IGNORE);
	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_DISPLAY_GAP_CNTL, display_gap);

	ref_clock =  frb_vi_get_xclk(hwmgr->adev);
	refresh_rate = hwmgr->display_config->vrefresh;

	if (0 == refresh_rate)
		refresh_rate = 60;

	frame_time_in_us = 1000000 / refresh_rate;

	pre_vbi_time_in_us = frame_time_in_us - 200 - hwmgr->display_config->min_vblank_time;

	data->frame_time_x2 = frame_time_in_us * 2 / 100;

	if (data->frame_time_x2 < 280) {
		pr_debug("%s: enforce minimal VBITimeout: %d -> 280\n", __func__, data->frame_time_x2);
		data->frame_time_x2 = 280;
	}

	display_gap2 = pre_vbi_time_in_us * (ref_clock / 100);

	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_DISPLAY_GAP_CNTL2, display_gap2);

	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			data->soft_regs_start + smum_get_offsetof(hwmgr,
							SMU_SoftRegisters,
							PreVBlankGap), 0x64);

	cgs_write_ind_register(hwmgr->device, CGS_IND_REG__SMC,
			data->soft_regs_start + smum_get_offsetof(hwmgr,
							SMU_SoftRegisters,
							VBlankTimeout),
					(frame_time_in_us - pre_vbi_time_in_us));

	return 0;
}

static int smu7_display_configuration_changed_task(struct pp_hwmgr *hwmgr)
{
	return smu7_program_display_gap(hwmgr);
}

/**
 * smu7_set_max_fan_rpm_output - Set maximum target operating fan output RPM
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * @us_max_fan_rpm:  max operating fan RPM value.
 * Return:   The response that came from the SMC.
 */
static int smu7_set_max_fan_rpm_output(struct pp_hwmgr *hwmgr, uint16_t us_max_fan_rpm)
{
	hwmgr->thermal_controller.
	advanceFanControlParameters.usMaxFanRPM = us_max_fan_rpm;

	return smum_send_msg_to_smc_with_parameter(hwmgr,
			PPSMC_MSG_SetFanRpmMax, us_max_fan_rpm,
			NULL);
}

static bool
smu7_check_smc_update_required_for_display_configuration(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	bool is_update_required = false;

	if (data->display_timing.num_existing_displays != hwmgr->display_config->num_display)
		is_update_required = true;

	if (data->display_timing.vrefresh != hwmgr->display_config->vrefresh)
		is_update_required = true;

	if (hwmgr->chip_id >= CHIP_POLARIS10 &&
	    hwmgr->chip_id <= CHIP_VEGAM &&
	    data->last_sent_vbi_timeout != data->frame_time_x2)
		is_update_required = true;

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps, PHM_PlatformCaps_SclkDeepSleep)) {
		if (data->display_timing.min_clock_in_sr != hwmgr->display_config->min_core_set_clock_in_sr &&
			(data->display_timing.min_clock_in_sr >= SMU7_MINIMUM_ENGINE_CLOCK ||
			hwmgr->display_config->min_core_set_clock_in_sr >= SMU7_MINIMUM_ENGINE_CLOCK))
			is_update_required = true;
	}
	return is_update_required;
}

static inline bool smu7_are_power_levels_equal(const struct smu7_performance_level *pl1,
							   const struct smu7_performance_level *pl2)
{
	return ((pl1->memory_clock == pl2->memory_clock) &&
		  (pl1->engine_clock == pl2->engine_clock) &&
		  (pl1->pcie_gen == pl2->pcie_gen) &&
		  (pl1->pcie_lane == pl2->pcie_lane));
}

static int smu7_check_states_equal(struct pp_hwmgr *hwmgr,
		const struct pp_hw_power_state *pstate1,
		const struct pp_hw_power_state *pstate2, bool *equal)
{
	const struct smu7_power_state *psa;
	const struct smu7_power_state *psb;
	int i;
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (pstate1 == NULL || pstate2 == NULL || equal == NULL)
		return -EINVAL;

	psa = cast_const_phw_smu7_power_state(pstate1);
	psb = cast_const_phw_smu7_power_state(pstate2);
	/* If the two states don't even have the same number of performance levels they cannot be the same state. */
	if (psa->performance_level_count != psb->performance_level_count) {
		*equal = false;
		return 0;
	}

	for (i = 0; i < psa->performance_level_count; i++) {
		if (!smu7_are_power_levels_equal(&(psa->performance_levels[i]), &(psb->performance_levels[i]))) {
			/* If we have found even one performance level pair that is different the states are different. */
			*equal = false;
			return 0;
		}
	}

	/* If all performance levels are the same try to use the UVD clocks to break the tie.*/
	*equal = ((psa->uvd_clks.vclk == psb->uvd_clks.vclk) && (psa->uvd_clks.dclk == psb->uvd_clks.dclk));
	*equal &= ((psa->vce_clks.evclk == psb->vce_clks.evclk) && (psa->vce_clks.ecclk == psb->vce_clks.ecclk));
	*equal &= (psa->sclk_threshold == psb->sclk_threshold);
	/* For OD call, set value based on flag */
	*equal &= !(data->need_update_smu7_dpm_table & (DPMTABLE_OD_UPDATE_SCLK |
							DPMTABLE_OD_UPDATE_MCLK |
							DPMTABLE_OD_UPDATE_VDDC));

	return 0;
}

static int smu7_check_mc_firmware(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	uint32_t tmp;

	/* Read MC indirect register offset 0x9F bits [3:0] to see
	 * if VBIOS has already loaded a full version of MC ucode
	 * or not.
	 */

	smu7_get_mc_microcode_version(hwmgr);

	data->need_long_memory_training = false;

	cgs_write_register(hwmgr->device, mmMC_SEQ_IO_DEBUG_INDEX,
							ixMC_IO_DEBUG_UP_13);
	tmp = cgs_read_register(hwmgr->device, mmMC_SEQ_IO_DEBUG_DATA);

	if (tmp & (1 << 23)) {
		data->mem_latency_high = MEM_LATENCY_HIGH;
		data->mem_latency_low = MEM_LATENCY_LOW;
		if ((hwmgr->chip_id == CHIP_POLARIS10) ||
		    (hwmgr->chip_id == CHIP_POLARIS11) ||
		    (hwmgr->chip_id == CHIP_POLARIS12))
			smum_send_msg_to_smc(hwmgr, PPSMC_MSG_EnableFFC, NULL);
	} else {
		data->mem_latency_high = 330;
		data->mem_latency_low = 330;
		if ((hwmgr->chip_id == CHIP_POLARIS10) ||
		    (hwmgr->chip_id == CHIP_POLARIS11) ||
		    (hwmgr->chip_id == CHIP_POLARIS12))
			smum_send_msg_to_smc(hwmgr, PPSMC_MSG_DisableFFC, NULL);
	}

	return 0;
}

static int smu7_read_clock_registers(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	data->clock_registers.vCG_SPLL_FUNC_CNTL         =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_FUNC_CNTL);
	data->clock_registers.vCG_SPLL_FUNC_CNTL_2       =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_FUNC_CNTL_2);
	data->clock_registers.vCG_SPLL_FUNC_CNTL_3       =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_FUNC_CNTL_3);
	data->clock_registers.vCG_SPLL_FUNC_CNTL_4       =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_FUNC_CNTL_4);
	data->clock_registers.vCG_SPLL_SPREAD_SPECTRUM   =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_SPREAD_SPECTRUM);
	data->clock_registers.vCG_SPLL_SPREAD_SPECTRUM_2 =
		cgs_read_ind_register(hwmgr->device, CGS_IND_REG__SMC, ixCG_SPLL_SPREAD_SPECTRUM_2);
	data->clock_registers.vDLL_CNTL                  =
		cgs_read_register(hwmgr->device, mmDLL_CNTL);
	data->clock_registers.vMCLK_PWRMGT_CNTL          =
		cgs_read_register(hwmgr->device, mmMCLK_PWRMGT_CNTL);
	data->clock_registers.vMPLL_AD_FUNC_CNTL         =
		cgs_read_register(hwmgr->device, mmMPLL_AD_FUNC_CNTL);
	data->clock_registers.vMPLL_DQ_FUNC_CNTL         =
		cgs_read_register(hwmgr->device, mmMPLL_DQ_FUNC_CNTL);
	data->clock_registers.vMPLL_FUNC_CNTL            =
		cgs_read_register(hwmgr->device, mmMPLL_FUNC_CNTL);
	data->clock_registers.vMPLL_FUNC_CNTL_1          =
		cgs_read_register(hwmgr->device, mmMPLL_FUNC_CNTL_1);
	data->clock_registers.vMPLL_FUNC_CNTL_2          =
		cgs_read_register(hwmgr->device, mmMPLL_FUNC_CNTL_2);
	data->clock_registers.vMPLL_SS1                  =
		cgs_read_register(hwmgr->device, mmMPLL_SS1);
	data->clock_registers.vMPLL_SS2                  =
		cgs_read_register(hwmgr->device, mmMPLL_SS2);
	return 0;

}

/**
 * smu7_get_memory_type - Find out if memory is GDDR5.
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_get_memory_type(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct frb_device *adev = hwmgr->adev;

	data->is_memory_gddr5 = (adev->gmc.vram_type == 5 /* AMDGPU_VRAM_TYPE_GDDR5 */);

	return 0;
}

/**
 * smu7_enable_acpi_power_management - Enables Dynamic Power Management by SMC
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_enable_acpi_power_management(struct pp_hwmgr *hwmgr)
{
	PHM_WRITE_INDIRECT_FIELD(hwmgr->device, CGS_IND_REG__SMC,
			GENERAL_PWRMGT, STATIC_PM_EN, 1);

	return 0;
}

/**
 * smu7_init_power_gate_state - Initialize PowerGating States for different engines
 *
 * @hwmgr:  the address of the powerplay hardware manager.
 * Return:   always 0
 */
static int smu7_init_power_gate_state(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	data->uvd_power_gated = false;
	data->vce_power_gated = false;

	return 0;
}

static int smu7_init_sclk_threshold(struct pp_hwmgr *hwmgr)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	data->low_sclk_interrupt_threshold = 0;
	return 0;
}

static int smu7_setup_asic_task(struct pp_hwmgr *hwmgr)
{
	int tmp_result, result = 0;

	smu7_check_mc_firmware(hwmgr);

	tmp_result = smu7_read_clock_registers(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to read clock registers!", result = tmp_result);

	tmp_result = smu7_get_memory_type(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to get memory type!", result = tmp_result);

	tmp_result = smu7_enable_acpi_power_management(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to enable ACPI power management!", result = tmp_result);

	tmp_result = smu7_init_power_gate_state(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to init power gate state!", result = tmp_result);

	tmp_result = smu7_get_mc_microcode_version(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to get MC microcode version!", result = tmp_result);

	tmp_result = smu7_init_sclk_threshold(hwmgr);
	PP_ASSERT_WITH_CODE((0 == tmp_result),
			"Failed to init sclk threshold!", result = tmp_result);

	return result;
}

static int smu7_force_clock_level(struct pp_hwmgr *hwmgr,
		enum pp_clock_type type, uint32_t mask)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	if (mask == 0)
		return -EINVAL;

	switch (type) {
	case PP_SCLK:
		if (!data->sclk_dpm_key_disabled)
			smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_SCLKDPM_SetEnabledMask,
					data->dpm_level_enable_mask.sclk_dpm_enable_mask & mask,
					NULL);
		break;
	case PP_MCLK:
		if (!data->mclk_dpm_key_disabled)
			smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_MCLKDPM_SetEnabledMask,
					data->dpm_level_enable_mask.mclk_dpm_enable_mask & mask,
					NULL);
		break;
	case PP_PCIE:
	{
		uint32_t tmp = mask & data->dpm_level_enable_mask.pcie_dpm_enable_mask;

		if (!data->pcie_dpm_key_disabled) {
			if (fls(tmp) != ffs(tmp))
				smum_send_msg_to_smc(hwmgr, PPSMC_MSG_PCIeDPM_UnForceLevel,
						NULL);
			else
				smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_PCIeDPM_ForceLevel,
					fls(tmp) - 1,
					NULL);
		}
		break;
	}
	default:
		break;
	}

	return 0;
}

static int smu7_get_thermal_temperature_range(struct pp_hwmgr *hwmgr,
		struct PP_TemperatureRange *thermal_data)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);
	struct phm_ppt_v1_information *table_info =
			(struct phm_ppt_v1_information *)hwmgr->pptable;

	memcpy(thermal_data, &SMU7ThermalPolicy[0], sizeof(struct PP_TemperatureRange));

	(void)data;
	thermal_data->max = table_info->cac_dtp_table->usSoftwareShutdownTemp *
		PP_TEMPERATURE_UNITS_PER_CENTIGRADES;

	thermal_data->sw_ctf_threshold = thermal_data->max;

	return 0;
}

uint8_t smu7_get_sleep_divider_id_from_clock(uint32_t clock,
		uint32_t clock_insr)
{
	uint8_t i;
	uint32_t temp;
	uint32_t min = max(clock_insr, (uint32_t)SMU7_MINIMUM_ENGINE_CLOCK);

	PP_ASSERT_WITH_CODE((clock >= min), "Engine clock can't satisfy stutter requirement!", return 0);
	for (i = SMU7_MAX_DEEPSLEEP_DIVIDER_ID;  ; i--) {
		temp = clock >> i;

		if (temp >= min || i == 0)
			break;
	}
	return i;
}

static const struct pp_hwmgr_func smu7_hwmgr_funcs = {
	.backend_init = &smu7_hwmgr_backend_init,
	.backend_fini = &smu7_hwmgr_backend_fini,
	.asic_setup = &smu7_setup_asic_task,
	.dynamic_state_management_enable = &smu7_enable_dpm_tasks,
	.apply_state_adjust_rules = smu7_apply_state_adjust_rules,
	.force_dpm_level = &smu7_force_dpm_level,
	.power_state_set = smu7_set_power_state_tasks,
	.get_power_state_size = smu7_get_power_state_size,
	.get_mclk = smu7_dpm_get_mclk,
	.get_sclk = smu7_dpm_get_sclk,
	.patch_boot_state = smu7_dpm_patch_boot_state,
	.get_pp_table_entry = smu7_get_pp_table_entry,
	.get_num_of_pp_table_entries = smu7_get_number_of_powerplay_table_entries,
	.notify_smc_display_config_after_ps_adjustment = smu7_notify_smc_display_config_after_ps_adjustment,
	.display_config_changed = smu7_display_configuration_changed_task,
	.set_max_fan_pwm_output = smu7_set_max_fan_pwm_output,
	.set_max_fan_rpm_output = smu7_set_max_fan_rpm_output,
	.stop_thermal_controller = smu7_thermal_stop_thermal_controller,
	.get_fan_speed_pwm = smu7_fan_ctrl_get_fan_speed_pwm,
	.get_fan_speed_rpm = smu7_fan_ctrl_get_fan_speed_rpm,
	.check_smc_update_required_for_display_configuration = smu7_check_smc_update_required_for_display_configuration,
	.check_states_equal = smu7_check_states_equal,
	.force_clock_level = smu7_force_clock_level,
	.read_sensor = smu7_read_sensor,
	.dynamic_state_management_disable = smu7_disable_dpm_tasks,
	.avfs_control = smu7_avfs_control,
	.start_thermal_controller = smu7_start_thermal_controller,
	.get_thermal_temperature_range = smu7_get_thermal_temperature_range,
	.set_power_limit = smu7_set_power_limit,
	.powergate_uvd = smu7_powergate_uvd,
};

/* the v1 (Tonga and later) PowerPlay table only */
int smu7_init_function_pointers(struct pp_hwmgr *hwmgr)
{
	hwmgr->hwmgr_func = &smu7_hwmgr_funcs;
	hwmgr->pptable_func = &pptable_v1_0_funcs;

	return 0;
}
