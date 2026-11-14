/*
 * pm/powerplay/hwmgr/ppatomctrl.c (Linux 7.1.1), the helpers the Polaris
 * hardware manager calls: VBIOS data tables (VoltageObjectInfo, GPIO_Pin_LUT,
 * ASIC_InternalSS_Info, SMU_Info, ASIC_ProfilingInfo, GFX_Info) and command
 * tables run through the interpreter (ComputeMemoryEnginePLL,
 * DynamicMemorySettings, ReadEfuseValue, GetVoltageInfo).
 *
 * Deviations, all byte order:
 * - Data tables are read field by field, little-endian, at offsetof()
 *   positions (FRB_VB16/32), never through a structure pointer. Linux reads
 *   some fields with no le*_to_cpu at all and so relies on its LE host; here
 *   they are read LE like the rest. Those are: atomctrl_get_svi2_info's
 *   usLoadLine_PSI, atomctrl_get_edc_hilo_leakage_offset_table's three
 *   fields, atomctrl_get_edc_leakage_table's DIDT_REG words.
 * - Command-table parameter blocks are built byte by byte in the layout an
 *   LE host's structure has (frb_ps), and results read back the same way.
 *   Linux leaves the bytes it does not set uninitialised (stack); they are
 *   zero here.
 * Not ported: SI/CI/Kong dividers, memory PLL dividers, the MC register
 * table (polaris10_initialize_mc_reg_table builds one nothing on Polaris
 * reads), leakage-by-efuse (EVV parts use GetVoltageInfo), voltage range
 * (overdrive only).
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
#include "frb_ppatomctrl.h"
#include "frb_display.h"	/* struct frb_ps, frb_ps_exec */
#include "../shared/atom/atombios.h"

/* amdgpu_atom_execute_table on a parameter block of `bytes` */
static int exec_table(struct pp_hwmgr *hwmgr, int index, struct frb_ps *ps)
{
	return frb_ps_exec(hwmgr->adev, index, ps);
}

/*
 * Set DRAM timings based on engine clock and memory clock.
 * SET_ENGINE_CLOCK_PS_ALLOCATION: ulTargetEngineClock (LE), then
 * sReserved.ulClock (LE), both in 10 kHz units.
 */
int atomctrl_set_engine_dram_timings_rv770(
		struct pp_hwmgr *hwmgr,
		uint32_t engine_clock,
		uint32_t memory_clock)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, offsetof(SET_ENGINE_CLOCK_PS_ALLOCATION, ulTargetEngineClock),
		    (engine_clock & SET_CLOCK_FREQ_MASK) | ((COMPUTE_ENGINE_PLL_PARAM << 24)));
	frb_ps_le32(&ps, offsetof(SET_ENGINE_CLOCK_PS_ALLOCATION, sReserved),
		    memory_clock & SET_CLOCK_FREQ_MASK);
	return exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, DynamicMemorySettings), &ps);
}

/* get_voltage_info_table: the VoltageObjectInfo table (its v3 view) */
static const uint8_t *get_voltage_info_table(struct frb_device *device)
{
	int index = GetIndexIntoMasterTable(DATA, VoltageObjectInfo);
	u8 frev, crev;
	u16 size;

	return smu_atom_get_data_table(device, index, &size, &frev, &crev);
}

/* the ATOM_VOLTAGE_OBJECT_V3 of this type and mode, or NULL */
static const uint8_t *atomctrl_lookup_voltage_type_v3(
		const uint8_t *voltage_object_info_table,
		uint8_t voltage_type, uint8_t voltage_mode)
{
	unsigned int size = FRB_VB16(voltage_object_info_table, ATOM_VOLTAGE_OBJECT_INFO_V3_1, sHeader.usStructureSize);
	unsigned int offset = offsetof(ATOM_VOLTAGE_OBJECT_INFO_V3_1, asVoltageObj[0]);
	const uint8_t *start = voltage_object_info_table;

	while (offset < size) {
		const uint8_t *voltage_object = start + offset;
		uint16_t obj_size = FRB_VB16(voltage_object, ATOM_VOLTAGE_OBJECT_HEADER_V3, usSize);

		if (voltage_type == FRB_VB8(voltage_object, ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageType) &&
		    voltage_mode == FRB_VB8(voltage_object, ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageMode))
			return voltage_object;

		if (!obj_size)		/* not in Linux: a corrupt table would loop forever */
			break;
		offset += obj_size;
	}

	return NULL;
}

/*
 * ComputeMemoryEnginePLL 1.7 (Polaris): the SCLK PLL settings. The input is
 * COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V4: the clock in the low 24 bits, the
 * flag in the top byte, little-endian.
 */
int atomctrl_get_engine_pll_dividers_ai(struct pp_hwmgr *hwmgr,
		uint32_t clock_value,
		pp_atomctrl_clock_dividers_ai *dividers)
{
	struct frb_ps ps;
	int result;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_7, ulClock),
		    (clock_value & 0x00ffffff) | ((uint32_t)COMPUTE_GPUCLK_INPUT_FLAG_SCLK << 24));

	result = exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL), &ps);

	if (0 == result) {
#define O(f)	offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_7, f)
		dividers->usSclk_fcw_frac     = frb_ps_get_le16(&ps, O(usSclk_fcw_frac));
		dividers->usSclk_fcw_int      = frb_ps_get_le16(&ps, O(usSclk_fcw_int));
		dividers->ucSclkPostDiv       = frb_ps_get_u8(&ps, O(ucSclkPostDiv));
		dividers->ucSclkVcoMode       = frb_ps_get_u8(&ps, O(ucSclkVcoMode));
		dividers->ucSclkPllRange      = frb_ps_get_u8(&ps, O(ucSclkPllRange));
		dividers->ucSscEnable         = frb_ps_get_u8(&ps, O(ucSscEnable));
		dividers->usSsc_fcw1_frac     = frb_ps_get_le16(&ps, O(usSsc_fcw1_frac));
		dividers->usSsc_fcw1_int      = frb_ps_get_le16(&ps, O(usSsc_fcw1_int));
		dividers->usPcc_fcw_int       = frb_ps_get_le16(&ps, O(usPcc_fcw_int));
		dividers->usSsc_fcw_slew_frac = frb_ps_get_le16(&ps, O(usSsc_fcw_slew_frac));
		dividers->usPcc_fcw_slew_frac = frb_ps_get_le16(&ps, O(usPcc_fcw_slew_frac));
#undef O
	}
	return result;
}

/* ComputeMemoryEnginePLL, DFS divider of a clock (COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6) */
int atomctrl_get_dfs_pll_dividers_vi(
		struct pp_hwmgr *hwmgr,
		uint32_t clock_value,
		pp_atomctrl_clock_dividers_vi *dividers)
{
	struct frb_ps ps;
	int result;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ulClock),
		    (clock_value & 0x00ffffff) | ((uint32_t)COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK << 24));

	result = exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL), &ps);

	if (0 == result) {
#define O(f)	offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, f)
		uint32_t clk = frb_ps_get_le32(&ps, O(ulClock));

		/* ulClock.ucPostDiv is the top byte, ulClock.ulClock the low 24 bits */
		dividers->pll_post_divider = clk >> 24;
		dividers->real_clock = clk & 0x00ffffff;
		/* pp_atomctrl_tcipll_fb_divider's 14- and 12-bit fields */
		dividers->ul_fb_div_frac = frb_ps_get_le16(&ps, O(ulFbDiv.usFbDivFrac)) & 0x3fff;
		dividers->ul_fb_div = frb_ps_get_le16(&ps, O(ulFbDiv.usFbDiv)) & 0xfff;
		dividers->uc_pll_ref_div = frb_ps_get_u8(&ps, O(ucPllRefDiv));
		dividers->uc_pll_post_div = frb_ps_get_u8(&ps, O(ucPllPostDiv));
		dividers->uc_pll_cntl_flag = frb_ps_get_u8(&ps, O(ucPllCntlFlag));
#undef O
	}

	return result;
}

/*
 * Returns true if the given voltage type is controlled by GPIO pins.
 * voltage_type is one of SET_VOLTAGE_TYPE_ASIC_VDDC,
 * SET_VOLTAGE_TYPE_ASIC_MVDDC, SET_VOLTAGE_TYPE_ASIC_MVDDQ.
 * voltage_mode is one of ATOM_SET_VOLTAGE, ATOM_SET_VOLTAGE_PHASE
 */
bool atomctrl_is_voltage_controlled_by_gpio_v3(
		struct pp_hwmgr *hwmgr,
		uint8_t voltage_type,
		uint8_t voltage_mode)
{
	const uint8_t *voltage_info = get_voltage_info_table(hwmgr->adev);
	bool ret;

	PP_ASSERT_WITH_CODE((NULL != voltage_info),
			"Could not find Voltage Table in BIOS.", return false;);

	ret = atomctrl_lookup_voltage_type_v3
			(voltage_info, voltage_type, voltage_mode) != NULL;

	return ret;
}

int atomctrl_get_voltage_table_v3(
		struct pp_hwmgr *hwmgr,
		uint8_t voltage_type,
		uint8_t voltage_mode,
		pp_atomctrl_voltage_table *voltage_table)
{
	const uint8_t *voltage_info = get_voltage_info_table(hwmgr->adev);
	const uint8_t *voltage_object;
	unsigned int i, n;

	PP_ASSERT_WITH_CODE((NULL != voltage_info),
			"Could not find Voltage Table in BIOS.", return -1;);

	voltage_object = atomctrl_lookup_voltage_type_v3
		(voltage_info, voltage_type, voltage_mode);

	if (voltage_object == NULL)
		return -1;

	n = FRB_VB8(voltage_object, ATOM_GPIO_VOLTAGE_OBJECT_V3, ucGpioEntryNum);
	PP_ASSERT_WITH_CODE(
			(n <= PP_ATOMCTRL_MAX_VOLTAGE_ENTRIES),
			"Too many voltage entries!",
			return -1;
			);

	for (i = 0; i < n; i++) {
		const uint8_t *e = FRB_VB_ENTRY(voltage_object, ATOM_GPIO_VOLTAGE_OBJECT_V3, asVolGpioLut,
						VOLTAGE_LUT_ENTRY_V2, i);

		voltage_table->entries[i].value = FRB_VB16(e, VOLTAGE_LUT_ENTRY_V2, usVoltageValue);
		voltage_table->entries[i].smio_low = FRB_VB32(e, VOLTAGE_LUT_ENTRY_V2, ulVoltageId);
	}

	voltage_table->mask_low    = FRB_VB32(voltage_object, ATOM_GPIO_VOLTAGE_OBJECT_V3, ulGpioMaskVal);
	voltage_table->count      = n;
	voltage_table->phase_delay = FRB_VB8(voltage_object, ATOM_GPIO_VOLTAGE_OBJECT_V3, ucPhaseDelay);

	return 0;
}

/* GPIO_Pin_LUT: entries are 4 bytes (offsetof(ucGPIO_ID) + 1), as Linux steps */
static bool atomctrl_lookup_gpio_pin(
		const uint8_t *gpio_lookup_table,
		const uint32_t pinId,
		pp_atomctrl_gpio_pin_assignment *gpio_pin_assignment)
{
	unsigned int size = FRB_VB16(gpio_lookup_table, ATOM_GPIO_PIN_LUT, sHeader.usStructureSize);
	unsigned int offset = offsetof(ATOM_GPIO_PIN_LUT, asGPIO_Pin[0]);
	const uint8_t *start = gpio_lookup_table;

	while (offset < size) {
		const uint8_t *pin_assignment = start + offset;

		if (pinId == FRB_VB8(pin_assignment, ATOM_GPIO_PIN_ASSIGNMENT, ucGPIO_ID)) {
			gpio_pin_assignment->uc_gpio_pin_bit_shift =
				FRB_VB8(pin_assignment, ATOM_GPIO_PIN_ASSIGNMENT, ucGpioPinBitShift);
			gpio_pin_assignment->us_gpio_pin_aindex =
				FRB_VB16(pin_assignment, ATOM_GPIO_PIN_ASSIGNMENT, usGpioPin_AIndex);
			return true;
		}

		offset += offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGPIO_ID) + 1;
	}

	return false;
}

static const uint8_t *get_gpio_lookup_table(struct frb_device *device)
{
	u8 frev, crev;
	u16 size;
	const uint8_t *table_address;

	table_address = smu_atom_get_data_table(device,
				GetIndexIntoMasterTable(DATA, GPIO_Pin_LUT),
				&size, &frev, &crev);

	PP_ASSERT_WITH_CODE((NULL != table_address),
			"Error retrieving BIOS Table Address!", return NULL;);

	return table_address;
}

/*
 * Returns 1 if the given pin id find in lookup table.
 */
bool atomctrl_get_pp_assign_pin(
		struct pp_hwmgr *hwmgr,
		const uint32_t pinId,
		pp_atomctrl_gpio_pin_assignment *gpio_pin_assignment)
{
	bool bRet = false;
	const uint8_t *gpio_lookup_table = get_gpio_lookup_table(hwmgr->adev);

	PP_ASSERT_WITH_CODE((NULL != gpio_lookup_table),
			"Could not find GPIO lookup Table in BIOS.", return false);

	bRet = atomctrl_lookup_gpio_pin(gpio_lookup_table, pinId,
		gpio_pin_assignment);

	return bRet;
}

bool atomctrl_is_asic_internal_ss_supported(struct pp_hwmgr *hwmgr)
{
	u8 frev, crev;
	u16 size;

	return smu_atom_get_data_table(hwmgr->adev,
			GetIndexIntoMasterTable(DATA, ASIC_InternalSS_Info),
			&size, &frev, &crev) != NULL;
}

/* ReadEfuseValue: EFUSE_INPUT_PARAMETER in, ulEfuseValue out (both LE) */
int atomctrl_read_efuse(struct pp_hwmgr *hwmgr, uint16_t start_index,
		uint16_t end_index, uint32_t *efuse)
{
	uint32_t mask;
	int result;
	struct frb_ps ps;

	if ((end_index - start_index)  == 31)
		mask = 0xFFFFFFFF;
	else
		mask = (1 << ((end_index - start_index) + 1)) - 1;

	frb_ps_init(&ps);
	frb_ps_le16(&ps, offsetof(EFUSE_INPUT_PARAMETER, usEfuseIndex), (start_index / 32) * 4);
	frb_ps_u8(&ps, offsetof(EFUSE_INPUT_PARAMETER, ucBitShift),
		  (uint8_t)(start_index - ((start_index / 32) * 32)));
	frb_ps_u8(&ps, offsetof(EFUSE_INPUT_PARAMETER, ucBitLength),
		  (uint8_t)((end_index - start_index) + 1));

	result = exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, ReadEfuseValue), &ps);
	*efuse = result ? 0 : frb_ps_get_le32(&ps, offsetof(READ_EFUSE_VALUE_PARAMETER, ulEfuseValue)) & mask;

	return result;
}

/*
 * DynamicMemorySettings 2.1, asDPMMCReg: ATOM_COMPUTE_CLOCK_FREQ (clock in
 * the low 24 bits, ADJUST_MC_SETTING_PARAM in the top byte; an LE host's
 * bitfields), then ucMclkDPMState.
 */
int atomctrl_set_ac_timing_ai(struct pp_hwmgr *hwmgr, uint32_t memory_clock,
			      uint8_t level)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, offsetof(DYNAMICE_MC_DPM_SETTINGS_PARAMETER, ulClock),
		    (memory_clock & SET_CLOCK_FREQ_MASK) | ((uint32_t)ADJUST_MC_SETTING_PARAM << 24));
	frb_ps_u8(&ps, offsetof(DYNAMICE_MC_DPM_SETTINGS_PARAMETER, ucMclkDPMState), level);

	return exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, DynamicMemorySettings), &ps);
}

/* GetVoltageInfo 1.3, ATOM_GET_VOLTAGE_EVV_VOLTAGE: the voltage in 0.01 mV */
int atomctrl_get_voltage_evv_on_sclk_ai(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
				uint32_t sclk, uint16_t virtual_voltage_Id, uint32_t *voltage)
{
	int result;
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_3, ucVoltageType), voltage_type);
	frb_ps_u8(&ps, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_3, ucVoltageMode), ATOM_GET_VOLTAGE_EVV_VOLTAGE);
	frb_ps_le16(&ps, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_3, usVoltageLevel), virtual_voltage_Id);
	frb_ps_le32(&ps, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_3, ulSCLKFreq), sclk);

	result = exec_table(hwmgr, GetIndexIntoMasterTable(COMMAND, GetVoltageInfo), &ps);

	*voltage = result ? 0 :
		frb_ps_get_le32(&ps, offsetof(GET_EVV_VOLTAGE_INFO_OUTPUT_PARAMETER_V1_3, ulVoltageLevel));

	return result;
}

int atomctrl_get_smc_sclk_range_table(struct pp_hwmgr *hwmgr, struct pp_atom_ctrl_sclk_range_table *table)
{
	int i, n;
	u8 frev, crev;
	u16 size;
	const uint8_t *psmu_info = smu_atom_get_data_table(hwmgr->adev,
			GetIndexIntoMasterTable(DATA, SMU_Info),
			&size, &frev, &crev);

	if (!psmu_info)
		return -EINVAL;

	n = FRB_VB8(psmu_info, ATOM_SMU_INFO_V2_1, ucSclkEntryNum);
	if (n > MAX_SCLK_RANGE)		/* not in Linux: the table holds 8 */
		n = MAX_SCLK_RANGE;
	for (i = 0; i < n; i++) {
		const uint8_t *e = FRB_VB_ENTRY(psmu_info, ATOM_SMU_INFO_V2_1, asSclkFcwRangeEntry,
						ATOM_SCLK_FCW_RANGE_ENTRY_V1, i);

		table->entry[i].ucVco_setting = FRB_VB8(e, ATOM_SCLK_FCW_RANGE_ENTRY_V1, ucVco_setting);
		table->entry[i].ucPostdiv = FRB_VB8(e, ATOM_SCLK_FCW_RANGE_ENTRY_V1, ucPostdiv);
		table->entry[i].usFcw_pcc = FRB_VB16(e, ATOM_SCLK_FCW_RANGE_ENTRY_V1, ucFcw_pcc);
		table->entry[i].usFcw_trans_upper = FRB_VB16(e, ATOM_SCLK_FCW_RANGE_ENTRY_V1, ucFcw_trans_upper);
		table->entry[i].usRcw_trans_lower = FRB_VB16(e, ATOM_SCLK_FCW_RANGE_ENTRY_V1, ucRcw_trans_lower);
	}

	return 0;
}

int atomctrl_get_vddc_shared_railinfo(struct pp_hwmgr *hwmgr, uint8_t *shared_rail)
{
	const uint8_t *psmu_info = smu_atom_get_data_table(hwmgr->adev,
			GetIndexIntoMasterTable(DATA, SMU_Info),
			NULL, NULL, NULL);
	if (!psmu_info)
		return -1;

	*shared_rail = FRB_VB8(psmu_info, ATOM_SMU_INFO_V2_1, ucSharePowerSource);

	return 0;
}

/*
 * atomctrl_get_voltage_evv_on_sclk: Tonga/Fiji's GetVoltageInfo 1.2 for
 * VDDGFX. Polaris takes the _ai path (phm_get_voltage_evv_on_sclk) for VDDC;
 * this one is reached only with VDDGFX on SVI2, which no Polaris VBIOS seen
 * here has. Not ported.
 */
int atomctrl_get_voltage_evv_on_sclk(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
				     uint32_t sclk, uint16_t virtual_voltage_Id, uint16_t *voltage)
{
	(void)hwmgr; (void)voltage_type; (void)sclk; (void)virtual_voltage_Id;
	*voltage = 0;
	pr_err("atomctrl_get_voltage_evv_on_sclk (VDDGFX EVV): not ported\n");
	return -EINVAL;
}

int atomctrl_get_avfs_information(struct pp_hwmgr *hwmgr,
				  struct pp_atom_ctrl__avfs_parameters *param)
{
	const uint8_t *profile;

	if (param == NULL)
		return -EINVAL;

	profile = smu_atom_get_data_table(hwmgr->adev,
			GetIndexIntoMasterTable(DATA, ASIC_ProfilingInfo),
			NULL, NULL, NULL);
	if (!profile)
		return -1;

#define P32(f)	FRB_VB32(profile, ATOM_ASIC_PROFILING_INFO_V3_6, f)
#define P16(f)	FRB_VB16(profile, ATOM_ASIC_PROFILING_INFO_V3_6, f)
#define P8(f)	FRB_VB8(profile, ATOM_ASIC_PROFILING_INFO_V3_6, f)
	param->ulAVFS_meanNsigma_Acontant0 = P32(ulAVFS_meanNsigma_Acontant0);
	param->ulAVFS_meanNsigma_Acontant1 = P32(ulAVFS_meanNsigma_Acontant1);
	param->ulAVFS_meanNsigma_Acontant2 = P32(ulAVFS_meanNsigma_Acontant2);
	param->usAVFS_meanNsigma_DC_tol_sigma = P16(usAVFS_meanNsigma_DC_tol_sigma);
	param->usAVFS_meanNsigma_Platform_mean = P16(usAVFS_meanNsigma_Platform_mean);
	param->usAVFS_meanNsigma_Platform_sigma = P16(usAVFS_meanNsigma_Platform_sigma);
	param->ulGB_VDROOP_TABLE_CKSOFF_a0 = P32(ulGB_VDROOP_TABLE_CKSOFF_a0);
	param->ulGB_VDROOP_TABLE_CKSOFF_a1 = P32(ulGB_VDROOP_TABLE_CKSOFF_a1);
	param->ulGB_VDROOP_TABLE_CKSOFF_a2 = P32(ulGB_VDROOP_TABLE_CKSOFF_a2);
	param->ulGB_VDROOP_TABLE_CKSON_a0 = P32(ulGB_VDROOP_TABLE_CKSON_a0);
	param->ulGB_VDROOP_TABLE_CKSON_a1 = P32(ulGB_VDROOP_TABLE_CKSON_a1);
	param->ulGB_VDROOP_TABLE_CKSON_a2 = P32(ulGB_VDROOP_TABLE_CKSON_a2);
	param->ulAVFSGB_FUSE_TABLE_CKSOFF_m1 = P32(ulAVFSGB_FUSE_TABLE_CKSOFF_m1);
	param->usAVFSGB_FUSE_TABLE_CKSOFF_m2 = P16(usAVFSGB_FUSE_TABLE_CKSOFF_m2);
	param->ulAVFSGB_FUSE_TABLE_CKSOFF_b = P32(ulAVFSGB_FUSE_TABLE_CKSOFF_b);
	param->ulAVFSGB_FUSE_TABLE_CKSON_m1 = P32(ulAVFSGB_FUSE_TABLE_CKSON_m1);
	param->usAVFSGB_FUSE_TABLE_CKSON_m2 = P16(usAVFSGB_FUSE_TABLE_CKSON_m2);
	param->ulAVFSGB_FUSE_TABLE_CKSON_b = P32(ulAVFSGB_FUSE_TABLE_CKSON_b);
	param->usMaxVoltage_0_25mv = P16(usMaxVoltage_0_25mv);
	param->ucEnableGB_VDROOP_TABLE_CKSOFF = P8(ucEnableGB_VDROOP_TABLE_CKSOFF);
	param->ucEnableGB_VDROOP_TABLE_CKSON = P8(ucEnableGB_VDROOP_TABLE_CKSON);
	param->ucEnableGB_FUSE_TABLE_CKSOFF = P8(ucEnableGB_FUSE_TABLE_CKSOFF);
	param->ucEnableGB_FUSE_TABLE_CKSON = P8(ucEnableGB_FUSE_TABLE_CKSON);
	param->usPSM_Age_ComFactor = P16(usPSM_Age_ComFactor);
	param->ucEnableApplyAVFS_CKS_OFF_Voltage = P8(ucEnableApplyAVFS_CKS_OFF_Voltage);
#undef P32
#undef P16
#undef P8

	return 0;
}

/* usLoadLine_PSI: Linux reads it raw (LE host); here little-endian */
int  atomctrl_get_svi2_info(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
				uint8_t *svd_gpio_id, uint8_t *svc_gpio_id,
				uint16_t *load_line)
{
	const uint8_t *voltage_info = get_voltage_info_table(hwmgr->adev);
	const uint8_t *voltage_object;

	PP_ASSERT_WITH_CODE((NULL != voltage_info),
			"Could not find Voltage Table in BIOS.", return -EINVAL);

	voltage_object = atomctrl_lookup_voltage_type_v3
		(voltage_info, voltage_type,  VOLTAGE_OBJ_SVID2);
	/* Linux dereferences it unchecked */
	PP_ASSERT_WITH_CODE((NULL != voltage_object),
			"No SVID2 voltage object.", return -EINVAL);

	*svd_gpio_id = FRB_VB8(voltage_object, ATOM_SVID2_VOLTAGE_OBJECT_V3, ucSVDGpioId);
	*svc_gpio_id = FRB_VB8(voltage_object, ATOM_SVID2_VOLTAGE_OBJECT_V3, ucSVCGpioId);
	*load_line = FRB_VB16(voltage_object, ATOM_SVID2_VOLTAGE_OBJECT_V3, usLoadLine_PSI);

	return 0;
}

/*
 * GFX_Info 2.3's EDC/DIDT leakage offsets. Linux reads them raw (LE host);
 * here little-endian.
 */
int atomctrl_get_edc_hilo_leakage_offset_table(struct pp_hwmgr *hwmgr,
					       AtomCtrl_HiLoLeakageOffsetTable *table)
{
	const uint8_t *gfxinfo = smu_atom_get_data_table(hwmgr->adev,
					GetIndexIntoMasterTable(DATA, GFX_Info),
					NULL, NULL, NULL);
	if (!gfxinfo)
		return -ENOENT;

	table->usHiLoLeakageThreshold = FRB_VB16(gfxinfo, ATOM_GFX_INFO_V2_3, usHiLoLeakageThreshold);
	table->usEdcDidtLoDpm7TableOffset = FRB_VB16(gfxinfo, ATOM_GFX_INFO_V2_3, usEdcDidtLoDpm7TableOffset);
	table->usEdcDidtHiDpm7TableOffset = FRB_VB16(gfxinfo, ATOM_GFX_INFO_V2_3, usEdcDidtHiDpm7TableOffset);

	return 0;
}

/*
 * get_edc_leakage_table + atomctrl_get_edc_leakage_table. As in Linux, the
 * words are taken from the start of GFX_Info: Linux advances its copy of
 * the address by `offset` and returns the other copy, so the offset has no
 * effect. Kept as it is (this VBIOS has no offsets, so the path is not
 * taken on the E9173). The words are read little-endian (Linux: raw).
 */
int atomctrl_get_edc_leakage_table(struct pp_hwmgr *hwmgr,
				   AtomCtrl_EDCLeakgeTable *table,
				   uint16_t offset)
{
	uint32_t length, i;
	const uint8_t *leakage_table = smu_atom_get_data_table(hwmgr->adev,
			GetIndexIntoMasterTable(DATA, GFX_Info),
			NULL, NULL, NULL);

	(void)offset;
	if (!leakage_table)
		return -ENOENT;

	length = sizeof(table->DIDT_REG) / sizeof(table->DIDT_REG[0]);
	for (i = 0; i < length; i++)
		table->DIDT_REG[i] = frb_get_le32(leakage_table, i * 4);

	return 0;
}
