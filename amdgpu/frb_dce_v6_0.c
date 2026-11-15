/*
 * dce_v6_0.c (Linux 7.0.2, amdgpu's legacy display for Southern Islands),
 * the parts a single CRTC driving DisplayPort or DVI/HDMI (TMDS) needs, on
 * the same frame as frb_dce_v11_0.c: hw_init, HPD, the scanout surface
 * (crtc_do_set_base), the LUT, FMT, line buffer and watermarks; and the
 * modeset in the order drm_crtc_helper_set_mode runs dce_v6_0's helpers:
 *
 *   encoder mode_fixup (DP link config) -> crtc mode_fixup (prepare_pll,
 *   pick_pll) -> encoder prepare (pick DIG, crtc source, FMT) -> crtc
 *   prepare -> crtc mode_set (set_pll: amdgpu_pll_compute and SetPixelClock
 *   1.6, DTD timing, set_base, overscan, scaler, cursor) -> encoder mode_set
 *   (transmitter off, interleave) -> crtc commit -> encoder commit
 *   (transmitter, link training, video on).
 *
 * What DCE 6 does differently from DCE 11 (beyond register offsets):
 * - hw_init has no golden registers, no display power gating init and no
 *   SetDCEClock: the display engine clock is set with SetPixelClock 1.6 on
 *   PPLL0 (amdgpu_atombios_crtc_set_disp_eng_pll).
 * - The pixel PLL dividers are the driver's (amdgpu_pll_compute), with the
 *   spread programmed around them; DP takes PPLL0 when the board has no DP
 *   external clock (Verde has none), TMDS a free PPLL2/PPLL1.
 * - The line buffer is split per CRTC pair (DC_LB_MEMORY_SPLIT) and the
 *   watermarks select through DPG_PIPE_ARBITRATION_CONTROL3, with the
 *   priority marks.
 * - crtc_dpms has no VGA toggling around the blank.
 * - The analog half of DVI-I (DAC1) is not ported: its path is skipped.
 * - DP audio is frb_dce_audio.c (RadeonNI's rdn_dce8_afmt.c, the DCE6 arm
 *   the LG played from on this card) on DCE 6's registers
 *   (frb_audio_regs_dce6 below): six codec pins 6 dwords apart with
 *   AZ_ENDPOINT_REG_WRITE_EN in the index on writes, DCE 6's DTO1 pair, no
 *   AFMT_CNTL (the AFMT clock is ungated) and no DC_MEM_GLOBAL_PWR_REQ.
 *   DTO1's module is the display engine clock, as dce_v6_0_afmt_setmode
 *   passes it (default_dispclk * 10).
 *
 * The scanout surface is linear 8/16/32 bpp in VRAM inside the CPU window;
 * the host writes pixels as big-endian words, so GRPH_SWAP_CNTL takes
 * ENDIAN_8IN16/8IN32: crtc_do_set_base's own __BIG_ENDIAN arms.
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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
#include "frb_pm.h"
#include "../shared/atom/atombios.h"
#include "include/sid.h"
#include "include/si_enums.h"
#include "include/asic_reg/dce/dce_6_0_d.h"
#include "include/asic_reg/dce/dce_6_0_sh_mask.h"
#include "include/asic_reg/gmc/gmc_6_0_d.h"
#include "include/asic_reg/gmc/gmc_6_0_sh_mask.h"

static const uint32_t crtc_offsets[6] = {
	CRTC0_REGISTER_OFFSET,
	CRTC1_REGISTER_OFFSET,
	CRTC2_REGISTER_OFFSET,
	CRTC3_REGISTER_OFFSET,
	CRTC4_REGISTER_OFFSET,
	CRTC5_REGISTER_OFFSET
};

static const uint32_t hpd_offsets[] = {
	HPD0_REGISTER_OFFSET,
	HPD1_REGISTER_OFFSET,
	HPD2_REGISTER_OFFSET,
	HPD3_REGISTER_OFFSET,
	HPD4_REGISTER_OFFSET,
	HPD5_REGISTER_OFFSET
};

static const uint32_t vga_control_regs[6] = {
	mmD1VGA_CONTROL,
	mmD2VGA_CONTROL,
	mmD3VGA_CONTROL,
	mmD4VGA_CONTROL,
	mmD5VGA_CONTROL,
	mmD6VGA_CONTROL,
};

/* dce_v6_0_hpd_sense */
static int dce_v6_0_hpd_sense(struct frb_device *adev, int hpd)
{
	struct frb_display *d = adev->display;

	if (hpd < 0 || !d || (uint32_t)hpd >= d->num_hpd)
		return 0;
	return (RREG32(mmDC_HPD1_INT_STATUS + hpd_offsets[hpd]) & DC_HPD1_INT_STATUS__DC_HPD1_SENSE_MASK) != 0;
}

/* DC_GPIO_HPD_Y's bit for each line (the pins dce_v6_0_hpd_get_gpio_reg's DC_GPIO_HPD_A names) */
static int dce_v6_0_hpd_pad(struct frb_device *adev, int hpd)
{
	static const uint32_t y[6] = { 0x1, 0x100, 0x10000, 0x1000000, 0x4000000, 0x10000000 };

	if (hpd < 0 || hpd >= 6)
		return -1;
	return (RREG32(mmDC_GPIO_HPD_Y) & y[hpd]) != 0;
}

/* dce_v6_0_set_vga_render_state */
static void set_vga_render_state(struct frb_device *adev, int render)
{
	if (!render)
		WREG32(mmVGA_RENDER_CONTROL,
		       RREG32(mmVGA_RENDER_CONTROL) & ~VGA_RENDER_CONTROL__VGA_VSTATUS_CNTL_MASK);
}

/* dce_v6_0_hpd_init (+ int_ack, set_polarity); the interrupt itself stays off (polling) */
static void hpd_init(struct frb_device *adev, struct frb_display *d)
{
	uint32_t tmp;
	int i;

	for (i = 0; i < d->num_connectors; i++) {
		int hpd = d->conn[i].hpd;

		if (hpd < 0 || hpd >= (int)d->num_hpd)
			continue;
		tmp = RREG32(mmDC_HPD1_CONTROL + hpd_offsets[hpd]);
		tmp |= DC_HPD1_CONTROL__DC_HPD1_EN_MASK;
		WREG32(mmDC_HPD1_CONTROL + hpd_offsets[hpd], tmp);
		/* dce_v6_0_hpd_int_ack */
		tmp = RREG32(mmDC_HPD1_INT_CONTROL + hpd_offsets[hpd]);
		tmp |= DC_HPD1_INT_CONTROL__DC_HPD1_INT_ACK_MASK;
		WREG32(mmDC_HPD1_INT_CONTROL + hpd_offsets[hpd], tmp);
		/* dce_v6_0_hpd_set_polarity */
		tmp = RREG32(mmDC_HPD1_INT_CONTROL + hpd_offsets[hpd]);
		if (dce_v6_0_hpd_sense(adev, hpd))
			tmp &= ~DC_HPD1_INT_CONTROL__DC_HPD1_INT_POLARITY_MASK;
		else
			tmp |= DC_HPD1_INT_CONTROL__DC_HPD1_INT_POLARITY_MASK;
		WREG32(mmDC_HPD1_INT_CONTROL + hpd_offsets[hpd], tmp);
	}
}

/* dce_v6_0_hpd_fini */
static void hpd_fini(struct frb_device *adev, struct frb_display *d)
{
	uint32_t tmp;
	int i;

	for (i = 0; i < d->num_connectors; i++) {
		int hpd = d->conn[i].hpd;

		if (hpd < 0 || hpd >= (int)d->num_hpd)
			continue;
		tmp = RREG32(mmDC_HPD1_CONTROL + hpd_offsets[hpd]);
		tmp &= ~DC_HPD1_CONTROL__DC_HPD1_EN_MASK;
		WREG32(mmDC_HPD1_CONTROL + hpd_offsets[hpd], tmp);
	}
}

/* a UNIPHY path: the encoders this port drives (the DAC is not ported) */
static int conn_is_uniphy(const struct frb_connector *c)
{
	switch (c->encoder_id) {
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:
		return 1;
	default:
		return 0;
	}
}

/* dce_v6_0_hw_init (+ early/sw_init's counts and the object table) */
static int dce_v6_0_hw_init(struct frb_device *adev, struct frb_display *d)
{
	const uint8_t *bios = adev->bios;
	uint16_t off;
	int r;

	memset(d, 0, sizeof(*d));
	/* dce_v6_0_early_init: Tahiti/Pitcairn/Verde 6 CRTCs, 6 HPD, 6 DIG (Oland 2) */
	switch (adev->asic_type) {
	case CHIP_TAHITI:
	case CHIP_PITCAIRN:
	case CHIP_VERDE:
		d->num_crtc = 6;
		d->num_hpd = 6;
		d->num_dig = 6;
		break;
	case CHIP_OLAND:
		d->num_crtc = 2;
		d->num_hpd = 2;
		d->num_dig = 2;
		break;
	default:
		return -EINVAL;
	}
	d->crtc.crtc_id = 0;
	d->crtc.crtc_offset = crtc_offsets[0];
	d->crtc.pll_id = ATOM_PPLL_INVALID;
	for (r = 0; r < 256; r++)
		d->crtc.gamma[0][r] = d->crtc.gamma[1][r] = d->crtc.gamma[2][r] = (uint16_t)(r << 8);
	/* amdgpu_atombios_get_clock_info: default_dispclk, dp_extclk, the pixel PLLs */
	if (frb_atom_parse_data_header(adev->atom, GetIndexIntoMasterTable(DATA, FirmwareInfo), NULL, NULL, NULL, &off)) {
		d->default_dispclk = frb_get_le32(bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultDispEngineClkFreq));
		d->dp_extclk = frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usUniphyDPModeExtClkFreq));
	}
	if (d->default_dispclk < 53900)
		d->default_dispclk = 60000;
	else if (d->default_dispclk <= 60000)
		d->default_dispclk = 62500;
	r = frb_atombios_get_ppll_info(adev, d);
	if (r)
		return r;
	d->current_sclk = adev->default_sclk;
	d->current_mclk = adev->default_mclk;
	d->pm_cfg.crtc_index = d->num_crtc;
	d->pm_cfg.min_vblank_time = 0xffffffff;

	r = frb_atombios_get_connector_info(adev, d);
	if (r)
		return r;

	/* gfx_v6_0_constants_init's address configuration, which amdgpu has done by now (frb_si.c) */
	r = frb_si_gfx_addr_config(adev);
	if (r)
		return r;

	/* disable vga render */
	set_vga_render_state(adev, 0);
	/* init dig PHYs, disp eng pll */
	frb_atombios_encoder_init_dig(adev, d);
	frb_atombios_crtc_set_disp_eng_pll(adev, d->default_dispclk);
	/* initialize hpd */
	hpd_init(adev, d);
	/* dce_v6_0_hw_init's audio_enable(pin, false) for every pin: RadeonNI's init (FirebrandAudio) */
	if (adev->want_audio)
		frb_audio_hw_init(adev, d);
	/* not carried: pageflip_interrupt_init (no IH ring; flips and vblank are polled) */
	frb_info(adev, "display: DCE 6 up: %u CRTCs, %u HPD, display engine clock %u kHz, DP external clock %u kHz",
		 d->num_crtc, d->num_hpd, d->default_dispclk * 10, d->dp_extclk * 10);
	d->ready = 1;
	return 0;
}

/*
 * amdgpu_connector_dp_detect / dvi_detect for every UNIPHY connector
 * (frb_dce_v11_0.c's, on DCE 6's HPD block). Returns the first connected
 * connector, or -1.
 */
static int dce_v6_0_detect(struct frb_device *adev, struct frb_display *d)
{
	int i, found = -1;

	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];
		int connected = 0, sense;
		uint8_t was = c->detect_logged;

		c->edid_len = 0;
		c->sink_tmds = 0;
		c->is_hdmi = 0;
		if (!conn_is_uniphy(c)) {
			c->detect_logged = 7;
			if (c->detect_logged != was)
				frb_info(adev, "display: connector %d (encoder 0x%02x): not a UNIPHY path (analog "
					 "is not ported); skipped", i, c->encoder_id);
			continue;
		}
		if (frb_connector_is_tmds(c)) {
			sense = dce_v6_0_hpd_sense(adev, c->hpd);
			if (frb_ddc_probe(adev, c) && !frb_ddc_read_edid(adev, c) && c->edid_len >= 128 &&
			    (c->edid[20] & 0x80)) {
				c->sink_tmds = 1;
				c->is_hdmi = frb_edid_is_hdmi(c->edid, c->edid_len);
				connected = 1;
			} else if (adev->force_tmds) {
				/* the test aid: drive the link blind with DMT modes, as RadeonNI's FakeEDID did */
				c->edid_len = 0;
				c->sink_tmds = 1;
				connected = 1;
				if (c->detect_logged != 8)
					frb_info(adev, "display: connector %d (hpd %d): no sink answers; taken as a DVI sink "
						 "with no EDID (FirebrandForceTMDS)", i, c->hpd);
				c->detect_logged = 8;
				if (found < 0)
					found = i;
				frb_atombios_encoder_set_bios_scratch_regs(adev, c, connected);
				continue;
			}
			frb_atombios_encoder_set_bios_scratch_regs(adev, c, connected);
			if (!connected) {
				c->detect_logged = 1 + (sense ? 1 : 0);
				if (c->detect_logged != was)
					frb_info(adev, "display: connector %d (%s, hpd %d%s): nothing", i,
						 c->connector_type == CONNECTOR_OBJECT_ID_HDMI_TYPE_A ? "HDMI" : "DVI",
						 c->hpd, sense ? ", line up" : "");
				c->edid_len = 0;
				continue;
			}
			c->detect_logged = 3;
			frb_info(adev, "display: connector %d (%s, hpd %d): %s sink over DDC; EDID %u bytes", i,
				 c->connector_type == CONNECTOR_OBJECT_ID_HDMI_TYPE_A ? "HDMI" : "DVI", c->hpd,
				 c->is_hdmi ? "HDMI" : "DVI", c->edid_len);
			if (found < 0)
				found = i;
			continue;
		}
		if (c->connector_type != CONNECTOR_OBJECT_ID_DISPLAYPORT) {
			frb_info(adev, "display: connector %d: type %u not handled yet", i, c->connector_type);
			continue;
		}
		c->dp_sink_type = frb_dp_get_sinktype(adev, c);
		sense = dce_v6_0_hpd_sense(adev, c->hpd);
		if (sense) {
			connected = 1;
			if (c->dp_sink_type == CONNECTOR_OBJECT_ID_DISPLAYPORT)
				frb_dp_get_dpcd(adev, c);
		} else if (c->dp_sink_type == CONNECTOR_OBJECT_ID_DISPLAYPORT) {
			if (!frb_dp_get_dpcd(adev, c))
				connected = 1;
		}
		frb_atombios_encoder_set_bios_scratch_regs(adev, c, connected);
		if (!connected) {
			c->detect_logged = 4;
			if (c->detect_logged != was)
				frb_info(adev, "display: connector %d (hpd %d): nothing (sink type 0x%x, pad %d)", i, c->hpd,
					 c->dp_sink_type, dce_v6_0_hpd_pad(adev, c->hpd));
			continue;
		}
		if (c->dp_sink_type != CONNECTOR_OBJECT_ID_DISPLAYPORT || !c->dpcd[0]) {
			c->detect_logged = 5;
			if (c->detect_logged != was)
				frb_info(adev, "display: connector %d (hpd %d): HPD line up (pad %d, DC_HPD_INT_STATUS %08x) "
					 "but no DPCD answers: no DisplayPort sink", i, c->hpd,
					 dce_v6_0_hpd_pad(adev, c->hpd),
					 c->hpd >= 0 && c->hpd < 6 ? RREG32(mmDC_HPD1_INT_STATUS + hpd_offsets[c->hpd]) : 0);
			continue;
		}
		c->detect_logged = 6;
		if (frb_dp_read_edid(adev, c))
			frb_err(adev, "display: connector %d: no EDID", i);
		frb_info(adev, "display: connector %d (hpd %d): DP sink, DPCD %x.%x, max %s x%u; EDID %u bytes",
			 i, c->hpd, c->dpcd[0] >> 4, c->dpcd[0] & 0xf,
			 c->dpcd[1] == 0x14 ? "HBR2" : c->dpcd[1] == 0x1e ? "HBR3" : c->dpcd[1] == 0x0a ? "HBR" : "RBR",
			 c->dpcd[2] & 0x1f, c->edid_len);
		if (found < 0)
			found = i;
	}
	return found;
}

/* dce_v6_0_crtc_load_lut, from the CRTC's gamma store */
static void dce_v6_0_load_lut(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t o = c->crtc_offset;
	int i;

	WREG32(mmINPUT_CSC_CONTROL + o,
	       ((INPUT_CSC_BYPASS << INPUT_CSC_CONTROL__INPUT_CSC_GRPH_MODE__SHIFT) |
		(INPUT_CSC_BYPASS << INPUT_CSC_CONTROL__INPUT_CSC_OVL_MODE__SHIFT)));
	WREG32(mmPRESCALE_GRPH_CONTROL + o, PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK);
	WREG32(mmPRESCALE_OVL_CONTROL + o, PRESCALE_OVL_CONTROL__OVL_PRESCALE_BYPASS_MASK);
	WREG32(mmINPUT_GAMMA_CONTROL + o,
	       ((INPUT_GAMMA_USE_LUT << INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE__SHIFT) |
		(INPUT_GAMMA_USE_LUT << INPUT_GAMMA_CONTROL__OVL_INPUT_GAMMA_MODE__SHIFT)));

	WREG32(mmDC_LUT_CONTROL + o, 0);

	WREG32(mmDC_LUT_BLACK_OFFSET_BLUE + o, 0);
	WREG32(mmDC_LUT_BLACK_OFFSET_GREEN + o, 0);
	WREG32(mmDC_LUT_BLACK_OFFSET_RED + o, 0);

	WREG32(mmDC_LUT_WHITE_OFFSET_BLUE + o, 0xffff);
	WREG32(mmDC_LUT_WHITE_OFFSET_GREEN + o, 0xffff);
	WREG32(mmDC_LUT_WHITE_OFFSET_RED + o, 0xffff);

	WREG32(mmDC_LUT_RW_MODE + o, 0);
	WREG32(mmDC_LUT_WRITE_EN_MASK + o, 0x00000007);

	WREG32(mmDC_LUT_RW_INDEX + o, 0);
	/* the ramp every CRTC starts with is i << 8 (hw_init): 10-bit entries i << 2 (frb_dce_v11_0.c) */
	for (i = 0; i < 256; i++)
		WREG32(mmDC_LUT_30_COLOR + o, ((uint32_t)(c->gamma[0][i] & 0xffc0) << 14) |
		       ((uint32_t)(c->gamma[1][i] & 0xffc0) << 4) | (c->gamma[2][i] >> 6));

	WREG32(mmDEGAMMA_CONTROL + o,
	       ((DEGAMMA_BYPASS << DEGAMMA_CONTROL__GRPH_DEGAMMA_MODE__SHIFT) |
		(DEGAMMA_BYPASS << DEGAMMA_CONTROL__OVL_DEGAMMA_MODE__SHIFT) |
		(DEGAMMA_BYPASS << DEGAMMA_CONTROL__ICON_DEGAMMA_MODE__SHIFT) |
		(DEGAMMA_BYPASS << DEGAMMA_CONTROL__CURSOR_DEGAMMA_MODE__SHIFT)));
	WREG32(mmGAMUT_REMAP_CONTROL + o,
	       ((GAMUT_REMAP_BYPASS << GAMUT_REMAP_CONTROL__GRPH_GAMUT_REMAP_MODE__SHIFT) |
		(GAMUT_REMAP_BYPASS << GAMUT_REMAP_CONTROL__OVL_GAMUT_REMAP_MODE__SHIFT)));
	WREG32(mmREGAMMA_CONTROL + o,
	       ((REGAMMA_BYPASS << REGAMMA_CONTROL__GRPH_REGAMMA_MODE__SHIFT) |
		(REGAMMA_BYPASS << REGAMMA_CONTROL__OVL_REGAMMA_MODE__SHIFT)));
	WREG32(mmOUTPUT_CSC_CONTROL + o,
	       ((OUTPUT_CSC_BYPASS << OUTPUT_CSC_CONTROL__OUTPUT_CSC_GRPH_MODE__SHIFT) |
		(OUTPUT_CSC_BYPASS << OUTPUT_CSC_CONTROL__OUTPUT_CSC_OVL_MODE__SHIFT)));
	/* XXX match this to the depth of the crtc fmt block, move to modeset? */
	WREG32(0x1a50 + o, 0);
}

static int dce_v6_0_read_lut(struct frb_device *adev, struct frb_display *d, uint32_t *out)
{
	struct frb_crtc *c = &d->crtc;
	uint32_t o = c->crtc_offset;
	int i;

	if (!c->enabled)
		return -ENODEV;
	WREG32(mmDC_LUT_RW_MODE + o, 0);
	for (i = 0; i < 256; i++) {
		WREG32(mmDC_LUT_RW_INDEX + o, i);
		out[i] = RREG32(mmDC_LUT_30_COLOR + o);
	}
	return 0;
}

/* dce_v6_0_program_fmt, 8 bpc (amdgpu_connector_get_monitor_bpc), dither off: truncate to 8 */
static void program_fmt(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp = FMT_BIT_DEPTH_CONTROL__FMT_TRUNCATE_EN_MASK | FMT_BIT_DEPTH_CONTROL__FMT_TRUNCATE_DEPTH_MASK;

	WREG32(mmFMT_BIT_DEPTH_CONTROL + c->crtc_offset, tmp);
}

/* --- line buffer and watermarks (dce_v6_0.c, drm_fixed.h 20.12) ----------- */

typedef struct { uint32_t full; } fixed20_12;
#define dfixed_const(A)		(uint32_t)(((A) << 12))
#define dfixed_mul(A, B)	((uint64_t)((uint64_t)(A).full * (B).full + 2048) >> 12)
#define dfixed_trunc(A)		((A).full >> 12)
static uint32_t dfixed_div(fixed20_12 A, fixed20_12 B)
{
	uint64_t tmp = ((uint64_t)A.full << 13);

	tmp /= B.full;
	tmp += 1;
	tmp /= 2;
	return (uint32_t)tmp;
}
static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

struct dce6_wm_params {
	uint32_t dram_channels;	/* number of dram channels */
	uint32_t yclk;		/* bandwidth per dram data pin in kHz */
	uint32_t sclk;		/* engine clock in kHz */
	uint32_t disp_clk;	/* display clock in kHz */
	uint32_t src_width;	/* viewport width */
	uint32_t active_time;	/* active display time in ns */
	uint32_t blank_time;	/* blank time in ns */
	int interlaced;		/* mode is interlaced */
	fixed20_12 vsc;		/* vertical scale ratio */
	uint32_t num_heads;	/* number of active crtcs */
	uint32_t bytes_per_pixel; /* bytes per pixel display + overlay */
	uint32_t lb_size;	/* line buffer allocated to pipe */
	uint32_t vtaps;		/* vertical scaler taps */
};

/* si_get_number_of_dram_channels */
static uint32_t si_get_number_of_dram_channels(struct frb_device *adev)
{
	uint32_t tmp = RREG32(mmMC_SHARED_CHMAP);

	switch ((tmp & MC_SHARED_CHMAP__NOOFCHAN_MASK) >> MC_SHARED_CHMAP__NOOFCHAN__SHIFT) {
	case 0:
	default:
		return 1;
	case 1:
		return 2;
	case 2:
		return 4;
	case 3:
		return 8;
	case 4:
		return 3;
	case 5:
		return 6;
	case 6:
		return 10;
	case 7:
		return 12;
	case 8:
		return 16;
	}
}

static uint32_t dce_v6_0_dram_bandwidth(struct dce6_wm_params *wm)
{
	fixed20_12 dram_efficiency, yclk, dram_channels, bandwidth, a;

	a.full = dfixed_const(1000);
	yclk.full = dfixed_const(wm->yclk);
	yclk.full = dfixed_div(yclk, a);
	dram_channels.full = dfixed_const(wm->dram_channels * 4);
	a.full = dfixed_const(10);
	dram_efficiency.full = dfixed_const(7);
	dram_efficiency.full = dfixed_div(dram_efficiency, a);
	bandwidth.full = dfixed_mul(dram_channels, yclk);
	bandwidth.full = dfixed_mul(bandwidth, dram_efficiency);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v6_0_dram_bandwidth_for_display(struct dce6_wm_params *wm)
{
	fixed20_12 disp_dram_allocation, yclk, dram_channels, bandwidth, a;

	a.full = dfixed_const(1000);
	yclk.full = dfixed_const(wm->yclk);
	yclk.full = dfixed_div(yclk, a);
	dram_channels.full = dfixed_const(wm->dram_channels * 4);
	a.full = dfixed_const(10);
	disp_dram_allocation.full = dfixed_const(3); /* XXX worse case value 0.3 */
	disp_dram_allocation.full = dfixed_div(disp_dram_allocation, a);
	bandwidth.full = dfixed_mul(dram_channels, yclk);
	bandwidth.full = dfixed_mul(bandwidth, disp_dram_allocation);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v6_0_data_return_bandwidth(struct dce6_wm_params *wm)
{
	fixed20_12 return_efficiency, sclk, bandwidth, a;

	a.full = dfixed_const(1000);
	sclk.full = dfixed_const(wm->sclk);
	sclk.full = dfixed_div(sclk, a);
	a.full = dfixed_const(10);
	return_efficiency.full = dfixed_const(8);
	return_efficiency.full = dfixed_div(return_efficiency, a);
	a.full = dfixed_const(32);
	bandwidth.full = dfixed_mul(a, sclk);
	bandwidth.full = dfixed_mul(bandwidth, return_efficiency);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v6_0_dmif_request_bandwidth(struct dce6_wm_params *wm)
{
	fixed20_12 disp_clk_request_efficiency, disp_clk, bandwidth, a, b;

	a.full = dfixed_const(1000);
	disp_clk.full = dfixed_const(wm->disp_clk);
	disp_clk.full = dfixed_div(disp_clk, a);
	a.full = dfixed_const(32);
	b.full = dfixed_mul(a, disp_clk);
	a.full = dfixed_const(10);
	disp_clk_request_efficiency.full = dfixed_const(8);
	disp_clk_request_efficiency.full = dfixed_div(disp_clk_request_efficiency, a);
	bandwidth.full = dfixed_mul(b, disp_clk_request_efficiency);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v6_0_available_bandwidth(struct dce6_wm_params *wm)
{
	return min_u32(dce_v6_0_dram_bandwidth(wm),
		       min_u32(dce_v6_0_data_return_bandwidth(wm), dce_v6_0_dmif_request_bandwidth(wm)));
}

static uint32_t dce_v6_0_average_bandwidth(struct dce6_wm_params *wm)
{
	fixed20_12 bpp, line_time, src_width, bandwidth, a;

	a.full = dfixed_const(1000);
	line_time.full = dfixed_const(wm->active_time + wm->blank_time);
	line_time.full = dfixed_div(line_time, a);
	bpp.full = dfixed_const(wm->bytes_per_pixel);
	src_width.full = dfixed_const(wm->src_width);
	bandwidth.full = dfixed_mul(src_width, bpp);
	bandwidth.full = dfixed_mul(bandwidth, wm->vsc);
	bandwidth.full = dfixed_div(bandwidth, line_time);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v6_0_latency_watermark(struct dce6_wm_params *wm)
{
	uint32_t mc_latency = 2000;	/* 2000 ns. */
	uint32_t available_bandwidth = dce_v6_0_available_bandwidth(wm);
	uint32_t worst_chunk_return_time, cursor_line_pair_return_time, dc_latency;
	uint32_t other_heads_data_return_time, latency;
	uint32_t max_src_lines_per_dst_line, lb_fill_bw, line_fill_time;
	uint32_t tmp, dmif_size = 12288;
	fixed20_12 a, b, c;

	if (wm->num_heads == 0 || !available_bandwidth || !wm->disp_clk)
		return 0;
	worst_chunk_return_time = (512 * 8 * 1000) / available_bandwidth;
	cursor_line_pair_return_time = (128 * 4 * 1000) / available_bandwidth;
	dc_latency = 40000000 / wm->disp_clk; /* dc pipe latency */
	other_heads_data_return_time = ((wm->num_heads + 1) * worst_chunk_return_time) +
		(wm->num_heads * cursor_line_pair_return_time);
	latency = mc_latency + other_heads_data_return_time + dc_latency;

	a.full = dfixed_const(2);
	b.full = dfixed_const(1);
	if ((wm->vsc.full > a.full) ||
	    ((wm->vsc.full > b.full) && (wm->vtaps >= 3)) ||
	    (wm->vtaps >= 5) ||
	    ((wm->vsc.full >= a.full) && wm->interlaced))
		max_src_lines_per_dst_line = 4;
	else
		max_src_lines_per_dst_line = 2;

	a.full = dfixed_const(available_bandwidth);
	b.full = dfixed_const(wm->num_heads);
	a.full = dfixed_div(a, b);
	tmp = (uint32_t)(((uint64_t)dmif_size * (uint64_t)wm->disp_clk) / (mc_latency + 512));
	tmp = min_u32(dfixed_trunc(a), tmp);

	lb_fill_bw = min_u32(tmp, wm->disp_clk * wm->bytes_per_pixel / 1000);

	a.full = dfixed_const(max_src_lines_per_dst_line * wm->src_width * wm->bytes_per_pixel);
	b.full = dfixed_const(1000);
	c.full = dfixed_const(lb_fill_bw);
	b.full = dfixed_div(c, b);
	a.full = dfixed_div(a, b);
	line_fill_time = dfixed_trunc(a);

	if (line_fill_time < wm->active_time)
		return latency;
	return latency + (line_fill_time - wm->active_time);
}

static int dce_v6_0_average_bandwidth_vs_dram_bandwidth_for_display(struct dce6_wm_params *wm)
{
	return dce_v6_0_average_bandwidth(wm) <= (dce_v6_0_dram_bandwidth_for_display(wm) / wm->num_heads);
}

static int dce_v6_0_average_bandwidth_vs_available_bandwidth(struct dce6_wm_params *wm)
{
	return dce_v6_0_average_bandwidth(wm) <= (dce_v6_0_available_bandwidth(wm) / wm->num_heads);
}

static int dce_v6_0_check_latency_hiding(struct dce6_wm_params *wm)
{
	uint32_t lb_partitions = wm->lb_size / wm->src_width;
	uint32_t line_time = wm->active_time + wm->blank_time;
	uint32_t latency_tolerant_lines, latency_hiding;
	fixed20_12 a;

	a.full = dfixed_const(1);
	if (wm->vsc.full > a.full)
		latency_tolerant_lines = 1;
	else if (lb_partitions <= (wm->vtaps + 1))
		latency_tolerant_lines = 1;
	else
		latency_tolerant_lines = 2;

	latency_hiding = (latency_tolerant_lines * line_time + wm->blank_time);

	return dce_v6_0_latency_watermark(wm) <= latency_hiding;
}

/*
 * dce_v6_0_program_watermarks: watermark A for the high clocks, B for the
 * low ones (DPM's top and bottom levels, else the boot clocks; SI DPM is
 * not ported, so the boot clocks). No scaling: vsc = hsc = 1.
 */
static void program_watermarks(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c,
			       uint32_t lb_size, uint32_t num_heads)
{
	struct dce6_wm_params wm_low, wm_high;
	uint32_t active_time, line_time = 0;
	uint32_t latency_watermark_a = 0, latency_watermark_b = 0;
	uint32_t priority_a_mark = 0, priority_b_mark = 0;
	uint32_t priority_a_cnt = PRIORITY_OFF, priority_b_cnt = PRIORITY_OFF;
	uint32_t tmp, arb_control3, lb_vblank_lead_lines = 0;
	fixed20_12 a, b, cc, one;

	one.full = dfixed_const(1);
	if (c->base_enabled && num_heads && c->mode.clock) {
		uint32_t dram_channels = si_get_number_of_dram_channels(adev);

		active_time = (uint32_t)(((uint64_t)c->mode.hdisplay * 1000000) / c->mode.clock);
		line_time = (uint32_t)(((uint64_t)c->mode.htotal * 1000000) / c->mode.clock);
		line_time = min_u32(line_time, 65535);
		priority_a_cnt = 0;
		priority_b_cnt = 0;

		memset(&wm_high, 0, sizeof(wm_high));
		/* watermark for high clocks */
		if (frb_pm_enabled(adev)) {
			wm_high.yclk = frb_pm_get_mclk(adev, 0) * 10;
			wm_high.sclk = frb_pm_get_sclk(adev, 0) * 10;
		} else {
			wm_high.yclk = d->current_mclk * 10;
			wm_high.sclk = d->current_sclk * 10;
		}
		wm_high.disp_clk = c->mode.clock;
		wm_high.src_width = c->mode.hdisplay;
		wm_high.active_time = active_time;
		wm_high.blank_time = line_time - wm_high.active_time;
		wm_high.interlaced = 0;
		wm_high.vsc = one;
		wm_high.vtaps = 1;
		wm_high.bytes_per_pixel = 4; /* XXX: get this from fb config */
		wm_high.lb_size = lb_size;
		wm_high.dram_channels = dram_channels;
		wm_high.num_heads = num_heads;

		wm_low = wm_high;
		/* watermark for low clocks */
		if (frb_pm_enabled(adev)) {
			wm_low.yclk = frb_pm_get_mclk(adev, 1) * 10;
			wm_low.sclk = frb_pm_get_sclk(adev, 1) * 10;
		}

		/* set for high clocks */
		latency_watermark_a = min_u32(dce_v6_0_latency_watermark(&wm_high), 65535);
		/* set for low clocks */
		latency_watermark_b = min_u32(dce_v6_0_latency_watermark(&wm_low), 65535);

		/* possibly force display priority to high */
		if (!dce_v6_0_average_bandwidth_vs_dram_bandwidth_for_display(&wm_high) ||
		    !dce_v6_0_average_bandwidth_vs_available_bandwidth(&wm_high) ||
		    !dce_v6_0_check_latency_hiding(&wm_high)) {
			priority_a_cnt |= PRIORITY_ALWAYS_ON;
			priority_b_cnt |= PRIORITY_ALWAYS_ON;
		}
		if (!dce_v6_0_average_bandwidth_vs_dram_bandwidth_for_display(&wm_low) ||
		    !dce_v6_0_average_bandwidth_vs_available_bandwidth(&wm_low) ||
		    !dce_v6_0_check_latency_hiding(&wm_low)) {
			priority_a_cnt |= PRIORITY_ALWAYS_ON;
			priority_b_cnt |= PRIORITY_ALWAYS_ON;
		}

		a.full = dfixed_const(1000);
		b.full = dfixed_const(c->mode.clock);
		b.full = dfixed_div(b, a);
		cc.full = dfixed_const(latency_watermark_a);
		cc.full = dfixed_mul(cc, b);
		cc.full = dfixed_mul(cc, one);	/* amdgpu_crtc->hsc */
		cc.full = dfixed_div(cc, a);
		a.full = dfixed_const(16);
		cc.full = dfixed_div(cc, a);
		priority_a_mark = dfixed_trunc(cc);
		priority_a_cnt |= priority_a_mark & PRIORITY_MARK_MASK;

		a.full = dfixed_const(1000);
		b.full = dfixed_const(c->mode.clock);
		b.full = dfixed_div(b, a);
		cc.full = dfixed_const(latency_watermark_b);
		cc.full = dfixed_mul(cc, b);
		cc.full = dfixed_mul(cc, one);
		cc.full = dfixed_div(cc, a);
		a.full = dfixed_const(16);
		cc.full = dfixed_div(cc, a);
		priority_b_mark = dfixed_trunc(cc);
		priority_b_cnt |= priority_b_mark & PRIORITY_MARK_MASK;

		lb_vblank_lead_lines = (lb_size + c->mode.hdisplay - 1) / c->mode.hdisplay;
	}

	/* select wm A */
	arb_control3 = RREG32(mmDPG_PIPE_ARBITRATION_CONTROL3 + c->crtc_offset);
	tmp = arb_control3;
	tmp &= ~(3 << DPG_PIPE_ARBITRATION_CONTROL3__URGENCY_WATERMARK_MASK__SHIFT);
	tmp |= (1 << DPG_PIPE_ARBITRATION_CONTROL3__URGENCY_WATERMARK_MASK__SHIFT);
	WREG32(mmDPG_PIPE_ARBITRATION_CONTROL3 + c->crtc_offset, tmp);
	WREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset,
	       ((latency_watermark_a << DPG_PIPE_URGENCY_CONTROL__URGENCY_LOW_WATERMARK__SHIFT) |
		(line_time << DPG_PIPE_URGENCY_CONTROL__URGENCY_HIGH_WATERMARK__SHIFT)));
	/* select wm B */
	tmp = RREG32(mmDPG_PIPE_ARBITRATION_CONTROL3 + c->crtc_offset);
	tmp &= ~(3 << DPG_PIPE_ARBITRATION_CONTROL3__URGENCY_WATERMARK_MASK__SHIFT);
	tmp |= (2 << DPG_PIPE_ARBITRATION_CONTROL3__URGENCY_WATERMARK_MASK__SHIFT);
	WREG32(mmDPG_PIPE_ARBITRATION_CONTROL3 + c->crtc_offset, tmp);
	WREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset,
	       ((latency_watermark_b << DPG_PIPE_URGENCY_CONTROL__URGENCY_LOW_WATERMARK__SHIFT) |
		(line_time << DPG_PIPE_URGENCY_CONTROL__URGENCY_HIGH_WATERMARK__SHIFT)));
	/* restore original selection */
	WREG32(mmDPG_PIPE_ARBITRATION_CONTROL3 + c->crtc_offset, arb_control3);

	/* write the priority marks */
	WREG32(mmPRIORITY_A_CNT + c->crtc_offset, priority_a_cnt);
	WREG32(mmPRIORITY_B_CNT + c->crtc_offset, priority_b_cnt);

	/* save values for DPM */
	c->line_time = line_time;
	c->wm_high = latency_watermark_a;
	c->wm_low = latency_watermark_b;
	c->lb_vblank_lead_lines = lb_vblank_lead_lines;
}

/*
 * dce_v6_0_line_buffer_adjust. amdgpu passes the paired CRTC's mode as
 * other_mode, a pointer that is never NULL, so an enabled CRTC always takes
 * half the line buffer (DC_LB_MEMORY_CONFIG 0, one DMIF buffer); carried as
 * it is.
 */
static uint32_t line_buffer_adjust(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp, buffer_alloc, i, pipe_offset = c->crtc_id * 0x8;

	if (c->base_enabled && (adev->si_dpm_quirks & FRB_SI_DPM_LB_WHOLE)) {
		tmp = 2;	/* whole: bring-up, FRB_SI_DPM_LB_WHOLE */
		buffer_alloc = 2;
	} else if (c->base_enabled) {
		tmp = 0;	/* 1/2 */
		buffer_alloc = 1;
	} else {
		tmp = 0;
		buffer_alloc = 0;
	}

	WREG32(mmDC_LB_MEMORY_SPLIT + c->crtc_offset, (tmp << DC_LB_MEMORY_SPLIT__DC_LB_MEMORY_CONFIG__SHIFT));

	WREG32(mmPIPE0_DMIF_BUFFER_CONTROL + pipe_offset,
	       (buffer_alloc << PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATED__SHIFT));
	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmPIPE0_DMIF_BUFFER_CONTROL + pipe_offset) &
		    PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK)
			break;
		udelay(1);
	}

	if (c->base_enabled)
		return (adev->si_dpm_quirks & FRB_SI_DPM_LB_WHOLE) ? 8192 * 2 : 4096 * 2;
	/* controller not enabled, so no lb used */
	return 0;
}

/* dce_v6_0_bandwidth_update: every CRTC; only CRTC0 carries state here */
static void bandwidth_update(struct frb_device *adev, struct frb_display *d)
{
	uint32_t num_heads = d->crtc.base_enabled ? 1 : 0, lb_size, i;

	if (frb_pm_enabled(adev)) {
		d->current_sclk = frb_pm_get_sclk(adev, 0);
		d->current_mclk = frb_pm_get_mclk(adev, 0);
	} else {
		d->current_sclk = adev->default_sclk;
		d->current_mclk = adev->default_mclk;
	}

	for (i = 0; i < d->num_crtc; i++) {
		struct frb_crtc idle, *c = &d->crtc;

		if (i != (uint32_t)d->crtc.crtc_id) {
			memset(&idle, 0, sizeof(idle));
			idle.crtc_id = i;
			idle.crtc_offset = crtc_offsets[i];
			c = &idle;
		}
		lb_size = line_buffer_adjust(adev, c);
		program_watermarks(adev, d, c, lb_size, num_heads);
		if (c == &d->crtc)
			frb_info(adev, "display: line buffer %u px, watermarks %u/%u ns, line time %u ns (sclk %u kHz, "
				 "mclk %u kHz)", lb_size, c->wm_high, c->wm_low, c->line_time, d->current_sclk * 10,
				 d->current_mclk * 10);
	}
}

/* dce_v6_0_vga_enable / grph_enable */
static void vga_enable(struct frb_device *adev, struct frb_crtc *c, int enable)
{
	uint32_t v = RREG32(vga_control_regs[c->crtc_id]) & ~1u;

	WREG32(vga_control_regs[c->crtc_id], v | (enable ? 1 : 0));
}

/*
 * dce_v6_0_crtc_do_set_base: a linear surface (ARRAY_LINEAR_GENERAL,
 * pipe config 0) at fb_offset in VRAM: DRM_FORMAT_C8, XRGB1555, XRGB8888,
 * swapped 8 in 16 / 8 in 32 on a big-endian host (__BIG_ENDIAN).
 */
static void crtc_do_set_base(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c)
{
	uint64_t fb_location = adev->gmc.vram_start + c->fb_offset;
	uint32_t fb_format, fb_swap = (GRPH_ENDIAN_NONE << GRPH_SWAP_CNTL__GRPH_ENDIAN_SWAP__SHIFT);
	uint32_t o = c->crtc_offset;

	switch (c->fb_bpp) {
	case 8:
		fb_format = ((GRPH_DEPTH_8BPP << GRPH_CONTROL__GRPH_DEPTH__SHIFT) |
			     (GRPH_FORMAT_INDEXED << GRPH_CONTROL__GRPH_FORMAT__SHIFT));
		break;
	case 16:
		fb_format = ((GRPH_DEPTH_16BPP << GRPH_CONTROL__GRPH_DEPTH__SHIFT) |
			     (GRPH_FORMAT_ARGB1555 << GRPH_CONTROL__GRPH_FORMAT__SHIFT));
#if FRB_BIG_ENDIAN
		fb_swap = (GRPH_ENDIAN_8IN16 << GRPH_SWAP_CNTL__GRPH_ENDIAN_SWAP__SHIFT);
#endif
		break;
	case 32:
	default:
		fb_format = ((GRPH_DEPTH_32BPP << GRPH_CONTROL__GRPH_DEPTH__SHIFT) |
			     (GRPH_FORMAT_ARGB8888 << GRPH_CONTROL__GRPH_FORMAT__SHIFT));
#if FRB_BIG_ENDIAN
		fb_swap = (GRPH_ENDIAN_8IN32 << GRPH_SWAP_CNTL__GRPH_ENDIAN_SWAP__SHIFT);
#endif
		break;
	}
	/* linear: no array mode bits; GRPH_PIPE_CONFIG from the tiling flags, 0 */

	vga_enable(adev, c, 0);

	/* Make sure surface address is updated at vertical blank rather than horizontal blank */
	WREG32(mmGRPH_FLIP_CONTROL + o, 0);

	WREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + o, (uint32_t)(fb_location >> 32));
	WREG32(mmGRPH_SECONDARY_SURFACE_ADDRESS_HIGH + o, (uint32_t)(fb_location >> 32));
	WREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS + o,
	       (uint32_t)fb_location & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
	WREG32(mmGRPH_SECONDARY_SURFACE_ADDRESS + o,
	       (uint32_t)fb_location & GRPH_SECONDARY_SURFACE_ADDRESS__GRPH_SECONDARY_SURFACE_ADDRESS_MASK);
	WREG32(mmGRPH_CONTROL + o, fb_format);
	WREG32(mmGRPH_SWAP_CNTL + o, fb_swap);

	/* 8 bpc: the LUT stays in the path */
	WREG32_P(mmGRPH_LUT_10BIT_BYPASS + o, 0, ~GRPH_LUT_10BIT_BYPASS__GRPH_LUT_10BIT_BYPASS_EN_MASK);

	WREG32(mmGRPH_SURFACE_OFFSET_X + o, 0);
	WREG32(mmGRPH_SURFACE_OFFSET_Y + o, 0);
	WREG32(mmGRPH_X_START + o, 0);
	WREG32(mmGRPH_Y_START + o, 0);
	WREG32(mmGRPH_X_END + o, c->fb_width);
	WREG32(mmGRPH_Y_END + o, c->fb_height);
	WREG32(mmGRPH_PITCH + o, c->fb_pitch);

	WREG32(mmGRPH_ENABLE + o, 1);	/* dce_v6_0_grph_enable */

	WREG32(mmDESKTOP_HEIGHT + o, c->fb_height);
	WREG32(mmVIEWPORT_START + o, 0);
	WREG32(mmVIEWPORT_SIZE + o, ((uint32_t)c->mode.hdisplay << 16) | ((c->mode.vdisplay + 1) & ~1u));

	/* set pageflip to happen anywhere in vblank interval */
	WREG32(mmMASTER_UPDATE_MODE + o, 0);

	/* Bytes per pixel may have changed */
	bandwidth_update(adev, d);
}

/* amdgpu_dpm_get_display_cfg (frb_dce_v11_0.c's) */
static void amdgpu_dpm_get_display_cfg(struct frb_device *adev, struct frb_display *d)
{
	struct amd_pp_display_configuration *cfg = &d->pm_cfg;
	struct frb_crtc *c = &d->crtc;
	int num_crtcs = 0;

	cfg->min_vblank_time = 0xffffffff;
	if (c->enabled) {
		struct single_display_configuration *display_cfg = &cfg->displays[num_crtcs++];

		if (c->mode.clock) {
			uint32_t vrefresh = frb_mode_hz(&c->mode);
			uint32_t vblank_in_pixels = (uint32_t)c->mode.htotal * (c->mode.vtotal - c->mode.vdisplay);
			uint32_t vblank_time_us = vblank_in_pixels * 1000 / c->mode.clock;

			if (vrefresh > 120)
				vblank_time_us = 0;
			if (vblank_time_us < cfg->min_vblank_time)
				cfg->min_vblank_time = vblank_time_us;
			if (!cfg->vrefresh)
				cfg->vrefresh = vrefresh;
		}
		if ((uint32_t)c->crtc_id < cfg->crtc_index) {
			cfg->crtc_index = c->crtc_id;
			cfg->line_time_in_us = c->line_time;
		}
		display_cfg->controller_id = c->crtc_id;
		display_cfg->pixel_clock = c->conn ? c->conn->pixel_clock : 0;
	}
	cfg->display_clk = d->default_dispclk;
	cfg->num_display = num_crtcs;
}

/* amdgpu_dpm_compute_clocks (SI DPM is not ported: frb_pm_enabled is false) */
static void amdgpu_dpm_compute_clocks(struct frb_device *adev, struct frb_display *d)
{
	if (!frb_pm_enabled(adev))
		return;
	bandwidth_update(adev, d);
	amdgpu_dpm_get_display_cfg(adev, d);
	frb_pm_compute_clocks(adev, &d->pm_cfg);
}

static void dce_v6_0_dpm_changed(struct frb_device *adev, struct frb_display *d)
{
	amdgpu_dpm_compute_clocks(adev, d);
}

/* dce_v6_0_crtc_dpms */
static void crtc_dpms(struct frb_device *adev, struct frb_crtc *c, int on)
{
	if (on) {
		c->enabled = 1;
		frb_atombios_crtc_enable(adev, c, ATOM_ENABLE);
		frb_atombios_crtc_blank(adev, c, ATOM_DISABLE);
		dce_v6_0_load_lut(adev, c);
	} else {
		if (c->enabled)
			frb_atombios_crtc_blank(adev, c, ATOM_ENABLE);
		frb_atombios_crtc_enable(adev, c, ATOM_DISABLE);
		c->enabled = 0;
	}
	/* adjust pm to dpms */
	if (adev->display)
		amdgpu_dpm_compute_clocks(adev, adev->display);
}

/*
 * dce_v6_0_pick_pll: DP takes PPLL0 (the shared DP PLL) unless the board
 * has a DP external clock, which needs none; TMDS the first free of PPLL2
 * and PPLL1 (one CRTC here: PPLL2).
 */
static int pick_pll(struct frb_display *d, struct frb_connector *conn)
{
	if (frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP)
		return d->dp_extclk ? ATOM_PPLL_INVALID : ATOM_PPLL0;
	return ATOM_PPLL2;
}

/* dce_v6_0_pick_dig_encoder */
static int pick_dig_encoder(struct frb_connector *conn)
{
	switch (conn->encoder_id) {
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:		return conn->linkb ? 1 : 0;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:	return conn->linkb ? 3 : 2;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:	return conn->linkb ? 5 : 4;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:	return 6;
	default:					return 0;
	}
}

/* --- the hardware cursor (dce_v6_0.c) --------------------------------------- */

static void dce_v6_0_lock_cursor(struct frb_device *adev, struct frb_crtc *c, int lock)
{
	uint32_t cur_lock = RREG32(mmCUR_UPDATE + c->crtc_offset);

	if (lock)
		cur_lock |= CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK;
	else
		cur_lock &= ~CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK;
	WREG32(mmCUR_UPDATE + c->crtc_offset, cur_lock);
}

static void dce_v6_0_hide_cursor(struct frb_device *adev, struct frb_crtc *c)
{
	WREG32(mmCUR_CONTROL + c->crtc_offset,
	       (CURSOR_24_8_PRE_MULT << CUR_CONTROL__CURSOR_MODE__SHIFT) |
	       (CURSOR_URGENT_1_2 << CUR_CONTROL__CURSOR_URGENT_CONTROL__SHIFT));
}

static void dce_v6_0_show_cursor(struct frb_device *adev, struct frb_crtc *c)
{
	WREG32(mmCUR_SURFACE_ADDRESS_HIGH + c->crtc_offset, (uint32_t)(c->cursor_addr >> 32));
	WREG32(mmCUR_SURFACE_ADDRESS + c->crtc_offset, (uint32_t)c->cursor_addr);
	WREG32(mmCUR_CONTROL + c->crtc_offset,
	       CUR_CONTROL__CURSOR_EN_MASK |
	       (CURSOR_24_8_PRE_MULT << CUR_CONTROL__CURSOR_MODE__SHIFT) |
	       (CURSOR_URGENT_1_2 << CUR_CONTROL__CURSOR_URGENT_CONTROL__SHIFT));
}

static int dce_v6_0_cursor_move_locked(struct frb_device *adev, struct frb_crtc *c, int x, int y)
{
	int xorigin = 0, yorigin = 0;

	c->cursor_x = x;
	c->cursor_y = y;

	/* avivo cursor are offset into the total surface */
	if (x < 0) {
		xorigin = -x < CURSOR_WIDTH - 1 ? -x : CURSOR_WIDTH - 1;
		x = 0;
	}
	if (y < 0) {
		yorigin = -y < CURSOR_HEIGHT - 1 ? -y : CURSOR_HEIGHT - 1;
		y = 0;
	}

	WREG32(mmCUR_POSITION + c->crtc_offset, ((uint32_t)x << 16) | (uint32_t)y);
	WREG32(mmCUR_HOT_SPOT + c->crtc_offset, ((uint32_t)xorigin << 16) | (uint32_t)yorigin);
	WREG32(mmCUR_SIZE + c->crtc_offset, ((c->cursor_width - 1) << 16) | (c->cursor_height - 1));
	return 0;
}

/* dce_v6_0_crtc_cursor_set2 with the cursor bo already in VRAM (frb_dce_v11_0.c's) */
static int dce_v6_0_cursor_set(struct frb_device *adev, struct frb_display *d, uint32_t bar_offset,
			       uint32_t width, uint32_t height)
{
	struct frb_crtc *c = &d->crtc;

	if (!d->ready)
		return -ENODEV;
	if (bar_offset == ~0u) {
		dce_v6_0_hide_cursor(adev, c);
		c->cursor_addr = 0;
		return 0;
	}
	/* the plane is 64 x 64 at a 64-pixel pitch (CURSOR_WIDTH), unlike DCE 8-11's 128 */
	if (width < 1 || height < 1 || width > CURSOR_WIDTH || height > CURSOR_HEIGHT) {
		frb_err(adev, "display: bad cursor width or height %u x %u", width, height);
		return -EINVAL;
	}
	if ((bar_offset & 4095) || !frb_vram_ok(adev, bar_offset, CURSOR_WIDTH * CURSOR_HEIGHT * 4))
		return -ERANGE;
	c->cursor_addr = adev->gmc.vram_start + bar_offset;

	dce_v6_0_lock_cursor(adev, c, 1);
	if (width != c->cursor_width || height != c->cursor_height) {
		c->cursor_width = width;
		c->cursor_height = height;
		dce_v6_0_cursor_move_locked(adev, c, c->cursor_x, c->cursor_y);
	}
	if (c->cursor_visible)
		dce_v6_0_show_cursor(adev, c);
	dce_v6_0_lock_cursor(adev, c, 0);
	return 0;
}

static int dce_v6_0_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible)
{
	struct frb_crtc *c = &d->crtc;

	if (!d->ready)
		return -ENODEV;
	c->cursor_visible = visible;
	if (!c->cursor_addr)
		return 0;
	dce_v6_0_lock_cursor(adev, c, 1);
	dce_v6_0_cursor_move_locked(adev, c, x, y);
	if (visible)
		dce_v6_0_show_cursor(adev, c);
	else
		dce_v6_0_hide_cursor(adev, c);
	dce_v6_0_lock_cursor(adev, c, 0);
	return 0;
}

/* dce_v6_0_cursor_reset (the cursor kept hidden when IOFramebuffer hid it) */
static void dce_v6_0_cursor_reset(struct frb_device *adev, struct frb_crtc *c)
{
	if (c->cursor_addr) {
		dce_v6_0_lock_cursor(adev, c, 1);
		dce_v6_0_cursor_move_locked(adev, c, c->cursor_x, c->cursor_y);
		if (c->cursor_visible)
			dce_v6_0_show_cursor(adev, c);
		dce_v6_0_lock_cursor(adev, c, 0);
	}
}

static void dce_v6_0_off(struct frb_device *adev, struct frb_display *d);
static void dce_v6_0_report(struct frb_device *adev, struct frb_display *d);

/* drm_crtc_helper_set_mode with dce_v6_0's helpers, for one connector on CRTC0 */
static int dce_v6_0_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			     const struct frb_mode *mode, uint32_t fb_offset, uint32_t pitch_pixels,
			     uint32_t bpp, uint32_t max_link_rate)
{
	struct frb_crtc *c = &d->crtc;
	uint64_t bytes = (uint64_t)pitch_pixels * (bpp / 8) * mode->vdisplay;
	int is_dp, tr;

	if (!d->ready || !conn)
		return -EINVAL;
	if (!conn_is_uniphy(conn))
		return -ENODEV;
	if (bpp != 8 && bpp != 16 && bpp != 32)
		return -EINVAL;
	if (!frb_vram_ok(adev, fb_offset, (uint32_t)bytes) || fb_offset + bytes > adev->layout.reserved_start ||
	    (fb_offset & 0xff) || pitch_pixels < mode->hdisplay ||
	    pitch_pixels != frb_display_align_pitch(pitch_pixels, bpp)) {
		frb_err(adev, "display: surface %ux%u at %u bpp at 0x%x does not fit below the driver's VRAM",
			pitch_pixels, mode->vdisplay, bpp, fb_offset);
		return -ERANGE;
	}
	is_dp = frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP;
	if (is_dp ? !conn->dpcd[0] : !conn->sink_tmds) {
		frb_err(adev, "display: connector hpd %d has no detected sink", conn->hpd);
		return -ENODEV;
	}
	if (is_dp) {
		/* encoder mode_fixup (amdgpu_atombios_encoder_mode_fixup): the DP link configuration */
		if (frb_dp_set_link_config(conn, mode->clock, max_link_rate)) {
			frb_err(adev, "display: %ux%u at %u kHz does not fit the DP link", mode->hdisplay,
				mode->vdisplay, mode->clock);
			return -ERANGE;
		}
	} else if (!frb_display_mode_valid(conn, mode, 0)) {
		frb_err(adev, "display: %ux%u at %u kHz is past the %s link", mode->hdisplay, mode->vdisplay,
			mode->clock, conn->is_hdmi ? "HDMI" : "DVI");
		return -ERANGE;
	}
	if (c->conn && c->conn != conn)
		dce_v6_0_off(adev, d);
	c->conn = conn;
	c->mode = *mode;
	c->fb_offset = fb_offset;
	c->fb_pitch = pitch_pixels;
	c->fb_width = mode->hdisplay;
	c->fb_height = mode->vdisplay;
	c->fb_bpp = bpp;
	c->base_enabled = 1;		/* drm_helper_crtc_in_use */
	/* ... and its "vertical FP must be at least 1" */
	if (c->mode.vsync_start == c->mode.vdisplay) {
		c->mode.vsync_start++;
		if (c->mode.vsync_end < c->mode.vsync_start)
			c->mode.vsync_end = c->mode.vsync_start;
	}
	if (is_dp)
		frb_info(adev, "display: mode %ux%u@%u (%u kHz) on connector hpd %d: DP %u MHz x%u",
			 mode->hdisplay, mode->vdisplay, frb_mode_hz(mode), mode->clock, conn->hpd,
			 conn->dp_clock / 1000, conn->dp_lane_count);
	else
		frb_info(adev, "display: mode %ux%u@%u (%u kHz) on connector hpd %d: TMDS (%s sink, DVI signalling%s)",
			 mode->hdisplay, mode->vdisplay, frb_mode_hz(mode), mode->clock, conn->hpd,
			 conn->is_hdmi ? "HDMI" : "DVI",
			 frb_dig_monitor_is_duallink(conn, mode->clock) ? ", dual link" : "");
	/* crtc mode_fixup: prepare_pll, then pick_pll */
	frb_atombios_crtc_prepare_pll(adev, d, c);
	c->pll_id = pick_pll(d, conn);

	/* encoder prepare */
	conn->dig_encoder = pick_dig_encoder(conn);
	frb_atombios_scratch_regs_lock(adev, 1);
	/* this is needed for the pll/ss setup to work correctly in some cases */
	frb_atombios_encoder_set_crtc_source(adev, c);
	/* set up the FMT blocks */
	program_fmt(adev, c);
	/* crtc prepare: disable crtc pair power gating before programming */
	frb_atombios_crtc_powergate(adev, c, ATOM_DISABLE);
	frb_atombios_crtc_lock(adev, c, ATOM_ENABLE);
	crtc_dpms(adev, c, 0);
	/* crtc mode_set */
	if (!c->adjusted_clock) {
		frb_err(adev, "display: no adjusted pixel clock");
		frb_atombios_crtc_lock(adev, c, ATOM_DISABLE);
		frb_atombios_scratch_regs_lock(adev, 0);
		c->base_enabled = 0;
		c->conn = NULL;
		return -EINVAL;
	}
	frb_atombios_crtc_set_pll(adev, d, c);
	frb_atombios_crtc_set_dtd_timing(adev, c, &c->mode);
	crtc_do_set_base(adev, d, c);
	frb_atombios_crtc_overscan_setup(adev, c);
	frb_atombios_crtc_scaler_setup(adev, c);
	dce_v6_0_cursor_reset(adev, c);
	/* encoder mode_set */
	conn->pixel_clock = c->mode.clock;
	/* RadeonNI's rdn_modeset: the DIG's audio off before DP_VIDEO_OFF and the transmitter */
	if (adev->want_audio && is_dp)
		frb_audio_dp_enable(adev, d, conn->dig_encoder, false);
	/* need to call this here rather than in prepare() since we need some crtc info */
	frb_atombios_encoder_dpms(adev, c, 0);
	/* ... and set up for this mode while the transmitter is off (dce_v6_0_afmt_setmode's place) */
	if (adev->want_audio && is_dp) {
		int ar = frb_audio_dp_mode_set(adev, d, c->crtc_id, conn->dig_encoder, &c->mode,
					       conn->edid, conn->edid_len);

		if (ar)
			frb_err(adev, "audio: not set up (%d)", ar);
	}
	/* set scaler clears this on some chips: dce_v6_0_set_interleave, progressive */
	WREG32(mmDATA_FORMAT + c->crtc_offset, 0);
	/* crtc commit */
	crtc_dpms(adev, c, 1);
	frb_atombios_crtc_lock(adev, c, ATOM_DISABLE);
	/* encoder commit */
	tr = frb_atombios_encoder_dpms(adev, c, 1);
	/* RadeonNI: the audio packets and the pin's presence once the link trained and the video is on */
	if (adev->want_audio && is_dp && !tr)
		frb_audio_dp_enable(adev, d, conn->dig_encoder, true);
	frb_atombios_scratch_regs_lock(adev, 0);
	if (is_dp)
		frb_dp_link_report(adev, conn);
	dce_v6_0_report(adev, d);
	return 0;
}

/*
 * drm_crtc_helper's disable: dce_v6_0_encoder_disable (transmitter off),
 * then dce_v6_0_crtc_disable (dpms off, GRPH off, power gate, the PPLL off
 * for TMDS: SetPixelClock with ATOM_DISABLE), then the line buffer.
 */
static void dce_v6_0_off(struct frb_device *adev, struct frb_display *d)
{
	struct frb_crtc *c = &d->crtc;

	if (!c->conn)
		return;
	/* RadeonNI's rdn_output_disable: the DIG's audio off first */
	if (adev->want_audio && frb_atombios_encoder_get_encoder_mode(c->conn) == ATOM_ENCODER_MODE_DP)
		frb_audio_dp_enable(adev, d, c->conn->dig_encoder, false);
	frb_atombios_encoder_dpms(adev, c, 0);
	c->conn->dig_encoder = -1;
	crtc_dpms(adev, c, 0);
	WREG32(mmGRPH_ENABLE + c->crtc_offset, 0);
	frb_atombios_crtc_powergate(adev, c, ATOM_ENABLE);
	if (c->pll_id == ATOM_PPLL1 || c->pll_id == ATOM_PPLL2)
		frb_atombios_crtc_pll_off(adev, c->crtc_id, c->pll_id);	/* disable the ppll */
	c->pll_id = ATOM_PPLL_INVALID;
	c->adjusted_clock = 0;
	c->base_enabled = 0;
	bandwidth_update(adev, d);
	c->conn = NULL;
}

/* the scanout state, for the log: is the pipe alive */
static void dce_v6_0_report(struct frb_device *adev, struct frb_display *d)
{
	uint32_t o = d->crtc.crtc_offset, a, b;

	a = RREG32(mmCRTC_STATUS_FRAME_COUNT + o);
	udelay(20000);
	b = RREG32(mmCRTC_STATUS_FRAME_COUNT + o);
	frb_info(adev, "display: CRTC_CONTROL %08x, BLANK %08x, frames %u -> %u in 20 ms, H/V total %u/%u, "
		 "GRPH enable %x control %08x swap %08x addr %08x:%08x pitch %u, viewport %08x, LB split %08x",
		 RREG32(mmCRTC_CONTROL + o), RREG32(mmCRTC_BLANK_CONTROL + o), a, b,
		 RREG32(mmCRTC_H_TOTAL + o) + 1, RREG32(mmCRTC_V_TOTAL + o) + 1,
		 RREG32(mmGRPH_ENABLE + o), RREG32(mmGRPH_CONTROL + o), RREG32(mmGRPH_SWAP_CNTL + o),
		 RREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + o), RREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS + o),
		 RREG32(mmGRPH_PITCH + o), RREG32(mmVIEWPORT_SIZE + o), RREG32(mmDC_LB_MEMORY_SPLIT + o));
}

/* frb_display_funcs.hpd_sense: the connector's HPD pin */
static int dce_v6_0_conn_hpd_sense(struct frb_device *adev, const struct frb_connector *c)
{
	return dce_v6_0_hpd_sense(adev, c->hpd);
}

const struct frb_display_funcs frb_dce_v6_0_funcs = {
	.name = "dce_v6_0",
	.has_audio = 1,
	.audio_regs = &frb_audio_regs_dce6,
	.cursor_max = CURSOR_WIDTH,
	.hpd_gpio_reg = mmDC_GPIO_HPD_A,
	.hw_init = dce_v6_0_hw_init,
	.hw_fini = hpd_fini,
	.hpd_sense = dce_v6_0_conn_hpd_sense,
	.hpd_pad = dce_v6_0_hpd_pad,
	.detect = dce_v6_0_detect,
	.set_mode = dce_v6_0_set_mode,
	.off = dce_v6_0_off,
	.dpm_changed = dce_v6_0_dpm_changed,
	.report = dce_v6_0_report,
	.load_lut = dce_v6_0_load_lut,
	.read_lut = dce_v6_0_read_lut,
	.cursor_set = dce_v6_0_cursor_set,
	.cursor_move = dce_v6_0_cursor_move,
};
