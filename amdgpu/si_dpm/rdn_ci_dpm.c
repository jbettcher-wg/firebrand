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
 *
 * Authors: Alex Deucher (the Linux radeon driver's ci_dpm.c, which this
 * follows; rdn_ci_smu.c holds its SMU-table half and rdn_ci_tables.c the
 * VBIOS parsing)
 */

/*
 * Sea Islands dynamic power management: bring-up (ci_dpm_init,
 * ci_dpm_enable), the power-state switch (ci_dpm_set_power_state),
 * ci_dpm_force_performance_level, the thermal controller and fan control,
 * the display gap, teardown, and the readouts. See rdn_ci_dpm.h for what is
 * left out and why.
 */

#include "rdn_ci_internal.h"
#include "atom/atom.h"
#include "atom/atombios.h"
#include "linux/pptable.h"

static const struct ci_pt_config_reg didt_config_ci[] = {
	{ 0x10, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x10, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x10, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x10, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x11, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x11, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x11, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x11, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x12, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x12, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x12, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x12, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x2, 0x00003fff, 0, 0x4, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x2, 0x03ff0000, 16, 0x80, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x2, 0x78000000, 27, 0x3, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x1, 0x0000ffff, 0, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x1, 0xffff0000, 16, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x0, 0x00000001, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x30, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x30, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x30, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x30, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x31, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x31, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x31, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x31, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x32, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x32, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x32, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x32, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x22, 0x00003fff, 0, 0x4, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x22, 0x03ff0000, 16, 0x80, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x22, 0x78000000, 27, 0x3, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x21, 0x0000ffff, 0, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x21, 0xffff0000, 16, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x20, 0x00000001, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x50, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x50, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x50, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x50, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x51, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x51, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x51, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x51, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x52, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x52, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x52, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x52, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x42, 0x00003fff, 0, 0x4, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x42, 0x03ff0000, 16, 0x80, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x42, 0x78000000, 27, 0x3, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x41, 0x0000ffff, 0, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x41, 0xffff0000, 16, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x40, 0x00000001, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x70, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x70, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x70, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x70, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x71, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x71, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x71, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x71, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x72, 0x000000ff, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x72, 0x0000ff00, 8, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x72, 0x00ff0000, 16, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x72, 0xff000000, 24, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x62, 0x00003fff, 0, 0x4, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x62, 0x03ff0000, 16, 0x80, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x62, 0x78000000, 27, 0x3, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x61, 0x0000ffff, 0, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x61, 0xffff0000, 16, 0x3FFF, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0x60, 0x00000001, 0, 0x0, CISLANDS_CONFIGREG_DIDT_IND },
	{ 0xFFFFFFFF, 0, 0, 0, CISLANDS_CONFIGREG_MMR }
};

static int ci_set_power_limit(struct rdn_ci_dpm *dpm, uint32_t n);
static int ci_set_overdrive_target_tdp(struct rdn_ci_dpm *dpm, uint32_t target_tdp);
static int ci_update_uvd_dpm(struct rdn_ci_dpm *dpm, bool gate);
static void ci_thermal_start_smc_fan_control(struct rdn_ci_dpm *dpm);
static void ci_fan_ctrl_set_default_mode(struct rdn_ci_dpm *dpm);

#define ERR(dpm, ...) rdn_log((dpm)->card->os, RDN_LOG_ERROR, __VA_ARGS__)
#define INFO(dpm, ...) rdn_log((dpm)->card->os, RDN_LOG_INFO, __VA_ARGS__)

/* ---- DIDT ---- */

static void ci_do_enable_didt(struct rdn_ci_dpm *dpm, const bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t data;

	if (pi->caps_sq_ramping) {
		data = RREG32_DIDT(dpm->card, DIDT_SQ_CTRL0);
		if (enable)
			data |= DIDT_CTRL_EN;
		else
			data &= ~DIDT_CTRL_EN;
		WREG32_DIDT(dpm->card, DIDT_SQ_CTRL0, data);
	}

	if (pi->caps_db_ramping) {
		data = RREG32_DIDT(dpm->card, DIDT_DB_CTRL0);
		if (enable)
			data |= DIDT_CTRL_EN;
		else
			data &= ~DIDT_CTRL_EN;
		WREG32_DIDT(dpm->card, DIDT_DB_CTRL0, data);
	}

	if (pi->caps_td_ramping) {
		data = RREG32_DIDT(dpm->card, DIDT_TD_CTRL0);
		if (enable)
			data |= DIDT_CTRL_EN;
		else
			data &= ~DIDT_CTRL_EN;
		WREG32_DIDT(dpm->card, DIDT_TD_CTRL0, data);
	}

	if (pi->caps_tcp_ramping) {
		data = RREG32_DIDT(dpm->card, DIDT_TCP_CTRL0);
		if (enable)
			data |= DIDT_CTRL_EN;
		else
			data &= ~DIDT_CTRL_EN;
		WREG32_DIDT(dpm->card, DIDT_TCP_CTRL0, data);
	}
}

static int ci_program_pt_config_registers(struct rdn_ci_dpm *dpm, const struct ci_pt_config_reg *cac_config_regs)
{
	const struct ci_pt_config_reg *config_regs = cac_config_regs;
	uint32_t data;
	uint32_t cache = 0;

	if (config_regs == NULL)
		return -EINVAL;

	while (config_regs->offset != 0xFFFFFFFF) {
		if (config_regs->type == CISLANDS_CONFIGREG_CACHE) {
			cache |= ((config_regs->value << config_regs->shift) & config_regs->mask);
		} else {
			switch (config_regs->type) {
			case CISLANDS_CONFIGREG_SMC_IND:
				data = RREG32_SMC(dpm->card, config_regs->offset);
				break;
			case CISLANDS_CONFIGREG_DIDT_IND:
				data = RREG32_DIDT(dpm->card, config_regs->offset);
				break;
			default:
				data = RREG32(dpm->card, config_regs->offset << 2);
				break;
			}

			data &= ~config_regs->mask;
			data |= ((config_regs->value << config_regs->shift) & config_regs->mask);
			data |= cache;

			switch (config_regs->type) {
			case CISLANDS_CONFIGREG_SMC_IND:
				WREG32_SMC(dpm->card, config_regs->offset, data);
				break;
			case CISLANDS_CONFIGREG_DIDT_IND:
				WREG32_DIDT(dpm->card, config_regs->offset, data);
				break;
			default:
				WREG32(dpm->card, config_regs->offset << 2, data);
				break;
			}
			cache = 0;
		}
		config_regs++;
	}
	return 0;
}

static int ci_enable_didt(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	if (pi->caps_sq_ramping || pi->caps_db_ramping ||
	    pi->caps_td_ramping || pi->caps_tcp_ramping) {
		cik_enter_rlc_safe_mode(dpm->card);

		if (enable) {
			ret = ci_program_pt_config_registers(dpm, didt_config_ci);
			if (ret) {
				cik_exit_rlc_safe_mode(dpm->card);
				return ret;
			}
		}

		ci_do_enable_didt(dpm, enable);

		cik_exit_rlc_safe_mode(dpm->card);
	}

	return 0;
}

/* ---- power containment, CAC ---- */

static int ci_enable_power_containment(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;
	int ret = 0;

	if (enable) {
		pi->power_containment_features = 0;
		if (pi->caps_power_containment) {
			if (pi->enable_bapm_feature) {
				smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_EnableDTE);
				if (smc_result != PPSMC_Result_OK)
					ret = -EINVAL;
				else
					pi->power_containment_features |= POWERCONTAINMENT_FEATURE_BAPM;
			}

			if (pi->enable_tdc_limit_feature) {
				smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_TDCLimitEnable);
				if (smc_result != PPSMC_Result_OK)
					ret = -EINVAL;
				else
					pi->power_containment_features |= POWERCONTAINMENT_FEATURE_TDCLimit;
			}

			if (pi->enable_pkg_pwr_tracking_feature) {
				smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_PkgPwrLimitEnable);
				if (smc_result != PPSMC_Result_OK) {
					ret = -EINVAL;
				} else {
					struct rdn_ci_cac_tdp_table *cac_tdp_table = &dpm->dyn_state.cac_tdp_table;
					uint32_t default_pwr_limit =
						(uint32_t)(cac_tdp_table->maximum_power_delivery_limit * 256);

					pi->power_containment_features |= POWERCONTAINMENT_FEATURE_PkgPwrLimit;

					ci_set_power_limit(dpm, default_pwr_limit);
				}
			}
		}
	} else {
		if (pi->caps_power_containment && pi->power_containment_features) {
			if (pi->power_containment_features & POWERCONTAINMENT_FEATURE_TDCLimit)
				ci_send_msg_to_smc(dpm, PPSMC_MSG_TDCLimitDisable);

			if (pi->power_containment_features & POWERCONTAINMENT_FEATURE_BAPM)
				ci_send_msg_to_smc(dpm, PPSMC_MSG_DisableDTE);

			if (pi->power_containment_features & POWERCONTAINMENT_FEATURE_PkgPwrLimit)
				ci_send_msg_to_smc(dpm, PPSMC_MSG_PkgPwrLimitDisable);
			pi->power_containment_features = 0;
		}
	}

	return ret;
}

static int ci_enable_smc_cac(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;
	int ret = 0;

	if (pi->caps_cac) {
		if (enable) {
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_EnableCac);
			if (smc_result != PPSMC_Result_OK) {
				ret = -EINVAL;
				pi->cac_enabled = false;
			} else {
				pi->cac_enabled = true;
			}
		} else if (pi->cac_enabled) {
			ci_send_msg_to_smc(dpm, PPSMC_MSG_DisableCac);
			pi->cac_enabled = false;
		}
	}

	return ret;
}

static int ci_enable_thermal_based_sclk_dpm(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result = PPSMC_Result_OK;

	if (pi->thermal_sclk_dpm_enabled) {
		if (enable)
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_ENABLE_THERMAL_DPM);
		else
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_DISABLE_THERMAL_DPM);
	}

	if (smc_result == PPSMC_Result_OK)
		return 0;
	else
		return -EINVAL;
}

static int ci_power_control_set_level(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_cac_tdp_table *cac_tdp_table = &dpm->dyn_state.cac_tdp_table;
	int32_t adjust_percent;
	int32_t target_tdp;
	int ret = 0;
	bool adjust_polarity = false; /* ??? */

	if (pi->caps_power_containment) {
		adjust_percent = adjust_polarity ?
			dpm->tdp_adjustment : (-1 * dpm->tdp_adjustment);
		target_tdp = ((100 + adjust_percent) *
			      (int32_t)cac_tdp_table->configurable_tdp) / 100;

		ret = ci_set_overdrive_target_tdp(dpm, (uint32_t)target_tdp);
	}

	return ret;
}

/* ---- UVD / VCE / SAMU / ACP DPM: stubs ----
 *
 * Linux's ci_enable_uvd_dpm(), ci_update_uvd_dpm(), ci_dpm_powergate_uvd(),
 * ci_enable_vce_dpm() and ci_update_vce_dpm() gate those engines' clocks with
 * their own DPM levels (PPSMC_MSG_UVDDPM_* / VCEDPM_*). This library does
 * not drive UVD or VCE through the SMU, so they stay off: the SMU table
 * carries their levels (rdn_ci_smu.c) but no enable message is ever sent,
 * exactly as Linux behaves while uvd_power_gated is true and no VCE state is
 * active. The SAMU and ACP versions are under #if 0 in Linux too.
 */
static int ci_enable_uvd_dpm(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct rdn_ci_uvd_dep_table *dep_table =
		&dpm->dyn_state.uvd_clock_voltage_dependency_table;
	PPSMC_Result smc_result;
	int i;

	if (enable) {
		if (pi->caps_uvd_dpm || (dep_table->count > 0)) {
			pi->dpm_level_enable_mask.uvd_dpm_enable_mask = 0;
			for (i = (int)dep_table->count - 1; i >= 0; i--) {
				if (dep_table->entries[i].v <= pi->max_vddc_in_pp_table) {
					pi->dpm_level_enable_mask.uvd_dpm_enable_mask |= 1u << i;
					if (!pi->caps_uvd_dpm)
						break;
				}
			}
			ci_send_msg_to_smc_with_parameter(dpm,
							  PPSMC_MSG_UVDDPM_SetEnabledMask,
							  pi->dpm_level_enable_mask.uvd_dpm_enable_mask);
			if (pi->last_mclk_dpm_enable_mask & 1) {
				pi->uvd_enabled = true;
				pi->dpm_level_enable_mask.mclk_dpm_enable_mask &= 0xFFFFFFFEu;
				ci_send_msg_to_smc_with_parameter(dpm,
								  PPSMC_MSG_MCLKDPM_SetEnabledMask,
								  pi->dpm_level_enable_mask.mclk_dpm_enable_mask);
			}
		}
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_UVDDPM_Enable);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	} else {
		if (pi->caps_uvd_dpm || (dep_table->count > 0)) {
			if (pi->uvd_enabled) {
				pi->uvd_enabled = false;
				pi->dpm_level_enable_mask.mclk_dpm_enable_mask |= 1;
				ci_send_msg_to_smc_with_parameter(dpm,
								  PPSMC_MSG_MCLKDPM_SetEnabledMask,
								  pi->dpm_level_enable_mask.mclk_dpm_enable_mask);
			}
		}
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_UVDDPM_Disable);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}
	return 0;
}

static int ci_update_uvd_dpm(struct rdn_ci_dpm *dpm, bool gate)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct rdn_ci_uvd_dep_table *dep_table =
		&dpm->dyn_state.uvd_clock_voltage_dependency_table;
	uint32_t tmp;

	if (!gate) {
		if (pi->caps_uvd_dpm || (dep_table->count <= 0))
			pi->smc_state_table.UvdBootLevel = 0;
		else
			pi->smc_state_table.UvdBootLevel = (uint8_t)(dep_table->count - 1);

		tmp = RREG32_SMC(dpm->card, DPM_TABLE_475);
		tmp &= ~UvdBootLevel_MASK;
		tmp |= UvdBootLevel(pi->smc_state_table.UvdBootLevel);
		WREG32_SMC(dpm->card, DPM_TABLE_475, tmp);
	}
	return ci_enable_uvd_dpm(dpm, !gate);
}

static void ci_dpm_powergate_uvd(struct rdn_ci_dpm *dpm, bool gate)
{
	struct ci_power_info *pi = &dpm->pi;

	if (pi->uvd_power_gated == gate)
		return;

	pi->uvd_power_gated = gate;

	ci_update_uvd_dpm(dpm, gate);
}

void rdn_ci_dpm_powergate_uvd(struct rdn_ci_dpm *dpm, bool gate)
{
	if (!dpm || !dpm->enabled)
		return;
	ci_dpm_powergate_uvd(dpm, gate);
}

static int ci_update_vce_dpm(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_new_state,
			     struct rdn_ci_rps *radeon_current_state)
{
	(void)dpm;
	(void)radeon_new_state;
	(void)radeon_current_state;
	return 0;
}

/* ---- the state adjust rules ---- */

/* ci_dpm_vblank_too_short(): r600_dpm_get_vblank_time() is dpm->vblank_time_us here */
bool rdn_ci_dpm_vblank_too_short(struct rdn_ci_dpm *dpm, uint32_t vrefresh)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t vblank_time = dpm->vblank_time_us;
	uint32_t switch_limit = pi->mem_gddr5 ? 450 : 300;

	/* disable mclk switching if the refresh is >120Hz, even if the
	 * blanking period would allow it
	 */
	if (vrefresh > 120)
		return true;

	if (vblank_time < switch_limit)
		return true;
	else
		return false;

}

static void ci_apply_state_adjust_rules(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *rps)
{
	struct ci_ps *ps = &rps->ps;
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_clock_and_voltage_limits *max_limits;
	bool disable_mclk_switching;
	uint32_t sclk, mclk;
	int i;

	if (rps->vce_active) {
		/* never here: no VCE state is ever activated (the stubs above) */
		rps->evclk = 0;
		rps->ecclk = 0;
	} else {
		rps->evclk = 0;
		rps->ecclk = 0;
	}

	if ((dpm->new_active_crtc_count > 1) ||
	    rdn_ci_dpm_vblank_too_short(dpm, dpm->vrefresh))
		disable_mclk_switching = true;
	else
		disable_mclk_switching = false;

	if ((rps->classification & ATOM_PPLIB_CLASSIFICATION_UI_MASK) == ATOM_PPLIB_CLASSIFICATION_UI_BATTERY)
		pi->battery_state = true;
	else
		pi->battery_state = false;

	if (dpm->ac_power)
		max_limits = &dpm->dyn_state.max_clock_voltage_on_ac;
	else
		max_limits = &dpm->dyn_state.max_clock_voltage_on_dc;

	if (dpm->ac_power == false) {
		for (i = 0; i < ps->performance_level_count; i++) {
			if (ps->performance_levels[i].mclk > max_limits->mclk)
				ps->performance_levels[i].mclk = max_limits->mclk;
			if (ps->performance_levels[i].sclk > max_limits->sclk)
				ps->performance_levels[i].sclk = max_limits->sclk;
		}
	}

	/* XXX validate the min clocks required for display */

	if (disable_mclk_switching) {
		mclk  = ps->performance_levels[ps->performance_level_count - 1].mclk;
		sclk = ps->performance_levels[0].sclk;
	} else {
		mclk = ps->performance_levels[0].mclk;
		sclk = ps->performance_levels[0].sclk;
	}

	ps->performance_levels[0].sclk = sclk;
	ps->performance_levels[0].mclk = mclk;

	if (ps->performance_levels[1].sclk < ps->performance_levels[0].sclk)
		ps->performance_levels[1].sclk = ps->performance_levels[0].sclk;

	if (disable_mclk_switching) {
		if (ps->performance_levels[0].mclk < ps->performance_levels[1].mclk)
			ps->performance_levels[0].mclk = ps->performance_levels[1].mclk;
	} else {
		if (ps->performance_levels[1].mclk < ps->performance_levels[0].mclk)
			ps->performance_levels[1].mclk = ps->performance_levels[0].mclk;
	}
}

/* ---- the thermal controller ---- */

static int ci_thermal_set_temperature_range(struct rdn_ci_dpm *dpm, int min_temp, int max_temp)
{
	int low_temp = 0 * 1000;
	int high_temp = 255 * 1000;
	uint32_t tmp;

	if (low_temp < min_temp)
		low_temp = min_temp;
	if (high_temp > max_temp)
		high_temp = max_temp;
	if (high_temp < low_temp) {
		ERR(dpm, "invalid thermal range: %d - %d", low_temp, high_temp);
		return -EINVAL;
	}

	tmp = RREG32_SMC(dpm->card, CG_THERMAL_INT);
	tmp &= ~(CI_DIG_THERM_INTH_MASK | CI_DIG_THERM_INTL_MASK);
	tmp |= CI_DIG_THERM_INTH(high_temp / 1000) |
		CI_DIG_THERM_INTL(low_temp / 1000);
	WREG32_SMC(dpm->card, CG_THERMAL_INT, tmp);

	dpm->thermal_min_temp = low_temp;
	dpm->thermal_max_temp = high_temp;

	return 0;
}

/*
 * ci_thermal_enable_alert(): the registers and the SMC messages as in Linux.
 * Linux also arms its interrupt handler (rdev->irq.dpm_thermal); there is
 * none here, so the thermal interrupt is unmasked at the SMC but nothing
 * receives it. The SMU's own protection (GENERAL_PWRMGT's thermal
 * protection and the firmware's throttling) does not depend on it.
 */
static int ci_thermal_enable_alert(struct rdn_ci_dpm *dpm, bool enable)
{
	uint32_t thermal_int = RREG32_SMC(dpm->card, CG_THERMAL_INT);
	PPSMC_Result result;

	if (enable) {
		thermal_int &= ~(THERM_INT_MASK_HIGH | THERM_INT_MASK_LOW);
		WREG32_SMC(dpm->card, CG_THERMAL_INT, thermal_int);
		result = ci_send_msg_to_smc(dpm, PPSMC_MSG_Thermal_Cntl_Enable);
		if (result != PPSMC_Result_OK) {
			INFO(dpm, "Could not enable thermal interrupts.");
			return -EINVAL;
		}
	} else {
		thermal_int |= THERM_INT_MASK_HIGH | THERM_INT_MASK_LOW;
		WREG32_SMC(dpm->card, CG_THERMAL_INT, thermal_int);
		result = ci_send_msg_to_smc(dpm, PPSMC_MSG_Thermal_Cntl_Disable);
		if (result != PPSMC_Result_OK) {
			INFO(dpm, "Could not disable thermal interrupts.");
			return -EINVAL;
		}
	}

	return 0;
}

static void ci_fan_ctrl_set_static_mode(struct rdn_ci_dpm *dpm, uint32_t mode)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;

	if (pi->fan_ctrl_is_in_default_mode) {
		tmp = (RREG32_SMC(dpm->card, CG_FDO_CTRL2) & FDO_PWM_MODE_MASK) >> FDO_PWM_MODE_SHIFT;
		pi->fan_ctrl_default_mode = tmp;
		tmp = (RREG32_SMC(dpm->card, CG_FDO_CTRL2) & TMIN_MASK) >> TMIN_SHIFT;
		pi->t_min = tmp;
		pi->fan_ctrl_is_in_default_mode = false;
	}

	tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & ~TMIN_MASK;
	tmp |= TMIN(0);
	WREG32_SMC(dpm->card, CG_FDO_CTRL2, tmp);

	tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & ~FDO_PWM_MODE_MASK;
	tmp |= FDO_PWM_MODE(mode);
	WREG32_SMC(dpm->card, CG_FDO_CTRL2, tmp);
}

static int ci_fan_ctrl_start_smc_fan_control(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result ret;

	if (pi->caps_od_fuzzy_fan_control_support) {
		ret = ci_send_msg_to_smc_with_parameter(dpm,
							PPSMC_StartFanControl,
							FAN_CONTROL_FUZZY);
		if (ret != PPSMC_Result_OK)
			return -EINVAL;
		ret = ci_send_msg_to_smc_with_parameter(dpm,
							PPSMC_MSG_SetFanPwmMax,
							dpm->fan.default_max_fan_pwm);
		if (ret != PPSMC_Result_OK)
			return -EINVAL;
	} else {
		ret = ci_send_msg_to_smc_with_parameter(dpm,
							PPSMC_StartFanControl,
							FAN_CONTROL_TABLE);
		if (ret != PPSMC_Result_OK)
			return -EINVAL;
	}

	pi->fan_is_controlled_by_smc = true;
	return 0;
}

static int ci_fan_ctrl_stop_smc_fan_control(struct rdn_ci_dpm *dpm)
{
	PPSMC_Result ret;
	struct ci_power_info *pi = &dpm->pi;

	ret = ci_send_msg_to_smc(dpm, PPSMC_StopFanControl);
	if (ret == PPSMC_Result_OK) {
		pi->fan_is_controlled_by_smc = false;
		return 0;
	} else
		return -EINVAL;
}

int rdn_ci_dpm_get_fan_speed_percent(struct rdn_ci_dpm *dpm, uint32_t *speed)
{
	uint32_t duty, duty100;

	if (dpm->no_fan)
		return -ENOENT;

	duty100 = (RREG32_SMC(dpm->card, CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;
	duty = (RREG32_SMC(dpm->card, CG_THERMAL_STATUS) & FDO_PWM_DUTY_MASK) >> FDO_PWM_DUTY_SHIFT;

	if (duty100 == 0)
		return -EINVAL;

	/* Linux: do_div on a u64; duty is 8 bits, so 32 bits suffice */
	*speed = (duty * 100) / duty100;

	if (*speed > 100)
		*speed = 100;

	return 0;
}

int rdn_ci_dpm_set_fan_speed_percent(struct rdn_ci_dpm *dpm, uint32_t speed)
{
	uint32_t tmp;
	uint32_t duty, duty100;
	struct ci_power_info *pi = &dpm->pi;

	if (dpm->no_fan)
		return -ENOENT;

	if (pi->fan_is_controlled_by_smc)
		return -EINVAL;

	if (speed > 100)
		return -EINVAL;

	duty100 = (RREG32_SMC(dpm->card, CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;

	if (duty100 == 0)
		return -EINVAL;

	/* Linux: do_div on a u64; speed <= 100 and duty100 is 8 bits */
	duty = (speed * duty100) / 100;

	tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL0) & ~FDO_STATIC_DUTY_MASK;
	tmp |= FDO_STATIC_DUTY(duty);
	WREG32_SMC(dpm->card, CG_FDO_CTRL0, tmp);

	return 0;
}

void rdn_ci_dpm_set_fan_mode(struct rdn_ci_dpm *dpm, uint32_t mode)
{
	if (mode) {
		/* stop auto-manage */
		if (dpm->fan.ucode_fan_control)
			ci_fan_ctrl_stop_smc_fan_control(dpm);
		ci_fan_ctrl_set_static_mode(dpm, mode);
	} else {
		/* restart auto-manage */
		if (dpm->fan.ucode_fan_control)
			ci_thermal_start_smc_fan_control(dpm);
		else
			ci_fan_ctrl_set_default_mode(dpm);
	}
}

uint32_t rdn_ci_dpm_get_fan_mode(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;

	if (pi->fan_is_controlled_by_smc)
		return 0;

	tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & FDO_PWM_MODE_MASK;
	return (tmp >> FDO_PWM_MODE_SHIFT);
}

/*
 * ci_fan_ctrl_get_fan_speed_rpm(): under #if 0 in Linux. As written there
 * the 60 * xclk * 10000 product overflows 32 bits for any real reference
 * clock; it is done in 64 bits here.
 */
int rdn_ci_dpm_get_fan_speed_rpm(struct rdn_ci_dpm *dpm, uint32_t *speed)
{
	uint32_t tach_period;
	uint32_t xclk = cik_get_xclk(dpm);

	if (dpm->no_fan)
		return -ENOENT;

	if (dpm->fan_pulses_per_revolution == 0)
		return -ENOENT;

	tach_period = (RREG32_SMC(dpm->card, CG_TACH_STATUS) & TACH_PERIOD_MASK) >> TACH_PERIOD_SHIFT;
	if (tach_period == 0)
		return -ENOENT;

	*speed = (uint32_t)ci_div64((uint64_t)60 * xclk * 10000, tach_period);

	return 0;
}

static void ci_fan_ctrl_set_default_mode(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;

	if (!pi->fan_ctrl_is_in_default_mode) {
		tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & ~FDO_PWM_MODE_MASK;
		tmp |= FDO_PWM_MODE(pi->fan_ctrl_default_mode);
		WREG32_SMC(dpm->card, CG_FDO_CTRL2, tmp);

		tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & ~TMIN_MASK;
		tmp |= TMIN(pi->t_min);
		WREG32_SMC(dpm->card, CG_FDO_CTRL2, tmp);
		pi->fan_ctrl_is_in_default_mode = true;
	}
}

static void ci_thermal_start_smc_fan_control(struct rdn_ci_dpm *dpm)
{
	if (dpm->fan.ucode_fan_control) {
		ci_fan_ctrl_start_smc_fan_control(dpm);
		ci_fan_ctrl_set_static_mode(dpm, FDO_PWM_MODE_STATIC);
	}
}

static void ci_thermal_initialize(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp;

	if (dpm->fan_pulses_per_revolution) {
		tmp = RREG32_SMC(dpm->card, CG_TACH_CTRL) & ~EDGE_PER_REV_MASK;
		tmp |= EDGE_PER_REV(dpm->fan_pulses_per_revolution - 1);
		WREG32_SMC(dpm->card, CG_TACH_CTRL, tmp);
	}

	tmp = RREG32_SMC(dpm->card, CG_FDO_CTRL2) & ~TACH_PWM_RESP_RATE_MASK;
	tmp |= TACH_PWM_RESP_RATE(0x28);
	WREG32_SMC(dpm->card, CG_FDO_CTRL2, tmp);
}

static int ci_thermal_start_thermal_controller(struct rdn_ci_dpm *dpm)
{
	int ret;

	ci_thermal_initialize(dpm);
	ret = ci_thermal_set_temperature_range(dpm, R600_TEMP_RANGE_MIN, R600_TEMP_RANGE_MAX);
	if (ret)
		return ret;
	ret = ci_thermal_enable_alert(dpm, true);
	if (ret)
		return ret;
	if (dpm->fan.ucode_fan_control) {
		ret = ci_thermal_setup_fan_table(dpm);
		if (ret)
			return ret;
		ci_thermal_start_smc_fan_control(dpm);
	}

	return 0;
}

static void ci_thermal_stop_thermal_controller(struct rdn_ci_dpm *dpm)
{
	if (!dpm->no_fan)
		ci_fan_ctrl_set_default_mode(dpm);
}

/* ---- the leakage voltages ---- */

static void ci_get_leakage_voltages(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint16_t leakage_id, virtual_voltage_id;
	uint16_t vddc, vddci;
	int i;

	pi->vddc_leakage.count = 0;
	pi->vddci_leakage.count = 0;

	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_EVV) {
		for (i = 0; i < CISLANDS_MAX_LEAKAGE_COUNT; i++) {
			virtual_voltage_id = ATOM_VIRTUAL_VOLTAGE_ID0 + i;
			if (radeon_atom_get_voltage_evv(dpm, virtual_voltage_id, &vddc) != 0)
				continue;
			if (vddc != 0 && vddc != virtual_voltage_id) {
				pi->vddc_leakage.actual_voltage[pi->vddc_leakage.count] = vddc;
				pi->vddc_leakage.leakage_id[pi->vddc_leakage.count] = virtual_voltage_id;
				pi->vddc_leakage.count++;
			}
		}
	} else if (radeon_atom_get_leakage_id_from_vbios(dpm->card, &leakage_id) == 0) {
		for (i = 0; i < CISLANDS_MAX_LEAKAGE_COUNT; i++) {
			virtual_voltage_id = ATOM_VIRTUAL_VOLTAGE_ID0 + i;
			if (radeon_atom_get_leakage_vddc_based_on_leakage_params(dpm->card, &vddc, &vddci,
										 virtual_voltage_id,
										 leakage_id) == 0) {
				if (vddc != 0 && vddc != virtual_voltage_id) {
					pi->vddc_leakage.actual_voltage[pi->vddc_leakage.count] = vddc;
					pi->vddc_leakage.leakage_id[pi->vddc_leakage.count] = virtual_voltage_id;
					pi->vddc_leakage.count++;
				}
				if (vddci != 0 && vddci != virtual_voltage_id) {
					pi->vddci_leakage.actual_voltage[pi->vddci_leakage.count] = vddci;
					pi->vddci_leakage.leakage_id[pi->vddci_leakage.count] = virtual_voltage_id;
					pi->vddci_leakage.count++;
				}
			}
		}
	}
}

/* ---- the thermal protection sources ---- */

static void ci_set_dpm_event_sources(struct rdn_ci_dpm *dpm, uint32_t sources)
{
	struct ci_power_info *pi = &dpm->pi;
	bool want_thermal_protection;
	uint32_t tmp;

	switch (sources) {
	case 0:
	default:
		want_thermal_protection = false;
		break;
	case (1 << RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL):
		want_thermal_protection = true;
		break;
	case (1 << RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL):
		want_thermal_protection = true;
		break;
	case ((1 << RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL) |
	      (1 << RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL)):
		want_thermal_protection = true;
		break;
	}

	if (want_thermal_protection) {
		tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
		if (pi->thermal_protection)
			tmp &= ~THERMAL_PROTECTION_DIS;
		else
			tmp |= THERMAL_PROTECTION_DIS;
		WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
	} else {
		tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
		tmp |= THERMAL_PROTECTION_DIS;
		WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
	}
}

static void ci_enable_auto_throttle_source(struct rdn_ci_dpm *dpm,
					   enum radeon_dpm_auto_throttle_src source,
					   bool enable)
{
	struct ci_power_info *pi = &dpm->pi;

	if (enable) {
		if (!(pi->active_auto_throttle_sources & (1 << source))) {
			pi->active_auto_throttle_sources |= 1 << source;
			ci_set_dpm_event_sources(dpm, pi->active_auto_throttle_sources);
		}
	} else {
		if (pi->active_auto_throttle_sources & (1 << source)) {
			pi->active_auto_throttle_sources &= ~(1 << source);
			ci_set_dpm_event_sources(dpm, pi->active_auto_throttle_sources);
		}
	}
}

static void ci_enable_vr_hot_gpio_interrupt(struct rdn_ci_dpm *dpm)
{
	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_REGULATOR_HOT)
		ci_send_msg_to_smc(dpm, PPSMC_MSG_EnableVRHotGPIOInterrupt);
}

/* ---- starting and stopping the DPM ---- */

static int ci_unfreeze_sclk_mclk_dpm(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;

	if (!pi->need_update_smu7_dpm_table)
		return 0;

	if ((!pi->sclk_dpm_key_disabled) &&
	    (pi->need_update_smu7_dpm_table & (DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK))) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_SCLKDPM_UnfreezeLevel);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	if ((!pi->mclk_dpm_key_disabled) &&
	    (pi->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK)) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_MCLKDPM_UnfreezeLevel);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	pi->need_update_smu7_dpm_table = 0;
	return 0;
}

static int ci_enable_sclk_mclk_dpm(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;

	if (enable) {
		if (!pi->sclk_dpm_key_disabled) {
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_DPM_Enable);
			if (smc_result != PPSMC_Result_OK)
				return -EINVAL;
		}

		if (!pi->mclk_dpm_key_disabled) {
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_MCLKDPM_Enable);
			if (smc_result != PPSMC_Result_OK)
				return -EINVAL;

			WREG32_P(dpm->card, MC_SEQ_CNTL_3, CAC_EN, ~CAC_EN);

			WREG32_SMC(dpm->card, LCAC_MC0_CNTL, 0x05);
			WREG32_SMC(dpm->card, LCAC_MC1_CNTL, 0x05);
			WREG32_SMC(dpm->card, LCAC_CPL_CNTL, 0x100005);

			udelay(dpm->card, 10);

			WREG32_SMC(dpm->card, LCAC_MC0_CNTL, 0x400005);
			WREG32_SMC(dpm->card, LCAC_MC1_CNTL, 0x400005);
			WREG32_SMC(dpm->card, LCAC_CPL_CNTL, 0x500005);
		}
	} else {
		if (!pi->sclk_dpm_key_disabled) {
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_DPM_Disable);
			if (smc_result != PPSMC_Result_OK)
				return -EINVAL;
		}

		if (!pi->mclk_dpm_key_disabled) {
			smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_MCLKDPM_Disable);
			if (smc_result != PPSMC_Result_OK)
				return -EINVAL;
		}
	}

	return 0;
}

static int ci_start_dpm(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;
	int ret;
	uint32_t tmp;

	tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
	tmp |= GLOBAL_PWRMGT_EN;
	WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);

	tmp = RREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL);
	tmp |= DYNAMIC_PM_EN;
	WREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL, tmp);

	ci_write_smc_soft_register(dpm, offsetof(SMU7_SoftRegisters, VoltageChangeTimeout), 0x1000);

	WREG32_P(dpm->card, BIF_LNCNT_RESET, 0, ~RESET_LNCNT_EN);

	smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_Voltage_Cntl_Enable);
	if (smc_result != PPSMC_Result_OK)
		return -EINVAL;

	ret = ci_enable_sclk_mclk_dpm(dpm, true);
	if (ret)
		return ret;

	if (!pi->pcie_dpm_key_disabled) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_PCIeDPM_Enable);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_freeze_sclk_mclk_dpm(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;

	if (!pi->need_update_smu7_dpm_table)
		return 0;

	if ((!pi->sclk_dpm_key_disabled) &&
	    (pi->need_update_smu7_dpm_table & (DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK))) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_SCLKDPM_FreezeLevel);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	if ((!pi->mclk_dpm_key_disabled) &&
	    (pi->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK)) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_MCLKDPM_FreezeLevel);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_stop_dpm(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result smc_result;
	int ret;
	uint32_t tmp;

	tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
	tmp &= ~GLOBAL_PWRMGT_EN;
	WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);

	tmp = RREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL);
	tmp &= ~DYNAMIC_PM_EN;
	WREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL, tmp);

	if (!pi->pcie_dpm_key_disabled) {
		smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_PCIeDPM_Disable);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	ret = ci_enable_sclk_mclk_dpm(dpm, false);
	if (ret)
		return ret;

	smc_result = ci_send_msg_to_smc(dpm, PPSMC_MSG_Voltage_Cntl_Disable);
	if (smc_result != PPSMC_Result_OK)
		return -EINVAL;

	return 0;
}

static void ci_enable_sclk_control(struct rdn_ci_dpm *dpm, bool enable)
{
	uint32_t tmp = RREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL);

	if (enable)
		tmp &= ~SCLK_PWRMGT_OFF;
	else
		tmp |= SCLK_PWRMGT_OFF;
	WREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL, tmp);
}

/* ---- forcing levels ---- */

static int ci_dpm_force_state_sclk(struct rdn_ci_dpm *dpm, uint32_t n)
{
	struct ci_power_info *pi = &dpm->pi;

	if (!pi->sclk_dpm_key_disabled) {
		PPSMC_Result smc_result =
			ci_send_msg_to_smc_with_parameter(dpm, PPSMC_MSG_SCLKDPM_SetEnabledMask, 1 << n);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_dpm_force_state_mclk(struct rdn_ci_dpm *dpm, uint32_t n)
{
	struct ci_power_info *pi = &dpm->pi;

	if (!pi->mclk_dpm_key_disabled) {
		PPSMC_Result smc_result =
			ci_send_msg_to_smc_with_parameter(dpm, PPSMC_MSG_MCLKDPM_SetEnabledMask, 1 << n);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_dpm_force_state_pcie(struct rdn_ci_dpm *dpm, uint32_t n)
{
	struct ci_power_info *pi = &dpm->pi;

	if (!pi->pcie_dpm_key_disabled) {
		PPSMC_Result smc_result =
			ci_send_msg_to_smc_with_parameter(dpm, PPSMC_MSG_PCIeDPM_ForceLevel, n);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_set_power_limit(struct rdn_ci_dpm *dpm, uint32_t n)
{
	struct ci_power_info *pi = &dpm->pi;

	if (pi->power_containment_features & POWERCONTAINMENT_FEATURE_PkgPwrLimit) {
		PPSMC_Result smc_result =
			ci_send_msg_to_smc_with_parameter(dpm, PPSMC_MSG_PkgPwrSetLimit, n);
		if (smc_result != PPSMC_Result_OK)
			return -EINVAL;
	}

	return 0;
}

static int ci_set_overdrive_target_tdp(struct rdn_ci_dpm *dpm, uint32_t target_tdp)
{
	PPSMC_Result smc_result =
		ci_send_msg_to_smc_with_parameter(dpm, PPSMC_MSG_OverDriveSetTargetTdp, target_tdp);
	if (smc_result != PPSMC_Result_OK)
		return -EINVAL;
	return 0;
}

/* ---- the readouts ---- */

static uint32_t ci_get_average_sclk_freq(struct rdn_ci_dpm *dpm)
{
	uint32_t sclk_freq;
	PPSMC_Result smc_result =
		ci_send_msg_to_smc_return_parameter(dpm,
						    PPSMC_MSG_API_GetSclkFrequency,
						    &sclk_freq);
	if (smc_result != PPSMC_Result_OK)
		sclk_freq = 0;

	return sclk_freq;
}

static uint32_t ci_get_average_mclk_freq(struct rdn_ci_dpm *dpm)
{
	uint32_t mclk_freq;
	PPSMC_Result smc_result =
		ci_send_msg_to_smc_return_parameter(dpm,
						    PPSMC_MSG_API_GetMclkFrequency,
						    &mclk_freq);
	if (smc_result != PPSMC_Result_OK)
		mclk_freq = 0;

	return mclk_freq;
}

/* ---- the SMC ---- */

/* ci_dpm_start_smc(): rdn_cik_smc_run() is ci_program_jump_on_start() + the clock + the reset release + the wait */
static void ci_dpm_start_smc(struct rdn_ci_dpm *dpm)
{
	rdn_cik_smc_run(dpm->card);
}

/* ci_dpm_stop_smc(): ci_reset_smc() then ci_stop_smc_clock() */
static void ci_dpm_stop_smc(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp;

	tmp = RREG32_SMC(dpm->card, SMC_SYSCON_RESET_CNTL);
	tmp |= RST_REG;
	WREG32_SMC(dpm->card, SMC_SYSCON_RESET_CNTL, tmp);

	tmp = RREG32_SMC(dpm->card, SMC_SYSCON_CLOCK_CNTL_0);
	tmp |= CK_DISABLE;
	WREG32_SMC(dpm->card, SMC_SYSCON_CLOCK_CNTL_0, tmp);
}

static void ci_read_clock_registers(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_card *card = dpm->card;

	pi->clock_registers.cg_spll_func_cntl =
		RREG32_SMC(card, CG_SPLL_FUNC_CNTL);
	pi->clock_registers.cg_spll_func_cntl_2 =
		RREG32_SMC(card, CG_SPLL_FUNC_CNTL_2);
	pi->clock_registers.cg_spll_func_cntl_3 =
		RREG32_SMC(card, CG_SPLL_FUNC_CNTL_3);
	pi->clock_registers.cg_spll_func_cntl_4 =
		RREG32_SMC(card, CG_SPLL_FUNC_CNTL_4);
	pi->clock_registers.cg_spll_spread_spectrum =
		RREG32_SMC(card, CG_SPLL_SPREAD_SPECTRUM);
	pi->clock_registers.cg_spll_spread_spectrum_2 =
		RREG32_SMC(card, CG_SPLL_SPREAD_SPECTRUM_2);
	pi->clock_registers.dll_cntl = RREG32(card, DLL_CNTL);
	pi->clock_registers.mclk_pwrmgt_cntl = RREG32(card, MCLK_PWRMGT_CNTL);
	pi->clock_registers.mpll_ad_func_cntl = RREG32(card, MPLL_AD_FUNC_CNTL);
	pi->clock_registers.mpll_dq_func_cntl = RREG32(card, MPLL_DQ_FUNC_CNTL);
	pi->clock_registers.mpll_func_cntl = RREG32(card, MPLL_FUNC_CNTL);
	pi->clock_registers.mpll_func_cntl_1 = RREG32(card, MPLL_FUNC_CNTL_1);
	pi->clock_registers.mpll_func_cntl_2 = RREG32(card, MPLL_FUNC_CNTL_2);
	pi->clock_registers.mpll_ss1 = RREG32(card, MPLL_SS1);
	pi->clock_registers.mpll_ss2 = RREG32(card, MPLL_SS2);
}

static void ci_init_sclk_t(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	pi->low_sclk_interrupt_t = 0;
}

static void ci_enable_thermal_protection(struct rdn_ci_dpm *dpm, bool enable)
{
	uint32_t tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);

	if (enable)
		tmp &= ~THERMAL_PROTECTION_DIS;
	else
		tmp |= THERMAL_PROTECTION_DIS;
	WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
}

static void ci_enable_acpi_power_management(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);

	tmp |= STATIC_PM_EN;

	WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
}

static int ci_notify_smc_display_change(struct rdn_ci_dpm *dpm, bool has_display)
{
	PPSMC_Msg msg = has_display ? PPSMC_MSG_HasDisplay : PPSMC_MSG_NoDisplay;

	return (ci_send_msg_to_smc(dpm, msg) == PPSMC_Result_OK) ?  0 : -EINVAL;
}

static int ci_enable_ds_master_switch(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;

	if (enable) {
		if (pi->caps_sclk_ds) {
			if (ci_send_msg_to_smc(dpm, PPSMC_MSG_MASTER_DeepSleep_ON) != PPSMC_Result_OK)
				return -EINVAL;
		} else {
			if (ci_send_msg_to_smc(dpm, PPSMC_MSG_MASTER_DeepSleep_OFF) != PPSMC_Result_OK)
				return -EINVAL;
		}
	} else {
		if (pi->caps_sclk_ds) {
			if (ci_send_msg_to_smc(dpm, PPSMC_MSG_MASTER_DeepSleep_OFF) != PPSMC_Result_OK)
				return -EINVAL;
		}
	}

	return 0;
}

/* ci_program_display_gap(): refresh and vblank come from rdn_ci_dpm_display_configuration_changed() */
static void ci_program_display_gap(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp = RREG32_SMC(dpm->card, CG_DISPLAY_GAP_CNTL);
	uint32_t pre_vbi_time_in_us;
	uint32_t frame_time_in_us;
	uint32_t ref_clock = dpm->reference_freq;
	uint32_t refresh_rate = dpm->vrefresh;
	uint32_t vblank_time = dpm->vblank_time_us;

	tmp &= ~DISP_GAP_MASK;
	if (dpm->new_active_crtc_count > 0)
		tmp |= DISP_GAP(R600_PM_DISPLAY_GAP_VBLANK_OR_WM);
	else
		tmp |= DISP_GAP(R600_PM_DISPLAY_GAP_IGNORE);
	WREG32_SMC(dpm->card, CG_DISPLAY_GAP_CNTL, tmp);

	if (refresh_rate == 0)
		refresh_rate = 60;
	if (vblank_time == 0xffffffff)
		vblank_time = 500;
	frame_time_in_us = 1000000 / refresh_rate;
	pre_vbi_time_in_us =
		frame_time_in_us - 200 - vblank_time;
	tmp = pre_vbi_time_in_us * (ref_clock / 100);

	WREG32_SMC(dpm->card, CG_DISPLAY_GAP_CNTL2, tmp);
	ci_write_smc_soft_register(dpm, offsetof(SMU7_SoftRegisters, PreVBlankGap), 0x64);
	ci_write_smc_soft_register(dpm, offsetof(SMU7_SoftRegisters, VBlankTimeout), (frame_time_in_us - pre_vbi_time_in_us));


	ci_notify_smc_display_change(dpm, (dpm->new_active_crtc_count == 1));

}

static void ci_enable_spread_spectrum(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;

	if (enable) {
		if (pi->caps_sclk_ss_support) {
			tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
			tmp |= DYN_SPREAD_SPECTRUM_EN;
			WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
		}
	} else {
		tmp = RREG32_SMC(dpm->card, CG_SPLL_SPREAD_SPECTRUM);
		tmp &= ~SSEN;
		WREG32_SMC(dpm->card, CG_SPLL_SPREAD_SPECTRUM, tmp);

		tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);
		tmp &= ~DYN_SPREAD_SPECTRUM_EN;
		WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
	}
}

static void ci_program_sstp(struct rdn_ci_dpm *dpm)
{
	WREG32_SMC(dpm->card, CG_SSP, (SSTU(R600_SSTU_DFLT) | SST(R600_SST_DFLT)));
}

static void ci_enable_display_gap(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp = RREG32_SMC(dpm->card, CG_DISPLAY_GAP_CNTL);

	tmp &= ~(DISP_GAP_MASK | DISP_GAP_MCHG_MASK);
	tmp |= (DISP_GAP(R600_PM_DISPLAY_GAP_IGNORE) |
		DISP_GAP_MCHG(R600_PM_DISPLAY_GAP_VBLANK));

	WREG32_SMC(dpm->card, CG_DISPLAY_GAP_CNTL, tmp);
}

static void ci_program_vc(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp;

	tmp = RREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL);
	tmp &= ~(RESET_SCLK_CNT | RESET_BUSY_CNT);
	WREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL, tmp);

	WREG32_SMC(dpm->card, CG_FTV_0, CISLANDS_VRC_DFLT0);
	WREG32_SMC(dpm->card, CG_FTV_1, CISLANDS_VRC_DFLT1);
	WREG32_SMC(dpm->card, CG_FTV_2, CISLANDS_VRC_DFLT2);
	WREG32_SMC(dpm->card, CG_FTV_3, CISLANDS_VRC_DFLT3);
	WREG32_SMC(dpm->card, CG_FTV_4, CISLANDS_VRC_DFLT4);
	WREG32_SMC(dpm->card, CG_FTV_5, CISLANDS_VRC_DFLT5);
	WREG32_SMC(dpm->card, CG_FTV_6, CISLANDS_VRC_DFLT6);
	WREG32_SMC(dpm->card, CG_FTV_7, CISLANDS_VRC_DFLT7);
}

static void ci_clear_vc(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp;

	tmp = RREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL);
	tmp |= (RESET_SCLK_CNT | RESET_BUSY_CNT);
	WREG32_SMC(dpm->card, SCLK_PWRMGT_CNTL, tmp);

	WREG32_SMC(dpm->card, CG_FTV_0, 0);
	WREG32_SMC(dpm->card, CG_FTV_1, 0);
	WREG32_SMC(dpm->card, CG_FTV_2, 0);
	WREG32_SMC(dpm->card, CG_FTV_3, 0);
	WREG32_SMC(dpm->card, CG_FTV_4, 0);
	WREG32_SMC(dpm->card, CG_FTV_5, 0);
	WREG32_SMC(dpm->card, CG_FTV_6, 0);
	WREG32_SMC(dpm->card, CG_FTV_7, 0);
}

/* ci_upload_firmware(): the BOOT_SEQ_DONE wait and MISC_CNTL, then rdn_cik_smc_load() (stop clock, reset, load) */
static int ci_upload_firmware(struct rdn_ci_dpm *dpm, const void *fw, size_t size)
{
	uint32_t i;

	for (i = 0; i < CI_USEC_TIMEOUT; i++) {
		if (RREG32_SMC(dpm->card, RCU_UC_EVENTS) & BOOT_SEQ_DONE)
			break;
	}
	WREG32_SMC(dpm->card, SMC_SYSCON_MISC_CNTL, 1);

	return rdn_cik_smc_load(dpm->card, fw, size);
}

static int ci_enable_ulv(struct rdn_ci_dpm *dpm, bool enable)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_ulv_parm *ulv = &pi->ulv;

	if (ulv->supported) {
		if (enable)
			return (ci_send_msg_to_smc(dpm, PPSMC_MSG_EnableULV) == PPSMC_Result_OK) ?
				0 : -EINVAL;
		else
			return (ci_send_msg_to_smc(dpm, PPSMC_MSG_DisableULV) == PPSMC_Result_OK) ?
				0 : -EINVAL;
	}

	return 0;
}

/* ---- the level enable masks of a state ---- */

static void ci_trim_single_dpm_states(struct ci_single_dpm_table *dpm_table,
				      uint32_t low_limit, uint32_t high_limit)
{
	uint32_t i;

	for (i = 0; i < dpm_table->count; i++) {
		if ((dpm_table->dpm_levels[i].value < low_limit) ||
		    (dpm_table->dpm_levels[i].value > high_limit))
			dpm_table->dpm_levels[i].enabled = false;
		else
			dpm_table->dpm_levels[i].enabled = true;
	}
}

static void ci_trim_pcie_dpm_states(struct rdn_ci_dpm *dpm,
				    uint32_t speed_low, uint32_t lanes_low,
				    uint32_t speed_high, uint32_t lanes_high)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_single_dpm_table *pcie_table = &pi->dpm_table.pcie_speed_table;
	uint32_t i, j;

	for (i = 0; i < pcie_table->count; i++) {
		if ((pcie_table->dpm_levels[i].value < speed_low) ||
		    (pcie_table->dpm_levels[i].param1 < lanes_low) ||
		    (pcie_table->dpm_levels[i].value > speed_high) ||
		    (pcie_table->dpm_levels[i].param1 > lanes_high))
			pcie_table->dpm_levels[i].enabled = false;
		else
			pcie_table->dpm_levels[i].enabled = true;
	}

	for (i = 0; i < pcie_table->count; i++) {
		if (pcie_table->dpm_levels[i].enabled) {
			for (j = i + 1; j < pcie_table->count; j++) {
				if (pcie_table->dpm_levels[j].enabled) {
					if ((pcie_table->dpm_levels[i].value == pcie_table->dpm_levels[j].value) &&
					    (pcie_table->dpm_levels[i].param1 == pcie_table->dpm_levels[j].param1))
						pcie_table->dpm_levels[j].enabled = false;
				}
			}
		}
	}
}

static int ci_trim_dpm_states(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_state)
{
	struct ci_ps *state = &radeon_state->ps;
	struct ci_power_info *pi = &dpm->pi;
	uint32_t high_limit_count;

	if (state->performance_level_count < 1)
		return -EINVAL;

	if (state->performance_level_count == 1)
		high_limit_count = 0;
	else
		high_limit_count = 1;

	ci_trim_single_dpm_states(&pi->dpm_table.sclk_table,
				  state->performance_levels[0].sclk,
				  state->performance_levels[high_limit_count].sclk);

	ci_trim_single_dpm_states(&pi->dpm_table.mclk_table,
				  state->performance_levels[0].mclk,
				  state->performance_levels[high_limit_count].mclk);

	ci_trim_pcie_dpm_states(dpm,
				state->performance_levels[0].pcie_gen,
				state->performance_levels[0].pcie_lane,
				state->performance_levels[high_limit_count].pcie_gen,
				state->performance_levels[high_limit_count].pcie_lane);

	return 0;
}

/*
 * ci_apply_disp_minimum_voltage_request(): Linux matches
 * rdev->clock.current_dispclk against the dispclk table ci_dpm_init() made
 * up (0/36000/54000/72000 -> 0/720/810/900 mV). The display clock is not
 * tracked here, so current_dispclk is 0: the request is the table's first
 * voltage, 0, which the loop below raises to the lowest VDDC level, as it
 * does in Linux for any clock the table does not list.
 */
static int ci_apply_disp_minimum_voltage_request(struct rdn_ci_dpm *dpm)
{
	struct rdn_ci_dep_table *disp_voltage_table =
		&dpm->dyn_state.vddc_dependency_on_dispclk;
	struct rdn_ci_dep_table *vddc_table =
		&dpm->dyn_state.vddc_dependency_on_sclk;
	uint32_t current_dispclk = 0;
	uint32_t requested_voltage = 0;
	uint32_t i;

	if (disp_voltage_table == NULL)
		return -EINVAL;
	if (!disp_voltage_table->count)
		return -EINVAL;

	for (i = 0; i < disp_voltage_table->count; i++) {
		if (current_dispclk == disp_voltage_table->entries[i].clk)
			requested_voltage = disp_voltage_table->entries[i].v;
	}

	for (i = 0; i < vddc_table->count; i++) {
		if (requested_voltage <= vddc_table->entries[i].v) {
			requested_voltage = vddc_table->entries[i].v;
			return (ci_send_msg_to_smc_with_parameter(dpm,
								  PPSMC_MSG_VddC_Request,
								  requested_voltage * VOLTAGE_SCALE) == PPSMC_Result_OK) ?
				0 : -EINVAL;
		}
	}

	return -EINVAL;
}

static int ci_upload_dpm_level_enable_mask(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	PPSMC_Result result;

	ci_apply_disp_minimum_voltage_request(dpm);

	if (!pi->sclk_dpm_key_disabled) {
		if (pi->dpm_level_enable_mask.sclk_dpm_enable_mask) {
			result = ci_send_msg_to_smc_with_parameter(dpm,
								   PPSMC_MSG_SCLKDPM_SetEnabledMask,
								   pi->dpm_level_enable_mask.sclk_dpm_enable_mask);
			if (result != PPSMC_Result_OK)
				return -EINVAL;
		}
	}

	if (!pi->mclk_dpm_key_disabled) {
		if (pi->dpm_level_enable_mask.mclk_dpm_enable_mask) {
			result = ci_send_msg_to_smc_with_parameter(dpm,
								   PPSMC_MSG_MCLKDPM_SetEnabledMask,
								   pi->dpm_level_enable_mask.mclk_dpm_enable_mask);
			if (result != PPSMC_Result_OK)
				return -EINVAL;
		}
	}
	/* Linux has the PCIe mask upload under #if 0 */
	return 0;
}

static void ci_find_dpm_states_clocks_in_dpm_table(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_state)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_ps *state = &radeon_state->ps;
	struct ci_single_dpm_table *sclk_table = &pi->dpm_table.sclk_table;
	uint32_t sclk = state->performance_levels[state->performance_level_count-1].sclk;
	struct ci_single_dpm_table *mclk_table = &pi->dpm_table.mclk_table;
	uint32_t mclk = state->performance_levels[state->performance_level_count-1].mclk;
	uint32_t i;

	pi->need_update_smu7_dpm_table = 0;

	for (i = 0; i < sclk_table->count; i++) {
		if (sclk == sclk_table->dpm_levels[i].value)
			break;
	}

	if (i >= sclk_table->count) {
		pi->need_update_smu7_dpm_table |= DPMTABLE_OD_UPDATE_SCLK;
	} else {
		/* XXX The current code always reprogrammed the sclk levels,
		 * but we don't currently handle disp sclk requirements
		 * so just skip it.
		 */
	}

	for (i = 0; i < mclk_table->count; i++) {
		if (mclk == mclk_table->dpm_levels[i].value)
			break;
	}

	if (i >= mclk_table->count)
		pi->need_update_smu7_dpm_table |= DPMTABLE_OD_UPDATE_MCLK;

	if (dpm->current_active_crtc_count !=
	    dpm->new_active_crtc_count)
		pi->need_update_smu7_dpm_table |= DPMTABLE_UPDATE_MCLK;
}

static int ci_populate_and_upload_sclk_mclk_dpm_levels(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_state)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_ps *state = &radeon_state->ps;
	uint32_t sclk = state->performance_levels[state->performance_level_count-1].sclk;
	uint32_t mclk = state->performance_levels[state->performance_level_count-1].mclk;
	struct ci_dpm_table *dpm_table = &pi->dpm_table;
	int ret;

	if (!pi->need_update_smu7_dpm_table)
		return 0;

	if (pi->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_SCLK)
		dpm_table->sclk_table.dpm_levels[dpm_table->sclk_table.count-1].value = sclk;

	if (pi->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK)
		dpm_table->mclk_table.dpm_levels[dpm_table->mclk_table.count-1].value = mclk;

	if (pi->need_update_smu7_dpm_table & (DPMTABLE_OD_UPDATE_SCLK | DPMTABLE_UPDATE_SCLK)) {
		ret = ci_populate_all_graphic_levels(dpm);
		if (ret)
			return ret;
	}

	if (pi->need_update_smu7_dpm_table & (DPMTABLE_OD_UPDATE_MCLK | DPMTABLE_UPDATE_MCLK)) {
		ret = ci_populate_all_memory_levels(dpm);
		if (ret)
			return ret;
	}

	return 0;
}

static int ci_generate_dpm_level_enable_mask(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_state)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	ret = ci_trim_dpm_states(dpm, radeon_state);
	if (ret)
		return ret;

	pi->dpm_level_enable_mask.sclk_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&pi->dpm_table.sclk_table);
	pi->dpm_level_enable_mask.mclk_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&pi->dpm_table.mclk_table);
	pi->last_mclk_dpm_enable_mask =
		pi->dpm_level_enable_mask.mclk_dpm_enable_mask;
	if (pi->uvd_enabled) {
		if (pi->dpm_level_enable_mask.mclk_dpm_enable_mask & 1)
			pi->dpm_level_enable_mask.mclk_dpm_enable_mask &= 0xFFFFFFFE;
	}
	pi->dpm_level_enable_mask.pcie_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&pi->dpm_table.pcie_speed_table);

	return 0;
}

static uint32_t ci_get_lowest_enabled_level(uint32_t level_mask)
{
	uint32_t level = 0;

	while ((level_mask & (1 << level)) == 0)
		level++;

	return level;
}

int rdn_ci_dpm_force_performance_level(struct rdn_ci_dpm *dpm, enum rdn_ci_forced_level level)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp, levels, i;
	int ret;

	if (!dpm->enabled)
		return -EINVAL;

	if (level == RDN_CI_FORCED_LEVEL_HIGH) {
		if ((!pi->pcie_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.pcie_dpm_enable_mask) {
			levels = 0;
			tmp = pi->dpm_level_enable_mask.pcie_dpm_enable_mask;
			while (tmp >>= 1)
				levels++;
			if (levels) {
				/* Linux passes `level` (the enum, 2) rather than `levels` here; kept as is */
				ret = ci_dpm_force_state_pcie(dpm, level);
				if (ret)
					return ret;
				for (i = 0; i < CI_USEC_TIMEOUT; i++) {
					tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX_1) &
					       CURR_PCIE_INDEX_MASK) >> CURR_PCIE_INDEX_SHIFT;
					if (tmp == levels)
						break;
					udelay(dpm->card, 1);
				}
			}
		}
		if ((!pi->sclk_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.sclk_dpm_enable_mask) {
			levels = 0;
			tmp = pi->dpm_level_enable_mask.sclk_dpm_enable_mask;
			while (tmp >>= 1)
				levels++;
			if (levels) {
				ret = ci_dpm_force_state_sclk(dpm, levels);
				if (ret)
					return ret;
				for (i = 0; i < CI_USEC_TIMEOUT; i++) {
					tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX) &
					       CURR_SCLK_INDEX_MASK) >> CURR_SCLK_INDEX_SHIFT;
					if (tmp == levels)
						break;
					udelay(dpm->card, 1);
				}
			}
		}
		if ((!pi->mclk_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.mclk_dpm_enable_mask) {
			levels = 0;
			tmp = pi->dpm_level_enable_mask.mclk_dpm_enable_mask;
			while (tmp >>= 1)
				levels++;
			if (levels) {
				ret = ci_dpm_force_state_mclk(dpm, levels);
				if (ret)
					return ret;
				for (i = 0; i < CI_USEC_TIMEOUT; i++) {
					tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX) &
					       CURR_MCLK_INDEX_MASK) >> CURR_MCLK_INDEX_SHIFT;
					if (tmp == levels)
						break;
					udelay(dpm->card, 1);
				}
			}
		}
	} else if (level == RDN_CI_FORCED_LEVEL_LOW) {
		if ((!pi->sclk_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.sclk_dpm_enable_mask) {
			levels = ci_get_lowest_enabled_level(pi->dpm_level_enable_mask.sclk_dpm_enable_mask);
			ret = ci_dpm_force_state_sclk(dpm, levels);
			if (ret)
				return ret;
			for (i = 0; i < CI_USEC_TIMEOUT; i++) {
				tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX) &
				       CURR_SCLK_INDEX_MASK) >> CURR_SCLK_INDEX_SHIFT;
				if (tmp == levels)
					break;
				udelay(dpm->card, 1);
			}
		}
		if ((!pi->mclk_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.mclk_dpm_enable_mask) {
			levels = ci_get_lowest_enabled_level(pi->dpm_level_enable_mask.mclk_dpm_enable_mask);
			ret = ci_dpm_force_state_mclk(dpm, levels);
			if (ret)
				return ret;
			for (i = 0; i < CI_USEC_TIMEOUT; i++) {
				tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX) &
				       CURR_MCLK_INDEX_MASK) >> CURR_MCLK_INDEX_SHIFT;
				if (tmp == levels)
					break;
				udelay(dpm->card, 1);
			}
		}
		if ((!pi->pcie_dpm_key_disabled) &&
		    pi->dpm_level_enable_mask.pcie_dpm_enable_mask) {
			levels = ci_get_lowest_enabled_level(pi->dpm_level_enable_mask.pcie_dpm_enable_mask);
			ret = ci_dpm_force_state_pcie(dpm, levels);
			if (ret)
				return ret;
			for (i = 0; i < CI_USEC_TIMEOUT; i++) {
				tmp = (RREG32_SMC(dpm->card, TARGET_AND_CURRENT_PROFILE_INDEX_1) &
				       CURR_PCIE_INDEX_MASK) >> CURR_PCIE_INDEX_SHIFT;
				if (tmp == levels)
					break;
				udelay(dpm->card, 1);
			}
		}
	} else if (level == RDN_CI_FORCED_LEVEL_AUTO) {
		if (!pi->pcie_dpm_key_disabled) {
			PPSMC_Result smc_result;

			smc_result = ci_send_msg_to_smc(dpm,
							PPSMC_MSG_PCIeDPM_UnForceLevel);
			if (smc_result != PPSMC_Result_OK)
				return -EINVAL;
		}
		ret = ci_upload_dpm_level_enable_mask(dpm);
		if (ret)
			return ret;
	}

	dpm->forced_level = level;

	return 0;
}

/* ---- voltage control, the PCIe link ---- */

static void ci_enable_voltage_control(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp = RREG32_SMC(dpm->card, GENERAL_PWRMGT);

	tmp |= VOLT_PWRMGT_EN;
	WREG32_SMC(dpm->card, GENERAL_PWRMGT, tmp);
}

static enum rdn_ci_pcie_gen ci_get_maximum_link_speed(struct rdn_ci_rps *radeon_state)
{
	struct ci_ps *state = &radeon_state->ps;
	int i;
	uint16_t pcie_speed, max_speed = 0;

	for (i = 0; i < state->performance_level_count; i++) {
		pcie_speed = state->performance_levels[i].pcie_gen;
		if (max_speed < pcie_speed)
			max_speed = pcie_speed;
	}

	return (enum rdn_ci_pcie_gen)max_speed;
}

/*
 * ci_request_link_speed_change_before_state_change(): Linux asks the
 * platform's ACPI for a faster link here (radeon_acpi_pcie_performance_request)
 * and, without CONFIG_ACPI, falls to the default branch, which is what this
 * is. Never called while pcie_performance_request is false (it is: there is
 * no ACPI), kept for the order of ci_dpm_set_power_state().
 */
static void ci_request_link_speed_change_before_state_change(struct rdn_ci_dpm *dpm,
							     struct rdn_ci_rps *radeon_new_state,
							     struct rdn_ci_rps *radeon_current_state)
{
	struct ci_power_info *pi = &dpm->pi;
	enum rdn_ci_pcie_gen target_link_speed =
		ci_get_maximum_link_speed(radeon_new_state);
	enum rdn_ci_pcie_gen current_link_speed;

	if (pi->force_pcie_gen == RDN_CI_PCIE_GEN_INVALID)
		current_link_speed = ci_get_maximum_link_speed(radeon_current_state);
	else
		current_link_speed = pi->force_pcie_gen;

	pi->force_pcie_gen = RDN_CI_PCIE_GEN_INVALID;
	pi->pspp_notify_required = false;
	if (target_link_speed > current_link_speed) {
		switch (target_link_speed) {
		default:
			pi->force_pcie_gen = (enum rdn_ci_pcie_gen)ci_get_current_pcie_speed(dpm->card);
			break;
		}
	} else {
		if (target_link_speed < current_link_speed)
			pi->pspp_notify_required = true;
	}
}

static void ci_notify_link_speed_change_after_state_change(struct rdn_ci_dpm *dpm,
							   struct rdn_ci_rps *radeon_new_state,
							   struct rdn_ci_rps *radeon_current_state)
{
	struct ci_power_info *pi = &dpm->pi;
	enum rdn_ci_pcie_gen target_link_speed =
		ci_get_maximum_link_speed(radeon_new_state);
	uint8_t request;

	(void)radeon_current_state;
	if (pi->pspp_notify_required) {
		if (target_link_speed == RDN_CI_PCIE_GEN3)
			request = PCIE_PERF_REQ_PECI_GEN3;
		else if (target_link_speed == RDN_CI_PCIE_GEN2)
			request = PCIE_PERF_REQ_PECI_GEN2;
		else
			request = PCIE_PERF_REQ_PECI_GEN1;

		if ((request == PCIE_PERF_REQ_PECI_GEN1) &&
		    (ci_get_current_pcie_speed(dpm->card) > 0))
			return;

		/* Linux: radeon_acpi_pcie_performance_request(rdev, request, false) under CONFIG_ACPI; no ACPI here */
	}
}

static int ci_set_private_data_variables_based_on_pptable(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_dep_table *allowed_sclk_vddc_table =
		&dpm->dyn_state.vddc_dependency_on_sclk;
	struct rdn_ci_dep_table *allowed_mclk_vddc_table =
		&dpm->dyn_state.vddc_dependency_on_mclk;
	struct rdn_ci_dep_table *allowed_mclk_vddci_table =
		&dpm->dyn_state.vddci_dependency_on_mclk;

	if (allowed_sclk_vddc_table->count < 1)
		return -EINVAL;
	if (allowed_mclk_vddc_table->count < 1)
		return -EINVAL;
	if (allowed_mclk_vddci_table->count < 1)
		return -EINVAL;

	pi->min_vddc_in_pp_table = allowed_sclk_vddc_table->entries[0].v;
	pi->max_vddc_in_pp_table =
		allowed_sclk_vddc_table->entries[allowed_sclk_vddc_table->count - 1].v;

	pi->min_vddci_in_pp_table = allowed_mclk_vddci_table->entries[0].v;
	pi->max_vddci_in_pp_table =
		allowed_mclk_vddci_table->entries[allowed_mclk_vddci_table->count - 1].v;

	dpm->dyn_state.max_clock_voltage_on_ac.sclk =
		allowed_sclk_vddc_table->entries[allowed_sclk_vddc_table->count - 1].clk;
	/* Linux indexes the mclk table by the sclk table's count here; kept, with the bound Linux lacks */
	if (allowed_sclk_vddc_table->count <= allowed_mclk_vddc_table->count)
		dpm->dyn_state.max_clock_voltage_on_ac.mclk =
			allowed_mclk_vddc_table->entries[allowed_sclk_vddc_table->count - 1].clk;
	else
		dpm->dyn_state.max_clock_voltage_on_ac.mclk =
			allowed_mclk_vddc_table->entries[allowed_mclk_vddc_table->count - 1].clk;
	dpm->dyn_state.max_clock_voltage_on_ac.vddc =
		allowed_sclk_vddc_table->entries[allowed_sclk_vddc_table->count - 1].v;
	dpm->dyn_state.max_clock_voltage_on_ac.vddci =
		allowed_mclk_vddci_table->entries[allowed_mclk_vddci_table->count - 1].v;

	return 0;
}

/* ---- patching the tables with the leakage voltages ---- */

static void ci_patch_with_vddc_leakage(struct rdn_ci_dpm *dpm, uint16_t *vddc)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_leakage_voltage *leakage_table = &pi->vddc_leakage;
	uint32_t leakage_index;

	for (leakage_index = 0; leakage_index < leakage_table->count; leakage_index++) {
		if (leakage_table->leakage_id[leakage_index] == *vddc) {
			*vddc = leakage_table->actual_voltage[leakage_index];
			break;
		}
	}
}

static void ci_patch_with_vddci_leakage(struct rdn_ci_dpm *dpm, uint16_t *vddci)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_leakage_voltage *leakage_table = &pi->vddci_leakage;
	uint32_t leakage_index;

	for (leakage_index = 0; leakage_index < leakage_table->count; leakage_index++) {
		if (leakage_table->leakage_id[leakage_index] == *vddci) {
			*vddci = leakage_table->actual_voltage[leakage_index];
			break;
		}
	}
}

static void ci_patch_clock_voltage_dependency_table_with_vddc_leakage(struct rdn_ci_dpm *dpm,
								      struct rdn_ci_dep_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddc_leakage(dpm, &table->entries[i].v);
	}
}

static void ci_patch_clock_voltage_dependency_table_with_vddci_leakage(struct rdn_ci_dpm *dpm,
								       struct rdn_ci_dep_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddci_leakage(dpm, &table->entries[i].v);
	}
}

static void ci_patch_vce_clock_voltage_dependency_table_with_vddc_leakage(struct rdn_ci_dpm *dpm,
									  struct rdn_ci_vce_dep_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddc_leakage(dpm, &table->entries[i].v);
	}
}

static void ci_patch_uvd_clock_voltage_dependency_table_with_vddc_leakage(struct rdn_ci_dpm *dpm,
									  struct rdn_ci_uvd_dep_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddc_leakage(dpm, &table->entries[i].v);
	}
}

static void ci_patch_vddc_phase_shed_limit_table_with_vddc_leakage(struct rdn_ci_dpm *dpm,
								   struct rdn_ci_phase_shedding_limits_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddc_leakage(dpm, &table->entries[i].voltage);
	}
}

static void ci_patch_clock_voltage_limits_with_vddc_leakage(struct rdn_ci_dpm *dpm,
							    struct rdn_ci_clock_and_voltage_limits *table)
{
	if (table) {
		ci_patch_with_vddc_leakage(dpm, &table->vddc);
		ci_patch_with_vddci_leakage(dpm, &table->vddci);
	}
}

static void ci_patch_cac_leakage_table_with_vddc_leakage(struct rdn_ci_dpm *dpm,
							 struct rdn_ci_cac_leakage_table *table)
{
	uint32_t i;

	if (table) {
		for (i = 0; i < table->count; i++)
			ci_patch_with_vddc_leakage(dpm, &table->entries[i].vddc);
	}
}

static void ci_patch_dependency_tables_with_leakage(struct rdn_ci_dpm *dpm)
{

	ci_patch_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								  &dpm->dyn_state.vddc_dependency_on_sclk);
	ci_patch_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								  &dpm->dyn_state.vddc_dependency_on_mclk);
	ci_patch_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								  &dpm->dyn_state.vddc_dependency_on_dispclk);
	ci_patch_clock_voltage_dependency_table_with_vddci_leakage(dpm,
								   &dpm->dyn_state.vddci_dependency_on_mclk);
	ci_patch_vce_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								      &dpm->dyn_state.vce_clock_voltage_dependency_table);
	ci_patch_uvd_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								      &dpm->dyn_state.uvd_clock_voltage_dependency_table);
	ci_patch_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								  &dpm->dyn_state.samu_clock_voltage_dependency_table);
	ci_patch_clock_voltage_dependency_table_with_vddc_leakage(dpm,
								  &dpm->dyn_state.acp_clock_voltage_dependency_table);
	ci_patch_vddc_phase_shed_limit_table_with_vddc_leakage(dpm,
							       &dpm->dyn_state.phase_shedding_limits_table);
	ci_patch_clock_voltage_limits_with_vddc_leakage(dpm,
							&dpm->dyn_state.max_clock_voltage_on_ac);
	ci_patch_clock_voltage_limits_with_vddc_leakage(dpm,
							&dpm->dyn_state.max_clock_voltage_on_dc);
	ci_patch_cac_leakage_table_with_vddc_leakage(dpm,
						     &dpm->dyn_state.cac_leakage_table);

}

static void ci_get_memory_type(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;

	tmp = RREG32(dpm->card, MC_SEQ_MISC0);

	if (((tmp & MC_SEQ_MISC0_GDDR5_MASK) >> MC_SEQ_MISC0_GDDR5_SHIFT) ==
	    MC_SEQ_MISC0_GDDR5_VALUE)
		pi->mem_gddr5 = true;
	else
		pi->mem_gddr5 = false;

}

/* ---- the current and requested states ---- */

static void ci_update_current_ps(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *rps)
{
	struct ci_power_info *pi = &dpm->pi;

	pi->current_rps = *rps;
}

static void ci_update_requested_ps(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *rps)
{
	struct ci_power_info *pi = &dpm->pi;

	pi->requested_rps = *rps;
}

static int ci_dpm_pre_set_power_state(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *requested)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_rps requested_ps = *requested;
	struct rdn_ci_rps *new_ps = &requested_ps;

	ci_update_requested_ps(dpm, new_ps);

	ci_apply_state_adjust_rules(dpm, &pi->requested_rps);

	return 0;
}

static void ci_dpm_post_set_power_state(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_rps *new_ps = &pi->requested_rps;

	ci_update_current_ps(dpm, new_ps);
}

/* ci_dpm_setup_asic() less ci_mc_load_microcode(), which is the caller's rdn_si_mc_load_microcode() */
static void ci_dpm_setup_asic(struct rdn_ci_dpm *dpm)
{
	ci_read_clock_registers(dpm);
	ci_get_memory_type(dpm);
	ci_enable_acpi_power_management(dpm);
	ci_init_sclk_t(dpm);
}

/*
 * Firebrand: ci_dpm_enable is cut in two at ci_dpm_start_smc. The first half
 * writes the CG registers Linux writes before the upload, loads the SMC image
 * and every table into the SMC's SRAM and leaves the SMC in reset; nothing
 * has changed a clock or a voltage yet. The second half starts the SMC and
 * DPM. rdn_ci_dpm_enable runs both, as before; rdn_ci_dpm_upload and
 * rdn_ci_dpm_start let the bring-up compare the SRAM with Linux radeon's on
 * the same card between the two (docs/cik-bring-up.md).
 */
static int ci_dpm_enable_upload(struct rdn_ci_dpm *dpm, const void *smc_fw, size_t smc_size)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	if (ci_is_smc_running(dpm->card)) {
		ERR(dpm, "ci_dpm: the SMC is already running; nothing enabled (rdn_ci_dpm_disable() first)");
		return -EINVAL;
	}
	if (pi->voltage_control != CISLANDS_VOLTAGE_CONTROL_NONE) {
		ci_enable_voltage_control(dpm);
		ret = ci_construct_voltage_tables(dpm);
		if (ret) {
			ERR(dpm, "ci_construct_voltage_tables failed");
			return ret;
		}
	}
	if (pi->caps_dynamic_ac_timing) {
		ret = ci_initialize_mc_reg_table(dpm);
		if (ret)
			pi->caps_dynamic_ac_timing = false;
	}
	if (pi->dynamic_ss)
		ci_enable_spread_spectrum(dpm, true);
	if (pi->thermal_protection)
		ci_enable_thermal_protection(dpm, true);
	ci_program_sstp(dpm);
	ci_enable_display_gap(dpm);
	ci_program_vc(dpm);
	ret = ci_upload_firmware(dpm, smc_fw, smc_size);
	if (ret) {
		ERR(dpm, "ci_upload_firmware failed");
		return ret;
	}
	ret = ci_process_firmware_header(dpm);
	if (ret) {
		ERR(dpm, "ci_process_firmware_header failed");
		return ret;
	}
	ret = ci_initial_switch_from_arb_f0_to_f1(dpm);
	if (ret) {
		ERR(dpm, "ci_initial_switch_from_arb_f0_to_f1 failed");
		return ret;
	}
	ret = ci_init_smc_table(dpm);
	if (ret) {
		ERR(dpm, "ci_init_smc_table failed");
		return ret;
	}
	ret = ci_init_arb_table_index(dpm);
	if (ret) {
		ERR(dpm, "ci_init_arb_table_index failed");
		return ret;
	}
	if (pi->caps_dynamic_ac_timing) {
		ret = ci_populate_initial_mc_reg_table(dpm);
		if (ret) {
			ERR(dpm, "ci_populate_initial_mc_reg_table failed");
			return ret;
		}
	}
	ret = ci_populate_pm_base(dpm);
	if (ret) {
		ERR(dpm, "ci_populate_pm_base failed");
		return ret;
	}
	return 0;
}

static int ci_dpm_enable_start(struct rdn_ci_dpm *dpm)
{
	struct rdn_ci_rps *boot_ps = &dpm->ps[dpm->boot_ps];
	int ret;

	ci_dpm_start_smc(dpm);
	ci_enable_vr_hot_gpio_interrupt(dpm);
	ret = ci_notify_smc_display_change(dpm, false);
	if (ret) {
		ERR(dpm, "ci_notify_smc_display_change failed");
		return ret;
	}
	ci_enable_sclk_control(dpm, true);
	ret = ci_enable_ulv(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_ulv failed");
		return ret;
	}
	ret = ci_enable_ds_master_switch(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_ds_master_switch failed");
		return ret;
	}
	ret = ci_start_dpm(dpm);
	if (ret) {
		ERR(dpm, "ci_start_dpm failed");
		return ret;
	}
	ret = ci_enable_didt(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_didt failed");
		return ret;
	}
	ret = ci_enable_smc_cac(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_smc_cac failed");
		return ret;
	}
	ret = ci_enable_power_containment(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_power_containment failed");
		return ret;
	}

	ret = ci_power_control_set_level(dpm);
	if (ret) {
		ERR(dpm, "ci_power_control_set_level failed");
		return ret;
	}

	ci_enable_auto_throttle_source(dpm, RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL, true);

	ret = ci_enable_thermal_based_sclk_dpm(dpm, true);
	if (ret) {
		ERR(dpm, "ci_enable_thermal_based_sclk_dpm failed");
		return ret;
	}

	ci_thermal_start_thermal_controller(dpm);

	ci_update_current_ps(dpm, boot_ps);

	return 0;
}

static int ci_set_temperature_range(struct rdn_ci_dpm *dpm)
{
	int ret;

	ret = ci_thermal_enable_alert(dpm, false);
	if (ret)
		return ret;
	ret = ci_thermal_set_temperature_range(dpm, R600_TEMP_RANGE_MIN, R600_TEMP_RANGE_MAX);
	if (ret)
		return ret;
	ret = ci_thermal_enable_alert(dpm, true);
	if (ret)
		return ret;

	return ret;
}

/* ci_dpm_late_enable(): the temperature range; Linux wires its thermal interrupt here, there is none */
static int ci_dpm_late_enable(struct rdn_ci_dpm *dpm)
{
	int ret;

	ret = ci_set_temperature_range(dpm);
	if (ret)
		return ret;

	ci_dpm_powergate_uvd(dpm, true);

	return 0;
}

int rdn_ci_dpm_upload(struct rdn_ci_dpm *dpm, const void *smc_fw, size_t smc_size)
{
	int ret;

	if (dpm->boot_ps < 0 || !dpm->ps)
		return -EINVAL;
	ci_dpm_setup_asic(dpm);
	ret = ci_dpm_enable_upload(dpm, smc_fw, smc_size);
	if (ret)
		return ret;
	dpm->uploaded = true;
	INFO(dpm, "ci_dpm: SMC image and tables in the SMC's SRAM (DpmTable 0x%05x, SoftRegisters 0x%05x, "
	     "mcRegisterTable 0x%05x, mcArbDramTimingTable 0x%05x, FanTable 0x%05x); the SMC is held in reset",
	     dpm->pi.dpm_table_start, dpm->pi.soft_regs_start, dpm->pi.mc_reg_table_start,
	     dpm->pi.arb_table_start, dpm->pi.fan_table_start);
	return 0;
}

int rdn_ci_dpm_start(struct rdn_ci_dpm *dpm)
{
	int ret;

	if (!dpm->uploaded)
		return -EINVAL;
	ret = ci_dpm_enable_start(dpm);
	if (ret)
		return ret;
	dpm->enabled = true;
	/* radeon_dpm_enable(): current = requested = boot */
	ci_update_requested_ps(dpm, &dpm->ps[dpm->boot_ps]);
	ret = ci_dpm_late_enable(dpm);
	if (ret)
		return ret;
	INFO(dpm, "ci_dpm: enabled; %s memory, voltage control %s, dynamic ac timing %s, firmware fan control %s",
	     dpm->pi.mem_gddr5 ? "GDDR5" : "non-GDDR5",
	     dpm->pi.voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_SVID2 ? "SVID2" :
	     dpm->pi.voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO ? "GPIO" : "none",
	     dpm->pi.caps_dynamic_ac_timing ? "on" : "off",
	     dpm->fan.ucode_fan_control ? "on" : "off");
	return 0;
}

int rdn_ci_dpm_enable(struct rdn_ci_dpm *dpm, const void *smc_fw, size_t smc_size)
{
	int ret = rdn_ci_dpm_upload(dpm, smc_fw, smc_size);

	return ret ? ret : rdn_ci_dpm_start(dpm);
}

int rdn_ci_dpm_disable(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_rps *boot_ps;

	if (dpm->boot_ps < 0 || !dpm->ps)
		return -EINVAL;
	boot_ps = &dpm->ps[dpm->boot_ps];

	ci_dpm_powergate_uvd(dpm, false);

	if (!ci_is_smc_running(dpm->card)) {
		dpm->enabled = false;
		return 0;
	}

	ci_thermal_stop_thermal_controller(dpm);

	if (pi->thermal_protection)
		ci_enable_thermal_protection(dpm, false);
	ci_enable_power_containment(dpm, false);
	ci_enable_smc_cac(dpm, false);
	ci_enable_didt(dpm, false);
	ci_enable_spread_spectrum(dpm, false);
	ci_enable_auto_throttle_source(dpm, RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL, false);
	ci_stop_dpm(dpm);
	ci_enable_ds_master_switch(dpm, false);
	ci_enable_ulv(dpm, false);
	ci_clear_vc(dpm);
	/* ci_reset_to_default() */
	ci_send_msg_to_smc(dpm, PPSMC_MSG_ResetToDefaults);
	ci_dpm_stop_smc(dpm);
	ci_force_switch_to_arb_f0(dpm);
	ci_enable_thermal_based_sclk_dpm(dpm, false);

	ci_update_current_ps(dpm, boot_ps);
	dpm->enabled = false;
	dpm->uploaded = false;
	return 0;
}

static int ci_dpm_set_power_state(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_rps *new_ps = &pi->requested_rps;
	struct rdn_ci_rps *old_ps = &pi->current_rps;
	int ret;

	ci_find_dpm_states_clocks_in_dpm_table(dpm, new_ps);
	if (pi->pcie_performance_request)
		ci_request_link_speed_change_before_state_change(dpm, new_ps, old_ps);
	ret = ci_freeze_sclk_mclk_dpm(dpm);
	if (ret) {
		ERR(dpm, "ci_freeze_sclk_mclk_dpm failed");
		return ret;
	}
	ret = ci_populate_and_upload_sclk_mclk_dpm_levels(dpm, new_ps);
	if (ret) {
		ERR(dpm, "ci_populate_and_upload_sclk_mclk_dpm_levels failed");
		return ret;
	}
	ret = ci_generate_dpm_level_enable_mask(dpm, new_ps);
	if (ret) {
		ERR(dpm, "ci_generate_dpm_level_enable_mask failed");
		return ret;
	}

	ret = ci_update_vce_dpm(dpm, new_ps, old_ps);
	if (ret) {
		ERR(dpm, "ci_update_vce_dpm failed");
		return ret;
	}

	ret = ci_update_sclk_t(dpm);
	if (ret) {
		ERR(dpm, "ci_update_sclk_t failed");
		return ret;
	}
	if (pi->caps_dynamic_ac_timing) {
		ret = ci_update_and_upload_mc_reg_table(dpm);
		if (ret) {
			ERR(dpm, "ci_update_and_upload_mc_reg_table failed");
			return ret;
		}
	}
	ret = ci_program_memory_timing_parameters(dpm);
	if (ret) {
		ERR(dpm, "ci_program_memory_timing_parameters failed");
		return ret;
	}
	ret = ci_unfreeze_sclk_mclk_dpm(dpm);
	if (ret) {
		ERR(dpm, "ci_unfreeze_sclk_mclk_dpm failed");
		return ret;
	}
	ret = ci_upload_dpm_level_enable_mask(dpm);
	if (ret) {
		ERR(dpm, "ci_upload_dpm_level_enable_mask failed");
		return ret;
	}
	if (pi->pcie_performance_request)
		ci_notify_link_speed_change_after_state_change(dpm, new_ps, old_ps);

	return 0;
}

/*
 * radeon_dpm_pick_power_state() for a desktop card on AC: the UI class asked
 * for, falling back battery -> balanced -> performance as Linux does. Linux
 * then gives up; here the state with the highest engine clock stands in,
 * because a VBIOS without a performance-classed state would otherwise leave
 * DPM at the boot clocks.
 */
static int ci_pick_power_state(struct rdn_ci_dpm *dpm, enum rdn_ci_power_state state)
{
	unsigned i;
	int best = -1;
	unsigned want;

	if (state == RDN_CI_STATE_BOOT)
		return dpm->boot_ps;
	while (1) {
		want = state == RDN_CI_STATE_BATTERY ? ATOM_PPLIB_CLASSIFICATION_UI_BATTERY :
		       state == RDN_CI_STATE_BALANCED ? ATOM_PPLIB_CLASSIFICATION_UI_BALANCED :
		       ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE;
		for (i = 0; i < dpm->num_ps; i++) {
			if (dpm->ps[i].classification & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
				continue;
			if ((dpm->ps[i].classification & ATOM_PPLIB_CLASSIFICATION_UI_MASK) == want)
				return (int)i;
		}
		if (state == RDN_CI_STATE_BATTERY)
			state = RDN_CI_STATE_BALANCED;
		else if (state == RDN_CI_STATE_BALANCED)
			state = RDN_CI_STATE_PERFORMANCE;
		else
			break;
	}
	for (i = 0; i < dpm->num_ps; i++) {
		struct ci_ps *ps = &dpm->ps[i].ps;

		if ((int)i == dpm->boot_ps || ps->performance_level_count == 0)
			continue;
		if (dpm->ps[i].classification & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
			continue;
		if (best < 0 || ps->performance_levels[ps->performance_level_count - 1].sclk >
		    dpm->ps[best].ps.performance_levels[dpm->ps[best].ps.performance_level_count - 1].sclk)
			best = (int)i;
	}
	if (best >= 0)
		INFO(dpm, "ci_dpm: no performance-classed power state; using state %d (the highest engine clock)", best);
	return best >= 0 ? best : dpm->boot_ps;
}

int rdn_ci_dpm_set_power_state(struct rdn_ci_dpm *dpm, enum rdn_ci_power_state state)
{
	int idx, ret;

	if (!dpm->enabled)
		return -EINVAL;
	idx = ci_pick_power_state(dpm, state);
	if (idx < 0)
		return -EINVAL;

	/* radeon_dpm_change_power_state_locked() */
	ret = ci_dpm_pre_set_power_state(dpm, &dpm->ps[idx]);
	if (ret)
		return ret;
	ret = ci_dpm_set_power_state(dpm);
	if (ret)
		return ret;
	ci_dpm_post_set_power_state(dpm);
	dpm->current_active_crtc_count = dpm->new_active_crtc_count;
	dpm->last_power_state = (int)state;

	/* Linux re-applies a forced level after each switch */
	if (dpm->forced_level != RDN_CI_FORCED_LEVEL_AUTO)
		return rdn_ci_dpm_force_performance_level(dpm, dpm->forced_level);
	return 0;
}

void rdn_ci_dpm_display_configuration_changed(struct rdn_ci_dpm *dpm, unsigned num_displays,
					      uint32_t vrefresh, uint32_t vblank_time_us)
{
	dpm->new_active_crtc_count = num_displays;
	dpm->vrefresh = vrefresh;
	dpm->vblank_time_us = vblank_time_us;
	if (dpm->enabled)
		ci_program_display_gap(dpm);
}

/* ---- init and fini ---- */

static void ci_free_all(struct rdn_ci_dpm *dpm)
{
	struct rdn_os *os = dpm->card->os;

	ci_free_power_table(dpm);
	if (dpm->dyn_state.vddc_dependency_on_dispclk.entries) {
		os->free(os->cookie, dpm->dyn_state.vddc_dependency_on_dispclk.entries);
		dpm->dyn_state.vddc_dependency_on_dispclk.entries = NULL;
	}
	r600_free_extended_power_table(dpm);
}

void rdn_ci_dpm_fini(struct rdn_ci_dpm *dpm)
{
	if (!dpm->card)
		return;
	ci_free_all(dpm);
	dpm->card = NULL;
}

int rdn_ci_dpm_init(struct rdn_ci_dpm *dpm, struct rdn_card *card, unsigned sys_pcie_mask)
{
	SMU7_Discrete_DpmTable  *dpm_table;
	struct radeon_gpio_rec gpio;
	struct ci_power_info *pi;
	int ret;

	memset(dpm, 0, sizeof(*dpm));
	dpm->card = card;
	dpm->ac_power = true;
	dpm->boot_ps = -1;
	dpm->uvd_ps = -1;
	dpm->vblank_time_us = 0xffffffff;	/* displays off until told otherwise */
	dpm->new_active_crtc_count = 0;
	dpm->last_power_state = -1;
	pi = &dpm->pi;

	if (card->family != RDN_FAMILY_CIK) {
		ERR(dpm, "ci_dpm: not a Sea Islands card");
		return -EINVAL;
	}

	ret = ci_parse_firmware_info(dpm);
	if (ret) {
		ERR(dpm, "ci_dpm: FirmwareInfo not understood");
		return ret;
	}

	/*
	 * Linux asks the PCI core for the root port's speed capability and
	 * sets sys_pcie_mask from it; here the caller says.
	 */
	pi->sys_pcie_mask = sys_pcie_mask;
	pi->force_pcie_gen = RDN_CI_PCIE_GEN_INVALID;

	pi->pcie_gen_performance.max = RDN_CI_PCIE_GEN1;
	pi->pcie_gen_performance.min = RDN_CI_PCIE_GEN3;
	pi->pcie_gen_powersaving.max = RDN_CI_PCIE_GEN1;
	pi->pcie_gen_powersaving.min = RDN_CI_PCIE_GEN3;

	pi->pcie_lane_performance.max = 0;
	pi->pcie_lane_performance.min = 16;
	pi->pcie_lane_powersaving.max = 0;
	pi->pcie_lane_powersaving.min = 16;

	ret = ci_get_vbios_boot_values(dpm, &pi->vbios_boot_state);
	if (ret)
		return ret;

	ret = r600_get_platform_caps(dpm);
	if (ret)
		return ret;

	ret = r600_parse_extended_power_table(dpm);
	if (ret)
		return ret;

	ret = ci_parse_power_table(dpm);
	if (ret) {
		r600_free_extended_power_table(dpm);
		return ret;
	}

	pi->dll_default_on = false;
	pi->sram_end = SMC_RAM_END;

	pi->activity_target[0] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[1] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[2] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[3] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[4] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[5] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[6] = CISLAND_TARGETACTIVITY_DFLT;
	pi->activity_target[7] = CISLAND_TARGETACTIVITY_DFLT;

	pi->mclk_activity_target = CISLAND_MCLK_TARGETACTIVITY_DFLT;

	pi->sclk_dpm_key_disabled = 0;
	pi->mclk_dpm_key_disabled = 0;
	/*
	 * PCIe DPM stays off on this port, as a precaution. With it on the
	 * SMU retrains the link between its link levels as the load changes,
	 * and a read that reaches the card mid-retrain fails; a failed PCIe
	 * completion on this part is an uncorrectable machine check, not a
	 * read of all ones as on x86. No panic has actually been pinned on a
	 * retrain (the 2026-11-04 machine checks that first looked like one
	 * were `storedump`'s file write in copypv/bcopy_phys32 and recurred
	 * with DPM off -- KNOWN-ISSUES 21), but the link is best left as the
	 * firmware trained it (gen2 x4 here). Engine and memory DPM are
	 * unaffected.
	 */
	pi->pcie_dpm_key_disabled = 1;
	pi->thermal_sclk_dpm_enabled = 0;

	/*
	 * Linux: "mclk dpm is unstable on some R7 260X cards with the old mc
	 * ucode" and disables it for 0x6658 when the MC firmware is the old
	 * size. The MC firmware's size is not known here; nothing is disabled.
	 */

	pi->caps_sclk_ds = true;

	pi->mclk_strobe_mode_threshold = 40000;
	pi->mclk_stutter_mode_threshold = 40000;
	pi->mclk_edc_enable_threshold = 40000;
	pi->mclk_edc_wr_enable_threshold = 40000;

	ci_initialize_powertune_defaults(dpm);

	pi->caps_fps = false;

	pi->caps_sclk_throttle_low_notification = false;

	pi->caps_uvd_dpm = true;
	pi->caps_vce_dpm = true;

	ci_get_leakage_voltages(dpm);
	ci_patch_dependency_tables_with_leakage(dpm);
	ci_set_private_data_variables_based_on_pptable(dpm);

	dpm->dyn_state.vddc_dependency_on_dispclk.entries =
		card->os->alloc(card->os->cookie, 4 * sizeof(struct rdn_ci_dep_entry));
	if (!dpm->dyn_state.vddc_dependency_on_dispclk.entries) {
		ci_free_all(dpm);
		return -ENOMEM;
	}
	dpm->dyn_state.vddc_dependency_on_dispclk.count = 4;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[0].clk = 0;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[0].v = 0;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[1].clk = 36000;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[1].v = 720;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[2].clk = 54000;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[2].v = 810;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[3].clk = 72000;
	dpm->dyn_state.vddc_dependency_on_dispclk.entries[3].v = 900;

	dpm->dyn_state.mclk_sclk_ratio = 4;
	dpm->dyn_state.sclk_mclk_delta = 15000;
	dpm->dyn_state.vddc_vddci_delta = 200;

	if (card->chip && card->chip->chip == RDN_CHIP_HAWAII) {
		pi->thermal_temp_setting.temperature_low = 94500;
		pi->thermal_temp_setting.temperature_high = 95000;
		pi->thermal_temp_setting.temperature_shutdown = 104000;
	} else {
		pi->thermal_temp_setting.temperature_low = 99500;
		pi->thermal_temp_setting.temperature_high = 100000;
		pi->thermal_temp_setting.temperature_shutdown = 104000;
	}

	pi->uvd_enabled = false;

	dpm_table = &pi->smc_state_table;

	gpio = radeon_atombios_lookup_gpio(card, VDDC_VRHOT_GPIO_PINID);
	if (gpio.valid) {
		dpm_table->VRHotGpio = gpio.shift;
		dpm->platform_caps |= ATOM_PP_PLATFORM_CAP_REGULATOR_HOT;
	} else {
		dpm_table->VRHotGpio = CISLANDS_UNUSED_GPIO_PIN;
		dpm->platform_caps &= ~ATOM_PP_PLATFORM_CAP_REGULATOR_HOT;
	}

	gpio = radeon_atombios_lookup_gpio(card, PP_AC_DC_SWITCH_GPIO_PINID);
	if (gpio.valid) {
		dpm_table->AcDcGpio = gpio.shift;
		dpm->platform_caps |= ATOM_PP_PLATFORM_CAP_HARDWAREDC;
	} else {
		dpm_table->AcDcGpio = CISLANDS_UNUSED_GPIO_PIN;
		dpm->platform_caps &= ~ATOM_PP_PLATFORM_CAP_HARDWAREDC;
	}

	gpio = radeon_atombios_lookup_gpio(card, VDDC_PCC_GPIO_PINID);
	if (gpio.valid) {
		uint32_t tmp = RREG32_SMC(card, CNB_PWRMGT_CNTL);

		switch (gpio.shift) {
		case 0:
			tmp &= ~GNB_SLOW_MODE_MASK;
			tmp |= GNB_SLOW_MODE(1);
			break;
		case 1:
			tmp &= ~GNB_SLOW_MODE_MASK;
			tmp |= GNB_SLOW_MODE(2);
			break;
		case 2:
			tmp |= GNB_SLOW;
			break;
		case 3:
			tmp |= FORCE_NB_PS1;
			break;
		case 4:
			tmp |= DPM_ENABLED;
			break;
		default:
			INFO(dpm, "Invalid PCC GPIO: %u!", gpio.shift);
			break;
		}
		WREG32_SMC(card, CNB_PWRMGT_CNTL, tmp);
	}

	pi->voltage_control = CISLANDS_VOLTAGE_CONTROL_NONE;
	pi->vddci_control = CISLANDS_VOLTAGE_CONTROL_NONE;
	pi->mvdd_control = CISLANDS_VOLTAGE_CONTROL_NONE;
	if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_GPIO_LUT))
		pi->voltage_control = CISLANDS_VOLTAGE_CONTROL_BY_GPIO;
	else if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_VDDC, VOLTAGE_OBJ_SVID2))
		pi->voltage_control = CISLANDS_VOLTAGE_CONTROL_BY_SVID2;

	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_VDDCI_CONTROL) {
		if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_GPIO_LUT))
			pi->vddci_control = CISLANDS_VOLTAGE_CONTROL_BY_GPIO;
		else if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_VDDCI, VOLTAGE_OBJ_SVID2))
			pi->vddci_control = CISLANDS_VOLTAGE_CONTROL_BY_SVID2;
		else
			dpm->platform_caps &= ~ATOM_PP_PLATFORM_CAP_VDDCI_CONTROL;
	}

	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_MVDDCONTROL) {
		if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_GPIO_LUT))
			pi->mvdd_control = CISLANDS_VOLTAGE_CONTROL_BY_GPIO;
		else if (radeon_atom_is_voltage_gpio(card, VOLTAGE_TYPE_MVDDC, VOLTAGE_OBJ_SVID2))
			pi->mvdd_control = CISLANDS_VOLTAGE_CONTROL_BY_SVID2;
		else
			dpm->platform_caps &= ~ATOM_PP_PLATFORM_CAP_MVDDCONTROL;
	}

	pi->vddc_phase_shed_control = true;

	/* Linux: radeon_acpi_is_pcie_performance_request_supported(); no ACPI here */
	pi->pcie_performance_request = false;

	if (radeon_atombios_has_asic_ss_info(card)) {
		pi->caps_sclk_ss_support = true;
		pi->caps_mclk_ss_support = true;
		pi->dynamic_ss = true;
	} else {
		pi->caps_sclk_ss_support = false;
		pi->caps_mclk_ss_support = false;
		pi->dynamic_ss = true;
	}

	/* rdev->pm.int_thermal_type != THERMAL_TYPE_NONE: any thermal controller in the PowerPlay table */
	if (dpm->thermal_controller_type != 0)
		pi->thermal_protection = true;
	else
		pi->thermal_protection = false;

	pi->caps_dynamic_ac_timing = true;

	pi->uvd_power_gated = false;

	/* make sure dc limits are valid */
	if ((dpm->dyn_state.max_clock_voltage_on_dc.sclk == 0) ||
	    (dpm->dyn_state.max_clock_voltage_on_dc.mclk == 0))
		dpm->dyn_state.max_clock_voltage_on_dc =
			dpm->dyn_state.max_clock_voltage_on_ac;

	pi->fan_ctrl_is_in_default_mode = true;

	if (dpm->boot_ps < 0) {
		ERR(dpm, "ci_dpm: the PowerPlay table has no boot state");
		ci_free_all(dpm);
		return -EINVAL;
	}

	return 0;
}

/* ---- readouts ---- */

uint32_t rdn_ci_dpm_get_sclk(struct rdn_ci_dpm *dpm)
{
	uint32_t sclk = dpm->enabled ? ci_get_average_sclk_freq(dpm) : 0;
	struct ci_ps *requested_state = &dpm->pi.requested_rps.ps;

	if (sclk)
		return sclk;
	/* ci_dpm_get_sclk(rdev, false) */
	if (requested_state->performance_level_count)
		return requested_state->performance_levels[requested_state->performance_level_count - 1].sclk;
	return dpm->pi.vbios_boot_state.sclk_bootup_value;
}

uint32_t rdn_ci_dpm_get_mclk(struct rdn_ci_dpm *dpm)
{
	uint32_t mclk = dpm->enabled ? ci_get_average_mclk_freq(dpm) : 0;
	struct ci_ps *requested_state = &dpm->pi.requested_rps.ps;

	if (mclk)
		return mclk;
	/* ci_dpm_get_mclk(rdev, false) */
	if (requested_state->performance_level_count)
		return requested_state->performance_levels[requested_state->performance_level_count - 1].mclk;
	return dpm->pi.vbios_boot_state.mclk_bootup_value;
}

/* ci_get_temp() (cik.c) */
int rdn_ci_dpm_temperature(struct rdn_ci_dpm *dpm)
{
	uint32_t temp;
	int actual_temp = 0;

	temp = (RREG32_SMC(dpm->card, CG_MULT_THERMAL_STATUS) & CTF_TEMP_MASK) >>
		CTF_TEMP_SHIFT;

	if (temp & 0x200)
		actual_temp = 255;
	else
		actual_temp = temp & 0x1ff;

	actual_temp = actual_temp * 1000;

	return actual_temp;
}

/* ---- describe ---- */

static const char *ci_voltage_control_name(uint32_t control)
{
	switch (control) {
	case CISLANDS_VOLTAGE_CONTROL_BY_GPIO:
		return "GPIO";
	case CISLANDS_VOLTAGE_CONTROL_BY_SVID2:
		return "SVID2";
	default:
		return "none";
	}
}

static void ci_describe_dep_table(struct rdn_ci_dpm *dpm, const char *name, struct rdn_ci_dep_table *t)
{
	uint32_t i;

	INFO(dpm, "ci_dpm:   %s: %u entries", name, t->count);
	for (i = 0; i < t->count; i++)
		INFO(dpm, "ci_dpm:     [%u] clk %u0 kHz, %u mV", i, t->entries[i].clk, t->entries[i].v);
}

void rdn_ci_dpm_describe(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_dyn_state *ds = &dpm->dyn_state;
	unsigned i, j;

	INFO(dpm, "ci_dpm: %s (0x%04x), reference clock %u0 kHz, firmware flags 0x%04x, platform caps 0x%08x",
	     dpm->card->chip ? dpm->card->chip->name : "?", dpm->card->device,
	     dpm->reference_freq, dpm->firmware_flags, dpm->platform_caps);
	/*
	 * The CAC leakage table and the vddc(sclk) dependency table feed
	 * ci_get_std_voltage_value_sidd(); the Linux session of 2026-11-04 put
	 * StdVoltageHiSidd 19 mV below this driver's for every level, so both
	 * inputs are logged to be compared entry by entry.
	 */
	for (i = 0; i < ds->cac_leakage_table.count; i++)
		INFO(dpm, "ci_dpm: cac leakage[%u]: vddc %u leakage 0x%08x (vddc1 %u vddc2 %u vddc3 %u)", i,
		     ds->cac_leakage_table.entries[i].vddc, ds->cac_leakage_table.entries[i].leakage,
		     ds->cac_leakage_table.entries[i].vddc1, ds->cac_leakage_table.entries[i].vddc2,
		     ds->cac_leakage_table.entries[i].vddc3);
	for (i = 0; i < ds->vddc_dependency_on_sclk.count; i++)
		INFO(dpm, "ci_dpm: vddc(sclk)[%u]: %u0 kHz -> %u mV", i,
		     ds->vddc_dependency_on_sclk.entries[i].clk, ds->vddc_dependency_on_sclk.entries[i].v);
	for (i = 0; i < pi->vddc_voltage_table.count; i++)
		INFO(dpm, "ci_dpm: vddc voltage table[%u]: %u mV smio_low 0x%08x", i,
		     pi->vddc_voltage_table.entries[i].value, pi->vddc_voltage_table.entries[i].smio_low);
	INFO(dpm, "ci_dpm: boot: sclk %u0 kHz, mclk %u0 kHz, vddc %u mV, vddci %u mV, mvdd %u mV, pcie gen %u x%u",
	     pi->vbios_boot_state.sclk_bootup_value, pi->vbios_boot_state.mclk_bootup_value,
	     pi->vbios_boot_state.vddc_bootup_value, pi->vbios_boot_state.vddci_bootup_value,
	     pi->vbios_boot_state.mvdd_bootup_value, pi->vbios_boot_state.pcie_gen_bootup_value + 1,
	     pi->vbios_boot_state.pcie_lane_bootup_value);
	INFO(dpm, "ci_dpm: voltage control: vddc %s, vddci %s, mvdd %s; leakage ids %u vddc, %u vddci; EVV %s; phase shedding %u limits",
	     ci_voltage_control_name(pi->voltage_control), ci_voltage_control_name(pi->vddci_control),
	     ci_voltage_control_name(pi->mvdd_control), pi->vddc_leakage.count, pi->vddci_leakage.count,
	     (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_EVV) ? "yes" : "no",
	     ds->phase_shedding_limits_table.count);
	for (i = 0; i < pi->vddc_leakage.count; i++)
		INFO(dpm, "ci_dpm:   vddc leakage id 0x%04x -> %u mV", pi->vddc_leakage.leakage_id[i], pi->vddc_leakage.actual_voltage[i]);
	INFO(dpm, "ci_dpm: thermal controller type %u, %s, tach %u pulses/rev, fan %u..%u00 rpm; backbias %u us, voltage %u us",
	     dpm->thermal_controller_type, dpm->no_fan ? "no fan" : "fan", dpm->fan_pulses_per_revolution,
	     dpm->fan_min_rpm, dpm->fan_max_rpm, dpm->backbias_response_time, dpm->voltage_response_time);
	if (dpm->fan.ucode_fan_control)
		INFO(dpm, "ci_dpm: fan table format %u: t_hyst %u, t_min %u, t_med %u, t_high %u, t_max %u (0.01 C), pwm %u/%u/%u (0.01 %%), mode %u, pwm max %u, sensitivity %u",
		     dpm->fan.table_format, dpm->fan.t_hyst, dpm->fan.t_min, dpm->fan.t_med, dpm->fan.t_high, dpm->fan.t_max,
		     dpm->fan.pwm_min, dpm->fan.pwm_med, dpm->fan.pwm_high, dpm->fan.control_mode,
		     dpm->fan.default_max_fan_pwm, dpm->fan.fan_output_sensitivity);
	else
		INFO(dpm, "ci_dpm: no fan table");
	INFO(dpm, "ci_dpm: %u power states, boot %d, uvd %d", dpm->num_ps, dpm->boot_ps, dpm->uvd_ps);
	for (i = 0; i < dpm->num_ps; i++) {
		struct rdn_ci_rps *rps = &dpm->ps[i];

		INFO(dpm, "ci_dpm:   state %u: class 0x%04x class2 0x%04x caps 0x%08x, %u levels", i,
		     rps->classification, rps->classification2, rps->caps, rps->ps.performance_level_count);
		for (j = 0; j < rps->ps.performance_level_count; j++) {
			struct ci_pl *pl = &rps->ps.performance_levels[j];

			INFO(dpm, "ci_dpm:     level %u: sclk %u0 kHz, mclk %u0 kHz, pcie gen %u x%u", j,
			     pl->sclk, pl->mclk, pl->pcie_gen + 1, pl->pcie_lane);
		}
	}
	ci_describe_dep_table(dpm, "vddc(sclk)", &ds->vddc_dependency_on_sclk);
	ci_describe_dep_table(dpm, "vddc(mclk)", &ds->vddc_dependency_on_mclk);
	ci_describe_dep_table(dpm, "vddci(mclk)", &ds->vddci_dependency_on_mclk);
	ci_describe_dep_table(dpm, "mvdd(mclk)", &ds->mvdd_dependency_on_mclk);
	INFO(dpm, "ci_dpm:   cac leakage: %u entries, uvd %u, vce %u, samu %u, acp %u", ds->cac_leakage_table.count,
	     ds->uvd_clock_voltage_dependency_table.count, ds->vce_clock_voltage_dependency_table.count,
	     ds->samu_clock_voltage_dependency_table.count, ds->acp_clock_voltage_dependency_table.count);
	for (i = 0; i < ds->cac_leakage_table.count; i++)
		INFO(dpm, "ci_dpm:     [%u] vddc %u, leakage %u (vddc1/2/3 %u/%u/%u)", i, ds->cac_leakage_table.entries[i].vddc,
		     ds->cac_leakage_table.entries[i].leakage, ds->cac_leakage_table.entries[i].vddc1,
		     ds->cac_leakage_table.entries[i].vddc2, ds->cac_leakage_table.entries[i].vddc3);
	INFO(dpm, "ci_dpm:   max on ac: sclk %u0 kHz, mclk %u0 kHz, vddc %u, vddci %u; on dc: sclk %u0, mclk %u0",
	     ds->max_clock_voltage_on_ac.sclk, ds->max_clock_voltage_on_ac.mclk, ds->max_clock_voltage_on_ac.vddc,
	     ds->max_clock_voltage_on_ac.vddci, ds->max_clock_voltage_on_dc.sclk, ds->max_clock_voltage_on_dc.mclk);
	if (ds->has_cac_tdp_table)
		INFO(dpm, "ci_dpm:   powertune: tdp %u, configurable %u, tdc %u, battery %u, small %u, cac leakage %u/%u, max delivery %u",
		     ds->cac_tdp_table.tdp, ds->cac_tdp_table.configurable_tdp, ds->cac_tdp_table.tdc,
		     ds->cac_tdp_table.battery_power_limit, ds->cac_tdp_table.small_power_limit,
		     ds->cac_tdp_table.low_cac_leakage, ds->cac_tdp_table.high_cac_leakage,
		     ds->cac_tdp_table.maximum_power_delivery_limit);
	else
		INFO(dpm, "ci_dpm:   no powertune table (power containment will fail its table reads)");
	if (ds->has_ppm_table)
		INFO(dpm, "ci_dpm:   ppm: design %u, dgpu tdp %u, tj max %u", ds->ppm_table.ppm_design, ds->ppm_table.dgpu_tdp, ds->ppm_table.tj_max);
	INFO(dpm, "ci_dpm: caps: power containment %u (bapm %u, tdc %u, pkg pwr %u), cac %u, sclk ds %u, ss %u/%u, dynamic ac timing %u, thermal protection %u, ulv %u (0x%08x)",
	     pi->caps_power_containment, pi->enable_bapm_feature, pi->enable_tdc_limit_feature, pi->enable_pkg_pwr_tracking_feature,
	     pi->caps_cac, pi->caps_sclk_ds, pi->caps_sclk_ss_support, pi->caps_mclk_ss_support, pi->caps_dynamic_ac_timing,
	     pi->thermal_protection, pi->ulv.supported, pi->ulv.cg_ulv_parameter);
	INFO(dpm, "ci_dpm: pcie: performance gen %u..%u x%u..%u (%u), powersaving gen %u..%u x%u..%u (%u), sys mask 0x%x",
	     pi->pcie_gen_performance.min + 1, pi->pcie_gen_performance.max + 1, pi->pcie_lane_performance.min,
	     pi->pcie_lane_performance.max, pi->use_pcie_performance_levels,
	     pi->pcie_gen_powersaving.min + 1, pi->pcie_gen_powersaving.max + 1, pi->pcie_lane_powersaving.min,
	     pi->pcie_lane_powersaving.max, pi->use_pcie_powersaving_levels, pi->sys_pcie_mask);
}
