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
 * The VBIOS half of the DPM ports, shared by Sea Islands (rdn_ci_*.c) and
 * Southern Islands (rdn_si_dpm.c): radeon_mode.h's ATOM result structures
 * and the r600_dpm.c / radeon_atombios.c helpers rdn_ci_tables.c carries.
 * Nothing here names a family's register file (cikd.h or sid.h), so either
 * family's port can include it.
 */
#ifndef RDN_DPM_ATOM_H
#define RDN_DPM_ATOM_H

#include "rdn_ci_dpm.h"

/* An ATOM command table's parameter space: n dwords, built and read as bytes */
#define ATOM_PS_WORDS 8
union atom_ps {
	uint32_t w[ATOM_PS_WORDS];
	uint8_t b[ATOM_PS_WORDS * 4];
};

/*
 * 64-bit division without a runtime routine (the kext has none): Linux's
 * do_div sites in ci_dpm.c. Bit by bit; the operands are small.
 */
uint64_t ci_div64(uint64_t n, uint32_t d);

/* ---- radeon_mode.h ---- */

struct atom_clock_dividers {
	uint32_t post_div;
	uint32_t fb_div;	/* whole_fb_div << 14 | frac_fb_div, Linux's union */
	uint32_t whole_fb_div;
	uint32_t frac_fb_div;
	uint32_t ref_div;
	bool enable_post_div;
	bool enable_dithen;
	uint32_t vco_mode;
	uint32_t real_clock;
	/* added for CI */
	uint32_t post_divider;
	uint32_t flags;
};

struct atom_mpll_param {
	uint32_t clkf;
	uint32_t clkfrac;
	uint32_t post_div;
	uint32_t bwcntl;
	uint32_t dll_speed;
	uint32_t vco_mode;
	uint32_t yclk_sel;
	uint32_t qdr;
	uint32_t half_rate;
};

struct radeon_atom_ss {
	uint16_t percentage;
	uint16_t percentage_divider;
	uint8_t type;
	uint16_t step;
	uint8_t delay;
	uint8_t range;
	uint8_t refdiv;
	/* asic_ss */
	uint16_t rate;
	uint16_t amount;
};

struct radeon_gpio_rec {
	bool valid;
	uint8_t id;
	uint32_t reg;
	uint32_t mask;
	uint32_t shift;
};

struct atom_mc_reg_entry {
	uint32_t mclk_max;
	uint32_t mc_data[VBIOS_MC_REGISTER_ARRAY_SIZE];
};

struct atom_mc_register_address {
	uint16_t s1;
	uint8_t pre_reg_data;
};

/* the VBIOS-sized table radeon_atom_init_mc_reg_table() fills; ~2.7 KB, so from os->alloc */
struct atom_mc_reg_table {
	uint8_t last;
	uint8_t num_entries;
	struct atom_mc_reg_entry mc_reg_table_entry[VBIOS_MAX_AC_TIMING_ENTRIES];
	struct atom_mc_register_address mc_reg_address[VBIOS_MC_REGISTER_ARRAY_SIZE];
};

/* ---- rdn_ci_tables.c: r600_dpm.c, radeon_atombios.c, rv770_dpm.c, si_dpm.c, cik.c ---- */

int ci_get_vbios_boot_values(struct rdn_ci_dpm *dpm, struct ci_vbios_boot_state *boot_state);
int r600_get_platform_caps(struct rdn_ci_dpm *dpm);
int r600_parse_extended_power_table(struct rdn_ci_dpm *dpm);
void r600_free_extended_power_table(struct rdn_ci_dpm *dpm);
int ci_parse_power_table(struct rdn_ci_dpm *dpm);
void ci_free_power_table(struct rdn_ci_dpm *dpm);
int ci_parse_firmware_info(struct rdn_ci_dpm *dpm);

enum rdn_ci_pcie_gen r600_get_pcie_gen_support(uint32_t sys_mask, enum rdn_ci_pcie_gen asic_gen,
					       enum rdn_ci_pcie_gen default_gen);
uint16_t r600_get_pcie_lane_support(uint16_t asic_lanes, uint16_t default_lanes);
uint8_t r600_encode_pci_lane_width(uint32_t lanes);
uint16_t ci_get_current_pcie_speed(struct rdn_card *card);
int ci_get_current_pcie_lane_number(struct rdn_card *card);

struct radeon_gpio_rec radeon_atombios_lookup_gpio(struct rdn_card *card, uint8_t id);
bool radeon_atombios_get_asic_ss_info(struct rdn_ci_dpm *dpm, struct radeon_atom_ss *ss, int id, uint32_t clock);
bool radeon_atombios_has_asic_ss_info(struct rdn_card *card);
int radeon_atom_get_clock_dividers(struct rdn_card *card, uint8_t clock_type, uint32_t clock,
				   bool strobe_mode, struct atom_clock_dividers *dividers);
int radeon_atom_get_memory_pll_dividers(struct rdn_card *card, uint32_t clock, bool strobe_mode,
					struct atom_mpll_param *mpll_param);
void radeon_atom_set_engine_dram_timings(struct rdn_card *card, uint32_t eng_clock, uint32_t mem_clock);
int radeon_atom_get_voltage_table(struct rdn_card *card, uint8_t voltage_type, uint8_t voltage_mode,
				  struct atom_voltage_table *voltage_table);
bool radeon_atom_is_voltage_gpio(struct rdn_card *card, uint8_t voltage_type, uint8_t voltage_mode);
int radeon_atom_init_mc_reg_table(struct rdn_card *card, uint8_t module_index,
				  struct atom_mc_reg_table *reg_table);
int radeon_atom_get_leakage_id_from_vbios(struct rdn_card *card, uint16_t *leakage_id);
int radeon_atom_get_leakage_vddc_based_on_leakage_params(struct rdn_card *card, uint16_t *vddc,
							 uint16_t *vddci, uint16_t virtual_voltage_id,
							 uint16_t vbios_voltage_id);
int radeon_atom_get_voltage_evv(struct rdn_ci_dpm *dpm, uint16_t virtual_voltage_id, uint16_t *voltage);

uint8_t rv770_get_memory_module_index(struct rdn_card *card);
/*
 * `request`: end with ni_dpm.c's MC_CG_CONFIG / MC_ARB_CG (CG_ARB_REQ) writes,
 * which ask the memory controller to switch to the destination set. The
 * Sea Islands port ran without them (verified on the W8100 that way); they
 * are off there unless rdn_ci_dpm.arb_switch_request is set (untested).
 */
int ni_copy_and_switch_arb_sets(struct rdn_card *card, uint32_t arb_freq_src, uint32_t arb_freq_dest,
				bool request);
uint8_t si_get_ddr3_mclk_frequency_ratio(uint32_t memory_clock);
uint8_t si_get_mclk_frequency_ratio(uint32_t memory_clock, bool strobe_mode);
void si_trim_voltage_table_to_fit_state_table(uint32_t max_voltage_steps,
					      struct atom_voltage_table *voltage_table);
uint32_t cik_get_xclk(struct rdn_ci_dpm *dpm);
void cik_enter_rlc_safe_mode(struct rdn_card *card);
void cik_exit_rlc_safe_mode(struct rdn_card *card);

#endif /* RDN_DPM_ATOM_H */
