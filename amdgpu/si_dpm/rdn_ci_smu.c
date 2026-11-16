/*
 * Copyright 2011-2013 Advanced Micro Devices, Inc.
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
 * Authors: Alex Deucher (the Linux radeon driver's ci_smc.c and the SMU
 * table half of ci_dpm.c, which this follows)
 */

/*
 * The SMU side: the SMC's SRAM and message registers (ci_smc.c), the
 * firmware header, and every populate_* function that fills
 * SMU7_Discrete_DpmTable, SMU7_Discrete_MCRegisters, the arbiter timing
 * table, the PM fuses and the fan table before the firmware is started.
 *
 * The SMU reads its tables big-endian. Each field is converted with
 * cpu_to_be16/32 (rdn_ci_internal.h, explicit about the host's byte order)
 * at the same point Linux converts it, and ci_copy_bytes_to_smc() then
 * sends the bytes in memory order, as Linux does.
 */

#include "rdn_ci_internal.h"
#include "atom/atom.h"
#include "atom/atombios.h"
#include "linux/pptable.h"

/* ---- ci_smc.c ---- */

static int ci_set_smc_sram_address(struct rdn_card *card, uint32_t smc_address, uint32_t limit)
{
	if (smc_address & 3)
		return -EINVAL;
	if ((smc_address + 3) > limit)
		return -EINVAL;

	WREG32(card, SMC_IND_INDEX_0, smc_address);
	WREG32_P(card, SMC_IND_ACCESS_CNTL, 0, ~AUTO_INCREMENT_IND_0);

	return 0;
}

int ci_copy_bytes_to_smc(struct rdn_ci_dpm *dpm, uint32_t smc_start_address, const uint8_t *src,
			 uint32_t byte_count, uint32_t limit)
{
	struct rdn_card *card = dpm->card;
	uint32_t data, original_data;
	uint32_t addr;
	uint32_t extra_shift;
	int ret = 0;

	if (smc_start_address & 3)
		return -EINVAL;
	if ((smc_start_address + byte_count) > limit)
		return -EINVAL;

	addr = smc_start_address;

	/* Linux takes smc_idx_lock here; this library runs under the caller's lock */
	while (byte_count >= 4) {
		/* SMC address space is BE */
		data = ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) | src[3];

		ret = ci_set_smc_sram_address(card, addr, limit);
		if (ret)
			goto done;

		WREG32(card, SMC_IND_DATA_0, data);

		src += 4;
		byte_count -= 4;
		addr += 4;
	}

	/* RMW for the final bytes */
	if (byte_count > 0) {
		data = 0;

		ret = ci_set_smc_sram_address(card, addr, limit);
		if (ret)
			goto done;

		original_data = RREG32(card, SMC_IND_DATA_0);

		extra_shift = 8 * (4 - byte_count);

		while (byte_count > 0) {
			data = (data << 8) + *src++;
			byte_count--;
		}

		data <<= extra_shift;

		data |= (original_data & ~((~0UL) << extra_shift));

		ret = ci_set_smc_sram_address(card, addr, limit);
		if (ret)
			goto done;

		WREG32(card, SMC_IND_DATA_0, data);
	}

done:
	return ret;
}

bool ci_is_smc_running(struct rdn_card *card)
{
	uint32_t clk = RREG32_SMC(card, SMC_SYSCON_CLOCK_CNTL_0);
	uint32_t pc_c = RREG32_SMC(card, SMC_PC_C);

	if (!(clk & CK_DISABLE) && (0x20100 <= pc_c))
		return true;

	return false;
}

int ci_read_smc_sram_dword(struct rdn_ci_dpm *dpm, uint32_t smc_address, uint32_t *value, uint32_t limit)
{
	int ret;

	ret = ci_set_smc_sram_address(dpm->card, smc_address, limit);
	if (ret == 0)
		*value = RREG32(dpm->card, SMC_IND_DATA_0);

	return ret;
}

int ci_write_smc_sram_dword(struct rdn_ci_dpm *dpm, uint32_t smc_address, uint32_t value, uint32_t limit)
{
	int ret;

	ret = ci_set_smc_sram_address(dpm->card, smc_address, limit);
	if (ret == 0)
		WREG32(dpm->card, SMC_IND_DATA_0, value);

	return ret;
}

/* ---- ci_dpm.c: messages ---- */

PPSMC_Result ci_send_msg_to_smc(struct rdn_ci_dpm *dpm, PPSMC_Msg msg)
{
	struct rdn_card *card = dpm->card;
	uint32_t tmp;
	int i;

	if (!ci_is_smc_running(card))
		return PPSMC_Result_Failed;

	WREG32(card, SMC_MESSAGE_0, msg);

	for (i = 0; i < CI_USEC_TIMEOUT; i++) {
		tmp = RREG32(card, SMC_RESP_0);
		if (tmp != 0)
			break;
		udelay(card, 1);
	}
	tmp = RREG32(card, SMC_RESP_0);

	/* Every message, while the engine clock's refusal to switch is being measured (W8100, 2026-11-03). */
	rdn_log(card->os, tmp != PPSMC_Result_OK ? RDN_LOG_ERROR : RDN_LOG_INFO,
		"ci_dpm: SMC message 0x%x (arg 0x%08x) answered 0x%x after %d us", msg, RREG32(card, SMC_MSG_ARG_0), tmp, i);

	return (PPSMC_Result)tmp;
}

PPSMC_Result ci_send_msg_to_smc_with_parameter(struct rdn_ci_dpm *dpm, PPSMC_Msg msg, uint32_t parameter)
{
	WREG32(dpm->card, SMC_MSG_ARG_0, parameter);
	return ci_send_msg_to_smc(dpm, msg);
}

PPSMC_Result ci_send_msg_to_smc_return_parameter(struct rdn_ci_dpm *dpm, PPSMC_Msg msg, uint32_t *parameter)
{
	PPSMC_Result smc_result;

	smc_result = ci_send_msg_to_smc(dpm, msg);

	if ((smc_result == PPSMC_Result_OK) && parameter)
		*parameter = RREG32(dpm->card, SMC_MSG_ARG_0);

	return smc_result;
}

int ci_write_smc_soft_register(struct rdn_ci_dpm *dpm, uint16_t reg_offset, uint32_t value)
{
	struct ci_power_info *pi = &dpm->pi;

	return ci_write_smc_sram_dword(dpm,
				       pi->soft_regs_start + reg_offset,
				       value, pi->sram_end);
}

/* ---- the firmware header ---- */

int ci_process_firmware_header(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;
	int ret;

	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, DpmTable),
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	pi->dpm_table_start = tmp;

	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, SoftRegisters),
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	pi->soft_regs_start = tmp;

	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, mcRegisterTable),
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	pi->mc_reg_table_start = tmp;

	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, FanTable),
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	pi->fan_table_start = tmp;

	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, mcArbDramTimingTable),
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	pi->arb_table_start = tmp;

	rdn_log(dpm->card->os, RDN_LOG_INFO,
		"ci_dpm: SMU firmware header: dpm table 0x%x, soft regs 0x%x, mc reg table 0x%x, fan table 0x%x, arb table 0x%x",
		pi->dpm_table_start, pi->soft_regs_start, pi->mc_reg_table_start, pi->fan_table_start, pi->arb_table_start);

	return 0;
}

/* ---- the powertune defaults and the PM fuse table ---- */

static const struct ci_pt_defaults defaults_hawaii_xt = {
	1, 0xF, 0xFD, 0x19, 5, 0x14, 0, 0xB0000,
	{ 0x2E,  0x00,  0x00,  0x88,  0x00,  0x00,  0x72,  0x60,  0x51,  0xA7,  0x79,  0x6B,  0x90,  0xBD,  0x79  },
	{ 0x217, 0x217, 0x217, 0x242, 0x242, 0x242, 0x269, 0x269, 0x269, 0x2A1, 0x2A1, 0x2A1, 0x2C9, 0x2C9, 0x2C9 }
};

static const struct ci_pt_defaults defaults_hawaii_pro = {
	1, 0xF, 0xFD, 0x19, 5, 0x14, 0, 0x65062,
	{ 0x2E,  0x00,  0x00,  0x88,  0x00,  0x00,  0x72,  0x60,  0x51,  0xA7,  0x79,  0x6B,  0x90,  0xBD,  0x79  },
	{ 0x217, 0x217, 0x217, 0x242, 0x242, 0x242, 0x269, 0x269, 0x269, 0x2A1, 0x2A1, 0x2A1, 0x2C9, 0x2C9, 0x2C9 }
};

static const struct ci_pt_defaults defaults_bonaire_xt = {
	1, 0xF, 0xFD, 0x19, 5, 45, 0, 0xB0000,
	{ 0x79,  0x253, 0x25D, 0xAE,  0x72,  0x80,  0x83,  0x86,  0x6F,  0xC8,  0xC9,  0xC9,  0x2F,  0x4D,  0x61  },
	{ 0x17C, 0x172, 0x180, 0x1BC, 0x1B3, 0x1BD, 0x206, 0x200, 0x203, 0x25D, 0x25A, 0x255, 0x2C3, 0x2C5, 0x2B4 }
};

static const struct ci_pt_defaults defaults_saturn_xt = {
	1, 0xF, 0xFD, 0x19, 5, 55, 0, 0x70000,
	{ 0x8C,  0x247, 0x249, 0xA6,  0x80,  0x81,  0x8B,  0x89,  0x86,  0xC9,  0xCA,  0xC9,  0x4D,  0x4D,  0x4D  },
	{ 0x187, 0x187, 0x187, 0x1C7, 0x1C7, 0x1C7, 0x210, 0x210, 0x210, 0x266, 0x266, 0x266, 0x2C9, 0x2C9, 0x2C9 }
};

void ci_initialize_powertune_defaults(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	switch (dpm->card->device) {
	case 0x6649:
	case 0x6650:
	case 0x6651:
	case 0x6658:
	case 0x665C:
	case 0x665D:
	default:
		pi->powertune_defaults = &defaults_bonaire_xt;
		break;
	case 0x6640:
	case 0x6641:
	case 0x6646:
	case 0x6647:
		pi->powertune_defaults = &defaults_saturn_xt;
		break;
	case 0x67B8:
	case 0x67B0:
		pi->powertune_defaults = &defaults_hawaii_xt;
		break;
	case 0x67BA:
	case 0x67B1:
		pi->powertune_defaults = &defaults_hawaii_pro;
		break;
	case 0x67A0:
	case 0x67A1:
	case 0x67A2:
	case 0x67A8:
	case 0x67A9:
	case 0x67AA:
	case 0x67B9:
	case 0x67BE:
		pi->powertune_defaults = &defaults_bonaire_xt;
		break;
	}

	pi->dte_tj_offset = 0;

	pi->caps_power_containment = true;
	pi->caps_cac = false;
	pi->caps_sq_ramping = false;
	pi->caps_db_ramping = false;
	pi->caps_td_ramping = false;
	pi->caps_tcp_ramping = false;

	if (pi->caps_power_containment) {
		pi->caps_cac = true;
		if (dpm->card->chip && dpm->card->chip->chip == RDN_CHIP_HAWAII)
			pi->enable_bapm_feature = false;
		else
			pi->enable_bapm_feature = true;
		pi->enable_tdc_limit_feature = true;
		pi->enable_pkg_pwr_tracking_feature = true;
	}
}

static uint8_t ci_convert_to_vid(uint16_t vddc)
{
	return (6200 - (vddc * VOLTAGE_SCALE)) / 25;
}

static int ci_populate_bapm_vddc_vid_sidd(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint8_t *hi_vid = pi->smc_powertune_table.BapmVddCVidHiSidd;
	uint8_t *lo_vid = pi->smc_powertune_table.BapmVddCVidLoSidd;
	uint8_t *hi2_vid = pi->smc_powertune_table.BapmVddCVidHiSidd2;
	uint32_t i;

	if (dpm->dyn_state.cac_leakage_table.entries == NULL)
		return -EINVAL;
	if (dpm->dyn_state.cac_leakage_table.count > 8)
		return -EINVAL;
	if (dpm->dyn_state.cac_leakage_table.count !=
	    dpm->dyn_state.vddc_dependency_on_sclk.count)
		return -EINVAL;

	for (i = 0; i < dpm->dyn_state.cac_leakage_table.count; i++) {
		if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_EVV) {
			lo_vid[i] = ci_convert_to_vid(dpm->dyn_state.cac_leakage_table.entries[i].vddc1);
			hi_vid[i] = ci_convert_to_vid(dpm->dyn_state.cac_leakage_table.entries[i].vddc2);
			hi2_vid[i] = ci_convert_to_vid(dpm->dyn_state.cac_leakage_table.entries[i].vddc3);
		} else {
			lo_vid[i] = ci_convert_to_vid(dpm->dyn_state.cac_leakage_table.entries[i].vddc);
			hi_vid[i] = ci_convert_to_vid((uint16_t)dpm->dyn_state.cac_leakage_table.entries[i].leakage);
		}
	}
	return 0;
}

static int ci_populate_vddc_vid(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint8_t *vid = pi->smc_powertune_table.VddCVid;
	uint32_t i;

	if (pi->vddc_voltage_table.count > 8)
		return -EINVAL;

	for (i = 0; i < pi->vddc_voltage_table.count; i++)
		vid[i] = ci_convert_to_vid(pi->vddc_voltage_table.entries[i].value);

	return 0;
}

static int ci_populate_svi_load_line(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct ci_pt_defaults *pt_defaults = pi->powertune_defaults;

	pi->smc_powertune_table.SviLoadLineEn = pt_defaults->svi_load_line_en;
	pi->smc_powertune_table.SviLoadLineVddC = pt_defaults->svi_load_line_vddc;
	pi->smc_powertune_table.SviLoadLineTrimVddC = 3;
	pi->smc_powertune_table.SviLoadLineOffsetVddC = 0;

	return 0;
}

static int ci_populate_tdc_limit(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct ci_pt_defaults *pt_defaults = pi->powertune_defaults;
	uint16_t tdc_limit;

	tdc_limit = dpm->dyn_state.cac_tdp_table.tdc * 256;
	pi->smc_powertune_table.TDC_VDDC_PkgLimit = cpu_to_be16(tdc_limit);
	pi->smc_powertune_table.TDC_VDDC_ThrottleReleaseLimitPerc =
		pt_defaults->tdc_vddc_throttle_release_limit_perc;
	pi->smc_powertune_table.TDC_MAWt = pt_defaults->tdc_mawt;

	return 0;
}

static int ci_populate_dw8(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct ci_pt_defaults *pt_defaults = pi->powertune_defaults;
	uint32_t tmp;
	int ret;

	/*
	 * Linux reads the dword into (u32 *)&TdcWaterfallCtl, which overwrites
	 * the next three fuse bytes too, and then sets TdcWaterfallCtl from
	 * the defaults anyway; the read is only a probe that the address is
	 * reachable, so its value goes to a temporary here.
	 */
	ret = ci_read_smc_sram_dword(dpm,
				     SMU7_FIRMWARE_HEADER_LOCATION +
				     offsetof(SMU7_Firmware_Header, PmFuseTable) +
				     offsetof(SMU7_Discrete_PmFuses, TdcWaterfallCtl),
				     &tmp,
				     pi->sram_end);
	if (ret)
		return -EINVAL;
	else
		pi->smc_powertune_table.TdcWaterfallCtl = pt_defaults->tdc_waterfall_ctl;

	return 0;
}

static int ci_populate_fuzzy_fan(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	if ((dpm->fan.fan_output_sensitivity & (1 << 15)) ||
	    (dpm->fan.fan_output_sensitivity == 0))
		dpm->fan.fan_output_sensitivity =
			dpm->fan.default_fan_output_sensitivity;

	pi->smc_powertune_table.FuzzyFan_PwmSetDelta =
		cpu_to_be16(dpm->fan.fan_output_sensitivity);

	return 0;
}

static int ci_min_max_v_gnbl_pm_lid_from_bapm_vddc(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint8_t *hi_vid = pi->smc_powertune_table.BapmVddCVidHiSidd;
	uint8_t *lo_vid = pi->smc_powertune_table.BapmVddCVidLoSidd;
	int i, min, max;

	min = max = hi_vid[0];
	for (i = 0; i < 8; i++) {
		if (0 != hi_vid[i]) {
			if (min > hi_vid[i])
				min = hi_vid[i];
			if (max < hi_vid[i])
				max = hi_vid[i];
		}

		if (0 != lo_vid[i]) {
			if (min > lo_vid[i])
				min = lo_vid[i];
			if (max < lo_vid[i])
				max = lo_vid[i];
		}
	}

	if ((min == 0) || (max == 0))
		return -EINVAL;
	pi->smc_powertune_table.GnbLPMLMaxVid = (uint8_t)max;
	pi->smc_powertune_table.GnbLPMLMinVid = (uint8_t)min;

	return 0;
}

static int ci_populate_bapm_vddc_base_leakage_sidd(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint16_t hi_sidd, lo_sidd;
	struct rdn_ci_cac_tdp_table *cac_tdp_table = &dpm->dyn_state.cac_tdp_table;

	hi_sidd = cac_tdp_table->high_cac_leakage / 100 * 256;
	lo_sidd = cac_tdp_table->low_cac_leakage / 100 * 256;

	pi->smc_powertune_table.BapmVddCBaseLeakageHiSidd = cpu_to_be16(hi_sidd);
	pi->smc_powertune_table.BapmVddCBaseLeakageLoSidd = cpu_to_be16(lo_sidd);

	return 0;
}

int ci_populate_bapm_parameters_in_dpm_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	const struct ci_pt_defaults *pt_defaults = pi->powertune_defaults;
	SMU7_Discrete_DpmTable  *dpm_table = &pi->smc_state_table;
	struct rdn_ci_cac_tdp_table *cac_tdp_table = &dpm->dyn_state.cac_tdp_table;
	struct rdn_ci_ppm_table *ppm = dpm->dyn_state.has_ppm_table ? &dpm->dyn_state.ppm_table : NULL;
	int i, j, k;
	const uint16_t *def1;
	const uint16_t *def2;

	/*
	 * Linux leaves these two in host order (no cpu_to_be16), so the SMU gets
	 * a little-endian host's bytes: 00 85 for 133 W, read as 133. The same
	 * bytes from here (rdn_ci_internal.h cpu_to_le16); as plain host order
	 * this host sent 85 00, read as 34048 (measured 2026-11-04).
	 */
	dpm_table->DefaultTdp = cpu_to_le16((uint16_t)(cac_tdp_table->tdp * 256));
	dpm_table->TargetTdp = cpu_to_le16((uint16_t)(cac_tdp_table->configurable_tdp * 256));

	dpm_table->DTETjOffset = (uint8_t)pi->dte_tj_offset;
	dpm_table->GpuTjMax =
		(uint8_t)(pi->thermal_temp_setting.temperature_high / 1000);
	dpm_table->GpuTjHyst = 8;

	dpm_table->DTEAmbientTempBase = pt_defaults->dte_ambient_temp_base;

	if (ppm) {
		dpm_table->PPM_PkgPwrLimit = cpu_to_be16((uint16_t)ppm->dgpu_tdp * 256 / 1000);
		dpm_table->PPM_TemperatureLimit = cpu_to_be16((uint16_t)ppm->tj_max * 256);
	} else {
		dpm_table->PPM_PkgPwrLimit = cpu_to_be16(0);
		dpm_table->PPM_TemperatureLimit = cpu_to_be16(0);
	}

	dpm_table->BAPM_TEMP_GRADIENT = cpu_to_be32(pt_defaults->bapm_temp_gradient);
	def1 = pt_defaults->bapmti_r;
	def2 = pt_defaults->bapmti_rc;

	for (i = 0; i < SMU7_DTE_ITERATIONS; i++) {
		for (j = 0; j < SMU7_DTE_SOURCES; j++) {
			for (k = 0; k < SMU7_DTE_SINKS; k++) {
				dpm_table->BAPMTI_R[i][j][k] = cpu_to_be16(*def1);
				dpm_table->BAPMTI_RC[i][j][k] = cpu_to_be16(*def2);
				def1++;
				def2++;
			}
		}
	}

	return 0;
}

int ci_populate_pm_base(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t pm_fuse_table_offset;
	int ret;

	if (pi->caps_power_containment) {
		ret = ci_read_smc_sram_dword(dpm,
					     SMU7_FIRMWARE_HEADER_LOCATION +
					     offsetof(SMU7_Firmware_Header, PmFuseTable),
					     &pm_fuse_table_offset, pi->sram_end);
		if (ret)
			return ret;
		ret = ci_populate_bapm_vddc_vid_sidd(dpm);
		if (ret)
			return ret;
		ret = ci_populate_vddc_vid(dpm);
		if (ret)
			return ret;
		ret = ci_populate_svi_load_line(dpm);
		if (ret)
			return ret;
		ret = ci_populate_tdc_limit(dpm);
		if (ret)
			return ret;
		ret = ci_populate_dw8(dpm);
		if (ret)
			return ret;
		ret = ci_populate_fuzzy_fan(dpm);
		if (ret)
			return ret;
		ret = ci_min_max_v_gnbl_pm_lid_from_bapm_vddc(dpm);
		if (ret)
			return ret;
		ret = ci_populate_bapm_vddc_base_leakage_sidd(dpm);
		if (ret)
			return ret;
		ret = ci_copy_bytes_to_smc(dpm, pm_fuse_table_offset,
					   (uint8_t *)&pi->smc_powertune_table,
					   sizeof(SMU7_Discrete_PmFuses), pi->sram_end);
		if (ret)
			return ret;
	}

	return 0;
}

/* ---- the low-sclk interrupt threshold ---- */

int ci_update_sclk_t(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret = 0;
	uint32_t low_sclk_interrupt_t = 0;

	if (pi->caps_sclk_throttle_low_notification) {
		low_sclk_interrupt_t = cpu_to_be32(pi->low_sclk_interrupt_t);

		ret = ci_copy_bytes_to_smc(dpm,
					   pi->dpm_table_start +
					   offsetof(SMU7_Discrete_DpmTable, LowSclkInterruptT),
					   (uint8_t *)&low_sclk_interrupt_t,
					   sizeof(uint32_t), pi->sram_end);

	}

	return ret;
}

/* ---- the voltage tables ---- */

static int ci_get_svi2_voltage_table(struct rdn_ci_dpm *dpm,
				     struct rdn_ci_dep_table *voltage_dependency_table,
				     struct atom_voltage_table *voltage_table)
{
	uint32_t i;

	(void)dpm;
	if (voltage_dependency_table == NULL)
		return -EINVAL;

	voltage_table->mask_low = 0;
	voltage_table->phase_delay = 0;

	voltage_table->count = voltage_dependency_table->count;
	if (voltage_table->count > MAX_VOLTAGE_ENTRIES)
		return -EINVAL;	/* Linux would overrun entries[]; the dependency tables are 8 deep in practice */
	for (i = 0; i < voltage_table->count; i++) {
		voltage_table->entries[i].value = voltage_dependency_table->entries[i].v;
		voltage_table->entries[i].smio_low = 0;
	}

	return 0;
}

int ci_construct_voltage_tables(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	if (pi->voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO) {
		ret = radeon_atom_get_voltage_table(dpm->card, VOLTAGE_TYPE_VDDC,
						    VOLTAGE_OBJ_GPIO_LUT,
						    &pi->vddc_voltage_table);
		if (ret)
			return ret;
	} else if (pi->voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_SVID2) {
		ret = ci_get_svi2_voltage_table(dpm,
						&dpm->dyn_state.vddc_dependency_on_mclk,
						&pi->vddc_voltage_table);
		if (ret)
			return ret;
	}

	if (pi->vddc_voltage_table.count > SMU7_MAX_LEVELS_VDDC)
		si_trim_voltage_table_to_fit_state_table(SMU7_MAX_LEVELS_VDDC,
							 &pi->vddc_voltage_table);

	if (pi->vddci_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO) {
		ret = radeon_atom_get_voltage_table(dpm->card, VOLTAGE_TYPE_VDDCI,
						    VOLTAGE_OBJ_GPIO_LUT,
						    &pi->vddci_voltage_table);
		if (ret)
			return ret;
	} else if (pi->vddci_control == CISLANDS_VOLTAGE_CONTROL_BY_SVID2) {
		ret = ci_get_svi2_voltage_table(dpm,
						&dpm->dyn_state.vddci_dependency_on_mclk,
						&pi->vddci_voltage_table);
		if (ret)
			return ret;
	}

	if (pi->vddci_voltage_table.count > SMU7_MAX_LEVELS_VDDCI)
		si_trim_voltage_table_to_fit_state_table(SMU7_MAX_LEVELS_VDDCI,
							 &pi->vddci_voltage_table);

	if (pi->mvdd_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO) {
		ret = radeon_atom_get_voltage_table(dpm->card, VOLTAGE_TYPE_MVDDC,
						    VOLTAGE_OBJ_GPIO_LUT,
						    &pi->mvdd_voltage_table);
		if (ret)
			return ret;
	} else if (pi->mvdd_control == CISLANDS_VOLTAGE_CONTROL_BY_SVID2) {
		ret = ci_get_svi2_voltage_table(dpm,
						&dpm->dyn_state.mvdd_dependency_on_mclk,
						&pi->mvdd_voltage_table);
		if (ret)
			return ret;
	}

	if (pi->mvdd_voltage_table.count > SMU7_MAX_LEVELS_MVDD)
		si_trim_voltage_table_to_fit_state_table(SMU7_MAX_LEVELS_MVDD,
							 &pi->mvdd_voltage_table);

	return 0;
}

static int ci_get_std_voltage_value_sidd(struct rdn_ci_dpm *dpm,
					 struct atom_voltage_table_entry *voltage_table,
					 uint16_t *std_voltage_hi_sidd, uint16_t *std_voltage_lo_sidd)
{
	uint16_t v_index, idx;
	bool voltage_found = false;
	*std_voltage_hi_sidd = voltage_table->value * VOLTAGE_SCALE;
	*std_voltage_lo_sidd = voltage_table->value * VOLTAGE_SCALE;

	if (dpm->dyn_state.vddc_dependency_on_sclk.entries == NULL)
		return -EINVAL;

	if (dpm->dyn_state.cac_leakage_table.entries) {
		for (v_index = 0; (uint32_t)v_index < dpm->dyn_state.vddc_dependency_on_sclk.count; v_index++) {
			if (voltage_table->value ==
			    dpm->dyn_state.vddc_dependency_on_sclk.entries[v_index].v) {
				voltage_found = true;
				if ((uint32_t)v_index < dpm->dyn_state.cac_leakage_table.count)
					idx = v_index;
				else
					idx = dpm->dyn_state.cac_leakage_table.count - 1;
				*std_voltage_lo_sidd =
					dpm->dyn_state.cac_leakage_table.entries[idx].vddc * VOLTAGE_SCALE;
				*std_voltage_hi_sidd =
					(uint16_t)(dpm->dyn_state.cac_leakage_table.entries[idx].leakage * VOLTAGE_SCALE);
				break;
			}
		}

		if (!voltage_found) {
			for (v_index = 0; (uint32_t)v_index < dpm->dyn_state.vddc_dependency_on_sclk.count; v_index++) {
				if (voltage_table->value <=
				    dpm->dyn_state.vddc_dependency_on_sclk.entries[v_index].v) {
					voltage_found = true;
					if ((uint32_t)v_index < dpm->dyn_state.cac_leakage_table.count)
						idx = v_index;
					else
						idx = dpm->dyn_state.cac_leakage_table.count - 1;
					*std_voltage_lo_sidd =
						dpm->dyn_state.cac_leakage_table.entries[idx].vddc * VOLTAGE_SCALE;
					*std_voltage_hi_sidd =
						(uint16_t)(dpm->dyn_state.cac_leakage_table.entries[idx].leakage * VOLTAGE_SCALE);
					break;
				}
			}
		}
	}

	return 0;
}

static void ci_populate_smc_voltage_table(struct rdn_ci_dpm *dpm,
					  struct atom_voltage_table_entry *voltage_table,
					  SMU7_Discrete_VoltageLevel *smc_voltage_table)
{
	int ret;

	ret = ci_get_std_voltage_value_sidd(dpm, voltage_table,
					    &smc_voltage_table->StdVoltageHiSidd,
					    &smc_voltage_table->StdVoltageLoSidd);

	if (ret) {
		smc_voltage_table->StdVoltageHiSidd = voltage_table->value * VOLTAGE_SCALE;
		smc_voltage_table->StdVoltageLoSidd = voltage_table->value * VOLTAGE_SCALE;
	}

	smc_voltage_table->Voltage = cpu_to_be16(voltage_table->value * VOLTAGE_SCALE);
	smc_voltage_table->StdVoltageHiSidd =
		cpu_to_be16(smc_voltage_table->StdVoltageHiSidd);
	smc_voltage_table->StdVoltageLoSidd =
		cpu_to_be16(smc_voltage_table->StdVoltageLoSidd);
}

static int ci_populate_smc_vddc_table(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	struct ci_power_info *pi = &dpm->pi;
	unsigned int count;

	table->VddcLevelCount = pi->vddc_voltage_table.count;
	for (count = 0; count < table->VddcLevelCount; count++) {
		ci_populate_smc_voltage_table(dpm,
					      &pi->vddc_voltage_table.entries[count],
					      &table->VddcLevel[count]);

		if (pi->voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO)
			table->VddcLevel[count].Smio |=
				pi->vddc_voltage_table.entries[count].smio_low;
		else
			table->VddcLevel[count].Smio = 0;
	}
	table->VddcLevelCount = cpu_to_be32(table->VddcLevelCount);

	return 0;
}

static int ci_populate_smc_vddci_table(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	unsigned int count;
	struct ci_power_info *pi = &dpm->pi;

	table->VddciLevelCount = pi->vddci_voltage_table.count;
	for (count = 0; count < table->VddciLevelCount; count++) {
		ci_populate_smc_voltage_table(dpm,
					      &pi->vddci_voltage_table.entries[count],
					      &table->VddciLevel[count]);

		if (pi->vddci_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO)
			table->VddciLevel[count].Smio |=
				pi->vddci_voltage_table.entries[count].smio_low;
		else
			table->VddciLevel[count].Smio = 0;
	}
	table->VddciLevelCount = cpu_to_be32(table->VddciLevelCount);

	return 0;
}

static int ci_populate_smc_mvdd_table(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	struct ci_power_info *pi = &dpm->pi;
	unsigned int count;

	table->MvddLevelCount = pi->mvdd_voltage_table.count;
	for (count = 0; count < table->MvddLevelCount; count++) {
		ci_populate_smc_voltage_table(dpm,
					      &pi->mvdd_voltage_table.entries[count],
					      &table->MvddLevel[count]);

		if (pi->mvdd_control == CISLANDS_VOLTAGE_CONTROL_BY_GPIO)
			table->MvddLevel[count].Smio |=
				pi->mvdd_voltage_table.entries[count].smio_low;
		else
			table->MvddLevel[count].Smio = 0;
	}
	table->MvddLevelCount = cpu_to_be32(table->MvddLevelCount);

	return 0;
}

static int ci_populate_smc_voltage_tables(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	int ret;

	ret = ci_populate_smc_vddc_table(dpm, table);
	if (ret)
		return ret;

	ret = ci_populate_smc_vddci_table(dpm, table);
	if (ret)
		return ret;

	ret = ci_populate_smc_mvdd_table(dpm, table);
	if (ret)
		return ret;

	return 0;
}

/*
 * ci_populate_mvdd_value(): as in Linux this returns -EINVAL on every path
 * (the found case falls out of the if to the final return), so the ACPI
 * level's MinMvdd ends up 0. Kept as is.
 */
static int ci_populate_mvdd_value(struct rdn_ci_dpm *dpm, uint32_t mclk,
				  SMU7_Discrete_VoltageLevel *voltage)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t i = 0;

	if (pi->mvdd_control != CISLANDS_VOLTAGE_CONTROL_NONE) {
		for (i = 0; i < dpm->dyn_state.mvdd_dependency_on_mclk.count; i++) {
			if (mclk <= dpm->dyn_state.mvdd_dependency_on_mclk.entries[i].clk) {
				voltage->Voltage = pi->mvdd_voltage_table.entries[i].value;
				break;
			}
		}

		if (i >= dpm->dyn_state.mvdd_dependency_on_mclk.count)
			return -EINVAL;
	}

	return -EINVAL;
}

static void ci_populate_phase_value_based_on_sclk(const struct rdn_ci_phase_shedding_limits_table *limits,
						  uint32_t sclk, uint32_t *phase_shedding)
{
	unsigned int i;

	*phase_shedding = 1;

	for (i = 0; i < limits->count; i++) {
		if (sclk < limits->entries[i].sclk) {
			*phase_shedding = i;
			break;
		}
	}
}

static void ci_populate_phase_value_based_on_mclk(const struct rdn_ci_phase_shedding_limits_table *limits,
						  uint32_t mclk, uint32_t *phase_shedding)
{
	unsigned int i;

	*phase_shedding = 1;

	for (i = 0; i < limits->count; i++) {
		if (mclk < limits->entries[i].mclk) {
			*phase_shedding = i;
			break;
		}
	}
}

/* ---- the arbiter ---- */

int ci_init_arb_table_index(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t tmp;
	int ret;

	ret = ci_read_smc_sram_dword(dpm, pi->arb_table_start,
				     &tmp, pi->sram_end);
	if (ret)
		return ret;

	tmp &= 0x00FFFFFF;
	tmp |= MC_CG_ARB_FREQ_F1 << 24;

	return ci_write_smc_sram_dword(dpm, pi->arb_table_start,
				       tmp, pi->sram_end);
}

static int ci_get_dependency_volt_by_clk(struct rdn_ci_dep_table *allowed_clock_voltage_table,
					 uint32_t clock, uint32_t *voltage)
{
	uint32_t i = 0;

	if (allowed_clock_voltage_table->count == 0)
		return -EINVAL;

	for (i = 0; i < allowed_clock_voltage_table->count; i++) {
		if (allowed_clock_voltage_table->entries[i].clk >= clock) {
			*voltage = allowed_clock_voltage_table->entries[i].v;
			return 0;
		}
	}

	*voltage = allowed_clock_voltage_table->entries[i-1].v;

	return 0;
}

static uint8_t ci_get_sleep_divider_id_from_clock(uint32_t sclk, uint32_t min_sclk_in_sr)
{
	uint32_t i;
	uint32_t tmp;
	uint32_t min = (min_sclk_in_sr > CISLAND_MINIMUM_ENGINE_CLOCK) ?
		min_sclk_in_sr : CISLAND_MINIMUM_ENGINE_CLOCK;

	if (sclk < min)
		return 0;

	for (i = CISLAND_MAX_DEEPSLEEP_DIVIDER_ID;  ; i--) {
		tmp = sclk / (1 << i);
		if (tmp >= min || i == 0)
			break;
	}

	return (uint8_t)i;
}

int ci_initial_switch_from_arb_f0_to_f1(struct rdn_ci_dpm *dpm)
{
	return ni_copy_and_switch_arb_sets(dpm->card, MC_CG_ARB_FREQ_F0, MC_CG_ARB_FREQ_F1, dpm->arb_switch_request);
}

int ci_force_switch_to_arb_f0(struct rdn_ci_dpm *dpm)
{
	uint32_t tmp;

	tmp = (RREG32_SMC(dpm->card, SMC_SCRATCH9) & 0x0000ff00) >> 8;

	if (tmp == MC_CG_ARB_FREQ_F0)
		return 0;

	return ni_copy_and_switch_arb_sets(dpm->card, tmp, MC_CG_ARB_FREQ_F0, dpm->arb_switch_request);
}

static void ci_register_patching_mc_arb(struct rdn_ci_dpm *dpm,
					const uint32_t engine_clock,
					const uint32_t memory_clock,
					uint32_t *dram_timimg2)
{
	bool patch;
	uint32_t tmp, tmp2;

	tmp = RREG32(dpm->card, MC_SEQ_MISC0);
	patch = (tmp & 0x0000f00) == 0x300;

	if (patch &&
	    ((dpm->card->device == 0x67B0) ||
	     (dpm->card->device == 0x67B1))) {
		if ((memory_clock > 100000) && (memory_clock <= 125000)) {
			tmp2 = (((0x31 * engine_clock) / 125000) - 1) & 0xff;
			*dram_timimg2 &= ~0x00ff0000;
			*dram_timimg2 |= tmp2 << 16;
		} else if ((memory_clock > 125000) && (memory_clock <= 137500)) {
			tmp2 = (((0x36 * engine_clock) / 137500) - 1) & 0xff;
			*dram_timimg2 &= ~0x00ff0000;
			*dram_timimg2 |= tmp2 << 16;
		}
	}
}


static int ci_populate_memory_timing_parameters(struct rdn_ci_dpm *dpm,
						uint32_t sclk,
						uint32_t mclk,
						SMU7_Discrete_MCArbDramTimingTableEntry *arb_regs)
{
	uint32_t dram_timing;
	uint32_t dram_timing2;
	uint32_t burst_time;

	radeon_atom_set_engine_dram_timings(dpm->card, sclk, mclk);

	dram_timing  = RREG32(dpm->card, MC_ARB_DRAM_TIMING);
	dram_timing2 = RREG32(dpm->card, MC_ARB_DRAM_TIMING2);
	burst_time = RREG32(dpm->card, MC_ARB_BURST_TIME) & STATE0_MASK;

	ci_register_patching_mc_arb(dpm, sclk, mclk, &dram_timing2);

	arb_regs->McArbDramTiming  = cpu_to_be32(dram_timing);
	arb_regs->McArbDramTiming2 = cpu_to_be32(dram_timing2);
	arb_regs->McArbBurstTime = (uint8_t)burst_time;

	return 0;
}

static int ci_do_program_memory_timing_parameters(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	SMU7_Discrete_MCArbDramTimingTable arb_regs;
	uint32_t i, j;
	int ret =  0;

	memset(&arb_regs, 0, sizeof(SMU7_Discrete_MCArbDramTimingTable));

	for (i = 0; i < pi->dpm_table.sclk_table.count; i++) {
		for (j = 0; j < pi->dpm_table.mclk_table.count; j++) {
			ret = ci_populate_memory_timing_parameters(dpm,
								   pi->dpm_table.sclk_table.dpm_levels[i].value,
								   pi->dpm_table.mclk_table.dpm_levels[j].value,
								   &arb_regs.entries[i][j]);
			if (ret)
				break;
		}
	}

	if (ret == 0)
		ret = ci_copy_bytes_to_smc(dpm,
					   pi->arb_table_start,
					   (uint8_t *)&arb_regs,
					   sizeof(SMU7_Discrete_MCArbDramTimingTable),
					   pi->sram_end);

	return ret;
}

int ci_program_memory_timing_parameters(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	if (pi->need_update_smu7_dpm_table == 0)
		return 0;

	return ci_do_program_memory_timing_parameters(dpm);
}

/* ---- the boot levels and the link levels ---- */

static void ci_populate_smc_initial_state(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *radeon_boot_state)
{
	struct ci_ps *boot_state = &radeon_boot_state->ps;
	struct ci_power_info *pi = &dpm->pi;
	uint32_t level = 0;

	for (level = 0; level < dpm->dyn_state.vddc_dependency_on_sclk.count; level++) {
		if (dpm->dyn_state.vddc_dependency_on_sclk.entries[level].clk >=
		    boot_state->performance_levels[0].sclk) {
			pi->smc_state_table.GraphicsBootLevel = level;
			break;
		}
	}

	for (level = 0; level < dpm->dyn_state.vddc_dependency_on_mclk.count; level++) {
		if (dpm->dyn_state.vddc_dependency_on_mclk.entries[level].clk >=
		    boot_state->performance_levels[0].mclk) {
			pi->smc_state_table.MemoryBootLevel = level;
			break;
		}
	}
}

uint32_t ci_get_dpm_level_enable_mask_value(struct ci_single_dpm_table *dpm_table)
{
	uint32_t i;
	uint32_t mask_value = 0;

	for (i = dpm_table->count; i > 0; i--) {
		mask_value = mask_value << 1;
		if (dpm_table->dpm_levels[i-1].enabled)
			mask_value |= 0x1;
		else
			mask_value &= 0xFFFFFFFE;
	}

	return mask_value;
}

static void ci_populate_smc_link_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_dpm_table *dpm_table = &pi->dpm_table;
	uint32_t i;

	for (i = 0; i < dpm_table->pcie_speed_table.count; i++) {
		table->LinkLevel[i].PcieGenSpeed =
			(uint8_t)dpm_table->pcie_speed_table.dpm_levels[i].value;
		table->LinkLevel[i].PcieLaneCount =
			r600_encode_pci_lane_width(dpm_table->pcie_speed_table.dpm_levels[i].param1);
		table->LinkLevel[i].EnabledForActivity = 1;
		table->LinkLevel[i].DownT = cpu_to_be32(5);
		table->LinkLevel[i].UpT = cpu_to_be32(30);
	}

	pi->smc_state_table.LinkLevelCount = (uint8_t)dpm_table->pcie_speed_table.count;
	pi->dpm_level_enable_mask.pcie_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&dpm_table->pcie_speed_table);
}

/* ---- the UVD / VCE / ACP / SAMU levels (the tables are filled; the DPM for them stays off, rdn_ci_dpm.c) ---- */

static int ci_populate_smc_uvd_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	uint32_t count;
	struct atom_clock_dividers dividers;
	int ret = -EINVAL;

	table->UvdLevelCount =
		dpm->dyn_state.uvd_clock_voltage_dependency_table.count;

	for (count = 0; count < table->UvdLevelCount; count++) {
		table->UvdLevel[count].VclkFrequency =
			dpm->dyn_state.uvd_clock_voltage_dependency_table.entries[count].vclk;
		table->UvdLevel[count].DclkFrequency =
			dpm->dyn_state.uvd_clock_voltage_dependency_table.entries[count].dclk;
		table->UvdLevel[count].MinVddc =
			dpm->dyn_state.uvd_clock_voltage_dependency_table.entries[count].v * VOLTAGE_SCALE;
		table->UvdLevel[count].MinVddcPhases = 1;

		ret = radeon_atom_get_clock_dividers(dpm->card,
						     COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
						     table->UvdLevel[count].VclkFrequency, false, &dividers);
		if (ret)
			return ret;

		table->UvdLevel[count].VclkDivider = (uint8_t)dividers.post_divider;

		ret = radeon_atom_get_clock_dividers(dpm->card,
						     COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
						     table->UvdLevel[count].DclkFrequency, false, &dividers);
		if (ret)
			return ret;

		table->UvdLevel[count].DclkDivider = (uint8_t)dividers.post_divider;

		table->UvdLevel[count].VclkFrequency = cpu_to_be32(table->UvdLevel[count].VclkFrequency);
		table->UvdLevel[count].DclkFrequency = cpu_to_be32(table->UvdLevel[count].DclkFrequency);
		table->UvdLevel[count].MinVddc = cpu_to_be16(table->UvdLevel[count].MinVddc);
	}

	return ret;
}

static int ci_populate_smc_vce_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	uint32_t count;
	struct atom_clock_dividers dividers;
	int ret = -EINVAL;

	table->VceLevelCount =
		dpm->dyn_state.vce_clock_voltage_dependency_table.count;

	for (count = 0; count < table->VceLevelCount; count++) {
		table->VceLevel[count].Frequency =
			dpm->dyn_state.vce_clock_voltage_dependency_table.entries[count].evclk;
		table->VceLevel[count].MinVoltage =
			(uint16_t)dpm->dyn_state.vce_clock_voltage_dependency_table.entries[count].v * VOLTAGE_SCALE;
		table->VceLevel[count].MinPhases = 1;

		ret = radeon_atom_get_clock_dividers(dpm->card,
						     COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
						     table->VceLevel[count].Frequency, false, &dividers);
		if (ret)
			return ret;

		table->VceLevel[count].Divider = (uint8_t)dividers.post_divider;

		table->VceLevel[count].Frequency = cpu_to_be32(table->VceLevel[count].Frequency);
		table->VceLevel[count].MinVoltage = cpu_to_be16(table->VceLevel[count].MinVoltage);
	}

	return ret;

}

static int ci_populate_smc_acp_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	uint32_t count;
	struct atom_clock_dividers dividers;
	int ret = -EINVAL;

	table->AcpLevelCount = (uint8_t)
		(dpm->dyn_state.acp_clock_voltage_dependency_table.count);

	for (count = 0; count < table->AcpLevelCount; count++) {
		table->AcpLevel[count].Frequency =
			dpm->dyn_state.acp_clock_voltage_dependency_table.entries[count].clk;
		table->AcpLevel[count].MinVoltage =
			dpm->dyn_state.acp_clock_voltage_dependency_table.entries[count].v;
		table->AcpLevel[count].MinPhases = 1;

		ret = radeon_atom_get_clock_dividers(dpm->card,
						     COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
						     table->AcpLevel[count].Frequency, false, &dividers);
		if (ret)
			return ret;

		table->AcpLevel[count].Divider = (uint8_t)dividers.post_divider;

		table->AcpLevel[count].Frequency = cpu_to_be32(table->AcpLevel[count].Frequency);
		table->AcpLevel[count].MinVoltage = cpu_to_be16(table->AcpLevel[count].MinVoltage);
	}

	return ret;
}

static int ci_populate_smc_samu_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	uint32_t count;
	struct atom_clock_dividers dividers;
	int ret = -EINVAL;

	table->SamuLevelCount =
		dpm->dyn_state.samu_clock_voltage_dependency_table.count;

	for (count = 0; count < table->SamuLevelCount; count++) {
		table->SamuLevel[count].Frequency =
			dpm->dyn_state.samu_clock_voltage_dependency_table.entries[count].clk;
		table->SamuLevel[count].MinVoltage =
			dpm->dyn_state.samu_clock_voltage_dependency_table.entries[count].v * VOLTAGE_SCALE;
		table->SamuLevel[count].MinPhases = 1;

		ret = radeon_atom_get_clock_dividers(dpm->card,
						     COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK,
						     table->SamuLevel[count].Frequency, false, &dividers);
		if (ret)
			return ret;

		table->SamuLevel[count].Divider = (uint8_t)dividers.post_divider;

		table->SamuLevel[count].Frequency = cpu_to_be32(table->SamuLevel[count].Frequency);
		table->SamuLevel[count].MinVoltage = cpu_to_be16(table->SamuLevel[count].MinVoltage);
	}

	return ret;
}

/* ---- the memory levels ---- */

static int ci_calculate_mclk_params(struct rdn_ci_dpm *dpm,
				    uint32_t memory_clock,
				    SMU7_Discrete_MemoryLevel *mclk,
				    bool strobe_mode,
				    bool dll_state_on)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t  dll_cntl = pi->clock_registers.dll_cntl;
	uint32_t  mclk_pwrmgt_cntl = pi->clock_registers.mclk_pwrmgt_cntl;
	uint32_t  mpll_ad_func_cntl = pi->clock_registers.mpll_ad_func_cntl;
	uint32_t  mpll_dq_func_cntl = pi->clock_registers.mpll_dq_func_cntl;
	uint32_t  mpll_func_cntl = pi->clock_registers.mpll_func_cntl;
	uint32_t  mpll_func_cntl_1 = pi->clock_registers.mpll_func_cntl_1;
	uint32_t  mpll_func_cntl_2 = pi->clock_registers.mpll_func_cntl_2;
	uint32_t  mpll_ss1 = pi->clock_registers.mpll_ss1;
	uint32_t  mpll_ss2 = pi->clock_registers.mpll_ss2;
	struct atom_mpll_param mpll_param;
	int ret;

	ret = radeon_atom_get_memory_pll_dividers(dpm->card, memory_clock, strobe_mode, &mpll_param);
	if (ret)
		return ret;

	mpll_func_cntl &= ~BWCTRL_MASK;
	mpll_func_cntl |= BWCTRL(mpll_param.bwcntl);

	mpll_func_cntl_1 &= ~(CLKF_MASK | CLKFRAC_MASK | VCO_MODE_MASK);
	mpll_func_cntl_1 |= CLKF(mpll_param.clkf) |
		CLKFRAC(mpll_param.clkfrac) | VCO_MODE(mpll_param.vco_mode);

	mpll_ad_func_cntl &= ~YCLK_POST_DIV_MASK;
	mpll_ad_func_cntl |= YCLK_POST_DIV(mpll_param.post_div);

	if (pi->mem_gddr5) {
		mpll_dq_func_cntl &= ~(YCLK_SEL_MASK | YCLK_POST_DIV_MASK);
		mpll_dq_func_cntl |= YCLK_SEL(mpll_param.yclk_sel) |
			YCLK_POST_DIV(mpll_param.post_div);
	}

	if (pi->caps_mclk_ss_support) {
		struct radeon_atom_ss ss;
		uint32_t freq_nom;
		uint32_t tmp;
		uint32_t reference_clock = dpm->reference_freq;

		if (mpll_param.qdr == 1)
			freq_nom = memory_clock * 4 * (1 << mpll_param.post_div);
		else
			freq_nom = memory_clock * 2 * (1 << mpll_param.post_div);

		tmp = (freq_nom / reference_clock);
		tmp = tmp * tmp;
		if (radeon_atombios_get_asic_ss_info(dpm, &ss,
						     ASIC_INTERNAL_MEMORY_SS, freq_nom)) {
			uint32_t clks = reference_clock * 5 / ss.rate;
			uint32_t clkv = (uint32_t)((((131 * ss.percentage * ss.rate) / 100) * tmp) / freq_nom);

			mpll_ss1 &= ~CLKV_MASK;
			mpll_ss1 |= CLKV(clkv);

			mpll_ss2 &= ~CLKS_MASK;
			mpll_ss2 |= CLKS(clks);
		}
	}

	mclk_pwrmgt_cntl &= ~DLL_SPEED_MASK;
	mclk_pwrmgt_cntl |= DLL_SPEED(mpll_param.dll_speed);

	if (dll_state_on)
		mclk_pwrmgt_cntl |= MRDCK0_PDNB | MRDCK1_PDNB;
	else
		mclk_pwrmgt_cntl &= ~(MRDCK0_PDNB | MRDCK1_PDNB);

	mclk->MclkFrequency = memory_clock;
	mclk->MpllFuncCntl = mpll_func_cntl;
	mclk->MpllFuncCntl_1 = mpll_func_cntl_1;
	mclk->MpllFuncCntl_2 = mpll_func_cntl_2;
	mclk->MpllAdFuncCntl = mpll_ad_func_cntl;
	mclk->MpllDqFuncCntl = mpll_dq_func_cntl;
	mclk->MclkPwrmgtCntl = mclk_pwrmgt_cntl;
	mclk->DllCntl = dll_cntl;
	mclk->MpllSs1 = mpll_ss1;
	mclk->MpllSs2 = mpll_ss2;

	return 0;
}

static int ci_populate_single_memory_level(struct rdn_ci_dpm *dpm,
					   uint32_t memory_clock,
					   SMU7_Discrete_MemoryLevel *memory_level)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;
	bool dll_state_on;

	if (dpm->dyn_state.vddc_dependency_on_mclk.entries) {
		ret = ci_get_dependency_volt_by_clk(&dpm->dyn_state.vddc_dependency_on_mclk,
						    memory_clock, &memory_level->MinVddc);
		if (ret)
			return ret;
	}

	if (dpm->dyn_state.vddci_dependency_on_mclk.entries) {
		ret = ci_get_dependency_volt_by_clk(&dpm->dyn_state.vddci_dependency_on_mclk,
						    memory_clock, &memory_level->MinVddci);
		if (ret)
			return ret;
	}

	if (dpm->dyn_state.mvdd_dependency_on_mclk.entries) {
		ret = ci_get_dependency_volt_by_clk(&dpm->dyn_state.mvdd_dependency_on_mclk,
						    memory_clock, &memory_level->MinMvdd);
		if (ret)
			return ret;
	}

	memory_level->MinVddcPhases = 1;

	if (pi->vddc_phase_shed_control)
		ci_populate_phase_value_based_on_mclk(&dpm->dyn_state.phase_shedding_limits_table,
						      memory_clock,
						      &memory_level->MinVddcPhases);

	memory_level->EnabledForThrottle = 1;
	memory_level->UpH = 0;
	memory_level->DownH = 100;
	memory_level->VoltageDownH = 0;
	memory_level->ActivityLevel = (uint16_t)pi->mclk_activity_target;

	memory_level->StutterEnable = false;
	memory_level->StrobeEnable = false;
	memory_level->EdcReadEnable = false;
	memory_level->EdcWriteEnable = false;
	memory_level->RttEnable = false;

	memory_level->DisplayWatermark = PPSMC_DISPLAY_WATERMARK_LOW;

	if (pi->mclk_stutter_mode_threshold &&
	    (memory_clock <= pi->mclk_stutter_mode_threshold) &&
	    (pi->uvd_enabled == false) &&
	    (RREG32(dpm->card, DPG_PIPE_STUTTER_CONTROL) & STUTTER_ENABLE) &&
	    (dpm->new_active_crtc_count <= 2))
		memory_level->StutterEnable = true;

	if (pi->mclk_strobe_mode_threshold &&
	    (memory_clock <= pi->mclk_strobe_mode_threshold))
		memory_level->StrobeEnable = 1;

	if (pi->mem_gddr5) {
		memory_level->StrobeRatio =
			si_get_mclk_frequency_ratio(memory_clock, memory_level->StrobeEnable);
		if (pi->mclk_edc_enable_threshold &&
		    (memory_clock > pi->mclk_edc_enable_threshold))
			memory_level->EdcReadEnable = true;

		if (pi->mclk_edc_wr_enable_threshold &&
		    (memory_clock > pi->mclk_edc_wr_enable_threshold))
			memory_level->EdcWriteEnable = true;

		if (memory_level->StrobeEnable) {
			if (si_get_mclk_frequency_ratio(memory_clock, true) >=
			    ((RREG32(dpm->card, MC_SEQ_MISC7) >> 16) & 0xf))
				dll_state_on = ((RREG32(dpm->card, MC_SEQ_MISC5) >> 1) & 0x1) ? true : false;
			else
				dll_state_on = ((RREG32(dpm->card, MC_SEQ_MISC6) >> 1) & 0x1) ? true : false;
		} else {
			dll_state_on = pi->dll_default_on;
		}
	} else {
		memory_level->StrobeRatio = si_get_ddr3_mclk_frequency_ratio(memory_clock);
		dll_state_on = ((RREG32(dpm->card, MC_SEQ_MISC5) >> 1) & 0x1) ? true : false;
	}

	ret = ci_calculate_mclk_params(dpm, memory_clock, memory_level, memory_level->StrobeEnable, dll_state_on);
	if (ret)
		return ret;

	memory_level->MinVddc = cpu_to_be32(memory_level->MinVddc * VOLTAGE_SCALE);
	memory_level->MinVddcPhases = cpu_to_be32(memory_level->MinVddcPhases);
	memory_level->MinVddci = cpu_to_be32(memory_level->MinVddci * VOLTAGE_SCALE);
	memory_level->MinMvdd = cpu_to_be32(memory_level->MinMvdd * VOLTAGE_SCALE);

	memory_level->MclkFrequency = cpu_to_be32(memory_level->MclkFrequency);
	memory_level->ActivityLevel = cpu_to_be16(memory_level->ActivityLevel);
	memory_level->MpllFuncCntl = cpu_to_be32(memory_level->MpllFuncCntl);
	memory_level->MpllFuncCntl_1 = cpu_to_be32(memory_level->MpllFuncCntl_1);
	memory_level->MpllFuncCntl_2 = cpu_to_be32(memory_level->MpllFuncCntl_2);
	memory_level->MpllAdFuncCntl = cpu_to_be32(memory_level->MpllAdFuncCntl);
	memory_level->MpllDqFuncCntl = cpu_to_be32(memory_level->MpllDqFuncCntl);
	memory_level->MclkPwrmgtCntl = cpu_to_be32(memory_level->MclkPwrmgtCntl);
	memory_level->DllCntl = cpu_to_be32(memory_level->DllCntl);
	memory_level->MpllSs1 = cpu_to_be32(memory_level->MpllSs1);
	memory_level->MpllSs2 = cpu_to_be32(memory_level->MpllSs2);

	return 0;
}

static int ci_populate_smc_acpi_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_DpmTable *table)
{
	struct ci_power_info *pi = &dpm->pi;
	struct atom_clock_dividers dividers;
	SMU7_Discrete_VoltageLevel voltage_level;
	uint32_t spll_func_cntl = pi->clock_registers.cg_spll_func_cntl;
	uint32_t spll_func_cntl_2 = pi->clock_registers.cg_spll_func_cntl_2;
	uint32_t dll_cntl = pi->clock_registers.dll_cntl;
	uint32_t mclk_pwrmgt_cntl = pi->clock_registers.mclk_pwrmgt_cntl;
	int ret;

	memset(&voltage_level, 0, sizeof(voltage_level));
	table->ACPILevel.Flags &= ~PPSMC_SWSTATE_FLAG_DC;

	if (pi->acpi_vddc)
		table->ACPILevel.MinVddc = cpu_to_be32(pi->acpi_vddc * VOLTAGE_SCALE);
	else
		table->ACPILevel.MinVddc = cpu_to_be32(pi->min_vddc_in_pp_table * VOLTAGE_SCALE);

	table->ACPILevel.MinVddcPhases = pi->vddc_phase_shed_control ? 0 : 1;

	table->ACPILevel.SclkFrequency = dpm->reference_freq;

	ret = radeon_atom_get_clock_dividers(dpm->card,
					     COMPUTE_GPUCLK_INPUT_FLAG_SCLK,
					     table->ACPILevel.SclkFrequency, false, &dividers);
	if (ret)
		return ret;

	table->ACPILevel.SclkDid = (uint8_t)dividers.post_divider;
	table->ACPILevel.DisplayWatermark = PPSMC_DISPLAY_WATERMARK_LOW;
	table->ACPILevel.DeepSleepDivId = 0;

	spll_func_cntl &= ~SPLL_PWRON;
	spll_func_cntl |= SPLL_RESET;

	spll_func_cntl_2 &= ~SCLK_MUX_SEL_MASK;
	spll_func_cntl_2 |= SCLK_MUX_SEL(4);

	table->ACPILevel.CgSpllFuncCntl = spll_func_cntl;
	table->ACPILevel.CgSpllFuncCntl2 = spll_func_cntl_2;
	table->ACPILevel.CgSpllFuncCntl3 = pi->clock_registers.cg_spll_func_cntl_3;
	table->ACPILevel.CgSpllFuncCntl4 = pi->clock_registers.cg_spll_func_cntl_4;
	table->ACPILevel.SpllSpreadSpectrum = pi->clock_registers.cg_spll_spread_spectrum;
	table->ACPILevel.SpllSpreadSpectrum2 = pi->clock_registers.cg_spll_spread_spectrum_2;
	table->ACPILevel.CcPwrDynRm = 0;
	table->ACPILevel.CcPwrDynRm1 = 0;

	table->ACPILevel.Flags = cpu_to_be32(table->ACPILevel.Flags);
	table->ACPILevel.MinVddcPhases = cpu_to_be32(table->ACPILevel.MinVddcPhases);
	table->ACPILevel.SclkFrequency = cpu_to_be32(table->ACPILevel.SclkFrequency);
	table->ACPILevel.CgSpllFuncCntl = cpu_to_be32(table->ACPILevel.CgSpllFuncCntl);
	table->ACPILevel.CgSpllFuncCntl2 = cpu_to_be32(table->ACPILevel.CgSpllFuncCntl2);
	table->ACPILevel.CgSpllFuncCntl3 = cpu_to_be32(table->ACPILevel.CgSpllFuncCntl3);
	table->ACPILevel.CgSpllFuncCntl4 = cpu_to_be32(table->ACPILevel.CgSpllFuncCntl4);
	table->ACPILevel.SpllSpreadSpectrum = cpu_to_be32(table->ACPILevel.SpllSpreadSpectrum);
	table->ACPILevel.SpllSpreadSpectrum2 = cpu_to_be32(table->ACPILevel.SpllSpreadSpectrum2);
	table->ACPILevel.CcPwrDynRm = cpu_to_be32(table->ACPILevel.CcPwrDynRm);
	table->ACPILevel.CcPwrDynRm1 = cpu_to_be32(table->ACPILevel.CcPwrDynRm1);

	table->MemoryACPILevel.MinVddc = table->ACPILevel.MinVddc;
	table->MemoryACPILevel.MinVddcPhases = table->ACPILevel.MinVddcPhases;

	if (pi->vddci_control != CISLANDS_VOLTAGE_CONTROL_NONE) {
		if (pi->acpi_vddci)
			table->MemoryACPILevel.MinVddci =
				cpu_to_be32(pi->acpi_vddci * VOLTAGE_SCALE);
		else
			table->MemoryACPILevel.MinVddci =
				cpu_to_be32(pi->min_vddci_in_pp_table * VOLTAGE_SCALE);
	}

	if (ci_populate_mvdd_value(dpm, 0, &voltage_level))
		table->MemoryACPILevel.MinMvdd = 0;
	else
		table->MemoryACPILevel.MinMvdd =
			cpu_to_be32(voltage_level.Voltage * VOLTAGE_SCALE);

	mclk_pwrmgt_cntl |= MRDCK0_RESET | MRDCK1_RESET;
	mclk_pwrmgt_cntl &= ~(MRDCK0_PDNB | MRDCK1_PDNB);

	dll_cntl &= ~(MRDCK0_BYPASS | MRDCK1_BYPASS);

	table->MemoryACPILevel.DllCntl = cpu_to_be32(dll_cntl);
	table->MemoryACPILevel.MclkPwrmgtCntl = cpu_to_be32(mclk_pwrmgt_cntl);
	table->MemoryACPILevel.MpllAdFuncCntl =
		cpu_to_be32(pi->clock_registers.mpll_ad_func_cntl);
	table->MemoryACPILevel.MpllDqFuncCntl =
		cpu_to_be32(pi->clock_registers.mpll_dq_func_cntl);
	table->MemoryACPILevel.MpllFuncCntl =
		cpu_to_be32(pi->clock_registers.mpll_func_cntl);
	table->MemoryACPILevel.MpllFuncCntl_1 =
		cpu_to_be32(pi->clock_registers.mpll_func_cntl_1);
	table->MemoryACPILevel.MpllFuncCntl_2 =
		cpu_to_be32(pi->clock_registers.mpll_func_cntl_2);
	table->MemoryACPILevel.MpllSs1 = cpu_to_be32(pi->clock_registers.mpll_ss1);
	table->MemoryACPILevel.MpllSs2 = cpu_to_be32(pi->clock_registers.mpll_ss2);

	table->MemoryACPILevel.EnabledForThrottle = 0;
	table->MemoryACPILevel.EnabledForActivity = 0;
	table->MemoryACPILevel.UpH = 0;
	table->MemoryACPILevel.DownH = 100;
	table->MemoryACPILevel.VoltageDownH = 0;
	table->MemoryACPILevel.ActivityLevel =
		cpu_to_be16((uint16_t)pi->mclk_activity_target);

	table->MemoryACPILevel.StutterEnable = false;
	table->MemoryACPILevel.StrobeEnable = false;
	table->MemoryACPILevel.EdcReadEnable = false;
	table->MemoryACPILevel.EdcWriteEnable = false;
	table->MemoryACPILevel.RttEnable = false;

	return 0;
}

/* ---- ULV ---- */

static int ci_populate_ulv_level(struct rdn_ci_dpm *dpm, SMU7_Discrete_Ulv *state)
{
	struct ci_power_info *pi = &dpm->pi;
	uint16_t ulv_voltage = dpm->backbias_response_time;

	state->CcPwrDynRm = 0;
	state->CcPwrDynRm1 = 0;

	if (ulv_voltage == 0) {
		pi->ulv.supported = false;
		return 0;
	}

	if (pi->voltage_control != CISLANDS_VOLTAGE_CONTROL_BY_SVID2) {
		if (ulv_voltage > dpm->dyn_state.vddc_dependency_on_sclk.entries[0].v)
			state->VddcOffset = 0;
		else
			state->VddcOffset =
				dpm->dyn_state.vddc_dependency_on_sclk.entries[0].v - ulv_voltage;
	} else {
		if (ulv_voltage > dpm->dyn_state.vddc_dependency_on_sclk.entries[0].v)
			state->VddcOffsetVid = 0;
		else
			state->VddcOffsetVid = (uint8_t)
				((dpm->dyn_state.vddc_dependency_on_sclk.entries[0].v - ulv_voltage) *
				 VOLTAGE_VID_OFFSET_SCALE2 / VOLTAGE_VID_OFFSET_SCALE1);
	}
	state->VddcPhase = pi->vddc_phase_shed_control ? 0 : 1;

	state->CcPwrDynRm = cpu_to_be32(state->CcPwrDynRm);
	state->CcPwrDynRm1 = cpu_to_be32(state->CcPwrDynRm1);
	state->VddcOffset = cpu_to_be16(state->VddcOffset);

	return 0;
}

static int ci_populate_ulv_state(struct rdn_ci_dpm *dpm, SMU7_Discrete_Ulv *ulv_level)
{
	return ci_populate_ulv_level(dpm, ulv_level);
}

/* ---- the graphics levels ---- */

static int ci_calculate_sclk_params(struct rdn_ci_dpm *dpm,
				    uint32_t engine_clock,
				    SMU7_Discrete_GraphicsLevel *sclk)
{
	struct ci_power_info *pi = &dpm->pi;
	struct atom_clock_dividers dividers;
	uint32_t spll_func_cntl_3 = pi->clock_registers.cg_spll_func_cntl_3;
	uint32_t spll_func_cntl_4 = pi->clock_registers.cg_spll_func_cntl_4;
	uint32_t cg_spll_spread_spectrum = pi->clock_registers.cg_spll_spread_spectrum;
	uint32_t cg_spll_spread_spectrum_2 = pi->clock_registers.cg_spll_spread_spectrum_2;
	uint32_t reference_clock = dpm->reference_freq;
	uint32_t reference_divider;
	uint32_t fbdiv;
	int ret;

	ret = radeon_atom_get_clock_dividers(dpm->card,
					     COMPUTE_GPUCLK_INPUT_FLAG_SCLK,
					     engine_clock, false, &dividers);
	if (ret)
		return ret;

	reference_divider = 1 + dividers.ref_div;
	fbdiv = dividers.fb_div & 0x3FFFFFF;

	spll_func_cntl_3 &= ~SPLL_FB_DIV_MASK;
	spll_func_cntl_3 |= SPLL_FB_DIV(fbdiv);
	spll_func_cntl_3 |= SPLL_DITHEN;

	if (pi->caps_sclk_ss_support) {
		struct radeon_atom_ss ss;
		uint32_t vco_freq = engine_clock * dividers.post_div;

		if (radeon_atombios_get_asic_ss_info(dpm, &ss,
						     ASIC_INTERNAL_ENGINE_SS, vco_freq)) {
			uint32_t clk_s = reference_clock * 5 / (reference_divider * ss.rate);
			uint32_t clk_v = 4 * ss.percentage * fbdiv / (clk_s * 10000);

			cg_spll_spread_spectrum &= ~CLK_S_MASK;
			cg_spll_spread_spectrum |= CLK_S(clk_s);
			cg_spll_spread_spectrum |= SSEN;

			cg_spll_spread_spectrum_2 &= ~CLK_V_MASK;
			cg_spll_spread_spectrum_2 |= CLK_V(clk_v);
		}
	}

	sclk->SclkFrequency = engine_clock;
	sclk->CgSpllFuncCntl3 = spll_func_cntl_3;
	sclk->CgSpllFuncCntl4 = spll_func_cntl_4;
	sclk->SpllSpreadSpectrum = cg_spll_spread_spectrum;
	sclk->SpllSpreadSpectrum2  = cg_spll_spread_spectrum_2;
	sclk->SclkDid = (uint8_t)dividers.post_divider;

	return 0;
}

static int ci_populate_single_graphic_level(struct rdn_ci_dpm *dpm,
					    uint32_t engine_clock,
					    uint16_t sclk_activity_level_t,
					    SMU7_Discrete_GraphicsLevel *graphic_level)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	ret = ci_calculate_sclk_params(dpm, engine_clock, graphic_level);
	if (ret)
		return ret;

	ret = ci_get_dependency_volt_by_clk(&dpm->dyn_state.vddc_dependency_on_sclk,
					    engine_clock, &graphic_level->MinVddc);
	if (ret)
		return ret;

	graphic_level->SclkFrequency = engine_clock;

	graphic_level->Flags =  0;
	graphic_level->MinVddcPhases = 1;

	if (pi->vddc_phase_shed_control)
		ci_populate_phase_value_based_on_sclk(&dpm->dyn_state.phase_shedding_limits_table,
						      engine_clock,
						      &graphic_level->MinVddcPhases);

	graphic_level->ActivityLevel = sclk_activity_level_t;

	graphic_level->CcPwrDynRm = 0;
	graphic_level->CcPwrDynRm1 = 0;
	graphic_level->EnabledForThrottle = 1;
	graphic_level->UpH = 0;
	graphic_level->DownH = 0;
	graphic_level->VoltageDownH = 0;
	graphic_level->PowerThrottle = 0;

	if (pi->caps_sclk_ds)
		graphic_level->DeepSleepDivId = ci_get_sleep_divider_id_from_clock(engine_clock,
										   CISLAND_MINIMUM_ENGINE_CLOCK);

	graphic_level->DisplayWatermark = PPSMC_DISPLAY_WATERMARK_LOW;

	graphic_level->Flags = cpu_to_be32(graphic_level->Flags);
	graphic_level->MinVddc = cpu_to_be32(graphic_level->MinVddc * VOLTAGE_SCALE);
	graphic_level->MinVddcPhases = cpu_to_be32(graphic_level->MinVddcPhases);
	graphic_level->SclkFrequency = cpu_to_be32(graphic_level->SclkFrequency);
	graphic_level->ActivityLevel = cpu_to_be16(graphic_level->ActivityLevel);
	graphic_level->CgSpllFuncCntl3 = cpu_to_be32(graphic_level->CgSpllFuncCntl3);
	graphic_level->CgSpllFuncCntl4 = cpu_to_be32(graphic_level->CgSpllFuncCntl4);
	graphic_level->SpllSpreadSpectrum = cpu_to_be32(graphic_level->SpllSpreadSpectrum);
	graphic_level->SpllSpreadSpectrum2 = cpu_to_be32(graphic_level->SpllSpreadSpectrum2);
	graphic_level->CcPwrDynRm = cpu_to_be32(graphic_level->CcPwrDynRm);
	graphic_level->CcPwrDynRm1 = cpu_to_be32(graphic_level->CcPwrDynRm1);

	return 0;
}

int ci_populate_all_graphic_levels(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_dpm_table *dpm_table = &pi->dpm_table;
	uint32_t level_array_address = pi->dpm_table_start +
		offsetof(SMU7_Discrete_DpmTable, GraphicsLevel);
	uint32_t level_array_size = sizeof(SMU7_Discrete_GraphicsLevel) *
		SMU7_MAX_LEVELS_GRAPHICS;
	SMU7_Discrete_GraphicsLevel *levels = pi->smc_state_table.GraphicsLevel;
	int ret;
	uint32_t i;

	memset(levels, 0, level_array_size);

	for (i = 0; i < dpm_table->sclk_table.count; i++) {
		ret = ci_populate_single_graphic_level(dpm,
						       dpm_table->sclk_table.dpm_levels[i].value,
						       (uint16_t)pi->activity_target[i],
						       &pi->smc_state_table.GraphicsLevel[i]);
		if (ret)
			return ret;
		if (i > 1)
			pi->smc_state_table.GraphicsLevel[i].DeepSleepDivId = 0;
		if (i == (dpm_table->sclk_table.count - 1))
			pi->smc_state_table.GraphicsLevel[i].DisplayWatermark =
				PPSMC_DISPLAY_WATERMARK_HIGH;
	}
	pi->smc_state_table.GraphicsLevel[0].EnabledForActivity = 1;

	pi->smc_state_table.GraphicsDpmLevelCount = (uint8_t)dpm_table->sclk_table.count;
	pi->dpm_level_enable_mask.sclk_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&dpm_table->sclk_table);

	ret = ci_copy_bytes_to_smc(dpm, level_array_address,
				   (uint8_t *)levels, level_array_size,
				   pi->sram_end);
	if (ret)
		return ret;

	return 0;
}

int ci_populate_all_memory_levels(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_dpm_table *dpm_table = &pi->dpm_table;
	uint32_t level_array_address = pi->dpm_table_start +
		offsetof(SMU7_Discrete_DpmTable, MemoryLevel);
	uint32_t level_array_size = sizeof(SMU7_Discrete_MemoryLevel) *
		SMU7_MAX_LEVELS_MEMORY;
	SMU7_Discrete_MemoryLevel *levels = pi->smc_state_table.MemoryLevel;
	int ret;
	uint32_t i;

	memset(levels, 0, level_array_size);

	for (i = 0; i < dpm_table->mclk_table.count; i++) {
		if (dpm_table->mclk_table.dpm_levels[i].value == 0)
			return -EINVAL;
		ret = ci_populate_single_memory_level(dpm,
						      dpm_table->mclk_table.dpm_levels[i].value,
						      &pi->smc_state_table.MemoryLevel[i]);
		if (ret)
			return ret;
	}

	pi->smc_state_table.MemoryLevel[0].EnabledForActivity = 1;

	if ((dpm_table->mclk_table.count >= 2) &&
	    ((dpm->card->device == 0x67B0) || (dpm->card->device == 0x67B1))) {
		pi->smc_state_table.MemoryLevel[1].MinVddc =
			pi->smc_state_table.MemoryLevel[0].MinVddc;
		pi->smc_state_table.MemoryLevel[1].MinVddcPhases =
			pi->smc_state_table.MemoryLevel[0].MinVddcPhases;
	}

	pi->smc_state_table.MemoryLevel[0].ActivityLevel = cpu_to_be16(0x1F);

	pi->smc_state_table.MemoryDpmLevelCount = (uint8_t)dpm_table->mclk_table.count;
	pi->dpm_level_enable_mask.mclk_dpm_enable_mask =
		ci_get_dpm_level_enable_mask_value(&dpm_table->mclk_table);

	pi->smc_state_table.MemoryLevel[dpm_table->mclk_table.count - 1].DisplayWatermark =
		PPSMC_DISPLAY_WATERMARK_HIGH;

	ret = ci_copy_bytes_to_smc(dpm, level_array_address,
				   (uint8_t *)levels, level_array_size,
				   pi->sram_end);
	if (ret)
		return ret;

	return 0;
}

/* ---- the default DPM tables ---- */

static void ci_reset_single_dpm_table(struct ci_single_dpm_table *dpm_table, uint32_t count)
{
	uint32_t i;

	dpm_table->count = count;
	for (i = 0; i < MAX_REGULAR_DPM_NUMBER; i++)
		dpm_table->dpm_levels[i].enabled = false;
}

static void ci_setup_pcie_table_entry(struct ci_single_dpm_table *dpm_table,
				      uint32_t index, uint32_t pcie_gen, uint32_t pcie_lanes)
{
	dpm_table->dpm_levels[index].value = pcie_gen;
	dpm_table->dpm_levels[index].param1 = pcie_lanes;
	dpm_table->dpm_levels[index].enabled = true;
}

static int ci_setup_default_pcie_tables(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	if (!pi->use_pcie_performance_levels && !pi->use_pcie_powersaving_levels)
		return -EINVAL;

	if (pi->use_pcie_performance_levels && !pi->use_pcie_powersaving_levels) {
		pi->pcie_gen_powersaving = pi->pcie_gen_performance;
		pi->pcie_lane_powersaving = pi->pcie_lane_performance;
	} else if (!pi->use_pcie_performance_levels && pi->use_pcie_powersaving_levels) {
		pi->pcie_gen_performance = pi->pcie_gen_powersaving;
		pi->pcie_lane_performance = pi->pcie_lane_powersaving;
	}

	ci_reset_single_dpm_table(&pi->dpm_table.pcie_speed_table,
				  SMU7_MAX_LEVELS_LINK);

	if (dpm->card->chip && dpm->card->chip->chip == RDN_CHIP_BONAIRE)
		ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 0,
					  pi->pcie_gen_powersaving.min,
					  pi->pcie_lane_powersaving.max);
	else
		ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 0,
					  pi->pcie_gen_powersaving.min,
					  pi->pcie_lane_powersaving.min);
	ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 1,
				  pi->pcie_gen_performance.min,
				  pi->pcie_lane_performance.min);
	ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 2,
				  pi->pcie_gen_powersaving.min,
				  pi->pcie_lane_powersaving.max);
	ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 3,
				  pi->pcie_gen_performance.min,
				  pi->pcie_lane_performance.max);
	ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 4,
				  pi->pcie_gen_powersaving.max,
				  pi->pcie_lane_powersaving.max);
	ci_setup_pcie_table_entry(&pi->dpm_table.pcie_speed_table, 5,
				  pi->pcie_gen_performance.max,
				  pi->pcie_lane_performance.max);

	pi->dpm_table.pcie_speed_table.count = 6;

	return 0;
}

int ci_setup_default_dpm_tables(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_ci_dep_table *allowed_sclk_vddc_table =
		&dpm->dyn_state.vddc_dependency_on_sclk;
	struct rdn_ci_dep_table *allowed_mclk_table =
		&dpm->dyn_state.vddc_dependency_on_mclk;
	struct rdn_ci_cac_leakage_table *std_voltage_table =
		&dpm->dyn_state.cac_leakage_table;
	uint32_t i;

	if (allowed_sclk_vddc_table->count < 1)
		return -EINVAL;
	if (allowed_mclk_table->count < 1)
		return -EINVAL;
	/* Linux indexes dpm_levels[] by the dependency count without a bound; the tables are 8 deep */
	if (allowed_sclk_vddc_table->count > MAX_REGULAR_DPM_NUMBER ||
	    allowed_mclk_table->count > MAX_REGULAR_DPM_NUMBER ||
	    dpm->dyn_state.vddci_dependency_on_mclk.count > MAX_REGULAR_DPM_NUMBER ||
	    dpm->dyn_state.mvdd_dependency_on_mclk.count > MAX_REGULAR_DPM_NUMBER) {
		rdn_log(dpm->card->os, RDN_LOG_ERROR, "ci_dpm: a dependency table has more than %u entries", MAX_REGULAR_DPM_NUMBER);
		return -EINVAL;
	}

	memset(&pi->dpm_table, 0, sizeof(struct ci_dpm_table));

	ci_reset_single_dpm_table(&pi->dpm_table.sclk_table,
				  SMU7_MAX_LEVELS_GRAPHICS);
	ci_reset_single_dpm_table(&pi->dpm_table.mclk_table,
				  SMU7_MAX_LEVELS_MEMORY);
	ci_reset_single_dpm_table(&pi->dpm_table.vddc_table,
				  SMU7_MAX_LEVELS_VDDC);
	ci_reset_single_dpm_table(&pi->dpm_table.vddci_table,
				  SMU7_MAX_LEVELS_VDDCI);
	ci_reset_single_dpm_table(&pi->dpm_table.mvdd_table,
				  SMU7_MAX_LEVELS_MVDD);

	pi->dpm_table.sclk_table.count = 0;
	for (i = 0; i < allowed_sclk_vddc_table->count; i++) {
		if ((i == 0) ||
		    (pi->dpm_table.sclk_table.dpm_levels[pi->dpm_table.sclk_table.count-1].value !=
		     allowed_sclk_vddc_table->entries[i].clk)) {
			pi->dpm_table.sclk_table.dpm_levels[pi->dpm_table.sclk_table.count].value =
				allowed_sclk_vddc_table->entries[i].clk;
			pi->dpm_table.sclk_table.dpm_levels[pi->dpm_table.sclk_table.count].enabled =
				i == 0;
			pi->dpm_table.sclk_table.count++;
		}
	}

	pi->dpm_table.mclk_table.count = 0;
	for (i = 0; i < allowed_mclk_table->count; i++) {
		if ((i == 0) ||
		    (pi->dpm_table.mclk_table.dpm_levels[pi->dpm_table.mclk_table.count-1].value !=
		     allowed_mclk_table->entries[i].clk)) {
			pi->dpm_table.mclk_table.dpm_levels[pi->dpm_table.mclk_table.count].value =
				allowed_mclk_table->entries[i].clk;
			pi->dpm_table.mclk_table.dpm_levels[pi->dpm_table.mclk_table.count].enabled =
				i == 0;
			pi->dpm_table.mclk_table.count++;
		}
	}

	for (i = 0; i < allowed_sclk_vddc_table->count; i++) {
		pi->dpm_table.vddc_table.dpm_levels[i].value =
			allowed_sclk_vddc_table->entries[i].v;
		pi->dpm_table.vddc_table.dpm_levels[i].param1 =
			(i < std_voltage_table->count) ? std_voltage_table->entries[i].leakage : 0;
		pi->dpm_table.vddc_table.dpm_levels[i].enabled = true;
	}
	pi->dpm_table.vddc_table.count = allowed_sclk_vddc_table->count;

	allowed_mclk_table = &dpm->dyn_state.vddci_dependency_on_mclk;
	for (i = 0; i < allowed_mclk_table->count; i++) {
		pi->dpm_table.vddci_table.dpm_levels[i].value =
			allowed_mclk_table->entries[i].v;
		pi->dpm_table.vddci_table.dpm_levels[i].enabled = true;
	}
	pi->dpm_table.vddci_table.count = allowed_mclk_table->count;

	allowed_mclk_table = &dpm->dyn_state.mvdd_dependency_on_mclk;
	for (i = 0; i < allowed_mclk_table->count; i++) {
		pi->dpm_table.mvdd_table.dpm_levels[i].value =
			allowed_mclk_table->entries[i].v;
		pi->dpm_table.mvdd_table.dpm_levels[i].enabled = true;
	}
	pi->dpm_table.mvdd_table.count = allowed_mclk_table->count;

	ci_setup_default_pcie_tables(dpm);

	return 0;
}

static int ci_find_boot_level(struct ci_single_dpm_table *table,
			      uint32_t value, uint32_t *boot_level)
{
	uint32_t i;
	int ret = -EINVAL;

	for (i = 0; i < table->count; i++) {
		if (value == table->dpm_levels[i].value) {
			*boot_level = i;
			ret = 0;
		}
	}

	return ret;
}

static void ci_init_fps_limits(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	SMU7_Discrete_DpmTable *table = &pi->smc_state_table;

	if (pi->caps_fps) {
		uint16_t tmp;

		tmp = 45;
		table->FpsHighT = cpu_to_be16(tmp);

		tmp = 30;
		table->FpsLowT = cpu_to_be16(tmp);
	}
}

/* ---- ci_init_smc_table(): the whole SMU7_Discrete_DpmTable ---- */

int ci_init_smc_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_ulv_parm *ulv = &pi->ulv;
	struct rdn_ci_rps *radeon_boot_state;
	SMU7_Discrete_DpmTable *table = &pi->smc_state_table;
	uint32_t boot_level;
	int ret;

	if (dpm->boot_ps < 0) {
		rdn_log(dpm->card->os, RDN_LOG_ERROR, "ci_dpm: the PowerPlay table has no boot state");
		return -EINVAL;
	}
	radeon_boot_state = &dpm->ps[dpm->boot_ps];

	ret = ci_setup_default_dpm_tables(dpm);
	if (ret)
		return ret;

	if (pi->voltage_control != CISLANDS_VOLTAGE_CONTROL_NONE)
		ci_populate_smc_voltage_tables(dpm, table);

	ci_init_fps_limits(dpm);

	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_HARDWAREDC)
		table->SystemFlags |= PPSMC_SYSTEMFLAG_GPIO_DC;

	if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_STEPVDDC)
		table->SystemFlags |= PPSMC_SYSTEMFLAG_STEPVDDC;

	if (pi->mem_gddr5)
		table->SystemFlags |= PPSMC_SYSTEMFLAG_GDDR5;

	if (ulv->supported) {
		ret = ci_populate_ulv_state(dpm, &pi->smc_state_table.Ulv);
		if (ret)
			return ret;
		WREG32_SMC(dpm->card, CG_ULV_PARAMETER, ulv->cg_ulv_parameter);
	}

	ret = ci_populate_all_graphic_levels(dpm);
	if (ret)
		return ret;

	ret = ci_populate_all_memory_levels(dpm);
	if (ret)
		return ret;

	ci_populate_smc_link_level(dpm, table);

	ret = ci_populate_smc_acpi_level(dpm, table);
	if (ret)
		return ret;

	ret = ci_populate_smc_vce_level(dpm, table);
	if (ret)
		return ret;

	ret = ci_populate_smc_acp_level(dpm, table);
	if (ret)
		return ret;

	ret = ci_populate_smc_samu_level(dpm, table);
	if (ret)
		return ret;

	ret = ci_do_program_memory_timing_parameters(dpm);
	if (ret)
		return ret;

	ret = ci_populate_smc_uvd_level(dpm, table);
	if (ret)
		return ret;

	table->UvdBootLevel  = 0;
	table->VceBootLevel  = 0;
	table->AcpBootLevel  = 0;
	table->SamuBootLevel  = 0;
	table->GraphicsBootLevel  = 0;
	table->MemoryBootLevel  = 0;

	/*
	 * Linux passes (u32 *)&GraphicsBootLevel, a u8, so on its
	 * little-endian hosts the level lands in the byte and the three
	 * fields after it are zeroed (and set again below). The intent, the
	 * level into the byte, is spelled out here.
	 */
	boot_level = table->GraphicsBootLevel;
	ret = ci_find_boot_level(&pi->dpm_table.sclk_table,
				 pi->vbios_boot_state.sclk_bootup_value,
				 &boot_level);
	table->GraphicsBootLevel = (uint8_t)boot_level;

	boot_level = table->MemoryBootLevel;
	ret = ci_find_boot_level(&pi->dpm_table.mclk_table,
				 pi->vbios_boot_state.mclk_bootup_value,
				 &boot_level);
	table->MemoryBootLevel = (uint8_t)boot_level;

	table->BootVddc = pi->vbios_boot_state.vddc_bootup_value;
	table->BootVddci = pi->vbios_boot_state.vddci_bootup_value;
	table->BootMVdd = pi->vbios_boot_state.mvdd_bootup_value;

	ci_populate_smc_initial_state(dpm, radeon_boot_state);

	ret = ci_populate_bapm_parameters_in_dpm_table(dpm);
	if (ret)
		return ret;

	table->UVDInterval = 1;
	table->VCEInterval = 1;
	table->ACPInterval = 1;
	table->SAMUInterval = 1;
	table->GraphicsVoltageChangeEnable = 1;
	table->GraphicsThermThrottleEnable = 1;
	table->GraphicsInterval = 1;
	table->VoltageInterval = 1;
	table->ThermalInterval = 1;
	table->TemperatureLimitHigh = (uint16_t)((pi->thermal_temp_setting.temperature_high *
					     CISLANDS_Q88_FORMAT_CONVERSION_UNIT) / 1000);
	table->TemperatureLimitLow = (uint16_t)((pi->thermal_temp_setting.temperature_low *
					    CISLANDS_Q88_FORMAT_CONVERSION_UNIT) / 1000);
	table->MemoryVoltageChangeEnable = 1;
	table->MemoryInterval = 1;
	table->VoltageResponseTime = 0;
	table->VddcVddciDelta = 4000;
	table->PhaseResponseTime = 0;
	table->MemoryThermThrottleEnable = 1;
	table->PCIeBootLinkLevel = pi->dpm_table.pcie_speed_table.count - 1;
	table->PCIeGenInterval = 1;
	if (pi->voltage_control == CISLANDS_VOLTAGE_CONTROL_BY_SVID2)
		table->SVI2Enable  = 1;
	else
		table->SVI2Enable  = 0;

	table->ThermGpio = 17;
	table->SclkStepSize = 0x4000;

	table->SystemFlags = cpu_to_be32(table->SystemFlags);
	table->SmioMaskVddcVid = cpu_to_be32(table->SmioMaskVddcVid);
	table->SmioMaskVddcPhase = cpu_to_be32(table->SmioMaskVddcPhase);
	table->SmioMaskVddciVid = cpu_to_be32(table->SmioMaskVddciVid);
	table->SmioMaskMvddVid = cpu_to_be32(table->SmioMaskMvddVid);
	table->SclkStepSize = cpu_to_be32(table->SclkStepSize);
	table->TemperatureLimitHigh = cpu_to_be16(table->TemperatureLimitHigh);
	table->TemperatureLimitLow = cpu_to_be16(table->TemperatureLimitLow);
	table->VddcVddciDelta = cpu_to_be16(table->VddcVddciDelta);
	table->VoltageResponseTime = cpu_to_be16(table->VoltageResponseTime);
	table->PhaseResponseTime = cpu_to_be16(table->PhaseResponseTime);
	table->BootVddc = cpu_to_be16(table->BootVddc * VOLTAGE_SCALE);
	table->BootVddci = cpu_to_be16(table->BootVddci * VOLTAGE_SCALE);
	table->BootMVdd = cpu_to_be16(table->BootMVdd * VOLTAGE_SCALE);

	ret = ci_copy_bytes_to_smc(dpm,
				   pi->dpm_table_start +
				   offsetof(SMU7_Discrete_DpmTable, SystemFlags),
				   (uint8_t *)&table->SystemFlags,
				   sizeof(SMU7_Discrete_DpmTable) - 3 * sizeof(SMU7_PIDController),
				   pi->sram_end);
	if (ret)
		return ret;

	return 0;
}

/* ---- the memory controller's per-clock register table ---- */

static int ci_set_mc_special_registers(struct rdn_ci_dpm *dpm, struct ci_mc_reg_table *table)
{
	struct ci_power_info *pi = &dpm->pi;
	uint8_t i, j, k;
	uint32_t temp_reg;

	for (i = 0, j = table->last; i < table->last; i++) {
		if (j >= SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
			return -EINVAL;
		switch (table->mc_reg_address[i].s1 << 2) {
		case MC_SEQ_MISC1:
			temp_reg = RREG32(dpm->card, MC_PMG_CMD_EMRS);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_EMRS >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_EMRS_LP >> 2;
			for (k = 0; k < table->num_entries; k++) {
				table->mc_reg_table_entry[k].mc_data[j] =
					((temp_reg & 0xffff0000)) | ((table->mc_reg_table_entry[k].mc_data[i] & 0xffff0000) >> 16);
			}
			j++;
			if (j >= SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;

			temp_reg = RREG32(dpm->card, MC_PMG_CMD_MRS);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_MRS >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_MRS_LP >> 2;
			for (k = 0; k < table->num_entries; k++) {
				table->mc_reg_table_entry[k].mc_data[j] =
					(temp_reg & 0xffff0000) | (table->mc_reg_table_entry[k].mc_data[i] & 0x0000ffff);
				if (!pi->mem_gddr5)
					table->mc_reg_table_entry[k].mc_data[j] |= 0x100;
			}
			j++;
			if (j >= SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;

			if (!pi->mem_gddr5) {
				table->mc_reg_address[j].s1 = MC_PMG_AUTO_CMD >> 2;
				table->mc_reg_address[j].s0 = MC_PMG_AUTO_CMD >> 2;
				for (k = 0; k < table->num_entries; k++) {
					table->mc_reg_table_entry[k].mc_data[j] =
						(table->mc_reg_table_entry[k].mc_data[i] & 0xffff0000) >> 16;
				}
				j++;
				if (j > SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
					return -EINVAL;
			}
			break;
		case MC_SEQ_RESERVE_M:
			temp_reg = RREG32(dpm->card, MC_PMG_CMD_MRS1);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_MRS1 >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_MRS1_LP >> 2;
			for (k = 0; k < table->num_entries; k++) {
				table->mc_reg_table_entry[k].mc_data[j] =
					(temp_reg & 0xffff0000) | (table->mc_reg_table_entry[k].mc_data[i] & 0x0000ffff);
			}
			j++;
			if (j > SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;
			break;
		default:
			break;
		}

	}

	table->last = j;

	return 0;
}

static bool ci_check_s0_mc_reg_index(uint16_t in_reg, uint16_t *out_reg)
{
	bool result = true;

	switch (in_reg) {
	case MC_SEQ_RAS_TIMING >> 2:
		*out_reg = MC_SEQ_RAS_TIMING_LP >> 2;
		break;
	case MC_SEQ_DLL_STBY >> 2:
		*out_reg = MC_SEQ_DLL_STBY_LP >> 2;
		break;
	case MC_SEQ_G5PDX_CMD0 >> 2:
		*out_reg = MC_SEQ_G5PDX_CMD0_LP >> 2;
		break;
	case MC_SEQ_G5PDX_CMD1 >> 2:
		*out_reg = MC_SEQ_G5PDX_CMD1_LP >> 2;
		break;
	case MC_SEQ_G5PDX_CTRL >> 2:
		*out_reg = MC_SEQ_G5PDX_CTRL_LP >> 2;
		break;
	case MC_SEQ_CAS_TIMING >> 2:
		*out_reg = MC_SEQ_CAS_TIMING_LP >> 2;
		break;
	case MC_SEQ_MISC_TIMING >> 2:
		*out_reg = MC_SEQ_MISC_TIMING_LP >> 2;
		break;
	case MC_SEQ_MISC_TIMING2 >> 2:
		*out_reg = MC_SEQ_MISC_TIMING2_LP >> 2;
		break;
	case MC_SEQ_PMG_DVS_CMD >> 2:
		*out_reg = MC_SEQ_PMG_DVS_CMD_LP >> 2;
		break;
	case MC_SEQ_PMG_DVS_CTL >> 2:
		*out_reg = MC_SEQ_PMG_DVS_CTL_LP >> 2;
		break;
	case MC_SEQ_RD_CTL_D0 >> 2:
		*out_reg = MC_SEQ_RD_CTL_D0_LP >> 2;
		break;
	case MC_SEQ_RD_CTL_D1 >> 2:
		*out_reg = MC_SEQ_RD_CTL_D1_LP >> 2;
		break;
	case MC_SEQ_WR_CTL_D0 >> 2:
		*out_reg = MC_SEQ_WR_CTL_D0_LP >> 2;
		break;
	case MC_SEQ_WR_CTL_D1 >> 2:
		*out_reg = MC_SEQ_WR_CTL_D1_LP >> 2;
		break;
	case MC_PMG_CMD_EMRS >> 2:
		*out_reg = MC_SEQ_PMG_CMD_EMRS_LP >> 2;
		break;
	case MC_PMG_CMD_MRS >> 2:
		*out_reg = MC_SEQ_PMG_CMD_MRS_LP >> 2;
		break;
	case MC_PMG_CMD_MRS1 >> 2:
		*out_reg = MC_SEQ_PMG_CMD_MRS1_LP >> 2;
		break;
	case MC_SEQ_PMG_TIMING >> 2:
		*out_reg = MC_SEQ_PMG_TIMING_LP >> 2;
		break;
	case MC_PMG_CMD_MRS2 >> 2:
		*out_reg = MC_SEQ_PMG_CMD_MRS2_LP >> 2;
		break;
	case MC_SEQ_WR_CTL_2 >> 2:
		*out_reg = MC_SEQ_WR_CTL_2_LP >> 2;
		break;
	default:
		result = false;
		break;
	}

	return result;
}

static void ci_set_valid_flag(struct ci_mc_reg_table *table)
{
	uint8_t i, j;

	for (i = 0; i < table->last; i++) {
		for (j = 1; j < table->num_entries; j++) {
			if (table->mc_reg_table_entry[j-1].mc_data[i] !=
			    table->mc_reg_table_entry[j].mc_data[i]) {
				table->valid_flag |= 1 << i;
				break;
			}
		}
	}
}

static void ci_set_s0_mc_reg_index(struct ci_mc_reg_table *table)
{
	uint32_t i;
	uint16_t address;

	for (i = 0; i < table->last; i++) {
		table->mc_reg_address[i].s0 =
			ci_check_s0_mc_reg_index(table->mc_reg_address[i].s1, &address) ?
			address : table->mc_reg_address[i].s1;
	}
}

static int ci_copy_vbios_mc_reg_table(const struct atom_mc_reg_table *table,
				      struct ci_mc_reg_table *ci_table)
{
	uint8_t i, j;

	if (table->last > SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
		return -EINVAL;
	if (table->num_entries > MAX_AC_TIMING_ENTRIES)
		return -EINVAL;

	for (i = 0; i < table->last; i++)
		ci_table->mc_reg_address[i].s1 = table->mc_reg_address[i].s1;

	ci_table->last = table->last;

	for (i = 0; i < table->num_entries; i++) {
		ci_table->mc_reg_table_entry[i].mclk_max =
			table->mc_reg_table_entry[i].mclk_max;
		for (j = 0; j < table->last; j++)
			ci_table->mc_reg_table_entry[i].mc_data[j] =
				table->mc_reg_table_entry[i].mc_data[j];
	}
	ci_table->num_entries = table->num_entries;

	return 0;
}

static int ci_register_patching_mc_seq(struct rdn_ci_dpm *dpm, struct ci_mc_reg_table *table)
{
	uint8_t i, k;
	uint32_t tmp;
	bool patch;

	tmp = RREG32(dpm->card, MC_SEQ_MISC0);
	patch = (tmp & 0x0000f00) == 0x300;

	if (patch &&
	    ((dpm->card->device == 0x67B0) ||
	     (dpm->card->device == 0x67B1))) {
		for (i = 0; i < table->last; i++) {
			if (table->last >= SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;
			/* Linux compares s1 >> 2 against byte addresses here (so it never matches); kept as is */
			switch (table->mc_reg_address[i].s1 >> 2) {
			case MC_SEQ_MISC1:
				for (k = 0; k < table->num_entries; k++) {
					if ((table->mc_reg_table_entry[k].mclk_max == 125000) ||
					    (table->mc_reg_table_entry[k].mclk_max == 137500))
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFFFFFF8) |
							0x00000007;
				}
				break;
			case MC_SEQ_WR_CTL_D0:
				for (k = 0; k < table->num_entries; k++) {
					if ((table->mc_reg_table_entry[k].mclk_max == 125000) ||
					    (table->mc_reg_table_entry[k].mclk_max == 137500))
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFFF0F00) |
							0x0000D0DD;
				}
				break;
			case MC_SEQ_WR_CTL_D1:
				for (k = 0; k < table->num_entries; k++) {
					if ((table->mc_reg_table_entry[k].mclk_max == 125000) ||
					    (table->mc_reg_table_entry[k].mclk_max == 137500))
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFFF0F00) |
							0x0000D0DD;
				}
				break;
			case MC_SEQ_WR_CTL_2:
				for (k = 0; k < table->num_entries; k++) {
					if ((table->mc_reg_table_entry[k].mclk_max == 125000) ||
					    (table->mc_reg_table_entry[k].mclk_max == 137500))
						table->mc_reg_table_entry[k].mc_data[i] = 0;
				}
				break;
			case MC_SEQ_CAS_TIMING:
				for (k = 0; k < table->num_entries; k++) {
					if (table->mc_reg_table_entry[k].mclk_max == 125000)
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFE0FE0F) |
							0x000C0140;
					else if (table->mc_reg_table_entry[k].mclk_max == 137500)
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFE0FE0F) |
							0x000C0150;
				}
				break;
			case MC_SEQ_MISC_TIMING:
				for (k = 0; k < table->num_entries; k++) {
					if (table->mc_reg_table_entry[k].mclk_max == 125000)
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFFFFFE0) |
							0x00000030;
					else if (table->mc_reg_table_entry[k].mclk_max == 137500)
						table->mc_reg_table_entry[k].mc_data[i] =
							(table->mc_reg_table_entry[k].mc_data[i] & 0xFFFFFFE0) |
							0x00000035;
				}
				break;
			default:
				break;
			}
		}

		WREG32(dpm->card, MC_SEQ_IO_DEBUG_INDEX, 3);
		tmp = RREG32(dpm->card, MC_SEQ_IO_DEBUG_DATA);
		tmp = (tmp & 0xFFF8FFFF) | (1 << 16);
		WREG32(dpm->card, MC_SEQ_IO_DEBUG_INDEX, 3);
		WREG32(dpm->card, MC_SEQ_IO_DEBUG_DATA, tmp);
	}

	return 0;
}

int ci_initialize_mc_reg_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	struct rdn_card *card = dpm->card;
	struct atom_mc_reg_table *table;
	struct ci_mc_reg_table *ci_table = &pi->mc_reg_table;
	uint8_t module_index = rv770_get_memory_module_index(card);
	int ret;

	table = card->os->alloc(card->os->cookie, sizeof(struct atom_mc_reg_table));
	if (!table)
		return -ENOMEM;

	WREG32(card, MC_SEQ_RAS_TIMING_LP, RREG32(card, MC_SEQ_RAS_TIMING));
	WREG32(card, MC_SEQ_CAS_TIMING_LP, RREG32(card, MC_SEQ_CAS_TIMING));
	WREG32(card, MC_SEQ_DLL_STBY_LP, RREG32(card, MC_SEQ_DLL_STBY));
	WREG32(card, MC_SEQ_G5PDX_CMD0_LP, RREG32(card, MC_SEQ_G5PDX_CMD0));
	WREG32(card, MC_SEQ_G5PDX_CMD1_LP, RREG32(card, MC_SEQ_G5PDX_CMD1));
	WREG32(card, MC_SEQ_G5PDX_CTRL_LP, RREG32(card, MC_SEQ_G5PDX_CTRL));
	WREG32(card, MC_SEQ_PMG_DVS_CMD_LP, RREG32(card, MC_SEQ_PMG_DVS_CMD));
	WREG32(card, MC_SEQ_PMG_DVS_CTL_LP, RREG32(card, MC_SEQ_PMG_DVS_CTL));
	WREG32(card, MC_SEQ_MISC_TIMING_LP, RREG32(card, MC_SEQ_MISC_TIMING));
	WREG32(card, MC_SEQ_MISC_TIMING2_LP, RREG32(card, MC_SEQ_MISC_TIMING2));
	WREG32(card, MC_SEQ_PMG_CMD_EMRS_LP, RREG32(card, MC_PMG_CMD_EMRS));
	WREG32(card, MC_SEQ_PMG_CMD_MRS_LP, RREG32(card, MC_PMG_CMD_MRS));
	WREG32(card, MC_SEQ_PMG_CMD_MRS1_LP, RREG32(card, MC_PMG_CMD_MRS1));
	WREG32(card, MC_SEQ_WR_CTL_D0_LP, RREG32(card, MC_SEQ_WR_CTL_D0));
	WREG32(card, MC_SEQ_WR_CTL_D1_LP, RREG32(card, MC_SEQ_WR_CTL_D1));
	WREG32(card, MC_SEQ_RD_CTL_D0_LP, RREG32(card, MC_SEQ_RD_CTL_D0));
	WREG32(card, MC_SEQ_RD_CTL_D1_LP, RREG32(card, MC_SEQ_RD_CTL_D1));
	WREG32(card, MC_SEQ_PMG_TIMING_LP, RREG32(card, MC_SEQ_PMG_TIMING));
	WREG32(card, MC_SEQ_PMG_CMD_MRS2_LP, RREG32(card, MC_PMG_CMD_MRS2));
	WREG32(card, MC_SEQ_WR_CTL_2_LP, RREG32(card, MC_SEQ_WR_CTL_2));

	ret = radeon_atom_init_mc_reg_table(card, module_index, table);
	if (ret)
		goto init_mc_done;

	ret = ci_copy_vbios_mc_reg_table(table, ci_table);
	if (ret)
		goto init_mc_done;

	ci_set_s0_mc_reg_index(ci_table);

	ret = ci_register_patching_mc_seq(dpm, ci_table);
	if (ret)
		goto init_mc_done;

	ret = ci_set_mc_special_registers(dpm, ci_table);
	if (ret)
		goto init_mc_done;

	ci_set_valid_flag(ci_table);

	rdn_log(card->os, RDN_LOG_INFO, "ci_dpm: mc reg table: module %u, %u registers (%u after the special ones), %u clock ranges, valid 0x%04x",
		module_index, table->last, ci_table->last, ci_table->num_entries, ci_table->valid_flag);

init_mc_done:
	if (ret)
		rdn_log(card->os, RDN_LOG_ERROR, "ci_dpm: mc reg table from VRAM_Info (module %u) failed (%d); dynamic AC timing off", module_index, ret);
	card->os->free(card->os->cookie, table);

	return ret;
}

static int ci_populate_mc_reg_addresses(struct rdn_ci_dpm *dpm, SMU7_Discrete_MCRegisters *mc_reg_table)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t i, j;

	for (i = 0, j = 0; j < pi->mc_reg_table.last; j++) {
		if (pi->mc_reg_table.valid_flag & (1 << j)) {
			if (i >= SMU7_DISCRETE_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;
			mc_reg_table->address[i].s0 = cpu_to_be16(pi->mc_reg_table.mc_reg_address[j].s0);
			mc_reg_table->address[i].s1 = cpu_to_be16(pi->mc_reg_table.mc_reg_address[j].s1);
			i++;
		}
	}

	mc_reg_table->last = (uint8_t)i;

	return 0;
}

static void ci_convert_mc_registers(const struct ci_mc_reg_entry *entry,
				    SMU7_Discrete_MCRegisterSet *data,
				    uint32_t num_entries, uint32_t valid_flag)
{
	uint32_t i, j;

	for (i = 0, j = 0; j < num_entries; j++) {
		if (valid_flag & (1 << j)) {
			data->value[i] = cpu_to_be32(entry->mc_data[j]);
			i++;
		}
	}
}

static void ci_convert_mc_reg_table_entry_to_smc(struct rdn_ci_dpm *dpm,
						 const uint32_t memory_clock,
						 SMU7_Discrete_MCRegisterSet *mc_reg_table_data)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t i = 0;

	for (i = 0; i < pi->mc_reg_table.num_entries; i++) {
		if (memory_clock <= pi->mc_reg_table.mc_reg_table_entry[i].mclk_max)
			break;
	}

	if ((i == pi->mc_reg_table.num_entries) && (i > 0))
		--i;

	ci_convert_mc_registers(&pi->mc_reg_table.mc_reg_table_entry[i],
				mc_reg_table_data, pi->mc_reg_table.last,
				pi->mc_reg_table.valid_flag);
}

static void ci_convert_mc_reg_table_to_smc(struct rdn_ci_dpm *dpm, SMU7_Discrete_MCRegisters *mc_reg_table)
{
	struct ci_power_info *pi = &dpm->pi;
	uint32_t i;

	for (i = 0; i < pi->dpm_table.mclk_table.count; i++)
		ci_convert_mc_reg_table_entry_to_smc(dpm,
						     pi->dpm_table.mclk_table.dpm_levels[i].value,
						     &mc_reg_table->data[i]);
}

int ci_populate_initial_mc_reg_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	int ret;

	memset(&pi->smc_mc_reg_table, 0, sizeof(SMU7_Discrete_MCRegisters));

	ret = ci_populate_mc_reg_addresses(dpm, &pi->smc_mc_reg_table);
	if (ret)
		return ret;
	ci_convert_mc_reg_table_to_smc(dpm, &pi->smc_mc_reg_table);

	return ci_copy_bytes_to_smc(dpm,
				    pi->mc_reg_table_start,
				    (uint8_t *)&pi->smc_mc_reg_table,
				    sizeof(SMU7_Discrete_MCRegisters),
				    pi->sram_end);
}

int ci_update_and_upload_mc_reg_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;

	if (!(pi->need_update_smu7_dpm_table & DPMTABLE_OD_UPDATE_MCLK))
		return 0;

	memset(&pi->smc_mc_reg_table, 0, sizeof(SMU7_Discrete_MCRegisters));

	ci_convert_mc_reg_table_to_smc(dpm, &pi->smc_mc_reg_table);

	return ci_copy_bytes_to_smc(dpm,
				    pi->mc_reg_table_start +
				    offsetof(SMU7_Discrete_MCRegisters, data[0]),
				    (uint8_t *)&pi->smc_mc_reg_table.data[0],
				    sizeof(SMU7_Discrete_MCRegisterSet) *
				    pi->dpm_table.mclk_table.count,
				    pi->sram_end);
}

/* ---- the fan table ---- */

int ci_thermal_setup_fan_table(struct rdn_ci_dpm *dpm)
{
	struct ci_power_info *pi = &dpm->pi;
	SMU7_Discrete_FanTable fan_table;
	uint32_t duty100;
	uint32_t t_diff1, t_diff2, pwm_diff1, pwm_diff2;
	uint16_t fdo_min, slope1, slope2;
	uint32_t reference_clock, tmp;
	int ret;

	memset(&fan_table, 0, sizeof(fan_table));
	fan_table.FdoMode = FDO_MODE_HARDWARE;	/* { FDO_MODE_HARDWARE }, which is 0 */

	if (!pi->fan_table_start) {
		dpm->fan.ucode_fan_control = false;
		return 0;
	}

	duty100 = (RREG32_SMC(dpm->card, CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;

	if (duty100 == 0) {
		dpm->fan.ucode_fan_control = false;
		return 0;
	}

	/* Linux: do_div on a u64; pwm_min (0.01 %) * duty100 (8 bits) fits 32 bits */
	fdo_min = (uint16_t)(((uint32_t)dpm->fan.pwm_min * duty100) / 10000);

	t_diff1 = dpm->fan.t_med - dpm->fan.t_min;
	t_diff2 = dpm->fan.t_high - dpm->fan.t_med;

	pwm_diff1 = dpm->fan.pwm_med - dpm->fan.pwm_min;
	pwm_diff2 = dpm->fan.pwm_high - dpm->fan.pwm_med;

	if (t_diff1 == 0 || t_diff2 == 0) {
		/* Linux divides by zero here on a degenerate table; refuse instead */
		rdn_log(dpm->card->os, RDN_LOG_ERROR, "ci_dpm: fan table with equal temperature points; firmware fan control off");
		dpm->fan.ucode_fan_control = false;
		return 0;
	}

	slope1 = (uint16_t)((50 + ((16 * duty100 * pwm_diff1) / t_diff1)) / 100);
	slope2 = (uint16_t)((50 + ((16 * duty100 * pwm_diff2) / t_diff2)) / 100);

	fan_table.TempMin = cpu_to_be16((50 + dpm->fan.t_min) / 100);
	fan_table.TempMed = cpu_to_be16((50 + dpm->fan.t_med) / 100);
	fan_table.TempMax = cpu_to_be16((50 + dpm->fan.t_max) / 100);

	fan_table.Slope1 = cpu_to_be16(slope1);
	fan_table.Slope2 = cpu_to_be16(slope2);

	fan_table.FdoMin = cpu_to_be16(fdo_min);

	fan_table.HystDown = cpu_to_be16(dpm->fan.t_hyst);

	fan_table.HystUp = cpu_to_be16(1);

	fan_table.HystSlope = cpu_to_be16(1);

	fan_table.TempRespLim = cpu_to_be16(5);

	reference_clock = cik_get_xclk(dpm);

	fan_table.RefreshPeriod = cpu_to_be32((dpm->fan.cycle_delay *
					       reference_clock) / 1600);

	fan_table.FdoMax = cpu_to_be16((uint16_t)duty100);

	tmp = (RREG32_SMC(dpm->card, CG_MULT_THERMAL_CTRL) & TEMP_SEL_MASK) >> TEMP_SEL_SHIFT;
	fan_table.TempSrc = (uint8_t)tmp;

	ret = ci_copy_bytes_to_smc(dpm,
				   pi->fan_table_start,
				   (uint8_t *)(&fan_table),
				   sizeof(fan_table),
				   pi->sram_end);

	if (ret) {
		rdn_log(dpm->card->os, RDN_LOG_ERROR, "Failed to load fan table to the SMC.");
		dpm->fan.ucode_fan_control = false;
	}

	return 0;
}
