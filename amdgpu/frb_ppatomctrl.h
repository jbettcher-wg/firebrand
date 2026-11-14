/*
 * pm/powerplay/hwmgr/ppatomctrl.h (Linux 7.1.1): the structures the ATOM
 * helpers hand to the smu7 hardware manager, and the helpers the Polaris
 * paths call. The others (SI/CI/Kong dividers, the MC register table) are
 * not ported.
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
#ifndef FRB_PPATOMCTRL_H
#define FRB_PPATOMCTRL_H

#include "frb_hwmgr.h"

#define PP_ATOMCTRL_MAX_VOLTAGE_ENTRIES 32

struct pp_atomctrl_clock_dividers_vi {
	uint32_t    pll_post_divider;		/* post divider value */
	uint32_t    real_clock;
	/* pp_atomctrl_tcipll_fb_divider ul_fb_div: two plain fields here (no bitfields) */
	uint32_t    ul_fb_div_frac;
	uint32_t    ul_fb_div;
	uint8_t   uc_pll_ref_div;
	uint8_t   uc_pll_post_div;
	uint8_t   uc_pll_cntl_flag;
};
typedef struct pp_atomctrl_clock_dividers_vi pp_atomctrl_clock_dividers_vi;

struct pp_atomctrl_clock_dividers_ai {
	u16 usSclk_fcw_frac;
	u16  usSclk_fcw_int;
	u8   ucSclkPostDiv;
	u8   ucSclkVcoMode;
	u8   ucSclkPllRange;
	u8   ucSscEnable;
	u16  usSsc_fcw1_frac;
	u16  usSsc_fcw1_int;
	u16  usReserved;
	u16  usPcc_fcw_int;
	u16  usSsc_fcw_slew_frac;
	u16  usPcc_fcw_slew_frac;
};
typedef struct pp_atomctrl_clock_dividers_ai pp_atomctrl_clock_dividers_ai;

struct pp_atomctrl_voltage_table_entry {
	uint16_t value;
	uint32_t smio_low;
};
typedef struct pp_atomctrl_voltage_table_entry pp_atomctrl_voltage_table_entry;

struct pp_atomctrl_voltage_table {
	uint32_t count;
	uint32_t mask_low;
	uint32_t phase_delay;
	pp_atomctrl_voltage_table_entry entries[PP_ATOMCTRL_MAX_VOLTAGE_ENTRIES];
};
typedef struct pp_atomctrl_voltage_table pp_atomctrl_voltage_table;

#define MAX_SCLK_RANGE 8

struct pp_atom_ctrl_sclk_range_table_entry {
	uint8_t  ucVco_setting;
	uint8_t  ucPostdiv;
	uint16_t usFcw_pcc;
	uint16_t usFcw_trans_upper;
	uint16_t usRcw_trans_lower;
};

struct pp_atom_ctrl_sclk_range_table {
	struct pp_atom_ctrl_sclk_range_table_entry entry[MAX_SCLK_RANGE];
};

struct pp_atomctrl_gpio_pin_assignment {
	uint16_t                   us_gpio_pin_aindex;
	uint8_t                    uc_gpio_pin_bit_shift;
};
typedef struct pp_atomctrl_gpio_pin_assignment pp_atomctrl_gpio_pin_assignment;

struct pp_atom_ctrl__avfs_parameters {
	uint32_t  ulAVFS_meanNsigma_Acontant0;
	uint32_t  ulAVFS_meanNsigma_Acontant1;
	uint32_t  ulAVFS_meanNsigma_Acontant2;
	uint16_t usAVFS_meanNsigma_DC_tol_sigma;
	uint16_t usAVFS_meanNsigma_Platform_mean;
	uint16_t usAVFS_meanNsigma_Platform_sigma;
	uint32_t  ulGB_VDROOP_TABLE_CKSOFF_a0;
	uint32_t  ulGB_VDROOP_TABLE_CKSOFF_a1;
	uint32_t  ulGB_VDROOP_TABLE_CKSOFF_a2;
	uint32_t  ulGB_VDROOP_TABLE_CKSON_a0;
	uint32_t  ulGB_VDROOP_TABLE_CKSON_a1;
	uint32_t  ulGB_VDROOP_TABLE_CKSON_a2;
	uint32_t  ulAVFSGB_FUSE_TABLE_CKSOFF_m1;
	uint16_t  usAVFSGB_FUSE_TABLE_CKSOFF_m2;
	uint32_t  ulAVFSGB_FUSE_TABLE_CKSOFF_b;
	uint32_t  ulAVFSGB_FUSE_TABLE_CKSON_m1;
	uint16_t  usAVFSGB_FUSE_TABLE_CKSON_m2;
	uint32_t  ulAVFSGB_FUSE_TABLE_CKSON_b;
	uint16_t  usMaxVoltage_0_25mv;
	uint8_t  ucEnableGB_VDROOP_TABLE_CKSOFF;
	uint8_t  ucEnableGB_VDROOP_TABLE_CKSON;
	uint8_t  ucEnableGB_FUSE_TABLE_CKSOFF;
	uint8_t  ucEnableGB_FUSE_TABLE_CKSON;
	uint16_t usPSM_Age_ComFactor;
	uint8_t  ucEnableApplyAVFS_CKS_OFF_Voltage;
	uint8_t  ucReserved;
};

struct _AtomCtrl_HiLoLeakageOffsetTable {
	uint16_t usHiLoLeakageThreshold;
	uint16_t usEdcDidtLoDpm7TableOffset;
	uint16_t usEdcDidtHiDpm7TableOffset;
};
typedef struct _AtomCtrl_HiLoLeakageOffsetTable AtomCtrl_HiLoLeakageOffsetTable;

struct _AtomCtrl_EDCLeakgeTable {
	uint32_t DIDT_REG[24];
};
typedef struct _AtomCtrl_EDCLeakgeTable AtomCtrl_EDCLeakgeTable;

bool atomctrl_get_pp_assign_pin(struct pp_hwmgr *hwmgr, const uint32_t pinId,
				pp_atomctrl_gpio_pin_assignment *gpio_pin_assignment);
bool atomctrl_is_asic_internal_ss_supported(struct pp_hwmgr *hwmgr);
int atomctrl_set_engine_dram_timings_rv770(struct pp_hwmgr *hwmgr, uint32_t engine_clock,
					   uint32_t memory_clock);
int atomctrl_get_dfs_pll_dividers_vi(struct pp_hwmgr *hwmgr, uint32_t clock_value,
				     pp_atomctrl_clock_dividers_vi *dividers);
bool atomctrl_is_voltage_controlled_by_gpio_v3(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
					       uint8_t voltage_mode);
int atomctrl_get_voltage_table_v3(struct pp_hwmgr *hwmgr, uint8_t voltage_type, uint8_t voltage_mode,
				  pp_atomctrl_voltage_table *voltage_table);
int atomctrl_read_efuse(struct pp_hwmgr *hwmgr, uint16_t start_index, uint16_t end_index, uint32_t *efuse);
int atomctrl_get_engine_pll_dividers_ai(struct pp_hwmgr *hwmgr, uint32_t clock_value,
					pp_atomctrl_clock_dividers_ai *dividers);
int atomctrl_set_ac_timing_ai(struct pp_hwmgr *hwmgr, uint32_t memory_clock, uint8_t level);
int atomctrl_get_voltage_evv_on_sclk_ai(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
					uint32_t sclk, uint16_t virtual_voltage_Id, uint32_t *voltage);
int atomctrl_get_smc_sclk_range_table(struct pp_hwmgr *hwmgr, struct pp_atom_ctrl_sclk_range_table *table);
int atomctrl_get_avfs_information(struct pp_hwmgr *hwmgr, struct pp_atom_ctrl__avfs_parameters *param);
int atomctrl_get_svi2_info(struct pp_hwmgr *hwmgr, uint8_t voltage_type, uint8_t *svd_gpio_id,
			   uint8_t *svc_gpio_id, uint16_t *load_line);
int atomctrl_get_edc_hilo_leakage_offset_table(struct pp_hwmgr *hwmgr, AtomCtrl_HiLoLeakageOffsetTable *table);
int atomctrl_get_vddc_shared_railinfo(struct pp_hwmgr *hwmgr, uint8_t *shared_rail);
int atomctrl_get_voltage_evv_on_sclk(struct pp_hwmgr *hwmgr, uint8_t voltage_type,
				     uint32_t sclk, uint16_t virtual_voltage_Id, uint16_t *voltage);
int atomctrl_get_edc_leakage_table(struct pp_hwmgr *hwmgr, AtomCtrl_EDCLeakgeTable *table, uint16_t offset);

#endif /* FRB_PPATOMCTRL_H */
