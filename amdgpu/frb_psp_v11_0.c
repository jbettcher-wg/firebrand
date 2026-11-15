/*
 * psp_v11_0.c: the MP0 11.0.2 (Vega 20) mailboxes, ported from Linux 7.1.1
 * for bare metal: the bootloader (system driver, then secure OS, each
 * through the 1 MB firmware buffer whose MC address >> 20 goes in
 * C2PMSG_36), the kernel-mode ring (C2PMSG_64/69/70/71) and its write
 * pointer (C2PMSG_67). No KDB, SPL or SOC/INTF/DBG/RAS drivers: Vega 20's
 * vega20_sos.bin is a v1.0 header with a system driver and a sOS only.
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
#include "frb_psp.h"
#include "include/asic_reg/mp/mp_11_0_offset.h"

/* psp_v11_0_is_sos_alive: the sOS sign of life */
int frb_psp_v11_0_sos_alive(struct frb_device *adev)
{
	return RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_81) != 0x0;
}

/* psp_v11_0_wait_for_bootloader: bit 31 of C2PMSG_35, 20 tries of psp_wait_for */
static int psp_v11_0_wait_for_bootloader(struct frb_device *adev)
{
	int ret = -ETIMEDOUT, retry_loop;

	for (retry_loop = 0; retry_loop < 20; retry_loop++) {
		ret = frb_psp_wait_for(adev, SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_35),
				       0x80000000, 0x8000FFFF, PSP_WAITREG_NOVERBOSE);
		if (ret == 0)
			return 0;
	}
	return ret;
}

/* psp_v11_0_bootloader_load_component (PSP_BL__LOAD_SYSDRV) */
int frb_psp_v11_0_bootloader_load_sysdrv(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	int ret;

	/* sys driver and sOS already loaded: an earlier load in this boot */
	if (frb_psp_v11_0_sos_alive(adev))
		return 0;
	ret = psp_v11_0_wait_for_bootloader(adev);
	if (ret)
		return ret;
	/* Copy PSP System Driver binary to memory */
	frb_psp_copy_fw(adev, psp->sys_start, psp->sys_size);
	/* Provide the sys driver to bootloader */
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_36, (uint32_t)(psp->fw_pri_mc >> 20));
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35, PSP_BL__LOAD_SYSDRV);
	return psp_v11_0_wait_for_bootloader(adev);
}

/* psp_v11_0_bootloader_load_sos */
int frb_psp_v11_0_bootloader_load_sos(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	int ret;

	if (frb_psp_v11_0_sos_alive(adev))
		return 0;
	ret = psp_v11_0_wait_for_bootloader(adev);
	if (ret)
		return ret;
	/* Copy Secure OS binary to PSP memory */
	frb_psp_copy_fw(adev, psp->sos_start, psp->sos_size);
	/* Provide the PSP secure OS to bootloader */
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_36, (uint32_t)(psp->fw_pri_mc >> 20));
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_35, PSP_BL__LOAD_SOSDRV);
	/* there might be handshake issue with hardware which needs delay */
	mdelay(20);
	return frb_psp_wait_for(adev, SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_81),
				RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_81), 0, PSP_WAITREG_CHANGED);
}

/* psp_v11_0_ring_stop, bare metal: DESTROY_RINGS through C2PMSG_64 */
int frb_psp_v11_0_ring_stop(struct frb_device *adev)
{
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_64, GFX_CTRL_CMD_ID_DESTROY_RINGS);
	/* there might be handshake issue with hardware which needs delay */
	mdelay(20);
	return frb_psp_wait_for(adev, SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_64),
				MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK, 0);
}

/* psp_v11_0_ring_create, bare metal: the KM ring through C2PMSG_69/70/71 and 64 */
int frb_psp_v11_0_ring_create(struct frb_device *adev)
{
	struct frb_psp *psp = adev->psp;
	int ret;

	/* Wait for sOS ready for ring creation */
	ret = frb_psp_wait_for(adev, SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_64),
			       MBOX_TOS_READY_FLAG, MBOX_TOS_READY_MASK, 0);
	if (ret) {
		frb_err(adev, "Failed to wait for sOS ready for ring creation");
		return ret;
	}
	/* low and high address of the ring, its size, then the init command */
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_69, lower_32_bits(psp->ring_mc));
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_70, upper_32_bits(psp->ring_mc));
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_71, PSP_RING_SIZE);
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_64, PSP_RING_TYPE__KM << 16);
	/* there might be handshake issue with hardware which needs delay */
	mdelay(20);
	/* Wait for response flag (bit 31) in C2PMSG_64 */
	return frb_psp_wait_for(adev, SOC15_REG_OFFSET(MP0, 0, mmMP0_SMN_C2PMSG_64),
				MBOX_TOS_RESP_FLAG, MBOX_TOS_RESP_MASK, 0);
}

uint32_t frb_psp_v11_0_ring_get_wptr(struct frb_device *adev)
{
	return RREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_67);
}

void frb_psp_v11_0_ring_set_wptr(struct frb_device *adev, uint32_t value)
{
	WREG32_SOC15(MP0, 0, mmMP0_SMN_C2PMSG_67, value);
}
