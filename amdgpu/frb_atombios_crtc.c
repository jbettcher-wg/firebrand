/*
 * atombios_crtc.c (Linux 6.16), the table calls DCE 11.2 uses: lock, enable,
 * blank, power gating, DTD timing, overscan, scaler, AdjustDisplayPll 1.3,
 * SetDCEClock 2.1, SetPixelClock 1.7 and prepare_pll; and DCE 6's and DCE 8's
 * (Linux 7.0.2): SetPixelClock 1.5/1.6 with the driver's dividers, the
 * spread, the display engine PLL (PPLL0 on SI, ATOM_EXT_PLL1 on CIK). A
 * CRTC with no pixel PLL (ATOM_PPLL_INVALID: DP on a DCE 8 board's external
 * clock) still gets SetPixelClock, the PLL id 0xff, and no spread, as in
 * amdgpu. Each parameter block
 * is a little-endian byte image (struct frb_ps) filled at the offsetof()
 * positions of the atombios.h structure the comment names; the interpreter
 * reads it as LE dwords and writes outputs back the same way.
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
 */
#include <stddef.h>
#include "frb_display.h"
#include "../shared/atom/atombios.h"

#define OFS(t, f)	((uint32_t)offsetof(t, f))

/* ENABLE_CRTC_PS_ALLOCATION: ucCRTC, ucEnable */
static void crtc_cmd(struct frb_device *adev, int index, int crtc, int state)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(ENABLE_CRTC_PARAMETERS, ucCRTC), crtc);
	frb_ps_u8(&ps, OFS(ENABLE_CRTC_PARAMETERS, ucEnable), state);
	frb_ps_exec(adev, index, &ps);
}

void frb_atombios_crtc_lock(struct frb_device *adev, struct frb_crtc *c, int lock)
{
	crtc_cmd(adev, GetIndexIntoMasterTable(COMMAND, UpdateCRTC_DoubleBufferRegisters), c->crtc_id, lock);
}

void frb_atombios_crtc_enable(struct frb_device *adev, struct frb_crtc *c, int state)
{
	crtc_cmd(adev, GetIndexIntoMasterTable(COMMAND, EnableCRTC), c->crtc_id, state);
}

/* BLANK_CRTC_PS_ALLOCATION: ucCRTC, ucBlanking, black colour 0 */
void frb_atombios_crtc_blank(struct frb_device *adev, struct frb_crtc *c, int state)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(BLANK_CRTC_PARAMETERS, ucCRTC), c->crtc_id);
	frb_ps_u8(&ps, OFS(BLANK_CRTC_PARAMETERS, ucBlanking), state);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, BlankCRTC), &ps);
}

/* ENABLE_DISP_POWER_GATING_PS_ALLOCATION: ucDispPipeId, ucEnable */
void frb_atombios_crtc_powergate(struct frb_device *adev, struct frb_crtc *c, int state)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(ENABLE_DISP_POWER_GATING_PARAMETERS_V2_1, ucDispPipeId), c->crtc_id);
	frb_ps_u8(&ps, OFS(ENABLE_DISP_POWER_GATING_PARAMETERS_V2_1, ucEnable), state);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, EnableDispPowerGating), &ps);
}

void frb_atombios_crtc_powergate_init(struct frb_device *adev)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(ENABLE_DISP_POWER_GATING_PARAMETERS_V2_1, ucEnable), ATOM_INIT);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, EnableDispPowerGating), &ps);
}

/* SET_CRTC_USING_DTD_TIMING_PARAMETERS (no borders) */
void frb_atombios_crtc_set_dtd_timing(struct frb_device *adev, struct frb_crtc *c, const struct frb_mode *m)
{
	struct frb_ps ps;
	uint16_t misc = 0;

	frb_ps_init(&ps);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usH_Size), m->hdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usH_Blanking_Time), m->htotal - m->hdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usV_Size), m->vdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usV_Blanking_Time), m->vtotal - m->vdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usH_SyncOffset), m->hsync_start - m->hdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usH_SyncWidth), m->hsync_end - m->hsync_start);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usV_SyncOffset), m->vsync_start - m->vdisplay);
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, usV_SyncWidth), m->vsync_end - m->vsync_start);
	if (m->flags & FRB_MODE_NVSYNC)
		misc |= ATOM_VSYNC_POLARITY;
	if (m->flags & FRB_MODE_NHSYNC)
		misc |= ATOM_HSYNC_POLARITY;
	frb_ps_le16(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, susModeMiscInfo), misc);
	frb_ps_u8(&ps, OFS(SET_CRTC_USING_DTD_TIMING_PARAMETERS, ucCRTC), c->crtc_id);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, SetCRTC_UsingDTDTiming), &ps);
}

/* SET_CRTC_OVERSCAN_PS_ALLOCATION, RMX_OFF: all borders 0 */
void frb_atombios_crtc_overscan_setup(struct frb_device *adev, struct frb_crtc *c)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(SET_CRTC_OVERSCAN_PARAMETERS, ucCRTC), c->crtc_id);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, SetCRTC_OverScan), &ps);
}

/* ENABLE_SCALER_PS_ALLOCATION, RMX_OFF: ATOM_SCALER_DISABLE */
void frb_atombios_crtc_scaler_setup(struct frb_device *adev, struct frb_crtc *c)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, OFS(ENABLE_SCALER_PARAMETERS, ucScaler), c->crtc_id);
	frb_ps_u8(&ps, OFS(ENABLE_SCALER_PARAMETERS, ucEnable), ATOM_SCALER_DISABLE);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, EnableScaler), &ps);
}

/* amdgpu_atombios_crtc_set_dce_clock, SET_DCE_CLOCK_PS_ALLOCATION_V2_1 */
uint32_t frb_atombios_crtc_set_dce_clock(struct frb_device *adev, uint32_t freq, uint8_t clk_type, uint8_t clk_src)
{
	int index = GetIndexIntoMasterTable(COMMAND, SetDCEClock);
	struct frb_ps ps;
	uint8_t frev, crev;
	uint32_t ret;

	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 2 || crev != 1) {
		frb_err(adev, "display: SetDCEClock %u.%u not handled", frev, crev);
		return 0;
	}
	frb_ps_init(&ps);
	frb_ps_le32(&ps, OFS(SET_DCE_CLOCK_PARAMETERS_V2_1, ulDCEClkFreq), freq);	/* 10 kHz */
	frb_ps_u8(&ps, OFS(SET_DCE_CLOCK_PARAMETERS_V2_1, ucDCEClkType), clk_type);
	frb_ps_u8(&ps, OFS(SET_DCE_CLOCK_PARAMETERS_V2_1, ucDCEClkSrc), clk_src);
	frb_ps_exec(adev, index, &ps);
	ret = frb_ps_get_le32(&ps, OFS(SET_DCE_CLOCK_PARAMETERS_V2_1, ulDCEClkFreq)) * 10;
	frb_info(adev, "display: SetDCEClock type %u: asked %u kHz, got %u kHz", clk_type, freq * 10, ret);
	return ret;
}

/*
 * amdgpu_atombios_crtc_program_pll, the 1.7 arm only (Polaris): the VBIOS
 * computes the dividers and the spread itself ("SetPixelClock calculates
 * the plls and ss values now", dce_v11_0_crtc_mode_set).
 */
void frb_atombios_crtc_program_pll(struct frb_device *adev, int crtc_id, int pll_id, uint32_t encoder_mode,
				   uint32_t encoder_id, uint32_t clock, int bpc, int duallink)
{
	int index = GetIndexIntoMasterTable(COMMAND, SetPixelClock);
	struct frb_ps ps;
	uint8_t frev, crev, misc = 0;

	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 1 || crev != 7) {
		frb_err(adev, "display: SetPixelClock %u.%u not handled", frev, crev);
		return;
	}
	frb_ps_init(&ps);
	frb_ps_le32(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ulPixelClock), clock * 10);	/* 100 Hz units */
	/*
	 * amdgpu: DVI over 165 MHz is dual link. Here an HDMI sink in DVI
	 * mode runs single link to 340 MHz (amdgpu drives it in HDMI mode
	 * there), so the flag follows amdgpu_dig_monitor_is_duallink.
	 */
	if (encoder_mode == ATOM_ENCODER_MODE_DVI && duallink)
		misc |= PIXEL_CLOCK_V7_MISC_DVI_DUALLINK_EN;
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucMiscInfo), misc);
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucCRTC), crtc_id);
	(void)bpc;	/* deep colour only for HDMI */
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucDeepColorRatio), PIXEL_CLOCK_V7_DEEPCOLOR_RATIO_DIS);
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucTransmitterID), encoder_id);
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucEncoderMode), encoder_mode);
	frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V7, ucPpll), pll_id);
	frb_info(adev, "display: SetPixelClock 1.7: %u kHz, crtc %d, pll %d, transmitter 0x%02x, mode %u",
		 clock, crtc_id, pll_id, encoder_id, encoder_mode);
	frb_ps_exec(adev, index, &ps);
}

/* amdgpu_atombios_crtc_adjust_pll, the 1.3 arm */
static uint32_t adjust_pll(struct frb_device *adev, struct frb_crtc *c)
{
	struct frb_connector *conn = c->conn;
	int index = GetIndexIntoMasterTable(COMMAND, AdjustDisplayPll);
	int encoder_mode = frb_atombios_encoder_get_encoder_mode(conn);
	uint32_t clock = c->mode.clock, adjusted = c->mode.clock;
	struct frb_ps ps;
	uint8_t frev, crev, config = 0;

	if (frb_cmd_rev(adev, index, &frev, &crev))
		return adjusted;
	if (frev != 1 || crev != 3) {
		frb_err(adev, "display: AdjustDisplayPll %u.%u not handled", frev, crev);
		return adjusted;
	}
	c->pll_flags = FRB_PLL_USE_FRAC_FB_DIV;
	c->pll_reference_div = 0;
	c->pll_post_div = 0;
	frb_ps_init(&ps);
	if (c->ss_enabled && c->ss.percentage)
		config |= DISPPLL_CONFIG_SS_ENABLE;
	if (encoder_mode == ATOM_ENCODER_MODE_DP || encoder_mode == ATOM_ENCODER_MODE_DP_MST) {
		config |= DISPPLL_CONFIG_COHERENT_MODE;
		clock = conn->dp_clock;		/* 16200 or 27000 */
	} else if (conn->devices & ATOM_DEVICE_DFP_SUPPORT) {
		/* dig->coherent_mode is amdgpu's default (amdgpu_atombios_encoder_get_dig_info) */
		config |= DISPPLL_CONFIG_COHERENT_MODE;
		if (frb_dig_monitor_is_duallink(conn, c->mode.clock))
			config |= DISPPLL_CONFIG_DUAL_LINK;
	}
	frb_ps_le16(&ps, OFS(ADJUST_DISPLAY_PLL_INPUT_PARAMETERS_V3, usPixelClock), clock / 10);
	frb_ps_u8(&ps, OFS(ADJUST_DISPLAY_PLL_INPUT_PARAMETERS_V3, ucTransmitterID), conn->encoder_id);
	frb_ps_u8(&ps, OFS(ADJUST_DISPLAY_PLL_INPUT_PARAMETERS_V3, ucEncodeMode), encoder_mode);
	frb_ps_u8(&ps, OFS(ADJUST_DISPLAY_PLL_INPUT_PARAMETERS_V3, ucDispPllConfig), config);
	frb_ps_u8(&ps, OFS(ADJUST_DISPLAY_PLL_INPUT_PARAMETERS_V3, ucExtTransmitterID), 0);
	frb_ps_exec(adev, index, &ps);
	adjusted = frb_ps_get_le32(&ps, OFS(ADJUST_DISPLAY_PLL_OUTPUT_PARAMETERS_V3, ulDispPllFreq)) * 10;
	if (frb_ps_get_u8(&ps, OFS(ADJUST_DISPLAY_PLL_OUTPUT_PARAMETERS_V3, ucRefDiv))) {
		c->pll_flags |= FRB_PLL_USE_FRAC_FB_DIV | FRB_PLL_USE_REF_DIV;
		c->pll_reference_div = frb_ps_get_u8(&ps, OFS(ADJUST_DISPLAY_PLL_OUTPUT_PARAMETERS_V3, ucRefDiv));
	}
	if (frb_ps_get_u8(&ps, OFS(ADJUST_DISPLAY_PLL_OUTPUT_PARAMETERS_V3, ucPostDiv))) {
		c->pll_flags |= FRB_PLL_USE_FRAC_FB_DIV | FRB_PLL_USE_POST_DIV;
		c->pll_post_div = frb_ps_get_u8(&ps, OFS(ADJUST_DISPLAY_PLL_OUTPUT_PARAMETERS_V3, ucPostDiv));
	}
	return adjusted;
}

/* amdgpu_atombios_crtc_prepare_pll: the DP, DVI and HDMI arms (bpc 8: amdgpu.deep_color=0) */
int frb_atombios_crtc_prepare_pll(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c)
{
	int encoder_mode = frb_atombios_encoder_get_encoder_mode(c->conn);

	(void)d;
	c->bpc = 8;
	c->ss_enabled = 0;
	switch (encoder_mode) {
	case ATOM_ENCODER_MODE_DP_MST:
	case ATOM_ENCODER_MODE_DP:
		c->ss_enabled = frb_atombios_get_asic_ss_info(adev, &c->ss, ASIC_INTERNAL_SS_ON_DP,
							      c->conn->dp_clock / 10);
		break;
	case ATOM_ENCODER_MODE_DVI:
		c->ss_enabled = frb_atombios_get_asic_ss_info(adev, &c->ss, ASIC_INTERNAL_SS_ON_TMDS,
							      c->mode.clock / 10);
		break;
	case ATOM_ENCODER_MODE_HDMI:
		c->ss_enabled = frb_atombios_get_asic_ss_info(adev, &c->ss, ASIC_INTERNAL_SS_ON_HDMI,
							      c->mode.clock / 10);
		break;
	default:
		break;
	}
	c->adjusted_clock = adjust_pll(adev, c);
	frb_info(adev, "display: prepare_pll: %u kHz -> adjusted %u kHz, spread %s (%u/%u)",
		 c->mode.clock, c->adjusted_clock, c->ss_enabled ? "on" : "off",
		 c->ss.percentage, c->ss.percentage_divider);
	return 0;
}

/* amdgpu_atombios_crtc_program_ss, the v3 table (DCE5+) */
static void program_ss(struct frb_device *adev, struct frb_display *d, int enable, int pll_id, int crtc_id,
		       struct frb_atom_ss *ss)
{
	int index = GetIndexIntoMasterTable(COMMAND, EnableSpreadSpectrumOnPPLL);
	struct frb_ps ps;
	uint8_t type;

	(void)d;
	(void)crtc_id;
	if (enable) {
		/*
		 * Don't mess with SS if percentage is 0 or external ss. SS is
		 * already disabled previously, and disabling it again can cause
		 * display problems if the pll is already programmed.
		 */
		if (ss->percentage == 0)
			return;
		if (ss->type & ATOM_EXTERNAL_SS_MASK)
			return;
	}
	/* (one CRTC here: no other one can share the PLL) */
	type = ss->type & ATOM_SS_CENTRE_SPREAD_MODE_MASK;
	switch (pll_id) {
	case ATOM_PPLL1:
		type |= ATOM_PPLL_SS_TYPE_V3_P1PLL;
		break;
	case ATOM_PPLL2:
		type |= ATOM_PPLL_SS_TYPE_V3_P2PLL;
		break;
	case ATOM_DCPLL:
		type |= ATOM_PPLL_SS_TYPE_V3_DCPLL;
		break;
	case ATOM_PPLL_INVALID:
		return;
	}
	frb_ps_init(&ps);
	frb_ps_le16(&ps, OFS(ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3, usSpreadSpectrumAmountFrac), 0);
	frb_ps_u8(&ps, OFS(ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3, ucSpreadSpectrumType), type);
	frb_ps_le16(&ps, OFS(ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3, usSpreadSpectrumAmount), ss->amount);
	frb_ps_le16(&ps, OFS(ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3, usSpreadSpectrumStep), ss->step);
	frb_ps_u8(&ps, OFS(ENABLE_SPREAD_SPECTRUM_ON_PPLL_V3, ucEnable), enable);
	frb_ps_exec(adev, index, &ps);
}

/* is_pixel_clock_source_from_pll */
static int is_pixel_clock_source_from_pll(uint32_t encoder_mode, int pll_id)
{
	if (encoder_mode == ATOM_ENCODER_MODE_DP || encoder_mode == ATOM_ENCODER_MODE_DP_MST)
		return pll_id < ATOM_EXT_PLL1;
	return 1;
}

/* amdgpu_atombios_crtc_program_pll, the 1.5 and 1.6 arms (no HDMI deep colour: bpc 8) */
static void program_pll_div(struct frb_device *adev, int crtc_id, int pll_id, uint32_t encoder_mode,
			    uint32_t encoder_id, uint32_t clock, uint32_t ref_div, uint32_t fb_div,
			    uint32_t frac_fb_div, uint32_t post_div, int ss_enabled, const struct frb_atom_ss *ss)
{
	int index = GetIndexIntoMasterTable(COMMAND, SetPixelClock);
	struct frb_ps ps;
	uint8_t frev, crev, misc = 0;

	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 1 || (crev != 5 && crev != 6)) {
		frb_err(adev, "display: SetPixelClock %u.%u not handled", frev, crev);
		return;
	}
	frb_ps_init(&ps);
	if (crev == 5) {
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucCRTC), crtc_id);
		frb_ps_le16(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, usPixelClock), clock / 10);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucRefDiv), ref_div);
		frb_ps_le16(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, usFbDiv), fb_div);
		frb_ps_le32(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ulFbDivDecFrac), frac_fb_div * 100000);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucPostDiv), post_div);
		if ((ss_enabled && (ss->type & ATOM_EXTERNAL_SS_MASK)) && (pll_id < ATOM_EXT_PLL1))
			misc |= PIXEL_CLOCK_V5_MISC_REF_DIV_SRC;
		if (encoder_mode == ATOM_ENCODER_MODE_HDMI)
			misc |= PIXEL_CLOCK_V5_MISC_HDMI_24BPP;
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucMiscInfo), misc);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucTransmitterID), encoder_id);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucEncoderMode), encoder_mode);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucPpll), pll_id);
	} else {
		/* ulCrtcPclkFreq: ulPixelClock:24 (10 kHz), ucCRTC:8 in the top byte (little-endian) */
		frb_ps_le32(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ulDispEngClkFreq), ((uint32_t)crtc_id << 24) | (clock / 10));
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucRefDiv), ref_div);
		frb_ps_le16(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, usFbDiv), fb_div);
		frb_ps_le32(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ulFbDivDecFrac), frac_fb_div * 100000);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucPostDiv), post_div);
		if ((ss_enabled && (ss->type & ATOM_EXTERNAL_SS_MASK)) && (pll_id < ATOM_EXT_PLL1) &&
		    !is_pixel_clock_source_from_pll(encoder_mode, pll_id))
			misc |= PIXEL_CLOCK_V6_MISC_REF_DIV_SRC;
		if (encoder_mode == ATOM_ENCODER_MODE_HDMI)
			misc |= PIXEL_CLOCK_V6_MISC_HDMI_24BPP;
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucMiscInfo), misc);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucTransmitterID), encoder_id);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucEncoderMode), encoder_mode);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucPpll), pll_id);
	}
	frb_info(adev, "display: SetPixelClock 1.%u: %u kHz, crtc %d, pll %d, fb %u.%u ref %u post %u, "
		 "transmitter 0x%02x, mode %u", crev, clock, crtc_id, pll_id, fb_div, frac_fb_div, ref_div,
		 post_div, encoder_id, encoder_mode);
	frb_ps_exec(adev, index, &ps);
}

/* amdgpu_atombios_crtc_set_pll */
void frb_atombios_crtc_set_pll(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c)
{
	struct frb_connector *conn = c->conn;
	uint32_t pll_clock = c->mode.clock, clock = c->mode.clock;
	uint32_t ref_div = 0, fb_div = 0, frac_fb_div = 0, post_div = 0;
	int encoder_mode = frb_atombios_encoder_get_encoder_mode(conn);
	struct frb_pll *pll;
	uint8_t frev, crev;

	if (frb_cmd_rev(adev, GetIndexIntoMasterTable(COMMAND, SetPixelClock), &frev, &crev) == 0 &&
	    frev == 1 && crev == 7) {
		/* the VBIOS works out the dividers and the spread itself */
		frb_atombios_crtc_program_pll(adev, c->crtc_id, c->pll_id, encoder_mode, conn->encoder_id,
					      c->mode.clock, c->bpc, frb_dig_monitor_is_duallink(conn, c->mode.clock));
		return;
	}

	switch (c->pll_id) {
	case ATOM_PPLL1:
		pll = &d->ppll[0];
		break;
	case ATOM_PPLL2:
		pll = &d->ppll[1];
		break;
	case ATOM_PPLL0:
	case ATOM_PPLL_INVALID:
	default:
		pll = &d->ppll[2];
		break;
	}

	/* update pll params */
	pll->flags = c->pll_flags;
	pll->reference_div = c->pll_reference_div;
	pll->post_div = c->pll_post_div;

	frb_pll_compute(adev, pll, c->adjusted_clock, &pll_clock, &fb_div, &frac_fb_div, &ref_div, &post_div);

	program_ss(adev, d, ATOM_DISABLE, c->pll_id, c->crtc_id, &c->ss);

	program_pll_div(adev, c->crtc_id, c->pll_id, encoder_mode, conn->encoder_id, clock,
			ref_div, fb_div, frac_fb_div, post_div, c->ss_enabled, &c->ss);

	if (c->ss_enabled) {
		/* calculate ss amount and step size */
		uint32_t step_size;
		uint32_t amount = (((fb_div * 10) + frac_fb_div) * (uint32_t)c->ss.percentage) /
			(100 * (uint32_t)c->ss.percentage_divider);

		c->ss.amount = (amount / 10) & ATOM_PPLL_SS_AMOUNT_V2_FBDIV_MASK;
		c->ss.amount |= ((amount - (amount / 10)) << ATOM_PPLL_SS_AMOUNT_V2_NFRAC_SHIFT) &
			ATOM_PPLL_SS_AMOUNT_V2_NFRAC_MASK;
		if (c->ss.type & ATOM_PPLL_SS_TYPE_V2_CENTRE_SPREAD)
			step_size = (4 * amount * ref_div * ((uint32_t)c->ss.rate * 2048)) /
				(125 * 25 * pll->reference_freq / 100);
		else
			step_size = (2 * amount * ref_div * ((uint32_t)c->ss.rate * 2048)) /
				(125 * 25 * pll->reference_freq / 100);
		c->ss.step = step_size;

		program_ss(adev, d, ATOM_ENABLE, c->pll_id, c->crtc_id, &c->ss);
	}
}

/* amdgpu_atombios_crtc_set_disp_eng_pll (dispclk in 10 kHz) */
void frb_atombios_crtc_set_disp_eng_pll(struct frb_device *adev, uint32_t dispclk)
{
	int index = GetIndexIntoMasterTable(COMMAND, SetPixelClock);
	struct frb_ps ps;
	uint8_t frev, crev;

	if (frb_cmd_rev(adev, index, &frev, &crev))
		return;
	frb_ps_init(&ps);
	if (frev == 1 && crev == 5) {
		/* if the default dcpll clock is specified, SetPixelClock provides the dividers */
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucCRTC), ATOM_CRTC_INVALID);
		frb_ps_le16(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, usPixelClock), dispclk);
		frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V5, ucPpll), ATOM_DCPLL);
	} else if (frev == 1 && crev == 6) {
		/* if the default dcpll clock is specified, SetPixelClock provides the dividers */
		frb_ps_le32(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ulDispEngClkFreq), dispclk);
		switch (adev->asic_type) {
		case CHIP_TAHITI:
		case CHIP_PITCAIRN:
		case CHIP_VERDE:
		case CHIP_OLAND:
			frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucPpll), ATOM_PPLL0);
			break;
		default:	/* CIK (dce_v8_0): Bonaire, Hawaii */
			frb_ps_u8(&ps, OFS(PIXEL_CLOCK_PARAMETERS_V6, ucPpll), ATOM_EXT_PLL1);
			break;
		}
	} else {
		frb_err(adev, "display: SetPixelClock %u.%u for the display engine clock not handled", frev, crev);
		return;
	}
	frb_info(adev, "display: display engine clock %u kHz (SetPixelClock %u.%u)", dispclk * 10, frev, crev);
	frb_ps_exec(adev, index, &ps);
}

/* dce_v6_0_crtc_disable's amdgpu_atombios_crtc_program_pll(crtc, id, pll, 0, 0, ATOM_DISABLE, 0, ...) */
void frb_atombios_crtc_pll_off(struct frb_device *adev, int crtc_id, int pll_id)
{
	struct frb_atom_ss ss;

	memset(&ss, 0, sizeof(ss));
	program_pll_div(adev, crtc_id, pll_id, 0, 0, ATOM_DISABLE, 0, 0, 0, 0, 0, &ss);
}
