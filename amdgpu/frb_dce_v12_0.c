/*
 * DCE 12 (Vega 20) display, derived from DC's dce120 path (Linux 7.x
 * drivers/gpu/drm/amd/display/dc): there never was a legacy dce_v12_0.c, so
 * this follows the register sequences DC's objects issue, in DC's order,
 * checked write for write against a Linux DC trace of the MI50
 * (replayed against that trace). Not a DC
 * port: one CRTC drives one DisplayPort link, and every step is the one DC
 * takes for that case.
 *
 *   resource construct      dce120_resource.c (straps, audio endpoints, xGMI),
 *                           link_factory.c (HPD GPIO open + filter per link)
 *   init_hw                 dce110_hwseq.c dce110_init_hw with dce120's hooks
 *   link encoder            dce/dce_link_encoder.c
 *   AUX                     dce/dce_aux.c + gpio/hw_ddc.c (frb_dce12_aux.c)
 *
 * Links come from the atomfirmware object table (bios_parser2.c): path i is
 * DC's link i, its connector's HPD and I2C records and its encoder (the
 * UNIPHY = the DIG back end) pick the HPD, AUX and DIG instances.
 *
 * Copyright 2012-17 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "../shared/atom/atomfirmware-frb.h"	/* before anything that includes atombios.h */
#include "frb_soc15.h"
#include "frb_display.h"
#include "frb_dce12.h"

#define FW_DATA_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_data_tables_v2_1, member) / sizeof(uint16_t)))
#define FW_CMD_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_command_functions_v2_1, member) / sizeof(uint16_t)))

/* dce_12_0_offset.h's ix (Azalia endpoint) registers DC takes from dce_11_x */
#define ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL			0x54
#define ixAZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_CONFIGURATION_DEFAULT	0x56
#define AZ_PORT_CONNECTIVITY(v)		((v) >> 30)
#define AZ_ENDPOINT_STRIDE		(mmAZF0ENDPOINT1_AZALIA_F0_CODEC_ENDPOINT_INDEX - \
					 mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX)
#define FRB_DCE12_MAX_AUDIO		7

/* include/grph_object_id.h / atomfirmware.h object ids */
#define FRB_OBJ_TYPE(id)		(((id) >> 12) & 0xf)
#define FRB_OBJ_ENUM(id)		(((id) >> 8) & 0x7)
#define FRB_OBJ_ID(id)			((id) & 0xff)

/* ------------------------------------------------------------------------- */

void frb_dce12_update(struct frb_device *adev, uint32_t reg, uint32_t mask, uint32_t val)
{
	uint32_t v = RREG32(reg);

	WREG32(reg, (v & ~mask) | (val & mask));
}

/* dce_audio.c AZ_REG_READ / AZ_REG_WRITE: the endpoint's index, then data */
static uint32_t az_read(struct frb_device *adev, int ep, uint32_t index)
{
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + ep * AZ_ENDPOINT_STRIDE, index);
	return RREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + ep * AZ_ENDPOINT_STRIDE);
}

static void az_write(struct frb_device *adev, int ep, uint32_t index, uint32_t v)
{
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_INDEX) + ep * AZ_ENDPOINT_STRIDE, index);
	WREG32(DCE_REG(mmAZF0ENDPOINT0_AZALIA_F0_CODEC_ENDPOINT_DATA) + ep * AZ_ENDPOINT_STRIDE, v);
}

/* ------------------------------------------------------------------------- */
/* bios_parser2.c: the display paths, their HPD and DDC/AUX through gpio_pin_lut */

struct gpio_pin {
	uint32_t reg;		/* data_a_reg_index */
	uint8_t shift;		/* gpio_bitshift */
};

static int gpio_pin_lookup(struct frb_device *adev, uint8_t id, struct gpio_pin *pin)
{
	const uint8_t *bios = adev->bios;
	uint16_t off, size, o;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(gpio_pin_lut), NULL, NULL, &off))
		return -ENOENT;
	size = frb_get_le16(bios, off);
	for (o = off + 4; o + 8 <= off + size; o += 8) {
		if (bios[o + 6] != id)
			continue;
		pin->reg = frb_get_le32(bios, o);
		pin->shift = bios[o + 4];
		return 0;
	}
	return -ENOENT;
}

/*
 * The encoder object's transmitter (bios_parser2 / DC's
 * translate_encoder_to_transmitter): UNIPHY ids 0x1e/0x20/0x21 for the
 * A-B/C-D/E-F pairs, enum 2 = the second of the pair.
 */
static int encoder_to_transmitter(uint16_t enc)
{
	int base;

	switch (FRB_OBJ_ID(enc)) {
	case 0x1e: base = 0; break;	/* ENCODER_OBJECT_ID_INTERNAL_UNIPHY */
	case 0x20: base = 2; break;	/* INTERNAL_UNIPHY1 */
	case 0x21: base = 4; break;	/* INTERNAL_UNIPHY2 */
	default: return -1;
	}
	return base + (FRB_OBJ_ENUM(enc) == 2);
}

static int get_links(struct frb_device *adev, struct frb_display *d)
{
	const uint8_t *bios = adev->bios;
	uint16_t off;
	uint8_t frev, crev, n, i;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(displayobjectinfo), &frev, &crev, &off) ||
	    frev != 1 || crev < 4) {
		frb_err(adev, "display: no display object table v1.4 (%u.%u)", frev, crev);
		return -ENOENT;
	}
	n = bios[off + 6];
	d->num_connectors = 0;
	for (i = 0; i < n && d->num_connectors < FRB_MAX_CONNECTORS; i++) {
		uint32_t p = off + 8 + 16 * i;
		uint16_t conn = frb_get_le16(bios, p), rec = frb_get_le16(bios, p + 2);
		uint16_t enc = frb_get_le16(bios, p + 4);
		struct frb_connector *c;
		struct gpio_pin pin;
		uint32_t r;

		if (FRB_OBJ_TYPE(conn) != GRAPH_OBJECT_TYPE_CONNECTOR || encoder_to_transmitter(enc) < 0)
			continue;
		c = &d->conn[d->num_connectors];
		memset(c, 0, sizeof(*c));
		c->connector_object_id = conn;
		c->connector_type = FRB_OBJ_ID(conn);
		c->encoder_object_id = enc;
		c->encoder_id = FRB_OBJ_ID(enc);
		c->linkb = FRB_OBJ_ENUM(enc) == 2;
		c->transmitter = (int8_t)encoder_to_transmitter(enc);
		c->hpd = -1;
		c->aux = -1;
		c->dig_encoder = -1;
		/* records: atom_common_record_header, 0xff ends the list */
		for (r = off + rec; bios[r] != 0xff && bios[r + 1] >= 2; r += bios[r + 1]) {
			if (bios[r] == ATOM_I2C_RECORD_TYPE) {
				c->i2c_id = bios[r + 2];
				if (!gpio_pin_lookup(adev, c->i2c_id, &pin) &&
				    pin.reg >= DCE_REG(mmDC_GPIO_DDC1_A) && pin.reg <= DCE_REG(mmDC_GPIO_DDC6_A))
					c->aux = (int8_t)((pin.reg - DCE_REG(mmDC_GPIO_DDC1_A)) /
							  (mmDC_GPIO_DDC2_A - mmDC_GPIO_DDC1_A));
			} else if (bios[r] == ATOM_HPD_INT_RECORD_TYPE) {
				/* DC_GPIO_HPD_A bits 0, 8, 16, 24, 26, 28 = HPD1..6 (hw_translate_dce120) */
				static const uint8_t hpd_shift[6] = { 0, 8, 16, 24, 26, 28 };
				int h;

				if (gpio_pin_lookup(adev, bios[r + 2], &pin) || pin.reg != DCE_REG(mmDC_GPIO_HPD_A))
					continue;
				for (h = 0; h < 6; h++)
					if (hpd_shift[h] == pin.shift)
						c->hpd = (int8_t)h;
			}
		}
		d->num_connectors++;
	}
	return d->num_connectors ? 0 : -ENODEV;
}

/* ------------------------------------------------------------------------- */
/* command_table2.c */

/* get_smu_clock_info_v3_1 */
static uint32_t get_smu_clock_info(struct frb_device *adev, uint8_t syspll_id)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, 0, syspll_id);
	frb_ps_u8(&ps, 2, GET_SMU_CLOCK_INFO_V3_1_GET_PLLVCO_FREQ);
	if (frb_ps_exec(adev, FW_CMD_INDEX(getsmuclockinfo), &ps))
		return 0;
	return frb_ps_get_le32(&ps, 0);
}

/*
 * transmitter_control_v1_6 (dig_transmitter_control_parameters_v1_6):
 * phyid, action, digmode or dplaneset, lanenum, symclk_10khz,
 * hpdsel, digfe_sel (0 after DCE 8), connobj_id.
 */
int frb_dce12_transmitter_control(struct frb_device *adev, struct frb_connector *c, uint8_t action,
				  uint8_t mode_laneset, uint8_t lanes, uint32_t pixel_clock_khz)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_u8(&ps, 0, (uint8_t)c->transmitter);
	frb_ps_u8(&ps, 1, action);
	frb_ps_u8(&ps, 2, mode_laneset);
	frb_ps_u8(&ps, 3, lanes);
	frb_ps_le32(&ps, 4, pixel_clock_khz / 10);
	frb_ps_u8(&ps, 8, c->hpd >= 0 ? (uint8_t)(c->hpd + 1) : 0);	/* ATOM_TRANSMITTER_V6_HPDn_SEL */
	frb_ps_u8(&ps, 9, 0);
	frb_ps_u8(&ps, 10, c->connector_type);
	return frb_ps_exec(adev, FW_CMD_INDEX(dig1transmittercontrol), &ps);
}

/* ------------------------------------------------------------------------- */
/* resource construct: dce120_resource_construct + link_construct per path */

/* gpio/hw_gpio.c store_registers + dal_hw_gpio_config_mode(GPIO_MODE_INTERRUPT) */
static void hpd_gpio_open(struct frb_device *adev, int hpd)
{
	static const uint32_t mask[6] = {
		DC_GPIO_HPD_MASK__DC_GPIO_HPD1_MASK_MASK, DC_GPIO_HPD_MASK__DC_GPIO_HPD2_MASK_MASK,
		DC_GPIO_HPD_MASK__DC_GPIO_HPD3_MASK_MASK, DC_GPIO_HPD_MASK__DC_GPIO_HPD4_MASK_MASK,
		DC_GPIO_HPD_MASK__DC_GPIO_HPD5_MASK_MASK, DC_GPIO_HPD_MASK__DC_GPIO_HPD6_MASK_MASK,
	};

	(void)RREG32(DCE_REG(mmDC_GPIO_HPD_MASK));
	(void)RREG32(DCE_REG(mmDC_GPIO_HPD_A));
	(void)RREG32(DCE_REG(mmDC_GPIO_HPD_EN));
	frb_dce12_update(adev, DCE_REG(mmDC_GPIO_HPD_MASK), mask[hpd], 0);
}

/* dce110_program_hpd_filter -> hw_hpd set_config: DP, 80 ms on connect, 0 on disconnect */
static void hpd_filter(struct frb_device *adev, int hpd)
{
	uint32_t v = 0;

	v = REG_SET_FIELD(v, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_CONNECT_INT_DELAY, 80 / 10);
	v = REG_SET_FIELD(v, HPD0_DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_DISCONNECT_INT_DELAY, 0);
	frb_dce12_update(adev, DCE_REG(mmHPD0_DC_HPD_TOGGLE_FILT_CNTL) + hpd * FRB_DCE12_HPD_STRIDE,
			 HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_CONNECT_INT_DELAY_MASK |
			 HPD0_DC_HPD_TOGGLE_FILT_CNTL__DC_HPD_DISCONNECT_INT_DELAY_MASK, v);
}

static int resource_construct(struct frb_device *adev, struct frb_display *d)
{
	uint32_t v;
	int i, r;

	/*
	 * bios_parser2 get_firmware_info_v3_2: the GPU PLL VCO, SMU11_SYSPLL3_0
	 * because smu_info is 3.3 on Vega 20 (SMU9_SYSPLL0 for 3.2)
	 */
	d->dce12.syspll_vco_10khz = get_smu_clock_info(adev, SMU11_SYSPLL3_0_ID);

	/* read_pipe_fuses, then the audio straps */
	d->dce12.pipe_fuses = RREG32(DCE_REG(mmCC_DC_PIPE_DIS));
	(void)RREG32(DCE_REG(mmCC_DC_MISC_STRAPS));
	d->dce12.pinstraps = RREG32(DCE_REG(mmDC_PINSTRAPS));
	d->num_crtc = 6;

	/* resource_construct: audio endpoints until the first unconnected one */
	d->dce12.audio_count = 0;
	for (i = 0; i < FRB_DCE12_MAX_AUDIO; i++) {
		v = az_read(adev, i, ixAZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_CONFIGURATION_DEFAULT);
		if (AZ_PORT_CONNECTIVITY(v) == 1)
			break;
		d->dce12.audio_count++;
	}

	/* dce121_xgmi_enabled */
	v = RREG32_SOC15(MMHUB, 0, mmMC_VM_XGMI_LFB_CNTL);
	d->dce12.xgmi = REG_GET_FIELD(v, MC_VM_XGMI_LFB_CNTL, PF_MAX_REGION) != 0;

	r = get_links(adev, d);
	if (r)
		return r;
	/* link_construct: the HPD GPIO opened for interrupts, then its filter */
	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];

		if (c->hpd < 0)
			continue;
		hpd_gpio_open(adev, c->hpd);
		hpd_filter(adev, c->hpd);
	}
	return 0;
}

/* ------------------------------------------------------------------------- */
/* init_hw: dce110_init_hw with dce120's hooks */

/* dce110_link_encoder_hw_init: transmitter INIT, aux_initialize, hpd_initialize */
static void link_encoder_hw_init(struct frb_device *adev, struct frb_connector *c)
{
	uint32_t aux = c->aux >= 0 ? (uint32_t)c->aux * FRB_DCE12_AUX_STRIDE : 0;
	uint32_t v;

	/* TRANSMITTER_CONTROL_INIT, SIGNAL_TYPE_NONE (-> DVI dig mode), 4 lanes */
	frb_dce12_transmitter_control(adev, c, ATOM_TRANSMITTER_ACTION_INIT, ATOM_TRANSMITTER_DIGMODE_V6_DVI, 4, 0);

	if (c->aux >= 0) {
		v = RREG32(DCE_REG(mmDP_AUX0_AUX_CONTROL) + aux);
		v = REG_SET_FIELD(v, DP_AUX0_AUX_CONTROL, AUX_HPD_SEL, c->hpd >= 0 ? c->hpd : 0);
		v = REG_SET_FIELD(v, DP_AUX0_AUX_CONTROL, AUX_LS_READ_EN, 0);
		WREG32(DCE_REG(mmDP_AUX0_AUX_CONTROL) + aux, v);
		/* 1/4 window (the maximum allowed) */
		v = RREG32(DCE_REG(mmDP_AUX0_AUX_DPHY_RX_CONTROL0) + aux);
		v = REG_SET_FIELD(v, DP_AUX0_AUX_DPHY_RX_CONTROL0, AUX_RX_RECEIVE_WINDOW, 1);
		WREG32(DCE_REG(mmDP_AUX0_AUX_DPHY_RX_CONTROL0) + aux, v);
	}
	/* hpd_initialize: associate the HPD with the DIG back end */
	frb_dce12_update(adev, DCE_REG(mmDIG0_DIG_BE_CNTL) + c->transmitter * FRB_DCE12_DIG_STRIDE,
			 DIG0_DIG_BE_CNTL__DIG_HPD_SELECT_MASK,
			 REG_SET_FIELD(0, DIG0_DIG_BE_CNTL, DIG_HPD_SELECT, c->hpd >= 0 ? c->hpd : 0));
}

static const uint32_t vga_control[6] = {
	mmD1VGA_CONTROL, mmD2VGA_CONTROL, mmD3VGA_CONTROL, mmD4VGA_CONTROL, mmD5VGA_CONTROL, mmD6VGA_CONTROL,
};

/* dce120_timing_generator_disable_vga */
void frb_dce12_tg_disable_vga(struct frb_device *adev, int crtc)
{
	uint32_t reg = adev->reg_offset[DCE_HWIP][0][mmD1VGA_CONTROL_BASE_IDX] + vga_control[crtc];
	uint32_t v = RREG32(reg);

	v = REG_SET_FIELD(v, D1VGA_CONTROL, D1VGA_MODE_ENABLE, 0);
	v = REG_SET_FIELD(v, D1VGA_CONTROL, D1VGA_TIMING_SELECT, 0);
	v = REG_SET_FIELD(v, D1VGA_CONTROL, D1VGA_SYNC_POLARITY_SELECT, 0);
	v = REG_SET_FIELD(v, D1VGA_CONTROL, D1VGA_OVERSCAN_COLOR_EN, 0);
	WREG32(reg, v);
}

/* dce120_timing_generator_set_blank */
void frb_dce12_tg_set_blank(struct frb_device *adev, int crtc, int blank)
{
	uint32_t o = crtc * FRB_DCE12_CRTC_STRIDE;

	WREG32(DCE_REG(mmCRTC0_CRTC_DOUBLE_BUFFER_CONTROL) + o,
	       REG_SET_FIELD(0, CRTC0_CRTC_DOUBLE_BUFFER_CONTROL, CRTC_BLANK_DATA_DOUBLE_BUFFER_EN, 1));
	WREG32(DCE_REG(mmCRTC0_CRTC_BLANK_CONTROL) + o,
	       blank ? REG_SET_FIELD(0, CRTC0_CRTC_BLANK_CONTROL, CRTC_BLANK_DATA_EN, 1) : 0);
}

/* dce120_timing_generator_is_blanked */
int frb_dce12_tg_is_blanked(struct frb_device *adev, int crtc)
{
	uint32_t v = RREG32(DCE_REG(mmCRTC0_CRTC_BLANK_CONTROL) + crtc * FRB_DCE12_CRTC_STRIDE);

	return REG_GET_FIELD(v, CRTC0_CRTC_BLANK_CONTROL, CRTC_BLANK_DATA_EN) == 1 &&
	       REG_GET_FIELD(v, CRTC0_CRTC_BLANK_CONTROL, CRTC_CURRENT_BLANK_STATE) == 1;
}

/* hwss_wait_for_blank_complete: 100 ms, in 1 ms steps */
void frb_dce12_tg_wait_blank(struct frb_device *adev, int crtc)
{
	int counter;

	for (counter = 0; counter < 100; counter++) {
		if (frb_dce12_tg_is_blanked(adev, crtc))
			return;
		mdelay(1);
	}
	frb_err(adev, "display: CRTC%d did not blank", crtc);
}

/* dce_aud_hw_init (instance 0 only) */
static void audio_hw_init(struct frb_device *adev)
{
	uint32_t v, w;

	v = az_read(adev, 0, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL);
	v |= AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE_MASK;
	az_write(adev, 0, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL, v);
	w = REG_SET_FIELD(0, AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES, AUDIO_RATE_CAPABILITIES, 0x70);
	frb_dce12_update(adev, DCE_REG(mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES),
			 AZALIA_F0_CODEC_FUNCTION_PARAMETER_SUPPORTED_SIZE_RATES__AUDIO_RATE_CAPABILITIES_MASK, w);
	w = REG_SET_FIELD(0, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, CLKSTOP, 1);
	w = REG_SET_FIELD(w, AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES, EPSS, 1);
	frb_dce12_update(adev, DCE_REG(mmAZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES),
			 AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__CLKSTOP_MASK |
			 AZALIA_F0_CODEC_FUNCTION_PARAMETER_POWER_STATES__EPSS_MASK, w);
	v &= ~AZF0ENDPOINT0_AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__CLOCK_GATING_DISABLE_MASK;
	az_write(adev, 0, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL, v);
}

/* dce_abm_init with MAX_BACKLIGHT_LEVEL (no panel) */
static void abm_init(struct frb_device *adev)
{
	uint32_t v;

	WREG32(DCE_REG(mmDC_ABM1_HG_SAMPLE_RATE), 0x103);
	WREG32(DCE_REG(mmDC_ABM1_HG_SAMPLE_RATE), 0x101);
	WREG32(DCE_REG(mmDC_ABM1_LS_SAMPLE_RATE), 0x103);
	WREG32(DCE_REG(mmDC_ABM1_LS_SAMPLE_RATE), 0x101);
	WREG32(DCE_REG(mmBL1_PWM_BL_UPDATE_SAMPLE_RATE), 0x101);
	v = REG_SET_FIELD(0, DC_ABM1_HG_MISC_CTRL, ABM1_HG_VMAX_SEL, 1);
	WREG32(DCE_REG(mmDC_ABM1_HG_MISC_CTRL), v);
	v = REG_SET_FIELD(0, DC_ABM1_IPCSC_COEFF_SEL, ABM1_IPCSC_COEFF_SEL_R, 2);
	v = REG_SET_FIELD(v, DC_ABM1_IPCSC_COEFF_SEL, ABM1_IPCSC_COEFF_SEL_G, 4);
	v = REG_SET_FIELD(v, DC_ABM1_IPCSC_COEFF_SEL, ABM1_IPCSC_COEFF_SEL_B, 2);
	WREG32(DCE_REG(mmDC_ABM1_IPCSC_COEFF_SEL), v);
	frb_dce12_update(adev, DCE_REG(mmBL1_PWM_CURRENT_ABM_LEVEL),
			 BL1_PWM_CURRENT_ABM_LEVEL__BL1_PWM_CURRENT_ABM_LEVEL_MASK, 0xffff);
	frb_dce12_update(adev, DCE_REG(mmBL1_PWM_TARGET_ABM_LEVEL),
			 BL1_PWM_TARGET_ABM_LEVEL__BL1_PWM_TARGET_ABM_LEVEL_MASK, 0xffff);
	frb_dce12_update(adev, DCE_REG(mmBL1_PWM_USER_LEVEL), BL1_PWM_USER_LEVEL__BL1_PWM_USER_LEVEL_MASK, 0xffff);
	v = REG_SET_FIELD(0, DC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES, ABM1_LS_MIN_PIXEL_VALUE_THRES, 0);
	v = REG_SET_FIELD(v, DC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES, ABM1_LS_MAX_PIXEL_VALUE_THRES, 1000);
	frb_dce12_update(adev, DCE_REG(mmDC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES),
			 DC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES__ABM1_LS_MIN_PIXEL_VALUE_THRES_MASK |
			 DC_ABM1_LS_MIN_MAX_PIXEL_VALUE_THRES__ABM1_LS_MAX_PIXEL_VALUE_THRES_MASK, v);
	v = REG_SET_FIELD(0, DC_ABM1_HGLS_REG_READ_PROGRESS, ABM1_HG_REG_READ_MISSED_FRAME_CLEAR, 1);
	v = REG_SET_FIELD(v, DC_ABM1_HGLS_REG_READ_PROGRESS, ABM1_LS_REG_READ_MISSED_FRAME_CLEAR, 1);
	v = REG_SET_FIELD(v, DC_ABM1_HGLS_REG_READ_PROGRESS, ABM1_BL_REG_READ_MISSED_FRAME_CLEAR, 1);
	WREG32(DCE_REG(mmDC_ABM1_HGLS_REG_READ_PROGRESS), v);
}

static void init_hw(struct frb_device *adev, struct frb_display *d)
{
	uint32_t v;
	int i;

	/*
	 * dce_clock_gating_power_up(hws, false): DC_MEM_GLOBAL_PWR_REQ_DIS = 1,
	 * which keeps the display SRAMs (line buffers, DMIF, cursor) powered.
	 * Without it the RX 550's cursor and scanout memories went to sleep
	 * under DC's power gating (dce_disable_sram_shut_down, the Polaris lesson).
	 * enable_display_power_gating is a no-op on dce120 and transform_reset
	 * touches no register.
	 */
	frb_dce12_update(adev, DCE_REG(mmDC_MEM_GLOBAL_PWR_REQ_CNTL),
			 DC_MEM_GLOBAL_PWR_REQ_CNTL__DC_MEM_GLOBAL_PWR_REQ_DIS_MASK,
			 DC_MEM_GLOBAL_PWR_REQ_CNTL__DC_MEM_GLOBAL_PWR_REQ_DIS_MASK);

	for (i = 0; i < d->num_connectors; i++)
		link_encoder_hw_init(adev, &d->conn[i]);

	for (i = 0; i < (int)d->num_crtc; i++) {
		frb_dce12_tg_disable_vga(adev, i);
		/* blank with register writes, not the command table */
		frb_dce12_tg_set_blank(adev, i, 1);
		frb_dce12_tg_wait_blank(adev, i);
	}

	if (d->dce12.audio_count)
		audio_hw_init(adev);
	abm_init(adev);
	/* dce_is_dmcu_initialized */
	v = RREG32(DCE_REG(mmDMCU_STATUS));
	d->dce12.dmcu_running = !REG_GET_FIELD(v, DMCU_STATUS, UC_IN_RESET);
}

/* ------------------------------------------------------------------------- */

int frb_dce_v12_0_hw_init(struct frb_device *adev, struct frb_display *d)
{
	int r, i;

	r = resource_construct(adev, d);
	if (r)
		return r;
	/* the CRTC's gamma store starts as the linear ramp the DCE 11 path keeps (i << 8): no LUT */
	for (i = 0; i < 256; i++)
		d->crtc.gamma[0][i] = d->crtc.gamma[1][i] = d->crtc.gamma[2][i] = (uint16_t)(i << 8);
	init_hw(adev, d);
	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];

		frb_info(adev, "display: link %d: DP connector %04x, UNIPHY %c (DIG%d), AUX%d, HPD%d", i,
			 c->connector_object_id, 'A' + c->transmitter, c->transmitter, c->aux, c->hpd);
	}
	frb_info(adev, "display: DCE 12, %u pipes (fuses %08x), %u audio endpoint(s), DMCU %s, SYSPLL0 VCO %u MHz%s",
		 d->num_crtc, d->dce12.pipe_fuses, d->dce12.audio_count, d->dce12.dmcu_running ? "running" : "in reset",
		 d->dce12.syspll_vco_10khz / 100, d->dce12.xgmi ? ", xGMI" : "");
	d->ready = 1;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* detection: amdgpu_dm's boot probe, then dc_link_detect, link by link */

/* dce110_get_hpd_state -> hw_hpd get_value in interrupt mode: DC_HPD_SENSE_DELAYED */
static int hpd_sense(struct frb_device *adev, const struct frb_connector *c)
{
	uint32_t v;

	if (c->hpd < 0)
		return 0;
	v = RREG32(DCE_REG(mmHPD0_DC_HPD_INT_STATUS) + c->hpd * FRB_DCE12_HPD_STRIDE);
	return REG_GET_FIELD(v, HPD0_DC_HPD_INT_STATUS, DC_HPD_SENSE_DELAYED);
}

int frb_dce_v12_0_hpd_sense(struct frb_device *adev, const struct frb_connector *c)
{
	return hpd_sense(adev, c);
}

void frb_dce_v12_0_readout(struct frb_device *adev, int ep, uint32_t *crtc_control, uint32_t *frames,
			   uint32_t *hot_plug, uint32_t *speakers)
{
	if (ep < 0 || ep >= FRB_DCE12_MAX_AUDIO)
		ep = 0;
	*crtc_control = RREG32(DCE_REG(mmCRTC0_CRTC_CONTROL));
	*frames = RREG32(DCE_REG(mmCRTC0_CRTC_STATUS_FRAME_COUNT));
	*hot_plug = az_read(adev, ep, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL);
	*speakers = az_read(adev, ep, 0x25);	/* CHANNEL_SPEAKER */
}

#define DP_SET_POWER			0x600
#define DP_SET_POWER_D3			0x2
#define DP_SOURCE_OUI			0x300
#define DP_DPCD_REV			0x000
#define DP_TRAINING_AUX_RD_INTERVAL	0x00e
#define DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT	(1 << 7)
#define DP_DP13_DPCD_REV		0x2200
#define DP_DPRX_FEATURE_ENUMERATION_LIST	0x2210
#define DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1	0x2214
#define DP_MAX_LANE_COUNT		0x002
#define DP_MSTM_CAP			0x021
#define DP_DSC_SUPPORT			0x060
#define DP_FEC_CAPABILITY		0x090
#define DP_PANEL_REPLAY_CAPABILITY_SUPPORT	0x0b0
#define DP_PANEL_REPLAY_CAPABILITY	0x0b1
#define DP_PR_SU_X_GRANULARITY_LOW	0x0b2
#define DP_SINK_COUNT			0x200
#define DP_SOURCE_SINK_CAP		0x317	/* AMD's, dpcd_defs.h */
#define DP_SINK_OUI			0x400
#define DP_SINK_HW_REVISION_START	0x409
#define DP_BRANCH_OUI			0x500
#define DP_EDP_GENERAL_CAP_2		0x703
#define DP_CABLE_ATTRIBUTES_UPDATED_BY_DPRX	0x2217
#define DPCD_MAX_UNCOMPRESSED_PIXEL_RATE_CAP	0x221c
#define DP_TUNNELING_CAPABILITIES	0xe000d
#define FRB_DCE_VERSION_12_1		0x0b	/* dc_types.h enum dce_version, as the trace's 0x303 write */

/* link_dp_capability.c wake_up_aux_channel */
static int wake_up_aux_channel(struct frb_device *adev, struct frb_connector *c)
{
	uint8_t state = 0;
	int r = -EIO, tries = 0;

	while (r < 0 && tries < 10) {
		r = frb_dce12_dpcd_read(adev, c, DP_SET_POWER, &state, 1);
		if (r < 0 || state == DP_SET_POWER_D3) {
			mdelay(1);
			tries++;
		}
	}
	if (r < 0) {
		uint8_t d0 = 1, d3 = DP_SET_POWER_D3;

		frb_dce12_dpcd_write(adev, c, DP_SET_POWER, &d0, 1);
		frb_dce12_dpcd_write(adev, c, DP_SET_POWER, &d3, 1);
		return r;
	}
	return 0;
}

/* link_dpcd.c dpcd_set_source_specific_data: AMD's source OUI and device id */
void frb_dce12_set_source_specific_data(struct frb_device *adev, struct frb_connector *c)
{
	uint8_t sig[3] = { 0 }, id[9];

	frb_dce12_dpcd_read(adev, c, DP_SOURCE_OUI, sig, sizeof(sig));
	if (!(sig[0] == 0 && sig[1] == 0 && sig[2] == 0x1a)) {
		sig[0] = 0;
		sig[1] = 0;
		sig[2] = 0x1a;
		frb_dce12_dpcd_write(adev, c, DP_SOURCE_OUI, sig, sizeof(sig));
	}
	memset(id, 0, sizeof(id));
	id[0] = (uint8_t)adev->row->device;
	id[1] = (uint8_t)(adev->row->device >> 8);
	id[6] = FRB_DCE_VERSION_12_1;
	frb_dce12_dpcd_write(adev, c, DP_SOURCE_OUI + 3, id, sizeof(id));
}

/* link_dp_capability.c retrieve_link_cap (dce120: no LTTPR) */
static int retrieve_link_cap(struct frb_device *adev, struct frb_connector *c)
{
	uint8_t dpcd[16], id[9], b = 0;
	int r = -EIO, i;

	if (wake_up_aux_channel(adev, c))
		return -EIO;
	frb_dce12_set_source_specific_data(adev, c);
	for (i = 0; i < 20; i++) {
		mdelay(30);	/* post_oui_delay */
		r = frb_dce12_dpcd_read(adev, c, DP_DPCD_REV, dpcd, sizeof(dpcd));
		if (r >= 0)
			break;
	}
	if (r < 0) {
		frb_err(adev, "display: link %s: the receiver caps did not read (%d)", "?", r);
		return r;
	}
	c->dp_ext_caps = (dpcd[DP_TRAINING_AUX_RD_INTERVAL] & DP_EXTENDED_RECEIVER_CAP_FIELD_PRESENT) != 0;
	if (c->dp_ext_caps) {
		uint8_t ext[16];

		for (i = 0; i < 20; i++)
			if (frb_dce12_dpcd_read(adev, c, DP_DP13_DPCD_REV, ext, sizeof(ext)) >= 0) {
				memcpy(dpcd, ext, sizeof(dpcd));
				break;
			}
		for (i = 0; i < 20; i++)
			if (frb_dce12_dpcd_read(adev, c, DP_DPRX_FEATURE_ENUMERATION_LIST, &b, 1) >= 0)
				break;
		c->dprx_feature = b;
		b = 0;
		for (i = 0; i < 20; i++)
			if (frb_dce12_dpcd_read(adev, c, DP_DPRX_FEATURE_ENUMERATION_LIST_CONT_1, &b, 1) >= 0)
				break;
	}
	/* a sink cannot have no lanes */
	if (!(dpcd[DP_MAX_LANE_COUNT] & 0x1f))
		return -EIO;

	/* read_dp_device_vendor_id: the branch OUI and device name */
	memset(id, 0, sizeof(id));
	frb_dce12_dpcd_read(adev, c, DP_BRANCH_OUI, id, sizeof(id));
	c->branch_oui = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];

	/* read_is_mst_supported */
	c->mst = 0;
	b = 0;
	if (frb_dce12_dpcd_read(adev, c, DP_DPCD_REV, &b, 1) >= 0 && b >= 0x12) {
		b = 0;
		if (frb_dce12_dpcd_read(adev, c, DP_MSTM_CAP, &b, 1) >= 0)
			c->mst = b & 1;
	}
	/* get_active_converter_info and dp_wa_power_up_0010FA read nothing for a sink without a downstream port */

	c->sink_count = 0;
	frb_dce12_dpcd_read(adev, c, DP_SINK_COUNT, &c->sink_count, 1);
	memset(id, 0, sizeof(id));
	frb_dce12_dpcd_read(adev, c, DP_SINK_OUI, id, sizeof(id));
	c->sink_oui = (uint32_t)id[0] << 16 | (uint32_t)id[1] << 8 | id[2];
	frb_dce12_dpcd_read(adev, c, DP_SINK_HW_REVISION_START, id, 3);	/* hardware, firmware revision */

	/* DSC and FEC caps from DPCD 1.4 on */
	c->fec_cap = 0;
	c->dsc_support = 0;
	if (dpcd[DP_DPCD_REV] >= 0x14) {
		uint8_t dsc[16];

		frb_dce12_dpcd_read(adev, c, DP_FEC_CAPABILITY, &c->fec_cap, 1);
		if (frb_dce12_dpcd_read(adev, c, DP_DSC_SUPPORT, dsc, sizeof(dsc)) >= 0)
			c->dsc_support = dsc[0];
	}
	/* dpcd_read_sink_ext_caps: the AMD sink caps, then eDP's general caps 2 */
	if (frb_dce12_dpcd_read(adev, c, DP_SOURCE_SINK_CAP, &b, 1) >= 0)
		frb_dce12_dpcd_read(adev, c, DP_EDP_GENERAL_CAP_2, &b, 1);
	/* (128b/132b: not on DCE 12) */
	frb_dce12_dpcd_read(adev, c, DPCD_MAX_UNCOMPRESSED_PIXEL_RATE_CAP, id, 2);
	frb_dce12_dpcd_read(adev, c, DP_PANEL_REPLAY_CAPABILITY_SUPPORT, &b, 1);
	frb_dce12_dpcd_read(adev, c, DP_PANEL_REPLAY_CAPABILITY, &b, 1);
	/* retrieve_vesa_replay_su_info: 0xb2..0xb6 one byte each */
	for (i = 0; i < 5; i++)
		frb_dce12_dpcd_read(adev, c, DP_PR_SU_X_GRANULARITY_LOW + i, &b, 1);
	/* dpcd_get_tunneling_device_data: a USB4 tunnel's caps (none on a plain DP sink) */
	frb_dce12_dpcd_read(adev, c, DP_TUNNELING_CAPABILITIES, id, 3);
	/* retrieve_cable_id (dpcd_write_cable_id_to_dprx is for 128b/132b) */
	frb_dce12_dpcd_read(adev, c, DP_CABLE_ATTRIBUTES_UPDATED_BY_DPRX, &b, 1);

	memcpy(c->dpcd, dpcd, 15);
	return 0;
}

/* the zlib CRC-32 of the EDID, for the log: equal to the capture's (analysis/aux-init/edid-aux1-0.bin) */
static uint32_t crc32(const uint8_t *p, uint32_t n)
{
	uint32_t crc = 0xffffffff;
	int k;

	while (n--) {
		crc ^= *p++;
		for (k = 0; k < 8; k++)
			crc = (crc >> 1) ^ (0xedb88320 & (0 - (crc & 1)));
	}
	return ~crc;
}

/* ------------------------------------------------------------------------- */
/*
 * The link encoder (dce/dce_link_encoder.c, dce110 functions dce120 uses) and
 * DC's 8b/10b link training (link_dp_training*.c), for an SST DP link
 * without LTTPRs. The DP back end's registers are the transmitter's
 * instance (DIG4/DP4 for UNIPHY E).
 */

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmDP0_DP_CONFIG
 * mmDP0_DP_DPHY_SCRAM_CNTL mmDP0_DP_LINK_CNTL mmDP0_DP_DPHY_CNTL mmDP0_DP_DPHY_PRBS_CNTL
 * mmDP0_DP_DPHY_TRAINING_PATTERN_SEL mmDP0_DP_LINK_FRAMING_CNTL mmDIG0_DIG_BE_EN_CNTL
 */
#define FRB_DCE12_DP_STRIDE	(mmDP1_DP_LINK_CNTL - mmDP0_DP_LINK_CNTL)
#define DP_REG(c, r)		(DCE_REG(mmDP0_##r) + (uint32_t)(c)->transmitter * FRB_DCE12_DP_STRIDE)
#define DIG_REG(c, r)		(DCE_REG(mmDIG0_##r) + (uint32_t)(c)->transmitter * FRB_DCE12_DIG_STRIDE)
#define LINK_RATE_REF_FREQ_IN_KHZ	27000

#define DP_LINK_BW_SET			0x100
#define DP_LANE_COUNT_SET		0x101
#define DP_TRAINING_PATTERN_SET		0x102
#define DP_TRAINING_LANE0_SET		0x103
#define DP_DOWNSPREAD_CTRL		0x107
#define DP_MAIN_LINK_CHANNEL_CODING_SET	0x108
#define DP_LANE0_1_STATUS		0x202
#define DP_SINK_COUNT_ESI		0x2002
#define DP_SET_POWER_D0			0x1

int frb_dce12_dpcd_write1(struct frb_device *adev, struct frb_connector *c, uint32_t addr, uint8_t v)
{
	return frb_dce12_dpcd_write(adev, c, addr, &v, 1);
}

/* dce110_is_dig_enabled */
int frb_dce12_dig_enabled(struct frb_device *adev, const struct frb_connector *c)
{
	return REG_GET_FIELD(RREG32(DIG_REG(c, DIG_BE_EN_CNTL)), DIG0_DIG_BE_EN_CNTL, DIG_ENABLE);
}

/* set_link_training_complete, enable_phy_bypass_mode(false), disable_prbs_mode */
static void link_training_complete(struct frb_device *adev, const struct frb_connector *c, int complete)
{
	frb_dce12_update(adev, DP_REG(c, DP_LINK_CNTL), DP0_DP_LINK_CNTL__DP_LINK_TRAINING_COMPLETE_MASK,
			 complete ? DP0_DP_LINK_CNTL__DP_LINK_TRAINING_COMPLETE_MASK : 0);
}

static void phy_bypass_off_prbs_off(struct frb_device *adev, const struct frb_connector *c)
{
	frb_dce12_update(adev, DP_REG(c, DP_DPHY_CNTL), DP0_DP_DPHY_CNTL__DPHY_BYPASS_MASK, 0);
	frb_dce12_update(adev, DP_REG(c, DP_DPHY_PRBS_CNTL), DP0_DP_DPHY_PRBS_CNTL__DPHY_PRBS_EN_MASK, 0);
}

/* dce110_link_encoder_set_dp_phy_pattern_training_pattern: TPS1..4 = index 0..3 */
static void set_hw_training_pattern(struct frb_device *adev, const struct frb_connector *c, uint32_t index)
{
	WREG32(DP_REG(c, DP_DPHY_TRAINING_PATTERN_SEL), index);
	link_training_complete(adev, c, 0);
	phy_bypass_off_prbs_off(adev, c);
}

/* set_dp_phy_pattern_passthrough_mode (DP_TEST_PATTERN_VIDEO_MODE; setup_panel_mode does nothing here) */
static void set_hw_video_mode(struct frb_device *adev, const struct frb_connector *c)
{
	uint32_t v = 0;

	v = REG_SET_FIELD(v, DP0_DP_LINK_FRAMING_CNTL, DP_IDLE_BS_INTERVAL, 0x2000);
	v = REG_SET_FIELD(v, DP0_DP_LINK_FRAMING_CNTL, DP_VBID_DISABLE, 0);
	v = REG_SET_FIELD(v, DP0_DP_LINK_FRAMING_CNTL, DP_VID_ENHANCED_FRAME_MODE, 1);
	frb_dce12_update(adev, DP_REG(c, DP_LINK_FRAMING_CNTL),
			 DP0_DP_LINK_FRAMING_CNTL__DP_IDLE_BS_INTERVAL_MASK | DP0_DP_LINK_FRAMING_CNTL__DP_VBID_DISABLE_MASK |
			 DP0_DP_LINK_FRAMING_CNTL__DP_VID_ENHANCED_FRAME_MODE_MASK, v);
	frb_dce12_update(adev, DP_REG(c, DP_DPHY_SCRAM_CNTL), DP0_DP_DPHY_SCRAM_CNTL__DPHY_SCRAMBLER_BS_COUNT_MASK,
			 REG_SET_FIELD(0, DP0_DP_DPHY_SCRAM_CNTL, DPHY_SCRAMBLER_BS_COUNT, 0x1ff));
	link_training_complete(adev, c, 1);
	phy_bypass_off_prbs_off(adev, c);
}

/* dce110_link_encoder_enable_dp_output: configure_encoder, then the VBIOS's transmitter ENABLE */
static int link_enc_enable_dp_output(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes)
{
	WREG32(DP_REG(c, DP_CONFIG), REG_SET_FIELD(0, DP0_DP_CONFIG, DP_UDI_LANES, lanes - 1));
	frb_dce12_update(adev, DP_REG(c, DP_DPHY_SCRAM_CNTL), DP0_DP_DPHY_SCRAM_CNTL__DPHY_SCRAMBLER_ADVANCE_MASK,
			 DP0_DP_DPHY_SCRAM_CNTL__DPHY_SCRAMBLER_ADVANCE_MASK);
	return frb_dce12_transmitter_control(adev, c, ATOM_TRANSMITTER_ACTION_ENABLE, ATOM_TRANSMITTER_DIGMODE_V6_DP,
					     (uint8_t)lanes, rate * LINK_RATE_REF_FREQ_IN_KHZ);
}

/* dce110_link_encoder_disable_output for DP: transmitter DISABLE, then link_encoder_disable */
void frb_dce12_link_disable_output(struct frb_device *adev, struct frb_connector *c)
{
	if (!frb_dce12_dig_enabled(adev, c))
		return;		/* OF_SKIP_POWER_DOWN_INACTIVE_ENCODER */
	if (frb_dce12_transmitter_control(adev, c, ATOM_TRANSMITTER_ACTION_DISABLE, ATOM_TRANSMITTER_DIGMODE_V6_DP, 0, 0))
		return;
	WREG32(DP_REG(c, DP_DPHY_TRAINING_PATTERN_SEL), 0);
	link_training_complete(adev, c, 0);
}

/*
 * dce110_link_encoder_dp_set_lane_settings: the VBIOS's SETUP_VSEMPH once per
 * lane with that lane's dpcd_training_lane_set (swing, pre-emphasis, and at
 * HBR2 post cursor 2); disallow_per_lane_settings makes them all equal.
 */
static void set_hw_lane_settings(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes,
				 uint8_t vs, uint8_t pe)
{
	uint32_t lane;
	uint8_t set = (uint8_t)(vs | pe << 3);

	(void)rate;	/* post cursor 2 (bits 6-7 at HBR2) stays 0: DC never raises it */
	for (lane = 0; lane < lanes; lane++)
		frb_dce12_transmitter_control(adev, c, ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH, set, (uint8_t)lanes,
					      rate * LINK_RATE_REF_FREQ_IN_KHZ);
}

/* link_dp_phy.c dp_enable_link_phy (dce110_enable_dp_link_output) and dp_disable_link_phy */
int frb_dce12_dp_enable_link_phy(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes)
{
	int r = link_enc_enable_dp_output(adev, c, rate, lanes);

	frb_dce12_dpcd_write1(adev, c, DP_SET_POWER, DP_SET_POWER_D0);
	return r;
}

void frb_dce12_dp_disable_link_phy(struct frb_device *adev, struct frb_connector *c)
{
	frb_dce12_dpcd_write1(adev, c, DP_SET_POWER, DP_SET_POWER_D3);
	frb_dce12_link_disable_output(adev, c);
}

/* the DPCD training lane byte: swing, pre-emphasis, the "max reached" bits */
static uint8_t dpcd_lane(uint8_t vs, uint8_t pe)
{
	return (uint8_t)(vs | (vs == 3) << 2 | pe << 3 | (pe == 3) << 5);
}

/* dp_decide_lane_settings with disallow_per_lane_settings: the highest request, VS + PE <= 3 */
static void decide_lane_settings(const uint8_t *adj, uint32_t lanes, uint8_t *vs, uint8_t *pe)
{
	uint32_t lane;
	uint8_t v = 0, p = 0;

	for (lane = 0; lane < lanes; lane++) {
		uint8_t a = (uint8_t)(adj[lane / 2] >> (4 * (lane & 1)));

		if ((a & 3) > v)
			v = a & 3;
		if (((a >> 2) & 3) > p)
			p = (a >> 2) & 3;
	}
	if (p > 3 - v)
		p = (uint8_t)(3 - v);
	*vs = v;
	*pe = p;
}

/* lane status bits of DPCD 0x202/0x203: CR_DONE 1, CHANNEL_EQ_DONE 2, SYMBOL_LOCKED 4 per nibble */
static int lanes_have(const uint8_t *st, uint32_t lanes, uint8_t bits)
{
	uint32_t lane;

	for (lane = 0; lane < lanes; lane++)
		if (((st[lane / 2] >> (4 * (lane & 1))) & bits) != bits)
			return 0;
	return 1;
}


/* dp_wait_for_training_aux_rd_interval */
static void wait_aux_rd_interval(struct frb_device *adev, uint32_t us)
{
	if (us > 16000)
		mdelay(us / 1000);
	else
		udelay(us);
}

/*
 * dp_perform_link_training (8b/10b, no LTTPR): the receiver out of training,
 * channel coding, link settings, clock recovery with TPS1, channel
 * equalisation with TPS4 (the sink and DCE 12 both have it), out of training,
 * the video idle pattern and the 5 ms link-loss check.
 */
int frb_dce12_dp_perform_link_training(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes)
{
	uint8_t interval = 0, st[6], burst[5], vs = 0, pe = 0, ls[4];
	uint32_t cr_time, eq_time, retries, retry_count, lane;
	int status = LT_SUCCESS, tps, i;

	/* decide_8b_10b_training_settings: the EQ, then the CR wait times (DPCD 1.2 and up) */
	static const uint32_t eq_us[8] = { 400, 4000, 8000, 12000, 16000, 32000, 64000, 400 };

	eq_time = 400;
	cr_time = 100;
	if (c->dpcd[DP_DPCD_REV] >= 0x12) {
		frb_dce12_dpcd_read(adev, c, DP_TRAINING_AUX_RD_INTERVAL, &interval, 1);
		eq_time = eq_us[interval & 7];
		interval = 0;
		frb_dce12_dpcd_read(adev, c, DP_TRAINING_AUX_RD_INTERVAL, &interval, 1);
		if (interval)
			cr_time = (interval & 0x7f) ? 4000u * (interval & 0x7f) : 400;
	}
	/* decide_eq_training_pattern: TPS4 (DCE 12 has it), else TPS3, else TPS2 */
	tps = (c->dpcd[3] & 0x80) ? 4 : (c->dpcd[2] & 0x40) ? 3 : 2;

	frb_dce12_dpcd_write1(adev, c, DP_TRAINING_PATTERN_SET, 0);		/* dpcd_exit_training_mode */
	frb_dce12_dpcd_write1(adev, c, DP_MAIN_LINK_CHANNEL_CODING_SET, 1);	/* DP_SET_ANSI_8B10B */
	/* dpcd_set_link_settings: spread on (dp_ss_off is false on DCE 12), enhanced framing */
	frb_dce12_dpcd_write1(adev, c, DP_DOWNSPREAD_CTRL, 0x10);
	frb_dce12_dpcd_write1(adev, c, DP_LANE_COUNT_SET, (uint8_t)(lanes | 0x80));
	frb_dce12_dpcd_write1(adev, c, DP_LINK_BW_SET, (uint8_t)rate);

	/* perform_8b_10b_clock_recovery_sequence */
	set_hw_training_pattern(adev, c, 0);
	status = LT_CR_FAIL;
	for (retries = 0, retry_count = 0; retries < 5 && retry_count < 100; retry_count++) {
		set_hw_lane_settings(adev, c, rate, lanes, vs, pe);
		for (lane = 0; lane < 4; lane++)
			ls[lane] = dpcd_lane(vs, pe);
		if (!retry_count) {
			burst[0] = 0x21;	/* TPS1, scrambling disabled */
			memcpy(burst + 1, ls, 4);
			frb_dce12_dpcd_write(adev, c, DP_TRAINING_PATTERN_SET, burst, 1 + lanes);
		} else {
			frb_dce12_dpcd_write(adev, c, DP_TRAINING_LANE0_SET, ls, lanes);
		}
		wait_aux_rd_interval(adev, cr_time);
		memset(st, 0, sizeof(st));
		frb_dce12_dpcd_read(adev, c, DP_LANE0_1_STATUS, st, sizeof(st));
		if (lanes_have(st, lanes, 1)) {
			status = LT_SUCCESS;
			break;
		}
		/* dp_get_cr_failure: which lanes are without clock recovery (for the fallback) */
		status = !(st[0] & 0x01) ? LT_CR_FAIL : (lanes > 1 && !(st[0] & 0x10)) ? LT_CR_FAIL_LANE1 :
			 lanes > 2 ? LT_CR_FAIL_LANE23 : LT_CR_FAIL;
		if (vs == 3)		/* dp_is_max_vs_reached */
			break;
		if (vs == (st[4] & 3))
			retries++;
		else
			retries = 0;
		decide_lane_settings(st + 4, lanes, &vs, &pe);
	}

	/* perform_8b_10b_channel_equalization_sequence */
	if (status == LT_SUCCESS) {
		set_hw_training_pattern(adev, c, (uint32_t)tps - 1);
		status = LT_EQ_FAIL_EQ;
		for (i = 0; i <= 5; i++) {
			set_hw_lane_settings(adev, c, rate, lanes, vs, pe);
			for (lane = 0; lane < 4; lane++)
				ls[lane] = dpcd_lane(vs, pe);
			if (!i) {
				burst[0] = tps == 4 ? 0x07 : (uint8_t)(0x20 | tps);	/* TPS4 is scrambled, 2 and 3 not */
				memcpy(burst + 1, ls, 4);
				frb_dce12_dpcd_write(adev, c, DP_TRAINING_PATTERN_SET, burst, 1 + lanes);
			} else {
				frb_dce12_dpcd_write(adev, c, DP_TRAINING_LANE0_SET, ls, lanes);
			}
			wait_aux_rd_interval(adev, eq_time);
			memset(st, 0, sizeof(st));
			frb_dce12_dpcd_read(adev, c, DP_LANE0_1_STATUS, st, sizeof(st));
			if (!lanes_have(st, lanes, 1)) {
				status = LT_EQ_FAIL_CR;
				break;
			}
			if (lanes_have(st, lanes, 7) && (st[2] & 1)) {
				status = LT_SUCCESS;
				break;
			}
			decide_lane_settings(st + 4, lanes, &vs, &pe);
		}
	}
	frb_dce12_dpcd_write1(adev, c, DP_TRAINING_PATTERN_SET, 0);

	/*
	 * dp_transition_to_video_idle (after a failure only at RBR, where the
	 * video pattern is not skipped): the idle pattern, then DPCD 0x200..
	 * after 5 ms (no POST_LT_ADJ_REQ with TPS4)
	 */
	if (status == LT_SUCCESS || rate == 0x06)
		set_hw_video_mode(adev, c);
	if (status == LT_SUCCESS) {
		mdelay(5);
		memset(st, 0, sizeof(st));
		frb_dce12_dpcd_read(adev, c, DP_SINK_COUNT, st, sizeof(st));
		if (!lanes_have(st + 2, lanes, 7) || !(st[4] & 1))
			status = LT_LINK_LOSS;
	}
	c->lt_vs = vs;
	c->lt_pe = pe;
	return status;
}

/* dp_read_hpd_rx_irq_data (DPCD 1.4: the ESI block) and dp_parse_link_loss_status */
static int link_lost(struct frb_device *adev, struct frb_connector *c, uint32_t lanes)
{
	uint8_t esi[14];

	memset(esi, 0, sizeof(esi));
	if (frb_dce12_dpcd_read(adev, c, DP_SINK_COUNT_ESI, esi, sizeof(esi)) < 0)
		return 0;
	/* 0x200c/0x200d lane status, 0x200e interlane align */
	return !lanes_have(esi + 10, lanes, 7) || !(esi[12] & 1);
}

/*
 * dp_verify_link_cap: train at the limit and, while that fails, at what
 * decide_fallback_link_setting gives next; the link off after each. The
 * first setting that trains is the verified cap (a link lost within 1 ms of
 * training counts as trained, and as a failure for the retries). 0 with
 * *rate / *lanes set, or -EIO: nothing trained.
 */
static int verify_link_cap_once(struct frb_device *adev, struct frb_connector *c, uint32_t max_rate,
				uint32_t max_lanes, uint32_t *rate, uint32_t *lanes, int *fail_count)
{
	uint32_t cur_rate = max_rate, cur_lanes = max_lanes;
	int status, success = 0;

	do {
		frb_dce12_dp_enable_link_phy(adev, c, cur_rate, cur_lanes);
		status = frb_dce12_dp_perform_link_training(adev, c, cur_rate, cur_lanes);
		if (status == LT_SUCCESS) {
			success = 1;
			udelay(1000);
			if (link_lost(adev, c, cur_lanes))
				(*fail_count)++;
		} else if (status == LT_LINK_LOSS) {
			success = 1;
			(*fail_count)++;
		} else {
			(*fail_count)++;
			frb_info(adev, "display: link %c: training failed at 0x%02x x%u (%d) while verifying the link",
				 'A' + c->transmitter, cur_rate, cur_lanes, status);
		}
		frb_dce12_dp_disable_link_phy(adev, c);
	} while (!success && frb_dce12_dp_decide_fallback(&max_rate, max_lanes, &cur_rate, &cur_lanes, status));
	if (!success)
		return -EIO;
	*rate = cur_rate;
	*lanes = cur_lanes;
	return 0;
}

/*
 * verify_link_capability_destructive: link_set_all_streams_dpms_off_for_link
 * (no streams; the PHY off in case the VBIOS left it on), then
 * dp_verify_link_cap_with_retries (LINK_TRAINING_MAX_VERIFY_RETRY 3): up to
 * three rounds of dp_verify_link_cap, done at the first round without a
 * failure; otherwise the last round's result stands. When nothing trains
 * at all DC's cap is the fail-safe RBR x1; Firebrand leaves it unverified
 * (0: decide_link_settings then works from the sink's reported maximum and
 * the mode set's own training and fallback have the last word).
 */
static int verify_link_cap(struct frb_device *adev, struct frb_connector *c)
{
	uint32_t max_rate = c->dpcd[1], max_lanes = c->dpcd[2] & 0x1f, rate = 0, lanes = 0;
	int attempt, fail_count = 0, r = -EIO;

	if (max_rate > 0x1e)
		max_rate = 0x1e;	/* HBR3, the DCE 12 encoder's maximum */
	frb_dce12_dp_disable_link_phy(adev, c);
	c->verified_rate = 0;
	c->verified_lanes = 0;
	for (attempt = 0; attempt < 3; attempt++) {
		if (!hpd_sense(adev, c))
			return -ENODEV;
		if (!verify_link_cap_once(adev, c, max_rate, max_lanes, &rate, &lanes, &fail_count)) {
			c->verified_rate = (uint8_t)rate;
			c->verified_lanes = (uint8_t)lanes;
			r = 0;
			if (!fail_count)
				break;
		}
		mdelay(10);
	}
	if (!r && (c->verified_rate != max_rate || c->verified_lanes != max_lanes))
		frb_info(adev, "display: link %c: verified at 0x%02x x%u, below the sink's 0x%02x x%u (%d failed training(s))",
			 'A' + c->transmitter, c->verified_rate, c->verified_lanes, max_rate, max_lanes, fail_count);
	return r;
}

/*
 * link_detection.c query_hdcp_capability for DP (dc_process_hdcp_msg, five
 * tries each): HDCP 2.2 RxCaps (3 bytes at 0x6921d), HDCP 1.4 Bcaps (0x68028).
 */
static void query_hdcp_capability(struct frb_device *adev, struct frb_connector *c)
{
	uint8_t rx_caps[3], bcaps = 0;
	int i;

	memset(rx_caps, 0, sizeof(rx_caps));
	for (i = 0; i < 5; i++)
		if (frb_dce12_dpcd_read(adev, c, 0x6921d, rx_caps, sizeof(rx_caps)) >= 0)
			break;
	for (i = 0; i < 5; i++)
		if (frb_dce12_dpcd_read(adev, c, 0x68028, &bcaps, 1) >= 0)
			break;
	c->hdcp_caps = (uint32_t)rx_caps[0] | (uint32_t)rx_caps[1] << 8 | (uint32_t)rx_caps[2] << 16 |
		       (uint32_t)bcaps << 24;
}

int frb_dce_v12_0_detect(struct frb_device *adev, struct frb_display *d)
{
	int i, found = -1;

	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *c = &d->conn[i];
		int was = c->detect_logged;

		/*
		 * A link carrying the stream keeps its sink and caps while its line
		 * is up (DC does not re-detect a link under an active stream; the
		 * destructive verification would take the picture down)
		 */
		if (d->crtc.enabled && d->crtc.conn == c && hpd_sense(adev, c)) {
			if (found < 0)
				found = i;
			continue;
		}
		(void)hpd_sense(adev, c);		/* dc_link_detect_connection_type */
		memset(c->dpcd, 0, sizeof(c->dpcd));
		c->edid_len = 0;
		if (!hpd_sense(adev, c)) {		/* dc_link_detect: link_detect_connection_type */
			c->detect_logged = 1;
			if (c->detect_logged != was)
				frb_info(adev, "display: link %d (HPD%d): nothing connected", i, c->hpd);
			continue;
		}
		if (!frb_dce12_dp_sink_present(adev, c)) {
			c->detect_logged = 2;
			if (c->detect_logged != was)
				frb_info(adev, "display: link %d: HPD high but the DDC clock is not low: no DP sink "
					 "(a passive DVI/HDMI dongle is not supported)", i);
			continue;
		}
		if (retrieve_link_cap(adev, c)) {
			c->detect_logged = 3;
			memset(c->dpcd, 0, sizeof(c->dpcd));
			if (c->detect_logged != was)
				frb_info(adev, "display: link %d: HPD high but no DPCD answers", i);
			continue;
		}
		/* detect_link_and_local_sink: the sink's EDID, then its HDCP capabilities */
		if (frb_dce12_read_edid(adev, c))
			frb_err(adev, "display: link %d: the EDID did not read", i);
		query_hdcp_capability(adev, c);
		/* link_detect: verify_link_capability, the destructive way for an SST DP sink */
		if (verify_link_cap(adev, c))
			frb_err(adev, "display: link %d: the link did not train at %u x%u (vs %u pe %u)", i,
				c->dpcd[1], c->dpcd[2] & 0x1f, c->lt_vs, c->lt_pe);
		c->detect_logged = 4;
		frb_info(adev, "display: link %d: DP sink, DPCD %x.%x, max %s x%u, sink count %u%s, sink OUI %06x; "
			 "EDID %u bytes, %02x%02x:%02x%02x, crc32 %08x; HDCP RxCaps/Bcaps %08x; trained %s x%u (vs %u pe %u)", i,
			 c->dpcd[0] >> 4, c->dpcd[0] & 0xf,
			 c->dpcd[1] == 0x14 ? "HBR2" : c->dpcd[1] == 0x1e ? "HBR3" : c->dpcd[1] == 0x0a ? "HBR" : "RBR",
			 c->dpcd[2] & 0x1f, c->sink_count & 0x3f, c->mst ? ", MST" : "", c->sink_oui, c->edid_len,
			 c->edid[8], c->edid[9], c->edid[11], c->edid[10], crc32(c->edid, c->edid_len), c->hdcp_caps,
			 c->verified_rate == 0x1e ? "HBR3" : c->verified_rate == 0x14 ? "HBR2" : c->verified_rate == 0x0a ? "HBR" :
			 c->verified_rate ? "RBR" : "none", c->verified_lanes, c->lt_vs, c->lt_pe);
		if (found < 0)
			found = i;
	}
	return found;
}

/*
 * DCE 12 behind frb_display_funcs (frb_display.c's entry points), as SI's
 * fold made DCE 6 and DCE 11 tables: the same steps the frb-vega20 branch
 * took on SOC15 inside the DCE 11 entry points. No hw_fini (DC's HPD is not
 * set up by Firebrand), no HPD pad readout, no DCE-side DPM hook (powerplay's
 * display requests are made in the mode set); DP audio is DC's, in
 * frb_dce12_hwseq.c (FirebrandAudio), with no audio_regs.
 */
static int dce_v12_0_detect(struct frb_device *adev, struct frb_display *d)
{
	return d->ready ? frb_dce_v12_0_detect(adev, d) : -1;
}

static int dce_v12_0_set_mode(struct frb_device *adev, struct frb_display *d, struct frb_connector *conn,
			      const struct frb_mode *mode, uint32_t fb_offset, uint32_t pitch_pixels,
			      uint32_t bpp, uint32_t max_link_rate)
{
	struct frb_crtc *c = &d->crtc;
	uint64_t bytes = (uint64_t)pitch_pixels * (bpp / 8) * mode->vdisplay;
	int r;

	(void)max_link_rate;
	if (!d->ready || !conn || (bpp != 8 && bpp != 16 && bpp != 32) || !frb_vram_ok(adev, fb_offset, (uint32_t)bytes) ||
	    fb_offset + bytes > adev->layout.reserved_start || (fb_offset & 0xff))
		return -EINVAL;
	c->conn = conn;
	c->mode = *mode;
	c->fb_offset = fb_offset;
	c->fb_pitch = pitch_pixels;
	c->fb_width = mode->hdisplay;
	c->fb_height = mode->vdisplay;
	c->fb_bpp = bpp;
	c->crtc_id = 0;
	r = frb_dce_v12_0_set_mode(adev, d, conn, mode, fb_offset, pitch_pixels, bpp);
	c->enabled = c->base_enabled = 1;	/* the pipe runs even when the link did not train */
	return r;
}

/* the CRTC's gamma store into the input transfer function (frb_display_set_gamma) */
static void dce_v12_0_load_lut(struct frb_device *adev, struct frb_crtc *c)
{
	(void)c;
	frb_dce_v12_0_load_gamma(adev, adev->display);
}

static int dce_v12_0_cursor_set(struct frb_device *adev, struct frb_display *d, uint32_t bar_offset,
				uint32_t width, uint32_t height)
{
	struct frb_crtc *c = &d->crtc;

	if (!d->ready)
		return -ENODEV;
	if (bar_offset == ~0u)
		return frb_dce_v12_0_cursor_set(adev, d, 0, c->cursor_width, c->cursor_height);
	if (width < 1 || height < 1 || width > FRB_CURSOR_MAX || height > FRB_CURSOR_MAX ||
	    (bar_offset & 4095) || !frb_vram_ok(adev, bar_offset, FRB_CURSOR_MAX * FRB_CURSOR_MAX * 4))
		return -EINVAL;
	return frb_dce_v12_0_cursor_set(adev, d, adev->gmc.vram_start + bar_offset, width, height);
}

static int dce_v12_0_cursor_move(struct frb_device *adev, struct frb_display *d, int x, int y, int visible)
{
	if (!d->ready)
		return -ENODEV;
	return frb_dce_v12_0_cursor_move(adev, d, x, y, visible);
}

const struct frb_display_funcs frb_dce_v12_0_funcs = {
	.name = "dce_v12_0",
	.has_audio = 1,
	.audio_regs = NULL,
	.cursor_max = FRB_CURSOR_MAX,
	.hpd_gpio_reg = 0,		/* atomfirmware: no atombios GPIO pin table walk */
	.hw_init = frb_dce_v12_0_hw_init,
	.hw_fini = NULL,
	.hpd_sense = frb_dce_v12_0_hpd_sense,
	.hpd_pad = NULL,
	.detect = dce_v12_0_detect,
	.set_mode = dce_v12_0_set_mode,
	.off = frb_dce_v12_0_off,
	.dpm_changed = NULL,
	.report = frb_dce_v12_0_report,
	.load_lut = dce_v12_0_load_lut,
	.read_lut = frb_dce_v12_0_read_lut,
	.cursor_set = dce_v12_0_cursor_set,
	.cursor_move = dce_v12_0_cursor_move,
	.pipe_crc = frb_dce_v12_0_pipe_crc,
};
