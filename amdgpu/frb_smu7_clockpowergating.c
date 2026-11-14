/*
 * pm/powerplay/hwmgr/smu7_clockpowergating.c (Linux 7.1.1), the UVD part:
 * smu7_powergate_uvd, which amdgpu_dpm_enable_uvd reaches through
 * pp_set_powergating_by_smu / pp_dpm_powergate_uvd when the UVD ring's
 * begin_use finds the block idle (enable) and from the idle work (disable).
 *
 * Deviations: amdgpu_device_ip_set_powergating_state / _clockgating_state
 * for AMD_IP_BLOCK_TYPE_UVD are frb_uvd_v6_0's, and only when stage uvd has
 * brought the block in (amdgpu calls them for every UVD block it has; with
 * none, nothing). VCE and the GFX clock-gating messages are not ported
 * (Firebrand drives no VCE; PP_ENABLE_GFX_CG_THRU_SMU is off on Polaris 12).
 * smu7_disable_clock_power_gating is not needed: with adev->pg_flags 0 on
 * Polaris 12 its power-up sends nothing (hwmgr_hw_fini, frb_hwmgr.c).
 *
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include "frb_uvd.h"
#include "include/pp/smu7_ppsmc.h"

/* amdgpu_device_ip_set_powergating_state(adev, AMD_IP_BLOCK_TYPE_UVD, ...) */
static void uvd_set_powergating_state(struct frb_device *adev, int gate)
{
	if (adev->uvd)
		frb_uvd_v6_0_set_powergating_state(adev, gate);
}

/* amdgpu_device_ip_set_clockgating_state(adev, AMD_IP_BLOCK_TYPE_UVD, ...) */
static void uvd_set_clockgating_state(struct frb_device *adev, int gate)
{
	if (adev->uvd)
		frb_uvd_v6_0_set_clockgating_state(adev, gate);
}

static int smu7_enable_disable_uvd_dpm(struct pp_hwmgr *hwmgr, bool enable)
{
	return smum_send_msg_to_smc(hwmgr, enable ?
			PPSMC_MSG_UVDDPM_Enable :
			PPSMC_MSG_UVDDPM_Disable,
			NULL);
}

static int smu7_update_uvd_dpm(struct pp_hwmgr *hwmgr, bool bgate)
{
	if (!bgate)
		smum_update_smc_table(hwmgr, SMU_UVD_TABLE);
	return smu7_enable_disable_uvd_dpm(hwmgr, !bgate);
}

static int smu7_powerdown_uvd(struct pp_hwmgr *hwmgr)
{
	if (phm_cf_want_uvd_power_gating(hwmgr))
		return smum_send_msg_to_smc(hwmgr,
				PPSMC_MSG_UVDPowerOFF,
				NULL);
	return 0;
}

static int smu7_powerup_uvd(struct pp_hwmgr *hwmgr)
{
	if (phm_cf_want_uvd_power_gating(hwmgr)) {
		if (phm_cap_enabled(hwmgr->platform_descriptor.platformCaps,
				  PHM_PlatformCaps_UVDDynamicPowerGating)) {
			return smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_UVDPowerON, 1, NULL);
		} else {
			return smum_send_msg_to_smc_with_parameter(hwmgr,
					PPSMC_MSG_UVDPowerON, 0, NULL);
		}
	}

	return 0;
}

void smu7_powergate_uvd(struct pp_hwmgr *hwmgr, bool bgate)
{
	struct smu7_hwmgr *data = (struct smu7_hwmgr *)(hwmgr->backend);

	data->uvd_power_gated = bgate;

	if (bgate) {
		uvd_set_powergating_state(hwmgr->adev, 1);
		uvd_set_clockgating_state(hwmgr->adev, 1);
		smu7_update_uvd_dpm(hwmgr, true);
		smu7_powerdown_uvd(hwmgr);
	} else {
		smu7_powerup_uvd(hwmgr);
		uvd_set_clockgating_state(hwmgr->adev, 0);
		uvd_set_powergating_state(hwmgr->adev, 0);
		smu7_update_uvd_dpm(hwmgr, false);
	}

}
