/*
 * amdgpu_atomfirmware.c, the parts Vega 20 needs so far: the BIOS scratch
 * registers' place (firmwareinfo v3), the interpreter's FB scratch
 * (vram_usagebyfirmware v2.1), the clocks amdgpu_device_init reads
 * (firmwareinfo, smu_info, umc_info), the VRAM type and width gmc_v9_0
 * takes from vram_info v2.3; plus the survey (VBIOS identity, the data and
 * command tables Firebrand's display and power code will meet, the card's
 * state by reads), which has no Linux counterpart.
 *
 * SoC15 VBIOSes are "atomfirmware": the same ATOM interpreter and the same
 * ROM header, but new master-list indices (atom_master_list_of_*_v2_1) and
 * new table layouts. The tables are read with frb_get_le16/32 at offsetof()
 * positions in atomfirmware.h's packed structures, never dereferenced whole.
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
#include "../shared/atom/atomfirmware-frb.h"
#include "frb_soc15.h"

/* atomfirmware.h's get_index_into_master_table: the dword index of a uint16_t member */
#define FW_DATA_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_data_tables_v2_1, member) / sizeof(uint16_t)))
#define FW_CMD_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_command_functions_v2_1, member) / sizeof(uint16_t)))

/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_VRAM_TYPE_UNKNOWN	0
#define AMDGPU_VRAM_TYPE_GDDR5		5
#define AMDGPU_VRAM_TYPE_HBM		6
#define AMDGPU_VRAM_TYPE_GDDR6		9

int frb_atomfirmware_data_table(struct frb_device *adev, int index, uint8_t *frev, uint8_t *crev, uint16_t *offset)
{
	uint16_t off = 0;
	uint8_t f = 0, c = 0;

	if (!frb_atom_parse_data_header(adev->atom, index, NULL, &f, &c, &off))
		return 0;
	if (frev)
		*frev = f;
	if (crev)
		*crev = c;
	if (offset)
		*offset = off;
	return 1;
}

/* amdgpu_atomfirmware_scratch_regs_init: no register access, only the place */
void frb_atomfirmware_scratch_regs_init(struct frb_device *adev)
{
	uint16_t off;

	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(firmwareinfo), NULL, NULL, &off))
		adev->bios_scratch_reg_offset =
			frb_get_le32(adev->bios, off + offsetof(struct atom_firmware_info_v3_1, bios_scratch_reg_startaddr));
}

/* amdgpu_atomfirmware_allocate_fb_scratch, the v2.1 arm (Vega 20) and the 20 KB default */
int frb_atomfirmware_allocate_fb_scratch(struct frb_device *adev)
{
	struct atom_context *ctx = adev->atom;
	uint32_t start_addr, fw_size, drv_size;
	int usage_bytes = 0;
	uint16_t off;
	uint8_t frev, crev;

	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(vram_usagebyfirmware), &frev, &crev, &off)) {
		if (frev == 2 && crev == 1) {
			start_addr = frb_get_le32(ctx->bios, off + offsetof(struct vram_usagebyfirmware_v2_1, start_address_in_kb));
			fw_size = frb_get_le16(ctx->bios, off + offsetof(struct vram_usagebyfirmware_v2_1, used_by_firmware_in_kb));
			drv_size = frb_get_le16(ctx->bios, off + offsetof(struct vram_usagebyfirmware_v2_1, used_by_driver_in_kb));
			frb_dbg(adev, "atom firmware v2_1 requested %08x %ukb fw %ukb drv", start_addr, fw_size, drv_size);
			if ((start_addr & ATOM_VRAM_OPERATION_FLAGS_MASK) ==
			    (uint32_t)(ATOM_VRAM_BLOCK_SRIOV_MSG_SHARE_RESERVATION << ATOM_VRAM_OPERATION_FLAGS_SHIFT))
				usage_bytes = 0;	/* SR-IOV: the default scratch */
			else
				usage_bytes = drv_size << 10;
			if (fw_size)
				frb_info(adev, "atomfirmware: the VBIOS reserves %u KB of VRAM at %u KB for itself "
					 "(not yet honoured by Firebrand's placement)", fw_size,
				 (uint32_t)(start_addr & ~(uint32_t)ATOM_VRAM_OPERATION_FLAGS_MASK));
		} else {
			frb_err(adev, "vram_usagebyfirmware %u.%u: only 2.1 is ported", frev, crev);
		}
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
 * amdgpu_atomfirmware_get_clock_info: the boot clocks (firmwareinfo v3), the
 * SPLL reference (smu_info v3 core_refclk_10khz, soc15_get_xclk's value) and
 * the memory reference (umc_info v3 mem_refclk_10khz).
 */
int frb_atomfirmware_get_clock_info(struct frb_device *adev)
{
	uint16_t off;
	uint8_t frev, crev;
	uint32_t mem_ref = 0;
	int ret = -EINVAL;

	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(firmwareinfo), &frev, &crev, &off)) {
		adev->default_sclk = frb_get_le32(adev->bios, off + offsetof(struct atom_firmware_info_v3_1, bootup_sclk_in10khz));
		adev->default_mclk = frb_get_le32(adev->bios, off + offsetof(struct atom_firmware_info_v3_1, bootup_mclk_in10khz));
		ret = 0;
	}
	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(smu_info), &frev, &crev, &off)) {
		if (frev == 3)
			adev->spll_reference_freq = frb_get_le32(adev->bios, off + offsetof(struct atom_smu_info_v3_1, core_refclk_10khz));
		else if (frev == 4)
			adev->spll_reference_freq = frb_get_le32(adev->bios, off + offsetof(struct atom_smu_info_v4_0, core_refclk_10khz));
		ret = 0;
	}
	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(umc_info), &frev, &crev, &off)) {
		mem_ref = frb_get_le32(adev->bios, off + offsetof(struct atom_umc_info_v3_1, mem_refclk_10khz));
		ret = 0;
	}
	frb_info(adev, "firmwareinfo: boot engine clock %u kHz, memory clock %u kHz; core/memory reference %u/%u kHz",
		 adev->default_sclk * 10, adev->default_mclk * 10, adev->spll_reference_freq * 10, mem_ref * 10);
	return ret;
}

/* convert_atom_mem_type_to_vram_type, the discrete arm */
static int convert_atom_mem_type_to_vram_type(int atom_mem_type)
{
	switch (atom_mem_type) {
	case ATOM_DGPU_VRAM_TYPE_GDDR5:	return AMDGPU_VRAM_TYPE_GDDR5;
	case ATOM_DGPU_VRAM_TYPE_HBM2:
	case ATOM_DGPU_VRAM_TYPE_HBM2E:
	case ATOM_DGPU_VRAM_TYPE_HBM3:	return AMDGPU_VRAM_TYPE_HBM;
	case ATOM_DGPU_VRAM_TYPE_GDDR6:	return AMDGPU_VRAM_TYPE_GDDR6;
	default:			return AMDGPU_VRAM_TYPE_UNKNOWN;
	}
}

/*
 * amdgpu_atomfirmware_get_vram_info, the vram_info v2.3 arm (Vega 20): the
 * module BIOS_SCRATCH_4 names (bits 23:16), its atom_vram_module_v9.
 */
int frb_atomfirmware_get_vram_info(struct frb_device *adev, int *vram_width, int *vram_type, int *vram_vendor)
{
	const uint8_t *b = adev->bios;
	uint16_t off;
	uint8_t frev, crev, mem_type, num;
	uint32_t module_id, mod, i;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(vram_info), &frev, &crev, &off))
		return -EINVAL;
	module_id = (RREG32(adev->bios_scratch_reg_offset + 4) & 0x00ff0000) >> 16;
	if (frev != 2 || crev != 3) {
		frb_err(adev, "vram_info %u.%u: only 2.3 is ported", frev, crev);
		return -EINVAL;
	}
	num = b[off + offsetof(struct atom_vram_info_header_v2_3, vram_module_num)];
	if (module_id > num)
		module_id = 0;
	mod = off + offsetof(struct atom_vram_info_header_v2_3, vram_module);
	for (i = 0; i < module_id; i++)
		mod += frb_get_le16(b, mod + offsetof(struct atom_vram_module_v9, vram_module_size));
	mem_type = b[mod + offsetof(struct atom_vram_module_v9, memory_type)];
	if (vram_type)
		*vram_type = convert_atom_mem_type_to_vram_type(mem_type);
	if (vram_width)
		*vram_width = b[mod + offsetof(struct atom_vram_module_v9, channel_num)] *
			      (1 << b[mod + offsetof(struct atom_vram_module_v9, channel_width)]);
	if (vram_vendor)
		*vram_vendor = b[mod + offsetof(struct atom_vram_module_v9, vender_rev_id)] & 0xF;
	return 0;
}

/* --- the survey ------------------------------------------------------------ */

/*
 * The atomfirmware tables Firebrand's code will read or run, at the revision
 * its Linux reference handles. A different revision is new work.
 */
static void survey_tables(struct frb_device *adev)
{
	static const struct {
		const char *name;
		int index;
		uint8_t frev, crev;	/* what the Linux reference handles; 0.0 = any */
		const char *who;
	} data[] = {
		{ "firmwareinfo", FW_DATA_INDEX(firmwareinfo), 3, 0, "amdgpu_atomfirmware.c (v3.x)" },
		{ "smu_info", FW_DATA_INDEX(smu_info), 3, 0, "amdgpu_atomfirmware_get_clock_info (v3.x)" },
		{ "umc_info", FW_DATA_INDEX(umc_info), 3, 0, "amdgpu_atomfirmware_get_clock_info / ras" },
		{ "vram_info", FW_DATA_INDEX(vram_info), 2, 3, "amdgpu_atomfirmware_get_vram_info v2.3" },
		{ "vram_usagebyfirmware", FW_DATA_INDEX(vram_usagebyfirmware), 2, 1, "allocate_fb_scratch v2.1" },
		{ "powerplayinfo", FW_DATA_INDEX(powerplayinfo), 11, 0, "vega20_processpptables.c (ATOM_Vega20_POWERPLAYTABLE)" },
		{ "smc_dpm_info", FW_DATA_INDEX(smc_dpm_info), 4, 4, "vega20 append_vbios_pptable (atom_smc_dpm_info_v4_4)" },
		{ "displayobjectinfo", FW_DATA_INDEX(displayobjectinfo), 1, 4, "DC bios_parser2 (display_object_info_table_v1_4)" },
		{ "dce_info", FW_DATA_INDEX(dce_info), 4, 1, "DC bios_parser2 get_firmware_info / dce120 clocks" },
		{ "gpio_pin_lut", FW_DATA_INDEX(gpio_pin_lut), 2, 1, "DC bios_parser2 GPIO (HPD, DDC)" },
		{ "voltageobject_info", FW_DATA_INDEX(voltageobject_info), 4, 0, "powerplay (SMU owns voltages on Vega)" },
		{ "gfx_info", FW_DATA_INDEX(gfx_info), 2, 0, "gfx_v9_0 / amdgpu_atomfirmware_get_gfx_info" },
	};
	static const struct {
		const char *name;
		int index;
		uint8_t frev, crev;
		const char *who;
	} cmd[] = {
		{ "asic_init", FW_CMD_INDEX(asic_init), 2, 1, "amdgpu_atom_asic_init (ps[0..1] = boot sclk/mclk)" },
		{ "digxencodercontrol", FW_CMD_INDEX(digxencodercontrol), 1, 5, "DC command_table2 dig_encoder_control_v1_5" },
		{ "dig1transmittercontrol", FW_CMD_INDEX(dig1transmittercontrol), 1, 6, "DC command_table2 transmitter_control_v1_6" },
		{ "setpixelclock", FW_CMD_INDEX(setpixelclock), 1, 7, "DC command_table2 set_pixel_clock_v7" },
		{ "setdceclock", FW_CMD_INDEX(setdceclock), 2, 1, "DC command_table2 set_dce_clock_v2_1" },
		{ "setcrtc_usingdtdtiming", FW_CMD_INDEX(setcrtc_usingdtdtiming), 2, 3, "DC command_table2 set_crtc_using_dtd_timing_v3" },
		{ "enablecrtc", FW_CMD_INDEX(enablecrtc), 1, 1, "DC command_table2 enable_crtc_v1" },
		{ "blankcrtc", FW_CMD_INDEX(blankcrtc), 1, 1, "DC command_table2 blank_crtc" },
		{ "selectcrtc_source", FW_CMD_INDEX(selectcrtc_source), 1, 3, "DC (dce120 uses registers)" },
		{ "enabledisppowergating", FW_CMD_INDEX(enabledisppowergating), 2, 1, "DC command_table2 enable_disp_power_gating_v2_1" },
		{ "getsmuclockinfo", FW_CMD_INDEX(getsmuclockinfo), 3, 1, "DC command_table2 get_smu_clock_info_v3_1" },
		{ "processauxchanneltransaction", FW_CMD_INDEX(processauxchanneltransaction), 1, 2, "(DC drives the AUX engine itself)" },
		{ "processi2cchanneltransaction", FW_CMD_INDEX(processi2cchanneltransaction), 1, 1, "amdgpu_atombios_i2c" },
		{ "computegpuclockparam", FW_CMD_INDEX(computegpuclockparam), 1, 8, "(the SMU owns clocks on Vega)" },
		{ "readefusevalue", FW_CMD_INDEX(readefusevalue), 3, 1, "(fuses through the SMU on Vega)" },
		{ "getmemoryclock", FW_CMD_INDEX(getmemoryclock), 1, 1, "-" },
		{ "dynamicmemorysettings", FW_CMD_INDEX(dynamicmemorysettings), 2, 1, "(VBIOS-internal)" },
		{ "memorytraining", FW_CMD_INDEX(memorytraining), 1, 2, "(HBM2 is trained by the IFWI at power-on)" },
	};
	unsigned int i, news = 0;
	uint16_t off;

	for (i = 0; i < ARRAY_SIZE(data); i++) {
		uint8_t f = 0, c = 0;

		if (!frb_atomfirmware_data_table(adev, data[i].index, &f, &c, &off)) {
			frb_info(adev, "data table %-22s absent", data[i].name);
		} else if (f != data[i].frev || (data[i].crev && c != data[i].crev)) {
			news++;
			frb_info(adev, "data table %-22s %u.%u  NEW: the reference handles %u.%u; %s", data[i].name, f, c,
				 data[i].frev, data[i].crev, data[i].who);
		} else {
			frb_info(adev, "data table %-22s %u.%u  %s", data[i].name, f, c, data[i].who);
		}
	}
	for (i = 0; i < ARRAY_SIZE(cmd); i++) {
		uint8_t f = 0, c = 0;

		if (!frb_atom_parse_cmd_header(adev->atom, cmd[i].index, &f, &c)) {
			frb_info(adev, "command table %-29s absent", cmd[i].name);
		} else if (f != cmd[i].frev || c != cmd[i].crev) {
			news++;
			frb_info(adev, "command table %-29s %u.%u  NEW: the reference handles %u.%u; %s", cmd[i].name, f, c,
				 cmd[i].frev, cmd[i].crev, cmd[i].who);
		} else {
			frb_info(adev, "command table %-29s %u.%u  %s", cmd[i].name, f, c, cmd[i].who);
		}
	}
	frb_info(adev, "survey: %u atomfirmware tables at a revision the Linux reference does not handle", news);
}

/* the VRAM and the board the VBIOS describes */
static void survey_memory(struct frb_device *adev)
{
	const uint8_t *b = adev->bios;
	uint16_t off;
	uint8_t frev, crev;

	if (frb_atomfirmware_data_table(adev, FW_DATA_INDEX(firmwareinfo), &frev, &crev, &off) && frev == 3 && crev >= 2)
		frb_info(adev, "firmwareinfo %u.%u: capability 0x%x, cooling solution %u (0 air, 1 liquid), "
			 "boot VDDC %u mV, MVDDC %u mV, mem module %u", frev, crev,
			 frb_get_le32(b, off + offsetof(struct atom_firmware_info_v3_2, firmware_capability)),
			 b[off + offsetof(struct atom_firmware_info_v3_2, coolingsolution_id)],
			 frb_get_le16(b, off + offsetof(struct atom_firmware_info_v3_2, bootup_vddc_mv)),
			 frb_get_le16(b, off + offsetof(struct atom_firmware_info_v3_2, bootup_mvddc_mv)),
			 b[off + offsetof(struct atom_firmware_info_v3_2, mem_module_id)]);
}

void frb_atomfirmware_survey(struct frb_device *adev)
{
	frb_atombios_survey_strings(adev);
	survey_tables(adev);
	survey_memory(adev);
	frb_soc15_survey_registers(adev);
}
