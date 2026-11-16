/*
 * Sea Islands DPM: radeon's ci_dpm.c and ci_smc.c as RadeonNI ported and
 * verified them on this FirePro W8100 (amdgpu/si_dpm/rdn_ci_dpm.c,
 * rdn_ci_smu.c, rdn_cik_smc.c, rdn_ci_tables.c, carried from
 * RadeonNI's hw). Measured there (2026-11-04): with the PCIe link table and the leakage aliasing fixed, the
 * SMU's DPM table, PM fuses, MC arbiter and MC register tables equal Linux
 * radeon's on the card byte for byte but the state-dependent bytes; forced
 * high the firmware takes the engine to level 4 (824 MHz, profile index
 * 0x00841100, CG_SPLL_FUNC_CNTL_3 0x100f4250) as Linux's does.
 *
 * amdgpu drives CI through powerplay (smu7_hwmgr + ci_smumgr); radeon's
 * ci_dpm is the code RadeonNI verified, so it is the one carried (as SI
 * carries si_dpm). This file is the frb_pm.h interface's CIK arm over it.
 *
 * Where it runs: right after stage smu, before the display and the engines
 * (the user's call, 2026-11-15: a Hawaii die heats up at boot clocks with
 * the fan on the VBIOS's curve). amdgpu's IP order starts pp_smu after gfx
 * and sdma; radeon's ci_dpm needs neither (its DIDT, the only part that
 * enters RLC safe mode, is off on CI: no caps_*_ramping).
 *
 * Bring-up switches (FirebrandCIDPM / frb_cidpm, frbctl dpm on <bits>):
 * FRB_CI_DPM_UPLOAD_ONLY loads the SMC image and every table into the SMC's
 * SRAM and stops with the SMC held in reset, for comparing the SRAM with
 * Linux radeon's (a capture of 2026-10-21) before anything
 * changes a clock; a later frbctl dpm on starts it.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_device.h"
#include "frb_pm.h"
/* the carried code's public half; its internals define RREG32 & co. over the rdn_card, frb_device.h over adev */
#include "si_dpm/rdn_ci_dpm.h"
#include "si_dpm/linux/cikd.h"

struct frb_ci_dpm {
	struct rdn_card card;
	struct rdn_chip_info chip;
	struct frb_device *adev;
	int forced;			/* enum amd_dpm_forced_level in force */
	struct amd_pp_display_configuration cfg;
	int have_cfg;
	int uvd_on;			/* frb_ci_dpm_enable_uvd: the block started, UVD DPM on */
	const uint8_t *fw;
	uint32_t fw_size;
	char fw_name[24];
	struct rdn_ci_dpm dpm;		/* large: the SMU tables are inside */
};

/* the T4240's root ports run gen 2 (RadeonNI passed the same) */
#define CI_SYS_PCIE_MASK	(RDN_CI_PCIE_SPEED_25 | RDN_CI_PCIE_SPEED_50)

static int chip_of(struct frb_device *adev, struct rdn_chip_info *chip)
{
	memset(chip, 0, sizeof(*chip));
	chip->family = RDN_FAMILY_CIK;
	chip->dce = 8;
	switch (adev->asic_type) {
	case CHIP_HAWAII:
		chip->chip = RDN_CHIP_HAWAII;
		chip->name = "Hawaii";
		chip->fw = "HAWAII";
		return 0;
	case CHIP_BONAIRE:
		chip->chip = RDN_CHIP_BONAIRE;
		chip->name = "Bonaire";
		chip->fw = "BONAIRE";
		return 0;
	default:
		return -ENODEV;
	}
}

/* the level and its clocks, from TARGET_AND_CURRENT_PROFILE_INDEX (no SMC message) */
static void ci_current_level(struct frb_ci_dpm *s, int32_t *sidx, int32_t *midx, uint32_t *sclk, uint32_t *mclk)
{
	struct ci_power_info *pi = &s->dpm.pi;
	uint32_t v = frb_smc_rreg(s->adev, TARGET_AND_CURRENT_PROFILE_INDEX);
	uint32_t si = (v & CURR_SCLK_INDEX_MASK) >> CURR_SCLK_INDEX_SHIFT;
	uint32_t mi = (v & CURR_MCLK_INDEX_MASK) >> CURR_MCLK_INDEX_SHIFT;

	*sidx = si < pi->dpm_table.sclk_table.count ? (int32_t)si : -1;
	*midx = mi < pi->dpm_table.mclk_table.count ? (int32_t)mi : -1;
	*sclk = *sidx >= 0 ? pi->dpm_table.sclk_table.dpm_levels[si].value : 0;
	*mclk = *midx >= 0 ? pi->dpm_table.mclk_table.dpm_levels[mi].value : 0;
}

/* one line: where the SMC is, for every step of the bring-up */
static void ci_report(struct frb_ci_dpm *s, const char *when)
{
	struct frb_device *adev = s->adev;
	int32_t si, mi;
	uint32_t sclk, mclk, pct = 0, rpm = 0;
	int fp = rdn_ci_dpm_get_fan_speed_percent(&s->dpm, &pct);
	int fr = rdn_ci_dpm_get_fan_speed_rpm(&s->dpm, &rpm);

	ci_current_level(s, &si, &mi, &sclk, &mclk);
	frb_info(adev, "ci_dpm: %s: profile index %08x (sclk level %d, %u0 kHz; mclk level %d, %u0 kHz), "
		 "CG_SPLL_FUNC_CNTL_3 %08x, MPLL_FUNC_CNTL_1 %08x, %d mC, fan %d%% %u rpm (FDO mode %u), "
		 "SMC PC %08x FIRMWARE_FLAGS %08x", when, frb_smc_rreg(s->adev, TARGET_AND_CURRENT_PROFILE_INDEX),
		 si, sclk, mi, mclk, frb_smc_rreg(s->adev, CG_SPLL_FUNC_CNTL_3), frb_rreg(s->adev, MPLL_FUNC_CNTL_1 >> 2),
		 rdn_ci_dpm_temperature(&s->dpm), fp ? -1 : (int)pct, fr ? 0 : rpm, rdn_ci_dpm_get_fan_mode(&s->dpm),
		 frb_smc_rreg(s->adev, SMC_PC_C), frb_smc_rreg(s->adev, FIRMWARE_FLAGS));
}

/* the tables parsed and described once (reads and ATOM tables only) */
static struct frb_ci_dpm *ci_parse(struct frb_device *adev)
{
	struct frb_ci_dpm *s;
	int r;

	if (adev->ci_dpm)
		return adev->ci_dpm;
	if (!adev->atom)
		return NULL;
	s = (struct frb_ci_dpm *)adev->os->alloc(adev->os->cookie, sizeof(*s));
	if (!s)
		return NULL;
	memset(s, 0, sizeof(*s));
	if (chip_of(adev, &s->chip)) {
		adev->os->free(adev->os->cookie, s);
		return NULL;
	}
	s->card.os = adev->os;
	s->card.atom.ctx = adev->atom;
	s->card.chip = &s->chip;
	s->card.family = RDN_FAMILY_CIK;
	s->card.device = adev->device;
	s->card.adev = adev;
	s->adev = adev;
	s->forced = AMD_DPM_FORCED_LEVEL_AUTO;
	r = rdn_ci_dpm_init(&s->dpm, &s->card, CI_SYS_PCIE_MASK);
	if (r) {
		frb_err(adev, "ci_dpm: the power tables did not parse (%d): boot clocks kept", r);
		adev->os->free(adev->os->cookie, s);
		return NULL;
	}
	rdn_ci_dpm_describe(&s->dpm);
	/* ni_dpm.c's closing arbiter switch request: off, as verified on the W8100 (RadeonNI) */
	s->dpm.arb_switch_request = false;
	adev->ci_dpm = s;
	return s;
}

static int ci_apply(struct frb_ci_dpm *s, const char *when)
{
	struct frb_device *adev = s->adev;
	const struct amd_pp_display_configuration *cfg = s->have_cfg ? &s->cfg : NULL;
	uint32_t n = cfg ? cfg->num_display : 0;
	int r;

	/*
	 * ci_dpm_display_configuration_changed with radeon's inputs: no display
	 * is "displays off" (the memory clock may switch); RadeonNI's headless
	 * guess of one display with an unknown blank held it at 1250 MHz, where
	 * Linux idles at 150 MHz on this card.
	 */
	rdn_ci_dpm_display_configuration_changed(&s->dpm, n, cfg && cfg->vrefresh ? cfg->vrefresh : 60,
						 n ? cfg->min_vblank_time : 0xffffffff);
	r = rdn_ci_dpm_set_power_state(&s->dpm, RDN_CI_STATE_PERFORMANCE);
	frb_info(adev, "ci_dpm: %s: %u display(s), vblank %u us: the performance state %s (%d)", when, n,
		 n ? cfg->min_vblank_time : 0xffffffff, r ? "FAILED" : "set", r);
	return r;
}

int frb_ci_dpm_hw_init(struct frb_device *adev)
{
	struct frb_ci_dpm *s;
	const uint8_t *fw = 0;
	uint32_t fw_size = 0;
	int r;

	if (adev->ci_dpm && adev->ci_dpm->dpm.enabled)
		return 0;
	if (!adev->ci_dpm_wanted && !adev->ci_dpm_quirks) {
		frb_info(adev, "ci_dpm: off (FirebrandCIDPM 0 / frb_cidpm=0): the card stays at its boot clocks");
		return -ENOSYS;
	}
	s = ci_parse(adev);
	if (!s)
		return -EINVAL;
	if (s->dpm.uploaded && (adev->ci_dpm_quirks & FRB_CI_DPM_UPLOAD_ONLY))
		return 0;	/* uploaded and held: only frbctl dpm on (no switches) starts it */
	if (!s->dpm.uploaded) {
		/* the image stage smu checked: hawaii_k_smc.bin for rev 0x80, hawaii_smc.bin otherwise */
		if (adev->asic_type == CHIP_HAWAII)
			frb_fw_name(s->fw_name, sizeof(s->fw_name), adev->revision == 0x80 ? "hawaii_k" : "hawaii",
				    "_smc.bin");
		else
			frb_fw_name(s->fw_name, sizeof(s->fw_name),
				    (adev->revision == 0x80 || adev->revision == 0x81 || adev->device == 0x665f) ?
				    "bonaire_k" : "bonaire", "_smc.bin");
		r = adev->os->firmware(adev->os->cookie, s->fw_name, &fw, &fw_size);
		if (r) {
			frb_err(adev, "ci_dpm: no %s (%d): boot clocks kept", s->fw_name, r);
			return r;
		}
		s->fw = fw;
		s->fw_size = fw_size;
		ci_report(s, "before the upload");
		r = rdn_ci_dpm_upload(&s->dpm, s->fw, s->fw_size);
		if (r) {
			frb_err(adev, "ci_dpm: the upload failed (%d): the SMC stays in reset, boot clocks kept", r);
			return r;
		}
		if (adev->ci_dpm_quirks & FRB_CI_DPM_UPLOAD_ONLY) {
			frb_info(adev, "ci_dpm: stopped after the upload (FRB_CI_DPM_UPLOAD_ONLY): frbctl smcdump the "
				 "SRAM, then frbctl dpm on starts the SMC");
			return 0;
		}
	}
	r = rdn_ci_dpm_start(&s->dpm);
	if (r || !s->dpm.enabled) {
		frb_err(adev, "ci_dpm: the start failed (%d)", r);
		ci_report(s, "after the failed start");
		return r ? r : -EIO;
	}
	ci_report(s, "started, boot state");
	r = ci_apply(s, "hw_init");
	ci_report(s, "after the first state switch");
	return r;
}

int frb_ci_dpm_enabled(struct frb_device *adev)
{
	return adev->ci_dpm && adev->ci_dpm->dpm.enabled;
}

/* ci_dpm_disable: back to the boot state, the SMC in reset; then freed */
void frb_ci_dpm_fini(struct frb_device *adev)
{
	struct frb_ci_dpm *s = adev->ci_dpm;

	if (!s)
		return;
	if (s->dpm.enabled || s->dpm.uploaded) {
		rdn_ci_dpm_disable(&s->dpm);
		frb_info(adev, "ci_dpm: off (boot state, DPM disabled)");
	}
	rdn_ci_dpm_fini(&s->dpm);
	adev->os->free(adev->os->cookie, s);
	adev->ci_dpm = NULL;
}

/* the display configuration from the DCE (once dce_v8_0 is up) */
void frb_ci_dpm_compute_clocks(struct frb_device *adev, const struct amd_pp_display_configuration *cfg)
{
	struct frb_ci_dpm *s = adev->ci_dpm;

	if (!s || !s->dpm.enabled)
		return;
	s->cfg = *cfg;
	s->have_cfg = 1;
	ci_apply(s, "display");
}

int frb_ci_dpm_force_performance_level(struct frb_device *adev, enum amd_dpm_forced_level level)
{
	struct frb_ci_dpm *s = adev->ci_dpm;
	int r;

	if (!s || !s->dpm.enabled)
		return -ENODEV;
	switch (level) {
	case AMD_DPM_FORCED_LEVEL_AUTO:
		r = rdn_ci_dpm_force_performance_level(&s->dpm, RDN_CI_FORCED_LEVEL_AUTO);
		break;
	case AMD_DPM_FORCED_LEVEL_LOW:
		r = rdn_ci_dpm_force_performance_level(&s->dpm, RDN_CI_FORCED_LEVEL_LOW);
		break;
	case AMD_DPM_FORCED_LEVEL_HIGH:
		r = rdn_ci_dpm_force_performance_level(&s->dpm, RDN_CI_FORCED_LEVEL_HIGH);
		break;
	default:
		return -EINVAL;
	}
	if (!r)
		s->forced = level;
	adev->os->delay_us(adev->os->cookie, 50000);
	ci_report(s, level == AMD_DPM_FORCED_LEVEL_HIGH ? "forced high" :
		  level == AMD_DPM_FORCED_LEVEL_LOW ? "forced low" : "auto");
	return r;
}

/*
 * amdgpu_dpm_enable_uvd's CIK arm, in smu7_powergate_uvd's order (amdgpu
 * runs Hawaii on powerplay's smu7): on, the block started
 * (uvd_v4_2_set_powergating_state UNGATE) and then radeon's
 * ci_dpm_powergate_uvd(false) (the UVD boot level in DPM_TABLE_475, the
 * enabled-level mask, UVDDPM_Enable; memory level 0 masked while UVD runs);
 * off, the block stopped and then ci_dpm_powergate_uvd(true)
 * (UVDDPM_Disable, memory level 0 back). ci_dpm_late_enable gated it.
 */
int frb_ci_dpm_enable_uvd(struct frb_device *adev, int enable)
{
	struct frb_ci_dpm *s = adev->ci_dpm;
	int r = 0;

	if (!s || !s->dpm.enabled)
		return -ENODEV;
	if (s->uvd_on == !!enable)
		return 0;
	if (enable) {
		r = frb_uvd_v4_2_set_powergating_state(adev, 0);
		rdn_ci_dpm_powergate_uvd(&s->dpm, false);
	} else {
		frb_uvd_v4_2_set_powergating_state(adev, 1);
		rdn_ci_dpm_powergate_uvd(&s->dpm, true);
	}
	s->uvd_on = !!enable;
	frb_info(adev, "ci_dpm: UVD %s (VCLK/DCLK by the SMC's UVD levels, mask 0x%x)%s", enable ? "on" : "off",
		 s->dpm.pi.dpm_level_enable_mask.uvd_dpm_enable_mask, r ? "; the VCPU did not start" : "");
	return r;
}

uint32_t frb_ci_dpm_get_sclk(struct frb_device *adev, int low)
{
	struct frb_ci_dpm *s = adev->ci_dpm;
	struct ci_single_dpm_table *t;

	if (!s || !s->dpm.enabled)
		return 0;
	t = &s->dpm.pi.dpm_table.sclk_table;
	return t->count ? t->dpm_levels[low ? 0 : t->count - 1].value : 0;
}

uint32_t frb_ci_dpm_get_mclk(struct frb_device *adev, int low)
{
	struct frb_ci_dpm *s = adev->ci_dpm;
	struct ci_single_dpm_table *t;

	if (!s || !s->dpm.enabled)
		return 0;
	t = &s->dpm.pi.dpm_table.mclk_table;
	return t->count ? t->dpm_levels[low ? 0 : t->count - 1].value : 0;
}

/* the SMC's average clocks (PPSMC_MSG_API_GetSclkFrequency / GetMclkFrequency): frbctl dpm only */
int frb_ci_dpm_get_status(struct frb_device *adev, struct frb_pm_status *st)
{
	struct frb_ci_dpm *s = adev->ci_dpm;
	struct ci_power_info *pi;
	uint32_t i, sclk, mclk, v = 0;

	memset(st, 0, sizeof(*st));
	if (!s || !s->dpm.enabled)
		return -ENODEV;
	pi = &s->dpm.pi;
	st->enabled = 1;
	st->forced_level = s->forced;
	st->sclk_count = pi->dpm_table.sclk_table.count;
	st->mclk_count = pi->dpm_table.mclk_table.count;
	st->pcie_count = pi->dpm_table.pcie_speed_table.count;
	for (i = 0; i < st->sclk_count && i < 8; i++)
		st->sclk_levels[i] = pi->dpm_table.sclk_table.dpm_levels[i].value;
	for (i = 0; i < st->mclk_count && i < 8; i++)
		st->mclk_levels[i] = pi->dpm_table.mclk_table.dpm_levels[i].value;
	st->sclk_mask = pi->dpm_level_enable_mask.sclk_dpm_enable_mask;
	st->mclk_mask = pi->dpm_level_enable_mask.mclk_dpm_enable_mask;
	st->pcie_mask = pi->dpm_level_enable_mask.pcie_dpm_enable_mask;
	ci_current_level(s, &st->cur_sclk_index, &st->cur_mclk_index, &sclk, &mclk);
	st->cur_sclk = rdn_ci_dpm_get_sclk(&s->dpm);
	st->cur_mclk = rdn_ci_dpm_get_mclk(&s->dpm);
	st->temp_mc = rdn_ci_dpm_temperature(&s->dpm);
	if (!rdn_ci_dpm_get_fan_speed_rpm(&s->dpm, &v))
		st->fan_rpm = v;
	if (!rdn_ci_dpm_get_fan_speed_percent(&s->dpm, &v))
		st->fan_pwm = v * 255 / 100;
	if (s->have_cfg) {
		st->num_display = s->cfg.num_display;
		st->vrefresh = s->cfg.vrefresh;
		st->min_vblank_time = s->cfg.min_vblank_time;
	}
	return 0;
}

/*
 * The status call (Firebrand Monitor, at most every 500 ms): register reads
 * only, no SMC message. The temperature (ci_get_temp's CG_MULT_THERMAL_STATUS)
 * and the fan's duty (CG_THERMAL_STATUS over CG_FDO_CTRL1's 100 %) with or
 * without DPM; its speed from the tachometer once the VBIOS's pulses per
 * revolution are known (the tables parsed); with DPM running the current
 * level's clocks (TARGET_AND_CURRENT_PROFILE_INDEX) and the levels.
 */
int frb_ci_read_sensors(struct frb_device *adev, struct frb_pm_sensors *s)
{
	struct frb_ci_dpm *c = adev->ci_dpm;
	uint32_t t = (frb_smc_rreg(adev, CG_MULT_THERMAL_STATUS) & CTF_TEMP_MASK) >> CTF_TEMP_SHIFT;
	uint32_t duty100 = (frb_smc_rreg(adev, CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;
	uint32_t duty = (frb_smc_rreg(adev, CG_THERMAL_STATUS) & FDO_PWM_DUTY_MASK) >> FDO_PWM_DUTY_SHIFT;
	uint32_t i, rpm, sclk, mclk;
	int32_t si, mi;

	s->temp_mc = (t & 0x200) ? 255000 : (int32_t)(t & 0x1ff) * 1000;
	s->valid |= FRB_PMS_TEMP;
	if (duty100) {
		s->fan_pwm = duty * 255 / duty100 > 255 ? 255 : duty * 255 / duty100;
		s->valid |= FRB_PMS_FAN;
	}
	if (!c)
		return -ENODEV;
	if (!rdn_ci_dpm_get_fan_speed_rpm(&c->dpm, &rpm)) {
		s->fan_rpm = rpm;
		s->valid |= FRB_PMS_FAN;
	}
	if (!c->dpm.enabled)
		return -ENODEV;
	s->dpm_enabled = 1;
	ci_current_level(c, &si, &mi, &sclk, &mclk);
	if (si >= 0 && mi >= 0) {
		s->gfx_mhz = sclk / 100;
		s->mem_mhz = mclk / 100;
		s->valid |= FRB_PMS_CLOCKS;
	}
	s->gfx_count = c->dpm.pi.dpm_table.sclk_table.count < 16 ? c->dpm.pi.dpm_table.sclk_table.count : 16;
	s->mem_count = c->dpm.pi.dpm_table.mclk_table.count < 16 ? c->dpm.pi.dpm_table.mclk_table.count : 16;
	for (i = 0; i < s->gfx_count; i++)
		s->gfx_levels[i] = c->dpm.pi.dpm_table.sclk_table.dpm_levels[i].value / 100;
	for (i = 0; i < s->mem_count; i++)
		s->mem_levels[i] = c->dpm.pi.dpm_table.mclk_table.dpm_levels[i].value / 100;
	if (s->gfx_count || s->mem_count)
		s->valid |= FRB_PMS_LEVELS;
	return 0;
}
