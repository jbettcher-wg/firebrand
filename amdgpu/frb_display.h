/*
 * The display: amdgpu's legacy (non-DC) DCE 11 path, which Linux carried for
 * Polaris until 6.x (amdgpu.dc=0): dce_v11_0.c with atombios_crtc.c,
 * atombios_encoders.c and atombios_dp.c. Reference: Linux 6.16
 * (drivers/gpu/drm/amd/amdgpu), the
 * last tree we have that still holds dce_v11_0.c.
 *
 *   frb_atombios_conn.c      amdgpu_atombios.c (object table, HPD GPIOs, SS)
 *   frb_atombios_crtc.c      atombios_crtc.c
 *   frb_atombios_encoders.c  atombios_encoders.c (DIG paths)
 *   frb_atombios_dp.c        atombios_dp.c (AUX through the VBIOS, training)
 *   frb_dce_v11_0.c          dce_v11_0.c (hw_init, HPD, CRTC, scanout, LB/watermarks)
 *                            + the DRM helper's modeset order (drm_crtc_helper_set_mode)
 *
 * One CRTC drives one connector at a time (DRM's object graph is not
 * carried: no MST, no clones). Every AtomBIOS parameter block is built byte
 * by byte at offsetof() positions in a little-endian buffer (frb_ps), never
 * as a host structure.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_DISPLAY_H
#define FRB_DISPLAY_H

#include "frb_device.h"
#include "frb_pm.h"
#include "../shared/frb_mode.h"
#include "../shared/atom/ObjectID.h"

#define FRB_MAX_CONNECTORS	6
#define FRB_EDID_MAX		512

/* amdgpu_mode.h: an encoder/connector as the legacy path holds it */
struct frb_connector {
	uint16_t connector_object_id;	/* full object id (type/enum/id) */
	uint8_t connector_type;		/* OBJECT_ID part: CONNECTOR_OBJECT_ID_DISPLAYPORT ... */
	uint16_t encoder_object_id;	/* full object id of the internal encoder */
	uint8_t encoder_id;		/* OBJECT_ID part: ENCODER_OBJECT_ID_INTERNAL_UNIPHY.. */
	uint8_t linkb;			/* enum 2 = the B link of the UNIPHY */
	uint16_t devices;		/* ATOM_DEVICE_DFPx_SUPPORT */
	uint8_t i2c_id;			/* ATOM_I2C_ID_CONFIG.ucAccess: the AUX/DDC channel */
	int8_t hpd;			/* AMDGPU_HPD_1 = 0 .. 5, -1 none */
	/* amdgpu_connector_atom_dig */
	uint8_t dpcd[16];		/* DP_RECEIVER_CAP_SIZE (15) bytes used */
	uint8_t dp_sink_type;
	uint32_t dp_clock;		/* link rate in kHz: 162000/270000/540000 */
	uint32_t dp_lane_count;
	uint8_t edid[FRB_EDID_MAX];
	uint32_t edid_len;
	/* amdgpu_connector for DVI/HDMI: a digital (TMDS) sink found over DDC, and whether its EDID says HDMI */
	uint8_t sink_tmds;
	uint8_t is_hdmi;
	/* amdgpu_encoder_atom_dig */
	int dig_encoder;		/* -1 unassigned */
	int panel_mode;
	uint32_t pixel_clock;		/* kHz, the encoder's */
	uint8_t detect_logged;		/* frb_display_detect's last outcome + 1 (0: none yet): logged on a change */
};

/* amdgpu_atom_ss */
struct frb_atom_ss {
	uint16_t percentage, percentage_divider;
	uint8_t type, delay, range, refdiv;
	uint16_t step, rate, amount;
};

/* amdgpu_crtc, CRTC0 only so far */
struct frb_crtc {
	int crtc_id;
	uint32_t crtc_offset;
	int base_enabled;		/* drm_crtc.enabled: in use by a mode (set before mode_set) */
	int enabled;			/* amdgpu_crtc.enabled: dpms on */
	int pll_id;
	uint32_t adjusted_clock;
	int bpc;
	int ss_enabled;
	struct frb_atom_ss ss;
	struct frb_connector *conn;
	struct frb_mode mode;
	/* the scanout surface */
	uint32_t fb_offset;		/* from the start of VRAM (BAR0 offset) */
	uint32_t fb_pitch;		/* pixels */
	uint32_t fb_width, fb_height;
	uint32_t fb_bpp;		/* 8 (indexed through the LUT), 16 (xRGB1555) or 32 (xRGB8888) */
	uint32_t line_time, wm_high, wm_low, lb_vblank_lead_lines;
	uint16_t gamma[3][256];		/* drm_crtc.gamma_store: red, green, blue; 16-bit */
	/* the hardware cursor (amdgpu_crtc's cursor_*): cursor_addr 0 = no cursor bo */
	uint64_t cursor_addr;
	int cursor_x, cursor_y;
	uint32_t cursor_width, cursor_height;
	int cursor_visible;
};

/* amdgpu_crtc max_cursor_width/height on DCE 10/11; the plane reads its surface at this pitch */
#define FRB_CURSOR_MAX		128

/*
 * DP audio's state, RadeonNI's struct rdn_dce8_afmt: which Azalia codec pins
 * (AZF0ENDPOINTn) are wired, which DIG each feeds, the one stream with audio.
 */
#define FRB_AFMT_NUM_PINS	7
struct frb_afmt_pin {
	uint8_t id;
	bool connected;		/* the pin's configuration default does not say "no physical connection" */
	int8_t dig_encoder;	/* -1 when free */
};
struct frb_afmt {
	bool initialized;
	bool has_audio;
	bool enabled;		/* frb_audio_dp_enable(true) ran for active_dig (frbctl's view) */
	uint32_t dprefclk_khz;	/* DTO1's module */
	int8_t active_dig;
	int8_t active_pin;
	struct frb_afmt_pin pins[FRB_AFMT_NUM_PINS];
};

struct frb_display {
	int ready;			/* dce hw_init done */
	uint32_t num_crtc, num_hpd, num_dig;
	uint32_t default_dispclk;	/* 10 kHz */
	uint32_t dp_extclk;		/* 10 kHz */
	uint32_t current_sclk, current_mclk;	/* 10 kHz, for the watermarks (DPM's top level when on) */
	struct amd_pp_display_configuration pm_cfg;	/* adev->pm.pm_display_cfg (frb_pm.h) */
	int num_connectors;
	struct frb_connector conn[FRB_MAX_CONNECTORS];
	struct frb_crtc crtc;
	struct frb_afmt afmt;		/* DP audio (frb_dce_audio.c, RadeonNI's rdn_dce8_afmt), FirebrandAudio */
};

/* a parameter block for the interpreter: little-endian bytes in dwords */
struct frb_ps {
	uint32_t w[16];
};
static inline void frb_ps_init(struct frb_ps *ps) { memset(ps, 0, sizeof(*ps)); }
static inline void frb_ps_u8(struct frb_ps *ps, uint32_t off, uint32_t v) { ((uint8_t *)ps->w)[off] = (uint8_t)v; }
static inline void frb_ps_le16(struct frb_ps *ps, uint32_t off, uint32_t v) { frb_put_le16(ps->w, off, (uint16_t)v); }
static inline void frb_ps_le32(struct frb_ps *ps, uint32_t off, uint32_t v) { frb_put_le32(ps->w, off, v); }
static inline uint8_t frb_ps_get_u8(const struct frb_ps *ps, uint32_t off) { return ((const uint8_t *)ps->w)[off]; }
static inline uint16_t frb_ps_get_le16(const struct frb_ps *ps, uint32_t off) { return frb_get_le16(ps->w, off); }
static inline uint32_t frb_ps_get_le32(const struct frb_ps *ps, uint32_t off) { return frb_get_le32(ps->w, off); }
int frb_ps_exec(struct frb_device *adev, int index, struct frb_ps *ps);
int frb_cmd_rev(struct frb_device *adev, int index, uint8_t *frev, uint8_t *crev);

/* frb_atombios_conn.c */
int frb_atombios_get_connector_info(struct frb_device *adev, struct frb_display *d);
int frb_atombios_get_asic_ss_info(struct frb_device *adev, struct frb_atom_ss *ss, int id, uint32_t clock);
void frb_atombios_scratch_regs_lock(struct frb_device *adev, int lock);

/* frb_atombios_crtc.c */
void frb_atombios_crtc_lock(struct frb_device *adev, struct frb_crtc *c, int lock);
void frb_atombios_crtc_enable(struct frb_device *adev, struct frb_crtc *c, int state);
void frb_atombios_crtc_blank(struct frb_device *adev, struct frb_crtc *c, int state);
void frb_atombios_crtc_powergate(struct frb_device *adev, struct frb_crtc *c, int state);
void frb_atombios_crtc_powergate_init(struct frb_device *adev);
void frb_atombios_crtc_set_dtd_timing(struct frb_device *adev, struct frb_crtc *c, const struct frb_mode *m);
void frb_atombios_crtc_overscan_setup(struct frb_device *adev, struct frb_crtc *c);
void frb_atombios_crtc_scaler_setup(struct frb_device *adev, struct frb_crtc *c);
uint32_t frb_atombios_crtc_set_dce_clock(struct frb_device *adev, uint32_t freq, uint8_t clk_type, uint8_t clk_src);
void frb_atombios_crtc_program_pll(struct frb_device *adev, int crtc_id, int pll_id, uint32_t encoder_mode,
				   uint32_t encoder_id, uint32_t clock, int bpc, int duallink);
int frb_atombios_crtc_prepare_pll(struct frb_device *adev, struct frb_display *d, struct frb_crtc *c);

/* frb_atombios_encoders.c */
int frb_atombios_encoder_get_encoder_mode(struct frb_connector *conn);
int frb_atombios_encoder_get_bpc(struct frb_crtc *c);
void frb_atombios_encoder_setup_dig_encoder(struct frb_device *adev, struct frb_crtc *c, int action, int panel_mode);
void frb_atombios_encoder_setup_dig_transmitter(struct frb_device *adev, struct frb_connector *conn,
						struct frb_crtc *c, int action, uint8_t lane_num, uint8_t lane_set);
void frb_atombios_encoder_set_crtc_source(struct frb_device *adev, struct frb_crtc *c);
void frb_atombios_encoder_init_dig(struct frb_device *adev, struct frb_display *d);
/* returns the DP link training result when it turns a DP output on, else 0 */
int frb_atombios_encoder_dpms(struct frb_device *adev, struct frb_crtc *c, int on);
void frb_atombios_encoder_set_bios_scratch_regs(struct frb_device *adev, struct frb_connector *conn, int connected);

/* frb_atombios_dp.c */
int frb_dp_aux(struct frb_device *adev, struct frb_connector *conn, int request, uint32_t address,
	       uint8_t *buf, int size, uint8_t *reply);
int frb_dp_dpcd_read(struct frb_device *adev, struct frb_connector *conn, uint32_t addr, uint8_t *buf, int n);
int frb_dp_dpcd_write(struct frb_device *adev, struct frb_connector *conn, uint32_t addr, const uint8_t *buf, int n);
uint8_t frb_dp_get_sinktype(struct frb_device *adev, struct frb_connector *conn);
int frb_dp_get_dpcd(struct frb_device *adev, struct frb_connector *conn);
int frb_dp_read_edid(struct frb_device *adev, struct frb_connector *conn);
int frb_dp_set_link_config(struct frb_connector *conn, uint32_t pix_clock, uint32_t max_rate);
int frb_dp_link_train(struct frb_device *adev, struct frb_crtc *c);
void frb_dp_set_rx_power_state(struct frb_device *adev, struct frb_connector *conn, uint8_t state);
void frb_dp_link_report(struct frb_device *adev, struct frb_connector *conn);
int frb_dp_sink_crc(struct frb_device *adev, struct frb_connector *conn, uint16_t crc[3]);

/* frb_atombios_i2c.c: DDC through the VBIOS (atombios_i2c.c) */
int frb_ddc_probe(struct frb_device *adev, struct frb_connector *conn);
int frb_ddc_read_edid(struct frb_device *adev, struct frb_connector *conn);

/* a sink is there: DisplayPort with its DPCD read, or TMDS with a digital EDID */
static inline int frb_connector_connected(const struct frb_connector *c)
{
	return c->dpcd[0] || c->sink_tmds;
}
/* the connector carries TMDS (DVI-D, DVI-I's digital side, HDMI) */
static inline int frb_connector_is_tmds(const struct frb_connector *c)
{
	switch (c->connector_type) {
	case CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_I:
	case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I:
	case CONNECTOR_OBJECT_ID_SINGLE_LINK_DVI_D:
	case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D:
	case CONNECTOR_OBJECT_ID_HDMI_TYPE_A:
	case CONNECTOR_OBJECT_ID_HDMI_TYPE_B:
		return 1;
	default:
		return 0;
	}
}
/* amdgpu_dig_monitor_is_duallink */
int frb_dig_monitor_is_duallink(const struct frb_connector *c, uint32_t pixel_clock);
/*
 * The connector's mode_valid: DP (amdgpu_connector_dp_mode_valid, the link
 * at 24 bpp; max_rate caps it, 0 = the sink's) or DVI/HDMI
 * (amdgpu_connector_dvi_mode_valid: 165 MHz single link, 340 MHz to an HDMI
 * sink, dual-link DVI above 165). 1 = settable.
 */
int frb_display_mode_valid(const struct frb_connector *c, const struct frb_mode *mode, uint32_t max_rate);

/* frb_dce_v11_0.c */
int frb_dce_v11_0_hw_init(struct frb_device *adev, struct frb_display *d);
int frb_dce_v11_0_hpd_sense(struct frb_device *adev, int hpd);
/* the HPD pad itself (DC_GPIO_HPD_Y), before the HPD block's filter: 1 high, 0 low, -1 no such line */
int frb_dce_v11_0_hpd_pad(struct frb_device *adev, int hpd);
int frb_display_detect(struct frb_device *adev, struct frb_display *d);
int frb_display_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			 const struct frb_mode *mode, uint32_t fb_offset, uint32_t pitch_pixels,
			 uint32_t bpp, uint32_t max_link_rate);
/* amdgpu_align_pitch for a linear surface: 256 bytes, i.e. 256/128/64 pixels at 8/16/32 bpp */
static inline uint32_t frb_display_align_pitch(uint32_t width, uint32_t bpp)
{
	uint32_t mask = bpp == 8 ? 255 : bpp == 16 ? 127 : 63;

	return (width + mask) & ~mask;
}
void frb_display_off(struct frb_device *adev, struct frb_display *d);
/* amdgpu_dpm_compute_clocks for a display lit before DPM came up (frb_device_init) */
void frb_display_dpm_changed(struct frb_device *adev);
void frb_display_report(struct frb_device *adev, struct frb_display *d);
void frb_dce_v11_0_load_lut(struct frb_device *adev, struct frb_crtc *c);
/*
 * frb_dce_audio.c: DP audio, RadeonNI's (hw/rdn_dce8_afmt.c, DCE8) on DCE
 * 11.2's registers, FirebrandAudio. hw_init at stage dce (rdn_dce8_afmt_init);
 * dp_mode_set with the DIG's transmitter off in a mode set; dp_enable(true)
 * once the link trained and the video is on, (false) before it goes off.
 */
void frb_audio_hw_init(struct frb_device *adev, struct frb_display *d);
int frb_audio_dp_mode_set(struct frb_device *adev, struct frb_display *d, uint8_t crtc_id, int dig_encoder,
			  const struct frb_mode *mode, const uint8_t *edid, size_t edid_len);
void frb_audio_dp_enable(struct frb_device *adev, struct frb_display *d, int dig_encoder, bool enable);
void frb_audio_fini(struct frb_device *adev, struct frb_display *d);
/* frbctl audio: the stream's audio set up again (dp_enable false, dp_mode_set, dp_enable true); repick: the pin afresh */
int frb_audio_rerun(struct frb_device *adev, struct frb_display *d, bool repick);
uint32_t frb_audio_decode_dfs_div(uint32_t divider_id);
/* the CRTC's 256 legacy LUT entries as DC_LUT_30_COLOR returns them; -ENODEV when it is not running */
int frb_display_read_lut(struct frb_device *adev, struct frb_display *d, uint32_t *out);
/* dce_v11_0_crtc_gamma_set: a new gamma_store (256 16-bit entries per colour) */
void frb_display_set_gamma(struct frb_device *adev, struct frb_display *d,
			   const uint16_t *r, const uint16_t *g, const uint16_t *b);
/*
 * The hardware cursor (dce_v11_0_crtc_cursor_set2 / _cursor_move): a
 * FRB_CURSOR_MAX-pitch premultiplied ARGB surface at bar_offset, little-endian
 * words, width x height of it shown; bar_offset ~0u turns it off. x, y: the
 * image's top left on the screen (may be negative). Kept across mode sets
 * (dce_v11_0_cursor_reset).
 */
int frb_display_cursor_set(struct frb_device *adev, struct frb_display *d, uint32_t bar_offset,
			   uint32_t width, uint32_t height);
int frb_display_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible);

#endif
