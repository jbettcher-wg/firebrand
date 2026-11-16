/*
 * Copyright 2013 Advanced Micro Devices, Inc.
 * Copyright 2026 kouta-kun and Claude
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

/*
 * Sea Islands (Bonaire, Hawaii) dynamic power management: the Linux radeon
 * driver's ci_dpm.c, ci_smc.c and the parts of r600_dpm.c and
 * radeon_atombios.c they call, as a freestanding library.
 *
 * The SMU (an embedded controller running HAWAII_smc.bin) does the clock and
 * voltage switching by itself once its SRAM tables are filled in; this code
 * parses the VBIOS's PowerPlay tables, fills those tables, starts the
 * firmware and then only sends it messages. The structures below are
 * ci_dpm.h's with the fields ci_dpm.c reaches into rdev->pm.dpm for added
 * (struct rdn_ci_dpm stands in for rdev).
 *
 * Deviations from Linux, all marked at their site: no interrupt handler
 * (the thermal alert's registers are programmed, nothing receives it), no
 * DRM mode list (the vblank time and refresh come in as arguments), no ACPI
 * (no PCIe performance requests), and UVD/VCE/SAMU/ACP DPM are compile-time
 * stubs that keep those features off.
 *
 * Numbers follow Linux's units: clocks in 10 kHz, voltages in mV,
 * temperatures in millidegrees Celsius.
 */
#ifndef RDN_CI_DPM_H
#define RDN_CI_DPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdn_card.h"
#include "linux/ppsmc.h"

#define SMU__NUM_SCLK_DPM_STATE  8
#define SMU__NUM_MCLK_DPM_LEVELS 6
#define SMU__NUM_LCLK_DPM_LEVELS 8
#define SMU__NUM_PCIE_DPM_LEVELS 8
#include "linux/smu7_discrete.h"

/* radeon.h's enum radeon_pcie_gen */
enum rdn_ci_pcie_gen {
	RDN_CI_PCIE_GEN1 = 0,
	RDN_CI_PCIE_GEN2 = 1,
	RDN_CI_PCIE_GEN3 = 2,
	RDN_CI_PCIE_GEN_INVALID = 0xffff
};

/* radeon.h's RADEON_PCIE_SPEED_*: what the host's root port can do (rdn_ci_dpm_init's sys_pcie_mask) */
#define RDN_CI_PCIE_SPEED_25	1
#define RDN_CI_PCIE_SPEED_50	2
#define RDN_CI_PCIE_SPEED_80	4

/* radeon.h's enum radeon_dpm_forced_level */
enum rdn_ci_forced_level {
	RDN_CI_FORCED_LEVEL_AUTO = 0,
	RDN_CI_FORCED_LEVEL_LOW = 1,
	RDN_CI_FORCED_LEVEL_HIGH = 2
};

/* Which VBIOS power state rdn_ci_dpm_set_power_state() requests (radeon_pm.c's enum radeon_pm_state_type) */
enum rdn_ci_power_state {
	RDN_CI_STATE_BOOT,		/* the state ASIC_Init left: ATOM_PPLIB_CLASSIFICATION_BOOT */
	RDN_CI_STATE_BATTERY,		/* ATOM_PPLIB_CLASSIFICATION_UI_BATTERY, the lowest */
	RDN_CI_STATE_BALANCED,		/* ATOM_PPLIB_CLASSIFICATION_UI_BALANCED, else performance */
	RDN_CI_STATE_PERFORMANCE	/* ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE */
};

/* ---- ci_dpm.h ---- */

#define CISLANDS_MAX_HARDWARE_POWERLEVELS 2

#define CISLANDS_UNUSED_GPIO_PIN 0x7F

struct ci_pl {
	uint32_t mclk;
	uint32_t sclk;
	enum rdn_ci_pcie_gen pcie_gen;
	uint16_t pcie_lane;
};

struct ci_ps {
	uint16_t performance_level_count;
	bool dc_compatible;
	uint32_t sclk_t;
	struct ci_pl performance_levels[CISLANDS_MAX_HARDWARE_POWERLEVELS];
};

struct ci_dpm_level {
	bool enabled;
	uint32_t value;
	uint32_t param1;
};

#define CISLAND_MAX_DEEPSLEEP_DIVIDER_ID 5
#define MAX_REGULAR_DPM_NUMBER 8
#define CISLAND_MINIMUM_ENGINE_CLOCK 800

struct ci_single_dpm_table {
	uint32_t count;
	struct ci_dpm_level dpm_levels[MAX_REGULAR_DPM_NUMBER];
};

struct ci_dpm_table {
	struct ci_single_dpm_table sclk_table;
	struct ci_single_dpm_table mclk_table;
	struct ci_single_dpm_table pcie_speed_table;
	struct ci_single_dpm_table vddc_table;
	struct ci_single_dpm_table vddci_table;
	struct ci_single_dpm_table mvdd_table;
};

/* radeon_mode.h */
#define MAX_AC_TIMING_ENTRIES 16
#define VBIOS_MC_REGISTER_ARRAY_SIZE 32
#define VBIOS_MAX_AC_TIMING_ENTRIES 20
#define MAX_VOLTAGE_ENTRIES 32

struct ci_mc_reg_entry {
	uint32_t mclk_max;
	uint32_t mc_data[SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE];
};

struct ci_mc_reg_table {
	uint8_t last;
	uint8_t num_entries;
	uint16_t valid_flag;
	struct ci_mc_reg_entry mc_reg_table_entry[MAX_AC_TIMING_ENTRIES];
	SMU7_Discrete_MCRegisterAddress mc_reg_address[SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE];
};

struct ci_ulv_parm {
	bool supported;
	uint32_t cg_ulv_parameter;
	uint32_t volt_change_delay;
	struct ci_pl pl;
};

#define CISLANDS_MAX_LEAKAGE_COUNT  8

struct ci_leakage_voltage {
	uint16_t count;
	uint16_t leakage_id[CISLANDS_MAX_LEAKAGE_COUNT];
	uint16_t actual_voltage[CISLANDS_MAX_LEAKAGE_COUNT];
};

struct ci_dpm_level_enable_mask {
	uint32_t uvd_dpm_enable_mask;
	uint32_t vce_dpm_enable_mask;
	uint32_t acp_dpm_enable_mask;
	uint32_t samu_dpm_enable_mask;
	uint32_t sclk_dpm_enable_mask;
	uint32_t mclk_dpm_enable_mask;
	uint32_t pcie_dpm_enable_mask;
};

struct ci_vbios_boot_state {
	uint16_t mvdd_bootup_value;
	uint16_t vddc_bootup_value;
	uint16_t vddci_bootup_value;
	uint32_t sclk_bootup_value;
	uint32_t mclk_bootup_value;
	uint16_t pcie_gen_bootup_value;
	uint16_t pcie_lane_bootup_value;
};

struct ci_clock_registers {
	uint32_t cg_spll_func_cntl;
	uint32_t cg_spll_func_cntl_2;
	uint32_t cg_spll_func_cntl_3;
	uint32_t cg_spll_func_cntl_4;
	uint32_t cg_spll_spread_spectrum;
	uint32_t cg_spll_spread_spectrum_2;
	uint32_t dll_cntl;
	uint32_t mclk_pwrmgt_cntl;
	uint32_t mpll_ad_func_cntl;
	uint32_t mpll_dq_func_cntl;
	uint32_t mpll_func_cntl;
	uint32_t mpll_func_cntl_1;
	uint32_t mpll_func_cntl_2;
	uint32_t mpll_ss1;
	uint32_t mpll_ss2;
};

struct ci_thermal_temperature_setting {
	int32_t temperature_low;
	int32_t temperature_high;
	int32_t temperature_shutdown;
};

struct ci_pcie_perf_range {
	uint16_t max;
	uint16_t min;
};

enum ci_pt_config_reg_type {
	CISLANDS_CONFIGREG_MMR = 0,
	CISLANDS_CONFIGREG_SMC_IND,
	CISLANDS_CONFIGREG_DIDT_IND,
	CISLANDS_CONFIGREG_CACHE,
	CISLANDS_CONFIGREG_MAX
};

#define POWERCONTAINMENT_FEATURE_BAPM            0x00000001
#define POWERCONTAINMENT_FEATURE_TDCLimit        0x00000002
#define POWERCONTAINMENT_FEATURE_PkgPwrLimit     0x00000004

struct ci_pt_config_reg {
	uint32_t offset;
	uint32_t mask;
	uint32_t shift;
	uint32_t value;
	enum ci_pt_config_reg_type type;
};

struct ci_pt_defaults {
	uint8_t svi_load_line_en;
	uint8_t svi_load_line_vddc;
	uint8_t tdc_vddc_throttle_release_limit_perc;
	uint8_t tdc_mawt;
	uint8_t tdc_waterfall_ctl;
	uint8_t dte_ambient_temp_base;
	uint32_t display_cac;
	uint32_t bapm_temp_gradient;
	uint16_t bapmti_r[SMU7_DTE_ITERATIONS * SMU7_DTE_SOURCES * SMU7_DTE_SINKS];
	uint16_t bapmti_rc[SMU7_DTE_ITERATIONS * SMU7_DTE_SOURCES * SMU7_DTE_SINKS];
};

#define DPMTABLE_OD_UPDATE_SCLK     0x00000001
#define DPMTABLE_OD_UPDATE_MCLK     0x00000002
#define DPMTABLE_UPDATE_SCLK        0x00000004
#define DPMTABLE_UPDATE_MCLK        0x00000008

/* radeon_mode.h's struct atom_voltage_table */
struct atom_voltage_table_entry {
	uint16_t value;
	uint32_t smio_low;
};

struct atom_voltage_table {
	uint32_t count;
	uint32_t mask_low;
	uint32_t phase_delay;
	struct atom_voltage_table_entry entries[MAX_VOLTAGE_ENTRIES];
};

/* radeon.h's struct radeon_ps: the VBIOS's view of a power state */
struct rdn_ci_rps {
	uint32_t caps;		/* vbios flags */
	uint32_t classification;	/* vbios flags (usClassification; Linux: class, a C++ keyword) */
	uint32_t classification2;	/* vbios flags */
	/* UVD clocks */
	uint32_t vclk;
	uint32_t dclk;
	/* VCE clocks */
	uint32_t evclk;
	uint32_t ecclk;
	bool vce_active;
	/* asic priv (ps_priv) */
	struct ci_ps ps;
};

struct ci_power_info {
	struct ci_dpm_table dpm_table;
	uint32_t voltage_control;
	uint32_t mvdd_control;
	uint32_t vddci_control;
	uint32_t active_auto_throttle_sources;
	struct ci_clock_registers clock_registers;
	uint16_t acpi_vddc;
	uint16_t acpi_vddci;
	enum rdn_ci_pcie_gen force_pcie_gen;
	enum rdn_ci_pcie_gen acpi_pcie_gen;
	struct ci_leakage_voltage vddc_leakage;
	struct ci_leakage_voltage vddci_leakage;
	uint16_t max_vddc_in_pp_table;
	uint16_t min_vddc_in_pp_table;
	uint16_t max_vddci_in_pp_table;
	uint16_t min_vddci_in_pp_table;
	uint32_t mclk_strobe_mode_threshold;
	uint32_t mclk_stutter_mode_threshold;
	uint32_t mclk_edc_enable_threshold;
	uint32_t mclk_edc_wr_enable_threshold;
	struct ci_vbios_boot_state vbios_boot_state;
	/* smc offsets */
	uint32_t sram_end;
	uint32_t dpm_table_start;
	uint32_t soft_regs_start;
	uint32_t mc_reg_table_start;
	uint32_t fan_table_start;
	uint32_t arb_table_start;
	/* smc tables */
	SMU7_Discrete_DpmTable smc_state_table;
	SMU7_Discrete_MCRegisters smc_mc_reg_table;
	SMU7_Discrete_PmFuses smc_powertune_table;
	/* other stuff */
	struct ci_mc_reg_table mc_reg_table;
	struct atom_voltage_table vddc_voltage_table;
	struct atom_voltage_table vddci_voltage_table;
	struct atom_voltage_table mvdd_voltage_table;
	struct ci_ulv_parm ulv;
	uint32_t power_containment_features;
	const struct ci_pt_defaults *powertune_defaults;
	uint32_t dte_tj_offset;
	bool vddc_phase_shed_control;
	struct ci_thermal_temperature_setting thermal_temp_setting;
	struct ci_dpm_level_enable_mask dpm_level_enable_mask;
	uint32_t need_update_smu7_dpm_table;
	uint32_t sclk_dpm_key_disabled;
	uint32_t mclk_dpm_key_disabled;
	uint32_t pcie_dpm_key_disabled;
	uint32_t thermal_sclk_dpm_enabled;
	struct ci_pcie_perf_range pcie_gen_performance;
	struct ci_pcie_perf_range pcie_lane_performance;
	struct ci_pcie_perf_range pcie_gen_powersaving;
	struct ci_pcie_perf_range pcie_lane_powersaving;
	uint32_t activity_target[SMU7_MAX_LEVELS_GRAPHICS];
	uint32_t mclk_activity_target;
	uint32_t low_sclk_interrupt_t;
	uint32_t last_mclk_dpm_enable_mask;
	uint32_t sys_pcie_mask;
	/* caps */
	bool caps_power_containment;
	bool caps_cac;
	bool caps_sq_ramping;
	bool caps_db_ramping;
	bool caps_td_ramping;
	bool caps_tcp_ramping;
	bool caps_fps;
	bool caps_sclk_ds;
	bool caps_sclk_ss_support;
	bool caps_mclk_ss_support;
	bool caps_uvd_dpm;
	bool caps_vce_dpm;
	bool caps_samu_dpm;
	bool caps_acp_dpm;
	bool caps_automatic_dc_transition;
	bool caps_sclk_throttle_low_notification;
	bool caps_dynamic_ac_timing;
	bool caps_od_fuzzy_fan_control_support;
	/* flags */
	bool thermal_protection;
	bool pcie_performance_request;
	bool dynamic_ss;
	bool dll_default_on;
	bool cac_enabled;
	bool uvd_enabled;
	bool battery_state;
	bool pspp_notify_required;
	bool mem_gddr5;
	bool enable_bapm_feature;
	bool enable_tdc_limit_feature;
	bool enable_pkg_pwr_tracking_feature;
	bool use_pcie_performance_levels;
	bool use_pcie_powersaving_levels;
	bool uvd_power_gated;
	/* driver states */
	struct rdn_ci_rps current_rps;
	struct rdn_ci_rps requested_rps;
	/* fan control */
	bool fan_ctrl_is_in_default_mode;
	bool fan_is_controlled_by_smc;
	uint32_t t_min;
	uint32_t fan_ctrl_default_mode;
};

#define CISLANDS_VOLTAGE_CONTROL_NONE                   0x0
#define CISLANDS_VOLTAGE_CONTROL_BY_GPIO                0x1
#define CISLANDS_VOLTAGE_CONTROL_BY_SVID2               0x2

#define CISLANDS_Q88_FORMAT_CONVERSION_UNIT             256

#define CISLANDS_VRC_DFLT0                              0x3FFFC000
#define CISLANDS_VRC_DFLT1                              0x000400
#define CISLANDS_VRC_DFLT2                              0xC00080
#define CISLANDS_VRC_DFLT3                              0xC00200
#define CISLANDS_VRC_DFLT4                              0xC01680
#define CISLANDS_VRC_DFLT5                              0xC00033
#define CISLANDS_VRC_DFLT6                              0xC00033
#define CISLANDS_VRC_DFLT7                              0x3FFFC000

#define CISLANDS_CGULVPARAMETER_DFLT                    0x00040035
#define CISLAND_TARGETACTIVITY_DFLT                     30
#define CISLAND_MCLK_TARGETACTIVITY_DFLT                10

#define PCIE_PERF_REQ_REMOVE_REGISTRY   0
#define PCIE_PERF_REQ_FORCE_LOWPOWER    1
#define PCIE_PERF_REQ_PECI_GEN1         2
#define PCIE_PERF_REQ_PECI_GEN2         3
#define PCIE_PERF_REQ_PECI_GEN3         4

/* ---- radeon.h: rdev->pm.dpm.dyn_state and its neighbours ---- */

struct rdn_ci_clock_and_voltage_limits {
	uint32_t sclk;
	uint32_t mclk;
	uint16_t vddc;
	uint16_t vddci;
};

struct rdn_ci_dep_entry {
	uint32_t clk;
	uint16_t v;
};

/* radeon_clock_voltage_dependency_table; entries from os->alloc */
struct rdn_ci_dep_table {
	uint32_t count;
	struct rdn_ci_dep_entry *entries;
};

/*
 * radeon_cac_leakage_entry is a union in Linux: {vddc, leakage} over
 * {vddc1, vddc2, vddc3}. Both views are kept here; with
 * ATOM_PP_PLATFORM_CAP_EVV the parser also fills vddc/leakage with what the
 * union would read on a little-endian host (vddc = vddc1, leakage = vddc3:
 * the u32 is naturally aligned at offset 4, after padding, so it aliases
 * vddc3, not vddc2), because ci_get_std_voltage_value_sidd() reads them that
 * way whatever the platform caps say. Measured against Linux's SMU table
 * 2026-11-04.
 */
struct rdn_ci_cac_leakage_entry {
	uint16_t vddc;
	uint32_t leakage;
	uint16_t vddc1;
	uint16_t vddc2;
	uint16_t vddc3;
};

struct rdn_ci_cac_leakage_table {
	uint32_t count;
	struct rdn_ci_cac_leakage_entry *entries;
};

struct rdn_ci_phase_shedding_limits_entry {
	uint16_t voltage;
	uint32_t sclk;
	uint32_t mclk;
};

struct rdn_ci_phase_shedding_limits_table {
	uint32_t count;
	struct rdn_ci_phase_shedding_limits_entry *entries;
};

struct rdn_ci_uvd_dep_entry {
	uint32_t vclk;
	uint32_t dclk;
	uint16_t v;
};

struct rdn_ci_uvd_dep_table {
	uint8_t count;
	struct rdn_ci_uvd_dep_entry *entries;
};

struct rdn_ci_vce_dep_entry {
	uint32_t ecclk;
	uint32_t evclk;
	uint16_t v;
};

struct rdn_ci_vce_dep_table {
	uint8_t count;
	struct rdn_ci_vce_dep_entry *entries;
};

struct rdn_ci_ppm_table {
	uint8_t ppm_design;
	uint16_t cpu_core_number;
	uint32_t platform_tdp;
	uint32_t small_ac_platform_tdp;
	uint32_t platform_tdc;
	uint32_t small_ac_platform_tdc;
	uint32_t apu_tdp;
	uint32_t dgpu_tdp;
	uint32_t dgpu_ulv_power;
	uint32_t tj_max;
};

struct rdn_ci_cac_tdp_table {
	uint16_t tdp;
	uint16_t configurable_tdp;
	uint16_t tdc;
	uint16_t battery_power_limit;
	uint16_t small_power_limit;
	uint16_t low_cac_leakage;
	uint16_t high_cac_leakage;
	uint16_t maximum_power_delivery_limit;
};

struct rdn_ci_dyn_state {
	struct rdn_ci_dep_table vddc_dependency_on_sclk;
	struct rdn_ci_dep_table vddci_dependency_on_mclk;
	struct rdn_ci_dep_table vddc_dependency_on_mclk;
	struct rdn_ci_dep_table mvdd_dependency_on_mclk;
	struct rdn_ci_dep_table vddc_dependency_on_dispclk;
	struct rdn_ci_uvd_dep_table uvd_clock_voltage_dependency_table;
	struct rdn_ci_vce_dep_table vce_clock_voltage_dependency_table;
	struct rdn_ci_dep_table samu_clock_voltage_dependency_table;
	struct rdn_ci_dep_table acp_clock_voltage_dependency_table;
	struct rdn_ci_clock_and_voltage_limits max_clock_voltage_on_dc;
	struct rdn_ci_clock_and_voltage_limits max_clock_voltage_on_ac;
	uint32_t mclk_sclk_ratio;
	uint32_t sclk_mclk_delta;
	uint16_t vddc_vddci_delta;
	uint16_t min_vddc_for_pcie_gen2;
	struct rdn_ci_cac_leakage_table cac_leakage_table;
	struct rdn_ci_phase_shedding_limits_table phase_shedding_limits_table;
	bool has_ppm_table;			/* Linux: ppm_table != NULL */
	struct rdn_ci_ppm_table ppm_table;
	bool has_cac_tdp_table;			/* Linux: cac_tdp_table != NULL */
	struct rdn_ci_cac_tdp_table cac_tdp_table;
};

/* radeon.h's struct radeon_dpm_fan: ATOM_PPLIB_FANTABLE 1..3 */
struct rdn_ci_fan {
	uint16_t t_min;
	uint16_t t_med;
	uint16_t t_high;
	uint16_t pwm_min;
	uint16_t pwm_med;
	uint16_t pwm_high;
	uint8_t t_hyst;
	uint32_t cycle_delay;
	uint16_t t_max;
	uint8_t control_mode;
	uint16_t default_max_fan_pwm;
	uint16_t default_fan_output_sensitivity;
	uint16_t fan_output_sensitivity;
	bool ucode_fan_control;
	uint8_t table_format;		/* ucFanTableFormat, for rdn_ci_dpm_describe() */
};

#define RDN_CI_MAX_VCE_LEVELS 6

struct rdn_ci_vce_state {
	uint32_t evclk;
	uint32_t ecclk;
	uint32_t sclk;
	uint32_t mclk;
	uint8_t clk_idx;
	uint8_t pstate;
};

/*
 * The whole state: Linux's rdev->pm, rdev->pm.dpm and the ci_power_info
 * hanging off it. Large (the SMU tables are inside); allocate it once, not on
 * a stack. Zeroed and filled by rdn_ci_dpm_init(), released by
 * rdn_ci_dpm_fini().
 */
struct rdn_ci_dpm {
	struct rdn_card *card;

	/* rdev->pm */
	uint8_t thermal_controller_type;	/* ATOM_PP_THERMALCONTROLLER_*; non-zero is Linux's int_thermal_type != THERMAL_TYPE_NONE */
	bool no_fan;
	uint8_t fan_pulses_per_revolution;
	uint8_t fan_min_rpm;			/* hundreds */
	uint8_t fan_max_rpm;

	/* rdev->pm.dpm */
	struct rdn_ci_rps *ps;			/* num_ps states from os->alloc */
	unsigned num_ps;
	int boot_ps;				/* index into ps, or -1 */
	int uvd_ps;
	struct rdn_ci_vce_state vce_states[RDN_CI_MAX_VCE_LEVELS];
	uint32_t platform_caps;
	uint16_t backbias_response_time;
	uint16_t voltage_response_time;
	uint32_t tdp_limit;
	uint32_t near_tdp_limit;
	uint32_t near_tdp_limit_adjusted;
	uint32_t tdp_od_limit;
	bool power_control;
	int32_t tdp_adjustment;
	uint32_t sq_ramping_threshold;
	uint32_t cac_leakage;
	uint16_t load_line_slope;
	struct rdn_ci_dyn_state dyn_state;
	struct rdn_ci_fan fan;
	bool ac_power;				/* always true: a desktop card */
	unsigned new_active_crtc_count;
	unsigned current_active_crtc_count;
	enum rdn_ci_forced_level forced_level;
	/* rdev->pm.dpm.thermal */
	int thermal_min_temp;
	int thermal_max_temp;

	/* rdev->clock and rdev->mode_info, from FirmwareInfo */
	uint32_t reference_freq;		/* spll/mpll reference clock, 10 kHz */
	uint16_t firmware_flags;		/* usFirmwareCapability */

	/*
	 * What Linux reads from the DRM mode list (r600_dpm_get_vblank_time,
	 * r600_dpm_get_vrefresh): set by rdn_ci_dpm_display_configuration_changed().
	 * vblank_time_us 0xffffffff means the displays are off.
	 */
	uint32_t vblank_time_us;
	uint32_t vrefresh;

	/* rdev->pm.dpm.priv */
	struct ci_power_info pi;

	bool enabled;
	bool uploaded;				/* Firebrand: rdn_ci_dpm_upload ran (the tables are in the SRAM) */
	/*
	 * ni_copy_and_switch_arb_sets()'s closing switch request (MC_CG_CONFIG,
	 * MC_ARB_CG), which Linux makes and this port did not; false keeps the
	 * W8100-verified behaviour. Untested on a card (rdn_ci_arbreq=1).
	 */
	bool arb_switch_request;
	int last_power_state;			/* enum rdn_ci_power_state of the last request, -1 before */
};

/*
 * ci_dpm_init(): the private state, the VBIOS boot values, the platform
 * caps, r600_parse_extended_power_table(), ci_parse_power_table(), the
 * voltage control detection, the leakage voltages and the dependency tables
 * patched with them, the PCIe ranges, the GPIO pins, the thermal controller
 * and the fan table. sys_pcie_mask is RDN_CI_PCIE_SPEED_* for what the
 * host's root port supports (Linux asks the PCI core; 0 means unknown and
 * only forces Gen1 where a state's own PCIe gen is "default"). Returns 0 or
 * a negative errno; on failure nothing is left allocated.
 */
int rdn_ci_dpm_init(struct rdn_ci_dpm *dpm, struct rdn_card *card, unsigned sys_pcie_mask);
void rdn_ci_dpm_fini(struct rdn_ci_dpm *dpm);

/*
 * ci_dpm_setup_asic() less the MC microcode (the caller's
 * rdn_si_mc_load_microcode()) followed by ci_dpm_enable() in Linux's order
 * and ci_dpm_late_enable()'s temperature range. smc_fw is HAWAII_smc.bin or
 * BONAIRE_smc.bin (raw, 0x1FDEC bytes). Fails (-EINVAL) if the SMC is already
 * running, as Linux does. Returns 0 or a negative errno.
 */
int rdn_ci_dpm_enable(struct rdn_ci_dpm *dpm, const void *smc_fw, size_t smc_size);
/*
 * Firebrand: rdn_ci_dpm_enable in two halves (rdn_ci_dpm.c, ci_dpm_enable_upload):
 * the image and every table into the SMC's SRAM with the SMC held in reset,
 * then the SMC and DPM started.
 */
int rdn_ci_dpm_upload(struct rdn_ci_dpm *dpm, const void *smc_fw, size_t smc_size);
int rdn_ci_dpm_start(struct rdn_ci_dpm *dpm);

/* ci_dpm_disable(): everything off, the SMC in reset, arbiter set F0. */
int rdn_ci_dpm_disable(struct rdn_ci_dpm *dpm);

/*
 * radeon_dpm_change_power_state_locked() for one VBIOS state:
 * pick the state, ci_dpm_pre_set_power_state() (ci_apply_state_adjust_rules),
 * ci_dpm_set_power_state(), ci_dpm_post_set_power_state(). With all levels
 * left enabled the firmware then chooses the level by load (Linux's
 * default). Call it after rdn_ci_dpm_enable() and whenever the display
 * configuration changed. Returns 0 or a negative errno.
 */
int rdn_ci_dpm_set_power_state(struct rdn_ci_dpm *dpm, enum rdn_ci_power_state state);

/* ci_dpm_force_performance_level(): pin the lowest or highest enabled level, or let the firmware choose. */
int rdn_ci_dpm_force_performance_level(struct rdn_ci_dpm *dpm, enum rdn_ci_forced_level level);

/*
 * ci_dpm_display_configuration_changed(): the display gap, for num_displays
 * active CRTCs refreshing at vrefresh Hz with a vertical blank of
 * vblank_time_us microseconds (0xffffffff: displays off; 0: unknown, which
 * keeps the memory clock from switching, the safe side). Linux takes these
 * from the DRM mode list; here they are arguments.
 */
void rdn_ci_dpm_display_configuration_changed(struct rdn_ci_dpm *dpm, unsigned num_displays,
					      uint32_t vrefresh, uint32_t vblank_time_us);

/* ci_dpm_vblank_too_short(): whether the memory clock may switch in the blank (450 us for GDDR5, 300 otherwise; never above 120 Hz). */
bool rdn_ci_dpm_vblank_too_short(struct rdn_ci_dpm *dpm, uint32_t vrefresh);

/*
 * Readouts. The clocks are what the firmware reports as its average
 * (PPSMC_MSG_API_GetSclkFrequency / GetMclkFrequency, 10 kHz); when the
 * firmware does not answer they fall back to the requested state's clocks,
 * low or high level (ci_dpm_get_sclk / ci_dpm_get_mclk with low = false).
 */
uint32_t rdn_ci_dpm_get_sclk(struct rdn_ci_dpm *dpm);
uint32_t rdn_ci_dpm_get_mclk(struct rdn_ci_dpm *dpm);
/* ci_get_temp(): CG_MULT_THERMAL_STATUS, millidegrees Celsius. */
int rdn_ci_dpm_temperature(struct rdn_ci_dpm *dpm);

/* Fan control (ci_fan_ctrl_*). Percent is 0..100; mode 0 restores the firmware's control, FDO_PWM_MODE_STATIC (1) pins the duty. */
int rdn_ci_dpm_get_fan_speed_percent(struct rdn_ci_dpm *dpm, uint32_t *speed);
int rdn_ci_dpm_set_fan_speed_percent(struct rdn_ci_dpm *dpm, uint32_t speed);
void rdn_ci_dpm_set_fan_mode(struct rdn_ci_dpm *dpm, uint32_t mode);
uint32_t rdn_ci_dpm_get_fan_mode(struct rdn_ci_dpm *dpm);
int rdn_ci_dpm_get_fan_speed_rpm(struct rdn_ci_dpm *dpm, uint32_t *speed);

/* Log what rdn_ci_dpm_init() parsed, to compare with Linux's dmesg on the same card. */
void rdn_ci_dpm_describe(struct rdn_ci_dpm *dpm);

/* Un-powergate or powergate UVD through the SMU (ci_dpm_powergate_uvd). */
void rdn_ci_dpm_powergate_uvd(struct rdn_ci_dpm *dpm, bool gate);

#endif /* RDN_CI_DPM_H */
