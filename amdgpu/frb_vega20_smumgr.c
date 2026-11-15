/*
 * vega20_smumgr.c and smu9_smumgr.c (Linux 7.1.1, pm/powerplay/smumgr):
 * the SMU's table buffers (vega20_smu_init), the start (vega20_start_smu:
 * the PSP-loaded SMC must be running, then the tools table's address), the
 * message mailbox (C2PMSG_66 message, _82 argument and answer, _90
 * response) and the table transfers.
 *
 * Firebrand's differences: the table buffers live in the CPU window
 * (layout.smu_tables) instead of amdgpu's top-down VRAM placement, so the
 * addresses the SetDriverDramAddr/SetToolsDramAddr messages carry differ
 * from the capture's (tests/frb_replay.c matches those arguments by
 * message); vega20_smu_init's smu_v11_0_i2c_control_init (the RAS EEPROM's
 * I2C bus) is not carried.
 *
 * Copyright 2018 Advanced Micro Devices, Inc.
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
#include "frb_smu11.h"
#include "include/pp/vega20_ppsmc.h"
#include "include/asic_reg/mp/mp_9_0_offset.h"

/* vega20_smumgr.c: MP1's firmware flags through SMN */
#define MP1_Public					0x03b00000
#define smnMP1_FIRMWARE_FLAGS				0x3010024
#define MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK	0x00000001L
#define PPSMC_Result_OK					0x1

/* sizeof the smu11_driver_if.h tables (checked on both hosts by tests/frb_unit.c) */
#define SIZEOF_PPTable_t			1256
#define SIZEOF_Watermarks_t			124
#define SIZEOF_OverDriveTable_t			32
#define SIZEOF_SmuMetrics_t			96
#define SIZEOF_DpmActivityMonitorCoeffInt_t	128
#define VEGA20_TOOLS_SIZE			0x19000

/* vega20_smu_init: the buffers, in its order, page aligned, in layout.smu_tables */
static int vega20_smu_init(struct frb_device *adev)
{
	static const struct { int id; uint32_t size; } t[] = {
		{ FRB_SMU11_TABLE_PPTABLE, SIZEOF_PPTable_t },
		{ FRB_SMU11_TABLE_WATERMARKS, SIZEOF_Watermarks_t },
		{ FRB_SMU11_TABLE_PMSTATUSLOG, VEGA20_TOOLS_SIZE },
		{ FRB_SMU11_TABLE_OVERDRIVE, SIZEOF_OverDriveTable_t },
		{ FRB_SMU11_TABLE_SMU_METRICS, SIZEOF_SmuMetrics_t },
		{ FRB_SMU11_TABLE_ACTIVITY_MONITOR_COEFF, SIZEOF_DpmActivityMonitorCoeffInt_t },
	};
	struct frb_smu11 *smu;
	uint32_t off = 0, i, b;

	if (!adev->smu11) {
		adev->smu11 = (struct frb_smu11 *)adev->os->alloc(adev->os->cookie, sizeof(*adev->smu11));
		if (!adev->smu11)
			return -ENOMEM;
	}
	smu = adev->smu11;
	memset(smu, 0, sizeof(*smu));
	for (i = 0; i < ARRAY_SIZE(t); i++) {
		struct frb_smu11_table *e = &smu->table[t[i].id];

		e->vram = adev->layout.smu_tables + off;
		e->mc_addr = adev->gmc.vram_start + e->vram;
		e->size = t[i].size;
		e->version = 0x01;
		off += (t[i].size + 4095u) & ~4095u;
		/* amdgpu_bo_create_kernel leaves VRAM as it was; zero it here (the CPU's first touch) */
		for (b = 0; b < ((t[i].size + 4095u) & ~4095u); b += 4)
			frb_vram_write32(adev, e->vram + b, 0);
	}
	if (off > adev->layout.smu_tables_size) {
		frb_err(adev, "smu: the tables need %u KB, the buffer has %u KB", off >> 10,
			adev->layout.smu_tables_size >> 10);
		return -ERANGE;
	}
	return 0;
}

/* smu9_wait_for_response: C2PMSG_90 until nonzero, then its value */
static uint32_t smu9_wait_for_response(struct frb_device *adev)
{
	uint32_t reg = SOC15_REG_OFFSET(MP1, 0, mmMP1_SMN_C2PMSG_90), i;

	/* phm_wait_for_register_unequal(hwmgr, reg, 0, MP1_C2PMSG_90__CONTENT_MASK) */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(reg) != 0)
			break;
		udelay(1);
	}
	if (i == adev->usec_timeout)
		frb_err(adev, "No response from smu");
	return RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90);
}

/*
 * smu9_send_msg_to_smc(_with_parameter): the previous response, C2PMSG_90
 * cleared, the argument (only with a parameter: smum_send_msg_to_smc
 * leaves C2PMSG_82 alone), the message, the response.
 */
static int smu9_send(struct frb_device *adev, uint16_t msg, int with_parameter, uint32_t parameter, uint32_t *resp)
{
	uint32_t ret;

	smu9_wait_for_response(adev);
	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_90, 0);
	if (with_parameter)
		WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82, parameter);
	WREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_66, msg);
	ret = smu9_wait_for_response(adev);
	if (ret != PPSMC_Result_OK) {
		frb_err(adev, "Failed message: 0x%x, input parameter: 0x%x, error code: 0x%x", msg, parameter, ret);
		if (adev->smu11)
			adev->smu11->failed_msgs++;
	}
	if (resp)
		*resp = RREG32_SOC15(MP1, 0, mmMP1_SMN_C2PMSG_82);	/* smu9_get_argument */
	/* smu9 returns 0 whatever the answer; Linux logs and goes on */
	return 0;
}

int frb_smu11_send_msg(struct frb_device *adev, uint16_t msg, uint32_t parameter, uint32_t *resp)
{
	return smu9_send(adev, msg, 1, parameter, resp);
}

int frb_smu11_send_msg_noarg(struct frb_device *adev, uint16_t msg, uint32_t *resp)
{
	return smu9_send(adev, msg, 0, 0, resp);
}

/* vega20_copy_table_to_smc: the bytes into the buffer (LE), the HDP flush, the address, the transfer */
int frb_smu11_copy_table_to_smc(struct frb_device *adev, const uint8_t *table, int table_id)
{
	struct frb_smu11_table *e = &adev->smu11->table[table_id];
	uint32_t off;
	int ret;

	if (table_id >= FRB_SMU11_TABLE_COUNT || !e->version || !e->size)
		return -EINVAL;
	for (off = 0; off < e->size; off += 4) {
		uint8_t w[4] = { 0, 0, 0, 0 };

		memcpy(w, table + off, e->size - off < 4 ? e->size - off : 4);
		frb_vram_write32(adev, e->vram + off, frb_get_le32(w, 0));
	}
	frb_soc15_flush_hdp(adev);
	if ((ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetDriverDramAddrHigh, upper_32_bits(e->mc_addr), NULL)) ||
	    (ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetDriverDramAddrLow, lower_32_bits(e->mc_addr), NULL)) ||
	    (ret = frb_smu11_send_msg(adev, PPSMC_MSG_TransferTableDram2Smu, table_id, NULL)))
		return ret;
	return 0;
}

/* vega20_copy_table_from_smc: the address, the transfer, the HDP invalidate, the bytes out (LE) */
int frb_smu11_copy_table_from_smc(struct frb_device *adev, uint8_t *table, int table_id)
{
	struct frb_smu11_table *e = &adev->smu11->table[table_id];
	uint32_t off, v;
	int ret;

	if (table_id >= FRB_SMU11_TABLE_COUNT || !e->version || !e->size)
		return -EINVAL;
	if ((ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetDriverDramAddrHigh, upper_32_bits(e->mc_addr), NULL)) ||
	    (ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetDriverDramAddrLow, lower_32_bits(e->mc_addr), NULL)) ||
	    (ret = frb_smu11_send_msg(adev, PPSMC_MSG_TransferTableSmu2Dram, table_id, NULL)))
		return ret;
	frb_soc15_invalidate_hdp(adev);
	for (off = 0; off < e->size; off += 4) {
		uint8_t w[4];

		v = frb_vram_read32(adev, e->vram + off);
		frb_put_le32(w, 0, v);
		memcpy(table + off, w, e->size - off < 4 ? e->size - off : 4);
	}
	return 0;
}

/* vega20_is_smc_ram_running: MP1_FIRMWARE_FLAGS.INTERRUPTS_ENABLED through SMN */
static int vega20_is_smc_ram_running(struct frb_device *adev)
{
	uint32_t mp1_fw_flags = RREG32_SMN(MP1_Public | (smnMP1_FIRMWARE_FLAGS & 0xffffffff));

	frb_info(adev, "smu: MP1_FIRMWARE_FLAGS 0x%08x", mp1_fw_flags);
	return (mp1_fw_flags & MP1_FIRMWARE_FLAGS__INTERRUPTS_ENABLED_MASK) != 0;
}

/* vega20_set_tools_address */
static int vega20_set_tools_address(struct frb_device *adev)
{
	struct frb_smu11_table *e = &adev->smu11->table[FRB_SMU11_TABLE_PMSTATUSLOG];
	int ret = 0;

	if (e->mc_addr) {
		ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetToolsDramAddrHigh, upper_32_bits(e->mc_addr), NULL);
		if (!ret)
			ret = frb_smu11_send_msg(adev, PPSMC_MSG_SetToolsDramAddrLow, lower_32_bits(e->mc_addr), NULL);
	}
	return ret;
}

/* vega20_start_smu (amdgpu_pm_load_smu_firmware -> pp_dpm_load_fw) */
static int vega20_start_smu(struct frb_device *adev)
{
	if (!vega20_is_smc_ram_running(adev)) {
		frb_err(adev, "[Vega20StartSmu] SMC is not running!");
		return -EINVAL;
	}
	if (vega20_set_tools_address(adev)) {
		frb_err(adev, "[Vega20StartSmu] Failed to set tools address!");
		return -EIO;
	}
	adev->smu11->started = 1;
	return 0;
}

int frb_smu11_hw_init(struct frb_device *adev)
{
	int r;

	r = vega20_smu_init(adev);
	if (r)
		return r;
	r = vega20_start_smu(adev);
	if (r)
		return r;
	frb_info(adev, "smu: the SMC runs (the PSP loaded it); GPU temperature %d C", frb_soc15_get_temperature(adev));
	if (!adev->want_dpm) {
		frb_info(adev, "smu: FirebrandDPM off: no DPM tasks, the SMU stays at the boot clocks");
		return 0;
	}
	return frb_vega20_hwmgr_hw_init(adev);
}

void frb_smu11_hw_fini(struct frb_device *adev)
{
	if (adev->smu11 && adev->smu11->dpm_enabled)
		frb_vega20_hwmgr_hw_fini(adev);
}
