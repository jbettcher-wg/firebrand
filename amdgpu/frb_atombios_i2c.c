/*
 * DDC for HDMI and DVI: atombios_i2c.c (Linux 6.16), I2C transfers through
 * the VBIOS's ProcessI2cChannelTransaction on the connector's I2C line, and
 * the EDID read drm_get_edid makes over it (offset 0 written to 0x50, then
 * 128 bytes read; the first extension block the same way at offset 128).
 *
 * WHERE FIREBRAND DIFFERS. amdgpu takes this path for its DDC buses only
 * with amdgpu.hw_i2c=1 and bit-bangs the GPIO lines otherwise (i2c-algo-bit
 * over DC_GPIO_DDCx); the VBIOS table does the same transfer and keeps the
 * GPIO timing out of the kext. Both RX 550 and E9173 VBIOSes carry
 * ProcessI2cChannelTransaction 1.1. Only EDID blocks 0 and 1 are read (no
 * E-DDC segment pointer), as frb_dp_read_edid does over AUX.
 *
 * BYTE ORDER. The table returns read data in the ATOM scratch area, whose
 * dwords the interpreter holds as numbers: the bytes are each dword's
 * little-endian representation (amdgpu_atombios_copy_swap(..., false)).
 *
 * Copyright 2011 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include "frb_display.h"
#include "../shared/atom/atombios.h"

#define OFS(t, f)	((uint32_t)offsetof(t, f))

#define TARGET_HW_I2C_CLOCK	50
/* these are a limitation of ProcessI2cChannelTransaction not the hw */
#define ATOM_MAX_HW_I2C_WRITE	3
#define ATOM_MAX_HW_I2C_READ	255

#define DDC_ADDR		0x50
#define EDID_BLOCK		128

/* amdgpu_atombios_i2c_process_i2c_ch */
static int process_i2c_ch(struct frb_device *adev, struct frb_connector *conn, uint8_t slave_addr,
			  uint8_t flags, uint8_t *buf, uint8_t num)
{
	int index = GetIndexIntoMasterTable(COMMAND, ProcessI2cChannelTransaction);
	struct frb_ps ps;
	uint8_t out[2] = { 0, 0 };
	uint32_t i;

	frb_ps_init(&ps);
	if (flags & HW_I2C_WRITE) {
		if (num > ATOM_MAX_HW_I2C_WRITE) {
			frb_err(adev, "hw i2c: tried to write too many bytes (%d vs 3)", num);
			return -EINVAL;
		}
		frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucRegIndex), buf ? buf[0] : 0);
		if (num)
			num--;
		if (num) {
			if (!buf) {
				frb_err(adev, "hw i2c: missing buf with num > 1");
				return -EINVAL;
			}
			memcpy(out, &buf[1], num);
		}
		frb_ps_le16(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, lpI2CDataOut), frb_get_le16(out, 0));
	} else {
		if (!adev->atom->scratch || adev->atom->scratch_size_bytes < num)
			return -EIO;
	}
	frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucFlag), flags);
	frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucI2CSpeed), TARGET_HW_I2C_CLOCK);
	frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucTransBytes), num);
	frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucSlaveAddr), slave_addr << 1);
	frb_ps_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucLineNumber), conn->i2c_id);
	frb_ps_exec(adev, index, &ps);

	/* error */
	if (frb_ps_get_u8(&ps, OFS(PROCESS_I2C_CHANNEL_TRANSACTION_PARAMETERS, ucStatus)) !=
	    HW_ASSISTED_I2C_STATUS_SUCCESS)
		return -EIO;

	if (!(flags & HW_I2C_WRITE)) {
		for (i = 0; i < num; i++)
			buf[i] = (uint8_t)(adev->atom->scratch[i / 4] >> (8 * (i % 4)));
	}
	return 0;
}

/* amdgpu_display_ddc_probe: does anything answer at the DDC address (a one-byte offset write) */
int frb_ddc_probe(struct frb_device *adev, struct frb_connector *conn)
{
	uint8_t offset = 0;

	if (!conn->i2c_id)
		return 0;
	return process_i2c_ch(adev, conn, DDC_ADDR, HW_I2C_WRITE, &offset, 1) == 0;
}

/* drm_get_edid over amdgpu_atombios_i2c_xfer: blocks 0 and 1 */
int frb_ddc_read_edid(struct frb_device *adev, struct frb_connector *conn)
{
	int block, nblocks = 1, off;
	uint8_t sum;

	conn->edid_len = 0;
	for (block = 0; block < nblocks && block < 2; block++) {
		uint8_t offset = (uint8_t)(block * EDID_BLOCK);

		if (process_i2c_ch(adev, conn, DDC_ADDR, HW_I2C_WRITE, &offset, 1))
			return -EIO;
		if (process_i2c_ch(adev, conn, DDC_ADDR, HW_I2C_READ, conn->edid + block * EDID_BLOCK, EDID_BLOCK))
			return -EIO;
		for (sum = 0, off = 0; off < EDID_BLOCK; off++)
			sum += conn->edid[block * EDID_BLOCK + off];
		if (sum) {
			frb_err(adev, "display: DDC EDID block %d checksum %02x", block, sum);
			return -EIO;
		}
		if (block == 0) {
			static const uint8_t header[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };

			if (memcmp(conn->edid, header, 8)) {
				frb_err(adev, "display: DDC EDID without its header");
				return -EIO;
			}
			nblocks = 1 + conn->edid[126];
			if (nblocks > 2)
				frb_info(adev, "display: EDID has %d extensions; reading the first", nblocks - 1);
		}
		conn->edid_len = (block + 1) * EDID_BLOCK;
	}
	return 0;
}
