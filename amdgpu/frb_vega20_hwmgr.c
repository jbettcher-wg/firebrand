/*
 * Powerplay's hw_init on Vega 20, ported from Linux 7.1.1
 * (pm/powerplay/hwmgr/hwmgr.c hwmgr_hw_init, vega20_processpptables.c,
 * vega20_hwmgr.c, vega20_thermal.c, ppatomfwctrl.c): the VBIOS power-play
 * table and its smc_dpm_info overlay, the backend's registry defaults and
 * feature map, then vega20_enable_dpm_tasks (allowed features, the PPTable
 * to the SMU, BTC, EnableAllSmuFeatures, PCIe limits, the DPM ladders, the
 * DC-mode maxima, OverDrive8's defaults, the power limit) and the thermal
 * controller (alert range, the fan target).
 *
 * The capture's 98 messages from GetSmuVersion to SetFanTemperatureTarget
 * are what tests/frb_replay.c checks this file against, with the PPTable
 * the SMU gets compared byte for byte with the capture's PPTABLE bo.
 *
 * BYTE ORDER. The driver's PPTable_t and OverDriveTable_t are kept as the
 * SMU's little-endian byte images (pptable[], od_table[]); every field is
 * read and written at offsetof() with its width, little-endian. The VBIOS
 * tables are read the same way. No smu11_driver_if.h structure is laid over
 * memory, so the host's byte order never enters (docs/byte-order.md).
 *
 * Not carried (not on this path, or no consumer yet): SR-IOV/one-VF arms,
 * BACO, the power profiles, OD8 editing, the sensors through SMU_METRICS
 * (the THM temperature is read directly), fan control (the MI50 has none).
 *
 * Copyright 2018 Advanced Micro Devices, Inc.
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
#include "../shared/atom/atomfirmware-frb.h"
#include "frb_soc15.h"
#include "frb_smu11.h"
#include "frb_pm.h"
#include "include/pp/smu11_driver_if.h"
#include "include/pp/vega20_pptable.h"
#include "include/pp/vega20_ppsmc.h"
#include "include/asic_reg/thm/thm_11_0_2_offset.h"
#include "include/asic_reg/thm/thm_11_0_2_sh_mask.h"
#include "include/asic_reg/smuio/smuio_9_0_offset.h"	/* vega20_hwmgr.c's SMUSVI0_TEL_PLANE0 */
#include "include/asic_reg/smuio/smuio_9_0_sh_mask.h"

/* vega20_hwmgr.h enum: GNLD_* (equal to the FEATURE_*_BIT numbers on Vega 20) */
enum {
	GNLD_DPM_PREFETCHER = 0, GNLD_DPM_GFXCLK, GNLD_DPM_UCLK, GNLD_DPM_SOCCLK, GNLD_DPM_UVD, GNLD_DPM_VCE,
	GNLD_ULV, GNLD_DPM_MP0CLK, GNLD_DPM_LINK, GNLD_DPM_DCEFCLK, GNLD_DS_GFXCLK, GNLD_DS_SOCCLK,
	GNLD_DS_LCLK, GNLD_PPT, GNLD_TDC, GNLD_THERMAL, GNLD_GFX_PER_CU_CG, GNLD_RM, GNLD_DS_DCEFCLK,
	GNLD_ACDC, GNLD_VR0HOT, GNLD_VR1HOT, GNLD_FW_CTF, GNLD_LED_DISPLAY, GNLD_FAN_CONTROL, GNLD_DIDT,
	GNLD_GFXOFF, GNLD_CG, GNLD_DPM_FCLK, GNLD_DS_FCLK, GNLD_DS_MP1CLK, GNLD_DS_MP0CLK, GNLD_XGMI,
	GNLD_ECC, GNLD_FEATURES_MAX
};

/* vega20_hwmgr.h enum OD8_SETTING_ID, and the OD8 capability bits */
enum {
	OD8_SETTING_GFXCLK_FMIN = 0, OD8_SETTING_GFXCLK_FMAX, OD8_SETTING_GFXCLK_FREQ1,
	OD8_SETTING_GFXCLK_VOLTAGE1, OD8_SETTING_GFXCLK_FREQ2, OD8_SETTING_GFXCLK_VOLTAGE2,
	OD8_SETTING_GFXCLK_FREQ3, OD8_SETTING_GFXCLK_VOLTAGE3, OD8_SETTING_UCLK_FMAX,
	OD8_SETTING_POWER_PERCENTAGE, OD8_SETTING_FAN_ACOUSTIC_LIMIT, OD8_SETTING_FAN_MIN_SPEED,
	OD8_SETTING_FAN_TARGET_TEMP, OD8_SETTING_OPERATING_TEMP_MAX, OD8_SETTING_AC_TIMING,
	OD8_SETTING_FAN_ZERO_RPM_CONTROL, OD8_SETTING_COUNT
};
#define OD8_GFXCLK_LIMITS		(1 << 0)
#define OD8_GFXCLK_CURVE		(1 << 1)
#define OD8_UCLK_MAX			(1 << 2)
#define OD8_POWER_LIMIT			(1 << 3)
#define OD8_ACOUSTIC_LIMIT_SCLK		(1 << 4)
#define OD8_FAN_SPEED_MIN		(1 << 5)
#define OD8_TEMPERATURE_FAN		(1 << 6)
#define OD8_TEMPERATURE_SYSTEM		(1 << 7)
#define OD8_MEMORY_TIMING_TUNE		(1 << 8)
#define OD8_FAN_ZERO_RPM_CONTROL	(1 << 9)
#define AVFS_CURVE			0
#define OD8_HOTCURVE_TEMPERATURE	85
#define VOLTAGE_SCALE			4
#define VEGA20_FAN_TARGET_TEMPERATURE_OVERRIDE	105
#define VEGA20_THERMAL_MINIMUM_ALERT_TEMP	0
#define VEGA20_THERMAL_MAXIMUM_ALERT_TEMP	255
#define POWER_SOURCE_AC			0

/* amd_shared.h PP_FEATURE_MASK bits */
#define PP_SCLK_DPM_MASK		0x1
#define PP_MCLK_DPM_MASK		0x2
#define PP_PCIE_DPM_MASK		0x4
#define PP_SCLK_DEEP_SLEEP_MASK		0x8
#define PP_ULV_MASK			0x100
#define PP_SOCCLK_DPM_MASK		0x1000
#define PP_DCEFCLK_DPM_MASK		0x2000
#define FRB_VEGA20_PP_FEATURE_MASK_DEFAULT 0xfff7bfffu	/* amdgpu_pp_feature_mask */
/* amd_pcie.h */
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1	0x00010000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2	0x00020000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3	0x00040000
#define CAIL_PCIE_LINK_SPEED_SUPPORT_GEN4	0x00080000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X1		0x00010000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X2		0x00020000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X4		0x00040000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X8		0x00080000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X12	0x00100000
#define CAIL_PCIE_LINK_WIDTH_SUPPORT_X16	0x00200000

/* a DPM ladder (vega20_single_dpm_table, the values only) */
struct dpm_ladder {
	uint32_t count;
	uint32_t value[16];
};

struct frb_vega20_hwmgr {
	/* the driver's PPTable_t and OverDriveTable_t, as the SMU reads them (LE) */
	uint8_t pptable[sizeof(PPTable_t)];
	uint8_t od_table[sizeof(OverDriveTable_t)];
	/* vega20_processpptables.c's phm_ppt_v3_information, the fields used */
	uint8_t od_feature_capabilities[ATOM_VEGA20_ODFEATURE_COUNT];
	uint32_t od_settings_max[ATOM_VEGA20_ODSETTING_COUNT], od_settings_min[ATOM_VEGA20_ODSETTING_COUNT];
	uint32_t od_feature_count, od_setting_count;
	uint16_t us_software_shutdown_temp;
	uint8_t thermal_controller_type;
	uint32_t platform_caps;
	/* vega20_hwmgr */
	struct {
		uint64_t bitmap;
		int allowed, enabled, supported;
	} f[GNLD_FEATURES_MAX];
	uint64_t disallowed_features;
	uint32_t feature_mask;
	uint32_t smu_version, serial_top, serial_bottom;
	uint64_t features_enabled;
	struct {	/* vbios_boot_state, 10 kHz */
		uint32_t gfx_clock, mem_clock, soc_clock, dcef_clock, eclock, vclock, dclock, fclock;
		uint16_t vddc, vddci, mvddc;
		uint8_t cooling_id;
	} boot;
	struct dpm_ladder soc, gfx, mem, eclk, vclk, dclk, dcef, pixel, display, phy, fclk;
	struct {
		uint32_t uclock, soc_clock, dcef_clock, display_clock, phy_clock, pixel_clock;
	} max_sustainable;
	uint32_t od8_capabilities;
	uint32_t default_power_limit;
	int uvd_power_gated, vce_power_gated;	/* vega20_init_powergate_state */
	int thermal_low, thermal_high;
};

/* the PPTable_t / OverDriveTable_t / VBIOS fields, by offset and width, little-endian */
#define PPT(f)		offsetof(PPTable_t, f)
#define PPTW(f)		sizeof(((PPTable_t *)0)->f)
#define ODT(f)		offsetof(OverDriveTable_t, f)
#define ODTW(f)		sizeof(((OverDriveTable_t *)0)->f)

static uint32_t get_le(const uint8_t *b, uint32_t off, uint32_t width)
{
	switch (width) {
	case 1:	return b[off];
	case 2:	return frb_get_le16(b, off);
	default: return frb_get_le32(b, off);
	}
}

static void put_le(uint8_t *b, uint32_t off, uint32_t width, uint32_t v)
{
	switch (width) {
	case 1:	b[off] = (uint8_t)v; break;
	case 2:	frb_put_le16(b, off, (uint16_t)v); break;
	default: frb_put_le32(b, off, v); break;
	}
}

#define PP_GET(h, f)		get_le((h)->pptable, PPT(f), PPTW(f))
#define PP_SET(h, f, v)		put_le((h)->pptable, PPT(f), PPTW(f), (v))
#define OD_GET(h, f)		get_le((h)->od_table, ODT(f), ODTW(f))
#define OD_SET(h, f, v)		put_le((h)->od_table, ODT(f), ODTW(f), (v))

static int send(struct frb_device *adev, uint16_t msg, uint32_t param, uint32_t *resp)
{
	return frb_smu11_send_msg(adev, msg, param, resp);
}

/* smum_send_msg_to_smc: no argument written */
static int send0(struct frb_device *adev, uint16_t msg, uint32_t *resp)
{
	return frb_smu11_send_msg_noarg(adev, msg, resp);
}

/* the atomfirmware master-list indices */
#define FW_DATA_INDEX(m)	((int)(offsetof(struct atom_master_list_of_data_tables_v2_1, m) / sizeof(uint16_t)))
#define FW_CMD_INDEX(m)		((int)(offsetof(struct atom_master_list_of_command_functions_v2_1, m) / sizeof(uint16_t)))

/* --- vega20_processpptables.c ---------------------------------------------- */

/*
 * append_vbios_pptable: smc_dpm_info v4.4's board fields over the PPTable's,
 * each read at its width and stored at the PPTable field's (a C assignment
 * between the two structures on Linux).
 */
#define DPM(f)		offsetof(struct atom_smc_dpm_info_v4_4, f)
#define DPMW(f)		sizeof(((struct atom_smc_dpm_info_v4_4 *)0)->f)
#define OVERLAY(dst, src) \
	put_le(h->pptable, PPT(dst), PPTW(dst), get_le(vb, off + DPM(src), DPMW(src)))

static int append_vbios_pptable(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	const uint8_t *vb = adev->bios;
	uint16_t off;
	int i;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(smc_dpm_info), NULL, NULL, &off)) {
		frb_err(adev, "[appendVbiosPPTable] Failed to retrieve Smc Dpm Table from VBIOS!");
		return -EINVAL;
	}
	OVERLAY(MaxVoltageStepGfx, maxvoltagestepgfx);
	OVERLAY(MaxVoltageStepSoc, maxvoltagestepsoc);
	OVERLAY(VddGfxVrMapping, vddgfxvrmapping);
	OVERLAY(VddSocVrMapping, vddsocvrmapping);
	OVERLAY(VddMem0VrMapping, vddmem0vrmapping);
	OVERLAY(VddMem1VrMapping, vddmem1vrmapping);
	OVERLAY(GfxUlvPhaseSheddingMask, gfxulvphasesheddingmask);
	OVERLAY(SocUlvPhaseSheddingMask, soculvphasesheddingmask);
	OVERLAY(ExternalSensorPresent, externalsensorpresent);
	OVERLAY(GfxMaxCurrent, gfxmaxcurrent);
	OVERLAY(GfxOffset, gfxoffset);
	OVERLAY(Padding_TelemetryGfx, padding_telemetrygfx);
	OVERLAY(SocMaxCurrent, socmaxcurrent);
	OVERLAY(SocOffset, socoffset);
	OVERLAY(Padding_TelemetrySoc, padding_telemetrysoc);
	OVERLAY(Mem0MaxCurrent, mem0maxcurrent);
	OVERLAY(Mem0Offset, mem0offset);
	OVERLAY(Padding_TelemetryMem0, padding_telemetrymem0);
	OVERLAY(Mem1MaxCurrent, mem1maxcurrent);
	OVERLAY(Mem1Offset, mem1offset);
	OVERLAY(Padding_TelemetryMem1, padding_telemetrymem1);
	OVERLAY(AcDcGpio, acdcgpio);
	OVERLAY(AcDcPolarity, acdcpolarity);
	OVERLAY(VR0HotGpio, vr0hotgpio);
	OVERLAY(VR0HotPolarity, vr0hotpolarity);
	OVERLAY(VR1HotGpio, vr1hotgpio);
	OVERLAY(VR1HotPolarity, vr1hotpolarity);
	OVERLAY(Padding1, padding1);
	OVERLAY(Padding2, padding2);
	OVERLAY(LedPin0, ledpin0);
	OVERLAY(LedPin1, ledpin1);
	OVERLAY(LedPin2, ledpin2);
	OVERLAY(PllGfxclkSpreadEnabled, pllgfxclkspreadenabled);
	OVERLAY(PllGfxclkSpreadPercent, pllgfxclkspreadpercent);
	OVERLAY(PllGfxclkSpreadFreq, pllgfxclkspreadfreq);
	PP_SET(h, UclkSpreadEnabled, 0);
	OVERLAY(UclkSpreadPercent, uclkspreadpercent);
	OVERLAY(UclkSpreadFreq, uclkspreadfreq);
	OVERLAY(FclkSpreadEnabled, fclkspreadenabled);
	OVERLAY(FclkSpreadPercent, fclkspreadpercent);
	OVERLAY(FclkSpreadFreq, fclkspreadfreq);
	OVERLAY(FllGfxclkSpreadEnabled, fllgfxclkspreadenabled);
	OVERLAY(FllGfxclkSpreadPercent, fllgfxclkspreadpercent);
	OVERLAY(FllGfxclkSpreadFreq, fllgfxclkspreadfreq);
	for (i = 0; i < I2C_CONTROLLER_NAME_COUNT; i++) {
		uint32_t d = PPT(I2cControllers) + i * sizeof(I2cControllerConfig_t);
		uint32_t s = off + DPM(i2ccontrollers) + i * sizeof(struct smudpm_i2ccontrollerconfig_t);

#define I2C(dstf, srcf) \
		put_le(h->pptable, d + offsetof(I2cControllerConfig_t, dstf), \
		       sizeof(((I2cControllerConfig_t *)0)->dstf), \
		       get_le(vb, s + offsetof(struct smudpm_i2ccontrollerconfig_t, srcf), \
			      sizeof(((struct smudpm_i2ccontrollerconfig_t *)0)->srcf)))
		I2C(Enabled, enabled);
		I2C(SlaveAddress, slaveaddress);
		I2C(ControllerPort, controllerport);
		I2C(ThermalThrottler, thermalthrottler);
		I2C(I2cProtocol, i2cprotocol);
		I2C(I2cSpeed, i2cspeed);
#undef I2C
	}
	return 0;
}

/* vega20_pp_tables_initialize */
static int vega20_pp_tables_initialize(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	const uint8_t *vb = adev->bios;
	uint16_t off;
	uint8_t frev, crev;
	uint32_t i, n;
#define VB(f)	offsetof(ATOM_Vega20_POWERPLAYTABLE, f)

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(powerplayinfo), &frev, &crev, &off)) {
		frb_err(adev, "Missing PowerPlay Table!");
		return -EINVAL;
	}
	/* check_powerplay_tables */
	if (frev < ATOM_VEGA20_TABLE_REVISION_VEGA20 || !frb_get_le16(vb, off + VB(sHeader))) {
		frb_err(adev, "Unsupported PPTable format!");
		return -EINVAL;
	}
	if (frb_get_le32(vb, off + VB(smcPPTable) + PPT(Version)) != PPTABLE_V20_SMU_VERSION) {
		frb_err(adev, "Unmatch PPTable version: pptable from VBIOS is V%u while driver supported is V%u!",
			frb_get_le32(vb, off + VB(smcPPTable) + PPT(Version)), PPTABLE_V20_SMU_VERSION);
		return -EINVAL;
	}
	h->platform_caps = frb_get_le32(vb, off + VB(ulPlatformCaps));
	/* init_powerplay_table_information */
	h->thermal_controller_type = vb[off + VB(ucThermalControllerType)];
	if (vb[off + VB(OverDrive8Table) + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ucODTableRevision)] == 1) {
		uint32_t od = off + VB(OverDrive8Table);

		n = frb_get_le32(vb, od + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ODFeatureCount));
		h->od_feature_count = n > ATOM_VEGA20_ODFEATURE_COUNT ? ATOM_VEGA20_ODFEATURE_COUNT : n;
		n = frb_get_le32(vb, od + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ODSettingCount));
		h->od_setting_count = n > ATOM_VEGA20_ODSETTING_COUNT ? ATOM_VEGA20_ODSETTING_COUNT : n;
		for (i = 0; i < h->od_feature_count; i++)
			h->od_feature_capabilities[i] =
				vb[od + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ODFeatureCapabilities) + i];
		for (i = 0; i < h->od_setting_count; i++) {
			h->od_settings_max[i] =
				frb_get_le32(vb, od + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ODSettingsMax) + 4 * i);
			h->od_settings_min[i] =
				frb_get_le32(vb, od + offsetof(ATOM_VEGA20_OVERDRIVE8_RECORD, ODSettingsMin) + 4 * i);
		}
	}
	h->us_software_shutdown_temp = frb_get_le16(vb, off + VB(usSoftwareShutdownTemp));
	/* the smcPPTable, the VBIOS's bytes (LE already), then the board overlay and the fan target */
	memcpy(h->pptable, vb + off + VB(smcPPTable), sizeof(h->pptable));
	if (append_vbios_pptable(adev, h))
		return -EINVAL;
	PP_SET(h, FanTargetTemperature, VEGA20_FAN_TARGET_TEMPERATURE_OVERRIDE);
#undef VB
	return 0;
}

/* --- vega20_hwmgr.c -------------------------------------------------------- */

/* vega20_set_default_registry_data: the feature defaults (GetSmuVersion for the ECC check) */
static void vega20_set_default_registry_data(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	h->disallowed_features = 0xE0041C00;
	send0(adev, PPSMC_MSG_GetSmuVersion, &h->smu_version);
	if (h->smu_version < 0x282100)
		h->disallowed_features |= 1ULL << FEATURE_ECC_BIT;
	if (!(h->feature_mask & PP_PCIE_DPM_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DPM_LINK_BIT;
	if (!(h->feature_mask & PP_SCLK_DPM_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DPM_GFXCLK_BIT;
	if (!(h->feature_mask & PP_SOCCLK_DPM_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DPM_SOCCLK_BIT;
	if (!(h->feature_mask & PP_MCLK_DPM_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DPM_UCLK_BIT;
	if (!(h->feature_mask & PP_DCEFCLK_DPM_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DPM_DCEFCLK_BIT;
	if (!(h->feature_mask & PP_ULV_MASK))
		h->disallowed_features |= 1ULL << FEATURE_ULV_BIT;
	if (!(h->feature_mask & PP_SCLK_DEEP_SLEEP_MASK))
		h->disallowed_features |= 1ULL << FEATURE_DS_GFXCLK_BIT;
}

/* vega20_init_dpm_defaults: the feature map, the serial number */
static int vega20_init_dpm_defaults(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int i;

	for (i = 0; i < GNLD_FEATURES_MAX; i++) {
		h->f[i].bitmap = 1ULL << i;	/* smu_feature_id == the GNLD index on Vega 20 */
		h->f[i].allowed = ((h->disallowed_features >> i) & 1) ? 0 : 1;
	}
	send0(adev, PPSMC_MSG_ReadSerialNumTop32, &h->serial_top);
	send0(adev, PPSMC_MSG_ReadSerialNumBottom32, &h->serial_bottom);
	return 0;
}

/* vega20_get_enabled_smc_features (vega20_smumgr.c) */
static int vega20_get_enabled_smc_features(struct frb_device *adev, uint64_t *features_enabled)
{
	uint32_t lo = 0, hi = 0;

	send0(adev, PPSMC_MSG_GetEnabledSmuFeaturesLow, &lo);
	send0(adev, PPSMC_MSG_GetEnabledSmuFeaturesHigh, &hi);
	*features_enabled = (uint64_t)hi << 32 | lo;
	return 0;
}

static int vega20_set_allowed_featuresmask(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	uint32_t low = 0, high = 0;
	int i;

	for (i = 0; i < GNLD_FEATURES_MAX; i++)
		if (h->f[i].allowed) {
			if (i > 31)
				high |= (uint32_t)(h->f[i].bitmap >> 32);
			else
				low |= (uint32_t)h->f[i].bitmap;
		}
	send(adev, PPSMC_MSG_SetAllowedFeaturesMaskHigh, high, NULL);
	send(adev, PPSMC_MSG_SetAllowedFeaturesMaskLow, low, NULL);
	return 0;
}

/*
 * pp_atomfwctrl_get_clk_information_by_clkid: the getsmuclockinfo command
 * table (v3.1), its 4-byte parameter block built LE (syspll_id, clk_id,
 * command, dfsdid), the frequency read back from the same dword (Hz).
 */
static int get_clk_information_by_clkid(struct frb_device *adev, uint8_t clk_id, uint8_t syspll_id, uint32_t *freq)
{
	uint32_t ps[4];

	memset(ps, 0, sizeof(ps));
	frb_put_le32(ps, 0, syspll_id | (uint32_t)clk_id << 8 | (uint32_t)GET_SMU_CLOCK_INFO_V3_1_GET_CLOCK_FREQ << 16);
	if (frb_atom_execute_table(adev->atom, FW_CMD_INDEX(getsmuclockinfo), ps, 4))
		return -EINVAL;
	*freq = frb_get_le32(ps, 0) / 10000;
	return 0;
}

/* pp_atomfwctrl_get_vbios_bootup_values, firmwareinfo v3.2 */
static int get_vbios_bootup_values(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	const uint8_t *vb = adev->bios;
	uint16_t off;
	uint8_t frev, crev;
	uint32_t f = 0;
#define FW(m)	offsetof(struct atom_firmware_info_v3_2, m)

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(firmwareinfo), &frev, &crev, &off) ||
	    frev != 3 || crev != 2) {
		frb_err(adev, "Fw info table revision does not match!");
		return -EINVAL;
	}
	h->boot.gfx_clock = frb_get_le32(vb, off + FW(bootup_sclk_in10khz));
	h->boot.mem_clock = frb_get_le32(vb, off + FW(bootup_mclk_in10khz));
	h->boot.vddc = frb_get_le16(vb, off + FW(bootup_vddc_mv));
	h->boot.vddci = frb_get_le16(vb, off + FW(bootup_vddci_mv));
	h->boot.mvddc = frb_get_le16(vb, off + FW(bootup_mvddc_mv));
	h->boot.cooling_id = vb[off + FW(coolingsolution_id)];
#undef FW
	h->boot.soc_clock = h->boot.dcef_clock = 0;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL0_SOCCLK_ID, SMU11_SYSPLL0_ID, &f))
		h->boot.soc_clock = f;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL0_DCEFCLK_ID, SMU11_SYSPLL0_ID, &f))
		h->boot.dcef_clock = f;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL0_ECLK_ID, SMU11_SYSPLL0_ID, &f))
		h->boot.eclock = f;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL0_VCLK_ID, SMU11_SYSPLL0_ID, &f))
		h->boot.vclock = f;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL0_DCLK_ID, SMU11_SYSPLL0_ID, &f))
		h->boot.dclock = f;
	if (!get_clk_information_by_clkid(adev, SMU11_SYSPLL1_0_FCLK_ID, SMU11_SYSPLL1_2_ID, &f))
		h->boot.fclock = f;
	return 0;
}

/* vega20_init_smc_table: boot values, the deep-sleep DCEFCLK floor, the PPTable to the SMU */
static int vega20_init_smc_table(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int r = get_vbios_bootup_values(adev, h);

	if (r)
		return r;
	send(adev, PPSMC_MSG_SetMinDeepSleepDcefclk, h->boot.dcef_clock / 100, NULL);
	r = frb_smu11_copy_table_to_smc(adev, h->pptable, FRB_SMU11_TABLE_PPTABLE);
	if (r)
		frb_err(adev, "[InitSMCTable] Failed to upload PPtable!");
	return r;
}

static int vega20_enable_all_smu_features(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int i;

	send0(adev, PPSMC_MSG_EnableAllSmuFeatures, NULL);
	vega20_get_enabled_smc_features(adev, &h->features_enabled);
	for (i = 0; i < GNLD_FEATURES_MAX; i++) {
		int en = (h->features_enabled & h->f[i].bitmap) ? 1 : 0;

		h->f[i].enabled = en;
		h->f[i].supported = en;
	}
	return 0;
}

/* vega20_override_pcie_parameters: the link levels capped at what this platform carries */
static int vega20_override_pcie_parameters(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	uint32_t pcie_gen = 0, pcie_width = 0, arg, gen_arg, width_arg, g, w;
	int i;

	if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN4)
		pcie_gen = 3;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN3)
		pcie_gen = 2;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN2)
		pcie_gen = 1;
	else if (adev->pcie_gen_mask & CAIL_PCIE_LINK_SPEED_SUPPORT_GEN1)
		pcie_gen = 0;

	if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X16)
		pcie_width = 6;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X12)
		pcie_width = 5;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X8)
		pcie_width = 4;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X4)
		pcie_width = 3;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X2)
		pcie_width = 2;
	else if (adev->pcie_mlw_mask & CAIL_PCIE_LINK_WIDTH_SUPPORT_X1)
		pcie_width = 1;

	/* Bit 31:16 LCLK DPM level, 15:8 PCIE GEN (0..3 = GEN1..4), 7:0 lane width (1..7 = x1..x32) */
	for (i = 0; i < NUM_LINK_LEVELS; i++) {
		g = h->pptable[PPT(PcieGenSpeed) + i];
		w = h->pptable[PPT(PcieLaneCount) + i];
		gen_arg = g > pcie_gen ? pcie_gen : g;
		width_arg = w > pcie_width ? pcie_width : w;
		if (gen_arg != g || width_arg != w) {
			arg = ((uint32_t)i << 16) | (gen_arg << 8) | width_arg;
			send(adev, PPSMC_MSG_OverridePcieParameters, arg, NULL);
		}
		h->pptable[PPT(PcieGenSpeed) + i] = (uint8_t)gen_arg;
		h->pptable[PPT(PcieLaneCount) + i] = (uint8_t)width_arg;
	}
	/* registry pcie_dpm_key_disabled: PP_PCIE_DPM_MASK is in the default feature mask */
	return 0;
}

/* vega20_setup_single_dpm_table */
static int setup_single(struct frb_device *adev, struct dpm_ladder *t, int clk_id)
{
	uint32_t i, n = 0, clk = 0;

	send(adev, PPSMC_MSG_GetDpmFreqByIndex, (uint32_t)clk_id << 16 | 0xFF, &n);	/* number of levels */
	if (n > 16) {
		frb_err(adev, "smu: clock %d reports %u DPM levels", clk_id, n);
		n = 16;
	}
	t->count = n;
	for (i = 0; i < n; i++) {
		send(adev, PPSMC_MSG_GetDpmFreqByIndex, (uint32_t)clk_id << 16 | i, &clk);
		t->value[i] = clk;
	}
	return 0;
}

/* vega20_setup_default_dpm_tables, in its order */
static int vega20_setup_default_dpm_tables(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
#define LADDER(tbl, gnld, clk, boot)					\
	do {								\
		if (h->f[gnld].enabled)					\
			setup_single(adev, &h->tbl, clk);		\
		else {							\
			h->tbl.count = 1;				\
			h->tbl.value[0] = (boot) / 100;			\
		}							\
	} while (0)
#define LADDER0(tbl, gnld, clk)						\
	do {								\
		if (h->f[gnld].enabled)					\
			setup_single(adev, &h->tbl, clk);		\
		else							\
			h->tbl.count = 0;				\
	} while (0)
	LADDER(soc, GNLD_DPM_SOCCLK, PPCLK_SOCCLK, h->boot.soc_clock);
	LADDER(gfx, GNLD_DPM_GFXCLK, PPCLK_GFXCLK, h->boot.gfx_clock);
	LADDER(mem, GNLD_DPM_UCLK, PPCLK_UCLK, h->boot.mem_clock);
	LADDER(eclk, GNLD_DPM_VCE, PPCLK_ECLK, h->boot.eclock);
	LADDER(vclk, GNLD_DPM_UVD, PPCLK_VCLK, h->boot.vclock);
	LADDER(dclk, GNLD_DPM_UVD, PPCLK_DCLK, h->boot.dclock);
	LADDER(dcef, GNLD_DPM_DCEFCLK, PPCLK_DCEFCLK, h->boot.dcef_clock);
	LADDER0(pixel, GNLD_DPM_DCEFCLK, PPCLK_PIXCLK);
	LADDER0(display, GNLD_DPM_DCEFCLK, PPCLK_DISPCLK);
	LADDER0(phy, GNLD_DPM_DCEFCLK, PPCLK_PHYCLK);
	LADDER(fclk, GNLD_DPM_FCLK, PPCLK_FCLK, h->boot.fclock);
#undef LADDER
#undef LADDER0
	return 0;
}

/* vega20_get_max_sustainable_clock: the DC-mode maximum, else the AC one */
static void get_max_sustainable(struct frb_device *adev, uint32_t *clock, int clk)
{
	send(adev, PPSMC_MSG_GetDcModeMaxDpmFreq, (uint32_t)clk << 16, clock);
	if (*clock == 0)
		send(adev, PPSMC_MSG_GetMaxDpmFreq, (uint32_t)clk << 16, clock);
}

static int vega20_init_max_sustainable_clocks(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	h->max_sustainable.uclock = h->boot.mem_clock / 100;
	h->max_sustainable.soc_clock = h->boot.soc_clock / 100;
	h->max_sustainable.dcef_clock = h->boot.dcef_clock / 100;
	h->max_sustainable.display_clock = 0xFFFFFFFF;
	h->max_sustainable.phy_clock = 0xFFFFFFFF;
	h->max_sustainable.pixel_clock = 0xFFFFFFFF;
	if (h->f[GNLD_DPM_UCLK].enabled)
		get_max_sustainable(adev, &h->max_sustainable.uclock, PPCLK_UCLK);
	if (h->f[GNLD_DPM_SOCCLK].enabled)
		get_max_sustainable(adev, &h->max_sustainable.soc_clock, PPCLK_SOCCLK);
	if (h->f[GNLD_DPM_DCEFCLK].enabled) {
		get_max_sustainable(adev, &h->max_sustainable.dcef_clock, PPCLK_DCEFCLK);
		get_max_sustainable(adev, &h->max_sustainable.display_clock, PPCLK_DISPCLK);
		get_max_sustainable(adev, &h->max_sustainable.phy_clock, PPCLK_PHYCLK);
		get_max_sustainable(adev, &h->max_sustainable.pixel_clock, PPCLK_PIXCLK);
	}
	if (h->max_sustainable.soc_clock < h->max_sustainable.uclock)
		h->max_sustainable.uclock = h->max_sustainable.soc_clock;
	return 0;
}

/*
 * vega20_od8_set_feature_capabilities. Linux indexes the VBIOS's
 * ODSettingsMax/Min (ATOM_VEGA20_ODSETTING_* order) with OD8_SETTING_*
 * numbers; kept as it is, since that is what decides the capabilities.
 */
static void vega20_od8_set_feature_capabilities(struct frb_vega20_hwmgr *h)
{
	const uint8_t *cap = h->od_feature_capabilities;
	const uint32_t *mx = h->od_settings_max, *mn = h->od_settings_min;
	uint32_t c = 0;

	if (h->f[GNLD_DPM_GFXCLK].enabled) {
		if (cap[ATOM_VEGA20_ODFEATURE_GFXCLK_LIMITS] && mx[OD8_SETTING_GFXCLK_FMAX] > 0 &&
		    mn[OD8_SETTING_GFXCLK_FMIN] > 0 && mx[OD8_SETTING_GFXCLK_FMAX] >= mn[OD8_SETTING_GFXCLK_FMIN])
			c |= OD8_GFXCLK_LIMITS;
		if (cap[ATOM_VEGA20_ODFEATURE_GFXCLK_CURVE] &&
		    mn[OD8_SETTING_GFXCLK_VOLTAGE1] >= PP_GET(h, MinVoltageGfx) / VOLTAGE_SCALE &&
		    mx[OD8_SETTING_GFXCLK_VOLTAGE3] <= PP_GET(h, MaxVoltageGfx) / VOLTAGE_SCALE &&
		    mx[OD8_SETTING_GFXCLK_VOLTAGE3] >= mn[OD8_SETTING_GFXCLK_VOLTAGE1])
			c |= OD8_GFXCLK_CURVE;
	}
	if (h->f[GNLD_DPM_UCLK].enabled && h->mem.count >= 2) {
		h->od_settings_min[OD8_SETTING_UCLK_FMAX] = h->mem.value[h->mem.count - 2];
		if (cap[ATOM_VEGA20_ODFEATURE_UCLK_MAX] && mn[OD8_SETTING_UCLK_FMAX] > 0 &&
		    mx[OD8_SETTING_UCLK_FMAX] > 0 && mx[OD8_SETTING_UCLK_FMAX] >= mn[OD8_SETTING_UCLK_FMAX])
			c |= OD8_UCLK_MAX;
	}
	if (cap[ATOM_VEGA20_ODFEATURE_POWER_LIMIT] && mx[OD8_SETTING_POWER_PERCENTAGE] > 0 &&
	    mx[OD8_SETTING_POWER_PERCENTAGE] <= 100 && mn[OD8_SETTING_POWER_PERCENTAGE] > 0 &&
	    mn[OD8_SETTING_POWER_PERCENTAGE] <= 100)
		c |= OD8_POWER_LIMIT;
	if (h->f[GNLD_FAN_CONTROL].enabled) {
		if (cap[ATOM_VEGA20_ODFEATURE_FAN_ACOUSTIC_LIMIT] && mn[OD8_SETTING_FAN_ACOUSTIC_LIMIT] > 0 &&
		    mx[OD8_SETTING_FAN_ACOUSTIC_LIMIT] > 0 &&
		    mx[OD8_SETTING_FAN_ACOUSTIC_LIMIT] >= mn[OD8_SETTING_FAN_ACOUSTIC_LIMIT])
			c |= OD8_ACOUSTIC_LIMIT_SCLK;
		if (cap[ATOM_VEGA20_ODFEATURE_FAN_SPEED_MIN] &&
		    mn[OD8_SETTING_FAN_MIN_SPEED] >= PP_GET(h, FanPwmMin) * PP_GET(h, FanMaximumRpm) / 100 &&
		    mx[OD8_SETTING_FAN_MIN_SPEED] > 0 && mx[OD8_SETTING_FAN_MIN_SPEED] >= mn[OD8_SETTING_FAN_MIN_SPEED])
			c |= OD8_FAN_SPEED_MIN;
	}
	if (h->f[GNLD_THERMAL].enabled) {
		if (cap[ATOM_VEGA20_ODFEATURE_TEMPERATURE_FAN] && mx[OD8_SETTING_FAN_TARGET_TEMP] > 0 &&
		    mn[OD8_SETTING_FAN_TARGET_TEMP] > 0 && mx[OD8_SETTING_FAN_TARGET_TEMP] >= mn[OD8_SETTING_FAN_TARGET_TEMP])
			c |= OD8_TEMPERATURE_FAN;
		if (cap[ATOM_VEGA20_ODFEATURE_TEMPERATURE_SYSTEM] && mx[OD8_SETTING_OPERATING_TEMP_MAX] > 0 &&
		    mn[OD8_SETTING_OPERATING_TEMP_MAX] > 0 &&
		    mx[OD8_SETTING_OPERATING_TEMP_MAX] >= mn[OD8_SETTING_OPERATING_TEMP_MAX])
			c |= OD8_TEMPERATURE_SYSTEM;
	}
	if (cap[ATOM_VEGA20_ODFEATURE_MEMORY_TIMING_TUNE])
		c |= OD8_MEMORY_TIMING_TUNE;
	if (cap[ATOM_VEGA20_ODFEATURE_FAN_ZERO_RPM_CONTROL] && PP_GET(h, FanZeroRpmEnable))
		c |= OD8_FAN_ZERO_RPM_CONTROL;
	h->od8_capabilities = c;
}

/* vega20_od8_get_gfx_clock_base_voltage */
static uint32_t od8_base_voltage(struct frb_device *adev, uint32_t freq)
{
	uint32_t v = 0;

	send(adev, PPSMC_MSG_GetAVFSVoltageByDpm, (AVFS_CURVE << 24) | (OD8_HOTCURVE_TEMPERATURE << 16) | freq, &v);
	return v / VOLTAGE_SCALE;
}

/*
 * vega20_od8_initialize_default_settings: the OverDrive table from the SMU,
 * the GFXCLK curve's three points filled in when the board allows it, the
 * table back to the SMU. (The per-setting min/max/default bookkeeping is
 * for the sysfs interface Firebrand does not have.)
 */
static int vega20_od8_initialize_default_settings(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int r;

	vega20_od8_set_feature_capabilities(h);
	r = frb_smu11_copy_table_from_smc(adev, h->od_table, FRB_SMU11_TABLE_OVERDRIVE);
	if (r) {
		frb_err(adev, "Failed to export over drive table!");
		return r;
	}
	if (h->od8_capabilities & OD8_GFXCLK_CURVE) {
		uint32_t f1 = OD_GET(h, GfxclkFmin), f3 = OD_GET(h, GfxclkFmax), f2 = (f1 + f3) / 2;

		OD_SET(h, GfxclkFreq1, f1);
		OD_SET(h, GfxclkFreq3, f3);
		OD_SET(h, GfxclkFreq2, f2);
		OD_SET(h, GfxclkVolt1, od8_base_voltage(adev, f1) * VOLTAGE_SCALE);
		OD_SET(h, GfxclkVolt2, od8_base_voltage(adev, f2) * VOLTAGE_SCALE);
		OD_SET(h, GfxclkVolt3, od8_base_voltage(adev, f3) * VOLTAGE_SCALE);
	}
	r = frb_smu11_copy_table_to_smc(adev, h->od_table, FRB_SMU11_TABLE_OVERDRIVE);
	if (r)
		frb_err(adev, "Failed to import over drive table!");
	return r;
}

/* vega20_enable_dpm_tasks */
static int vega20_enable_dpm_tasks(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int r;

	send(adev, PPSMC_MSG_NumOfDisplays, 0, NULL);
	if ((r = vega20_set_allowed_featuresmask(adev, h)))
		return r;
	if ((r = vega20_init_smc_table(adev, h)))
		return r;
	send0(adev, PPSMC_MSG_RunBtc, NULL);		/* vega20_run_btc */
	send0(adev, PPSMC_MSG_RunAfllBtc, NULL);	/* vega20_run_btc_afll */
	if ((r = vega20_enable_all_smu_features(adev, h)))
		return r;
	if ((r = vega20_override_pcie_parameters(adev, h)))
		return r;
	/* vega20_notify_smc_display_change */
	if (h->f[GNLD_DPM_UCLK].enabled)
		send(adev, PPSMC_MSG_SetUclkFastSwitch, 1, NULL);
	send(adev, PPSMC_MSG_SetFclkGfxClkRatio, 0, NULL);	/* registry fclk_gfxclk_ratio */
	/* vega20_init_powergate_state: UVD and VCE gated (no messages) */
	h->uvd_power_gated = 1;
	h->vce_power_gated = 1;
	if ((r = vega20_setup_default_dpm_tables(adev, h)))
		return r;
	if ((r = vega20_init_max_sustainable_clocks(adev, h)))
		return r;
	/* vega20_power_control_set_level: no PowerContainment cap, nothing sent */
	if ((r = vega20_od8_initialize_default_settings(adev, h)))
		return r;
	/* vega20_populate_umdpstate_clocks: no messages */
	send(adev, PPSMC_MSG_GetPptLimit, POWER_SOURCE_AC << 16, &h->default_power_limit);
	return 0;
}

/*
 * phm_start_thermal_controller -> vega20_start_thermal_controller: the
 * alert range (SMU7ThermalWithDelayPolicy's minimum, below 0 C, so 0; the
 * software shutdown temperature as the top), the alert enables, the fan
 * target (SetFanTemperatureTarget, FanTargetTemperature = 105 C).
 */
static int vega20_start_thermal_controller(struct frb_device *adev, struct frb_vega20_hwmgr *h)
{
	int low = VEGA20_THERMAL_MINIMUM_ALERT_TEMP, high = VEGA20_THERMAL_MAXIMUM_ALERT_TEMP;
	uint32_t val;

	if (h->thermal_controller_type == 0)	/* ATOM_VEGA20_PP_THERMALCONTROLLER_NONE */
		return 0;
	if (high > h->us_software_shutdown_temp)
		high = h->us_software_shutdown_temp;
	if (low > high)
		return -EINVAL;
	val = RREG32_SOC15(THM, 0, mmTHM_THERMAL_INT_CTRL);
	val = REG_SET_FIELD(val, THM_THERMAL_INT_CTRL, MAX_IH_CREDIT, 5);
	val = REG_SET_FIELD(val, THM_THERMAL_INT_CTRL, THERM_IH_HW_ENA, 1);
	val = REG_SET_FIELD(val, THM_THERMAL_INT_CTRL, DIG_THERM_INTH, high);
	val = REG_SET_FIELD(val, THM_THERMAL_INT_CTRL, DIG_THERM_INTL, low);
	val &= ~THM_THERMAL_INT_CTRL__THERM_TRIGGER_MASK_MASK;
	val &= ~THM_THERMAL_INT_CTRL__THERM_INTH_MASK_MASK;
	val &= ~THM_THERMAL_INT_CTRL__THERM_INTL_MASK_MASK;
	WREG32_SOC15(THM, 0, mmTHM_THERMAL_INT_CTRL, val);
	/* vega20_thermal_enable_alert */
	val = (1 << THM_THERMAL_INT_ENA__THERM_INTH_CLR__SHIFT) | (1 << THM_THERMAL_INT_ENA__THERM_INTL_CLR__SHIFT) |
	      (1 << THM_THERMAL_INT_ENA__THERM_TRIGGER_CLR__SHIFT);
	WREG32_SOC15(THM, 0, mmTHM_THERMAL_INT_ENA, val);
	h->thermal_low = low;
	h->thermal_high = high;
	/* vega20_thermal_setup_fan_table */
	send(adev, PPSMC_MSG_SetFanTemperatureTarget, PP_GET(h, FanTargetTemperature), NULL);
	return 0;
}

/* hwmgr_hw_init for Vega 20 */
int frb_vega20_hwmgr_hw_init(struct frb_device *adev)
{
	struct frb_smu11 *smu = adev->smu11;
	struct frb_vega20_hwmgr *h;
	uint64_t running = 0;
	int r;

	if (!smu)
		return -EINVAL;
	if (!smu->hw) {
		smu->hw = (struct frb_vega20_hwmgr *)adev->os->alloc(adev->os->cookie, sizeof(*smu->hw));
		if (!smu->hw)
			return -ENOMEM;
	}
	h = smu->hw;
	memset(h, 0, sizeof(*h));
	h->feature_mask = adev->pp_feature ? adev->pp_feature : FRB_VEGA20_PP_FEATURE_MASK_DEFAULT;
	frb_device_get_pcie_info(adev);

	/* pptable_init, then backend_init */
	if ((r = vega20_pp_tables_initialize(adev, h)))
		return r;
	vega20_set_default_registry_data(adev, h);
	if ((r = vega20_init_dpm_defaults(adev, h)))
		return r;
	/* phm_setup_asic -> vega20_setup_asic_task: the sclk threshold, no BACO: no register access */
	/* phm_enable_dynamic_state_management: smum_is_dpm_running first (Linux evaluates it before in_suspend) */
	vega20_get_enabled_smc_features(adev, &running);
	if ((r = vega20_enable_dpm_tasks(adev, h)))
		return r;
	if ((r = vega20_start_thermal_controller(adev, h)))
		return r;
	/* psm_set_performance_states: nothing sent at init */

	smu->smu_version = h->smu_version;
	smu->features_enabled = h->features_enabled;
	smu->dpm_enabled = 1;
	frb_info(adev, "smu: SMU %u.%u.%u, features enabled 0x%llx; GFXCLK %u levels %u..%u MHz, UCLK %u levels "
		 "%u..%u MHz, SOCCLK %u..%u MHz, FCLK %u..%u MHz; PPT %u W; thermal alert %d..%d C, fan target %u C; "
		 "GPU %d C; %d message(s) not acknowledged",
		 (h->smu_version >> 16) & 0xff, (h->smu_version >> 8) & 0xff, h->smu_version & 0xff,
		 (unsigned long long)h->features_enabled, h->gfx.count, h->gfx.value[0],
		 h->gfx.count ? h->gfx.value[h->gfx.count - 1] : 0, h->mem.count, h->mem.value[0],
		 h->mem.count ? h->mem.value[h->mem.count - 1] : 0, h->soc.value[0],
		 h->soc.count ? h->soc.value[h->soc.count - 1] : 0, h->fclk.value[0],
		 h->fclk.count ? h->fclk.value[h->fclk.count - 1] : 0, h->default_power_limit, h->thermal_low,
		 h->thermal_high, (unsigned int)PP_GET(h, FanTargetTemperature), frb_soc15_get_temperature(adev),
		 smu->failed_msgs);
	return 0;
}

/* vega20_smumgr.c vega20_enable_smc_features: the mask's low then high word, enable or disable */
static int vega20_enable_smc_features(struct frb_device *adev, int enable, uint64_t feature_mask)
{
	uint32_t smu_features_low = (uint32_t)feature_mask, smu_features_high = (uint32_t)(feature_mask >> 32);
	int ret;

	if (enable) {
		if ((ret = send(adev, PPSMC_MSG_EnableSmuFeaturesLow, smu_features_low, NULL)))
			return ret;
		if ((ret = send(adev, PPSMC_MSG_EnableSmuFeaturesHigh, smu_features_high, NULL)))
			return ret;
	} else {
		if ((ret = send(adev, PPSMC_MSG_DisableSmuFeaturesLow, smu_features_low, NULL)))
			return ret;
		if ((ret = send(adev, PPSMC_MSG_DisableSmuFeaturesHigh, smu_features_high, NULL)))
			return ret;
	}
	return 0;
}

/* vega20_enable_disable_uvd_dpm */
static int vega20_enable_disable_uvd_dpm(struct frb_device *adev, struct frb_vega20_hwmgr *h, int enable)
{
	int ret;

	if (h->f[GNLD_DPM_UVD].supported) {
		/* "feature DPM UVD already enabled/disabled" is only a debug line on Linux: the messages go out */
		ret = vega20_enable_smc_features(adev, enable, h->f[GNLD_DPM_UVD].bitmap);
		if (ret) {
			frb_err(adev, "[EnableDisableUVDDPM] Attempt to Enable/Disable DPM UVD Failed!");
			return ret;
		}
		h->f[GNLD_DPM_UVD].enabled = enable;
	}
	return 0;
}

/*
 * pp_dpm_powergate_uvd -> vega20_power_gate_uvd: what amdgpu_dpm_enable_uvd
 * comes to on Vega 20 (hwmgr->pm_en; amdgpu's pwr_state check in front of it
 * is the same test as uvd_power_gated here, both start "gated").
 */
int frb_vega20_power_gate_uvd(struct frb_device *adev, int bgate)
{
	struct frb_vega20_hwmgr *h = adev->smu11 ? adev->smu11->hw : NULL;

	if (!h || !adev->smu11->dpm_enabled)
		return -ENODEV;
	bgate = bgate != 0;
	if (h->uvd_power_gated == bgate)
		return 0;

	h->uvd_power_gated = bgate;
	return vega20_enable_disable_uvd_dpm(adev, h, !bgate);
}

/* vega20_disable_dpm_tasks (pp hw_fini): every SMU feature off, as amdgpu leaves the SMU at unload */
void frb_vega20_hwmgr_hw_fini(struct frb_device *adev)
{
	if (!adev->smu11 || !adev->smu11->dpm_enabled)
		return;
	send0(adev, PPSMC_MSG_DisableAllSmuFeatures, NULL);
	adev->smu11->dpm_enabled = 0;
}

/* ------------------------------------------------------------------------- */
/*
 * What DC's display requirements make powerplay do (amdgpu_dpm display
 * configuration change, then the DISPLAY_CONFIG_CHANGE task's
 * psm_adjust_power_state_dynamic), Vega 20's hooks in their order:
 *   vega20_display_clock_voltage_request   DC's DISPCLK / PHYCLK hard minimums
 *   vega20_pre_display_configuration_changed_task
 *                                          NumOfDisplays 0, UCLK and FCLK to the top
 *   vega20_display_configuration_changed_task  (NumOfDisplays only with a
 *                                          watermark table, which DC never sends here)
 *   vega20_apply_clocks_adjust_rules       the soft/hard levels (no message)
 *   vega20_notify_smc_display_config_after_ps_adjustment
 *                                          DCEFCLK hard min, UCLK hard min = DC's memory floor
 *   vega20_dpm_force_dpm_level(AUTO)       upload_dpm_min_level, upload_dpm_max_level
 */

int frb_vega20_display_clock_request(struct frb_device *adev, int clk, uint32_t khz)
{
	struct frb_vega20_hwmgr *h = adev->smu11 ? adev->smu11->hw : NULL;

	if (!h || !adev->smu11->dpm_enabled || !h->f[GNLD_DPM_DCEFCLK].enabled)
		return -ENODEV;
	return send(adev, PPSMC_MSG_SetHardMinByFreq, (uint32_t)clk << 16 | (khz / 1000), NULL);
}

static uint32_t top(const struct dpm_ladder *t)
{
	return t->count ? t->value[t->count - 1] : 0;
}

/*
 * min_mem_khz: DC's min_memory_clock_khz (the bandwidth model's yclk);
 * nb_pstate_switch_disable: the model said the memory clock must not switch.
 */
int frb_vega20_display_config_changed(struct frb_device *adev, uint32_t min_mem_khz, uint32_t min_dcef_khz,
				      int nb_pstate_switch_disable)
{
	struct frb_vega20_hwmgr *h = adev->smu11 ? adev->smu11->hw : NULL;
	uint32_t uclk_hard_min;

	if (!h || !adev->smu11->dpm_enabled)
		return -ENODEV;
	/* pre_display_config_changed */
	send(adev, PPSMC_MSG_NumOfDisplays, 0, NULL);
	if (h->f[GNLD_DPM_UCLK].enabled && h->mem.count)
		send(adev, PPSMC_MSG_SetHardMinByFreq, PPCLK_UCLK << 16 | top(&h->mem), NULL);
	if (h->f[GNLD_DPM_FCLK].enabled && h->fclk.count)
		send(adev, PPSMC_MSG_SetSoftMinByFreq, PPCLK_FCLK << 16 | top(&h->fclk), NULL);
	/* apply_clocks_adjust_rules: UCLK's hard minimum (no message) */
	uclk_hard_min = h->mem.count ? h->mem.value[0] : 0;
	if (uclk_hard_min < min_mem_khz / 1000)
		uclk_hard_min = min_mem_khz / 1000;
	if (nb_pstate_switch_disable)
		uclk_hard_min = top(&h->mem);
	(void)uclk_hard_min;
	/* notify_smc_display_config_after_ps_adjustment */
	if (h->f[GNLD_DPM_DCEFCLK].supported) {
		if (!frb_vega20_display_clock_request(adev, PPCLK_DCEFCLK, min_dcef_khz) &&
		    h->f[GNLD_DS_DCEFCLK].supported)
			send(adev, PPSMC_MSG_SetMinDeepSleepDcefclk, 0, NULL);
	}
	if (h->f[GNLD_DPM_UCLK].enabled)
		send(adev, PPSMC_MSG_SetHardMinByFreq,
		     PPCLK_UCLK << 16 | (min_mem_khz / 1000 > h->mem.value[0] ? min_mem_khz / 1000 : h->mem.value[0]), NULL);
	/* force_dpm_level(AUTO): upload_dpm_min_level, then _max_level */
	if (h->f[GNLD_DPM_GFXCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMinByFreq, PPCLK_GFXCLK << 16 | (h->gfx.value[0] & 0xffff), NULL);
	if (h->f[GNLD_DPM_UCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMinByFreq, PPCLK_UCLK << 16 | (h->mem.value[0] & 0xffff), NULL);
	if (h->f[GNLD_DPM_SOCCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMinByFreq, PPCLK_SOCCLK << 16 | (h->soc.value[0] & 0xffff), NULL);
	if (h->f[GNLD_DPM_GFXCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMaxByFreq, PPCLK_GFXCLK << 16 | (top(&h->gfx) & 0xffff), NULL);
	if (h->f[GNLD_DPM_UCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMaxByFreq, PPCLK_UCLK << 16 | (top(&h->mem) & 0xffff), NULL);
	if (h->f[GNLD_DPM_SOCCLK].enabled)
		send(adev, PPSMC_MSG_SetSoftMaxByFreq, PPCLK_SOCCLK << 16 | (top(&h->soc) & 0xffff), NULL);
	return 0;
}

int frb_vega20_set_fclk_soft_min(struct frb_device *adev, int top_level)
{
	struct frb_vega20_hwmgr *h = adev->smu11 ? adev->smu11->hw : NULL;
	uint32_t mhz;

	if (!h || !adev->smu11->dpm_enabled || !h->f[GNLD_DPM_FCLK].enabled || !h->fclk.count)
		return -ENODEV;
	mhz = top_level ? top(&h->fclk) : h->fclk.value[0];
	frb_info(adev, "dpm: FCLK soft minimum %u MHz (by hand)", mhz);
	return send(adev, PPSMC_MSG_SetSoftMinByFreq, PPCLK_FCLK << 16 | (mhz & 0xffff), NULL);
}

/* --- the readout (vega20_read_sensor's sources), for frbctl dpm --------------------- */

#define MET(f)	frb_get_le16(m, offsetof(SmuMetrics_t, f))

/* vega20_get_current_clk_freq: GetDpmClockFreq(clk << 16), MHz -> 10 kHz */
static uint32_t vega20_get_current_clk_freq(struct frb_device *adev, int clk_id)
{
	uint32_t freq = 0;

	if (send(adev, PPSMC_MSG_GetDpmClockFreq, (uint32_t)clk_id << 16, &freq))
		return 0;
	return freq * 100;
}

static void ladder_out(const struct dpm_ladder *l, uint32_t *count, uint32_t *out)
{
	uint32_t i;

	*count = l->count < 16 ? l->count : 16;
	for (i = 0; i < *count; i++)
		out[i] = l->value[i];
}

/*
 * The SMU11 side of frbctl dpm: what amdgpu's sensors read on Vega 20.
 * GFX_SCLK is SmuMetrics_t's AverageGfxclkFrequency (vega20_read_sensor),
 * GFX_MCLK GetDpmClockFreq(UCLK); SOCCLK and FCLK the same message
 * (pp_dpm_socclk / pp_dpm_fclk); power CurrSocketPower (GPU_INPUT_POWER:
 * AverageSocketPower is only valid on SMU 40.46, vega20_get_gpu_power); the
 * edge/HBM temperatures from the metrics, the hotspot from THM's CTF
 * (vega20_thermal_get_temperature); VDDGFX from SVI2 plane 0's VID
 * (convert_to_vddc). No fan (the MI50 is passive; no tach on the card).
 */
int frb_vega20_get_status(struct frb_device *adev, struct frb_pm_status *st)
{
	struct frb_smu11 *smu = adev->smu11;
	struct frb_vega20_hwmgr *h = smu ? smu->hw : NULL;
	uint8_t m[sizeof(SmuMetrics_t)];
	uint32_t vid;
	int r;

	memset(st, 0, sizeof(*st));
	st->cg_flags = adev->cg_flags;
	st->gating = (adev->want_gating ? 1 : 0) | (adev->cg_gated ? 2 : 0);
	st->temp_mc = frb_soc15_get_temperature(adev) * 1000;
	st->temp_hotspot = st->temp_mc;
	vid = (RREG32_SOC15(SMUIO, 0, mmSMUSVI0_TEL_PLANE0) & SMUSVI0_TEL_PLANE0__SVI0_PLANE0_VDDCOR_MASK) >>
	      SMUSVI0_TEL_PLANE0__SVI0_PLANE0_VDDCOR__SHIFT;
	/*
	 * convert_to_vddc((uint8_t)val_vid), VOLTAGE_SCALE 4: the field is 9
	 * bits wide and reads with bit 8 set on the MI50 (0x181 idle, 0x157
	 * busy); Linux's cast keeps the low 8, the SVI2 VID (0x81: 743 mV, the
	 * capture's idle VDDGFX)
	 */
	vid &= 0xff;
	st->vddgfx_mv = (6200 - vid * 25) / 4;
	if (!smu || !smu->started)
		return 0;
	st->smu_version = smu->smu_version;
	st->smc_feature_status = (uint32_t)smu->features_enabled;
	if (!smu->dpm_enabled || !h)
		return 0;
	st->enabled = 1;
	st->forced_level = AMD_DPM_FORCED_LEVEL_AUTO;	/* forcing levels is not ported on SOC15 */
	{
		/*
		 * vega20_get_ppfeature_status: the features enabled now, not at
		 * hw_init (DPM_UVD, bit 4, comes and goes with the decoder's use)
		 */
		uint64_t now = 0;

		if (!vega20_get_enabled_smc_features(adev, &now) && now)
			st->smc_feature_status = (uint32_t)now;
	}
	ladder_out(&h->gfx, &st->gfx_count, st->gfx_levels);
	ladder_out(&h->mem, &st->uclk_count, st->uclk_levels);
	ladder_out(&h->soc, &st->soc_count, st->soc_levels);
	st->sclk_count = st->gfx_count < 8 ? st->gfx_count : 8;
	st->mclk_count = st->uclk_count < 8 ? st->uclk_count : 8;
	{
		uint32_t i;

		for (i = 0; i < st->sclk_count; i++)
			st->sclk_levels[i] = st->gfx_levels[i] * 100;
		for (i = 0; i < st->mclk_count; i++)
			st->mclk_levels[i] = st->uclk_levels[i] * 100;
	}
	st->cur_sclk_index = st->cur_mclk_index = -1;

	/* vega20_get_metrics_table (bypassing the 1 ms cache: frbctl asks seldom) */
	memset(m, 0, sizeof(m));
	r = frb_smu11_copy_table_from_smc(adev, m, FRB_SMU11_TABLE_SMU_METRICS);
	if (r) {
		frb_err(adev, "dpm: Failed to export SMU metrics table! (%d)", r);
		return r;
	}
	st->cur_sclk = MET(AverageGfxclkFrequency) * 100;
	st->gpu_load = MET(AverageGfxActivity);
	st->mem_load = MET(AverageUclkActivity);
	st->power_mw = MET(CurrSocketPower) * 1000;
	st->temp_edge = (int32_t)MET(TemperatureEdge) * 1000;
	st->temp_hbm = (int32_t)MET(TemperatureHBM) * 1000;
	st->cur_mclk = vega20_get_current_clk_freq(adev, PPCLK_UCLK);
	st->cur_socclk = vega20_get_current_clk_freq(adev, PPCLK_SOCCLK);
	st->cur_fclk = vega20_get_current_clk_freq(adev, PPCLK_FCLK);
	return 0;
}

/*
 * frb_pm_read_sensors' Vega 20 arm (frb_pm.h), for a monitor's once-a-second
 * poll: one SmuMetrics_t transfer (SetDriverDramAddrHigh/Low,
 * TransferTableSmu2Dram) and register reads, nothing else sent. GFXCLK is
 * the metrics' AverageGfxclkFrequency (vega20_read_sensor's GFX_SCLK);
 * UCLK, SOCCLK and FCLK their CurrClock entries, the values
 * GetDpmClockFreq answers, so no message each; the temperatures are
 * amdgpu's edge, junction (hotspot) and memory sensors. The feature mask is
 * the driver's own bookkeeping (h->f[].enabled: every Enable/Disable it
 * sends, DPM_UVD's coming and going included), not GetEnabledSmuFeatures.
 * Without DPM (or before it): THM's CTF temperature and SVI2's VDDGFX, which
 * need no SMU. A message the SMU did not acknowledge fails the read (-EIO),
 * so the caller can back off. No fan: the MI50 has no tach (frbctl dpm).
 */
/* frb_pm_dpm_running's Vega 20 arm: the hwmgr's DPM tasks ran */
int frb_vega20_dpm_running(struct frb_device *adev)
{
	return adev->smu11 && adev->smu11->dpm_enabled;
}

#define CURRCLK(i)	frb_get_le16(m, offsetof(SmuMetrics_t, CurrClock) + 2 * (i))
int frb_vega20_read_sensors(struct frb_device *adev, struct frb_pm_sensors *s)
{
	struct frb_smu11 *smu = adev->smu11;
	struct frb_vega20_hwmgr *h = smu ? smu->hw : NULL;
	uint8_t m[sizeof(SmuMetrics_t)];
	uint32_t vid, i;
	int failed, r;

	memset(s, 0, sizeof(*s));
	s->temp_hotspot_mc = frb_soc15_get_temperature(adev) * 1000;
	s->valid |= FRB_PMS_TEMP_HOTSPOT;
	vid = (RREG32_SOC15(SMUIO, 0, mmSMUSVI0_TEL_PLANE0) & SMUSVI0_TEL_PLANE0__SVI0_PLANE0_VDDCOR_MASK) >>
	      SMUSVI0_TEL_PLANE0__SVI0_PLANE0_VDDCOR__SHIFT;
	s->vddgfx_mv = (6200 - (vid & 0xff) * 25) / 4;	/* convert_to_vddc of the low 8 bits (frb_vega20_get_status) */
	s->valid |= FRB_PMS_VDDGFX;
	if (!smu || !smu->started)
		return -ENODEV;
	s->smu_version = smu->smu_version;
	s->features_kind = FRB_PMS_FEATURES_SMU11;
	s->features = smu->features_enabled;
	s->valid |= FRB_PMS_SMU;
	if (!smu->dpm_enabled || !h)
		return -ENODEV;
	s->dpm_enabled = 1;
	s->features = 0;
	for (i = 0; i < GNLD_FEATURES_MAX; i++)
		if (h->f[i].enabled)
			s->features |= h->f[i].bitmap;
	s->gfx_count = h->gfx.count < 16 ? h->gfx.count : 16;
	s->mem_count = h->mem.count < 16 ? h->mem.count : 16;
	for (i = 0; i < s->gfx_count; i++)
		s->gfx_levels[i] = h->gfx.value[i];
	for (i = 0; i < s->mem_count; i++)
		s->mem_levels[i] = h->mem.value[i];
	if (s->gfx_count || s->mem_count)
		s->valid |= FRB_PMS_LEVELS;
	if (h->default_power_limit) {
		s->power_limit_w = h->default_power_limit;	/* GetPptLimit(AC), W */
		s->valid |= FRB_PMS_POWER_LIMIT;
	}

	failed = smu->failed_msgs;
	memset(m, 0, sizeof(m));
	r = frb_smu11_copy_table_from_smc(adev, m, FRB_SMU11_TABLE_SMU_METRICS);
	if (!r && smu->failed_msgs != failed)
		r = -EIO;
	if (r)
		return r;
	s->gfx_mhz = MET(AverageGfxclkFrequency);
	s->mem_mhz = CURRCLK(PPCLK_UCLK);
	s->valid |= FRB_PMS_CLOCKS;
	s->soc_mhz = CURRCLK(PPCLK_SOCCLK);
	s->fclk_mhz = CURRCLK(PPCLK_FCLK);
	s->valid |= FRB_PMS_SOC_CLOCKS;
	s->gpu_load = MET(AverageGfxActivity);
	s->mem_load = MET(AverageUclkActivity);
	s->valid |= FRB_PMS_LOAD;
	s->power_mw = MET(CurrSocketPower) * 1000;
	s->valid |= FRB_PMS_POWER;
	s->temp_mc = (int32_t)MET(TemperatureEdge) * 1000;
	s->temp_hotspot_mc = (int32_t)MET(TemperatureHotspot) * 1000;
	s->temp_mem_mc = (int32_t)MET(TemperatureHBM) * 1000;
	s->valid |= FRB_PMS_TEMP | FRB_PMS_TEMP_MEM;
	s->throttler = frb_get_le32(m, offsetof(SmuMetrics_t, ThrottlerStatus));
	s->valid |= FRB_PMS_THROTTLE;
	return 0;
}
#undef CURRCLK
#undef MET
