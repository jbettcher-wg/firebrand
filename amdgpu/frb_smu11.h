/*
 * The SMU (MP1) of Vega 20, driven as Linux 7.1.1's powerplay drives it:
 * vega20_smumgr.c (the table buffers, the SMC's start, messages through
 * smu9_smumgr.c's C2PMSG_66/82/90 mailbox) and vega20_hwmgr.c (the DPM
 * set-up powerplay's hw_init makes). frb_vega20_smumgr.c and
 * frb_vega20_hwmgr.c port them.
 *
 * BYTE ORDER. The SMU reads its tables (PPTable_t, OverDriveTable_t, ...)
 * from VRAM as little-endian structures; Linux memcpy's host structures
 * there. Firebrand keeps each table as a little-endian byte image: the
 * PPTable is the VBIOS's own bytes (already LE) with the smc_dpm_info
 * overlay stored field by field LE at offsetof() positions (static
 * asserts check the packed layout), and every field it reads back from an
 * SMU-written table goes through frb_get_le16/32 (docs/byte-order.md).
 *
 * Copyright 2018 Advanced Micro Devices, Inc.
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#ifndef FRB_SMU11_H
#define FRB_SMU11_H

#include "frb_device.h"

/* smu11_driver_if.h table ids (TABLE_*) */
#define FRB_SMU11_TABLE_PPTABLE			0
#define FRB_SMU11_TABLE_WATERMARKS		1
#define FRB_SMU11_TABLE_AVFS			2
#define FRB_SMU11_TABLE_AVFS_PSM_DEBUG		3
#define FRB_SMU11_TABLE_AVFS_FUSE_OVERRIDE	4
#define FRB_SMU11_TABLE_PMSTATUSLOG		5
#define FRB_SMU11_TABLE_SMU_METRICS		6
#define FRB_SMU11_TABLE_DRIVER_SMU_CONFIG	7
#define FRB_SMU11_TABLE_ACTIVITY_MONITOR_COEFF	8
#define FRB_SMU11_TABLE_OVERDRIVE		9
#define FRB_SMU11_TABLE_COUNT			10

struct frb_smu11_table {
	uint32_t vram;		/* offset into VRAM (BAR0), 0 = no buffer */
	uint64_t mc_addr;
	uint32_t size;
	uint32_t version;
};

struct frb_vega20_hwmgr;	/* frb_vega20_hwmgr.c */

struct frb_smu11 {
	struct frb_smu11_table table[FRB_SMU11_TABLE_COUNT];
	uint32_t smu_version;		/* GetSmuVersion */
	uint64_t features_allowed, features_enabled;
	int started;			/* vega20_start_smu passed */
	int dpm_enabled;		/* the hwmgr's DPM tasks ran */
	int failed_msgs;
	struct frb_vega20_hwmgr *hw;
};

/* frb_vega20_smumgr.c */
int frb_smu11_hw_init(struct frb_device *adev);		/* start_smu, then the hwmgr's hw_init (want_dpm) */
void frb_smu11_hw_fini(struct frb_device *adev);
/*
 * smum_send_msg_to_smc_with_parameter on smu9: wait for the previous
 * response, clear it, the argument, the message, wait; *resp (if given) is
 * C2PMSG_82 afterwards. 0, or -EIO when the SMU did not answer 1 (OK); the
 * answer is logged either way, as Linux does.
 */
int frb_smu11_send_msg(struct frb_device *adev, uint16_t msg, uint32_t parameter, uint32_t *resp);
/* smum_send_msg_to_smc: the same without an argument (C2PMSG_82 untouched) */
int frb_smu11_send_msg_noarg(struct frb_device *adev, uint16_t msg, uint32_t *resp);
/* vega20_copy_table_to/from_smc: the table's bytes (LE) to and from its VRAM buffer */
int frb_smu11_copy_table_to_smc(struct frb_device *adev, const uint8_t *table, int table_id);
int frb_smu11_copy_table_from_smc(struct frb_device *adev, uint8_t *table, int table_id);
/* the SMC's own sensors, without messages: THM CTF temperature (frb_soc15.c) */

/* frb_vega20_hwmgr.c */
int frb_vega20_hwmgr_hw_init(struct frb_device *adev);
void frb_vega20_hwmgr_hw_fini(struct frb_device *adev);
/*
 * vega20_power_gate_uvd (amdgpu_dpm_enable_uvd(!gate)): the SMU's DPM_UVD
 * feature off (gate) or on, when the state changes; 0 or the message's error
 */
int frb_vega20_power_gate_uvd(struct frb_device *adev, int gate);
/* DC's display requests (vega20_display_clock_voltage_request: clk is a PPCLK_e) and requirements */
int frb_vega20_display_clock_request(struct frb_device *adev, int clk, uint32_t khz);
int frb_vega20_display_config_changed(struct frb_device *adev, uint32_t min_mem_khz, uint32_t min_dcef_khz,
				      int nb_pstate_switch_disable);

#endif /* FRB_SMU11_H */
