/*
 * The PSP (MP0, the Platform Security Processor) on Vega 20: amdgpu_psp.c's
 * firmware loading and psp_v11_0.c's mailboxes, as frb_psp.c and
 * frb_psp_v11_0.c port them.
 *
 * BYTE ORDER. Linux fills the ring frames and the command buffer as host
 * C structures (psp_gfx_if.h) and memcpy's them into VRAM: little-endian on
 * every host it ever ran on. Firebrand stores every field with an explicit
 * little-endian store at the byte offsets below (docs/byte-order.md); no
 * psp_gfx_if.h structure is laid over memory. The offsets are checked
 * against the Linux capture's last command buffer (vram/psp-cmd-*.bin:
 * cmd_id at +8, the INVOKE payload at +28, the response at +864) and its
 * ring (psp-ring.bin: one 64-byte frame per submission).
 *
 * Copyright 2016-2019 Advanced Micro Devices, Inc.
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
#ifndef FRB_PSP_H
#define FRB_PSP_H

#include "frb_device.h"

/* amdgpu_psp.h */
#define PSP_FENCE_BUFFER_SIZE	0x1000
#define PSP_CMD_BUFFER_SIZE	0x1000
#define PSP_1_MEG		0x100000
#define PSP_TMR_SIZE		0x400000
#define PSP_TMR_ALIGNMENT	0x100000
#define PSP_RING_SIZE		0x1000		/* psp_ring_init */
#define PSP_ASD_SHARED_MEM_SIZE	0x0
#define PSP_BL__LOAD_SYSDRV	0x10000
#define PSP_BL__LOAD_SOSDRV	0x20000
#define PSP_RING_TYPE__KM	2		/* enum psp_ring_type: kernel mode ring (GPCOM) */
/* psp_gfx_if.h: the mailbox flags (MBOX_TOS_*) */
#define GFX_FLAG_RESPONSE	0x80000000
#define GFX_CMD_STATUS_MASK	0x0000FFFF
#define GFX_CMD_RESPONSE_MASK	0x80000000
#define MBOX_TOS_READY_FLAG	GFX_FLAG_RESPONSE
#define MBOX_TOS_READY_MASK	(GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)
#define MBOX_TOS_RESP_FLAG	GFX_FLAG_RESPONSE
#define MBOX_TOS_RESP_MASK	(GFX_CMD_RESPONSE_MASK | GFX_CMD_STATUS_MASK)

/* psp_gfx_if.h enum psp_gfx_crtl_cmd_id */
#define GFX_CTRL_CMD_ID_INIT_GPCOM_RING	0x00020000
#define GFX_CTRL_CMD_ID_DESTROY_RINGS	0x00030000
/* psp_gfx_if.h enum psp_gfx_cmd_id */
#define GFX_CMD_ID_LOAD_TA		0x00000001
#define GFX_CMD_ID_UNLOAD_TA		0x00000002
#define GFX_CMD_ID_INVOKE_CMD		0x00000003
#define GFX_CMD_ID_LOAD_ASD		0x00000004
#define GFX_CMD_ID_SETUP_TMR		0x00000005
#define GFX_CMD_ID_LOAD_IP_FW		0x00000006
#define GFX_CMD_ID_DESTROY_TMR		0x00000007
/* psp_gfx_if.h enum psp_gfx_fw_type, the Vega 20 images */
#define GFX_FW_TYPE_CP_ME		1
#define GFX_FW_TYPE_CP_PFP		2
#define GFX_FW_TYPE_CP_CE		3
#define GFX_FW_TYPE_CP_MEC		4
#define GFX_FW_TYPE_CP_MEC_ME1		5
#define GFX_FW_TYPE_CP_MEC_ME2		6
#define GFX_FW_TYPE_RLC_G		8
#define GFX_FW_TYPE_SDMA0		9
#define GFX_FW_TYPE_SDMA1		10
#define GFX_FW_TYPE_UVD			14
#define GFX_FW_TYPE_VCE			15
#define GFX_FW_TYPE_SMU			18
#define GFX_FW_TYPE_RLC_RESTORE_LIST_GPM_MEM	20
#define GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_MEM	21
#define GFX_FW_TYPE_RLC_RESTORE_LIST_SRM_CNTL	22
#define GFX_FW_TYPE_UVD1		23

/* struct psp_gfx_rb_frame, 64 bytes */
#define PSP_FRAME_SIZE			64
#define PSP_FRAME_CMD_BUF_ADDR_LO	0
#define PSP_FRAME_CMD_BUF_ADDR_HI	4
#define PSP_FRAME_CMD_BUF_SIZE		8	/* Linux leaves it 0 */
#define PSP_FRAME_FENCE_ADDR_LO		12
#define PSP_FRAME_FENCE_ADDR_HI		16
#define PSP_FRAME_FENCE_VALUE		20
#define PSP_FRAME_SID_LO		24
#define PSP_FRAME_SID_HI		28
#define PSP_FRAME_VMID			32	/* u8 */
#define PSP_FRAME_TYPE			33	/* u8 */

/* struct psp_gfx_cmd_resp, 1024 bytes */
#define PSP_CMD_RESP_SIZE		1024
#define PSP_CMD_BUF_SIZE		0
#define PSP_CMD_BUF_VERSION		4
#define PSP_CMD_ID			8
#define PSP_CMD_PAYLOAD			28	/* union psp_gfx_commands */
#define PSP_CMD_RESP			864	/* struct psp_gfx_resp */
#define PSP_RESP_STATUS			(PSP_CMD_RESP + 0)
#define PSP_RESP_SESSION_ID		(PSP_CMD_RESP + 4)
#define PSP_RESP_FW_ADDR_LO		(PSP_CMD_RESP + 8)
#define PSP_RESP_FW_ADDR_HI		(PSP_CMD_RESP + 12)
#define PSP_RESP_TMR_SIZE		(PSP_CMD_RESP + 16)
/* union members, from PSP_CMD_PAYLOAD */
#define PSP_LOAD_IP_FW_ADDR_LO		0	/* struct psp_gfx_cmd_load_ip_fw */
#define PSP_LOAD_IP_FW_ADDR_HI		4
#define PSP_LOAD_IP_FW_SIZE		8
#define PSP_LOAD_IP_FW_TYPE		12	/* enum psp_gfx_fw_type: 4 bytes */
#define PSP_SETUP_TMR_ADDR_LO		0	/* struct psp_gfx_cmd_setup_tmr */
#define PSP_SETUP_TMR_ADDR_HI		4
#define PSP_SETUP_TMR_SIZE		8
#define PSP_SETUP_TMR_FLAGS		12	/* bit 0 sriov_enabled, bit 1 virt_phy_addr (LE bitfield) */
#define PSP_SETUP_TMR_SYS_ADDR_LO	16
#define PSP_SETUP_TMR_SYS_ADDR_HI	20
#define PSP_TMR_FLAG_VIRT_PHY_ADDR	(1u << 1)
#define PSP_LOAD_TA_APP_ADDR_LO		0	/* struct psp_gfx_cmd_load_ta (LOAD_ASD too) */
#define PSP_LOAD_TA_APP_ADDR_HI		4
#define PSP_LOAD_TA_APP_LEN		8
#define PSP_LOAD_TA_CMD_BUF_ADDR_LO	12
#define PSP_LOAD_TA_CMD_BUF_ADDR_HI	16
#define PSP_LOAD_TA_CMD_BUF_LEN		20
#define PSP_UNLOAD_TA_SESSION_ID	0	/* struct psp_gfx_cmd_unload_ta */

/* the images psp_load_non_psp_fw loads, in AMDGPU_UCODE_ID order (Vega 20: 17) */
#define FRB_PSP_MAX_IMAGES	20
struct frb_psp_image {
	const char *name;		/* amdgpu_ucode_name, for the log */
	uint32_t fw_type;		/* GFX_FW_TYPE_* */
	const uint8_t *src;		/* the bytes the PSP gets */
	uint32_t size;			/* ucode_size */
	uint32_t buf_off;		/* in the fw_buf (layout.fw_buf) */
	uint32_t tmr_lo, tmr_hi;	/* where the PSP put it (resp.fw_addr) */
};

struct frb_psp {
	/* vega20_sos.bin (psp_firmware_header_v1_0): the system driver and the secure OS */
	const uint8_t *sys_start, *sos_start;
	uint32_t sys_size, sos_size, sys_version, sos_version;
	/* vega20_asd.bin */
	const uint8_t *asd_start;
	uint32_t asd_size, asd_version;
	struct frb_psp_image img[FRB_PSP_MAX_IMAGES];
	uint32_t num_images;
	uint64_t fw_pri_mc, ring_mc, cmd_mc, fence_mc, tmr_mc, fw_buf_mc;
	uint32_t fence_value;
	int ring_created, tmr_loaded;
	int asd_initialized;
	uint32_t asd_session_id;
	uint32_t loaded;		/* images the PSP took */
	uint32_t timeout;		/* psp_cmd_submit_buf's poll count (adev->psp_timeout) */
};

/* frb_psp.c */
void frb_psp_copy_fw(struct frb_device *adev, const uint8_t *start, uint32_t size);
int frb_psp_sw_init(struct frb_device *adev);
int frb_psp_hw_init(struct frb_device *adev);
void frb_psp_hw_fini(struct frb_device *adev);
/* stage smu on SOC15: the PSP's hw_init, then the SMU's (frb_vega20_smumgr.c) */
int frb_soc15_stage_smu(struct frb_device *adev);
/* psp_wait_for: 0 or -ETIMEDOUT */
#define PSP_WAITREG_CHANGED	(1u << 0)
#define PSP_WAITREG_NOVERBOSE	(1u << 1)
int frb_psp_wait_for(struct frb_device *adev, uint32_t reg_index, uint32_t reg_val, uint32_t mask, uint32_t flags);

/* frb_psp_v11_0.c */
int frb_psp_v11_0_sos_alive(struct frb_device *adev);
int frb_psp_v11_0_bootloader_load_sysdrv(struct frb_device *adev);
int frb_psp_v11_0_bootloader_load_sos(struct frb_device *adev);
int frb_psp_v11_0_ring_create(struct frb_device *adev);
int frb_psp_v11_0_ring_stop(struct frb_device *adev);
uint32_t frb_psp_v11_0_ring_get_wptr(struct frb_device *adev);
void frb_psp_v11_0_ring_set_wptr(struct frb_device *adev, uint32_t value);

#endif /* FRB_PSP_H */
