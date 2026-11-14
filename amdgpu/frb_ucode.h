/*
 * amdgpu_ucode.h/.c: the firmware file headers, read little-endian field by
 * field (the files are little-endian; amdgpu reads them with le32_to_cpu).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_UCODE_H
#define FRB_UCODE_H

#include "frb_device.h"

/* struct common_firmware_header, byte offsets */
#define FRB_FWH_SIZE_BYTES		0
#define FRB_FWH_HEADER_SIZE_BYTES	4
#define FRB_FWH_HEADER_VERSION_MAJOR	8	/* u16 */
#define FRB_FWH_HEADER_VERSION_MINOR	10	/* u16 */
#define FRB_FWH_IP_VERSION_MAJOR	12	/* u16 */
#define FRB_FWH_IP_VERSION_MINOR	14	/* u16 */
#define FRB_FWH_UCODE_VERSION		16
#define FRB_FWH_UCODE_SIZE_BYTES	20
#define FRB_FWH_UCODE_ARRAY_OFFSET	24
#define FRB_FWH_CRC32			28
#define FRB_FWH_COMMON_SIZE		32
/* struct mc_firmware_header_v1_0 */
#define FRB_FWH_MC_IO_DEBUG_SIZE	32
#define FRB_FWH_MC_IO_DEBUG_OFFSET	36
/* struct smc_firmware_header_v1_0 */
#define FRB_FWH_SMC_UCODE_START_ADDR	32
/* struct gfx_firmware_header_v1_0 */
#define FRB_FWH_GFX_UCODE_FEATURE_VERSION 32
#define FRB_FWH_GFX_JT_OFFSET		36
#define FRB_FWH_GFX_JT_SIZE		40
/* struct rlc_firmware_header_v2_0 (save_and_restore_offset etc. follow) */
#define FRB_FWH_RLC_UCODE_FEATURE_VERSION 32
/* struct sdma_firmware_header_v1_0 */
#define FRB_FWH_SDMA_UCODE_FEATURE_VERSION 32
#define FRB_FWH_SDMA_UCODE_CHANGE_VERSION 36
#define FRB_FWH_SDMA_JT_OFFSET		40
#define FRB_FWH_SDMA_JT_SIZE		44

struct frb_ucode {
	uint32_t size_bytes, header_size_bytes;
	uint16_t header_major, header_minor, ip_major, ip_minor;
	uint32_t ucode_version, ucode_size_bytes, ucode_array_offset;
	const uint8_t *data;
	uint32_t file_size;
};

/* amdgpu_ucode_validate + header read; -EINVAL when the file is not whole */
int frb_ucode_parse(struct frb_device *adev, const char *name,
		    const struct frb_fw *fw, struct frb_ucode *u);

static inline uint32_t frb_ucode_le32(const struct frb_ucode *u, uint32_t off)
{
	return frb_get_le32(u->data, off);
}

#endif
