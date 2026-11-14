/*
 * pm/powerplay/hwmgr/hwmgr.c (Linux 7.1.1): the hardware manager's life
 * cycle, the VI/Polaris arm: early_init (caps, the polaris10 SMU manager),
 * sw_init, hw_init (PowerPlay table, backend, power states, setup_asic,
 * DPM enable, thermal controller, performance state), hw_fini, and the
 * task dispatch.
 *
 * Deviations: no ACPI (the T4240 has none; Linux on that platform builds
 * without it too, so PHM_PlatformCaps_PCIEPerformanceRequest stays off);
 * no irq handlers (phm_register_irq_handlers: Firebrand has no IH ring);
 * hwmgr_sw_init's "smu backed is" line goes to the log as Linux's.
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
#include "frb_polaris10_smumgr.h"

static int polaris_set_asic_special_caps(struct pp_hwmgr *hwmgr);
static void hwmgr_init_default_caps(struct pp_hwmgr *hwmgr);
static int hwmgr_set_user_specify_caps(struct pp_hwmgr *hwmgr);

static void hwmgr_init_workload_prority(struct pp_hwmgr *hwmgr)
{
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT] = 0;
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_FULLSCREEN3D] = 1;
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_POWERSAVING] = 2;
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_VIDEO] = 3;
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_VR] = 4;
	hwmgr->workload_prority[PP_SMC_POWER_PROFILE_COMPUTE] = 5;

	hwmgr->workload_setting[0] = PP_SMC_POWER_PROFILE_BOOTUP_DEFAULT;
	hwmgr->workload_setting[1] = PP_SMC_POWER_PROFILE_FULLSCREEN3D;
	hwmgr->workload_setting[2] = PP_SMC_POWER_PROFILE_POWERSAVING;
	hwmgr->workload_setting[3] = PP_SMC_POWER_PROFILE_VIDEO;
	hwmgr->workload_setting[4] = PP_SMC_POWER_PROFILE_VR;
	hwmgr->workload_setting[5] = PP_SMC_POWER_PROFILE_COMPUTE;
}

/* the AMDGPU_FAMILY_VI, CHIP_POLARIS12 arm */
int hwmgr_early_init(struct pp_hwmgr *hwmgr)
{
	if (!hwmgr)
		return -EINVAL;

	hwmgr->usec_timeout = AMD_MAX_USEC_TIMEOUT;
	hwmgr->pp_table_version = PP_TABLE_V1;
	hwmgr->dpm_level = AMD_DPM_FORCED_LEVEL_AUTO;
	hwmgr->request_dpm_level = AMD_DPM_FORCED_LEVEL_AUTO;
	hwmgr_init_default_caps(hwmgr);
	hwmgr_set_user_specify_caps(hwmgr);
	hwmgr->fan_ctrl_is_in_default_mode = true;
	hwmgr_init_workload_prority(hwmgr);
	hwmgr->gfxoff_state_changed_by_workload = false;

	if (hwmgr->chip_family != AMDGPU_FAMILY_VI)
		return -EINVAL;
	/* adev->pm.pp_feature &= ~PP_GFXOFF_MASK; */
	hwmgr->feature_mask &= ~PP_GFXOFF_MASK;
	switch (hwmgr->chip_id) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS10:
	case CHIP_POLARIS12:
		hwmgr->smumgr_funcs = &polaris10_smu_funcs;
		polaris_set_asic_special_caps(hwmgr);
		hwmgr->feature_mask &= ~(PP_UVD_HANDSHAKE_MASK);
		break;
	default:
		return -EINVAL;
	}
	smu7_init_function_pointers(hwmgr);

	return 0;
}

int hwmgr_sw_init(struct pp_hwmgr *hwmgr)
{
	if (!hwmgr || !hwmgr->smumgr_funcs || !hwmgr->smumgr_funcs->smu_init)
		return -EINVAL;

	/* phm_register_irq_handlers: no interrupts on Firebrand */
	pr_info("hwmgr_sw_init smu backed is %s\n", hwmgr->smumgr_funcs->name);

	return hwmgr->smumgr_funcs->smu_init(hwmgr);
}

int hwmgr_sw_fini(struct pp_hwmgr *hwmgr)
{
	if (hwmgr && hwmgr->smumgr_funcs && hwmgr->smumgr_funcs->smu_fini)
		hwmgr->smumgr_funcs->smu_fini(hwmgr);

	return 0;
}

int hwmgr_hw_init(struct pp_hwmgr *hwmgr)
{
	int ret = 0;

	hwmgr->pp_one_vf = false;	/* amdgpu_sriov_is_pp_one_vf: not a VF */
	hwmgr->pm_en = (hwmgr->not_vf || hwmgr->pp_one_vf)	/* amdgpu_dpm = -1: on */
			? true : false;
	if (!hwmgr->pm_en)
		return 0;

	if (!hwmgr->pptable_func ||
	    !hwmgr->pptable_func->pptable_init ||
	    !hwmgr->hwmgr_func->backend_init) {
		hwmgr->pm_en = false;
		pr_info("dpm not supported \n");
		return 0;
	}

	ret = hwmgr->pptable_func->pptable_init(hwmgr);
	if (ret)
		goto err;

	/* ((struct amdgpu_device *)hwmgr->adev)->pm.no_fan = bNoFan; */

	ret = hwmgr->hwmgr_func->backend_init(hwmgr);
	if (ret)
		goto err1;
 /* make sure dc limits are valid */
	if ((hwmgr->dyn_state.max_clock_voltage_on_dc.sclk == 0) ||
			(hwmgr->dyn_state.max_clock_voltage_on_dc.mclk == 0))
			hwmgr->dyn_state.max_clock_voltage_on_dc =
					hwmgr->dyn_state.max_clock_voltage_on_ac;

	ret = psm_init_power_state_table(hwmgr);
	if (ret)
		goto err2;

	ret = phm_setup_asic(hwmgr);
	if (ret)
		goto err2;

	ret = phm_enable_dynamic_state_management(hwmgr);
	if (ret)
		goto err2;
	ret = phm_start_thermal_controller(hwmgr);
	ret |= psm_set_performance_states(hwmgr);
	if (ret)
		goto err2;

	/* ((struct amdgpu_device *)hwmgr->adev)->pm.dpm_enabled = true; (the caller) */

	return 0;
err2:
	if (hwmgr->hwmgr_func->backend_fini)
		hwmgr->hwmgr_func->backend_fini(hwmgr);
err1:
	if (hwmgr->pptable_func->pptable_fini)
		hwmgr->pptable_func->pptable_fini(hwmgr);
err:
	return ret;
}

/*
 * hwmgr_hw_fini: back to the boot state, DPM off, the SMC left running (a
 * later load finds it so and takes polaris10_start_smu's "running" arm).
 * phm_disable_clock_power_gatings is a no-op here: Firebrand never enabled
 * clock or power gating through the SMU.
 */
int hwmgr_hw_fini(struct pp_hwmgr *hwmgr)
{
	if (!hwmgr || !hwmgr->pm_en || !hwmgr->not_vf)
		return 0;

	phm_stop_thermal_controller(hwmgr);
	psm_set_boot_states(hwmgr);
	psm_adjust_power_state_dynamic(hwmgr, true, NULL);
	phm_disable_dynamic_state_management(hwmgr);

	if (hwmgr->hwmgr_func->backend_fini)
		hwmgr->hwmgr_func->backend_fini(hwmgr);
	if (hwmgr->pptable_func->pptable_fini)
		hwmgr->pptable_func->pptable_fini(hwmgr);
	return psm_fini_power_state_table(hwmgr);
}

int hwmgr_handle_task(struct pp_hwmgr *hwmgr, enum amd_pp_task task_id,
		enum amd_pm_state_type *user_state)
{
	int ret = 0;

	if (hwmgr == NULL)
		return -EINVAL;

	(void)user_state;
	switch (task_id) {
	case AMD_PP_TASK_DISPLAY_CONFIG_CHANGE:
		if (!hwmgr->not_vf)
			return ret;
		ret = phm_pre_display_configuration_changed(hwmgr);
		if (ret)
			return ret;
		ret = phm_set_cpu_power_state(hwmgr);
		if (ret)
			return ret;
		ret = psm_set_performance_states(hwmgr);
		if (ret)
			return ret;
		ret = psm_adjust_power_state_dynamic(hwmgr, false, NULL);
		break;
	case AMD_PP_TASK_COMPLETE_INIT:
	case AMD_PP_TASK_READJUST_POWER_STATE:
		ret = psm_adjust_power_state_dynamic(hwmgr, true, NULL);
		break;
	default:
		/* AMD_PP_TASK_ENABLE_USER_STATE: no user power states on Firebrand */
		break;
	}
	return ret;
}

/* no ACPI: amdgpu_acpi_is_pcie_performance_request_supported is not asked */
void hwmgr_init_default_caps(struct pp_hwmgr *hwmgr)
{
	phm_cap_unset(hwmgr->platform_descriptor.platformCaps, PHM_PlatformCaps_PCIEPerformanceRequest);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps, PHM_PlatformCaps_UVDDPM);
	phm_cap_set(hwmgr->platform_descriptor.platformCaps, PHM_PlatformCaps_VCEDPM);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
		PHM_PlatformCaps_DynamicPatchPowerState);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
		PHM_PlatformCaps_EnableSMU7ThermalManagement);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_DynamicPowerManagement);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_SMC);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_DynamicUVDState);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_FanSpeedInTableIsRPM);
	return;
}

int hwmgr_set_user_specify_caps(struct pp_hwmgr *hwmgr)
{
	if (hwmgr->feature_mask & PP_SCLK_DEEP_SLEEP_MASK)
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_SclkDeepSleep);
	else
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_SclkDeepSleep);

	if (hwmgr->feature_mask & PP_POWER_CONTAINMENT_MASK) {
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			    PHM_PlatformCaps_PowerContainment);
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_CAC);
	} else {
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			    PHM_PlatformCaps_PowerContainment);
		phm_cap_unset(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_CAC);
	}

	if (hwmgr->feature_mask & PP_OVERDRIVE_MASK)
		hwmgr->od_enabled = true;

	return 0;
}

int polaris_set_asic_special_caps(struct pp_hwmgr *hwmgr)
{
	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_EVV);
	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_SQRamping);
	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
						PHM_PlatformCaps_RegulatorHot);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_MemorySpreadSpectrumSupport);
	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
			PHM_PlatformCaps_EngineSpreadSpectrumSupport);

	phm_cap_set(hwmgr->platform_descriptor.platformCaps,
					PHM_PlatformCaps_AutomaticDCTransition);

	if (((hwmgr->chip_id == CHIP_POLARIS11) && !hwmgr->is_kicker) ||
	    (hwmgr->chip_id == CHIP_POLARIS12))
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
				PHM_PlatformCaps_SPLLShutdownSupport);

	if (hwmgr->chip_id != CHIP_POLARIS11) {
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
							PHM_PlatformCaps_DBRamping);
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
							PHM_PlatformCaps_TDRamping);
		phm_cap_set(hwmgr->platform_descriptor.platformCaps,
							PHM_PlatformCaps_TCPRamping);
	}
	return 0;
}
