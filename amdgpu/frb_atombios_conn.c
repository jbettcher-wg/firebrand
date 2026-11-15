/*
 * amdgpu_atombios.c, the display parts: the connector/encoder graph from the
 * VBIOS object table (amdgpu_atombios_get_connector_info_from_object_table),
 * the HPD pin from GPIO_Pin_LUT, the ASIC spread-spectrum table and the
 * scratch-register lock. Read endian-clean: every multi-byte field with
 * frb_get_le16/32 at its offsetof() position.
 *
 * Simplified against Linux: no routers, no external encoders, no TV/CV;
 * one connector per display path.
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

int frb_cmd_rev(struct frb_device *adev, int index, uint8_t *frev, uint8_t *crev)
{
	return frb_atom_parse_cmd_header(adev->atom, index, frev, crev) ? 0 : -ENOENT;
}

/* amdgpu_atom_execute_table on a little-endian parameter block */
int frb_ps_exec(struct frb_device *adev, int index, struct frb_ps *ps)
{
	return frb_atom_execute_table(adev->atom, index, ps->w, sizeof(ps->w));
}

#define U16(o)	frb_get_le16(bios, (o))
#define U8(o)	(bios[(o)])

/* amdgpu_atombios_lookup_gpio + amdgpu_atombios_get_hpd_info_from_gpio */
static int hpd_from_gpio(struct frb_device *adev, uint8_t id)
{
	const uint8_t *bios = adev->bios;
	int index = GetIndexIntoMasterTable(DATA, GPIO_Pin_LUT);
	uint16_t data_offset, size;
	int i, n;

	if (!frb_atom_parse_data_header(adev->atom, index, &size, NULL, NULL, &data_offset))
		return -1;
	n = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) / sizeof(ATOM_GPIO_PIN_ASSIGNMENT);
	for (i = 0; i < n; i++) {
		uint32_t pin = data_offset + offsetof(ATOM_GPIO_PIN_LUT, asGPIO_Pin) +
			       i * sizeof(ATOM_GPIO_PIN_ASSIGNMENT);
		uint16_t reg;
		uint32_t mask;

		if (U8(pin + offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGPIO_ID)) != id)
			continue;
		reg = U16(pin + offsetof(ATOM_GPIO_PIN_ASSIGNMENT, usGpioPin_AIndex));
		mask = 1u << U8(pin + offsetof(ATOM_GPIO_PIN_ASSIGNMENT, ucGpioPinBitShift));
		if (!frb_funcs(adev)->display || reg != frb_funcs(adev)->display->hpd_gpio_reg)	/* dce_vX_0_hpd_get_gpio_reg */
			return -1;
		switch (mask) {
		case (1 << 0):	return 0;
		case (1 << 8):	return 1;
		case (1 << 16):	return 2;
		case (1 << 24):	return 3;
		case (1 << 26):	return 4;
		case (1 << 28):	return 5;
		default:	return -1;
		}
	}
	return -1;
}

/* the records of one object: I2C (the AUX/DDC channel) and HPD */
static void object_records(struct frb_device *adev, uint32_t rec, struct frb_connector *c)
{
	const uint8_t *bios = adev->bios;
	int guard = 64;

	while (guard-- > 0) {
		uint8_t type = U8(rec + offsetof(ATOM_COMMON_RECORD_HEADER, ucRecordType));
		uint8_t sz = U8(rec + offsetof(ATOM_COMMON_RECORD_HEADER, ucRecordSize));

		if (!sz || !type || type > ATOM_MAX_OBJECT_RECORD_NUMBER)
			break;
		if (type == ATOM_I2C_RECORD_TYPE)
			c->i2c_id = U8(rec + offsetof(ATOM_I2C_RECORD, sucI2cId));
		else if (type == ATOM_HPD_INT_RECORD_TYPE)
			c->hpd = (int8_t)hpd_from_gpio(adev, U8(rec + offsetof(ATOM_HPD_INT_RECORD, ucHPDIntGPIOID)));
		rec += sz;
	}
}

int frb_atombios_get_connector_info(struct frb_device *adev, struct frb_display *d)
{
	const uint8_t *bios = adev->bios;
	int index = GetIndexIntoMasterTable(DATA, Object_Header);
	uint16_t size, data_offset, device_support;
	uint8_t frev, crev;
	uint32_t path_tab, con_tab, enc_tab, path;
	int i, j, k, npaths;

	d->num_connectors = 0;
	if (!frb_atom_parse_data_header(adev->atom, index, &size, &frev, &crev, &data_offset) || crev < 2)
		return -ENOENT;
	path_tab = data_offset + U16(data_offset + offsetof(ATOM_OBJECT_HEADER, usDisplayPathTableOffset));
	con_tab = data_offset + U16(data_offset + offsetof(ATOM_OBJECT_HEADER, usConnectorObjectTableOffset));
	enc_tab = data_offset + U16(data_offset + offsetof(ATOM_OBJECT_HEADER, usEncoderObjectTableOffset));
	device_support = U16(data_offset + offsetof(ATOM_OBJECT_HEADER, usDeviceSupport));
	npaths = U8(path_tab + offsetof(ATOM_DISPLAY_OBJECT_PATH_TABLE, ucNumOfDispPath));
	path = path_tab + offsetof(ATOM_DISPLAY_OBJECT_PATH_TABLE, asDispPath);

	for (i = 0; i < npaths && d->num_connectors < FRB_MAX_CONNECTORS; i++) {
		uint16_t tag = U16(path + offsetof(ATOM_DISPLAY_OBJECT_PATH, usDeviceTag));
		uint16_t psize = U16(path + offsetof(ATOM_DISPLAY_OBJECT_PATH, usSize));
		uint16_t conn_id = U16(path + offsetof(ATOM_DISPLAY_OBJECT_PATH, usConnObjectId));
		struct frb_connector *c = &d->conn[d->num_connectors];

		if (psize < 8)
			break;
		if (!(device_support & tag) || tag == ATOM_DEVICE_TV1_SUPPORT || tag == ATOM_DEVICE_CV_SUPPORT) {
			path += psize;
			continue;
		}
		memset(c, 0, sizeof(*c));
		c->connector_object_id = conn_id;
		c->connector_type = (conn_id & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT;
		c->devices = tag;
		c->hpd = -1;
		c->dig_encoder = -1;
		for (j = 0; j < (psize - 8) / 2; j++) {
			uint16_t obj = U16(path + offsetof(ATOM_DISPLAY_OBJECT_PATH, usGraphicObjIds) + 2 * j);

			if (((obj & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT) != GRAPH_OBJECT_TYPE_ENCODER)
				continue;
			for (k = 0; k < U8(enc_tab); k++) {
				uint32_t o = enc_tab + offsetof(ATOM_OBJECT_TABLE, asObjects) + k * sizeof(ATOM_OBJECT);

				if (U16(o + offsetof(ATOM_OBJECT, usObjectID)) == obj && !c->encoder_object_id) {
					c->encoder_object_id = obj;
					c->encoder_id = (obj & OBJECT_ID_MASK) >> OBJECT_ID_SHIFT;
					c->linkb = ((obj & ENUM_ID_MASK) >> ENUM_ID_SHIFT) == 2;
				}
			}
		}
		for (k = 0; k < U8(con_tab); k++) {
			uint32_t o = con_tab + offsetof(ATOM_OBJECT_TABLE, asObjects) + k * sizeof(ATOM_OBJECT);

			if (U16(o + offsetof(ATOM_OBJECT, usObjectID)) == conn_id) {
				object_records(adev, data_offset + U16(o + offsetof(ATOM_OBJECT, usRecordOffset)), c);
				break;
			}
		}
		frb_info(adev, "display: path %d: device 0x%04x, connector 0x%04x (type 0x%02x), encoder 0x%04x "
			 "(id 0x%02x, link %c), i2c/aux 0x%02x, hpd %d", i, tag, conn_id, c->connector_type,
			 c->encoder_object_id, c->encoder_id, c->linkb ? 'B' : 'A', c->i2c_id, c->hpd);
		d->num_connectors++;
		path += psize;
	}
	return d->num_connectors ? 0 : -ENOENT;
}

/* amdgpu_atombios_get_asic_ss_info, the v3 table this VBIOS has (3.1) */
int frb_atombios_get_asic_ss_info(struct frb_device *adev, struct frb_atom_ss *ss, int id, uint32_t clock)
{
	const uint8_t *bios = adev->bios;
	int index = GetIndexIntoMasterTable(DATA, ASIC_InternalSS_Info);
	uint16_t data_offset, size;
	uint8_t frev, crev;
	int i, n;

	memset(ss, 0, sizeof(*ss));
	if (!frb_atom_parse_data_header(adev->atom, index, &size, &frev, &crev, &data_offset))
		return 0;
	if (frev != 3) {
		frb_info(adev, "display: ASIC_InternalSS_Info %u.%u not handled; no spread spectrum", frev, crev);
		return 0;
	}
	n = (size - sizeof(ATOM_COMMON_TABLE_HEADER)) / sizeof(ATOM_ASIC_SS_ASSIGNMENT_V3);
	for (i = 0; i < n; i++) {
		uint32_t a = data_offset + offsetof(ATOM_ASIC_INTERNAL_SS_INFO_V3, asSpreadSpectrum) +
			     i * sizeof(ATOM_ASIC_SS_ASSIGNMENT_V3);

		if (U8(a + offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ucClockIndication)) == id &&
		    clock <= frb_get_le32(bios, a + offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ulTargetClockRange))) {
			ss->percentage = U16(a + offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, usSpreadSpectrumPercentage));
			ss->type = U8(a + offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, ucSpreadSpectrumMode));
			ss->rate = U16(a + offsetof(ATOM_ASIC_SS_ASSIGNMENT_V3, usSpreadRateIn10Hz));
			ss->percentage_divider = (ss->type & SS_MODE_V3_PERCENTAGE_DIV_BY_1000_MASK) ? 1000 : 100;
			return 1;
		}
	}
	return 0;
}

/* amdgpu_atombios_scratch_regs_lock */
void frb_atombios_scratch_regs_lock(struct frb_device *adev, int lock)
{
	uint32_t bios_6_scratch = RREG32(adev->bios_scratch_reg_offset + 6);

	if (lock) {
		bios_6_scratch |= ATOM_S6_CRITICAL_STATE;
		bios_6_scratch &= ~ATOM_S6_ACC_MODE;
	} else {
		bios_6_scratch &= ~ATOM_S6_CRITICAL_STATE;
		bios_6_scratch |= ATOM_S6_ACC_MODE;
	}
	WREG32(adev->bios_scratch_reg_offset + 6, bios_6_scratch);
}
