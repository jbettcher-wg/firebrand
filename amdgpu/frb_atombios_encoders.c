/*
 * atombios_encoders.c (Linux 6.16), the DIG (UNIPHY) paths DCE 11.2 uses:
 * DIGxEncoderControl 1.5, UNIPHYTransmitterControl 1.6, SelectCRTC_Source
 * 1.3, the transmitter INIT pass, and the DIG dpms sequence with DP link
 * training; and the arms DCE 6 takes (Linux 7.0.2): DIGxEncoderControl
 * 1.4, UNIPHYTransmitterControl 1.5, SelectCRTC_Source 1.2. Each parameter
 * block is built byte by byte, its bitfield bytes by hand (atombios.h's
 * bitfields lay out the other way round on a big-endian compiler).
 *
 * Copyright 2007-11 Advanced Micro Devices, Inc.
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

/*
 * amdgpu_atombios_encoder_get_encoder_mode. A DisplayPort connector is DP
 * once detect has found a DP sink (dp_sink_type); before that (hw_init's
 * transmitter INIT) and for passive adapters it is DVI. eDP is always DP.
 * DVI and HDMI connectors are DVI: amdgpu's arm with amdgpu.audio=0, so an
 * HDMI sink gets DVI signalling (no infoframes, no audio; dce_v11_0's AFMT
 * is not ported), which every HDMI sink takes, up to 340 MHz.
 */
int frb_atombios_encoder_get_encoder_mode(struct frb_connector *conn)
{
	if (!conn)
		return ATOM_ENCODER_MODE_DVI;
	switch (conn->connector_type) {
	case CONNECTOR_OBJECT_ID_eDP:
		return ATOM_ENCODER_MODE_DP;
	case CONNECTOR_OBJECT_ID_DISPLAYPORT:
		if (conn->dp_sink_type == CONNECTOR_OBJECT_ID_DISPLAYPORT ||
		    conn->dp_sink_type == CONNECTOR_OBJECT_ID_eDP)
			return ATOM_ENCODER_MODE_DP;
		return ATOM_ENCODER_MODE_DVI;
	default:
		return ATOM_ENCODER_MODE_DVI;
	}
}

/*
 * amdgpu_atombios_encoder_set_bios_scratch_regs: the connected/active/
 * access-request bits of BIOS_SCRATCH_0/3/6 for the connector's devices
 * (each connector here has its own encoder object, so encoder->devices &
 * connector->devices is the connector's).
 */
void frb_atombios_encoder_set_bios_scratch_regs(struct frb_device *adev, struct frb_connector *conn, int connected)
{
	static const struct { uint32_t dev, s0, s0_clear, s3, s6; } bits[] = {
		{ ATOM_DEVICE_LCD1_SUPPORT, ATOM_S0_LCD1, ATOM_S0_LCD1, ATOM_S3_LCD1_ACTIVE, ATOM_S6_ACC_REQ_LCD1 },
		{ ATOM_DEVICE_CRT1_SUPPORT, ATOM_S0_CRT1_COLOR, ATOM_S0_CRT1_MASK, ATOM_S3_CRT1_ACTIVE, ATOM_S6_ACC_REQ_CRT1 },
		{ ATOM_DEVICE_CRT2_SUPPORT, ATOM_S0_CRT2_COLOR, ATOM_S0_CRT2_MASK, ATOM_S3_CRT2_ACTIVE, ATOM_S6_ACC_REQ_CRT2 },
		{ ATOM_DEVICE_DFP1_SUPPORT, ATOM_S0_DFP1, ATOM_S0_DFP1, ATOM_S3_DFP1_ACTIVE, ATOM_S6_ACC_REQ_DFP1 },
		{ ATOM_DEVICE_DFP2_SUPPORT, ATOM_S0_DFP2, ATOM_S0_DFP2, ATOM_S3_DFP2_ACTIVE, ATOM_S6_ACC_REQ_DFP2 },
		{ ATOM_DEVICE_DFP3_SUPPORT, ATOM_S0_DFP3, ATOM_S0_DFP3, ATOM_S3_DFP3_ACTIVE, ATOM_S6_ACC_REQ_DFP3 },
		{ ATOM_DEVICE_DFP4_SUPPORT, ATOM_S0_DFP4, ATOM_S0_DFP4, ATOM_S3_DFP4_ACTIVE, ATOM_S6_ACC_REQ_DFP4 },
		{ ATOM_DEVICE_DFP5_SUPPORT, ATOM_S0_DFP5, ATOM_S0_DFP5, ATOM_S3_DFP5_ACTIVE, ATOM_S6_ACC_REQ_DFP5 },
		{ ATOM_DEVICE_DFP6_SUPPORT, ATOM_S0_DFP6, ATOM_S0_DFP6, ATOM_S3_DFP6_ACTIVE, ATOM_S6_ACC_REQ_DFP6 },
	};
	uint32_t base = adev->bios_scratch_reg_offset, s0, s3, s6;
	unsigned i;

	s0 = RREG32(base + 0);
	s3 = RREG32(base + 3);
	s6 = RREG32(base + 6);
	for (i = 0; i < ARRAY_SIZE(bits); i++) {
		if (!(conn->devices & bits[i].dev))
			continue;
		if (connected) {
			s0 |= bits[i].s0;
			s3 |= bits[i].s3;
			s6 |= bits[i].s6;
		} else {
			s0 &= ~bits[i].s0_clear;
			s3 &= ~bits[i].s3;
			s6 &= ~bits[i].s6;
		}
	}
	WREG32(base + 0, s0);
	WREG32(base + 3, s3);
	WREG32(base + 6, s6);
}

/*
 * amdgpu_dig_monitor_is_duallink: a DVI sink over 165 MHz on a dual-link
 * connector; an HDMI sink only over 340 MHz (HDMI 1.3+ runs single link to
 * there). DP never.
 */
int frb_dig_monitor_is_duallink(const struct frb_connector *c, uint32_t pixel_clock)
{
	if (!frb_connector_is_tmds(c) || !c->sink_tmds)
		return 0;
	if (c->is_hdmi)
		return pixel_clock > 340000;
	switch (c->connector_type) {
	case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I:
	case CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D:
	case CONNECTOR_OBJECT_ID_HDMI_TYPE_B:
		return pixel_clock > 165000;
	default:
		return 0;
	}
}

/* amdgpu_atombios_encoder_get_bpc */
int frb_atombios_encoder_get_bpc(struct frb_crtc *c)
{
	switch (c->bpc) {
	case 0:		return PANEL_BPC_UNDEFINE;
	case 6:		return PANEL_6BIT_PER_COLOR;
	case 10:	return PANEL_10BIT_PER_COLOR;
	case 12:	return PANEL_12BIT_PER_COLOR;
	case 16:	return PANEL_16BIT_PER_COLOR;
	case 8:
	default:	return PANEL_8BIT_PER_COLOR;
	}
}

/* amdgpu_atombios_encoder_setup_dig_encoder, the 1.5 arm */
void frb_atombios_encoder_setup_dig_encoder(struct frb_device *adev, struct frb_crtc *c, int action, int panel_mode)
{
	struct frb_connector *conn = c->conn;
	int index = GetIndexIntoMasterTable(COMMAND, DIGxEncoderControl);
	struct frb_ps ps;
	uint8_t frev, crev;

	if (conn->dig_encoder == -1)
		return;
	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 1 || (crev != 4 && crev != 5)) {
		frb_err(adev, "display: DIGxEncoderControl %u.%u not handled", frev, crev);
		return;
	}
	frb_ps_init(&ps);
	if (crev == 4) {
		/* DIG_ENCODER_CONTROL_PARAMETERS_V4 (DCE6/8); ucConfig: link rate in bits 0-1, DIG in 4-6 */
		int is_dp = frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP;
		uint8_t config = 0;

		frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucAction), action);
		frb_ps_le16(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, usPixelClock), conn->pixel_clock / 10);
		if (action == ATOM_ENCODER_CMD_SETUP_PANEL_MODE)
			frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucPanelMode), panel_mode);
		else
			frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucEncoderMode),
				  frb_atombios_encoder_get_encoder_mode(conn));
		if (is_dp)
			frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucLaneNum), conn->dp_lane_count);
		else
			frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucLaneNum),
				  frb_dig_monitor_is_duallink(conn, conn->pixel_clock) ? 8 : 4);
		if (is_dp) {
			if (conn->dp_clock == 540000)
				config |= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_5_40GHZ;
			else if (conn->dp_clock == 324000)
				config |= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_3_24GHZ;
			else if (conn->dp_clock == 270000)
				config |= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_2_70GHZ;
			else
				config |= ATOM_ENCODER_CONFIG_V4_DPLINKRATE_1_62GHZ;
		}
		config |= (uint8_t)((conn->dig_encoder & 7) << 4);	/* acConfig.ucDigSel */
		frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucConfig), config);
		frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucBitPerColor), frb_atombios_encoder_get_bpc(c));
		frb_ps_u8(&ps, OFS(DIG_ENCODER_CONTROL_PARAMETERS_V4, ucHPD_ID), conn->hpd < 0 ? 0 : conn->hpd + 1);
		frb_ps_exec(adev, index, &ps);
		return;
	}
	switch (action) {
	case ATOM_ENCODER_CMD_SETUP_PANEL_MODE:
		frb_ps_u8(&ps, OFS(DP_PANEL_MODE_SETUP_PARAMETERS_V5, ucAction), action);
		frb_ps_u8(&ps, OFS(DP_PANEL_MODE_SETUP_PARAMETERS_V5, ucPanelMode), panel_mode);
		frb_ps_u8(&ps, OFS(DP_PANEL_MODE_SETUP_PARAMETERS_V5, ucDigId), conn->dig_encoder);
		break;
	case ATOM_ENCODER_CMD_STREAM_SETUP:
		frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucAction), action);
		frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucDigId), conn->dig_encoder);
		frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucDigMode), frb_atombios_encoder_get_encoder_mode(conn));
		if (frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP)
			frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucLaneNum), conn->dp_lane_count);
		else
			frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucLaneNum),
				  frb_dig_monitor_is_duallink(conn, conn->pixel_clock) ? 8 : 4);
		frb_ps_le32(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ulPixelClock), conn->pixel_clock / 10);
		frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucBitPerColor), frb_atombios_encoder_get_bpc(c));
		frb_ps_u8(&ps, OFS(ENCODER_STREAM_SETUP_PARAMETERS_V5, ucLinkRateIn270Mhz), conn->dp_clock / 27000);
		break;
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_START:
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1:
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2:
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN3:
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN4:
	case ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE:
	case ATOM_ENCODER_CMD_DP_VIDEO_OFF:
	case ATOM_ENCODER_CMD_DP_VIDEO_ON:
		frb_ps_u8(&ps, OFS(ENCODER_GENERIC_CMD_PARAMETERS_V5, ucAction), action);
		frb_ps_u8(&ps, OFS(ENCODER_GENERIC_CMD_PARAMETERS_V5, ucDigId), conn->dig_encoder);
		break;
	default:
		frb_err(adev, "display: DIG encoder action 0x%x not supported", action);
		return;
	}
	frb_ps_exec(adev, index, &ps);
}

/* amdgpu_atombios_encoder_setup_dig_transmitter, the 1.6 arm (UNIPHY) */
void frb_atombios_encoder_setup_dig_transmitter(struct frb_device *adev, struct frb_connector *conn,
						struct frb_crtc *c, int action, uint8_t lane_num, uint8_t lane_set)
{
	int index = GetIndexIntoMasterTable(COMMAND, UNIPHYTransmitterControl);
	int is_dp = frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP;
	int dig_encoder = conn->dig_encoder;
	struct frb_ps ps;
	uint8_t frev, crev, phy = 0;

	(void)lane_num;
	if (action == ATOM_TRANSMITTER_ACTION_INIT)
		dig_encoder = 0;	/* "the encoder isn't used for init" */
	if (dig_encoder == -1)
		return;
	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 1 || (crev != 5 && crev != 6)) {
		frb_err(adev, "display: UNIPHYTransmitterControl %u.%u not handled", frev, crev);
		return;
	}
	switch (conn->encoder_id) {
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
		phy = conn->linkb ? ATOM_PHY_ID_UNIPHYB : ATOM_PHY_ID_UNIPHYA;
		break;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
		phy = conn->linkb ? ATOM_PHY_ID_UNIPHYD : ATOM_PHY_ID_UNIPHYC;
		break;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
		phy = conn->linkb ? ATOM_PHY_ID_UNIPHYF : ATOM_PHY_ID_UNIPHYE;
		break;
	case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:
		phy = ATOM_PHY_ID_UNIPHYG;
		break;
	}
	frb_ps_init(&ps);
	if (crev == 5) {
		/*
		 * DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5 (DCE6/8). ucConfig
		 * (ATOM_DIG_TRANSMITTER_CONFIG_V5): bit 1 coherent mode, bits
		 * 2-3 the PHY's clock source, bits 4-6 the HPD line + 1.
		 */
		uint8_t config = 0, clk_src;
		/* amdgpu: the CRTC's pll_id, 0 for INIT (no CRTC yet) */
		int pll_id = (c && action != ATOM_TRANSMITTER_ACTION_INIT) ? c->pll_id : 0;

		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucAction), action);
		frb_ps_le16(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, usSymClock),
			    is_dp ? conn->dp_clock / 10 : conn->pixel_clock / 10);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucPhyId), phy);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucLaneNum),
			  is_dp ? conn->dp_lane_count : frb_dig_monitor_is_duallink(conn, conn->pixel_clock) ? 8 : 4);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucConnObjId), conn->connector_type);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucDigMode),
			  frb_atombios_encoder_get_encoder_mode(conn));
		if (is_dp && adev->display && adev->display->dp_extclk)
			clk_src = ENCODER_REFCLK_SRC_EXTCLK;
		else
			clk_src = (uint8_t)pll_id;
		config |= (uint8_t)((clk_src & 3) << 2);
		/* DP requires coherent; a DFP's dig->coherent_mode is amdgpu's default (true) */
		if (is_dp || (conn->devices & ATOM_DEVICE_DFP_SUPPORT))
			config |= 1 << 1;
		config |= (uint8_t)(((conn->hpd < 0 ? 0 : conn->hpd + 1) & 7) << 4);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucConfig), config);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucDigEncoderSel), 1 << dig_encoder);
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_5, ucDPLaneSet), lane_set);
		frb_ps_exec(adev, index, &ps);
		return;
	}
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucAction), action);
	frb_ps_le32(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ulSymClock),
		    is_dp ? conn->dp_clock / 10 : conn->pixel_clock / 10);
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucPhyId), phy);
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucLaneNum),
		  is_dp ? conn->dp_lane_count : frb_dig_monitor_is_duallink(conn, conn->pixel_clock) ? 8 : 4);
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucConnObjId), conn->connector_type);
	if (action == ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH)
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucDPLaneSet), lane_set);
	else
		frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucDigMode),
			  frb_atombios_encoder_get_encoder_mode(conn));
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucHPDSel), conn->hpd < 0 ? 0 : conn->hpd + 1);
	frb_ps_u8(&ps, OFS(DIG_TRANSMITTER_CONTROL_PARAMETERS_V1_6, ucDigEncoderSel), 1 << dig_encoder);
	frb_ps_exec(adev, index, &ps);
}

/* amdgpu_atombios_encoder_set_crtc_source, the 1.3 arm */
void frb_atombios_encoder_set_crtc_source(struct frb_device *adev, struct frb_crtc *c)
{
	static const uint8_t dig_id[7] = {
		ASIC_INT_DIG1_ENCODER_ID, ASIC_INT_DIG2_ENCODER_ID, ASIC_INT_DIG3_ENCODER_ID,
		ASIC_INT_DIG4_ENCODER_ID, ASIC_INT_DIG5_ENCODER_ID, ASIC_INT_DIG6_ENCODER_ID,
		ASIC_INT_DIG7_ENCODER_ID,
	};
	int index = GetIndexIntoMasterTable(COMMAND, SelectCRTC_Source);
	struct frb_ps ps;
	uint8_t frev, crev;

	if (frb_cmd_rev(adev, index, &frev, &crev) || frev != 1 || (crev != 2 && crev != 3)) {
		frb_err(adev, "display: SelectCRTC_Source %u.%u not handled", frev, crev);
		return;
	}
	if (c->conn->dig_encoder < 0 || c->conn->dig_encoder > 6)
		return;
	frb_ps_init(&ps);
	if (crev == 2) {
		/* SELECT_CRTC_SOURCE_PARAMETERS_V2 (DCE6/8): no destination bpc */
		frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V2, ucCRTC), c->crtc_id);
		frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V2, ucEncodeMode),
			  frb_atombios_encoder_get_encoder_mode(c->conn));
		frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V2, ucEncoderID), dig_id[c->conn->dig_encoder]);
		frb_ps_exec(adev, index, &ps);
		return;
	}
	frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V3, ucCRTC), c->crtc_id);
	frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V3, ucEncodeMode), frb_atombios_encoder_get_encoder_mode(c->conn));
	frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V3, ucDstBpc), frb_atombios_encoder_get_bpc(c));
	frb_ps_u8(&ps, OFS(SELECT_CRTC_SOURCE_PARAMETERS_V3, ucEncoderID), dig_id[c->conn->dig_encoder]);
	frb_ps_exec(adev, index, &ps);
}

/* amdgpu_atombios_encoder_init_dig: every UNIPHY transmitter, action INIT */
void frb_atombios_encoder_init_dig(struct frb_device *adev, struct frb_display *d)
{
	int i;

	for (i = 0; i < d->num_connectors; i++) {
		struct frb_connector *conn = &d->conn[i];

		switch (conn->encoder_id) {
		case ENCODER_OBJECT_ID_INTERNAL_UNIPHY:
		case ENCODER_OBJECT_ID_INTERNAL_UNIPHY1:
		case ENCODER_OBJECT_ID_INTERNAL_UNIPHY2:
		case ENCODER_OBJECT_ID_INTERNAL_UNIPHY3:
			frb_atombios_encoder_setup_dig_transmitter(adev, conn, &d->crtc,
								   ATOM_TRANSMITTER_ACTION_INIT, 0, 0);
			break;
		}
	}
}

/* amdgpu_atombios_encoder_setup_dig (amdgpu_atombios_encoder_dpms for UNIPHY): DP, and DVI/HDMI (TMDS) */
int frb_atombios_encoder_dpms(struct frb_device *adev, struct frb_crtc *c, int on)
{
	struct frb_connector *conn = c->conn;
	int is_dp = frb_atombios_encoder_get_encoder_mode(conn) == ATOM_ENCODER_MODE_DP;
	int r = 0;

	if (on) {
		conn->panel_mode = DP_PANEL_MODE_EXTERNAL_DP_MODE;
		/* setup and enable the encoder */
		frb_atombios_encoder_setup_dig_encoder(adev, c, ATOM_ENCODER_CMD_SETUP, 0);
		frb_atombios_encoder_setup_dig_encoder(adev, c, ATOM_ENCODER_CMD_SETUP_PANEL_MODE, conn->panel_mode);
		/* enable the transmitter */
		frb_atombios_encoder_setup_dig_transmitter(adev, conn, c, ATOM_TRANSMITTER_ACTION_ENABLE, 0, 0);
		if (is_dp) {
			/* DP_SET_POWER_D0 is set in the link training */
			r = frb_dp_link_train(adev, c);
			frb_atombios_encoder_setup_dig_encoder(adev, c, ATOM_ENCODER_CMD_DP_VIDEO_ON, 0);
		}
	} else {
		if (is_dp)
			frb_atombios_encoder_setup_dig_encoder(adev, c, ATOM_ENCODER_CMD_DP_VIDEO_OFF, 0);
		if (is_dp)
			frb_dp_set_rx_power_state(adev, conn, 2 /* DP_SET_POWER_D3 */);
		/* disable the transmitter */
		frb_atombios_encoder_setup_dig_transmitter(adev, conn, c, ATOM_TRANSMITTER_ACTION_DISABLE, 0, 0);
	}
	return r;
}
