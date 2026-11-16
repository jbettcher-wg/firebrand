/*
 * DisplayPort audio, RadeonNI's (RadeonNI's hw/rdn_dce8_afmt.c) on
 * DCE 11.2's registers (its DCE8 arm), DCE 8's (the same arm, on its own
 * registers) and DCE 6's (its DCE6 arm).
 *
 * PER DCE. Every register and layout fact comes from the DCE's table,
 * frb_display_funcs.audio_regs (struct frb_audio_regs): frb_audio_regs_dce11
 * at the end of this file, frb_audio_regs_dce6 in frb_dce_v6_0_audio.c (the
 * endpoint index's write-enable bit, DCE 6's DTO1 pair, six pins 6 dwords
 * apart, no AFMT_CNTL, DTO1's module from the display engine clock),
 * frb_audio_regs_dce8 in frb_dce_v8_0_audio.c (RadeonNI's DCE8 arm at its
 * own registers: seven pins 6 dwords apart, no write-enable bit, no
 * AFMT_CNTL, DTO1's module the DP reference clock as here). What follows
 * describes the DCE 11.2 table; tests/frb_audio_test.c holds all three
 * against RadeonNI's arms, write for write.
 *
 * WHY RADEONNI'S. It is the DP audio that has been heard on this platform
 * and this monitor: an LG HDR 4K played it from the R7 450
 * (DCE6) and from the W8100 (DCE8), 2026-11-11 (RadeonNI). The
 * first Firebrand port followed Linux DC's sequence, which nobody has heard
 * on these cards, and both Polaris cards stayed silent with it.
 *
 * THE PORT. Function for function and register for register, in RadeonNI's
 * order; the names are RadeonNI's with frb_ for rdn_, static ones unchanged.
 * Every register keeps its DCE8 field layout on DCE 11.2 (dce_8_0_sh_mask.h
 * against dce_11_2_sh_mask.h: DCCG_AUDIO_DTO_SOURCE, the DTO1 pair,
 * DENTIST_DISPCLK_CNTL, HDMI_AUDIO_PACKET_CONTROL, HDMI_INFOFRAME_CONTROL1,
 * AFMT_AVI_INFO0-3, AFMT_AUDIO_PACKET_CONTROL(2), AFMT_60958_0-2,
 * AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_SRC_CONTROL, DP_SEC_CNTL,
 * DP_SEC_TIMESTAMP and the endpoint registers RadeonNI writes are identical);
 * only the offsets move:
 *   - the codec pins' index/data pairs: DCE8 0x5e00 + 0x18 n (bytes),
 *     DCE 11.2 AZF0ENDPOINTn = 0x17a8 + 4 n (dwords); no write-enable bit
 *     in the index on either (RadeonNI sets it on DCE6 only);
 *   - the DIG blocks: DCE8 0x7000 + afmt (bytes), DCE 11.2 0x4a00 + 0x100 n.
 * One step is DCE 10+'s and has no DCE8 counterpart: AFMT_CNTL's
 * AFMT_AUDIO_CLOCK_EN, the AFMT block's audio clock gate (DCE8 has no
 * AFMT_CNTL; its AFMT runs on an ungated clock). It goes on in dp_mode_set
 * before the first AFMT write and off when the DIG's audio is released.
 *
 * WHEN (frb_dce_v11_0.c's set_mode, and dce_v6_0's and dce_v8_0's, as rdn_modeset):
 *   - hw_init at stage dce, after the display engine clocks
 *     (rdn_dce8_afmt_init after SetPixelClock's display engine PLL);
 *   - encoder prepare: dp_enable(false), DP_VIDEO_OFF, transmitter off,
 *     then dp_mode_set (the DTO, the DIG's AFMT, the pin's sink description);
 *   - encoder commit: transmitter on, link training, DP_VIDEO_ON, then
 *     dp_enable(true) when the training succeeded (packets, then the pin's
 *     AUDIO_ENABLED, which is what HDAudio's pin sense sees);
 *   - frb_display_off: dp_enable(false) first (rdn_output_disable);
 *   - unload: frb_audio_fini.
 *
 * WHICH PIN. RadeonNI's select_pin_for_dig: the pin this DIG already has,
 * else the first connected free one. On the test machine that was pin 0 for the LG on
 * DIG4, with both RadeonNI cards. `frbctl audio pin N` overrides it.
 *
 * Copyright 2008-2014 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_display.h"
#include "../shared/atom/atombios.h"
#include "include/asic_reg/dce/dce_11_2_d.h"
#include "include/asic_reg/dce/dce_11_2_sh_mask.h"
#include "include/vid.h"

/* RadeonNI's register names (rdn_dce8_afmt.h), at their DCE 11.2 dword offsets */
#define RDN_DENTIST_DISPCLK_CNTL		mmDENTIST_DISPCLK_CNTL
#define RDN_DENTIST_DISPCLK_WDIVIDER_SHIFT	24	/* DENTIST_DPREFCLK_WDIVIDER, as radeon's cikd.h has it */
#define RDN_DENTIST_DISPCLK_WDIVIDER_MASK	0x7fu

#define RDN_DCCG_AUDIO_DTO_SOURCE		mmDCCG_AUDIO_DTO_SOURCE
#define RDN_DCCG_AUDIO_DTO0_SOURCE_SEL(crtc)	((uint32_t)((crtc) & 7u))
#define RDN_DCCG_AUDIO_DTO_SEL			(1u << 4)
#define RDN_DCE8_DCCG_AUDIO_DTO1_PHASE		mmDCCG_AUDIO_DTO1_PHASE
#define RDN_DCE8_DCCG_AUDIO_DTO1_MODULE		mmDCCG_AUDIO_DTO1_MODULE

#define RDN_AZ_F0_CODEC_ENDPOINT_INDEX		mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX
#define RDN_AZ_F0_CODEC_ENDPOINT_DATA		mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA

#define RDN_AZ_PIN_CHANNEL_SPEAKER		0x25u
#define RDN_AZ_SPEAKER_ALLOCATION_MASK		0x0000007fu
#define RDN_AZ_HDMI_CONNECTION			(1u << 16)
#define RDN_AZ_DP_CONNECTION			(1u << 17)
#define RDN_AZ_PIN_AUDIO_DESCRIPTOR0		0x28u
#define RDN_AZ_PIN_AUDIO_DESCRIPTOR_COUNT	14u
#define RDN_AZ_PIN_RESPONSE_LIPSYNC		0x37u
#define RDN_AZ_PIN_SINK_INFO0			0x3au
#define RDN_AZ_PIN_SINK_INFO1			0x3bu
#define RDN_AZ_PIN_SINK_INFO2			0x3cu
#define RDN_AZ_PIN_SINK_INFO3			0x3du
#define RDN_AZ_PIN_SINK_INFO4			0x3eu
#define RDN_AZ_PIN_HOT_PLUG_CONTROL		0x54u
#define RDN_AZ_AUDIO_ENABLED			(1u << 31)
#define RDN_AZ_PIN_CONFIG_DEFAULT		0x56u

/* the DIG block's registers, at DIG0's offsets */
#define RDN_HDMI_AUDIO_PACKET_CONTROL		mmHDMI_AUDIO_PACKET_CONTROL
#define RDN_AFMT_AUDIO_PACKET_CONTROL2		mmAFMT_AUDIO_PACKET_CONTROL2
#define RDN_AFMT_60958_0			mmAFMT_60958_0
#define RDN_AFMT_60958_1			mmAFMT_60958_1
#define RDN_AFMT_60958_2			mmAFMT_60958_2
#define RDN_AFMT_AUDIO_PACKET_CONTROL		mmAFMT_AUDIO_PACKET_CONTROL
#define RDN_AFMT_AUDIO_SAMPLE_SEND		(1u << 0)
#define RDN_AFMT_RESET_FIFO_WHEN_AUDIO_DIS	(1u << 11)
#define RDN_AFMT_60958_CS_UPDATE		(1u << 26)
#define RDN_AFMT_INFOFRAME_CONTROL0		mmAFMT_INFOFRAME_CONTROL0
#define RDN_AFMT_AUDIO_INFO_UPDATE		(1u << 7)
#define RDN_AFMT_AUDIO_SRC_CONTROL		mmAFMT_AUDIO_SRC_CONTROL
#define RDN_EVERGREEN_DP_SEC_CNTL		mmDP_SEC_CNTL
#define RDN_DP_SEC_STREAM_ENABLE		(1u << 0)
#define RDN_DP_SEC_ASP_ENABLE			(1u << 4)
#define RDN_DP_SEC_ATP_ENABLE			(1u << 8)
#define RDN_DP_SEC_AIP_ENABLE			(1u << 12)
#define RDN_DP_SEC_AUDIO_MASK	(RDN_DP_SEC_STREAM_ENABLE | RDN_DP_SEC_ASP_ENABLE | \
				 RDN_DP_SEC_ATP_ENABLE | RDN_DP_SEC_AIP_ENABLE)
#define RDN_EVERGREEN_DP_SEC_TIMESTAMP		mmDP_SEC_TIMESTAMP
#define RDN_DP_SEC_TIMESTAMP_MODE_MASK		0x3u
#define RDN_DP_SEC_TIMESTAMP_AUTO		0x1u
#define AFMT_AVI_INFO0				mmAFMT_AVI_INFO0
#define AFMT_AVI_INFO1				mmAFMT_AVI_INFO1
#define AFMT_AVI_INFO2				mmAFMT_AVI_INFO2
#define AFMT_AVI_INFO3				mmAFMT_AVI_INFO3
#define HDMI_INFOFRAME_CONTROL1			mmHDMI_INFOFRAME_CONTROL1
#define HDMI_AVI_INFO_LINE(x)			(((x) & 0x3fu) << 0)
#define HDMI_AVI_INFO_LINE_MASK			(0x3fu << 0)

/* DCE 10+ only: the AFMT block's audio clock gate (no DCE8 counterpart) */
#define FRB_AFMT_CNTL				mmAFMT_CNTL
#define FRB_AFMT_AUDIO_CLOCK_EN			AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK

/* the DCE's DP audio registers (frb_display_funcs.audio_regs) */
static const struct frb_audio_regs *AR(struct frb_device *adev)
{
	return frb_funcs(adev)->display->audio_regs;
}

/* DCE8/DCE6: pin_offsets[] 0x18 bytes (6 dwords) apart; DCE 11.2: AZF0ENDPOINTn, 4 dwords apart */
#define pin_offset(pin)		(AR(adev)->pin_stride * (uint32_t)(pin))

#define dig_afmt_offsets	(AR(adev)->dig_off)
#define RDN_DCE8_AFMT_NUM_DIG	((int)AR(adev)->num_dig)
#define RDN_DCE8_AFMT_NUM_PINS	((int)AR(adev)->num_pins)

static uint32_t endpoint_rreg(struct frb_device *adev, uint32_t pin_off, uint8_t reg)
{
	WREG32(AR(adev)->ep_index + pin_off, (uint32_t)reg);
	return RREG32(AR(adev)->ep_data + pin_off);
}

/* DCE8 and DCE 11.2: no AZ_ENDPOINT_REG_WRITE_EN in the index; DCE6 needs it (dce6_endpoint_wreg) */
static void endpoint_wreg(struct frb_device *adev, uint32_t pin_off, uint8_t reg, uint32_t val)
{
	WREG32(AR(adev)->ep_index + pin_off, (uint32_t)reg | AR(adev)->ep_write_en);
	WREG32(AR(adev)->ep_data + pin_off, val);
}

static void pin_audio_enable(struct frb_device *adev, uint32_t pin_off, bool enable)
{
	uint32_t v = endpoint_rreg(adev, pin_off, RDN_AZ_PIN_HOT_PLUG_CONTROL);
	uint32_t want = enable ? (v | RDN_AZ_AUDIO_ENABLED) : (v & ~RDN_AZ_AUDIO_ENABLED);

	if (want != v)
		endpoint_wreg(adev, pin_off, RDN_AZ_PIN_HOT_PLUG_CONTROL, want);
}

/* the AFMT block's audio clock (DCE 10+; see the top of the file); DCE6 has no AFMT_CNTL */
static void afmt_clock(struct frb_device *adev, uint32_t afmt_off, bool on)
{
	uint32_t v, want;

	if (!AR(adev)->afmt_cntl)
		return;
	v = RREG32(AR(adev)->afmt_cntl + afmt_off);
	want = on ? (v | AR(adev)->afmt_audio_clock_en) : (v & ~AR(adev)->afmt_audio_clock_en);
	if (want != v)
		WREG32(AR(adev)->afmt_cntl + afmt_off, want);
}

uint32_t frb_audio_decode_dfs_div(uint32_t did)
{
	if (did >= 8u && did < 64u)
		return (did - 8u) * 25u + 200u;
	if (did >= 64u && did < 96u)
		return (did - 64u) * 50u + 1600u;
	if (did >= 96u && did < 128u)
		return (did - 96u) * 100u + 3200u;
	return 0;
}

/*
 * The DP audio DTO1 MODULE, in kHz: rdn_dce8_dispclk_khz's DCE8 arm.
 * FirmwareInfo v2.2 ulGPUPLL_OutputFreq (0 -> 360000, 3.6 GHz), scaled by
 * the DENTIST DPREFCLK divider (clock * 100 / div): the DP reference clock.
 * DCE 11.2's and DCE 8's (frb_dce_v8_0_audio.c) table both take it;
 * DENTIST_DISPCLK_CNTL is 0x124 with the divider in bits 24-30 on both.
 */
uint32_t frb_audio_dprefclk_khz(struct frb_device *adev, struct frb_display *d)
{
	uint16_t data_offset = 0, size = 0;
	uint8_t frev = 0, crev = 0;
	const uint8_t *info = NULL;
	uint32_t vco_10khz = 0, cntl, wdiv, div_x100;

	if (adev->atom && adev->bios &&
	    frb_atom_parse_data_header(adev->atom, GetIndexIntoMasterTable(DATA, FirmwareInfo),
				       &size, &frev, &crev, &data_offset))
		info = (const uint8_t *)adev->bios + data_offset;

	if (info && frev >= 2u && crev >= 2u && size >= 0x18u)
		vco_10khz = frb_get_le32(info, 0x14u);
	if (vco_10khz == 0u || vco_10khz > 4294967u)
		vco_10khz = 360000u;
	cntl = RREG32(RDN_DENTIST_DISPCLK_CNTL);
	wdiv = (cntl >> RDN_DENTIST_DISPCLK_WDIVIDER_SHIFT) & RDN_DENTIST_DISPCLK_WDIVIDER_MASK;
	div_x100 = frb_audio_decode_dfs_div(wdiv);
	/* 32-bit math (no __udivdi3 in a 32-bit PPC kext): vco_10khz <= 4,294,967 keeps vco_10khz * 1000 in range */
	return div_x100 ? (vco_10khz * 1000u) / div_x100 : vco_10khz * 10u;
}

/* rdn_dce8_afmt_init: probe the codec pins and quiesce AUDIO_ENABLED until a mode with audio is set */
void frb_audio_hw_init(struct frb_device *adev, struct frb_display *d)
{
	struct frb_afmt *afmt = &d->afmt;
	unsigned i, connected = 0;

	memset(afmt, 0, sizeof(*afmt));
	afmt->active_dig = -1;
	afmt->active_pin = -1;
	afmt->dprefclk_khz = AR(adev)->dto1_module_khz(adev, d);

	for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
		uint32_t cfg = endpoint_rreg(adev, pin_offset(i), RDN_AZ_PIN_CONFIG_DEFAULT);
		bool conn = ((cfg >> 30) & 3u) != 1u;

		afmt->pins[i].id = (uint8_t)i;
		afmt->pins[i].connected = conn;
		afmt->pins[i].dig_encoder = -1;
		if (conn) {
			connected++;
			pin_audio_enable(adev, pin_offset(i), false);
		}
	}

	if (connected == 0u) {
		afmt->pins[0].connected = true;
		pin_audio_enable(adev, pin_offset(0), false);
		connected = 1u;
	}

	afmt->initialized = true;
	frb_info(adev, "audio: init: %u/%u codec pin(s) connected, DPREFCLK %u kHz",
		 connected, (unsigned)RDN_DCE8_AFMT_NUM_PINS, (unsigned)afmt->dprefclk_khz);
}

/*
 * RadeonNI's select_pin_for_dig. With `frbctl audio pin N` (adev->audio_pin
 * >= 0) that pin instead, the DIG's earlier one let go.
 */
static int select_pin_for_dig(struct frb_device *adev, struct frb_afmt *afmt, uint8_t dig_encoder)
{
	unsigned i;
	int forced = adev->audio_pin;

	if (forced >= 0 && forced < RDN_DCE8_AFMT_NUM_PINS) {
		for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
			if ((int)i != forced && afmt->pins[i].dig_encoder == (int8_t)dig_encoder) {
				pin_audio_enable(adev, pin_offset(i), false);
				afmt->pins[i].dig_encoder = -1;
			}
		}
		afmt->pins[forced].dig_encoder = (int8_t)dig_encoder;
		return forced;
	}
	for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
		if (afmt->pins[i].connected &&
		    afmt->pins[i].dig_encoder == (int8_t)dig_encoder)
			return (int)i;
	}
	for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
		if (afmt->pins[i].connected && afmt->pins[i].dig_encoder < 0) {
			afmt->pins[i].dig_encoder = (int8_t)dig_encoder;
			return (int)i;
		}
	}
	afmt->pins[0].dig_encoder = (int8_t)dig_encoder;
	return 0;
}

static void release_pin_for_dig(struct frb_device *adev, struct frb_afmt *afmt, uint8_t dig_encoder)
{
	unsigned i;

	for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
		if (afmt->pins[i].dig_encoder == (int8_t)dig_encoder) {
			pin_audio_enable(adev, pin_offset(i), false);
			afmt->pins[i].dig_encoder = -1;
		}
	}
	if (afmt->active_dig == (int8_t)dig_encoder) {
		afmt->active_dig = -1;
		afmt->active_pin = -1;
		afmt->has_audio = false;
	}
	afmt_clock(adev, dig_afmt_offsets[dig_encoder], false);
}

static void write_pin_sad_regs(struct frb_device *adev, uint32_t pin_off, const struct frb_edid_audio *aud)
{
	uint8_t stereo_freqs = 0;
	unsigned i, k;

	for (k = 0; k < aud->sad_count; k++) {
		uint8_t fmt = (aud->sads[k][0] >> 3) & 0x0fu;
		uint8_t max_ch = aud->sads[k][0] & 0x07u;

		if (fmt == 1u && max_ch == 1u)
			stereo_freqs |= aud->sads[k][1];
	}

	for (i = 0; i < RDN_AZ_PIN_AUDIO_DESCRIPTOR_COUNT; i++) {
		uint8_t want_fmt = (uint8_t)(i + 1u);
		uint32_t val = 0;
		int best = -1;
		uint8_t best_ch = 0;

		for (k = 0; k < aud->sad_count; k++) {
			uint8_t fmt = (aud->sads[k][0] >> 3) & 0x0fu;
			uint8_t ch = aud->sads[k][0] & 0x07u;

			if (fmt != want_fmt)
				continue;
			if (best < 0 || ch > best_ch) {
				best = (int)k;
				best_ch = ch;
			}
		}

		if (best >= 0) {
			uint8_t b0 = aud->sads[best][0] & 0x07u;
			uint8_t b1 = aud->sads[best][1];
			uint8_t b2 = aud->sads[best][2];
			uint8_t st = (want_fmt == 1u) ? (stereo_freqs ? stereo_freqs : b1) : 0u;

			val = (uint32_t)b0 |
			      ((uint32_t)b1 << 8) |
			      ((uint32_t)b2 << 16) |
			      ((uint32_t)st << 24);
		}

		endpoint_wreg(adev, pin_off, (uint8_t)(RDN_AZ_PIN_AUDIO_DESCRIPTOR0 + i), val);
	}
}

static void write_pin_sink_info(struct frb_device *adev, uint32_t pin_off, const struct frb_edid_audio *aud)
{
	uint8_t desc[FRB_SINK_NAME_MAX];
	unsigned r;

	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_SINK_INFO0,
		      (uint32_t)aud->mfg_id | ((uint32_t)aud->prod_id << 16));
	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_SINK_INFO1, (uint32_t)aud->sink_name_len);
	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_SINK_INFO2, 0u);
	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_SINK_INFO3, 0u);

	memset(desc, 0, sizeof(desc));
	if (aud->sink_name_len > 0u)
		memcpy(desc, aud->sink_name, aud->sink_name_len);

	for (r = 0; r < 5u; r++) {
		unsigned base = r * 4u;
		uint32_t word = 0;

		if (base < FRB_SINK_NAME_MAX)
			word |= (uint32_t)desc[base];
		if (base + 1u < FRB_SINK_NAME_MAX)
			word |= (uint32_t)desc[base + 1u] << 8;
		if (base + 2u < FRB_SINK_NAME_MAX)
			word |= (uint32_t)desc[base + 2u] << 16;
		if (base + 3u < FRB_SINK_NAME_MAX)
			word |= (uint32_t)desc[base + 3u] << 24;
		endpoint_wreg(adev, pin_off, (uint8_t)(RDN_AZ_PIN_SINK_INFO4 + r), word);
	}
}

static void write_dp_avi_infoframe(struct frb_device *adev, uint32_t afmt_off, const struct frb_mode *mode)
{
	uint8_t frame[14], vic = 0, aspect = 0, sum;
	uint32_t tmp;
	unsigned i;

	memset(frame, 0, sizeof(frame));
	if (mode) {
		if (mode->hdisplay == 1920u && mode->vdisplay == 1080u) {
			vic = 16u;
			aspect = 2u;
		} else if (mode->hdisplay == 1280u && mode->vdisplay == 720u) {
			vic = 4u;
			aspect = 2u;
		} else if (mode->hdisplay == 720u && mode->vdisplay == 480u) {
			vic = 2u;
			aspect = 1u;
		} else if (mode->hdisplay == 640u && mode->vdisplay == 480u) {
			vic = 1u;
			aspect = 1u;
		}
	}
	frame[1] = (1u << 4) | 2u;
	frame[2] = (uint8_t)(aspect << 4) | 8u;
	frame[4] = vic;
	sum = 0x82u + 2u + 13u;
	for (i = 1u; i < sizeof(frame); i++)
		sum = (uint8_t)(sum + frame[i]);
	frame[0] = (uint8_t)(0x100u - sum);

	WREG32(AR(adev)->afmt_avi_info0 + afmt_off,
	       (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
	       ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24));
	WREG32(AR(adev)->afmt_avi_info1 + afmt_off,
	       (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) |
	       ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 24));
	WREG32(AR(adev)->afmt_avi_info2 + afmt_off,
	       (uint32_t)frame[8] | ((uint32_t)frame[9] << 8) |
	       ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 24));
	WREG32(AR(adev)->afmt_avi_info3 + afmt_off,
	       (uint32_t)frame[12] | ((uint32_t)frame[13] << 8) | (2u << 24));

	tmp = RREG32(AR(adev)->hdmi_infoframe_control1 + afmt_off);
	tmp &= ~HDMI_AVI_INFO_LINE_MASK;
	WREG32(AR(adev)->hdmi_infoframe_control1 + afmt_off, tmp | HDMI_AVI_INFO_LINE(2));
}

/* rdn_dce8_afmt_dp_mode_set: DTO1, the DIG's AFMT framing, the pin's descriptors and sink info */
int frb_audio_dp_mode_set(struct frb_device *adev, struct frb_display *d, uint8_t crtc_id, int dig,
			  const struct frb_mode *mode, const uint8_t *edid, size_t edid_len)
{
	struct frb_afmt *afmt = &d->afmt;
	struct frb_edid_audio aud;
	uint32_t afmt_off, pin_off;
	uint32_t tmp, spk, lipsync;
	uint8_t dig_encoder;
	int pin_idx;

	if (dig < 0 || dig >= RDN_DCE8_AFMT_NUM_DIG)
		return -EINVAL;
	dig_encoder = (uint8_t)dig;

	if (!afmt->initialized)
		frb_audio_hw_init(adev, d);

	if (!frb_edid_extract_audio(edid, edid_len, &aud) || !aud.has_audio) {
		frb_audio_dp_enable(adev, d, dig_encoder, false);
		release_pin_for_dig(adev, afmt, dig_encoder);
		frb_info(adev, "audio: DP audio: DIG%u sink reports no CEA audio", (unsigned)dig_encoder);
		return 0;
	}

	if (afmt->active_dig >= 0 && afmt->active_dig != (int8_t)dig_encoder) {
		frb_audio_dp_enable(adev, d, (uint8_t)afmt->active_dig, false);
		release_pin_for_dig(adev, afmt, (uint8_t)afmt->active_dig);
	}

	pin_idx = select_pin_for_dig(adev, afmt, dig_encoder);
	pin_off = pin_offset((unsigned)pin_idx);
	afmt_off = dig_afmt_offsets[dig_encoder];

	/* Keep presence/ELDV gated while endpoint descriptors are rewritten. */
	pin_audio_enable(adev, pin_off, false);

	/* 1. DCCG Audio DTO1 (24 MHz reference over the DP reference clock). */
	afmt->dprefclk_khz = AR(adev)->dto1_module_khz(adev, d);
	WREG32(AR(adev)->dto_source, RDN_DCCG_AUDIO_DTO0_SOURCE_SEL(crtc_id) | RDN_DCCG_AUDIO_DTO_SEL);
	WREG32(AR(adev)->dto1_phase, 24000u);
	WREG32(AR(adev)->dto1_module, afmt->dprefclk_khz);

	/* DCE 10+: the AFMT block's audio clock on before its first register (no DCE8 step) */
	afmt_clock(adev, afmt_off, true);

	/* 2. Route selected codec pin to this DIG's AFMT and frame IEC-60958 packets. */
	WREG32(AR(adev)->afmt_audio_src_control + afmt_off, (uint32_t)(pin_idx & 7));

	tmp = RREG32(AR(adev)->afmt_audio_packet_control + afmt_off);
	tmp &= ~RDN_AFMT_AUDIO_SAMPLE_SEND;
	tmp |= RDN_AFMT_RESET_FIFO_WHEN_AUDIO_DIS | RDN_AFMT_60958_CS_UPDATE;
	WREG32(AR(adev)->afmt_audio_packet_control + afmt_off, tmp);

	/* DP timestamp auto-generation (never touch DP_SEC_AUD_N, as RadeonNI on DCE6/DCE8). */
	tmp = RREG32(AR(adev)->dp_sec_timestamp + afmt_off);
	tmp = (tmp & ~RDN_DP_SEC_TIMESTAMP_MODE_MASK) | RDN_DP_SEC_TIMESTAMP_AUTO;
	WREG32(AR(adev)->dp_sec_timestamp + afmt_off, tmp);

	WREG32(AR(adev)->hdmi_audio_packet_control + afmt_off, 0x00030010u);
	WREG32(AR(adev)->afmt_60958_0 + afmt_off, 1u << 20);
	WREG32(AR(adev)->afmt_60958_1 + afmt_off, 2u << 20);
	WREG32(AR(adev)->afmt_60958_2 + afmt_off, 0x00876543u);

	tmp = RREG32(AR(adev)->afmt_audio_packet_control2 + afmt_off);
	tmp = (tmp & ~0x0000ff00u) | 0x0000ff00u;
	WREG32(AR(adev)->afmt_audio_packet_control2 + afmt_off, tmp);

	write_dp_avi_infoframe(adev, afmt_off, mode);
	tmp = RREG32(AR(adev)->afmt_infoframe_control0 + afmt_off);
	WREG32(AR(adev)->afmt_infoframe_control0 + afmt_off, tmp | RDN_AFMT_AUDIO_INFO_UPDATE);

	/* 3. Program the codec pin's speaker allocation, SADs, lipsync and sink info. */
	spk = endpoint_rreg(adev, pin_off, RDN_AZ_PIN_CHANNEL_SPEAKER);
	spk &= ~(RDN_AZ_HDMI_CONNECTION | RDN_AZ_DP_CONNECTION | RDN_AZ_SPEAKER_ALLOCATION_MASK);
	spk |= RDN_AZ_DP_CONNECTION;
	spk |= aud.has_speaker_alloc ? (uint32_t)(aud.speaker_alloc & 0x7fu) : 0x05u;
	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_CHANNEL_SPEAKER, spk);

	write_pin_sad_regs(adev, pin_off, &aud);

	lipsync = aud.latency_present[0]
		? ((uint32_t)aud.video_latency[0] | ((uint32_t)aud.audio_latency[0] << 8))
		: 0u;
	endpoint_wreg(adev, pin_off, RDN_AZ_PIN_RESPONSE_LIPSYNC, lipsync);

	write_pin_sink_info(adev, pin_off, &aud);

	afmt->active_dig = (int8_t)dig_encoder;
	afmt->active_pin = (int8_t)pin_idx;
	afmt->has_audio = true;

	frb_info(adev, "audio: DP audio configured: DIG%u <- pin %d%s, DTO1 24000/%u kHz, %u SAD(s), spk 0x%02x, "
		 "sink \"%s\"", (unsigned)dig_encoder, pin_idx, adev->audio_pin >= 0 ? " (frbctl audio pin)" : "",
		 (unsigned)afmt->dprefclk_khz, (unsigned)aud.sad_count, (unsigned)(spk & 0x7fu),
		 aud.sink_name[0] ? aud.sink_name : "DP");
	return 0;
}

/* rdn_dce8_afmt_dp_enable: the DIG's DP secondary audio packets and the pin's presence */
void frb_audio_dp_enable(struct frb_device *adev, struct frb_display *d, int dig, bool enable)
{
	struct frb_afmt *afmt = &d->afmt;
	uint32_t afmt_off, pkt, sec;
	uint8_t dig_encoder;
	bool active;

	if (dig < 0 || dig >= RDN_DCE8_AFMT_NUM_DIG)
		return;
	dig_encoder = (uint8_t)dig;

	afmt_off = dig_afmt_offsets[dig_encoder];
	active = enable && afmt->has_audio &&
		 afmt->active_dig == (int8_t)dig_encoder &&
		 afmt->active_pin >= 0;

	pkt = RREG32(AR(adev)->afmt_audio_packet_control + afmt_off);
	sec = RREG32(AR(adev)->dp_sec_cntl + afmt_off);

	if (active) {
		pkt |= RDN_AFMT_AUDIO_SAMPLE_SEND;
		sec |= RDN_DP_SEC_AUDIO_MASK;
		WREG32(AR(adev)->afmt_audio_packet_control + afmt_off, pkt);
		WREG32(AR(adev)->dp_sec_cntl + afmt_off, sec);
		pin_audio_enable(adev, pin_offset((unsigned)afmt->active_pin), true);
		afmt->enabled = true;
	} else {
		if (afmt->active_dig == (int8_t)dig_encoder && afmt->active_pin >= 0)
			pin_audio_enable(adev, pin_offset((unsigned)afmt->active_pin), false);
		sec &= ~RDN_DP_SEC_AUDIO_MASK;
		pkt &= ~RDN_AFMT_AUDIO_SAMPLE_SEND;
		WREG32(AR(adev)->dp_sec_cntl + afmt_off, sec);
		WREG32(AR(adev)->afmt_audio_packet_control + afmt_off, pkt);
		if (afmt->active_dig == (int8_t)dig_encoder)
			afmt->enabled = false;
	}
}

/* rdn_dce8_afmt_fini: every DIG's DP audio packets and every pin's presence off (the card's DIGs only) */
void frb_audio_fini(struct frb_device *adev, struct frb_display *d)
{
	struct frb_afmt *afmt = &d->afmt;
	int i, ndig = RDN_DCE8_AFMT_NUM_DIG;

	if (!afmt->initialized)
		return;
	if (d->num_dig && (int)d->num_dig < ndig)
		ndig = (int)d->num_dig;

	for (i = 0; i < ndig; i++)
		frb_audio_dp_enable(adev, d, i, false);
	for (i = 0; i < RDN_DCE8_AFMT_NUM_PINS; i++) {
		pin_audio_enable(adev, pin_offset((unsigned)i), false);
		afmt->pins[i].dig_encoder = -1;
	}
	afmt->active_dig = -1;
	afmt->active_pin = -1;
	afmt->has_audio = false;
	afmt->enabled = false;
	afmt->initialized = false;
}

/*
 * frbctl audio: the stream's audio set up again on the running link, in
 * rdn_modeset's audio order without the video steps (dp_enable(false),
 * dp_mode_set, dp_enable(true)). For trying another pin without a mode set:
 * with repick the DIG lets its pin go first, so the pin is chosen afresh
 * (frbctl audio pin N, or RadeonNI's choice for pin auto).
 */
int frb_audio_rerun(struct frb_device *adev, struct frb_display *d, bool repick)
{
	struct frb_crtc *c = &d->crtc;
	struct frb_connector *conn = c->conn;
	int r;

	if (!conn || conn->dig_encoder < 0 || conn->dig_encoder >= RDN_DCE8_AFMT_NUM_DIG ||
	    frb_atombios_encoder_get_encoder_mode(conn) != ATOM_ENCODER_MODE_DP)
		return -ENODEV;
	frb_audio_dp_enable(adev, d, conn->dig_encoder, false);
	if (repick)
		release_pin_for_dig(adev, &d->afmt, (uint8_t)conn->dig_encoder);
	r = frb_audio_dp_mode_set(adev, d, c->crtc_id, conn->dig_encoder, &c->mode, conn->edid, conn->edid_len);
	if (r)
		return r;
	frb_audio_dp_enable(adev, d, conn->dig_encoder, true);
	return 0;
}

/* DCE 11.2's registers (Polaris): RadeonNI's DCE8 layout at these offsets (see the top of the file) */
const struct frb_audio_regs frb_audio_regs_dce11 = {
	.dto_source = RDN_DCCG_AUDIO_DTO_SOURCE,
	.dto1_phase = RDN_DCE8_DCCG_AUDIO_DTO1_PHASE,
	.dto1_module = RDN_DCE8_DCCG_AUDIO_DTO1_MODULE,
	.ep_index = RDN_AZ_F0_CODEC_ENDPOINT_INDEX,
	.ep_data = RDN_AZ_F0_CODEC_ENDPOINT_DATA,
	.pin_stride = 4,		/* vid.h AUDn_REGISTER_OFFSET */
	.num_pins = 7,			/* RadeonNI's DCE8 walk */
	.ep_write_en = 0,
	.num_dig = 7,
	.dig_off = { DIG0_REGISTER_OFFSET, DIG1_REGISTER_OFFSET, DIG2_REGISTER_OFFSET, DIG3_REGISTER_OFFSET,
		     DIG4_REGISTER_OFFSET, DIG5_REGISTER_OFFSET, DIG6_REGISTER_OFFSET },
	.hdmi_audio_packet_control = RDN_HDMI_AUDIO_PACKET_CONTROL,
	.afmt_audio_packet_control2 = RDN_AFMT_AUDIO_PACKET_CONTROL2,
	.afmt_60958_0 = RDN_AFMT_60958_0,
	.afmt_60958_1 = RDN_AFMT_60958_1,
	.afmt_60958_2 = RDN_AFMT_60958_2,
	.afmt_audio_packet_control = RDN_AFMT_AUDIO_PACKET_CONTROL,
	.afmt_infoframe_control0 = RDN_AFMT_INFOFRAME_CONTROL0,
	.afmt_audio_src_control = RDN_AFMT_AUDIO_SRC_CONTROL,
	.dp_sec_cntl = RDN_EVERGREEN_DP_SEC_CNTL,
	.dp_sec_timestamp = RDN_EVERGREEN_DP_SEC_TIMESTAMP,
	.afmt_avi_info0 = AFMT_AVI_INFO0,
	.afmt_avi_info1 = AFMT_AVI_INFO1,
	.afmt_avi_info2 = AFMT_AVI_INFO2,
	.afmt_avi_info3 = AFMT_AVI_INFO3,
	.hdmi_infoframe_control1 = HDMI_INFOFRAME_CONTROL1,
	.afmt_cntl = FRB_AFMT_CNTL,
	.afmt_audio_clock_en = FRB_AFMT_AUDIO_CLOCK_EN,
	.dto1_module_khz = frb_audio_dprefclk_khz,
};
