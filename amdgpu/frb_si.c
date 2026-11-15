/*
 * si.c: the SI common block, ported from Linux 7.0.2 (amdgpu): register
 * windows (SMC index/data 0, PCIE index 0xc), rev ids and the clock/power
 * gating flags (si_common_early_init), the golden registers, the PCI config
 * reset, and si_common_hw_init. Only Verde has its golden tables here: the
 * family table has a row for the R7 450 (Cape Verde) and no other SI card,
 * and a chip with no row never gets past the survey.
 *
 * Where Firebrand differs from amdgpu:
 * - si_pcie_gen3_enable reads the link and changes nothing: the T4240's
 *   root ports train gen2 x4/x8 and the card comes up at the rate the slot
 *   gives it (amdgpu returns at the same point when the link is already at
 *   its best speed). A speed change retrains the link through
 *   QorIQPCIBridge's root port, which nothing here has tested.
 * - si_program_aspm: amdgpu_device_should_use_aspm() is false on this
 *   platform, as for VI (frb_vi.c).
 * - si_fix_pci_max_read_req_size is the bridge's business (config space
 *   belongs to IOPCIFamily and QorIQPCIBridge); it is read and logged.
 *
 * The golden tables are amdgpu's verbatim; RadeonNI wrote radeon's (the
 * same values, byte offsets) on this card and logged them read back
 * (RadeonNI's hw/rdn_si.c rdn_si_golden_registers).
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
#include "frb_display.h"
#include "frb_ucode.h"
#include "include/sid.h"
#include "include/asic_reg/smu/smu_6_0_d.h"
#include "include/asic_reg/smu/smu_6_0_sh_mask.h"
#include "include/asic_reg/gca/gfx_6_0_d.h"
#include "include/asic_reg/gca/gfx_6_0_sh_mask.h"
#include "include/asic_reg/oss/oss_1_0_d.h"
#include "include/asic_reg/oss/oss_1_0_sh_mask.h"
#include "include/asic_reg/gmc/gmc_6_0_d.h"
#include "include/asic_reg/gmc/gmc_6_0_sh_mask.h"
#include "include/asic_reg/dce/dce_6_0_d.h"
#include "include/asic_reg/dce/dce_6_0_sh_mask.h"
#include "include/asic_reg/bif/bif_3_0_d.h"
#include "include/asic_reg/bif/bif_3_0_sh_mask.h"
#include "include/si_enums.h"

#define AMDGPU_ASIC_RESET_DATA	0x39d5e86b	/* amdgpu.h */
#define PCI_COMMAND		0x04
#define PCI_COMMAND_MASTER	0x4

static const uint32_t verde_pg_init[] =
{
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x40000,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x200010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x7007,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x300010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x400000,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x100010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x120200,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x500010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x1e1e16,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x600010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x171f1e,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x700010ff,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_WRITE, 0xffffffff, 0x0,
	mmGMCON_PGFSM_CONFIG, 0xffffffff, 0x9ff,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x0,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x10000800,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xf,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xf,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x4,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1000051e,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xffff,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xffff,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x8,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x80500,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x12,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x9050c,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x1d,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xb052c,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x2a,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1053e,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x2d,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x10546,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x30,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xa054e,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x3c,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1055f,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x3f,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x10567,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x42,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1056f,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x45,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x10572,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x48,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x20575,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x4c,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x190801,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x67,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1082a,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x6a,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1b082d,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x87,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x310851,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xba,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x891,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xbc,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x893,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xbe,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x20895,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xc2,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x20899,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xc6,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x2089d,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xca,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x8a1,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xcc,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x8a3,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xce,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x308a5,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0xd3,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x6d08cd,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x142,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x2000095a,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x1,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x144,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x301f095b,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x165,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xc094d,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x173,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xf096d,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x184,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x15097f,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x19b,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xc0998,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x1a9,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x409a7,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x1af,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0xcdc,
	mmGMCON_RENG_RAM_INDEX, 0xffffffff, 0x1b1,
	mmGMCON_RENG_RAM_DATA, 0xffffffff, 0x800,
	mmGMCON_RENG_EXECUTE, 0xffffffff, 0x6c9b2000,
	mmGMCON_MISC2, 0xfc00, 0x2000,
	mmGMCON_MISC3, 0xffffffff, 0xfc0,
	mmMC_PMG_AUTO_CFG, 0x00000100, 0x100,
};

static const uint32_t verde_golden_rlc_registers[] =
{
	mmGB_ADDR_CONFIG, 0xffffffff, 0x02010002,
	mmRLC_LB_PARAMS, 0xffffffff, 0x033f1005,
	0x311f, 0xffffffff, 0x10808020,
	0x3122, 0xffffffff, 0x00800008,
	mmRLC_LB_CNTR_MAX, 0xffffffff, 0x00001000,
	mmRLC_LB_CNTL, 0xffffffff, 0x80010014,
};

static const uint32_t verde_golden_registers[] =
{
	mmAZALIA_SCLK_CONTROL, 0x00000030, 0x00000011,
	mmCB_HW_CONTROL, 0x00010000, 0x00018208,
	mmDB_DEBUG, 0xffffffff, 0x00000000,
	mmDB_DEBUG2, 0xf00fffff, 0x00000400,
	mmDB_DEBUG3, 0x0002021c, 0x00020200,
	mmDCI_CLK_CNTL, 0x00000080, 0x00000000,
	0x340c, 0x000300c0, 0x00800040,
	0x360c, 0x000300c0, 0x00800040,
	mmFBC_DEBUG_COMP, 0x000000f0, 0x00000070,
	mmFBC_MISC, 0x00200000, 0x50100000,
	mmDIG0_HDMI_CONTROL, 0x31000311, 0x00000011,
	mmMC_SEQ_PMG_PG_HWCNTL, 0x00073ffe, 0x000022a2,
	mmMC_XPB_P2P_BAR_CFG, 0x000007ff, 0x00000000,
	mmPA_CL_ENHANCE, 0xf000001f, 0x00000007,
	mmPA_SC_FORCE_EOV_MAX_CNTS, 0xffffffff, 0x00ffffff,
	mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000,
	mmPA_SC_MODE_CNTL_1, 0x07ffffff, 0x4e000000,
	mmPA_SC_RASTER_CONFIG, 0x3f3f3fff, 0x0000124a,
	0x000c, 0xffffffff, 0x0040,
	0x000d, 0x00000040, 0x00004040,
	mmSPI_CONFIG_CNTL, 0x07ffffff, 0x03000000,
	mmSQ_DED_CNT, 0x01ff1f3f, 0x00000000,
	mmSQ_SEC_CNT, 0x01ff1f3f, 0x00000000,
	mmSX_DEBUG_1, 0x0000007f, 0x00000020,
	mmTA_CNTL_AUX, 0x00010000, 0x00010000,
	mmTCP_ADDR_CONFIG, 0x000003ff, 0x00000003,
	mmTCP_CHAN_STEER_HI, 0xffffffff, 0x00000000,
	mmTCP_CHAN_STEER_LO, 0xffffffff, 0x00001032,
	mmVGT_GS_VERTEX_REUSE, 0x0000001f, 0x00000010,
	mmVM_L2_CG, 0x000c0fc0, 0x000c0400,
	mmVM_PRT_APERTURE0_LOW_ADDR, 0x0fffffff, 0xffffffff,
	mmVM_PRT_APERTURE1_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE2_LOW_ADDR, 0x0fffffff, 0x0fffffff,
	mmVM_PRT_APERTURE3_LOW_ADDR, 0x0fffffff, 0x0fffffff,
};


static const uint32_t verde_mgcg_cgcg_init[] =
{
	mmRLC_CGTT_MGCG_OVERRIDE, 0xffffffff, 0xfffffffc,
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmCB_CGTT_SCLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_BCI_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_CP_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_GDS_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_IA_CLK_CTRL, 0xffffffff, 0x06000100,
	mmCGTT_PA_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_PC_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_RLC_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_SC_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_SPI_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_SQ_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_SQG_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_SX_CLK_CTRL0, 0xffffffff, 0x00000100,
	mmCGTT_SX_CLK_CTRL1, 0xffffffff, 0x00000100,
	mmCGTT_SX_CLK_CTRL2, 0xffffffff, 0x00000100,
	mmCGTT_SX_CLK_CTRL3, 0xffffffff, 0x00000100,
	mmCGTT_TCI_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_TCP_CLK_CTRL, 0xffffffff, 0x00000100,
	mmCGTT_VGT_CLK_CTRL, 0xffffffff, 0x06000100,
	mmDB_CGTT_CLK_CTRL_0, 0xffffffff, 0x00000100,
	mmTA_CGTT_CTRL, 0xffffffff, 0x00000100,
	mmTCA_CGTT_SCLK_CTRL, 0xffffffff, 0x00000100,
	mmTCC_CGTT_SCLK_CTRL, 0xffffffff, 0x00000100,
	mmTD_CGTT_CTRL, 0xffffffff, 0x00000100,
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	0x2458, 0xffffffff, 0x00010000,
	0x2459, 0xffffffff, 0x00030002,
	0x245a, 0xffffffff, 0x00040007,
	0x245b, 0xffffffff, 0x00060005,
	0x245c, 0xffffffff, 0x00090008,
	0x245d, 0xffffffff, 0x00020001,
	0x245e, 0xffffffff, 0x00040003,
	0x245f, 0xffffffff, 0x00000007,
	0x2460, 0xffffffff, 0x00060005,
	0x2461, 0xffffffff, 0x00090008,
	0x2462, 0xffffffff, 0x00030002,
	0x2463, 0xffffffff, 0x00050004,
	0x2464, 0xffffffff, 0x00000008,
	0x2465, 0xffffffff, 0x00070006,
	0x2466, 0xffffffff, 0x000a0009,
	0x2467, 0xffffffff, 0x00040003,
	0x2468, 0xffffffff, 0x00060005,
	0x2469, 0xffffffff, 0x00000009,
	0x246a, 0xffffffff, 0x00080007,
	0x246b, 0xffffffff, 0x000b000a,
	0x246c, 0xffffffff, 0x00050004,
	0x246d, 0xffffffff, 0x00070006,
	0x246e, 0xffffffff, 0x0008000b,
	0x246f, 0xffffffff, 0x000a0009,
	0x2470, 0xffffffff, 0x000d000c,
	0x2480, 0xffffffff, 0x00090008,
	0x2481, 0xffffffff, 0x000b000a,
	0x2482, 0xffffffff, 0x000c000f,
	0x2483, 0xffffffff, 0x000e000d,
	0x2484, 0xffffffff, 0x00110010,
	0x2485, 0xffffffff, 0x000a0009,
	0x2486, 0xffffffff, 0x000c000b,
	0x2487, 0xffffffff, 0x0000000f,
	0x2488, 0xffffffff, 0x000e000d,
	0x2489, 0xffffffff, 0x00110010,
	0x248a, 0xffffffff, 0x000b000a,
	0x248b, 0xffffffff, 0x000d000c,
	0x248c, 0xffffffff, 0x00000010,
	0x248d, 0xffffffff, 0x000f000e,
	0x248e, 0xffffffff, 0x00120011,
	0x248f, 0xffffffff, 0x000c000b,
	0x2490, 0xffffffff, 0x000e000d,
	0x2491, 0xffffffff, 0x00000011,
	0x2492, 0xffffffff, 0x0010000f,
	0x2493, 0xffffffff, 0x00130012,
	0x2494, 0xffffffff, 0x000d000c,
	0x2495, 0xffffffff, 0x000f000e,
	0x2496, 0xffffffff, 0x00100013,
	0x2497, 0xffffffff, 0x00120011,
	0x2498, 0xffffffff, 0x00150014,
	mmCGTS_SM_CTRL_REG, 0xffffffff, 0x96940200,
	mmCP_RB_WPTR_POLL_CNTL, 0xffffffff, 0x00900100,
	mmRLC_GCPM_GENERAL_3, 0xffffffff, 0x00000080,
	mmRLC_CGCG_CGLS_CTRL, 0xffffffff, 0x0020003f,
	0x000c, 0xffffffff, 0x0000001c,
	0x000d, 0x000f0000, 0x000f0000,
	0x0583, 0xffffffff, 0x00000100,
	mmXDMA_CLOCK_GATING_CNTL, 0xffffffff, 0x00000100,
	mmXDMA_MEM_POWER_CNTL, 0x00000101, 0x00000000,
	mmMC_MEM_POWER_LS, 0xffffffff, 0x00000104,
	mmMC_CITF_MISC_WR_CG, 0x000c0000, 0x000c0000,
	mmMC_CITF_MISC_RD_CG, 0x000c0000, 0x000c0000,
	mmCGTT_DRM_CLK_CTRL0, 0xff000fff, 0x00000100,
	0x157a, 0x00000001, 0x00000001,
	mmHDP_MEM_POWER_LS, 0x00000001, 0x00000001,
	mmHDP_XDP_CGTT_BLK_CTRL, 0xc0000fff, 0x00000104,
	mmCP_MEM_SLP_CNTL, 0x00000001, 0x00000001,
	0x3430, 0xfffffff0, 0x00000100,
	0x3630, 0xfffffff0, 0x00000100,
};


/* --- register windows (si_smc_rreg, si_pcie_rreg, si_pciep_rreg) ----------- */

static uint32_t si_smc_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(mmSMC_IND_INDEX_0, (reg));
	return RREG32(mmSMC_IND_DATA_0);
}

static void si_smc_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(mmSMC_IND_INDEX_0, (reg));
	WREG32(mmSMC_IND_DATA_0, (v));
}

uint32_t frb_si_pciep_rreg(struct frb_device *adev, uint32_t reg)
{
	WREG32(PCIE_PORT_INDEX, ((reg) & 0xff));
	(void)RREG32(PCIE_PORT_INDEX);
	return RREG32(PCIE_PORT_DATA);
}

void frb_si_pciep_wreg(struct frb_device *adev, uint32_t reg, uint32_t v)
{
	WREG32(PCIE_PORT_INDEX, ((reg) & 0xff));
	(void)RREG32(PCIE_PORT_INDEX);
	WREG32(PCIE_PORT_DATA, (v));
	(void)RREG32(PCIE_PORT_DATA);
}

/* --- si_common_early_init: reads only (the survey runs it) ------------------ */

static uint32_t si_get_rev_id(struct frb_device *adev)
{
	return (RREG32(mmCC_DRM_ID_STRAPS) & CC_DRM_ID_STRAPS__ATI_REV_ID_MASK)
		>> CC_DRM_ID_STRAPS__ATI_REV_ID__SHIFT;
}

static void si_common_early_init(struct frb_device *adev)
{
	adev->rev_id = si_get_rev_id(adev);
	adev->external_rev_id = 0xFF;
	adev->family = AMDGPU_FAMILY_SI;
	switch (adev->asic_type) {
	case CHIP_TAHITI:
		adev->external_rev_id = (adev->rev_id == 0) ? 1 :
					(adev->rev_id == 1) ? 5 : 6;
		break;
	case CHIP_PITCAIRN:
		adev->external_rev_id = adev->rev_id + 20;
		break;
	case CHIP_VERDE:
		adev->cg_flags =
			AMD_CG_SUPPORT_GFX_MGCG |
			AMD_CG_SUPPORT_GFX_MGLS |
			AMD_CG_SUPPORT_GFX_CGLS |
			AMD_CG_SUPPORT_GFX_CGTS |
			AMD_CG_SUPPORT_GFX_CGTS_LS |
			AMD_CG_SUPPORT_GFX_CP_LS |
			AMD_CG_SUPPORT_MC_LS |
			AMD_CG_SUPPORT_MC_MGCG |
			AMD_CG_SUPPORT_SDMA_MGCG |
			AMD_CG_SUPPORT_SDMA_LS |
			AMD_CG_SUPPORT_BIF_LS |
			AMD_CG_SUPPORT_VCE_MGCG |
			AMD_CG_SUPPORT_UVD_MGCG |
			AMD_CG_SUPPORT_HDP_LS |
			AMD_CG_SUPPORT_HDP_MGCG;
		adev->pg_flags = 0;
		adev->external_rev_id = adev->rev_id + 40;
		break;
	case CHIP_OLAND:
		adev->external_rev_id = 60;
		break;
	case CHIP_HAINAN:
		adev->external_rev_id = 70;
		break;
	default:
		break;
	}
	frb_info(adev, "si: rev_id %u, external_rev_id %u, cg_flags 0x%llx", adev->rev_id,
		 adev->external_rev_id, (unsigned long long)adev->cg_flags);
}

/* --- si_init_golden_registers ---------------------------------------------- */

/*
 * The count of entries whose register reads back as written, for the log
 * (RadeonNI's rdn_si_program_registers measure; a write-only or
 * self-clearing register reads otherwise, so this is a fingerprint to
 * compare against RadeonNI's on the same card, not a pass/fail).
 */
static uint32_t si_program_and_count(struct frb_device *adev, const uint32_t *regs, uint32_t n)
{
	uint32_t i, ok = 0;

	frb_device_program_register_sequence(adev, regs, n);
	for (i = 0; i < n; i += 3) {
		uint32_t and_mask = regs[i + 1], or_mask = regs[i + 2], v = RREG32(regs[i]);

		if (and_mask == 0xffffffff ? v == or_mask : (v & and_mask) == (or_mask & and_mask))
			ok++;
	}
	return ok;
}

static int si_init_golden_registers(struct frb_device *adev)
{
	uint32_t a, b, c, d;

	switch (adev->asic_type) {
	case CHIP_VERDE:
		a = si_program_and_count(adev, verde_golden_registers, ARRAY_SIZE(verde_golden_registers));
		b = si_program_and_count(adev, verde_golden_rlc_registers, ARRAY_SIZE(verde_golden_rlc_registers));
		c = si_program_and_count(adev, verde_mgcg_cgcg_init, ARRAY_SIZE(verde_mgcg_cgcg_init));
		d = si_program_and_count(adev, verde_pg_init, ARRAY_SIZE(verde_pg_init));
		frb_info(adev, "si: Verde golden registers %u of %u, rlc %u of %u, mgcg/cgcg %u of %u, pg %u of %u "
			 "read back as written", a, (uint32_t)ARRAY_SIZE(verde_golden_registers) / 3,
			 b, (uint32_t)ARRAY_SIZE(verde_golden_rlc_registers) / 3,
			 c, (uint32_t)ARRAY_SIZE(verde_mgcg_cgcg_init) / 3,
			 d, (uint32_t)ARRAY_SIZE(verde_pg_init) / 3);
		return 0;
	default:
		frb_err(adev, "si: no golden registers for this chip here (only Verde has a row)");
		return -ENODEV;
	}
}

/* --- PCIe: si_pcie_gen3_enable's reads (see the header) --------------------- */

static void si_pcie_report(struct frb_device *adev)
{
	uint32_t speed_cntl = frb_si_pciep_rreg(adev, ixPCIE_LC_SPEED_CNTL);
	uint32_t width_cntl = frb_si_pciep_rreg(adev, ixPCIE_LC_LINK_WIDTH_CNTL);
	uint32_t rate = (speed_cntl & PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE_MASK) >>
		PCIE_LC_SPEED_CNTL__LC_CURRENT_DATA_RATE__SHIFT;
	static const uint8_t lanes[8] = { 0, 1, 2, 4, 8, 12, 16, 0 };

	frb_info(adev, "si: PCIe link at gen %u x%u (LC_SPEED_CNTL %08x, LC_LINK_WIDTH_CNTL %08x); "
		 "speed and ASPM left as they are", rate + 1,
		 lanes[(width_cntl & PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD_MASK) >>
		       PCIE_LC_LINK_WIDTH_CNTL__LC_LINK_WIDTH_RD__SHIFT], speed_cntl, width_cntl);
}

/* si_common_hw_init */
static int si_common_hw_init(struct frb_device *adev)
{
	int r;

	/* si_fix_pci_max_read_req_size: config space is the bridge's; read it for the log */
	frb_info(adev, "si: PCIe device control %04x (max read request %u bytes)",
		 adev->os->cfg_read32(adev->os->cookie, 0x58 + 8) & 0xffff,
		 128u << ((adev->os->cfg_read32(adev->os->cookie, 0x58 + 8) >> 12) & 7));
	r = si_init_golden_registers(adev);
	if (r)
		return r;
	si_pcie_report(adev);		/* si_pcie_gen3_enable */
	/* si_program_aspm: amdgpu_device_should_use_aspm() is false here */
	return 0;
}

static uint32_t si_get_config_memsize(struct frb_device *adev)
{
	return RREG32(mmCONFIG_MEMSIZE);
}

/*
 * si_need_reset_on_init is false in amdgpu. Firebrand resets when an
 * earlier driver ran the card in this power cycle, as its VI port does for
 * a running SMC: here, a command processor that is not halted
 * (CP_ME_CNTL.ME_HALT clear). The VBIOS leaves it halted (0x15000000, the
 * radeon capture's cold read; ASIC_Init does not start it) and the stages
 * up to dce never start it, so only another driver's state (RadeonNI's
 * relocated VRAM and running CP after a warm reboot) takes the reset, and
 * the card comes back unposted for ASIC_Init, as from cold.
 */
static int si_need_reset_on_init(struct frb_device *adev)
{
	uint32_t cp = RREG32(mmCP_ME_CNTL);

	frb_info(adev, "si: CP_ME_CNTL %08x (%s), MC_VM_FB_LOCATION %08x, BIOS_SCRATCH_7 %08x", cp,
		 (cp & CP_ME_CNTL__ME_HALT_MASK) ? "halted, as the VBIOS leaves it" : "RUNNING: an earlier driver",
		 RREG32(mmMC_VM_FB_LOCATION), RREG32(mmBIOS_SCRATCH_7));
	return !(cp & CP_ME_CNTL__ME_HALT_MASK);
}

/* --- si_asic_reset: si_gpu_pci_config_reset --------------------------------- */

static void si_set_clk_bypass_mode(struct frb_device *adev)
{
	uint32_t tmp, i;

	tmp = RREG32(mmCG_SPLL_FUNC_CNTL);
	tmp |= CG_SPLL_FUNC_CNTL__SPLL_BYPASS_EN_MASK;
	WREG32(mmCG_SPLL_FUNC_CNTL, tmp);

	tmp = RREG32(mmCG_SPLL_FUNC_CNTL_2);
	tmp |= CG_SPLL_FUNC_CNTL_2__SPLL_CTLREQ_CHG_MASK;
	WREG32(mmCG_SPLL_FUNC_CNTL_2, tmp);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmCG_SPLL_STATUS) & CG_SPLL_STATUS__SPLL_CHG_STATUS_MASK)
			break;
		udelay(1);
	}

	tmp = RREG32(mmCG_SPLL_FUNC_CNTL_2);
	tmp &= ~(CG_SPLL_FUNC_CNTL_2__SPLL_CTLREQ_CHG_MASK |
		CG_SPLL_FUNC_CNTL_2__SCLK_MUX_UPDATE_MASK);
	WREG32(mmCG_SPLL_FUNC_CNTL_2, tmp);

	tmp = RREG32(MPLL_CNTL_MODE);
	tmp &= ~MPLL_MCLK_SEL;
	WREG32(MPLL_CNTL_MODE, tmp);
}

static void si_spll_powerdown(struct frb_device *adev)
{
	uint32_t tmp;

	tmp = RREG32(mmSPLL_CNTL_MODE);
	tmp |= SPLL_CNTL_MODE__SPLL_SW_DIR_CONTROL_MASK;
	WREG32(mmSPLL_CNTL_MODE, tmp);

	tmp = RREG32(mmCG_SPLL_FUNC_CNTL);
	tmp |= CG_SPLL_FUNC_CNTL__SPLL_RESET_MASK;
	WREG32(mmCG_SPLL_FUNC_CNTL, tmp);

	tmp = RREG32(mmCG_SPLL_FUNC_CNTL);
	tmp |= CG_SPLL_FUNC_CNTL__SPLL_SLEEP_MASK;
	WREG32(mmCG_SPLL_FUNC_CNTL, tmp);

	tmp = RREG32(mmSPLL_CNTL_MODE);
	tmp &= ~SPLL_CNTL_MODE__SPLL_SW_DIR_CONTROL_MASK;
	WREG32(mmSPLL_CNTL_MODE, tmp);
}

/* si_gpu_pci_config_reset, with the bus-master bit put back only if it was set (frb_vi.c) */
static int si_asic_reset(struct frb_device *adev)
{
	uint32_t i, cmd;
	int r = -EINVAL;

	frb_info(adev, "reset: PCI CONFIG reset");
	frb_atombios_scratch_regs_engine_hung(adev, 1);

	/* set mclk/sclk to bypass */
	si_set_clk_bypass_mode(adev);
	/* powerdown spll */
	si_spll_powerdown(adev);
	/* disable BM */
	cmd = adev->os->cfg_read32(adev->os->cookie, PCI_COMMAND) & 0xffff;
	if (cmd & PCI_COMMAND_MASTER)
		adev->os->cfg_write32(adev->os->cookie, PCI_COMMAND, cmd & ~PCI_COMMAND_MASTER);
	/* reset */
	adev->os->cfg_write32(adev->os->cookie, 0x7c, AMDGPU_ASIC_RESET_DATA);

	udelay(100);

	/* wait for asic to come out of reset */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmCONFIG_MEMSIZE) != 0xffffffff) {
			if (cmd & PCI_COMMAND_MASTER)
				adev->os->cfg_write32(adev->os->cookie, PCI_COMMAND, cmd);
			adev->has_hw_reset = 1;
			r = 0;
			break;
		}
		udelay(1);
	}
	frb_info(adev, "reset: PCI CONFIG reset: %s after %u us", r ? "the ASIC did not come back" : "back", 100 + i);
	frb_atombios_scratch_regs_engine_hung(adev, 0);
	if (!r)
		adev->gpu_reset_counter++;
	return r;
}

/* --- gfx_v6_0_constants_init's address configuration ------------------------ */

#define VERDE_GB_ADDR_CONFIG_GOLDEN	0x12010002	/* gfx_v6_0.c */

/*
 * How addresses interleave across pipes, banks and rows: GB_ADDR_CONFIG for
 * the GPU, and the copies the display (DMIF), the CPU's HDP window and the
 * DMA engines keep. amdgpu writes them in gfx_v6_0_constants_init, before
 * dce_v6_0 (its IP order: gmc, gfx, dma, dce); Firebrand's stage order puts
 * dce before gfx, so stage dce writes them first. The VBIOS leaves them
 * disagreeing (R7 450 after ASIC_Init: GB 0, DMIF 0x00011003, CALC 0, HDP
 * 0x42011003), and a scanout read through one config of a surface the CPU
 * wrote through another is broken every 512 bytes (RadeonNI on the W7000,
 * hw/rdn_si.c rdn_si_addr_config).
 */
int frb_si_gfx_addr_config(struct frb_device *adev)
{
	uint32_t gb_addr_config, mc_arb_ramcfg, tmp, row_kb, before[4];

	switch (adev->asic_type) {
	case CHIP_VERDE:
		gb_addr_config = VERDE_GB_ADDR_CONFIG_GOLDEN;
		break;
	default:
		frb_err(adev, "si: no address configuration for this chip here (only Verde has a row)");
		return -ENODEV;
	}
	before[0] = RREG32(mmGB_ADDR_CONFIG);
	before[1] = RREG32(mmDMIF_ADDR_CONFIG);
	before[2] = RREG32(mmDMIF_ADDR_CALC);
	before[3] = RREG32(mmHDP_ADDR_CONFIG);

	mc_arb_ramcfg = RREG32(mmMC_ARB_RAMCFG);
	tmp = (mc_arb_ramcfg & MC_ARB_RAMCFG__NOOFCOLS_MASK) >> MC_ARB_RAMCFG__NOOFCOLS__SHIFT;
	row_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (row_kb > 4)
		row_kb = 4;
	gb_addr_config &= ~GB_ADDR_CONFIG__ROW_SIZE_MASK;
	switch (row_kb) {
	case 1:
	default:
		gb_addr_config |= 0 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT;
		break;
	case 2:
		gb_addr_config |= 1 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT;
		break;
	case 4:
		gb_addr_config |= 2 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT;
		break;
	}
	/* max_shader_engines 1 on Verde: NUM_SHADER_ENGINES stays 0 */
	gb_addr_config &= ~GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK;

	WREG32(mmGB_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMIF_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMIF_ADDR_CALC, gb_addr_config);
	WREG32(mmHDP_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMA_TILING_CONFIG + DMA0_REGISTER_OFFSET, gb_addr_config);
	WREG32(mmDMA_TILING_CONFIG + DMA1_REGISTER_OFFSET, gb_addr_config);
	frb_info(adev, "si: address config %08x (rows %u KB from MC_ARB_RAMCFG %08x); the VBIOS had GB %08x "
		 "DMIF %08x CALC %08x HDP %08x", gb_addr_config, row_kb, mc_arb_ramcfg,
		 before[0], before[1], before[2], before[3]);
	return 0;
}

/* --- stage smu on SI -------------------------------------------------------- */

/* si_smc.c / sid.h, the SMC's own registers through the index window */
#define SI_SMC_SYSCON_RESET_CNTL	0x80000000
#define SI_RST_REG			(1 << 0)
#define SI_SMC_SYSCON_CLOCK_CNTL_0	0x80000004
#define SI_CK_DISABLE			(1 << 0)
#define SI_SMC_PC_C			0x80000370

/*
 * On SI the SMC has no start of its own before DPM: si_dpm_enable builds
 * the state tables, uploads the microcode (amdgpu_si_load_smc_ucode) and
 * starts the SMC with them (si_start_smc) in one go, which is the DPM step
 * after stage sdma. What stage smu can do here is what si_dpm_sw_init does:
 * pick the image (si_dpm_init_microcode: the "kicker" verde_k_smc.bin for
 * this 682b rev 87) and check it, and report whether the VBIOS left the SMC
 * running (amdgpu_si_is_smc_running). Reads and the firmware file only.
 */
int frb_si_smc_stage(struct frb_device *adev)
{
	const char *chip_name;
	struct frb_fw fw = { 0, 0 };
	struct frb_ucode u;
	char name[32];
	uint32_t rst, clk, pc;
	int r;

	switch (adev->asic_type) {
	case CHIP_VERDE:
		if (((adev->device == 0x6820) && ((adev->revision == 0x81) || (adev->revision == 0x83))) ||
		    ((adev->device == 0x6821) && ((adev->revision == 0x83) || (adev->revision == 0x87))) ||
		    ((adev->revision == 0x87) && ((adev->device == 0x6823) || (adev->device == 0x682b))))
			chip_name = "verde_k";
		else
			chip_name = "verde";
		break;
	default:
		frb_err(adev, "si: no SMC image choice for this chip here (only Verde has a row)");
		return -ENODEV;
	}
	frb_fw_name(name, sizeof(name), chip_name, "_smc.bin");
	r = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (r) {
		frb_err(adev, "si_smc: Failed to load firmware. err = %d \"%s\"", r, name);
		return r;
	}
	r = frb_ucode_parse(adev, name, &fw, &u);
	if (r)
		return r;
	rst = RREG32_SMC(SI_SMC_SYSCON_RESET_CNTL);
	clk = RREG32_SMC(SI_SMC_SYSCON_CLOCK_CNTL_0);
	pc = RREG32_SMC(SI_SMC_PC_C);
	frb_info(adev, "si: SMC image %s, %u bytes of microcode (version 0x%08x); the SMC is %s "
		 "(SYSCON_RESET_CNTL %08x, CLOCK_CNTL_0 %08x, PC %08x); it is loaded and started by "
		 "si_dpm_enable with its tables (DPM)", name, u.ucode_size_bytes, u.ucode_version,
		 (!(rst & SI_RST_REG) && !(clk & SI_CK_DISABLE)) ? "running" : "stopped", rst, clk, pc);
	return 0;
}

/* si_get_xclk (si.c): the reference clock (10 kHz) as the clock pins say; not an APU */
uint32_t frb_si_get_xclk(struct frb_device *adev)
{
	uint32_t reference_clock = adev->spll_reference_freq;
	uint32_t tmp;

	tmp = RREG32(mmCG_CLKPIN_CNTL_2);
	if (tmp & CG_CLKPIN_CNTL_2__MUX_TCLK_TO_XCLK_MASK)
		return TCLK;

	tmp = RREG32(mmCG_CLKPIN_CNTL);
	if (tmp & CG_CLKPIN_CNTL__XTALIN_DIVIDE_MASK)
		return reference_clock / 4;

	return reference_clock;
}

/* --- si_set_uvd_clocks (si.c): UVD's UPLL, for uvd_v3_1 without DPM ---------- */

static int si_uvd_send_upll_ctlreq(struct frb_device *adev,
				   unsigned cg_upll_func_cntl)
{
	unsigned i;

	/* Make sure UPLL_CTLREQ is deasserted */
	WREG32_P(cg_upll_func_cntl, 0, ~UPLL_CTLREQ_MASK);

	mdelay(10);

	/* Assert UPLL_CTLREQ */
	WREG32_P(cg_upll_func_cntl, UPLL_CTLREQ_MASK, ~UPLL_CTLREQ_MASK);

	/* Wait for CTLACK and CTLACK2 to get asserted */
	for (i = 0; i < SI_MAX_CTLACKS_ASSERTION_WAIT; ++i) {
		uint32_t mask = UPLL_CTLACK_MASK | UPLL_CTLACK2_MASK;

		if ((RREG32(cg_upll_func_cntl) & mask) == mask)
			break;
		mdelay(10);
	}

	/* Deassert UPLL_CTLREQ */
	WREG32_P(cg_upll_func_cntl, 0, ~UPLL_CTLREQ_MASK);

	if (i == SI_MAX_CTLACKS_ASSERTION_WAIT) {
		frb_err(adev, "gfx: Timeout setting UVD clocks!");
		return -ETIMEDOUT;
	}

	return 0;
}

static unsigned si_uvd_calc_upll_post_div(unsigned vco_freq,
					  unsigned target_freq,
					  unsigned pd_min,
					  unsigned pd_even)
{
	unsigned post_div = vco_freq / target_freq;

	/* Adjust to post divider minimum value */
	if (post_div < pd_min)
		post_div = pd_min;

	/* We alway need a frequency less than or equal the target */
	if ((vco_freq / post_div) > target_freq)
		post_div += 1;

	/* Post dividers above a certain value must be even */
	if (post_div > pd_even && post_div % 2)
		post_div += 1;

	return post_div;
}

static int si_calc_upll_dividers(struct frb_device *adev,
				 unsigned vclk, unsigned dclk,
				 unsigned vco_min, unsigned vco_max,
				 unsigned fb_factor, unsigned fb_mask,
				 unsigned pd_min, unsigned pd_max,
				 unsigned pd_even,
				 unsigned *optimal_fb_div,
				 unsigned *optimal_vclk_div,
				 unsigned *optimal_dclk_div)
{
	unsigned vco_freq, ref_freq = adev->spll_reference_freq;

	/* Start off with something large */
	unsigned optimal_score = ~0;

	/* Loop through vco from low to high */
	if (vco_min < vclk)
		vco_min = vclk;
	if (vco_min < dclk)
		vco_min = dclk;
	for (vco_freq = vco_min; vco_freq <= vco_max; vco_freq += 100) {
		/* 250000 * 16384 still fits 32 bits: no 64-bit division in the kext */
		uint32_t fb_div = vco_freq * fb_factor;
		unsigned vclk_div, dclk_div, score;

		fb_div /= ref_freq;

		/* fb div out of range ? */
		if (fb_div > fb_mask)
			break; /* It can oly get worse */

		fb_div &= fb_mask;

		/* Calc vclk divider with current vco freq */
		vclk_div = si_uvd_calc_upll_post_div(vco_freq, vclk,
						     pd_min, pd_even);
		if (vclk_div > pd_max)
			break; /* vco is too big, it has to stop */

		/* Calc dclk divider with current vco freq */
		dclk_div = si_uvd_calc_upll_post_div(vco_freq, dclk,
						     pd_min, pd_even);
		if (dclk_div > pd_max)
			break; /* vco is too big, it has to stop */

		/* Calc score with current vco freq */
		score = vclk - (vco_freq / vclk_div) + dclk - (vco_freq / dclk_div);

		/* Determine if this vco setting is better than current optimal settings */
		if (score < optimal_score) {
			*optimal_fb_div = fb_div;
			*optimal_vclk_div = vclk_div;
			*optimal_dclk_div = dclk_div;
			optimal_score = score;
			if (optimal_score == 0)
				break; /* It can't get better than this */
		}
	}

	/* Did we found a valid setup ? */
	if (optimal_score == ~0)
		return -EINVAL;

	return 0;
}

int frb_si_set_uvd_clocks(struct frb_device *adev, uint32_t vclk, uint32_t dclk)
{
	unsigned fb_div = 0, vclk_div = 0, dclk_div = 0;
	int r;

	/* Bypass vclk and dclk with bclk */
	WREG32_P(CG_UPLL_FUNC_CNTL_2,
		 VCLK_SRC_SEL(1) | DCLK_SRC_SEL(1),
		 ~(VCLK_SRC_SEL_MASK | DCLK_SRC_SEL_MASK));

	/* Put PLL in bypass mode */
	WREG32_P(CG_UPLL_FUNC_CNTL, UPLL_BYPASS_EN_MASK, ~UPLL_BYPASS_EN_MASK);

	if (!vclk || !dclk) {
		/* Keep the Bypass mode */
		return 0;
	}

	r = si_calc_upll_dividers(adev, vclk, dclk, 125000, 250000,
				  16384, 0x03FFFFFF, 0, 128, 5,
				  &fb_div, &vclk_div, &dclk_div);
	if (r)
		return r;

	/* Set RESET_ANTI_MUX to 0 */
	WREG32_P(CG_UPLL_FUNC_CNTL_5, 0, ~RESET_ANTI_MUX_MASK);

	/* Set VCO_MODE to 1 */
	WREG32_P(CG_UPLL_FUNC_CNTL, UPLL_VCO_MODE_MASK, ~UPLL_VCO_MODE_MASK);

	/* Disable sleep mode */
	WREG32_P(CG_UPLL_FUNC_CNTL, 0, ~UPLL_SLEEP_MASK);

	/* Deassert UPLL_RESET */
	WREG32_P(CG_UPLL_FUNC_CNTL, 0, ~UPLL_RESET_MASK);

	mdelay(1);

	r = si_uvd_send_upll_ctlreq(adev, CG_UPLL_FUNC_CNTL);
	if (r)
		return r;

	/* Assert UPLL_RESET again */
	WREG32_P(CG_UPLL_FUNC_CNTL, UPLL_RESET_MASK, ~UPLL_RESET_MASK);

	/* Disable spread spectrum. */
	WREG32_P(CG_UPLL_SPREAD_SPECTRUM, 0, ~SSEN_MASK);

	/* Set feedback divider */
	WREG32_P(CG_UPLL_FUNC_CNTL_3, UPLL_FB_DIV(fb_div), ~UPLL_FB_DIV_MASK);

	/* Set ref divider to 0 */
	WREG32_P(CG_UPLL_FUNC_CNTL, 0, ~UPLL_REF_DIV_MASK);

	if (fb_div < 307200)
		WREG32_P(CG_UPLL_FUNC_CNTL_4, 0, ~UPLL_SPARE_ISPARE9);
	else
		WREG32_P(CG_UPLL_FUNC_CNTL_4,
			 UPLL_SPARE_ISPARE9,
			 ~UPLL_SPARE_ISPARE9);

	/* Set PDIV_A and PDIV_B */
	WREG32_P(CG_UPLL_FUNC_CNTL_2,
		 UPLL_PDIV_A(vclk_div) | UPLL_PDIV_B(dclk_div),
		 ~(UPLL_PDIV_A_MASK | UPLL_PDIV_B_MASK));

	/* Give the PLL some time to settle */
	mdelay(15);

	/* Deassert PLL_RESET */
	WREG32_P(CG_UPLL_FUNC_CNTL, 0, ~UPLL_RESET_MASK);

	mdelay(15);

	/* Switch from bypass mode to normal mode */
	WREG32_P(CG_UPLL_FUNC_CNTL, 0, ~UPLL_BYPASS_EN_MASK);

	r = si_uvd_send_upll_ctlreq(adev, CG_UPLL_FUNC_CNTL);
	if (r)
		return r;

	/* Switch VCLK and DCLK selection */
	WREG32_P(CG_UPLL_FUNC_CNTL_2,
		 VCLK_SRC_SEL(2) | DCLK_SRC_SEL(2),
		 ~(VCLK_SRC_SEL_MASK | DCLK_SRC_SEL_MASK));

	mdelay(100);

	return 0;
}


/* --- si_read_register (si.c): what user space may read ------------------------ */

static const struct { uint32_t reg_offset; int grbm_indexed; } si_allowed_read_registers[] = {
	{mmGRBM_STATUS},
	{mmGRBM_STATUS2},
	{mmGRBM_STATUS_SE0},
	{mmGRBM_STATUS_SE1},
	{mmSRBM_STATUS},
	{mmSRBM_STATUS2},
	{mmDMA_STATUS_REG + DMA0_REGISTER_OFFSET},
	{mmDMA_STATUS_REG + DMA1_REGISTER_OFFSET},
	{mmCP_STAT},
	{mmCP_STALLED_STAT1},
	{mmCP_STALLED_STAT2},
	{mmCP_STALLED_STAT3},
	{mmGB_ADDR_CONFIG},
	{mmMC_ARB_RAMCFG},
	{mmGB_TILE_MODE0},
	{mmGB_TILE_MODE1},
	{mmGB_TILE_MODE2},
	{mmGB_TILE_MODE3},
	{mmGB_TILE_MODE4},
	{mmGB_TILE_MODE5},
	{mmGB_TILE_MODE6},
	{mmGB_TILE_MODE7},
	{mmGB_TILE_MODE8},
	{mmGB_TILE_MODE9},
	{mmGB_TILE_MODE10},
	{mmGB_TILE_MODE11},
	{mmGB_TILE_MODE12},
	{mmGB_TILE_MODE13},
	{mmGB_TILE_MODE14},
	{mmGB_TILE_MODE15},
	{mmGB_TILE_MODE16},
	{mmGB_TILE_MODE17},
	{mmGB_TILE_MODE18},
	{mmGB_TILE_MODE19},
	{mmGB_TILE_MODE20},
	{mmGB_TILE_MODE21},
	{mmGB_TILE_MODE22},
	{mmGB_TILE_MODE23},
	{mmGB_TILE_MODE24},
	{mmGB_TILE_MODE25},
	{mmGB_TILE_MODE26},
	{mmGB_TILE_MODE27},
	{mmGB_TILE_MODE28},
	{mmGB_TILE_MODE29},
	{mmGB_TILE_MODE30},
	{mmGB_TILE_MODE31},
	{mmCC_RB_BACKEND_DISABLE, 1},
	{mmGC_USER_RB_BACKEND_DISABLE, 1},
	{mmPA_SC_RASTER_CONFIG, 1},
};

/*
 * si_get_register_value: gfx_v6_0's copies where it keeps them (the RB
 * registers per SE/SH, GB_ADDR_CONFIG, MC_ARB_RAMCFG, the tiling table;
 * GB_ADDR_CONFIG itself reads back 0x20000000 on GCN, RadeonNI's
 * accel-state notes), the register otherwise. Every indexed register on the
 * list is one gfx_v6_0 keeps, so GRBM_GFX_INDEX is never touched here.
 */
static int si_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			    uint32_t *value)
{
	uint32_t i;

	*value = 0;
	for (i = 0; i < ARRAY_SIZE(si_allowed_read_registers); i++) {
		int indexed = si_allowed_read_registers[i].grbm_indexed;

		if (reg_offset != si_allowed_read_registers[i].reg_offset)
			continue;
		if (frb_gfx_v6_0_get_register_value(adev, indexed ? se_num : 0xffffffff,
						    indexed ? sh_num : 0xffffffff, reg_offset, value))
			*value = RREG32(reg_offset);
		return 0;
	}
	return -EINVAL;
}

/*
 * Stage sdma on SI: si_dma's hw_init, then what amdgpu_device_ip_late_init
 * does with the blocks up: amdgpu_device_set_cg_state(GATE) in IP-block
 * order (si_common: nothing; gmc_v6_0: MC MGCG/LS, BIF LS, HDP MGCG/LS;
 * gfx_v6_0: MGCG on, CGCG off, as Verde's cg_flags have no GFX_CGCG;
 * si_dma: MGCG). Without it the golden registers leave RLC_CGCG_CGLS_CTRL's
 * CGCG/CGLS on with no RLC set-up for it, which RadeonNI found holds the
 * SMC at its top level and darkens the CG block (its accel-state notes, "SI
 * DPM"). frb_cg=0 leaves the gating as hw_init left it.
 */
static int si_sdma_stage(struct frb_device *adev)
{
	int r = frb_si_dma_init(adev);

	if (r || adev->no_cg)
		return r;
	frb_gmc_v6_0_set_clockgating_state(adev, 1);
	frb_gfx_v6_0_set_clockgating_state(adev, 1);
	frb_si_dma_set_clockgating_state(adev, 1);
	adev->cg_on = 1;
	frb_info(adev, "si: clock gating (cg_flags 0x%llx): RLC_CGTT_MGCG_OVERRIDE %08x, RLC_CGCG_CGLS_CTRL %08x, "
		 "HDP_HOST_PATH_CNTL %08x, DMA0_CLK_CTRL %08x", (unsigned long long)adev->cg_flags,
		 RREG32(mmRLC_CGTT_MGCG_OVERRIDE), RREG32(mmRLC_CGCG_CGLS_CTRL), RREG32(mmHDP_HOST_PATH_CNTL),
		 RREG32(mmDMA_CLK_CTRL));
	return 0;
}

/* --- si_set_ip_blocks (Verde, Tahiti, Pitcairn): the blocks Firebrand has --- */

const struct frb_asic_funcs frb_si_asic_funcs = {
	.name = "si",
	.rmmio_bar = 0x18,		/* BAR2 (amdgpu_device_init: BAR5 from Bonaire on) */
	.family = AMDGPU_FAMILY_SI,
	.pcie_index = mmPCIE_INDEX,	/* 0xc, sid.h AMDGPU_PCIE_INDEX */
	.pcie_data = mmPCIE_DATA,
	.early_init = si_common_early_init,
	.smc_rreg = si_smc_rreg,
	.smc_wreg = si_smc_wreg,
	.get_config_memsize = si_get_config_memsize,
	.need_reset_on_init = si_need_reset_on_init,
	.asic_reset = si_asic_reset,
	.common_hw_init = si_common_hw_init,
	.gmc_early_init = NULL,
	.gmc_sw_init = frb_gmc_v6_0_sw_init,
	.gmc_hw_init = frb_gmc_v6_0_hw_init,
	.smu_start = frb_si_smc_stage,
	.display = &frb_dce_v6_0_funcs,
	.gfx_init = frb_gfx_v6_0_init,
	.sdma_init = si_sdma_stage,
	.uvd_init = frb_uvd_v3_1_init,
	.read_register = si_read_register,
	.num_crtc = 6,			/* dce_v6_0_get_num_crtc: Tahiti/Pitcairn/Verde */
};
