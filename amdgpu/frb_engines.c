/*
 * The engines by number, for the control client (frbctl test), and their
 * teardown in amdgpu_device_ip_fini_early's reverse order (UVD, SDMA, GFX).
 * A SOC15 card's GFX and SDMA are gfx_v9_0 / sdma_v4_0 (frb_gfx_v9_0.c,
 * frb_sdma_v4_0.c) behind the same engine and test numbers.
 *
 * And the family dispatch the accelerator (kext/Firebrand/FirebrandAccel*.cpp,
 * FirebrandVideo.cpp) goes through for every engine call: the GFX ring, the
 * SDMA rings, their IB padding, copy and fill, the GFX configuration and the
 * register reads user space may make (frb_device.h).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_ring.h"
#include "frb_psp.h"
#include "frb_smu11.h"
#include "frb_pm.h"
#include "frb_soc15.h"
#include "include/asic_reg/gc/gc_9_0_offset.h"
#include "include/asic_reg/sdma0/sdma0_4_2_offset.h"
#include "include/asic_reg/sdma1/sdma1_4_2_offset.h"

/* --- the family dispatch ------------------------------------------------------ */

struct frb_ring *frb_gfx_ring(struct frb_device *adev)
{
	if (frb_is_soc15(adev))
		return frb_gfx_v9_0_ring(adev);
	return frb_is_si(adev) ? frb_gfx_v6_0_ring(adev) : frb_gfx_v8_0_ring(adev);
}

struct frb_ring *frb_sdma_ring(struct frb_device *adev, int instance)
{
	if (frb_is_soc15(adev))
		return frb_sdma_v4_0_ring(adev, instance);
	return frb_is_si(adev) ? frb_si_dma_ring(adev, instance) : frb_sdma_v3_0_ring(adev, instance);
}

void frb_sdma_pad_ib(struct frb_ring *ring, struct frb_ib *ib)
{
	if (frb_is_soc15(ring->adev))
		frb_sdma_v4_0_pad_ib(ring, ib);
	else if (frb_is_si(ring->adev))
		frb_si_dma_pad_ib(ring, ib);
	else
		frb_sdma_v3_0_pad_ib(ring, ib);
}

int frb_sdma_copy_buffer(struct frb_device *adev, int instance, uint64_t src_offset, uint64_t dst_offset,
			 uint32_t byte_count)
{
	if (frb_is_soc15(adev))
		return frb_sdma_v4_0_copy_buffer(adev, instance, src_offset, dst_offset, byte_count);
	if (frb_is_si(adev))
		return frb_si_dma_copy_buffer(adev, instance, src_offset, dst_offset, byte_count);
	return frb_sdma_v3_0_copy_buffer(adev, instance, src_offset, dst_offset, byte_count);
}

int frb_sdma_fill_buffer(struct frb_device *adev, int instance, uint32_t src_data, uint64_t dst_addr,
			 uint32_t byte_count)
{
	if (frb_is_soc15(adev))
		return frb_sdma_v4_0_fill_buffer(adev, instance, src_data, dst_addr, byte_count);
	if (frb_is_si(adev))
		return frb_si_dma_fill_buffer(adev, instance, src_data, dst_addr, byte_count);
	return frb_sdma_v3_0_fill_buffer(adev, instance, src_data, dst_addr, byte_count);
}

int frb_gfx_get_info(struct frb_device *adev, struct frb_gfx_info *info)
{
	if (frb_is_soc15(adev))
		return frb_gfx_v9_0_get_info(adev, info);
	return frb_is_si(adev) ? frb_gfx_v6_0_get_info(adev, info) : frb_gfx_v8_0_get_info(adev, info);
}

/*
 * soc15.c soc15_allowed_read_registers / soc15_read_register (7.1.1): no
 * GRBM-indexed entry on the list, so GRBM_GFX_INDEX is never touched;
 * GB_ADDR_CONFIG and DB_DEBUG2 come from gfx_v9_0's copies
 * (soc15_get_register_value).
 */
static int soc15_read_register(struct frb_device *adev, uint32_t reg_offset, uint32_t *value)
{
	const uint32_t allowed[] = {
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS),
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS2),
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE0),
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE1),
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE2),
		SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS_SE3),
		SOC15_REG_OFFSET(SDMA0, 0, mmSDMA0_STATUS_REG),
		SOC15_REG_OFFSET(SDMA1, 0, mmSDMA1_STATUS_REG),
		SOC15_REG_OFFSET(GC, 0, mmCP_STAT),
		SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT1),
		SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT2),
		SOC15_REG_OFFSET(GC, 0, mmCP_STALLED_STAT3),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPF_BUSY_STAT),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPF_STALLED_STAT1),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPF_STATUS),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPC_BUSY_STAT),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPC_STALLED_STAT1),
		SOC15_REG_OFFSET(GC, 0, mmCP_CPC_STATUS),
		SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG),
		SOC15_REG_OFFSET(GC, 0, mmDB_DEBUG2),
	};
	struct frb_gfx_info gfx;
	uint32_t i;

	*value = 0;
	for (i = 0; i < ARRAY_SIZE(allowed); i++) {
		if (reg_offset != allowed[i])
			continue;
		if ((reg_offset == SOC15_REG_OFFSET(GC, 0, mmGB_ADDR_CONFIG) ||
		     reg_offset == SOC15_REG_OFFSET(GC, 0, mmDB_DEBUG2)) && !frb_gfx_v9_0_get_info(adev, &gfx)) {
			*value = reg_offset == SOC15_REG_OFFSET(GC, 0, mmDB_DEBUG2) ? gfx.db_debug2 : gfx.gb_addr_config;
			return 0;
		}
		*value = RREG32(reg_offset);
		return 0;
	}
	return -EINVAL;
}

/* frb_soc15_asic_funcs.read_register: no GRBM-indexed register, so se/sh are not used */
int frb_soc15_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
			    uint32_t *value)
{
	(void)se_num;
	(void)sh_num;
	return soc15_read_register(adev, reg_offset, value);
}

/* amdgpu_asic_read_register: the family's (vi_read_register, si_read_register, soc15's) */
int frb_read_register(struct frb_device *adev, uint32_t se_num, uint32_t sh_num, uint32_t reg_offset,
		      uint32_t *value)
{
	if (!frb_funcs(adev)->read_register)
		return -EINVAL;
	return frb_funcs(adev)->read_register(adev, se_num, sh_num, reg_offset, value);
}

uint32_t frb_get_xclk(struct frb_device *adev)
{
	if (frb_is_soc15(adev))
		return frb_soc15_get_xclk(adev);
	return frb_is_si(adev) ? frb_si_get_xclk(adev) : frb_vi_get_xclk(adev);
}

/* gfx_8_0_d.h's offsets (the header cannot sit beside gc_9_0_offset.h: the same names) */
#define VI_mmGRBM_STATUS	0x2004
#define VI_mmCP_STALLED_STAT1	0x219d
#define VI_mmCP_STAT		0x21a0

void frb_gfx_hang_status(struct frb_device *adev, uint32_t *grbm_status, uint32_t *cp_stat,
			 uint32_t *cp_stalled_stat1)
{
	if (frb_is_soc15(adev)) {
		*grbm_status = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
		*cp_stat = RREG32_SOC15(GC, 0, mmCP_STAT);
		*cp_stalled_stat1 = RREG32_SOC15(GC, 0, mmCP_STALLED_STAT1);
		return;
	}
	*grbm_status = RREG32(VI_mmGRBM_STATUS);
	*cp_stat = RREG32(VI_mmCP_STAT);
	*cp_stalled_stat1 = RREG32(VI_mmCP_STALLED_STAT1);
}

/* frb_reset.c's snapshot on SOC15 (its own headers are VI's) */
void frb_soc15_recover_snapshot(struct frb_device *adev, struct frb_recover_report *rep)
{
	rep->grbm_status = RREG32_SOC15(GC, 0, mmGRBM_STATUS);
	rep->grbm_status2 = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	rep->srbm_status = 0;
	rep->srbm_status2 = 0;
	rep->cp_stat = RREG32_SOC15(GC, 0, mmCP_STAT);
	rep->sdma_status[0] = RREG32_SOC15(SDMA0, 0, mmSDMA0_STATUS_REG);
	rep->sdma_status[1] = RREG32_SOC15(SDMA1, 0, mmSDMA1_STATUS_REG);
}

/* --- the tests ----------------------------------------------------------------- */

#define GTT_TEST_BYTES	(256 * 1024)

static uint32_t gtt_pattern(uint32_t seed, uint32_t i)
{
	uint32_t x = seed ^ (i * 0x9e3779b9u);

	x ^= x >> 15;
	x *= 0x2c1b3c6du;
	return x ^ (x >> 12);
}

static int gtt_check(struct frb_device *adev, const char *what, int in_gtt, uint32_t vram, uint32_t seed,
		     int fill, uint32_t fill_word)
{
	const uint8_t *cpu = (const uint8_t *)adev->gtt_test.cpu;
	uint32_t i, n = GTT_TEST_BYTES / 4, bad = 0, first = 0, got = 0, want = 0;

	if (!in_gtt)
		frb_invalidate_hdp(adev);
	for (i = 0; i < n; i++) {
		uint32_t w = fill ? fill_word : gtt_pattern(seed, i);
		uint32_t v = in_gtt ? frb_get_le32(cpu, 4 * i) : frb_vram_read32(adev, vram + 4 * i);

		if (v != w && !bad++) {
			first = i;
			got = v;
			want = w;
		}
	}
	if (bad) {
		frb_err(adev, "gtt test: %s: %u of %u dwords wrong; first at +0x%x: 0x%08x, want 0x%08x",
			what, bad, n, first * 4, got, want);
		return -EIO;
	}
	frb_info(adev, "gtt test: %s: %u bytes right", what, GTT_TEST_BYTES);
	return 0;
}

/*
 * GTT through an SDMA engine: system pages bound into the GART (os gtt_alloc:
 * wired pages, inbound window 2, bus mastering), then a GTT->VRAM copy (the
 * GPU reads host memory), and only if that is right a VRAM->GTT copy and a
 * fill into GTT (the GPU writes host memory). CPU readback each time.
 */
static int gtt_test(struct frb_device *adev, int instance)
{
	uint8_t *cpu;
	uint64_t gtt_gpu, vram_gpu;
	uint32_t i, seed;
	int r;

	if (!adev->os->gtt_alloc)
		return -ENODEV;
	if (!adev->gtt_test.cpu) {
		r = adev->os->gtt_alloc(adev->os->cookie, GTT_TEST_BYTES, &adev->gtt_test);
		if (r)
			return r;
		r = frb_vram_obj_alloc(adev, GTT_TEST_BYTES, 4096, &adev->gtt_test_vram);
		if (r)
			return r;
		frb_info(adev, "gtt test: %u KB of system memory at GART %llx (page %u), VRAM buffer at %llx",
			 GTT_TEST_BYTES >> 10, (unsigned long long)adev->gtt_test.gpu_addr, adev->gtt_test.first_page,
			 (unsigned long long)frb_vram_gpu_addr(adev, adev->gtt_test_vram));
	}
	cpu = (uint8_t *)adev->gtt_test.cpu;
	gtt_gpu = adev->gtt_test.gpu_addr;
	vram_gpu = frb_vram_gpu_addr(adev, adev->gtt_test_vram);
	seed = 0x47545400u ^ (uint32_t)adev->os->time_ms(adev->os->cookie);

	/* 1: GTT -> VRAM */
	for (i = 0; i < GTT_TEST_BYTES / 4; i++) {
		frb_put_le32(cpu, 4 * i, gtt_pattern(seed, i));
		frb_vram_write32(adev, adev->gtt_test_vram + 4 * i, 0xdeadbeef);
	}
	r = frb_sdma_copy_buffer(adev, instance, gtt_gpu, vram_gpu, GTT_TEST_BYTES);
	if (!r)
		r = gtt_check(adev, "GTT -> VRAM copy", 0, adev->gtt_test_vram, seed, 0, 0);
	if (r)
		return r;

	/* 2: VRAM -> GTT */
	seed = ~seed;
	for (i = 0; i < GTT_TEST_BYTES / 4; i++) {
		frb_vram_write32(adev, adev->gtt_test_vram + 4 * i, gtt_pattern(seed, i));
		frb_put_le32(cpu, 4 * i, 0xdeadbeef);
	}
	r = frb_sdma_copy_buffer(adev, instance, vram_gpu, gtt_gpu, GTT_TEST_BYTES);
	if (!r)
		r = gtt_check(adev, "VRAM -> GTT copy", 1, 0, seed, 0, 0);
	if (r)
		return r;

	/* 3: a fill into GTT (CONST_FILL repeats the low byte on SDMA 3 and 4, the whole dword on SI's DMA) */
	r = frb_sdma_fill_buffer(adev, instance, frb_is_si(adev) ? 0x5a5a5a5au : 0x5a, gtt_gpu, GTT_TEST_BYTES);
	if (!r)
		r = gtt_check(adev, "fill into GTT", 1, 0, 0, 1, 0x5a5a5a5au);
	return r;
}

/*
 * FRB_TEST_HANG: the engine waits (WAIT_REG_MEM / POLL_REGMEM, >=) for a
 * writeback dword to reach 1 while it holds 0, with a fence behind the
 * wait, and is left that way: a hung ring for frbctl recover and the
 * accelerator's watchdog. Writing 1 there would let it go on.
 */
static int hang_test(struct frb_device *adev, int engine)
{
	struct frb_ring *ring = engine == FRB_ENGINE_GFX ? frb_gfx_ring(adev) :
				frb_sdma_ring(adev, engine - FRB_ENGINE_SDMA0);
	uint32_t seq;
	int r;

	if (!ring)
		return -ENODEV;
	if (!adev->hang_wb) {
		uint32_t wb;

		r = frb_wb_get(adev, &wb);
		if (r)
			return r;
		adev->hang_wb = wb + 1;
	}
	frb_wb_write(adev, adev->hang_wb - 1, 0);
	frb_flush_hdp(adev);
	r = frb_ring_fence_after(ring, frb_wb_gpu_addr(adev, adev->hang_wb - 1), 1, &seq);
	if (r)
		return r;
	frb_info(adev, "hang test: %s waits for writeback dword %u (at %llx) to reach 1; fence %u behind it",
		 ring->name, adev->hang_wb - 1, (unsigned long long)frb_wb_gpu_addr(adev, adev->hang_wb - 1), seq);
	return 0;
}

int frb_engine_test(struct frb_device *adev, int engine, int test)
{
	if (test < 0 || test >= FRB_TEST_COUNT)
		return -EINVAL;
	if (test == FRB_TEST_HANG) {
		if (engine < FRB_ENGINE_GFX || engine > FRB_ENGINE_SDMA1)
			return -EINVAL;
		return hang_test(adev, engine);
	}
	if (test == FRB_TEST_GTT) {
		if (engine != FRB_ENGINE_SDMA0 && engine != FRB_ENGINE_SDMA1)
			return -EINVAL;
		/* SOC15: the pages through gmc_v9's GART (VMID 0, MTYPE UC: frb_gart_gtt_flags) */
		return gtt_test(adev, engine - FRB_ENGINE_SDMA0);
	}
	if (frb_is_soc15(adev)) {
		/* Vega 20: gfx v9, sdma v4, uvd v7 */
		switch (engine) {
		case FRB_ENGINE_GFX:	return frb_gfx_v9_0_test(adev, test);
		case FRB_ENGINE_SDMA0:	return frb_sdma_v4_0_test(adev, 0, test);
		case FRB_ENGINE_SDMA1:	return frb_sdma_v4_0_test(adev, 1, test);
		case FRB_ENGINE_UVD:	return frb_uvd_v7_0_test(adev, test);
		default:		return -ENOSYS;
		}
	}
	if (frb_is_si(adev)) {
		switch (engine) {
		case FRB_ENGINE_GFX:	return frb_gfx_v6_0_test(adev, test);
		case FRB_ENGINE_SDMA0:	return frb_si_dma_test(adev, 0, test);
		case FRB_ENGINE_SDMA1:	return frb_si_dma_test(adev, 1, test);
		case FRB_ENGINE_UVD:	return frb_uvd_v3_1_test(adev, test);
		default:		return -EINVAL;
		}
	}
	switch (engine) {
	case FRB_ENGINE_GFX:	return frb_gfx_v8_0_test(adev, test);
	case FRB_ENGINE_SDMA0:	return frb_sdma_v3_0_test(adev, 0, test);
	case FRB_ENGINE_SDMA1:	return frb_sdma_v3_0_test(adev, 1, test);
	case FRB_ENGINE_UVD:	return frb_uvd_v6_0_test(adev, test);
	default:		return -EINVAL;
	}
}

/* nothing fetches any more: the GTT test buffer can go (its entries back to the dummy page) */
static void gtt_test_free(struct frb_device *adev)
{
	if (adev->gtt_test.cpu && adev->os->gtt_free)
		adev->os->gtt_free(adev->os->cookie, &adev->gtt_test);
}

void frb_engines_fini(struct frb_device *adev)
{
	if (frb_is_soc15(adev)) {
		/*
		 * Vega: amdgpu_device_ip_fini_early disables the SMC first
		 * (amdgpu_device_smu_fini_early), then runs hw_fini in reverse
		 * IP-block order: UVD (block 8; VCE is not ported), SDMA, GFX,
		 * (DM: the kext's frb_display_fini), PSP. The GTT test buffer goes once SDMA is halted, while the
		 * GMC is as gmc_v9_0 left it (its TLB flush polls the hubs).
		 */
		/* amdgpu_device_fini_hw: set_pg_state, set_cg_state UNGATE first (reverse block order) */
		if (adev->cg_gated)
			frb_soc15_set_cg_state(adev, 0);
		frb_smu11_hw_fini(adev);
		frb_uvd_v7_0_fini(adev);
		frb_sdma_v4_0_fini(adev);
		frb_gfx_v9_0_fini(adev);
		gtt_test_free(adev);
		frb_psp_hw_fini(adev);
		return;
	}
	if (frb_is_si(adev)) {
		/* si_set_ip_blocks' reverse order: uvd, (dce), DPM, the gating undone, dma, gfx */
		frb_uvd_v3_1_fini(adev);
		frb_pm_fini(adev);
		if (adev->cg_on) {
			frb_si_dma_set_clockgating_state(adev, 0);
			frb_gfx_v6_0_set_clockgating_state(adev, 0);
			frb_gmc_v6_0_set_clockgating_state(adev, 0);
			adev->cg_on = 0;
		}
		frb_si_dma_fini(adev);
		frb_gfx_v6_0_fini(adev);
		if (adev->gtt_test.cpu && adev->os->gtt_free)
			adev->os->gtt_free(adev->os->cookie, &adev->gtt_test);
		return;
	}
	/*
	 * amdgpu's hw_fini order is the reverse of init: UVD (block 7) first,
	 * then the SMC block (pp_smu, after SDMA here), back to the boot state
	 * with DPM off
	 */
	frb_uvd_v6_0_fini(adev);
	frb_pm_fini(adev);
	frb_sdma_v3_0_fini(adev);
	frb_gfx_v8_0_fini(adev);
	gtt_test_free(adev);
}
