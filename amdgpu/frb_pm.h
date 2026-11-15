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
	/* SOC15 (Vega 20, frb_vega20_get_status: vega20_read_sensor's sources) */
	uint32_t cur_socclk, cur_fclk;	/* PPSMC_MSG_GetDpmClockFreq, 10 kHz */
	int32_t temp_edge, temp_hotspot, temp_hbm;	/* SmuMetrics_t, millidegrees */
	uint32_t gfx_count, uclk_count, soc_count;	/* the DPM ladders */
	uint32_t gfx_levels[16], uclk_levels[16], soc_levels[16];	/* MHz */
	uint32_t smu_version;
	uint32_t gating;		/* bit 0: FirebrandGating; bit 1: the blocks are gated now */
	uint64_t cg_flags;
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
/*
 * SOC15 (Vega 20): the same readout from the SMU11 (frb_vega20_hwmgr.c):
 * SmuMetrics_t (TransferTableSmu2Dram: average GFXCLK, activity, socket
 * power, edge/hotspot/HBM temperatures), GetDpmClockFreq for UCLK, SOCCLK
 * and FCLK, THM's CTF temperature, SVI2 plane 0's VID, the DPM ladders.
 */
int frb_vega20_get_status(struct frb_device *adev, struct frb_pm_status *st);
/*
 * FCLK's soft minimum by hand (frbctl dpm fclk low|top). A display
 * configuration puts it at the top (vega20_set_fclk_to_highest_dpm_level in
 * the pre-display task) and nothing brings it down again, in Linux as here:
 * vega20_apply_clocks_adjust_rules works out the lowest level for it unless
 * the memory clock may not switch, but the DISPLAY_CONFIG_CHANGE task's
 * force_dpm_level(AUTO) uploads only GFXCLK, UCLK and SOCCLK (the capture:
 * SetSoftMinByFreq 0x000a049c after each NumOfDisplays, never a lower one).
 * So with a monitor FCLK idles at 1180 MHz, without one at 550.
 */
int frb_vega20_set_fclk_soft_min(struct frb_device *adev, int top);
/*
 * The sensors a monitor polls (FirebrandCore's status call: frbctl report,
 * Firebrand Monitor): a family-neutral sample in which a field counts only
 * when its FRB_PMS_* bit is set. Cheap reads only, nothing that wakes a gated
 * block, and no SMU message beyond what each family needs for its numbers:
 *   VI (Polaris)  amdgpu's smu7 sensors (GetSclkFrequency, GetMclkFrequency,
 *                 GetCurrPkgPwr; the activity soft registers, CTF, SVI2 and
 *                 FEATURE_STATUS through SMC indirect reads), the fan only
 *                 when the card has one; with DPM on.
 *   SI            CG_MULT_THERMAL_STATUS (no DPM needed); with SI DPM on,
 *                 the current level's clocks and VDDC (register reads).
 *   Vega 20       SmuMetrics_t (one TransferTableSmu2Dram: three messages):
 *                 every current clock, activity, socket power, edge/hotspot/
 *                 HBM temperatures, ThrottlerStatus; VDDGFX from SVI2 and the
 *                 hotspot from THM without the SMU; the feature mask as the
 *                 driver last set it (no message).
 * The caller holds the lock that serializes the driver's SMU traffic and
 * keeps the rate down (FirebrandCore samples at most every 500 ms).
 * 0; -ENODEV when the family or its state has nothing to read (the bits that
 * could be read are set); else the read's error.
 */
enum {
	FRB_PMS_CLOCKS		= 1u << 0,	/* gfx_mhz, mem_mhz */
	FRB_PMS_SOC_CLOCKS	= 1u << 1,	/* soc_mhz, fclk_mhz */
	FRB_PMS_LEVELS		= 1u << 2,	/* gfx_levels, mem_levels */
	FRB_PMS_TEMP		= 1u << 3,	/* temp_mc */
	FRB_PMS_TEMP_HOTSPOT	= 1u << 4,
	FRB_PMS_TEMP_MEM	= 1u << 5,
	FRB_PMS_POWER		= 1u << 6,
	FRB_PMS_POWER_LIMIT	= 1u << 7,
	FRB_PMS_LOAD		= 1u << 8,
	FRB_PMS_VDDGFX		= 1u << 9,
	FRB_PMS_FAN		= 1u << 10,
	FRB_PMS_THROTTLE	= 1u << 11,
	FRB_PMS_SMU		= 1u << 12	/* smu_version, features */
};
#define FRB_PMS_FEATURES_SMU7	7	/* features: ixFEATURE_STATUS (FEATURE_STATUS__*_MASK) */
#define FRB_PMS_FEATURES_SMU11	11	/* features: the SMU11 feature bits (FEATURE_*_BIT, GNLD_*) */
struct frb_pm_sensors {
	uint32_t valid;				/* FRB_PMS_* */
	int dpm_enabled;
	uint32_t gfx_mhz, mem_mhz, soc_mhz, fclk_mhz;
	int32_t temp_mc, temp_hotspot_mc, temp_mem_mc;	/* millidegrees */
	uint32_t power_mw, power_limit_w;
	uint32_t gpu_load, mem_load;		/* % */
	uint32_t vddgfx_mv;
	uint32_t fan_rpm, fan_pwm;		/* pwm 0-255 */
	uint32_t throttler;			/* SmuMetrics_t ThrottlerStatus */
	uint32_t smu_version;
	uint32_t features_kind;			/* FRB_PMS_FEATURES_*, 0 none */
	uint64_t features;
	uint32_t gfx_count, mem_count;
	uint32_t gfx_levels[16], mem_levels[16];	/* MHz, lowest first */
};
int frb_pm_read_sensors(struct frb_device *adev, struct frb_pm_sensors *s);
/* DPM runs: powerplay's (VI), the SMU11 hwmgr's (Vega 20), si_dpm's (SI) */
int frb_pm_dpm_running(struct frb_device *adev);
/* the Vega 20 arms (frb_vega20_hwmgr.c) */
int frb_vega20_read_sensors(struct frb_device *adev, struct frb_pm_sensors *s);
int frb_vega20_dpm_running(struct frb_device *adev);

/* amdgpu_dpm_get_sclk/mclk: the requested state's lowest/highest level, 10 kHz (0: no DPM) */
uint32_t frb_pm_get_sclk(struct frb_device *adev, int low);
uint32_t frb_pm_get_mclk(struct frb_device *adev, int low);

/* SI: radeon's si_dpm through RadeonNI's verified port (frb_si_dpm.c, si_dpm/) */
int frb_si_dpm_hw_init(struct frb_device *adev);
int frb_si_dpm_enabled(struct frb_device *adev);
void frb_si_dpm_fini(struct frb_device *adev);
void frb_si_dpm_compute_clocks(struct frb_device *adev, const struct amd_pp_display_configuration *cfg);
int frb_si_dpm_force_performance_level(struct frb_device *adev, enum amd_dpm_forced_level level);
uint32_t frb_si_dpm_get_sclk(struct frb_device *adev, int low);
uint32_t frb_si_dpm_get_mclk(struct frb_device *adev, int low);
int frb_si_dpm_get_status(struct frb_device *adev, struct frb_pm_status *st);

#endif /* FRB_PM_H */
