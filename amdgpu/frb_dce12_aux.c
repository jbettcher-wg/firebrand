/*
 * DCE 12 AUX: DC's hardware AUX engine (dce/dce_aux.c) behind its DDC GPIO
 * pair (gpio/hw_ddc.c, hw_gpio.c, gpio_service.c dal_ddc_open/close), and
 * above it the two layers Linux puts between DC and a DPCD or EDID read:
 * amdgpu_dm's dm_dp_aux_transfer and DRM's drm_dp_dpcd_access /
 * drm_dp_i2c_xfer (retries, 16-byte I2C-over-AUX chunks, MOT, the address-
 * only start and stop). Register for register what the MI50 trace shows for
 * each transaction: acquire (pad to AUX mode, SW request), IMPCAL
 * clear/calibrate, the request bytes, the SW_DONE poll, the reply, release.
 *
 * With no sink a transaction ends in AUX_SW_STATUS's HPD_DISCON or timeout
 * bits and comes back as -EIO / -ETIMEDOUT, after DRM's 32 tries.
 *
 * Copyright 2012-15 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_dce12.h"

/* gpio/hw_gpio.c: one pin of a DDC line (data or clock) */
struct ddc_pin {
	uint32_t mask, a, en, y;	/* field masks */
	uint32_t st_mask, st_a, st_en;	/* store_registers */
};

/*
 * Pasted register names, listed for tools/mk-soc15-headers.py: mmDC_GPIO_DDC1_MASK
 * mmDC_GPIO_DDC1_A mmDC_GPIO_DDC1_EN mmDC_GPIO_DDC1_Y mmDP_AUX0_AUX_ARB_CONTROL
 * mmDP_AUX0_AUX_CONTROL mmDP_AUX0_AUX_INTERRUPT_CONTROL mmDP_AUX0_AUX_SW_STATUS
 * mmDP_AUX0_AUX_SW_CONTROL mmDP_AUX0_AUX_SW_DATA
 */
#define DDC_REG(line, r)	(DCE_REG(mmDC_GPIO_DDC1_##r) + (line) * FRB_DCE12_DDC_STRIDE)

static void ddc_pins(struct ddc_pin *data, struct ddc_pin *clk)
{
	memset(data, 0, sizeof(*data));
	memset(clk, 0, sizeof(*clk));
	data->mask = DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_MASK_MASK;
	data->a = DC_GPIO_DDC1_A__DC_GPIO_DDC1DATA_A_MASK;
	data->en = DC_GPIO_DDC1_EN__DC_GPIO_DDC1DATA_EN_MASK;
	data->y = DC_GPIO_DDC1_Y__DC_GPIO_DDC1DATA_Y_MASK;
	clk->mask = DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_MASK_MASK;
	clk->a = DC_GPIO_DDC1_A__DC_GPIO_DDC1CLK_A_MASK;
	clk->en = DC_GPIO_DDC1_EN__DC_GPIO_DDC1CLK_EN_MASK;
	clk->y = DC_GPIO_DDC1_Y__DC_GPIO_DDC1CLK_Y_MASK;
}

enum { GPIO_MODE_INPUT = 1, GPIO_MODE_HARDWARE = 4 };
enum { DDC_CONFIG_MODE_AUX = 0, DDC_CONFIG_MODE_I2C = 1 };

/* dal_hw_gpio_open: store_registers, then config_mode */
static void pin_open(struct frb_device *adev, int line, struct ddc_pin *p, int mode)
{
	p->st_mask = RREG32(DDC_REG(line, MASK)) & p->mask;
	p->st_a = RREG32(DDC_REG(line, A)) & p->a;
	p->st_en = RREG32(DDC_REG(line, EN)) & p->en;
	if (mode == GPIO_MODE_INPUT) {
		/* turn off output enable, act as input pin; mask out the HW-driven signal */
		frb_dce12_update(adev, DDC_REG(line, EN), p->en, 0);
		frb_dce12_update(adev, DDC_REG(line, MASK), p->mask, p->mask);
	} else {
		/* tri-state, the pin is driven by HW */
		frb_dce12_update(adev, DDC_REG(line, MASK), p->mask, 0);
	}
}

/* dal_hw_gpio_close: restore_registers */
static void pin_close(struct frb_device *adev, int line, const struct ddc_pin *p)
{
	frb_dce12_update(adev, DDC_REG(line, MASK), p->mask, p->st_mask);
	frb_dce12_update(adev, DDC_REG(line, A), p->a, p->st_a);
	frb_dce12_update(adev, DDC_REG(line, EN), p->en, p->st_en);
}

/* dal_ddc_open: both pins, then hw_ddc set_config on the data pin's MASK register */
static void ddc_open(struct frb_device *adev, int line, struct ddc_pin *data, struct ddc_pin *clk,
		     int mode, int config)
{
	uint32_t regval;

	ddc_pins(data, clk);
	pin_open(adev, line, data, mode);
	pin_open(adev, line, clk, mode);
	regval = RREG32(DDC_REG(line, MASK));
	if (config == DDC_CONFIG_MODE_I2C) {
		if (!(regval & DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK) ||
		    !(regval & DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK)) {
			/* discharge the plug-in transient through the internal pull-downs */
			regval |= DC_GPIO_DDC1_MASK__DC_GPIO_DDC1DATA_PD_EN_MASK |
				  DC_GPIO_DDC1_MASK__DC_GPIO_DDC1CLK_PD_EN_MASK;
			WREG32(DDC_REG(line, MASK), regval);
			if (mode == GPIO_MODE_INPUT)	/* GPIO_CONFIG_TYPE_I2C_AUX_DUAL_MODE */
				mdelay(3);
		}
		if (regval & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK) {
			if (data->st_en || clk->st_en)
				mdelay(2);
			frb_dce12_update(adev, DDC_REG(line, MASK), DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK, 0);
		}
	} else if (!(regval & DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK)) {
		WREG32(DDC_REG(line, MASK), regval | DC_GPIO_DDC1_MASK__AUX_PAD1_MODE_MASK);
	}
}

static void ddc_close(struct frb_device *adev, int line, const struct ddc_pin *data, const struct ddc_pin *clk)
{
	pin_close(adev, line, clk);
	pin_close(adev, line, data);
}

/*
 * dc_link_is_dp_sink_present: the DDC pair as GPIO inputs in I2C mode; a DP
 * sink holds the clock low (three retries 1 ms apart for a short pulse).
 */
int frb_dce12_dp_sink_present(struct frb_device *adev, const struct frb_connector *c)
{
	struct ddc_pin data, clk;
	uint32_t clock_pin = 0;
	int retry = 0;

	if (c->aux < 0)
		return 1;
	ddc_open(adev, c->aux, &data, &clk, GPIO_MODE_INPUT, DDC_CONFIG_MODE_I2C);
	do {
		clock_pin = RREG32(DDC_REG(c->aux, Y)) & clk.y;
		if (clock_pin)
			udelay(1000);
		else
			break;
	} while (retry++ < 3);
	ddc_close(adev, c->aux, &data, &clk);
	return !clock_pin;
}

/* ------------------------------------------------------------------------- */
/* dce_aux.c */

#define AUX_REG(c, r)		(DCE_REG(mmDP_AUX0_##r) + (uint32_t)(c)->aux * FRB_DCE12_AUX_STRIDE)
#define AUX_POLL_TRIES		(6 * 400 / 10)	/* SW_AUX_TIMEOUT_PERIOD_MULTIPLIER * AUX_TIMEOUT_PERIOD, 10 us steps */
#define SW_CAN_ACCESS_AUX	1
#define DMCU_CAN_ACCESS_AUX	2

/* REG_WAIT: read until the field equals cond, delay between polls */
static int reg_wait(struct frb_device *adev, uint32_t reg, uint32_t mask, uint32_t cond, uint32_t delay_us, int tries)
{
	int i;

	for (i = 0; i < tries; i++) {
		if ((RREG32(reg) & mask) == cond)
			return 0;
		udelay(delay_us);
	}
	return -ETIMEDOUT;
}

static void release_engine(struct frb_device *adev, const struct frb_connector *c,
			   const struct ddc_pin *data, const struct ddc_pin *clk)
{
	ddc_close(adev, c->aux, data, clk);
	frb_dce12_update(adev, AUX_REG(c, AUX_ARB_CONTROL),
			 DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK |
			 DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK,
			 DP_AUX0_AUX_ARB_CONTROL__AUX_SW_DONE_USING_AUX_REG_MASK);
}

static int arb_status(uint32_t v)
{
	return REG_GET_FIELD(v, DP_AUX0_AUX_ARB_CONTROL, AUX_REG_RW_CNTL_STATUS);
}

/* acquire: is_engine_available, dal_ddc_open(HARDWARE, AUX), acquire_engine */
static int aux_acquire(struct frb_device *adev, const struct frb_connector *c, struct ddc_pin *data, struct ddc_pin *clk)
{
	uint32_t v;

	if (arb_status(RREG32(AUX_REG(c, AUX_ARB_CONTROL))) == DMCU_CAN_ACCESS_AUX)
		return 0;
	ddc_open(adev, c->aux, data, clk, GPIO_MODE_HARDWARE, DDC_CONFIG_MODE_AUX);
	if (arb_status(RREG32(AUX_REG(c, AUX_ARB_CONTROL))) == DMCU_CAN_ACCESS_AUX)
		goto fail;
	/* enable AUX before requesting SW access (dce120 has no AUX_RESET) */
	v = RREG32(AUX_REG(c, AUX_CONTROL));
	if (!REG_GET_FIELD(v, DP_AUX0_AUX_CONTROL, AUX_EN))
		WREG32(AUX_REG(c, AUX_CONTROL), REG_SET_FIELD(v, DP_AUX0_AUX_CONTROL, AUX_EN, 1));
	frb_dce12_update(adev, AUX_REG(c, AUX_ARB_CONTROL), DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK,
			 DP_AUX0_AUX_ARB_CONTROL__AUX_SW_USE_AUX_REG_REQ_MASK);
	if (arb_status(RREG32(AUX_REG(c, AUX_ARB_CONTROL))) == SW_CAN_ACCESS_AUX)
		return 1;
fail:
	release_engine(adev, c, data, clk);
	return 0;
}

/* REG_UPDATE_SEQ_2: one read, then the first field written, then the second */
static void update_seq2(struct frb_device *adev, uint32_t reg, uint32_t m1, uint32_t v1, uint32_t m2, uint32_t v2)
{
	uint32_t v = RREG32(reg);

	v = (v & ~m1) | (v1 & m1);
	WREG32(reg, v);
	v = (v & ~m2) | (v2 & m2);
	WREG32(reg, v);
}

static void submit_channel_request(struct frb_device *adev, const struct frb_connector *c,
				   const struct frb_aux_msg *m)
{
	uint32_t v, length, i;
	int is_write = !(m->action & FRB_AUX_ACTION_READ);

	/* clear_aux_error */
	update_seq2(adev, DCE_REG(mmAUXN_IMPCAL), AUXN_IMPCAL__AUXN_CALOUT_ERROR_AK_MASK, AUXN_IMPCAL__AUXN_CALOUT_ERROR_AK_MASK,
		    AUXN_IMPCAL__AUXN_CALOUT_ERROR_AK_MASK, 0);
	update_seq2(adev, DCE_REG(mmAUXP_IMPCAL), AUXP_IMPCAL__AUXP_CALOUT_ERROR_AK_MASK, AUXP_IMPCAL__AUXP_CALOUT_ERROR_AK_MASK,
		    AUXP_IMPCAL__AUXP_CALOUT_ERROR_AK_MASK, 0);
	/* force_default_calibrate */
	update_seq2(adev, DCE_REG(mmAUXN_IMPCAL), AUXN_IMPCAL__AUXN_IMPCAL_ENABLE_MASK, AUXN_IMPCAL__AUXN_IMPCAL_ENABLE_MASK,
		    AUXN_IMPCAL__AUXN_IMPCAL_OVERRIDE_ENABLE_MASK, 0);
	update_seq2(adev, DCE_REG(mmAUXP_IMPCAL), AUXP_IMPCAL__AUXP_IMPCAL_OVERRIDE_ENABLE_MASK,
		    AUXP_IMPCAL__AUXP_IMPCAL_OVERRIDE_ENABLE_MASK, AUXP_IMPCAL__AUXP_IMPCAL_OVERRIDE_ENABLE_MASK, 0);

	frb_dce12_update(adev, AUX_REG(c, AUX_INTERRUPT_CONTROL), DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK,
			 DP_AUX0_AUX_INTERRUPT_CONTROL__AUX_SW_DONE_ACK_MASK);
	reg_wait(adev, AUX_REG(c, AUX_SW_STATUS), DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK, 0, 10, AUX_POLL_TRIES);

	/* the 4-bit command and 20-bit address (3 bytes), a length byte if any data */
	length = m->size ? 4 : 3;
	if (is_write)
		length += m->size;
	v = REG_SET_FIELD(0, DP_AUX0_AUX_SW_CONTROL, AUX_SW_START_DELAY, 0);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_CONTROL, AUX_SW_WR_BYTES, length);
	frb_dce12_update(adev, AUX_REG(c, AUX_SW_CONTROL),
			 DP_AUX0_AUX_SW_CONTROL__AUX_SW_START_DELAY_MASK | DP_AUX0_AUX_SW_CONTROL__AUX_SW_WR_BYTES_MASK, v);

	/* REG_UPDATE_4 (read once), then REG_SET onto the same value for each byte */
	v = RREG32(AUX_REG(c, AUX_SW_DATA));
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_INDEX, 0);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA_RW, 0);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 1);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, m->action | ((m->address >> 16) & 0xf));
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 0);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, (m->address >> 8) & 0xff);
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, m->address & 0xff);
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	if (m->size) {
		v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, m->size - 1);
		WREG32(AUX_REG(c, AUX_SW_DATA), v);
	}
	if (is_write) {
		for (i = 0; i < m->size; i++) {
			v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA, m->buf[i]);
			WREG32(AUX_REG(c, AUX_SW_DATA), v);
		}
	}
	frb_dce12_update(adev, AUX_REG(c, AUX_SW_CONTROL), DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK,
			 DP_AUX0_AUX_SW_CONTROL__AUX_SW_GO_MASK);
}

/* get_channel_status: FRB_AUX_RET_*, *returned the reply bytes after the command byte */
static int get_channel_status(struct frb_device *adev, const struct frb_connector *c, uint32_t *returned)
{
	uint32_t v;

	*returned = 0;
	reg_wait(adev, AUX_REG(c, AUX_SW_STATUS), DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK,
		 DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK, 10, AUX_POLL_TRIES);
	v = RREG32(AUX_REG(c, AUX_SW_STATUS));
	if (v & DP_AUX0_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK)
		return FRB_AUX_RET_HPD_DISCON;
	if (!(v & DP_AUX0_AUX_SW_STATUS__AUX_SW_DONE_MASK))
		return FRB_AUX_RET_TIMEOUT;
	if (v & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_STATE_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_TIMEOUT_MASK))
		return FRB_AUX_RET_TIMEOUT;
	if (v & (DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_INVALID_STOP_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_NO_DET_MASK |
		 DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_H_MASK | DP_AUX0_AUX_SW_STATUS__AUX_SW_RX_RECV_INVALID_L_MASK))
		return FRB_AUX_RET_INVALID_REPLY;
	*returned = REG_GET_FIELD(v, DP_AUX0_AUX_SW_STATUS, AUX_SW_REPLY_BYTE_COUNT);
	if (!*returned)
		return FRB_AUX_RET_INVALID_REPLY;
	*returned -= 1;
	return FRB_AUX_RET_SUCCESS;
}

/* read_channel_reply: the reply command (in *reply) and, on an ACK, the data */
static int read_channel_reply(struct frb_device *adev, const struct frb_connector *c, uint32_t size,
			      uint8_t *buffer, uint8_t *reply)
{
	uint32_t st, n, v, i;

	st = RREG32(AUX_REG(c, AUX_SW_STATUS));
	n = REG_GET_FIELD(st, DP_AUX0_AUX_SW_STATUS, AUX_SW_REPLY_BYTE_COUNT);
	if (st & DP_AUX0_AUX_SW_STATUS__AUX_SW_HPD_DISCON_MASK)
		return -1;
	if (!n)
		return -1;
	/* REG_UPDATE_SEQ_3: index 0, autoincrement on, read */
	v = RREG32(AUX_REG(c, AUX_SW_DATA));
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_INDEX, 0);
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_AUTOINCREMENT_DISABLE, 1);
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	v = REG_SET_FIELD(v, DP_AUX0_AUX_SW_DATA, AUX_SW_DATA_RW, 1);
	WREG32(AUX_REG(c, AUX_SW_DATA), v);
	v = REG_GET_FIELD(RREG32(AUX_REG(c, AUX_SW_DATA)), DP_AUX0_AUX_SW_DATA, AUX_SW_DATA) >> 4;
	if (reply)
		*reply = (uint8_t)v;
	if (v)
		return 0;
	n--;
	if (n > size)
		return -1;
	for (i = 0; i < n; i++)
		buffer[i] = (uint8_t)REG_GET_FIELD(RREG32(AUX_REG(c, AUX_SW_DATA)), DP_AUX0_AUX_SW_DATA, AUX_SW_DATA);
	return (int)i;
}

/* dce_aux_transfer_raw_with_ddc_pin */
static int aux_transfer_raw(struct frb_device *adev, const struct frb_connector *c, struct frb_aux_msg *m, int *op)
{
	struct ddc_pin data, clk;
	uint32_t returned = 0;
	int res = -1;

	if (!aux_acquire(adev, c, &data, &clk)) {
		*op = FRB_AUX_RET_ENGINE_ACQUIRE;
		return -1;
	}
	submit_channel_request(adev, c, m);
	*op = get_channel_status(adev, c, &returned);
	if (*op == FRB_AUX_RET_SUCCESS) {
		read_channel_reply(adev, c, m->size, m->buf, &m->reply);
		res = (int)returned;
	}
	release_engine(adev, c, &data, &clk);
	return res;
}

/*
 * amdgpu_dm's dm_dp_aux_transfer: DC's result to DRM's (bytes or -errno); a
 * write that the sink acked reports its whole size.
 */
int frb_dce12_aux_transfer(struct frb_device *adev, const struct frb_connector *c, struct frb_aux_msg *m)
{
	int op = FRB_AUX_RET_SUCCESS, r;
	int is_write = !(m->action & FRB_AUX_ACTION_READ);
	uint8_t copy[16];

	if (c->aux < 0 || m->size > 16)
		return -EINVAL;
	m->reply = 0;
	if (is_write)
		memcpy(copy, m->buf, m->size);
	r = aux_transfer_raw(adev, c, m, &op);
	if (is_write && r >= 0) {
		if (r)
			r = m->buf[0];		/* partially written */
		else if (!m->reply)
			r = m->size;		/* I2C_ACK | AUX_ACK */
		memcpy(m->buf, copy, m->size);
	}
	if (r < 0) {
		switch (op) {
		case FRB_AUX_RET_INVALID_REPLY:
		case FRB_AUX_RET_ENGINE_ACQUIRE:
			r = -EBUSY;
			break;
		case FRB_AUX_RET_TIMEOUT:
			r = -ETIMEDOUT;
			break;
		default:
			r = -EIO;
			break;
		}
	}
	m->op = op;
	return r;
}

/* ------------------------------------------------------------------------- */
/* DRM: drm_dp_dpcd_access (32 tries) */

#define DP_AUX_NATIVE_REPLY_MASK	(0x3 << 0)
#define DP_AUX_NATIVE_REPLY_ACK		(0x0 << 0)
#define AUX_RETRY_INTERVAL		500	/* us */

static int dpcd_access(struct frb_device *adev, const struct frb_connector *c, uint8_t action, uint32_t addr,
		       uint8_t *buf, uint32_t size)
{
	struct frb_aux_msg m;
	int retry, ret = 0, err = 0;

	memset(&m, 0, sizeof(m));
	m.action = action;
	m.address = addr;
	m.size = size;
	for (retry = 0; retry < 32; retry++) {
		if (ret != 0 && ret != -ETIMEDOUT)
			udelay(AUX_RETRY_INTERVAL);
		if (action == FRB_AUX_DP_WRITE)
			memcpy(m.buf, buf, size);
		ret = frb_dce12_aux_transfer(adev, c, &m);
		if (ret >= 0) {
			if ((m.reply & DP_AUX_NATIVE_REPLY_MASK) == DP_AUX_NATIVE_REPLY_ACK) {
				if (ret == (int)size) {
					if (action == FRB_AUX_DP_READ)
						memcpy(buf, m.buf, size);
					return ret;
				}
				ret = -EPROTO;
			} else {
				ret = -EIO;
			}
		}
		if (!err)
			err = ret;
	}
	return err;
}

/* drm_dp_dpcd_read / _write (no probe read: amdgpu_dm turns it off) */
int frb_dce12_dpcd_read(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, uint8_t *buf, uint32_t size)
{
	return dpcd_access(adev, c, FRB_AUX_DP_READ, addr, buf, size);
}

int frb_dce12_dpcd_write(struct frb_device *adev, const struct frb_connector *c, uint32_t addr, const uint8_t *buf,
			 uint32_t size)
{
	uint8_t tmp[16];

	if (size > sizeof(tmp))
		return -EINVAL;
	memcpy(tmp, buf, size);
	return dpcd_access(adev, c, FRB_AUX_DP_WRITE, addr, tmp, size);
}

/* ------------------------------------------------------------------------- */
/* DRM: drm_dp_i2c_xfer (I2C over AUX) and drm_edid_read_ddc */

#define DP_AUX_I2C_REPLY_MASK		(0x3 << 2)
#define DP_AUX_I2C_REPLY_ACK		(0x0 << 2)
#define DP_AUX_I2C_REPLY_DEFER		(0x2 << 2)
#define DP_AUX_NATIVE_REPLY_NACK	(0x1 << 0)
#define DP_AUX_NATIVE_REPLY_DEFER	(0x2 << 0)
#define DDC_ADDR			0x50
#define EDID_LENGTH			128

/* drm_dp_i2c_do_msg: 7 tries plus up to 7 more for I2C defers; bytes moved or -errno */
static int i2c_do_msg(struct frb_device *adev, const struct frb_connector *c, struct frb_aux_msg *m)
{
	int retry, defer_i2c = 0, ret;

	for (retry = 0; retry < 7 + defer_i2c; retry++) {
		ret = frb_dce12_aux_transfer(adev, c, m);
		if (ret < 0) {
			if (ret == -EBUSY)
				continue;
			return ret;
		}
		switch (m->reply & DP_AUX_NATIVE_REPLY_MASK) {
		case DP_AUX_NATIVE_REPLY_ACK:
			break;
		case DP_AUX_NATIVE_REPLY_DEFER:
			udelay(AUX_RETRY_INTERVAL);
			continue;
		default:
			return -EIO;
		}
		switch (m->reply & DP_AUX_I2C_REPLY_MASK) {
		case DP_AUX_I2C_REPLY_ACK:
			return ret;
		case DP_AUX_I2C_REPLY_DEFER:
			if (defer_i2c < 7)
				defer_i2c++;
			udelay(AUX_RETRY_INTERVAL);
			continue;
		default:
			return -EIO;
		}
	}
	return -EIO;
}

struct i2c_msg_frb {
	uint8_t addr;
	uint8_t read;
	uint32_t len;
	uint8_t *buf;
};

/*
 * drm_dp_i2c_xfer: per message an address-only start (MOT), the data in
 * 16-byte AUX requests (drm_dp_i2c_drain_msg), and at the end an
 * address-only request without MOT, the stop. Returns 0 when all moved.
 */
static int i2c_xfer(struct frb_device *adev, const struct frb_connector *c, struct i2c_msg_frb *msgs, int num)
{
	struct frb_aux_msg m;
	uint32_t j, transfer_size;
	int i, err = 0;

	memset(&m, 0, sizeof(m));
	for (i = 0; i < num; i++) {
		m.address = msgs[i].addr;
		m.action = (msgs[i].read ? FRB_AUX_I2C_READ : FRB_AUX_I2C_WRITE) | 0x40;	/* MOT */
		m.size = 0;
		err = i2c_do_msg(adev, c, &m);
		if (err < 0)
			break;
		transfer_size = 16;
		for (j = 0; j < msgs[i].len; j += m.size) {
			uint32_t left, n;

			m.size = msgs[i].len - j < transfer_size ? msgs[i].len - j : transfer_size;
			/* drm_dp_i2c_drain_msg */
			for (left = m.size, n = 0; left > 0; ) {
				struct frb_aux_msg d = m;

				d.size = left;
				if (!msgs[i].read)
					memcpy(d.buf, msgs[i].buf + j + n, left);
				err = i2c_do_msg(adev, c, &d);
				if (err <= 0) {
					err = err == 0 ? -EPROTO : err;
					break;
				}
				if (msgs[i].read)
					memcpy(msgs[i].buf + j + n, d.buf, (uint32_t)err);
				left -= (uint32_t)err;
				n += (uint32_t)err;
			}
			if (err < 0)
				break;
			transfer_size = (uint32_t)err;
		}
		if (err < 0)
			break;
	}
	m.action &= (uint8_t)~0x40;
	m.size = 0;
	(void)i2c_do_msg(adev, c, &m);
	return err < 0 ? err : 0;
}

/* drm_do_probe_ddc_edid: offset written to 0x50, then len bytes read; 5 tries */
static int probe_ddc_edid(struct frb_device *adev, const struct frb_connector *c, uint8_t *buf, int block,
			  uint32_t len)
{
	uint8_t start = (uint8_t)(block * EDID_LENGTH), segment = (uint8_t)(block >> 1);
	int retries = 5, r;

	do {
		struct i2c_msg_frb msgs[3] = {
			{ 0x30, 0, 1, &segment },	/* DDC_SEGMENT_ADDR */
			{ DDC_ADDR, 0, 1, &start },
			{ DDC_ADDR, 1, len, buf },
		};

		r = segment ? i2c_xfer(adev, c, msgs, 3) : i2c_xfer(adev, c, msgs + 1, 2);
	} while (r && --retries);
	return r;
}

static int edid_block_ok(const uint8_t *b, int block)
{
	static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	uint8_t sum = 0;
	int i;

	for (i = 0; i < EDID_LENGTH; i++)
		sum += b[i];
	return !sum && (block || !memcmp(b, header, sizeof(header)));
}

/*
 * dm_helpers_read_local_edid over the AUX channel: drm_edid_read_ddc (the
 * one-byte drm_probe_ddc, then _drm_do_get_edid: block 0 with up to four
 * tries, then its extensions), and DP compliance's DP_TEST_REQUEST check.
 */
int frb_dce12_read_edid(struct frb_device *adev, struct frb_connector *c)
{
	uint8_t probe, test_request = 0;
	int block, nblocks, tries, r = -EIO;

	c->edid_len = 0;
	if (!probe_ddc_edid(adev, c, &probe, 0, 1)) {
		for (tries = 0; tries < 4; tries++) {
			r = probe_ddc_edid(adev, c, c->edid, 0, EDID_LENGTH);
			if (!r && edid_block_ok(c->edid, 0))
				break;
			r = -EIO;
		}
		if (!r) {
			nblocks = 1 + c->edid[126];
			if (nblocks > FRB_EDID_MAX / EDID_LENGTH)
				nblocks = FRB_EDID_MAX / EDID_LENGTH;
			for (block = 1; block < nblocks; block++) {
				uint8_t *b = c->edid + block * EDID_LENGTH;

				for (tries = 0; tries < 4; tries++)
					if (!probe_ddc_edid(adev, c, b, block, EDID_LENGTH) && edid_block_ok(b, block))
						break;
				if (tries == 4)
					break;
			}
			c->edid_len = (uint32_t)block * EDID_LENGTH;
		}
	}
	/* DP compliance 4.2.2.3: a test request for the EDID's checksum (none from a monitor) */
	frb_dce12_dpcd_read(adev, c, 0x218, &test_request, 1);	/* DP_TEST_REQUEST */
	if (test_request & 0x04) {				/* DP_TEST_LINK_EDID_READ */
		uint8_t resp = 0x04;				/* DP_TEST_EDID_CHECKSUM_WRITE */

		if (c->edid_len)
			frb_dce12_dpcd_write(adev, c, 0x261, &c->edid[c->edid_len - 1], 1);	/* DP_TEST_EDID_CHECKSUM */
		frb_dce12_dpcd_write(adev, c, 0x260, &resp, 1);	/* DP_TEST_RESPONSE */
	}
	return c->edid_len ? 0 : r;
}
