/*
 * pm/powerplay/hwmgr/process_pptables_v1_0.c (Linux 7.1.1): the VBIOS
 * PowerPlayInfo table v7 (ATOM_Tonga_POWERPLAYTABLE, Polaris sub-tables)
 * into the hardware manager's phm_ppt_v1 tables.
 *
 * Deviations, all byte order (the table is little-endian in the ROM):
 * - every field is read with FRB_VB8/16/32 at its offsetof() position in the
 *   packed include/pp/pptable_v1_0.h structures, never by dereferencing one;
 * - fields Linux reads with no conversion (its LE host does it) are read LE
 *   too. Two of them would go wrong on a big-endian host if ported as
 *   written: get_pcie_table's le16_to_cpu() of the one-byte usPCIELaneWidth
 *   (a swap of a byte, truncated: 0 lanes), and the Polaris PowerTune
 *   table's raw usBoostPowerLimit. The Polaris fan table's three
 *   le16_to_cpu() of one-byte fields are plain byte reads here.
 * Not ported: the Tonga/Fiji sub-table revisions (a Polaris VBIOS has the
 * Polaris ones; an older revision is refused), the VCE state table (no VCE
 * on Firebrand).
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
#include "frb_hwmgr.h"
#include "frb_ppatomctrl.h"
#include "../shared/atom/atombios.h"
#include "include/pp/pptable_v1_0.h"

/* the layouts the reads below depend on (checked here so a compiler that
 * ignores #pragma pack fails to build instead of misreading the VBIOS) */
FRB_STATIC_ASSERT(pp_header, offsetof(ATOM_Tonga_POWERPLAYTABLE, usStateArrayOffset) == 0x23);
FRB_STATIC_ASSERT(pp_state, sizeof(ATOM_Tonga_State) == 20);
FRB_STATIC_ASSERT(pp_mclk, sizeof(ATOM_Tonga_MCLK_Dependency_Record) == 13);
FRB_STATIC_ASSERT(pp_sclk_polaris, sizeof(ATOM_Polaris_SCLK_Dependency_Record) == 15);
FRB_STATIC_ASSERT(pp_pcie_polaris, sizeof(ATOM_Polaris10_PCIE_Record) == 8);
FRB_STATIC_ASSERT(pp_mm, sizeof(ATOM_Tonga_MM_Dependency_Record) == 23);
FRB_STATIC_ASSERT(pp_lookup, sizeof(ATOM_Tonga_Voltage_Lookup_Record) == 8);
FRB_STATIC_ASSERT(pp_hardlimit, sizeof(ATOM_Tonga_Hard_Limit_Record) == 14);

#define PT	ATOM_Tonga_POWERPLAYTABLE
/* a sub-table of the PowerPlay table at its offset field */
#define SUBTABLE(pp, field)	((pp) + FRB_VB16((pp), PT, field))

/**
 * set_hw_cap - Private Function used during initialization.
 * @hwmgr: Pointer to the hardware manager.
 * @setIt: A flag indication if the capability should be set (TRUE) or reset (FALSE).
 * @cap: Which capability to set/reset.
 */
static void set_hw_cap(struct pp_hwmgr *hwmgr, bool setIt, enum phm_platform_caps cap)
{
	if (setIt)
		phm_cap_set(hwmgr->platform_descriptor.platformCaps, cap);
	else
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps, cap);
}

/**
 * set_platform_caps - Private Function used during initialization.
 * @hwmgr: Pointer to the hardware manager.
 * @powerplay_caps: the bit array (from BIOS) of capability bits.
 */
static int set_platform_caps(struct pp_hwmgr *hwmgr, uint32_t powerplay_caps)
{
	PP_ASSERT_WITH_CODE((~powerplay_caps & ____RETIRE16____),
		"ATOM_PP_PLATFORM_CAP_ASPM_L1 is not supported!", ;);
	PP_ASSERT_WITH_CODE((~powerplay_caps & ____RETIRE64____),
		"ATOM_PP_PLATFORM_CAP_GEMINIPRIMARY is not supported!", ;);
	PP_ASSERT_WITH_CODE((~powerplay_caps & ____RETIRE512____),
		"ATOM_PP_PLATFORM_CAP_SIDEPORTCONTROL is not supported!", ;);
	PP_ASSERT_WITH_CODE((~powerplay_caps & ____RETIRE1024____),
		"ATOM_PP_PLATFORM_CAP_TURNOFFPLL_ASPML1 is not supported!", ;);
	PP_ASSERT_WITH_CODE((~powerplay_caps & ____RETIRE2048____),
		"ATOM_PP_PLATFORM_CAP_HTLINKCONTROL is not supported!", ;);

	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_POWERPLAY),
		   PHM_PlatformCaps_PowerPlaySupport);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_SBIOSPOWERSOURCE),
		   PHM_PlatformCaps_BiosPowerSourceControl);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_HARDWAREDC),
		   PHM_PlatformCaps_AutomaticDCTransition);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_MVDD_CONTROL),
		   PHM_PlatformCaps_EnableMVDDControl);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_VDDCI_CONTROL),
		   PHM_PlatformCaps_ControlVDDCI);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_VDDGFX_CONTROL),
		   PHM_PlatformCaps_ControlVDDGFX);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_BACO),
		   PHM_PlatformCaps_BACO);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_CAP_DISABLE_VOLTAGE_ISLAND),
		   PHM_PlatformCaps_DisableVoltageIsland);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PP_PLATFORM_COMBINE_PCC_WITH_THERMAL_SIGNAL),
		   PHM_PlatformCaps_CombinePCCWithThermalSignal);
	set_hw_cap(hwmgr, 0 != (powerplay_caps & ATOM_TONGA_PLATFORM_LOAD_POST_PRODUCTION_FIRMWARE),
		   PHM_PlatformCaps_LoadPostProductionFirmware);

	return 0;
}

/* get_powerplay_table: the PowerPlayInfo data table in the VBIOS image, cached */
static const uint8_t *get_powerplay_table(struct pp_hwmgr *hwmgr)
{
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	u16 size;
	u8 frev, crev;
	const uint8_t *table_address = hwmgr->soft_pp_table;

	if (!table_address) {
		table_address = smu_atom_get_data_table(hwmgr->adev, index, &size, &frev, &crev);
		hwmgr->soft_pp_table = table_address;	/*Cache the result in RAM.*/
		hwmgr->soft_pp_table_size = size;
	}

	return table_address;
}

static int get_vddc_lookup_table(
		struct pp_hwmgr	*hwmgr,
		phm_ppt_v1_voltage_lookup_table	**lookup_table,
		const uint8_t *vddc_lookup_pp_tables,
		uint32_t max_levels
		)
{
	uint32_t i, n = FRB_VB8(vddc_lookup_pp_tables, ATOM_Tonga_Voltage_Lookup_Table, ucNumEntries);
	phm_ppt_v1_voltage_lookup_table *table;
	phm_ppt_v1_voltage_lookup_record *record;

	PP_ASSERT_WITH_CODE((0 != n), "Invalid CAC Leakage PowerPlay Table!", return 1);
	/* not in Linux: the table is allocated for max_levels entries */
	PP_ASSERT_WITH_CODE((n <= max_levels), "Too many voltage lookup entries!", return -EINVAL);

	table = frb_pp_zalloc(hwmgr, sizeof(*table) + max_levels * sizeof(table->entries[0]));
	if (!table)
		return -ENOMEM;

	table->count = n;

	for (i = 0; i < n; i++) {
		const uint8_t *atom_record = FRB_VB_ENTRY(vddc_lookup_pp_tables, ATOM_Tonga_Voltage_Lookup_Table,
							  entries, ATOM_Tonga_Voltage_Lookup_Record, i);

		record = &table->entries[i];
		record->us_calculated = 0;
		record->us_vdd = FRB_VB16(atom_record, ATOM_Tonga_Voltage_Lookup_Record, usVdd);
		record->us_cac_low = FRB_VB16(atom_record, ATOM_Tonga_Voltage_Lookup_Record, usCACLow);
		record->us_cac_mid = FRB_VB16(atom_record, ATOM_Tonga_Voltage_Lookup_Record, usCACMid);
		record->us_cac_high = FRB_VB16(atom_record, ATOM_Tonga_Voltage_Lookup_Record, usCACHigh);
	}

	*lookup_table = table;

	return 0;
}

/* get_platform_power_management_table: the PPM table */
static int get_platform_power_management_table(
		struct pp_hwmgr *hwmgr,
		const uint8_t *atom_ppm_table)
{
	struct phm_ppm_table *ptr = frb_pp_zalloc(hwmgr, sizeof(*ptr));
	struct phm_ppt_v1_information *pp_table_information =
		(struct phm_ppt_v1_information *)(hwmgr->pptable);

	if (NULL == ptr)
		return -ENOMEM;

#define PPM8(f)		FRB_VB8(atom_ppm_table, ATOM_Tonga_PPM_Table, f)
#define PPM16(f)	FRB_VB16(atom_ppm_table, ATOM_Tonga_PPM_Table, f)
#define PPM32(f)	FRB_VB32(atom_ppm_table, ATOM_Tonga_PPM_Table, f)
	ptr->ppm_design = PPM8(ucPpmDesign);
	ptr->cpu_core_number = PPM16(usCpuCoreNumber);
	ptr->platform_tdp = PPM32(ulPlatformTDP);
	ptr->small_ac_platform_tdp = PPM32(ulSmallACPlatformTDP);
	ptr->platform_tdc = PPM32(ulPlatformTDC);
	ptr->small_ac_platform_tdc = PPM32(ulSmallACPlatformTDC);
	ptr->apu_tdp = PPM32(ulApuTDP);
	ptr->dgpu_tdp = PPM32(ulDGpuTDP);
	ptr->dgpu_ulv_power = PPM32(ulDGpuUlvPower);
	ptr->tj_max = PPM32(ulTjmax);
#undef PPM8
#undef PPM16
#undef PPM32

	pp_table_information->ppm_parameter_table = ptr;

	return 0;
}

/**
 * init_dpm_2_parameters - Private Function used during initialization.
 * Initialize TDP limits for DPM2
 * @hwmgr: Pointer to the hardware manager.
 * @powerplay_table: Pointer to the PowerPlay Table.
 */
static int init_dpm_2_parameters(
		struct pp_hwmgr *hwmgr,
		const uint8_t *powerplay_table
		)
{
	int result = 0;
	struct phm_ppt_v1_information *pp_table_information = (struct phm_ppt_v1_information *)(hwmgr->pptable);
	uint32_t disable_ppm = 0;
	uint32_t disable_power_control = 0;

	pp_table_information->us_ulv_voltage_offset = FRB_VB16(powerplay_table, PT, usUlvVoltageOffset);

	pp_table_information->ppm_parameter_table = NULL;
	pp_table_information->vddc_lookup_table = NULL;
	pp_table_information->vddgfx_lookup_table = NULL;
	/* TDP limits */
	hwmgr->platform_descriptor.TDPODLimit = FRB_VB16(powerplay_table, PT, usPowerControlLimit);
	hwmgr->platform_descriptor.TDPAdjustment = 0;
	hwmgr->platform_descriptor.VidAdjustment = 0;
	hwmgr->platform_descriptor.VidAdjustmentPolarity = 0;
	hwmgr->platform_descriptor.VidMinLimit = 0;
	hwmgr->platform_descriptor.VidMaxLimit = 1500000;
	hwmgr->platform_descriptor.VidStep = 6250;

	disable_power_control = 0;
	if (0 == disable_power_control) {
		/* enable TDP overdrive (PowerControl) feature as well if supported */
		if (hwmgr->platform_descriptor.TDPODLimit != 0)
			phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_PowerControl);
	}

	if (0 != FRB_VB16(powerplay_table, PT, usVddcLookupTableOffset)) {
		result = get_vddc_lookup_table(hwmgr, &pp_table_information->vddc_lookup_table,
					       SUBTABLE(powerplay_table, usVddcLookupTableOffset), 16);
	}

	if (0 != FRB_VB16(powerplay_table, PT, usVddgfxLookupTableOffset)) {
		result = get_vddc_lookup_table(hwmgr, &pp_table_information->vddgfx_lookup_table,
					       SUBTABLE(powerplay_table, usVddgfxLookupTableOffset), 16);
	}

	disable_ppm = 0;
	if (0 == disable_ppm) {
		if (0 != FRB_VB16(powerplay_table, PT, usPPMTableOffset)) {
			if (get_platform_power_management_table(hwmgr,
					SUBTABLE(powerplay_table, usPPMTableOffset)) == 0) {
				phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_EnablePlatformPowerManagement);
			}
		}
	}

	return result;
}

static int get_valid_clk(
		struct pp_hwmgr *hwmgr,
		struct phm_clock_array **clk_table,
		phm_ppt_v1_clock_voltage_dependency_table const *clk_volt_pp_table
		)
{
	uint32_t i;
	struct phm_clock_array *table;

	PP_ASSERT_WITH_CODE((0 != clk_volt_pp_table->count),
		"Invalid PowerPlay Table!", return -1);

	table = frb_pp_zalloc(hwmgr, sizeof(*table) + clk_volt_pp_table->count * sizeof(table->values[0]));
	if (!table)
		return -ENOMEM;

	table->count = (uint32_t)clk_volt_pp_table->count;

	for (i = 0; i < table->count; i++)
		table->values[i] = (uint32_t)clk_volt_pp_table->entries[i].clk;
	*clk_table = table;

	return 0;
}

static int get_hard_limits(
		struct pp_hwmgr *hwmgr,
		struct phm_clock_and_voltage_limits *limits,
		const uint8_t *limitable
		)
{
	const uint8_t *e = FRB_VB_ENTRY(limitable, ATOM_Tonga_Hard_Limit_Table, entries,
					ATOM_Tonga_Hard_Limit_Record, 0);

	PP_ASSERT_WITH_CODE((0 != FRB_VB8(limitable, ATOM_Tonga_Hard_Limit_Table, ucNumEntries)),
			    "Invalid PowerPlay Table!", return -1);

	/* currently we always take entries[0] parameters */
	limits->sclk = FRB_VB32(e, ATOM_Tonga_Hard_Limit_Record, ulSCLKLimit);
	limits->mclk = FRB_VB32(e, ATOM_Tonga_Hard_Limit_Record, ulMCLKLimit);
	limits->vddc = FRB_VB16(e, ATOM_Tonga_Hard_Limit_Record, usVddcLimit);
	limits->vddci = FRB_VB16(e, ATOM_Tonga_Hard_Limit_Record, usVddciLimit);
	limits->vddgfx = FRB_VB16(e, ATOM_Tonga_Hard_Limit_Record, usVddgfxLimit);

	return 0;
}

static int get_mclk_voltage_dependency_table(
		struct pp_hwmgr *hwmgr,
		phm_ppt_v1_clock_voltage_dependency_table **pp_tonga_mclk_dep_table,
		const uint8_t *mclk_dep_table
		)
{
	uint32_t i, n = FRB_VB8(mclk_dep_table, ATOM_Tonga_MCLK_Dependency_Table, ucNumEntries);
	phm_ppt_v1_clock_voltage_dependency_table *mclk_table;
	phm_ppt_v1_clock_voltage_dependency_record *mclk_table_record;

	PP_ASSERT_WITH_CODE((0 != n), "Invalid PowerPlay Table!", return -1);

	mclk_table = frb_pp_zalloc(hwmgr, sizeof(*mclk_table) + n * sizeof(mclk_table->entries[0]));
	if (!mclk_table)
		return -ENOMEM;

	mclk_table->count = n;

	for (i = 0; i < n; i++) {
		const uint8_t *r = FRB_VB_ENTRY(mclk_dep_table, ATOM_Tonga_MCLK_Dependency_Table, entries,
						ATOM_Tonga_MCLK_Dependency_Record, i);

		mclk_table_record = &mclk_table->entries[i];
		mclk_table_record->vddInd = FRB_VB8(r, ATOM_Tonga_MCLK_Dependency_Record, ucVddcInd);
		mclk_table_record->vdd_offset = FRB_VB16(r, ATOM_Tonga_MCLK_Dependency_Record, usVddgfxOffset);
		mclk_table_record->vddci = FRB_VB16(r, ATOM_Tonga_MCLK_Dependency_Record, usVddci);
		mclk_table_record->mvdd = FRB_VB16(r, ATOM_Tonga_MCLK_Dependency_Record, usMvdd);
		mclk_table_record->clk = FRB_VB32(r, ATOM_Tonga_MCLK_Dependency_Record, ulMclk);
	}

	*pp_tonga_mclk_dep_table = mclk_table;

	return 0;
}

/* the Polaris SCLK table (ucRevId >= 1); a Tonga one (revision 0) is refused */
static int get_sclk_voltage_dependency_table(
		struct pp_hwmgr *hwmgr,
		phm_ppt_v1_clock_voltage_dependency_table **pp_tonga_sclk_dep_table,
		const uint8_t *sclk_dep_table
		)
{
	uint32_t i, n;
	phm_ppt_v1_clock_voltage_dependency_table *sclk_table;
	phm_ppt_v1_clock_voltage_dependency_record *sclk_table_record;

	PP_ASSERT_WITH_CODE((FRB_VB8(sclk_dep_table, PPTable_Generic_SubTable_Header, ucRevId) >= 1),
			    "Tonga SCLK dependency table: not ported", return -EINVAL);

	n = FRB_VB8(sclk_dep_table, ATOM_Polaris_SCLK_Dependency_Table, ucNumEntries);
	PP_ASSERT_WITH_CODE((0 != n), "Invalid PowerPlay Table!", return -1);

	sclk_table = frb_pp_zalloc(hwmgr, sizeof(*sclk_table) + n * sizeof(sclk_table->entries[0]));
	if (!sclk_table)
		return -ENOMEM;

	sclk_table->count = n;

	for (i = 0; i < n; i++) {
		const uint8_t *r = FRB_VB_ENTRY(sclk_dep_table, ATOM_Polaris_SCLK_Dependency_Table, entries,
						ATOM_Polaris_SCLK_Dependency_Record, i);
		uint8_t cks = FRB_VB8(r, ATOM_Polaris_SCLK_Dependency_Record, ucCKSVOffsetandDisable);

		sclk_table_record = &sclk_table->entries[i];
		sclk_table_record->vddInd = FRB_VB8(r, ATOM_Polaris_SCLK_Dependency_Record, ucVddInd);
		sclk_table_record->vdd_offset = FRB_VB16(r, ATOM_Polaris_SCLK_Dependency_Record, usVddcOffset);
		sclk_table_record->clk = FRB_VB32(r, ATOM_Polaris_SCLK_Dependency_Record, ulSclk);
		sclk_table_record->cks_enable = (((cks & 0x80) >> 7) == 0) ? 1 : 0;
		sclk_table_record->cks_voffset = (cks & 0x7F);
		sclk_table_record->sclk_offset = FRB_VB32(r, ATOM_Polaris_SCLK_Dependency_Record, ulSclkOffset);
	}
	*pp_tonga_sclk_dep_table = sclk_table;

	return 0;
}

/* the Polaris10 PCIe table (ucRevId >= 1) */
static int get_pcie_table(
		struct pp_hwmgr *hwmgr,
		phm_ppt_v1_pcie_table **pp_tonga_pcie_table,
		const uint8_t *ptable
		)
{
	uint32_t i, pcie_count, n;
	phm_ppt_v1_pcie_table *pcie_table;
	struct phm_ppt_v1_information *pp_table_information =
		(struct phm_ppt_v1_information *)(hwmgr->pptable);
	phm_ppt_v1_pcie_record *pcie_record;

	PP_ASSERT_WITH_CODE((FRB_VB8(ptable, PPTable_Generic_SubTable_Header, ucRevId) >= 1),
			    "Tonga PCIE table: not ported", return -EINVAL);

	/* Polaris10/Polaris11 and newer. */
	n = FRB_VB8(ptable, ATOM_Polaris10_PCIE_Table, ucNumEntries);
	PP_ASSERT_WITH_CODE((n != 0), "Invalid PowerPlay Table!", return -1);

	pcie_table = frb_pp_zalloc(hwmgr, sizeof(*pcie_table) + n * sizeof(pcie_table->entries[0]));
	if (!pcie_table)
		return -ENOMEM;

	/*
	* Make sure the number of pcie entries are less than or equal to sclk dpm levels.
	* Since first PCIE entry is for ULV, #pcie has to be <= SclkLevel + 1.
	*/
	pcie_count = (pp_table_information->vdd_dep_on_sclk->count) + 1;
	if (n <= pcie_count)
		pcie_count = n;
	else
		pr_err("Number of Pcie Entries exceed the number of SCLK Dpm Levels! Disregarding the excess entries...\n");

	pcie_table->count = pcie_count;

	for (i = 0; i < pcie_count; i++) {
		const uint8_t *r = FRB_VB_ENTRY(ptable, ATOM_Polaris10_PCIE_Table, entries,
						ATOM_Polaris10_PCIE_Record, i);

		pcie_record = &pcie_table->entries[i];
		pcie_record->gen_speed = FRB_VB8(r, ATOM_Polaris10_PCIE_Record, ucPCIEGenSpeed);
		/* a one-byte field: Linux's le16_to_cpu() of it is a no-op only on its LE host */
		pcie_record->lane_width = FRB_VB8(r, ATOM_Polaris10_PCIE_Record, usPCIELaneWidth);
		pcie_record->pcie_sclk = FRB_VB32(r, ATOM_Polaris10_PCIE_Record, ulPCIE_Sclk);
	}

	*pp_tonga_pcie_table = pcie_table;

	return 0;
}

/* the Polaris PowerTune table (ucRevId >= 4) */
static int get_cac_tdp_table(
		struct pp_hwmgr *hwmgr,
		struct phm_cac_tdp_table **cac_tdp_table,
		const uint8_t *table
		)
{
	uint32_t table_size;
	struct phm_cac_tdp_table *tdp_table;

	PP_ASSERT_WITH_CODE((FRB_VB8(table, PPTable_Generic_SubTable_Header, ucRevId) >= 4),
			    "Tonga/Fiji PowerTune table: not ported", return -EINVAL);

	table_size = sizeof(uint32_t) + sizeof(struct phm_cac_tdp_table);
	tdp_table = frb_pp_zalloc(hwmgr, table_size);

	if (NULL == tdp_table)
		return -ENOMEM;

	hwmgr->dyn_state.cac_dtp_table = frb_pp_zalloc(hwmgr, table_size);

	if (NULL == hwmgr->dyn_state.cac_dtp_table) {
		frb_pp_free(hwmgr, tdp_table);
		return -ENOMEM;
	}

#define PW8(f)	FRB_VB8(table, ATOM_Polaris_PowerTune_Table, f)
#define PW16(f)	FRB_VB16(table, ATOM_Polaris_PowerTune_Table, f)
	tdp_table->usTDP = PW16(usTDP);
	tdp_table->usConfigurableTDP = PW16(usConfigurableTDP);
	tdp_table->usTDC = PW16(usTDC);
	tdp_table->usBatteryPowerLimit = PW16(usBatteryPowerLimit);
	tdp_table->usSmallPowerLimit = PW16(usSmallPowerLimit);
	tdp_table->usLowCACLeakage = PW16(usLowCACLeakage);
	tdp_table->usHighCACLeakage = PW16(usHighCACLeakage);
	tdp_table->usMaximumPowerDeliveryLimit = PW16(usMaximumPowerDeliveryLimit);
	tdp_table->usDefaultTargetOperatingTemp = PW16(usTjMax);
	tdp_table->usTargetOperatingTemp = PW16(usTjMax); /*Set the initial temp to the same as default */
	tdp_table->usPowerTuneDataSetID = PW16(usPowerTuneDataSetID);
	tdp_table->usSoftwareShutdownTemp = PW16(usSoftwareShutdownTemp);
	tdp_table->usClockStretchAmount = PW16(usClockStretchAmount);
	tdp_table->usTemperatureLimitHotspot = PW16(usTemperatureLimitHotspot);
	tdp_table->usTemperatureLimitLiquid1 = PW16(usTemperatureLimitLiquid1);
	tdp_table->usTemperatureLimitLiquid2 = PW16(usTemperatureLimitLiquid2);
	tdp_table->usTemperatureLimitVrVddc = PW16(usTemperatureLimitVrVddc);
	tdp_table->usTemperatureLimitVrMvdd = PW16(usTemperatureLimitVrMvdd);
	tdp_table->usTemperatureLimitPlx = PW16(usTemperatureLimitPlx);
	tdp_table->ucLiquid1_I2C_address = PW8(ucLiquid1_I2C_address);
	tdp_table->ucLiquid2_I2C_address = PW8(ucLiquid2_I2C_address);
	tdp_table->ucLiquid_I2C_Line = PW8(ucLiquid_I2C_Line);
	tdp_table->ucVr_I2C_address = PW8(ucVr_I2C_address);
	tdp_table->ucVr_I2C_Line = PW8(ucVr_I2C_Line);
	tdp_table->ucPlx_I2C_address = PW8(ucPlx_I2C_address);
	tdp_table->ucPlx_I2C_Line = PW8(ucPlx_I2C_Line);
	tdp_table->usBoostPowerLimit = PW16(usBoostPowerLimit);	/* Linux: raw */
	tdp_table->ucCKS_LDO_REFSEL = PW8(ucCKS_LDO_REFSEL);
	tdp_table->ucHotSpotOnly = PW8(ucHotSpotOnly);
#undef PW8
#undef PW16

	*cac_tdp_table = tdp_table;

	return 0;
}

static int get_mm_clock_voltage_table(
		struct pp_hwmgr *hwmgr,
		phm_ppt_v1_mm_clock_voltage_dependency_table **tonga_mm_table,
		const uint8_t *mm_dependency_table
		)
{
	uint32_t i, n = FRB_VB8(mm_dependency_table, ATOM_Tonga_MM_Dependency_Table, ucNumEntries);
	phm_ppt_v1_mm_clock_voltage_dependency_table *mm_table;
	phm_ppt_v1_mm_clock_voltage_dependency_record *mm_table_record;

	PP_ASSERT_WITH_CODE((0 != n), "Invalid PowerPlay Table!", return -1);
	mm_table = frb_pp_zalloc(hwmgr, sizeof(*mm_table) + n * sizeof(mm_table->entries[0]));
	if (!mm_table)
		return -ENOMEM;

	mm_table->count = n;

	for (i = 0; i < n; i++) {
		const uint8_t *r = FRB_VB_ENTRY(mm_dependency_table, ATOM_Tonga_MM_Dependency_Table, entries,
						ATOM_Tonga_MM_Dependency_Record, i);

		mm_table_record = &mm_table->entries[i];
		mm_table_record->vddcInd = FRB_VB8(r, ATOM_Tonga_MM_Dependency_Record, ucVddcInd);
		mm_table_record->vddgfx_offset = FRB_VB16(r, ATOM_Tonga_MM_Dependency_Record, usVddgfxOffset);
		mm_table_record->aclk = FRB_VB32(r, ATOM_Tonga_MM_Dependency_Record, ulAClk);
		mm_table_record->samclock = FRB_VB32(r, ATOM_Tonga_MM_Dependency_Record, ulSAMUClk);
		mm_table_record->eclk = FRB_VB32(r, ATOM_Tonga_MM_Dependency_Record, ulEClk);
		mm_table_record->vclk = FRB_VB32(r, ATOM_Tonga_MM_Dependency_Record, ulVClk);
		mm_table_record->dclk = FRB_VB32(r, ATOM_Tonga_MM_Dependency_Record, ulDClk);
	}

	*tonga_mm_table = mm_table;

	return 0;
}

static int get_gpio_table(struct pp_hwmgr *hwmgr,
		struct phm_ppt_v1_gpio_table **pp_tonga_gpio_table,
		const uint8_t *atom_gpio_table)
{
	uint32_t table_size;
	struct phm_ppt_v1_gpio_table *pp_gpio_table;
	struct phm_ppt_v1_information *pp_table_information =
			(struct phm_ppt_v1_information *)(hwmgr->pptable);
	uint8_t idx = FRB_VB8(atom_gpio_table, ATOM_Tonga_GPIO_Table, ucVRHotTriggeredSclkDpmIndex);

	table_size = sizeof(struct phm_ppt_v1_gpio_table);
	pp_gpio_table = frb_pp_zalloc(hwmgr, table_size);
	if (!pp_gpio_table)
		return -ENOMEM;

	if (pp_table_information->vdd_dep_on_sclk->count < idx)
		PP_ASSERT_WITH_CODE(false,
				"SCLK DPM index for VRHot cannot exceed the total sclk level count!",);
	else
		pp_gpio_table->vrhot_triggered_sclk_dpm_index = idx;

	*pp_tonga_gpio_table = pp_gpio_table;

	return 0;
}

/**
 * init_clock_voltage_dependency - Private Function used during initialization.
 * Initialize clock voltage dependency
 * @hwmgr: Pointer to the hardware manager.
 * @powerplay_table: Pointer to the PowerPlay Table.
 */
static int init_clock_voltage_dependency(
		struct pp_hwmgr *hwmgr,
		const uint8_t *powerplay_table
		)
{
	int result = 0;
	struct phm_ppt_v1_information *pp_table_information =
		(struct phm_ppt_v1_information *)(hwmgr->pptable);

	pp_table_information->vdd_dep_on_sclk = NULL;
	pp_table_information->vdd_dep_on_mclk = NULL;
	pp_table_information->mm_dep_table = NULL;
	pp_table_information->pcie_table = NULL;
	pp_table_information->gpio_table = NULL;

	if (FRB_VB16(powerplay_table, PT, usMMDependencyTableOffset) != 0)
		result = get_mm_clock_voltage_table(hwmgr,
		&pp_table_information->mm_dep_table, SUBTABLE(powerplay_table, usMMDependencyTableOffset));

	if (result == 0 && FRB_VB16(powerplay_table, PT, usPowerTuneTableOffset) != 0)
		result = get_cac_tdp_table(hwmgr,
		&pp_table_information->cac_dtp_table, SUBTABLE(powerplay_table, usPowerTuneTableOffset));

	if (result == 0 && FRB_VB16(powerplay_table, PT, usSclkDependencyTableOffset) != 0)
		result = get_sclk_voltage_dependency_table(hwmgr,
		&pp_table_information->vdd_dep_on_sclk, SUBTABLE(powerplay_table, usSclkDependencyTableOffset));

	if (result == 0 && FRB_VB16(powerplay_table, PT, usMclkDependencyTableOffset) != 0)
		result = get_mclk_voltage_dependency_table(hwmgr,
		&pp_table_information->vdd_dep_on_mclk, SUBTABLE(powerplay_table, usMclkDependencyTableOffset));

	if (result == 0 && FRB_VB16(powerplay_table, PT, usPCIETableOffset) != 0)
		result = get_pcie_table(hwmgr,
		&pp_table_information->pcie_table, SUBTABLE(powerplay_table, usPCIETableOffset));

	if (result == 0 && FRB_VB16(powerplay_table, PT, usHardLimitTableOffset) != 0)
		result = get_hard_limits(hwmgr,
		&pp_table_information->max_clock_voltage_on_dc, SUBTABLE(powerplay_table, usHardLimitTableOffset));

	hwmgr->dyn_state.max_clock_voltage_on_dc.sclk =
		pp_table_information->max_clock_voltage_on_dc.sclk;
	hwmgr->dyn_state.max_clock_voltage_on_dc.mclk =
		pp_table_information->max_clock_voltage_on_dc.mclk;
	hwmgr->dyn_state.max_clock_voltage_on_dc.vddc =
		pp_table_information->max_clock_voltage_on_dc.vddc;
	hwmgr->dyn_state.max_clock_voltage_on_dc.vddci =
		pp_table_information->max_clock_voltage_on_dc.vddci;

	if (result == 0 && (NULL != pp_table_information->vdd_dep_on_mclk)
		&& (0 != pp_table_information->vdd_dep_on_mclk->count))
		result = get_valid_clk(hwmgr, &pp_table_information->valid_mclk_values,
		pp_table_information->vdd_dep_on_mclk);

	if (result == 0 && (NULL != pp_table_information->vdd_dep_on_sclk)
		&& (0 != pp_table_information->vdd_dep_on_sclk->count))
		result = get_valid_clk(hwmgr, &pp_table_information->valid_sclk_values,
		pp_table_information->vdd_dep_on_sclk);

	/* Linux tests the computed pointer, which is never NULL: the GPIO table is always read */
	if (!result)
		result = get_gpio_table(hwmgr, &pp_table_information->gpio_table,
				SUBTABLE(powerplay_table, usGPIOTableOffset));

	return result;
}

/**
 * init_over_drive_limits - Retrieves the (signed) Overdrive limits from VBIOS.
 */
static int init_over_drive_limits(
		struct pp_hwmgr *hwmgr,
		const uint8_t *powerplay_table)
{
	hwmgr->platform_descriptor.overdriveLimit.engineClock = FRB_VB32(powerplay_table, PT, ulMaxODEngineClock);
	hwmgr->platform_descriptor.overdriveLimit.memoryClock = FRB_VB32(powerplay_table, PT, ulMaxODMemoryClock);

	hwmgr->platform_descriptor.minOverdriveVDDC = 0;
	hwmgr->platform_descriptor.maxOverdriveVDDC = 0;
	hwmgr->platform_descriptor.overdriveVDDCStep = 0;

	return 0;
}

/**
 * init_thermal_controller - Private Function used during initialization.
 * The thermal controller and the fan table (Polaris: revision 9 and up).
 */
static int init_thermal_controller(
		struct pp_hwmgr *hwmgr,
		const uint8_t *powerplay_table
		)
{
	const uint8_t *fan_table;
	const uint8_t *thermal_controller;
	uint8_t fan_parameters, rev;

	PP_ASSERT_WITH_CODE((0 != FRB_VB16(powerplay_table, PT, usThermalControllerOffset)),
		"Thermal controller table not set!", return -1);
	thermal_controller = SUBTABLE(powerplay_table, usThermalControllerOffset);

#define TC8(f)	FRB_VB8(thermal_controller, ATOM_Tonga_Thermal_Controller, f)
	hwmgr->thermal_controller.ucType = TC8(ucType);
	hwmgr->thermal_controller.ucI2cLine = TC8(ucI2cLine);
	hwmgr->thermal_controller.ucI2cAddress = TC8(ucI2cAddress);
	fan_parameters = TC8(ucFanParameters);

	hwmgr->thermal_controller.fanInfo.bNoFan =
		(0 != (fan_parameters & ATOM_TONGA_PP_FANPARAMETERS_NOFAN));

	hwmgr->thermal_controller.fanInfo.ucTachometerPulsesPerRevolution =
		fan_parameters & ATOM_TONGA_PP_FANPARAMETERS_TACHOMETER_PULSES_PER_REVOLUTION_MASK;

	hwmgr->thermal_controller.fanInfo.ulMinRPM = TC8(ucFanMinRPM) * 100UL;
	hwmgr->thermal_controller.fanInfo.ulMaxRPM = TC8(ucFanMaxRPM) * 100UL;
#undef TC8

	set_hw_cap(
			hwmgr,
			ATOM_TONGA_PP_THERMALCONTROLLER_NONE != hwmgr->thermal_controller.ucType,
			PHM_PlatformCaps_ThermalController
		  );

	if (0 == FRB_VB16(powerplay_table, PT, usFanTableOffset)) {
		hwmgr->thermal_controller.use_hw_fan_control = 1;
		return 0;
	}

	fan_table = SUBTABLE(powerplay_table, usFanTableOffset);
	rev = FRB_VB8(fan_table, PPTable_Generic_SubTable_Header, ucRevId);

	PP_ASSERT_WITH_CODE((0 < rev), "Unsupported fan table format!", return -1);
	PP_ASSERT_WITH_CODE((rev >= 9), "Tonga/Fiji fan table: not ported", return -1);

	hwmgr->thermal_controller.advanceFanControlParameters.ulCycleDelay
		= 100000;
	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
		PHM_PlatformCaps_MicrocodeFanControl);

	{
		struct pp_advance_fan_control_parameters *p = &hwmgr->thermal_controller.advanceFanControlParameters;
#define FT8(f)	FRB_VB8(fan_table, ATOM_Polaris_Fan_Table, f)
#define FT16(f)	FRB_VB16(fan_table, ATOM_Polaris_Fan_Table, f)
#define FT32(f)	FRB_VB32(fan_table, ATOM_Polaris_Fan_Table, f)
		p->ucTHyst = FT8(ucTHyst);
		p->usTMin = FT16(usTMin);
		p->usTMed = FT16(usTMed);
		p->usTHigh = FT16(usTHigh);
		p->usPWMMin = FT16(usPWMMin);
		p->usPWMMed = FT16(usPWMMed);
		p->usPWMHigh = FT16(usPWMHigh);
		p->usTMax = FT16(usTMax);
		p->ucFanControlMode = FT8(ucFanControlMode);
		p->usDefaultMaxFanPWM = FT16(usFanPWMMax);
		p->usDefaultFanOutputSensitivity = 4836;
		p->usFanOutputSensitivity = FT16(usFanOutputSensitivity);
		p->usDefaultMaxFanRPM = FT16(usFanRPMMax);
		/* PPTable stores it in 10Khz unit for 2 decimal places.  SMC wants MHz. */
		p->ulMinFanSCLKAcousticLimit = FT32(ulMinFanSCLKAcousticLimit) / 100;
		p->ucTargetTemperature = FT8(ucTargetTemperature);
		p->ucMinimumPWMLimit = FT8(ucMinimumPWMLimit);
		p->usFanGainEdge = FT16(usFanGainEdge);
		p->usFanGainHotspot = FT16(usFanGainHotspot);
		p->usFanGainLiquid = FT16(usFanGainLiquid);
		p->usFanGainVrVddc = FT16(usFanGainVrVddc);
		p->usFanGainVrMvdd = FT16(usFanGainVrMvdd);
		p->usFanGainPlx = FT16(usFanGainPlx);
		p->usFanGainHbm = FT16(usFanGainHbm);
		/* one-byte fields (Linux: le16_to_cpu of them) */
		p->ucEnableZeroRPM = FT8(ucEnableZeroRPM);
		p->ucFanStopTemperature = FT8(ucFanStopTemperature);
		p->ucFanStartTemperature = FT8(ucFanStartTemperature);
#undef FT8
#undef FT16
#undef FT32
	}

	return 0;
}

/**
 * check_powerplay_tables - Private Function used during initialization.
 * Inspect the PowerPlay table for obvious signs of corruption.
 */
static int check_powerplay_tables(
		struct pp_hwmgr *hwmgr,
		const uint8_t *powerplay_table
		)
{
	const uint8_t *state_arrays = SUBTABLE(powerplay_table, usStateArrayOffset);

	PP_ASSERT_WITH_CODE((ATOM_Tonga_TABLE_REVISION_TONGA <=
		FRB_VB8(powerplay_table, PT, sHeader.ucTableFormatRevision)),
		"Unsupported PPTable format!", return -1);
	PP_ASSERT_WITH_CODE((0 != FRB_VB16(powerplay_table, PT, usStateArrayOffset)),
		"State table is not set!", return -1);
	PP_ASSERT_WITH_CODE((0 < FRB_VB16(powerplay_table, PT, sHeader.usStructureSize)),
		"Invalid PowerPlay Table!", return -1);
	PP_ASSERT_WITH_CODE((0 < FRB_VB8(state_arrays, ATOM_Tonga_State_Array, ucNumEntries)),
		"Invalid PowerPlay Table!", return -1);

	return 0;
}

static int pp_tables_v1_0_initialize(struct pp_hwmgr *hwmgr)
{
	int result = 0;
	const uint8_t *powerplay_table;

	hwmgr->pptable = frb_pp_zalloc(hwmgr, sizeof(struct phm_ppt_v1_information));

	PP_ASSERT_WITH_CODE((NULL != hwmgr->pptable),
			    "Failed to allocate hwmgr->pptable!", return -ENOMEM);

	powerplay_table = get_powerplay_table(hwmgr);

	PP_ASSERT_WITH_CODE((NULL != powerplay_table),
		"Missing PowerPlay Table!", return -1);

	result = check_powerplay_tables(hwmgr, powerplay_table);

	PP_ASSERT_WITH_CODE((result == 0),
			    "check_powerplay_tables failed", return result);

	result = set_platform_caps(hwmgr, FRB_VB32(powerplay_table, PT, ulPlatformCaps));

	PP_ASSERT_WITH_CODE((result == 0),
			    "set_platform_caps failed", return result);

	result = init_thermal_controller(hwmgr, powerplay_table);

	PP_ASSERT_WITH_CODE((result == 0),
			    "init_thermal_controller failed", return result);

	result = init_over_drive_limits(hwmgr, powerplay_table);

	PP_ASSERT_WITH_CODE((result == 0),
			    "init_over_drive_limits failed", return result);

	result = init_clock_voltage_dependency(hwmgr, powerplay_table);

	PP_ASSERT_WITH_CODE((result == 0),
			    "init_clock_voltage_dependency failed", return result);

	result = init_dpm_2_parameters(hwmgr, powerplay_table);

	PP_ASSERT_WITH_CODE((result == 0),
			    "init_dpm_2_parameters failed", return result);

	return result;
}

static int pp_tables_v1_0_uninitialize(struct pp_hwmgr *hwmgr)
{
	struct phm_ppt_v1_information *pp_table_information =
		(struct phm_ppt_v1_information *)(hwmgr->pptable);

	if (!pp_table_information)
		return 0;

	frb_pp_free(hwmgr, pp_table_information->vdd_dep_on_sclk);
	frb_pp_free(hwmgr, pp_table_information->vdd_dep_on_mclk);
	frb_pp_free(hwmgr, pp_table_information->valid_mclk_values);
	frb_pp_free(hwmgr, pp_table_information->valid_sclk_values);
	frb_pp_free(hwmgr, pp_table_information->vddc_lookup_table);
	frb_pp_free(hwmgr, pp_table_information->vddgfx_lookup_table);
	frb_pp_free(hwmgr, pp_table_information->mm_dep_table);
	frb_pp_free(hwmgr, pp_table_information->cac_dtp_table);
	frb_pp_free(hwmgr, hwmgr->dyn_state.cac_dtp_table);
	hwmgr->dyn_state.cac_dtp_table = NULL;
	frb_pp_free(hwmgr, pp_table_information->ppm_parameter_table);
	frb_pp_free(hwmgr, pp_table_information->pcie_table);
	frb_pp_free(hwmgr, pp_table_information->gpio_table);
	frb_pp_free(hwmgr, hwmgr->pptable);
	hwmgr->pptable = NULL;

	return 0;
}

const struct pp_table_func pptable_v1_0_funcs = {
	.pptable_init = pp_tables_v1_0_initialize,
	.pptable_fini = pp_tables_v1_0_uninitialize,
};

int get_number_of_powerplay_table_entries_v1_0(struct pp_hwmgr *hwmgr)
{
	const uint8_t *pp_table = get_powerplay_table(hwmgr);

	PP_ASSERT_WITH_CODE((NULL != pp_table),
			"Missing PowerPlay Table!", return -1);
	PP_ASSERT_WITH_CODE((FRB_VB8(pp_table, PT, sHeader.ucTableFormatRevision) >=
			ATOM_Tonga_TABLE_REVISION_TONGA),
			"Incorrect PowerPlay table revision!", return -1);

	return (uint32_t)FRB_VB8(SUBTABLE(pp_table, usStateArrayOffset), ATOM_Tonga_State_Array, ucNumEntries);
}

/*
 * Private function to convert flags stored in the BIOS to software flags in PowerPlay.
 */
static uint32_t make_classification_flags(struct pp_hwmgr *hwmgr,
		uint16_t classification, uint16_t classification2)
{
	uint32_t result = 0;

	if (classification & ATOM_PPLIB_CLASSIFICATION_BOOT)
		result |= PP_StateClassificationFlag_Boot;

	if (classification & ATOM_PPLIB_CLASSIFICATION_THERMAL)
		result |= PP_StateClassificationFlag_Thermal;

	if (classification & ATOM_PPLIB_CLASSIFICATION_LIMITEDPOWERSOURCE)
		result |= PP_StateClassificationFlag_LimitedPowerSource;

	if (classification & ATOM_PPLIB_CLASSIFICATION_REST)
		result |= PP_StateClassificationFlag_Rest;

	if (classification & ATOM_PPLIB_CLASSIFICATION_FORCED)
		result |= PP_StateClassificationFlag_Forced;

	if (classification & ATOM_PPLIB_CLASSIFICATION_ACPI)
		result |= PP_StateClassificationFlag_ACPI;

	if (classification2 & ATOM_PPLIB_CLASSIFICATION2_LIMITEDPOWERSOURCE_2)
		result |= PP_StateClassificationFlag_LimitedPowerSource_2;

	return result;
}

/**
 * get_powerplay_table_entry_v1_0 - Create a Power State out of an entry in the PowerPlay table.
 * The callback gets the ATOM_Tonga_State and the PowerPlay table as VBIOS
 * byte pointers. The VCE state table Linux reads afterwards is not (no VCE).
 */
int get_powerplay_table_entry_v1_0(struct pp_hwmgr *hwmgr,
		uint32_t entry_index, struct pp_power_state *power_state,
		int (*call_back_func)(struct pp_hwmgr *, const uint8_t *,
				struct pp_power_state *, const uint8_t *, uint32_t))
{
	int result = 0;
	const uint8_t *state_arrays;
	const uint8_t *state_entry;
	const uint8_t *pp_table = get_powerplay_table(hwmgr);

	PP_ASSERT_WITH_CODE((NULL != pp_table), "Missing PowerPlay Table!", return -1;);
	power_state->classification.bios_index = entry_index;

	if (FRB_VB8(pp_table, PT, sHeader.ucTableFormatRevision) >= ATOM_Tonga_TABLE_REVISION_TONGA) {
		state_arrays = SUBTABLE(pp_table, usStateArrayOffset);

		PP_ASSERT_WITH_CODE((0 < FRB_VB16(pp_table, PT, usStateArrayOffset)),
				"Invalid PowerPlay Table State Array Offset.", return -1);
		PP_ASSERT_WITH_CODE((0 < FRB_VB8(state_arrays, ATOM_Tonga_State_Array, ucNumEntries)),
				"Invalid PowerPlay Table State Array.", return -1);
		PP_ASSERT_WITH_CODE((entry_index <= FRB_VB8(state_arrays, ATOM_Tonga_State_Array, ucNumEntries)),
				"Invalid PowerPlay Table State Array Entry.", return -1);

		state_entry = FRB_VB_ENTRY(state_arrays, ATOM_Tonga_State_Array, entries, ATOM_Tonga_State,
					   entry_index);

		result = call_back_func(hwmgr, state_entry, power_state, pp_table,
				make_classification_flags(hwmgr,
					FRB_VB16(state_entry, ATOM_Tonga_State, usClassification),
					FRB_VB16(state_entry, ATOM_Tonga_State, usClassification2)));
	}

	if (!result && (power_state->classification.flags &
			PP_StateClassificationFlag_Boot))
		result = hwmgr->hwmgr_func->patch_boot_state(hwmgr, &(power_state->hardware));

	return result;
}
