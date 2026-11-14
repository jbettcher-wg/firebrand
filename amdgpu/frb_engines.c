/*
 * The engines by number, for the control client (frbctl test), and their
 * teardown in amdgpu_device_ip_fini_early's reverse order (UVD, SDMA, GFX).
 *
 * Copyright (c) 2026 Jordan Bettcher
 * SPDX-License-Identifier: MIT
 */
#include "frb_ring.h"
#include "frb_pm.h"

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
	r = frb_sdma_v3_0_copy_buffer(adev, instance, gtt_gpu, vram_gpu, GTT_TEST_BYTES);
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
	r = frb_sdma_v3_0_copy_buffer(adev, instance, vram_gpu, gtt_gpu, GTT_TEST_BYTES);
	if (!r)
		r = gtt_check(adev, "VRAM -> GTT copy", 1, 0, seed, 0, 0);
	if (r)
		return r;

	/* 3: a fill into GTT (CONST_FILL repeats the low byte on this engine) */
	r = frb_sdma_v3_0_fill_buffer(adev, instance, 0x5a, gtt_gpu, GTT_TEST_BYTES);
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
	struct frb_ring *ring = engine == FRB_ENGINE_GFX ? frb_gfx_v8_0_ring(adev) :
				frb_sdma_v3_0_ring(adev, engine - FRB_ENGINE_SDMA0);
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
		return gtt_test(adev, engine - FRB_ENGINE_SDMA0);
	}
	switch (engine) {
	case FRB_ENGINE_GFX:	return frb_gfx_v8_0_test(adev, test);
	case FRB_ENGINE_SDMA0:	return frb_sdma_v3_0_test(adev, 0, test);
	case FRB_ENGINE_SDMA1:	return frb_sdma_v3_0_test(adev, 1, test);
	case FRB_ENGINE_UVD:	return frb_uvd_v6_0_test(adev, test);
	default:		return -EINVAL;
	}
}

void frb_engines_fini(struct frb_device *adev)
{
	/*
	 * amdgpu's hw_fini order is the reverse of init: UVD (block 7) first,
	 * then the SMC block (pp_smu, after SDMA here), back to the boot state
	 * with DPM off
	 */
	frb_uvd_v6_0_fini(adev);
	frb_pm_fini(adev);
	frb_sdma_v3_0_fini(adev);
	frb_gfx_v8_0_fini(adev);
	/* nothing fetches any more: the GTT test buffer can go */
	if (adev->gtt_test.cpu && adev->os->gtt_free)
		adev->os->gtt_free(adev->os->cookie, &adev->gtt_test);
}
