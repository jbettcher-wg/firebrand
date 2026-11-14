/*
 * pm/powerplay/amd_powerplay.c (Linux 7.1.1): the pp_smu IP block's entry
 * points as Firebrand calls them (frb_pm.h), and amdgpu_dpm.c's
 * amdgpu_dpm_compute_clocks / amdgpu_dpm_get_sclk/mclk powerplay arms.
 *
 * WHERE THE DPM INIT RUNS. amdgpu's vi_set_ip_blocks puts pp_smu after
 * gfx_v8_0 and sdma_v3_1 for Polaris: the SMU start (firmware load) runs
 * early, in amdgpu_device_fw_loading, but the SMC block's hw_init (this
 * file's frb_pm_hw_init) runs in hw_init phase 2 after GFX and SDMA, and the
 * E9173 capture has it there (DisableFFC at 2105.452985, after SDMA's last
 * write at .452892). Firebrand runs it at the same place, after stage sdma,
 * not inside stage smu: smu7_enable_didt_config enters RLC safe mode, which
 * amdgpu_gfx_rlc_enter_safe_mode skips while the RLC is not yet running
 * (before stage gfx it would take a different path than the capture's), and
 * the replay then checks it against the capture's own DPM window in order.
 * pp_late_init (AMD_PP_TASK_COMPLETE_INIT) follows at once: of the DM, UVD
 * and VCE blocks Linux runs between them, Firebrand has its display (stage
 * dce, before; frb_pm_hw_init tells powerplay about it after) and UVD (stage
 * uvd, after both: its ring test's UVDDPM_Enable then follows COMPLETE_INIT
 * instead of preceding it, which nothing in either depends on;
 * frb_pm_enable_uvd, frb_smu7_clockpowergating.c).
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
#include "frb_smu7_hwmgr.h"
#include "frb_polaris10_smumgr.h"
#include "include/pp/smu7_ppsmc.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"
#include "include/asic_reg/smu/smu_7_1_3_sh_mask.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"

struct frb_os *frb_pp_log_os;

/* adev->pm and adev->powerplay, the parts Firebrand keeps */
struct frb_pm {
	struct pp_hwmgr hwmgr;
	struct amd_pp_display_configuration pm_display_cfg;
	int dpm_enabled;
	int uvd_pwr_state;	/* adev->pm.pwr_state[AMD_IP_BLOCK_TYPE_UVD] */
};

/* enum ip_power_state */
enum { POWER_STATE_UNKNOWN, POWER_STATE_ON, POWER_STATE_OFF };

/*
 * amdgpu_device_get_pcie_info (amdgpu_device.c): the CAIL masks from the
 * GPU's and the upstream port's Link Capabilities (not a root-bus device,
 * not passthrough, no amdgpu_pcie_gen_cap/lane_cap overrides).
 */
static void amdgpu_device_get_pcie_info(struct frb_device *adev)
{
	uint32_t speed_cap = adev->pcie_caps.asic_speed, platform_speed_cap = adev->pcie_caps.platform_speed;
	uint32_t link_width = adev->pcie_caps.asic_width, platform_link_width = adev->pcie_caps.platform_width;

	adev->pcie_gen_mask = 0;
	adev->pcie_mlw_mask = 0;

	/* asic caps (LnkCap speed encoding: 1 2.5, 2 5, 3 8, 4 16, 5 32 GT/s) */
	if (speed_cap == 0) {
		adev->pcie_gen_mask |= (CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1 |
					CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2 |
					CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3);
	} else {
		if (speed_cap >= 5)
			adev->pcie_gen_mask |= CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN5;
		if (speed_cap >= 4)
			adev->pcie_gen_mask |= CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN4;
		if (speed_cap >= 3)
			adev->pcie_gen_mask |= CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN3;
		if (speed_cap >= 2)
			adev->pcie_gen_mask |= CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN2;
		adev->pcie_gen_mask |= CAIL_ASIC_PCIE_LINK_SPEED_SUPPORT_GEN1;
	}
	/* platform caps */
	if (platform_speed_cap == 0) {
		adev->pcie_gen_mask |= (CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1 |
					CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2);
	} else {
		if (platform_speed_cap >= 5)
			adev->pcie_gen_mask |= CAIL_PCIE_LINK_SPEED_SUPPORT_GEN5;
		if (platform_speed_cap >= 4)
			adev->pcie_gen_mask |= CAIL_PCIE_LINK_SPEED_SUPPORT_GEN4;
		if (platform_speed_cap >= 3)
			adev->pcie_gen_mask |= CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3;
		if (platform_speed_cap >= 2)
			adev->pcie_gen_mask |= CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2;
		adev->pcie_gen_mask |= CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1;
	}

	/* asic caps: every width up to the cap (x12 only from x12 up, as Linux) */
	if (link_width == 0) {
		adev->pcie_mlw_mask |= AMDGPU_DEFAULT_ASIC_PCIE_MLW_MASK;
	} else {
		if (link_width >= 32)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X32;
		if (link_width >= 16)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X16;
		if (link_width >= 12)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X12;
		if (link_width >= 8)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X8;
		if (link_width >= 4)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X4;
		if (link_width >= 2)
			adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X2;
		adev->pcie_mlw_mask |= CAIL_ASIC_PCIE_LINK_WIDTH_SUPPORT_X1;
	}
	/* platform caps */
	if (platform_link_width == 0) {
		adev->pcie_mlw_mask |= AMDGPU_DEFAULT_PCIE_MLW_MASK;
	} else {
		if (platform_link_width >= 32)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X32;
		if (platform_link_width >= 16)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X16;
		if (platform_link_width >= 12)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X12;
		if (platform_link_width >= 8)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X8;
		if (platform_link_width >= 4)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X4;
		if (platform_link_width >= 2)
			adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X2;
		adev->pcie_mlw_mask |= CAIL_PCIE_LINK_WIDTH_SUPPORT_X1;
	}
}

/* amd_powerplay_create + pp_early_init + pp_sw_init + pp_hw_init + pp_late_init */
int frb_pm_hw_init(struct frb_device *adev)
{
	struct frb_pm *pm;
	struct pp_hwmgr *hwmgr;
	int ret;

	if (adev->pm)
		return 0;
	if (adev->stage_done < FRB_STAGE_SDMA)
		return -EPERM;
	frb_pp_log_os = adev->os;

	pm = adev->os->alloc(adev->os->cookie, sizeof(*pm));
	if (!pm)
		return -ENOMEM;
	adev->pm = pm;
	hwmgr = &pm->hwmgr;

	amdgpu_device_get_pcie_info(adev);
	frb_info(adev, "dpm: PCIe gen mask %08x, width mask %08x (GPU %u/x%u, upstream %u/x%u)",
		 adev->pcie_gen_mask, adev->pcie_mlw_mask, adev->pcie_caps.asic_speed, adev->pcie_caps.asic_width,
		 adev->pcie_caps.platform_speed, adev->pcie_caps.platform_width);

	/* amd_powerplay_create */
	hwmgr->adev = adev;
	hwmgr->not_vf = true;
	hwmgr->device = adev;
	hwmgr->chip_family = AMDGPU_FAMILY_VI;
	hwmgr->chip_id = adev->asic_type;
	hwmgr->feature_mask = adev->pp_feature ? adev->pp_feature : FRB_PP_FEATURE_MASK_DEFAULT;
	hwmgr->display_config = &pm->pm_display_cfg;
	/* smu7_upload_smu_firmware_image: amdgpu_cgs_get_firmware_info's is_kicker */
	hwmgr->is_kicker = ASICID_IS_P23(adev->device, adev->revision);
	/* amdgpu_dpm_get_display_cfg before any display: vblank time "max" */
	pm->pm_display_cfg.min_vblank_time = 0xffffffff;

	ret = hwmgr_early_init(hwmgr);
	if (ret) {
		frb_err(adev, "dpm: hwmgr_early_init: %d", ret);
		goto fail;
	}
	ret = hwmgr_sw_init(hwmgr);
	if (ret) {
		frb_err(adev, "dpm: powerplay sw init failed (%d)", ret);
		goto fail;
	}
	ret = hwmgr_hw_init(hwmgr);
	if (ret) {
		pr_err("powerplay hw init failed\n");
		hwmgr_sw_fini(hwmgr);
		goto fail;
	}
	if (!hwmgr->pm_en) {
		ret = -ENODEV;
		hwmgr_sw_fini(hwmgr);
		goto fail;
	}
	pm->dpm_enabled = 1;	/* adev->pm.dpm_enabled */
	pm->uvd_pwr_state = POWER_STATE_UNKNOWN;	/* amdgpu_device_init */

	/* pp_late_init */
	if (hwmgr->pm_en)
		hwmgr_handle_task(hwmgr, AMD_PP_TASK_COMPLETE_INIT, NULL);

	frb_info(adev, "dpm: enabled: %u SCLK levels to %u MHz, %u MCLK levels to %u MHz, %u PCIe levels",
		 ((struct smu7_hwmgr *)hwmgr->backend)->dpm_table.sclk_table.count,
		 frb_pm_get_sclk(adev, 0) / 100,
		 ((struct smu7_hwmgr *)hwmgr->backend)->dpm_table.mclk_table.count,
		 frb_pm_get_mclk(adev, 0) / 100,
		 ((struct smu7_hwmgr *)hwmgr->backend)->dpm_table.pcie_speed_table.count);
	return 0;
fail:
	adev->os->free(adev->os->cookie, pm);
	adev->pm = NULL;
	return ret;
}

int frb_pm_enabled(struct frb_device *adev)
{
	return adev->pm && adev->pm->dpm_enabled;
}

/* pp_dpm_powergate_uvd */
static void pp_dpm_powergate_uvd(struct pp_hwmgr *hwmgr, bool gate)
{
	if (!hwmgr || !hwmgr->pm_en)
		return;

	if (hwmgr->hwmgr_func->powergate_uvd == NULL) {
		pr_info("%s was not implemented.\n", __func__);
		return;
	}
	hwmgr->hwmgr_func->powergate_uvd(hwmgr, gate);
}

/*
 * amdgpu_dpm_enable_uvd (not SI): amdgpu_dpm_set_powergating_by_smu with
 * its "already in the target state" check, then pp_set_powergating_by_smu's
 * UVD arm. Returns 0, or -ENODEV without DPM.
 */
int frb_pm_enable_uvd(struct frb_device *adev, int enable)
{
	struct frb_pm *pm = adev->pm;
	int pwr_state = enable ? POWER_STATE_ON : POWER_STATE_OFF;

	if (!pm || !pm->dpm_enabled)
		return -ENODEV;
	if (pm->uvd_pwr_state == pwr_state)
		return 0;
	pp_dpm_powergate_uvd(&pm->hwmgr, !enable);
	pm->uvd_pwr_state = pwr_state;
	return 0;
}

/* pp_hw_fini + pp_sw_fini + pp_late_fini (amd_powerplay_destroy) */
void frb_pm_fini(struct frb_device *adev)
{
	struct frb_pm *pm = adev->pm;

	if (!pm)
		return;
	if (pm->dpm_enabled) {
		hwmgr_hw_fini(&pm->hwmgr);
		pm->dpm_enabled = 0;
	}
	hwmgr_sw_fini(&pm->hwmgr);
	adev->os->free(adev->os->cookie, pm);
	adev->pm = NULL;
	frb_info(adev, "dpm: off (boot state, DPM disabled)");
}

/*
 * amdgpu_dpm_compute_clocks's powerplay arm, pp_pm_compute_clocks, with the
 * legacy display path's configuration (amdgpu_dpm_get_display_cfg, which
 * the display code fills: frb_display_get_pm_cfg). The caller has run the
 * bandwidth update and has no work in flight on the rings.
 */
void frb_pm_compute_clocks(struct frb_device *adev, const struct amd_pp_display_configuration *cfg)
{
	struct frb_pm *pm = adev->pm;

	if (!pm || !pm->dpm_enabled)
		return;
	pm->pm_display_cfg = *cfg;
	/* pp_display_configuration_change */
	phm_store_dal_configuration_data(&pm->hwmgr, &pm->pm_display_cfg);
	/* pp_dpm_dispatch_tasks */
	hwmgr_handle_task(&pm->hwmgr, AMD_PP_TASK_DISPLAY_CONFIG_CHANGE, NULL);
}

/* pp_dpm_en_umd_pstate */
static void pp_dpm_en_umd_pstate(struct pp_hwmgr  *hwmgr,
						enum amd_dpm_forced_level *level)
{
	uint32_t profile_mode_mask = AMD_DPM_FORCED_LEVEL_PROFILE_STANDARD |
					AMD_DPM_FORCED_LEVEL_PROFILE_MIN_SCLK |
					AMD_DPM_FORCED_LEVEL_PROFILE_MIN_MCLK |
					AMD_DPM_FORCED_LEVEL_PROFILE_PEAK;

	if (!(hwmgr->dpm_level & profile_mode_mask)) {
		/* enter umd pstate, save current level, disable gfx cg*/
		if (*level & profile_mode_mask) {
			hwmgr->saved_dpm_level = hwmgr->dpm_level;
			hwmgr->en_umd_pstate = true;
		}
	} else {
		/* exit umd pstate, restore level, enable gfx cg*/
		if (!(*level & profile_mode_mask)) {
			if (*level == AMD_DPM_FORCED_LEVEL_PROFILE_EXIT)
				*level = hwmgr->saved_dpm_level;
			hwmgr->en_umd_pstate = false;
		}
	}
}

/* pp_dpm_force_performance_level (power_dpm_force_performance_level) */
int frb_pm_force_performance_level(struct frb_device *adev, enum amd_dpm_forced_level level)
{
	struct pp_hwmgr *hwmgr;

	if (!adev->pm || !adev->pm->dpm_enabled)
		return -ENODEV;
	hwmgr = &adev->pm->hwmgr;
	if (!hwmgr->pm_en)
		return -EINVAL;
	if (level != AMD_DPM_FORCED_LEVEL_AUTO && level != AMD_DPM_FORCED_LEVEL_LOW &&
	    level != AMD_DPM_FORCED_LEVEL_HIGH)
		return -EINVAL;	/* the profile levels are not wired to frbctl */

	if (level == hwmgr->dpm_level)
		return 0;

	pp_dpm_en_umd_pstate(hwmgr, &level);
	hwmgr->request_dpm_level = level;
	hwmgr_handle_task(hwmgr, AMD_PP_TASK_READJUST_POWER_STATE, NULL);

	return 0;
}

/* pp_dpm_get_sclk / pp_dpm_get_mclk: the requested state's levels, 10 kHz */
uint32_t frb_pm_get_sclk(struct frb_device *adev, int low)
{
	struct pp_hwmgr *hwmgr;

	if (!adev->pm || !adev->pm->dpm_enabled)
		return 0;
	hwmgr = &adev->pm->hwmgr;
	if (hwmgr->hwmgr_func->get_sclk == NULL)
		return 0;
	return hwmgr->hwmgr_func->get_sclk(hwmgr, low);
}

uint32_t frb_pm_get_mclk(struct frb_device *adev, int low)
{
	struct pp_hwmgr *hwmgr;

	if (!adev->pm || !adev->pm->dpm_enabled)
		return 0;
	hwmgr = &adev->pm->hwmgr;
	if (hwmgr->hwmgr_func->get_mclk == NULL)
		return 0;
	return hwmgr->hwmgr_func->get_mclk(hwmgr, low);
}

/* the level of a table whose value is clk, or -1 */
static int32_t level_of(const struct smu7_single_dpm_table *t, uint32_t clk)
{
	uint32_t i;

	for (i = 0; i < t->count; i++)
		if (t->dpm_levels[i].value == clk)
			return (int32_t)i;
	return -1;
}

/*
 * frbctl dpm: amdgpu's sensors (smu7_read_sensor: GFX_SCLK, GFX_MCLK,
 * GPU_LOAD, MEM_LOAD, GPU_TEMP, VDDGFX), the fan (hwmon fan1_input, pwm1),
 * the package power (PPSMC_MSG_GetCurrPkgPwr, as hwmon power1_input; the
 * slow PmStatusLog fallback is not used here), the DPM masks and the link.
 */
int frb_pm_get_status(struct frb_device *adev, struct frb_pm_status *st)
{
	struct pp_hwmgr *hwmgr;
	struct smu7_hwmgr *data;
	uint32_t v, i;
	int size;

	memset(st, 0, sizeof(*st));
	if (!adev->pm || !adev->pm->dpm_enabled)
		return -ENODEV;
	hwmgr = &adev->pm->hwmgr;
	data = (struct smu7_hwmgr *)hwmgr->backend;

	st->enabled = 1;
	st->forced_level = hwmgr->dpm_level;
	st->sclk_count = data->dpm_table.sclk_table.count;
	st->mclk_count = data->dpm_table.mclk_table.count;
	st->pcie_count = data->dpm_table.pcie_speed_table.count;
	for (i = 0; i < st->sclk_count && i < 8; i++)
		st->sclk_levels[i] = data->dpm_table.sclk_table.dpm_levels[i].value;
	for (i = 0; i < st->mclk_count && i < 8; i++)
		st->mclk_levels[i] = data->dpm_table.mclk_table.dpm_levels[i].value;
	st->sclk_mask = data->dpm_level_enable_mask.sclk_dpm_enable_mask;
	st->mclk_mask = data->dpm_level_enable_mask.mclk_dpm_enable_mask;
	st->pcie_mask = data->dpm_level_enable_mask.pcie_dpm_enable_mask;

	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_GFX_SCLK, &v, &size))
		st->cur_sclk = v;
	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_GFX_MCLK, &v, &size))
		st->cur_mclk = v;
	st->cur_sclk_index = level_of(&data->dpm_table.sclk_table, st->cur_sclk);
	st->cur_mclk_index = level_of(&data->dpm_table.mclk_table, st->cur_mclk);
	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_VDDGFX, &v, &size))
		st->vddgfx_mv = v;
	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_GPU_TEMP, &v, &size))
		st->temp_mc = (int32_t)v;
	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_GPU_LOAD, &v, &size))
		st->gpu_load = v;
	size = 4;
	if (!hwmgr->hwmgr_func->read_sensor(hwmgr, AMDGPU_PP_SENSOR_MEM_LOAD, &v, &size))
		st->mem_load = v;
	if (hwmgr->hwmgr_func->get_fan_speed_rpm && !hwmgr->hwmgr_func->get_fan_speed_rpm(hwmgr, &v))
		st->fan_rpm = v;
	if (hwmgr->hwmgr_func->get_fan_speed_pwm && !hwmgr->hwmgr_func->get_fan_speed_pwm(hwmgr, &v))
		st->fan_pwm = v;
	v = 0;
	smum_send_msg_to_smc_with_parameter(hwmgr, PPSMC_MSG_GetCurrPkgPwr, 0, &v);
	st->power_mw = (uint32_t)(((uint64_t)v * 1000) >> 8);	/* 8.8 W */
	st->pcie_gen = PHM_GET_FIELD(cgs_read_ind_register(adev, CGS_IND_REG__PCIE, ixPCIE_LC_SPEED_CNTL),
				     PCIE_LC_SPEED_CNTL, LC_CURRENT_DATA_RATE) + 1;
	st->pcie_lanes = decode_pcie_lane_width(PHM_GET_FIELD(cgs_read_ind_register(adev, CGS_IND_REG__PCIE,
				ixPCIE_LC_LINK_WIDTH_CNTL), PCIE_LC_LINK_WIDTH_CNTL, LC_LINK_WIDTH_RD));
	st->smc_feature_status = cgs_read_ind_register(adev, CGS_IND_REG__SMC, ixFEATURE_STATUS);
	st->num_display = adev->pm->pm_display_cfg.num_display;
	st->vrefresh = adev->pm->pm_display_cfg.vrefresh;
	st->min_vblank_time = adev->pm->pm_display_cfg.min_vblank_time;
	return 0;
}
