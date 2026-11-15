/*
 * gfx_v6_0.c (Linux 7.0.2), for Southern Islands (the R7 450, Cape Verde):
 * the GFX 6 block's sw_init and hw_init up to the gfx ring test, the IB test,
 * and hw_fini's halt.
 *
 * What is ported, in amdgpu's order:
 *   sw_init:  gfx_v6_0_init_microcode (pfp, me, ce, rlc: the amdgpu-headered
 *             verde_*.bin), gfx_v6_0_rlc_init (the save/restore list, the
 *             clear-state block), the gfx ring (2048 dwords)
 *   hw_init:  gfx_v6_0_constants_init (the chip's limits, the address
 *             configuration and its copies, the tiling table, setup_rb,
 *             setup_spi, cu info, the 3D engine's defaults),
 *             gfx_v6_0_rlc_resume (stop, reset, init_pg, the RLC microcode
 *             through RLC_UCODE_ADDR/DATA, LBPW for DDR3 only, start),
 *             gfx_v6_0_cp_resume's gfx part (CP microcode through the
 *             PFP/CE/ME ucode ports, cp_gfx_resume, cp_gfx_start with the
 *             clear state, the gfx ring test, the GUI idle interrupt)
 *   late:     gfx_v6_0_ring_test_ib (frbctl test gfx ib)
 *   hw_fini:  CP halt, RLC stop, fini_pg
 * Unlike GFX 8, the CP and RLC images go in by MMIO: SI's SMC loads nothing.
 *
 * NOT PORTED (yet): the two compute rings (cp_compute_resume: CP_RB1/RB2),
 * interrupts (IH, the EOP/privileged-fault sources), clock gating
 * (set_clockgating_state; RadeonNI's si_init_cg has it measured on this
 * card), the soft reset, the other SI chips' rows (the code carries their
 * arms, untested).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - PLACEMENT. Ring, writeback, IB pool, CSB and the save/restore list in
 *   VRAM (frb_ring.c's objects region), as on GFX 8.
 * - BYTE ORDER. Ring, IB, CSB and writeback are little-endian dwords
 *   (frb_ring_write, frb_ib_write, frb_vram_write32): CP_RB0_CNTL.BUF_SWAP and
 *   the IB packet's swap field stay 0, where amdgpu (and RadeonNI, which ran
 *   host-order rings and IBs on this card) set BUF_SWAP_32BIT and swap field 2
 *   under __BIG_ENDIAN. One order for every family (docs/byte-order.md,
 *   card-support-fold-scope.md section 6.3); frbctl test gfx ibprobe measures
 *   what this CP does with the swap field.
 * - The CSB's first dwords (the save/restore list address and size) are
 *   written as amdgpu writes them, little-endian; the RLC never reads them
 *   on SI (RLC_CLEAR_STATE_RESTORE_BASE points at the block).
 * - HDP. SI has no GPU_HDP_FLUSH_REQ: the ring flushes and invalidates the
 *   HDP with register writes (si_flush_hdp / si_invalidate_hdp's ring arms,
 *   emit_wreg), which frb_ib_schedule takes when a ring has no
 *   emit_hdp_flush.
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
#include <stddef.h>
#include "frb_ring.h"
#include "frb_ucode.h"
#include "include/clearstate_defs.h"
#include "include/clearstate_si.h"
#include "include/sid.h"
#include "include/asic_reg/bif/bif_3_0_d.h"
#include "include/asic_reg/bif/bif_3_0_sh_mask.h"
#include "include/asic_reg/oss/oss_1_0_d.h"
#include "include/asic_reg/oss/oss_1_0_sh_mask.h"
#include "include/asic_reg/gca/gfx_6_0_d.h"
#include "include/asic_reg/gca/gfx_6_0_sh_mask.h"
#include "include/asic_reg/gca/gfx_7_2_enum.h"
#include "include/asic_reg/gmc/gmc_6_0_d.h"
#include "include/asic_reg/gmc/gmc_6_0_sh_mask.h"
#include "include/asic_reg/dce/dce_6_0_d.h"
#include "include/asic_reg/dce/dce_6_0_sh_mask.h"
#include "include/si_enums.h"

#ifndef lower_32_bits
#define lower_32_bits(n)	((uint32_t)((n) & 0xffffffff))
#define upper_32_bits(n)	((uint32_t)(((n) >> 16) >> 16))
#endif

/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_IB_FLAG_CE	(1 << 0)


/* gfx_v6_0.c: the golden address configurations */
#ifndef TAHITI_GB_ADDR_CONFIG_GOLDEN
#define TAHITI_GB_ADDR_CONFIG_GOLDEN	0x12011003
#define VERDE_GB_ADDR_CONFIG_GOLDEN	0x12010002
#define HAINAN_GB_ADDR_CONFIG_GOLDEN	0x02010001
#endif

#define ARRAY_MODE(x)					((x) << GB_TILE_MODE0__ARRAY_MODE__SHIFT)
#define PIPE_CONFIG(x)					((x) << GB_TILE_MODE0__PIPE_CONFIG__SHIFT)
#define TILE_SPLIT(x)					((x) << GB_TILE_MODE0__TILE_SPLIT__SHIFT)
#define MICRO_TILE_MODE(x)				((x) << 0)
#define SAMPLE_SPLIT(x)					((x) << GB_TILE_MODE0__SAMPLE_SPLIT__SHIFT)
#define BANK_WIDTH(x)					((x) << 14)
#define BANK_HEIGHT(x)					((x) << 16)
#define MACRO_TILE_ASPECT(x)				((x) << 18)
#define NUM_BANKS(x)					((x) << 20)

#define GFX6_MAX_SE		4
#define GFX6_MAX_SH_PER_SE	2

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
static uint32_t gfx6_create_bitmask(uint32_t bit_width)
{
	return (uint32_t)((1ULL << bit_width) - 1);
}

struct frb_gfx6_rb_config {
	uint32_t rb_backend_disable;
	uint32_t user_rb_backend_disable;
	uint32_t raster_config;
	uint32_t raster_config_1;
};

/* amdgpu_gfx_config, the fields gfx_v6_0 sets */
struct frb_gfx6_config {
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
	uint32_t gb_addr_config;
	uint32_t num_rbs;
	uint32_t double_offchip_lds_buf;
	uint32_t tile_mode_array[32];
	struct frb_gfx6_rb_config rb_config[GFX6_MAX_SE][GFX6_MAX_SH_PER_SE];
};

/* amdgpu_cu_info, one XCC */
struct frb_gfx6_cu_info {
	uint32_t number;
	uint32_t ao_cu_mask;
	uint32_t bitmap[1][4][4];
	uint32_t ao_cu_bitmap[4][2];
};

/* amdgpu_rlc, the fields gfx_v6_0 uses */
struct frb_gfx6_rlc {
	const uint32_t *reg_list;
	uint32_t reg_list_size;
	const struct cs_section_def *cs_data;
	uint32_t clear_state_size;		/* dwords */
	uint32_t clear_state_offset;		/* BAR0 offset */
	uint64_t clear_state_gpu_addr;
	uint32_t save_restore_offset;
	uint64_t save_restore_gpu_addr;
};

/* the CP and RLC images (adev->gfx.*_fw and their versions) */
struct frb_gfx6_fw {
	struct frb_ucode u;
	uint32_t feature_version;
};

/* adev->gfx, the gfx_v6_0 subset */
struct frb_gfx6 {
	struct frb_gfx6_config config;
	struct frb_gfx6_cu_info cu_info;
	struct frb_gfx6_rlc rlc;
	struct frb_gfx6_fw pfp_fw, me_fw, ce_fw, rlc_fw;
	struct frb_ring gfx_ring;
	uint32_t ce_ram_size;
	int sw_done;
	int hw_started;
};

static const uint32_t verde_rlc_save_restore_register_list[] =
{
	(0x8000 << 16) | (0x98f4 >> 2),
	0x00000000,
	(0x8040 << 16) | (0x98f4 >> 2),
	0x00000000,
	(0x8000 << 16) | (0xe80 >> 2),
	0x00000000,
	(0x8041 << 16) | (0x9874 >> 2),
	0x00000000,
	0x00000000
};

static const struct frb_ring_funcs gfx_v6_0_ring_funcs_gfx;

/* empty in amdgpu too */
static void gfx_v6_0_enable_sclk_slowdown_on_pu(struct frb_device *adev, int enable) { (void)adev; (void)enable; }
static void gfx_v6_0_enable_sclk_slowdown_on_pd(struct frb_device *adev, int enable) { (void)adev; (void)enable; }
static void gfx_v6_0_enable_gds_pg(struct frb_device *adev, int enable) { (void)adev; (void)enable; }

/* --- amdgpu's functions, as amdgpu has them ----------------------------------- */

static void gfx_v6_0_tiling_mode_table_init(struct frb_device *adev);
static void gfx_v6_0_select_se_sh(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t instance, int xcc_id);
static uint32_t gfx_v6_0_get_rb_active_bitmap(struct frb_device *adev);
static void gfx_v6_0_raster_config(struct frb_device *adev, uint32_t *rconf);
static void gfx_v6_0_write_harvested_raster_configs(struct frb_device *adev, uint32_t raster_config, unsigned rb_mask, unsigned num_rb);
static void gfx_v6_0_setup_rb(struct frb_device *adev);
static void gfx_v6_0_set_user_cu_inactive_bitmap(struct frb_device *adev, uint32_t bitmap);
static uint32_t gfx_v6_0_get_cu_enabled(struct frb_device *adev);
static void gfx_v6_0_setup_spi(struct frb_device *adev);
static void gfx_v6_0_config_init(struct frb_device *adev);
static void gfx_v6_0_constants_init(struct frb_device *adev);
static void gfx_v6_0_enable_gui_idle_interrupt(struct frb_device *adev, int enable);
static void gfx_v6_0_enable_lbpw(struct frb_device *adev, int enable);
static void gfx_v6_0_wait_for_rlc_serdes(struct frb_device *adev);
static void gfx_v6_0_update_rlc(struct frb_device *adev, uint32_t rlc);
static uint32_t gfx_v6_0_halt_rlc(struct frb_device *adev);
static void gfx_v6_0_rlc_stop(struct frb_device *adev);
static void gfx_v6_0_rlc_start(struct frb_device *adev);
static void gfx_v6_0_rlc_reset(struct frb_device *adev);
static int gfx_v6_0_lbpw_supported(struct frb_device *adev);
static void gfx_v6_0_enable_cgcg(struct frb_device *adev, int enable);
static void gfx_v6_0_enable_mgcg(struct frb_device *adev, int enable);
static void gfx_v6_0_enable_cp_pg(struct frb_device *adev, int enable);
static void gfx_v6_0_enable_gfx_cgpg(struct frb_device *adev, int enable);
static void gfx_v6_0_init_ao_cu_mask(struct frb_device *adev);
static void gfx_v6_0_enable_gfx_static_mgpg(struct frb_device *adev, int enable);
static void gfx_v6_0_enable_gfx_dynamic_mgpg(struct frb_device *adev, int enable);
static void gfx_v6_0_init_gfx_cgpg(struct frb_device *adev);
static void gfx_v6_0_update_gfx_pg(struct frb_device *adev, int enable);
static uint32_t gfx_v6_0_get_csb_size(struct frb_device *adev);
static void gfx_v6_0_init_pg(struct frb_device *adev);
static void gfx_v6_0_fini_pg(struct frb_device *adev);
static void gfx_v6_0_get_cu_info(struct frb_device *adev);

static void gfx_v6_0_tiling_mode_table_init(struct frb_device *adev)
{
	const uint32_t num_tile_mode_states = ARRAY_SIZE(adev->gfx6->config.tile_mode_array);
	uint32_t reg_offset, split_equal_to_row_size, *tilemode;

	memset(adev->gfx6->config.tile_mode_array, 0, sizeof(adev->gfx6->config.tile_mode_array));
	tilemode = adev->gfx6->config.tile_mode_array;

	switch (adev->gfx6->config.mem_row_size_in_kb) {
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

	if (adev->asic_type == CHIP_VERDE) {
		tilemode[0] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[1] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[2] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[3] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[4] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16);
		tilemode[5] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[6] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[7] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[8] =   ARRAY_MODE(ARRAY_LINEAR_ALIGNED);
		tilemode[9] =   MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16);
		tilemode[10] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[11] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[12] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[13] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16);
		tilemode[14] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[15] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[16] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[17] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[18] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16);
		tilemode[19] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[20] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[21] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[22] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[23] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[24] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[25] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[26] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[27] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[28] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[29] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[30] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tilemode[reg_offset]);
	} else if (adev->asic_type == CHIP_OLAND) {
		tilemode[0] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[1] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[2] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[3] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[4] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[5] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(split_equal_to_row_size) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[6] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(split_equal_to_row_size) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[7] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(split_equal_to_row_size) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[8] =   MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_LINEAR_ALIGNED) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[9] =   MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[10] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[11] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[12] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[13] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[14] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[15] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[16] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[17] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(split_equal_to_row_size) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[18] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16);
		tilemode[19] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[20] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[21] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[22] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4);
		tilemode[23] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[24] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2);
		tilemode[25] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				NUM_BANKS(ADDR_SURF_8_BANK) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1);
		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tilemode[reg_offset]);
	} else if (adev->asic_type == CHIP_HAINAN) {
		tilemode[0] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[1] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[2] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[3] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[4] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2);
		tilemode[5] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[6] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[7] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[8] =   ARRAY_MODE(ARRAY_LINEAR_ALIGNED);
		tilemode[9] =   MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2);
		tilemode[10] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_4) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[11] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[12] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[13] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2);
		tilemode[14] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[15] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[16] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[17] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[18] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P2);
		tilemode[19] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[20] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[21] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[22] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_2) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[23] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[24] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_8_BANK);
		tilemode[25] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[26] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[27] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[28] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[29] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[30] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P2) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tilemode[reg_offset]);
	} else if ((adev->asic_type == CHIP_TAHITI) || (adev->asic_type == CHIP_PITCAIRN)) {
		tilemode[0] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_64B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[1] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_128B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[2] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[3] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[4] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16);
		tilemode[5] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[6] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_8) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[7] =   MICRO_TILE_MODE(ADDR_SURF_DEPTH_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[8] =   ARRAY_MODE(ARRAY_LINEAR_ALIGNED);
		tilemode[9] =   MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16);
		tilemode[10] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[11] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_2) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[12] =  MICRO_TILE_MODE(ADDR_SURF_DISPLAY_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[13] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16);
		tilemode[14] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[15] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[16] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK);
		tilemode[17] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[18] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_1D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16);
		tilemode[19] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_XTHICK) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[20] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THICK) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_1) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_16_BANK) |
				TILE_SPLIT(split_equal_to_row_size);
		tilemode[21] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_8) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[22] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_4_BANK);
		tilemode[23] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_256B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_8) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[24] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P8_32x32_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_512B) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[25] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[26] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[27] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[28] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[29] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_1KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_4) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		tilemode[30] =  MICRO_TILE_MODE(ADDR_SURF_THIN_MICRO_TILING) |
				ARRAY_MODE(ARRAY_2D_TILED_THIN1) |
				PIPE_CONFIG(ADDR_SURF_P4_8x16) |
				TILE_SPLIT(ADDR_SURF_TILE_SPLIT_2KB) |
				BANK_WIDTH(ADDR_SURF_BANK_WIDTH_1) |
				BANK_HEIGHT(ADDR_SURF_BANK_HEIGHT_2) |
				MACRO_TILE_ASPECT(ADDR_SURF_MACRO_ASPECT_1) |
				NUM_BANKS(ADDR_SURF_2_BANK);
		for (reg_offset = 0; reg_offset < num_tile_mode_states; reg_offset++)
			WREG32(mmGB_TILE_MODE0 + reg_offset, tilemode[reg_offset]);
	} else {
		frb_err(adev, "gfx: unknown asic: 0x%x", adev->asic_type);
	}
}

static void gfx_v6_0_select_se_sh(struct frb_device *adev, uint32_t se_num,
				  uint32_t sh_num, uint32_t instance, int xcc_id)
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

static uint32_t gfx_v6_0_get_rb_active_bitmap(struct frb_device *adev)
{
	uint32_t data, mask;

	data = RREG32(mmCC_RB_BACKEND_DISABLE) |
		RREG32(mmGC_USER_RB_BACKEND_DISABLE);

	data = REG_GET_FIELD(data, GC_USER_RB_BACKEND_DISABLE, BACKEND_DISABLE);

	mask = gfx6_create_bitmask(adev->gfx6->config.max_backends_per_se/
					 adev->gfx6->config.max_sh_per_se);

	return ~data & mask;
}

static void gfx_v6_0_raster_config(struct frb_device *adev, uint32_t *rconf)
{
	switch (adev->asic_type) {
	case CHIP_TAHITI:
	case CHIP_PITCAIRN:
		*rconf |=
			   (2 << PA_SC_RASTER_CONFIG__RB_XSEL2__SHIFT) |
			   (1 << PA_SC_RASTER_CONFIG__RB_XSEL__SHIFT) |
			   (2 << PA_SC_RASTER_CONFIG__PKR_MAP__SHIFT) |
			   (1 << PA_SC_RASTER_CONFIG__PKR_YSEL__SHIFT) |
			   (2 << PA_SC_RASTER_CONFIG__SE_MAP__SHIFT) |
			   (2 << PA_SC_RASTER_CONFIG__SE_XSEL__SHIFT) |
			   (2 << PA_SC_RASTER_CONFIG__SE_YSEL__SHIFT);
		break;
	case CHIP_VERDE:
		*rconf |=
			   (1 << PA_SC_RASTER_CONFIG__RB_XSEL__SHIFT) |
			   (2 << PA_SC_RASTER_CONFIG__PKR_MAP__SHIFT) |
			   (1 << PA_SC_RASTER_CONFIG__PKR_YSEL__SHIFT);
		break;
	case CHIP_OLAND:
		*rconf |= (1 << PA_SC_RASTER_CONFIG__RB_YSEL__SHIFT);
		break;
	case CHIP_HAINAN:
		*rconf |= 0x0;
		break;
	default:
		frb_err(adev, "gfx: unknown asic: 0x%x", adev->asic_type);
		break;
	}
}

static void gfx_v6_0_write_harvested_raster_configs(struct frb_device *adev,
						    uint32_t raster_config, unsigned rb_mask,
						    unsigned num_rb)
{
	unsigned sh_per_se = max_u(adev->gfx6->config.max_sh_per_se, 1);
	unsigned num_se = max_u(adev->gfx6->config.max_shader_engines, 1);
	unsigned rb_per_pkr = min_u(num_rb / num_se / sh_per_se, 2);
	unsigned rb_per_se = num_rb / num_se;
	unsigned se_mask[4];
	unsigned se;

	se_mask[0] = ((1 << rb_per_se) - 1) & rb_mask;
	se_mask[1] = (se_mask[0] << rb_per_se) & rb_mask;
	se_mask[2] = (se_mask[1] << rb_per_se) & rb_mask;
	se_mask[3] = (se_mask[2] << rb_per_se) & rb_mask;


	for (se = 0; se < num_se; se++) {
		unsigned raster_config_se = raster_config;
		unsigned pkr0_mask = ((1 << rb_per_pkr) - 1) << (se * rb_per_se);
		unsigned pkr1_mask = pkr0_mask << rb_per_pkr;
		int idx = (se / 2) * 2;

		if ((num_se > 1) && (!se_mask[idx] || !se_mask[idx + 1])) {
			raster_config_se &= ~PA_SC_RASTER_CONFIG__SE_MAP_MASK;

			if (!se_mask[idx])
				raster_config_se |= RASTER_CONFIG_SE_MAP_3 << PA_SC_RASTER_CONFIG__SE_MAP__SHIFT;
			else
				raster_config_se |= RASTER_CONFIG_SE_MAP_0 << PA_SC_RASTER_CONFIG__SE_MAP__SHIFT;
		}

		pkr0_mask &= rb_mask;
		pkr1_mask &= rb_mask;
		if (rb_per_se > 2 && (!pkr0_mask || !pkr1_mask)) {
			raster_config_se &= ~PA_SC_RASTER_CONFIG__PKR_MAP_MASK;

			if (!pkr0_mask)
				raster_config_se |= RASTER_CONFIG_PKR_MAP_3 << PA_SC_RASTER_CONFIG__PKR_MAP__SHIFT;
			else
				raster_config_se |= RASTER_CONFIG_PKR_MAP_0 << PA_SC_RASTER_CONFIG__PKR_MAP__SHIFT;
		}

		if (rb_per_se >= 2) {
			unsigned rb0_mask = 1 << (se * rb_per_se);
			unsigned rb1_mask = rb0_mask << 1;

			rb0_mask &= rb_mask;
			rb1_mask &= rb_mask;
			if (!rb0_mask || !rb1_mask) {
				raster_config_se &= ~PA_SC_RASTER_CONFIG__RB_MAP_PKR0_MASK;

				if (!rb0_mask)
					raster_config_se |=
						RASTER_CONFIG_RB_MAP_3 << PA_SC_RASTER_CONFIG__RB_MAP_PKR0__SHIFT;
				else
					raster_config_se |=
						RASTER_CONFIG_RB_MAP_0 << PA_SC_RASTER_CONFIG__RB_MAP_PKR0__SHIFT;
			}

			if (rb_per_se > 2) {
				rb0_mask = 1 << (se * rb_per_se + rb_per_pkr);
				rb1_mask = rb0_mask << 1;
				rb0_mask &= rb_mask;
				rb1_mask &= rb_mask;
				if (!rb0_mask || !rb1_mask) {
					raster_config_se &= ~PA_SC_RASTER_CONFIG__RB_MAP_PKR1_MASK;

					if (!rb0_mask)
						raster_config_se |=
							RASTER_CONFIG_RB_MAP_3 << PA_SC_RASTER_CONFIG__RB_MAP_PKR1__SHIFT;
					else
						raster_config_se |=
							RASTER_CONFIG_RB_MAP_0 << PA_SC_RASTER_CONFIG__RB_MAP_PKR1__SHIFT;
				}
			}
		}

		/* GRBM_GFX_INDEX has a different offset on SI */
		gfx_v6_0_select_se_sh(adev, se, 0xffffffff, 0xffffffff, 0);
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config_se);
	}

	/* GRBM_GFX_INDEX has a different offset on SI */
	gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v6_0_setup_rb(struct frb_device *adev)
{
	int i, j;
	uint32_t data;
	uint32_t raster_config = 0;
	uint32_t active_rbs = 0;
	uint32_t rb_bitmap_width_per_sh = adev->gfx6->config.max_backends_per_se /
					adev->gfx6->config.max_sh_per_se;
	unsigned num_rb_pipes;

	for (i = 0; i < adev->gfx6->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx6->config.max_sh_per_se; j++) {
			gfx_v6_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			data = gfx_v6_0_get_rb_active_bitmap(adev);
			active_rbs |= data <<
				((i * adev->gfx6->config.max_sh_per_se + j) *
				 rb_bitmap_width_per_sh);
		}
	}
	gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	adev->gfx6->config.backend_enable_mask = active_rbs;
	adev->gfx6->config.num_rbs = hweight32(active_rbs);

	num_rb_pipes = min_u(adev->gfx6->config.max_backends_per_se *
			     adev->gfx6->config.max_shader_engines, 16);

	gfx_v6_0_raster_config(adev, &raster_config);

	if (!adev->gfx6->config.backend_enable_mask ||
	     adev->gfx6->config.num_rbs >= num_rb_pipes)
		WREG32(mmPA_SC_RASTER_CONFIG, raster_config);
	else
		gfx_v6_0_write_harvested_raster_configs(adev, raster_config,
							adev->gfx6->config.backend_enable_mask,
							num_rb_pipes);

	/* cache the values for userspace */
	for (i = 0; i < adev->gfx6->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx6->config.max_sh_per_se; j++) {
			gfx_v6_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			adev->gfx6->config.rb_config[i][j].rb_backend_disable =
				RREG32(mmCC_RB_BACKEND_DISABLE);
			adev->gfx6->config.rb_config[i][j].user_rb_backend_disable =
				RREG32(mmGC_USER_RB_BACKEND_DISABLE);
			adev->gfx6->config.rb_config[i][j].raster_config =
				RREG32(mmPA_SC_RASTER_CONFIG);
		}
	}
	gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v6_0_set_user_cu_inactive_bitmap(struct frb_device *adev,
						 uint32_t bitmap)
{
	uint32_t data;

	if (!bitmap)
		return;

	data = bitmap << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;
	data &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;

	WREG32(mmGC_USER_SHADER_ARRAY_CONFIG, data);
}

static uint32_t gfx_v6_0_get_cu_enabled(struct frb_device *adev)
{
	uint32_t data, mask;

	data = RREG32(mmCC_GC_SHADER_ARRAY_CONFIG) |
		RREG32(mmGC_USER_SHADER_ARRAY_CONFIG);

	mask = gfx6_create_bitmask(adev->gfx6->config.max_cu_per_sh);
	return ~REG_GET_FIELD(data, CC_GC_SHADER_ARRAY_CONFIG, INACTIVE_CUS) & mask;
}

static void gfx_v6_0_setup_spi(struct frb_device *adev)
{
	int i, j, k;
	uint32_t data, mask;
	uint32_t active_cu = 0;

	for (i = 0; i < adev->gfx6->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx6->config.max_sh_per_se; j++) {
			gfx_v6_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			data = RREG32(mmSPI_STATIC_THREAD_MGMT_3);
			active_cu = gfx_v6_0_get_cu_enabled(adev);

			mask = 1;
			for (k = 0; k < 16; k++) {
				mask <<= k;
				if (active_cu & mask) {
					data &= ~mask;
					WREG32(mmSPI_STATIC_THREAD_MGMT_3, data);
					break;
				}
			}
		}
	}
	gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v6_0_config_init(struct frb_device *adev)
{
	adev->gfx6->config.double_offchip_lds_buf = 0;
}

static void gfx_v6_0_constants_init(struct frb_device *adev)
{
	uint32_t gb_addr_config = 0;
	uint32_t mc_arb_ramcfg;
	uint32_t sx_debug_1;
	uint32_t hdp_host_path_cntl;
	uint32_t tmp;

	switch (adev->asic_type) {
	case CHIP_TAHITI:
		adev->gfx6->config.max_shader_engines = 2;
		adev->gfx6->config.max_tile_pipes = 12;
		adev->gfx6->config.max_cu_per_sh = 8;
		adev->gfx6->config.max_sh_per_se = 2;
		adev->gfx6->config.max_backends_per_se = 4;
		adev->gfx6->config.max_texture_channel_caches = 12;
		adev->gfx6->config.max_gprs = 256;
		adev->gfx6->config.max_gs_threads = 32;
		adev->gfx6->config.max_hw_contexts = 8;

		adev->gfx6->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx6->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx6->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx6->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = TAHITI_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_PITCAIRN:
		adev->gfx6->config.max_shader_engines = 2;
		adev->gfx6->config.max_tile_pipes = 8;
		adev->gfx6->config.max_cu_per_sh = 5;
		adev->gfx6->config.max_sh_per_se = 2;
		adev->gfx6->config.max_backends_per_se = 4;
		adev->gfx6->config.max_texture_channel_caches = 8;
		adev->gfx6->config.max_gprs = 256;
		adev->gfx6->config.max_gs_threads = 32;
		adev->gfx6->config.max_hw_contexts = 8;

		adev->gfx6->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx6->config.sc_prim_fifo_size_backend = 0x100;
		adev->gfx6->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx6->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = TAHITI_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_VERDE:
		adev->gfx6->config.max_shader_engines = 1;
		adev->gfx6->config.max_tile_pipes = 4;
		adev->gfx6->config.max_cu_per_sh = 5;
		adev->gfx6->config.max_sh_per_se = 2;
		adev->gfx6->config.max_backends_per_se = 4;
		adev->gfx6->config.max_texture_channel_caches = 4;
		adev->gfx6->config.max_gprs = 256;
		adev->gfx6->config.max_gs_threads = 32;
		adev->gfx6->config.max_hw_contexts = 8;

		adev->gfx6->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx6->config.sc_prim_fifo_size_backend = 0x40;
		adev->gfx6->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx6->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = VERDE_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_OLAND:
		adev->gfx6->config.max_shader_engines = 1;
		adev->gfx6->config.max_tile_pipes = 4;
		adev->gfx6->config.max_cu_per_sh = 6;
		adev->gfx6->config.max_sh_per_se = 1;
		adev->gfx6->config.max_backends_per_se = 2;
		adev->gfx6->config.max_texture_channel_caches = 4;
		adev->gfx6->config.max_gprs = 256;
		adev->gfx6->config.max_gs_threads = 16;
		adev->gfx6->config.max_hw_contexts = 8;

		adev->gfx6->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx6->config.sc_prim_fifo_size_backend = 0x40;
		adev->gfx6->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx6->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = VERDE_GB_ADDR_CONFIG_GOLDEN;
		break;
	case CHIP_HAINAN:
		adev->gfx6->config.max_shader_engines = 1;
		adev->gfx6->config.max_tile_pipes = 4;
		adev->gfx6->config.max_cu_per_sh = 5;
		adev->gfx6->config.max_sh_per_se = 1;
		adev->gfx6->config.max_backends_per_se = 1;
		adev->gfx6->config.max_texture_channel_caches = 2;
		adev->gfx6->config.max_gprs = 256;
		adev->gfx6->config.max_gs_threads = 16;
		adev->gfx6->config.max_hw_contexts = 8;

		adev->gfx6->config.sc_prim_fifo_size_frontend = 0x20;
		adev->gfx6->config.sc_prim_fifo_size_backend = 0x40;
		adev->gfx6->config.sc_hiz_tile_fifo_size = 0x30;
		adev->gfx6->config.sc_earlyz_tile_fifo_size = 0x130;
		gb_addr_config = HAINAN_GB_ADDR_CONFIG_GOLDEN;
		break;
	default:
		frb_err(adev, "gfx: no gfx_v6_0 constants for this chip");
		return;
		break;
	}

	WREG32(mmGRBM_CNTL, (0xff << GRBM_CNTL__READ_TIMEOUT__SHIFT));
	WREG32(mmSRBM_INT_CNTL, 1);
	WREG32(mmSRBM_INT_ACK, 1);

	WREG32(mmBIF_FB_EN, BIF_FB_EN__FB_READ_EN_MASK | BIF_FB_EN__FB_WRITE_EN_MASK);

	adev->gfx6->config.mc_arb_ramcfg = RREG32(mmMC_ARB_RAMCFG);
	mc_arb_ramcfg = adev->gfx6->config.mc_arb_ramcfg;

	adev->gfx6->config.num_tile_pipes = adev->gfx6->config.max_tile_pipes;
	adev->gfx6->config.mem_max_burst_length_bytes = 256;
	tmp = (mc_arb_ramcfg & MC_ARB_RAMCFG__NOOFCOLS_MASK) >> MC_ARB_RAMCFG__NOOFCOLS__SHIFT;
	adev->gfx6->config.mem_row_size_in_kb = (4 * (1 << (8 + tmp))) / 1024;
	if (adev->gfx6->config.mem_row_size_in_kb > 4)
		adev->gfx6->config.mem_row_size_in_kb = 4;
	adev->gfx6->config.shader_engine_tile_size = 32;
	adev->gfx6->config.num_gpus = 1;
	adev->gfx6->config.multi_gpu_tile_size = 64;

	gb_addr_config &= ~GB_ADDR_CONFIG__ROW_SIZE_MASK;
	switch (adev->gfx6->config.mem_row_size_in_kb) {
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
	gb_addr_config &= ~GB_ADDR_CONFIG__NUM_SHADER_ENGINES_MASK;
	if (adev->gfx6->config.max_shader_engines == 2)
		gb_addr_config |= 1 << GB_ADDR_CONFIG__NUM_SHADER_ENGINES__SHIFT;
	adev->gfx6->config.gb_addr_config = gb_addr_config;

	WREG32(mmGB_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMIF_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMIF_ADDR_CALC, gb_addr_config);
	WREG32(mmHDP_ADDR_CONFIG, gb_addr_config);
	WREG32(mmDMA_TILING_CONFIG + DMA0_REGISTER_OFFSET, gb_addr_config);
	WREG32(mmDMA_TILING_CONFIG + DMA1_REGISTER_OFFSET, gb_addr_config);

#if 0
	if (adev->has_uvd) {
		WREG32(mmUVD_UDEC_ADDR_CONFIG, gb_addr_config);
		WREG32(mmUVD_UDEC_DB_ADDR_CONFIG, gb_addr_config);
		WREG32(mmUVD_UDEC_DBW_ADDR_CONFIG, gb_addr_config);
	}
#endif
	gfx_v6_0_tiling_mode_table_init(adev);

	gfx_v6_0_setup_rb(adev);

	gfx_v6_0_setup_spi(adev);

	gfx_v6_0_get_cu_info(adev);
	gfx_v6_0_config_init(adev);

	WREG32(mmCP_QUEUE_THRESHOLDS,
		((0x16 << CP_QUEUE_THRESHOLDS__ROQ_IB1_START__SHIFT) |
		(0x2b << CP_QUEUE_THRESHOLDS__ROQ_IB2_START__SHIFT)));

	/* set HW defaults for 3D engine */
	WREG32(mmCP_MEQ_THRESHOLDS,
		(0x30 << CP_MEQ_THRESHOLDS__MEQ1_START__SHIFT) |
		(0x60 << CP_MEQ_THRESHOLDS__MEQ2_START__SHIFT));

	sx_debug_1 = RREG32(mmSX_DEBUG_1);
	WREG32(mmSX_DEBUG_1, sx_debug_1);

	WREG32(mmSPI_CONFIG_CNTL_1, (4 << SPI_CONFIG_CNTL_1__VTX_DONE_DELAY__SHIFT));

	WREG32(mmPA_SC_FIFO_SIZE, ((adev->gfx6->config.sc_prim_fifo_size_frontend << PA_SC_FIFO_SIZE__SC_FRONTEND_PRIM_FIFO_SIZE__SHIFT) |
				   (adev->gfx6->config.sc_prim_fifo_size_backend << PA_SC_FIFO_SIZE__SC_BACKEND_PRIM_FIFO_SIZE__SHIFT) |
				   (adev->gfx6->config.sc_hiz_tile_fifo_size << PA_SC_FIFO_SIZE__SC_HIZ_TILE_FIFO_SIZE__SHIFT) |
				   (adev->gfx6->config.sc_earlyz_tile_fifo_size << PA_SC_FIFO_SIZE__SC_EARLYZ_TILE_FIFO_SIZE__SHIFT)));

	WREG32(mmVGT_NUM_INSTANCES, 1);
	WREG32(mmCP_PERFMON_CNTL, 0);
	WREG32(mmSQ_CONFIG, 0);
	WREG32(mmPA_SC_FORCE_EOV_MAX_CNTS, ((4095 << PA_SC_FORCE_EOV_MAX_CNTS__FORCE_EOV_MAX_CLK_CNT__SHIFT) |
					  (255 << PA_SC_FORCE_EOV_MAX_CNTS__FORCE_EOV_MAX_REZ_CNT__SHIFT)));

	WREG32(mmVGT_CACHE_INVALIDATION,
		(VC_AND_TC << VGT_CACHE_INVALIDATION__CACHE_INVALIDATION__SHIFT) |
		(ES_AND_GS_AUTO << VGT_CACHE_INVALIDATION__AUTO_INVLD_EN__SHIFT));

	WREG32(mmVGT_GS_VERTEX_REUSE, 16);
	WREG32(mmPA_SC_LINE_STIPPLE_STATE, 0);

	WREG32(mmCB_PERFCOUNTER0_SELECT0, 0);
	WREG32(mmCB_PERFCOUNTER0_SELECT1, 0);
	WREG32(mmCB_PERFCOUNTER1_SELECT0, 0);
	WREG32(mmCB_PERFCOUNTER1_SELECT1, 0);
	WREG32(mmCB_PERFCOUNTER2_SELECT0, 0);
	WREG32(mmCB_PERFCOUNTER2_SELECT1, 0);
	WREG32(mmCB_PERFCOUNTER3_SELECT0, 0);
	WREG32(mmCB_PERFCOUNTER3_SELECT1, 0);

	hdp_host_path_cntl = RREG32(mmHDP_HOST_PATH_CNTL);
	WREG32(mmHDP_HOST_PATH_CNTL, hdp_host_path_cntl);

	WREG32(mmPA_CL_ENHANCE, PA_CL_ENHANCE__CLIP_VTX_REORDER_ENA_MASK |
				(3 << PA_CL_ENHANCE__NUM_CLIP_SEQ__SHIFT));

	udelay(50);
}

static void gfx_v6_0_enable_gui_idle_interrupt(struct frb_device *adev,
					       int enable)
{
	uint32_t tmp = RREG32(mmCP_INT_CNTL_RING0);
	uint32_t mask;
	int i;

	if (enable)
		tmp |= (CP_INT_CNTL__CNTX_BUSY_INT_ENABLE_MASK |
			CP_INT_CNTL__CNTX_EMPTY_INT_ENABLE_MASK);
	else
		tmp &= ~(CP_INT_CNTL__CNTX_BUSY_INT_ENABLE_MASK |
			 CP_INT_CNTL__CNTX_EMPTY_INT_ENABLE_MASK);
	WREG32(mmCP_INT_CNTL_RING0, tmp);

	if (!enable) {
		/* read a gfx register */
		tmp = RREG32(mmDB_DEPTH_INFO);

		mask = RLC_BUSY_STATUS | GFX_POWER_STATUS | GFX_CLOCK_STATUS | GFX_LS_STATUS;
		for (i = 0; i < adev->usec_timeout; i++) {
			if ((RREG32(mmRLC_STAT) & mask) == (GFX_CLOCK_STATUS | GFX_POWER_STATUS))
				break;
			udelay(1);
		}
	}
}

static void gfx_v6_0_enable_lbpw(struct frb_device *adev, int enable)
{
	WREG32_FIELD(RLC_LB_CNTL, LOAD_BALANCE_ENABLE, enable ? 1 : 0);

	if (!enable) {
		gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
		WREG32(mmSPI_LB_CU_MASK, 0x00ff);
	}
}

static void gfx_v6_0_wait_for_rlc_serdes(struct frb_device *adev)
{
	int i;

	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmRLC_SERDES_MASTER_BUSY_0) == 0)
			break;
		udelay(1);
	}

	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32(mmRLC_SERDES_MASTER_BUSY_1) == 0)
			break;
		udelay(1);
	}
}

static void gfx_v6_0_update_rlc(struct frb_device *adev, uint32_t rlc)
{
	uint32_t tmp;

	tmp = RREG32(mmRLC_CNTL);
	if (tmp != rlc)
		WREG32(mmRLC_CNTL, rlc);
}

static uint32_t gfx_v6_0_halt_rlc(struct frb_device *adev)
{
	uint32_t data, orig;

	orig = data = RREG32(mmRLC_CNTL);

	if (data & RLC_CNTL__RLC_ENABLE_F32_MASK) {
		data &= ~RLC_CNTL__RLC_ENABLE_F32_MASK;
		WREG32(mmRLC_CNTL, data);

		gfx_v6_0_wait_for_rlc_serdes(adev);
	}

	return orig;
}

static void gfx_v6_0_rlc_stop(struct frb_device *adev)
{
	WREG32(mmRLC_CNTL, 0);

	gfx_v6_0_enable_gui_idle_interrupt(adev, 0);
	gfx_v6_0_wait_for_rlc_serdes(adev);
}

static void gfx_v6_0_rlc_start(struct frb_device *adev)
{
	WREG32(mmRLC_CNTL, RLC_CNTL__RLC_ENABLE_F32_MASK);

	gfx_v6_0_enable_gui_idle_interrupt(adev, 1);

	udelay(50);
}

static void gfx_v6_0_rlc_reset(struct frb_device *adev)
{
	WREG32_FIELD(GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);
	udelay(50);
	WREG32_FIELD(GRBM_SOFT_RESET, SOFT_RESET_RLC, 0);
	udelay(50);
}

static int gfx_v6_0_lbpw_supported(struct frb_device *adev)
{
	uint32_t tmp;

	/* Enable LBPW only for DDR3 */
	tmp = RREG32(mmMC_SEQ_MISC0);
	if ((tmp & 0xF0000000) == 0xB0000000)
		return 1;
	return 0;
}

static void gfx_v6_0_enable_cgcg(struct frb_device *adev, int enable)
{
	uint32_t data, orig, tmp;

	orig = data = RREG32(mmRLC_CGCG_CGLS_CTRL);

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)) {
		gfx_v6_0_enable_gui_idle_interrupt(adev, 1);

		WREG32(mmRLC_GCPM_GENERAL_3, 0x00000080);

		tmp = gfx_v6_0_halt_rlc(adev);

		WREG32(mmRLC_SERDES_WR_MASTER_MASK_0, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_MASTER_MASK_1, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_CTRL, 0x00b000ff);

		gfx_v6_0_wait_for_rlc_serdes(adev);
		gfx_v6_0_update_rlc(adev, tmp);

		WREG32(mmRLC_SERDES_WR_CTRL, 0x007000ff);

		data |= RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;
	} else {
		gfx_v6_0_enable_gui_idle_interrupt(adev, 0);

		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);
		RREG32(mmCB_CGTT_SCLK_CTRL);

		data &= ~(RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK);
	}

	if (orig != data)
		WREG32(mmRLC_CGCG_CGLS_CTRL, data);

}

static void gfx_v6_0_enable_mgcg(struct frb_device *adev, int enable)
{

	uint32_t data, orig, tmp = 0;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGCG)) {
		orig = data = RREG32(mmCGTS_SM_CTRL_REG);
		data = 0x96940200;
		if (orig != data)
			WREG32(mmCGTS_SM_CTRL_REG, data);

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CP_LS) {
			orig = data = RREG32(mmCP_MEM_SLP_CNTL);
			data |= CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
			if (orig != data)
				WREG32(mmCP_MEM_SLP_CNTL, data);
		}

		orig = data = RREG32(mmRLC_CGTT_MGCG_OVERRIDE);
		data &= 0xffffffc0;
		if (orig != data)
			WREG32(mmRLC_CGTT_MGCG_OVERRIDE, data);

		tmp = gfx_v6_0_halt_rlc(adev);

		WREG32(mmRLC_SERDES_WR_MASTER_MASK_0, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_MASTER_MASK_1, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_CTRL, 0x00d000ff);

		gfx_v6_0_update_rlc(adev, tmp);
	} else {
		orig = data = RREG32(mmRLC_CGTT_MGCG_OVERRIDE);
		data |= 0x00000003;
		if (orig != data)
			WREG32(mmRLC_CGTT_MGCG_OVERRIDE, data);

		data = RREG32(mmCP_MEM_SLP_CNTL);
		if (data & CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK) {
			data &= ~CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
			WREG32(mmCP_MEM_SLP_CNTL, data);
		}
		orig = data = RREG32(mmCGTS_SM_CTRL_REG);
		data |= CGTS_SM_CTRL_REG__LS_OVERRIDE_MASK | CGTS_SM_CTRL_REG__OVERRIDE_MASK;
		if (orig != data)
			WREG32(mmCGTS_SM_CTRL_REG, data);

		tmp = gfx_v6_0_halt_rlc(adev);

		WREG32(mmRLC_SERDES_WR_MASTER_MASK_0, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_MASTER_MASK_1, 0xffffffff);
		WREG32(mmRLC_SERDES_WR_CTRL, 0x00e000ff);

		gfx_v6_0_update_rlc(adev, tmp);
	}
}

static void gfx_v6_0_enable_cp_pg(struct frb_device *adev, int enable)
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

static void gfx_v6_0_enable_gfx_cgpg(struct frb_device *adev,
				     int enable)
{
	if (enable && (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG)) {
		WREG32(mmRLC_TTOP_D, RLC_PUD(0x10) | RLC_PDD(0x10) | RLC_TTPD(0x10) | RLC_MSD(0x10));
		WREG32_FIELD(RLC_PG_CNTL, GFX_POWER_GATING_ENABLE, 1);
		WREG32_FIELD(RLC_AUTO_PG_CTRL, AUTO_PG_EN, 1);
	} else {
		WREG32_FIELD(RLC_AUTO_PG_CTRL, AUTO_PG_EN, 0);
		(void)RREG32(mmDB_RENDER_CONTROL);
	}
}

static void gfx_v6_0_init_ao_cu_mask(struct frb_device *adev)
{
	uint32_t tmp;

	WREG32(mmRLC_PG_ALWAYS_ON_CU_MASK, adev->gfx6->cu_info.ao_cu_mask);

	tmp = RREG32(mmRLC_MAX_PG_CU);
	tmp &= ~RLC_MAX_PG_CU__MAX_POWERED_UP_CU_MASK;
	tmp |= (adev->gfx6->cu_info.number << RLC_MAX_PG_CU__MAX_POWERED_UP_CU__SHIFT);
	WREG32(mmRLC_MAX_PG_CU, tmp);
}

static void gfx_v6_0_enable_gfx_static_mgpg(struct frb_device *adev,
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

static void gfx_v6_0_enable_gfx_dynamic_mgpg(struct frb_device *adev,
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

static void gfx_v6_0_init_gfx_cgpg(struct frb_device *adev)
{
	uint32_t tmp;

	WREG32(mmRLC_SAVE_AND_RESTORE_BASE, adev->gfx6->rlc.save_restore_gpu_addr >> 8);
	WREG32_FIELD(RLC_PG_CNTL, GFX_POWER_GATING_SRC, 1);
	WREG32(mmRLC_CLEAR_STATE_RESTORE_BASE, adev->gfx6->rlc.clear_state_gpu_addr >> 8);

	tmp = RREG32(mmRLC_AUTO_PG_CTRL);
	tmp &= ~RLC_AUTO_PG_CTRL__GRBM_REG_SAVE_GFX_IDLE_THRESHOLD_MASK;
	tmp |= (0x700 << RLC_AUTO_PG_CTRL__GRBM_REG_SAVE_GFX_IDLE_THRESHOLD__SHIFT);
	tmp &= ~RLC_AUTO_PG_CTRL__PG_AFTER_GRBM_REG_SAVE_THRESHOLD_MASK;
	WREG32(mmRLC_AUTO_PG_CTRL, tmp);
}

static void gfx_v6_0_update_gfx_pg(struct frb_device *adev, int enable)
{
	gfx_v6_0_enable_gfx_cgpg(adev, enable);
	gfx_v6_0_enable_gfx_static_mgpg(adev, enable);
	gfx_v6_0_enable_gfx_dynamic_mgpg(adev, enable);
}

static uint32_t gfx_v6_0_get_csb_size(struct frb_device *adev)
{
	uint32_t count = 0;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;

	if (adev->gfx6->rlc.cs_data == NULL)
		return 0;

	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = adev->gfx6->rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT)
				count += 2 + ext->reg_count;
			else
				return 0;
		}
	}
	/* pa_sc_raster_config */
	count += 3;
	/* end clear state */
	count += 2;
	/* clear state */
	count += 2;

	return count;
}

static void gfx_v6_0_init_pg(struct frb_device *adev)
{
	if (adev->pg_flags & (AMD_PG_SUPPORT_GFX_PG |
			      AMD_PG_SUPPORT_GFX_SMG |
			      AMD_PG_SUPPORT_GFX_DMG |
			      AMD_PG_SUPPORT_CP |
			      AMD_PG_SUPPORT_GDS |
			      AMD_PG_SUPPORT_RLC_SMU_HS)) {
		gfx_v6_0_enable_sclk_slowdown_on_pu(adev, 1);
		gfx_v6_0_enable_sclk_slowdown_on_pd(adev, 1);
		if (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG) {
			gfx_v6_0_init_gfx_cgpg(adev);
			gfx_v6_0_enable_cp_pg(adev, 1);
			gfx_v6_0_enable_gds_pg(adev, 1);
		} else {
			WREG32(mmRLC_SAVE_AND_RESTORE_BASE, adev->gfx6->rlc.save_restore_gpu_addr >> 8);
			WREG32(mmRLC_CLEAR_STATE_RESTORE_BASE, adev->gfx6->rlc.clear_state_gpu_addr >> 8);

		}
		gfx_v6_0_init_ao_cu_mask(adev);
		gfx_v6_0_update_gfx_pg(adev, 1);
	} else {

		WREG32(mmRLC_SAVE_AND_RESTORE_BASE, adev->gfx6->rlc.save_restore_gpu_addr >> 8);
		WREG32(mmRLC_CLEAR_STATE_RESTORE_BASE, adev->gfx6->rlc.clear_state_gpu_addr >> 8);
	}
}

static void gfx_v6_0_fini_pg(struct frb_device *adev)
{
	if (adev->pg_flags & (AMD_PG_SUPPORT_GFX_PG |
			      AMD_PG_SUPPORT_GFX_SMG |
			      AMD_PG_SUPPORT_GFX_DMG |
			      AMD_PG_SUPPORT_CP |
			      AMD_PG_SUPPORT_GDS |
			      AMD_PG_SUPPORT_RLC_SMU_HS)) {
		gfx_v6_0_update_gfx_pg(adev, 0);
		if (adev->pg_flags & AMD_PG_SUPPORT_GFX_PG) {
			gfx_v6_0_enable_cp_pg(adev, 0);
			gfx_v6_0_enable_gds_pg(adev, 0);
		}
	}
}

static void gfx_v6_0_get_cu_info(struct frb_device *adev)
{
	int i, j, k, counter, active_cu_number = 0;
	uint32_t mask, bitmap, ao_bitmap, ao_cu_mask = 0;
	struct frb_gfx6_cu_info *cu_info = &adev->gfx6->cu_info;
	unsigned disable_masks[4 * 2];
	uint32_t ao_cu_num;

	if (0)	/* adev->flags & AMD_IS_APU: no SI APU */
		ao_cu_num = 2;
	else
		ao_cu_num = adev->gfx6->config.max_cu_per_sh;

	memset(cu_info, 0, sizeof(*cu_info));

	/* amdgpu_gfx_parse_disable_cu: the disable_cu parameter is unset */
	memset(disable_masks, 0, sizeof(disable_masks));

	for (i = 0; i < adev->gfx6->config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx6->config.max_sh_per_se; j++) {
			mask = 1;
			ao_bitmap = 0;
			counter = 0;
			gfx_v6_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			if (i < 4 && j < 2)
				gfx_v6_0_set_user_cu_inactive_bitmap(
					adev, disable_masks[i * 2 + j]);
			bitmap = gfx_v6_0_get_cu_enabled(adev);
			cu_info->bitmap[0][i][j] = bitmap;

			for (k = 0; k < adev->gfx6->config.max_cu_per_sh; k++) {
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

	gfx_v6_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cu_info->number = active_cu_number;
	cu_info->ao_cu_mask = ao_cu_mask;
}


/* --- microcode, RLC objects --------------------------------------------------- */

static int gfx6_request(struct frb_device *adev, const char *chip, const char *what, struct frb_gfx6_fw *f)
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
		frb_err(adev, "gfx6: Failed to load firmware \"%s\" (%d)", name, r);
		return r;
	}
	r = frb_ucode_parse(adev, name, &fw, &f->u);
	if (r)
		return r;
	/* gfx_firmware_header_v1_0 / rlc_firmware_header_v1_0: ucode_feature_version after the common header */
	f->feature_version = frb_ucode_le32(&f->u, FRB_FWH_GFX_UCODE_FEATURE_VERSION);
	return 0;
}

/* gfx_v6_0_init_microcode: the amdgpu-headered images */
static int gfx_v6_0_init_microcode(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	const char *chip_name;
	int err;

	switch (adev->asic_type) {
	case CHIP_TAHITI:
		chip_name = "tahiti";
		break;
	case CHIP_PITCAIRN:
		chip_name = "pitcairn";
		break;
	case CHIP_VERDE:
		chip_name = "verde";
		break;
	case CHIP_OLAND:
		chip_name = "oland";
		break;
	case CHIP_HAINAN:
		chip_name = "hainan";
		break;
	default:
		return -ENODEV;
	}
	err = gfx6_request(adev, chip_name, "pfp", &g->pfp_fw);
	if (!err)
		err = gfx6_request(adev, chip_name, "me", &g->me_fw);
	if (!err)
		err = gfx6_request(adev, chip_name, "ce", &g->ce_fw);
	if (!err)
		err = gfx6_request(adev, chip_name, "rlc", &g->rlc_fw);
	if (err)
		frb_err(adev, "gfx6: Failed to load firmware %s gfx firmware", chip_name);
	else
		frb_info(adev, "gfx: %s pfp %u (feature %u), me %u (%u), ce %u (%u), rlc %u (%u)", chip_name,
			 g->pfp_fw.u.ucode_version, g->pfp_fw.feature_version, g->me_fw.u.ucode_version,
			 g->me_fw.feature_version, g->ce_fw.u.ucode_version, g->ce_fw.feature_version,
			 g->rlc_fw.u.ucode_version, g->rlc_fw.feature_version);
	return err;
}

/*
 * gfx_v6_0_get_csb_buffer (amdgpu_gfx_csb_preamble_start, _data_parser,
 * _preamble_end), written straight into the block in VRAM as little-endian
 * dwords (frb_vram_write32: the cpu_to_le32 of amdgpu's buffer)
 */
static void gfx_v6_0_get_csb_buffer(struct frb_device *adev, uint32_t at)
{
	struct frb_gfx6 *g = adev->gfx6;
	const struct cs_section_def *sect;
	const struct cs_extent_def *ext;
	uint32_t count = 0, i;

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
	CSB(PACKET3(PACKET3_SET_CONTEXT_REG, 1));
	CSB(mmPA_SC_RASTER_CONFIG - PACKET3_SET_CONTEXT_REG_START);
	CSB(g->config.rb_config[0][0].raster_config);
	CSB(PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	CSB(PACKET3_PREAMBLE_END_CLEAR_STATE);
	CSB(PACKET3(PACKET3_CLEAR_STATE, 0));
	CSB(0);
#undef CSB
}

/*
 * gfx_v6_0_rlc_init: the save/restore list (amdgpu_gfx_rlc_init_sr) and the
 * clear-state block, both in VRAM. The block's contents depend on
 * rb_config[0][0].raster_config, which constants_init sets: amdgpu fills
 * it in sw_init, when that is still 0 (the CSB's raster config is then 0
 * on Linux too); Firebrand does the same, then fills it again after
 * constants_init (gfx_v6_0_csb_refill) so the RLC's copy carries the
 * value the ring's clear state has. Linux's CSB keeps 0.
 */
static int gfx_v6_0_rlc_init(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	uint64_t reg_list_mc_addr;
	uint32_t dws, i;
	int r;

	g->rlc.reg_list = verde_rlc_save_restore_register_list;
	g->rlc.reg_list_size = (uint32_t)ARRAY_SIZE(verde_rlc_save_restore_register_list);
	g->rlc.cs_data = si_cs_data;

	/* init save restore block */
	r = frb_vram_obj_alloc(adev, g->rlc.reg_list_size * 4, 4096, &g->rlc.save_restore_offset);
	if (r)
		return r;
	g->rlc.save_restore_gpu_addr = frb_vram_gpu_addr(adev, g->rlc.save_restore_offset);
	for (i = 0; i < g->rlc.reg_list_size; i++)
		frb_vram_write32(adev, g->rlc.save_restore_offset + 4 * i, g->rlc.reg_list[i]);

	/* clear state block */
	g->rlc.clear_state_size = gfx_v6_0_get_csb_size(adev);
	dws = g->rlc.clear_state_size + (256 / 4);
	r = frb_vram_obj_alloc(adev, dws * 4, 4096, &g->rlc.clear_state_offset);
	if (r)
		return r;
	g->rlc.clear_state_gpu_addr = frb_vram_gpu_addr(adev, g->rlc.clear_state_offset);
	reg_list_mc_addr = g->rlc.clear_state_gpu_addr + 256;
	frb_vram_write32(adev, g->rlc.clear_state_offset + 0, upper_32_bits(reg_list_mc_addr));
	frb_vram_write32(adev, g->rlc.clear_state_offset + 4, lower_32_bits(reg_list_mc_addr));
	frb_vram_write32(adev, g->rlc.clear_state_offset + 8, g->rlc.clear_state_size);
	gfx_v6_0_get_csb_buffer(adev, g->rlc.clear_state_offset + 256);
	frb_flush_hdp(adev);
	return 0;
}

/* gfx_v6_0_rlc_resume: the RLC image by MMIO */
static int gfx_v6_0_rlc_resume(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	const struct frb_ucode *u = &g->rlc_fw.u;
	uint32_t i, fw_size;

	if (!u->data)
		return -EINVAL;

	gfx_v6_0_rlc_stop(adev);
	gfx_v6_0_rlc_reset(adev);
	gfx_v6_0_init_pg(adev);
	/* gfx_v6_0_init_cg: empty */

	WREG32(mmRLC_RL_BASE, 0);
	WREG32(mmRLC_RL_SIZE, 0);
	WREG32(mmRLC_LB_CNTL, 0);
	WREG32(mmRLC_LB_CNTR_MAX, 0xffffffff);
	WREG32(mmRLC_LB_CNTR_INIT, 0);
	WREG32(mmRLC_LB_INIT_CU_MASK, 0xffffffff);

	WREG32(mmRLC_MC_CNTL, 0);
	WREG32(mmRLC_UCODE_CNTL, 0);

	fw_size = u->ucode_size_bytes / 4;
	for (i = 0; i < fw_size; i++) {
		WREG32(mmRLC_UCODE_ADDR, i);
		WREG32(mmRLC_UCODE_DATA, frb_ucode_le32(u, u->ucode_array_offset + 4 * i));
	}
	WREG32(mmRLC_UCODE_ADDR, 0);

	gfx_v6_0_enable_lbpw(adev, gfx_v6_0_lbpw_supported(adev));
	gfx_v6_0_rlc_start(adev);
	return 0;
}

/* --- the command processor ----------------------------------------------------- */

static void gfx_v6_0_cp_gfx_enable(struct frb_device *adev, int enable)
{
	if (enable) {
		WREG32(mmCP_ME_CNTL, 0);
	} else {
		WREG32(mmCP_ME_CNTL, (CP_ME_CNTL__ME_HALT_MASK |
				      CP_ME_CNTL__PFP_HALT_MASK |
				      CP_ME_CNTL__CE_HALT_MASK));
		WREG32(mmSCRATCH_UMSK, 0);
	}
	udelay(50);
}

static void load_ucode(struct frb_device *adev, const struct frb_ucode *u, uint32_t addr_reg, uint32_t data_reg)
{
	uint32_t i, fw_size = u->ucode_size_bytes / 4;

	WREG32(addr_reg, 0);
	for (i = 0; i < fw_size; i++)
		WREG32(data_reg, frb_ucode_le32(u, u->ucode_array_offset + 4 * i));
	WREG32(addr_reg, 0);
}

static int gfx_v6_0_cp_gfx_load_microcode(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;

	if (!g->me_fw.u.data || !g->pfp_fw.u.data || !g->ce_fw.u.data)
		return -EINVAL;

	gfx_v6_0_cp_gfx_enable(adev, 0);
	load_ucode(adev, &g->pfp_fw.u, mmCP_PFP_UCODE_ADDR, mmCP_PFP_UCODE_DATA);
	load_ucode(adev, &g->ce_fw.u, mmCP_CE_UCODE_ADDR, mmCP_CE_UCODE_DATA);
	load_ucode(adev, &g->me_fw.u, mmCP_ME_RAM_WADDR, mmCP_ME_RAM_DATA);

	WREG32(mmCP_PFP_UCODE_ADDR, 0);
	WREG32(mmCP_CE_UCODE_ADDR, 0);
	WREG32(mmCP_ME_RAM_WADDR, 0);
	WREG32(mmCP_ME_RAM_RADDR, 0);
	return 0;
}

static int gfx_v6_0_cp_gfx_start(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	struct frb_ring *ring = &g->gfx_ring;
	uint32_t i;
	int r;

	r = frb_ring_alloc(ring, 7 + 4);
	if (r) {
		frb_err(adev, "gfx: cp failed to lock ring (%d).", r);
		return r;
	}
	frb_ring_write(ring, PACKET3(PACKET3_ME_INITIALIZE, 5));
	frb_ring_write(ring, 0x1);
	frb_ring_write(ring, 0x0);
	frb_ring_write(ring, g->config.max_hw_contexts - 1);
	frb_ring_write(ring, PACKET3_ME_INITIALIZE_DEVICE_ID(1));
	frb_ring_write(ring, 0);
	frb_ring_write(ring, 0);

	frb_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	frb_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	frb_ring_write(ring, 0xc000);
	frb_ring_write(ring, 0xe000);
	frb_ring_commit(ring);

	gfx_v6_0_cp_gfx_enable(adev, 1);

	r = frb_ring_alloc(ring, gfx_v6_0_get_csb_size(adev) + 10);
	if (r) {
		frb_err(adev, "gfx: cp failed to lock ring (%d).", r);
		return r;
	}

	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	for (sect = g->rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				frb_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				frb_ring_write(ring, ext->reg_index - PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					frb_ring_write(ring, ext->extent[i]);
			}
		}
	}

	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_END_CLEAR_STATE);

	frb_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
	frb_ring_write(ring, 0);

	frb_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 2));
	frb_ring_write(ring, 0x00000316);
	frb_ring_write(ring, 0x0000000e);
	frb_ring_write(ring, 0x00000010);

	frb_ring_commit(ring);
	return 0;
}

static int gfx_v6_0_ring_test_ring(struct frb_ring *ring);

/* ilog2 of a power of two, amdgpu's order_base_2 for the sizes here */
static uint32_t order_base_2(uint32_t v)
{
	uint32_t n = 0;

	while ((1u << n) < v)
		n++;
	return n;
}

static int gfx_v6_0_cp_gfx_resume(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	struct frb_ring *ring;
	uint32_t tmp;
	uint32_t rb_bufsz;
	uint64_t rptr_addr;
	int r;

	WREG32(mmCP_SEM_WAIT_TIMER, 0x0);
	WREG32(mmCP_SEM_INCOMPLETE_TIMER_CNTL, 0x0);

	/* Set the write pointer delay */
	WREG32(mmCP_RB_WPTR_DELAY, 0);

	WREG32(mmCP_DEBUG, 0);
	WREG32(mmSCRATCH_ADDR, 0);

	/* ring 0 - compute and gfx */
	/* Set ring buffer size */
	ring = &g->gfx_ring;
	rb_bufsz = order_base_2(ring->ring_size / 8);
	tmp = (order_base_2(4096 / 8) << 8) | rb_bufsz;
	/*
	 * No BUF_SWAP_32BIT (amdgpu and RadeonNI set it under __BIG_ENDIAN):
	 * the ring is little-endian, as the CP reads it natively.
	 */
	WREG32(mmCP_RB0_CNTL, tmp);

	/* Initialize the ring buffer's read and write pointers */
	WREG32(mmCP_RB0_CNTL, tmp | CP_RB0_CNTL__RB_RPTR_WR_ENA_MASK);
	ring->wptr = 0;
	WREG32(mmCP_RB0_WPTR, (uint32_t)ring->wptr);

	/* set the wb address whether it's enabled or not */
	rptr_addr = ring->rptr_gpu_addr;
	WREG32(mmCP_RB0_RPTR_ADDR, lower_32_bits(rptr_addr));
	WREG32(mmCP_RB0_RPTR_ADDR_HI, upper_32_bits(rptr_addr) & 0xFF);

	WREG32(mmSCRATCH_UMSK, 0);

	mdelay(1);
	WREG32(mmCP_RB0_CNTL, tmp);

	WREG32(mmCP_RB0_BASE, (uint32_t)(ring->gpu_addr >> 8));

	/* start the rings */
	g->hw_started = 1;
	gfx_v6_0_cp_gfx_start(adev);
	/* amdgpu_ring_test_helper */
	r = gfx_v6_0_ring_test_ring(ring);
	ring->ready = !r;
	return r;
}

/* gfx_v6_0_cp_resume, the gfx ring (no compute rings yet) */
static int gfx_v6_0_cp_resume(struct frb_device *adev)
{
	int r;

	gfx_v6_0_enable_gui_idle_interrupt(adev, 0);

	r = gfx_v6_0_cp_gfx_load_microcode(adev);
	if (r)
		return r;

	r = gfx_v6_0_cp_gfx_resume(adev);
	if (r)
		return r;
	/* gfx_v6_0_cp_compute_resume: CP_RB1/RB2 not set up (not ported) */

	gfx_v6_0_enable_gui_idle_interrupt(adev, 1);
	return 0;
}

/* --- the ring ------------------------------------------------------------------ */

static uint64_t gfx_v6_0_ring_get_rptr(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->rptr_offs);	/* *ring->rptr_cpu_addr */
}

static uint64_t gfx_v6_0_ring_get_wptr(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	return RREG32(mmCP_RB0_WPTR);
}

static void gfx_v6_0_ring_set_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	WREG32(mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
	(void)RREG32(mmCP_RB0_WPTR);
}

static void gfx_v6_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr,
				     uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;
	int int_sel = flags & FRB_FENCE_FLAG_INT;
	/* flush read cache over gart */
	frb_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
	frb_ring_write(ring, (mmCP_COHER_CNTL2 - PACKET3_SET_CONFIG_REG_START));
	frb_ring_write(ring, 0);
	frb_ring_write(ring, PACKET3(PACKET3_SURFACE_SYNC, 3));
	frb_ring_write(ring, PACKET3_TCL1_ACTION_ENA |
			  PACKET3_TC_ACTION_ENA |
			  PACKET3_SH_KCACHE_ACTION_ENA |
			  PACKET3_SH_ICACHE_ACTION_ENA);
	frb_ring_write(ring, 0xFFFFFFFF);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, 10); /* poll interval */
	/* EVENT_WRITE_EOP - flush caches, send int */
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE_EOP, 4));
	frb_ring_write(ring, EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5));
	frb_ring_write(ring, (uint32_t)(addr & 0xfffffffc));
	frb_ring_write(ring, (upper_32_bits(addr) & 0xffff) |
				((write64bit ? 2 : 1) << CP_EOP_DONE_DATA_CNTL__DATA_SEL__SHIFT) |
				((int_sel ? 2 : 0) << CP_EOP_DONE_DATA_CNTL__INT_SEL__SHIFT));
	frb_ring_write(ring, lower_32_bits(seq));
	frb_ring_write(ring, upper_32_bits(seq));
}

static void gfx_v6_0_ring_emit_ib(struct frb_ring *ring, const struct frb_ib *ib,
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
	 * little-endian (frbctl test gfx ibprobe measures the field on SI).
	 */
	frb_ring_write(ring, (uint32_t)(ib->gpu_addr & 0xFFFFFFFC));
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr) & 0xFFFF);
	frb_ring_write(ring, control);
}

static void gfx_v6_0_ring_emit_pipeline_sync(struct frb_ring *ring)
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
		/* synce CE with ME to prevent CE fetch CEIB before context switch done */
		frb_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
		frb_ring_write(ring, 0);
		frb_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
		frb_ring_write(ring, 0);
	}
}

/* emit_pipeline_sync's wait with another address and compare: another engine's fence */
static void gfx_v6_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
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

/* si_flush_hdp / si_invalidate_hdp take this on a ring (frb_ib_schedule) */
static void gfx_v6_0_ring_emit_wreg(struct frb_ring *ring, uint32_t reg, uint32_t val)
{
	int usepfp = 1;	/* AMDGPU_RING_TYPE_GFX */

	frb_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	frb_ring_write(ring, (WRITE_DATA_ENGINE_SEL(usepfp) |
				 WRITE_DATA_DST_SEL(0)));
	frb_ring_write(ring, reg);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, val);
}

static const struct frb_ring_funcs gfx_v6_0_ring_funcs_gfx = {
	.align_mask = 0xff,
	.nop = 0x80000000,
	.support_64bit_ptrs = 0,
	.get_rptr = gfx_v6_0_ring_get_rptr,
	.get_wptr = gfx_v6_0_ring_get_wptr,
	.set_wptr = gfx_v6_0_ring_set_wptr_gfx,
	.emit_ib = gfx_v6_0_ring_emit_ib,
	.emit_fence = gfx_v6_0_ring_emit_fence,
	.emit_hdp_flush = NULL,			/* si_flush_hdp: emit_wreg */
	.insert_nop = NULL,			/* amdgpu_ring_insert_nop */
	.pad_ib = frb_ring_generic_pad_ib,	/* amdgpu_ring_generic_pad_ib */
	.emit_wreg = gfx_v6_0_ring_emit_wreg,
	.emit_pipeline_sync = gfx_v6_0_ring_emit_pipeline_sync,
	.emit_wait_mem = gfx_v6_0_ring_emit_wait_mem,
};

/* --- the tests ---------------------------------------------------------------- */

static int gfx_v6_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);

	r = frb_ring_alloc(ring, 3);
	if (r)
		return r;

	frb_ring_write(ring, PACKET3(PACKET3_SET_CONFIG_REG, 1));
	frb_ring_write(ring, mmSCRATCH_REG0 - PACKET3_SET_CONFIG_REG_START);
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
		frb_err(adev, "gfx: ring test FAILED: SCRATCH_REG0 %08x after %u polls (wptr %u, rptr %u, "
			"CP_RB0_RPTR %08x, GRBM_STATUS %08x, CP_STAT %08x, CP_ME_CNTL %08x)", tmp, i,
			(uint32_t)ring->wptr, (uint32_t)gfx_v6_0_ring_get_rptr(ring), RREG32(mmCP_RB0_RPTR),
			RREG32(mmGRBM_STATUS), RREG32(mmCP_STAT), RREG32(mmCP_ME_CNTL));
	else
		frb_info(adev, "gfx: ring test passed: SCRATCH_REG0 deadbeef after %u polls (wptr %u)",
			 i, (uint32_t)ring->wptr);
	return r;
}

/* gfx_v6_0_ring_test_ib: a SET_CONFIG_REG of SCRATCH_REG0 from an IB, then the fence */
static int gfx_v6_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t tmp = 0, seq = 0;
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);
	r = frb_ib_get(adev, 256, &ib);
	if (r)
		return r;

	frb_ib_write(adev, &ib, PACKET3(PACKET3_SET_CONFIG_REG, 1));
	frb_ib_write(adev, &ib, mmSCRATCH_REG0 - PACKET3_SET_CONFIG_REG_START);
	frb_ib_write(adev, &ib, 0xDEADBEEF);
	ib.length_dw = 3;

	r = frb_ib_schedule(ring, &ib, &seq);
	if (r)
		goto error;

	r = frb_fence_wait(ring, seq, timeout_us);
	if (r) {
		frb_err(adev, "gfx: IB test FAILED: fence %u not signalled (reads %u) in %u us; rptr %u wptr %u, "
			"SCRATCH_REG0 %08x, GRBM_STATUS %08x, CP_STAT %08x", seq, frb_fence_read(ring), timeout_us,
			(uint32_t)ring->funcs->get_rptr(ring), (uint32_t)ring->funcs->get_wptr(ring),
			RREG32(mmSCRATCH_REG0), RREG32(mmGRBM_STATUS), RREG32(mmCP_STAT));
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
 * field, the IB in either order (host k > 1: only the first k - 1 dwords in
 * the CPU's order). The IB sets SCRATCH_REG0, so no memory write is
 * involved: 0xDEADBEEF ran as written, 0xEFBEADDE ran with the payload's
 * bytes as they stood, 0xCAFEDEAD did not run. RadeonNI ran host-order IBs
 * with swap field 2 on this card (and a host-order ring with BUF_SWAP).
 */
static int gfx_v6_0_ib_probe_one(struct frb_ring *ring, uint32_t bits, int host, uint32_t *got)
{
	struct frb_device *adev = ring->adev;
	struct frb_ib ib;
	uint32_t seq, i, w[3];
	int r;

	WREG32(mmSCRATCH_REG0, 0xCAFEDEAD);
	r = frb_ib_get(adev, 64, &ib);
	if (r)
		return r;
	w[0] = PACKET3(PACKET3_SET_CONFIG_REG, 1);
	w[1] = mmSCRATCH_REG0 - PACKET3_SET_CONFIG_REG_START;
	w[2] = 0xDEADBEEF;
	for (i = 0; i < 3; i++)
		frb_vram_write32(adev, ib.bar_offset + 4 * i,
				 (host == 1 || (host > 1 && i < (uint32_t)host - 1)) ? frb_swap_le32(w[i]) : w[i]);
	r = frb_ring_alloc(ring, 64);
	if (r)
		goto out;
	gfx_v6_0_ring_emit_wreg(ring, mmHDP_MEM_COHERENCY_FLUSH_CNTL, 1);
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

static int gfx_v6_0_ib_probe(struct frb_ring *ring)
{
	static const struct { uint32_t bits; int host; } v[] = {
		{ 0, 0 },	/* the kernel's IBs */
		{ 2, 1 },	/* amdgpu's and RadeonNI's big-endian IB arm */
		{ 1, 1 },	/* the other encodings of the field */
		{ 3, 1 },
		{ 2, 0 },	/* the field on a little-endian IB */
		{ 1, 0 },
		{ 3, 0 },
		{ 2, 2 },	/* the header swapped, the rest raw? */
	};
	struct frb_device *adev = ring->adev;
	uint32_t got = 0, i, ran = 0;
	int r = 0;

	for (i = 0; i < ARRAY_SIZE(v) && !r; i++) {
		r = gfx_v6_0_ib_probe_one(ring, v[i].bits, v[i].host, &got);
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
		frb_err(adev, "gfx: ib probe: the CP stopped at variant %u (GRBM_STATUS %08x CP_STAT %08x): reboot",
			i - 1, RREG32(mmGRBM_STATUS), RREG32(mmCP_STAT));
		return r;
	}
	frb_info(adev, "gfx: ib probe: variants that ran: mask %03x", ran);
	return 0;
}

/* --- the stage, the tests, the teardown ------------------------------------------ */

/* gfx_v6_0_sw_init: the microcode, the RLC objects, the gfx ring; no IRQ sources, no compute rings */
static int gfx_v6_0_sw_init(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	int r;

	r = gfx_v6_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to load gfx firmware!");
		return r;
	}
	r = gfx_v6_0_rlc_init(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to init rlc BOs!");
		return r;
	}
	r = frb_ring_init(adev, &g->gfx_ring, "gfx", &gfx_v6_0_ring_funcs_gfx, 2048);
	if (r)
		return r;
	frb_ring_clear(&g->gfx_ring);	/* amdgpu_ring_init clears a new ring */
	g->sw_done = 1;
	return 0;
}

static int gfx_v6_0_hw_init(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	int r;

	gfx_v6_0_constants_init(adev);
	/* the RLC's clear-state block with the raster config constants_init set (gfx_v6_0_rlc_init) */
	gfx_v6_0_get_csb_buffer(adev, g->rlc.clear_state_offset + 256);
	frb_flush_hdp(adev);

	r = gfx_v6_0_rlc_resume(adev);
	if (r)
		return r;

	r = gfx_v6_0_cp_resume(adev);
	if (r)
		return r;

	g->ce_ram_size = 0x8000;
	return r;
}

static void gfx_v6_0_hw_fini(struct frb_device *adev)
{
	gfx_v6_0_cp_gfx_enable(adev, 0);
	gfx_v6_0_rlc_stop(adev);
	gfx_v6_0_fini_pg(adev);
}

/* Stage gfx on SI: gfx_v6_0_sw_init, then gfx_v6_0_hw_init. */
int frb_gfx_v6_0_init(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;
	int r;

	if (!adev->aper || !adev->layout.objects_size) {
		frb_err(adev, "gfx: no VRAM window or objects region (stage gmc)");
		return -ENODEV;
	}
	if (!g) {
		g = (struct frb_gfx6 *)adev->os->alloc(adev->os->cookie, sizeof(*g));
		if (!g)
			return -ENOMEM;
		memset(g, 0, sizeof(*g));
		adev->gfx6 = g;
	}
	if (!g->sw_done) {
		r = gfx_v6_0_sw_init(adev);
		if (r)
			return r;
	}
	r = gfx_v6_0_hw_init(adev);
	frb_info(adev, "gfx: %u SE x %u SH, %u CUs (always-on mask %08x, bitmaps %x %x), %u RBs (mask %x), "
		 "raster config %08x, gb_addr_config %08x; ring %u KB at %llx, CSB %u dwords at %llx",
		 g->config.max_shader_engines, g->config.max_sh_per_se, g->cu_info.number,
		 g->cu_info.ao_cu_mask, g->cu_info.bitmap[0][0][0], g->cu_info.bitmap[0][0][1],
		 g->config.num_rbs, g->config.backend_enable_mask, g->config.rb_config[0][0].raster_config,
		 g->config.gb_addr_config, g->gfx_ring.ring_size >> 10,
		 (unsigned long long)g->gfx_ring.gpu_addr, g->rlc.clear_state_size,
		 (unsigned long long)g->rlc.clear_state_gpu_addr);
	if (r)
		frb_err(adev, "gfx: hw_init failed (%d)", r);
	return r;
}

void frb_gfx_v6_0_fini(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;

	if (!g)
		return;
	if (g->hw_started) {
		g->gfx_ring.ready = 0;
		gfx_v6_0_hw_fini(adev);
		frb_info(adev, "gfx: hw_fini: CP_ME_CNTL %08x, RLC_CNTL %08x", RREG32(mmCP_ME_CNTL),
			 RREG32(mmRLC_CNTL));
	}
	adev->os->free(adev->os->cookie, g);
	adev->gfx6 = NULL;
}

int frb_gfx_v6_0_test(struct frb_device *adev, int test)
{
	struct frb_gfx6 *g = adev->gfx6;
	int r;

	if (!g || !g->hw_started)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		r = gfx_v6_0_ring_test_ring(&g->gfx_ring);
		g->gfx_ring.ready = !r;	/* amdgpu_ring_test_helper */
		return r;
	case FRB_TEST_IB:
		return gfx_v6_0_ring_test_ib(&g->gfx_ring, FRB_FENCE_TIMEOUT_US);
	case FRB_TEST_IB_PROBE:
		return gfx_v6_0_ib_probe(&g->gfx_ring);
	default:
		return -ENOSYS;
	}
}

/* --- for the accelerator ---------------------------------------------------------- */

struct frb_ring *frb_gfx_v6_0_ring(struct frb_device *adev)
{
	struct frb_gfx6 *g = adev->gfx6;

	return g && g->hw_started && g->gfx_ring.ready ? &g->gfx_ring : NULL;
}

int frb_gfx_v6_0_get_info(struct frb_device *adev, struct frb_gfx_info *info)
{
	struct frb_gfx6 *g = adev->gfx6;

	memset(info, 0, sizeof(*info));
	if (!g || !g->sw_done)
		return -ENODEV;
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
 * si_get_register_value's cached arms: what gfx_v6_0 keeps (the SE/SH
 * indexed RB registers, the tiling table, the address configuration).
 * 0 and the value, or -ENOENT when this block keeps no copy.
 */
int frb_gfx_v6_0_get_register_value(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				    uint32_t reg_offset, uint32_t *value)
{
	struct frb_gfx6 *g = adev->gfx6;
	unsigned se_idx = (se_num == 0xffffffff) ? 0 : se_num;
	unsigned sh_idx = (sh_num == 0xffffffff) ? 0 : sh_num;

	if (!g || !g->sw_done || !g->hw_started)
		return -ENOENT;
	if (se_idx >= GFX6_MAX_SE || sh_idx >= GFX6_MAX_SH_PER_SE)
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
	return -ENOENT;
}

/* gfx_v6_0_set_clockgating_state (amdgpu's late init: GATE; hw_fini's ungate) */
void frb_gfx_v6_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	gfx_v6_0_enable_gui_idle_interrupt(adev, 0);
	if (gate) {
		gfx_v6_0_enable_mgcg(adev, 1);
		gfx_v6_0_enable_cgcg(adev, 1);
	} else {
		gfx_v6_0_enable_cgcg(adev, 0);
		gfx_v6_0_enable_mgcg(adev, 0);
	}
	gfx_v6_0_enable_gui_idle_interrupt(adev, 1);
}
