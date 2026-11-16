/*
 * The SMU7 start for Polaris: pm/powerplay/smumgr/smu7_smumgr.c and
 * polaris10_smumgr.c (Linux 7.0.2), the parts powerplay's hw_init runs before
 * any DPM table: smu7_init, polaris10_start_smu (protected or not), the AVFS
 * event manager's table copy, the soft-register lookup, and
 * smu7_request_smu_load_fw with its table of contents and amdgpu_ucode's
 * firmware buffer. It ends where the E9173 capture's LoadUcodes completes;
 * the next message there (DisableFFC) is DPM.
 *
 * BYTE ORDER, the three places it matters (docs/byte-order.md):
 * - The SMC image goes in as little-endian words: amdgpu writes `*src++` of
 *   the file on an LE host, i.e. le32 of each 4 bytes (smu7_upload_smc_firmware_data).
 * - smu7_copy_bytes_to_smc puts bytes into the SMC "MSB first". amdgpu hands
 *   it the in-memory bytes of LE-host structures, so every structure copied
 *   that way is serialized here as the LE host would hold it (the AVFS
 *   graphics/memory levels, VRConfig), field by field; BootMVdd is
 *   PP_HOST_TO_SMC_US, i.e. its bytes are big-endian.
 * - The TOC (SMU_DRAMData_TOC) is read by the SMU from VRAM in the layout of
 *   smu_ucode_xfer_vi.h's little-endian branch; it is built byte by byte.
 *
 * PLACEMENT: amdgpu keeps the microcode images in GTT (system memory through
 * the GART); Firebrand keeps them in VRAM (amdgpu's SR-IOV/debug_use_vram_fw_buf
 * placement), so the SMU fetches nothing from host memory, and bus mastering
 * can stay off. The header and SMU buffers are amdgpu's VRAM objects, at the
 * top of the CPU window (frb_gmc_v8_0.c, placement).
 *
 * Copyright 2015 Advanced Micro Devices, Inc.
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
#include "frb_device.h"
#include "frb_ucode.h"
#include "include/asic_reg/smu/smu_7_1_3_d.h"
#include "include/asic_reg/smu/smu_7_1_3_sh_mask.h"

/* smu7_ppsmc.h */
#define PPSMC_MSG_Test			((uint16_t)0x100)
#define PPSMC_MSG_DRV_DRAM_ADDR_HI	((uint16_t)0x250)
#define PPSMC_MSG_DRV_DRAM_ADDR_LO	((uint16_t)0x251)
#define PPSMC_MSG_SMU_DRAM_ADDR_HI	((uint16_t)0x252)
#define PPSMC_MSG_SMU_DRAM_ADDR_LO	((uint16_t)0x253)
#define PPSMC_MSG_LoadUcodes		((uint16_t)0x254)

/* smu_ucode_xfer_vi.h */
#define UCODE_ID_SMU		0
#define UCODE_ID_SDMA0		1
#define UCODE_ID_SDMA1		2
#define UCODE_ID_CP_CE		3
#define UCODE_ID_CP_PFP		4
#define UCODE_ID_CP_ME		5
#define UCODE_ID_CP_MEC		6
#define UCODE_ID_CP_MEC_JT1	7
#define UCODE_ID_CP_MEC_JT2	8
#define UCODE_ID_RLC_G		10
#define UCODE_ID_SDMA0_MASK	0x00000002
#define UCODE_ID_SDMA1_MASK	0x00000004
#define UCODE_ID_CP_CE_MASK	0x00000008
#define UCODE_ID_CP_PFP_MASK	0x00000010
#define UCODE_ID_CP_ME_MASK	0x00000020
#define UCODE_ID_CP_MEC_MASK	0x00000040
#define UCODE_ID_RLC_G_MASK	0x00000400
#define SMU_MAX_ENTRIES		12
#define SMU_TOC_ENTRY_BYTES	28	/* struct SMU_Entry */
#define SMU_TOC_BYTES		(8 + SMU_MAX_ENTRIES * SMU_TOC_ENTRY_BYTES)

/* smu7.h / smu74.h / smu74_discrete.h (offsets checked with offsetof on the Linux headers) */
#define SMU7_FIRMWARE_HEADER_LOCATION	0x20000
#define SMU7_SMC_SIZE			0x20000
#define SMC_RAM_END			0x40000
#define FWH_SOFT_REGISTERS		0x30	/* SMU74_Firmware_Header.SoftRegisters */
#define FWH_DPM_TABLE			0x34	/* SMU74_Firmware_Header.DpmTable */
#define DPM_VRCONFIG			0x70	/* SMU74_Discrete_DpmTable.VRConfig */
#define DPM_GRAPHICS_LEVEL		0xec	/* .GraphicsLevel[8], 0x38 each */
#define DPM_MEMORY_LEVEL		0x2c4	/* .MemoryLevel[], 0x18 each */
#define DPM_BOOT_MVDD			0x638	/* .BootMVdd */
#define SOFT_UCODE_LOAD_STATUS		0x6c	/* SMU74_SoftRegisters.UcodeLoadStatus */
#define GRAPHICS_LEVEL_BYTES		0x38
#define MEMORY_LEVEL_BYTES		0x18
#define VOLTAGE_SCALE			4

struct smu7_state {
	int protected_mode, security_hard_key, avfs_supported;
	uint32_t soft_regs_start;
	uint64_t header_mc, smu_buffer_mc, fw_buf_mc;
};

/* --- the register-level helpers (smu7_smumgr.c, hwmgr.c phm_wait_*) -------- */

/* phm_wait_on_indirect_register: index once, then poll the data port */
static int phm_wait_on_indirect_register(struct frb_device *adev, uint32_t index,
					 uint32_t value, uint32_t mask)
{
	uint32_t i, cur;

	WREG32(mmSMC_IND_INDEX_11, index);
	for (i = 0; i < adev->usec_timeout; i++) {
		cur = RREG32(mmSMC_IND_DATA_11);
		if ((cur & mask) == (value & mask))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* phm_wait_for_indirect_register_unequal */
static int phm_wait_for_indirect_register_unequal(struct frb_device *adev, uint32_t index,
						  uint32_t value, uint32_t mask)
{
	uint32_t i, cur;

	WREG32(mmSMC_IND_INDEX_11, index);
	for (i = 0; i < adev->usec_timeout; i++) {
		cur = RREG32(mmSMC_IND_DATA_11);
		if ((cur & mask) != (value & mask))
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* phm_wait_for_register_unequal on SMC_RESP_0.SMC_RESP */
static int wait_resp(struct frb_device *adev)
{
	uint32_t i;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP) != 0)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

/* PHM_WRITE_VFPF_INDIRECT_FIELD on the SMC space: read, change one field, write */
static void smc_write_field(struct frb_device *adev, uint32_t reg, uint32_t mask,
			    uint32_t shift, uint32_t val)
{
	uint32_t tmp = RREG32_SMC(reg);

	tmp = (tmp & ~mask) | ((val << shift) & mask);
	WREG32_SMC(reg, tmp);
}

static int smu7_set_smc_sram_address(struct frb_device *adev, uint32_t smc_addr, uint32_t limit)
{
	uint32_t tmp;

	if ((3 & smc_addr) || !(limit > (smc_addr + 3)))
		return -EINVAL;
	WREG32(mmSMC_IND_INDEX_11, smc_addr);
	tmp = RREG32(mmSMC_IND_ACCESS_CNTL);
	tmp = REG_SET_FIELD(tmp, SMC_IND_ACCESS_CNTL, AUTO_INCREMENT_IND_11, 0);
	WREG32(mmSMC_IND_ACCESS_CNTL, tmp);
	return 0;
}

/* smu7_copy_bytes_to_smc: "Bytes are written into the SMC address space with the MSB first." */
static int smu7_copy_bytes_to_smc(struct frb_device *adev, uint32_t smc_start_address,
				  const uint8_t *src, uint32_t byte_count, uint32_t limit)
{
	uint32_t data, original_data, addr, extra_shift;
	int result;

	if ((3 & smc_start_address) || !(limit > (smc_start_address + byte_count)))
		return -EINVAL;
	addr = smc_start_address;
	while (byte_count >= 4) {
		data = src[0] * 0x1000000 + src[1] * 0x10000 + src[2] * 0x100 + src[3];
		result = smu7_set_smc_sram_address(adev, addr, limit);
		if (result)
			return result;
		WREG32(mmSMC_IND_DATA_11, data);
		src += 4;
		byte_count -= 4;
		addr += 4;
	}
	if (byte_count) {
		data = 0;
		result = smu7_set_smc_sram_address(adev, addr, limit);
		if (result)
			return result;
		original_data = RREG32(mmSMC_IND_DATA_11);
		extra_shift = 8 * (4 - byte_count);
		while (byte_count > 0) {
			data = (0x100 * data) + *src++;
			byte_count--;
		}
		data <<= extra_shift;
		data |= (original_data & ~((~0UL) << extra_shift));
		result = smu7_set_smc_sram_address(adev, addr, limit);
		if (result)
			return result;
		WREG32(mmSMC_IND_DATA_11, data);
	}
	return 0;
}

static int smu7_read_smc_sram_dword(struct frb_device *adev, uint32_t smc_addr, uint32_t *value, uint32_t limit)
{
	int result = smu7_set_smc_sram_address(adev, smc_addr, limit);

	*value = result ? 0 : RREG32(mmSMC_IND_DATA_11);
	return result;
}

static int smu7_is_smc_ram_running(struct frb_device *adev)
{
	return (0 == REG_GET_FIELD(RREG32_SMC(ixSMC_SYSCON_CLOCK_CNTL_0), SMC_SYSCON_CLOCK_CNTL_0, ck_disable)) &&
	       (0x20100 <= RREG32_SMC(ixSMC_PC_C));
}

/* smu7_send_msg_to_smc: amdgpu goes on whatever the answer; Firebrand returns it */
static int smu7_send_msg_to_smc(struct frb_device *adev, uint16_t msg)
{
	uint32_t ret;

	if (wait_resp(adev))
		frb_err(adev, "smu: no answer to the previous message before 0x%x", msg);
	ret = REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP);
	if (ret != 1 && ret != 0xFE)
		frb_info(adev, "smu: last message was failed ret is %u", ret);

	WREG32(mmSMC_RESP_0, 0);
	WREG32(mmSMC_MESSAGE_0, msg);

	if (wait_resp(adev)) {
		frb_err(adev, "smu: message 0x%x: no answer", msg);
		return -ETIMEDOUT;
	}
	ret = REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP);
	if (ret != 1) {
		frb_err(adev, "smu: message 0x%x answered 0x%x", msg, ret);
		return ret == 0xFE ? -ENOENT : -EIO;
	}
	return 0;
}

static int smu7_send_msg_to_smc_with_parameter(struct frb_device *adev, uint16_t msg, uint32_t parameter)
{
	if (wait_resp(adev))
		frb_err(adev, "smu: busy before message 0x%x", msg);
	WREG32(mmSMC_MSG_ARG_0, parameter);
	return smu7_send_msg_to_smc(adev, msg);
}

/*
 * --- the smu7_smumgr helpers the DPM code calls (frb_polaris10_smumgr.h) ---
 *
 * The message pair keeps Linux's semantics there: smu7_send_msg_to_smc
 * waits for the previous answer (PHM_WAIT_FIELD_UNEQUAL, the hardware
 * manager's 1 s timeout: MCLKDPM_Enable takes 256 ms on the E9173), sends,
 * waits again and returns 0 whatever the SMC answered. The answer is logged:
 * 0xFE (not supported) at debug level, as Linux does.
 */
static int wait_resp_us(struct frb_device *adev, uint32_t timeout_us)
{
	uint32_t i;

	for (i = 0; i < timeout_us; i++) {
		if (REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP) != 0)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

int frb_smu7_send_msg_to_smc(struct frb_device *adev, uint16_t msg, uint32_t timeout_us)
{
	uint32_t ret;

	if (wait_resp_us(adev, timeout_us))
		frb_err(adev, "smu: no answer to the previous message before 0x%x", msg);
	ret = REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP);
	if (ret == 0xFE)
		frb_dbg(adev, "smu: last message was not supported");
	else if (ret != 1)
		frb_info(adev, "smu: last message was failed ret is %u", ret);

	WREG32(mmSMC_RESP_0, 0);
	WREG32(mmSMC_MESSAGE_0, msg);

	if (wait_resp_us(adev, timeout_us))
		frb_err(adev, "smu: message 0x%x: no answer in %u us", msg, timeout_us);
	ret = REG_GET_FIELD(RREG32(mmSMC_RESP_0), SMC_RESP_0, SMC_RESP);
	if (ret == 0xFE)
		frb_dbg(adev, "smu: message %x was not supported", msg);
	else if (ret != 1)
		frb_info(adev, "smu: failed to send message %x ret is %u", msg, ret);
	return 0;
}

int frb_smu7_send_msg_to_smc_with_parameter(struct frb_device *adev, uint16_t msg, uint32_t parameter,
					    uint32_t timeout_us)
{
	if (wait_resp_us(adev, timeout_us))
		frb_err(adev, "smu: busy before message 0x%x", msg);
	WREG32(mmSMC_MSG_ARG_0, parameter);
	return frb_smu7_send_msg_to_smc(adev, msg, timeout_us);
}

uint32_t frb_smu7_get_argument(struct frb_device *adev)
{
	return RREG32(mmSMC_MSG_ARG_0);
}

int frb_smu7_copy_bytes_to_smc(struct frb_device *adev, uint32_t smc_start_address,
			       const uint8_t *src, uint32_t byte_count, uint32_t limit)
{
	return smu7_copy_bytes_to_smc(adev, smc_start_address, src, byte_count, limit);
}

int frb_smu7_read_smc_sram_dword(struct frb_device *adev, uint32_t smc_addr, uint32_t *value, uint32_t limit)
{
	return smu7_read_smc_sram_dword(adev, smc_addr, value, limit);
}

/* smu7_write_smc_sram_dword */
int frb_smu7_write_smc_sram_dword(struct frb_device *adev, uint32_t smc_addr, uint32_t value, uint32_t limit)
{
	int result = smu7_set_smc_sram_address(adev, smc_addr, limit);

	if (!result)
		WREG32(mmSMC_IND_DATA_11, value);
	return result;
}

/* --- firmware ------------------------------------------------------------ */

struct frb_smu_fw {
	const char *name;		/* the file amdgpu loads (frb_fw_select.c) */
	struct frb_fw fw;
	struct frb_ucode u;
	uint32_t offset;		/* in the firmware buffer (VRAM, BAR0 offset) */
};

/* amdgpu_ucode_init_bo order: SDMA0, SDMA1, CE, PFP, ME, MEC1 (+JT), RLC_G */
enum { FW_SDMA0, FW_SDMA1, FW_CE, FW_PFP, FW_ME, FW_MEC, FW_RLC, FW_COUNT };

static int load_file(struct frb_device *adev, struct frb_smu_fw *f)
{
	int r = adev->os->firmware(adev->os->cookie, f->name, &f->fw.data, &f->fw.size);

	if (r) {
		frb_err(adev, "smu: firmware %s missing (%d)", f->name, r);
		return r;
	}
	return frb_ucode_parse(adev, f->name, &f->fw, &f->u);
}

/*
 * amdgpu_ucode_init_bo + amdgpu_ucode_init_single_fw + amdgpu_ucode_patch_jt:
 * each image's payload, page aligned, through BAR0 (bytes as in the file,
 * which is what the engine expects: amdgpu memcpy's the file's bytes).
 */
static void copy_to_vram(struct frb_device *adev, uint32_t dst, const uint8_t *src, uint32_t bytes)
{
	uint32_t i;

	for (i = 0; i + 4 <= bytes; i += 4)
		frb_vram_write32(adev, dst + i, frb_get_le32(src, i));
	if (i < bytes) {
		uint8_t tail[4] = { 0, 0, 0, 0 };
		uint32_t k;

		for (k = 0; i + k < bytes; k++)
			tail[k] = src[i + k];
		frb_vram_write32(adev, dst + i, frb_get_le32(tail, 0));
	}
}

#define PAGE_ALIGN4K(x)	(((x) + 4095u) & ~4095u)

static int init_fw_buf(struct frb_device *adev, struct frb_smu_fw *fw)
{
	uint32_t off = adev->layout.fw_buf, end = adev->layout.fw_buf + adev->layout.fw_buf_size;
	int i;

	for (i = 0; i < FW_COUNT; i++) {
		struct frb_ucode *u = &fw[i].u;
		uint32_t need = PAGE_ALIGN4K(u->ucode_size_bytes);

		if (i == FW_MEC)
			need += PAGE_ALIGN4K(frb_ucode_le32(u, FRB_FWH_GFX_JT_SIZE) << 2);
		if (off + need > end || !frb_vram_ok(adev, off, need)) {
			frb_err(adev, "smu: the firmware buffer (%u KB) is too small", adev->layout.fw_buf_size >> 10);
			return -ERANGE;
		}
		fw[i].offset = off;
		copy_to_vram(adev, off, u->data + u->ucode_array_offset, u->ucode_size_bytes);
		if (i == FW_MEC) {
			/* amdgpu_ucode_patch_jt: the jump table after the page-aligned ucode */
			uint32_t jt_off = frb_ucode_le32(u, FRB_FWH_GFX_JT_OFFSET) * 4;
			uint32_t jt_size = frb_ucode_le32(u, FRB_FWH_GFX_JT_SIZE) * 4;

			if (u->ucode_array_offset + jt_off + jt_size > u->file_size)
				return -EINVAL;
			copy_to_vram(adev, off + PAGE_ALIGN4K(u->ucode_size_bytes),
				     u->data + u->ucode_array_offset + jt_off, jt_size);
		}
		off += need;
	}
	return 0;
}

/*
 * smu7_populate_single_firmware_entry + amdgpu_cgs_get_firmware_info, into
 * the LE TOC: id u16, version u16, image_addr_high, image_addr_low,
 * meta_data_addr_high, meta_data_addr_low, data_size_byte, flags u16,
 * num_register_entries u16.
 */
static void toc_entry(struct frb_device *adev, uint8_t *toc, uint32_t n, uint32_t fw_type,
		      const struct frb_smu_fw *f)
{
	uint8_t *e = toc + 8 + n * SMU_TOC_ENTRY_BYTES;
	uint64_t mc = adev->gmc.vram_start + f->offset;
	uint32_t size = f->u.ucode_size_bytes;

	if (fw_type == UCODE_ID_CP_MEC_JT1 || fw_type == UCODE_ID_CP_MEC_JT2) {
		mc += PAGE_ALIGN4K(f->u.ucode_size_bytes);
		size = frb_ucode_le32(&f->u, FRB_FWH_GFX_JT_SIZE) << 2;
	} else if (fw_type == UCODE_ID_CP_MEC) {
		size = frb_ucode_le32(&f->u, FRB_FWH_GFX_JT_OFFSET) << 2;
	}
	frb_put_le16(e, 0, (uint16_t)fw_type);
	frb_put_le16(e, 2, (uint16_t)f->u.ucode_version);
	frb_put_le32(e, 4, (uint32_t)(mc >> 32));
	frb_put_le32(e, 8, (uint32_t)mc);
	frb_put_le32(e, 12, 0);
	frb_put_le32(e, 16, 0);
	frb_put_le32(e, 20, size);
	frb_put_le16(e, 24, (fw_type == UCODE_ID_RLC_G || fw_type == UCODE_ID_CP_MEC) ? 1 : 0);
	frb_put_le16(e, 26, 0);
	frb_info(adev, "smu: toc[%u] id %u version 0x%04x at 0x%010llx, %u bytes, flags %u", n, fw_type,
		 (uint16_t)f->u.ucode_version, (unsigned long long)mc, size,
		 (fw_type == UCODE_ID_RLC_G || fw_type == UCODE_ID_CP_MEC) ? 1 : 0);
}

/* --- polaris10_smumgr.c ---------------------------------------------------- */

/* the LE-host bytes of avfs_graphics_level_polaris10[8] (smu74_discrete.h layout) */
static void avfs_graphics_level_bytes(uint8_t *b)
{
	/* MinVoltage, pcieDpmLevel, DeepSleepDivId, and the SclkSetting words */
	static const struct {
		uint32_t min_voltage;
		uint8_t pcie, deep_sleep;
		uint32_t sclk;
		uint16_t fcw_int, pcc_fcw_int;
		uint8_t pll_range, ssc_en;
		uint16_t pcc_up, fcw1_int, fcw1_frac, ss_slew;
	} lv[8] = {
		{ 0x100ea446, 0x00, 0x03, 0x30750000, 0x3000, 0x2600, 0, 0, 0x8f02, 0x2f00, 0x300e, 0x2700 },
		{ 0x400ea446, 0x01, 0x04, 0x409c0000, 0x2000, 0x1e00, 1, 1, 0x8300, 0x1f00, 0xcb5e, 0x1a00 },
		{ 0x740ea446, 0x01, 0x00, 0x50c30000, 0x2800, 0x2000, 1, 1, 0x0c02, 0x2700, 0x6433, 0x2100 },
		{ 0xa40ea446, 0x01, 0x00, 0x60ea0000, 0x3000, 0x2600, 1, 1, 0x8f02, 0x2f00, 0x300e, 0x2700 },
		{ 0xd80ea446, 0x01, 0x00, 0x70110100, 0x3800, 0x2c00, 1, 1, 0x1203, 0x3600, 0xc9e2, 0x2e00 },
		{ 0x3c0fa446, 0x01, 0x00, 0x80380100, 0x2000, 0x1e00, 2, 1, 0x8300, 0x1f00, 0xcb5e, 0x1a00 },
		{ 0x6c0fa446, 0x01, 0x00, 0x905f0100, 0x2400, 0x1e00, 2, 1, 0x8901, 0x2300, 0x314c, 0x1d00 },
		{ 0xa00fa446, 0x01, 0x00, 0xa0860100, 0x2800, 0x2000, 2, 1, 0x0c02, 0x2700, 0x6433, 0x2100 },
	};
	int i;

	memset(b, 0, 8 * GRAPHICS_LEVEL_BYTES);
	for (i = 0; i < 8; i++) {
		uint8_t *g = b + i * GRAPHICS_LEVEL_BYTES;

		frb_put_le32(g, 0, lv[i].min_voltage);	/* MinVoltage */
		g[4] = lv[i].pcie;			/* pcieDpmLevel */
		g[5] = lv[i].deep_sleep;		/* DeepSleepDivId */
		frb_put_le16(g, 6, 0x3200);		/* ActivityLevel */
		/* CgSpllFuncCntl3/4, CcPwrDynRm/1 (8..23), SclkDid, padding: 0 */
		g[26] = 0x01;				/* EnabledForActivity */
		g[27] = 0x01;				/* EnabledForThrottle */
		g[28] = 0x0a;				/* UpHyst */
		/* DownHyst, VoltageDownHyst, PowerThrottle: 0 */
		frb_put_le32(g, 32, lv[i].sclk);	/* SclkSetting.SclkFrequency */
		frb_put_le16(g, 36, lv[i].fcw_int);	/* Fcw_int */
		frb_put_le16(g, 38, 0);			/* Fcw_frac */
		frb_put_le16(g, 40, lv[i].pcc_fcw_int);	/* Pcc_fcw_int */
		g[42] = lv[i].pll_range;		/* PllRange */
		g[43] = lv[i].ssc_en;			/* SSc_En */
		frb_put_le16(g, 44, 0x0004);		/* Sclk_slew_rate */
		frb_put_le16(g, 46, lv[i].pcc_up);	/* Pcc_up_slew_rate */
		frb_put_le16(g, 48, 0xffff);		/* Pcc_down_slew_rate */
		frb_put_le16(g, 50, lv[i].fcw1_int);	/* Fcw1_int */
		frb_put_le16(g, 52, lv[i].fcw1_frac);	/* Fcw1_frac */
		frb_put_le16(g, 54, lv[i].ss_slew);	/* Sclk_ss_slew_rate */
	}
}

/* the LE-host bytes of avfs_memory_level_polaris10 */
static void avfs_memory_level_bytes(uint8_t *m)
{
	memset(m, 0, MEMORY_LEVEL_BYTES);
	frb_put_le32(m, 0, 0x100ea446);		/* MinVoltage */
	frb_put_le32(m, 4, 0);			/* MinMvdd */
	frb_put_le32(m, 8, 0x30750000);		/* MclkFrequency */
	m[12] = 0x01;				/* StutterEnable */
	m[13] = 0x01;				/* EnabledForThrottle */
	m[14] = 0x01;				/* EnabledForActivity */
	m[15] = 0x00;				/* padding_0 */
	m[16] = 0x00;				/* UpHyst */
	m[17] = 0x64;				/* DownHyst */
	m[18] = 0x00;				/* VoltageDownHyst */
	m[19] = 0x00;				/* padding_1 */
	frb_put_le16(m, 20, 0x1f00);		/* ActivityLevel */
	m[22] = 0x00;				/* DisplayWatermark */
	m[23] = 0x00;				/* Reserved */
}

/* polaris10_setup_graphics_level_structure */
static int polaris10_setup_graphics_level_structure(struct frb_device *adev)
{
	uint8_t vr_config[4], gl[8 * GRAPHICS_LEVEL_BYTES], ml[MEMORY_LEVEL_BYTES], boot_mvdd[2];
	uint32_t dpm_table_start = 0;
	uint16_t mvdd = 1300 * VOLTAGE_SCALE;

	if (smu7_read_smc_sram_dword(adev, SMU7_FIRMWARE_HEADER_LOCATION + FWH_DPM_TABLE,
				     &dpm_table_start, SMC_RAM_END))
		return -1;

	/* Default value for VRConfig = VR_MERGED_WITH_VDDC + VR_STATIC_VOLTAGE(VDDCI) */
	frb_put_le32(vr_config, 0, 0x01000500);	/* Real value:0x50001 */
	if (smu7_copy_bytes_to_smc(adev, dpm_table_start + DPM_VRCONFIG, vr_config, 4, SMC_RAM_END))
		return -1;

	avfs_graphics_level_bytes(gl);
	if (smu7_copy_bytes_to_smc(adev, dpm_table_start + DPM_GRAPHICS_LEVEL, gl, sizeof(gl), SMC_RAM_END))
		return -1;

	avfs_memory_level_bytes(ml);
	if (smu7_copy_bytes_to_smc(adev, dpm_table_start + DPM_MEMORY_LEVEL, ml, sizeof(ml), SMC_RAM_END))
		return -1;

	/* MVDD Boot value: PP_HOST_TO_SMC_US, i.e. the SMC's (big-endian) order */
	boot_mvdd[0] = (uint8_t)(mvdd >> 8);
	boot_mvdd[1] = (uint8_t)mvdd;
	if (smu7_copy_bytes_to_smc(adev, dpm_table_start + DPM_BOOT_MVDD, boot_mvdd, 2, SMC_RAM_END))
		return -1;
	frb_info(adev, "smu: AVFS: DPM table at 0x%05x; VRConfig, 8 graphics levels, 1 memory level, "
		 "BootMVdd copied", dpm_table_start);
	return 0;
}

/* polaris10_avfs_event_mgr: avfs_btc_param is 0 on this path (no PerformBtc) */
static int polaris10_avfs_event_mgr(struct frb_device *adev, struct smu7_state *s)
{
	if (!s->avfs_supported)
		return 0;
	if (polaris10_setup_graphics_level_structure(adev)) {
		frb_err(adev, "[AVFS] Could not Copy Graphics Level table over to SMU");
		return -EINVAL;
	}
	return 0;
}

/* smu7_upload_smc_firmware_data: little-endian words, auto-incrementing from 0x20000 */
static int smu7_upload_smc_firmware_data(struct frb_device *adev, const struct frb_ucode *u)
{
	uint32_t byte_count = u->ucode_size_bytes, tmp, i = 0;
	const uint8_t *src = u->data + u->ucode_array_offset;

	if (byte_count > SMU7_SMC_SIZE)
		return -EINVAL;
	WREG32(mmSMC_IND_INDEX_11, 0x20000);
	tmp = RREG32(mmSMC_IND_ACCESS_CNTL);
	WREG32(mmSMC_IND_ACCESS_CNTL, REG_SET_FIELD(tmp, SMC_IND_ACCESS_CNTL, AUTO_INCREMENT_IND_11, 1));
	for (; byte_count >= 4; byte_count -= 4, i += 4)
		WREG32(mmSMC_IND_DATA_11, frb_get_le32(src, i));
	tmp = RREG32(mmSMC_IND_ACCESS_CNTL);
	WREG32(mmSMC_IND_ACCESS_CNTL, REG_SET_FIELD(tmp, SMC_IND_ACCESS_CNTL, AUTO_INCREMENT_IND_11, 0));
	return byte_count ? -EINVAL : 0;
}

static int polaris10_start_smu_in_protection_mode(struct frb_device *adev, const struct frb_ucode *smc)
{
	int r;

	/* Assert reset */
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 1);
	r = smu7_upload_smc_firmware_data(adev, smc);
	if (r)
		return r;
	/* Clear status */
	WREG32_SMC(ixSMU_STATUS, 0);
	smc_write_field(adev, ixSMC_SYSCON_CLOCK_CNTL_0, SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK,
			SMC_SYSCON_CLOCK_CNTL_0__ck_disable__SHIFT, 0);
	/* De-assert reset */
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 0);
	if (phm_wait_on_indirect_register(adev, ixRCU_UC_EVENTS, RCU_UC_EVENTS__INTERRUPTS_ENABLED_MASK,
					  RCU_UC_EVENTS__INTERRUPTS_ENABLED_MASK))
		frb_err(adev, "smu: RCU_UC_EVENTS.INTERRUPTS_ENABLED did not come up");

	/* Call Test SMU message with 0x20000 offset to trigger SMU start */
	r = smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_Test, 0x20000);
	if (r)
		frb_err(adev, "smu: Test(0x20000): %d", r);

	/* Wait done bit to be set; check pass/failed indicator */
	if (phm_wait_for_indirect_register_unequal(adev, ixSMU_STATUS, 0, SMU_STATUS__SMU_DONE_MASK))
		frb_err(adev, "smu: SMU_STATUS.SMU_DONE never set");
	if (1 != REG_GET_FIELD(RREG32_SMC(ixSMU_STATUS), SMU_STATUS, SMU_PASS)) {
		frb_err(adev, "SMU Firmware start failed! (SMU_STATUS %08x)", RREG32_SMC(ixSMU_STATUS));
		return -EIO;
	}

	WREG32_SMC(ixFIRMWARE_FLAGS, 0);
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 1);
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 0);

	/* Wait for firmware to initialize */
	if (phm_wait_on_indirect_register(adev, ixFIRMWARE_FLAGS, FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK,
					  FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK)) {
		frb_err(adev, "smu: FIRMWARE_FLAGS.INTERRUPTS_ENABLED did not come up");
		return -ETIMEDOUT;
	}
	return 0;
}

static int polaris10_start_smu_in_non_protection_mode(struct frb_device *adev, const struct frb_ucode *smc)
{
	static const uint8_t jump[4] = { 0xE0, 0x00, 0x80, 0x40 };	/* smu7_program_jump_on_start */
	int r;

	/* wait for smc boot up */
	if (phm_wait_for_indirect_register_unequal(adev, ixRCU_UC_EVENTS, 0, RCU_UC_EVENTS__boot_seq_done_MASK))
		frb_err(adev, "smu: RCU_UC_EVENTS.boot_seq_done never set");
	/* Clear firmware interrupt enable flag */
	WREG32_SMC(ixFIRMWARE_FLAGS, 0);
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 1);
	r = smu7_upload_smc_firmware_data(adev, smc);
	if (r)
		return r;
	/* Set smc instruct start point at 0x0 */
	smu7_copy_bytes_to_smc(adev, 0x0, jump, 4, sizeof(jump) + 1);
	smc_write_field(adev, ixSMC_SYSCON_CLOCK_CNTL_0, SMC_SYSCON_CLOCK_CNTL_0__ck_disable_MASK,
			SMC_SYSCON_CLOCK_CNTL_0__ck_disable__SHIFT, 0);
	smc_write_field(adev, ixSMC_SYSCON_RESET_CNTL, SMC_SYSCON_RESET_CNTL__rst_reg_MASK,
			SMC_SYSCON_RESET_CNTL__rst_reg__SHIFT, 0);
	/* Wait for firmware to initialize */
	if (phm_wait_on_indirect_register(adev, ixFIRMWARE_FLAGS, FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK,
					  FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK)) {
		frb_err(adev, "smu: FIRMWARE_FLAGS.INTERRUPTS_ENABLED did not come up");
		return -ETIMEDOUT;
	}
	return 0;
}

/* smu7_request_smu_load_fw (a VI dGPU: not_vf, chip_id > TOPAZ) */
static int smu7_request_smu_load_fw(struct frb_device *adev, struct smu7_state *s, struct frb_smu_fw *fw)
{
	uint8_t toc[SMU_TOC_BYTES];
	uint32_t fw_to_load, n = 0, i;
	int r;

	/* amdgpu_ucode_init_bo */
	r = init_fw_buf(adev, fw);
	if (r)
		return r;

	if (s->soft_regs_start)
		WREG32_SMC(s->soft_regs_start + SOFT_UCODE_LOAD_STATUS, 0x0);

	smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_SMU_DRAM_ADDR_HI, (uint32_t)(s->smu_buffer_mc >> 32));
	smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_SMU_DRAM_ADDR_LO, (uint32_t)s->smu_buffer_mc);
	fw_to_load = UCODE_ID_RLC_G_MASK + UCODE_ID_SDMA0_MASK + UCODE_ID_SDMA1_MASK + UCODE_ID_CP_CE_MASK +
		     UCODE_ID_CP_ME_MASK + UCODE_ID_CP_PFP_MASK + UCODE_ID_CP_MEC_MASK;

	memset(toc, 0, sizeof(toc));
	frb_put_le32(toc, 0, 1);	/* structure_version */
	toc_entry(adev, toc, n++, UCODE_ID_RLC_G, &fw[FW_RLC]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_CE, &fw[FW_CE]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_PFP, &fw[FW_PFP]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_ME, &fw[FW_ME]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_MEC, &fw[FW_MEC]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_MEC_JT1, &fw[FW_MEC]);
	toc_entry(adev, toc, n++, UCODE_ID_CP_MEC_JT2, &fw[FW_MEC]);
	toc_entry(adev, toc, n++, UCODE_ID_SDMA0, &fw[FW_SDMA0]);
	toc_entry(adev, toc, n++, UCODE_ID_SDMA1, &fw[FW_SDMA1]);
	frb_put_le32(toc, 4, n);	/* num_entries */
	/* memcpy_toio of the LE structure */
	for (i = 0; i < sizeof(toc); i += 4)
		frb_vram_write32(adev, adev->layout.toc + i, frb_get_le32(toc, i));

	smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_DRV_DRAM_ADDR_HI, (uint32_t)(s->header_mc >> 32));
	smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_DRV_DRAM_ADDR_LO, (uint32_t)s->header_mc);

	r = smu7_send_msg_to_smc_with_parameter(adev, PPSMC_MSG_LoadUcodes, fw_to_load);
	if (r)
		frb_err(adev, "smu: LoadUcodes(0x%x): %d", fw_to_load, r);

	/* smu7_check_fw_load_finish */
	r = phm_wait_on_indirect_register(adev, s->soft_regs_start + SOFT_UCODE_LOAD_STATUS,
					  fw_to_load, fw_to_load);
	frb_info(adev, "smu: UcodeLoadStatus %08x, wanted %08x: %s",
		 RREG32(mmSMC_IND_DATA_11), fw_to_load, r ? "SMU load firmware failed" : "loaded");
	return r;
}

int frb_smu7_start(struct frb_device *adev)
{
	static struct frb_smu_fw fw[FW_COUNT];
	static char names[FRB_FW_TOC_COUNT][32];
	char smc_name[32];
	struct frb_smu_fw smc;
	struct smu7_state s;
	uint32_t efuse;
	int i, r;

	memset(&s, 0, sizeof(s));
	memset(fw, 0, sizeof(fw));
	memset(&smc, 0, sizeof(smc));
	if (adev->stage_done < FRB_STAGE_GMC || !adev->aper)
		return -EPERM;

	/*
	 * The files gfx_v8_0, sdma_v3_0 and amdgpu_cgs ask for (frb_fw_select.c):
	 * Polaris 10, 11 and 12 (the _2 CP images) and Vega M; the SMC image
	 * once SMU_FIRMWARE says which key the SMU takes (below).
	 */
	if (frb_fw_vi_toc_names(adev->asic_type, names)) {
		frb_err(adev, "smu: no SMU7 firmware set for %s", frb_asic_name(adev->asic_type));
		return -ENODEV;
	}
	for (i = 0; i < FW_COUNT; i++)
		fw[i].name = names[i];
	for (i = 0; i < FW_COUNT; i++) {
		r = load_file(adev, &fw[i]);
		if (r)
			return r;
	}

	/* smu7_init: amdgpu's two VRAM objects, here at the top of the CPU window */
	s.header_mc = adev->gmc.vram_start + adev->layout.toc;
	s.smu_buffer_mc = adev->gmc.vram_start + adev->layout.smu_buffer;
	s.fw_buf_mc = adev->gmc.vram_start + adev->layout.fw_buf;
	if (!frb_vram_ok(adev, adev->layout.toc, SMU_TOC_BYTES) ||
	    !frb_vram_ok(adev, adev->layout.smu_buffer, adev->layout.smu_buffer_size))
		return -ERANGE;
	/* polaris10_is_hw_avfs_present (and amdgpu_pp_feature_mask has PP_AVFS_MASK) */
	efuse = RREG32_SMC(ixSMU_EFUSE_0 + (49 * 4));
	s.avfs_supported = (efuse & 0x00000001) != 0;
	adev->smu_hw_avfs = s.avfs_supported;	/* for the DPM code's smu7_init tail */

	/* polaris10_start_smu */
	if (!smu7_is_smc_ram_running(adev)) {
		uint32_t smu_fw = RREG32_SMC(ixSMU_FIRMWARE);

		s.protected_mode = REG_GET_FIELD(smu_fw, SMU_FIRMWARE, SMU_MODE);
		s.security_hard_key = REG_GET_FIELD(RREG32_SMC(ixSMU_FIRMWARE), SMU_FIRMWARE, SMU_SEL);
		/*
		 * smu7_upload_smu_firmware_image: the image for the key the SMU
		 * takes (Polaris 10/11's _sk image for the soft key) and the
		 * kicker parts' own (amdgpu_cgs_get_firmware_info)
		 */
		frb_fw_smc_name(smc_name, sizeof(smc_name), adev->asic_type, adev->device, adev->revision,
				!s.security_hard_key);
		smc.name = smc_name;
		r = load_file(adev, &smc);
		if (r)
			return r;
		frb_info(adev, "smu: SMU_FIRMWARE %08x: %s mode, %s key; AVFS fuse %08x (%s); "
			 "uploading %s (version 0x%08x, %u bytes)",
			 smu_fw, s.protected_mode ? "protected" : "non-protected",
			 s.security_hard_key ? "hard" : "soft", efuse, s.avfs_supported ? "AVFS" : "no AVFS",
			 smc.name, smc.u.ucode_version, smc.u.ucode_size_bytes);
		if (s.protected_mode == 0)
			r = polaris10_start_smu_in_non_protection_mode(adev, &smc.u);
		else
			r = polaris10_start_smu_in_protection_mode(adev, &smc.u);
		if (r) {
			frb_err(adev, "Failed to load SMU ucode.");
			return r;
		}
		/* vegam_start_smu is polaris10_start_smu without the AVFS event step */
		if (adev->asic_type != CHIP_VEGAM)
			polaris10_avfs_event_mgr(adev, &s);
	} else {
		frb_info(adev, "smu: the SMC is already running");
	}

	/* Setup SoftRegsStart */
	smu7_read_smc_sram_dword(adev, SMU7_FIRMWARE_HEADER_LOCATION + FWH_SOFT_REGISTERS,
				 &s.soft_regs_start, SMC_RAM_END);
	frb_info(adev, "smu: firmware running; SoftRegisters at 0x%05x", s.soft_regs_start);

	return smu7_request_smu_load_fw(adev, &s, fw);
}

void frb_smu7_report(struct frb_device *adev)
{
	(void)adev;
}
