/*
 * Dynamic power management (powerplay, the SMC's DPM) as the rest of
 * Firebrand sees it: what amdgpu's amd_pm_funcs and amdgpu_dpm.c offer the
 * other IP blocks, cut to Polaris 12 and the calls Firebrand makes.
 *
 * Switched on by the personality key FirebrandDPM or boot-arg frb_dpm=1
 * (adev->want_dpm); off by default. With it, frb_device_init runs the SMC
 * block's hw_init (hwmgr_hw_init) and late_init (AMD_PP_TASK_COMPLETE_INIT)
 * after stage sdma, at the place amdgpu's vi_set_ip_blocks order puts the
 * pp_smu block: after gfx_v8_0 and sdma_v3_1 (frb_amd_powerplay.c says why
 * that and not right after the SMU start).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_PM_H
#define FRB_PM_H

#include "frb_device.h"

/* include/kgd_pp_interface.h */
enum amd_dpm_forced_level {
	AMD_DPM_FORCED_LEVEL_AUTO = 0x1,
	AMD_DPM_FORCED_LEVEL_MANUAL = 0x2,
	AMD_DPM_FORCED_LEVEL_LOW = 0x4,
	AMD_DPM_FORCED_LEVEL_HIGH = 0x8,
	AMD_DPM_FORCED_LEVEL_PROFILE_STANDARD = 0x10,
	AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK = 0x20,
	AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK = 0x40,
	AMD_DPM_FORCED_LEVEL_PROFILE_PEAK = 0x80,
	AMD_DPM_FORCED_LEVEL_PROFILE_EXIT = 0x100,
	AMD_DPM_FORCED_LEVEL_PERF_DETERMINISM = 0x200,
};

/*
 * include/dm_pp_interface.h, the members the smu7 hardware manager reads.
 * Its bools are uint8_t here: the structure is embedded in frb_display,
 * which the kext's C++ reads too, and C's and C++'s bool need not agree.
 */
#define MAX_NUM_DISPLAY 32
struct single_display_configuration {
	uint32_t controller_index;
	uint32_t controller_id;
	uint32_t signal_type;
	uint32_t display_state;
	uint32_t pixel_clock;
};

struct amd_pp_display_configuration {
	uint8_t nb_pstate_switch_disable;
	uint8_t cpu_cc6_disable;
	uint8_t cpu_pstate_disable;
	uint32_t cpu_pstate_separation_time;
	uint32_t num_display;
	uint32_t num_path_including_non_display;
	uint32_t crossfire_display_index;
	uint32_t min_mem_set_clock;
	uint32_t min_core_set_clock;
	uint32_t min_bus_bandwidth;
	uint32_t min_core_set_clock_in_sr;
	struct single_display_configuration displays[MAX_NUM_DISPLAY];
	uint32_t vrefresh;
	uint32_t min_vblank_time;
	uint8_t multi_monitor_in_sync;
	uint32_t crtc_index;
	uint32_t line_time_in_us;
	uint8_t invalid_vblank_time;
	uint32_t display_clk;
	uint32_t dce_tolerable_mclk_in_active_latency;
	uint32_t min_dcef_set_clk;
	uint32_t min_dcef_deep_sleep_set_clk;
};

/* frbctl dpm: one readout of the SMC's state */
struct frb_pm_status {
	int enabled;			/* hw_init and late_init ran */
	int forced_level;		/* enum amd_dpm_forced_level in force */
	uint32_t sclk_count, mclk_count, pcie_count;
	uint32_t sclk_levels[8], mclk_levels[8];	/* 10 kHz */
	uint32_t sclk_mask, mclk_mask, pcie_mask;	/* enabled levels */
	uint32_t cur_sclk, cur_mclk;	/* PPSMC_MSG_API_GetSclkFrequency/Mclk, 10 kHz */
	int32_t cur_sclk_index, cur_mclk_index;	/* the level those match, -1 none */
	uint32_t vddgfx_mv;		/* PWR_SVI2_STATUS plane VID */
	int32_t temp_mc;		/* CG_MULT_THERMAL_STATUS, millidegrees */
	uint32_t fan_rpm, fan_pwm;	/* CG_TACH_STATUS; FDO duty 0-255 */
	uint32_t power_mw;		/* PPSMC_MSG_GetCurrPkgPwr (8.8 W), 0 = none */
	uint32_t gpu_load, mem_load;	/* AverageGraphics/MemoryActivity, % */
	uint32_t pcie_gen, pcie_lanes;	/* PCIE_LC_SPEED_CNTL / LINK_WIDTH */
	uint32_t smc_feature_status;	/* ixFEATURE_STATUS */
	uint32_t num_display, vrefresh, min_vblank_time;	/* the display config last stored */
};

/* frb_amd_powerplay.c */
int frb_pm_hw_init(struct frb_device *adev);	/* pp_early/sw/hw_init, then pp_late_init */
void frb_pm_fini(struct frb_device *adev);	/* frees the hardware manager; the SMC keeps running */
int frb_pm_enabled(struct frb_device *adev);	/* adev->pm.dpm_enabled */
/* amdgpu_dpm_enable_uvd: UVD's DPM levels on (and the block started) or off (stopped, gated) */
int frb_pm_enable_uvd(struct frb_device *adev, int enable);
/* amdgpu_dpm_compute_clocks's powerplay arm: the display configuration changed */
void frb_pm_compute_clocks(struct frb_device *adev, const struct amd_pp_display_configuration *cfg);
int frb_pm_force_performance_level(struct frb_device *adev, enum amd_dpm_forced_level level);
int frb_pm_get_status(struct frb_device *adev, struct frb_pm_status *st);
/* amdgpu_dpm_get_sclk/mclk: the requested state's lowest/highest level, 10 kHz (0: no DPM) */
uint32_t frb_pm_get_sclk(struct frb_device *adev, int low);
uint32_t frb_pm_get_mclk(struct frb_device *adev, int low);

#endif /* FRB_PM_H */
