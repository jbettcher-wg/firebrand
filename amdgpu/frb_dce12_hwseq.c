/*
 * DCE 12 mode set: what DC's dc_commit_streams does for one SST DP stream on
 * a dce120 pipe, in DC's order (dce110_hwseq.c with dce120's objects):
 *
 *   dce110_enable_accelerated_mode   the first commit: encoders, CRTCs and VGA off
 *   dce110_prepare_bandwidth         safe display marks, dce12_update_clocks
 *   dce110_apply_ctx_to_hw           audio DTO, the CRTC timing, the stream
 *                                    encoder, the link (training), unblank
 *   dce110_optimize_bandwidth        the marks and clocks for the mode
 *   apply_ctx_for_surface            the plane: DCP, LB, SCL, the scanout surface
 *
 * checked write for write against DC's first mode set in the MI50 capture
 * (tests/Makefile replay-v20-ms). Not a DC port; where DC computes a value
 * from code Firebrand does not carry (dce_calcs' bandwidth model) the
 * comment says what stands in for it.
 *
 * Copyright 2012-17 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "../shared/atom/atomfirmware-frb.h"
#include "frb_soc15.h"
#include "frb_display.h"
#include "frb_dce12.h"
#include "frb_smu11.h"

#define FW_CMD_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_command_functions_v2_1, member) / sizeof(uint16_t)))

/* BIOS_SCRATCH_6 bits DC's bios_parser_helper sets (atombios.h ATOM_S6_*) */
#define S6_ACC_MODE		0x00000010
#define S6_CRITICAL_STATE	0x00000100

static uint32_t scratch6(struct frb_device *adev)
{
	return adev->bios_scratch_reg_offset + 6;
}

/* bios_is_accelerated_mode */
static int bios_is_accelerated_mode(struct frb_device *adev)
{
	return (RREG32(scratch6(adev)) & S6_ACC_MODE) != 0;
}

static void bios_scratch_update(struct frb_device *adev, uint32_t mask, int set)
{
	frb_dce12_update(adev, scratch6(adev), mask, set ? mask : 0);
}

/* command_table2.c enable_crtc_v1 */
static int bios_enable_crtc(struct frb_device *adev, int crtc, int enable)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, 0, (uint8_t)crtc);		/* ATOM_CRTC1 + n */
	frb_ps_u8(&ps, 1, enable ? 1 : 0);		/* ATOM_ENABLE / ATOM_DISABLE */
	return frb_ps_exec(adev, FW_CMD_INDEX(enablecrtc), &ps);
}

/*
 * dce110_enable_accelerated_mode with no eDP and no seamless boot:
 * power_down_all_hw_blocks (each link's DP stream blanked and its encoder
 * off, the six CRTCs off through the VBIOS, the PLLs, which on DCE 12 DP
 * cost nothing), VGA off and the front ends gated (both no-ops but the VGA
 * write on dce120), then the BIOS's accelerated-mode flag.
 */
static void enable_accelerated_mode(struct frb_device *adev, struct frb_display *d)
{
	int i;

	(void)bios_is_accelerated_mode(adev);	/* clk_mgr_exit_optimized_pwr_state's check */
	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];

		/* link_blank_dp_stream(link, false): the DIG's front end is off, then the sink to D3 */
		(void)frb_dce12_dig_enabled(adev, c);
		if (c->dpcd[0])
			frb_dce12_dpcd_write1(adev, c, 0x600, 2);	/* DP_SET_POWER D3 */
		/* disable_output(SIGNAL_TYPE_NONE): nothing to do for a DIG that is off */
		frb_dce12_link_disable_output(adev, c);
	}
	for (i = 0; i < (int)d->num_crtc; i++)
		bios_enable_crtc(adev, i, 0);
	for (i = 0; i < (int)d->num_crtc; i++)
		frb_dce12_tg_disable_vga(adev, i);
	(void)bios_is_accelerated_mode(adev);	/* clk_mgr_optimize_pwr_state's check */
	bios_scratch_update(adev, S6_ACC_MODE, 1);
}


/* ------------------------------------------------------------------------- */
/* dce_mem_input.c dce120_mi_program_display_marks (DMIF_PG of the pipe) */

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py:
 * mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL mmDMIF_PG0_DPG_PIPE_URGENCY_CONTROL
 * mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL mmDMIF_PG0_DPG_PIPE_STUTTER_CONTROL2 mmDMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL
 */
#define FRB_DCE12_DMIF_STRIDE	(mmDMIF_PG1_DPG_WATERMARK_MASK_CONTROL - mmDMIF_PG0_DPG_WATERMARK_MASK_CONTROL)
#define DMIF_REG(p, r)		(DCE_REG(mmDMIF_PG0_##r) + (uint32_t)(p) * FRB_DCE12_DMIF_STRIDE)
#define WMC(f)			DMIF_PG0_DPG_WATERMARK_MASK_CONTROL__##f##_MASK

struct dce_watermarks {
	uint32_t a, b, c, d;
};

#define MAX_WATERMARK	0xFFFF
#define SAFE_NBP_MARK	0x7FFF

static void program_display_marks(struct frb_device *adev, int pipe, const struct dce_watermarks *nbp,
				  const struct dce_watermarks *stutter_exit, const struct dce_watermarks *stutter_entry,
				  const struct dce_watermarks *urgent, uint32_t total_dest_line_time_ns)
{
	const uint32_t *u = &urgent->a, *n = &nbp->a, *sx = &stutter_exit->a, *se = &stutter_entry->a;
	uint32_t set, v;

	/* dce120_program_urgency_watermark (its URGENT_LEVEL write goes to a register dce120's list lacks) */
	for (set = 0; set < 4; set++) {
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_WATERMARK_MASK_CONTROL), WMC(URGENCY_WATERMARK_MASK),
				 REG_SET_FIELD(0, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL, URGENCY_WATERMARK_MASK, set));
		v = REG_SET_FIELD(0, DMIF_PG0_DPG_PIPE_URGENCY_CONTROL, URGENCY_LOW_WATERMARK, u[set]);
		v = REG_SET_FIELD(v, DMIF_PG0_DPG_PIPE_URGENCY_CONTROL, URGENCY_HIGH_WATERMARK, total_dest_line_time_ns);
		WREG32(DMIF_REG(pipe, DPG_PIPE_URGENCY_CONTROL), v);
	}
	/* stutter on (debug.disable_stutter is off), FBC ignored */
	frb_dce12_update(adev, DMIF_REG(pipe, DPG_PIPE_STUTTER_CONTROL),
			 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_ENABLE_MASK |
			 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_IGNORE_FBC_MASK,
			 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_ENABLE_MASK |
			 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL__STUTTER_IGNORE_FBC_MASK);
	/* program_nbp_watermark: the P-state change mark (no NB_PSTATE register on dce120) */
	for (set = 0; set < 4; set++) {
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_WATERMARK_MASK_CONTROL), WMC(PSTATE_CHANGE_WATERMARK_MASK),
				 REG_SET_FIELD(0, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL, PSTATE_CHANGE_WATERMARK_MASK, set));
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_PIPE_LOW_POWER_CONTROL),
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_ENABLE_MASK |
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_URGENT_DURING_REQUEST_MASK |
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_NOT_SELF_REFRESH_DURING_REQUEST_MASK,
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_ENABLE_MASK |
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_URGENT_DURING_REQUEST_MASK |
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_NOT_SELF_REFRESH_DURING_REQUEST_MASK);
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_PIPE_LOW_POWER_CONTROL),
				 DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL__PSTATE_CHANGE_WATERMARK_MASK,
				 REG_SET_FIELD(0, DMIF_PG0_DPG_PIPE_LOW_POWER_CONTROL, PSTATE_CHANGE_WATERMARK, n[set]));
	}
	/* dce120_program_stutter_watermark */
	for (set = 0; set < 4; set++) {
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_WATERMARK_MASK_CONTROL),
				 WMC(STUTTER_EXIT_SELF_REFRESH_WATERMARK_MASK),
				 REG_SET_FIELD(0, DMIF_PG0_DPG_WATERMARK_MASK_CONTROL,
					       STUTTER_EXIT_SELF_REFRESH_WATERMARK_MASK, set));
		v = REG_SET_FIELD(0, DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2, STUTTER_EXIT_SELF_REFRESH_WATERMARK, sx[set]);
		v = REG_SET_FIELD(v, DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2, STUTTER_ENTER_SELF_REFRESH_WATERMARK, se[set]);
		frb_dce12_update(adev, DMIF_REG(pipe, DPG_PIPE_STUTTER_CONTROL2),
				 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2__STUTTER_EXIT_SELF_REFRESH_WATERMARK_MASK |
				 DMIF_PG0_DPG_PIPE_STUTTER_CONTROL2__STUTTER_ENTER_SELF_REFRESH_WATERMARK_MASK, v);
	}
}

/* dce110_set_safe_displaymarks */
static void set_safe_displaymarks(struct frb_device *adev, int pipe)
{
	static const struct dce_watermarks max_marks = { MAX_WATERMARK, MAX_WATERMARK, MAX_WATERMARK, MAX_WATERMARK };
	static const struct dce_watermarks nbp_marks = { SAFE_NBP_MARK, SAFE_NBP_MARK, SAFE_NBP_MARK, SAFE_NBP_MARK };
	static const struct dce_watermarks min_marks = { 0, 0, 0, 0 };

	program_display_marks(adev, pipe, &nbp_marks, &max_marks, &min_marks, &max_marks, MAX_WATERMARK);
}

/* ------------------------------------------------------------------------- */
/* clk_mgr/dce120 dce12_update_clocks, dce112_set_clock, dce11_pplib_apply_display_requirements */

#define FRB_PPCLK_DISPCLK	7	/* smu11_driver_if.h PPCLK_e */
#define FRB_PPCLK_PHYCLK	9
#define ATOM_GCK_DFS		8
#define DCE_CLOCK_TYPE_DISPCLK	0
#define DCE_CLOCK_TYPE_DPREFCLK	1

/* command_table2.c set_dce_clock_v2_1: the clock the VBIOS set, kHz */
static uint32_t bios_set_dce_clock(struct frb_device *adev, uint8_t type, uint32_t khz)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, 0, type == DCE_CLOCK_TYPE_DISPCLK ? khz / 10 : 0);
	frb_ps_u8(&ps, 4, type);
	frb_ps_u8(&ps, 5, ATOM_GCK_DFS);
	if (frb_ps_exec(adev, FW_CMD_INDEX(setdceclock), &ps))
		return khz;
	return frb_ps_get_le32(&ps, 0) * 10;
}

/*
 * dce112_set_clock: DISPCLK, no lower than the DENTIST's VCO over 62 (its
 * largest divider; dce_clk_mgr_construct takes the VCO from the
 * firmware's SMU GPU PLL, SYSPLL0), then DPREFCLK (the VBIOS picks it; no
 * genlock source on Vega 20)
 */
static uint32_t set_dispclk(struct frb_device *adev, struct frb_display *d, uint32_t khz)
{
	uint32_t min_khz = d->dce12.syspll_vco_10khz * 10 / 62, actual;

	actual = bios_set_dce_clock(adev, DCE_CLOCK_TYPE_DISPCLK, khz > min_khz ? khz : min_khz);

	bios_set_dce_clock(adev, DCE_CLOCK_TYPE_DPREFCLK, 0);
	/* dce_is_dmcu_initialized: a running DMCU would get the PSR wait loop */
	(void)RREG32(DCE_REG(mmDMCU_STATUS));
	return actual;
}

/*
 * dce12_update_clocks. DC's display clock is its bandwidth model's
 * (dce_calcs, not carried) times 1.15; for one unscaled stream that model
 * asks for the pixel clock plus its 5% ramping margin, which is what stands
 * in here. The PHY clock is the larger of the pixel and symbol clocks.
 * pp: DC hands powerplay its display requirements when they changed.
 */
static void update_clocks(struct frb_device *adev, struct frb_display *d, uint32_t pix_khz, uint32_t sym_khz,
			  int safe_to_lower, int pp)
{
	uint32_t disp = pix_khz * 105 / 100 * 115 / 100, phy = pix_khz > sym_khz ? pix_khz : sym_khz;

	if ((safe_to_lower && disp != d->dce12.dispclk_khz) || disp > d->dce12.dispclk_khz) {
		d->dce12.dispclk_khz = set_dispclk(adev, d, disp);
		frb_vega20_display_clock_request(adev, FRB_PPCLK_DISPCLK, disp);
	}
	if ((safe_to_lower && phy != d->dce12.phyclk_khz) || phy > d->dce12.phyclk_khz) {
		d->dce12.phyclk_khz = phy;
		frb_vega20_display_clock_request(adev, FRB_PPCLK_PHYCLK, phy);
	}
	/* the memory floor and DCEFCLK: one stream needs no more than the lowest levels */
	if (pp)
		frb_vega20_display_config_changed(adev, 0, 0, 1);
}

/* dce110_prepare_bandwidth */
static void prepare_bandwidth(struct frb_device *adev, struct frb_display *d, int pipe, uint32_t pix_khz,
			      uint32_t sym_khz, int pp)
{
	set_safe_displaymarks(adev, pipe);
	update_clocks(adev, d, pix_khz, sym_khz, 0, pp);
}

/* ------------------------------------------------------------------------- */
/* dce110_apply_single_controller_ctx_to_hw, up to the link */

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmFMT0_FMT_BIT_DEPTH_CONTROL
 * mmFMT0_FMT_CONTROL mmFMT0_FMT_DITHER_RAND_R_SEED mmFMT0_FMT_DITHER_RAND_G_SEED mmFMT0_FMT_DITHER_RAND_B_SEED
 * mmFMT0_FMT_CLAMP_CNTL mmFMT0_FMT_DYNAMIC_EXP_CNTL mmCRTC0_CRTC_BLACK_COLOR mmCRTC0_CRTC_START_LINE_CONTROL
 * mmCRTC0_CRTC_MASTER_UPDATE_MODE mmCRTC0_CRTC_MASTER_UPDATE_LOCK mmCRTC0_CRTC_V_TOTAL_CONTROL
 * mmCRTC0_CRTC_V_TOTAL_MIN mmCRTC0_CRTC_V_TOTAL_MAX mmCRTC0_CRTC_STATIC_SCREEN_CONTROL mmDIG0_DIG_FE_CNTL
 */
#define FRB_DCE12_FMT_STRIDE	(mmFMT1_FMT_CONTROL - mmFMT0_FMT_CONTROL)
#define FMT_REG(p, r)		(DCE_REG(mmFMT0_##r) + (uint32_t)(p) * FRB_DCE12_FMT_STRIDE)
#define CRTC_REG(p, r)		(DCE_REG(mmCRTC0_##r) + (uint32_t)(p) * FRB_DCE12_CRTC_STRIDE)
#define DIGI_REG(i, r)		(DCE_REG(mmDIG0_##r) + (uint32_t)(i) * FRB_DCE12_DIG_STRIDE)
#define FMTF(r, f)		FMT0_##r##__##f##_MASK

/*
 * The stream's colour depth: the sink's EDID 1.4 bits per colour, at most
 * 10 (dc_stream's display_color_depth from the EDID caps, as amdgpu_dm
 * fills it: 6 and 8 as they are, above 10 capped to 10 for a 32 bpp surface).
 */
static int stream_bpc(const struct frb_connector *c)
{
	int bpc = 8;

	if (c->edid_len >= 128 && c->edid[18] == 1 && c->edid[19] >= 4 && (c->edid[20] & 0x80)) {
		switch ((c->edid[20] >> 4) & 7) {
		case 1: bpc = 6; break;
		case 2: bpc = 8; break;
		default: bpc = 10; break;
		}
	}
	return bpc;
}

/*
 * dce110_opp_program_fmt for resource_build_bit_depth_reduction_params'
 * default: spatial dither to the stream's depth (highpass and RGB random,
 * zero seeds, no frame random), no truncation, no temporal dither; full
 * range, RGB; then dce110_opp_set_dyn_expansion.
 */
static void opp_program_fmt(struct frb_device *adev, int pipe, int bpc)
{
	uint32_t r = FMT_REG(pipe, FMT_BIT_DEPTH_CONTROL);
	int depth = bpc == 6 ? 0 : bpc == 8 ? 1 : 2;

	/* set_truncation: off */
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_MODE), 0);
	/* set_spatial_dither */
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_DEPTH) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_MODE), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_HIGHPASS_RANDOM_ENABLE) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_FRAME_RANDOM_ENABLE) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_RGB_RANDOM_ENABLE), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_EN), 0);
	frb_dce12_update(adev, FMT_REG(pipe, FMT_CONTROL), FMTF(FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_MAX) |
			 FMTF(FMT_CONTROL, FMT_SPATIAL_DITHER_FRAME_COUNTER_BIT_SWAP), 0);
	frb_dce12_update(adev, FMT_REG(pipe, FMT_DITHER_RAND_R_SEED), FMTF(FMT_DITHER_RAND_R_SEED, FMT_RAND_R_SEED), 0);
	frb_dce12_update(adev, FMT_REG(pipe, FMT_DITHER_RAND_G_SEED), FMTF(FMT_DITHER_RAND_G_SEED, FMT_RAND_G_SEED), 0);
	frb_dce12_update(adev, FMT_REG(pipe, FMT_DITHER_RAND_B_SEED), FMTF(FMT_DITHER_RAND_B_SEED, FMT_RAND_B_SEED), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_HIGHPASS_RANDOM_ENABLE) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_FRAME_RANDOM_ENABLE) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_RGB_RANDOM_ENABLE),
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_HIGHPASS_RANDOM_ENABLE) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_RGB_RANDOM_ENABLE));
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_MODE) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN),
			 REG_SET_FIELD(0, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_DEPTH, depth) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN));
	/* set_temporal_dither: off */
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_EN) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_RESET) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_OFFSET), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_LEVEL), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_25FRC_SEL) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_50FRC_SEL) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_75FRC_SEL), 0);
	/* dce110_opp_set_clamping (full range), set_pixel_encoding (RGB) */
	WREG32(FMT_REG(pipe, FMT_CLAMP_CNTL), 0);
	frb_dce12_update(adev, FMT_REG(pipe, FMT_CONTROL), FMTF(FMT_CONTROL, FMT_PIXEL_ENCODING) |
			 FMTF(FMT_CONTROL, FMT_SUBSAMPLING_MODE) | FMTF(FMT_CONTROL, FMT_CBCR_BIT_REDUCTION_BYPASS), 0);
	/* dce110_opp_set_dyn_expansion: DP, 8 bpc expands 8->12 (mode 1), 10 and 12 bpc 10->12 */
	frb_dce12_update(adev, FMT_REG(pipe, FMT_DYNAMIC_EXP_CNTL), FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) |
			 FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE), 0);
	if (bpc >= 8)
		frb_dce12_update(adev, FMT_REG(pipe, FMT_DYNAMIC_EXP_CNTL), FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) |
				 FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE),
				 FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) |
				 (bpc == 8 ? FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE) : 0));
}

/* command_table2.c set_pixel_clock_v7 for the DP DTO (pll ATOM_DP_DTO), DP encoder mode */
static int bios_set_pixel_clock_dp(struct frb_device *adev, int crtc, const struct frb_connector *c, uint32_t pix_khz)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_le32(&ps, 0, pix_khz * 10);			/* pixclk_100hz */
	frb_ps_u8(&ps, 4, 11);					/* ATOM_DP_DTO */
	frb_ps_u8(&ps, 5, c->encoder_id);			/* the UNIPHY's encoder object id */
	frb_ps_u8(&ps, 6, 0);					/* ATOM_ENCODER_MODE_DP */
	frb_ps_u8(&ps, 8, (uint8_t)crtc);
	return frb_ps_exec(adev, FW_CMD_INDEX(setpixelclock), &ps);
}

/* command_table2.c set_crtc_using_dtd_timing_v3, after dce110's front porch workaround */
static int bios_program_crtc_timing(struct frb_device *adev, int crtc, const struct frb_mode *m)
{
	struct frb_ps ps;
	uint32_t vfp = m->vsync_start - m->vdisplay, misc = 0;

	if (vfp < 1)
		vfp = 1;
	if (m->flags & FRB_MODE_NHSYNC)
		misc |= ATOM_HSYNC_POLARITY;
	if (m->flags & FRB_MODE_NVSYNC)
		misc |= ATOM_VSYNC_POLARITY;
	frb_ps_init(&ps);
	frb_ps_le16(&ps, 0, m->hdisplay);
	frb_ps_le16(&ps, 2, m->htotal - m->hdisplay);
	frb_ps_le16(&ps, 4, m->vdisplay);
	frb_ps_le16(&ps, 6, m->vtotal - m->vdisplay);
	frb_ps_le16(&ps, 8, m->hsync_start - m->hdisplay);
	frb_ps_le16(&ps, 10, m->hsync_end - m->hsync_start);
	frb_ps_le16(&ps, 12, vfp);
	frb_ps_le16(&ps, 14, m->vsync_end - m->vsync_start);
	frb_ps_le16(&ps, 16, misc);
	frb_ps_u8(&ps, 20, (uint8_t)crtc);
	return frb_ps_exec(adev, FW_CMD_INDEX(setcrtc_usingdtdtiming), &ps);
}

/*
 * dce110_enable_stream_timing for a pipe without an old stream: black
 * blank colour, blank, the pixel clock, the timing through the VBIOS
 * (dce110_timing_generator_program_timing_generator; its program_horz_count_by_2
 * goes to dce110's CRTC_COUNT_CONTROL address, not a DCE 12 register, and is
 * left out), the advanced request, then dce120_timing_generator_enable_crtc.
 */
static int enable_stream_timing(struct frb_device *adev, int crtc, const struct frb_connector *c,
				const struct frb_mode *m)
{
	uint32_t vsync_bp, v;
	int r;

	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_BLACK_COLOR),
			 CRTC0_CRTC_BLACK_COLOR__CRTC_BLACK_COLOR_B_CB_MASK | CRTC0_CRTC_BLACK_COLOR__CRTC_BLACK_COLOR_G_Y_MASK |
			 CRTC0_CRTC_BLACK_COLOR__CRTC_BLACK_COLOR_R_CR_MASK, 0);
	frb_dce12_tg_set_blank(adev, crtc, 1);
	r = bios_set_pixel_clock_dp(adev, crtc, c, m->clock);
	if (!r)
		r = bios_program_crtc_timing(adev, crtc, m);
	if (r)
		return r;
	/* dce120_timing_generator_enable_advanced_request: up to 10 lines of prefetch in the back porch */
	vsync_bp = m->vtotal - m->vsync_start;
	if (vsync_bp > 10)
		vsync_bp = 10;
	v = RREG32(CRTC_REG(crtc, CRTC_START_LINE_CONTROL));
	v = REG_SET_FIELD(v, CRTC0_CRTC_START_LINE_CONTROL, CRTC_LEGACY_REQUESTOR_EN, 0);
	v = REG_SET_FIELD(v, CRTC0_CRTC_START_LINE_CONTROL, CRTC_ADVANCED_START_LINE_POSITION, vsync_bp);
	WREG32(CRTC_REG(crtc, CRTC_START_LINE_CONTROL), v);
	/* enable_crtc */
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_MASTER_UPDATE_MODE),
			 CRTC0_CRTC_MASTER_UPDATE_MODE__MASTER_UPDATE_MODE_MASK, 0);
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_MASTER_UPDATE_LOCK),
			 CRTC0_CRTC_MASTER_UPDATE_LOCK__UNDERFLOW_UPDATE_LOCK_MASK, 0);
	return bios_enable_crtc(adev, crtc, 1);
}

/* dce120_timing_generator_set_drr without DRR, then set_static_screen_control(0 events, 2 frames) */
static void tg_no_drr(struct frb_device *adev, int crtc)
{
	WREG32(CRTC_REG(crtc, CRTC_V_TOTAL_CONTROL), 0);
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_V_TOTAL_MIN), CRTC0_CRTC_V_TOTAL_MIN__CRTC_V_TOTAL_MIN_MASK, 0);
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_V_TOTAL_MAX), CRTC0_CRTC_V_TOTAL_MAX__CRTC_V_TOTAL_MAX_MASK, 0);
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_STATIC_SCREEN_CONTROL),
			 CRTC0_CRTC_STATIC_SCREEN_CONTROL__CRTC_STATIC_SCREEN_EVENT_MASK_MASK, 0);
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_STATIC_SCREEN_CONTROL),
			 CRTC0_CRTC_STATIC_SCREEN_CONTROL__CRTC_STATIC_SCREEN_EVENT_MASK_MASK |
			 CRTC0_CRTC_STATIC_SCREEN_CONTROL__CRTC_STATIC_SCREEN_FRAME_COUNT_MASK,
			 REG_SET_FIELD(0, CRTC0_CRTC_STATIC_SCREEN_CONTROL, CRTC_STATIC_SCREEN_FRAME_COUNT, 2));
}

/* ------------------------------------------------------------------------- */
/* link_set_dpms_on: the stream encoder (dce110_stream_encoder), the link, unblank */

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmDIG0_DIG_BE_CNTL mmDP0_DP_PIXEL_FORMAT
 * mmDIG0_AFMT_CNTL mmDIG0_AFMT_VBI_PACKET_CONTROL mmDIG0_AFMT_GENERIC_HDR mmDIG0_AFMT_GENERIC_0
 * mmDP0_DP_SEC_CNTL mmCRTC0_CRTC_TEST_PATTERN_CONTROL mmCRTC0_CRTC_TEST_PATTERN_COLOR
 * mmCRTC0_CRTC_TEST_PATTERN_PARAMETERS mmCRTC0_CRTC_CONTROL mmDP0_DP_VID_TIMING mmDP0_DP_VID_N mmDP0_DP_VID_M
 * mmDP0_DP_STEER_FIFO mmDP0_DP_VID_STREAM_CNTL
 */
#define FRB_DCE12_DPI_STRIDE	(mmDP1_DP_LINK_CNTL - mmDP0_DP_LINK_CNTL)
#define DPI_REG(i, r)		(DCE_REG(mmDP0_##r) + (uint32_t)(i) * FRB_DCE12_DPI_STRIDE)
#define LINK_RATE_REF_FREQ_IN_KHZ	27000

/* ------------------------------------------------------------------------- */
/*
 * DP audio, DC's (dce_audio.c, dce_stream_encoder.c's dce110_se_* audio,
 * dce110_hwseq.c's enable/disable_audio_stream), in DC's order, checked
 * against the capture's first mode set (296.8536-296.9049) and its DPMS
 * off/on (337.4098-337.4865, 340.9437-):
 *   - apply_ctx_to_hw: the wall DTO (DTO1 for DP: 24 MHz over the DP
 *     reference clock less its spread), the stream encoder's audio source
 *     and channels, the Azalia endpoint (speakers, SADs, HBR, lipsync, the
 *     sink's ids and name: what the HD Audio codec reports as its ELD);
 *   - link_set_dpms_on, after the stream: az_enable (AUDIO_ENABLED, the
 *     pin's presence), the AFMT clock, ATP/AIP/ASP, then unmute (samples);
 *   - link_set_dpms_off, first: mute, the packets off, the AFMT clock off;
 *     after the info packets stop and the back end lets go: az_disable.
 * The DC_MEM_GLOBAL_PWR_REQ_DIS the Polaris cards taught is in init_hw.
 * Not carried: check_audio_bandwidth_dp (the rates the link's blanking can
 * carry; Firebrand's DP modes run at HBR2/HBR3 x4, where the LG's 2-channel
 * LPCM passes at every rate, as the capture shows), HDMI, MST.
 *
 * Pasted register names, listed for tools/mk-soc15-headers.py:
 * mmDCCG_AUDIO_DTO_SOURCE mmDCCG_AUDIO_DTO1_MODULE mmDCCG_AUDIO_DTO1_PHASE mmDIG0_AFMT_AUDIO_SRC_CONTROL
 * mmDIG0_AFMT_AUDIO_PACKET_CONTROL mmDIG0_AFMT_AUDIO_PACKET_CONTROL2 mmDIG0_AFMT_INFOFRAME_CONTROL0
 * mmDIG0_AFMT_60958_0 mmDP0_DP_SEC_AUD_N mmDP0_DP_SEC_TIMESTAMP mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS
 * AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0
 * AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_HBR AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC
 * AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_SINK_INFO0 AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_SINK_INFO1
 * AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL
 * SPEAKER_ALLOCATION HDMI_CONNECTION DP_CONNECTION EXTRA_CONNECTION_INFO LFE_PLAYBACK_LEVEL MAX_CHANNELS
 * SUPPORTED_FREQUENCIES DESCRIPTOR_BYTE_2 SUPPORTED_FREQUENCIES_STEREO HBR_CAPABLE VIDEO_LIPSYNC AUDIO_LIPSYNC
 * MANUFACTURER_ID PRODUCT_ID SINK_DESCRIPTION_LEN CLOCK_GATING_DISABLE AUDIO_ENABLED DCCG_AUDIO_DTO_SEL
 * DCCG_AUDIO_DTO2_USE_512FBR_DTO AFMT_AUDIO_SRC_SELECT AFMT_AUDIO_CHANNEL_ENABLE AFMT_AUDIO_LAYOUT_OVRD
 * AFMT_60958_OSF_OVRD AFMT_60958_CS_UPDATE AFMT_AUDIO_SAMPLE_SEND AFMT_AUDIO_INFO_UPDATE AFMT_60958_CS_CLOCK_ACCURACY
 * DP_SEC_ASP_ENABLE DP_SEC_ATP_ENABLE DP_SEC_AIP_ENABLE DP_SEC_ACM_ENABLE DP_SEC_AUD_N DP_SEC_TIMESTAMP_MODE
 */
#define AZ_ENDPOINT_STRIDE_HW	(mmAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - \
				 mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX)
/* dce_11_0_d.h's ix (Azalia endpoint) registers, which DC uses on DCE 12 */
#define ixAZ_CHANNEL_SPEAKER	0x25
#define ixAZ_ACP_DATA		0x27
#define ixAZ_AUDIO_DESCRIPTOR0	0x28
#define ixAZ_RESPONSE_LIPSYNC	0x37
#define ixAZ_RESPONSE_HBR	0x38
#define ixAZ_SINK_INFO0		0x3a
#define ixAZ_SINK_INFO1		0x3b
#define ixAZ_SINK_INFO2		0x3c
#define ixAZ_SINK_INFO3		0x3d
#define ixAZ_SINK_INFO4		0x3e
#define ixAZ_HOT_PLUG_CONTROL	0x54
#define AZF(r, f)		AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_##r##__##f##_MASK
#define AZF_SHIFT(r, f)		AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_##r##__##f##__SHIFT
#define AZ_SET(v, r, f, x)	(((v) & ~AZF(r, f)) | (((uint32_t)(x) << AZF_SHIFT(r, f)) & AZF(r, f)))
/* audio_format_code: LPCM 1 .. WMAPRO 14 (dc/inc/audio_types.h); 1-bit audio and DST are not programmed */
#define AUDIO_FORMAT_CODE_COUNT		14
#define AUDIO_FORMAT_CODE_AC3		2
#define AUDIO_FORMAT_CODE_1BITAUDIO	9
#define AUDIO_FORMAT_CODE_DST		13
#define DEFAULT_SPEAKER_LOCATION	5	/* amdgpu_dm_helpers.c: FL/FR and FC */
#define MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS	18
#define DCE121_DPREFCLK_KHZ		625000	/* dce121_clk_mgr_construct */
#define FW_DATA_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_data_tables_v2_1, member) / sizeof(uint16_t)))

static uint32_t az_rd(struct frb_device *adev, int ep, uint32_t index)
{
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + ep * AZ_ENDPOINT_STRIDE_HW, index);
	return RREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + ep * AZ_ENDPOINT_STRIDE_HW);
}

static void az_wr(struct frb_device *adev, int ep, uint32_t index, uint32_t v)
{
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + ep * AZ_ENDPOINT_STRIDE_HW, index);
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + ep * AZ_ENDPOINT_STRIDE_HW, v);
}

/*
 * The audio resource DC gives a stream: the endpoint with the stream
 * encoder's own number (find_first_free_match_audio), else the first one.
 */
static int audio_endpoint(const struct frb_display *d, int fe)
{
	if (!d->dce12.audio_count)
		return -1;
	return (uint32_t)fe < d->dce12.audio_count ? fe : 0;
}

/*
 * dce12_get_dp_ref_freq_khz: DCE 12.1's 625 MHz DPREFCLK less half its
 * downspread (dce_adjust_dp_ref_freq_for_ss), the spread from dce_info's
 * DP entry (bios_parser2 get_ss_info_v4_1: 0.001 % units, divider 1000).
 */
static uint32_t dp_ref_clk_khz(struct frb_device *adev)
{
	uint64_t div = 1000ull * 200;
	uint16_t off = 0, pct;
	uint8_t frev = 0, crev = 0, mode;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(dce_info), &frev, &crev, &off) || frev != 4 || crev != 1)
		return DCE121_DPREFCLK_KHZ;
	pct = frb_get_le16(adev->bios, off + offsetof(struct atom_display_controller_info_v4_1, dp_ss_percentage));
	mode = adev->bios[off + offsetof(struct atom_display_controller_info_v4_1, dp_ss_mode)];
	if (!pct || (mode & ATOM_SS_CENTRE_SPREAD_MODE))
		return DCE121_DPREFCLK_KHZ;
	return (uint32_t)((uint64_t)DCE121_DPREFCLK_KHZ * (div - pct) / div);
}

/* dce_aud_wall_dto_setup, the DP arm: DTO1 at 24 MHz / the DP reference clock */
static void audio_wall_dto_setup_dp(struct frb_device *adev, struct frb_display *d)
{
	d->dce12.audio_dto_khz = dp_ref_clk_khz(adev);
	frb_dce12_update(adev, DCE_REG(mmDCCG_AUDIO_DTO_SOURCE), DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL_MASK,
			 1u << DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO_SEL__SHIFT);
	frb_dce12_update(adev, DCE_REG(mmDCCG_AUDIO_DTO1_MODULE), DCCG_AUDIO_DTO1_MODULE__DCCG_AUDIO_DTO1_MODULE_MASK,
			 d->dce12.audio_dto_khz * 10);
	frb_dce12_update(adev, DCE_REG(mmDCCG_AUDIO_DTO1_PHASE), DCCG_AUDIO_DTO1_PHASE__DCCG_AUDIO_DTO1_PHASE_MASK,
			 24 * 10000);
	frb_dce12_update(adev, DCE_REG(mmDCCG_AUDIO_DTO_SOURCE), DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO2_USE_512FBR_DTO_MASK,
			 DCCG_AUDIO_DTO_SOURCE__DCCG_AUDIO_DTO2_USE_512FBR_DTO_MASK);
}

/* dce_stream_encoder.c speakers_to_channels: the CEA speaker flags to AFMT channel bits */
static uint32_t speakers_to_channels(uint8_t spk)
{
	uint32_t ch = 0;
	int fl_fr = spk & 1, lfe = (spk >> 1) & 1, fc = (spk >> 2) & 1, rl_rr = (spk >> 3) & 1;
	int rc = (spk >> 4) & 1, flc_frc = (spk >> 5) & 1, rlc_rrc = (spk >> 6) & 1;

	/* audio_cea_channels: FL, FR, LFE, FC, RL_RC, RR, RC_RLC_FLC, RRC_FRC (bits 0-7) */
	ch |= fl_fr ? 0x03 : 0;
	ch |= lfe << 2;
	ch |= fc << 3;
	if (rl_rr)
		ch |= 0x30;		/* RC's channel 7 is overwritten just below, as DC does */
	else
		ch |= rc << 4;
	if (flc_frc || rlc_rrc)
		ch |= 0xc0;
	return ch;
}

/* the sink's audio as DM hands it to DC (amdgpu_dm_helpers dm_helpers_parse_edid_caps) */
static int sink_audio(const struct frb_connector *c, struct frb_edid_audio *aud, uint8_t *spk)
{
	if (!c->edid_len || !frb_edid_extract_audio(c->edid, c->edid_len, aud) || !aud->has_audio)
		return 0;
	*spk = aud->has_speaker_alloc ? aud->speaker_alloc : DEFAULT_SPEAKER_LOCATION;
	return 1;
}

/* dce110_se_audio_setup: the stream encoder's audio source (the endpoint) and its channels */
static void se_audio_setup(struct frb_device *adev, int fe, int ep, uint8_t spk)
{
	WREG32(DIGI_REG(fe, AFMT_AUDIO_SRC_CONTROL),
	       ((uint32_t)ep << DIG0_AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT__SHIFT) &
	       DIG0_AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK);
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_AUDIO_PACKET_CONTROL2),
			 DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE_MASK,
			 speakers_to_channels(spk) << DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE__SHIFT);
}

/* dce_aud_az_configure for a DP signal */
static void az_configure(struct frb_device *adev, int ep, const struct frb_edid_audio *aud, uint8_t spk,
			 uint32_t rate, uint32_t lanes)
{
	uint32_t v, field, code, i, len;
	int ac3 = 0;

	v = az_rd(adev, ep, ixAZ_HOT_PLUG_CONTROL);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 1);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);

	v = az_rd(adev, ep, ixAZ_CHANNEL_SPEAKER);
	v = AZ_SET(v, CHANNEL_SPEAKER, SPEAKER_ALLOCATION, spk & 0x7f);
	v = AZ_SET(v, CHANNEL_SPEAKER, LFE_PLAYBACK_LEVEL, 0);
	v = AZ_SET(v, CHANNEL_SPEAKER, HDMI_CONNECTION, 0);
	v = AZ_SET(v, CHANNEL_SPEAKER, DP_CONNECTION, 0);
	field = (v & AZF(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO)) >> AZF_SHIFT(CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO);
	v = AZ_SET(v, CHANNEL_SPEAKER, EXTRA_CONNECTION_INFO, field & ~1u);
	v = AZ_SET(v, CHANNEL_SPEAKER, DP_CONNECTION, 1);
	az_wr(adev, ep, ixAZ_CHANNEL_SPEAKER, v);

	/* ACP data: SUPPORTS_AI is the speaker byte's bit 7 (audio_info flags) */
	v = az_rd(adev, ep, ixAZ_ACP_DATA);
	v = (v & ~1u) | ((spk >> 7) & 1u);
	az_wr(adev, ep, ixAZ_ACP_DATA, v);

	/* the descriptors, one per format code, from the sink's SADs (byte 2 as DM keeps it for each code) */
	for (code = 1; code <= AUDIO_FORMAT_CODE_COUNT; code++) {
		if (code == AUDIO_FORMAT_CODE_1BITAUDIO || code == AUDIO_FORMAT_CODE_DST)
			continue;
		v = 0;
		for (i = 0; i < aud->sad_count; i++) {
			const uint8_t *sad = aud->sads[i];
			uint32_t ch = (sad[0] & 7u) + 1, rates = sad[1] & 0x7f;

			if (((sad[0] >> 3) & 0xf) != code)
				continue;
			if (code == 1)	/* LPCM: the stereo rates too (check_audio_bandwidth: all pass here) */
				v |= (rates << AZF_SHIFT(AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES_STEREO)) &
				     AZF(AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES_STEREO);
			if (code == AUDIO_FORMAT_CODE_AC3)
				ac3 = 1;
			v |= ((ch - 1) << AZF_SHIFT(AUDIO_DESCRIPTOR0, MAX_CHANNELS)) & AZF(AUDIO_DESCRIPTOR0, MAX_CHANNELS);
			v |= (rates << AZF_SHIFT(AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES)) &
			     AZF(AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES);
			v |= ((uint32_t)sad[2] << AZF_SHIFT(AUDIO_DESCRIPTOR0, DESCRIPTOR_BYTE_2)) &
			     AZF(AUDIO_DESCRIPTOR0, DESCRIPTOR_BYTE_2);
			break;	/* is_audio_format_supported: the first SAD of the code (DC keeps the most channels) */
		}
		az_wr(adev, ep, ixAZ_AUDIO_DESCRIPTOR0 + code - 1, v);
	}
	if (ac3)
		WREG32(DCE_REG(mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_STREAM_FORMATS), 0x05);

	/* HBR (192 kHz, 8 channels): DC's bandwidth check passes on a four-lane HBR2/HBR3 link */
	v = az_rd(adev, ep, ixAZ_RESPONSE_HBR);
	v = AZ_SET(v, RESPONSE_HBR, HBR_CAPABLE, (rate >= 0x14 && lanes == 4) ? 1 : 0);
	az_wr(adev, ep, ixAZ_RESPONSE_HBR, v);

	/* lipsync: video, then audio latency (the EDID's progressive values, else 0) */
	v = az_rd(adev, ep, ixAZ_RESPONSE_LIPSYNC);
	v = AZ_SET(v, RESPONSE_LIPSYNC, VIDEO_LIPSYNC, aud->latency_present[0] ? aud->video_latency[0] : 0);
	az_wr(adev, ep, ixAZ_RESPONSE_LIPSYNC, v);
	v = az_rd(adev, ep, ixAZ_RESPONSE_LIPSYNC);
	v = AZ_SET(v, RESPONSE_LIPSYNC, AUDIO_LIPSYNC, aud->latency_present[0] ? aud->audio_latency[0] : 0);
	az_wr(adev, ep, ixAZ_RESPONSE_LIPSYNC, v);

	az_wr(adev, ep, ixAZ_SINK_INFO0, (uint32_t)aud->mfg_id | (uint32_t)aud->prod_id << 16);
	/* the name's length counts its terminator (DC's loop), at most 18 */
	for (len = 0; len < MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS; ) {
		if (aud->sink_name[len++] == '\0')
			break;
	}
	az_wr(adev, ep, ixAZ_SINK_INFO1, len);
	/* the port ids dc_stream_construct uses without a container id */
	az_wr(adev, ep, ixAZ_SINK_INFO2, 0x5558859e);
	az_wr(adev, ep, ixAZ_SINK_INFO3, 0x0d989449);
	for (i = 0; i < 5; i++) {
		uint32_t w = 0, k;

		for (k = 0; k < 4 && i * 4 + k < MAX_HW_AUDIO_INFO_DISPLAY_NAME_SIZE_IN_CHARS; k++) {
			uint32_t n = i * 4 + k;
			uint8_t ch = n < sizeof(aud->sink_name) ? (uint8_t)aud->sink_name[n] : 0;

			if (n >= aud->sink_name_len)
				ch = 0;
			w |= (uint32_t)ch << (8 * k);
		}
		az_wr(adev, ep, ixAZ_SINK_INFO4 + i, w);
	}

	v = az_rd(adev, ep, ixAZ_HOT_PLUG_CONTROL);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 0);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
}

/* apply_ctx_to_hw's audio: the wall DTO, then per pipe the encoder's audio and the endpoint */
static void audio_setup(struct frb_device *adev, struct frb_display *d, struct frb_connector *c, int fe,
			uint32_t rate, uint32_t lanes)
{
	struct frb_edid_audio aud;
	uint8_t spk = 0;
	int ep = audio_endpoint(d, fe);

	d->dce12.audio_ep = -1;
	if (!adev->want_audio || ep < 0 || !c->dpcd[0] || !sink_audio(c, &aud, &spk))
		return;
	audio_wall_dto_setup_dp(adev, d);
	se_audio_setup(adev, fe, ep, spk);
	az_configure(adev, ep, &aud, spk, rate, lanes);
	d->dce12.audio_ep = ep;
	frb_info(adev, "display: audio: endpoint %d on DIG%d, DTO1 24 MHz / %u kHz, %u SAD(s), speakers 0x%02x, sink \"%s\"",
		 ep, fe, d->dce12.audio_dto_khz, (unsigned)aud.sad_count, spk, aud.sink_name);
}

/* dce110_enable_audio_stream: az_enable, then the encoder's DP audio, then unmute */
static void audio_stream_enable(struct frb_device *adev, struct frb_display *d, int fe)
{
	int ep = d->dce12.audio_ep;
	uint32_t v;

	if (ep < 0)
		return;
	/* dce_aud_az_enable */
	v = az_rd(adev, ep, ixAZ_HOT_PLUG_CONTROL);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 1);
	v = AZ_SET(v, HOT_PLUG_CONTROL, AUDIO_ENABLED, 1);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 0);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
	/* dce110_se_dp_audio_enable: the AFMT clock, setup_dp_audio, enable_dp_audio */
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_CNTL), DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK,
			 DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK);
	WREG32(DPI_REG(fe, DP_SEC_AUD_N), 0x8000 & DP0_DP_SEC_AUD_N__DP_SEC_AUD_N_MASK);	/* DP_SEC_AUD_N__DEFAULT */
	WREG32(DPI_REG(fe, DP_SEC_TIMESTAMP), 1u << DP0_DP_SEC_TIMESTAMP__DP_SEC_TIMESTAMP_MODE__SHIFT);	/* AUTO_CALC */
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_AUDIO_PACKET_CONTROL), DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_60958_CS_UPDATE_MASK,
			 DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_60958_CS_UPDATE_MASK);
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_AUDIO_PACKET_CONTROL2),
			 DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_LAYOUT_OVRD_MASK |
			 DIG0_AFMT_AUDIO_PACKET_CONTROL2__AFMT_60958_OSF_OVRD_MASK, 0);
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_INFOFRAME_CONTROL0), DIG0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK,
			 DIG0_AFMT_INFOFRAME_CONTROL0__AFMT_AUDIO_INFO_UPDATE_MASK);
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_60958_0), DIG0_AFMT_60958_0__AFMT_60958_CS_CLOCK_ACCURACY_MASK, 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK,
			 DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL),
			 DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK,
			 DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK,
			 DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK);
	/* dce110_se_audio_mute_control(false) */
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_AUDIO_PACKET_CONTROL), DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK,
			 DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK);
	d->dce12.audio_on = 1;
}

/* dce110_disable_audio_stream's encoder half (link_set_dpms_off, before the blank): mute, packets off, clock off */
static void audio_stream_disable(struct frb_device *adev, struct frb_display *d, int fe)
{
	if (!d->dce12.audio_on)
		return;
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_AUDIO_PACKET_CONTROL), DIG0_AFMT_AUDIO_PACKET_CONTROL__AFMT_AUDIO_SAMPLE_SEND_MASK, 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL),
			 DP0_DP_SEC_CNTL__DP_SEC_ASP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ATP_ENABLE_MASK |
			 DP0_DP_SEC_CNTL__DP_SEC_AIP_ENABLE_MASK | DP0_DP_SEC_CNTL__DP_SEC_ACM_ENABLE_MASK |
			 DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK, 0);
	if (RREG32(DPI_REG(fe, DP_SEC_CNTL)))
		frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK,
				 DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK);
	frb_dce12_update(adev, DIGI_REG(fe, AFMT_CNTL), DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK, 0);
}

/* dce_aud_az_disable (after the info packets stop and the encoder leaves the back end): the pin's presence off */
static void audio_az_disable(struct frb_device *adev, struct frb_display *d)
{
	int ep = d->dce12.audio_ep;
	uint32_t v;

	if (!d->dce12.audio_on || ep < 0)
		return;
	v = az_rd(adev, ep, ixAZ_HOT_PLUG_CONTROL);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 1);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
	v = AZ_SET(v, HOT_PLUG_CONTROL, AUDIO_ENABLED, 0);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
	v = AZ_SET(v, HOT_PLUG_CONTROL, CLOCK_GATING_DISABLE, 0);
	az_wr(adev, ep, ixAZ_HOT_PLUG_CONTROL, v);
	(void)az_rd(adev, ep, ixAZ_HOT_PLUG_CONTROL);
	d->dce12.audio_on = 0;
}

/* dce110_link_encoder_setup for DP SST */
static void link_enc_setup_dp(struct frb_device *adev, const struct frb_connector *c)
{
	frb_dce12_update(adev, DIGI_REG(c->transmitter, DIG_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_MODE_MASK, 0);
}

/* dce110_link_encoder_connect_dig_be_to_fe: the back end's front-end source mask gets this front end */
static void connect_dig_be_to_fe(struct frb_device *adev, const struct frb_connector *c, int fe)
{
	uint32_t field = REG_GET_FIELD(RREG32(DIGI_REG(c->transmitter, DIG_BE_CNTL)), DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT);

	field |= 1u << fe;
	frb_dce12_update(adev, DIGI_REG(c->transmitter, DIG_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK,
			 REG_SET_FIELD(0, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, field));
}

/* setup_dio_stream_attribute: setup_stereo_sync (off), dp_set_stream_attribute (RGB, full range; no MSA registers) */
static void dp_set_stream_attribute(struct frb_device *adev, int fe, int crtc, int bpc)
{
	frb_dce12_update(adev, DIGI_REG(fe, DIG_FE_CNTL), DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_SELECT_MASK,
			 REG_SET_FIELD(0, DIG0_DIG_FE_CNTL, DIG_STEREOSYNC_SELECT, crtc));
	frb_dce12_update(adev, DIGI_REG(fe, DIG_FE_CNTL), DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_GATE_EN_MASK,
			 DIG0_DIG_FE_CNTL__DIG_STEREOSYNC_GATE_EN_MASK);
	frb_dce12_update(adev, DPI_REG(fe, DP_PIXEL_FORMAT), DP0_DP_PIXEL_FORMAT__DP_PIXEL_ENCODING_MASK, 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_PIXEL_FORMAT), DP0_DP_PIXEL_FORMAT__DP_COMPONENT_DEPTH_MASK,
			 REG_SET_FIELD(0, DP0_DP_PIXEL_FORMAT, DP_COMPONENT_DEPTH, bpc == 6 ? 0 : bpc == 8 ? 1 : 2));
}

/*
 * dce110_update_info_frame for DP: the VSC SDP (revision 5, colorimetry:
 * RGB, sRGB, full range, the stream's depth) in generic packet 0 when the
 * sink takes VSC colorimetry (DPRX feature list bit 3), as
 * mod_build_vsc_infopacket builds it; no SPD or HDR packet.
 */
static void update_dp_info_packets(struct frb_device *adev, const struct frb_connector *c, int fe, int bpc)
{
	int vsc = (c->dprx_feature & 0x08) != 0;
	uint32_t v;

	if (vsc) {
		uint32_t sb[8] = { 0 };

		sb[4] = (uint32_t)(bpc == 6 ? 0 : bpc == 8 ? 1 : 2) << 8;	/* DB17: bit depth */
		frb_dce12_update(adev, DIGI_REG(fe, AFMT_CNTL), DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK,
				 DIG0_AFMT_CNTL__AFMT_AUDIO_CLOCK_EN_MASK);
		(void)RREG32(DIGI_REG(fe, AFMT_VBI_PACKET_CONTROL));
		frb_dce12_update(adev, DIGI_REG(fe, AFMT_VBI_PACKET_CONTROL),
				 DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC_INDEX_MASK, 0);
		WREG32(DIGI_REG(fe, AFMT_GENERIC_HDR), 0x00 | 0x07 << 8 | 0x05 << 16 | 0x13 << 24);
		for (v = 0; v < 8; v++)
			WREG32(DIGI_REG(fe, AFMT_GENERIC_0) + v, sb[v]);
		frb_dce12_update(adev, DIGI_REG(fe, AFMT_VBI_PACKET_CONTROL),
				 DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC0_UPDATE_MASK |
				 DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC2_UPDATE_MASK,
				 DIG0_AFMT_VBI_PACKET_CONTROL__AFMT_GENERIC0_UPDATE_MASK);
	}
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_GSP0_ENABLE_MASK,
			 vsc ? DP0_DP_SEC_CNTL__DP_SEC_GSP0_ENABLE_MASK : 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_GSP2_ENABLE_MASK, 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_GSP3_ENABLE_MASK, 0);
	if (RREG32(DPI_REG(fe, DP_SEC_CNTL)))
		frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK,
				 DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK);
}

/*
 * decide_dp_link_settings: from RBR x1 up, lanes first, the first link
 * whose 8b/10b bandwidth carries the stream, within the verified caps.
 */
static int decide_link_settings(const struct frb_connector *c, uint32_t pix_khz, int bpc, uint32_t *rate,
				uint32_t *lanes)
{
	static const uint8_t rates[4] = { 0x06, 0x0a, 0x14, 0x1e };
	uint64_t need = (uint64_t)pix_khz * (uint64_t)(3 * bpc);
	uint32_t max_rate = c->verified_rate ? c->verified_rate : c->dpcd[1];
	uint32_t max_lanes = c->verified_lanes ? c->verified_lanes : (c->dpcd[2] & 0x1f);
	int i, l;

	for (i = 0; i < 4 && rates[i] <= max_rate; i++)
		for (l = 1; l <= (int)max_lanes; l <<= 1)
			if ((uint64_t)rates[i] * LINK_RATE_REF_FREQ_IN_KHZ * (uint64_t)l * 8 >= need) {
				*rate = rates[i];
				*lanes = (uint32_t)l;
				return 0;
			}
	return -ERANGE;
}

/*
 * enable_link_dp: the clocks with the symbol clock, the source OUI again (a
 * mode set's power cycle loses it), perform_link_training_with_retries
 * (the stream encoder to the back end, the PHY on, the eDP panel mode
 * check, the training; three attempts with the PHY off between).
 */
static int enable_link_dp(struct frb_device *adev, struct frb_display *d, struct frb_connector *c, int fe,
			  uint32_t pix_khz, uint32_t rate, uint32_t lanes)
{
	int attempt, status = LT_CR_FAIL;
	uint8_t edp_config = 0;

	update_clocks(adev, d, pix_khz, rate * LINK_RATE_REF_FREQ_IN_KHZ, 0, 1);
	frb_dce12_set_source_specific_data(adev, c);
	connect_dig_be_to_fe(adev, c, fe);
	for (attempt = 0; attempt < 3; attempt++) {
		frb_dce12_dp_enable_link_phy(adev, c, rate, lanes);
		/* dp_set_panel_mode(DEFAULT): DP_EDP_CONFIGURATION_SET written only when it differs */
		frb_dce12_dpcd_read(adev, c, 0x10a, &edp_config, 1);
		if (edp_config & 0x01)
			frb_dce12_dpcd_write1(adev, c, 0x10a, (uint8_t)(edp_config & ~0x01));
		status = frb_dce12_dp_perform_link_training(adev, c, rate, lanes);
		if (status == LT_SUCCESS)
			break;
		frb_dce12_dp_disable_link_phy(adev, c);
		mdelay(50 * (attempt + 1));	/* LINK_TRAINING_RETRY_DELAY grows */
	}
	c->dp_clock = rate * LINK_RATE_REF_FREQ_IN_KHZ / 10 * 10;
	c->dp_lane_count = lanes;
	c->lt_status = (uint8_t)status;
	c->lt_rate = (uint8_t)rate;
	c->lt_lanes = (uint8_t)lanes;
	return status == LT_SUCCESS ? 0 : -EIO;
}

/* dce110_stream_encoder_dp_unblank: M/N for the link, the DIG started, the steer FIFO, the video stream on */
static void dp_unblank(struct frb_device *adev, int fe, uint32_t pix_khz, uint32_t rate)
{
	uint32_t n_vid = 0x8000;
	uint32_t m_vid = (uint32_t)((uint64_t)n_vid * pix_khz / (rate * LINK_RATE_REF_FREQ_IN_KHZ));

	frb_dce12_update(adev, DPI_REG(fe, DP_VID_TIMING), DP0_DP_VID_TIMING__DP_VID_M_N_GEN_EN_MASK, 0);
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_N), DP0_DP_VID_N__DP_VID_N_MASK, n_vid);
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_M), DP0_DP_VID_M__DP_VID_M_MASK, m_vid);
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_TIMING), DP0_DP_VID_TIMING__DP_VID_M_N_GEN_EN_MASK,
			 DP0_DP_VID_TIMING__DP_VID_M_N_GEN_EN_MASK);
	frb_dce12_update(adev, DIGI_REG(fe, DIG_FE_CNTL), DIG0_DIG_FE_CNTL__DIG_START_MASK, DIG0_DIG_FE_CNTL__DIG_START_MASK);
	frb_dce12_update(adev, DPI_REG(fe, DP_STEER_FIFO), DP0_DP_STEER_FIFO__DP_STEER_FIFO_RESET_MASK, 0);
	udelay(100);
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_STREAM_CNTL), DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK,
			 DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK);
}

/* link_set_dpms_on for an SST DP stream (no DSC, no MST, no PSR) */
static int set_dpms_on(struct frb_device *adev, struct frb_display *d, struct frb_connector *c, int fe, int crtc,
		       const struct frb_mode *m, int bpc, uint32_t rate, uint32_t lanes)
{
	uint32_t early;
	uint8_t downspread = 0;
	int r;

	link_enc_setup_dp(adev, c);
	dp_set_stream_attribute(adev, fe, crtc, bpc);
	update_dp_info_packets(adev, c, fe, bpc);
	if (!c->dpcd[0]) {
		/* no sink: the clocks for the symbol rate, the video into the stream encoder, no link */
		update_clocks(adev, d, m->clock, rate * LINK_RATE_REF_FREQ_IN_KHZ, 0, 1);
		dp_unblank(adev, fe, m->clock, rate);
		return 0;
	}
	r = enable_link_dp(adev, d, c, fe, m->clock, rate, lanes);
	if (r)
		frb_err(adev, "display: link %c: HBR training failed at 0x%02x x%u", 'A' + c->transmitter, rate, lanes);
	/* the CRTC's test pattern off (video mode) */
	WREG32(CRTC_REG(crtc, CRTC_TEST_PATTERN_CONTROL), 0);
	WREG32(CRTC_REG(crtc, CRTC_TEST_PATTERN_COLOR), 0);
	WREG32(CRTC_REG(crtc, CRTC_TEST_PATTERN_PARAMETERS), 0);
	link_enc_setup_dp(adev, c);
	/* dce110_enable_stream: the stream encoder again, the info packets, the HBLANK early control */
	connect_dig_be_to_fe(adev, c, fe);
	update_dp_info_packets(adev, c, fe, bpc);
	early = m->hdisplay % lanes;
	if (!early)
		early = lanes;
	frb_dce12_update(adev, CRTC_REG(crtc, CRTC_CONTROL), CRTC0_CRTC_CONTROL__CRTC_HBLANK_EARLY_CONTROL_MASK,
			 REG_SET_FIELD(0, CRTC0_CRTC_CONTROL, CRTC_HBLANK_EARLY_CONTROL, early));
	dp_unblank(adev, fe, m->clock, rate);
	/* enable_stream_features: MSA timing parameters are not ignored (DOWNSPREAD_CTRL bit 7 stays) */
	if (frb_dce12_dpcd_read(adev, c, 0x107, &downspread, 1) >= 0 && (downspread & 0x80))
		frb_dce12_dpcd_write1(adev, c, 0x107, (uint8_t)(downspread & ~0x80));
	/* dc->hwss.enable_audio_stream */
	if (!r)
		audio_stream_enable(adev, d, fe);
	return r;
}

/* ------------------------------------------------------------------------- */
/* dce110_apply_ctx_for_surface: one full-screen, unscaled, linear 32 bpp plane */

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmPIPE0_DMIF_BUFFER_CONTROL
 * mmDMIF_PG0_DPG_PIPE_ARBITRATION_CONTROL1 mmDCFE0_DCFE_CLOCK_CONTROL mmDCP0_OUTPUT_CSC_C11_C12
 * mmDCP0_OUTPUT_CSC_C13_C14 mmDCP0_OUTPUT_CSC_C21_C22 mmDCP0_OUTPUT_CSC_C23_C24 mmDCP0_OUTPUT_CSC_C31_C32
 * mmDCP0_OUTPUT_CSC_C33_C34 mmDCP0_OUTPUT_CSC_CONTROL mmDCP0_GAMUT_REMAP_CONTROL mmDCP0_DENORM_CONTROL
 * mmDCP0_OUT_CLAMP_CONTROL_B_CB mmDCP0_OUT_CLAMP_CONTROL_G_Y mmDCP0_OUT_CLAMP_CONTROL_R_CR mmDCP0_OUT_ROUND_CONTROL
 * mmDCP0_DCP_SPATIAL_DITHER_CNTL mmLB0_LB_DATA_FORMAT mmCRTC0_CRTC_OVERSCAN_COLOR mmCRTC0_CRTC_BLANK_DATA_COLOR
 * mmLB0_LB_MEMORY_CTRL mmSCL0_SCL_F_SHARP_CONTROL mmSCL0_EXT_OVERSCAN_LEFT_RIGHT mmSCL0_EXT_OVERSCAN_TOP_BOTTOM
 * mmSCL0_SCL_BYPASS_CONTROL mmSCL0_SCL_MODE mmSCL0_VIEWPORT_START mmSCL0_VIEWPORT_SIZE mmDCP0_GRPH_ENABLE
 * mmDCP0_GRPH_CONTROL mmDCP0_GRPH_X_START mmDCP0_GRPH_Y_START mmDCP0_GRPH_X_END mmDCP0_GRPH_Y_END mmDCP0_GRPH_PITCH
 * mmDCP0_HW_ROTATION mmDCP0_GRPH_SWAP_CNTL mmDCP0_PRESCALE_GRPH_CONTROL mmDCP0_PRESCALE_VALUES_GRPH_R
 * mmDCP0_PRESCALE_VALUES_GRPH_G mmDCP0_PRESCALE_VALUES_GRPH_B mmDCP0_INPUT_GAMMA_CONTROL mmDCP0_DEGAMMA_CONTROL
 * mmDCFE0_DCFE_MEM_PWR_CTRL mmDCP0_REGAMMA_CONTROL mmDCP0_GRPH_UPDATE mmDCP0_GRPH_FLIP_CONTROL
 * mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS_HIGH mmDCP0_GRPH_PRIMARY_SURFACE_ADDRESS mmBLND0_BLND_CONTROL
 * mmBLND0_BLND_V_UPDATE_LOCK
 */
#define FRB_DCE12_DCP_STRIDE	(mmDCP1_GRPH_ENABLE - mmDCP0_GRPH_ENABLE)
#define FRB_DCE12_LB_STRIDE	(mmLB1_LB_DATA_FORMAT - mmLB0_LB_DATA_FORMAT)
#define FRB_DCE12_SCL_STRIDE	(mmSCL1_SCL_MODE - mmSCL0_SCL_MODE)
#define FRB_DCE12_DCFE_STRIDE	(mmDCFE1_DCFE_CLOCK_CONTROL - mmDCFE0_DCFE_CLOCK_CONTROL)
#define FRB_DCE12_BLND_STRIDE	(mmBLND1_BLND_CONTROL - mmBLND0_BLND_CONTROL)
#define FRB_DCE12_PIPE_STRIDE	(mmPIPE1_DMIF_BUFFER_CONTROL - mmPIPE0_DMIF_BUFFER_CONTROL)
#define DCP_REG(p, r)		(DCE_REG(mmDCP0_##r) + (uint32_t)(p) * FRB_DCE12_DCP_STRIDE)
#define LB_REG(p, r)		(DCE_REG(mmLB0_##r) + (uint32_t)(p) * FRB_DCE12_LB_STRIDE)
#define SCL_REG(p, r)		(DCE_REG(mmSCL0_##r) + (uint32_t)(p) * FRB_DCE12_SCL_STRIDE)
#define DCFE_REG(p, r)		(DCE_REG(mmDCFE0_##r) + (uint32_t)(p) * FRB_DCE12_DCFE_STRIDE)
#define BLND_REG(p, r)		(DCE_REG(mmBLND0_##r) + (uint32_t)(p) * FRB_DCE12_BLND_STRIDE)
#define PIPE_REG(p, r)		(DCE_REG(mmPIPE0_##r) + (uint32_t)(p) * FRB_DCE12_PIPE_STRIDE)

/*
 * amdgpu's GB_ADDR_CONFIG for Vega 20 (gfx_v9_0 VEGA20_GB_ADDR_CONFIG_GOLDEN),
 * which fill_gfx9_tiling_info_from_device hands DC: 4 pipes, 16 banks, 4 SEs
 */
#define VEGA20_GB_ADDR_CONFIG_GOLDEN	0x2A114042
#define LB_MEMORY_SIZE_DCE12		0x1404	/* dce120 transform's lb_memory_size */

/*
 * The plane's legacy gamma (dce_ipp.c, dce110_set_input_transfer_func).
 *
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmDCP0_DC_LUT_RW_MODE mmDCP0_DC_LUT_RW_INDEX
 * mmDCP0_DC_LUT_SEQ_COLOR mmDCP0_DC_LUT_30_COLOR mmDCP0_DC_LUT_WRITE_EN_MASK mmDCP0_DC_LUT_CONTROL
 * mmDCP0_DC_LUT_BLACK_OFFSET_BLUE mmDCP0_DC_LUT_BLACK_OFFSET_GREEN mmDCP0_DC_LUT_BLACK_OFFSET_RED
 * mmDCP0_DC_LUT_WHITE_OFFSET_BLUE mmDCP0_DC_LUT_WHITE_OFFSET_GREEN mmDCP0_DC_LUT_WHITE_OFFSET_RED
 */

/*
 * amdgpu_dm's __is_lut_linear: every entry within one of i * 0xffff / 255.
 * Firebrand also counts the window server's 8-bit identity widened to 16
 * bits (i << 8) as linear: its 8 significant bits are the identity.
 */
static int gamma_is_identity(uint16_t gamma[3][256])
{
	int ch, i, delta;

	for (ch = 0; ch < 3; ch++)
		for (i = 0; i < 256; i++) {
			delta = (int)gamma[ch][i] - i * 257;
			if ((gamma[ch][i] != (uint16_t)(i << 8)) && (delta < -1 || delta > 1))
				return 0;
		}
	return 1;
}

/*
 * dce_ipp_program_input_lut's sequence (the LUT memory powered for the
 * write, all three components, 256-entry mode, from index 0, powered down
 * after, the prescale bypassed and the LUT selected), with the entries as
 * the DCE 11 path writes them (dce_v11_0_crtc_load_lut: format 0, black 0,
 * white 0xffff, one DC_LUT_30_COLOR write of 10:10:10 per entry). DC's own
 * u0.12 DC_LUT_SEQ_COLOR form wants 12-bit values; given amdgpu_dm's 16-bit
 * legacy entries it wraps them (on the card a 0x449a entry read back as
 * 0x049): the DCE 11 form is the one the hardware and the Polaris path agree on.
 */
static void program_input_lut(struct frb_device *adev, int p, uint16_t gamma[3][256])
{
	int i;

	WREG32(DCFE_REG(p, DCFE_MEM_PWR_CTRL), DCFE0_DCFE_MEM_PWR_CTRL__DCP_LUT_MEM_PWR_DIS_MASK);
	WREG32(DCP_REG(p, DC_LUT_CONTROL), 0);
	WREG32(DCP_REG(p, DC_LUT_BLACK_OFFSET_BLUE), 0);
	WREG32(DCP_REG(p, DC_LUT_BLACK_OFFSET_GREEN), 0);
	WREG32(DCP_REG(p, DC_LUT_BLACK_OFFSET_RED), 0);
	WREG32(DCP_REG(p, DC_LUT_WHITE_OFFSET_BLUE), 0xffff);
	WREG32(DCP_REG(p, DC_LUT_WHITE_OFFSET_GREEN), 0xffff);
	WREG32(DCP_REG(p, DC_LUT_WHITE_OFFSET_RED), 0xffff);
	frb_dce12_update(adev, DCP_REG(p, DC_LUT_RW_MODE), DCP0_DC_LUT_RW_MODE__DC_LUT_RW_MODE_MASK, 0);
	WREG32(DCP_REG(p, DC_LUT_WRITE_EN_MASK), 7);
	WREG32(DCP_REG(p, DC_LUT_RW_INDEX), 0);
	for (i = 0; i < 256; i++)
		WREG32(DCP_REG(p, DC_LUT_30_COLOR), ((uint32_t)(gamma[0][i] & 0xffc0) << 14) |
		       ((uint32_t)(gamma[1][i] & 0xffc0) << 4) | (gamma[2][i] >> 6));
	WREG32(DCFE_REG(p, DCFE_MEM_PWR_CTRL), 0);
	frb_dce12_update(adev, DCP_REG(p, PRESCALE_GRPH_CONTROL), DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK,
			 DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK);
	frb_dce12_update(adev, DCP_REG(p, INPUT_GAMMA_CONTROL), DCP0_INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE_MASK, 0);
}

/*
 * dce110_set_input_transfer_func for a 32 bpp surface: the prescale (scale
 * 0x2020 = 1.0 in DC's 2.13, bias 0, dce_ipp_program_prescale: bypassed
 * while written, then on with the legacy LUT in bypass), the legacy LUT when
 * the gamma is not the identity, no degamma (the in transfer function
 * bypassed, as amdgpu_dm leaves it)
 */
static void set_input_transfer_func(struct frb_device *adev, int p, uint16_t gamma[3][256], uint32_t bpp)
{
	/* dce110_prescale_params: 0x2020 for 8-bit components, 0x2082 for 5-bit ones (DC's RGB565 value) */
	uint32_t scale = bpp == 16 ? 0x2082 : 0x2020;

	frb_dce12_update(adev, DCP_REG(p, PRESCALE_GRPH_CONTROL), DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK,
			 DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK);
	WREG32(DCP_REG(p, PRESCALE_VALUES_GRPH_R), scale << 16);
	WREG32(DCP_REG(p, PRESCALE_VALUES_GRPH_G), scale << 16);
	WREG32(DCP_REG(p, PRESCALE_VALUES_GRPH_B), scale << 16);
	frb_dce12_update(adev, DCP_REG(p, PRESCALE_GRPH_CONTROL), DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_BYPASS_MASK, 0);
	frb_dce12_update(adev, DCP_REG(p, INPUT_GAMMA_CONTROL), DCP0_INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE_MASK,
			 DCP0_INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE_MASK);
	/* at 8 bpp the LUT is the palette: always on */
	if (bpp == 8 || !gamma_is_identity(gamma))
		program_input_lut(adev, p, gamma);
	WREG32(DCP_REG(p, DEGAMMA_CONTROL), 0);
}

/*
 * dce_pipe_control_lock: on, not on a blanked CRTC, the DCP and SCL locks,
 * then (from the value read, as DC writes it) the blender lock and its mode;
 * off, both cleared from a fresh read
 */
static void pipe_lock(struct frb_device *adev, int crtc, int on)
{
	uint32_t lock;

	if (on) {
		if (frb_dce12_tg_is_blanked(adev, crtc))
			return;
		lock = RREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK));
		WREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK), lock | BLND0_BLND_V_UPDATE_LOCK__BLND_DCP_GRPH_V_UPDATE_LOCK_MASK |
		       BLND0_BLND_V_UPDATE_LOCK__BLND_SCL_V_UPDATE_LOCK_MASK);
		WREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK), lock | BLND0_BLND_V_UPDATE_LOCK__BLND_BLND_V_UPDATE_LOCK_MASK |
		       BLND0_BLND_V_UPDATE_LOCK__BLND_V_UPDATE_LOCK_MODE_MASK);
		return;
	}
	lock = RREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK));
	lock &= ~(BLND0_BLND_V_UPDATE_LOCK__BLND_DCP_GRPH_V_UPDATE_LOCK_MASK | BLND0_BLND_V_UPDATE_LOCK__BLND_SCL_V_UPDATE_LOCK_MASK);
	WREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK), lock);
	lock &= ~(BLND0_BLND_V_UPDATE_LOCK__BLND_BLND_V_UPDATE_LOCK_MASK | BLND0_BLND_V_UPDATE_LOCK__BLND_V_UPDATE_LOCK_MODE_MASK);
	WREG32(BLND_REG(crtc, BLND_V_UPDATE_LOCK), lock);
}

static void program_plane(struct frb_device *adev, int p, int crtc, const struct frb_mode *m, uint64_t fb_mc,
			  uint32_t pitch_pixels, uint32_t bpp, int bpc, uint16_t gamma[3][256])
{
	uint32_t gb = VEGA20_GB_ADDR_CONFIG_GOLDEN, v;
	int i;

	pipe_lock(adev, crtc, 1);

	/* dce_mi_allocate_dmif: two DMIF buffers, the pixel duration in ps */
	v = RREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL));
	if (REG_GET_FIELD(v, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATED) != 2) {
		WREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL), REG_SET_FIELD(v, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATED, 2));
		for (i = 0; i < 100; i++) {
			if (RREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL)) & PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK)
				break;
			udelay(10);
		}
		frb_dce12_update(adev, DMIF_REG(p, DPG_PIPE_ARBITRATION_CONTROL1),
				 DMIF_PG0_DPG_PIPE_ARBITRATION_CONTROL1__PIXEL_DURATION_MASK, 1000000000u / m->clock);
	}
	/* dce_enable_fe_clock */
	frb_dce12_update(adev, DCFE_REG(p, DCFE_CLOCK_CONTROL), DCFE0_DCFE_CLOCK_CONTROL__DCFE_CLOCK_ENABLE_MASK,
			 DCFE0_DCFE_CLOCK_CONTROL__DCFE_CLOCK_ENABLE_MASK);
	/* set_default_colors -> dce110_opp_set_csc_default: sRGB full range, the identity (S2.13) in set A */
	WREG32(DCP_REG(p, OUTPUT_CSC_C11_C12), 0x00002000);
	WREG32(DCP_REG(p, OUTPUT_CSC_C13_C14), 0);
	WREG32(DCP_REG(p, OUTPUT_CSC_C21_C22), 0x20000000);
	WREG32(DCP_REG(p, OUTPUT_CSC_C23_C24), 0);
	WREG32(DCP_REG(p, OUTPUT_CSC_C31_C32), 0);
	WREG32(DCP_REG(p, OUTPUT_CSC_C33_C34), 0x00002000);
	WREG32(DCP_REG(p, OUTPUT_CSC_CONTROL), 0);
	WREG32(DCP_REG(p, OUTPUT_CSC_CONTROL), 4);	/* OUTPUT_CSC_GRPH_MODE: set A */
	/* transform_set_gamut_remap: bypass */
	WREG32(DCP_REG(p, GAMUT_REMAP_CONTROL), 0);
	/*
	 * program_scaler -> dce_transform_set_pixel_storage_depth (LB 30 bpp):
	 * denormalisation to the stream's depth, its clamp and rounding, the
	 * DCP's spatial dither (highpass + RGB random, mode 1), then the LB format
	 */
	WREG32(DCP_REG(p, DENORM_CONTROL), bpc == 6 ? 1 : bpc == 8 ? 2 : 3);
	v = bpc == 6 ? 0x3f00 : bpc == 8 ? 0x3fc0 : 0x3ff0;
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_B_CB), v);
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_G_Y), v);
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_R_CR), v);
	WREG32(DCP_REG(p, OUT_ROUND_CONTROL), 0);
	WREG32(DCP_REG(p, DCP_SPATIAL_DITHER_CNTL), DCP0_DCP_SPATIAL_DITHER_CNTL__DCP_SPATIAL_DITHER_EN_MASK |
	       REG_SET_FIELD(0, DCP0_DCP_SPATIAL_DITHER_CNTL, DCP_SPATIAL_DITHER_MODE, 1) |
	       DCP0_DCP_SPATIAL_DITHER_CNTL__DCP_RGB_RANDOM_ENABLE_MASK |
	       DCP0_DCP_SPATIAL_DITHER_CNTL__DCP_HIGHPASS_RANDOM_ENABLE_MASK);
	frb_dce12_update(adev, LB_REG(p, LB_DATA_FORMAT), LB0_LB_DATA_FORMAT__PIXEL_DEPTH_MASK |
			 LB0_LB_DATA_FORMAT__PIXEL_EXPAN_MODE_MASK, LB0_LB_DATA_FORMAT__PIXEL_EXPAN_MODE_MASK);
	/* dce120 set_overscan_blank_color: black overscan, black and blank-data colours */
	WREG32(CRTC_REG(crtc, CRTC_OVERSCAN_COLOR), 0);
	v = RREG32(CRTC_REG(crtc, CRTC_OVERSCAN_COLOR));
	WREG32(CRTC_REG(crtc, CRTC_BLACK_COLOR), v);
	WREG32(CRTC_REG(crtc, CRTC_BLANK_DATA_COLOR), v);
	/* dce_transform_set_scaler: the whole LB, no sharpening, no overscan, bypass (no scaling), the viewport */
	WREG32(LB_REG(p, LB_MEMORY_CTRL), REG_SET_FIELD(0, LB0_LB_MEMORY_CTRL, LB_MEMORY_SIZE, LB_MEMORY_SIZE_DCE12));
	WREG32(SCL_REG(p, SCL_F_SHARP_CONTROL), 0);
	WREG32(SCL_REG(p, EXT_OVERSCAN_LEFT_RIGHT), 0);
	WREG32(SCL_REG(p, EXT_OVERSCAN_TOP_BOTTOM), 0);
	WREG32(SCL_REG(p, SCL_BYPASS_CONTROL), 0);
	frb_dce12_update(adev, SCL_REG(p, SCL_MODE), SCL0_SCL_MODE__SCL_MODE_MASK, 0);
	WREG32(SCL_REG(p, VIEWPORT_START), 0);
	WREG32(SCL_REG(p, VIEWPORT_SIZE), REG_SET_FIELD(0, SCL0_VIEWPORT_SIZE, VIEWPORT_WIDTH, m->hdisplay) |
	       REG_SET_FIELD(0, SCL0_VIEWPORT_SIZE, VIEWPORT_HEIGHT, m->vdisplay));
	frb_dce12_update(adev, LB_REG(p, LB_DATA_FORMAT), LB0_LB_DATA_FORMAT__ALPHA_EN_MASK, 0);
	/* dce_mi_program_surface_config: linear (GFX9 swizzle 0), the size, no rotation, ARGB8888 */
	frb_dce12_update(adev, DCP_REG(p, GRPH_ENABLE), DCP0_GRPH_ENABLE__GRPH_ENABLE_MASK, DCP0_GRPH_ENABLE__GRPH_ENABLE_MASK);
	v = REG_SET_FIELD(0, DCP0_GRPH_CONTROL, GRPH_SW_MODE, 0);
	v = REG_SET_FIELD(v, DCP0_GRPH_CONTROL, GRPH_NUM_BANKS, (gb >> 12) & 7);
	v = REG_SET_FIELD(v, DCP0_GRPH_CONTROL, GRPH_NUM_SHADER_ENGINES, (gb >> 19) & 3);
	v = REG_SET_FIELD(v, DCP0_GRPH_CONTROL, GRPH_NUM_PIPES, gb & 7);
	v = REG_SET_FIELD(v, DCP0_GRPH_CONTROL, GRPH_COLOR_EXPANSION_MODE, 1);
	v = REG_SET_FIELD(v, DCP0_GRPH_CONTROL, GRPH_SE_ENABLE, 1);
	frb_dce12_update(adev, DCP_REG(p, GRPH_CONTROL), DCP0_GRPH_CONTROL__GRPH_SW_MODE_MASK |
			 DCP0_GRPH_CONTROL__GRPH_NUM_BANKS_MASK | DCP0_GRPH_CONTROL__GRPH_NUM_SHADER_ENGINES_MASK |
			 DCP0_GRPH_CONTROL__GRPH_NUM_PIPES_MASK | DCP0_GRPH_CONTROL__GRPH_COLOR_EXPANSION_MODE_MASK |
			 DCP0_GRPH_CONTROL__GRPH_SE_ENABLE_MASK, v);
	WREG32(DCP_REG(p, GRPH_X_START), 0);
	WREG32(DCP_REG(p, GRPH_Y_START), 0);
	WREG32(DCP_REG(p, GRPH_X_END), m->hdisplay);
	WREG32(DCP_REG(p, GRPH_Y_END), m->vdisplay);
	WREG32(DCP_REG(p, GRPH_PITCH), pitch_pixels);
	WREG32(DCP_REG(p, HW_ROTATION), 0);
	/*
	 * program_grph_pixel_format: no crossbar swap; a big-endian host writes
	 * its pixels as big-endian words, so the scanout swaps 8-in-32 (amdgpu's
	 * own __BIG_ENDIAN arm in dce_v11_0_crtc_do_set_base, as on Polaris)
	 */
	WREG32(DCP_REG(p, GRPH_SWAP_CNTL), FRB_BIG_ENDIAN && bpp > 8 ?
	       REG_SET_FIELD(0, DCP0_GRPH_SWAP_CNTL, GRPH_ENDIAN_SWAP, bpp == 32 ? 2 : 1) : 0);
	/*
	 * GRPH_DEPTH 2 format 0 ARGB8888 (DC's); the Mac's other depths, which
	 * DC also has: 1 / 0 ARGB1555 (swapped 8-in-16 on a big-endian host),
	 * 0 / 0 the 256-colour palette through the legacy LUT
	 */
	frb_dce12_update(adev, DCP_REG(p, GRPH_CONTROL), DCP0_GRPH_CONTROL__GRPH_DEPTH_MASK | DCP0_GRPH_CONTROL__GRPH_FORMAT_MASK,
			 REG_SET_FIELD(0, DCP0_GRPH_CONTROL, GRPH_DEPTH, bpp == 32 ? 2 : bpp == 16 ? 1 : 0));
	frb_dce12_update(adev, DCP_REG(p, PRESCALE_GRPH_CONTROL), DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_SELECT_MASK |
			 DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_R_SIGN_MASK | DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_G_SIGN_MASK |
			 DCP0_PRESCALE_GRPH_CONTROL__GRPH_PRESCALE_B_SIGN_MASK, 0);
	set_input_transfer_func(adev, p, gamma, bpp);
	/* dce110_set_output_transfer_func: the regamma LUT memories on, regamma in bypass, off again */
	frb_dce12_update(adev, DCFE_REG(p, DCFE_MEM_PWR_CTRL), DCFE0_DCFE_MEM_PWR_CTRL__DCP_LUT_MEM_PWR_DIS_MASK |
			 DCFE0_DCFE_MEM_PWR_CTRL__DCP_REGAMMA_MEM_PWR_DIS_MASK,
			 DCFE0_DCFE_MEM_PWR_CTRL__DCP_LUT_MEM_PWR_DIS_MASK | DCFE0_DCFE_MEM_PWR_CTRL__DCP_REGAMMA_MEM_PWR_DIS_MASK);
	WREG32(DCP_REG(p, REGAMMA_CONTROL), 0);
	frb_dce12_update(adev, DCFE_REG(p, DCFE_MEM_PWR_CTRL), DCFE0_DCFE_MEM_PWR_CTRL__DCP_LUT_MEM_PWR_DIS_MASK |
			 DCFE0_DCFE_MEM_PWR_CTRL__DCP_REGAMMA_MEM_PWR_DIS_MASK, 0);
	/* update_plane_addr -> dce_mi_program_surface_flip_and_addr: locked, flip at vertical retrace off, the address */
	frb_dce12_update(adev, DCP_REG(p, GRPH_UPDATE), DCP0_GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK,
			 DCP0_GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK);
	frb_dce12_update(adev, DCP_REG(p, GRPH_FLIP_CONTROL), DCP0_GRPH_FLIP_CONTROL__GRPH_SURFACE_UPDATE_H_RETRACE_EN_MASK, 0);
	WREG32(DCP_REG(p, GRPH_PRIMARY_SURFACE_ADDRESS_HIGH), (uint32_t)(fb_mc >> 32));
	WREG32(DCP_REG(p, GRPH_PRIMARY_SURFACE_ADDRESS), (uint32_t)fb_mc & ~0xffu);
	frb_dce12_update(adev, DCP_REG(p, GRPH_UPDATE), DCP0_GRPH_UPDATE__GRPH_UPDATE_LOCK_MASK, 0);
	/* program_surface_visibility: dce_set_blender_mode(current pipe), then unblank */
	frb_dce12_update(adev, BLND_REG(crtc, BLND_CONTROL), BLND0_BLND_CONTROL__BLND_MODE_MASK, 0);
	frb_dce12_update(adev, BLND_REG(crtc, BLND_CONTROL), BLND0_BLND_CONTROL__BLND_FEEDTHROUGH_EN_MASK |
			 BLND0_BLND_CONTROL__BLND_ALPHA_MODE_MASK | BLND0_BLND_CONTROL__BLND_MULTIPLIED_MODE_MASK,
			 REG_SET_FIELD(0, BLND0_BLND_CONTROL, BLND_ALPHA_MODE, 2));
	frb_dce12_tg_set_blank(adev, crtc, 0);
	pipe_lock(adev, crtc, 0);
}

static void cursor_restore(struct frb_device *adev, struct frb_display *d);

static int set_mode_once(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			 const struct frb_mode *mode, uint64_t fb_offset, uint32_t pitch_pixels, uint32_t bpp)
{
	int crtc = 0, fe = conn->transmitter, bpc = stream_bpc(conn), r, link_r;
	uint32_t rate = 0, lanes = 0;

	conn->lt_status = LT_SUCCESS;

	if (bpp != 8 && bpp != 16 && bpp != 32)
		return -EINVAL;
	if (!d->ready)
		return -EINVAL;
	if (!conn->dpcd[0]) {
		/*
		 * Bring-up without a monitor: the pipe runs (timing, plane, the
		 * stream encoder, the pipe CRC), the link stays off. 10 bpc, HBR2 x4
		 * stand in for the sink's caps (what the LG of the capture asks).
		 */
		bpc = 10;
		rate = 0x14;
		lanes = 4;
		frb_info(adev, "display: link %c has no sink: the pipe alone (no PHY, no training)", 'A' + conn->transmitter);
	} else if (decide_link_settings(conn, mode->clock, bpc, &rate, &lanes)) {
		frb_err(adev, "display: %ux%u at %u kHz, %d bpc does not fit the DP link", mode->hdisplay, mode->vdisplay,
			mode->clock, bpc);
		return -ERANGE;
	}
	if (!bios_is_accelerated_mode(adev))
		enable_accelerated_mode(adev, d);
	prepare_bandwidth(adev, d, 0, mode->clock, 0, 1);

	/* dce110_apply_ctx_to_hw: the BIOS's critical state around the programming */
	bios_scratch_update(adev, S6_CRITICAL_STATE, 1);
	audio_setup(adev, d, conn, fe, rate, lanes);
	opp_program_fmt(adev, crtc, bpc);
	r = enable_stream_timing(adev, crtc, conn, mode);
	if (r)
		return r;
	tg_no_drr(adev, crtc);
	/* dig_connect_to_otg: the stream encoder (the link's DIG front end) takes this CRTC */
	frb_dce12_update(adev, DIGI_REG(fe, DIG_FE_CNTL), DIG0_DIG_FE_CNTL__DIG_SOURCE_SELECT_MASK,
			 REG_SET_FIELD(0, DIG0_DIG_FE_CNTL, DIG_SOURCE_SELECT, crtc));
	link_r = set_dpms_on(adev, d, conn, fe, crtc, mode, bpc, rate, lanes);
	bios_scratch_update(adev, S6_CRITICAL_STATE, 0);

	/*
	 * dce110_optimize_bandwidth: DC programs its bandwidth model's marks
	 * here; without that model the safe marks stay (urgent always, no
	 * stutter, P-state changes at the safe mark), which costs power, not
	 * pixels. Then the clocks, allowed to come down.
	 */
	set_safe_displaymarks(adev, crtc);
	update_clocks(adev, d, mode->clock, rate * LINK_RATE_REF_FREQ_IN_KHZ, 1, 1);

	/* the plane's commit: prepare_bandwidth (the requirements unchanged, so powerplay hears nothing), the plane */
	prepare_bandwidth(adev, d, crtc, mode->clock, rate * LINK_RATE_REF_FREQ_IN_KHZ, 0);
	program_plane(adev, crtc, crtc, mode, adev->gmc.vram_start + fb_offset, pitch_pixels, bpp, bpc, d->crtc.gamma);
	cursor_restore(adev, d);
	d->dce12.bpc = bpc;
	frb_info(adev, "display: link %c: %ux%u at %u kHz, %d bpc, DP %s x%u%s; scanout at VRAM +0x%llx", 'A' + conn->transmitter,
		 mode->hdisplay, mode->vdisplay, mode->clock, bpc,
		 rate == 0x1e ? "HBR3" : rate == 0x14 ? "HBR2" : rate == 0x0a ? "HBR" : "RBR", lanes,
		 link_r ? " (link NOT trained)" : "", (unsigned long long)fb_offset);
	return link_r;
}

/*
 * The mode set, and the link-rate fallback after a failed training. DC
 * trains a conventional DP link with fallback only at detection
 * (dp_verify_link_cap, frb_dce_v12_0.c) and at a mode set retries the
 * decided setting three times and gives up (perform_link_training_with_
 * retries with do_fallback false; true only for USB4 DPIA). Firebrand goes
 * on from there the way that function does with do_fallback: the next
 * setting decide_fallback_link_setting gives becomes the link's cap, and
 * while a setting within it still carries the stream the mode is set again.
 * (The first mode set of a boot once failed HBR2 x4 training during a
 * burst of EHCI errors and left the screen dark; the same link trained on
 * the next mode set.) A cap lowered this way stays until the next detection
 * when the mode trained under it; when nothing carries the stream the
 * verified cap is put back and the failure returned.
 */
int frb_dce_v12_0_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			   const struct frb_mode *mode, uint64_t fb_offset, uint32_t pitch_pixels, uint32_t bpp)
{
	uint8_t cap_rate = conn->verified_rate, cap_lanes = conn->verified_lanes;
	uint32_t max_rate, max_lanes, rate, lanes, try_rate, try_lanes;
	int r, tries;

	r = set_mode_once(adev, d, conn, mode, fb_offset, pitch_pixels, bpp);
	if (r != -EIO || !conn->dpcd[0] || conn->lt_status == LT_SUCCESS)
		return r;
	max_rate = cap_rate ? cap_rate : conn->dpcd[1];
	if (max_rate > 0x1e)
		max_rate = 0x1e;
	max_lanes = cap_lanes ? cap_lanes : (uint32_t)(conn->dpcd[2] & 0x1f);
	rate = conn->lt_rate;
	lanes = conn->lt_lanes;
	/* at most every rate and lane count once */
	for (tries = 0; tries < 12 && r == -EIO && conn->lt_status != LT_SUCCESS; tries++) {
		if (!frb_dce12_dp_decide_fallback(&max_rate, max_lanes, &rate, &lanes, conn->lt_status))
			break;
		conn->verified_rate = (uint8_t)rate;
		conn->verified_lanes = (uint8_t)lanes;
		if (decide_link_settings(conn, mode->clock, stream_bpc(conn), &try_rate, &try_lanes)) {
			frb_err(adev, "display: link %c: the fallback (0x%02x x%u) does not carry %ux%u at %u kHz",
				'A' + conn->transmitter, rate, lanes, mode->hdisplay, mode->vdisplay, mode->clock);
			break;
		}
		frb_info(adev, "display: link %c: training failed (%d) at 0x%02x x%u; falling back to at most 0x%02x x%u",
			 'A' + conn->transmitter, conn->lt_status, conn->lt_rate, conn->lt_lanes, rate, lanes);
		r = set_mode_once(adev, d, conn, mode, fb_offset, pitch_pixels, bpp);
	}
	if (r) {
		conn->verified_rate = cap_rate;
		conn->verified_lanes = cap_lanes;
	}
	return r;
}

/* ------------------------------------------------------------------------- */

/*
 * The pipe CRC as amdgpu_dm's crtc CRC source takes it (dce120_configure_crc:
 * window A and B the whole active area, continuous, CRC0), read after three
 * frames: R/Cr, G/Y, B/Cb.
 *
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmCRTC0_CRTC_CRC_CNTL
 * mmCRTC0_CRTC_CRC0_WINDOWA_X_CONTROL mmCRTC0_CRTC_CRC0_WINDOWA_Y_CONTROL mmCRTC0_CRTC_CRC0_WINDOWB_X_CONTROL
 * mmCRTC0_CRTC_CRC0_WINDOWB_Y_CONTROL mmCRTC0_CRTC_CRC0_DATA_RG mmCRTC0_CRTC_CRC0_DATA_B mmCRTC0_CRTC_STATUS_FRAME_COUNT
 */
/* dc_stream_set_dither_option(DITHER_OPTION_TRUN8): the transform's pixel storage depth, then the FMT */
static void set_dither_trun8(struct frb_device *adev, int p, int bpc)
{
	uint32_t r = FMT_REG(p, FMT_BIT_DEPTH_CONTROL), v;

	/* dce_transform_set_pixel_storage_depth (LB 30 bpp): program_bit_depth_reduction with truncation */
	WREG32(DCP_REG(p, DENORM_CONTROL), 3);
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_B_CB), 0x3ff0);
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_G_Y), 0x3ff0);
	WREG32(DCP_REG(p, OUT_CLAMP_CONTROL_R_CR), 0x3ff0);
	WREG32(DCP_REG(p, OUT_ROUND_CONTROL), 4);	/* truncate (no rounding) to 8 bits */
	WREG32(DCP_REG(p, DCP_SPATIAL_DITHER_CNTL), REG_SET_FIELD(0, DCP0_DCP_SPATIAL_DITHER_CNTL, DCP_SPATIAL_DITHER_MODE, 1) |
	       DCP0_DCP_SPATIAL_DITHER_CNTL__DCP_FRAME_RANDOM_ENABLE_MASK);
	frb_dce12_update(adev, LB_REG(p, LB_DATA_FORMAT), LB0_LB_DATA_FORMAT__PIXEL_DEPTH_MASK |
			 LB0_LB_DATA_FORMAT__PIXEL_EXPAN_MODE_MASK, LB0_LB_DATA_FORMAT__PIXEL_EXPAN_MODE_MASK);
	/* dce110_opp_program_bit_depth_reduction: truncate to 8 bpc, no spatial, no temporal dither */
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_MODE), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_MODE),
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_EN) | REG_SET_FIELD(0, FMT0_FMT_BIT_DEPTH_CONTROL, FMT_TRUNCATE_DEPTH, 1));
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_EN) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_DEPTH) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_SPATIAL_DITHER_MODE), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_HIGHPASS_RANDOM_ENABLE) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_FRAME_RANDOM_ENABLE) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_RGB_RANDOM_ENABLE), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_EN), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_EN) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_RESET) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_OFFSET), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_DITHER_DEPTH) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_TEMPORAL_LEVEL), 0);
	frb_dce12_update(adev, r, FMTF(FMT_BIT_DEPTH_CONTROL, FMT_25FRC_SEL) | FMTF(FMT_BIT_DEPTH_CONTROL, FMT_50FRC_SEL) |
			 FMTF(FMT_BIT_DEPTH_CONTROL, FMT_75FRC_SEL), 0);
	/* the stream's dynamic expansion as the mode set left it */
	v = FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) | FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE);
	frb_dce12_update(adev, FMT_REG(p, FMT_DYNAMIC_EXP_CNTL), v, 0);
	if (bpc >= 8)
		frb_dce12_update(adev, FMT_REG(p, FMT_DYNAMIC_EXP_CNTL), v,
				 FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_EN) |
				 (bpc == 8 ? FMTF(FMT_DYNAMIC_EXP_CNTL, FMT_DYNAMIC_EXP_MODE) : 0));
}

int frb_dce_v12_0_pipe_crc(struct frb_device *adev, struct frb_display *d, uint16_t crc[3])
{
	const struct frb_mode *m = &d->crtc.mode;
	int crtc = 0;
	uint32_t x = (uint32_t)m->hdisplay << 16, y = (uint32_t)m->vdisplay << 16, v;

	if (!d->crtc.enabled)
		return -ENODEV;
	WREG32(CRTC_REG(crtc, CRTC_CRC_CNTL), 0);
	WREG32(CRTC_REG(crtc, CRTC_CRC0_WINDOWA_X_CONTROL), x);
	WREG32(CRTC_REG(crtc, CRTC_CRC0_WINDOWA_Y_CONTROL), y);
	WREG32(CRTC_REG(crtc, CRTC_CRC0_WINDOWB_X_CONTROL), x);
	WREG32(CRTC_REG(crtc, CRTC_CRC0_WINDOWB_Y_CONTROL), y);
	WREG32(CRTC_REG(crtc, CRTC_CRC_CNTL), CRTC0_CRTC_CRC_CNTL__CRTC_CRC_EN_MASK | CRTC0_CRTC_CRC_CNTL__CRTC_CRC_CONT_EN_MASK);
	/*
	 * amdgpu_dm_crtc_configure_crc_source: with a CRC source on, the stream
	 * goes to DITHER_OPTION_TRUN8 (dc_stream_set_dither_option) so the CRC
	 * is the same frame after frame: the DCP rounds to 8 bits without its
	 * dither, the FMT truncates to 8 bits. The mode set's dither returns
	 * with the next mode set.
	 */
	set_dither_trun8(adev, crtc, d->dce12.bpc);
	mdelay(3 * 1000 / 24 + 10);	/* three frames at 24 Hz or more */
	v = RREG32(CRTC_REG(crtc, CRTC_CRC0_DATA_RG));
	crc[0] = (uint16_t)REG_GET_FIELD(v, CRTC0_CRTC_CRC0_DATA_RG, CRC0_R_CR);
	crc[1] = (uint16_t)REG_GET_FIELD(v, CRTC0_CRTC_CRC0_DATA_RG, CRC0_G_Y);
	crc[2] = (uint16_t)REG_GET_FIELD(RREG32(CRTC_REG(crtc, CRTC_CRC0_DATA_B)), CRTC0_CRTC_CRC0_DATA_B, CRC0_B_CB);
	return 0;
}

/* frb_display_report for DCE 12: is CRTC0 counting frames, its blank state */
void frb_dce_v12_0_report(struct frb_device *adev, struct frb_display *d)
{
	uint32_t a, b;

	a = RREG32(CRTC_REG(0, CRTC_STATUS_FRAME_COUNT));
	mdelay(40);
	b = RREG32(CRTC_REG(0, CRTC_STATUS_FRAME_COUNT));
	frb_info(adev, "display: CRTC0 frame count %u -> %u in 40 ms, blank control %08x, DP%d video %08x",
		 a, b, RREG32(CRTC_REG(0, CRTC_BLANK_CONTROL)), d->crtc.conn ? d->crtc.conn->transmitter : -1,
		 d->crtc.conn ? RREG32(DPI_REG(d->crtc.conn->transmitter, DP_VID_STREAM_CNTL)) : 0);
}

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmDCP0_CUR_UPDATE mmDCP0_CUR_CONTROL
 * mmDCP0_CUR_POSITION mmDCP0_CUR_HOT_SPOT mmDCP0_CUR_SIZE mmDCP0_CUR_SURFACE_ADDRESS mmDCP0_CUR_SURFACE_ADDRESS_HIGH
 */
/* dce_ipp_cursor_set_position: under the cursor's own update lock */
static void cursor_set_position(struct frb_device *adev, int p, int enable, uint32_t x, uint32_t y, uint32_t hot_x,
				uint32_t hot_y)
{
	frb_dce12_update(adev, DCP_REG(p, CUR_UPDATE), DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK,
			 DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
	frb_dce12_update(adev, DCP_REG(p, CUR_CONTROL), DCP0_CUR_CONTROL__CURSOR_EN_MASK,
			 enable ? DCP0_CUR_CONTROL__CURSOR_EN_MASK : 0);
	WREG32(DCP_REG(p, CUR_POSITION), REG_SET_FIELD(REG_SET_FIELD(0, DCP0_CUR_POSITION, CURSOR_X_POSITION, x),
						      DCP0_CUR_POSITION, CURSOR_Y_POSITION, y));
	WREG32(DCP_REG(p, CUR_HOT_SPOT), REG_SET_FIELD(REG_SET_FIELD(0, DCP0_CUR_HOT_SPOT, CURSOR_HOT_SPOT_X, hot_x),
						      DCP0_CUR_HOT_SPOT, CURSOR_HOT_SPOT_Y, hot_y));
	frb_dce12_update(adev, DCP_REG(p, CUR_UPDATE), DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK, 0);
}

/* dce_ipp_cursor_set_attributes: 24.8 ARGB, premultiplied alpha (amdgpu_dm's), no magnify */
static void cursor_set_attributes(struct frb_device *adev, int p, uint64_t addr, uint32_t width, uint32_t height)
{
	frb_dce12_update(adev, DCP_REG(p, CUR_UPDATE), DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK,
			 DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK);
	frb_dce12_update(adev, DCP_REG(p, CUR_CONTROL), DCP0_CUR_CONTROL__CURSOR_MODE_MASK |
			 DCP0_CUR_CONTROL__CURSOR_2X_MAGNIFY_MASK | DCP0_CUR_CONTROL__CUR_INV_TRANS_CLAMP_MASK,
			 REG_SET_FIELD(0, DCP0_CUR_CONTROL, CURSOR_MODE, 2));
	WREG32(DCP_REG(p, CUR_SIZE), REG_SET_FIELD(REG_SET_FIELD(0, DCP0_CUR_SIZE, CURSOR_WIDTH, width - 1),
						   DCP0_CUR_SIZE, CURSOR_HEIGHT, height - 1));
	WREG32(DCP_REG(p, CUR_SURFACE_ADDRESS_HIGH), (uint32_t)(addr >> 32));
	WREG32(DCP_REG(p, CUR_SURFACE_ADDRESS), (uint32_t)addr);
	frb_dce12_update(adev, DCP_REG(p, CUR_UPDATE), DCP0_CUR_UPDATE__CURSOR_UPDATE_LOCK_MASK, 0);
}

/*
 * dce110_set_cursor_position with amdgpu_dm's get_cursor_position: a cursor
 * off the top or left edge sits at 0 with the hot spot taking the overhang
 */
static void cursor_position(struct frb_device *adev, int p, int x, int y, int enable)
{
	uint32_t xo = 0, yo = 0;

	if (x < 0) {
		xo = -x < FRB_CURSOR_MAX - 1 ? (uint32_t)-x : FRB_CURSOR_MAX - 1;
		x = 0;
	}
	if (y < 0) {
		yo = -y < FRB_CURSOR_MAX - 1 ? (uint32_t)-y : FRB_CURSOR_MAX - 1;
		y = 0;
	}
	cursor_set_position(adev, p, enable, (uint32_t)x, (uint32_t)y, xo, yo);
}

/*
 * frb_display_cursor_set/move for DCE 12 (amdgpu_dm's handle_cursor_update:
 * dc_stream_program_cursor_attributes, then _position, each under the pipe
 * lock DC takes for the cursor on DCE). The surface is FRB_CURSOR_MAX
 * pixels a row, as the DCE 11 path's.
 */
int frb_dce_v12_0_cursor_set(struct frb_device *adev, struct frb_display *d, uint64_t addr, uint32_t width,
			     uint32_t height)
{
	struct frb_crtc *c = &d->crtc;

	c->cursor_addr = addr;
	c->cursor_width = width;
	c->cursor_height = height;
	if (!c->enabled)
		return 0;
	pipe_lock(adev, 0, 1);
	if (addr)
		cursor_set_attributes(adev, 0, addr, width, height);
	cursor_position(adev, 0, c->cursor_x, c->cursor_y, addr && c->cursor_visible);
	pipe_lock(adev, 0, 0);
	return 0;
}

int frb_dce_v12_0_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible)
{
	struct frb_crtc *c = &d->crtc;

	c->cursor_x = x;
	c->cursor_y = y;
	c->cursor_visible = visible;
	if (!c->enabled || !c->cursor_addr)
		return 0;
	pipe_lock(adev, 0, 1);
	cursor_position(adev, 0, x, y, visible);
	pipe_lock(adev, 0, 0);
	return 0;
}

/* after a mode set: the cursor as it was (nothing when there is none: the replay's mode set) */
static void cursor_restore(struct frb_device *adev, struct frb_display *d)
{
	if (d->crtc.cursor_addr)
		frb_dce_v12_0_cursor_set(adev, d, d->crtc.cursor_addr, d->crtc.cursor_width, d->crtc.cursor_height);
}

/* frbctl lut on DCE 12: the legacy LUT as the hardware holds it, 10 bits a component (DC_LUT_30_COLOR) */
int frb_dce_v12_0_read_lut(struct frb_device *adev, struct frb_display *d, uint32_t *out)
{
	int i;

	if (!d->crtc.enabled)
		return -ENODEV;
	/* the identity gamma leaves the legacy LUT in bypass (GRPH_INPUT_GAMMA_MODE 1): nothing to compare */
	if (RREG32(DCP_REG(0, INPUT_GAMMA_CONTROL)) & DCP0_INPUT_GAMMA_CONTROL__GRPH_INPUT_GAMMA_MODE_MASK)
		return -ENOENT;
	frb_dce12_update(adev, DCP_REG(0, DC_LUT_RW_MODE), DCP0_DC_LUT_RW_MODE__DC_LUT_RW_MODE_MASK, 0);
	for (i = 0; i < 256; i++) {
		WREG32(DCP_REG(0, DC_LUT_RW_INDEX), i);
		out[i] = RREG32(DCP_REG(0, DC_LUT_30_COLOR));
	}
	return 0;
}

/* frb_display_set_gamma on a running pipe: the input transfer function again under the pipe lock */
void frb_dce_v12_0_load_gamma(struct frb_device *adev, struct frb_display *d)
{
	if (!d->crtc.enabled)
		return;
	pipe_lock(adev, 0, 1);
	set_input_transfer_func(adev, 0, d->crtc.gamma, d->crtc.fb_bpp ? d->crtc.fb_bpp : 32);
	pipe_lock(adev, 0, 0);
}

/*
 * dce110_stream_encoder_dp_blank: the video stream off at the next vertical
 * blank (DIS_DEFER 2), waited for (10 us steps, 5000 of them), then the
 * steer FIFO held in reset so the encoder ignores the CRTC's timing
 */
static void dp_blank(struct frb_device *adev, int fe)
{
	int i;

	if (!(RREG32(DPI_REG(fe, DP_VID_STREAM_CNTL)) & DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK))
		return;
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_STREAM_CNTL), DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_DIS_DEFER_MASK,
			 REG_SET_FIELD(0, DP0_DP_VID_STREAM_CNTL, DP_VID_STREAM_DIS_DEFER, 2));
	frb_dce12_update(adev, DPI_REG(fe, DP_VID_STREAM_CNTL), DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_ENABLE_MASK, 0);
	for (i = 0; i < 5000; i++) {
		if (!(RREG32(DPI_REG(fe, DP_VID_STREAM_CNTL)) & DP0_DP_VID_STREAM_CNTL__DP_VID_STREAM_STATUS_MASK))
			break;
		udelay(10);
	}
	frb_dce12_update(adev, DPI_REG(fe, DP_STEER_FIFO), DP0_DP_STEER_FIFO__DP_STEER_FIFO_RESET_MASK,
			 DP0_DP_STEER_FIFO__DP_STEER_FIFO_RESET_MASK);
}

/* dce110_stream_encoder_stop_dp_info_packets: every generic packet off, the SEC stream off if nothing is left */
static void stop_dp_info_packets(struct frb_device *adev, int fe)
{
	WREG32(DPI_REG(fe, DP_SEC_CNTL), 0);
	if (RREG32(DPI_REG(fe, DP_SEC_CNTL)))
		frb_dce12_update(adev, DPI_REG(fe, DP_SEC_CNTL), DP0_DP_SEC_CNTL__DP_SEC_STREAM_ENABLE_MASK, 0);
}

/* dce110_link_encoder_connect_dig_be_to_fe(false): the front end leaves the back end's source mask */
static void disconnect_dig_be_from_fe(struct frb_device *adev, const struct frb_connector *c, int fe)
{
	uint32_t field = REG_GET_FIELD(RREG32(DIGI_REG(c->transmitter, DIG_BE_CNTL)), DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT);

	field &= ~(1u << fe);
	frb_dce12_update(adev, DIGI_REG(c->transmitter, DIG_BE_CNTL), DIG0_DIG_BE_CNTL__DIG_FE_SOURCE_SELECT_MASK,
			 REG_SET_FIELD(0, DIG0_DIG_BE_CNTL, DIG_FE_SOURCE_SELECT, field));
}

/* dce_mi_free_dmif: the pipe's DMIF buffers released, waited for (10 us steps, 3500 of them) */
static void free_dmif(struct frb_device *adev, int p)
{
	uint32_t v = RREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL));
	int i;

	if (!REG_GET_FIELD(v, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATED))
		return;
	WREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL), REG_SET_FIELD(v, PIPE0_DMIF_BUFFER_CONTROL, DMIF_BUFFERS_ALLOCATED, 0));
	for (i = 0; i < 3500; i++) {
		if (RREG32(PIPE_REG(p, DMIF_BUFFER_CONTROL)) & PIPE0_DMIF_BUFFER_CONTROL__DMIF_BUFFERS_ALLOCATION_COMPLETED_MASK)
			break;
		udelay(10);
	}
}

/*
 * frb_display_off for DCE 12: what DPMS off is on Linux (the modeset
 * trace's fbcon blank, 337.39-337.51), amdgpu_dm's commit of the CRTC
 * inactive, DC's dc_commit_streams with no stream:
 *   - the cursor off as the plane goes (dc_stream_set_cursor_position)
 *   - the accelerated-mode check, prepare_bandwidth (no stream: no marks;
 *     powerplay hears the display count)
 *   - disable_dangling_plane: the pipe lock around an empty surface commit
 *   - dce110_reset_hw_ctx_wrap: link_set_dpms_off (the stream blanked, 60 ms
 *     for the sink to see the idle pattern, the sink to D3 and the PHY off,
 *     the info packets off, the stream encoder off the back end), the CRTC
 *     blanked and disabled through the VBIOS, the DMIF freed
 *   - optimize_bandwidth: DISPCLK and the PHY clock down, powerplay told
 * DP audio goes off first (mute, packets, the AFMT clock) and the endpoint's
 * presence after the info packets.
 * The mode set (frb_dce_v12_0_set_mode) brings it all back, as DPMS on does.
 */
void frb_dce_v12_0_off(struct frb_device *adev, struct frb_display *d)
{
	struct frb_connector *c = d->crtc.conn;
	int crtc = 0, p = 0, fe;

	if (!c || !d->crtc.enabled)
		return;
	fe = c->transmitter;
	pipe_lock(adev, crtc, 1);
	cursor_set_position(adev, p, 0, 0, 0, 0, 0);
	pipe_lock(adev, crtc, 0);
	(void)bios_is_accelerated_mode(adev);
	update_clocks(adev, d, 0, 0, 0, 1);
	pipe_lock(adev, crtc, 1);
	pipe_lock(adev, crtc, 0);

	audio_stream_disable(adev, d, fe);
	dp_blank(adev, fe);
	mdelay(60);
	if (c->dpcd[0])
		frb_dce12_dp_disable_link_phy(adev, c);
	else
		frb_dce12_link_disable_output(adev, c);
	stop_dp_info_packets(adev, fe);
	disconnect_dig_be_from_fe(adev, c, fe);
	audio_az_disable(adev, d);
	frb_dce12_tg_set_blank(adev, crtc, 1);
	frb_dce12_tg_wait_blank(adev, crtc);
	bios_enable_crtc(adev, crtc, 0);
	free_dmif(adev, p);

	update_clocks(adev, d, 0, 0, 1, 1);
	d->crtc.enabled = 0;
	d->crtc.conn = NULL;
	frb_info(adev, "display: link %c off (DPMS off: stream, link, CRTC; DISPCLK %u kHz)", 'A' + c->transmitter,
		 d->dce12.dispclk_khz);
}

/*
 * A plane update on a running stream (dc_update_planes_and_stream for a new
 * surface address or pitch, the modeset trace's fbcon re-commits):
 * prepare_bandwidth without new requirements, then the plane under the pipe
 * lock.
 */
int frb_dce_v12_0_update_plane(struct frb_device *adev, struct frb_display *d, uint64_t fb_offset, uint32_t pitch_pixels)
{
	struct frb_crtc *c = &d->crtc;

	if (!c->enabled || !c->conn)
		return -ENODEV;
	prepare_bandwidth(adev, d, 0, c->mode.clock, d->dce12.phyclk_khz, 0);
	program_plane(adev, 0, 0, &c->mode, adev->gmc.vram_start + fb_offset, pitch_pixels, c->fb_bpp ? c->fb_bpp : 32,
		      d->dce12.bpc, c->gamma);
	c->fb_offset = (uint32_t)fb_offset;
	c->fb_pitch = pitch_pixels;
	return 0;
}
