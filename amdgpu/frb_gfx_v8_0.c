/*
 * gfx_v8_0.c (Linux 7.0.2), for Polaris 12: the GFX 8 block's sw_init and
 * hw_init up to the gfx ring test, the IB test, and hw_fini's halt.
 *
 * What is ported, in amdgpu's order:
 *   sw_init:  gfx_v8_0_init_microcode (the RLC v2.0 header and its lists),
 *             gfx_v8_0_rlc_init (the clear-state block, update_spm_vmid),
 *             the gfx ring (amdgpu_ring_init), gfx_v8_0_gpu_early_init
 *   hw_init:  gfx_v8_0_init_golden_registers, gfx_v8_0_constants_init
 *             (tiling table, setup_rb, cu info, SH_MEM per VMID, compute and
 *             GDS VMIDs, PA_SC_FIFO_SIZE, SPI_ARB_PRIORITY),
 *             gfx_v8_0_rlc_resume (stop, reset, init_pg: CSB, save/restore
 *             list, save/restore machine, power gating; start),
 *             gfx_v8_0_cp_resume's gfx part (cp_gfx_resume, cp_gfx_start,
 *             the gfx ring test, the GUI idle interrupt)
 *   late:     gfx_v8_0_ring_test_ib (amdgpu_ib_ring_tests runs it from
 *             late_init; here frbctl test gfx ib)
 *   hw_fini:  RLC safe mode, CP halt, RLC stop
 * The CP, RLC and MEC microcode is loaded by the SMU (stage smu), as on Linux.
 *
 * NOT PORTED (yet): the KIQ and the compute queues (kiq_resume, kcq_resume,
 * mec_init, the MQDs, kcq_disable in hw_fini), doorbells, interrupts (IH,
 * the EOP/privileged-fault sources), clock and power gating beyond what
 * hw_init itself writes (set_clockgating_state, set_powergating_state run
 * from late_init), the EDC GPR workarounds (Carrizo only), SR-IOV, and the
 * other VI chips' arms.
 *
 * Where Firebrand differs from amdgpu, and why:
 * - PLACEMENT. The ring, its writeback slots, the IB pool and the clear-state
 *   block live in VRAM (frb_ring.c's objects region), not GTT, until the
 *   GART carries system pages: CP_RB0_BASE, CP_RB0_RPTR_ADDR(_HI),
 *   CP_RB_WPTR_POLL_ADDR_LO/HI and RLC_CSIB_ADDR_LO/HI carry different
 *   addresses (Linux's CSB was in VRAM too). tests/frb_replay matches them
 *   by register only (--any). As for every structure the CPU fills through
 *   BAR0 for the GPU, the CSB fill is followed by an HDP flush (frb_ring.h).
 * - NO DOORBELL. The gfx ring takes Linux's use_doorbell = false arm:
 *   CP_RB_DOORBELL_CONTROL gets DOORBELL_EN = 0, and the write pointer goes
 *   to CP_RB0_WPTR by MMIO (gfx_v8_0_ring_set_wptr_gfx's register arm), where
 *   Linux rang the doorbell (not in its register trace; --extra in the replay).
 * - BYTE ORDER. Ring, IB, CSB and writeback are little-endian dwords
 *   (frb_ring_write, frb_ib_write, frb_vram_write32), so the GPU swaps
 *   nothing: CP_RB0_CNTL.BUF_SWAP and the IB packet's swap bits stay 0, where
 *   Linux sets them under __BIG_ENDIAN. The register values are then those of
 *   the little-endian capture (docs/byte-order.md). It is also the only
 *   choice: on the E9173 neither Linux's __BIG_ENDIAN settings nor
 *   CP_ENDIAN_SWAP make the CP fetch a ring or IB in the CPU's order
 *   (gfx_v8_0_ib_probe, cp_gfx_resume), so user command buffers are
 *   little-endian too (the accelerator's clients swap them).
 * - The IB test's ring stream is Linux's minus COND_EXEC (always "continue"
 *   for a kernel ring; it exists for preemption) and the in-ring HDP_DEBUG0
 *   write (vi_invalidate_hdp's ring arm): the CPU invalidates the HDP by MMIO
 *   before every writeback read instead (frb_wb_read).
 *
 * Copyright 2014 Advanced Micro Devices, Inc.
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
#include "frb_ring.h"
#include "frb_ucode.h"
#include "../shared/atom/atombios.h"
#include "include/vid.h"
#include "include/clearstate_defs.h"
#include "include/clearstate_vi.h"

#include "include/asic_reg/gmc/gmc_8_1_d.h"
#include "include/asic_reg/gmc/gmc_8_1_sh_mask.h"
#include "include/asic_reg/oss/oss_3_0_d.h"
#include "include/asic_reg/oss/oss_3_0_sh_mask.h"
#include "include/asic_reg/bif/bif_5_0_d.h"
#include "include/asic_reg/bif/bif_5_0_sh_mask.h"
#include "include/asic_reg/gca/gfx_8_0_d.h"
#include "include/asic_reg/gca/gfx_8_0_enum.h"
#include "include/asic_reg/gca/gfx_8_0_sh_mask.h"
#include "include/asic_reg/dce/dce_10_0_d.h"

#define POLARIS11_GB_ADDR_CONFIG_GOLDEN 0x22011002

#define ARRAY_MODE(x)					((x) << GB_TILE_MODE0__ARRAY_MODE__SHIFT)
#define PIPE_CONFIG(x)					((x) << GB_TILE_MODE0__PIPE_CONFIG__SHIFT)
#define TILE_SPLIT(x)					((x) << GB_TILE_MODE0__TILE_SPLIT__SHIFT)
#define MICRO_TILE_MODE_NEW(x)				((x) << GB_TILE_MODE0__MICRO_TILE_MODE_NEW__SHIFT)
#define SAMPLE_SPLIT(x)					((x) << GB_TILE_MODE0__SAMPLE_SPLIT__SHIFT)
#define BANK_WIDTH(x)					((x) << GB_MACROTILE_MODE0__BANK_WIDTH__SHIFT)
#define BANK_HEIGHT(x)					((x) << GB_MACROTILE_MODE0__BANK_HEIGHT__SHIFT)
#define MACRO_TILE_ASPECT(x)				((x) << GB_MACROTILE_MODE0__MACRO_TILE_ASPECT__SHIFT)
#define NUM_BANKS(x)					((x) << GB_MACROTILE_MODE0__NUM_BANKS__SHIFT)

#define RLC_FormatDirectRegListLength        14

/* amdgpu.h */
#define WREG32_FIELD(reg, field, val)	\
	WREG32(mm##reg, (RREG32(mm##reg) & ~REG_FIELD_MASK(reg, field)) | (val) << REG_FIELD_SHIFT(reg, field))
#ifndef lower_32_bits
#define lower_32_bits(n)	((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n)	((uint32_t)(((n) >> 16) >> 16))
#endif

/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_IB_FLAG_CE	(1 << 0)
/* amdgpu_ids.h */
#define AMDGPU_NUM_VMID		16
/* amdgpu_doorbell.h: AMDGPU_DOORBELL_GFX_RING0, vi.c's doorbell_index.gfx_ring0 */
#define AMDGPU_DOORBELL_GFX_RING0	0x020

/*
 * What other blocks set up and this one reads, with where amdgpu sets it:
 * - gmc_v8_0_sw_init: first_kfd_vmid = 8; amdgpu_vmid_mgr_init gives the
 *   gfx hub first_kfd_vmid ids; shared_aperture_start = 0x2000000000000000.
 * - vi_common_early_init, CHIP_POLARIS12: cg_flags include GFX_MGCG, GFX_CGCG
 *   and GFX_3D_CGCG (so amdgpu_gfx_rlc_enter_safe_mode takes its
 *   set_safe_mode arm in hw_fini); not an APU.
 * - amdgpu_gfx_parse_disable_cu: the disable_cu parameter is unset (the
 *   capture's amdgpu-params.txt), so every mask is 0.
 */
#define FRB_FIRST_KFD_VMID		8
#define FRB_GFXHUB_NUM_IDS		FRB_FIRST_KFD_VMID
#define FRB_SHARED_APERTURE_START	0x2000000000000000ULL
#define FRB_CG_GFX_SAFE_MODE		1

/* amdgpu_gfx.h, the parts this block keeps */
#define AMDGPU_GFX_MAX_SE		4
#define AMDGPU_GFX_MAX_SH_PER_SE	2

struct frb_rb_config {
	uint32_t rb_backend_disable;
	uint32_t user_rb_backend_disable;
	uint32_t raster_config;
	uint32_t raster_config_1;
};

struct frb_gfx_config {
	uint32_t max_shader_engines;
	uint32_t max_tile_pipes;
	uint32_t max_cu_per_sh;
	uint32_t max_sh_per_se;
	uint32_t max_backends_per_se;
	uint32_t max_texture_channel_caches;
	uint32_t max_gprs;
	uint32_t max_gs_threads;
	uint32_t max_hw_contexts;
	uint32_t sc_prim_fifo_size_frontend;
	uint32_t sc_prim_fifo_size_backend;
	uint32_t sc_hiz_tile_fifo_size;
	uint32_t sc_earlyz_tile_fifo_size;

	uint32_t num_tile_pipes;
	uint32_t backend_enable_mask;
	uint32_t mem_max_burst_length_bytes;
	uint32_t mem_row_size_in_kb;
	uint32_t shader_engine_tile_size;
	uint32_t num_gpus;
	uint32_t multi_gpu_tile_size;
	uint32_t mc_arb_ramcfg;
	uint32_t num_banks;
	uint32_t num_ranks;
	uint32_t gb_addr_config;
	uint32_t num_rbs;
	uint32_t double_offchip_lds_buf;
	uint32_t tile_mode_array[32];
	uint32_t macrotile_mode_array[16];
	struct frb_rb_config rb_config[AMDGPU_GFX_MAX_SE][AMDGPU_GFX_MAX_SH_PER_SE];
};

struct frb_cu_info {
	uint32_t number;
	uint32_t ao_cu_mask;
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t wave_front_size;
	uint32_t max_scratch_slots_per_cu;
	uint32_t lds_size;
	uint32_t bitmap[4][4];
	uint32_t ao_cu_bitmap[4][2];
};

/* amdgpu_rlc.h, the fields gfx_v8_0 uses on Polaris */
struct frb_rlc {
	const struct cs_section_def *cs_data;
	uint32_t clear_state_size;		/* dwords */
	uint32_t clear_state_offset;		/* the CSB, as a BAR0 (VRAM) offset */
	uint64_t clear_state_gpu_addr;

	uint32_t fw_version, feature_version;
	uint32_t save_and_restore_offset;
	uint32_t clear_state_descriptor_offset;
	uint32_t avail_scratch_ram_locations;
	uint32_t reg_restore_list_size;
	uint32_t reg_list_format_start;
	uint32_t reg_list_format_separate_start;
	uint32_t starting_offsets_start;
	uint32_t reg_list_format_size_bytes;
	uint32_t reg_list_size_bytes;
	uint32_t *register_list_format;		/* CPU order, from the LE file */
	uint32_t *register_restore;
	uint32_t lists_bytes;			/* the allocation behind both */
	int in_safe_mode;
};

/* adev->gfx, the Polaris 12 subset */
struct frb_gfx {
	struct frb_gfx_config config;
	struct frb_cu_info cu_info;
	struct frb_rlc rlc;
	struct frb_ring gfx_ring;
	uint32_t ce_ram_size;
	int sw_done;		/* sw_init completed */
	int hw_started;		/* the RLC/CP were touched: hw_fini must halt them */
	uint32_t grbm_soft_reset, srbm_soft_reset;	/* check_soft_reset's verdict */
};

struct amdgpu_gds_reg_offset {
	uint32_t mem_base;
	uint32_t mem_size;
	uint32_t gws;
	uint32_t oa;
};

static const struct amdgpu_gds_reg_offset amdgpu_gds_reg_offset[] =
{
	{mmGDS_VMID0_BASE, mmGDS_VMID0_SIZE, mmGDS_GWS_VMID0, mmGDS_OA_VMID0},
	{mmGDS_VMID1_BASE, mmGDS_VMID1_SIZE, mmGDS_GWS_VMID1, mmGDS_OA_VMID1},
	{mmGDS_VMID2_BASE, mmGDS_VMID2_SIZE, mmGDS_GWS_VMID2, mmGDS_OA_VMID2},
	{mmGDS_VMID3_BASE, mmGDS_VMID3_SIZE, mmGDS_GWS_VMID3, mmGDS_OA_VMID3},
	{mmGDS_VMID4_BASE, mmGDS_VMID4_SIZE, mmGDS_GWS_VMID4, mmGDS_OA_VMID4},
	{mmGDS_VMID5_BASE, mmGDS_VMID5_SIZE, mmGDS_GWS_VMID5, mmGDS_OA_VMID5},
	{mmGDS_VMID6_BASE, mmGDS_VMID6_SIZE, mmGDS_GWS_VMID6, mmGDS_OA_VMID6},
	{mmGDS_VMID7_BASE, mmGDS_VMID7_SIZE, mmGDS_GWS_VMID7, mmGDS_OA_VMID7},
	{mmGDS_VMID8_BASE, mmGDS_VMID8_SIZE, mmGDS_GWS_VMID8, mmGDS_OA_VMID8},
	{mmGDS_VMID9_BASE, mmGDS_VMID9_SIZE, mmGDS_GWS_VMID9, mmGDS_OA_VMID9},
	{mmGDS_VMID10_BASE, mmGDS_VMID10_SIZE, mmGDS_GWS_VMID10, mmGDS_OA_VMID10},
	{mmGDS_VMID11_BASE, mmGDS_VMID11_SIZE, mmGDS_GWS_VMID11, mmGDS_OA_VMID11},
	{mmGDS_VMID12_BASE, mmGDS_VMID12_SIZE, mmGDS_GWS_VMID12, mmGDS_OA_VMID12},
	{mmGDS_VMID13_BASE, mmGDS_VMID13_SIZE, mmGDS_GWS_VMID13, mmGDS_OA_VMID13},
	{mmGDS_VMID14_BASE, mmGDS_VMID14_SIZE, mmGDS_GWS_VMID14, mmGDS_OA_VMID14},
	{mmGDS_VMID15_BASE, mmGDS_VMID15_SIZE, mmGDS_GWS_VMID15, mmGDS_OA_VMID15}
};

static const uint32_t golden_settings_polaris11_a11[] =
{
	mmCB_HW_CONTROL, 0x0000f3cf, 0x00007208,
	mmCB_HW_CONTROL_2, 0x0f000000, 0x0f000000,
	mmCB_HW_CONTROL_3, 0x000001ff, 0x00000040,
	mmDB_DEBUG2, 0xf00fffff, 0x00000400,
	mmPA_SC_ENHANCE, 0xffffffff, 0x20000001,
	mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000,
	mmPA_SC_RASTER_CONFIG, 0x3f3fffff, 0x16000012,
	mmPA_SC_RASTER_CONFIG_1, 0x0000003f, 0x00000000,
	mmRLC_CGCG_CGLS_CTRL, 0x00000003, 0x0001003c,
	mmRLC_CGCG_CGLS_CTRL_3D, 0xffffffff, 0x0001003c,
	mmSQ_CONFIG, 0x07f80000, 0x01180000,
	mmTA_CNTL_AUX, 0x000f000f, 0x000b0000,
	mmTCC_CTRL, 0x00100000, 0xf31fff7f,
	mmTCP_ADDR_CONFIG, 0x000003ff, 0x000000f3,
	mmTCP_CHAN_STEER_HI, 0xffffffff, 0x00000000,
	mmTCP_CHAN_STEER_LO, 0xffffffff, 0x00003210,
	mmVGT_RESET_DEBUG, 0x00000004, 0x00000004,
};

static const uint32_t polaris11_golden_common_all[] =
{
	mmGRBM_GFX_INDEX, 0xffffffff, 0xe0000000,
	mmGB_ADDR_CONFIG, 0xffffffff, 0x22011002,
	mmSPI_RESOURCE_RESERVE_CU_0, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_CU_1, 0xffffffff, 0x00000800,
	mmSPI_RESOURCE_RESERVE_EN_CU_0, 0xffffffff, 0x00FF7FBF,
	mmSPI_RESOURCE_RESERVE_EN_CU_1, 0xffffffff, 0x00FF7FAF,
};

static void gfx_v8_0_select_se_sh(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				  uint32_t instance, int xcc_id);
static void gfx_v8_0_get_cu_info(struct frb_device *adev);
static uint32_t gfx_v8_0_get_csb_size(struct frb_device *adev);
static const struct frb_ring_funcs gfx_v8_0_ring_funcs_gfx;

static uint32_t hweight32(uint32_t w)
{
	uint32_t n = 0;

	for (; w; w &= w - 1)
		n++;
	return n;
}

/* include/linux/log2.h order_base_2: log2, rounded up */
static uint32_t order_base_2(uint32_t n)
{
	uint32_t r = 0;

	while ((1u << r) < n)
		r++;
	return r;
}

/* amdgpu_gfx.h amdgpu_gfx_create_bitmask */
static uint32_t amdgpu_gfx_create_bitmask(uint32_t bit_width)
{
	return (uint32_t)((1ULL << bit_width) - 1);
}

/* vi.c vi_srbm_select (here until another ported block needs it) */
static void vi_srbm_select(struct frb_device *adev, uint32_t me, uint32_t pipe,
			   uint32_t queue, uint32_t vmid)
{
	uint32_t srbm_gfx_cntl = 0;

	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, PIPEID, pipe);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, MEID, me);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, VMID, vmid);
	srbm_gfx_cntl = REG_SET_FIELD(srbm_gfx_cntl, SRBM_GFX_CNTL, QUEUEID, queue);
	WREG32(mmSRBM_GFX_CNTL, srbm_gfx_cntl);
}

/*
 * amdgpu_atombios.c amdgpu_atombios_get_gfx_info: GFX_Info (ATOM_GFX_INFO_V2_1),
 * byte fields at their offsetof() positions. Here because only this block
 * reads it.
 */
static int amdgpu_atombios_get_gfx_info(struct frb_device *adev)
{
	struct atom_context *ctx = adev->atom;
	struct frb_gfx_config *cfg = &adev->gfx->config;
	int index = GetIndexIntoMasterTable(DATA, GFX_Info);
	uint8_t frev, crev;
	uint16_t off;

	if (!ctx || !frb_atom_parse_data_header(ctx, index, NULL, &frev, &crev, &off))
		return -EINVAL;
	cfg->max_shader_engines = frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_shader_engines));
	cfg->max_tile_pipes = frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_tile_pipes));
	cfg->max_cu_per_sh = frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_cu_per_sh));
	cfg->max_sh_per_se = frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_sh_per_se));
	cfg->max_backends_per_se = frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_backends_per_se));
	cfg->max_texture_channel_caches =
		frb_get_u8(ctx->bios, off + offsetof(ATOM_GFX_INFO_V2_1, max_texture_channel_caches));
	frb_info(adev, "gfx: GFX_Info %u.%u: %u SE, %u SH/SE, %u CU/SH, %u RB/SE, %u tile pipes, %u TCCs",
		 frev, crev, cfg->max_shader_engines, cfg->max_sh_per_se, cfg->max_cu_per_sh,
		 cfg->max_backends_per_se, cfg->max_tile_pipes, cfg->max_texture_channel_caches);
	if (!cfg->max_shader_engines || !cfg->max_sh_per_se || cfg->max_shader_engines > AMDGPU_GFX_MAX_SE ||
	    cfg->max_sh_per_se > AMDGPU_GFX_MAX_SH_PER_SE || cfg->max_cu_per_sh > 16)
		return -EINVAL;
	return 0;
}

static void gfx_v8_0_init_golden_registers(struct frb_device *adev)
{
	switch (adev->asic_type) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		frb_device_program_register_sequence(adev,
						     golden_settings_polaris11_a11,
						     ARRAY_SIZE(golden_settings_polaris11_a11));
		frb_device_program_register_sequence(adev,
						     polaris11_golden_common_all,
						     ARRAY_SIZE(polaris11_golden_common_all));
		break;
	default:
		break;	/* the other VI arms are not carried */
	}
}

static int gfx_v8_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);
	r = frb_ring_alloc(ring, 3);
	if (r)
		return r;

	frb_ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	frb_ring_write(ring, mmSCRATCH_REG0 - PACKET3_SET_UCONFIG_REG_START);
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(mmSCRATCH_REG0);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;

	if (r)
		frb_err(adev, "gfx: ring test FAILED: SCRATCH_REG0 %08x after %u polls (wptr %u)",
			tmp, i, (uint32_t)ring->wptr);
	else
		frb_info(adev, "gfx: ring test passed: SCRATCH_REG0 deadbeef after %u polls (wptr %u)",
			 i, (uint32_t)ring->wptr);
	return r;
}

static int gfx_v8_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t index, seq = 0;
	uint64_t gpu_addr;
	uint32_t tmp;
	int r;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;

	gpu_addr = frb_wb_gpu_addr(adev, index);
	frb_wb_write(adev, index, 0xCAFEDEAD);

	r = frb_ib_get(adev, 20, &ib);
	if (r)
		goto err1;

	frb_ib_write(adev, &ib, PACKET3(PACKET3_WRITE_DATA, 3));
	frb_ib_write(adev, &ib, WRITE_DATA_DST_SEL(5) | WR_CONFIRM);
	frb_ib_write(adev, &ib, lower_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, upper_32_bits(gpu_addr));
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	ib.length_dw = 5;

	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto err1;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		frb_err(adev, "gfx: IB test FAILED: fence %u not signalled (reads %u) in %u us; "
			"rptr %u wptr %u, writeback %08x",
			seq, frb_fence_read(ring), timeout_us, (uint32_t)ring->funcs->get_rptr(ring),
			(uint32_t)ring->funcs->get_wptr(ring), frb_wb_read(adev, index));
		goto err1;
	}

	tmp = frb_wb_read(adev, index);
	if (tmp == 0xDEADBEEF)
		r = 0;
	else
		r = -EINVAL;
	if (r)
		frb_err(adev, "gfx: IB test FAILED: fence %u signalled, writeback %08x", seq, tmp);
	else
		frb_info(adev, "gfx: IB test passed: fence %u signalled, writeback deadbeef at %llx "
			 "(IB at %llx, rptr %u)", seq, (unsigned long long)gpu_addr,
			 (unsigned long long)ib.gpu_addr, (uint32_t)ring->funcs->get_rptr(ring));
err1:
	frb_wb_free(adev, index);
	return r;
}

/*
 * gfx_v8_0_init_microcode, the RLC part: the v2.0 header's save/restore
 * fields and its two lists, read little-endian from the file. The CE, PFP,
 * ME and MEC images are the SMU's (stage smu); nothing here needs their
 * versions.
 */
static int gfx_v8_0_init_microcode(struct frb_device *adev)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;
	struct frb_fw fw;
	struct frb_ucode u;
	char name[32];
	uint32_t i, fmt_off, list_off, bytes;
	int err;

	if (adev->asic_type != CHIP_POLARIS12)
		return -EINVAL;	/* chip_name: only polaris12 is carried */
	frb_fw_name(name, sizeof(name), adev->row ? adev->row->fw_prefix : "polaris12", "_rlc.bin");
	err = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (err) {
		frb_err(adev, "gfx: firmware %s missing (%d)", name, err);
		return err;
	}
	err = frb_ucode_parse(adev, name, &fw, &u);
	if (err)
		return err;
	if (u.header_major != 2 || u.header_size_bytes < 100) {
		frb_err(adev, "gfx: %s: header v%u.%u, %u bytes; rlc_firmware_header_v2_0 expected",
			name, u.header_major, u.header_minor, u.header_size_bytes);
		return -EINVAL;
	}
	/* struct rlc_firmware_header_v2_0, byte offsets after the common header */
	rlc->fw_version = u.ucode_version;
	rlc->feature_version = frb_ucode_le32(&u, 32);
	rlc->save_and_restore_offset = frb_ucode_le32(&u, 44);
	rlc->clear_state_descriptor_offset = frb_ucode_le32(&u, 48);
	rlc->avail_scratch_ram_locations = frb_ucode_le32(&u, 52);
	rlc->reg_restore_list_size = frb_ucode_le32(&u, 56);
	rlc->reg_list_format_start = frb_ucode_le32(&u, 60);
	rlc->reg_list_format_separate_start = frb_ucode_le32(&u, 64);
	rlc->starting_offsets_start = frb_ucode_le32(&u, 68);
	rlc->reg_list_format_size_bytes = frb_ucode_le32(&u, 72);
	fmt_off = frb_ucode_le32(&u, 76);		/* reg_list_format_array_offset_bytes */
	rlc->reg_list_size_bytes = frb_ucode_le32(&u, 80);
	list_off = frb_ucode_le32(&u, 84);		/* reg_list_array_offset_bytes */

	rlc->reg_list_format_size_bytes &= ~3u;
	rlc->reg_list_size_bytes &= ~3u;
	if ((rlc->reg_list_format_size_bytes &&
	     (fmt_off > fw.size || rlc->reg_list_format_size_bytes > fw.size - fmt_off)) ||
	    (rlc->reg_list_size_bytes &&
	     (list_off > fw.size || rlc->reg_list_size_bytes > fw.size - list_off))) {
		frb_err(adev, "gfx: %s: the save/restore lists do not fit the file", name);
		return -EINVAL;
	}

	bytes = rlc->reg_list_format_size_bytes + rlc->reg_list_size_bytes;
	rlc->lists_bytes = bytes ? bytes : 4;
	rlc->register_list_format = (uint32_t *)adev->os->alloc(adev->os->cookie, rlc->lists_bytes);
	if (!rlc->register_list_format)
		return -ENOMEM;
	for (i = 0; i < (rlc->reg_list_format_size_bytes >> 2); i++)
		rlc->register_list_format[i] = frb_get_le32(fw.data, fmt_off + 4 * i);
	rlc->register_restore = rlc->register_list_format + i;
	for (i = 0; i < (rlc->reg_list_size_bytes >> 2); i++)
		rlc->register_restore[i] = frb_get_le32(fw.data, list_off + 4 * i);

	frb_info(adev, "gfx: %s: feature %u; save/restore list %u bytes, format list %u bytes, "
		 "restore size at %u, format at %u, offsets at %u", name, rlc->feature_version,
		 rlc->reg_list_size_bytes, rlc->reg_list_format_size_bytes,
		 rlc->reg_restore_list_size, rlc->reg_list_format_start, rlc->starting_offsets_start);
	return 0;
}

/*
 * gfx_v8_0_get_csb_buffer (amdgpu_gfx_csb_preamble_start, _data_parser,
 * _preamble_end), written straight into the VRAM clear-state block, one
 * little-endian dword at a time (Linux: cpu_to_le32 into the BO).
 */
static void gfx_v8_0_get_csb_buffer(struct frb_device *adev, uint32_t offset)
{
	struct frb_gfx *g = adev->gfx;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	uint32_t count = 0, i;

#define CSB_PUT(v)	frb_vram_write32(adev, offset + 4 * count++, (v))
	if (g->rlc.cs_data == NULL)
		return;

	/* amdgpu_gfx_csb_preamble_start */
	CSB_PUT(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	CSB_PUT(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);
	CSB_PUT(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	CSB_PUT(0x80000000);
	CSB_PUT(0x80000000);

	/* amdgpu_gfx_csb_data_parser */
	for (sect = g->rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				CSB_PUT(PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				CSB_PUT(ext->reg_index - PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					CSB_PUT(ext->extent[i]);
			}
		}
	}

	CSB_PUT(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	CSB_PUT(mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	CSB_PUT(g->config.rb_config[0][0].raster_config);
	CSB_PUT(g->config.rb_config[0][0].raster_config_1);

	/* amdgpu_gfx_csb_preamble_end */
	CSB_PUT(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	CSB_PUT(PACKET3_PREAMBLE_END_CLEAR_STATE);
	CSB_PUT(PACKET3(PACKET3_CLEAR_STATE, 0));
	CSB_PUT(0);
#undef CSB_PUT
	if (count != g->rlc.clear_state_size)
		frb_err(adev, "gfx: clear-state block: %u dwords written, %u sized", count,
			g->rlc.clear_state_size);
}

/* gfx_v8_0_update_spm_vmid (amdgpu_gfx_off_ctrl is a no-op without GFXOFF) */
static void gfx_v8_0_update_spm_vmid(struct frb_device *adev, unsigned vmid)
{
	uint32_t data;

	data = RREG32(mmRLC_SPM_VMID);
	data &= ~RLC_SPM_VMID__RLC_SPM_VMID_MASK;
	data |= (vmid & RLC_SPM_VMID__RLC_SPM_VMID_MASK) << RLC_SPM_VMID__RLC_SPM_VMID__SHIFT;
	WREG32(mmRLC_SPM_VMID, data);
}

/*
 * gfx_v8_0_rlc_init: amdgpu_gfx_rlc_init_csb's clear-state block (VRAM in
 * amdgpu too), then the SPM VMID. No CP table on Polaris.
 */
static int gfx_v8_0_rlc_init(struct frb_device *adev)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;
	int r;

	rlc->cs_data = vi_cs_data;

	if (rlc->cs_data) {
		/* amdgpu_gfx_rlc_init_csb */
		rlc->clear_state_size = gfx_v8_0_get_csb_size(adev);
		r = frb_vram_obj_alloc(adev, rlc->clear_state_size * 4, AMDGPU_GPU_PAGE_SIZE,
				       &rlc->clear_state_offset);
		if (r) {
			frb_err(adev, "gfx: (%d) failed to create rlc csb bo", r);
			return r;
		}
		rlc->clear_state_gpu_addr = frb_vram_gpu_addr(adev, rlc->clear_state_offset);
	}

	/* init spm vmid with 0xf */
	gfx_v8_0_update_spm_vmid(adev, 0xf);

	return 0;
}

static int gfx_v8_0_gpu_early_init(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t gb_addr_config;
	uint32_t mc_arb_ramcfg;
	uint32_t tmp;
	int ret;

	switch (adev->asic_type) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		ret = amdgpu_atombios_get_gfx_info(adev);
		if (ret)
			return ret;
		cfg->max_gprs = 256;
		cfg->max_gs_threads = 32;
		cfg->max_hw_contexts = 8;

		cfg->sc_prim_fifo_size_frontend = 0x20;
		cfg->sc_prim_fifo_size_backend = 0x100;
		cfg->sc_hiz_tile_fifo_size = 0x30;
		cfg->sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = POLARIS11_GB_ADDR_CONFIG_GOLDEN;
		break;
	default:
		return -EINVAL;	/* the other VI arms are not carried */
	}

	cfg->mc_arb_ramcfg = RREG32(mmMC_ARB_RAMCFG);
	mc_arb_ramcfg = cfg->mc_arb_ramcfg;

	cfg->num_banks = REG_GET_FIELD(mc_arb_ramcfg, MC_ARB_RAMCFG, NOOFBANK);
	cfg->num_ranks = REG_GET_FIELD(mc_arb_ramcfg, MC_ARB_RAMCFG, NOOFRANKS);

	cfg->num_tile_pipes = cfg->max_tile_pipes;
	cfg->mem_max_burst_length_bytes = 256;
	/* not an APU */
	tmp = REG_GET_FIELD(mc_arb_ramcfg, MC_ARB_RAMCFG, NOOFCOLS);
	cfg->mem_row_size_in_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (cfg->mem_row_size_in_kb > 4)
		cfg->mem_row_size_in_kb = 4;

	cfg->shader_engine_tile_size = 32;
	cfg->num_gpus = 1;
	cfg->multi_gpu_tile_size = 64;

	/* fix up row size */
	switch (cfg->mem_row_size_in_kb) {
	case 1:
	default:
		gb_addr_config = REG_SET_FIELD(gb_addr_config, GB_ADDR_CONFIG, ROW_SIZE, 0);
		break;
	case 2:
		gb_addr_config = REG_SET_FIELD(gb_addr_config, GB_ADDR_CONFIG, ROW_SIZE, 1);
		break;
	case 4:
		gb_addr_config = REG_SET_FIELD(gb_addr_config, GB_ADDR_CONFIG, ROW_SIZE, 2);
		break;
	}
	cfg->gb_addr_config = gb_addr_config;

	return 0;
}

/*
 * gfx_v8_0_sw_init, the gfx-ring subset: no IRQ sources (no IH yet), no
 * MEC/KIQ/compute rings/MQDs.
 */
static int gfx_v8_0_sw_init(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	struct frb_ring *ring;
	int r;

	r = gfx_v8_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to load gfx firmware!");
		return r;
	}

	r = gfx_v8_0_rlc_init(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to init rlc BOs!");
		return r;
	}

	/* set up the gfx ring: no doorbell (Linux's use_doorbell = false arm) */
	ring = &g->gfx_ring;
	r = frb_ring_init(adev, ring, "gfx", &gfx_v8_0_ring_funcs_gfx, 1024);
	if (r)
		return r;
	frb_ring_clear(ring);	/* amdgpu_ring_init clears a new ring */

	g->ce_ram_size = 0x8000;

	r = gfx_v8_0_gpu_early_init(adev);
	if (r)
		return r;

	g->sw_done = 1;
	return 0;
}

static void gfx_v8_0_tiling_mode_table_init(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t *modearray, *mod2array;
	const uint32_t num_tile_mode_states = ARRAY_SIZE(cfg->tile_mode_array);
	const uint32_t num_secondary_tile_mode_states = ARRAY_SIZE(cfg->macrotile_mode_array);
	uint32_t reg_offset;

	modearray = cfg->tile_mode_array;
	mod2array = cfg->macrotile_mode_array;

	for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
		modearray[reg_offset] = 0;

	for (reg_offset = 0; reg_offset <  num_secondary_tile_mode_states; reg_offset++)
		mod2array[reg_offset] = 0;

	switch (adev->asic_type) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		modearray[0] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[1] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[2] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[3] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[4] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[5] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[6] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[7] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		modearray[8] = (ARRAY_MODE(ARRAY_LINEAR_ALIGNED) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16));
		modearray[9] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[10] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[11] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		modearray[12] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		modearray[13] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[14] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[15] = (ARRAY_MODE(ARRAY_3D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[16] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		modearray[17] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		modearray[18] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[19] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[20] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[21] = (ARRAY_MODE(ARRAY_3D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[22] = (ARRAY_MODE(ARRAY_PRT_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[23] = (ARRAY_MODE(ARRAY_PRT_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[24] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[25] = (ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[26] = (ARRAY_MODE(ARRAY_3D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		modearray[27] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[28] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		modearray[29] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		modearray[30] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_16x16) |
				MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
				SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));

		mod2array[0] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[1] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[2] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[3] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[4] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[5] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[6] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[8] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_8) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[9] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[10] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[11] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[12] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));

		mod2array[13] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_8_BANK));

		mod2array[14] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));

		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, modearray[reg_offset]);

		for (reg_offset = 0; reg_offset < num_secondary_tile_mode_states; reg_offset++)
			if (reg_offset != 7)
				WREG32(mmGB_MACROTILE_MODE0 + reg_offset, mod2array[reg_offset]);

		break;
	default:
		break;	/* the other VI arms are not carried */
	}
}

static void gfx_v8_0_select_se_sh(struct frb_device *adev,
				  uint32_t se_num, uint32_t sh_num, uint32_t instance,
				  int xcc_id)
{
	uint32_t data;

	if (instance == 0xffffffff)
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX, instance);

	if (se_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se_num);

	if (sh_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SH_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SH_INDEX, sh_num);

	WREG32(mmGRBM_GFX_INDEX, data);
}

static uint32_t gfx_v8_0_get_rb_active_bitmap(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t data, mask;

	data =  RREG32(mmCC_RB_BACKEND_DISABLE) |
		RREG32(mmGC_USER_RB_BACKEND_DISABLE);

	data = REG_GET_FIELD(data, GC_USER_RB_BACKEND_DISABLE, BACKEND_DISABLE);

	mask = amdgpu_gfx_create_bitmask(cfg->max_backends_per_se /
					 cfg->max_sh_per_se);

	return (~data) & mask;
}

static void
gfx_v8_0_raster_config(struct frb_device *adev, uint32_t *rconf, uint32_t *rconf1)
{
	switch (adev->asic_type) {
	case CHIP_POLARIS11:
	case CHIP_POLARIS12:
		*rconf |= RB_MAP_PKR0(2) | RB_XSEL2(1) | SE_MAP(2) |
			  SE_XSEL(1) | SE_YSEL(1);
		*rconf1 |= 0x0;
		break;
	default:
		frb_err(adev, "gfx: unknown asic: 0x%x", adev->asic_type);
		break;
	}
}

static void
gfx_v8_0_write_harvested_raster_configs(struct frb_device *adev,
					uint32_t raster_config, uint32_t raster_config_1,
					unsigned rb_mask, unsigned num_rb)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	unsigned sh_per_se = cfg->max_sh_per_se > 1 ? cfg->max_sh_per_se : 1;
	unsigned num_se = cfg->max_shader_engines > 1 ? cfg->max_shader_engines : 1;
	unsigned rb_per_pkr = num_rb / num_se / sh_per_se < 2 ? num_rb / num_se / sh_per_se : 2;
	unsigned rb_per_se = num_rb / num_se;
	unsigned se_mask[4];
	unsigned se;

	se_mask[0] = ((1 << rb_per_se) - 1) & rb_mask;
	se_mask[1] = (se_mask[0] << rb_per_se) & rb_mask;
	se_mask[2] = (se_mask[1] << rb_per_se) & rb_mask;
	se_mask[3] = (se_mask[2] << rb_per_se) & rb_mask;

	/* Linux: WARN_ON */
	if (!(num_se == 1 || num_se == 2 || num_se == 4) || !(sh_per_se == 1 || sh_per_se == 2) ||
	    !(rb_per_pkr == 1 || rb_per_pkr == 2))
		frb_err(adev, "gfx: harvested raster config: %u SE, %u SH/SE, %u RB/packer", num_se,
			sh_per_se, rb_per_pkr);

	if ((num_se > 2) && ((!se_mask[0] && !se_mask[1]) ||
			     (!se_mask[2] && !se_mask[3]))) {
		raster_config_1 &= ~SE_PAIR_MAP_MASK;

		if (!se_mask[0] && !se_mask[1]) {
			raster_config_1 |=
				SE_PAIR_MAP(RASTER_CONFIG_SE_PAIR_MAP_3);
		} else {
			raster_config_1 |=
				SE_PAIR_MAP(RASTER_CONFIG_SE_PAIR_MAP_0);
		}
	}

	for (se = 0; se < num_se; se++) {
		unsigned raster_config_se = raster_config;
		unsigned pkr0_mask = ((1 << rb_per_pkr) - 1) << (se * rb_per_se);
		unsigned pkr1_mask = pkr0_mask << rb_per_pkr;
		int idx = (se / 2) * 2;

		if ((num_se > 1) && (!se_mask[idx] || !se_mask[idx + 1])) {
			raster_config_se &= ~SE_MAP_MASK;

			if (!se_mask[idx]) {
				raster_config_se |= SE_MAP(RASTER_CONFIG_SE_MAP_3);
			} else {
				raster_config_se |= SE_MAP(RASTER_CONFIG_SE_MAP_0);
			}
		}

		pkr0_mask &= rb_mask;
		pkr1_mask &= rb_mask;
		if (rb_per_se > 2 && (!pkr0_mask || !pkr1_mask)) {
			raster_config_se &= ~PKR_MAP_MASK;

			if (!pkr0_mask) {
				raster_config_se |= PKR_MAP(RASTER_CONFIG_PKR_MAP_3);
			} else {
				raster_config_se |= PKR_MAP(RASTER_CONFIG_PKR_MAP_0);
			}
		}

		if (rb_per_se >= 2) {
			unsigned rb0_mask = 1 << (se * rb_per_se);
			unsigned rb1_mask = rb0_mask << 1;

			rb0_mask &= rb_mask;
			rb1_mask &= rb_mask;
			if (!rb0_mask || !rb1_mask) {
				raster_config_se &= ~RB_MAP_PKR0_MASK;

				if (!rb0_mask) {
					raster_config_se |=
						RB_MAP_PKR0(RASTER_CONFIG_RB_MAP_3);
				} else {
					raster_config_se |=
						RB_MAP_PKR0(RASTER_CONFIG_RB_MAP_0);
				}
			}

			if (rb_per_se > 2) {
				rb0_mask = 1 << (se * rb_per_se + rb_per_pkr);
				rb1_mask = rb0_mask << 1;
				rb0_mask &= rb_mask;
				rb1_mask &= rb_mask;
				if (!rb0_mask || !rb1_mask) {
					raster_config_se &= ~RB_MAP_PKR1_MASK;

					if (!rb0_mask) {
						raster_config_se |=
							RB_MAP_PKR1(RASTER_CONFIG_RB_MAP_3);
					} else {
						raster_config_se |=
							RB_MAP_PKR1(RASTER_CONFIG_RB_MAP_0);
					}
				}
			}
		}

		/* GRBM_GFX_INDEX has a different offset on VI */
		gfx_v8_0_select_se_sh(adev, se, 0xffffffff, 0xffffffff, 0);
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config_se);
		WREG32(mmPA_SC_RASTER_CONFIG_1, raster_config_1);
	}

	/* GRBM_GFX_INDEX has a different offset on VI */
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v8_0_setup_rb(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t i, j;
	uint32_t data;
	uint32_t raster_config = 0, raster_config_1 = 0;
	uint32_t active_rbs = 0;
	uint32_t rb_bitmap_width_per_sh = cfg->max_backends_per_se /
					cfg->max_sh_per_se;
	unsigned num_rb_pipes;

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			gfx_v8_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			data = gfx_v8_0_get_rb_active_bitmap(adev);
			active_rbs |= data << ((i * cfg->max_sh_per_se + j) *
					       rb_bitmap_width_per_sh);
		}
	}
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cfg->backend_enable_mask = active_rbs;
	cfg->num_rbs = hweight32(active_rbs);

	num_rb_pipes = cfg->max_backends_per_se * cfg->max_shader_engines;
	if (num_rb_pipes > 16)
		num_rb_pipes = 16;

	gfx_v8_0_raster_config(adev, &raster_config, &raster_config_1);

	if (!cfg->backend_enable_mask ||
			cfg->num_rbs >= num_rb_pipes) {
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config);
		WREG32(mmPA_SC_RASTER_CONFIG_1, raster_config_1);
	} else {
		gfx_v8_0_write_harvested_raster_configs(adev, raster_config, raster_config_1,
							cfg->backend_enable_mask,
							num_rb_pipes);
	}

	/* cache the values for userspace */
	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			gfx_v8_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			cfg->rb_config[i][j].rb_backend_disable =
				RREG32(mmCC_RB_BACKEND_DISABLE);
			cfg->rb_config[i][j].user_rb_backend_disable =
				RREG32(mmGC_USER_RB_BACKEND_DISABLE);
			cfg->rb_config[i][j].raster_config =
				RREG32(mmPA_SC_RASTER_CONFIG);
			cfg->rb_config[i][j].raster_config_1 =
				RREG32(mmPA_SC_RASTER_CONFIG_1);
		}
	}
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

#define DEFAULT_SH_MEM_BASES	(0x6000)
/*
 * gfx_v8_0_init_compute_vmid - gart enable
 *
 * Initialize compute vmid sh_mem registers
 */
static void gfx_v8_0_init_compute_vmid(struct frb_device *adev)
{
	int i;
	uint32_t sh_mem_config;
	uint32_t sh_mem_bases;

	/*
	 * Configure apertures:
	 * LDS:         0x60000000'00000000 - 0x60000001'00000000 (4GB)
	 * Scratch:     0x60000001'00000000 - 0x60000002'00000000 (4GB)
	 * GPUVM:       0x60010000'00000000 - 0x60020000'00000000 (1TB)
	 */
	sh_mem_bases = DEFAULT_SH_MEM_BASES | (DEFAULT_SH_MEM_BASES << 16);

	sh_mem_config = SH_MEM_ADDRESS_MODE_HSA64 <<
			SH_MEM_CONFIG__ADDRESS_MODE__SHIFT |
			SH_MEM_ALIGNMENT_MODE_UNALIGNED <<
			SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT |
			MTYPE_CC << SH_MEM_CONFIG__DEFAULT_MTYPE__SHIFT |
			SH_MEM_CONFIG__PRIVATE_ATC_MASK;

	for (i = FRB_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		vi_srbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		WREG32(mmSH_MEM_CONFIG, sh_mem_config);
		WREG32(mmSH_MEM_APE1_BASE, 1);
		WREG32(mmSH_MEM_APE1_LIMIT, 0);
		WREG32(mmSH_MEM_BASES, sh_mem_bases);
	}
	vi_srbm_select(adev, 0, 0, 0, 0);

	/* Initialize all compute VMIDs to have no GDS, GWS, or OA
	   access. These should be enabled by FW for target VMIDs. */
	for (i = FRB_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		WREG32(amdgpu_gds_reg_offset[i].mem_base, 0);
		WREG32(amdgpu_gds_reg_offset[i].mem_size, 0);
		WREG32(amdgpu_gds_reg_offset[i].gws, 0);
		WREG32(amdgpu_gds_reg_offset[i].oa, 0);
	}
}

static void gfx_v8_0_init_gds_vmid(struct frb_device *adev)
{
	int vmid;

	/*
	 * Initialize all compute and user-gfx VMIDs to have no GDS, GWS, or OA
	 * access. Compute VMIDs should be enabled by FW for target VMIDs,
	 * the driver can enable them for graphics. VMID0 should maintain
	 * access so that HWS firmware can save/restore entries.
	 */
	for (vmid = 1; vmid < AMDGPU_NUM_VMID; vmid++) {
		WREG32(amdgpu_gds_reg_offset[vmid].mem_base, 0);
		WREG32(amdgpu_gds_reg_offset[vmid].mem_size, 0);
		WREG32(amdgpu_gds_reg_offset[vmid].gws, 0);
		WREG32(amdgpu_gds_reg_offset[vmid].oa, 0);
	}
}

static void gfx_v8_0_config_init(struct frb_device *adev)
{
	switch (adev->asic_type) {
	default:
		adev->gfx->config.double_offchip_lds_buf = 1;
		break;
	case CHIP_CARRIZO:
	case CHIP_STONEY:
		adev->gfx->config.double_offchip_lds_buf = 0;
		break;
	}
}

static void gfx_v8_0_constants_init(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t tmp, sh_static_mem_cfg;
	int i;

	WREG32_FIELD(GRBM_CNTL, READ_TIMEOUT, 0xFF);
	WREG32(mmGB_ADDR_CONFIG, cfg->gb_addr_config);
	WREG32(mmHDP_ADDR_CONFIG, cfg->gb_addr_config);
	WREG32(mmDMIF_ADDR_CALC, cfg->gb_addr_config);

	gfx_v8_0_tiling_mode_table_init(adev);
	gfx_v8_0_setup_rb(adev);
	gfx_v8_0_get_cu_info(adev);
	gfx_v8_0_config_init(adev);

	/* XXX SH_MEM regs */
	/* where to put LDS, scratch, GPUVM in FSA64 space */
	sh_static_mem_cfg = REG_SET_FIELD(0, SH_STATIC_MEM_CONFIG,
				   SWIZZLE_ENABLE, 1);
	sh_static_mem_cfg = REG_SET_FIELD(sh_static_mem_cfg, SH_STATIC_MEM_CONFIG,
				   ELEMENT_SIZE, 1);
	sh_static_mem_cfg = REG_SET_FIELD(sh_static_mem_cfg, SH_STATIC_MEM_CONFIG,
				   INDEX_STRIDE, 3);
	WREG32(mmSH_STATIC_MEM_CONFIG, sh_static_mem_cfg);

	for (i = 0; i < FRB_GFXHUB_NUM_IDS; i++) {
		vi_srbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		if (i == 0) {
			tmp = REG_SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, MTYPE_UC);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, APE1_MTYPE, MTYPE_UC);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, ALIGNMENT_MODE,
					    SH_MEM_ALIGNMENT_MODE_UNALIGNED);
			WREG32(mmSH_MEM_CONFIG, tmp);
			WREG32(mmSH_MEM_BASES, 0);
		} else {
			tmp = REG_SET_FIELD(0, SH_MEM_CONFIG, DEFAULT_MTYPE, MTYPE_NC);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, APE1_MTYPE, MTYPE_UC);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, ALIGNMENT_MODE,
					    SH_MEM_ALIGNMENT_MODE_UNALIGNED);
			WREG32(mmSH_MEM_CONFIG, tmp);
			tmp = (uint32_t)(FRB_SHARED_APERTURE_START >> 48);
			WREG32(mmSH_MEM_BASES, tmp);
		}

		WREG32(mmSH_MEM_APE1_BASE, 1);
		WREG32(mmSH_MEM_APE1_LIMIT, 0);
	}
	vi_srbm_select(adev, 0, 0, 0, 0);

	gfx_v8_0_init_compute_vmid(adev);
	gfx_v8_0_init_gds_vmid(adev);

	/*
	 * making sure that the following register writes will be broadcasted
	 * to all the shaders
	 */
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	WREG32(mmPA_SC_FIFO_SIZE,
		   (cfg->sc_prim_fifo_size_frontend <<
			PA_SC_FIFO_SIZE__SC_FRONTEND_PRIM_FIFO_SIZE__SHIFT) |
		   (cfg->sc_prim_fifo_size_backend <<
			PA_SC_FIFO_SIZE__SC_BACKEND_PRIM_FIFO_SIZE__SHIFT) |
		   (cfg->sc_hiz_tile_fifo_size <<
			PA_SC_FIFO_SIZE__SC_HIZ_TILE_FIFO_SIZE__SHIFT) |
		   (cfg->sc_earlyz_tile_fifo_size <<
			PA_SC_FIFO_SIZE__SC_EARLYZ_TILE_FIFO_SIZE__SHIFT));

	tmp = RREG32(mmSPI_ARB_PRIORITY);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS0, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS1, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS2, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS3, 2);
	WREG32(mmSPI_ARB_PRIORITY, tmp);
}

static void gfx_v8_0_wait_for_rlc_serdes(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t i, j, k;
	uint32_t mask;

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			gfx_v8_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			for (k = 0; k < adev->usec_timeout; k++) {
				if (RREG32(mmRLC_SERDES_CU_MASTER_BUSY) == 0)
					break;
				udelay(1);
			}
			if (k == adev->usec_timeout) {
				gfx_v8_0_select_se_sh(adev, 0xffffffff,
						      0xffffffff, 0xffffffff, 0);
				frb_info(adev, "gfx: Timeout wait for RLC serdes %u,%u", i, j);
				return;
			}
		}
	}
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	mask = RLC_SERDES_NONCU_MASTER_BUSY__SE_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__GC_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__TC0_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__TC1_MASTER_BUSY_MASK;
	for (k = 0; k < adev->usec_timeout; k++) {
		if ((RREG32(mmRLC_SERDES_NONCU_MASTER_BUSY) & mask) == 0)
			break;
		udelay(1);
	}
}

static void gfx_v8_0_enable_gui_idle_interrupt(struct frb_device *adev,
					       int enable)
{
	uint32_t tmp = RREG32(mmCP_INT_CNTL_RING0);

	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE, enable ? 1 : 0);

	WREG32(mmCP_INT_CNTL_RING0, tmp);
}

static void gfx_v8_0_init_csb(struct frb_device *adev)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;

	gfx_v8_0_get_csb_buffer(adev, rlc->clear_state_offset);
	frb_flush_hdp(adev);	/* the CSB went in through BAR0 (frb_ring.h, placement) */
	/* csib */
	WREG32(mmRLC_CSIB_ADDR_HI,
			upper_32_bits(rlc->clear_state_gpu_addr));
	WREG32(mmRLC_CSIB_ADDR_LO,
			lower_32_bits(rlc->clear_state_gpu_addr) & 0xfffffffc);
	WREG32(mmRLC_CSIB_LENGTH,
			rlc->clear_state_size);
}

static void gfx_v8_0_parse_ind_reg_list(uint32_t *register_list_format,
				int ind_offset,
				int list_size,
				uint32_t *unique_indices,
				int *indices_count,
				int max_indices,
				uint32_t *ind_start_offsets,
				int *offset_count,
				int max_offset)
{
	int indices;
	int new_entry = 1;

	for (; ind_offset < list_size; ind_offset++) {

		if (new_entry) {
			new_entry = 0;
			ind_start_offsets[*offset_count] = ind_offset;
			*offset_count = *offset_count + 1;
			if (*offset_count >= max_offset)
				return;	/* Linux: BUG_ON */
		}

		if (register_list_format[ind_offset] == 0xFFFFFFFF) {
			new_entry = 1;
			continue;
		}

		ind_offset += 2;
		if (ind_offset >= list_size)
			return;

		/* look for the matching indice */
		for (indices = 0;
			indices < *indices_count;
			indices++) {
			if (unique_indices[indices] ==
				register_list_format[ind_offset])
				break;
		}

		if (indices >= *indices_count) {
			unique_indices[*indices_count] =
				register_list_format[ind_offset];
			indices = *indices_count;
			*indices_count = *indices_count + 1;
			if (*indices_count >= max_indices)
				return;	/* Linux: BUG_ON */
		}

		register_list_format[ind_offset] = indices;
	}
}

static int gfx_v8_0_init_save_restore_list(struct frb_device *adev)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;
	int i, temp, data;
	uint32_t unique_indices[] = {0, 0, 0, 0, 0, 0, 0, 0};
	int indices_count = 0;
	uint32_t indirect_start_offsets[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int offset_count = 0;

	int list_size;
	uint32_t *register_list_format;

	/* kmemdup: the parse rewrites the indices in place */
	register_list_format = (uint32_t *)adev->os->alloc(adev->os->cookie, rlc->lists_bytes);
	if (!register_list_format)
		return -ENOMEM;
	memcpy(register_list_format, rlc->register_list_format, rlc->reg_list_format_size_bytes);

	gfx_v8_0_parse_ind_reg_list(register_list_format,
				RLC_FormatDirectRegListLength,
				rlc->reg_list_format_size_bytes >> 2,
				unique_indices,
				&indices_count,
				ARRAY_SIZE(unique_indices),
				indirect_start_offsets,
				&offset_count,
				ARRAY_SIZE(indirect_start_offsets));

	/* save and restore list */
	WREG32_FIELD(RLC_SRM_CNTL, AUTO_INCR_ADDR, 1);

	WREG32(mmRLC_SRM_ARAM_ADDR, 0);
	for (i = 0; i < (int)(rlc->reg_list_size_bytes >> 2); i++)
		WREG32(mmRLC_SRM_ARAM_DATA, rlc->register_restore[i]);

	/* indirect list */
	WREG32(mmRLC_GPM_SCRATCH_ADDR, rlc->reg_list_format_start);
	for (i = 0; i < (int)(rlc->reg_list_format_size_bytes >> 2); i++)
		WREG32(mmRLC_GPM_SCRATCH_DATA, register_list_format[i]);

	list_size = rlc->reg_list_size_bytes >> 2;
	list_size = list_size >> 1;
	WREG32(mmRLC_GPM_SCRATCH_ADDR, rlc->reg_restore_list_size);
	WREG32(mmRLC_GPM_SCRATCH_DATA, list_size);

	/* starting offsets starts */
	WREG32(mmRLC_GPM_SCRATCH_ADDR,
		rlc->starting_offsets_start);
	for (i = 0; i < (int)ARRAY_SIZE(indirect_start_offsets); i++)
		WREG32(mmRLC_GPM_SCRATCH_DATA,
				indirect_start_offsets[i]);

	/* unique indices */
	temp = mmRLC_SRM_INDEX_CNTL_ADDR_0;
	data = mmRLC_SRM_INDEX_CNTL_DATA_0;
	for (i = 0; i < (int)ARRAY_SIZE(unique_indices); i++) {
		if (unique_indices[i] != 0) {
			WREG32(temp + i, unique_indices[i] & 0x3FFFF);
			WREG32(data + i, unique_indices[i] >> 20);
		}
	}
	adev->os->free(adev->os->cookie, register_list_format);

	return 0;
}

static void gfx_v8_0_enable_save_restore_machine(struct frb_device *adev)
{
	WREG32_FIELD(RLC_SRM_CNTL, SRM_ENABLE, 1);
}

static void gfx_v8_0_init_power_gating(struct frb_device *adev)
{
	uint32_t data;

	WREG32_FIELD(CP_RB_WPTR_POLL_CNTL, IDLE_POLL_COUNT, 0x60);

	data = REG_SET_FIELD(0, RLC_PG_DELAY, POWER_UP_DELAY, 0x10);
	data = REG_SET_FIELD(data, RLC_PG_DELAY, POWER_DOWN_DELAY, 0x10);
	data = REG_SET_FIELD(data, RLC_PG_DELAY, CMD_PROPAGATE_DELAY, 0x10);
	data = REG_SET_FIELD(data, RLC_PG_DELAY, MEM_SLEEP_DELAY, 0x10);
	WREG32(mmRLC_PG_DELAY, data);

	WREG32_FIELD(RLC_PG_DELAY_2, SERDES_CMD_DELAY, 0x3);
	WREG32_FIELD(RLC_AUTO_PG_CTRL, GRBM_REG_SAVE_GFX_IDLE_THRESHOLD, 0x55f0);

}

static int gfx_v8_0_init_pg(struct frb_device *adev)
{
	int r = 0;

	if ((adev->asic_type == CHIP_POLARIS11) ||
	    (adev->asic_type == CHIP_POLARIS12) ||
	    (adev->asic_type == CHIP_VEGAM)) {
		gfx_v8_0_init_csb(adev);
		r = gfx_v8_0_init_save_restore_list(adev);
		gfx_v8_0_enable_save_restore_machine(adev);
		gfx_v8_0_init_power_gating(adev);
	}
	return r;
}

static void gfx_v8_0_rlc_stop(struct frb_device *adev)
{
	WREG32_FIELD(RLC_CNTL, RLC_ENABLE_F32, 0);

	gfx_v8_0_enable_gui_idle_interrupt(adev, 0);
	gfx_v8_0_wait_for_rlc_serdes(adev);
}

static void gfx_v8_0_rlc_reset(struct frb_device *adev)
{
	WREG32_FIELD(GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);
	udelay(50);

	WREG32_FIELD(GRBM_SOFT_RESET, SOFT_RESET_RLC, 0);
	udelay(50);
}

static void gfx_v8_0_rlc_start(struct frb_device *adev)
{
	WREG32_FIELD(RLC_CNTL, RLC_ENABLE_F32, 1);

	/* carrizo do enable cp interrupt after cp inited; not an APU here */
	gfx_v8_0_enable_gui_idle_interrupt(adev, 1);

	udelay(50);
}

static int gfx_v8_0_rlc_resume(struct frb_device *adev)
{
	int r;

	gfx_v8_0_rlc_stop(adev);
	gfx_v8_0_rlc_reset(adev);
	r = gfx_v8_0_init_pg(adev);
	gfx_v8_0_rlc_start(adev);

	return r;
}

static void gfx_v8_0_cp_gfx_enable(struct frb_device *adev, int enable)
{
	uint32_t tmp = RREG32(mmCP_ME_CNTL);

	if (enable) {
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, 0);
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, 0);
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_HALT, 0);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, 1);
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, 1);
		tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_HALT, 1);
	}
	WREG32(mmCP_ME_CNTL, tmp);
	udelay(50);
}

static uint32_t gfx_v8_0_get_csb_size(struct frb_device *adev)
{
	uint32_t count = 0;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;

	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = vi_cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT)
				count += 2 + ext->reg_count;
			else
				return 0;
		}
	}
	/* pa_sc_raster_config/pa_sc_raster_config1 */
	count += 4;
	/* end clear state */
	count += 2;
	/* clear state */
	count += 2;

	return count;
}

static int gfx_v8_0_cp_gfx_start(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	struct frb_ring *ring = &g->gfx_ring;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	int r;
	uint32_t i;

	/* init the CP */
	WREG32(mmCP_MAX_CONTEXT, g->config.max_hw_contexts - 1);
	WREG32(mmCP_ENDIAN_SWAP, 0);
	WREG32(mmCP_DEVICE_ID, 1);

	gfx_v8_0_cp_gfx_enable(adev, 1);

	r = frb_ring_alloc(ring, gfx_v8_0_get_csb_size(adev) + 4);
	if (r) {
		frb_err(adev, "gfx: cp failed to lock ring (%d).", r);
		return r;
	}

	/* clear state buffer */
	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	frb_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	frb_ring_write(ring, 0x80000000);
	frb_ring_write(ring, 0x80000000);

	for (sect = vi_cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				frb_ring_write(ring,
				       PACKET3(PACKET3_SET_CONTEXT_REG,
					       ext->reg_count));
				frb_ring_write(ring,
				       ext->reg_index - PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					frb_ring_write(ring, ext->extent[i]);
			}
		}
	}

	frb_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	frb_ring_write(ring, mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	frb_ring_write(ring, g->config.rb_config[0][0].raster_config);
	frb_ring_write(ring, g->config.rb_config[0][0].raster_config_1);

	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_END_CLEAR_STATE);

	frb_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
	frb_ring_write(ring, 0);

	/* init the CE partitions */
	frb_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	frb_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	frb_ring_write(ring, 0x8000);
	frb_ring_write(ring, 0x8000);

	frb_ring_commit(ring);

	return 0;
}

/* gfx_v8_0_set_cpg_door_bell, the use_doorbell = false arm (not an APU) */
static void gfx_v8_0_set_cpg_door_bell(struct frb_device *adev, struct frb_ring *ring)
{
	uint32_t tmp;

	tmp = RREG32(mmCP_RB_DOORBELL_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
	WREG32(mmCP_RB_DOORBELL_CONTROL, tmp);

	tmp = REG_SET_FIELD(0, CP_RB_DOORBELL_RANGE_LOWER,
					DOORBELL_RANGE_LOWER,
					AMDGPU_DOORBELL_GFX_RING0);
	WREG32(mmCP_RB_DOORBELL_RANGE_LOWER, tmp);

	WREG32(mmCP_RB_DOORBELL_RANGE_UPPER,
		CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
}

static int gfx_v8_0_cp_gfx_resume(struct frb_device *adev)
{
	struct frb_ring *ring;
	uint32_t tmp;
	uint32_t rb_bufsz;
	uint64_t rb_addr, rptr_addr, wptr_gpu_addr;

	/* Set the write pointer delay */
	WREG32(mmCP_RB_WPTR_DELAY, 0);

	/* set the RB to use vmid 0 */
	WREG32(mmCP_RB_VMID, 0);

	/* Set ring buffer size */
	ring = &adev->gfx->gfx_ring;
	rb_bufsz = order_base_2(ring->ring_size / 8);
	tmp = REG_SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, MTYPE, 3);
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, MIN_IB_AVAILSZ, 1);
	/*
	 * No BUF_SWAP: the ring is little-endian (see the file header).
	 * Measured on the E9173 (2026-11-13) with the ring in the
	 * CPU's order: amdgpu's __BIG_ENDIAN arm (BUF_SWAP 1, i.e. 0x20000,
	 * which is radeon's BUF_SWAP_32BIT on CIK), BUF_SWAP 2, and
	 * CP_ENDIAN_SWAP 2 each fail the ring test.
	 */
	WREG32(mmCP_RB0_CNTL, tmp);

	/* Initialize the ring buffer's read and write pointers */
	WREG32(mmCP_RB0_CNTL, tmp | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
	ring->wptr = 0;
	WREG32(mmCP_RB0_WPTR, lower_32_bits(ring->wptr));

	/* set the wb address whether it's enabled or not */
	rptr_addr = ring->rptr_gpu_addr;
	WREG32(mmCP_RB0_RPTR_ADDR, lower_32_bits(rptr_addr));
	WREG32(mmCP_RB0_RPTR_ADDR_HI, upper_32_bits(rptr_addr) & 0xFF);

	wptr_gpu_addr = ring->wptr_gpu_addr;
	WREG32(mmCP_RB_WPTR_POLL_ADDR_LO, lower_32_bits(wptr_gpu_addr));
	WREG32(mmCP_RB_WPTR_POLL_ADDR_HI, upper_32_bits(wptr_gpu_addr));
	mdelay(1);
	WREG32(mmCP_RB0_CNTL, tmp);

	rb_addr = ring->gpu_addr >> 8;
	WREG32(mmCP_RB0_BASE, lower_32_bits(rb_addr));
	WREG32(mmCP_RB0_BASE_HI, upper_32_bits(rb_addr));

	gfx_v8_0_set_cpg_door_bell(adev, ring);
	/* start the ring */
	frb_ring_clear(ring);
	return gfx_v8_0_cp_gfx_start(adev);
}

static void gfx_v8_0_cp_compute_enable(struct frb_device *adev, int enable)
{
	if (enable) {
		WREG32(mmCP_MEC_CNTL, 0);
	} else {
		WREG32(mmCP_MEC_CNTL, (CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK));
	}
	udelay(50);
}

/* gfx_v8_0_cp_test_all_rings: the gfx ring (no KIQ, no compute rings) */
static int gfx_v8_0_cp_test_all_rings(struct frb_device *adev)
{
	struct frb_ring *ring = &adev->gfx->gfx_ring;
	int r;

	/* amdgpu_ring_test_helper */
	r = gfx_v8_0_ring_test_ring(ring);
	ring->ready = !r;
	return r;
}

static int gfx_v8_0_cp_resume(struct frb_device *adev)
{
	int r;

	/* not an APU */
	gfx_v8_0_enable_gui_idle_interrupt(adev, 0);

	/* gfx_v8_0_kiq_resume: not ported */

	r = gfx_v8_0_cp_gfx_resume(adev);
	if (r)
		return r;

	/* gfx_v8_0_kcq_resume: not ported */

	r = gfx_v8_0_cp_test_all_rings(adev);
	if (r)
		return r;

	gfx_v8_0_enable_gui_idle_interrupt(adev, 1);

	return 0;
}

static void gfx_v8_0_cp_enable(struct frb_device *adev, int enable)
{
	gfx_v8_0_cp_gfx_enable(adev, enable);
	gfx_v8_0_cp_compute_enable(adev, enable);
}

static int gfx_v8_0_hw_init(struct frb_device *adev)
{
	int r;

	gfx_v8_0_init_golden_registers(adev);
	gfx_v8_0_constants_init(adev);

	adev->gfx->hw_started = 1;
	r = gfx_v8_0_rlc_resume(adev);
	if (r)
		return r;

	r = gfx_v8_0_cp_resume(adev);

	return r;
}

static int gfx_v8_0_is_idle(struct frb_device *adev)
{
	if (REG_GET_FIELD(RREG32(mmGRBM_STATUS), GRBM_STATUS, GUI_ACTIVE)
		|| RREG32(mmGRBM_STATUS2) != 0x8)
		return 0;
	else
		return 1;
}

static int gfx_v8_0_rlc_is_idle(struct frb_device *adev)
{
	if (RREG32(mmGRBM_STATUS2) != 0x8)
		return 0;
	else
		return 1;
}

static int gfx_v8_0_wait_for_rlc_idle(struct frb_device *adev)
{
	unsigned int i;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (gfx_v8_0_rlc_is_idle(adev))
			return 0;

		udelay(1);
	}
	return -ETIMEDOUT;
}

static int gfx_v8_0_wait_for_idle(struct frb_device *adev)
{
	unsigned int i;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (gfx_v8_0_is_idle(adev))
			return 0;

		udelay(1);
	}
	return -ETIMEDOUT;
}

static int gfx_v8_0_is_rlc_enabled(struct frb_device *adev)
{
	uint32_t rlc_setting;

	rlc_setting = RREG32(mmRLC_CNTL);
	if (!(rlc_setting & RLC_CNTL__RLC_ENABLE_F32_MASK))
		return 0;

	return 1;
}

static void gfx_v8_0_set_safe_mode(struct frb_device *adev, int xcc_id)
{
	uint32_t data;
	unsigned i;
	data = RREG32(mmRLC_CNTL);
	data |= RLC_SAFE_MODE__CMD_MASK;
	data &= ~RLC_SAFE_MODE__MESSAGE_MASK;
	data |= (1 << RLC_SAFE_MODE__MESSAGE__SHIFT);
	WREG32(mmRLC_SAFE_MODE, data);

	/* wait for RLC_SAFE_MODE */
	for (i = 0; i < adev->usec_timeout; i++) {
		if ((RREG32(mmRLC_GPM_STAT) &
		     (RLC_GPM_STAT__GFX_CLOCK_STATUS_MASK |
		      RLC_GPM_STAT__GFX_POWER_STATUS_MASK)) ==
		    (RLC_GPM_STAT__GFX_CLOCK_STATUS_MASK |
		     RLC_GPM_STAT__GFX_POWER_STATUS_MASK))
			break;
		udelay(1);
	}
	for (i = 0; i < adev->usec_timeout; i++) {
		if (!REG_GET_FIELD(RREG32(mmRLC_SAFE_MODE), RLC_SAFE_MODE, CMD))
			break;
		udelay(1);
	}
}

static void gfx_v8_0_unset_safe_mode(struct frb_device *adev, int xcc_id)
{
	uint32_t data;
	unsigned i;

	data = RREG32(mmRLC_CNTL);
	data |= RLC_SAFE_MODE__CMD_MASK;
	data &= ~RLC_SAFE_MODE__MESSAGE_MASK;
	WREG32(mmRLC_SAFE_MODE, data);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (!REG_GET_FIELD(RREG32(mmRLC_SAFE_MODE), RLC_SAFE_MODE, CMD))
			break;
		udelay(1);
	}
}

/* amdgpu_rlc.c amdgpu_gfx_rlc_enter_safe_mode / _exit_safe_mode */
static void amdgpu_gfx_rlc_enter_safe_mode(struct frb_device *adev, int xcc_id)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;

	if (rlc->in_safe_mode)
		return;

	/* if RLC is not enabled, do nothing */
	if (!gfx_v8_0_is_rlc_enabled(adev))
		return;

	if (FRB_CG_GFX_SAFE_MODE) {
		gfx_v8_0_set_safe_mode(adev, xcc_id);
		rlc->in_safe_mode = 1;
	}
}

static void amdgpu_gfx_rlc_exit_safe_mode(struct frb_device *adev, int xcc_id)
{
	struct frb_rlc *rlc = &adev->gfx->rlc;

	if (!rlc->in_safe_mode)
		return;

	/* if RLC is not enabled, do nothing */
	if (!gfx_v8_0_is_rlc_enabled(adev))
		return;

	if (FRB_CG_GFX_SAFE_MODE) {
		gfx_v8_0_unset_safe_mode(adev, xcc_id);
		rlc->in_safe_mode = 0;
	}
}

/* for the DPM code (smu7_enable_didt_config): amdgpu_gfx_rlc_enter/exit_safe_mode(adev, 0) */
void frb_gfx_v8_0_rlc_enter_safe_mode(struct frb_device *adev)
{
	if (adev->gfx)
		amdgpu_gfx_rlc_enter_safe_mode(adev, 0);
}

void frb_gfx_v8_0_rlc_exit_safe_mode(struct frb_device *adev)
{
	if (adev->gfx)
		amdgpu_gfx_rlc_exit_safe_mode(adev, 0);
}

/*
 * gfx_v8_0_hw_fini: no interrupt sources to put, no KCQs to unmap (the KIQ
 * is not ported), not SR-IOV. The CP is halted only when idle, as amdgpu
 * does; the log says so when it is not.
 */
static int gfx_v8_0_hw_fini(struct frb_device *adev)
{
	amdgpu_gfx_rlc_enter_safe_mode(adev, 0);
	if (!gfx_v8_0_wait_for_idle(adev))
		gfx_v8_0_cp_enable(adev, 0);
	else
		frb_err(adev, "gfx: cp is busy, skip halt cp (GRBM_STATUS %08x, GRBM_STATUS2 %08x)",
			RREG32(mmGRBM_STATUS), RREG32(mmGRBM_STATUS2));
	if (!gfx_v8_0_wait_for_rlc_idle(adev))
		gfx_v8_0_rlc_stop(adev);
	else
		frb_err(adev, "gfx: rlc is busy, skip halt rlc (GRBM_STATUS2 %08x)",
			RREG32(mmGRBM_STATUS2));
	amdgpu_gfx_rlc_exit_safe_mode(adev, 0);

	return 0;
}

/* --- ring functions -------------------------------------------------------- */

static uint64_t gfx_v8_0_ring_get_rptr(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->rptr_offs);	/* *ring->rptr_cpu_addr */
}

static uint64_t gfx_v8_0_ring_get_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmCP_RB0_WPTR);	/* no doorbell */
}

static void gfx_v8_0_ring_set_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	/* no doorbell */
	WREG32(mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
	(void)RREG32(mmCP_RB0_WPTR);
}

/* gfx_v8_0_get_hdp_flush_mask, the gfx-ring arm */
static void gfx_v8_0_get_hdp_flush_mask(struct frb_ring *ring,
					uint32_t *ref_and_mask, uint32_t *reg_mem_engine)
{
	*ref_and_mask = GPU_HDP_FLUSH_DONE__CP0_MASK;
	*reg_mem_engine = WAIT_REG_MEM_ENGINE(1); /* pfp */
}

static void gfx_v8_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	uint32_t ref_and_mask, reg_mem_engine;

	gfx_v8_0_get_hdp_flush_mask(ring, &ref_and_mask, &reg_mem_engine);
	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_OPERATION(1) | /* write, wait, write */
				 WAIT_REG_MEM_FUNCTION(3) |  /* == */
				 reg_mem_engine));
	frb_ring_write(ring, mmGPU_HDP_FLUSH_REQ);
	frb_ring_write(ring, mmGPU_HDP_FLUSH_DONE);
	frb_ring_write(ring, ref_and_mask);
	frb_ring_write(ring, ref_and_mask);
	frb_ring_write(ring, 0x20); /* poll interval */
}

static void gfx_v8_0_ring_emit_ib_gfx(struct frb_ring *ring,
				      const struct frb_ib *ib,
				      uint32_t vmid, uint32_t flags)
{
	uint32_t header, control = 0;

	if (ib->flags & AMDGPU_IB_FLAG_CE)
		header = PACKET3(PACKET3_INDIRECT_BUFFER_CONST, 2);
	else
		header = PACKET3(PACKET3_INDIRECT_BUFFER, 2);

	control |= ib->length_dw | (vmid << 24);

	frb_ring_write(ring, header);
	/*
	 * No IB swap (Linux: (2 << 0) under __BIG_ENDIAN): every IB is
	 * little-endian, the kernel's and the accelerator's clients' alike.
	 * On this GFX8 CP the swap field does not make a host-order IB run;
	 * any nonzero value stops a little-endian one too (frbctl test gfx
	 * ibprobe, gfx_v8_0_ib_probe).
	 */
	frb_ring_write(ring, (uint32_t)(ib->gpu_addr & 0xFFFFFFFC));
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr) & 0xFFFF);
	frb_ring_write(ring, control);
}

static void gfx_v8_0_ring_emit_fence_gfx(struct frb_ring *ring, uint64_t addr,
					 uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;
	int int_sel = flags & FRB_FENCE_FLAG_INT;
	int exec = 0;	/* AMDGPU_FENCE_FLAG_EXEC: trailing fences only */

	/* Workaround for cache flush problems. First send a dummy EOP
	 * event down the pipe with seq one below.
	 */
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	frb_ring_write(ring, (EOP_TCL1_ACTION_EN |
				 EOP_TC_ACTION_EN |
				 EOP_TC_WB_ACTION_EN |
				 EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 EVENT_INDEX(5)));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xffff) |
				DATA_SEL(1) | INT_SEL(0));
	frb_ring_write(ring, lower_32_bits(seq - 1));
	frb_ring_write(ring, upper_32_bits(seq - 1));

	/* Then send the real EOP event down the pipe:
	 * EVENT_WRITE_EOP - flush caches, send int */
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	frb_ring_write(ring, (EOP_TCL1_ACTION_EN |
				 EOP_TC_ACTION_EN |
				 EOP_TC_WB_ACTION_EN |
				 EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 EVENT_INDEX(5) |
				 (exec ? EOP_EXEC : 0)));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xffff) |
			  DATA_SEL(write64bit ? 2 : 1) | INT_SEL(int_sel ? 2 : 0));
	frb_ring_write(ring, lower_32_bits(seq));
	frb_ring_write(ring, upper_32_bits(seq));
}

/*
 * Wait in the PFP for a dword in memory, as gfx_v8_0_ring_emit_pipeline_sync
 * does for the ring's own fence (memory space, the PFP on the gfx ring),
 * with the compare function given: the accelerator waits on SDMA0's fence
 * so that a GFX fence covers 2D work done there.
 */
static void gfx_v8_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
				 WAIT_REG_MEM_FUNCTION(func) |
				 WAIT_REG_MEM_ENGINE(1)));   /* pfp */
	frb_ring_write(ring, addr & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, 4); /* poll interval */
}

static const struct frb_ring_funcs gfx_v8_0_ring_funcs_gfx = {
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = 0,
	.get_rptr = gfx_v8_0_ring_get_rptr,
	.get_wptr = gfx_v8_0_ring_get_wptr_gfx,
	.set_wptr = gfx_v8_0_ring_set_wptr_gfx,
	.emit_ib = gfx_v8_0_ring_emit_ib_gfx,
	.emit_fence = gfx_v8_0_ring_emit_fence_gfx,
	.emit_hdp_flush = gfx_v8_0_ring_emit_hdp_flush,
	.insert_nop = NULL,			/* amdgpu_ring_insert_nop */
	.pad_ib = frb_ring_generic_pad_ib,	/* amdgpu_ring_generic_pad_ib */
	.emit_wait_mem = gfx_v8_0_ring_emit_wait_mem,
};

/* --- cu info --------------------------------------------------------------- */

static void gfx_v8_0_set_user_cu_inactive_bitmap(struct frb_device *adev,
						 uint32_t bitmap)
{
	uint32_t data;

	if (!bitmap)
		return;

	data = bitmap << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;
	data &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;

	WREG32(mmGC_USER_SHADER_ARRAY_CONFIG, data);
}

static uint32_t gfx_v8_0_get_cu_active_bitmap(struct frb_device *adev)
{
	uint32_t data, mask;

	data =  RREG32(mmCC_GC_SHADER_ARRAY_CONFIG) |
		RREG32(mmGC_USER_SHADER_ARRAY_CONFIG);

	mask = amdgpu_gfx_create_bitmask(adev->gfx->config.max_cu_per_sh);

	return ~REG_GET_FIELD(data, CC_GC_SHADER_ARRAY_CONFIG, INACTIVE_CUS) & mask;
}

static void gfx_v8_0_get_cu_info(struct frb_device *adev)
{
	struct frb_gfx_config *cfg = &adev->gfx->config;
	uint32_t i, j, k, counter, active_cu_number = 0;
	uint32_t mask, bitmap, ao_bitmap, ao_cu_mask = 0;
	struct frb_cu_info *cu_info = &adev->gfx->cu_info;
	unsigned disable_masks[4 * 2];
	uint32_t ao_cu_num;

	memset(cu_info, 0, sizeof(*cu_info));

	ao_cu_num = cfg->max_cu_per_sh;	/* not an APU */

	/* amdgpu_gfx_parse_disable_cu: amdgpu.disable_cu is unset */
	memset(disable_masks, 0, sizeof(disable_masks));

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			mask = 1;
			ao_bitmap = 0;
			counter = 0;
			gfx_v8_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			if (i < 4 && j < 2)
				gfx_v8_0_set_user_cu_inactive_bitmap(
					adev, disable_masks[i * 2 + j]);
			bitmap = gfx_v8_0_get_cu_active_bitmap(adev);
			cu_info->bitmap[i][j] = bitmap;

			for (k = 0; k < cfg->max_cu_per_sh; k ++) {
				if (bitmap & mask) {
					if (counter < ao_cu_num)
						ao_bitmap |= mask;
					counter ++;
				}
				mask <<= 1;
			}
			active_cu_number += counter;
			if (i < 2 && j < 2)
				ao_cu_mask |= (ao_bitmap << (i * 16 + j * 8));
			cu_info->ao_cu_bitmap[i][j] = ao_bitmap;
		}
	}
	gfx_v8_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cu_info->number = active_cu_number;
	cu_info->ao_cu_mask = ao_cu_mask;
	cu_info->simd_per_cu = NUM_SIMD_PER_CU;
	cu_info->max_waves_per_simd = 10;
	cu_info->max_scratch_slots_per_cu = 32;
	cu_info->wave_front_size = 64;
	cu_info->lds_size = 64;
}

/*
 * Which IBs run, in which byte order, under the INDIRECT_BUFFER's swap
 * field (address bits 1:0; amdgpu's __BIG_ENDIAN arm puts 2 there):
 * frbctl test gfx ibprobe, the measurement behind docs/accel-interface.md
 * Q4b. Each variant is the IB test's WRITE_DATA of 0xDEADBEEF to a
 * writeback slot, from the safest; the probe stops at the first one whose
 * fence does not come (the CP hung: reboot).
 */
static int gfx_v8_0_ib_probe_one(struct frb_ring *ring, uint32_t bits, int host, uint32_t *wb_out)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t index, seq, i, w[5];
	uint64_t gpu_addr;
	int r;

	r = frb_wb_get(adev, &index);
	if (r)
		return r;
	gpu_addr = frb_wb_gpu_addr(adev, index);
	frb_wb_write(adev, index, 0xCAFEDEAD);
	r = frb_ib_get(adev, 20, &ib);
	if (r)
		goto out;
	w[0] = PACKET3(PACKET3_WRITE_DATA, 3);
	w[1] = WRITE_DATA_DST_SEL(5) | WR_CONFIRM;
	w[2] = lower_32_bits(gpu_addr);
	w[3] = upper_32_bits(gpu_addr);
	w[4] = 0xDEADBEEF;
	/* host 1: every dword in the CPU's order; host k > 1: the first k - 1 only (header, control), the rest LE */
	for (i = 0; i < 5; i++)
		frb_vram_write32(adev, ib.bar_offset + 4 * i,
				 (host == 1 || (host > 1 && i < (uint32_t)host - 1)) ? frb_swap_le32(w[i]) : w[i]);
	r = frb_ring_alloc(ring, 64);
	if (r)
		goto out;
	gfx_v8_0_ring_emit_hdp_flush(ring);
	frb_ring_write(ring, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	frb_ring_write(ring, (bits & 3) | (uint32_t)(ib.gpu_addr & 0xFFFFFFFC));
	frb_ring_write(ring, upper_32_bits(ib.gpu_addr) & 0xFFFF);
	frb_ring_write(ring, 5);
	seq = ++ring->sync_seq;
	ring->funcs->emit_fence(ring, ring->fence_gpu_addr, seq, FRB_FENCE_FLAG_INT);
	frb_ring_commit(ring);
	r = frb_fence_wait(ring, seq, 200 * 1000);
	*wb_out = frb_wb_read(adev, index);
out:
	frb_wb_free(adev, index);
	return r;
}

static int gfx_v8_0_ib_probe(struct frb_ring *ring)
{
	/*
	 * Measured on the E9173 (2026-11-13): only the first runs.
	 * Every nonzero swap field leaves the WRITE_DATA unwritten, whatever
	 * order the IB is in, and the fence still comes; CP_ENDIAN_SWAP 2
	 * (RadeonNI's Sea Islands setting) hangs the CP, so it is not tried
	 * here. So GFX8 IBs are little-endian, and the accelerator's clients
	 * byte-swap theirs (RDN_VRAM_INFO64_LE_IBS).
	 */
	static const struct { uint32_t bits; int host; } v[] = {
		{ 0, 0 },	/* the kernel's IBs */
		{ 2, 1 },	/* amdgpu's big-endian IB arm */
		{ 1, 1 },	/* the other encodings of the field */
		{ 3, 1 },
		{ 2, 0 },	/* are the bits ignored? (Sea Islands: hung) */
		{ 1, 0 },
		{ 3, 0 },
		{ 2, 2 },	/* the header swapped, the rest raw? */
		{ 2, 3 },	/* header and control swapped, address and payload raw? */
	};
	struct frb_device *adev = ring->adev;
	uint32_t wb = 0, i, ran = 0;
	int r = 0;

	for (i = 0; i < ARRAY_SIZE(v) && !r; i++) {
		r = gfx_v8_0_ib_probe_one(ring, v[i].bits, v[i].host, &wb);
		frb_info(adev, "gfx: ib probe: IB swap bits %u, %s IB: %s, writeback %08x (%s)",
			 v[i].bits, v[i].host == 1 ? "host-order" : v[i].host == 2 ? "header-host" :
			 v[i].host == 3 ? "header+control-host" : "little-endian",
			 r ? "fence NOT signalled" : "fence signalled", wb,
			 wb == 0xDEADBEEF ? "the payload as its value" :
			 wb == 0xEFBEADDE ? "the payload's bytes as they stood" :
			 wb == 0xCAFEDEAD ? "not written" : "something else");
		if (!r && (wb == 0xDEADBEEF || wb == 0xEFBEADDE))
			ran |= 1u << i;
	}
	if (r) {
		frb_err(adev, "gfx: ib probe: the CP stopped at variant %u (GRBM_STATUS %08x CP_STAT %08x): reboot",
			i - 1, RREG32(mmGRBM_STATUS), RREG32(mmCP_STAT));
		return r;
	}
	frb_info(adev, "gfx: ib probe: variants that ran: mask %03x", ran);
	return 0;
}

/* --- the stage, the tests, the teardown ------------------------------------ */

static void gfx_free(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;

	if (!g)
		return;
	if (g->rlc.register_list_format)
		adev->os->free(adev->os->cookie, g->rlc.register_list_format);
	adev->os->free(adev->os->cookie, g);
	adev->gfx = NULL;
}

/*
 * Stage gfx: gfx_v8_0_sw_init, then gfx_v8_0_hw_init. amdgpu_device_ip_init
 * runs every block's sw_init before the SMU starts; Firebrand's stages run
 * this block's after it, which changes no register value
 * (tests/Makefile, trace-gfx).
 */
int frb_gfx_v8_0_init(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	int r;

	if (adev->asic_type != CHIP_POLARIS12) {
		frb_err(adev, "gfx: only the Polaris 12 paths of gfx_v8_0 are ported");
		return -ENODEV;
	}
	if (!adev->aper || !adev->layout.objects_size) {
		frb_err(adev, "gfx: no VRAM window or objects region (stage gmc)");
		return -ENODEV;
	}
	if (!g) {
		g = (struct frb_gfx *)adev->os->alloc(adev->os->cookie, sizeof(*g));
		if (!g)
			return -ENOMEM;
		adev->gfx = g;
	}
	if (!g->sw_done) {
		r = gfx_v8_0_sw_init(adev);
		if (r)
			return r;
	}
	r = gfx_v8_0_hw_init(adev);
	frb_info(adev, "gfx: %u SE x %u SH, %u CUs (always-on mask %08x), %u RBs (mask %x), "
		 "gb_addr_config %08x; ring %u KB at %llx, CSB %u dwords at %llx",
		 g->config.max_shader_engines, g->config.max_sh_per_se, g->cu_info.number,
		 g->cu_info.ao_cu_mask, g->config.num_rbs, g->config.backend_enable_mask,
		 g->config.gb_addr_config, g->gfx_ring.ring_size >> 10,
		 (unsigned long long)g->gfx_ring.gpu_addr, g->rlc.clear_state_size,
		 (unsigned long long)g->rlc.clear_state_gpu_addr);
	if (r)
		frb_err(adev, "gfx: hw_init failed (%d)", r);
	return r;
}

/* gfx_v8_0_hw_fini, then sw_fini's frees (the VRAM objects are not reused) */
void frb_gfx_v8_0_fini(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;

	if (!g)
		return;
	if (g->hw_started) {
		g->gfx_ring.ready = 0;
		gfx_v8_0_hw_fini(adev);
		frb_info(adev, "gfx: hw_fini: CP_ME_CNTL %08x, CP_MEC_CNTL %08x, RLC_CNTL %08x",
			 RREG32(mmCP_ME_CNTL), RREG32(mmCP_MEC_CNTL), RREG32(mmRLC_CNTL));
	}
	gfx_free(adev);
}

int frb_gfx_v8_0_test(struct frb_device *adev, int test)
{
	struct frb_gfx *g = adev->gfx;
	int r;

	if (!g || !g->hw_started)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		r = gfx_v8_0_ring_test_ring(&g->gfx_ring);
		g->gfx_ring.ready = !r;	/* amdgpu_ring_test_helper */
		return r;
	case FRB_TEST_IB:
		return gfx_v8_0_ring_test_ib(&g->gfx_ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_IB_PROBE:
		return gfx_v8_0_ib_probe(&g->gfx_ring);
	default:
		return -ENOSYS;
	}
}

/* --- soft reset (amdgpu_device_gpu_recover's IP soft-reset arm) -------------- */

/* gfx_v8_0_check_soft_reset: nonzero when a block is busy (the verdict is kept for the rest) */
static int gfx_v8_0_check_soft_reset(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	uint32_t grbm_soft_reset = 0, srbm_soft_reset = 0;
	uint32_t tmp;

	if (!g || !g->hw_started)
		return 0;

	/* GRBM_STATUS */
	tmp = RREG32(mmGRBM_STATUS);
	if (tmp & (GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK |
		   GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
		   GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__VGT_BUSY_MASK |
		   GRBM_STATUS__DB_BUSY_MASK | GRBM_STATUS__CB_BUSY_MASK |
		   GRBM_STATUS__GDS_BUSY_MASK | GRBM_STATUS__SPI_BUSY_MASK |
		   GRBM_STATUS__IA_BUSY_MASK | GRBM_STATUS__IA_BUSY_NO_DMA_MASK |
		   GRBM_STATUS__CP_BUSY_MASK | GRBM_STATUS__CP_COHERENCY_BUSY_MASK)) {
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_CP, 1);
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_GFX, 1);
		srbm_soft_reset = REG_SET_FIELD(srbm_soft_reset,
						SRBM_SOFT_RESET, SOFT_RESET_GRBM, 1);
	}

	/* GRBM_STATUS2 */
	tmp = RREG32(mmGRBM_STATUS2);
	if (REG_GET_FIELD(tmp, GRBM_STATUS2, RLC_BUSY))
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);

	if (REG_GET_FIELD(tmp, GRBM_STATUS2, CPF_BUSY) ||
	    REG_GET_FIELD(tmp, GRBM_STATUS2, CPC_BUSY) ||
	    REG_GET_FIELD(tmp, GRBM_STATUS2, CPG_BUSY)) {
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET,
						SOFT_RESET_CPF, 1);
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET,
						SOFT_RESET_CPC, 1);
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET,
						SOFT_RESET_CPG, 1);
		srbm_soft_reset = REG_SET_FIELD(srbm_soft_reset, SRBM_SOFT_RESET,
						SOFT_RESET_GRBM, 1);
	}

	/* SRBM_STATUS */
	tmp = RREG32(mmSRBM_STATUS);
	if (REG_GET_FIELD(tmp, SRBM_STATUS, GRBM_RQ_PENDING))
		srbm_soft_reset = REG_SET_FIELD(srbm_soft_reset,
						SRBM_SOFT_RESET, SOFT_RESET_GRBM, 1);
	if (REG_GET_FIELD(tmp, SRBM_STATUS, SEM_BUSY))
		srbm_soft_reset = REG_SET_FIELD(srbm_soft_reset,
						SRBM_SOFT_RESET, SOFT_RESET_SEM, 1);

	g->grbm_soft_reset = grbm_soft_reset;
	g->srbm_soft_reset = srbm_soft_reset;
	return grbm_soft_reset || srbm_soft_reset;
}

/*
 * Not amdgpu: mark the block hung whatever its status says (frbctl recover's
 * forced levels, RadeonNI's rdnctl recover): what check_soft_reset asks for
 * a busy CP and a busy CPF/CPC/CPG together, so a CP that sits idle after a
 * fault is reset as well.
 */
static void gfx_v8_0_force_soft_reset(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;

	if (!g || !g->hw_started)
		return;
	g->grbm_soft_reset = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK |
			     GRBM_SOFT_RESET__SOFT_RESET_CPF_MASK | GRBM_SOFT_RESET__SOFT_RESET_CPC_MASK |
			     GRBM_SOFT_RESET__SOFT_RESET_CPG_MASK;
	g->srbm_soft_reset = SRBM_SOFT_RESET__SOFT_RESET_GRBM_MASK;
}

static int gfx_v8_0_pre_soft_reset(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	uint32_t grbm_soft_reset = 0;

	if (!g || ((!g->grbm_soft_reset) &&
		   (!g->srbm_soft_reset)))
		return 0;

	grbm_soft_reset = g->grbm_soft_reset;

	/* stop the rlc */
	gfx_v8_0_rlc_stop(adev);

	if (REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CP) ||
	    REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_GFX))
		/* Disable GFX parsing/prefetching */
		gfx_v8_0_cp_gfx_enable(adev, 0);

	if (REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CP) ||
	    REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CPF) ||
	    REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CPC) ||
	    REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CPG)) {
		/* the compute rings' HQDs (gfx_v8_0_deactivate_hqd): none, KCQ is not ported */
		/* Disable MEC parsing/prefetching */
		gfx_v8_0_cp_compute_enable(adev, 0);
	}

	return 0;
}

static int gfx_v8_0_soft_reset(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	uint32_t grbm_soft_reset = 0, srbm_soft_reset = 0;
	uint32_t tmp;

	if (!g || ((!g->grbm_soft_reset) &&
		   (!g->srbm_soft_reset)))
		return 0;

	grbm_soft_reset = g->grbm_soft_reset;
	srbm_soft_reset = g->srbm_soft_reset;

	if (grbm_soft_reset || srbm_soft_reset) {
		tmp = RREG32(mmGMCON_DEBUG);
		tmp = REG_SET_FIELD(tmp, GMCON_DEBUG, GFX_STALL, 1);
		tmp = REG_SET_FIELD(tmp, GMCON_DEBUG, GFX_CLEAR, 1);
		WREG32(mmGMCON_DEBUG, tmp);
		udelay(50);
	}

	if (grbm_soft_reset) {
		tmp = RREG32(mmGRBM_SOFT_RESET);
		tmp |= grbm_soft_reset;
		frb_info(adev, "GRBM_SOFT_RESET=0x%08X", tmp);
		WREG32(mmGRBM_SOFT_RESET, tmp);
		tmp = RREG32(mmGRBM_SOFT_RESET);

		udelay(50);

		tmp &= ~grbm_soft_reset;
		WREG32(mmGRBM_SOFT_RESET, tmp);
		tmp = RREG32(mmGRBM_SOFT_RESET);
	}

	if (srbm_soft_reset) {
		tmp = RREG32(mmSRBM_SOFT_RESET);
		tmp |= srbm_soft_reset;
		frb_info(adev, "SRBM_SOFT_RESET=0x%08X", tmp);
		WREG32(mmSRBM_SOFT_RESET, tmp);
		tmp = RREG32(mmSRBM_SOFT_RESET);

		udelay(50);

		tmp &= ~srbm_soft_reset;
		WREG32(mmSRBM_SOFT_RESET, tmp);
		tmp = RREG32(mmSRBM_SOFT_RESET);
	}

	if (grbm_soft_reset || srbm_soft_reset) {
		tmp = RREG32(mmGMCON_DEBUG);
		tmp = REG_SET_FIELD(tmp, GMCON_DEBUG, GFX_STALL, 0);
		tmp = REG_SET_FIELD(tmp, GMCON_DEBUG, GFX_CLEAR, 0);
		WREG32(mmGMCON_DEBUG, tmp);
	}

	/* Wait a little for things to settle down */
	udelay(50);

	return 0;
}

/* gfx_v8_0_post_soft_reset; the ring test's result is returned (amdgpu keeps it in the ring) */
static int gfx_v8_0_post_soft_reset(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;
	uint32_t grbm_soft_reset = 0;
	int r;

	if (!g || ((!g->grbm_soft_reset) &&
		   (!g->srbm_soft_reset)))
		return 0;

	grbm_soft_reset = g->grbm_soft_reset;

	/*
	 * CP/CPF/CPC/CPG: amdgpu deactivates the compute HQDs and resumes the
	 * KIQ and the KCQs here; none are ported (the MEC stays halted).
	 */

	if (REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_CP) ||
	    REG_GET_FIELD(grbm_soft_reset, GRBM_SOFT_RESET, SOFT_RESET_GFX))
		gfx_v8_0_cp_gfx_resume(adev);

	r = gfx_v8_0_cp_test_all_rings(adev);

	gfx_v8_0_rlc_start(adev);

	return r;
}

const struct frb_ip_reset_funcs frb_gfx_v8_0_reset_funcs = {
	.name = "gfx_v8_0",
	.check_soft_reset = gfx_v8_0_check_soft_reset,
	.force_soft_reset = gfx_v8_0_force_soft_reset,
	.pre_soft_reset = gfx_v8_0_pre_soft_reset,
	.soft_reset = gfx_v8_0_soft_reset,
	.post_soft_reset = gfx_v8_0_post_soft_reset,
};

/*
 * gfx_v8_0_ring_soft_recovery: kill the waves of a VMID (amdgpu_ring_soft_
 * recovery sends it until the hung job's fence signals, for 10 ms), for a
 * shader that never ends.
 */
void frb_gfx_v8_0_ring_soft_recovery(struct frb_ring *ring, unsigned vmid)
{
	struct frb_device *adev = ring->adev;
	uint32_t value = 0;

	value = REG_SET_FIELD(value, SQ_CMD, CMD, 0x03);
	value = REG_SET_FIELD(value, SQ_CMD, MODE, 0x01);
	value = REG_SET_FIELD(value, SQ_CMD, CHECK_VMID, 1);
	value = REG_SET_FIELD(value, SQ_CMD, VM_ID, vmid);
	WREG32(mmSQ_CMD, value);
}

/* --- for the accelerator ---------------------------------------------------- */

struct frb_ring *frb_gfx_v8_0_ring(struct frb_device *adev)
{
	struct frb_gfx *g = adev->gfx;

	return g && g->hw_started && g->gfx_ring.ready ? &g->gfx_ring : NULL;
}

int frb_gfx_v8_0_get_info(struct frb_device *adev, struct frb_gfx_info *info)
{
	struct frb_gfx *g = adev->gfx;

	memset(info, 0, sizeof(*info));
	if (!g || !g->sw_done)
		return -ENODEV;
	info->gfx_level = 8;
	info->num_se = g->config.max_shader_engines;
	info->num_sh_per_se = g->config.max_sh_per_se;
	info->max_cu_per_sh = g->config.max_cu_per_sh;
	info->num_cu = g->cu_info.number;
	info->num_rbs = g->config.num_rbs;
	info->backend_enable_mask = g->config.backend_enable_mask;
	info->max_backends_per_se = g->config.max_backends_per_se;
	info->max_tile_pipes = g->config.max_tile_pipes;
	info->gb_addr_config = g->config.gb_addr_config;
	info->mc_arb_ramcfg = g->config.mc_arb_ramcfg;
	info->max_hw_contexts = g->config.max_hw_contexts;
	info->ce_ram_size = g->ce_ram_size;
	return 0;
}

/*
 * vi_get_register_value's cached arms: what gfx_v8_0 keeps of the
 * registers vi_allowed_read_registers lets user space read (the SE/SH
 * indexed RB registers as rb_config[se][sh], the tiling tables). 0 and the
 * value, or -ENOENT when this block keeps no copy (read the register).
 */
int frb_gfx_v8_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value)
{
	struct frb_gfx *g = adev->gfx;
	unsigned se_idx = (se_num == 0xffffffff) ? 0 : se_num;
	unsigned sh_idx = (sh_num == 0xffffffff) ? 0 : sh_num;

	if (!g || !g->sw_done || !g->hw_started)
		return -ENOENT;
	if (se_idx >= AMDGPU_GFX_MAX_SE || sh_idx >= AMDGPU_GFX_MAX_SH_PER_SE)
		return -EINVAL;
	switch (reg_offset) {
	case mmCC_RB_BACKEND_DISABLE:
		*value = g->config.rb_config[se_idx][sh_idx].rb_backend_disable;
		return 0;
	case mmGC_USER_RB_BACKEND_DISABLE:
		*value = g->config.rb_config[se_idx][sh_idx].user_rb_backend_disable;
		return 0;
	case mmPA_SC_RASTER_CONFIG:
		*value = g->config.rb_config[se_idx][sh_idx].raster_config;
		return 0;
	case mmPA_SC_RASTER_CONFIG_1:
		*value = g->config.rb_config[se_idx][sh_idx].raster_config_1;
		return 0;
	case mmGB_ADDR_CONFIG:
		*value = g->config.gb_addr_config;
		return 0;
	case mmMC_ARB_RAMCFG:
		*value = g->config.mc_arb_ramcfg;
		return 0;
	default:
		break;
	}
	if (reg_offset >= mmGB_TILE_MODE0 && reg_offset <= mmGB_TILE_MODE31) {
		*value = g->config.tile_mode_array[reg_offset - mmGB_TILE_MODE0];
		return 0;
	}
	if (reg_offset >= mmGB_MACROTILE_MODE0 && reg_offset <= mmGB_MACROTILE_MODE15) {
		*value = g->config.macrotile_mode_array[reg_offset - mmGB_MACROTILE_MODE0];
		return 0;
	}
	return -ENOENT;
}
