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
 */

/*
 * Southern Islands DPM: Linux's si_dpm.c, close to the letter. The function
 * bodies keep Linux's text (the device pointer is still called `rdev`, the
 * register macros take one argument) so that a diff against si_dpm.c shows
 * only what had to change; what changed and why is said at each site. See
 * rdn_si_dpm.h for the interface and the list of deviations.
 *
 * Byte order: the SMC is big-endian and Linux wraps every SRAM table field
 * in cpu_to_be16/32. Here those are explicit about the host's order (no-ops
 * on PowerPC). Where Linux stores a multi-byte field WITHOUT the wrapper, a
 * little-endian host puts it in the SRAM byte-swapped; those sites use
 * cpu_to_le*() so this host writes the same bytes Linux does, and say so
 * (the measured reference is a Linux session's SRAM dump of this card, the
 * method of the Sea Islands port, docs/HAWAII-DEEP-DIVE.md).
 */

#include <string.h>

#include "rdn_si_dpm.h"
#include "rdn_ci_dpm.h"
#include "rdn_dpm_atom.h"
#include "rdn_family.h"
#include "atom/atom.h"
#include "atom/atombios.h"
#include "linux/pptable.h"
#include "linux/sid.h"
#include "linux/sislands_smc.h"

#ifndef EINVAL
#define EINVAL 22
#endif
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef ENOENT
#define ENOENT 2
#endif
#ifndef EAGAIN
#define EAGAIN 35
#endif

/* ---- Linux's types and idioms ---- */

/* u8, u16 and u32 come from atom/atom_port.h (gcc 4.0 refuses a repeated typedef) */
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#ifndef offsetof
#define offsetof(t, m) __builtin_offsetof(t, m)
#endif

/* rdev->usec_timeout */
#define SI_USEC_TIMEOUT 100000

/* radeon_family's Southern Islands members; rdev->family */
enum si_chip_family {
	CHIP_TAHITI = 1,
	CHIP_PITCAIRN,
	CHIP_VERDE,
	CHIP_OLAND,
	CHIP_HAINAN
};

/* radeon.h */
enum radeon_int_thermal_type {
	THERMAL_TYPE_NONE,
	THERMAL_TYPE_EXTERNAL,
	THERMAL_TYPE_EXTERNAL_GPIO,
	THERMAL_TYPE_RV6XX,
	THERMAL_TYPE_RV770,
	THERMAL_TYPE_ADT7473_WITH_INTERNAL,
	THERMAL_TYPE_EVERGREEN,
	THERMAL_TYPE_SUMO,
	THERMAL_TYPE_NI,
	THERMAL_TYPE_SI,
	THERMAL_TYPE_EMC2103_WITH_INTERNAL,
	THERMAL_TYPE_CI,
	THERMAL_TYPE_KV,
};

enum radeon_dpm_auto_throttle_src {
	RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL,
	RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL
};

enum radeon_dpm_event_src {
	RADEON_DPM_EVENT_SRC_ANALOG = 0,
	RADEON_DPM_EVENT_SRC_EXTERNAL = 1,
	RADEON_DPM_EVENT_SRC_DIGITAL = 2,
	RADEON_DPM_EVENT_SRC_ANALOG_OR_EXTERNAL = 3,
	RADEON_DPM_EVENT_SRC_DIGIAL_OR_EXTERNAL = 4
};

/* r600_dpm.h */
#define R600_AH_DFLT			5
#define R600_TD_DFLT			0
#define R600_UTC_DFLT_00		0x24
#define R600_UTC_DFLT_XX		0x22
#define R600_DTC_DFLT_00		0x24
#define R600_DTC_DFLT_XX		0x22
#define R600_VOLTAGERESPONSETIME_DFLT	1000
#define R600_BACKBIASRESPONSETIME_DFLT	1000
#define R600_TPC_DFLT			0x200
#define R600_SSTU_DFLT			0
#define R600_SST_DFLT			0x00C8
#define R600_GICST_DFLT			0x200
#define R600_REFERENCEDIVIDER_DFLT	4
#define R600_PM_NUMBER_OF_TC		15
#define R600_TEMP_RANGE_MIN		(90 * 1000)
#define R600_TEMP_RANGE_MAX		(120 * 1000)
#define FDO_PWM_MODE_STATIC		1
#define FDO_PWM_MODE_STATIC_RPM		5

enum r600_td {
	R600_TD_AUTO,
	R600_TD_UP,
	R600_TD_DOWN,
};

enum r600_display_gap {
	R600_PM_DISPLAY_GAP_VBLANK_OR_WM = 0,
	R600_PM_DISPLAY_GAP_VBLANK       = 1,
	R600_PM_DISPLAY_GAP_WATERMARK    = 2,
	R600_PM_DISPLAY_GAP_IGNORE       = 3,
};

/* r600_dpm.c */
static const u32 r600_utc[R600_PM_NUMBER_OF_TC] = {
	R600_UTC_DFLT_00, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX,
	R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX,
	R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX, R600_UTC_DFLT_XX,
};

static const u32 r600_dtc[R600_PM_NUMBER_OF_TC] = {
	R600_DTC_DFLT_00, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX,
	R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX,
	R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX, R600_DTC_DFLT_XX,
};

/* rv770_dpm.h, cypress_dpm.h */
#define RV770_ASI_DFLT			1000
#define CYPRESS_HASI_DFLT		400000
#define RV770_DEFAULT_VCLK_FREQ		53300
#define RV770_DEFAULT_DCLK_FREQ		40000

/* si.c: PCIE_BUS_CLK / 10 */
#define TCLK				(10000 / 10)

/* nid.h: the arbiter's other timing sets and the switch request, which sid.h leaves out */
#define MC_ARB_DRAM_TIMING_1		0x27f0
#define MC_ARB_DRAM_TIMING_2		0x27f4
#define MC_ARB_DRAM_TIMING_3		0x27f8
#define MC_ARB_DRAM_TIMING2_1		0x27fc
#define MC_ARB_DRAM_TIMING2_2		0x2800
#define MC_ARB_DRAM_TIMING2_3		0x2804
#define MC_CG_CONFIG			0x25bc
#define MC_ARB_CG			0x27e8
#define CG_ARB_REQ(x)			((x) << 0)
#define CG_ARB_REQ_MASK			(0xff << 0)

/* rv770d.h: the memory module index in BIOS_SCRATCH_4[23:16] */
#ifndef BIOS_SCRATCH_4
#define BIOS_SCRATCH_4			0x1734
#endif

/* radeon_reg.h: the PCIe port's link width (r600_get_pcie_lanes) */
#define RADEON_PCIE_LC_LINK_WIDTH_CNTL		0xa2
#define RADEON_PCIE_LC_LINK_WIDTH_RD_MASK	(0x7 << 4)
#define RADEON_PCIE_LC_LINK_WIDTH_RD_SHIFT	4
#define RADEON_PCIE_LC_LINK_WIDTH_X0		0
#define RADEON_PCIE_LC_LINK_WIDTH_X1		1
#define RADEON_PCIE_LC_LINK_WIDTH_X2		2
#define RADEON_PCIE_LC_LINK_WIDTH_X4		3
#define RADEON_PCIE_LC_LINK_WIDTH_X8		4
#define RADEON_PCIE_LC_LINK_WIDTH_X12		5
#define RADEON_PCIE_LC_LINK_WIDTH_X16		6

#define NISLANDS_MAX_SMC_PERFORMANCE_LEVELS_PER_SWSTATE 16

/* si_dpm.c */
#define MC_CG_ARB_FREQ_F0           0x0a
#define MC_CG_ARB_FREQ_F1           0x0b
#define MC_CG_ARB_FREQ_F2           0x0c
#define MC_CG_ARB_FREQ_F3           0x0d

#define SMC_RAM_END                 0x20000

#define SCLK_MIN_DEEPSLEEP_FREQ     1350

/* ---- byte order ---- */

/* The SMC's SRAM tables are big-endian (Linux's cpu_to_be16/32). */
static inline u16 cpu_to_be16(u16 v)
{
#if RDN_BIG_ENDIAN
	return v;
#else
	return (u16)((v << 8) | (v >> 8));
#endif
}

static inline u32 cpu_to_be32(u32 v)
{
#if RDN_BIG_ENDIAN
	return v;
#else
	return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
#endif
}

#define be16_to_cpu(v) cpu_to_be16(v)
#define be32_to_cpu(v) cpu_to_be32(v)

/*
 * cpu_to_le32() (atom_port.h) is what a little-endian host leaves in memory
 * for a field Linux forgets to wrap. Used only where Linux's own bytes are
 * the reference; each site says which field.
 */

/* VBIOS data is little-endian; read it as bytes. */
#define le16_at(base, off)	rdn_get_le16((base), (uint32_t)(off))
#define le32_at(base, off)	rdn_get_le32((base), (uint32_t)(off))
#define u8_at(base, off)	rdn_get_u8((base), (uint32_t)(off))

static inline void put_le16(void *base, uint32_t off, uint16_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(void *base, uint32_t off, uint32_t v)
{
	uint8_t *p = (uint8_t *)base + off;

	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* ---- the power state structures: rv770_dpm.h, cypress_dpm.h, ni_dpm.h, si_dpm.h (the fields si_dpm.c uses) ---- */

struct rv7xx_pl {
	u32 sclk;
	u32 mclk;
	u16 vddc;
	u16 vddci; /* eg+ only */
	u32 flags;
	enum rdn_ci_pcie_gen pcie_gen; /* si+ only */
};

struct ni_ps {
	u16 performance_level_count;
	bool dc_compatible;
	struct rv7xx_pl performance_levels[NISLANDS_MAX_SMC_PERFORMANCE_LEVELS_PER_SWSTATE];
};

/* radeon.h's struct radeon_ps (`class` is `classification` here) */
struct si_rps {
	u32 caps;
	u32 classification;
	u32 classification2;
	u32 vclk;
	u32 dclk;
	u32 evclk;
	u32 ecclk;
	bool vce_active;
	struct ni_ps *ps_priv;
};

struct rv7xx_power_info {
	bool mem_gddr5;
	bool voltage_control; /* vddc */
	bool mvdd_control;
	bool sclk_ss;
	bool mclk_ss;
	bool dynamic_ss;
	bool gfx_clock_gating;
	bool thermal_protection;
	u32 mvdd_split_frequency;
	u16 max_vddc;
	u16 max_vddc_in_table;
	u16 min_vddc_in_table;
	u16 acpi_vddc;
	u32 ref_div;
	u32 active_auto_throttle_sources;
	u32 mclk_stutter_mode_threshold;
	u32 mclk_strobe_mode_threshold;
	u32 mclk_edc_enable_threshold;
	u32 bsp;
	u32 bsu;
	u32 pbsp;
	u32 pbsu;
	u32 dsp;
	u32 psp;
	u32 asi;
	u32 pasi;
	u32 vrc;
};

struct evergreen_power_info {
	/* must be first! */
	struct rv7xx_power_info rv7xx;
	/* flags */
	bool vddci_control;
	bool dynamic_ac_timing;
	bool light_sleep;
	bool pcie_performance_request;
	bool sclk_deep_sleep;
	bool smu_uvd_hs;
	bool uvd_enabled;
	/* stored values */
	u16 acpi_vddci;
	u32 mclk_edc_wr_enable_threshold;
	struct atom_voltage_table vddc_voltage_table;
	struct atom_voltage_table vddci_voltage_table;
	struct si_rps current_rps;
	struct si_rps requested_rps;
};

#define NISLANDS_DCCAC_MAX_LEVELS 8
#define NISLANDS_DCCAC_LEVEL_0 0

struct ni_leakage_coeffients {
	u32 at;
	u32 bt;
	u32 av;
	u32 bv;
	s32 t_slope;
	s32 t_intercept;
	u32 t_ref;
};

struct ni_power_info {
	/* must be first! */
	struct evergreen_power_info eg;
	u32 mclk_rtt_mode_threshold;
	/* flags */
	bool support_cac_long_term_average;
	bool cac_enabled;
	bool cac_configuration_required;
	bool driver_calculate_cac_leakage;
	bool pc_enabled;
	bool enable_power_containment;
	bool enable_cac;
	bool enable_sq_ramping;
	struct ni_ps current_ps;
	struct ni_ps requested_ps;
};

enum si_cac_config_reg_type {
	SISLANDS_CACCONFIG_MMR = 0,
	SISLANDS_CACCONFIG_CGIND,
	SISLANDS_CACCONFIG_MAX
};

struct si_cac_config_reg {
	u32 offset;
	u32 mask;
	u32 shift;
	u32 value;
	enum si_cac_config_reg_type type;
};

struct si_powertune_data {
	u32 cac_window;
	u32 l2_lta_window_size_default;
	u8 lts_truncate_default;
	u8 shift_n_default;
	u8 operating_temp;
	struct ni_leakage_coeffients leakage_coefficients;
	u32 fixed_kt;
	u32 lkge_lut_v0_percent;
	u8 dc_cac[NISLANDS_DCCAC_MAX_LEVELS];
	bool enable_powertune_by_default;
};

struct si_dyn_powertune_data {
	u32 cac_leakage;
	s32 leakage_minimum_temperature;
	u32 wintime;
	u32 l2_lta_window_size;
	u8 lts_truncate;
	u8 shift_n;
	u8 dc_pwr_value;
	bool disable_uvd_powertune;
};

struct si_dte_data {
	u32 tau[SMC_SISLANDS_DTE_MAX_FILTER_STAGES];
	u32 r[SMC_SISLANDS_DTE_MAX_FILTER_STAGES];
	u32 k;
	u32 t0;
	u32 max_t;
	u8 window_size;
	u8 temp_select;
	u8 dte_mode;
	u8 tdep_count;
	u8 t_limits[SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE];
	u32 tdep_tau[SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE];
	u32 tdep_r[SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE];
	u32 t_threshold;
	bool enable_dte_by_default;
};

struct si_clock_registers {
	u32 cg_spll_func_cntl;
	u32 cg_spll_func_cntl_2;
	u32 cg_spll_func_cntl_3;
	u32 cg_spll_func_cntl_4;
	u32 cg_spll_spread_spectrum;
	u32 cg_spll_spread_spectrum_2;
	u32 dll_cntl;
	u32 mclk_pwrmgt_cntl;
	u32 mpll_ad_func_cntl;
	u32 mpll_dq_func_cntl;
	u32 mpll_func_cntl;
	u32 mpll_func_cntl_1;
	u32 mpll_func_cntl_2;
	u32 mpll_ss1;
	u32 mpll_ss2;
};

/* nislands_smc.h */
struct SMC_NIslands_MCRegisterAddress {
	uint16_t s0;
	uint16_t s1;
};
typedef struct SMC_NIslands_MCRegisterAddress SMC_NIslands_MCRegisterAddress;

struct si_mc_reg_entry {
	u32 mclk_max;
	u32 mc_data[SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE];
};

struct si_mc_reg_table {
	u8 last;
	u8 num_entries;
	u16 valid_flag;
	struct si_mc_reg_entry mc_reg_table_entry[MAX_AC_TIMING_ENTRIES];
	SMC_NIslands_MCRegisterAddress mc_reg_address[SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE];
};

#define SISLANDS_MCREGISTERTABLE_INITIAL_SLOT               0
#define SISLANDS_MCREGISTERTABLE_ACPI_SLOT                  1
#define SISLANDS_MCREGISTERTABLE_ULV_SLOT                   2
#define SISLANDS_MCREGISTERTABLE_FIRST_DRIVERSTATE_SLOT     3

struct si_leakage_voltage_entry {
	u16 voltage;
	u16 leakage_index;
};

#define SISLANDS_LEAKAGE_INDEX0     0xff01
#define SISLANDS_MAX_LEAKAGE_COUNT  4

struct si_leakage_voltage {
	u16 count;
	struct si_leakage_voltage_entry entries[SISLANDS_MAX_LEAKAGE_COUNT];
};

#define SISLANDS_MAX_HARDWARE_POWERLEVELS 5

struct si_ulv_param {
	bool supported;
	u32 cg_ulv_control;
	u32 cg_ulv_parameter;
	u32 volt_change_delay;
	struct rv7xx_pl pl;
	bool one_pcie_lane_in_ulv;
};

struct si_power_info {
	/* must be first! */
	struct ni_power_info ni;
	struct si_clock_registers clock_registers;
	struct si_mc_reg_table mc_reg_table;
	struct atom_voltage_table mvdd_voltage_table;
	struct atom_voltage_table vddc_phase_shed_table;
	struct si_leakage_voltage leakage_voltage;
	u16 mvdd_bootup_value;
	struct si_ulv_param ulv;
	u32 max_cu;
	/* pcie gen */
	enum rdn_ci_pcie_gen force_pcie_gen;
	enum rdn_ci_pcie_gen boot_pcie_gen;
	enum rdn_ci_pcie_gen acpi_pcie_gen;
	u32 sys_pcie_mask;
	/* flags */
	bool enable_dte;
	bool enable_ppm;
	bool vddc_phase_shed_control;
	bool pspp_notify_required;
	bool sclk_deep_sleep_above_low;
	bool voltage_control_svi2;
	bool vddci_control_svi2;
	/* smc offsets */
	u32 sram_end;
	u32 state_table_start;
	u32 soft_regs_start;
	u32 mc_reg_table_start;
	u32 arb_table_start;
	u32 cac_table_start;
	u32 dte_table_start;
	u32 spll_table_start;
	u32 papm_cfg_table_start;
	u32 fan_table_start;
	/* CAC stuff */
	const struct si_cac_config_reg *cac_weights;
	const struct si_cac_config_reg *lcac_config;
	const struct si_cac_config_reg *cac_override;
	const struct si_powertune_data *powertune_data;
	struct si_dyn_powertune_data dyn_powertune_data;
	/* DTE stuff */
	struct si_dte_data dte_data;
	/* scratch structs */
	SMC_SIslands_MCRegisters smc_mc_reg_table;
	SISLANDS_SMC_STATETABLE smc_statetable;
	PP_SIslands_PAPMParameters papm_parm;
	/* SVI2 */
	u8 svd_gpio_id;
	u8 svc_gpio_id;
	/* fan control */
	bool fan_ctrl_is_in_default_mode;
	u32 t_min;
	u32 fan_ctrl_default_mode;
	bool fan_is_controlled_by_smc;
};

#define SISLANDS_INITIAL_STATE_ARB_INDEX    0
#define SISLANDS_ACPI_STATE_ARB_INDEX       1
#define SISLANDS_ULV_STATE_ARB_INDEX        2
#define SISLANDS_DRIVER_STATE_ARB_INDEX     3

#define SISLANDS_DPM2_MAX_PULSE_SKIP        256

#define SISLANDS_DPM2_NEAR_TDP_DEC          10
#define SISLANDS_DPM2_ABOVE_SAFE_INC        5
#define SISLANDS_DPM2_BELOW_SAFE_INC        20

#define SISLANDS_DPM2_TDP_SAFE_LIMIT_PERCENT            80

#define SISLANDS_DPM2_MAXPS_PERCENT_H                   99
#define SISLANDS_DPM2_MAXPS_PERCENT_M                   99

#define SISLANDS_DPM2_SQ_RAMP_MAX_POWER                 0x3FFF
#define SISLANDS_DPM2_SQ_RAMP_MIN_POWER                 0x12
#define SISLANDS_DPM2_SQ_RAMP_MAX_POWER_DELTA           0x15
#define SISLANDS_DPM2_SQ_RAMP_STI_SIZE                  0x1E
#define SISLANDS_DPM2_SQ_RAMP_LTI_RATIO                 0xF

#define SISLANDS_DPM2_PWREFFICIENCYRATIO_MARGIN         10

#define SISLANDS_VRC_DFLT                               0xC000B3
#define SISLANDS_ULVVOLTAGECHANGEDELAY_DFLT             1687
#define SISLANDS_CGULVPARAMETER_DFLT                    0x00040035
#define SISLANDS_CGULVCONTROL_DFLT                      0x1f007550

/* ---- the device: Linux's rdev, rdev->pm and rdev->pm.dpm as si_dpm.c reaches into them ---- */

struct rdn_si_dpm {
	/*
	 * rdev->pm.dpm's family-independent half, as rdn_ci_tables.c parses it
	 * (platform caps, dependency tables, fan, TDP, thermal controller,
	 * reference clock): Linux's `rdev->pm.dpm.X` is `rdev->ci.X` here.
	 */
	struct rdn_ci_dpm ci;
	struct rdn_card *card;

	/* rdev->family, rdev->pdev */
	enum si_chip_family family;
	u16 device;
	u8 revision;

	/* rdev->pm.int_thermal_type */
	int int_thermal_type;

	/* rdev->clock */
	u32 default_sclk;
	u32 default_mclk;
	u32 current_dispclk;

	/* rdev->pm.dpm: the parsed states and the ones in use */
	struct si_rps *ps;
	struct ni_ps *ps_priv;	/* num_ps of them, ps[i].ps_priv = &ps_priv[i] */
	int num_ps;
	struct si_rps *boot_ps;
	struct si_rps *uvd_ps;
	struct si_rps *current_ps;
	struct si_rps *requested_ps;
	struct si_power_info *priv;
	u32 new_active_crtcs;
	int high_pixelclock_count;
	int vce_level;

	/* what the DRM mode list and the watermark code give Linux */
	struct rdn_si_display display;
	int num_crtc;
	u32 sys_pcie_mask;	/* RDN_SI_PCIE_SPEED_*, from the caller */

	/* rdev->smc_fw, for si_upload_firmware() */
	const void *smc_fw;
	size_t smc_fw_size;

	bool enabled;

	/* Firebrand's: RDN_SI_DPM_* as given, the message hook, the last state adjustment's memory clock decision */
	unsigned quirks;
	rdn_si_dpm_msg_hook msg_hook;
	void *msg_hook_ctx;
	const char *mclk_held;
	bool smc_silent;	/* a message timed out with SMC_RESP_0 still 0 */
};

/* ---- logging and memory ---- */

#define SI_ERROR(rdev, ...) rdn_log((rdev)->card->os, RDN_LOG_ERROR, "si_dpm: " __VA_ARGS__)
#define SI_DEBUG(rdev, ...) rdn_log((rdev)->card->os, RDN_LOG_DEBUG, "si_dpm: " __VA_ARGS__)
#define SI_INFO(rdev, ...) rdn_log((rdev)->card->os, RDN_LOG_INFO, "si_dpm: " __VA_ARGS__)

static void *si_zalloc(struct rdn_si_dpm *rdev, size_t size)
{
	return rdev->card->os->alloc(rdev->card->os->cookie, size);	/* zeroed */
}

static void si_free(struct rdn_si_dpm *rdev, void *p)
{
	if (p)
		rdev->card->os->free(rdev->card->os->cookie, p);
}

/* ---- register access: Linux's one-argument macros over `rdev` ---- */

#define RREG32(reg)		rdn_rreg(rdev->card, (reg))
#define WREG32(reg, v)		rdn_wreg(rdev->card, (reg), (v))
#define WREG32_P(reg, val, mask)					\
	do {								\
		u32 tmp_ = rdn_rreg(rdev->card, (reg));			\
		tmp_ &= (mask);						\
		tmp_ |= ((val) & ~(mask));				\
		rdn_wreg(rdev->card, (reg), tmp_);			\
	} while (0)

/*
 * tn_smc_rreg()/wreg(): the SMC's space through index/data pair 0. The
 * index is read back before the data is read: on the T4240 a data read
 * straight after the index write has been seen to return the previous
 * address's word (the Linux reference session's SRAM reader, which does
 * not read back, dumped the whole SRAM one dword late).
 */
static u32 si_smc_rreg(struct rdn_si_dpm *rdev, u32 reg)
{
	WREG32(SMC_IND_INDEX_0, reg);
	(void)RREG32(SMC_IND_INDEX_0);
	return RREG32(SMC_IND_DATA_0);
}

static void si_smc_wreg(struct rdn_si_dpm *rdev, u32 reg, u32 v)
{
	WREG32(SMC_IND_INDEX_0, reg);
	WREG32(SMC_IND_DATA_0, v);
}

#define RREG32_SMC(reg)		si_smc_rreg(rdev, (reg))
#define WREG32_SMC(reg, v)	si_smc_wreg(rdev, (reg), (v))

/* r600d.h: the PCIe port index/data pair */
#ifndef PCIE_INDEX
#define PCIE_INDEX			0x0038
#define PCIE_DATA			0x003C
#endif

/* r600_pciep_rreg(): the PCIe port registers (Linux masks the index to 0xff on SI) */
static u32 si_pciep_rreg(struct rdn_si_dpm *rdev, u32 reg)
{
	WREG32(PCIE_INDEX, reg & 0xff);
	(void)RREG32(PCIE_INDEX);
	return RREG32(PCIE_DATA);
}

#define RREG32_PCIE_PORT(reg)	si_pciep_rreg(rdev, (reg))



static inline void si_udelay(struct rdn_si_dpm *rdev, u32 us)
{
	rdev->card->os->delay_us(rdev->card->os->cookie, us);
}

#define udelay(us) si_udelay(rdev, (us))

/* ---- 64-bit arithmetic without a runtime library (the kext links none) ---- */

static u64 si_div64_u64(u64 n, u64 d)
{
	u64 q = 0, r = 0;
	int i;

	if (!d)
		return 0;
	if (!(d >> 32) && !(n >> 32))
		return (u32)n / (u32)d;
	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) {
			r -= d;
			q |= (u64)1 << i;
		}
	}
	return q;
}

static s64 si_div64_s64(s64 n, s64 d)
{
	bool neg = (n < 0) != (d < 0);
	u64 q = si_div64_u64(n < 0 ? (u64)-n : (u64)n, d < 0 ? (u64)-d : (u64)d);

	return neg ? -(s64)q : (s64)q;
}

#define div64_u64(n, d) si_div64_u64((n), (d))
#define div64_s64(n, d) si_div64_s64((n), (d))
/* do_div(n, base): n becomes the quotient (the remainder is never used here) */
#define do_div(n, base) ((n) = si_div64_u64((n), (u64)(base)))

static inline unsigned int hweight32(u32 w)
{
	unsigned int n = 0;

	while (w) {
		n += w & 1;
		w >>= 1;
	}
	return n;
}

/* ---- drm_fixed.h: 32.32 fixed point (the CAC leakage formulas) ---- */

#define DRM_FIXED_POINT		32
#define DRM_FIXED_ONE		(1ULL << DRM_FIXED_POINT)

static inline s64 drm_int2fixp(int a)
{
	return ((s64)a) << DRM_FIXED_POINT;
}

static inline int drm_fixp2int(s64 a)
{
	return (int)(((s64)a) >> DRM_FIXED_POINT);
}

static inline unsigned drm_fixp_msbset(s64 a)
{
	unsigned shift, sign = (a >> 63) & 1;

	for (shift = 62; shift > 0; --shift)
		if (((a >> shift) & 1) != sign)
			return shift;

	return 0;
}

static inline s64 drm_fixp_mul(s64 a, s64 b)
{
	unsigned shift = drm_fixp_msbset(a) + drm_fixp_msbset(b);
	s64 result;

	if (shift > 61) {
		shift = shift - 61;
		a >>= (shift >> 1) + (shift & 1);
		b >>= shift >> 1;
	} else
		shift = 0;

	result = a * b;

	if (shift > DRM_FIXED_POINT)
		return result << (shift - DRM_FIXED_POINT);

	if (shift < DRM_FIXED_POINT)
		return result >> (DRM_FIXED_POINT - shift);

	return result;
}

static inline s64 drm_fixp_div(s64 a, s64 b)
{
	unsigned shift = 62 - drm_fixp_msbset(a);
	s64 result;

	a <<= shift;

	if (shift < DRM_FIXED_POINT)
		b >>= (DRM_FIXED_POINT - shift);

	result = div64_s64(a, b);

	if (shift > DRM_FIXED_POINT)
		return result >> (shift - DRM_FIXED_POINT);

	return result;
}

static inline s64 drm_fixp_exp(s64 x)
{
	s64 tolerance = div64_s64(DRM_FIXED_ONE, 1000000);
	s64 sum = DRM_FIXED_ONE, term, y = x;
	u64 count = 1;

	if (x < 0)
		y = -1 * x;

	term = y;

	while (term >= tolerance) {
		sum = sum + term;
		count = count + 1;
		term = drm_fixp_mul(term, div64_s64(y, count));
	}

	if (x < 0)
		sum = drm_fixp_div(DRM_FIXED_ONE, sum);

	return sum;
}

/* ---- si_smc.c ---- */

static int si_set_smc_sram_address(struct rdn_si_dpm *rdev,
				   u32 smc_address, u32 limit)
{
	if (smc_address & 3)
		return -EINVAL;
	if ((smc_address + 3) > limit)
		return -EINVAL;

	WREG32(SMC_IND_INDEX_0, smc_address);
	WREG32_P(SMC_IND_ACCESS_CNTL, 0, ~AUTO_INCREMENT_IND_0);

	return 0;
}

static int si_copy_bytes_to_smc(struct rdn_si_dpm *rdev,
				u32 smc_start_address,
				const u8 *src, u32 byte_count, u32 limit)
{
	int ret = 0;
	u32 data, original_data, addr, extra_shift;

	if (smc_start_address & 3)
		return -EINVAL;
	if ((smc_start_address + byte_count) > limit)
		return -EINVAL;

	addr = smc_start_address;

	while (byte_count >= 4) {
		/* SMC address space is BE */
		data = ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3];

		ret = si_set_smc_sram_address(rdev, addr, limit);
		if (ret)
			goto done;

		WREG32(SMC_IND_DATA_0, data);

		src += 4;
		byte_count -= 4;
		addr += 4;
	}

	/* RMW for the final bytes */
	if (byte_count > 0) {
		data = 0;

		ret = si_set_smc_sram_address(rdev, addr, limit);
		if (ret)
			goto done;

		(void)RREG32(SMC_IND_INDEX_0);	/* the index has landed (si_smc_rreg) */
		original_data = RREG32(SMC_IND_DATA_0);

		extra_shift = 8 * (4 - byte_count);

		while (byte_count > 0) {
			/* SMC address space is BE */
			data = (data << 8) + *src++;
			byte_count--;
		}

		data <<= extra_shift;

		data |= (original_data & ~((~0UL) << extra_shift));

		ret = si_set_smc_sram_address(rdev, addr, limit);
		if (ret)
			goto done;

		WREG32(SMC_IND_DATA_0, data);
	}

done:
	return ret;
}

static void si_start_smc(struct rdn_si_dpm *rdev)
{
	u32 tmp = RREG32_SMC(SMC_SYSCON_RESET_CNTL);

	tmp &= ~RST_REG;

	WREG32_SMC(SMC_SYSCON_RESET_CNTL, tmp);
}

static void si_reset_smc(struct rdn_si_dpm *rdev)
{
	u32 tmp;

	RREG32(CB_CGTT_SCLK_CTRL);
	RREG32(CB_CGTT_SCLK_CTRL);
	RREG32(CB_CGTT_SCLK_CTRL);
	RREG32(CB_CGTT_SCLK_CTRL);

	tmp = RREG32_SMC(SMC_SYSCON_RESET_CNTL);
	tmp |= RST_REG;
	WREG32_SMC(SMC_SYSCON_RESET_CNTL, tmp);
}

static int si_program_jump_on_start(struct rdn_si_dpm *rdev)
{
	static const u8 data[] = { 0x0E, 0x00, 0x40, 0x40 };

	return si_copy_bytes_to_smc(rdev, 0x0, data, 4, sizeof(data)+1);
}

static void si_stop_smc_clock(struct rdn_si_dpm *rdev)
{
	u32 tmp = RREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0);

	tmp |= CK_DISABLE;

	WREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0, tmp);
}

static void si_start_smc_clock(struct rdn_si_dpm *rdev)
{
	u32 tmp = RREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0);

	tmp &= ~CK_DISABLE;

	WREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0, tmp);
}

static bool si_is_smc_running(struct rdn_si_dpm *rdev)
{
	u32 rst = RREG32_SMC(SMC_SYSCON_RESET_CNTL);
	u32 clk = RREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0);

	if (!(rst & RST_REG) && !(clk & CK_DISABLE))
		return true;

	return false;
}

static PPSMC_Result si_send_msg_to_smc(struct rdn_si_dpm *rdev, PPSMC_Msg msg)
{
	u32 tmp;
	int i, limit;

	if (!si_is_smc_running(rdev))
		return PPSMC_Result_Failed;

	/* Firebrand: the caller's hook sees every message and samples while it is pending */
	limit = SI_USEC_TIMEOUT;
	if (rdev->msg_hook)
		rdev->msg_hook(rdev->msg_hook_ctx, msg, 0, 0);

	WREG32(SMC_MESSAGE_0, msg);

	for (i = 0; i < limit; i++) {
		tmp = RREG32(SMC_RESP_0);
		if (tmp != 0)
			break;
		if (rdev->msg_hook && !(i & 63))
			rdev->msg_hook(rdev->msg_hook_ctx, msg, 1, (u32)i);
		udelay(1);
	}
	tmp = RREG32(SMC_RESP_0);
	if (rdev->msg_hook)
		rdev->msg_hook(rdev->msg_hook_ctx, msg, 2, tmp);

	if (tmp != PPSMC_Result_OK)
		SI_DEBUG(rdev, "SMC message 0x%x answered 0x%x", (unsigned)msg, (unsigned)tmp);
	if (tmp == 0)
		rdev->smc_silent = true;

	return (PPSMC_Result)tmp;
}

static PPSMC_Result si_wait_for_smc_inactive(struct rdn_si_dpm *rdev)
{
	u32 tmp;
	int i;

	if (!si_is_smc_running(rdev))
		return PPSMC_Result_OK;

	for (i = 0; i < SI_USEC_TIMEOUT; i++) {
		tmp = RREG32_SMC(SMC_SYSCON_CLOCK_CNTL_0);
		if ((tmp & CKEN) == 0)
			break;
		udelay(1);
	}

	return PPSMC_Result_OK;
}

/*
 * si_load_smc_ucode(): the image comes in as an argument (Linux's
 * rdev->smc_fw). The v1-headered files (verde_k_smc.bin and the other
 * lower-case names) carry their start address and size; the raw ones
 * (VERDE_smc.bin) load at the per-chip address from radeon_ucode.h.
 */
#define SMC_FW_HEADER_SIZE_BYTES	0x00	/* common_firmware_header.size_bytes */
#define SMC_FW_UCODE_SIZE_BYTES		0x14
#define SMC_FW_UCODE_ARRAY_OFFSET	0x18
#define SMC_FW_UCODE_START_ADDR		0x20
#define SI_SMC_UCODE_START		0x10000

static int si_load_smc_ucode(struct rdn_si_dpm *rdev, const void *fw, size_t fw_size, u32 limit)
{
	u32 ucode_start_address;
	u32 ucode_size;
	const u8 *src;
	u32 data;

	if (!fw || fw_size < 0x24)
		return -EINVAL;

	if (le32_at(fw, SMC_FW_HEADER_SIZE_BYTES) == fw_size) {
		u32 off = le32_at(fw, SMC_FW_UCODE_ARRAY_OFFSET);

		ucode_start_address = le32_at(fw, SMC_FW_UCODE_START_ADDR);
		ucode_size = le32_at(fw, SMC_FW_UCODE_SIZE_BYTES);
		if (off > fw_size || ucode_size > fw_size - off)
			return -EINVAL;
		src = (const u8 *)fw + off;
		SI_INFO(rdev, "SMC firmware v1 header: version 0x%08x, %u bytes at 0x%x",
			(unsigned)le32_at(fw, 0x10), (unsigned)ucode_size, (unsigned)ucode_start_address);
	} else {
		/* radeon_ucode.h: TAHITI/PITCAIRN/VERDE/OLAND/HAINAN_SMC_UCODE_START are all 0x10000 */
		ucode_start_address = SI_SMC_UCODE_START;
		ucode_size = (u32)fw_size;
		src = (const u8 *)fw;
	}

	if (ucode_size & 3)
		return -EINVAL;
	if (ucode_start_address + ucode_size > limit)
		return -EINVAL;

	WREG32(SMC_IND_INDEX_0, ucode_start_address);
	WREG32_P(SMC_IND_ACCESS_CNTL, AUTO_INCREMENT_IND_0, ~AUTO_INCREMENT_IND_0);
	while (ucode_size >= 4) {
		/* SMC address space is BE */
		data = ((u32)src[0] << 24) | ((u32)src[1] << 16) | ((u32)src[2] << 8) | src[3];

		WREG32(SMC_IND_DATA_0, data);

		src += 4;
		ucode_size -= 4;
	}
	WREG32_P(SMC_IND_ACCESS_CNTL, 0, ~AUTO_INCREMENT_IND_0);

	return 0;
}

static int si_read_smc_sram_dword(struct rdn_si_dpm *rdev, u32 smc_address,
				  u32 *value, u32 limit)
{
	int ret;

	ret = si_set_smc_sram_address(rdev, smc_address, limit);
	if (ret == 0) {
		(void)RREG32(SMC_IND_INDEX_0);	/* the index has landed (si_smc_rreg) */
		*value = RREG32(SMC_IND_DATA_0);
	}

	return ret;
}

static int si_write_smc_sram_dword(struct rdn_si_dpm *rdev, u32 smc_address,
				   u32 value, u32 limit)
{
	int ret;

	ret = si_set_smc_sram_address(rdev, smc_address, limit);
	if (ret == 0)
		WREG32(SMC_IND_DATA_0, value);

	return ret;
}

/* ---- si.c ---- */

/* si_get_xclk() */
static u32 si_get_xclk(struct rdn_si_dpm *rdev)
{
	u32 reference_clock = rdev->ci.reference_freq;
	u32 tmp;

	tmp = RREG32(CG_CLKPIN_CNTL_2);
	if (tmp & MUX_TCLK_TO_XCLK)
		return TCLK;

	tmp = RREG32(CG_CLKPIN_CNTL);
	if (tmp & XTALIN_DIVIDE)
		return reference_clock / 4;

	return reference_clock;
}

/* si_get_temp(): millidegrees */
static int si_get_temp(struct rdn_si_dpm *rdev)
{
	u32 temp;
	int actual_temp = 0;

	temp = (RREG32(CG_MULT_THERMAL_STATUS) & CTF_TEMP_MASK) >>
		CTF_TEMP_SHIFT;

	if (temp & 0x200)
		actual_temp = 255;
	else
		actual_temp = temp & 0x1ff;

	actual_temp = (actual_temp * 1000);

	return actual_temp;
}

/* r600_get_pcie_lanes() for a discrete PCIe card */
static int r600_get_pcie_lanes(struct rdn_si_dpm *rdev)
{
	u32 link_width_cntl;

	link_width_cntl = RREG32_PCIE_PORT(RADEON_PCIE_LC_LINK_WIDTH_CNTL);

	switch ((link_width_cntl & RADEON_PCIE_LC_LINK_WIDTH_RD_MASK) >> RADEON_PCIE_LC_LINK_WIDTH_RD_SHIFT) {
	case RADEON_PCIE_LC_LINK_WIDTH_X1:
		return 1;
	case RADEON_PCIE_LC_LINK_WIDTH_X2:
		return 2;
	case RADEON_PCIE_LC_LINK_WIDTH_X4:
		return 4;
	case RADEON_PCIE_LC_LINK_WIDTH_X8:
		return 8;
	case RADEON_PCIE_LC_LINK_WIDTH_X12:
		/* not actually supported */
		return 12;
	case RADEON_PCIE_LC_LINK_WIDTH_X0:
	case RADEON_PCIE_LC_LINK_WIDTH_X16:
	default:
		return 16;
	}
}

#define radeon_get_pcie_lanes(rdev) r600_get_pcie_lanes(rdev)

/*
 * r600_set_pcie_lanes() retrains the link to another width. Not done here:
 * Linux reaches it only when two states' PCIe-width caps differ, and a
 * width change on this host's root port is untested. Logged instead.
 */
static void radeon_set_pcie_lanes(struct rdn_si_dpm *rdev, int lanes)
{
	SI_INFO(rdev, "a state asks for a x%d link; the width is left as it is", lanes);
}

/* ---- ni_dpm.c, rv770_dpm.c, btc_dpm.c, r600_dpm.c ---- */

static struct si_power_info *si_get_pi(struct rdn_si_dpm *rdev)
{
	return rdev->priv;
}

static struct ni_power_info *ni_get_pi(struct rdn_si_dpm *rdev)
{
	return &rdev->priv->ni;
}

static struct evergreen_power_info *evergreen_get_pi(struct rdn_si_dpm *rdev)
{
	return &rdev->priv->ni.eg;
}

static struct rv7xx_power_info *rv770_get_pi(struct rdn_si_dpm *rdev)
{
	return &rdev->priv->ni.eg.rv7xx;
}

static struct ni_ps *ni_get_ps(struct si_rps *rps)
{
	return rps->ps_priv;
}

static void ni_update_current_ps(struct rdn_si_dpm *rdev,
				 struct si_rps *rps)
{
	struct ni_ps *new_ps = ni_get_ps(rps);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);

	eg_pi->current_rps = *rps;
	ni_pi->current_ps = *new_ps;
	eg_pi->current_rps.ps_priv = &ni_pi->current_ps;
	rdev->current_ps = &eg_pi->current_rps;	/* radeon_pm.c: rdev->pm.dpm.current_ps */
}

static void ni_update_requested_ps(struct rdn_si_dpm *rdev,
				   struct si_rps *rps)
{
	struct ni_ps *new_ps = ni_get_ps(rps);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);

	eg_pi->requested_rps = *rps;
	ni_pi->requested_ps = *new_ps;
	eg_pi->requested_rps.ps_priv = &ni_pi->requested_ps;
}

/*
 * ni_set_uvd_clock_before/after_set_eng_clock(): Linux calls
 * radeon_set_uvd_clocks() (si_set_uvd_clocks) here when the state's UVD
 * clocks differ. UVD on Southern Islands is not brought up in this driver
 * yet, so the request is logged and the clocks are left alone.
 */
static void radeon_set_uvd_clocks(struct rdn_si_dpm *rdev, u32 vclk, u32 dclk)
{
	SI_DEBUG(rdev, "UVD clocks %u/%u requested; not driven", (unsigned)vclk, (unsigned)dclk);
}

static void ni_set_uvd_clock_before_set_eng_clock(struct rdn_si_dpm *rdev,
						  struct si_rps *new_ps,
						  struct si_rps *old_ps)
{
	struct ni_ps *new_state = ni_get_ps(new_ps);
	struct ni_ps *current_state = ni_get_ps(old_ps);

	if ((new_ps->vclk == old_ps->vclk) &&
	    (new_ps->dclk == old_ps->dclk))
		return;

	if (new_state->performance_levels[new_state->performance_level_count - 1].sclk >=
	    current_state->performance_levels[current_state->performance_level_count - 1].sclk)
		return;

	radeon_set_uvd_clocks(rdev, new_ps->vclk, new_ps->dclk);
}

static void ni_set_uvd_clock_after_set_eng_clock(struct rdn_si_dpm *rdev,
						 struct si_rps *new_ps,
						 struct si_rps *old_ps)
{
	struct ni_ps *new_state = ni_get_ps(new_ps);
	struct ni_ps *current_state = ni_get_ps(old_ps);

	if ((new_ps->vclk == old_ps->vclk) &&
	    (new_ps->dclk == old_ps->dclk))
		return;

	if (new_state->performance_levels[new_state->performance_level_count - 1].sclk <
	    current_state->performance_levels[current_state->performance_level_count - 1].sclk)
		return;

	radeon_set_uvd_clocks(rdev, new_ps->vclk, new_ps->dclk);
}

/* r600_dpm_get_vblank_time(): from the display configuration the caller gave */
static u32 r600_dpm_get_vblank_time(struct rdn_si_dpm *rdev)
{
	if (!rdev->new_active_crtcs)
		return 0xffffffff; /* if the displays are off, vblank time is max */
	return rdev->display.vblank_time_us;
}

static bool ni_dpm_vblank_too_short(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	u32 vblank_time = r600_dpm_get_vblank_time(rdev);
	/* we never hit the non-gddr5 limit so disable it */
	u32 switch_limit = pi->mem_gddr5 ? 450 : 0;

	if (vblank_time < switch_limit)
		return true;
	else
		return false;

}

/*
 * ni_copy_and_switch_arb_sets() as ni_dpm.c has it, with the switch request
 * at the end (MC_CG_CONFIG, MC_ARB_CG); rdn_ci_tables.c's copy lacks those
 * two writes and is left as the Sea Islands port verified it.
 */
static int si_copy_and_switch_arb_sets(struct rdn_si_dpm *rdev,
				       u32 arb_freq_src, u32 arb_freq_dest)
{
	u32 mc_arb_dram_timing;
	u32 mc_arb_dram_timing2;
	u32 burst_time;
	u32 mc_cg_config;

	switch (arb_freq_src) {
	case MC_CG_ARB_FREQ_F0:
		mc_arb_dram_timing  = RREG32(MC_ARB_DRAM_TIMING);
		mc_arb_dram_timing2 = RREG32(MC_ARB_DRAM_TIMING2);
		burst_time = (RREG32(MC_ARB_BURST_TIME) & STATE0_MASK) >> STATE0_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F1:
		mc_arb_dram_timing  = RREG32(MC_ARB_DRAM_TIMING_1);
		mc_arb_dram_timing2 = RREG32(MC_ARB_DRAM_TIMING2_1);
		burst_time = (RREG32(MC_ARB_BURST_TIME) & STATE1_MASK) >> STATE1_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F2:
		mc_arb_dram_timing  = RREG32(MC_ARB_DRAM_TIMING_2);
		mc_arb_dram_timing2 = RREG32(MC_ARB_DRAM_TIMING2_2);
		burst_time = (RREG32(MC_ARB_BURST_TIME) & STATE2_MASK) >> STATE2_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F3:
		mc_arb_dram_timing  = RREG32(MC_ARB_DRAM_TIMING_3);
		mc_arb_dram_timing2 = RREG32(MC_ARB_DRAM_TIMING2_3);
		burst_time = (RREG32(MC_ARB_BURST_TIME) & STATE3_MASK) >> STATE3_SHIFT;
		break;
	default:
		return -EINVAL;
	}

	switch (arb_freq_dest) {
	case MC_CG_ARB_FREQ_F0:
		WREG32(MC_ARB_DRAM_TIMING, mc_arb_dram_timing);
		WREG32(MC_ARB_DRAM_TIMING2, mc_arb_dram_timing2);
		WREG32_P(MC_ARB_BURST_TIME, STATE0(burst_time), ~STATE0_MASK);
		break;
	case MC_CG_ARB_FREQ_F1:
		WREG32(MC_ARB_DRAM_TIMING_1, mc_arb_dram_timing);
		WREG32(MC_ARB_DRAM_TIMING2_1, mc_arb_dram_timing2);
		WREG32_P(MC_ARB_BURST_TIME, STATE1(burst_time), ~STATE1_MASK);
		break;
	case MC_CG_ARB_FREQ_F2:
		WREG32(MC_ARB_DRAM_TIMING_2, mc_arb_dram_timing);
		WREG32(MC_ARB_DRAM_TIMING2_2, mc_arb_dram_timing2);
		WREG32_P(MC_ARB_BURST_TIME, STATE2(burst_time), ~STATE2_MASK);
		break;
	case MC_CG_ARB_FREQ_F3:
		WREG32(MC_ARB_DRAM_TIMING_3, mc_arb_dram_timing);
		WREG32(MC_ARB_DRAM_TIMING2_3, mc_arb_dram_timing2);
		WREG32_P(MC_ARB_BURST_TIME, STATE3(burst_time), ~STATE3_MASK);
		break;
	default:
		return -EINVAL;
	}

	mc_cg_config = RREG32(MC_CG_CONFIG) | 0x0000000F;
	WREG32(MC_CG_CONFIG, mc_cg_config);
	WREG32_P(MC_ARB_CG, CG_ARB_REQ(arb_freq_dest), ~CG_ARB_REQ_MASK);

	return 0;
}

#define ni_copy_and_switch_arb_sets(rdev, src, dest) si_copy_and_switch_arb_sets((rdev), (src), (dest))

static void rv770_get_memory_type(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	u32 tmp;

	tmp = RREG32(MC_SEQ_MISC0);

	if (((tmp & MC_SEQ_MISC0_GDDR5_MASK) >> MC_SEQ_MISC0_GDDR5_SHIFT) ==
	    MC_SEQ_MISC0_GDDR5_VALUE)
		pi->mem_gddr5 = true;
	else
		pi->mem_gddr5 = false;
}

static void btc_get_max_clock_from_voltage_dependency_table(struct rdn_ci_dep_table *table,
							    u32 *max_clock)
{
	u32 i, clock = 0;

	if ((table == NULL) || (table->count == 0)) {
		*max_clock = clock;
		return;
	}

	for (i = 0; i < table->count; i++) {
		if (clock < table->entries[i].clk)
			clock = table->entries[i].clk;
	}
	*max_clock = clock;
}

static void btc_apply_voltage_dependency_rules(struct rdn_ci_dep_table *table,
					       u32 clock, u16 max_voltage, u16 *voltage)
{
	u32 i;

	if ((table == NULL) || (table->count == 0))
		return;

	for (i = 0; i < table->count; i++) {
		if (clock <= table->entries[i].clk) {
			if (*voltage < table->entries[i].v)
				*voltage = (u16)((table->entries[i].v < max_voltage) ?
						  table->entries[i].v : max_voltage);
			return;
		}
	}

	*voltage = (*voltage > max_voltage) ? *voltage : max_voltage;
}

static u16 btc_find_voltage(struct atom_voltage_table *table, u16 voltage)
{
	unsigned int i;

	for (i = 0; i < table->count; i++) {
		if (voltage <= table->entries[i].value)
			return table->entries[i].value;
	}

	return table->entries[table->count - 1].value;
}

static void btc_apply_voltage_delta_rules(struct rdn_si_dpm *rdev,
					  u16 max_vddc, u16 max_vddci,
					  u16 *vddc, u16 *vddci)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	u16 new_voltage;

	if ((0 == *vddc) || (0 == *vddci))
		return;

	if (*vddc > *vddci) {
		if ((*vddc - *vddci) > rdev->ci.dyn_state.vddc_vddci_delta) {
			new_voltage = btc_find_voltage(&eg_pi->vddci_voltage_table,
						       (*vddc - rdev->ci.dyn_state.vddc_vddci_delta));
			*vddci = (new_voltage < max_vddci) ? new_voltage : max_vddci;
		}
	} else {
		if ((*vddci - *vddc) > rdev->ci.dyn_state.vddc_vddci_delta) {
			new_voltage = btc_find_voltage(&eg_pi->vddc_voltage_table,
						       (*vddci - rdev->ci.dyn_state.vddc_vddci_delta));
			*vddc = (new_voltage < max_vddc) ? new_voltage : max_vddc;
		}
	}
}

/* btc_get_valid_sclk/mclk(): Linux's valid_sclk/mclk_values lists are empty on SI */
static u32 btc_find_valid_clock(u32 max_clock, u32 requested_clock)
{
	return (requested_clock < max_clock) ? requested_clock : max_clock;
}

static void btc_adjust_clock_combinations(struct rdn_si_dpm *rdev,
					  const struct rdn_ci_clock_and_voltage_limits *max_limits,
					  struct rv7xx_pl *pl)
{

	if ((pl->mclk == 0) || (pl->sclk == 0))
		return;

	if (pl->mclk == pl->sclk)
		return;

	if (pl->mclk > pl->sclk) {
		if (((pl->mclk + (pl->sclk - 1)) / pl->sclk) > rdev->ci.dyn_state.mclk_sclk_ratio)
			pl->sclk = btc_find_valid_clock(max_limits->sclk,
							(pl->mclk +
							 (rdev->ci.dyn_state.mclk_sclk_ratio - 1)) /
							rdev->ci.dyn_state.mclk_sclk_ratio);
	} else {
		if ((pl->sclk - pl->mclk) > rdev->ci.dyn_state.sclk_mclk_delta)
			pl->mclk = btc_find_valid_clock(max_limits->mclk,
							pl->sclk -
							rdev->ci.dyn_state.sclk_mclk_delta);
	}
}

static void r600_calculate_u_and_p(u32 i, u32 r_c, u32 p_b,
				   u32 *p, u32 *u)
{
	u32 b_c = 0;
	u32 i_c;
	u32 tmp;

	i_c = (i * r_c) / 100;
	tmp = i_c >> p_b;

	while (tmp) {
		b_c++;
		tmp >>= 1;
	}

	*u = (b_c + 1) / 2;
	*p = i_c / (1 << (2 * (*u)));
}

static int r600_calculate_at(u32 t, u32 h, u32 fh, u32 fl, u32 *tl, u32 *th)
{
	u32 k, a, ah, al;
	u32 t1;

	if ((fl == 0) || (fh == 0) || (fl > fh))
		return -EINVAL;

	k = (100 * fh) / fl;
	t1 = (t * (k - 100));
	a = (1000 * (100 * h + t1)) / (10000 + (t1 / 100));
	a = (a + 5) / 10;
	ah = ((a * t) + 5000) / 10000;
	al = a - ah;

	*th = t - ah;
	*tl = t + al;

	return 0;
}

static bool r600_is_uvd_state(u32 classification, u32 classification2)
{
	if (classification & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
		return true;
	if (classification & ATOM_PPLIB_CLASSIFICATION_HD2STATE)
		return true;
	if (classification & ATOM_PPLIB_CLASSIFICATION_HDSTATE)
		return true;
	if (classification & ATOM_PPLIB_CLASSIFICATION_SDSTATE)
		return true;
	if (classification2 & ATOM_PPLIB_CLASSIFICATION2_MVC)
		return true;
	return false;
}

/* ---- radeon_atombios.c: the Southern Islands revisions ---- */

static int si_atom_exec(struct rdn_si_dpm *rdev, int index, union atom_ps *ps, unsigned bytes)
{
	int r = atom_execute_table(rdev->card->atom.ctx, index, ps->w, (int)bytes);

	if (r)
		SI_ERROR(rdev, "AtomBIOS command table %d failed (%d)", index, r);
	return r;
}

/*
 * radeon_atom_get_clock_dividers() for the engine PLL on Southern Islands:
 * ComputeMemoryEnginePLL 1.5 (Verde measured 2026-11-11; 1.3 is the same
 * layout), COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3. SI computes its memory
 * PLL with ComputeMemoryClockParam instead (Linux returns -EINVAL here for
 * a memory clock), and so does this.
 */
static int si_atom_get_clock_dividers(struct rdn_si_dpm *rdev, u8 clock_type, u32 clock,
				      bool strobe_mode, struct atom_clock_dividers *dividers)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL);
	u8 frev = 0, crev = 0, cntl;
	int r;

	(void)strobe_mode;
	memset(&args, 0, sizeof(args));
	memset(dividers, 0, sizeof(struct atom_clock_dividers));

	if (!atom_parse_cmd_header(rdev->card->atom.ctx, index, &frev, &crev))
		return -EINVAL;

	switch (crev) {
	case 3:
	case 5:
		if (clock_type != COMPUTE_ENGINE_PLL_PARAM)
			return -EINVAL;
		/* ulClockParams: the clock in 10 kHz in bits 23:0, the type in 31:24 */
		put_le32(args.b, 0, ((u32)clock_type << 24) | clock);

		r = si_atom_exec(rdev, index, &args, sizeof(COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V5));
		if (r)
			return r;

		cntl = u8_at(args.b, offsetof(COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3, ucCntlFlag));
		dividers->post_div = u8_at(args.b, offsetof(COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3, ucPostDiv));
		dividers->enable_post_div = (cntl & ATOM_PLL_CNTL_FLAG_PLL_POST_DIV_EN) ? true : false;
		dividers->enable_dithen = (cntl & ATOM_PLL_CNTL_FLAG_FRACTION_DISABLE) ? false : true;
		/* ulFbDiv (ATOM_S_MPLL_FB_DIVIDER) shares offset 0 with the input in an anonymous union */
		dividers->whole_fb_div = le16_at(args.b, offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDiv));
		dividers->frac_fb_div = le16_at(args.b, offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDivFrac));
		dividers->ref_div = u8_at(args.b, offsetof(COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3, ucRefDiv));
		dividers->vco_mode = (cntl & ATOM_PLL_CNTL_FLAG_MPLL_VCO_MODE) ? 1 : 0;
		break;
	default:
		SI_ERROR(rdev, "ComputeMemoryEnginePLL is revision %u.%u; only 1.3 and 1.5 (Southern Islands) are handled",
			 frev, crev);
		return -EINVAL;
	}
	return 0;
}

#define radeon_atom_get_clock_dividers(rdev, t, c, s, d) si_atom_get_clock_dividers((rdev), (t), (c), (s), (d))

/*
 * radeon_atom_get_max_vddc(): SetVoltage 1.3, ATOM_GET_VOLTAGE_LEVEL, the
 * real voltage of a voltage id (a leakage id 0xff0x among them). Linux
 * leaves the parameter space uninitialised beyond the fields it sets; it
 * is zeroed here.
 */
static int radeon_atom_get_max_vddc(struct rdn_si_dpm *rdev, u8 voltage_type,
				    u16 voltage_id, u16 *voltage)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, SetVoltage);
	u8 frev = 0, crev = 0;
	int r;

	if (!atom_parse_cmd_header(rdev->card->atom.ctx, index, &frev, &crev))
		return -EINVAL;

	memset(&args, 0, sizeof(args));
	switch (crev) {
	case 1:
		return -EINVAL;
	case 2:
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V2, ucVoltageType)] = SET_VOLTAGE_GET_MAX_VOLTAGE;
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V2, ucVoltageMode)] = 0;
		put_le16(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V2, usVoltageLevel), 0);

		r = si_atom_exec(rdev, index, &args, sizeof(SET_VOLTAGE_PARAMETERS_V2));
		if (r)
			return r;

		*voltage = le16_at(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V2, usVoltageLevel));
		break;
	case 3:
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V1_3, ucVoltageType)] = voltage_type;
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V1_3, ucVoltageMode)] = ATOM_GET_VOLTAGE_LEVEL;
		put_le16(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V1_3, usVoltageLevel), voltage_id);

		r = si_atom_exec(rdev, index, &args, sizeof(SET_VOLTAGE_PARAMETERS_V1_3));
		if (r)
			return r;

		*voltage = le16_at(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V1_3, usVoltageLevel));
		break;
	default:
		SI_ERROR(rdev, "Unknown table version %d, %d", frev, crev);
		return -EINVAL;
	}

	return 0;
}

static int radeon_atom_get_leakage_vddc_based_on_leakage_idx(struct rdn_si_dpm *rdev,
							      u16 *voltage,
							      u16 leakage_idx)
{
	return radeon_atom_get_max_vddc(rdev, VOLTAGE_TYPE_VDDC, leakage_idx, voltage);
}

static void rv770_get_max_vddc(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	u16 vddc;

	if (radeon_atom_get_max_vddc(rdev, 0, 0, &vddc))
		pi->max_vddc = 0;
	else
		pi->max_vddc = vddc;
}

/* The start of a data table in the VBIOS image, or NULL. */
static const u8 *si_data_table(struct rdn_si_dpm *rdev, int index, u16 *size, u8 *frev, u8 *crev)
{
	uint16_t data_offset = 0;

	if (!atom_parse_data_header(rdev->card->atom.ctx, index, size, frev, crev, &data_offset))
		return NULL;
	return (const u8 *)rdev->card->atom.ctx->bios + data_offset;
}

/* radeon_atom_get_svi2_info(): VoltageObjectInfo 3.1's SVID2 object for this rail */
static int radeon_atom_get_svi2_info(struct rdn_si_dpm *rdev, u8 voltage_type,
				     u8 *svd_gpio_id, u8 *svc_gpio_id)
{
	int index = GetIndexIntoMasterTable(DATA, VoltageObjectInfo);
	u8 frev = 0, crev = 0;
	u16 size = 0;
	const u8 *v3;
	u32 offset;

	v3 = si_data_table(rdev, index, &size, &frev, &crev);
	if (!v3)
		return 0;
	if (frev != 3 || crev != 1) {
		SI_ERROR(rdev, "unknown voltage object table");
		return -EINVAL;
	}
	/* atom_lookup_voltage_object_v3() */
	size = le16_at(v3, offsetof(ATOM_COMMON_TABLE_HEADER, usStructureSize));
	offset = offsetof(ATOM_VOLTAGE_OBJECT_INFO_V3_1, asVoltageObj);
	while (offset < size) {
		const u8 *vo = v3 + offset;
		u16 vo_size = le16_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, usSize));

		if ((u8_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageType)) == voltage_type) &&
		    (u8_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageMode)) == VOLTAGE_OBJ_SVID2)) {
			*svd_gpio_id = u8_at(vo, offsetof(ATOM_SVID2_VOLTAGE_OBJECT_V3, ucSVDGpioId));
			*svc_gpio_id = u8_at(vo, offsetof(ATOM_SVID2_VOLTAGE_OBJECT_V3, ucSVCGpioId));
			return 0;
		}
		if (vo_size == 0)
			break;
		offset += vo_size;
	}
	return -EINVAL;
}

/* radeon_atombios_get_default_voltages(): FirmwareInfo's boot voltages */
static void radeon_atombios_get_default_voltages(struct rdn_si_dpm *rdev,
						 u16 *vddc, u16 *vddci, u16 *mvdd)
{
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	u8 frev = 0, crev = 0;
	const u8 *firmware_info;

	*vddc = 0;
	*vddci = 0;
	*mvdd = 0;

	firmware_info = si_data_table(rdev, index, NULL, &frev, &crev);
	if (firmware_info) {
		*vddc = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V1_4, usBootUpVDDCVoltage));
		if ((frev == 2) && (crev >= 2)) {
			*vddci = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, usBootUpVDDCIVoltage));
			*mvdd = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, usBootUpMVDDCVoltage));
		}
	}
}

/*
 * radeon_atombios_get_clock_info()'s default clocks: rdev->clock.default_sclk,
 * default_mclk and current_dispclk (the DCE6 rules: 600 MHz when the VBIOS
 * says nothing or less than DisplayPort needs). current_dispclk feeds the
 * display-clock voltage rule of si_apply_state_adjust_rules(); on the R7 450
 * it lifts the two lowest levels from 825/850 mV to 900, which the GPIO
 * table rounds to 950 (measured against Linux's SRAM, 2026-11-11).
 */
static void si_get_default_clocks(struct rdn_si_dpm *rdev)
{
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	const u8 *firmware_info;
	u8 frev = 0, crev = 0;
	u16 size = 0;
	u32 dispclk = 0;

	firmware_info = si_data_table(rdev, index, &size, &frev, &crev);
	if (firmware_info) {
		rdev->default_sclk = le32_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO, ulDefaultEngineClock));
		rdev->default_mclk = le32_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO, ulDefaultMemoryClock));
		if (size >= offsetof(ATOM_FIRMWARE_INFO_V2_1, ulDefaultDispEngineClkFreq) + 4)
			dispclk = le32_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_1, ulDefaultDispEngineClkFreq));
	}
	if (dispclk == 0)
		dispclk = 60000; /* 600 Mhz */
	/* set a reasonable default for DP */
	if (dispclk < 53900)
		dispclk = 60000;
	rdev->current_dispclk = dispclk;
}

/* rv770_get_engine_memory_ss() */
static void rv770_get_engine_memory_ss(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct radeon_atom_ss ss;

	pi->sclk_ss = radeon_atombios_get_asic_ss_info(&rdev->ci, &ss,
						       ASIC_INTERNAL_ENGINE_SS, 0);
	pi->mclk_ss = radeon_atombios_get_asic_ss_info(&rdev->ci, &ss,
						       ASIC_INTERNAL_MEMORY_SS, 0);

	if (pi->sclk_ss || pi->mclk_ss)
		pi->dynamic_ss = true;
	else
		pi->dynamic_ss = false;
}

/* ---- si_dpm.c ---- */

static const struct si_cac_config_reg cac_weights_tahiti[] = {
	{ 0x0, 0x0000ffff, 0, 0xc, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x101, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0xc, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x8fc, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x95, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x34e, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x1a1, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0xda, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x46, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x208, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0xe7, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x948, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x167, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x31, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x18e, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg lcac_tahiti[] = {
	{ 0x143, 0x0001fffe, 1, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x0001fffe, 1, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x149, 0x0001fffe, 1, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0x149, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14c, 0x0001fffe, 1, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0x14c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x9e, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x9e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x101, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x101, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10a, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10d, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x92, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x92, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x95, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x95, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x155, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x155, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x158, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x158, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x116, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x116, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x119, 0x0001fffe, 1, 0x8, SISLANDS_CACCONFIG_CGIND },
	{ 0x119, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x122, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x122, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x125, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x125, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x128, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x128, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x12b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x12b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }

};

static const struct si_cac_config_reg cac_override_tahiti[] = {
	{ 0xFFFFFFFF }
};

static const struct si_powertune_data powertune_data_tahiti = {
	((1 << 16) | 27027),
	6,
	0,
	4,
	95,
	{
		0UL,
		0UL,
		4521550UL,
		309631529UL,
		-1270850L,
		4513710L,
		40
	},
	595000000UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static const struct si_dte_data dte_data_tahiti = {
	{ 1159409, 0, 0, 0, 0 },
	{ 777, 0, 0, 0, 0 },
	2,
	54000,
	127000,
	25,
	2,
	10,
	13,
	{ 27, 31, 35, 39, 43, 47, 54, 61, 67, 74, 81, 88, 95, 0, 0, 0 },
	{ 240888759, 221057860, 235370597, 162287531, 158510299, 131423027, 116673180, 103067515, 87941937, 76209048, 68209175, 64090048, 58301890, 0, 0, 0 },
	{ 12024, 11189, 11451, 8411, 7939, 6666, 5681, 4905, 4241, 3720, 3354, 3122, 2890, 0, 0, 0 },
	85,
	false
};

static const struct si_dte_data dte_data_tahiti_pro = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x7D0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_new_zealand = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0 },
	{ 0x29B, 0x3E9, 0x537, 0x7D2, 0 },
	0x5,
	0xAFC8,
	0x69,
	0x32,
	1,
	0,
	0x10,
	{ 0x82, 0xA0, 0xB4, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE, 0xFE },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0xDAC, 0x1388, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685, 0x685 },
	85,
	true
};

static const struct si_dte_data dte_data_aruba_pro = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x1000, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_malta = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static struct si_cac_config_reg cac_weights_pitcairn[] = {
	{ 0x0, 0x0000ffff, 0, 0x8a, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x24d, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x19, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x76, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0xc11, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x7f3, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x403, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x367, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x4c9, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x45d, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x36d, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x534, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x5da, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x880, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0x201, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x9f, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x1f, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5de, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x7b, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x13, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0xf9, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x66, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x13, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x186, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg lcac_pitcairn[] = {
	{ 0x98, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x9e, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x9e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10a, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x116, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x116, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x155, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x155, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x92, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x92, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x149, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x149, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x101, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x101, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10d, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x119, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x119, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x158, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x158, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x95, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x95, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x14c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x122, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x122, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x125, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x125, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x128, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x128, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x12b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x12b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_override_pitcairn[] = {
	{ 0xFFFFFFFF }
};

static const struct si_powertune_data powertune_data_pitcairn = {
	((1 << 16) | 27027),
	5,
	0,
	6,
	100,
	{
		51600000UL,
		1800000UL,
		7194395UL,
		309631529UL,
		-1270850L,
		4513710L,
		100
	},
	117830498UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static const struct si_dte_data dte_data_pitcairn = {
	{ 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0 },
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	0,
	false
};

static const struct si_dte_data dte_data_curacao_xt = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x1D17, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_curacao_pro = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x1D17, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_neptune_xt = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	45000,
	100,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0x3A2F, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_cac_config_reg cac_weights_chelsea_pro[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x2BD, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_chelsea_xt[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x30A, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_heathrow[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x362, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_cape_verde_pro[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x315, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_cape_verde[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x3BA, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg lcac_cape_verde[] = {
	{ 0x98, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x9b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x107, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x113, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x0001fffe, 1, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x152, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x146, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_override_cape_verde[] = {
	{ 0xFFFFFFFF }
};

static const struct si_powertune_data powertune_data_cape_verde = {
	((1 << 16) | 0x6993),
	5,
	0,
	7,
	105,
	{
		0UL,
		0UL,
		7194395UL,
		309631529UL,
		-1270850L,
		4513710L,
		100
	},
	117830498UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static const struct si_dte_data dte_data_cape_verde = {
	{ 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0 },
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	0,
	false
};

static const struct si_dte_data dte_data_venus_xtx = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x71C, 0xAAB, 0xE39, 0x11C7, 0x0 },
	5,
	55000,
	0x69,
	0xA,
	1,
	0,
	0x3,
	{ 0x96, 0xB4, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x895440, 0x3D0900, 0x989680, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0xD6D8, 0x88B8, 0x1555, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_venus_xt = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0xBDA, 0x11C7, 0x17B4, 0x1DA1, 0x0 },
	5,
	55000,
	0x69,
	0xA,
	1,
	0,
	0x3,
	{ 0x96, 0xB4, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x895440, 0x3D0900, 0x989680, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0xAFC8, 0x88B8, 0x238E, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_venus_pro = {
	{  0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x11C7, 0x1AAB, 0x238E, 0x2C72, 0x0 },
	5,
	55000,
	0x69,
	0xA,
	1,
	0,
	0x3,
	{ 0x96, 0xB4, 0xFF, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x895440, 0x3D0900, 0x989680, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	{ 0x88B8, 0x88B8, 0x3555, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static struct si_cac_config_reg cac_weights_oland[] = {
	{ 0x0, 0x0000ffff, 0, 0x82, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x153, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x52, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x4F, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x135, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0xAC, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x118, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0xBE, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x110, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x4CD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x37, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x27, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0xC3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x35, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0x28, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x26C, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3B2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x99D, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA3F, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0xA, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x34, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x3BA, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x30, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7A, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x100, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_mars_pro[] = {
	{ 0x0, 0x0000ffff, 0, 0x43, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0xAF, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x2A, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x59, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x1A5, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x1D6, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x2A3, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x8FD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x76, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x8A, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0xA3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x71, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0xA6, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x81, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3D2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x27C, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA96, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0xB, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x32, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7E, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x280, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x3C, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x203, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0xB4, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_mars_xt[] = {
	{ 0x0, 0x0000ffff, 0, 0x43, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0xAF, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x2A, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x59, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x1A5, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x1D6, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x2A3, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x8FD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x76, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x8A, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0xA3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x71, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0xA6, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x81, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3D2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x27C, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA96, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0xB, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x60, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x32, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7E, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x280, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x3C, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x203, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0xB4, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_oland_pro[] = {
	{ 0x0, 0x0000ffff, 0, 0x43, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0xAF, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x2A, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x59, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x1A5, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x1D6, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x2A3, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x8FD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x76, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x8A, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0xA3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x71, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0xA6, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x81, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3D2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x27C, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA96, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0xB, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x90, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x32, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7E, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x280, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x3C, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x203, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0xB4, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_weights_oland_xt[] = {
	{ 0x0, 0x0000ffff, 0, 0x43, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0xAF, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x2A, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x29, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0xA0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x59, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x1A5, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x1D6, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0x2A3, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x8FD, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x76, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x8A, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0xA3, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x71, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0xA6, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x81, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0x3D2, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0x27C, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xA96, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x5, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0xB, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x15, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x36, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x10, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x120, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x32, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x7E, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0x280, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0x7, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0x3C, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0x203, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0xB4, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg lcac_oland[] = {
	{ 0x98, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x0001fffe, 1, 0x4, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg lcac_mars_pro[] = {
	{ 0x98, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x98, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x104, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x110, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x14f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x0001fffe, 1, 0x6, SISLANDS_CACCONFIG_CGIND },
	{ 0x8c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x143, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x11f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x164, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x167, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16a, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15e, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x161, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x15b, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x0001fffe, 1, 0x2, SISLANDS_CACCONFIG_CGIND },
	{ 0x16d, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x170, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x173, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x176, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x179, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17c, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x0001fffe, 1, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0x17f, 0x00000001, 0, 0x1, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_cac_config_reg cac_override_oland[] = {
	{ 0xFFFFFFFF }
};

static const struct si_powertune_data powertune_data_oland = {
	((1 << 16) | 0x6993),
	5,
	0,
	7,
	105,
	{
		0UL,
		0UL,
		7194395UL,
		309631529UL,
		-1270850L,
		4513710L,
		100
	},
	117830498UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static const struct si_powertune_data powertune_data_mars_pro = {
	((1 << 16) | 0x6993),
	5,
	0,
	7,
	105,
	{
		0UL,
		0UL,
		7194395UL,
		309631529UL,
		-1270850L,
		4513710L,
		100
	},
	117830498UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static const struct si_dte_data dte_data_oland = {
	{ 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0 },
	0,
	0,
	0,
	0,
	0,
	0,
	0,
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	0,
	false
};

static const struct si_dte_data dte_data_mars_pro = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	55000,
	105,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0xF627, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};

static const struct si_dte_data dte_data_sun_xt = {
	{ 0x1E8480, 0x3D0900, 0x989680, 0x2625A00, 0x0 },
	{ 0x0, 0x0, 0x0, 0x0, 0x0 },
	5,
	55000,
	105,
	0xA,
	1,
	0,
	0x10,
	{ 0x96, 0xB4, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF },
	{ 0x895440, 0x3D0900, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680, 0x989680 },
	{ 0xD555, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 },
	90,
	true
};


static const struct si_cac_config_reg cac_weights_hainan[] = {
	{ 0x0, 0x0000ffff, 0, 0x2d9, SISLANDS_CACCONFIG_CGIND },
	{ 0x0, 0xffff0000, 16, 0x22b, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0x0000ffff, 0, 0x21c, SISLANDS_CACCONFIG_CGIND },
	{ 0x1, 0xffff0000, 16, 0x1dc, SISLANDS_CACCONFIG_CGIND },
	{ 0x2, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0x0000ffff, 0, 0x24e, SISLANDS_CACCONFIG_CGIND },
	{ 0x3, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0x0000ffff, 0, 0x35e, SISLANDS_CACCONFIG_CGIND },
	{ 0x5, 0xffff0000, 16, 0x1143, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0x0000ffff, 0, 0xe17, SISLANDS_CACCONFIG_CGIND },
	{ 0x6, 0xffff0000, 16, 0x441, SISLANDS_CACCONFIG_CGIND },
	{ 0x18f, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0x0000ffff, 0, 0x28b, SISLANDS_CACCONFIG_CGIND },
	{ 0x7, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x8, 0xffff0000, 16, 0xabe, SISLANDS_CACCONFIG_CGIND },
	{ 0x9, 0x0000ffff, 0, 0xf11, SISLANDS_CACCONFIG_CGIND },
	{ 0xa, 0x0000ffff, 0, 0x907, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0x0000ffff, 0, 0xb45, SISLANDS_CACCONFIG_CGIND },
	{ 0xb, 0xffff0000, 16, 0xd1e, SISLANDS_CACCONFIG_CGIND },
	{ 0xc, 0x0000ffff, 0, 0xa2c, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0x0000ffff, 0, 0x62, SISLANDS_CACCONFIG_CGIND },
	{ 0xd, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0xe, 0x0000ffff, 0, 0x1f3, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0x0000ffff, 0, 0x42, SISLANDS_CACCONFIG_CGIND },
	{ 0xf, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x10, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0x0000ffff, 0, 0x709, SISLANDS_CACCONFIG_CGIND },
	{ 0x11, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x12, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x13, 0xffff0000, 16, 0x3a, SISLANDS_CACCONFIG_CGIND },
	{ 0x14, 0x0000ffff, 0, 0x357, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0x0000ffff, 0, 0x9f, SISLANDS_CACCONFIG_CGIND },
	{ 0x15, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x4e, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0x0000ffff, 0, 0x314, SISLANDS_CACCONFIG_CGIND },
	{ 0x16, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x17, 0x0000ffff, 0, 0x6d, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x18, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0x0000ffff, 0, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x19, 0xffff0000, 16, 0x0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1a, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1b, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1c, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1d, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1e, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x1f, 0xffff0000, 16, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x20, 0x0000ffff, 0, 0, SISLANDS_CACCONFIG_CGIND },
	{ 0x6d, 0x0000ffff, 0, 0x1b9, SISLANDS_CACCONFIG_CGIND },
	{ 0xFFFFFFFF }
};

static const struct si_powertune_data powertune_data_hainan = {
	((1 << 16) | 0x6993),
	5,
	0,
	9,
	105,
	{
		0UL,
		0UL,
		7194395UL,
		309631529UL,
		-1270850L,
		4513710L,
		100
	},
	117830498UL,
	12,
	{
		0,
		0,
		0,
		0,
		0,
		0,
		0,
		0
	},
	true
};

static int si_populate_voltage_value(struct rdn_si_dpm *rdev,
				     const struct atom_voltage_table *table,
				     u16 value, SISLANDS_SMC_VOLTAGE_VALUE *voltage);
static int si_get_std_voltage_value(struct rdn_si_dpm *rdev,
				    SISLANDS_SMC_VOLTAGE_VALUE *voltage,
				    u16 *std_voltage);
static int si_write_smc_soft_register(struct rdn_si_dpm *rdev,
				      u16 reg_offset, u32 value);
static int si_convert_power_level_to_smc(struct rdn_si_dpm *rdev,
					 struct rv7xx_pl *pl,
					 SISLANDS_SMC_HW_PERFORMANCE_LEVEL *level);
static int si_calculate_sclk_params(struct rdn_si_dpm *rdev,
				    u32 engine_clock,
				    SISLANDS_SMC_SCLK_VALUE *sclk);

static void si_thermal_start_smc_fan_control(struct rdn_si_dpm *rdev);
static void si_fan_ctrl_set_default_mode(struct rdn_si_dpm *rdev);


static void si_calculate_leakage_for_v_and_t_formula(const struct ni_leakage_coeffients *coeff,
						     u16 v, s32 t, u32 ileakage, u32 *leakage)
{
	s64 kt, kv, leakage_w, i_leakage, vddc;
	s64 temperature, t_slope, t_intercept, av, bv, t_ref;
	s64 tmp;

	i_leakage = div64_s64(drm_int2fixp(ileakage), 100);
	vddc = div64_s64(drm_int2fixp(v), 1000);
	temperature = div64_s64(drm_int2fixp(t), 1000);

	t_slope = div64_s64(drm_int2fixp(coeff->t_slope), 100000000);
	t_intercept = div64_s64(drm_int2fixp(coeff->t_intercept), 100000000);
	av = div64_s64(drm_int2fixp(coeff->av), 100000000);
	bv = div64_s64(drm_int2fixp(coeff->bv), 100000000);
	t_ref = drm_int2fixp(coeff->t_ref);

	tmp = drm_fixp_mul(t_slope, vddc) + t_intercept;
	kt = drm_fixp_exp(drm_fixp_mul(tmp, temperature));
	kt = drm_fixp_div(kt, drm_fixp_exp(drm_fixp_mul(tmp, t_ref)));
	kv = drm_fixp_mul(av, drm_fixp_exp(drm_fixp_mul(bv, vddc)));

	leakage_w = drm_fixp_mul(drm_fixp_mul(drm_fixp_mul(i_leakage, kt), kv), vddc);

	*leakage = drm_fixp2int(leakage_w * 1000);
}

static void si_calculate_leakage_for_v_and_t(struct rdn_si_dpm *rdev,
					     const struct ni_leakage_coeffients *coeff,
					     u16 v,
					     s32 t,
					     u32 i_leakage,
					     u32 *leakage)
{
	si_calculate_leakage_for_v_and_t_formula(coeff, v, t, i_leakage, leakage);
}

static void si_calculate_leakage_for_v_formula(const struct ni_leakage_coeffients *coeff,
					       const u32 fixed_kt, u16 v,
					       u32 ileakage, u32 *leakage)
{
	s64 kt, kv, leakage_w, i_leakage, vddc;

	i_leakage = div64_s64(drm_int2fixp(ileakage), 100);
	vddc = div64_s64(drm_int2fixp(v), 1000);

	kt = div64_s64(drm_int2fixp(fixed_kt), 100000000);
	kv = drm_fixp_mul(div64_s64(drm_int2fixp(coeff->av), 100000000),
			  drm_fixp_exp(drm_fixp_mul(div64_s64(drm_int2fixp(coeff->bv), 100000000), vddc)));

	leakage_w = drm_fixp_mul(drm_fixp_mul(drm_fixp_mul(i_leakage, kt), kv), vddc);

	*leakage = drm_fixp2int(leakage_w * 1000);
}

static void si_calculate_leakage_for_v(struct rdn_si_dpm *rdev,
				       const struct ni_leakage_coeffients *coeff,
				       const u32 fixed_kt,
				       u16 v,
				       u32 i_leakage,
				       u32 *leakage)
{
	si_calculate_leakage_for_v_formula(coeff, fixed_kt, v, i_leakage, leakage);
}


static void si_update_dte_from_pl2(struct rdn_si_dpm *rdev,
				   struct si_dte_data *dte_data)
{
	u32 p_limit1 = rdev->ci.tdp_limit;
	u32 p_limit2 = rdev->ci.near_tdp_limit;
	u32 k = dte_data->k;
	u32 t_max = dte_data->max_t;
	u32 t_split[5] = { 10, 15, 20, 25, 30 };
	u32 t_0 = dte_data->t0;
	u32 i;

	if (p_limit2 != 0 && p_limit2 <= p_limit1) {
		dte_data->tdep_count = 3;

		for (i = 0; i < k; i++) {
			dte_data->r[i] =
				(t_split[i] * (t_max - t_0/(u32)1000) * (1 << 14)) /
				(p_limit2  * (u32)100);
		}

		dte_data->tdep_r[1] = dte_data->r[4] * 2;

		for (i = 2; i < SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE; i++) {
			dte_data->tdep_r[i] = dte_data->r[4];
		}
	} else {
		SI_ERROR(rdev, "Invalid PL2! DTE will not be updated.\n");
	}
}

static void si_initialize_powertune_defaults(struct rdn_si_dpm *rdev)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	bool update_dte_from_pl2 = false;

	if (rdev->family == CHIP_TAHITI) {
		si_pi->cac_weights = cac_weights_tahiti;
		si_pi->lcac_config = lcac_tahiti;
		si_pi->cac_override = cac_override_tahiti;
		si_pi->powertune_data = &powertune_data_tahiti;
		si_pi->dte_data = dte_data_tahiti;

		switch (rdev->device) {
		case 0x6798:
			si_pi->dte_data.enable_dte_by_default = true;
			break;
		case 0x6799:
			si_pi->dte_data = dte_data_new_zealand;
			break;
		case 0x6790:
		case 0x6791:
		case 0x6792:
		case 0x679E:
			si_pi->dte_data = dte_data_aruba_pro;
			update_dte_from_pl2 = true;
			break;
		case 0x679B:
			si_pi->dte_data = dte_data_malta;
			update_dte_from_pl2 = true;
			break;
		case 0x679A:
			si_pi->dte_data = dte_data_tahiti_pro;
			update_dte_from_pl2 = true;
			break;
		default:
			if (si_pi->dte_data.enable_dte_by_default == true)
				SI_ERROR(rdev, "DTE is not enabled!\n");
			break;
		}
	} else if (rdev->family == CHIP_PITCAIRN) {
		switch (rdev->device) {
		case 0x6810:
		case 0x6818:
			si_pi->cac_weights = cac_weights_pitcairn;
			si_pi->lcac_config = lcac_pitcairn;
			si_pi->cac_override = cac_override_pitcairn;
			si_pi->powertune_data = &powertune_data_pitcairn;
			si_pi->dte_data = dte_data_curacao_xt;
			update_dte_from_pl2 = true;
			break;
		case 0x6819:
		case 0x6811:
			si_pi->cac_weights = cac_weights_pitcairn;
			si_pi->lcac_config = lcac_pitcairn;
			si_pi->cac_override = cac_override_pitcairn;
			si_pi->powertune_data = &powertune_data_pitcairn;
			si_pi->dte_data = dte_data_curacao_pro;
			update_dte_from_pl2 = true;
			break;
		case 0x6800:
		case 0x6806:
			si_pi->cac_weights = cac_weights_pitcairn;
			si_pi->lcac_config = lcac_pitcairn;
			si_pi->cac_override = cac_override_pitcairn;
			si_pi->powertune_data = &powertune_data_pitcairn;
			si_pi->dte_data = dte_data_neptune_xt;
			update_dte_from_pl2 = true;
			break;
		default:
			si_pi->cac_weights = cac_weights_pitcairn;
			si_pi->lcac_config = lcac_pitcairn;
			si_pi->cac_override = cac_override_pitcairn;
			si_pi->powertune_data = &powertune_data_pitcairn;
			si_pi->dte_data = dte_data_pitcairn;
			break;
		}
	} else if (rdev->family == CHIP_VERDE) {
		si_pi->lcac_config = lcac_cape_verde;
		si_pi->cac_override = cac_override_cape_verde;
		si_pi->powertune_data = &powertune_data_cape_verde;

		switch (rdev->device) {
		case 0x683B:
		case 0x683F:
		case 0x6829:
		case 0x6835:
			si_pi->cac_weights = cac_weights_cape_verde_pro;
			si_pi->dte_data = dte_data_cape_verde;
			break;
		case 0x682C:
			si_pi->cac_weights = cac_weights_cape_verde_pro;
			si_pi->dte_data = dte_data_sun_xt;
			update_dte_from_pl2 = true;
			break;
		case 0x6825:
		case 0x6827:
			si_pi->cac_weights = cac_weights_heathrow;
			si_pi->dte_data = dte_data_cape_verde;
			break;
		case 0x6824:
		case 0x682D:
			si_pi->cac_weights = cac_weights_chelsea_xt;
			si_pi->dte_data = dte_data_cape_verde;
			break;
		case 0x682F:
			si_pi->cac_weights = cac_weights_chelsea_pro;
			si_pi->dte_data = dte_data_cape_verde;
			break;
		case 0x6820:
			si_pi->cac_weights = cac_weights_heathrow;
			si_pi->dte_data = dte_data_venus_xtx;
			break;
		case 0x6821:
			si_pi->cac_weights = cac_weights_heathrow;
			si_pi->dte_data = dte_data_venus_xt;
			break;
		case 0x6823:
		case 0x682B:
		case 0x6822:
		case 0x682A:
			si_pi->cac_weights = cac_weights_chelsea_pro;
			si_pi->dte_data = dte_data_venus_pro;
			break;
		default:
			si_pi->cac_weights = cac_weights_cape_verde;
			si_pi->dte_data = dte_data_cape_verde;
			break;
		}
	} else if (rdev->family == CHIP_OLAND) {
		switch (rdev->device) {
		case 0x6601:
		case 0x6621:
		case 0x6603:
		case 0x6605:
			si_pi->cac_weights = cac_weights_mars_pro;
			si_pi->lcac_config = lcac_mars_pro;
			si_pi->cac_override = cac_override_oland;
			si_pi->powertune_data = &powertune_data_mars_pro;
			si_pi->dte_data = dte_data_mars_pro;
			update_dte_from_pl2 = true;
			break;
		case 0x6600:
		case 0x6606:
		case 0x6620:
		case 0x6604:
			si_pi->cac_weights = cac_weights_mars_xt;
			si_pi->lcac_config = lcac_mars_pro;
			si_pi->cac_override = cac_override_oland;
			si_pi->powertune_data = &powertune_data_mars_pro;
			si_pi->dte_data = dte_data_mars_pro;
			update_dte_from_pl2 = true;
			break;
		case 0x6611:
		case 0x6613:
		case 0x6608:
			si_pi->cac_weights = cac_weights_oland_pro;
			si_pi->lcac_config = lcac_mars_pro;
			si_pi->cac_override = cac_override_oland;
			si_pi->powertune_data = &powertune_data_mars_pro;
			si_pi->dte_data = dte_data_mars_pro;
			update_dte_from_pl2 = true;
			break;
		case 0x6610:
			si_pi->cac_weights = cac_weights_oland_xt;
			si_pi->lcac_config = lcac_mars_pro;
			si_pi->cac_override = cac_override_oland;
			si_pi->powertune_data = &powertune_data_mars_pro;
			si_pi->dte_data = dte_data_mars_pro;
			update_dte_from_pl2 = true;
			break;
		default:
			si_pi->cac_weights = cac_weights_oland;
			si_pi->lcac_config = lcac_oland;
			si_pi->cac_override = cac_override_oland;
			si_pi->powertune_data = &powertune_data_oland;
			si_pi->dte_data = dte_data_oland;
			break;
		}
	} else if (rdev->family == CHIP_HAINAN) {
		si_pi->cac_weights = cac_weights_hainan;
		si_pi->lcac_config = lcac_oland;
		si_pi->cac_override = cac_override_oland;
		si_pi->powertune_data = &powertune_data_hainan;
		si_pi->dte_data = dte_data_sun_xt;
		update_dte_from_pl2 = true;
	} else {
		SI_ERROR(rdev, "Unknown SI asic revision, failed to initialize PowerTune!\n");
		return;
	}

	ni_pi->enable_power_containment = false;
	ni_pi->enable_cac = false;
	ni_pi->enable_sq_ramping = false;
	si_pi->enable_dte = false;

	if (si_pi->powertune_data->enable_powertune_by_default) {
		ni_pi->enable_power_containment= true;
		ni_pi->enable_cac = true;
		if (si_pi->dte_data.enable_dte_by_default) {
			si_pi->enable_dte = true;
			if (update_dte_from_pl2)
				si_update_dte_from_pl2(rdev, &si_pi->dte_data);

		}
		ni_pi->enable_sq_ramping = true;
	}

	ni_pi->driver_calculate_cac_leakage = true;
	ni_pi->cac_configuration_required = true;

	if (ni_pi->cac_configuration_required) {
		ni_pi->support_cac_long_term_average = true;
		si_pi->dyn_powertune_data.l2_lta_window_size =
			si_pi->powertune_data->l2_lta_window_size_default;
		si_pi->dyn_powertune_data.lts_truncate =
			si_pi->powertune_data->lts_truncate_default;
	} else {
		ni_pi->support_cac_long_term_average = false;
		si_pi->dyn_powertune_data.l2_lta_window_size = 0;
		si_pi->dyn_powertune_data.lts_truncate = 0;
	}

	si_pi->dyn_powertune_data.disable_uvd_powertune = false;
}

static u32 si_get_smc_power_scaling_factor(struct rdn_si_dpm *rdev)
{
	return 1;
}

static u32 si_calculate_cac_wintime(struct rdn_si_dpm *rdev)
{
	u32 xclk;
	u32 wintime;
	u32 cac_window;
	u32 cac_window_size;

	xclk = si_get_xclk(rdev);

	if (xclk == 0)
		return 0;

	cac_window = RREG32(CG_CAC_CTRL) & CAC_WINDOW_MASK;
	cac_window_size = ((cac_window & 0xFFFF0000) >> 16) * (cac_window & 0x0000FFFF);

	wintime = (cac_window_size * 100) / xclk;

	return wintime;
}

static u32 si_scale_power_for_smc(u32 power_in_watts, u32 scaling_factor)
{
	return power_in_watts;
}

static int si_calculate_adjusted_tdp_limits(struct rdn_si_dpm *rdev,
					    bool adjust_polarity,
					    u32 tdp_adjustment,
					    u32 *tdp_limit,
					    u32 *near_tdp_limit)
{
	u32 adjustment_delta, max_tdp_limit;

	if (tdp_adjustment > (u32)rdev->ci.tdp_od_limit)
		return -EINVAL;

	max_tdp_limit = ((100 + 100) * rdev->ci.tdp_limit) / 100;

	if (adjust_polarity) {
		*tdp_limit = ((100 + tdp_adjustment) * rdev->ci.tdp_limit) / 100;
		*near_tdp_limit = rdev->ci.near_tdp_limit_adjusted + (*tdp_limit - rdev->ci.tdp_limit);
	} else {
		*tdp_limit = ((100 - tdp_adjustment) * rdev->ci.tdp_limit) / 100;
		adjustment_delta  = rdev->ci.tdp_limit - *tdp_limit;
		if (adjustment_delta < rdev->ci.near_tdp_limit_adjusted)
			*near_tdp_limit = rdev->ci.near_tdp_limit_adjusted - adjustment_delta;
		else
			*near_tdp_limit = 0;
	}

	if ((*tdp_limit <= 0) || (*tdp_limit > max_tdp_limit))
		return -EINVAL;
	if ((*near_tdp_limit <= 0) || (*near_tdp_limit > *tdp_limit))
		return -EINVAL;

	return 0;
}

static int si_populate_smc_tdp_limits(struct rdn_si_dpm *rdev,
				      struct si_rps *radeon_state)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (ni_pi->enable_power_containment) {
		SISLANDS_SMC_STATETABLE *smc_table = &si_pi->smc_statetable;
		PP_SIslands_PAPMParameters *papm_parm;
		struct rdn_ci_ppm_table *ppm = &rdev->ci.dyn_state.ppm_table;	/* Linux: a pointer, NULL without the table; enable_ppm is never set */
		u32 scaling_factor = si_get_smc_power_scaling_factor(rdev);
		u32 tdp_limit;
		u32 near_tdp_limit;
		int ret;

		if (scaling_factor == 0)
			return -EINVAL;

		memset(smc_table, 0, sizeof(SISLANDS_SMC_STATETABLE));

		ret = si_calculate_adjusted_tdp_limits(rdev,
						       false, /* ??? */
						       rdev->ci.tdp_adjustment,
						       &tdp_limit,
						       &near_tdp_limit);
		if (ret)
			return ret;

		smc_table->dpm2Params.TDPLimit =
			cpu_to_be32(si_scale_power_for_smc(tdp_limit, scaling_factor) * 1000);
		smc_table->dpm2Params.NearTDPLimit =
			cpu_to_be32(si_scale_power_for_smc(near_tdp_limit, scaling_factor) * 1000);
		smc_table->dpm2Params.SafePowerLimit =
			cpu_to_be32(si_scale_power_for_smc((near_tdp_limit * SISLANDS_DPM2_TDP_SAFE_LIMIT_PERCENT) / 100, scaling_factor) * 1000);

		ret = si_copy_bytes_to_smc(rdev,
					   (si_pi->state_table_start + offsetof(SISLANDS_SMC_STATETABLE, dpm2Params) +
						 offsetof(PP_SIslands_DPM2Parameters, TDPLimit)),
					   (u8 *)(&(smc_table->dpm2Params.TDPLimit)),
					   sizeof(u32) * 3,
					   si_pi->sram_end);
		if (ret)
			return ret;

		if (si_pi->enable_ppm) {
			papm_parm = &si_pi->papm_parm;
			memset(papm_parm, 0, sizeof(PP_SIslands_PAPMParameters));
			papm_parm->NearTDPLimitTherm = cpu_to_be32(ppm->dgpu_tdp);
			papm_parm->dGPU_T_Limit = cpu_to_be32(ppm->tj_max);
			papm_parm->dGPU_T_Warning = cpu_to_be32(95);
			papm_parm->dGPU_T_Hysteresis = cpu_to_be32(5);
			papm_parm->PlatformPowerLimit = 0xffffffff;
			papm_parm->NearTDPLimitPAPM = 0xffffffff;

			ret = si_copy_bytes_to_smc(rdev, si_pi->papm_cfg_table_start,
						   (u8 *)papm_parm,
						   sizeof(PP_SIslands_PAPMParameters),
						   si_pi->sram_end);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static int si_populate_smc_tdp_limits_2(struct rdn_si_dpm *rdev,
					struct si_rps *radeon_state)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (ni_pi->enable_power_containment) {
		SISLANDS_SMC_STATETABLE *smc_table = &si_pi->smc_statetable;
		u32 scaling_factor = si_get_smc_power_scaling_factor(rdev);
		int ret;

		memset(smc_table, 0, sizeof(SISLANDS_SMC_STATETABLE));

		smc_table->dpm2Params.NearTDPLimit =
			cpu_to_be32(si_scale_power_for_smc(rdev->ci.near_tdp_limit_adjusted, scaling_factor) * 1000);
		smc_table->dpm2Params.SafePowerLimit =
			cpu_to_be32(si_scale_power_for_smc((rdev->ci.near_tdp_limit_adjusted * SISLANDS_DPM2_TDP_SAFE_LIMIT_PERCENT) / 100, scaling_factor) * 1000);

		ret = si_copy_bytes_to_smc(rdev,
					   (si_pi->state_table_start +
					    offsetof(SISLANDS_SMC_STATETABLE, dpm2Params) +
					    offsetof(PP_SIslands_DPM2Parameters, NearTDPLimit)),
					   (u8 *)(&(smc_table->dpm2Params.NearTDPLimit)),
					   sizeof(u32) * 2,
					   si_pi->sram_end);
		if (ret)
			return ret;
	}

	return 0;
}

static u16 si_calculate_power_efficiency_ratio(struct rdn_si_dpm *rdev,
					       const u16 prev_std_vddc,
					       const u16 curr_std_vddc)
{
	u64 margin = (u64)SISLANDS_DPM2_PWREFFICIENCYRATIO_MARGIN;
	u64 prev_vddc = (u64)prev_std_vddc;
	u64 curr_vddc = (u64)curr_std_vddc;
	u64 pwr_efficiency_ratio, n, d;

	if ((prev_vddc == 0) || (curr_vddc == 0))
		return 0;

	n = div64_u64((u64)1024 * curr_vddc * curr_vddc * ((u64)1000 + margin), (u64)1000);
	d = prev_vddc * prev_vddc;
	pwr_efficiency_ratio = div64_u64(n, d);

	if (pwr_efficiency_ratio > (u64)0xFFFF)
		return 0;

	return (u16)pwr_efficiency_ratio;
}

static bool si_should_disable_uvd_powertune(struct rdn_si_dpm *rdev,
					    struct si_rps *radeon_state)
{
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (si_pi->dyn_powertune_data.disable_uvd_powertune &&
	    radeon_state->vclk && radeon_state->dclk)
		return true;

	return false;
}

static int si_populate_power_containment_values(struct rdn_si_dpm *rdev,
						struct si_rps *radeon_state,
						SISLANDS_SMC_SWSTATE *smc_state)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct ni_ps *state = ni_get_ps(radeon_state);
	SISLANDS_SMC_VOLTAGE_VALUE vddc;
	u32 prev_sclk;
	u32 max_sclk;
	u32 min_sclk;
	u16 prev_std_vddc;
	u16 curr_std_vddc;
	int i;
	u16 pwr_efficiency_ratio;
	u8 max_ps_percent;
	bool disable_uvd_power_tune;
	int ret;

	if (ni_pi->enable_power_containment == false)
		return 0;

	if (state->performance_level_count == 0)
		return -EINVAL;

	if (smc_state->levelCount != state->performance_level_count)
		return -EINVAL;

	disable_uvd_power_tune = si_should_disable_uvd_powertune(rdev, radeon_state);

	smc_state->levels[0].dpm2.MaxPS = 0;
	smc_state->levels[0].dpm2.NearTDPDec = 0;
	smc_state->levels[0].dpm2.AboveSafeInc = 0;
	smc_state->levels[0].dpm2.BelowSafeInc = 0;
	smc_state->levels[0].dpm2.PwrEfficiencyRatio = 0;

	for (i = 1; i < state->performance_level_count; i++) {
		prev_sclk = state->performance_levels[i-1].sclk;
		max_sclk  = state->performance_levels[i].sclk;
		if (i == 1)
			max_ps_percent = SISLANDS_DPM2_MAXPS_PERCENT_M;
		else
			max_ps_percent = SISLANDS_DPM2_MAXPS_PERCENT_H;

		if (prev_sclk > max_sclk)
			return -EINVAL;

		if ((max_ps_percent == 0) ||
		    (prev_sclk == max_sclk) ||
		    disable_uvd_power_tune) {
			min_sclk = max_sclk;
		} else if (i == 1) {
			min_sclk = prev_sclk;
		} else {
			min_sclk = (prev_sclk * (u32)max_ps_percent) / 100;
		}

		if (min_sclk < state->performance_levels[0].sclk)
			min_sclk = state->performance_levels[0].sclk;

		if (min_sclk == 0)
			return -EINVAL;

		ret = si_populate_voltage_value(rdev, &eg_pi->vddc_voltage_table,
						state->performance_levels[i-1].vddc, &vddc);
		if (ret)
			return ret;

		ret = si_get_std_voltage_value(rdev, &vddc, &prev_std_vddc);
		if (ret)
			return ret;

		ret = si_populate_voltage_value(rdev, &eg_pi->vddc_voltage_table,
						state->performance_levels[i].vddc, &vddc);
		if (ret)
			return ret;

		ret = si_get_std_voltage_value(rdev, &vddc, &curr_std_vddc);
		if (ret)
			return ret;

		pwr_efficiency_ratio = si_calculate_power_efficiency_ratio(rdev,
									   prev_std_vddc, curr_std_vddc);

		smc_state->levels[i].dpm2.MaxPS = (u8)((SISLANDS_DPM2_MAX_PULSE_SKIP * (max_sclk - min_sclk)) / max_sclk);
		smc_state->levels[i].dpm2.NearTDPDec = SISLANDS_DPM2_NEAR_TDP_DEC;
		smc_state->levels[i].dpm2.AboveSafeInc = SISLANDS_DPM2_ABOVE_SAFE_INC;
		smc_state->levels[i].dpm2.BelowSafeInc = SISLANDS_DPM2_BELOW_SAFE_INC;
		smc_state->levels[i].dpm2.PwrEfficiencyRatio = cpu_to_be16(pwr_efficiency_ratio);
	}

	return 0;
}

static int si_populate_sq_ramping_values(struct rdn_si_dpm *rdev,
					 struct si_rps *radeon_state,
					 SISLANDS_SMC_SWSTATE *smc_state)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct ni_ps *state = ni_get_ps(radeon_state);
	u32 sq_power_throttle, sq_power_throttle2;
	bool enable_sq_ramping = ni_pi->enable_sq_ramping;
	int i;

	if (state->performance_level_count == 0)
		return -EINVAL;

	if (smc_state->levelCount != state->performance_level_count)
		return -EINVAL;

	if (rdev->ci.sq_ramping_threshold == 0)
		return -EINVAL;

	if (SISLANDS_DPM2_SQ_RAMP_MAX_POWER > (MAX_POWER_MASK >> MAX_POWER_SHIFT))
		enable_sq_ramping = false;

	if (SISLANDS_DPM2_SQ_RAMP_MIN_POWER > (MIN_POWER_MASK >> MIN_POWER_SHIFT))
		enable_sq_ramping = false;

	if (SISLANDS_DPM2_SQ_RAMP_MAX_POWER_DELTA > (MAX_POWER_DELTA_MASK >> MAX_POWER_DELTA_SHIFT))
		enable_sq_ramping = false;

	if (SISLANDS_DPM2_SQ_RAMP_STI_SIZE > (STI_SIZE_MASK >> STI_SIZE_SHIFT))
		enable_sq_ramping = false;

	if (SISLANDS_DPM2_SQ_RAMP_LTI_RATIO > (LTI_RATIO_MASK >> LTI_RATIO_SHIFT))
		enable_sq_ramping = false;

	for (i = 0; i < state->performance_level_count; i++) {
		sq_power_throttle = 0;
		sq_power_throttle2 = 0;

		if ((state->performance_levels[i].sclk >= rdev->ci.sq_ramping_threshold) &&
		    enable_sq_ramping) {
			sq_power_throttle |= MAX_POWER(SISLANDS_DPM2_SQ_RAMP_MAX_POWER);
			sq_power_throttle |= MIN_POWER(SISLANDS_DPM2_SQ_RAMP_MIN_POWER);
			sq_power_throttle2 |= MAX_POWER_DELTA(SISLANDS_DPM2_SQ_RAMP_MAX_POWER_DELTA);
			sq_power_throttle2 |= STI_SIZE(SISLANDS_DPM2_SQ_RAMP_STI_SIZE);
			sq_power_throttle2 |= LTI_RATIO(SISLANDS_DPM2_SQ_RAMP_LTI_RATIO);
		} else {
			sq_power_throttle |= MAX_POWER_MASK | MIN_POWER_MASK;
			sq_power_throttle2 |= MAX_POWER_DELTA_MASK | STI_SIZE_MASK | LTI_RATIO_MASK;
		}

		smc_state->levels[i].SQPowerThrottle = cpu_to_be32(sq_power_throttle);
		smc_state->levels[i].SQPowerThrottle_2 = cpu_to_be32(sq_power_throttle2);
	}

	return 0;
}

static int si_enable_power_containment(struct rdn_si_dpm *rdev,
				       struct si_rps *radeon_new_state,
				       bool enable)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	PPSMC_Result smc_result;
	int ret = 0;

	if (ni_pi->enable_power_containment) {
		if (enable) {
			if (!si_should_disable_uvd_powertune(rdev, radeon_new_state)) {
				smc_result = si_send_msg_to_smc(rdev, PPSMC_TDPClampingActive);
				if (smc_result != PPSMC_Result_OK) {
					ret = -EINVAL;
					ni_pi->pc_enabled = false;
				} else {
					ni_pi->pc_enabled = true;
				}
			}
		} else {
			smc_result = si_send_msg_to_smc(rdev, PPSMC_TDPClampingInactive);
			if (smc_result != PPSMC_Result_OK)
				ret = -EINVAL;
			ni_pi->pc_enabled = false;
		}
	}

	return ret;
}

static int si_initialize_smc_dte_tables(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	int ret = 0;
	struct si_dte_data *dte_data = &si_pi->dte_data;
	Smc_SIslands_DTE_Configuration *dte_tables = NULL;
	u32 table_size;
	u8 tdep_count;
	u32 i;

	if (dte_data == NULL)
		si_pi->enable_dte = false;

	if (si_pi->enable_dte == false)
		return 0;

	if (dte_data->k <= 0)
		return -EINVAL;

	dte_tables = si_zalloc(rdev, sizeof(Smc_SIslands_DTE_Configuration));
	if (dte_tables == NULL) {
		si_pi->enable_dte = false;
		return -ENOMEM;
	}

	table_size = dte_data->k;

	if (table_size > SMC_SISLANDS_DTE_MAX_FILTER_STAGES)
		table_size = SMC_SISLANDS_DTE_MAX_FILTER_STAGES;

	tdep_count = dte_data->tdep_count;
	if (tdep_count > SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE)
		tdep_count = SMC_SISLANDS_DTE_MAX_TEMPERATURE_DEPENDENT_ARRAY_SIZE;

	dte_tables->K = cpu_to_be32(table_size);
	dte_tables->T0 = cpu_to_be32(dte_data->t0);
	dte_tables->MaxT = cpu_to_be32(dte_data->max_t);
	dte_tables->WindowSize = dte_data->window_size;
	dte_tables->temp_select = dte_data->temp_select;
	dte_tables->DTE_mode = dte_data->dte_mode;
	dte_tables->Tthreshold = cpu_to_be32(dte_data->t_threshold);

	if (tdep_count > 0)
		table_size--;

	for (i = 0; i < table_size; i++) {
		dte_tables->tau[i] = cpu_to_be32(dte_data->tau[i]);
		dte_tables->R[i]   = cpu_to_be32(dte_data->r[i]);
	}

	dte_tables->Tdep_count = tdep_count;

	for (i = 0; i < (u32)tdep_count; i++) {
		dte_tables->T_limits[i] = dte_data->t_limits[i];
		dte_tables->Tdep_tau[i] = cpu_to_be32(dte_data->tdep_tau[i]);
		dte_tables->Tdep_R[i] = cpu_to_be32(dte_data->tdep_r[i]);
	}

	ret = si_copy_bytes_to_smc(rdev, si_pi->dte_table_start, (u8 *)dte_tables,
				   sizeof(Smc_SIslands_DTE_Configuration), si_pi->sram_end);
	si_free(rdev, dte_tables);

	return ret;
}

static int si_get_cac_std_voltage_max_min(struct rdn_si_dpm *rdev,
					  u16 *max, u16 *min)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct rdn_ci_cac_leakage_table *table =
		&rdev->ci.dyn_state.cac_leakage_table;
	u32 i;
	u32 v0_loadline;


	if (table == NULL)
		return -EINVAL;

	*max = 0;
	*min = 0xFFFF;

	for (i = 0; i < table->count; i++) {
		if (table->entries[i].vddc > *max)
			*max = table->entries[i].vddc;
		if (table->entries[i].vddc < *min)
			*min = table->entries[i].vddc;
	}

	if (si_pi->powertune_data->lkge_lut_v0_percent > 100)
		return -EINVAL;

	v0_loadline = (*min) * (100 - si_pi->powertune_data->lkge_lut_v0_percent) / 100;

	if (v0_loadline > 0xFFFFUL)
		return -EINVAL;

	*min = (u16)v0_loadline;

	if ((*min > *max) || (*max == 0) || (*min == 0))
		return -EINVAL;

	return 0;
}

static u16 si_get_cac_std_voltage_step(u16 max, u16 min)
{
	return ((max - min) + (SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES - 1)) /
		SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES;
}

static int si_init_dte_leakage_table(struct rdn_si_dpm *rdev,
				     PP_SIslands_CacConfig *cac_tables,
				     u16 vddc_max, u16 vddc_min, u16 vddc_step,
				     u16 t0, u16 t_step)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 leakage;
	unsigned int i, j;
	s32 t;
	u32 smc_leakage;
	u32 scaling_factor;
	u16 voltage;

	scaling_factor = si_get_smc_power_scaling_factor(rdev);

	for (i = 0; i < SMC_SISLANDS_LKGE_LUT_NUM_OF_TEMP_ENTRIES ; i++) {
		t = (1000 * (i * t_step + t0));

		for (j = 0; j < SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES; j++) {
			voltage = vddc_max - (vddc_step * j);

			si_calculate_leakage_for_v_and_t(rdev,
							 &si_pi->powertune_data->leakage_coefficients,
							 voltage,
							 t,
							 si_pi->dyn_powertune_data.cac_leakage,
							 &leakage);

			smc_leakage = si_scale_power_for_smc(leakage, scaling_factor) / 4;

			if (smc_leakage > 0xFFFF)
				smc_leakage = 0xFFFF;

			cac_tables->cac_lkge_lut[i][SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES-1-j] =
				cpu_to_be16((u16)smc_leakage);
		}
	}
	return 0;
}

static int si_init_simplified_leakage_table(struct rdn_si_dpm *rdev,
					    PP_SIslands_CacConfig *cac_tables,
					    u16 vddc_max, u16 vddc_min, u16 vddc_step)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 leakage;
	unsigned int i, j;
	u32 smc_leakage;
	u32 scaling_factor;
	u16 voltage;

	scaling_factor = si_get_smc_power_scaling_factor(rdev);

	for (j = 0; j < SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES; j++) {
		voltage = vddc_max - (vddc_step * j);

		si_calculate_leakage_for_v(rdev,
					   &si_pi->powertune_data->leakage_coefficients,
					   si_pi->powertune_data->fixed_kt,
					   voltage,
					   si_pi->dyn_powertune_data.cac_leakage,
					   &leakage);

		smc_leakage = si_scale_power_for_smc(leakage, scaling_factor) / 4;

		if (smc_leakage > 0xFFFF)
			smc_leakage = 0xFFFF;

		for (i = 0; i < SMC_SISLANDS_LKGE_LUT_NUM_OF_TEMP_ENTRIES ; i++)
			cac_tables->cac_lkge_lut[i][SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES-1-j] =
				cpu_to_be16((u16)smc_leakage);
	}
	return 0;
}

static int si_initialize_smc_cac_tables(struct rdn_si_dpm *rdev)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	PP_SIslands_CacConfig *cac_tables = NULL;
	u16 vddc_max, vddc_min, vddc_step;
	u16 t0, t_step;
	u32 load_line_slope, reg;
	int ret = 0;
	u32 ticks_per_us = si_get_xclk(rdev) / 100;

	if (ni_pi->enable_cac == false)
		return 0;

	cac_tables = si_zalloc(rdev, sizeof(PP_SIslands_CacConfig));
	if (!cac_tables)
		return -ENOMEM;

	reg = RREG32(CG_CAC_CTRL) & ~CAC_WINDOW_MASK;
	reg |= CAC_WINDOW(si_pi->powertune_data->cac_window);
	WREG32(CG_CAC_CTRL, reg);

	si_pi->dyn_powertune_data.cac_leakage = rdev->ci.cac_leakage;
	si_pi->dyn_powertune_data.dc_pwr_value =
		si_pi->powertune_data->dc_cac[NISLANDS_DCCAC_LEVEL_0];
	si_pi->dyn_powertune_data.wintime = si_calculate_cac_wintime(rdev);
	si_pi->dyn_powertune_data.shift_n = si_pi->powertune_data->shift_n_default;

	si_pi->dyn_powertune_data.leakage_minimum_temperature = 80 * 1000;

	ret = si_get_cac_std_voltage_max_min(rdev, &vddc_max, &vddc_min);
	if (ret)
		goto done_free;

	vddc_step = si_get_cac_std_voltage_step(vddc_max, vddc_min);
	vddc_min = vddc_max - (vddc_step * (SMC_SISLANDS_LKGE_LUT_NUM_OF_VOLT_ENTRIES - 1));
	t_step = 4;
	t0 = 60;

	if (si_pi->enable_dte || ni_pi->driver_calculate_cac_leakage)
		ret = si_init_dte_leakage_table(rdev, cac_tables,
						vddc_max, vddc_min, vddc_step,
						t0, t_step);
	else
		ret = si_init_simplified_leakage_table(rdev, cac_tables,
						       vddc_max, vddc_min, vddc_step);
	if (ret)
		goto done_free;

	load_line_slope = ((u32)rdev->ci.load_line_slope << SMC_SISLANDS_SCALE_R) / 100;

	cac_tables->l2numWin_TDP = cpu_to_be32(si_pi->dyn_powertune_data.l2_lta_window_size);
	cac_tables->lts_truncate_n = si_pi->dyn_powertune_data.lts_truncate;
	cac_tables->SHIFT_N = si_pi->dyn_powertune_data.shift_n;
	cac_tables->lkge_lut_V0 = cpu_to_be32((u32)vddc_min);
	cac_tables->lkge_lut_Vstep = cpu_to_be32((u32)vddc_step);
	cac_tables->R_LL = cpu_to_be32(load_line_slope);
	cac_tables->WinTime = cpu_to_be32(si_pi->dyn_powertune_data.wintime);
	cac_tables->calculation_repeats = cpu_to_be32(2);
	cac_tables->dc_cac = cpu_to_be32(0);
	cac_tables->log2_PG_LKG_SCALE = 12;
	cac_tables->cac_temp = si_pi->powertune_data->operating_temp;
	cac_tables->lkge_lut_T0 = cpu_to_be32((u32)t0);
	cac_tables->lkge_lut_Tstep = cpu_to_be32((u32)t_step);

	ret = si_copy_bytes_to_smc(rdev, si_pi->cac_table_start, (u8 *)cac_tables,
				   sizeof(PP_SIslands_CacConfig), si_pi->sram_end);

	if (ret)
		goto done_free;

	ret = si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_ticks_per_us, ticks_per_us);

done_free:
	if (ret) {
		ni_pi->enable_cac = false;
		ni_pi->enable_power_containment = false;
	}

	si_free(rdev, cac_tables);

	return 0;
}

static int si_program_cac_config_registers(struct rdn_si_dpm *rdev,
					   const struct si_cac_config_reg *cac_config_regs)
{
	const struct si_cac_config_reg *config_regs = cac_config_regs;
	u32 data = 0, offset;

	if (!config_regs)
		return -EINVAL;

	while (config_regs->offset != 0xFFFFFFFF) {
		switch (config_regs->type) {
		case SISLANDS_CACCONFIG_CGIND:
			offset = SMC_CG_IND_START + config_regs->offset;
			if (offset < SMC_CG_IND_END)
				data = RREG32_SMC(offset);
			break;
		default:
			data = RREG32(config_regs->offset << 2);
			break;
		}

		data &= ~config_regs->mask;
		data |= ((config_regs->value << config_regs->shift) & config_regs->mask);

		switch (config_regs->type) {
		case SISLANDS_CACCONFIG_CGIND:
			offset = SMC_CG_IND_START + config_regs->offset;
			if (offset < SMC_CG_IND_END)
				WREG32_SMC(offset, data);
			break;
		default:
			WREG32(config_regs->offset << 2, data);
			break;
		}
		config_regs++;
	}
	return 0;
}

static int si_initialize_hardware_cac_manager(struct rdn_si_dpm *rdev)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	int ret;

	if ((ni_pi->enable_cac == false) ||
	    (ni_pi->cac_configuration_required == false))
		return 0;

	ret = si_program_cac_config_registers(rdev, si_pi->lcac_config);
	if (ret)
		return ret;
	ret = si_program_cac_config_registers(rdev, si_pi->cac_override);
	if (ret)
		return ret;
	ret = si_program_cac_config_registers(rdev, si_pi->cac_weights);
	if (ret)
		return ret;

	return 0;
}

static int si_enable_smc_cac(struct rdn_si_dpm *rdev,
			     struct si_rps *radeon_new_state,
			     bool enable)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	PPSMC_Result smc_result;
	int ret = 0;

	if (ni_pi->enable_cac) {
		if (enable) {
			if (!si_should_disable_uvd_powertune(rdev, radeon_new_state)) {
				if (ni_pi->support_cac_long_term_average) {
					smc_result = si_send_msg_to_smc(rdev, PPSMC_CACLongTermAvgEnable);
					if (smc_result != PPSMC_Result_OK)
						ni_pi->support_cac_long_term_average = false;
				}

				smc_result = si_send_msg_to_smc(rdev, PPSMC_MSG_EnableCac);
				if (smc_result != PPSMC_Result_OK) {
					ret = -EINVAL;
					ni_pi->cac_enabled = false;
				} else {
					ni_pi->cac_enabled = true;
				}

				if (si_pi->enable_dte) {
					smc_result = si_send_msg_to_smc(rdev, PPSMC_MSG_EnableDTE);
					if (smc_result != PPSMC_Result_OK)
						ret = -EINVAL;
				}
			}
		} else if (ni_pi->cac_enabled) {
			if (si_pi->enable_dte)
				smc_result = si_send_msg_to_smc(rdev, PPSMC_MSG_DisableDTE);

			smc_result = si_send_msg_to_smc(rdev, PPSMC_MSG_DisableCac);

			ni_pi->cac_enabled = false;

			if (ni_pi->support_cac_long_term_average)
				smc_result = si_send_msg_to_smc(rdev, PPSMC_CACLongTermAvgDisable);
		}
	}
	return ret;
}

static int si_init_smc_spll_table(struct rdn_si_dpm *rdev)
{
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	SMC_SISLANDS_SPLL_DIV_TABLE *spll_table;
	SISLANDS_SMC_SCLK_VALUE sclk_params;
	u32 fb_div, p_div;
	u32 clk_s, clk_v;
	u32 sclk = 0;
	int ret = 0;
	u32 tmp;
	int i;

	if (si_pi->spll_table_start == 0)
		return -EINVAL;

	spll_table = si_zalloc(rdev, sizeof(SMC_SISLANDS_SPLL_DIV_TABLE));
	if (spll_table == NULL)
		return -ENOMEM;

	for (i = 0; i < 256; i++) {
		ret = si_calculate_sclk_params(rdev, sclk, &sclk_params);
		if (ret)
			break;

		p_div = (sclk_params.vCG_SPLL_FUNC_CNTL & SPLL_PDIV_A_MASK) >> SPLL_PDIV_A_SHIFT;
		fb_div = (sclk_params.vCG_SPLL_FUNC_CNTL_3 & SPLL_FB_DIV_MASK) >> SPLL_FB_DIV_SHIFT;
		clk_s = (sclk_params.vCG_SPLL_SPREAD_SPECTRUM & CLK_S_MASK) >> CLK_S_SHIFT;
		clk_v = (sclk_params.vCG_SPLL_SPREAD_SPECTRUM_2 & CLK_V_MASK) >> CLK_V_SHIFT;

		fb_div &= ~0x00001FFF;
		fb_div >>= 1;
		clk_v >>= 6;

		if (p_div & ~(SMC_SISLANDS_SPLL_DIV_TABLE_PDIV_MASK >> SMC_SISLANDS_SPLL_DIV_TABLE_PDIV_SHIFT))
			ret = -EINVAL;
		if (fb_div & ~(SMC_SISLANDS_SPLL_DIV_TABLE_FBDIV_MASK >> SMC_SISLANDS_SPLL_DIV_TABLE_FBDIV_SHIFT))
			ret = -EINVAL;
		if (clk_s & ~(SMC_SISLANDS_SPLL_DIV_TABLE_CLKS_MASK >> SMC_SISLANDS_SPLL_DIV_TABLE_CLKS_SHIFT))
			ret = -EINVAL;
		if (clk_v & ~(SMC_SISLANDS_SPLL_DIV_TABLE_CLKV_MASK >> SMC_SISLANDS_SPLL_DIV_TABLE_CLKV_SHIFT))
			ret = -EINVAL;

		if (ret)
			break;

		tmp = ((fb_div << SMC_SISLANDS_SPLL_DIV_TABLE_FBDIV_SHIFT) & SMC_SISLANDS_SPLL_DIV_TABLE_FBDIV_MASK) |
			((p_div << SMC_SISLANDS_SPLL_DIV_TABLE_PDIV_SHIFT) & SMC_SISLANDS_SPLL_DIV_TABLE_PDIV_MASK);
		spll_table->freq[i] = cpu_to_be32(tmp);

		tmp = ((clk_v << SMC_SISLANDS_SPLL_DIV_TABLE_CLKV_SHIFT) & SMC_SISLANDS_SPLL_DIV_TABLE_CLKV_MASK) |
			((clk_s << SMC_SISLANDS_SPLL_DIV_TABLE_CLKS_SHIFT) & SMC_SISLANDS_SPLL_DIV_TABLE_CLKS_MASK);
		spll_table->ss[i] = cpu_to_be32(tmp);

		sclk += 512;
	}


	if (!ret)
		ret = si_copy_bytes_to_smc(rdev, si_pi->spll_table_start,
					   (u8 *)spll_table, sizeof(SMC_SISLANDS_SPLL_DIV_TABLE),
					   si_pi->sram_end);

	if (ret)
		ni_pi->enable_power_containment = false;

	si_free(rdev, spll_table);

	return ret;
}

static u16 si_get_lower_of_leakage_and_vce_voltage(struct rdn_si_dpm *rdev,
						   u16 vce_voltage)
{
	u16 highest_leakage = 0;
	struct si_power_info *si_pi = si_get_pi(rdev);
	int i;

	for (i = 0; i < si_pi->leakage_voltage.count; i++){
		if (highest_leakage < si_pi->leakage_voltage.entries[i].voltage)
			highest_leakage = si_pi->leakage_voltage.entries[i].voltage;
	}

	if (si_pi->leakage_voltage.count && (highest_leakage < vce_voltage))
		return highest_leakage;

	return vce_voltage;
}

static int si_get_vce_clock_voltage(struct rdn_si_dpm *rdev,
				    u32 evclk, u32 ecclk, u16 *voltage)
{
	u32 i;
	int ret = -EINVAL;
	struct rdn_ci_vce_dep_table *table =
		&rdev->ci.dyn_state.vce_clock_voltage_dependency_table;

	if (((evclk == 0) && (ecclk == 0)) ||
	    (table && (table->count == 0))) {
		*voltage = 0;
		return 0;
	}

	for (i = 0; i < table->count; i++) {
		if ((evclk <= table->entries[i].evclk) &&
		    (ecclk <= table->entries[i].ecclk)) {
			*voltage = table->entries[i].v;
			ret = 0;
			break;
		}
	}

	/* if no match return the highest voltage */
	if (ret)
		*voltage = table->entries[table->count - 1].v;

	*voltage = si_get_lower_of_leakage_and_vce_voltage(rdev, *voltage);

	return ret;
}

static void si_apply_state_adjust_rules(struct rdn_si_dpm *rdev,
					struct si_rps *rps)
{
	struct ni_ps *ps = ni_get_ps(rps);
	struct rdn_ci_clock_and_voltage_limits *max_limits;
	bool disable_mclk_switching = false;
	bool disable_sclk_switching = false;
	u32 mclk, sclk;
	u16 vddc, vddci, min_vce_voltage = 0;
	u32 max_sclk_vddc, max_mclk_vddci, max_mclk_vddc;
	u32 max_sclk = 0, max_mclk = 0;
	int i;

	if (rdev->family == CHIP_HAINAN) {
		if ((rdev->revision == 0x81) ||
		    (rdev->revision == 0xC3) ||
		    (rdev->device == 0x6660) ||
		    (rdev->device == 0x6664) ||
		    (rdev->device == 0x6665) ||
		    (rdev->device == 0x6667) ||
		    (rdev->device == 0x666F)) {
			max_sclk = 75000;
		}
		if ((rdev->revision == 0xC3) ||
		    (rdev->device == 0x6665)) {
			max_sclk = 60000;
			max_mclk = 80000;
		}
		if ((rdev->device == 0x666f) &&
		    (rdev->revision == 0x00)) {
			max_sclk = 80000;
			max_mclk = 95000;
		}
	} else if (rdev->family == CHIP_OLAND) {
		if ((rdev->revision == 0xC7) ||
		    (rdev->revision == 0x80) ||
		    (rdev->revision == 0x81) ||
		    (rdev->revision == 0x83) ||
		    (rdev->revision == 0x87) ||
		    (rdev->device == 0x6604) ||
		    (rdev->device == 0x6605)) {
			max_sclk = 75000;
		}

		if (rdev->high_pixelclock_count > 1)
			disable_sclk_switching = true;
	}

	if (rps->vce_active) {
		rps->evclk = rdev->ci.vce_states[rdev->vce_level].evclk;
		rps->ecclk = rdev->ci.vce_states[rdev->vce_level].ecclk;
		si_get_vce_clock_voltage(rdev, rps->evclk, rps->ecclk,
					 &min_vce_voltage);
	} else {
		rps->evclk = 0;
		rps->ecclk = 0;
	}

	rdev->mclk_held = NULL;
	if ((rdev->ci.new_active_crtc_count > 1) ||
	    ni_dpm_vblank_too_short(rdev)) {
		disable_mclk_switching = true;
		rdev->mclk_held = rdev->ci.new_active_crtc_count > 1 ? "more than one display" :
			"the vertical blank is shorter than a GDDR5 memory clock change (450 us)";
	}

	/*
	 * amdgpu's si_dpm.c (Linux 7.0): "We define 'high pixelclock' for SI
	 * as higher than necessary for 4K 30Hz", and with one such display
	 * the memory clock stays put ("flickering lines at the bottom edge of
	 * the screen when using a single 4K 60Hz monitor"); on Oland two of
	 * them hold the engine clock up as well.
	 */
	if (rdev->high_pixelclock_count && !(rdev->quirks & RDN_SI_DPM_RADEON_RULES)) {
		disable_mclk_switching = true;
		rdev->mclk_held = "a display above 297 MHz";
		if (rdev->high_pixelclock_count > 1 && rdev->family == CHIP_OLAND)
			disable_sclk_switching = true;
	}
	if (rdev->quirks & (RDN_SI_DPM_MCLK_FIXED_HIGH | RDN_SI_DPM_MCLK_FIXED_BOOT)) {
		disable_mclk_switching = true;
		rdev->mclk_held = (rdev->quirks & RDN_SI_DPM_MCLK_FIXED_BOOT) ?
			"asked for (at the boot clock)" : "asked for";
	}

	if (rps->vclk || rps->dclk) {
		disable_mclk_switching = true;
		disable_sclk_switching = true;
		rdev->mclk_held = "UVD is decoding";
	}

	if (rdev->ci.ac_power)
		max_limits = &rdev->ci.dyn_state.max_clock_voltage_on_ac;
	else
		max_limits = &rdev->ci.dyn_state.max_clock_voltage_on_dc;

	for (i = ps->performance_level_count - 2; i >= 0; i--) {
		if (ps->performance_levels[i].vddc > ps->performance_levels[i+1].vddc)
			ps->performance_levels[i].vddc = ps->performance_levels[i+1].vddc;
	}
	if (rdev->ci.ac_power == false) {
		for (i = 0; i < ps->performance_level_count; i++) {
			if (ps->performance_levels[i].mclk > max_limits->mclk)
				ps->performance_levels[i].mclk = max_limits->mclk;
			if (ps->performance_levels[i].sclk > max_limits->sclk)
				ps->performance_levels[i].sclk = max_limits->sclk;
			if (ps->performance_levels[i].vddc > max_limits->vddc)
				ps->performance_levels[i].vddc = max_limits->vddc;
			if (ps->performance_levels[i].vddci > max_limits->vddci)
				ps->performance_levels[i].vddci = max_limits->vddci;
		}
	}

	/* limit clocks to max supported clocks based on voltage dependency tables */
	btc_get_max_clock_from_voltage_dependency_table(&rdev->ci.dyn_state.vddc_dependency_on_sclk,
							&max_sclk_vddc);
	btc_get_max_clock_from_voltage_dependency_table(&rdev->ci.dyn_state.vddci_dependency_on_mclk,
							&max_mclk_vddci);
	btc_get_max_clock_from_voltage_dependency_table(&rdev->ci.dyn_state.vddc_dependency_on_mclk,
							&max_mclk_vddc);

	for (i = 0; i < ps->performance_level_count; i++) {
		if (max_sclk_vddc) {
			if (ps->performance_levels[i].sclk > max_sclk_vddc)
				ps->performance_levels[i].sclk = max_sclk_vddc;
		}
		if (max_mclk_vddci) {
			if (ps->performance_levels[i].mclk > max_mclk_vddci)
				ps->performance_levels[i].mclk = max_mclk_vddci;
		}
		if (max_mclk_vddc) {
			if (ps->performance_levels[i].mclk > max_mclk_vddc)
				ps->performance_levels[i].mclk = max_mclk_vddc;
		}
		if (max_mclk) {
			if (ps->performance_levels[i].mclk > max_mclk)
				ps->performance_levels[i].mclk = max_mclk;
		}
		if (max_sclk) {
			if (ps->performance_levels[i].sclk > max_sclk)
				ps->performance_levels[i].sclk = max_sclk;
		}
	}

	/* XXX validate the min clocks required for display */

	if (disable_mclk_switching) {
		mclk  = ps->performance_levels[ps->performance_level_count - 1].mclk;
		vddci = ps->performance_levels[ps->performance_level_count - 1].vddci;
	} else {
		mclk = ps->performance_levels[0].mclk;
		vddci = ps->performance_levels[0].vddci;
	}

	if (disable_sclk_switching) {
		sclk = ps->performance_levels[ps->performance_level_count - 1].sclk;
		vddc = ps->performance_levels[ps->performance_level_count - 1].vddc;
	} else {
		sclk = ps->performance_levels[0].sclk;
		vddc = ps->performance_levels[0].vddc;
	}

	if (rps->vce_active) {
		if (sclk < rdev->ci.vce_states[rdev->vce_level].sclk)
			sclk = rdev->ci.vce_states[rdev->vce_level].sclk;
		if (mclk < rdev->ci.vce_states[rdev->vce_level].mclk)
			mclk = rdev->ci.vce_states[rdev->vce_level].mclk;
	}

	/* adjusted low state */
	ps->performance_levels[0].sclk = sclk;
	ps->performance_levels[0].mclk = mclk;
	ps->performance_levels[0].vddc = vddc;
	ps->performance_levels[0].vddci = vddci;

	if (disable_sclk_switching) {
		sclk = ps->performance_levels[0].sclk;
		for (i = 1; i < ps->performance_level_count; i++) {
			if (sclk < ps->performance_levels[i].sclk)
				sclk = ps->performance_levels[i].sclk;
		}
		for (i = 0; i < ps->performance_level_count; i++) {
			ps->performance_levels[i].sclk = sclk;
			ps->performance_levels[i].vddc = vddc;
		}
	} else {
		for (i = 1; i < ps->performance_level_count; i++) {
			if (ps->performance_levels[i].sclk < ps->performance_levels[i - 1].sclk)
				ps->performance_levels[i].sclk = ps->performance_levels[i - 1].sclk;
			if (ps->performance_levels[i].vddc < ps->performance_levels[i - 1].vddc)
				ps->performance_levels[i].vddc = ps->performance_levels[i - 1].vddc;
		}
	}

	if (disable_mclk_switching) {
		mclk = ps->performance_levels[0].mclk;
		for (i = 1; i < ps->performance_level_count; i++) {
			if (mclk < ps->performance_levels[i].mclk)
				mclk = ps->performance_levels[i].mclk;
		}
		for (i = 0; i < ps->performance_level_count; i++) {
			ps->performance_levels[i].mclk = mclk;
			ps->performance_levels[i].vddci = vddci;
		}
	} else {
		for (i = 1; i < ps->performance_level_count; i++) {
			if (ps->performance_levels[i].mclk < ps->performance_levels[i - 1].mclk)
				ps->performance_levels[i].mclk = ps->performance_levels[i - 1].mclk;
			if (ps->performance_levels[i].vddci < ps->performance_levels[i - 1].vddci)
				ps->performance_levels[i].vddci = ps->performance_levels[i - 1].vddci;
		}
	}

	/*
	 * Firebrand, MCLK_FIXED_BOOT (bring-up): every level at the boot
	 * state's memory clock, so that a state switch changes the engine
	 * clock and the voltage and never the MPLL.
	 */
	if ((rdev->quirks & RDN_SI_DPM_MCLK_FIXED_BOOT) && rdev->boot_ps) {
		struct ni_ps *boot = ni_get_ps(rdev->boot_ps);

		for (i = 0; i < ps->performance_level_count; i++) {
			ps->performance_levels[i].mclk = boot->performance_levels[0].mclk;
			ps->performance_levels[i].vddci = boot->performance_levels[0].vddci;
		}
	} else {
		for (i = 0; i < ps->performance_level_count; i++)
			btc_adjust_clock_combinations(rdev, max_limits,
						      &ps->performance_levels[i]);
	}

	for (i = 0; i < ps->performance_level_count; i++) {
		if (ps->performance_levels[i].vddc < min_vce_voltage)
			ps->performance_levels[i].vddc = min_vce_voltage;
		btc_apply_voltage_dependency_rules(&rdev->ci.dyn_state.vddc_dependency_on_sclk,
						   ps->performance_levels[i].sclk,
						   max_limits->vddc,  &ps->performance_levels[i].vddc);
		btc_apply_voltage_dependency_rules(&rdev->ci.dyn_state.vddci_dependency_on_mclk,
						   ps->performance_levels[i].mclk,
						   max_limits->vddci, &ps->performance_levels[i].vddci);
		btc_apply_voltage_dependency_rules(&rdev->ci.dyn_state.vddc_dependency_on_mclk,
						   ps->performance_levels[i].mclk,
						   max_limits->vddc,  &ps->performance_levels[i].vddc);
		btc_apply_voltage_dependency_rules(&rdev->ci.dyn_state.vddc_dependency_on_dispclk,
						   rdev->current_dispclk,
						   max_limits->vddc,  &ps->performance_levels[i].vddc);
	}

	for (i = 0; i < ps->performance_level_count; i++) {
		btc_apply_voltage_delta_rules(rdev,
					      max_limits->vddc, max_limits->vddci,
					      &ps->performance_levels[i].vddc,
					      &ps->performance_levels[i].vddci);
	}

	ps->dc_compatible = true;
	for (i = 0; i < ps->performance_level_count; i++) {
		if (ps->performance_levels[i].vddc > rdev->ci.dyn_state.max_clock_voltage_on_dc.vddc)
			ps->dc_compatible = false;
	}
}

#if 0
static int si_read_smc_soft_register(struct rdn_si_dpm *rdev,
				     u16 reg_offset, u32 *value)
{
	struct si_power_info *si_pi = si_get_pi(rdev);

	return si_read_smc_sram_dword(rdev,
				      si_pi->soft_regs_start + reg_offset, value,
				      si_pi->sram_end);
}
#endif

static int si_write_smc_soft_register(struct rdn_si_dpm *rdev,
				      u16 reg_offset, u32 value)
{
	struct si_power_info *si_pi = si_get_pi(rdev);

	return si_write_smc_sram_dword(rdev,
				       si_pi->soft_regs_start + reg_offset,
				       value, si_pi->sram_end);
}

static bool si_is_special_1gb_platform(struct rdn_si_dpm *rdev)
{
	bool ret = false;
	u32 tmp, width, row, column, bank, density;
	bool is_memory_gddr5, is_special;

	tmp = RREG32(MC_SEQ_MISC0);
	is_memory_gddr5 = (MC_SEQ_MISC0_GDDR5_VALUE == ((tmp & MC_SEQ_MISC0_GDDR5_MASK) >> MC_SEQ_MISC0_GDDR5_SHIFT));
	is_special = (MC_SEQ_MISC0_REV_ID_VALUE == ((tmp & MC_SEQ_MISC0_REV_ID_MASK) >> MC_SEQ_MISC0_REV_ID_SHIFT))
		& (MC_SEQ_MISC0_VEN_ID_VALUE == ((tmp & MC_SEQ_MISC0_VEN_ID_MASK) >> MC_SEQ_MISC0_VEN_ID_SHIFT));

	WREG32(MC_SEQ_IO_DEBUG_INDEX, 0xb);
	width = ((RREG32(MC_SEQ_IO_DEBUG_DATA) >> 1) & 1) ? 16 : 32;

	tmp = RREG32(MC_ARB_RAMCFG);
	row = ((tmp & NOOFROWS_MASK) >> NOOFROWS_SHIFT) + 10;
	column = ((tmp & NOOFCOLS_MASK) >> NOOFCOLS_SHIFT) + 8;
	bank = ((tmp & NOOFBANK_MASK) >> NOOFBANK_SHIFT) + 2;

	density = (1 << (row + column - 20 + bank)) * width;

	if ((rdev->device == 0x6819) &&
	    is_memory_gddr5 && is_special && (density == 0x400))
		ret = true;

	return ret;
}

static void si_get_leakage_vddc(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u16 vddc, count = 0;
	int i, ret;

	for (i = 0; i < SISLANDS_MAX_LEAKAGE_COUNT; i++) {
		ret = radeon_atom_get_leakage_vddc_based_on_leakage_idx(rdev, &vddc, SISLANDS_LEAKAGE_INDEX0 + i);

		if (!ret && (vddc > 0) && (vddc != (SISLANDS_LEAKAGE_INDEX0 + i))) {
			si_pi->leakage_voltage.entries[count].voltage = vddc;
			si_pi->leakage_voltage.entries[count].leakage_index =
				SISLANDS_LEAKAGE_INDEX0 + i;
			count++;
		}
	}
	si_pi->leakage_voltage.count = count;
}

static int si_get_leakage_voltage_from_leakage_index(struct rdn_si_dpm *rdev,
						     u32 index, u16 *leakage_voltage)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	int i;

	if (leakage_voltage == NULL)
		return -EINVAL;

	if ((index & 0xff00) != 0xff00)
		return -EINVAL;

	if ((index & 0xff) > SISLANDS_MAX_LEAKAGE_COUNT + 1)
		return -EINVAL;

	if (index < SISLANDS_LEAKAGE_INDEX0)
		return -EINVAL;

	for (i = 0; i < si_pi->leakage_voltage.count; i++) {
		if (si_pi->leakage_voltage.entries[i].leakage_index == index) {
			*leakage_voltage = si_pi->leakage_voltage.entries[i].voltage;
			return 0;
		}
	}
	return -EAGAIN;
}

static void si_set_dpm_event_sources(struct rdn_si_dpm *rdev, u32 sources)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	bool want_thermal_protection;
	enum radeon_dpm_event_src dpm_event_src;

	switch (sources) {
	case 0:
	default:
		want_thermal_protection = false;
		break;
	case (1 << RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL):
		want_thermal_protection = true;
		dpm_event_src = RADEON_DPM_EVENT_SRC_DIGITAL;
		break;
	case (1 << RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL):
		want_thermal_protection = true;
		dpm_event_src = RADEON_DPM_EVENT_SRC_EXTERNAL;
		break;
	case ((1 << RADEON_DPM_AUTO_THROTTLE_SRC_EXTERNAL) |
	      (1 << RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL)):
		want_thermal_protection = true;
		dpm_event_src = RADEON_DPM_EVENT_SRC_DIGIAL_OR_EXTERNAL;
		break;
	}

	if (want_thermal_protection) {
		WREG32_P(CG_THERMAL_CTRL, DPM_EVENT_SRC(dpm_event_src), ~DPM_EVENT_SRC_MASK);
		if (pi->thermal_protection)
			WREG32_P(GENERAL_PWRMGT, 0, ~THERMAL_PROTECTION_DIS);
	} else {
		WREG32_P(GENERAL_PWRMGT, THERMAL_PROTECTION_DIS, ~THERMAL_PROTECTION_DIS);
	}
}

static void si_enable_auto_throttle_source(struct rdn_si_dpm *rdev,
					   enum radeon_dpm_auto_throttle_src source,
					   bool enable)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);

	if (enable) {
		if (!(pi->active_auto_throttle_sources & (1 << source))) {
			pi->active_auto_throttle_sources |= 1 << source;
			si_set_dpm_event_sources(rdev, pi->active_auto_throttle_sources);
		}
	} else {
		if (pi->active_auto_throttle_sources & (1 << source)) {
			pi->active_auto_throttle_sources &= ~(1 << source);
			si_set_dpm_event_sources(rdev, pi->active_auto_throttle_sources);
		}
	}
}

static void si_start_dpm(struct rdn_si_dpm *rdev)
{
	WREG32_P(GENERAL_PWRMGT, GLOBAL_PWRMGT_EN, ~GLOBAL_PWRMGT_EN);
}

static void si_stop_dpm(struct rdn_si_dpm *rdev)
{
	WREG32_P(GENERAL_PWRMGT, 0, ~GLOBAL_PWRMGT_EN);
}

static void si_enable_sclk_control(struct rdn_si_dpm *rdev, bool enable)
{
	if (enable)
		WREG32_P(SCLK_PWRMGT_CNTL, 0, ~SCLK_PWRMGT_OFF);
	else
		WREG32_P(SCLK_PWRMGT_CNTL, SCLK_PWRMGT_OFF, ~SCLK_PWRMGT_OFF);

}

#if 0
static int si_notify_hardware_of_thermal_state(struct rdn_si_dpm *rdev,
					       u32 thermal_level)
{
	PPSMC_Result ret;

	if (thermal_level == 0) {
		ret = si_send_msg_to_smc(rdev, PPSMC_MSG_EnableThermalInterrupt);
		if (ret == PPSMC_Result_OK)
			return 0;
		else
			return -EINVAL;
	}
	return 0;
}

static void si_notify_hardware_vpu_recovery_event(struct rdn_si_dpm *rdev)
{
	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_tdr_is_about_to_happen, true);
}
#endif

#if 0
static int si_notify_hw_of_powersource(struct rdn_si_dpm *rdev, bool ac_power)
{
	if (ac_power)
		return (si_send_msg_to_smc(rdev, PPSMC_MSG_RunningOnAC) == PPSMC_Result_OK) ?
			0 : -EINVAL;

	return 0;
}
#endif

static PPSMC_Result si_send_msg_to_smc_with_parameter(struct rdn_si_dpm *rdev,
						      PPSMC_Msg msg, u32 parameter)
{
	WREG32(SMC_SCRATCH0, parameter);
	return si_send_msg_to_smc(rdev, msg);
}

static int si_restrict_performance_levels_before_switch(struct rdn_si_dpm *rdev)
{
	if (si_send_msg_to_smc(rdev, PPSMC_MSG_NoForcedLevel) != PPSMC_Result_OK)
		return -EINVAL;

	return (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetEnabledLevels, 1) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static int si_dpm_force_performance_level(struct rdn_si_dpm *rdev,
				   enum rdn_si_forced_level level)
{
	struct si_rps *rps = rdev->current_ps;
	struct ni_ps *ps = ni_get_ps(rps);
	u32 levels = ps->performance_level_count;

	if (level == RDN_SI_FORCED_LEVEL_HIGH) {
		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetEnabledLevels, levels) != PPSMC_Result_OK)
			return -EINVAL;

		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetForcedLevels, 1) != PPSMC_Result_OK)
			return -EINVAL;
	} else if (level == RDN_SI_FORCED_LEVEL_LOW) {
		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetForcedLevels, 0) != PPSMC_Result_OK)
			return -EINVAL;

		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetEnabledLevels, 1) != PPSMC_Result_OK)
			return -EINVAL;
	} else if (level == RDN_SI_FORCED_LEVEL_AUTO) {
		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetForcedLevels, 0) != PPSMC_Result_OK)
			return -EINVAL;

		if (si_send_msg_to_smc_with_parameter(rdev, PPSMC_MSG_SetEnabledLevels, levels) != PPSMC_Result_OK)
			return -EINVAL;
	}

	rdev->ci.forced_level = (enum rdn_ci_forced_level)level;

	return 0;
}

#if 0
static int si_set_boot_state(struct rdn_si_dpm *rdev)
{
	return (si_send_msg_to_smc(rdev, PPSMC_MSG_SwitchToInitialState) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}
#endif

static int si_set_sw_state(struct rdn_si_dpm *rdev)
{
	return (si_send_msg_to_smc(rdev, PPSMC_MSG_SwitchToSwState) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static int si_halt_smc(struct rdn_si_dpm *rdev)
{
	if (si_send_msg_to_smc(rdev, PPSMC_MSG_Halt) != PPSMC_Result_OK)
		return -EINVAL;

	return (si_wait_for_smc_inactive(rdev) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static int si_resume_smc(struct rdn_si_dpm *rdev)
{
	if (si_send_msg_to_smc(rdev, PPSMC_FlushDataCache) != PPSMC_Result_OK)
		return -EINVAL;

	return (si_send_msg_to_smc(rdev, PPSMC_MSG_Resume) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static void si_dpm_start_smc(struct rdn_si_dpm *rdev)
{
	si_program_jump_on_start(rdev);
	si_start_smc(rdev);
	si_start_smc_clock(rdev);
}

static void si_dpm_stop_smc(struct rdn_si_dpm *rdev)
{
	si_reset_smc(rdev);
	si_stop_smc_clock(rdev);
}

static int si_process_firmware_header(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;
	int ret;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_stateTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->state_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_softRegisters,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->soft_regs_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_mcRegisterTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->mc_reg_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_fanTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->fan_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_mcArbDramAutoRefreshTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->arb_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_CacConfigTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->cac_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_DteConfiguration,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->dte_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_spllTable,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->spll_table_start = tmp;

	ret = si_read_smc_sram_dword(rdev,
				     SISLANDS_SMC_FIRMWARE_HEADER_LOCATION +
				     SISLANDS_SMC_FIRMWARE_HEADER_PAPMParameters,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	si_pi->papm_cfg_table_start = tmp;

	return ret;
}

static void si_read_clock_registers(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);

	si_pi->clock_registers.cg_spll_func_cntl = RREG32(CG_SPLL_FUNC_CNTL);
	si_pi->clock_registers.cg_spll_func_cntl_2 = RREG32(CG_SPLL_FUNC_CNTL_2);
	si_pi->clock_registers.cg_spll_func_cntl_3 = RREG32(CG_SPLL_FUNC_CNTL_3);
	si_pi->clock_registers.cg_spll_func_cntl_4 = RREG32(CG_SPLL_FUNC_CNTL_4);
	si_pi->clock_registers.cg_spll_spread_spectrum = RREG32(CG_SPLL_SPREAD_SPECTRUM);
	si_pi->clock_registers.cg_spll_spread_spectrum_2 = RREG32(CG_SPLL_SPREAD_SPECTRUM_2);
	si_pi->clock_registers.dll_cntl = RREG32(DLL_CNTL);
	si_pi->clock_registers.mclk_pwrmgt_cntl = RREG32(MCLK_PWRMGT_CNTL);
	si_pi->clock_registers.mpll_ad_func_cntl = RREG32(MPLL_AD_FUNC_CNTL);
	si_pi->clock_registers.mpll_dq_func_cntl = RREG32(MPLL_DQ_FUNC_CNTL);
	si_pi->clock_registers.mpll_func_cntl = RREG32(MPLL_FUNC_CNTL);
	si_pi->clock_registers.mpll_func_cntl_1 = RREG32(MPLL_FUNC_CNTL_1);
	si_pi->clock_registers.mpll_func_cntl_2 = RREG32(MPLL_FUNC_CNTL_2);
	si_pi->clock_registers.mpll_ss1 = RREG32(MPLL_SS1);
	si_pi->clock_registers.mpll_ss2 = RREG32(MPLL_SS2);
}

static void si_enable_thermal_protection(struct rdn_si_dpm *rdev,
					  bool enable)
{
	if (enable)
		WREG32_P(GENERAL_PWRMGT, 0, ~THERMAL_PROTECTION_DIS);
	else
		WREG32_P(GENERAL_PWRMGT, THERMAL_PROTECTION_DIS, ~THERMAL_PROTECTION_DIS);
}

static void si_enable_acpi_power_management(struct rdn_si_dpm *rdev)
{
	WREG32_P(GENERAL_PWRMGT, STATIC_PM_EN, ~STATIC_PM_EN);
}

#if 0
static int si_enter_ulp_state(struct rdn_si_dpm *rdev)
{
	WREG32(SMC_MESSAGE_0, PPSMC_MSG_SwitchToMinimumPower);

	udelay(25000);

	return 0;
}

static int si_exit_ulp_state(struct rdn_si_dpm *rdev)
{
	int i;

	WREG32(SMC_MESSAGE_0, PPSMC_MSG_ResumeFromMinimumPower);

	udelay(7000);

	for (i = 0; i < SI_USEC_TIMEOUT; i++) {
		if (RREG32(SMC_RESP_0) == 1)
			break;
		udelay(1000);
	}

	return 0;
}
#endif

static int si_notify_smc_display_change(struct rdn_si_dpm *rdev,
				     bool has_display)
{
	PPSMC_Msg msg = has_display ?
		PPSMC_MSG_HasDisplay : PPSMC_MSG_NoDisplay;

	return (si_send_msg_to_smc(rdev, msg) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static void si_program_response_times(struct rdn_si_dpm *rdev)
{
	u32 voltage_response_time, acpi_delay_time, vbi_time_out;
	u32 vddc_dly, acpi_dly, vbi_dly;
	u32 reference_clock;

	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_mvdd_chg_time, 1);

	voltage_response_time = (u32)rdev->ci.voltage_response_time;

	if (voltage_response_time == 0)
		voltage_response_time = 1000;

	acpi_delay_time = 15000;
	vbi_time_out = 100000;

	reference_clock = si_get_xclk(rdev);

	vddc_dly = (voltage_response_time  * reference_clock) / 100;
	acpi_dly = (acpi_delay_time * reference_clock) / 100;
	vbi_dly  = (vbi_time_out * reference_clock) / 100;

	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_delay_vreg,  vddc_dly);
	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_delay_acpi,  acpi_dly);
	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_mclk_chg_timeout, vbi_dly);
	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_mc_block_delay, 0xAA);
}

static void si_program_ds_registers(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	u32 tmp = 1; /* XXX: 0x10 on tahiti A0 */

	if (eg_pi->sclk_deep_sleep) {
		WREG32_P(MISC_CLK_CNTL, DEEP_SLEEP_CLK_SEL(tmp), ~DEEP_SLEEP_CLK_SEL_MASK);
		WREG32_P(CG_SPLL_AUTOSCALE_CNTL, AUTOSCALE_ON_SS_CLEAR,
			 ~AUTOSCALE_ON_SS_CLEAR);
	}
}

static void si_program_display_gap(struct rdn_si_dpm *rdev)
{
	u32 tmp, pipe;
	int i;

	tmp = RREG32(CG_DISPLAY_GAP_CNTL) & ~(DISP1_GAP_MASK | DISP2_GAP_MASK);
	if (rdev->ci.new_active_crtc_count > 0)
		tmp |= DISP1_GAP(R600_PM_DISPLAY_GAP_VBLANK_OR_WM);
	else
		tmp |= DISP1_GAP(R600_PM_DISPLAY_GAP_IGNORE);

	if (rdev->ci.new_active_crtc_count > 1)
		tmp |= DISP2_GAP(R600_PM_DISPLAY_GAP_VBLANK_OR_WM);
	else
		tmp |= DISP2_GAP(R600_PM_DISPLAY_GAP_IGNORE);

	WREG32(CG_DISPLAY_GAP_CNTL, tmp);

	tmp = RREG32(DCCG_DISP_SLOW_SELECT_REG);
	pipe = (tmp & DCCG_DISP1_SLOW_SELECT_MASK) >> DCCG_DISP1_SLOW_SELECT_SHIFT;

	if ((rdev->ci.new_active_crtc_count > 0) &&
	    (!(rdev->new_active_crtcs & (1 << pipe)))) {
		/* find the first active crtc */
		for (i = 0; i < rdev->num_crtc; i++) {
			if (rdev->new_active_crtcs & (1 << i))
				break;
		}
		if (i == rdev->num_crtc)
			pipe = 0;
		else
			pipe = i;

		tmp &= ~DCCG_DISP1_SLOW_SELECT_MASK;
		tmp |= DCCG_DISP1_SLOW_SELECT(pipe);
		WREG32(DCCG_DISP_SLOW_SELECT_REG, tmp);
	}

	/* Setting this to false forces the performance state to low if the crtcs are disabled.
	 * This can be a problem on PowerXpress systems or if you want to use the card
	 * for offscreen rendering or compute if there are no crtcs enabled.
	 */
	si_notify_smc_display_change(rdev, rdev->ci.new_active_crtc_count > 0);
}

static void si_enable_spread_spectrum(struct rdn_si_dpm *rdev, bool enable)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);

	if (enable) {
		if (pi->sclk_ss)
			WREG32_P(GENERAL_PWRMGT, DYN_SPREAD_SPECTRUM_EN, ~DYN_SPREAD_SPECTRUM_EN);
	} else {
		WREG32_P(CG_SPLL_SPREAD_SPECTRUM, 0, ~SSEN);
		WREG32_P(GENERAL_PWRMGT, 0, ~DYN_SPREAD_SPECTRUM_EN);
	}
}

static void si_setup_bsp(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	u32 xclk = si_get_xclk(rdev);

	r600_calculate_u_and_p(pi->asi,
			       xclk,
			       16,
			       &pi->bsp,
			       &pi->bsu);

	r600_calculate_u_and_p(pi->pasi,
			       xclk,
			       16,
			       &pi->pbsp,
			       &pi->pbsu);


	pi->dsp = BSP(pi->bsp) | BSU(pi->bsu);
	pi->psp = BSP(pi->pbsp) | BSU(pi->pbsu);

	WREG32(CG_BSP, pi->dsp);
}

static void si_program_git(struct rdn_si_dpm *rdev)
{
	WREG32_P(CG_GIT, CG_GICST(R600_GICST_DFLT), ~CG_GICST_MASK);
}

static void si_program_tp(struct rdn_si_dpm *rdev)
{
	int i;
	enum r600_td td = R600_TD_DFLT;

	for (i = 0; i < R600_PM_NUMBER_OF_TC; i++)
		WREG32(CG_FFCT_0 + (i * 4), (UTC_0(r600_utc[i]) | DTC_0(r600_dtc[i])));

	if (td == R600_TD_AUTO)
		WREG32_P(SCLK_PWRMGT_CNTL, 0, ~FIR_FORCE_TREND_SEL);
	else
		WREG32_P(SCLK_PWRMGT_CNTL, FIR_FORCE_TREND_SEL, ~FIR_FORCE_TREND_SEL);

	if (td == R600_TD_UP)
		WREG32_P(SCLK_PWRMGT_CNTL, 0, ~FIR_TREND_MODE);

	if (td == R600_TD_DOWN)
		WREG32_P(SCLK_PWRMGT_CNTL, FIR_TREND_MODE, ~FIR_TREND_MODE);
}

static void si_program_tpp(struct rdn_si_dpm *rdev)
{
	WREG32(CG_TPC, R600_TPC_DFLT);
}

static void si_program_sstp(struct rdn_si_dpm *rdev)
{
	WREG32(CG_SSP, (SSTU(R600_SSTU_DFLT) | SST(R600_SST_DFLT)));
}

static void si_enable_display_gap(struct rdn_si_dpm *rdev)
{
	u32 tmp = RREG32(CG_DISPLAY_GAP_CNTL);

	tmp &= ~(DISP1_GAP_MASK | DISP2_GAP_MASK);
	tmp |= (DISP1_GAP(R600_PM_DISPLAY_GAP_IGNORE) |
		DISP2_GAP(R600_PM_DISPLAY_GAP_IGNORE));

	tmp &= ~(DISP1_GAP_MCHG_MASK | DISP2_GAP_MCHG_MASK);
	tmp |= (DISP1_GAP_MCHG(R600_PM_DISPLAY_GAP_VBLANK) |
		DISP2_GAP_MCHG(R600_PM_DISPLAY_GAP_IGNORE));
	WREG32(CG_DISPLAY_GAP_CNTL, tmp);
}

static void si_program_vc(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);

	WREG32(CG_FTV, pi->vrc);
}

static void si_clear_vc(struct rdn_si_dpm *rdev)
{
	WREG32(CG_FTV, 0);
}



static u8 si_get_strobe_mode_settings(struct rdn_si_dpm *rdev, u32 mclk)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	bool strobe_mode = false;
	u8 result = 0;

	if (mclk <= pi->mclk_strobe_mode_threshold)
		strobe_mode = true;

	if (pi->mem_gddr5)
		result = si_get_mclk_frequency_ratio(mclk, strobe_mode);
	else
		result = si_get_ddr3_mclk_frequency_ratio(mclk);

	if (strobe_mode)
		result |= SISLANDS_SMC_STROBE_ENABLE;

	return result;
}

static int si_upload_firmware(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	int ret;

	si_reset_smc(rdev);
	si_stop_smc_clock(rdev);

	ret = si_load_smc_ucode(rdev, rdev->smc_fw, rdev->smc_fw_size, si_pi->sram_end);

	return ret;
}

static bool si_validate_phase_shedding_tables(struct rdn_si_dpm *rdev,
					      const struct atom_voltage_table *table,
					      const struct rdn_ci_phase_shedding_limits_table *limits)
{
	u32 data, num_bits, num_levels;

	if ((table == NULL) || (limits == NULL))
		return false;

	data = table->mask_low;

	num_bits = hweight32(data);

	if (num_bits == 0)
		return false;

	num_levels = (1 << num_bits);

	if (table->count != num_levels)
		return false;

	if (limits->count != (num_levels - 1))
		return false;

	return true;
}


static int si_get_svi2_voltage_table(struct rdn_si_dpm *rdev,
				     struct rdn_ci_dep_table *voltage_dependency_table,
				     struct atom_voltage_table *voltage_table)
{
	u32 i;

	if (voltage_dependency_table == NULL)
		return -EINVAL;

	voltage_table->mask_low = 0;
	voltage_table->phase_delay = 0;

	voltage_table->count = voltage_dependency_table->count;
	for (i = 0; i < voltage_table->count; i++) {
		voltage_table->entries[i].value = voltage_dependency_table->entries[i].v;
		voltage_table->entries[i].smio_low = 0;
	}

	return 0;
}

static int si_construct_voltage_tables(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	int ret;

	if (pi->voltage_control) {
		ret = radeon_atom_get_voltage_table(rdev->card, VOLTAGE_TYPE_VDDC,
						    VOLTAGE_OBJ_GPIO_LUT, &eg_pi->vddc_voltage_table);
		if (ret)
			return ret;

		if (eg_pi->vddc_voltage_table.count > SISLANDS_MAX_NO_VREG_STEPS)
			si_trim_voltage_table_to_fit_state_table(SISLANDS_MAX_NO_VREG_STEPS,
								 &eg_pi->vddc_voltage_table);
	} else if (si_pi->voltage_control_svi2) {
		ret = si_get_svi2_voltage_table(rdev,
						&rdev->ci.dyn_state.vddc_dependency_on_mclk,
						&eg_pi->vddc_voltage_table);
		if (ret)
			return ret;
	} else {
		return -EINVAL;
	}

	if (eg_pi->vddci_control) {
		ret = radeon_atom_get_voltage_table(rdev->card, VOLTAGE_TYPE_VDDCI,
						    VOLTAGE_OBJ_GPIO_LUT, &eg_pi->vddci_voltage_table);
		if (ret)
			return ret;

		if (eg_pi->vddci_voltage_table.count > SISLANDS_MAX_NO_VREG_STEPS)
			si_trim_voltage_table_to_fit_state_table(SISLANDS_MAX_NO_VREG_STEPS,
								 &eg_pi->vddci_voltage_table);
	}
	if (si_pi->vddci_control_svi2) {
		ret = si_get_svi2_voltage_table(rdev,
						&rdev->ci.dyn_state.vddci_dependency_on_mclk,
						&eg_pi->vddci_voltage_table);
		if (ret)
			return ret;
	}

	if (pi->mvdd_control) {
		ret = radeon_atom_get_voltage_table(rdev->card, VOLTAGE_TYPE_MVDDC,
						    VOLTAGE_OBJ_GPIO_LUT, &si_pi->mvdd_voltage_table);

		if (ret) {
			pi->mvdd_control = false;
			return ret;
		}

		if (si_pi->mvdd_voltage_table.count == 0) {
			pi->mvdd_control = false;
			return -EINVAL;
		}

		if (si_pi->mvdd_voltage_table.count > SISLANDS_MAX_NO_VREG_STEPS)
			si_trim_voltage_table_to_fit_state_table(SISLANDS_MAX_NO_VREG_STEPS,
								 &si_pi->mvdd_voltage_table);
	}

	if (si_pi->vddc_phase_shed_control) {
		ret = radeon_atom_get_voltage_table(rdev->card, VOLTAGE_TYPE_VDDC,
						    VOLTAGE_OBJ_PHASE_LUT, &si_pi->vddc_phase_shed_table);
		if (ret)
			si_pi->vddc_phase_shed_control = false;

		if ((si_pi->vddc_phase_shed_table.count == 0) ||
		    (si_pi->vddc_phase_shed_table.count > SISLANDS_MAX_NO_VREG_STEPS))
			si_pi->vddc_phase_shed_control = false;
	}

	return 0;
}

static void si_populate_smc_voltage_table(struct rdn_si_dpm *rdev,
					  const struct atom_voltage_table *voltage_table,
					  SISLANDS_SMC_STATETABLE *table)
{
	unsigned int i;

	for (i = 0; i < voltage_table->count; i++)
		table->lowSMIO[i] |= cpu_to_be32(voltage_table->entries[i].smio_low);
}

static int si_populate_smc_voltage_tables(struct rdn_si_dpm *rdev,
					  SISLANDS_SMC_STATETABLE *table)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	u8 i;

	if (si_pi->voltage_control_svi2) {
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_svi_rework_gpio_id_svc,
			si_pi->svc_gpio_id);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_svi_rework_gpio_id_svd,
			si_pi->svd_gpio_id);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_svi_rework_plat_type,
					   2);
	} else {
		if (eg_pi->vddc_voltage_table.count) {
			si_populate_smc_voltage_table(rdev, &eg_pi->vddc_voltage_table, table);
			table->voltageMaskTable.lowMask[SISLANDS_SMC_VOLTAGEMASK_VDDC] =
				cpu_to_be32(eg_pi->vddc_voltage_table.mask_low);

			for (i = 0; i < eg_pi->vddc_voltage_table.count; i++) {
				if (pi->max_vddc_in_table <= eg_pi->vddc_voltage_table.entries[i].value) {
					table->maxVDDCIndexInPPTable = i;
					break;
				}
			}
		}

		if (eg_pi->vddci_voltage_table.count) {
			si_populate_smc_voltage_table(rdev, &eg_pi->vddci_voltage_table, table);

			table->voltageMaskTable.lowMask[SISLANDS_SMC_VOLTAGEMASK_VDDCI] =
				cpu_to_be32(eg_pi->vddci_voltage_table.mask_low);
		}


		if (si_pi->mvdd_voltage_table.count) {
			si_populate_smc_voltage_table(rdev, &si_pi->mvdd_voltage_table, table);

			table->voltageMaskTable.lowMask[SISLANDS_SMC_VOLTAGEMASK_MVDD] =
				cpu_to_be32(si_pi->mvdd_voltage_table.mask_low);
		}

		if (si_pi->vddc_phase_shed_control) {
			if (si_validate_phase_shedding_tables(rdev, &si_pi->vddc_phase_shed_table,
							      &rdev->ci.dyn_state.phase_shedding_limits_table)) {
				si_populate_smc_voltage_table(rdev, &si_pi->vddc_phase_shed_table, table);

				table->phaseMaskTable.lowMask[SISLANDS_SMC_VOLTAGEMASK_VDDC_PHASE_SHEDDING] =
					cpu_to_be32(si_pi->vddc_phase_shed_table.mask_low);

				si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_phase_shedding_delay,
							   (u32)si_pi->vddc_phase_shed_table.phase_delay);
			} else {
				si_pi->vddc_phase_shed_control = false;
			}
		}
	}

	return 0;
}

static int si_populate_voltage_value(struct rdn_si_dpm *rdev,
				     const struct atom_voltage_table *table,
				     u16 value, SISLANDS_SMC_VOLTAGE_VALUE *voltage)
{
	unsigned int i;

	for (i = 0; i < table->count; i++) {
		if (value <= table->entries[i].value) {
			voltage->index = (u8)i;
			voltage->value = cpu_to_be16(table->entries[i].value);
			break;
		}
	}

	if (i >= table->count)
		return -EINVAL;

	return 0;
}

static int si_populate_mvdd_value(struct rdn_si_dpm *rdev, u32 mclk,
				  SISLANDS_SMC_VOLTAGE_VALUE *voltage)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (pi->mvdd_control) {
		if (mclk <= pi->mvdd_split_frequency)
			voltage->index = 0;
		else
			voltage->index = (u8)(si_pi->mvdd_voltage_table.count) - 1;

		voltage->value = cpu_to_be16(si_pi->mvdd_voltage_table.entries[voltage->index].value);
	}
	return 0;
}

static int si_get_std_voltage_value(struct rdn_si_dpm *rdev,
				    SISLANDS_SMC_VOLTAGE_VALUE *voltage,
				    u16 *std_voltage)
{
	u16 v_index;
	bool voltage_found = false;
	*std_voltage = be16_to_cpu(voltage->value);

	if (rdev->ci.dyn_state.cac_leakage_table.entries) {
		if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_NEW_CAC_VOLTAGE) {
			if (rdev->ci.dyn_state.vddc_dependency_on_sclk.entries == NULL)
				return -EINVAL;

			for (v_index = 0; (u32)v_index < rdev->ci.dyn_state.vddc_dependency_on_sclk.count; v_index++) {
				if (be16_to_cpu(voltage->value) ==
				    (u16)rdev->ci.dyn_state.vddc_dependency_on_sclk.entries[v_index].v) {
					voltage_found = true;
					if ((u32)v_index < rdev->ci.dyn_state.cac_leakage_table.count)
						*std_voltage =
							rdev->ci.dyn_state.cac_leakage_table.entries[v_index].vddc;
					else
						*std_voltage =
							rdev->ci.dyn_state.cac_leakage_table.entries[rdev->ci.dyn_state.cac_leakage_table.count-1].vddc;
					break;
				}
			}

			if (!voltage_found) {
				for (v_index = 0; (u32)v_index < rdev->ci.dyn_state.vddc_dependency_on_sclk.count; v_index++) {
					if (be16_to_cpu(voltage->value) <=
					    (u16)rdev->ci.dyn_state.vddc_dependency_on_sclk.entries[v_index].v) {
						voltage_found = true;
						if ((u32)v_index < rdev->ci.dyn_state.cac_leakage_table.count)
							*std_voltage =
								rdev->ci.dyn_state.cac_leakage_table.entries[v_index].vddc;
						else
							*std_voltage =
								rdev->ci.dyn_state.cac_leakage_table.entries[rdev->ci.dyn_state.cac_leakage_table.count-1].vddc;
						break;
					}
				}
			}
		} else {
			if ((u32)voltage->index < rdev->ci.dyn_state.cac_leakage_table.count)
				*std_voltage = rdev->ci.dyn_state.cac_leakage_table.entries[voltage->index].vddc;
		}
	}

	return 0;
}

static int si_populate_std_voltage_value(struct rdn_si_dpm *rdev,
					 u16 value, u8 index,
					 SISLANDS_SMC_VOLTAGE_VALUE *voltage)
{
	voltage->index = index;
	voltage->value = cpu_to_be16(value);

	return 0;
}

static int si_populate_phase_shedding_value(struct rdn_si_dpm *rdev,
					    const struct rdn_ci_phase_shedding_limits_table *limits,
					    u16 voltage, u32 sclk, u32 mclk,
					    SISLANDS_SMC_VOLTAGE_VALUE *smc_voltage)
{
	unsigned int i;

	for (i = 0; i < limits->count; i++) {
		if ((voltage <= limits->entries[i].voltage) &&
		    (sclk <= limits->entries[i].sclk) &&
		    (mclk <= limits->entries[i].mclk))
			break;
	}

	smc_voltage->phase_settings = (u8)i;

	return 0;
}

static int si_init_arb_table_index(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;
	int ret;

	ret = si_read_smc_sram_dword(rdev, si_pi->arb_table_start, &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	tmp &= 0x00FFFFFF;
	tmp |= MC_CG_ARB_FREQ_F1 << 24;

	return si_write_smc_sram_dword(rdev, si_pi->arb_table_start,  tmp, si_pi->sram_end);
}

static int si_initial_switch_from_arb_f0_to_f1(struct rdn_si_dpm *rdev)
{
	return ni_copy_and_switch_arb_sets(rdev, MC_CG_ARB_FREQ_F0, MC_CG_ARB_FREQ_F1);
}

static int si_reset_to_default(struct rdn_si_dpm *rdev)
{
	return (si_send_msg_to_smc(rdev, PPSMC_MSG_ResetToDefaults) == PPSMC_Result_OK) ?
		0 : -EINVAL;
}

static int si_force_switch_to_arb_f0(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;
	int ret;

	ret = si_read_smc_sram_dword(rdev, si_pi->arb_table_start,
				     &tmp, si_pi->sram_end);
	if (ret)
		return ret;

	tmp = (tmp >> 24) & 0xff;

	if (tmp == MC_CG_ARB_FREQ_F0)
		return 0;

	return ni_copy_and_switch_arb_sets(rdev, tmp, MC_CG_ARB_FREQ_F0);
}

static u32 si_calculate_memory_refresh_rate(struct rdn_si_dpm *rdev,
					    u32 engine_clock)
{
	u32 dram_rows;
	u32 dram_refresh_rate;
	u32 mc_arb_rfsh_rate;
	u32 tmp = (RREG32(MC_ARB_RAMCFG) & NOOFROWS_MASK) >> NOOFROWS_SHIFT;

	if (tmp >= 4)
		dram_rows = 16384;
	else
		dram_rows = 1 << (tmp + 10);

	dram_refresh_rate = 1 << ((RREG32(MC_SEQ_MISC0) & 0x3) + 3);
	mc_arb_rfsh_rate = ((engine_clock * 10) * dram_refresh_rate / dram_rows - 32) / 64;

	return mc_arb_rfsh_rate;
}

static int si_populate_memory_timing_parameters(struct rdn_si_dpm *rdev,
						struct rv7xx_pl *pl,
						SMC_SIslands_MCArbDramTimingRegisterSet *arb_regs)
{
	u32 dram_timing;
	u32 dram_timing2;
	u32 burst_time;

	arb_regs->mc_arb_rfsh_rate =
		(u8)si_calculate_memory_refresh_rate(rdev, pl->sclk);

	radeon_atom_set_engine_dram_timings(rdev->card,
					    pl->sclk,
					    pl->mclk);

	dram_timing  = RREG32(MC_ARB_DRAM_TIMING);
	dram_timing2 = RREG32(MC_ARB_DRAM_TIMING2);
	burst_time = RREG32(MC_ARB_BURST_TIME) & STATE0_MASK;

	arb_regs->mc_arb_dram_timing  = cpu_to_be32(dram_timing);
	arb_regs->mc_arb_dram_timing2 = cpu_to_be32(dram_timing2);
	arb_regs->mc_arb_burst_time = (u8)burst_time;

	return 0;
}

static int si_do_program_memory_timing_parameters(struct rdn_si_dpm *rdev,
						  struct si_rps *radeon_state,
						  unsigned int first_arb_set)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct ni_ps *state = ni_get_ps(radeon_state);
	SMC_SIslands_MCArbDramTimingRegisterSet arb_regs = { 0 };
	int i, ret = 0;

	for (i = 0; i < state->performance_level_count; i++) {
		ret = si_populate_memory_timing_parameters(rdev, &state->performance_levels[i], &arb_regs);
		if (ret)
			break;
		ret = si_copy_bytes_to_smc(rdev,
					   si_pi->arb_table_start +
					   offsetof(SMC_SIslands_MCArbDramTimingRegisters, data) +
					   sizeof(SMC_SIslands_MCArbDramTimingRegisterSet) * (first_arb_set + i),
					   (u8 *)&arb_regs,
					   sizeof(SMC_SIslands_MCArbDramTimingRegisterSet),
					   si_pi->sram_end);
		if (ret)
			break;
	}

	return ret;
}

static int si_program_memory_timing_parameters(struct rdn_si_dpm *rdev,
					       struct si_rps *radeon_new_state)
{
	return si_do_program_memory_timing_parameters(rdev, radeon_new_state,
						      SISLANDS_DRIVER_STATE_ARB_INDEX);
}

static int si_populate_initial_mvdd_value(struct rdn_si_dpm *rdev,
					  struct SISLANDS_SMC_VOLTAGE_VALUE *voltage)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (pi->mvdd_control)
		return si_populate_voltage_value(rdev, &si_pi->mvdd_voltage_table,
						 si_pi->mvdd_bootup_value, voltage);

	return 0;
}

static int si_populate_smc_initial_state(struct rdn_si_dpm *rdev,
					 struct si_rps *radeon_initial_state,
					 SISLANDS_SMC_STATETABLE *table)
{
	struct ni_ps *initial_state = ni_get_ps(radeon_initial_state);
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 reg;
	int ret;

	table->initialState.level.mclk.vDLL_CNTL =
		cpu_to_be32(si_pi->clock_registers.dll_cntl);
	table->initialState.level.mclk.vMCLK_PWRMGT_CNTL =
		cpu_to_be32(si_pi->clock_registers.mclk_pwrmgt_cntl);
	table->initialState.level.mclk.vMPLL_AD_FUNC_CNTL =
		cpu_to_be32(si_pi->clock_registers.mpll_ad_func_cntl);
	table->initialState.level.mclk.vMPLL_DQ_FUNC_CNTL =
		cpu_to_be32(si_pi->clock_registers.mpll_dq_func_cntl);
	table->initialState.level.mclk.vMPLL_FUNC_CNTL =
		cpu_to_be32(si_pi->clock_registers.mpll_func_cntl);
	table->initialState.level.mclk.vMPLL_FUNC_CNTL_1 =
		cpu_to_be32(si_pi->clock_registers.mpll_func_cntl_1);
	table->initialState.level.mclk.vMPLL_FUNC_CNTL_2 =
		cpu_to_be32(si_pi->clock_registers.mpll_func_cntl_2);
	table->initialState.level.mclk.vMPLL_SS =
		cpu_to_be32(si_pi->clock_registers.mpll_ss1);
	table->initialState.level.mclk.vMPLL_SS2 =
		cpu_to_be32(si_pi->clock_registers.mpll_ss2);

	table->initialState.level.mclk.mclk_value =
		cpu_to_be32(initial_state->performance_levels[0].mclk);

	table->initialState.level.sclk.vCG_SPLL_FUNC_CNTL =
		cpu_to_be32(si_pi->clock_registers.cg_spll_func_cntl);
	table->initialState.level.sclk.vCG_SPLL_FUNC_CNTL_2 =
		cpu_to_be32(si_pi->clock_registers.cg_spll_func_cntl_2);
	table->initialState.level.sclk.vCG_SPLL_FUNC_CNTL_3 =
		cpu_to_be32(si_pi->clock_registers.cg_spll_func_cntl_3);
	table->initialState.level.sclk.vCG_SPLL_FUNC_CNTL_4 =
		cpu_to_be32(si_pi->clock_registers.cg_spll_func_cntl_4);
	table->initialState.level.sclk.vCG_SPLL_SPREAD_SPECTRUM =
		cpu_to_be32(si_pi->clock_registers.cg_spll_spread_spectrum);
	table->initialState.level.sclk.vCG_SPLL_SPREAD_SPECTRUM_2  =
		cpu_to_be32(si_pi->clock_registers.cg_spll_spread_spectrum_2);

	table->initialState.level.sclk.sclk_value =
		cpu_to_be32(initial_state->performance_levels[0].sclk);

	table->initialState.level.arbRefreshState =
		SISLANDS_INITIAL_STATE_ARB_INDEX;

	table->initialState.level.ACIndex = 0;

	ret = si_populate_voltage_value(rdev, &eg_pi->vddc_voltage_table,
					initial_state->performance_levels[0].vddc,
					&table->initialState.level.vddc);

	if (!ret) {
		u16 std_vddc;

		ret = si_get_std_voltage_value(rdev,
					       &table->initialState.level.vddc,
					       &std_vddc);
		if (!ret)
			si_populate_std_voltage_value(rdev, std_vddc,
						      table->initialState.level.vddc.index,
						      &table->initialState.level.std_vddc);
	}

	if (eg_pi->vddci_control)
		si_populate_voltage_value(rdev,
					  &eg_pi->vddci_voltage_table,
					  initial_state->performance_levels[0].vddci,
					  &table->initialState.level.vddci);

	if (si_pi->vddc_phase_shed_control)
		si_populate_phase_shedding_value(rdev,
						 &rdev->ci.dyn_state.phase_shedding_limits_table,
						 initial_state->performance_levels[0].vddc,
						 initial_state->performance_levels[0].sclk,
						 initial_state->performance_levels[0].mclk,
						 &table->initialState.level.vddc);

	si_populate_initial_mvdd_value(rdev, &table->initialState.level.mvdd);

	reg = CG_R(0xffff) | CG_L(0);
	table->initialState.level.aT = cpu_to_be32(reg);

	table->initialState.level.bSP = cpu_to_be32(pi->dsp);

	table->initialState.level.gen2PCIE = (u8)si_pi->boot_pcie_gen;

	if (pi->mem_gddr5) {
		table->initialState.level.strobeMode =
			si_get_strobe_mode_settings(rdev,
						    initial_state->performance_levels[0].mclk);

		if (initial_state->performance_levels[0].mclk > pi->mclk_edc_enable_threshold)
			table->initialState.level.mcFlags = SISLANDS_SMC_MC_EDC_RD_FLAG | SISLANDS_SMC_MC_EDC_WR_FLAG;
		else
			table->initialState.level.mcFlags =  0;
	}

	table->initialState.levelCount = 1;

	table->initialState.flags |= PPSMC_SWSTATE_FLAG_DC;

	table->initialState.level.dpm2.MaxPS = 0;
	table->initialState.level.dpm2.NearTDPDec = 0;
	table->initialState.level.dpm2.AboveSafeInc = 0;
	table->initialState.level.dpm2.BelowSafeInc = 0;
	table->initialState.level.dpm2.PwrEfficiencyRatio = 0;

	reg = MIN_POWER_MASK | MAX_POWER_MASK;
	table->initialState.level.SQPowerThrottle = cpu_to_be32(reg);

	reg = MAX_POWER_DELTA_MASK | STI_SIZE_MASK | LTI_RATIO_MASK;
	table->initialState.level.SQPowerThrottle_2 = cpu_to_be32(reg);

	return 0;
}

static int si_populate_smc_acpi_state(struct rdn_si_dpm *rdev,
				      SISLANDS_SMC_STATETABLE *table)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 spll_func_cntl = si_pi->clock_registers.cg_spll_func_cntl;
	u32 spll_func_cntl_2 = si_pi->clock_registers.cg_spll_func_cntl_2;
	u32 spll_func_cntl_3 = si_pi->clock_registers.cg_spll_func_cntl_3;
	u32 spll_func_cntl_4 = si_pi->clock_registers.cg_spll_func_cntl_4;
	u32 dll_cntl = si_pi->clock_registers.dll_cntl;
	u32 mclk_pwrmgt_cntl = si_pi->clock_registers.mclk_pwrmgt_cntl;
	u32 mpll_ad_func_cntl = si_pi->clock_registers.mpll_ad_func_cntl;
	u32 mpll_dq_func_cntl = si_pi->clock_registers.mpll_dq_func_cntl;
	u32 mpll_func_cntl = si_pi->clock_registers.mpll_func_cntl;
	u32 mpll_func_cntl_1 = si_pi->clock_registers.mpll_func_cntl_1;
	u32 mpll_func_cntl_2 = si_pi->clock_registers.mpll_func_cntl_2;
	u32 reg;
	int ret;

	table->ACPIState = table->initialState;

	table->ACPIState.flags &= ~PPSMC_SWSTATE_FLAG_DC;

	if (pi->acpi_vddc) {
		ret = si_populate_voltage_value(rdev, &eg_pi->vddc_voltage_table,
						pi->acpi_vddc, &table->ACPIState.level.vddc);
		if (!ret) {
			u16 std_vddc;

			ret = si_get_std_voltage_value(rdev,
						       &table->ACPIState.level.vddc, &std_vddc);
			if (!ret)
				si_populate_std_voltage_value(rdev, std_vddc,
							      table->ACPIState.level.vddc.index,
							      &table->ACPIState.level.std_vddc);
		}
		table->ACPIState.level.gen2PCIE = si_pi->acpi_pcie_gen;

		if (si_pi->vddc_phase_shed_control) {
			si_populate_phase_shedding_value(rdev,
							 &rdev->ci.dyn_state.phase_shedding_limits_table,
							 pi->acpi_vddc,
							 0,
							 0,
							 &table->ACPIState.level.vddc);
		}
	} else {
		ret = si_populate_voltage_value(rdev, &eg_pi->vddc_voltage_table,
						pi->min_vddc_in_table, &table->ACPIState.level.vddc);
		if (!ret) {
			u16 std_vddc;

			ret = si_get_std_voltage_value(rdev,
						       &table->ACPIState.level.vddc, &std_vddc);

			if (!ret)
				si_populate_std_voltage_value(rdev, std_vddc,
							      table->ACPIState.level.vddc.index,
							      &table->ACPIState.level.std_vddc);
		}
		table->ACPIState.level.gen2PCIE = (u8)r600_get_pcie_gen_support(si_pi->sys_pcie_mask,
										    si_pi->boot_pcie_gen,
										    RDN_CI_PCIE_GEN1);

		if (si_pi->vddc_phase_shed_control)
			si_populate_phase_shedding_value(rdev,
							 &rdev->ci.dyn_state.phase_shedding_limits_table,
							 pi->min_vddc_in_table,
							 0,
							 0,
							 &table->ACPIState.level.vddc);
	}

	if (pi->acpi_vddc) {
		if (eg_pi->acpi_vddci)
			si_populate_voltage_value(rdev, &eg_pi->vddci_voltage_table,
						  eg_pi->acpi_vddci,
						  &table->ACPIState.level.vddci);
	}

	mclk_pwrmgt_cntl |= MRDCK0_RESET | MRDCK1_RESET;
	mclk_pwrmgt_cntl &= ~(MRDCK0_PDNB | MRDCK1_PDNB);

	dll_cntl &= ~(MRDCK0_BYPASS | MRDCK1_BYPASS);

	spll_func_cntl_2 &= ~SCLK_MUX_SEL_MASK;
	spll_func_cntl_2 |= SCLK_MUX_SEL(4);

	table->ACPIState.level.mclk.vDLL_CNTL =
		cpu_to_be32(dll_cntl);
	table->ACPIState.level.mclk.vMCLK_PWRMGT_CNTL =
		cpu_to_be32(mclk_pwrmgt_cntl);
	table->ACPIState.level.mclk.vMPLL_AD_FUNC_CNTL =
		cpu_to_be32(mpll_ad_func_cntl);
	table->ACPIState.level.mclk.vMPLL_DQ_FUNC_CNTL =
		cpu_to_be32(mpll_dq_func_cntl);
	table->ACPIState.level.mclk.vMPLL_FUNC_CNTL =
		cpu_to_be32(mpll_func_cntl);
	table->ACPIState.level.mclk.vMPLL_FUNC_CNTL_1 =
		cpu_to_be32(mpll_func_cntl_1);
	table->ACPIState.level.mclk.vMPLL_FUNC_CNTL_2 =
		cpu_to_be32(mpll_func_cntl_2);
	table->ACPIState.level.mclk.vMPLL_SS =
		cpu_to_be32(si_pi->clock_registers.mpll_ss1);
	table->ACPIState.level.mclk.vMPLL_SS2 =
		cpu_to_be32(si_pi->clock_registers.mpll_ss2);

	table->ACPIState.level.sclk.vCG_SPLL_FUNC_CNTL =
		cpu_to_be32(spll_func_cntl);
	table->ACPIState.level.sclk.vCG_SPLL_FUNC_CNTL_2 =
		cpu_to_be32(spll_func_cntl_2);
	table->ACPIState.level.sclk.vCG_SPLL_FUNC_CNTL_3 =
		cpu_to_be32(spll_func_cntl_3);
	table->ACPIState.level.sclk.vCG_SPLL_FUNC_CNTL_4 =
		cpu_to_be32(spll_func_cntl_4);

	table->ACPIState.level.mclk.mclk_value = 0;
	table->ACPIState.level.sclk.sclk_value = 0;

	si_populate_mvdd_value(rdev, 0, &table->ACPIState.level.mvdd);

	if (eg_pi->dynamic_ac_timing)
		table->ACPIState.level.ACIndex = 0;

	table->ACPIState.level.dpm2.MaxPS = 0;
	table->ACPIState.level.dpm2.NearTDPDec = 0;
	table->ACPIState.level.dpm2.AboveSafeInc = 0;
	table->ACPIState.level.dpm2.BelowSafeInc = 0;
	table->ACPIState.level.dpm2.PwrEfficiencyRatio = 0;

	reg = MIN_POWER_MASK | MAX_POWER_MASK;
	table->ACPIState.level.SQPowerThrottle = cpu_to_be32(reg);

	reg = MAX_POWER_DELTA_MASK | STI_SIZE_MASK | LTI_RATIO_MASK;
	table->ACPIState.level.SQPowerThrottle_2 = cpu_to_be32(reg);

	return 0;
}

static int si_populate_ulv_state(struct rdn_si_dpm *rdev,
				 struct SISLANDS_SMC_SWSTATE_SINGLE *state)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_ulv_param *ulv = &si_pi->ulv;
	u32 sclk_in_sr = 1350; /* ??? */
	int ret;

	ret = si_convert_power_level_to_smc(rdev, &ulv->pl,
					    &state->level);
	if (!ret) {
		if (eg_pi->sclk_deep_sleep) {
			if (sclk_in_sr <= SCLK_MIN_DEEPSLEEP_FREQ)
				state->level.stateFlags |= PPSMC_STATEFLAG_DEEPSLEEP_BYPASS;
			else
				state->level.stateFlags |= PPSMC_STATEFLAG_DEEPSLEEP_THROTTLE;
		}
		if (ulv->one_pcie_lane_in_ulv)
			state->flags |= PPSMC_SWSTATE_FLAG_PCIE_X1;
		state->level.arbRefreshState = (u8)(SISLANDS_ULV_STATE_ARB_INDEX);
		state->level.ACIndex = 1;
		state->level.std_vddc = state->level.vddc;
		state->levelCount = 1;

		state->flags |= PPSMC_SWSTATE_FLAG_DC;
	}

	return ret;
}

static int si_program_ulv_memory_timing_parameters(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_ulv_param *ulv = &si_pi->ulv;
	SMC_SIslands_MCArbDramTimingRegisterSet arb_regs = { 0 };
	int ret;

	ret = si_populate_memory_timing_parameters(rdev, &ulv->pl,
						   &arb_regs);
	if (ret)
		return ret;

	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_ulv_volt_change_delay,
				   ulv->volt_change_delay);

	ret = si_copy_bytes_to_smc(rdev,
				   si_pi->arb_table_start +
				   offsetof(SMC_SIslands_MCArbDramTimingRegisters, data) +
				   sizeof(SMC_SIslands_MCArbDramTimingRegisterSet) * SISLANDS_ULV_STATE_ARB_INDEX,
				   (u8 *)&arb_regs,
				   sizeof(SMC_SIslands_MCArbDramTimingRegisterSet),
				   si_pi->sram_end);

	return ret;
}

static void si_get_mvdd_configuration(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);

	pi->mvdd_split_frequency = 30000;
}

static int si_init_smc_table(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_rps *radeon_boot_state = rdev->boot_ps;
	const struct si_ulv_param *ulv = &si_pi->ulv;
	SISLANDS_SMC_STATETABLE  *table = &si_pi->smc_statetable;
	int ret;
	u32 lane_width;
	u32 vr_hot_gpio;

	si_populate_smc_voltage_tables(rdev, table);

	switch (rdev->int_thermal_type) {
	case THERMAL_TYPE_SI:
	case THERMAL_TYPE_EMC2103_WITH_INTERNAL:
		table->thermalProtectType = PPSMC_THERMAL_PROTECT_TYPE_INTERNAL;
		break;
	case THERMAL_TYPE_NONE:
		table->thermalProtectType = PPSMC_THERMAL_PROTECT_TYPE_NONE;
		break;
	default:
		table->thermalProtectType = PPSMC_THERMAL_PROTECT_TYPE_EXTERNAL;
		break;
	}

	if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_HARDWAREDC)
		table->systemFlags |= PPSMC_SYSTEMFLAG_GPIO_DC;

	if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_REGULATOR_HOT) {
		if ((rdev->device != 0x6818) && (rdev->device != 0x6819))
			table->systemFlags |= PPSMC_SYSTEMFLAG_REGULATOR_HOT;
	}

	if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_STEPVDDC)
		table->systemFlags |= PPSMC_SYSTEMFLAG_STEPVDDC;

	if (pi->mem_gddr5)
		table->systemFlags |= PPSMC_SYSTEMFLAG_GDDR5;

	if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_REVERT_GPIO5_POLARITY)
		table->extraFlags |= PPSMC_EXTRAFLAGS_AC2DC_GPIO5_POLARITY_HIGH;

	if (rdev->ci.platform_caps & ATOM_PP_PLATFORM_CAP_VRHOT_GPIO_CONFIGURABLE) {
		table->systemFlags |= PPSMC_SYSTEMFLAG_REGULATOR_HOT_PROG_GPIO;
		vr_hot_gpio = rdev->ci.backbias_response_time;
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_vr_hot_gpio,
					   vr_hot_gpio);
	}

	ret = si_populate_smc_initial_state(rdev, radeon_boot_state, table);
	if (ret)
		return ret;

	ret = si_populate_smc_acpi_state(rdev, table);
	if (ret)
		return ret;

	table->driverState.flags = table->initialState.flags;
	table->driverState.levelCount = table->initialState.levelCount;
	table->driverState.levels[0] = table->initialState.level;

	ret = si_do_program_memory_timing_parameters(rdev, radeon_boot_state,
						     SISLANDS_INITIAL_STATE_ARB_INDEX);
	if (ret)
		return ret;

	if (ulv->supported && ulv->pl.vddc) {
		ret = si_populate_ulv_state(rdev, &table->ULVState);
		if (ret)
			return ret;

		ret = si_program_ulv_memory_timing_parameters(rdev);
		if (ret)
			return ret;

		WREG32(CG_ULV_CONTROL, ulv->cg_ulv_control);
		WREG32(CG_ULV_PARAMETER, ulv->cg_ulv_parameter);

		lane_width = radeon_get_pcie_lanes(rdev);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_non_ulv_pcie_link_width, lane_width);
	} else {
		table->ULVState = table->initialState;
	}

	return si_copy_bytes_to_smc(rdev, si_pi->state_table_start,
				    (u8 *)table, sizeof(SISLANDS_SMC_STATETABLE),
				    si_pi->sram_end);
}

static int si_calculate_sclk_params(struct rdn_si_dpm *rdev,
				    u32 engine_clock,
				    SISLANDS_SMC_SCLK_VALUE *sclk)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct atom_clock_dividers dividers;
	u32 spll_func_cntl = si_pi->clock_registers.cg_spll_func_cntl;
	u32 spll_func_cntl_2 = si_pi->clock_registers.cg_spll_func_cntl_2;
	u32 spll_func_cntl_3 = si_pi->clock_registers.cg_spll_func_cntl_3;
	u32 spll_func_cntl_4 = si_pi->clock_registers.cg_spll_func_cntl_4;
	u32 cg_spll_spread_spectrum = si_pi->clock_registers.cg_spll_spread_spectrum;
	u32 cg_spll_spread_spectrum_2 = si_pi->clock_registers.cg_spll_spread_spectrum_2;
	u64 tmp;
	u32 reference_clock = rdev->ci.reference_freq;
	u32 reference_divider;
	u32 fbdiv;
	int ret;

	ret = radeon_atom_get_clock_dividers(rdev, COMPUTE_ENGINE_PLL_PARAM,
					     engine_clock, false, &dividers);
	if (ret)
		return ret;

	reference_divider = 1 + dividers.ref_div;

	tmp = (u64) engine_clock * reference_divider * dividers.post_div * 16384;
	do_div(tmp, reference_clock);
	fbdiv = (u32) tmp;

	spll_func_cntl &= ~(SPLL_PDIV_A_MASK | SPLL_REF_DIV_MASK);
	spll_func_cntl |= SPLL_REF_DIV(dividers.ref_div);
	spll_func_cntl |= SPLL_PDIV_A(dividers.post_div);

	spll_func_cntl_2 &= ~SCLK_MUX_SEL_MASK;
	spll_func_cntl_2 |= SCLK_MUX_SEL(2);

	spll_func_cntl_3 &= ~SPLL_FB_DIV_MASK;
	spll_func_cntl_3 |= SPLL_FB_DIV(fbdiv);
	spll_func_cntl_3 |= SPLL_DITHEN;

	if (pi->sclk_ss) {
		struct radeon_atom_ss ss;
		u32 vco_freq = engine_clock * dividers.post_div;

		if (radeon_atombios_get_asic_ss_info(&rdev->ci, &ss,
						     ASIC_INTERNAL_ENGINE_SS, vco_freq)) {
			u32 clk_s = reference_clock * 5 / (reference_divider * ss.rate);
			u32 clk_v = 4 * ss.percentage * fbdiv / (clk_s * 10000);

			cg_spll_spread_spectrum &= ~CLK_S_MASK;
			cg_spll_spread_spectrum |= CLK_S(clk_s);
			cg_spll_spread_spectrum |= SSEN;

			cg_spll_spread_spectrum_2 &= ~CLK_V_MASK;
			cg_spll_spread_spectrum_2 |= CLK_V(clk_v);
		}
	}

	sclk->sclk_value = engine_clock;
	sclk->vCG_SPLL_FUNC_CNTL = spll_func_cntl;
	sclk->vCG_SPLL_FUNC_CNTL_2 = spll_func_cntl_2;
	sclk->vCG_SPLL_FUNC_CNTL_3 = spll_func_cntl_3;
	sclk->vCG_SPLL_FUNC_CNTL_4 = spll_func_cntl_4;
	sclk->vCG_SPLL_SPREAD_SPECTRUM = cg_spll_spread_spectrum;
	sclk->vCG_SPLL_SPREAD_SPECTRUM_2 = cg_spll_spread_spectrum_2;

	return 0;
}

static int si_populate_sclk_value(struct rdn_si_dpm *rdev,
				  u32 engine_clock,
				  SISLANDS_SMC_SCLK_VALUE *sclk)
{
	SISLANDS_SMC_SCLK_VALUE sclk_tmp;
	int ret;

	ret = si_calculate_sclk_params(rdev, engine_clock, &sclk_tmp);
	if (!ret) {
		sclk->sclk_value = cpu_to_be32(sclk_tmp.sclk_value);
		sclk->vCG_SPLL_FUNC_CNTL = cpu_to_be32(sclk_tmp.vCG_SPLL_FUNC_CNTL);
		sclk->vCG_SPLL_FUNC_CNTL_2 = cpu_to_be32(sclk_tmp.vCG_SPLL_FUNC_CNTL_2);
		sclk->vCG_SPLL_FUNC_CNTL_3 = cpu_to_be32(sclk_tmp.vCG_SPLL_FUNC_CNTL_3);
		sclk->vCG_SPLL_FUNC_CNTL_4 = cpu_to_be32(sclk_tmp.vCG_SPLL_FUNC_CNTL_4);
		sclk->vCG_SPLL_SPREAD_SPECTRUM = cpu_to_be32(sclk_tmp.vCG_SPLL_SPREAD_SPECTRUM);
		sclk->vCG_SPLL_SPREAD_SPECTRUM_2 = cpu_to_be32(sclk_tmp.vCG_SPLL_SPREAD_SPECTRUM_2);
	}

	return ret;
}

static int si_populate_mclk_value(struct rdn_si_dpm *rdev,
				  u32 engine_clock,
				  u32 memory_clock,
				  SISLANDS_SMC_MCLK_VALUE *mclk,
				  bool strobe_mode,
				  bool dll_state_on)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32  dll_cntl = si_pi->clock_registers.dll_cntl;
	u32  mclk_pwrmgt_cntl = si_pi->clock_registers.mclk_pwrmgt_cntl;
	u32  mpll_ad_func_cntl = si_pi->clock_registers.mpll_ad_func_cntl;
	u32  mpll_dq_func_cntl = si_pi->clock_registers.mpll_dq_func_cntl;
	u32  mpll_func_cntl = si_pi->clock_registers.mpll_func_cntl;
	u32  mpll_func_cntl_1 = si_pi->clock_registers.mpll_func_cntl_1;
	u32  mpll_func_cntl_2 = si_pi->clock_registers.mpll_func_cntl_2;
	u32  mpll_ss1 = si_pi->clock_registers.mpll_ss1;
	u32  mpll_ss2 = si_pi->clock_registers.mpll_ss2;
	struct atom_mpll_param mpll_param;
	int ret;

	ret = radeon_atom_get_memory_pll_dividers(rdev->card, memory_clock, strobe_mode, &mpll_param);
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

	if (pi->mclk_ss) {
		struct radeon_atom_ss ss;
		u32 freq_nom;
		u32 tmp;
		u32 reference_clock = rdev->ci.reference_freq;

		if (pi->mem_gddr5)
			freq_nom = memory_clock * 4;
		else
			freq_nom = memory_clock * 2;

		tmp = freq_nom / reference_clock;
		tmp = tmp * tmp;
		if (radeon_atombios_get_asic_ss_info(&rdev->ci, &ss,
						     ASIC_INTERNAL_MEMORY_SS, freq_nom)) {
			u32 clks = reference_clock * 5 / ss.rate;
			u32 clkv = (u32)((((131 * ss.percentage * ss.rate) / 100) * tmp) / freq_nom);

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

	mclk->mclk_value = cpu_to_be32(memory_clock);
	mclk->vMPLL_FUNC_CNTL = cpu_to_be32(mpll_func_cntl);
	mclk->vMPLL_FUNC_CNTL_1 = cpu_to_be32(mpll_func_cntl_1);
	mclk->vMPLL_FUNC_CNTL_2 = cpu_to_be32(mpll_func_cntl_2);
	mclk->vMPLL_AD_FUNC_CNTL = cpu_to_be32(mpll_ad_func_cntl);
	mclk->vMPLL_DQ_FUNC_CNTL = cpu_to_be32(mpll_dq_func_cntl);
	mclk->vMCLK_PWRMGT_CNTL = cpu_to_be32(mclk_pwrmgt_cntl);
	mclk->vDLL_CNTL = cpu_to_be32(dll_cntl);
	mclk->vMPLL_SS = cpu_to_be32(mpll_ss1);
	mclk->vMPLL_SS2 = cpu_to_be32(mpll_ss2);

	return 0;
}

static void si_populate_smc_sp(struct rdn_si_dpm *rdev,
			       struct si_rps *radeon_state,
			       SISLANDS_SMC_SWSTATE *smc_state)
{
	struct ni_ps *ps = ni_get_ps(radeon_state);
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	int i;

	for (i = 0; i < ps->performance_level_count - 1; i++)
		smc_state->levels[i].bSP = cpu_to_be32(pi->dsp);

	smc_state->levels[ps->performance_level_count - 1].bSP =
		cpu_to_be32(pi->psp);
}

static int si_convert_power_level_to_smc(struct rdn_si_dpm *rdev,
					 struct rv7xx_pl *pl,
					 SISLANDS_SMC_HW_PERFORMANCE_LEVEL *level)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	int ret;
	bool dll_state_on;
	u16 std_vddc;
	bool gmc_pg = false;

	if (eg_pi->pcie_performance_request &&
	    (si_pi->force_pcie_gen != RDN_CI_PCIE_GEN_INVALID))
		level->gen2PCIE = (u8)si_pi->force_pcie_gen;
	else
		level->gen2PCIE = (u8)pl->pcie_gen;

	ret = si_populate_sclk_value(rdev, pl->sclk, &level->sclk);
	if (ret)
		return ret;

	level->mcFlags =  0;

	if (pi->mclk_stutter_mode_threshold &&
	    (pl->mclk <= pi->mclk_stutter_mode_threshold) &&
	    !eg_pi->uvd_enabled &&
	    (RREG32(DPG_PIPE_STUTTER_CONTROL) & STUTTER_ENABLE) &&
	    (rdev->ci.new_active_crtc_count <= 2)) {
		level->mcFlags |= SISLANDS_SMC_MC_STUTTER_EN;

		if (gmc_pg)
			level->mcFlags |= SISLANDS_SMC_MC_PG_EN;
	}

	if (pi->mem_gddr5) {
		if (pl->mclk > pi->mclk_edc_enable_threshold)
			level->mcFlags |= SISLANDS_SMC_MC_EDC_RD_FLAG;

		if (pl->mclk > eg_pi->mclk_edc_wr_enable_threshold)
			level->mcFlags |= SISLANDS_SMC_MC_EDC_WR_FLAG;

		level->strobeMode = si_get_strobe_mode_settings(rdev, pl->mclk);

		if (level->strobeMode & SISLANDS_SMC_STROBE_ENABLE) {
			if (si_get_mclk_frequency_ratio(pl->mclk, true) >=
			    ((RREG32(MC_SEQ_MISC7) >> 16) & 0xf))
				dll_state_on = ((RREG32(MC_SEQ_MISC5) >> 1) & 0x1) ? true : false;
			else
				dll_state_on = ((RREG32(MC_SEQ_MISC6) >> 1) & 0x1) ? true : false;
		} else {
			dll_state_on = false;
		}
	} else {
		level->strobeMode = si_get_strobe_mode_settings(rdev,
								pl->mclk);

		dll_state_on = ((RREG32(MC_SEQ_MISC5) >> 1) & 0x1) ? true : false;
	}

	ret = si_populate_mclk_value(rdev,
				     pl->sclk,
				     pl->mclk,
				     &level->mclk,
				     (level->strobeMode & SISLANDS_SMC_STROBE_ENABLE) != 0, dll_state_on);
	if (ret)
		return ret;

	ret = si_populate_voltage_value(rdev,
					&eg_pi->vddc_voltage_table,
					pl->vddc, &level->vddc);
	if (ret)
		return ret;


	ret = si_get_std_voltage_value(rdev, &level->vddc, &std_vddc);
	if (ret)
		return ret;

	ret = si_populate_std_voltage_value(rdev, std_vddc,
					    level->vddc.index, &level->std_vddc);
	if (ret)
		return ret;

	if (eg_pi->vddci_control) {
		ret = si_populate_voltage_value(rdev, &eg_pi->vddci_voltage_table,
						pl->vddci, &level->vddci);
		if (ret)
			return ret;
	}

	if (si_pi->vddc_phase_shed_control) {
		ret = si_populate_phase_shedding_value(rdev,
						       &rdev->ci.dyn_state.phase_shedding_limits_table,
						       pl->vddc,
						       pl->sclk,
						       pl->mclk,
						       &level->vddc);
		if (ret)
			return ret;
	}

	/*
	 * Linux stores this u32 without cpu_to_be32(), so the SMC finds it
	 * byte-swapped (10 CUs reads as 0x0a000000). Linux's bytes are kept:
	 * they are what the firmware has run with on every Linux machine. 0 on
	 * the parts si_set_max_cu_value() does not list (the R7 450, 0x682B).
	 */
	level->MaxPoweredUpCU = cpu_to_le32(si_pi->max_cu);

	ret = si_populate_mvdd_value(rdev, pl->mclk, &level->mvdd);

	return ret;
}

static int si_populate_smc_t(struct rdn_si_dpm *rdev,
			     struct si_rps *radeon_state,
			     SISLANDS_SMC_SWSTATE *smc_state)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct ni_ps *state = ni_get_ps(radeon_state);
	u32 a_t;
	u32 t_l, t_h;
	u32 high_bsp;
	int i, ret;

	if (state->performance_level_count >= 9)
		return -EINVAL;

	if (state->performance_level_count < 2) {
		a_t = CG_R(0xffff) | CG_L(0);
		smc_state->levels[0].aT = cpu_to_be32(a_t);
		return 0;
	}

	smc_state->levels[0].aT = cpu_to_be32(0);

	for (i = 0; i <= state->performance_level_count - 2; i++) {
		ret = r600_calculate_at(
			(50 / SISLANDS_MAX_HARDWARE_POWERLEVELS) * 100 * (i + 1),
			100 * R600_AH_DFLT,
			state->performance_levels[i + 1].sclk,
			state->performance_levels[i].sclk,
			&t_l,
			&t_h);

		if (ret) {
			t_h = (i + 1) * 1000 - 50 * R600_AH_DFLT;
			t_l = (i + 1) * 1000 + 50 * R600_AH_DFLT;
		}

		a_t = be32_to_cpu(smc_state->levels[i].aT) & ~CG_R_MASK;
		a_t |= CG_R(t_l * pi->bsp / 20000);
		smc_state->levels[i].aT = cpu_to_be32(a_t);

		high_bsp = (i == state->performance_level_count - 2) ?
			pi->pbsp : pi->bsp;
		a_t = CG_R(0xffff) | CG_L(t_h * high_bsp / 20000);
		smc_state->levels[i + 1].aT = cpu_to_be32(a_t);
	}

	return 0;
}

static int si_disable_ulv(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_ulv_param *ulv = &si_pi->ulv;

	/*
	 * amdgpu's si_dpm.c sends DisableULV whether or not ULV is supported,
	 * "to make sure it is properly turned off"; radeon's only when it is.
	 */
	if (ulv->supported || !(rdev->quirks & RDN_SI_DPM_RADEON_RULES))
		return (si_send_msg_to_smc(rdev, PPSMC_MSG_DisableULV) == PPSMC_Result_OK) ?
			0 : -EINVAL;

	return 0;
}

static bool si_is_state_ulv_compatible(struct rdn_si_dpm *rdev,
				       struct si_rps *radeon_state)
{
	const struct si_power_info *si_pi = si_get_pi(rdev);
	const struct si_ulv_param *ulv = &si_pi->ulv;
	const struct ni_ps *state = ni_get_ps(radeon_state);
	int i;

	if (state->performance_levels[0].mclk != ulv->pl.mclk)
		return false;

	/* XXX validate against display requirements! */

	for (i = 0; i < rdev->ci.dyn_state.vddc_dependency_on_dispclk.count; i++) {
		if (rdev->current_dispclk <=
		    rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[i].clk) {
			if (ulv->pl.vddc <
			    rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[i].v)
				return false;
		}
	}

	if ((radeon_state->vclk != 0) || (radeon_state->dclk != 0))
		return false;

	return true;
}

static int si_set_power_state_conditionally_enable_ulv(struct rdn_si_dpm *rdev,
						       struct si_rps *radeon_new_state)
{
	const struct si_power_info *si_pi = si_get_pi(rdev);
	const struct si_ulv_param *ulv = &si_pi->ulv;

	if (ulv->supported) {
		if (si_is_state_ulv_compatible(rdev, radeon_new_state))
			return (si_send_msg_to_smc(rdev, PPSMC_MSG_EnableULV) == PPSMC_Result_OK) ?
				0 : -EINVAL;
	}
	return 0;
}

static int si_convert_power_state_to_smc(struct rdn_si_dpm *rdev,
					 struct si_rps *radeon_state,
					 SISLANDS_SMC_SWSTATE *smc_state)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct ni_ps *state = ni_get_ps(radeon_state);
	int i, ret;
	u32 threshold;
	u32 sclk_in_sr = 1350; /* ??? */

	if (state->performance_level_count > SISLANDS_MAX_HARDWARE_POWERLEVELS)
		return -EINVAL;

	threshold = state->performance_levels[state->performance_level_count-1].sclk * 100 / 100;

	if (radeon_state->vclk && radeon_state->dclk) {
		eg_pi->uvd_enabled = true;
		if (eg_pi->smu_uvd_hs)
			smc_state->flags |= PPSMC_SWSTATE_FLAG_UVD;
	} else {
		eg_pi->uvd_enabled = false;
	}

	if (state->dc_compatible)
		smc_state->flags |= PPSMC_SWSTATE_FLAG_DC;

	smc_state->levelCount = 0;
	for (i = 0; i < state->performance_level_count; i++) {
		if (eg_pi->sclk_deep_sleep) {
			if ((i == 0) || si_pi->sclk_deep_sleep_above_low) {
				if (sclk_in_sr <= SCLK_MIN_DEEPSLEEP_FREQ)
					smc_state->levels[i].stateFlags |= PPSMC_STATEFLAG_DEEPSLEEP_BYPASS;
				else
					smc_state->levels[i].stateFlags |= PPSMC_STATEFLAG_DEEPSLEEP_THROTTLE;
			}
		}

		ret = si_convert_power_level_to_smc(rdev, &state->performance_levels[i],
						    &smc_state->levels[i]);
		smc_state->levels[i].arbRefreshState =
			(u8)(SISLANDS_DRIVER_STATE_ARB_INDEX + i);

		if (ret)
			return ret;

		if (ni_pi->enable_power_containment)
			smc_state->levels[i].displayWatermark =
				(state->performance_levels[i].sclk < threshold) ?
				PPSMC_DISPLAY_WATERMARK_LOW : PPSMC_DISPLAY_WATERMARK_HIGH;
		else
			smc_state->levels[i].displayWatermark = (i < 2) ?
				PPSMC_DISPLAY_WATERMARK_LOW : PPSMC_DISPLAY_WATERMARK_HIGH;

		if (eg_pi->dynamic_ac_timing)
			smc_state->levels[i].ACIndex = SISLANDS_MCREGISTERTABLE_FIRST_DRIVERSTATE_SLOT + i;
		else
			smc_state->levels[i].ACIndex = 0;

		smc_state->levelCount++;
	}

	si_write_smc_soft_register(rdev,
				   SI_SMC_SOFT_REGISTER_watermark_threshold,
				   threshold / 512);

	si_populate_smc_sp(rdev, radeon_state, smc_state);

	ret = si_populate_power_containment_values(rdev, radeon_state, smc_state);
	if (ret)
		ni_pi->enable_power_containment = false;

	ret = si_populate_sq_ramping_values(rdev, radeon_state, smc_state);
	if (ret)
		ni_pi->enable_sq_ramping = false;

	return si_populate_smc_t(rdev, radeon_state, smc_state);
}

static int si_upload_sw_state(struct rdn_si_dpm *rdev,
			      struct si_rps *radeon_new_state)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct ni_ps *new_state = ni_get_ps(radeon_new_state);
	int ret;
	u32 address = si_pi->state_table_start +
		offsetof(SISLANDS_SMC_STATETABLE, driverState);
	SISLANDS_SMC_SWSTATE *smc_state = &si_pi->smc_statetable.driverState;
	size_t state_size = offsetof(SISLANDS_SMC_SWSTATE, levels) +
		sizeof(SISLANDS_SMC_HW_PERFORMANCE_LEVEL) * new_state->performance_level_count;	/* struct_size() */

	memset(smc_state, 0, state_size);

	ret = si_convert_power_state_to_smc(rdev, radeon_new_state, smc_state);
	if (ret)
		return ret;

	ret = si_copy_bytes_to_smc(rdev, address, (u8 *)smc_state,
				   state_size, si_pi->sram_end);

	return ret;
}

static int si_upload_ulv_state(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_ulv_param *ulv = &si_pi->ulv;
	int ret = 0;

	if (ulv->supported && ulv->pl.vddc) {
		u32 address = si_pi->state_table_start +
			offsetof(SISLANDS_SMC_STATETABLE, ULVState);
		struct SISLANDS_SMC_SWSTATE_SINGLE *smc_state = &si_pi->smc_statetable.ULVState;
		u32 state_size = sizeof(struct SISLANDS_SMC_SWSTATE_SINGLE);

		memset(smc_state, 0, state_size);

		ret = si_populate_ulv_state(rdev, smc_state);
		if (!ret)
			ret = si_copy_bytes_to_smc(rdev, address, (u8 *)smc_state,
						   state_size, si_pi->sram_end);
	}

	return ret;
}

/*
 * si_upload_smc_data(): the first active CRTC and its watermark figures come
 * from rdn_si_dpm_display_changed() (Linux: rdev->mode_info.crtcs[i]).
 * Linux compares si_write_smc_soft_register()'s 0 with PPSMC_Result_OK (1),
 * so it returns after the first write: only crtc_index ever reaches the
 * SMC. Kept as Linux runs it.
 */
static int si_upload_smc_data(struct rdn_si_dpm *rdev)
{
	int crtc_id = -1;
	int i;

	/*
	 * amdgpu's si_dpm.c (Linux 7.0) writes all three whatever the display:
	 * the first active CRTC and, from its line time, the lines around
	 * which the SMC keeps the CP off the memory for a memory clock change
	 * (200 us and 100 us of lines; DAL: "used in MCLK SMC switching hang
	 * SW Workaround"), zero without a display. amdgpu divides by a line
	 * time its legacy display code keeps in ns under a _in_us name, which
	 * gives 0 and 0; DC's is in us, which is what is meant and what this
	 * does (the SMC's own defaults are 20 and 10).
	 */
	if (!(rdev->quirks & RDN_SI_DPM_RADEON_RULES)) {
		u32 crtc_index = 0, cp_min = 0, cp_max = 0;

		if (rdev->ci.new_active_crtc_count) {
			for (i = 0; i < rdev->num_crtc; i++) {
				if (rdev->new_active_crtcs & (1 << i)) {
					crtc_index = i;
					break;
				}
			}
			if (rdev->display.line_time) {
				cp_min = 200000 / rdev->display.line_time;
				cp_max = 100000 / rdev->display.line_time;
			}
		}
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_crtc_index, crtc_index);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_mclk_change_block_cp_min, cp_min);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_mclk_change_block_cp_max, cp_max);
		return 0;
	}

	if (rdev->ci.new_active_crtc_count == 0)
		return 0;

	for (i = 0; i < rdev->num_crtc; i++) {
		if (rdev->new_active_crtcs & (1 << i)) {
			crtc_id = i;
			break;
		}
	}

	if (crtc_id < 0)
		return 0;

	if (rdev->display.line_time <= 0)
		return 0;

	if (si_write_smc_soft_register(rdev,
				       SI_SMC_SOFT_REGISTER_crtc_index,
				       crtc_id) != PPSMC_Result_OK)
		return 0;

	if (si_write_smc_soft_register(rdev,
				       SI_SMC_SOFT_REGISTER_mclk_change_block_cp_min,
				       rdev->display.wm_high / rdev->display.line_time) != PPSMC_Result_OK)
		return 0;

	if (si_write_smc_soft_register(rdev,
				       SI_SMC_SOFT_REGISTER_mclk_change_block_cp_max,
				       rdev->display.wm_low / rdev->display.line_time) != PPSMC_Result_OK)
		return 0;

	return 0;
}

static int si_set_mc_special_registers(struct rdn_si_dpm *rdev,
				       struct si_mc_reg_table *table)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	u8 i, j, k;
	u32 temp_reg;

	for (i = 0, j = table->last; i < table->last; i++) {
		if (j >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
			return -EINVAL;
		switch (table->mc_reg_address[i].s1 << 2) {
		case MC_SEQ_MISC1:
			temp_reg = RREG32(MC_PMG_CMD_EMRS);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_EMRS >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_EMRS_LP >> 2;
			for (k = 0; k < table->num_entries; k++)
				table->mc_reg_table_entry[k].mc_data[j] =
					((temp_reg & 0xffff0000)) |
					((table->mc_reg_table_entry[k].mc_data[i] & 0xffff0000) >> 16);
			j++;
			if (j >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;

			temp_reg = RREG32(MC_PMG_CMD_MRS);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_MRS >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_MRS_LP >> 2;
			for (k = 0; k < table->num_entries; k++) {
				table->mc_reg_table_entry[k].mc_data[j] =
					(temp_reg & 0xffff0000) |
					(table->mc_reg_table_entry[k].mc_data[i] & 0x0000ffff);
				if (!pi->mem_gddr5)
					table->mc_reg_table_entry[k].mc_data[j] |= 0x100;
			}
			j++;
			if (j >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;

			if (!pi->mem_gddr5) {
				table->mc_reg_address[j].s1 = MC_PMG_AUTO_CMD >> 2;
				table->mc_reg_address[j].s0 = MC_PMG_AUTO_CMD >> 2;
				for (k = 0; k < table->num_entries; k++)
					table->mc_reg_table_entry[k].mc_data[j] =
						(table->mc_reg_table_entry[k].mc_data[i] & 0xffff0000) >> 16;
				j++;
				if (j >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
					return -EINVAL;
			}
			break;
		case MC_SEQ_RESERVE_M:
			temp_reg = RREG32(MC_PMG_CMD_MRS1);
			table->mc_reg_address[j].s1 = MC_PMG_CMD_MRS1 >> 2;
			table->mc_reg_address[j].s0 = MC_SEQ_PMG_CMD_MRS1_LP >> 2;
			for(k = 0; k < table->num_entries; k++)
				table->mc_reg_table_entry[k].mc_data[j] =
					(temp_reg & 0xffff0000) |
					(table->mc_reg_table_entry[k].mc_data[i] & 0x0000ffff);
			j++;
			if (j >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
				return -EINVAL;
			break;
		default:
			break;
		}
	}

	table->last = j;

	return 0;
}

static bool si_check_s0_mc_reg_index(u16 in_reg, u16 *out_reg)
{
	bool result = true;

	switch (in_reg) {
	case  MC_SEQ_RAS_TIMING >> 2:
		*out_reg = MC_SEQ_RAS_TIMING_LP >> 2;
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

static void si_set_valid_flag(struct si_mc_reg_table *table)
{
	u8 i, j;

	for (i = 0; i < table->last; i++) {
		for (j = 1; j < table->num_entries; j++) {
			if (table->mc_reg_table_entry[j-1].mc_data[i] != table->mc_reg_table_entry[j].mc_data[i]) {
				table->valid_flag |= 1 << i;
				break;
			}
		}
	}
}

static void si_set_s0_mc_reg_index(struct si_mc_reg_table *table)
{
	u32 i;
	u16 address;

	for (i = 0; i < table->last; i++)
		table->mc_reg_address[i].s0 = si_check_s0_mc_reg_index(table->mc_reg_address[i].s1, &address) ?
			address : table->mc_reg_address[i].s1;

}

static int si_copy_vbios_mc_reg_table(struct atom_mc_reg_table *table,
				      struct si_mc_reg_table *si_table)
{
	u8 i, j;

	if (table->last > SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
		return -EINVAL;
	if (table->num_entries > MAX_AC_TIMING_ENTRIES)
		return -EINVAL;

	for (i = 0; i < table->last; i++)
		si_table->mc_reg_address[i].s1 = table->mc_reg_address[i].s1;
	si_table->last = table->last;

	for (i = 0; i < table->num_entries; i++) {
		si_table->mc_reg_table_entry[i].mclk_max =
			table->mc_reg_table_entry[i].mclk_max;
		for (j = 0; j < table->last; j++) {
			si_table->mc_reg_table_entry[i].mc_data[j] =
				table->mc_reg_table_entry[i].mc_data[j];
		}
	}
	si_table->num_entries = table->num_entries;

	return 0;
}

static int si_initialize_mc_reg_table(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct atom_mc_reg_table *table;
	struct si_mc_reg_table *si_table = &si_pi->mc_reg_table;
	u8 module_index = rv770_get_memory_module_index(rdev->card);
	int ret;

	table = si_zalloc(rdev, sizeof(struct atom_mc_reg_table));
	if (!table)
		return -ENOMEM;

	WREG32(MC_SEQ_RAS_TIMING_LP, RREG32(MC_SEQ_RAS_TIMING));
	WREG32(MC_SEQ_CAS_TIMING_LP, RREG32(MC_SEQ_CAS_TIMING));
	WREG32(MC_SEQ_MISC_TIMING_LP, RREG32(MC_SEQ_MISC_TIMING));
	WREG32(MC_SEQ_MISC_TIMING2_LP, RREG32(MC_SEQ_MISC_TIMING2));
	WREG32(MC_SEQ_PMG_CMD_EMRS_LP, RREG32(MC_PMG_CMD_EMRS));
	WREG32(MC_SEQ_PMG_CMD_MRS_LP, RREG32(MC_PMG_CMD_MRS));
	WREG32(MC_SEQ_PMG_CMD_MRS1_LP, RREG32(MC_PMG_CMD_MRS1));
	WREG32(MC_SEQ_WR_CTL_D0_LP, RREG32(MC_SEQ_WR_CTL_D0));
	WREG32(MC_SEQ_WR_CTL_D1_LP, RREG32(MC_SEQ_WR_CTL_D1));
	WREG32(MC_SEQ_RD_CTL_D0_LP, RREG32(MC_SEQ_RD_CTL_D0));
	WREG32(MC_SEQ_RD_CTL_D1_LP, RREG32(MC_SEQ_RD_CTL_D1));
	WREG32(MC_SEQ_PMG_TIMING_LP, RREG32(MC_SEQ_PMG_TIMING));
	WREG32(MC_SEQ_PMG_CMD_MRS2_LP, RREG32(MC_PMG_CMD_MRS2));
	WREG32(MC_SEQ_WR_CTL_2_LP, RREG32(MC_SEQ_WR_CTL_2));

	ret = radeon_atom_init_mc_reg_table(rdev->card, module_index, table);
	if (ret)
		goto init_mc_done;

	ret = si_copy_vbios_mc_reg_table(table, si_table);
	if (ret)
		goto init_mc_done;

	si_set_s0_mc_reg_index(si_table);

	ret = si_set_mc_special_registers(rdev, si_table);
	if (ret)
		goto init_mc_done;

	si_set_valid_flag(si_table);

init_mc_done:
	si_free(rdev, table);

	return ret;

}

static void si_populate_mc_reg_addresses(struct rdn_si_dpm *rdev,
					 SMC_SIslands_MCRegisters *mc_reg_table)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 i, j;

	for (i = 0, j = 0; j < si_pi->mc_reg_table.last; j++) {
		if (si_pi->mc_reg_table.valid_flag & (1 << j)) {
			if (i >= SMC_SISLANDS_MC_REGISTER_ARRAY_SIZE)
				break;
			mc_reg_table->address[i].s0 =
				cpu_to_be16(si_pi->mc_reg_table.mc_reg_address[j].s0);
			mc_reg_table->address[i].s1 =
				cpu_to_be16(si_pi->mc_reg_table.mc_reg_address[j].s1);
			i++;
		}
	}
	mc_reg_table->last = (u8)i;
}

static void si_convert_mc_registers(const struct si_mc_reg_entry *entry,
				    SMC_SIslands_MCRegisterSet *data,
				    u32 num_entries, u32 valid_flag)
{
	u32 i, j;

	for(i = 0, j = 0; j < num_entries; j++) {
		if (valid_flag & (1 << j)) {
			data->value[i] = cpu_to_be32(entry->mc_data[j]);
			i++;
		}
	}
}

static void si_convert_mc_reg_table_entry_to_smc(struct rdn_si_dpm *rdev,
						 struct rv7xx_pl *pl,
						 SMC_SIslands_MCRegisterSet *mc_reg_table_data)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 i = 0;

	for (i = 0; i < si_pi->mc_reg_table.num_entries; i++) {
		if (pl->mclk <= si_pi->mc_reg_table.mc_reg_table_entry[i].mclk_max)
			break;
	}

	if ((i == si_pi->mc_reg_table.num_entries) && (i > 0))
		--i;

	si_convert_mc_registers(&si_pi->mc_reg_table.mc_reg_table_entry[i],
				mc_reg_table_data, si_pi->mc_reg_table.last,
				si_pi->mc_reg_table.valid_flag);
}

static void si_convert_mc_reg_table_to_smc(struct rdn_si_dpm *rdev,
					   struct si_rps *radeon_state,
					   SMC_SIslands_MCRegisters *mc_reg_table)
{
	struct ni_ps *state = ni_get_ps(radeon_state);
	int i;

	for (i = 0; i < state->performance_level_count; i++) {
		si_convert_mc_reg_table_entry_to_smc(rdev,
						     &state->performance_levels[i],
						     &mc_reg_table->data[SISLANDS_MCREGISTERTABLE_FIRST_DRIVERSTATE_SLOT + i]);
	}
}

static int si_populate_mc_reg_table(struct rdn_si_dpm *rdev,
				    struct si_rps *radeon_boot_state)
{
	struct ni_ps *boot_state = ni_get_ps(radeon_boot_state);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_ulv_param *ulv = &si_pi->ulv;
	SMC_SIslands_MCRegisters *smc_mc_reg_table = &si_pi->smc_mc_reg_table;

	memset(smc_mc_reg_table, 0, sizeof(SMC_SIslands_MCRegisters));

	si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_seq_index, 1);

	si_populate_mc_reg_addresses(rdev, smc_mc_reg_table);

	si_convert_mc_reg_table_entry_to_smc(rdev, &boot_state->performance_levels[0],
					     &smc_mc_reg_table->data[SISLANDS_MCREGISTERTABLE_INITIAL_SLOT]);

	si_convert_mc_registers(&si_pi->mc_reg_table.mc_reg_table_entry[0],
				&smc_mc_reg_table->data[SISLANDS_MCREGISTERTABLE_ACPI_SLOT],
				si_pi->mc_reg_table.last,
				si_pi->mc_reg_table.valid_flag);

	if (ulv->supported && ulv->pl.vddc != 0)
		si_convert_mc_reg_table_entry_to_smc(rdev, &ulv->pl,
						     &smc_mc_reg_table->data[SISLANDS_MCREGISTERTABLE_ULV_SLOT]);
	else
		si_convert_mc_registers(&si_pi->mc_reg_table.mc_reg_table_entry[0],
					&smc_mc_reg_table->data[SISLANDS_MCREGISTERTABLE_ULV_SLOT],
					si_pi->mc_reg_table.last,
					si_pi->mc_reg_table.valid_flag);

	si_convert_mc_reg_table_to_smc(rdev, radeon_boot_state, smc_mc_reg_table);

	return si_copy_bytes_to_smc(rdev, si_pi->mc_reg_table_start,
				    (u8 *)smc_mc_reg_table,
				    sizeof(SMC_SIslands_MCRegisters), si_pi->sram_end);
}

static int si_upload_mc_reg_table(struct rdn_si_dpm *rdev,
				  struct si_rps *radeon_new_state)
{
	struct ni_ps *new_state = ni_get_ps(radeon_new_state);
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 address = si_pi->mc_reg_table_start +
		offsetof(SMC_SIslands_MCRegisters,
			 data[SISLANDS_MCREGISTERTABLE_FIRST_DRIVERSTATE_SLOT]);
	SMC_SIslands_MCRegisters *smc_mc_reg_table = &si_pi->smc_mc_reg_table;

	memset(smc_mc_reg_table, 0, sizeof(SMC_SIslands_MCRegisters));

	si_convert_mc_reg_table_to_smc(rdev, radeon_new_state, smc_mc_reg_table);


	return si_copy_bytes_to_smc(rdev, address,
				    (u8 *)&smc_mc_reg_table->data[SISLANDS_MCREGISTERTABLE_FIRST_DRIVERSTATE_SLOT],
				    sizeof(SMC_SIslands_MCRegisterSet) * new_state->performance_level_count,
				    si_pi->sram_end);

}

static void si_enable_voltage_control(struct rdn_si_dpm *rdev, bool enable)
{
	if (enable)
		WREG32_P(GENERAL_PWRMGT, VOLT_PWRMGT_EN, ~VOLT_PWRMGT_EN);
	else
		WREG32_P(GENERAL_PWRMGT, 0, ~VOLT_PWRMGT_EN);
}

static enum rdn_ci_pcie_gen si_get_maximum_link_speed(struct rdn_si_dpm *rdev,
						      struct si_rps *radeon_state)
{
	struct ni_ps *state = ni_get_ps(radeon_state);
	int i;
	u16 pcie_speed, max_speed = 0;

	for (i = 0; i < state->performance_level_count; i++) {
		pcie_speed = state->performance_levels[i].pcie_gen;
		if (max_speed < pcie_speed)
			max_speed = pcie_speed;
	}
	return max_speed;
}

static u16 si_get_current_pcie_speed(struct rdn_si_dpm *rdev)
{
	u32 speed_cntl;

	speed_cntl = RREG32_PCIE_PORT(PCIE_LC_SPEED_CNTL) & LC_CURRENT_DATA_RATE_MASK;
	speed_cntl >>= LC_CURRENT_DATA_RATE_SHIFT;

	return (u16)speed_cntl;
}

static void si_request_link_speed_change_before_state_change(struct rdn_si_dpm *rdev,
							     struct si_rps *radeon_new_state,
							     struct si_rps *radeon_current_state)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	enum rdn_ci_pcie_gen target_link_speed = si_get_maximum_link_speed(rdev, radeon_new_state);
	enum rdn_ci_pcie_gen current_link_speed;

	if (si_pi->force_pcie_gen == RDN_CI_PCIE_GEN_INVALID)
		current_link_speed = si_get_maximum_link_speed(rdev, radeon_current_state);
	else
		current_link_speed = si_pi->force_pcie_gen;

	si_pi->force_pcie_gen = RDN_CI_PCIE_GEN_INVALID;
	si_pi->pspp_notify_required = false;
	if (target_link_speed > current_link_speed) {
		switch (target_link_speed) {
#if defined(CONFIG_ACPI)
		case RDN_CI_PCIE_GEN3:
			if (radeon_acpi_pcie_performance_request(rdev, PCIE_PERF_REQ_PECI_GEN3, false) == 0)
				break;
			si_pi->force_pcie_gen = RDN_CI_PCIE_GEN2;
			if (current_link_speed == RDN_CI_PCIE_GEN2)
				break;
			fallthrough;
		case RDN_CI_PCIE_GEN2:
			if (radeon_acpi_pcie_performance_request(rdev, PCIE_PERF_REQ_PECI_GEN2, false) == 0)
				break;
			fallthrough;
#endif
		default:
			si_pi->force_pcie_gen = si_get_current_pcie_speed(rdev);
			break;
		}
	} else {
		if (target_link_speed < current_link_speed)
			si_pi->pspp_notify_required = true;
	}
}

static void si_notify_link_speed_change_after_state_change(struct rdn_si_dpm *rdev,
							   struct si_rps *radeon_new_state,
							   struct si_rps *radeon_current_state)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	enum rdn_ci_pcie_gen target_link_speed = si_get_maximum_link_speed(rdev, radeon_new_state);
	u8 request;

	if (si_pi->pspp_notify_required) {
		if (target_link_speed == RDN_CI_PCIE_GEN3)
			request = PCIE_PERF_REQ_PECI_GEN3;
		else if (target_link_speed == RDN_CI_PCIE_GEN2)
			request = PCIE_PERF_REQ_PECI_GEN2;
		else
			request = PCIE_PERF_REQ_PECI_GEN1;

		if ((request == PCIE_PERF_REQ_PECI_GEN1) &&
		    (si_get_current_pcie_speed(rdev) > 0))
			return;

#if defined(CONFIG_ACPI)
		radeon_acpi_pcie_performance_request(rdev, request, false);
#endif
	}
}

#if 0
static int si_ds_request(struct rdn_si_dpm *rdev,
			 bool ds_status_on, u32 count_write)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);

	if (eg_pi->sclk_deep_sleep) {
		if (ds_status_on)
			return (si_send_msg_to_smc(rdev, PPSMC_MSG_CancelThrottleOVRDSCLKDS) ==
				PPSMC_Result_OK) ?
				0 : -EINVAL;
		else
			return (si_send_msg_to_smc(rdev, PPSMC_MSG_ThrottleOVRDSCLKDS) ==
				PPSMC_Result_OK) ? 0 : -EINVAL;
	}
	return 0;
}
#endif

static void si_set_max_cu_value(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);

	if (rdev->family == CHIP_VERDE) {
		switch (rdev->device) {
		case 0x6820:
		case 0x6825:
		case 0x6821:
		case 0x6823:
		case 0x6827:
			si_pi->max_cu = 10;
			break;
		case 0x682D:
		case 0x6824:
		case 0x682F:
		case 0x6826:
			si_pi->max_cu = 8;
			break;
		case 0x6828:
		case 0x6830:
		case 0x6831:
		case 0x6838:
		case 0x6839:
		case 0x683D:
			si_pi->max_cu = 10;
			break;
		case 0x683B:
		case 0x683F:
		case 0x6829:
			si_pi->max_cu = 8;
			break;
		default:
			si_pi->max_cu = 0;
			break;
		}
	} else {
		si_pi->max_cu = 0;
	}
}

static int si_patch_single_dependency_table_based_on_leakage(struct rdn_si_dpm *rdev,
							     struct rdn_ci_dep_table *table)
{
	u32 i;
	int j;
	u16 leakage_voltage;

	if (table) {
		for (i = 0; i < table->count; i++) {
			switch (si_get_leakage_voltage_from_leakage_index(rdev,
									  table->entries[i].v,
									  &leakage_voltage)) {
			case 0:
				table->entries[i].v = leakage_voltage;
				break;
			case -EAGAIN:
				return -EINVAL;
			case -EINVAL:
			default:
				break;
			}
		}

		for (j = (table->count - 2); j >= 0; j--) {
			table->entries[j].v = (table->entries[j].v <= table->entries[j + 1].v) ?
				table->entries[j].v : table->entries[j + 1].v;
		}
	}
	return 0;
}

static int si_patch_dependency_tables_based_on_leakage(struct rdn_si_dpm *rdev)
{
	int ret;

	ret = si_patch_single_dependency_table_based_on_leakage(rdev,
								&rdev->ci.dyn_state.vddc_dependency_on_sclk);
	ret = si_patch_single_dependency_table_based_on_leakage(rdev,
								&rdev->ci.dyn_state.vddc_dependency_on_mclk);
	ret = si_patch_single_dependency_table_based_on_leakage(rdev,
								&rdev->ci.dyn_state.vddci_dependency_on_mclk);
	return ret;
}

static void si_set_pcie_lane_width_in_smc(struct rdn_si_dpm *rdev,
					  struct si_rps *radeon_new_state,
					  struct si_rps *radeon_current_state)
{
	u32 lane_width;
	u32 new_lane_width =
		((radeon_new_state->caps & ATOM_PPLIB_PCIE_LINK_WIDTH_MASK) >> ATOM_PPLIB_PCIE_LINK_WIDTH_SHIFT) + 1;
	u32 current_lane_width =
		((radeon_current_state->caps & ATOM_PPLIB_PCIE_LINK_WIDTH_MASK) >> ATOM_PPLIB_PCIE_LINK_WIDTH_SHIFT) + 1;

	if (new_lane_width != current_lane_width) {
		radeon_set_pcie_lanes(rdev, new_lane_width);
		lane_width = radeon_get_pcie_lanes(rdev);
		si_write_smc_soft_register(rdev, SI_SMC_SOFT_REGISTER_non_ulv_pcie_link_width, lane_width);
	}
}

/* si_set_vce_clock(): VCE is not brought up on Southern Islands here; its clocks stay as POST left them. */
static void si_set_vce_clock(struct rdn_si_dpm *rdev,
			     struct si_rps *new_rps,
			     struct si_rps *old_rps)
{
	if ((old_rps->evclk != new_rps->evclk) ||
	    (old_rps->ecclk != new_rps->ecclk))
		SI_DEBUG(rdev, "VCE clocks %u/%u requested; not driven",
			 (unsigned)new_rps->evclk, (unsigned)new_rps->ecclk);
}

static void si_dpm_setup_asic(struct rdn_si_dpm *rdev)
{
	/* si_mc_load_microcode(): the MC firmware was loaded at POST (rdn_si.c) */
	rv770_get_memory_type(rdev);
	si_read_clock_registers(rdev);
	si_enable_acpi_power_management(rdev);
}

static int si_thermal_enable_alert(struct rdn_si_dpm *rdev,
				   bool enable)
{
	u32 thermal_int = RREG32(CG_THERMAL_INT);

	if (enable) {
		PPSMC_Result result;

		thermal_int &= ~(THERM_INT_MASK_HIGH | THERM_INT_MASK_LOW);
		WREG32(CG_THERMAL_INT, thermal_int);
		result = si_send_msg_to_smc(rdev, PPSMC_MSG_EnableThermalInterrupt);
		if (result != PPSMC_Result_OK) {
			SI_DEBUG(rdev, "Could not enable thermal interrupts.\n");
			return -EINVAL;
		}
	} else {
		thermal_int |= THERM_INT_MASK_HIGH | THERM_INT_MASK_LOW;
		WREG32(CG_THERMAL_INT, thermal_int);
	}

	return 0;
}

static int si_thermal_set_temperature_range(struct rdn_si_dpm *rdev,
					    int min_temp, int max_temp)
{
	int low_temp = 0 * 1000;
	int high_temp = 255 * 1000;

	if (low_temp < min_temp)
		low_temp = min_temp;
	if (high_temp > max_temp)
		high_temp = max_temp;
	if (high_temp < low_temp) {
		SI_ERROR(rdev, "invalid thermal range: %d - %d\n", low_temp, high_temp);
		return -EINVAL;
	}

	WREG32_P(CG_THERMAL_INT, DIG_THERM_INTH(high_temp / 1000), ~DIG_THERM_INTH_MASK);
	WREG32_P(CG_THERMAL_INT, DIG_THERM_INTL(low_temp / 1000), ~DIG_THERM_INTL_MASK);
	WREG32_P(CG_THERMAL_CTRL, DIG_THERM_DPM(high_temp / 1000), ~DIG_THERM_DPM_MASK);

	rdev->ci.thermal_min_temp = low_temp;
	rdev->ci.thermal_max_temp = high_temp;

	return 0;
}

static void si_fan_ctrl_set_static_mode(struct rdn_si_dpm *rdev, u32 mode)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;

	if (si_pi->fan_ctrl_is_in_default_mode) {
		tmp = (RREG32(CG_FDO_CTRL2) & FDO_PWM_MODE_MASK) >> FDO_PWM_MODE_SHIFT;
		si_pi->fan_ctrl_default_mode = tmp;
		tmp = (RREG32(CG_FDO_CTRL2) & TMIN_MASK) >> TMIN_SHIFT;
		si_pi->t_min = tmp;
		si_pi->fan_ctrl_is_in_default_mode = false;
	}

	tmp = RREG32(CG_FDO_CTRL2) & ~TMIN_MASK;
	tmp |= TMIN(0);
	WREG32(CG_FDO_CTRL2, tmp);

	tmp = RREG32(CG_FDO_CTRL2) & ~FDO_PWM_MODE_MASK;
	tmp |= FDO_PWM_MODE(mode);
	WREG32(CG_FDO_CTRL2, tmp);
}

static int si_thermal_setup_fan_table(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	PP_SIslands_FanTable fan_table = { FDO_MODE_HARDWARE };
	u32 duty100;
	u32 t_diff1, t_diff2, pwm_diff1, pwm_diff2;
	u16 fdo_min, slope1, slope2;
	u32 reference_clock, tmp;
	int ret;
	u64 tmp64;

	if (!si_pi->fan_table_start) {
		rdev->ci.fan.ucode_fan_control = false;
		return 0;
	}

	duty100 = (RREG32(CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;

	if (duty100 == 0) {
		rdev->ci.fan.ucode_fan_control = false;
		return 0;
	}

	tmp64 = (u64)rdev->ci.fan.pwm_min * duty100;
	do_div(tmp64, 10000);
	fdo_min = (u16)tmp64;

	t_diff1 = rdev->ci.fan.t_med - rdev->ci.fan.t_min;
	t_diff2 = rdev->ci.fan.t_high - rdev->ci.fan.t_med;

	pwm_diff1 = rdev->ci.fan.pwm_med - rdev->ci.fan.pwm_min;
	pwm_diff2 = rdev->ci.fan.pwm_high - rdev->ci.fan.pwm_med;

	slope1 = (u16)((50 + ((16 * duty100 * pwm_diff1) / t_diff1)) / 100);
	slope2 = (u16)((50 + ((16 * duty100 * pwm_diff2) / t_diff2)) / 100);

	fan_table.temp_min = cpu_to_be16((50 + rdev->ci.fan.t_min) / 100);
	fan_table.temp_med = cpu_to_be16((50 + rdev->ci.fan.t_med) / 100);
	fan_table.temp_max = cpu_to_be16((50 + rdev->ci.fan.t_max) / 100);

	fan_table.slope1 = cpu_to_be16(slope1);
	fan_table.slope2 = cpu_to_be16(slope2);

	fan_table.fdo_min = cpu_to_be16(fdo_min);

	fan_table.hys_down = cpu_to_be16(rdev->ci.fan.t_hyst);

	fan_table.hys_up = cpu_to_be16(1);

	fan_table.hys_slope = cpu_to_be16(1);

	fan_table.temp_resp_lim = cpu_to_be16(5);

	reference_clock = si_get_xclk(rdev);

	fan_table.refresh_period = cpu_to_be32((rdev->ci.fan.cycle_delay *
						reference_clock) / 1600);

	fan_table.fdo_max = cpu_to_be16((u16)duty100);

	tmp = (RREG32(CG_MULT_THERMAL_CTRL) & TEMP_SEL_MASK) >> TEMP_SEL_SHIFT;
	fan_table.temp_src = (uint8_t)tmp;

	ret = si_copy_bytes_to_smc(rdev,
				   si_pi->fan_table_start,
				   (u8 *)(&fan_table),
				   sizeof(fan_table),
				   si_pi->sram_end);

	if (ret) {
		SI_ERROR(rdev, "Failed to load fan table to the SMC.");
		rdev->ci.fan.ucode_fan_control = false;
	}

	return 0;
}

static int si_fan_ctrl_start_smc_fan_control(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	PPSMC_Result ret;

	ret = si_send_msg_to_smc(rdev, PPSMC_StartFanControl);
	if (ret == PPSMC_Result_OK) {
		si_pi->fan_is_controlled_by_smc = true;
		return 0;
	} else {
		return -EINVAL;
	}
}

static int si_fan_ctrl_stop_smc_fan_control(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	PPSMC_Result ret;

	ret = si_send_msg_to_smc(rdev, PPSMC_StopFanControl);

	if (ret == PPSMC_Result_OK) {
		si_pi->fan_is_controlled_by_smc = false;
		return 0;
	} else {
		return -EINVAL;
	}
}

static int si_fan_ctrl_get_fan_speed_percent(struct rdn_si_dpm *rdev,
				      u32 *speed)
{
	u32 duty, duty100;
	u64 tmp64;

	if (rdev->ci.no_fan)
		return -ENOENT;

	duty100 = (RREG32(CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;
	duty = (RREG32(CG_THERMAL_STATUS) & FDO_PWM_DUTY_MASK) >> FDO_PWM_DUTY_SHIFT;

	if (duty100 == 0)
		return -EINVAL;

	tmp64 = (u64)duty * 100;
	do_div(tmp64, duty100);
	*speed = (u32)tmp64;

	if (*speed > 100)
		*speed = 100;

	return 0;
}

static int si_fan_ctrl_set_fan_speed_percent(struct rdn_si_dpm *rdev,
				      u32 speed)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;
	u32 duty, duty100;
	u64 tmp64;

	if (rdev->ci.no_fan)
		return -ENOENT;

	if (si_pi->fan_is_controlled_by_smc)
		return -EINVAL;

	if (speed > 100)
		return -EINVAL;

	duty100 = (RREG32(CG_FDO_CTRL1) & FMAX_DUTY100_MASK) >> FMAX_DUTY100_SHIFT;

	if (duty100 == 0)
		return -EINVAL;

	tmp64 = (u64)speed * duty100;
	do_div(tmp64, 100);
	duty = (u32)tmp64;

	tmp = RREG32(CG_FDO_CTRL0) & ~FDO_STATIC_DUTY_MASK;
	tmp |= FDO_STATIC_DUTY(duty);
	WREG32(CG_FDO_CTRL0, tmp);

	return 0;
}

static void si_fan_ctrl_set_mode(struct rdn_si_dpm *rdev, u32 mode)
{
	if (mode) {
		/* stop auto-manage */
		if (rdev->ci.fan.ucode_fan_control)
			si_fan_ctrl_stop_smc_fan_control(rdev);
		si_fan_ctrl_set_static_mode(rdev, mode);
	} else {
		/* restart auto-manage */
		if (rdev->ci.fan.ucode_fan_control)
			si_thermal_start_smc_fan_control(rdev);
		else
			si_fan_ctrl_set_default_mode(rdev);
	}
}

static u32 si_fan_ctrl_get_mode(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;

	if (si_pi->fan_is_controlled_by_smc)
		return 0;

	tmp = RREG32(CG_FDO_CTRL2) & FDO_PWM_MODE_MASK;
	return (tmp >> FDO_PWM_MODE_SHIFT);
}

#if 0
static int si_fan_ctrl_get_fan_speed_rpm(struct rdn_si_dpm *rdev,
					 u32 *speed)
{
	u32 tach_period;
	u32 xclk = si_get_xclk(rdev);

	if (rdev->ci.no_fan)
		return -ENOENT;

	if (rdev->ci.fan_pulses_per_revolution == 0)
		return -ENOENT;

	tach_period = (RREG32(CG_TACH_STATUS) & TACH_PERIOD_MASK) >> TACH_PERIOD_SHIFT;
	if (tach_period == 0)
		return -ENOENT;

	*speed = 60 * xclk * 10000 / tach_period;

	return 0;
}

static int si_fan_ctrl_set_fan_speed_rpm(struct rdn_si_dpm *rdev,
					 u32 speed)
{
	u32 tach_period, tmp;
	u32 xclk = si_get_xclk(rdev);

	if (rdev->ci.no_fan)
		return -ENOENT;

	if (rdev->ci.fan_pulses_per_revolution == 0)
		return -ENOENT;

	if ((speed < rdev->ci.fan_min_rpm) ||
	    (speed > rdev->ci.fan_max_rpm))
		return -EINVAL;

	if (rdev->ci.fan.ucode_fan_control)
		si_fan_ctrl_stop_smc_fan_control(rdev);

	tach_period = 60 * xclk * 10000 / (8 * speed);
	tmp = RREG32(CG_TACH_CTRL) & ~TARGET_PERIOD_MASK;
	tmp |= TARGET_PERIOD(tach_period);
	WREG32(CG_TACH_CTRL, tmp);

	si_fan_ctrl_set_static_mode(rdev, FDO_PWM_MODE_STATIC_RPM);

	return 0;
}
#endif

static void si_fan_ctrl_set_default_mode(struct rdn_si_dpm *rdev)
{
	struct si_power_info *si_pi = si_get_pi(rdev);
	u32 tmp;

	if (!si_pi->fan_ctrl_is_in_default_mode) {
		tmp = RREG32(CG_FDO_CTRL2) & ~FDO_PWM_MODE_MASK;
		tmp |= FDO_PWM_MODE(si_pi->fan_ctrl_default_mode);
		WREG32(CG_FDO_CTRL2, tmp);

		tmp = RREG32(CG_FDO_CTRL2) & ~TMIN_MASK;
		tmp |= TMIN(si_pi->t_min);
		WREG32(CG_FDO_CTRL2, tmp);
		si_pi->fan_ctrl_is_in_default_mode = true;
	}
}

static void si_thermal_start_smc_fan_control(struct rdn_si_dpm *rdev)
{
	if (rdev->ci.fan.ucode_fan_control) {
		si_fan_ctrl_start_smc_fan_control(rdev);
		si_fan_ctrl_set_static_mode(rdev, FDO_PWM_MODE_STATIC);
	}
}

static void si_thermal_initialize(struct rdn_si_dpm *rdev)
{
	u32 tmp;

	if (rdev->ci.fan_pulses_per_revolution) {
		tmp = RREG32(CG_TACH_CTRL) & ~EDGE_PER_REV_MASK;
		tmp |= EDGE_PER_REV(rdev->ci.fan_pulses_per_revolution -1);
		WREG32(CG_TACH_CTRL, tmp);
	}

	tmp = RREG32(CG_FDO_CTRL2) & ~TACH_PWM_RESP_RATE_MASK;
	tmp |= TACH_PWM_RESP_RATE(0x28);
	WREG32(CG_FDO_CTRL2, tmp);
}

static int si_thermal_start_thermal_controller(struct rdn_si_dpm *rdev)
{
	int ret;

	si_thermal_initialize(rdev);
	ret = si_thermal_set_temperature_range(rdev, R600_TEMP_RANGE_MIN, R600_TEMP_RANGE_MAX);
	if (ret)
		return ret;
	ret = si_thermal_enable_alert(rdev, true);
	if (ret)
		return ret;
	if (rdev->ci.fan.ucode_fan_control) {
		ret = si_halt_smc(rdev);
		if (ret)
			return ret;
		ret = si_thermal_setup_fan_table(rdev);
		if (ret)
			return ret;
		ret = si_resume_smc(rdev);
		if (ret)
			return ret;
		si_thermal_start_smc_fan_control(rdev);
	}

	return 0;
}

static void si_thermal_stop_thermal_controller(struct rdn_si_dpm *rdev)
{
	if (!rdev->ci.no_fan) {
		si_fan_ctrl_set_default_mode(rdev);
		si_fan_ctrl_stop_smc_fan_control(rdev);
	}
}

static int si_dpm_enable(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct si_rps *boot_ps = rdev->boot_ps;
	int ret;

	if (si_is_smc_running(rdev))
		return -EINVAL;
	if (pi->voltage_control || si_pi->voltage_control_svi2)
		si_enable_voltage_control(rdev, true);
	if (pi->mvdd_control)
		si_get_mvdd_configuration(rdev);
	if (pi->voltage_control || si_pi->voltage_control_svi2) {
		ret = si_construct_voltage_tables(rdev);
		if (ret) {
			SI_ERROR(rdev, "si_construct_voltage_tables failed\n");
			return ret;
		}
	}
	if (eg_pi->dynamic_ac_timing) {
		ret = si_initialize_mc_reg_table(rdev);
		if (ret)
			eg_pi->dynamic_ac_timing = false;
	}
	if (pi->dynamic_ss)
		si_enable_spread_spectrum(rdev, true);
	if (pi->thermal_protection)
		si_enable_thermal_protection(rdev, true);
	si_setup_bsp(rdev);
	si_program_git(rdev);
	si_program_tp(rdev);
	si_program_tpp(rdev);
	si_program_sstp(rdev);
	si_enable_display_gap(rdev);
	si_program_vc(rdev);
	ret = si_upload_firmware(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_upload_firmware failed\n");
		return ret;
	}
	ret = si_process_firmware_header(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_process_firmware_header failed\n");
		return ret;
	}
	ret = si_initial_switch_from_arb_f0_to_f1(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_initial_switch_from_arb_f0_to_f1 failed\n");
		return ret;
	}
	ret = si_init_smc_table(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_init_smc_table failed\n");
		return ret;
	}
	ret = si_init_smc_spll_table(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_init_smc_spll_table failed\n");
		return ret;
	}
	ret = si_init_arb_table_index(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_init_arb_table_index failed\n");
		return ret;
	}
	if (eg_pi->dynamic_ac_timing) {
		ret = si_populate_mc_reg_table(rdev, boot_ps);
		if (ret) {
			SI_ERROR(rdev, "si_populate_mc_reg_table failed\n");
			return ret;
		}
	}
	ret = si_initialize_smc_cac_tables(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_initialize_smc_cac_tables failed\n");
		return ret;
	}
	ret = si_initialize_hardware_cac_manager(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_initialize_hardware_cac_manager failed\n");
		return ret;
	}
	ret = si_initialize_smc_dte_tables(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_initialize_smc_dte_tables failed\n");
		return ret;
	}
	ret = si_populate_smc_tdp_limits(rdev, boot_ps);
	if (ret) {
		SI_ERROR(rdev, "si_populate_smc_tdp_limits failed\n");
		return ret;
	}
	ret = si_populate_smc_tdp_limits_2(rdev, boot_ps);
	if (ret) {
		SI_ERROR(rdev, "si_populate_smc_tdp_limits_2 failed\n");
		return ret;
	}
	si_program_response_times(rdev);
	si_program_ds_registers(rdev);
	si_dpm_start_smc(rdev);
	ret = si_notify_smc_display_change(rdev, false);
	if (ret) {
		SI_ERROR(rdev, "si_notify_smc_display_change failed\n");
		return ret;
	}
	si_enable_sclk_control(rdev, true);
	si_start_dpm(rdev);

	si_enable_auto_throttle_source(rdev, RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL, true);

	si_thermal_start_thermal_controller(rdev);

	ni_update_current_ps(rdev, boot_ps);

	return 0;
}

static int si_set_temperature_range(struct rdn_si_dpm *rdev)
{
	int ret;

	ret = si_thermal_enable_alert(rdev, false);
	if (ret)
		return ret;
	ret = si_thermal_set_temperature_range(rdev, R600_TEMP_RANGE_MIN, R600_TEMP_RANGE_MAX);
	if (ret)
		return ret;
	ret = si_thermal_enable_alert(rdev, true);
	if (ret)
		return ret;

	return ret;
}

static int si_dpm_late_enable(struct rdn_si_dpm *rdev)
{
	int ret;

	ret = si_set_temperature_range(rdev);
	if (ret)
		return ret;

	return ret;
}

static void si_dpm_disable(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct si_rps *boot_ps = rdev->boot_ps;

	if (!si_is_smc_running(rdev))
		return;
	si_thermal_stop_thermal_controller(rdev);
	si_disable_ulv(rdev);
	si_clear_vc(rdev);
	if (pi->thermal_protection)
		si_enable_thermal_protection(rdev, false);
	si_enable_power_containment(rdev, boot_ps, false);
	si_enable_smc_cac(rdev, boot_ps, false);
	si_enable_spread_spectrum(rdev, false);
	si_enable_auto_throttle_source(rdev, RADEON_DPM_AUTO_THROTTLE_SRC_THERMAL, false);
	si_stop_dpm(rdev);
	si_reset_to_default(rdev);
	si_dpm_stop_smc(rdev);
	si_force_switch_to_arb_f0(rdev);

	ni_update_current_ps(rdev, boot_ps);
}

static int si_dpm_pre_set_power_state(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_rps requested_ps = *rdev->requested_ps;
	struct si_rps *new_ps = &requested_ps;

	ni_update_requested_ps(rdev, new_ps);

	si_apply_state_adjust_rules(rdev, &eg_pi->requested_rps);

	return 0;
}

static int si_power_control_set_level(struct rdn_si_dpm *rdev)
{
	struct si_rps *new_ps = rdev->requested_ps;
	int ret;

	ret = si_restrict_performance_levels_before_switch(rdev);
	if (ret)
		return ret;
	ret = si_halt_smc(rdev);
	if (ret)
		return ret;
	ret = si_populate_smc_tdp_limits(rdev, new_ps);
	if (ret)
		return ret;
	ret = si_populate_smc_tdp_limits_2(rdev, new_ps);
	if (ret)
		return ret;
	ret = si_resume_smc(rdev);
	if (ret)
		return ret;
	ret = si_set_sw_state(rdev);
	if (ret)
		return ret;
	return 0;
}

static int si_dpm_set_power_state(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_rps *new_ps = &eg_pi->requested_rps;
	struct si_rps *old_ps = &eg_pi->current_rps;
	int ret;

	ret = si_disable_ulv(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_disable_ulv failed\n");
		return ret;
	}
	ret = si_restrict_performance_levels_before_switch(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_restrict_performance_levels_before_switch failed\n");
		return ret;
	}
	if (eg_pi->pcie_performance_request)
		si_request_link_speed_change_before_state_change(rdev, new_ps, old_ps);
	ni_set_uvd_clock_before_set_eng_clock(rdev, new_ps, old_ps);
	ret = si_enable_power_containment(rdev, new_ps, false);
	if (ret) {
		SI_ERROR(rdev, "si_enable_power_containment failed\n");
		return ret;
	}
	ret = si_enable_smc_cac(rdev, new_ps, false);
	if (ret) {
		SI_ERROR(rdev, "si_enable_smc_cac failed\n");
		return ret;
	}
	ret = si_halt_smc(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_halt_smc failed\n");
		return ret;
	}
	ret = si_upload_sw_state(rdev, new_ps);
	if (ret) {
		SI_ERROR(rdev, "si_upload_sw_state failed\n");
		return ret;
	}
	ret = si_upload_smc_data(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_upload_smc_data failed\n");
		return ret;
	}
	ret = si_upload_ulv_state(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_upload_ulv_state failed\n");
		return ret;
	}
	if (eg_pi->dynamic_ac_timing) {
		ret = si_upload_mc_reg_table(rdev, new_ps);
		if (ret) {
			SI_ERROR(rdev, "si_upload_mc_reg_table failed\n");
			return ret;
		}
	}
	ret = si_program_memory_timing_parameters(rdev, new_ps);
	if (ret) {
		SI_ERROR(rdev, "si_program_memory_timing_parameters failed\n");
		return ret;
	}
	si_set_pcie_lane_width_in_smc(rdev, new_ps, old_ps);

	ret = si_resume_smc(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_resume_smc failed\n");
		return ret;
	}
	ret = si_set_sw_state(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_set_sw_state failed\n");
		return ret;
	}
	ni_set_uvd_clock_after_set_eng_clock(rdev, new_ps, old_ps);
	si_set_vce_clock(rdev, new_ps, old_ps);
	if (eg_pi->pcie_performance_request)
		si_notify_link_speed_change_after_state_change(rdev, new_ps, old_ps);
	ret = si_set_power_state_conditionally_enable_ulv(rdev, new_ps);
	if (ret) {
		SI_ERROR(rdev, "si_set_power_state_conditionally_enable_ulv failed\n");
		return ret;
	}
	ret = si_enable_smc_cac(rdev, new_ps, true);
	if (ret) {
		SI_ERROR(rdev, "si_enable_smc_cac failed\n");
		return ret;
	}
	ret = si_enable_power_containment(rdev, new_ps, true);
	if (ret) {
		SI_ERROR(rdev, "si_enable_power_containment failed\n");
		return ret;
	}

	ret = si_power_control_set_level(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_power_control_set_level failed\n");
		return ret;
	}

	return 0;
}

static void si_dpm_post_set_power_state(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_rps *new_ps = &eg_pi->requested_rps;

	ni_update_current_ps(rdev, new_ps);
}

#if 0
static void si_dpm_reset_asic(struct rdn_si_dpm *rdev)
{
	si_restrict_performance_levels_before_switch(rdev);
	si_disable_ulv(rdev);
	si_set_boot_state(rdev);
}
#endif

static void si_dpm_display_configuration_changed(struct rdn_si_dpm *rdev)
{
	si_program_display_gap(rdev);
}

/*
 * si_parse_pplib_non_clock_info(), si_parse_pplib_clock_info() and
 * si_parse_power_table(): Linux's walk of the PowerPlay 6 StateArray /
 * ClockInfoArray / NonClockInfoArray, reading the little-endian VBIOS
 * through byte accessors instead of overlaying its structures.
 */
static void si_parse_pplib_non_clock_info(struct rdn_si_dpm *rdev,
					  struct si_rps *rps,
					  const u8 *non_clock_info,
					  u8 table_rev)
{
	rps->caps = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulCapsAndSettings));
	rps->classification = le16_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, usClassification));
	rps->classification2 = le16_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, usClassification2));

	if (ATOM_PPLIB_NONCLOCKINFO_VER1 < table_rev) {
		rps->vclk = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulVCLK));
		rps->dclk = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulDCLK));
	} else if (r600_is_uvd_state(rps->classification, rps->classification2)) {
		rps->vclk = RV770_DEFAULT_VCLK_FREQ;
		rps->dclk = RV770_DEFAULT_DCLK_FREQ;
	} else {
		rps->vclk = 0;
		rps->dclk = 0;
	}

	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_BOOT)
		rdev->boot_ps = rps;
	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
		rdev->uvd_ps = rps;
}

static void si_parse_pplib_clock_info(struct rdn_si_dpm *rdev,
				      struct si_rps *rps, int index,
				      const u8 *clock_info)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	struct ni_ps *ps = ni_get_ps(rps);
	u16 leakage_voltage;
	struct rv7xx_pl *pl = &ps->performance_levels[index];
	int ret;

	ps->performance_level_count = index + 1;

	pl->sclk = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usEngineClockLow));
	pl->sclk |= (u32)u8_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ucEngineClockHigh)) << 16;
	pl->mclk = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usMemoryClockLow));
	pl->mclk |= (u32)u8_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ucMemoryClockHigh)) << 16;

	pl->vddc = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usVDDC));
	pl->vddci = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usVDDCI));
	pl->flags = le32_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ulFlags));
	pl->pcie_gen = r600_get_pcie_gen_support(si_pi->sys_pcie_mask,
						 si_pi->boot_pcie_gen,
						 (enum rdn_ci_pcie_gen)u8_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ucPCIEGen)));

	/* patch up vddc if necessary */
	ret = si_get_leakage_voltage_from_leakage_index(rdev, pl->vddc,
							&leakage_voltage);
	if (ret == 0)
		pl->vddc = leakage_voltage;

	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_ACPI) {
		pi->acpi_vddc = pl->vddc;
		eg_pi->acpi_vddci = pl->vddci;
		si_pi->acpi_pcie_gen = pl->pcie_gen;
	}

	if ((rps->classification2 & ATOM_PPLIB_CLASSIFICATION2_ULV) &&
	    index == 0) {
		/* XXX disable for A0 tahiti */
		si_pi->ulv.supported = false;
		si_pi->ulv.pl = *pl;
		si_pi->ulv.one_pcie_lane_in_ulv = false;
		si_pi->ulv.volt_change_delay = SISLANDS_ULVVOLTAGECHANGEDELAY_DFLT;
		si_pi->ulv.cg_ulv_parameter = SISLANDS_CGULVPARAMETER_DFLT;
		si_pi->ulv.cg_ulv_control = SISLANDS_CGULVCONTROL_DFLT;
	}

	if (pi->min_vddc_in_table > pl->vddc)
		pi->min_vddc_in_table = pl->vddc;

	if (pi->max_vddc_in_table < pl->vddc)
		pi->max_vddc_in_table = pl->vddc;

	/* patch up boot state */
	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_BOOT) {
		u16 vddc, vddci, mvdd;
		radeon_atombios_get_default_voltages(rdev, &vddc, &vddci, &mvdd);
		pl->mclk = rdev->default_mclk;
		pl->sclk = rdev->default_sclk;
		pl->vddc = vddc;
		pl->vddci = vddci;
		si_pi->mvdd_bootup_value = mvdd;
	}

	if ((rps->classification & ATOM_PPLIB_CLASSIFICATION_UI_MASK) ==
	    ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE) {
		rdev->ci.dyn_state.max_clock_voltage_on_ac.sclk = pl->sclk;
		rdev->ci.dyn_state.max_clock_voltage_on_ac.mclk = pl->mclk;
		rdev->ci.dyn_state.max_clock_voltage_on_ac.vddc = pl->vddc;
		rdev->ci.dyn_state.max_clock_voltage_on_ac.vddci = pl->vddci;
	}
}

static int si_parse_power_table(struct rdn_si_dpm *rdev)
{
	const u8 *non_clock_info;
	const u8 *power_state;
	int i, j, k, non_clock_array_index, clock_array_index;
	const u8 *clock_info;
	const u8 *state_array;
	const u8 *clock_info_array;
	const u8 *non_clock_info_array;
	const u8 *power_info;
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	u16 size = 0;
	u8 frev = 0, crev = 0;
	const u8 *power_state_offset;
	u8 num_states, clock_info_entries, clock_info_entry_size, non_clock_entry_size;

	power_info = si_data_table(rdev, index, &size, &frev, &crev);
	if (!power_info)
		return -EINVAL;

	state_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usStateArrayOffset));
	clock_info_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usClockInfoArrayOffset));
	non_clock_info_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usNonClockInfoArrayOffset));
	num_states = u8_at(state_array, offsetof(StateArray, ucNumEntries));
	clock_info_entries = u8_at(clock_info_array, offsetof(ClockInfoArray, ucNumEntries));
	clock_info_entry_size = u8_at(clock_info_array, offsetof(ClockInfoArray, ucEntrySize));
	non_clock_entry_size = u8_at(non_clock_info_array, offsetof(NonClockInfoArray, ucEntrySize));

	/* Linux checks nothing here; refuse what is not the SI layout rather than read past it */
	if (frev < 6 || clock_info_entry_size < sizeof(ATOM_PPLIB_SI_CLOCK_INFO)) {
		SI_ERROR(rdev, "PowerPlay table %u.%u with %u-byte clock entries is not a Southern Islands one",
			 frev, crev, clock_info_entry_size);
		return -EINVAL;
	}

	rdev->ps = si_zalloc(rdev, (num_states ? num_states : 1) * sizeof(struct si_rps));
	if (!rdev->ps)
		return -ENOMEM;
	rdev->ps_priv = si_zalloc(rdev, (num_states ? num_states : 1) * sizeof(struct ni_ps));
	if (!rdev->ps_priv) {
		si_free(rdev, rdev->ps);
		rdev->ps = NULL;
		return -ENOMEM;
	}
	power_state_offset = state_array + offsetof(StateArray, states);
	for (i = 0; i < num_states; i++) {
		const u8 *idx;
		u8 num_levels;

		power_state = power_state_offset;
		num_levels = u8_at(power_state, offsetof(ATOM_PPLIB_STATE_V2, ucNumDPMLevels));
		non_clock_array_index = u8_at(power_state, offsetof(ATOM_PPLIB_STATE_V2, nonClockInfoIndex));
		non_clock_info = non_clock_info_array + offsetof(NonClockInfoArray, nonClockInfo) +
			non_clock_array_index * non_clock_entry_size;
		/* Linux checks rdev->pm.power_state[i].clock_info, its older parse of the same table; there is none here */
		rdev->ps[i].ps_priv = &rdev->ps_priv[i];
		si_parse_pplib_non_clock_info(rdev, &rdev->ps[i],
					      non_clock_info,
					      non_clock_entry_size);
		k = 0;
		idx = power_state + offsetof(ATOM_PPLIB_STATE_V2, clockInfoIndex);
		for (j = 0; j < num_levels; j++) {
			clock_array_index = u8_at(idx, j);
			if (clock_array_index >= clock_info_entries)
				continue;
			if (k >= SISLANDS_MAX_HARDWARE_POWERLEVELS)
				break;
			clock_info = clock_info_array + offsetof(ClockInfoArray, clockInfo) +
				(clock_array_index * clock_info_entry_size);
			si_parse_pplib_clock_info(rdev,
						  &rdev->ps[i], k,
						  clock_info);
			k++;
		}
		power_state_offset += 2 + num_levels;
	}
	rdev->num_ps = num_states;

	/* fill in the vce power states */
	for (i = 0; i < RDN_CI_MAX_VCE_LEVELS; i++) {
		u32 sclk, mclk;
		clock_array_index = rdev->ci.vce_states[i].clk_idx;
		/*
		 * Linux indexes clockInfo[] by clk_idx * ucEntrySize, a byte
		 * offset scaled twice (clockInfo is a byte array, so the same
		 * thing); out of range when the VBIOS has no VCE table. Skipped.
		 */
		if (clock_array_index >= clock_info_entries)
			continue;
		clock_info = clock_info_array + offsetof(ClockInfoArray, clockInfo) +
			clock_array_index * clock_info_entry_size;
		sclk = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usEngineClockLow));
		sclk |= (u32)u8_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ucEngineClockHigh)) << 16;
		mclk = le16_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, usMemoryClockLow));
		mclk |= (u32)u8_at(clock_info, offsetof(ATOM_PPLIB_SI_CLOCK_INFO, ucMemoryClockHigh)) << 16;
		rdev->ci.vce_states[i].sclk = sclk;
		rdev->ci.vce_states[i].mclk = mclk;
	}

	return 0;
}

static int si_dpm_init(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi;
	struct evergreen_power_info *eg_pi;
	struct ni_power_info *ni_pi;
	struct si_power_info *si_pi;
	struct atom_clock_dividers dividers;
	int ret;

	si_pi = si_zalloc(rdev, sizeof(struct si_power_info));
	if (si_pi == NULL)
		return -ENOMEM;
	rdev->priv = si_pi;
	ni_pi = &si_pi->ni;
	eg_pi = &ni_pi->eg;
	pi = &eg_pi->rv7xx;

	/* Linux asks the PCI core for the root port's speed; the caller says it (rdn_si_dpm_create) */
	si_pi->sys_pcie_mask = rdev->sys_pcie_mask;
	si_pi->force_pcie_gen = RDN_CI_PCIE_GEN_INVALID;
	si_pi->boot_pcie_gen = si_get_current_pcie_speed(rdev);

	si_set_max_cu_value(rdev);

	rv770_get_max_vddc(rdev);
	si_get_leakage_vddc(rdev);
	si_patch_dependency_tables_based_on_leakage(rdev);

	pi->acpi_vddc = 0;
	eg_pi->acpi_vddci = 0;
	pi->min_vddc_in_table = 0;
	pi->max_vddc_in_table = 0;

	ret = r600_get_platform_caps(&rdev->ci);
	if (ret)
		return ret;

	ret = r600_parse_extended_power_table(&rdev->ci);
	if (ret)
		return ret;

	ret = si_parse_power_table(rdev);
	if (ret)
		return ret;

	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries =
		si_zalloc(rdev, 4 * sizeof(struct rdn_ci_dep_entry));
	if (!rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries) {
		r600_free_extended_power_table(&rdev->ci);
		return -ENOMEM;
	}
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.count = 4;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[0].clk = 0;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[0].v = 0;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[1].clk = 36000;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[1].v = 720;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[2].clk = 54000;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[2].v = 810;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[3].clk = 72000;
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries[3].v = 900;

	if (rdev->ci.voltage_response_time == 0)
		rdev->ci.voltage_response_time = R600_VOLTAGERESPONSETIME_DFLT;
	if (rdev->ci.backbias_response_time == 0)
		rdev->ci.backbias_response_time = R600_BACKBIASRESPONSETIME_DFLT;

	ret = radeon_atom_get_clock_dividers(rdev, COMPUTE_ENGINE_PLL_PARAM,
					     0, false, &dividers);
	if (ret)
		pi->ref_div = dividers.ref_div + 1;
	else
		pi->ref_div = R600_REFERENCEDIVIDER_DFLT;

	eg_pi->smu_uvd_hs = false;

	pi->mclk_strobe_mode_threshold = 40000;
	if (si_is_special_1gb_platform(rdev))
		pi->mclk_stutter_mode_threshold = 0;
	else
		pi->mclk_stutter_mode_threshold = pi->mclk_strobe_mode_threshold;
	pi->mclk_edc_enable_threshold = 40000;
	eg_pi->mclk_edc_wr_enable_threshold = 40000;

	ni_pi->mclk_rtt_mode_threshold = eg_pi->mclk_edc_wr_enable_threshold;

	pi->voltage_control =
		radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_VDDC,
					    VOLTAGE_OBJ_GPIO_LUT);
	if (!pi->voltage_control) {
		si_pi->voltage_control_svi2 =
			radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_VDDC,
						    VOLTAGE_OBJ_SVID2);
		if (si_pi->voltage_control_svi2)
			radeon_atom_get_svi2_info(rdev, SET_VOLTAGE_TYPE_ASIC_VDDC,
						  &si_pi->svd_gpio_id, &si_pi->svc_gpio_id);
	}

	pi->mvdd_control =
		radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_MVDDC,
					    VOLTAGE_OBJ_GPIO_LUT);

	eg_pi->vddci_control =
		radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_VDDCI,
					    VOLTAGE_OBJ_GPIO_LUT);
	if (!eg_pi->vddci_control)
		si_pi->vddci_control_svi2 =
			radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_VDDCI,
						    VOLTAGE_OBJ_SVID2);

	si_pi->vddc_phase_shed_control =
		radeon_atom_is_voltage_gpio(rdev->card, SET_VOLTAGE_TYPE_ASIC_VDDC,
					    VOLTAGE_OBJ_PHASE_LUT);

	rv770_get_engine_memory_ss(rdev);

	pi->asi = RV770_ASI_DFLT;
	pi->pasi = CYPRESS_HASI_DFLT;
	pi->vrc = SISLANDS_VRC_DFLT;

	pi->gfx_clock_gating = true;

	eg_pi->sclk_deep_sleep = true;
	si_pi->sclk_deep_sleep_above_low = false;

	if (rdev->int_thermal_type != THERMAL_TYPE_NONE)
		pi->thermal_protection = true;
	else
		pi->thermal_protection = false;

	eg_pi->dynamic_ac_timing = true;

	eg_pi->light_sleep = true;
#if defined(CONFIG_ACPI)
	eg_pi->pcie_performance_request =
		radeon_acpi_is_pcie_performance_request_supported(rdev);
#else
	eg_pi->pcie_performance_request = false;
#endif

	si_pi->sram_end = SMC_RAM_END;

	rdev->ci.dyn_state.mclk_sclk_ratio = 4;
	rdev->ci.dyn_state.sclk_mclk_delta = 15000;
	rdev->ci.dyn_state.vddc_vddci_delta = 200;
	/* valid_sclk_values / valid_mclk_values: empty, as Linux leaves them (btc_find_valid_clock) */

	si_initialize_powertune_defaults(rdev);

	/* make sure dc limits are valid */
	if ((rdev->ci.dyn_state.max_clock_voltage_on_dc.sclk == 0) ||
	    (rdev->ci.dyn_state.max_clock_voltage_on_dc.mclk == 0))
		rdev->ci.dyn_state.max_clock_voltage_on_dc =
			rdev->ci.dyn_state.max_clock_voltage_on_ac;

	si_pi->fan_ctrl_is_in_default_mode = true;

	return 0;
}


static void si_dpm_fini(struct rdn_si_dpm *rdev)
{
	si_free(rdev, rdev->ps_priv);
	rdev->ps_priv = NULL;
	si_free(rdev, rdev->ps);
	rdev->ps = NULL;
	rdev->num_ps = 0;
	si_free(rdev, rdev->priv);
	rdev->priv = NULL;
	si_free(rdev, rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries);
	rdev->ci.dyn_state.vddc_dependency_on_dispclk.entries = NULL;
	r600_free_extended_power_table(&rdev->ci);
}

/* ---- the interface (rdn_si_dpm.h): radeon_pm.c's part ---- */

static enum si_chip_family si_family_of(const struct rdn_card *card)
{
	if (!card->chip)
		return (enum si_chip_family)0;
	switch (card->chip->chip) {
	case RDN_CHIP_TAHITI:
		return CHIP_TAHITI;
	case RDN_CHIP_PITCAIRN:
		return CHIP_PITCAIRN;
	case RDN_CHIP_VERDE:
		return CHIP_VERDE;
	default:
		/* Oland and Hainan have rows in si_dpm.c's tables but none in rdn_family.c yet */
		return (enum si_chip_family)0;
	}
}

/* radeon_atombios_add_pplib_thermal_controller()'s rdev->pm.int_thermal_type for SI */
static int si_thermal_type(u8 controller)
{
	switch (controller) {
	case ATOM_PP_THERMALCONTROLLER_NONE:
		return THERMAL_TYPE_NONE;
	case ATOM_PP_THERMALCONTROLLER_SISLANDS:
		return THERMAL_TYPE_SI;
	case ATOM_PP_THERMALCONTROLLER_EMC2103_WITH_INTERNAL:
		return THERMAL_TYPE_EMC2103_WITH_INTERNAL;
	case ATOM_PP_THERMALCONTROLLER_EXTERNAL_GPIO:
		return THERMAL_TYPE_EXTERNAL_GPIO;
	default:
		return THERMAL_TYPE_EXTERNAL;
	}
}

struct rdn_si_dpm *rdn_si_dpm_create(struct rdn_card *card, unsigned sys_pcie_mask)
{
	struct rdn_si_dpm *rdev;
	int ret;

	rdev = card->os->alloc(card->os->cookie, sizeof(*rdev));
	if (!rdev)
		return NULL;
	rdev->card = card;
	rdev->ci.card = card;
	rdev->family = si_family_of(card);
	if (!rdev->family) {
		rdn_log(card->os, RDN_LOG_ERROR, "si_dpm: %s is not a Southern Islands chip this port knows",
			card->chip ? card->chip->name : "this card");
		card->os->free(card->os->cookie, rdev);
		return NULL;
	}
	rdev->device = card->device;
	rdev->revision = card->os->cfg_read32 ? (u8)(card->os->cfg_read32(card->os->cookie, 0x08) & 0xff) : 0;
	/* rdev->num_crtc: si_init() gives Oland 2 and Hainan none */
	rdev->num_crtc = rdev->family == CHIP_OLAND ? 2 : (rdev->family == CHIP_HAINAN ? 0 : 6);
	rdev->sys_pcie_mask = sys_pcie_mask;
	rdev->ci.ac_power = true;	/* radeon_pm: power_supply_is_system_supplied(); a laptop would ask */
	rdev->vce_level = 0;

	/* radeon_atombios_get_clock_info(): the reference clock, capability flags, default clocks */
	ret = ci_parse_firmware_info(&rdev->ci);
	if (ret) {
		SI_ERROR(rdev, "no FirmwareInfo");
		card->os->free(card->os->cookie, rdev);
		return NULL;
	}
	si_get_default_clocks(rdev);

	/* radeon_atombios_get_power_modes(): the thermal controller, before si_dpm_init() */
	ret = r600_get_platform_caps(&rdev->ci);
	if (ret) {
		SI_ERROR(rdev, "no PowerPlay table");
		card->os->free(card->os->cookie, rdev);
		return NULL;
	}
	rdev->int_thermal_type = si_thermal_type(rdev->ci.thermal_controller_type);

	ret = si_dpm_init(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_dpm_init failed (%d)", ret);
		si_dpm_fini(rdev);
		card->os->free(card->os->cookie, rdev);
		return NULL;
	}
	if (!rdev->boot_ps) {
		SI_ERROR(rdev, "the PowerPlay table has no boot state");
		si_dpm_fini(rdev);
		card->os->free(card->os->cookie, rdev);
		return NULL;
	}
	/* radeon_pm_init_dpm() */
	rdev->current_ps = rdev->requested_ps = rdev->boot_ps;
	return rdev;
}

void rdn_si_dpm_destroy(struct rdn_si_dpm *rdev)
{
	struct rdn_card *card;

	if (!rdev)
		return;
	card = rdev->card;
	si_dpm_fini(rdev);
	card->os->free(card->os->cookie, rdev);
}

bool rdn_si_dpm_enabled(const struct rdn_si_dpm *rdev)
{
	return rdev && rdev->enabled;
}

static const char *si_ui_class_name(u32 c)
{
	switch (c & ATOM_PPLIB_CLASSIFICATION_UI_MASK) {
	case ATOM_PPLIB_CLASSIFICATION_UI_BATTERY:
		return "battery";
	case ATOM_PPLIB_CLASSIFICATION_UI_BALANCED:
		return "balanced";
	case ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE:
		return "performance";
	default:
		return "none";
	}
}

/* ni_dpm_print_power_state() for one state */
static void si_print_state(struct rdn_si_dpm *rdev, const char *what, int n, struct si_rps *rps)
{
	struct ni_ps *ps = ni_get_ps(rps);
	int i;

	SI_INFO(rdev, "%s %d: ui class %s, class 0x%04x/0x%04x, caps 0x%08x, uvd vclk %u dclk %u",
		what, n, si_ui_class_name(rps->classification), (unsigned)rps->classification,
		(unsigned)rps->classification2, (unsigned)rps->caps, (unsigned)rps->vclk, (unsigned)rps->dclk);
	for (i = 0; i < ps->performance_level_count; i++) {
		struct rv7xx_pl *pl = &ps->performance_levels[i];

		SI_INFO(rdev, "    power level %d    sclk: %u mclk: %u vddc: %u vddci: %u pcie gen: %u",
			i, (unsigned)pl->sclk, (unsigned)pl->mclk, pl->vddc, pl->vddci, (unsigned)pl->pcie_gen + 1);
	}
}

static void si_print_dep(struct rdn_si_dpm *rdev, const char *what, const struct rdn_ci_dep_table *t)
{
	char line[160];
	unsigned i, n = 0;

	line[0] = 0;
	for (i = 0; i < t->count && n + 20 < sizeof(line); i++) {
		unsigned clk = (unsigned)t->entries[i].clk, v = t->entries[i].v, k;
		char item[24];
		int len = 0;

		/* no snprintf in the kext: digits by hand */
		char digits[12];
		int d = 0;
		k = clk;
		do { digits[d++] = (char)('0' + k % 10); k /= 10; } while (k && d < 11);
		while (d) item[len++] = digits[--d];
		item[len++] = ':';
		k = v;
		do { digits[d++] = (char)('0' + k % 10); k /= 10; } while (k && d < 11);
		while (d) item[len++] = digits[--d];
		item[len++] = ' ';
		item[len] = 0;
		memcpy(line + n, item, (size_t)len + 1);
		n += (unsigned)len;
	}
	SI_INFO(rdev, "%s (10 kHz:mV): %s", what, t->count ? line : "none");
}

void rdn_si_dpm_describe(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi = rv770_get_pi(rdev);
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);
	int i;

	SI_INFO(rdev, "%s 0x%04x rev 0x%02x: reference clock %u0 kHz, boot %u0/%u0 kHz, dispclk %u0 kHz",
		rdev->card->chip ? rdev->card->chip->name : "?", rdev->device, rdev->revision,
		(unsigned)rdev->ci.reference_freq, (unsigned)rdev->default_sclk, (unsigned)rdev->default_mclk,
		(unsigned)rdev->current_dispclk);
	SI_INFO(rdev, "platform caps 0x%08x, thermal controller %u (type %d)%s, %u CUs max (0: all)",
		(unsigned)rdev->ci.platform_caps, rdev->ci.thermal_controller_type, rdev->int_thermal_type,
		rdev->ci.no_fan ? ", no fan" : "", (unsigned)si_pi->max_cu);
	SI_INFO(rdev, "voltage: vddc %s, vddci %s, mvdd %s, phase shedding %s; max vddc %u mV, table %u..%u mV",
		pi->voltage_control ? "GPIO" : (si_pi->voltage_control_svi2 ? "SVI2" : "none"),
		eg_pi->vddci_control ? "GPIO" : (si_pi->vddci_control_svi2 ? "SVI2" : "none"),
		pi->mvdd_control ? "GPIO" : "none", si_pi->vddc_phase_shed_control ? "yes" : "no",
		pi->max_vddc, pi->min_vddc_in_table, pi->max_vddc_in_table);
	for (i = 0; i < si_pi->leakage_voltage.count; i++)
		SI_INFO(rdev, "leakage id 0x%04x = %u mV", si_pi->leakage_voltage.entries[i].leakage_index,
			si_pi->leakage_voltage.entries[i].voltage);
	SI_INFO(rdev, "TDP %u W, near %u W, OD %u%%, SQ ramping above %u0 kHz, CAC leakage %u, load line %u",
		(unsigned)rdev->ci.tdp_limit, (unsigned)rdev->ci.near_tdp_limit, (unsigned)rdev->ci.tdp_od_limit,
		(unsigned)rdev->ci.sq_ramping_threshold, (unsigned)rdev->ci.cac_leakage, rdev->ci.load_line_slope);
	SI_INFO(rdev, "PowerTune %s, CAC %s, SQ ramping %s, DTE %s; sclk SS %s, mclk SS %s; PCIe mask %u, boot gen %u",
		ni_pi->enable_power_containment ? "on" : "off", ni_pi->enable_cac ? "on" : "off",
		ni_pi->enable_sq_ramping ? "on" : "off", si_pi->enable_dte ? "on" : "off",
		pi->sclk_ss ? "yes" : "no", pi->mclk_ss ? "yes" : "no",
		(unsigned)si_pi->sys_pcie_mask, (unsigned)si_pi->boot_pcie_gen + 1);
	if (rdev->ci.fan.ucode_fan_control)
		SI_INFO(rdev, "fan table: %u..%u..%u C at %u..%u..%u (0.01%%), max %u C, hysteresis %u",
			rdev->ci.fan.t_min / 100, rdev->ci.fan.t_med / 100, rdev->ci.fan.t_high / 100,
			rdev->ci.fan.pwm_min, rdev->ci.fan.pwm_med, rdev->ci.fan.pwm_high,
			rdev->ci.fan.t_max / 100, rdev->ci.fan.t_hyst);
	si_print_dep(rdev, "vddc on sclk", &rdev->ci.dyn_state.vddc_dependency_on_sclk);
	si_print_dep(rdev, "vddc on mclk", &rdev->ci.dyn_state.vddc_dependency_on_mclk);
	si_print_dep(rdev, "vddci on mclk", &rdev->ci.dyn_state.vddci_dependency_on_mclk);
	si_print_dep(rdev, "vddc on dispclk", &rdev->ci.dyn_state.vddc_dependency_on_dispclk);
	SI_INFO(rdev, "limits on ac: %u/%u at %u/%u mV, on dc: %u/%u at %u/%u mV",
		(unsigned)rdev->ci.dyn_state.max_clock_voltage_on_ac.sclk, (unsigned)rdev->ci.dyn_state.max_clock_voltage_on_ac.mclk,
		rdev->ci.dyn_state.max_clock_voltage_on_ac.vddc, rdev->ci.dyn_state.max_clock_voltage_on_ac.vddci,
		(unsigned)rdev->ci.dyn_state.max_clock_voltage_on_dc.sclk, (unsigned)rdev->ci.dyn_state.max_clock_voltage_on_dc.mclk,
		rdev->ci.dyn_state.max_clock_voltage_on_dc.vddc, rdev->ci.dyn_state.max_clock_voltage_on_dc.vddci);
	for (i = 0; i < rdev->num_ps; i++)
		si_print_state(rdev, "power state", i, &rdev->ps[i]);
}

int rdn_si_dpm_enable(struct rdn_si_dpm *rdev, const void *smc_fw, size_t smc_size)
{
	int ret;

	rdev->smc_fw = smc_fw;
	rdev->smc_fw_size = smc_size;

	/* radeon_dpm_setup_asic(), radeon_dpm_enable(), radeon_dpm_late_enable() */
	si_dpm_setup_asic(rdev);
	ret = si_dpm_enable(rdev);
	if (ret)
		return ret;
	rdev->enabled = true;
	ret = si_dpm_late_enable(rdev);
	if (ret)
		SI_ERROR(rdev, "si_dpm_late_enable failed (%d)", ret);
	return 0;
}

void rdn_si_dpm_disable(struct rdn_si_dpm *rdev)
{
	if (!rdev->enabled)
		return;
	si_dpm_disable(rdev);
	rdev->current_ps = rdev->requested_ps = rdev->boot_ps;
	rdev->enabled = false;
}

/* radeon_dpm_pick_power_state() */
static struct si_rps *si_pick_power_state(struct rdn_si_dpm *rdev, enum rdn_si_power_state state)
{
	bool single_display = rdev->ci.new_active_crtc_count < 2;
	u32 want;
	int i, pass;

	if (state == RDN_SI_STATE_BOOT)
		return rdev->boot_ps;
	/* check if the vblank period is too short to adjust the mclk */
	if (single_display && ni_dpm_vblank_too_short(rdev))
		single_display = false;
	/* Linux tries an internal 3D performance state first, then the UI class; balanced states "don't exist", so performance */
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < rdev->num_ps; i++) {
			struct si_rps *ps = &rdev->ps[i];
			u32 ui_class = ps->classification & ATOM_PPLIB_CLASSIFICATION_UI_MASK;

			if (pass == 0) {
				if (state == RDN_SI_STATE_PERFORMANCE &&
				    (ps->classification & ATOM_PPLIB_CLASSIFICATION_3DPERFORMANCE))
					return ps;
				continue;
			}
			want = (pass == 1 && state == RDN_SI_STATE_BATTERY) ?
				ATOM_PPLIB_CLASSIFICATION_UI_BATTERY : ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE;
			if (ui_class != want)
				continue;
			if ((ps->caps & ATOM_PPLIB_SINGLE_DISPLAY_ONLY) && !single_display)
				continue;
			return ps;
		}
	}
	return rdev->boot_ps;
}

/*
 * amdgpu's si_check_state_equal() (si_dpm.c, Linux 7.0) over the state the
 * SMC runs and the adjusted one asked for: the same levels and the same UVD
 * and VCE clocks.
 */
static bool si_check_state_equal(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct si_rps *cps = &eg_pi->current_rps, *rps = &eg_pi->requested_rps;
	struct ni_ps *si_cps = ni_get_ps(cps), *si_rps = ni_get_ps(rps);
	int i;

	if (!si_cps || !si_rps || si_cps->performance_level_count != si_rps->performance_level_count)
		return false;
	for (i = 0; i < si_cps->performance_level_count; i++) {
		const struct rv7xx_pl *a = &si_cps->performance_levels[i], *b = &si_rps->performance_levels[i];

		/* si_are_power_levels_equal() */
		if (a->sclk != b->sclk || a->mclk != b->mclk || a->pcie_gen != b->pcie_gen ||
		    a->vddc != b->vddc || a->vddci != b->vddci)
			return false;
	}
	return cps->vclk == rps->vclk && cps->dclk == rps->dclk && cps->evclk == rps->evclk && cps->ecclk == rps->ecclk;
}

/*
 * radeon_dpm_change_power_state_locked(), less the ring drain (the caller
 * holds the engine). Firebrand: unless RADEON_RULES, as amdgpu's
 * amdgpu_dpm_change_power_state_locked(), which leaves the SMC alone when
 * the adjusted state is the one it runs (1 is returned then): a display
 * that comes or goes without changing the levels costs the display gap and
 * HasDisplay, not a state switch.
 */
int rdn_si_dpm_set_power_state(struct rdn_si_dpm *rdev, enum rdn_si_power_state state)
{
	struct si_rps *ps;
	int ret;

	if (!rdev->enabled)
		return -EINVAL;
	rdev->smc_silent = false;
	ps = si_pick_power_state(rdev, state);
	rdev->requested_ps = ps;
	ps->vce_active = false;

	ret = si_dpm_pre_set_power_state(rdev);
	if (ret)
		return ret;
	/* radeon_dpm_display_configuration_changed() */
	si_dpm_display_configuration_changed(rdev);
	if (!(rdev->quirks & RDN_SI_DPM_RADEON_RULES) && si_check_state_equal(rdev)) {
		rdev->requested_ps = rdev->current_ps;
		return 1;
	}
	ret = si_dpm_set_power_state(rdev);
	if (ret) {
		SI_ERROR(rdev, "si_dpm_set_power_state failed (%d)", ret);
		/*
		 * Firebrand: an SMC that left a message unanswered is not
		 * left to finish the switch behind the driver's back (on
		 * the R7 450 the card stopped answering some time after such a
		 * failure): it is held in reset with its clock off, where it
		 * stands, and DPM counts as off. Linux returns the error and
		 * leaves it running.
		 */
		if (rdev->smc_silent && si_is_smc_running(rdev)) {
			si_reset_smc(rdev);
			si_stop_smc_clock(rdev);
			SI_ERROR(rdev, "the SMC is stopped where it stood (reset, clock off): the clocks stay as they are, DPM is off");
		}
		if (rdev->smc_silent) {
			rdev->current_ps = rdev->requested_ps = rdev->boot_ps;
			rdev->enabled = false;
		}
		return ret;
	}
	rdev->current_ps = rdev->requested_ps;
	si_dpm_post_set_power_state(rdev);
	/* the user's forced level stays in force */
	return si_dpm_force_performance_level(rdev, (enum rdn_si_forced_level)rdev->ci.forced_level);
}

int rdn_si_dpm_force_performance_level(struct rdn_si_dpm *rdev, enum rdn_si_forced_level level)
{
	if (!rdev->enabled)
		return -EINVAL;
	return si_dpm_force_performance_level(rdev, level);
}

void rdn_si_dpm_display_changed(struct rdn_si_dpm *rdev, const struct rdn_si_display *display)
{
	rdev->display = *display;
	/* Firebrand, SWITCH_NO_DISPLAY (bring-up): the SMC and the state rules see no display */
	if (rdev->quirks & RDN_SI_DPM_SWITCH_NO_DISPLAY) {
		rdev->display.active_crtcs = 0;
		rdev->display.vblank_time_us = 0xffffffff;
		rdev->display.high_pixelclock_count = 0;
	}
	display = &rdev->display;
	rdev->high_pixelclock_count = (int)display->high_pixelclock_count;
	rdev->new_active_crtcs = display->active_crtcs;
	rdev->ci.new_active_crtc_count = hweight32(display->active_crtcs);
	if (display->dispclk)
		rdev->current_dispclk = display->dispclk;
	if (rdev->enabled)
		si_dpm_display_configuration_changed(rdev);
}

int rdn_si_dpm_current_level(struct rdn_si_dpm *rdev, unsigned *index, uint32_t *sclk,
			     uint32_t *mclk, uint16_t *vddc)
{
	struct evergreen_power_info *eg_pi;
	struct ni_ps *ps;
	struct rv7xx_pl *pl;
	u32 current_index;

	if (!rdev->enabled)
		return -EINVAL;
	eg_pi = evergreen_get_pi(rdev);
	ps = ni_get_ps(&eg_pi->current_rps);
	current_index = (RREG32(TARGET_AND_CURRENT_PROFILE_INDEX) & CURRENT_STATE_INDEX_MASK) >>
		CURRENT_STATE_INDEX_SHIFT;
	if (index)
		*index = current_index;
	if (current_index >= ps->performance_level_count)
		return -EINVAL;
	pl = &ps->performance_levels[current_index];
	if (sclk)
		*sclk = pl->sclk;
	if (mclk)
		*mclk = pl->mclk;
	if (vddc)
		*vddc = pl->vddc;
	return 0;
}

int rdn_si_dpm_temperature(struct rdn_si_dpm *rdev)
{
	return si_get_temp(rdev);
}

int rdn_si_dpm_get_fan_speed_percent(struct rdn_si_dpm *rdev, uint32_t *speed)
{
	return si_fan_ctrl_get_fan_speed_percent(rdev, speed);
}

int rdn_si_dpm_set_fan_speed_percent(struct rdn_si_dpm *rdev, uint32_t speed)
{
	return si_fan_ctrl_set_fan_speed_percent(rdev, speed);
}

void rdn_si_dpm_set_fan_mode(struct rdn_si_dpm *rdev, uint32_t mode)
{
	si_fan_ctrl_set_mode(rdev, mode);
}

uint32_t rdn_si_dpm_get_fan_mode(struct rdn_si_dpm *rdev)
{
	return si_fan_ctrl_get_mode(rdev);
}

uint32_t rdn_si_dpm_table_address(const struct rdn_si_dpm *rdev, unsigned which)
{
	const struct si_power_info *si_pi = rdev ? rdev->priv : NULL;

	if (!si_pi)
		return 0;
	switch (which) {
	case 0: return si_pi->state_table_start;
	case 1: return si_pi->soft_regs_start;
	case 2: return si_pi->mc_reg_table_start;
	case 3: return si_pi->fan_table_start;
	case 4: return si_pi->arb_table_start;
	case 5: return si_pi->cac_table_start;
	case 6: return si_pi->dte_table_start;
	case 7: return si_pi->spll_table_start;
	case 8: return si_pi->papm_cfg_table_start;
	default: return 0;
	}
}

/*
 * si_init_microcode()'s choice between the plain SMC image and the "kicker"
 * one (<chip>_k_smc.bin), by device and revision. Hainan rev 0xc3 /
 * 0x6665 takes a third image (banks_k_2_smc.bin) that is not carried.
 */
static const struct {
	u16 device;
	u8 revision;
} si_kicker_smc[] = {
	{ 0x6810, 0x81 }, { 0x6811, 0x81 },			/* Pitcairn */
	{ 0x6820, 0x81 }, { 0x6820, 0x83 }, { 0x6821, 0x83 },	/* Verde */
	{ 0x6821, 0x87 }, { 0x6823, 0x87 }, { 0x682b, 0x87 },
	{ 0x6600, 0x81 }, { 0x6604, 0x81 }, { 0x6605, 0x81 },	/* Oland */
	{ 0x6610, 0x81 }, { 0x6610, 0x83 },
	{ 0x6660, 0x81 }, { 0x6660, 0x83 }, { 0x6663, 0x83 },	/* Hainan */
	{ 0x6665, 0x83 }, { 0x6667, 0x83 },
};

bool rdn_si_dpm_wants_kicker_smc(const struct rdn_si_dpm *rdev)
{
	unsigned i;

	for (i = 0; i < sizeof(si_kicker_smc) / sizeof(si_kicker_smc[0]); i++)
		if (si_kicker_smc[i].device == rdev->device && si_kicker_smc[i].revision == rdev->revision)
			return true;
	return false;
}

void rdn_si_dpm_set_quirks(struct rdn_si_dpm *rdev, unsigned quirks)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_power_info *ni_pi = ni_get_pi(rdev);
	struct si_power_info *si_pi = si_get_pi(rdev);

	rdev->quirks = quirks;
	if (quirks & RDN_SI_DPM_NO_SCLK_DEEP_SLEEP)
		eg_pi->sclk_deep_sleep = false;
	if (quirks & RDN_SI_DPM_NO_POWERTUNE) {
		ni_pi->enable_power_containment = false;
		ni_pi->enable_cac = false;
		ni_pi->enable_sq_ramping = false;
		si_pi->enable_dte = false;
	}
	if (quirks & RDN_SI_DPM_NO_SMC_FAN)
		rdev->ci.fan.ucode_fan_control = false;
	if (quirks & RDN_SI_DPM_NO_MC_AC_TIMING)
		eg_pi->dynamic_ac_timing = false;
}

void rdn_si_dpm_set_msg_hook(struct rdn_si_dpm *rdev, rdn_si_dpm_msg_hook hook, void *ctx)
{
	rdev->msg_hook = hook;
	rdev->msg_hook_ctx = ctx;
}

int rdn_si_dpm_mclk_held(const struct rdn_si_dpm *rdev, const char **why)
{
	if (why)
		*why = rdev->mclk_held;
	return rdev->mclk_held != NULL;
}

void rdn_si_dpm_clock_range(struct rdn_si_dpm *rdev, uint32_t *sclk_low, uint32_t *mclk_low,
			    uint32_t *sclk_high, uint32_t *mclk_high)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	struct ni_ps *ps = ni_get_ps(&eg_pi->requested_rps);

	/* ni_dpm_get_sclk/mclk(low) */
	if (!ps || !ps->performance_level_count) {
		*sclk_low = *sclk_high = rdev->default_sclk;
		*mclk_low = *mclk_high = rdev->default_mclk;
		return;
	}
	*sclk_low = ps->performance_levels[0].sclk;
	*mclk_low = ps->performance_levels[0].mclk;
	*sclk_high = ps->performance_levels[ps->performance_level_count - 1].sclk;
	*mclk_high = ps->performance_levels[ps->performance_level_count - 1].mclk;
}

/*
 * The VDDC the regulator is set to now. On boards whose VDDC is GPIO
 * controlled (the VBIOS's VOLTAGE_OBJ_GPIO_LUT, the R7 450 among them) the
 * SMC drives the regulator's VID pins through the GPIO pads: GPIOPAD_Y,
 * masked with the table's GPIO mask, is one of the LUT's ids (measured
 * 2026-11-11: 0x40000000 = 950 mV at level 0, 0x00108000 = 1225 mV at
 * level 3). SVI2 parts (most mobile SI) have no readback on this family;
 * there the current level's programmed voltage is given. 0 if unknown.
 */
#define SI_GPIOPAD_Y	0x17a4

uint16_t rdn_si_dpm_vddc_now(struct rdn_si_dpm *rdev)
{
	struct rv7xx_power_info *pi;
	struct evergreen_power_info *eg_pi;
	uint16_t vddc = 0;
	u32 pins, i;

	if (!rdev->enabled)
		return 0;
	pi = rv770_get_pi(rdev);
	eg_pi = evergreen_get_pi(rdev);
	if (pi->voltage_control && eg_pi->vddc_voltage_table.count && eg_pi->vddc_voltage_table.mask_low) {
		pins = RREG32(SI_GPIOPAD_Y) & eg_pi->vddc_voltage_table.mask_low;
		for (i = 0; i < eg_pi->vddc_voltage_table.count; i++)
			if (eg_pi->vddc_voltage_table.entries[i].smio_low == pins)
				return eg_pi->vddc_voltage_table.entries[i].value;
		return 0;
	}
	if (rdn_si_dpm_current_level(rdev, NULL, NULL, NULL, &vddc))
		return 0;
	return vddc;
}

void rdn_si_dpm_describe_voltage_table(struct rdn_si_dpm *rdev)
{
	struct evergreen_power_info *eg_pi = evergreen_get_pi(rdev);
	u32 i;

	SI_INFO(rdev, "VDDC table: %u entries, GPIO mask 0x%08x",
		(unsigned)eg_pi->vddc_voltage_table.count, (unsigned)eg_pi->vddc_voltage_table.mask_low);
	for (i = 0; i < eg_pi->vddc_voltage_table.count; i++)
		SI_INFO(rdev, "    %u mV: GPIO 0x%08x", eg_pi->vddc_voltage_table.entries[i].value,
			(unsigned)eg_pi->vddc_voltage_table.entries[i].smio_low);
}

/* Fan duty in percent as the controller drives it now (si_fan_ctrl_get_fan_speed_percent), or -1. */
int rdn_si_dpm_fan_percent(struct rdn_si_dpm *rdev)
{
	u32 speed;

	if (si_fan_ctrl_get_fan_speed_percent(rdev, &speed))
		return -1;
	return (int)speed;
}
