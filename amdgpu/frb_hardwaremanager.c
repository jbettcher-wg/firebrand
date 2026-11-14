/*
 * pm/powerplay/hwmgr/hardwaremanager.c (Linux 7.1.1): the phm_* dispatch to
 * the hardware manager's function table. The entries Polaris uses, unchanged;
 * phm_start_thermal_controller's amdgpu thermal limits go to Firebrand's
 * frb_pm instead of adev->pm.dpm.thermal.
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
#include "frb_hwmgr.h"

#define TEMP_RANGE_MIN (0)
#define TEMP_RANGE_MAX (80 * 1000)

#define PHM_FUNC_CHECK(hw) \
	do {							\
		if ((hw) == NULL || (hw)->hwmgr_func == NULL)	\
			return -EINVAL;				\
	} while (0)

int phm_setup_asic(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	if (NULL != hwmgr->hwmgr_func->asic_setup)
		return hwmgr->hwmgr_func->asic_setup(hwmgr);

	return 0;
}

int phm_set_power_state(struct pp_hwmgr *hwmgr,
		    const struct pp_hw_power_state *pcurrent_state,
		    const struct pp_hw_power_state *pnew_power_state)
{
	struct phm_set_power_state_input states;

	PHM_FUNC_CHECK(hwmgr);

	states.pcurrent_state = pcurrent_state;
	states.pnew_state = pnew_power_state;

	if (NULL != hwmgr->hwmgr_func->power_state_set)
		return hwmgr->hwmgr_func->power_state_set(hwmgr, &states);

	return 0;
}

/*
 * The suspend/resume skip ("dpm has been enabled") needs adev->in_suspend,
 * which Firebrand has not: smum_is_dpm_running is still read first, as
 * Linux's && evaluates it first (the trace has its FEATURE_STATUS read).
 */
int phm_enable_dynamic_state_management(struct pp_hwmgr *hwmgr)
{
	int ret = -EINVAL;

	PHM_FUNC_CHECK(hwmgr);

	if (!hwmgr->pp_one_vf && smum_is_dpm_running(hwmgr)
	    && 0 /* adev->in_suspend */) {
		pr_info("dpm has been enabled\n");
		return 0;
	}

	if (NULL != hwmgr->hwmgr_func->dynamic_state_management_enable)
		ret = hwmgr->hwmgr_func->dynamic_state_management_enable(hwmgr);

	return ret;
}

int phm_disable_dynamic_state_management(struct pp_hwmgr *hwmgr)
{
	int ret = -EINVAL;

	PHM_FUNC_CHECK(hwmgr);

	if (!hwmgr->not_vf)
		return 0;

	if (!smum_is_dpm_running(hwmgr)) {
		pr_info("dpm has been disabled\n");
		return 0;
	}

	if (hwmgr->hwmgr_func->dynamic_state_management_disable)
		ret = hwmgr->hwmgr_func->dynamic_state_management_disable(hwmgr);

	return ret;
}

int phm_force_dpm_levels(struct pp_hwmgr *hwmgr, enum amd_dpm_forced_level level)
{
	int ret = 0;

	PHM_FUNC_CHECK(hwmgr);

	if (hwmgr->hwmgr_func->force_dpm_level != NULL)
		ret = hwmgr->hwmgr_func->force_dpm_level(hwmgr, level);

	return ret;
}

int phm_apply_state_adjust_rules(struct pp_hwmgr *hwmgr,
				   struct pp_power_state *adjusted_ps,
			     const struct pp_power_state *current_ps)
{
	PHM_FUNC_CHECK(hwmgr);

	if (hwmgr->hwmgr_func->apply_state_adjust_rules != NULL)
		return hwmgr->hwmgr_func->apply_state_adjust_rules(
									hwmgr,
								 adjusted_ps,
								 current_ps);
	return 0;
}

int phm_pre_display_configuration_changed(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	/* smu7 has no pre_display_config_changed */
	return 0;
}

int phm_display_configuration_changed(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	if (NULL != hwmgr->hwmgr_func->display_config_changed)
		hwmgr->hwmgr_func->display_config_changed(hwmgr);

	return 0;
}

int phm_notify_smc_display_config_after_ps_adjustment(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	if (NULL != hwmgr->hwmgr_func->notify_smc_display_config_after_ps_adjustment)
			hwmgr->hwmgr_func->notify_smc_display_config_after_ps_adjustment(hwmgr);

	return 0;
}

int phm_stop_thermal_controller(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	if (!hwmgr->not_vf)
		return 0;

	if (hwmgr->hwmgr_func->stop_thermal_controller == NULL)
		return -EINVAL;

	return hwmgr->hwmgr_func->stop_thermal_controller(hwmgr);
}

/**
 * phm_start_thermal_controller - Initializes the thermal controller subsystem.
 *
 * @hwmgr:   the address of the powerplay hardware manager.
 * Exception PP_Result_Failed if any of the paramters is NULL, otherwise the return value from the dispatcher.
 */
int phm_start_thermal_controller(struct pp_hwmgr *hwmgr)
{
	int ret = 0;
	struct PP_TemperatureRange range = {
		TEMP_RANGE_MIN,
		TEMP_RANGE_MAX,
		TEMP_RANGE_MAX,
		TEMP_RANGE_MIN,
		TEMP_RANGE_MAX,
		TEMP_RANGE_MAX,
		TEMP_RANGE_MIN,
		TEMP_RANGE_MAX,
		TEMP_RANGE_MAX,
		0};

	if (!hwmgr->not_vf)
		return 0;

	if (hwmgr->hwmgr_func->get_thermal_temperature_range)
		hwmgr->hwmgr_func->get_thermal_temperature_range(
				hwmgr, &range);

	if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_ThermalController)
			&& hwmgr->hwmgr_func->start_thermal_controller != NULL)
		ret = hwmgr->hwmgr_func->start_thermal_controller(hwmgr, &range);

	/* adev->pm.dpm.thermal: the limits amdgpu's hwmon reports */
	hwmgr->adev->pm_thermal_min = range.min;
	hwmgr->adev->pm_thermal_max = range.max;

	return ret;
}

bool phm_check_smc_update_required_for_display_configuration(struct pp_hwmgr *hwmgr)
{
	if (hwmgr == NULL ||
	    hwmgr->hwmgr_func == NULL)
		return false;

	if (hwmgr->pp_one_vf)
		return false;

	if (hwmgr->hwmgr_func->check_smc_update_required_for_display_configuration == NULL)
		return false;

	return hwmgr->hwmgr_func->check_smc_update_required_for_display_configuration(hwmgr);
}

int phm_check_states_equal(struct pp_hwmgr *hwmgr,
				 const struct pp_hw_power_state *pstate1,
				 const struct pp_hw_power_state *pstate2,
				 bool *equal)
{
	PHM_FUNC_CHECK(hwmgr);

	if (hwmgr->hwmgr_func->check_states_equal == NULL)
		return -EINVAL;

	return hwmgr->hwmgr_func->check_states_equal(hwmgr, pstate1, pstate2, equal);
}

/*
 * phm_store_dal_configuration_data: smu7 has none of the callbacks it
 * dispatches to (set_min_deep_sleep_dcefclk, set_active_display_count,
 * store_cc6_data), so Linux returns -EINVAL here, which nobody checks; the
 * configuration itself is read through hwmgr->display_config.
 */
int phm_store_dal_configuration_data(struct pp_hwmgr *hwmgr,
		const struct amd_pp_display_configuration *display_config)
{
	PHM_FUNC_CHECK(hwmgr);

	if (display_config == NULL)
		return -EINVAL;

	return -EINVAL;
}

int phm_set_cpu_power_state(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	/* smu7 has no set_cpu_power_state */
	return 0;
}

/* smu7 has no apply_clocks_adjust_rules (pp_psm calls it only without power states) */
int phm_apply_clock_adjust_rules(struct pp_hwmgr *hwmgr)
{
	PHM_FUNC_CHECK(hwmgr);

	return 0;
}
