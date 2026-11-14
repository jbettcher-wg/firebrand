/*
 * pm/powerplay/smumgr/polaris10_smumgr.h and smu7_smumgr.h (Linux 7.1.1):
 * the SMU manager's state, and the smu7_smumgr helpers the DPM code calls
 * (in frb_smu7.c, which also holds the SMU start).
 *
 * The SMC structures (SMU74_Discrete_*) are include/pp/smu74_discrete.h,
 * unchanged; frb_polaris10_smumgr.c checks their sizes and the offsets the
 * firmware header gives against the E9173's SMC RAM.
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
#ifndef FRB_POLARIS10_SMUMGR_H
#define FRB_POLARIS10_SMUMGR_H

#include "frb_hwmgr.h"
#include "frb_ppatomctrl.h"
#include "include/pp/smu74.h"
#include "include/pp/smu74_discrete.h"

#define SMC_RAM_END 0x40000

/* smu7_smumgr.h: the SMU buffers live in frb_smu7.c; the table starts here */
struct smu7_smumgr {
	uint32_t                             soft_regs_start;
	uint32_t                             dpm_table_start;
	uint32_t                             mc_reg_table_start;
	uint32_t                             fan_table_start;
	uint32_t                             arb_table_start;
	uint32_t                             ulv_setting_starts;
	uint8_t                              security_hard_key;
	uint32_t                             acpi_optimization;
	uint32_t                             avfs_btc_param;
};

struct polaris10_pt_defaults {
	uint8_t   SviLoadLineEn;
	uint8_t   SviLoadLineVddC;
	uint8_t   TDC_VDDC_ThrottleReleaseLimitPerc;
	uint8_t   TDC_MAWt;
	uint8_t   TdcWaterfallCtl;
	uint8_t   DTEAmbientTempBase;

	uint32_t  DisplayCac;
	uint32_t  BAPM_TEMP_GRADIENT;
	uint16_t  BAPMTI_R[SMU74_DTE_ITERATIONS * SMU74_DTE_SOURCES * SMU74_DTE_SINKS];
	uint16_t  BAPMTI_RC[SMU74_DTE_ITERATIONS * SMU74_DTE_SOURCES * SMU74_DTE_SINKS];
};

struct polaris10_range_table {
	uint32_t trans_lower_frequency; /* in 10khz */
	uint32_t trans_upper_frequency;
};

struct polaris10_smumgr {
	struct smu7_smumgr smu7_data;
	uint8_t protected_mode;
	SMU74_Discrete_DpmTable              smc_state_table;
	struct SMU74_Discrete_Ulv            ulv_setting;
	struct SMU74_Discrete_PmFuses  power_tune_table;
	struct polaris10_range_table                range_table[NUM_SCLK_RANGE];
	const struct polaris10_pt_defaults       *power_tune_defaults;
	uint32_t               bif_sclk_table[SMU74_MAX_LEVELS_LINK];
};

extern const struct pp_smumgr_func polaris10_smu_funcs;

/* smu7_smumgr.c's helpers are in frb_smu7.c (prototypes in frb_device.h) */
#define smu7_copy_bytes_to_smc(hwmgr, a, s, n, l)	frb_smu7_copy_bytes_to_smc((hwmgr)->adev, (a), (s), (n), (l))
#define smu7_read_smc_sram_dword(hwmgr, a, v, l)	frb_smu7_read_smc_sram_dword((hwmgr)->adev, (a), (v), (l))
#define smu7_write_smc_sram_dword(hwmgr, a, v, l)	frb_smu7_write_smc_sram_dword((hwmgr)->adev, (a), (v), (l))

#endif /* FRB_POLARIS10_SMUMGR_H */
