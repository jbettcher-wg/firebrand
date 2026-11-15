/*
 * dce_v11_0.c (Linux 6.16, the legacy non-DC display of Polaris), the parts
 * a single CRTC driving DisplayPort needs: hw_init, HPD, the scanout surface
 * (crtc_do_set_base), the LUT, FMT, line buffer and watermarks; and the
 * modeset in the order drm_crtc_helper_set_mode runs the helpers:
 *
 *   encoder mode_fixup (DP link config) -> crtc mode_fixup (prepare_pll,
 *   pick_pll) -> encoder prepare -> crtc prepare -> crtc mode_set ->
 *   encoder mode_set -> crtc commit -> encoder commit (transmitter, link
 *   training, video on).
 *
 * The scanout surface is linear 32 bpp in VRAM inside the CPU window. The
 * host writes pixels as big-endian words (ARGB in memory), so GRPH_SWAP_CNTL
 * gets ENDIAN_8IN32: dce_v11_0_crtc_do_set_base's own __BIG_ENDIAN arm.
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
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
#include "frb_display.h"
#include "frb_pm.h"
#include "../shared/atom/atombios.h"
#include "include/asic_reg/dce/dce_11_2_d.h"
#include "include/asic_reg/dce/dce_11_2_sh_mask.h"

/* gmc_8_1: MC_SHARED_CHMAP (cik_get_number_of_dram_channels) */
#define mmMC_SHARED_CHMAP_GMC		0x801
#define MC_SHARED_CHMAP__NOOFCHAN_MASK_GMC	0x0000f000
#define MC_SHARED_CHMAP__NOOFCHAN__SHIFT_GMC	12

/* vid.h */
#define ENDIAN_NONE	0
#define ENDIAN_8IN16	1
#define ENDIAN_8IN32	2

static const uint32_t crtc_offsets[] = {
	0x1b9c - 0x1b9c, 0x1d9c - 0x1b9c, 0x1f9c - 0x1b9c,
	0x419c - 0x1b9c, 0x439c - 0x1b9c, 0x459c - 0x1b9c,
};
static const uint32_t hpd_offsets[] = {
	0x1898 - 0x1898, 0x18a0 - 0x1898, 0x18a8 - 0x1898,
	0x18b0 - 0x1898, 0x18b8 - 0x1898, 0x18c0 - 0x1898,
};
static const uint32_t vga_control_regs[6] = {
	mmD1VGA_CONTROL, mmD2VGA_CONTROL, mmD3VGA_CONTROL,
	mmD4VGA_CONTROL, mmD5VGA_CONTROL, mmD6VGA_CONTROL,
};

static const uint32_t polaris11_golden_settings_a11[] = {
	mmDCI_CLK_CNTL, 0x00000080, 0x00000000,
	mmFBC_DEBUG_COMP, 0x000000f0, 0x00000070,
	mmFBC_DEBUG1, 0xffffffff, 0x00000008,
	mmFBC_MISC, 0x9f313fff, 0x14302008,
	mmHDMI_CONTROL, 0x313f031f, 0x00000011,
};

/* dce_v11_0_hpd_sense */
static int dce_v11_0_hpd_sense(struct frb_device *adev, int hpd)
{
	if (hpd < 0 || hpd >= 6)
		return 0;
	return (RREG32(mmDC_HPD_INT_STATUS + hpd_offsets[hpd]) & DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK) != 0;
}

/* DC_GPIO_HPD_Y's bit for each line (DC_GPIO_HPD1_Y .. HPD6_Y: not evenly spaced) */
static int dce_v11_0_hpd_pad(struct frb_device *adev, int hpd)
{
	static const uint32_t y[6] = { 0x1, 0x100, 0x10000, 0x1000000, 0x4000000, 0x10000000 };

	if (hpd < 0 || hpd >= 6)
		return -1;
	return (RREG32(mmDC_GPIO_HPD_Y) & y[hpd]) != 0;
}

/* dce_v11_0_set_vga_render_state */
static void set_vga_render_state(struct frb_device *adev, int render)
{
	uint32_t tmp;

	tmp = RREG32(mmVGA_HDP_CONTROL);
	tmp = REG_SET_FIELD(tmp, VGA_HDP_CONTROL, VGA_MEMORY_DISABLE, render ? 0 : 1);
	WREG32(mmVGA_HDP_CONTROL, tmp);
	tmp = RREG32(mmVGA_RENDER_CONTROL);
	tmp = REG_SET_FIELD(tmp, VGA_RENDER_CONTROL, VGA_VSTATUS_CNTL, render ? 1 : 0);
	WREG32(mmVGA_RENDER_CONTROL, tmp);
}

/* dce_v11_0_hpd_init, for the connectors the object table names */
static void hpd_init(struct frb_device *adev, struct frb_display *d)
{
	uint32_t tmp;
	int i;

	for (i = 0; i < d->num_connectors; i++) {
		int hpd = d->conn[i].hpd;

		if (hpd < 0 || hpd >= (int)d->num_hpd)
			continue;
		tmp = RREG32(mmDC_HPD_CONTROL + hpd_offsets[hpd]);
		tmp = REG_SET_FIELD(tmp, DC_HPD_CONTROL, DC_HPD_EN, 1);
		WREG32(mmDC_HPD_CONTROL + hpd_offsets[hpd], tmp);
		tmp = RREG32(mmDC_HPD_TOGGLE_FILT_CNTL + hpd_offsets[hpd]);
		tmp = REG_SET_FIELD(tmp, DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_CONNECT_INT_DELAY, 50);
		tmp = REG_SET_FIELD(tmp, DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_DISCONNECT_INT_DELAY, 10);
		WREG32(mmDC_HPD_TOGGLE_FILT_CNTL + hpd_offsets[hpd], tmp);
		/* dce_v11_0_hpd_int_ack + set_polarity; the interrupt itself stays off (polling) */
		tmp = RREG32(mmDC_HPD_INT_CONTROL + hpd_offsets[hpd]);
		tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_INT_ACK, 1);
		WREG32(mmDC_HPD_INT_CONTROL + hpd_offsets[hpd], tmp);
		tmp = RREG32(mmDC_HPD_INT_CONTROL + hpd_offsets[hpd]);
		tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY,
				    dce_v11_0_hpd_sense(adev, hpd) ? 0 : 1);
		WREG32(mmDC_HPD_INT_CONTROL + hpd_offsets[hpd], tmp);
	}
}

/* dce_v11_0_hw_init (+ early/sw_init's counts and the object table) */
static int dce_v11_0_hw_init(struct frb_device *adev, struct frb_display *d)
{
	const uint8_t *bios = adev->bios;
	uint16_t off;
	int r;

	memset(d, 0, sizeof(*d));
	d->num_crtc = 5;	/* POLARIS11/12 */
	d->num_hpd = 5;
	d->num_dig = 5;
	d->crtc.crtc_id = 0;
	d->crtc.crtc_offset = crtc_offsets[0];
	d->crtc.pll_id = ATOM_PPLL_INVALID;
	/* drm_mode_crtc_set_gamma_size(crtc, 256): the linear ramp, i << 8 */
	for (r = 0; r < 256; r++)
		d->crtc.gamma[0][r] = d->crtc.gamma[1][r] = d->crtc.gamma[2][r] = (uint16_t)(r << 8);
	/* amdgpu_atombios_get_clock_info: default_dispclk, dp_extclk */
	if (frb_atom_parse_data_header(adev->atom, GetIndexIntoMasterTable(DATA, FirmwareInfo), NULL, NULL, NULL, &off)) {
		d->default_dispclk = frb_get_le32(bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, ulDefaultDispEngineClkFreq));
		d->dp_extclk = frb_get_le16(bios, off + offsetof(ATOM_FIRMWARE_INFO_V2_2, usUniphyDPModeExtClkFreq));
	}
	/* amdgpu_atombios_get_clock_info: "set a reasonable default for DP" */
	if (d->default_dispclk < 53900)
		d->default_dispclk = 60000;
	else if (d->default_dispclk <= 60000)
		d->default_dispclk = 62500;
	d->current_sclk = adev->default_sclk;
	d->current_mclk = adev->default_mclk;
	/* adev->pm.pm_display_cfg before any display: no CRTC, vblank "max" */
	d->pm_cfg.crtc_index = d->num_crtc;
	d->pm_cfg.min_vblank_time = 0xffffffff;

	r = frb_atombios_get_connector_info(adev, d);
	if (r)
		return r;

	frb_device_program_register_sequence(adev, polaris11_golden_settings_a11,
					     ARRAY_SIZE(polaris11_golden_settings_a11));
	set_vga_render_state(adev, 0);
	frb_atombios_crtc_powergate_init(adev);
	/*
	 * DC's init_hw after the pipe power gating: dce_clock_gating_power_up(hws,
	 * false) -> dce_disable_sram_shut_down, DC_MEM_GLOBAL_PWR_REQ_DIS = 1 (the
	 * DCE 11.2 hwseq list has no DCFEV_CLOCK_CONTROL, so its underlay clock half
	 * does nothing here). Without it the DCO memories sit in light sleep
	 * (DCO_MEM_PWR_STATUS 0x554d8 on the RX 550), and the AFMT audio FIFO reads
	 * out a fixed pattern whatever goes in (FIFO output CRC L 7807ce, R 0):
	 * DP audio is silent. With it the FIFO output CRC follows the samples, as
	 * under Linux DC on the same card (docs/dp-audio.md).
	 */
	WREG32(mmDC_MEM_GLOBAL_PWR_REQ_CNTL,
	       REG_SET_FIELD(RREG32(mmDC_MEM_GLOBAL_PWR_REQ_CNTL), DC_MEM_GLOBAL_PWR_REQ_CNTL,
			     DC_MEM_GLOBAL_PWR_REQ_DIS, 1));
	frb_atombios_encoder_init_dig(adev, d);
	frb_atombios_crtc_set_dce_clock(adev, d->default_dispclk, DCE_CLOCK_TYPE_DISPCLK, ATOM_GCK_DFS);
	frb_atombios_crtc_set_dce_clock(adev, 0, DCE_CLOCK_TYPE_DPREFCLK, ATOM_GCK_DFS);
	hpd_init(adev, d);
	/* DP audio's init after the display engine clocks, as RadeonNI's rdn_dce8_afmt_init (FirebrandAudio) */
	if (adev->want_audio)
		frb_audio_hw_init(adev, d);
	/* not carried: pageflip_interrupt_init (no IH ring; flips and vblank are polled) */
	d->ready = 1;
	return 0;
}

/*
 * amdgpu_connector_dp_detect for every DisplayPort connector (sink type
 * from the VBIOS, hot-plug sense, the DPCD; the BIOS scratch bits), then
 * amdgpu_connector_get_edid's DDC-over-AUX read for connected DP sinks, and
 * for DVI/HDMI connectors the DDC probe and EDID (amdgpu_connector_dvi_detect).
 * Returns the first connected connector, or -1.
 */
static int dce_v11_0_detect(struct frb_device *adev, struct frb_display *d)
{
	int i, found = -1;

	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];
		int connected = 0, sense;
		uint8_t was = c->detect_logged;

		c->edid_len = 0;
		c->sink_tmds = 0;
		c->is_hdmi = 0;
		if (frb_connector_is_tmds(c)) {
			/*
			 * amdgpu_connector_dvi_detect: a DDC probe, then the
			 * EDID; digital input (byte 20 bit 7) is a TMDS sink.
			 * The HPD line is only logged (amdgpu decides on DDC).
			 */
			sense = dce_v11_0_hpd_sense(adev, c->hpd);
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
		sense = dce_v11_0_hpd_sense(adev, c->hpd);
		if (sense) {
			connected = 1;
			if (c->dp_sink_type == CONNECTOR_OBJECT_ID_DISPLAYPORT)
				frb_dp_get_dpcd(adev, c);
		} else if (c->dp_sink_type == CONNECTOR_OBJECT_ID_DISPLAYPORT) {
			if (!frb_dp_get_dpcd(adev, c))
				connected = 1;
		}
		/* (the non-AUX DDC probe for DP-to-DVI/HDMI adapters is not carried) */
		frb_atombios_encoder_set_bios_scratch_regs(adev, c, connected);
		if (!connected) {
			c->detect_logged = 4;
			if (c->detect_logged != was)
				frb_info(adev, "display: connector %d (hpd %d): nothing (sink type 0x%x, pad %d)", i, c->hpd,
					 c->dp_sink_type, dce_v11_0_hpd_pad(adev, c->hpd));
			continue;
		}
		if (c->dp_sink_type != CONNECTOR_OBJECT_ID_DISPLAYPORT || !c->dpcd[0]) {
			/*
			 * The HPD line is up and nothing answers on AUX: no
			 * DisplayPort sink (the kernel log of one machine had the RX
			 * 550's connector 0 like this for hours; with the cable
			 * out its pad reads low). Not a monitor; the hot-plug
			 * poll keeps asking.
			 */
			c->detect_logged = 5;
			if (c->detect_logged != was)
				frb_info(adev, "display: connector %d (hpd %d): HPD line up (pad %d, DC_HPD_INT_STATUS %08x) "
					 "but no DPCD answers: no DisplayPort sink", i, c->hpd,
					 dce_v11_0_hpd_pad(adev, c->hpd),
					 c->hpd >= 0 && c->hpd < 6 ? RREG32(mmDC_HPD_INT_STATUS + hpd_offsets[c->hpd]) : 0);
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

/* dce_v11_0_hpd_fini */
static void hpd_fini(struct frb_device *adev, struct frb_display *d)
{
	uint32_t tmp;
	int i;

	for (i = 0; i < d->num_connectors; i++) {
		int hpd = d->conn[i].hpd;

		if (hpd < 0 || hpd >= (int)d->num_hpd)
			continue;
		tmp = RREG32(mmDC_HPD_CONTROL + hpd_offsets[hpd]);
		tmp = REG_SET_FIELD(tmp, DC_HPD_CONTROL, DC_HPD_EN, 0);
		WREG32(mmDC_HPD_CONTROL + hpd_offsets[hpd], tmp);
	}
}

/* dce_v11_0_crtc_load_lut, from the CRTC's gamma store */
static void dce_v11_0_load_lut(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t o = c->crtc_offset, tmp;
	int i;

	tmp = RREG32(mmINPUT_CSC_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, INPUT_CSC_CONTROL, INPUT_CSC_GRPH_MODE, 0);
	WREG32(mmINPUT_CSC_CONTROL + o, tmp);
	tmp = RREG32(mmPRESCALE_GRPH_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, PRESCALE_GRPH_CONTROL, GRPH_PRESCALE_BYPASS, 1);
	WREG32(mmPRESCALE_GRPH_CONTROL + o, tmp);
	tmp = RREG32(mmINPUT_GAMMA_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, INPUT_GAMMA_CONTROL, GRPH_INPUT_GAMMA_MODE, 0);
	WREG32(mmINPUT_GAMMA_CONTROL + o, tmp);
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
	/*
	 * The ramp every CRTC starts with is i << 8 (hw_init), so the 10-bit
	 * entries are exactly i << 2. (An i << 8 | i ramp puts 11 in the low
	 * bits from 0xc0 on, and the scanout's reduction to 8 bpc rounds those
	 * up by one: the sink CRC showed it.)
	 */
	for (i = 0; i < 256; i++)
		WREG32(mmDC_LUT_30_COLOR + o, ((uint32_t)(c->gamma[0][i] & 0xffc0) << 14) |
		       ((uint32_t)(c->gamma[1][i] & 0xffc0) << 4) | (c->gamma[2][i] >> 6));
	tmp = RREG32(mmDEGAMMA_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, DEGAMMA_CONTROL, GRPH_DEGAMMA_MODE, 0);
	tmp = REG_SET_FIELD(tmp, DEGAMMA_CONTROL, CURSOR_DEGAMMA_MODE, 0);
	tmp = REG_SET_FIELD(tmp, DEGAMMA_CONTROL, CURSOR2_DEGAMMA_MODE, 0);
	WREG32(mmDEGAMMA_CONTROL + o, tmp);
	tmp = RREG32(mmGAMUT_REMAP_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, GAMUT_REMAP_CONTROL, GRPH_GAMUT_REMAP_MODE, 0);
	WREG32(mmGAMUT_REMAP_CONTROL + o, tmp);
	tmp = RREG32(mmREGAMMA_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, REGAMMA_CONTROL, GRPH_REGAMMA_MODE, 0);
	WREG32(mmREGAMMA_CONTROL + o, tmp);
	tmp = RREG32(mmOUTPUT_CSC_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, OUTPUT_CSC_CONTROL, OUTPUT_CSC_GRPH_MODE, 0);
	WREG32(mmOUTPUT_CSC_CONTROL + o, tmp);
	WREG32(mmDENORM_CONTROL + o, 0);
	tmp = RREG32(mmALPHA_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, ALPHA_CONTROL, CURSOR_ALPHA_BLND_ENA, 1);
	WREG32(mmALPHA_CONTROL + o, tmp);
}

static int dce_v11_0_read_lut(struct frb_device *adev, struct frb_display *d, uint32_t *out)
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

/* dce_v11_0_program_fmt, 8 bpc, no dither: truncate to 8 */
static void program_fmt(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp = 0;

	tmp = REG_SET_FIELD(tmp, FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN, 1);
	tmp = REG_SET_FIELD(tmp, FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH, 1);
	WREG32(mmFMT_BIT_DEPTH_CONTROL + c->crtc_offset, tmp);
}

/* --- line buffer and watermarks (dce_v11_0.c, drm_fixed.h 20.12) --------- */

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

struct dce10_wm_params {
	uint32_t dram_channels, yclk, sclk, disp_clk, src_width, active_time, blank_time;
	int interlaced;
	fixed20_12 vsc;
	uint32_t num_heads, bytes_per_pixel, lb_size, vtaps;
};

static uint32_t dram_channels(struct frb_device *adev)
{
	switch ((RREG32(mmMC_SHARED_CHMAP_GMC) & MC_SHARED_CHMAP__NOOFCHAN_MASK_GMC) >> MC_SHARED_CHMAP__NOOFCHAN__SHIFT_GMC) {
	case 0: default: return 1;
	case 1: return 2;
	case 2: return 4;
	case 3: return 8;
	case 4: return 3;
	case 5: return 6;
	case 6: return 10;
	case 7: return 12;
	case 8: return 16;
	}
}

static uint32_t dce_v11_0_dram_bandwidth(struct dce10_wm_params *wm)
{
	fixed20_12 dram_efficiency, yclk, dram_ch, bandwidth, a;

	a.full = dfixed_const(1000);
	yclk.full = dfixed_const(wm->yclk);
	yclk.full = dfixed_div(yclk, a);
	dram_ch.full = dfixed_const(wm->dram_channels * 4);
	a.full = dfixed_const(10);
	dram_efficiency.full = dfixed_const(7);
	dram_efficiency.full = dfixed_div(dram_efficiency, a);
	bandwidth.full = dfixed_mul(dram_ch, yclk);
	bandwidth.full = dfixed_mul(bandwidth, dram_efficiency);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v11_0_data_return_bandwidth(struct dce10_wm_params *wm)
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

static uint32_t dce_v11_0_dmif_request_bandwidth(struct dce10_wm_params *wm)
{
	fixed20_12 eff, disp_clk, bandwidth, a, b;

	a.full = dfixed_const(1000);
	disp_clk.full = dfixed_const(wm->disp_clk);
	disp_clk.full = dfixed_div(disp_clk, a);
	a.full = dfixed_const(32);
	b.full = dfixed_mul(a, disp_clk);
	a.full = dfixed_const(10);
	eff.full = dfixed_const(8);
	eff.full = dfixed_div(eff, a);
	bandwidth.full = dfixed_mul(b, eff);
	return dfixed_trunc(bandwidth);
}

static uint32_t dce_v11_0_available_bandwidth(struct dce10_wm_params *wm)
{
	return min_u32(dce_v11_0_dram_bandwidth(wm),
		       min_u32(dce_v11_0_data_return_bandwidth(wm), dce_v11_0_dmif_request_bandwidth(wm)));
}

static uint32_t dce_v11_0_latency_watermark(struct dce10_wm_params *wm)
{
	uint32_t mc_latency = 2000;
	uint32_t available_bandwidth = dce_v11_0_available_bandwidth(wm);
	uint32_t worst_chunk_return_time, cursor_line_pair_return_time, dc_latency;
	uint32_t other_heads_data_return_time, latency, max_src_lines_per_dst_line;
	uint32_t lb_fill_bw, line_fill_time, tmp, dmif_size = 12288;
	fixed20_12 a, b, c;

	if (wm->num_heads == 0 || !available_bandwidth || !wm->disp_clk)
		return 0;
	worst_chunk_return_time = (512 * 8 * 1000) / available_bandwidth;
	cursor_line_pair_return_time = (128 * 4 * 1000) / available_bandwidth;
	dc_latency = 40000000 / wm->disp_clk;
	other_heads_data_return_time = ((wm->num_heads + 1) * worst_chunk_return_time) +
		(wm->num_heads * cursor_line_pair_return_time);
	latency = mc_latency + other_heads_data_return_time + dc_latency;
	a.full = dfixed_const(2);
	b.full = dfixed_const(1);
	if ((wm->vsc.full > a.full) || ((wm->vsc.full > b.full) && (wm->vtaps >= 3)) ||
	    (wm->vtaps >= 5) || ((wm->vsc.full >= a.full) && wm->interlaced))
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

/* dce_v11_0_line_buffer_adjust */
static uint32_t line_buffer_adjust(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp, buffer_alloc, mem_cfg, i, pipe_offset = c->crtc_id;

	if (c->base_enabled) {
		if (c->mode.hdisplay < 1920) {
			mem_cfg = 1;
			buffer_alloc = 2;
		} else if (c->mode.hdisplay < 2560) {
			mem_cfg = 2;
			buffer_alloc = 2;
		} else {
			mem_cfg = 0;	/* (>= 4096 is "too big for LB" in amdgpu, same settings) */
			buffer_alloc = 4;
		}
	} else {
		mem_cfg = 1;
		buffer_alloc = 0;
	}
	tmp = RREG32(mmLB_MEMORY_CTRL + c->crtc_offset);
	tmp = REG_SET_FIELD(tmp, LB_MEMORY_CTRL, LB_MEMORY_CONFIG, mem_cfg);
	WREG32(mmLB_MEMORY_CTRL + c->crtc_offset, tmp);
	tmp = RREG32(mmPIPE0_DMIF_BUFFER_CONTROL + pipe_offset);
	tmp = REG_SET_FIELD(tmp, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATED, buffer_alloc);
	WREG32(mmPIPE0_DMIF_BUFFER_CONTROL + pipe_offset, tmp);
	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(mmPIPE0_DMIF_BUFFER_CONTROL + pipe_offset);
		if (REG_GET_FIELD(tmp, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATION_COMPLETED))
			break;
		udelay(1);
	}
	if (!c->base_enabled)
		return 0;
	switch (mem_cfg) {
	case 0: default: return 4096 * 2;
	case 1: return 1920 * 2;
	case 2: return 2560 * 2;
	}
}

/*
 * dce_v11_0_program_watermarks: watermark A for the high clocks, B for the
 * low ones. With DPM on they are the requested power state's highest and
 * lowest levels (amdgpu_dpm_get_mclk/sclk(adev, false/true)); without it
 * both are the boot clocks (adev->pm.current_mclk/sclk).
 */
static uint32_t wm_latency(struct frb_device *adev, struct frb_crtc *c, uint32_t lb_size, uint32_t num_heads,
			   uint32_t active_time, uint32_t line_time, uint32_t yclk, uint32_t sclk)
{
	struct dce10_wm_params wm;

	memset(&wm, 0, sizeof(wm));
	wm.yclk = yclk;
	wm.sclk = sclk;
	wm.disp_clk = c->mode.clock;
	wm.src_width = c->mode.hdisplay;
	wm.active_time = active_time;
	wm.blank_time = line_time - active_time;
	wm.vsc.full = dfixed_const(1);
	wm.vtaps = 1;
	wm.bytes_per_pixel = 4;
	wm.lb_size = lb_size;
	wm.dram_channels = dram_channels(adev);
	wm.num_heads = num_heads;
	return min_u32(dce_v11_0_latency_watermark(&wm), 65535);
}

static void program_watermarks(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c,
			       uint32_t lb_size, uint32_t num_heads)
{
	uint32_t active_time, line_time = 0, latency_a = 0, latency_b = 0, tmp, wm_mask, lead = 0;

	if (c->base_enabled && num_heads && c->mode.clock) {
		active_time = (uint32_t)(((uint64_t)c->mode.hdisplay * 1000000) / c->mode.clock);
		line_time = (uint32_t)(((uint64_t)c->mode.htotal * 1000000) / c->mode.clock);
		line_time = min_u32(line_time, 65535);
		/* watermark for high clocks */
		if (frb_pm_enabled(adev))
			latency_a = wm_latency(adev, c, lb_size, num_heads, active_time, line_time,
					       frb_pm_get_mclk(adev, 0) * 10, frb_pm_get_sclk(adev, 0) * 10);
		else
			latency_a = wm_latency(adev, c, lb_size, num_heads, active_time, line_time,
					       d->current_mclk * 10, d->current_sclk * 10);
		/* watermark for low clocks */
		if (frb_pm_enabled(adev))
			latency_b = wm_latency(adev, c, lb_size, num_heads, active_time, line_time,
					       frb_pm_get_mclk(adev, 1) * 10, frb_pm_get_sclk(adev, 1) * 10);
		else
			latency_b = wm_latency(adev, c, lb_size, num_heads, active_time, line_time,
					       d->current_mclk * 10, d->current_sclk * 10);
		lead = (lb_size + c->mode.hdisplay - 1) / c->mode.hdisplay;
	}
	/* select wm A */
	wm_mask = RREG32(mmDPG_WATERMARK_MASK_CONTROL + c->crtc_offset);
	tmp = REG_SET_FIELD(wm_mask, DPG_WATERMARK_MASK_CONTROL, URGENCY_WATERMARK_MASK, 1);
	WREG32(mmDPG_WATERMARK_MASK_CONTROL + c->crtc_offset, tmp);
	tmp = RREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset);
	tmp = REG_SET_FIELD(tmp, DPG_PIPE_URGENCY_CONTROL, URGENCY_LOW_WATERMARK, latency_a);
	tmp = REG_SET_FIELD(tmp, DPG_PIPE_URGENCY_CONTROL, URGENCY_HIGH_WATERMARK, line_time);
	WREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset, tmp);
	/* select wm B */
	tmp = REG_SET_FIELD(wm_mask, DPG_WATERMARK_MASK_CONTROL, URGENCY_WATERMARK_MASK, 2);
	WREG32(mmDPG_WATERMARK_MASK_CONTROL + c->crtc_offset, tmp);
	tmp = RREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset);
	tmp = REG_SET_FIELD(tmp, DPG_PIPE_URGENCY_CONTROL, URGENCY_LOW_WATERMARK, latency_b);
	tmp = REG_SET_FIELD(tmp, DPG_PIPE_URGENCY_CONTROL, URGENCY_HIGH_WATERMARK, line_time);
	WREG32(mmDPG_PIPE_URGENCY_CONTROL + c->crtc_offset, tmp);
	/* restore original selection */
	WREG32(mmDPG_WATERMARK_MASK_CONTROL + c->crtc_offset, wm_mask);
	/* save values for DPM */
	c->line_time = line_time;
	c->wm_high = latency_a;
	c->wm_low = latency_b;
	c->lb_vblank_lead_lines = lead;
}

/*
 * dce_v11_0_bandwidth_update: every CRTC's line buffer and watermarks; the
 * ones without a mode get no line buffer and zero watermarks. Only CRTC0
 * carries state here, the others are described on the fly.
 */
static void bandwidth_update(struct frb_device *adev, struct frb_display *d)
{
	uint32_t num_heads = d->crtc.base_enabled ? 1 : 0, lb_size, i;

	/* the clocks the watermarks are for (adev->pm.current_sclk/mclk, or DPM's top level) */
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
			frb_info(adev, "display: line buffer %u px, watermarks %u/%u ns, line time %u ns (top sclk %u kHz, "
				 "mclk %u kHz%s)", lb_size, c->wm_high, c->wm_low, c->line_time, d->current_sclk * 10,
				 d->current_mclk * 10, frb_pm_enabled(adev) ? ", DPM" : "");
	}
}

/* dce_v11_0_vga_enable / grph_enable */
static void vga_enable(struct frb_device *adev, struct frb_crtc *c, int enable)
{
	uint32_t v = RREG32(vga_control_regs[c->crtc_id]) & ~1u;

	WREG32(vga_control_regs[c->crtc_id], enable ? (v | 1) : v);
}

/*
 * dce_v11_0_crtc_do_set_base: a linear surface at fb_offset in VRAM, in
 * one of the formats it takes for DRM_FORMAT_C8 (8 bpp, indexed through the
 * LUT), XRGB1555 and XRGB8888; on a big-endian host the pixels are swapped
 * 8 in 16 and 8 in 32 as there (__BIG_ENDIAN).
 */
static void crtc_do_set_base(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c)
{
	uint64_t fb_location = adev->gmc.vram_start + c->fb_offset;
	uint32_t fb_format, fb_swap, tmp, o = c->crtc_offset;
	uint32_t depth = c->fb_bpp == 8 ? 0 : c->fb_bpp == 16 ? 1 : 2;

	fb_format = REG_SET_FIELD(0, GRPH_CONTROL, GRPH_DEPTH, depth);
	fb_format = REG_SET_FIELD(fb_format, GRPH_CONTROL, GRPH_FORMAT, 0);	/* indexed, ARGB1555, ARGB8888 */
	fb_swap = REG_SET_FIELD(0, GRPH_SWAP_CNTL, GRPH_ENDIAN_SWAP, ENDIAN_NONE);
#if FRB_BIG_ENDIAN
	if (depth)
		fb_swap = REG_SET_FIELD(fb_swap, GRPH_SWAP_CNTL, GRPH_ENDIAN_SWAP,
					depth == 1 ? ENDIAN_8IN16 : ENDIAN_8IN32);
#endif
	/* ARRAY_LINEAR_GENERAL (0); GRPH_PIPE_CONFIG from the tiling flags: 0 for linear */
	vga_enable(adev, c, 0);
	tmp = RREG32(mmGRPH_FLIP_CONTROL + o);
	tmp = REG_SET_FIELD(tmp, GRPH_FLIP_CONTROL, GRPH_SURFACE_UPDATE_H_RETRACE_EN, 0);
	WREG32(mmGRPH_FLIP_CONTROL + o, tmp);
	WREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + o, (uint32_t)(fb_location >> 32));
	WREG32(mmGRPH_SECONDARY_SURFACE_ADDRESS_HIGH + o, (uint32_t)(fb_location >> 32));
	WREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS + o,
	       (uint32_t)fb_location & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
	WREG32(mmGRPH_SECONDARY_SURFACE_ADDRESS + o,
	       (uint32_t)fb_location & GRPH_SECONDARY_SURFACE_ADDRESS__GRPH_SECONDARY_SURFACE_ADDRESS_MASK);
	WREG32(mmGRPH_CONTROL + o, fb_format);
	WREG32(mmGRPH_SWAP_CNTL + o, fb_swap);
	tmp = RREG32(mmGRPH_LUT_10BIT_BYPASS + o);
	tmp = REG_SET_FIELD(tmp, GRPH_LUT_10BIT_BYPASS, GRPH_LUT_10BIT_BYPASS_EN, 0);
	WREG32(mmGRPH_LUT_10BIT_BYPASS + o, tmp);
	WREG32(mmGRPH_SURFACE_OFFSET_X + o, 0);
	WREG32(mmGRPH_SURFACE_OFFSET_Y + o, 0);
	WREG32(mmGRPH_X_START + o, 0);
	WREG32(mmGRPH_Y_START + o, 0);
	WREG32(mmGRPH_X_END + o, c->fb_width);
	WREG32(mmGRPH_Y_END + o, c->fb_height);
	WREG32(mmGRPH_PITCH + o, c->fb_pitch);
	WREG32(mmGRPH_ENABLE + o, 1);
	WREG32(mmLB_DESKTOP_HEIGHT + o, c->fb_height);
	WREG32(mmVIEWPORT_START + o, 0);
	WREG32(mmVIEWPORT_SIZE + o, ((uint32_t)c->mode.hdisplay << 16) | ((c->mode.vdisplay + 1) & ~1u));
	WREG32(mmCRTC_MASTER_UPDATE_MODE + o, 0);
	bandwidth_update(adev, d);
}

/*
 * amdgpu_dpm_get_display_cfg (amdgpu_dpm_internal.c), the legacy path's
 * display configuration for powerplay: the active CRTCs, the shortest
 * vblank, the first one's refresh rate. Kept in the display state, as
 * adev->pm.pm_display_cfg is, so vrefresh holds its first value.
 */
static void amdgpu_dpm_get_display_cfg(struct frb_device *adev, struct frb_display *d)
{
	struct amd_pp_display_configuration *cfg = &d->pm_cfg;
	struct frb_crtc *c = &d->crtc;
	int num_crtcs = 0;

	cfg->min_vblank_time = 0xffffffff; /* if the displays are off, vblank time is max */
	if (c->enabled) {
		struct single_display_configuration *display_cfg = &cfg->displays[num_crtcs++];

		if (c->mode.clock) {
			uint32_t vrefresh = frb_mode_hz(&c->mode);
			/* crtc_vblank_end - crtc_vdisplay (progressive: vtotal), no borders */
			uint32_t vblank_in_pixels = (uint32_t)c->mode.htotal * (c->mode.vtotal - c->mode.vdisplay);
			uint32_t vblank_time_us = vblank_in_pixels * 1000 / c->mode.clock;

			/* The legacy (non-DC) code has issues with mclk switching
			 * with refresh rates over 120 Hz. Disable mclk switching.
			 */
			if (vrefresh > 120)
				vblank_time_us = 0;
			/* Find minimum vblank time. */
			if (vblank_time_us < cfg->min_vblank_time)
				cfg->min_vblank_time = vblank_time_us;
			/* Find vertical refresh rate of first active display. */
			if (!cfg->vrefresh)
				cfg->vrefresh = vrefresh;
		}
		if ((uint32_t)c->crtc_id < cfg->crtc_index) {
			/* Find first active CRTC and its line time. */
			cfg->crtc_index = c->crtc_id;
			cfg->line_time_in_us = c->line_time;
		}
		display_cfg->controller_id = c->crtc_id;
		display_cfg->pixel_clock = c->conn ? c->conn->pixel_clock : 0;
	}
	cfg->display_clk = d->default_dispclk;	/* adev->clock.default_dispclk */
	cfg->num_display = num_crtcs;
}

/*
 * amdgpu_dpm_compute_clocks: the watermarks for the DPM clocks, then
 * powerplay's display-configuration task (the display gap, MCLK switching
 * allowed or not, HasDisplay/NoDisplay, the VBI timeout). amdgpu also waits
 * for its rings to go idle here; Firebrand's display changes come from
 * frbctl or IOFramebuffer with no engine work of their own.
 */
static void amdgpu_dpm_compute_clocks(struct frb_device *adev, struct frb_display *d)
{
	if (!frb_pm_enabled(adev))
		return;
	bandwidth_update(adev, d);
	amdgpu_dpm_get_display_cfg(adev, d);
	frb_pm_compute_clocks(adev, &d->pm_cfg);
}

/* after the DPM init: tell powerplay about a display stage dce lit before it */
static void dce_v11_0_dpm_changed(struct frb_device *adev, struct frb_display *d)
{
	amdgpu_dpm_compute_clocks(adev, d);
}

/* dce_v11_0_crtc_dpms */
static void crtc_dpms(struct frb_device *adev, struct frb_crtc *c, int on)
{
	if (on) {
		c->enabled = 1;
		frb_atombios_crtc_enable(adev, c, ATOM_ENABLE);
		vga_enable(adev, c, 1);
		frb_atombios_crtc_blank(adev, c, ATOM_DISABLE);
		vga_enable(adev, c, 0);
		dce_v11_0_load_lut(adev, c);
	} else {
		if (c->enabled) {
			vga_enable(adev, c, 1);
			frb_atombios_crtc_blank(adev, c, ATOM_ENABLE);
			vga_enable(adev, c, 0);
		}
		frb_atombios_crtc_enable(adev, c, ATOM_DISABLE);
		c->enabled = 0;
	}
	/* adjust pm to dpms */
	if (adev->display)
		amdgpu_dpm_compute_clocks(adev, adev->display);
}

/* dce_v11_0_pick_dig_encoder */
/*
 * dce_v11_0_pick_pll, the Polaris arm: the DP DTO for DP, else the PHY's own
 * COMBOPHY PLL (UNIPHY A/B 0/1, C/D 2/3, E/F 4/5).
 */
static int pick_pll(struct frb_connector *conn)
{
	if (frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP)
		return ATOM_DP_DTO;
	switch (conn->encoder_id) {
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:		return conn->linkb ? ATOM_COMBOPHY_PLL1 : ATOM_COMBOPHY_PLL0;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:	return conn->linkb ? ATOM_COMBOPHY_PLL3 : ATOM_COMBOPHY_PLL2;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:	return conn->linkb ? ATOM_COMBOPHY_PLL5 : ATOM_COMBOPHY_PLL4;
	default:					return ATOM_PPLL_INVALID;
	}
}

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

/*
 * drm_crtc_helper_set_mode with dce_v11_0's helpers, for one DP connector on
 * CRTC0. max_link_rate (kHz, 0 = the sink's) lets a first light run slower.
 */
static void dce_v11_0_cursor_reset(struct frb_device *adev, struct frb_crtc *c);
static void dce_v11_0_off(struct frb_device *adev, struct frb_display *d);
static void dce_v11_0_report(struct frb_device *adev, struct frb_display *d);

static int dce_v11_0_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			 const struct frb_mode *mode, uint32_t fb_offset, uint32_t pitch_pixels,
			 uint32_t bpp, uint32_t max_link_rate)
{
	struct frb_crtc *c = &d->crtc;
	uint64_t bytes = (uint64_t)pitch_pixels * (bpp / 8) * mode->vdisplay;
	int is_dp, tr;

	if (!d->ready || !conn)
		return -EINVAL;
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
		dce_v11_0_off(adev, d);
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
	/* crtc mode_fixup: prepare_pll, then pick_pll (Polaris: the DP DTO, or the PHY's PLL) */
	frb_atombios_crtc_prepare_pll(adev, d, c);
	c->pll_id = pick_pll(conn);

	/* encoder prepare */
	conn->dig_encoder = pick_dig_encoder(conn);
	frb_atombios_scratch_regs_lock(adev, 1);
	frb_atombios_encoder_set_crtc_source(adev, c);
	program_fmt(adev, c);
	/* crtc prepare */
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
	frb_atombios_crtc_program_pll(adev, c->crtc_id, c->pll_id, frb_atombios_encoder_get_encoder_mode(conn),
				      conn->encoder_id, c->mode.clock, c->bpc,
				      frb_dig_monitor_is_duallink(conn, c->mode.clock));
	frb_atombios_crtc_set_dtd_timing(adev, c, &c->mode);
	crtc_do_set_base(adev, d, c);
	frb_atombios_crtc_overscan_setup(adev, c);
	frb_atombios_crtc_scaler_setup(adev, c);
	dce_v11_0_cursor_reset(adev, c);
	/* encoder mode_set */
	conn->pixel_clock = c->mode.clock;
	/* RadeonNI's rdn_modeset: the DIG's audio off before DP_VIDEO_OFF and the transmitter */
	if (adev->want_audio && is_dp)
		frb_audio_dp_enable(adev, d, conn->dig_encoder, false);
	frb_atombios_encoder_dpms(adev, c, 0);
	/* ... and set up for this mode while the transmitter is off (rdn_dce8_afmt_dp_mode_set) */
	if (adev->want_audio && is_dp) {
		int ar = frb_audio_dp_mode_set(adev, d, c->crtc_id, conn->dig_encoder, &c->mode,
					       conn->edid, conn->edid_len);

		if (ar)
			frb_err(adev, "audio: not set up (%d)", ar);
	}
	{	/* dce_v11_0_set_interleave: progressive */
		uint32_t tmp = RREG32(mmLB_DATA_FORMAT + c->crtc_offset);

		tmp = REG_SET_FIELD(tmp, LB_DATA_FORMAT, INTERLEAVE_EN, 0);
		WREG32(mmLB_DATA_FORMAT + c->crtc_offset, tmp);
	}
	/* crtc commit */
	crtc_dpms(adev, c, 1);
	frb_atombios_crtc_lock(adev, c, ATOM_DISABLE);
	/* encoder commit */
	tr = frb_atombios_encoder_dpms(adev, c, 1);
	/* RadeonNI: the audio packets and the pin's presence once the link trained and the video is on */
	if (adev->want_audio && is_dp && !tr)
		frb_audio_dp_enable(adev, d, conn->dig_encoder, true);
	frb_atombios_scratch_regs_lock(adev, 0);
	frb_dp_link_report(adev, conn);
	dce_v11_0_report(adev, d);
	return 0;
}

/* --- the hardware cursor (dce_v11_0.c, Linux 6.16; dce_v10_0.c is the same in 7.1) --- */

static void dce_v11_0_lock_cursor(struct frb_device *adev, struct frb_crtc *c, int lock)
{
	uint32_t cur_lock;

	cur_lock = RREG32(mmCUR_UPDATE + c->crtc_offset);
	if (lock)
		cur_lock = REG_SET_FIELD(cur_lock, CUR_UPDATE, CURSOR_UPDATE_LOCK, 1);
	else
		cur_lock = REG_SET_FIELD(cur_lock, CUR_UPDATE, CURSOR_UPDATE_LOCK, 0);
	WREG32(mmCUR_UPDATE + c->crtc_offset, cur_lock);
}

static void dce_v11_0_hide_cursor(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp;

	tmp = RREG32(mmCUR_CONTROL + c->crtc_offset);
	tmp = REG_SET_FIELD(tmp, CUR_CONTROL, CURSOR_EN, 0);
	WREG32(mmCUR_CONTROL + c->crtc_offset, tmp);
}

/* CURSOR_MODE 2: 24.8 ARGB, premultiplied alpha */
static void dce_v11_0_show_cursor(struct frb_device *adev, struct frb_crtc *c)
{
	uint32_t tmp;

	WREG32(mmCUR_SURFACE_ADDRESS_HIGH + c->crtc_offset, (uint32_t)(c->cursor_addr >> 32));
	WREG32(mmCUR_SURFACE_ADDRESS + c->crtc_offset, (uint32_t)c->cursor_addr);

	tmp = RREG32(mmCUR_CONTROL + c->crtc_offset);
	tmp = REG_SET_FIELD(tmp, CUR_CONTROL, CURSOR_EN, 1);
	tmp = REG_SET_FIELD(tmp, CUR_CONTROL, CURSOR_MODE, 2);
	WREG32(mmCUR_CONTROL + c->crtc_offset, tmp);
}

/* crtc->x/y, the scanout's panning offset, is 0 here */
static int dce_v11_0_cursor_move_locked(struct frb_device *adev, struct frb_crtc *c, int x, int y)
{
	int xorigin = 0, yorigin = 0;

	c->cursor_x = x;
	c->cursor_y = y;

	/* avivo cursor are offset into the total surface */
	if (x < 0) {
		xorigin = -x < FRB_CURSOR_MAX - 1 ? -x : FRB_CURSOR_MAX - 1;
		x = 0;
	}
	if (y < 0) {
		yorigin = -y < FRB_CURSOR_MAX - 1 ? -y : FRB_CURSOR_MAX - 1;
		y = 0;
	}

	WREG32(mmCUR_POSITION + c->crtc_offset, ((uint32_t)x << 16) | (uint32_t)y);
	WREG32(mmCUR_HOT_SPOT + c->crtc_offset, ((uint32_t)xorigin << 16) | (uint32_t)yorigin);
	WREG32(mmCUR_SIZE + c->crtc_offset, ((c->cursor_width - 1) << 16) | (c->cursor_height - 1));

	return 0;
}

/*
 * dce_v11_0_crtc_cursor_set2 with the cursor bo already in VRAM (the
 * framebuffer owns it) and the hot spot 0: IOFramebuffer positions the
 * image's top left. Off: bar_offset ~0u.
 */
static int dce_v11_0_cursor_set(struct frb_device *adev, struct frb_display *d, uint32_t bar_offset,
			   uint32_t width, uint32_t height)
{
	struct frb_crtc *c = &d->crtc;

	if (!d->ready)
		return -ENODEV;
	if (bar_offset == ~0u) {
		/* turn off cursor */
		dce_v11_0_hide_cursor(adev, c);
		c->cursor_addr = 0;
		return 0;
	}
	if (width < 1 || height < 1 || width > FRB_CURSOR_MAX || height > FRB_CURSOR_MAX) {
		frb_err(adev, "display: bad cursor width or height %u x %u", width, height);
		return -EINVAL;
	}
	/* the surface: FRB_CURSOR_MAX pixels a row, 4 KB aligned (a pinned bo's PAGE_SIZE) */
	if ((bar_offset & 4095) || !frb_vram_ok(adev, bar_offset, FRB_CURSOR_MAX * FRB_CURSOR_MAX * 4))
		return -ERANGE;
	c->cursor_addr = adev->gmc.vram_start + bar_offset;

	dce_v11_0_lock_cursor(adev, c, 1);
	if (width != c->cursor_width || height != c->cursor_height) {
		c->cursor_width = width;
		c->cursor_height = height;
		dce_v11_0_cursor_move_locked(adev, c, c->cursor_x, c->cursor_y);
	}
	if (c->cursor_visible)
		dce_v11_0_show_cursor(adev, c);
	dce_v11_0_lock_cursor(adev, c, 0);
	return 0;
}

/* dce_v11_0_crtc_cursor_move, and IOFramebuffer's setCursorState(visible) */
static int dce_v11_0_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible)
{
	struct frb_crtc *c = &d->crtc;

	if (!d->ready)
		return -ENODEV;
	c->cursor_visible = visible;
	if (!c->cursor_addr)
		return 0;
	dce_v11_0_lock_cursor(adev, c, 1);
	dce_v11_0_cursor_move_locked(adev, c, x, y);
	if (visible)
		dce_v11_0_show_cursor(adev, c);
	else
		dce_v11_0_hide_cursor(adev, c);
	dce_v11_0_lock_cursor(adev, c, 0);
	return 0;
}

/* dce_v11_0_cursor_reset: after a mode set, the cursor as it was */
static void dce_v11_0_cursor_reset(struct frb_device *adev, struct frb_crtc *c)
{
	if (c->cursor_addr) {
		dce_v11_0_lock_cursor(adev, c, 1);

		dce_v11_0_cursor_move_locked(adev, c, c->cursor_x, c->cursor_y);

		if (c->cursor_visible)
			dce_v11_0_show_cursor(adev, c);

		dce_v11_0_lock_cursor(adev, c, 0);
	}
}

/*
 * drm_crtc_helper's disable: encoder disable (dce_v11_0_encoder_disable),
 * then dce_v11_0_crtc_disable (dpms off, GRPH off, power gate; the DP DTO
 * has no PLL to turn off), then the line buffer is released.
 */
static void dce_v11_0_off(struct frb_device *adev, struct frb_display *d)
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
	c->pll_id = ATOM_PPLL_INVALID;
	c->adjusted_clock = 0;
	c->base_enabled = 0;
	bandwidth_update(adev, d);
	c->conn = NULL;
}

/* the scanout state, for the log: is the pipe alive */
static void dce_v11_0_report(struct frb_device *adev, struct frb_display *d)
{
	uint32_t o = d->crtc.crtc_offset, a, b;

	a = RREG32(mmCRTC_STATUS_FRAME_COUNT + o);
	udelay(20000);
	b = RREG32(mmCRTC_STATUS_FRAME_COUNT + o);
	frb_info(adev, "display: CRTC_CONTROL %08x, BLANK %08x, frames %u -> %u in 20 ms, H/V total %u/%u, "
		 "GRPH enable %x control %08x swap %08x addr %08x:%08x pitch %u, viewport %08x",
		 RREG32(mmCRTC_CONTROL + o), RREG32(mmCRTC_BLANK_CONTROL + o), a, b,
		 RREG32(mmCRTC_H_TOTAL + o) + 1, RREG32(mmCRTC_V_TOTAL + o) + 1,
		 RREG32(mmGRPH_ENABLE + o), RREG32(mmGRPH_CONTROL + o), RREG32(mmGRPH_SWAP_CNTL + o),
		 RREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + o), RREG32(mmGRPH_PRIMARY_SURFACE_ADDRESS + o),
		 RREG32(mmGRPH_PITCH + o), RREG32(mmVIEWPORT_SIZE + o));
}

/* frb_display_funcs.hpd_sense: the connector's HPD pin */
static int dce_v11_0_conn_hpd_sense(struct frb_device *adev, const struct frb_connector *c)
{
	return dce_v11_0_hpd_sense(adev, c->hpd);
}

const struct frb_display_funcs frb_dce_v11_0_funcs = {
	.name = "dce_v11_0",
	.has_audio = 1,
	.audio_regs = &frb_audio_regs_dce11,
	.cursor_max = FRB_CURSOR_MAX,
	.hpd_gpio_reg = mmDC_GPIO_HPD_A,
	.hw_init = dce_v11_0_hw_init,
	.hw_fini = hpd_fini,
	.hpd_sense = dce_v11_0_conn_hpd_sense,
	.hpd_pad = dce_v11_0_hpd_pad,
	.detect = dce_v11_0_detect,
	.set_mode = dce_v11_0_set_mode,
	.off = dce_v11_0_off,
	.dpm_changed = dce_v11_0_dpm_changed,
	.report = dce_v11_0_report,
	.load_lut = dce_v11_0_load_lut,
	.read_lut = dce_v11_0_read_lut,
	.cursor_set = dce_v11_0_cursor_set,
	.cursor_move = dce_v11_0_cursor_move,
};
