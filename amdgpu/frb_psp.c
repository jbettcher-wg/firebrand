/*
 * amdgpu_psp.c, the parts Vega 20 (MP0 11.0.2) takes on bare metal, from
 * Linux 7.1.1: psp_hw_init -> amdgpu_ucode_init_bo -> psp_load_fw:
 * psp_ring_init, psp_hw_start (bootloader: system driver, secure OS; the KM
 * ring; psp_tmr_init and SETUP_TMR), psp_load_non_psp_fw (LOAD_IP_FW for
 * every image in AMDGPU_UCODE_ID order, the SMC's among them: Vega 20 has
 * neither RLC autoload nor PMFW-centralised C-state management, so the SMC
 * is the 14th of 17 images), psp_asd_initialize (LOAD_ASD). The capture's
 * 21 ring submissions are those 19 plus the RAS TA's LOAD_TA and INVOKE_CMD
 * (psp_ras_initialize, amdgpu_ras_late_init), which Firebrand leaves out:
 * no RAS yet (docs/vega20-bring-up.md).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - The 1 MB firmware buffer (psp->fw_pri) and the images' buffer
 *   (adev->firmware.fw_buf) are in VRAM, in the CPU window, as amdgpu puts
 *   them under SR-IOV or debug_use_vram_fw_buf; on bare metal amdgpu uses
 *   GTT. So the PSP never reaches host memory here, and the CPU's stores
 *   through BAR0 are followed by an HDP flush before the PSP is told.
 * - Every structure the PSP reads (ring frames, command buffer) is stored
 *   field by field little-endian (frb_psp.h offsets); the fence and the
 *   response are read little-endian.
 *
 * Copyright 2016 Advanced Micro Devices, Inc.
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
#include "frb_soc15.h"
#include "frb_psp.h"
#include "frb_ucode.h"
#include "frb_smu11.h"

/*
 * amdgpu_ucode.h: rlc_firmware_header_v2_1's save/restore list descriptors,
 * after v2_0's 104 bytes (the 32-byte common header and 18 dwords) and
 * reg_list_format_direct_reg_list_length at 104: each list is ucode_ver,
 * feature_ver, size_bytes, offset_bytes. (These were 4 bytes late until the
 * MI50 itself refused the lists, 2026-11-14: PSP status 0xFFFF300F for CNTL
 * and GPM_MEM, a timeout for SRM_MEM. The replay had cut the files with the
 * same wrong offsets, so it agreed.) psp_firmware_header_v1_0's sos desc.
 */
#define FRB_FWH_RLC21_SRL_CNTL_SIZE	116
#define FRB_FWH_RLC21_SRL_CNTL_OFFSET	120
#define FRB_FWH_RLC21_SRL_GPM_SIZE	132
#define FRB_FWH_RLC21_SRL_GPM_OFFSET	136
#define FRB_FWH_RLC21_SRL_SRM_SIZE	148
#define FRB_FWH_RLC21_SRL_SRM_OFFSET	152
#define FRB_FWH_PSP_SOS_FW_VERSION	32
#define FRB_FWH_PSP_SOS_OFFSET		36
#define FRB_FWH_PSP_SOS_SIZE		40

#define ALIGN_PAGE(x)	(((x) + 4095u) & ~4095u)

/* psp_wait_for */
int frb_psp_wait_for(struct frb_device *adev, uint32_t reg_index, uint32_t reg_val, uint32_t mask, uint32_t flags)
{
	uint32_t val = 0, i;

	for (i = 0; i < adev->usec_timeout; i++) {
		val = RREG32(reg_index);
		if (flags & PSP_WAITREG_CHANGED) {
			if (val != reg_val)
				return 0;
		} else if ((val & mask) == reg_val) {
			return 0;
		}
		udelay(1);
	}
	if (!(flags & PSP_WAITREG_NOVERBOSE))
		frb_err(adev, "psp reg (0x%x) wait timed out, mask: %x, read: %x exp: %x", reg_index, mask, val, reg_val);
	return -ETIMEDOUT;
}

/*
 * psp_copy_fw: the 1 MB firmware buffer zeroed, then the image, through
 * BAR0: each 32-bit word of the file read little-endian and stored
 * little-endian, so the bytes land in file order (memcpy on Linux). Then
 * the HDP flush: the buffer is VRAM here, GTT on Linux.
 */
void frb_psp_copy_fw(struct frb_device *adev, const uint8_t *start, uint32_t size)
{
	uint32_t off, base = adev->layout.psp_fw_pri;

	for (off = 0; off < PSP_1_MEG; off += 4)
		frb_vram_write32(adev, base + off, 0);
	for (off = 0; off + 4 <= size; off += 4)
		frb_vram_write32(adev, base + off, frb_get_le32(start, off));
	if (off < size) {
		uint8_t tail[4] = { 0, 0, 0, 0 };

		memcpy(tail, start + off, size - off);
		frb_vram_write32(adev, base + off, frb_get_le32(tail, 0));
	}
	frb_soc15_flush_hdp(adev);
}

/* --- firmware (psp_init_sos/asd_microcode, the IP blocks' init_microcode) ---- */

static int get_fw(struct frb_device *adev, const char *name, struct frb_ucode *u)
{
	struct frb_fw fw;
	int r;

	r = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (r) {
		frb_err(adev, "psp: firmware %s missing (%d)", name, r);
		return r;
	}
	return frb_ucode_parse(adev, name, &fw, u);
}

static int add_image(struct frb_device *adev, const char *name, uint32_t fw_type, const uint8_t *src, uint32_t size)
{
	struct frb_psp *psp = adev->psp;
	struct frb_psp_image *im;

	if (psp->num_images >= FRB_PSP_MAX_IMAGES || !size)
		return -EINVAL;
	im = &psp->img[psp->num_images++];
	im->name = name;
	im->fw_type = fw_type;
	im->src = src;
	im->size = size;
	return 0;
}

/*
 * One of RLC v2.1's save/restore lists (amdgpu_gfx_rlc_init_microcode_v2_1's
 * cut). The list has to lie in the file past the header, so a descriptor read
 * from the wrong place is refused here instead of being handed to the PSP.
 */
static int add_rlc_list(struct frb_device *adev, const struct frb_ucode *rlc, const char *name, uint32_t fw_type,
			uint32_t off_at, uint32_t size_at)
{
	uint32_t off = frb_ucode_le32(rlc, off_at), size = frb_ucode_le32(rlc, size_at);

	if (!size || off < rlc->header_size_bytes || off > rlc->file_size || size > rlc->file_size - off) {
		frb_err(adev, "psp: vega20_rlc.bin: %s at %u, %u bytes, is not inside the %u-byte file",
			name, off, size, rlc->file_size);
		return -EINVAL;
	}
	return add_image(adev, name, fw_type, rlc->data + off, size);
}

/* the default arm of amdgpu_ucode_init_single_fw: ucode_size bytes at ucode_array_offset */
static int add_whole(struct frb_device *adev, const char *file, const char *name, uint32_t fw_type,
		     struct frb_ucode *keep)
{
	struct frb_ucode u;
	int r = get_fw(adev, file, &u);

	if (r)
		return r;
	if (keep)
		*keep = u;
	return add_image(adev, name, fw_type, u.data + u.ucode_array_offset, u.ucode_size_bytes);
}

/*
 * The 17 images, in AMDGPU_UCODE_ID order (SDMA0..1, CP_CE, CP_PFP, CP_ME,
 * CP_MEC1, CP_MEC1_JT, CP_MEC2, CP_MEC2_JT, RLC_RESTORE_LIST_CNTL/GPM/SRM,
 * RLC_G, SMC, UVD, UVD1, VCE), cut as amdgpu_ucode_init_single_fw cuts them
 * for PSP loading: the MEC images without their jump tables, which go as
 * MEC*_JT; the RLC's save/restore lists from its v2.1 header; UVD's image
 * once per instance.
 */
static int psp_collect_images(struct frb_device *adev)
{
	struct frb_ucode mec, mec2, rlc, uvd;
	uint32_t jt_off, jt_size;
	int r;

	if ((r = add_whole(adev, "vega20_sdma.bin", "SDMA0", GFX_FW_TYPE_SDMA0, NULL)) ||
	    (r = add_whole(adev, "vega20_sdma1.bin", "SDMA1", GFX_FW_TYPE_SDMA1, NULL)) ||
	    (r = add_whole(adev, "vega20_ce.bin", "CP_CE", GFX_FW_TYPE_CP_CE, NULL)) ||
	    (r = add_whole(adev, "vega20_pfp.bin", "CP_PFP", GFX_FW_TYPE_CP_PFP, NULL)) ||
	    (r = add_whole(adev, "vega20_me.bin", "CP_ME", GFX_FW_TYPE_CP_ME, NULL)))
		return r;

	if ((r = get_fw(adev, "vega20_mec.bin", &mec)) || (r = get_fw(adev, "vega20_mec2.bin", &mec2)))
		return r;
	jt_off = frb_ucode_le32(&mec, FRB_FWH_GFX_JT_OFFSET);
	jt_size = frb_ucode_le32(&mec, FRB_FWH_GFX_JT_SIZE);
	if ((r = add_image(adev, "CP_MEC1", GFX_FW_TYPE_CP_MEC, mec.data + mec.ucode_array_offset,
			   mec.ucode_size_bytes - jt_size * 4)) ||
	    (r = add_image(adev, "CP_MEC1_JT", GFX_FW_TYPE_CP_MEC_ME1, mec.data + mec.ucode_array_offset + jt_off * 4,
			   jt_size * 4)))
		return r;
	jt_off = frb_ucode_le32(&mec2, FRB_FWH_GFX_JT_OFFSET);
	jt_size = frb_ucode_le32(&mec2, FRB_FWH_GFX_JT_SIZE);
	if ((r = add_image(adev, "CP_MEC2", GFX_FW_TYPE_CP_MEC, mec2.data + mec2.ucode_array_offset,
			   mec2.ucode_size_bytes - jt_size * 4)) ||
	    (r = add_image(adev, "CP_MEC2_JT", GFX_FW_TYPE_CP_MEC_ME2, mec2.data + mec2.ucode_array_offset + jt_off * 4,
			   jt_size * 4)))
		return r;

	if ((r = get_fw(adev, "vega20_rlc.bin", &rlc)))
		return r;
	if (rlc.header_major != 2 || rlc.header_minor < 1) {
		frb_err(adev, "psp: vega20_rlc.bin header %u.%u: the save/restore lists need v2.1",
			rlc.header_major, rlc.header_minor);
		return -EINVAL;
	}
	if ((r = add_rlc_list(adev, &rlc, "RLC_RESTORE_LIST_CNTL", GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_CNTL,
			      FRB_FWH_RLC21_SRL_CNTL_OFFSET, FRB_FWH_RLC21_SRL_CNTL_SIZE)) ||
	    (r = add_rlc_list(adev, &rlc, "RLC_RESTORE_LIST_GPM_MEM", GFX_FW_TYPE_RLC_RESTORE_LIST_GPM_MEM,
			      FRB_FWH_RLC21_SRL_GPM_OFFSET, FRB_FWH_RLC21_SRL_GPM_SIZE)) ||
	    (r = add_rlc_list(adev, &rlc, "RLC_RESTORE_LIST_SRM_MEM", GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_MEM,
			      FRB_FWH_RLC21_SRL_SRM_OFFSET, FRB_FWH_RLC21_SRL_SRM_SIZE)) ||
	    (r = add_image(adev, "RLC_G", GFX_FW_TYPE_RLC_G, rlc.data + rlc.ucode_array_offset, rlc.ucode_size_bytes)))
		return r;

	if ((r = add_whole(adev, "vega20_smc.bin", "SMC", GFX_FW_TYPE_SMU, NULL)) ||
	    (r = add_whole(adev, "vega20_uvd.bin", "UVD", GFX_FW_TYPE_UVD, &uvd)) ||
	    (r = add_image(adev, "UVD1", GFX_FW_TYPE_UVD1, uvd.data + uvd.ucode_array_offset, uvd.ucode_size_bytes)) ||
	    (r = add_whole(adev, "vega20_vce.bin", "VCE", GFX_FW_TYPE_VCE, NULL)))
		return r;
	return 0;
}

/* psp_sw_init's microcode part: psp_init_sos_microcode (v1.0) and psp_init_asd_microcode */
int frb_psp_sw_init(struct frb_device *adev)
{
	struct frb_psp *psp;
	struct frb_ucode sos, asd;
	uint32_t off, total = 0, i;
	int r;

	if (!adev->psp) {
		adev->psp = (struct frb_psp *)adev->os->alloc(adev->os->cookie, sizeof(*adev->psp));
		if (!adev->psp)
			return -ENOMEM;
	}
	psp = adev->psp;
	memset(psp, 0, sizeof(*psp));
	if ((r = get_fw(adev, "vega20_sos.bin", &sos)))
		return r;
	if (sos.header_major != 1) {
		frb_err(adev, "psp: vega20_sos.bin header %u.%u: only 1.x is ported", sos.header_major, sos.header_minor);
		return -EINVAL;
	}
	/* psp_init_sos_microcode v1: both versions are the header's ucode_version; sos.fw_version is the feature */
	psp->sys_version = sos.ucode_version;
	psp->sos_version = sos.ucode_version;
	psp->sys_size = frb_ucode_le32(&sos, FRB_FWH_PSP_SOS_OFFSET);
	psp->sys_start = sos.data + sos.ucode_array_offset;
	psp->sos_size = frb_ucode_le32(&sos, FRB_FWH_PSP_SOS_SIZE);
	psp->sos_start = psp->sys_start + psp->sys_size;
	if (psp->sys_start + psp->sys_size + psp->sos_size > sos.data + sos.file_size) {
		frb_err(adev, "psp: vega20_sos.bin is shorter than its header says");
		return -EINVAL;
	}
	if ((r = get_fw(adev, "vega20_asd.bin", &asd)))
		return r;
	psp->asd_version = asd.ucode_version;
	psp->asd_size = asd.ucode_size_bytes;
	psp->asd_start = asd.data + asd.ucode_array_offset;
	if ((r = psp_collect_images(adev)))
		return r;

	/* amdgpu_ucode_init_bo: each image at the next page in the buffer */
	for (i = 0, off = 0; i < psp->num_images; i++) {
		psp->img[i].buf_off = off;
		off += ALIGN_PAGE(psp->img[i].size);
		total += psp->img[i].size;
	}
	if (off > adev->layout.fw_buf_size || psp->sys_size > PSP_1_MEG || psp->sos_size > PSP_1_MEG ||
	    psp->asd_size > PSP_1_MEG) {
		frb_err(adev, "psp: the images need %u KB, the buffer has %u KB", off >> 10, adev->layout.fw_buf_size >> 10);
		return -ERANGE;
	}
	psp->fw_pri_mc = adev->gmc.vram_start + adev->layout.psp_fw_pri;
	psp->ring_mc = adev->gmc.vram_start + adev->layout.psp_ring;
	psp->cmd_mc = adev->gmc.vram_start + adev->layout.psp_cmd;
	psp->fence_mc = adev->gmc.vram_start + adev->layout.psp_fence;
	psp->tmr_mc = adev->gmc.vram_start + adev->layout.psp_tmr;
	psp->fw_buf_mc = adev->gmc.vram_start + adev->layout.fw_buf;
	psp->timeout = 20000;		/* adev->psp_timeout */
	frb_info(adev, "psp: sys_drv 0x%08x (%u bytes), sOS 0x%08x (%u bytes), ASD 0x%08x (%u bytes); "
		 "%u images, %u KB, for LOAD_IP_FW", psp->sys_version, psp->sys_size, psp->sos_version, psp->sos_size,
		 psp->asd_version, psp->asd_size, psp->num_images, total >> 10);
	return 0;
}

/* --- submission (psp_ring_cmd_submit, psp_cmd_submit_buf) ------------------ */

/* psp_ring_cmd_submit: one 64-byte frame at the write pointer, LE, then the HDP flush and the new pointer */
static int psp_ring_cmd_submit(struct frb_device *adev, uint64_t cmd_buf_mc_addr, uint64_t fence_mc_addr, uint32_t index)
{
	uint32_t ring_size_dw = PSP_RING_SIZE / 4, rb_frame_size_dw = PSP_FRAME_SIZE / 4;
	uint32_t wptr, frame, off, i;

	/* KM (GPCOM) prepare write pointer */
	wptr = frb_psp_v11_0_ring_get_wptr(adev);
	if ((wptr % ring_size_dw) == 0)
		frame = 0;
	else
		frame = wptr / rb_frame_size_dw;
	if (frame >= PSP_RING_SIZE / PSP_FRAME_SIZE) {
		frb_err(adev, "psp: write pointer %u is outside the ring", wptr);
		return -EINVAL;
	}
	off = adev->layout.psp_ring + frame * PSP_FRAME_SIZE;

	/* Initialize KM RB frame, then the fields */
	for (i = 0; i < PSP_FRAME_SIZE; i += 4)
		frb_vram_write32(adev, off + i, 0);
	frb_vram_write32(adev, off + PSP_FRAME_CMD_BUF_ADDR_HI, upper_32_bits(cmd_buf_mc_addr));
	frb_vram_write32(adev, off + PSP_FRAME_CMD_BUF_ADDR_LO, lower_32_bits(cmd_buf_mc_addr));
	frb_vram_write32(adev, off + PSP_FRAME_FENCE_ADDR_HI, upper_32_bits(fence_mc_addr));
	frb_vram_write32(adev, off + PSP_FRAME_FENCE_ADDR_LO, lower_32_bits(fence_mc_addr));
	frb_vram_write32(adev, off + PSP_FRAME_FENCE_VALUE, index);
	frb_soc15_flush_hdp(adev);

	/* Update the write Pointer in DWORDs */
	frb_psp_v11_0_ring_set_wptr(adev, (wptr + rb_frame_size_dw) % ring_size_dw);
	return 0;
}

/*
 * A command as psp_cmd_submit_buf sends it: the command buffer zeroed and
 * filled (the caller's words at their byte offsets, little-endian), the
 * frame, then the fence polled with an HDP read-cache invalidate before
 * each look. `words` is a list of (offset, value) pairs.
 */
struct psp_word {
	uint32_t off, val;
};

static int psp_cmd_submit_buf(struct frb_device *adev, const char *what, uint32_t cmd_id,
			      const struct psp_word *words, uint32_t nwords, struct frb_psp_image *ucode)
{
	struct frb_psp *psp = adev->psp;
	uint32_t i, index, timeout = psp->timeout, status, base = adev->layout.psp_cmd;
	int ret;

	for (i = 0; i < PSP_CMD_BUFFER_SIZE; i += 4)
		frb_vram_write32(adev, base + i, 0);
	frb_vram_write32(adev, base + PSP_CMD_ID, cmd_id);
	for (i = 0; i < nwords; i++)
		frb_vram_write32(adev, base + words[i].off, words[i].val);

	index = ++psp->fence_value;
	ret = psp_ring_cmd_submit(adev, psp->cmd_mc, psp->fence_mc, index);
	if (ret) {
		psp->fence_value--;
		return ret;
	}

	frb_soc15_invalidate_hdp(adev);
	while (frb_vram_read32(adev, adev->layout.psp_fence) != index) {
		if (--timeout == 0)
			break;
		udelay(80);	/* usleep_range(60, 100) */
		frb_soc15_invalidate_hdp(adev);
	}

	status = frb_vram_read32(adev, base + PSP_RESP_STATUS);
	if (status || !timeout) {
		if (ucode)
			frb_err(adev, "failed to load ucode %s", ucode->name);
		frb_err(adev, "psp gfx command %s(0x%X) failed and response status is (0x%X)%s", what, cmd_id, status,
			timeout ? "" : ", timed out");
		/* amdgpu fails only on a timeout here (or SR-IOV), and warns otherwise */
		if (!timeout)
			return -EINVAL;
	}
	if (ucode) {
		ucode->tmr_lo = frb_vram_read32(adev, base + PSP_RESP_FW_ADDR_LO);
		ucode->tmr_hi = frb_vram_read32(adev, base + PSP_RESP_FW_ADDR_HI);
	}
	return 0;
}

/* --- the commands ---------------------------------------------------------- */

/* psp_tmr_load: SETUP_TMR with the TMR's MC address, size and physical (VRAM) address */
static int psp_tmr_load(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	uint64_t tmr_pa = frb_gmc_v9_0_vram_mc2pa(adev, psp->tmr_mc);
	struct psp_word w[] = {
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_ADDR_LO, lower_32_bits(psp->tmr_mc) },
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_ADDR_HI, upper_32_bits(psp->tmr_mc) },
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_SIZE, adev->layout.psp_tmr_size },
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_FLAGS, PSP_TMR_FLAG_VIRT_PHY_ADDR },
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_SYS_ADDR_LO, lower_32_bits(tmr_pa) },
		{ PSP_CMD_PAYLOAD + PSP_SETUP_TMR_SYS_ADDR_HI, upper_32_bits(tmr_pa) },
	};
	int r;

	frb_info(adev, "reserve 0x%x from 0x%llx for PSP TMR", adev->layout.psp_tmr_size,
		 (unsigned long long)psp->tmr_mc);
	r = psp_cmd_submit_buf(adev, "SETUP_TMR", GFX_CMD_ID_SETUP_TMR, w, ARRAY_SIZE(w), NULL);
	if (!r)
		psp->tmr_loaded = 1;
	return r;
}

/* psp_execute_ip_fw_load */
static int psp_execute_ip_fw_load(struct frb_device *adev, struct frb_psp_image *ucode)
{
	uint64_t mc = adev->psp->fw_buf_mc + ucode->buf_off;
	struct psp_word w[] = {
		{ PSP_CMD_PAYLOAD + PSP_LOAD_IP_FW_ADDR_LO, lower_32_bits(mc) },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_IP_FW_ADDR_HI, upper_32_bits(mc) },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_IP_FW_SIZE, ucode->size },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_IP_FW_TYPE, ucode->fw_type },
	};

	return psp_cmd_submit_buf(adev, "LOAD_IP_FW", GFX_CMD_ID_LOAD_IP_FW, w, ARRAY_SIZE(w), ucode);
}

/* psp_load_non_psp_fw for MP0 11.0.2: every image, the SMC's in its place */
static int psp_load_non_psp_fw(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	uint32_t i;
	int r;

	for (i = 0; i < psp->num_images; i++) {
		r = psp_execute_ip_fw_load(adev, &psp->img[i]);
		if (r) {
			if (psp->img[i].fw_type == GFX_FW_TYPE_SMU)
				frb_err(adev, "PSP load smu failed!");
			return r;
		}
		psp->loaded++;
		frb_dbg(adev, "psp: %s (%u bytes) -> TMR 0x%08x%08x", psp->img[i].name, psp->img[i].size,
			psp->img[i].tmr_hi, psp->img[i].tmr_lo);
	}
	return 0;
}

/* psp_asd_initialize -> psp_ta_load(LOAD_ASD): the image through the 1 MB buffer, no shared memory */
static int psp_asd_initialize(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	struct psp_word w[] = {
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_APP_ADDR_LO, lower_32_bits(psp->fw_pri_mc) },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_APP_ADDR_HI, upper_32_bits(psp->fw_pri_mc) },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_APP_LEN, psp->asd_size },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_CMD_BUF_ADDR_LO, 0 },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_CMD_BUF_ADDR_HI, 0 },
		{ PSP_CMD_PAYLOAD + PSP_LOAD_TA_CMD_BUF_LEN, PSP_ASD_SHARED_MEM_SIZE },
	};
	int r;

	if (!psp->asd_size)
		return 0;
	frb_psp_copy_fw(adev, psp->asd_start, psp->asd_size);
	r = psp_cmd_submit_buf(adev, "LOAD_ASD", GFX_CMD_ID_LOAD_ASD, w, ARRAY_SIZE(w), NULL);
	if (!r) {
		psp->asd_session_id = frb_vram_read32(adev, adev->layout.psp_cmd + PSP_RESP_SESSION_ID);
		psp->asd_initialized = 1;
	}
	return r;
}

/* --- psp_hw_init ----------------------------------------------------------- */

/* amdgpu_ucode_init_bo's copies: every image into the buffer, LE words, then the HDP flush */
static void psp_ucode_init_bo(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	uint32_t i, off, base;

	for (i = 0; i < psp->num_images; i++) {
		const struct frb_psp_image *im = &psp->img[i];

		base = adev->layout.fw_buf + im->buf_off;
		for (off = 0; off + 4 <= im->size; off += 4)
			frb_vram_write32(adev, base + off, frb_get_le32(im->src, off));
		if (off < im->size) {
			uint8_t tail[4] = { 0, 0, 0, 0 };

			memcpy(tail, im->src + off, im->size - off);
			frb_vram_write32(adev, base + off, frb_get_le32(tail, 0));
			off += 4;
		}
		for (; off < ALIGN_PAGE(im->size); off += 4)
			frb_vram_write32(adev, base + off, 0);
	}
	frb_soc15_flush_hdp(adev);
}

/* psp_hw_start for 11.0.2 on bare metal */
static int psp_hw_start(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	int r;

	r = frb_psp_v11_0_bootloader_load_sysdrv(adev);
	if (r) {
		frb_err(adev, "PSP load sys drv failed!");
		return r;
	}
	r = frb_psp_v11_0_bootloader_load_sos(adev);
	if (r) {
		frb_err(adev, "PSP load sos failed!");
		return r;
	}
	r = frb_psp_v11_0_ring_create(adev);
	if (r) {
		frb_err(adev, "PSP create ring failed!");
		return r;
	}
	psp->ring_created = 1;
	/* psp_update_fw_reservation: nothing for 11.0.2; psp_tmr_init: Firebrand's placement (layout.psp_tmr) */
	r = psp_tmr_load(adev);
	if (r) {
		frb_err(adev, "PSP load tmr failed!");
		return r;
	}
	return 0;
}

/* psp_ring_destroy (psp_v11_0_ring_destroy) */
static void psp_ring_destroy(struct frb_device *adev)
{
	if (adev->psp->ring_created && frb_psp_v11_0_ring_stop(adev))
		frb_err(adev, "Fail to stop psp ring");
	adev->psp->ring_created = 0;
}

/* psp_hw_init: amdgpu_ucode_init_bo, then psp_load_fw */
int frb_psp_hw_init(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	uint32_t off;
	int r;

	if (!psp)
		return -EINVAL;
	psp_ucode_init_bo(adev);
	/* psp_load_fw: the fence buffer zeroed, psp_ring_init (the ring zeroed, wptr 0) */
	for (off = 0; off < PSP_FENCE_BUFFER_SIZE; off += 4)
		frb_vram_write32(adev, adev->layout.psp_fence + off, 0);
	for (off = 0; off < PSP_RING_SIZE; off += 4)
		frb_vram_write32(adev, adev->layout.psp_ring + off, 0);
	frb_soc15_flush_hdp(adev);
	psp->fence_value = 0;

	r = psp_hw_start(adev);
	if (r)
		goto failed;
	r = psp_load_non_psp_fw(adev);
	if (r)
		goto failed;
	r = psp_asd_initialize(adev);
	if (r) {
		frb_err(adev, "PSP load asd failed!");
		goto failed;
	}
	frb_info(adev, "psp: sOS 0x%08x up, TMR %u MB at 0x%llx, %u images loaded (the SMC's among them), "
		 "ASD session %u", psp->sos_version, adev->layout.psp_tmr_size >> 20, (unsigned long long)psp->tmr_mc,
		 psp->loaded, psp->asd_session_id);
	return 0;
failed:
	frb_err(adev, "PSP firmware loading failed");
	psp_ring_destroy(adev);
	return r ? r : -EINVAL;
}

/* psp_hw_fini, bare metal and no TAs: ASD unloaded, the TMR destroyed, the ring destroyed */
void frb_psp_hw_fini(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;

	if (!psp || !psp->ring_created)
		return;
	if (psp->asd_initialized) {
		struct psp_word w[] = { { PSP_CMD_PAYLOAD + PSP_UNLOAD_TA_SESSION_ID, psp->asd_session_id } };

		if (!psp_cmd_submit_buf(adev, "UNLOAD_TA", GFX_CMD_ID_UNLOAD_TA, w, ARRAY_SIZE(w), NULL))
			psp->asd_initialized = 0;
	}
	if (psp->tmr_loaded && !psp_cmd_submit_buf(adev, "DESTROY_TMR", GFX_CMD_ID_DESTROY_TMR, NULL, 0, NULL))
		psp->tmr_loaded = 0;
	psp_ring_destroy(adev);
}

/*
 * Stage smu on SOC15, in amdgpu_device_fw_loading's order: psp_hw_init
 * (every image, the SMC's included), then amdgpu_pm_load_smu_firmware
 * (vega20_start_smu: is the SMC running, the tools table address), then
 * powerplay's hw_init (vega20's DPM tasks) unless FirebrandDPM is off.
 * FirebrandPSPOnly stops after the PSP, for the first run on a card.
 */
int frb_soc15_stage_smu(struct frb_device *adev)
{
	int r;

	r = frb_psp_sw_init(adev);
	if (r)
		return r;
	r = frb_psp_hw_init(adev);
	if (r)
		return r;
	if (adev->psp_only) {
		frb_info(adev, "stage smu: the PSP only (FirebrandPSPOnly); the SMC was loaded, the SMU is not started");
		return 0;
	}
	return frb_smu11_hw_init(adev);
}
