/*
 * pm/powerplay/hwmgr/pp_psm.c (Linux 7.1.1): the power state table and
 * psm_adjust_power_state_dynamic. Unchanged but for the allocations
 * (through the OS layer) and fls().
 *
 * Copyright 2017 Advanced Micro Devices, Inc.
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
int psm_init_power_state_table(struct pp_hwmgr *hwmgr)
{
	int result;
	unsigned int i;
	struct pp_power_state *state;
	int size, table_entries;

	if (hwmgr->hwmgr_func->get_num_of_pp_table_entries == NULL)
		return 0;

	if (hwmgr->hwmgr_func->get_power_state_size == NULL)
		return 0;

	table_entries = hwmgr->hwmgr_func->get_num_of_pp_table_entries(hwmgr);

	size = hwmgr->hwmgr_func->get_power_state_size(hwmgr) +
					  sizeof(struct pp_power_state);

	if (table_entries <= 0 || size == 0) {
		pr_warn("Please check whether power state management is supported on this asic\n");
		hwmgr->num_ps = 0;
		hwmgr->ps_size = 0;
		return 0;
	}
	hwmgr->num_ps = table_entries;
	hwmgr->ps_size = size;

	hwmgr->ps = frb_pp_zalloc(hwmgr, (uint32_t)table_entries * size);
	if (hwmgr->ps == NULL)
		return -ENOMEM;

	hwmgr->request_ps = frb_pp_zalloc(hwmgr, size);
	if (hwmgr->request_ps == NULL) {
		frb_pp_free(hwmgr, hwmgr->ps);
		hwmgr->ps = NULL;
		return -ENOMEM;
	}

	hwmgr->current_ps = frb_pp_zalloc(hwmgr, size);
	if (hwmgr->current_ps == NULL) {
		frb_pp_free(hwmgr, hwmgr->request_ps);
		frb_pp_free(hwmgr, hwmgr->ps);
		hwmgr->request_ps = NULL;
		hwmgr->ps = NULL;
		return -ENOMEM;
	}

	state = hwmgr->ps;

	for (i = 0; i < table_entries; i++) {
		result = hwmgr->hwmgr_func->get_pp_table_entry(hwmgr, i, state);
		if (result) {
			frb_pp_free(hwmgr, hwmgr->current_ps);
			frb_pp_free(hwmgr, hwmgr->request_ps);
			frb_pp_free(hwmgr, hwmgr->ps);
			hwmgr->current_ps = NULL;
			hwmgr->request_ps = NULL;
			hwmgr->ps = NULL;
			return -EINVAL;
		}

		if (state->classification.flags & PP_StateClassificationFlag_Boot) {
			hwmgr->boot_ps = state;
			memcpy(hwmgr->current_ps, state, size);
			memcpy(hwmgr->request_ps, state, size);
		}

		state->id = i + 1; /* assigned unique num for every power state id */

		if (state->classification.flags & PP_StateClassificationFlag_Uvd)
			hwmgr->uvd_ps = state;
		state = (struct pp_power_state *)((uintptr_t)state + size);
	}

	return 0;
}

int psm_fini_power_state_table(struct pp_hwmgr *hwmgr)
{
	if (hwmgr == NULL)
		return -EINVAL;

	if (!hwmgr->ps)
		return 0;

	frb_pp_free(hwmgr, hwmgr->current_ps);
	frb_pp_free(hwmgr, hwmgr->request_ps);
	frb_pp_free(hwmgr, hwmgr->ps);
	hwmgr->request_ps = NULL;
	hwmgr->ps = NULL;
	hwmgr->current_ps = NULL;
	return 0;
}

static int psm_get_ui_state(struct pp_hwmgr *hwmgr,
				enum PP_StateUILabel ui_label,
				unsigned long *state_id)
{
	struct pp_power_state *state;
	int table_entries;
	int i;

	table_entries = hwmgr->num_ps;
	state = hwmgr->ps;

	for (i = 0; i < table_entries; i++) {
		if (state->classification.ui_label & ui_label) {
			*state_id = state->id;
			return 0;
		}
		state = (struct pp_power_state *)((uintptr_t)state + hwmgr->ps_size);
	}
	return -EINVAL;
}

static int psm_get_state_by_classification(struct pp_hwmgr *hwmgr,
					enum PP_StateClassificationFlag flag,
					unsigned long *state_id)
{
	struct pp_power_state *state;
	int table_entries;
	int i;

	table_entries = hwmgr->num_ps;
	state = hwmgr->ps;

	for (i = 0; i < table_entries; i++) {
		if (state->classification.flags & flag) {
			*state_id = state->id;
			return 0;
		}
		state = (struct pp_power_state *)((uintptr_t)state + hwmgr->ps_size);
	}
	return -EINVAL;
}

static int psm_set_states(struct pp_hwmgr *hwmgr, unsigned long state_id)
{
	struct pp_power_state *state;
	int table_entries;
	int i;

	table_entries = hwmgr->num_ps;

	state = hwmgr->ps;

	for (i = 0; i < table_entries; i++) {
		if (state->id == state_id) {
			memcpy(hwmgr->request_ps, state, hwmgr->ps_size);
			return 0;
		}
		state = (struct pp_power_state *)((uintptr_t)state + hwmgr->ps_size);
	}
	return -EINVAL;
}

int psm_set_boot_states(struct pp_hwmgr *hwmgr)
{
	unsigned long state_id;
	int ret = -EINVAL;

	if (!hwmgr->ps)
		return 0;

	if (!psm_get_state_by_classification(hwmgr, PP_StateClassificationFlag_Boot,
					&state_id))
		ret = psm_set_states(hwmgr, state_id);

	return ret;
}

int psm_set_performance_states(struct pp_hwmgr *hwmgr)
{
	unsigned long state_id;
	int ret = -EINVAL;

	if (!hwmgr->ps)
		return 0;

	if (!psm_get_ui_state(hwmgr, PP_StateUILabel_Performance,
					&state_id))
		ret = psm_set_states(hwmgr, state_id);

	return ret;
}

int psm_set_user_performance_state(struct pp_hwmgr *hwmgr,
					enum PP_StateUILabel label_id,
					struct pp_power_state **state)
{
	int table_entries;
	int i;

	if (!hwmgr->ps)
		return 0;

	table_entries = hwmgr->num_ps;
	*state = hwmgr->ps;

restart_search:
	for (i = 0; i < table_entries; i++) {
		if ((*state)->classification.ui_label & label_id)
			return 0;
		*state = (struct pp_power_state *)((uintptr_t)*state + hwmgr->ps_size);
	}

	switch (label_id) {
	case PP_StateUILabel_Battery:
	case PP_StateUILabel_Balanced:
		label_id = PP_StateUILabel_Performance;
		goto restart_search;
	default:
		break;
	}
	return -EINVAL;
}

static void power_state_management(struct pp_hwmgr *hwmgr,
						struct pp_power_state *new_ps)
{
	struct pp_power_state *pcurrent;
	struct pp_power_state *requested;
	bool equal;

	if (new_ps != NULL)
		requested = new_ps;
	else
		requested = hwmgr->request_ps;

	pcurrent = hwmgr->current_ps;

	phm_apply_state_adjust_rules(hwmgr, requested, pcurrent);
	if (pcurrent == NULL || (0 != phm_check_states_equal(hwmgr,
			&pcurrent->hardware, &requested->hardware, &equal)))
		equal = false;

	if (!equal || phm_check_smc_update_required_for_display_configuration(hwmgr)) {
		phm_set_power_state(hwmgr, &pcurrent->hardware, &requested->hardware);
		memcpy(hwmgr->current_ps, hwmgr->request_ps, hwmgr->ps_size);
	}
}

int psm_adjust_power_state_dynamic(struct pp_hwmgr *hwmgr, bool skip_display_settings,
						struct pp_power_state *new_ps)
{
	uint32_t index;
	long workload[1];

	if (hwmgr->not_vf) {
		if (!skip_display_settings)
			phm_display_configuration_changed(hwmgr);

		if (hwmgr->ps)
			power_state_management(hwmgr, new_ps);
		else
			/*
			 * for vega12/vega20 which does not support power state manager
			 * DAL clock limits should also be honoured
			 */
			phm_apply_clock_adjust_rules(hwmgr);

		if (!skip_display_settings)
			phm_notify_smc_display_config_after_ps_adjustment(hwmgr);
	}

	if (!phm_force_dpm_levels(hwmgr, hwmgr->request_dpm_level))
		hwmgr->dpm_level = hwmgr->request_dpm_level;

	/*
	 * The power profile: smu7_set_power_profile_mode is not ported, and
	 * the profile Linux starts in (FULLSCREEN3D, smu7_init_dpm_defaults) is
	 * the one workload_mask selects, so Linux makes no call here either.
	 */
	if (hwmgr->dpm_level != AMD_DPM_FORCED_LEVEL_MANUAL) {
		index = fls(hwmgr->workload_mask);
		index = index > 0 && index <= Workload_Policy_Max ? index - 1 : 0;
		workload[0] = hwmgr->workload_setting[index];

		if (hwmgr->power_profile_mode != workload[0])
			pr_warn("power profile %ld wanted, %u in force: profiles are not ported",
				workload[0], hwmgr->power_profile_mode);
	}

	return 0;
}

