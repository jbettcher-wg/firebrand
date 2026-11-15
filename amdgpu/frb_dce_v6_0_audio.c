/*
 * DCE 6's DP audio registers (Southern Islands: Tahiti, Pitcairn, Verde,
 * Oland) for frb_dce_audio.c, which is RadeonNI's rdn_dce8_afmt.c (the DCE6
 * arm the LG played DP audio from on the R7 450, 2026-11-11). A
 * file of its own so frb_audio_test runs the same table the kext does.
 *
 * What DCE 6 has that DCE 8 and DCE 11.2 do not: AZ_ENDPOINT_REG_WRITE_EN in
 * the codec endpoint index on writes (dce_v6_0.c dce_v6_0_audio_endpt_wreg),
 * DTO1's pair at 0x170/0x171, the pins' index/data pairs 6 dwords apart
 * (AZF0ENDPOINTn), six pins (dce_v6_0_audio_init). No AFMT_CNTL (the AFMT
 * clock is not gated) and no DC_MEM_GLOBAL_PWR_REQ_CNTL. The DIGs' AFMT
 * blocks sit at the CRTC offsets (sid.h; dce_v6_0.c dig_offsets[]).
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_display.h"
#include "include/sid.h"
#include "include/asic_reg/dce/dce_6_0_d.h"
#include "include/asic_reg/dce/dce_6_0_sh_mask.h"

/*
 * DTO1's module on DCE 6: the display engine clock, kHz (dce_v6_0_afmt_setmode:
 * dce_v6_0_audio_set_dto(encoder, adev->clock.default_dispclk * 10) for DP).
 * RadeonNI took FirmwareInfo's default (600 MHz, where radeon runs the
 * engine); Firebrand runs it at amdgpu's 625 MHz (hw_init), so the module
 * follows the clock the engine is set to.
 */
static uint32_t dce_v6_0_audio_dto1_module_khz(struct frb_device *adev, struct frb_display *d)
{
	(void)adev;
	return d->default_dispclk * 10u;
}

/* DCE 6's DP audio registers for frb_dce_audio.c (dce_6_0_d.h; DIGn's AFMT at the CRTC offsets) */
const struct frb_audio_regs frb_audio_regs_dce6 = {
	.dto_source = mmDCCG_AUDIO_DTO_SOURCE,
	.dto1_phase = mmDCCG_AUDIO_DTO1_PHASE,
	.dto1_module = mmDCCG_AUDIO_DTO1_MODULE,
	.ep_index = mmAZALIA_F0_CODEC_ENDPOINT_INDEX,
	.ep_data = mmAZALIA_F0_CODEC_ENDPOINT_DATA,
	.pin_stride = mmAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX,
	.num_pins = 6,			/* dce_v6_0_audio_init: Tahiti/Pitcairn/Verde (Oland 2) */
	.ep_write_en = AZALIA_F0_CODEC_ENDPOINT_INDEX__AZALIA_ENDPOINT_REG_WRITE_EN_MASK,
	.num_dig = 6,
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
	.afmt_cntl = 0,			/* none on DCE 6 */
	.dto1_module_khz = dce_v6_0_audio_dto1_module_khz,
};
