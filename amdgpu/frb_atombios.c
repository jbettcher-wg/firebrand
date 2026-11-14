/*
 * amdgpu_atombios.c, the parts Firebrand has: the interpreter's register
 * callbacks, amdgpu_atombios_init, the BIOS scratch registers, ASIC_Init,
 * the FirmwareInfo clocks; plus the survey (VBIOS identity and the command
 * table revisions the display code will meet), which has no Linux
 * counterpart and is RadeonNI's cmd_table_survey brought to DCE 11.2.
 *
 * VBIOS tables are read with frb_get_le16/32 at offsetof() positions; no
 * atombios.h structure is ever dereferenced as a whole on this big-endian
 * host (its bitfields and multi-byte fields are little-endian).
 *
 * Copyright 2007-8 Advanced Micro Devices, Inc.
 * Copyright 2008 Red Hat Inc.
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
 *
 * Authors: Dave Airlie
 *          Alex Deucher
 */
#include <stddef.h>
#include "frb_device.h"
#include "../shared/atom/atombios.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/oss/oss_3_0_d.h"

/* atomfirmware.h: where the VBIOS keeps its strings (legacy ATOM too) */
#define OFFSET_TO_ATOM_ROM_HEADER_POINTER	0x48
#define OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS 0x2f
#define OFFSET_TO_GET_ATOMBIOS_STRING_START	0x6e
#define OFFSET_TO_VBIOS_PART_NUMBER		0x80
#define OFFSET_TO_VBIOS_DATE			0x50
#define BIOS_VERSION_PREFIX			"ATOMBIOSBK-AMD"

/* --- the interpreter's register callbacks (cail_*) ------------------------- */

/*
 * The interpreter's last register reads, for the loop-abort path: a table
 * that spins for 20 s is polling one of these, and the value it kept seeing
 * is the diagnosis (RadeonNI's rdn_atom.c, W8100, 2026-11-03).
 */
#define FRB_ATOM_TRACE 32
static struct { uint32_t reg, val, count; } recent[FRB_ATOM_TRACE];
static unsigned recent_next;

static void recent_read(uint32_t reg, uint32_t val)
{
	unsigned last = (recent_next + FRB_ATOM_TRACE - 1) % FRB_ATOM_TRACE;

	if (recent[last].count && recent[last].reg == reg && recent[last].val == val) {
		recent[last].count++;
		return;
	}
	recent[recent_next].reg = reg;
	recent[recent_next].val = val;
	recent[recent_next].count = 1;
	recent_next = (recent_next + 1) % FRB_ATOM_TRACE;
}

void frb_atom_dump_recent_reads(struct frb_os *os)
{
	unsigned i, n;

	for (n = 0; n < FRB_ATOM_TRACE; n++) {
		i = (recent_next + n) % FRB_ATOM_TRACE;
		if (!recent[i].count)
			continue;
		frb_log(os, FRB_LOG_ERROR, "atom: recent read: reg 0x%05x = 0x%08x (x%u)",
			recent[i].reg, recent[i].val, recent[i].count);
	}
}

/* amdgpu: no separately addressed PLL or MC space on VI; reads are 0 */
static uint32_t cail_pll_read(struct card_info *info, uint32_t reg)
{
	frb_log(info->os, FRB_LOG_ERROR, "atom: PLL read of 0x%x (amdgpu returns 0)", reg);
	return 0;
}

static void cail_pll_write(struct card_info *info, uint32_t reg, uint32_t val)
{
	frb_log(info->os, FRB_LOG_ERROR, "atom: PLL write 0x%x = 0x%x dropped (as amdgpu)", reg, val);
}

static uint32_t cail_mc_read(struct card_info *info, uint32_t reg)
{
	frb_log(info->os, FRB_LOG_ERROR, "atom: MC read of 0x%x (amdgpu returns 0)", reg);
	return 0;
}

static void cail_mc_write(struct card_info *info, uint32_t reg, uint32_t val)
{
	frb_log(info->os, FRB_LOG_ERROR, "atom: MC write 0x%x = 0x%x dropped (as amdgpu)", reg, val);
}

static void cail_reg_write(struct card_info *info, uint32_t reg, uint32_t val)
{
	struct frb_device *adev = (struct frb_device *)info->priv;

	WREG32(reg, val);
}

static uint32_t cail_reg_read(struct card_info *info, uint32_t reg)
{
	struct frb_device *adev = (struct frb_device *)info->priv;
	uint32_t r;

	r = RREG32(reg);
	recent_read(reg, r);
	return r;
}

/* --- amdgpu_atombios_init -------------------------------------------------- */

/*
 * amdgpu_atombios_allocate_fb_scratch. Upstream reads ulStartAddrUsedByFirmware
 * without le32_to_cpu (only its SR-IOV flag bits are tested); read LE here.
 */
int frb_atombios_allocate_fb_scratch(struct frb_device *adev)
{
	struct atom_context *ctx = adev->atom;
	int index = GetIndexIntoMasterTable(DATA, VRAM_UsageByFirmware);
	uint16_t data_offset;
	int usage_bytes = 0;
	uint32_t start_addr, size;

	if (frb_atom_parse_data_header(ctx, index, NULL, NULL, NULL, &data_offset)) {
		uint32_t rec = data_offset + offsetof(ATOM_VRAM_USAGE_BY_FIRMWARE, asFirmwareVramReserveInfo);

		start_addr = frb_get_le32(ctx->bios, rec + offsetof(ATOM_FIRMWARE_VRAM_RESERVE_INFO, ulStartAddrUsedByFirmware));
		size = frb_get_le16(ctx->bios, rec + offsetof(ATOM_FIRMWARE_VRAM_RESERVE_INFO, usFirmwareUseInKb));
		frb_dbg(adev, "atom firmware requested %08x %ukb", start_addr, size);
		if ((start_addr & ATOM_VRAM_OPERATION_FLAGS_MASK) ==
		    (uint32_t)(ATOM_VRAM_BLOCK_SRIOV_MSG_SHARE_RESERVATION << ATOM_VRAM_OPERATION_FLAGS_SHIFT))
			usage_bytes = 0;	/* SR-IOV reservation: default scratch */
		else
			usage_bytes = size * 1024;
	}
	ctx->scratch_size_bytes = 0;
	if (usage_bytes == 0)
		usage_bytes = 20 * 1024;
	ctx->scratch = adev->os->alloc(adev->os->cookie, usage_bytes);
	if (!ctx->scratch)
		return -ENOMEM;
	ctx->scratch_size_bytes = usage_bytes;
	return 0;
}

/*
 * amdgpu_atombios_init without its scratch-register writes: those are the
 * first register writes of the bring-up and belong to the POST stage
 * (frb_atombios_scratch_regs_init), so the survey writes nothing.
 */
int frb_atombios_init(struct frb_device *adev)
{
	struct card_info *atom_card_info = &adev->atom_card;

	memset(atom_card_info, 0, sizeof(*atom_card_info));
	atom_card_info->os = adev->os;
	atom_card_info->priv = adev;
	atom_card_info->reg_read = cail_reg_read;
	atom_card_info->reg_write = cail_reg_write;
	atom_card_info->mc_read = cail_mc_read;
	atom_card_info->mc_write = cail_mc_write;
	atom_card_info->pll_read = cail_pll_read;
	atom_card_info->pll_write = cail_pll_write;

	adev->atom = frb_atom_parse(atom_card_info, adev->bios);
	if (!adev->atom)
		return -ENOMEM;
	adev->bios_scratch_reg_offset = mmBIOS_SCRATCH_0;
	return frb_atombios_allocate_fb_scratch(adev);
}

void frb_atombios_fini(struct frb_device *adev)
{
	if (adev->atom)
		frb_atom_destroy(adev->atom);
	adev->atom = 0;
}

/* amdgpu_atombios_scratch_regs_init */
void frb_atombios_scratch_regs_init(struct frb_device *adev)
{
	uint32_t bios_2_scratch, bios_6_scratch;

	adev->bios_scratch_reg_offset = mmBIOS_SCRATCH_0;

	bios_2_scratch = RREG32(adev->bios_scratch_reg_offset + 2);
	bios_6_scratch = RREG32(adev->bios_scratch_reg_offset + 6);

	/* let the bios control the backlight */
	bios_2_scratch &= ~ATOM_S2_VRI_BRIGHT_ENABLE;

	/* tell the bios not to handle mode switching */
	bios_6_scratch |= ATOM_S6_ACC_BLOCK_DISPLAY_SWITCH;

	/* clear the vbios dpms state */
	bios_2_scratch &= ~ATOM_S2_DEVICE_DPMS_STATE;

	WREG32(adev->bios_scratch_reg_offset + 2, bios_2_scratch);
	WREG32(adev->bios_scratch_reg_offset + 6, bios_6_scratch);
}

/* amdgpu_atombios_scratch_regs_engine_hung: around a reset, for the VBIOS */
void frb_atombios_scratch_regs_engine_hung(struct frb_device *adev, int hung)
{
	uint32_t tmp = RREG32(adev->bios_scratch_reg_offset + 3);

	if (hung)
		tmp |= ATOM_S3_ASIC_GUI_ENGINE_HUNG;
	else
		tmp &= ~ATOM_S3_ASIC_GUI_ENGINE_HUNG;

	WREG32(adev->bios_scratch_reg_offset + 3, tmp);
}

/* amdgpu_atombios_scratch_need_asic_init */
int frb_atombios_scratch_need_asic_init(struct frb_device *adev)
{
	uint32_t tmp = RREG32(adev->bios_scratch_reg_offset + 7);

	return !(tmp & ATOM_S7_ASIC_INIT_COMPLETE_MASK);
}

/* amdgpu_device_asic_init, the legacy-ATOM branch: amdgpu_atom_asic_init */
int frb_device_asic_init(struct frb_device *adev)
{
	return frb_atom_asic_init(adev->atom);
}

/* amdgpu_atombios_get_clock_info, the fields the bring-up uses so far */
int frb_atombios_get_clock_info(struct frb_device *adev)
{
	struct atom_context *ctx = adev->atom;
	int index = GetIndexIntoMasterTable(DATA, FirmwareInfo);
	uint8_t frev, crev;
	uint16_t off;

	if (!frb_atom_parse_data_header(ctx, index, NULL, &frev, &crev, &off))
		return -EINVAL;
	adev->default_sclk = frb_get_le32(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultEngineClock));
	adev->default_mclk = frb_get_le32(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultMemoryClock));
	/* adev->clock.spll.reference_freq (info_21.usCoreReferenceClock) */
	adev->spll_reference_freq = frb_get_le16(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usCoreReferenceClock));
	frb_info(adev, "FirmwareInfo %u.%u: boot engine clock %u kHz, memory clock %u kHz, "
		 "display engine %u kHz, DP external clock %u kHz, core/memory reference %u/%u kHz",
		 frev, crev, adev->default_sclk * 10, adev->default_mclk * 10,
		 frb_get_le32(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultDispEngineClkFreq)) * 10,
		 frb_get_le16(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usUniphyDPModeExtClkFreq)) * 10,
		 frb_get_le16(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usCoreReferenceClock)) * 10,
		 frb_get_le16(ctx->bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usMemoryReferenceClock)) * 10);
	return 0;
}

/*
 * amdgpu_atombios_get_clock_dividers, the arm VI takes (ComputeMemoryEnginePLL
 * crev 6, "CI"). The parameter block is laid out by byte offset, as the
 * table reads it (little-endian):
 *   in  (COMPUTE_GPU_CLOCK_INPUT_PARAMETERS_V1_6):  0 ulClockFreq:24 | ulComputeClockFlag:8;
 *   out (COMPUTE_GPU_CLOCK_OUTPUT_PARAMETERS_V1_6): 0 ulClock:24 | ucPostDiv:8 (the DFS divider),
 *       4 usFbDivFrac, 6 usFbDiv, 8 ucPllRefDiv, 9 ucPllPostDiv, 10 ucPllCntlFlag.
 * atombios.h describes both dwords as bitfields, which a big-endian
 * compiler lays out the other way round; the offsets are what the VBIOS
 * byte code uses.
 */
int frb_atombios_get_clock_dividers(struct frb_device *adev, uint8_t clock_type, uint32_t clock,
				    int strobe_mode, struct frb_atom_clock_dividers *dividers)
{
	int index = GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL);
	uint32_t ps[8];
	uint8_t frev, crev;

	(void)strobe_mode;
	memset(ps, 0, sizeof(ps));
	memset(dividers, 0, sizeof(*dividers));

	if (!frb_atom_parse_cmd_header(adev->atom, index, &frev, &crev))
		return -EINVAL;

	switch (crev) {
	case 6:
		/* CI */
		/* COMPUTE_GPUCLK_INPUT_FLAG_DEFAULT_GPUCLK, COMPUTE_GPUCLK_INPUT_FLAG_SCLK */
		frb_put_le32(ps, 0, (clock & 0xffffff) | ((uint32_t)clock_type << 24));	/* 10 khz */

		if (frb_atom_execute_table(adev->atom, index, ps, sizeof(ps)))
			return -EINVAL;

		dividers->whole_fb_div = frb_get_le16(ps, 6);
		dividers->frac_fb_div = frb_get_le16(ps, 4);
		dividers->ref_div = ((const uint8_t *)ps)[8];
		dividers->post_div = ((const uint8_t *)ps)[9];
		dividers->flags = ((const uint8_t *)ps)[10];
		dividers->real_clock = frb_get_le32(ps, 0) & 0xffffff;
		dividers->post_divider = ((const uint8_t *)ps)[3];
		break;
	default:
		/* r6xx..SI (crev 2, 3, 5) and fusion (4) are not Polaris */
		frb_err(adev, "ComputeMemoryEnginePLL revision %u.%u: only 1.6 (CI/VI) is ported", frev, crev);
		return -EINVAL;
	}
	return 0;
}

/* --- the survey ------------------------------------------------------------ */

static int rom_find(const uint8_t *rom, uint32_t size, const char *s, uint32_t start, uint32_t end)
{
	uint32_t n = (uint32_t)strlen(s), i;

	if (end > size)
		end = size;
	for (i = start; i + n <= end; i++)
		if (!memcmp(rom + i, s, n))
			return (int)i;
	return -1;
}

static void rom_copy_printable(char *dst, uint32_t cap, const uint8_t *src, uint32_t max)
{
	uint32_t i;

	for (i = 0; i + 1 < cap && i < max && src[i] >= ' ' && src[i] <= 'z'; i++)
		dst[i] = (char)src[i];
	dst[i] = 0;
	while (i && dst[i - 1] == ' ')
		dst[--i] = 0;
}

/*
 * atom_get_vbios_pn / _date / _version from amdgpu's atom.c, read
 * endian-clean (upstream loads the u16 offsets through casts).
 */
static void survey_vbios_strings(struct frb_device *adev)
{
	const uint8_t *rom = adev->bios;
	uint32_t size = adev->bios_size;
	char pn[48], ver[24], date[20];
	uint32_t str_off, start, end;
	int at;

	if (size < 0x100)
		return;
	if (rom[OFFSET_TO_GET_ATOMBIOS_NUMBER_OF_STRINGS])
		str_off = frb_get_le16(rom, OFFSET_TO_GET_ATOMBIOS_STRING_START);
	else
		str_off = OFFSET_TO_VBIOS_PART_NUMBER;
	if (str_off < size && rom[str_off] == 0 && str_off + 1 < size)
		str_off++;
	rom_copy_printable(pn, sizeof(pn), str_off < size ? rom + str_off : rom, str_off < size ? size - str_off : 0);

	start = frb_get_le16(rom, OFFSET_TO_GET_ATOMBIOS_STRING_START);
	end = frb_get_le16(rom, OFFSET_TO_ATOM_ROM_HEADER_POINTER);
	if (end <= start) {
		start = 3;
		end = 1024;
	}
	at = rom_find(rom, size, BIOS_VERSION_PREFIX, start, end);
	if (at >= 0 && (uint32_t)at + 18 < size)
		rom_copy_printable(ver, sizeof(ver), rom + at + 18, size - at - 18);
	else
		ver[0] = 0;
	/* "MM/DD/YY HH:MM" at 0x50 -> 20YY/MM/DD HH:MM */
	date[0] = '2'; date[1] = '0'; date[2] = rom[0x56]; date[3] = rom[0x57]; date[4] = '/';
	date[5] = rom[0x50]; date[6] = rom[0x51]; date[7] = '/'; date[8] = rom[0x53]; date[9] = rom[0x54];
	date[10] = ' '; date[11] = rom[0x59]; date[12] = rom[0x5a]; date[13] = rom[0x5b];
	date[14] = rom[0x5c]; date[15] = rom[0x5d]; date[16] = 0;
	frb_info(adev, "ATOM BIOS: %s, ver: %s, %s (%u bytes)", pn, ver[0] ? ver : "?", date, size);
}

/*
 * The command tables Firebrand's display and power code will call, with the
 * revision RadeonNI's DCE8 code handles. A table whose revision differs is
 * new work for the DCE 11.2 port; its amdgpu reference is named.
 */
static void survey_cmd_tables(struct frb_device *adev)
{
	static const struct {
		const char *name;
		int index;
		uint8_t rdn_frev, rdn_crev;	/* what RadeonNI handles; 0.0 = not called there */
		const char *amdgpu;		/* where amdgpu (legacy DCE or DC) builds it */
	} want[] = {
		{ "ASIC_Init", GetIndexIntoMasterTable(COMMAND, ASIC_Init), 1, 1,
		  "atom.c amdgpu_atom_asic_init (ps[0..1] = default sclk/mclk)" },
		{ "UNIPHYTransmitterControl", GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl), 1, 5,
		  "atombios_encoders.c case 6: DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6" },
		{ "DIGxEncoderControl", GetIndexIntoMasterTable(COMMAND, DIGxEncoderControl), 1, 4,
		  "atombios_encoders.c case 5: DIG_ENCODER_CONTROL_PARAMETERS_V5" },
		{ "SetPixelClock", GetIndexIntoMasterTable(COMMAND, SetPixelClock), 1, 6,
		  "atombios_crtc.c case 7: PIXEL_CLOCK_PARAMETERS_V7" },
		{ "SelectCRTC_Source", GetIndexIntoMasterTable(COMMAND, SelectCRTC_Source), 1, 2,
		  "atombios_encoders.c case 3: SELECT_CRTC_SOURCE_PARAMETERS_V3" },
		{ "SetVoltage", GetIndexIntoMasterTable(COMMAND, SetVoltage), 1, 3,
		  "amdgpu_atombios.c amdgpu_atombios_set_voltage / SET_VOLTAGE_PARAMETERS_V1_4" },
		{ "SetDCEClock", GetIndexIntoMasterTable(COMMAND, SetDCEClock), 0, 0,
		  "dce_v11_0.c / atombios_crtc.c amdgpu_atombios_crtc_set_dce_clock (SET_DCE_CLOCK_PS_ALLOCATION_V2_1)" },
		{ "AdjustDisplayPll", GetIndexIntoMasterTable(COMMAND, AdjustDisplayPll), 1, 3,
		  "atombios_crtc.c amdgpu_atombios_crtc_adjust_pll" },
		{ "EnableDispPowerGating", GetIndexIntoMasterTable(COMMAND, EnableDispPowerGating), 2, 1,
		  "atombios_crtc.c amdgpu_atombios_crtc_powergate" },
		{ "EnableCRTC", GetIndexIntoMasterTable(COMMAND, EnableCRTC), 1, 1, "atombios_crtc.c" },
		{ "BlankCRTC", GetIndexIntoMasterTable(COMMAND, BlankCRTC), 1, 1, "atombios_crtc.c" },
		{ "SetCRTC_UsingDTDTiming", GetIndexIntoMasterTable(COMMAND, SetCRTC_UsingDTDTiming), 2, 3, "atombios_crtc.c" },
		{ "EnableScaler", GetIndexIntoMasterTable(COMMAND, EnableScaler), 1, 1, "atombios_crtc.c" },
		{ "EnableSpreadSpectrumOnPPLL", GetIndexIntoMasterTable(COMMAND, EnableSpreadSpectrumOnPPLL), 1, 3,
		  "atombios_crtc.c case 4? (DC: \"Don't have enable_spread_spectrum_on_ppll for v4\")" },
		{ "DPEncoderService", GetIndexIntoMasterTable(COMMAND, DPEncoderService), 1, 2, "atombios_dp.c" },
		{ "ProcessAuxChannelTransaction", GetIndexIntoMasterTable(COMMAND, ProcessAuxChannelTransaction), 1, 2,
		  "atombios_dp.c (DC uses the native AUX engine instead)" },
		{ "GetEngineClock", GetIndexIntoMasterTable(COMMAND, GetEngineClock), 1, 1, "amdgpu_atombios.c" },
		{ "GetMemoryClock", GetIndexIntoMasterTable(COMMAND, GetMemoryClock), 1, 1, "amdgpu_atombios.c" },
		{ "SetEngineClock", GetIndexIntoMasterTable(COMMAND, SetEngineClock), 1, 1, "(SMU owns clocks on VI)" },
		{ "SetMemoryClock", GetIndexIntoMasterTable(COMMAND, SetMemoryClock), 1, 1, "(SMU owns clocks on VI)" },
		{ "ComputeMemoryEnginePLL", GetIndexIntoMasterTable(COMMAND, ComputeMemoryEnginePLL), 0, 0,
		  "ppatomctrl.c atomctrl_get_engine_pll_dividers_vi (v1.6)" },
		{ "SpeedFanControl", GetIndexIntoMasterTable(COMMAND, SpeedFanControl), 0, 0,
		  "called from ASIC_Init: programs the board's 0x4C fan/thermal device" },
		{ "ProcessI2cChannelTransaction", GetIndexIntoMasterTable(COMMAND, ProcessI2cChannelTransaction), 0, 0,
		  "atombios_i2c.c" },
		{ "ReadEfuseValue", GetIndexIntoMasterTable(COMMAND, ReadEfuseValue), 0, 0,
		  "ppatomctrl.c atomctrl_read_efuse (AVFS)" },
		{ "GetSMUClockInfo", GetIndexIntoMasterTable(COMMAND, GetSMUClockInfo), 0, 0,
		  "ppatomctrl.c atomctrl_get_smc_sclk_range_table" },
	};
	unsigned int i, news = 0;

	for (i = 0; i < ARRAY_SIZE(want); i++) {
		uint8_t frev = 0, crev = 0;

		if (!frb_atom_parse_cmd_header(adev->atom, want[i].index, &frev, &crev)) {
			frb_info(adev, "table %-28s absent", want[i].name);
			continue;
		}
		if (want[i].rdn_frev == 0) {
			frb_info(adev, "table %-28s %u.%u  (not in RadeonNI) %s", want[i].name, frev, crev,
				 want[i].amdgpu);
		} else if (frev != want[i].rdn_frev || crev != want[i].rdn_crev) {
			news++;
			frb_info(adev, "table %-28s %u.%u  NEW: RadeonNI handles %u.%u; amdgpu: %s",
				 want[i].name, frev, crev, want[i].rdn_frev, want[i].rdn_crev, want[i].amdgpu);
		} else {
			frb_info(adev, "table %-28s %u.%u  (as RadeonNI)", want[i].name, frev, crev);
		}
	}
	frb_info(adev, "survey: %u command tables at a revision RadeonNI's DCE8 code does not handle", news);
}

/*
 * The state the card is in, by READS only: whether it is posted, what the
 * VBIOS left in the memory controller and the scratch registers.
 */
static void survey_registers(struct frb_device *adev)
{
	uint32_t s[8], i;

	for (i = 0; i < 8; i++)
		s[i] = RREG32(mmBIOS_SCRATCH_0 + i);
	frb_info(adev, "BIOS_SCRATCH_0..7: %08x %08x %08x %08x %08x %08x %08x %08x",
		 s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
	frb_info(adev, "ASIC_Init complete (BIOS_SCRATCH_7 bit 9): %s; CONFIG_MEMSIZE %u MB; "
		 "MC_VM_FB_LOCATION %08x; MC_SEQ_MISC0 %08x; BIF_FB_EN %x; HDP_NONSURFACE_BASE %08x",
		 (s[7] & ATOM_S7_ASIC_INIT_COMPLETE_MASK) ? "yes" : "no",
		 RREG32(mmCONFIG_MEMSIZE), RREG32(mmMC_VM_FB_LOCATION), RREG32(mmMC_SEQ_MISC0),
		 RREG32(mmBIF_FB_EN), RREG32(mmHDP_NONSURFACE_BASE));
}

void frb_atombios_survey(struct frb_device *adev)
{
	survey_vbios_strings(adev);
	survey_cmd_tables(adev);
	survey_registers(adev);
}
