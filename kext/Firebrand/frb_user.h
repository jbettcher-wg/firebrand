/*
 * The interface between Firebrand.kext and its control tool (tools/frbctl.c):
 * the service class, the client type, the method numbers, the memory types
 * and the info structure. Administrator only, like RadeonNI's 'rdn2'.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_USER_H
#define FRB_USER_H

#include <stdint.h>

#define FRB_UC_SERVICE_CLASS	"Firebrand"
#define FRB_UC_FB_CLASS		"FirebrandFramebuffer"
#define FRB_UC_TYPE		0x66726231	/* 'frb1' */

enum {
	FRB_UC_GET_INFO = 0,	/* struct out: struct frb_user_info */
	FRB_UC_REG_READ,	/* in: dword register index; out: value */
	FRB_UC_REG_WRITE,	/* in: index, value (only with frb_regwrite=1 / FirebrandAllowRegWrite) */
	FRB_UC_SMC_READ,	/* in: SMC address; out: value (through SMC_IND_INDEX_11) */
	FRB_UC_RUN_STAGE,	/* in: stage; out: errno-style result */
	FRB_UC_VRAM_READ,	/* in: BAR0 offset; out: the little-endian word, as a number */
	FRB_UC_TRACE,		/* in: 1 = start a fresh register trace, 0 = stop; out: entries */
	FRB_UC_DISPLAY_INFO,	/* struct out: struct frb_user_display (stage dce done) */
	FRB_UC_EDID,		/* in: connector; struct out: its EDID (up to 512 bytes) */
	FRB_UC_DISPLAY_MODE,	/* in: connector, width, height, refresh Hz, max link rate kHz; out: result.
				   width 0 = the EDID's preferred mode; rate 0 = the sink's */
	FRB_UC_DISPLAY_OFF,	/* out: result */
	FRB_UC_DISPLAY_FILL,	/* in: pattern (FRB_PATTERN_*), arg0, arg1; out: result */
	FRB_UC_DETECT,		/* out: the first connected DP connector, or -1 (re-runs detect) */
	FRB_UC_DPCD_READ,	/* in: connector, DPCD address; out: 4 bytes from there, first in the top byte */
	FRB_UC_SINK_CRC,	/* in: connector; out: result, CRC red/Cr, green/Y, blue/Cb */
	FRB_UC_ENGINE_TEST,	/* in: engine (FRB_ENGINE_*), test (FRB_TEST_*); out: result */
	FRB_UC_FB_DETACH,	/* out: result. The framebuffer takes no new IOGraphics clients from here
				   on, so the window server cannot reopen it and the kext can unload */
	FRB_UC_RECOVER,		/* in: blocks to reset regardless (FRB_RECOVER_*: 1 gfx, 2 sdma; 0 the busy
				   ones); out: result, blocks hung, still hung | rings failed << 8,
				   soft-recovered, ms */
	FRB_UC_CURSOR,		/* in: FRB_CURSOR_OP_*, x, y; out: result (the hardware cursor, for tests) */
	FRB_UC_DPM,		/* in: FRB_DPM_OP_*; struct out: struct frb_user_dpm (after the op) */
	FRB_UC_LUT,		/* struct out: struct frb_user_lut (the CRTC's LUT, as stored and as the hardware holds it) */
	FRB_UC_AUDIO,		/* in: FRB_AUDIO_OP_*, arg; out: result, active DIG, active codec pin, flags
				   (bit 0 packets on, bit 1 the sink has audio, 15:8 the forced pin or 0xff) */
	FRB_UC_METHOD_COUNT
};

/* FRB_UC_AUDIO (DP audio, FirebrandAudio) */
enum {
	FRB_AUDIO_OP_STATUS = 0,
	FRB_AUDIO_OP_PIN,	/* arg: the codec pin to use from now on, -1 = RadeonNI's choice; the stream's
				   audio is set up again on it */
	FRB_AUDIO_OP_RERUN,	/* the stream's audio set up again (off, mode set, on) */
	FRB_AUDIO_OP_OFF,	/* the DIG's audio packets and the pin's presence off */
	FRB_AUDIO_OP_ON
};

/* FRB_UC_CURSOR */
enum {
	FRB_CURSOR_OP_OFF = 0,	/* no cursor */
	FRB_CURSOR_OP_TEST,	/* the test image at x, y: 128x128, left half opaque ramps
				   (A ff, R 4x, G 2y, B 3(x+y), low bytes), right half transparent */
	FRB_CURSOR_OP_MOVE,	/* shown at x, y */
	FRB_CURSOR_OP_HIDE,
	FRB_CURSOR_OP_SOLID	/* x = one ARGB word for the whole 128x128 image, y = how many rows of it
				   (the rest transparent); where it was, shown */
};

/* FRB_UC_LUT: what the LUT was last given (the gamma ramp, or at 8 bpp the colour table through it) */
struct frb_user_lut {
	uint32_t bpp;			/* the screen's depth: 8, 16, 32 (0: no screen) */
	int32_t hw_read;		/* 0: hw[] read back from DC_LUT_30_COLOR; else -errno (no CRTC running) */
	uint16_t store[3][256];		/* red, green, blue, 16-bit */
	uint32_t hw[256];		/* DC_LUT_30_COLOR: red 29:20, green 19:10, blue 9:0 */
};

/* FRB_UC_DPM: power_dpm_force_performance_level and the sensors */
enum {
	FRB_DPM_OP_STATUS = 0,	/* nothing; the readout */
	FRB_DPM_OP_HIGH,	/* smu7_force_dpm_highest */
	FRB_DPM_OP_LOW,		/* smu7_force_dpm_lowest */
	FRB_DPM_OP_AUTO,	/* smu7_unforce_dpm_levels */
	FRB_DPM_OP_ENABLE,	/* run the DPM init now (stage sdma done, DPM not yet on) */
	FRB_DPM_OP_DISABLE	/* hw_fini: boot state, DPM off */
};

struct frb_user_dpm {
	uint32_t version;		/* 1 */
	int32_t result;			/* of the op */
	int32_t enabled, failed;	/* DPM running; the init's errno */
	uint32_t forced_level;		/* AMD_DPM_FORCED_LEVEL_*: 1 auto, 4 low, 8 high */
	uint32_t sclk_count, mclk_count, pcie_count;
	uint32_t sclk_levels[8], mclk_levels[8];	/* 10 kHz */
	uint32_t sclk_mask, mclk_mask, pcie_mask;
	uint32_t cur_sclk, cur_mclk;	/* 10 kHz, the SMC's answer */
	int32_t cur_sclk_index, cur_mclk_index;
	uint32_t vddgfx_mv;
	int32_t temp_mc;		/* millidegrees */
	uint32_t fan_rpm, fan_pwm;	/* pwm 0-255 */
	uint32_t power_mw;
	uint32_t gpu_load, mem_load;	/* % */
	uint32_t pcie_gen, pcie_lanes;
	uint32_t smc_feature_status;
	uint32_t num_display, vrefresh, min_vblank_time;
};

/* clientMemoryForType */
enum {
	FRB_UC_MEMORY_TRACE = 1,	/* struct { uint32 reg (bit 31 = write); uint32 val } [], CPU order */
	FRB_UC_MEMORY_LOG = 2		/* the driver's log ring, text */
};

#define FRB_EDID_BYTES		512	/* FRB_EDID_MAX in amdgpu/frb_display.h */

/* test patterns for the scanout surface */
enum {
	FRB_PATTERN_BLACK = 0,
	FRB_PATTERN_BARS,	/* colour bars, ramps, a 1-pixel white border, red top-left square */
	FRB_PATTERN_GREY,	/* plain 0x9a9a9a */
	FRB_PATTERN_SOLID,	/* arg0 = 0xRRGGBB everywhere */
	FRB_PATTERN_DOT,	/* one white pixel at (arg0, arg1) on black */
	FRB_PATTERN_TOP,	/* the top arg0 lines white, the rest black */
	FRB_PATTERN_LEFT,	/* the left arg0 columns white, the rest black */
	FRB_PATTERN_SPLASH,	/* the boot splash: grey 0xBFBFBF, the Firebrand logo (shared/frb_splash.c); any depth */
	FRB_PATTERN_COUNT
};

struct frb_user_connector {
	uint16_t connector_object_id, encoder_object_id;
	uint16_t devices, edid_len;
	uint8_t connector_type, encoder_id, linkb, i2c_id;
	int8_t hpd, dig_encoder;
	uint8_t sense, dp_sink_type;
	uint8_t dpcd[16];
	uint32_t dp_clock, dp_lane_count;
};

struct frb_user_display {
	uint32_t version;		/* 1 */
	uint32_t ready, num_connectors, num_crtc;
	int32_t active;			/* connector on CRTC0, -1 none */
	uint32_t default_dispclk, dp_extclk;	/* 10 kHz */
	uint32_t clock, flags;		/* the mode on CRTC0 */
	uint16_t hdisplay, hsync_start, hsync_end, htotal;
	uint16_t vdisplay, vsync_start, vsync_end, vtotal;
	uint32_t adjusted_clock, fb_offset, fb_pitch;
	uint32_t line_time, watermark, lb_lead_lines;
	uint32_t crtc_control, crtc_status_frame_count;
	struct frb_user_connector conn[6];
	int32_t audio_dig;		/* DP audio: the DIG audio_ep feeds, -1 off */
	uint32_t audio_hot_plug, audio_speakers;	/* audio_ep's HOT_PLUG_CONTROL, CHANNEL_SPEAKER */
	int32_t audio_ep;		/* the codec pin (AZF0ENDPOINTn) DP audio takes, 0 when none */
};

struct frb_user_info {
	uint32_t version;		/* 1 */
	uint16_t vendor, device, subsys_vendor, subsys_device;
	uint32_t revision, rev_id, external_rev_id;
	uint32_t asic_type;
	uint32_t row_max_stage;		/* 0xff: no row */
	uint32_t stage_done;
	int32_t stage_failed;
	uint32_t asic_init_ms;
	uint32_t rmmio_size, aper_size;
	uint32_t vram_mb, vram_width;
	uint32_t vram_start_hi, vram_start_lo;
	uint32_t gart_start_hi, gart_start_lo;
	uint32_t gart_table_offset, mem_scratch_offset, toc_offset, smu_buffer_offset, fw_buf_offset;
	uint32_t dummy_hi, dummy_lo;
	uint32_t trace_len, trace_cap, trace_lost;
	uint32_t log_len, log_cap;
	uint32_t mc_fw_version;
	char board[64];
	char mc_fw[32];
};

#endif
