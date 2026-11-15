/*
 * Copyright 2007-8 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
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
 * Authors: Dave Airlie, Alex Deucher (the Linux radeon driver's
 * radeon_atombios.c, r600_dpm.c, rv770_dpm.c, ni_dpm.c, si_dpm.c and cik.c,
 * which this follows)
 */

/*
 * The VBIOS side of Sea Islands power management: the PowerPlay table and
 * its extended tables, the power states, and the ATOM data and command
 * tables ci_dpm.c consults (FirmwareInfo, GPIO_Pin_LUT, ASIC_InternalSS_Info,
 * VoltageObjectInfo 3.1, ASIC_ProfilingInfo 2.1, VRAM_Info 2.1,
 * ComputeMemoryEnginePLL 1.6, ComputeMemoryClockParam 2.1,
 * DynamicMemorySettings, SetVoltage 1.3/1.4, GetVoltageInfo 1.2).
 *
 * Every VBIOS field is read little-endian from a byte pointer; the packed
 * structures in atombios.h and pptable.h supply only offsetof()/sizeof().
 * Only the table revisions Linux handles for Sea Islands are handled; an
 * older one is refused with a log line rather than guessed at.
 */

#include "rdn_ci_internal.h"
#include "atom/atom.h"
#include "atom/atombios.h"
#include "linux/pptable.h"

/* ---- memory ---- */

static void *ci_alloc(struct rdn_ci_dpm *dpm, size_t size)
{
	return dpm->card->os->alloc(dpm->card->os->cookie, size);
}

static void ci_free(struct rdn_ci_dpm *dpm, void *p)
{
	if (p)
		dpm->card->os->free(dpm->card->os->cookie, p);
}

uint64_t ci_div64(uint64_t n, uint32_t d)
{
	uint64_t q = 0, r = 0;
	int i;

	for (i = 63; i >= 0; i--) {
		r = (r << 1) | ((n >> i) & 1);
		if (r >= d) {
			r -= d;
			q |= (uint64_t)1 << i;
		}
	}
	return q;
}

/* The start of a data table in the VBIOS image, or NULL. */
static const uint8_t *ci_data_table(struct rdn_card *card, int index, uint16_t *size,
				    uint8_t *frev, uint8_t *crev)
{
	uint16_t data_offset = 0;

	if (!atom_parse_data_header(card->atom.ctx, index, size, frev, crev, &data_offset))
		return NULL;
	return (const uint8_t *)card->atom.ctx->bios + data_offset;
}

/* ---- cik.c ---- */

/* cik_get_xclk(): the reference clock the gfx engine uses (CIK, dGPU branch) */
uint32_t cik_get_xclk(struct rdn_ci_dpm *dpm)
{
	uint32_t reference_clock = dpm->reference_freq;

	if (RREG32_SMC(dpm->card, CG_CLKPIN_CNTL) & XTALIN_DIVIDE)
		return reference_clock / 4;
	return reference_clock;
}

void cik_enter_rlc_safe_mode(struct rdn_card *card)
{
	uint32_t tmp, i, mask;

	tmp = REQ | MESSAGE(MSG_ENTER_RLC_SAFE_MODE);
	WREG32(card, RLC_GPR_REG2, tmp);

	mask = GFX_POWER_STATUS | GFX_CLOCK_STATUS;
	for (i = 0; i < CI_USEC_TIMEOUT; i++) {
		if ((RREG32(card, RLC_GPM_STAT) & mask) == mask)
			break;
		udelay(card, 1);
	}

	for (i = 0; i < CI_USEC_TIMEOUT; i++) {
		if ((RREG32(card, RLC_GPR_REG2) & REQ) == 0)
			break;
		udelay(card, 1);
	}
}

void cik_exit_rlc_safe_mode(struct rdn_card *card)
{
	uint32_t tmp;

	tmp = REQ | MESSAGE(MSG_EXIT_RLC_SAFE_MODE);
	WREG32(card, RLC_GPR_REG2, tmp);
}

/* ---- rv770_dpm.c, ni_dpm.c, si_dpm.c ---- */

uint8_t rv770_get_memory_module_index(struct rdn_card *card)
{
	return (uint8_t)((RREG32(card, BIOS_SCRATCH_4) >> 16) & 0xff);
}

int ni_copy_and_switch_arb_sets(struct rdn_card *card, uint32_t arb_freq_src, uint32_t arb_freq_dest,
				bool request)
{
	uint32_t mc_arb_dram_timing;
	uint32_t mc_arb_dram_timing2;
	uint32_t burst_time;

	switch (arb_freq_src) {
	case MC_CG_ARB_FREQ_F0:
		mc_arb_dram_timing  = RREG32(card, MC_ARB_DRAM_TIMING);
		mc_arb_dram_timing2 = RREG32(card, MC_ARB_DRAM_TIMING2);
		burst_time = (RREG32(card, MC_ARB_BURST_TIME) & STATE0_MASK) >> STATE0_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F1:
		mc_arb_dram_timing  = RREG32(card, MC_ARB_DRAM_TIMING_1);
		mc_arb_dram_timing2 = RREG32(card, MC_ARB_DRAM_TIMING2_1);
		burst_time = (RREG32(card, MC_ARB_BURST_TIME) & STATE1_MASK) >> STATE1_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F2:
		mc_arb_dram_timing  = RREG32(card, MC_ARB_DRAM_TIMING_2);
		mc_arb_dram_timing2 = RREG32(card, MC_ARB_DRAM_TIMING2_2);
		burst_time = (RREG32(card, MC_ARB_BURST_TIME) & STATE2_MASK) >> STATE2_SHIFT;
		break;
	case MC_CG_ARB_FREQ_F3:
		mc_arb_dram_timing  = RREG32(card, MC_ARB_DRAM_TIMING_3);
		mc_arb_dram_timing2 = RREG32(card, MC_ARB_DRAM_TIMING2_3);
		burst_time = (RREG32(card, MC_ARB_BURST_TIME) & STATE3_MASK) >> STATE3_SHIFT;
		break;
	default:
		return -EINVAL;
	}

	switch (arb_freq_dest) {
	case MC_CG_ARB_FREQ_F0:
		WREG32(card, MC_ARB_DRAM_TIMING, mc_arb_dram_timing);
		WREG32(card, MC_ARB_DRAM_TIMING2, mc_arb_dram_timing2);
		WREG32_P(card, MC_ARB_BURST_TIME, STATE0(burst_time), ~STATE0_MASK);
		break;
	case MC_CG_ARB_FREQ_F1:
		WREG32(card, MC_ARB_DRAM_TIMING_1, mc_arb_dram_timing);
		WREG32(card, MC_ARB_DRAM_TIMING2_1, mc_arb_dram_timing2);
		WREG32_P(card, MC_ARB_BURST_TIME, STATE1(burst_time), ~STATE1_MASK);
		break;
	case MC_CG_ARB_FREQ_F2:
		WREG32(card, MC_ARB_DRAM_TIMING_2, mc_arb_dram_timing);
		WREG32(card, MC_ARB_DRAM_TIMING2_2, mc_arb_dram_timing2);
		WREG32_P(card, MC_ARB_BURST_TIME, STATE2(burst_time), ~STATE2_MASK);
		break;
	case MC_CG_ARB_FREQ_F3:
		WREG32(card, MC_ARB_DRAM_TIMING_3, mc_arb_dram_timing);
		WREG32(card, MC_ARB_DRAM_TIMING2_3, mc_arb_dram_timing2);
		WREG32_P(card, MC_ARB_BURST_TIME, STATE3(burst_time), ~STATE3_MASK);
		break;
	default:
		return -EINVAL;
	}

	/* ni_dpm.c's switch request; off by default on CIK (see rdn_dpm_atom.h) */
	if (request) {
		rdn_wreg(card, MC_CG_CONFIG, rdn_rreg(card, MC_CG_CONFIG) | 0x0000000F);
		WREG32_P(card, MC_ARB_CG, CG_ARB_REQ(arb_freq_dest), ~CG_ARB_REQ_MASK);
	}
	return 0;
}

uint8_t si_get_ddr3_mclk_frequency_ratio(uint32_t memory_clock)
{
	uint8_t mc_para_index;

	if (memory_clock < 10000)
		mc_para_index = 0;
	else if (memory_clock >= 80000)
		mc_para_index = 0x0f;
	else
		mc_para_index = (uint8_t)((memory_clock - 10000) / 5000 + 1);
	return mc_para_index;
}

uint8_t si_get_mclk_frequency_ratio(uint32_t memory_clock, bool strobe_mode)
{
	uint8_t mc_para_index;

	if (strobe_mode) {
		if (memory_clock < 12500)
			mc_para_index = 0x00;
		else if (memory_clock > 47500)
			mc_para_index = 0x0f;
		else
			mc_para_index = (uint8_t)((memory_clock - 10000) / 2500);
	} else {
		if (memory_clock < 65000)
			mc_para_index = 0x00;
		else if (memory_clock > 135000)
			mc_para_index = 0x0f;
		else
			mc_para_index = (uint8_t)((memory_clock - 60000) / 5000);
	}
	return mc_para_index;
}

void si_trim_voltage_table_to_fit_state_table(uint32_t max_voltage_steps,
					      struct atom_voltage_table *voltage_table)
{
	unsigned int i, diff;

	if (voltage_table->count <= max_voltage_steps)
		return;

	diff = voltage_table->count - max_voltage_steps;

	for (i = 0; i < max_voltage_steps; i++)
		voltage_table->entries[i] = voltage_table->entries[i + diff];

	voltage_table->count = max_voltage_steps;
}

/* ---- r600_dpm.c: the PCIe helpers ---- */

enum rdn_ci_pcie_gen r600_get_pcie_gen_support(uint32_t sys_mask, enum rdn_ci_pcie_gen asic_gen,
					       enum rdn_ci_pcie_gen default_gen)
{
	switch (asic_gen) {
	case RDN_CI_PCIE_GEN1:
		return RDN_CI_PCIE_GEN1;
	case RDN_CI_PCIE_GEN2:
		return RDN_CI_PCIE_GEN2;
	case RDN_CI_PCIE_GEN3:
		return RDN_CI_PCIE_GEN3;
	default:
		if ((sys_mask & RDN_CI_PCIE_SPEED_80) && (default_gen == RDN_CI_PCIE_GEN3))
			return RDN_CI_PCIE_GEN3;
		else if ((sys_mask & RDN_CI_PCIE_SPEED_50) && (default_gen == RDN_CI_PCIE_GEN2))
			return RDN_CI_PCIE_GEN2;
		else
			return RDN_CI_PCIE_GEN1;
	}
}

uint16_t r600_get_pcie_lane_support(uint16_t asic_lanes, uint16_t default_lanes)
{
	switch (asic_lanes) {
	case 0:
	default:
		return default_lanes;
	case 1:
		return 1;
	case 2:
		return 2;
	case 4:
		return 4;
	case 8:
		return 8;
	case 12:
		return 12;
	case 16:
		return 16;
	}
}

uint8_t r600_encode_pci_lane_width(uint32_t lanes)
{
	static const uint8_t encoded_lanes[] = {
		0, 1, 2, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0, 6
	};

	if (lanes > 16)
		return 0;

	return encoded_lanes[lanes];
}

/* ci_dpm.c */
uint16_t ci_get_current_pcie_speed(struct rdn_card *card)
{
	uint32_t speed_cntl = 0;

	speed_cntl = RREG32_PCIE_PORT(card, PCIE_LC_SPEED_CNTL) & LC_CURRENT_DATA_RATE_MASK;
	speed_cntl >>= LC_CURRENT_DATA_RATE_SHIFT;

	return (uint16_t)speed_cntl;
}

/* radeon.h's RADEON_PCIE_LC_LINK_WIDTH_X* */
#define RADEON_PCIE_LC_LINK_WIDTH_X0		0
#define RADEON_PCIE_LC_LINK_WIDTH_X1		1
#define RADEON_PCIE_LC_LINK_WIDTH_X2		2	/* radeon_reg.h: X2 is 2 and X4 is 3; they were swapped here until 2026-11-04 */
#define RADEON_PCIE_LC_LINK_WIDTH_X4		3
#define RADEON_PCIE_LC_LINK_WIDTH_X8		4
#define RADEON_PCIE_LC_LINK_WIDTH_X12		5
#define RADEON_PCIE_LC_LINK_WIDTH_X16		6

int ci_get_current_pcie_lane_number(struct rdn_card *card)
{
	uint32_t link_width = 0;

	link_width = RREG32_PCIE_PORT(card, PCIE_LC_LINK_WIDTH_CNTL) & LC_LINK_WIDTH_RD_MASK;
	link_width >>= LC_LINK_WIDTH_RD_SHIFT;

	switch (link_width) {
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

/* ---- FirmwareInfo ---- */

/*
 * ci_get_vbios_boot_values(): ATOM_FIRMWARE_INFO_V2_2's boot voltages and
 * clocks, and the PCIe link as it is now.
 */
int ci_get_vbios_boot_values(struct rdn_ci_dpm *dpm, struct ci_vbios_boot_state *boot_state)
{
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	const uint8_t *firmware_info;
	uint16_t size = 0;
	uint8_t frev = 0, crev = 0;

	firmware_info = ci_data_table(dpm->card, index, &size, &frev, &crev);
	if (firmware_info) {
		if (size < sizeof(ATOM_FIRMWARE_INFO_V2_2)) {
			rdn_log(dpm->card->os, RDN_LOG_ERROR, "ci_dpm: FirmwareInfo %u.%u is %u bytes, need %u (V2_2)",
				frev, crev, size, (unsigned)sizeof(ATOM_FIRMWARE_INFO_V2_2));
			return -EINVAL;
		}
		boot_state->mvdd_bootup_value = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, usBootUpMVDDCVoltage));
		boot_state->vddc_bootup_value = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, usBootUpVDDCVoltage));
		boot_state->vddci_bootup_value = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, usBootUpVDDCIVoltage));
		boot_state->pcie_gen_bootup_value = ci_get_current_pcie_speed(dpm->card);
		boot_state->pcie_lane_bootup_value = (uint16_t)ci_get_current_pcie_lane_number(dpm->card);
		boot_state->sclk_bootup_value = le32_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultEngineClock));
		boot_state->mclk_bootup_value = le32_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultMemoryClock));

		return 0;
	}
	return -EINVAL;
}

/*
 * radeon_atombios_get_clock_info()'s two words this code needs: the SPLL/MPLL
 * reference clock and the firmware capability flags (both at the same offset
 * in every FirmwareInfo revision, read through the V1 layout as Linux does).
 */
int ci_parse_firmware_info(struct rdn_ci_dpm *dpm)
{
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	const uint8_t *firmware_info;
	uint16_t size = 0;

	firmware_info = ci_data_table(dpm->card, index, &size, NULL, NULL);
	if (!firmware_info || size < offsetof(ATOM_FIRMWARE_INFO, usReferenceClock) + 2)
		return -EINVAL;
	dpm->reference_freq = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO, usReferenceClock));
	dpm->firmware_flags = le16_at(firmware_info, offsetof(ATOM_FIRMWARE_INFO, usFirmwareCapability));
	return 0;
}

/* ---- GPIO_Pin_LUT ---- */

struct radeon_gpio_rec radeon_atombios_lookup_gpio(struct rdn_card *card, uint8_t id)
{
	struct radeon_gpio_rec gpio;
	int index = GetIndexIntoMasterTable(DATA, GPIO_Pin_LUT);
	const uint8_t *gpio_info;
	uint16_t size = 0;
	int i, num_indices;

	memset(&gpio, 0, sizeof(struct radeon_gpio_rec));
	gpio.valid = false;

	gpio_info = ci_data_table(card, index, &size, NULL, NULL);
	if (gpio_info) {
		const uint8_t *pin;

		num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) /
			sizeof(ATOM_GPIO_PIN_ASSIGNMENT);

		pin = gpio_info + offsetof(ATOM_GPIO_PIN_LUT, asGPIO_Pin);
		for (i = 0; i < num_indices; i++) {
			if (id == u8_at(pin, offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGPIO_ID))) {
				gpio.id = u8_at(pin, offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGPIO_ID));
				gpio.reg = le16_at(pin, offsetof(ATOM_GPIO_PIN_ASSIGNMENT, usGpioPin_AIndex)) * 4;
				gpio.shift = u8_at(pin, offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGpioPinBitShift));
				gpio.mask = (1 << gpio.shift);
				gpio.valid = true;
				break;
			}
			pin += sizeof(ATOM_GPIO_PIN_ASSIGNMENT);
		}
	}

	return gpio;
}

/* ---- ASIC_InternalSS_Info ---- */

/* ci_dpm_init() only asks whether the table exists */
bool radeon_atombios_has_asic_ss_info(struct rdn_card *card)
{
	uint16_t size = 0;
	uint8_t frev = 0, crev = 0;

	return ci_data_table(card, GetIndexIntoMasterTable(DATA, ASIC_InternalSS_Info), &size, &frev, &crev) != NULL;
}

bool radeon_atombios_get_asic_ss_info(struct rdn_ci_dpm *dpm, struct radeon_atom_ss *ss, int id, uint32_t clock)
{
	int index = GetIndexIntoMasterTable(DATA, ASIC_InternalSS_Info);
	uint16_t size = 0;
	const uint8_t *ss_info;
	const uint8_t *ss_assign;
	uint8_t frev = 0, crev = 0;
	int i, num_indices;

	if (id == ASIC_INTERNAL_MEMORY_SS) {
		if (!(dpm->firmware_flags & ATOM_BIOS_INFO_MEMORY_CLOCK_SS_SUPPORT))
			return false;
	}
	if (id == ASIC_INTERNAL_ENGINE_SS) {
		if (!(dpm->firmware_flags & ATOM_BIOS_INFO_ENGINE_CLOCK_SS_SUPPORT))
			return false;
	}

	memset(ss, 0, sizeof(struct radeon_atom_ss));
	ss_info = ci_data_table(dpm->card, index, &size, &frev, &crev);
	if (ss_info) {
		/* the three assignment layouts share their field offsets and size (12 bytes) */
		switch (frev) {
		case 1:
			num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) /
				sizeof(ATOM_ASIC_SS_ASSIGNMENT);

			ss_assign = ss_info + offsetof(ATOM_ASIC_INTERNAL_SS_INFO, asSpreadSpectrum);
			for (i = 0; i < num_indices; i++) {
				if ((u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT, ucClockIndication)) == id) &&
				    (clock <= le32_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT, ulTargetClockRange)))) {
					ss->percentage =
						le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT, usSpreadSpectrumPercentage));
					ss->type = u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT, ucSpreadSpectrumMode));
					ss->rate = le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT, usSpreadRateInKhz));
					ss->percentage_divider = 100;
					return true;
				}
				ss_assign += sizeof(ATOM_ASIC_SS_ASSIGNMENT);
			}
			break;
		case 2:
			num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) /
				sizeof(ATOM_ASIC_SS_ASSIGNMENT_V2);
			ss_assign = ss_info + offsetof(ATOM_ASIC_INTERNAL_SS_INFO_V2, asSpreadSpectrum);
			for (i = 0; i < num_indices; i++) {
				if ((u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V2, ucClockIndication)) == id) &&
				    (clock <= le32_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V2, ulTargetClockRange)))) {
					ss->percentage =
						le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V2, usSpreadSpectrumPercentage));
					ss->type = u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V2, ucSpreadSpectrumMode));
					ss->rate = le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V2, usSpreadRateIn10Hz));
					ss->percentage_divider = 100;
					if ((crev == 2) &&
					    ((id == ASIC_INTERNAL_ENGINE_SS) ||
					     (id == ASIC_INTERNAL_MEMORY_SS)))
						ss->rate /= 100;
					return true;
				}
				ss_assign += sizeof(ATOM_ASIC_SS_ASSIGNMENT_V2);
			}
			break;
		case 3:
			num_indices = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) /
				sizeof(ATOM_ASIC_SS_ASSIGNMENT_V3);
			ss_assign = ss_info + offsetof(ATOM_ASIC_INTERNAL_SS_INFO_V3, asSpreadSpectrum);
			for (i = 0; i < num_indices; i++) {
				if ((u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ucClockIndication)) == id) &&
				    (clock <= le32_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ulTargetClockRange)))) {
					ss->percentage =
						le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, usSpreadSpectrumPercentage));
					ss->type = u8_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ucSpreadSpectrumMode));
					ss->rate = le16_at(ss_assign, offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, usSpreadRateIn10Hz));
					if (ss->type & SS_MODE_V3_PERCENTAGE_DIV_BY_1000_MASK)
						ss->percentage_divider = 1000;
					else
						ss->percentage_divider = 100;
					if ((id == ASIC_INTERNAL_ENGINE_SS) ||
					    (id == ASIC_INTERNAL_MEMORY_SS))
						ss->rate /= 100;
					/* Linux applies radeon_atombios_get_igp_ss_overrides() to IGPs here; this is a dGPU */
					return true;
				}
				ss_assign += sizeof(ATOM_ASIC_SS_ASSIGNMENT_V3);
			}
			break;
		default:
			rdn_log(dpm->card->os, RDN_LOG_ERROR, "Unsupported ASIC_InternalSS_Info table: %d %d", frev, crev);
			break;
		}

	}
	return false;
}

/* ---- ComputeMemoryEnginePLL, ComputeMemoryClockParam, DynamicMemorySettings ---- */

static int ci_exec(struct rdn_card *card, int index, union atom_ps *ps, unsigned bytes)
{
	int r = atom_execute_table(card->atom.ctx, index, ps->w, (int)bytes);

	if (r)
		rdn_log(card->os, RDN_LOG_ERROR, "ci_dpm: AtomBIOS command table %d failed (%d)", index, r);
	return r;
}

/*
 * radeon_atom_get_clock_dividers(): the revision Sea Islands VBIOSes carry is
 * 1.6 (COMPUTE_GPU_CLOCK_INPUT/OUTPUT_PARAMETERS_V1_6); Linux's other
 * revisions are for other families and are refused here.
 */
int radeon_atom_get_clock_dividers(struct rdn_card *card, uint8_t clock_type, uint32_t clock,
				   bool strobe_mode, struct atom_clock_dividers *dividers)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL);
	uint8_t frev = 0, crev = 0;
	uint32_t dword0;
	int r;

	(void)strobe_mode;
	memset(&args, 0, sizeof(args));
	memset(dividers, 0, sizeof(struct atom_clock_dividers));

	if (!atom_parse_cmd_header(card->atom.ctx, index, &frev, &crev))
		return -EINVAL;

	switch (crev) {
	case 6:
		/* CI */
		/* COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK, COMPUTE_GPUCLK_INPUT_FLAG_SCLK */
		/* ATOM_COMPUTE_CLOCK_FREQ: ulClockFreq in bits 23:0, ulComputeClockFlag in 31:24 */
		put_le32(args.b, 0, ((uint32_t)clock_type << 24) | (clock & 0x00ffffff));	/* 10 khz */

		r = ci_exec(card, index, &args, sizeof(COMPUTE_GPU_CLOCK_INPUT_PARAMETERS_V1_6));
		if (r)
			return r;

		dword0 = le32_at(args.b, 0);
		dividers->whole_fb_div = le16_at(args.b, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ulFbDiv) +
							 offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDiv));
		dividers->frac_fb_div = le16_at(args.b, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ulFbDiv) +
							offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDivFrac));
		dividers->fb_div = (dividers->whole_fb_div << 14) | (dividers->frac_fb_div & 0x3fff);
		dividers->ref_div = u8_at(args.b, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ucPllRefDiv));
		dividers->post_div = u8_at(args.b, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ucPllPostDiv));
		dividers->flags = u8_at(args.b, offsetof(COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6, ucPllCntlFlag));
		dividers->real_clock = dword0 & 0x00ffffff;	/* COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V4.ulClock */
		dividers->post_divider = dword0 >> 24;		/* .ucPostDiv */
		break;
	default:
		rdn_log(card->os, RDN_LOG_ERROR, "ci_dpm: ComputeMemoryEnginePLL is revision %u.%u; only 1.6 (Sea Islands) is handled",
			frev, crev);
		return -EINVAL;
	}
	return 0;
}

/* radeon_atom_get_memory_pll_dividers(): ComputeMemoryClockParam 2.1 (SI and CI) */
int radeon_atom_get_memory_pll_dividers(struct rdn_card *card, uint32_t clock, bool strobe_mode,
					struct atom_mpll_param *mpll_param)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, ComputeMemoryClockParam);
	uint8_t frev = 0, crev = 0;
	uint8_t cntl_flag;
	int r;

	memset(&args, 0, sizeof(args));
	memset(mpll_param, 0, sizeof(struct atom_mpll_param));

	if (!atom_parse_cmd_header(card->atom.ctx, index, &frev, &crev))
		return -EINVAL;

	switch (frev) {
	case 2:
		switch (crev) {
		case 1:
			/* SI */
			put_le32(args.b, 0, clock);	/* 10 khz */
			args.b[offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucInputFlag)] = 0;
			if (strobe_mode)
				args.b[offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucInputFlag)] |=
					MPLL_INPUT_FLAG_STROBE_MODE_EN;

			r = ci_exec(card, index, &args, sizeof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1));
			if (r)
				return r;

			mpll_param->clkfrac = le16_at(args.b, offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDivFrac));
			mpll_param->clkf = le16_at(args.b, offsetof(ATOM_S_MPLL_FB_DIVIDER, usFbDiv));
			mpll_param->post_div = u8_at(args.b, offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucPostDiv));
			mpll_param->dll_speed = u8_at(args.b, offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucDllSpeed));
			mpll_param->bwcntl = u8_at(args.b, offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucBWCntl));
			cntl_flag = u8_at(args.b, offsetof(COMPUTE_MEMORY_CLOCK_PARAM_PARAMETERS_V2_1, ucPllCntlFlag));
			mpll_param->vco_mode =
				(cntl_flag & MPLL_CNTL_FLAG_VCO_MODE_MASK);
			mpll_param->yclk_sel =
				(cntl_flag & MPLL_CNTL_FLAG_BYPASS_DQ_PLL) ? 1 : 0;
			mpll_param->qdr =
				(cntl_flag & MPLL_CNTL_FLAG_QDR_ENABLE) ? 1 : 0;
			mpll_param->half_rate =
				(cntl_flag & MPLL_CNTL_FLAG_AD_HALF_RATE) ? 1 : 0;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

/* radeon_atom_set_engine_dram_timings(): DynamicMemorySettings with an engine and a memory clock */
void radeon_atom_set_engine_dram_timings(struct rdn_card *card, uint32_t eng_clock, uint32_t mem_clock)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, DynamicMemorySettings);
	uint32_t tmp;

	memset(&args, 0, sizeof(args));

	tmp = eng_clock & SET_CLOCK_FREQ_MASK;
	tmp |= (COMPUTE_ENGINE_PLL_PARAM << 24);

	put_le32(args.b, offsetof(SET_ENGINE_CLOCK_PS_ALLOCATION, ulTargetEngineClock), tmp);
	if (mem_clock)
		put_le32(args.b, offsetof(SET_ENGINE_CLOCK_PS_ALLOCATION, sReserved.ulClock),
			 mem_clock & SET_CLOCK_FREQ_MASK);

	ci_exec(card, index, &args, sizeof(SET_ENGINE_CLOCK_PS_ALLOCATION));
}

/* ---- VoltageObjectInfo 3.1 ---- */

/*
 * atom_lookup_voltage_object_v3(): the object of this type and mode, as a
 * byte pointer into the table, or NULL.
 */
static const uint8_t *atom_lookup_voltage_object_v3(const uint8_t *v3, uint8_t voltage_type, uint8_t voltage_mode)
{
	uint32_t size = le16_at(v3, offsetof(ATOM_COMMON_TABLE_HEADER, usStructureSize));
	uint32_t offset = offsetof(ATOM_VOLTAGE_OBJECT_INFO_V3_1, asVoltageObj);

	while (offset < size) {
		const uint8_t *vo = v3 + offset;
		uint16_t vo_size = le16_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, usSize));

		if ((u8_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageType)) == voltage_type) &&
		    (u8_at(vo, offsetof(ATOM_VOLTAGE_OBJECT_HEADER_V3, ucVoltageMode)) == voltage_mode))
			return vo;
		if (vo_size == 0)
			break;	/* a malformed table would loop for ever */
		offset += vo_size;
	}
	return NULL;
}

/*
 * radeon_atom_is_voltage_gpio(): true when the VBIOS has a voltage object of
 * this type and control mode. Sea Islands VBIOSes carry VoltageObjectInfo
 * 3.1; Linux's 1.x/2.x branches (VOLTAGE_CONTROLLED_BY_GPIO) are for older
 * families and are not handled here.
 */
bool radeon_atom_is_voltage_gpio(struct rdn_card *card, uint8_t voltage_type, uint8_t voltage_mode)
{
	int index = GetIndexIntoMasterTable(DATA, VoltageObjectInfo);
	uint8_t frev = 0, crev = 0;
	uint16_t size = 0;
	const uint8_t *voltage_info;

	voltage_info = ci_data_table(card, index, &size, &frev, &crev);
	if (voltage_info) {
		switch (frev) {
		case 3:
			switch (crev) {
			case 1:
				if (atom_lookup_voltage_object_v3(voltage_info, voltage_type, voltage_mode))
					return true;
				break;
			default:
				rdn_log(card->os, RDN_LOG_ERROR, "unknown voltage object table (%u.%u)", frev, crev);
				return false;
			}
			break;
		default:
			rdn_log(card->os, RDN_LOG_ERROR, "voltage object table %u.%u is not the 3.1 Sea Islands carry", frev, crev);
			return false;
		}

	}
	return false;
}

/* radeon_atom_get_voltage_table(): the GPIO look-up table of a 3.1 voltage object */
int radeon_atom_get_voltage_table(struct rdn_card *card, uint8_t voltage_type, uint8_t voltage_mode,
				  struct atom_voltage_table *voltage_table)
{
	int index = GetIndexIntoMasterTable(DATA, VoltageObjectInfo);
	uint8_t frev = 0, crev = 0;
	uint16_t size = 0;
	int i;
	const uint8_t *voltage_info;
	const uint8_t *voltage_object;

	voltage_info = ci_data_table(card, index, &size, &frev, &crev);
	if (voltage_info) {
		switch (frev) {
		case 3:
			switch (crev) {
			case 1:
				voltage_object = atom_lookup_voltage_object_v3(voltage_info, voltage_type, voltage_mode);
				if (voltage_object) {
					const uint8_t *gpio = voltage_object;
					const uint8_t *lut;
					uint8_t n = u8_at(gpio, offsetof(ATOM_GPIO_VOLTAGE_OBJECT_V3, ucGpioEntryNum));

					if (n > MAX_VOLTAGE_ENTRIES)
						return -EINVAL;
					lut = gpio + offsetof(ATOM_GPIO_VOLTAGE_OBJECT_V3, asVolGpioLut);
					for (i = 0; i < n; i++) {
						voltage_table->entries[i].value =
							le16_at(lut, offsetof(VOLTAGE_LUT_ENTRY_V2, usVoltageValue));
						voltage_table->entries[i].smio_low =
							le32_at(lut, offsetof(VOLTAGE_LUT_ENTRY_V2, ulVoltageId));
						lut += sizeof(VOLTAGE_LUT_ENTRY_V2);
					}
					voltage_table->mask_low = le32_at(gpio, offsetof(ATOM_GPIO_VOLTAGE_OBJECT_V3, ulGpioMaskVal));
					voltage_table->count = n;
					voltage_table->phase_delay = u8_at(gpio, offsetof(ATOM_GPIO_VOLTAGE_OBJECT_V3, ucPhaseDelay));
					return 0;
				}
				break;
			default:
				rdn_log(card->os, RDN_LOG_ERROR, "unknown voltage object table (%u.%u)", frev, crev);
				return -EINVAL;
			}
			break;
		default:
			rdn_log(card->os, RDN_LOG_ERROR, "voltage object table %u.%u is not the 3.1 Sea Islands carry", frev, crev);
			return -EINVAL;
		}
	}
	return -EINVAL;
}

/* ---- SetVoltage 1.3/1.4, GetVoltageInfo 1.2, ASIC_ProfilingInfo 2.1: the leakage voltages ---- */

int radeon_atom_get_leakage_id_from_vbios(struct rdn_card *card, uint16_t *leakage_id)
{
	union atom_ps args;
	int index = GetIndexIntoMasterTable(COMMAND, SetVoltage);
	uint8_t frev = 0, crev = 0;
	int r;

	if (!atom_parse_cmd_header(card->atom.ctx, index, &frev, &crev))
		return -EINVAL;

	memset(&args, 0, sizeof(args));
	switch (crev) {
	case 3:
	case 4:
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V1_3, ucVoltageType)] = 0;
		args.b[offsetof(SET_VOLTAGE_PARAMETERS_V1_3, ucVoltageMode)] = ATOM_GET_LEAKAGE_ID;
		put_le16(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V1_3, usVoltageLevel), 0);

		r = ci_exec(card, index, &args, sizeof(SET_VOLTAGE_PARAMETERS_V1_3));
		if (r)
			return r;

		*leakage_id = le16_at(args.b, offsetof(SET_VOLTAGE_PARAMETERS_V1_3, usVoltageLevel));
		break;
	default:
		rdn_log(card->os, RDN_LOG_ERROR, "Unknown table version %d, %d", frev, crev);
		return -EINVAL;
	}

	return 0;
}

int radeon_atom_get_leakage_vddc_based_on_leakage_params(struct rdn_card *card, uint16_t *vddc,
							 uint16_t *vddci, uint16_t virtual_voltage_id,
							 uint16_t vbios_voltage_id)
{
	int index = GetIndexIntoMasterTable(DATA, ASIC_ProfilingInfo);
	uint8_t frev = 0, crev = 0;
	uint16_t size = 0;
	int i, j;
	const uint8_t *profile;
	const uint8_t *leakage_bin, *vddc_id_buf, *vddc_buf, *vddci_id_buf, *vddci_buf;
	uint8_t num_bins, num_vddc, num_vddci;

	*vddc = 0;
	*vddci = 0;

	profile = ci_data_table(card, index, &size, &frev, &crev);
	if (!profile)
		return -EINVAL;

	switch (frev) {
	case 1:
		return -EINVAL;
	case 2:
		switch (crev) {
		case 1:
			if (size < sizeof(ATOM_ASIC_PROFILING_INFO_V2_1))
				return -EINVAL;
			/*
			 * The five arrays are of little-endian u16; Linux indexes
			 * them as host u16 (a little-endian assumption), this
			 * reads them explicitly.
			 */
			leakage_bin = profile + le16_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, usLeakageBinArrayOffset));
			vddc_id_buf = profile + le16_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, usElbVDDC_IdArrayOffset));
			vddc_buf = profile + le16_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, usElbVDDC_LevelArrayOffset));
			vddci_id_buf = profile + le16_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, usElbVDDCI_IdArrayOffset));
			vddci_buf = profile + le16_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, usElbVDDCI_LevelArrayOffset));
			num_bins = u8_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, ucLeakageBinNum));
			num_vddc = u8_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, ucElbVDDC_Num));
			num_vddci = u8_at(profile, offsetof(ATOM_ASIC_PROFILING_INFO_V2_1, ucElbVDDCI_Num));

			if (num_vddc > 0) {
				for (i = 0; i < num_vddc; i++) {
					if (le16_at(vddc_id_buf, 2 * i) == virtual_voltage_id) {
						for (j = 0; j < num_bins; j++) {
							if (vbios_voltage_id <= le16_at(leakage_bin, 2 * j)) {
								*vddc = le16_at(vddc_buf, 2 * (j * num_vddc + i));
								break;
							}
						}
						break;
					}
				}
			}
			if (num_vddci > 0) {
				for (i = 0; i < num_vddci; i++) {
					if (le16_at(vddci_id_buf, 2 * i) == virtual_voltage_id) {
						for (j = 0; j < num_bins; j++) {
							if (vbios_voltage_id <= le16_at(leakage_bin, 2 * j)) {
								*vddci = le16_at(vddci_buf, 2 * (j * num_vddci + i));
								break;
							}
						}
						break;
					}
				}
			}
			break;
		default:
			rdn_log(card->os, RDN_LOG_ERROR, "Unknown table version %d, %d", frev, crev);
			return -EINVAL;
		}
		break;
	default:
		rdn_log(card->os, RDN_LOG_ERROR, "Unknown table version %d, %d", frev, crev);
		return -EINVAL;
	}

	return 0;
}

/* radeon_atom_get_voltage_evv(): GetVoltageInfo 1.2 for one virtual voltage id (the EVV boards, Hawaii among them) */
int radeon_atom_get_voltage_evv(struct rdn_ci_dpm *dpm, uint16_t virtual_voltage_id, uint16_t *voltage)
{
	int index = GetIndexIntoMasterTable(COMMAND, GetVoltageInfo);
	uint32_t entry_id;
	uint32_t count = dpm->dyn_state.vddc_dependency_on_sclk.count;
	union atom_ps args;
	int r;

	for (entry_id = 0; entry_id < count; entry_id++) {
		if (dpm->dyn_state.vddc_dependency_on_sclk.entries[entry_id].v ==
		    virtual_voltage_id)
			break;
	}

	if (entry_id >= count)
		return -EINVAL;

	memset(&args, 0, sizeof(args));
	args.b[offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_2, ucVoltageType)] = VOLTAGE_TYPE_VDDC;
	args.b[offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_2, ucVoltageMode)] = ATOM_GET_VOLTAGE_EVV_VOLTAGE;
	put_le16(args.b, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_2, usVoltageLevel), virtual_voltage_id);
	put_le32(args.b, offsetof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_2, ulSCLKFreq),
		 dpm->dyn_state.vddc_dependency_on_sclk.entries[entry_id].clk);

	r = ci_exec(dpm->card, index, &args, sizeof(GET_VOLTAGE_INFO_INPUT_PARAMETER_V1_2));
	if (r)
		return r;

	*voltage = le16_at(args.b, offsetof(GET_EVV_VOLTAGE_INFO_OUTPUT_PARAMETER_V1_2, usVoltageLevel));

	return 0;
}

/* ---- VRAM_Info 2.1: the memory controller's per-clock register table ---- */

#define MEM_ID_MASK           0xff000000
#define MEM_ID_SHIFT          24
#define CLOCK_RANGE_MASK      0x00ffffff
#define CLOCK_RANGE_SHIFT     0
#define LOW_NIBBLE_MASK       0xf
#define DATA_EQU_PREV         0
#define DATA_FROM_TABLE       4

int radeon_atom_init_mc_reg_table(struct rdn_card *card, uint8_t module_index,
				  struct atom_mc_reg_table *reg_table)
{
	int index = GetIndexIntoMasterTable(DATA, VRAM_Info);
	uint8_t frev = 0, crev = 0, num_entries, t_mem_id, num_ranges = 0;
	uint32_t i = 0, j;
	uint16_t size = 0;
	const uint8_t *vram_info;

	memset(reg_table, 0, sizeof(struct atom_mc_reg_table));

	vram_info = ci_data_table(card, index, &size, &frev, &crev);
	if (vram_info) {
		switch (frev) {
		case 1:
			rdn_log(card->os, RDN_LOG_ERROR, "old table version %d, %d", frev, crev);
			return -EINVAL;
		case 2:
			switch (crev) {
			case 1:
				if (module_index < u8_at(vram_info, offsetof(ATOM_VRAM_INFO_HEADER_V2_1, ucNumOfVRAMModule))) {
					const uint8_t *reg_block = vram_info +
						le16_at(vram_info, offsetof(ATOM_VRAM_INFO_HEADER_V2_1, usMemClkPatchTblOffset));
					uint16_t index_tbl_size = le16_at(reg_block, offsetof(ATOM_INIT_REG_BLOCK, usRegIndexTblSize));
					uint16_t data_blk_size = le16_at(reg_block, offsetof(ATOM_INIT_REG_BLOCK, usRegDataBlkSize));
					const uint8_t *reg_data = reg_block + (2 * sizeof(uint16_t)) + index_tbl_size;
					const uint8_t *format = reg_block + offsetof(ATOM_INIT_REG_BLOCK, asRegIndexBuf);

					num_entries = (uint8_t)(index_tbl_size / sizeof(ATOM_INIT_REG_INDEX_FORMAT)) - 1;
					if (num_entries > VBIOS_MC_REGISTER_ARRAY_SIZE)
						return -EINVAL;
					if (data_blk_size == 0)
						return -EINVAL;
					while (i < num_entries) {
						if (u8_at(format, offsetof(ATOM_INIT_REG_INDEX_FORMAT, ucPreRegDataLength)) & ACCESS_PLACEHOLDER)
							break;
						reg_table->mc_reg_address[i].s1 =
							le16_at(format, offsetof(ATOM_INIT_REG_INDEX_FORMAT, usRegIndex));
						reg_table->mc_reg_address[i].pre_reg_data =
							u8_at(format, offsetof(ATOM_INIT_REG_INDEX_FORMAT, ucPreRegDataLength));
						i++;
						format += sizeof(ATOM_INIT_REG_INDEX_FORMAT);
					}
					reg_table->last = i;
					while ((le32_at(reg_data, 0) != END_OF_REG_DATA_BLOCK) &&
					       (num_ranges < VBIOS_MAX_AC_TIMING_ENTRIES)) {
						t_mem_id = (uint8_t)((le32_at(reg_data, 0) & MEM_ID_MASK)
								     >> MEM_ID_SHIFT);
						if (module_index == t_mem_id) {
							reg_table->mc_reg_table_entry[num_ranges].mclk_max =
								(uint32_t)((le32_at(reg_data, 0) & CLOCK_RANGE_MASK)
									   >> CLOCK_RANGE_SHIFT);
							for (i = 0, j = 1; i < reg_table->last; i++) {
								if ((reg_table->mc_reg_address[i].pre_reg_data & LOW_NIBBLE_MASK) == DATA_FROM_TABLE) {
									reg_table->mc_reg_table_entry[num_ranges].mc_data[i] =
										le32_at(reg_data, 4 * j);
									j++;
								} else if ((reg_table->mc_reg_address[i].pre_reg_data & LOW_NIBBLE_MASK) == DATA_EQU_PREV) {
									reg_table->mc_reg_table_entry[num_ranges].mc_data[i] =
										reg_table->mc_reg_table_entry[num_ranges].mc_data[i - 1];
								}
							}
							num_ranges++;
						}
						reg_data += data_blk_size;
					}
					if (le32_at(reg_data, 0) != END_OF_REG_DATA_BLOCK)
						return -EINVAL;
					reg_table->num_entries = num_ranges;
				} else
					return -EINVAL;
				break;
			default:
				rdn_log(card->os, RDN_LOG_ERROR, "Unknown table version %d, %d", frev, crev);
				return -EINVAL;
			}
			break;
		default:
			rdn_log(card->os, RDN_LOG_ERROR, "Unknown table version %d, %d", frev, crev);
			return -EINVAL;
		}
		return 0;
	}
	return -EINVAL;
}

/* ---- PowerPlayInfo: r600_dpm.c ---- */

/* r600_get_platform_caps() */
int r600_get_platform_caps(struct rdn_ci_dpm *dpm)
{
	const uint8_t *power_info;
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	uint16_t size = 0;
	uint8_t frev = 0, crev = 0;

	power_info = ci_data_table(dpm->card, index, &size, &frev, &crev);
	if (!power_info)
		return -EINVAL;

	dpm->platform_caps = le32_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, ulPlatformCaps));
	dpm->backbias_response_time = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usBackbiasTime));
	dpm->voltage_response_time = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usVoltageTime));

	/*
	 * radeon_atombios_add_pplib_thermal_controller(): the thermal
	 * controller entry, which on Sea Islands is the internal one
	 * (ATOM_PP_THERMALCONTROLLER_CISLANDS) and names the fan.
	 */
	{
		const uint8_t *controller = power_info + offsetof(ATOM_PPLIB_POWERPLAYTABLE, sThermalController);
		uint8_t type = u8_at(controller, offsetof(ATOM_PPLIB_THERMALCONTROLLER, ucType));
		uint8_t fan_parameters = u8_at(controller, offsetof(ATOM_PPLIB_THERMALCONTROLLER, ucFanParameters));

		dpm->thermal_controller_type = type;
		if (type > 0) {
			if (fan_parameters & ATOM_PP_FANPARAMETERS_NOFAN)
				dpm->no_fan = true;
			dpm->fan_pulses_per_revolution =
				fan_parameters & ATOM_PP_FANPARAMETERS_TACHOMETER_PULSES_PER_REVOLUTION_MASK;
			if (dpm->fan_pulses_per_revolution) {
				dpm->fan_min_rpm = u8_at(controller, offsetof(ATOM_PPLIB_THERMALCONTROLLER, ucFanMinRPM));
				dpm->fan_max_rpm = u8_at(controller, offsetof(ATOM_PPLIB_THERMALCONTROLLER, ucFanMaxRPM));
			}
			if (type == ATOM_PP_THERMALCONTROLLER_CISLANDS)
				rdn_log(dpm->card->os, RDN_LOG_INFO, "Internal thermal controller %s fan control",
					(fan_parameters & ATOM_PP_FANPARAMETERS_NOFAN) ? "without" : "with");
			else
				rdn_log(dpm->card->os, RDN_LOG_INFO, "thermal controller type %u (not Sea Islands' internal one) %s fan control",
					type, (fan_parameters & ATOM_PP_FANPARAMETERS_NOFAN) ? "without" : "with");
		}
	}

	return 0;
}

/* sizeof(ATOM_PPLIB_EXTENDEDHEADER) */
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V2 12
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V3 14
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V4 16
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V5 18
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V6 20
#define SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V7 22

/* r600_parse_clk_voltage_dep_table(): ATOM_PPLIB_Clock_Voltage_Dependency_Table at atom_table */
static int r600_parse_clk_voltage_dep_table(struct rdn_ci_dpm *dpm, struct rdn_ci_dep_table *radeon_table,
					    const uint8_t *atom_table)
{
	int i;
	const uint8_t *entry;
	uint8_t n = u8_at(atom_table, offsetof(ATOM_PPLIB_Clock_Voltage_Dependency_Table, ucNumEntries));

	radeon_table->entries = ci_alloc(dpm, n ? n * sizeof(struct rdn_ci_dep_entry) : 1);
	if (!radeon_table->entries)
		return -ENOMEM;

	entry = atom_table + offsetof(ATOM_PPLIB_Clock_Voltage_Dependency_Table, entries);
	for (i = 0; i < n; i++) {
		radeon_table->entries[i].clk = le16_at(entry, offsetof(ATOM_PPLIB_Clock_Voltage_Dependency_Record, usClockLow)) |
			((uint32_t)u8_at(entry, offsetof(ATOM_PPLIB_Clock_Voltage_Dependency_Record, ucClockHigh)) << 16);
		radeon_table->entries[i].v = le16_at(entry, offsetof(ATOM_PPLIB_Clock_Voltage_Dependency_Record, usVoltage));
		entry += sizeof(ATOM_PPLIB_Clock_Voltage_Dependency_Record);
	}
	radeon_table->count = n;

	return 0;
}

void r600_free_extended_power_table(struct rdn_ci_dpm *dpm)
{
	struct rdn_ci_dyn_state *dyn_state = &dpm->dyn_state;

	ci_free(dpm, dyn_state->vddc_dependency_on_sclk.entries);
	ci_free(dpm, dyn_state->vddci_dependency_on_mclk.entries);
	ci_free(dpm, dyn_state->vddc_dependency_on_mclk.entries);
	ci_free(dpm, dyn_state->mvdd_dependency_on_mclk.entries);
	ci_free(dpm, dyn_state->cac_leakage_table.entries);
	ci_free(dpm, dyn_state->phase_shedding_limits_table.entries);
	ci_free(dpm, dyn_state->vce_clock_voltage_dependency_table.entries);
	ci_free(dpm, dyn_state->uvd_clock_voltage_dependency_table.entries);
	ci_free(dpm, dyn_state->samu_clock_voltage_dependency_table.entries);
	ci_free(dpm, dyn_state->acp_clock_voltage_dependency_table.entries);
	dyn_state->vddc_dependency_on_sclk.entries = NULL;
	dyn_state->vddci_dependency_on_mclk.entries = NULL;
	dyn_state->vddc_dependency_on_mclk.entries = NULL;
	dyn_state->mvdd_dependency_on_mclk.entries = NULL;
	dyn_state->cac_leakage_table.entries = NULL;
	dyn_state->phase_shedding_limits_table.entries = NULL;
	dyn_state->vce_clock_voltage_dependency_table.entries = NULL;
	dyn_state->uvd_clock_voltage_dependency_table.entries = NULL;
	dyn_state->samu_clock_voltage_dependency_table.entries = NULL;
	dyn_state->acp_clock_voltage_dependency_table.entries = NULL;
	dyn_state->has_ppm_table = false;
	dyn_state->has_cac_tdp_table = false;
}

int r600_parse_extended_power_table(struct rdn_ci_dpm *dpm)
{
	const uint8_t *power_info;
	const uint8_t *fan_info;
	const uint8_t *dep_table;
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	uint16_t size = 0, table_size;
	uint8_t frev = 0, crev = 0;
	int ret, i;
	struct rdn_ci_dyn_state *ds = &dpm->dyn_state;

	power_info = ci_data_table(dpm->card, index, &size, &frev, &crev);
	if (!power_info)
		return -EINVAL;
	table_size = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usTableSize));

	/* fan table */
	if (table_size >= sizeof(ATOM_PPLIB_POWERPLAYTABLE3)) {
		uint16_t fan_off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE3, usFanTableOffset));

		if (fan_off) {
			fan_info = power_info + fan_off;
			dpm->fan.table_format = u8_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, ucFanTableFormat));
			dpm->fan.t_hyst = u8_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, ucTHyst));
			dpm->fan.t_min = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usTMin));
			dpm->fan.t_med = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usTMed));
			dpm->fan.t_high = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usTHigh));
			dpm->fan.pwm_min = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usPWMMin));
			dpm->fan.pwm_med = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usPWMMed));
			dpm->fan.pwm_high = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE, usPWMHigh));
			if (dpm->fan.table_format >= 2)
				dpm->fan.t_max = le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE2, usTMax));
			else
				dpm->fan.t_max = 10900;
			dpm->fan.cycle_delay = 100000;
			if (dpm->fan.table_format >= 3) {
				dpm->fan.control_mode = u8_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE3, ucFanControlMode));
				dpm->fan.default_max_fan_pwm =
					le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE3, usFanPWMMax));
				dpm->fan.default_fan_output_sensitivity = 4836;
				dpm->fan.fan_output_sensitivity =
					le16_at(fan_info, offsetof(ATOM_PPLIB_FANTABLE3, usFanOutputSensitivity));
			}
			dpm->fan.ucode_fan_control = true;
		}
	}

	/* clock dependancy tables, shedding tables */
	if (table_size >= sizeof(ATOM_PPLIB_POWERPLAYTABLE4)) {
		uint16_t off;

		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usVddcDependencyOnSCLKOffset));
		if (off) {
			dep_table = power_info + off;
			ret = r600_parse_clk_voltage_dep_table(dpm, &ds->vddc_dependency_on_sclk, dep_table);
			if (ret)
				return ret;
		}
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usVddciDependencyOnMCLKOffset));
		if (off) {
			dep_table = power_info + off;
			ret = r600_parse_clk_voltage_dep_table(dpm, &ds->vddci_dependency_on_mclk, dep_table);
			if (ret) {
				r600_free_extended_power_table(dpm);
				return ret;
			}
		}
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usVddcDependencyOnMCLKOffset));
		if (off) {
			dep_table = power_info + off;
			ret = r600_parse_clk_voltage_dep_table(dpm, &ds->vddc_dependency_on_mclk, dep_table);
			if (ret) {
				r600_free_extended_power_table(dpm);
				return ret;
			}
		}
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usMvddDependencyOnMCLKOffset));
		if (off) {
			dep_table = power_info + off;
			ret = r600_parse_clk_voltage_dep_table(dpm, &ds->mvdd_dependency_on_mclk, dep_table);
			if (ret) {
				r600_free_extended_power_table(dpm);
				return ret;
			}
		}
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usMaxClockVoltageOnDCOffset));
		if (off) {
			const uint8_t *clk_v = power_info + off;

			if (u8_at(clk_v, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Table, ucNumEntries))) {
				const uint8_t *e = clk_v + offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Table, entries);

				ds->max_clock_voltage_on_dc.sclk =
					le16_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, usSclkLow)) |
					((uint32_t)u8_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, ucSclkHigh)) << 16);
				ds->max_clock_voltage_on_dc.mclk =
					le16_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, usMclkLow)) |
					((uint32_t)u8_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, ucMclkHigh)) << 16);
				ds->max_clock_voltage_on_dc.vddc =
					le16_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, usVddc));
				ds->max_clock_voltage_on_dc.vddci =
					le16_at(e, offsetof(ATOM_PPLIB_Clock_Voltage_Limit_Record, usVddci));
			}
		}
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE4, usVddcPhaseShedLimitsTableOffset));
		if (off) {
			const uint8_t *psl = power_info + off;
			const uint8_t *entry;
			uint8_t n = u8_at(psl, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Table, ucNumEntries));

			ds->phase_shedding_limits_table.entries =
				ci_alloc(dpm, n ? n * sizeof(struct rdn_ci_phase_shedding_limits_entry) : 1);
			if (!ds->phase_shedding_limits_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}

			entry = psl + offsetof(ATOM_PPLIB_PhaseSheddingLimits_Table, entries);
			for (i = 0; i < n; i++) {
				ds->phase_shedding_limits_table.entries[i].sclk =
					le16_at(entry, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Record, usSclkLow)) |
					((uint32_t)u8_at(entry, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Record, ucSclkHigh)) << 16);
				ds->phase_shedding_limits_table.entries[i].mclk =
					le16_at(entry, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Record, usMclkLow)) |
					((uint32_t)u8_at(entry, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Record, ucMclkHigh)) << 16);
				ds->phase_shedding_limits_table.entries[i].voltage =
					le16_at(entry, offsetof(ATOM_PPLIB_PhaseSheddingLimits_Record, usVoltage));
				entry += sizeof(ATOM_PPLIB_PhaseSheddingLimits_Record);
			}
			ds->phase_shedding_limits_table.count = n;
		}
	}

	/* cac data */
	if (table_size >= sizeof(ATOM_PPLIB_POWERPLAYTABLE5)) {
		uint16_t off;

		dpm->tdp_limit = le32_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, ulTDPLimit));
		dpm->near_tdp_limit = le32_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, ulNearTDPLimit));
		dpm->near_tdp_limit_adjusted = dpm->near_tdp_limit;
		dpm->tdp_od_limit = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, usTDPODLimit));
		if (dpm->tdp_od_limit)
			dpm->power_control = true;
		else
			dpm->power_control = false;
		dpm->tdp_adjustment = 0;
		dpm->sq_ramping_threshold = le32_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, ulSQRampingThreshold));
		dpm->cac_leakage = le32_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, ulCACLeakage));
		dpm->load_line_slope = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, usLoadLineSlope));
		off = le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE5, usCACLeakageTableOffset));
		if (off) {
			const uint8_t *cac_table = power_info + off;
			const uint8_t *entry;
			uint8_t n = u8_at(cac_table, offsetof(ATOM_PPLIB_CAC_Leakage_Table, ucNumEntries));

			ds->cac_leakage_table.entries = ci_alloc(dpm, n ? n * sizeof(struct rdn_ci_cac_leakage_entry) : 1);
			if (!ds->cac_leakage_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}
			entry = cac_table + offsetof(ATOM_PPLIB_CAC_Leakage_Table, entries);
			for (i = 0; i < n; i++) {
				struct rdn_ci_cac_leakage_entry *e = &ds->cac_leakage_table.entries[i];

				if (dpm->platform_caps & ATOM_PP_PLATFORM_CAP_EVV) {
					e->vddc1 = le16_at(entry, 0);	/* usVddc1 */
					e->vddc2 = le16_at(entry, 2);	/* usVddc2 */
					e->vddc3 = le16_at(entry, 4);	/* usVddc3 */
					/*
					 * The union's other view, as Linux's little-endian host reads
					 * it: radeon_cac_leakage_entry is NOT packed, so the u32
					 * leakage sits at offset 4 after two bytes of padding and
					 * aliases vddc3 (bytes 4-5) plus two bytes beyond the record;
					 * ci_get_std_voltage_value_sidd() keeps only the low 16 bits
					 * of leakage * 4. Measured 2026-11-04: Linux's SMU table has
					 * StdVoltageHiSidd = vddc3 * 4, this one had vddc2 * 4.
					 */
					e->vddc = e->vddc1;
					e->leakage = e->vddc3;
				} else {
					e->vddc = le16_at(entry, 0);	/* usVddc */
					e->leakage = le32_at(entry, 2);	/* ulLeakageValue */
					e->vddc1 = e->vddc;
					e->vddc2 = (uint16_t)e->leakage;
					e->vddc3 = (uint16_t)(e->leakage >> 16);
				}
				entry += sizeof(ATOM_PPLIB_CAC_Leakage_Record);
			}
			ds->cac_leakage_table.count = n;
		}
	}

	/* ext tables */
	if (table_size >= sizeof(ATOM_PPLIB_POWERPLAYTABLE3)) {
		const uint8_t *ext_hdr = power_info +
			le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE3, usExtendendedHeaderOffset));
		uint16_t ext_size = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usSize));
		uint16_t off;

		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usVCETableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V2) && off) {
			const uint8_t *array = power_info + off + 1;
			uint8_t array_n = u8_at(array, offsetof(VCEClockInfoArray, ucNumEntries));
			const uint8_t *limits = power_info + off + 1 +
				1 + array_n * sizeof(VCEClockInfo);
			uint8_t limits_n = u8_at(limits, offsetof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Table, numEntries));
			const uint8_t *states = power_info + off + 1 +
				1 + (array_n * sizeof(VCEClockInfo)) +
				1 + (limits_n * sizeof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Record));
			uint8_t states_n = u8_at(states, offsetof(ATOM_PPLIB_VCE_State_Table, numEntries));
			const uint8_t *entry;
			const uint8_t *state_entry;
			const uint8_t *vce_clk;

			ds->vce_clock_voltage_dependency_table.entries =
				ci_alloc(dpm, limits_n ? limits_n * sizeof(struct rdn_ci_vce_dep_entry) : 1);
			if (!ds->vce_clock_voltage_dependency_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}
			ds->vce_clock_voltage_dependency_table.count = limits_n;
			entry = limits + offsetof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Table, entries);
			state_entry = states + offsetof(ATOM_PPLIB_VCE_State_Table, entries);
			for (i = 0; i < limits_n; i++) {
				vce_clk = array + offsetof(VCEClockInfoArray, entries) +
					u8_at(entry, offsetof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Record, ucVCEClockInfoIndex)) * sizeof(VCEClockInfo);
				ds->vce_clock_voltage_dependency_table.entries[i].evclk =
					le16_at(vce_clk, offsetof(VCEClockInfo, usEVClkLow)) |
					((uint32_t)u8_at(vce_clk, offsetof(VCEClockInfo, ucEVClkHigh)) << 16);
				ds->vce_clock_voltage_dependency_table.entries[i].ecclk =
					le16_at(vce_clk, offsetof(VCEClockInfo, usECClkLow)) |
					((uint32_t)u8_at(vce_clk, offsetof(VCEClockInfo, ucECClkHigh)) << 16);
				ds->vce_clock_voltage_dependency_table.entries[i].v =
					le16_at(entry, offsetof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Record, usVoltage));
				entry += sizeof(ATOM_PPLIB_VCE_Clock_Voltage_Limit_Record);
			}
			for (i = 0; i < states_n; i++) {
				uint8_t clock_info_index;

				if (i >= RDN_CI_MAX_VCE_LEVELS)
					break;
				vce_clk = array + offsetof(VCEClockInfoArray, entries) +
					u8_at(state_entry, offsetof(ATOM_PPLIB_VCE_State_Record, ucVCEClockInfoIndex)) * sizeof(VCEClockInfo);
				clock_info_index = u8_at(state_entry, offsetof(ATOM_PPLIB_VCE_State_Record, ucClockInfoIndex));
				dpm->vce_states[i].evclk =
					le16_at(vce_clk, offsetof(VCEClockInfo, usEVClkLow)) |
					((uint32_t)u8_at(vce_clk, offsetof(VCEClockInfo, ucEVClkHigh)) << 16);
				dpm->vce_states[i].ecclk =
					le16_at(vce_clk, offsetof(VCEClockInfo, usECClkLow)) |
					((uint32_t)u8_at(vce_clk, offsetof(VCEClockInfo, ucECClkHigh)) << 16);
				dpm->vce_states[i].clk_idx = clock_info_index & 0x3f;
				dpm->vce_states[i].pstate = (clock_info_index & 0xc0) >> 6;
				state_entry += sizeof(ATOM_PPLIB_VCE_State_Record);
			}
		}
		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usUVDTableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V3) && off) {
			const uint8_t *array = power_info + off + 1;
			uint8_t array_n = u8_at(array, offsetof(UVDClockInfoArray, ucNumEntries));
			const uint8_t *limits = power_info + off + 1 +
				1 + (array_n * sizeof(UVDClockInfo));
			uint8_t limits_n = u8_at(limits, offsetof(ATOM_PPLIB_UVD_Clock_Voltage_Limit_Table, numEntries));
			const uint8_t *entry;

			ds->uvd_clock_voltage_dependency_table.entries =
				ci_alloc(dpm, limits_n ? limits_n * sizeof(struct rdn_ci_uvd_dep_entry) : 1);
			if (!ds->uvd_clock_voltage_dependency_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}
			ds->uvd_clock_voltage_dependency_table.count = limits_n;
			entry = limits + offsetof(ATOM_PPLIB_UVD_Clock_Voltage_Limit_Table, entries);
			for (i = 0; i < limits_n; i++) {
				const uint8_t *uvd_clk = array + offsetof(UVDClockInfoArray, entries) +
					u8_at(entry, offsetof(ATOM_PPLIB_UVD_Clock_Voltage_Limit_Record, ucUVDClockInfoIndex)) * sizeof(UVDClockInfo);

				ds->uvd_clock_voltage_dependency_table.entries[i].vclk =
					le16_at(uvd_clk, offsetof(UVDClockInfo, usVClkLow)) |
					((uint32_t)u8_at(uvd_clk, offsetof(UVDClockInfo, ucVClkHigh)) << 16);
				ds->uvd_clock_voltage_dependency_table.entries[i].dclk =
					le16_at(uvd_clk, offsetof(UVDClockInfo, usDClkLow)) |
					((uint32_t)u8_at(uvd_clk, offsetof(UVDClockInfo, ucDClkHigh)) << 16);
				ds->uvd_clock_voltage_dependency_table.entries[i].v =
					le16_at(entry, offsetof(ATOM_PPLIB_UVD_Clock_Voltage_Limit_Record, usVoltage));
				entry += sizeof(ATOM_PPLIB_UVD_Clock_Voltage_Limit_Record);
			}
		}
		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usSAMUTableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V4) && off) {
			const uint8_t *limits = power_info + off + 1;
			uint8_t limits_n = u8_at(limits, offsetof(ATOM_PPLIB_SAMClk_Voltage_Limit_Table, numEntries));
			const uint8_t *entry;

			ds->samu_clock_voltage_dependency_table.entries =
				ci_alloc(dpm, limits_n ? limits_n * sizeof(struct rdn_ci_dep_entry) : 1);
			if (!ds->samu_clock_voltage_dependency_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}
			ds->samu_clock_voltage_dependency_table.count = limits_n;
			entry = limits + offsetof(ATOM_PPLIB_SAMClk_Voltage_Limit_Table, entries);
			for (i = 0; i < limits_n; i++) {
				ds->samu_clock_voltage_dependency_table.entries[i].clk =
					le16_at(entry, offsetof(ATOM_PPLIB_SAMClk_Voltage_Limit_Record, usSAMClockLow)) |
					((uint32_t)u8_at(entry, offsetof(ATOM_PPLIB_SAMClk_Voltage_Limit_Record, ucSAMClockHigh)) << 16);
				ds->samu_clock_voltage_dependency_table.entries[i].v =
					le16_at(entry, offsetof(ATOM_PPLIB_SAMClk_Voltage_Limit_Record, usVoltage));
				entry += sizeof(ATOM_PPLIB_SAMClk_Voltage_Limit_Record);
			}
		}
		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usPPMTableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V5) && off) {
			const uint8_t *ppm = power_info + off;

			ds->has_ppm_table = true;
			ds->ppm_table.ppm_design = u8_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ucPpmDesign));
			ds->ppm_table.cpu_core_number = le16_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, usCpuCoreNumber));
			ds->ppm_table.platform_tdp = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulPlatformTDP));
			ds->ppm_table.small_ac_platform_tdp = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulSmallACPlatformTDP));
			ds->ppm_table.platform_tdc = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulPlatformTDC));
			ds->ppm_table.small_ac_platform_tdc = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulSmallACPlatformTDC));
			ds->ppm_table.apu_tdp = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulApuTDP));
			ds->ppm_table.dgpu_tdp = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulDGpuTDP));
			ds->ppm_table.dgpu_ulv_power = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulDGpuUlvPower));
			ds->ppm_table.tj_max = le32_at(ppm, offsetof(ATOM_PPLIB_PPM_Table, ulTjmax));
		}
		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usACPTableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V6) && off) {
			const uint8_t *limits = power_info + off + 1;
			uint8_t limits_n = u8_at(limits, offsetof(ATOM_PPLIB_ACPClk_Voltage_Limit_Table, numEntries));
			const uint8_t *entry;

			ds->acp_clock_voltage_dependency_table.entries =
				ci_alloc(dpm, limits_n ? limits_n * sizeof(struct rdn_ci_dep_entry) : 1);
			if (!ds->acp_clock_voltage_dependency_table.entries) {
				r600_free_extended_power_table(dpm);
				return -ENOMEM;
			}
			ds->acp_clock_voltage_dependency_table.count = limits_n;
			entry = limits + offsetof(ATOM_PPLIB_ACPClk_Voltage_Limit_Table, entries);
			for (i = 0; i < limits_n; i++) {
				ds->acp_clock_voltage_dependency_table.entries[i].clk =
					le16_at(entry, offsetof(ATOM_PPLIB_ACPClk_Voltage_Limit_Record, usACPClockLow)) |
					((uint32_t)u8_at(entry, offsetof(ATOM_PPLIB_ACPClk_Voltage_Limit_Record, ucACPClockHigh)) << 16);
				ds->acp_clock_voltage_dependency_table.entries[i].v =
					le16_at(entry, offsetof(ATOM_PPLIB_ACPClk_Voltage_Limit_Record, usVoltage));
				entry += sizeof(ATOM_PPLIB_ACPClk_Voltage_Limit_Record);
			}
		}
		off = le16_at(ext_hdr, offsetof(ATOM_PPLIB_EXTENDEDHEADER, usPowerTuneTableOffset));
		if ((ext_size >= SIZE_OF_ATOM_PPLIB_EXTENDEDHEADER_V7) && off) {
			uint8_t rev = u8_at(power_info, off);
			const uint8_t *pt;

			ds->has_cac_tdp_table = true;
			if (rev > 0) {
				const uint8_t *ppt = power_info + off;

				ds->cac_tdp_table.maximum_power_delivery_limit =
					le16_at(ppt, offsetof(ATOM_PPLIB_POWERTUNE_Table_V1, usMaximumPowerDeliveryLimit));
				pt = ppt + offsetof(ATOM_PPLIB_POWERTUNE_Table_V1, power_tune_table);
			} else {
				const uint8_t *ppt = power_info + off;

				ds->cac_tdp_table.maximum_power_delivery_limit = 255;
				pt = ppt + offsetof(ATOM_PPLIB_POWERTUNE_Table, power_tune_table);
			}
			ds->cac_tdp_table.tdp = le16_at(pt, offsetof(ATOM_PowerTune_Table, usTDP));
			ds->cac_tdp_table.configurable_tdp = le16_at(pt, offsetof(ATOM_PowerTune_Table, usConfigurableTDP));
			ds->cac_tdp_table.tdc = le16_at(pt, offsetof(ATOM_PowerTune_Table, usTDC));
			ds->cac_tdp_table.battery_power_limit = le16_at(pt, offsetof(ATOM_PowerTune_Table, usBatteryPowerLimit));
			ds->cac_tdp_table.small_power_limit = le16_at(pt, offsetof(ATOM_PowerTune_Table, usSmallPowerLimit));
			ds->cac_tdp_table.low_cac_leakage = le16_at(pt, offsetof(ATOM_PowerTune_Table, usLowCACLeakage));
			ds->cac_tdp_table.high_cac_leakage = le16_at(pt, offsetof(ATOM_PowerTune_Table, usHighCACLeakage));
		}
	}

	return 0;
}

/* ---- PowerPlayInfo: ci_dpm.c's power states ---- */

/* ci_parse_pplib_non_clock_info() */
static void ci_parse_pplib_non_clock_info(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *rps, int rps_index,
					  const uint8_t *non_clock_info, uint8_t table_rev)
{
	rps->caps = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulCapsAndSettings));
	rps->classification = le16_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, usClassification));
	rps->classification2 = le16_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, usClassification2));

	if (ATOM_PPLIB_NONCLOCKINFO_VER1 < table_rev) {
		rps->vclk = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulVCLK));
		rps->dclk = le32_at(non_clock_info, offsetof(ATOM_PPLIB_NONCLOCK_INFO, ulDCLK));
	} else {
		rps->vclk = 0;
		rps->dclk = 0;
	}

	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_BOOT)
		dpm->boot_ps = rps_index;
	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_UVDSTATE)
		dpm->uvd_ps = rps_index;
}

/* ci_parse_pplib_clock_info(): one ATOM_PPLIB_CI_CLOCK_INFO into a performance level */
static void ci_parse_pplib_clock_info(struct rdn_ci_dpm *dpm, struct rdn_ci_rps *rps, int index,
				      const uint8_t *clock_info)
{
	struct ci_power_info *pi = &dpm->pi;
	struct ci_ps *ps = &rps->ps;
	struct ci_pl *pl = &ps->performance_levels[index];

	ps->performance_level_count = index + 1;

	pl->sclk = le16_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, usEngineClockLow));
	pl->sclk |= (uint32_t)u8_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, ucEngineClockHigh)) << 16;
	pl->mclk = le16_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, usMemoryClockLow));
	pl->mclk |= (uint32_t)u8_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, ucMemoryClockHigh)) << 16;

	pl->pcie_gen = r600_get_pcie_gen_support(pi->sys_pcie_mask,
						 (enum rdn_ci_pcie_gen)pi->vbios_boot_state.pcie_gen_bootup_value,
						 (enum rdn_ci_pcie_gen)u8_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, ucPCIEGen)));
	pl->pcie_lane = r600_get_pcie_lane_support(pi->vbios_boot_state.pcie_lane_bootup_value,
						   le16_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, usPCIELane)));

	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_ACPI) {
		pi->acpi_pcie_gen = pl->pcie_gen;
	}

	if (rps->classification2 & ATOM_PPLIB_CLASSIFICATION2_ULV) {
		pi->ulv.supported = true;
		pi->ulv.pl = *pl;
		pi->ulv.cg_ulv_parameter = CISLANDS_CGULVPARAMETER_DFLT;
	}

	/* patch up boot state */
	if (rps->classification & ATOM_PPLIB_CLASSIFICATION_BOOT) {
		pl->mclk = pi->vbios_boot_state.mclk_bootup_value;
		pl->sclk = pi->vbios_boot_state.sclk_bootup_value;
		pl->pcie_gen = (enum rdn_ci_pcie_gen)pi->vbios_boot_state.pcie_gen_bootup_value;
		pl->pcie_lane = pi->vbios_boot_state.pcie_lane_bootup_value;
	}

	switch (rps->classification & ATOM_PPLIB_CLASSIFICATION_UI_MASK) {
	case ATOM_PPLIB_CLASSIFICATION_UI_BATTERY:
		pi->use_pcie_powersaving_levels = true;
		if (pi->pcie_gen_powersaving.max < pl->pcie_gen)
			pi->pcie_gen_powersaving.max = pl->pcie_gen;
		if (pi->pcie_gen_powersaving.min > pl->pcie_gen)
			pi->pcie_gen_powersaving.min = pl->pcie_gen;
		if (pi->pcie_lane_powersaving.max < pl->pcie_lane)
			pi->pcie_lane_powersaving.max = pl->pcie_lane;
		if (pi->pcie_lane_powersaving.min > pl->pcie_lane)
			pi->pcie_lane_powersaving.min = pl->pcie_lane;
		break;
	case ATOM_PPLIB_CLASSIFICATION_UI_PERFORMANCE:
		pi->use_pcie_performance_levels = true;
		if (pi->pcie_gen_performance.max < pl->pcie_gen)
			pi->pcie_gen_performance.max = pl->pcie_gen;
		if (pi->pcie_gen_performance.min > pl->pcie_gen)
			pi->pcie_gen_performance.min = pl->pcie_gen;
		if (pi->pcie_lane_performance.max < pl->pcie_lane)
			pi->pcie_lane_performance.max = pl->pcie_lane;
		if (pi->pcie_lane_performance.min > pl->pcie_lane)
			pi->pcie_lane_performance.min = pl->pcie_lane;
		break;
	default:
		break;
	}
}

void ci_free_power_table(struct rdn_ci_dpm *dpm)
{
	ci_free(dpm, dpm->ps);
	dpm->ps = NULL;
	dpm->num_ps = 0;
}

/*
 * ci_parse_power_table(): the StateArray / ClockInfoArray /
 * NonClockInfoArray layout of PowerPlay revision 6 and later (the only one
 * Sea Islands has; Linux's ci_parse_power_table assumes it too).
 */
int ci_parse_power_table(struct rdn_ci_dpm *dpm)
{
	const uint8_t *power_info;
	const uint8_t *non_clock_info;
	const uint8_t *power_state;
	int i, j, k, non_clock_array_index, clock_array_index;
	const uint8_t *clock_info;
	const uint8_t *state_array;
	const uint8_t *clock_info_array;
	const uint8_t *non_clock_info_array;
	int index = GetIndexIntoMasterTable(DATA, PowerPlayInfo);
	uint16_t size = 0;
	uint8_t frev = 0, crev = 0;
	const uint8_t *power_state_offset;
	uint8_t num_states, clock_info_entries, clock_info_entry_size, non_clock_entry_size;

	power_info = ci_data_table(dpm->card, index, &size, &frev, &crev);
	if (!power_info)
		return -EINVAL;

	state_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usStateArrayOffset));
	clock_info_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usClockInfoArrayOffset));
	non_clock_info_array = power_info + le16_at(power_info, offsetof(ATOM_PPLIB_POWERPLAYTABLE, usNonClockInfoArrayOffset));
	num_states = u8_at(state_array, offsetof(StateArray, ucNumEntries));
	clock_info_entries = u8_at(clock_info_array, offsetof(ClockInfoArray, ucNumEntries));
	clock_info_entry_size = u8_at(clock_info_array, offsetof(ClockInfoArray, ucEntrySize));
	non_clock_entry_size = u8_at(non_clock_info_array, offsetof(NonClockInfoArray, ucEntrySize));

	/*
	 * The format revision is the table's layout (6 is the PowerPlay 6
	 * walk, frev of the header); Linux checks nothing here. The W8100's
	 * table is 6.1 with 9-byte entries (measured 2026-11-03).
	 */
	if (frev < 6 || clock_info_entry_size < sizeof(ATOM_PPLIB_CI_CLOCK_INFO)) {
		rdn_log(dpm->card->os, RDN_LOG_ERROR, "ci_dpm: PowerPlay table %u.%u with %u-byte clock entries is not a Sea Islands one",
			frev, crev, clock_info_entry_size);
		return -EINVAL;
	}

	dpm->ps = ci_alloc(dpm, num_states ? num_states * sizeof(struct rdn_ci_rps) : 1);
	if (!dpm->ps)
		return -ENOMEM;
	dpm->boot_ps = -1;
	dpm->uvd_ps = -1;
	power_state_offset = state_array + offsetof(StateArray, states);
	dpm->num_ps = 0;
	for (i = 0; i < num_states; i++) {
		const uint8_t *idx;
		uint8_t num_levels;

		power_state = power_state_offset;
		num_levels = u8_at(power_state, offsetof(ATOM_PPLIB_STATE_V2, ucNumDPMLevels));
		non_clock_array_index = u8_at(power_state, offsetof(ATOM_PPLIB_STATE_V2, nonClockInfoIndex));
		non_clock_info = non_clock_info_array + offsetof(NonClockInfoArray, nonClockInfo) +
			non_clock_array_index * non_clock_entry_size;
		/*
		 * Linux checks rdev->pm.power_state[i].clock_info here, the
		 * older pm code's parse of the same table; there is no such
		 * parse here.
		 */
		ci_parse_pplib_non_clock_info(dpm, &dpm->ps[i], i, non_clock_info, non_clock_entry_size);
		k = 0;
		idx = power_state + offsetof(ATOM_PPLIB_STATE_V2, clockInfoIndex);
		for (j = 0; j < num_levels; j++) {
			clock_array_index = u8_at(idx, j);
			if (clock_array_index >= clock_info_entries)
				continue;
			if (k >= CISLANDS_MAX_HARDWARE_POWERLEVELS)
				break;
			clock_info = clock_info_array + offsetof(ClockInfoArray, clockInfo) +
				(clock_array_index * clock_info_entry_size);
			ci_parse_pplib_clock_info(dpm, &dpm->ps[i], k, clock_info);
			k++;
		}
		power_state_offset += 2 + num_levels;
		dpm->num_ps = i + 1;
	}

	/* fill in the vce power states */
	for (i = 0; i < RDN_CI_MAX_VCE_LEVELS; i++) {
		uint32_t sclk, mclk;

		clock_array_index = dpm->vce_states[i].clk_idx;
		if (clock_array_index >= clock_info_entries)
			continue;	/* Linux reads past the array for an unset state; the VCE path is a stub here anyway */
		clock_info = clock_info_array + offsetof(ClockInfoArray, clockInfo) +
			clock_array_index * clock_info_entry_size;
		sclk = le16_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, usEngineClockLow));
		sclk |= (uint32_t)u8_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, ucEngineClockHigh)) << 16;
		mclk = le16_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, usMemoryClockLow));
		mclk |= (uint32_t)u8_at(clock_info, offsetof(ATOM_PPLIB_CI_CLOCK_INFO, ucMemoryClockHigh)) << 16;
		dpm->vce_states[i].sclk = sclk;
		dpm->vce_states[i].mclk = mclk;
	}

	return 0;
}
