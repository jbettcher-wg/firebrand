/*
 * gfx_v9_0.c (Linux 7.1.1, the kernel of the Vega 20 capture), for GC 9.4.0
 * (Vega 20, the MI50): the GFX 9 block's sw_init and hw_init up to the gfx
 * ring test, the IB test, and hw_fini's halt.
 *
 * What is ported, in amdgpu's order:
 *   sw_init:  gfx_v9_0_init_microcode's RLC header (is_rlc_v2_1),
 *             gfx_v9_0_rlc_init (the clear-state block), the gfx ring
 *             (amdgpu_ring_init, doorbell gfx_ring0 << 1),
 *             gfx_v9_0_gpu_early_init (GB_ADDR_CONFIG, the atomfirmware
 *             gfx_info 2.4 table)
 *   hw_init:  gfx_v9_0_init_golden_registers (gc_9_0, gc_9_0_vg20,
 *             gc_9_x_common), gfx_v9_0_constants_init (GRBM_CNTL, setup_rb,
 *             get_cu_info, SH_MEM per VMID, compute and GDS VMIDs),
 *             gfx_v9_0_init_tcp_config (nothing on 9.4.0),
 *             gfx_v9_0_rlc_resume on the PSP-loaded path (stop, CG off,
 *             init_pg: the CSB and the save/restore machine, 9.4's load
 *             balancing per CU, the SPM VMID, start),
 *             gfx_v9_0_cp_resume's gfx part (cp_gfx_enable(false),
 *             cp_compute_enable(false), cp_gfx_resume with the doorbell,
 *             cp_gfx_start with the clear-state packets, the gfx ring test,
 *             the GUI idle interrupt)
 *   late:     gfx_v9_0_ring_test_ib (amdgpu_ib_ring_tests runs it from
 *             late_init; here frbctl test gfx ib, as on Polaris)
 *   hw_fini:  cp_enable(false) (CP and MEC halted), RLC stop
 * The CP, RLC and MEC microcode is the PSP's (stage smu, LOAD_IP_FW), as on
 * Linux for Vega 20: no cp_gfx_load_microcode / rlc_load_microcode here.
 *
 * NOT PORTED, on purpose:
 * - THE KIQ AND THE COMPUTE QUEUES. Linux's cp_resume programs the KIQ's
 *   HQD (kiq_resume: RLC_CP_SCHEDULERS, the MQD, CP_HQD_*), unhalts the MEC
 *   (kcq_resume: CP_MEC_CNTL = 0), initialises eight compute MQDs, maps
 *   them through the KIQ ring (MAP_QUEUES, amdgpu_gfx_enable_kcq) and tests
 *   them. None of it is on the gfx ring's path: the gfx ring, its IB test,
 *   the 2D plug-in and Quartz Extreme run on the gfx CP (ME/PFP/CE), and the
 *   compute queues serve amdkfd and amdgpu's compute rings, which Firebrand
 *   has no client for. Carrying it would be the MQD/EOP buffers, the KIQ
 *   ring and its PM4 (MAP_QUEUES, SET_RESOURCES), and the HQD state machine,
 *   all untestable without a compute client. So the MEC is left halted as
 *   cp_compute_enable(false) leaves it (Linux's own write), and the replay
 *   passes over the KIQ/KCQ windows (tests/Makefile, replay-v20-gfx).
 * - Interrupts (IH: EOP, privileged register/instruction, ECC sources), RAS,
 *   clock and power gating (late_init's set_clockgating_state; Firebrand
 *   carries no AMD_CG/PG flags on SOC15, frb_soc15_common_early_init),
 *   GFXOFF, mid-command-buffer preemption (gfx.mcbp: off on bare metal), the
 *   software rings and the cleaner shader (only emitted per job), SR-IOV,
 *   soft reset/recovery (frbctl recover refuses on SOC15).
 *
 * Where Firebrand differs from amdgpu, and why:
 * - PLACEMENT. The ring, its writeback slots and the IB pool live in VRAM
 *   (frb_ring.c's objects region, -14 MB..-10 MB of the 256 MB window), not
 *   GTT; Linux's CSB was in VRAM too. CP_RB0_BASE(_HI), CP_RB0_RPTR_ADDR(_HI),
 *   CP_RB_WPTR_POLL_ADDR_LO/HI and RLC_CSIB_ADDR_HI/LO carry other addresses
 *   (the replay matches them by register only). Every structure the CPU
 *   fills through BAR0 for the GPU is followed by an HDP flush (frb_ring.h),
 *   the WPTR writeback included, before the doorbell.
 * - BYTE ORDER. Ring, IB, CSB and writeback are little-endian dwords
 *   (frb_ring_write, frb_ib_write, frb_vram_write32), so the CP swaps
 *   nothing: CP_RB0_CNTL.BUF_SWAP and the IB packet's swap bits stay 0
 *   where Linux sets them under __BIG_ENDIAN, and the register values are the
 *   little-endian capture's (docs/byte-order.md). The doorbell's value goes
 *   out little-endian (os->doorbell_write64).
 * - The IB frame is Linux's for a kernel IB (no job) minus COND_EXEC (always
 *   "continue" on a kernel ring; it exists for preemption): HDP flush packet,
 *   the IB, the HDP invalidate as a WRITE_DATA to HDP_READ_CACHE_INVALIDATE,
 *   RELEASE_MEM.
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
#include <stddef.h>
#include "../shared/atom/atomfirmware-frb.h"	/* before atom.h (frb_device.h), as frb_atomfirmware.c */
#include "frb_ring.h"
#include "frb_soc15.h"
#include "frb_ucode.h"
#include "include/soc15d.h"
#include "include/clearstate_defs.h"
#include "include/clearstate_gfx9.h"
#include "include/asic_reg/gc/gc_9_0_offset.h"
#include "include/asic_reg/gc/gc_9_0_sh_mask.h"
#include "include/asic_reg/nbio/nbio_7_4_offset.h"
#include "include/asic_reg/nbio/nbio_7_4_sh_mask.h"

/* atomfirmware.h's get_index_into_master_table */
#define FW_DATA_INDEX(member) \
	((int)(offsetof(struct atom_master_list_of_data_tables_v2_1, member) / sizeof(uint16_t)))

/* soc15_common.h, the RLC-path names (bare metal: plain register access) */
#define WREG32_FIELD15_RLC(ip, idx, reg, field, val)	WREG32_FIELD15(ip, idx, reg, field, val)

/* vega10_enum.h, the values this block uses */
#define SH_MEM_ADDRESS_MODE_64			0x00000000
#define SH_MEM_ALIGNMENT_MODE_UNALIGNED		0x00000003
#define CS_PARTIAL_FLUSH			0x00000007
#define VS_PARTIAL_FLUSH			0x0000000f
#define PS_PARTIAL_FLUSH			0x00000010
#define CACHE_FLUSH_AND_INV_TS_EVENT		0x00000014
#define NUM_SIMD_PER_CU				0x00000004

/* include/uapi/drm/amdgpu_drm.h */
#define AMDGPU_IB_FLAG_CE	(1 << 0)
/* amdgpu_ids.h */
#define AMDGPU_NUM_VMID		16
/* amdgpu_doorbell.h AMDGPU_VEGA20_DOORBELL_GFX_RING0 (vega20_doorbell_index_init's gfx_ring0) */
#define AMDGPU_VEGA20_DOORBELL_GFX_RING0	0x08B
/* amdgpu_gfx.h */
#define AMDGPU_GFX_MAX_SE		4
#define AMDGPU_GFX_MAX_SH_PER_SE	2

/*
 * What other blocks set up and this one reads, with where amdgpu sets it:
 * - gmc_v9_0_sw_init: first_kfd_vmid 8 (not 9.4.1/9.4.2/9.4.3), so the gfx
 *   hub's id manager has 8 ids; shared/private apertures at
 *   0x2000/0x1000 << 48 (amdgpu_gmc_noretry_set: noretry on for 9.4.0:
 *   adev->gmc.noretry, frb_gmc_v9_0.c).
 * - amdgpu_gfx_parse_disable_cu: the disable_cu parameter is unset, so every
 *   mask is 0.
 * - amdgpu_lbpw is -1 (auto): gfx_v9_0_enable_lbpw(false) on 9.4.0.
 */
#define FRB_FIRST_KFD_VMID		8
#define FRB_GFXHUB_NUM_IDS		FRB_FIRST_KFD_VMID
#define FRB_SHARED_APERTURE_START	0x2000000000000000ULL
#define FRB_PRIVATE_APERTURE_START	0x1000000000000000ULL

/* amdgpu_gfx.h, the parts this block keeps */
struct frb_gfx9_config {
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
	uint32_t gs_vgt_table_depth;
	uint32_t gs_prim_buffer_depth;
	uint32_t double_offchip_lds_buf;
	uint32_t backend_enable_mask;
	uint32_t num_rbs;
	uint32_t gb_addr_config;
	uint32_t db_debug2;
	struct {
		uint32_t num_pipes, num_banks, max_compress_frags, num_rb_per_se, num_se, pipe_interleave_size;
	} gb_addr_config_fields;
};

struct frb_gfx9_cu_info {
	uint32_t number;
	uint32_t ao_cu_mask;
	uint32_t simd_per_cu;
	uint32_t max_waves_per_simd;
	uint32_t wave_front_size;
	uint32_t max_scratch_slots_per_cu;
	uint32_t lds_size;
	uint32_t bitmap[4][4];
	uint32_t ao_cu_bitmap[4][4];
};

/* amdgpu_rlc.h, the fields gfx_v9_0 uses on Vega 20 */
struct frb_gfx9_rlc {
	const struct cs_section_def *cs_data;
	uint32_t clear_state_size;		/* dwords */
	uint32_t clear_state_offset;		/* the CSB, as a BAR0 (VRAM) offset */
	uint64_t clear_state_gpu_addr;
	uint32_t fw_version, feature_version;
	int is_rlc_v2_1;
	int in_safe_mode;			/* amdgpu_gfx_rlc_enter/exit_safe_mode */
};

/* adev->gfx, the GC 9.4.0 subset */
struct frb_gfx9 {
	struct frb_gfx9_config config;
	struct frb_gfx9_cu_info cu_info;
	struct frb_gfx9_rlc rlc;
	struct frb_ring gfx_ring;
	uint32_t me_fw_version, pfp_fw_version, ce_fw_version, mec_fw_version;
	uint32_t cp_fw_feature[4];	/* ME, PFP, CE, MEC: ucode_feature_version (Mesa's me_fw_feature ...) */
	int sw_done;		/* sw_init completed */
	int hw_started;		/* the RLC/CP were touched: hw_fini must halt them */
	uint32_t grbm_soft_reset;	/* check_soft_reset's verdict */
	int regate;		/* pre_soft_reset ungated the block: post_soft_reset gates it again */
};

static const struct soc15_reg_golden golden_settings_gc_9_0[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0xf00fffff, 0x00000400),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG3, 0x80000000, 0x80000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGB_GPU_ID, 0x0000000f, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_BINNER_EVENT_CNTL_3, 0x00000003, 0x82400024),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE, 0x3fffffff, 0x00000001),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_LINE_STIPPLE_STATE, 0x0000ff0f, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSH_MEM_CONFIG, 0x00001000, 0x00001000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_RESOURCE_RESERVE_CU_0, 0x0007ffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_RESOURCE_RESERVE_CU_1, 0x0007ffff, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_RESOURCE_RESERVE_EN_CU_0, 0x01ffffff, 0x00ffff87),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_RESOURCE_RESERVE_EN_CU_1, 0x01ffffff, 0x00ffff8f),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSQC_CONFIG, 0x03000000, 0x020a2000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0xfffffeef, 0x010b0000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTCP_CHAN_STEER_HI, 0xffffffff, 0x4a2c0e68),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTCP_CHAN_STEER_LO, 0xffffffff, 0xb5d3f197),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmVGT_CACHE_INVALIDATION, 0x3fff3af3, 0x19200000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmVGT_GS_MAX_WAVE_ID, 0x00000fff, 0x000003ff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_MEC1_F32_INT_DIS, 0x00000800, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_MEC2_F32_INT_DIS, 0x00000800, 0x00000800),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_DEBUG, 0x00008000, 0x00008000)
};

static const struct soc15_reg_golden golden_settings_gc_9_0_vg20[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_DCC_CONFIG, 0x0f000080, 0x04000080),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_HW_CONTROL_2, 0x0f000000, 0x0a000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCB_HW_CONTROL_3, 0x30000000, 0x10000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGB_ADDR_CONFIG, 0xf3e777ff, 0x22014042),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGB_ADDR_CONFIG_READ, 0xf3e777ff, 0x22014042),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmDB_DEBUG2, 0x00003e00, 0x00000400),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmPA_SC_ENHANCE_1, 0xff840000, 0x04040000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmRMI_UTCL1_CNTL2, 0x00030000, 0x00030000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmSPI_CONFIG_CNTL_1, 0xffff010f, 0x01000107),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTA_CNTL_AUX, 0x000b0000, 0x000b0000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmTD_CNTL, 0x01000000, 0x01000000)
};

static const struct soc15_reg_golden golden_settings_gc_9_x_common[] =
{
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmCP_SD_CNTL, 0xffffffff, 0x000001ff),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_CAM_INDEX, 0xffffffff, 0x00000000),
	SOC15_REG_GOLDEN_VALUE(GC, 0, mmGRBM_CAM_DATA, 0xffffffff, 0x2544c382)
};

static const struct frb_ring_funcs gfx_v9_0_ring_funcs_gfx;

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

/* soc15.c soc15_grbm_select (the caller holds the driver's lock: Linux's srbm_mutex) */
static void soc15_grbm_select(struct frb_device *adev, uint32_t me, uint32_t pipe, uint32_t queue,
			      uint32_t vmid, int xcc_id)
{
	uint32_t grbm_gfx_cntl = 0;

	(void)xcc_id;
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, PIPEID, pipe);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, MEID, me);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, VMID, vmid);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, QUEUEID, queue);

	WREG32_SOC15(GC, 0, mmGRBM_GFX_CNTL, grbm_gfx_cntl);
}

/* gfx_v9_0_select_se_sh (amdgpu_gfx_select_se_sh) */
static void gfx_v9_0_select_se_sh(struct frb_device *adev, uint32_t se_num, uint32_t sh_num,
				  uint32_t instance, int xcc_id)
{
	uint32_t data;

	(void)xcc_id;
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

	WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, data);
}

/*
 * amdgpu_atomfirmware_get_gfx_info, the gfx_info 2.4 arm (Vega 20's VBIOS):
 * byte and 16-bit fields at their offsetof() places in struct
 * atom_gfx_info_v2_4. Here because only this block reads it.
 */
static int amdgpu_atomfirmware_get_gfx_info(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	struct frb_gfx9_config *cfg = &g->config;
	uint8_t frev, crev;
	uint16_t off;
	const uint8_t *b = adev->bios;

	if (!frb_atomfirmware_data_table(adev, FW_DATA_INDEX(gfx_info), &frev, &crev, &off)) {
		frb_err(adev, "gfx: the VBIOS has no gfx_info table");
		return -EINVAL;
	}
	if (frev != 2 || crev != 4) {
		frb_err(adev, "gfx: gfx_info %u.%u; only 2.4 (Vega 20's) is carried", frev, crev);
		return -EINVAL;
	}
#define GI8(f)	frb_get_u8(b, off + offsetof(struct atom_gfx_info_v2_4, f))
#define GI16(f)	frb_get_le16(b, off + offsetof(struct atom_gfx_info_v2_4, f))
	cfg->max_shader_engines = GI8(max_shader_engines);
	cfg->max_cu_per_sh = GI8(max_cu_per_sh);
	cfg->max_sh_per_se = GI8(max_sh_per_se);
	cfg->max_backends_per_se = GI8(max_backends_per_se);
	cfg->max_texture_channel_caches = GI8(max_texture_channel_caches);
	cfg->max_gprs = GI16(gc_num_gprs);
	cfg->max_gs_threads = GI8(gc_num_max_gs_thds);
	cfg->gs_vgt_table_depth = GI8(gc_gs_table_depth);
	cfg->gs_prim_buffer_depth = GI16(gc_gsprim_buff_depth);
	cfg->double_offchip_lds_buf = GI8(gc_double_offchip_lds_buffer);
	g->cu_info.wave_front_size = GI16(gc_wave_size);
	g->cu_info.max_waves_per_simd = GI16(gc_max_waves_per_simd);
	g->cu_info.max_scratch_slots_per_cu = GI8(gc_max_scratch_slots_per_cu);
	g->cu_info.lds_size = GI16(gc_lds_size);
#undef GI8
#undef GI16
	frb_info(adev, "gfx: gfx_info 2.4: %u SE, %u SH/SE, %u CU/SH, %u RB/SE, %u TCCs, %u GPRs, wave %u, "
		 "LDS %u KB", cfg->max_shader_engines, cfg->max_sh_per_se, cfg->max_cu_per_sh,
		 cfg->max_backends_per_se, cfg->max_texture_channel_caches, cfg->max_gprs,
		 g->cu_info.wave_front_size, g->cu_info.lds_size);
	if (!cfg->max_shader_engines || !cfg->max_sh_per_se ||
	    cfg->max_shader_engines * cfg->max_sh_per_se > 16 || cfg->max_cu_per_sh > 16 ||
	    !cfg->max_backends_per_se || cfg->max_backends_per_se < cfg->max_sh_per_se)
		return -EINVAL;
	return 0;
}

static void gfx_v9_0_init_golden_registers(struct frb_device *adev)
{
	/* IP_VERSION(9, 4, 0) */
	frb_soc15_program_register_sequence(adev, golden_settings_gc_9_0, ARRAY_SIZE(golden_settings_gc_9_0));
	frb_soc15_program_register_sequence(adev, golden_settings_gc_9_0_vg20,
					    ARRAY_SIZE(golden_settings_gc_9_0_vg20));
	/* not 9.4.1 / 9.4.2 */
	frb_soc15_program_register_sequence(adev, golden_settings_gc_9_x_common,
					    ARRAY_SIZE(golden_settings_gc_9_x_common));
}

static int gfx_v9_0_ring_test_ring(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t scratch = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0);
	uint32_t tmp = 0;
	unsigned i;
	int r;

	WREG32(scratch, 0xCAFEDEAD);
	r = frb_ring_alloc(ring, 3);
	if (r)
		return r;

	frb_ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	frb_ring_write(ring, scratch - PACKET3_SET_UCONFIG_REG_START);
	frb_ring_write(ring, 0xDEADBEEF);
	frb_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(scratch);
		if (tmp == 0xDEADBEEF)
			break;
		udelay(1);
	}

	if (i >= adev->usec_timeout)
		r = -ETIMEDOUT;

	if (r)
		frb_err(adev, "gfx: ring test FAILED: SCRATCH_REG0 %08x after %u polls (wptr %u, rptr %u, "
			"CP_RB0_RPTR %08x, GRBM_STATUS %08x, CP_STAT %08x, CP_ME_CNTL %08x)", tmp, i,
			(uint32_t)ring->wptr, (uint32_t)ring->funcs->get_rptr(ring),
			RREG32_SOC15(GC, 0, mmCP_RB0_RPTR), RREG32_SOC15(GC, 0, mmGRBM_STATUS),
			RREG32_SOC15(GC, 0, mmCP_STAT), RREG32_SOC15(GC, 0, mmCP_ME_CNTL));
	else
		frb_info(adev, "gfx: ring test passed: SCRATCH_REG0 deadbeef after %u polls (wptr %u, %s)",
			 i, (uint32_t)ring->wptr, ring->use_doorbell ? "doorbell" : "MMIO WPTR");
	return r;
}

static int gfx_v9_0_ring_test_ib(struct frb_ring *ring, uint32_t timeout_us)
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
		/* the IB stays allocated: the CP may still fetch it */
		frb_err(adev, "gfx: IB test FAILED: fence %u not signalled (reads %u) in %u us; "
			"rptr %u wptr %u, writeback %08x, GRBM_STATUS %08x, CP_STAT %08x",
			seq, frb_fence_read(ring), timeout_us, (uint32_t)ring->funcs->get_rptr(ring),
			(uint32_t)ring->funcs->get_wptr(ring), frb_wb_read(adev, index),
			RREG32_SOC15(GC, 0, mmGRBM_STATUS), RREG32_SOC15(GC, 0, mmCP_STAT));
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
	frb_ib_free(adev, &ib);
err1:
	frb_wb_free(adev, index);
	return r;
}

/*
 * gfx_v9_0_init_microcode, the part the PSP-loaded path keeps: the RLC
 * header's version (amdgpu_gfx_rlc_init_microcode: v2.1 means the
 * save/restore lists are in the file, which the PSP took in stage smu, and
 * gfx_v9_0_init_pg turns the save/restore machine on), and the CP images'
 * versions for the log.
 */
static int gfx_v9_0_init_microcode(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	static const char *const cp[4] = { "_me.bin", "_pfp.bin", "_ce.bin", "_mec.bin" };
	uint32_t *ver[4];
	struct frb_fw fw;
	struct frb_ucode u;
	char name[32];
	int err, i;

	ver[0] = &g->me_fw_version;
	ver[1] = &g->pfp_fw_version;
	ver[2] = &g->ce_fw_version;
	ver[3] = &g->mec_fw_version;
	frb_fw_name(name, sizeof(name), adev->row ? adev->row->fw_prefix : "vega20", "_rlc.bin");
	err = adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size);
	if (err) {
		frb_err(adev, "gfx: firmware %s missing (%d)", name, err);
		return err;
	}
	err = frb_ucode_parse(adev, name, &fw, &u);
	if (err)
		return err;
	if (u.header_major != 2) {
		frb_err(adev, "gfx: %s: header v%u.%u; rlc_firmware_header_v2_x expected", name,
			u.header_major, u.header_minor);
		return -EINVAL;
	}
	g->rlc.fw_version = u.ucode_version;
	g->rlc.feature_version = frb_ucode_le32(&u, FRB_FWH_RLC_UCODE_FEATURE_VERSION);
	g->rlc.is_rlc_v2_1 = u.header_minor == 1;
	for (i = 0; i < 4; i++) {
		frb_fw_name(name, sizeof(name), adev->row ? adev->row->fw_prefix : "vega20", cp[i]);
		if (adev->os->firmware(adev->os->cookie, name, &fw.data, &fw.size) ||
		    frb_ucode_parse(adev, name, &fw, &u))
			continue;	/* the PSP has them; the version is for the log */
		*ver[i] = u.ucode_version;
		g->cp_fw_feature[i] = frb_ucode_le32(&u, FRB_FWH_GFX_UCODE_FEATURE_VERSION);
	}
	frb_info(adev, "gfx: RLC v2.%u microcode %u (feature %u)%s; ME %u, PFP %u, CE %u, MEC %u (loaded by the PSP)",
		 g->rlc.is_rlc_v2_1 ? 1 : 0, g->rlc.fw_version, g->rlc.feature_version,
		 g->rlc.is_rlc_v2_1 ? ", save/restore lists" : "", g->me_fw_version, g->pfp_fw_version,
		 g->ce_fw_version, g->mec_fw_version);
	return 0;
}

static uint32_t gfx_v9_0_get_csb_size(struct frb_device *adev)
{
	uint32_t count = 0;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;

	(void)adev;
	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = gfx9_cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT)
				count += 2 + ext->reg_count;
			else
				return 0;
		}
	}

	/* end clear state */
	count += 2;
	/* clear state */
	count += 2;

	return count;
}

/*
 * gfx_v9_0_get_csb_buffer (amdgpu_gfx_csb_preamble_start, _data_parser,
 * _preamble_end), written straight into the VRAM clear-state block, one
 * little-endian dword at a time (Linux: cpu_to_le32 into the bo).
 */
static void gfx_v9_0_get_csb_buffer(struct frb_device *adev, uint32_t offset)
{
	struct frb_gfx9 *g = adev->gfx9;
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

/*
 * gfx_v9_0_rlc_init: amdgpu_gfx_rlc_init_csb's clear-state block (VRAM in
 * amdgpu too). No CP table: not an APU.
 */
static int gfx_v9_0_rlc_init(struct frb_device *adev)
{
	struct frb_gfx9_rlc *rlc = &adev->gfx9->rlc;
	int r;

	rlc->cs_data = gfx9_cs_data;

	if (rlc->cs_data) {
		/* init clear state block */
		rlc->clear_state_size = gfx_v9_0_get_csb_size(adev);
		r = frb_vram_obj_alloc(adev, rlc->clear_state_size * 4, AMDGPU_GPU_PAGE_SIZE,
				       &rlc->clear_state_offset);
		if (r) {
			frb_err(adev, "gfx: (%d) failed to create rlc csb bo", r);
			return r;
		}
		rlc->clear_state_gpu_addr = frb_vram_gpu_addr(adev, rlc->clear_state_offset);
	}
	return 0;
}

/* gfx_v9_0_gpu_early_init, the IP_VERSION(9, 4, 0) arm */
static int gfx_v9_0_gpu_early_init(struct frb_device *adev)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	uint32_t gb_addr_config;
	int err;

	cfg->max_hw_contexts = 8;
	cfg->sc_prim_fifo_size_frontend = 0x20;
	cfg->sc_prim_fifo_size_backend = 0x100;
	cfg->sc_hiz_tile_fifo_size = 0x30;
	cfg->sc_earlyz_tile_fifo_size = 0x4C0;
	gb_addr_config = RREG32_SOC15(GC, 0, mmGB_ADDR_CONFIG);
	gb_addr_config &= ~0xf3e777ff;
	gb_addr_config |= 0x22014042;
	/* check vbios table if gpu info is not available */
	err = amdgpu_atomfirmware_get_gfx_info(adev);
	if (err)
		return err;

	cfg->gb_addr_config = gb_addr_config;

	cfg->gb_addr_config_fields.num_pipes = 1 << REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, NUM_PIPES);
	cfg->max_tile_pipes = cfg->gb_addr_config_fields.num_pipes;
	cfg->gb_addr_config_fields.num_banks = 1 << REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, NUM_BANKS);
	cfg->gb_addr_config_fields.max_compress_frags =
		1 << REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, MAX_COMPRESSED_FRAGS);
	cfg->gb_addr_config_fields.num_rb_per_se =
		1 << REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, NUM_RB_PER_SE);
	cfg->gb_addr_config_fields.num_se = 1 << REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, NUM_SHADER_ENGINES);
	cfg->gb_addr_config_fields.pipe_interleave_size =
		1 << (8 + REG_GET_FIELD(cfg->gb_addr_config, GB_ADDR_CONFIG, PIPE_INTERLEAVE_SIZE));

	return 0;
}

/*
 * gfx_v9_0_sw_init, the gfx-ring subset: no IRQ sources (no IH yet), no
 * MEC/KIQ/compute rings/MQDs (see the file header), no software rings
 * (gfx.mcbp is off).
 */
static int gfx_v9_0_sw_init(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	struct frb_ring *ring;
	int r;

	r = gfx_v9_0_init_microcode(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to load gfx firmware!");
		return r;
	}

	r = gfx_v9_0_rlc_init(adev);
	if (r) {
		frb_err(adev, "gfx: Failed to init rlc BOs!");
		return r;
	}

	/* set up the gfx ring */
	ring = &g->gfx_ring;
	r = frb_ring_init(adev, ring, "gfx", &gfx_v9_0_ring_funcs_gfx, 1024);
	if (r)
		return r;
	ring->use_doorbell = frb_doorbell_usable(adev);
	ring->doorbell_index = AMDGPU_VEGA20_DOORBELL_GFX_RING0 << 1;
	frb_wb_write64(adev, ring->wptr_offs, 0);
	frb_ring_clear(ring);

	r = gfx_v9_0_gpu_early_init(adev);
	if (r)
		return r;

	g->sw_done = 1;
	return 0;
}

static uint32_t gfx_v9_0_get_rb_active_bitmap(struct frb_device *adev)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	uint32_t data, mask;

	data = RREG32_SOC15(GC, 0, mmCC_RB_BACKEND_DISABLE);
	data |= RREG32_SOC15(GC, 0, mmGC_USER_RB_BACKEND_DISABLE);

	data &= CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK;
	data >>= GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

	mask = amdgpu_gfx_create_bitmask(cfg->max_backends_per_se / cfg->max_sh_per_se);

	return (~data) & mask;
}

static void gfx_v9_0_setup_rb(struct frb_device *adev)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	uint32_t i, j;
	uint32_t data;
	uint32_t active_rbs = 0;
	uint32_t rb_bitmap_width_per_sh = cfg->max_backends_per_se / cfg->max_sh_per_se;

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			gfx_v9_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			data = gfx_v9_0_get_rb_active_bitmap(adev);
			active_rbs |= data << ((i * cfg->max_sh_per_se + j) * rb_bitmap_width_per_sh);
		}
	}
	gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cfg->backend_enable_mask = active_rbs;
	cfg->num_rbs = hweight32(active_rbs);
}

#define DEFAULT_SH_MEM_BASES	(0x6000)
static void gfx_v9_0_init_compute_vmid(struct frb_device *adev)
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

	sh_mem_config = SH_MEM_ADDRESS_MODE_64 |
			SH_MEM_ALIGNMENT_MODE_UNALIGNED <<
			SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT;

	for (i = FRB_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		soc15_grbm_select(adev, 0, 0, 0, i, 0);
		/* CP and shaders */
		WREG32_SOC15_RLC(GC, 0, mmSH_MEM_CONFIG, sh_mem_config);
		WREG32_SOC15_RLC(GC, 0, mmSH_MEM_BASES, sh_mem_bases);
	}
	soc15_grbm_select(adev, 0, 0, 0, 0, 0);

	/* Initialize all compute VMIDs to have no GDS, GWS, or OA
	   access. These should be enabled by FW for target VMIDs. */
	for (i = FRB_FIRST_KFD_VMID; i < AMDGPU_NUM_VMID; i++) {
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_BASE, 2 * i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_SIZE, 2 * i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_GWS_VMID0, i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_OA_VMID0, i, 0);
	}
}

static void gfx_v9_0_init_gds_vmid(struct frb_device *adev)
{
	int vmid;

	/*
	 * Initialize all compute and user-gfx VMIDs to have no GDS, GWS, or OA
	 * access. Compute VMIDs should be enabled by FW for target VMIDs,
	 * the driver can enable them for graphics. VMID0 should maintain
	 * access so that HWS firmware can save/restore entries.
	 */
	for (vmid = 1; vmid < AMDGPU_NUM_VMID; vmid++) {
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_BASE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_SIZE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_GWS_VMID0, vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_OA_VMID0, vmid, 0);
	}
}

static void gfx_v9_0_set_user_cu_inactive_bitmap(struct frb_device *adev, uint32_t bitmap)
{
	uint32_t data;

	if (!bitmap)
		return;

	data = bitmap << GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;
	data &= GC_USER_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;

	WREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG, data);
}

static uint32_t gfx_v9_0_get_cu_active_bitmap(struct frb_device *adev)
{
	uint32_t data, mask;

	data = RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
	data |= RREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);

	data &= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS_MASK;
	data >>= CC_GC_SHADER_ARRAY_CONFIG__INACTIVE_CUS__SHIFT;

	mask = amdgpu_gfx_create_bitmask(adev->gfx9->config.max_cu_per_sh);

	return (~data) & mask;
}

static int gfx_v9_0_get_cu_info(struct frb_device *adev, struct frb_gfx9_cu_info *cu_info)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	uint32_t i, j, k, counter, active_cu_number = 0;
	uint32_t mask, bitmap, ao_bitmap, ao_cu_mask = 0;
	unsigned disable_masks[4 * 4];

	/*
	 * 16 comes from bitmap array size 4*4, and it can cover all gfx9 ASICs
	 */
	if (cfg->max_shader_engines * cfg->max_sh_per_se > 16)
		return -EINVAL;

	/* amdgpu_gfx_parse_disable_cu: amdgpu.disable_cu is unset */
	memset(disable_masks, 0, sizeof(disable_masks));

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			mask = 1;
			ao_bitmap = 0;
			counter = 0;
			gfx_v9_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			gfx_v9_0_set_user_cu_inactive_bitmap(adev, disable_masks[i * cfg->max_sh_per_se + j]);
			bitmap = gfx_v9_0_get_cu_active_bitmap(adev);

			/* SE4..7 (Arcturus) fold into bitmap[0..3][1]; Vega 20 has four */
			cu_info->bitmap[i % 4][j + i / 4] = bitmap;

			for (k = 0; k < cfg->max_cu_per_sh; k++) {
				if (bitmap & mask) {
					if (counter < cfg->max_cu_per_sh)
						ao_bitmap |= mask;
					counter++;
				}
				mask <<= 1;
			}
			active_cu_number += counter;
			if (i < 2 && j < 2)
				ao_cu_mask |= (ao_bitmap << (i * 16 + j * 8));
			cu_info->ao_cu_bitmap[i % 4][j + i / 4] = ao_bitmap;
		}
	}
	gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	cu_info->number = active_cu_number;
	cu_info->ao_cu_mask = ao_cu_mask;
	cu_info->simd_per_cu = NUM_SIMD_PER_CU;

	return 0;
}

static void gfx_v9_0_constants_init(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	uint32_t tmp;
	int i;

	WREG32_FIELD15_RLC(GC, 0, GRBM_CNTL, READ_TIMEOUT, 0xff);

	/* gfx_v9_0_tiling_mode_table_init: nothing on GFX9 */

	gfx_v9_0_setup_rb(adev);
	gfx_v9_0_get_cu_info(adev, &g->cu_info);
	g->config.db_debug2 = RREG32_SOC15(GC, 0, mmDB_DEBUG2);

	/* XXX SH_MEM regs */
	/* where to put LDS, scratch, GPUVM in FSA64 space */
	for (i = 0; i < FRB_GFXHUB_NUM_IDS; i++) {
		soc15_grbm_select(adev, 0, 0, 0, i, 0);
		/* CP and shaders */
		if (i == 0) {
			tmp = REG_SET_FIELD(0, SH_MEM_CONFIG, ALIGNMENT_MODE,
					    SH_MEM_ALIGNMENT_MODE_UNALIGNED);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, RETRY_DISABLE,
					    !!adev->gmc.noretry);
			WREG32_SOC15_RLC(GC, 0, mmSH_MEM_CONFIG, tmp);
			WREG32_SOC15_RLC(GC, 0, mmSH_MEM_BASES, 0);
		} else {
			tmp = REG_SET_FIELD(0, SH_MEM_CONFIG, ALIGNMENT_MODE,
					    SH_MEM_ALIGNMENT_MODE_UNALIGNED);
			tmp = REG_SET_FIELD(tmp, SH_MEM_CONFIG, RETRY_DISABLE,
					    !!adev->gmc.noretry);
			WREG32_SOC15_RLC(GC, 0, mmSH_MEM_CONFIG, tmp);
			tmp = REG_SET_FIELD(0, SH_MEM_BASES, PRIVATE_BASE,
					    (uint32_t)(FRB_PRIVATE_APERTURE_START >> 48));
			tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE,
					    (uint32_t)(FRB_SHARED_APERTURE_START >> 48));
			WREG32_SOC15_RLC(GC, 0, mmSH_MEM_BASES, tmp);
		}
	}
	soc15_grbm_select(adev, 0, 0, 0, 0, 0);

	gfx_v9_0_init_compute_vmid(adev);
	gfx_v9_0_init_gds_vmid(adev);
	/* gfx_v9_0_init_sq_config: nothing on 9.4.0 */
}

static void gfx_v9_0_wait_for_rlc_serdes(struct frb_device *adev)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	uint32_t i, j, k;
	uint32_t mask;

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			gfx_v9_0_select_se_sh(adev, i, j, 0xffffffff, 0);
			for (k = 0; k < adev->usec_timeout; k++) {
				if (RREG32_SOC15(GC, 0, mmRLC_SERDES_CU_MASTER_BUSY) == 0)
					break;
				udelay(1);
			}
			if (k == adev->usec_timeout) {
				gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
				frb_info(adev, "gfx: Timeout wait for RLC serdes %u,%u", i, j);
				return;
			}
		}
	}
	gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);

	mask = RLC_SERDES_NONCU_MASTER_BUSY__SE_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__GC_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__TC0_MASTER_BUSY_MASK |
		RLC_SERDES_NONCU_MASTER_BUSY__TC1_MASTER_BUSY_MASK;
	for (k = 0; k < adev->usec_timeout; k++) {
		if ((RREG32_SOC15(GC, 0, mmRLC_SERDES_NONCU_MASTER_BUSY) & mask) == 0)
			break;
		udelay(1);
	}
}

static void gfx_v9_0_enable_gui_idle_interrupt(struct frb_device *adev, int enable)
{
	uint32_t tmp;

	/* These interrupts should be enabled to drive DS clock */

	tmp = RREG32_SOC15(GC, 0, mmCP_INT_CNTL_RING0);

	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_BUSY_INT_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CNTX_EMPTY_INT_ENABLE, enable ? 1 : 0);
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, CMP_BUSY_INT_ENABLE, enable ? 1 : 0);
	/* num_gfx_rings */
	tmp = REG_SET_FIELD(tmp, CP_INT_CNTL_RING0, GFX_IDLE_INT_ENABLE, enable ? 1 : 0);

	WREG32_SOC15(GC, 0, mmCP_INT_CNTL_RING0, tmp);
}

static void gfx_v9_0_init_csb(struct frb_device *adev)
{
	struct frb_gfx9_rlc *rlc = &adev->gfx9->rlc;

	gfx_v9_0_get_csb_buffer(adev, rlc->clear_state_offset);
	frb_flush_hdp(adev);	/* the CSB went in through BAR0 (frb_ring.h, placement) */
	/* csib */
	WREG32_RLC(SOC15_REG_OFFSET(GC, 0, mmRLC_CSIB_ADDR_HI), (uint32_t)(rlc->clear_state_gpu_addr >> 32));
	WREG32_RLC(SOC15_REG_OFFSET(GC, 0, mmRLC_CSIB_ADDR_LO), rlc->clear_state_gpu_addr & 0xfffffffc);
	WREG32_RLC(SOC15_REG_OFFSET(GC, 0, mmRLC_CSIB_LENGTH), rlc->clear_state_size);
}

static void gfx_v9_0_enable_save_restore_machine(struct frb_device *adev)
{
	WREG32_FIELD15(GC, 0, RLC_SRM_CNTL, SRM_ENABLE, 1);
}

/*
 * gfx_v9_0_init_pg: the CSB; with an RLC v2.1 image the save/restore
 * machine (gfx_v9_1_init_rlc_save_restore_list is Raven's only: the PSP
 * loaded Vega 20's lists, RLC_RESTORE_LIST_CNTL/GPM_MEM/SRM_MEM in stage
 * smu); no power gating (pg_flags 0 on Vega 20, as Linux's soc15.c has it).
 */
static void gfx_v9_0_init_pg(struct frb_device *adev)
{
	gfx_v9_0_init_csb(adev);

	if (adev->gfx9->rlc.is_rlc_v2_1)
		gfx_v9_0_enable_save_restore_machine(adev);
}

static void gfx_v9_0_rlc_stop(struct frb_device *adev)
{
	WREG32_FIELD15(GC, 0, RLC_CNTL, RLC_ENABLE_F32, 0);
	gfx_v9_0_enable_gui_idle_interrupt(adev, 0);
	gfx_v9_0_wait_for_rlc_serdes(adev);
}

static void gfx_v9_0_rlc_start(struct frb_device *adev)
{
	WREG32_FIELD15(GC, 0, RLC_CNTL, RLC_ENABLE_F32, 1);
	udelay(50);

	/* carrizo do enable cp interrupt after cp inited; not an APU */
	gfx_v9_0_enable_gui_idle_interrupt(adev, 1);
	udelay(50);
}

/* gfx_v9_0_init_always_on_cu_mask (not an APU, not 9.2.1: 12 CUs always on) */
static void gfx_v9_0_init_always_on_cu_mask(struct frb_device *adev)
{
	struct frb_gfx9_config *cfg = &adev->gfx9->config;
	struct frb_gfx9_cu_info *cu_info = &adev->gfx9->cu_info;
	uint32_t pg_always_on_cu_num = 2;
	uint32_t always_on_cu_num = 12;
	uint32_t i, j, k;
	uint32_t mask, cu_bitmap, counter;

	for (i = 0; i < cfg->max_shader_engines; i++) {
		for (j = 0; j < cfg->max_sh_per_se; j++) {
			mask = 1;
			cu_bitmap = 0;
			counter = 0;
			gfx_v9_0_select_se_sh(adev, i, j, 0xffffffff, 0);

			for (k = 0; k < cfg->max_cu_per_sh; k++) {
				if (cu_info->bitmap[i][j] & mask) {
					if (counter == pg_always_on_cu_num)
						WREG32_SOC15(GC, 0, mmRLC_PG_ALWAYS_ON_CU_MASK, cu_bitmap);
					if (counter < always_on_cu_num)
						cu_bitmap |= mask;
					else
						break;
					counter++;
				}
				mask <<= 1;
			}

			WREG32_SOC15(GC, 0, mmRLC_LB_ALWAYS_ACTIVE_CU_MASK, cu_bitmap);
			cu_info->ao_cu_bitmap[i][j] = cu_bitmap;
		}
	}
	gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
}

static void gfx_v9_4_init_lbpw(struct frb_device *adev)
{
	uint32_t data;

	/* set mmRLC_LB_THR_CONFIG_1/2/3/4 */
	WREG32_SOC15(GC, 0, mmRLC_LB_THR_CONFIG_1, 0x0000007F);
	WREG32_SOC15(GC, 0, mmRLC_LB_THR_CONFIG_2, 0x033388F8);
	WREG32_SOC15(GC, 0, mmRLC_LB_THR_CONFIG_3, 0x00000077);
	WREG32_SOC15(GC, 0, mmRLC_LB_THR_CONFIG_4, (0x10 | 0x27 << 8 | 0x02FA << 16));

	/* set mmRLC_LB_CNTR_INIT = 0x0000_0000 */
	WREG32_SOC15(GC, 0, mmRLC_LB_CNTR_INIT, 0x00000000);

	/* set mmRLC_LB_CNTR_MAX = 0x0000_0500 */
	WREG32_SOC15(GC, 0, mmRLC_LB_CNTR_MAX, 0x00000800);

	/* set mmRLC_LB_INIT_CU_MASK thru broadcast mode to enable all SE/SH*/
	gfx_v9_0_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff, 0);
	WREG32_SOC15(GC, 0, mmRLC_LB_INIT_CU_MASK, 0xffffffff);

	/* set mmRLC_LB_PARAMS = 0x003F_1006 */
	data = REG_SET_FIELD(0, RLC_LB_PARAMS, FIFO_SAMPLES, 0x0003);
	data |= REG_SET_FIELD(data, RLC_LB_PARAMS, PG_IDLE_SAMPLES, 0x0010);
	data |= REG_SET_FIELD(data, RLC_LB_PARAMS, PG_IDLE_SAMPLE_INTERVAL, 0x033F);
	WREG32_SOC15(GC, 0, mmRLC_LB_PARAMS, data);

	/* set mmRLC_GPM_GENERAL_7[31-16] = 0x00C0 */
	data = RREG32_SOC15(GC, 0, mmRLC_GPM_GENERAL_7);
	data &= 0x0000FFFF;
	data |= 0x00C00000;
	WREG32_SOC15(GC, 0, mmRLC_GPM_GENERAL_7, data);

	/*
	 * RLC_LB_ALWAYS_ACTIVE_CU_MASK = 0xFFF (12 CUs AON),
	 * programmed in gfx_v9_0_init_always_on_cu_mask()
	 */

	/* set RLC_LB_CNTL = 0x8000_0095, 31 bit is reserved,
	 * but used for RLC_LB_CNTL configuration */
	data = RLC_LB_CNTL__LB_CNT_SPIM_ACTIVE_MASK;
	data |= REG_SET_FIELD(data, RLC_LB_CNTL, CU_MASK_USED_OFF_HYST, 0x09);
	data |= REG_SET_FIELD(data, RLC_LB_CNTL, RESERVED, 0x80000);
	WREG32_SOC15(GC, 0, mmRLC_LB_CNTL, data);

	gfx_v9_0_init_always_on_cu_mask(adev);
}

static void gfx_v9_0_enable_lbpw(struct frb_device *adev, int enable)
{
	WREG32_FIELD15(GC, 0, RLC_LB_CNTL, LOAD_BALANCE_ENABLE, enable ? 1 : 0);
}

static void gfx_v9_0_update_spm_vmid_internal(struct frb_device *adev, unsigned int vmid)
{
	uint32_t data;

	data = RREG32_SOC15(GC, 0, mmRLC_SPM_MC_CNTL);

	data &= ~RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK;
	data |= (vmid & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT;

	WREG32_SOC15(GC, 0, mmRLC_SPM_MC_CNTL, data);
}

/* gfx_v9_0_rlc_resume, bare metal, PSP-loaded RLC, IP_VERSION(9, 4, 0) */
static int gfx_v9_0_rlc_resume(struct frb_device *adev)
{
	gfx_v9_0_rlc_stop(adev);

	/* disable CG */
	WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, 0);

	gfx_v9_0_init_pg(adev);

	/* AMDGPU_FW_LOAD_PSP: no gfx_v9_0_rlc_load_microcode */

	gfx_v9_4_init_lbpw(adev);
	gfx_v9_0_enable_lbpw(adev, 0);	/* amdgpu_lbpw -1 */

	gfx_v9_0_update_spm_vmid_internal(adev, 0xf);

	gfx_v9_0_rlc_start(adev);

	return 0;
}

static void gfx_v9_0_cp_gfx_enable(struct frb_device *adev, int enable)
{
	uint32_t tmp = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);

	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_INVALIDATE_ICACHE, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_INVALIDATE_ICACHE, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_INVALIDATE_ICACHE, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_PIPE0_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_PIPE1_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE0_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_PIPE1_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE0_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_PIPE1_RESET, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_HALT, enable ? 0 : 1);
	WREG32_SOC15_RLC(GC, 0, mmCP_ME_CNTL, tmp);
	udelay(50);
}

static int gfx_v9_0_cp_gfx_start(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	struct frb_ring *ring = &g->gfx_ring;
	const struct cs_section_def *sect = NULL;
	const struct cs_extent_def *ext = NULL;
	int r;
	uint32_t i, tmp;

	/* init the CP */
	WREG32_SOC15(GC, 0, mmCP_MAX_CONTEXT, g->config.max_hw_contexts - 1);
	WREG32_SOC15(GC, 0, mmCP_DEVICE_ID, 1);

	gfx_v9_0_cp_gfx_enable(adev, 1);

	/* not an APU: the CSB packets go out */
	r = frb_ring_alloc(ring, gfx_v9_0_get_csb_size(adev) + 4 + 3);
	if (r) {
		frb_err(adev, "gfx: cp failed to lock ring (%d).", r);
		return r;
	}

	frb_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	frb_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	frb_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	frb_ring_write(ring, 0x80000000);
	frb_ring_write(ring, 0x80000000);

	for (sect = gfx9_cs_data; sect->section != NULL; ++sect) {
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

	frb_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	frb_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	frb_ring_write(ring, 0x8000);
	frb_ring_write(ring, 0x8000);

	frb_ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	tmp = (PACKET3_SET_UCONFIG_REG_INDEX_TYPE |
	       (SOC15_REG_OFFSET(GC, 0, mmVGT_INDEX_TYPE) - PACKET3_SET_UCONFIG_REG_START));
	frb_ring_write(ring, tmp);
	frb_ring_write(ring, 0);

	frb_ring_commit(ring);

	return 0;
}

static int gfx_v9_0_cp_gfx_resume(struct frb_device *adev)
{
	struct frb_ring *ring;
	uint32_t tmp;
	uint32_t rb_bufsz;
	uint64_t rb_addr, rptr_addr, wptr_gpu_addr;

	/* Set the write pointer delay */
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_DELAY, 0);

	/* set the RB to use vmid 0 */
	WREG32_SOC15(GC, 0, mmCP_RB_VMID, 0);

	/* Set ring buffer size */
	ring = &adev->gfx9->gfx_ring;
	rb_bufsz = order_base_2(ring->ring_size / 8);
	tmp = REG_SET_FIELD(0, CP_RB0_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, CP_RB0_CNTL, RB_BLKSZ, rb_bufsz - 2);
	/* no BUF_SWAP: the ring is little-endian (the file header) */
	WREG32_SOC15(GC, 0, mmCP_RB0_CNTL, tmp);

	/* Initialize the ring buffer's write pointers */
	ring->wptr = 0;
	WREG32_SOC15(GC, 0, mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
	WREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI, upper_32_bits(ring->wptr));

	/* set the wb address whether it's enabled or not */
	rptr_addr = ring->rptr_gpu_addr;
	WREG32_SOC15(GC, 0, mmCP_RB0_RPTR_ADDR, lower_32_bits(rptr_addr));
	WREG32_SOC15(GC, 0, mmCP_RB0_RPTR_ADDR_HI, upper_32_bits(rptr_addr) & CP_RB_RPTR_ADDR_HI__RB_RPTR_ADDR_HI_MASK);

	wptr_gpu_addr = ring->wptr_gpu_addr;
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_LO, lower_32_bits(wptr_gpu_addr));
	WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_ADDR_HI, upper_32_bits(wptr_gpu_addr));

	mdelay(1);
	WREG32_SOC15(GC, 0, mmCP_RB0_CNTL, tmp);

	rb_addr = ring->gpu_addr >> 8;
	WREG32_SOC15(GC, 0, mmCP_RB0_BASE, lower_32_bits(rb_addr));
	WREG32_SOC15(GC, 0, mmCP_RB0_BASE_HI, upper_32_bits(rb_addr));

	tmp = RREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL);
	if (ring->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET, ring->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
	}
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL, tmp);

	tmp = REG_SET_FIELD(0, CP_RB_DOORBELL_RANGE_LOWER, DOORBELL_RANGE_LOWER, ring->doorbell_index);
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_LOWER, tmp);

	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_UPPER, CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);

	/* start the ring */
	frb_ring_clear(ring);
	frb_wb_write64(adev, ring->wptr_offs, 0);
	return gfx_v9_0_cp_gfx_start(adev);
}

static void gfx_v9_0_cp_compute_enable(struct frb_device *adev, int enable)
{
	if (enable) {
		WREG32_SOC15_RLC(GC, 0, mmCP_MEC_CNTL, 0);
	} else {
		WREG32_SOC15_RLC(GC, 0, mmCP_MEC_CNTL,
				 (CP_MEC_CNTL__MEC_INVALIDATE_ICACHE_MASK |
				  CP_MEC_CNTL__MEC_ME1_PIPE0_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME1_PIPE1_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME1_PIPE2_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME1_PIPE3_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME2_PIPE0_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME2_PIPE1_RESET_MASK |
				  CP_MEC_CNTL__MEC_ME1_HALT_MASK |
				  CP_MEC_CNTL__MEC_ME2_HALT_MASK));
	}
	udelay(50);
}

/* amdgpu_ring_test_helper on the gfx ring */
static int gfx_v9_0_ring_test_helper(struct frb_ring *ring)
{
	int r = gfx_v9_0_ring_test_ring(ring);

	ring->ready = !r;
	return r;
}

/*
 * gfx_v9_0_cp_resume: the gfx ring's part. The KIQ (kiq_resume) and the
 * compute queues (kcq_resume, their ring tests) are not ported (the file
 * header): the MEC stays halted as cp_compute_enable(false) leaves it.
 */
static int gfx_v9_0_cp_resume(struct frb_device *adev)
{
	int r;

	/* not an APU */
	gfx_v9_0_enable_gui_idle_interrupt(adev, 0);

	/* AMDGPU_FW_LOAD_PSP: no cp_gfx_load_microcode / cp_compute_load_microcode */

	gfx_v9_0_cp_gfx_enable(adev, 0);
	gfx_v9_0_cp_compute_enable(adev, 0);

	/* gfx_v9_0_kiq_resume: not ported */

	r = gfx_v9_0_cp_gfx_resume(adev);
	if (r)
		return r;

	/*
	 * gfx_v9_0_kcq_resume's first step, cp_compute_enable(true), and none of
	 * its queues: the MEC runs with nothing mapped. On the card (
	 * 2026-11-15) with the MEC left halted the gfx ring test timed out with
	 * the PFP waiting on an RCIU read (CP_STALLED_STAT2 PFP_RCIU_READ_PENDING),
	 * nothing parsed (CP_PFP_HEADER_DUMP def0def0) and the fetcher stopped at
	 * 0x98 dwords (the ROQ full); Linux unhalts the MEC before its ring test.
	 */
	gfx_v9_0_cp_compute_enable(adev, 1);

	r = gfx_v9_0_ring_test_helper(&adev->gfx9->gfx_ring);
	if (r)
		return r;

	/* the compute rings' tests: none */

	gfx_v9_0_enable_gui_idle_interrupt(adev, 1);

	return 0;
}

static void gfx_v9_0_cp_enable(struct frb_device *adev, int enable)
{
	gfx_v9_0_cp_gfx_enable(adev, enable);
	gfx_v9_0_cp_compute_enable(adev, enable);
}

/* gfx_v9_0_hw_init (no cleaner shader bo: it is only emitted per job) */
static int gfx_v9_0_hw_init(struct frb_device *adev)
{
	int r;

	gfx_v9_0_init_golden_registers(adev);

	gfx_v9_0_constants_init(adev);

	/* gfx_v9_0_init_tcp_config: 9.4.1 / 9.4.2 only */

	adev->gfx9->hw_started = 1;
	r = gfx_v9_0_rlc_resume(adev);
	if (r)
		return r;

	r = gfx_v9_0_cp_resume(adev);
	if (r)
		return r;

	return r;
}

/*
 * gfx_v9_0_hw_fini, bare metal, not in reset or suspend: no interrupt
 * sources to put, no KCQs to disable and no KIQ to tear down (none were set
 * up), then both CPs halted and the RLC stopped (GC 9.4.0 < 9.4.2).
 */
static int gfx_v9_0_hw_fini(struct frb_device *adev)
{
	gfx_v9_0_cp_enable(adev, 0);
	gfx_v9_0_rlc_stop(adev);
	return 0;
}

/* --- ring functions -------------------------------------------------------- */

static uint64_t gfx_v9_0_ring_get_rptr_gfx(struct frb_ring *ring)
{
	return frb_wb_read(ring->adev, ring->rptr_offs);	/* gfx9 is 32bit rptr */
}

static uint64_t gfx_v9_0_ring_get_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint64_t wptr;

	if (ring->use_doorbell) {
		wptr = frb_wb_read64(adev, ring->wptr_offs);
	} else {
		wptr = RREG32_SOC15(GC, 0, mmCP_RB0_WPTR);
		wptr += (uint64_t)RREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI) << 32;
	}

	return wptr;
}

/*
 * gfx_v9_0_ring_set_wptr_gfx. The doorbell arm writes the WPTR writeback
 * (amdgpu's atomic64_set on wptr_cpu_addr) little-endian, flushes the HDP
 * (it is in VRAM here, through BAR0), then rings the doorbell.
 */
static void gfx_v9_0_ring_set_wptr_gfx(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;

	if (ring->use_doorbell) {
		frb_wb_write64(adev, ring->wptr_offs, ring->wptr);
		frb_flush_hdp(adev);
		frb_doorbell_write64(adev, ring->doorbell_index, ring->wptr);
	} else {
		WREG32_SOC15(GC, 0, mmCP_RB0_WPTR, lower_32_bits(ring->wptr));
		WREG32_SOC15(GC, 0, mmCP_RB0_WPTR_HI, upper_32_bits(ring->wptr));
	}
}

static void gfx_v9_0_wait_reg_mem(struct frb_ring *ring, int eng_sel, int mem_space, int opt,
				  uint32_t addr0, uint32_t addr1, uint32_t ref, uint32_t mask, uint32_t inv)
{
	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring,
		       /* memory (1) or register (0) */
		       (WAIT_REG_MEM_MEM_SPACE(mem_space) |
			WAIT_REG_MEM_OPERATION(opt) | /* wait */
			WAIT_REG_MEM_FUNCTION(3) |  /* equal */
			WAIT_REG_MEM_ENGINE(eng_sel)));

	frb_ring_write(ring, addr0);
	frb_ring_write(ring, addr1);
	frb_ring_write(ring, ref);
	frb_ring_write(ring, mask);
	frb_ring_write(ring, inv); /* poll interval */
}

/* gfx_v9_0_ring_emit_hdp_flush (amdgpu_gfx_get_hdp_flush_mask's GFX arm: CP0 << pipe, the PFP) */
static void gfx_v9_0_ring_emit_hdp_flush(struct frb_ring *ring)
{
	struct frb_device *adev = ring->adev;
	uint32_t ref_and_mask = GPU_HDP_FLUSH_DONE__CP0_MASK << ring->pipe, reg_mem_engine = 1;

	gfx_v9_0_wait_reg_mem(ring, reg_mem_engine, 0, 1,
			      SOC15_REG_OFFSET(NBIO, 0, mmGPU_HDP_FLUSH_REQ),
			      SOC15_REG_OFFSET(NBIO, 0, mmGPU_HDP_FLUSH_DONE),
			      ref_and_mask, ref_and_mask, 0x20);
}

static void gfx_v9_0_ring_emit_ib_gfx(struct frb_ring *ring, const struct frb_ib *ib,
				      uint32_t vmid, uint32_t flags)
{
	uint32_t header, control = 0;

	(void)flags;
	if (ib->flags & AMDGPU_IB_FLAG_CE)
		header = PACKET3(PACKET3_INDIRECT_BUFFER_CONST, 2);
	else
		header = PACKET3(PACKET3_INDIRECT_BUFFER, 2);

	control |= ib->length_dw | (vmid << 24);

	frb_ring_write(ring, header);
	/* no IB swap (Linux: (2 << 0) under __BIG_ENDIAN): every IB is little-endian */
	frb_ring_write(ring, lower_32_bits(ib->gpu_addr) & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(ib->gpu_addr));
	frb_ring_write(ring, control);
}

static void gfx_v9_0_ring_emit_fence(struct frb_ring *ring, uint64_t addr, uint64_t seq, unsigned flags)
{
	int write64bit = flags & FRB_FENCE_FLAG_64BIT;
	int int_sel = flags & FRB_FENCE_FLAG_INT;
	uint32_t dw2;

	/* RELEASE_MEM - flush caches, send int */
	frb_ring_write(ring, PACKET3(PACKET3_RELEASE_MEM, 6));

	/* not AMDGPU_FENCE_FLAG_TC_WB_ONLY, not EXEC */
	dw2 = EOP_TCL1_ACTION_EN | EOP_TC_ACTION_EN | EOP_TC_MD_ACTION_EN;
	dw2 |= EOP_TC_WB_ACTION_EN | EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) | EVENT_INDEX(5);

	frb_ring_write(ring, dw2);
	frb_ring_write(ring, DATA_SEL(write64bit ? 2 : 1) | INT_SEL(int_sel ? 2 : 0));

	/*
	 * the address should be Qword aligned if 64bit write, Dword
	 * aligned if only send 32bit data low (discard data high)
	 */
	frb_ring_write(ring, lower_32_bits(addr));
	frb_ring_write(ring, upper_32_bits(addr));
	frb_ring_write(ring, lower_32_bits(seq));
	frb_ring_write(ring, upper_32_bits(seq));
	frb_ring_write(ring, 0);
}

static void gfx_v9_0_ring_emit_event_write(struct frb_ring *ring, uint32_t event_type, uint32_t event_index)
{
	frb_ring_write(ring, PACKET3(PACKET3_EVENT_WRITE, 0));
	frb_ring_write(ring, EVENT_TYPE(event_type) | EVENT_INDEX(event_index));
}

static void gfx_v9_0_emit_mem_sync(struct frb_ring *ring)
{
	const unsigned int cp_coher_cntl =
			PACKET3_ACQUIRE_MEM_CP_COHER_CNTL_SH_ICACHE_ACTION_ENA(1) |
			PACKET3_ACQUIRE_MEM_CP_COHER_CNTL_SH_KCACHE_ACTION_ENA(1) |
			PACKET3_ACQUIRE_MEM_CP_COHER_CNTL_TC_ACTION_ENA(1) |
			PACKET3_ACQUIRE_MEM_CP_COHER_CNTL_TCL1_ACTION_ENA(1) |
			PACKET3_ACQUIRE_MEM_CP_COHER_CNTL_TC_WB_ACTION_ENA(1);

	/* ACQUIRE_MEM -make one or more surfaces valid for use by the subsequent operations */
	frb_ring_write(ring, PACKET3(PACKET3_ACQUIRE_MEM, 5));
	frb_ring_write(ring, cp_coher_cntl); /* CP_COHER_CNTL */
	frb_ring_write(ring, 0xffffffff);  /* CP_COHER_SIZE */
	frb_ring_write(ring, 0xffffff);  /* CP_COHER_SIZE_HI */
	frb_ring_write(ring, 0); /* CP_COHER_BASE */
	frb_ring_write(ring, 0);  /* CP_COHER_BASE_HI */
	frb_ring_write(ring, 0x0000000A); /* POLL_INTERVAL */
}

/* gfx_v9_0_ring_emit_pipeline_sync, the GFX arm */
static void gfx_v9_0_ring_emit_pipeline_sync(struct frb_ring *ring)
{
	gfx_v9_0_ring_emit_event_write(ring, VS_PARTIAL_FLUSH, 4);
	gfx_v9_0_ring_emit_event_write(ring, PS_PARTIAL_FLUSH, 4);
	gfx_v9_0_ring_emit_event_write(ring, CS_PARTIAL_FLUSH, 4);
	gfx_v9_0_emit_mem_sync(ring);
}

/*
 * Wait in the PFP for a dword in memory to compare with ref by func (another
 * engine's fence), as gfx_v9_0_wait_reg_mem's memory arm.
 */
static void gfx_v9_0_ring_emit_wait_mem(struct frb_ring *ring, uint64_t addr, uint32_t ref, uint32_t func)
{
	frb_ring_write(ring, PACKET3(PACKET3_WAIT_REG_MEM, 5));
	frb_ring_write(ring, (WAIT_REG_MEM_MEM_SPACE(1) | /* memory */
			      WAIT_REG_MEM_FUNCTION(func) |
			      WAIT_REG_MEM_ENGINE(1)));   /* pfp */
	frb_ring_write(ring, lower_32_bits(addr) & 0xfffffffc);
	frb_ring_write(ring, upper_32_bits(addr));
	frb_ring_write(ring, ref);
	frb_ring_write(ring, 0xffffffff);
	frb_ring_write(ring, 4); /* poll interval */
}

/* gfx_v9_0_ring_emit_wreg, the GFX arm (the ME writes, confirmed) */
static void gfx_v9_0_ring_emit_wreg(struct frb_ring *ring, uint32_t reg, uint32_t val)
{
	uint32_t cmd = WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM;

	frb_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
	frb_ring_write(ring, cmd);
	frb_ring_write(ring, reg);
	frb_ring_write(ring, 0);
	frb_ring_write(ring, val);
}

/* gfx_v9_ring_insert_nop: one NOP packet whose payload is the rest */
static void gfx_v9_ring_insert_nop(struct frb_ring *ring, uint32_t num_nop)
{
	uint32_t i;

	if (!num_nop)
		return;
	/* Header itself is a NOP packet */
	if (num_nop == 1) {
		frb_ring_write(ring, ring->funcs->nop);
		return;
	}

	/* Max HW optimization till 0x3ffe, followed by remaining one NOP at a time*/
	frb_ring_write(ring, PACKET3(PACKET3_NOP, min_t(uint32_t, num_nop - 2, 0x3ffe)));

	/* Header is at index 0, followed by num_nops - 1 NOP packet's */
	for (i = 0; i < num_nop - 1; i++)
		frb_ring_write(ring, ring->funcs->nop);
}

static const struct frb_ring_funcs gfx_v9_0_ring_funcs_gfx = {
	.align_mask = 0xff,
	.nop = PACKET3(PACKET3_NOP, 0x3FFF),
	.support_64bit_ptrs = 1,
	.get_rptr = gfx_v9_0_ring_get_rptr_gfx,
	.get_wptr = gfx_v9_0_ring_get_wptr_gfx,
	.set_wptr = gfx_v9_0_ring_set_wptr_gfx,
	.emit_ib = gfx_v9_0_ring_emit_ib_gfx,
	.emit_fence = gfx_v9_0_ring_emit_fence,
	.emit_hdp_flush = gfx_v9_0_ring_emit_hdp_flush,
	.insert_nop = gfx_v9_ring_insert_nop,
	.pad_ib = frb_ring_generic_pad_ib,	/* amdgpu_ring_generic_pad_ib */
	.emit_wreg = gfx_v9_0_ring_emit_wreg,
	.emit_pipeline_sync = gfx_v9_0_ring_emit_pipeline_sync,
	.emit_wait_mem = gfx_v9_0_ring_emit_wait_mem,
};

/* --- the stage, the tests, the teardown ------------------------------------ */

static void gfx_free(struct frb_device *adev)
{
	if (!adev->gfx9)
		return;
	adev->os->free(adev->os->cookie, adev->gfx9);
	adev->gfx9 = NULL;
}

/*
 * Stage gfx on SOC15: gfx_v9_0_sw_init, then gfx_v9_0_hw_init (the gfx ring
 * test included). amdgpu_device_ip_init runs every block's sw_init before
 * the PSP loads the firmware; Firebrand's stages run this block's after it,
 * which changes no register value (sw_init reads GB_ADDR_CONFIG only).
 */
int frb_gfx_v9_0_init(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	int r;

	if (adev->asic_type != CHIP_VEGA20) {
		frb_err(adev, "gfx: only the GC 9.4.0 (Vega 20) paths of gfx_v9_0 are ported");
		return -ENODEV;
	}
	if (adev->stage_done < FRB_STAGE_SMU || !adev->psp) {
		frb_err(adev, "gfx: the PSP has not loaded the CP/RLC microcode (stage smu)");
		return -EPERM;
	}
	if (!adev->aper || !adev->layout.objects_size) {
		frb_err(adev, "gfx: no VRAM window or objects region (stage gmc)");
		return -ENODEV;
	}
	if (!g) {
		g = (struct frb_gfx9 *)adev->os->alloc(adev->os->cookie, sizeof(*g));
		if (!g)
			return -ENOMEM;
		adev->gfx9 = g;
	}
	if (!g->sw_done) {
		r = gfx_v9_0_sw_init(adev);
		if (r)
			return r;
	}
	frb_info(adev, "gfx: ring %u KB at %llx (BAR0 0x%08x), WPTR by %s (doorbell 0x%x)",
		 g->gfx_ring.ring_size >> 10, (unsigned long long)g->gfx_ring.gpu_addr, g->gfx_ring.bar_offset,
		 g->gfx_ring.use_doorbell ? "doorbell" : "MMIO (CP_RB0_WPTR)", g->gfx_ring.doorbell_index);
	r = gfx_v9_0_hw_init(adev);
	frb_info(adev, "gfx: %u SE x %u SH, %u CUs (always-on mask %08x), %u RBs (mask %x), "
		 "gb_addr_config %08x; CSB %u dwords at %llx",
		 g->config.max_shader_engines, g->config.max_sh_per_se, g->cu_info.number,
		 g->cu_info.ao_cu_mask, g->config.num_rbs, g->config.backend_enable_mask,
		 g->config.gb_addr_config, g->rlc.clear_state_size,
		 (unsigned long long)g->rlc.clear_state_gpu_addr);
	if (r)
		frb_err(adev, "gfx: hw_init failed (%d)", r);
	return r;
}

/* gfx_v9_0_hw_fini, then sw_fini's frees (the VRAM objects are not reused) */
void frb_gfx_v9_0_fini(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;

	if (!g)
		return;
	if (g->hw_started) {
		g->gfx_ring.ready = 0;
		gfx_v9_0_hw_fini(adev);
		frb_info(adev, "gfx: hw_fini: CP_ME_CNTL %08x, CP_MEC_CNTL %08x, RLC_CNTL %08x",
			 RREG32_SOC15(GC, 0, mmCP_ME_CNTL), RREG32_SOC15(GC, 0, mmCP_MEC_CNTL),
			 RREG32_SOC15(GC, 0, mmRLC_CNTL));
	}
	gfx_free(adev);
}

int frb_gfx_v9_0_test(struct frb_device *adev, int test)
{
	struct frb_gfx9 *g = adev->gfx9;

	if (!g || !g->hw_started)
		return -ENODEV;
	switch (test) {
	case FRB_TEST_RING:
		return gfx_v9_0_ring_test_helper(&g->gfx_ring);
	case FRB_TEST_IB:
		if (!g->gfx_ring.ready)
			return -ENODEV;
		return gfx_v9_0_ring_test_ib(&g->gfx_ring, FRB_FENCE_TIMEOUT_US);
	default:
		return -ENOSYS;
	}
}

struct frb_ring *frb_gfx_v9_0_ring(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;

	return g && g->hw_started && g->gfx_ring.ready ? &g->gfx_ring : NULL;
}

/*
 * The accelerator's view of the GFX block (frb_gfx_get_info, the kext's
 * GET_INFO and GFX_INFO): what amdgpu_info_ioctl's AMDGPU_INFO_DEV_INFO
 * takes from adev->gfx.config and cu_info for Mesa. GFX9 has no tiling
 * tables (mc_arb_ramcfg stays 0): addrlib's Gfx9Lib works from
 * gb_addr_config alone.
 */
int frb_gfx_v9_0_get_info(struct frb_device *adev, struct frb_gfx_info *info)
{
	struct frb_gfx9 *g = adev->gfx9;
	uint32_t se, sh;

	memset(info, 0, sizeof(*info));
	if (!g || !g->sw_done)
		return -ENODEV;
	info->gfx_level = 9;
	info->num_se = g->config.max_shader_engines;
	info->num_sh_per_se = g->config.max_sh_per_se;
	info->max_cu_per_sh = g->config.max_cu_per_sh;
	info->num_cu = g->cu_info.number;
	info->num_rbs = g->config.num_rbs;
	info->backend_enable_mask = g->config.backend_enable_mask;
	info->max_backends_per_se = g->config.max_backends_per_se;
	info->max_tile_pipes = g->config.max_tile_pipes;
	info->gb_addr_config = g->config.gb_addr_config;
	info->max_hw_contexts = g->config.max_hw_contexts;
	for (se = 0; se < 4; se++)
		for (sh = 0; sh < 4; sh++)
			info->cu_bitmap[se][sh] = g->cu_info.bitmap[se][sh];
	info->cu_ao_mask = g->cu_info.ao_cu_mask;
	info->simd_per_cu = g->cu_info.simd_per_cu;
	info->max_waves_per_simd = g->cu_info.max_waves_per_simd;
	info->wave_front_size = g->cu_info.wave_front_size;
	info->max_scratch_slots_per_cu = g->cu_info.max_scratch_slots_per_cu;
	info->lds_size = g->cu_info.lds_size;
	info->num_tcc_blocks = g->config.max_texture_channel_caches;
	info->max_gprs = g->config.max_gprs;
	info->max_gs_threads = g->config.max_gs_threads;
	info->gs_vgt_table_depth = g->config.gs_vgt_table_depth;
	info->gs_prim_buffer_depth = g->config.gs_prim_buffer_depth;
	info->sc_prim_fifo_size_frontend = g->config.sc_prim_fifo_size_frontend;
	info->sc_prim_fifo_size_backend = g->config.sc_prim_fifo_size_backend;
	info->sc_hiz_tile_fifo_size = g->config.sc_hiz_tile_fifo_size;
	info->sc_earlyz_tile_fifo_size = g->config.sc_earlyz_tile_fifo_size;
	info->double_offchip_lds_buf = g->config.double_offchip_lds_buf;
	info->db_debug2 = g->config.db_debug2;
	info->me_fw_version = g->me_fw_version;
	info->me_fw_feature = g->cp_fw_feature[0];
	info->pfp_fw_version = g->pfp_fw_version;
	info->pfp_fw_feature = g->cp_fw_feature[1];
	info->mec_fw_version = g->mec_fw_version;
	info->mec_fw_feature = g->cp_fw_feature[3];
	info->csb_loaded = g->rlc.clear_state_size != 0;
	return 0;
}

/* --- clock gating (gfx_v9_0_set_clockgating_state, IP_VERSION(9, 4, 0)) ---------- */

static int gfx_v9_0_is_rlc_enabled(struct frb_device *adev)
{
	uint32_t rlc_setting;

	/* if RLC is not enabled, do nothing */
	rlc_setting = RREG32_SOC15(GC, 0, mmRLC_CNTL);
	if (!(rlc_setting & RLC_CNTL__RLC_ENABLE_F32_MASK))
		return 0;

	return 1;
}

static void gfx_v9_0_set_safe_mode(struct frb_device *adev, int xcc_id)
{
	uint32_t data;
	unsigned i;

	(void)xcc_id;
	data = RLC_SAFE_MODE__CMD_MASK;
	data |= (1 << RLC_SAFE_MODE__MESSAGE__SHIFT);
	WREG32_SOC15(GC, 0, mmRLC_SAFE_MODE, data);

	/* wait for RLC_SAFE_MODE */
	for (i = 0; i < adev->usec_timeout; i++) {
		if (!REG_GET_FIELD(RREG32_SOC15(GC, 0, mmRLC_SAFE_MODE), RLC_SAFE_MODE, CMD))
			break;
		udelay(1);
	}
	if (i >= adev->usec_timeout)
		frb_err(adev, "gfx: the RLC did not take safe mode (RLC_SAFE_MODE %08x)",
			RREG32_SOC15(GC, 0, mmRLC_SAFE_MODE));
}

static void gfx_v9_0_unset_safe_mode(struct frb_device *adev, int xcc_id)
{
	uint32_t data;

	(void)xcc_id;
	data = RLC_SAFE_MODE__CMD_MASK;
	WREG32_SOC15(GC, 0, mmRLC_SAFE_MODE, data);
}

/* amdgpu_rlc.c amdgpu_gfx_rlc_enter_safe_mode */
static void amdgpu_gfx_rlc_enter_safe_mode(struct frb_device *adev, int xcc_id)
{
	struct frb_gfx9_rlc *rlc = &adev->gfx9->rlc;

	if (rlc->in_safe_mode)
		return;

	/* if RLC is not enabled, do nothing */
	if (!gfx_v9_0_is_rlc_enabled(adev))
		return;

	if (adev->cg_flags & (AMD_CG_SUPPORT_GFX_CGCG | AMD_CG_SUPPORT_GFX_MGCG | AMD_CG_SUPPORT_GFX_3D_CGCG)) {
		gfx_v9_0_set_safe_mode(adev, xcc_id);
		rlc->in_safe_mode = 1;
	}
}

/* amdgpu_rlc.c amdgpu_gfx_rlc_exit_safe_mode */
static void amdgpu_gfx_rlc_exit_safe_mode(struct frb_device *adev, int xcc_id)
{
	struct frb_gfx9_rlc *rlc = &adev->gfx9->rlc;

	if (!rlc->in_safe_mode)
		return;

	/* if RLC is not enabled, do nothing */
	if (!gfx_v9_0_is_rlc_enabled(adev))
		return;

	if (adev->cg_flags & (AMD_CG_SUPPORT_GFX_CGCG | AMD_CG_SUPPORT_GFX_MGCG | AMD_CG_SUPPORT_GFX_3D_CGCG)) {
		gfx_v9_0_unset_safe_mode(adev, xcc_id);
		rlc->in_safe_mode = 0;
	}
}

static void gfx_v9_0_update_medium_grain_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t data, def;

	/* It is disabled by HW by default */
	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGCG)) {
		/* 1 - RLC_CGTT_MGCG_OVERRIDE */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);

		/* not 9.2.1 */
		data &= ~RLC_CGTT_MGCG_OVERRIDE__CPF_CGTT_SCLK_OVERRIDE_MASK;

		data &= ~(RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK |
			  RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGLS_OVERRIDE_MASK);

		/* only for Vega10 & Raven1 */
		data |= RLC_CGTT_MGCG_OVERRIDE__RLC_CGTT_SCLK_OVERRIDE_MASK;

		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* MGLS is a global flag to control all MGLS in GFX */
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_MGLS) {
			/* 2 - RLC memory Light sleep */
			if (adev->cg_flags & AMD_CG_SUPPORT_GFX_RLC_LS) {
				def = data = RREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL);
				data |= RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK;
				if (def != data)
					WREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL, data);
			}
			/* 3 - CP memory Light sleep */
			if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CP_LS) {
				def = data = RREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL);
				data |= CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
				if (def != data)
					WREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL, data);
			}
		}
	} else {
		/* 1 - MGCG_OVERRIDE */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);

		/* not 9.2.1 */
		data |= RLC_CGTT_MGCG_OVERRIDE__CPF_CGTT_SCLK_OVERRIDE_MASK;

		data |= (RLC_CGTT_MGCG_OVERRIDE__RLC_CGTT_SCLK_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GRBM_CGTT_SCLK_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGCG_OVERRIDE_MASK |
			 RLC_CGTT_MGCG_OVERRIDE__GFXIP_MGLS_OVERRIDE_MASK);

		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* 2 - disable MGLS in RLC */
		data = RREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL);
		if (data & RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK) {
			data &= ~RLC_MEM_SLP_CNTL__RLC_MEM_LS_EN_MASK;
			WREG32_SOC15(GC, 0, mmRLC_MEM_SLP_CNTL, data);
		}

		/* 3 - disable MGLS in CP */
		data = RREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL);
		if (data & CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK) {
			data &= ~CP_MEM_SLP_CNTL__CP_MEM_LS_EN_MASK;
			WREG32_SOC15(GC, 0, mmCP_MEM_SLP_CNTL, data);
		}
	}
}

static void gfx_v9_0_update_3d_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t data, def;

	/* num_gfx_rings: 1 */

	/* Enable 3D CGCG/CGLS */
	if (enable) {
		/* write cmd to clear cgcg/cgls ov */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		/* unset CGCG override */
		data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_GFX3D_CG_OVERRIDE_MASK;
		/* update CGCG and CGLS override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* enable 3Dcgcg FSM(0x0000363f) */
		def = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D);

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGCG)
			data = (0x36 << RLC_CGCG_CGLS_CTRL_3D__CGCG_GFX_IDLE_THRESHOLD__SHIFT) |
				RLC_CGCG_CGLS_CTRL_3D__CGCG_EN_MASK;
		else
			data = 0x0 << RLC_CGCG_CGLS_CTRL_3D__CGCG_GFX_IDLE_THRESHOLD__SHIFT;

		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_3D_CGLS)
			data |= (0x000F << RLC_CGCG_CGLS_CTRL_3D__CGLS_REP_COMPANSAT_DELAY__SHIFT) |
				RLC_CGCG_CGLS_CTRL_3D__CGLS_EN_MASK;
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D, data);

		/* set IDLE_POLL_COUNT(0x00900100) */
		def = RREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL);
		data = (0x0100 << CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY__SHIFT) |
			(0x0090 << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
		if (def != data)
			WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL, data);
	} else {
		/* Disable CGCG/CGLS */
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D);
		/* disable cgcg, cgls should be disabled */
		data &= ~(RLC_CGCG_CGLS_CTRL_3D__CGCG_EN_MASK |
			  RLC_CGCG_CGLS_CTRL_3D__CGLS_EN_MASK);
		/* disable cgcg and cgls in FSM */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL_3D, data);
	}
}

static void gfx_v9_0_update_coarse_grain_clock_gating(struct frb_device *adev, int enable)
{
	uint32_t def, data;

	if (enable && (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGCG)) {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE);
		/* unset CGCG override */
		data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGCG_OVERRIDE_MASK;
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGLS)
			data &= ~RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK;
		else
			data |= RLC_CGTT_MGCG_OVERRIDE__GFXIP_CGLS_OVERRIDE_MASK;
		/* update CGCG and CGLS override bits */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGTT_MGCG_OVERRIDE, data);

		/* enable cgcg FSM(0x0000363F) (not 9.4.1) */
		def = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL);

		data = (0x36 << RLC_CGCG_CGLS_CTRL__CGCG_GFX_IDLE_THRESHOLD__SHIFT) |
			RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK;
		if (adev->cg_flags & AMD_CG_SUPPORT_GFX_CGLS)
			data |= (0x000F << RLC_CGCG_CGLS_CTRL__CGLS_REP_COMPANSAT_DELAY__SHIFT) |
				RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK;
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, data);

		/* set IDLE_POLL_COUNT(0x00900100) */
		def = RREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL);
		data = (0x0100 << CP_RB_WPTR_POLL_CNTL__POLL_FREQUENCY__SHIFT) |
			(0x0090 << CP_RB_WPTR_POLL_CNTL__IDLE_POLL_COUNT__SHIFT);
		if (def != data)
			WREG32_SOC15(GC, 0, mmCP_RB_WPTR_POLL_CNTL, data);
	} else {
		def = data = RREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL);
		/* reset CGCG/CGLS bits */
		data &= ~(RLC_CGCG_CGLS_CTRL__CGCG_EN_MASK | RLC_CGCG_CGLS_CTRL__CGLS_EN_MASK);
		/* disable cgcg and cgls in FSM */
		if (def != data)
			WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, data);
	}
}

static int gfx_v9_0_update_gfx_clock_gating(struct frb_device *adev, int enable)
{
	amdgpu_gfx_rlc_enter_safe_mode(adev, 0);
	if (enable) {
		/* CGCG/CGLS should be enabled after MGCG/MGLS
		 * ===  MGCG + MGLS ===
		 */
		gfx_v9_0_update_medium_grain_clock_gating(adev, enable);
		/* ===  CGCG /CGLS for GFX 3D Only === */
		gfx_v9_0_update_3d_clock_gating(adev, enable);
		/* ===  CGCG + CGLS === */
		gfx_v9_0_update_coarse_grain_clock_gating(adev, enable);
	} else {
		/* CGCG/CGLS should be disabled before MGCG/MGLS
		 * ===  CGCG + CGLS ===
		 */
		gfx_v9_0_update_coarse_grain_clock_gating(adev, enable);
		/* ===  CGCG /CGLS for GFX 3D Only === */
		gfx_v9_0_update_3d_clock_gating(adev, enable);
		/* ===  MGCG + MGLS === */
		gfx_v9_0_update_medium_grain_clock_gating(adev, enable);
	}
	amdgpu_gfx_rlc_exit_safe_mode(adev, 0);
	return 0;
}

/*
 * gfx_v9_0_set_clockgating_state, IP_VERSION(9, 4, 0), bare metal. Its
 * set_powergating_state does nothing on 9.4.0 (only the APUs' 9.1/9.2.2/9.3
 * and 9.2.1's GFXOFF arms write), and Vega 20 has no AMD_PG_SUPPORT_* flags.
 */
void frb_gfx_v9_0_set_clockgating_state(struct frb_device *adev, int gate)
{
	if (!frb_gfx_v9_0_up(adev))
		return;
	gfx_v9_0_update_gfx_clock_gating(adev, gate);
}

int frb_gfx_v9_0_up(struct frb_device *adev)
{
	return adev->gfx9 && adev->gfx9->hw_started;
}

/* --- recovery (frb_reset.c): soft recovery and the soft reset ------------------ */

/* gfx_v9_0_ring_soft_recovery: kill the VMID's waves (SQ_CMD), under RLC safe mode */
void frb_gfx_v9_0_ring_soft_recovery(struct frb_ring *ring, unsigned vmid)
{
	struct frb_device *adev = ring->adev;
	uint32_t value = 0;

	value = REG_SET_FIELD(value, SQ_CMD, CMD, 0x03);
	value = REG_SET_FIELD(value, SQ_CMD, MODE, 0x01);
	value = REG_SET_FIELD(value, SQ_CMD, CHECK_VMID, 1);
	value = REG_SET_FIELD(value, SQ_CMD, VM_ID, vmid);
	amdgpu_gfx_rlc_enter_safe_mode(adev, 0);
	WREG32_SOC15(GC, 0, mmSQ_CMD, value);
	amdgpu_gfx_rlc_exit_safe_mode(adev, 0);
}

/*
 * amdgpu's gfx_v9_0 has one soft_reset hook and no check/pre/post: it reads
 * the status, stops the RLC and both CPs, and pulses GRBM_SOFT_RESET, and
 * amdgpu_device_gpu_recover never reaches it on Vega 20 (no check_soft_reset,
 * so no block is ever marked hung; a timeout that the soft recovery and the
 * KIQ's queue reset do not cure ends in a reset of the whole ASIC and every
 * block's resume). Firebrand has no KIQ and does not reset the ASIC at run
 * time, so that hook is split over frb_ip_reset_funcs the way gfx_v8_0's are:
 * its status tests are check_soft_reset, its stops pre_soft_reset, its pulse
 * soft_reset, and post_soft_reset is hw_init's rlc_resume and cp_resume (the
 * microcode stays: the PSP loaded it, a soft reset does not clear it).
 */
static int gfx_v9_0_check_soft_reset(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	uint32_t grbm_soft_reset = 0;
	uint32_t tmp;

	if (!g || !g->hw_started)
		return 0;

	/* GRBM_STATUS */
	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
	if (tmp & (GRBM_STATUS__PA_BUSY_MASK | GRBM_STATUS__SC_BUSY_MASK |
		   GRBM_STATUS__BCI_BUSY_MASK | GRBM_STATUS__SX_BUSY_MASK |
		   GRBM_STATUS__TA_BUSY_MASK | GRBM_STATUS__VGT_BUSY_MASK |
		   GRBM_STATUS__DB_BUSY_MASK | GRBM_STATUS__CB_BUSY_MASK |
		   GRBM_STATUS__GDS_BUSY_MASK | GRBM_STATUS__SPI_BUSY_MASK |
		   GRBM_STATUS__IA_BUSY_MASK | GRBM_STATUS__IA_BUSY_NO_DMA_MASK)) {
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_CP, 1);
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_GFX, 1);
	}

	if (tmp & (GRBM_STATUS__CP_BUSY_MASK | GRBM_STATUS__CP_COHERENCY_BUSY_MASK)) {
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_CP, 1);
	}

	/* GRBM_STATUS2 */
	tmp = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	if (REG_GET_FIELD(tmp, GRBM_STATUS2, RLC_BUSY))
		grbm_soft_reset = REG_SET_FIELD(grbm_soft_reset,
						GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);

	g->grbm_soft_reset = grbm_soft_reset;
	return grbm_soft_reset != 0;
}

/* not amdgpu: mark the block hung whatever its status says (frbctl recover's forced levels) */
static void gfx_v9_0_force_soft_reset(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;

	if (!g || !g->hw_started)
		return;
	g->grbm_soft_reset = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK | GRBM_SOFT_RESET__SOFT_RESET_GFX_MASK;
}

static int gfx_v9_0_pre_soft_reset(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;

	if (!g || !g->grbm_soft_reset)
		return 0;

	/*
	 * amdgpu ungates before any reset (amdgpu_device_set_cg_state UNGATE
	 * in its suspend path); rlc_resume would clear RLC_CGCG_CGLS_CTRL anyway
	 */
	g->regate = adev->cg_gated;
	if (g->regate)
		gfx_v9_0_update_gfx_clock_gating(adev, 0);

	g->gfx_ring.ready = 0;

	/* stop the rlc */
	gfx_v9_0_rlc_stop(adev);

	/* Disable GFX parsing/prefetching */
	gfx_v9_0_cp_gfx_enable(adev, 0);

	/* Disable MEC parsing/prefetching */
	gfx_v9_0_cp_compute_enable(adev, 0);

	return 0;
}

static int gfx_v9_0_soft_reset(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	uint32_t grbm_soft_reset;
	uint32_t tmp;

	if (!g || !g->grbm_soft_reset)
		return 0;
	grbm_soft_reset = g->grbm_soft_reset;

	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	tmp |= grbm_soft_reset;
	frb_info(adev, "GRBM_SOFT_RESET=0x%08X", tmp);
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);

	udelay(50);

	tmp &= ~grbm_soft_reset;
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);

	/* Wait a little for things to settle down */
	udelay(50);

	return 0;
}

/* hw_init's rlc_resume and cp_resume (its ring test's result is returned), the gating as it was */
static int gfx_v9_0_post_soft_reset(struct frb_device *adev)
{
	struct frb_gfx9 *g = adev->gfx9;
	int r;

	if (!g || !g->grbm_soft_reset)
		return 0;

	/* the CP's pointers restart at 0 (cp_gfx_resume); so does the copy the driver reads */
	frb_wb_write64(adev, g->gfx_ring.rptr_offs, 0);

	r = gfx_v9_0_rlc_resume(adev);
	if (!r)
		r = gfx_v9_0_cp_resume(adev);
	if (!r && g->regate)
		gfx_v9_0_update_gfx_clock_gating(adev, 1);
	g->regate = 0;
	return r;
}

const struct frb_ip_reset_funcs frb_gfx_v9_0_reset_funcs = {
	.name = "gfx_v9_0",
	.check_soft_reset = gfx_v9_0_check_soft_reset,
	.force_soft_reset = gfx_v9_0_force_soft_reset,
	.pre_soft_reset = gfx_v9_0_pre_soft_reset,
	.soft_reset = gfx_v9_0_soft_reset,
	.post_soft_reset = gfx_v9_0_post_soft_reset,
};
