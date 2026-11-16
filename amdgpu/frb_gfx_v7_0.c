/*
 * gfx_v7_0.c (Linux 7.0.2), for Sea Islands (the FirePro W8100, Hawaii):
 * the GFX 7 block's sw_init and hw_init up to the gfx ring test, the IB
 * test, clock gating, and hw_fini's halt.
 *
 * What is ported, in amdgpu's order:
 *   sw_init:  gfx_v7_0_init_microcode (pfp, me, ce, rlc: amdgpu's
 *             hawaii_*.bin, the headered images gfx_v7_0 asks for),
 *             gfx_v7_0_rlc_init (the clear-state block from clearstate_ci.h;
 *             a dGPU has no save/restore list in amdgpu, so none; the CP
 *             jump table is not made, see below; the SPM VMID), the gfx
 *             ring (1024 dwords), gfx_v7_0_gpu_early_init (the chip's
 *             limits, GB_ADDR_CONFIG with the row size from MC_ARB_RAMCFG)
 *   hw_init:  gfx_v7_0_constants_init (GB_ADDR_CONFIG and its HDP/DMIF
 *             copies, the tiling and macrotiling tables, setup_rb, cu info,
 *             the SH_MEM apertures per VMID through cik_srbm_select, the
 *             compute and GDS VMIDs, the 3D engine's defaults), the CSB,
 *             gfx_v7_0_rlc_resume (stop, CG off, reset, init_pg, load
 *             balancing, the RLC image through RLC_GPM_UCODE_ADDR/DATA,
 *             start), gfx_v7_0_cp_resume's gfx part (PFP/CE/ME images by
 *             MMIO, cp_gfx_resume, cp_gfx_start with the clear state, the
 *             gfx ring test, the GUI idle interrupt)
 *   late:     gfx_v7_0_ring_test_ib (frbctl test gfx ib), and amdgpu's late
 *             init gate: gfx_v7_0_set_clockgating_state (frb_cik.c's stage
 *             sdma: MGCG under the RLC; Hawaii's cg_flags have no GFX_CGCG,
 *             so coarse-grain gating is turned off, which also undoes the
 *             golden table's RLC_CGCG_CGLS_CTRL 0x0020003f)
 *   hw_fini:  the CP halted (gfx and the MEC), the RLC stopped, fini_pg
 * As on SI the CP and RLC images go in by MMIO (CIK's SMC loads nothing).
 *
 * NOT PORTED (yet): the compute rings and the MEC (hawaii_mec.bin is not
 * loaded; the MEC is halted at cp_resume and at hw_fini, as amdgpu's
 * cp_compute_load_microcode and hw_fini halt it), interrupts (IH, the
 * EOP/privileged-fault sources), the soft reset (frb_reset.c refuses on CIK),
 * power gating (pg_flags 0 on Hawaii and Bonaire, so init_pg and fini_pg do
 * nothing; their code is carried), the other CIK chips' rows (Bonaire's arms
 * are carried, untested; the APUs' are not).
 *
 * Where Firebrand departs from amdgpu, and why:
 * - PLACEMENT. Ring, writeback, IB pool and CSB in VRAM (frb_ring.c's
 *   objects region), as on GFX 6 and 8.
 * - BYTE ORDER: amdgpu's little-endian arrangement, as on SI and VI. Ring,
 *   IB, CSB and writeback are little-endian dwords (frb_ring_write,
 *   frb_ib_write, frb_vram_write32); CP_RB0_CNTL.BUF_SWAP stays 0 and the
 *   INDIRECT_BUFFER's swap field stays 0 where amdgpu's __BIG_ENDIAN arm sets
 *   2, and CP_ENDIAN_SWAP is 0 (amdgpu's cp_gfx_start writes 0 either way).
 *   So clients' IBs are little-endian too (RDN_VRAM_INFO64_LE_IBS). RadeonNI
 *   ran this card the other way, a host-order ring with BUF_SWAP_32BIT and
 *   host-order IBs under CP_ENDIAN_SWAP 2 (and measured an LE buffer hanging
 *   under that setting); frbctl test gfx ibprobe measures the swap field
 *   under an LE ring on the card, as it did on SI.
 * - PADDING. amdgpu fills the ring and pads IBs with PACKET3(NOP, 0x3FFF).
 *   RadeonNI measured on this card (radeon's hawaii_pfp/me/ce, 2026-11-03):
 *   inside an IB that NOP stalls the ME (CP_STALLED_STAT2 0x4000, waiting
 *   for data from the PFP) and the fence behind it never comes; two of them
 *   in the ring before an end-of-pipe event did the same. So the ring is
 *   padded with one counted NOP, PACKET3(NOP, n - 2) and n - 1 words (a
 *   type-2 NOP for one word: Mesa's amdgpu_pad_gfx_compute_ib, RadeonNI's
 *   rdn_ring_commit), and IBs with the type-2 NOP 0x80000000 (the ring's
 *   nop, which frb_ring_generic_pad_ib writes). amdgpu's newer CP images
 *   (the ones loaded here) have not been measured either way.
 * - The CP JUMP TABLE (amdgpu_gfx_rlc_init_cpt) is not made: only the
 *   RLC's power gating reads it (RLC_JUMP_TABLE_RESTORE in
 *   gfx_v7_0_init_gfx_cgpg, under AMD_PG_SUPPORT_GFX_PG), and Hawaii's
 *   pg_flags are 0.
 * - The SH_MEM loop takes amdgpu's values for this chip as constants:
 *   vm_manager.id_mgr[0].num_ids is first_kfd_vmid (8: gmc_v7_0_sw_init,
 *   amdgpu_vmid_mgr_init for GC < 10) and gmc.shared_aperture_start is
 *   gmc_v7_0's 0x2000000000000000.
 * - HDP. The ring flushes the HDP with gfx_v7_0's GPU_HDP_FLUSH_REQ/DONE
 *   wait (emit_hdp_flush) and invalidates it with a register write
 *   (cik_invalidate_hdp's ring arm, emit_wreg), as amdgpu_ib_schedule does.
 * - gmc_7_1_d.h for MC_ARB_RAMCFG where gfx_v7_0.c includes gmc_7_0_d.h
 *   (the same offset; frb_gmc_v7_0.c has 7_1, as Linux's gmc_v7_0.c).
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
#include "include/clearstate_defs.h"
#include "include/clearstate_ci.h"
#include "include/cikd.h"
#include "include/asic_reg/dce/dce_8_0_d.h"
#include "include/asic_reg/dce/dce_8_0_sh_mask.h"
#include "include/asic_reg/bif/bif_4_1_d.h"
#include "include/asic_reg/bif/bif_4_1_sh_mask.h"
#include "include/asic_reg/gca/gfx_7_0_d.h"
#include "include/asic_reg/gca/gfx_7_2_enum.h"
#include "include/asic_reg/gca/gfx_7_2_sh_mask.h"
#include "include/asic_reg/gmc/gmc_7_1_d.h"
#include "include/asic_reg/gmc/gmc_7_1_sh_mask.h"
#include "include/asic_reg/oss/oss_2_0_d.h"
#include "include/asic_reg/oss/oss_2_0_sh_mask.h"

#ifndef lower_32_bits
#define lower_32_bits(n)	((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n)	((uint32_t)(((n) >> 16) >> 16))
#endif

/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_IB_FLAG_CE	(1 << 0)

#define NUM_SIMD_PER_CU 0x4 /* missing from the gfx_7 IP headers */

#define BONAIRE_GB_ADDR_CONFIG_GOLDEN        0x12010001
#define HAWAII_GB_ADDR_CONFIG_GOLDEN         0x12011003

/* amdgpu.h, gmc_v7_0_sw_init, amdgpu_vmid_mgr_init (see the header) */
#define AMDGPU_NUM_VMID			16
#define AMDGPU_FIRST_KFD_VMID		8
#define GMC_V7_SHARED_APERTURE_START	0x2000000000000000ULL
#define DEFAULT_SH_MEM_BASES		(0x6000)

/* amdgpu_gfx.h */
#define GFX7_MAX_SE		4
#define GFX7_MAX_SH_PER_SE	2

/* the type-2 NOP: one dword of filler (see the header, PADDING) */
#define GFX7_TYPE2_NOP		0x80000000u

static unsigned min_u(unsigned a, unsigned b) { return a < b ? a : b; }
static unsigned max_u(unsigned a, unsigned b) { return a > b ? a : b; }

static uint32_t hweight32(uint32_t w)
{
	uint32_t n = 0;

	for (; w; w &= w - 1)
		n++;
	return n;
}

/* amdgpu_gfx_create_bitmask */
static uint32_t gfx7_create_bitmask(uint32_t bit_width)
{
	return (uint32_t)((1ULL << bit_width) - 1);
}

/* include/linux/log2.h order_base_2 for the sizes here */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

struct frb_gfx7_rb_config {
	uint32_t rb_backend_disable;
	uint32_t user_rb_backend_disable;
	uint32_t raster_config;
	uint32_t raster_config_1;
};

/* amdgpu_gfx_config, the fields gfx_v7_0 sets */
struct frb_gfx7_config {
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
	struct frb_gfx7_rb_config rb_config[GFX7_MAX_SE][GFX7_MAX_SH_PER_SE];
};

/* amdgpu_cu_info, one XCC */
struct frb_gfx7_cu_info {
	uint32_t number;
	uint32_t ao_cu_mask;
	uint32_t simd_per_cu, max_waves_per_simd, max_scratch_slots_per_cu, wave_front_size, lds_size;
	uint32_t bitmap[1][4][4];
	uint32_t ao_cu_bitmap[4][2];
};

/* amdgpu_rlc, the fields gfx_v7_0 uses */
struct frb_gfx7_rlc {
	const uint32_t *reg_list;		/* the APUs' save/restore lists; NULL on a dGPU */
	uint32_t reg_list_size;
	uint64_t save_restore_gpu_addr;		/* none: no list */
	const struct cs_section_def *cs_data;
	uint32_t clear_state_size;		/* dwords */
	uint32_t clear_state_offset;		/* BAR0 offset */
	uint64_t clear_state_gpu_addr;
	uint64_t cp_table_gpu_addr;		/* not made (see the header): 0 */
};

/* the CP and RLC images (adev->gfx.*_fw and their versions) */
struct frb_gfx7_fw {
	struct frb_ucode u;
	uint32_t feature_version;
};

/* adev->gfx, the gfx_v7_0 subset */
struct frb_gfx7 {
	struct frb_gfx7_config config;
	struct frb_gfx7_cu_info cu_info;
	struct frb_gfx7_rlc rlc;
	struct frb_gfx7_fw pfp_fw, me_fw, ce_fw, rlc_fw;
	struct frb_ring gfx_ring;
	uint32_t ce_ram_size;
	int sw_done;
	int hw_started;			/* hw_init began (hw_fini halts the CP and the RLC) */
	int constants_done;		/* constants_init ran: the RB and tiling copies are good */
	int cg_gated;			/* frb_gfx_v7_0_set_clockgating_state(1) ran */
};

/* amdgpu_gfx.h struct amdgpu_gds_reg_offset */
struct gfx7_gds_reg_offset {
	uint32_t mem_base;
	uint32_t mem_size;
	uint32_t gws;
	uint32_t oa;
};

static const struct gfx7_gds_reg_offset gfx7_gds_reg_offset[] = {
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

static const struct frb_ring_funcs gfx_v7_0_ring_funcs_gfx;

static void gfx_v7_0_get_cu_info(struct frb_device *adev);
static uint32_t gfx_v7_0_get_csb_size(struct frb_device *adev);
static void gfx_v7_0_init_pg(struct frb_device *adev);

/* --- amdgpu's functions, as amdgpu has them ----------------------------------- */

static void gfx_v7_0_enable_gui_idle_interrupt(struct frb_device *adev,
					       int enable)
{
	uint32_t tmp = RREG32(mmCP_INT_CNTL_RING0);

	if (enable)
		tmp |= (CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE_MASK |
				CP_INT_CNTL_RING0__CNTX_EMPTY_INT_ENABLE_MASK);
	else
		tmp &= ~(CP_INT_CNTL_RING0__CNTX_BUSY_INT_ENABLE_MASK |
				CP_INT_CNTL_RING0__CNTX_EMPTY_INT_ENABLE_MASK);
	WREG32(mmCP_INT_CNTL_RING0, tmp);
}

static void gfx_v7_0_tiling_mode_table_init(struct frb_device *adev)
{
	const uint32_t num_tile_mode_states =
			ARRAY_SIZE(adev->gfx7->config.tile_mode_array);
	const uint32_t num_secondary_tile_mode_states =
			ARRAY_SIZE(adev->gfx7->config.macrotile_mode_array);
	uint32_t reg_offset, split_equal_to_row_size;
	uint32_t *tile, *macrotile;

	tile = adev->gfx7->config.tile_mode_array;
	macrotile = adev->gfx7->config.macrotile_mode_array;

	switch (adev->gfx7->config.mem_row_size_in_kb) {
	case 1:
		split_equal_to_row_size = ADDR_SURF_TILE_SPLIT_1KB;
		break;
	case 2:
	default:
		split_equal_to_row_size = ADDR_SURF_TILE_SPLIT_2KB;
		break;
	case 4:
		split_equal_to_row_size = ADDR_SURF_TILE_SPLIT_4KB;
		break;
	}

	for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
		tile[reg_offset] = 0;
	for (reg_offset = 0; reg_offset < num_secondary_tile_mode_states; reg_offset++)
		macrotile[reg_offset] = 0;

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		tile[0] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[1] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[2] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[3] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[4] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[5] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[6] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[7] = (TILE_SPLIT(split_equal_to_row_size));
		tile[8] = (ARRAY_MODE(ARRAY_LINEAR_ALIGNED) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16));
		tile[9] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING));
		tile[10] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[11] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[12] = (TILE_SPLIT(split_equal_to_row_size));
		tile[13] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING));
		tile[14] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[15] = (ARRAY_MODE(ARRAY_3D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[16] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[17] = (TILE_SPLIT(split_equal_to_row_size));
		tile[18] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[19] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING));
		tile[20] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[21] =  (ARRAY_MODE(ARRAY_3D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[22] = (ARRAY_MODE(ARRAY_PRT_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[23] = (TILE_SPLIT(split_equal_to_row_size));
		tile[24] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[25] = (ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[26] = (ARRAY_MODE(ARRAY_3D_TILED_XTHICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[27] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING));
		tile[28] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[29] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[30] = (TILE_SPLIT(split_equal_to_row_size));

		macrotile[0] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[1] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[2] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[3] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[4] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[5] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_8_BANK));
		macrotile[6] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));
		macrotile[8] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_8) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[9] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[10] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[11] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[12] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[13] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_8_BANK));
		macrotile[14] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));

		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tile[reg_offset]);
		for (reg_offset = 0; reg_offset < num_secondary_tile_mode_states; reg_offset++)
			if (reg_offset != 7)
				WREG32(mmGB_MACROTILE_MODE0 + reg_offset, macrotile[reg_offset]);
		break;
	case CHIP_HAWAII:
		tile[0] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[1] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[2] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[3] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING));
		tile[4] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[5] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[6] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[7] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DEPTH_MICRO_TILING) |
			   TILE_SPLIT(split_equal_to_row_size));
		tile[8] = (ARRAY_MODE(ARRAY_LINEAR_ALIGNED) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16));
		tile[9] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			   PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			   MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING));
		tile[10] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[11] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[12] = (ARRAY_MODE(ARRAY_PRT_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_DISPLAY_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[13] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING));
		tile[14] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[15] = (ARRAY_MODE(ARRAY_3D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[16] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[17] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[18] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[19] = (ARRAY_MODE(ARRAY_1D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING));
		tile[20] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[21] = (ARRAY_MODE(ARRAY_3D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[22] = (ARRAY_MODE(ARRAY_PRT_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[23] = (ARRAY_MODE(ARRAY_PRT_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[24] = (ARRAY_MODE(ARRAY_2D_TILED_THICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THIN_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[25] = (ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[26] = (ARRAY_MODE(ARRAY_3D_TILED_XTHICK) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_THICK_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_1));
		tile[27] = (ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING));
		tile[28] = (ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_2));
		tile[29] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P16_32x32_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));
		tile[30] = (ARRAY_MODE(ARRAY_PRT_TILED_THIN1) |
			    PIPE_CONFIG(ADDR_SURF_P4_16x16) |
			    MICRO_TILE_MODE_NEW(ADDR_SURF_ROTATED_MICRO_TILING) |
			    SAMPLE_SPLIT(ADDR_SURF_SAMPLE_SPLIT_8));

		macrotile[0] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[1] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[2] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[3] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[4] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK));
		macrotile[5] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));
		macrotile[6] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));
		macrotile[8] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[9] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[10] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[11] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK));
		macrotile[12] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK));
		macrotile[13] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_8_BANK));
		macrotile[14] = (BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK));

		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tile[reg_offset]);
		for (reg_offset = 0; reg_offset < num_secondary_tile_mode_states; reg_offset++)
			if (reg_offset != 7)
				WREG32(mmGB_MACROTILE_MODE0 + reg_offset, macrotile[reg_offset]);
		break;
	default:
		/* Kaveri, Kabini, Mullins: APUs, never in a PCIe slot */
		frb_err(adev, "gfx7: no tiling table for asic 0x%x here", adev->asic_type);
		break;
	}
}

static void gfx_v7_0_select_se_sh(struct frb_device *adev,
				  uint32_t se_num, uint32_t sh_num, uint32_t instance,
				  int xcc_id)
{
	uint32_t data;

	if (instance == 0xffffffff)
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX, instance);

	if ((se_num == 0xffffffff) && (sh_num == 0xffffffff))
		data |= GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK |
			GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK;
	else if (se_num == 0xffffffff)
		data |= GRBM_GFX_INDEX__SE_BROADCAST_WRITES_MASK |
			(sh_num << GRBM_GFX_INDEX__SH_INDEX__SHIFT);
	else if (sh_num == 0xffffffff)
		data |= GRBM_GFX_INDEX__SH_BROADCAST_WRITES_MASK |
			(se_num << GRBM_GFX_INDEX__SE_INDEX__SHIFT);
	else
		data |= (sh_num << GRBM_GFX_INDEX__SH_INDEX__SHIFT) |
			(se_num << GRBM_GFX_INDEX__SE_INDEX__SHIFT);
	WREG32(mmGRBM_GFX_INDEX, data);
}

/**
 * gfx_v7_0_get_rb_active_bitmap - computes the mask of enabled RBs
 *
 * @adev: amdgpu_device pointer
 *
 * Calculates the bitmask of enabled RBs (CIK).
 * Returns the enabled RB bitmask.
 */
static uint32_t gfx_v7_0_get_rb_active_bitmap(struct frb_device *adev)
{
	uint32_t data, mask;

	data = RREG32(mmCC_RB_BACKEND_DISABLE);
	data |= RREG32(mmGC_USER_RB_BACKEND_DISABLE);

	data &= CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK;
	data >>= GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

	mask = gfx7_create_bitmask(adev->gfx7->config.max_backends_per_se /
					 adev->gfx7->config.max_sh_per_se);

	return (~data) & mask;
}

static void
gfx_v7_0_raster_config(struct frb_device *adev, uint32_t *rconf, uint32_t *rconf1)
{
	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		*rconf |= RB_MAP_PKR0(2) | RB_XSEL2(1) | SE_MAP(2) |
			  SE_XSEL(1) | SE_YSEL(1);
		*rconf1 |= 0x0;
		break;
	case CHIP_HAWAII:
		*rconf |= RB_MAP_PKR0(2) | RB_MAP_PKR1(2) |
			  RB_XSEL2(1) | PKR_MAP(2) | PKR_XSEL(1) |
			  PKR_YSEL(1) | SE_MAP(2) | SE_XSEL(2) |
			  SE_YSEL(3);
		*rconf1 |= SE_PAIR_MAP(2) | SE_PAIR_XSEL(3) |
			   SE_PAIR_YSEL(2);
		break;
	case CHIP_KAVERI:
		*rconf |= RB_MAP_PKR0(2);
		*rconf1 |= 0x0;
		break;
	case CHIP_KABINI:
	case CHIP_MULLINS:
		*rconf |= 0x0;
		*rconf1 |= 0x0;
		break;
	default:
		frb_err(adev, "gfx7: unknown asic: 0x%x", adev->asic_type);
		break;
	}
}

static void
gfx_v7_0_write_harvested_raster_configs(struct frb_device *adev,
					uint32_t raster_config, uint32_t raster_config_1,
					unsigned rb_mask, unsigned num_rb)
{
	unsigned sh_per_se = max_u(adev->gfx7->config.max_sh_per_se, 1);
	unsigned num_se = max_u(adev->gfx7->config.max_shader_engines, 1);
	unsigned rb_per_pkr = min_u(num_rb / num_se / sh_per_se, 2);
	unsigned rb_per_se = num_rb / num_se;
	unsigned se_mask[4];
	unsigned se;

	se_mask[0] = ((1 << rb_per_se) - 1) & rb_mask;
	se_mask[1] = (se_mask[0] << rb_per_se) & rb_mask;
	se_mask[2] = (se_mask[1] << rb_per_se) & rb_mask;
	se_mask[3] = (se_mask[2] << rb_per_se) & rb_mask;


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

		/* GRBM_GFX_INDEX has a different offset on CI+ */
		gfx_v7_0_select_se_sh(adev, se, 0xffffffff, 0xffffffff, 0);
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config_se);
		WREG32(mmPA_SC_RASTER_CONFIG_1, raster_config_1);
	}

	/* GRBM_GFX_INDEX has a different offset on CI+ */
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

/**
 * gfx_v7_0_setup_rb - setup the RBs on the asic
 *
 * @adev: amdgpu_device pointer
 *
 * Configures per-SE/SH RB registers (CIK).
 */
static void gfx_v7_0_setup_rb(struct frb_device *adev)
{
	int i, j;
	uint32_t data;
	uint32_t raster_config = 0, raster_config_1 = 0;
	uint32_t active_rbs = 0;
	uint32_t rb_bitmap_width_per_sh = adev->gfx7->config.max_backends_per_se /
					adev->gfx7->config.max_sh_per_se;
	unsigned num_rb_pipes;

	for (i = 0; i < adev->gfx7->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx7->config.max_sh_per_se; j++) {
			gfx_v7_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			data = gfx_v7_0_get_rb_active_bitmap(adev);
			active_rbs |= data << ((i * adev->gfx7->config.max_sh_per_se + j) *
					       rb_bitmap_width_per_sh);
		}
	}
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	adev->gfx7->config.backend_enable_mask = active_rbs;
	adev->gfx7->config.num_rbs = hweight32(active_rbs);

	num_rb_pipes = min_u(adev->gfx7->config.max_backends_per_se *
			     adev->gfx7->config.max_shader_engines, 16);

	gfx_v7_0_raster_config(adev, &raster_config, &raster_config_1);

	if (!adev->gfx7->config.backend_enable_mask ||
			adev->gfx7->config.num_rbs >= num_rb_pipes) {
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config);
		WREG32(mmPA_SC_RASTER_CONFIG_1, raster_config_1);
	} else {
		gfx_v7_0_write_harvested_raster_configs(adev, raster_config, raster_config_1,
							adev->gfx7->config.backend_enable_mask,
							num_rb_pipes);
	}

	/* cache the values for userspace */
	for (i = 0; i < adev->gfx7->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx7->config.max_sh_per_se; j++) {
			gfx_v7_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			adev->gfx7->config.rb_config[i][j].rb_backend_disable =
				RREG32(mmCC_RB_BACKEND_DISABLE);
			adev->gfx7->config.rb_config[i][j].user_rb_backend_disable =
				RREG32(mmGC_USER_RB_BACKEND_DISABLE);
			adev->gfx7->config.rb_config[i][j].raster_config =
				RREG32(mmPA_SC_RASTER_CONFIG);
			adev->gfx7->config.rb_config[i][j].raster_config_1 =
				RREG32(mmPA_SC_RASTER_CONFIG_1);
		}
	}
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v7_0_init_compute_vmid(struct frb_device *adev)
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
	sh_mem_config = SH_MEM_ALIGNMENT_MODE_UNALIGNED <<
			SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT;
	sh_mem_config |= MTYPE_NONCACHED << SH_MEM_CONFIG__DEFAULT_MTYPE__SHIFT;
	for (i = AMDGPU_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		frb_cik_srbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		WREG32(mmSH_MEM_CONFIG, sh_mem_config);
		WREG32(mmSH_MEM_APE1_BASE, 1);
		WREG32(mmSH_MEM_APE1_LIMIT, 0);
		WREG32(mmSH_MEM_BASES, sh_mem_bases);
	}
	frb_cik_srbm_select(adev, 0, 0, 0, 0);

	/* Initialize all compute VMIDs to have no GDS, GWS, or OA
	   access. These should be enabled by FW for target VMIDs. */
	for (i = AMDGPU_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		WREG32(gfx7_gds_reg_offset[i].mem_base, 0);
		WREG32(gfx7_gds_reg_offset[i].mem_size, 0);
		WREG32(gfx7_gds_reg_offset[i].gws, 0);
		WREG32(gfx7_gds_reg_offset[i].oa, 0);
	}
}

static void gfx_v7_0_init_gds_vmid(struct frb_device *adev)
{
	int vmid;

	/*
	 * Initialize all compute and user-gfx VMIDs to have no GDS, GWS, or OA
	 * access. Compute VMIDs should be enabled by FW for target VMIDs,
	 * the driver can enable them for graphics. VMID0 should maintain
	 * access so that HWS firmware can save/restore entries.
	 */
	for (vmid = 1; vmid < AMDGPU_NUM_VMID; vmid++) {
		WREG32(gfx7_gds_reg_offset[vmid].mem_base, 0);
		WREG32(gfx7_gds_reg_offset[vmid].mem_size, 0);
		WREG32(gfx7_gds_reg_offset[vmid].gws, 0);
		WREG32(gfx7_gds_reg_offset[vmid].oa, 0);
	}
}

static void gfx_v7_0_config_init(struct frb_device *adev)
{
	adev->gfx7->config.double_offchip_lds_buf = 1;
}

/**
 * gfx_v7_0_constants_init - setup the 3D engine
 *
 * @adev: amdgpu_device pointer
 *
 * init the gfx constants such as the 3D engine, tiling configuration
 * registers, maximum number of quad pipes, render backends...
 */
static void gfx_v7_0_constants_init(struct frb_device *adev)
{
	uint32_t sh_mem_cfg, sh_static_mem_cfg, sh_mem_base;
	uint32_t tmp;
	int i;

	WREG32(mmGRBM_CNTL, (0xff << GRBM_CNTL__READ_TIMEOUT__SHIFT));

	WREG32(mmGB_ADDR_CONFIG, adev->gfx7->config.gb_addr_config);
	WREG32(mmHDP_ADDR_CONFIG, adev->gfx7->config.gb_addr_config);
	WREG32(mmDMIF_ADDR_CALC, adev->gfx7->config.gb_addr_config);

	gfx_v7_0_tiling_mode_table_init(adev);

	gfx_v7_0_setup_rb(adev);
	gfx_v7_0_get_cu_info(adev);
	gfx_v7_0_config_init(adev);

	/* set HW defaults for 3D engine */
	WREG32(mmCP_MEQ_THRESHOLDS,
	       (0x30 << CP_MEQ_THRESHOLDS__MEQ1_START__SHIFT) |
	       (0x60 << CP_MEQ_THRESHOLDS__MEQ2_START__SHIFT));

	/*
	 * making sure that the following register writes will be broadcasted
	 * to all the shaders
	 */
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	/* XXX SH_MEM regs */
	/* where to put LDS, scratch, GPUVM in FSA64 space */
	sh_mem_cfg = REG_SET_FIELD(0, SH_MEM_CONFIG, ALIGNMENT_MODE,
				   SH_MEM_ALIGNMENT_MODE_UNALIGNED);
	sh_mem_cfg = REG_SET_FIELD(sh_mem_cfg, SH_MEM_CONFIG, DEFAULT_MTYPE,
				   MTYPE_NC);
	sh_mem_cfg = REG_SET_FIELD(sh_mem_cfg, SH_MEM_CONFIG, APE1_MTYPE,
				   MTYPE_UC);
	sh_mem_cfg = REG_SET_FIELD(sh_mem_cfg, SH_MEM_CONFIG, PRIVATE_ATC, 0);

	sh_static_mem_cfg = REG_SET_FIELD(0, SH_STATIC_MEM_CONFIG,
				   SWIZZLE_ENABLE, 1);
	sh_static_mem_cfg = REG_SET_FIELD(sh_static_mem_cfg, SH_STATIC_MEM_CONFIG,
				   ELEMENT_SIZE, 1);
	sh_static_mem_cfg = REG_SET_FIELD(sh_static_mem_cfg, SH_STATIC_MEM_CONFIG,
				   INDEX_STRIDE, 3);
	WREG32(mmSH_STATIC_MEM_CONFIG, sh_static_mem_cfg);

	for (i = 0; i < AMDGPU_FIRST_KFD_VMID; i++) {
		if (i == 0)
			sh_mem_base = 0;
		else
			sh_mem_base = GMC_V7_SHARED_APERTURE_START >> 48;
		frb_cik_srbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		WREG32(mmSH_MEM_CONFIG, sh_mem_cfg);
		WREG32(mmSH_MEM_APE1_BASE, 1);
		WREG32(mmSH_MEM_APE1_LIMIT, 0);
		WREG32(mmSH_MEM_BASES, sh_mem_base);
	}
	frb_cik_srbm_select(adev, 0, 0, 0, 0);

	gfx_v7_0_init_compute_vmid(adev);
	gfx_v7_0_init_gds_vmid(adev);

	WREG32(mmSX_DEBUG_1, 0x20);

	WREG32(mmTA_CNTL_AUX, 0x00010000);

	tmp = RREG32(mmSPI_CONFIG_CNTL);
	tmp |= 0x03000000;
	WREG32(mmSPI_CONFIG_CNTL, tmp);

	WREG32(mmSQ_CONFIG, 1);

	WREG32(mmDB_DEBUG, 0);

	tmp = RREG32(mmDB_DEBUG2) & ~0xf00fffff;
	tmp |= 0x00000400;
	WREG32(mmDB_DEBUG2, tmp);

	tmp = RREG32(mmDB_DEBUG3) & ~0x0002021c;
	tmp |= 0x00020200;
	WREG32(mmDB_DEBUG3, tmp);

	tmp = RREG32(mmCB_HW_CONTROL) & ~0x00010000;
	tmp |= 0x00018208;
	WREG32(mmCB_HW_CONTROL, tmp);

	WREG32(mmSPI_CONFIG_CNTL_1, (4 << SPI_CONFIG_CNTL_1__VTX_DONE_DELAY__SHIFT));

	WREG32(mmPA_SC_FIFO_SIZE,
		((adev->gfx7->config.sc_prim_fifo_size_frontend << PA_SC_FIFO_SIZE__SC_FRONTEND_PRIM_FIFO_SIZE__SHIFT) |
		(adev->gfx7->config.sc_prim_fifo_size_backend << PA_SC_FIFO_SIZE__SC_BACKEND_PRIM_FIFO_SIZE__SHIFT) |
		(adev->gfx7->config.sc_hiz_tile_fifo_size << PA_SC_FIFO_SIZE__SC_HIZ_TILE_FIFO_SIZE__SHIFT) |
		(adev->gfx7->config.sc_earlyz_tile_fifo_size << PA_SC_FIFO_SIZE__SC_EARLYZ_TILE_FIFO_SIZE__SHIFT)));

	WREG32(mmVGT_NUM_INSTANCES, 1);

	WREG32(mmCP_PERFMON_CNTL, 0);

	WREG32(mmSQ_CONFIG, 0);

	WREG32(mmPA_SC_FORCE_EOV_MAX_CNTS,
		((4095 << PA_SC_FORCE_EOV_MAX_CNTS__FORCE_EOV_MAX_CLK_CNT__SHIFT) |
		(255 << PA_SC_FORCE_EOV_MAX_CNTS__FORCE_EOV_MAX_REZ_CNT__SHIFT)));

	WREG32(mmVGT_CACHE_INVALIDATION,
		(VC_AND_TC << VGT_CACHE_INVALIDATION__CACHE_INVALIDATION__SHIFT) |
		(ES_AND_GS_AUTO << VGT_CACHE_INVALIDATION__AUTO_INVLD_EN__SHIFT));

	WREG32(mmVGT_GS_VERTEX_REUSE, 16);
	WREG32(mmPA_SC_LINE_STIPPLE_STATE, 0);

	WREG32(mmPA_CL_ENHANCE, PA_CL_ENHANCE__CLIP_VTX_REORDER_ENA_MASK |
			(3 << PA_CL_ENHANCE__NUM_CLIP_SEQ__SHIFT));
	WREG32(mmPA_SC_ENHANCE, PA_SC_ENHANCE__ENABLE_PA_SC_OUT_OF_ORDER_MASK);

	tmp = RREG32(mmSPI_ARB_PRIORITY);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS0, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS1, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS2, 2);
	tmp = REG_SET_FIELD(tmp, SPI_ARB_PRIORITY, PIPE_ORDER_TS3, 2);
	WREG32(mmSPI_ARB_PRIORITY, tmp);


	udelay(50);
}


static void gfx_v7_0_enable_lbpw(struct frb_device *adev, int enable)
{
	uint32_t tmp;

	tmp = RREG32(mmRLC_LB_CNTL);
	if (enable)
		tmp |= RLC_LB_CNTL__LOAD_BALANCE_ENABLE_MASK;
	else
		tmp &= ~RLC_LB_CNTL__LOAD_BALANCE_ENABLE_MASK;
	WREG32(mmRLC_LB_CNTL, tmp);
}

static void gfx_v7_0_wait_for_rlc_serdes(struct frb_device *adev)
{
	uint32_t i, j, k;
	uint32_t mask;

	for (i = 0; i < adev->gfx7->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx7->config.max_sh_per_se; j++) {
			gfx_v7_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			for (k = 0; k < adev->usec_timeout; k++) {
				if (RREG32(mmRLC_SERDES_CU_MASTER_BUSY) == 0)
					break;
				udelay(1);
			}
		}
	}
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

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

static void gfx_v7_0_update_rlc(struct frb_device *adev, uint32_t rlc)
{
	uint32_t tmp;

	tmp = RREG32(mmRLC_CNTL);
	if (tmp != rlc)
		WREG32(mmRLC_CNTL, rlc);
}

static uint32_t gfx_v7_0_halt_rlc(struct frb_device *adev)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_CNTL);

	if (data & RLC_CNTL__RLC_ENABLE_F32_MASK) {
		uint32_t i;

		data &= ~RLC_CNTL__RLC_ENABLE_F32_MASK;
		WREG32(mmRLC_CNTL, data);

		for (i = 0; i < adev->usec_timeout; i++) {
			if ((RREG32(mmRLC_GPM_STAT) & RLC_GPM_STAT__RLC_BUSY_MASK) == 0)
				break;
			udelay(1);
		}

		gfx_v7_0_wait_for_rlc_serdes(adev);
	}

	return orig;
}

/**
 * gfx_v7_0_rlc_stop - stop the RLC ME
 *
 * @adev: amdgpu_device pointer
 *
 * Halt the RLC ME (MicroEngine) (CIK).
 */
static void gfx_v7_0_rlc_stop(struct frb_device *adev)
{
	WREG32(mmRLC_CNTL, 0);

	gfx_v7_0_enable_gui_idle_interrupt(adev, 0);

	gfx_v7_0_wait_for_rlc_serdes(adev);
}

/**
 * gfx_v7_0_rlc_start - start the RLC ME
 *
 * @adev: amdgpu_device pointer
 *
 * Unhalt the RLC ME (MicroEngine) (CIK).
 */
static void gfx_v7_0_rlc_start(struct frb_device *adev)
{
	WREG32(mmRLC_CNTL, RLC_CNTL__RLC_ENABLE_F32_MASK);

	gfx_v7_0_enable_gui_idle_interrupt(adev, 1);

	udelay(50);
}

static void gfx_v7_0_rlc_reset(struct frb_device *adev)
{
	uint32_t tmp = RREG32(mmGRBM_SOFT_RESET);

	tmp |= GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK;
	WREG32(mmGRBM_SOFT_RESET, tmp);
	udelay(50);
	tmp &= ~GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK;
	WREG32(mmGRBM_SOFT_RESET, tmp);
	udelay(50);
}

static void gfx_v7_0_enable_cgcg(struct frb_device *adev, int enable)
{
	uint32_t data, orig, tmp, tmp2;

	orig = data = RREG32(mmRLC_CGCG_CGLS_CTRL);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)) {
		gfx_v7_0_enable_gui_idle_interrupt(adev, 1);

		tmp = gfx_v7_0_halt_rlc(adev);

		gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
		WREG32(mmRLC_SERDES_WR_CU_MASTER_MASK, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_NONCU_MASTER_MASK, 0xffffffff);
		tmp2 = RLC_SERDES_WR_CTRL__BPM_ADDR_MASK |
			RLC_SERDES_WR_CTRL__CGCG_OVERRIDE_0_MASK |
			RLC_SERDES_WR_CTRL__CGLS_ENABLE_MASK;
		WREG32(mmRLC_SERDES_WR_CTRL, tmp2);

		gfx_v7_0_update_rlc(adev, tmp);

		data |= RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;
		if (orig != data)
			WREG32(mmRLC_CGCG_CGLS_CTRL, data);

	} else {
		gfx_v7_0_enable_gui_idle_interrupt(adev, 0);

		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);

		data &= ~(RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK);
		if (orig != data)
			WREG32(mmRLC_CGCG_CGLS_CTRL, data);

		gfx_v7_0_enable_gui_idle_interrupt(adev, 1);
	}
}

static void gfx_v7_0_enable_mgcg(struct frb_device *adev, int enable)
{
	uint32_t data, orig, tmp = 0;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGCG)) {
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGLS) {
			if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CP_LS) {
				orig = data = RREG32(mmCP_MEM_SLP_CNTL);
				data |= CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
				if (orig != data)
					WREG32(mmCP_MEM_SLP_CNTL, data);
			}
		}

		orig = data = RREG32(mmRLC_CGTT_MGCG_OVERRIDE);
		data |= 0x00000001;
		data &= 0xfffffffd;
		if (orig != data)
			WREG32(mmRLC_CGTT_MGCG_OVERRIDE, data);

		tmp = gfx_v7_0_halt_rlc(adev);

		gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
		WREG32(mmRLC_SERDES_WR_CU_MASTER_MASK, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_NONCU_MASTER_MASK, 0xffffffff);
		data = RLC_SERDES_WR_CTRL__BPM_ADDR_MASK |
			RLC_SERDES_WR_CTRL__MGCG_OVERRIDE_0_MASK;
		WREG32(mmRLC_SERDES_WR_CTRL, data);

		gfx_v7_0_update_rlc(adev, tmp);

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGTS) {
			orig = data = RREG32(mmCGTS_SM_CTRL_REG);
			data &= ~CGTS_SM_CTRL_REG__SM_MODE_MASK;
			data |= (0x2 << CGTS_SM_CTRL_REG__SM_MODE__SHIFT);
			data |= CGTS_SM_CTRL_REG__SM_MODE_ENABLE_MASK;
			data &= ~CGTS_SM_CTRL_REG__OVERRIDE_MASK;
			if ((adev->cg_flags & AMD_CG_SUPPORT_GFX_MGLS) &&
			    (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGTS_LS))
				data &= ~CGTS_SM_CTRL_REG__LS_OVERRIDE_MASK;
			data &= ~CGTS_SM_CTRL_REG__ON_MONITOR_ADD_MASK;
			data |= CGTS_SM_CTRL_REG__ON_MONITOR_ADD_EN_MASK;
			data |= (0x96 << CGTS_SM_CTRL_REG__ON_MONITOR_ADD__SHIFT);
			if (orig != data)
				WREG32(mmCGTS_SM_CTRL_REG, data);
		}
	} else {
		orig = data = RREG32(mmRLC_CGTT_MGCG_OVERRIDE);
		data |= 0x00000003;
		if (orig != data)
			WREG32(mmRLC_CGTT_MGCG_OVERRIDE, data);

		data = RREG32(mmRLC_MEM_SLP_CNTL);
		if (data & RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK) {
			data &= ~RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK;
			WREG32(mmRLC_MEM_SLP_CNTL, data);
		}

		data = RREG32(mmCP_MEM_SLP_CNTL);
		if (data & CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK) {
			data &= ~CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
			WREG32(mmCP_MEM_SLP_CNTL, data);
		}

		orig = data = RREG32(mmCGTS_SM_CTRL_REG);
		data |= CGTS_SM_CTRL_REG__OVERRIDE_MASK | CGTS_SM_CTRL_REG__LS_OVERRIDE_MASK;
		if (orig != data)
			WREG32(mmCGTS_SM_CTRL_REG, data);

		tmp = gfx_v7_0_halt_rlc(adev);

		gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
		WREG32(mmRLC_SERDES_WR_CU_MASTER_MASK, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_NONCU_MASTER_MASK, 0xffffffff);
		data = RLC_SERDES_WR_CTRL__BPM_ADDR_MASK | RLC_SERDES_WR_CTRL__MGCG_OVERRIDE_1_MASK;
		WREG32(mmRLC_SERDES_WR_CTRL, data);

		gfx_v7_0_update_rlc(adev, tmp);
	}
}

static void gfx_v7_0_update_cg(struct frb_device *adev,
			       int enable)
{
	gfx_v7_0_enable_gui_idle_interrupt(adev, 0);
	/* order matters! */
	if (enable) {
		gfx_v7_0_enable_mgcg(adev, 1);
		gfx_v7_0_enable_cgcg(adev, 1);
	} else {
		gfx_v7_0_enable_cgcg(adev, 0);
		gfx_v7_0_enable_mgcg(adev, 0);
	}
	gfx_v7_0_enable_gui_idle_interrupt(adev, 1);
}

static void gfx_v7_0_enable_sclk_slowdown_on_pu(struct frb_device *adev,
						int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_RLC_SMU_HS))
		data |= RLC_PG_CNTL__SMU_CLK_SLOWDOWN_ON_PU_ENABLE_MASK;
	else
		data &= ~RLC_PG_CNTL__SMU_CLK_SLOWDOWN_ON_PU_ENABLE_MASK;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

static void gfx_v7_0_enable_sclk_slowdown_on_pd(struct frb_device *adev,
						int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_RLC_SMU_HS))
		data |= RLC_PG_CNTL__SMU_CLK_SLOWDOWN_ON_PD_ENABLE_MASK;
	else
		data &= ~RLC_PG_CNTL__SMU_CLK_SLOWDOWN_ON_PD_ENABLE_MASK;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

static void gfx_v7_0_enable_cp_pg(struct frb_device *adev, int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_CP))
		data &= ~0x8000;
	else
		data |= 0x8000;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

static void gfx_v7_0_enable_gds_pg(struct frb_device *adev, int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_GDS))
		data &= ~0x2000;
	else
		data |= 0x2000;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

static void gfx_v7_0_enable_gfx_cgpg(struct frb_device *adev,
				     int enable)
{
	uint32_t data, orig;

	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG)) {
		orig = data = RREG32(mmRLC_PG_CNTL);
		data |= RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK;
		if (orig != data)
			WREG32(mmRLC_PG_CNTL, data);

		orig = data = RREG32(mmRLC_AUTO_PG_CTRL);
		data |= RLC_AUTO_PG_CTRL__AUTO_PG_EN_MASK;
		if (orig != data)
			WREG32(mmRLC_AUTO_PG_CTRL, data);
	} else {
		orig = data = RREG32(mmRLC_PG_CNTL);
		data &= ~RLC_PG_CNTL__GFX_POWER_GATING_ENABLE_MASK;
		if (orig != data)
			WREG32(mmRLC_PG_CNTL, data);

		orig = data = RREG32(mmRLC_AUTO_PG_CTRL);
		data &= ~RLC_AUTO_PG_CTRL__AUTO_PG_EN_MASK;
		if (orig != data)
			WREG32(mmRLC_AUTO_PG_CTRL, data);

		data = RREG32(mmDB_RENDER_CONTROL);
	}
}

static void gfx_v7_0_set_user_cu_inactive_bitmap(struct frb_device *adev,
						 uint32_t bitmap)
{
	uint32_t data;

	if (!bitmap)
		return;

	data = bitmap << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;
	data &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;

	WREG32(mmGC_USER_SHADER_ARRAY_CONFIG, data);
}

static uint32_t gfx_v7_0_get_cu_active_bitmap(struct frb_device *adev)
{
	uint32_t data, mask;

	data = RREG32(mmCC_GC_SHADER_ARRAY_CONFIG);
	data |= RREG32(mmGC_USER_SHADER_ARRAY_CONFIG);

	data &= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;
	data >>= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;

	mask = gfx7_create_bitmask(adev->gfx7->config.max_cu_per_sh);

	return (~data) & mask;
}

static void gfx_v7_0_init_ao_cu_mask(struct frb_device *adev)
{
	uint32_t tmp;

	WREG32(mmRLC_PG_ALWAYS_ON_CU_MASK, adev->gfx7->cu_info.ao_cu_mask);

	tmp = RREG32(mmRLC_MAX_PG_CU);
	tmp &= ~RLC_MAX_PG_CU__MAX_POWERED_UP_CU_MASK;
	tmp |= (adev->gfx7->cu_info.number << RLC_MAX_PG_CU__MAX_POWERED_UP_CU__SHIFT);
	WREG32(mmRLC_MAX_PG_CU, tmp);
}

static void gfx_v7_0_enable_gfx_static_mgpg(struct frb_device *adev,
					    int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_GFX_SMG))
		data |= RLC_PG_CNTL__STATIC_PER_CU_PG_ENABLE_MASK;
	else
		data &= ~RLC_PG_CNTL__STATIC_PER_CU_PG_ENABLE_MASK;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

static void gfx_v7_0_enable_gfx_dynamic_mgpg(struct frb_device *adev,
					     int enable)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_PG_CNTL);
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_GFX_DMG))
		data |= RLC_PG_CNTL__DYN_PER_CU_PG_ENABLE_MASK;
	else
		data &= ~RLC_PG_CNTL__DYN_PER_CU_PG_ENABLE_MASK;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);
}

#define RLC_SAVE_AND_RESTORE_STARTING_OFFSET 0x90
#define RLC_CLEAR_STATE_DESCRIPTOR_OFFSET    0x3D

static void gfx_v7_0_init_gfx_cgpg(struct frb_device *adev)
{
	uint32_t data, orig;
	uint32_t i;

	if (adev->gfx7->rlc.cs_data) {
		WREG32(mmRLC_GPM_SCRATCH_ADDR, RLC_CLEAR_STATE_DESCRIPTOR_OFFSET);
		WREG32(mmRLC_GPM_SCRATCH_DATA, upper_32_bits(adev->gfx7->rlc.clear_state_gpu_addr));
		WREG32(mmRLC_GPM_SCRATCH_DATA, lower_32_bits(adev->gfx7->rlc.clear_state_gpu_addr));
		WREG32(mmRLC_GPM_SCRATCH_DATA, adev->gfx7->rlc.clear_state_size);
	} else {
		WREG32(mmRLC_GPM_SCRATCH_ADDR, RLC_CLEAR_STATE_DESCRIPTOR_OFFSET);
		for (i = 0; i < 3; i++)
			WREG32(mmRLC_GPM_SCRATCH_DATA, 0);
	}
	if (adev->gfx7->rlc.reg_list) {
		WREG32(mmRLC_GPM_SCRATCH_ADDR, RLC_SAVE_AND_RESTORE_STARTING_OFFSET);
		for (i = 0; i < adev->gfx7->rlc.reg_list_size; i++)
			WREG32(mmRLC_GPM_SCRATCH_DATA, adev->gfx7->rlc.reg_list[i]);
	}

	orig = data = RREG32(mmRLC_PG_CNTL);
	data |= RLC_PG_CNTL__GFX_POWER_GATING_SRC_MASK;
	if (orig != data)
		WREG32(mmRLC_PG_CNTL, data);

	WREG32(mmRLC_SAVE_AND_RESTORE_BASE, adev->gfx7->rlc.save_restore_gpu_addr >> 8);
	WREG32(mmRLC_JUMP_TABLE_RESTORE, adev->gfx7->rlc.cp_table_gpu_addr >> 8);

	data = RREG32(mmCP_RB_WPTR_POLL_CNTL);
	data &= ~CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT_MASK;
	data |= (0x60 << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
	WREG32(mmCP_RB_WPTR_POLL_CNTL, data);

	data = 0x10101010;
	WREG32(mmRLC_PG_DELAY, data);

	data = RREG32(mmRLC_PG_DELAY_2);
	data &= ~0xff;
	data |= 0x3;
	WREG32(mmRLC_PG_DELAY_2, data);

	data = RREG32(mmRLC_AUTO_PG_CTRL);
	data &= ~RLC_AUTO_PG_CTRL__GRBM_REG_SAVE_GFX_IDLE_THRESHOLD_MASK;
	data |= (0x700 << RLC_AUTO_PG_CTRL__GRBM_REG_SAVE_GFX_IDLE_THRESHOLD__SHIFT);
	WREG32(mmRLC_AUTO_PG_CTRL, data);

}

static void gfx_v7_0_update_gfx_pg(struct frb_device *adev, int enable)
{
	gfx_v7_0_enable_gfx_cgpg(adev, enable);
	gfx_v7_0_enable_gfx_static_mgpg(adev, enable);
	gfx_v7_0_enable_gfx_dynamic_mgpg(adev, enable);
}


static uint32_t gfx_v7_0_get_csb_size(struct frb_device *adev)
{
	uint32_t count = 0;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;

	if (adev->gfx7->rlc.cs_data == NULL)
		return 0;

	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = adev->gfx7->rlc.cs_data; sect->section != NULL; ++sect) {
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


static void gfx_v7_0_init_pg(struct frb_device *adev)
{
	if (adev->pg_flags & (AMD_PG_SUPPORT_GFX_PG |
			      AMD_PG_SUPPORT_GFX_SMG |
			      AMD_PG_SUPPORT_GFX_DMG |
			      AMD_PG_SUPPORT_CP |
			      AMD_PG_SUPPORT_GDS |
			      AMD_PG_SUPPORT_RLC_SMU_HS)) {
		gfx_v7_0_enable_sclk_slowdown_on_pu(adev, 1);
		gfx_v7_0_enable_sclk_slowdown_on_pd(adev, 1);
		if (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG) {
			gfx_v7_0_init_gfx_cgpg(adev);
			gfx_v7_0_enable_cp_pg(adev, 1);
			gfx_v7_0_enable_gds_pg(adev, 1);
		}
		gfx_v7_0_init_ao_cu_mask(adev);
		gfx_v7_0_update_gfx_pg(adev, 1);
	}
}

static void gfx_v7_0_fini_pg(struct frb_device *adev)
{
	if (adev->pg_flags & (AMD_PG_SUPPORT_GFX_PG |
			      AMD_PG_SUPPORT_GFX_SMG |
			      AMD_PG_SUPPORT_GFX_DMG |
			      AMD_PG_SUPPORT_CP |
			      AMD_PG_SUPPORT_GDS |
			      AMD_PG_SUPPORT_RLC_SMU_HS)) {
		gfx_v7_0_update_gfx_pg(adev, 0);
		if (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG) {
			gfx_v7_0_enable_cp_pg(adev, 0);
			gfx_v7_0_enable_gds_pg(adev, 0);
		}
	}
}

static void gfx_v7_0_gpu_early_init(struct frb_device *adev)
{
	uint32_t gb_addr_config;
	uint32_t mc_arb_ramcfg;
	uint32_t tmp;

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		adev->gfx7->config.max_shader_engines = 2;
		adev->gfx7->config.max_tile_pipes = 4;
		adev->gfx7->config.max_cu_per_sh = 7;
		adev->gfx7->config.max_sh_per_se = 1;
		adev->gfx7->config.max_backends_per_se = 2;
		adev->gfx7->config.max_texture_channel_caches = 4;
		adev->gfx7->config.max_gprs = 256;
		adev->gfx7->config.max_gs_threads = 32;
		adev->gfx7->config.max_hw_contexts = 8;

		adev->gfx7->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx7->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx7->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx7->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = BONAIRE_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_HAWAII:
		adev->gfx7->config.max_shader_engines = 4;
		adev->gfx7->config.max_tile_pipes = 16;
		adev->gfx7->config.max_cu_per_sh = 11;
		adev->gfx7->config.max_sh_per_se = 1;
		adev->gfx7->config.max_backends_per_se = 4;
		adev->gfx7->config.max_texture_channel_caches = 16;
		adev->gfx7->config.max_gprs = 256;
		adev->gfx7->config.max_gs_threads = 32;
		adev->gfx7->config.max_hw_contexts = 8;

		adev->gfx7->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx7->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx7->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx7->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = HAWAII_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_KAVERI:
		adev->gfx7->config.max_shader_engines = 1;
		adev->gfx7->config.max_tile_pipes = 4;
		adev->gfx7->config.max_cu_per_sh = 8;
		adev->gfx7->config.max_backends_per_se = 2;
		adev->gfx7->config.max_sh_per_se = 1;
		adev->gfx7->config.max_texture_channel_caches = 4;
		adev->gfx7->config.max_gprs = 256;
		adev->gfx7->config.max_gs_threads = 16;
		adev->gfx7->config.max_hw_contexts = 8;

		adev->gfx7->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx7->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx7->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx7->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = BONAIRE_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_KABINI:
	case CHIP_MULLINS:
	default:
		adev->gfx7->config.max_shader_engines = 1;
		adev->gfx7->config.max_tile_pipes = 2;
		adev->gfx7->config.max_cu_per_sh = 2;
		adev->gfx7->config.max_sh_per_se = 1;
		adev->gfx7->config.max_backends_per_se = 1;
		adev->gfx7->config.max_texture_channel_caches = 2;
		adev->gfx7->config.max_gprs = 256;
		adev->gfx7->config.max_gs_threads = 16;
		adev->gfx7->config.max_hw_contexts = 8;

		adev->gfx7->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx7->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx7->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx7->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = BONAIRE_GB_ADDR_CONFIG_GOLDEN;
		break;
	}

	adev->gfx7->config.mc_arb_ramcfg = RREG32(mmMC_ARB_RAMCFG);
	mc_arb_ramcfg = adev->gfx7->config.mc_arb_ramcfg;

	adev->gfx7->config.num_banks = REG_GET_FIELD(mc_arb_ramcfg,
				MC_ARB_RAMCFG, NOOFBANK);
	adev->gfx7->config.num_ranks = REG_GET_FIELD(mc_arb_ramcfg,
				MC_ARB_RAMCFG, NOOFRANKS);

	adev->gfx7->config.num_tile_pipes = adev->gfx7->config.max_tile_pipes;
	adev->gfx7->config.mem_max_burst_length_bytes = 256;
	/* the dGPU arm: no CIK APU (MC_FUS_DRAM*_BANK_ADDR_MAPPING) in a PCIe slot */
	tmp = (mc_arb_ramcfg & MC_ARB_RAMCFG__NOOFCOLS_MASK) >> MC_ARB_RAMCFG__NOOFCOLS__SHIFT;
	adev->gfx7->config.mem_row_size_in_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (adev->gfx7->config.mem_row_size_in_kb > 4)
		adev->gfx7->config.mem_row_size_in_kb = 4;
	/* XXX use MC settings? */
	adev->gfx7->config.shader_engine_tile_size = 32;
	adev->gfx7->config.num_gpus = 1;
	adev->gfx7->config.multi_gpu_tile_size = 64;

	/* fix up row size */
	gb_addr_config &= ~GB_ADDR_CONFIG__ROW_SIZE_MASK;
	switch (adev->gfx7->config.mem_row_size_in_kb) {
	case 1:
	default:
		gb_addr_config |= (0 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	case 2:
		gb_addr_config |= (1 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	case 4:
		gb_addr_config |= (2 << GB_ADDR_CONFIG__ROW_SIZE__SHIFT);
		break;
	}
	adev->gfx7->config.gb_addr_config = gb_addr_config;
}

static void gfx_v7_0_get_cu_info(struct frb_device *adev)
{
	int i, j, k, counter, active_cu_number = 0;
	uint32_t mask, bitmap, ao_bitmap, ao_cu_mask = 0;
	struct frb_gfx7_cu_info *cu_info = &adev->gfx7->cu_info;
	unsigned disable_masks[4 * 2];
	uint32_t ao_cu_num;

	if (0)	/* adev->flags & AMD_IS_APU: no CIK APU here */
		ao_cu_num = 2;
	else
		ao_cu_num = adev->gfx7->config.max_cu_per_sh;

	memset(cu_info, 0, sizeof(*cu_info));

	/* amdgpu_gfx_parse_disable_cu: the disable_cu parameter is unset */
	memset(disable_masks, 0, sizeof(disable_masks));

	for (i = 0; i < adev->gfx7->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx7->config.max_sh_per_se; j++) {
			mask = 1;
			ao_bitmap = 0;
			counter = 0;
			gfx_v7_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			if (i < 4 && j < 2)
				gfx_v7_0_set_user_cu_inactive_bitmap(
					adev, disable_masks[i * 2 + j]);
			bitmap = gfx_v7_0_get_cu_active_bitmap(adev);
			cu_info->bitmap[0][i][j] = bitmap;

			for (k = 0; k < adev->gfx7->config.max_cu_per_sh; k++) {
				if (bitmap & mask) {
					if (counter < ao_cu_num)
						ao_bitmap |= mask;
					counter++;
				}
				mask <<= 1;
			}
			active_cu_number += counter;
			if (i < 2 && j < 2)
				ao_cu_mask |= (ao_bitmap << (i * 16 + j * 8));
			cu_info->ao_cu_bitmap[i][j] = ao_bitmap;
		}
	}
	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cu_info->number = active_cu_number;
	cu_info->ao_cu_mask = ao_cu_mask;
	cu_info->simd_per_cu = NUM_SIMD_PER_CU;
	cu_info->max_waves_per_simd = 10;
	cu_info->max_scratch_slots_per_cu = 32;
	cu_info->wave_front_size = 64;
	cu_info->lds_size = 64;
}

/* gfx_v7_0_update_spm_vmid (amdgpu_gfx_off_ctrl around it does nothing: no GFXOFF on CIK) */
static void gfx_v7_0_update_spm_vmid(struct frb_device *adev, unsigned vmid)
{
	uint32_t data;

	data = RREG32(mmRLC_SPM_VMID);

	data &= ~RLC_SPM_VMID__RLC_SPM_VMID_MASK;
	data |= (vmid & RLC_SPM_VMID__RLC_SPM_VMID_MASK) << RLC_SPM_VMID__RLC_SPM_VMID__SHIFT;

	WREG32(mmRLC_SPM_VMID, data);
}

/* --- microcode, RLC objects --------------------------------------------------- */

static int gfx7_request(struct frb_device *adev, const char *chip, const char *what, struct frb_gfx7_fw *f)
{
	struct frb_fw fw = { 0, 0 };
	char name[32], suffix[16];
	int r;

	/* "<chip>_<what>.bin" */
	frb_fw_name(suffix, sizeof(suffix), "_", what);
	frb_fw_name(name, sizeof(name), suffix, ".bin");
	frb_fw_name(suffix, sizeof(suffix), name, "");
	frb_fw_name(name, sizeof(name), chip, suffix);
	r = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (r) {
		frb_err(adev, "gfx7: Failed to load firmware \"%s\" (%d)", name, r);
		return r;
	}
	r = frb_ucode_parse(adev, name, &fw, &f->u);
	if (r)
		return r;
	/* gfx_firmware_header_v1_0 / rlc_firmware_header_v1_0: ucode_feature_version after the common header */
	if (f->u.header_size_bytes < FRB_FWH_GFX_UCODE_FEATURE_VERSION + 4) {
		frb_err(adev, "gfx7: %s: no v1.0 header", name);
		return -EINVAL;
	}
	f->feature_version = frb_ucode_le32(&f->u, FRB_FWH_GFX_UCODE_FEATURE_VERSION);
	return 0;
}

/*
 * gfx_v7_0_init_microcode: amdgpu's names (amdgpu/hawaii_*.bin), which
 * differ from radeon's hawaii_pfp/me/ce (RadeonNI's) in the CP images; the
 * RLC image is the same file in both. The MEC image is not asked for: the
 * compute rings are not ported (the MEC stays halted).
 */
static int gfx_v7_0_init_microcode(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	const char *chip_name;
	int err;

	switch (adev->asic_type) {
	case CHIP_BONAIRE:
		chip_name = "bonaire";
		break;
	case CHIP_HAWAII:
		chip_name = "hawaii";
		break;
	default:
		frb_err(adev, "gfx7: no microcode choice for asic 0x%x here", adev->asic_type);
		return -ENODEV;
	}
	err = gfx7_request(adev, chip_name, "pfp", &g->pfp_fw);
	if (!err)
		err = gfx7_request(adev, chip_name, "me", &g->me_fw);
	if (!err)
		err = gfx7_request(adev, chip_name, "ce", &g->ce_fw);
	if (!err)
		err = gfx7_request(adev, chip_name, "rlc", &g->rlc_fw);
	if (err)
		frb_err(adev, "gfx7: Failed to load firmware %s gfx firmware", chip_name);
	else
		frb_info(adev, "gfx: %s pfp %u (feature %u), me %u (%u), ce %u (%u), rlc %u (%u)", chip_name,
			 g->pfp_fw.u.ucode_version, g->pfp_fw.feature_version, g->me_fw.u.ucode_version,
			 g->me_fw.feature_version, g->ce_fw.u.ucode_version, g->ce_fw.feature_version,
			 g->rlc_fw.u.ucode_version, g->rlc_fw.feature_version);
	return err;
}

/*
 * gfx_v7_0_get_csb_buffer (amdgpu_gfx_csb_preamble_start, _data_parser,
 * the raster configuration pair, _preamble_end), written straight into the
 * block in VRAM as little-endian dwords (frb_vram_write32: the cpu_to_le32
 * of amdgpu's buffer)
 */
static void gfx_v7_0_get_csb_buffer(struct frb_device *adev, uint32_t at)
{
	struct frb_gfx7 *g = adev->gfx7;
	const struct cs_section_def *sect;
	const struct cs_extent_def *ext;
	uint32_t count = 0, i;

	if (g->rlc.cs_data == NULL)
		return;
#define CSB(v)	frb_vram_write32(adev, at + 4 * count++, (v))
	CSB(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	CSB(PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);
	CSB(PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	CSB(0x80000000);
	CSB(0x80000000);
	for (sect = g->rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				CSB(PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				CSB(ext->reg_index - PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					CSB(ext->extent[i]);
			}
		}
	}
	CSB(PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	CSB(mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	CSB(g->config.rb_config[0][0].raster_config);
	CSB(g->config.rb_config[0][0].raster_config_1);
	CSB(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	CSB(PACKET3_PREAMBLE_END_CLEAR_STATE);
	CSB(PACKET3(PACKET3_CLEAR_STATE, 0));
	CSB(0);
#undef CSB
}

/*
 * gfx_v7_0_rlc_init: a dGPU has no save/restore list (amdgpu sets one only
 * for the APUs), so only the clear-state block (amdgpu_gfx_rlc_init_csb),
 * in VRAM; it is filled in hw_init, after constants_init, as amdgpu fills
 * it. The CP jump table is not made (see the header). Then the SPM VMID.
 */
static int gfx_v7_0_rlc_init(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	int r;

	g->rlc.reg_list = NULL;
	g->rlc.reg_list_size = 0;
	g->rlc.cs_data = ci_cs_data;

	/* init clear state block */
	g->rlc.clear_state_size = gfx_v7_0_get_csb_size(adev);
	r = frb_vram_obj_alloc(adev, g->rlc.clear_state_size * 4, 4096, &g->rlc.clear_state_offset);
	if (r)
		return r;
	g->rlc.clear_state_gpu_addr = frb_vram_gpu_addr(adev, g->rlc.clear_state_offset);

	/* init spm vmid with 0xf */
	gfx_v7_0_update_spm_vmid(adev, 0xf);
	return 0;
}

/* gfx_v7_0_rlc_resume: the RLC image by MMIO */
static int gfx_v7_0_rlc_resume(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	const struct frb_ucode *u = &g->rlc_fw.u;
	unsigned i, fw_size;
	uint32_t tmp;

	if (!u->data)
		return -EINVAL;

	gfx_v7_0_rlc_stop(adev);

	/* disable CG */
	tmp = RREG32(mmRLC_CGCG_CGLS_CTRL) & 0xfffffffc;
	WREG32(mmRLC_CGCG_CGLS_CTRL, tmp);

	gfx_v7_0_rlc_reset(adev);

	gfx_v7_0_init_pg(adev);

	WREG32(mmRLC_LB_CNTR_INIT, 0);
	WREG32(mmRLC_LB_CNTR_MAX, 0x00008000);

	gfx_v7_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
	WREG32(mmRLC_LB_INIT_CU_MASK, 0xffffffff);
	WREG32(mmRLC_LB_PARAMS, 0x00600408);
	WREG32(mmRLC_LB_CNTL, 0x80000004);

	WREG32(mmRLC_MC_CNTL, 0);
	WREG32(mmRLC_UCODE_CNTL, 0);

	fw_size = u->ucode_size_bytes / 4;
	WREG32(mmRLC_GPM_UCODE_ADDR, 0);
	for (i = 0; i < fw_size; i++)
		WREG32(mmRLC_GPM_UCODE_DATA, frb_ucode_le32(u, u->ucode_array_offset + 4 * i));
	WREG32(mmRLC_GPM_UCODE_ADDR, u->ucode_version);

	/* XXX - find out what chips support lbpw */
	gfx_v7_0_enable_lbpw(adev, 0);

	if (adev->asic_type == CHIP_BONAIRE)
		WREG32(mmRLC_DRIVER_CPDMA_STATUS, 0);

	gfx_v7_0_rlc_start(adev);

	return 0;
}

/* --- the command processor ----------------------------------------------------- */

static void gfx_v7_0_cp_gfx_enable(struct frb_device *adev, int enable)
{
	if (enable)
		WREG32(mmCP_ME_CNTL, 0);
	else
		WREG32(mmCP_ME_CNTL, (CP_ME_CNTL__ME_HALT_MASK |
				      CP_ME_CNTL__PFP_HALT_MASK |
				      CP_ME_CNTL__CE_HALT_MASK));
	udelay(50);
}

/* gfx_v7_0_cp_compute_enable; only its halt is used (the MEC is not loaded) */
static void gfx_v7_0_cp_compute_enable(struct frb_device *adev, int enable)
{
	if (enable)
		WREG32(mmCP_MEC_CNTL, 0);
	else
		WREG32(mmCP_MEC_CNTL, (CP_MEC_CNTL__MEC_ME1_HALT_MASK |
				       CP_MEC_CNTL__MEC_ME2_HALT_MASK));
	udelay(50);
}

/* one image through its ucode port: address 0, the words, then the version in the address register (amdgpu) */
static void load_ucode(struct frb_device *adev, const struct frb_ucode *u, uint32_t addr_reg, uint32_t data_reg)
{
	uint32_t i, fw_size = u->ucode_size_bytes / 4;

	WREG32(addr_reg, 0);
	for (i = 0; i < fw_size; i++)
		WREG32(data_reg, frb_ucode_le32(u, u->ucode_array_offset + 4 * i));
	WREG32(addr_reg, u->ucode_version);
}

static int gfx_v7_0_cp_gfx_load_microcode(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;

	if (!g->me_fw.u.data || !g->pfp_fw.u.data || !g->ce_fw.u.data)
		return -EINVAL;

	gfx_v7_0_cp_gfx_enable(adev, 0);

	/* PFP */
	load_ucode(adev, &g->pfp_fw.u, mmCP_PFP_UCODE_ADDR, mmCP_PFP_UCODE_DATA);
	/* CE */
	load_ucode(adev, &g->ce_fw.u, mmCP_CE_UCODE_ADDR, mmCP_CE_UCODE_DATA);
	/* ME */
	load_ucode(adev, &g->me_fw.u, mmCP_ME_RAM_WADDR, mmCP_ME_RAM_DATA);

	return 0;
}

static int gfx_v7_0_cp_gfx_start(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	struct frb_ring *ring = &g->gfx_ring;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	int r, i;

	/* init the CP */
	WREG32(mmCP_MAX_CONTEXT, g->config.max_hw_contexts - 1);
	WREG32(mmCP_ENDIAN_SWAP, 0);
	WREG32(mmCP_DEVICE_ID, 1);

	gfx_v7_0_cp_gfx_enable(adev, 1);

	r = frb_ring_alloc(ring, gfx_v7_0_get_csb_size(adev) + 8);
	if (r) {
		frb_err(adev, "gfx: cp failed to lock ring (%d).", r);
		return r;
	}

	/* init the CE partitions.  CE only used for gfx on CIK */
	frb_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	frb_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	frb_ring_write(ring, 0x8000);
	frb_ring_write(ring, 0x8000);

	/* clear state buffer */
	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	frb_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	frb_ring_write(ring, 0x80000000);
	frb_ring_write(ring, 0x80000000);

	for (sect = g->rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				frb_ring_write(ring,
					       PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				frb_ring_write(ring, ext->reg_index - PACKET3_SET_CONTEXT_REG_START);
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

	frb_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	frb_ring_write(ring, 0x00000316);
	frb_ring_write(ring, 0x0000000e); /* VGT_VERTEX_REUSE_BLOCK_CNTL */
	frb_ring_write(ring, 0x00000010); /* VGT_OUT_DEALLOC_CNTL */

	frb_ring_commit(ring);

	return 0;
}

static int gfx_v7_0_ring_test_ring(struct frb_ring *ring);

static int gfx_v7_0_cp_gfx_resume(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	struct frb_ring *ring;
	uint32_t tmp;
	uint32_t rb_bufsz;
	uint64_t rb_addr, rptr_addr;
	int r;

	WREG32(mmCP_SEM_WAIT_TIMER, 0x0);
	if (adev->asic_type != CHIP_HAWAII)
		WREG32(mmCP_SEM_INCOMPLETE_TIMER_CNTL, 0x0);

	/* Set the write pointer delay */
	WREG32(mmCP_RB_WPTR_DELAY, 0);

	/* set the RB to use vmid 0 */
	WREG32(mmCP_RB_VMID, 0);

	WREG32(mmSCRATCH_ADDR, 0);

	/* ring 0 - compute and gfx */
	/* Set ring buffer size */
	ring = &g->gfx_ring;
	rb_bufsz = order_base_2(ring->ring_size / 8);
	tmp = (order_base_2(AMDGPU_GPU_PAGE_SIZE/8) << 8) | rb_bufsz;
	/*
	 * No BUF_SWAP (amdgpu: 2 << CP_RB0_CNTL__BUF_SWAP__SHIFT under
	 * __BIG_ENDIAN; RadeonNI: BUF_SWAP_32BIT): the ring is little-endian.
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

	/* scratch register shadowing is no longer supported */
	WREG32(mmSCRATCH_UMSK, 0);

	mdelay(1);
	WREG32(mmCP_RB0_CNTL, tmp);

	rb_addr = ring->gpu_addr >> 8;
	WREG32(mmCP_RB0_BASE, (uint32_t)rb_addr);
	WREG32(mmCP_RB0_BASE_HI, upper_32_bits(rb_addr));

	/* start the ring */
	r = gfx_v7_0_cp_gfx_start(adev);
	if (r)
		return r;
	/* amdgpu_ring_test_helper */
	r = gfx_v7_0_ring_test_ring(ring);
	ring->ready = !r;
	return r;
}

/*
 * gfx_v7_0_cp_resume, the gfx ring: the PFP/CE/ME images, then the MEC
 * halted as gfx_v7_0_cp_compute_load_microcode halts it first (its image is
 * not loaded and cp_compute_resume does not run: no compute rings).
 */
static int gfx_v7_0_cp_resume(struct frb_device *adev)
{
	int r;

	gfx_v7_0_enable_gui_idle_interrupt(adev, 0);

	r = gfx_v7_0_cp_gfx_load_microcode(adev);
	if (r)
		return r;
	gfx_v7_0_cp_compute_enable(adev, 0);

	r = gfx_v7_0_cp_gfx_resume(adev);
	if (r)
		return r;

	gfx_v7_0_enable_gui_idle_interrupt(adev, 1);

	return 0;
}

/* --- the ring ------------------------------------------------------------------ */

static uint64_t gfx_v7_0_ring_get_rptr(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->rptr_offs);	/* *ring->rptr_cpu_addr */
}

static uint64_t gfx_v7_0_ring_get_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmCP_RB0_WPTR);
}

static void gfx_v7_0_ring_set_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
	(void)RREG32(mmCP_RB0_WPTR);
}

/*
 * The ring's filler (see the header, PADDING): one counted NOP, the type-2
 * NOP for a single dword. amdgpu: amdgpu_ring_insert_nop, count times
 * PACKET3(NOP, 0x3FFF).
 */
static void gfx_v7_0_ring_insert_nop(struct frb_ring *ring, uint32_t count)
{
	if (!count)
		return;
	if (count == 1) {
		frb_ring_write(ring, GFX7_TYPE2_NOP);
		return;
	}
	frb_ring_write(ring, PACKET3(PACKET3_NOP, count - 2));
	while (--count)
		frb_ring_write(ring, 0);
}

/* gfx_v7_0_ring_emit_hdp_flush, gfx_v7_0_get_hdp_flush_mask's gfx arm */
static void gfx_v7_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	uint32_t ref_and_mask = GPU_HDP_FLUSH_DONE__CP0_MASK;
	int usepfp = 1;

	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_OPERATION(1) | /* write, wait, write */
				 WAIT_REG_MEM_FUNCTION(3) |  /* == */
				 WAIT_REG_MEM_ENGINE(usepfp)));   /* pfp or me */
	frb_ring_write(ring, mmGPU_HDP_FLUSH_REQ);
	frb_ring_write(ring, mmGPU_HDP_FLUSH_DONE);
	frb_ring_write(ring, ref_and_mask);
	frb_ring_write(ring, ref_and_mask);
	frb_ring_write(ring, 0x20); /* poll interval */
}

static void gfx_v7_0_ring_emit_fence_gfx(struct frb_ring *ring, uint64_t addr,
					 uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;
	int int_sel = flags & FRB_FENCE_FLAG_INT;

	/* Workaround for cache flush problems. First send a dummy EOP
	 * event down the pipe with seq one below.
	 */
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	frb_ring_write(ring, (EOP_TCL1_ACTION_EN |
				 EOP_TC_ACTION_EN |
				 EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 EVENT_INDEX(5)));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xffff) |
				DATA_SEL(1) | INT_SEL(0));
	frb_ring_write(ring, lower_32_bits(seq - 1));
	frb_ring_write(ring, upper_32_bits(seq - 1));

	/* Then send the real EOP event down the pipe. */
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	frb_ring_write(ring, (EOP_TCL1_ACTION_EN |
				 EOP_TC_ACTION_EN |
				 EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 EVENT_INDEX(5)));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xffff) |
				DATA_SEL(write64bit ? 2 : 1) | INT_SEL(int_sel ? 2 : 0));
	frb_ring_write(ring, lower_32_bits(seq));
	frb_ring_write(ring, upper_32_bits(seq));
}

static void gfx_v7_0_ring_emit_ib_gfx(struct frb_ring *ring, const struct frb_ib *ib,
				      uint32_t vmid, uint32_t flags)
{
	uint32_t header, control = 0;

	(void)flags;	/* AMDGPU_HAVE_CTX_SWITCH: no SWITCH_BUFFER here (one context) */
	if (ib->flags & AMDGPU_IB_FLAG_CE)
		header = PACKET3(PACKET3_INDIRECT_BUFFER_CONST, 2);
	else
		header = PACKET3(PACKET3_INDIRECT_BUFFER, 2);

	control |= ib->length_dw | (vmid << 24);

	frb_ring_write(ring, header);
	/*
	 * No swap field (amdgpu: (2 << 0) under __BIG_ENDIAN): every IB is
	 * little-endian (frbctl test gfx ibprobe measures the field).
	 */
	frb_ring_write(ring, (uint32_t)(ib->gpu_addr & 0xFFFFFFFC));
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr) & 0xFFFF);
	frb_ring_write(ring, control);
}

static void gfx_v7_0_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	int usepfp = 1;	/* AMDGPU_RING_TYPE_GFX */
	uint32_t seq = ring->sync_seq;
	uint64_t addr = ring->fence_gpu_addr;

	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
				 WAIT_REG_MEM_FUNCTION(3) | /* equal */
				 WAIT_REG_MEM_ENGINE(usepfp)));   /* pfp or me */
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, seq);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, 4); /* poll interval */

	if (usepfp) {
		/* sync CE with ME to prevent CE fetch CEIB before context switch done */
		frb_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
		frb_ring_write(ring, 0);
		frb_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
		frb_ring_write(ring, 0);
	}
}

/* emit_pipeline_sync's wait with another address and compare: another engine's fence */
static void gfx_v7_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
				 WAIT_REG_MEM_FUNCTION(func) |
				 WAIT_REG_MEM_ENGINE(1)));   /* pfp */
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, upper_32_bits(addr) & 0xffffffff);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, 4); /* poll interval */
}

/* cik_invalidate_hdp's ring arm (frb_ib_schedule) */
static void gfx_v7_0_ring_emit_wreg(struct frb_ring *ring,
				    uint32_t reg, uint32_t val)
{
	int usepfp = 1;	/* AMDGPU_RING_TYPE_GFX */

	frb_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	frb_ring_write(ring, (WRITE_DATA_ENGINE_SEL(usepfp) |
				 WRITE_DATA_DST_SEL(0)));
	frb_ring_write(ring, reg);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, val);
}

static const struct frb_ring_funcs gfx_v7_0_ring_funcs_gfx = {
	.align_mask = 0xff,
	.nop = GFX7_TYPE2_NOP,			/* amdgpu: PACKET3(PACKET3_NOP, 0x3FFF); see the header */
	.support_64bit_ptrs = 0,
	.get_rptr = gfx_v7_0_ring_get_rptr,
	.get_wptr = gfx_v7_0_ring_get_wptr_gfx,
	.set_wptr = gfx_v7_0_ring_set_wptr_gfx,
	.emit_ib = gfx_v7_0_ring_emit_ib_gfx,
	.emit_fence = gfx_v7_0_ring_emit_fence_gfx,
	.emit_hdp_flush = gfx_v7_0_ring_emit_hdp_flush,
	.insert_nop = gfx_v7_0_ring_insert_nop,	/* amdgpu: amdgpu_ring_insert_nop */
	.pad_ib = frb_ring_generic_pad_ib,	/* amdgpu_ring_generic_pad_ib, with the type-2 nop */
	.emit_wreg = gfx_v7_0_ring_emit_wreg,
	.emit_pipeline_sync = gfx_v7_0_ring_emit_pipeline_sync,
	.emit_wait_mem = gfx_v7_0_ring_emit_wait_mem,
};

/* --- the tests ---------------------------------------------------------------- */

static void gfx_v7_0_cp_status(struct frb_device *adev, uint32_t *s)
{
	s[0] = RREG32(mmGRBM_STATUS);
	s[1] = RREG32(mmCP_STAT);
	s[2] = RREG32(mmCP_STALLED_STAT1);
	s[3] = RREG32(mmCP_STALLED_STAT2);
	s[4] = RREG32(mmCP_ME_CNTL);
}

static int gfx_v7_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t tmp = 0, st[5];
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
	if (r) {
		gfx_v7_0_cp_status(adev, st);
		frb_err(adev, "gfx: ring test FAILED: SCRATCH_REG0 %08x after %u polls (wptr %u, rptr %u, "
			"CP_RB0_RPTR %08x, GRBM_STATUS %08x, CP_STAT %08x, CP_STALLED_STAT1 %08x, "
			"CP_STALLED_STAT2 %08x, CP_ME_CNTL %08x)", tmp, i, (uint32_t)ring->wptr,
			(uint32_t)gfx_v7_0_ring_get_rptr(ring), RREG32(mmCP_RB0_RPTR), st[0], st[1], st[2], st[3],
			st[4]);
	} else {
		frb_info(adev, "gfx: ring test passed: SCRATCH_REG0 deadbeef after %u polls (wptr %u)",
			 i, (uint32_t)ring->wptr);
	}
	return r;
}

/* gfx_v7_0_ring_test_ib: a SET_UCONFIG_REG of SCRATCH_REG0 from an IB, then the fence */
static int gfx_v7_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t tmp = 0, seq = 0, st[5];
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);
	r = frb_ib_get(adev, 256, &ib);
	if (r)
		return r;

	frb_ib_write(adev, &ib, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	frb_ib_write(adev, &ib, mmSCRATCH_REG0 - PACKET3_SET_UCONFIG_REG_START);
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	ib.length_dw = 3;

	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto error;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		gfx_v7_0_cp_status(adev, st);
		frb_err(adev, "gfx: IB test FAILED: fence %u not signalled (reads %u) in %u us; rptr %u wptr %u, "
			"SCRATCH_REG0 %08x, GRBM_STATUS %08x, CP_STAT %08x, CP_STALLED_STAT1 %08x, "
			"CP_STALLED_STAT2 %08x", seq, frb_fence_read(ring), timeout_us,
			(uint32_t)ring->funcs->get_rptr(ring), (uint32_t)ring->funcs->get_wptr(ring),
			RREG32(mmSCRATCH_REG0), st[0], st[1], st[2], st[3]);
		return r;	/* the CP may still fetch it: no frb_ib_free after a timeout */
	}
	tmp = RREG32(mmSCRATCH_REG0);
	if (tmp == 0xDEADBEEF)
		r = 0;
	else
		r = -EINVAL;
	if (r)
		frb_err(adev, "gfx: IB test FAILED: fence %u signalled, SCRATCH_REG0 %08x", seq, tmp);
	else
		frb_info(adev, "gfx: IB test passed (fence %u)", seq);
error:
	frb_ib_free(adev, &ib);
	return r;
}

/*
 * frbctl test gfx ibprobe: what this CP does with the INDIRECT_BUFFER swap
 * field (address bits 1:0; amdgpu's __BIG_ENDIAN arm puts 2 there) under
 * the little-endian ring, the IB in either order (host k > 1: only the
 * first k - 1 dwords in the CPU's order). The IB sets SCRATCH_REG0 through
 * SET_UCONFIG_REG, so no memory write is involved: 0xDEADBEEF ran as
 * written, 0xEFBEADDE ran with the payload's bytes as they stood,
 * 0xCAFEDEAD did not run. frb_gfx_v6_0.c's probe with CIK's scratch
 * register. RadeonNI ran host-order IBs with no swap field under
 * CP_ENDIAN_SWAP 2 (and a host-order ring with BUF_SWAP_32BIT) on this card.
 */
static int gfx_v7_0_ib_probe_one(struct frb_ring *ring, uint32_t bits, int host, uint32_t *got)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t seq, i, w[3];
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);
	r = frb_ib_get(adev, 64, &ib);
	if (r)
		return r;
	w[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
	w[1] = mmSCRATCH_REG0 - PACKET3_SET_UCONFIG_REG_START;
	w[2] = 0xDEADBEEF;
	for (i = 0; i < 3; i++)
		frb_vram_write32(adev, ib.bar_offset + 4 * i,
				 (host == 1 || (host > 1 && i < (uint32_t)host - 1)) ? frb_swap_le32(w[i]) : w[i]);
	r = frb_ring_alloc(ring, 64);
	if (r)
		goto out;
	gfx_v7_0_ring_emit_hdp_flush(ring);
	frb_ring_write(ring, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	frb_ring_write(ring, (bits & 3) | (uint32_t)(ib.gpu_addr & 0xFFFFFFFC));
	frb_ring_write(ring, upper_32_bits(ib.gpu_addr) & 0xFFFF);
	frb_ring_write(ring, 3);
	seq = ++ring->sync_seq;
	ring->funcs->emit_fence(ring, ring->fence_gpu_addr, seq, FRB_FENCE_FLAG_INT);
	frb_ring_commit(ring);
	r = frb_fence_wait(ring, seq, 200 * 1000);
	*got = RREG32(mmSCRATCH_REG0);
	if (r)
		return r;	/* no frb_ib_free after a timeout */
out:
	frb_ib_free(adev, &ib);
	return r;
}

static int gfx_v7_0_ib_probe(struct frb_ring *ring)
{
	/*
	 * Safest first; the probe stops at the first variant whose fence does
	 * not come (the CP hung: reboot). On SI (the R7 450) only the first
	 * ran, every other one left SCRATCH_REG0 unwritten with the fence
	 * still coming; on VI (the E9173) the same, with WRITE_DATA.
	 */
	static const struct { uint32_t bits; int host; } v[] = {
		{ 0, 0 },	/* the kernel's IBs */
		{ 2, 1 },	/* amdgpu's big-endian IB arm */
		{ 1, 1 },	/* the other encodings of the field */
		{ 3, 1 },
		{ 2, 0 },	/* the field on a little-endian IB */
		{ 1, 0 },
		{ 3, 0 },
		{ 2, 2 },	/* the header swapped, the rest raw? */
	};
	struct frb_device *adev = ring->adev;
	uint32_t got = 0, i, ran = 0, st[5];
	int r = 0;

	for (i = 0; i < ARRAY_SIZE(v) && !r; i++) {
		r = gfx_v7_0_ib_probe_one(ring, v[i].bits, v[i].host, &got);
		frb_info(adev, "gfx: ib probe: IB swap bits %u, %s IB: %s, SCRATCH_REG0 %08x (%s)",
			 v[i].bits, v[i].host == 1 ? "host-order" : v[i].host == 2 ? "header-host" : "little-endian",
			 r ? "fence NOT signalled" : "fence signalled", got,
			 got == 0xDEADBEEF ? "the payload as its value" :
			 got == 0xEFBEADDE ? "the payload's bytes as they stood" :
			 got == 0xCAFEDEAD ? "not written" : "something else");
		if (!r && (got == 0xDEADBEEF || got == 0xEFBEADDE))
			ran |= 1u << i;
	}
	if (r) {
		gfx_v7_0_cp_status(adev, st);
		frb_err(adev, "gfx: ib probe: the CP stopped at variant %u (GRBM_STATUS %08x CP_STAT %08x "
			"CP_STALLED_STAT1 %08x CP_STALLED_STAT2 %08x): reboot", i - 1, st[0], st[1], st[2], st[3]);
		return r;
	}
	frb_info(adev, "gfx: ib probe: variants that ran: mask %03x", ran);
	return 0;
}

/* --- the stage, the tests, the teardown ------------------------------------------ */

/*
 * gfx_v7_0_sw_init: the microcode, the RLC objects, the gfx ring,
 * ce_ram_size and gpu_early_init; no IRQ sources, no MEC buffers, no
 * compute rings
 */
static int gfx_v7_0_sw_init(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	int r;

	r = gfx_v7_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to load gfx firmware!");
		return r;
	}
	r = gfx_v7_0_rlc_init(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to init rlc BOs!");
		return r;
	}
	r = frb_ring_init(adev, &g->gfx_ring, "gfx", &gfx_v7_0_ring_funcs_gfx, 1024);
	if (r)
		return r;
	frb_ring_clear(&g->gfx_ring);	/* amdgpu_ring_init clears a new ring */

	g->ce_ram_size = 0x8000;

	gfx_v7_0_gpu_early_init(adev);
	adev->cik_gb_addr_config = g->config.gb_addr_config;
	g->sw_done = 1;
	return 0;
}

static int gfx_v7_0_hw_init(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	int r;

	g->hw_started = 1;
	gfx_v7_0_constants_init(adev);
	g->constants_done = 1;

	/* init CSB */
	gfx_v7_0_get_csb_buffer(adev, g->rlc.clear_state_offset);
	frb_flush_hdp(adev);
	/* init rlc */
	r = gfx_v7_0_rlc_resume(adev);
	if (r)
		return r;

	r = gfx_v7_0_cp_resume(adev);
	if (r)
		return r;

	return r;
}

static void gfx_v7_0_hw_fini(struct frb_device *adev)
{
	/* gfx_v7_0_cp_enable(adev, false) */
	gfx_v7_0_cp_gfx_enable(adev, 0);
	gfx_v7_0_cp_compute_enable(adev, 0);
	gfx_v7_0_rlc_stop(adev);
	gfx_v7_0_fini_pg(adev);
}

/* Stage gfx on CIK: gfx_v7_0_sw_init, then gfx_v7_0_hw_init. */
int frb_gfx_v7_0_init(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;
	int r;

	if (!adev->aper || !adev->layout.objects_size) {
		frb_err(adev, "gfx: no VRAM window or objects region (stage gmc)");
		return -ENODEV;
	}
	if (!g) {
		g = (struct frb_gfx7 *)adev->os->alloc(adev->os->cookie, sizeof(*g));
		if (!g)
			return -ENOMEM;
		memset(g, 0, sizeof(*g));
		adev->gfx7 = g;
	}
	if (!g->sw_done) {
		r = gfx_v7_0_sw_init(adev);
		if (r)
			return r;
	}
	r = gfx_v7_0_hw_init(adev);
	frb_info(adev, "gfx: %u SE x %u SH, %u CUs (bitmaps %x %x %x %x), %u RBs (mask %x), "
		 "raster config %08x / %08x, gb_addr_config %08x; ring %u KB at %llx, CSB %u dwords at %llx",
		 g->config.max_shader_engines, g->config.max_sh_per_se, g->cu_info.number,
		 g->cu_info.bitmap[0][0][0], g->cu_info.bitmap[0][1][0], g->cu_info.bitmap[0][2][0],
		 g->cu_info.bitmap[0][3][0], g->config.num_rbs, g->config.backend_enable_mask,
		 g->config.rb_config[0][0].raster_config, g->config.rb_config[0][0].raster_config_1,
		 g->config.gb_addr_config, g->gfx_ring.ring_size >> 10,
		 (unsigned long long)g->gfx_ring.gpu_addr, g->rlc.clear_state_size,
		 (unsigned long long)g->rlc.clear_state_gpu_addr);
	if (r)
		frb_err(adev, "gfx: hw_init failed (%d)", r);
	return r;
}

void frb_gfx_v7_0_fini(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;

	if (!g)
		return;
	if (g->hw_started) {
		g->gfx_ring.ready = 0;
		gfx_v7_0_hw_fini(adev);
		frb_info(adev, "gfx: hw_fini: CP_ME_CNTL %08x, CP_MEC_CNTL %08x, RLC_CNTL %08x", RREG32(mmCP_ME_CNTL),
			 RREG32(mmCP_MEC_CNTL), RREG32(mmRLC_CNTL));
	}
	adev->os->free(adev->os->cookie, g);
	adev->gfx7 = NULL;
}

int frb_gfx_v7_0_test(struct frb_device *adev, int test)
{
	struct frb_gfx7 *g = adev->gfx7;
	int r;

	if (!g || !g->hw_started)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		r = gfx_v7_0_ring_test_ring(&g->gfx_ring);
		g->gfx_ring.ready = !r;	/* amdgpu_ring_test_helper */
		return r;
	case FRB_TEST_IB:
		if (!g->gfx_ring.ready)
			return -ENODEV;
		return gfx_v7_0_ring_test_ib(&g->gfx_ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_IB_PROBE:
		if (!g->gfx_ring.ready)
			return -ENODEV;
		return gfx_v7_0_ib_probe(&g->gfx_ring);
	default:
		return -ENOSYS;
	}
}

/* --- for the accelerator ---------------------------------------------------------- */

struct frb_ring *frb_gfx_v7_0_ring(struct frb_device *adev)
{
	struct frb_gfx7 *g = adev->gfx7;

	return g && g->hw_started && g->gfx_ring.ready ? &g->gfx_ring : NULL;
}

int frb_gfx_v7_0_get_info(struct frb_device *adev, struct frb_gfx_info *info)
{
	struct frb_gfx7 *g = adev->gfx7;
	uint32_t se, sh;

	memset(info, 0, sizeof(*info));
	if (!g || !g->sw_done)
		return -ENODEV;
	info->gfx_level = 7;
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
	for (se = 0; se < 4; se++)
		for (sh = 0; sh < 4; sh++)
			info->cu_bitmap[se][sh] = g->cu_info.bitmap[0][se][sh];
	info->cu_ao_mask = g->cu_info.ao_cu_mask;
	info->simd_per_cu = g->cu_info.simd_per_cu;
	info->max_waves_per_simd = g->cu_info.max_waves_per_simd;
	info->wave_front_size = g->cu_info.wave_front_size;
	info->max_scratch_slots_per_cu = g->cu_info.max_scratch_slots_per_cu;
	info->lds_size = g->cu_info.lds_size;
	info->num_tcc_blocks = g->config.max_texture_channel_caches;
	info->max_gprs = g->config.max_gprs;
	info->max_gs_threads = g->config.max_gs_threads;
	info->sc_prim_fifo_size_frontend = g->config.sc_prim_fifo_size_frontend;
	info->sc_prim_fifo_size_backend = g->config.sc_prim_fifo_size_backend;
	info->sc_hiz_tile_fifo_size = g->config.sc_hiz_tile_fifo_size;
	info->sc_earlyz_tile_fifo_size = g->config.sc_earlyz_tile_fifo_size;
	info->double_offchip_lds_buf = g->config.double_offchip_lds_buf;
	info->me_fw_version = g->me_fw.u.ucode_version;
	info->me_fw_feature = g->me_fw.feature_version;
	info->pfp_fw_version = g->pfp_fw.u.ucode_version;
	info->pfp_fw_feature = g->pfp_fw.feature_version;
	/*
	 * csb_loaded stays 0: the RLC is never given the clear-state block on
	 * a CIK dGPU (gfx_v7_0_init_gfx_cgpg, power gating only); the CP's
	 * clear state comes from cp_gfx_start's preamble in the ring.
	 */
	return 0;
}

/*
 * cik_get_register_value's cached arms: what gfx_v7_0 keeps (the SE/SH
 * indexed RB registers, the tiling and macrotiling tables, the address
 * configuration: GB_ADDR_CONFIG itself reads back 0x20000000 on GCN,
 * RadeonNI's accel-state notes). 0 and the value, or -ENOENT when this
 * block keeps no copy.
 */
int frb_gfx_v7_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value)
{
	struct frb_gfx7 *g = adev->gfx7;
	unsigned se_idx = (se_num == 0xffffffff) ? 0 : se_num;
	unsigned sh_idx = (sh_num == 0xffffffff) ? 0 : sh_num;

	if (!g || !g->sw_done || !g->constants_done)
		return -ENOENT;
	if (se_idx >= GFX7_MAX_SE || sh_idx >= GFX7_MAX_SH_PER_SE)
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

/*
 * gfx_v7_0_set_clockgating_state (amdgpu's late init: GATE; hw_fini's
 * ungate): gfx_v7_0_update_cg. Only once hw_init has run (the RLC is up).
 */
void frb_gfx_v7_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	struct frb_gfx7 *g = adev->gfx7;

	if (!g || !g->hw_started)
		return;
	gfx_v7_0_update_cg(adev, gate);
	g->cg_gated = gate;
}
