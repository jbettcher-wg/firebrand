/*
 * DCE 12 (Vega) display internals shared by frb_dce_v12_0.c and its helpers.
 * Registers are SOC15-addressed: DCE_REG(mmFOO) is the absolute dword
 * offset; instance n of a replicated block is DCE_REG(mmFOO0_...) + n * its
 * stride (the strides are the offset differences in dce_12_0_offset.h).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_DCE12_H
#define FRB_DCE12_H

#include "frb_soc15.h"
#include "frb_display.h"
#include "include/asic_reg/dce/dce_12_0_offset.h"
#include "include/asic_reg/dce/dce_12_0_sh_mask.h"
#include "include/asic_reg/mmhub/mmhub_1_0_offset.h"
#include "include/asic_reg/mmhub/mmhub_1_0_sh_mask.h"

#define DCE_REG(reg)	(adev->reg_offset[DCE_HWIP][0][reg##_BASE_IDX] + (reg))

#define FRB_DCE12_HPD_STRIDE	(mmHPD1_DC_HPD_INT_STATUS - mmHPD0_DC_HPD_INT_STATUS)
#define FRB_DCE12_AUX_STRIDE	(mmDP_AUX1_AUX_CONTROL - mmDP_AUX0_AUX_CONTROL)
#define FRB_DCE12_DIG_STRIDE	(mmDIG1_DIG_BE_CNTL - mmDIG0_DIG_BE_CNTL)
#define FRB_DCE12_CRTC_STRIDE	(mmCRTC1_CRTC_BLANK_CONTROL - mmCRTC0_CRTC_BLANK_CONTROL)
#define FRB_DCE12_DDC_STRIDE	(mmDC_GPIO_DDC2_MASK - mmDC_GPIO_DDC1_MASK)

/* REG_UPDATE: read, replace the masked bits, write (always writes, as DC's generic_reg_update) */
void frb_dce12_update(struct frb_device *adev, uint32_t reg, uint32_t mask, uint32_t val);

/* command_table2.c transmitter_control_v1_6 */
int frb_dce12_transmitter_control(struct frb_device *adev, struct frb_connector *c, uint8_t action,
				  uint8_t mode_laneset, uint8_t lanes, uint32_t pixel_clock_khz);

/* frb_dce12_aux.c: one AUX transaction (dce_aux + dm_dp_aux_transfer) and DRM's DPCD access */
#define FRB_AUX_ACTION_READ	0x10	/* i2caux_transaction_action: the read bit */
#define FRB_AUX_I2C_WRITE	0x00
#define FRB_AUX_I2C_READ	0x10
#define FRB_AUX_I2C_WRITE_MOT	0x40
#define FRB_AUX_I2C_READ_MOT	0x50
#define FRB_AUX_DP_WRITE	0x80
#define FRB_AUX_DP_READ		0x90
enum { FRB_AUX_RET_SUCCESS = 0, FRB_AUX_RET_INVALID_REPLY, FRB_AUX_RET_TIMEOUT, FRB_AUX_RET_HPD_DISCON,
       FRB_AUX_RET_ENGINE_ACQUIRE };
struct frb_aux_msg {
	uint8_t action;
	uint32_t address;
	uint8_t buf[16];
	uint32_t size;
	uint8_t reply;		/* the reply command nibble */
	int op;			/* FRB_AUX_RET_* */
};
int frb_dce12_aux_transfer(struct frb_device *adev, const struct frb_connector *c, struct frb_aux_msg *m);
int frb_dce12_dpcd_read(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, uint8_t *buf, uint32_t size);
int frb_dce12_dpcd_write(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, const uint8_t *buf,
			 uint32_t size);
/* dm_helpers_read_local_edid over I2C-over-AUX (DRM's drm_edid_read_ddc), the blocks into c->edid */
int frb_dce12_read_edid(struct frb_device *adev, struct frb_connector *c);
int frb_dce12_dp_sink_present(struct frb_device *adev, const struct frb_connector *c);

/* frb_dce_v12_0.c's link encoder and timing generator pieces the mode set shares */
int frb_dce12_dig_enabled(struct frb_device *adev, const struct frb_connector *c);
void frb_dce12_link_disable_output(struct frb_device *adev, struct frb_connector *c);
int frb_dce12_dpcd_write1(struct frb_device *adev, struct frb_connector *c, uint32_t addr, uint8_t v);
void frb_dce12_tg_disable_vga(struct frb_device *adev, int crtc);
void frb_dce12_set_source_specific_data(struct frb_device *adev, struct frb_connector *c);
int frb_dce12_dp_enable_link_phy(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes);
void frb_dce12_dp_disable_link_phy(struct frb_device *adev, struct frb_connector *c);
/* dp_perform_link_training: LT_* */
#include "frb_dp_fallback.h"	/* LT_*, frb_dce12_dp_decide_fallback */
int frb_dce12_dp_perform_link_training(struct frb_device *adev, struct frb_connector *c, uint32_t rate, uint32_t lanes);
void frb_dce12_tg_set_blank(struct frb_device *adev, int crtc, int blank);
int frb_dce12_tg_is_blanked(struct frb_device *adev, int crtc);
void frb_dce12_tg_wait_blank(struct frb_device *adev, int crtc);

#endif
