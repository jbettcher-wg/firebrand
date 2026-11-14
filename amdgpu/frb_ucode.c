/*
 * amdgpu_ucode.c: amdgpu_ucode_validate and the common header, read
 * little-endian byte by byte.
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_ucode.h"

int frb_ucode_parse(struct frb_device *adev, const char *name,
		    const struct frb_fw *fw, struct frb_ucode *u)
{
	memset(u, 0, sizeof(*u));
	if (!fw->data || fw->size < FRB_FWH_COMMON_SIZE) {
		frb_err(adev, "%s: no file, or shorter than a firmware header", name);
		return -EINVAL;
	}
	u->data = fw->data;
	u->file_size = fw->size;
	u->size_bytes = frb_get_le32(fw->data, FRB_FWH_SIZE_BYTES);
	u->header_size_bytes = frb_get_le32(fw->data, FRB_FWH_HEADER_SIZE_BYTES);
	u->header_major = frb_get_le16(fw->data, FRB_FWH_HEADER_VERSION_MAJOR);
	u->header_minor = frb_get_le16(fw->data, FRB_FWH_HEADER_VERSION_MINOR);
	u->ip_major = frb_get_le16(fw->data, FRB_FWH_IP_VERSION_MAJOR);
	u->ip_minor = frb_get_le16(fw->data, FRB_FWH_IP_VERSION_MINOR);
	u->ucode_version = frb_get_le32(fw->data, FRB_FWH_UCODE_VERSION);
	u->ucode_size_bytes = frb_get_le32(fw->data, FRB_FWH_UCODE_SIZE_BYTES);
	u->ucode_array_offset = frb_get_le32(fw->data, FRB_FWH_UCODE_ARRAY_OFFSET);

	/* amdgpu_ucode_validate: the header's own size must be the file's */
	if (u->size_bytes != fw->size) {
		frb_err(adev, "%s: header says %u bytes, the file has %u", name,
			u->size_bytes, fw->size);
		return -EINVAL;
	}
	if (u->ucode_array_offset > fw->size ||
	    u->ucode_size_bytes > fw->size - u->ucode_array_offset ||
	    (u->ucode_size_bytes & 3)) {
		frb_err(adev, "%s: ucode %u bytes at %u does not fit the file", name,
			u->ucode_size_bytes, u->ucode_array_offset);
		return -EINVAL;
	}
	frb_info(adev, "%s: header v%u.%u, ip v%u.%u, ucode version 0x%08x, %u bytes",
		 name, u->header_major, u->header_minor, u->ip_major, u->ip_minor,
		 u->ucode_version, u->ucode_size_bytes);
	return 0;
}
