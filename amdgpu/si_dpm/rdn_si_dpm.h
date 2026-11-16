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
 * Southern Islands (Tahiti, Pitcairn, Verde, Oland, Hainan) dynamic power
 * management: the Linux radeon driver's si_dpm.c and si_smc.c, with the
 * parts of ni_dpm.c, btc_dpm.c, rv770_dpm.c, r600_dpm.c and
 * radeon_atombios.c they call, as a freestanding library.
 *
 * As on Sea Islands, an embedded controller (the SMC, running the
 * VERDE_smc.bin / verde_k_smc.bin family of images) switches clocks and
 * voltages by itself once its SRAM tables are filled; this code parses the
 * VBIOS PowerPlay table, fills those tables, starts the firmware and then
 * only sends it messages. The VBIOS half of the parse (the dependency
 * tables, fan, TDP, platform caps) is rdn_ci_tables.c's, shared with the
 * Sea Islands port; everything family-specific is in rdn_si_dpm.c.
 *
 * Per-chip data (CAC weights, PowerTune and DTE coefficients, the CU
 * count, clock caps for Oland and Hainan parts) is Linux's, keyed on the
 * family and the PCI device id exactly as si_initialize_powertune_defaults()
 * and si_set_max_cu_value() key it. Mobile SKUs (Venus, Heathrow, Chelsea,
 * Mars, Sun, Hainan) take their own rows there.
 *
 * Deviations from Linux, each marked at its site: no interrupt handler (the
 * thermal alert is armed, nothing receives it), no DRM mode list (the
 * display configuration comes in through rdn_si_dpm_display_changed()), no
 * ACPI (no PCIe performance requests, as Linux without CONFIG_ACPI), VCE
 * clocks are not driven, and UVD clocks are left to the caller.
 *
 * Units follow Linux: clocks in 10 kHz, voltages in mV, temperatures in
 * millidegrees Celsius.
 */
#ifndef RDN_SI_DPM_H
#define RDN_SI_DPM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rdn_card.h"

struct rdn_si_dpm;

/* Which VBIOS power state rdn_si_dpm_set_power_state() requests (radeon_pm.c's enum radeon_pm_state_type) */
enum rdn_si_power_state {
	RDN_SI_STATE_BOOT,		/* ATOM_PPLIB_CLASSIFICATION_BOOT: the VBIOS's boot clocks */
	RDN_SI_STATE_BATTERY,		/* UI_BATTERY, else the performance state */
	RDN_SI_STATE_BALANCED,		/* UI_BALANCED, else the performance state */
	RDN_SI_STATE_PERFORMANCE,	/* UI_PERFORMANCE: every level, the SMC picks by load */
	RDN_SI_STATE_UVD		/* POWER_STATE_TYPE_INTERNAL_UVD: the UVDSTATE class (its VCLK/DCLK), else performance */
};

/* radeon.h's enum radeon_dpm_forced_level */
enum rdn_si_forced_level {
	RDN_SI_FORCED_LEVEL_AUTO = 0,
	RDN_SI_FORCED_LEVEL_LOW = 1,
	RDN_SI_FORCED_LEVEL_HIGH = 2
};

/* radeon.h's RADEON_PCIE_SPEED_*: what the root port can do */
#define RDN_SI_PCIE_SPEED_25	1
#define RDN_SI_PCIE_SPEED_50	2
#define RDN_SI_PCIE_SPEED_80	4

/*
 * What Linux reads from its DRM mode list and the display watermark code
 * (r600_dpm_get_vblank_time, rdev->pm.dpm.new_active_crtcs, the first
 * active radeon_crtc's line_time / wm_high / wm_low, rdev->clock.current_dispclk).
 */
struct rdn_si_display {
	uint32_t active_crtcs;		/* bit per CRTC scanning out */
	uint32_t vblank_time_us;	/* 0xffffffff: no display; 0: unknown, keeps the memory clock put */
	uint32_t dispclk;		/* display engine clock, 10 kHz */
	uint32_t line_time;		/* ns, the first active CRTC's (dce6_program_watermarks); 0: unknown */
	uint32_t wm_high;		/* ns, latency watermark at the highest clocks */
	uint32_t wm_low;		/* ns, latency watermark at the lowest clocks */
	uint32_t high_pixelclock_count;	/* active displays above 297 MHz (amdgpu's "high pixelclock": more than 4K30 needs) */
};

/*
 * si_dpm_init(): the VBIOS boot values, leakage voltages, platform caps,
 * r600_parse_extended_power_table(), si_parse_power_table(), the voltage
 * control detection, PowerTune defaults. sys_pcie_mask is
 * RDN_SI_PCIE_SPEED_* (0: unknown). NULL on failure, logged.
 */
struct rdn_si_dpm *rdn_si_dpm_create(struct rdn_card *card, unsigned sys_pcie_mask);
void rdn_si_dpm_destroy(struct rdn_si_dpm *dpm);

/* Log what rdn_si_dpm_create() parsed, in Linux's power state print format. */
void rdn_si_dpm_describe(struct rdn_si_dpm *dpm);

/*
 * si_dpm_setup_asic() less the MC microcode (loaded at POST) followed by
 * si_dpm_enable() and si_dpm_late_enable(). smc_fw is the SMC image as
 * linux-firmware ships it, raw (VERDE_smc.bin) or with the v1 header
 * (verde_k_smc.bin). -EINVAL if the SMC already runs. 0 or a negative errno.
 */
int rdn_si_dpm_enable(struct rdn_si_dpm *dpm, const void *smc_fw, size_t smc_size);

/* si_dpm_disable(): thermal and fan back to the hardware, DPM off, the SMC in reset, arbiter set F0. */
void rdn_si_dpm_disable(struct rdn_si_dpm *dpm);

/*
 * radeon_dpm_change_power_state_locked() for one VBIOS state:
 * si_dpm_pre_set_power_state(), si_dpm_set_power_state(),
 * si_dpm_post_set_power_state(). 0, 1 when the SMC already runs that state
 * (Firebrand: amdgpu's check_state_equal, not under RADEON_RULES), or a
 * negative errno.
 */
int rdn_si_dpm_set_power_state(struct rdn_si_dpm *dpm, enum rdn_si_power_state state);

/* si_dpm_force_performance_level() */
int rdn_si_dpm_force_performance_level(struct rdn_si_dpm *dpm, enum rdn_si_forced_level level);

/*
 * The display configuration changed: record it and run
 * si_dpm_display_configuration_changed() (the display gap and the SMC's
 * HasDisplay). Call rdn_si_dpm_set_power_state() afterwards, as Linux's
 * radeon_pm_compute_clocks() does, so the state is re-adjusted to it.
 */
void rdn_si_dpm_display_changed(struct rdn_si_dpm *dpm, const struct rdn_si_display *display);

/*
 * The level the SMC runs now (TARGET_AND_CURRENT_PROFILE_INDEX against the
 * state last uploaded): index, engine and memory clock (10 kHz), VDDC (mV).
 * -EINVAL when DPM is off or the index is outside the state.
 */
int rdn_si_dpm_current_level(struct rdn_si_dpm *dpm, unsigned *index, uint32_t *sclk,
			     uint32_t *mclk, uint16_t *vddc);

/*
 * The VDDC the regulator is set to now, mV: read back from the VID pins on
 * GPIO-controlled boards, else the current level's programmed value; 0 if
 * unknown. Log the GPIO table with rdn_si_dpm_describe_voltage_table()
 * (built at enable time).
 */
uint16_t rdn_si_dpm_vddc_now(struct rdn_si_dpm *dpm);
void rdn_si_dpm_describe_voltage_table(struct rdn_si_dpm *dpm);

/* The fan duty the controller drives now, percent, or -1 (no fan, no duty range). */
int rdn_si_dpm_fan_percent(struct rdn_si_dpm *dpm);

/* si_get_temp(): CG_MULT_THERMAL_STATUS, millidegrees Celsius. */
int rdn_si_dpm_temperature(struct rdn_si_dpm *dpm);

/* Fan (si_fan_ctrl_*): percent 0..100; mode 0 gives it back to the SMC, 1 (static) pins the duty. */
int rdn_si_dpm_get_fan_speed_percent(struct rdn_si_dpm *dpm, uint32_t *speed);
int rdn_si_dpm_set_fan_speed_percent(struct rdn_si_dpm *dpm, uint32_t speed);
void rdn_si_dpm_set_fan_mode(struct rdn_si_dpm *dpm, uint32_t mode);
uint32_t rdn_si_dpm_get_fan_mode(struct rdn_si_dpm *dpm);

bool rdn_si_dpm_enabled(const struct rdn_si_dpm *dpm);

/* si_init_microcode(): true when this device and revision run the "kicker" SMC image (<chip>_k_smc.bin). */
bool rdn_si_dpm_wants_kicker_smc(const struct rdn_si_dpm *dpm);

/*
 * Features to leave off, set before rdn_si_dpm_enable(); 0 is Linux's stock
 * configuration. NO_SCLK_DEEP_SLEEP is the reference session's si_sclk_ds=0
 * (the run4 capture was taken with it); NO_POWERTUNE drops PowerTune's
 * power containment, CAC, SQ ramping and DTE; NO_SMC_FAN leaves the fan on
 * the hardware's own curve; NO_MC_AC_TIMING keeps one memory timing set.
 */
#define RDN_SI_DPM_NO_SCLK_DEEP_SLEEP	(1u << 0)
#define RDN_SI_DPM_NO_POWERTUNE		(1u << 1)
#define RDN_SI_DPM_NO_SMC_FAN		(1u << 2)
#define RDN_SI_DPM_NO_MC_AC_TIMING	(1u << 3)
/*
 * Firebrand's additions (not RadeonNI's; docs/si-bring-up.md,
 * "DPM"). The memory clock choices and the trace are for bring-up and stay
 * as switches; RADEON_RULES gives radeon's si_dpm.c as RadeonNI ran it,
 * without what amdgpu's si_dpm.c has since added (DisableULV sent whether or
 * not ULV is supported, all three of si_upload_smc_data's soft registers
 * written, the memory clock kept still at high pixel clocks, no state
 * switch when the adjusted state is the one the SMC runs).
 */
#define RDN_SI_DPM_MCLK_FIXED_HIGH	(1u << 4)	/* every level at the state's highest memory clock (Linux's disable_mclk_switching) */
#define RDN_SI_DPM_MCLK_FIXED_BOOT	(1u << 5)	/* every level at the boot memory clock: the MPLL is never reprogrammed */
#define RDN_SI_DPM_SWITCH_NO_DISPLAY	(1u << 6)	/* the SMC is told there is no display: gap ignored, low level forced */
#define RDN_SI_DPM_RADEON_RULES		(1u << 7)
#define RDN_SI_DPM_SWITCH_TRACE		(1u << 8)	/* for the caller: its message hook samples the registers while a state switch is pending */
void rdn_si_dpm_set_quirks(struct rdn_si_dpm *dpm, unsigned quirks);

/*
 * A hook around every SMC message (Firebrand's): phase 0 before the message
 * is written (v = 0), 1 while its answer is awaited (v = the wait loop's
 * count, about one a microsecond; called every 64th), 2 with the answer
 * (v = SMC_RESP_0).
 */
typedef void (*rdn_si_dpm_msg_hook)(void *ctx, uint32_t msg, int phase, uint32_t v);
void rdn_si_dpm_set_msg_hook(struct rdn_si_dpm *dpm, rdn_si_dpm_msg_hook hook, void *ctx);

/* What si_apply_state_adjust_rules() decided for the state last requested: 1 the memory clock is held (and why, a static string). */
int rdn_si_dpm_mclk_held(const struct rdn_si_dpm *dpm, const char **why);

/* The requested state's lowest and highest clocks (ni_dpm_get_sclk/mclk), for the display watermarks. */
void rdn_si_dpm_clock_range(struct rdn_si_dpm *dpm, uint32_t *sclk_low, uint32_t *mclk_low,
			    uint32_t *sclk_high, uint32_t *mclk_high);

/*
 * Where the firmware header put the tables (SMC addresses), for comparing
 * the SRAM against a Linux session's dump: 0 state table, 1 soft
 * registers, 2 MC register table, 3 fan table, 4 arbiter table, 5 CAC
 * table, 6 DTE table, 7 SPLL table, 8 PAPM parameters. 0 if unknown.
 */
uint32_t rdn_si_dpm_table_address(const struct rdn_si_dpm *dpm, unsigned which);

#endif /* RDN_SI_DPM_H */
