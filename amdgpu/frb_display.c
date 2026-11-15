/*
 * The display's entry points, for whichever DCE the family has: amdgpu
 * reaches each DCE file through adev->mode_info.funcs and its helper vtables;
 * Firebrand through frb_asic_funcs.display (frb_display_funcs). What is the
 * same for every DCE lives here: the stage dce frame (allocate, hw_init, the
 * first probe), the unload, the gamma store and the connectors' mode_valid.
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

static const struct frb_display_funcs *dfuncs(struct frb_device *adev)
{
	return frb_funcs(adev)->display;
}

/*
 * Stage dce: the DCE's sw_init (object table) and hw_init, then what DRM's
 * first probe does (each connector's detect and EDID).
 */
int frb_display_init(struct frb_device *adev)
{
	const struct frb_display_funcs *f = dfuncs(adev);
	struct frb_display *d = adev->display;
	int r;

	if (!f) {
		frb_err(adev, "display: no DCE port for %s", frb_funcs(adev)->name);
		return -ENOSYS;
	}
	if (adev->want_audio && !f->has_audio) {
		frb_info(adev, "display: %s has no DP audio port yet; FirebrandAudio ignored", f->name);
		adev->want_audio = 0;
	}
	if (!d) {
		d = (struct frb_display *)adev->os->alloc(adev->os->cookie, sizeof(*d));
		if (!d)
			return -ENOMEM;
		adev->display = d;
	}
	r = f->hw_init(adev, d);
	if (r)
		return r;
	f->detect(adev, d);
	return 0;
}

/*
 * The unload: amdgpu_device_fini_hw's drm_helper_force_disable_all (the
 * CRTC and its encoder off), DP audio's teardown (RadeonNI's
 * rdn_dce8_afmt_fini), then the DCE's hw_fini (hpd_fini; the pageflip
 * interrupts were never set up).
 */
void frb_display_fini(struct frb_device *adev)
{
	struct frb_display *d = adev->display;
	const struct frb_display_funcs *f = dfuncs(adev);

	if (!d)
		return;
	if (d->ready && f) {
		f->off(adev, d);
		if (adev->want_audio && f->audio_regs)
			frb_audio_fini(adev, d);	/* rdn_dce8_afmt_fini */
		if (f->hw_fini)
			f->hw_fini(adev, d);
	}
	adev->os->free(adev->os->cookie, d);
	adev->display = NULL;
}

int frb_display_hpd_sense(struct frb_device *adev, const struct frb_connector *c)
{
	return dfuncs(adev) ? dfuncs(adev)->hpd_sense(adev, c) : 0;
}

int frb_display_hpd_pad(struct frb_device *adev, int hpd)
{
	return dfuncs(adev) && dfuncs(adev)->hpd_pad ? dfuncs(adev)->hpd_pad(adev, hpd) : -1;
}

int frb_display_detect(struct frb_device *adev, struct frb_display *d)
{
	return dfuncs(adev)->detect(adev, d);
}

int frb_display_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			 const struct frb_mode *mode, uint32_t fb_offset, uint32_t pitch_pixels,
			 uint32_t bpp, uint32_t max_link_rate)
{
	return dfuncs(adev)->set_mode(adev, d, conn, mode, fb_offset, pitch_pixels, bpp, max_link_rate);
}

void frb_display_off(struct frb_device *adev, struct frb_display *d)
{
	dfuncs(adev)->off(adev, d);
}

/* after the DPM init: tell powerplay about a display stage dce lit before it */
void frb_display_dpm_changed(struct frb_device *adev)
{
	struct frb_display *d = adev->display;

	if (d && d->ready && dfuncs(adev)->dpm_changed)
		dfuncs(adev)->dpm_changed(adev, d);
}

void frb_display_report(struct frb_device *adev, struct frb_display *d)
{
	dfuncs(adev)->report(adev, d);
}

void frb_display_load_lut(struct frb_device *adev, struct frb_crtc *c)
{
	dfuncs(adev)->load_lut(adev, c);
}

int frb_display_read_lut(struct frb_device *adev, struct frb_display *d, uint32_t *out)
{
	return dfuncs(adev)->read_lut(adev, d, out);
}

/* the CRTC's gamma_set: a new gamma_store, loaded when the CRTC runs */
void frb_display_set_gamma(struct frb_device *adev, struct frb_display *d,
			   const uint16_t *r, const uint16_t *g, const uint16_t *b)
{
	struct frb_crtc *c = &d->crtc;

	memcpy(c->gamma[0], r, sizeof(c->gamma[0]));
	memcpy(c->gamma[1], g, sizeof(c->gamma[1]));
	memcpy(c->gamma[2], b, sizeof(c->gamma[2]));
	if (c->enabled)
		dfuncs(adev)->load_lut(adev, c);
}

int frb_display_cursor_set(struct frb_device *adev, struct frb_display *d, uint32_t bar_offset,
			   uint32_t width, uint32_t height)
{
	return dfuncs(adev)->cursor_set(adev, d, bar_offset, width, height);
}

uint32_t frb_display_cursor_max(struct frb_device *adev)
{
	const struct frb_display_funcs *f = dfuncs(adev);

	return f && f->cursor_max ? f->cursor_max : FRB_CURSOR_MAX;
}

int frb_display_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible)
{
	return dfuncs(adev)->cursor_move(adev, d, x, y, visible);
}

int frb_display_pipe_crc(struct frb_device *adev, struct frb_display *d, uint16_t crc[3])
{
	if (!d || !d->ready)
		return -ENODEV;
	if (!dfuncs(adev)->pipe_crc)
		return -ENOSYS;	/* DCE 6/11: the sink CRC is the check there */
	return dfuncs(adev)->pipe_crc(adev, d, crc);
}

/*
 * The connector's mode_valid (amdgpu_connector_dp_mode_valid /
 * amdgpu_connector_dvi_mode_valid), the same for every DCE here.
 */
int frb_display_mode_valid(const struct frb_connector *c, const struct frb_mode *mode, uint32_t max_rate)
{
	uint32_t rate, lanes;

	if (!c || !mode || !mode->clock || mode->clock > FRB_MODE_MAX_CLOCK)
		return 0;
	if (frb_connector_is_tmds(c)) {
		if (!c->sink_tmds)
			return 0;
		if (mode->clock <= 165000)
			return 1;
		if (c->connector_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I ||
		    c->connector_type == CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D ||
		    c->connector_type == CONNECTOR_OBJECT_ID_HDMI_TYPE_B)
			return c->is_hdmi ? mode->clock <= 340000 : mode->clock <= 330000;
		/* HDMI 1.3+ supports max clock of 340 Mhz */
		return c->is_hdmi && mode->clock <= 340000;
	}
	switch (c->dpcd[1]) {
	case 0x0a:		rate = 270000; break;
	case 0x14: case 0x1e:	rate = 540000; break;
	default:		rate = 162000; break;
	}
	if (max_rate && max_rate < rate)
		rate = max_rate;
	lanes = c->dpcd[2] & 0x1f;
	if (lanes > 4)
		lanes = 4;
	return c->dpcd[0] && (uint64_t)lanes * rate * 8 / 24 >= mode->clock;
}
