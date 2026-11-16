/*
 * DCE 8's DP audio registers (Sea Islands: Bonaire, Hawaii) for
 * frb_dce_audio.c, which is RadeonNI's rdn_dce8_afmt.c: its DCE8 arm is the
 * DP audio the LG HDR 4K played from the W8100 (2026-11-11,
 * "DCE8 DP audio configured: DIG4 (afmt 0xb000) <- pin 0, DTO1 24000/652500
 * kHz"). amdgpu's dce_v8_0.c drives HDMI audio only (afmt_setmode for
 * ATOM_ENCODER_MODE_HDMI, DTO0), so the DP sequence is RadeonNI's; the
 * register file is dce_8_0_d.h's and the counts are dce_v8_0's. A file of
 * its own so frb_audio_test runs the same table the kext does.
 *
 * What DCE 8 has, against DCE 6 (frb_dce_v6_0_audio.c) and DCE 11.2
 * (frb_audio_regs_dce11):
 *   - no AZ_ENDPOINT_REG_WRITE_EN in the codec index (dce_v8_0_audio_endpt_wreg
 *     writes the plain index; RadeonNI sets the bit on DCE6 only);
 *   - DTO1's pair at 0x16e/0x16f (DCE 11.2's offsets; DCE 6's are 0x170/0x171);
 *   - seven codec pins (dce_v8_0_audio_init: "BN/HW: 6 streams, 7
 *     endpoints"; RadeonNI's walk), their index/data pairs 6 dwords apart
 *     from 0x1780 (AZF0ENDPOINT0..6, dce_8_0_d.h). amdgpu's cikd.h has
 *     AUD5_REGISTER_OFFSET 0x179d - 0x1780, one short of AZF0ENDPOINT5's
 *     0x179e: the register header and RadeonNI's 0x18-byte stride are
 *     followed, not that offset;
 *   - six DIGs, their AFMT blocks at the CRTC offsets (dce_v8_0.c
 *     dig_offsets[0..5]; RadeonNI's 0x0000, 0x0c00, 0x9800 ... bytes);
 *   - no AFMT_CNTL (the AFMT audio clock is not gated) and no
 *     DC_MEM_GLOBAL_PWR_REQ_CNTL;
 *   - DTO1's module is the DP reference clock, DENTIST's DPREFCLK divider
 *     over FirmwareInfo's ulGPUPLL_OutputFreq (rdn_dce8_dispclk_khz's DCE8
 *     arm, radeon's dce6_dp_audio_set_dto for DCE8), the same function
 *     DCE 11.2 uses.
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_display.h"
#include "include/cikd.h"
#include "include/asic_reg/dce/dce_8_0_d.h"
#include "include/asic_reg/dce/dce_8_0_sh_mask.h"

/* DCE 8's DP audio registers for frb_dce_audio.c (dce_8_0_d.h; DIGn's AFMT at the CRTC offsets) */
const struct frb_audio_regs frb_audio_regs_dce8 = {
	.dto_source = mmDCCG_AUDIO_DTO_SOURCE,
	.dto1_phase = mmDCCG_AUDIO_DTO1_PHASE,
	.dto1_module = mmDCCG_AUDIO_DTO1_MODULE,
	.ep_index = mmAZALIA_F0_CODEC_ENDPOINT_INDEX,
	.ep_data = mmAZALIA_F0_CODEC_ENDPOINT_DATA,
	.pin_stride = mmAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX,
	.num_pins = 7,			/* dce_v8_0_audio_init: Bonaire/Hawaii */
	.ep_write_en = 0,		/* dce_v8_0_audio_endpt_wreg: none */
	.num_dig = 6,			/* dce_v8_0_early_init: Bonaire/Hawaii */
	.dig_off = { CRTC0_REGISTER_OFFSET, CRTC1_REGISTER_OFFSET, CRTC2_REGISTER_OFFSET,
		     CRTC3_REGISTER_OFFSET, CRTC4_REGISTER_OFFSET, CRTC5_REGISTER_OFFSET },
	.hdmi_audio_packet_control = mmHDMI_AUDIO_PACKET_CONTROL,
	.afmt_audio_packet_control2 = mmAFMT_AUDIO_PACKET_CONTROL2,
	.afmt_60958_0 = mmAFMT_60958_0,
	.afmt_60958_1 = mmAFMT_60958_1,
	.afmt_60958_2 = mmAFMT_60958_2,
	.afmt_audio_packet_control = mmAFMT_AUDIO_PACKET_CONTROL,
	.afmt_infoframe_control0 = mmAFMT_INFOFRAME_CONTROL0,
	.afmt_audio_src_control = mmAFMT_AUDIO_SRC_CONTROL,
	.dp_sec_cntl = mmDP_SEC_CNTL,
	.dp_sec_timestamp = mmDP_SEC_TIMESTAMP,
	.afmt_avi_info0 = mmAFMT_AVI_INFO0,
	.afmt_avi_info1 = mmAFMT_AVI_INFO1,
	.afmt_avi_info2 = mmAFMT_AVI_INFO2,
	.afmt_avi_info3 = mmAFMT_AVI_INFO3,
	.hdmi_infoframe_control1 = mmHDMI_INFOFRAME_CONTROL1,
	.afmt_cntl = 0,			/* none on DCE 8 */
	.dto1_module_khz = frb_audio_dprefclk_khz,
};
