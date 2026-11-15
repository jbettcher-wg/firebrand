/*
 * atombios_dp.c (Linux 6.16): DisplayPort AUX through the VBIOS's
 * ProcessAuxChannelTransaction table, DPCD access, the EDID over I2C-over-AUX
 * (drm_dp_i2c_xfer's pattern, as the E9173 capture shows DC doing it), the
 * link configuration and the link training (clock recovery, then channel
 * equalization), with the drm_dp_helper pieces it calls written out.
 *
 * BYTE ORDER: the AUX request and reply travel through the interpreter's FB
 * scratch, which the VBIOS reads and writes as CPU-order dwords
 * (ATOM_ARG_FB). On a little-endian host the request's bytes sit in memory
 * in order; amdgpu_atombios_copy_swap makes a big-endian host hold the same
 * dword VALUES, i.e. scratch word i = le32 of request bytes 4i..4i+3. That
 * is what aux_copy_in/out do.
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

/* include/drm/display/drm_dp.h */
#define DP_AUX_I2C_WRITE		0x0
#define DP_AUX_I2C_READ			0x1
#define DP_AUX_I2C_MOT			0x4
#define DP_AUX_NATIVE_WRITE		0x8
#define DP_AUX_NATIVE_READ		0x9
#define DP_AUX_NATIVE_REPLY_NACK	(0x1 << 0)
#define DP_AUX_NATIVE_REPLY_DEFER	(0x2 << 0)
#define DP_AUX_I2C_REPLY_NACK		(0x1 << 2)
#define DP_AUX_I2C_REPLY_DEFER		(0x2 << 2)
#define DP_DPCD_REV			0x000
#define DP_MAX_LINK_RATE		0x001
#define DP_MAX_LANE_COUNT		0x002
#define  DP_MAX_LANE_COUNT_MASK		0x1f
#define  DP_TPS3_SUPPORTED		(1 << 6)
#define  DP_ENHANCED_FRAME_CAP		(1 << 7)
#define DP_MAX_DOWNSPREAD		0x003
#define DP_DOWN_STREAM_PORT_COUNT	0x007
#define  DP_OUI_SUPPORT			(1 << 7)
#define DP_TRAINING_AUX_RD_INTERVAL	0x00e
#define DP_RECEIVER_CAP_SIZE		0xf
#define DP_SINK_OUI			0x400
#define DP_BRANCH_OUI			0x500
#define DP_LINK_BW_SET			0x100
#define DP_LANE_COUNT_SET		0x101
#define  DP_LANE_COUNT_ENHANCED_FRAME_EN (1 << 7)
#define DP_TRAINING_PATTERN_SET		0x102
#define  DP_TRAINING_PATTERN_DISABLE	0
#define  DP_TRAINING_PATTERN_1		1
#define  DP_TRAINING_PATTERN_2		2
#define  DP_TRAINING_PATTERN_3		3
#define DP_TRAINING_LANE0_SET		0x103
#define  DP_TRAIN_VOLTAGE_SWING_MASK	0x3
#define  DP_TRAIN_MAX_SWING_REACHED	(1 << 2)
#define  DP_TRAIN_PRE_EMPHASIS_MASK	(3 << 3)
#define  DP_TRAIN_PRE_EMPHASIS_SHIFT	3
#define  DP_TRAIN_MAX_PRE_EMPHASIS_REACHED (1 << 5)
#define DP_DOWNSPREAD_CTRL		0x107
#define  DP_SPREAD_AMP_0_5		(1 << 4)
#define DP_LANE0_1_STATUS		0x202
#define  DP_LANE_CR_DONE		(1 << 0)
#define  DP_LANE_CHANNEL_EQ_DONE	(1 << 1)
#define  DP_LANE_SYMBOL_LOCKED		(1 << 2)
#define DP_LANE_ALIGN_STATUS_UPDATED	0x204
#define  DP_INTERLANE_ALIGN_DONE	(1 << 0)
#define DP_ADJUST_REQUEST_LANE0_1	0x206
#define DP_SET_POWER			0x600
#define  DP_SET_POWER_D0		0x1
#define DP_LINK_STATUS_SIZE		6

/* --- AUX through the VBIOS ---------------------------------------------- */

static void aux_copy_in(struct frb_device *adev, const uint8_t *src, int n)
{
	uint32_t *s = adev->atom->scratch + 1;	/* amdgpu: atom_context->scratch + 1 */
	uint8_t tmp[20];
	int i;

	memset(tmp, 0, sizeof(tmp));
	memcpy(tmp, src, n);
	for (i = 0; i < (n + 3) / 4; i++)
		s[i] = frb_get_le32(tmp, 4 * i);
}

static void aux_copy_out(struct frb_device *adev, uint8_t *dst, int n)
{
	const uint32_t *s = adev->atom->scratch + 1 + 4;	/* base + 16 bytes */
	uint8_t tmp[20];
	int i;

	for (i = 0; i < (n + 3) / 4; i++)
		frb_put_le32(tmp, 4 * i, s[i]);
	memcpy(dst, tmp, n);
}

/* amdgpu_atombios_dp_process_aux_ch */
static int process_aux_ch(struct frb_device *adev, struct frb_connector *conn, const uint8_t *send,
			  int send_bytes, uint8_t *recv, int recv_size, uint8_t delay, uint8_t *ack)
{
	int index = GetIndexIntoMasterTable(COMMAND, ProcessAuxChannelTransaction);
	struct frb_ps ps;
	int recv_bytes;
	uint8_t status;

	if (!adev->atom->scratch || adev->atom->scratch_size_bytes < 40)
		return -EIO;
	aux_copy_in(adev, send, send_bytes);
	frb_ps_init(&ps);
	frb_ps_le16(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, lpAuxRequest), 0 + 4);
	frb_ps_le16(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, lpDataOut), 16 + 4);
	frb_ps_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucDataOutLen), 0);
	frb_ps_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucChannelID), conn->i2c_id);
	frb_ps_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucDelay), delay / 10);
	frb_ps_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucHPD_ID), conn->hpd < 0 ? 0 : conn->hpd);
	frb_ps_exec(adev, index, &ps);
	status = frb_ps_get_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucReplyStatus));
	*ack = status;
	if (status == 1)
		return -ETIMEDOUT;
	if (status == 2 || status == 3)
		return -EIO;
	recv_bytes = frb_ps_get_u8(&ps, OFS(PROCESS_AUX_CHANNEL_TRANSACTION_PARAMETERS_V2, ucDataOutLen));
	if (recv_bytes > recv_size)
		recv_bytes = recv_size;
	if (recv && recv_size)
		aux_copy_out(adev, recv, recv_bytes);
	return recv_bytes;
}

/*
 * amdgpu_atombios_dp_aux_transfer: one AUX message of up to 16 bytes.
 * Returns the payload size or a negative errno; *reply gets the reply code.
 */
int frb_dp_aux(struct frb_device *adev, struct frb_connector *conn, int request, uint32_t address,
	       uint8_t *buf, int size, uint8_t *reply)
{
	uint8_t tx[20], ack = 0;
	int tx_size, ret;

	if (size > 16)
		return -EINVAL;
	tx[0] = address & 0xff;
	tx[1] = (address >> 8) & 0xff;
	tx[2] = (uint8_t)((request << 4) | ((address >> 16) & 0xf));
	tx[3] = size ? (size - 1) : 0;
	switch (request & ~DP_AUX_I2C_MOT) {
	case DP_AUX_NATIVE_WRITE:
	case DP_AUX_I2C_WRITE:
		tx_size = 4 + size;
		tx[3] |= (size == 0 ? 3 : tx_size) << 4;
		memcpy(tx + 4, buf, size);
		ret = process_aux_ch(adev, conn, tx, tx_size, NULL, 0, 0, &ack);
		if (ret >= 0)
			ret = size;
		break;
	case DP_AUX_NATIVE_READ:
	case DP_AUX_I2C_READ:
		tx_size = 4;
		tx[3] |= (size == 0 ? 3 : tx_size) << 4;
		ret = process_aux_ch(adev, conn, tx, tx_size, buf, size, 0, &ack);
		break;
	default:
		return -EINVAL;
	}
	if (reply)
		*reply = ack >> 4;
	return ret;
}

/* drm_dp_dpcd_access: retries on DEFER, chunks of 16 */
static int dpcd_access(struct frb_device *adev, struct frb_connector *conn, int request, uint32_t addr,
		       uint8_t *buf, int n)
{
	int done = 0;

	while (done < n) {
		int chunk = n - done > 16 ? 16 : n - done, tries, ret = -EIO;
		uint8_t reply = 0;

		for (tries = 0; tries < 32; tries++) {
			ret = frb_dp_aux(adev, conn, request, addr + done, buf + done, chunk, &reply);
			if (ret < 0) {
				adev->os->delay_us(adev->os->cookie, 500);
				continue;
			}
			if ((reply & 0x3) == DP_AUX_NATIVE_REPLY_DEFER) {
				adev->os->delay_us(adev->os->cookie, 500);
				ret = -EAGAIN;
				continue;
			}
			if ((reply & 0x3) == DP_AUX_NATIVE_REPLY_NACK)
				return -EIO;
			break;
		}
		if (ret <= 0)
			return ret < 0 ? ret : -EIO;
		done += ret;
	}
	return done;
}

/*
 * DCE 12 (SOC15) has no AUX command table: its DPCD goes through DC's dce_aux
 * engine (frb_dce12_aux.c), 16 bytes a message. The DCE 11 callers (frbctl
 * dpcd, the sink CRC) keep this entry point on both.
 */
int frb_dce12_dpcd_read(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, uint8_t *buf, uint32_t size);
int frb_dce12_dpcd_write(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, const uint8_t *buf,
			 uint32_t size);

static int dce12_dpcd(struct frb_device *adev, struct frb_connector *conn, int write, uint32_t addr, uint8_t *buf, int n)
{
	int done = 0;

	while (done < n) {
		int chunk = n - done > 16 ? 16 : n - done, r;

		r = write ? frb_dce12_dpcd_write(adev, conn, addr + done, buf + done, chunk) :
			    frb_dce12_dpcd_read(adev, conn, addr + done, buf + done, chunk);
		if (r != chunk)
			return r < 0 ? r : -EIO;
		done += chunk;
	}
	return done;
}

int frb_dp_dpcd_read(struct frb_device *adev, struct frb_connector *conn, uint32_t addr, uint8_t *buf, int n)
{
	if (frb_is_soc15(adev))
		return dce12_dpcd(adev, conn, 0, addr, buf, n);
	return dpcd_access(adev, conn, DP_AUX_NATIVE_READ, addr, buf, n);
}

int frb_dp_dpcd_write(struct frb_device *adev, struct frb_connector *conn, uint32_t addr, const uint8_t *buf, int n)
{
	if (frb_is_soc15(adev))
		return dce12_dpcd(adev, conn, 1, addr, (uint8_t *)buf, n);
	return dpcd_access(adev, conn, DP_AUX_NATIVE_WRITE, addr, (uint8_t *)buf, n);
}

static int dpcd_writeb(struct frb_device *adev, struct frb_connector *conn, uint32_t addr, uint8_t v)
{
	return frb_dp_dpcd_write(adev, conn, addr, &v, 1);
}

/* amdgpu_atombios_dp_encoder_service / amdgpu_atombios_dp_get_sinktype */
uint8_t frb_dp_get_sinktype(struct frb_device *adev, struct frb_connector *conn)
{
	struct frb_ps ps;

	frb_ps_init(&ps);
	frb_ps_le16(&ps, OFS(DP_ENCODER_SERVICE_PARAMETERS, ucLinkClock), 0);
	frb_ps_u8(&ps, OFS(DP_ENCODER_SERVICE_PARAMETERS, ucAction) - 1, conn->i2c_id);	/* ucConfig / ucI2cId */
	frb_ps_u8(&ps, OFS(DP_ENCODER_SERVICE_PARAMETERS, ucAction), ATOM_DP_ACTION_GET_SINK_TYPE);
	frb_ps_u8(&ps, OFS(DP_ENCODER_SERVICE_PARAMETERS, ucLaneNum), 0);
	frb_ps_exec(adev, GetIndexIntoMasterTable(COMMAND, DPEncoderService), &ps);
	return frb_ps_get_u8(&ps, OFS(DP_ENCODER_SERVICE_PARAMETERS, ucStatus));
}

/* amdgpu_atombios_dp_probe_oui: reads, for the log */
static void probe_oui(struct frb_device *adev, struct frb_connector *conn)
{
	uint8_t s[3], b[3];
	int rs, rb;

	if (!(conn->dpcd[DP_DOWN_STREAM_PORT_COUNT] & DP_OUI_SUPPORT))
		return;
	rs = frb_dp_dpcd_read(adev, conn, DP_SINK_OUI, s, 3);
	rb = frb_dp_dpcd_read(adev, conn, DP_BRANCH_OUI, b, 3);
	frb_info(adev, "display: sink OUI %02x%02x%02x%s, branch OUI %02x%02x%02x%s",
		 s[0], s[1], s[2], rs == 3 ? "" : " (unread)", b[0], b[1], b[2], rb == 3 ? "" : " (unread)");
}

/* amdgpu_atombios_dp_get_dpcd: the receiver capabilities, DP_RECEIVER_CAP_SIZE bytes */
int frb_dp_get_dpcd(struct frb_device *adev, struct frb_connector *conn)
{
	int r;

	memset(conn->dpcd, 0, sizeof(conn->dpcd));
	r = frb_dp_dpcd_read(adev, conn, DP_DPCD_REV, conn->dpcd, DP_RECEIVER_CAP_SIZE);
	if (r != DP_RECEIVER_CAP_SIZE) {
		conn->dpcd[0] = 0;
		return r < 0 ? r : -EIO;
	}
	frb_info(adev, "display: DPCD %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		 conn->dpcd[0], conn->dpcd[1], conn->dpcd[2], conn->dpcd[3], conn->dpcd[4], conn->dpcd[5],
		 conn->dpcd[6], conn->dpcd[7], conn->dpcd[8], conn->dpcd[9], conn->dpcd[10], conn->dpcd[11],
		 conn->dpcd[12], conn->dpcd[13], conn->dpcd[14]);
	probe_oui(adev, conn);
	return 0;
}

/* one I2C-over-AUX message, retried on DEFER (drm_dp_i2c_do_msg) */
static int i2c_msg(struct frb_device *adev, struct frb_connector *conn, int request, uint8_t *buf, int n)
{
	int tries, ret = -EIO;
	uint8_t reply = 0;

	for (tries = 0; tries < 16; tries++) {
		ret = frb_dp_aux(adev, conn, request, 0x50, buf, n, &reply);
		if (ret < 0) {
			adev->os->delay_us(adev->os->cookie, 500);
			continue;
		}
		if ((reply & 0x3) == DP_AUX_NATIVE_REPLY_DEFER || (reply & 0xc) == DP_AUX_I2C_REPLY_DEFER) {
			adev->os->delay_us(adev->os->cookie, 500);
			ret = -EAGAIN;
			continue;
		}
		if ((reply & 0x3) == DP_AUX_NATIVE_REPLY_NACK || (reply & 0xc) == DP_AUX_I2C_REPLY_NACK)
			return -EIO;
		return ret;
	}
	return ret;
}

/*
 * The EDID at DDC address 0x50, 128-byte blocks: the base block and the
 * first extension, all the 8-bit word offset reaches (blocks 2+ need the
 * E-DDC segment pointer at 0x30, which no monitor here needs yet).
 */
int frb_dp_read_edid(struct frb_device *adev, struct frb_connector *conn)
{
	int block, nblocks = 1, off, r;
	uint8_t sum;

	conn->edid_len = 0;
	for (block = 0; block < nblocks && block < 2; block++) {
		uint8_t offset = (uint8_t)(block * 128);

		/* write the offset (MOT), then read with MOT, then a bare read to stop */
		if (i2c_msg(adev, conn, DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT, &offset, 1) < 0)
			return -EIO;
		for (off = 0; off < 128; off += 16) {
			r = i2c_msg(adev, conn, DP_AUX_I2C_READ | DP_AUX_I2C_MOT, conn->edid + block * 128 + off, 16);
			if (r != 16)
				return -EIO;
		}
		i2c_msg(adev, conn, DP_AUX_I2C_READ, NULL, 0);
		for (sum = 0, off = 0; off < 128; off++)
			sum += conn->edid[block * 128 + off];
		if (sum) {
			frb_err(adev, "display: EDID block %d checksum %02x", block, sum);
			return -EIO;
		}
		if (block == 0) {
			nblocks = 1 + conn->edid[126];
			if (nblocks > 2)
				frb_info(adev, "display: EDID has %d extensions; reading the first", nblocks - 1);
		}
		conn->edid_len = (block + 1) * 128;
	}
	return 0;
}

/* amdgpu_atombios_dp_get_dp_link_config, 24 bpp; max_rate caps the rate (0 = sink max) */
int frb_dp_set_link_config(struct frb_connector *conn, uint32_t pix_clock, uint32_t max_rate)
{
	static const unsigned link_rates[3] = { 162000, 270000, 540000 };
	unsigned bpp = 24, sink_rate, max_lanes, lanes, i;

	switch (conn->dpcd[DP_MAX_LINK_RATE]) {
	case 0x06: sink_rate = 162000; break;
	case 0x0a: sink_rate = 270000; break;
	case 0x14:
	case 0x1e: sink_rate = 540000; break;	/* HBR3 sinks: legacy amdgpu tops at HBR2 */
	default:   sink_rate = 162000; break;
	}
	if (max_rate && max_rate < sink_rate)
		sink_rate = max_rate;
	max_lanes = conn->dpcd[DP_MAX_LANE_COUNT] & DP_MAX_LANE_COUNT_MASK;
	for (i = 0; i < 3 && link_rates[i] <= sink_rate; i++) {
		for (lanes = 1; lanes <= max_lanes; lanes <<= 1) {
			if ((lanes * link_rates[i] * 8) / bpp >= pix_clock) {
				conn->dp_lane_count = lanes;
				conn->dp_clock = link_rates[i];
				return 0;
			}
		}
	}
	conn->dp_clock = 0;
	conn->dp_lane_count = 0;
	return -ERANGE;
}

void frb_dp_set_rx_power_state(struct frb_device *adev, struct frb_connector *conn, uint8_t state)
{
	if (conn->dpcd[0] >= 0x11) {
		dpcd_writeb(adev, conn, DP_SET_POWER, state);
		adev->os->delay_us(adev->os->cookie, 1000);
	}
}

/* --- link training (amdgpu_atombios_dp_link_train and drm_dp_helper) ------- */

struct train {
	struct frb_device *adev;
	struct frb_crtc *c;
	struct frb_connector *conn;
	uint8_t train_set[4];
	uint8_t link_status[DP_LINK_STATUS_SIZE];
	int tries;
	int tp3;
};

static uint8_t lane_status(const uint8_t *ls, int lane)
{
	return (ls[lane >> 1] >> ((lane & 1) * 4)) & 0xf;
}

static int clock_recovery_ok(const uint8_t *ls, int lanes)
{
	int i;

	for (i = 0; i < lanes; i++)
		if (!(lane_status(ls, i) & DP_LANE_CR_DONE))
			return 0;
	return 1;
}

static int channel_eq_ok(const uint8_t *ls, int lanes)
{
	int i;

	if (!(ls[DP_LANE_ALIGN_STATUS_UPDATED - DP_LANE0_1_STATUS] & DP_INTERLANE_ALIGN_DONE))
		return 0;
	for (i = 0; i < lanes; i++) {
		uint8_t s = lane_status(ls, i);

		if ((s & (DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED)) !=
		    (DP_LANE_CR_DONE | DP_LANE_CHANNEL_EQ_DONE | DP_LANE_SYMBOL_LOCKED))
			return 0;
	}
	return 1;
}

/* amdgpu_atombios_dp_get_adjust_train */
static void get_adjust_train(struct train *t)
{
	const uint8_t *ls = t->link_status;
	uint8_t v = 0, p = 0;
	int lane;

	for (lane = 0; lane < (int)t->conn->dp_lane_count; lane++) {
		uint8_t adj = ls[DP_ADJUST_REQUEST_LANE0_1 - DP_LANE0_1_STATUS + (lane >> 1)];
		int shift = (lane & 1) * 4;
		uint8_t this_v = (adj >> shift) & 0x3;
		uint8_t this_p = ((adj >> (shift + 2)) & 0x3) << DP_TRAIN_PRE_EMPHASIS_SHIFT;

		if (this_v > v)
			v = this_v;
		if (this_p > p)
			p = this_p;
	}
	if (v >= 3)
		v |= DP_TRAIN_MAX_SWING_REACHED;
	if (p >= (3 << DP_TRAIN_PRE_EMPHASIS_SHIFT))
		p |= DP_TRAIN_MAX_PRE_EMPHASIS_REACHED;
	for (lane = 0; lane < 4; lane++)
		t->train_set[lane] = v | p;
}

static int read_link_status(struct train *t)
{
	return frb_dp_dpcd_read(t->adev, t->conn, DP_LANE0_1_STATUS, t->link_status, DP_LINK_STATUS_SIZE) ==
	       DP_LINK_STATUS_SIZE ? 0 : -EIO;
}

/*
 * drm_dp_link_train_clock_recovery_delay / _channel_eq_delay: the sink's AUX
 * read interval in 4 ms units, 0 = 100 us (CR) / 400 us (EQ); DPCD 1.4+
 * sinks always 100 us for CR.
 */
static void train_delay(struct train *t, int cr)
{
	uint32_t rd = t->conn->dpcd[DP_TRAINING_AUX_RD_INTERVAL] & 0x7f;
	uint32_t us;

	if (cr && (rd == 0 || t->conn->dpcd[DP_DPCD_REV] >= 0x14))
		us = 100;
	else if (rd == 0)
		us = 400;
	else
		us = rd * 4000;
	t->adev->os->delay_us(t->adev->os->cookie, us);
}

static void update_vs_emph(struct train *t)
{
	/* the source: all lanes at once; then the sink */
	frb_atombios_encoder_setup_dig_transmitter(t->adev, t->conn, t->c, ATOM_TRANSMITTER_ACTION_SETUP_VSEMPH,
						   0, t->train_set[0]);
	frb_dp_dpcd_write(t->adev, t->conn, DP_TRAINING_LANE0_SET, t->train_set, t->conn->dp_lane_count);
}

static void set_tp(struct train *t, int tp)
{
	int rtp = ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN1;

	if (tp == DP_TRAINING_PATTERN_2)
		rtp = ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN2;
	else if (tp == DP_TRAINING_PATTERN_3)
		rtp = ATOM_ENCODER_CMD_DP_LINK_TRAINING_PATTERN3;
	frb_atombios_encoder_setup_dig_encoder(t->adev, t->c, rtp, 0);
	dpcd_writeb(t->adev, t->conn, DP_TRAINING_PATTERN_SET, (uint8_t)tp);
}

static int link_train_init(struct train *t)
{
	struct frb_connector *conn = t->conn;
	uint8_t tmp;

	frb_dp_set_rx_power_state(t->adev, conn, DP_SET_POWER_D0);
	dpcd_writeb(t->adev, conn, DP_DOWNSPREAD_CTRL, (conn->dpcd[DP_MAX_DOWNSPREAD] & 0x1) ? DP_SPREAD_AMP_0_5 : 0);
	tmp = (uint8_t)conn->dp_lane_count;
	if (conn->dpcd[DP_DPCD_REV] >= 0x11 && (conn->dpcd[DP_MAX_LANE_COUNT] & DP_ENHANCED_FRAME_CAP))	/* drm_dp_enhanced_frame_cap */
		tmp |= DP_LANE_COUNT_ENHANCED_FRAME_EN;
	dpcd_writeb(t->adev, conn, DP_LANE_COUNT_SET, tmp);
	dpcd_writeb(t->adev, conn, DP_LINK_BW_SET, (uint8_t)(conn->dp_clock / 27000));	/* drm_dp_link_rate_to_bw_code */
	frb_atombios_encoder_setup_dig_encoder(t->adev, t->c, ATOM_ENCODER_CMD_DP_LINK_TRAINING_START, 0);
	dpcd_writeb(t->adev, conn, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
	return 0;
}

static int link_train_cr(struct train *t)
{
	uint8_t voltage = 0xff;
	int i;

	set_tp(t, DP_TRAINING_PATTERN_1);
	memset(t->train_set, 0, 4);
	update_vs_emph(t);
	t->adev->os->delay_us(t->adev->os->cookie, 400);
	t->tries = 0;
	for (;;) {
		train_delay(t, 1);
		if (read_link_status(t)) {
			frb_err(t->adev, "displayport link status failed");
			return -EIO;
		}
		if (clock_recovery_ok(t->link_status, t->conn->dp_lane_count))
			return 0;
		for (i = 0; i < (int)t->conn->dp_lane_count; i++)
			if (!(t->train_set[i] & DP_TRAIN_MAX_SWING_REACHED))
				break;
		if (i == (int)t->conn->dp_lane_count) {
			frb_err(t->adev, "clock recovery reached max voltage");
			return -EIO;
		}
		if ((t->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK) == voltage) {
			if (++t->tries == 5) {
				frb_err(t->adev, "clock recovery tried 5 times");
				return -EIO;
			}
		} else {
			t->tries = 0;
		}
		voltage = t->train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK;
		get_adjust_train(t);
		update_vs_emph(t);
	}
}

static int link_train_ce(struct train *t)
{
	set_tp(t, t->tp3 ? DP_TRAINING_PATTERN_3 : DP_TRAINING_PATTERN_2);
	t->tries = 0;
	for (;;) {
		train_delay(t, 0);
		if (read_link_status(t)) {
			frb_err(t->adev, "displayport link status failed");
			return -EIO;
		}
		if (channel_eq_ok(t->link_status, t->conn->dp_lane_count))
			return 0;
		if (t->tries > 5) {
			frb_err(t->adev, "channel eq failed: 5 tries");
			return -EIO;
		}
		get_adjust_train(t);
		update_vs_emph(t);
		t->tries++;
	}
}

int frb_dp_link_train(struct frb_device *adev, struct frb_crtc *c)
{
	struct train t;
	uint8_t tmp = 0;
	int r;

	memset(&t, 0, sizeof(t));
	t.adev = adev;
	t.c = c;
	t.conn = c->conn;
	if (frb_dp_dpcd_read(adev, t.conn, DP_MAX_LANE_COUNT, &tmp, 1) == 1)
		t.tp3 = (tmp & DP_TPS3_SUPPORTED) != 0;
	link_train_init(&t);
	r = link_train_cr(&t);
	if (!r)
		r = link_train_ce(&t);
	/* amdgpu_atombios_dp_link_train_finish */
	adev->os->delay_us(adev->os->cookie, 400);
	dpcd_writeb(adev, t.conn, DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);
	frb_atombios_encoder_setup_dig_encoder(adev, c, ATOM_ENCODER_CMD_DP_LINK_TRAINING_COMPLETE, 0);
	frb_info(adev, "display: DP link training at %u MHz x%u: %s (voltage swing %u, pre-emphasis %u; "
		 "lane status %02x %02x, align %02x)", t.conn->dp_clock / 1000, t.conn->dp_lane_count,
		 r ? "FAILED" : "done", t.train_set[0] & DP_TRAIN_VOLTAGE_SWING_MASK,
		 (t.train_set[0] & DP_TRAIN_PRE_EMPHASIS_MASK) >> DP_TRAIN_PRE_EMPHASIS_SHIFT,
		 t.link_status[0], t.link_status[1], t.link_status[2]);
	return r;
}

/*
 * The sink's CRC of what it receives (drm_dp_start_crc, drm_dp_aux_get_crc,
 * drm_dp_stop_crc): one 16-bit CRC per colour component of a whole frame.
 * Waits for the CRC count to move twice (the first may cover a partial
 * frame). -ENODEV when the sink has no test CRC.
 */
#define DP_TEST_CRC_R_CR		0x240
#define DP_TEST_SINK_MISC		0x246
#define  DP_TEST_CRC_SUPPORTED		(1 << 5)
#define  DP_TEST_COUNT_MASK		0xf
#define DP_TEST_SINK			0x270
#define  DP_TEST_SINK_START		(1 << 0)

int frb_dp_sink_crc(struct frb_device *adev, struct frb_connector *conn, uint16_t crc[3])
{
	uint8_t misc = 0, sink = 0, count, b[6];
	int moved = 0, i, r = -ETIMEDOUT;

	if (frb_dp_dpcd_read(adev, conn, DP_TEST_SINK_MISC, &misc, 1) != 1)
		return -EIO;
	if (!(misc & DP_TEST_CRC_SUPPORTED))
		return -ENODEV;
	if (frb_dp_dpcd_read(adev, conn, DP_TEST_SINK, &sink, 1) != 1)
		return -EIO;
	dpcd_writeb(adev, conn, DP_TEST_SINK, sink | DP_TEST_SINK_START);
	count = misc & DP_TEST_COUNT_MASK;
	for (i = 0; i < 40 && moved < 2; i++) {	/* 200 ms: a dozen frames at 60 Hz */
		adev->os->delay_us(adev->os->cookie, 5000);
		if (frb_dp_dpcd_read(adev, conn, DP_TEST_SINK_MISC, &misc, 1) != 1)
			break;
		if ((misc & DP_TEST_COUNT_MASK) != count) {
			count = misc & DP_TEST_COUNT_MASK;
			moved++;
		}
	}
	if (moved == 2 && frb_dp_dpcd_read(adev, conn, DP_TEST_CRC_R_CR, b, 6) == 6) {
		crc[0] = (uint16_t)(b[0] | (b[1] << 8));
		crc[1] = (uint16_t)(b[2] | (b[3] << 8));
		crc[2] = (uint16_t)(b[4] | (b[5] << 8));
		r = 0;
	}
	dpcd_writeb(adev, conn, DP_TEST_SINK, sink & ~DP_TEST_SINK_START);
	return r;
}

/* the sink's own view of the link, for the log */
void frb_dp_link_report(struct frb_device *adev, struct frb_connector *conn)
{
	uint8_t s[8], cfg[2];

	memset(s, 0, sizeof(s));
	memset(cfg, 0, sizeof(cfg));
	frb_dp_dpcd_read(adev, conn, 0x200, s, 8);
	frb_dp_dpcd_read(adev, conn, DP_LINK_BW_SET, cfg, 2);
	frb_info(adev, "display: sink: count %02x, lanes %02x %02x, align %02x, sink status %02x; "
		 "link bw %02x lanes %02x", s[0], s[2], s[3], s[4], s[5], cfg[0], cfg[1]);
}
